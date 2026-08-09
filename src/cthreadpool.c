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
} ctpool_slot_t;

static struct {
  mutex_t mutex;
  once_flag_t once;
  cvec slots;        /* cvec of ctpool_slot_t; grows via push_back only,
                         indices permanent once allocated */
  cvec free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
} ctpool_slot_table = {0};

static void _ctpool_slot_table_init_globals(void) {
  mutex_init(ctpool_slot_table.mutex);
  ctpool_slot_table.slots = cvector_create(sizeof(ctpool_slot_t), NULL);
  if (!ctpool_slot_table.slots)
    fatal_err("ctpool slot table: failed to allocate slots vector");
  ctpool_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!ctpool_slot_table.free_indices)
    fatal_err("ctpool slot table: failed to allocate free-index vector");
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
};

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
  ctpool h = ((ctpool)idx << 32) | (ctpool)slot->generation;
  mutex_unlock(ctpool_slot_table.mutex);
  return h;
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

/* Allocate a task struct using the pool's allocator. */
static ctpool_task *task_alloc(cthread_pool *pool) {
  return (ctpool_task *)_mem_calloc(pool->m_procs, 1, sizeof(ctpool_task));
}

static void task_free(cthread_pool *pool, ctpool_task *task) {
  _mem_free(pool->m_procs, task);
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
 * Decrement a future's refcount.  Frees the future when the count reaches 0.
 * Caller must NOT hold future->mu; this function acquires and releases it.
 */
static void future_deref(ctpool_future *f) {
  mutex_lock(f->mu);
  int remaining = --f->refcount;
  mutex_unlock(f->mu);
  if (remaining == 0) {
    mutex_destroy(f->mu);
    cond_var_destroy(f->cv);
    free(f);
  }
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
  if (remaining == 0) {
    mutex_destroy(f->mu);
    cond_var_destroy(f->cv);
    free(f);
  }
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
 */
static bool make_abs_deadline(const struct timespec *rel,
                              struct timespec *abs_out) {
  if (clock_gettime(CLOCK_REALTIME, abs_out) != 0) return false;
  abs_out->tv_sec += rel->tv_sec;
  abs_out->tv_nsec += rel->tv_nsec;
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
      if (remaining == 0) {
        mutex_destroy(task->future->mu);
        cond_var_destroy(task->future->cv);
        free(task->future);
      }
    } else {
      task->fn(task->arg);
      if (task->on_complete) {
        task->on_complete(task->arg);
      }
    }

    task_free(pool, task);

    mutex_lock(pool->mu);
    pool->active_count--;
    if (pool->active_count == 0 && pool->queue_size == 0) {
      cond_var_broadcast(pool->idle_cv);
    }
    mutex_unlock(pool->mu);
  }

  return NULL;
}

/* Forward declarations: create_cthread_pool_mp's own slot-acquire-failure
 * rollback path needs the shared teardown helper defined later in this
 * file (right after the shutdown_drain/_immediate internal/public split it
 * itself depends on). */
static void _ctpool_teardown_raw(cthread_pool *pool);

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
    _ctpool_teardown_raw(pool);
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

ccol_retval_t ctpool_submit(ctpool pool, void (*fn)(void *), void *arg,
                            void (*on_complete)(void *)) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ctpool_task *task = task_alloc(raw);
  if (!task) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  task->fn = fn;
  task->arg = arg;
  task->on_complete = on_complete;

  ccol_retval_t r = submit_internal(raw, task, 1, NULL);
  if (r != ccol_success) task_free(raw, task);
  _ctpool_resolve_unpin(raw);
  return r;
}

ccol_retval_t ctpool_try_submit(ctpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *)) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  ccol_retval_t pre = try_precheck(raw);
  if (pre != ccol_success) {
    _ctpool_resolve_unpin(raw);
    return pre;
  }

  ctpool_task *task = task_alloc(raw);
  if (!task) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  task->fn = fn;
  task->arg = arg;
  task->on_complete = on_complete;

  ccol_retval_t r = submit_internal(raw, task, 0, NULL);
  if (r != ccol_success) task_free(raw, task);
  _ctpool_resolve_unpin(raw);
  return r;
}

ccol_retval_t ctpool_timed_submit(ctpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *),
                                  struct timespec *timeout) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }

  struct timespec deadline = {0, 0};
  if (!timeout) {
    /* NULL timeout: behave like try_submit. Unpin this function's own
     * resolve first (its work here is done), then delegate via the
     * ORIGINAL handle value, not raw: ctpool_try_submit performs its own
     * independent resolve/unpin. Cheap, harmless, intentional redundancy,
     * not a bug; mirrors chttpclient_do_pooled delegating to
     * chttpclient_do_async via the original handle in the already-shipped
     * chttpcli redesign. */
    _ctpool_resolve_unpin(raw);
    return ctpool_try_submit(pool, fn, arg, on_complete);
  }
  if (!make_abs_deadline(timeout, &deadline)) {
    _ctpool_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }

  ctpool_task *task = task_alloc(raw);
  if (!task) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }
  task->fn = fn;
  task->arg = arg;
  task->on_complete = on_complete;

  ccol_retval_t r = submit_internal(raw, task, 2, &deadline);
  if (r != ccol_success) task_free(raw, task);
  _ctpool_resolve_unpin(raw);
  return r;
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

ctpool_future *ctpool_submit_future(ctpool pool, void *(*fn)(void *),
                                    void *arg) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return NULL;
  if (!fn) {
    _ctpool_resolve_unpin(raw);
    return NULL;
  }

  ctpool_task *task;
  ctpool_future *f = alloc_future_task(raw, fn, arg, &task);
  if (!f) {
    _ctpool_resolve_unpin(raw);
    return NULL;
  }

  ccol_retval_t r = submit_internal(raw, task, 1, NULL);
  if (r != ccol_success) {
    free_future_task(raw, f, task);
    _ctpool_resolve_unpin(raw);
    return NULL;
  }
  _ctpool_resolve_unpin(raw);
  return f;
}

ccol_retval_t ctpool_try_submit_future(ctpool pool, void *(*fn)(void *),
                                       void *arg, ctpool_future **out) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn || !out) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }
  *out = NULL;

  ccol_retval_t pre = try_precheck(raw);
  if (pre != ccol_success) {
    _ctpool_resolve_unpin(raw);
    return pre;
  }

  ctpool_task *task;
  ctpool_future *f = alloc_future_task(raw, fn, arg, &task);
  if (!f) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }

  ccol_retval_t r = submit_internal(raw, task, 0, NULL);
  if (r != ccol_success) {
    free_future_task(raw, f, task);
    _ctpool_resolve_unpin(raw);
    return r;
  }
  *out = f;
  _ctpool_resolve_unpin(raw);
  return ccol_success;
}

ccol_retval_t ctpool_timed_submit_future(ctpool pool, void *(*fn)(void *),
                                         void *arg, struct timespec *timeout,
                                         ctpool_future **out) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return ccol_invalid_args;
  if (!fn || !out) {
    _ctpool_resolve_unpin(raw);
    return ccol_invalid_args;
  }
  *out = NULL;

  if (!timeout) {
    /* NULL timeout: behave like try_submit_future. Unpin this function's
     * own resolve first, then delegate via the ORIGINAL handle value, not
     * raw; see ctpool_timed_submit's identical delegation comment above. */
    _ctpool_resolve_unpin(raw);
    return ctpool_try_submit_future(pool, fn, arg, out);
  }

  struct timespec deadline = {0, 0};
  if (!make_abs_deadline(timeout, &deadline)) {
    _ctpool_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }

  ctpool_task *task;
  ctpool_future *f = alloc_future_task(raw, fn, arg, &task);
  if (!f) {
    _ctpool_resolve_unpin(raw);
    return ccol_not_enough_memory;
  }

  ccol_retval_t r = submit_internal(raw, task, 2, &deadline);
  if (r != ccol_success) {
    free_future_task(raw, f, task);
    _ctpool_resolve_unpin(raw);
    return r;
  }
  *out = f;
  _ctpool_resolve_unpin(raw);
  return ccol_success;
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
  if (remaining == 0) {
    mutex_destroy(f->mu);
    cond_var_destroy(f->cv);
    free(f);
  }
  return ccol_success;
}

/* ========================================================================== */
/*                         POOL MANAGEMENT                                    */
/* ========================================================================== */

void ctpool_wait(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
  mutex_lock(raw->mu);
  while (raw->active_count > 0 || raw->queue_size > 0) {
    cond_var_wait(raw->idle_cv, raw->mu);
  }
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

  for (size_t i = 0; i < pool->num_threads; i++) {
    thread_join(pool->threads[i]);
  }
}

void ctpool_shutdown_drain(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
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

  for (size_t i = 0; i < pool->num_threads; i++) {
    thread_join(pool->threads[i]);
  }
}

void ctpool_shutdown_immediate(ctpool pool) {
  cthread_pool *raw = _ctpool_resolve(pool);
  if (!raw) return;
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
 * wait phase there is trivially instant. */
static void _ctpool_teardown_raw(cthread_pool *pool) {
  if (!pool->shutdown_started) {
    _ctpool_shutdown_drain_internal(pool);
  }

  mutex_lock(pool->mu);
  while (atomic_load(&pool->pending_resolve_count) > 0) {
    cond_var_wait(pool->pin_cv, pool->mu);
  }
  mutex_unlock(pool->mu);

  _mem_free(pool->m_procs, pool->threads);

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
  slot->in_use = false; /* blocks ALL future resolves for this handle from
                            this instant, including a second concurrent
                            destroy attempt */
  mutex_unlock(ctpool_slot_table.mutex);

  _ctpool_teardown_raw(raw);

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
