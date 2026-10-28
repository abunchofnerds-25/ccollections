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
 * @file test_helpers/cthreadcomm.h
 * @brief INTERNAL ONLY. The test-only hooks of cthreadcomm.
 *
 * src/cthreadcomm.c defines these hooks and the cthreadcomm tests call them.
 * They let a test force failures, hold a thread at a chosen point and read
 * state that the public API doesn't show.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <cthreadcomm.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Make the next allocation of an internal ccol_event_reg handle slot
 *        report a failure, for tests.
 *
 * Arms a process-wide one-shot flag that disarms itself. The next time
 * ccol_event_loop_add makes a public ccol_event_reg handle for a new
 * registration, that step fails exactly as a failed growth allocation of the
 * slot table would, whatever allocator the target loop uses, and
 * ccol_event_loop_add returns CCOL_EVENT_REG_NULL with nothing put into
 * the registry.
 *
 * A test can hit that failure every time this way, which fault injection
 * can't easily do: the loop allocates the slot table at a minimum capacity
 * above zero when it is created, and the table shares its allocator with
 * every other allocation of the loop.
 *
 * @note Once the next ccol_event_loop_add call has used the flag it does
 *       nothing more; call this function again to arm another one.
 */
void _ccol_event_loop_test_force_next_reg_slot_acquire_failure(void);

/**
 * @brief Read the registration count of the whole loop at the exact moment of
 *        the most recent forced slot-acquire failure, for tests.
 *
 * With this a test proves the order inside ccol_event_loop_add directly and
 * every time: reg's handle slot is taken BEFORE reg goes into the registry of
 * fds and queues, not after. Without the value a test can only see the end
 * state, and both orders give the same one, ccol_event_loop_add returning
 * CCOL_EVENT_REG_NULL and ccol_event_loop_reg_count() back at 0, because
 * rolling back a failure after the wiring restores the count too. A snapshot
 * of 0 proves that the loop hadn't counted this call's registration yet, so
 * it hadn't wired it yet when the forced failure happened.
 *
 * @return The snapshot of the most recent forced slot-acquire failure (see
 * _ccol_event_loop_test_force_next_reg_slot_acquire_failure), or 0 if no such
 * failure has happened yet in this process.
 */
size_t _ccol_event_loop_test_last_forced_slot_acquire_failure_reg_count(void);

/**
 * @brief Install a function that ccol_event_loop_add calls after the new
 *        registration is live and before the call returns, for tests.
 *
 * The hook runs on the thread calling ccol_event_loop_add, once the
 * registration is wired into the loop and can be dispatched but before the
 * handle goes back to the caller, so a hook that waits for one of the
 * registration's callbacks makes that callback run before
 * ccol_event_loop_add returns, every time. Pass NULL to remove the hook.
 *
 * @param hook Function to call, or NULL
 */
void _ccol_event_loop_test_set_add_before_return_hook(void (*hook)(void));

/**
 * @brief Show how many dispatch jobs are queued in, or running on, loop's
 *        dispatch_pool, for tests.
 *
 * Gives 0 for a NULL loop and for a loop built with num_reactor_threads ==
 * 1, since that configuration has no dispatch_pool. It is the direct
 * regression check for the EPOLLONESHOT re-arm mechanism: an fd or queue
 * selectable that is ready all the time must not make this count grow
 * without bound while the dispatch_pool workers catch up.
 *
 * @param loop  ccol_event_loop to ask
 * @return      Number of dispatch jobs that wait or that run now
 */
size_t _ccol_event_loop_dispatch_pool_pending_count_for_tests(
    ccol_event_loop loop);

/**
 * @brief Show how many epoll_wait calls loop's poller thread has finished so
 *        far, for tests.
 *
 * A test can catch a busy spin in the reactor with this directly: during a
 * spin the counter jumps far ahead within a short, bounded window, so the
 * test needs no wall-clock or CPU measurement, both of which are flaky. Read
 * the value twice across a bounded interval and compare.
 *
 * @param loop  ccol_event_loop to ask
 * @return      Total count of finished epoll_wait calls, or 0 for a loop
 * handle that is not valid or that is stale.
 */
uint64_t _ccol_event_loop_poller_iterations_for_tests(ccol_event_loop loop);

/**
 * @brief Make the next _ccol_cond_var_timedwait call report an unexpected
 *        error (an error other than ETIMEDOUT) in the wait path of
 *        ccol_select_timed that has no fd selectables, for
 *        tests.
 *
 * Arms a one-shot flag that disarms itself. The condition-variable-only wait
 * of ccol_select_timed calls _ccol_cond_var_timedwait (a path reachable only
 * with a deadline and no fd selectable in the call), and the next such call
 * reports EINVAL without waiting.
 *
 * That lets a test reach the path where _ccol_cond_var_timedwait itself
 * fails and ccol_unexpected_failure comes back, which no correct call from
 * the library's own deadline computation can reach.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 */
void _ccol_select_test_force_next_condvar_wait_error(void);

/**
 * @brief The same as _ccol_select_test_force_next_condvar_wait_error, but it
 *        also marks the internal wait as satisfied at the same instant, for
 *        tests.
 *
 * Arms the same one-shot forced EINVAL as
 * _ccol_select_test_force_next_condvar_wait_error and also sets the internal
 * "ready" flag to true, inside the same critical section that reports the
 * forced error. That imitates a producer whose notify completes correctly an
 * instant before the unrelated forced error shows up.
 *
 * This order of events can really happen in production, because a real
 * _ccol_cond_var_timedwait call always takes its mutex again before it
 * returns, on success and on failure, but a test can't produce it reliably
 * any other way: this hook replaces the real wait call, so it never unlocks
 * the mutex for a producer thread to race into.
 *
 * The property a test checks with it, every time, is that a wakeup racing an
 * unrelated spurious failure of _ccol_cond_var_timedwait counts as a
 * successful wakeup, so ccol_select_timed scans again and finally succeeds
 * instead of dropping the wakeup and reporting
 * ccol_unexpected_failure.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 */
void _ccol_select_test_force_next_condvar_wait_error_racing_ready(void);

/**
 * @brief For tests only: delay the next timed-out deregistration of
 *        ccol_select_timed
 *
 * Arms a one-shot delay that disarms itself. The delay sits between the
 * point where ccol_select_timed's condition-variable-only wait reports that
 * its deadline ended and the point where the call unlinks its waiter nodes
 * from the queues it watched. A producer's notify that lands in that window
 * goes to a waiter that has already stopped waiting, so the leaving waiter
 * must hand the wake on. In production the window is a few instructions,
 * far too narrow for a test to aim at.
 *
 * @param us Delay in microseconds. A value of 0 disarms a delay that hasn't
 * run yet.
 *
 * @note The next such deregistration uses up the delay; call this function
 * again to arm another one.
 */
void _ccol_select_test_delay_next_timed_out_deregister_us(uint32_t us);

/**
 * @brief For tests only: make the next initialisation of the wait mutex of a
 *        queue registration fail
 *
 * Arms a one-shot flag that disarms itself, so the next ccol_event_loop_add
 * of a queue or ccol_channel selectable fails as if pthread_mutex_init() had
 * failed. A test can then check the message for a failed system call, which
 * no allocator can force.
 */
void _ccol_event_loop_test_fail_next_reg_sync_init(void);

/**
 * @brief For tests only: make the next count capped waits of
 *        ccol_select_timed end at once
 *
 * A single wait of ccol_select_timed with an fd selectable asks the kernel
 * for at most INT_MAX milliseconds (about 24.8 days), so a longer timeout
 * takes several waits. Each armed count makes one such capped wait return at
 * once with no event, as the real wait does when the capped time runs out,
 * and a test can check that the call waits again instead of reporting a
 * timeout before its deadline.
 *
 * @param count The number of capped waits to end at once. A value of 0
 * disarms the counts that haven't run yet.
 */
void _ccol_select_test_short_next_capped_waits(uint32_t count);

/* How many of the counts armed by _ccol_select_test_short_next_capped_waits()
 * haven't ended a capped wait yet. */
uint32_t _ccol_select_test_short_capped_waits_left(void);

/**
 * @brief For tests only: how many times a thread entered the delay that
 *        _ccol_select_test_delay_next_timed_out_deregister_us armed
 *
 * A test polls this value in a bounded loop to know that the delayed waiter
 * really reached that window, and only then produces the message whose
 * notify has to land inside it, so the test doesn't have to guess with a
 * sleep.
 */
uint64_t _ccol_select_test_timed_out_deregister_delay_count(void);

/**
 * @brief Make the next _ccol_cond_var_timedwait call in the wait loop of
 * ccol_circq_timed_send_zc report an unexpected error (an error other
 * than ETIMEDOUT), for tests.
 *
 * Works like _ccol_select_test_force_next_condvar_wait_error, but for
 * ccol_circq_timed_send_zc: that function's next wait reports EINVAL without
 * waiting, and the flag then disarms itself.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 * @see _ccol_circq_test_force_next_send_condvar_wait_error_racing_ready
 */
void _ccol_circq_test_force_next_send_condvar_wait_error(void);

/**
 * @brief For tests only: widen the window inside _notify_waiter(). The
 *        window starts where a thread already holds a queue's mutex and ends
 *        where this function tries to lock wait_mtx, and the test picks the
 *        new width in microseconds. A value of 0 turns the widening off,
 *        which is the default.
 *
 * A test can then land a concurrent fork() call inside that exact window
 * every time and reproduce the lock-order hazard between the queue's mutex
 * and wait_mtx, which the merged atfork handler of ccol_event_loop and the
 * queue registry exists to prevent. In production the window is normally
 * only a few instructions, too narrow to hit by luck.
 *
 * @warning While armed, the widening applies to every _notify_waiter() call
 * in the whole process, not to one queue, so a test must disarm it with 0
 * when it finishes. Otherwise every later ccol_circq_send_zc,
 * ccol_circq_recv_zc, ccol_dynmq_send_zc and every other such call anywhere
 * in the process pays the delay.
 */
void _notify_waiter_test_set_delay_us(int us);

/**
 * @brief Make the next internal unpin of a ccol_event_reg sleep for ms
 *        milliseconds while it still holds its resolve pin, for tests.
 *
 * Arms a process-wide one-shot flag that disarms itself. Several public calls
 * resolve a ccol_event_reg and then unpin it (ccol_event_loop_modify,
 * ccol_event_loop_pause, ccol_event_loop_resume, ccol_event_loop_remove, and
 * ccol_event_loop_reg_generation), and the next such call sleeps for ms
 * milliseconds when it gets to the point of releasing its resolve pin.
 *
 * A test can hold a resolve pin open this way, every time, long enough for
 * another thread to run ccol_event_loop_remove on the same registration,
 * which marks it removed and defers it first. That reaches one exact case,
 * where the delivery of an on_removed notification depends on the reactor's
 * bounded reclaim retry instead of on one call's wakeup ping.
 *
 * @param ms  Milliseconds to sleep. A value of 0 turns the delay off, which is
 * the default.
 * @note Once the next unpin has used the flag it does nothing more; call this
 *       function again to arm another one.
 */
void _ccol_event_loop_test_delay_next_reg_resolve_unpin_ms(uint32_t ms);

/**
 * @brief For tests only: tell whether an unpin has taken the delay that
 *        _ccol_event_loop_test_delay_next_reg_resolve_unpin_ms armed.
 *
 * The unpin takes the delay after it has read whether the registration is
 * removed and before it releases its resolve pin. So once a test has armed
 * the delay and started the thread that must take it, a true result means
 * that thread holds its pin and has already made that read. A test can poll
 * this function, instead of sleeping for a fixed time, before it races a
 * ccol_event_loop_remove against the pin.
 *
 * @return true when no delay is armed, false while an armed delay waits for
 *         an unpin.
 */
bool _ccol_event_loop_test_reg_resolve_unpin_delay_taken_for_tests(void);

/**
 * @brief For tests only: lock the internal mutex of cq directly, and go around
 *        every function of the public API.
 *
 * A test can keep cq's mutex locked this way for exactly as long as it
 * wants, long enough to land a fork() call inside the window every time,
 * while a real critical section in production is normally only a few
 * instructions and can't be hit by luck. The fork-safety regression test of
 * the queue mutex uses it.
 *
 * @warning This call must always be followed by a
 * _ccol_circq_test_unlock_mutex_for_tests call on the same cq from the same
 * thread, since nothing else in this file expects a thread to keep holding
 * cq's mutex after this call returns.
 *
 * @see _ccol_circq_test_unlock_mutex_for_tests
 */
void _ccol_circq_test_lock_mutex_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: unlock the internal mutex of cq. See
 *        _ccol_circq_test_lock_mutex_for_tests.
 */
void _ccol_circq_test_unlock_mutex_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: lock the write side of the reg_slot_rwlock of loop
 *        directly, and go around every function of the public API.
 *
 * A test can keep the write side of this rwlock locked this way for exactly
 * as long as it wants, with the lock held by a thread OTHER than the one
 * that calls fork(). It exists for the same reason as
 * _ccol_circq_test_lock_mutex_for_tests, but covers a ccol_event_loop's
 * reg-slot table instead of a ccol_circular_queue.
 *
 * @return An opaque pointer to the resolved loop, which you MUST pass to the
 *         matching _ccol_event_loop_test_wrunlock_reg_slot_for_tests
 *         call. It is NULL if loop wasn't valid, in which case nothing was
 *         locked and the matching unlock call with NULL is a safe no-op. It's
 *         deliberately not loop itself, because the unlock side has to use
 *         this exact resolved pointer instead of resolving loop again: a
 *         second resolve from another thread can deadlock against a
 *         concurrent fork() that already holds the
 *         ccol_event_loop_slot_table mutex while it waits for this very write
 *         lock to be released. That deadlock is real and reachable, not a
 *         theoretical concern.
 *
 * @warning This call must always be followed by a
 * _ccol_event_loop_test_wrunlock_reg_slot_for_tests call with its return
 * value, since nothing else in this file expects reg_slot_rwlock to keep a
 * write lock after that call returns.
 *
 * @see _ccol_event_loop_test_wrunlock_reg_slot_for_tests
 */
void *_ccol_event_loop_test_wrlock_reg_slot_for_tests(ccol_event_loop loop);

/**
 * @brief For tests only: unlock the write side of reg_slot_rwlock that an
 *        earlier _ccol_event_loop_test_wrlock_reg_slot_for_tests call locked.
 *
 * @param resolved_loop The exact return value of the matching
 *        _ccol_event_loop_test_wrlock_reg_slot_for_tests call, which is NULL
 *        for an invalid loop; NULL here is safe and does nothing.
 *
 * @see _ccol_event_loop_test_wrlock_reg_slot_for_tests
 */
void _ccol_event_loop_test_wrunlock_reg_slot_for_tests(void *resolved_loop);

/**
 * @brief For tests only: lock the write side of the rwlock of
 *        ccol_event_loop_slot_table directly, and go around every function of
 *        the public API.
 *
 * A test can keep the write side of this rwlock locked this way for exactly
 * as long as it wants, with the lock held by a thread OTHER than the one
 * that calls fork(). This one covers the process-wide loop table, not the
 * per-loop reg_slot_rwlock that
 * _ccol_event_loop_test_wrlock_reg_slot_for_tests covers. There is no
 * instance handle to resolve here, so it takes no argument, and the matching
 * unlock call needs none of the AB-BA precautions of that other pair.
 *
 * @warning This call must always be followed by a
 * _ccol_event_loop_test_wrunlock_slot_table_for_tests call, since nothing
 * else in this file expects ccol_event_loop_slot_table.rwlock to keep a write
 * lock after that call returns.
 *
 * @see _ccol_event_loop_test_wrunlock_slot_table_for_tests
 */
void _ccol_event_loop_test_wrlock_slot_table_for_tests(void);

/**
 * @brief For tests only: unlock the write side of the rwlock of
 *        ccol_event_loop_slot_table. See
 *        _ccol_event_loop_test_wrlock_slot_table_for_tests.
 */
void _ccol_event_loop_test_wrunlock_slot_table_for_tests(void);

/**
 * @brief For tests only: report whether a ccol_select() caller is really
 *        linked into cq's read-waiter list right now.
 *
 * The function locks, checks and unlocks on its own. A test that needs a
 * select-waiter thread to get through its own Phase 1 _ccol_mutex_lock and
 * link step polls this value in a bounded loop, instead of guessing that a
 * fixed sleep after pthread_create() gives the OS time to schedule the new
 * thread that far, which the return of pthread_create() doesn't guarantee.
 */
bool _ccol_circq_test_has_sel_read_waiter_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: how many ccol_select() waiter nodes are linked into
 *        cq's read-waiter list right now.
 *
 * Where _ccol_circq_test_has_sel_read_waiter_for_tests only answers yes or
 * no, this one counts, for tests that need an exact number of ccol_select()
 * callers to reach their link step and not just one or more. The order in
 * which two waiters link decides which of them gets a producer's next
 * notify, so a test that depends on that order polls this value until each
 * waiter is in place before it starts the next one.
 */
size_t _ccol_circq_test_sel_read_waiter_count_for_tests(
    ccol_circular_queue *cq);

/**
 * @brief For tests only: how many receivers are parked right now in cq's
 *        read condition variable.
 *
 * A send signals that condition variable only when this count isn't zero. A
 * test that must prove a send really wakes a blocked receiver (and that the
 * receiver didn't just leave on its own timeout) polls this value in a
 * bounded loop until the receiver has registered, and only then sends. A
 * wait site that doesn't register never lets that poll succeed, and that's
 * what keeps such a test from passing while it examines nothing.
 */
size_t _ccol_circq_waiting_readers_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: how many senders are parked right now in cq's write
 *        condition variable; the send-side twin of
 *        _ccol_circq_waiting_readers_for_tests.
 */
size_t _ccol_circq_waiting_writers_for_tests(ccol_circular_queue *cq);

/**
 * @brief For tests only: how many receivers are parked right now in dq's
 *        read condition variable; _ccol_circq_waiting_readers_for_tests for a
 *        dynamic queue.
 */
size_t _ccol_dynmq_waiting_readers_for_tests(ccol_dynamic_queue *dq);

/**
 * @brief The same as _ccol_circq_test_force_next_send_condvar_wait_error, but
 * it also frees the slot of one queued message at the same instant, for
 * tests.
 *
 * Arms the same one-shot forced EINVAL and also does exactly the bookkeeping
 * of a real receive by a concurrent consumer (it frees one slot and signals
 * write_cond) under the same mutex that reports the forced error. That
 * imitates a consumer whose wakeup completes correctly an instant before the
 * unrelated forced error shows up, so a test can check that the send uses a
 * slot freed this way instead of dropping it and reporting
 * ccol_unexpected_failure.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 */
void _ccol_circq_test_force_next_send_condvar_wait_error_racing_ready(void);

/**
 * @brief Get the message that the most recent race of
 *        _ccol_circq_test_force_next_send_condvar_wait_error_racing_ready
 *        freed, for tests.
 *
 * The imitated concurrent consumer's receive (see
 * _ccol_circq_test_force_next_send_condvar_wait_error_racing_ready) takes a
 * message out of the queue and has nowhere to put it. This function hands
 * that message out exactly once and {NULL, 0} after that, so a test can free
 * its data pointer, matching the ownership a real caller of
 * ccol_circq_recv_zc or ccol_circq_try_recv_zc would have.
 *
 * @return The freed message, or {NULL, 0} if no message waits
 */
c_message_t _ccol_circq_test_take_race_freed_msg(void);

/**
 * @brief Make the next _ccol_cond_var_timedwait call in the wait loop of
 * ccol_circq_timed_recv_zc report an unexpected error (an error other
 * than ETIMEDOUT), for tests.
 *
 * Works like _ccol_select_test_force_next_condvar_wait_error, but for
 * ccol_circq_timed_recv_zc.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 * @see _ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready
 */
void _ccol_circq_test_force_next_recv_condvar_wait_error(void);

/**
 * @brief The same as _ccol_circq_test_force_next_recv_condvar_wait_error, but
 *        it also puts a sentinel message into the queue at the same instant,
 *        for tests.
 *
 * Arms the same one-shot forced EINVAL and also puts a {NULL, 0} sentinel
 * message into the queue, through the same internal path a real concurrent
 * producer's send takes and under the same mutex that reports the forced
 * error. That imitates a producer whose wakeup completes correctly an
 * instant before the unrelated forced error shows up, so a test can check
 * that the library receives a message that arrived this way instead of
 * dropping it and reporting ccol_unexpected_failure.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 */
void _ccol_circq_test_force_next_recv_condvar_wait_error_racing_ready(void);

/**
 * @brief Make the next _ccol_cond_var_timedwait call in the wait loop of
 * ccol_dynmq_timed_recv_zc report an unexpected error (an error other
 * than ETIMEDOUT), for tests.
 *
 * Works like _ccol_select_test_force_next_condvar_wait_error, but for
 * ccol_dynmq_timed_recv_zc.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 * @see _ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready
 */
void _ccol_dynmq_test_force_next_recv_condvar_wait_error(void);

/**
 * @brief The same as _ccol_dynmq_test_force_next_recv_condvar_wait_error, but
 *        it also puts a sentinel message into the queue at the same instant,
 *        for tests.
 *
 * Arms the same one-shot forced EINVAL and also puts a {NULL, 0} sentinel
 * message into the queue, through the same internal path a real concurrent
 * producer's send takes and under the same mutex that reports the forced
 * error. That imitates a producer whose wakeup completes correctly an
 * instant before the unrelated forced error shows up, so a test can check
 * that the library receives a message that arrived this way instead of
 * dropping it and reporting ccol_unexpected_failure.
 *
 * @note Once the next such call has used the flag it does nothing more; call
 *       this function again to arm another one.
 */
void _ccol_dynmq_test_force_next_recv_condvar_wait_error_racing_ready(void);

/**
 * @brief Delay the next cascade notification of a queue by us microseconds,
 *        for tests.
 *
 * Arms a process-wide one-shot flag that disarms itself. The next cascade
 * step after a dispatch, for a queue registration or a ccol_channel
 * registration, sleeps for us microseconds after it starts looking at the
 * queue and before it touches the queue's mutex. That widens the window
 * enough for a test to land in it every time, a window in which the library
 * has to hold off a concurrent ccol_event_loop_remove call and the queue
 * destroy that follows right after it.
 *
 * @param us  Microseconds to sleep. A value of 0 disarms the delay.
 */
void _ccol_event_loop_test_delay_next_queue_cascade_us(uint32_t us);

/**
 * @brief Read how many cascade steps of a queue started, and how many stopped
 *        to touch the queue, for tests.
 *
 * Both counters are process-wide and only go up. A cascade step adds one to
 * the first when it starts looking at the queue and one to the second when
 * it is done with the queue, so a test can prove that a concurrent
 * ccol_event_loop_remove can't return while a cascade step sits between the
 * two, which is what makes destroying the queue right after that call safe.
 *
 * @param begun     Optional; it gets the count of the steps that started.
 * @param finished  Optional; it gets the count of the steps that finished.
 */
void _ccol_event_loop_test_queue_cascade_counts(uint64_t *begun,
                                                uint64_t *finished);

/**
 * @brief Read how many wakes the queue notify path delivered through an
 *        eventfd, for tests.
 *
 * The counter is process-wide and only goes up. It counts each write(2) that
 * a send, a receive, a state change or a cascade step makes to the eventfd of
 * a ccol_select() waiter in epoll mode or of a ccol_event_loop queue
 * registration, but not a ccol_event_loop's own internal eventfds. A test
 * samples it before and after a burst of sends and asserts that a listener
 * with a wake already pending costs no extra write.
 *
 * @return Number of eventfd wakes so far
 */
uint64_t _ccol_select_test_eventfd_wake_count(void);

/**
 * @brief Make one later allocation of ccol_select_timed() fail, for tests.
 *
 * Allocation number index, counting from 1 at this call, fails the way an
 * out-of-memory allocator fails, and the hook then disarms itself; 0 disarms
 * it at once. The count runs across every ccol_select() and
 * ccol_select_timed() call in the process.
 *
 * @param index The number of the allocation that fails, or 0
 */
void _ccol_select_test_fail_alloc(uint32_t index);

/**
 * @brief Read how many epoll sets ccol_select_timed() created, for tests.
 *
 * The counter is process-wide and only goes up. A call with fd selectables
 * creates a set only when it has to wait, so a call that a ready selectable
 * answers at once leaves it unchanged.
 *
 * @return Number of epoll sets created so far
 */
uint64_t _ccol_select_test_poll_sets_created(void);

/**
 * @brief Give the lock stripe that an fd selectable maps to, for tests.
 *
 * @param num_stripes The number of stripes of a loop, at least 1
 * @param fd          The fd
 * @return The stripe index, below num_stripes
 */
size_t _ccol_event_loop_test_stripe_index(size_t num_stripes, int fd);

/**
 * @brief Make the next count thread marks of the event loop fail, for tests.
 *
 * Each of the next count stores of a thread mark (the poller's, a dispatch
 * job's, or a queue dispatch's) fails the way pthread_setspecific() fails
 * without memory, so the thread keeps the mark in its fallback copy. The
 * count is process-wide.
 *
 * @param count The number of stores that fail
 */
void _ccol_event_loop_test_fail_next_marks(uint32_t count);

/**
 * @brief Say whether the calling thread carries the mark of the poller or of
 *        a dispatch job of loop, for tests.
 *
 * It's the same answer the self-call guards of ccol_event_loop_shutdown() and
 * ccol_event_loop_destroy() rely on.
 *
 * @param loop The loop
 * @return true inside a callback of loop, false elsewhere and for an invalid
 *         handle
 */
bool _ccol_event_loop_test_runs_on_loop_thread(ccol_event_loop loop);

/**
 * @brief Say whether the calling thread runs a callback that dispatches the
 *        queue, for tests.
 *
 * A queue's destroy uses this answer to refuse being called from inside such
 * a callback.
 *
 * @param queue The address of a ccol_circular_queue or a ccol_dynamic_queue
 * @return true inside an event-loop callback of that queue
 */
bool _ccol_event_loop_test_dispatches_queue(const void *queue);

#if CCOL_FORK_SAFETY_REQUIRED
/* Puts the number of registered queue mutexes in *count and the number of
 * entries that removes have examined so far in *remove_probes. Returns true
 * when the registry index of every queue names the position of its entry. */
bool _ccol_queue_mutex_registry_check_for_tests(size_t *count,
                                                uint64_t *remove_probes);
#endif

/* Makes the next push onto the free list of the loop slot table behave
 * exactly like an allocation failure, then disarms itself, so a test reaches
 * the recovery of a lost index without a real out-of-memory condition. */
void _ccol_event_loop_force_next_free_index_push_failure_for_tests(void);

/* Makes the next free-index push of a released registration slot report a
 * failure, then disarms itself. The second call gives how many registration
 * slots loop has ever allocated, which a recovered lost index must not make
 * grow. */
void _ccol_event_loop_force_next_reg_free_index_push_failure_for_tests(void);
size_t _ccol_event_loop_reg_slot_count_for_tests(ccol_event_loop loop);

/* Returns the thread ID of the poller thread of loop, joined or not, so a
 * test can start threads until one carries that ID and prove that the
 * self-call guards don't recognise the poller by its ID. */
_ccol_thread_id_t _ccol_event_loop_poller_thread_for_tests(
    ccol_event_loop loop);

/* Reads how many event entries the epoch reclaim of loop has freed so far,
 * or 0 for an invalid or stale loop handle. */
uint64_t _ccol_event_loop_entries_reclaimed_for_tests(ccol_event_loop loop);

/* Stores value into the poller batch generation of loop, so a test can put
 * the counter just below a 32-bit boundary and check that a deferred free
 * past that boundary still gets reclaimed. */
void _ccol_event_loop_set_poller_batch_gen_for_tests(ccol_event_loop loop,
                                                     uint64_t value);

/* Reads how many slots the loop handle table holds right now, freed slots
 * not reused yet included, so a test can check that a churn of creates and
 * destroys reuses freed slots. */
size_t _ccol_event_loop_slot_table_capacity_for_tests(void);

/* Resolves h through the real resolve, so the pin is real, sets *pinned,
 * waits until the test sets *release and then unpins. A destroy that runs
 * meanwhile can't return while *release is clear. Returns false when h
 * doesn't resolve. */
bool _ccol_event_loop_resolve_pin_until_for_tests(ccol_event_loop h,
                                                  atomic_int *pinned,
                                                  atomic_int *release);

struct _ccol_event_loop_s;

/* Resolves h to the loop behind it without a pin, so there is no unpin to
 * forget; NULL under the same conditions as the real resolve. */
struct _ccol_event_loop_s *_ccol_event_loop_resolve_for_tests(
    ccol_event_loop h);
#endif /* RUNNING_UNIT_TESTS */
