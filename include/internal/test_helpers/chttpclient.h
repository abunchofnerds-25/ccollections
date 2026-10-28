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
 * @file test_helpers/chttpclient.h
 * @brief INTERNAL ONLY. The test-only hooks of chttpclient.
 *
 * src/chttpclient.c defines these hooks and the chttpclient tests call them.
 * They let a test reach internal helpers and overflow guards directly, force
 * failures, hold a thread at a chosen point and read state that the public
 * API doesn't show.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <chttpclient.h>
#include <sys/socket.h>

#ifdef RUNNING_UNIT_TESTS
/* The client struct behind a chttpcli handle; it is defined in
 * src/chttpclient.c, and a test only passes its pointer around. */
struct chttpclient;

/* ---------------------------------------------------------------------------
 * Connect attempts
 * ------------------------------------------------------------------------- */

/* A non-zero ms takes the place of the Happy Eyeballs connection attempt
 * delay, so a test can tell a hand-over that waits for the delay from one
 * that doesn't. */
void _chttp_set_connect_attempt_delay_ms_for_tests(long ms);

/* The next n attempt delays last only 1 ms each, so a second attempt can start
 * at once while a later one gets the long delay above. */
void _chttp_set_connect_short_delays_for_tests(int n);

/* The next n Tier 1 attempts that complete are treated as ECONNREFUSED, the
 * way a remote host refuses after the connect has started. */
void _chttp_set_connect_async_failures_for_tests(int n);

/* ---------------------------------------------------------------------------
 * Handle slot table
 * ------------------------------------------------------------------------- */

/* Makes the next push onto the free list of slot indexes act as if its
 * allocation had failed. The flag clears itself. */
void _chttpcli_force_next_free_index_push_failure_for_tests(void);

/* Resolves h to its client struct without pinning it, or gives NULL under the
 * same conditions as the real resolve. Only for a test that runs on one
 * thread with no concurrent destroy. */
struct chttpclient *_chttpcli_resolve_for_tests(chttpcli h);

/* The number of indexes on the free list and the number of slots in the
 * table. Read together, they show a lost slot whatever earlier tests left in
 * the table. */
size_t _chttpcli_free_index_count_for_tests(void);
size_t _chttpcli_slot_table_capacity_for_tests(void);

/* The number of pins held on h's slot right now, so a test can check that
 * resolves and unpins stay balanced. */
size_t _chttpcli_pin_count_for_tests(chttpcli h);

/* ---------------------------------------------------------------------------
 * Overflow guards of the length arithmetic
 *
 * Each helper hands an internal function a made-up length far above anything
 * allocated and returns whether the guard refused it. Call them only with a
 * length above the guard's threshold: below it the function would read the
 * small real buffer out of bounds.
 * ------------------------------------------------------------------------- */

/* The guard of the percent-encoding of a unix socket path; fake_len stands in
 * for strlen(path). */
bool _chttp_percent_encode_unix_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *path, size_t fake_len);

/* The guard of the merge of a reference path onto a base path. */
bool _chttp_merge_ref_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *base_path_and_query,
    const char *ref_path, size_t fake_ref_path_len);

/* The guard of the concatenation of two strings. */
bool _chttp_concat_len_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                const char *a,
                                                size_t fake_a_len,
                                                const char *b,
                                                size_t fake_b_len);

/* The guard of the list of header names a request has already seen; count
 * and capacity are both fake_cap. */
bool _chttp_seen_names_add_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                    size_t fake_cap);

/* The guard of an append of n bytes (at most 64) to the request output
 * buffer whose length is fake_len. It returns whether the append failed as an
 * allocation failure; the caller checks that mp was never called. */
bool _chttp_ob_append_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                               size_t fake_len, size_t n);

/* Grows the request output buffer from a capacity of fake_cap, whose doubling
 * would pass SIZE_MAX, with an allocator whose realloc fails. It returns
 * whether the append failed as an allocation failure, and it doesn't return
 * at all if the growth wraps. */
bool _chttp_ob_append_cap_overflow_for_tests(ccol_memmgmt_procs_t *mp,
                                             size_t fake_cap);

/* The guard of the buffered response body sink, whose body length is
 * fake_len. */
bool _chttp_sink_buffered_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                   size_t fake_len);

/* Grows the buffered body from a capacity of fake_cap with an allocator
 * whose realloc fails, and returns whether the sink refused the bytes as an
 * allocation failure. */
bool _chttp_sink_buffered_cap_overflow_for_tests(ccol_memmgmt_procs_t *mp,
                                                 size_t fake_cap);

/* ---------------------------------------------------------------------------
 * URLs and redirects
 *
 * With an mp of NULL every string these give back comes from plain malloc,
 * and the test frees it with plain free().
 * ------------------------------------------------------------------------- */

/* True on a thread while it resolves a redirect Location, so a test
 * allocator can fail exactly the allocations of that resolution. */
bool _chttp_in_redirect_resolve_for_tests(void);

/* Parses url as a request does and flattens the internal result into the out
 * parameters; each one may be NULL. */
ccol_retval_t _chttp_parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out, bool *is_unix_out,
    char **unix_socket_path_out);

/* Resolves location against base_url as a redirect hop does, and gives the
 * absolute URL or NULL. */
char *_chttp_resolve_redirect_url_for_tests(const char *base_url,
                                            const char *location);

/* Runs the transport check of a redirect on an already resolved next_url with
 * the caller's allocator, so a test can fail each of its allocations. */
ccol_retval_t _chttp_redirect_transport_check_for_tests(
    ccol_memmgmt_procs_t *mp, const char *base_url, const char *next_url,
    bool prevent_downgrade);

/* Resolves location against base_url, hands the result back through
 * *resolved_out (NULL when it doesn't resolve) and returns whether the
 * redirect transport check accepts it. prevent_downgrade is the
 * prevent_tls_downgrade_on_redirect opt-in of a request. */
bool _chttp_redirect_transport_allowed_for_tests(const char *base_url,
                                                 const char *location,
                                                 bool prevent_downgrade,
                                                 char **resolved_out);

/* ---------------------------------------------------------------------------
 * Deadlines and waits
 * ------------------------------------------------------------------------- */

/* Runs the remaining-milliseconds conversion on a deadline add_sec seconds
 * from now; the distance may be one whose millisecond product overflows a
 * long. */
bool _chttp_deadline_remaining_ms_for_tests(long long add_sec, int *out_ms);

/* The elapsed milliseconds between two instants, rounded down, so a test can
 * feed it instants on both sides of a second boundary. */
long long _chttp_ms_elapsed_floor_for_tests(long long from_sec, long from_nsec,
                                            long long to_sec, long to_nsec);

/* Runs the remaining-milliseconds conversion at a fixed instant, for a
 * deadline ahead_ns nanoseconds after it, so the rounding is checked without
 * racing the clock. */
bool _chttp_deadline_remaining_ms_at_for_tests(long long ahead_ns, int *out_ms);

/* While on, every read point of every tier waits for its fd to be readable
 * and then sees its overall deadline as passed. */
void _chttp_set_expire_at_read_for_tests(bool on);

/* While cap is not zero, the first TLS read of each Tier 2 read dispatch asks
 * for at most cap bytes, which leaves part of a record inside the TLS layer. */
void _chttp_set_async_tls_first_read_cap_for_tests(size_t cap);

/* The next n waits capped at INT_MAX ms return at once with no event, as a
 * capped wait that ran out does. */
void _chttp_short_next_capped_waits_for_tests(int n);

/* ---------------------------------------------------------------------------
 * Name resolution and address lists
 * ------------------------------------------------------------------------- */

/* Adds addr to a set of at most 4 silent addresses: an attempt on one of them
 * gets a socket that never connects and never fails, as a dropped SYN does. A
 * NULL addr empties the set. */
void _chttp_set_silent_addr_for_tests(const struct sockaddr *addr,
                                      socklen_t len);

/* Answers every TCP resolution with these count addresses (at most 8) in this
 * order instead of calling getaddrinfo(); a NULL addrs or a count of 0 clears
 * the override. */
void _chttp_set_addr_override_for_tests(const struct sockaddr *const *addrs,
                                        const socklen_t *lens, size_t count);

/* Builds a list where address i has family families[i], interleaves it by
 * family as a connect race does and writes the resulting order of the
 * original indexes to order_out. */
void _chttp_addr_list_interleave_for_tests(const int *families, size_t n,
                                           size_t *order_out);

/* While ms is not zero, every name lookup sleeps that long first, as a
 * resolver that doesn't answer. Address literals are never affected. */
void _chttp_set_name_lookup_delay_ms_for_tests(unsigned ms);

/* The number of name lookups that all tiers started; address literals never
 * count. */
unsigned long _chttp_name_lookups_for_tests(void);

/* ---------------------------------------------------------------------------
 * TLS
 * ------------------------------------------------------------------------- */

/* Creates the client side of a TLS connection on a fresh socket pair and
 * returns the code a request would report. It is the only way to reach the
 * refusal of an empty host, which no URL can carry. */
ccol_retval_t _chttp_tls_client_create_code_for_tests(const char *host,
                                                      bool verify_host);

/* Runs a full handshake over a fresh socket pair: the client verifies host
 * against ca_path, the server offers default_cert and, under the SNI name
 * named_as, named_cert. Returns true when both sides finish it. */
bool _chttp_tls_client_handshake_for_tests(
    const char *host, const char *ca_path, const char *default_cert,
    const char *default_key, const char *named_as, const char *named_cert,
    const char *named_key);

/* Arms the TLS direction injection. Mode 0 disarms it, mode 1 makes a TLS
 * write need the connection to become readable first and mode 2 makes a TLS
 * read need it to become writable first. Arming clears the counters below,
 * and a test must disarm it before it returns. */
void _chttpclient_tls_dir_inject_for_tests(int mode);

/* True once the module has released the injected block by waiting or
 * registering in the direction the injection asked for. */
bool _chttpclient_tls_dir_released_for_tests(void);

/* How many times the module asked for the read direction while it was
 * writing, and for the write direction while it was reading. */
unsigned _chttpclient_tls_dir_read_interest_while_writing_for_tests(void);
unsigned _chttpclient_tls_dir_write_interest_while_reading_for_tests(void);

/* How many TLS I/O attempts the armed injection has refused. A caller that
 * follows the reported direction pays one; more than that measures a busy
 * spin. */
unsigned _chttpclient_tls_dir_block_attempts_for_tests(void);

/* ---------------------------------------------------------------------------
 * Response fields and the Tier 1 read loop
 * ------------------------------------------------------------------------- */

/* How many record headers of repeated response fields have been read, so a
 * test can check that no operation rescans the block. */
unsigned long _chttpclient_field_rec_reads_for_tests(void);

/* The number of reads and of readability waits in the Tier 1 read loop, so a
 * test can check how the two are ordered. */
unsigned long _chttp_tier1_reads_for_tests(void);
unsigned long _chttp_tier1_read_waits_for_tests(void);

/* The number of Tier 1 hops that have opened a 100-continue window. */
unsigned long _chttp_tier1_continue_waits_for_tests(void);

/* ---------------------------------------------------------------------------
 * Deadline sweep of Tiers 2 and 3
 * ------------------------------------------------------------------------- */

/* How many sweep ticks the process has run. */
unsigned long _chttp_sweep_ticks_for_tests(void);

/* For each kind of timed work (0: an expired deadline, 1: the start of the
 * next Happy Eyeballs attempt), how often a walk acted on it, and the largest
 * gap between the instant the walk was planned for and the instant the work
 * was due. The reset sets the counts to 0 and the gaps to INT64_MIN. */
void _chttp_sweep_due_reset_for_tests(void);
unsigned long _chttp_sweep_due_for_tests(int kind, int64_t *max_late_ns);

/* Arms the next sweep tick to declare the first reading ctx expired and to
 * park on it in phase 1 (after its verdict) and phase 2 (before it shuts the
 * fd down). The test reads the phase it waits in and lets a phase go by
 * releasing its number; each park is bounded. */
void _chttp_sweep_race_arm_for_tests(void);
int _chttp_sweep_race_parked_phase_for_tests(void);
void _chttp_sweep_race_release_for_tests(int phase);
void _chttp_sweep_race_disarm_for_tests(void);

/* How many times the idle-pool offer has entered, and has finished, the
 * critical section that makes a ctx idle. */
unsigned _chttp_offer_entered_count_for_tests(void);
unsigned _chttp_offer_done_count_for_tests(void);

/* Arms the next sweep tick that meets a ctx waiting for 100 Continue to park
 * after its critical section, before it judges the continue window, until
 * the release (10 s at most). A release counts for the arm before it, also
 * when it comes before the sweep parks, and no later arm takes it back. The
 * last one counts the stores of the continue deadline. */
void _chttp_continue_sweep_arm_for_tests(void);
bool _chttp_continue_sweep_parked_for_tests(void);
void _chttp_continue_sweep_release_for_tests(void);
unsigned _chttp_continue_deadline_stores_for_tests(void);

/* Arms the first sweep that meets a connecting ctx with no fd and no race to
 * wait until the connect task has registered its fd and the connect deadline
 * has passed. The second one tells whether it waited, and disarms it. */
void _chttp_sweep_fd_wait_arm_for_tests(void);
bool _chttp_sweep_fd_wait_held_for_tests(void);

/* Writes what the armed hold saw into buf (which ctxs it turned away and
 * why, and how the lookup's wait ended), so a test can print it when the
 * hold didn't happen. */
void _chttp_sweep_fd_wait_trace_for_tests(char *buf, size_t size);

/* How many hops have a future that the sweep already fulfilled while the hop
 * and its ctx still exist. */
size_t _chttp_async_hops_abandoned_for_tests(void);

/* Makes the next hop that turns to reading its response get no dispatch: its
 * registration is paused. The resume returns true when there was a paused
 * registration and it got resumed. */
void _chttp_stall_next_read_for_tests(void);
bool _chttp_stalled_read_resume_for_tests(void);

/* ---------------------------------------------------------------------------
 * Engine
 * ------------------------------------------------------------------------- */

/* The size of the internal async chain struct, so a fault-injecting
 * allocator can fail exactly that allocation. */
size_t _chttp_async_chain_struct_size_for_tests(void);

/* The number of references on the engine, and the logger it uses. */
int _chttpclient_engine_ref_count_for_tests(void);
clog _chttpclient_engine_logger_for_tests(void);

/* Removes and closes the logger installed by chttpcli_set_engine_logger, so
 * the next engine run opens a fallback logger. The engine must not be
 * running. */
void _chttpclient_clear_engine_user_logger_for_tests(void);

/* Whether the engine runs, and direct calls to its acquire, its release and
 * its wait for quiescence. */
bool _chttpclient_engine_running_for_tests(void);
ccol_retval_t _chttpclient_engine_acquire_for_tests(void);
void _chttpclient_engine_release_for_tests(void);
void _chttpclient_engine_wait_for_quiescence_for_tests(void);

/* Whether this process has started initializing the engine globals. */
bool _chttpclient_engine_globals_entered_for_tests(void);

/* The DNS pool of the running engine, or CTPOOL_NULL when there is no
 * engine. */
ctpool _chttpclient_engine_dns_pool_for_tests(void);

/* The number of reactor threads the last engine start resolved. */
size_t _chttpclient_engine_num_reactor_threads_for_tests(void);

/* Makes the next reaper hold the engine mutex for ms milliseconds in its
 * final critical section. The flag stays set from the start of the hold
 * until the next hold is armed. */
void _chttpclient_hold_next_reaper_final_section_for_tests(int ms);
bool _chttpclient_reaper_final_section_was_entered_for_tests(void);

/* Makes the next reaper wait ms milliseconds, holding no lock, after it has
 * closed the fallback logger, and reports when that wait starts and ends. */
void _chttpclient_hold_next_reaper_tail_for_tests(int ms);
bool _chttpclient_reaper_tail_was_entered_for_tests(void);
bool _chttpclient_reaper_tail_was_left_for_tests(void);

/* Waits until no engine teardown is in flight and leaves the reaper that
 * finished it unjoined. */
void _chttpclient_engine_wait_stopped_unjoined_for_tests(void);

/* Copies the handle of the reaper that nobody has joined yet into *out, and
 * returns false when there isn't one. */
bool _chttpclient_engine_unjoined_reaper_for_tests(_ccol_thread_id_t *out);

/* Holds the next reaper between its creation and the publishing of its
 * handle, for ten seconds at most, until the release. It fires once. */
void _chttpclient_hold_next_reaper_spawn_for_tests(void);
bool _chttpclient_reaper_spawn_is_held_for_tests(void);
void _chttpclient_release_reaper_spawn_for_tests(void);

/* How many reaper threads were created and joined; the two match once the
 * engine is quiescent. */
unsigned _chttpclient_reaper_created_count_for_tests(void);
unsigned _chttpclient_reaper_joined_count_for_tests(void);

/* While on, every attempt to create a reaper thread fails, and the counter
 * reports how often the reap was then given up. */
void _chttpclient_force_reaper_spawn_fail_for_tests(bool on);
unsigned _chttpclient_engine_reaper_abandoned_count_for_tests(void);

/* -1 until the shutdown of an abandoned ctx has run, then 1 if the last one
 * ran under the ctx's idle_lock (the only safe order) and 0 if it ran after
 * the unlock. The reset sets it back to -1. */
int _chttpclient_abandon_shutdown_under_lock_for_tests(void);
void _chttpclient_reset_abandon_shutdown_probe_for_tests(void);

/* ---------------------------------------------------------------------------
 * Idle pools
 * ------------------------------------------------------------------------- */

/* Ages every connection in the Tier 2 and Tier 3 idle pool of cli past the
 * staleness limit, so the next take evicts it. */
void _chttpclient_force_async_idle_stale_for_tests(struct chttpclient *cli);

/* Shuts down the fd of every connection in the Tier 2 and Tier 3 idle pool
 * of cli, so the dispatch of each one frees it on a reactor thread. */
void _chttpclient_shutdown_async_idle_connections_for_tests(
    struct chttpclient *cli);

/* The number of connections in the Tier 2 and Tier 3 idle pool of cli, and
 * the number of them that carry a timed_out mark, which should be 0. */
size_t _chttpclient_async_idle_total_count_for_tests(struct chttpclient *cli);
size_t _chttpclient_async_idle_timed_out_count_for_tests(
    struct chttpclient *cli);

/* The number of origins with an entry in the Tier 1 idle pool of cli, and in
 * its Tier 2 and Tier 3 idle pool. */
size_t _chttpclient_idle_pools_key_count_for_tests(struct chttpclient *cli);
size_t _chttpclient_idle_pools_async_key_count_for_tests(
    struct chttpclient *cli);

/* Overrides the limit on idle origins for the whole process; 0 restores the
 * real constant. */
void _chttpclient_set_max_idle_origins_for_tests(size_t n);

/* Makes the next push of a connection onto the idle pool of its origin count
 * as a failure. The flag is consumed by its first use. */
void _chttpclient_force_offer_push_fail_once_for_tests(void);

/* Makes the next plain-HTTP hop that finishes writing its request, or the
 * next connection that lands in the idle pool, turn its own registration
 * back to the write direction, as a late continue-timeout flip of the sweep
 * does. Each flag is consumed by its first use. */
void _chttpclient_force_stray_write_arm_once_for_tests(void);
void _chttpclient_force_stray_idle_write_arm_once_for_tests(void);

/* Holds the IDLE branch of the read dispatch once, between deciding that a
 * ctx with input waiting is idle and reading, so a take can win the race.
 * The wait returns whether the branch got to the hold within timeout_ms; the
 * release ends the hold, or disarms it. */
void _chttpclient_arm_idle_probe_hold_for_tests(void);
bool _chttpclient_wait_idle_probe_held_for_tests(long timeout_ms);
void _chttpclient_release_idle_probe_hold_for_tests(void);

/* Widens the window on both sides of the 100-continue carry-over replay by ms
 * milliseconds each; 0 turns it off. */
void _chttpclient_set_continue_carry_replay_delay_ms_for_tests(long ms);

/* Arms one stray write dispatch: the next take of a pooled connection arms
 * the write direction of its registration while it holds idle_lock, before
 * it builds the new hop, and waits 200 ms. */
void _chttpclient_arm_stray_write_at_take_for_tests(void);

/* Arms one stray write dispatch that races a take: the dispatch classifies
 * the ctx as IDLE, then the take moves it to WRITING and holds idle_lock for
 * 200 ms. The second one tells whether the dispatch was held so, and disarms
 * the hook. */
void _chttpclient_arm_stray_write_before_take_for_tests(void);
bool _chttpclient_stray_write_before_take_held_for_tests(void);

/* How many times the write dispatch has put a stray registration back to the
 * read direction. */
unsigned long long _chttpclient_stray_write_arm_restores_for_tests(void);

/* Makes the next ccol_event_loop_modify that reactivates a reused connection
 * count as a failure. The flag is consumed by its first use. */
void _chttpclient_force_reactivate_fail_once_for_tests(void);

/* Makes the next read dispatch of a ctx that has already read successfully
 * act as a hard read error (ECONNRESET), without touching the socket. */
void _chttpclient_force_async_hard_read_error_once_for_tests(void);

/* Reads back the connect, request and inactivity timeouts of cli, in
 * microseconds. Returns false for a handle that doesn't resolve. */
bool _chttpclient_timeouts_for_tests(chttpcli cli, uint64_t *connect_us,
                                     uint64_t *request_us,
                                     uint64_t *inactivity_us);

/* The primitive initializations of chttpclient_create_mp that
 * _chttpclient_arm_create_fault_for_tests can make fail, in the order the
 * create makes them. */
typedef enum {
  _CHTTPCLI_FAULT_NONE = 0,
  _CHTTPCLI_FAULT_LOCK,
  _CHTTPCLI_FAULT_AVAILABLE_CV,
  _CHTTPCLI_FAULT_IDLE_ASYNC_DRAINED_CV,
  _CHTTPCLI_FAULT_ASYNC_COUNT_LOCK,
  _CHTTPCLI_FAULT_ASYNC_COUNT_DRAINED_CV
} _chttpcli_fault_t;

/* Arms one failure of step: the next create that reaches it sees the init
 * fail, as pthread would report it, and the hook disarms itself.
 * _CHTTPCLI_FAULT_NONE disarms it. The failing init is skipped, so the
 * failure path has nothing of it to destroy, as after a real failure. */
void _chttpclient_arm_create_fault_for_tests(_chttpcli_fault_t step);
#endif /* RUNNING_UNIT_TESTS */
