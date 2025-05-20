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

#pragma once

#include <common.h>
#include <stdbool.h>
#include <time.h>

/**
 * @file cthreadpool.h
 * @brief Generic thread pool with bounded or unbounded task queue, completion
 *        callbacks, and futures.
 *
 * Two queue modes, selected at construction:
 *   queue_capacity == 0 or ccol_invalid_size -> unbounded: ctpool_submit never
 *     blocks on capacity; the only failure path is ccol_not_enough_memory.
 *   queue_capacity > 0 -> bounded: ctpool_submit blocks when the queue is full;
 *     ctpool_try_submit returns ccol_container_full instead of blocking.
 *
 * Two shutdown modes:
 *   ctpool_shutdown_drain    - finish all queued tasks, then stop workers.
 *   ctpool_shutdown_immediate - cancel all queued tasks, stop after current
 * ones.
 *
 * Futures allow the submitting thread to collect a void* result:
 *   ctpool_future *f = ctpool_submit_future(pool, fn, arg);
 *   void *result = ctpool_future_get(f);
 *   ctpool_future_free(f);
 *
 * Thread safety: all public functions are safe to call concurrently except
 * ctpool_shutdown_drain, ctpool_shutdown_immediate, and __ctpool_destroy, which
 * must each be called at most once and not concurrently with each other.
 */

/* ========================================================================== */
/*                         OPAQUE TYPES                                       */
/* ========================================================================== */

/** @brief Opaque thread pool structure */
typedef struct cthread_pool cthread_pool;

/** @brief Handle type (pointer to opaque struct) */
typedef cthread_pool *ctpool;

/** @brief Future handle -- represents a pending void* result */
typedef struct ctpool_future ctpool_future;

/* ========================================================================== */
/*                         CREATION                                           */
/* ========================================================================== */

/**
 * @brief Create a thread pool with custom memory management
 *
 * @param num_threads     Number of worker threads (must be >= 1)
 * @param queue_capacity  0 or ccol_invalid_size -> unbounded task queue;
 *                        N > 0 -> bounded task queue of capacity N
 * @param mprocs          Custom allocator, or NULL for malloc/free
 * @param err_str         Optional: receives error description on failure
 * @return New pool handle, or NULL on failure
 */
ctpool create_cthread_pool_mp(size_t num_threads, size_t queue_capacity,
                              ccol_memmgmt_procs_t *mprocs, char **err_str);

/**
 * @brief Create a thread pool with default memory management
 *
 * Equivalent to create_cthread_pool_mp with mprocs = NULL.
 */
static inline __attribute__((always_inline)) ctpool
create_cthread_pool(size_t num_threads, size_t queue_capacity, char **err_str) {
  return create_cthread_pool_mp(num_threads, queue_capacity, NULL, err_str);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/**
 * @brief Internal destroy -- use ctpool_destroy macro instead
 *
 * If neither ctpool_shutdown_drain nor ctpool_shutdown_immediate was called
 * beforehand, performs a drain shutdown inline before freeing resources.
 */
void __ctpool_destroy(ctpool pool);

/**
 * @brief RAII cleanup function (used with _ccol_destructor)
 */
static inline __attribute__((always_inline)) void ___ctpool_destroy(
    ctpool *pp) {
  if (pp && *pp) {
    __ctpool_destroy(*pp);
    *pp = NULL;
  }
}

/**
 * @brief Destroy a thread pool and set handle to NULL
 */
#define ctpool_destroy(pool)  \
  do {                        \
    __ctpool_destroy((pool)); \
    (pool) = NULL;            \
  } while (0)

/* ========================================================================== */
/*                    DECLARE / CONSTRUCT / SCOPED MACROS                     */
/* ========================================================================== */

/**
 * @brief Declare an uninitialised pool variable
 *
 * Must be followed by ctpool_construct or a create_cthread_pool* call.
 */
#define ctpool_declare(name) ctpool name

/**
 * @brief Declare with automatic destruction on scope exit
 */
#define ctpool_declare_scoped(name) \
  ctpool name _ccol_destructor(___ctpool_destroy) = NULL

/**
 * @brief Declare and initialise in one step; fatal_err on failure
 *
 * Example:
 * @code
 * ctpool_construct(pool, 4, 256);
 * ctpool_submit(pool, my_fn, my_arg, NULL);
 * ctpool_shutdown_drain(pool);
 * ctpool_destroy(pool);
 * @endcode
 */
#define ctpool_construct(name, num_threads, queue_capacity)                   \
  ctpool name = NULL;                                                         \
  do {                                                                        \
    char *_ctp_err = NULL;                                                    \
    (name) = create_cthread_pool((num_threads), (queue_capacity), &_ctp_err); \
    if (!(name)) {                                                            \
      fatal_err("ctpool_construct('%s'): %s", #name,                          \
                _ctp_err ? _ctp_err : "unknown error");                       \
    }                                                                         \
  } while (0)

/**
 * @brief Declare, initialise, and auto-destroy on scope exit; fatal_err on
 *        failure
 */
#define ctpool_construct_scoped(name, num_threads, queue_capacity)            \
  ctpool name _ccol_destructor(___ctpool_destroy) = NULL;                     \
  do {                                                                        \
    char *_ctp_err = NULL;                                                    \
    (name) = create_cthread_pool((num_threads), (queue_capacity), &_ctp_err); \
    if (!(name)) {                                                            \
      fatal_err("ctpool_construct_scoped('%s'): %s", #name,                   \
                _ctp_err ? _ctp_err : "unknown error");                       \
    }                                                                         \
  } while (0)

/* ========================================================================== */
/*                         TASK SUBMISSION                                    */
/* ========================================================================== */

/**
 * @brief Submit a task (blocking when bounded queue is full)
 *
 * For unbounded queues this never blocks on capacity; the only failure path
 * other than ccol_invalid_args is ccol_not_enough_memory.
 * For bounded queues this blocks until space is available.
 *
 * @param pool        Thread pool handle
 * @param fn          Task function (must not be NULL)
 * @param arg         Argument passed to fn and on_complete (may be NULL)
 * @param on_complete Called by the worker after fn returns (may be NULL)
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_not_permitted (pool is shutting down)
 */
ccol_retval_t ctpool_submit(ctpool pool, void (*fn)(void *), void *arg,
                            void (*on_complete)(void *));

/**
 * @brief Submit without blocking
 *
 * Returns ccol_container_full immediately if a bounded queue is at capacity.
 * For unbounded queues the behaviour is identical to ctpool_submit.
 *
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory,
 *         ccol_not_permitted, or ccol_container_full
 */
ccol_retval_t ctpool_try_submit(ctpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *));

/**
 * @brief Submit with a relative timeout
 *
 * Blocks up to timeout waiting for space in a bounded queue. The timeout is a
 * relative duration (converted to an absolute deadline internally using
 * CLOCK_REALTIME).
 *
 * @param timeout Relative duration to wait; NULL is treated as zero (try-only)
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory,
 *         ccol_not_permitted, ccol_container_full, ccol_timed_out, or
 *         ccol_unexpected_failure
 */
ccol_retval_t ctpool_timed_submit(ctpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *),
                                  struct timespec *timeout);

/* ========================================================================== */
/*                         FUTURES                                            */
/* ========================================================================== */

/**
 * @brief Submit a task that returns a void* result (blocking)
 *
 * The returned future handle has a reference count of 2: one held by the
 * caller, one held internally by the queued task. The caller must call
 * ctpool_future_free exactly once when done with the future.
 *
 * For bounded queues this blocks until space is available. Use
 * ctpool_try_submit_future or ctpool_timed_submit_future to avoid blocking.
 *
 * @param pool  Thread pool handle
 * @param fn    Task function returning a void* result (must not be NULL)
 * @param arg   Argument passed to fn (may be NULL)
 * @return New future handle, or NULL on OOM or if the pool is shutting down
 */
ctpool_future *ctpool_submit_future(ctpool pool, void *(*fn)(void *),
                                    void *arg);

/**
 * @brief Submit a future task without blocking
 *
 * Returns ccol_container_full immediately if a bounded queue is at capacity.
 * For unbounded queues the behaviour is identical to ctpool_submit_future.
 *
 * @param pool  Thread pool handle
 * @param fn    Task function returning a void* result (must not be NULL)
 * @param arg   Argument passed to fn (may be NULL)
 * @param out   Receives the future handle on success; set to NULL on failure
 *              (must not be NULL)
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory,
 *         ccol_not_permitted, or ccol_container_full
 */
ccol_retval_t ctpool_try_submit_future(ctpool pool, void *(*fn)(void *),
                                       void *arg, ctpool_future **out);

/**
 * @brief Submit a future task with a relative timeout
 *
 * Blocks up to timeout waiting for space in a bounded queue. The timeout is a
 * relative duration (converted to an absolute deadline internally using
 * CLOCK_REALTIME). Passing NULL as timeout is equivalent to
 * ctpool_try_submit_future (no waiting).
 *
 * @param pool    Thread pool handle
 * @param fn      Task function returning a void* result (must not be NULL)
 * @param arg     Argument passed to fn (may be NULL)
 * @param timeout Relative duration to wait; NULL is treated as zero (try-only)
 * @param out     Receives the future handle on success; set to NULL on failure
 *                (must not be NULL)
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory,
 *         ccol_not_permitted, ccol_container_full, ccol_timed_out, or
 *         ccol_unexpected_failure
 */
ccol_retval_t ctpool_timed_submit_future(ctpool pool, void *(*fn)(void *),
                                         void *arg, struct timespec *timeout,
                                         ctpool_future **out);

/**
 * @brief Block until the future has a result and return it
 *
 * Returns NULL if the task was cancelled due to ctpool_shutdown_immediate.
 * Does not free the future; call ctpool_future_free afterwards.
 */
void *ctpool_future_get(ctpool_future *f);

/**
 * @brief Non-blocking check: true if the future has a result (or was
 *        cancelled)
 */
bool ctpool_future_done(ctpool_future *f);

/**
 * @brief True if the future was cancelled via ctpool_shutdown_immediate
 */
bool ctpool_future_cancelled(ctpool_future *f);

/**
 * @brief Release the caller's reference to the future
 *
 * Must be called exactly once per successful ctpool_submit_future,
 * ctpool_try_submit_future, or ctpool_timed_submit_future call. May be called
 * before or after ctpool_future_get. When called before ctpool_future_get the
 * future will be freed automatically once the worker finishes (cancel
 * semantics).
 */
void ctpool_future_free(ctpool_future *f);

/* ========================================================================== */
/*                         POOL MANAGEMENT                                    */
/* ========================================================================== */

/**
 * @brief Block until the task queue is empty and all active tasks are done
 *
 * Does not shut down the pool; workers remain alive and the pool accepts new
 * submissions after this call returns.
 *
 * Contract: no other thread should be submitting tasks concurrently when this
 * is called. With an unbounded queue, concurrent submissions can cause
 * indefinite blocking.
 */
void ctpool_wait(ctpool pool);

/**
 * @brief Graceful shutdown: finish all queued tasks, then stop workers
 *
 * Blocks until every queued and active task has completed. No new tasks may
 * be submitted after this call. Must be called before ctpool_destroy unless
 * ctpool_shutdown_immediate was called instead.
 */
void ctpool_shutdown_drain(ctpool pool);

/**
 * @brief Immediate shutdown: cancel queued tasks, stop after current ones
 *
 * All tasks still in the queue are discarded (future handles for those tasks
 * become cancelled). Workers finish their current task and then exit. Blocks
 * until all worker threads have exited.
 */
void ctpool_shutdown_immediate(ctpool pool);

/**
 * @brief Number of tasks currently in the queue (not yet picked up by a
 *        worker)
 */
size_t ctpool_pending_count(ctpool pool);

/**
 * @brief Number of tasks currently being executed by worker threads
 */
size_t ctpool_active_count(ctpool pool);
