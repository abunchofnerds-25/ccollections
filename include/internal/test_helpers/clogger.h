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
 * @file test_helpers/clogger.h
 * @brief INTERNAL ONLY. The test-only hooks of clogger.
 *
 * src/clogger.c defines these hooks and the clogger tests call them. They let a
 * test force failures, hold a thread at a chosen point and read state that the
 * public API doesn't show.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <clogger.h>

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Arm (or disarm) an artificial delay inside the internal
 *        gzip-compression step of the log rotation, for a test.
 *
 * While this hook is armed (delay_us != 0), the next rotation's compression
 * step sleeps for delay_us microseconds right after it creates its ".gz"
 * destination file on disk and before it writes anything into it. That
 * widens, deterministically, the window in which a concurrent rotation's
 * deletion pass must not be able to delete that destination. The call also
 * resets what _clog_test_gz_dest_opened() reports. It has no effect outside a
 * RUNNING_UNIT_TESTS build.
 *
 * @param delay_us Microseconds to sleep, or 0 to disarm.
 */
void _clog_test_set_pending_compress_delay_us(unsigned int delay_us);

/**
 * @brief Report whether the compression step created its ".gz" destination file
 *        on disk, for a test.
 *
 * The latest call to _clog_test_set_pending_compress_delay_us() arms that
 * compression step. A test spin-polls this function so it can wait,
 * deterministically, until a concurrent rotation's compressed destination
 * really exists on disk before starting a second rotation that races the
 * first, instead of sleeping for a fixed time and hoping the timing works
 * out.
 *
 * @return true once the destination file has been created for the delay the
 *         latest call armed. A call to
 *         _clog_test_set_pending_compress_delay_us() resets it to false.
 */
bool _clog_test_gz_dest_opened(void);

/**
 * @brief Hold or release every gzip compression of a rotated file, for a test.
 *
 * While the gate is held, each compression pauses right after creating its
 * ".gz" destination until the gate is released. The pause gives up after 30
 * seconds, so a test whose expectation fails doesn't hang. Holding the gate
 * also resets what _clog_test_gz_dest_opened() reports.
 *
 * @param hold true to hold the gate, false to release it.
 */
void _clog_test_hold_compressions(bool hold);

/**
 * @brief Test-only: make the publish step of each of the next `count`
 *        compressions fail, and the removal of its temporary output fail too,
 *        as in a directory that lost its write permission.
 *
 * @param count How many compressions fail that way; 0 turns it off.
 */
void _clog_test_force_gz_publish_failures(unsigned int count);

/**
 * @brief Report how many gzip compressions ran to their end, whatever their
 *        outcome, since the process started, for a test.
 */
size_t _clog_test_compressions_finished(void);

/**
 * @brief Hold or release the recovery of unfinished compressions that the open
 *        of a compressing logger runs, for a test.
 *
 * While the gate is held, each recovery pauses before it takes its logger's
 * mutex, until the gate is released. The pause gives up after 3 seconds, so
 * a recovery that runs where no test can release it doesn't hang. Holding
 * the gate also resets what _clog_test_compression_recovery_entered()
 * reports.
 *
 * @param hold true to hold the gate, false to release it.
 */
void _clog_test_hold_compression_recovery(bool hold);

/**
 * @brief Report whether a recovery reached the gate that
 *        _clog_test_hold_compression_recovery() holds, for a test.
 */
bool _clog_test_compression_recovery_entered(void);

/**
 * @brief Report how many times the timed wait of the async writer thread of
 *        the target behind logger ended with no job, for a test.
 */
size_t _clog_test_writer_timeout_wakeups(clog logger);

/**
 * @brief Test-only: while on is true, a socket sink that a logger (or a
 *        rotation) opens is written the way a system without MSG_NOSIGNAL
 *        writes it, through the pipe path.
 */
void _clog_test_classify_sockets_as_pipes(bool on);

/**
 * @brief Report how many flushes the async writer thread of the target behind
 *        logger started because flush_interval_us was over, for a test.
 */
size_t _clog_test_writer_interval_flushes(clog logger);

/**
 * @brief Reset the counter of real rotation attempts (_rotate() calls) to zero,
 *        for a test.
 *
 * A test uses it to check the retry policy for a rotation that keeps
 * failing: such a rotation has to be retried with a bounded backoff, not on
 * every single write.
 */
void _clog_test_reset_rotate_attempt_count(void);

/**
 * @brief Add secs seconds to the wall clock that rotation reads (the start of
 *        the interval, the retry backoff and the stamp of a rotated name),
 *        for a test. A negative value moves that clock back and 0 restores it.
 */
void _clog_test_set_wall_clock_offset(int64_t secs);

/**
 * @brief Make each of the next count rotation attempts fail at once, before
 *        it touches the directory, for a test. 0 disarms it.
 */
void _clog_test_force_rotate_failures(unsigned int count);

/**
 * @brief Report how many times _rotate() was called since the last call to
 *        _clog_test_reset_rotate_attempt_count(), for a test.
 */
size_t _clog_test_get_rotate_attempt_count(void);

/**
 * @brief Reset to zero the three maxima below, which rotation's deletion
 *        passes record, for a test.
 */
void _clog_test_reset_max_rotated_generations(void);

/**
 * @brief Report the largest number of rotated generations that any deletion
 *        pass has left on disk since the last call to
 *        _clog_test_reset_max_rotated_generations(), for a test.
 */
int _clog_test_get_max_rotated_generations(void);

/**
 * @brief Report the largest number of compressions in flight that any deletion
 *        pass saw since the last reset, for a test. A compression is in flight
 *        from the rotation that queues it until it finishes, whether it is
 *        running or still waiting in the queue.
 */
int _clog_test_get_max_compressions_in_flight(void);

/**
 * @brief Report the largest number of rotated generations that any deletion
 *        pass left on disk beyond the older generations it kept only because
 *        they were still under compression, since the last reset, for a test.
 */
int _clog_test_get_max_generations_beyond_old_compressions(void);

/**
 * @brief Report how many times the calling thread read its process ID,
 *        thread ID and thread name from the system, for a test.
 */
unsigned _clog_test_thread_identity_fill_count(void);

/**
 * @brief Test-only: the identity cache block of the calling thread, or NULL.
 */
const void *_clog_test_thread_identity_block(void);

/**
 * @brief Test-only: clear the thread-local pointer to the calling thread's
 *        identity cache and leave the thread-specific value that owns the
 *        cache, the way macOS does when it releases thread-local storage
 *        before an exiting thread's key destructors run.
 */
void _clog_test_forget_thread_identity_pointer(void);

/**
 * @brief Sanitize `raw` into `out` for use as an RFC 5424 PRINTUSASCII field
 *        (APP-NAME or HOSTNAME), for a test.
 *
 * clog_create_from_fd_mp() and clog_create_from_file_mp() use the same
 * PRINTUSASCII filter internally to cache both APP-NAME and HOSTNAME from the
 * names the OS reports for the process and the host. This function takes the
 * filter apart from those OS-specific sources, so a test can exercise it
 * directly with any input.
 *
 * @param raw   NUL-terminated input string.
 * @param out   Destination buffer of size outsz. It is always NUL-terminated.
 * @param outsz Size of out, in bytes. It must be >= 2.
 */
void _clog_test_sanitize_syslog_appname(const char *raw, char *out,
                                        size_t outsz);

/**
 * @brief Backslash-escape the control characters in `raw` into `out`, for a
 *        test.
 *
 * The same escape logic is used for RFC 5424 MSG content, and for the text of
 * a backtrace frame in the logfmt and syslog formats (see
 * _buf_append_ctrl_escaped() in clogger.c). With this function a test checks
 * the escape logic directly instead of going through a whole log line.
 *
 * @param raw   NUL-terminated input string.
 * @param out   Destination buffer of size outsz. If the escaped result doesn't
 *              fit, the function truncates it. It is always NUL-terminated.
 * @param outsz Size of out, in bytes. It must be >= 1.
 */
void _clog_test_append_ctrl_escaped(const char *raw, char *out, size_t outsz);

/**
 * @brief Force (or stop forcing) every backtrace capture after this call to
 *        report a failure, for a test.
 *
 * While this hook is armed, every log call with with_backtrace=true behaves
 * as if the platform had no backtrace support, or as if backtrace_symbols()
 * itself had failed to allocate, so a test doesn't need to reproduce either
 * real condition: one depends on the platform and the other on a real
 * out-of-memory state. The hook affects every logger in the process; disarm
 * it (force == false) once the test doesn't need it.
 *
 * @param force true to force every future capture to fail. false to start real
 *              backtrace capture again.
 */
void _clog_test_force_backtrace_capture_failure(bool force);

/**
 * @brief Force (or stop forcing) every frame of a real backtrace to fail to
 *        append inside the syslog backtrace emitter, for a test.
 *
 * While this hook is armed, every backtrace frame of a CLOG_FMT_SYSLOG logger
 * is treated as too large to fit, and those frames come from a real captured
 * backtrace that isn't NULL. So a test doesn't need a real allocation failure
 * lasting across every one of those small per-frame appends. The hook
 * affects every logger in the process; disarm it (force == false) once the
 * test doesn't need it.
 *
 * @param force true to force every future frame to fail. false to write the
 *              frames in the normal way again.
 */
void _clog_test_force_all_syslog_backtrace_frames_failure(bool force);

/**
 * @brief Force (or stop forcing) every backtrace capture after this call to
 *        report a real but shallow result, for a test.
 *
 * While this hook is armed, _capture_backtrace() still does a real capture
 * and syms isn't NULL, exactly as for an ordinary successful capture, but the
 * reported depth is clamped to CLOG_BT_INITIAL_FRAME (2). Whatever the real
 * depth of the call stack, it never reports a frame beyond the two internal
 * bookkeeping frames, _capture_backtrace itself and _clog_write. That
 * exercises, deterministically, the path where the capture really succeeds
 * but finds nothing to show, without needing a real call stack shallow
 * enough to get there. The hook affects every logger in the process; disarm
 * it (force == false) once the test doesn't need it.
 *
 * @param force true to force every future capture to report a clamped, shallow
 *              depth. false to report the real depth again.
 */
void _clog_test_force_shallow_backtrace_depth(bool force);

/**
 * @brief Report whether this build of the library captures backtraces.
 *
 * Backtraces are captured where the C library has backtrace(3) (glibc, macOS
 * and the BSDs), and elsewhere, musl for example, the "backtrace unavailable"
 * marker is written. A test of a successful capture skips where this gives
 * false.
 */
bool _clog_test_backtrace_supported(void);

/**
 * @brief Force the next handle acquisition to grow the slot table with a new
 *        slot, and then fail the live_shareds registration step of that slot,
 *        for a test. It also stops that force.
 *
 * clog_slot_table.slots, free_indices and live_shareds are always allocated
 * with the process's plain default allocator, never with one logger's custom
 * mprocs, so a test can't force a real allocation failure at this exact call
 * through a public constructor's mprocs parameter. A test also can't know or
 * control the state of the slot table's free_indices list when it runs,
 * because earlier tests in the same process may have put the slots of closed
 * loggers there for reuse. This hook lets a test exercise, deterministically,
 * the matching rollback path in _clog_handle_acquire(), which
 * clog_create_from_fd_mp(), clog_create_from_file_mp() and clog_derive() all
 * use, without depending on either of those two problems and without the real
 * global allocator having to fail.
 *
 * While the hook is armed, the next call into _clog_handle_acquire() does
 * three things: it grows the slot table with a fresh slot, exactly as if
 * free_indices were empty, it treats that slot's live_shareds registration as
 * failed without really attempting it, and it returns CLOG_INVALID. The hook
 * then disarms itself, so it affects that one call only.
 *
 * @param force true to force the next acquisition to take this path. false to
 *              disarm without a wait for it to fire.
 */
void _clog_test_force_next_fresh_slot_registration_failure(bool force);

/**
 * @brief Widen the window inside clog_close() between its step 3 and its step
 *        4, for a test.
 *
 * Step 3 waits for any concurrent resolver holding a pin to finish, and step
 * 4 retires the slot and frees the reference to the shared target. Inside
 * this window a slot has in_use == false but freed == false, and some OTHER
 * thread can call fork(); the child-side handling of "closing" slots in
 * _clog_atfork_release() is there to make that safe (see
 * clog_atfork_closing_t in clogger.c). The window is normally only a few
 * instructions, far too narrow for a test to land a real fork() inside it
 * deterministically, so this hook lets a test widen it on demand instead of
 * relying on scheduling luck.
 *
 * While the hook is armed (delay_us != 0), every clog_close() call sleeps for
 * delay_us microseconds right after step 3 finishes and before step 4
 * starts. That affects every clog_close() call in the process until you
 * disarm it (delay_us == 0). Unlike most other hooks in this section it
 * doesn't disarm itself, because a test needs it armed across the one
 * clog_close() call under test, which a background thread makes while the
 * main test thread arranges the fork() into the widened window.
 *
 * @param delay_us Microseconds to sleep after step 3, or 0 to disarm.
 */
void _clog_test_set_close_finalize_delay_us(unsigned int delay_us);

/**
 * @brief Test-only: widen the window in which a close has cleared its slot but
 *        still needs the slot table.
 *
 * Partway through, a clog_close() stops being visible as a live slot, and
 * then takes the table's write lock again to finish. While this hook is
 * armed (delay_us != 0), every clog_close() sleeps for delay_us microseconds
 * inside exactly that span, so a test can let a second close finish in the
 * middle of it on demand instead of relying on scheduling luck. The hook
 * doesn't disarm itself, for the same reason as the hook above.
 *
 * @param delay_us Microseconds to sleep inside that window, or 0 to disarm.
 */
void _clog_test_set_close_release_window_us(unsigned int delay_us);

/**
 * @brief Test-only: report whether any clog_close() entered that window since
 *        the last call that armed it.
 *
 * A test can then wait until a call really sits in the window instead of
 * sleeping for a guessed interval, so a thread that starts slowly can't leave
 * the test with a pass that exercised nothing.
 *
 * @return true after a clog_close() enters the armed window.
 */
bool _clog_test_close_release_window_entered(void);

/**
 * @brief Report whether some clog_close() call entered the armed delay, for a
 *        test.
 *
 * The latest call to _clog_test_set_close_finalize_delay_us() arms that
 * delay. A test spin-polls this function, as with
 * _clog_test_gz_dest_opened(), so it can wait deterministically until a
 * concurrent clog_close() call really enters its widened step-3 to step-4
 * window before forking into it, instead of sleeping for a fixed time and
 * hoping the timing works out. A call to
 * _clog_test_set_close_finalize_delay_us() resets this to false.
 *
 * @return true after some clog_close() call enters the currently armed delay. A
 *         call to _clog_test_set_close_finalize_delay_us() resets it to false.
 */
bool _clog_test_close_finalize_delay_entered(void);

/**
 * @brief Report how many different shared logging targets are live in this
 *        process right now, for a test.
 *
 * A shared logging target is a root logger, in the sense of the words "shares
 * the parent's target" in clog_derive(). The count is simply the element
 * count of clog_slot_table.live_shareds; the doc comment of that field in
 * clogger.c gives the exact meaning of "live" here. It covers a target's
 * whole lifetime, from the acquisition of its first handle until the target
 * is freed, and doesn't depend on the state of any one handle. It's safe to
 * call inside a child that a fork just made, because it reads this process's
 * copy-on-write copy of the table.
 *
 * @return The current count of live shared targets.
 */
size_t _clog_test_live_shareds_count(void);

/**
 * @brief Call the internal gzip-compression helper of the log rotation
 *        directly, for a test.
 *
 * _rotate() calls this same helper to compress a rotated file. With this
 * function a test checks the cleanup on the failure path directly. A failure
 * can happen before the destination file is created, when src can't be
 * opened for example, and such a failure must leave whatever is already on
 * disk at dst completely untouched instead of deleting it unconditionally.
 *
 * @param src Path of the uncompressed source file.
 * @param dst Destination path. By convention this is src plus ".gz".
 * @return true on success, and then dst is a valid gzip file and src is
 *         unlinked. On a failure it gives false, and removes dst only if this
 *         call itself created dst. src is left untouched in both cases.
 */
bool _clog_test_gzip_compress_file(const char *src, const char *dst);

/**
 * @brief Test-only: _clog_test_gzip_compress_file() for a log whose live file
 *        live_uid owns.
 *
 * A rotation compresses a generation owned by the effective user or by the
 * live file's owner. This hook supplies that owner the way a rotation does,
 * so a test running as root can give the source to a different user and
 * still see it compressed.
 *
 * @param src Path of the uncompressed source file.
 * @param dst Destination path.
 * @param live_uid The owner of the live file of the log.
 * @return The same as _clog_test_gzip_compress_file().
 */
bool _clog_test_gzip_compress_file_for_owner(const char *src, const char *dst,
                                             uid_t live_uid);

/**
 * @brief Test-only: call hook inside the window of a rotation in which the live
 *        name names neither the old file nor the new one.
 *
 * The rotation calls hook(dir_fd, base) after moving the old file to its
 * rotated name and before the new live file takes the live name. dir_fd is
 * the log's directory and base is the last component of its path. NULL
 * disarms it. It has no effect outside a RUNNING_UNIT_TESTS build.
 */
void _clog_test_set_rotate_window_hook(void (*hook)(int dir_fd,
                                                    const char *base));

/**
 * @brief Test-only: make the next fsync() of a compressed output report err.
 *
 * The next compression then sees err from the call that brings its output to
 * the disk, instead of what the real call answers. The hook disarms itself
 * when it fires, and 0 disarms it.
 *
 * @param err The errno to report, or 0.
 */
void _clog_test_force_next_fsync_error(int err);

/**
 * @brief Test-only: make the writer thread of every async logger hold the mutex
 *        of its target for delay_us microseconds on each record.
 *
 * The writer thread sleeps that long after taking the mutex for a record and
 * before writing it. A test uses this to keep the writer thread busy under
 * the lock while another thread waits for that lock. 0 turns it off.
 *
 * @param delay_us The delay in microseconds, or 0.
 */
void _clog_test_set_writer_job_delay_us(unsigned int delay_us);

/**
 * @brief Test-only: make the writer thread of every async logger wait
 *        delay_us microseconds on each record before it takes the mutex of
 *        its target.
 *
 * The writer thread sleeps that long after taking a record off the queue,
 * without the mutex, so another thread can take the mutex while that record
 * and the ones behind it are still unwritten. 0 turns it off.
 *
 * @param delay_us The delay in microseconds, or 0.
 */
void _clog_test_set_writer_unlocked_job_delay_us(unsigned int delay_us);

/**
 * @brief Test-only: set the time budget of the drain that runs at process exit,
 *        in milliseconds.
 *
 * Within this budget the drain delivers what async loggers still hold and
 * finishes the queued compressions. A test sets a short budget to see what
 * the drain does when the budget runs out. 0 restores the default.
 *
 * @param budget_ms The budget in milliseconds, or 0 for the default.
 */
void _clog_test_set_exit_drain_budget_ms(unsigned int budget_ms);

/**
 * @brief Test-only: make the flush request the exit drain sends to a bounded
 *        queue wait for 0 microseconds, as it does once the drain's budget is
 *        spent.
 *
 * @param on true to force the send without a wait, false to restore it.
 */
void _clog_test_force_exit_drain_send_without_wait(bool on);

/**
 * @brief Test-only: run the drain that runs at process exit, right now.
 *
 * A test calls it in a forked child, which then ends with _exit().
 */
void _clog_test_run_exit_drain(void);

/**
 * @brief Test-only: whether the exit drain left the target of h alone
 *        because its writer thread didn't answer in time.
 *
 * @param h A logger handle.
 * @return true when a drain of this process left the target of h.
 */
bool _clog_test_exit_drain_left_target(clog h);

/**
 * @brief Test-only: lock the mutex of the target of h, and keep it locked.
 *
 * The calling thread takes the mutex that every write, rotation and
 * compression step of the target takes, and returns holding it; nothing ever
 * unlocks it. A test calls this from a thread that then never returns, to
 * show that the drain at process exit gives up on that target within its
 * budget.
 *
 * @param h An open logger handle. An invalid handle does nothing.
 */
void _clog_test_lock_target_forever(clog h);

/**
 * @brief Test-only: take the write lock of the table of loggers, and keep it.
 *
 * Like _clog_test_lock_target_forever(), but for the lock the drain at
 * process exit takes before it visits any target.
 */
void _clog_test_lock_table_forever(void);

/**
 * @brief Force (or stop forcing) the next mutex initialization that
 *        clog_flush() drives to fail, for a test.
 *
 * clog_flush() builds a fresh synchronization object on the stack for each
 * call, and so does the pre-flush step on _clog_write()'s FATAL path; both
 * initialize it with _ccol_mutex_init(). With default or NULL attributes that
 * call has no real failure path a test could trigger portably, and unlike an
 * ordinary heap allocation it doesn't go through a logger's custom
 * allocator. This hook lets a test exercise the matching path
 * deterministically, where the library fails safely instead of using a mutex
 * that isn't fully initialized.
 *
 * While the hook is armed, the very next such initialization reports a
 * failure without calling the underlying primitive at all. The hook then
 * disarms itself, so it affects that one call only.
 *
 * @param force true to force the next initialization to fail. false to disarm
 *              without a wait for it to fire.
 */
void _clog_test_force_flush_mutex_init_failure(bool force);

/**
 * @brief Force (or stop forcing) the next condition variable initialization
 *        that clog_flush() drives to fail, for a test.
 *
 * The condition variable counterpart of
 * _clog_test_force_flush_mutex_init_failure(). It exercises the path where
 * the mutex half of the synchronization object clog_flush() builds per call
 * initializes fine but the condition variable doesn't; the library then has
 * to free the mutex it already initialized instead of leaking it.
 *
 * @param force true to force the next initialization to fail. false to disarm
 *              without a wait for it to fire.
 */
void _clog_test_force_flush_condvar_init_failure(bool force);

/**
 * @brief Call the internal last-resort record writer directly, for a test.
 *
 * That writer is used when not even a small fallback placeholder fits in the
 * target buffer: _clog_write() falls back to this same helper in one case
 * only, when _clog_build_record() can't fit its own small, fixed-size
 * fallback placeholder into the write buffer. The library's buffer-size
 * constants make that condition unreachable through the ordinary path of a
 * logger and a log_* call; see the doc comment on that function in
 * clogger.c. This hook lets a test drive the helper directly and
 * deterministically, and in particular check whether the helper still
 * signals a backtrace the caller asked for but the helper left out.
 *
 * @param logger         An open logger handle. This writes to the underlying fd
 *                       of that handle exactly as a real log_* call does, and
 *                       it obeys the byte accounting of the log rotation.
 * @param fmt            The output format for the placeholder record.
 * @param level          The severity level to render.
 * @param with_backtrace Whether to also signal that the caller asked for a
 *                       backtrace that the library could not include.
 */
void _clog_test_write_unrepresentable_record(clog logger, clog_format_t fmt,
                                             clog_level_t level,
                                             bool with_backtrace);

/**
 * @brief Force (or stop forcing) the next append to the internal write buffer
 *        to report a real allocation failure, for a test.
 *
 * The internal append helpers in clogger.c can really fail only when the
 * buffer needs to grow, and the library's buffer-size constants make that
 * growth impossible for a few deliberately small, fixed-size records, such as
 * the "backtrace unavailable" marker _emit_backtrace_syslog_lines() builds
 * for CLOG_FMT_SYSLOG. This hook lets a test force that one append to fail
 * without the buffer having to grow.
 *
 * While the hook is armed, the very next internal buffer append reports a
 * failure without touching the underlying allocator at all. The hook then
 * disarms itself, so it affects that one call only.
 *
 * @param force true to force the next check to fail. false to disarm without a
 *              wait for it to fire.
 */
void _clog_test_force_next_buf_ensure_failure(bool force);

/**
 * @brief Make each of the next `write_count` internal writes deliver only its
 *        first `accepted_bytes` bytes, for a test.
 *
 * It reproduces a short write, where the kernel takes only the first part of
 * a record, as a filesystem with no free space or a process-wide
 * RLIMIT_FSIZE ceiling makes it do, without putting the whole test process
 * under a real resource limit. It differs from a real limit in one way: the
 * writes after the last capped one still work, so a test can see how the
 * logger restores the record framing afterwards. A count above one
 * reproduces a descriptor that can't take the rest of a record across more
 * than one delivery attempt.
 *
 * Each capped write uses up one of the count, so the hook affects only the
 * writes a test targets. Give 0 bytes to disarm without waiting for the rest
 * of the count to fire.
 *
 * @param accepted_bytes Number of bytes that each capped write can deliver.
 * @param write_count    How many writes in a row the library caps.
 */
void _clog_test_force_short_writes(size_t accepted_bytes,
                                   unsigned int write_count);

/**
 * @brief Make each of the next write_count writes of the library fail with
 *        errno err, before it delivers a byte.
 *
 * It reproduces a write error of any class on a descriptor that otherwise
 * works: one a later attempt can outlast, such as ENOSPC, or one no retry
 * outlasts, such as EPIPE. The writes after the last forced failure work
 * again, so a test can see what the logger wrote about the bytes that didn't
 * go out. Give 0 as err to disarm.
 *
 * @param err         The errno of each forced failure.
 * @param write_count How many writes in a row fail.
 */
void _clog_test_force_write_errors(int err, unsigned int write_count);

/**
 * @brief Test-only: give the output of every logger `room` bytes of room,
 *        after which every write fails with errno err, as on a full disk.
 *
 * Writes take bytes until the room is used up, the write that needs more
 * takes what is left and reports err, and every later write takes nothing
 * and reports err. A write that gets less than it asks for counts as a
 * refusal. After `refusals` refusals the output has room again; with 0 it
 * stays full until the next call. Give 0 as err to turn it off.
 *
 * @param err      The errno of a write that finds no room, or 0.
 * @param room     The bytes that the outputs take before they are full.
 * @param refusals The refusals after which the outputs have room again, or 0.
 */
void _clog_test_set_output_room(int err, size_t room, unsigned int refusals);

/**
 * @brief Test-only: run the part of the module destructor that releases the
 *        calling thread's identity cache and deletes its key, as the end of
 *        a process does. Only a child process that ends soon after may call
 *        it.
 */
void _clog_test_run_identity_fini(void);

/**
 * @brief Call the "backtrace capture failed" marker branch of
 *        _emit_backtrace_syslog_lines() directly against the shared target of
 *        logger, for a test.
 *
 * Reaching this branch through the ordinary log_* call path means forcing the
 * backtrace capture to fail (see _clog_test_force_backtrace_capture_failure()).
 * This hook goes one step further and lets a test exercise that branch on its
 * own, so it can deterministically exercise the branch's internal fallback,
 * which _clog_test_force_next_buf_ensure_failure() arms, without some other
 * unrelated buffer append racing to use up that one-shot hook first.
 *
 * @param logger An open logger handle. This writes to the underlying fd of that
 *              handle exactly as a real log_* call does, and it obeys the byte
 *              accounting of the log rotation.
 * @param level  The severity level to render.
 */
void _clog_test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                         clog_level_t level);
#endif /* RUNNING_UNIT_TESTS */
