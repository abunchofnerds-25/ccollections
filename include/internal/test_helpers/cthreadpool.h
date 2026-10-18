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

/* While true, a starting worker skips setting its thread-local mark, as if
 * pthread_setspecific had failed. A test sets it only around the creation of
 * the pool it looks at. */
extern atomic_bool _ctpool_skip_worker_mark_for_tests;

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
