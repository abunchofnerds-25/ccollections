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

// INTERNAL ONLY: the test hooks of cthreadpool. src/cthreadpool.c defines them
// so a test can force failures, hold the slot table and read pool state that
// the public API doesn't show. They exist only when RUNNING_UNIT_TESTS is
// defined, which the shipped library never does.
#include <cthreadpool.h>
#include <stdatomic.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <time.h>

#ifdef RUNNING_UNIT_TESTS
// Makes the next push onto the slot table's free list fail like an allocation,
// then disarms itself, so a test reaches the recovery of a lost index without a
// real out-of-memory condition.
void cthrpool__force_next_free_index_push_failure_for_tests(void);

// Sets how many idle points a trim of the task free list waits for, so a test
// reaches a trim with a few tasks. 0 restores the shipped value.
void cthrpool__set_task_free_list_window_for_tests(uint32_t idle_points);

// When set, the next future allocation of a submit fails like calloc would. The
// counter holds how many futures exist right now.
extern atomic_bool cthrpool__fail_next_future_alloc_for_tests;
extern atomic_long cthrpool__live_futures_for_tests;

// How many future blocks exist, from calloc to free, so a future that fails
// half built is counted too.
extern atomic_long cthrpool__future_blocks_for_tests;

// The creation steps that cthrpool__arm_fault_for_tests can make fail: the five
// primitive inits of cthrpool_create_mp, and the allocation and two primitive
// inits of a future.
typedef enum {
  CTHRPOOL__FAULT_NONE = 0,
  CTHRPOOL__FAULT_POOL_MUTEX,
  CTHRPOOL__FAULT_POOL_NOT_EMPTY,
  CTHRPOOL__FAULT_POOL_NOT_FULL,
  CTHRPOOL__FAULT_POOL_IDLE_CV,
  CTHRPOOL__FAULT_POOL_SHUTDOWN_CV,
  CTHRPOOL__FAULT_FUTURE_ALLOC,
  CTHRPOOL__FAULT_FUTURE_MUTEX,
  CTHRPOOL__FAULT_FUTURE_CV
} cthrpool__fault;

// Makes the next thread that reaches step see it fail, then disarms itself, so
// a test reaches each error path of a create without a real failure.
// CTHRPOOL__FAULT_NONE disarms. A failing init is skipped, so the error path
// has nothing of it to destroy, as after a real failure.
void cthrpool__arm_fault_for_tests(cthrpool__fault step);

// Makes the creation of worker index fail once in the next pool that gets that
// far, after workers 0 to index - 1 have started, so a test can check that the
// create joins the started workers. SIZE_MAX disarms it.
void cthrpool__fail_worker_create_at_for_tests(size_t index);

// While true, a starting worker skips its thread-local mark, as if
// pthread_setspecific had failed.
extern atomic_bool cthrpool__skip_worker_mark_for_tests;

// Running workers of every pool, from thread create to the worker's last
// statement. With a non-zero delay each worker pauses that many microseconds
// before its last statement, so a worker nobody joined is still counted when
// the call that should have joined it returns.
extern atomic_long cthrpool__running_workers_for_tests;
extern atomic_uint cthrpool__worker_exit_delay_us_for_tests;

// When the first flag is set, the next timed wait for room ignores its
// deadline, waits for a real wake and then reports ETIMEDOUT. The second flag
// tells the test that the waiter is inside that wait.
extern atomic_bool cthrpool__timeout_after_wake_for_tests;
extern atomic_bool cthrpool__timeout_after_wake_waiting_for_tests;

// Looks h up in the slot table with no pin and gives the pool, or NULL where
// the real resolve would fail. The accessors below take this pointer; with no
// pin there's no unpin a test could forget.
cthread_pool *cthrpool__resolve_for_tests(cthrpool h);

// Give the number of indices on the slot table's free list and the number of
// slots the table holds, so a test can see that slots are reused and none is
// lost, whatever handles earlier tests held.
size_t cthrpool__free_index_count_for_tests(void);
size_t cthrpool__slot_table_capacity_for_tests(void);

// Copies the shutdown flags of pool out, so a test can wait until one shutdown
// call owns the shutdown before it starts a second one.
void cthrpool__shutdown_state_for_tests(struct cthread_pool *pool,
                                        bool *started, bool *immediate,
                                        bool *complete);

// Gives the self-call check that a wait, shutdown or destroy of pool makes,
// answered for the calling thread, so a callback can check it before a call
// that would block for ever on a wrong answer.
bool cthrpool__is_self_call_for_tests(struct cthread_pool *pool);

// Give the current size and the cap of pool's task node free list, so a test
// can check the recycling and the trim directly.
size_t cthrpool__task_free_list_size_for_tests(cthread_pool *pool);
size_t cthrpool__task_free_list_cap_for_tests(cthread_pool *pool);

// Gives the outstanding pins on pool's slot, read with no lock held, so a test
// can see that a concurrent resolve happened without guessing a delay.
size_t cthrpool__pending_resolve_count_for_tests(cthread_pool *pool);

// Waits on pool's not_full condition until abs, on that condition's clock, and
// gives the status of the wait, so a test can tell which clock not_full uses.
int cthrpool__timedwait_not_full_for_tests(cthread_pool *pool,
                                           const struct timespec *abs);

// What the last timed wait for room on the calling thread did: whether it
// waited, its CLOCK_MONOTONIC deadline, when it started waiting and, when it
// gave up, whether that was a timeout and when. Only a read through
// cthrpool__timed_wait_record_for_tests() clears it.
typedef struct {
  bool waited;
  bool timed_out;
  struct timespec deadline;
  struct timespec wait_started;
  struct timespec wait_ended;
} cthrpool__timed_wait_record;

// Gives the timed wait record of the calling thread and clears it, so a test
// can check the deadline and the moment a timed submit gave up.
cthrpool__timed_wait_record cthrpool__timed_wait_record_for_tests(void);

// Gives how many times the calling thread has locked the mutex of any pool, so
// a test can count the critical sections of one call.
unsigned long cthrpool__mu_locks_for_tests(void);

// Gives the reference count of f, read under its mutex, so a test can wait
// until a concurrent cthrpool_future_get has taken its reference.
int cthrpool__future_refcount_for_tests(cthrpool_future *f);

// Take and release the write side of the slot table lock, so a test can hold
// it while another thread forks and check the child's handling of it.
void cthrpool_test_wrlock_slot_table_for_tests(void);
void cthrpool_test_wrunlock_slot_table_for_tests(void);

// Runs the process-exit cleanup of the slot table right now, so a test can
// reach the deferred release with a pool still live.
void cthrpool__run_exit_cleanup_for_tests(void);

// Tells whether the worker key exists, so a test can check that the release of
// the table deletes it.
bool cthrpool__worker_key_live_for_tests(void);
#endif
