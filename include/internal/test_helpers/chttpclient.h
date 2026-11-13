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

// Internal only: The test hooks of chttpclient. src/chttpclient.c defines them
// and the chttpclient tests call them, to reach internal helpers and overflow
// guards, force failures, hold a thread at a chosen point and read state the
// public API doesn't show. They exist only when RUNNING_UNIT_TESTS is defined,
// so nothing here is installed or exported.
#include <chttpclient.h>
#include <sys/socket.h>

#ifdef RUNNING_UNIT_TESTS
// The client struct behind a chttpcli handle, defined in src/chttpclient.c; a
// test only passes its pointer around.
struct chttpclient;

// Connect attempts
//
// Replaces the Happy Eyeballs attempt delay with ms (0 restores it), so a test
// can tell a hand-over that waits for the delay from one that doesn't.
void chttp__set_connect_attempt_delay_ms_for_tests(long ms);

// Makes the next n attempt delays last 1 ms each, so a second attempt starts at
// once while a later one gets the long delay.
void chttp__set_connect_short_delays_for_tests(int n);

// Makes the next n Tier 1 attempts that complete count as ECONNREFUSED, like a
// remote host that refuses after the connect started, so a test can make the
// race fall back.
void chttp__set_connect_async_failures_for_tests(int n);

// Handle slot table
//
// Makes the next push onto the free list of slot indexes act as if its
// allocation failed, so a test reaches the lost-index path.
void chttpcli__force_next_free_index_push_failure_for_tests(void);

// Gives the client behind h without pinning it, or NULL as the real resolve
// would. Only for a test that runs on one thread with no concurrent destroy.
struct chttpclient *chttpcli__resolve_for_tests(chttpcli h);

// Give the number of indexes on the free list and of slots in the table, so a
// test can see a lost slot whatever earlier tests left behind.
size_t chttpcli__free_index_count_for_tests(void);
size_t chttpcli__slot_table_capacity_for_tests(void);

// Gives the number of pins held on h's slot, so a test can check that resolves
// and unpins stay balanced.
size_t chttpcli__pin_count_for_tests(chttpcli h);

// Overflow guards of the length arithmetic
//
// Each of these hands an internal function a made-up length far above anything
// allocated and gives whether the guard refused it, so a test reaches the guard
// without exabytes of memory. Call them only with a length above the guard's
// threshold, or the function reads the small real buffer out of bounds.
//
// The guard of the percent-encoding of a unix socket path; fake_len stands in
// for strlen(path).
bool chttp__percent_encode_unix_path_overflow_guard_for_tests(
    ccol_memmgmt_procs *mp, const char *path, size_t fake_len);

// The guard of the merge of a reference path onto a base path.
bool chttp__merge_ref_path_overflow_guard_for_tests(
    ccol_memmgmt_procs *mp, const char *base_path_and_query,
    const char *ref_path, size_t fake_ref_path_len);

// The guard of the concatenation of two strings.
bool chttp__concat_len_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                const char *a,
                                                size_t fake_a_len,
                                                const char *b,
                                                size_t fake_b_len);

// The guard of the list of header names a request has sent; count and capacity
// are both fake_cap.
bool chttp__seen_names_add_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                    size_t fake_cap);

// The guard of an append of n bytes (at most 64) to the request buffer whose
// length is fake_len. Gives whether the append failed as an allocation failure;
// the caller checks that mp was never called.
bool chttp__ob_append_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                               size_t fake_len, size_t n);

// Grows the request buffer from a capacity of fake_cap, whose doubling would
// pass SIZE_MAX, with an allocator whose realloc fails, so a test sees that the
// capacity never wraps. Gives whether the append failed as an allocation
// failure, and never returns if the growth wraps.
bool chttp__ob_append_cap_overflow_for_tests(ccol_memmgmt_procs *mp,
                                             size_t fake_cap);

// The guard of the buffered body sink, whose body length is fake_len.
bool chttp__sink_buffered_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                   size_t fake_len);

// Grows the buffered body from a capacity of fake_cap with an allocator whose
// realloc fails, and gives whether the sink refused the bytes as an allocation
// failure.
bool chttp__sink_buffered_cap_overflow_for_tests(ccol_memmgmt_procs *mp,
                                                 size_t fake_cap);

// URLs and redirects
//
// With an mp of NULL every string these give comes from plain malloc, and the
// test frees it with free().
//
// Tells a test allocator whether the calling thread is resolving a redirect, so
// it can fail exactly those allocations.
bool chttp__in_redirect_resolve_for_tests(void);

// Parses url as a request does and flattens the internal result into the out
// parameters (each may be NULL), so a test can check the URL parser.
ccol_retval chttp__parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out, bool *is_unix_out,
    char **unix_socket_path_out);

// Resolves location against base_url as a redirect hop does, so a test can
// check RFC 3986 resolution. Gives the absolute URL, or NULL.
char *chttp__resolve_redirect_url_for_tests(const char *base_url,
                                            const char *location);

// Runs the redirect transport check on a resolved next_url with the caller's
// allocator, so a test can fail each of its allocations.
ccol_retval chttp__redirect_transport_check_for_tests(ccol_memmgmt_procs *mp,
                                                      const char *base_url,
                                                      const char *next_url,
                                                      bool prevent_downgrade);

// Resolves location against base_url, gives the result in *resolved_out (NULL
// when it doesn't resolve) and returns whether the transport check accepts it,
// so a test can check the AF_UNIX and downgrade rules. prevent_downgrade is the
// request's prevent_tls_downgrade_on_redirect.
bool chttp__redirect_transport_allowed_for_tests(const char *base_url,
                                                 const char *location,
                                                 bool prevent_downgrade,
                                                 char **resolved_out);

// Deadlines and waits
//
// Runs the remaining-milliseconds conversion on a deadline add_sec seconds from
// now, so a test can try distances whose milliseconds don't fit a long.
bool chttp__deadline_remaining_ms_for_tests(long long add_sec, int *out_ms);

// Gives the milliseconds between two instants, rounded down, so a test can feed
// it instants on both sides of a second boundary.
long long chttp__ms_elapsed_floor_for_tests(long long from_sec, long from_nsec,
                                            long long to_sec, long to_nsec);

// Runs the remaining-milliseconds conversion at a fixed instant, for a deadline
// ahead_ns after it, so the rounding is checked without racing the clock.
bool chttp__deadline_remaining_ms_at_for_tests(long long ahead_ns, int *out_ms);

// While on, every read point of every tier waits for its fd to be readable and
// then sees its overall deadline as passed, so a test hits that case at the
// same point on each tier.
void chttp__set_expire_at_read_for_tests(bool on);

// While cap isn't 0, the first TLS read of each Tier 2 read dispatch asks for
// at most cap bytes, so a test can leave part of a record inside the TLS layer.
void chttp__set_async_tls_first_read_cap_for_tests(size_t cap);

// Makes the next n waits capped at INT_MAX ms return at once with no event, so
// a test checks that callers wait again until their real deadline.
void chttp__short_next_capped_waits_for_tests(int n);

// Name resolution and address lists
//
// Adds addr to a set of at most 4 silent addresses, whose attempts get a socket
// that never connects and never fails, like a dropped SYN. NULL empties the
// set.
void chttp__set_silent_addr_for_tests(const struct sockaddr *addr,
                                      socklen_t len);

// Answers every TCP resolution with these count addresses (at most 8) in this
// order instead of calling getaddrinfo(), so a test controls the candidates.
// NULL or a count of 0 clears it.
void chttp__set_addr_override_for_tests(const struct sockaddr *const *addrs,
                                        const socklen_t *lens, size_t count);

// Builds a list where address i has family families[i], interleaves it as a
// connect race does and writes the new order of the original indexes to
// order_out, so a test can check the interleave.
void chttp__addr_list_interleave_for_tests(const int *families, size_t n,
                                           size_t *order_out);

// While ms isn't 0, every name lookup sleeps that long first, like a resolver
// that doesn't answer. Address literals aren't affected.
void chttp__set_name_lookup_delay_ms_for_tests(unsigned ms);

// Gives the number of name lookups all tiers started (address literals don't
// count), so a test can check that a request made none.
unsigned long chttp__name_lookups_for_tests(void);

// TLS
//
// Creates the client side of a TLS connection for host on a socket pair and
// gives the code a request would report, so a test reaches the empty-host
// refusal that no URL can produce.
ccol_retval chttp__tls_client_create_code_for_tests(const char *host,
                                                    bool verify_host);

// Runs a full handshake over a socket pair (the client verifies host against
// ca_path, the server offers default_cert and named_cert under the SNI name
// named_as), so a test can check SNI. Gives true when both sides finish.
bool chttp__tls_client_handshake_for_tests(
    const char *host, const char *ca_path, const char *default_cert,
    const char *default_key, const char *named_as, const char *named_cert,
    const char *named_key);

// Arms the TLS direction injection, so a test can check that the code waits in
// the direction a TLS call asks for: 0 disarms it, 1 makes a TLS write need the
// connection readable first, 2 makes a TLS read need it writable first. Arming
// clears the counters below; disarm it before the test returns.
void chttpcli__tls_dir_inject_for_tests(int mode);

// True once the module released the injected block by waiting or registering in
// the direction the injection asked for.
bool chttpcli__tls_dir_released_for_tests(void);

// How often the module asked for the read direction while writing, and for the
// write direction while reading.
unsigned chttpcli__tls_dir_read_interest_while_writing_for_tests(void);
unsigned chttpcli__tls_dir_write_interest_while_reading_for_tests(void);

// How many TLS attempts the armed injection refused. A caller that follows the
// reported direction pays one; more measures a busy spin.
unsigned chttpcli__tls_dir_block_attempts_for_tests(void);

// Response fields and the Tier 1 read loop
//
// Gives how many record headers of repeated response fields were read, so a
// test can check that nothing rescans them.
unsigned long chttpcli__field_rec_reads_for_tests(void);

// Give the number of reads and of readability waits in the Tier 1 read loop, so
// a test can check that a large body isn't read with a wait per read.
unsigned long chttp__tier1_reads_for_tests(void);
unsigned long chttp__tier1_read_waits_for_tests(void);

// Gives the number of Tier 1 hops that opened a 100-continue window, so a test
// can check that a request which must not wait never opens one.
unsigned long chttp__tier1_continue_waits_for_tests(void);

// Deadline sweep of Tiers 2 and 3
//
// Gives how many sweep ticks the process has run, so a test can count walks.
unsigned long chttp__sweep_ticks_for_tests(void);

// For each kind of timed work (0: An expired deadline, 1: A Happy Eyeballs
// start), reset or read how often a walk acted on it and the largest gap
// between the walk's planned instant and the due instant, so a test can check
// the plan on a loaded machine. With a count of 0 the gap is INT64_MIN.
void chttp__sweep_due_reset_for_tests(void);
unsigned long chttp__sweep_due_for_tests(int kind, int64_t *max_late_ns);

// Arm, query, release and disarm a sweep that declares the first reading ctx
// expired and parks on it in phase 1 (after its verdict) and phase 2 (before
// the shutdown of the fd), so a test can race it against the idle-pool offer.
// Each park is bounded.
void chttp__sweep_race_arm_for_tests(void);
int chttp__sweep_race_parked_phase_for_tests(void);
void chttp__sweep_race_release_for_tests(int phase);
void chttp__sweep_race_disarm_for_tests(void);

// How many times the idle-pool offer entered, and finished, its critical
// section, for the same race tests.
unsigned chttp__offer_entered_count_for_tests(void);
unsigned chttp__offer_done_count_for_tests(void);

// Arm, query and release a park of the next sweep tick that meets a ctx waiting
// for 100 Continue, after its critical section and before it judges the window
// (10 s at most), so a test can race the continue decision. A release counts
// even when it comes before the park. The last one counts stores of the
// continue deadline.
void chttp__continue_sweep_arm_for_tests(void);
bool chttp__continue_sweep_parked_for_tests(void);
void chttp__continue_sweep_release_for_tests(void);
unsigned chttp__continue_deadline_stores_for_tests(void);

// Arms the first sweep that meets a connecting ctx with no fd and no race to
// wait until the connect registers its fd and the deadline passes, so a test
// hits a connect that starts as the sweep judges it. The second tells whether
// it waited, and disarms it.
void chttp__sweep_fd_wait_arm_for_tests(void);
bool chttp__sweep_fd_wait_held_for_tests(void);

// Writes what the armed hold saw into buf, so a test can print why the hold
// didn't happen.
void chttp__sweep_fd_wait_trace_for_tests(char *buf, size_t size);

// Gives how many hops have a future the sweep fulfilled while the hop still
// exists, so a test can check they're counted and released.
size_t chttp__async_hops_abandoned_for_tests(void);

// Makes the next hop that turns to reading get no dispatch (its registration is
// paused), so a test can check that the request timeout still holds; the resume
// gives true when it resumed one.
void chttp__stall_next_read_for_tests(void);
bool chttp__stalled_read_resume_for_tests(void);

// Engine
//
// Gives the size of the internal async chain struct, so a fault-injecting
// allocator can fail exactly that allocation.
size_t chttp__async_chain_struct_size_for_tests(void);

// Give the engine's reference count and the logger it uses.
int chttpcli__engine_ref_count_for_tests(void);
clog chttpcli__engine_logger_for_tests(void);

// Removes and closes the logger installed by chttpcli_set_engine_logger, so the
// next engine run opens a fallback logger. The engine must not run.
void chttpcli__clear_engine_user_logger_for_tests(void);

// Tell whether the engine runs, and call its acquire, release and wait for
// quiescence directly, so a test can drive its lifecycle.
bool chttpcli__engine_running_for_tests(void);
ccol_retval chttpcli__engine_acquire_for_tests(void);
void chttpcli__engine_release_for_tests(void);
void chttpcli__engine_wait_for_quiescence_for_tests(void);

// Tells whether this process started initializing the engine globals, so a test
// can check that an unused module initializes nothing.
bool chttpcli__engine_globals_entered_for_tests(void);

// Gives the running engine's DNS pool, or CTHRPOOL_NULL with no engine.
cthrpool chttpcli__engine_dns_pool_for_tests(void);

// Gives the reactor thread count the last engine start resolved.
size_t chttpcli__engine_num_reactor_threads_for_tests(void);

// Makes the next reaper hold the engine mutex ms milliseconds in its final
// critical section and tells whether it got there, so a test can race the end
// of a teardown.
void chttpcli__hold_next_reaper_final_section_for_tests(int ms);
bool chttpcli__reaper_final_section_was_entered_for_tests(void);

// Makes the next reaper wait ms milliseconds with no lock after it closes the
// fallback logger, and tells when that wait starts and ends, so a test can fork
// while the reaper finishes.
void chttpcli__hold_next_reaper_tail_for_tests(int ms);
bool chttpcli__reaper_tail_was_entered_for_tests(void);
bool chttpcli__reaper_tail_was_left_for_tests(void);

// Waits until no engine teardown is in flight and leaves the reaper unjoined,
// the state between an engine stop and the next acquire.
void chttpcli__engine_wait_stopped_unjoined_for_tests(void);

// Copies the handle of the unjoined reaper into *out, so a test can compare it
// after a fork. Gives false when there's none.
bool chttpcli__engine_unjoined_reaper_for_tests(ccol__thread_id *out);

// Hold the next reaper between its creation and the publish of its handle (10 s
// at most, once), tell whether it's held, and release it, so a test can start a
// second spawn in that window.
void chttpcli__hold_next_reaper_spawn_for_tests(void);
bool chttpcli__reaper_spawn_is_held_for_tests(void);
void chttpcli__release_reaper_spawn_for_tests(void);

// How many reaper threads were created and joined; they match once the engine
// is quiescent, so a lost handle shows.
unsigned chttpcli__reaper_created_count_for_tests(void);
unsigned chttpcli__reaper_joined_count_for_tests(void);

// While on, every reaper thread creation fails, so a test reaches the
// abandoned-reap path; the counter says how often the reap was given up.
void chttpcli__force_reaper_spawn_fail_for_tests(bool on);
unsigned chttpcli__engine_reaper_abandoned_count_for_tests(void);

// Gives -1 until the shutdown of an abandoned ctx has run, then 1 if the last
// one ran under the ctx's idle_lock (the only safe order) and 0 if after the
// unlock; the reset sets it back to -1.
int chttpcli__abandon_shutdown_under_lock_for_tests(void);
void chttpcli__reset_abandon_shutdown_probe_for_tests(void);

// Idle pools
//
// Ages every connection in cli's Tier 2 and Tier 3 idle pool past the staleness
// limit, so the next take evicts it without a 60 s wait.
void chttpcli__force_async_idle_stale_for_tests(struct chttpclient *cli);

// Shuts down the fd of every connection in cli's Tier 2 and Tier 3 idle pool,
// so each one's dispatch frees it on a reactor thread.
void chttpcli__shutdown_async_idle_connections_for_tests(
    struct chttpclient *cli);

// Give the number of connections in cli's Tier 2 and Tier 3 idle pool, and of
// those with a timed_out mark (0 when correct).
size_t chttpcli__async_idle_total_count_for_tests(struct chttpclient *cli);
size_t chttpcli__async_idle_timed_out_count_for_tests(struct chttpclient *cli);

// Give the number of origins with an entry in cli's Tier 1 and Tier 2 idle
// pools, so a test can see an emptied origin lose its entry.
size_t chttpcli__idle_pools_key_count_for_tests(struct chttpclient *cli);
size_t chttpcli__idle_pools_async_key_count_for_tests(struct chttpclient *cli);

// Overrides the idle origin cap for the whole process (0 restores it), so a
// test reaches the cap with a few origins.
void chttpcli__set_max_idle_origins_for_tests(size_t n);

// Makes the next push onto an origin's idle list fail once, which allocator
// failures can't reach.
void chttpcli__force_offer_push_fail_once_for_tests(void);

// Make the next plain-HTTP hop that finishes writing, or the next connection
// that goes idle, turn its registration back to the write direction once, as a
// late continue-timeout flip of the sweep does.
void chttpcli__force_stray_write_arm_once_for_tests(void);
void chttpcli__force_stray_idle_write_arm_once_for_tests(void);

// Hold the IDLE branch of the read dispatch once, between deciding that a ctx
// with input waiting is idle and reading, so a take can win the race. The wait
// tells whether it got there within timeout_ms; the release ends or disarms it.
void chttpcli__arm_idle_probe_hold_for_tests(void);
bool chttpcli__wait_idle_probe_held_for_tests(long timeout_ms);
void chttpcli__release_idle_probe_hold_for_tests(void);

// Widens the window on both sides of the 100-continue carry replay by ms (0
// turns it off), so a test can race a destroy against it.
void chttpcli__set_continue_carry_replay_delay_ms_for_tests(long ms);

// Arms one stray write dispatch at a take: The take arms the write direction
// under idle_lock before it builds the new hop and waits 200 ms.
void chttpcli__arm_stray_write_at_take_for_tests(void);

// Arms one stray write dispatch that races a take: The dispatch classifies the
// ctx as IDLE, then the take moves it to WRITING and holds idle_lock for 200
// ms. The second tells whether that happened, and disarms the hook.
void chttpcli__arm_stray_write_before_take_for_tests(void);
bool chttpcli__stray_write_before_take_held_for_tests(void);

// Gives how often the write dispatch put a stray registration back to reading.
unsigned long long chttpcli__stray_write_arm_restores_for_tests(void);

// Makes the next reactivation of a reused connection fail once, which allocator
// failures can't reach.
void chttpcli__force_reactivate_fail_once_for_tests(void);

// Makes the next read dispatch of a ctx that has read before act as ECONNRESET,
// without touching the socket, so a test reaches that error path.
void chttpcli__force_async_hard_read_error_once_for_tests(void);

// Reads back cli's connect, request and inactivity timeouts in microseconds, so
// a test can check the setters and defaults. Gives false for a handle that
// doesn't resolve.
bool chttpcli__timeouts_for_tests(chttpcli cli, uint64_t *connect_us,
                                  uint64_t *request_us,
                                  uint64_t *inactivity_us);

// The initializations of chttpcli_create_mp that
// chttpcli__arm_create_fault_for_tests can fail, in create order.
typedef enum {
  CHTTPCLI__FAULT_NONE = 0,
  CHTTPCLI__FAULT_LOCK,
  CHTTPCLI__FAULT_AVAILABLE_CV,
  CHTTPCLI__FAULT_IDLE_ASYNC_DRAINED_CV,
  CHTTPCLI__FAULT_ASYNC_COUNT_LOCK,
  CHTTPCLI__FAULT_ASYNC_COUNT_DRAINED_CV
} chttpcli__fault;

// Arms one failure at step, so a test can walk every failure path of create:
// the next create that reaches it sees the init fail and the hook disarms
// itself (CHTTPCLI__FAULT_NONE disarms it). The failing init is skipped, so the
// failure path has nothing of it to destroy.
void chttpcli__arm_create_fault_for_tests(chttpcli__fault step);
#endif
