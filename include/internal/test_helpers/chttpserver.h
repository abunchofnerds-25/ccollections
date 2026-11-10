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

// Test-only hooks of chttpserver, internal. src/chttpserver.c defines them
// and the chttpserver tests call them to force failures, hold a thread at a
// chosen point of a race, move the server's clocks and read state that the
// public API doesn't show. They exist only in a build that defines
// RUNNING_UNIT_TESTS, which the shipped library never does.

#include <chttpserver.h>

#ifdef RUNNING_UNIT_TESTS
#include <stdatomic.h>

struct chttpserver;

// Engine, reaper and stop watcher

// Raises the 2 s ceiling of the writes the library makes itself, so a client
// that reads a large rejection slowly still gets all of it. 0 restores the
// default ceiling.
void chttpsvr__set_internal_write_ceiling_ms_for_tests(unsigned ms);

// A delay in ms between creating the engine reaper and recording its thread,
// so a test can let the reaper run to its end inside that window.
void chttpsvr__set_spawn_reaper_delay_for_tests(unsigned ms);

// While force is set, creating the reaper thread fails as if the system had
// no thread left to give, so the reaper runs inline.
void chttpsvr__force_reaper_thread_create_fail_for_tests(bool force);

// The process that created the engine's reactor (0 when none runs), for a
// test that checks that a fork() child runs a reactor of its own.
pid_t chttpsvr__engine_reactor_pid_for_tests(void);

// True while there is an engine reaper thread that nobody has joined yet.
bool chttpsvr__engine_reaper_unjoined_for_tests(void);

// A delay in ms that the stop watcher waits after each wakeup before it acts
// on a stop request, so a test can start an engine between a
// chttpsvr_engine_stop() call and the work that it asks for.
void chttpsvr__set_engine_stop_watcher_delay_for_tests(unsigned ms);

// While force is set, creating the stop watcher thread fails as if the system
// had no thread left to give.
void chttpsvr__force_watcher_thread_create_fail_for_tests(bool force);

// While force is set, creating the idle sweep thread fails as if the system
// had no thread left to give, so chttpsvr_start() fails.
void chttpsvr__force_idle_sweep_thread_create_fail_for_tests(bool force);

// How many times the idle sweep thread has stopped without sitting out a tick
// (a stop woke its wait, or came while it swept), and how many times only
// after its wait for the tick ran out.
void chttpsvr__idle_sweep_exits_for_tests(size_t *woken, size_t *timed_out);

// Locks the engine mutex and then raises sig on the calling thread, so the
// signal handler runs on a thread that holds that mutex, as a real signal can
// land. A handler that calls chttpsvr_engine_stop() must not deadlock here.
void chttpsvr__test_hold_engine_mutex_and_signal_self(int sig);

// How many kernel queries judged a parked connection, and how many of them
// the sweep made while it held the idle mutex (that one must stay 0).
void chttpsvr__parked_queries_for_tests(size_t *total, size_t *locked);

// The growth rule of the internal registries: 0 gives initial, a capacity
// that doubled would overflow gives 0, and any other gives twice cap.
size_t chttpsvr__doubling_growth_cap_for_tests(size_t cap, size_t elem_size,
                                               size_t initial);

// Makes the next registration of a server with the engine fail, as a failed
// allocation of its growth would.
void chttpsvr__fail_next_servers_register_for_tests(void);

// Connections, responses and rejections

// How many direction changes of a connection's registration went to the
// reactor; a request for the direction it already has is skipped and not
// counted.
size_t chttpsvr__conn_reg_modify_calls_for_tests(void);

// How many requests have been handed to the worker pool, so a test can tell
// that a request is waiting in its queue.
extern atomic_size_t chttpsvr__worker_submits_for_tests;

// Any value other than INT64_MIN replaces the clock of the Date header, so a
// test can cross a second boundary without sleeping.
void chttpsvr__set_date_override_for_tests(int64_t sec);

// The next handler response write sends only its first n bytes and then acts
// as if the socket were full, so a test can watch the parked rest resume.
// Disarms itself after one use.
void chttpsvr__force_short_response_write_for_tests(size_t n);

// Armed with 0 < n < 25, the next interim 100 Continue line sends only its
// first n bytes and then acts as if its deadline had run out. Disarms itself
// after one use.
void chttpsvr__force_short_interim_write_for_tests(size_t n);

// The next write-deadline check of a response write reports the deadline as
// already expired, whatever the real elapsed time. Disarms itself after one
// use.
void chttpsvr__force_next_response_write_deadline_expired_for_tests(void);

// The next interim line that a buffered route writes without waiting sends
// its first n bytes (n may be 0) and then acts as if the socket were full.
// Disarms itself after one use; SIZE_MAX means "not armed".
void chttpsvr__force_interim_park_after_for_tests(size_t n);

// Holds the nth worker submit from now, after it has read the pool and before
// it submits, until the release call, so a test can restart the server inside
// that window. The retried count tells how many submits moved to a newer
// pool.
void chttpsvr__arm_divert_submit_hook_for_tests(int nth);
size_t chttpsvr__submit_retried_for_tests(void);
bool chttpsvr__divert_submit_hook_entered_for_tests(void);
void chttpsvr__release_divert_submit_hook_for_tests(void);

// While force is set, every pause of a connection's registration takes the
// failed-pause path; the count tells how many removals that path made.
void chttpsvr__force_pause_fail_for_tests(bool force);
size_t chttpsvr__pause_fallback_count_for_tests(void);

// When armed, the next rejection skips the reject pool and is answered inline
// on the rejecting thread, as when that pool is full. Disarms itself after
// one use.
void chttpsvr__force_reject_inline_for_tests(bool force);

// While force is set, the first stream prepare of a worker counts as failed,
// so the request takes the same fallback as a failed allocation there.
void chttpsvr__force_stream_prepare_fail_for_tests(bool force);

// Race hooks
//
// Each arm call makes the named point block once; the wait call returns when
// a thread got there, and the release call lets that thread go on. A test uses
// them to run a race in one chosen order every time instead of by luck.

// chttpsvr_start(), after it has acquired the engine reference and registered
// the server, and before any more reactor work.
void chttpsvr__arm_start_race_hook_for_tests(void);
void chttpsvr__wait_start_race_hook_entered_for_tests(void);
void chttpsvr__release_start_race_hook_for_tests(void);

// chttpsvr_start(), right before its engine acquire, where that acquire can
// find the engine stopping.
void chttpsvr__arm_engine_stopping_race_hook_for_tests(void);
void chttpsvr__wait_engine_stopping_race_hook_entered_for_tests(void);
void chttpsvr__release_engine_stopping_race_hook_for_tests(void);

// chttpsvr_start(), right after its resolve pin is live and before any of its
// own checks.
void chttpsvr__arm_start_resolve_race_hook_for_tests(void);
void chttpsvr__wait_start_resolve_race_hook_entered_for_tests(void);
void chttpsvr__release_start_resolve_race_hook_for_tests(void);

// The stop of a server, after it entered the stopping state and before it
// removes and closes the old listener registration.
void chttpsvr__arm_stop_race_hook_for_tests(void);
void chttpsvr__wait_stop_race_hook_entered_for_tests(void);
void chttpsvr__release_stop_race_hook_for_tests(void);

// The reaper's stop of every server, after it has pinned a server and before
// it quiesces that server.
void chttpsvr__arm_reaper_race_hook_for_tests(void);
void chttpsvr__wait_reaper_race_hook_entered_for_tests(void);
void chttpsvr__release_reaper_race_hook_for_tests(void);

// The quiesce of a server, after it has claimed the teardown and before any
// of the real teardown work.
void chttpsvr__arm_quiesce_teardown_race_hook_for_tests(void);
void chttpsvr__wait_quiesce_teardown_race_hook_entered_for_tests(void);
void chttpsvr__release_quiesce_teardown_race_hook_for_tests(void);

// The quiescing backoff of chttpsvr_start(), after it has registered as a
// waiter and released its resolve pin, and before it takes the server mutex
// again. The wait is bounded to 10 s and returns false on a timeout.
void chttpsvr__arm_start_quiescing_unpinned_race_hook_for_tests(void);
bool chttpsvr__wait_start_quiescing_unpinned_race_hook_entered_for_tests(void);
void chttpsvr__release_start_quiescing_unpinned_race_hook_for_tests(void);

// Signals once, without blocking, when the next quiesce of a server has
// claimed it. The wait is bounded to 10 s and returns false on a timeout.
void chttpsvr__arm_quiesce_claimed_signal_for_tests(void);
bool chttpsvr__wait_quiesce_claimed_signal_entered_for_tests(void);

// Signals once, without blocking, when chttpsvr_start() has reached its wait
// for a server that is still stopping and is about to enter it.
void chttpsvr__arm_start_stopping_wait_signal_for_tests(void);
void chttpsvr__wait_start_stopping_wait_signal_entered_for_tests(void);

// A listener dispatch, after it has pinned the server and before it accepts;
// the entry form blocks at the first statement of the dispatch instead,
// before the pin.
void chttpsvr__arm_listener_dispatch_race_hook_for_tests(void);
void chttpsvr__arm_listener_dispatch_entry_race_hook_for_tests(void);
void chttpsvr__wait_listener_dispatch_race_hook_entered_for_tests(void);
void chttpsvr__release_listener_dispatch_race_hook_for_tests(void);

// Slow clients, parked connections and body memory

// How many parks of one kind happened, indexed by the park kind.
size_t chttpsvr__park_kind_count_for_tests(int kind);

// Moves the clock of every slow-client limit, and nothing else, forward by
// ms, so a test can expire a limit without sleeping.
void chttpsvr__advance_slow_clock_for_tests(long long ms);

// How many parked connections were resumed, and how many parks happened.
size_t chttpsvr__resume_dispatch_count_for_tests(void);
size_t chttpsvr__park_count_for_tests(void);

// Turns the receive low-water mark of a parked body on or off.
void chttpsvr__set_rcvlowat_enabled_for_tests(bool enabled);

// Holds a streaming-pool task just before it takes the next queued request
// (where 1) or just after (where 2); 0 disarms. The wait returns false when
// no task got there within timeout_ms.
void chttpsvr__arm_stream_claim_hook_for_tests(int where);
bool chttpsvr__wait_stream_claim_hook_entered_for_tests(unsigned timeout_ms);
void chttpsvr__release_stream_claim_hook_for_tests(void);

// The highest body memory in use that any server has reached since the last
// reset.
size_t chttpsvr__mem_peak_for_tests(void);
void chttpsvr__mem_peak_reset_for_tests(void);

// Since the last reset, the most that the body buffer of a request ever held
// beyond its reservation, the old and new blocks of a growth together.
size_t chttpsvr__body_overshoot_for_tests(void);
void chttpsvr__body_overshoot_reset_for_tests(void);

// Timeouts and clocks

// The pure idle-timeout decision for a now and last_activity pair that the
// test builds itself.
bool chttpsvr__conn_idle_timed_out_for_tests(struct timespec now,
                                             struct timespec last_activity,
                                             unsigned idle_ms);

// What the deadline check of a body read or a response write decides for a
// deadline that is already set, at the instant now, and the timeout it
// shrinks *timeout_ms_inout to.
bool chttpsvr__shrink_timeout_at_for_tests(struct timespec deadline,
                                           struct timespec now,
                                           unsigned *timeout_ms_inout);

// The milliseconds between then and now, rounded down, or up when ceil is
// set.
long long chttpsvr__ms_since_for_tests(struct timespec now,
                                       struct timespec then, bool ceil);

// The idle timeout in ms that chttpsvr_start() stored for h, or 0 for an
// invalid handle.
unsigned chttpsvr__idle_timeout_ms_for_tests(chttpsvr h);

// Every duration that chttpsvr_start() resolved for h, in ms, in this order:
// stream_read_timeout, max_body_read_duration, response_write_timeout,
// max_response_write_duration, max_header_read_duration, the idle timeout,
// min_transfer_rate_grace, body_memory_wait_timeout and
// streaming_queue_timeout. Returns false for an invalid handle.
bool chttpsvr__resolved_ms_for_tests(chttpsvr h, unsigned out[9]);

// Streaming pool and restarts

// The thread count of the streaming pool created last, or 0 when none was
// created since the test last took the value.
int chttpsvr__take_stream_pool_threads_for_tests(void);

// Holds the next creation of a streaming pool after it has read the size and
// before it creates the pool, until the release call, 10 s at most.
void chttpsvr__arm_stream_create_hold_for_tests(void);
bool chttpsvr__stream_create_hold_entered_for_tests(void);
void chttpsvr__release_stream_create_hold_for_tests(void);

// Holds the next chttpsvr_start() right after it has swapped in the pools of
// the new run and released the server mutex, until the release call, 10 s at
// most.
void chttpsvr__arm_start_hold_for_tests(void);
bool chttpsvr__start_hold_entered_for_tests(void);
void chttpsvr__release_start_hold_for_tests(void);

// How many streaming token tasks all servers of the process have submitted.
size_t chttpsvr__stream_tokens_started_for_tests(void);

// How many connections have started to linger before their close.
size_t chttpsvr__linger_count_for_tests(void);

// Listener

// The two errno classifiers of the accept loop.
bool chttpsvr__accept_errno_is_transient_for_tests(int e);
bool chttpsvr__accept_errno_is_resource_exhaustion_for_tests(int e);

// How often the accept loop paused a listener after an allocation failure
// that followed an accept, and how often a listener was paused for resource
// pressure. A test reads the difference over its own window.
size_t chttpsvr__listener_alloc_failure_pause_count_for_tests(void);
size_t chttpsvr__listener_resource_pause_count_for_tests(void);

// How many accepted connections had their socket buffers raised.
size_t chttpsvr__accepted_sockbuf_raise_count_for_tests(void);

// How many times the listener was dispatched. A paused listener stays at a
// small handful, while a busy-looping one climbs into the thousands.
size_t chttpsvr__listener_dispatch_count_for_tests(void);

// The most connections that one listener dispatch accepted since the last
// reset.
size_t chttpsvr__listener_max_accepts_per_dispatch_for_tests(void);
void chttpsvr__listener_max_accepts_per_dispatch_reset_for_tests(void);

// The errno of the last listening socket setup that failed.
int chttpsvr__last_listen_errno_for_tests(void);

// The next accept of the listener of h fails with errno_val, so a test can
// check how the accept loop handles an errno that is hard to make real.
void chttpsvr__force_next_accept_errno_for_tests(chttpsvr h, int errno_val);

// Teardown

// Overrides the graceful wait and the grace period of the bounded wait for
// in-flight requests at a stop; 0 in both uses the real values.
void chttpsvr__set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                   unsigned grace_ms);

// Overrides how long a teardown waits for every connection to close; 0 uses
// the real bound.
void chttpsvr__set_drain_connections_wait_ms_for_tests(unsigned ms);

// How many retired pool sets in the process have finished, and started,
// draining.
size_t chttpsvr__retired_drained_for_tests(void);
size_t chttpsvr__retired_drain_started_for_tests(void);

// Whether the shutdown of the worker pool that the latest restart retired has
// started. The caller must hold a task of that pool.
bool chttpsvr__retired_worker_shutdown_started_for_tests(void);

// How many teardowns of route data found a connection of the server still
// allocated; it must stay 0.
size_t chttpsvr__route_teardown_with_live_connections_for_tests(void);

// Names one server whose final free the next call reports, whatever the
// other servers of the process do.
void chttpsvr__watch_finish_destroy_for_tests(chttpsvr h);
bool chttpsvr__watched_finish_destroy_ran_for_tests(void);

// Engine and server state

// The reactor thread count wired into the reactor created last.
size_t chttpsvr__engine_num_reactor_threads_for_tests(void);

// How many rejection responses ran on a reject pool thread.
size_t chttpsvr__reject_pool_task_count_for_tests(void);

// How many requests of h are in flight, read under the server mutex, or -1
// for an invalid handle.
int chttpsvr__in_flight_requests_for_tests(chttpsvr h);

// Resolves h to its server without a pin, so there is no unpin to forget;
// NULL when h does not resolve.
struct chttpserver *chttpsvr__resolve_for_tests(chttpsvr h);

// How many slots the handle table holds right now.
size_t chttpsvr__slot_table_capacity_for_tests(void);

// How many servers are registered with the engine right now.
size_t chttpsvr__servers_bundler_count_for_tests(void);

// Marks the calling thread as a worker thread of h, so a test in a forked
// child can reach the self-call guard of chttpsvr_destroy(). Does nothing
// when h does not resolve.
void chttpsvr__mark_self_as_worker_for_tests(chttpsvr h);

// Slow-client state of h. Each one returns 0 or false for an invalid handle.
// mem_holders counts the connections that hold a body memory reservation and
// tells in *exempt_out whether one runs past the limit under the progress
// rule.
size_t chttpsvr__mem_in_use_for_tests(chttpsvr h);
size_t chttpsvr__mem_holders_for_tests(chttpsvr h, bool *exempt_out);

// For the request that a buffered route's handler holds: the body memory
// reservation of its connection and the capacity of its body buffer.
void chttpsvr__req_body_memory_for_tests(chttpsvr_req *req, size_t *charged,
                                         size_t *held);

// How many connections of h wait for body memory, how many streaming requests
// wait in its queue, and whether its streaming pool exists.
size_t chttpsvr__mem_waiting_for_tests(chttpsvr h);
size_t chttpsvr__stream_queue_len_for_tests(chttpsvr h);
bool chttpsvr__stream_pool_created_for_tests(chttpsvr h);

// How many tasks the worker pool, or the streaming pool, runs right now.
size_t chttpsvr__pool_active_for_tests(chttpsvr h, bool streaming);

// How many connections are parked on a body (kind 1) or on a response
// (kind 2).
size_t chttpsvr__parked_count_for_tests(chttpsvr h, int kind);

// What the connections parked on a response or lingering still hold of their
// request (and, when lingering, of their response); 0 when nothing.
size_t chttpsvr__parked_request_state_for_tests(chttpsvr h);

// How many connections are in the idle list of h.
size_t chttpsvr__idle_count_for_tests(chttpsvr h);

// Runs one sweep tick for h on the calling thread, with the clock read as
// ahead_ms later than it is, and returns how many connections it looked at.
// The now form uses no offset.
size_t chttpsvr__sweep_ahead_for_tests(chttpsvr h, unsigned ahead_ms);
size_t chttpsvr__sweep_now_for_tests(chttpsvr h);

// Exit-time cleanup

// Runs the exit-time destructors of the module now, in a forked child, so the
// test can then destroy a server as a late application destructor would.
void chttpsvr__run_exit_cleanup_for_tests(void);

// How many router shells the registry holds, and whether its array is
// allocated.
void chttpsvr__router_shell_registry_state_for_tests(size_t *count,
                                                     bool *array);

// True once the arrays of the slot table have been released.
bool chttpsvr__slot_table_released_for_tests(void);

// Whether the exit-time destructor counts the slot of h as live, and a
// setter of its in_use flag, so a test can build the window of a teardown
// where in_use is already clear and the slot still names the server.
bool chttpsvr__slot_live_for_tests(chttpsvr h);
void chttpsvr__slot_set_in_use_for_tests(chttpsvr h, bool in_use);

// True while a logger of chttpsvr_set_engine_logger() is installed.
bool chttpsvr__engine_user_log_set_for_tests(void);
#endif  // RUNNING_UNIT_TESTS
