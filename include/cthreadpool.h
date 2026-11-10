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

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// A generic thread pool. With queue_capacity 0 the task queue is unbounded and
// a submit never waits for room; with a capacity N a submit waits while N tasks
// are queued (cthrpool_try_submit refuses instead). cthrpool_shutdown_drain
// finishes the queue before the workers stop, cthrpool_shutdown_immediate
// discards it.
//
// Every public function may run at the same time as any other on the same live
// handle, shutdowns and destroy included. Both shutdowns are idempotent: a
// second call does no work of its own but blocks until no worker is alive. An
// immediate shutdown that arrives during a drain escalates it and discards what
// is left in the queue.
//
// The pool never allocates, copies or frees the void * result of a future.
// Whoever calls cthrpool_future_get owns it and frees it the way the task
// allocated it; cthrpool_future_free frees only the future itself.
typedef struct cthread_pool cthread_pool;

// The handle is an opaque value (slot index plus generation), never a pointer.
// Don't cast it to or from void *. Compare it with CTHRPOOL_NULL, which is 0,
// so `if (!pool)` works. Every use goes through the library's slot table, so a
// stale handle is always detected and never dereferenced.
typedef uint64_t cthrpool;

// "No pool": the failure value of a create and the value a destroy leaves in
// the handle, so a handle can be tested for validity.
#define CTHRPOOL_NULL ((cthrpool)0)

// A future holds the void * result of a task that may not have run yet.
typedef struct cthrpool_future cthrpool_future;

// Creates a pool of num_threads (at least 1) workers that allocates through
// mprocs (NULL for malloc and free). It's the one constructor behind
// cthrpool_create() and the construct macros. queue_capacity 0 means an
// unbounded queue, N a bounded one; ccol_invalid_size is refused. Gives the
// handle, or CTHRPOOL_NULL with an error string in *err_str when err_str isn't
// NULL.
//
// The pool keeps idle task nodes for reuse (up to max(num_threads * 4, 32768)),
// so mprocs doesn't see an allocation and a free per submission. Workers mark
// themselves on one thread-specific key that the library creates for the whole
// process; when no key is left (PTHREAD_KEYS_MAX) every create fails. The key
// is deleted when the library unloads with no pool left.
cthrpool cthrpool_create_mp(size_t num_threads, size_t queue_capacity,
                            ccol_memmgmt_procs *mprocs, char **err_str);

// Creates a pool with the default allocator, for callers that don't need a
// custom one; see cthrpool_create_mp().
static inline __attribute__((always_inline)) cthrpool
cthrpool_create(size_t num_threads, size_t queue_capacity, char **err_str) {
  return cthrpool_create_mp(num_threads, queue_capacity, NULL, err_str);
}

// Internal: destroys a pool. It's the body of cthrpool_destroy(); use the
// macro, which also clears the handle.
//
// CTHRPOOL_NULL is a no-op. Any other handle that isn't live (already
// destroyed, being destroyed by another thread, or garbage) is a fatal error,
// and so is a destroy from a task or on_complete callback running on this
// pool's own worker. When no shutdown ran, a drain shutdown runs first; then it
// waits for every call that resolved the handle on another thread to leave
// before it frees anything.
void cthrpool__destroy(cthrpool pool);

// Internal: destroys *pp and clears it. The _scoped macros attach it with
// ccol__destructor so a pool goes away with its scope.
static inline __attribute__((always_inline)) void cthrpool__destroy_cleanup(
    cthrpool *pp) {
  if (pp && *pp) {
    cthrpool__destroy(*pp);
    *pp = CTHRPOOL_NULL;
  }
}

// Destroys the pool and sets the handle to CTHRPOOL_NULL, so the variable can't
// be used again by mistake. This is how a pool is destroyed. pool must be a
// modifiable lvalue and is evaluated once. A stale handle, or a destroy from
// one of the pool's own tasks or on_complete callbacks, is a fatal error.
#define cthrpool_destroy(pool) \
  ccol__cthrpool_destroy_impl( \
      pool, ccol__uniq(ccol__cthrpool_destroy_slot, __COUNTER__))

// Internal: the body of cthrpool_destroy(), destroying *&(pool) and clearing
// it. slot comes from ccol__uniq(), so destroy macros can nest without -Wshadow
// warnings.
#define ccol__cthrpool_destroy_impl(pool, slot) \
  do {                                          \
    __typeof__(pool) *slot = &(pool);           \
    cthrpool__destroy(*slot);                   \
    *slot = CTHRPOOL_NULL;                      \
  } while (0)

// Declares a pool variable called name, for code that creates the pool later
// with cthrpool_create*.
#define cthrpool_declare(name) cthrpool name

// Declares a pool variable called name, set to CTHRPOOL_NULL, that is destroyed
// when it goes out of scope, so no exit path can leak the pool.
#define cthrpool_declare_scoped(name) \
  cthrpool name ccol__destructor(cthrpool__destroy_cleanup) = CTHRPOOL_NULL

// Declares a pool variable called name and creates the pool in one step, for
// code that treats a failed create as fatal: it calls ccol__fatal_err when the
// create fails.
//
//   cthrpool_construct(pool, 4, 256);
//   cthrpool_submit(pool, my_fn, my_arg, NULL);
//   cthrpool_shutdown_drain(pool);
//   cthrpool_destroy(pool);
#define cthrpool_construct(name, num_threads, queue_capacity)                  \
  cthrpool name = CTHRPOOL_NULL;                                               \
  do {                                                                         \
    char *cthrpool__err = NULL;                                                \
    (name) = cthrpool_create((num_threads), (queue_capacity), &cthrpool__err); \
    if (!(name)) {                                                             \
      ccol__fatal_err("cthrpool_construct('%s'): %s", #name,                   \
                      cthrpool__err ? cthrpool__err : "unknown error");        \
    }                                                                          \
  } while (0)

// cthrpool_construct() for a pool that is destroyed at the end of its scope, so
// no exit path can leak it.
#define cthrpool_construct_scoped(name, num_threads, queue_capacity)           \
  cthrpool name ccol__destructor(cthrpool__destroy_cleanup) = CTHRPOOL_NULL;   \
  do {                                                                         \
    char *cthrpool__err = NULL;                                                \
    (name) = cthrpool_create((num_threads), (queue_capacity), &cthrpool__err); \
    if (!(name)) {                                                             \
      ccol__fatal_err("cthrpool_construct_scoped('%s'): %s", #name,            \
                      cthrpool__err ? cthrpool__err : "unknown error");        \
    }                                                                          \
  } while (0)

// Queues fn(arg) to run on a worker. It's the ordinary way to hand work to the
// pool, and it waits for room on a full bounded queue, which gives a producer
// backpressure. The exception is a caller that is a task or on_complete
// callback of this same pool: only the workers can free a slot, so it gives
// ccol_container_full at once. It gives ccol_success, ccol_invalid_args (bad
// handle or NULL fn), ccol_not_permitted (shutdown started),
// ccol_not_enough_memory or that ccol_container_full.
//
// on_complete (may be NULL) runs exactly once for every accepted task: on the
// worker with ran == true right after fn, or with ran == false when a shutdown
// or the destroy of a pool inherited across fork() discards the task, so it can
// always release arg. The ran == false calls run on the thread that discards,
// before it returns, with no pool lock held. Such a call counts as a callback
// of the pool: a wait or shutdown of the same pool from it is a no-op, a
// destroy is fatal and a submit gives ccol_not_permitted (ccol_invalid_args
// inside the destroy of an inherited pool). A failed submit accepts nothing and
// never calls on_complete.
ccol_retval cthrpool_submit(cthrpool pool, void (*fn)(void *), void *arg,
                            void (*on_complete)(void *arg, bool ran));

// Queues fn(arg) like cthrpool_submit() but never waits, for a producer that
// must not block: a full bounded queue gives ccol_container_full at once.
ccol_retval cthrpool_try_submit(cthrpool pool, void (*fn)(void *), void *arg,
                                void (*on_complete)(void *arg, bool ran));

// Queues fn(arg) like cthrpool_submit(), waiting at most timeout_us
// microseconds for room, for a producer that can wait a little but not for
// ever. The deadline is on CLOCK_MONOTONIC, so wall clock changes don't affect
// it. 0 behaves like cthrpool_try_submit(), and a huge value such as
// UINT64_MAX saturates and waits as long as the queue stays full. A call from
// this pool's own task never waits. Besides the submit codes it can give
// ccol_timed_out or ccol_unexpected_failure.
ccol_retval cthrpool_timed_submit(cthrpool pool, void (*fn)(void *), void *arg,
                                  void (*on_complete)(void *arg, bool ran),
                                  uint64_t timeout_us);

// Queues fn(arg) and gives a future for its void * result, so the submitting
// thread can collect what the task computed. NULL on failure. It waits for room
// like cthrpool_submit(), with the same self-call exception. When rv_out isn't
// NULL it gets the result code on every return, the same code
// cthrpool_submit() gives for the same condition. Call cthrpool_future_free()
// exactly once for every future you get.
cthrpool_future *cthrpool_submit_future(cthrpool pool, void *(*fn)(void *),
                                        void *arg, ccol_retval *rv_out);

// cthrpool_submit_future() for a caller that must not block: it never waits.
// The future goes to *out, which is set to NULL on failure; out must not be
// NULL.
ccol_retval cthrpool_try_submit_future(cthrpool pool, void *(*fn)(void *),
                                       void *arg, cthrpool_future **out);

// cthrpool_submit_future() that waits at most timeout_us microseconds for room,
// with the rules of cthrpool_timed_submit(). The future goes to *out, which is
// set to NULL on failure.
ccol_retval cthrpool_timed_submit_future(cthrpool pool, void *(*fn)(void *),
                                         void *arg, uint64_t timeout_us,
                                         cthrpool_future **out);

// Blocks until the future has a result and gives it, so the caller can collect
// what the task computed; NULL when an immediate shutdown cancelled the task.
// It doesn't free the future. Any number of threads may wait on one future;
// each holds its own reference during the call, so the future outlives all of
// them. From here on the caller owns the result.
void *cthrpool_future_get(cthrpool_future *f);

// Tells, without blocking, whether the future has a result or was cancelled,
// so a caller can poll instead of waiting in cthrpool_future_get().
bool cthrpool_future_done(cthrpool_future *f);

// Tells whether an immediate shutdown cancelled the future, which is how a
// caller tells a cancelled task from one that returned NULL.
bool cthrpool_future_cancelled(cthrpool_future *f);

// Gives up the caller's reference, which is how a future is released. Call it
// exactly once per successful future submit, whether or not you called
// cthrpool_future_get(); without a get the result is thrown away. Threads
// already inside cthrpool_future_get() keep the future alive. Don't use f after
// this call.
void cthrpool_future_free(cthrpool_future *f);

// Creates a future with no pool behind it, so a producer that isn't a cthrpool
// worker (an event-driven engine, for example) can hand a result to a waiting
// thread. The producer calls cthrpool_future_fulfill() exactly once and the
// consumer calls cthrpool_future_free() exactly once, in any order. Like every
// future it's allocated with plain malloc, so producer and consumer don't need
// to share an allocator. Gives NULL, with *err_str set, when it fails.
cthrpool_future *cthrpool_future_create_detached(char **err_str);

// Delivers result to a detached future, wakes every waiter and drops the
// producer's reference (freeing the future when it was the last one). It's the
// producer's half of cthrpool_future_create_detached(); never call it on a
// future from a submit. It gives ccol_success, ccol_invalid_args for a NULL f,
// or ccol_not_permitted when f is already fulfilled.
ccol_retval cthrpool_future_fulfill(cthrpool_future *f, void *result);

// Blocks until the queue is empty and no task runs, so a caller can wait for a
// batch of work without shutting the pool down; the pool stays usable. Nobody
// may submit while it waits. It doesn't wait for the ran == false callbacks of
// an immediate shutdown running on another thread. It's a no-op for a bad
// handle and for a call from one of this pool's own tasks or callbacks (which
// would wait for itself).
void cthrpool_wait(cthrpool pool);

// Runs every queued task, then stops and joins the workers: the graceful way to
// end a pool. After it the pool refuses new tasks. Calling it before destroy is
// optional. It's a no-op for a bad handle, and a no-op that doesn't block for a
// call from one of this pool's own tasks or callbacks (a worker can't join
// itself).
void cthrpool_shutdown_drain(cthrpool pool);

// Stops the pool without running its queue, for a caller that must stop fast.
// Every queued task is discarded (its future becomes cancelled and its
// on_complete runs with ran == false on this thread before it returns), each
// worker finishes its current task, and all of them are joined. During a drain
// it escalates the drain. No-op for a bad handle or a call from one of this
// pool's own tasks or callbacks.
void cthrpool_shutdown_immediate(cthrpool pool);

// Gives the number of queued tasks that no worker has taken yet, for
// monitoring and backpressure decisions; 0 for a bad handle.
size_t cthrpool_pending_count(cthrpool pool);

// Gives the number of tasks the workers are running right now, for
// monitoring; 0 for a bad handle.
size_t cthrpool_active_count(cthrpool pool);

#pragma GCC visibility pop
