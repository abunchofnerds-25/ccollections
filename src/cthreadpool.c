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
#include <internal/cdeadline.h>
#include <internal/cpintable.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Initialises cond so that every absolute deadline that goes to
 * ccol_cond_var_timedwait on it is measured against CLOCK_MONOTONIC, and not
 * against the default CLOCK_REALTIME. Returns 0 on success, or the first
 * non-zero status that the threading library reports.
 *
 * not_full is the one condition variable of a pool that a timed wait blocks
 * on, and make_abs_deadline computes its deadline on CLOCK_MONOTONIC too, so
 * the two can never disagree about the clock. An administrator, an NTP step
 * or the resume of a virtual machine can move CLOCK_REALTIME backwards at any
 * moment, and a deadline against that clock then sits further in the future:
 * a timed submit with a budget of 100 ms would block for the whole backward
 * step. Nothing can step CLOCK_MONOTONIC. cthreadcomm.c initialises its own
 * timed condition variables in the same way. */
static int _ctpool_init_monotonic_cond_var(ccol_cond_var_t *cond) {
  ccol_cond_var_attr_t attr;
  int rv = ccol_cond_var_attr_init(attr);
  if (rv != 0) return rv;
  rv = ccol_cond_var_attr_setclock(attr, CLOCK_MONOTONIC);
  if (rv == 0) rv = ccol_cond_var_init_ca(*cond, attr);
  ccol_cond_var_attr_destroy(attr);
  return rv;
}

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/* ctpool is an opaque value handle. The top 32 bits are the slot index, and
 * the bottom 32 bits are the generation. See the doc comment on the typedef in
 * include/cthreadpool.h. The library resolves the handle through this table
 * before it touches the struct cthread_pool* behind it. This is what lets
 * __ctpool_destroy find two kinds of double-destroy and report both with
 * ccol_fatal_err instead of a use-after-free or a double-free. The first kind
 * is a concurrent double-destroy, which races another destroy on the same
 * still-live handle. The second kind is a sequential one, with a stale handle
 * from an earlier, complete destroy. The library marks a slot not-in-use at
 * the instant it releases that slot. It also bumps the generation of the slot
 * on every reuse. A stale handle can therefore never alias a later, unrelated
 * pool in the same slot index. This table mirrors chttpcli_slot_table,
 * chttpsvr_slot_table, ccol_event_loop_slot_table and clrucache_slot_table
 * exactly. See the copy of this comment in src/chttpclient.c for the full
 * design rationale.
 *
 * The lock of the table is a read-write lock, not a plain mutex.
 * _ctpool_resolve only reads: it bounds-checks idx, compares the generation
 * and reads slot->ptr. It runs on every ctpool_submit, _try_submit,
 * _timed_submit, _wait and similar call. For a caller like chttpserver, which
 * submits one task for each request, that is the full request rate.
 * _ctpool_handle_slot_acquire and __ctpool_destroy are the only writers. Each
 * of them runs once for the whole life of a pool, not once for each task.
 * This mirrors the identical reg_slot_rwlock of ccol_event_loop, and it
 * carries the same fork-safety subtlety. See the doc comment of
 * _ctpool_atfork_prepare for the hazard of a write lock that tracks the thread
 * ID, and for the way the library handles it. */
typedef struct {
  cthread_pool *ptr;   /* NULL when slot is free */
  uint32_t generation; /* fresh on every acquire. It only rises, for each
                           slot index. It starts at 0 before the first use,
                           and becomes 1 on the first acquire */
  bool in_use;
#if CCOL_FORK_SAFETY_REQUIRED
  /* True from the exact instant that __ctpool_destroy clears in_use. It stays
   * true until _ctpool_teardown_raw is about to make ptr->mu unsafe to touch.
   * _ctpool_teardown_raw does that when it destroys ptr->mu. On the
   * foreign_since_fork path it does that when it frees the struct that holds
   * ptr->mu. See the doc comment of _ctpool_atfork_prepare for the hang that
   * this flag closes.
   *
   * in_use alone is not a good enough signal that ptr->mu is both live and
   * worth a lock. The worker threads of a pool are not gone at the instant
   * that in_use goes false. They are gone only after
   * _ctpool_shutdown_drain_internal joins every one of them. That step runs
   * fully AFTER the library clears in_use, so that the library rejects a
   * concurrent resolve or a second destroy as early as possible. Without
   * torn_down, a fork() inside that join window inherits ptr->mu locked by one
   * of those very real worker threads that nothing joined yet. in_use is
   * already false there, and it tells _ctpool_atfork_prepare that there is
   * nothing here worth protection.
   *
   * torn_down is never true while in_use is true. The library sets the two at
   * different points, never both together. ptr is not NULL and is safe to
   * dereference for as long as torn_down is true. The library clears
   * torn_down, under the write side of this same ctpool_slot_table.rwlock,
   * strictly before the struct that ptr points to becomes unsafe to touch. */
  bool torn_down;
#endif
} ctpool_slot_t;

static struct {
  ccol_rw_lock_t rwlock;
  ccol_once_flag_t once;
  cvec slots;        /* cvec of ctpool_slot_t. It grows only with push_back.
                         An index is permanent after the library allocates it */
  cvec free_indices; /* cvec of uint32_t. A LIFO free list, with O(1) reuse */
  /* How many slot indices the library could not push back onto free_indices,
     because that push could not allocate. Such a slot is fully released (ptr
     is NULL and in_use is false), but nothing names it. Without this counter
     the slot is unreachable for the rest of the life of the process, and every
     later create grows the table by one more slot. An acquire that finds
     free_indices empty, and this counter above zero, recovers one slot with a
     scan of slots instead. That scan is unambiguous, because it runs only with
     the free list empty: a released slot is either on that list or lost, never
     both. The library reads and writes this field only under the write
     lock. */
  size_t lost_indices;
  /* The library sets this when the process-exit destructor finds a pool still
     live and leaves this table alone. Whichever destroy releases the last slot
     after that then does the release that the destructor could not do. Without
     this flag, a pool that outlives the destructor of this translation unit
     leaves the table and the pin index allocated for the rest of the process.
     The lazily created HTTP client of this library owns such a pool, and its
     reaper runs from a destructor that the linker places earlier. A leak
     checker that treats still-reachable memory as an error reports that
     memory. The library reads and writes this field only under the write
     lock. */
  bool release_deferred;
} ctpool_slot_table = {0};

#if CCOL_FORK_SAFETY_REQUIRED
/* Forward declarations. The bodies come further below, after the declaration
 * of struct cthread_pool itself, because they dereference the mu of a live
 * pool. _ctpool_slot_table_init_globals below registers them. That function is
 * the first point in this file that runs once, lazily, at the first use of
 * this module. This mirrors the identical placement in src/cthreadcomm.c,
 * where ccol_event_loop also declares first and defines after the struct.
 *
 * The compiler removes this whole fork() safety mechanism when
 * CCOL_FORK_SAFETY_REQUIRED is 0. That covers these three handlers, their
 * ccol_at_fork() registration below, the foreign_since_fork field of struct
 * cthread_pool, and every site that reads it. See the doc comment of that
 * macro in common.h. */
static void _ctpool_atfork_prepare(void);
static void _ctpool_atfork_release(void);
static void _ctpool_atfork_child_release(void);
#endif

static void _ctpool_slot_table_init_globals(void) {
  if (ccol_rw_lock_init(ctpool_slot_table.rwlock) != 0)
    ccol_fatal_err("ctpool slot table: failed to initialize rwlock");
  ctpool_slot_table.slots = cvector_create(sizeof(ctpool_slot_t), NULL);
  if (!ctpool_slot_table.slots)
    ccol_fatal_err("ctpool slot table: failed to allocate slots vector");
  ctpool_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!ctpool_slot_table.free_indices)
    ccol_fatal_err("ctpool slot table: failed to allocate free-index vector");
#if CCOL_FORK_SAFETY_REQUIRED
  /* fork() duplicates only the thread that calls it. See the doc comment of
   * _ctpool_atfork_prepare for the full hazard that this closes for BOTH
   * parent() and child(), which is a still-locked mutex that the child
   * inherits. See the doc comment of _ctpool_atfork_child_release for a second
   * hazard that only the child has. Neither parent() nor a plain shared
   * release function can handle that one: a join of worker threads that exist
   * only in the parent. */
  ccol_at_fork(_ctpool_atfork_prepare, _ctpool_atfork_release,
               _ctpool_atfork_child_release);
#endif
}

/* Not static, and reachable on purpose from other .c files in this library.
 * chttpserver.c is one of them. Such a file must guarantee that this module
 * registers its own ccol_at_fork() triple BEFORE that file registers its own.
 * pthread_atfork calls the prepare handlers in LIFO order, so that ordering
 * makes the prepare handler of the CALLER run FIRST at every later fork(). The
 * caller therefore runs before _ctpool_atfork_prepare, the prepare handler of
 * this module, can lock ctpool_slot_table.rwlock or the mu of any live pool.
 *
 * This function is not declared in cthreadpool.h. It is not part of the public
 * API. It is a narrow, deliberate escape hatch for a caller that already read
 * this exact ordering requirement and must satisfy it. See the call site in
 * chttpserver.c for the full reasoning, and for the lock-order inversion that
 * this ordering prevents. ThreadSanitizer reports that cycle whenever nothing
 * forces the ordering. A caller that never uses this function is not affected
 * at all. The lazy registration of this module, which ccol_call_once guards,
 * then happens at the first create of a ctpool, exactly as it does with no
 * such caller. */
void _ctpool_ensure_atfork_registered_before_caller(void) {
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
}

/*
 * A single unit of work in the queue. A future task sets `future` and leaves
 * `on_complete` NULL. A callback task does the opposite. `fn_future` shares
 * storage with `fn` in a union, so both pointer widths are the same and the
 * call site needs no cast.
 */
typedef struct ctpool_task {
  union {
    void (*fn)(void *);         /* callback-style task */
    void *(*fn_future)(void *); /* future-style task   */
  };
  void *arg;
  void (*on_complete)(void *, bool); /* NULL for future tasks */
  ctpool_future *future;             /* NULL for callback tasks */
  struct ctpool_task *next;
} ctpool_task;

/*
 * The library always allocates a future on the heap with plain malloc, and
 * frees it with plain free. The lifetime of a future is therefore fully
 * independent of the custom allocator of the pool. A submit creates two
 * references: one for the caller and one for the queued task. Each
 * ctpool_future_get call in flight holds one more for the length of that call.
 * The library frees the struct after the last reference goes.
 */
struct ctpool_future {
  ccol_mutex_t mu;
  ccol_cond_var_t cv;
  void *result;
  bool done;
  bool cancelled;
  int refcount;
};

/* The hot half of the slot table. It maps a handle to a pointer. It also holds
 * the pin that keeps a pool alive for the length of a call. It is separate
 * from the table itself for two reasons. A resolve runs on every public call,
 * and it must not write anything that another thread reads. Slot recycling is
 * cold, and it stays under the rwlock. */
static ccol_pintable ctpool_pintable;

/* One worker of a pool. See the threads field of struct cthread_pool. */
typedef struct ctpool_worker_slot {
  ccol_thread_id_t id;
  atomic_bool exited;
} ctpool_worker_slot;

struct cthread_pool {
  /* Task queue (intrusive singly-linked list) */
  ctpool_task *head;
  ctpool_task *tail;
  size_t queue_size;
  size_t queue_cap; /* 0 = unbounded */

  /* Recycled ctpool_task nodes. They form a separate intrusive singly-linked
   * list, which reuses the `next` field of the same struct. This list removes
   * one malloc and free round trip through m_procs for each task submission.
   * That round trip is a real allocator-pressure hot path for a workload that
   * submits a large number of short-lived tasks. mu protects this list,
   * exactly like every other data structure that the pool owns.
   *
   * The list keeps every node that the recent traffic of the pool needed,
   * and not a fixed handful. A producer that runs ahead of the workers builds
   * a queue of many tasks, and every one of them is a node. A list that kept
   * only a few of them back sent every further node through the allocator
   * again: one malloc on the submit and one free on the completion, for each
   * task, for as long as the queue stayed deep, with a cross-thread free on
   * every one of them. Keeping the nodes makes a burst of any depth cost no
   * allocation at all once the pool has seen that depth.
   *
   * Two things bound what the list keeps. task_free_list_cap is a hard
   * ceiling, max(num_threads * 4, CTPOOL_TASK_FREE_LIST_CEILING), that the
   * library sets once at construction. And the list decays while the traffic
   * does not need it: at every CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS-th point
   * where the pool is idle, with no task queued and none running, the list
   * gives back half of what it holds above num_threads * 4, unless it ran
   * empty since the previous such point; see task_free_list_ran_dry and
   * _ctpool_idle_unlock_and_trim. The list therefore holds what the recent
   * traffic needed, no more than the ceiling, and never less than
   * num_threads * 4 while it has that many.
   *
   * None of that bookkeeping runs on the ordinary path. A pop that leaves the
   * list with nodes, a push, and an enqueue touch no field of it. Every
   * submitter and every worker takes mu for each task, so a store inside
   * those critical sections to a line that they do not otherwise touch would
   * move that line between cores on every task, and lengthen the critical
   * section that all of them wait on.
   *
   * A node in this list is only recycled. Nothing hands it back to m_procs.
   * The teardown of the pool must therefore genuinely free it with
   * drain_task_free_list. A discard alone is not enough. */
  ctpool_task *task_free_list;
  size_t task_free_list_size;
  size_t task_free_list_cap;

  /* Synchronisation */
  ccol_mutex_t mu;
  ccol_cond_var_t not_empty; /* a worker waits here when idle            */
  ccol_cond_var_t not_full;  /* a submitter waits here on a full queue   */
  ccol_cond_var_t idle_cv;   /* ctpool_wait waits here                   */

  /* Live counters */
  size_t active_count; /* tasks that the workers run now */

  /* Shutdown state */
  bool shutdown_drain;     /* the pool takes no new task. drain, then stop */
  bool shutdown_immediate; /* stop after the current tasks. discard queue  */
  bool shutdown_started;   /* one of the two shutdowns started             */
  /* Whichever call does the real shutdown sets this, under mu, after it joins
   * every worker thread. Every other shutdown call on this pool waits on
   * shutdown_cv until this flag is true. That is what makes the documented
   * "blocks until ..." guarantee of ctpool_shutdown_drain and
   * ctpool_shutdown_immediate hold for the second caller and every later one,
   * and not only for the first. The library sets shutdown_started before it
   * wakes a single worker, and long before it joins one. A return on
   * shutdown_started alone therefore hands application code a pool whose
   * workers all still run. This flag sits in padding that the three flags
   * above already had, so it costs the struct nothing. */
  bool shutdown_complete;

  /* Worker threads. exited is set by the worker itself, as its last write to
   * the pool, so a joined worker is always marked. _ctpool_is_self_call skips
   * a marked slot, because glibc hands the descriptor, and so the ID, of a
   * joined thread to the next thread that starts. */
  ctpool_worker_slot *threads;
  size_t num_threads;

  ccol_memmgmt_procs_t *m_procs;

  /* The handle of this pool. An unpin uses it to find the slot that holds its
   * pin, so the caller does not need to carry one. The library writes it once,
   * before it publishes the handle, and never again. */
  ctpool self_handle;

#if CCOL_FORK_SAFETY_REQUIRED
  /* Only the CHILD-side fork handler of this process
   * (_ctpool_atfork_child_release) sets this to true. It sets it for every
   * pool that is still marked in_use at the moment of the fork(). fork()
   * duplicates only the thread that calls it. From the point of view of this
   * process, every worker thread of this pool therefore exists only as inert,
   * copy-on-write memory. No execution context for any of them ever existed
   * here.
   *
   * After this flag is true, _ctpool_shutdown_drain_internal and
   * _ctpool_shutdown_immediate_internal must never call ccol_thread_join on
   * threads[i]. That is undefined behaviour, because this process never
   * created the target thread and can never join it. Take a process that holds
   * a live ctpool with several workers, for example a ccol_event_loop with
   * num_reactor_threads > 1. A fork() there, and then a destroy or a shutdown
   * of the inherited pool in the child, SIGSEGVs inside __pthread_clockjoin_ex
   * in glibc.
   *
   * This flag is never true for a pool that ccol_create_cthread_pool_mp really
   * created in this process. The compiler removes it when
   * CCOL_FORK_SAFETY_REQUIRED is 0. See the doc comment of that macro in
   * common.h. Every site that reads this field then takes, with no condition,
   * the same path that it takes when the field is false. A pool can never
   * legitimately be "foreign" in a build with no fork() machinery to mark one
   * as such. */
  _Atomic bool foreign_since_fork;
#endif

  /* The trim state of task_free_list; see the field comment of that list.
   * mu guards both. task_free_list_ran_dry is set when a pop takes the last
   * node or finds none, which is the slow path of a submit anyway, and the
   * idle point clears it. idle_points_since_trim counts idle points. Both sit
   * after every field that the submit path and the dequeue path read,
   * because only those rare events touch them. */
  bool task_free_list_ran_dry;
  uint32_t idle_points_since_trim;

  /* The library broadcasts this, under mu, at the exact moment that
   * shutdown_complete goes true. It is declared last on purpose, and not
   * beside the other condition variables. Nothing on the submit path or the
   * dequeue path reads it. A place among the others shifts active_count and
   * the queue bookkeeping that those paths do read onto other cache lines. */
  ccol_cond_var_t shutdown_cv;
};

/* ========================================================================== */
/*                         FORK SAFETY (pthread_atfork)                       */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/* fork() duplicates only the thread that calls it. A lock that some OTHER
 * thread holds at that instant passes to the child in a permanently locked
 * state. No thread survives in the child that could ever unlock it. Two kinds
 * of lock matter here. The first is ctpool_slot_table.rwlock, which is
 * process-wide, and which every ccol_create_cthread_pool_mp, __ctpool_destroy,
 * ctpool_submit and _ctpool_resolve call takes. The second is the mu of each
 * still-live pool, which every submit, dequeue, shutdown and wait call takes.
 * A worker thread that picks up or finishes a task takes it too.
 *
 * prepare() below therefore takes both of them before fork() can proceed.
 * fork() then completes only when no thread holds one of them for a moment.
 * Both parent() and child() release them again through the same function.
 * Every PLAIN mutex in this module, the mu of every pool included, uses the
 * default ("normal") pthread mutex type. That type does no owner or thread ID
 * tracking on Linux glibc. A plain pthread_mutex_unlock is therefore well
 * defined even when a thread other than the one that locked it calls it. For
 * anything that the thread that forks did not hold itself, that other thread
 * does not exist in the child at all.
 *
 * ctpool_slot_table.rwlock is the one exception. It is a read-write lock; see
 * the doc comment of ctpool_slot_t for the reason. prepare() below takes its
 * write side on the thread that calls fork(), once every other holder has
 * released it. The unlock of a glibc rwlock decides between a writer unlock
 * and a reader unlock by comparing the writer thread ID that it recorded with
 * the thread ID of the caller. fork() gives the one thread of the child a new
 * thread ID, so in the child a plain ccol_rw_lock_unlock returns 0 and
 * releases nothing, and every later _ctpool_resolve there blocks for ever.
 * The in_child branch of _ctpool_atfork_release_impl therefore reinitializes
 * the rwlock instead of unlocking it, exactly as the atfork handling of
 * clog_slot_table.rwlock in clogger.c and of the two rwlocks of
 * ccol_event_loop does.
 *
 * This mirrors the identical atfork handling of ccol_event_loop in
 * src/cthreadcomm.c exactly. See the comment of that module for the full
 * account of the two hangs that this shape of handler prevents. The first hang
 * is a fresh create call that inherits its own process-wide slot-table mutex
 * already locked. The second is an ordinary call on one object that inherits
 * the lock of a still-live object already locked.
 *
 * This module carries its own copy instead of a reliance on the one in
 * ccol_event_loop. The dispatch_pool of ccol_event_loop, which that module
 * creates whenever a caller configures num_reactor_threads > 1, is exactly
 * such a pool. ccol_event_loop has no way to reach the opaque ctpool internals
 * of this module from outside. The registration here instead covers every
 * ctpool that this module ever created, not only the ones of ccol_event_loop.
 * It therefore covers every caller of this module.
 *
 * prepare() walks every slot with in_use == true. That is the exact condition
 * that _ctpool_resolve itself trusts as the only sign that slot->ptr is safe
 * to dereference for an ordinary resolve. prepare() ALSO walks every slot with
 * torn_down == true. __ctpool_destroy clears in_use BEFORE it starts its own
 * teardown work. It does that under the write side of this same
 * ctpool_slot_table.rwlock. That teardown work can be slow, because it joins
 * every worker thread and frees the task queue.
 * The worker threads of a pool are therefore not gone at the instant that
 * in_use goes false. They are gone only after that join phase completes.
 * torn_down stays true for exactly that window; see the field comment of
 * ctpool_slot_t. A fork() that lands while a worker thread that nothing joined
 * yet still holds ptr->mu is therefore caught here, exactly like the mu of an
 * ordinary live pool. The library frees ptr, and finally clears slot->ptr,
 * well after both in_use and torn_down go false. By the established convention
 * of this file, a pool past that point is already off-limits for any purpose,
 * fork-related or not. This mechanism opens no new gap there.
 *
 * This mechanism does NOT extend to the mu of an individual ctpool_future, and
 * that is deliberate. ctpool_slot_table reaches every pool. It is the
 * complete, walkable registry of every live pool in this module. A future has
 * no equivalent registry after it leaves the task queue. ctpool_submit_future
 * hands the caller the only reference to a future that outlives the task list
 * of the pool. To find "every live future" at fork time, this module needs a
 * new, dedicated global registry with a lock of its own. That registry is well
 * outside the scope of this mechanism. ccol_event_loop excludes its own
 * per-entry
 * dispatch_lock for the same reason. See the comment of that module for the
 * same argument, that only a walk of a structure this mechanism does not hold
 * the right lock for can find one.
 *
 * worker_thread_fn, future_deref, future_cancel, ctpool_future_fulfill and
 * ctpool_future_get each hold the mu of a future for a short window. A fork()
 * inside one of those windows is therefore a real gap. It is a narrow one, and
 * the probability is low. The locking discipline of this module is the
 * reason. Every future->mu critical section is a handful of instructions. No
 * code holds it across a callback, and the condition variable itself releases
 * it for the one wait that exists. The gap stays open, rather than silently
 * claimed as covered. */
static void _ctpool_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(ccol_atfork_module_cthreadpool);
#endif
  ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);

  /* A table that the library released at unload holds no pool. */
  size_t n =
      ctpool_slot_table.slots ? cvector_elem_count(ctpool_slot_table.slots) : 0;
  for (size_t i = 0; i < n; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    ccol_mutex_lock(slot->ptr->mu);
  }
}

/* Both parent() and child() share this function. See the doc comment of
 * _ctpool_atfork_prepare for two things. The first is why a plain unlock is
 * correct for the PLAIN mutexes of this module, in both branches. The second
 * is why ctpool_slot_table.rwlock alone needs a reinitialize in the child,
 * which the end of this function does.
 *
 * It is safe to walk again the same structure that prepare() walked, and to
 * release every lock in the same way. Nothing can change the slot table or the
 * state of any live pool in between, because this function still holds every
 * lock that such a change needs.
 *
 * is_child also marks every still-live pool as foreign_since_fork, and drops
 * every pin that is outstanding against it. See the doc comment of
 * _ctpool_atfork_child_release for why the child needs both steps. See the
 * field comment of foreign_since_fork in struct cthread_pool for the SIGSEGV
 * that this closes. Neither step applies to the parent. The fork took nothing
 * away from the parent. Every one of its own threads, and
 * every pin that any of them held, is exactly as it was immediately before the
 * fork() call. */
static void _ctpool_atfork_release_impl(bool is_child) {
  size_t n =
      ctpool_slot_table.slots ? cvector_elem_count(ctpool_slot_table.slots) : 0;
  for (size_t i = 0; i < n; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    cthread_pool *pool = slot->ptr;

    /* A torn_down slot is already not in_use, and its own destroy is already
     * in progress in THIS process. Nothing can resolve it again, because
     * in_use is already false. Unlike a truly still-live pool, it therefore
     * has no way to reach the foreign_since_fork branch of
     * _ctpool_teardown_raw a second time in the child. The thread that
     * destroys it does not exist there, unless that thread is the one that
     * called fork(). The memory of this pool is then harmlessly unreachable
     * for the rest of the life of the child. Only an unlock of its mu is
     * needed to close the hang that this whole mechanism prevents, and the
     * code below does that for both branches. */
    if (slot->in_use && is_child) {
      atomic_store(&pool->foreign_since_fork, true);
      /* A parent-side thread that is now gone can be in the middle of a
       * resolve, inside a pinned _ctpool_resolve call, at the instant of the
       * fork(). That pin then stays outstanding forever from the point of
       * view of this process. Nothing here can ever run the matching
       * _ctpool_resolve_unpin call that the vanished thread would make. The
       * wait loop of _ctpool_teardown_raw blocks until the pin count reaches
       * 0, before it does anything else. Without a reset here, the destroy in
       * the child hangs forever. That is the same class of hang as an
       * inherited locked mutex; see the doc comment of _ctpool_atfork_prepare
       * above.
       *
       * A reset here is safe for the very common case. In that case no
       * thread in this process is itself already in a resolve of this exact
       * handle across this exact fork() call. One narrow case stays open: the
       * thread
       * that calls fork() has its OWN resolve in flight across its own fork()
       * call. That is a limit of a fork() call from inside a held pin, and
       * not a defect of this reset. */
      ccol_pintable_reset_for(&ctpool_pintable, pool->self_handle);

      /* The worker threads of a live pool spend most of their lives blocked in
       * ccol_cond_var_wait(pool->not_empty, pool->mu); see the main loop of
       * worker_thread_fn. That call, unlike a plain ccol_mutex_lock, releases
       * pool->mu for the length of the wait. The lock that
       * _ctpool_atfork_prepare takes on pool->mu therefore proves nothing
       * about some OTHER thread that the child does not have. At the exact
       * instant of the fork(), such a thread can sit inside the internal
       * waiter bookkeeping of pthread_cond_wait. The condvar implementation
       * of glibc tracks its registered waiters in state private to the
       * condvar itself, for example an internal waiter reference count and
       * generation or group counters. That state is fully independent of
       * whatever external mutex the condvar is paired with.
       *
       * fork() duplicates the raw memory of that bookkeeping as it is. The
       * thread that owned it is now gone. A LATER ccol_cond_var_signal or
       * _broadcast call on the same copied condvar in the child can then hang
       * on that stale internal state, which nothing can ever resolve. The
       * child ends up stuck inside pthread_cond_signal itself, and not inside
       * pool->mu at all. It reaches that point from the ordinary
       * ccol_cond_var_signal(pool->not_empty) call that submit_internal makes
       * after it puts a task in the queue.
       *
       * A plain "normal" pthread mutex does no owner tracking, so a bare
       * unlock from a different thread clears it fully and correctly; see the
       * header comment of this function. A condition variable has no such safe
       * "just unlock" equivalent for internal waiter state that may be stale.
       * The handling here is to REINITIALIZE every condvar of this pool,
       * with no condition. The one surviving thread of the child does that.
       * It does it before any application code in this process can reach
       * them. The slot-table rwlock at the end of this function is
       * reinitialized in the child for a different reason; see the comment
       * there.
       *
       * This is safe because this handler runs single-threaded and
       * synchronously, before fork() returns to application code. No
       * concurrent access to these condvars is possible yet. A freshly
       * initialized condvar with zero real waiters is the correct state for an
       * inherited pool. The real worker threads of that pool are the only
       * threads that could ever be genuine waiters. All of them are gone in
       * this process, whatever the fork() caught them doing. */
      if (ccol_cond_var_init(pool->not_empty) != 0)
        ccol_fatal_err("ctpool atfork release: failed to reinit not_empty");
      /* The same clock as the ordinary init, so that a timed submit in the
       * child keeps its CLOCK_MONOTONIC deadline. */
      if (_ctpool_init_monotonic_cond_var(&pool->not_full) != 0)
        ccol_fatal_err("ctpool atfork release: failed to reinit not_full");
      if (ccol_cond_var_init(pool->idle_cv) != 0)
        ccol_fatal_err("ctpool atfork release: failed to reinit idle_cv");
      /* The reasoning is the same as for the three condvars above, and the
       * same class of waiter needs it. A thread that is blocked in
       * ctpool_shutdown_drain or ctpool_shutdown_immediate, while it waits
       * out a shutdown that another thread runs, sits on this condvar. */
      if (ccol_cond_var_init(pool->shutdown_cv) != 0)
        ccol_fatal_err("ctpool atfork release: failed to reinit shutdown_cv");
    }

    ccol_mutex_unlock(pool->mu);
  }

  /* prepare() above took the write side of ctpool_slot_table.rwlock on this
   * same thread, the one that calls fork(). In the parent a plain
   * ccol_rw_lock_unlock from here releases it.
   *
   * In the child it does not. The unlock of a glibc rwlock tells a writer
   * unlock from a reader unlock by comparing the writer thread ID that it
   * recorded with the thread ID of the caller, and fork() gives the one thread
   * of the child a new thread ID. A plain unlock therefore returns 0 and
   * releases nothing, and every later _ctpool_resolve in this child blocks
   * for ever. The child reinitializes the rwlock instead, as
   * _clog_atfork_release in clogger.c and _cthreadcomm_atfork_release_impl in
   * ccol_event_loop do. That is safe because the child has exactly one thread
   * at this point, so nothing can be waiting on the rwlock. */
  if (is_child) {
    if (ccol_rw_lock_init(ctpool_slot_table.rwlock) != 0)
      ccol_fatal_err(
          "ctpool atfork release: failed to reinit slot table rwlock");
  } else {
    ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  }
}

static void _ctpool_atfork_release(void) { _ctpool_atfork_release_impl(false); }

/* The child-side counterpart of _ctpool_atfork_release. It releases the same
 * locks; see the comment of _ctpool_atfork_release_impl. It also marks the
 * worker threads of every inherited pool as permanently gone from the point of
 * view of this process. It must run before any application code in this
 * process can reach the shutdown path or the destroy path of one of these
 * pools. The child handler of pthread_atfork runs synchronously, as a part of
 * the return of fork() itself, strictly before the return value of fork()
 * reaches the code that called it. */
static void _ctpool_atfork_child_release(void) {
  _ctpool_atfork_release_impl(true);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* These are defined with the process-exit teardown below. They are declared
   here because the final locked section of __ctpool_destroy does the release
   that the destructor deferred. */
static bool _ctpool_any_slot_live_locked(void);
static void _ctpool_release_slot_table_locked(void);
static void _ctpool_release_slot_table_if_deferred_locked(void);

/* ========================================================================== */
/*                    CTPOOL HANDLE RESOLVE / UNPIN                           */
/* ========================================================================== */

/* Waits out every caller that is in flight and that resolved this pool before
 * the library retired its slot. This function polls. It does not sleep on a
 * condition variable, because the unpin side deliberately sends no wakeup: the
 * whole point of the unpin is to touch nothing but the pin. The wait is
 * unbounded by design. To give up means to free memory that a live resolver
 * still points at. */
static void _ctpool_wait_for_pins(cthread_pool *pool) {
  /* A pool whose handle the library never published has no slot to wait on. It
   * must not guess one. Without this check, a teardown that a failed
   * construction reaches derives an index from a zeroed handle, or from a slot
   * that is already back on the free list. It then waits out the pins of
   * whichever unrelated pool holds that slot. */
  if (pool->self_handle == 0) return;
  uint32_t idx = (uint32_t)(pool->self_handle >> 32);
  long delay_ns = 1000;
  while (ccol_pintable_pins(&ctpool_pintable, idx) > 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
    nanosleep(&ts, NULL);
    if (delay_ns < 1000000L) delay_ns *= 2;
  }
}

/* Resolves h and pins the result against a concurrent destroy. It returns NULL
 * if h is 0 or garbage. It also returns NULL if h names a slot that is free
 * now, or a slot that the library already reused, which gives the wrong
 * generation. On success the caller MUST call _ctpool_resolve_unpin(result)
 * exactly once, as soon as it finishes with the resolved cthread_pool*. */
static cthread_pool *_ctpool_resolve(ctpool h) {
  /* No lock and no shared write. Every public entry point of this module runs
   * this function. A write here to memory that another thread reads is
   * therefore paid by every submit, from every thread that submits. */
  return (cthread_pool *)ccol_pintable_pin(&ctpool_pintable, h);
}

static void _ctpool_resolve_unpin(cthread_pool *raw) {
  /* Releases the pin and touches nothing else. A lock of raw->mu here means
   * that every call takes the lock of this pool one more time, immediately
   * after it releases that lock. That is what turns concurrent submitters into
   * a convoy. There is also no wakeup to deliver: __ctpool_destroy polls the
   * pin count, and it does not sleep on a condition variable that waits for
   * this function.
   *
   * A read of raw->self_handle before the release is safe, because the pin is
   * still held at that point. After the release, the library can free the pool
   * at any instant. Nothing may touch raw after this call. */
  ccol_pintable_unpin(&ctpool_pintable, raw->self_handle);
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. It makes the next free-list push behave exactly like an
 * allocation failure, and then it disarms itself. Without it, the lost-index
 * recovery below needs a real out-of-memory condition at one specific
 * cvector_push_back call, which an ordinary test run cannot reach. */
static _Atomic bool g_ctpool_fail_next_free_index_push = false;

void _ctpool_force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_ctpool_fail_next_free_index_push, true);
}
#endif

/* Hands idx back for reuse. The caller holds the write lock.
 *
 * This function records a failed push. It does not swallow it. The slot that
 * the failed push names is already fully released. A silent drop of that index
 * therefore strands it for the life of the process, and it makes every later
 * create grow the table again. See ctpool_slot_table.lost_indices. */
static void _ctpool_free_index_release_locked(uint32_t idx) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_ctpool_fail_next_free_index_push, false)) {
    ctpool_slot_table.lost_indices++;
    return;
  }
#endif
  if (cvector_push_back(ctpool_slot_table.free_indices, &idx) != ccol_success)
    ctpool_slot_table.lost_indices++;
}

/* Recovers one slot index that a failed free-list push stranded; see
 * ctpool_slot_table.lost_indices. It scans for a slot that is released but
 * that nothing names. The caller holds the write lock and already found
 * free_indices empty. That is what makes "ptr is NULL and in_use is false"
 * mean lost rather than only free. A slot that the library released
 * successfully is on that list. A slot whose destroy is still in progress
 * keeps a ptr that is not NULL until the very end of that destroy.
 *
 * This function is out of line, and it runs only when the counter is above
 * zero, so an ordinary acquire pays one comparison for it. It sets the counter
 * to zero if the scan finds nothing. A counter that somehow outlives its slot
 * therefore cannot make every later acquire scan the table. */
static __attribute__((noinline)) bool _ctpool_reclaim_lost_index_locked(
    uint32_t *out_idx) {
  size_t slot_count = cvector_elem_count(ctpool_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (slot->ptr == NULL && !slot->in_use) {
      *out_idx = (uint32_t)i;
      ctpool_slot_table.lost_indices--;
      return true;
    }
  }
  ctpool_slot_table.lost_indices = 0;
  return false;
}

/* Allocates a fresh slot for pool, or reuses a freed one, and returns the
 * handle. It returns 0 if there is not enough memory. It runs once, from
 * ccol_create_cthread_pool_mp, after the object is otherwise fully
 * constructed. At that point every worker thread already runs. */
static ctpool _ctpool_handle_slot_acquire(cthread_pool *pool) {
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  /* The library released the table at unload. No handle can be published any
   * more, and the creation that asked fails cleanly. */
  if (!ctpool_slot_table.slots) {
    ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    return 0;
  }
  uint32_t idx;
  ctpool_slot_t *slot;
  if (cvector_elem_count(ctpool_slot_table.free_indices) > 0) {
    cvector_pop_back(ctpool_slot_table.free_indices, &idx);
    slot = (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  } else if (ctpool_slot_table.lost_indices > 0 &&
             _ctpool_reclaim_lost_index_locked(&idx)) {
    slot = (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  } else {
    /* The library can never publish a slot whose index is beyond what the pin
     * table holds. This code therefore refuses such a slot here, instead of a
     * claim and a rollback. A rollback puts an index that no later publish can
     * use onto the free list that every acquire pops from. This is reported as
     * an ordinary failure, which is how a caller must already treat a table
     * that cannot grow. */
    if (cvector_elem_count(ctpool_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
      return 0;
    }
    ctpool_slot_t fresh = {0};
    if (cvector_push_back(ctpool_slot_table.slots, &fresh) != ccol_success) {
      ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
      return 0; /* an ordinary, non-fatal out-of-memory failure */
    }
    idx = (uint32_t)cvector_elem_count(ctpool_slot_table.slots) - 1;
    slot = (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that collides with the reserved "invalid
   * handle" sentinel, which is 0. That value comes after about 2^32 reuses of
   * this exact slot index. See the identical guard in
   * chttpcli_handle_slot_acquire for the full rationale. */
  if (slot->generation == 0) slot->generation++;
  ctpool h = ((ctpool)idx << 32) | (ctpool)slot->generation;

  /* The library writes this before it publishes the handle. A resolver that
   * finds this pool therefore also finds the handle that its own unpin
   * needs. */
  pool->self_handle = h;

  /* A publish can allocate. A failure leaves a handle that no call can
   * resolve, so the slot goes back on the free list instead. */
  if (!ccol_pintable_publish(&ctpool_pintable, idx, slot->generation, pool)) {
    /* Cleared, because the slot goes back on the free list. A handle left
     * behind points the teardown of this pool at a slot that now belongs to
     * somebody else. */
    pool->self_handle = 0;
    /* This slot goes back to the free list in the same shape as a slot that a
     * destroy retires, and not merely unused. The walk of
     * _ctpool_atfork_prepare processes any slot with either flag set, and it
     * dereferences the ptr of that slot. A slot that goes back on the free
     * list must therefore leave both flags clear. */
    slot->ptr = NULL;
#if CCOL_FORK_SAFETY_REQUIRED
    slot->torn_down = false;
#endif
    _ctpool_free_index_release_locked(idx);
    ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    return 0;
  }

  slot->ptr = pool;
  slot->in_use = true;
#if CCOL_FORK_SAFETY_REQUIRED
  slot->torn_down = false; /* this is always false already when the library
                               reuses a slot (see the field comment of
                               ctpool_slot_t). The reset here is a standing
                               defence, rather than a trust in that invariant
                               alone */
#endif
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                    WORKER SELF-CALL DETECTION                              */
/* ========================================================================== */

/* A process-wide thread-local key. It records which cthread_pool, if any, the
 * calling thread is a worker thread of. worker_thread_fn sets it once, at its
 * own entry. Nothing assigns it again or clears it for the rest of the life of
 * that thread. One ctpool worker thread belongs privately to exactly one pool
 * instance for its whole life. A ctpool always creates and owns its own
 * dedicated OS threads, and it never shares them across pools. One "which
 * pool
 * am I" value is therefore enough. There is no set-and-clear lifecycle for
 * each job to manage here. The analogous ccol_event_loop_job_key_bundle of
 * ccol_event_loop does need one in cthreadcomm.c, because a dispatch_pool
 * worker there serves many jobs over its life, from many different
 * registrations.
 *
 * This key finds a task, or the on_complete callback of a task, that calls
 * ctpool_wait, _shutdown_drain, _shutdown_immediate or __ctpool_destroy on the
 * very pool that it runs on. worker_thread_fn gets `pool` as a bare pointer,
 * captured once at the start of the thread. That pointer is fully independent
 * of the resolve and pin mechanism that every public API entry point goes
 * through. Nothing about the pin count therefore says that "a worker of this
 * pool still uses it".
 *
 * Normally, one thing alone keeps the continued use of `pool` by a worker safe
 * across a shutdown: the ccol_thread_join loop of
 * _ctpool_shutdown_drain_internal and _ctpool_shutdown_immediate_internal.
 * That loop blocks until every worker truly finishes and returns from
 * worker_thread_fn, before any teardown proceeds. A self-call breaks that
 * guarantee silently. pthread_join on the ID of the calling thread returns
 * EDEADLK at once, and does not block. POSIX leaves that case undefined, and
 * pthread_join(3) of glibc documents this detection and rejection instead.
 * Nothing here checks the return value of ccol_thread_join. The join loop
 * therefore believes that every worker exited, while the calling worker is
 * still deep in its own call stack. That worker is about to return to
 * worker_thread_fn and to keep touching `pool`.
 *
 * For __ctpool_destroy this is a real, deterministic use-after-free. After the
 * task returns, task_free, ccol_mutex_lock, the decrement of
 * pool->active_count and ccol_cond_var_broadcast all run against the pool that
 * the destroy already freed. A pool with one worker, whose only task calls
 * ctpool_destroy on its own pool, hits this every time.
 *
 * live says whether the key exists. The creation of the key can fail, for
 * example in a process that already reached PTHREAD_KEYS_MAX. key then holds
 * no key of this module. It can name a key that another component owns, and
 * a value set on it would reach the destructor of that component. A pool
 * therefore refuses to be created while live is false, because its workers
 * could neither set the key nor be told apart from any other thread. live
 * goes false again, and the key is deleted, when the module unloads with no
 * pool left. Repeated cycles of dlopen() and dlclose() therefore do not use
 * up the keys of the process. */
static struct {
  ccol_thread_ls_key_t key;
  ccol_once_flag_t once;
  atomic_bool live;
} ctpool_worker_key_bundle = {.once = CCOL_ONCE_INIT};

static void _ctpool_init_worker_key(void) {
  if (ccol_thread_ls_key_create(ctpool_worker_key_bundle.key, NULL) == 0)
    atomic_store(&ctpool_worker_key_bundle.live, true);
}

/* True when the worker key exists. Every other use of the key comes from a
 * pool that exists, and a pool exists only after this answered true. */
static bool _ctpool_worker_key_available(void) {
  ccol_call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  return atomic_load(&ctpool_worker_key_bundle.live);
}

/* One frame of the stack of pools whose discarded tasks the calling thread
 * completes right now. _ctpool_discard_tasks pushes a frame, which lives on
 * its own C stack, around its loop and pops it afterwards. It is a stack and
 * not a single pool because an on_complete callback there can shut down
 * another pool, whose discards then run nested inside the outer ones; the
 * thread still counts as a worker of every pool on the stack. */
typedef struct _ctpool_discard_frame {
  cthread_pool *pool;
  struct _ctpool_discard_frame *prev;
} _ctpool_discard_frame;

static __thread _ctpool_discard_frame *_ctpool_discarding_top = NULL;

/* True when the calling thread is one of the worker threads of pool, and false
 * in every other case. That is, the call comes, directly or through other
 * calls, from inside a task that runs on pool, or from the on_complete
 * callback of that task. */
static bool _ctpool_is_self_call(cthread_pool *pool) {
  /* A thread that runs the on_complete callbacks of the discarded tasks of
   * pool counts as a worker of pool while it does; see
   * _ctpool_discard_tasks. */
  for (const _ctpool_discard_frame *f = _ctpool_discarding_top; f; f = f->prev)
    if (f->pool == pool) return true;
  ccol_call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  void *mark = ccol_thread_ls_get(ctpool_worker_key_bundle.key);
  if (mark == (void *)pool) return true;
  /* A worker of another pool carries that pool's mark. */
  if (mark != NULL) return false;
  /* No mark at all. That is every thread that is not a worker, and also a
   * worker of this pool whose pthread_setspecific failed: glibc allocates the
   * storage for a key index of 32 or above lazily for each thread, so that
   * call can fail with ENOMEM. Such a worker must still be recognised, or a
   * task that waits on, shuts down or destroys its own pool deadlocks or
   * frees the pool under itself. The IDs of the workers are written before
   * the handle of the pool is published and never change afterwards, so they
   * can be read here with no lock. Every caller of this function is a cold
   * path (a full queue, a wait, a shutdown or a destroy), which is why a scan
   * over the workers costs nothing that matters. */
  /* A slot whose worker has exited is skipped. glibc caches the stack and the
   * descriptor of a joined thread and gives both to the next thread that
   * starts, so a thread that the application starts after a shutdown commonly
   * carries the ID of a joined worker. A worker marks its slot as its last
   * write to the pool, before it can be joined, so the ID of a joined worker
   * never matches here. A pool inherited across fork() has no workers in this
   * process, and the children of that process reuse the descriptors of the
   * parent's threads in the same way. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (atomic_load(&pool->foreign_since_fork)) return false;
#endif
  ccol_thread_id_t me = ccol_get_thread_id();
  for (size_t i = 0; i < pool->num_threads; i++) {
    if (ccol_thread_id_equal(pool->threads[i].id, me) &&
        !atomic_load_explicit(&pool->threads[i].exited, memory_order_acquire))
      return true;
  }
  return false;
}

/* ========================================================================== */
/*                         INTERNAL HELPERS                                   */
/* ========================================================================== */

static void pool_free_self(cthread_pool *pool) {
  ccol_memmgmt_procs_t *mp = pool->m_procs;
  if (mp) {
    ccol_free_t free_fn = mp->free;
    free_fn(pool);
    free_fn(mp);
  } else {
    free(pool);
  }
}

/* The ceiling on the task free list of a pool with few workers, in nodes.
 * One node is a ctpool_task of five pointer-sized fields, so a pool keeps at
 * most 1.25 MiB of them on LP64, before allocator overhead, and only after
 * traffic that really had that many tasks in flight at once. That covers the
 * queue depth that a producer running ahead of one worker reaches in a burst
 * of about a hundred thousand submissions. */
#define CTPOOL_TASK_FREE_LIST_CEILING ((size_t)32768)

/* How many idle points a trim of the task free list waits for. A pool whose
 * workers keep up with its producer goes idle between almost every pair of
 * tasks, and a trim at each of them gives back nodes that the next burst
 * allocates again. See _ctpool_idle_unlock_and_trim. */
#define CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS ((uint32_t)256)

#ifdef RUNNING_UNIT_TESTS
/* Test-only. The number of idle points that a trim waits for, so that a test
 * can reach a trim with a few tasks. It defaults to the value that the
 * library ships with. See _ctpool_set_task_free_list_window_for_tests. */
static _Atomic uint32_t g_ctpool_trim_window =
    CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS;
#define CTPOOL_TRIM_WINDOW() atomic_load(&g_ctpool_trim_window)
void _ctpool_set_task_free_list_window_for_tests(uint32_t idle_points) {
  atomic_store(
      &g_ctpool_trim_window,
      idle_points ? idle_points : CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS);
}
#else
#define CTPOOL_TRIM_WINDOW() CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS
#endif

/* Trims the task free list at every CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS-th
 * idle point: it gives back to the allocator of the pool half of what the
 * list holds above num_threads * 4, unless the list ran empty since the
 * previous trim, which means that the traffic needed all of it. A list that
 * nothing needs therefore halves at every trim, and a list that the traffic
 * keeps using stays whole. The caller holds pool->mu and calls this at the
 * idle point, where no task is queued or running. The function unlinks the
 * nodes under the lock and frees them after it unlocks, because a custom free
 * function may be slow. It unlocks pool->mu itself on every path. It stays
 * out of line, so that the completion path of a worker keeps its shape. */
static __attribute__((noinline)) void _ctpool_idle_unlock_and_trim(
    cthread_pool *pool) {
  if (++pool->idle_points_since_trim < CTPOOL_TRIM_WINDOW()) {
    ccol_mutex_unlock(pool->mu);
    return;
  }
  pool->idle_points_since_trim = 0;
  bool ran_dry = pool->task_free_list_ran_dry;
  pool->task_free_list_ran_dry = false;
  size_t keep_min = pool->num_threads * 4;
  size_t size = pool->task_free_list_size;
  size_t excess = size > keep_min ? size - keep_min : 0;
  size_t to_free = ran_dry ? 0 : (excess + 1) / 2;
  ctpool_task *freed = NULL;
  if (to_free > 0) {
    freed = pool->task_free_list;
    ctpool_task *last = freed;
    for (size_t i = 1; i < to_free; i++) last = last->next;
    pool->task_free_list = last->next;
    last->next = NULL;
    pool->task_free_list_size -= to_free;
  }
  ccol_mutex_unlock(pool->mu);
  while (freed) {
    ctpool_task *next = freed->next;
    _ccol_mem_free(pool->m_procs, freed);
    freed = next;
  }
}

/*
 * Recycles task into the free list of the pool, if there is still room under
 * task_free_list_cap. The caller must already hold pool->mu. It returns true
 * when it recycles the task, and the caller then has nothing more to do. It
 * returns false when the caller must still genuinely free task through the
 * allocator of the pool. The caller must do that only AFTER it unlocks
 * pool->mu. A custom free function that the caller gives can be as slow as it
 * likes. It must therefore never run while other threads can be blocked on
 * this same lock.
 *
 * This function is separate from task_free for one reason. The hot completion
 * path of worker_thread_fn can fold this push into the same critical section
 * as the decrement of active_count that comes right after it. For the pool,
 * "this task is done" is one indivisible event. Two separate lock and unlock
 * round trips on pool->mu would pay for that one event twice.
 */
static bool task_release_locked(cthread_pool *pool, ctpool_task *task) {
  bool recycled = pool->task_free_list_size < pool->task_free_list_cap;
  if (recycled) {
    task->next = pool->task_free_list;
    pool->task_free_list = task;
    pool->task_free_list_size++;
  }
  return recycled;
}

/*
 * Releases a task struct. It recycles the struct into the free list of the
 * pool while there is still room under task_free_list_cap. In every other case
 * it genuinely frees the struct through the allocator of the pool.
 */
static void task_free(cthread_pool *pool, ctpool_task *task) {
  ccol_mutex_lock(pool->mu);
  bool recycled = task_release_locked(pool, task);
  ccol_mutex_unlock(pool->mu);

  if (!recycled) _ccol_mem_free(pool->m_procs, task);
}

/*
 * Genuinely frees, through the allocator of the pool, every node that the task
 * free list of the pool holds. task_free is different, because it can merely
 * recycle a node into that same list instead of a free.
 *
 * The teardown of the pool must call this function, after no further
 * submit or task_free call for this pool is possible. That point is when
 * every worker is joined and every pinned resolve is released. The thread and
 * memory cleanup of _ctpool_teardown_raw already depends on the same
 * precondition. Without this call, a node that the library only recycled, and
 * never handed back to m_procs, leaks. There is no lock here: when this runs,
 * nothing else can still touch the task_free_list of this pool.
 */
static void drain_task_free_list(cthread_pool *pool) {
  ctpool_task *t = pool->task_free_list;
  pool->task_free_list = NULL;
  pool->task_free_list_size = 0;
  while (t) {
    ctpool_task *next = t->next;
    _ccol_mem_free(pool->m_procs, t);
    t = next;
  }
}

/*
 * A pre-check for the try-submit path of a future. It takes the lock, looks at
 * the shutdown state and, for a bounded queue, at the capacity, and then
 * unlocks. The caller runs it BEFORE it allocates the future. A full queue
 * therefore returns ccol_container_full instead of ccol_not_enough_memory, and
 * a shutdown in progress returns ccol_not_permitted with no wasted allocation.
 * submit_internal repeats the same checks under the lock. The window between
 * the two checks is harmless, because submit_internal always returns the
 * correct code for the state that it sees.
 */
static ccol_retval_t try_precheck(cthread_pool *pool) {
  ccol_mutex_lock(pool->mu);
  ccol_retval_t r;
  if (pool->shutdown_drain || pool->shutdown_immediate) {
    r = ccol_not_permitted;
  } else if (pool->queue_cap > 0 && pool->queue_size >= pool->queue_cap) {
    r = ccol_container_full;
  } else {
    r = ccol_success;
  }
  ccol_mutex_unlock(pool->mu);
  return r;
}

/*
 * Allocates a task struct. It prefers a node that it recycles from the bounded
 * free list of the pool over a fresh allocation through the allocator of the
 * pool. See the field comment of task_free_list in struct cthread_pool for the
 * reason. This function zeroes a recycled node again before it hands the node
 * back. Every caller of task_alloc therefore gets exactly the same
 * fresh-calloc guarantee as a direct allocation. Several call sites set only
 * some of the fields of this struct, and they depend on the rest already being
 * NULL or zero. The `future` field on the plain submit path and the
 * `on_complete` field in alloc_future_task are two of them. A raw recycled
 * node still holds whatever an earlier, unrelated task stored last. Without
 * the zeroing, such a node breaks that guarantee silently.
 */
static ctpool_task *task_alloc(cthread_pool *pool) {
  ccol_mutex_lock(pool->mu);
  ctpool_task *task = pool->task_free_list;
  if (task) {
    pool->task_free_list = task->next;
    if (--pool->task_free_list_size == 0) pool->task_free_list_ran_dry = true;
  } else {
    pool->task_free_list_ran_dry = true;
  }
  ccol_mutex_unlock(pool->mu);

  if (task) {
    memset(task, 0, sizeof(*task));
    return task;
  }
  return (ctpool_task *)_ccol_mem_calloc(pool->m_procs, 1, sizeof(ctpool_task));
}

/* Puts the shutdown and capacity check of try_precheck, and the free-list pop
 * of task_alloc, into one pool->mu critical section. This is for the
 * try-submit path, where block == 0. Both operations read and write the same
 * pool state under the same lock. Two separate, back-to-back lock and unlock
 * round trips on every ctpool_try_submit call therefore double the contention
 * of this path on that lock for no gain.
 *
 * Take a caller that drives one submit for each unit of external work, at a
 * high rate. The per-request handoff from the reactor to the worker pool in
 * chttpsvr is one such caller. This lock is also shared with the dequeue and
 * completion critical sections of every worker thread. Half as many round
 * trips on the submit side therefore cuts the contention measurably.
 *
 * On ccol_success, *task_out is one of two things. It is a free-list node that
 * this function popped and that is still dirty, and the caller must still
 * memset it, which matches the contract of task_alloc. Or it is NULL, because
 * the free list was empty. The caller must then allocate on the heap through
 * the allocator of the pool, outside the lock, exactly as task_alloc
 * does. On any return other than success, *task_out is always NULL. */
static ccol_retval_t try_precheck_and_pop_free_list(cthread_pool *pool,
                                                    ctpool_task **task_out) {
  ccol_mutex_lock(pool->mu);
  ccol_retval_t r;
  if (pool->shutdown_drain || pool->shutdown_immediate) {
    r = ccol_not_permitted;
  } else if (pool->queue_cap > 0 && pool->queue_size >= pool->queue_cap) {
    r = ccol_container_full;
  } else {
    r = ccol_success;
  }
  ctpool_task *task = NULL;
  if (r == ccol_success) {
    task = pool->task_free_list;
    if (task) {
      pool->task_free_list = task->next;
      if (--pool->task_free_list_size == 0) pool->task_free_list_ran_dry = true;
    } else {
      pool->task_free_list_ran_dry = true;
    }
  }
  ccol_mutex_unlock(pool->mu);
  *task_out = task;
  return r;
}

/* Puts a task in the queue. The caller must hold pool->mu. */
static void enqueue(cthread_pool *pool, ctpool_task *task) {
  task->next = NULL;
  if (pool->tail) {
    pool->tail->next = task;
  } else {
    pool->head = task;
  }
  pool->tail = task;
  pool->queue_size++;
}

/* Takes the head task out of the queue. The caller must hold pool->mu, and the
 * queue must not be empty. */
static ctpool_task *dequeue(cthread_pool *pool) {
  ctpool_task *task = pool->head;
  pool->head = task->next;
  if (!pool->head) pool->tail = NULL;
  pool->queue_size--;
  return task;
}

/*
 * Destroys and frees f, after the caller sees the refcount of f reach 0. The
 * caller decrements that count under f->mu, and it unlocks f->mu before this
 * call. This is the shared tail for every "drop a reference, free on the last
 * release" site in this file. Those sites are future_deref, future_cancel, the
 * inline future-completion code of the worker in worker_thread_fn,
 * ctpool_future_fulfill, and the release by ctpool_future_get of its own
 * waiter reference. Each of them otherwise repeats the same three lines. Any
 * of those threads can be the one that sees zero, so this function does not
 * care which reference the caller held.
 */
static void future_destroy_if_unreferenced(ctpool_future *f, int remaining) {
  if (remaining == 0) {
    ccol_mutex_destroy(f->mu);
    ccol_cond_var_destroy(f->cv);
    free(f);
  }
}

/*
 * Decrements the refcount of a future. It frees the future when the count
 * reaches 0. The caller must NOT hold future->mu. This function locks and
 * unlocks it.
 */
static void future_deref(ctpool_future *f) {
  ccol_mutex_lock(f->mu);
  int remaining = --f->refcount;
  ccol_mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

/*
 * Cancels a future. It marks the future done and cancelled, it broadcasts to
 * every ctpool_future_get caller that waits, and then it gives up the
 * reference of the task.
 */
static void future_cancel(ctpool_future *f) {
  ccol_mutex_lock(f->mu);
  f->cancelled = true;
  f->done = true;
  ccol_cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  ccol_mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

/*
 * Takes the whole task list out of the queue in one step. The caller must hold
 * pool->mu. The caller must cancel and free the list that this function
 * returns only after it unlocks pool->mu. future_cancel locks future->mu, and
 * future->mu must never nest under pool->mu.
 */
static ctpool_task *steal_queue(cthread_pool *pool) {
  ctpool_task *list = pool->head;
  pool->head = NULL;
  pool->tail = NULL;
  pool->queue_size = 0;
  return list;
}

/*
 * Completes and frees every task of list, a list that steal_queue took out of
 * the queue before any worker ran it. A future task has its future cancelled.
 * A callback task has its on_complete called with ran == false, so every task
 * that a submit accepted gets exactly one on_complete call: with true from the
 * worker that ran it, or with false from here. The caller must not hold
 * pool->mu. future_cancel locks future->mu, which must never nest under
 * pool->mu, and an on_complete callback can call back into this module.
 *
 * While the callbacks run, the calling thread counts as a worker of pool for
 * _ctpool_is_self_call. A callback that calls ctpool_wait,
 * ctpool_shutdown_drain or ctpool_shutdown_immediate on pool therefore gets the
 * documented no-op instead of a wait on the shutdown that this very thread
 * still has to finish, and one that calls ctpool_destroy on pool gets the
 * documented fatal error instead of a wait on its own pin.
 *
 * recycle selects how a node is freed: through task_free, which can keep it
 * on the free list of a live pool, or straight back to m_procs, for a pool
 * that nothing can submit to again.
 */
static void _ctpool_discard_tasks(cthread_pool *pool, ctpool_task *list,
                                  bool recycle) {
  _ctpool_discard_frame frame = {pool, _ctpool_discarding_top};
  _ctpool_discarding_top = &frame;
  for (ctpool_task *t = list; t;) {
    ctpool_task *next = t->next;
    if (t->future)
      future_cancel(t->future);
    else if (t->on_complete)
      t->on_complete(t->arg, false);
    if (recycle)
      task_free(pool, t);
    else
      _ccol_mem_free(pool->m_procs, t);
    t = next;
  }
  _ctpool_discarding_top = frame.prev;
}

/*
 * Computes the absolute deadline for a timed submit: the current
 * CLOCK_MONOTONIC time plus timeout_us microseconds. It returns false if
 * clock_gettime fails. not_full, the condition variable that the deadline goes
 * to, measures on the same clock; see _ctpool_init_monotonic_cond_var.
 *
 * The sum saturates. A timeout that reaches past the largest time_t, such as
 * UINT64_MAX, gives the latest representable time, and not a tv_sec that
 * wrapped into the past and makes the submit time out at once.
 */
static bool make_abs_deadline(uint64_t timeout_us, struct timespec *abs_out) {
  return ccol_deadline_after_us(timeout_us, abs_out);
}

/* ========================================================================== */
/*                         WORKER THREAD                                      */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/* While true, a worker that starts skips setting its thread-local mark, as a
 * failed pthread_setspecific would. A test sets it only around the creation
 * of the pool it examines. */
atomic_bool _ctpool_skip_worker_mark_for_tests;
#endif /* RUNNING_UNIT_TESTS */

static void *worker_thread_fn(void *arg) {
  cthread_pool *pool = (cthread_pool *)arg;

  /* The library publishes this once, before this thread can run any task. No
   * code that this thread runs can therefore call back into this module before
   * this point. See the comment of ctpool_worker_key_bundle above for why
   * nothing must set it or clear it again for the rest of the life of this
   * thread. */
  ccol_call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  /* A failure here is tolerated: _ctpool_is_self_call then recognises this
   * thread from the worker IDs of the pool instead. A key that the library
   * already deleted at unload is never set, because its index can belong to
   * another component by now; the pool that this worker serves is then
   * refused a slot and torn down. */
#ifdef RUNNING_UNIT_TESTS
  if (!atomic_load(&_ctpool_skip_worker_mark_for_tests))
#endif /* RUNNING_UNIT_TESTS */
    if (atomic_load(&ctpool_worker_key_bundle.live))
      (void)ccol_thread_ls_set(ctpool_worker_key_bundle.key, (void *)pool);

  for (;;) {
    ccol_mutex_lock(pool->mu);

    /*
     * Wait until there is work to do, or until a shutdown condition is true.
     * The wakeup conditions are:
     *   - a task went into the queue (queue_size > 0)
     *   - shutdown_immediate is set
     *   - shutdown_drain is set AND the queue is now empty (all work done)
     */
    while (pool->queue_size == 0 && !pool->shutdown_immediate &&
           !pool->shutdown_drain) {
      ccol_cond_var_wait(pool->not_empty, pool->mu);
    }

    /* A drain shutdown with an empty queue. The job of this worker is done. */
    if (pool->shutdown_immediate ||
        (pool->shutdown_drain && pool->queue_size == 0)) {
      ccol_mutex_unlock(pool->mu);
      break;
    }

    ctpool_task *task = dequeue(pool);
    pool->active_count++;

    /* Wake a blocked submitter, now that a slot is free (bounded queue
     * only). */
    if (pool->queue_cap > 0) {
      ccol_cond_var_signal(pool->not_full);
    }

    ccol_mutex_unlock(pool->mu);

    /* Execute the task. */
    if (task->future) {
      void *result = task->fn_future(task->arg);
      ccol_mutex_lock(task->future->mu);
      task->future->result = result;
      task->future->done = true;
      ccol_cond_var_broadcast(task->future->cv);
      int remaining = --task->future->refcount;
      ccol_mutex_unlock(task->future->mu);
      future_destroy_if_unreferenced(task->future, remaining);
    } else {
      task->fn(task->arg);
      if (task->on_complete) {
        task->on_complete(task->arg, true);
      }
    }

    /* This folds the free-list release of task into the same critical section
     * as the decrement of active_count just below. Two separate lock and
     * unlock round trips on pool->mu would otherwise pay twice for one "this
     * task is done" event. See the doc comment of task_release_locked. The
     * _ccol_mem_free call itself, which can be a slow function that the caller
     * gives, still happens after the unlock. */
    ccol_mutex_lock(pool->mu);
    bool recycled = task_release_locked(pool, task);
    pool->active_count--;
    if (pool->active_count == 0 && pool->queue_size == 0) {
      ccol_cond_var_broadcast(pool->idle_cv);
      _ctpool_idle_unlock_and_trim(pool);
    } else {
      ccol_mutex_unlock(pool->mu);
    }
    if (!recycled) _ccol_mem_free(pool->m_procs, task);
  }

  /* See the threads field of struct cthread_pool. The IDs are safe to read
   * here: the creator wrote them before it published the handle, and the
   * shutdown that ends this loop reached this thread through pool->mu. */
  ccol_thread_id_t me = ccol_get_thread_id();
  for (size_t i = 0; i < pool->num_threads; i++) {
    if (ccol_thread_id_equal(pool->threads[i].id, me)) {
      atomic_store_explicit(&pool->threads[i].exited, true,
                            memory_order_release);
      break;
    }
  }
  return NULL;
}

/* A sentinel `idx` for _ctpool_teardown_raw. It means that the pool was never
 * registered in ctpool_slot_table at all. The rollback path of
 * ccol_create_cthread_pool_mp, after a slot acquire fails, uses it. There is
 * no slot to read there, so torn_down bookkeeping is neither possible nor
 * needed. No caller ever saw that handle, so a concurrent fork() cannot race
 * it through the slot-table atfork mechanism of this module. */
#define CTPOOL_TEARDOWN_NO_SLOT ((uint32_t)-1)

/* A forward declaration. The rollback path of ccol_create_cthread_pool_mp,
 * after a slot acquire fails, needs the shared teardown helper. That helper is
 * defined later in this file, right after the split between the internal and
 * the public shutdown_drain and shutdown_immediate functions that it depends
 * on. */
static void _ctpool_teardown_raw(cthread_pool *pool, uint32_t idx);

/* ========================================================================== */
/*                         CREATION                                           */
/* ========================================================================== */

ctpool ccol_create_cthread_pool_mp(size_t num_threads, size_t queue_capacity,
                                   ccol_memmgmt_procs_t *mprocs,
                                   char **err_str) {
  if (num_threads == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("num_threads must be >= 1");
    return CTPOOL_INVALID;
  }

  if (!ccol_verify_memmgmt_procs(mprocs, err_str)) return CTPOOL_INVALID;

  if (!_ctpool_worker_key_available()) {
    if (err_str)
      *err_str = CCOL_ERR_STR(
          "the thread-local worker key is not available: it could not be "
          "created, or the library already released it at unload");
    return CTPOOL_INVALID;
  }

  cthread_pool *pool =
      (cthread_pool *)(mprocs ? mprocs->calloc(1, sizeof(*pool))
                              : calloc(1, sizeof(*pool)));
  if (!pool) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate pool struct");
    return CTPOOL_INVALID;
  }

  if (mprocs) {
    pool->m_procs = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(*mprocs));
    if (!pool->m_procs) {
      mprocs->free(pool);
      if (err_str) *err_str = CCOL_ERR_STR("failed to allocate m_procs copy");
      return CTPOOL_INVALID;
    }
    memcpy(pool->m_procs, mprocs, sizeof(*mprocs));
  }

  /* Treat ccol_invalid_size as unbounded. */
  pool->queue_cap = (queue_capacity == ccol_invalid_size) ? 0 : queue_capacity;

  if (ccol_mutex_init(pool->mu) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("mutex init failed");
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (ccol_cond_var_init(pool->not_empty) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    ccol_mutex_destroy(pool->mu);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (_ctpool_init_monotonic_cond_var(&pool->not_full) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    ccol_mutex_destroy(pool->mu);
    ccol_cond_var_destroy(pool->not_empty);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (ccol_cond_var_init(pool->idle_cv) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    ccol_mutex_destroy(pool->mu);
    ccol_cond_var_destroy(pool->not_empty);
    ccol_cond_var_destroy(pool->not_full);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (ccol_cond_var_init(pool->shutdown_cv) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    ccol_mutex_destroy(pool->mu);
    ccol_cond_var_destroy(pool->not_empty);
    ccol_cond_var_destroy(pool->not_full);
    ccol_cond_var_destroy(pool->idle_cv);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }

#if CCOL_FORK_SAFETY_REQUIRED
  atomic_init(&pool->foreign_since_fork, false);
#endif

  pool->threads = (ctpool_worker_slot *)_ccol_mem_calloc(
      pool->m_procs, num_threads, sizeof(ctpool_worker_slot));
  if (!pool->threads) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate threads array");
    ccol_mutex_destroy(pool->mu);
    ccol_cond_var_destroy(pool->not_empty);
    ccol_cond_var_destroy(pool->not_full);
    ccol_cond_var_destroy(pool->idle_cv);
    ccol_cond_var_destroy(pool->shutdown_cv);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }

  pool->num_threads = num_threads;
  /* See the field comment of task_free_list_cap in struct cthread_pool for the
   * reason for this multiplier. */
  pool->task_free_list_cap = num_threads * 4 > CTPOOL_TASK_FREE_LIST_CEILING
                                 ? num_threads * 4
                                 : CTPOOL_TASK_FREE_LIST_CEILING;

  for (size_t i = 0; i < num_threads; i++) {
    if (ccol_thread_create(pool->threads[i].id, worker_thread_fn, pool) != 0) {
      /* Shut down the threads that already started, then clean up. */
      ccol_mutex_lock(pool->mu);
      pool->shutdown_drain = true;
      pool->shutdown_started = true;
      ccol_cond_var_broadcast(pool->not_empty);
      ccol_mutex_unlock(pool->mu);
      for (size_t j = 0; j < i; j++) {
        ccol_thread_join(pool->threads[j].id);
      }
      if (err_str) *err_str = CCOL_ERR_STR("pthread_create failed");
      _ccol_mem_free(pool->m_procs, pool->threads);
      ccol_mutex_destroy(pool->mu);
      ccol_cond_var_destroy(pool->not_empty);
      ccol_cond_var_destroy(pool->not_full);
      ccol_cond_var_destroy(pool->idle_cv);
      ccol_cond_var_destroy(pool->shutdown_cv);
      pool_free_self(pool);
      return CTPOOL_INVALID;
    }
  }

  /* The slot acquire is the LAST step, after every worker thread already runs.
   * The pool is otherwise fully constructed at this point. An out-of-memory
   * failure here is therefore safe to unwind on its own. There is no concern
   * about a double destroy or about visibility, because no caller ever saw the
   * handle. The earlier failure paths above are different. Here every worker
   * thread can already run, so the rollback must really stop them and must not
   * only free memory. _ctpool_teardown_raw does exactly that. It runs a drain
   * shutdown, joins every thread, and then frees. It reuses the same sequence
   * that __ctpool_destroy uses, rather than a hand-written variant that can
   * drift away from it. */
  ctpool h = _ctpool_handle_slot_acquire(pool);
  if (h == 0) {
    if (err_str)
      *err_str = CCOL_ERR_STR("failed to allocate ctpool handle slot");
    _ctpool_teardown_raw(pool, CTPOOL_TEARDOWN_NO_SLOT);
    return CTPOOL_INVALID;
  }

  return h;
}

/* ========================================================================== */
/*                         INTERNAL SUBMIT HELPER                             */
/* ========================================================================== */

/*
 * The common path for all three submit variants. `block` says how to wait for
 * a free slot: block until one is free, never wait, or wait until a deadline.
 * This function reads `abs_deadline` only when block == 2.
 *
 *   block == 0: try (return ccol_container_full at once)
 *   block == 1: block and wait
 *   block == 2: wait until the deadline
 */
/* The shutdown check and the wait for room that every submit makes. The
 * caller holds pool->mu. On ccol_success the lock is still held and the queue
 * has room. On any other result this function has released the lock. */
static ccol_retval_t submit_wait_for_room_locked(
    cthread_pool *pool, int block, const struct timespec *abs_deadline) {
  if (pool->shutdown_drain || pool->shutdown_immediate) {
    ccol_mutex_unlock(pool->mu);
    return ccol_not_permitted;
  }

  if (pool->queue_cap > 0) {
    while (pool->queue_size >= pool->queue_cap && !pool->shutdown_drain &&
           !pool->shutdown_immediate) {
      if (block == 0) {
        /* try_submit: return at once */
        ccol_mutex_unlock(pool->mu);
        return ccol_container_full;
      } else if (block == 1) {
        /*
         * A submit that blocks, and that comes from a worker of this very
         * pool, must not park on not_full. Such a submit comes from a task, or
         * from the on_complete callback of that task, that sends work back
         * into the pool it runs on. Only the workers of this pool can ever
         * free a slot, and this call runs on one of them. A wait here takes
         * that worker out of the set that can satisfy the wait. With every
         * worker in this state at once, and the queue at capacity, no thread
         * in the process can make progress again. The pool is then deadlocked
         * for good, and it takes with it every external submitter that is
         * blocked on the same queue.
         *
         * This code reports a full queue to such a caller instead, exactly as
         * ctpool_try_submit reports it. The recovery is then up to the caller:
         * a retry later, a handoff of the work to another pool, or an
         * unbounded queue. The check sits inside the wait loop for a full
         * queue. A submit that finds room therefore pays nothing for it, and
         * the blocking behaviour of an external submitter does not change.
         */
        if (_ctpool_is_self_call(pool)) {
          ccol_mutex_unlock(pool->mu);
          return ccol_container_full;
        }
        ccol_cond_var_wait(pool->not_full, pool->mu);
      } else {
        /* timed_submit */
        int rc =
            ccol_cond_var_timedwait(pool->not_full, pool->mu, *abs_deadline);
        if (rc == ETIMEDOUT) {
          ccol_mutex_unlock(pool->mu);
          return ccol_timed_out;
        } else if (rc != 0) {
          ccol_mutex_unlock(pool->mu);
          return ccol_unexpected_failure;
        }
      }
    }
    /* Check the shutdown state again after the wakeup. */
    if (pool->shutdown_drain || pool->shutdown_immediate) {
      ccol_mutex_unlock(pool->mu);
      return ccol_not_permitted;
    }
  }

  return ccol_success;
}

static ccol_retval_t submit_internal(cthread_pool *pool, ctpool_task *task,
                                     int block,
                                     const struct timespec *abs_deadline) {
  ccol_mutex_lock(pool->mu);
  ccol_retval_t r = submit_wait_for_room_locked(pool, block, abs_deadline);
  if (r != ccol_success) return r;
  enqueue(pool, task);
  ccol_cond_var_signal(pool->not_empty);
  ccol_mutex_unlock(pool->mu);
  return ccol_success;
}

/* A blocking or timed submit that takes its task node from the free list in
 * the same pool->mu critical section as the enqueue, so the submit locks the
 * pool once and not twice. The pool->mu of a busy pool is contended by every
 * worker, which takes it for each dequeue, so each round trip that a submit
 * saves is a handoff of that line that it does not wait for. When the free
 * list is empty, the node is allocated outside the lock, through the
 * allocator of the pool, and the checks run again, because the queue can
 * have filled or the pool can have started to shut down meanwhile. */
static ccol_retval_t submit_pop_and_enqueue(
    cthread_pool *pool, void (*fn)(void *), void *arg,
    void (*on_complete)(void *, bool), int block,
    const struct timespec *abs_deadline) {
  ctpool_task *spare = NULL;
  ccol_mutex_lock(pool->mu);
  for (;;) {
    ccol_retval_t r = submit_wait_for_room_locked(pool, block, abs_deadline);
    if (r != ccol_success) {
      if (spare) _ccol_mem_free(pool->m_procs, spare);
      return r;
    }
    ctpool_task *task = pool->task_free_list;
    if (task) {
      pool->task_free_list = task->next;
      if (--pool->task_free_list_size == 0) pool->task_free_list_ran_dry = true;
    } else {
      pool->task_free_list_ran_dry = true;
      task = spare;
      spare = NULL;
    }
    if (!task) {
      ccol_mutex_unlock(pool->mu);
      spare = (ctpool_task *)_ccol_mem_calloc(pool->m_procs, 1,
                                              sizeof(ctpool_task));
      if (!spare) return ccol_not_enough_memory;
      ccol_mutex_lock(pool->mu);
      continue;
    }
    memset(task, 0, sizeof(*task));
    task->fn = fn;
    task->arg = arg;
    task->on_complete = on_complete;
    enqueue(pool, task);
    ccol_cond_var_signal(pool->not_empty);
    ccol_mutex_unlock(pool->mu);
    if (spare) _ccol_mem_free(pool->m_procs, spare);
    return ccol_success;
  }
}

/* ========================================================================== */
/*                         TASK SUBMISSION                                    */
/* ========================================================================== */

/*
 * The shared body for ctpool_submit, ctpool_try_submit and
 * ctpool_timed_submit. Those three differ only in the block and timeout_us
 * pair that they pass to submit_internal. Without this shared body, each of
 * them repeats the full sequence around that call: resolve, validate,
 * allocate, submit, clean up and unpin.
 *
 * block follows the block convention of submit_internal: 0 is try, 1 blocks,
 * and 2 is timed. timeout_us is the relative timeout of the caller for
 * block == 2, in microseconds, and is not read otherwise. This function
 * computes the absolute deadline itself, with make_abs_deadline. The contract
 * of ctpool_timed_submit fixes where in the validation order that happens. It
 * happens after the code resolves pool and validates fn, and before it
 * allocates the task node.
 *
 * The try-submit precheck skips the allocation of a task node that this
 * function discards at once. It applies exactly when block == 0. "This is
 * try_submit" and "try_precheck runs" are therefore the same condition.
 */
static ccol_retval_t submit_generic(ctpool pool, void (*fn)(void *), void *arg,
                                    void (*on_complete)(void *, bool),
                                    int block, uint64_t timeout_us) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  struct timespec deadline;
  const struct timespec *abs_deadline = NULL;
  ctpool_task *task;
  if (block == 2) {
    if (!make_abs_deadline(timeout_us, &deadline)) {
      _ctpool_resolve_unpin(raw);
      return ccol_unexpected_failure;
    }
    abs_deadline = &deadline;
  }
  if (block != 0) {
    ccol_retval_t r =
        submit_pop_and_enqueue(raw, fn, arg, on_complete, block, abs_deadline);
    _ctpool_resolve_unpin(raw);
    return r;
  }
  {
    ccol_retval_t pre = try_precheck_and_pop_free_list(raw, &task);
    if (pre != ccol_success) {
      _ctpool_resolve_unpin(raw);
      return pre;
    }
    /* try_precheck_and_pop_free_list only pops a node that the library already
     * recycled. It never touches the allocator itself. A NULL task here means
     * that the free list was empty, which matches the identical fallback of
     * task_alloc. */
    if (task)
      memset(task, 0, sizeof(*task));
    else
      task =
          (ctpool_task *)_ccol_mem_calloc(raw->m_procs, 1, sizeof(ctpool_task));
  }
  if (!task) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  task->fn = fn;
  task->arg = arg;
  task->on_complete = on_complete;

  ccol_retval_t r = submit_internal(raw, task, block, abs_deadline);
  if (r != ccol_success) task_free(raw, task);
  _ctpool_resolve_unpin(raw);
  return r;
}

ccol_retval_t ctpool_submit(ctpool pool, void (*fn)(void *), void *arg,
                            void (*on_complete)(void *, bool)) {
  return submit_generic(pool, fn, arg, on_complete, 1, 0);
}

ccol_retval_t ctpool_try_submit(ctpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *, bool)) {
  return submit_generic(pool, fn, arg, on_complete, 0, 0);
}

ccol_retval_t ctpool_timed_submit(ctpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *, bool),
                                  uint64_t timeout_us) {
  /* A timeout_us of 0 behaves like try_submit. This code delegates before it
   * resolves pool at all. A resolve here, only to unpin at once and then
   * delegate, costs two resolve and unpin pairs. This path pays for exactly
   * one. The resolve inside ctpool_try_submit is what validates pool and fn in
   * either case, so the result that a caller sees is the same for every input,
   * valid or not. */
  if (timeout_us == 0) return ctpool_try_submit(pool, fn, arg, on_complete);
  return submit_generic(pool, fn, arg, on_complete, 2, timeout_us);
}

/* ========================================================================== */
/*                         FUTURES                                            */
/* ========================================================================== */

/*
 * Allocates a future and its linked task in one step. On success it sets
 * *task_out and returns the future, whose refcount is 2. On any allocation
 * failure it cleans up and returns NULL.
 */
static ctpool_future *alloc_future_task(cthread_pool *pool, void *(*fn)(void *),
                                        void *arg, ctpool_task **task_out) {
  ctpool_future *f = (ctpool_future *)calloc(1, sizeof(ctpool_future));
  if (!f) return NULL;
  if (ccol_mutex_init(f->mu) != 0) {
    free(f);
    return NULL;
  }
  if (ccol_cond_var_init(f->cv) != 0) {
    ccol_mutex_destroy(f->mu);
    free(f);
    return NULL;
  }
  f->refcount = 2; /* one reference for the caller, one for the task */

  ctpool_task *task = task_alloc(pool);
  if (!task) {
    ccol_mutex_destroy(f->mu);
    ccol_cond_var_destroy(f->cv);
    free(f);
    return NULL;
  }
  task->fn_future = fn;
  task->arg = arg;
  task->future = f;
  *task_out = task;
  return f;
}

/* Undoes alloc_future_task after a submit fails. */
static void free_future_task(cthread_pool *pool, ctpool_future *f,
                             ctpool_task *task) {
  task_free(pool, task);
  ccol_mutex_destroy(f->mu);
  ccol_cond_var_destroy(f->cv);
  free(f);
}

/*
 * The shared body for ctpool_submit_future, ctpool_try_submit_future and
 * ctpool_timed_submit_future. It mirrors the factoring of submit_generic above
 * and uses the same block and timeout_us convention.
 *
 * This function sets *out to NULL as its very first step, with no condition,
 * before it even resolves pool. Every caller below already guarantees that out
 * itself is not NULL. ctpool_try_submit_future and ctpool_timed_submit_future
 * check out themselves, and they return ccol_invalid_args with no call to this
 * function when out is NULL. ctpool_submit_future always passes the address of
 * its own local variable. This one assignment is therefore what makes *out
 * reliably NULL on every failure path of this function. It matches the
 * documented contract of each public function, and it does not repeat that
 * guarantee at every single return site.
 */
static ccol_retval_t submit_future_generic(ctpool pool, void *(*fn)(void *),
                                           void *arg, int block,
                                           uint64_t timeout_us,
                                           ctpool_future **out) {
  *out = NULL;

  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  struct timespec deadline;
  const struct timespec *abs_deadline = NULL;
  if (block == 2) {
    if (!make_abs_deadline(timeout_us, &deadline)) {
      _ctpool_resolve_unpin(raw);
      return ccol_unexpected_failure;
    }
    abs_deadline = &deadline;
  } else if (block == 0) {
    ccol_retval_t pre = try_precheck(raw);
    if (pre != ccol_success) {
      _ctpool_resolve_unpin(raw);
      return pre;
    }
  }

  ctpool_task *task;
  ctpool_future *f = alloc_future_task(raw, fn, arg, &task);
  if (!f) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }

  ccol_retval_t r = submit_internal(raw, task, block, abs_deadline);
  if (r != ccol_success) {
    free_future_task(raw, f, task);
    _ctpool_resolve_unpin(raw);
    return r;
  }
  *out = f;
  _ctpool_resolve_unpin(raw);
  return ccol_success;
}

ctpool_future *ctpool_submit_future(ctpool pool, void *(*fn)(void *),
                                    void *arg) {
  ctpool_future *f = NULL;
  submit_future_generic(pool, fn, arg, 1, 0, &f);
  return f;
}

ccol_retval_t ctpool_try_submit_future(ctpool pool, void *(*fn)(void *),
                                       void *arg, ctpool_future **out) {
  if (!out) return ccol_invalid_args;
  return submit_future_generic(pool, fn, arg, 0, 0, out);
}

ccol_retval_t ctpool_timed_submit_future(ctpool pool, void *(*fn)(void *),
                                         void *arg, uint64_t timeout_us,
                                         ctpool_future **out) {
  if (!out) return ccol_invalid_args;
  /* A timeout_us of 0 behaves like try_submit_future. See the identical
   * delegation comment of ctpool_timed_submit above for why a direct call to
   * submit_future_generic here keeps the behaviour. The alternative is a
   * resolve first and then a delegation to ctpool_try_submit_future. */
  if (timeout_us == 0) return submit_future_generic(pool, fn, arg, 0, 0, out);
  return submit_future_generic(pool, fn, arg, 2, timeout_us, out);
}

void *ctpool_future_get(ctpool_future *f) {
  if (!f) return NULL;
  ccol_mutex_lock(f->mu);
  /*
   * A waiter holds a reference of its own for the whole call. It takes that
   * reference under f->mu before the first wait, and it gives the reference up
   * under the same mutex on the way out. Without the reference, this call
   * blocks inside a struct that it has no claim on. Every other reference, the
   * one of the queued task and the one of the caller, can go while this thread
   * sits in ccol_cond_var_wait. The release that brings the count to zero then
   * destroys f->mu, destroys f->cv and frees f under this thread.
   *
   * The completion broadcast makes that likely, and not merely possible. One
   * broadcast wakes every waiter at once, and each of them must lock f->mu
   * again to leave the wait. Whichever thread gets out first can therefore
   * drop the last other reference while the rest are still inside the mutex.
   *
   * With the reference, the count cannot reach zero while a waiter is still
   * here. The last thread out of the future destroys it, exactly once. That
   * thread is a waiter or a reference owner, in whatever order they finish.
   * The increment and the decrement are plain int operations, on a mutex that
   * this call already holds. A get with no contention therefore pays two
   * arithmetic operations and one branch that the CPU already predicts.
   */
  f->refcount++;
  while (!f->done) {
    ccol_cond_var_wait(f->cv, f->mu);
  }
  void *result = f->result;
  int remaining = --f->refcount;
  ccol_mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
  return result;
}

bool ctpool_future_done(ctpool_future *f) {
  if (!f) return false;
  ccol_mutex_lock(f->mu);
  bool done = f->done;
  ccol_mutex_unlock(f->mu);
  return done;
}

bool ctpool_future_cancelled(ctpool_future *f) {
  if (!f) return false;
  ccol_mutex_lock(f->mu);
  bool cancelled = f->cancelled;
  ccol_mutex_unlock(f->mu);
  return cancelled;
}

void ctpool_future_free(ctpool_future *f) {
  if (!f) return;
  future_deref(f);
}

ctpool_future *ctpool_future_create_detached(char **err_str) {
  ctpool_future *f = (ctpool_future *)calloc(1, sizeof(ctpool_future));
  if (!f) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate future");
    return NULL;
  }
  if (ccol_mutex_init(f->mu) != 0) {
    free(f);
    if (err_str) *err_str = CCOL_ERR_STR("future mutex init failed");
    return NULL;
  }
  if (ccol_cond_var_init(f->cv) != 0) {
    ccol_mutex_destroy(f->mu);
    free(f);
    if (err_str) *err_str = CCOL_ERR_STR("future cond_var init failed");
    return NULL;
  }
  f->refcount = 2; /* one reference for the caller, one for the producer */
  return f;
}

ccol_retval_t ctpool_future_fulfill(ctpool_future *f, void *result) {
  if (!f) return ccol_invalid_args;
  ccol_mutex_lock(f->mu);
  if (f->done) {
    ccol_mutex_unlock(f->mu);
    return ccol_not_permitted;
  }
  f->result = result;
  f->done = true;
  ccol_cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  ccol_mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
  return ccol_success;
}

/* ========================================================================== */
/*                         POOL MANAGEMENT                                    */
/* ========================================================================== */

void ctpool_wait(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* A self-call; see the comment of ctpool_worker_key_bundle above. A task
   * that calls ctpool_wait on the very pool it runs on deadlocks against
   * itself. The pool still counts that task in active_count until the task
   * returns. This code treats such a pool as idle instead. It mirrors how the
   * code a few lines below already treats a foreign pool, one that this
   * process inherited across a fork, as idle. The reason is the same: nothing
   * can ever satisfy a wait here. */
  if (_ctpool_is_self_call(raw)) {
    _ctpool_resolve_unpin(raw);
    return;
  }
  ccol_mutex_lock(raw->mu);
  /* A foreign pool is one that this process inherited across a fork; see the
   * field comment of foreign_since_fork in struct cthread_pool. The
   * active_count and the queue_size of such a pool can never legitimately
   * reach zero through the actions of this process. Every worker thread that
   * could process the rest of the queue, or finish a task in flight, exists
   * only in the parent that is gone. Nothing in this process ever decrements
   * active_count, drains queue_size or broadcasts idle_cv again. A wait on
   * that condition here hangs forever. This code treats a foreign pool as idle
   * instead. It mirrors how _ctpool_shutdown_drain_internal and
   * _ctpool_shutdown_immediate_internal already skip a join of the worker
   * threads of a foreign pool, which do not exist either. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&raw->foreign_since_fork)) {
    while (raw->active_count > 0 || raw->queue_size > 0) {
      ccol_cond_var_wait(raw->idle_cv, raw->mu);
    }
  }
#else
  while (raw->active_count > 0 || raw->queue_size > 0) {
    ccol_cond_var_wait(raw->idle_cv, raw->mu);
  }
#endif
  ccol_mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
}

/* Publishes the fact that the library joined every worker of this pool. It
 * then releases every other shutdown call that waits on that fact. Whichever
 * of the two shutdown bodies below did the real shutdown calls this as its
 * very last step. A pool that never reaches one of them publishes nothing. A
 * construction rollback that joins its own partly started workers inline is
 * such a pool. No caller ever saw its handle, so nothing can be waiting. */
static void _ctpool_mark_shutdown_complete(cthread_pool *pool) {
  ccol_mutex_lock(pool->mu);
  pool->shutdown_complete = true;
  ccol_cond_var_broadcast(pool->shutdown_cv);
  ccol_mutex_unlock(pool->mu);
}

/* The internal shutdown_drain body, which takes a raw pointer. Two places
 * share it: the public ctpool_shutdown_drain wrapper below, and
 * _ctpool_teardown_raw. Both __ctpool_destroy and the rollback of
 * ccol_create_cthread_pool_mp, after a slot acquire fails, use
 * _ctpool_teardown_raw. This mirrors the _chttpsvr_stop_internal pattern that
 * chttpsvr_stop already uses. */
static void _ctpool_shutdown_drain_internal(cthread_pool *pool) {
  ccol_mutex_lock(pool->mu);
  if (pool->shutdown_started) {
    /* Another call already owns the shutdown of this pool. This code waits it
     * out, and it does not return. The library sets shutdown_started before it
     * wakes a single worker, and long before it joins one. A return here hands
     * the caller a pool whose workers all still run, and whose queued tasks
     * did not run. That breaks the documented guarantee of this function, that
     * it blocks until every queued task and every active task completes, for
     * every caller but the first.
     *
     * The owner sets shutdown_complete and broadcasts below, after its own
     * join loop. That owner is never this thread. The public wrapper rejects a
     * self-call before it reaches here, and the caller of
     * _ctpool_teardown_raw is never a worker of this pool. This wait therefore
     * cannot deadlock against the join that it waits on. */
    while (!pool->shutdown_complete) {
      ccol_cond_var_wait(pool->shutdown_cv, pool->mu);
    }
    ccol_mutex_unlock(pool->mu);
    return;
  }
  pool->shutdown_drain = true;
  pool->shutdown_started = true;
  /* Wake the workers that block on an empty queue, so they can read
   * shutdown_drain. */
  ccol_cond_var_broadcast(pool->not_empty);
  /* Wake the submitters that block on a full queue, so they get
   * ccol_not_permitted. */
  ccol_cond_var_broadcast(pool->not_full);
  ccol_mutex_unlock(pool->mu);

  /* foreign_since_fork; see the field comment in struct cthread_pool. This
   * process inherited pool across a fork() call. It never created any thread
   * in threads[], and it can never join one of them. A try SIGSEGVs inside
   * __pthread_clockjoin_ex in glibc. This code therefore treats every worker
   * as already joined. _ctpool_teardown_raw still frees pool->threads safely
   * later, whatever this flag says, because that array is plain data. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      ccol_thread_join(pool->threads[i].id);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    ccol_thread_join(pool->threads[i].id);
  }
#endif

  _ctpool_mark_shutdown_complete(pool);
}

void ctpool_shutdown_drain(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* A self-call; see the comment of ctpool_worker_key_bundle above. This code
   * treats it as a complete no-op. It deliberately leaves shutdown_drain and
   * shutdown_started untouched, and it does not proceed.
   *
   * To proceed still joins every OTHER worker, which is safe on its own. But
   * it can never join the calling thread itself. ccol_thread_join on the ID of
   * the calling thread returns EDEADLK at once, and it does not block. Nothing
   * checks that return value. A shutdown_started flag set here then stops
   * every LATER, legitimate external shutdown_drain, shutdown_immediate or
   * destroy call from a retry of that join. Both internal helpers
   * below become a no-op at once when shutdown_started is already true. The OS
   * resources of that one worker thread then leak for the rest of the life of
   * the process.
   *
   * Every flag stays untouched here. A later, correct call from outside
   * therefore still does a real, complete shutdown. By then this worker
   * returned to its own idle wait long ago, and it is an ordinary worker that
   * the library can join again. */
  if (_ctpool_is_self_call(raw)) {
    _ctpool_resolve_unpin(raw);
    return;
  }
  _ctpool_shutdown_drain_internal(raw);
  _ctpool_resolve_unpin(raw);
}

/* The internal shutdown_immediate body, which takes a raw pointer. See the
 * comment of _ctpool_shutdown_drain_internal above for why this split
 * exists. */
static void _ctpool_shutdown_immediate_internal(cthread_pool *pool) {
  ccol_mutex_lock(pool->mu);
  if (pool->shutdown_started) {
    /* A shutdown is already under way. Two things must still happen here. To
     * skip either one breaks the documented contract of this function for
     * every caller but the first.
     *
     * The first thing is the escalation. If the shutdown that already runs is
     * a DRAIN, the queued tasks that it is about to run are exactly what this
     * call promises to discard. The futures of those tasks are exactly what it
     * promises to cancel. A set of shutdown_immediate, and a steal of the
     * queue here, is what makes "immediate wins over drain" true. Without it,
     * a request for an immediate shutdown while a drain is in flight silently
     * delivers drain behaviour, with nothing discarded and no future
     * cancelled.
     *
     * The second thing is the block. The library sets shutdown_started before
     * it wakes a single worker, and long before it joins one. A return on that
     * flag alone hands the caller a pool whose workers all still run. That
     * goes against the documented promise to block until every worker thread
     * exits. */
    if (!pool->shutdown_immediate) {
      pool->shutdown_immediate = true;
      ctpool_task *escalated = steal_queue(pool);
      /* The discarded tasks are completed with pool->mu unlocked; see
       * _ctpool_discard_tasks. This is the same shape as the owning path
       * below. */
      ccol_mutex_unlock(pool->mu);
      _ctpool_discard_tasks(pool, escalated, true);
      ccol_mutex_lock(pool->mu);
      ccol_cond_var_broadcast(pool->not_empty);
      ccol_cond_var_broadcast(pool->not_full);
      /* The queue is discarded now. Once active_count is already zero,
       * nothing is left to broadcast idle_cv for a ctpool_wait caller. */
      if (pool->active_count == 0) {
        ccol_cond_var_broadcast(pool->idle_cv);
      }
    }
    while (!pool->shutdown_complete) {
      ccol_cond_var_wait(pool->shutdown_cv, pool->mu);
    }
    ccol_mutex_unlock(pool->mu);
    return;
  }
  pool->shutdown_immediate = true;
  pool->shutdown_started = true;

  ctpool_task *discarded = steal_queue(pool);
  /* Complete the discarded tasks with pool->mu unlocked; see
   * _ctpool_discard_tasks. */
  ccol_mutex_unlock(pool->mu);
  _ctpool_discard_tasks(pool, discarded, true);
  ccol_mutex_lock(pool->mu);

  /* Wake the workers that block on not_empty, so they see
   * shutdown_immediate. */
  ccol_cond_var_broadcast(pool->not_empty);
  /* Wake the submitters that block on not_full. */
  ccol_cond_var_broadcast(pool->not_full);
  /* Wake the ctpool_wait callers when no active task is left. The queue is
   * discarded and active_count is already zero, so no task ever broadcasts
   * idle_cv on its own. This code must do it here. */
  if (pool->active_count == 0) {
    ccol_cond_var_broadcast(pool->idle_cv);
  }
  ccol_mutex_unlock(pool->mu);

  /* See the identical guard and comment of _ctpool_shutdown_drain_internal. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      ccol_thread_join(pool->threads[i].id);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    ccol_thread_join(pool->threads[i].id);
  }
#endif

  _ctpool_mark_shutdown_complete(pool);
}

void ctpool_shutdown_immediate(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* A self-call. See the identical guard and comment of ctpool_shutdown_drain
   * above. The same reasoning applies here without change: it prevents a
   * worker thread that nothing joins and that leaks permanently. */
  if (_ctpool_is_self_call(raw)) {
    _ctpool_resolve_unpin(raw);
    return;
  }
  _ctpool_shutdown_immediate_internal(raw);
  _ctpool_resolve_unpin(raw);
}

size_t ctpool_pending_count(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return 0;
  ccol_mutex_lock(raw->mu);
  size_t n = raw->queue_size;
  ccol_mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
  return n;
}

size_t ctpool_active_count(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return 0;
  ccol_mutex_lock(raw->mu);
  size_t n = raw->active_count;
  ccol_mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
  return n;
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/* Two places share this function: __ctpool_destroy, and the rollback of
 * ccol_create_cthread_pool_mp after a slot acquire fails. The forward
 * declaration is above ccol_create_cthread_pool_mp.
 *
 * The order of the waits here is the OPPOSITE of the order in the teardown
 * helpers of ccol_event_loop and clru_cache, and that is deliberate. Only a
 * not_full broadcast releases the blocking wait of submit_internal on a full
 * bounded queue. In the worst case every worker thread is itself stuck, and
 * only the broadcast of the shutdown can then release a submitter that is
 * blocked and pinned. A drain shutdown BEFORE the wait on the pin count
 * guarantees that the broadcast already happened. The wait below therefore
 * always completes. Without this order, it can deadlock against a submitter
 * that this same function never wakes. On the rollback path of
 * ccol_create_cthread_pool_mp, no caller ever saw a handle. There is then no
 * slot to wait on at all, and the wait phase returns at once.
 *
 * The non-foreign path below always calls _ctpool_shutdown_drain_internal,
 * with no condition. It does not first peek at pool->shutdown_started. Such a
 * peek can only happen with pool->mu unlocked. A pinned ctpool_shutdown_drain
 * or ctpool_shutdown_immediate call that is in flight writes the same field
 * under its lock. The peek then races that write.
 * _ctpool_shutdown_drain_internal is already idempotent under its own lock,
 * because it becomes a no-op at the instant it sees shutdown_started already
 * true. An unconditional call therefore costs nothing extra in the ordinary
 * case, and it needs no unsynchronized read at all. */
#if CCOL_FORK_SAFETY_REQUIRED
/* Clears slots[idx].torn_down; see the field comment of ctpool_slot_t. It is a
 * no-op when idx is CTPOOL_TEARDOWN_NO_SLOT, which means that the pool was
 * never registered in the slot table. In every caller it must run strictly
 * before pool->mu becomes unsafe for _ctpool_atfork_prepare to dereference.
 * pool->mu becomes unsafe when the library destroys it, or, on the
 * foreign_since_fork path, when the library frees the struct that holds it. */
static void _ctpool_teardown_clear_torn_down(uint32_t idx) {
  if (idx == CTPOOL_TEARDOWN_NO_SLOT) return;
  ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  ctpool_slot_t *slot =
      (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  slot->torn_down = false;
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
}
#endif

static void _ctpool_teardown_raw(cthread_pool *pool, uint32_t idx) {
#if !CCOL_FORK_SAFETY_REQUIRED
  (void)idx; /* idx means something only for the torn_down slot bookkeeping
                below. The compiler removes that bookkeeping, with the rest of
                the atfork machinery of this module, when the builder turns
                fork safety off. */
#endif
#if CCOL_FORK_SAFETY_REQUIRED
  if (atomic_load(&pool->foreign_since_fork)) {
    /* This process inherited `pool` across a fork() call; see the field
     * comment of foreign_since_fork. At the instant of the fork(), a genuinely
     * live PARENT-side thread can be blocked on, or otherwise actively use,
     * each of its synchronization primitives: mu, not_empty, not_full and
     * idle_cv. To DESTROY one of them here is undefined behaviour at best. For
     * ccol_cond_var_destroy it HANGS FOREVER. pthread_cond_destroy in glibc
     * waits for an internal waiter reference count, __wrefs. Only a
     * parent-side worker thread that is still genuinely blocked on this exact
     * condvar in the parent can decrement that count, and no such thread
     * exists here. That hang is a separate failure from the SIGSEGV of a join
     * on a foreign thread, which the guards in
     * _ctpool_shutdown_drain_internal and _ctpool_shutdown_immediate_internal
     * above prevent.
     *
     * None of that OS-level state is this process to release. Its real owner
     * stays the PARENT. The parent tears it down normally, in its own time,
     * when it destroys its own copy of pool. This process frees only its own
     * private copy-on-write bookkeeping memory instead. It leaves every
     * synchronization primitive untouched. From the point of view of this
     * process that memory is deliberately leaked, and the parent still owns
     * and releases the real primitives.
     *
     * A lock and an unlock of pool->mu, unlike a destroy, is safe here.
     * _ctpool_atfork_prepare locks the mu of every live pool. It does that
     * before fork() can proceed. _ctpool_atfork_release_impl unlocks every one
     * of them again before fork() returns to application code. That function
     * also runs in the child, through _ctpool_atfork_child_release. When any
     * code in this process can reach this function, pool->mu is therefore
     * already clean and unlocked. That holds whoever held it in the parent at
     * the instant of the fork().
     *
     * Two steps below are needed for real reasons, and not only to match the
     * non-foreign path. (1) The pin wait, which comes before the lock and
     * holds no lock itself, is as necessary here as on that path. A resolve
     * from another thread in THIS process, for example a concurrent
     * ctpool_pending_count or ctpool_submit call that races this exact
     * destroy, is an ordinary in-process race. The pin mechanism exists to
     * protect against it, and it has nothing to do with the fork. To skip the
     * pin wait reopens, for this one code path, the exact
     * resolve-then-use-after-free race that the generation-tagged slot table
     * closes. (2) The lock itself is needed because steal_queue changes the
     * task list and needs it. Any task that still sits in pool->head or
     * pool->tail at this instant is ordinary, private copy-on-write heap
     * memory that this process CAN safely free. Unlike the OS-level thread,
     * mutex and condvar state above, no ownership by a worker thread is
     * involved. This code therefore discards such a task the same way
     * ctpool_shutdown_immediate already discards the queue of a live pool.
     * future_cancel wakes anyone in this process who is blocked in
     * ctpool_future_get on one of these tasks. Without it, such a caller hangs
     * forever and waits for a worker that never exists here. A PARENT-side
     * worker that is now gone can already have taken a task out of the queue
     * and been running it at the instant of the fork(). Such a task has no
     * reachable pointer left in this process at all. It lived only on the stack
     * of that worker, which does not exist. Nothing here can recover it. That
     * is a limit of a fork() with work in flight, and not something this
     * mechanism can close. */
    _ctpool_wait_for_pins(pool);
    ccol_mutex_lock(pool->mu);
    ctpool_task *discarded = steal_queue(pool);
    ccol_mutex_unlock(pool->mu);
    /* This frees each discarded task directly with _ccol_mem_free, and not
     * through task_free. At this point the pin count is already 0, and the
     * slot of this handle is already marked not-in-use; see the ordering
     * inside __ctpool_destroy. No submit call for this pool can ever happen
     * again in this process, for the rest of its life. To recycle one of
     * these nodes into task_free_list therefore only pays for the lock and
     * unlock round trip of task_free on every discarded task. The
     * unconditional drain_task_free_list call a few lines below undoes that
     * recycling, and nothing can ever observe it. */
    _ctpool_discard_tasks(pool, discarded, false);
    /* This frees whatever the task_free_list of this pool already held from
     * BEFORE the fork. Those are nodes that the library genuinely recycled
     * during ordinary operation before the fork. The loop above deliberately
     * goes around that list, and it does not feed into it. */
    drain_task_free_list(pool);

    _ccol_mem_free(pool->m_procs, pool->threads);
    _ctpool_teardown_clear_torn_down(idx);
    pool_free_self(pool);
    return;
  }
#endif /* CCOL_FORK_SAFETY_REQUIRED */

  _ctpool_shutdown_drain_internal(pool);

  _ctpool_wait_for_pins(pool);

  /* Every worker is joined and no pin is outstanding. No further submit or
   * task_free call for this pool is possible from here on. This genuinely
   * frees whatever task_free recycled into task_free_list over the life of
   * this pool, instead of a discard. Without it, those nodes leak, because
   * nothing ever hands them back to m_procs. */
  drain_task_free_list(pool);

  _ccol_mem_free(pool->m_procs, pool->threads);

#if CCOL_FORK_SAFETY_REQUIRED
  _ctpool_teardown_clear_torn_down(idx);
#endif
  ccol_mutex_destroy(pool->mu);
  ccol_cond_var_destroy(pool->not_empty);
  ccol_cond_var_destroy(pool->not_full);
  ccol_cond_var_destroy(pool->idle_cv);
  ccol_cond_var_destroy(pool->shutdown_cv);

  pool_free_self(pool);
}

void __ctpool_destroy(ctpool pool) {
  if (!pool) return;

  /* Resolve pool through the slot table, and mark the slot not-in-use in the
   * same critical section as the lookup. This is what makes a second destroy
   * call on the same handle value see a resolve failure. That is true for a
   * concurrent second call and for a later, sequential one. Without it, such a
   * call races the teardown of this call. See the file-level comment of the
   * slot table, and the comment of _ctpool_resolve, for the full design. A
   * stale or already-destroyed handle that reaches here is exactly the misuse
   * that this slot table catches. It is fatal, and not a silent use-after-free
   * or double-free. */
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  uint32_t idx = (uint32_t)(pool >> 32);
  uint32_t gen = (uint32_t)(pool & 0xFFFFFFFFu);
  ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  ctpool_slot_t *slot = NULL;
  cthread_pool *raw = NULL;
  /* A released table holds no live pool, so every handle is stale then. */
  if (ctpool_slot_table.slots &&
      idx < cvector_elem_count(ctpool_slot_table.slots)) {
    ctpool_slot_t *s =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    ccol_fatal_err(
        "ctpool_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a ctpool handle)");
  }
  /* A self-call; see the comment of ctpool_worker_key_bundle above. Unlike
   * ctpool_wait, ctpool_shutdown_drain and ctpool_shutdown_immediate, there is
   * no safe no-op here. A destroy of pool from inside one of its own workers,
   * while that worker still runs, frees pool->mu and the pool struct itself.
   * That worker is still on its way back through worker_thread_fn: it calls
   * task_free, and then it locks, decrements, broadcasts and unlocks against
   * the object that the destroy just freed. That is a real, deterministic
   * use-after-free, and not a rare race.
   *
   * __ctpool_destroy has no ccol_retval_t of its own to report this through.
   * Its documented contract is a live handle in, or a ccol_fatal_err on
   * misuse. This function therefore treats a self-call exactly like its own
   * stale-handle case immediately above: a loud, immediate ccol_fatal_err(),
   * rather than a silent skip and then undefined behaviour. This mirrors the
   * identical self-destroy guard of __ccol_event_loop_destroy in
   * cthreadcomm.c. */
  if (_ctpool_is_self_call(raw)) {
    ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    ccol_fatal_err(
        "ctpool_destroy: called from within a task (or its on_complete "
        "callback) running on this very pool's own worker thread; "
        "destroying it here would free the pool out from under that "
        "still-executing worker");
  }
  slot->in_use = false; /* blocks ALL later resolves for this handle from
                            this instant. This includes a second, concurrent
                            destroy attempt */
  /* The same step, under the same lock. From here the library grants no new
   * pin, which is what lets the count reach zero and stay there. */
  ccol_pintable_retire(&ctpool_pintable, idx);
#if CCOL_FORK_SAFETY_REQUIRED
  /* See the torn_down field comment of ctpool_slot_t. The worker threads of
   * raw are not gone yet. From now on they are only unreachable through this
   * handle. _ctpool_atfork_prepare must therefore keep a lock on raw->mu
   * across a fork(). It keeps that lock until _ctpool_teardown_raw clears this
   * flag, right before raw->mu becomes unsafe to touch. */
  slot->torn_down = true;
#endif
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);

  _ctpool_teardown_raw(raw, idx);

  /* Release the slot last, only after the library fully tears raw down and
   * frees it. This is what makes the generation bump of the slot, and the push
   * of the index back onto the free list, mark the handle as reusable. No
   * earlier step does that. Fetch the slot again by idx, and do not reuse
   * `slot`. A concurrent ccol_create_cthread_pool_mp call can run
   * _ctpool_handle_slot_acquire in between, and cvector_push_back there can
   * move the backing array of slots. Any pointer into that array from before
   * this second lock is then invalid. idx itself is stable. */
  ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  ctpool_slot_t *slot2 =
      (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* takes the generation of this slot past whatever
      value the handle of the pool that the library just freed carried. That
      stale handle can therefore never match the generation of a LATER acquire
      for this same index */
  _ctpool_free_index_release_locked(idx);
  /* The process-exit destructor already ran and found this pool live. The
     release that it could not do belongs to whoever frees the last slot, and
     that can be this call. */
  _ctpool_release_slot_table_if_deferred_locked();
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
/* Resolves h to the cthread_pool* behind it WITHOUT a pin. It does not touch
 * the pin index at all. It is a bare slot-table lookup. It is safe for tests,
 * because the test code that calls it runs synchronously and single-threaded,
 * with no concurrent destroy to race. Unlike _ctpool_resolve, there is no
 * matching _unpin call for a test to remember. Such a call is an easy thing to
 * forget. A forgotten unpin leaves a pin outstanding against that pool
 * forever, and that hangs every later ctpool_destroy call on it. This
 * function returns NULL under exactly the same conditions as
 * _ctpool_resolve. */
cthread_pool *_ctpool_resolve_for_tests(ctpool h) {
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_rw_lock_rdlock(ctpool_slot_table.rwlock);
  cthread_pool *raw = NULL;
  if (idx < cvector_elem_count(ctpool_slot_table.slots)) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return raw;
}

/* Reads how many slots the ctpool handle table holds now. That is the slots
 * that the table grew, plus the slots that the library freed but did not reuse
 * yet. It lets a test assert that a loop of creates and destroys reuses freed
 * slots. Without the reuse, the table grows without bound. */
/* The indices that sit on the free list now. Read this together with the
 * capacity above. A rollback that loses a slot shows up as a table that grew,
 * but only while the free list was empty. A test that measures the growth
 * alone therefore passes or fails according to how many handles earlier tests
 * held at once. Together, the two numbers describe the table without that
 * dependence. */
size_t _ctpool_free_index_count_for_tests(void) {
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  ccol_rw_lock_rdlock(ctpool_slot_table.rwlock);
  size_t n = cvector_elem_count(ctpool_slot_table.free_indices);
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return n;
}

/* Reads the shutdown bookkeeping of pool. A test can therefore wait until one
 * specific shutdown call takes ownership, before it starts a second call that
 * must arrive second. It takes the cthread_pool* that a test already resolved,
 * which matches the other test-only accessors in this file. */
void _ctpool_shutdown_state_for_tests(struct cthread_pool *pool, bool *started,
                                      bool *immediate, bool *complete) {
  ccol_mutex_lock(pool->mu);
  if (started) *started = pool->shutdown_started;
  if (immediate) *immediate = pool->shutdown_immediate;
  if (complete) *complete = pool->shutdown_complete;
  ccol_mutex_unlock(pool->mu);
}

/* Answers the self-call check for pool on the calling thread, which is what a
 * wait, a shutdown or a destroy of pool consults. A test reads it from inside
 * an on_complete callback before it makes a call that would block for ever if
 * the answer were wrong. */
bool _ctpool_is_self_call_for_tests(struct cthread_pool *pool) {
  return _ctpool_is_self_call(pool);
}

size_t _ctpool_slot_table_capacity_for_tests(void) {
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  ccol_rw_lock_rdlock(ctpool_slot_table.rwlock);
  size_t n = cvector_elem_count(ctpool_slot_table.slots);
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return n;
}

/* Reads the current size of the task-node free list of pool. A test can
 * therefore check the recycling optimization directly, which is the pop in
 * submit_internal, task_free and drain_task_free_list. Without this accessor, a
 * test can only infer the effects of that optimization from the call counts of
 * a custom allocator. It takes the cthread_pool* that _ctpool_resolve_for_tests
 * returns, and not a ctpool handle. That follows the convention of the
 * test-only accessors here, which build on that one resolve step and do not
 * repeat it. */
size_t _ctpool_task_free_list_size_for_tests(cthread_pool *pool) {
  ccol_mutex_lock(pool->mu);
  size_t n = pool->task_free_list_size;
  ccol_mutex_unlock(pool->mu);
  return n;
}

/* Reads the cap of the task-node free list of pool. At construction that cap
 * is num_threads * 4; see the field comment in struct cthread_pool. A test can
 * therefore assert the bound against the num_threads that it really built the
 * pool with, and it does not write the multiplier a second time. */
size_t _ctpool_task_free_list_cap_for_tests(cthread_pool *pool) {
  ccol_mutex_lock(pool->mu);
  size_t n = pool->task_free_list_cap;
  ccol_mutex_unlock(pool->mu);
  return n;
}

/* Reads how many pins are outstanding against the slot of pool. The count
 * lives in the pin index, and not in the pool itself, and this function reads
 * it with no lock held. A test can therefore see that a concurrent resolve and
 * pin genuinely happened, with no assumption about timing. The alternative is
 * an assumption that a fixed sleep was long enough. A truly slow or loaded
 * machine can starve that thread past any fixed bound, which silently reopens
 * the exact resolve-then-use race that such a test closes. */
size_t _ctpool_pending_resolve_count_for_tests(cthread_pool *pool) {
  return ccol_pintable_pins_for(&ctpool_pintable, pool->self_handle);
}

/* Waits on not_full of pool until the absolute deadline abs, measured on
 * whatever clock not_full was initialised with, and returns the status of
 * the wait. A test passes a CLOCK_MONOTONIC deadline and times the call, so
 * that it observes the clock of the condition variable from its behaviour. */
int _ctpool_timedwait_not_full_for_tests(cthread_pool *pool,
                                         const struct timespec *abs) {
  ccol_mutex_lock(pool->mu);
  int rc = 0;
  do {
    rc = ccol_cond_var_timedwait(pool->not_full, pool->mu, *abs);
  } while (rc == 0);
  ccol_mutex_unlock(pool->mu);
  return rc;
}

/* Reads the current reference count of a future, under the mutex of that
 * future. A test can therefore wait until a concurrent ctpool_future_get
 * genuinely takes its own waiter reference, with no assumption about timing.
 * The alternative is an assumption that a fixed delay was long enough for that
 * thread to get there. Such a test lets the task of the future complete while
 * a waiter is still on its way in. That is a use of a pointer whose last
 * reference is already gone, and no reference count can make it safe.
 */
int _ctpool_future_refcount_for_tests(ctpool_future *f) {
  if (!f) return -1;
  ccol_mutex_lock(f->mu);
  int refcount = f->refcount;
  ccol_mutex_unlock(f->mu);
  return refcount;
}

/* Test-only. It locks and unlocks the write side of the rwlock of
 * ctpool_slot_table directly, and it goes around every public API function. It
 * mirrors the identical ccol_event_loop_test_wrlock_reg_slot_for_tests and
 * _wrunlock pair of ccol_event_loop in src/cthreadcomm.c. See the doc comment
 * of that pair for the full rationale, and for the AB-BA deadlock that a hook
 * of this shape must not introduce.
 *
 * It lets a test hold the write side of this rwlock locked for a window of any
 * length that the test controls exactly, from a thread OTHER than the one
 * that calls fork(). prepare() then waits for that window to end before it
 * takes the write side itself, and the child exercises the reinitialize path
 * in the in_child branch of _ctpool_atfork_release_impl.
 *
 * This pair deliberately has no "resolve" step, unlike the pair in
 * ccol_event_loop. There is no per-instance handle to resolve here, only the
 * process-wide slot table itself. This function therefore returns void, and
 * the matching unlock call takes no argument. Nothing like the AB-BA hazard of
 * ccol_event_loop applies here, because neither call touches anything else
 * that a concurrent fork() can contend for. */
void ctpool_test_wrlock_slot_table_for_tests(void) {
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
}

void ctpool_test_wrunlock_slot_table_for_tests(void) {
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
}
#endif

/* Frees the bookkeeping arrays of the slot table at process exit. The
 * leak-kind report of make memtest therefore does not flag them as still
 * reachable. This mirrors the _cleanup_*_slot_table function of
 * ccol_event_loop and of clru_cache exactly. See the comments of those
 * functions for the full rationale. That includes the reason this is sound
 * only when the application itself destroyed every ctpool that it created
 * before process exit. This test suite already satisfies that precondition for
 * a clean make memtest.
 *
 * The ccol_call_once here is mandatory. An __attribute__((destructor))
 * function runs for the whole shared object, whatever parts of it the process
 * used. A process can link this library and never create one ctpool. Without
 * the ccol_call_once, such a process locks a mutex here that nothing ever
 * initialized with pthread_mutex_init. */
/* Answers whether any slot still names a pool. The caller holds the write
 * lock.
 *
 * This scan reads slot->ptr, and not slot->in_use. A destroy clears in_use as
 * its first step, so that it rejects a second destroy or a new resolve as
 * early as possible. The rest of the teardown runs after that: it joins the
 * threads, drains the pins, and does the final locked release of the index. A
 * scan that trusts in_use alone frees this table under a destroy that is still
 * in that window. The last step of that destroy then indexes the freed
 * table. The library writes ptr only after it fully acquires a slot, and it
 * clears ptr only in that final locked step. ptr is therefore true for exactly
 * as long as the table must not be released, and it depends on no build-time
 * switch. */
static bool _ctpool_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(ctpool_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* Releases the bookkeeping of the table and the pin index. The caller holds
 * the write lock and already established that no slot is live.
 *
 * The library deliberately never releases the slot storage of the pin index
 * while the process runs, because a resolve indexes it with no lock held. A
 * leak checker that treats still-reachable memory as an error therefore
 * reports that storage at exit, unless this function releases it. This
 * function sets each vector to NULL as it goes. That is what makes a later
 * call answer "already released", instead of an index into a freed vector.
 * This function does not destroy the rwlock, because it can run from an
 * ordinary destroy that still holds that lock. */
/* The check and the release both sit behind one out-of-line call. The destroy
 * path that must make that call therefore keeps the code shape that it has
 * without any of this. Cold code in a hot object file is not free. Inline, the
 * same handful of instructions measurably slows the push path of an unrelated
 * container, because it shifts what the linker lays out around that path. The
 * instruction count does not change. */
static __attribute__((noinline)) void
_ctpool_release_slot_table_if_deferred_locked(void) {
  if (ctpool_slot_table.release_deferred && !_ctpool_any_slot_live_locked()) {
    _ctpool_release_slot_table_locked();
  }
}

/* The release also retires the worker key, on either path: the destructor
 * when no pool was left, or the destroy of the last pool when the destructor
 * had to defer. No pool is left, so no worker of this module runs and nothing
 * reads the key again. A thread that outlives a dlclose() then never meets a
 * key of this module, and the next load of the library creates a fresh one.
 * A pool that some code creates after this point is refused: its creation
 * finds live false, or its slot acquire finds the released table, and a
 * worker sets the key only while live is true. */
static void _ctpool_release_slot_table_locked(void) {
  cvector_destroy(ctpool_slot_table.slots);
  cvector_destroy(ctpool_slot_table.free_indices);
  ccol_pintable_dispose(&ctpool_pintable);
  ctpool_slot_table.release_deferred = false;
  if (atomic_exchange(&ctpool_worker_key_bundle.live, false))
    ccol_thread_ls_key_delete(ctpool_worker_key_bundle.key);
}

__attribute__((destructor)) static void _cleanup_ctpool_slot_table(void) {
  ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  /* There is nothing to do, and nothing safe to touch. The library sets each
     vector to NULL as it destroys that vector. A second run of this function
     therefore answers here, instead of an index into a freed vector. */
  if (!ctpool_slot_table.slots) return;
  ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  /* This runs only when nothing is left that can still resolve a handle. The
   * order of the destructor of one translation unit against the destructor of
   * another is not for this library to decide. Without this check, a later
   * destructor that still holds a live handle finds the table and the pin
   * index freed under it. When that happens, the release goes to whichever
   * destroy frees the last slot, and nothing skips it. An application that
   * does destroy its pools therefore leaves nothing behind, whatever the order
   * of the destructors. */
  if (_ctpool_any_slot_live_locked()) {
    ctpool_slot_table.release_deferred = true;
    ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    return;
  }
  _ctpool_release_slot_table_locked();
  ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  /* This code deliberately does not destroy the rwlock itself. The rwlock has
     static storage duration, so it holds nothing that a leak checker reports.
     The destructor also cannot own the lifetime of the rwlock. The deferred
     branch above returns with the table still live, and the release that
     happens later runs while it holds this very lock. There is therefore no
     path on which every user is provably finished with it. A destroy here
     leaves the entry points of the other branch with a read lock on a
     destroyed object. A stale-handle call that is documented to fail cleanly
     through ccol_fatal_err then becomes undefined behaviour instead. The order
     of destructors across translation units is also not for this library to
     decide. */
}

#ifdef RUNNING_UNIT_TESTS
/* Runs the process-exit cleanup of the slot table now. A test calls it in a
 * forked child, with a pool still live, to reach the deferred release. */
void _ctpool_run_exit_cleanup_for_tests(void) { _cleanup_ctpool_slot_table(); }

/* True while the worker key exists. */
bool _ctpool_worker_key_live_for_tests(void) {
  return atomic_load(&ctpool_worker_key_bundle.live);
}
#endif /* RUNNING_UNIT_TESTS */
