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

/**
 * @file test_helpers/cthreadpool.h
 * @brief INTERNAL ONLY. The test-only hooks of cthreadpool.
 *
 * src/cthreadpool.c defines these hooks and the tests call them. They let a
 * test force failures, hold the slot table and read state of a pool that the
 * public API doesn't show, without any assumption about timing.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <cthreadpool.h>
#include <stdatomic.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* bool, true and false are keywords in C23 */
#endif
#include <time.h>

#ifdef RUNNING_UNIT_TESTS
/* Makes the next push onto the free list of the slot table behave exactly
 * like an allocation failure, then disarms itself, so a test reaches the
 * recovery of a lost index without a real out-of-memory condition. */
void _ctpool_force_next_free_index_push_failure_for_tests(void);

/* Sets the number of idle points that a trim of the task free list waits
 * for, so a test gets to a trim with a few tasks. 0 restores the value that
 * the library ships with. */
void _ctpool_set_task_free_list_window_for_tests(uint32_t idle_points);

/* When set, the next future allocation of a submit fails the way a failed
 * calloc would. The counter holds how many futures exist right now. */
extern atomic_bool _ctpool_fail_next_future_alloc_for_tests;
extern atomic_long _ctpool_live_futures_for_tests;

/* How many future blocks exist right now, from the calloc of each to its
 * free, so a future that fails half built is counted too. */
extern atomic_long _ctpool_future_blocks_for_tests;

/* The steps of pool and future creation that _ctpool_arm_fault_for_tests can
 * make fail: the five primitive initializations of ctpool_create_mp, and the
 * allocation and the two primitive initializations of a future, which every
 * future submit and ctpool_future_create_detached make. */
typedef enum {
  _CTPOOL_FAULT_NONE = 0,
  _CTPOOL_FAULT_POOL_MUTEX,
  _CTPOOL_FAULT_POOL_NOT_EMPTY,
  _CTPOOL_FAULT_POOL_NOT_FULL,
  _CTPOOL_FAULT_POOL_IDLE_CV,
  _CTPOOL_FAULT_POOL_SHUTDOWN_CV,
  _CTPOOL_FAULT_FUTURE_ALLOC,
  _CTPOOL_FAULT_FUTURE_MUTEX,
  _CTPOOL_FAULT_FUTURE_CV
} _ctpool_fault_t;

/* Arms one failure of step: the next thread that reaches it sees the call
 * fail, as calloc or the pthread init would, and the hook disarms itself.
 * _CTPOOL_FAULT_NONE disarms it. A failing init is skipped, so the failure
 * path has nothing of it to destroy, exactly as after a real failure. */
void _ctpool_arm_fault_for_tests(_ctpool_fault_t step);

/* Makes the thread creation of worker index fail once, in the next pool that
 * gets that far, after workers 0 to index - 1 have started. SIZE_MAX
 * disarms it. */
void _ctpool_fail_worker_create_at_for_tests(size_t index);

/* While true, a starting worker skips setting its thread-local mark, as if
 * pthread_setspecific had failed. A test sets it only around the creation of
 * the pool it looks at. */
extern atomic_bool _ctpool_skip_worker_mark_for_tests;

/* How many worker threads of any pool are running, counted from the thread
 * create that started a worker to the worker's last statement. When
 * _ctpool_worker_exit_delay_us_for_tests is not 0, every worker pauses that
 * many microseconds before its last statement, so a worker that nobody
 * joined is still counted when the call that should have joined it
 * returns. */
extern atomic_long _ctpool_running_workers_for_tests;
extern atomic_uint _ctpool_worker_exit_delay_us_for_tests;

/* When the first flag is set, the next timed wait for room ignores its
 * deadline, waits for a real wake and then reports ETIMEDOUT. The second
 * flag tells the test that the waiter is inside the wait. */
extern atomic_bool _ctpool_timeout_after_wake_for_tests;
extern atomic_bool _ctpool_timeout_after_wake_waiting_for_tests;

/* Resolves h to the pool behind it with no pin, as a bare lookup in the slot
 * table, so there's no unpin to forget. It returns NULL under the same
 * conditions as the real resolve. The accessors below that take a
 * cthread_pool * take the pointer that this returns. */
cthread_pool *_ctpool_resolve_for_tests(ctpool h);

/* The indices on the free list of the slot table right now, and the number
 * of slots the table holds, counting freed slots not reused yet. Read
 * together, they describe the table whatever handles earlier tests held. */
size_t _ctpool_free_index_count_for_tests(void);
size_t _ctpool_slot_table_capacity_for_tests(void);

/* Reads the shutdown bookkeeping of pool, so a test can wait until one
 * shutdown call has taken ownership before it starts a second one. */
void _ctpool_shutdown_state_for_tests(struct cthread_pool *pool, bool *started,
                                      bool *immediate, bool *complete);

/* Answers the self-call check for pool on the calling thread, the same check
 * that a wait, a shutdown or a destroy of pool consults. */
bool _ctpool_is_self_call_for_tests(struct cthread_pool *pool);

/* The current size and the cap of the task-node free list of pool. */
size_t _ctpool_task_free_list_size_for_tests(cthread_pool *pool);
size_t _ctpool_task_free_list_cap_for_tests(cthread_pool *pool);

/* How many pins are outstanding against the slot of pool, read with no lock
 * held, so a test can see that a concurrent resolve and pin happened. */
size_t _ctpool_pending_resolve_count_for_tests(cthread_pool *pool);

/* Waits on the not_full condition of pool until the absolute deadline abs,
 * on whatever clock not_full uses, and returns the status of the wait. */
int _ctpool_timedwait_not_full_for_tests(cthread_pool *pool,
                                         const struct timespec *abs);

/* What the last timed wait for room on the calling thread did: whether it
 * waited at all, the absolute CLOCK_MONOTONIC deadline it waited for, the
 * instant its first wait began, and, when it gave up, whether that was a
 * timeout and the instant it decided so. Only reading the record through
 * _ctpool_timed_wait_record_for_tests() clears it. After a clear, a call
 * that found room without waiting leaves every field zero; one that waited
 * sets waited, deadline and wait_started even when it then found room. */
typedef struct {
  bool waited;
  bool timed_out;
  struct timespec deadline;
  struct timespec wait_started;
  struct timespec wait_ended;
} _ctpool_timed_wait_record_t;

/* Gives the record of the calling thread and clears it, so the next timed
 * submit starts a fresh one. */
_ctpool_timed_wait_record_t _ctpool_timed_wait_record_for_tests(void);

/* How many times the calling thread has locked the mutex of any pool. A test
 * reads it before and after one call to count its critical sections. */
unsigned long _ctpool_mu_locks_for_tests(void);

/* The current reference count of a future, read under its mutex, so a test
 * can wait until a concurrent ctpool_future_get has taken its reference. */
int _ctpool_future_refcount_for_tests(ctpool_future *f);

/* Locks and unlocks the write side of the slot table lock directly, so a
 * test can hold it for a window it controls while another thread forks. */
void ctpool_test_wrlock_slot_table_for_tests(void);
void ctpool_test_wrunlock_slot_table_for_tests(void);

/* Runs the process-exit cleanup of the slot table right now, which a test
 * calls in a forked child with a pool still live. */
void _ctpool_run_exit_cleanup_for_tests(void);

/* True while the worker key exists. */
bool _ctpool_worker_key_live_for_tests(void);
#endif /* RUNNING_UNIT_TESTS */
