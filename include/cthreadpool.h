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

/* Every declaration from here to the end of this header belongs to the public
 * ABI of libccollections, and the shared library exports all of them. The
 * library itself is built with -fvisibility=hidden, so a function or object
 * outside these blocks stays internal: it isn't in the dynamic symbol table,
 * an application that links against the library can't interpose it, and a
 * symbol of the same name in that application can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cthreadpool.h
 * @brief Generic thread pool with bounded or unbounded task queue, completion
 *        callbacks, and futures.
 *
 * You pick one of two queue modes when you create the pool:
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
 *   ctpool_future *f = ctpool_submit_future(pool, fn, arg, NULL);
 *   void *result = ctpool_future_get(f);
 *   ctpool_future_free(f);
 *
 * Result ownership: the pool doesn't know what the void* result points to or
 * how the task allocated it, so it never allocates, copies or frees the
 * result. ctpool_future_free frees only the future's own bookkeeping (the
 * handle that ctpool_submit_future returns), never the result payload. Once
 * a thread calls ctpool_future_get it owns that payload, and it has to free
 * it the same way the task function allocated it. The same rule holds for
 * the result argument of ctpool_future_fulfill on a detached future.
 *
 * Thread safety: every public function can run at the same time as every
 * other one on the same live handle, and that includes a race between
 * ctpool_shutdown_drain, ctpool_shutdown_immediate and __ctpool_destroy. An
 * internal resolve and pin mechanism makes this safe; the doc comment of
 * __ctpool_destroy describes it.
 *
 * ctpool_shutdown_drain and ctpool_shutdown_immediate are each idempotent: a
 * second call to either one, sequential or concurrent, does no shutdown work
 * of its own. It does block until the shutdown already under way stops,
 * though, so every caller gets the same guarantee on return: no worker is
 * alive.
 *
 * There is one exception to "no more work". A ctpool_shutdown_immediate call
 * that arrives while a ctpool_shutdown_drain is draining the queue escalates
 * the shutdown, which discards what is left in the queue and cancels the
 * futures of those tasks.
 *
 * The one hard restriction is __ctpool_destroy itself. Calling it again on a
 * handle whose destroy has completed is a fatal error, and so is racing a
 * second, concurrent __ctpool_destroy on the same live handle. Neither one is
 * a safe no-op.
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
 * Never cast a ctpool to or from void*, never compare it with a pointer cast,
 * and never treat it as an address. Compare it against CTPOOL_NULL, or use
 * it in a truthiness check: CTPOOL_NULL is 0, so `if (!pool)` is a valid
 * test for an invalid handle.
 *
 * The library resolves every use of a ctpool through a slot table of its own
 * before it touches the cthread_pool object behind the handle, so it always
 * finds a stale handle (one whose pool is already destroyed) and never
 * silently dereferences freed memory. Calling __ctpool_destroy with a stale
 * or already-destroyed handle is a fatal error; see the doc comment of that
 * function.
 */
typedef uint64_t ctpool;

/** @brief Sentinel value for "no pool" / "not yet created" / "destroyed" */
#define CTPOOL_NULL ((ctpool)0)

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
 * @return New pool handle, or CTPOOL_NULL on failure
 *
 * @note The pool recycles the internal node behind each submission through
 * its own cache, which keeps what the recent traffic needed, up to
 * max(num_threads * 4, 32768) idle nodes. At every 256th point where the pool
 * goes idle it gives half of what it holds above num_threads * 4 back to
 * mprocs, unless the traffic since the previous such point emptied the cache.
 * So mprocs doesn't see one allocation and one free per submission.
 * @note Each worker thread records its pool on one thread-specific key, which
 * the library creates for the whole process the first time a pool is
 * created; that key is what lets a task that destroys its own pool fail
 * loudly. When the process has no key left to give (PTHREAD_KEYS_MAX), this
 * function returns CTPOOL_NULL, and so does every later call in that
 * process. The library deletes the key when it unloads with no pool left, so
 * a dlopen() and dlclose() cycle gives the key back
 */
ctpool ctpool_create_mp(size_t num_threads, size_t queue_capacity,
                        ccol_memmgmt_procs_t *mprocs, char **err_str);

/**
 * @brief Create a thread pool with default memory management
 *
 * This is the same as ctpool_create_mp with mprocs = NULL.
 */
static inline __attribute__((always_inline)) ctpool
ctpool_create(size_t num_threads, size_t queue_capacity, char **err_str) {
  return ctpool_create_mp(num_threads, queue_capacity, NULL, err_str);
}

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/**
 * @brief Internal destroy. Use the ctpool_destroy macro instead
 *
 * pool is an opaque VALUE handle, and the library resolves it through its own
 * slot table before it touches the pool object behind it. pool must be live
 * at this moment, meaning ctpool_create or ctpool_create_mp returned it and
 * nothing has destroyed it yet. CTPOOL_NULL is a silent no-op.
 *
 * Every other stale handle is a FATAL ERROR: a handle that an earlier,
 * completed call to this function destroyed, a handle that another thread is
 * destroying right now in a race with this call, and a forged or garbage
 * value too. This function calls _ccol_fatal_err() (abort()/SIGABRT) for all
 * of them. Without this, a plain sequential double-destroy risks a
 * use-after-free or a double-free, and so does a concurrent destroy that
 * overlaps in time.
 *
 * Calling this function on pool from inside a task, or from that task's
 * on_complete callback, is also a FATAL ERROR when the task runs on one of
 * pool's own worker threads. A destroy there frees the pool's mutex,
 * condition variables and struct while that worker is on its way back
 * through its dispatch loop, and the worker touches all of them again right
 * after.
 *
 * On a live handle this function waits until every in-flight resolved use of
 * the handle has stopped, that is, a call to ctpool_submit, _try_submit,
 * _timed_submit, _submit_future, _try_submit_future, _timed_submit_future,
 * _wait, _shutdown_drain, _shutdown_immediate, _pending_count or _active_count
 * running on another thread at this moment. If neither ctpool_shutdown_drain
 * nor ctpool_shutdown_immediate was called before, a drain shutdown runs
 * first, before that wait, so a submitter blocked on a full bounded queue can
 * rely on the shutdown's broadcast to release its pin.
 */
void __ctpool_destroy(ctpool pool);

/**
 * @brief RAII cleanup function (used with _ccol_destructor)
 */
static inline __attribute__((always_inline)) void ___ctpool_destroy(
    ctpool *pp) {
  if (pp && *pp) {
    __ctpool_destroy(*pp);
    *pp = CTPOOL_NULL;
  }
}

/**
 * @brief Destroy a thread pool and set handle to CTPOOL_NULL
 *
 * Calling this macro with a stale handle is a FATAL ERROR (abort()/SIGABRT),
 * not a silent double-free. A stale handle is one that an earlier call
 * destroyed, or one that another thread is destroying right now in a race
 * with this call.
 *
 * Calling it on pool from inside a task, or from that task's on_complete
 * callback, is also a FATAL ERROR when the task runs on one of pool's own
 * worker threads, for the same reason: that worker is on its way back
 * through its dispatch loop and touches the pool's mutex, condition variables
 * and struct again right after the destroy frees them.
 *
 * @note The macro evaluates pool exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define ctpool_destroy(pool) \
  _ccol_ctpool_destroy_impl( \
      pool, _ccol_uniq(__ccol_ctpool_destroy_slot, __COUNTER__))

/* Internal: the body of ctpool_destroy. slot is a name from _ccol_uniq(), so
 * the macro can nest inside the argument of another destroy macro and stay
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_ctpool_destroy_impl(pool, slot) \
  do {                                        \
    __typeof__(pool) *slot = &(pool);         \
    __ctpool_destroy(*slot);                  \
    *slot = CTPOOL_NULL;                      \
  } while (0)

/* ========================================================================== */
/*                    DECLARE / CONSTRUCT / SCOPED MACROS                     */
/* ========================================================================== */

/**
 * @brief Declare an uninitialised pool variable
 *
 * Follow it with a ctpool_construct or a ctpool_create* call.
 */
#define ctpool_declare(name) ctpool name

/**
 * @brief Declare with automatic destruction on scope exit
 */
#define ctpool_declare_scoped(name) \
  ctpool name _ccol_destructor(___ctpool_destroy) = CTPOOL_NULL

/**
 * @brief Declare and initialise in one step. It calls _ccol_fatal_err on
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
#define ctpool_construct(name, num_threads, queue_capacity)             \
  ctpool name = CTPOOL_NULL;                                            \
  do {                                                                  \
    char *_ctp_err = NULL;                                              \
    (name) = ctpool_create((num_threads), (queue_capacity), &_ctp_err); \
    if (!(name)) {                                                      \
      _ccol_fatal_err("ctpool_construct('%s'): %s", #name,              \
                      _ctp_err ? _ctp_err : "unknown error");           \
    }                                                                   \
  } while (0)

/**
 * @brief Declare, initialise, and auto-destroy at scope exit. It calls
 *        _ccol_fatal_err on failure
 */
#define ctpool_construct_scoped(name, num_threads, queue_capacity)      \
  ctpool name _ccol_destructor(___ctpool_destroy) = CTPOOL_NULL;        \
  do {                                                                  \
    char *_ctp_err = NULL;                                              \
    (name) = ctpool_create((num_threads), (queue_capacity), &_ctp_err); \
    if (!(name)) {                                                      \
      _ccol_fatal_err("ctpool_construct_scoped('%s'): %s", #name,       \
                      _ctp_err ? _ctp_err : "unknown error");           \
    }                                                                   \
  } while (0)

/* ========================================================================== */
/*                         TASK SUBMISSION                                    */
/* ========================================================================== */

/**
 * @brief Submit a task. It blocks when a bounded queue is full
 *
 * With an unbounded queue this function never blocks on capacity, and the
 * only failure paths are ccol_invalid_args, ccol_not_permitted (the pool is
 * in shutdown) and ccol_not_enough_memory. With a bounded queue it blocks
 * until there is space.
 *
 * There is one case where it returns ccol_container_full without waiting: a
 * call from a task running on this same pool, or from that task's
 * on_complete callback, while a bounded queue is already at capacity. Only
 * this pool's workers can free a slot and the caller is one of them, so
 * nothing could ever satisfy the wait. Use ctpool_try_submit or
 * ctpool_timed_submit if you want to handle a full queue yourself, and an
 * unbounded queue if you must never get a refusal.
 *
 * on_complete runs exactly once for every task that a submit accepts, and
 * ran tells whether fn ran. A worker calls on_complete(arg, true) on its own
 * thread right after fn returns. A task that the pool discards before any
 * worker takes it gets on_complete(arg, false) instead, and fn never runs.
 * ctpool_shutdown_immediate discards every task left in the queue (also when
 * it escalates a ctpool_shutdown_drain that is under way), and so does a
 * ctpool_destroy of a pool that this process inherited across fork(). Those
 * on_complete(arg, false) calls run on the thread that makes that call,
 * before it returns and with no lock of the pool held, so on_complete can
 * release arg in both cases. A submit that fails (any return other than
 * ccol_success) accepts nothing and never calls on_complete.
 *
 * An on_complete(arg, false) call counts as a call from the on_complete
 * callback of a task of the pool, the same as an on_complete(arg, true) call
 * on a worker. From there, ctpool_wait, ctpool_shutdown_drain and
 * ctpool_shutdown_immediate on the same pool are no-ops that return at once,
 * ctpool_destroy on the same pool is a fatal error, and a submit to the same
 * pool returns ccol_not_permitted because the pool is in shutdown (inside
 * the ctpool_destroy of a pool inherited across fork() the handle is already
 * retired, so a submit returns ccol_invalid_args there). This holds for
 * every pool whose discarded callbacks the thread is running, also when such
 * a callback shuts down a second pool and that second pool's callbacks run
 * nested inside it.
 *
 * @param pool        Thread pool handle
 * @param fn          Task function (must not be NULL)
 * @param arg         Argument that the library gives to fn and to on_complete
 *                    (may be NULL)
 * @param on_complete Called once for the task: with ran == true after fn
 *                    returns, or with ran == false when the pool discards the
 *                    task unrun (may be NULL)
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_NULL or a
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
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_NULL or a
 *         stale/already-destroyed handle, or fn is NULL),
 *         ccol_not_enough_memory, ccol_not_permitted, or
 *         ccol_container_full
 */
ccol_retval_t ctpool_try_submit(ctpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *arg, bool ran));

/**
 * @brief Submit a task with a timeout
 *
 * While it waits for space in a bounded queue this function blocks for the
 * timeout at most. The timeout is a duration in microseconds counted from the
 * call, and the library turns it into an absolute CLOCK_MONOTONIC deadline,
 * so a change to the system's wall clock doesn't make the wait longer or
 * shorter.
 *
 * A timeout_us of 0 doesn't wait: the call does exactly what
 * ctpool_try_submit does and returns ccol_container_full at once when a
 * bounded queue is at capacity. A timeout too large to form a deadline, such
 * as UINT64_MAX, waits as long as the queue stays full; the deadline
 * saturates and never wraps into the past.
 *
 * A call from a task running on this same pool, or from that task's
 * on_complete callback, never waits while a bounded queue is at capacity,
 * because only this pool's workers can free a slot and the caller is one of
 * them. It returns ccol_container_full at once, whatever timeout_us says.
 *
 * @param timeout_us Longest time to wait, in microseconds. 0 means try only
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_NULL or a
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
 * The returned future has a reference count of 2: one reference belongs to
 * the caller and the queued task holds the other one internally. Each
 * ctpool_future_get call in progress on the future adds one more. You must
 * call ctpool_future_free exactly once, when you are done with the future.
 *
 * With a bounded queue this function blocks until there is space; use
 * ctpool_try_submit_future or ctpool_timed_submit_future if you don't want
 * it to block.
 *
 * There is one case where it fails with ccol_container_full without
 * waiting: a call from a task running on this same pool, or from that
 * task's on_complete callback, while a bounded queue is already at capacity.
 * The reason is the one ctpool_submit gives: only this pool's workers can
 * free a slot and the caller is one of them. An unbounded queue never gets
 * there.
 *
 * @param pool   Thread pool handle
 * @param fn     Task function that returns a void* result (must not be NULL)
 * @param arg    Argument that the library gives to fn (may be NULL)
 * @param rv_out If it is not NULL, this function stores the result code here
 *               on every return: ccol_success when it returns a future;
 *               ccol_invalid_args when pool is CTPOOL_NULL or a
 *               stale/already-destroyed handle, or fn is NULL;
 *               ccol_not_permitted when the pool is in shutdown;
 *               ccol_container_full when a bounded queue is at capacity and
 *               the caller is a worker of this same pool; and
 *               ccol_not_enough_memory when an allocation fails. These are
 *               the codes that ctpool_submit returns for the same conditions
 * @return New future handle, or NULL on failure
 */
ctpool_future *ctpool_submit_future(ctpool pool, void *(*fn)(void *), void *arg,
                                    ccol_retval_t *rv_out);

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
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_NULL or a
 *         stale/already-destroyed handle, or fn or out is NULL),
 *         ccol_not_enough_memory, ccol_not_permitted, or
 *         ccol_container_full
 */
ccol_retval_t ctpool_try_submit_future(ctpool pool, void *(*fn)(void *),
                                       void *arg, ctpool_future **out);

/**
 * @brief Submit a future task with a timeout
 *
 * While it waits for space in a bounded queue this function blocks for the
 * timeout at most. The timeout is a duration in microseconds counted from the
 * call, and the library turns it into an absolute CLOCK_MONOTONIC deadline,
 * so a change to the system's wall clock doesn't make the wait longer or
 * shorter. A timeout_us of 0 behaves like ctpool_try_submit_future, which
 * never waits.
 *
 * A timeout too large to form a deadline, such as UINT64_MAX, waits as long
 * as the queue stays full; the deadline saturates and never wraps into the
 * past.
 *
 * A call from a task running on this same pool, or from that task's
 * on_complete callback, never waits while a bounded queue is at capacity,
 * because only this pool's workers can free a slot and the caller is one of
 * them. It returns ccol_container_full at once, whatever timeout_us says.
 *
 * @param pool    Thread pool handle
 * @param fn      Task function that returns a void* result (must not be NULL)
 * @param arg     Argument that the library gives to fn (may be NULL)
 * @param timeout_us Longest time to wait, in microseconds. 0 means try only
 * @param out     It gets the future handle on success, and this function sets
 *                it to NULL on failure (must not be NULL)
 * @return ccol_success, ccol_invalid_args (pool is CTPOOL_NULL or a
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
 * It returns NULL if ctpool_shutdown_immediate cancelled the task. It doesn't
 * free the future, so call ctpool_future_free after it. From here on the
 * caller owns the result; the "Result ownership" note of this header says
 * what ctpool_future_free frees and what it doesn't.
 *
 * Any number of threads can be inside this call on one future at the same
 * time, and all of them get the same result. Each one holds its own
 * reference for the length of its call, so the future outlives every waiter
 * inside it, whatever order the other references go in. The task's
 * reference goes when the task completes and the caller's goes with
 * ctpool_future_free, in either order, and whichever thread leaves the
 * future last destroys it.
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
 * You must call this function exactly once for each successful
 * ctpool_submit_future, ctpool_try_submit_future or ctpool_timed_submit_future
 * call, whether or not you ever call ctpool_future_get. Calling it without
 * any ctpool_future_get gives fire-and-forget behaviour: the library frees
 * the future once the worker finishes, and the result is thrown
 * away.
 *
 * It's fine to call this while other threads are inside ctpool_future_get
 * on the same future, because each of them holds its own reference and the
 * library destroys the future only after the last one leaves. The call ends
 * the caller's claim on f, the same way a thread gives up its last
 * reference. After it you must not use f again, and you must not hand f to
 * a thread that isn't already inside ctpool_future_get.
 */
void ctpool_future_free(ctpool_future *f);

/**
 * @brief Create a detached future. It has no pool and no queued task
 *
 * A detached future, unlike one from ctpool_submit_future, has no
 * cthread_pool task queue behind it. It's a standalone handshake where the
 * caller waits and a producer fulfills. The external producer (an
 * event-driven engine that isn't a ctpool worker, for example) must call
 * ctpool_future_fulfill() exactly once to deliver the result.
 *
 * As with ctpool_submit_future, the future starts with a refcount of 2: one
 * reference for the caller, which ctpool_future_free gives up, and one for
 * the producer, which ctpool_future_fulfill gives up. Each ctpool_future_get
 * call in progress on the future adds one more. The struct is freed after
 * the last reference goes, in whatever order that happens, so you can call
 * ctpool_future_free before ctpool_future_fulfill, or call either one while
 * other threads wait in ctpool_future_get, exactly as for a pool-backed
 * future.
 *
 * A detached future is always allocated with plain malloc and freed with
 * plain free, never with the custom allocator of a pool or of the caller,
 * and the futures of ctpool_submit_future work the same way. A future's
 * lifetime is kept apart from any single allocator on purpose, so the
 * producer and the consumer don't have to agree on one.
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
 * each future that ctpool_future_create_detached creates. Don't call it on a
 * future from ctpool_submit_future, ctpool_try_submit_future or
 * ctpool_timed_submit_future, because the worker thread that runs such a
 * future's task fulfills it internally.
 *
 * This function wakes every caller blocked in ctpool_future_get(), then
 * gives up the producer's reference and frees the future right here if that
 * reference was the last one. It is the last one when the caller has already
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
 * This doesn't shut the pool down: the workers stay alive and the pool takes
 * new tasks after the call returns.
 *
 * Contract: no other thread may submit a task while this call runs, because
 * with an unbounded queue a submission from another thread can block this
 * call forever.
 *
 * The call waits for the queue and for the tasks the workers run, but not
 * for the on_complete callbacks of discarded tasks. A
 * ctpool_shutdown_immediate on another thread empties the queue at once and
 * then calls each discarded task's on_complete, with ran == false, on its own
 * thread, so this call can return while those callbacks are still running.
 * If you need to know that all of them finished, wait for the
 * ctpool_shutdown_immediate call itself to return.
 *
 * This function is a no-op if pool is CTPOOL_NULL or a stale
 * (already-destroyed) handle. It is also a no-op that returns at once when
 * it's called on pool from inside a task running on one of pool's own worker
 * threads, or from that task's on_complete callback. The pool counts the
 * calling task among its active tasks until the task returns, so without
 * this no-op a wait here would deadlock the calling worker against itself.
 */
void ctpool_wait(ctpool pool);

/**
 * @brief Graceful shutdown: finish all queued tasks, then stop the workers
 *
 * This function blocks until every queued and every active task completes,
 * and after it the pool accepts no new task. Calling it before
 * ctpool_destroy is optional, because ctpool_destroy runs this same drain by
 * itself when neither this function nor ctpool_shutdown_immediate was
 * called.
 *
 * A second call, sequential or concurrent, starts nothing new but blocks
 * until the shutdown already under way stops. If a ctpool_shutdown_immediate
 * arrives while this call is draining the queue, the pool discards the rest
 * of the queue and this call returns once every worker has exited.
 *
 * This function is a no-op if pool is CTPOOL_NULL or a stale
 * (already-destroyed) handle. It is also a no-op that doesn't block when
 * it's called on pool from inside a task running on one of pool's own worker
 * threads, or from that task's on_complete callback. A worker thread can't
 * join itself, so such a call doesn't stop the pool from accepting new tasks
 * and joins no worker, and the pool stays fully usable for a later,
 * legitimate shutdown call from outside.
 */
void ctpool_shutdown_drain(ctpool pool);

/**
 * @brief Immediate shutdown: cancel the queued tasks, stop after the current
 *        ones
 *
 * The pool discards every task left in the queue, and the futures of those
 * tasks become cancelled. Each discarded callback task gets its on_complete
 * called with ran == false on the thread that calls this function, before it
 * returns (see ctpool_submit). Each worker finishes its current task and then
 * exits, and this function blocks until every worker thread has exited.
 *
 * Called while a ctpool_shutdown_drain is already in progress, this function
 * escalates that drain: the pool discards what is left in the queue at that
 * moment, cancels the futures of those tasks and calls the on_complete of
 * each discarded callback task with ran == false. The result is exactly the
 * same as if this call had started the shutdown. A second
 * ctpool_shutdown_immediate starts nothing new but blocks until the shutdown
 * already under way stops.
 *
 * This function is a no-op if pool is CTPOOL_NULL or a stale
 * (already-destroyed) handle. It is also a no-op when it's called on pool
 * from inside a task running on one of pool's own worker threads, or from
 * that task's on_complete callback: such a call doesn't block and discards
 * nothing, for the same reason as in ctpool_shutdown_drain.
 */
void ctpool_shutdown_immediate(ctpool pool);

/**
 * @brief Number of tasks waiting in the queue right now (no worker has taken
 *        them yet)
 *
 * It returns 0 if pool is CTPOOL_NULL or a stale (already-destroyed)
 * handle.
 */
size_t ctpool_pending_count(ctpool pool);

/**
 * @brief Number of tasks the worker threads are running right now
 *
 * It returns 0 if pool is CTPOOL_NULL or a stale (already-destroyed)
 * handle.
 */
size_t ctpool_active_count(ctpool pool);

#pragma GCC visibility pop
