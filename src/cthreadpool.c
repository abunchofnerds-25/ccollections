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
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/* ctpool is an opaque value handle (top 32 bits = slot index, bottom 32
 * bits = generation; see include/cthreadpool.h's own doc comment on the
 * typedef), resolved through this table before the underlying struct
 * cthread_pool* is ever touched. This is what lets __ctpool_destroy detect
 * BOTH a concurrent double-destroy (racing another destroy on the same
 * still-live handle) AND a sequential one (a stale handle, from an earlier,
 * already-completed destroy) as a fatal_err rather than a use-after-free/
 * double-free: a slot is marked not-in-use the instant it is released, and
 * its generation is bumped on every reuse, so a stale handle can never
 * alias a later, unrelated pool occupying the same slot index. Mirrors
 * chttpcli_slot_table/chttpsvr_slot_table/event_loop_slot_table/
 * clrucache_slot_table exactly; see src/chttpclient.c's own copy of this
 * comment for the full design rationale. */
typedef struct {
  cthread_pool *ptr;   /* NULL when slot is free */
  uint32_t generation; /* minted fresh on every acquire; monotonic per
                           slot index, starts at 0 (pre-first-use),
                           becomes 1 on first acquire */
  bool in_use;
#if FORK_SAFETY_REQUIRED
  /* True from the exact instant __ctpool_destroy clears in_use until
   * _ctpool_teardown_raw is about to make ptr->mu unsafe to touch (either
   * by destroying it, or, on the foreign_since_fork path, by freeing the
   * struct that embeds it); see _ctpool_atfork_prepare's own doc comment
   * for the real, previously-unprotected hang this closes: in_use alone
   * used to be _ctpool_atfork_prepare's sole signal that ptr->mu is both
   * live and worth locking, but a pool's own worker threads are not
   * actually gone the instant in_use goes false, only once
   * _ctpool_shutdown_drain_internal has finished joining every one of
   * them (a step that runs entirely AFTER in_use is cleared, specifically
   * so a concurrent resolve/second-destroy is rejected as early as
   * possible); a fork() landing anywhere in that join window could
   * previously inherit ptr->mu locked by one of those still-very-real,
   * about-to-be-joined worker threads, with in_use already false telling
   * _ctpool_atfork_prepare there was nothing here worth protecting. Never
   * true while in_use is true (the two are set at different points, never
   * both together); ptr is guaranteed non-NULL and safe to dereference
   * for as long as this is true, since it is cleared (under this same
   * ctpool_slot_table.mutex) strictly before the struct it points to
   * becomes unsafe to touch. */
  bool torn_down;
#endif
} ctpool_slot_t;

static struct {
  mutex_t mutex;
  once_flag_t once;
  cvec slots;        /* cvec of ctpool_slot_t; grows via push_back only,
                         indices permanent once allocated */
  cvec free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
} ctpool_slot_table = {0};

#if FORK_SAFETY_REQUIRED
/* Forward declarations: bodies defined further below, once struct
 * cthread_pool itself is declared (they dereference a live pool's own mu);
 * registered from _ctpool_slot_table_init_globals below, the first point in
 * the file that runs once, lazily, the first time this module is used at
 * all. Mirrors event_loop's own identical forward-declare-then-define-after-
 * the-struct placement in src/cthreadcomm.c exactly.
 *
 * This entire fork()-safety mechanism (these three handlers, their
 * at_fork() registration below, struct cthread_pool's own
 * foreign_since_fork field, and every site that consults it) is compiled
 * out entirely when FORK_SAFETY_REQUIRED is defined to 0; see that macro's
 * own doc comment in common.h. */
static void _ctpool_atfork_prepare(void);
static void _ctpool_atfork_release(void);
static void _ctpool_atfork_child_release(void);
#endif

static void _ctpool_slot_table_init_globals(void) {
  mutex_init(ctpool_slot_table.mutex);
  ctpool_slot_table.slots = cvector_create(sizeof(ctpool_slot_t), NULL);
  if (!ctpool_slot_table.slots)
    fatal_err("ctpool slot table: failed to allocate slots vector");
  ctpool_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!ctpool_slot_table.free_indices)
    fatal_err("ctpool slot table: failed to allocate free-index vector");
#if FORK_SAFETY_REQUIRED
  /* fork() duplicates only the calling thread; see _ctpool_atfork_prepare's
   * own doc comment for the full hazard this closes for BOTH parent() and
   * child() (a still-locked mutex inherited by the child), and
   * _ctpool_atfork_child_release's own doc comment for a second,
   * child-only hazard neither parent() nor a plain shared release function
   * can address (joining worker threads that exist only in the parent). */
  at_fork(_ctpool_atfork_prepare, _ctpool_atfork_release,
          _ctpool_atfork_child_release);
#endif
}

/* Not static: intentionally reachable from other .c files in this library
 * (chttpserver.c) that must guarantee this module's own at_fork() triple is
 * registered BEFORE their own, so that pthread_atfork's LIFO prepare-
 * handler ordering makes the CALLER's own prepare handler run FIRST at
 * every future fork() (i.e. before this module's own prepare handler,
 * _ctpool_atfork_prepare, ever gets a chance to lock ctpool_slot_table.
 * mutex or any live pool's own mu). Not declared in cthreadpool.h: this is
 * not part of the public API, only a narrow, deliberate escape hatch for a
 * caller that has already read (and must satisfy) this exact ordering
 * requirement; see chttpserver.c's own call site for the full reasoning
 * and the real, TSan-confirmed deadlock this closes. A caller that never
 * uses this function is entirely unaffected: this module's own lazy,
 * call_once-guarded registration happens exactly as it always has,
 * whenever a ctpool is first created on its own. */
void _ctpool_ensure_atfork_registered_before_caller(void) {
  call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
}

/*
 * A single unit of work in the queue.  Future tasks set `future` and leave
 * `on_complete` NULL; callback tasks do the opposite.  `fn_future` shares
 * storage with `fn` via a union so both pointer widths are identical and no
 * cast is needed at the call site.
 */
typedef struct ctpool_task {
  union {
    void (*fn)(void *);         /* callback-style task */
    void *(*fn_future)(void *); /* future-style task   */
  };
  void *arg;
  void (*on_complete)(void *); /* NULL for future tasks */
  ctpool_future *future;       /* NULL for callback tasks */
  struct ctpool_task *next;
} ctpool_task;

/*
 * A future is always heap-allocated with plain malloc/free so that its
 * lifetime is fully independent of the pool's custom allocator.  Two
 * references are created at submit time (caller + queued task); the struct is
 * freed when the last reference is released.
 */
struct ctpool_future {
  mutex_t mu;
  cond_var_t cv;
  void *result;
  bool done;
  bool cancelled;
  int refcount;
};

struct cthread_pool {
  /* Task queue (intrusive singly-linked list) */
  ctpool_task *head;
  ctpool_task *tail;
  size_t queue_size;
  size_t queue_cap; /* 0 = unbounded */

  /* Recycled ctpool_task nodes (a separate intrusive singly-linked list,
   * reusing the same struct's own `next` field), avoiding a malloc/free
   * round trip through m_procs for every single task submission; a real
   * allocator-pressure hot path for workloads that submit large numbers of
   * short-lived tasks. Protected by mu, exactly like every other pool-owned
   * data structure. Bounded by task_free_list_cap so that a one-time burst
   * of submissions followed by a long idle period does not leave the pool
   * holding an unboundedly large amount of cached memory forever: with no
   * cap, the list could only ever grow (a genuinely idle pool never calls
   * task_alloc again to shrink it) up to the total number of tasks that
   * were ever in flight at once, which for an unbounded queue has no
   * inherent bound at all. task_free_list_cap is set once at construction
   * (num_threads * 4: enough to smooth over ordinary burstiness in how
   * closely allocation and release track each other, without holding onto
   * a multiple of the pool's own configured concurrency large enough to
   * matter). A node sitting in this list was only recycled, never actually
   * handed back to m_procs, so it must be genuinely freed (via
   * drain_task_free_list, not simply discarded) during pool teardown. */
  ctpool_task *task_free_list;
  size_t task_free_list_size;
  size_t task_free_list_cap;

  /* Synchronisation */
  mutex_t mu;
  cond_var_t not_empty; /* workers wait here when idle              */
  cond_var_t not_full;  /* submitters wait here when queue is full  */
  cond_var_t idle_cv;   /* ctpool_wait waits here                   */

  /* Live counters */
  size_t active_count; /* tasks currently being executed */

  /* Shutdown state */
  bool shutdown_drain;     /* new submissions rejected; drain then stop */
  bool shutdown_immediate; /* stop after current tasks; discard queue   */
  bool shutdown_started;   /* either shutdown has been initiated        */

  /* Worker threads */
  thread_id_t *threads;
  size_t num_threads;

  ccol_memmgmt_procs_t *m_procs;

  /* Pinned by _ctpool_resolve (lock-free atomic increment) for as long as
   * some caller holds a just-resolved cthread_pool* it hasn't yet released
   * via _ctpool_resolve_unpin. __ctpool_destroy blocks until this reaches 0
   * (via pin_cv, under mu) before freeing the object, closing a real
   * resolve-then-use race a naive "look up, unlock, return the pointer"
   * resolve step would otherwise leave open. Reuses this pool's own mu for
   * the unpin side's decrement+broadcast (unlike event_loop's fully
   * lock-free pin, this module is already a single-global-mutex design, so
   * this introduces no new contention beyond what every entry point
   * already pays today). */
  _Atomic size_t pending_resolve_count;
  cond_var_t pin_cv; /* wakes __ctpool_destroy's wait; shares mu */

#if FORK_SAFETY_REQUIRED
  /* Set to true, exclusively by this process's own CHILD-side fork handler
   * (_ctpool_atfork_child_release), for every pool still marked in_use at
   * the moment of fork(). fork() duplicates only the calling thread, so
   * every one of this pool's worker threads exists, from this process's
   * own point of view, only as inert, copy-on-write memory: no execution
   * context for any of them ever existed here. Once true,
   * _ctpool_shutdown_drain_internal/_ctpool_shutdown_immediate_internal
   * must never call thread_join on threads[i] (undefined behaviour: the
   * target was never created by, and can never be joined by, this
   * process); confirmed via a standalone reproduction (fork a process
   * with a live, multi-worker ctpool, e.g. an event_loop configured with
   * num_reactor_threads > 1, then destroy/shut down the inherited pool
   * in the child) to reliably (5/5) SIGSEGV inside glibc's own
   * __pthread_clockjoin_ex. Never true for a pool actually created (via
   * create_cthread_pool_mp) in this process. Compiled out entirely when
   * FORK_SAFETY_REQUIRED is 0 (see that macro's own doc comment in
   * common.h): every site that would otherwise consult this field instead
   * unconditionally takes the same path it already takes when this field
   * is false, since a pool can never legitimately be "foreign" in a build
   * with no fork()-handling machinery to ever mark one as such. */
  _Atomic bool foreign_since_fork;
#endif
};

/* ========================================================================== */
/*                         FORK SAFETY (pthread_atfork)                       */
/* ========================================================================== */

#if FORK_SAFETY_REQUIRED
/* fork() duplicates only the calling thread; any lock some OTHER thread held
 * at that instant is inherited by the child in a permanently locked state,
 * since no thread survives in the child that could ever unlock it.
 * ctpool_slot_table.mutex (process-wide, taken by every
 * create_cthread_pool_mp/__ctpool_destroy/ctpool_submit/.../_ctpool_resolve
 * call) and each still-live pool's own mu (taken by every submit/dequeue/
 * shutdown/wait call, including by a worker thread picking up or finishing a
 * task) are therefore both taken here, in prepare(), before fork() is
 * allowed to proceed (so fork() only ever completes once no thread is
 * transiently holding one of them), and released again in both parent() and
 * child() via the same function: every mutex in this module uses the
 * default ("normal") pthread mutex type, which does no owner/TID tracking on
 * Linux glibc, so a plain pthread_mutex_unlock is well-defined even when
 * called by a thread other than whichever one originally locked it (which,
 * for anything the forking thread itself did not hold, no longer exists in
 * the child at all).
 *
 * Mirrors event_loop's own identical atfork fix in src/cthreadcomm.c
 * exactly (see that module's own comment for the full account of two real,
 * reproduced hangs this same shape of fix closes: a fresh create call
 * inheriting its own process-wide slot-table mutex already locked, and an
 * ordinary per-object call inheriting a still-live object's own lock already
 * locked). This module needed the identical fix independently: event_loop's
 * own dispatch_pool (created whenever a caller configures
 * num_reactor_threads > 1) is exactly such a pool, and neither event_loop's
 * own atfork handler nor anything else in this codebase protected it before
 * this fix, since event_loop has no way to reach into this module's own
 * opaque ctpool internals from outside. Registering the fix here instead,
 * scoped to every ctpool this module has ever created (not just
 * event_loop's), closes it for every caller of this module, not only
 * event_loop's own usage of it.
 *
 * Walks every slot with in_use == true (mirroring the exact condition
 * _ctpool_resolve itself already trusts as the sole indicator that
 * slot->ptr is safe to dereference for ordinary resolve purposes) AND every
 * slot with torn_down == true: __ctpool_destroy clears in_use (under this
 * same ctpool_slot_table.mutex) BEFORE doing any of its own, possibly slow,
 * teardown work (joining every worker thread, freeing the task queue), so a
 * pool's own worker threads are not actually gone the instant in_use goes
 * false, only once that join phase completes; torn_down stays true for
 * exactly that window (see ctpool_slot_t's own field comment), so a fork()
 * landing while a not-yet-joined worker thread still holds ptr->mu is
 * caught here exactly like an ordinary live pool's mu would be. ptr is only
 * actually freed, and slot->ptr finally cleared, well after both in_use and
 * torn_down have gone false; a pool past that point is, by this file's own
 * established convention, already off-limits for any purpose, fork-related
 * or not (not a new gap this fix introduces).
 *
 * Deliberately does NOT extend to any individual ctpool_future's own mu.
 * Unlike a pool (reachable from ctpool_slot_table, this module's own
 * complete, walkable registry of every live pool), a future has no
 * equivalent registry once it is off the task queue: ctpool_submit_future
 * hands the caller the only reference to it that survives past the pool's
 * own task list, so discovering "every live future" at fork time would need
 * a new, dedicated, separately-locked global registry, a materially larger
 * change than this fix's own demonstrated scope; mirrors event_loop's own,
 * identically-reasoned exclusion of its own per-entry dispatch_lock (see
 * that module's own comment for the analogous "can only be discovered by
 * walking a structure this fix does not already hold the right lock for"
 * argument). A fork() landing inside the brief window worker_thread_fn/
 * future_deref/future_cancel/ctpool_future_fulfill hold a future's own mu is
 * therefore a real, but narrow and, per this module's own established
 * locking discipline (every future->mu critical section is a handful of
 * instructions, never held across a callback or a blocking wait), low-
 * probability gap, left open rather than silently declared fixed. */
static void _ctpool_atfork_prepare(void) {
  mutex_lock(ctpool_slot_table.mutex);

  size_t n = cvector_elem_count(ctpool_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    mutex_lock(slot->ptr->mu);
  }
}

/* Shared by both parent() and child(); see _ctpool_atfork_prepare's own doc
 * comment for why a plain unlock (not a reinit) is correct in both branches
 * for this module's mutexes. Safe to re-walk the identical structure
 * prepare() just walked and release every lock symmetrically: nothing could
 * have mutated the slot table or any live pool's own state in between,
 * since every lock that would be needed to do so is still held at this
 * exact point.
 *
 * is_child additionally marks every still-live pool as foreign_since_fork
 * and resets its own pending_resolve_count; see
 * _ctpool_atfork_child_release's own doc comment for why both are needed
 * in the child specifically, and struct cthread_pool's own
 * foreign_since_fork field comment for the SIGSEGV this closes. Neither
 * step applies to the parent (nothing was forked away from ITS point of
 * view: every one of its own threads, and every pin any of them held, is
 * exactly as it was immediately before fork() was called). */
static void _ctpool_atfork_release_impl(bool is_child) {
  size_t n = cvector_elem_count(ctpool_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, i);
    if (!slot->in_use && !slot->torn_down) continue;
    cthread_pool *pool = slot->ptr;

    /* A torn_down (already not-in_use) slot's own destroy is already
     * in progress in THIS process: it can never be resolved again (in_use
     * is already false) and so, unlike a genuinely still-live pool, has no
     * way to reach _ctpool_teardown_raw's foreign_since_fork branch a
     * second time in the child. Its own destroying thread simply no
     * longer exists there (unless it happened to be the forking thread
     * itself), leaving this pool's memory harmlessly unreachable for the
     * rest of the child's life; only unlocking its mu (done unconditionally
     * below, for both branches) is needed to close the hang this whole
     * mechanism exists to prevent. */
    if (slot->in_use && is_child) {
      atomic_store(&pool->foreign_since_fork, true);
      /* A now-vanished parent-side thread may have been mid-resolve (a
       * pinned _ctpool_resolve call) at the instant of fork(), leaving
       * pending_resolve_count permanently nonzero from this process's
       * own point of view: nothing here can ever run the matching
       * _ctpool_resolve_unpin call that vanished thread would have
       * made. _ctpool_teardown_raw's own wait loop blocks on this
       * reaching 0 before doing anything else, so left untouched this
       * would hang the child's own destroy forever, the same class of
       * hang an inherited locked mutex used to cause (see
       * _ctpool_atfork_prepare's own history above). Resetting it here
       * is safe for the overwhelmingly common case (no thread in this
       * process is itself already resolving this exact pool's handle
       * across this exact fork() call); the one narrow case this does not
       * cover (the forking thread's OWN resolve still in flight across its
       * own fork() call) is an inherent limitation of calling fork() from
       * inside a held pin at all, not a regression this introduces. */
      atomic_store(&pool->pending_resolve_count, (size_t)0);
    }

    mutex_unlock(pool->mu);
  }

  mutex_unlock(ctpool_slot_table.mutex);
}

static void _ctpool_atfork_release(void) { _ctpool_atfork_release_impl(false); }

/* Child-side counterpart to _ctpool_atfork_release: releases the identical
 * locks (see _ctpool_atfork_release_impl's own comment), but additionally
 * marks every inherited pool's own worker threads as permanently gone from
 * this process's point of view. Must run before any application code in
 * this process can possibly reach one of these pools' own shutdown/destroy
 * path: pthread_atfork's child handler runs synchronously, as part of
 * fork() itself returning, strictly before fork()'s return value ever
 * reaches the calling code. */
static void _ctpool_atfork_child_release(void) {
  _ctpool_atfork_release_impl(true);
}
#endif /* FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                    CTPOOL HANDLE RESOLVE / UNPIN                           */
/* ========================================================================== */

/* Resolves h and pins the result against concurrent destroy, or returns NULL
 * if h is 0, garbage, or references a currently-free or already-reused
 * (wrong-generation) slot. On success, the caller MUST call
 * _ctpool_resolve_unpin(result) exactly once, as soon as it is done touching
 * the resolved cthread_pool*. */
static cthread_pool *_ctpool_resolve(ctpool h) {
  call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  mutex_lock(ctpool_slot_table.mutex);
  cthread_pool *raw = NULL;
  if (idx < cvector_elem_count(ctpool_slot_table.slots)) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* Lock-free: no raw->mu acquisition here at all, so nothing can ever
   * block while ctpool_slot_table.mutex is held; matches chttpcli's own
   * _chttpcli_resolve reasoning exactly. Safe because raw is guaranteed
   * still-allocated here regardless: the only thing that could make it
   * unsafe to touch, __ctpool_destroy's slot-release step, also requires
   * ctpool_slot_table.mutex, which we still hold at this exact point. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  mutex_unlock(ctpool_slot_table.mutex);
  return raw;
}

static void _ctpool_resolve_unpin(cthread_pool *raw) {
  /* The decrement itself MUST happen under raw->mu, not as a bare atomic op
   * outside it: see _chttpcli_resolve_unpin's own comment in
   * src/chttpclient.c for the full account of the lost-wakeup use-after-free
   * an earlier draft of that exact function had, which this mirrors
   * exactly. */
  mutex_lock(raw->mu);
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
  cond_var_broadcast(raw->pin_cv); /* wake a destroy waiting on this */
  mutex_unlock(raw->mu);
}

/* Allocates a fresh slot (or reuses a freed one) for pool and returns the
 * resulting handle, or 0 on OOM. Called once, from create_cthread_pool_mp,
 * after the object is otherwise fully constructed (including every worker
 * thread already running). */
static ctpool _ctpool_handle_slot_acquire(cthread_pool *pool) {
  call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  mutex_lock(ctpool_slot_table.mutex);
  uint32_t idx;
  ctpool_slot_t *slot;
  if (cvector_elem_count(ctpool_slot_table.free_indices) > 0) {
    cvector_pop_back(ctpool_slot_table.free_indices, &idx);
    slot = (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  } else {
    ctpool_slot_t fresh = {0};
    if (cvector_push_back(ctpool_slot_table.slots, &fresh) != ccol_success) {
      mutex_unlock(ctpool_slot_table.mutex);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(ctpool_slot_table.slots) - 1;
    slot = (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  }
  slot->generation++;
  /* Skip the one generation value that would collide with the reserved
   * "invalid handle" sentinel (0) after ~2^32 reuses of this exact slot
   * index; see chttpcli_handle_slot_acquire's identical guard for the full
   * rationale. */
  if (slot->generation == 0) slot->generation++;
  slot->ptr = pool;
  slot->in_use = true;
#if FORK_SAFETY_REQUIRED
  slot->torn_down = false; /* always already false by the time a slot is
                               reused (see ctpool_slot_t's own field
                               comment); reset explicitly anyway, defensively,
                               rather than relying on that invariant alone */
#endif
  ctpool h = ((ctpool)idx << 32) | (ctpool)slot->generation;
  mutex_unlock(ctpool_slot_table.mutex);
  return h;
}

/* ========================================================================== */
/*                    WORKER SELF-CALL DETECTION                              */
/* ========================================================================== */

/* Process-wide thread-local key recording which cthread_pool (if any) the
 * calling thread is a worker thread of; set once, at worker_thread_fn's own
 * entry, and never reassigned or cleared for the rest of that thread's
 * life. A ctpool worker thread is always privately owned by exactly one
 * pool instance for its entire lifetime (ctpool always spawns and owns its
 * own dedicated OS threads, never shared across pools), so a single "which
 * pool am I" value suffices; there is no per-job set/clear lifecycle to
 * manage here the way event_loop's own analogous event_loop_job_key_bundle
 * needs in cthreadcomm.c (a dispatch_pool worker there is shared across many
 * jobs, from potentially many different registrations, over its lifetime).
 *
 * Exists to detect a task (or its on_complete callback) calling
 * ctpool_wait/_shutdown_drain/_shutdown_immediate/__ctpool_destroy on the
 * very pool it is currently executing on. worker_thread_fn receives `pool`
 * as a bare pointer captured once at thread start, entirely independent of
 * the resolve/pin mechanism every public API entry point otherwise goes
 * through, so nothing about pending_resolve_count reflects "a worker of
 * this pool is still using it": the only thing that normally keeps a
 * worker's continued use of `pool` safe across a shutdown is
 * _ctpool_shutdown_drain_internal/_immediate_internal's own thread_join
 * loop, which blocks until every worker has genuinely finished and
 * returned from worker_thread_fn before any teardown proceeds. That
 * guarantee silently breaks for a self-call: pthread_join on the calling
 * thread's own id returns EDEADLK immediately rather than blocking (POSIX
 * leaves this case undefined; glibc's own pthread_join(3) documents
 * detecting and rejecting it this way instead), and thread_join's own
 * return value is never checked here, so the join loop proceeds to believe
 * every worker has exited while the calling worker is still deep in its
 * own call stack, about to return to worker_thread_fn and keep touching
 * `pool`. For __ctpool_destroy specifically this is a real, deterministic
 * use-after-free: task_free/mutex_lock/pool->active_count--/
 * cond_var_broadcast all run against the just-freed/destroyed pool once the
 * task returns; reproduced directly with a single-worker pool whose sole
 * task calls ctpool_destroy on its own pool. */
static struct {
  thread_ls_key_t key;
  once_flag_t once;
} ctpool_worker_key_bundle = {0};

static void _ctpool_init_worker_key(void) {
  thread_ls_key_create(ctpool_worker_key_bundle.key, NULL);
}

/* True iff the calling thread is one of pool's own worker threads, i.e. this
 * call was made (directly or transitively) from within a task, or that
 * task's on_complete callback, currently executing on pool. */
static bool _ctpool_is_self_call(cthread_pool *pool) {
  call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  return thread_ls_get(ctpool_worker_key_bundle.key) == (void *)pool;
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

/*
 * Allocate a task struct, preferring a node recycled from the pool's own
 * bounded free list over a fresh allocation through the pool's allocator
 * (see struct cthread_pool's own task_free_list field comment for why).
 * A recycled node is explicitly re-zeroed before being handed back, so
 * every existing caller of task_alloc can keep relying on the same
 * fresh-calloc guarantee it always could: several call sites (the plain
 * submit path's own `future` field, alloc_future_task's own `on_complete`
 * field) only ever set a subset of this struct's fields explicitly and
 * depend on the rest already being NULL/zero, a guarantee a raw recycled
 * node (still holding whatever a previous, unrelated task last stored)
 * would otherwise silently break.
 */
static ctpool_task *task_alloc(cthread_pool *pool) {
  mutex_lock(pool->mu);
  ctpool_task *task = pool->task_free_list;
  if (task) {
    pool->task_free_list = task->next;
    pool->task_free_list_size--;
  }
  mutex_unlock(pool->mu);

  if (task) {
    memset(task, 0, sizeof(*task));
    return task;
  }
  return (ctpool_task *)_mem_calloc(pool->m_procs, 1, sizeof(ctpool_task));
}

/*
 * Recycle task into pool's own free list if there is still room under
 * task_free_list_cap.  Caller must already hold pool->mu.  Returns true if
 * task was recycled (nothing further to do); false if the caller must
 * still genuinely free task via the pool's allocator, which the caller must
 * do only AFTER releasing pool->mu (a custom, caller-supplied free function
 * may be arbitrarily slow, so it must never run while other threads could be
 * blocked waiting on this same lock). Factored out of task_free so that
 * worker_thread_fn's own hot completion path can fold this push into the
 * very same critical section as its immediately following active_count--
 * update, rather than paying for two separate lock/unlock round trips on
 * pool->mu for what is, from the pool's own point of view, one indivisible
 * "this task is done" event.
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
 * Release a task struct: recycles it into the pool's own free list while
 * there is still room under task_free_list_cap, otherwise genuinely frees
 * it through the pool's allocator.
 */
static void task_free(cthread_pool *pool, ctpool_task *task) {
  mutex_lock(pool->mu);
  bool recycled = task_release_locked(pool, task);
  mutex_unlock(pool->mu);

  if (!recycled) _mem_free(pool->m_procs, task);
}

/*
 * Genuinely frees (via the pool's own allocator) every node currently
 * cached in the pool's task free list, as opposed to task_free, which may
 * merely recycle a node into that same list instead of freeing it. Must be
 * called during pool teardown, once no further task_alloc/task_free call
 * for this pool is possible (every worker already joined and every pinned
 * resolve already released, exactly the same precondition _ctpool_teardown_
 * raw's own thread/memory cleanup already depends on); otherwise a node
 * only ever recycled, never actually handed back to m_procs, would leak.
 * No locking: by the time this runs, nothing else can still be touching
 * this pool's own task_free_list.
 */
static void drain_task_free_list(cthread_pool *pool) {
  ctpool_task *t = pool->task_free_list;
  pool->task_free_list = NULL;
  pool->task_free_list_size = 0;
  while (t) {
    ctpool_task *next = t->next;
    _mem_free(pool->m_procs, t);
    t = next;
  }
}

/*
 * Pre-check for try-submit paths: acquire the lock, inspect shutdown and (for
 * bounded queues) capacity, then release.  Called BEFORE allocating the task
 * node so that a full queue returns ccol_container_full rather than
 * ccol_not_enough_memory, and a shutdown in progress returns ccol_not_permitted
 * without a wasted allocation.  submit_internal repeats the checks under the
 * lock; the TOCTOU window is harmless because submit_internal always returns
 * the correct code for the state it observes.
 */
static ccol_retval_t try_precheck(cthread_pool *pool) {
  mutex_lock(pool->mu);
  ccol_retval_t r;
  if (pool->shutdown_drain || pool->shutdown_immediate) {
    r = ccol_not_permitted;
  } else if (pool->queue_cap > 0 && pool->queue_size >= pool->queue_cap) {
    r = ccol_container_full;
  } else {
    r = ccol_success;
  }
  mutex_unlock(pool->mu);
  return r;
}

/* Enqueue a task. Caller must hold pool->mu. */
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

/* Dequeue the head task. Caller must hold pool->mu. Queue must be non-empty. */
static ctpool_task *dequeue(cthread_pool *pool) {
  ctpool_task *task = pool->head;
  pool->head = task->next;
  if (!pool->head) pool->tail = NULL;
  pool->queue_size--;
  return task;
}

/*
 * Destroys and frees f once the caller has already observed its refcount
 * reach 0 (via a decrement made under f->mu, already released by the time
 * this is called). Shared tail for every one of this file's four "drop a
 * reference, free on last release" sites (future_deref, future_cancel, the
 * worker's own inline future-completion code in worker_thread_fn, and
 * ctpool_future_fulfill), which otherwise each repeated this identical
 * three-line sequence.
 */
static void future_destroy_if_unreferenced(ctpool_future *f, int remaining) {
  if (remaining == 0) {
    mutex_destroy(f->mu);
    cond_var_destroy(f->cv);
    free(f);
  }
}

/*
 * Decrement a future's refcount.  Frees the future when the count reaches 0.
 * Caller must NOT hold future->mu; this function acquires and releases it.
 */
static void future_deref(ctpool_future *f) {
  mutex_lock(f->mu);
  int remaining = --f->refcount;
  mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

/*
 * Cancel a future: mark it done+cancelled, broadcast to any waiting
 * ctpool_future_get callers, then release the task's reference.
 */
static void future_cancel(ctpool_future *f) {
  mutex_lock(f->mu);
  f->cancelled = true;
  f->done = true;
  cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
}

/*
 * Atomically steal the entire task list from the queue.  Caller must hold
 * pool->mu.  The returned list must be cancelled and freed after the caller
 * releases pool->mu (future_cancel acquires future->mu, which must not nest
 * under pool->mu).
 */
static ctpool_task *steal_queue(cthread_pool *pool) {
  ctpool_task *list = pool->head;
  pool->head = NULL;
  pool->tail = NULL;
  pool->queue_size = 0;
  return list;
}

/*
 * Compute the absolute deadline for timed_submit.  Returns false if
 * clock_gettime fails.
 *
 * rel is a caller-supplied struct timespec (ctpool_timed_submit/
 * _timed_submit_future's own `timeout` parameter), unlike every other
 * deadline-computation helper elsewhere in this codebase (chttp1_parser.c,
 * chttpserver.c, chttpclient.c, cthreadcomm.c's own timeout-in-milliseconds
 * variants), all of which derive their own tv_nsec purely from `% 1000` on a
 * plain, non-negative millisecond count and can therefore never see
 * anything outside [0, 999999999] in the first place. A directly
 * caller-supplied timespec carries no such guarantee: POSIX never itself
 * produces one with tv_nsec outside that range, but nothing stops a caller
 * from handing in one that is not (e.g. the result of subtracting two
 * timespecs to compute a remaining budget, without separately normalising
 * that subtraction's own result). rel->tv_nsec < 0 previously flowed
 * straight through into abs_out, itself then also possibly negative
 * (undefined behaviour per POSIX for the struct timespec ultimately handed
 * to cond_var_timedwait), since only the overflow direction (>= 1e9) was
 * ever normalised here. This is the identical hazard cthreadcomm.c's own
 * add_duration_to_timespec already documents and normalises for (see that
 * function's own doc comment for the full account); rel is normalised
 * independently first, using the identical borrow-based technique, before
 * being added to the already-valid (OS-guaranteed in-range) result of
 * clock_gettime, rather than importing a dependency on cthreadcomm.c for
 * one small, self-contained utility function.
 */
static bool make_abs_deadline(const struct timespec *rel,
                              struct timespec *abs_out) {
  if (clock_gettime(CLOCK_REALTIME, abs_out) != 0) return false;

  struct timespec r = *rel;
  if (r.tv_nsec >= 1000000000L) {
    r.tv_sec += r.tv_nsec / 1000000000L;
    r.tv_nsec %= 1000000000L;
  } else if (r.tv_nsec < 0) {
    long borrow = (-r.tv_nsec + 1000000000L - 1) / 1000000000L;
    r.tv_sec -= borrow;
    r.tv_nsec += borrow * 1000000000L;
  }

  /* Both operands are now individually in [0, 999999999], so their sum can
   * overflow by at most one whole second; a single check suffices. */
  abs_out->tv_sec += r.tv_sec;
  abs_out->tv_nsec += r.tv_nsec;
  if (abs_out->tv_nsec >= 1000000000L) {
    abs_out->tv_sec++;
    abs_out->tv_nsec -= 1000000000L;
  }
  return true;
}

/* ========================================================================== */
/*                         WORKER THREAD                                      */
/* ========================================================================== */

static void *worker_thread_fn(void *arg) {
  cthread_pool *pool = (cthread_pool *)arg;

  /* Published once, before this thread can possibly run any task (and
   * therefore before any code it runs could possibly call back into this
   * module at all); see ctpool_worker_key_bundle's own comment above for
   * why this needs no further set/clear for the rest of this thread's
   * life. */
  call_once(ctpool_worker_key_bundle.once, _ctpool_init_worker_key);
  thread_ls_set(ctpool_worker_key_bundle.key, (void *)pool);

  for (;;) {
    mutex_lock(pool->mu);

    /*
     * Wait until there is something to do or a shutdown condition is met.
     * Wakeup conditions:
     *   - A task was enqueued (queue_size > 0)
     *   - shutdown_immediate was set
     *   - shutdown_drain was set AND the queue is now empty (all done)
     */
    while (pool->queue_size == 0 && !pool->shutdown_immediate &&
           !pool->shutdown_drain) {
      cond_var_wait(pool->not_empty, pool->mu);
    }

    /* Drain-shutdown with empty queue: this worker's job is done. */
    if (pool->shutdown_immediate ||
        (pool->shutdown_drain && pool->queue_size == 0)) {
      mutex_unlock(pool->mu);
      break;
    }

    ctpool_task *task = dequeue(pool);
    pool->active_count++;

    /* Wake a blocked submitter now that we freed a slot (bounded only). */
    if (pool->queue_cap > 0) {
      cond_var_signal(pool->not_full);
    }

    mutex_unlock(pool->mu);

    /* Execute the task. */
    if (task->future) {
      void *result = task->fn_future(task->arg);
      mutex_lock(task->future->mu);
      task->future->result = result;
      task->future->done = true;
      cond_var_broadcast(task->future->cv);
      int remaining = --task->future->refcount;
      mutex_unlock(task->future->mu);
      future_destroy_if_unreferenced(task->future, remaining);
    } else {
      task->fn(task->arg);
      if (task->on_complete) {
        task->on_complete(task->arg);
      }
    }

    /* Folds task's own free-list release into the exact same critical
     * section as the active_count-- update immediately below, rather than
     * two separate lock/unlock round trips on pool->mu for what is a single
     * "this task is done" event; see task_release_locked's own doc comment.
     * The actual _mem_free call (potentially a slow, caller-supplied
     * function) still happens after the lock is released, unchanged. */
    mutex_lock(pool->mu);
    bool recycled = task_release_locked(pool, task);
    pool->active_count--;
    if (pool->active_count == 0 && pool->queue_size == 0) {
      cond_var_broadcast(pool->idle_cv);
    }
    mutex_unlock(pool->mu);
    if (!recycled) _mem_free(pool->m_procs, task);
  }

  return NULL;
}

/* Sentinel `idx` for _ctpool_teardown_raw meaning "pool was never
 * registered in ctpool_slot_table at all" (create_cthread_pool_mp's own
 * slot-acquire-failure rollback path): with no slot to consult, torn_down
 * bookkeeping is neither possible nor needed there, since a handle that was
 * never exposed to any caller cannot be raced by a concurrent fork() through
 * this module's own slot-table-driven atfork mechanism in the first place. */
#define CTPOOL_TEARDOWN_NO_SLOT ((uint32_t)-1)

/* Forward declarations: create_cthread_pool_mp's own slot-acquire-failure
 * rollback path needs the shared teardown helper defined later in this
 * file (right after the shutdown_drain/_immediate internal/public split it
 * itself depends on). */
static void _ctpool_teardown_raw(cthread_pool *pool, uint32_t idx);

/* ========================================================================== */
/*                         CREATION                                           */
/* ========================================================================== */

ctpool create_cthread_pool_mp(size_t num_threads, size_t queue_capacity,
                              ccol_memmgmt_procs_t *mprocs, char **err_str) {
  if (num_threads == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("num_threads must be >= 1");
    return CTPOOL_INVALID;
  }

  if (!ccol_verify_memmgmt_procs(mprocs, err_str)) return CTPOOL_INVALID;

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
    mem_cpy(pool->m_procs, mprocs, sizeof(*mprocs));
  }

  /* Treat ccol_invalid_size as unbounded. */
  pool->queue_cap = (queue_capacity == ccol_invalid_size) ? 0 : queue_capacity;

  if (mutex_init(pool->mu) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("mutex init failed");
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (cond_var_init(pool->not_empty) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    mutex_destroy(pool->mu);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (cond_var_init(pool->not_full) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    mutex_destroy(pool->mu);
    cond_var_destroy(pool->not_empty);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (cond_var_init(pool->idle_cv) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    mutex_destroy(pool->mu);
    cond_var_destroy(pool->not_empty);
    cond_var_destroy(pool->not_full);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  if (cond_var_init(pool->pin_cv) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    mutex_destroy(pool->mu);
    cond_var_destroy(pool->not_empty);
    cond_var_destroy(pool->not_full);
    cond_var_destroy(pool->idle_cv);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }
  atomic_init(&pool->pending_resolve_count, (size_t)0);
#if FORK_SAFETY_REQUIRED
  atomic_init(&pool->foreign_since_fork, false);
#endif

  pool->threads = (thread_id_t *)_mem_calloc(pool->m_procs, num_threads,
                                             sizeof(thread_id_t));
  if (!pool->threads) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate threads array");
    mutex_destroy(pool->mu);
    cond_var_destroy(pool->not_empty);
    cond_var_destroy(pool->not_full);
    cond_var_destroy(pool->idle_cv);
    cond_var_destroy(pool->pin_cv);
    pool_free_self(pool);
    return CTPOOL_INVALID;
  }

  pool->num_threads = num_threads;
  /* See struct cthread_pool's own task_free_list_cap field comment for the
   * rationale behind this specific multiplier. */
  pool->task_free_list_cap = num_threads * 4;

  for (size_t i = 0; i < num_threads; i++) {
    if (thread_create(pool->threads[i], worker_thread_fn, pool) != 0) {
      /* Shut down the threads already started, then clean up. */
      mutex_lock(pool->mu);
      pool->shutdown_drain = true;
      pool->shutdown_started = true;
      cond_var_broadcast(pool->not_empty);
      mutex_unlock(pool->mu);
      for (size_t j = 0; j < i; j++) {
        thread_join(pool->threads[j]);
      }
      if (err_str) *err_str = CCOL_ERR_STR("pthread_create failed");
      _mem_free(pool->m_procs, pool->threads);
      mutex_destroy(pool->mu);
      cond_var_destroy(pool->not_empty);
      cond_var_destroy(pool->not_full);
      cond_var_destroy(pool->idle_cv);
      cond_var_destroy(pool->pin_cv);
      pool_free_self(pool);
      return CTPOOL_INVALID;
    }
  }

  /* Slot acquisition is the LITERAL LAST step, after every worker thread is
   * already running: by this point the pool is otherwise fully
   * constructed, so an OOM here is safe to unwind unilaterally with no
   * double-destroy/visibility concern (the handle was never exposed to any
   * caller). Unlike the earlier failure paths above, every worker thread
   * may already be running at this point, so rollback must actually stop
   * them (not just free memory); _ctpool_teardown_raw does exactly that
   * (shutdown-drain, join every thread, then free), reusing the identical
   * sequence __ctpool_destroy itself uses rather than a hand-rolled
   * variant that could drift out of sync with it. */
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
 * Common path for all three submit variants.  `block` controls whether to
 * wait for a free slot (blocking), never wait (try), or wait with a deadline
 * (timed).  `abs_deadline` is only used when block == 2.
 *
 *   block == 0: try (return ccol_container_full immediately)
 *   block == 1: blocking wait
 *   block == 2: timed wait
 */
static ccol_retval_t submit_internal(cthread_pool *pool, ctpool_task *task,
                                     int block,
                                     const struct timespec *abs_deadline) {
  mutex_lock(pool->mu);

  if (pool->shutdown_drain || pool->shutdown_immediate) {
    mutex_unlock(pool->mu);
    return ccol_not_permitted;
  }

  if (pool->queue_cap > 0) {
    while (pool->queue_size >= pool->queue_cap && !pool->shutdown_drain &&
           !pool->shutdown_immediate) {
      if (block == 0) {
        /* try_submit: return immediately */
        mutex_unlock(pool->mu);
        return ccol_container_full;
      } else if (block == 1) {
        cond_var_wait(pool->not_full, pool->mu);
      } else {
        /* timed_submit */
        int rc = cond_var_timedwait(pool->not_full, pool->mu, *abs_deadline);
        if (rc == ETIMEDOUT) {
          mutex_unlock(pool->mu);
          return ccol_timed_out;
        } else if (rc != 0) {
          mutex_unlock(pool->mu);
          return ccol_unexpected_failure;
        }
      }
    }
    /* Re-check shutdown after waking. */
    if (pool->shutdown_drain || pool->shutdown_immediate) {
      mutex_unlock(pool->mu);
      return ccol_not_permitted;
    }
  }

  enqueue(pool, task);
  cond_var_signal(pool->not_empty);
  mutex_unlock(pool->mu);
  return ccol_success;
}

/* ========================================================================== */
/*                         TASK SUBMISSION                                    */
/* ========================================================================== */

/*
 * Shared body for ctpool_submit/_try_submit/_timed_submit, which otherwise
 * differed only in the block/rel_timeout combination they pass to
 * submit_internal, each repeating the full resolve/validate/allocate/submit/
 * cleanup/unpin sequence around it. block/rel_timeout follow submit_internal's
 * own block convention (0 = try, 1 = blocking, 2 = timed), except that here
 * rel_timeout is the caller's own RELATIVE timeout for block == 2: this
 * function computes the absolute deadline itself (via make_abs_deadline),
 * at the same point in the validation order ctpool_timed_submit's own
 * non-delegating path always has (after resolving pool and validating fn,
 * before allocating the task node), so factoring this out changes no
 * caller-observable behaviour. The try-submit precheck (skip allocating a
 * task node that would just be discarded immediately) applies precisely
 * when block == 0, mirroring the one-to-one correspondence the un-factored
 * code already had between "is this try_submit" and "was try_precheck
 * called".
 */
static ccol_retval_t submit_generic(ctpool pool, void (*fn)(void *), void *arg,
                                    void (*on_complete)(void *), int block,
                                    const struct timespec *rel_timeout) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  struct timespec deadline;
  const struct timespec *abs_deadline = NULL;
  if (block == 2) {
    if (!make_abs_deadline(rel_timeout, &deadline)) {
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

  ctpool_task *task = task_alloc(raw);
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
                            void (*on_complete)(void *)) {
  return submit_generic(pool, fn, arg, on_complete, 1, NULL);
}

ccol_retval_t ctpool_try_submit(ctpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *)) {
  return submit_generic(pool, fn, arg, on_complete, 0, NULL);
}

ccol_retval_t ctpool_timed_submit(ctpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *),
                                  struct timespec *timeout) {
  /* NULL timeout: behave like try_submit. Delegating before ever resolving
   * pool here (rather than resolving once just to immediately unpin and
   * delegate) means the common case now pays for exactly one resolve/unpin
   * pair, not two; ctpool_try_submit's own resolve is what actually
   * validates pool and fn either way, so the observable result for every
   * input (valid or not) is unchanged. */
  if (!timeout) return ctpool_try_submit(pool, fn, arg, on_complete);
  return submit_generic(pool, fn, arg, on_complete, 2, timeout);
}

/* ========================================================================== */
/*                         FUTURES                                            */
/* ========================================================================== */

/*
 * Allocate a future and its linked task in one shot.  On success sets
 * *task_out and returns the future (refcount == 2).  On any allocation
 * failure cleans up and returns NULL.
 */
static ctpool_future *alloc_future_task(cthread_pool *pool, void *(*fn)(void *),
                                        void *arg, ctpool_task **task_out) {
  ctpool_future *f = (ctpool_future *)calloc(1, sizeof(ctpool_future));
  if (!f) return NULL;
  if (mutex_init(f->mu) != 0) {
    free(f);
    return NULL;
  }
  if (cond_var_init(f->cv) != 0) {
    mutex_destroy(f->mu);
    free(f);
    return NULL;
  }
  f->refcount = 2; /* caller reference + task reference */

  ctpool_task *task = task_alloc(pool);
  if (!task) {
    mutex_destroy(f->mu);
    cond_var_destroy(f->cv);
    free(f);
    return NULL;
  }
  task->fn_future = fn;
  task->arg = arg;
  task->future = f;
  *task_out = task;
  return f;
}

/* Undo alloc_future_task when submission fails. */
static void free_future_task(cthread_pool *pool, ctpool_future *f,
                             ctpool_task *task) {
  task_free(pool, task);
  mutex_destroy(f->mu);
  cond_var_destroy(f->cv);
  free(f);
}

/*
 * Shared body for ctpool_submit_future/_try_submit_future/_timed_submit_future,
 * mirroring submit_generic's own factoring above and its identical block/
 * rel_timeout convention. *out is zeroed unconditionally as the very first
 * step, before pool is even resolved: every caller below already guarantees
 * out itself is non-NULL before reaching here (ctpool_try_submit_future/
 * _timed_submit_future check it themselves and return ccol_invalid_args
 * without calling this at all if it is NULL; ctpool_submit_future always
 * passes the address of its own local variable), so this one unconditional
 * assignment is what makes *out reliably NULL on every failure path this
 * function has, matching each public function's own documented contract
 * without needing to repeat that guarantee at every individual return site.
 */
static ccol_retval_t submit_future_generic(ctpool pool, void *(*fn)(void *),
                                           void *arg, int block,
                                           const struct timespec *rel_timeout,
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
    if (!make_abs_deadline(rel_timeout, &deadline)) {
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
  submit_future_generic(pool, fn, arg, 1, NULL, &f);
  return f;
}

ccol_retval_t ctpool_try_submit_future(ctpool pool, void *(*fn)(void *),
                                       void *arg, ctpool_future **out) {
  if (!out) return ccol_invalid_args;
  return submit_future_generic(pool, fn, arg, 0, NULL, out);
}

ccol_retval_t ctpool_timed_submit_future(ctpool pool, void *(*fn)(void *),
                                         void *arg, struct timespec *timeout,
                                         ctpool_future **out) {
  if (!out) return ccol_invalid_args;
  /* NULL timeout: behave like try_submit_future; see ctpool_timed_submit's
   * identical delegation comment above for why calling submit_future_generic
   * directly here (rather than resolving first and delegating to
   * ctpool_try_submit_future) is behaviour-preserving. */
  if (!timeout) return submit_future_generic(pool, fn, arg, 0, NULL, out);
  return submit_future_generic(pool, fn, arg, 2, timeout, out);
}

void *ctpool_future_get(ctpool_future *f) {
  if (!f) return NULL;
  mutex_lock(f->mu);
  while (!f->done) {
    cond_var_wait(f->cv, f->mu);
  }
  void *result = f->result;
  mutex_unlock(f->mu);
  return result;
}

bool ctpool_future_done(ctpool_future *f) {
  if (!f) return false;
  mutex_lock(f->mu);
  bool done = f->done;
  mutex_unlock(f->mu);
  return done;
}

bool ctpool_future_cancelled(ctpool_future *f) {
  if (!f) return false;
  mutex_lock(f->mu);
  bool cancelled = f->cancelled;
  mutex_unlock(f->mu);
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
  if (mutex_init(f->mu) != 0) {
    free(f);
    if (err_str) *err_str = CCOL_ERR_STR("future mutex init failed");
    return NULL;
  }
  if (cond_var_init(f->cv) != 0) {
    mutex_destroy(f->mu);
    free(f);
    if (err_str) *err_str = CCOL_ERR_STR("future cond_var init failed");
    return NULL;
  }
  f->refcount = 2; /* caller reference + producer reference */
  return f;
}

ccol_retval_t ctpool_future_fulfill(ctpool_future *f, void *result) {
  if (!f) return ccol_invalid_args;
  mutex_lock(f->mu);
  if (f->done) {
    mutex_unlock(f->mu);
    return ccol_not_permitted;
  }
  f->result = result;
  f->done = true;
  cond_var_broadcast(f->cv);
  int remaining = --f->refcount;
  mutex_unlock(f->mu);
  future_destroy_if_unreferenced(f, remaining);
  return ccol_success;
}

/* ========================================================================== */
/*                         POOL MANAGEMENT                                    */
/* ========================================================================== */

void ctpool_wait(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* Self-call (see ctpool_worker_key_bundle's own comment above): a task
   * calling ctpool_wait on the very pool it is executing on would deadlock
   * against itself, since the calling task is itself still counted in
   * active_count until it returns. Treated as vacuously idle instead,
   * mirroring how a foreign (post-fork) pool is already treated as
   * vacuously idle a few lines below, for the analogous reason that
   * waiting here can never be satisfied. */
  if (_ctpool_is_self_call(raw)) {
    _ctpool_resolve_unpin(raw);
    return;
  }
  mutex_lock(raw->mu);
  /* A foreign (post-fork-inherited; see struct cthread_pool's own
   * foreign_since_fork field comment) pool's active_count/queue_size can
   * never legitimately reach zero through this process's own actions: every
   * worker thread that could ever process the remaining queue or finish an
   * in-flight task exists only in the vanished parent, so nothing in this
   * process will ever decrement active_count, drain queue_size, or broadcast
   * idle_cv again. Waiting on that condition here would hang forever;
   * treat a foreign pool as vacuously idle instead, mirroring how
   * _ctpool_shutdown_drain_internal/_ctpool_shutdown_immediate_internal
   * already skip trying to join a foreign pool's own (equally nonexistent)
   * worker threads. */
#if FORK_SAFETY_REQUIRED
  if (!atomic_load(&raw->foreign_since_fork)) {
    while (raw->active_count > 0 || raw->queue_size > 0) {
      cond_var_wait(raw->idle_cv, raw->mu);
    }
  }
#else
  while (raw->active_count > 0 || raw->queue_size > 0) {
    cond_var_wait(raw->idle_cv, raw->mu);
  }
#endif
  mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
}

/* Internal (raw-pointer-taking) shutdown_drain body, shared by the public
 * ctpool_shutdown_drain wrapper below and by _ctpool_teardown_raw (used by
 * both __ctpool_destroy and create_cthread_pool_mp's own slot-acquire-
 * failure rollback), mirroring the _chttpsvr_stop_internal pattern already
 * established for chttpsvr_stop. */
static void _ctpool_shutdown_drain_internal(cthread_pool *pool) {
  mutex_lock(pool->mu);
  if (pool->shutdown_started) {
    mutex_unlock(pool->mu);
    return;
  }
  pool->shutdown_drain = true;
  pool->shutdown_started = true;
  /* Wake workers blocked on an empty queue so they can check shutdown_drain. */
  cond_var_broadcast(pool->not_empty);
  /* Wake submitters blocked on a full queue so they receive ccol_not_permitted.
   */
  cond_var_broadcast(pool->not_full);
  mutex_unlock(pool->mu);

  /* foreign_since_fork (see struct cthread_pool's own field comment): this
   * process inherited pool across a fork() call, so none of threads[]
   * was ever created here and none can ever be joined here; confirmed
   * to reliably SIGSEGV inside glibc's own __pthread_clockjoin_ex when
   * attempted. Treat every worker as already, trivially joined instead;
   * pool->threads itself is still safely freed later by
   * _ctpool_teardown_raw regardless of this flag, since it is just a
   * plain data array. */
#if FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      thread_join(pool->threads[i]);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    thread_join(pool->threads[i]);
  }
#endif
}

void ctpool_shutdown_drain(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* Self-call (see ctpool_worker_key_bundle's own comment above): treated as
   * a complete no-op, deliberately leaving shutdown_drain/shutdown_started
   * untouched, rather than proceeding. Proceeding would still need to join
   * every OTHER worker (safe on its own) but could never actually join the
   * calling thread itself (thread_join on one's own id returns EDEADLK
   * immediately instead of blocking, and that return value is never
   * checked); marking shutdown_started here would then permanently prevent
   * any LATER, legitimate external shutdown_drain/shutdown_immediate/destroy
   * call from ever retrying that join (both internal helpers below no-op
   * immediately once shutdown_started is already true), leaking that one
   * worker thread's OS resources for the remaining life of the process.
   * Leaving every flag untouched here means a later, correctly-issued
   * external call still performs a real, complete shutdown once this
   * worker has long since returned to its own idle wait and become an
   * ordinary, joinable-again worker again. */
  if (_ctpool_is_self_call(raw)) {
    _ctpool_resolve_unpin(raw);
    return;
  }
  _ctpool_shutdown_drain_internal(raw);
  _ctpool_resolve_unpin(raw);
}

/* Internal (raw-pointer-taking) shutdown_immediate body; see
 * _ctpool_shutdown_drain_internal's own comment above for why this split
 * exists. */
static void _ctpool_shutdown_immediate_internal(cthread_pool *pool) {
  mutex_lock(pool->mu);
  if (pool->shutdown_started) {
    mutex_unlock(pool->mu);
    return;
  }
  pool->shutdown_immediate = true;
  pool->shutdown_started = true;

  ctpool_task *discarded = steal_queue(pool);
  /* Cancel futures and free tasks without holding pool->mu: future_cancel
   * acquires future->mu, which must not nest under pool->mu. */
  mutex_unlock(pool->mu);
  for (ctpool_task *t = discarded; t;) {
    ctpool_task *next = t->next;
    if (t->future) future_cancel(t->future);
    task_free(pool, t);
    t = next;
  }
  mutex_lock(pool->mu);

  /* Wake workers blocked on not_empty so they see shutdown_immediate. */
  cond_var_broadcast(pool->not_empty);
  /* Wake submitters blocked on not_full. */
  cond_var_broadcast(pool->not_full);
  /* Wake ctpool_wait callers when no active tasks remain: once the queue is
   * discarded and active_count is already zero no task will ever naturally
   * broadcast idle_cv, so we must do it here. */
  if (pool->active_count == 0) {
    cond_var_broadcast(pool->idle_cv);
  }
  mutex_unlock(pool->mu);

  /* See _ctpool_shutdown_drain_internal's identical guard/comment. */
#if FORK_SAFETY_REQUIRED
  if (!atomic_load(&pool->foreign_since_fork)) {
    for (size_t i = 0; i < pool->num_threads; i++) {
      thread_join(pool->threads[i]);
    }
  }
#else
  for (size_t i = 0; i < pool->num_threads; i++) {
    thread_join(pool->threads[i]);
  }
#endif
}

void ctpool_shutdown_immediate(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  /* Self-call: see ctpool_shutdown_drain's identical guard/comment above;
   * the exact same reasoning (avoiding a permanently-leaked, never-joined
   * worker thread) applies here unchanged. */
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
  mutex_lock(raw->mu);
  size_t n = raw->queue_size;
  mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
  return n;
}

size_t ctpool_active_count(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return 0;
  mutex_lock(raw->mu);
  size_t n = raw->active_count;
  mutex_unlock(raw->mu);
  _ctpool_resolve_unpin(raw);
  return n;
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/* Shared by __ctpool_destroy and create_cthread_pool_mp's own slot-acquire-
 * failure rollback (forward-declared above create_cthread_pool_mp).
 *
 * Wait-ordering here is the OPPOSITE of event_loop's/clru_cache's own
 * teardown helpers, deliberately: submit_internal's blocking wait on a full
 * bounded queue is released ONLY by a not_full broadcast, and in the worst
 * case (every worker thread itself stuck) nothing but shutdown's own
 * broadcast would ever release a blocked, pinned submitter. Running
 * shutdown-drain BEFORE waiting on pending_resolve_count guarantees that
 * broadcast has already happened, so the wait below is guaranteed to
 * complete rather than risk deadlocking against a submitter this same
 * function would otherwise never wake. When called from
 * create_cthread_pool_mp's rollback path, pending_resolve_count is
 * provably already 0 (no handle was ever exposed to any caller), so the
 * wait phase there is trivially instant.
 *
 * The non-foreign path below always calls _ctpool_shutdown_drain_internal
 * unconditionally rather than first peeking at pool->shutdown_started: that
 * peek used to happen unguarded (no pool->mu held), racing a concurrently
 * pinned, in-flight ctpool_shutdown_drain/ctpool_shutdown_immediate call's
 * own, properly locked write to the same field. _ctpool_shutdown_drain_internal
 * is already idempotent under its own lock (it no-ops the instant it observes
 * shutdown_started already true), so calling it unconditionally costs nothing
 * extra for the ordinary case and removes the unsynchronized read entirely. */
#if FORK_SAFETY_REQUIRED
/* Clears slots[idx].torn_down (see ctpool_slot_t's own field comment),
 * a no-op when idx is CTPOOL_TEARDOWN_NO_SLOT (the pool was never
 * registered in the slot table to begin with). Must run, in every caller,
 * strictly before pool->mu becomes unsafe for _ctpool_atfork_prepare to
 * dereference: either by being destroyed outright, or, on the
 * foreign_since_fork path, by the struct that embeds it being freed. */
static void _ctpool_teardown_clear_torn_down(uint32_t idx) {
  if (idx == CTPOOL_TEARDOWN_NO_SLOT) return;
  mutex_lock(ctpool_slot_table.mutex);
  ctpool_slot_t *slot =
      (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  slot->torn_down = false;
  mutex_unlock(ctpool_slot_table.mutex);
}
#endif

static void _ctpool_teardown_raw(cthread_pool *pool, uint32_t idx) {
#if !FORK_SAFETY_REQUIRED
  (void)idx; /* only meaningful for the torn_down slot bookkeeping below,
                entirely compiled out along with the rest of this module's
                atfork machinery when fork safety is opted out of. */
#endif
#if FORK_SAFETY_REQUIRED
  if (atomic_load(&pool->foreign_since_fork)) {
    /* This process inherited `pool` across a fork() call (see
     * foreign_since_fork's own field comment): every one of its
     * synchronization primitives (mu, not_empty, not_full, idle_cv,
     * pin_cv) may, at the instant of fork(), have had a genuinely live,
     * still-running PARENT-side thread blocked on or otherwise actively
     * referencing it. Actually DESTROYING one of them here is undefined
     * behaviour at best; confirmed via a standalone reproduction to
     * reliably HANG FOREVER for cond_var_destroy specifically (glibc's
     * own pthread_cond_destroy waits for an internal waiter-reference
     * count, __wrefs, that only a since-vanished parent-side worker
     * thread, itself still genuinely blocked on this exact condvar in
     * the parent, could ever decrement; this is a distinct hang from,
     * and was found only after fixing, the join-on-a-foreign-thread
     * SIGSEGV the guards in _ctpool_shutdown_drain_internal/
     * _ctpool_shutdown_immediate_internal above close). None of that
     * OS-level state is this process's own to release: its real owner
     * remains the PARENT, which will tear it down normally, in its own
     * time, when it destroys its own copy of pool. This process only
     * frees its own, private (copy-on-write) bookkeeping memory instead,
     * leaving every synchronization primitive untouched (deliberately
     * leaked from this process's own point of view; the parent still
     * owns and will release the real ones).
     *
     * Merely LOCKING/UNLOCKING pool->mu, unlike destroying it, is safe here:
     * _ctpool_atfork_prepare locks every live pool's own mu before fork() is
     * allowed to proceed, and _ctpool_atfork_release_impl (run in the child
     * too, via _ctpool_atfork_child_release) unlocks every one of them again
     * before fork() ever returns to application code; so by the time any
     * code in this process can reach this function, pool->mu is guaranteed
     * to already be in a clean, unlocked state, regardless of who held it in
     * the parent at the instant of fork().
     *
     * That lock is still needed here for two real reasons, not merely for
     * symmetry with the non-foreign path below: (1) pending_resolve_count
     * must still be waited on before freeing pool, exactly like the
     * non-foreign path does: a resolve from another thread in THIS
     * process (e.g. a concurrent ctpool_pending_count/ctpool_submit call
     * racing this exact destroy) is a perfectly ordinary, in-process race
     * the pin mechanism exists to protect against, and is entirely
     * independent of anything fork-related; skipping this wait here would
     * reopen the exact resolve-then-use-after-free race the whole
     * generation-tagged slot table redesign exists to close, just for this
     * one code path. (2) any task still sitting in pool->head/pool->tail at
     * this instant is ordinary, private (copy-on-write) heap memory this
     * process CAN safely free (unlike the OS-level thread/mutex/condvar
     * state above, no worker thread's ownership is involved), so it is
     * discarded the same way ctpool_shutdown_immediate already discards a
     * live pool's queue (future_cancel wakes anyone in this process blocked
     * in ctpool_future_get on one of these, rather than leaving it to hang
     * forever waiting for a worker that will never exist here). A task
     * already dequeued and mid-execution by a now-vanished PARENT-side
     * worker at the instant of fork() has no reachable pointer left in this
     * process at all (it lived only on that worker's own, now-nonexistent
     * stack) and so cannot be recovered here; this is an inherent
     * limitation of forking with in-flight work, not something this fix
     * can close. */
    mutex_lock(pool->mu);
    while (atomic_load(&pool->pending_resolve_count) > 0) {
      cond_var_wait(pool->pin_cv, pool->mu);
    }
    ctpool_task *discarded = steal_queue(pool);
    mutex_unlock(pool->mu);
    /* Frees each discarded task directly via _mem_free, deliberately NOT
     * through task_free: by this point pending_resolve_count is already 0
     * and this handle's slot is already marked not-in-use (see
     * __ctpool_destroy's own ordering), so no task_alloc call for this pool
     * can ever happen again, in this process, for the rest of its life.
     * Recycling one of these nodes into task_free_list would therefore only
     * ever be undone a few lines below by the unconditional
     * drain_task_free_list call, paying for task_free's own lock/unlock
     * round trip on every discarded task with no chance of the recycling it
     * performs ever being observed. */
    for (ctpool_task *t = discarded; t;) {
      ctpool_task *next = t->next;
      if (t->future) future_cancel(t->future);
      _mem_free(pool->m_procs, t);
      t = next;
    }
    /* Frees whatever this pool's own task_free_list already held from
     * BEFORE the fork (nodes genuinely recycled during ordinary pre-fork
     * operation), which the loop above deliberately bypasses rather than
     * feeds into. */
    drain_task_free_list(pool);

    _mem_free(pool->m_procs, pool->threads);
    _ctpool_teardown_clear_torn_down(idx);
    pool_free_self(pool);
    return;
  }
#endif /* FORK_SAFETY_REQUIRED */

  _ctpool_shutdown_drain_internal(pool);

  mutex_lock(pool->mu);
  while (atomic_load(&pool->pending_resolve_count) > 0) {
    cond_var_wait(pool->pin_cv, pool->mu);
  }
  mutex_unlock(pool->mu);

  /* Every worker is joined and no pin is outstanding, so no further
   * task_alloc/task_free call for this pool is possible from here on;
   * genuinely free whatever task_free recycled into task_free_list over
   * this pool's lifetime instead of merely discarding it, or those nodes
   * would leak (never actually handed back to m_procs). */
  drain_task_free_list(pool);

  _mem_free(pool->m_procs, pool->threads);

#if FORK_SAFETY_REQUIRED
  _ctpool_teardown_clear_torn_down(idx);
#endif
  mutex_destroy(pool->mu);
  cond_var_destroy(pool->not_empty);
  cond_var_destroy(pool->not_full);
  cond_var_destroy(pool->idle_cv);
  cond_var_destroy(pool->pin_cv);

  pool_free_self(pool);
}

void __ctpool_destroy(ctpool pool) {
  if (!pool) return;

  /* Resolve pool through the slot table, marking the slot not-in-use in
   * the same critical section as the lookup: this is what makes a second,
   * concurrent (or later, sequential) destroy call on the same handle
   * value see a resolve failure rather than racing this call's own
   * teardown; see the slot table's own file-level comment and
   * _ctpool_resolve's comment for the full design. A stale or
   * already-destroyed handle reaching here is exactly the misuse this
   * redesign exists to catch: it is fatal, not a silent use-after-free/
   * double-free. */
  call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  uint32_t idx = (uint32_t)(pool >> 32);
  uint32_t gen = (uint32_t)(pool & 0xFFFFFFFFu);
  mutex_lock(ctpool_slot_table.mutex);
  ctpool_slot_t *slot = NULL;
  cthread_pool *raw = NULL;
  if (idx < cvector_elem_count(ctpool_slot_table.slots)) {
    ctpool_slot_t *s =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    mutex_unlock(ctpool_slot_table.mutex);
    fatal_err(
        "ctpool_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a ctpool handle)");
  }
  /* Self-call (see ctpool_worker_key_bundle's own comment above): unlike
   * ctpool_wait/_shutdown_drain/_shutdown_immediate, there is no safe
   * no-op available here. Destroying pool from within one of its own
   * still-executing workers would free pool->mu and the pool struct itself
   * while that worker is still on its way back through worker_thread_fn
   * (task_free, then a lock/decrement/broadcast/unlock against the
   * just-freed object): a real, deterministic use-after-free, not a rare
   * race. __ctpool_destroy has no ccol_retval_t of its own to report this
   * through (its documented contract is: a live handle in, or fatal_err on
   * misuse), so a detected self-call is treated exactly like this
   * function's own stale-handle case immediately above: a loud, immediate
   * fatal_err() rather than a silent skip followed by undefined behaviour,
   * mirroring __event_loop_destroy's own identical self-destroy guard in
   * cthreadcomm.c. */
  if (_ctpool_is_self_call(raw)) {
    mutex_unlock(ctpool_slot_table.mutex);
    fatal_err(
        "ctpool_destroy: called from within a task (or its on_complete "
        "callback) running on this very pool's own worker thread; "
        "destroying it here would free the pool out from under that "
        "still-executing worker");
  }
  slot->in_use = false; /* blocks ALL future resolves for this handle from
                            this instant, including a second concurrent
                            destroy attempt */
#if FORK_SAFETY_REQUIRED
  /* See ctpool_slot_t's own torn_down field comment: raw's worker threads
   * are not actually gone yet, only unreachable via this handle from now
   * on, so _ctpool_atfork_prepare must keep locking raw->mu across fork()
   * until _ctpool_teardown_raw itself clears this, right before raw->mu
   * becomes unsafe to touch. */
  slot->torn_down = true;
#endif
  mutex_unlock(ctpool_slot_table.mutex);

  _ctpool_teardown_raw(raw, idx);

  /* Release the slot last, only after raw is fully torn down and freed:
   * this is what makes the slot's generation bump (and the free-index
   * push-back) mark the handle as reusable, not any earlier step. Re-fetch
   * by idx rather than reusing `slot`: a concurrent create_cthread_pool_mp's
   * own _ctpool_handle_slot_acquire call in between may have reallocated
   * slots' backing array via cvector_push_back, invalidating any pointer
   * into it taken before this second lock acquisition; idx itself is
   * stable. */
  mutex_lock(ctpool_slot_table.mutex);
  ctpool_slot_t *slot2 =
      (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* bumps this slot's generation past whatever value
      the just-freed pool's handle carried, so that stale handle can never
      again match a FUTURE acquire's generation for this same index */
  cvector_push_back(ctpool_slot_table.free_indices, &idx);
  mutex_unlock(ctpool_slot_table.mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* Resolves h to its underlying cthread_pool* WITHOUT pinning it (does not
 * touch pending_resolve_count at all): a bare slot-table lookup, safe for
 * tests specifically because test code calling this runs synchronously,
 * single-threaded, with no concurrent destroy to race in the first place;
 * unlike _ctpool_resolve, there is no matching _unpin call a test needs to
 * remember, which would otherwise be an easy gap to leave (a forgotten
 * unpin would leave pending_resolve_count permanently nonzero on that
 * pool, silently hanging every future ctpool_destroy call against it).
 * Returns NULL under the exact same conditions _ctpool_resolve does. */
cthread_pool *_ctpool_resolve_for_tests(ctpool h) {
  call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  mutex_lock(ctpool_slot_table.mutex);
  cthread_pool *raw = NULL;
  if (idx < cvector_elem_count(ctpool_slot_table.slots)) {
    ctpool_slot_t *slot =
        (ctpool_slot_t *)cvector_at(ctpool_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  mutex_unlock(ctpool_slot_table.mutex);
  return raw;
}

/* Reads how many slots the ctpool handle table currently holds (grown ones
 * plus freed-but-not-yet-reused ones): lets a test assert that a
 * create/destroy churn loop reuses freed slots rather than growing the
 * table without bound. */
size_t _ctpool_slot_table_capacity_for_tests(void) {
  call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  mutex_lock(ctpool_slot_table.mutex);
  size_t n = cvector_elem_count(ctpool_slot_table.slots);
  mutex_unlock(ctpool_slot_table.mutex);
  return n;
}

/* Reads pool's own current task-node free-list size, for tests directly
 * verifying the recycling optimization (task_alloc/task_free/
 * drain_task_free_list) rather than only inferring its effects indirectly
 * through custom-allocator call counts. Takes the already-resolved
 * cthread_pool* (as returned by _ctpool_resolve_for_tests), not a ctpool
 * handle, matching this test-only accessor's own established convention of
 * building on that one resolve step rather than duplicating it. */
size_t _ctpool_task_free_list_size_for_tests(cthread_pool *pool) {
  mutex_lock(pool->mu);
  size_t n = pool->task_free_list_size;
  mutex_unlock(pool->mu);
  return n;
}

/* Reads pool's own task-node free-list cap (num_threads * 4 at
 * construction; see struct cthread_pool's own field comment), so a test can
 * assert the bound relative to whatever num_threads it actually constructed
 * the pool with, rather than hardcoding the multiplier a second time. */
size_t _ctpool_task_free_list_cap_for_tests(cthread_pool *pool) {
  mutex_lock(pool->mu);
  size_t n = pool->task_free_list_cap;
  mutex_unlock(pool->mu);
  return n;
}
#endif

/* Frees the slot table's own bookkeeping arrays at process exit, so make
 * memtest's leak-kind reporting does not flag them as still-reachable;
 * mirrors event_loop's/clru_cache's own _cleanup_*_slot_table exactly (see
 * those functions' own comments for the full rationale, including why this
 * is sound only given every ctpool the application created was itself
 * destroyed before process exit; the same precondition this test suite
 * already satisfies for a clean make memtest). MUST call_once here:
 * __attribute__((destructor)) functions run unconditionally for the whole
 * shared object regardless of which parts of it were actually used, so a
 * process that links this library but never creates a single ctpool would
 * otherwise lock a never-pthread_mutex_init'd mutex here. */
__attribute__((destructor)) static void _cleanup_ctpool_slot_table(void) {
  call_once(ctpool_slot_table.once, _ctpool_slot_table_init_globals);
  mutex_lock(ctpool_slot_table.mutex);
  __cvector_destroy(ctpool_slot_table.slots);
  __cvector_destroy(ctpool_slot_table.free_indices);
  mutex_unlock(ctpool_slot_table.mutex);
}
