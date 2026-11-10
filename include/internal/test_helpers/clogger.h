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

// Internal: the test hooks of clogger. src/clogger.c defines them and the
// clogger tests call them to force failures, hold a thread at a chosen point
// and read state the public API doesn't show. They exist only when
// RUNNING_UNIT_TESTS is defined, so nothing here is installed or exported.
#include <clogger.h>

#ifdef RUNNING_UNIT_TESTS

// Makes the next compression sleep delay_us microseconds right after it creates
// its .gz output, so a test can race a deletion pass inside that window; 0
// disarms it. It also resets clog__test_gz_dest_opened().
void clog__test_set_pending_compress_delay_us(unsigned int delay_us);

// While miss is true, every deletion pass leaves the running compression's two
// names out of its listing, as readdir() may when the compressor renames them
// during the scan, so a test can check that the pass still counts that
// generation.
void clog__test_set_prune_misses_running(bool miss);

// Tells whether the compression armed by the last delay call has created its
// output, so a test can wait for it instead of sleeping.
bool clog__test_gz_dest_opened(void);

// Holds (true) or releases every compression right after it creates its output,
// so a test controls when compressions finish. A held compression gives up
// after 30 seconds. Holding resets clog__test_gz_dest_opened() and
// clog__test_compressor_teardown_entered().
void clog__test_hold_compressions(bool hold);

// Tells whether a close is waiting for a running compressor thread to stop, so
// a test that holds a compression knows when to release it.
bool clog__test_compressor_teardown_entered(void);

// Gives how many times an open backed off from a target of the same file whose
// last close is still running, so a test can tell that the open waits.
size_t clog__test_closing_target_waits(void);

// Makes the publish of the next count compressions fail, and the removal of
// their temporary output too, as in a directory that lost its write permission,
// so a test can check the later cleanup; 0 turns it off.
void clog__test_force_gz_publish_failures(unsigned int count);

// Gives how many compressions have ended, whatever the outcome, so a test can
// wait for one.
size_t clog__test_compressions_finished(void);

// Holds (true) or releases the compression recovery an open runs, before it
// takes the logger's mutex, so a test can race other opens against it. A held
// recovery gives up after 3 seconds. Holding resets
// clog__test_compression_recovery_entered().
void clog__test_hold_compression_recovery(bool hold);

// Tells whether a recovery has reached the held gate.
bool clog__test_compression_recovery_entered(void);

// Gives how many timed waits of the writer thread of logger's target ended with
// no job, so a test can check that an idle writer doesn't wake; 0 for a handle
// that doesn't resolve.
size_t clog__test_writer_timeout_wakeups(clog logger);

// Reports in *pipe_blocked and *xfsz_blocked whether the writer thread of
// logger's target had SIGPIPE and SIGXFSZ blocked at its last mask sync, so a
// test can check the signal policy. False for a handle that doesn't resolve or
// a writer that hasn't synced yet.
bool clog__test_writer_blocked_signals(clog logger, bool *pipe_blocked,
                                       bool *xfsz_blocked);

// While on is true, a socket sink opened after the call is written through the
// pipe path, as on a system without MSG_NOSIGNAL, so a test can run that path
// on Linux.
void clog__test_classify_sockets_as_pipes(bool on);

// Gives how many flushes of the writer thread of logger's target were started
// by flush_interval_us running out, so a test can check the interval under
// load.
size_t clog__test_writer_interval_flushes(clog logger);

// Gives how many synchronous records of logger's target wait for its mutex
// right now (counted after the unlocked level check), so a test can line
// threads up behind the lock.
size_t clog__test_sync_lock_waiters(clog logger);

// Gives how many synchronous records of logger's target got its mutex and
// passed the locked level check, so a test can check which records a level
// change still reaches.
size_t clog__test_sync_records_locked(clog logger);

// Resets the count of rotate() attempts, so a test can check that a failing
// rotation is retried with a backoff and not on every write.
void clog__test_reset_rotate_attempt_count(void);

// Adds secs seconds to the wall clock rotation reads (interval start, retry
// backoff, rotated name stamps), so a test can step the clock forward or back;
// 0 restores it.
void clog__test_set_wall_clock_offset(int64_t secs);

// Makes the next count rotation attempts fail before they touch the directory,
// so a test can check the retry backoff; 0 disarms it.
void clog__test_force_rotate_failures(unsigned int count);

// Gives how many times rotate() ran since the last reset.
size_t clog__test_get_rotate_attempt_count(void);

// Resets the three maxima below, which the deletion passes record, before a
// retention test.
void clog__test_reset_max_rotated_generations(void);

// Gives the most generations any deletion pass left on disk since the reset, so
// a test can check the retention bound.
int clog__test_get_max_rotated_generations(void);

// Gives the most compressions in flight (running or queued) any deletion pass
// saw since the reset.
int clog__test_get_max_compressions_in_flight(void);

// Gives the most generations any deletion pass left beyond the old ones it kept
// only because they were being compressed, since the reset.
int clog__test_get_max_generations_beyond_old_compressions(void);

// Gives how many times the calling thread read its pid, tid and name from the
// system, so a test can check that the identity is cached.
unsigned clog__test_thread_identity_fill_count(void);

// Gives the calling thread's identity cache block, or NULL, so a test can check
// that a block is reused and not leaked.
const void *clog__test_thread_identity_block(void);

// Clears the calling thread's pointer to its identity cache and leaves the key
// value that owns it, as macOS does before an exiting thread's key destructors
// run, so a test can run that path anywhere.
void clog__test_forget_thread_identity_pointer(void);

// Filters raw into out (outsz >= 2, always NUL-terminated) with the
// PRINTUSASCII filter of the syslog APP-NAME and HOSTNAME, so a test can check
// it with any input.
void clog__test_sanitize_syslog_appname(const char *raw, char *out,
                                        size_t outsz);

// Escapes the control characters of raw into out (outsz >= 1, cut to fit,
// always NUL-terminated) with the escaper of syslog MSG and backtrace frames,
// so a test can check it without a whole log line.
void clog__test_append_ctrl_escaped(const char *raw, char *out, size_t outsz);

// While force is true, every backtrace capture of the process fails, as without
// backtrace support or when backtrace_symbols() runs out of memory, so a test
// can check the missing-backtrace markers.
void clog__test_force_backtrace_capture_failure(bool force);

// While force is true, every frame of a real backtrace fails to append in the
// syslog emitter, so a test can reach the marker for frames that all failed.
void clog__test_force_all_syslog_backtrace_frames_failure(bool force);

// While force is true, every capture is real but reports no frame past the
// first two (capture_backtrace and clog__write), so a test can check that a
// successful empty capture writes no marker.
void clog__test_force_shallow_backtrace_depth(bool force);

// Tells whether this build captures backtraces (glibc, macOS and the BSDs; not
// musl), so a test of a successful capture can skip elsewhere.
bool clog__test_backtrace_supported(void);

// Makes the next handle acquisition take a fresh slot and fail its live_shareds
// registration, then return CLOG_NULL; it disarms itself after that one call,
// and false disarms it early. The table's vectors use the default allocator and
// earlier tests leave free slots behind, so this is the only way a test can
// reach that rollback.
void clog__test_force_next_fresh_slot_registration_failure(bool force);

// Holds (true) every clog_close() between its step 3 (pins drained) and its
// step 4 (slot retired), where a slot has in_use false and freed false, so a
// test can fork inside that window. It doesn't disarm itself; a held close
// gives up after 30 seconds. Holding resets
// clog__test_close_finalize_entered().
void clog__test_hold_close_finalize(bool hold);

// Makes every clog_close() sleep delay_us microseconds after it cleared its
// slot and before it's done with the table, so a test can finish a second close
// in that window; 0 disarms it.
void clog__test_set_close_release_window_us(unsigned int delay_us);

// Tells whether a close has entered that window since the hook was armed, so a
// test waits for it instead of guessing.
bool clog__test_close_release_window_entered(void);

// Tells whether a close reached the held gate since it was last held, so a test
// forks only once a close waits there.
bool clog__test_close_finalize_entered(void);

// Gives how many closes found their handle pinned and started to wait, so a
// test knows a close waits before it releases the call that holds the pin.
size_t clog__test_close_pin_waits(void);

// Starts counting the waits for room on the output behind fd (by dev/ino); a
// negative fd stops it.
void clog__test_watch_writable(int fd);

// Gives how many writes to the watched output met a full output and started to
// wait, so a test makes room only once its record waits.
size_t clog__test_writable_waits(void);

// Gives how many shared targets are live in this process (from their first
// handle until they're freed), so a test can check that every rollback
// unregisters its target. It's safe in a fork child.
size_t clog__test_live_shareds_count(void);

// Runs the rotation's gzip helper directly on src and dst (src plus ".gz"), so
// a test can check its cleanup. True on success, with dst a valid gzip file and
// src unlinked. On failure src is untouched and dst is removed only when this
// call created it.
bool clog__test_gzip_compress_file(const char *src, const char *dst);

// clog__test_gzip_compress_file() for a log whose live file live_uid owns, so a
// test running as root can give the source to another user and still see it
// compressed.
bool clog__test_gzip_compress_file_for_owner(const char *src, const char *dst,
                                             uid_t live_uid);

// Makes rotate() call hook(dir_fd, base) while the live name names neither
// file, so a test can plant an entry there; NULL disarms it.
void clog__test_set_rotate_window_hook(void (*hook)(int dir_fd,
                                                    const char *base));

// Makes the next fsync() of a compressed output report err, so a test can check
// which errors fail a compression; it disarms itself, and 0 disarms it.
void clog__test_force_next_fsync_error(int err);

// Makes every writer thread sleep delay_us microseconds under its target's
// mutex before writing each record, so a test can keep the mutex busy; 0 turns
// it off.
void clog__test_set_writer_job_delay_us(unsigned int delay_us);

// Makes every writer thread sleep delay_us microseconds after taking a record
// and before taking the mutex, so a test can log in that window; 0 turns it
// off.
void clog__test_set_writer_unlocked_job_delay_us(unsigned int delay_us);

// Sets the exit drain's budget in milliseconds, so a test can see what happens
// when it runs out; 0 restores the default.
void clog__test_set_exit_drain_budget_ms(unsigned int budget_ms);

// While on is true, the exit drain's send to a bounded queue doesn't wait, as
// when its budget is spent.
void clog__test_force_exit_drain_send_without_wait(bool on);

// Runs the exit drain now, in a forked child that then ends with _exit().
void clog__test_run_exit_drain(void);

// Tells whether a drain left h's target because its writer thread didn't answer
// in time.
bool clog__test_exit_drain_left_target(clog h);

// Locks the mutex of h's target and never unlocks it, from a thread that never
// returns, so a test can show that the exit drain gives up on it; an invalid
// handle does nothing.
void clog__test_lock_target_forever(clog h);

// Takes the write lock of the table of loggers and keeps it, so a test can show
// that the exit drain gives up on it.
void clog__test_lock_table_forever(void);

// Makes the next mutex init of a flush request (clog_flush() and the fatal
// path) fail, which glibc gives no way to provoke, so a test can check that the
// flush gives up safely; it disarms itself after one call.
void clog__test_force_flush_mutex_init_failure(bool force);

// The same for the condition variable, so a test can check that the mutex
// already initialized is destroyed.
void clog__test_force_flush_condvar_init_failure(bool force);

// Runs the last-resort record writer against logger's fd (with rotation
// accounting) at level in fmt, so a test can check it; the buffer sizes keep
// the ordinary path from reaching it. with_backtrace checks that a missing
// backtrace is still signalled.
void clog__test_write_unrepresentable_record(clog logger, clog_format fmt,
                                             clog_level level,
                                             bool with_backtrace);

// Makes the next internal buffer append report an allocation failure even when
// it needs no growth, so a test can reach the fallbacks of small fixed records;
// it disarms itself after one call.
void clog__test_force_next_buf_ensure_failure(bool force);

// Makes each of the next write_count writes deliver only accepted_bytes, as a
// full disk or RLIMIT_FSIZE does, but later writes work, so a test can watch
// the framing get repaired; 0 bytes disarms it.
void clog__test_force_short_writes(size_t accepted_bytes,
                                   unsigned int write_count);

// Makes each of the next write_count writes fail with err before writing, so a
// test can check how each error class (ENOSPC, EPIPE, ...) is handled; err 0
// disarms it.
void clog__test_force_write_errors(int err, unsigned int write_count);

// Gives every output room bytes, after which writes fail with err as on a full
// disk, until refusals writes were refused (0 keeps it full), so a test can
// reproduce a full disk that recovers; err 0 turns it off.
void clog__test_set_output_room(int err, size_t room, unsigned int refusals);

// Runs the identity part of the module destructor (frees the calling thread's
// cache and deletes the key), in a child that ends soon after.
void clog__test_run_identity_fini(void);

// Runs the failed-capture branch of the syslog backtrace emitter against
// logger's fd at level, so a test can reach its last-resort fallback with
// clog__test_force_next_buf_ensure_failure() and nothing else using that hook
// first.
void clog__test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                         clog_level level);
#endif
