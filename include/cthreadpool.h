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

#include <stdbool.h>

#include "common.h"

/* Every declaration from here to the end of this header is part of the public
 * Application Binary Interface (ABI) of libccollections, and the shared library
 * exports all of them. Because the library itself is built with
 * -fvisibility=hidden, a function or object that is not inside one of these
 * blocks stays internal to the library: its dynamic symbol table does not hold
 * it, the application that links against the library cannot interpose it, and
 * a symbol of the same name in that application cannot collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cthreadpool.h
 * @brief Generic thread pool with bounded or unbounded task queue, completion
 *        callbacks, and futures.
 *
 * The caller selects one of two queue modes at construction:
 *   queue_capacity == 0 -> unbounded: ctpool_submit never blocks on capacity.
 *     The only failure path is ccol_not_enough_memory.
 *   queue_capacity > 0 -> bounded: ctpool_submit blocks when the queue is full.
 *     ctpool_try_submit does not block; it returns ccol_container_full.
 *
 * There are two shutdown modes:
 *   ctpool_shutdown_drain    - finish all queued tasks, then stop workers.
 *   ctpool_shutdown_immediate - cancel all queued tasks, stop after current
 * ones.
 *
 * A future lets the thread that submits a task collect a void* result:
 *   ctpool_future *f = ctpool_submit_future(pool, fn, arg);
 *   void *result = ctpool_future_get(f);
 *   ctpool_future_free(f);
 *
 * Result ownership: the pool knows neither what the void* result points to
 * nor how the task allocated it, so it never allocates, copies, or frees the
 * result. ctpool_future_free frees only the bookkeeping of the future itself,
 * which is the handle that ctpool_submit_future returns, and never the result
 * payload. The thread that calls ctpool_future_get owns that payload from that
 * point on, and it must free the payload in the way that the task function
 * allocated it. The same rule holds for the result argument of
 * ctpool_future_fulfill on a detached future.
 *
 * Thread safety: the caller can call every public function at the same time as
 * every other one on the same live handle, including a race between
 * ctpool_shutdown_drain, ctpool_shutdown_immediate and __ctpool_destroy. An
 * internal resolve and pin mechanism makes this safe; see the doc comment of
 * __ctpool_destroy for it.
 *
 * ctpool_shutdown_drain and ctpool_shutdown_immediate are each idempotent: a
 * second call to either one, sequential or concurrent, does no more shutdown
 * work of its own. The second call does block until the shutdown that is
 * already under way stops, so every caller gets the same guarantee when the
 * call returns: no worker is alive.
 *
 * There is one exception to "no more work": a ctpool_shutdown_immediate call
 * that arrives while a ctpool_shutdown_drain is draining the queue escalates
 * the shutdown. It discards what is left in the queue and cancels the futures
 * of those tasks.
 *
 * The one hard restriction is __ctpool_destroy itself. A second call to it on
 * a handle whose destroy is already complete is a fatal error, and so is a
 * race against a second, concurrent __ctpool_destroy call on the same live
 * handle. Neither one is a safe no-op.
 */

/* ========================================================================== */
/*                         OPAQUE TYPES                                       */
/* ========================================================================== */

/** @brief Opaque thread pool structure */
typedef struct cthread_pool cthread_pool;

/**
 * @brief Handle type: an opaque VALUE (a packed {slot index, generation}
 *        pair), NOT a pointer.
 *
 * Never cast a ctpool to void* or from void*, never compare it with a pointer
 * cast, and never treat it as an address. Compare it against CTPOOL_INVALID,
 * or use it in a truthiness check: CTPOOL_INVALID is 0, so `if (!pool)` is a
 * valid test for an invalid handle.
 *
 * The library resolves every use of a ctpool through a slot table that it owns
 * itself, before it touches the cthread_pool object behind the handle. So the
 * library always finds a stale handle (a handle whose pool is already
 * destroyed) and never silently dereferences freed memory. A call to
 * __ctpool_destroy with a stale or already-destroyed handle is a fatal error;
 * see the doc comment of that function.
 */
typedef uint64_t ctpool;

/** @brief Sentinel value for "no pool" / "not yet created" / "destroyed" */
#define CTPOOL_INVALID ((ctpool)0)

/** @brief Future handle. It holds a void* result that is not ready yet */
typedef struct ctpool_future ctpool_future;

/* ========================================================================== */
/*                         CREATION                                           */
/* ========================================================================== */

/**
 * @brief Create a thread pool with custom memory management
 *
 * @param num_threads     Number of worker threads (must be >= 1)
 * @param queue_capacity  0 -> unbounded task queue; N > 0 -> bounded task
 *                        queue of capacity N. ccol_invalid_size is refused.
 * @param mprocs          Custom allocator, or NULL for malloc/free
 * @param err_str         Optional. It gets a description of the error on
 *                        failure
 * @return New pool handle, or CTPOOL_INVALID on failure
 *
 * @note The pool recycles the internal node behind each submission through a
 * cache of its own, which keeps what the recent traffic needed, at most
 * max(num_threads * 4, 32768) idle nodes. At every 256th point where the
 * pool goes idle, it gives back to mprocs half of what it holds above
 * num_threads * 4, unless the traffic since the previous such point emptied
 * it. So mprocs does not see one allocation and one free for each
 * submission.
 * @note Each worker thread records its pool on one thread-specific key that
 * the library creates for the whole process on the first creation of a
 * pool; that key is what lets a task that destroys its own pool fail loudly.
 * When the process has no key left to give (PTHREAD_KEYS_MAX), this function
 * returns CTPOOL_INVALID, and so does every later call in that process. The
 * library deletes the key when it unloads with no pool left, so a cycle of
 * dlopen() and dlclose() gives the key back
 */
ctpool ccol_create_cthread_pool_mp(size_t num_threads, size_t queue_capacity,
                                   ccol_memmgmt_procs_t *mprocs,
                                   char **err_str);

/**
 * @brief Create a thread pool with default memory management
 *
 * This is the same as ccol_create_cthread_pool_mp with mprocs = NULL.
 */
static inline __attribute__((always_inline)) ctpool ccol_create_cthread_pool(
    size_t num_threads, size_t queue_capacity, char **err_str) {
  return ccol_create_cthread_pool_mp(num_threads, queue_capacity, NULL,
                                     err_str);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/**
 * @brief Internal destroy. Use the ctpool_destroy macro instead
 *
 * pool is an opaque VALUE handle, which the library resolves through a slot
 * table that it owns itself before it touches the pool object behind the
 * handle. pool must be a live handle at this moment, that is, one that
 * ccol_create_cthread_pool or ccol_create_cthread_pool_mp returned and that
 * nothing has destroyed yet. CTPOOL_INVALID is a silent no-op.
 *
 * Every other stale handle is a FATAL ERROR: a handle that an earlier,
 * complete call to this same function destroyed, a handle that another thread
 * destroys right now in a race with this call, and also a forged value or
 * garbage. This function calls ccol_fatal_err() (abort()/SIGABRT) for all of
 * them. Without this, a purely sequential double-destroy risks a
 * use-after-free or a double-free, and so does a concurrent destroy that
 * overlaps in time.
 *
 * A call to this function on pool from inside a task, or from the on_complete
 * callback of that task, is also a FATAL ERROR when the task runs on one of
 * the worker threads of pool itself. A destroy there frees the mutex, the
 * condition variables and the struct of the pool while that worker is on its
 * way back through its own dispatch loop, and the worker touches all of them
 * again immediately after.
 *
 * On a live handle, this function waits for every in-flight resolved use of
 * the handle to stop, that is, a call to ctpool_submit, _try_submit,
 * _timed_submit, _submit_future, _try_submit_future, _timed_submit_future,
 * _wait, _shutdown_drain, _shutdown_immediate, _pending_count or _active_count
 * that runs on another thread at this moment. If the caller called neither
 * ctpool_shutdown_drain nor ctpool_shutdown_immediate before this, a drain
 * shutdown runs first, before that wait, so a submitter that blocks on a full
 * bounded queue can depend on the broadcast of the shutdown to release its
 * pin.
 */
void __ctpool_destroy(ctpool pool);

/**
 * @brief RAII cleanup function (used with _ccol_destructor)
 */
static inline __attribute__((always_inline)) void ___ctpool_destroy(
    ctpool *pp) {
  if (pp && *pp) {
    __ctpool_destroy(*pp);
    *pp = CTPOOL_INVALID;
  }
}

/**
 * @brief Destroy a thread pool and set handle to CTPOOL_INVALID
 *
 * A call to this macro with a stale handle is a FATAL ERROR (abort()/SIGABRT),
 * not a silent double-free. A stale handle is one that an earlier call
 * destroyed, or one that another thread destroys right now in a race with
 * this call.
 *
 * A call to this macro on pool from inside a task, or from the on_complete
 * callback of that task, is also a FATAL ERROR when the task runs on one of
 * the worker threads of pool itself. The reason is the same: that worker is on
 * its way back through its own dispatch loop, and it touches the mutex, the
 * condition variables and the struct of the pool again immediately after the
 * destroy frees them.
 *
 * @note The macro evaluates pool exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define ctpool_destroy(pool) \
  _ccol_ctpool_destroy_impl( \
      pool, _ccol_uniq(__ccol_ctpool_destroy_slot, __COUNTER__))

/* Internal: the body of ctpool_destroy. slot is a name from _ccol_uniq(), so
 * the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_ctpool_destroy_impl(pool, slot) \
  do {                                        \
    __typeof__(pool) *slot = &(pool);         \
    __ctpool_destroy(*slot);                  \
    *slot = CTPOOL_INVALID;                   \
  } while (0)

/* ========================================================================== */
/*                    DECLARE / CONSTRUCT / SCOPED MACROS                     */
/* ========================================================================== */

/**
 * @brief Declare an uninitialised pool variable
 *
 * A ctpool_construct or a ccol_create_cthread_pool* call must follow this
 * macro.
 */
#define ctpool_declare(name) ctpool name

/**
 * @brief Declare with automatic destruction on scope exit
 */
#define ctpool_declare_scoped(name) \
  ctpool name _ccol_destructor(___ctpool_destroy) = CTPOOL_INVALID

/**
 * @brief Declare and initialise in one step. It calls ccol_fatal_err on
 *        failure
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
  ctpool name = CTPOOL_INVALID;                                               \
  do {                                                                        \
    char *_ctp_err = NULL;                                                    \
    (name) =                                                                  \
        ccol_create_cthread_pool((num_threads), (queue_capacity), &_ctp_err); \
    if (!(name)) {                                                            \
      ccol_fatal_err("ctpool_construct('%s'): %s", #name,                     \
                     _ctp_err ? _ctp_err : "unknown error");                  \
    }                                                                         \
  } while (0)

/**
 * @brief Declare, initialise, and auto-destroy at scope exit. It calls
 *        ccol_fatal_err on failure
 */
#define ctpool_construct_scoped(name, num_threads, queue_capacity)            \
  ctpool name _ccol_destructor(___ctpool_destroy) = CTPOOL_INVALID;           \
  do {                                                                        \
    char *_ctp_err = NULL;                                                    \
    (name) =                                                                  \
        ccol_create_cthread_pool((num_threads), (queue_capacity), &_ctp_err); \
    if (!(name)) {                                                            \
      ccol_fatal_err("ctpool_construct_scoped('%s'): %s", #name,              \
                     _ctp_err ? _ctp_err : "unknown error");                  \
    }                                                                         \
  } while (0)

/* ========================================================================== */
/*                         TASK SUBMISSION                                    */
/* ========================================================================== */

/**
 * @brief Submit a task. It blocks when a bounded queue is full
 *
 * For an unbounded queue this function never blocks on capacity. The only
 * failure paths are ccol_invalid_args, ccol_not_permitted (the pool is in
 * shutdown) and ccol_not_enough_memory.
 * For a bounded queue this function blocks until there is space.
 *
 * In one case this function returns ccol_container_full and does not wait: a
 * call from a task that runs on this same pool, or from the on_complete
 * callback of that task, while a bounded queue is already at capacity. Only
 * the workers of this pool can free a slot, and the caller is one of them, so
 * nothing can ever satisfy the wait. Use ctpool_try_submit or
 * ctpool_timed_submit if the caller wants to handle a full queue itself, and
 * an unbounded queue if the caller must never get a refusal.
 *
 * on_complete runs exactly once for every task that a submit accepts, and
 * ran says whether fn ran. A worker calls on_complete(arg, true) right after
 * fn returns, on that worker thread. A task that the pool discards before any
 * worker takes it gets on_complete(arg, false) instead, and fn never runs.
 * ctpool_shutdown_immediate discards every task that is left in the queue,
 * also when it escalates a ctpool_shutdown_drain that is under way, and so
 * does a ctpool_destroy of a pool that this process inherited across fork().
 * The on_complete(arg, false) calls run on the thread that makes that call,
 * before the call returns, with no lock of the pool held, so on_complete can
 * release arg in both cases. A submit that fails (every return other than
 * ccol_success) accepts nothing and never calls on_complete.
 *
 * An on_complete(arg, false) call counts as a call from the on_complete
 * callback of a task of the pool, exactly as an on_complete(arg, true) call on
 * a worker does. From there, ctpool_wait, ctpool_shutdown_drain and
 * ctpool_shutdown_immediate on the same pool are no-ops that return at once,
 * ctpool_destroy on the same pool is a fatal error, and a submit to the same
 * pool returns ccol_not_permitted, because the pool is in shutdown (from the
 * ctpool_destroy of a pool inherited across fork() the handle is already
 * retired, so a submit returns ccol_invalid_args there). This holds for
 * every pool whose discarded callbacks the thread is running, also when such
 * a callback shuts down a second pool and the callbacks of that second pool
 * run nested inside it.
 *
 * @param pool        Thread pool handle
 * @param fn          Task function (must not be NULL)
 * @param arg         Argument that the library gives to fn and to on_complete
 *                    (may be NULL)
 * @param on_complete Called once for the task: with ran == true after fn
 *                    returns, or with ran == false when the pool discards the
 *                    task unrun (may be NULL)
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_INVALID or a
 *         stale/already-destroyed handle, or fn is NULL),
 *         ccol_not_enough_memory, ccol_not_permitted (the pool is in
 *         shutdown), or ccol_container_full (a bounded queue is at capacity
 *         and the caller is a worker of this same pool)
 */
ccol_retval_t ctpool_submit(ctpool pool, void (*fn)(void *), void *arg,
                            void (*on_complete)(void *arg, bool ran));

/**
 * @brief Submit a task. It never blocks
 *
 * It returns ccol_container_full at once if a bounded queue is at capacity.
 * For an unbounded queue the behaviour is the same as ctpool_submit.
 *
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_INVALID or a
 *         stale/already-destroyed handle, or fn is NULL),
 *         ccol_not_enough_memory, ccol_not_permitted, or
 *         ccol_container_full
 */
ccol_retval_t ctpool_try_submit(ctpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *arg, bool ran));

/**
 * @brief Submit a task with a timeout
 *
 * This function blocks for the timeout at most while it waits for space in a
 * bounded queue. The timeout is a duration in microseconds, measured from the
 * call, which the library converts to an absolute deadline internally with
 * CLOCK_MONOTONIC, so a change to the wall clock of the system does not make
 * the wait longer or shorter.
 *
 * A timeout_us of 0 does not wait: the call then does exactly what
 * ctpool_try_submit does, and returns ccol_container_full at once when a
 * bounded queue is at capacity. A timeout too large to form a deadline, such
 * as UINT64_MAX, waits as long as the queue stays full; the deadline
 * saturates and never wraps into the past.
 *
 * @param timeout_us Longest time to wait, in microseconds. 0 means try only
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_INVALID or a
 *         stale/already-destroyed handle, or fn is NULL),
 *         ccol_not_enough_memory, ccol_not_permitted, ccol_container_full,
 *         ccol_timed_out, or ccol_unexpected_failure
 */
ccol_retval_t ctpool_timed_submit(ctpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *arg, bool ran),
                                  uint64_t timeout_us);

/* ========================================================================== */
/*                         FUTURES                                            */
/* ========================================================================== */

/**
 * @brief Submit a task that returns a void* result. It can block
 *
 * The future handle that this function returns has a reference count of 2:
 * the caller holds one reference, and the queued task holds the other one
 * internally. Each ctpool_future_get call that is in progress on the future
 * adds one more. The caller must call ctpool_future_free exactly once, after
 * it finishes with the future.
 *
 * For a bounded queue this function blocks until there is space; use
 * ctpool_try_submit_future or ctpool_timed_submit_future if you do not want a
 * block.
 *
 * In one case this function returns NULL and does not wait: a call from a
 * task that runs on this same pool, or from the on_complete callback of that
 * task, while a bounded queue is already at capacity. The reason is the one
 * that the documentation of ctpool_submit gives: only the workers of this
 * pool can free a slot, and the caller is one of them.
 * ctpool_try_submit_future and ctpool_timed_submit_future report that case as
 * ccol_container_full. An unbounded queue never reaches it.
 *
 * @param pool  Thread pool handle
 * @param fn    Task function that returns a void* result (must not be NULL)
 * @param arg   Argument that the library gives to fn (may be NULL)
 * @return New future handle, or NULL in these four cases: there is not
 *         enough memory; the pool is in shutdown; a bounded queue is at
 *         capacity and the caller is a worker of this same pool; or pool is
 *         CTPOOL_INVALID, or a stale (already-destroyed) handle
 */
ctpool_future *ctpool_submit_future(ctpool pool, void *(*fn)(void *),
                                    void *arg);

/**
 * @brief Submit a future task. It never blocks
 *
 * It returns ccol_container_full at once if a bounded queue is at capacity.
 * For an unbounded queue the behaviour is the same as ctpool_submit_future.
 *
 * @param pool  Thread pool handle
 * @param fn    Task function that returns a void* result (must not be NULL)
 * @param arg   Argument that the library gives to fn (may be NULL)
 * @param out   It gets the future handle on success, and this function sets
 *              it to NULL on failure (must not be NULL)
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_INVALID or a
 *         stale/already-destroyed handle, or fn or out is NULL),
 *         ccol_not_enough_memory, ccol_not_permitted, or
 *         ccol_container_full
 */
ccol_retval_t ctpool_try_submit_future(ctpool pool, void *(*fn)(void *),
                                       void *arg, ctpool_future **out);

/**
 * @brief Submit a future task with a timeout
 *
 * This function blocks for the timeout at most while it waits for space in a
 * bounded queue. The timeout is a duration in microseconds, measured from the
 * call. The library converts it to an absolute deadline internally with
 * CLOCK_MONOTONIC, so a change to the wall clock of the system does not make
 * the wait longer or shorter. A timeout_us of 0 gives the same behaviour as
 * ctpool_try_submit_future, which never waits.
 *
 * A timeout too large to form a deadline, such as UINT64_MAX, waits as long
 * as the queue stays full; the deadline saturates and never wraps into the
 * past.
 *
 * @param pool    Thread pool handle
 * @param fn      Task function that returns a void* result (must not be NULL)
 * @param arg     Argument that the library gives to fn (may be NULL)
 * @param timeout_us Longest time to wait, in microseconds. 0 means try only
 * @param out     It gets the future handle on success, and this function sets
 *                it to NULL on failure (must not be NULL)
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_INVALID or a
 *         stale/already-destroyed handle, or fn or out is NULL),
 *         ccol_not_enough_memory, ccol_not_permitted, ccol_container_full,
 *         ccol_timed_out, or ccol_unexpected_failure
 */
ccol_retval_t ctpool_timed_submit_future(ctpool pool, void *(*fn)(void *),
                                         void *arg, uint64_t timeout_us,
                                         ctpool_future **out);

/**
 * @brief Block until the future has a result, then return that result
 *
 * It returns NULL if ctpool_shutdown_immediate cancelled the task. It does not
 * free the future, so call ctpool_future_free after it. The caller owns the
 * result from this point on; see the "Result ownership" note of this header
 * for what ctpool_future_free frees and does not free.
 *
 * Any number of threads can be inside this call on one future at the same
 * time, and every one of them gets the same result. Each of them holds a
 * reference of its own for the length of its call, so the future outlives
 * every waiter that is inside it, whatever the order of the other references.
 * The reference of the task goes when the task completes, and the reference
 * of the caller goes with ctpool_future_free; these two can happen in either
 * order. The thread that leaves the future last is the one that destroys it.
 */
void *ctpool_future_get(ctpool_future *f);

/**
 * @brief A check that never blocks. It is true if the future has a result,
 *        and also true if the library cancelled the future
 */
bool ctpool_future_done(ctpool_future *f);

/**
 * @brief True if ctpool_shutdown_immediate cancelled the future
 */
bool ctpool_future_cancelled(ctpool_future *f);

/**
 * @brief Give up the reference of the caller to the future
 *
 * The caller must call this function exactly once for each successful
 * ctpool_submit_future, ctpool_try_submit_future or ctpool_timed_submit_future
 * call, whether or not the caller ever calls ctpool_future_get. A call to this
 * function with no ctpool_future_get call at all gives fire-and-forget
 * behaviour: the library frees the future automatically after the worker
 * finishes, and it discards the result.
 *
 * The caller can call this function while other threads are inside
 * ctpool_future_get on the same future, because each of those threads holds a
 * reference of its own and the library destroys the future only after the
 * last of them leaves. This call ends the claim of the caller on f, in the
 * same way as when a thread gives up its last reference. The caller must not
 * use f again after this call, and must not hand f to a thread that is not
 * yet inside ctpool_future_get.
 */
void ctpool_future_free(ctpool_future *f);

/**
 * @brief Create a detached future. It has no pool and no queued task
 *
 * Unlike a future from ctpool_submit_future, a detached future has no
 * cthread_pool task queue behind it. It is a standalone handshake: the caller
 * waits, and a producer fulfills. An external producer, such as an
 * event-driven engine that is not a ctpool worker itself, must call
 * ctpool_future_fulfill() exactly once to deliver the result.
 *
 * Following the convention of ctpool_submit_future, the future starts with a
 * refcount of 2: one reference is for the caller, and ctpool_future_free gives
 * it up; one reference is for the producer, and ctpool_future_fulfill gives it
 * up. Each ctpool_future_get call that is in progress on the future adds one
 * more. The library frees the struct after the last reference goes, in
 * whatever order that happens, so the caller can call ctpool_future_free
 * before ctpool_future_fulfill, or call either one while other threads wait
 * in ctpool_future_get, exactly as for a pool-backed future.
 *
 * The library always allocates a detached future with plain malloc and frees
 * it with plain free, instead of the custom allocator of a pool or of the
 * caller; the futures of ctpool_submit_future work the same way. The lifetime
 * of a future is deliberately separate from any single allocator, so the
 * producer and the consumer do not need to agree on one.
 *
 * @param err_str Optional. It gets a description of the error on failure
 * @return New future handle (not done yet), or NULL if the allocation fails
 *
 * @see ctpool_future_fulfill
 */
ctpool_future *ctpool_future_create_detached(char **err_str);

/**
 * @brief Deliver a result to a detached future (an external producer
 *        completes it)
 *
 * The code that produces the result must call this function exactly once for
 * each future that ctpool_future_create_detached creates. Do not call this
 * function on a future from ctpool_submit_future, ctpool_try_submit_future or
 * ctpool_timed_submit_future, because the worker thread that runs the task of
 * such a future fulfills it internally.
 *
 * This function wakes every caller that is blocked in ctpool_future_get(),
 * then gives up the reference of the producer, and frees the future here if
 * that reference is the last one. It is the last one when the caller has
 * given up its own with ctpool_future_free and no ctpool_future_get call is
 * in progress.
 *
 * @param f      Future to fulfill. ctpool_future_create_detached must create
 *               it, and nothing must fulfill it before this call
 * @param result Result value to deliver, which ctpool_future_get() gives back
 * @return ccol_success on delivery
 * @return ccol_invalid_args if f is NULL
 * @return ccol_not_permitted if f was already fulfilled
 *
 * @see ctpool_future_create_detached
 */
ccol_retval_t ctpool_future_fulfill(ctpool_future *f, void *result);

/* ========================================================================== */
/*                         POOL MANAGEMENT                                    */
/* ========================================================================== */

/**
 * @brief Block until the task queue is empty and every active task is done
 *
 * This function does not shut the pool down: the workers stay alive, and the
 * pool accepts new tasks after this call returns.
 *
 * Contract: no other thread must submit a task at the same time as this call,
 * because with an unbounded queue, a submission from another thread can block
 * this call forever.
 *
 * This call waits for the queue and for the tasks that workers run, but not
 * for the on_complete callbacks of discarded tasks. A
 * ctpool_shutdown_immediate on another thread empties the queue at once and
 * then calls the on_complete of each discarded task, with ran == false, on
 * its own thread, so this call can return while those callbacks are running.
 * Wait for the return of the ctpool_shutdown_immediate call itself to know
 * that every one of them finished.
 *
 * This function is a no-op if pool is CTPOOL_INVALID or a stale
 * (already-destroyed) handle. It is also a no-op that returns at once for a
 * call on pool from inside a task that runs on one of the worker threads of
 * pool itself, or from the on_complete callback of that task. The pool counts
 * the calling task among its own active tasks until that task returns, so
 * without this no-op, a wait here deadlocks the calling worker against itself.
 */
void ctpool_wait(ctpool pool);

/**
 * @brief Graceful shutdown: finish all queued tasks, then stop the workers
 *
 * This function blocks until every queued task and every active task
 * completes, and the pool accepts no new task after this call. The caller
 * must call this function before ctpool_destroy, unless it calls
 * ctpool_shutdown_immediate instead.
 *
 * A second call, sequential or concurrent, starts nothing new, but it blocks
 * until the shutdown that is already under way stops. If a
 * ctpool_shutdown_immediate arrives while this call is draining the queue,
 * the pool discards the rest of the queue, and this call returns after every
 * worker exits.
 *
 * This function is a no-op if pool is CTPOOL_INVALID or a stale
 * (already-destroyed) handle. It is also a no-op that does not block for a
 * call on pool from inside a task that runs on one of the worker threads of
 * pool itself, or from the on_complete callback of that task. Because a
 * worker thread cannot join itself, such a call does not stop the pool from
 * accepting new tasks and joins no worker, so the pool stays fully usable for
 * a later, legitimate shutdown call from outside.
 */
void ctpool_shutdown_drain(ctpool pool);

/**
 * @brief Immediate shutdown: cancel the queued tasks, stop after the current
 *        ones
 *
 * The pool discards every task that is left in the queue, and the future
 * handles of those tasks become cancelled. Each callback task that the pool
 * discards gets its on_complete called with ran == false, on the thread that
 * calls this function, before it returns; see ctpool_submit. Each worker
 * finishes its current task and then exits, and this function blocks until
 * every worker thread exits.
 *
 * A call to this function while a ctpool_shutdown_drain is already in progress
 * escalates that drain: the pool discards what is left in the queue at that
 * moment, cancels the futures of those tasks, and calls the on_complete of
 * each discarded callback task with ran == false. The result is exactly the
 * same as if this call starts the shutdown. A second ctpool_shutdown_immediate
 * starts nothing new, but it blocks until the shutdown that is already under
 * way stops.
 *
 * This function is a no-op if pool is CTPOOL_INVALID or a stale
 * (already-destroyed) handle. It is also a no-op for a call on pool from
 * inside a task that runs on one of the worker threads of pool itself, or
 * from the on_complete callback of that task: such a call does not block and
 * discards nothing, for the same reason as in ctpool_shutdown_drain.
 */
void ctpool_shutdown_immediate(ctpool pool);

/**
 * @brief Number of tasks that are in the queue now (no worker took them yet)
 *
 * It returns 0 if pool is CTPOOL_INVALID or a stale (already-destroyed)
 * handle.
 */
size_t ctpool_pending_count(ctpool pool);

/**
 * @brief Number of tasks that the worker threads execute now
 *
 * It returns 0 if pool is CTPOOL_INVALID or a stale (already-destroyed)
 * handle.
 */
size_t ctpool_active_count(ctpool pool);

#pragma GCC visibility pop
