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

/* Initialises cond so that every absolute deadline handed to
 * _ccol_cond_var_timedwait on it is measured against CLOCK_MONOTONIC, not the
 * default CLOCK_REALTIME. Returns 0 on success, or the first non-zero status
 * that the threading library reports.
 *
 * not_full is the one condition variable of a pool that a timed wait blocks
 * on, and make_abs_deadline computes its deadline on CLOCK_MONOTONIC as well,
 * so the two can't disagree about the clock. CLOCK_REALTIME can jump back at
 * any moment (an administrator, an NTP step, a virtual machine that resumes)
 * and a deadline on that clock then moves further into the future, so a timed
 * submit with a 100 ms budget would block for the whole backward step.
 * Nothing can step CLOCK_MONOTONIC. cthreadcomm.c sets up its own timed
 * condition variables the same way. */
static int _ctpool_init_monotonic_cond_var(_ccol_cond_var_t *cond) {
  _ccol_cond_var_attr_t attr;
  int rv = _ccol_cond_var_attr_init(attr);
  if (rv != 0) return rv;
  rv = _ccol_cond_var_attr_setclock(attr, CLOCK_MONOTONIC);
  if (rv == 0) rv = _ccol_cond_var_init_ca(*cond, attr);
  _ccol_cond_var_attr_destroy(attr);
  return rv;
}

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/* ctpool is an opaque value handle: the top 32 bits are the slot index and the
 * bottom 32 bits are the generation (see the doc comment on the typedef in
 * include/cthreadpool.h). We resolve the handle through this table before we
 * touch the struct cthread_pool* behind it, and that's what lets
 * __ctpool_destroy catch both kinds of double-destroy and report them with
 * _ccol_fatal_err instead of a use-after-free or a double-free. One kind is
 * concurrent, a destroy racing another destroy on the same still-live handle,
 * and the other is sequential, a stale handle from an earlier destroy that
 * completed. A slot is marked not-in-use the moment it's released, and its
 * generation goes up on every reuse, so a stale handle can never alias a
 * later, unrelated pool at the same slot index. The table mirrors
 * chttpcli_slot_table, chttpsvr_slot_table, ccol_event_loop_slot_table and
 * clrucache_slot_table exactly; the copy of this comment in src/chttpclient.c
 * gives the design rationale.
 *
 * The table's lock is a read-write lock rather than a plain mutex. A submit,
 * a wait or any other call on a pool never takes it, because _ctpool_resolve
 * pins the handle through the lock-free pin index of cpintable.h. Only
 * _ctpool_handle_slot_acquire and __ctpool_destroy take it, each once in a
 * pool's whole life, plus the fork handlers and the tests' white-box helpers.
 * It mirrors the identical reg_slot_rwlock of ccol_event_loop and has the same
 * fork-safety subtlety: the doc comment of _ctpool_atfork_prepare explains the
 * hazard of a write lock that tracks the thread ID and how we handle it. */
typedef struct {
  cthread_pool *ptr;   /* NULL when slot is free */
  uint32_t generation; /* fresh on every acquire. It only rises for each slot
                           index: it starts at 0 before the first use and
                           becomes 1 on the first acquire */
  bool in_use;
#if CCOL_FORK_SAFETY_REQUIRED
  /* True from the exact instant that __ctpool_destroy clears in_use until
   * _ctpool_teardown_raw is about to make ptr->mu unsafe to touch, which it
   * does by destroying ptr->mu (or, on the foreign_since_fork path, by freeing
   * the struct that holds ptr->mu). The doc comment of _ctpool_atfork_prepare
   * describes the hang that this flag closes.
   *
   * in_use alone doesn't tell us that ptr->mu is both live and worth
   * locking, because a pool's worker threads aren't gone at the instant in_use
   * goes false. They are gone only once _ctpool_shutdown_drain_internal has
   * joined every one of them, and that step runs entirely AFTER in_use is
   * cleared, so that a concurrent resolve or a second destroy gets rejected as
   * early as possible. Without torn_down, a fork() inside that join window
   * inherits ptr->mu locked by one of those very real, not yet joined worker
   * threads, while in_use (already false) tells _ctpool_atfork_prepare that
   * there's nothing here worth protecting.
   *
   * torn_down is never true while in_use is true; the two are set at different
   * points, never together. ptr is not NULL and is safe to dereference for as
   * long as torn_down is true. torn_down is cleared under the write side of
   * this same ctpool_slot_table.rwlock, strictly before the struct that ptr
   * points to becomes unsafe to touch. */
  bool torn_down;
#endif
} ctpool_slot_t;

static struct {
  _ccol_rw_lock_t rwlock;
  _ccol_once_flag_t once;
  cvec slots;        /* cvec of ctpool_slot_t that grows only with push_back;
                         once allocated, an index is permanent */
  cvec free_indices; /* cvec of uint32_t. A LIFO free list, with O(1) reuse */
  /* How many slot indices couldn't go back onto free_indices because that
     push failed to allocate. Such a slot is fully released (ptr is NULL and
     in_use is false) but nothing names it, so without this counter it would
     stay unreachable for the rest of the process and every later create would
     grow the table by one more slot. An acquire that finds free_indices empty
     while this counter is above zero gets one slot back by scanning slots
     instead. The scan is unambiguous because it runs only when the free list
     is empty: a released slot is either on that list or lost, never both.
     This field is read and written only under the write lock. */
  size_t lost_indices;
  /* Set when the process-exit destructor finds a pool still live and leaves
     this table alone; whichever destroy releases the last slot after that
     does the release the destructor couldn't do. Without this flag a pool
     that outlives this translation unit's destructor leaves the table and the
     pin index allocated for the rest of the process, and a leak checker that
     counts still-reachable memory as an error reports it. The library's lazily
     created HTTP client owns such a pool, and its reaper runs from a
     destructor that the linker places earlier. This field is read and written
     only under the write lock. */
  bool release_deferred;
} ctpool_slot_table = {0};

#if CCOL_FORK_SAFETY_REQUIRED
/* Forward declarations. The bodies come further down, after struct
 * cthread_pool itself is declared, because they dereference the mu of a live
 * pool. _ctpool_slot_table_init_globals below registers them; it is the first
 * thing in this file that runs once, lazily, when the module is first used.
 * src/cthreadcomm.c does the same, where ccol_event_loop also declares first
 * and defines after the struct.
 *
 * With CCOL_FORK_SAFETY_REQUIRED at 0 the compiler drops this whole fork()
 * safety mechanism: these three handlers, their _ccol_at_fork() registration
 * below, the foreign_since_fork field of struct cthread_pool and every site
 * that reads it. See the doc comment of that macro in common.h. */
static void _ctpool_atfork_prepare(void);
static void _ctpool_atfork_release(void);
static void _ctpool_atfork_child_release(void);
#endif

static void _ctpool_slot_table_init_globals(void) {
  if (_ccol_rw_lock_init(ctpool_slot_table.rwlock) != 0)
    _ccol_fatal_err("ctpool slot table: failed to initialize rwlock");
  ctpool_slot_table.slots = cvector_create(sizeof(ctpool_slot_t), NULL);
  if (!ctpool_slot_table.slots)
    _ccol_fatal_err("ctpool slot table: failed to allocate slots vector");
  ctpool_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!ctpool_slot_table.free_indices)
    _ccol_fatal_err("ctpool slot table: failed to allocate free-index vector");
#if CCOL_FORK_SAFETY_REQUIRED
  /* fork() copies only the thread that calls it. The doc comment of
   * _ctpool_atfork_prepare describes the hazard this closes for BOTH parent()
   * and child(), a child inheriting a mutex that is still locked. The doc
   * comment of _ctpool_atfork_child_release describes a second hazard that
   * only the child has, and that neither parent() nor a shared release
   * function can handle: joining worker threads that exist only in the
   * parent. */
  _ccol_at_fork(_ctpool_atfork_prepare, _ctpool_atfork_release,
                _ctpool_atfork_child_release);
#endif
}

#if defined(_CCOL_ATFORK_AT_LOAD)
/* See _CCOL_ATFORK_AT_LOAD in common.h. */
__attribute__((constructor)) static void _ctpool_atfork_at_load(void) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
}
#endif

/* Not static, and reachable from other .c files of the library on purpose
 * (chttpserver.c, for example). Such a file must make sure this module
 * registers its own _ccol_at_fork() triple BEFORE that file registers its
 * own. pthread_atfork runs the prepare handlers in LIFO order, so with that
 * ordering the CALLER's prepare handler runs FIRST at every later fork(),
 * before _ctpool_atfork_prepare (this module's) can lock
 * ctpool_slot_table.rwlock or the mu of any live pool.
 *
 * It isn't declared in cthreadpool.h (internal/catfork.h declares it) and
 * isn't part of the public API. It's a narrow escape hatch, kept on purpose,
 * for a caller that has read this exact ordering requirement and needs to
 * meet it. The call site in
 * chttpserver.c explains the reasoning and the lock-order inversion that
 * this ordering prevents; ThreadSanitizer reports that cycle whenever
 * nothing forces the ordering. A caller that never uses this function isn't
 * affected at all, because this module's lazy registration (guarded by
 * _ccol_call_once) then happens at the first ctpool create, exactly as it
 * would without such a caller. */
void _ctpool_ensure_atfork_registered_before_caller(void) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
}

/*
 * One unit of work in the queue. A future task sets `future` and leaves
 * `on_complete` NULL, and a callback task does it the other way round.
 * `fn_future` shares a union with `fn`, so both pointers have the same width
 * and the call site needs no cast.
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
 * A future is always allocated on the heap with plain malloc and freed with
 * plain free, so its lifetime doesn't depend on the pool's custom allocator
 * at all. A submit creates two references, one for the caller and one for the
 * queued task, and each ctpool_future_get call in flight holds one more while
 * it runs. The struct is freed once the last reference goes.
 */
struct ctpool_future {
  _ccol_mutex_t mu;
  _ccol_cond_var_t cv;
  void *result;
  bool done;
  bool cancelled;
  int refcount;
};

/* The hot half of the slot table. It maps a handle to a pointer and holds the
 * pin that keeps a pool alive for the length of a call. It lives apart from
 * the table because a resolve runs on every public call and must not write
 * anything another thread reads, while slot recycling is cold and stays under
 * the rwlock. */
static ccol_pintable ctpool_pintable;

/* One worker of a pool. See the threads field of struct cthread_pool. */
typedef struct ctpool_worker_slot {
  _ccol_thread_id_t id;
  atomic_bool exited;
} ctpool_worker_slot;

struct cthread_pool {
  /* Task queue (intrusive singly-linked list) */
  ctpool_task *head;
  ctpool_task *tail;
  /* queue_size, shutdown_drain and shutdown_immediate are written under mu,
   * always with a relaxed __atomic store, because the refusal hint of a future
   * try submit (submit_future_refusal_hint) reads them without the lock. A
   * reader that holds mu reads them plainly. queue_cap never changes once the
   * pool is published. */
  size_t queue_size;
  size_t queue_cap; /* 0 = unbounded */

  /* Recycled ctpool_task nodes. They form their own intrusive singly-linked
   * list that reuses the `next` field of the same struct. The list saves one
   * malloc and free round trip through m_procs per submission, and for a
   * workload that submits lots of short-lived tasks that round trip is a real
   * source of allocator pressure. mu protects this list, like every other data
   * structure the pool owns.
   *
   * The list keeps every node that the pool's recent traffic needed, not a
   * fixed handful. A producer running ahead of the workers builds a queue of
   * many tasks and every one of them is a node. If the list kept only a few
   * back, every further node would go through the allocator again, one malloc
   * on the submit and one free on the completion for each task, for as long as
   * the queue stays deep, and each of those frees is cross-thread. Since the
   * nodes are kept, a burst of any depth costs no allocation at all once the
   * pool has seen that depth.
   *
   * Two things bound what the list keeps. task_free_list_cap is a hard
   * ceiling, max(num_threads * 4, CTPOOL_TASK_FREE_LIST_CEILING), set once at
   * construction. And the list decays while the traffic doesn't need it: at
   * every CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS-th point where the pool is
   * idle, with no task queued and none running, it gives back half of what it
   * holds above num_threads * 4, unless it ran empty since the previous such
   * point (see task_free_list_ran_dry and _ctpool_idle_unlock_and_trim). So the
   * list holds what the recent traffic needed, never more than the ceiling,
   * and never less than num_threads * 4 while it has that many.
   *
   * None of that bookkeeping runs on the ordinary path: a pop that leaves the
   * list non-empty, a push and an enqueue touch none of its fields. Every
   * submitter and every worker takes mu for each task, and a store inside
   * those critical sections to a line they don't otherwise touch would bounce
   * that line between cores on every task and make the critical section that
   * all of them wait on longer.
   *
   * Nodes on this list are only recycled and never handed back to m_procs, so
   * the pool's teardown must really free them with drain_task_free_list;
   * just discarding them isn't enough. */
  ctpool_task *task_free_list;
  size_t task_free_list_size;
  size_t task_free_list_cap;

  /* Synchronisation */
  _ccol_mutex_t mu;
  _ccol_cond_var_t not_empty; /* a worker waits here when idle            */
  _ccol_cond_var_t not_full;  /* a submitter waits here on a full queue   */
  _ccol_cond_var_t idle_cv;   /* ctpool_wait waits here                   */

  /* Live counters */
  size_t active_count; /* tasks that the workers run now */

  /* Shutdown state */
  bool shutdown_drain;     /* the pool takes no new task. drain, then stop */
  bool shutdown_immediate; /* stop after the current tasks. discard queue  */
  bool shutdown_started;   /* one of the two shutdowns started             */
  /* Whichever call does the real shutdown sets this, under mu, once it has
   * joined every worker thread, and every other shutdown call on this pool
   * waits on shutdown_cv until it's true. That's what makes the documented
   * "blocks until ..." guarantee of ctpool_shutdown_drain and
   * ctpool_shutdown_immediate hold for the second caller and every later one,
   * not just the first. shutdown_started is set before a single worker is
   * woken, and long before one is joined, so returning on shutdown_started
   * alone would hand application code a pool whose workers are all still
   * running. The flag sits in padding the three flags above already had, so
   * it costs the struct nothing. */
  bool shutdown_complete;

  /* Worker threads. The worker sets exited itself, as its last write to the
   * pool, so a joined worker is always marked. _ctpool_is_self_call skips a
   * marked slot because glibc gives the descriptor (and so the ID) of a joined
   * thread to the next thread that starts. */
  ctpool_worker_slot *threads;
  size_t num_threads;

  ccol_memmgmt_procs_t *m_procs;

  /* The handle of this pool. An unpin uses it to find the slot holding its
   * pin, so the caller doesn't have to carry one. It's written once, before
   * the handle is published, and never again. */
  ctpool self_handle;

#if CCOL_FORK_SAFETY_REQUIRED
  /* Only the CHILD-side fork handler of this process
   * (_ctpool_atfork_child_release) sets this to true, and it does that for
   * every pool still marked in_use at the moment of the fork(). fork() copies
   * only the calling thread, so as far as this process is concerned every
   * worker thread of the pool is just inert copy-on-write memory, and no
   * execution context for any of them ever existed here.
   *
   * Once the flag is true, _ctpool_shutdown_drain_internal and
   * _ctpool_shutdown_immediate_internal must never call _ccol_thread_join on
   * threads[i]. That's undefined behaviour, since this process never created
   * the target thread and can never join it. In a process holding a live
   * ctpool with several workers (a ccol_event_loop with
   * num_reactor_threads > 1, for example), a fork() followed by a destroy or
   * a shutdown of the inherited pool in the child SIGSEGVs inside
   * __pthread_clockjoin_ex in glibc.
   *
   * The flag is never true for a pool that ctpool_create_mp really created in
   * this process. With CCOL_FORK_SAFETY_REQUIRED at 0 the compiler drops it
   * (see the doc comment of that macro in common.h), and every site that
   * reads the field then always takes the path it takes for false: a pool
   * can't legitimately be "foreign" in a build that has no fork() machinery
   * to mark it so. */
  _Atomic bool foreign_since_fork;
#endif

  /* Trim state of task_free_list (see the comment on that list); mu guards
   * both fields. task_free_list_ran_dry is set when a pop takes the last node
   * or finds none, which is the slow path of a submit anyway, and the idle
   * point clears it. idle_points_since_trim counts idle points. Both sit after
   * every field that the submit and dequeue paths read, since only those rare
   * events touch them. */
  bool task_free_list_ran_dry;
  uint32_t idle_points_since_trim;

  /* Broadcast under mu at the exact moment shutdown_complete goes true. It's
   * declared last on purpose instead of next to the other condition
   * variables: nothing on the submit or dequeue path reads it, and putting it
   * among the others would push active_count and the queue bookkeeping, which
   * those paths do read, onto other cache lines. */
  _ccol_cond_var_t shutdown_cv;
};

/* Every lock of a pool's mutex goes through this macro. A test build also
 * counts the locks each thread takes, so a test can assert how many critical
 * sections one call costs the calling thread; any other build compiles it to
 * the plain lock. */
#ifdef RUNNING_UNIT_TESTS
static __thread unsigned long _ctpool_mu_locks_taken;
#define _ctpool_lock_mu(p) \
  (_ctpool_mu_locks_taken++, (void)_ccol_mutex_lock((p)->mu))
#else
#define _ctpool_lock_mu(p) _ccol_mutex_lock((p)->mu)
#endif

/* ========================================================================== */
/*                         FORK SAFETY (pthread_atfork)                       */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/* fork() copies only the thread that calls it, so a lock that some OTHER
 * thread holds at that instant reaches the child permanently locked, and the
 * child has no thread left that could ever unlock it. Two kinds of lock
 * matter here. One is ctpool_slot_table.rwlock, process-wide, which every
 * ctpool_create_mp and __ctpool_destroy call takes. The other is the mu of
 * each pool that is still live, which every submit, dequeue, shutdown and
 * wait call takes, and so does a worker thread when it picks up or finishes
 * a task.
 *
 * So prepare() below takes both before fork() can go on, and fork() completes
 * only at a moment when no thread holds either of them; parent() and child()
 * then release them through the same function. Every PLAIN mutex in this
 * module, each pool's mu included, is of the default ("normal") pthread type,
 * which on Linux glibc does no owner or thread ID tracking, so a plain
 * pthread_mutex_unlock is well defined even when it's called by a thread
 * other than the one that locked it. For anything the forking thread didn't
 * hold itself, that other thread doesn't exist in the child at all.
 *
 * ctpool_slot_table.rwlock is the one exception, because it's a read-write
 * lock (the doc comment of ctpool_slot_t says why). prepare() takes its write
 * side on the thread that calls fork(), once every other holder has let it
 * go. A glibc rwlock unlock tells a writer unlock from a reader unlock by
 * comparing the writer thread ID it recorded with the caller's thread ID,
 * and fork() gives the child's one thread a new thread ID. In the child a
 * plain _ccol_rw_lock_unlock therefore returns 0 and releases nothing, and
 * every later ctpool_create_mp or ctpool_destroy there would block for ever.
 * That's why the in_child branch of _ctpool_atfork_release_impl
 * reinitializes the rwlock instead of unlocking it, exactly as the atfork
 * handling of clog_slot_table.rwlock in clogger.c and of the two rwlocks of
 * ccol_event_loop does.
 *
 * This mirrors the atfork handling of ccol_event_loop in src/cthreadcomm.c
 * exactly, and the comment there gives the full account of the two hangs
 * that this shape of handler prevents. In the first a fresh create call
 * inherits its own process-wide slot-table mutex already locked, and in the
 * second an ordinary call on one object inherits the lock of a still-live
 * object already locked.
 *
 * This module keeps its own copy rather than relying on the one in
 * ccol_event_loop. The dispatch_pool that ccol_event_loop creates whenever a
 * caller sets num_reactor_threads > 1 is exactly such a pool, and
 * ccol_event_loop has no way to reach this module's opaque ctpool internals
 * from outside. The registration here covers every ctpool this module ever
 * created, not just the ones of ccol_event_loop, and so it covers every
 * caller of this module.
 *
 * prepare() walks every slot with in_use == true, which is exactly the
 * condition _ctpool_resolve itself trusts as the only sign that slot->ptr is
 * safe to dereference for an ordinary resolve. It ALSO walks every slot with
 * torn_down == true. __ctpool_destroy clears in_use, under the write side of
 * this same ctpool_slot_table.rwlock, BEFORE it starts its own teardown, and
 * that teardown can be slow because it joins every worker thread and frees
 * the task queue. A pool's worker threads aren't gone the instant in_use goes
 * false but only once that join phase completes, and torn_down stays true for
 * exactly that window (see the field comment of ctpool_slot_t). So a fork()
 * landing while a not yet joined worker holds ptr->mu is caught here, just
 * like the mu of an ordinary live pool. ptr is freed, and slot->ptr finally
 * cleared, well after in_use and torn_down have both gone false. By this
 * file's convention a pool past that point is off-limits for any purpose,
 * fork-related or not, so the mechanism opens no new gap there.
 *
 * The mechanism does NOT cover the mu of an individual ctpool_future, on
 * purpose. ctpool_slot_table reaches every pool, since it's the complete,
 * walkable registry of every live pool in this module. A future has no such
 * registry once it leaves the task queue: ctpool_submit_future gives the
 * caller the only reference to a future that outlives the pool's task list.
 * To find "every live future" at fork time this module would need a new
 * global registry with a lock of its own, which is well outside the scope of
 * this mechanism. ccol_event_loop leaves out its own per-entry dispatch_lock
 * for the same reason; its comment makes the same argument, that only a walk
 * of a structure this mechanism doesn't hold the right lock for could find
 * one.
 *
 * worker_thread_fn, future_deref, future_cancel, ctpool_future_fulfill and
 * ctpool_future_get each hold a future's mu for a short window, so a fork()
 * inside one of those windows is a real gap. It's a narrow one, and the
 * locking discipline of this module keeps the odds low: every future->mu
 * critical section is a handful of instructions, nothing holds it across a
 * callback, and the condition variable itself releases it for the one wait
 * that exists. We leave the gap open rather than quietly claim it's
 * covered. */
static void _ctpool_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(_ccol_atfork_module_cthreadpool);
#endif
  _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);

  /* A table released at unload holds no pool. */
  size_t n =
      ctpool_slot_table.slots ? cvector_elem_count(ctpool_slot_table.slots) : 0;
  for (size_t i = 0; i < n; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    _ccol_mutex_lock(slot->ptr->mu);
  }
}

/* parent() and child() share this function. The doc comment of
 * _ctpool_atfork_prepare explains why a plain unlock is correct for this
 * module's PLAIN mutexes in both branches, and why ctpool_slot_table.rwlock
 * alone needs a reinitialize in the child, which the end of this function
 * does.
 *
 * Walking the same structure prepare() walked and releasing every lock the
 * same way is safe, because nothing can change the slot table or the state
 * of a live pool in between: this function still holds every lock such a
 * change would need.
 *
 * With is_child set it also marks every still-live pool foreign_since_fork
 * and drops every pin outstanding against it. The doc comment of
 * _ctpool_atfork_child_release says why the child needs both steps, and the
 * comment on foreign_since_fork in struct cthread_pool describes the SIGSEGV
 * this closes. The parent needs neither, because the fork took nothing away
 * from it: each of its own threads, and each pin any of them held, is
 * exactly as it was right before the fork() call. */
static void _ctpool_atfork_release_impl(bool is_child) {
  size_t n =
      ctpool_slot_table.slots ? cvector_elem_count(ctpool_slot_table.slots) : 0;
  for (size_t i = 0; i < n; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    cthread_pool *pool = slot->ptr;

    /* A torn_down slot is already not in_use, and its own destroy is already
     * in progress in THIS process. Nothing can resolve it again since in_use
     * is false, so unlike a pool that is really still live it has no way to
     * reach the foreign_since_fork branch of _ctpool_teardown_raw a second
     * time in the child. The thread destroying it doesn't exist there (unless
     * it's the thread that called fork()), and the pool's memory then just
     * stays unreachable, harmlessly, for the rest of the child's life. All
     * that's needed to close the hang this mechanism is about is an unlock of
     * its mu, and the code below does that in both branches. */
    if (slot->in_use && is_child) {
      atomic_store(&pool->foreign_since_fork, true);
      /* A parent-side thread that doesn't exist in this process can be in the
       * middle of a resolve, inside a pinned _ctpool_resolve call, at the
       * instant of the fork(). From this process's point of view that pin
       * then stays outstanding forever, because nothing here can ever run the
       * matching _ctpool_resolve_unpin that the vanished thread would make.
       * The wait loop of _ctpool_teardown_raw blocks until the pin count
       * reaches 0 before it does anything else, so without a reset here the
       * destroy in the child would hang forever. It's the same class of hang
       * as an inherited locked mutex; see the doc comment of
       * _ctpool_atfork_prepare above.
       *
       * The reset is safe in the very common case where no thread in this
       * process is already resolving this exact handle across this exact
       * fork() call. One narrow case stays open, where the thread calling
       * fork() has its OWN resolve in flight across its own fork() call. That
       * is a limit of calling fork() while holding a pin, not a defect of this
       * reset. */
      ccol_pintable_reset_for(&ctpool_pintable, pool->self_handle);

      /* A live pool's worker threads spend most of their lives blocked in
       * _ccol_cond_var_wait(pool->not_empty, pool->mu) (see the main loop of
       * worker_thread_fn), and unlike a plain _ccol_mutex_lock that call
       * releases pool->mu while it waits. So the lock _ctpool_atfork_prepare
       * takes on pool->mu proves nothing about some OTHER thread the child
       * doesn't have: at the exact instant of the fork() such a thread can be
       * inside pthread_cond_wait's internal waiter bookkeeping. glibc's condvar
       * keeps track of its registered waiters in state private to the condvar
       * (an internal waiter reference count and generation or group counters,
       * for example), and that state is completely independent of whatever
       * external mutex the condvar is paired with.
       *
       * fork() copies the raw memory of that bookkeeping as it is while the
       * thread that owned it is gone. A LATER _ccol_cond_var_signal or
       * _broadcast on the same copied condvar in the child can then hang on
       * that stale internal state, which nothing can ever resolve. The child
       * ends up stuck inside pthread_cond_signal itself, not in pool->mu at
       * all, and it gets there from the ordinary
       * _ccol_cond_var_signal(pool->not_empty) call that every submit makes
       * after putting a task in the queue.
       *
       * A plain "normal" pthread mutex does no owner tracking, so a bare
       * unlock from a different thread clears it fully and correctly (see this
       * function's header comment), but a condition variable has no such safe
       * "just unlock" equivalent for internal waiter state that may be stale.
       * So we REINITIALIZE every condvar of this pool, unconditionally, on the
       * child's one surviving thread, before any application code in this
       * process can reach them. The slot-table rwlock at the end of this
       * function is reinitialized in the child for a different reason; see the
       * comment there.
       *
       * This is safe because the handler runs single-threaded and
       * synchronously, before fork() returns to application code, so nothing
       * can touch these condvars concurrently yet. A freshly initialized
       * condvar with zero real waiters is the right state for an inherited
       * pool: its real worker threads are the only threads that could ever be
       * genuine waiters, and all of them are gone in this process, whatever
       * the fork() caught them doing. */
      if (_ccol_cond_var_init(pool->not_empty) != 0)
        _ccol_fatal_err("ctpool atfork release: failed to reinit not_empty");
      /* Same clock as the ordinary init, so a timed submit in the child keeps
       * its CLOCK_MONOTONIC deadline. */
      if (_ctpool_init_monotonic_cond_var(&pool->not_full) != 0)
        _ccol_fatal_err("ctpool atfork release: failed to reinit not_full");
      if (_ccol_cond_var_init(pool->idle_cv) != 0)
        _ccol_fatal_err("ctpool atfork release: failed to reinit idle_cv");
      /* Same reasoning as for the three condvars above, and the same class of
       * waiter needs it: a thread blocked in ctpool_shutdown_drain or
       * ctpool_shutdown_immediate, waiting out a shutdown that another thread
       * runs, sits on this condvar. */
      if (_ccol_cond_var_init(pool->shutdown_cv) != 0)
        _ccol_fatal_err("ctpool atfork release: failed to reinit shutdown_cv");
    }

    _ccol_mutex_unlock(pool->mu);
  }

  /* prepare() above took the write side of ctpool_slot_table.rwlock on this
   * same thread, the one calling fork(). In the parent a plain
   * _ccol_rw_lock_unlock from here releases it.
   *
   * In the child it doesn't. A glibc rwlock unlock tells a writer unlock from
   * a reader unlock by comparing the writer thread ID it recorded with the
   * caller's, and fork() gives the child's one thread a new thread ID, so a
   * plain unlock returns 0, releases nothing, and every later
   * ctpool_create_mp or ctpool_destroy in this child would block for ever.
   * The child reinitializes the rwlock instead, as _clog_atfork_release in
   * clogger.c and _cthreadcomm_atfork_release_impl in ccol_event_loop do.
   * That's safe because the child has exactly one thread at this point, so
   * nothing can be waiting on the rwlock. */
  if (is_child) {
    if (_ccol_rw_lock_reinit_in_child(ctpool_slot_table.rwlock) != 0)
      _ccol_fatal_err(
          "ctpool atfork release: failed to reinit slot table rwlock");
  } else {
    _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  }
}

static void _ctpool_atfork_release(void) { _ctpool_atfork_release_impl(false); }

/* The child-side counterpart of _ctpool_atfork_release. It releases the same
 * locks (see the comment of _ctpool_atfork_release_impl), and on top of that
 * it marks the worker threads of every inherited pool as gone for good as far
 * as this process is concerned. It must run before any application code in
 * this process can reach the shutdown or destroy path of one of these pools,
 * and it does, because pthread_atfork's child handler runs synchronously as
 * part of fork() returning, strictly before fork()'s return value reaches
 * the code that called it. */
static void _ctpool_atfork_child_release(void) {
  _ctpool_atfork_release_impl(true);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* Defined with the process-exit teardown below. They're declared here
   because the final locked section of __ctpool_destroy does the release that
   the destructor deferred. */
static bool _ctpool_any_slot_live_locked(void);
static void _ctpool_release_slot_table_locked(void);
static void _ctpool_release_slot_table_if_deferred_locked(void);

/* ========================================================================== */
/*                    CTPOOL HANDLE RESOLVE / UNPIN                           */
/* ========================================================================== */

/* Waits out every in-flight caller that resolved this pool before its slot
 * was retired. It polls instead of sleeping on a condition variable because
 * the unpin side sends no wakeup on purpose: the whole point of the unpin is
 * to touch nothing but the pin. The wait has no bound by design, since giving
 * up would mean freeing memory a live resolver still points at. */
static void _ctpool_wait_for_pins(cthread_pool *pool) {
  /* A pool whose handle was never published has no slot to wait on, and it
   * must not guess one. Without this check a teardown reached from a failed
   * construction derives an index from a zeroed handle, or from a slot that
   * is already back on the free list, and then waits out the pins of whatever
   * unrelated pool holds that slot. */
  if (pool->self_handle == 0) return;
  uint32_t idx = (uint32_t)(pool->self_handle >> 32);
  long delay_ns = 1000;
  while (ccol_pintable_pins(&ctpool_pintable, idx) > 0) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = delay_ns};
    nanosleep(&ts, NULL);
    if (delay_ns < 1000000L) delay_ns *= 2;
  }
}

/* Resolves h and pins the result against a concurrent destroy. Returns NULL
 * if h is 0 or garbage, or if it names a slot that is free now or has been
 * reused (the generation is wrong then). On success the caller MUST call
 * _ctpool_resolve_unpin(result) exactly once, as soon as it's done with the
 * resolved cthread_pool*. */
static cthread_pool *_ctpool_resolve(ctpool h) {
  /* No lock and no shared write. Every public entry point of this module goes
   * through here, so a write here to memory another thread reads would be
   * paid by every submit, from every submitting thread. */
  return (cthread_pool *)ccol_pintable_pin(&ctpool_pintable, h);
}

static void _ctpool_resolve_unpin(cthread_pool *raw) {
  /* Releases the pin and touches nothing else. Locking raw->mu here would
   * make every call take this pool's lock once more right after releasing
   * it, and that's what turns concurrent submitters into a convoy. There's no
   * wakeup to deliver either, since __ctpool_destroy polls the pin count
   * instead of sleeping on a condition variable that waits for this function.
   *
   * Reading raw->self_handle before the release is safe because the pin is
   * still held then. After the release the pool can be freed at any instant,
   * so nothing may touch raw after this call. */
  ccol_pintable_unpin(&ctpool_pintable, raw->self_handle);
}

#ifdef RUNNING_UNIT_TESTS
/* Test-only. Makes the next free-list push behave exactly like an allocation
 * failure, then disarms itself. Without it, the lost-index recovery below
 * would need a real out-of-memory at one particular cvector_push_back call,
 * which an ordinary test run can't produce. */
static _Atomic bool g_ctpool_fail_next_free_index_push = false;

void _ctpool_force_next_free_index_push_failure_for_tests(void) {
  atomic_store(&g_ctpool_fail_next_free_index_push, true);
}
#endif

/* Gives idx back for reuse. The caller holds the write lock.
 *
 * A failed push gets recorded, not swallowed. The slot it names is already
 * fully released, so silently dropping the index would strand that slot for
 * the life of the process and make every later create grow the table again.
 * See ctpool_slot_table.lost_indices. */
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

/* Gets back one slot index that a failed free-list push stranded (see
 * ctpool_slot_table.lost_indices), by scanning for a slot that is released
 * but that nothing names. The caller holds the write lock and has already
 * found free_indices empty, and that's what makes "ptr is NULL and in_use is
 * false" mean lost rather than just free: a slot released successfully is on
 * that list, and a slot whose destroy is still running keeps a non-NULL ptr
 * until the very end of that destroy.
 *
 * It's out of line and runs only when the counter is above zero, so an
 * ordinary acquire pays one comparison for it. If the scan finds nothing it
 * sets the counter to zero, so a counter that somehow outlives its slot
 * can't make every later acquire scan the table. */
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
 * handle, or 0 when there isn't enough memory. It runs once, from
 * ctpool_create_mp, after the object is otherwise fully built, and at that
 * point every worker thread is already running. */
static ctpool _ctpool_handle_slot_acquire(cthread_pool *pool) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  /* The table was released at unload, so no handle can be published any
   * more and the creation that asked fails cleanly. */
  if (!ctpool_slot_table.slots) {
    _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
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
    /* A slot whose index is past what the pin table holds can never be
     * published, so we refuse it here instead of claiming it and rolling
     * back. A rollback would put an index that no later publish can use onto
     * the free list that every acquire pops from. It's reported as an
     * ordinary failure, which is how a caller already treats a table
     * that can't grow. */
    if (cvector_elem_count(ctpool_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
      return 0;
    }
    ctpool_slot_t fresh = {0};
    if (cvector_push_back(ctpool_slot_table.slots, &fresh) != ccol_success) {
      _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
      return 0; /* an ordinary, non-fatal out-of-memory failure */
    }
    idx = (uint32_t)cvector_elem_count(ctpool_slot_table.slots) - 1;
    slot = (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that collides with the reserved "invalid
   * handle" sentinel, 0, which comes after about 2^32 reuses of this exact
   * slot index. The identical guard in chttpcli_handle_slot_acquire explains
   * why. */
  if (slot->generation == 0) slot->generation++;
  ctpool h = ((ctpool)idx << 32) | (ctpool)slot->generation;

  /* Written before the handle is published, so a resolver that finds this
   * pool also finds the handle its own unpin needs. */
  pool->self_handle = h;

  /* A publish can allocate. If it fails, no call could resolve the handle,
   * so the slot goes back on the free list instead. */
  if (!ccol_pintable_publish(&ctpool_pintable, idx, slot->generation, pool)) {
    /* Cleared because the slot goes back on the free list, and a handle left
     * behind would point this pool's teardown at a slot that belongs to
     * somebody else. */
    pool->self_handle = 0;
    /* The slot goes back on the free list in the same shape as a slot a
     * destroy retires, not just unused. _ctpool_atfork_prepare's walk handles
     * any slot with either flag set and dereferences its ptr, so a slot going
     * back on the free list must leave both flags clear. */
    slot->ptr = NULL;
#if CCOL_FORK_SAFETY_REQUIRED
    slot->torn_down = false;
#endif
    _ctpool_free_index_release_locked(idx);
    _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    return 0;
  }

  slot->ptr = pool;
  slot->in_use = true;
#if CCOL_FORK_SAFETY_REQUIRED
  slot->torn_down = false; /* it's always false already when a slot is
                               reused (see the field comment of
                               ctpool_slot_t), and the reset here is a
                               standing defence so we don't rely on that
                               invariant alone */
#endif
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                    WORKER SELF-CALL DETECTION                              */
/* ========================================================================== */

/* A process-wide thread-local key that records which cthread_pool, if any,
 * the calling thread is a worker of. worker_thread_fn sets it once at its own
 * entry, and nothing assigns or clears it again for the rest of that thread's
 * life. A ctpool worker thread belongs to exactly one pool for its whole
 * life, since a ctpool always creates and owns its own OS threads and never
 * shares them across pools, so one "which pool am I" value is enough and
 * there's no per-job set-and-clear to manage here. The analogous
 * ccol_event_loop_job_key_bundle of ccol_event_loop does need one in
 * cthreadcomm.c, because a dispatch_pool worker there serves many jobs from
 * many different registrations over its life.
 *
 * The key catches a task, or a task's on_complete callback, that calls
 * ctpool_wait, _shutdown_drain, _shutdown_immediate or __ctpool_destroy on
 * the very pool it runs on. worker_thread_fn gets `pool` as a bare pointer,
 * captured once when the thread starts, and that pointer has nothing to do
 * with the resolve and pin mechanism every public entry point goes through,
 * so the pin count says nothing about "a worker of this pool still uses it".
 *
 * Normally one thing alone keeps a worker's continued use of `pool` safe
 * across a shutdown: the _ccol_thread_join loop of
 * _ctpool_shutdown_drain_internal and _ctpool_shutdown_immediate_internal,
 * which blocks until every worker really finishes and returns from
 * worker_thread_fn, before any teardown goes on. A self-call silently breaks
 * that. pthread_join on the calling thread's own ID returns EDEADLK at once
 * instead of blocking (POSIX leaves the case undefined, and glibc's
 * pthread_join(3) documents this detection and rejection). Nothing here
 * checks what _ccol_thread_join returns, so the join loop believes every
 * worker has exited while the calling worker is still deep in its own call
 * stack, about to return to worker_thread_fn and keep touching `pool`.
 *
 * For __ctpool_destroy that's a real, deterministic use-after-free: after
 * the task returns, task_free, _ccol_mutex_lock, the decrement of
 * pool->active_count and _ccol_cond_var_broadcast all run against the pool
 * the destroy already freed. A pool with one worker whose only task calls
 * ctpool_destroy on its own pool hits it every single time.
 *
 * live tells whether the key exists. Creating the key can fail (in a process
 * that already hit PTHREAD_KEYS_MAX, for example), and key then holds no key
 * of this module: it can name a key some other component owns, and a value
 * set on it would reach that component's destructor. So a pool refuses to be
 * created while live is false, because its workers could neither set the key
 * nor be told apart from any other thread. live goes back to false, and the
 * key is deleted, when the module unloads with no pool left, so repeated
 * dlopen() and dlclose() cycles don't use up the process's keys. */
static struct {
  _ccol_thread_ls_key_t key;
  _ccol_once_flag_t once;
  atomic_bool live;
} ctpool_worker_key_bundle = {.once = _CCOL_ONCE_INIT};

static void _ctpool_init_worker_key(void) {
  if (_ccol_thread_ls_key_create(ctpool_worker_key_bundle.key, NULL) == 0)
    atomic_store(&ctpool_worker_key_bundle.live, true);
}

/* True when the worker key exists. Every other use of the key comes from a
 * pool that exists, and a pool exists only after this said true. */
static bool _ctpool_worker_key_available(void) {
  _ccol_call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  return atomic_load(&ctpool_worker_key_bundle.live);
}

/* One frame of the stack of pools whose discarded tasks the calling thread is
 * completing right now. _ctpool_discard_tasks pushes a frame, living on its
 * own C stack, around its loop and pops it afterwards. It's a stack and not
 * a single pool because an on_complete callback there can shut down another
 * pool, whose discards then run nested inside the outer ones, and the thread
 * counts as a worker of every pool on the stack. */
typedef struct _ctpool_discard_frame {
  cthread_pool *pool;
  struct _ctpool_discard_frame *prev;
} _ctpool_discard_frame;

static __thread _ctpool_discard_frame *_ctpool_discarding_top = NULL;

/* True when the calling thread is one of pool's worker threads, meaning the
 * call comes, directly or through other calls, from inside a task running on
 * pool or from that task's on_complete callback. False in every other
 * case. */
static bool _ctpool_is_self_call(cthread_pool *pool) {
  /* A thread running the on_complete callbacks of pool's discarded tasks
   * counts as a worker of pool while it does that; see
   * _ctpool_discard_tasks. */
  for (const _ctpool_discard_frame *f = _ctpool_discarding_top; f; f = f->prev)
    if (f->pool == pool) return true;
  _ccol_call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  void *mark = _ccol_thread_ls_get(ctpool_worker_key_bundle.key);
  if (mark == (void *)pool) return true;
  /* A worker of another pool carries that pool's mark. */
  if (mark != NULL) return false;
  /* No mark at all. That's every thread that isn't a worker, and also a
   * worker of this pool whose pthread_setspecific failed: glibc allocates the
   * storage for a key index of 32 or above lazily per thread, so that call
   * can fail with ENOMEM. Such a worker must still be recognised, or a task
   * that waits on, shuts down or destroys its own pool deadlocks or frees the
   * pool under itself. The workers' IDs are written before the pool's handle
   * is published and never change after that, so we can read them here with
   * no lock. Every caller of this function is a cold path (a full queue, a
   * wait, a shutdown or a destroy), so a scan over the workers costs nothing
   * that matters. */
  /* A slot whose worker has exited is skipped. glibc caches a joined
   * thread's stack and descriptor and gives both to the next thread that
   * starts, so a thread the application starts after a shutdown often
   * carries the ID of a joined worker. A worker marks its slot as its last
   * write to the pool, before it can be joined, so a joined worker's ID never
   * matches here. A pool inherited across fork() has no workers in this
   * process, and the children of that process reuse the descriptors of the
   * parent's threads the same way. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (atomic_load(&pool->foreign_since_fork)) return false;
#endif
  _ccol_thread_id_t me = _ccol_get_thread_id();
  for (size_t i = 0; i < pool->num_threads; i++) {
    if (_ccol_thread_id_equal(pool->threads[i].id, me) &&
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

/* Ceiling on the task free list of a pool with few workers, in nodes. A
 * node is a ctpool_task of five pointer-sized fields, so a pool keeps at most
 * 1.25 MiB of them on LP64 (before allocator overhead), and only after
 * traffic that really had that many tasks in flight at once. That covers the
 * queue depth a producer running ahead of one worker reaches in a burst of
 * about a hundred thousand submissions. */
#define CTPOOL_TASK_FREE_LIST_CEILING ((size_t)32768)

/* How many idle points a trim of the task free list waits for. A pool whose
 * workers keep up with its producer goes idle between almost every pair of
 * tasks, and trimming at each of them would give back nodes that the next
 * burst allocates again. See _ctpool_idle_unlock_and_trim. */
#define CTPOOL_TASK_FREE_LIST_TRIM_IDLE_POINTS ((uint32_t)256)

#ifdef RUNNING_UNIT_TESTS
/* Test-only. The number of idle points a trim waits for, so a test can get
 * to a trim with a few tasks. It defaults to the value the library ships
 * with. See _ctpool_set_task_free_list_window_for_tests. */
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
 * idle point. It gives half of what the list holds above num_threads * 4 back
 * to the pool's allocator, unless the list ran empty since the previous trim,
 * which means the traffic needed all of it. So a list nothing needs halves at
 * every trim, and a list the traffic keeps using stays whole. The caller
 * holds pool->mu and calls this at the idle point, where no task is queued
 * or running. The nodes are unlinked under the lock and freed after the
 * unlock, because a custom free function may be slow, and the function
 * unlocks pool->mu itself on every path. It's kept out of line so a worker's
 * completion path keeps its shape. */
static __attribute__((noinline)) void _ctpool_idle_unlock_and_trim(
    cthread_pool *pool) {
  if (++pool->idle_points_since_trim < CTPOOL_TRIM_WINDOW()) {
    _ccol_mutex_unlock(pool->mu);
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
  _ccol_mutex_unlock(pool->mu);
  while (freed) {
    ctpool_task *next = freed->next;
    _ccol_mem_free(pool->m_procs, freed);
    freed = next;
  }
}

/*
 * Recycles task into the pool's free list if there's still room under
 * task_free_list_cap. The caller must already hold pool->mu. Returns true
 * when the task was recycled, and then the caller has nothing more to do, and
 * false when the caller still has to really free task through the pool's
 * allocator. It may do that only AFTER unlocking pool->mu: a custom free
 * function from the caller can be as slow as it likes, so it must never run
 * while other threads may be blocked on this same lock.
 *
 * It's separate from task_free for one reason, which is that the hot
 * completion path of worker_thread_fn can fold this push into the same
 * critical section as the active_count decrement right after it. For the
 * pool "this task is done" is one indivisible event, and two lock and unlock
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
 * Releases a task struct. While there's still room under task_free_list_cap
 * it recycles the struct into the pool's free list, and otherwise it really
 * frees it through the pool's allocator.
 */
static void task_free(cthread_pool *pool, ctpool_task *task) {
  _ctpool_lock_mu(pool);
  bool recycled = task_release_locked(pool, task);
  _ccol_mutex_unlock(pool->mu);

  if (!recycled) _ccol_mem_free(pool->m_procs, task);
}

/*
 * Really frees, through the pool's allocator, every node the pool's task free
 * list holds. task_free differs in that it may just recycle a node into that
 * same list instead of freeing it.
 *
 * The pool's teardown calls this once no further submit or task_free
 * call for this pool can happen, that is, when every worker is joined and
 * every pinned resolve is released; the thread and memory cleanup of
 * _ctpool_teardown_raw already depends on the same precondition. Without this
 * call a node that was only recycled, and never handed back to m_procs,
 * leaks. There's no lock here, since by the time this runs nothing else can
 * touch this pool's task_free_list.
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

/* Appends a task to the queue. The caller must hold pool->mu. */
static void enqueue(cthread_pool *pool, ctpool_task *task) {
  task->next = NULL;
  if (pool->tail) {
    pool->tail->next = task;
  } else {
    pool->head = task;
  }
  pool->tail = task;
  __atomic_store_n(&pool->queue_size, pool->queue_size + 1, __ATOMIC_RELAXED);
}

/* Takes the head task off the queue. The caller must hold pool->mu, and the
 * queue must not be empty. */
static ctpool_task *dequeue(cthread_pool *pool) {
  ctpool_task *task = pool->head;
  pool->head = task->next;
  if (!pool->head) pool->tail = NULL;
  __atomic_store_n(&pool->queue_size, pool->queue_size - 1, __ATOMIC_RELAXED);
  return task;
}

#ifdef RUNNING_UNIT_TESTS
/* When set, the next future allocation of a submit fails the way a failed
 * calloc would; with the system allocator a test has no other way to reach
 * that path. The counter holds how many futures exist right now. */
atomic_bool _ctpool_fail_next_future_alloc_for_tests;
atomic_long _ctpool_live_futures_for_tests;
#endif

/*
 * Destroys and frees f once the caller has seen f's refcount reach 0. The
 * caller decrements that count under f->mu and unlocks f->mu before calling
 * this. It's the shared tail of every "drop a reference, free on the last
 * release" site in this file: future_deref, future_cancel, the worker's
 * inline future completion in worker_thread_fn, ctpool_future_fulfill, and
 * ctpool_future_get releasing its own waiter reference, each of which would
 * otherwise repeat the same three lines. Any of those threads can be the one
 * that sees zero, so this function doesn't care which reference the caller
 * held.
 */
static void future_destroy_if_unreferenced(ctpool_future *f, int remaining) {
  if (remaining == 0) {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_sub_explicit(&_ctpool_live_futures_for_tests, 1,
                              memory_order_relaxed);
#endif
    _ccol_mutex_destroy(f->mu);
    _ccol_cond_var_destroy(f->cv);
    free(f);
  }
}

/*
 * Decrements a future's refcount and frees the future when the count gets to
 * 0. The caller must NOT hold future->mu, because this function locks and
 * unlocks it.
 */
static void future_deref(ctpool_future *f) {
  _ccol_mutex_lock(f->mu);
  int remaining = --f->refcount;
  _ccol_mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

/*
 * Cancels a future: marks it done and cancelled, broadcasts to every waiting
 * ctpool_future_get caller, and then gives up the task's reference.
 */
static void future_cancel(ctpool_future *f) {
  _ccol_mutex_lock(f->mu);
  f->cancelled = true;
  f->done = true;
  _ccol_cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  _ccol_mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

/*
 * Takes the whole task list off the queue in one step. The caller must hold
 * pool->mu, and may cancel and free the returned list only after
 * unlocking pool->mu, because future_cancel locks future->mu and future->mu
 * must never nest under pool->mu.
 */
static ctpool_task *steal_queue(cthread_pool *pool) {
  ctpool_task *list = pool->head;
  pool->head = NULL;
  pool->tail = NULL;
  __atomic_store_n(&pool->queue_size, 0, __ATOMIC_RELAXED);
  return list;
}

/*
 * Completes and frees every task of list, a list that steal_queue took off
 * the queue before any worker ran it. A future task gets its future
 * cancelled and a callback task gets on_complete called with ran == false,
 * so every task a submit accepted gets exactly one on_complete call, with
 * true from the worker that ran it or with false from here. The caller must
 * not hold pool->mu, since future_cancel locks future->mu (which must never
 * nest under pool->mu) and an on_complete callback can call back into this
 * module.
 *
 * While the callbacks run, the calling thread counts as a worker of pool for
 * _ctpool_is_self_call. So a callback that calls ctpool_wait,
 * ctpool_shutdown_drain or ctpool_shutdown_immediate on pool gets the
 * documented no-op instead of waiting on the shutdown that this very thread
 * hasn't finished yet, and one that calls ctpool_destroy on pool gets the
 * documented fatal error instead of waiting on its own pin.
 *
 * recycle picks how a node is freed: through task_free, which may keep it on
 * a live pool's free list, or straight back to m_procs for a pool that
 * nothing can submit to again.
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
 * Computes the absolute deadline of a timed submit, the current
 * CLOCK_MONOTONIC time plus timeout_us microseconds. Returns false if
 * clock_gettime fails. not_full, the condition variable the deadline is used
 * with, measures on the same clock; see _ctpool_init_monotonic_cond_var.
 *
 * The sum saturates. A timeout that reaches past the largest time_t, such as
 * UINT64_MAX, gives the latest representable time, instead of a tv_sec that
 * wrapped into the past and would make the submit time out at once.
 */
static bool make_abs_deadline(uint64_t timeout_us, struct timespec *abs_out) {
  return ccol_deadline_after_us(timeout_us, abs_out);
}

/* ========================================================================== */
/*                         WORKER THREAD                                      */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/* While true, a starting worker skips setting its thread-local mark, as if
 * pthread_setspecific had failed. A test sets it only around the creation of
 * the pool it looks at. */
atomic_bool _ctpool_skip_worker_mark_for_tests;
#endif /* RUNNING_UNIT_TESTS */

static void *worker_thread_fn(void *arg) {
  cthread_pool *pool = (cthread_pool *)arg;

  /* This is published once, before this thread can run any task, so nothing
   * this thread runs can call back into this module before this point. The
   * comment of ctpool_worker_key_bundle above says why nothing must set or
   * clear it again for the rest of this thread's life. */
  _ccol_call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  /* A failure here is tolerated, since _ctpool_is_self_call then recognises
   * this thread from the pool's worker IDs instead. A key deleted at unload
   * is never set, because by then its index can belong to another component;
   * the pool this worker serves is then refused a slot and torn down. */
#ifdef RUNNING_UNIT_TESTS
  if (!atomic_load(&_ctpool_skip_worker_mark_for_tests))
#endif /* RUNNING_UNIT_TESTS */
    if (atomic_load(&ctpool_worker_key_bundle.live))
      (void)_ccol_thread_ls_set(ctpool_worker_key_bundle.key, (void *)pool);

  for (;;) {
    _ctpool_lock_mu(pool);

    /*
     * Wait until there's work to do or a shutdown condition holds. The
     * wakeup conditions are:
     *   - a task went into the queue (queue_size > 0)
     *   - shutdown_immediate is set
     *   - shutdown_drain is set AND the queue is now empty (all work done)
     */
    while (pool->queue_size == 0 && !pool->shutdown_immediate &&
           !pool->shutdown_drain) {
      _ccol_cond_var_wait(pool->not_empty, pool->mu);
    }

    /* A drain shutdown with an empty queue, so this worker's job is done. */
    if (pool->shutdown_immediate ||
        (pool->shutdown_drain && pool->queue_size == 0)) {
      _ccol_mutex_unlock(pool->mu);
      break;
    }

    ctpool_task *task = dequeue(pool);
    pool->active_count++;

    /* A slot is free now, so wake a blocked submitter (bounded queue only). */
    if (pool->queue_cap > 0) {
      _ccol_cond_var_signal(pool->not_full);
    }

    _ccol_mutex_unlock(pool->mu);

    /* Run the task. */
    if (task->future) {
      void *result = task->fn_future(task->arg);
      _ccol_mutex_lock(task->future->mu);
      task->future->result = result;
      task->future->done = true;
      _ccol_cond_var_broadcast(task->future->cv);
      int remaining = --task->future->refcount;
      _ccol_mutex_unlock(task->future->mu);
      future_destroy_if_unreferenced(task->future, remaining);
    } else {
      task->fn(task->arg);
      if (task->on_complete) {
        task->on_complete(task->arg, true);
      }
    }

    /* The task's free-list release goes into the same critical section as
     * the active_count decrement just below, since two separate lock and
     * unlock round trips on pool->mu would pay twice for one "this task is
     * done" event; see the doc comment of task_release_locked. The
     * _ccol_mem_free call itself, which may be a slow function the caller
     * gave us, happens after the unlock. */
    _ctpool_lock_mu(pool);
    bool recycled = task_release_locked(pool, task);
    pool->active_count--;
    if (pool->active_count == 0 && pool->queue_size == 0) {
      _ccol_cond_var_broadcast(pool->idle_cv);
      _ctpool_idle_unlock_and_trim(pool);
    } else {
      _ccol_mutex_unlock(pool->mu);
    }
    if (!recycled) _ccol_mem_free(pool->m_procs, task);
  }

  /* See the threads field of struct cthread_pool. Reading the IDs here is
   * safe: the creator wrote them before publishing the handle, and the
   * shutdown that ends this loop reached this thread through pool->mu. */
  _ccol_thread_id_t me = _ccol_get_thread_id();
  for (size_t i = 0; i < pool->num_threads; i++) {
    if (_ccol_thread_id_equal(pool->threads[i].id, me)) {
      atomic_store_explicit(&pool->threads[i].exited, true,
                            memory_order_release);
      break;
    }
  }
  return NULL;
}

/* A sentinel `idx` for _ctpool_teardown_raw that means the pool was never
 * registered in ctpool_slot_table at all. ctpool_create_mp's rollback path
 * uses it after a slot acquire fails. There's no slot to read then, so
 * torn_down bookkeeping is neither possible nor needed, and since no caller
 * ever saw that handle a concurrent fork() can't race it through this
 * module's slot-table atfork mechanism. */
#define CTPOOL_TEARDOWN_NO_SLOT ((uint32_t)-1)

/* A forward declaration. ctpool_create_mp's rollback path after a failed
 * slot acquire needs the shared teardown helper, which is defined later in
 * this file, right after the split between the internal and public
 * shutdown_drain and shutdown_immediate functions it depends on. */
static void _ctpool_teardown_raw(cthread_pool *pool, uint32_t idx);

/* ========================================================================== */
/*                         CREATION                                           */
/* ========================================================================== */

ctpool ctpool_create_mp(size_t num_threads, size_t queue_capacity,
                        ccol_memmgmt_procs_t *mprocs, char **err_str) {
  if (num_threads == 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("num_threads must be >= 1");
    return CTPOOL_INVALID;
  }
  /* ccol_invalid_size is the error value of a size and never a capacity. A
   * caller who wants an unbounded queue passes 0. */
  if (queue_capacity == ccol_invalid_size) {
    if (err_str)
      *err_str = _CCOL_ERR_STR(
          "queue_capacity is ccol_invalid_size; pass 0 for an unbounded queue");
    return CTPOOL_INVALID;
  }

  if (!_ccol_verify_memmgmt_procs(mprocs, err_str)) return CTPOOL_INVALID;

  if (!_ctpool_worker_key_available()) {
    if (err_str)
      *err_str = _CCOL_ERR_STR(
          "the thread-local worker key is not available: it could not be "
          "created, or the library already released it at unload");
    return CTPOOL_INVALID;
  }

  cthread_pool *pool =
      (cthread_pool *)(mprocs ? mprocs->calloc(1, sizeof(*pool))
                              : calloc(1, sizeof(*pool)));
  if (!pool) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate pool struct");
    return CTPOOL_INVALID;
  }

  if (mprocs) {
    pool->m_procs = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(*mprocs));
    if (!pool->m_procs) {
      mprocs->free(pool);
      if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate m_procs copy");
      return CTPOOL_INVALID;
    }
    memcpy(pool->m_procs, mprocs, sizeof(*mprocs));
  }

  pool->queue_cap = queue_capacity;

  if (_ccol_mutex_init(pool->mu) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("mutex init failed");
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (_ccol_cond_var_init(pool->not_empty) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("cond_var init failed");
    _ccol_mutex_destroy(pool->mu);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (_ctpool_init_monotonic_cond_var(&pool->not_full) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("cond_var init failed");
    _ccol_mutex_destroy(pool->mu);
    _ccol_cond_var_destroy(pool->not_empty);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (_ccol_cond_var_init(pool->idle_cv) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("cond_var init failed");
    _ccol_mutex_destroy(pool->mu);
    _ccol_cond_var_destroy(pool->not_empty);
    _ccol_cond_var_destroy(pool->not_full);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (_ccol_cond_var_init(pool->shutdown_cv) != 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("cond_var init failed");
    _ccol_mutex_destroy(pool->mu);
    _ccol_cond_var_destroy(pool->not_empty);
    _ccol_cond_var_destroy(pool->not_full);
    _ccol_cond_var_destroy(pool->idle_cv);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }

#if CCOL_FORK_SAFETY_REQUIRED
  atomic_init(&pool->foreign_since_fork, false);
#endif

  pool->threads = (ctpool_worker_slot *)_ccol_mem_calloc(
      pool->m_procs, num_threads, sizeof(ctpool_worker_slot));
  if (!pool->threads) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate threads array");
    _ccol_mutex_destroy(pool->mu);
    _ccol_cond_var_destroy(pool->not_empty);
    _ccol_cond_var_destroy(pool->not_full);
    _ccol_cond_var_destroy(pool->idle_cv);
    _ccol_cond_var_destroy(pool->shutdown_cv);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }

  pool->num_threads = num_threads;
  /* The field comment of task_free_list_cap in struct cthread_pool explains
   * this multiplier. */
  pool->task_free_list_cap = num_threads * 4 > CTPOOL_TASK_FREE_LIST_CEILING
                                 ? num_threads * 4
                                 : CTPOOL_TASK_FREE_LIST_CEILING;

  for (size_t i = 0; i < num_threads; i++) {
    if (_ccol_thread_create(pool->threads[i].id, worker_thread_fn, pool) != 0) {
      /* Shut down the threads that did start, then clean up. */
      _ctpool_lock_mu(pool);
      __atomic_store_n(&pool->shutdown_drain, true, __ATOMIC_RELAXED);
      pool->shutdown_started = true;
      _ccol_cond_var_broadcast(pool->not_empty);
      _ccol_mutex_unlock(pool->mu);
      for (size_t j = 0; j < i; j++) {
        _ccol_thread_join(pool->threads[j].id);
      }
      if (err_str) *err_str = _CCOL_ERR_STR("pthread_create failed");
      _ccol_mem_free(pool->m_procs, pool->threads);
      _ccol_mutex_destroy(pool->mu);
      _ccol_cond_var_destroy(pool->not_empty);
      _ccol_cond_var_destroy(pool->not_full);
      _ccol_cond_var_destroy(pool->idle_cv);
      _ccol_cond_var_destroy(pool->shutdown_cv);
      pool_free_self(pool);
      return CTPOOL_INVALID;
    }
  }

  /* The slot acquire is the LAST step, once every worker thread is running
   * and the pool is otherwise fully built. So an out-of-memory failure here
   * is safe to unwind on its own, with no worry about a double destroy or
   * about visibility, because no caller ever saw the handle. Unlike the
   * failure paths above, every worker thread may already be running here, so
   * the rollback must really stop them, freeing memory isn't enough.
   * _ctpool_teardown_raw does exactly that (a drain shutdown, a join of every
   * thread, then the frees), and we reuse the same sequence __ctpool_destroy
   * uses rather than a hand-written variant that could drift away from it. */
  ctpool h = _ctpool_handle_slot_acquire(pool);
  if (h == 0) {
    if (err_str)
      *err_str = _CCOL_ERR_STR("failed to allocate ctpool handle slot");
    _ctpool_teardown_raw(pool, CTPOOL_TEARDOWN_NO_SLOT);
    return CTPOOL_INVALID;
  }

  return h;
}

/* ========================================================================== */
/*                         INTERNAL SUBMIT HELPER                             */
/* ========================================================================== */

/*
 * The common path of all three submit variants. `block` says how to wait for
 * a free slot: block until one is free, don't wait at all, or wait until a
 * deadline. `abs_deadline` is read only when block == 2.
 *
 *   block == 0: try (return ccol_container_full at once)
 *   block == 1: block and wait
 *   block == 2: wait until the deadline
 */
/* The shutdown check and the wait for room that every submit makes. The
 * caller holds pool->mu. On ccol_success the lock is still held and the queue
 * has room; on any other result the lock has been released. */
#ifdef RUNNING_UNIT_TESTS
/* When set, the next timed wait for room ignores its deadline, waits for a
 * real wake and then reports ETIMEDOUT, which is what a timed wait returns
 * when it consumed a signal just as its deadline passed. The second flag
 * tells the test that the waiter is inside the wait. */
atomic_bool _ctpool_timeout_after_wake_for_tests;
atomic_bool _ctpool_timeout_after_wake_waiting_for_tests;
#endif

/* Always inlined, since each submit instantiates it with a constant block
 * and an out-of-line copy would add a call to the timed plain submit. */
static inline __attribute__((always_inline)) ccol_retval_t
submit_wait_for_room_locked(cthread_pool *pool, int block,
                            const struct timespec *abs_deadline) {
  if (pool->shutdown_drain || pool->shutdown_immediate) {
    _ccol_mutex_unlock(pool->mu);
    return ccol_not_permitted;
  }

  if (pool->queue_cap > 0) {
    while (pool->queue_size >= pool->queue_cap && !pool->shutdown_drain &&
           !pool->shutdown_immediate) {
      if (block == 0) {
        /* try_submit: return at once */
        _ccol_mutex_unlock(pool->mu);
        return ccol_container_full;
      } else if (_ctpool_is_self_call(pool)) {
        /*
         * A waiting submit, blocking or timed, that comes from a worker of
         * this very pool must not park on not_full. It comes from a task (or
         * that task's on_complete callback) sending work back into the pool
         * it runs on. Only this pool's workers can ever free a slot and this
         * call runs on one of them, so waiting here takes that worker out of
         * the set that could satisfy the wait. With every worker in that
         * state at once and the queue at capacity, no thread in the process
         * can make progress again: the pool is deadlocked for good, along
         * with every external submitter blocked on the same queue. A timed
         * wait ends only at its deadline, which for UINT64_MAX is never.
         *
         * So we report a full queue to such a caller instead, exactly as
         * ctpool_try_submit does, and recovery is up to the caller (retry
         * later, hand the work to another pool, or use an unbounded queue).
         * An error the caller can act on is much better than a pool that
         * never moves again.
         * The check sits inside the full-queue wait loop, so a submit that
         * finds room pays nothing for it and an external submitter waits
         * exactly as before.
         */
        _ccol_mutex_unlock(pool->mu);
        return ccol_container_full;
      } else if (block == 1) {
        _ccol_cond_var_wait(pool->not_full, pool->mu);
      } else {
        /* timed_submit */
        int rc;
#ifdef RUNNING_UNIT_TESTS
        if (atomic_exchange(&_ctpool_timeout_after_wake_for_tests, false)) {
          atomic_store(&_ctpool_timeout_after_wake_waiting_for_tests, true);
          _ccol_cond_var_wait(pool->not_full, pool->mu);
          rc = ETIMEDOUT;
        } else
#endif
          rc =
              _ccol_cond_var_timedwait(pool->not_full, pool->mu, *abs_deadline);
        /* A failed wait may still have consumed a dequeue's signal (POSIX
         * lets a timed wait that returns ETIMEDOUT do that), so the state
         * decides and not the return code: a free slot or a shutdown is
         * handled as after a wake. Reporting the failure next to a free slot
         * would leave a blocking submitter asleep beside it, with no further
         * dequeue to signal it. */
        if (rc != 0 && pool->queue_size >= pool->queue_cap &&
            !pool->shutdown_drain && !pool->shutdown_immediate) {
          _ccol_mutex_unlock(pool->mu);
          return rc == ETIMEDOUT ? ccol_timed_out : ccol_unexpected_failure;
        }
      }
    }
    /* Check the shutdown state again after waking up. */
    if (pool->shutdown_drain || pool->shutdown_immediate) {
      _ccol_mutex_unlock(pool->mu);
      return ccol_not_permitted;
    }
  }

  return ccol_success;
}

/* A submit that takes its task node from the free list in the same pool->mu
 * critical section as the enqueue, so it locks the pool once instead of
 * twice. In a busy pool every worker contends for pool->mu on each dequeue,
 * so every round trip a submit saves is one handoff of that line it doesn't
 * have to wait for. When the free list is empty the node is allocated
 * outside the lock, through the pool's allocator, and the checks run again,
 * because meanwhile the queue may have filled or the pool may have started
 * shutting down.
 *
 * The not_empty signal goes out on every enqueue. Skipping it when no
 * worker seems to wait looks cheaper, but a burst then wakes fewer workers
 * and the queue drains on fewer threads, which costs far more than the
 * signals it saves.
 *
 * A callback task passes fn and on_complete with a NULL future, and a future
 * task passes fn_future and its future, which the caller allocated before
 * the call so nothing allocates inside the critical section. Every caller
 * passes the two NULL arguments as constants, so the callback-task
 * instantiation holds none of the future-task code and the other way round.
 * block follows submit_generic's convention: 0 is try, 1 blocks and 2 is
 * timed. */
static inline __attribute__((always_inline)) ccol_retval_t
submit_pop_and_enqueue_body(cthread_pool *pool, void (*fn)(void *),
                            void *(*fn_future)(void *), void *arg,
                            void (*on_complete)(void *, bool),
                            ctpool_future *future, int block,
                            const struct timespec *abs_deadline) {
  ctpool_task *spare = NULL;
  _ctpool_lock_mu(pool);
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
      _ccol_mutex_unlock(pool->mu);
      spare = (ctpool_task *)_ccol_mem_calloc(pool->m_procs, 1,
                                              sizeof(ctpool_task));
      if (!spare) {
        /* This submitter may have used up the one not_full signal a dequeue
         * sent, and it's leaving without taking the room, so it passes the
         * signal on; otherwise another submitter waiting for room could stay
         * asleep next to a free slot. */
        _ctpool_lock_mu(pool);
        _ccol_cond_var_signal(pool->not_full);
        _ccol_mutex_unlock(pool->mu);
        return ccol_not_enough_memory;
      }
      _ctpool_lock_mu(pool);
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
    _ccol_cond_var_signal(pool->not_empty);
    _ccol_mutex_unlock(pool->mu);
    if (spare) _ccol_mem_free(pool->m_procs, spare);
    return ccol_success;
  }
}

/* Every callback-task submit, try, blocking or timed. It's always inlined so
 * the plain submit paths end up whole inside ctpool_submit, ctpool_try_submit
 * and ctpool_timed_submit: a second instantiation of the shared body makes
 * the inliner keep this one out of line, which adds a call and a frame to
 * every plain submit. */
static inline __attribute__((always_inline)) ccol_retval_t
submit_pop_and_enqueue(cthread_pool *pool, void (*fn)(void *), void *arg,
                       void (*on_complete)(void *, bool), int block,
                       const struct timespec *abs_deadline) {
  return submit_pop_and_enqueue_body(pool, fn, NULL, arg, on_complete, NULL,
                                     block, abs_deadline);
}

/* Every future-task submit, try, blocking or timed. future must not be NULL.
 * On any return other than ccol_success the queue doesn't hold it and the
 * caller still owns both of its references. */
static ccol_retval_t submit_future_pop_and_enqueue(
    cthread_pool *pool, void *(*fn_future)(void *), void *arg,
    ctpool_future *future, int block, const struct timespec *abs_deadline) {
  return submit_pop_and_enqueue_body(pool, NULL, fn_future, arg, NULL, future,
                                     block, abs_deadline);
}

/* ========================================================================== */
/*                         TASK SUBMISSION                                    */
/* ========================================================================== */

/*
 * The shared body of ctpool_submit, ctpool_try_submit and
 * ctpool_timed_submit, which differ only in the block and timeout_us pair
 * they pass. Without it each of them would repeat the whole sequence around
 * the submit: resolve, validate, submit and unpin.
 *
 * block follows submit_pop_and_enqueue_body's convention: 0 is try, 1 blocks
 * and 2 is timed. timeout_us is the caller's relative timeout in
 * microseconds for block == 2 and isn't read otherwise. The absolute deadline
 * is computed here, with make_abs_deadline, at the point in the validation
 * order that ctpool_timed_submit's contract fixes: after pool is resolved and
 * fn validated, and before a task node is taken. Every kind of submit takes
 * the pool's lock once when the free list holds a node, and a try refused
 * for a full queue or a shutdown allocates nothing. It's always inlined, so
 * each public submit has its own copy with block folded to a constant
 * instead of calling a shared one.
 */
static inline __attribute__((always_inline)) ccol_retval_t submit_generic(
    ctpool pool, void (*fn)(void *), void *arg,
    void (*on_complete)(void *, bool), int block, uint64_t timeout_us) {
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
  }
  ccol_retval_t r =
      submit_pop_and_enqueue(raw, fn, arg, on_complete, block, abs_deadline);
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
  /* A timeout_us of 0 behaves like try_submit, and we delegate before
   * resolving pool at all. Resolving here only to unpin at once and then
   * delegate would cost two resolve and unpin pairs, while this path pays for
   * exactly one. Either way it's the resolve inside ctpool_try_submit that
   * validates pool and fn, so the caller sees the same result for every
   * input, valid or not. */
  if (timeout_us == 0) return ctpool_try_submit(pool, fn, arg, on_complete);
  return submit_generic(pool, fn, arg, on_complete, 2, timeout_us);
}

/* ========================================================================== */
/*                         FUTURES                                            */
/* ========================================================================== */

/*
 * Allocates a submit's future with a refcount of 2, one reference for the
 * caller and one for the queued task. A submit calls it before taking the
 * pool's lock, so the submit's critical section allocates nothing for the
 * future.
 */
static ctpool_future *submit_future_alloc(void) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&_ctpool_fail_next_future_alloc_for_tests, false))
    return NULL;
#endif
  ctpool_future *f = (ctpool_future *)calloc(1, sizeof(ctpool_future));
  if (!f) return NULL;
  if (_ccol_mutex_init(f->mu) != 0) {
    free(f);
    return NULL;
  }
  if (_ccol_cond_var_init(f->cv) != 0) {
    _ccol_mutex_destroy(f->mu);
    free(f);
    return NULL;
  }
  f->refcount = 2;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_ctpool_live_futures_for_tests, 1,
                            memory_order_relaxed);
#endif
  return f;
}

/* Frees a future that submit_future_alloc made and no queue ever held. */
static void submit_future_discard(ctpool_future *f) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_sub_explicit(&_ctpool_live_futures_for_tests, 1,
                            memory_order_relaxed);
#endif
  _ccol_mutex_destroy(f->mu);
  _ccol_cond_var_destroy(f->cv);
  free(f);
}

/*
 * The answer a future try submit gets from the pool's state, read without
 * the lock: ccol_not_permitted once a shutdown has started,
 * ccol_container_full while a bounded queue is at capacity, and ccol_success
 * otherwise. A try the hint refuses returns at once, so a refusal neither
 * allocates a future nor takes the lock. ccol_success is only a hint, and
 * the submit's critical section checks the same state again under the lock
 * and decides. The loads are relaxed, and each one sees at least every store
 * that happens before the call, so a refusal never reports a state that the
 * caller has already seen end.
 */
static ccol_retval_t submit_future_refusal_hint(cthread_pool *pool) {
  if (__atomic_load_n(&pool->shutdown_drain, __ATOMIC_RELAXED) ||
      __atomic_load_n(&pool->shutdown_immediate, __ATOMIC_RELAXED))
    return ccol_not_permitted;
  size_t cap = pool->queue_cap;
  if (cap > 0 && __atomic_load_n(&pool->queue_size, __ATOMIC_RELAXED) >= cap)
    return ccol_container_full;
  return ccol_success;
}

/*
 * The shared body of ctpool_submit_future, ctpool_try_submit_future and
 * ctpool_timed_submit_future. It's factored like submit_generic above and
 * uses the same block and timeout_us convention. On the success path it
 * takes the pool's lock once: the future is allocated before that critical
 * section, and inside it the task node is popped from the free list and
 * enqueued, as in the blocking and timed plain submits.
 *
 * The very first thing it does, unconditionally and before it even resolves
 * pool, is set *out to NULL. Every caller below already makes sure out itself
 * isn't NULL: ctpool_try_submit_future and ctpool_timed_submit_future check
 * out and return ccol_invalid_args without calling this function when it's
 * NULL, and ctpool_submit_future always passes the address of its own local.
 * That one assignment is what makes *out reliably NULL on every failure path
 * here, which matches each public function's documented contract without
 * repeating the guarantee at every return site.
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
    ccol_retval_t hint = submit_future_refusal_hint(raw);
    if (hint != ccol_success) {
      _ctpool_resolve_unpin(raw);
      return hint;
    }
  }

  ctpool_future *f = submit_future_alloc();
  if (!f) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }

  ccol_retval_t r =
      submit_future_pop_and_enqueue(raw, fn, arg, f, block, abs_deadline);
  if (r != ccol_success)
    submit_future_discard(f);
  else
    *out = f;
  _ctpool_resolve_unpin(raw);
  return r;
}

ctpool_future *ctpool_submit_future(ctpool pool, void *(*fn)(void *), void *arg,
                                    ccol_retval_t *rv_out) {
  ctpool_future *f;
  ccol_retval_t r = submit_future_generic(pool, fn, arg, 1, 0, &f);
  if (rv_out) *rv_out = r;
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
  /* A timeout_us of 0 behaves like try_submit_future. The identical
   * delegation comment in ctpool_timed_submit above explains why calling
   * submit_future_generic directly here keeps the behaviour, instead of
   * resolving first and then delegating to ctpool_try_submit_future. */
  if (timeout_us == 0) return submit_future_generic(pool, fn, arg, 0, 0, out);
  return submit_future_generic(pool, fn, arg, 2, timeout_us, out);
}

void *ctpool_future_get(ctpool_future *f) {
  if (!f) return NULL;
  _ccol_mutex_lock(f->mu);
  /*
   * A waiter holds its own reference for the whole call, taken under f->mu
   * before the first wait and given up under the same mutex on the way out.
   * Without it this call would block inside a struct it has no claim on. All
   * the other references (the queued task's and the caller's) can go while
   * this thread sits in _ccol_cond_var_wait, and the release that brings the
   * count to zero then destroys f->mu and f->cv and frees f under this
   * thread.
   *
   * The completion broadcast makes that likely, not just possible. One
   * broadcast wakes every waiter at once and each of them locks f->mu
   * again to leave the wait, so whichever thread gets out first can drop the
   * last other reference while the rest are still inside the mutex.
   *
   * With the reference the count can't reach zero while a waiter is still
   * here, and the last thread out of the future destroys it exactly once,
   * waiter or reference owner, whatever order they finish in. The increment
   * and decrement are plain int operations on a mutex this call holds
   * anyway, so an uncontended get pays two arithmetic operations and one
   * branch the CPU already predicts.
   */
  f->refcount++;
  while (!f->done) {
    _ccol_cond_var_wait(f->cv, f->mu);
  }
  void *result = f->result;
  int remaining = --f->refcount;
  _ccol_mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
  return result;
}

bool ctpool_future_done(ctpool_future *f) {
  if (!f) return false;
  _ccol_mutex_lock(f->mu);
  bool done = f->done;
  _ccol_mutex_unlock(f->mu);
  return done;
}

bool ctpool_future_cancelled(ctpool_future *f) {
  if (!f) return false;
  _ccol_mutex_lock(f->mu);
  bool cancelled = f->cancelled;
  _ccol_mutex_unlock(f->mu);
  return cancelled;
}

void ctpool_future_free(ctpool_future *f) {
  if (!f) return;
  future_deref(f);
}

ctpool_future *ctpool_future_create_detached(char **err_str) {
  ctpool_future *f = (ctpool_future *)calloc(1, sizeof(ctpool_future));
  if (!f) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate future");
    return NULL;
  }
  if (_ccol_mutex_init(f->mu) != 0) {
    free(f);
    if (err_str) *err_str = _CCOL_ERR_STR("future mutex init failed");
    return NULL;
  }
  if (_ccol_cond_var_init(f->cv) != 0) {
    _ccol_mutex_destroy(f->mu);
    free(f);
    if (err_str) *err_str = _CCOL_ERR_STR("future cond_var init failed");
    return NULL;
  }
  f->refcount = 2; /* one reference for the caller and one for the producer */
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&_ctpool_live_futures_for_tests, 1,
                            memory_order_relaxed);
#endif
  return f;
}

ccol_retval_t ctpool_future_fulfill(ctpool_future *f, void *result) {
  if (!f) return ccol_invalid_args;
  _ccol_mutex_lock(f->mu);
  if (f->done) {
    _ccol_mutex_unlock(f->mu);
    return ccol_not_permitted;
  }
  f->result = result;
  f->done = true;
  _ccol_cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  _ccol_mutex_unlock(f->mu);
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
   * calling ctpool_wait on the very pool it runs on would deadlock against
   * itself, since the pool counts that task in active_count until it
   * returns. So we treat such a pool as idle, the same way the code a few
   * lines below treats a foreign pool (one inherited across a fork) as idle,
   * and for the same reason: nothing could ever satisfy a wait here. */
  if (_ctpool_is_self_call(raw)) {
    _ctpool_resolve_unpin(raw);
    return;
  }
  _ctpool_lock_mu(raw);
  /* A foreign pool is one this process inherited across a fork; see the
   * field comment of foreign_since_fork in struct cthread_pool. Its
   * active_count and queue_size can never legitimately reach zero through
   * anything this process does, because every worker thread that could work
   * through the rest of the queue or finish a task in flight exists only in
   * the parent, which is gone. Nothing here ever decrements active_count,
   * drains queue_size or broadcasts idle_cv again, so waiting on that
   * condition would hang forever. We treat a foreign pool as idle instead,
   * just as _ctpool_shutdown_drain_internal and
   * _ctpool_shutdown_immediate_internal skip joining a foreign pool's worker
   * threads, which don't exist either. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&raw->foreign_since_fork)) {
    while (raw->active_count > 0 || raw->queue_size > 0) {
      _ccol_cond_var_wait(raw->idle_cv, raw->mu);
    }
  }
#else
  while (raw->active_count > 0 || raw->queue_size > 0) {
    _ccol_cond_var_wait(raw->idle_cv, raw->mu);
  }
#endif
  _ccol_mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
}

/* Publishes that every worker of this pool has been joined, and then
 * releases every other shutdown call waiting for that. Whichever of the two
 * shutdown bodies below did the real shutdown calls this as its very last
 * step. A pool that never gets to either of them publishes nothing; a
 * construction rollback that joins its own partly started workers inline is
 * such a pool, and since no caller ever saw its handle nothing can be
 * waiting. */
static void _ctpool_mark_shutdown_complete(cthread_pool *pool) {
  _ctpool_lock_mu(pool);
  pool->shutdown_complete = true;
  _ccol_cond_var_broadcast(pool->shutdown_cv);
  _ccol_mutex_unlock(pool->mu);
}

/* The internal shutdown_drain body, taking a raw pointer. Two places share
 * it: the public ctpool_shutdown_drain wrapper below, and
 * _ctpool_teardown_raw, which both __ctpool_destroy and ctpool_create_mp's
 * rollback (after a failed slot acquire) use. It's the same pattern as
 * _chttpsvr_stop_internal behind chttpsvr_stop. */
static void _ctpool_shutdown_drain_internal(cthread_pool *pool) {
  _ctpool_lock_mu(pool);
  if (pool->shutdown_started) {
    /* Another call already owns this pool's shutdown, so we wait it out
     * instead of returning. shutdown_started is set before a single worker is
     * woken, and long before one is joined, so returning here would hand the
     * caller a pool whose workers are all still running and whose queued
     * tasks haven't run. For every caller but the first that would break this
     * function's documented guarantee, that it blocks until every queued and
     * every active task completes.
     *
     * The owner sets shutdown_complete and broadcasts below, after its own
     * join loop, and the owner is never this thread: the public wrapper
     * rejects a self-call before it gets here, and _ctpool_teardown_raw's
     * caller is never a worker of this pool. So this wait can't deadlock
     * against the join it is waiting for. */
    while (!pool->shutdown_complete) {
      _ccol_cond_var_wait(pool->shutdown_cv, pool->mu);
    }
    _ccol_mutex_unlock(pool->mu);
    return;
  }
  __atomic_store_n(&pool->shutdown_drain, true, __ATOMIC_RELAXED);
  pool->shutdown_started = true;
  /* Wake the workers blocked on an empty queue so they can read
   * shutdown_drain. */
  _ccol_cond_var_broadcast(pool->not_empty);
  /* Wake the submitters blocked on a full queue so they get
   * ccol_not_permitted. */
  _ccol_cond_var_broadcast(pool->not_full);
  _ccol_mutex_unlock(pool->mu);

  /* foreign_since_fork; see the field comment in struct cthread_pool. This
   * process inherited pool across a fork() call, so it never created any of
   * the threads in threads[] and can never join one of them, and trying to
   * SIGSEGVs inside __pthread_clockjoin_ex in glibc. So we treat every worker
   * as already joined. _ctpool_teardown_raw still frees pool->threads safely
   * later whatever this flag says, because that array is plain data. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      _ccol_thread_join(pool->threads[i].id);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    _ccol_thread_join(pool->threads[i].id);
  }
#endif

  _ctpool_mark_shutdown_complete(pool);
}

void ctpool_shutdown_drain(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* A self-call; see the comment of ctpool_worker_key_bundle above. We treat
   * it as a complete no-op, leaving shutdown_drain and shutdown_started alone
   * on purpose and going no further.
   *
   * Going on would still join every OTHER worker, which is safe in itself,
   * but it could never join the calling thread: _ccol_thread_join on the
   * calling thread's own ID returns EDEADLK at once instead of blocking, and
   * nothing checks that return value. A shutdown_started set here would then
   * keep every LATER, legitimate external shutdown_drain, shutdown_immediate
   * or destroy call from retrying that join, because both internal helpers
   * below turn into a no-op at once when shutdown_started is already true,
   * and the OS resources of that one worker thread would leak for the rest of
   * the process.
   *
   * Since every flag stays untouched, a later, correct call from outside
   * still does a real and complete shutdown. By then this worker has long
   * gone back to its idle wait and is an ordinary worker that can be joined
   * again. */
  if (_ctpool_is_self_call(raw)) {
    _ctpool_resolve_unpin(raw);
    return;
  }
  _ctpool_shutdown_drain_internal(raw);
  _ctpool_resolve_unpin(raw);
}

/* The internal shutdown_immediate body, taking a raw pointer. The comment of
 * _ctpool_shutdown_drain_internal above explains why the split exists. */
static void _ctpool_shutdown_immediate_internal(cthread_pool *pool) {
  _ctpool_lock_mu(pool);
  if (pool->shutdown_started) {
    /* A shutdown is already under way, and two things still need to happen
     * here; skipping either one breaks this function's documented contract
     * for every caller but the first.
     *
     * The first is the escalation. If the shutdown already running is a
     * DRAIN, the queued tasks it is about to run are exactly what this call
     * promises to discard, and their futures are exactly what it promises to
     * cancel. Setting shutdown_immediate and stealing the queue here is what
     * makes "immediate wins over drain" true. Without it, asking for an
     * immediate shutdown while a drain is in flight silently gets you drain
     * behaviour, with nothing discarded and no future cancelled.
     *
     * The second is the block. shutdown_started is set before a single worker
     * is woken, and long before one is joined, so returning on that flag
     * alone would hand the caller a pool whose workers are all still running,
     * against the documented promise to block until every worker thread
     * exits. */
    if (!pool->shutdown_immediate) {
      __atomic_store_n(&pool->shutdown_immediate, true, __ATOMIC_RELAXED);
      ctpool_task *escalated = steal_queue(pool);
      /* The discarded tasks are completed with pool->mu unlocked (see
       * _ctpool_discard_tasks), the same way as on the owning path below. */
      _ccol_mutex_unlock(pool->mu);
      _ctpool_discard_tasks(pool, escalated, true);
      _ctpool_lock_mu(pool);
      _ccol_cond_var_broadcast(pool->not_empty);
      _ccol_cond_var_broadcast(pool->not_full);
      /* The queue is gone by now. If active_count is already zero, nobody
       * else is left to broadcast idle_cv for a ctpool_wait caller. */
      if (pool->active_count == 0) {
        _ccol_cond_var_broadcast(pool->idle_cv);
      }
    }
    while (!pool->shutdown_complete) {
      _ccol_cond_var_wait(pool->shutdown_cv, pool->mu);
    }
    _ccol_mutex_unlock(pool->mu);
    return;
  }
  __atomic_store_n(&pool->shutdown_immediate, true, __ATOMIC_RELAXED);
  pool->shutdown_started = true;

  ctpool_task *discarded = steal_queue(pool);
  /* Complete the discarded tasks with pool->mu unlocked; see
   * _ctpool_discard_tasks. */
  _ccol_mutex_unlock(pool->mu);
  _ctpool_discard_tasks(pool, discarded, true);
  _ctpool_lock_mu(pool);

  /* Wake the workers blocked on not_empty so they see shutdown_immediate. */
  _ccol_cond_var_broadcast(pool->not_empty);
  /* Wake the submitters blocked on not_full. */
  _ccol_cond_var_broadcast(pool->not_full);
  /* Wake the ctpool_wait callers when no active task is left. The queue is
   * gone and active_count is already zero, so no task will ever broadcast
   * idle_cv and we have to do it here. */
  if (pool->active_count == 0) {
    _ccol_cond_var_broadcast(pool->idle_cv);
  }
  _ccol_mutex_unlock(pool->mu);

  /* Same guard and comment as in _ctpool_shutdown_drain_internal. */
#if CCOL_FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      _ccol_thread_join(pool->threads[i].id);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    _ccol_thread_join(pool->threads[i].id);
  }
#endif

  _ctpool_mark_shutdown_complete(pool);
}

void ctpool_shutdown_immediate(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* A self-call. The identical guard and comment in ctpool_shutdown_drain
   * above apply here unchanged, and it keeps a worker thread from going
   * unjoined and leaking for good. */
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
  _ctpool_lock_mu(raw);
  size_t n = raw->queue_size;
  _ccol_mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
  return n;
}

size_t ctpool_active_count(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return 0;
  _ctpool_lock_mu(raw);
  size_t n = raw->active_count;
  _ccol_mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
  return n;
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/* Shared by __ctpool_destroy and ctpool_create_mp's rollback after a failed
 * slot acquire. The forward declaration sits above ctpool_create_mp.
 *
 * The waits here come in the OPPOSITE order from the teardown helpers of
 * ccol_event_loop and clru_cache, on purpose. Only a not_full broadcast
 * releases a submit blocked on a full bounded queue, and in the worst case
 * every worker thread is itself stuck, so only the shutdown's broadcast can
 * release a submitter that is blocked and pinned. Running a drain shutdown
 * BEFORE the wait on the pin count makes sure that broadcast has happened,
 * so the wait below always completes; in the other order it could deadlock
 * against a submitter this same function never wakes. On ctpool_create_mp's
 * rollback path no caller ever saw a handle, so there's no slot to wait on
 * and the wait phase returns at once.
 *
 * The non-foreign path below always calls _ctpool_shutdown_drain_internal,
 * without peeking at pool->shutdown_started first. Such a peek could only
 * happen with pool->mu unlocked, while a pinned ctpool_shutdown_drain or
 * ctpool_shutdown_immediate in flight writes the same field under its lock,
 * so the peek would race that write. _ctpool_shutdown_drain_internal is
 * idempotent under its own lock anyway (it becomes a no-op the moment it
 * sees shutdown_started already true), so always calling it costs nothing
 * extra in the ordinary case and needs no unsynchronized read at all. */
#if CCOL_FORK_SAFETY_REQUIRED
/* Clears slots[idx].torn_down; see the field comment of ctpool_slot_t. It's a
 * no-op when idx is CTPOOL_TEARDOWN_NO_SLOT, meaning the pool was never
 * registered in the slot table. Every caller must run it strictly before
 * pool->mu becomes unsafe for _ctpool_atfork_prepare to dereference, which
 * is when pool->mu is destroyed or, on the foreign_since_fork path, when the
 * struct holding it is freed. */
static void _ctpool_teardown_clear_torn_down(uint32_t idx) {
  if (idx == CTPOOL_TEARDOWN_NO_SLOT) return;
  _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  ctpool_slot_t *slot =
      (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  slot->torn_down = false;
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
}
#endif

static void _ctpool_teardown_raw(cthread_pool *pool, uint32_t idx) {
#if !CCOL_FORK_SAFETY_REQUIRED
  (void)idx; /* idx only matters for the torn_down slot bookkeeping below,
                which the compiler drops along with the rest of this
                module's atfork machinery when the builder turns fork
                safety off. */
#endif
#if CCOL_FORK_SAFETY_REQUIRED
  if (atomic_load(&pool->foreign_since_fork)) {
    /* This process inherited `pool` across a fork() call; see the field
     * comment of foreign_since_fork. At the instant of the fork() a really
     * live PARENT-side thread may be blocked on, or otherwise using, each of
     * the pool's synchronization primitives: mu, not_empty, not_full and
     * idle_cv. DESTROYING one of them here is undefined behaviour at best, and
     * for _ccol_cond_var_destroy it HANGS FOREVER: glibc's
     * pthread_cond_destroy waits for an internal waiter reference count,
     * __wrefs, that only a parent-side worker still blocked on this exact
     * condvar in the parent could decrement, and no such thread exists here.
     * That hang is a separate failure from the SIGSEGV of joining a foreign
     * thread, which the guards in _ctpool_shutdown_drain_internal and
     * _ctpool_shutdown_immediate_internal above prevent.
     *
     * None of that OS-level state is ours to release in this process. Its
     * real owner is the PARENT, which tears it down normally, in its own time,
     * when it destroys its own copy of pool. This process frees only its own
     * private copy-on-write bookkeeping memory and leaves every
     * synchronization primitive alone. From this process's point of view that
     * memory is leaked on purpose, while the parent owns and releases the real
     * primitives.
     *
     * Locking and unlocking pool->mu, unlike destroying it, is safe here.
     * _ctpool_atfork_prepare locks the mu of every live pool before fork() can
     * go on, and _ctpool_atfork_release_impl unlocks each one again before
     * fork() returns to application code; that function also runs in the
     * child, through _ctpool_atfork_child_release. By the time any code in
     * this process can get here, pool->mu is already clean and unlocked,
     * whoever held it in the parent at the instant of the fork().
     *
     * Two of the steps below are needed for real reasons, not just to match
     * the non-foreign path. (1) The pin wait, which comes before the lock and
     * holds no lock itself, is as necessary here as there. A resolve from
     * another thread in THIS process, a concurrent ctpool_pending_count or
     * ctpool_submit racing this exact destroy for example, is an ordinary
     * in-process race that the pin mechanism exists for, and it has nothing
     * to do with the fork. Skipping the pin wait would reopen, for this one
     * code path, the exact resolve-then-use-after-free race that the
     * generation-tagged slot table closes. (2) The lock is needed because
     * steal_queue changes the task list. Any task still sitting in pool->head
     * or pool->tail at this instant is ordinary private copy-on-write heap
     * memory that this process CAN safely free, since unlike the OS-level
     * thread, mutex and condvar state above no worker thread owns it. So we
     * discard such a task the same way ctpool_shutdown_immediate discards a
     * live pool's queue, and future_cancel wakes anyone in this process
     * blocked in ctpool_future_get on one of these tasks; without that, such a
     * caller would hang forever waiting for a worker that never exists here.
     * A PARENT-side worker, gone in this process, may already have taken a
     * task off the queue and been running it at the instant of the fork().
     * That task has no reachable pointer left in this process at all, because
     * it lived only on the stack of that worker, which doesn't exist, so
     * nothing here can recover it. That is a limit of forking with work in
     * flight, not something this mechanism can close. */
    _ctpool_wait_for_pins(pool);
    _ctpool_lock_mu(pool);
    ctpool_task *discarded = steal_queue(pool);
    _ccol_mutex_unlock(pool->mu);
    /* Each discarded task is freed directly with _ccol_mem_free rather than
     * through task_free. By now the pin count is already 0 and this handle's
     * slot is already marked not-in-use (see the ordering inside
     * __ctpool_destroy), so no submit for this pool can ever happen again in
     * this process. Recycling these nodes into task_free_list would only pay
     * task_free's lock and unlock round trip on every discarded task, because
     * the unconditional drain_task_free_list call a few lines below undoes
     * the recycling and nothing could ever observe it. */
    _ctpool_discard_tasks(pool, discarded, false);
    /* This frees whatever this pool's task_free_list already held from
     * BEFORE the fork, the nodes recycled during ordinary operation before
     * the fork. The loop above deliberately goes around that list instead of
     * feeding it. */
    drain_task_free_list(pool);

    _ccol_mem_free(pool->m_procs, pool->threads);
    _ctpool_teardown_clear_torn_down(idx);
    pool_free_self(pool);
    return;
  }
#endif /* CCOL_FORK_SAFETY_REQUIRED */

  _ctpool_shutdown_drain_internal(pool);

  _ctpool_wait_for_pins(pool);

  /* Every worker is joined and no pin is outstanding, so no further submit
   * or task_free call for this pool can happen from here on. This really
   * frees, rather than discards, whatever task_free recycled into
   * task_free_list over the pool's life. Without it those nodes leak,
   * because nothing ever hands them back to m_procs. */
  drain_task_free_list(pool);

  _ccol_mem_free(pool->m_procs, pool->threads);

#if CCOL_FORK_SAFETY_REQUIRED
  _ctpool_teardown_clear_torn_down(idx);
#endif
  _ccol_mutex_destroy(pool->mu);
  _ccol_cond_var_destroy(pool->not_empty);
  _ccol_cond_var_destroy(pool->not_full);
  _ccol_cond_var_destroy(pool->idle_cv);
  _ccol_cond_var_destroy(pool->shutdown_cv);

  pool_free_self(pool);
}

void __ctpool_destroy(ctpool pool) {
  if (!pool) return;

  /* Resolve pool through the slot table and mark the slot not-in-use in the
   * same critical section as the lookup. That's what makes a second destroy
   * of the same handle value, concurrent or a later sequential one, see a
   * resolve failure; without it such a call races this call's teardown. The
   * file-level comment of the slot table and the comment of _ctpool_resolve
   * describe the design. A stale or already-destroyed handle that gets
   * here is exactly the misuse the slot table is there to catch, and it's
   * fatal instead of a silent use-after-free or double-free. */
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  uint32_t idx = (uint32_t)(pool >> 32);
  uint32_t gen = (uint32_t)(pool & 0xFFFFFFFFu);
  _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
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
    _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    _ccol_fatal_err(
        "ctpool_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a ctpool handle)");
  }
  /* A self-call; see the comment of ctpool_worker_key_bundle above. Unlike
   * ctpool_wait, ctpool_shutdown_drain and ctpool_shutdown_immediate there's
   * no safe no-op here. Destroying pool from inside one of its own workers,
   * while that worker is still running, frees pool->mu and the pool struct
   * while the worker is on its way back through worker_thread_fn, where it
   * calls task_free and then locks, decrements, broadcasts and unlocks
   * against the object the destroy just freed. That's a real, deterministic
   * use-after-free, not a rare race.
   *
   * __ctpool_destroy has no ccol_retval_t to report it through, since its
   * documented contract is a live handle in, or a _ccol_fatal_err on misuse.
   * So we treat a self-call exactly like the stale-handle case right above,
   * with a loud, immediate _ccol_fatal_err() instead of quietly skipping and
   * running into undefined behaviour. It's the same as the self-destroy guard
   * of __ccol_event_loop_destroy in cthreadcomm.c. */
  if (_ctpool_is_self_call(raw)) {
    _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    _ccol_fatal_err(
        "ctpool_destroy: called from within a task (or its on_complete "
        "callback) running on this very pool's own worker thread; "
        "destroying it here would free the pool out from under that "
        "still-executing worker");
  }
  slot->in_use = false; /* from this instant ALL later resolves of this handle
                            are blocked, a second, concurrent destroy attempt
                            included */
  /* The same step, under the same lock. From here on no new pin is granted,
   * and that's what lets the count reach zero and stay there. */
  ccol_pintable_retire(&ctpool_pintable, idx);
#if CCOL_FORK_SAFETY_REQUIRED
  /* See the torn_down field comment of ctpool_slot_t. raw's worker threads
   * aren't gone yet; from here on they are only unreachable through this
   * handle. So _ctpool_atfork_prepare needs to keep locking raw->mu across a
   * fork() until _ctpool_teardown_raw clears this flag, right before raw->mu
   * becomes unsafe to touch. */
  slot->torn_down = true;
#endif
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);

  _ctpool_teardown_raw(raw, idx);

  /* Release the slot last, only once raw has been fully torn down and freed.
   * This is the step whose generation bump and push of the index back onto
   * the free list make the handle reusable, and no earlier step does that.
   * Fetch the slot again by idx instead of reusing `slot`: a concurrent
   * ctpool_create_mp can run _ctpool_handle_slot_acquire in between, and its
   * cvector_push_back can move the backing array of slots, which invalidates
   * any pointer into that array taken before this second lock. idx itself is
   * stable. */
  _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  ctpool_slot_t *slot2 =
      (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* moves this slot's generation past whatever value
      the handle of the pool we just freed carried, so that stale handle can
      never match the generation of a LATER acquire of this same index */
  _ctpool_free_index_release_locked(idx);
  /* The process-exit destructor has already run and found this pool live.
     The release it couldn't do belongs to whoever frees the last slot, and
     that may be this call. */
  _ctpool_release_slot_table_if_deferred_locked();
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
/* Resolves h to the cthread_pool* behind it WITHOUT a pin, as a bare
 * slot-table lookup that doesn't touch the pin index at all. That's safe for
 * tests, because the test code calling it runs synchronously and
 * single-threaded, with no concurrent destroy to race. Unlike _ctpool_resolve
 * there's no matching _unpin call for a test to remember, and it's easy to
 * forget one: a forgotten unpin leaves a pin outstanding against that pool
 * forever and hangs every later ctpool_destroy on it. It returns NULL under
 * exactly the same conditions as _ctpool_resolve. */
cthread_pool *_ctpool_resolve_for_tests(ctpool h) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_rw_lock_rdlock(ctpool_slot_table.rwlock);
  cthread_pool *raw = NULL;
  if (idx < cvector_elem_count(ctpool_slot_table.slots)) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return raw;
}

/* The indices sitting on the free list right now. Read it together with
 * _ctpool_slot_table_capacity_for_tests() below. A rollback that loses a
 * slot shows up as a table that grew, but only while the free list was
 * empty, so a test that measures the growth alone passes or fails depending
 * on how many handles earlier tests held at once. Taken together the two
 * numbers describe the table without that dependence. */
size_t _ctpool_free_index_count_for_tests(void) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  _ccol_rw_lock_rdlock(ctpool_slot_table.rwlock);
  size_t n = cvector_elem_count(ctpool_slot_table.free_indices);
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return n;
}

/* Reads pool's shutdown bookkeeping, so a test can wait until one particular
 * shutdown call has taken ownership before starting a second call that must
 * arrive second. It takes a cthread_pool* the test already resolved, like the
 * other test-only accessors in this file. */
void _ctpool_shutdown_state_for_tests(struct cthread_pool *pool, bool *started,
                                      bool *immediate, bool *complete) {
  _ctpool_lock_mu(pool);
  if (started) *started = pool->shutdown_started;
  if (immediate) *immediate = pool->shutdown_immediate;
  if (complete) *complete = pool->shutdown_complete;
  _ccol_mutex_unlock(pool->mu);
}

/* Answers the self-call check for pool on the calling thread, the same check
 * a wait, a shutdown or a destroy of pool consults. A test reads it from
 * inside an on_complete callback before making a call that would block for
 * ever if the answer were wrong. */
bool _ctpool_is_self_call_for_tests(struct cthread_pool *pool) {
  return _ctpool_is_self_call(pool);
}

/* Reads how many slots the ctpool handle table holds right now: the slots
 * the table grew, plus the slots that were freed but not reused yet. It lets
 * a test assert that a loop of creates and destroys reuses freed slots;
 * without the reuse the table would grow without bound. */
size_t _ctpool_slot_table_capacity_for_tests(void) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  _ccol_rw_lock_rdlock(ctpool_slot_table.rwlock);
  size_t n = cvector_elem_count(ctpool_slot_table.slots);
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  return n;
}

/* Reads the current size of pool's task-node free list, so a test can check
 * the recycling (the pops of the submit paths, task_free and
 * drain_task_free_list) directly. Without this accessor a test could only
 * infer what the recycling does from a custom allocator's call counts. It
 * takes the cthread_pool* that _ctpool_resolve_for_tests returns, not a
 * ctpool handle, following the convention of the test-only accessors here,
 * which build on that one resolve step instead of repeating it. */
size_t _ctpool_task_free_list_size_for_tests(cthread_pool *pool) {
  _ctpool_lock_mu(pool);
  size_t n = pool->task_free_list_size;
  _ccol_mutex_unlock(pool->mu);
  return n;
}

/* Reads the cap of pool's task-node free list, which construction sets to
 * max(num_threads * 4, CTPOOL_TASK_FREE_LIST_CEILING) (see the field comment
 * in struct cthread_pool). So a test can assert the bound against the pool
 * it really built, without writing that formula a second time. */
size_t _ctpool_task_free_list_cap_for_tests(cthread_pool *pool) {
  _ctpool_lock_mu(pool);
  size_t n = pool->task_free_list_cap;
  _ccol_mutex_unlock(pool->mu);
  return n;
}

/* Reads how many pins are outstanding against pool's slot. The count lives
 * in the pin index, not in the pool, and is read here with no lock held, so
 * a test can see that a concurrent resolve and pin really happened without
 * any assumption about timing. The alternative would be to assume a fixed
 * sleep was long enough, but a really slow or loaded machine can starve that
 * thread past any fixed bound, which silently reopens the exact
 * resolve-then-use race such a test closes. */
size_t _ctpool_pending_resolve_count_for_tests(cthread_pool *pool) {
  return ccol_pintable_pins_for(&ctpool_pintable, pool->self_handle);
}

/* Waits on pool's not_full until the absolute deadline abs, measured on
 * whatever clock not_full was initialised with, and returns the wait's
 * status. A test passes a CLOCK_MONOTONIC deadline and times the call, so it
 * can tell the condition variable's clock from how it behaves. */
int _ctpool_timedwait_not_full_for_tests(cthread_pool *pool,
                                         const struct timespec *abs) {
  _ctpool_lock_mu(pool);
  int rc = 0;
  do {
    rc = _ccol_cond_var_timedwait(pool->not_full, pool->mu, *abs);
  } while (rc == 0);
  _ccol_mutex_unlock(pool->mu);
  return rc;
}

/* Reads how many times the calling thread has locked any pool's mutex since
 * the thread started. A test reads it before and after one call, and the
 * difference is the number of critical sections the call took on the
 * calling thread, which no worker or other thread can disturb. */
unsigned long _ctpool_mu_locks_for_tests(void) {
  return _ctpool_mu_locks_taken;
}

/* Reads a future's current reference count, under that future's mutex, so a
 * test can wait until a concurrent ctpool_future_get has really taken its own
 * waiter reference, without any assumption about timing. The alternative is
 * to assume a fixed delay was long enough for that thread to get there; such
 * a test lets the future's task complete while a waiter is still on its way
 * in, which uses a pointer whose last reference is already gone, and no
 * reference count can make that safe.
 */
int _ctpool_future_refcount_for_tests(ctpool_future *f) {
  if (!f) return -1;
  _ccol_mutex_lock(f->mu);
  int refcount = f->refcount;
  _ccol_mutex_unlock(f->mu);
  return refcount;
}

/* Test-only. Locks and unlocks the write side of ctpool_slot_table's rwlock
 * directly, going around every public API function. It mirrors the
 * _ccol_event_loop_test_wrlock_reg_slot_for_tests and _wrunlock pair of
 * ccol_event_loop in src/cthreadcomm.c, whose doc comment gives the
 * rationale and describes the AB-BA deadlock a hook of this shape must not
 * introduce.
 *
 * With it a test can hold the write side locked for a window of any length
 * it controls exactly, from a thread OTHER than the one calling fork().
 * prepare() then waits for the window to end before taking the write side
 * itself, and the child goes through the reinitialize path in the in_child
 * branch of _ctpool_atfork_release_impl.
 *
 * Unlike the ccol_event_loop pair, this one has no "resolve" step on
 * purpose, because there's no per-instance handle to resolve here, only the
 * process-wide slot table. So this function returns void and the matching
 * unlock takes no argument. Nothing like ccol_event_loop's AB-BA hazard
 * applies, since neither call touches anything else a concurrent fork() could
 * contend for. */
void ctpool_test_wrlock_slot_table_for_tests(void) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
}

void ctpool_test_wrunlock_slot_table_for_tests(void) {
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
}
#endif

/* Answers whether any slot still names a pool. The caller holds the write
 * lock.
 *
 * The scan reads slot->ptr, not slot->in_use. A destroy clears in_use as its
 * first step, so a second destroy or a new resolve gets rejected as early as
 * possible, and the rest of the teardown comes after that: it joins the
 * threads, drains the pins and does the final locked release of the index. A
 * scan that trusted in_use alone would free this table under a destroy still
 * in that window, and the last step of that destroy would then index the
 * freed table. ptr is written only once a slot is fully acquired and cleared
 * only in that final locked step, so it's set for exactly as long as the
 * table must not be released, and it depends on no build-time switch. */
static bool _ctpool_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(ctpool_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* The check and the release both sit behind one out-of-line call, so the
 * destroy path that makes that call keeps the code shape it would have
 * without any of this. Cold code in a hot object file isn't free: inlined,
 * the same handful of instructions measurably slows an unrelated container's
 * push path, because it shifts what the linker lays out around that path,
 * even though the instruction count doesn't change. */
static __attribute__((noinline)) void
_ctpool_release_slot_table_if_deferred_locked(void) {
  if (ctpool_slot_table.release_deferred && !_ctpool_any_slot_live_locked()) {
    _ctpool_release_slot_table_locked();
  }
}

/* Releases the table's bookkeeping and the pin index. The caller holds the
 * write lock and has already established that no slot is live.
 *
 * The pin index's slot storage is never released while the process runs, on
 * purpose, because a resolve indexes it with no lock held, so a leak checker
 * that counts still-reachable memory as an error reports that storage at
 * exit unless this function releases it. Each vector is set to NULL as it
 * goes, and that's what makes a later call answer "already released" instead
 * of indexing into a freed vector. The rwlock isn't destroyed, because this
 * can run from an ordinary destroy that still holds that lock.
 *
 * The release also retires the worker key, on either path: the destructor
 * when no pool was left, or the destroy of the last pool when the destructor
 * had to defer. No pool is left, so no worker of this module runs and nothing
 * reads the key again. A thread that outlives a dlclose() then never meets a
 * key of this module, and the next load of the library creates a fresh one.
 * A pool some code creates after this point is refused, because its creation
 * finds live false or its slot acquire finds the released table, and a
 * worker sets the key only while live is true. */
static void _ctpool_release_slot_table_locked(void) {
  cvector_destroy(ctpool_slot_table.slots);
  cvector_destroy(ctpool_slot_table.free_indices);
  ccol_pintable_dispose(&ctpool_pintable);
  ctpool_slot_table.release_deferred = false;
  if (atomic_exchange(&ctpool_worker_key_bundle.live, false))
    _ccol_thread_ls_key_delete(ctpool_worker_key_bundle.key);
}

/* Frees the slot table's bookkeeping arrays at process exit, so the leak-kind
 * report of make memtest doesn't flag them as still reachable. It mirrors the
 * _cleanup_*_slot_table function of ccol_event_loop and of clru_cache
 * exactly; the reasoning is in the comments there, including why this is
 * sound only when the application itself destroyed every ctpool it created
 * before the process exits. This test suite meets that precondition for a
 * clean make memtest.
 *
 * The _ccol_call_once here is mandatory. An __attribute__((destructor))
 * function runs for the whole shared object whatever parts of it the process
 * used, and a process can link this library and never create a single
 * ctpool. Without the _ccol_call_once such a process would lock a mutex here
 * that pthread_mutex_init never initialized. */
__attribute__((destructor)) static void _cleanup_ctpool_slot_table(void) {
  _ccol_call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  /* Nothing to do and nothing safe to touch. Each vector is set to NULL as
     it's destroyed, so a second run of this function stops here instead of
     indexing into a freed vector. */
  if (!ctpool_slot_table.slots) return;
  _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock);
  /* This only runs when nothing is left that could still resolve a handle.
   * The order of one translation unit's destructor against another's isn't
   * ours to decide, and without this check a later destructor still holding
   * a live handle would find the table and the pin index freed under it.
   * When a slot is still live the destructor defers the release to whichever
   * destroy frees the last slot, and nothing skips it, so an application that
   * does destroy its pools leaves nothing behind whatever order the
   * destructors run in. */
  if (_ctpool_any_slot_live_locked()) {
    ctpool_slot_table.release_deferred = true;
    _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
    return;
  }
  _ctpool_release_slot_table_locked();
  _ccol_rw_lock_unlock(ctpool_slot_table.rwlock);
  /* We don't destroy the rwlock itself, on purpose. It has static storage
     duration, so it holds nothing a leak checker reports. The destructor also
     can't own the rwlock's lifetime: the deferred branch above returns with
     the table still live, and the later release runs while holding this very
     lock, so there's no path on which every user is provably done with it. A
     destroy here would leave the other branch's entry points with a read
     lock on a destroyed object, and a stale-handle call that is documented to
     fail cleanly through _ccol_fatal_err would turn into undefined behaviour.
     The order of destructors across translation units isn't ours to decide
     either. */
}

#ifdef RUNNING_UNIT_TESTS
/* Runs the slot table's process-exit cleanup right now. A test calls it in a
 * forked child, with a pool still live, to reach the deferred release. */
void _ctpool_run_exit_cleanup_for_tests(void) { _cleanup_ctpool_slot_table(); }

/* True while the worker key exists. */
bool _ctpool_worker_key_live_for_tests(void) {
  return atomic_load(&ctpool_worker_key_bundle.live);
}
#endif /* RUNNING_UNIT_TESTS */
