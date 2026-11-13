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

// Internal only. The test hooks of cthreadcomm, defined in src/cthreadcomm.c.
// They force failures, hold a thread at a chosen point, or read state the
// public API doesn't show. They exist only under RUNNING_UNIT_TESTS, which no
// shipped build defines. Every "next" hook is one-shot: The call that uses it
// disarms it, and you call it again to arm another one.

#include <cthreadcomm.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
// Makes the next ceventloop_add fail at the step that takes the handle slot of
// the new registration, as a failed slot table growth would, so it gives
// CEVENTLOOP_REG_NULL with nothing put into the registry. Fault injection can't
// reach that reliably, since the table starts above zero capacity and shares
// the loop's allocator.
void ceventloop__test_force_next_reg_slot_acquire_failure(void);

// The loop's registration count at the moment of the last forced slot failure,
// or 0 when none happened. A 0 proves the slot is taken before the registration
// is wired, which the end state alone can't show, since a rollback after the
// wiring restores the count too.
size_t ceventloop__test_last_forced_slot_acquire_failure_reg_count(void);

// Installs a hook that ceventloop_add calls on its own thread once the
// registration is live and before the handle goes back, so a hook that waits
// for a callback makes that callback run before the add returns. NULL removes
// it.
void ceventloop__test_set_add_before_return_hook(void (*hook)(void));

// How many dispatch jobs wait in or run on the loop's dispatch pool (0 for a
// NULL loop or one with a single reactor thread). A selectable that is always
// ready must not make it grow without bound; that checks the one-shot re-arm.
size_t ceventloop__dispatch_pool_pending_count_for_tests(ceventloop loop);

// How many waits the poller thread has finished, 0 for a bad handle. Read it
// twice over a bounded interval to catch a busy spin without timing anything.
uint64_t ceventloop__poller_iterations_for_tests(ceventloop loop);

// Makes the next timed condition wait of cselect_timed (a deadline and no fd
// selectable) fail with EINVAL without waiting, so the
// ccol_unexpected_failure path can be reached.
void cselect__test_force_next_condvar_wait_error(void);

// Like cselect__test_force_next_condvar_wait_error(), but it also sets the
// waiter's ready flag in the same critical section, as a producer whose notify
// lands just before an unrelated wait error. cselect_timed must count that as a
// wakeup and scan again. The hook replaces the real wait, so it never unlocks
// the mutex for a real producer to race into.
void cselect__test_force_next_condvar_wait_error_racing_ready(void);

// Sleeps us microseconds between the moment a condition-only cselect_timed
// wait reports its deadline and the unlinking of its waiters, so a notify can
// land on a waiter that already left and must hand the wake on. 0 disarms a
// delay that hasn't run.
void cselect__test_delay_next_timed_out_deregister_us(uint32_t us);

// Makes the next ceventloop_add of a queue or cchan selectable fail as if the
// wait mutex init had failed, so a test can check the message for a failed
// system call.
void ceventloop__test_fail_next_reg_sync_init(void);

// Ends each of the next count capped waits (at most INT_MAX ms) of
// cselect_timed at once with no event, as a real capped wait that runs out, so
// a test can check that the call waits again before its deadline. 0 disarms.
void cselect__test_short_next_capped_waits(uint32_t count);

// How many of those counts haven't been used yet.
uint32_t cselect__test_short_capped_waits_left(void);

// How many times a thread entered the delayed deregistration window, so a test
// can wait for it in a bounded loop instead of sleeping.
uint64_t cselect__test_timed_out_deregister_delay_count(void);

// Makes the next condition wait of ccircq_timed_send fail with EINVAL without
// waiting.
void ccircq__test_force_next_send_condvar_wait_error(void);

// Widens the window in notify_waiter() between holding a queue's mutex and
// locking wait_mtx by us microseconds (0 turns it off), so a test can land a
// fork() inside it every time. It applies to every notify of the process, so a
// test must set 0 when it ends.
void cthreadcomm__notify_waiter_test_set_delay_us(int us);

// Makes the next unpin of a ceventloop_reg (modify, pause, resume, remove,
// reg_generation) sleep ms milliseconds while it holds its resolve pin, so
// another thread can remove the registration meanwhile. That reaches the case
// where on_removed depends on the reactor's bounded reclaim retry. 0 disarms.
void ceventloop__test_delay_next_reg_resolve_unpin_ms(uint32_t ms);

// True when no delay is armed. Once the delay above is armed, true means the
// unpinning thread holds its pin and has read whether the registration is
// removed, so a test can poll this before it races a remove.
bool ceventloop__test_reg_resolve_unpin_delay_taken_for_tests(void);

// Locks and unlocks cq's mutex directly, so a test can hold it across a fork().
// Each lock must be followed by the unlock on the same thread.
void ccircq__test_lock_mutex_for_tests(ccircq *cq);

void ccircq__test_unlock_mutex_for_tests(ccircq *cq);

// Write-locks the loop's reg_slot_rwlock directly, so a thread other than the
// forking one can hold it across a fork(). It gives the resolved loop (NULL for
// an invalid handle, and then nothing is locked), and the unlock must take that
// pointer: Resolving the handle again can deadlock against a fork() that holds
// the slot table mutex while it waits for this lock. The unlock with NULL does
// nothing.
void *ceventloop__test_wrlock_reg_slot_for_tests(ceventloop loop);

void ceventloop__test_wrunlock_reg_slot_for_tests(void *resolved_loop);

// Write-locks and unlocks the rwlock of the process-wide loop slot table
// directly, for the same fork() tests. Each lock must be followed by the
// unlock.
void ceventloop__test_wrlock_slot_table_for_tests(void);

void ceventloop__test_wrunlock_slot_table_for_tests(void);

// Whether a cselect() waiter is linked into cq's read-waiter list, and how
// many are. A test polls these instead of sleeping after pthread_create(); the
// link order decides which waiter gets the next notify.
bool ccircq__test_has_sel_read_waiter_for_tests(ccircq *cq);

size_t ccircq__test_sel_read_waiter_count_for_tests(ccircq *cq);

// How many receivers (or senders) are parked on a queue's condition variable.
// A send signals only when that count isn't zero, so a test waits for the count
// before it sends, which also proves the wait site registers.
size_t ccircq__waiting_readers_for_tests(ccircq *cq);

size_t ccircq__waiting_writers_for_tests(ccircq *cq);

size_t cdynmq__waiting_readers_for_tests(cdynmq *dq);

// Like ccircq__test_force_next_send_condvar_wait_error(), but it also does the
// work of a concurrent receive (frees one slot and signals write_cond) under
// the same mutex. The send must use that slot instead of failing.
void ccircq__test_force_next_send_condvar_wait_error_racing_ready(void);

// Hands out, once, the message that the imitated receive above took out of the
// queue, so the test can free its data; {NULL, 0} after that.
cmessage ccircq__test_take_race_freed_msg(void);

// Makes the next condition wait of ccircq_timed_recv fail with EINVAL.
void ccircq__test_force_next_recv_condvar_wait_error(void);

// Like the hook above, but it also sends a {NULL, 0} sentinel through the
// normal send path under the same mutex. The receive must take it instead of
// failing.
void ccircq__test_force_next_recv_condvar_wait_error_racing_ready(void);

// The two hooks above for cdynmq_timed_recv.
void cdynmq__test_force_next_recv_condvar_wait_error(void);

void cdynmq__test_force_next_recv_condvar_wait_error_racing_ready(void);

// Makes the next cascade step after a queue or cchan dispatch sleep us
// microseconds after it starts looking at the queue and before it takes the
// queue's mutex, the window where a concurrent remove and queue destroy must be
// held off. 0 disarms.
void ceventloop__test_delay_next_queue_cascade_us(uint32_t us);

// Reads the process-wide counts of cascade steps that started and finished
// (either pointer may be NULL), so a test can prove a remove never returns
// while a step is between the two.
void ceventloop__test_queue_cascade_counts(uint64_t *begun, uint64_t *finished);

// How many eventfd writes the queue notify path has made to cselect() waiters
// and loop queue registrations (not the loop's own eventfds). A listener with a
// wake already pending must cost no extra write.
uint64_t cselect__test_eventfd_wake_count(void);

// Makes allocation number index (counted from 1 across every cselect call of
// the process) fail, then disarms; 0 disarms at once.
void cselect__test_fail_alloc(uint32_t index);

// How many poll sets cselect_timed() has created. A call that returns at once
// creates none.
uint64_t cselect__test_poll_sets_created(void);

// The lock stripe that fd maps to, below num_stripes.
size_t ceventloop__test_stripe_index(size_t num_stripes, int fd);

// Makes the next count stores of a thread mark (poller, dispatch job, queue
// dispatch) fail as pthread_setspecific() does without memory, so the thread
// uses its fallback copy.
void ceventloop__test_fail_next_marks(uint32_t count);

// Whether the calling thread carries the poller or dispatch mark of loop, the
// answer the self-call guards of shutdown and destroy use.
bool ceventloop__test_runs_on_loop_thread(ceventloop loop);

// Whether the calling thread runs a callback that dispatches queue (a ccircq
// or cdynmq), which a queue destroy refuses.
bool ceventloop__test_dispatches_queue(const void *queue);

#if CCOL_FORK_SAFETY_REQUIRED
// Gives the number of registered queue mutexes and the entries that removes
// have examined so far, and true when each queue's registry index names its
// entry.
bool ccol__queue_mutex_registry_check_for_tests(size_t *count,
                                                uint64_t *remove_probes);
#endif

// Makes the next push onto the free list of the loop slot table fail as an
// allocation would, to reach the recovery of a lost index.
void ceventloop__force_next_free_index_push_failure_for_tests(void);

// The same for a released registration slot. The count of registration slots
// a loop has ever allocated must not grow when a lost index is recovered.
void ceventloop__force_next_reg_free_index_push_failure_for_tests(void);
size_t ceventloop__reg_slot_count_for_tests(ceventloop loop);

// The thread ID of the loop's poller, joined or not, so a test can start
// threads until one gets that ID and prove the guards don't go by ID.
ccol__thread_id ceventloop__poller_thread_for_tests(ceventloop loop);

// How many event entries the epoch reclaim has freed, 0 for a bad handle.
uint64_t ceventloop__entries_reclaimed_for_tests(ceventloop loop);

// Sets the poller batch generation, so a test can start it below a 32-bit
// boundary and check that a deferred free past it is reclaimed.
void ceventloop__set_poller_batch_gen_for_tests(ceventloop loop,
                                                uint64_t value);

// How many slots the loop handle table holds, freed ones included, to check
// that create and destroy churn reuses them.
size_t ceventloop__slot_table_capacity_for_tests(void);

// Resolves and pins h for real, sets *pinned, waits for *release and unpins.
// A destroy meanwhile can't return. False when h doesn't resolve.
bool ceventloop__resolve_pin_until_for_tests(ceventloop h, atomic_int *pinned,
                                             atomic_int *release);

struct ceventloop__s;

// Resolves h without a pin (so there's no unpin to forget); NULL when the real
// resolve would fail.
struct ceventloop__s *ceventloop__resolve_for_tests(ceventloop h);
#endif
