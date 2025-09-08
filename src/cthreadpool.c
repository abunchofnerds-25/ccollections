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
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

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
  pthread_t *threads;
  size_t num_threads;

  ccol_memmgmt_procs_t *m_procs;
};

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

/* ========================================================================== */
/*                         CREATION                                           */
/* ========================================================================== */

ctpool create_cthread_pool_mp(size_t num_threads, size_t queue_capacity,
                              ccol_memmgmt_procs_t *mprocs, char **err_str) {
  if (num_threads == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("num_threads must be >= 1");
    return NULL;
  }

  if (!ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;

  cthread_pool *pool =
      (cthread_pool *)(mprocs ? mprocs->calloc(1, sizeof(*pool))
                              : calloc(1, sizeof(*pool)));
  if (!pool) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate pool struct");
    return NULL;
  }

  if (mprocs) {
    pool->m_procs = (ccol_memmgmt_procs_t *)mprocs->malloc(sizeof(*mprocs));
    if (!pool->m_procs) {
      mprocs->free(pool);
      if (err_str) *err_str = CCOL_ERR_STR("failed to allocate m_procs copy");
      return NULL;
    }
    mem_cpy(pool->m_procs, mprocs, sizeof(*mprocs));
  }

  /* Treat ccol_invalid_size as unbounded. */
  pool->queue_cap = (queue_capacity == ccol_invalid_size) ? 0 : queue_capacity;

  if (mutex_init(pool->mu) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("mutex init failed");
    pool_free_self(pool);
    return NULL;
  }
  if (cond_var_init(pool->not_empty) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    mutex_destroy(pool->mu);
    pool_free_self(pool);
    return NULL;
  }
  if (cond_var_init(pool->not_full) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    mutex_destroy(pool->mu);
    cond_var_destroy(pool->not_empty);
    pool_free_self(pool);
    return NULL;
  }
  if (cond_var_init(pool->idle_cv) != 0) {
    if (err_str) *err_str = CCOL_ERR_STR("cond_var init failed");
    mutex_destroy(pool->mu);
    cond_var_destroy(pool->not_empty);
    cond_var_destroy(pool->not_full);
    pool_free_self(pool);
    return NULL;
  }

  pool->threads =
      (pthread_t *)_mem_calloc(pool->m_procs, num_threads, sizeof(pthread_t));
  if (!pool->threads) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate threads array");
    mutex_destroy(pool->mu);
    cond_var_destroy(pool->not_empty);
    cond_var_destroy(pool->not_full);
    cond_var_destroy(pool->idle_cv);
    pool_free_self(pool);
    return NULL;
  }

  pool->num_threads = num_threads;

  for (size_t i = 0; i < num_threads; i++) {
    if (pthread_create(&pool->threads[i], NULL, worker_thread_fn, pool) != 0) {
      /* Shut down the threads already started, then clean up. */
      mutex_lock(pool->mu);
      pool->shutdown_drain = true;
      pool->shutdown_started = true;
      cond_var_broadcast(pool->not_empty);
      mutex_unlock(pool->mu);
      for (size_t j = 0; j < i; j++) {
        pthread_join(pool->threads[j], NULL);
      }
      if (err_str) *err_str = CCOL_ERR_STR("pthread_create failed");
      _mem_free(pool->m_procs, pool->threads);
      mutex_destroy(pool->mu);
      cond_var_destroy(pool->not_empty);
      cond_var_destroy(pool->not_full);
      cond_var_destroy(pool->idle_cv);
      pool_free_self(pool);
      return NULL;
    }
  }

  return pool;
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
  if (!pool || !fn) return ccol_invalid_args;

  ctpool_task *task = task_alloc(pool);
  if (!task) return ccol_not_enough_memory;
  task->fn = fn;
  task->arg = arg;
  task->on_complete = on_complete;

  ccol_retval_t r = submit_internal(pool, task, 1, NULL);
  if (r != ccol_success) task_free(pool, task);
  return r;
}

ccol_retval_t ctpool_try_submit(ctpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *)) {
  if (!pool || !fn) return ccol_invalid_args;

  ccol_retval_t pre = try_precheck(pool);
  if (pre != ccol_success) return pre;

  ctpool_task *task = task_alloc(pool);
  if (!task) return ccol_not_enough_memory;
  task->fn = fn;
  task->arg = arg;
  task->on_complete = on_complete;

  ccol_retval_t r = submit_internal(pool, task, 0, NULL);
  if (r != ccol_success) task_free(pool, task);
  return r;
}

ccol_retval_t ctpool_timed_submit(ctpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *),
                                  struct timespec *timeout) {
  if (!pool || !fn) return ccol_invalid_args;

  struct timespec deadline = {0, 0};
  if (!timeout) {
    /* NULL timeout: behave like try_submit */
    return ctpool_try_submit(pool, fn, arg, on_complete);
  }
  if (!make_abs_deadline(timeout, &deadline)) {
    return ccol_unexpected_failure;
  }

  ctpool_task *task = task_alloc(pool);
  if (!task) return ccol_not_enough_memory;
  task->fn = fn;
  task->arg = arg;
  task->on_complete = on_complete;

  ccol_retval_t r = submit_internal(pool, task, 2, &deadline);
  if (r != ccol_success) task_free(pool, task);
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
  if (!pool || !fn) return NULL;

  ctpool_task *task;
  ctpool_future *f = alloc_future_task(pool, fn, arg, &task);
  if (!f) return NULL;

  ccol_retval_t r = submit_internal(pool, task, 1, NULL);
  if (r != ccol_success) {
    free_future_task(pool, f, task);
    return NULL;
  }
  return f;
}

ccol_retval_t ctpool_try_submit_future(ctpool pool, void *(*fn)(void *),
                                       void *arg, ctpool_future **out) {
  if (!pool || !fn || !out) return ccol_invalid_args;
  *out = NULL;

  ccol_retval_t pre = try_precheck(pool);
  if (pre != ccol_success) return pre;

  ctpool_task *task;
  ctpool_future *f = alloc_future_task(pool, fn, arg, &task);
  if (!f) return ccol_not_enough_memory;

  ccol_retval_t r = submit_internal(pool, task, 0, NULL);
  if (r != ccol_success) {
    free_future_task(pool, f, task);
    return r;
  }
  *out = f;
  return ccol_success;
}

ccol_retval_t ctpool_timed_submit_future(ctpool pool, void *(*fn)(void *),
                                         void *arg, struct timespec *timeout,
                                         ctpool_future **out) {
  if (!pool || !fn || !out) return ccol_invalid_args;
  *out = NULL;

  if (!timeout) return ctpool_try_submit_future(pool, fn, arg, out);

  struct timespec deadline = {0, 0};
  if (!make_abs_deadline(timeout, &deadline)) return ccol_unexpected_failure;

  ctpool_task *task;
  ctpool_future *f = alloc_future_task(pool, fn, arg, &task);
  if (!f) return ccol_not_enough_memory;

  ccol_retval_t r = submit_internal(pool, task, 2, &deadline);
  if (r != ccol_success) {
    free_future_task(pool, f, task);
    return r;
  }
  *out = f;
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
  if (!pool) return;
  mutex_lock(pool->mu);
  while (pool->active_count > 0 || pool->queue_size > 0) {
    cond_var_wait(pool->idle_cv, pool->mu);
  }
  mutex_unlock(pool->mu);
}

void ctpool_shutdown_drain(ctpool pool) {
  if (!pool) return;

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
    pthread_join(pool->threads[i], NULL);
  }
}

void ctpool_shutdown_immediate(ctpool pool) {
  if (!pool) return;

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
    pthread_join(pool->threads[i], NULL);
  }
}

size_t ctpool_pending_count(ctpool pool) {
  if (!pool) return 0;
  mutex_lock(pool->mu);
  size_t n = pool->queue_size;
  mutex_unlock(pool->mu);
  return n;
}

size_t ctpool_active_count(ctpool pool) {
  if (!pool) return 0;
  mutex_lock(pool->mu);
  size_t n = pool->active_count;
  mutex_unlock(pool->mu);
  return n;
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

void __ctpool_destroy(ctpool pool) {
  if (!pool) return;

  /* If the caller did not initiate a shutdown, drain everything first.
   * Otherwise both ctpool_shutdown_drain and ctpool_shutdown_immediate always
   * perform pthread_join before returning, so threads_joined is guaranteed
   * true and nothing further is needed here. */
  if (!pool->shutdown_started) {
    ctpool_shutdown_drain(pool);
  }

  _mem_free(pool->m_procs, pool->threads);

  mutex_destroy(pool->mu);
  cond_var_destroy(pool->not_empty);
  cond_var_destroy(pool->not_full);
  cond_var_destroy(pool->idle_cv);

  pool_free_self(pool);
}
