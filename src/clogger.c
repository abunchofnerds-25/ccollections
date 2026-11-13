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

// glibc declares memrchr(), renameat2() and the thread-name calls only under
// this.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <chashmap.h>
#include <clogger.h>
#include <common.h>
#include <cthreadcomm.h>
#include <ctype.h>
#include <cvector.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/catfork.h>
#include <internal/cgrowbuf.h>
#include <internal/chashinsert.h>
#include <internal/clogderive.h>
#include <internal/cpintable.h>
#include <internal/cqueueowner.h>
#include <internal/test_helpers/clogger.h>
#include <internal/test_helpers/common.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#endif
#include <internal/crandom.h>
#include <internal/csock.h>
#include <internal/ctlsmodel.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

// musl has no backtrace(), so a record there carries the "backtrace
// unavailable" marker instead of frames.
#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__)
#include <execinfo.h>
#define CLOG_HAS_BACKTRACE 1
#else
#define CLOG_HAS_BACKTRACE 0
#endif

// The first size of a record buffer, and the most one record may grow to.
#define CLOG_BUF_INITIAL 4096U
#define CLOG_BUF_LIMIT (16U * 1024U * 1024U)

// How many frames a backtrace captures at most.
#define CLOG_BACKTRACE_DEPTH 64

// A rotated name is "<base>.YYYYMMDDHHMMSS", optionally followed by "_" and a
// collision sequence number of at least 4 digits and at most
// CLOG_ROTATION_SEQ_MAX_DIGITS. CLOG_ROTATION_EXTRA is the room that suffix
// needs.
#define CLOG_ROTATION_FMT ".%Y%m%d%H%M%S"
#define CLOG_ROTATION_FMT_LEN 15
#define CLOG_ROTATION_SEQ_MAX_DIGITS 9
#define CLOG_ROTATION_SEQ_LIMIT 999999999UL
#define CLOG_ROTATION_EXTRA (1 + CLOG_ROTATION_SEQ_MAX_DIGITS)
#define CLOG_ROTATION_STAMP_DIGITS 14

// A rotated-looking name whose stamp is more than this above both the clock and
// the newest name the target made isn't one of its generations (see
// clog_gen_filter_init()). Real clock steps are far smaller than 30 days.
#define CLOG_ROTATION_FUTURE_TOLERANCE_SECS ((int64_t)30 * 24 * 60 * 60)

// A rotation creates the next live file under "<base>.tmp" plus 11 hex digits
// before it renames it onto the live name. The name is as long as a rotated
// name, and it never parses as one, because there a digit follows "<base>.".
#define CLOG_ROTATION_TMP_TAG ".tmp"
#define CLOG_ROTATION_TMP_HEX_DIGITS 11
#define CLOG_ROTATION_TMP_LEN \
  (sizeof(CLOG_ROTATION_TMP_TAG) - 1 + CLOG_ROTATION_TMP_HEX_DIGITS)
_Static_assert(CLOG_ROTATION_TMP_LEN == CLOG_ROTATION_FMT_LEN,
               "the private name of a new live file must be no longer than a "
               "rotated name");

// At most this many WARN records about entries that look like generations but
// aren't go out per target in each window of this many seconds.
#define CLOG_IGNORED_NOTE_BURST 8U
#define CLOG_IGNORED_NOTE_WINDOW_SECS ((time_t)60)

// After a failed rotation, later attempts wait this long instead of paying a
// rename() and an open() on every write while the failure lasts.
#define CLOG_ROTATE_RETRY_BACKOFF_SECS ((time_t)1)

#ifdef RUNNING_UNIT_TESTS

// Offset that clog__test_set_wall_clock_offset() adds to the wall clock of a
// test build.
static _Atomic int64_t clog_test_wall_offset = 0;

// Sets the offset, in seconds, that a test build adds to rotation's wall clock,
// so a test can step the clock forward and back.
void clog__test_set_wall_clock_offset(int64_t secs) {
  atomic_store(&clog_test_wall_offset, secs);
}

// Gives the wall clock that rotation reads (the stamp of a rotated name, the
// start of a time interval, the retry backoff). It's a function of its own so a
// test build can add an offset and step the clock without touching the system
// clock.
static time_t clog_wall_now(void) {
  return (time_t)((int64_t)time(NULL) + atomic_load(&clog_test_wall_offset));
}

// How many of the next rotate() calls fail before they touch the directory.
static _Atomic unsigned int clog_test_rotate_failures = 0;

// Makes the next count rotate() calls fail before they touch the directory, so
// a test can check the retry backoff.
void clog__test_force_rotate_failures(unsigned int count) {
  atomic_store(&clog_test_rotate_failures, count);
}

// Takes one forced rotation failure, for rotate() in a test build. True when
// one was left.
static bool clog_test_consume_rotate_failure(void) {
  unsigned int n = atomic_load(&clog_test_rotate_failures);
  while (n > 0) {
    if (atomic_compare_exchange_weak(&clog_test_rotate_failures, &n, n - 1))
      return true;
  }
  return false;
}
#else

// Stubs of a non-test build, so rotate() and the rotation checks need no
// #ifdef: The clock is time(NULL) and no rotation failure is forced.
static inline time_t clog_wall_now(void) { return time(NULL); }
static inline bool clog_test_consume_rotate_failure(void) { return false; }
#endif

// Level names, indexed by clog_level. The assert ties the size to the enum, so
// a new level that isn't added here fails to compile.
static const char *const LEVEL_STR[] = {"TRACE", "DEBUG", "INFO",  "WARN",
                                        "ERROR", "ALERT", "FATAL", "OFF"};
_Static_assert(sizeof(LEVEL_STR) / sizeof(LEVEL_STR[0]) == CLOG_OFF + 1,
               "LEVEL_STR must have exactly one entry per clog_level value");

// Keys clog_set_field() refuses: The fixed keys every record carries. A user
// field with one of them would give a duplicate key in JSON and logfmt, and a
// duplicate SD-PARAM-NAME in syslog, which RFC 5424 forbids.
static const char *const RESERVED_FIELD_KEYS[] = {
    "ts", "level", "proc", "src", "func", "msg", "bt", "bt_error"};
#define RESERVED_FIELD_KEYS_COUNT \
  (sizeof(RESERVED_FIELD_KEYS) / sizeof(RESERVED_FIELD_KEYS[0]))

// Tells whether key is one of RESERVED_FIELD_KEYS, so clog_set_field() can
// refuse a key every record already carries.
static bool is_reserved_field_key(const char *key) {
  for (size_t i = 0; i < RESERVED_FIELD_KEYS_COUNT; i++)
    if (strcmp(key, RESERVED_FIELD_KEYS[i]) == 0) return true;
  return false;
}

// RFC 5424 severity of each clog_level: TRACE and DEBUG are debug (7), INFO is
// informational, WARN warning, ERROR error, ALERT alert and FATAL emergency.
// The last entry is for CLOG_OFF, which is never a message level.
static const int SYSLOG_SEVERITY[] = {
    7, 7, 6, 4, 3, 1, 0, 7,
};
_Static_assert(
    sizeof(SYSLOG_SEVERITY) / sizeof(SYSLOG_SEVERITY[0]) == CLOG_OFF + 1,
    "SYSLOG_SEVERITY must have exactly one entry per clog_level value");

// One gzip compression of a rotated file. rotate() queues it for the target's
// compressor thread. src and dst ("<src>.gz") are relative to the target's
// dir_fd. Only the running job (clog_shared.compress_running) protects its
// generation from prune_rotated(); a queued job doesn't, and if its source gets
// pruned the compression does nothing. The queue is guarded by the target's
// mutex.
typedef struct clog_compress_job {
  struct clog_compress_job *next;
  char *src;
  char *dst;
  // The owner of the live file when the job was queued; src is compressed only
  // when it's owned by this user or by the effective user.
  uid_t live_uid;
  // Identity of a temporary output a failed compression couldn't remove.
  dev_t tmp_dev;
  ino_t tmp_ino;
} clog_compress_job;

// A growable byte buffer for building records.
typedef struct {
  char *data;
  size_t len;
  size_t cap;
  // Hard growth ceiling. CLOG_BUF_LIMIT for every buffer except the async
  // batch, which shared_async_init() raises to the configured
  // flush_buffer_size.
  size_t cap_limit;
  const ccol_memmgmt_procs *m_procs;
  // Set by buf_ensure() when the allocator itself failed, as opposed to hitting
  // cap_limit. clog_build_record() resets and reads it to pick the right
  // fallback message.
  bool oom;
} clog_buf;

// Values of clog_shared.async_partial_record. A record can span several lines
// (a logfmt backtrace), so a short write can stop inside a line or exactly at a
// newline inside the record. Only the first leaves a line open on disk, so the
// two need different repairs. CLOG_OWED_REPAIR is a separate bit: The target
// owes its output a newline or a loss marker.
enum {
  CLOG_SPLIT_NONE = 0,
  CLOG_SPLIT_MID_LINE = 1,
  CLOG_SPLIT_AT_LINE = 2,
  CLOG_SPLIT_MASK = 3,
  CLOG_OWED_REPAIR = 4,
};

// The backing store that a root logger and all loggers derived from it share:
// the fd, the rotation state, the async machinery and the mutex. It's reference
// counted, and an owned fd is closed when the count reaches zero. mutex guards
// almost everything here; the fields read without it are noted.
typedef struct clog_shared {
  int fd;
  bool owns_fd;
  // The low bits (CLOG_SPLIT_MASK) say whether the leading bytes of async_buf
  // continue a record whose first half a short write already put on disk. While
  // they're set no rotation happens and no other record goes to the fd, so the
  // two halves stay together. CLOG_OWED_REPAIR marks a debt (see owe_newline).
  // It sits next to fd because every write path tests both.
  unsigned char async_partial_record;
  // fd's clog_sink_kind, from fstat() at open and after each rotation. True
  // sink_per_record means a message socket: One write per record, never a
  // batch.
  unsigned char sink_kind;
  bool sink_per_record;
  // NULL for an fd-based logger.
  char *file_path;

  bool rotation_enabled;
  clog_rotation_cfg rotation;
  off_t bytes_written;
  time_t last_rotation;
  // Earliest time a rotation may be tried again after a failed one; 0 for no
  // backoff.
  time_t rotate_retry_after;
  // The job the compressor thread runs right now, or NULL.
  clog_compress_job *compress_running;

  ccol__mutex mutex;
  int ref_count;
  clog_format format;
  // Syslog facility, and the hostname and APP-NAME cached at creation.
  clog_syslog_facility syslog_facility;
  char syslog_hostname[256];
  char syslog_appname[49];
  // Heap copy of the caller's procs; NULL for the default allocator.
  ccol_memmgmt_procs *m_procs;

  // Async state; all zero for a synchronous logger. async_enabled is written
  // once before the object is published (or by the fork child handler), so it's
  // read without a lock.
  bool async_enabled;
  bool is_bounded_queue;
  union {
    ccircq *circq;
    cdynmq *dynmq;
  } q;
  // Deadline of the drain that turned q's sends off, set before the sends go
  // off. drain_left is set when a drain gave up on a writer thread that didn't
  // answer; later records then never wait on it. owes_repair mirrors
  // CLOG_OWED_REPAIR so the exit drain can see a debt without the mutex.
  _Atomic uint64_t drain_deadline_ns;
  _Atomic bool drain_left;
  _Atomic bool owes_repair;
  clog_async_cfg async_cfg;
  ccol__thread_id writer_thread;
  // The writer thread's batch, shared by every handle of this target.
  // last_flush_monotonic is on CLOCK_MONOTONIC so a clock step can't stretch
  // the flush interval.
  clog_buf async_buf;
  struct timespec last_flush_monotonic;

  // Rotation state, at the end so no field each write reads moves.
  // rot_last_stamp and rot_last_seq name the newest rotated file (stamp digits
  // without a NUL, and the collision number); every new name sorts above it.
  // The ignored_note fields rate-limit WARNs about entries that aren't
  // generations.
  char rot_last_stamp[CLOG_ROTATION_STAMP_DIGITS];
  bool rot_name_known;
  unsigned long rot_last_seq;
  time_t ignored_note_window;
  unsigned ignored_note_count;
  // Last component of file_path; every rotation step works on it relative to
  // dir_fd.
  const char *file_base;
  // Compression queue, oldest first, and its thread. compress_cv and the thread
  // exist while compressor_live is true.
  clog_compress_job *compress_head;
  clog_compress_job *compress_tail;
  ccol__cond_var compress_cv;
  ccol__thread_id compressor_thread;
  // The log file's directory, opened at create when rotation is on, else -1. It
  // keeps a relative path valid after a chdir().
  int dir_fd;
  bool compressor_live;
  bool compressor_stop;
  // compress_prune_deferred: A deletion pass kept an old generation only
  // because its compression was running, so the compressor runs the pass again
  // after it. compress_abort: The exit drain ran out of time, so the running
  // compression stops at its next block and keeps the source.
  bool compress_prune_deferred;
  _Atomic bool compress_abort;
  // The directory entry this target writes (directory dev/ino plus file_base),
  // so a second open of the same file finds it in clog_slot_table.file_shareds.
  bool file_id_known;
  dev_t file_dir_dev;
  ino_t file_dir_ino;
  // CLOG_FATAL calls waiting for mutex; the writer thread yields while it's not
  // zero (see clog_writer_lock()).
  atomic_uint fatal_waiters;
  // The repair this target owes its output (see clog_pay_owed()): A newline
  // closing a line left open on disk, and the count, records and bytes of
  // losses no marker has named yet. owed_err is the last failed write's errno,
  // and owed_detail is the text of the first loss.
  bool owe_newline;
  int owed_err;
  size_t owed_losses;
  size_t owed_records;
  size_t owed_bytes;
  char owed_detail[128];
  // errno of the last partial write of the async batch, while the batch keeps
  // the bytes.
  int retain_err;
#ifdef RUNNING_UNIT_TESTS
  // White-box counters for the test hooks of the same names, and the writer
  // thread's recorded signal mask (bit 0 SIGPIPE, bit 1 SIGXFSZ, bit 2
  // recorded).
  _Atomic size_t test_writer_timeout_wakeups;
  _Atomic size_t test_writer_interval_flushes;
  _Atomic size_t test_sync_lock_waiters;
  _Atomic size_t test_sync_records_locked;
  _Atomic unsigned test_writer_blocked_signals;
#endif
} clog_shared;

// One logger handle's object: Its level, its own fields and its record buffer.
// fields_mutex guards fields only, so field changes on one derived logger don't
// contend with writes on a sibling. When both locks are needed, shared->mutex
// is taken first. min_level is atomic because clog__write() reads it before it
// takes any lock.
struct clogger {
  clog_shared *shared;
  _Atomic clog_level min_level;
  ccol__mutex fields_mutex;
  chmap fields;
  clog_buf buf;
  // Written once before the handle is published, so an unpin finds its slot.
  clog self_handle;
};

// A slot of the handle table. A clog is (index << 32) | generation, so a stale
// handle never resolves. in_use goes false at the start of a close; freed goes
// true only when logger_free() has run, which is what the fork walk tests
// before it locks fields_mutex.
typedef struct {
  struct clogger *ptr;
  uint32_t generation;
  bool in_use;
  bool freed;
} clog_slot;

// The lock-free half of the handle table: Handle to pointer, plus the pin that
// keeps a logger alive during a call. Every log call resolves here and must
// write nothing another thread reads.
static ccol_pintable clog_pintable;

// The cold half of the handle table. The rwlock guards it for acquire, close
// and the fork walk; resolves go through clog_pintable and never take it. The
// vectors use the default allocator whatever a logger's procs are.
static struct {
  ccol__rw_lock rwlock;
  ccol__once_flag once;
  cvec slots;
  cvec free_indices;
  // Every clog_shared from its first handle until it's freed, whatever in_use
  // says; the fork walk locks the mutex of each.
  cvec live_shareds;
  // Closes that have cleared their slot but still use this table; without the
  // count a second close could free live_shareds under the first.
  size_t closes_in_flight;
  // Set when the exit destructor found a logger open; the last close then
  // releases the table.
  bool release_deferred;
  // File-backed targets with a known directory entry, so opening the same file
  // again attaches or fails instead of building a second target. A target
  // leaves it only after its last close stopped its threads.
  cvec file_shareds;
} clog_slot_table = {0};

static bool clog_any_slot_live_locked(void);
static void clog_release_slot_table_locked(void);
static void clog_release_slot_table_if_deferred_locked(void);

#if CCOL_FORK_SAFETY_REQUIRED

// A slot that a fork() caught in the middle of clog_close() (in_use false,
// freed false). The child finishes that close, since the closing thread doesn't
// exist there. With CCOL_FORK_SAFETY_REQUIRED at 0 none of the fork code is
// compiled.
typedef struct {
  uint32_t idx;
  struct clogger *raw;
} clog_atfork_closing;

// What clog_atfork_prepare() locked and recorded, so the parent and child
// handlers undo exactly that. Only the forking thread touches it, between
// prepare and release.
static struct {
  cvec locked_fields;
  cvec closing_slots;
} clog_atfork_state = {0};

static void clog_atfork_prepare(void);
static void clog_atfork_parent(void);
static void clog_atfork_child(void);
#endif
static void clog_slot_table_init_globals(void);

// Removes sh from v, a vector of clog_shared* in no order, by moving the last
// entry into its place. The close path uses it to take a target out of
// live_shareds or file_shareds. The caller holds the table's write lock.
static void clog_shareds_remove(cvec v, clog_shared *sh) {
  size_t n = cvector_size(v);
  for (size_t i = 0; i < n; i++) {
    if (*(clog_shared **)cvector_at(v, i) != sh) continue;
    clog_shared *last = *(clog_shared **)cvector_at(v, n - 1);
    *(clog_shared **)cvector_at(v, i) = last;
    cvector_pop_back(v, &last);
    return;
  }
}

static void logger_free(struct clogger *lg);
static void shared_close_owned_fd(clog_shared *sh);
static void shared_free_partial(clog_shared *sh);

#ifdef RUNNING_UNIT_TESTS

// Forces the next clog_handle_acquire() to take a fresh slot and fail its
// live_shareds registration, which reaches the rollback that has to reset
// freed. It disarms itself when it fires.
static _Atomic bool clog_test_force_next_fresh_slot_reg_failure = false;

// Takes the forced registration failure, for clog_handle_acquire() in a test
// build; true when it was armed.
static bool clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return atomic_exchange(&clog_test_force_next_fresh_slot_reg_failure, false);
}

// Arms or disarms the forced live_shareds failure of the next handle
// acquisition, so a test can reach the rollback that resets freed.
void clog__test_force_next_fresh_slot_registration_failure(bool force) {
  atomic_store(&clog_test_force_next_fresh_slot_reg_failure, force);
}

// Close hold gate of clog__test_hold_close_finalize(). It doesn't disarm
// itself; a held close gives up after CLOG_TEST_CLOSE_HOLD_MAX_US so a broken
// test fails instead of hanging.
#define CLOG_TEST_CLOSE_HOLD_MAX_US 30000000U
static _Atomic bool clog_test_close_finalize_hold = false;
static _Atomic bool clog_test_close_finalize_entered_state = false;

// Holds or releases closes at the close gate (between in_use and freed), so a
// test can fork inside that window; holding also clears the entered flag.
void clog__test_hold_close_finalize(bool hold) {
  if (hold) atomic_store(&clog_test_close_finalize_entered_state, false);
  atomic_store(&clog_test_close_finalize_hold, hold);
}

// Tells whether a close has reached the held gate, so a test knows when to act.
bool clog__test_close_finalize_entered(void) {
  return atomic_load(&clog_test_close_finalize_entered_state);
}

// Closes that found their handle pinned and started to wait (step 3).
static _Atomic size_t clog_test_close_pin_waits_state = 0;

// Gives how many closes started to wait for a pinned handle, so a test can see
// a close wait for a call in flight.
size_t clog__test_close_pin_waits(void) {
  return atomic_load(&clog_test_close_pin_waits_state);
}

// Delay of clog__test_set_close_release_window_us(); it doesn't disarm itself.
static _Atomic unsigned int clog_test_close_release_window_us = 0;
static _Atomic bool clog_test_close_release_window_entered_state = false;

// Sets how long a close stays in its release window (slot cleared, table still
// in use), so a test can race a second close there; it clears the entered flag.
void clog__test_set_close_release_window_us(unsigned int delay_us) {
  atomic_store(&clog_test_close_release_window_entered_state, false);
  atomic_store(&clog_test_close_release_window_us, delay_us);
}

// Tells whether a close has entered its release window.
bool clog__test_close_release_window_entered(void) {
  return atomic_load(&clog_test_close_release_window_entered_state);
}

// Gives the number of targets in live_shareds, read under the table's read
// lock, so a test can check that every rollback unregisters its target.
size_t clog__test_live_shareds_count(void) {
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
  ccol__rw_lock_rdlock(clog_slot_table.rwlock);
  size_t n = clog_slot_table.live_shareds
                 ? cvector_size(clog_slot_table.live_shareds)
                 : 0;
  ccol__rw_lock_unlock(clog_slot_table.rwlock);
  return n;
}
#else

// Stub of a non-test build, so clog_handle_acquire() needs no #ifdef: No
// registration failure is forced.
static inline bool clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return false;
}
#endif

// Initializes the handle table and the fork scratch vectors, once, through
// clog_slot_table.once, before the first handle exists. It also registers the
// fork handlers, so they exist whenever a logger can. A failure here is fatal.
static void clog_slot_table_init_globals(void) {
  if (ccol__rw_lock_init(clog_slot_table.rwlock) != 0)
    ccol__fatal_err("clog slot table: failed to initialize rwlock");
  clog_slot_table.slots = cvector_create(sizeof(clog_slot), NULL);
  if (!clog_slot_table.slots)
    ccol__fatal_err("clog slot table: failed to allocate slots vector");
  clog_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clog_slot_table.free_indices)
    ccol__fatal_err("clog slot table: failed to allocate free-index vector");
  clog_slot_table.live_shareds = cvector_create(sizeof(clog_shared *), NULL);
  if (!clog_slot_table.live_shareds)
    ccol__fatal_err("clog slot table: failed to allocate live-shareds vector");
  clog_slot_table.file_shareds = cvector_create(sizeof(clog_shared *), NULL);
  if (!clog_slot_table.file_shareds)
    ccol__fatal_err("clog slot table: failed to allocate file-shareds vector");
#if CCOL_FORK_SAFETY_REQUIRED
  clog_atfork_state.locked_fields =
      cvector_create(sizeof(struct clogger *), NULL);
  if (!clog_atfork_state.locked_fields)
    ccol__fatal_err(
        "clog slot table: failed to allocate atfork scratch vector");
  clog_atfork_state.closing_slots =
      cvector_create(sizeof(clog_atfork_closing), NULL);
  if (!clog_atfork_state.closing_slots)
    ccol__fatal_err(
        "clog slot table: failed to allocate atfork closing-slots vector");
  ccol__at_fork(clog_atfork_prepare, clog_atfork_parent, clog_atfork_child);
#endif
}

// Registers this module's fork handlers now, so they're registered before the
// caller's. Prepare handlers run in reverse order, so the caller's prepare (for
// chttpserver, which calls into clog while holding its engine mutex) then runs
// first and the fork lock order matches the ordinary one.
void clog__ensure_atfork_registered_before_caller(void) {
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
}

// Resolves h and pins the logger against a concurrent close; NULL for a null,
// stale or out of range handle. The caller must unpin exactly once on every
// path. It takes no lock and writes nothing shared, since every log call runs
// it.
static struct clogger *clog_resolve(clog h) {
  return (struct clogger *)ccol_pintable_pin(&clog_pintable, h);
}

// Releases the pin clog_resolve() took, on every path out of a public call.
// Nothing may touch raw after the release, since a close may free it at once,
// and no lock or wakeup belongs here, because the close polls the pin count.
static void clog_resolve_unpin(struct clogger *raw) {
  ccol_pintable_unpin(&clog_pintable, raw->self_handle);
}

// Makes a handle for a fully built lg (from a create or clog_derive()), and
// registers lg->shared in live_shareds unless it's there already. Gives
// CLOG_NULL when the table can't grow or was released at exit. Takes the write
// lock. Every acquire sets freed = false, and every rollback leaves the slot
// freed with ptr NULL, because the fork walk locks fields_mutex of any slot
// that isn't freed. A failed publish also undoes the live_shareds push (the
// tail entry, since the lock is held throughout), or the next fork would lock a
// mutex inside freed memory.
static clog clog_handle_acquire(struct clogger *lg) {
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
  if (__builtin_expect(clog_slot_table.slots == NULL, 0)) {
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_NULL;
  }

  bool force_fresh_slot_reg_failure =
      clog_test_consume_forced_fresh_slot_reg_failure();

  uint32_t idx;
  clog_slot *slot;
  if (!force_fresh_slot_reg_failure &&
      cvector_size(clog_slot_table.free_indices) > 0) {
    cvector_pop_back(clog_slot_table.free_indices, &idx);
    slot = (clog_slot *)cvector_at(clog_slot_table.slots, idx);
  } else {
    if (cvector_size(clog_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol__rw_lock_unlock(clog_slot_table.rwlock);
      return CLOG_NULL;
    }
    clog_slot fresh = {0};
    if (cvector_push_back(clog_slot_table.slots, &fresh) != ccol_success) {
      ccol__rw_lock_unlock(clog_slot_table.rwlock);
      return CLOG_NULL;
    }
    idx = (uint32_t)cvector_size(clog_slot_table.slots) - 1;
    slot = (clog_slot *)cvector_at(clog_slot_table.slots, idx);
  }

  bool already_registered = false;
  size_t n = cvector_size(clog_slot_table.live_shareds);
  for (size_t i = 0; i < n; i++) {
    clog_shared *seen =
        *(clog_shared **)cvector_at(clog_slot_table.live_shareds, i);
    if (seen == lg->shared) {
      already_registered = true;
      break;
    }
  }
  bool live_shareds_registration_failed = force_fresh_slot_reg_failure;
  if (!live_shareds_registration_failed && !already_registered) {
    live_shareds_registration_failed =
        cvector_push_back(clog_slot_table.live_shareds, &lg->shared) !=
        ccol_success;
  }
  if (live_shareds_registration_failed) {
    slot->freed = true;
    slot->ptr = NULL;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_NULL;
  }

  slot->generation++;
  if (slot->generation == 0) slot->generation++;

  clog h = ((clog)idx << 32) | (clog)slot->generation;

  lg->self_handle = h;

  if (!ccol_pintable_publish(&clog_pintable, idx, slot->generation, lg)) {
    if (!already_registered) {
      clog_shared *rolled_back = NULL;
      cvector_pop_back(clog_slot_table.live_shareds, &rolled_back);
    }
    lg->self_handle = 0;
    slot->freed = true;
    slot->ptr = NULL;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_NULL;
  }

  slot->ptr = lg;
  slot->in_use = true;
  slot->freed = false;

  ccol__rw_lock_unlock(clog_slot_table.rwlock);
  return h;
}

#if CCOL_FORK_SAFETY_REQUIRED

// Fork prepare handler. fork() copies only the calling thread, so it takes the
// table's write lock, the mutex of every live clog_shared (from live_shareds,
// because a closing slot isn't in_use while its close still holds that mutex)
// and the fields_mutex of every slot that isn't freed. A lock it can't record
// for lack of memory is released again at once. It also records the slots
// caught in the middle of a close (in_use false, freed false), which the child
// finishes.
static void clog_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  ccol__atfork_order_record(ccol__atfork_module_clogger);
#endif
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
  if (clog_slot_table.slots == NULL) return;

  size_t ns = cvector_size(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared *sh =
        *(clog_shared **)cvector_at(clog_slot_table.live_shareds, i);
    ccol__mutex_lock(sh->mutex);
  }

  size_t n = cvector_size(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot *slot = (clog_slot *)cvector_at(clog_slot_table.slots, i);
    if (slot->freed) continue;
    ccol__mutex_lock(slot->ptr->fields_mutex);
    if (cvector_push_back(clog_atfork_state.locked_fields, &slot->ptr) !=
        ccol_success) {
      ccol__mutex_unlock(slot->ptr->fields_mutex);
    }

    if (!slot->in_use) {
      clog_atfork_closing c = {.idx = (uint32_t)i, .raw = slot->ptr};
      cvector_push_back(clog_atfork_state.closing_slots, &c);
    }
  }
}

// Undoes clog_atfork_prepare() in the parent or the child. The child
// initializes the table's rwlock afresh, since glibc tracks the writer by TID
// and an unlock from the child's new TID releases nothing. In the child it also
// resets the pin count of every slot (no other thread can release a pin there),
// forgets the compression jobs and turns async off for every target (their
// threads don't exist in the child), finishes every close the fork interrupted,
// and forgets targets that were in the middle of their last close.
// closing_slots is cleared on both sides, so a later fork never acts on stale
// entries.
static void clog_atfork_release(bool in_child) {
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
  if (clog_slot_table.slots == NULL) {
    if (in_child) {
      if (ccol__rw_lock_reinit_in_child(clog_slot_table.rwlock) != 0)
        ccol__fatal_err(
            "clog atfork release: failed to reinit slot table rwlock");
    } else {
      ccol__rw_lock_unlock(clog_slot_table.rwlock);
    }
    return;
  }
  size_t nf = cvector_size(clog_atfork_state.locked_fields);
  for (size_t i = nf; i-- > 0;) {
    struct clogger *lg =
        *(struct clogger **)cvector_at(clog_atfork_state.locked_fields, i);
    if (in_child) {
      ccol_pintable_reset_for(&clog_pintable, lg->self_handle);
    }
    ccol__mutex_unlock(lg->fields_mutex);
  }

  size_t ns = cvector_size(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared *sh =
        *(clog_shared **)cvector_at(clog_slot_table.live_shareds, i);
    if (in_child) {
      sh->compress_running = NULL;
      sh->compress_head = NULL;
      sh->compress_tail = NULL;
      sh->compressor_live = false;
      sh->compressor_stop = false;
      sh->compress_prune_deferred = false;
    }
    if (in_child && sh->async_enabled) {
      sh->async_enabled = false;
      sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
    }
    ccol__mutex_unlock(sh->mutex);
  }

  if (in_child) {
    size_t nc = cvector_size(clog_atfork_state.closing_slots);
    for (size_t i = 0; i < nc; i++) {
      clog_atfork_closing *c =
          (clog_atfork_closing *)cvector_at(clog_atfork_state.closing_slots, i);
      clog_slot *slot = (clog_slot *)cvector_at(clog_slot_table.slots, c->idx);
      if (slot->freed) continue;

      struct clogger *raw = c->raw;
      clog_shared *sh = raw->shared;

      ccol_pintable_reset_for(&clog_pintable, raw->self_handle);

      logger_free(raw);
      slot->freed = true;
      slot->ptr = NULL;
      slot->generation++;
      if (slot->generation == 0) slot->generation++;
      cvector_push_back(clog_slot_table.free_indices, &c->idx);

      if (--sh->ref_count == 0) {
        clog_shareds_remove(clog_slot_table.live_shareds, sh);
        clog_shareds_remove(clog_slot_table.file_shareds, sh);
        shared_close_owned_fd(sh);
        ccol__mutex_destroy(sh->mutex);
        shared_free_partial(sh);
      }
    }
  }

  if (in_child) {
    size_t nfs = cvector_size(clog_slot_table.file_shareds);
    for (size_t i = nfs; i-- > 0;) {
      clog_shared *fs =
          *(clog_shared **)cvector_at(clog_slot_table.file_shareds, i);
      bool live = false;
      size_t nl = cvector_size(clog_slot_table.live_shareds);
      for (size_t j = 0; j < nl && !live; j++)
        live =
            *(clog_shared **)cvector_at(clog_slot_table.live_shareds, j) == fs;
      if (!live) clog_shareds_remove(clog_slot_table.file_shareds, fs);
    }
    if (ccol__rw_lock_reinit_in_child(clog_slot_table.rwlock) != 0)
      ccol__fatal_err(
          "clog atfork release: failed to reinit slot table rwlock");
  } else {
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
  }
  cvector_reset(clog_atfork_state.locked_fields);
  cvector_reset(clog_atfork_state.closing_slots);
}

// The fork handlers of the parent and the child, registered with
// clog_atfork_prepare(); both undo what prepare took through
// clog_atfork_release().
static void clog_atfork_parent(void) { clog_atfork_release(false); }
static void clog_atfork_child(void) { clog_atfork_release(true); }
#endif

static bool clog_table_still_needed_locked(void);

// True when a slot still names a logger. It reads ptr, which a close clears
// only in its last locked step; in_use goes false much earlier. Callers go
// through clog_table_still_needed_locked(). Needs the write lock.
static bool clog_any_slot_live_locked(void) {
  size_t n = cvector_size(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot *slot = (clog_slot *)cvector_at(clog_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

// Releases the table when the exit destructor deferred that and nothing needs
// it any more, so a program that closes its loggers late leaves nothing
// allocated. The last close calls it. It isn't inlined so that close path keeps
// its shape. Needs the write lock.
static __attribute__((noinline)) void
clog_release_slot_table_if_deferred_locked(void) {
  if (clog_slot_table.release_deferred && !clog_table_still_needed_locked()) {
    clog_release_slot_table_locked();
  }
}

// Tells whether a slot names a logger or a close hasn't finished with the
// table, the test both release paths use before they free the table. Needs the
// write lock.
static bool clog_table_still_needed_locked(void) {
  return clog_slot_table.closes_in_flight != 0 || clog_any_slot_live_locked();
}

// Frees the table's vectors and the pin index. The caller holds the write lock
// and knows nothing needs the table. Each vector is NULL afterwards, so a later
// call sees the table as released. The rwlock stays.
static void clog_release_slot_table_locked(void) {
  cvector_destroy(clog_slot_table.slots);
  cvector_destroy(clog_slot_table.free_indices);
  cvector_destroy(clog_slot_table.live_shareds);
  cvector_destroy(clog_slot_table.file_shareds);
  ccol_pintable_dispose(&clog_pintable);
#if CCOL_FORK_SAFETY_REQUIRED
  cvector_destroy(clog_atfork_state.locked_fields);
  cvector_destroy(clog_atfork_state.closing_slots);
#endif
  clog_slot_table.release_deferred = false;
}

// How long the exit release waits for the table's lock before it gives up and
// leaves the table allocated.
#define CLOG_TABLE_RELEASE_WAIT_MS 1000

// Frees the handle table at exit or unload, but only when no logger is open and
// no close is running; otherwise the last close does it (release_deferred). It
// holds the write lock throughout, with a bounded wait. The rwlock itself is
// never destroyed, because a later caller may still take it.
__attribute__((destructor)) static void cleanup_clog_slot_table(void) {
  if (!clog_slot_table.slots) return;
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += CLOG_TABLE_RELEASE_WAIT_MS / 1000;
  until.tv_nsec += (long)(CLOG_TABLE_RELEASE_WAIT_MS % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000L;
  }
  if (ccol__rw_lock_timedwrlock(clog_slot_table.rwlock, until) != 0) return;
  if (clog_table_still_needed_locked()) {
    clog_slot_table.release_deferred = true;
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
    return;
  }
  clog_release_slot_table_locked();
  ccol__rw_lock_unlock(clog_slot_table.rwlock);
}

// Gives b its first CLOG_BUF_INITIAL bytes from m_procs, with the default
// growth ceiling; every record buffer starts here. Returns 0 or -1.
static int buf_init(clog_buf *b, const ccol_memmgmt_procs *m_procs) {
  b->m_procs = m_procs;
  b->data = ccol__mem_alloc(m_procs, CLOG_BUF_INITIAL);
  if (!b->data) return -1;
  b->data[0] = '\0';
  b->len = 0;
  b->cap = CLOG_BUF_INITIAL;
  b->cap_limit = CLOG_BUF_LIMIT;
  b->oom = false;
  return 0;
}

// Raises b's growth ceiling to new_limit, never lowering it, so the async batch
// can hold a flush_buffer_size larger than one record's limit.
static void buf_raise_cap_limit(clog_buf *b, size_t new_limit) {
  if (new_limit > b->cap_limit) b->cap_limit = new_limit;
}

// Empties b and keeps its memory, so the next record reuses the allocation.
static void buf_reset(clog_buf *b) { b->len = 0; }

// Frees b's memory when its logger or target goes away.
static void buf_free(clog_buf *b) {
  ccol__mem_free(b->m_procs, b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

#ifdef RUNNING_UNIT_TESTS

// Forces the next buf_append() or buf_appendf() to report an allocation
// failure, even for an append that needs no growth. It disarms itself when it
// fires.
static _Atomic bool clog_test_force_next_buf_ensure_failure_state = false;

// Takes the forced append failure, for buf_append() and buf_appendf() in a test
// build; true when it was armed.
static bool clog_test_consume_forced_buf_ensure_failure(void) {
  return atomic_exchange(&clog_test_force_next_buf_ensure_failure_state, false);
}

// Arms or disarms the forced failure of the next buffer append, so a test can
// reach the fallbacks of a failed allocation.
void clog__test_force_next_buf_ensure_failure(bool force) {
  atomic_store(&clog_test_force_next_buf_ensure_failure_state, force);
}
#else

// Stub of a non-test build, so the append functions need no #ifdef: No append
// failure is forced.
static inline bool clog_test_consume_forced_buf_ensure_failure(void) {
  return false;
}
#endif

// Makes room for at least need more bytes, doubling the capacity up to
// cap_limit. Returns 0 or -1. Hitting cap_limit is an ordinary size refusal;
// only a failed allocation sets b->oom. The doubling is guarded against size_t
// overflow and goes back through the size check after clamping to cap_limit,
// since a reachable cap_limit can still be too small.
static int buf_ensure(clog_buf *b, size_t need) {
  if (b->cap - b->len >= need) return 0;
  size_t new_cap = b->cap ? b->cap : CLOG_BUF_INITIAL;
  while (new_cap - b->len < need) {
    if (new_cap >= b->cap_limit) return -1;
    if (new_cap > SIZE_MAX / 2) {
      new_cap = b->cap_limit;
    } else {
      new_cap *= 2;
      if (new_cap > b->cap_limit) new_cap = b->cap_limit;
    }
  }
  char *p = ccol__mem_realloc(b->m_procs, b->data, new_cap);
  if (!p) {
    b->oom = true;
    return -1;
  }
  b->data = p;
  b->cap = new_cap;
  return 0;
}

// Appends n bytes of s to b, growing it as needed, and keeps b NUL-terminated;
// the basic step of every record builder. Returns 0, or -1 when b can't grow (a
// test can force that).
static int buf_append(clog_buf *b, const char *s, size_t n) {
  if (clog_test_consume_forced_buf_ensure_failure()) {
    b->oom = true;
    return -1;
  }
  if (buf_ensure(b, n + 1) != 0) return -1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 0;
}

// Appends printf-formatted text to b, growing it when the first attempt doesn't
// fit, for the parts of a record that need formatting. Returns 0 or -1.
static int __attribute__((format(printf, 2, 3))) buf_appendf(clog_buf *b,
                                                             const char *fmt,
                                                             ...) {
  if (clog_test_consume_forced_buf_ensure_failure()) {
    b->oom = true;
    return -1;
  }

  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);

  size_t avail = b->cap - b->len;
  int n = vsnprintf(b->data + b->len, avail, fmt, ap);
  va_end(ap);

  if (n < 0) {
    va_end(ap2);
    return -1;
  }
  if ((size_t)n >= avail) {
    if (buf_ensure(b, (size_t)n + 1) != 0) {
      va_end(ap2);
      return -1;
    }
    avail = b->cap - b->len;
    n = vsnprintf(b->data + b->len, avail, fmt, ap2);
    if (n < 0 || (size_t)n >= avail) {
      va_end(ap2);
      return -1;
    }
  }
  va_end(ap2);
  b->len += (size_t)n;
  return 0;
}

// Writes the backslash escape of a control byte c (below 0x20, or 0x7f) to esc
// and its length to *esc_len: \n, \r and \t, or \xXX. Every escaper uses it, so
// a control byte looks the same in every format.
static void ctrl_escape(unsigned char c, char esc[5], int *esc_len) {
  if (c == '\n') {
    esc[0] = '\\';
    esc[1] = 'n';
    *esc_len = 2;
  } else if (c == '\r') {
    esc[0] = '\\';
    esc[1] = 'r';
    *esc_len = 2;
  } else if (c == '\t') {
    esc[0] = '\\';
    esc[1] = 't';
    *esc_len = 2;
  } else {
    snprintf(esc, 5, "\\x%02x", c);
    *esc_len = 4;
  }
}

// Length (2, 3 or 4) of the well-formed UTF-8 sequence that starts at the
// non-ASCII byte p[0], or 0 when the lead byte is invalid or the sequence is
// overlong, a surrogate, past U+10FFFF or truncated. It stops at the NUL, so it
// never reads past the end of the string.
static int utf8_valid_seq_len(const unsigned char *p) {
  unsigned char c0 = p[0];
  if (c0 < 0xc2 || c0 > 0xf4) return 0;

  int len;
  unsigned char lo1, hi1;
  if (c0 <= 0xdf) {
    len = 2;
    lo1 = 0x80;
    hi1 = 0xbf;
  } else if (c0 <= 0xef) {
    len = 3;
    lo1 = (c0 == 0xe0) ? 0xa0 : 0x80;
    hi1 = (c0 == 0xed) ? 0x9f : 0xbf;
  } else {
    len = 4;
    lo1 = (c0 == 0xf0) ? 0x90 : 0x80;
    hi1 = (c0 == 0xf4) ? 0x8f : 0xbf;
  }

  unsigned char c1 = p[1];
  if (c1 == '\0' || c1 < lo1 || c1 > hi1) return 0;
  for (int i = 2; i < len; i++) {
    unsigned char ci = p[i];
    if (ci == '\0' || ci < 0x80 || ci > 0xbf) return 0;
  }
  return len;
}

// For a non-ASCII byte at p, the length of the sequence an escaper copies as it
// is, or 0 when append_utf8_rewrite() must write it: A C1 control (a terminal
// can read 0xc2 0x9b as CSI) or a byte that isn't well-formed UTF-8.
static inline int utf8_pass_len(const unsigned char *p) {
  if (p[0] == 0xc2 && p[1] <= 0x9f) return 0;
  return utf8_valid_seq_len(p);
}

// Writes the replacement of what utf8_pass_len() refused and returns how many
// input bytes it stands for, or -1 on failure. A C1 control becomes \u0080 to
// \u009f (2 bytes); a bad byte becomes U+FFFD (1 byte), as an escape when json
// is true and as UTF-8 otherwise.
static int append_utf8_rewrite(clog_buf *b, const unsigned char *p, bool json) {
  if (p[0] == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) {
    static const char hex[] = "0123456789abcdef";
    char esc[6] = {'\\', 'u', '0', '0', hex[p[1] >> 4], hex[p[1] & 0x0f]};
    return buf_append(b, esc, sizeof(esc)) == 0 ? 2 : -1;
  }
  if (json) return buf_append(b, "\\ufffd", 6) == 0 ? 1 : -1;
  return buf_append(b, "\xef\xbf\xbd", 3) == 0 ? 1 : -1;
}

// Byte classes for buf_append_lv(): 0 copies as it is, 1 forces quotes (space,
// '=', '"', '\\', controls, DEL), 2 starts a UTF-8 sequence to check, 3 is the
// NUL.
static const unsigned char clog_lv_class[256] = {
    [0x00] = 3, [0x01 ... 0x1F] = 1, [' '] = 1,  ['='] = 1,
    ['"'] = 1,  ['\\'] = 1,          [0x7F] = 1, [0x80 ... 0xFF] = 2};

// Appends a logfmt value. A value with a space, '=', '"', '\\', a control or a
// bad UTF-8 byte goes in quotes with escapes; an empty one is "". One table
// load per byte and no strlen(), because records are built under the target's
// mutex.
static int buf_append_lv(clog_buf *b, const char *s) {
  if (!s) return buf_append(b, "null", 4);

  const unsigned char *p = (const unsigned char *)s;
  bool quote = false;
  for (;;) {
    unsigned char k = clog_lv_class[*p];
    if (__builtin_expect(k == 0, 1)) {
      p++;
      continue;
    }
    if (k == 3) break;
    if (k == 2) {
      int n = utf8_pass_len(p);
      if (n > 0) {
        p += n;
        continue;
      }
    }
    quote = true;
    break;
  }
  if (!quote && p != (const unsigned char *)s)
    return buf_append(b, s, (size_t)(p - (const unsigned char *)s));

  if (buf_append(b, "\"", 1) != 0) return -1;

  const char *run = s;
  for (const char *q = s;; q++) {
    unsigned char c = (unsigned char)*q;
    unsigned char k = clog_lv_class[c];
    if (__builtin_expect(k == 0, 1)) continue;

    if (k == 3) {
      if (q > run && buf_append(b, run, (size_t)(q - run)) != 0) return -1;
      break;
    }
    if (c == '"' || c == '\\') {
      if (q > run && buf_append(b, run, (size_t)(q - run)) != 0) return -1;
      char esc[2] = {'\\', (char)c};
      if (buf_append(b, esc, 2) != 0) return -1;
      run = q + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (q > run && buf_append(b, run, (size_t)(q - run)) != 0) return -1;
      char esc[5];
      int esc_len;
      ctrl_escape(c, esc, &esc_len);
      if (buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = q + 1;
    } else if (c >= 0x80) {
      int n = utf8_pass_len((const unsigned char *)q);
      if (n == 0) {
        if (q > run && buf_append(b, run, (size_t)(q - run)) != 0) return -1;
        n = append_utf8_rewrite(b, (const unsigned char *)q, false);
        if (n < 0) return -1;
        run = q + n;
      }
      q += n - 1;
    }
  }

  return buf_append(b, "\"", 1);
}

// Appends the JSON-escaped content of s without quotes. Controls (C1 included)
// become escapes and each byte that isn't well-formed UTF-8 becomes \ufffd, so
// the output is always valid JSON text whatever the input. s must not be NULL.
static int buf_append_json_content(clog_buf *b, const char *s) {
  const char *run = s;
  const char *p = s;
  for (;;) {
    unsigned char c = (unsigned char)*p;
    if (c == '\0') {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (c == '"' || c == '\\') {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[2] = {'\\', (char)c};
      if (buf_append(b, esc, 2) != 0) return -1;
      p++;
      run = p;
      continue;
    }
    if (c < 0x20 || c == 0x7f) {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[7];
      int esc_len;
      if (c == '\n') {
        esc[0] = '\\';
        esc[1] = 'n';
        esc_len = 2;
      } else if (c == '\r') {
        esc[0] = '\\';
        esc[1] = 'r';
        esc_len = 2;
      } else if (c == '\t') {
        esc[0] = '\\';
        esc[1] = 't';
        esc_len = 2;
      } else {
        esc_len = snprintf(esc, sizeof(esc), "\\u%04x", c);
      }
      if (buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      p++;
      run = p;
      continue;
    }
    if (c >= 0x80) {
      int seq_len = utf8_pass_len((const unsigned char *)p);
      if (seq_len > 0) {
        p += seq_len;
        continue;
      }
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      seq_len = append_utf8_rewrite(b, (const unsigned char *)p, true);
      if (seq_len < 0) return -1;
      p += seq_len;
      run = p;
      continue;
    }
    p++;
  }
  return 0;
}

// Appends ,"key":"val" with val escaped, or ,"key":null for a NULL val, for the
// fixed fields of a JSON record. Returns 0 or -1.
static int buf_append_json_kv(clog_buf *b, const char *key, const char *val) {
  if (buf_append(b, ",\"", 2) != 0) return -1;
  if (buf_append_json_content(b, key) != 0) return -1;
  if (buf_append(b, "\":", 2) != 0) return -1;
  if (!val) return buf_append(b, "null", 4);
  if (buf_append(b, "\"", 1) != 0) return -1;
  if (buf_append_json_content(b, val) != 0) return -1;
  return buf_append(b, "\"", 1);
}

// Appends an RFC 5424 SD-PARAM-VALUE: '"', '\\' and ']' get a backslash, and
// controls are escaped as in the other formats, \n and \r included, so a record
// never spans lines. Bad UTF-8 bytes become U+FFFD. NULL is written as null.
static int buf_append_sd_value(clog_buf *b, const char *s) {
  if (!s) return buf_append(b, "null", 4);
  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;
    if (*p == '\0') {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '"' || *p == '\\' || *p == ']') {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[2] = {'\\', *p};
      if (buf_append(b, esc, 2) != 0) return -1;
      run = p + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[5];
      int esc_len;
      ctrl_escape(c, esc, &esc_len);
      if (buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = p + 1;
    } else if (c >= 0x80) {
      int n = utf8_pass_len((const unsigned char *)p);
      if (n == 0) {
        if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
        n = append_utf8_rewrite(b, (const unsigned char *)p, false);
        if (n < 0) return -1;
        run = p + n;
      }
      p += n - 1;
    }
  }
  return 0;
}

// Writes the SD-PARAM-NAME for key into name. A key longer than 32 characters
// becomes its first 23, '~' and an FNV-1a hash of the whole key in 8 hex
// digits, so two keys with a common prefix don't collide.
static void sd_param_name(const char *key, char name[33]) {
  size_t klen = strlen(key);
  if (klen <= 32) {
    memcpy(name, key, klen + 1);
    return;
  }

  uint32_t h = 2166136261u;
  for (const char *p = key; *p; p++) {
    h ^= (unsigned char)*p;
    h *= 16777619u;
  }

  memcpy(name, key, 23);
  snprintf(name + 23, 33 - 23, "~%08x", h);
}

// Appends free text with every control escaped and '\\' doubled, so it reads
// back without ambiguity and can't split a record: A syslog MSG, and a
// backtrace frame in logfmt and syslog. Bad UTF-8 becomes U+FFFD. s must not be
// NULL.
static int buf_append_ctrl_escaped(clog_buf *b, const char *s) {
  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;
    if (*p == '\0') {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '\\') {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      if (buf_append(b, "\\\\", 2) != 0) return -1;
      run = p + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[5];
      int esc_len;
      ctrl_escape(c, esc, &esc_len);
      if (buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = p + 1;
    } else if (c >= 0x80) {
      int n = utf8_pass_len((const unsigned char *)p);
      if (n == 0) {
        if (p > run && buf_append(b, run, (size_t)(p - run)) != 0) return -1;
        n = append_utf8_rewrite(b, (const unsigned char *)p, false);
        if (n < 0) return -1;
        run = p + n;
      }
      p += n - 1;
    }
  }
  return 0;
}

// Copies the PRINTUSASCII bytes (0x21 to 0x7e) of raw into out, dropping every
// other byte, for the syslog APP-NAME and HOSTNAME fields. An empty result
// becomes "-". outsz may be 0 (nothing is written); the loop bound is written
// as i + 1 < outsz so it can't underflow.
static void sanitize_syslog_printusascii_field(const char *raw, char *out,
                                               size_t outsz) {
  if (outsz == 0) return;
  size_t i = 0;
  for (const char *p = raw; *p && i + 1 < outsz; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x21 || c > 0x7e) continue;
    out[i++] = (char)c;
  }
  if (i == 0 && outsz >= 2) {
    out[0] = '-';
    i = 1;
  }
  out[i] = '\0';
}

// Gives the short program name the platform reports, or NULL.
static const char *raw_progname(void) {
#if defined(__linux__)
  if (program_invocation_short_name && program_invocation_short_name[0])
    return program_invocation_short_name;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  const char *p = getprogname();
  if (p && p[0]) return p;
#endif
  return NULL;
}

// Gives the program name for APP-NAME, or "-".
static const char *syslog_appname_compute(void) {
  const char *p = raw_progname();
  return p ? p : "-";
}

// Gives the program name for the proc field, or "unknown".
static const char *get_progname(void) {
  const char *p = raw_progname();
  return p ? p : "unknown";
}

// Gives the calling thread's OS thread ID for the identity cache, so a record's
// tid matches what ps(1) shows. Where the platform has none, the pthread handle
// stands in: Unique per thread, but not what the tools show.
static pid_t get_tid(void) {
#if defined(__linux__)
  return (pid_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
  uint64_t tid64 = 0;
  ccol__get_thread_id_np(NULL, &tid64);
  return (pid_t)tid64;
#elif defined(__FreeBSD__)
  return (pid_t)ccol__get_own_thread_os_id_np();
#elif defined(__OpenBSD__)
  return getthrid();
#else
  return (pid_t)(uintptr_t)ccol__get_thread_id();
#endif
}

// Copies the calling thread's name into buf (at most bufsz - 1 characters) for
// the identity cache. An unnamed thread reads as the program name, as it does
// on Linux.
static void get_thread_name(char *buf, size_t bufsz) {
#if defined(__linux__)
  char name[16];
  if (prctl(PR_GET_NAME, name) == 0 && name[0]) {
    snprintf(buf, bufsz, "%s", name);
    return;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  if (ccol__get_thread_name_np(ccol__get_thread_id(), buf, bufsz) == 0 &&
      buf[0])
    return;
  snprintf(buf, bufsz, "%.15s", get_progname());
  return;
#endif
  snprintf(buf, bufsz, "unknown");
}

// Thread identity cache. Each record names its writer as
// "<progname>(<pid>):<thread name>(<tid>)". A thread reads those once, on its
// first record, into a heap block reached through a thread-local pointer, so
// later records make no system call. A fork child handler invalidates the
// cache, and ccol_set_thread_name() updates it; a rename by any other means
// isn't seen. A key destructor frees the block at thread exit. The block is on
// the heap so the library's static TLS stays small for dlopen().
// CLOG_THREAD_NAME_LIMIT is the longest name the kernel keeps.
#define CLOG_PROC_VAL_LEN 256
#define CLOG_THREAD_NAME_LIMIT 15

// One thread's cached identity; proc holds the formatted string.
typedef struct {
  bool valid;
  pid_t pid;
  pid_t tid;
  char name[CLOG_THREAD_NAME_LIMIT + 1];
  char proc[CLOG_PROC_VAL_LEN];
} clog_thread_ident;

// The calling thread's cache, initial-exec where allowed so a record reads it
// with no call.
#if defined(CMEMPOOL_DYNAMIC_TLS) && CMEMPOOL_DYNAMIC_TLS
static __thread clog_thread_ident *clog_tident;
#else
static __thread clog_thread_ident *clog_tident
    __attribute__((tls_model("initial-exec")));
#endif

// Guards the key creation and the fork handler registration.
static ccol__once_flag clog_tident_once = CCOL__ONCE_INIT;

// Identity of a record whose thread couldn't allocate its cache.
static const clog_thread_ident clog_tident_unknown = {
    .valid = true, .pid = 0, .tid = 0, .name = "", .proc = "unknown"};

// The key whose destructor frees an exiting thread's cache. The module
// destructor deletes it under the write lock of clog_tident_rwlock, and a
// thread sets it under the read lock after checking clog_tident_key_live, so a
// set never hits a deleted key. clog_tident_key_live stays false when the key
// or lock couldn't be created.
static ccol__thread_ls_key clog_tident_key;
static ccol__rw_lock clog_tident_rwlock;
static atomic_bool clog_tident_key_live;

// Set by the module destructor. A record after it gets clog_tident_late, one
// identity for the process with no thread part, because no destructor is left
// to free a new cache.
static atomic_bool clog_tident_finalized;
static clog_thread_ident clog_tident_late;
static ccol__once_flag clog_tident_late_once = CCOL__ONCE_INIT;

#ifdef RUNNING_UNIT_TESTS

// How many times the calling thread read its identity from the system.
static __thread unsigned clog_test_tident_fills;

// Gives how many times the calling thread read its identity from the system, so
// a test can check the cache.
unsigned clog__test_thread_identity_fill_count(void) {
  return clog_test_tident_fills;
}

// Gives the calling thread's identity block, or NULL, so a test can check that
// a block is reused and not leaked.
const void *clog__test_thread_identity_block(void) { return clog_tident; }

// Clears the calling thread's identity pointer while the key keeps the block,
// which is what macOS does before key destructors run, so a test can run that
// path anywhere.
void clog__test_forget_thread_identity_pointer(void) { clog_tident = NULL; }

static void clog_tident_fini(void);

// Runs the identity cache's module destructor now, so a test can check the
// records logged after it.
void clog__test_run_identity_fini(void) { clog_tident_fini(); }
#endif

// Marks the calling thread's identity cache stale in a fork child, which has a
// new pid and tid, so the child's records name the child. It takes no lock, so
// its place among other handlers doesn't matter.
static void clog_tident_atfork_child(void) {
  if (clog_tident) clog_tident->valid = false;
}

// Frees an exiting thread's identity cache (arg); it's the destructor of
// clog_tident_key, so a thread that ends doesn't leak its cache.
static void clog_tident_free(void *arg) {
  free(arg);
  clog_tident = NULL;
}

// Registers the identity cache's fork child handler and creates its rwlock and
// key, once. A failed registration only leaves a forked child with the parent's
// identity.
static void clog_tident_register_atfork(void) {
  (void)ccol__at_fork(NULL, NULL, clog_tident_atfork_child);
  if (ccol__rw_lock_init(clog_tident_rwlock) != 0) return;
  if (ccol__thread_ls_key_create(clog_tident_key, clog_tident_free) != 0)
    return;
  atomic_store(&clog_tident_key_live, true);
}

// See CCOL__ATFORK_AT_LOAD in common.h.
#if defined(CCOL__ATFORK_AT_LOAD)
__attribute__((constructor)) static void clog_atfork_at_load(void) {
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
  ccol__call_once(clog_tident_once, clog_tident_register_atfork);
}
#endif

// At unload and exit: Frees the calling thread's cache, which no key destructor
// frees for the thread that ends the process, and deletes the key so no thread
// exiting after a dlclose() calls into unmapped code.
__attribute__((destructor)) static void clog_tident_fini(void) {
  atomic_store(&clog_tident_finalized, true);
  free(clog_tident);
  clog_tident = NULL;
  if (!atomic_load(&clog_tident_key_live)) return;
  ccol__rw_lock_wrlock(clog_tident_rwlock);
  atomic_store(&clog_tident_key_live, false);
  ccol__thread_ls_key_delete(clog_tident_key);
  ccol__rw_lock_unlock(clog_tident_rwlock);
}

// Formats ti->proc, the identity string every record carries, from the other
// fields of ti.
static void clog_tident_format(clog_thread_ident *ti) {
  if (snprintf(ti->proc, sizeof(ti->proc), "%.200s(%d):%.15s(%d)",
               get_progname(), (int)ti->pid, ti->name, (int)ti->tid) < 0)
    ti->proc[0] = '\0';
}

// Fills clog_tident_late, once, for the records logged after the module
// destructor.
static void clog_tident_late_init(void) {
  clog_tident_late.pid = getpid();
  clog_tident_late.tid = 0;
  clog_tident_late.name[0] = '\0';
  if (snprintf(clog_tident_late.proc, sizeof(clog_tident_late.proc),
               "%.200s(%d)", get_progname(), (int)clog_tident_late.pid) < 0)
    clog_tident_late.proc[0] = '\0';
  clog_tident_late.valid = true;
}

// Reads the calling thread's identity from the system and caches it; out of
// line since a thread runs it once. The key's value owns the cache: macOS frees
// __thread storage before key destructors run, so a record logged from another
// destructor finds clog_tident NULL and takes the block back from the key
// instead of leaking it with a new one.
static __attribute__((noinline, cold)) const clog_thread_ident *
clog_tident_fill(void) {
  ccol__call_once(clog_tident_once, clog_tident_register_atfork);
  clog_thread_ident *ti = clog_tident;
  if (!ti && atomic_load(&clog_tident_key_live)) {
    ccol__rw_lock_rdlock(clog_tident_rwlock);
    if (atomic_load(&clog_tident_key_live))
      ti = ccol__thread_ls_get(clog_tident_key);
    ccol__rw_lock_unlock(clog_tident_rwlock);
    if (ti) clog_tident = ti;
  }
  if (!ti && atomic_load(&clog_tident_finalized)) {
    ccol__call_once(clog_tident_late_once, clog_tident_late_init);
    return &clog_tident_late;
  }
  if (!ti) {
    ti = malloc(sizeof(*ti));
    if (!ti) return &clog_tident_unknown;
    if (atomic_load(&clog_tident_key_live)) {
      ccol__rw_lock_rdlock(clog_tident_rwlock);
      if (atomic_load(&clog_tident_key_live))
        (void)ccol__thread_ls_set(clog_tident_key, ti);
      ccol__rw_lock_unlock(clog_tident_rwlock);
    }
    clog_tident = ti;
  }
#ifdef RUNNING_UNIT_TESTS
  clog_test_tident_fills++;
#endif
  ti->pid = getpid();
  ti->tid = get_tid();
  get_thread_name(ti->name, sizeof(ti->name));
  clog_tident_format(ti);
  ti->valid = true;
  return ti;
}

// Gives the calling thread's cached identity for a record, filling the cache
// first when it's missing or stale. The pointer is valid while the thread runs,
// and only that thread may read it.
static inline const clog_thread_ident *clog_tident_get(void) {
  const clog_thread_ident *ti = clog_tident;
  if (__builtin_expect(!ti || !ti->valid, 0)) return clog_tident_fill();
  return ti;
}

// Renames the calling thread (cut to the 15 bytes every platform keeps) and
// updates its identity cache, so its later records carry the new name; a rename
// by other means isn't seen by the cache.
ccol_retval ccol_set_thread_name(const char *thread_name) {
  if (!thread_name || thread_name[0] == '\0') return ccol_invalid_args;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__) || \
    defined(__OpenBSD__)
  char name[CLOG_THREAD_NAME_LIMIT + 1];
  size_t n = strnlen(thread_name, CLOG_THREAD_NAME_LIMIT);
  memcpy(name, thread_name, n);
  name[n] = '\0';
#if defined(__linux__)
  if (prctl(PR_SET_NAME, name) != 0) return ccol_not_permitted;
#else
  if (ccol__set_own_thread_name_np(name) != 0) return ccol_not_permitted;
#endif
  clog_thread_ident *ti = clog_tident;
  if (!ti || !ti->valid) {
    (void)clog_tident_fill();
  } else {
    memcpy(ti->name, name, n + 1);
    clog_tident_format(ti);
  }
  return ccol_success;
#else
  return ccol_not_permitted;
#endif
}

#ifdef RUNNING_UNIT_TESTS

// Each of the next clog_test_write_cap_uses writes takes at most this many
// bytes, so a test can make a short write without a real resource limit.
static _Atomic size_t clog_test_write_cap = 0;
static _Atomic unsigned int clog_test_write_cap_uses = 0;

// Takes one capped write, for the write loops of a test build. Gives the cap,
// or 0 when none is armed.
static size_t clog_test_consume_next_write_cap(void) {
  unsigned int uses = atomic_load(&clog_test_write_cap_uses);
  while (uses > 0) {
    if (atomic_compare_exchange_weak(&clog_test_write_cap_uses, &uses,
                                     uses - 1))
      return atomic_load(&clog_test_write_cap);
  }
  return 0;
}

// Caps each of the next write_count writes at accepted_bytes, so a test can
// make short writes without a real resource limit.
void clog__test_force_short_writes(size_t accepted_bytes,
                                   unsigned int write_count) {
  atomic_store(&clog_test_write_cap, accepted_bytes);
  atomic_store(&clog_test_write_cap_uses, accepted_bytes ? write_count : 0u);
}

// Each of the next clog_test_write_error_uses writes fails with this errno
// before writing a byte.
static _Atomic int clog_test_write_error = 0;
static _Atomic unsigned int clog_test_write_error_uses = 0;

// Takes one forced write error, for the write loops of a test build. Gives the
// errno, or 0 when none is armed.
static int clog_test_consume_next_write_error(void) {
  unsigned int uses = atomic_load(&clog_test_write_error_uses);
  while (uses > 0) {
    if (atomic_compare_exchange_weak(&clog_test_write_error_uses, &uses,
                                     uses - 1))
      return atomic_load(&clog_test_write_error);
  }
  return 0;
}

// Makes each of the next write_count writes fail with err before writing, so a
// test can check how each error class is handled.
void clog__test_force_write_errors(int err, unsigned int write_count) {
  atomic_store(&clog_test_write_error, err);
  atomic_store(&clog_test_write_error_uses, err ? write_count : 0u);
}

// A simulated full output: While clog_test_room_err isn't 0, writes take bytes
// until clog_test_room runs out, and then fail with that errno. After
// clog_test_room_refusals refused writes the output has room again (0 keeps it
// full).
static _Atomic int clog_test_room_err = 0;
static _Atomic size_t clog_test_room = 0;
static _Atomic unsigned int clog_test_room_refusals = 0;

// Simulates an output with room bytes left that fails with err once full, until
// refusals writes have been refused, so a test can reproduce a full disk; err 0
// turns it off.
void clog__test_set_output_room(int err, size_t room, unsigned int refusals) {
  atomic_store(&clog_test_room, room);
  atomic_store(&clog_test_room_refusals, refusals);
  atomic_store(&clog_test_room_err, err);
}

// Applies the simulated full output to a write of len bytes, for the write
// loops of a test build. Gives how many bytes it takes; *err is its errno when
// that's fewer than len, and 0 otherwise.
static size_t clog_test_room_clip(size_t len, int *err) {
  int e = atomic_load(&clog_test_room_err);
  *err = 0;
  if (e == 0) return len;
  size_t room = atomic_load(&clog_test_room);
  for (;;) {
    size_t take = len < room ? len : room;
    if (atomic_compare_exchange_weak(&clog_test_room, &room, room - take)) {
      if (take < len) {
        *err = e;
        unsigned int r = atomic_load(&clog_test_room_refusals);
        if (r == 1) atomic_store(&clog_test_room_err, 0);
        if (r > 0) atomic_store(&clog_test_room_refusals, r - 1);
      }
      return take;
    }
  }
}

// While set, a socket sink is classified as a pipe, the path of a system
// without MSG_NOSIGNAL.
static _Atomic bool clog_test_sockets_as_pipes = false;

// Turns the classification of sockets as pipes on or off, so a test can run the
// path of a system without MSG_NOSIGNAL.
void clog__test_classify_sockets_as_pipes(bool on) {
  atomic_store(&clog_test_sockets_as_pipes, on);
}

// Tells clog_classify_sink() whether to treat a socket as a pipe, which a test
// sets to run the path of a system without MSG_NOSIGNAL.
static bool clog_test_socket_is_pipe(void) {
  return atomic_load(&clog_test_sockets_as_pipes);
}
#else

// Stubs of a non-test build, so the write loops need no #ifdef: None of the
// write hooks does anything.
static inline bool clog_test_socket_is_pipe(void) { return false; }
static inline size_t clog_test_consume_next_write_cap(void) { return 0; }
static inline int clog_test_consume_next_write_error(void) { return 0; }
static inline size_t clog_test_room_clip(size_t len, int *err) {
  *err = 0;
  return len;
}
#endif

// Signal handling of writes. EPIPE on a pipe or socket raises SIGPIPE, and a
// write past RLIMIT_FSIZE raises SIGXFSZ; both kill by default. A socket is
// written with MSG_NOSIGNAL under every policy. Under CLOG_SIGNALS_AUTO the
// async writer and compressor threads keep both signals blocked and consume the
// ones their own writes raise; another thread's first pipe write sets SIGPIPE
// to SIG_IGN, and opening a regular file sets SIGXFSZ to SIG_IGN, each once and
// only while the disposition is SIG_DFL. The open is the right moment for
// SIGXFSZ, because a record ending exactly at the limit makes the next write
// fail with no short write first. Under CLOG_SIGNALS_UNTOUCHED nothing changes
// and each write raises what the kernel raises.
#if defined(MSG_NOSIGNAL)
#define CLOG_HAS_MSG_NOSIGNAL 1
#else
#define CLOG_HAS_MSG_NOSIGNAL 0
#endif

// What a target's fd is. CLOG_SINK_FATAL_BOUND is a flag a CLOG_FATAL call
// adds, under the mutex, to a sink that isn't a regular file, so every later
// write ends by clog_fatal_deadline. PLAIN with that flag is a terminal or
// other device.
typedef enum {
  CLOG_SINK_PLAIN = 0,
  CLOG_SINK_PIPE = 1,
  CLOG_SINK_SOCKET = 2,
  CLOG_SINK_FATAL_BOUND = 4,
} clog_sink_kind;

// The policy clog_set_signal_policy() sets. Read relaxed, since nothing else is
// ordered by it.
static _Atomic int clog_signal_policy_value = CLOG_SIGNALS_AUTO;

// The once of the SIGPIPE ignore, and a flag so later pipe writes skip the
// call.
static ccol__once_flag clog_sigpipe_once = CCOL__ONCE_INIT;
static atomic_bool clog_sigpipe_once_done;

// The same for SIGXFSZ, which the open of a regular file sets.
static ccol__once_flag clog_sigxfsz_once = CCOL__ONCE_INIT;
static atomic_bool clog_sigxfsz_once_done;

// Bits of clog_sig_thread. Only this module's writer and compressor threads
// (CLOG_SIGT_OWN) set them, recording for each signal whether the module
// blocked it or the thread inherited it blocked.
enum {
  CLOG_SIGT_OWN = 1,
  CLOG_SIGT_PIPE_BLOCKED = 2,
  CLOG_SIGT_PIPE_INHERITED = 4,
  CLOG_SIGT_XFSZ_BLOCKED = 8,
  CLOG_SIGT_XFSZ_INHERITED = 16
};

// The calling thread's signal bits, initial-exec where allowed because every
// pipe write reads it.
#if defined(CMEMPOOL_DYNAMIC_TLS) && CMEMPOOL_DYNAMIC_TLS
static __thread unsigned char clog_sig_thread;
#else
static __thread unsigned char clog_sig_thread
    __attribute__((tls_model("initial-exec")));
#endif

// Sets sig to SIG_IGN when it's SIG_DFL, the process-wide step of the AUTO
// signal policy. A handler or ignore the application installed stays, since
// that's the application's choice.
static void clog_ignore_if_default(int sig) {
  struct sigaction cur;
  if (sigaction(sig, NULL, &cur) != 0) return;
  if ((cur.sa_flags & SA_SIGINFO) || cur.sa_handler != SIG_DFL) return;
  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  (void)sigaction(sig, &ign, NULL);
}

// The bodies of the two once flags, which apply the process-wide ignore of
// SIGPIPE and SIGXFSZ.
static void clog_ignore_sigpipe(void) { clog_ignore_if_default(SIGPIPE); }
static void clog_ignore_sigxfsz(void) { clog_ignore_if_default(SIGXFSZ); }

// Runs the SIGXFSZ ignore once and marks it done, so later regular-file opens
// skip the call. Out of line since it's cold.
static __attribute__((noinline, cold)) void clog_ignore_sigxfsz_once(void) {
  ccol__call_once(clog_sigxfsz_once, clog_ignore_sigxfsz);
  atomic_store_explicit(&clog_sigxfsz_once_done, true, memory_order_relaxed);
}

// Applies the SIGXFSZ ignore under CLOG_SIGNALS_AUTO when a logger starts
// writing a regular file (its open, or a rotation's next file), so a write at
// RLIMIT_FSIZE fails with EFBIG instead of killing the process.
static inline void clog_file_sink_prepare(void) {
  if (atomic_load_explicit(&clog_signal_policy_value, memory_order_relaxed) ==
          CLOG_SIGNALS_AUTO &&
      !atomic_load_explicit(&clog_sigxfsz_once_done, memory_order_relaxed))
    clog_ignore_sigxfsz_once();
}

// fd's clog_sink_kind; it also prepares the signal state for a regular file. A
// descriptor fstat() can't read is PLAIN (its writes fail with EBADF). Without
// MSG_NOSIGNAL a socket is a pipe.
static unsigned char clog_classify_sink(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return CLOG_SINK_PLAIN;
  if (S_ISFIFO(st.st_mode)) return CLOG_SINK_PIPE;
  if (S_ISSOCK(st.st_mode))
    return CLOG_HAS_MSG_NOSIGNAL && !clog_test_socket_is_pipe()
               ? CLOG_SINK_SOCKET
               : CLOG_SINK_PIPE;
  if (S_ISREG(st.st_mode)) clog_file_sink_prepare();
  return CLOG_SINK_PLAIN;
}

// Tells whether fd is a message socket (SOCK_DGRAM, SOCK_SEQPACKET, SOCK_RDM),
// which keeps the boundary of each write, so the async writer gives it one
// record per write instead of a batch.
static bool clog_sink_is_message(int fd) {
  int type = 0;
  socklen_t type_len = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0) return false;
  if (type == SOCK_DGRAM || type == SOCK_SEQPACKET) return true;
#if defined(SOCK_RDM)
  if (type == SOCK_RDM) return true;
#endif
  return false;
}

// Changes the calling module thread's mask so the module blocks exactly the
// signals in want (CLOG_SIGT_*_BLOCKED bits), which is how the writer and
// compressor threads follow the policy. It records the result in
// clog_sig_thread.
static __attribute__((noinline, cold)) void clog_thread_set_signal_mask(
    unsigned char want) {
  unsigned char st = clog_sig_thread;
  unsigned char cur = st & (CLOG_SIGT_PIPE_BLOCKED | CLOG_SIGT_XFSZ_BLOCKED);
  unsigned char add = want & (unsigned char)~cur;
  unsigned char drop = cur & (unsigned char)~want;
  sigset_t set;
  if (add != 0) {
    sigemptyset(&set);
    if (add & CLOG_SIGT_PIPE_BLOCKED) sigaddset(&set, SIGPIPE);
    if (add & CLOG_SIGT_XFSZ_BLOCKED) sigaddset(&set, SIGXFSZ);
    if (ccol__thread_sigmask(SIG_BLOCK, &set, NULL) == 0) st |= add;
  }
  if (drop != 0) {
    sigemptyset(&set);
    if (drop & CLOG_SIGT_PIPE_BLOCKED) sigaddset(&set, SIGPIPE);
    if (drop & CLOG_SIGT_XFSZ_BLOCKED) sigaddset(&set, SIGXFSZ);
    if (ccol__thread_sigmask(SIG_UNBLOCK, &set, NULL) == 0)
      st &= (unsigned char)~drop;
  }
  clog_sig_thread = st;
}

// Brings a writer or compressor thread's mask in line with the policy: Both
// signals blocked under AUTO, as inherited under UNTOUCHED. It unblocks only
// what the module blocked, and the module never leaves its own signal pending,
// so an unblock delivers nothing.
static inline void clog_thread_sync_signal_mask(void) {
  unsigned char st = clog_sig_thread;
  unsigned char want = 0;
  if (atomic_load_explicit(&clog_signal_policy_value, memory_order_relaxed) ==
      CLOG_SIGNALS_AUTO) {
    if (!(st & CLOG_SIGT_PIPE_INHERITED)) want |= CLOG_SIGT_PIPE_BLOCKED;
    if (!(st & CLOG_SIGT_XFSZ_INHERITED)) want |= CLOG_SIGT_XFSZ_BLOCKED;
  }
  if ((st & (CLOG_SIGT_PIPE_BLOCKED | CLOG_SIGT_XFSZ_BLOCKED)) == want) return;
  clog_thread_set_signal_mask(want);
}

// Marks the calling thread as one of the module's writer or compressor threads
// in clog_sig_thread and applies the current policy to its mask; the first
// statement of those threads.
static __attribute__((noinline, cold)) void clog_thread_signal_init(void) {
  unsigned char st = CLOG_SIGT_OWN;
  sigset_t cur;
  sigemptyset(&cur);
  if (ccol__thread_sigmask(SIG_BLOCK, NULL, &cur) == 0) {
    if (sigismember(&cur, SIGPIPE) == 1) st |= CLOG_SIGT_PIPE_INHERITED;
    if (sigismember(&cur, SIGXFSZ) == 1) st |= CLOG_SIGT_XFSZ_INHERITED;
  }
  clog_sig_thread = st;
  clog_thread_sync_signal_mask();
}

// Runs the SIGPIPE ignore once and marks it done, so later pipe writes skip the
// call. Out of line since it's cold.
static __attribute__((noinline, cold)) void clog_ignore_sigpipe_once(void) {
  ccol__call_once(clog_sigpipe_once, clog_ignore_sigpipe);
  atomic_store_explicit(&clog_sigpipe_once_done, true, memory_order_relaxed);
}

// Prepares the signal state before every pipe write, so EPIPE can't kill the
// process: A module thread syncs its own mask, any other thread applies the
// process-wide SIGPIPE ignore under AUTO.
static inline void clog_pipe_write_prepare(void) {
  if (clog_sig_thread != 0) {
    clog_thread_sync_signal_mask();
    return;
  }
  if (atomic_load_explicit(&clog_signal_policy_value, memory_order_relaxed) ==
          CLOG_SIGNALS_AUTO &&
      !atomic_load_explicit(&clog_sigpipe_once_done, memory_order_relaxed))
    clog_ignore_sigpipe_once();
}

#if defined(F_SETNOSIGPIPE)

// macOS raises a pipe's SIGPIPE on the whole process, so the writer thread sets
// F_SETNOSIGPIPE on fd for the length of a write under AUTO. begin returns the
// value to restore, or -1 when it changed nothing; end restores it.
static int clog_pipe_nosigpipe_begin(int fd) {
  if (clog_sig_thread == 0 ||
      atomic_load_explicit(&clog_signal_policy_value, memory_order_relaxed) !=
          CLOG_SIGNALS_AUTO)
    return -1;
  int prev = fcntl(fd, F_GETNOSIGPIPE);
  if (prev != 0) return -1;
  return fcntl(fd, F_SETNOSIGPIPE, 1) == 0 ? 0 : -1;
}

static void clog_pipe_nosigpipe_end(int fd, int prev) {
  if (prev < 0) return;
  int saved_errno = errno;
  (void)fcntl(fd, F_SETNOSIGPIPE, prev);
  errno = saved_errno;
}
#endif

// After a write that raised sig for this thread (SIGPIPE after EPIPE, SIGXFSZ
// after EFBIG), takes every pending instance when the module blocked sig here
// (bit is its CLOG_SIGT_*_BLOCKED bit), so a later unblock can't deliver it.
// errno is kept for the caller. macOS and OpenBSD have no sigtimedwait(), so
// there one instance is directed at the thread and taken with sigwait().
static __attribute__((noinline, cold)) void clog_consume_own_signal(
    int sig, unsigned char bit) {
  if (!(clog_sig_thread & bit)) return;
  int saved_errno = errno;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
#if defined(__APPLE__) || defined(__OpenBSD__)
  sigset_t pending;
  sigemptyset(&pending);
  int got;
  if (sigpending(&pending) == 0 && sigismember(&pending, sig) == 1 &&
      ccol__thread_kill(ccol__get_thread_id(), sig) == 0)
    (void)sigwait(&set, &got);
#else
  const struct timespec zero = {0, 0};
  for (;;) {
    int got = sigtimedwait(&set, NULL, &zero);
    if (got == sig || (got < 0 && errno == EINTR)) continue;
    break;
  }
#endif
  errno = saved_errno;
}

// Consume the SIGPIPE or SIGXFSZ the calling module thread's own failed write
// left pending; the two users of clog_consume_own_signal().
static inline void clog_consume_own_sigpipe(void) {
  clog_consume_own_signal(SIGPIPE, CLOG_SIGT_PIPE_BLOCKED);
}
static inline void clog_consume_own_sigxfsz(void) {
  clog_consume_own_signal(SIGXFSZ, CLOG_SIGT_XFSZ_BLOCKED);
}

// Sets the process-wide signal policy, which every later write and module
// thread reads. Returns ccol_invalid_args for a value outside the enum.
ccol_retval clog_set_signal_policy(clog_signal_policy policy) {
  if ((int)policy != CLOG_SIGNALS_AUTO && (int)policy != CLOG_SIGNALS_UNTOUCHED)
    return ccol_invalid_args;
  atomic_store_explicit(&clog_signal_policy_value, (int)policy,
                        memory_order_relaxed);
  return ccol_success;
}

// poll_timeout_ms of a write that may wait without a bound; every write of a
// record's own bytes passes it. A recovery step (draining a split record,
// settling a batch at exit or on the fatal path, a loss marker) passes a bound
// in milliseconds instead. CLOG_SPLIT_RECORD_DRAIN_WAIT_MS is the bound for the
// continuation of a split record.
#define CLOG_WRITE_WAIT_FOREVER (-1)
#define CLOG_SPLIT_RECORD_DRAIN_WAIT_MS 100

// Gives the poll(2) events that wait for room in fd. On OpenBSD a POLLOUT wait
// on an anonymous pipe never wakes when the reader closes, so POLLIN is added
// there to get the POLLHUP.
static short clog_pollout_events(int fd) {
#if defined(__OpenBSD__)
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) && st.st_dev == 0 &&
      st.st_ino == 0)
    return POLLOUT | POLLIN;
#else
  (void)fd;
#endif
  return POLLOUT;
}

#ifdef RUNNING_UNIT_TESTS

// Waits for room on the watched output (named by dev/ino, so the logger's own
// descriptors for the same pipe count too).
static _Atomic size_t clog_test_writable_waits_state = 0;
static _Atomic bool clog_test_writable_watch_set = false;
static _Atomic uint64_t clog_test_writable_watch_dev = 0;
static _Atomic uint64_t clog_test_writable_watch_ino = 0;

// Starts counting, from zero, the waits for room on the output behind fd, so a
// test can see a write wait for a full pipe.
void clog__test_watch_writable(int fd) {
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    atomic_store(&clog_test_writable_watch_set, false);
    return;
  }
  atomic_store(&clog_test_writable_watch_dev, (uint64_t)st.st_dev);
  atomic_store(&clog_test_writable_watch_ino, (uint64_t)st.st_ino);
  atomic_store(&clog_test_writable_watch_set, true);
}

// Gives how many waits for room the watched output has had.
size_t clog__test_writable_waits(void) {
  return atomic_load(&clog_test_writable_waits_state);
}
#endif

// After EAGAIN, waits without a bound until fd is writable; true when the write
// should be tried again, false on an error or hangup without POLLOUT. Out of
// line so the write loop keeps no stack canary.
static __attribute__((noinline)) bool clog_wait_writable(int fd) {
#ifdef RUNNING_UNIT_TESTS
  {
    struct stat st;
    if (atomic_load(&clog_test_writable_watch_set) && fstat(fd, &st) == 0 &&
        (uint64_t)st.st_dev == atomic_load(&clog_test_writable_watch_dev) &&
        (uint64_t)st.st_ino == atomic_load(&clog_test_writable_watch_ino))
      atomic_fetch_add(&clog_test_writable_waits_state, 1);
  }
#endif
  struct pollfd pfd = {
      .fd = fd, .events = clog_pollout_events(fd), .revents = 0};
  int pr = poll(&pfd, 1, -1);
  if (pr < 0 && errno == EINTR) return true;
  return pr > 0 && (pfd.revents & POLLOUT);
}

// Pause after ENOBUFS, which FreeBSD gives for an AF_UNIX datagram socket whose
// receiver is full (poll still reports POLLOUT). *step_us doubles from 50 us to
// 10 ms; limit_ms bounds the pause when it isn't negative.
static __attribute__((noinline, cold)) void clog_enobufs_backoff(
    unsigned *step_us, int limit_ms) {
  unsigned us = *step_us ? *step_us : 50u;
  *step_us = us >= 5000u ? 10000u : us * 2u;
  if (limit_ms >= 0 && (long long)us > (long long)limit_ms * 1000LL)
    us = (unsigned)limit_ms * 1000u;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)us * 1000L};
  nanosleep(&ts, NULL);
}

// Writes len bytes of data to fd with no time bound, as kind says (send() for a
// socket, write() otherwise). It retries after EINTR, a partial write, EAGAIN
// (after a wait) and ENOBUFS (after a backoff). Returns the bytes written,
// which can be fewer than len after an unrecoverable error or a write of 0;
// callers that count bytes for rotation use this, not len. errno holds the
// failure.
static inline __attribute__((always_inline)) size_t
write_all_body(int fd, clog_sink_kind kind, const char *data, size_t len) {
  size_t total = 0;
  int forced_err = clog_test_consume_next_write_error();
  if (forced_err != 0) {
    errno = forced_err;
    return 0;
  }
  size_t forced_cap = clog_test_consume_next_write_cap();
  if (forced_cap > 0 && forced_cap < len) len = forced_cap;
  int room_err;
  len = clog_test_room_clip(len, &room_err);
  unsigned nobufs_step_us = 0;
  while (len > 0) {
    ssize_t w;
#if CLOG_HAS_MSG_NOSIGNAL
    if (kind == CLOG_SINK_SOCKET)
      w = send(fd, data, len, MSG_NOSIGNAL);
    else
#endif
      w = write(fd, data, len);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (kind == CLOG_SINK_SOCKET && errno == ENOBUFS) {
        clog_enobufs_backoff(&nobufs_step_us, -1);
        continue;
      }
      if (kind == CLOG_SINK_PIPE && errno == EPIPE) {
        clog_consume_own_sigpipe();
        break;
      }
      if (kind == CLOG_SINK_PLAIN && errno == EFBIG) {
        clog_consume_own_sigxfsz();
        break;
      }
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && clog_wait_writable(fd))
        continue;
      break;
    }
    if (w == 0) break;
    data += (size_t)w;
    len -= (size_t)w;
    total += (size_t)w;
  }
  if (room_err != 0) errno = room_err;
  return total;
}

// Writes data to a regular file or any other sink that can't raise SIGPIPE, the
// common path of write_all(). Returns the bytes written. A regular file can
// raise SIGXFSZ, which the policy handles.
static __attribute__((noinline)) size_t write_all_plain(int fd,
                                                        const char *data,
                                                        size_t len) {
  return write_all_body(fd, CLOG_SINK_PLAIN, data, len);
}

// Set once a CLOG_FATAL call starts, after clog_fatal_deadline is written. A
// bounded write reads the deadline under the target's mutex.
static _Atomic bool clog_fatal_bound_armed;
static struct timespec clog_fatal_deadline;

static __attribute__((noinline, cold)) size_t
write_all_fatal_bound(clog_shared *sh, const char *data, size_t len);

// Writes data to a pipe or socket sink, or to a sink with
// CLOG_SINK_FATAL_BOUND, handling the signals each raises, and returns the
// bytes written. The flag test sits behind the pipe test, so a pipe doesn't pay
// for it.
static __attribute__((noinline)) size_t write_all_nonplain(clog_shared *sh,
                                                           const char *data,
                                                           size_t len) {
  if (sh->sink_kind == CLOG_SINK_PIPE) {
    clog_pipe_write_prepare();
#if defined(F_SETNOSIGPIPE)
    int nosig = clog_pipe_nosigpipe_begin(sh->fd);
    size_t n = write_all_body(sh->fd, CLOG_SINK_PIPE, data, len);
    clog_pipe_nosigpipe_end(sh->fd, nosig);
    return n;
#else
    return write_all_body(sh->fd, CLOG_SINK_PIPE, data, len);
#endif
  }
  if (__builtin_expect(sh->sink_kind != CLOG_SINK_SOCKET, 0))
    return write_all_fatal_bound(sh, data, len);
  return write_all_body(sh->fd, CLOG_SINK_SOCKET, data, len);
}

// Gives the milliseconds until deadline for a poll(2) of a bounded write,
// rounded up so a wait never ends early, and 0 once it passed.
static int clog_ms_until(const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long long ns = (long long)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
                 (long long)(deadline->tv_nsec - now.tv_nsec);
  if (ns <= 0) return 0;
  long long ms = (ns + 999999LL) / 1000000LL;
  return ms > INT_MAX ? INT_MAX : (int)ms;
}

// Most a write after POLLOUT gives a terminal that couldn't be reopened
// non-blocking; a pseudo-terminal takes that much at once.
#define CLOG_TTY_FALLBACK_CHUNK 128

// Opens the terminal fd again with O_NONBLOCK (through /proc/self/fd, or the
// ttyname_r() name) and returns the new descriptor, or -1. The flag stays on
// the new description, so no other holder of the terminal sees it. A pty master
// is refused, and the device number must match fd's.
static int clog_open_tty_nonblocking(int fd) {
#if defined(TIOCGPTN)
  unsigned int ptn;
  if (ioctl(fd, TIOCGPTN, &ptn) == 0) return -1;
#endif
  const int flags = O_WRONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
  char path[sizeof("/proc/self/fd/") + 3 * sizeof(int)];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  int nfd = open(path, flags);
  if (nfd >= 0) return nfd;
  char name[256];
  struct stat want, got;
  if (ttyname_r(fd, name, sizeof(name)) != 0 || fstat(fd, &want) != 0)
    return -1;
  nfd = open(name, flags);
  if (nfd < 0) return -1;
  if (fstat(nfd, &got) != 0 || got.st_rdev != want.st_rdev) {
    close(nfd);
    return -1;
  }
  return nfd;
}

// Writes data for a recovery step, with the contract of write_all_body() but
// the whole write ends within timeout_ms. A poll timeout alone doesn't bound a
// blocking write, so a socket is written with ccol_send_nb(), a pipe or FIFO
// gets at most PIPE_BUF bytes per write after POLLOUT, and a terminal is
// written through a non-blocking second description. A regular file or block
// device never waits on a reader and takes the ordinary loop. A message socket
// gets each record in one write. When the deadline passes, errno is EAGAIN.
static __attribute__((noinline, cold)) size_t write_all_bounded(
    clog_shared *sh, const char *data, size_t len, int timeout_ms) {
  int fd = sh->fd;
  unsigned char raw_kind = sh->sink_kind;
  unsigned char kind = raw_kind & (unsigned char)~CLOG_SINK_FATAL_BOUND;
  int wfd = fd;
  int own_fd = -1;
  size_t chunk = sh->sink_per_record ? SIZE_MAX : PIPE_BUF;
  if (kind == CLOG_SINK_PLAIN) {
    struct stat st;
    if (fstat(fd, &st) == 0) {
      if (raw_kind == CLOG_SINK_PLAIN &&
          (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)))
        return write_all_plain(fd, data, len);
      if (S_ISCHR(st.st_mode) && isatty(fd)) {
        own_fd = clog_open_tty_nonblocking(fd);
        if (own_fd >= 0) {
          wfd = own_fd;
          chunk = SIZE_MAX;
        } else {
          chunk = CLOG_TTY_FALLBACK_CHUNK;
        }
      }
    }
  }
  int forced_err = clog_test_consume_next_write_error();
  if (forced_err != 0) {
    if (own_fd >= 0) close(own_fd);
    errno = forced_err;
    return 0;
  }
  size_t forced_cap = clog_test_consume_next_write_cap();
  if (forced_cap > 0 && forced_cap < len) len = forced_cap;
  int room_err;
  len = clog_test_room_clip(len, &room_err);
  if (kind == CLOG_SINK_PIPE) clog_pipe_write_prepare();
#if defined(F_SETNOSIGPIPE)
  int nosig = kind == CLOG_SINK_PIPE ? clog_pipe_nosigpipe_begin(wfd) : -1;
#endif

  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += (time_t)(timeout_ms / 1000);
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }

  size_t total = 0;
  unsigned nobufs_step_us = 0;
  while (len > 0) {
    ssize_t w;
#if CLOG_HAS_MSG_NOSIGNAL
    if (kind == CLOG_SINK_SOCKET) {
      w = ccol_send_nb(fd, data, len, MSG_NOSIGNAL);
    } else
#endif
    {
      struct pollfd pfd = {
          .fd = wfd, .events = clog_pollout_events(wfd), .revents = 0};
      int pr = poll(&pfd, 1, clog_ms_until(&deadline));
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pr == 0) {
        errno = EAGAIN;
        break;
      }
      if (pfd.revents & POLLNVAL) {
        errno = EBADF;
        break;
      }
      w = write(wfd, data, len < chunk ? len : chunk);
    }
    if (w < 0) {
      if (errno == EINTR) continue;
      if (kind == CLOG_SINK_PIPE && errno == EPIPE) {
        clog_consume_own_sigpipe();
        break;
      }
      if (kind == CLOG_SINK_SOCKET && errno == ENOBUFS) {
        int left = clog_ms_until(&deadline);
        if (left > 0) {
          clog_enobufs_backoff(&nobufs_step_us, left);
          continue;
        }
        errno = EAGAIN;
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
#if CLOG_HAS_MSG_NOSIGNAL
        if (kind == CLOG_SINK_SOCKET) {
          struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
          int pr = poll(&pfd, 1, clog_ms_until(&deadline));
          if (pr > 0 && (pfd.revents & POLLOUT)) continue;
          if (pr < 0 && errno == EINTR) continue;
          errno = EAGAIN;
          break;
        }
#endif
        if (clog_ms_until(&deadline) > 0) continue;
        errno = EAGAIN;
      }
      break;
    }
    if (w == 0) break;
    data += (size_t)w;
    len -= (size_t)w;
    total += (size_t)w;
  }
#if defined(F_SETNOSIGPIPE)
  clog_pipe_nosigpipe_end(wfd, nosig);
#endif
  if (room_err != 0) errno = room_err;
  if (own_fd >= 0) {
    int saved = errno;
    close(own_fd);
    errno = saved;
  }
  return total;
}

// Writes data to a sink with CLOG_SINK_FATAL_BOUND, bounded by
// clog_fatal_deadline, so a CLOG_FATAL call can't hang on an output that takes
// nothing. Returns the bytes written.
static __attribute__((noinline, cold)) size_t
write_all_fatal_bound(clog_shared *sh, const char *data, size_t len) {
  return write_all_bounded(sh, data, len, clog_ms_until(&clog_fatal_deadline));
}

// Writes to sh->fd; see write_all_body() for the contract. The sink kind picks
// the loop, and a regular file pays one test of a byte next to fd. A bounded
// write goes to write_all_bounded(); callers on a record's path pass the
// constant CLOG_WRITE_WAIT_FOREVER, so that test folds away. Needs sh->mutex.
static inline __attribute__((always_inline)) size_t
write_all(clog_shared *sh, const char *data, size_t len, int poll_timeout_ms) {
  if (poll_timeout_ms != CLOG_WRITE_WAIT_FOREVER)
    return write_all_bounded(sh, data, len, poll_timeout_ms);
  if (__builtin_expect(sh->sink_kind != CLOG_SINK_PLAIN, 0))
    return write_all_nonplain(sh, data, len);
  return write_all_plain(sh->fd, data, len);
}

// Renders a record the library makes itself (head plus detail at level, in
// format fmt) into note, with the lead-in nl in front ("\n" when the bytes on
// disk stop inside a line, else ""), so one write carries both. Returns the
// length; the text ends with a newline. head and detail are this file's own
// text and need no escape.
static size_t clog_note_render(const clog_shared *sh, clog_format fmt,
                               clog_level level, const char *nl,
                               const char *head, const char *detail, char *note,
                               size_t cap) {
  int n;
  if (fmt == CLOG_FMT_JSON) {
    n = snprintf(note, cap, "%s{\"level\":\"%s\",\"msg\":\"%s%s\"}\n", nl,
                 LEVEL_STR[level], head, detail);
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + SYSLOG_SEVERITY[level];
    n = snprintf(note, cap, "%s<%d>1 - - - - - - %s%s\n", nl, pri, head,
                 detail);
  } else {
    n = snprintf(note, cap, "%slevel=%s msg=\"%s%s\"\n", nl, LEVEL_STR[level],
                 head, detail);
  }
  if (n <= 0) return 0;
  if ((size_t)n >= cap) {
    note[cap - 2] = '\n';
    return cap - 1;
  }
  return (size_t)n;
}

// Room for a rendered note, and the head of every loss marker.
#define CLOG_NOTE_CAP 320

#define CLOG_LOSS_HEAD "log record truncated: "

static bool clog_write_error_is_transient(int err);

// Writes what the target owes (see clog_shared.owe_newline) in one write: The
// newline closing an open line, and one marker naming the unreported losses
// (the loss's own text for one, totals for more). A partial write is judged by
// its last byte. It writes nothing while async_buf holds the continuation of a
// split record. A permanent error such as EPIPE ends the debt. Returns true
// when the output stands at the start of a line. Needs sh->mutex.
static __attribute__((noinline, cold)) bool clog_pay_owed(clog_shared *sh,
                                                          clog_format fmt,
                                                          int poll_timeout_ms) {
  if (!(sh->async_partial_record & CLOG_OWED_REPAIR)) return true;
  if (sh->async_partial_record & CLOG_SPLIT_MASK) return !sh->owe_newline;
  if (sh->fd < 0) return !sh->owe_newline;
  char note[CLOG_NOTE_CAP];
  size_t nlen;
  const char *nl = sh->owe_newline ? "\n" : "";
  if (sh->owed_losses == 0) {
    if (!sh->owe_newline) {
      sh->async_partial_record &= (unsigned char)~CLOG_OWED_REPAIR;
      atomic_store_explicit(&sh->owes_repair, false, memory_order_relaxed);
      return true;
    }
    note[0] = '\n';
    nlen = 1;
  } else if (sh->owed_losses == 1) {
    nlen = clog_note_render(sh, fmt, CLOG_ERROR, nl, CLOG_LOSS_HEAD,
                            sh->owed_detail, note, sizeof(note));
  } else {
    char detail[160];
    if (sh->owed_err != 0)
      snprintf(detail, sizeof(detail),
               "%zu earlier losses (records=%zu bytes=%zu): write error %d",
               sh->owed_losses, sh->owed_records, sh->owed_bytes, sh->owed_err);
    else
      snprintf(detail, sizeof(detail),
               "%zu earlier losses (records=%zu bytes=%zu)", sh->owed_losses,
               sh->owed_records, sh->owed_bytes);
    nlen = clog_note_render(sh, fmt, CLOG_ERROR, nl, CLOG_LOSS_HEAD, detail,
                            note, sizeof(note));
  }
  if (nlen == 0) return !sh->owe_newline;
  errno = 0;
  size_t w = write_all(sh, note, nlen, poll_timeout_ms);
  int err = errno;
  if (sh->rotation_enabled) sh->bytes_written += (off_t)w;
  if (w == nlen || !clog_write_error_is_transient(err)) {
    sh->owe_newline = false;
    sh->owed_losses = 0;
    sh->owed_records = 0;
    sh->owed_bytes = 0;
    sh->owed_err = 0;
    sh->async_partial_record &= (unsigned char)~CLOG_OWED_REPAIR;
    atomic_store_explicit(&sh->owes_repair, false, memory_order_relaxed);
    return true;
  }
  if (w > 0) sh->owe_newline = note[w - 1] != '\n';
  if (err != 0) sh->owed_err = err;
  return !sh->owe_newline;
}

static void clog_exit_drain_arm(void);

// Adds a loss to the debt (records, bytes, errno, detail text; open_line when
// the bytes on disk now end inside a line) and tries the repair at once with
// the bound poll_timeout_ms. It arms the exit drain, so a debt is paid at exit
// even when nothing writes again. Needs sh->mutex and clear split bits.
static __attribute__((noinline, cold)) void clog_report_loss(
    clog_shared *sh, clog_format fmt, bool open_line, size_t records,
    size_t bytes, int err, const char *detail, int poll_timeout_ms) {
  if (open_line) sh->owe_newline = true;
  if (sh->owed_losses == 0)
    snprintf(sh->owed_detail, sizeof(sh->owed_detail), "%s", detail);
  sh->owed_losses++;
  sh->owed_records += records;
  sh->owed_bytes += bytes;
  if (err != 0) sh->owed_err = err;
  sh->async_partial_record |= CLOG_OWED_REPAIR;
  atomic_store_explicit(&sh->owes_repair, true, memory_order_relaxed);
  clog_exit_drain_arm();
  (void)clog_pay_owed(sh, fmt, poll_timeout_ms);
}

// Tries the repair the target owes before a record write, and returns false
// when a newline is still owed. The record must not go out then (it would
// continue the fragment); the caller counts it as a loss. Needs sh->mutex.
static __attribute__((noinline, cold)) bool clog_owed_gate(
    clog_shared *sh, clog_format fmt, size_t len, int poll_timeout_ms) {
  if (clog_pay_owed(sh, fmt, poll_timeout_ms)) return true;
  clog_report_loss(sh, fmt, false, 1, len, 0,
                   "a record was not written while a line stayed open",
                   poll_timeout_ms);
  return false;
}

// Writes a record the library makes itself (head plus detail at level), such as
// a WARN about rotation, so the operator sees what the logger did. Like any
// record it waits for an owed repair, and a short write of it is a loss.
// poll_timeout_ms bounds the write. Needs sh->mutex.
static void clog_note_record(clog_shared *sh, clog_format fmt, clog_level level,
                             const char *head, const char *detail,
                             int poll_timeout_ms) {
  char note[CLOG_NOTE_CAP];
  size_t nlen =
      clog_note_render(sh, fmt, level, "", head, detail, note, sizeof(note));
  if (nlen == 0) return;
  if ((sh->async_partial_record & CLOG_OWED_REPAIR) &&
      !clog_owed_gate(sh, fmt, nlen, poll_timeout_ms))
    return;
  size_t w = write_all(sh, note, nlen, poll_timeout_ms);
  int err = errno;
  if (sh->rotation_enabled) sh->bytes_written += (off_t)w;
  if (w < nlen) {
    char d[96];
    snprintf(d, sizeof(d), "%zu of %zu bytes written", w, nlen);
    clog_report_loss(sh, fmt, w > 0 && note[w - 1] != '\n', 1, nlen - w, err, d,
                     poll_timeout_ms);
  }
}

// After a write the kernel took only in part (ENOSPC, RLIMIT_FSIZE, a peer that
// left), reports the loss and closes the record's line unless the accepted part
// ends on a record boundary, so the next record doesn't join it. Needs
// sh->mutex.
static void clog_note_truncated_record(clog_shared *sh, clog_format fmt,
                                       const char *data, size_t written,
                                       size_t len, int err) {
  char detail[96];
  snprintf(detail, sizeof(detail), "%zu of %zu bytes written", written, len);
  clog_report_loss(sh, fmt, written > 0 && data[written - 1] != '\n', 1,
                   len - written, err, detail, CLOG_WRITE_WAIT_FOREVER);
}

// Reports that the continuation of a split record is given up, closing the line
// when the split was mid-line. split is never CLOG_SPLIT_NONE and the caller
// already cleared the split bits. Needs sh->mutex.
static void clog_note_dropped_continuation(clog_shared *sh, clog_format fmt,
                                           unsigned char split, size_t dropped,
                                           int err, int poll_timeout_ms) {
  char detail[96];
  snprintf(detail, sizeof(detail), "%zu undelivered bytes dropped", dropped);
  clog_report_loss(sh, fmt, split == CLOG_SPLIT_MID_LINE, 1, dropped, err,
                   detail, poll_timeout_ms);
}

// Reports that len undelivered batch bytes at data are given up, for the reason
// why. split is where the bytes on disk end (already cleared by the caller).
// Each line that doesn't start with a tab counts as a record, and a leading
// continuation counts for the unfinished record. The caller discards the bytes.
// Needs sh->mutex.
static __attribute__((noinline, cold)) void clog_note_dropped_batch(
    clog_shared *sh, const char *data, size_t len, unsigned char split,
    const char *why, int err, int poll_timeout_ms) {
  size_t records = 0;
  if (split == CLOG_SPLIT_AT_LINE) records++;
  for (size_t i = 0; i < len;) {
    if (data[i] != '\t') records++;
    const char *nl = memchr(data + i, '\n', len - i);
    if (!nl) break;
    i = (size_t)(nl - data) + 1;
  }
  char detail[128];
  snprintf(detail, sizeof(detail),
           "undelivered batch dropped (records=%zu bytes=%zu): %s", records,
           len, why);
  clog_report_loss(sh, sh->format, split == CLOG_SPLIT_MID_LINE, records, len,
                   err, detail, poll_timeout_ms);
}

// Writes one framed record to sh->fd, counts what the kernel took toward
// bytes_written, and repairs the framing after a short write. Every synchronous
// whole-record write goes through here, so none can leave half a record or
// write onto a fragment while a newline is owed. Out of line so its large
// callers keep their shape. Needs sh->mutex.
static __attribute__((noinline)) void clog_write_record(clog_shared *sh,
                                                        clog_format fmt,
                                                        const char *data,
                                                        size_t len) {
  if (__builtin_expect(sh->async_partial_record & CLOG_OWED_REPAIR, 0) &&
      !clog_owed_gate(sh, fmt, len, CLOG_WRITE_WAIT_FOREVER))
    return;
  size_t written = write_all(sh, data, len, CLOG_WRITE_WAIT_FOREVER);
  if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  if (written < len)
    clog_note_truncated_record(sh, fmt, data, written, len, errno);
}

// Appends the timestamp tv to b. It takes a timestamp captured when the record
// was submitted, since an async record is built much later on the writer
// thread. Returns 0 or -1.
static int buf_append_ts_at(clog_buf *b, const struct timeval *tv) {
  struct tm tm;
  gmtime_r(&tv->tv_sec, &tm);
  return buf_appendf(b, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, (long)tv->tv_usec);
}

// Flags of the directory descriptor a rotating logger keeps. It only names the
// directory for the *at() calls, so O_PATH is enough where it exists.
#if defined(O_PATH)
#define CLOG_DIR_OPEN_FLAGS (O_PATH | O_DIRECTORY | O_CLOEXEC)
#else
#define CLOG_DIR_OPEN_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif

// Opens the directory part of path ("." without a '/', "/" right under the
// root) for the directory descriptor a rotating logger keeps. Returns the
// descriptor, or -1 with errno set.
static int clog_open_log_dir(const char *path) {
  const char *slash = strrchr(path, '/');
  if (!slash) return open(".", CLOG_DIR_OPEN_FLAGS);
  size_t len = (size_t)(slash - path);
  if (len == 0) return open("/", CLOG_DIR_OPEN_FLAGS);
  char dir[PATH_MAX];
  if (len >= sizeof(dir)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(dir, path, len);
  dir[len] = '\0';
  return open(dir, CLOG_DIR_OPEN_FLAGS);
}

// Opens a listing of the directory dir_fd names, for the passes over rotated
// files. It uses a descriptor of its own, since closedir() closes what it reads
// and dir_fd must stay open. Returns NULL on failure.
static DIR *clog_opendir_at(int dir_fd) {
  int fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return NULL;
  DIR *d = fdopendir(fd);
  if (!d) close(fd);
  return d;
}

// Gives the part of path after the last '/'.
static const char *path_base(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

// True when the directory holds an entry name of any type, without following a
// link. A name that can't be examined gives false; the step that then uses it
// fails on the same condition.
static bool clog_entry_exists(int dir_fd, const char *name) {
  struct stat st;
  return fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
}

// Removes name only while it still is the file *st describes (same dev/ino, not
// following a link), so we never remove an entry another program put there.
// True when the name no longer holds that file, false when the unlink failed.
static bool clog_unlink_if_same(int dir_fd, const char *name,
                                const struct stat *st) {
  struct stat cur;
  if (fstatat(dir_fd, name, &cur, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT;
  if (cur.st_dev != st->st_dev || cur.st_ino != st->st_ino) return true;
  return unlinkat(dir_fd, name, 0) == 0 || errno == ENOENT;
}

// Which entries are generations of a rotating target. A name that parses as a
// rotated name is one only when the entry (not following a link) is a regular
// file owned by the effective user or by the live file's owner, and its stamp
// isn't above limit. Every other entry is never opened, renamed or deleted, so
// another user of the directory can't steer the logger through links, FIFOs,
// far-future stamps or planted names.
typedef struct {
  uid_t euid;
  uid_t live_uid;
  char limit[CLOG_ROTATION_STAMP_DIGITS];
} clog_gen_filter;

// Tells whether uid may own a generation (the effective user or the live file's
// owner), the owner test of clog_gen_filter.
static bool clog_gen_uid_ok(uid_t uid, uid_t euid, uid_t live_uid) {
  return uid == euid || uid == live_uid;
}

// Parses the 14 stamp digits d (UTC) into *out, so a rotated name can seed the
// time interval. False when the digits name no representable time.
static bool clog_stamp_to_time(const char *d, time_t *out) {
  struct tm tm = {0};
  tm.tm_year = (d[0] - '0') * 1000 + (d[1] - '0') * 100 + (d[2] - '0') * 10 +
               (d[3] - '0') - 1900;
  tm.tm_mon = (d[4] - '0') * 10 + (d[5] - '0') - 1;
  tm.tm_mday = (d[6] - '0') * 10 + (d[7] - '0');
  tm.tm_hour = (d[8] - '0') * 10 + (d[9] - '0');
  tm.tm_min = (d[10] - '0') * 10 + (d[11] - '0');
  tm.tm_sec = (d[12] - '0') * 10 + (d[13] - '0');
  time_t t = timegm(&tm);
  if (t == (time_t)-1) return false;
  *out = t;
  return true;
}

// Writes the 14 stamp digits of time t (UTC) to out, for a rotated name and the
// plausibility limit. A time past what time_t or the year 9999 holds gives the
// highest stamp.
static void clog_time_to_stamp(int64_t t,
                               char out[CLOG_ROTATION_STAMP_DIGITS]) {
  time_t tt = (time_t)t;
  struct tm tm;
  char buf[32];
  if ((int64_t)tt != t || !gmtime_r(&tt, &tm) || tm.tm_year > 9999 - 1900 ||
      tm.tm_year < 0 ||
      strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", &tm) !=
          CLOG_ROTATION_STAMP_DIGITS) {
    memset(out, '9', CLOG_ROTATION_STAMP_DIGITS);
    return;
  }
  memcpy(out, buf, CLOG_ROTATION_STAMP_DIGITS);
}

// Block size of the gzip copy loop.
#define GZ_BUF_SIZE 65536U

#ifdef RUNNING_UNIT_TESTS

// Test hooks that widen and report the window between a compression creating
// its output and finishing it, where no deletion pass may remove that output.
static _Atomic unsigned int clog_test_pending_compress_delay_us = 0;
static _Atomic bool clog_test_gz_dest_opened_state = false;

// Sets how long the next compression pauses after creating its output, and
// clears the opened flag, so a test can race a deletion pass inside that
// window.
void clog__test_set_pending_compress_delay_us(unsigned int delay_us) {
  atomic_store(&clog_test_gz_dest_opened_state, false);
  atomic_store(&clog_test_pending_compress_delay_us, delay_us);
}

// Tells whether a compression has created its output since the delay was set.
bool clog__test_gz_dest_opened(void) {
  bool opened = atomic_load(&clog_test_gz_dest_opened_state);
  return opened;
}

// While the gate is held, every compression pauses after creating its output,
// for at most CLOG_TEST_COMPRESS_HOLD_MAX_US. compressions_finished counts
// every compression that ended. teardown_entered is set by a close about to
// wait for a running compressor thread.
#define CLOG_TEST_COMPRESS_HOLD_MAX_US 30000000U
static _Atomic bool clog_test_compress_hold = false;
static _Atomic size_t clog_test_compressions_finished_state = 0;
static _Atomic bool clog_test_compressor_teardown_entered_state = false;

// Holds or releases compressions at the compression gate, so a test can control
// when one finishes; holding also clears the teardown flag.
void clog__test_hold_compressions(bool hold) {
  if (hold) {
    atomic_store(&clog_test_gz_dest_opened_state, false);
    atomic_store(&clog_test_compressor_teardown_entered_state, false);
  }
  atomic_store(&clog_test_compress_hold, hold);
}

// Tells whether a close has asked a running compressor thread to stop, so a
// test knows the close is waiting for it.
bool clog__test_compressor_teardown_entered(void) {
  return atomic_load(&clog_test_compressor_teardown_entered_state);
}

// How many times an open backed off from a same-file target whose last close is
// still running.
static _Atomic size_t clog_test_closing_target_waits_state = 0;

// Gives how many times an open backed off from a target still closing, so a
// test can check that an open waits for it.
size_t clog__test_closing_target_waits(void) {
  return atomic_load(&clog_test_closing_target_waits_state);
}

// Gives how many compressions have ended, whatever their outcome, so a test can
// wait for one.
size_t clog__test_compressions_finished(void) {
  return atomic_load(&clog_test_compressions_finished_state);
}

// How many upcoming compressions fail their publish and also leave their
// temporary output behind.
static _Atomic unsigned int clog_test_gz_publish_failures = 0;

// Makes the next count compressions fail to publish and keep their temporary
// output, so a test can check that the output is removed later.
void clog__test_force_gz_publish_failures(unsigned int count) {
  atomic_store(&clog_test_gz_publish_failures, count);
}

// Tells whether a forced publish failure is pending, without taking it, so
// gzip_compress_file() in a test build skips the publish.
static bool clog_test_peek_gz_publish_failure(void) {
  return atomic_load(&clog_test_gz_publish_failures) > 0;
}

// Takes one forced publish failure, for gzip_compress_file() in a test build;
// true when one was left.
static bool clog_test_consume_gz_publish_failure(void) {
  unsigned int n = atomic_load(&clog_test_gz_publish_failures);
  while (n > 0) {
    if (atomic_compare_exchange_weak(&clog_test_gz_publish_failures, &n, n - 1))
      return true;
  }
  return false;
}

// A gate like the one above that pauses compression recovery before it lists
// the directory.
#define CLOG_TEST_RECOVER_HOLD_MAX_US 3000000U
static _Atomic bool clog_test_recover_hold = false;
static _Atomic bool clog_test_recover_entered = false;

// Holds or releases compression recovery at its gate, and clears the entered
// flag, so a test can race an open against a recovery.
void clog__test_hold_compression_recovery(bool hold) {
  if (hold) atomic_store(&clog_test_recover_entered, false);
  atomic_store(&clog_test_recover_hold, hold);
}

// Tells whether a recovery has reached its gate.
bool clog__test_compression_recovery_entered(void) {
  return atomic_load(&clog_test_recover_entered);
}

// Waits at the recovery gate while a test holds it, at most
// CLOG_TEST_RECOVER_HOLD_MAX_US, and records that a recovery got there.
static void clog_test_recover_gate(void) {
  if (!atomic_load(&clog_test_recover_hold)) return;
  atomic_store(&clog_test_recover_entered, true);
  for (unsigned int waited = 0; atomic_load(&clog_test_recover_hold) &&
                                waited < CLOG_TEST_RECOVER_HOLD_MAX_US;
       waited += 1000)
    usleep(1000);
}

// Reports whether the writer thread of logger's target had SIGPIPE and SIGXFSZ
// blocked at its last mask sync, so a test can check the signal policy. False
// for a handle that doesn't resolve or a thread that hasn't synced yet.
bool clog__test_writer_blocked_signals(clog logger, bool *pipe_blocked,
                                       bool *xfsz_blocked) {
  struct clogger *lg = clog_resolve(logger);
  if (!lg) return false;
  unsigned bits = atomic_load(&lg->shared->test_writer_blocked_signals);
  clog_resolve_unpin(lg);
  *pipe_blocked = (bits & 1u) != 0;
  *xfsz_blocked = (bits & 2u) != 0;
  return (bits & 4u) != 0;
}

// Gives the timed-wait timeouts of the writer thread of logger's target (0 for
// a handle that doesn't resolve), so a test can check that an idle writer
// doesn't wake. The count is per target, so other loggers don't add to it.
size_t clog__test_writer_timeout_wakeups(clog logger) {
  struct clogger *lg = clog_resolve(logger);
  if (!lg) return 0;
  size_t n = atomic_load(&lg->shared->test_writer_timeout_wakeups);
  clog_resolve_unpin(lg);
  return n;
}

// Gives the interval flushes of the writer thread of logger's target, or 0, so
// a test can check the flush interval under load.
size_t clog__test_writer_interval_flushes(clog logger) {
  struct clogger *lg = clog_resolve(logger);
  if (!lg) return 0;
  size_t n = atomic_load(&lg->shared->test_writer_interval_flushes);
  clog_resolve_unpin(lg);
  return n;
}

// Gives the synchronous records of logger's target that wait for its mutex
// right now, or 0, so a test can line threads up behind the mutex.
size_t clog__test_sync_lock_waiters(clog logger) {
  struct clogger *lg = clog_resolve(logger);
  if (!lg) return 0;
  size_t n = atomic_load(&lg->shared->test_sync_lock_waiters);
  clog_resolve_unpin(lg);
  return n;
}

// Gives the synchronous records of logger's target that reached their write
// under the mutex, or 0.
size_t clog__test_sync_records_locked(clog logger) {
  struct clogger *lg = clog_resolve(logger);
  if (!lg) return 0;
  size_t n = atomic_load(&lg->shared->test_sync_records_locked);
  clog_resolve_unpin(lg);
  return n;
}

// Counts real rotate() attempts, so a test can check the retry backoff.
static _Atomic size_t clog_test_rotate_attempt_count = 0;

// Resets the count of rotate() attempts before a test of the retry backoff.
void clog__test_reset_rotate_attempt_count(void) {
  atomic_store(&clog_test_rotate_attempt_count, 0);
}

// Gives the count of rotate() attempts since the last reset.
size_t clog__test_get_rotate_attempt_count(void) {
  return atomic_load(&clog_test_rotate_attempt_count);
}

// Runs sanitize_syslog_printusascii_field() on raw, so a test can check the
// APP-NAME filter.
void clog__test_sanitize_syslog_appname(const char *raw, char *out,
                                        size_t outsz) {
  sanitize_syslog_printusascii_field(raw, out, outsz);
}

// Escapes raw with buf_append_ctrl_escaped() into out, cut to outsz - 1 bytes,
// so a test can check the escaper; out is empty when the append fails.
void clog__test_append_ctrl_escaped(const char *raw, char *out, size_t outsz) {
  if (outsz == 0) return;
  clog_buf b;
  if (buf_init(&b, NULL) != 0) {
    out[0] = '\0';
    return;
  }
  if (buf_append_ctrl_escaped(&b, raw) != 0) {
    out[0] = '\0';
    buf_free(&b);
    return;
  }
  size_t n = b.len < outsz - 1 ? b.len : outsz - 1;
  memcpy(out, b.data, n);
  out[n] = '\0';
  buf_free(&b);
}

// Backtrace hooks: Force a failed capture, force every syslog frame append to
// fail (armed until cleared), or force a real capture clamped to no frames. See
// test_helpers/clogger.h.
static _Atomic bool clog_test_force_bt_capture_failure = false;

// Makes every backtrace capture fail while force is true, so a test can check
// the missing-backtrace markers on any platform.
void clog__test_force_backtrace_capture_failure(bool force) {
  atomic_store(&clog_test_force_bt_capture_failure, force);
}

static _Atomic bool clog_test_force_all_syslog_bt_frames_fail = false;

// Makes every syslog backtrace frame fail to append while force is true, so a
// test can reach the marker for frames that all failed.
void clog__test_force_all_syslog_backtrace_frames_failure(bool force) {
  atomic_store(&clog_test_force_all_syslog_bt_frames_fail, force);
}

static _Atomic bool clog_test_force_shallow_bt_depth = false;

// Makes every backtrace capture report no frame past the first two while force
// is true, so a test can check that an empty but real capture writes no marker.
void clog__test_force_shallow_backtrace_depth(bool force) {
  atomic_store(&clog_test_force_shallow_bt_depth, force);
}

// Tells whether this build can capture backtraces, so a test of a real capture
// can skip on musl.
bool clog__test_backtrace_supported(void) { return CLOG_HAS_BACKTRACE != 0; }
#endif

// Gives fd, a file this module created with mode 0600, the owner, group and
// permission bits of *like. Owner and group go first and the bits are widened
// after, so the file is never more readable than *like. Like logrotate it's
// best effort: Without the owner it still tries the group, and without the
// group it clears the group bits. Used for a new live file and a .gz.
static void clog_copy_owner_and_mode(int fd, const struct stat *like) {
  mode_t mode = like->st_mode & 0777;
  struct stat cur;
  if (fstat(fd, &cur) == 0 &&
      (cur.st_uid != like->st_uid || cur.st_gid != like->st_gid)) {
    if (fchown(fd, like->st_uid, like->st_gid) != 0 &&
        fchown(fd, (uid_t)-1, like->st_gid) != 0)
      mode &= ~(mode_t)0070;
  }
  if (fchmod(fd, mode) != 0) {
  }
}

#ifdef RUNNING_UNIT_TESTS

// The errno the next fsync() of a compressed output reports instead of the real
// call; it disarms itself.
static _Atomic int clog_test_next_fsync_error = 0;

// Makes the next fsync() of a compressed output report err, so a test can check
// which errors fail a compression.
void clog__test_force_next_fsync_error(int err) {
  atomic_store(&clog_test_next_fsync_error, err);
}
#endif

// fsync()s fd. Returns 0 when the data is on disk, or when the filesystem can't
// do it (EINVAL, ENOSYS), since failing on that would leave every rotated file
// uncompressed there. Any other failure gives -1.
static int clog_sync_output(int fd) {
  int rc;
#ifdef RUNNING_UNIT_TESTS
  int forced = atomic_exchange(&clog_test_next_fsync_error, 0);
  if (forced != 0) {
    errno = forced;
    rc = -1;
  } else
#endif
    rc = fsync(fd);
  if (rc == 0 || errno == EINVAL || errno == ENOSYS) return 0;
  return -1;
}

// A compression writes "<rotated>.gz.tmp" until the output is complete. That
// name never parses as a rotated name, so retention and naming never see it.
#define CLOG_GZ_TMP_SUFFIX ".tmp"

// Renames tmp to dst (both relative to dir_fd) without ever replacing a file at
// dst: RENAME_NOREPLACE, else a hard link plus unlink, else a check and a
// rename (two steps, only on a filesystem without hard links). Returns 0 on
// success.
static int clog_publish_no_replace(int dir_fd, const char *tmp,
                                   const char *dst) {
#if defined(RENAME_NOREPLACE) && defined(__linux__)
  if (renameat2(dir_fd, tmp, dir_fd, dst, RENAME_NOREPLACE) == 0) return 0;
  if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
  if (linkat(dir_fd, tmp, dir_fd, dst, 0) == 0) {
    unlinkat(dir_fd, tmp, 0);
    return 0;
  }
  if (errno == EEXIST) return -1;
  if (clog_entry_exists(dir_fd, dst)) {
    errno = EEXIST;
    return -1;
  }
  return renameat(dir_fd, tmp, dir_fd, dst);
}

// Compresses src into dst (src plus ".gz"), names relative to dir_fd. The
// output goes to dst plus CLOG_GZ_TMP_SUFFIX and is flushed, fsync()ed and
// published without replacing anything, and only then is src unlinked, so a
// stop partway leaves only the temporary file. The output gets src's owner and
// mode. Returns 0, or -1 with src untouched and the temporary removed; when
// that removal fails, *tmp_left gets its dev/ino (st_ino 0 otherwise). A true
// *abort_flag stops at the next block, as a failure. src is read only when the
// descriptor, opened without following links or blocking, is a regular file
// owned by the effective user or live_uid. A leftover temporary is replaced
// only when it's such a file too; O_EXCL creates the new one.
static int gzip_compress_file(int dir_fd, const char *src, const char *dst,
                              _Atomic bool *abort_flag, uid_t live_uid,
                              struct stat *tmp_left) {
  FILE *in = NULL;
  gzFile out = NULL;
  int out_fd = -1;
  int rc = -1;
  bool tmp_created = false;
  struct stat st;
  struct stat tmp_st;

  if (tmp_left) tmp_left->st_ino = 0;
  char tmp[PATH_MAX + 8];
  size_t dlen = strlen(dst);
  if (dlen + sizeof(CLOG_GZ_TMP_SUFFIX) > sizeof(tmp)) return -1;
  memcpy(tmp, dst, dlen);
  memcpy(tmp + dlen, CLOG_GZ_TMP_SUFFIX, sizeof(CLOG_GZ_TMP_SUFFIX));

  int in_fd =
      openat(dir_fd, src, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (in_fd < 0) goto done;
  int in_flags;
  if (fstat(in_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      !clog_gen_uid_ok(st.st_uid, geteuid(), live_uid) ||
      (in_flags = fcntl(in_fd, F_GETFL)) == -1 ||
      fcntl(in_fd, F_SETFL, in_flags & ~O_NONBLOCK) == -1) {
    close(in_fd);
    goto done;
  }
  in = fdopen(in_fd, "rb");
  if (!in) {
    close(in_fd);
    goto done;
  }

  if (clog_entry_exists(dir_fd, dst)) goto done;

  out_fd = openat(dir_fd, tmp,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (out_fd < 0 && errno == EEXIST) {
    struct stat old_tmp;
    if (fstatat(dir_fd, tmp, &old_tmp, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(old_tmp.st_mode) &&
        clog_gen_uid_ok(old_tmp.st_uid, geteuid(), live_uid) &&
        unlinkat(dir_fd, tmp, 0) == 0)
      out_fd =
          openat(dir_fd, tmp,
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  }
  if (out_fd < 0) goto done;
  if (fstat(out_fd, &tmp_st) != 0) {
    (void)unlinkat(dir_fd, tmp, 0);
    goto done;
  }
  tmp_created = true;
  clog_copy_owner_and_mode(out_fd, &st);
  out = gzdopen(out_fd, "wb");
  if (!out) goto done;

#ifdef RUNNING_UNIT_TESTS
  {
    unsigned int delay = atomic_load(&clog_test_pending_compress_delay_us);
    atomic_store(&clog_test_gz_dest_opened_state, true);
    if (delay) usleep(delay);
    for (unsigned int waited = 0; atomic_load(&clog_test_compress_hold) &&
                                  !(abort_flag && atomic_load(abort_flag)) &&
                                  waited < CLOG_TEST_COMPRESS_HOLD_MAX_US;
         waited += 1000)
      usleep(1000);
  }
#endif

  unsigned char buf[GZ_BUF_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (abort_flag && atomic_load_explicit(abort_flag, memory_order_relaxed))
      goto done;
    if (gzwrite(out, buf, (unsigned)n) != (int)n) goto done;
  }
  if (ferror(in)) goto done;

  if (gzflush(out, Z_FINISH) != Z_OK || clog_sync_output(out_fd) != 0)
    goto done;
  int close_rc = gzclose(out);
  out = NULL;
  out_fd = -1;
  if (close_rc != Z_OK) goto done;
#ifdef RUNNING_UNIT_TESTS
  if (clog_test_peek_gz_publish_failure()) goto done;
#endif
  if (clog_publish_no_replace(dir_fd, tmp, dst) != 0) goto done;
  tmp_created = false;
  rc = 0;

done:
  if (in) fclose(in);
  if (out)
    gzclose(out);
  else if (out_fd >= 0)
    close(out_fd);
  if (tmp_left) tmp_left->st_ino = 0;
  if (tmp_created) {
    bool removed;
#ifdef RUNNING_UNIT_TESTS
    if (clog_test_consume_gz_publish_failure())
      removed = false;
    else
#endif
      removed = clog_unlink_if_same(dir_fd, tmp, &tmp_st);
    if (!removed && tmp_left) *tmp_left = tmp_st;
  }
  if (rc == 0) (void)clog_unlink_if_same(dir_fd, src, &st);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&clog_test_compressions_finished_state, 1);
#endif
  return rc;
}

#ifdef RUNNING_UNIT_TESTS

// Run gzip_compress_file() directly on paths relative to the working directory,
// with the effective user (or the given owner) allowed to own src, so a test
// can check the compression alone. True on success.
bool clog__test_gzip_compress_file(const char *src, const char *dst) {
  return gzip_compress_file(AT_FDCWD, src, dst, NULL, geteuid(), NULL) == 0;
}

bool clog__test_gzip_compress_file_for_owner(const char *src, const char *dst,
                                             uid_t live_uid) {
  return gzip_compress_file(AT_FDCWD, src, dst, NULL, live_uid, NULL) == 0;
}
#endif

// Parses what follows "<base>." in a candidate rotated name: 14 stamp digits,
// an optional "_<seq>" (at least 4 digits, zero-padded, no leading zero beyond
// that, never 0) and an optional ".gz". On a match, *stamp points at the digits
// and *seq gets the number (0 without a suffix). Anything else is refused, so
// prune_rotated() never deletes a file that only looks similar.
static bool rotated_name_parse(const char *rest, const char **stamp,
                               unsigned long *seq) {
  for (int i = 0; i < CLOG_ROTATION_STAMP_DIGITS; i++)
    if (!isdigit((unsigned char)rest[i])) return false;
  const char *p = rest + CLOG_ROTATION_STAMP_DIGITS;
  unsigned long s = 0;
  if (*p == '_') {
    p++;
    int nd = 0;
    while (nd <= CLOG_ROTATION_SEQ_MAX_DIGITS && isdigit((unsigned char)p[nd]))
      nd++;
    if (nd < 4 || nd > CLOG_ROTATION_SEQ_MAX_DIGITS) return false;
    if (nd > 4 && p[0] == '0') return false;
    for (int i = 0; i < nd; i++) s = s * 10 + (unsigned long)(p[i] - '0');
    if (s == 0) return false;
    p += nd;
  }
  if (*p != '\0' && strcmp(p, ".gz") != 0) return false;
  *stamp = rest;
  *seq = s;
  return true;
}

// Matches the directory entry d_name against "<base>.<rotated name>" for the
// passes over rotated files; on a match it fills the stamp and sequence through
// rotated_name_parse().
static bool rotated_entry_match(const char *d_name, const char *base,
                                size_t base_len, const char **stamp,
                                unsigned long *seq) {
  if (strncmp(d_name, base, base_len) != 0 || d_name[base_len] != '.')
    return false;
  return rotated_name_parse(d_name + base_len + 1, stamp, seq);
}

// Compares two rotated names by (stamp, sequence), the order of rotations, for
// naming and retention. Returns below, at or above zero.
static int rotated_key_cmp(const char *stamp_a, unsigned long seq_a,
                           const char *stamp_b, unsigned long seq_b) {
  int c = memcmp(stamp_a, stamp_b, CLOG_ROTATION_STAMP_DIGITS);
  if (c != 0) return c;
  return (seq_a > seq_b) - (seq_a < seq_b);
}

// Fills *f for one pass over sh's directory. The stamp limit is the tolerance
// above the later of the clock and rot_last_stamp, so a clock step back never
// turns one of sh's own generations into an ignored entry. Needs sh->mutex or
// sole ownership of sh.
static void clog_gen_filter_init(const clog_shared *sh, clog_gen_filter *f) {
  f->euid = geteuid();
  struct stat st;
  f->live_uid = (sh->fd >= 0 && fstat(sh->fd, &st) == 0) ? st.st_uid : f->euid;
  int64_t ref = (int64_t)clog_wall_now();
  time_t last;
  if (sh->rot_name_known && clog_stamp_to_time(sh->rot_last_stamp, &last) &&
      (int64_t)last > ref)
    ref = (int64_t)last;
  clog_time_to_stamp(ref + CLOG_ROTATION_FUTURE_TOLERANCE_SECS, f->limit);
}

// What a rotated-looking entry is: A generation, something else, or gone.
typedef enum {
  CLOG_ENTRY_GENERATION,
  CLOG_ENTRY_IGNORED,
  CLOG_ENTRY_GONE
} clog_entry_kind;

// Classifies the entry name (its stamp digits at stamp) against filter f, so
// only generations get renamed, compressed or deleted. It never follows a link
// and never opens the entry.
static clog_entry_kind clog_entry_classify(int dir_fd, const char *name,
                                           const char *stamp,
                                           const clog_gen_filter *f) {
  struct stat st;
  if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? CLOG_ENTRY_GONE : CLOG_ENTRY_IGNORED;
  if (!S_ISREG(st.st_mode) || !clog_gen_uid_ok(st.st_uid, f->euid, f->live_uid))
    return CLOG_ENTRY_IGNORED;
  if (memcmp(stamp, f->limit, CLOG_ROTATION_STAMP_DIGITS) > 0)
    return CLOG_ENTRY_IGNORED;
  return CLOG_ENTRY_GENERATION;
}

// Writes the WARN about an entry that looks like a rotated file but isn't a
// generation, rate-limited by CLOG_IGNORED_NOTE_BURST per window. Anyone with
// write access to the directory picks the name, so bytes outside printable
// ASCII, quotes and backslashes become '?' and a long name is cut. Needs
// sh->mutex (or sole ownership) and no half-written record.
static void clog_note_ignored_entry(clog_shared *sh, const char *name) {
  time_t now = time(NULL);
  if (now < sh->ignored_note_window ||
      now - sh->ignored_note_window >= CLOG_IGNORED_NOTE_WINDOW_SECS) {
    sh->ignored_note_window = now;
    sh->ignored_note_count = 0;
  }
  if (sh->ignored_note_count >= CLOG_IGNORED_NOTE_BURST) return;
  sh->ignored_note_count++;
  enum { SHOWN = 96 };
  char safe[SHOWN + 4];
  size_t i = 0;
  for (; name[i] != '\0' && i < SHOWN; i++) {
    unsigned char c = (unsigned char)name[i];
    safe[i] = (c > 0x20 && c < 0x7f && c != '"' && c != '\\') ? (char)c : '?';
  }
  if (name[i] != '\0') {
    memcpy(safe + i, "...", 3);
    i += 3;
  }
  safe[i] = '\0';
  clog_note_record(sh, sh->format, CLOG_WARN,
                   "log rotation: ignored an entry that is not a rotated "
                   "file of this log: ",
                   safe, CLOG_WRITE_WAIT_FOREVER);
}

// Classifies name like clog_entry_classify() and writes the WARN for an ignored
// entry when may_note is true, so an operator learns about entries the logger
// leaves alone.
static clog_entry_kind clog_entry_check(clog_shared *sh, const char *name,
                                        const char *stamp,
                                        const clog_gen_filter *f,
                                        bool may_note) {
  clog_entry_kind k = clog_entry_classify(sh->dir_fd, name, stamp, f);
  if (k == CLOG_ENTRY_IGNORED && may_note) clog_note_ignored_entry(sh, name);
  return k;
}

// Finds sh's newest generation on disk and fills stamp_out and *seq_out; false
// when there's none. rotate() calls it before a target's first rotation, so a
// restart in the same directory numbers its rotations above the old ones. Needs
// sh->mutex (or sole ownership).
static bool rotated_newest_on_disk(clog_shared *sh, bool may_note,
                                   char stamp_out[CLOG_ROTATION_STAMP_DIGITS],
                                   unsigned long *seq_out) {
  const char *base = sh->file_base;
  size_t base_len = strlen(base);
  clog_gen_filter f;
  clog_gen_filter_init(sh, &f);

  DIR *d = clog_opendir_at(sh->dir_fd);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *stamp;
    unsigned long seq;
    if (!rotated_entry_match(e->d_name, base, base_len, &stamp, &seq)) continue;
    if (clog_entry_check(sh, e->d_name, stamp, &f, may_note) !=
        CLOG_ENTRY_GENERATION)
      continue;
    if (!found || rotated_key_cmp(stamp, seq, stamp_out, *seq_out) > 0) {
      memcpy(stamp_out, stamp, CLOG_ROTATION_STAMP_DIGITS);
      *seq_out = seq;
      found = true;
    }
  }
  closedir(d);
  return found;
}

// One rotated file prune_rotated() found; stamp points into path.
typedef struct {
  char *path;
  const char *stamp;
  unsigned long seq;
  bool is_protected;
} clog_rotated_file;

// Compares two rotated files for qsort(), oldest first by (stamp, sequence),
// the order rotations happened in. The mtime can't be used: A compression
// rewrites the file, so a big old generation can finish later than a newer one.
static int cmp_rotated_oldest_first(const void *a, const void *b) {
  const clog_rotated_file *x = (const clog_rotated_file *)a;
  const clog_rotated_file *y = (const clog_rotated_file *)b;
  int c = rotated_key_cmp(x->stamp, x->seq, y->stamp, y->seq);
  if (c != 0) return c;
  return strcmp(x->path, y->path);
}

#ifdef RUNNING_UNIT_TESTS

// Since the last reset: The most generations a pass left on disk (counted from
// a listing of its own), the most compressions in flight a pass saw, and the
// most generations left beyond those kept only for a running compression.
static _Atomic int clog_test_max_rotated_generations = 0;
static _Atomic int clog_test_max_compressions_in_flight = 0;
static _Atomic int clog_test_max_generations_beyond_old_compressions = 0;

// Counts base's distinct generations from a listing of its own, so the test
// maxima don't depend on what the deletion pass listed.
static int clog_test_count_generations_on_disk(int dir_fd, const char *base) {
  size_t base_len = strlen(base);
  DIR *d = clog_opendir_at(dir_fd);
  if (!d) return 0;
  struct {
    char stamp[CLOG_ROTATION_STAMP_DIGITS];
    unsigned long seq;
  } seen[256];
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *stamp;
    unsigned long seq;
    if (!rotated_entry_match(e->d_name, base, base_len, &stamp, &seq)) continue;
    bool dup = false;
    for (int i = 0; i < n && !dup; i++)
      dup = rotated_key_cmp(seen[i].stamp, seen[i].seq, stamp, seq) == 0;
    if (dup || n == (int)(sizeof(seen) / sizeof(seen[0]))) continue;
    memcpy(seen[n].stamp, stamp, CLOG_ROTATION_STAMP_DIGITS);
    seen[n].seq = seq;
    n++;
  }
  closedir(d);
  return n;
}

// Raises the maximum *slot to value when value is larger, for the test maxima
// of the deletion pass.
static void clog_test_raise_to(_Atomic int *slot, int value) {
  int prev = atomic_load(slot);
  while (value > prev && !atomic_compare_exchange_weak(slot, &prev, value)) {
  }
}

// Resets the three maxima the deletion passes record, before a retention test.
void clog__test_reset_max_rotated_generations(void) {
  atomic_store(&clog_test_max_rotated_generations, 0);
  atomic_store(&clog_test_max_compressions_in_flight, 0);
  atomic_store(&clog_test_max_generations_beyond_old_compressions, 0);
}

// Give the three maxima the deletion passes recorded since the last reset, so a
// test can check the retention bound.
int clog__test_get_max_rotated_generations(void) {
  return atomic_load(&clog_test_max_rotated_generations);
}

int clog__test_get_max_compressions_in_flight(void) {
  return atomic_load(&clog_test_max_compressions_in_flight);
}

int clog__test_get_max_generations_beyond_old_compressions(void) {
  return atomic_load(&clog_test_max_generations_beyond_old_compressions);
}
#endif

#ifdef RUNNING_UNIT_TESTS

// While set, a deletion pass doesn't see the running compression's files in its
// listing, as a readdir() racing the compressor can.
static _Atomic bool clog_test_prune_misses_running = false;

// Turns on or off the hiding of the running compression's names from deletion
// passes, so a test can reproduce a readdir() that misses them.
void clog__test_set_prune_misses_running(bool miss) {
  atomic_store(&clog_test_prune_misses_running, miss);
}
#endif

// Deletes sh's oldest generations beyond max_rotated_files. A generation is one
// rotation: One file, or two (source and .gz) while it's being compressed. The
// pass keeps the newest max_keep generations and deletes the older ones, except
// the running compression and just_rotated (the file this rotation just moved
// aside), which are never deleted. When it keeps an old generation only because
// it's being compressed, it sets compress_prune_deferred so the compressor runs
// the pass again afterwards; at most max_keep plus one generations stay. The
// running job's names are added from the job when readdir() missed them, since
// the compressor renames them without the mutex. A pass that can't list
// everything for lack of memory deletes nothing. Only generations count; others
// get a WARN when may_note is true (the compressor's pass passes false, because
// a record can be half written then). Needs sh->mutex.
static void prune_rotated(clog_shared *sh, const char *just_rotated,
                          bool may_note) {
  int max_keep = sh->rotation.max_rotated_files;
  if (max_keep <= 0) return;

  const ccol_memmgmt_procs *m_procs = sh->m_procs;
  const clog_compress_job *running = sh->compress_running;
  const char *base = sh->file_base;
  size_t base_len = strlen(base);

  clog_gen_filter f;
  clog_gen_filter_init(sh, &f);
  DIR *d = clog_opendir_at(sh->dir_fd);
  if (!d) return;

  clog_rotated_file *files = NULL;
  int fc = 0, cap = 0;
  bool complete = true;
  size_t just_rotated_len = just_rotated ? strlen(just_rotated) : 0;
  bool running_listed = false;
  struct dirent *e;

  while ((e = readdir(d)) != NULL) {
    const char *n = e->d_name;
    const char *name_stamp;
    unsigned long seq;
    if (!rotated_entry_match(n, base, base_len, &name_stamp, &seq)) continue;
#ifdef RUNNING_UNIT_TESTS
    if (running && atomic_load(&clog_test_prune_misses_running) &&
        (strcmp(running->src, n) == 0 || strcmp(running->dst, n) == 0))
      continue;
#endif
    if (clog_entry_check(sh, n, name_stamp, &f, may_note) !=
        CLOG_ENTRY_GENERATION)
      continue;

    size_t n_len = strlen(n);
    char *name = ccol__mem_alloc(m_procs, n_len + 1);
    if (!name) {
      complete = false;
      break;
    }
    memcpy(name, n, n_len + 1);

    bool is_protected = running && (strcmp(running->src, name) == 0 ||
                                    strcmp(running->dst, name) == 0);
    if (is_protected) running_listed = true;
    if (just_rotated && (strcmp(name, just_rotated) == 0 ||
                         (strncmp(name, just_rotated, just_rotated_len) == 0 &&
                          strcmp(name + just_rotated_len, ".gz") == 0)))
      is_protected = true;

    if (fc >= cap) {
      int nc = cap ? cap * 2 : 16;
      clog_rotated_file *nf =
          ccol__mem_realloc(m_procs, files, (size_t)nc * sizeof(*files));
      if (!nf) {
        ccol__mem_free(m_procs, name);
        complete = false;
        break;
      }
      files = nf;
      cap = nc;
    }
    files[fc].path = name;
    files[fc].stamp = name + (size_t)(name_stamp - n);
    files[fc].seq = seq;
    files[fc].is_protected = is_protected;
    fc++;
  }
  closedir(d);

  const char *running_stamp;
  unsigned long running_seq;
  if (complete && running && !running_listed &&
      rotated_entry_match(running->src, base, base_len, &running_stamp,
                          &running_seq)) {
    size_t r_len = strlen(running->src);
    char *name = ccol__mem_alloc(m_procs, r_len + 1);
    clog_rotated_file *nf =
        name && fc >= cap
            ? ccol__mem_realloc(m_procs, files,
                                (size_t)(cap ? cap * 2 : 16) * sizeof(*files))
            : files;
    if (!name || !nf) {
      ccol__mem_free(m_procs, name);
      complete = false;
    } else {
      if (fc >= cap) cap = cap ? cap * 2 : 16;
      files = nf;
      memcpy(name, running->src, r_len + 1);
      files[fc].path = name;
      files[fc].stamp = name + (size_t)(running_stamp - running->src);
      files[fc].seq = running_seq;
      files[fc].is_protected = true;
      fc++;
    }
  }

  if (complete) {
    if (fc > 1)
      qsort(files, (size_t)fc, sizeof(*files), cmp_rotated_oldest_first);

    int generations = 0;
    for (int i = 0; i < fc; i++)
      if (i == 0 || rotated_key_cmp(files[i].stamp, files[i].seq,
                                    files[i - 1].stamp, files[i - 1].seq) != 0)
        generations++;

    int delete_below = generations - max_keep;
    int g = 0;
#ifdef RUNNING_UNIT_TESTS
    int old_under_compression = 0;
#endif
    for (int start = 0; start < fc && g < delete_below; g++) {
      int end = start + 1;
      while (end < fc &&
             rotated_key_cmp(files[end].stamp, files[end].seq,
                             files[start].stamp, files[start].seq) == 0)
        end++;
      bool is_protected = false;
      for (int i = start; i < end; i++)
        if (files[i].is_protected) is_protected = true;
      if (!is_protected) {
        for (int i = start; i < end; i++)
          unlinkat(sh->dir_fd, files[i].path, 0);
      } else {
        sh->compress_prune_deferred = true;
#ifdef RUNNING_UNIT_TESTS
        old_under_compression++;
#endif
      }
      start = end;
    }
#ifdef RUNNING_UNIT_TESTS
    int in_flight = running ? 1 : 0;
    for (const clog_compress_job *j = sh->compress_head; j; j = j->next)
      in_flight++;
    int on_disk = clog_test_count_generations_on_disk(sh->dir_fd, base);
    clog_test_raise_to(&clog_test_max_rotated_generations, on_disk);
    clog_test_raise_to(&clog_test_max_compressions_in_flight, in_flight);
    clog_test_raise_to(&clog_test_max_generations_beyond_old_compressions,
                       on_disk - old_under_compression);
#endif
  }

  for (int i = 0; i < fc; i++) ccol__mem_free(m_procs, files[i].path);
  ccol__mem_free(m_procs, files);
}

// True when candidate can't be a new rotation destination: The name exists, or
// (with check_gz) its .gz form exists. The second test matters because a
// finished compression unlinks its source, so the bare name of an earlier
// rotation in the same second looks free while its .gz holds the content.
static bool rotated_name_taken(int dir_fd, const char *candidate,
                               bool check_gz) {
  if (clog_entry_exists(dir_fd, candidate)) return true;
  if (!check_gz) return false;
  char gz[PATH_MAX + 8];
  size_t clen = strlen(candidate);
  memcpy(gz, candidate, clen);
  memcpy(gz + clen, ".gz", 4);
  return clog_entry_exists(dir_fd, gz);
}

// Background compression: A target that compresses owns one compressor thread,
// started by its first rotation that needs one. rotate() only queues a job, so
// no log call ever waits for gzip. Jobs run one at a time, oldest first, which
// is what bounds the files on disk to max_rotated_files plus one. The last
// close stops the thread after the queue is empty; the exit drain waits for it
// within its budget.
//
// The target whose writer or compressor thread the calling thread is. The exit
// drain skips it, since that thread can't serve the drain it would wait for and
// may hold the target's mutex.
static __thread const clog_shared *clog_thread_serves;

// True once there's work for the exit drain: A writer or compressor thread
// started, or a debt was left.
static _Atomic bool clog_exit_drain_armed;

// Arms the exit drain, which has nothing to do until a writer or compressor
// thread starts or a debt is left.
static void clog_exit_drain_arm(void) {
  atomic_store_explicit(&clog_exit_drain_armed, true, memory_order_relaxed);
}

// Tries again to remove the temporary output of each job in left (only while
// the name still holds that file) and frees the jobs that succeed. Returns the
// ones that failed again.
static clog_compress_job *clog_retry_tmp_removals(
    int dir_fd, clog_compress_job *left, const ccol_memmgmt_procs *m_procs) {
  clog_compress_job *keep = NULL;
  while (left) {
    clog_compress_job *job = left;
    left = job->next;
    char tmp[PATH_MAX + 8];
    size_t dlen = strlen(job->dst);
    bool gone = true;
    if (dlen + sizeof(CLOG_GZ_TMP_SUFFIX) <= sizeof(tmp)) {
      memcpy(tmp, job->dst, dlen);
      memcpy(tmp + dlen, CLOG_GZ_TMP_SUFFIX, sizeof(CLOG_GZ_TMP_SUFFIX));
      struct stat st;
      memset(&st, 0, sizeof(st));
      st.st_dev = job->tmp_dev;
      st.st_ino = job->tmp_ino;
      gone = clog_unlink_if_same(dir_fd, tmp, &st);
    }
    if (gone) {
      ccol__mem_free(m_procs, job);
    } else {
      job->next = keep;
      keep = job;
    }
  }
  return keep;
}

// Runs sh's compressor thread (arg is sh). It runs jobs until it's asked to
// stop and the queue is empty, setting compress_running under sh->mutex for
// each. After a job it reruns a deferred deletion pass and wakes the threads
// waiting for the queue. Temporary outputs a failed job couldn't remove are
// retried before each job and at stop; what's left after that the next open's
// recovery removes.
static void *clog_compressor_main(void *arg) {
  clog_shared *sh = (clog_shared *)arg;
  clog_thread_serves = sh;
  clog_thread_signal_init();
  clog_compress_job *left = NULL;
  ccol__mutex_lock(sh->mutex);
  for (;;) {
    while (!sh->compress_head && !sh->compressor_stop)
      ccol__cond_var_wait(sh->compress_cv, sh->mutex);
    clog_compress_job *job = sh->compress_head;
    if (!job) break;
    sh->compress_head = job->next;
    if (!sh->compress_head) sh->compress_tail = NULL;
    job->next = NULL;
    sh->compress_running = job;
    int dir_fd = sh->dir_fd;
    ccol__mutex_unlock(sh->mutex);

    if (left) left = clog_retry_tmp_removals(dir_fd, left, sh->m_procs);
    clog_thread_sync_signal_mask();
    struct stat tmp_left;
    if (gzip_compress_file(dir_fd, job->src, job->dst, &sh->compress_abort,
                           job->live_uid, &tmp_left) != 0)
      clog_consume_own_sigxfsz();

    ccol__mutex_lock(sh->mutex);
    sh->compress_running = NULL;
    if (tmp_left.st_ino != 0) {
      job->tmp_dev = tmp_left.st_dev;
      job->tmp_ino = tmp_left.st_ino;
      job->next = left;
      left = job;
    } else {
      ccol__mem_free(sh->m_procs, job);
    }
    if (sh->compress_prune_deferred) {
      sh->compress_prune_deferred = false;
      prune_rotated(sh, NULL, false);
    }
    ccol__cond_var_broadcast(sh->compress_cv);
  }
  int dir_fd = sh->dir_fd;
  ccol__mutex_unlock(sh->mutex);
  if (left) left = clog_retry_tmp_removals(dir_fd, left, sh->m_procs);
  while (left) {
    clog_compress_job *job = left;
    left = job->next;
    ccol__mem_free(sh->m_procs, job);
  }
  return NULL;
}

// Starts sh's compressor thread. compress_cv uses CLOCK_MONOTONIC so the exit
// drain's bounded wait ignores wall clock steps. Needs sh->mutex and
// compressor_live false. Returns false, with nothing left behind, when the
// thread can't start.
static bool clog_compressor_start(clog_shared *sh) {
  ccol__cond_var_attr ca;
  if (ccol__cond_var_attr_init(ca) != 0) return false;
  int cv_rc = ccol__cond_var_attr_setclock(ca, CLOCK_MONOTONIC);
  if (cv_rc == 0) cv_rc = ccol__cond_var_init_ca(sh->compress_cv, ca);
  ccol__cond_var_attr_destroy(ca);
  if (cv_rc != 0) return false;
  clog_exit_drain_arm();
  sh->compressor_stop = false;
  if (ccol__thread_create(sh->compressor_thread, clog_compressor_main, sh) !=
      0) {
    ccol__cond_var_destroy(sh->compress_cv);
    return false;
  }
  sh->compressor_live = true;
  return true;
}

// Queues the compression of rotated (relative to dir_fd) as one allocation
// holding both names. When the allocation or the thread start fails the file
// stays uncompressed and nothing is lost. live_uid is the live file's owner.
// Needs sh->mutex.
static void clog_compress_enqueue(clog_shared *sh, const char *rotated,
                                  uid_t live_uid) {
  size_t n = strlen(rotated);
  clog_compress_job *job =
      ccol__mem_alloc(sh->m_procs, sizeof(*job) + (n + 1) + (n + 4));
  if (!job) return;
  job->next = NULL;
  job->live_uid = live_uid;
  job->src = (char *)(job + 1);
  memcpy(job->src, rotated, n + 1);
  job->dst = job->src + n + 1;
  memcpy(job->dst, rotated, n);
  memcpy(job->dst + n, ".gz", 4);

  if (!sh->compressor_live && !clog_compressor_start(sh)) {
    ccol__mem_free(sh->m_procs, job);
    return;
  }
  if (sh->compress_tail)
    sh->compress_tail->next = job;
  else
    sh->compress_head = job;
  sh->compress_tail = job;
  ccol__cond_var_broadcast(sh->compress_cv);
}

// Requeues the compressions a stopped process left unfinished. It removes
// leftover temporary outputs, and a .gz next to its still existing source (a
// source is removed only after its .gz is complete, so that .gz isn't known to
// be complete), then queues every uncompressed generation oldest first. It acts
// on generations only and warns about other entries. Needs sh->mutex; sh
// rotates and compresses.
static void clog_recover_compressions(clog_shared *sh) {
  const ccol_memmgmt_procs *m_procs = sh->m_procs;
  const char *base = sh->file_base;
  size_t base_len = strlen(base);
  static const char tmp_tail[] = ".gz" CLOG_GZ_TMP_SUFFIX;
  size_t tmp_tail_len = sizeof(tmp_tail) - 1;
  clog_gen_filter f;
  clog_gen_filter_init(sh, &f);

  DIR *d = clog_opendir_at(sh->dir_fd);
  if (!d) return;
  clog_rotated_file *files = NULL;
  int fc = 0, cap = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *n = e->d_name;
    size_t n_len = strlen(n);
    const char *stamp;
    unsigned long seq;
    if (n_len > tmp_tail_len &&
        strcmp(n + n_len - tmp_tail_len, tmp_tail) == 0) {
      char gz_name[sizeof(e->d_name)];
      size_t gz_len = n_len - (sizeof(CLOG_GZ_TMP_SUFFIX) - 1);
      memcpy(gz_name, n, gz_len);
      gz_name[gz_len] = '\0';
      if (rotated_entry_match(gz_name, base, base_len, &stamp, &seq) &&
          clog_entry_check(sh, n, stamp, &f, true) == CLOG_ENTRY_GENERATION)
        unlinkat(sh->dir_fd, n, 0);
      continue;
    }
    if (!rotated_entry_match(n, base, base_len, &stamp, &seq)) continue;
    if (n_len >= 3 && strcmp(n + n_len - 3, ".gz") == 0) continue;
    if (clog_entry_check(sh, n, stamp, &f, true) != CLOG_ENTRY_GENERATION)
      continue;
    if (fc >= cap) {
      int nc = cap ? cap * 2 : 16;
      clog_rotated_file *nf =
          ccol__mem_realloc(m_procs, files, (size_t)nc * sizeof(*files));
      if (!nf) break;
      files = nf;
      cap = nc;
    }
    char *name = ccol__mem_alloc(m_procs, n_len + 1);
    if (!name) break;
    memcpy(name, n, n_len + 1);
    files[fc].path = name;
    files[fc].stamp = name + (size_t)(stamp - n);
    files[fc].seq = seq;
    files[fc].is_protected = false;
    fc++;
  }
  closedir(d);

  if (fc > 1)
    qsort(files, (size_t)fc, sizeof(*files), cmp_rotated_oldest_first);
  for (int i = 0; i < fc; i++) {
    char gz[PATH_MAX + 8];
    size_t plen = strlen(files[i].path);
    if (plen + 4 <= sizeof(gz)) {
      memcpy(gz, files[i].path, plen);
      memcpy(gz + plen, ".gz", 4);
      clog_entry_kind gz_kind =
          clog_entry_check(sh, gz, files[i].stamp, &f, true);
      if (gz_kind == CLOG_ENTRY_GENERATION) unlinkat(sh->dir_fd, gz, 0);
      if (gz_kind != CLOG_ENTRY_IGNORED)
        clog_compress_enqueue(sh, files[i].path, f.live_uid);
    }
    ccol__mem_free(m_procs, files[i].path);
  }
  ccol__mem_free(m_procs, files);
}

// Stops and joins sh's compressor thread after it finishes its queue. The
// caller must not hold sh->mutex and no handle that can rotate may remain (the
// last close calls it after the writer thread stopped). Does nothing when no
// thread runs.
static void shared_compressor_teardown(clog_shared *sh) {
  ccol__mutex_lock(sh->mutex);
  bool live = sh->compressor_live;
  if (live) {
    sh->compressor_stop = true;
    ccol__cond_var_broadcast(sh->compress_cv);
  }
  ccol__mutex_unlock(sh->mutex);
  if (!live) return;
#ifdef RUNNING_UNIT_TESTS
  atomic_store(&clog_test_compressor_teardown_entered_state, true);
#endif
  ccol__thread_join(sh->compressor_thread);
  ccol__cond_var_destroy(sh->compress_cv);
  sh->compressor_live = false;
}

// Mixes the bits of x with the splitmix64 finalizer, for the fallback of
// clog_random64().
static uint64_t clog_mix64(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

// Gives 64 bits another user can't predict, for the private name of a new live
// file. Without getrandom() it mixes clocks, the pid, a counter and an address;
// the O_EXCL creation stays correct either way.
static uint64_t clog_random64(void) {
  uint64_t r;
  if (ccol_random_bytes(&r, sizeof(r))) return r;
  static _Atomic uint64_t counter;
  struct timespec rt = {0}, mt = {0};
  clock_gettime(CLOCK_REALTIME, &rt);
  clock_gettime(CLOCK_MONOTONIC, &mt);
  r = clog_mix64(atomic_fetch_add(&counter, 1) ^ (uint64_t)getpid());
  r = clog_mix64(r ^
                 ((uint64_t)rt.tv_sec * 1000000000ULL + (uint64_t)rt.tv_nsec));
  r = clog_mix64(r ^
                 ((uint64_t)mt.tv_sec * 1000000000ULL + (uint64_t)mt.tv_nsec));
  r = clog_mix64(r ^ (uint64_t)(uintptr_t)&r);
  return r;
}

// Tells whether name is "<base>.tmp" plus 11 lowercase hex digits, the private
// name of a new live file, so a leftover one can be recognized and removed.
static bool clog_is_live_tmp_name(const char *name, const char *base,
                                  size_t base_len) {
  static const char tag[] = CLOG_ROTATION_TMP_TAG;
  if (strncmp(name, base, base_len) != 0) return false;
  const char *p = name + base_len;
  if (strncmp(p, tag, sizeof(tag) - 1) != 0) return false;
  p += sizeof(tag) - 1;
  for (int i = 0; i < CLOG_ROTATION_TMP_HEX_DIGITS; i++)
    if (!isdigit((unsigned char)p[i]) && !(p[i] >= 'a' && p[i] <= 'f'))
      return false;
  return p[CLOG_ROTATION_TMP_HEX_DIGITS] == '\0';
}

// Creates a rotation's next live file under a private random name (written to
// tmp) with O_EXCL and O_NOFOLLOW, so it's always a new regular file of ours.
// With old set it gets the owner, group and mode of the file it replaces, so a
// restricted log stays restricted; otherwise it's 0644 under the umask. Returns
// the descriptor and fills *st_out, or -1.
static int clog_create_fresh(int dir_fd, const char *base,
                             const struct stat *old, char *tmp, size_t tmp_size,
                             struct stat *st_out) {
  size_t blen = strlen(base);
  if (blen + CLOG_ROTATION_TMP_LEN + 1 > tmp_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t r = clog_random64() &
                 ((UINT64_C(1) << (4 * CLOG_ROTATION_TMP_HEX_DIGITS)) - 1);
    snprintf(tmp, tmp_size, "%s" CLOG_ROTATION_TMP_TAG "%0*llx", base,
             CLOG_ROTATION_TMP_HEX_DIGITS, (unsigned long long)r);
    int fd =
        openat(dir_fd, tmp,
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_APPEND | O_CLOEXEC,
               old ? 0600 : 0644);
    if (fd < 0) {
      if (errno == EEXIST) continue;
      return -1;
    }
    if (old) clog_copy_owner_and_mode(fd, old);
    if (fstat(fd, st_out) != 0) {
      int saved = errno;
      (void)unlinkat(dir_fd, tmp, 0);
      close(fd);
      errno = saved;
      return -1;
    }
    return fd;
  }
  errno = EEXIST;
  return -1;
}

// Removes a private live-file name a rotation left when its process stopped
// before the final rename. Only an empty regular file owned by the effective
// user or the live file's owner, with exactly that name shape, goes. Needs
// sh->mutex (or sole ownership); sh rotates.
static void clog_remove_stale_live_tmps(clog_shared *sh) {
  const char *base = sh->file_base;
  size_t base_len = strlen(base);
  clog_gen_filter f;
  clog_gen_filter_init(sh, &f);
  DIR *d = clog_opendir_at(sh->dir_fd);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!clog_is_live_tmp_name(e->d_name, base, base_len)) continue;
    struct stat st;
    if (fstatat(sh->dir_fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(st.st_mode) && st.st_size == 0 &&
        clog_gen_uid_ok(st.st_uid, f.euid, f.live_uid))
      (void)unlinkat(sh->dir_fd, e->d_name, 0);
  }
  closedir(d);
}

#ifdef RUNNING_UNIT_TESTS

// Called by rotate() while the live name names no file, so a test can plant an
// entry there and check that the rotation replaces it.
static void (*_Atomic clog_test_rotate_window_hook)(int dir_fd,
                                                    const char *base);

// Sets the function rotate() calls while the live name names no file, so a test
// can plant an entry there.
void clog__test_set_rotate_window_hook(void (*hook)(int dir_fd,
                                                    const char *base)) {
  atomic_store(&clog_test_rotate_window_hook, hook);
}
#endif

// Ends the file a rotation retires with a newline, so every generation does. It
// writes the newline, and when the file refuses it (size limit, full disk) cuts
// the file back to its last whole line, scanning backward through a read-only
// open of rotated that must still match old_st's dev/ino. The cut bytes join
// the owed loss. Only a regular file is handled. True when the file ends with a
// newline, is empty, or can't be reached by name. Needs sh->mutex.
static __attribute__((noinline, cold)) bool clog_close_retired_line(
    clog_shared *sh, int old_fd, const struct stat *old_st,
    const char *rotated) {
  if (!S_ISREG(old_st->st_mode)) return false;
  ssize_t w;
  do {
    w = write(old_fd, "\n", 1);
  } while (w < 0 && errno == EINTR);
  if (w == 1) return true;
  if (w < 0 && errno == EFBIG) clog_consume_own_sigxfsz();
  if (rotated == NULL) return true;
  int rfd = openat(sh->dir_fd, rotated,
                   O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
  if (rfd < 0) return false;
  struct stat st;
  if (fstat(rfd, &st) != 0 || st.st_dev != old_st->st_dev ||
      st.st_ino != old_st->st_ino) {
    close(rfd);
    return false;
  }
  off_t end = st.st_size;
  off_t pos = end;
  off_t cut = 0;
  char block[4096];
  bool read_ok = true;
  while (pos > 0) {
    size_t n = pos < (off_t)sizeof(block) ? (size_t)pos : sizeof(block);
    pos -= (off_t)n;
    ssize_t r;
    do {
      r = pread(rfd, block, n, pos);
    } while (r < 0 && errno == EINTR);
    if (r != (ssize_t)n) {
      read_ok = false;
      break;
    }
    size_t k = n;
    while (k > 0 && block[k - 1] != '\n') k--;
    if (k > 0) {
      cut = pos + (off_t)k;
      break;
    }
  }
  close(rfd);
  if (!read_ok) return false;
  if (cut == end) return true;
  int rv;
  do {
    rv = ftruncate(old_fd, cut);
  } while (rv != 0 && errno == EINTR);
  if (rv != 0) return false;
  sh->owed_bytes += (size_t)(end - cut);
  return true;
}

// Rotates the live file, for the size and time rotation checks; all names are
// relative to sh->dir_fd. It creates the next live file under a private name,
// picks a destination whose (stamp, sequence) sorts above every earlier name (a
// clock step back keeps the newest stamp and continues its sequence, so a freed
// name never comes back), renames the live file there without replacing
// anything, renames the new file onto the live name and only then switches fds.
// It never opens the live name or a rotated name, so an entry planted at the
// live name in between is simply replaced. A failed step puts the original name
// back and writes continue in the old file; ENOENT (the file was deleted)
// counts as a clean slate. After success it closes the old file's open line,
// runs the deletion pass and queues the compression (or warns when the name is
// too long for one). Needs sh->mutex. Returns 0 or -1.
static int rotate(clog_shared *sh) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&clog_test_rotate_attempt_count, 1);
#endif

  if (sh->dir_fd < 0 || sh->fd < 0) {
    return 0;
  }
  if (clog_test_consume_rotate_failure()) return -1;

  time_t now = clog_wall_now();
  struct tm tm;
  gmtime_r(&now, &tm);

  size_t plen = strlen(sh->file_base);
  char rotated[PATH_MAX];

  if (plen + CLOG_ROTATION_FMT_LEN + CLOG_ROTATION_EXTRA + 1 >
      sizeof(rotated)) {
    return -1;
  }

  memcpy(rotated, sh->file_base, plen);
  size_t slen =
      strftime(rotated + plen, sizeof(rotated) - plen, CLOG_ROTATION_FMT, &tm);
  if (slen != CLOG_ROTATION_FMT_LEN) {
    return -1;
  }
  char *stamp = rotated + plen + 1;

  if (!sh->rot_name_known)
    sh->rot_name_known =
        rotated_newest_on_disk(sh, true, sh->rot_last_stamp, &sh->rot_last_seq);
  unsigned long seq = 0;
  if (sh->rot_name_known &&
      memcmp(stamp, sh->rot_last_stamp, CLOG_ROTATION_STAMP_DIGITS) <= 0) {
    memcpy(stamp, sh->rot_last_stamp, CLOG_ROTATION_STAMP_DIGITS);
    seq = sh->rot_last_seq + 1;
  }

  struct stat old_st;
  bool have_old_st = fstat(sh->fd, &old_st) == 0;
  char tmp[PATH_MAX];
  struct stat new_st;
  int new_fd =
      clog_create_fresh(sh->dir_fd, sh->file_base, have_old_st ? &old_st : NULL,
                        tmp, sizeof(tmp), &new_st);
  if (new_fd < 0) return -1;

  bool check_gz = sh->rotation.compress_rotated;
  size_t base = plen + slen;
  int rename_rv = -1;
  int rename_errno = 0;
  for (;; seq++) {
    if (seq > CLOG_ROTATION_SEQ_LIMIT) {
      rename_errno = EEXIST;
      break;
    }
    rotated[base] = '\0';
    if (seq > 0 &&
        snprintf(rotated + base, sizeof(rotated) - base, "_%04lu", seq) < 0) {
      rename_errno = EINVAL;
      break;
    }
    if (rotated_name_taken(sh->dir_fd, rotated, check_gz)) continue;
    rename_rv = clog_publish_no_replace(sh->dir_fd, sh->file_base, rotated);
    rename_errno = errno;
    if (rename_rv == 0 || rename_errno != EEXIST) break;
  }
  if (rename_rv != 0 && rename_errno != ENOENT) {
    (void)clog_unlink_if_same(sh->dir_fd, tmp, &new_st);
    close(new_fd);
    return -1;
  }
  bool did_rename = (rename_rv == 0);

#ifdef RUNNING_UNIT_TESTS
  {
    void (*hook)(int, const char *) =
        atomic_load(&clog_test_rotate_window_hook);
    if (hook) hook(sh->dir_fd, sh->file_base);
  }
#endif

  if (renameat(sh->dir_fd, tmp, sh->dir_fd, sh->file_base) != 0) {
    if (did_rename)
      (void)clog_publish_no_replace(sh->dir_fd, rotated, sh->file_base);
    (void)clog_unlink_if_same(sh->dir_fd, tmp, &new_st);
    close(new_fd);
    return -1;
  }

  int old_fd = sh->fd;
  bool old_line_open = sh->owe_newline;
  sh->fd = new_fd;
  sh->sink_kind = clog_classify_sink(new_fd);
  sh->sink_per_record = clog_sink_is_message(new_fd);
  sh->bytes_written = 0;
  sh->last_rotation = now;
  if (old_line_open) sh->owe_newline = false;
  memcpy(sh->rot_last_stamp, stamp, CLOG_ROTATION_STAMP_DIGITS);
  sh->rot_last_seq = seq;
  sh->rot_name_known = true;

  if (sh->rotation.max_rotated_files > 0) {
    prune_rotated(sh, did_rename ? rotated : NULL, true);
  }
  if (old_line_open &&
      !(have_old_st && clog_close_retired_line(sh, old_fd, &old_st,
                                               did_rename ? rotated : NULL))) {
    sh->owe_newline = true;
  }
  if (!(sh->owe_newline || sh->owed_losses > 0)) {
    sh->async_partial_record &= (unsigned char)~CLOG_OWED_REPAIR;
    atomic_store_explicit(&sh->owes_repair, false, memory_order_relaxed);
  }
  close(old_fd);

  if (did_rename && sh->rotation.compress_rotated) {
    long name_max = fpathconf(sh->dir_fd, _PC_NAME_MAX);
    size_t gz_tmp_len = strlen(rotated) + sizeof(".gz" CLOG_GZ_TMP_SUFFIX) - 1;
    if (name_max > 0 && gz_tmp_len > (size_t)name_max)
      clog_note_record(sh, sh->format, CLOG_WARN,
                       "log rotation: a rotated file stays uncompressed: ",
                       "its compressed name exceeds the name length limit "
                       "of the directory",
                       CLOG_WRITE_WAIT_FOREVER);
    else
      clog_compress_enqueue(sh, rotated, new_st.st_uid);
  }

  return 0;
}

// True when the time interval is over at now. The difference is taken in 64
// bits (time_t can be 32), compared in microseconds, and a negative one (a
// clock step back) is never due.
static inline bool clog_time_rotation_elapsed(const clog_shared *sh,
                                              time_t now) {
  int64_t since = (int64_t)now - (int64_t)sh->last_rotation;
  return since >= 0 && ((uint64_t)since >= UINT64_MAX / UINT64_C(1000000) ||
                        (uint64_t)since * UINT64_C(1000000) >=
                            sh->rotation.rotation_interval_us);
}

// Tells whether the retry backoff allows a rotation at now, so a failing
// rotation isn't retried on every write. A backoff ending more than one backoff
// period ahead was set before a clock step back and doesn't count.
static inline bool clog_rotate_retry_allowed(const clog_shared *sh,
                                             time_t now) {
  int64_t left = (int64_t)sh->rotate_retry_after - (int64_t)now;
  return left <= 0 || left > (int64_t)CLOG_ROTATE_RETRY_BACKOFF_SECS;
}

// The time rotation check every path runs before writing a record. It waits
// while a split record's halves would land in two files, restarts the interval
// when the clock stepped back before it, and sets the backoff after a failed
// rotation. Needs sh->mutex.
static void clog_time_rotate_if_due(clog_shared *sh) {
  if (!(sh->rotation_enabled && sh->rotation.time_rotation_enabled)) return;
  if (sh->async_partial_record & CLOG_SPLIT_MASK) return;
  time_t now = clog_wall_now();
  if (now < sh->last_rotation) {
    sh->last_rotation = now;
    return;
  }
  if (clog_time_rotation_elapsed(sh, now) &&
      clog_rotate_retry_allowed(sh, now)) {
    int rrv = rotate(sh);
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

// The size rotation check every path runs after a write, once bytes_written
// covers it. Like the time check it waits while a record is split; the flush
// that rejoins it runs the check again. Needs sh->mutex.
static void clog_size_rotate_if_due(clog_shared *sh) {
  if (!(sh->rotation_enabled && sh->rotation.size_rotation_enabled &&
        (int64_t)sh->bytes_written >= sh->rotation.max_file_size))
    return;
  if (sh->async_partial_record & CLOG_SPLIT_MASK) return;
  time_t now = clog_wall_now();
  if (clog_rotate_retry_allowed(sh, now)) {
    int rrv = rotate(sh);
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

// Tells whether a size or time rotation is due now and the backoff allows it,
// ignoring the split bits; a flush uses it to decide whether to give up a
// continuation so the rotation can run. Needs sh->mutex.
static bool clog_rotation_due(const clog_shared *sh) {
  if (!sh->rotation_enabled) return false;
  time_t now = clog_wall_now();
  if (!clog_rotate_retry_allowed(sh, now)) return false;
  if (sh->rotation.size_rotation_enabled &&
      (int64_t)sh->bytes_written >= sh->rotation.max_file_size)
    return true;
  return sh->rotation.time_rotation_enabled &&
         clog_time_rotation_elapsed(sh, now);
}

// Frames 0 and 1 are capture_backtrace and clog__write; frame 2 is the caller.
#define CLOG_BT_INITIAL_FRAME 2

// Captures the calling thread's backtrace for a record that asks for one. It's
// noinline and clog__write calls it directly on every path, which is what keeps
// CLOG_BT_INITIAL_FRAME right. On success it stores the backtrace_symbols()
// array in *out_syms and the depth in *out_depth and returns true. It returns
// false and leaves both alone without backtrace support or when
// backtrace_symbols() fails; callers treat that as no frames. The test hooks
// can force a failure or a capture with no user frame.
static bool __attribute__((noinline)) capture_backtrace(char ***out_syms,
                                                        int *out_depth) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&clog_test_force_bt_capture_failure)) return false;
#endif
#if CLOG_HAS_BACKTRACE
  void *ptrs[CLOG_BACKTRACE_DEPTH];
  int depth = backtrace(ptrs, CLOG_BACKTRACE_DEPTH);
  char **syms = backtrace_symbols(ptrs, depth);
  if (!syms) return false;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&clog_test_force_shallow_bt_depth) &&
      depth > CLOG_BT_INITIAL_FRAME)
    depth = CLOG_BT_INITIAL_FRAME;
#endif
  *out_syms = syms;
  *out_depth = depth;
  return true;
#else
  (void)out_syms;
  (void)out_depth;
  return false;
#endif
}

// The logfmt line written instead of the frames when no backtrace could be
// captured, so a reader can tell a missing backtrace from one nobody asked for.
// BT_JSON_ERROR_MARKER does the same for JSON.
static const char BT_LINE_ERROR_MARKER[] = "\t#error backtrace unavailable\n";

// Appends the frames as tab-indented continuation lines after a logfmt record,
// so the backtrace lands in the same write as the record. Each frame goes
// through the control escaper and is rolled back when it doesn't fit, so later
// shorter frames still get in. A NULL syms appends BT_LINE_ERROR_MARKER.
// Returns true when out shows the backtrace accurately: Some frames went in, no
// frame existed (depth at most CLOG_BT_INITIAL_FRAME), or the marker stands in
// for frames that all failed to fit. False only when not even the marker fits.
static bool emit_backtrace_lines(clog_buf *out, char *const *syms, int depth) {
  if (!syms)
    return buf_append(out, BT_LINE_ERROR_MARKER,
                      sizeof(BT_LINE_ERROR_MARKER) - 1) == 0;

  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    size_t entry_start = out->len;
    bool ok = buf_appendf(out, "\t#%d ", i - CLOG_BT_INITIAL_FRAME) == 0;
    ok = ok && buf_append_ctrl_escaped(out, syms[i]) == 0;
    ok = ok && buf_append(out, "\n", 1) == 0;
    if (!ok) {
      out->len = entry_start;
      continue;
    }
    any_frame_written = true;
  }
  if (any_frame_written || depth <= CLOG_BT_INITIAL_FRAME) return true;

  return buf_append(out, BT_LINE_ERROR_MARKER,
                    sizeof(BT_LINE_ERROR_MARKER) - 1) == 0;
}

// The JSON key written instead of the "bt" array when the backtrace can't go
// into the record, so a reader can tell it was left out and the record itself
// isn't replaced by the oversized-record fallback.
static const char BT_JSON_ERROR_MARKER[] = ",\"bt_error\":\"unavailable\"";

// Appends ,"bt":["#0 sym",...] while the JSON object is still open, because
// JSON carries its backtrace inside the record. A frame that doesn't fit is
// skipped and later ones still tried. An empty array is written only when no
// frame existed; a NULL syms, or real frames none of which fit, give
// BT_JSON_ERROR_MARKER instead. Returns true when the record can be closed as
// it is, false when not even the marker fits and the caller must fall back.
static bool emit_backtrace_json(clog_buf *b, char *const *syms, int depth) {
  if (!syms)
    return buf_append(b, BT_JSON_ERROR_MARKER,
                      sizeof(BT_JSON_ERROR_MARKER) - 1) == 0;

  size_t bt_start = b->len;
  if (buf_append(b, ",\"bt\":[", 7) != 0) {
    return buf_append(b, BT_JSON_ERROR_MARKER,
                      sizeof(BT_JSON_ERROR_MARKER) - 1) == 0;
  }

  bool first = true;
  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    size_t entry_start = b->len;
    char prefix[20];
    int pl =
        snprintf(prefix, sizeof(prefix), "#%d ", i - CLOG_BT_INITIAL_FRAME);
    if ((!first && buf_append(b, ",", 1) != 0) || buf_append(b, "\"", 1) != 0 ||
        (pl > 0 && buf_append(b, prefix, (size_t)pl) != 0) ||
        buf_append_json_content(b, syms[i]) != 0 ||
        buf_append(b, "\"", 1) != 0) {
      b->len = entry_start;
      continue;
    }
    first = false;
    any_frame_written = true;
  }

  bool array_content_is_accurate =
      any_frame_written || depth <= CLOG_BT_INITIAL_FRAME;
  if (array_content_is_accurate && buf_append(b, "]", 1) == 0) return true;

  b->len = bt_start;
  return buf_append(b, BT_JSON_ERROR_MARKER,
                    sizeof(BT_JSON_ERROR_MARKER) - 1) == 0;
}

// Gives the HOSTNAME field of sh's syslog records, the cached name or "-" when
// it's empty, so the record headers and the backtrace lines agree.
static inline const char *clog_syslog_hostname(const clog_shared *sh) {
  return sh->syslog_hostname[0] ? sh->syslog_hostname : "-";
}

// Writes the one syslog record that says a requested backtrace is missing, used
// by emit_backtrace_syslog_lines() for a failed capture and for frames that all
// failed to append. It builds the record in out, and when not even that fits it
// writes a fixed line from a stack buffer straight to the fd, so the loss is
// never silent. out is left reset on every path, since out can be the async
// batch and stale bytes there would be written again. Needs sh->mutex.
static void write_backtrace_unavailable_syslog_marker(
    clog_buf *out, clog_shared *sh, clog_level level,
    const struct timeval *ts) {
  int pri = (int)sh->syslog_facility * 8 + SYSLOG_SEVERITY[level];
  const char *hostname = clog_syslog_hostname(sh);
  const char *appname = sh->syslog_appname;
  const char *msgid = LEVEL_STR[level];

  buf_reset(out);
  bool ok = buf_appendf(out, "<%d>1 ", pri) == 0;
  ok = ok && buf_append_ts_at(out, ts) == 0;
  ok = ok &&
       buf_appendf(out,
                   " %s %s %d %s - #error backtrace "
                   "unavailable\n",
                   hostname, appname, (int)clog_tident_get()->pid, msgid) == 0;
  if (ok) {
    clog_write_record(sh, CLOG_FMT_SYSLOG, out->data, out->len);
  } else {
    struct tm tm;
    gmtime_r(&ts->tv_sec, &tm);
    char fallback[512];
    int n = snprintf(fallback, sizeof(fallback),
                     "<%d>1 %04d-%02d-%02dT%02d:%02d:%02d.%06ldZ %s %s %d %s - "
                     "#error backtrace unavailable\n",
                     pri, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (long)ts->tv_usec,
                     hostname, appname, (int)clog_tident_get()->pid, msgid);
    if (n > 0) {
      size_t len =
          (size_t)n < sizeof(fallback) ? (size_t)n : sizeof(fallback) - 1;
      clog_write_record(sh, CLOG_FMT_SYSLOG, fallback, len);
    }
  }
  buf_reset(out);
}

// Writes the frames of a syslog record as separate RFC 5424 messages, one write
// each, never batched, because one write is one datagram. out is scratch space,
// reset before each frame and left reset at the end; a frame whose appends fail
// partway is skipped rather than written in part. ts is the primary record's
// own timestamp, captured at submission. A NULL syms, or real frames that all
// failed, write the "backtrace unavailable" marker; a capture with no user
// frame writes nothing. Needs sh->mutex.
static void emit_backtrace_syslog_lines(clog_buf *out, clog_shared *sh,
                                        clog_level level,
                                        const struct timeval *ts,
                                        char *const *syms, int depth) {
  int pri = (int)sh->syslog_facility * 8 + SYSLOG_SEVERITY[level];
  const char *hostname = clog_syslog_hostname(sh);
  const char *appname = sh->syslog_appname;
  const char *msgid = LEVEL_STR[level];

  if (!syms) {
    write_backtrace_unavailable_syslog_marker(out, sh, level, ts);
    return;
  }

  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    buf_reset(out);
    bool ok = buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && buf_append_ts_at(out, ts) == 0;
    ok = ok && buf_appendf(out, " %s %s %d %s - #%d ", hostname, appname,
                           (int)clog_tident_get()->pid, msgid,
                           i - CLOG_BT_INITIAL_FRAME) == 0;
    ok = ok && buf_append_ctrl_escaped(out, syms[i]) == 0;
    ok = ok && buf_append(out, "\n", 1) == 0;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&clog_test_force_all_syslog_bt_frames_fail)) ok = false;
#endif
    if (!ok) continue;
    clog_write_record(sh, CLOG_FMT_SYSLOG, out->data, out->len);
    any_frame_written = true;
  }
  buf_reset(out);

  if (!any_frame_written && depth > CLOG_BT_INITIAL_FRAME) {
    write_backtrace_unavailable_syslog_marker(out, sh, level, ts);
  }
}

#ifdef RUNNING_UNIT_TESTS

// Runs the failed-capture branch of emit_backtrace_syslog_lines() against
// logger's target under its mutex, so a test can reach its last-resort
// fallback.
void clog__test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                         clog_level level) {
  struct clogger *lg = clog_resolve(logger);
  if (!lg) return;
  struct timeval ts;
  gettimeofday(&ts, NULL);
  ccol__mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, NULL, 0);
  ccol__mutex_unlock(lg->shared->mutex);
  clog_resolve_unpin(lg);
}
#endif

// What an async queue message carries: A record job, a flush request, or a wake
// (a plain flush with nothing attached, see clog_wake_writer()).
typedef enum {
  CLOG_ASYNC_MSG_JOB,
  CLOG_ASYNC_MSG_FLUSH,
  CLOG_ASYNC_MSG_WAKE
} clog_async_msg_kind;

// One field of a job's snapshot, as offsets into field_pool. Offsets, because
// the pool can move while snapshot_fields() builds it.
typedef struct {
  size_t key_offset;
  size_t value_offset;
} clog_field_view;

// One record for the writer thread, complete in itself: Everything
// clog_build_record() needs, captured by the submitting thread at submission,
// since the writer may build it much later on another thread.
typedef struct clog_async_job {
  clog_level level;
  const char *file;
  int line;
  const char *func;
  // Timestamp and identity of the submitting thread, captured at submission.
  bool with_backtrace;
  struct timeval ts;
  char proc_val[CLOG_PROC_VAL_LEN];

  // The message: msg points at msg_inline, or at msg_heap when it spilled.
  char msg_inline[256];
  char *msg_heap;
  const char *msg;

  // The field snapshot: field_count views into field_pool.
  // fields_snapshot_failed is true only when the snapshot couldn't be taken,
  // which differs from a logger with no fields.
  clog_field_view *fields;
  size_t field_count;
  ccol_growbuf field_pool;
  bool fields_snapshot_failed;

  // The backtrace captured on the calling thread.
  char **bt_syms;
  int bt_depth;
} clog_async_job;

// The rendezvous of a flush request. The asking thread waits on cv until the
// writer thread, having flushed everything queued before the request, sets
// done. It lives on the asking thread's stack (the exit drain keeps its own on
// the heap); the writer never frees it.
typedef struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  bool done;
} clog_async_ctrl;

// The envelope that goes through the queue, with an explicit kind. A job sits
// in it by value, so the envelope is the job's only allocation; the writer
// frees it, or clog_write_async() when the send fails. ctrl points at the
// asker's clog_async_ctrl and is never freed by the writer.
typedef struct clog_async_msg {
  clog_async_msg_kind kind;
  union {
    clog_async_job job;
    clog_async_ctrl *ctrl;
  } u;
} clog_async_msg;

// Where a record's fields come from: A logger's live map read under its
// fields_mutex (synchronous and fatal paths), or a snapshot taken at submission
// (async path). failed marks a snapshot that couldn't be taken. One loop,
// clog_emit_fields(), serves both.
typedef struct {
  bool is_live;
  union {
    struct {
      chmap fields;
      ccol__mutex *mutex;
    } live;
    struct {
      const clog_field_view *views;
      size_t count;
      const char *pool;
      bool failed;
    } snap;
  } u;
} clog_field_source;

// Appends one field as " k=v", the logfmt primitive of clog_emit_fields(); the
// value goes through the logfmt escaper. Returns 0 or -1.
static int append_field_logfmt(clog_buf *b, const char *k, const char *v) {
  if (buf_appendf(b, " %s=", k) != 0) return -1;
  return buf_append_lv(b, v);
}

// Appends one field as an SD-PARAM, the syslog primitive of clog_emit_fields();
// the name is limited to 32 characters and the value escaped. Returns 0 or -1.
static int append_field_syslog_sd(clog_buf *b, const char *k, const char *v) {
  char param_name[33];
  sd_param_name(k, param_name);
  if (buf_append(b, " ", 1) != 0) return -1;
  if (buf_append(b, param_name, strlen(param_name)) != 0) return -1;
  if (buf_append(b, "=\"", 2) != 0) return -1;
  if (buf_append_sd_value(b, v) != 0) return -1;
  return buf_append(b, "\"", 1);
}

// Appends every field of src to out in fmt's shape, for clog_build_record(). A
// live source is walked under its mutex with ccol_chmap_walk_next(), which
// allocates nothing, so a record never loses its fields to an allocation and
// they come out in the order they were first set. A field that fails partway is
// rolled back. Returns false when the fields can't all go in;
// *out_alloc_failure tells an allocation failure from a record that is too
// large.
static bool clog_emit_fields(clog_buf *out, clog_format fmt,
                             const clog_field_source *src,
                             bool *out_alloc_failure) {
  *out_alloc_failure = false;
  int (*append_fn)(clog_buf *, const char *, const char *) =
      (fmt == CLOG_FMT_JSON)     ? buf_append_json_kv
      : (fmt == CLOG_FMT_SYSLOG) ? append_field_syslog_sd
                                 : append_field_logfmt;

  if (src->is_live) {
    chmap fields = src->u.live.fields;
    ccol__mutex_lock(*src->u.live.mutex);
    ccol_chmap_walk walk = {0};
    const void *kv;
    const void *vv;
    bool ok = true;
    while (ccol_chmap_walk_next(fields, &walk, &kv, &vv)) {
      size_t field_start = out->len;
      if (append_fn(out, (const char *)kv, (const char *)vv) != 0) {
        out->len = field_start;
        ok = false;
        break;
      }
    }
    ccol__mutex_unlock(*src->u.live.mutex);
    return ok;
  }

  if (src->u.snap.failed) {
    *out_alloc_failure = true;
    return false;
  }
  for (size_t i = 0; i < src->u.snap.count; i++) {
    const char *k = src->u.snap.pool + src->u.snap.views[i].key_offset;
    const char *v = src->u.snap.pool + src->u.snap.views[i].value_offset;
    size_t field_start = out->len;
    if (append_fn(out, k, v) != 0) {
      out->len = field_start;
      return false;
    }
  }
  return true;
}

// Appends the fixed part of a record (ts, level, proc, src and func) in fmt's
// shape, the first step of clog_build_record(). For syslog it also writes the
// RFC 5424 preamble and opens the "[ccol ..." element, which the fields then
// fill and clog_build_msg_and_close() closes. A src too long for the stack
// buffer goes through a heap copy so it's still escaped; a failed allocation
// there sets out->oom. Returns false on failure. Needs sh->mutex.
static bool clog_build_header(clog_buf *out, clog_shared *sh, clog_format fmt,
                              clog_level level, const struct timeval *ts,
                              const char *proc_val, const char *file, int line,
                              const char *func) {
  bool ok = true;

  if (fmt == CLOG_FMT_JSON) {
    ok = ok && buf_append(out, "{\"ts\":\"", 7) == 0;
    ok = ok && buf_append_ts_at(out, ts) == 0;
    ok = ok && buf_append(out, "\"", 1) == 0;
    ok = ok && buf_appendf(out, ",\"level\":\"%s\"", LEVEL_STR[level]) == 0;
    ok = ok && buf_append(out, ",\"proc\":\"", 9) == 0;
    ok = ok && buf_append_json_content(out, proc_val) == 0;
    ok = ok && buf_append(out, "\"", 1) == 0;
    ok = ok && buf_append(out, ",\"src\":\"", 8) == 0;
    ok = ok && buf_append_json_content(out, file) == 0;
    ok = ok && buf_appendf(out, ":%d\"", line) == 0;
    ok = ok && buf_append(out, ",\"func\":\"", 9) == 0;
    ok = ok && buf_append_json_content(out, func) == 0;
    ok = ok && buf_append(out, "\"", 1) == 0;
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + SYSLOG_SEVERITY[level];
    const char *hostname = clog_syslog_hostname(sh);
    const char *appname = sh->syslog_appname;
    ok = ok && buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && buf_append_ts_at(out, ts) == 0;
    ok = ok && buf_appendf(out, " %s %s %d %s", hostname, appname,
                           (int)clog_tident_get()->pid, LEVEL_STR[level]) == 0;
    ok = ok && buf_append(out, " [ccol proc=\"", 13) == 0;
    ok = ok && buf_append_sd_value(out, proc_val) == 0;
    ok = ok && buf_append(out, "\" src=\"", 7) == 0;
    ok = ok && buf_append_sd_value(out, file) == 0;
    ok = ok && buf_appendf(out, ":%d\" func=\"", line) == 0;
    ok = ok && buf_append_sd_value(out, func) == 0;
    ok = ok && buf_append(out, "\"", 1) == 0;
  } else {
    ok = ok && buf_append(out, "ts=", 3) == 0;
    ok = ok && buf_append_ts_at(out, ts) == 0;
    ok = ok && buf_appendf(out, " level=%s", LEVEL_STR[level]) == 0;
    ok = ok && buf_append(out, " proc=", 6) == 0;
    ok = ok && buf_append_lv(out, proc_val) == 0;
    ok = ok && buf_append(out, " src=", 5) == 0;
    {
      char src_stack[512];
      char *src_val = src_stack;
      char *src_heap = NULL;
      int slen = snprintf(src_stack, sizeof(src_stack), "%s:%d", file, line);
      if (slen < 0) {
        ok = false;
      } else if ((size_t)slen >= sizeof(src_stack)) {
        src_heap = ccol__mem_alloc(sh->m_procs, (size_t)slen + 1);
        if (!src_heap) {
          out->oom = true;
          ok = false;
        } else if (snprintf(src_heap, (size_t)slen + 1, "%s:%d", file, line) <
                   0) {
          ok = false;
        } else {
          src_val = src_heap;
        }
      }
      ok = ok && buf_append_lv(out, src_val) == 0;
      ccol__mem_free(sh->m_procs, src_heap);
    }
    ok = ok && buf_append(out, " func=", 6) == 0;
    ok = ok && buf_append_lv(out, func) == 0;
  }

  return ok;
}

// Appends the message and the punctuation that ends the record, plus the JSON
// backtrace array when with_backtrace is set; the last step of
// clog_build_record(). logfmt and syslog frames are added by their callers
// after the record. Returns false on failure. Needs sh->mutex.
static bool clog_build_msg_and_close(clog_buf *out, clog_format fmt,
                                     const char *msg, bool with_backtrace,
                                     char *const *bt_syms, int bt_depth) {
  bool ok = true;

  if (fmt == CLOG_FMT_JSON) {
    ok = ok && buf_append(out, ",\"msg\":\"", 8) == 0;
    ok = ok && buf_append_json_content(out, msg) == 0;
    ok = ok && buf_append(out, "\"", 1) == 0;
    if (ok && with_backtrace) ok = emit_backtrace_json(out, bt_syms, bt_depth);
    ok = ok && buf_append(out, "}\n", 2) == 0;
  } else if (fmt == CLOG_FMT_SYSLOG) {
    ok = ok && buf_append(out, "] ", 2) == 0;
    ok = ok && buf_append_ctrl_escaped(out, msg) == 0;
    ok = ok && buf_append(out, "\n", 1) == 0;
  } else {
    ok = ok && buf_append(out, " msg=", 5) == 0;
    ok = ok && buf_append_lv(out, msg) == 0;
    ok = ok && buf_append(out, "\n", 1) == 0;
  }

  return ok;
}

// Appends a small placeholder record in place of one that couldn't be built, so
// nothing vanishes silently. alloc_failure picks the message: An allocation
// failure, or a record larger than out->cap_limit. It starts at out->len (the
// caller already rolled back) and never resets out, because out can be a batch
// holding other records. With with_backtrace it adds the JSON marker. Returns
// false, leaving out as it was, when even the placeholder doesn't fit (only
// possible in a nearly full batch).
static bool clog_build_fallback_record(clog_buf *out, clog_shared *sh,
                                       clog_format fmt, clog_level level,
                                       const struct timeval *ts,
                                       const char *msg, const char *proc_val,
                                       bool alloc_failure,
                                       bool with_backtrace) {
  size_t fallback_start = out->len;
  char note[160];
  if (alloc_failure) {
    snprintf(note, sizeof(note),
             "log record dropped: a transient allocation failure prevented "
             "it from being fully built");
  } else {
    snprintf(note, sizeof(note),
             "log record too large to emit (%zu byte message; %zu byte "
             "limit)",
             strlen(msg), out->cap_limit);
  }

  bool ok = true;
  if (fmt == CLOG_FMT_JSON) {
    ok = ok && buf_append(out, "{\"ts\":\"", 7) == 0;
    ok = ok && buf_append_ts_at(out, ts) == 0;
    ok = ok && buf_appendf(out, "\",\"level\":\"%s\"", LEVEL_STR[level]) == 0;
    ok = ok && buf_append(out, ",\"proc\":\"", 9) == 0;
    ok = ok && buf_append_json_content(out, proc_val) == 0;
    ok = ok && buf_append(out, "\",\"msg\":\"", 9) == 0;
    ok = ok && buf_append_json_content(out, note) == 0;
    ok = ok && buf_append(out, "\"", 1) == 0;
    if (ok && with_backtrace)
      ok = buf_append(out, BT_JSON_ERROR_MARKER,
                      sizeof(BT_JSON_ERROR_MARKER) - 1) == 0;
    ok = ok && buf_append(out, "}\n", 2) == 0;
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + SYSLOG_SEVERITY[level];
    const char *hostname = clog_syslog_hostname(sh);
    const char *appname = sh->syslog_appname;
    ok = ok && buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && buf_append_ts_at(out, ts) == 0;
    ok = ok && buf_appendf(out, " %s %s %d %s [ccol proc=\"", hostname, appname,
                           (int)clog_tident_get()->pid, LEVEL_STR[level]) == 0;
    ok = ok && buf_append_sd_value(out, proc_val) == 0;
    ok = ok && buf_append(out, "\"] ", 3) == 0;
    ok = ok && buf_append_ctrl_escaped(out, note) == 0;
    ok = ok && buf_append(out, "\n", 1) == 0;
  } else {
    ok = ok && buf_append(out, "ts=", 3) == 0;
    ok = ok && buf_append_ts_at(out, ts) == 0;
    ok = ok && buf_appendf(out, " level=%s proc=", LEVEL_STR[level]) == 0;
    ok = ok && buf_append_lv(out, proc_val) == 0;
    ok = ok && buf_append(out, " msg=", 5) == 0;
    ok = ok && buf_append_lv(out, note) == 0;
    ok = ok && buf_append(out, "\n", 1) == 0;
  }

  if (!ok) out->len = fallback_start;
  return ok;
}

// Builds one complete record into out: Header, fields, message and close, and
// for logfmt the backtrace lines too, falling back to a placeholder on any
// failure. It's the one record builder of the synchronous, fatal and async
// paths; syslog frames are written by the caller afterwards. It resets out->oom
// first so an earlier build can't skew the verdict. Returns true once the
// record or its placeholder is in out; false, with out untouched, when even the
// placeholder doesn't fit, and the caller must empty out and retry.
// *out_used_fallback (may be NULL) says whether the placeholder stood in; a
// batch caller uses it to retry a small record in an empty buffer instead of
// calling it too large. Needs sh->mutex.
static bool clog_build_record(clog_buf *out, clog_shared *sh, clog_format fmt,
                              clog_level level, const struct timeval *ts,
                              const char *proc_val, const char *file, int line,
                              const char *func, const char *msg,
                              const clog_field_source *field_src,
                              bool with_backtrace, char *const *bt_syms,
                              int bt_depth, bool *out_used_fallback) {
  size_t record_start = out->len;
  out->oom = false;
  bool ok =
      clog_build_header(out, sh, fmt, level, ts, proc_val, file, line, func);
  bool alloc_failure = false;
  if (ok) ok = clog_emit_fields(out, fmt, field_src, &alloc_failure);
  if (ok)
    ok = clog_build_msg_and_close(out, fmt, msg, with_backtrace, bt_syms,
                                  bt_depth);
  if (!alloc_failure) alloc_failure = out->oom;

  bool logfmt_bt = with_backtrace && fmt == CLOG_FMT_LOGFMT;
  if (ok && logfmt_bt && !emit_backtrace_lines(out, bt_syms, bt_depth)) {
    ok = false;
    if (!alloc_failure) alloc_failure = out->oom;
  }

  if (out_used_fallback) *out_used_fallback = !ok;
  if (!ok) {
    out->len = record_start;
    if (!clog_build_fallback_record(out, sh, fmt, level, ts, msg, proc_val,
                                    alloc_failure, with_backtrace)) {
      out->len = record_start;
      return false;
    }
    if (logfmt_bt) {
      (void)emit_backtrace_lines(out, bt_syms, bt_depth);
    }
  }
  return true;
}

// Writes a minimal record straight to the fd from a stack buffer, the last
// resort when not even clog_build_record()'s placeholder fits. With the current
// buffer sizes that can't happen; the function keeps a future change from
// turning it into a silent loss. It allocates nothing and can't fail the same
// way. With with_backtrace, JSON gets its marker inside the line and logfmt
// gets a marker line; syslog callers write their frames separately. Needs
// sh->mutex and sh->fd >= 0.
static void clog_write_unrepresentable_record(clog_shared *sh, clog_format fmt,
                                              clog_level level,
                                              bool with_backtrace) {
  static const char note[] =
      "log record dropped: could not be represented within the buffer size "
      "limit";
  char line[256];
  int n;
  if (fmt == CLOG_FMT_JSON) {
    n = with_backtrace ? snprintf(line, sizeof(line),
                                  "{\"level\":\"%s\",\"msg\":\"%s\","
                                  "\"bt_error\":\"unavailable\"}\n",
                                  LEVEL_STR[level], note)
                       : snprintf(line, sizeof(line),
                                  "{\"level\":\"%s\",\"msg\":\"%s\"}\n",
                                  LEVEL_STR[level], note);
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + SYSLOG_SEVERITY[level];
    n = snprintf(line, sizeof(line), "<%d>1 - - - - - - %s\n", pri, note);
  } else {
    n = snprintf(line, sizeof(line), "level=%s msg=\"%s\"\n", LEVEL_STR[level],
                 note);
  }
  if (n > 0) {
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    clog_write_record(sh, fmt, line, len);
  }

  if (with_backtrace && fmt == CLOG_FMT_LOGFMT) {
    clog_write_record(sh, fmt, BT_LINE_ERROR_MARKER,
                      sizeof(BT_LINE_ERROR_MARKER) - 1);
  }
}

#ifdef RUNNING_UNIT_TESTS

// Runs clog_write_unrepresentable_record() against logger's target under its
// mutex, so a test can check it; the ordinary path can't reach it with the
// current buffer sizes.
void clog__test_write_unrepresentable_record(clog logger, clog_format fmt,
                                             clog_level level,
                                             bool with_backtrace) {
  struct clogger *lg = clog_resolve(logger);
  if (!lg) return;
  ccol__mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    clog_write_unrepresentable_record(lg->shared, fmt, level, with_backtrace);
  ccol__mutex_unlock(lg->shared->mutex);
  clog_resolve_unpin(lg);
}
#endif

// Creates a logger's field map (string to string) with m_procs, for
// logger_alloc(). Returns the map or NULL.
static chmap fields_create(ccol_memmgmt_procs *m_procs) {
  char *err = NULL;
  chmap m = chashmap_create_mp(CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                               ccol_string, ccol_string, m_procs, &err);
  return m;
}

// Frees a clog_shared that has no live mutex and no loggers, used by the
// constructors' rollback paths and the final teardown. It's safe at any point
// after shared_alloc() set m_procs. It never closes sh->fd, because on an early
// failure the constructor's caller still owns it; callers that own the fd close
// it first with shared_close_owned_fd().
static void shared_free_partial(clog_shared *sh) {
  ccol__mem_free(sh->m_procs, sh->file_path);
  ccol_memmgmt_procs *mp = sh->m_procs;
  if (mp) {
    ccol_free free_fn = mp->free;
    free_fn(mp);
    free_fn(sh);
  } else {
    free(sh);
  }
}

// Closes sh->fd when sh owns it (a file logger) and it's open, and always
// closes dir_fd, then marks both closed. clog_close() and every constructor
// rollback use it so a later failed step doesn't leak the fd just opened. An fd
// the caller owns (clog_create_from_fd_mp()) is left alone.
static void shared_close_owned_fd(clog_shared *sh) {
  if (sh->owns_fd && sh->fd >= 0) {
    close(sh->fd);
    sh->fd = -1;
  }
  if (sh->dir_fd >= 0) {
    close(sh->dir_fd);
    sh->dir_fd = -1;
  }
}

// Allocates and initializes a clog_shared for the constructors: Copies the
// procs and the path, creates the mutex, classifies the sink and caches the
// sanitized hostname and APP-NAME (the raw hostname may hold bytes that would
// split a syslog record). fd and dir_fd stay owned by the caller until it
// succeeds. Returns NULL on failure.
static clog_shared *shared_alloc(int fd, bool owns_fd, const char *file_path,
                                 int dir_fd, ccol_memmgmt_procs *m_procs) {
  clog_shared *sh = ccol__mem_calloc(m_procs, 1, sizeof(*sh));
  if (!sh) return NULL;

  sh->fd = fd;
  sh->owns_fd = owns_fd;
  sh->sink_kind = clog_classify_sink(fd);
  sh->sink_per_record = clog_sink_is_message(fd);
  sh->ref_count = 1;
  sh->dir_fd = -1;

  if (m_procs) {
    sh->m_procs = (ccol_memmgmt_procs *)m_procs->malloc(sizeof(*m_procs));
    if (!sh->m_procs) {
      m_procs->free(sh);
      return NULL;
    }
    memcpy(sh->m_procs, m_procs, sizeof(*m_procs));
  }

  if (file_path) {
    size_t len = strlen(file_path) + 1;
    sh->file_path = ccol__mem_alloc(sh->m_procs, len);
    if (!sh->file_path) {
      shared_free_partial(sh);
      return NULL;
    }
    memcpy(sh->file_path, file_path, len);
    sh->file_base = path_base(sh->file_path);
  }

  if (ccol__mutex_init(sh->mutex) != 0) {
    shared_free_partial(sh);
    return NULL;
  }

  sh->dir_fd = dir_fd;
  sh->last_rotation = clog_wall_now();

  sh->syslog_facility = CLOG_SYSLOG_USER;

  char raw_hostname[256];
  if (gethostname(raw_hostname, sizeof(raw_hostname)) != 0)
    raw_hostname[0] = '\0';
  raw_hostname[sizeof(raw_hostname) - 1] = '\0';
  sanitize_syslog_printusascii_field(raw_hostname, sh->syslog_hostname,
                                     sizeof(sh->syslog_hostname));

  sanitize_syslog_printusascii_field(
      syslog_appname_compute(), sh->syslog_appname, sizeof(sh->syslog_appname));

  return sh;
}

// Allocates a logger on shared at min_level, with its field map, fields_mutex
// and record buffer, for alloc() and clog_derive(). It doesn't touch shared's
// reference count. Returns NULL on failure.
static struct clogger *logger_alloc(clog_shared *shared, clog_level min_level) {
  struct clogger *lg = ccol__mem_calloc(shared->m_procs, 1, sizeof(*lg));
  if (!lg) return NULL;

  if (buf_init(&lg->buf, shared->m_procs) != 0) {
    ccol__mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->fields = fields_create(shared->m_procs);
  if (!lg->fields) {
    buf_free(&lg->buf);
    ccol__mem_free(shared->m_procs, lg);
    return NULL;
  }

  if (ccol__mutex_init(lg->fields_mutex) != 0) {
    chashmap__destroy(lg->fields);
    buf_free(&lg->buf);
    ccol__mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->shared = shared;
  lg->min_level = min_level;
  return lg;
}

// Frees a fully built logger (field map, buffer, fields_mutex) without touching
// the shared store, for clog_close() and the rollback paths.
static void logger_free(struct clogger *lg) {
  ccol__mutex_destroy(lg->fields_mutex);
  chashmap__destroy(lg->fields);
  lg->fields = NULL;
  buf_free(&lg->buf);
  ccol__mem_free(lg->shared->m_procs, lg);
}

// Builds a new shared store and its first logger, the common start of both
// public constructors. On failure it frees what it built and returns NULL; the
// caller still owns fd and dir_fd.
static struct clogger *alloc(int fd, bool owns_fd, const char *file_path,
                             int dir_fd, clog_level min_level,
                             ccol_memmgmt_procs *m_procs) {
  clog_shared *sh = shared_alloc(fd, owns_fd, file_path, dir_fd, m_procs);
  if (!sh) return NULL;

  struct clogger *lg = logger_alloc(sh, min_level);
  if (!lg) {
    ccol__mutex_destroy(sh->mutex);
    shared_free_partial(sh);
    return NULL;
  }

  return lg;
}

// Copies lg's fields under fields_mutex into *out_fields, *out_count and
// *out_pool, so a later change on lg can't alter a job already submitted; used
// by clog_write_async(). A logger without fields gets an empty pool with no
// allocation. *out_failed is set only on an allocation failure, with
// *out_fields NULL. The pool is never destroyed here; the job release does it
// on every path.
static void snapshot_fields(struct clogger *lg, clog_field_view **out_fields,
                            size_t *out_count, ccol_growbuf *out_pool,
                            bool *out_failed) {
  *out_fields = NULL;
  *out_count = 0;
  *out_failed = false;

  ccol__mutex_lock(lg->fields_mutex);
  size_t n = chashmap_size(lg->fields);
  if (n == 0) {
    ccol__mutex_unlock(lg->fields_mutex);
    out_pool->buf = NULL;
    out_pool->len = 0;
    out_pool->cap = 0;
    out_pool->oom = false;
    out_pool->m_procs = lg->shared->m_procs;
    return;
  }
  ccol_growbuf_init(out_pool, lg->shared->m_procs);

  clog_field_view *views =
      ccol__mem_calloc(lg->shared->m_procs, n, sizeof(*views));
  if (!views) {
    ccol__mutex_unlock(lg->fields_mutex);
    *out_failed = true;
    return;
  }

  ccol_chmap_walk walk = {0};
  const void *kv;
  const void *vv;
  size_t i = 0;
  while (i < n && ccol_chmap_walk_next(lg->fields, &walk, &kv, &vv)) {
    views[i].key_offset = out_pool->len;
    ccol_growbuf_append_cstr(out_pool, (const char *)kv);
    ccol_growbuf_append_c(out_pool, '\0');
    views[i].value_offset = out_pool->len;
    ccol_growbuf_append_cstr(out_pool, (const char *)vv);
    ccol_growbuf_append_c(out_pool, '\0');
    i++;
  }
  ccol__mutex_unlock(lg->fields_mutex);

  if (out_pool->oom) {
    ccol__mem_free(lg->shared->m_procs, views);
    *out_failed = true;
    return;
  }

  *out_fields = views;
  *out_count = n;
}

// Tells whether a later attempt on the same descriptor can outlast write error
// err, which decides if undelivered bytes are kept or dropped. True for a
// resource that is used up for now (ENOSPC, EDQUOT, EFBIG, ENOBUFS, ENOMEM),
// for EAGAIN and EINTR, and for 0 (a write that made no progress); false for
// errors such as EPIPE, ECONNRESET, EBADF or EMSGSIZE.
static bool clog_write_error_is_transient(int err) {
  switch (err) {
    case 0:
    case EINTR:
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
    case EFBIG:
    case ENOBUFS:
    case ENOMEM:
      return true;
    default:
      return false;
  }
}

// Tells where the first written bytes of data end, as a split value of
// async_partial_record, so a flush knows what a short write left open. The
// caller guarantees 0 < written < len.
static unsigned char clog_split_after(const char *data, size_t written) {
  if (data[written - 1] != '\n') return CLOG_SPLIT_MID_LINE;
  return data[written] == '\t' ? CLOG_SPLIT_AT_LINE : CLOG_SPLIT_NONE;
}

static void clog_repair_split_record(clog_shared *sh, clog_format fmt,
                                     int poll_timeout_ms);

// Tells whether write error err means the output has no room (full disk, quota,
// RLIMIT_FSIZE), the case where a due rotation must not wait for a continuation
// that only the room it frees can deliver.
static bool clog_write_error_is_no_room(int err) {
  switch (err) {
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
    case EFBIG:
      return true;
    default:
      return false;
  }
}

// Gives the length of the next piece of a large backlog, so a flush can rotate
// between pieces: Up to the first record boundary where the file reaches
// max_file_size, or all n bytes when it doesn't or the backoff blocks the
// rotation. A boundary is a newline not followed by a tab. Needs sh->mutex;
// rotation is on.
static __attribute__((noinline, cold)) size_t
clog_backlog_piece(const clog_shared *sh, const char *data, size_t n) {
  if (!sh->rotation.size_rotation_enabled) return n;
  int64_t room = sh->rotation.max_file_size - (int64_t)sh->bytes_written;
  if (room >= (int64_t)n) return n;
  size_t from = 0;
  if (room > 0)
    from = (size_t)room - 1;
  else if (!clog_rotate_retry_allowed(sh, clog_wall_now()))
    return n;
  const char *p = data + from;
  while ((p = memchr(p, '\n', n - (size_t)(p - data))) != NULL) {
    size_t b = (size_t)(p - data) + 1;
    if (b == n || data[b] != '\t') return b;
    p++;
  }
  return n;
}

// Writes sh->async_buf to the fd with the time rotation check before and the
// size check after, the flush of every async path; poll_timeout_ms bounds the
// write. After a transient error the unwritten bytes stay at the front for the
// next flush, and async_partial_record records whether the written part split a
// record (no rotation and no other record then until the rest goes out). Any
// other error drops them with a marker. When the output is out of room and a
// rotation is due, a split continuation is given up so the rotation can free
// room, and the kept bytes get one more try in the new file. A backlog that
// would pass max_file_size goes out in pieces with rotation checks between.
// Nothing is written while a newline is owed; an owed repair with an empty
// batch is paid here. It never touches last_flush_monotonic. Needs sh->mutex;
// not for syslog targets.
static void writer_flush_buffer(clog_shared *sh, int poll_timeout_ms) {
  bool may_retry = true;
again:
  if (__builtin_expect(sh->async_buf.len == 0 &&
                           (sh->async_partial_record & CLOG_OWED_REPAIR),
                       0)) {
    if (sh->fd >= 0) (void)clog_pay_owed(sh, sh->format, poll_timeout_ms);
  }
  if (sh->async_buf.len > 0) {
    bool retained = false;
    int fd_before = sh->fd;
    if (sh->fd >= 0) {
      clog_time_rotate_if_due(sh);

      if (__builtin_expect(sh->async_partial_record & CLOG_OWED_REPAIR, 0) &&
          !clog_pay_owed(sh, sh->format, poll_timeout_ms)) {
        retained = true;
      } else {
        char *data = sh->async_buf.data;
        size_t len = sh->async_buf.len;
        size_t off = 0;
        int write_err;
        for (;;) {
          size_t piece = len - off;
          if (__builtin_expect(sh->rotation_enabled, 0) &&
              piece > sh->async_cfg.flush_buffer_size)
            piece = clog_backlog_piece(sh, data + off, piece);
          errno = 0;
          size_t written = write_all(sh, data + off, piece, poll_timeout_ms);
          write_err = errno;
          if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
          off += written;
          if (written < piece || off == len) break;
          sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
          clog_size_rotate_if_due(sh);
        }

        if (off < len) {
          unsigned char split =
              off > 0
                  ? clog_split_after(data, off)
                  : (unsigned char)(sh->async_partial_record & CLOG_SPLIT_MASK);
          if (clog_write_error_is_transient(write_err)) {
            sh->async_partial_record =
                (unsigned char)((sh->async_partial_record & ~CLOG_SPLIT_MASK) |
                                split);
            sh->retain_err = write_err;
            memmove(data, data + off, len - off);
            sh->async_buf.len = len - off;
            retained = true;
            if (split != CLOG_SPLIT_NONE &&
                clog_write_error_is_no_room(write_err) &&
                clog_rotation_due(sh)) {
              clog_repair_split_record(sh, sh->format, poll_timeout_ms);
              clog_time_rotate_if_due(sh);
            }
          } else {
            char why[48];
            snprintf(why, sizeof(why), "write error %d", write_err);
            sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
            clog_note_dropped_batch(sh, data + off, len - off, split, why,
                                    write_err, poll_timeout_ms);
          }
        }
      }
    }
    if (!retained) {
      buf_reset(&sh->async_buf);
      sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
    }
    if (sh->fd >= 0) clog_size_rotate_if_due(sh);
    if (retained && may_retry && sh->fd >= 0 && sh->fd != fd_before) {
      may_retry = false;
      goto again;
    }
  }
}

// The writer thread's flush: writer_flush_buffer() plus advancing
// last_flush_monotonic, even when there was nothing to write, so the thread's
// next wait is a full interval and an idle logger doesn't spin. Only the writer
// thread calls it, since only it writes that field. Needs sh->mutex.
static void writer_flush_now(clog_shared *sh, int poll_timeout_ms) {
  writer_flush_buffer(sh, poll_timeout_ms);
  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);
}

// Gives up the continuation of a split record at the front of async_buf and
// writes, in one write, the newline the first half left open and a marker for
// the bytes dropped, so the file reads as whole lines again. The continuation
// runs to the newline of the record's last line, through tab-indented lines.
// Whole records behind it stay for a later flush. Needs sh->mutex, a usable fd
// and the split bits set.
static void clog_repair_split_record(clog_shared *sh, clog_format fmt,
                                     int poll_timeout_ms) {
  size_t len = sh->async_buf.len;
  const char *data = sh->async_buf.data;
  size_t dropped = 0;
  do {
    const char *nl = memchr(data + dropped, '\n', len - dropped);
    dropped = nl ? (size_t)(nl - data) + 1 : len;
  } while (dropped < len && data[dropped] == '\t');

  unsigned char split =
      (unsigned char)(sh->async_partial_record & CLOG_SPLIT_MASK);
  sh->async_buf.len = len - dropped;
  if (sh->async_buf.len > 0)
    memmove(sh->async_buf.data, sh->async_buf.data + dropped,
            sh->async_buf.len);
  else
    buf_reset(&sh->async_buf);
  sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;

  clog_note_dropped_continuation(sh, fmt, split, dropped, sh->retain_err,
                                 poll_timeout_ms);
}

// Drops every record async_buf still holds and writes a marker naming them, for
// the close and exit paths where no later flush will retry them. The caller
// already settled any split record. Needs sh->mutex.
static __attribute__((noinline, cold)) void clog_drop_undelivered(
    clog_shared *sh, const char *why, int poll_timeout_ms) {
  if (sh->async_buf.len == 0) return;
  sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
  if (sh->fd >= 0)
    clog_note_dropped_batch(sh, sh->async_buf.data, sh->async_buf.len,
                            CLOG_SPLIT_NONE, why, 0, poll_timeout_ms);
  buf_reset(&sh->async_buf);
}

// Repairs a record still split after a flush, and then pays an owed repair, so
// a record written straight to the fd next lands on a line of its own and not
// between the halves of another. Used by every path that writes past the batch;
// it gives the continuation no further wait. Needs sh->mutex.
static void clog_settle_after_flush(clog_shared *sh, clog_format fmt,
                                    int poll_timeout_ms) {
  if (sh->async_partial_record & CLOG_SPLIT_MASK)
    clog_repair_split_record(sh, fmt, poll_timeout_ms);
  if (sh->async_partial_record & CLOG_OWED_REPAIR)
    (void)clog_pay_owed(sh, fmt, poll_timeout_ms);
}

// Flushes async_buf (bounded by poll_timeout_ms) and settles what's left, for
// the two write paths that don't belong to the writer thread. Out of line so
// the ordinary log path keeps its shape. Needs sh->mutex.
static __attribute__((noinline)) void clog_flush_and_settle(
    clog_shared *sh, clog_format fmt, int poll_timeout_ms) {
  writer_flush_buffer(sh, poll_timeout_ms);
  clog_settle_after_flush(sh, fmt, poll_timeout_ms);
}

// Frees a job envelope and everything it owns: The message spill, the field
// snapshot and the backtrace (with free(), since backtrace_symbols() used
// malloc). Used by the writer thread after each job and by clog_write_async()
// when the send fails.
static void clog_async_job_release(clog_shared *sh, clog_async_msg *envelope) {
  clog_async_job *job = &envelope->u.job;
  if (job->bt_syms) free(job->bt_syms);
  ccol_growbuf_destroy(&job->field_pool);
  ccol__mem_free(sh->m_procs, job->fields);
  ccol__mem_free(sh->m_procs, job->msg_heap);
  ccol__mem_free(sh->m_procs, envelope);
}

#ifdef RUNNING_UNIT_TESTS

// How long the writer thread holds each job under the mutex, for tests of a
// slow output.
static _Atomic unsigned int clog_test_writer_job_delay_us = 0;

// Sets how long the writer thread holds each job under the mutex, so a test can
// make a slow output.
void clog__test_set_writer_job_delay_us(unsigned int delay_us) {
  atomic_store(&clog_test_writer_job_delay_us, delay_us);
}

// How long the writer thread holds a job before taking the mutex, so a test can
// put a record between taking a job and locking.
static _Atomic unsigned int clog_test_writer_unlocked_job_delay_us = 0;

// Sets how long the writer thread holds each job after taking it and before it
// takes the mutex, so a test can log a record in that window.
void clog__test_set_writer_unlocked_job_delay_us(unsigned int delay_us) {
  atomic_store(&clog_test_writer_unlocked_job_delay_us, delay_us);
}

// Holds the writer thread for delay_us, for the job delay hooks. Up to 50 ms it
// waits on the clock, since a sleep on a busy machine can end far too late for
// a test bound; longer holds just sleep.
static void clog_test_writer_job_delay(unsigned int delay_us) {
  if (delay_us > 50000U) {
    usleep(delay_us);
    return;
  }
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  do {
    sched_yield();
    clock_gettime(CLOCK_MONOTONIC, &now);
  } while ((long long)(now.tv_sec - start.tv_sec) * 1000000LL +
               (now.tv_nsec - start.tv_nsec) / 1000L <
           (long long)delay_us);
}
#endif

// Waits, polling, until no CLOG_FATAL call waits for sh's mutex, so the writer
// thread doesn't keep winning the mutex from a fatal call. The fatal call's own
// bounded wait lowers the count.
static __attribute__((noinline, cold)) void clog_writer_yield_to_fatal(
    clog_shared *sh) {
  while (atomic_load(&sh->fatal_waiters) != 0) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 200000L};
    nanosleep(&pause, NULL);
  }
}

// Locks sh->mutex for a piece of writer thread work, after stepping aside for
// any waiting CLOG_FATAL call (a mutex goes to whoever asks again first). It
// also syncs the thread's signal mask with the current policy.
static inline void clog_writer_lock(clog_shared *sh) {
  clog_thread_sync_signal_mask();
#ifdef RUNNING_UNIT_TESTS
  {
    sigset_t cur;
    sigemptyset(&cur);
    if (ccol__thread_sigmask(SIG_BLOCK, NULL, &cur) == 0)
      atomic_store(&sh->test_writer_blocked_signals,
                   4u | (sigismember(&cur, SIGPIPE) == 1 ? 1u : 0u) |
                       (sigismember(&cur, SIGXFSZ) == 1 ? 2u : 0u));
  }
#endif
  if (__builtin_expect(
          atomic_load_explicit(&sh->fatal_waiters, memory_order_relaxed) != 0,
          0))
    clog_writer_yield_to_fatal(sh);
  ccol__mutex_lock(sh->mutex);
}

// Tells whether the writer thread has nothing to deliver (empty batch, no split
// record, no owed repair), in which case it waits for a message with no time
// limit. Needs sh->mutex.
static inline bool clog_writer_has_no_work(const clog_shared *sh) {
  return sh->async_buf.len == 0 && sh->async_partial_record == 0;
}

// The async writer thread of target arg. It takes messages from the queue and
// builds each job into async_buf, flushing when the batch reaches
// flush_buffer_size, when flush_interval_us runs out (checked before every
// message, so sustained load can't starve it) and on flush or wake requests.
// With no work it waits without a limit. Syslog and message sockets get one
// write per record. A record that doesn't fit behind kept bytes goes through a
// scratch buffer or counts as a loss; a fallback caused by an almost full batch
// is rolled back and retried after a flush. A failing receive backs off with a
// sleep. On the shutdown sentinel it settles and drops what's left and exits.
// The interval arithmetic stays unsigned so UINT64_MAX means no timed flush.
static void *clog_writer_thread_main(void *arg) {
  clog_shared *sh = (clog_shared *)arg;
  clog_thread_serves = sh;
  clog_thread_signal_init();
  bool interval_flushed = false;
  bool no_work = true;

  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed_us =
        (long long)(now.tv_sec - sh->last_flush_monotonic.tv_sec) * 1000000LL +
        (now.tv_nsec - sh->last_flush_monotonic.tv_nsec) / 1000L;
    uint64_t interval_us = sh->async_cfg.flush_interval_us;
    uint64_t remaining_us = elapsed_us <= 0 ? interval_us
                            : (uint64_t)elapsed_us >= interval_us
                                ? 0
                                : interval_us - (uint64_t)elapsed_us;

    if (remaining_us == 0 && !interval_flushed) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&sh->test_writer_interval_flushes, 1);
#endif
      clog_writer_lock(sh);
      writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = clog_writer_has_no_work(sh);
      ccol__mutex_unlock(sh->mutex);
      interval_flushed = true;
      continue;
    }
    interval_flushed = false;

    if (no_work) remaining_us = UINT64_MAX;
    cmessage m = {0};
    ccol_retval rv = sh->is_bounded_queue
                         ? ccircq_timed_recv(sh->q.circq, &m, remaining_us)
                         : cdynmq_timed_recv(sh->q.dynmq, &m, remaining_us);

    if (rv == ccol_timed_out || rv == ccol_container_empty) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&sh->test_writer_timeout_wakeups, 1);
      atomic_fetch_add(&sh->test_writer_interval_flushes, 1);
#endif
      clog_writer_lock(sh);
      writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = clog_writer_has_no_work(sh);
      ccol__mutex_unlock(sh->mutex);
      continue;
    }
    if (rv != ccol_success) {
      usleep(1000);
      continue;
    }

    if (m.data == NULL && m.size == 0) {
      clog_writer_lock(sh);
      writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      clog_settle_after_flush(sh, sh->format, CLOG_WRITE_WAIT_FOREVER);
      clog_drop_undelivered(sh, "the logger closed", CLOG_WRITE_WAIT_FOREVER);
      ccol__mutex_unlock(sh->mutex);
      return NULL;
    }

    clog_async_msg *envelope = (clog_async_msg *)m.data;

    if (envelope->kind == CLOG_ASYNC_MSG_WAKE) {
      clog_writer_lock(sh);
      writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = clog_writer_has_no_work(sh);
      ccol__mutex_unlock(sh->mutex);
      continue;
    }

    if (envelope->kind == CLOG_ASYNC_MSG_FLUSH) {
      clog_async_ctrl *ctrl = envelope->u.ctrl;
      clog_writer_lock(sh);
      writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = clog_writer_has_no_work(sh);
      ccol__mutex_unlock(sh->mutex);
      ccol__mutex_lock(ctrl->mutex);
      ctrl->done = true;
      ccol__cond_var_broadcast(ctrl->cv);
      ccol__mutex_unlock(ctrl->mutex);
      continue;
    }

    clog_async_job *job = &envelope->u.job;
#ifdef RUNNING_UNIT_TESTS
    {
      unsigned int delay = atomic_load(&clog_test_writer_unlocked_job_delay_us);
      if (delay) usleep(delay);
    }
#endif
    clog_writer_lock(sh);
#ifdef RUNNING_UNIT_TESTS
    {
      unsigned int delay = atomic_load(&clog_test_writer_job_delay_us);
      if (delay) clog_test_writer_job_delay(delay);
    }
#endif
    if (sh->fd >= 0) {
      clog_format fmt = sh->format;
      clog_field_source fsrc;
      fsrc.is_live = false;
      fsrc.u.snap.views = job->fields;
      fsrc.u.snap.count = job->field_count;
      fsrc.u.snap.pool = job->field_pool.buf;
      fsrc.u.snap.failed = job->fields_snapshot_failed;

      if (fmt == CLOG_FMT_SYSLOG || sh->sink_per_record) {
        writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
        clog_settle_after_flush(sh, fmt, CLOG_WRITE_WAIT_FOREVER);

        clog_buf scratch;
        clog_buf *rec_buf = &sh->async_buf;
        if (sh->async_buf.len > 0)
          rec_buf = buf_init(&scratch, sh->m_procs) == 0 ? &scratch : NULL;

        if (!rec_buf) {
          clog_write_unrepresentable_record(sh, fmt, job->level,
                                            job->with_backtrace);
        } else {
          bool built = clog_build_record(
              rec_buf, sh, fmt, job->level, &job->ts, job->proc_val, job->file,
              job->line, job->func, job->msg, &fsrc, job->with_backtrace,
              job->bt_syms, job->bt_depth, NULL);
          if (built) {
            clog_write_record(sh, fmt, rec_buf->data, rec_buf->len);
          } else {
            clog_write_unrepresentable_record(sh, fmt, job->level,
                                              job->with_backtrace);
          }
          buf_reset(rec_buf);
          if (job->with_backtrace && fmt == CLOG_FMT_SYSLOG)
            emit_backtrace_syslog_lines(rec_buf, sh, job->level, &job->ts,
                                        job->bt_syms, job->bt_depth);
          if (rec_buf != &sh->async_buf) buf_free(rec_buf);
        }
      } else {
        size_t record_start = sh->async_buf.len;
        bool batch_had_prior_content = record_start > 0;
        bool used_fallback = false;
        bool built = clog_build_record(
            &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
            job->file, job->line, job->func, job->msg, &fsrc,
            job->with_backtrace, job->bt_syms, job->bt_depth, &used_fallback);
        if (!built || (used_fallback && batch_had_prior_content)) {
          sh->async_buf.len = record_start;
          writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
          clog_settle_after_flush(sh, fmt, CLOG_WRITE_WAIT_FOREVER);
          size_t retry_start = sh->async_buf.len;
          bool retry_fallback = false;
          if (!clog_build_record(&sh->async_buf, sh, fmt, job->level, &job->ts,
                                 job->proc_val, job->file, job->line, job->func,
                                 job->msg, &fsrc, job->with_backtrace,
                                 job->bt_syms, job->bt_depth,
                                 retry_start > 0 ? &retry_fallback : NULL)) {
            clog_write_unrepresentable_record(sh, fmt, job->level,
                                              job->with_backtrace);
            clog_size_rotate_if_due(sh);
          } else if (retry_fallback && !sh->async_buf.oom) {
            sh->async_buf.len = retry_start;
            size_t mlen = strlen(job->msg);
            char detail[112];
            snprintf(detail, sizeof(detail),
                     "a record did not fit behind the undelivered batch "
                     "(%zu byte message)",
                     mlen);
            clog_report_loss(sh, fmt, false, 1, mlen, 0, detail,
                             CLOG_WRITE_WAIT_FOREVER);
          }
        }
        if (sh->async_buf.len >= sh->async_cfg.flush_buffer_size)
          writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      }
    }
    no_work = clog_writer_has_no_work(sh);
    ccol__mutex_unlock(sh->mutex);

    clog_async_job_release(sh, envelope);
  }
}

// Turns async logging on for sh: Resolves the config defaults, creates the
// queue, sets up async_buf (its limit raised to flush_buffer_size) and starts
// the writer thread, undoing everything if a step fails. async_enabled is set
// only at the end. Each constructor calls it last, so the writer thread never
// sees a half-built sh. Returns false on failure.
static bool shared_async_init(clog_shared *sh,
                              const clog_async_cfg *async_cfg) {
  sh->async_cfg.queue_size = async_cfg->queue_size;
  sh->async_cfg.flush_buffer_size = async_cfg->flush_buffer_size
                                        ? async_cfg->flush_buffer_size
                                        : CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE;
  sh->async_cfg.flush_interval_us = async_cfg->flush_interval_us
                                        ? async_cfg->flush_interval_us
                                        : CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US;
  sh->is_bounded_queue = (async_cfg->queue_size > 0);

  char *err = NULL;
  if (sh->is_bounded_queue) {
    sh->q.circq = ccircq_create_mp(async_cfg->queue_size, sh->m_procs, &err);
    if (!sh->q.circq) return false;
  } else {
    sh->q.dynmq = cdynmq_create_mp(sh->m_procs, &err);
    if (!sh->q.dynmq) return false;
  }

  if (buf_init(&sh->async_buf, sh->m_procs) != 0) {
    if (sh->is_bounded_queue)
      ccircq_destroy(sh->q.circq);
    else
      cdynmq_destroy(sh->q.dynmq);
    return false;
  }
  buf_raise_cap_limit(&sh->async_buf, sh->async_cfg.flush_buffer_size);

  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);

  if (ccol__thread_create(sh->writer_thread, clog_writer_thread_main, sh) !=
      0) {
    buf_free(&sh->async_buf);
    if (sh->is_bounded_queue)
      ccircq_destroy(sh->q.circq);
    else
      cdynmq_destroy(sh->q.dynmq);
    return false;
  }

  sh->async_enabled = true;
  clog_exit_drain_arm();
  return true;
}

// Sends the writer thread its shutdown sentinel, retrying with a backoff up to
// 100 ms until the queue takes it. An unbounded queue can fail a send for lack
// of memory, and the join that follows would then wait for ever; giving up
// isn't possible either, because the caller frees what the thread still uses.
static void clog_send_shutdown_sentinel_blocking(clog_shared *sh) {
  unsigned int delay_us = 1000;
  for (;;) {
    cmessage sentinel = {.data = NULL, .size = 0};
    ccol_retval rv = sh->is_bounded_queue ? ccircq_send(sh->q.circq, &sentinel)
                                          : cdynmq_send(sh->q.dynmq, &sentinel);
    if (rv == ccol_success) return;
    usleep(delay_us);
    if (delay_us < 100000) delay_us *= 2;
  }
}

// Stops the writer thread (after re-enabling sends an exit drain may have
// turned off) and frees the queue and async_buf. Used by the last clog_close()
// and by a constructor rollback after shared_async_init() succeeded.
static void shared_async_teardown(clog_shared *sh) {
  if (sh->is_bounded_queue)
    (void)ccircq_enable_sending(sh->q.circq);
  else
    (void)cdynmq_enable_sending(sh->q.dynmq);
  clog_send_shutdown_sentinel_blocking(sh);
  ccol__thread_join(sh->writer_thread);
  if (sh->is_bounded_queue)
    ccircq_destroy(sh->q.circq);
  else
    cdynmq_destroy(sh->q.dynmq);
  buf_free(&sh->async_buf);
}

// Validators of the enum setters and the open calls: Each accepts only the
// values its enum names. They compare through int, since an enum can be
// unsigned and a negative value would become a large one.
static bool clog_level_is_valid(clog_level level) {
  int v = (int)level;
  return v >= (int)CLOG_TRACE && v <= (int)CLOG_OFF;
}

static bool clog_format_is_valid(clog_format fmt) {
  int v = (int)fmt;
  return v == (int)CLOG_FMT_LOGFMT || v == (int)CLOG_FMT_JSON ||
         v == (int)CLOG_FMT_SYSLOG;
}

static bool clog_facility_is_valid(clog_syslog_facility facility) {
  int v = (int)facility;
  return (v >= (int)CLOG_SYSLOG_KERN && v <= (int)CLOG_SYSLOG_FTP) ||
         (v >= (int)CLOG_SYSLOG_LOCAL0 && v <= (int)CLOG_SYSLOG_LOCAL7);
}

// Creates a logger that writes to the caller's fd, the public entry for pipes,
// sockets and terminals. The caller keeps owning fd. async_cfg turns on async
// logging and m_procs picks the allocator. Gives the handle, or CLOG_NULL when
// an argument is invalid or a step fails; every failure rolls back what was
// built.
clog clog_create_from_fd_mp(int fd, clog_level min_level,
                            const clog_async_cfg *async_cfg,
                            ccol_memmgmt_procs *mprocs) {
  if (fd < 0 || !clog_level_is_valid(min_level)) return CLOG_NULL;
  int fd_flags = fcntl(fd, F_GETFL);
  if (fd_flags == -1) return CLOG_NULL;
  int accmode = fd_flags & O_ACCMODE;
  if (accmode != O_WRONLY && accmode != O_RDWR) return CLOG_NULL;
  if (!ccol__verify_memmgmt_procs(mprocs, (char **)NULL)) return CLOG_NULL;
  struct clogger *lg = alloc(fd, false, NULL, -1, min_level, mprocs);
  if (!lg) return CLOG_NULL;

  if (async_cfg && !shared_async_init(lg->shared, async_cfg)) {
    clog_shared *sh = lg->shared;
    shared_close_owned_fd(sh);
    ccol__mutex_destroy(sh->mutex);
    logger_free(lg);
    shared_free_partial(sh);
    return CLOG_NULL;
  }

  clog h = clog_handle_acquire(lg);
  if (h == CLOG_NULL) {
    clog_shared *sh = lg->shared;
    if (sh->async_enabled) shared_async_teardown(sh);
    shared_compressor_teardown(sh);
    shared_close_owned_fd(sh);
    ccol__mutex_destroy(sh->mutex);
    logger_free(lg);
    shared_free_partial(sh);
    return CLOG_NULL;
  }
  return h;
}

// Sets sh->last_rotation for a time-rotating logger that opens a non-empty
// file, so the interval counts from when the file began and a process that
// restarts often still rotates. It uses the stamp of the newest generation on
// disk, else the file's birth time (statx on Linux, struct stat on macOS and
// FreeBSD), else its mtime; a seed in the future (clock stepped back) becomes
// the open time. The newest name found is also recorded for rotate(). It writes
// no WARN, since the open may still join another target.
static void clog_seed_last_rotation(clog_shared *sh, int fd,
                                    const struct stat *st) {
  time_t seed = st->st_mtime;
  bool seeded = false;
  if (!sh->rot_name_known)
    sh->rot_name_known = rotated_newest_on_disk(sh, false, sh->rot_last_stamp,
                                                &sh->rot_last_seq);
  time_t stamp;
  if (sh->rot_name_known && clog_stamp_to_time(sh->rot_last_stamp, &stamp)) {
    seed = stamp;
    seeded = true;
  }
#if defined(__linux__) && defined(STATX_BTIME)
  if (!seeded) {
    struct statx stx;
    if (statx(fd, "", AT_EMPTY_PATH, STATX_BTIME, &stx) == 0 &&
        (stx.stx_mask & STATX_BTIME))
      seed = (time_t)stx.stx_btime.tv_sec;
  }
#elif defined(__APPLE__)
  (void)fd;
  if (!seeded && st->st_birthtimespec.tv_sec > 0)
    seed = st->st_birthtimespec.tv_sec;
#elif defined(__FreeBSD__)
  (void)fd;
  if (!seeded && st->st_birthtim.tv_sec > 0) seed = st->st_birthtim.tv_sec;
#else
  (void)fd;
  (void)seeded;
#endif
  if (seed < sh->last_rotation) sh->last_rotation = seed;
}

static clog clog_file_register_or_attach(clog h, clog_shared *sh,
                                         clog_level min_level);

// Creates a logger that appends to the file at path, the public entry for log
// files. With rotation on it opens the directory first and the file relative to
// it, so a later chdir() doesn't move the log, and it removes stale private
// names, recovers unfinished compressions and seeds the byte count and the time
// interval from the file. Rotation fields left at 0 get their defaults
// (max_rotated_files too, so a zeroed config doesn't keep files for ever).
// Async setup comes last. Finally it joins or refuses another target of the
// same file (see clog_file_register_or_attach()). Gives the handle or
// CLOG_NULL.
clog clog_create_from_file_path_mp(const char *path, clog_level min_level,
                                   const clog_rotation_cfg *cfg,
                                   const clog_async_cfg *async_cfg,
                                   ccol_memmgmt_procs *mprocs) {
  if (!path || !clog_level_is_valid(min_level)) return CLOG_NULL;
  if (!ccol__verify_memmgmt_procs(mprocs, (char **)NULL)) return CLOG_NULL;

  int dir_fd = -1;
  int fd;
  if (cfg && (cfg->size_rotation_enabled || cfg->time_rotation_enabled)) {
    dir_fd = clog_open_log_dir(path);
    if (dir_fd < 0) return CLOG_NULL;
    fd = openat(dir_fd, path_base(path),
                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  } else {
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  }
  if (fd < 0) {
    if (dir_fd >= 0) close(dir_fd);
    return CLOG_NULL;
  }

  struct clogger *lg = alloc(fd, true, path, dir_fd, min_level, mprocs);
  if (!lg) {
    close(fd);
    if (dir_fd >= 0) close(dir_fd);
    return CLOG_NULL;
  }

  {
    int id_fd = dir_fd >= 0 ? dir_fd : clog_open_log_dir(path);
    struct stat dir_st;
    if (id_fd >= 0 && fstat(id_fd, &dir_st) == 0) {
      lg->shared->file_id_known = true;
      lg->shared->file_dir_dev = dir_st.st_dev;
      lg->shared->file_dir_ino = dir_st.st_ino;
    }
    if (id_fd >= 0 && id_fd != dir_fd) close(id_fd);
  }

  if (cfg) {
    lg->shared->rotation_enabled =
        cfg->size_rotation_enabled || cfg->time_rotation_enabled;
    lg->shared->rotation = *cfg;

    if (cfg->size_rotation_enabled && cfg->max_file_size <= 0)
      lg->shared->rotation.max_file_size = CLOG_DEFAULT_MAX_FILE_SIZE;

    if (cfg->time_rotation_enabled && cfg->rotation_interval_us == 0)
      lg->shared->rotation.rotation_interval_us =
          CLOG_DEFAULT_ROTATION_INTERVAL_US;

    if (cfg->max_rotated_files <= 0)
      lg->shared->rotation.max_rotated_files = CLOG_DEFAULT_MAX_ROTATED_FILES;

    struct stat st;
    bool have_size = false;
    if (fstat(fd, &st) == 0) {
      lg->shared->bytes_written = st.st_size;
      have_size = true;
      if (st.st_size > 0 && cfg->time_rotation_enabled)
        clog_seed_last_rotation(lg->shared, fd, &st);
    } else {
      off_t end = lseek(fd, 0, SEEK_END);
      if (end != (off_t)-1) {
        lg->shared->bytes_written = end;
        have_size = true;
      }
    }
    if (!have_size && cfg->size_rotation_enabled) {
      clog_shared *sh = lg->shared;
      shared_close_owned_fd(sh);
      ccol__mutex_destroy(sh->mutex);
      logger_free(lg);
      shared_free_partial(sh);
      return CLOG_NULL;
    }
  }

  if (async_cfg && !shared_async_init(lg->shared, async_cfg)) {
    clog_shared *sh = lg->shared;
    shared_close_owned_fd(sh);
    ccol__mutex_destroy(sh->mutex);
    logger_free(lg);
    shared_free_partial(sh);
    return CLOG_NULL;
  }

  clog h = clog_handle_acquire(lg);
  if (h == CLOG_NULL) {
    clog_shared *sh = lg->shared;
    if (sh->async_enabled) shared_async_teardown(sh);
    shared_compressor_teardown(sh);
    shared_close_owned_fd(sh);
    ccol__mutex_destroy(sh->mutex);
    logger_free(lg);
    shared_free_partial(sh);
    return CLOG_NULL;
  }
  return clog_file_register_or_attach(h, lg->shared, min_level);
}

// Tears down a target whose last handle is gone, for clog_close() and for an
// open that attached and then failed. It leaves live_shareds first (before
// touching sh->mutex, for the fork walk), stops the writer thread without
// holding the mutex (the thread needs it to drain) and before the fd closes,
// then the compressor, pays an owed repair with a bounded write, closes the
// descriptors, leaves file_shareds and frees sh. The caller holds no lock.
static void clog_shared_finish_last(clog_shared *sh) {
  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
  clog_shareds_remove(clog_slot_table.live_shareds, sh);
  ccol__rw_lock_unlock(clog_slot_table.rwlock);

  if (sh->async_enabled) shared_async_teardown(sh);

  shared_compressor_teardown(sh);

  ccol__mutex_lock(sh->mutex);
  if (sh->async_partial_record & CLOG_OWED_REPAIR)
    (void)clog_pay_owed(sh, sh->format, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
  ccol__mutex_unlock(sh->mutex);

  shared_close_owned_fd(sh);
  if (sh->file_id_known) {
    ccol__rw_lock_wrlock(clog_slot_table.rwlock);
    clog_shareds_remove(clog_slot_table.file_shareds, sh);
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
  }
  ccol__mutex_destroy(sh->mutex);
  shared_free_partial(sh);
}

// Tells whether target b may join target a on the same file: The same rotation,
// async configuration and allocator once defaults are applied. Used by
// clog_file_register_or_attach().
static bool clog_file_targets_compatible(const clog_shared *a,
                                         const clog_shared *b) {
  if (a->rotation_enabled != b->rotation_enabled) return false;
  if (a->rotation_enabled) {
    const clog_rotation_cfg *x = &a->rotation, *y = &b->rotation;
    if (x->size_rotation_enabled != y->size_rotation_enabled ||
        x->time_rotation_enabled != y->time_rotation_enabled ||
        x->max_rotated_files != y->max_rotated_files ||
        x->compress_rotated != y->compress_rotated)
      return false;
    if (x->size_rotation_enabled && x->max_file_size != y->max_file_size)
      return false;
    if (x->time_rotation_enabled &&
        x->rotation_interval_us != y->rotation_interval_us)
      return false;
  }
  if (a->async_enabled != b->async_enabled) return false;
  if (a->async_enabled &&
      (a->async_cfg.queue_size != b->async_cfg.queue_size ||
       a->async_cfg.flush_buffer_size != b->async_cfg.flush_buffer_size ||
       a->async_cfg.flush_interval_us != b->async_cfg.flush_interval_us))
    return false;
  if (!a->m_procs || !b->m_procs) return !a->m_procs && !b->m_procs;
  return a->m_procs->malloc == b->m_procs->malloc &&
         a->m_procs->free == b->m_procs->free &&
         a->m_procs->calloc == b->m_procs->calloc &&
         a->m_procs->realloc == b->m_procs->realloc;
}

// Drops one reference to the live target sh and tears it down when it was the
// last one, for the rollback of an attaching open. The teardown counts as a
// close in flight, since it uses the table's vectors.
static void clog_shared_unref(clog_shared *sh) {
  ccol__mutex_lock(sh->mutex);
  int remaining = --sh->ref_count;
  ccol__mutex_unlock(sh->mutex);
  if (remaining != 0) return;
  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight++;
  ccol__rw_lock_unlock(clog_slot_table.rwlock);
  clog_shared_finish_last(sh);
  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight--;
  clog_release_slot_table_if_deferred_locked();
  ccol__rw_lock_unlock(clog_slot_table.rwlock);
}

// The last step of clog_create_from_file_path_mp(), which keeps one target per
// file whenever either open rotates, since two targets renaming and pruning the
// same file lose records. h is the new logger on its new target sh. With no
// other target on the same directory entry, sh is registered in file_shareds
// (after its compression recovery, under the table lock, so no other handle can
// rotate meanwhile) and h is returned. Otherwise, if either rotates, the new
// target is closed and the open joins the existing one with a new logger at
// min_level when the configurations match, or fails with CLOG_NULL. Two targets
// that don't rotate stay independent. A target whose last close is still
// running is waited for. Takes the table's write lock.
static clog clog_file_register_or_attach(clog h, clog_shared *sh,
                                         clog_level min_level) {
  if (!sh->file_id_known) return h;
  unsigned int delay_us = 1000;
  for (;;) {
    ccol__rw_lock_wrlock(clog_slot_table.rwlock);
    clog_shared *match = NULL;
    size_t n = cvector_size(clog_slot_table.file_shareds);
    for (size_t i = 0; i < n && !match; i++) {
      clog_shared *fs =
          *(clog_shared **)cvector_at(clog_slot_table.file_shareds, i);
      if (fs->file_dir_dev == sh->file_dir_dev &&
          fs->file_dir_ino == sh->file_dir_ino &&
          strcmp(fs->file_base, sh->file_base) == 0 &&
          (fs->rotation_enabled || sh->rotation_enabled))
        match = fs;
    }
    if (!match) {
      if (sh->rotation_enabled) {
        ccol__mutex_lock(sh->mutex);
        clog_remove_stale_live_tmps(sh);
        ccol__mutex_unlock(sh->mutex);
      }
      if (sh->rotation_enabled && sh->rotation.compress_rotated) {
#ifdef RUNNING_UNIT_TESTS
        clog_test_recover_gate();
#endif
        ccol__mutex_lock(sh->mutex);
        clog_recover_compressions(sh);
        ccol__mutex_unlock(sh->mutex);
      }
      bool ok =
          cvector_push_back(clog_slot_table.file_shareds, &sh) == ccol_success;
      ccol__rw_lock_unlock(clog_slot_table.rwlock);
      if (!ok) {
        clog_close(h);
        return CLOG_NULL;
      }
      return h;
    }
    ccol__mutex_lock(match->mutex);
    int refs = match->ref_count;
    bool compatible = refs > 0 && clog_file_targets_compatible(match, sh);
    if (compatible) match->ref_count++;
    ccol__mutex_unlock(match->mutex);
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
    if (refs == 0) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&clog_test_closing_target_waits_state, 1);
#endif
      usleep(delay_us);
      if (delay_us < 100000) delay_us *= 2;
      continue;
    }
    clog_close(h);
    if (!compatible) return CLOG_NULL;
    struct clogger *joined = logger_alloc(match, min_level);
    if (!joined) {
      clog_shared_unref(match);
      return CLOG_NULL;
    }
    clog jh = clog_handle_acquire(joined);
    if (jh == CLOG_NULL) {
      logger_free(joined);
      clog_shared_unref(match);
    }
    return jh;
  }
}

static bool clog_exit_lock_target(clog_shared *sh,
                                  const struct timespec *deadline);
static struct timespec clog_ts_after_ms(const struct timespec *base,
                                        unsigned int ms);

// Does steps 5 and 6 of clog_close() for a target a drain left because its
// writer thread didn't answer, which may hold the mutex for ever. It takes the
// mutex only within a short bound, and the last handle only takes the target
// out of the module's tables instead of joining the thread; the process is
// ending anyway.
static void clog_close_left_target(clog_shared *sh) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec deadline =
      clog_ts_after_ms(&now, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
  if (!clog_exit_lock_target(sh, &deadline)) return;
  int remaining = --sh->ref_count;
  ccol__mutex_unlock(sh->mutex);
  if (remaining != 0) return;
  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
  clog_shareds_remove(clog_slot_table.live_shareds, sh);
  if (sh->file_id_known) clog_shareds_remove(clog_slot_table.file_shareds, sh);
  ccol__rw_lock_unlock(clog_slot_table.rwlock);
}

// Closes h, the public end of a logger. Steps: Under the write lock it marks
// the slot not in use (a second close is fatal) and counts a close in flight;
// it waits, polling without a bound, until no call still pins the handle
// (giving up would free memory a caller still uses); under the write lock again
// it frees the logger and retires the slot, so the fork walk never sees a
// destroyed fields_mutex; it drops the target's reference, and the last handle
// tears the target down. Last of all it releases the table when the exit
// destructor deferred that, which is why the close counts in flight until then.
// A stale handle is fatal.
void clog_close(clog h) {
  struct clogger *raw;
  clog_shared *sh;

  {
    ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
    if (h == 0)
      ccol__fatal_err(
          "clog_close: clog handle is invalid, stale, or already closed");
    uint32_t idx = (uint32_t)(h >> 32);
    uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
    ccol__rw_lock_wrlock(clog_slot_table.rwlock);
    clog_slot *slot = NULL;
    if (clog_slot_table.slots != NULL &&
        idx < cvector_size(clog_slot_table.slots))
      slot = (clog_slot *)cvector_at(clog_slot_table.slots, idx);
    if (!slot || !slot->in_use || slot->generation != gen) {
      ccol__rw_lock_unlock(clog_slot_table.rwlock);
      ccol__fatal_err(
          "clog_close: clog handle is invalid, stale, or already closed");
    }
    raw = slot->ptr;
    sh = raw->shared;
    slot->in_use = false;
    clog_slot_table.closes_in_flight++;
    ccol_pintable_retire(&clog_pintable, idx);
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
  }

  {
    uint32_t idx = (uint32_t)(h >> 32);
    unsigned int delay_us = 1;
#ifdef RUNNING_UNIT_TESTS
    if (ccol_pintable_pins(&clog_pintable, idx) > 0)
      atomic_fetch_add(&clog_test_close_pin_waits_state, 1);
#endif
    while (ccol_pintable_pins(&clog_pintable, idx) > 0) {
      usleep(delay_us);
      if (delay_us < 1000) delay_us *= 2;
    }
  }

#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&clog_test_close_finalize_hold)) {
    atomic_store(&clog_test_close_finalize_entered_state, true);
    for (unsigned int waited = 0; atomic_load(&clog_test_close_finalize_hold) &&
                                  waited < CLOG_TEST_CLOSE_HOLD_MAX_US;
         waited += 1000)
      usleep(1000);
  }
#endif

  {
    ccol__rw_lock_wrlock(clog_slot_table.rwlock);
    uint32_t idx = (uint32_t)(h >> 32);
    clog_slot *slot = (clog_slot *)cvector_at(clog_slot_table.slots, idx);
    logger_free(raw);
    slot->freed = true;
    slot->ptr = NULL;
    slot->generation++;
    if (slot->generation == 0) slot->generation++;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    ccol__rw_lock_unlock(clog_slot_table.rwlock);
  }

#ifdef RUNNING_UNIT_TESTS
  {
    unsigned int w = atomic_load(&clog_test_close_release_window_us);
    if (w) {
      atomic_store(&clog_test_close_release_window_entered_state, true);
      usleep(w);
    }
  }
#endif

  if (atomic_load_explicit(&sh->drain_left, memory_order_acquire)) {
    clog_close_left_target(sh);
  } else {
    ccol__mutex_lock(sh->mutex);
    int remaining = --sh->ref_count;
    ccol__mutex_unlock(sh->mutex);

    if (remaining == 0) clog_shared_finish_last(sh);
  }

  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight--;
  clog_release_slot_table_if_deferred_locked();
  ccol__rw_lock_unlock(clog_slot_table.rwlock);
}

// Creates a child logger that shares parent_h's output and starts with a copy
// of its level and fields; the public way to add context per component. A
// failure is fatal. See clog__derive_checked().
clog clog_derive(clog parent_h) {
  ccol_retval rv = ccol_success;
  clog h = clog__derive_checked(parent_h, &rv);
  if (rv == ccol_invalid_args)
    ccol__fatal_err(
        "clog_derive: clog handle is invalid, stale, or already closed");
  return h;
}

// Derives a logger from parent_h without a fatal error, for clog_derive() and
// the HTTP modules, which need to fail cleanly. The parent's pin keeps it and
// its target alive throughout, so a concurrent close comes before the resolve
// or waits. Fields are copied under the parent's fields_mutex only, in the
// order they were set; the reference count is raised under shared->mutex. Gives
// the handle, or CLOG_NULL with *rv set to ccol_invalid_args (bad, stale or
// closed parent) or ccol_not_enough_memory.
clog clog__derive_checked(clog parent_h, ccol_retval *rv) {
  *rv = ccol_invalid_args;
  struct clogger *parent = clog_resolve(parent_h);
  if (!parent) return CLOG_NULL;
  *rv = ccol_not_enough_memory;

  struct clogger *child = logger_alloc(parent->shared, parent->min_level);
  if (!child) {
    clog_resolve_unpin(parent);
    return CLOG_NULL;
  }

  ccol__mutex_lock(parent->fields_mutex);
  {
    ccol_chmap_walk walk = {0};
    const void *kv;
    const void *vv;
    while (ccol_chmap_walk_next(parent->fields, &walk, &kv, &vv)) {
      const char *k = (const char *)kv;
      const char *v = (const char *)vv;
      cmap_pair kp = {.ptr = (void *)k, .size = strlen(k) + 1};
      cmap_pair vp = {.ptr = (void *)v, .size = strlen(v) + 1};
      ccol_retval rv = chashmap_insert_elem(child->fields, &kp, &vp);
      if (rv != ccol_success && rv != ccol_key_already_present) {
        ccol__mutex_unlock(parent->fields_mutex);
        logger_free(child);
        clog_resolve_unpin(parent);
        return CLOG_NULL;
      }
    }
  }
  ccol__mutex_unlock(parent->fields_mutex);

  ccol__mutex_lock(parent->shared->mutex);
  parent->shared->ref_count++;
  ccol__mutex_unlock(parent->shared->mutex);

  clog h = clog_handle_acquire(child);
  if (h == CLOG_NULL) {
    ccol__mutex_lock(parent->shared->mutex);
    parent->shared->ref_count--;
    ccol__mutex_unlock(parent->shared->mutex);
    logger_free(child);
    clog_resolve_unpin(parent);
    return CLOG_NULL;
  }

  clog_resolve_unpin(parent);
  *rv = ccol_success;
  return h;
}

// Sets h's minimum level. The level is an atomic of this logger alone, so no
// lock is needed and siblings aren't affected. Returns ccol_invalid_args for a
// value outside the enum; a bad handle is fatal.
ccol_retval clog_set_level(clog h, clog_level level) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_set_level: clog handle is invalid, stale, or already closed");

  ccol_retval rv = ccol_invalid_args;
  if (clog_level_is_valid(level)) {
    lg->min_level = level;
    rv = ccol_success;
  }
  clog_resolve_unpin(lg);
  return rv;
}

// Gives h's minimum level, read atomically, so a caller can check what h
// records. A bad handle is fatal.
clog_level clog_get_level(clog h) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_get_level: clog handle is invalid, stale, or already closed");

  clog_level l = lg->min_level;
  clog_resolve_unpin(lg);
  return l;
}

// Sets the output format of h's target (every logger sharing it). Syslog is
// refused for a file logger, since it needs a caller's socket. Returns
// ccol_invalid_args for those cases and leaves the format alone. Takes the
// target's mutex.
ccol_retval clog_set_format(clog h, clog_format fmt) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_set_format: clog handle is invalid, stale, or already closed");

  if (!clog_format_is_valid(fmt)) {
    clog_resolve_unpin(lg);
    return ccol_invalid_args;
  }
  ccol_retval rv = ccol_invalid_args;
  ccol__mutex_lock(lg->shared->mutex);
  if (fmt != CLOG_FMT_SYSLOG || !lg->shared->owns_fd) {
    lg->shared->format = fmt;
    rv = ccol_success;
  }
  ccol__mutex_unlock(lg->shared->mutex);
  clog_resolve_unpin(lg);
  return rv;
}

// Gives the output format of h's target, read under its mutex. A bad handle is
// fatal.
clog_format clog_get_format(clog h) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_get_format: clog handle is invalid, stale, or already closed");

  ccol__mutex_lock(lg->shared->mutex);
  clog_format f = lg->shared->format;
  ccol__mutex_unlock(lg->shared->mutex);
  clog_resolve_unpin(lg);
  return f;
}

// Sets the syslog facility of h's target, which goes into the PRI of every
// later syslog record. Returns ccol_invalid_args for a value outside 0 to 11
// and 16 to 23. Takes the target's mutex.
ccol_retval clog_set_facility(clog h, clog_syslog_facility facility) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_set_facility: clog handle is invalid, stale, or already closed");

  if (!clog_facility_is_valid(facility)) {
    clog_resolve_unpin(lg);
    return ccol_invalid_args;
  }
  ccol__mutex_lock(lg->shared->mutex);
  lg->shared->syslog_facility = facility;
  ccol__mutex_unlock(lg->shared->mutex);
  clog_resolve_unpin(lg);
  return ccol_success;
}

// Gives the syslog facility of h's target, read under its mutex. A bad handle
// is fatal.
clog_syslog_facility clog_get_facility(clog h) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_get_facility: clog handle is invalid, stale, or already closed");

  ccol__mutex_lock(lg->shared->mutex);
  clog_syslog_facility f = lg->shared->syslog_facility;
  ccol__mutex_unlock(lg->shared->mutex);
  clog_resolve_unpin(lg);
  return f;
}

// Sets a field that every later record of h carries. The key must be 1 or more
// PRINTUSASCII characters and not a reserved key, so it can't break logfmt or
// an SD-PARAM-NAME. It works under h's fields_mutex, so siblings' writes don't
// contend. Returns ccol_success (a replaced value too), ccol_invalid_args, or
// ccol_not_enough_memory with the fields unchanged.
ccol_retval clog_set_field(clog h, const char *key, const char *value) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_set_field: clog handle is invalid, stale, or already closed");

  bool args_ok = key && value && *key != '\0' && !is_reserved_field_key(key);
  for (const char *p = key; args_ok && *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x21 || c > 0x7e || *p == '=' || *p == '"' || *p == '\\' ||
        *p == ']')
      args_ok = false;
  }
  if (!args_ok) {
    clog_resolve_unpin(lg);
    return ccol_invalid_args;
  }

  ccol__mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};
  ccol_retval rv = chashmap_insert_elem(lg->fields, &kp, &vp);

  ccol__mutex_unlock(lg->fields_mutex);
  clog_resolve_unpin(lg);
  return rv == ccol_key_already_present ? ccol_success : rv;
}

// Removes field key from h, so later records stop carrying it. Works under h's
// fields_mutex.
void clog_remove_field(clog h, const char *key) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_remove_field: clog handle is invalid, stale, or already closed");

  if (!key) {
    clog_resolve_unpin(lg);
    return;
  }

  ccol__mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  chashmap_remove_elem(lg->fields, &kp);

  ccol__mutex_unlock(lg->fields_mutex);
  clog_resolve_unpin(lg);
}

// Removes every field of h, so later records carry none. Works under h's
// fields_mutex.
void clog_clear_fields(clog h) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_clear_fields: clog handle is invalid, stale, or already closed");

  ccol__mutex_lock(lg->fields_mutex);
  chashmap_reset(lg->fields, 0);
  ccol__mutex_unlock(lg->fields_mutex);
  clog_resolve_unpin(lg);
}

static bool clog_exit_lock_target(clog_shared *sh,
                                  const struct timespec *deadline);
static struct timespec clog_after_drain_deadline(clog_shared *sh);
static bool clog_fatal_lock_target(clog_shared *sh,
                                   const struct timespec *deadline);

// Writes a CLOG_FATAL record like clog_write_record() and tells whether every
// byte went out, so the fatal path can report a loss on stderr. Its writes are
// already bounded by then.
static __attribute__((noinline, cold)) bool clog_fatal_write_record(
    clog_shared *sh, clog_format fmt, const char *data, size_t len) {
  if ((sh->async_partial_record & CLOG_OWED_REPAIR) &&
      !clog_owed_gate(sh, fmt, len, CLOG_WRITE_WAIT_FOREVER))
    return false;
  size_t written = write_all(sh, data, len, CLOG_WRITE_WAIT_FOREVER);
  if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  if (written < len)
    clog_note_truncated_record(sh, fmt, data, written, len, errno);
  return written == len;
}

// Builds and writes one record synchronously, under the target's mutex, which
// it takes itself (the caller must not hold it). It's instantiated twice with
// fatal as a constant: For ordinary records and for a CLOG_FATAL record, which
// skips the level filter, takes the mutex within deadline, marks a sink that
// isn't a regular file as bounded, and returns false when the record didn't go
// out whole. The ordinary records re-check the level under the lock. The
// message is formatted on the stack and spills to the heap (ap2 is its copy).
// It settles a split async record and pays an owed repair first, runs the time
// rotation check before the write and the size check after, and writes syslog
// frames after the record. bt_syms is only read; the caller frees it.
static inline __attribute__((always_inline, format(printf, 9, 0))) bool
clog_write_sync_body(struct clogger *lg, clog_level level, const char *file,
                     int line, const char *func, bool with_backtrace,
                     char *const *bt_syms, int bt_depth, const char *fmt,
                     va_list ap, va_list ap2, bool fatal,
                     const struct timespec *deadline) {
  bool delivered = true;
  const char *proc_val = clog_tident_get()->proc;

  if (fatal) {
    if (!clog_fatal_lock_target(lg->shared, deadline)) return false;
    struct stat st;
    if (lg->shared->sink_kind != CLOG_SINK_PLAIN ||
        (lg->shared->fd >= 0 && fstat(lg->shared->fd, &st) == 0 &&
         !S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)))
      lg->shared->sink_kind |= CLOG_SINK_FATAL_BOUND;
  } else {
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&lg->shared->test_sync_lock_waiters, 1);
#endif
    ccol__mutex_lock(lg->shared->mutex);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_sub(&lg->shared->test_sync_lock_waiters, 1);
#endif
  }

  if (level < lg->min_level && level != CLOG_FATAL) {
    ccol__mutex_unlock(lg->shared->mutex);
    return true;
  }

  if (lg->shared->fd < 0) {
    ccol__mutex_unlock(lg->shared->mutex);
    return false;
  }
#ifdef RUNNING_UNIT_TESTS
  if (!fatal) atomic_fetch_add(&lg->shared->test_sync_records_locked, 1);
#endif

  const clog_format log_fmt = lg->shared->format;

  char msg_stack[1024];
  char *msg = msg_stack;
  char *msg_heap = NULL;

  int mlen = vsnprintf(msg_stack, sizeof(msg_stack), fmt, ap);

  if (mlen < 0) {
    static const char fmt_err_msg[] = "<log message formatting failed>";
    memcpy(msg_stack, fmt_err_msg, sizeof(fmt_err_msg));
  } else if ((size_t)mlen >= sizeof(msg_stack)) {
    msg_heap = ccol__mem_alloc(lg->shared->m_procs, (size_t)mlen + 1);
    if (msg_heap) {
      int mlen2 = vsnprintf(msg_heap, (size_t)mlen + 1, fmt, ap2);
      if (mlen2 >= 0) msg = msg_heap;
    }
    if (msg == msg_stack) {
      static const char trunc_marker[] = "...[truncated]";
      size_t mark_len = sizeof(trunc_marker) - 1;
      if (mark_len < sizeof(msg_stack) - 1)
        memcpy(msg_stack + sizeof(msg_stack) - 1 - mark_len, trunc_marker,
               mark_len);
    }
  }

  if (lg->shared->async_partial_record)
    clog_flush_and_settle(lg->shared, log_fmt, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);

  clog_time_rotate_if_due(lg->shared);

  struct timeval ts;
  gettimeofday(&ts, NULL);

  buf_reset(&lg->buf);
  clog_field_source fsrc;
  fsrc.is_live = true;
  fsrc.u.live.fields = lg->fields;
  fsrc.u.live.mutex = &lg->fields_mutex;
  bool built = clog_build_record(&lg->buf, lg->shared, log_fmt, level, &ts,
                                 proc_val, file, line, func, msg, &fsrc,
                                 with_backtrace, bt_syms, bt_depth, NULL);

  ccol__mem_free(lg->shared->m_procs, msg_heap);

  if (lg->shared->fd >= 0) {
    if (built) {
      if (fatal)
        delivered = clog_fatal_write_record(lg->shared, log_fmt, lg->buf.data,
                                            lg->buf.len);
      else
        clog_write_record(lg->shared, log_fmt, lg->buf.data, lg->buf.len);
    } else {
      clog_write_unrepresentable_record(lg->shared, log_fmt, level,
                                        with_backtrace);
    }
  }

  if (with_backtrace && lg->shared->fd >= 0 && log_fmt == CLOG_FMT_SYSLOG)
    emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, bt_syms,
                                bt_depth);

  clog_size_rotate_if_due(lg->shared);

  ccol__mutex_unlock(lg->shared->mutex);
  return delivered;
}

// Writes one ordinary record synchronously through clog_write_sync_body(): The
// path of every non-async target, and the fallback of clog_write_async() when
// the envelope can't be allocated.
static void __attribute__((format(printf, 9, 0))) clog_write_sync(
    struct clogger *lg, clog_level level, const char *file, int line,
    const char *func, bool with_backtrace, char *const *bt_syms, int bt_depth,
    const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  (void)clog_write_sync_body(lg, level, file, line, func, with_backtrace,
                             bt_syms, bt_depth, fmt, ap, ap2, false, NULL);
  va_end(ap2);
}

static void clog_flush_pinned(struct clogger *lg);

// The static, empty envelope of a wake, so waking the writer allocates nothing
// on a bounded queue.
static clog_async_msg clog_wake_envelope = {.kind = CLOG_ASYNC_MSG_WAKE};

// Wakes sh's writer thread after a thread that wrote a record itself left work
// for it (kept batch bytes, an owed repair), since an idle writer waits without
// a limit. A refused send needs nothing more: A full queue wakes the writer
// anyway, sends off belong to a drain that settles the target, and an unbounded
// queue out of memory leaves the work to the next record, flush, close or exit
// drain.
static __attribute__((cold)) void clog_wake_writer(clog_shared *sh) {
  cmessage m = {.data = &clog_wake_envelope,
                .size = sizeof(clog_wake_envelope)};
  if (sh->is_bounded_queue)
    (void)ccircq_try_send(sh->q.circq, &m);
  else
    (void)cdynmq_send(sh->q.dynmq, &m);
}

// Formats one record into a job and queues it for the writer thread, the path
// of every async target. Fields and identity are captured now. Without an
// envelope the call goes to clog_write_sync(). When the send fails (no memory,
// or sends turned off by a drain), the record is written directly from the job
// under the mutex, after clog_flush_pinned() so it can't land ahead of records
// already queued; it joins the batch behind kept records when it can, and runs
// the rotation checks and the settle like any direct write. Only on the exit
// paths is a record dropped: When a drain left the target, or when the mutex
// doesn't come before the drain's deadline (an unbounded wait would keep the
// handle pinned and hang a later close).
static void __attribute__((format(printf, 9, 0))) clog_write_async(
    struct clogger *lg, clog_level level, const char *file, int line,
    const char *func, bool with_backtrace, char **bt_syms, int bt_depth,
    const char *fmt, va_list ap) {
  clog_async_msg *envelope =
      ccol__mem_calloc(lg->shared->m_procs, 1, sizeof(*envelope));
  if (!envelope) {
    if (atomic_load_explicit(&lg->shared->drain_left, memory_order_acquire)) {
      if (bt_syms) free(bt_syms);
      return;
    }
    clog_flush_pinned(lg);
    clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                    bt_depth, fmt, ap);
    if (bt_syms) free(bt_syms);
    clog_wake_writer(lg->shared);
    return;
  }
  envelope->kind = CLOG_ASYNC_MSG_JOB;
  clog_async_job *job = &envelope->u.job;

  job->bt_syms = bt_syms;
  job->bt_depth = bt_depth;
  job->level = level;
  job->file = file;
  job->line = line;
  job->func = func;
  job->with_backtrace = with_backtrace;
  gettimeofday(&job->ts, NULL);
  _Static_assert(sizeof(job->proc_val) == CLOG_PROC_VAL_LEN,
                 "a job holds the whole cached identity string");
  memcpy(job->proc_val, clog_tident_get()->proc, CLOG_PROC_VAL_LEN);

  job->msg_heap = NULL;
  va_list ap2;
  va_copy(ap2, ap);
  int mlen = vsnprintf(job->msg_inline, sizeof(job->msg_inline), fmt, ap);
  if (mlen < 0) {
    static const char fmt_err_msg[] = "<log message formatting failed>";
    memcpy(job->msg_inline, fmt_err_msg, sizeof(fmt_err_msg));
  } else if ((size_t)mlen >= sizeof(job->msg_inline)) {
    job->msg_heap = ccol__mem_alloc(lg->shared->m_procs, (size_t)mlen + 1);
    if (job->msg_heap) {
      int mlen2 = vsnprintf(job->msg_heap, (size_t)mlen + 1, fmt, ap2);
      if (mlen2 < 0) {
        ccol__mem_free(lg->shared->m_procs, job->msg_heap);
        job->msg_heap = NULL;
      }
    }
    if (!job->msg_heap) {
      static const char trunc_marker[] = "...[truncated]";
      size_t mark_len = sizeof(trunc_marker) - 1;
      if (mark_len < sizeof(job->msg_inline) - 1)
        memcpy(job->msg_inline + sizeof(job->msg_inline) - 1 - mark_len,
               trunc_marker, mark_len);
    }
  }
  va_end(ap2);
  job->msg = job->msg_heap ? job->msg_heap : job->msg_inline;

  snapshot_fields(lg, &job->fields, &job->field_count, &job->field_pool,
                  &job->fields_snapshot_failed);

  cmessage m = {.data = envelope, .size = sizeof(*envelope)};
  ccol_retval rv = lg->shared->is_bounded_queue
                       ? ccircq_send(lg->shared->q.circq, &m)
                       : cdynmq_send(lg->shared->q.dynmq, &m);
  if (rv != ccol_success &&
      atomic_load_explicit(&lg->shared->drain_left, memory_order_acquire)) {
    clog_async_job_release(lg->shared, envelope);
    return;
  }
  if (rv != ccol_success) {
    clog_flush_pinned(lg);

    if (rv == ccol_not_permitted) {
      struct timespec dl = clog_after_drain_deadline(lg->shared);
      if (!clog_exit_lock_target(lg->shared, &dl)) {
        clog_async_job_release(lg->shared, envelope);
        return;
      }
    } else {
      ccol__mutex_lock(lg->shared->mutex);
    }
    if (lg->shared->fd >= 0) {
      clog_format fmt2 = lg->shared->format;
      clog_field_source fsrc;
      fsrc.is_live = false;
      fsrc.u.snap.views = job->fields;
      fsrc.u.snap.count = job->field_count;
      fsrc.u.snap.pool = job->field_pool.buf;
      fsrc.u.snap.failed = job->fields_snapshot_failed;

      clog_shared *wsh = lg->shared;
      if (wsh->async_buf.len > 0)
        clog_flush_and_settle(wsh, fmt2, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
      if (wsh->async_buf.len > 0 && fmt2 != CLOG_FMT_SYSLOG &&
          !wsh->sink_per_record) {
        size_t record_start = wsh->async_buf.len;
        bool used_fallback = false;
        if (clog_build_record(&wsh->async_buf, wsh, fmt2, job->level, &job->ts,
                              job->proc_val, job->file, job->line, job->func,
                              job->msg, &fsrc, job->with_backtrace,
                              job->bt_syms, job->bt_depth, &used_fallback) &&
            !used_fallback) {
          ccol__mutex_unlock(wsh->mutex);
          clog_async_job_release(wsh, envelope);
          return;
        }
        wsh->async_buf.len = record_start;
      }

      clog_time_rotate_if_due(lg->shared);

      buf_reset(&lg->buf);
      bool built = clog_build_record(
          &lg->buf, lg->shared, fmt2, job->level, &job->ts, job->proc_val,
          job->file, job->line, job->func, job->msg, &fsrc, job->with_backtrace,
          job->bt_syms, job->bt_depth, NULL);
      if (built) {
        clog_write_record(lg->shared, fmt2, lg->buf.data, lg->buf.len);
      } else {
        clog_write_unrepresentable_record(lg->shared, fmt2, job->level,
                                          job->with_backtrace);
      }
      if (job->with_backtrace && fmt2 == CLOG_FMT_SYSLOG)
        emit_backtrace_syslog_lines(&lg->buf, lg->shared, job->level, &job->ts,
                                    job->bt_syms, job->bt_depth);

      clog_size_rotate_if_due(lg->shared);
    }
    ccol__mutex_unlock(lg->shared->mutex);

    clog_async_job_release(lg->shared, envelope);
    clog_wake_writer(lg->shared);
  }
}

#ifdef RUNNING_UNIT_TESTS

// Force the mutex or condition variable initialization of the next
// clog_flush_pinned() to fail, which glibc gives no way to provoke. Each
// disarms itself.
static _Atomic bool clog_test_force_flush_mutex_init_failure_state = false;
static _Atomic bool clog_test_force_flush_condvar_init_failure_state = false;

// Take one forced init failure of the flush request's mutex or condition
// variable, for clog_flush_pinned() in a test build; true when one was armed.
static bool clog_test_consume_forced_flush_mutex_init_failure(void) {
  return atomic_exchange(&clog_test_force_flush_mutex_init_failure_state,
                         false);
}
static bool clog_test_consume_forced_flush_condvar_init_failure(void) {
  return atomic_exchange(&clog_test_force_flush_condvar_init_failure_state,
                         false);
}

// Arm or disarm the forced failure of the next flush request's mutex or
// condition variable init, so a test can check that the flush returns at once.
void clog__test_force_flush_mutex_init_failure(bool force) {
  atomic_store(&clog_test_force_flush_mutex_init_failure_state, force);
}
void clog__test_force_flush_condvar_init_failure(bool force) {
  atomic_store(&clog_test_force_flush_condvar_init_failure_state, force);
}
#else

// Stubs of a non-test build, so clog_flush_pinned() needs no #ifdef: No init
// failure is forced.
static inline bool clog_test_consume_forced_flush_mutex_init_failure(void) {
  return false;
}
static inline bool clog_test_consume_forced_flush_condvar_init_failure(void) {
  return false;
}
#endif

static void clog_flush_after_drain(clog_shared *sh);

// Flushes lg's async target: Queues a flush request and waits until the writer
// thread has written everything queued before it. It's the body of
// clog_flush(), and clog_write_async() and the fatal path call it with a handle
// they already pinned (the pin is held through the wait, so a close waits for
// it). The request's mutex and condition variable live on this stack and are
// checked, since waiting on a broken one is undefined; a failed init or send
// returns at once. When a drain turned sends off, it waits, bounded, through
// the owner send for what the queue already holds. A target a drain left is
// skipped.
static void clog_flush_pinned(struct clogger *lg) {
  if (atomic_load_explicit(&lg->shared->drain_left, memory_order_acquire))
    return;
  clog_async_ctrl ctrl;
  if (clog_test_consume_forced_flush_mutex_init_failure() ||
      ccol__mutex_init(ctrl.mutex) != 0)
    return;
  if (clog_test_consume_forced_flush_condvar_init_failure() ||
      ccol__cond_var_init(ctrl.cv) != 0) {
    ccol__mutex_destroy(ctrl.mutex);
    return;
  }
  ctrl.done = false;

  clog_async_msg envelope;
  envelope.kind = CLOG_ASYNC_MSG_FLUSH;
  envelope.u.ctrl = &ctrl;
  cmessage m = {.data = &envelope, .size = sizeof(envelope)};
  ccol_retval rv = lg->shared->is_bounded_queue
                       ? ccircq_send(lg->shared->q.circq, &m)
                       : cdynmq_send(lg->shared->q.dynmq, &m);

  if (rv == ccol_success) {
    ccol__mutex_lock(ctrl.mutex);
    while (!ctrl.done) ccol__cond_var_wait(ctrl.cv, ctrl.mutex);
    ccol__mutex_unlock(ctrl.mutex);
  }
  ccol__mutex_destroy(ctrl.mutex);
  ccol__cond_var_destroy(ctrl.cv);
  if (rv == ccol_not_permitted) clog_flush_after_drain(lg->shared);
}

// Flushes h's async target and returns once every record queued before the call
// is written, so a caller can make sure its records reached the output. Does
// nothing for a synchronous logger.
void clog_flush(clog h) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog_flush: clog handle is invalid, stale, or already closed");
  if (lg->shared->async_enabled) clog_flush_pinned(lg);
  clog_resolve_unpin(lg);
}

// Makes the last delivery attempt for an async target's batch on the fatal and
// exit paths: One bounded flush, a settle of a split record, and a marker for
// whatever is still undelivered. timeout_ms bounds each write so an output that
// takes nothing can't hold the process. Needs sh->mutex.
static void clog_settle_batch_locked(clog_shared *sh, int timeout_ms) {
  if (sh->async_buf.len > 0) {
    writer_flush_buffer(sh, timeout_ms);
    clog_settle_after_flush(sh, sh->format, timeout_ms);
    clog_drop_undelivered(sh, "the process stopped", timeout_ms);
  }
}

static void clog_exit_drain(void);

// True once the exit drain is registered with atexit().
static bool clog_exit_drain_registered;

// Ends a CLOG_FATAL call after its record and unpin: Calls exit(), whose exit
// drain flushes every other async logger and finishes compressions within its
// budget. Runs the drain directly only when its registration failed.
static __attribute__((noinline, noreturn)) void clog_fatal_exit(void) {
  if (!clog_exit_drain_registered) clog_exit_drain();
  exit(EXIT_FAILURE);
}

// Exit drain: At exit() or a return from main() a still open async logger would
// lose its queue and batch and cut a compression off, so a handler registered
// at load drains every async target (queue, batch, a marker for what can't go
// out) and then the compressions. One budget bounds every wait and lock, so a
// stuck writer or a held lock delays exit by at most CLOG_EXIT_DRAIN_BUDGET_MS.
// A target that can't be locked in time, or whose own thread called exit(), is
// skipped. An abandoned compression gets CLOG_EXIT_ABANDON_GRACE_MS more to
// stop and remove its output.
#define CLOG_EXIT_DRAIN_BUDGET_MS 5000U
#define CLOG_EXIT_ABANDON_GRACE_MS 1000U

#ifdef RUNNING_UNIT_TESTS

// Budget a test sets for the exit drain; 0 keeps the default.
static _Atomic unsigned int clog_test_exit_drain_budget_ms = 0;

// Sets the exit drain budget, so a test of a stuck target runs fast; 0 restores
// CLOG_EXIT_DRAIN_BUDGET_MS.
void clog__test_set_exit_drain_budget_ms(unsigned int budget_ms) {
  atomic_store(&clog_test_exit_drain_budget_ms, budget_ms);
}

// Gives the exit drain budget in milliseconds; a test build can shorten it so a
// stuck-target test doesn't wait 5 s.
static unsigned int clog_exit_drain_budget_ms(void) {
  unsigned int b = atomic_load(&clog_test_exit_drain_budget_ms);
  return b ? b : CLOG_EXIT_DRAIN_BUDGET_MS;
}

// Locks the mutex of h's target and never unlocks it, so a test can make a
// stuck target.
void clog__test_lock_target_forever(clog h) {
  struct clogger *lg = clog_resolve(h);
  if (!lg) return;
  clog_shared *sh = lg->shared;
  clog_resolve_unpin(lg);
  ccol__mutex_lock(sh->mutex);
}

// Takes the handle table's write lock and never releases it, so a test can
// check that the exit drain gives up on it.
void clog__test_lock_table_forever(void) {
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
  ccol__rw_lock_wrlock(clog_slot_table.rwlock);
}

// While set, the drain's send to a bounded queue doesn't wait, as when the
// budget is spent.
static _Atomic bool clog_test_exit_drain_send_no_wait = false;

// Makes the exit drain's send to a bounded queue not wait while on is true, so
// a test can reach the full-queue path at once.
void clog__test_force_exit_drain_send_without_wait(bool on) {
  atomic_store(&clog_test_exit_drain_send_no_wait, on);
}

// The targets a drain left because their writer didn't answer, so a test can
// ask about one; a fixed table is enough.
#define CLOG_TEST_LEFT_TARGETS_LIMIT 16
static clog_shared *clog_test_left_targets[CLOG_TEST_LEFT_TARGETS_LIMIT];
static _Atomic size_t clog_test_left_targets_count = 0;

// Records that a drain left sh, so clog__test_exit_drain_left_target() can
// report it.
static void clog_test_note_left_target(clog_shared *sh) {
  size_t i = atomic_fetch_add(&clog_test_left_targets_count, 1);
  if (i < CLOG_TEST_LEFT_TARGETS_LIMIT) clog_test_left_targets[i] = sh;
}

// Tells whether a drain gave up on h's target, so a test can check that a stuck
// target is left.
bool clog__test_exit_drain_left_target(clog h) {
  struct clogger *lg = clog_resolve(h);
  if (!lg) return false;
  clog_shared *sh = lg->shared;
  clog_resolve_unpin(lg);
  size_t n = atomic_load(&clog_test_left_targets_count);
  if (n > CLOG_TEST_LEFT_TARGETS_LIMIT) n = CLOG_TEST_LEFT_TARGETS_LIMIT;
  for (size_t i = 0; i < n; i++)
    if (clog_test_left_targets[i] == sh) return true;
  return false;
}
#else

// Gives the exit drain budget in milliseconds, CLOG_EXIT_DRAIN_BUDGET_MS.
static inline unsigned int clog_exit_drain_budget_ms(void) {
  return CLOG_EXIT_DRAIN_BUDGET_MS;
}
#endif

// Gives the time ms milliseconds after base, for building the drain and fatal
// deadlines.
static struct timespec clog_ts_after_ms(const struct timespec *base,
                                        unsigned int ms) {
  struct timespec t = *base;
  t.tv_sec += (time_t)(ms / 1000U);
  t.tv_nsec += (long)(ms % 1000U) * 1000000L;
  if (t.tv_nsec >= 1000000000L) {
    t.tv_sec++;
    t.tv_nsec -= 1000000000L;
  }
  return t;
}

// Gives the time from now until the monotonic deadline, clamped at zero, for a
// timed condition wait of a drain.
static struct timespec clog_ts_until(const struct timespec *deadline) {
  struct timespec now, r = {0, 0};
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (now.tv_sec > deadline->tv_sec ||
      (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
    return r;
  r.tv_sec = deadline->tv_sec - now.tv_sec;
  r.tv_nsec = deadline->tv_nsec - now.tv_nsec;
  if (r.tv_nsec < 0) {
    r.tv_sec--;
    r.tv_nsec += 1000000000L;
  }
  return r;
}

// Gives the microseconds until the monotonic deadline, rounded up, or 0 once it
// passed; the timeout of a drain's queue send or receive.
static uint64_t clog_us_until(const struct timespec *deadline) {
  struct timespec left = clog_ts_until(deadline);
  return (uint64_t)left.tv_sec * UINT64_C(1000000) +
         ((uint64_t)left.tv_nsec + 999u) / 1000u;
}

// Converts a monotonic deadline to the CLOCK_REALTIME time that POSIX timed
// locks expect. The drain calls it right before each lock, so the time left
// stays right.
static struct timespec clog_realtime_at(const struct timespec *deadline) {
  struct timespec left = clog_ts_until(deadline);
  struct timespec at;
  clock_gettime(CLOCK_REALTIME, &at);
  at.tv_sec += left.tv_sec;
  at.tv_nsec += left.tv_nsec;
  if (at.tv_nsec >= 1000000000L) {
    at.tv_sec++;
    at.tv_nsec -= 1000000000L;
  }
  return at;
}

// Locks sh->mutex within the deadline, so a drain, and the paths that run after
// one, never wait for ever on a stuck target. A free lock is taken even after
// the deadline. Returns whether it locked.
static bool clog_exit_lock_target(clog_shared *sh,
                                  const struct timespec *deadline) {
  struct timespec at = clog_realtime_at(deadline);
  return ccol__mutex_timedlock(sh->mutex, at) == 0;
}

// How long a CLOG_FATAL call keeps waiting for its target's mutex past the
// deadline. The writer steps aside while the call waits, so a holder making
// progress hands the mutex over well within this time.
#define CLOG_FATAL_LOCK_GRACE_MS 100U

// Takes sh->mutex for a CLOG_FATAL record within the deadline, or at least
// CLOG_FATAL_LOCK_GRACE_MS from now. A drain can use up the whole budget, and a
// single try at the deadline would lose the record to a busy writer thread.
// Returns whether it locked.
static bool clog_fatal_lock_target(clog_shared *sh,
                                   const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec grace = clog_ts_after_ms(&now, CLOG_FATAL_LOCK_GRACE_MS);
  const struct timespec *until =
      (deadline->tv_sec > grace.tv_sec ||
       (deadline->tv_sec == grace.tv_sec && deadline->tv_nsec > grace.tv_nsec))
          ? deadline
          : &grace;
  atomic_fetch_add(&sh->fatal_waiters, 1u);
  bool locked = clog_exit_lock_target(sh, until);
  atomic_fetch_sub(&sh->fatal_waiters, 1u);
  return locked;
}

// The exit drain's flush request, on the heap because a request the budget
// gives up on stays in the queue and the writer answers it later.
typedef struct {
  clog_async_ctrl ctrl;
  clog_async_msg envelope;
} clog_exit_flush;

// Outcome of clog_owner_flush(): Answered, sent but not answered, not sent
// (queue full or refused), or no memory for the request.
typedef enum {
  CLOG_OWNER_FLUSH_DONE,
  CLOG_OWNER_FLUSH_PENDING,
  CLOG_OWNER_FLUSH_NOT_SENT,
  CLOG_OWNER_FLUSH_NO_MEMORY
} clog_owner_flush_result;

// Puts a flush request behind everything in sh's queue through the owner send
// (which works with sends off) and waits until the deadline for the writer to
// answer; used by the drains. The request comes from the target's allocator and
// is left to the writer thread if it answers late. Returns the outcome.
static clog_owner_flush_result clog_owner_flush(
    clog_shared *sh, const struct timespec *deadline) {
  clog_exit_flush *req = ccol__mem_alloc(sh->m_procs, sizeof(*req));
  if (!req) return CLOG_OWNER_FLUSH_NO_MEMORY;
  ccol__cond_var_attr ca;
  bool ready = false;
  if (ccol__mutex_init(req->ctrl.mutex) == 0) {
    if (ccol__cond_var_attr_init(ca) == 0) {
      ready = ccol__cond_var_attr_setclock(ca, CLOCK_MONOTONIC) == 0 &&
              ccol__cond_var_init_ca(req->ctrl.cv, ca) == 0;
      ccol__cond_var_attr_destroy(ca);
    }
    if (!ready) ccol__mutex_destroy(req->ctrl.mutex);
  }
  if (!ready) {
    ccol__mem_free(sh->m_procs, req);
    return CLOG_OWNER_FLUSH_NO_MEMORY;
  }
  req->ctrl.done = false;
  req->envelope.kind = CLOG_ASYNC_MSG_FLUSH;
  req->envelope.u.ctrl = &req->ctrl;
  cmessage m = {.data = &req->envelope, .size = sizeof(req->envelope)};
  ccol_retval rv;
  if (sh->is_bounded_queue) {
    uint64_t left_us = clog_us_until(deadline);
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&clog_test_exit_drain_send_no_wait)) left_us = 0;
#endif
    rv = ccircq_owner_send(sh->q.circq, &m, left_us);
  } else {
    rv = cdynmq_owner_send(sh->q.dynmq, &m);
  }
  bool done = false;
  if (rv == ccol_success) {
    ccol__mutex_lock(req->ctrl.mutex);
    while (!req->ctrl.done &&
           ccol__cond_var_timedwait(req->ctrl.cv, req->ctrl.mutex, *deadline) ==
               0) {
    }
    done = req->ctrl.done;
    ccol__mutex_unlock(req->ctrl.mutex);
    if (!done) return CLOG_OWNER_FLUSH_PENDING;
  }
  ccol__mutex_destroy(req->ctrl.mutex);
  ccol__cond_var_destroy(req->ctrl.cv);
  ccol__mem_free(sh->m_procs, req);
  return done ? CLOG_OWNER_FLUSH_DONE : CLOG_OWNER_FLUSH_NOT_SENT;
}

// Drains async target sh within the deadline, for the exit drain and the fatal
// path: It turns sends off first, then puts the flush request in through the
// owner send, so every accepted record is ahead of it and later records take
// the synchronous fallback. Then it settles the batch and pays an owed repair
// under the mutex. When the writer doesn't answer, the queue stays full or the
// mutex doesn't come, it sets drain_left (an unbounded queue gets its sends
// back, a bounded one stays closed) and returns false. The caller keeps sh
// alive (table lock or a pin).
static bool clog_exit_drain_async(clog_shared *sh,
                                  const struct timespec *deadline) {
  atomic_store_explicit(
      &sh->drain_deadline_ns,
      (uint64_t)deadline->tv_sec * 1000000000ull + (uint64_t)deadline->tv_nsec,
      memory_order_relaxed);
  if (sh->is_bounded_queue)
    (void)ccircq_disable_sending(sh->q.circq);
  else
    (void)cdynmq_disable_sending(sh->q.dynmq);
  clog_owner_flush_result r = clog_owner_flush(sh, deadline);
  if (r == CLOG_OWNER_FLUSH_NO_MEMORY) return true;
  if (r != CLOG_OWNER_FLUSH_DONE || !clog_exit_lock_target(sh, deadline)) {
    atomic_store_explicit(&sh->drain_left, true, memory_order_release);
    if (!sh->is_bounded_queue) (void)cdynmq_enable_sending(sh->q.dynmq);
    return false;
  }
  int left = clog_ms_until(deadline);
  int wait_ms = left < CLOG_SPLIT_RECORD_DRAIN_WAIT_MS
                    ? left
                    : CLOG_SPLIT_RECORD_DRAIN_WAIT_MS;
  clog_settle_batch_locked(sh, wait_ms);
  if (sh->async_partial_record & CLOG_OWED_REPAIR)
    (void)clog_pay_owed(sh, sh->format, wait_ms);
  ccol__mutex_unlock(sh->mutex);
  return true;
}

// Gives the deadline of a thread whose send a drain refused: The drain's own
// deadline, and at least CLOG_SPLIT_RECORD_DRAIN_WAIT_MS from now, so a record
// logged after a drain (from a destructor) still gets written.
static struct timespec clog_after_drain_deadline(clog_shared *sh) {
  uint64_t ns =
      atomic_load_explicit(&sh->drain_deadline_ns, memory_order_relaxed);
  struct timespec deadline = {(time_t)(ns / 1000000000ull),
                              (long)(ns % 1000000000ull)};
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec floor_at =
      clog_ts_after_ms(&now, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
  if (floor_at.tv_sec > deadline.tv_sec ||
      (floor_at.tv_sec == deadline.tv_sec &&
       floor_at.tv_nsec > deadline.tv_nsec))
    deadline = floor_at;
  return deadline;
}

// Waits, bounded by clog_after_drain_deadline(), for the records sh's queue
// accepted before a drain turned sends off, so a direct write after a refused
// send doesn't land ahead of them.
static void clog_flush_after_drain(clog_shared *sh) {
  struct timespec deadline = clog_after_drain_deadline(sh);
  (void)clog_owner_flush(sh, &deadline);
}

// Waits within the deadline for sh's compressor to finish every queued and
// running job; at the deadline it drops the queued jobs and aborts the running
// one. A mutex held past the deadline leaves them all to the thread. Needs the
// table's write lock.
static void clog_exit_drain_compressions(clog_shared *sh,
                                         const struct timespec *deadline) {
  if (!clog_exit_lock_target(sh, deadline)) return;
  if (sh->compressor_live) {
    while ((sh->compress_head || sh->compress_running) &&
           ccol__cond_var_timedwait(sh->compress_cv, sh->mutex, *deadline) ==
               0) {
    }
    if (sh->compress_head || sh->compress_running) {
      clog_compress_job *j = sh->compress_head;
      while (j) {
        clog_compress_job *next = j->next;
        ccol__mem_free(sh->m_procs, j);
        j = next;
      }
      sh->compress_head = NULL;
      sh->compress_tail = NULL;
      atomic_store(&sh->compress_abort, true);
      struct timespec grace =
          clog_ts_after_ms(deadline, CLOG_EXIT_ABANDON_GRACE_MS);
      while (sh->compress_running &&
             ccol__cond_var_timedwait(sh->compress_cv, sh->mutex, grace) == 0) {
      }
    }
  }
  ccol__mutex_unlock(sh->mutex);
}

// The exit handler: Drains every async target and then its compressions within
// one budget (the fatal deadline after a CLOG_FATAL call), and pays owed
// repairs with a try-lock only. It walks live_shareds under the table's write
// lock (taken for writing because a fork child re-initialized it), and skips
// the target whose own thread called exit() or a drain already left. Sends stay
// off afterwards, so later records are written synchronously. Does nothing
// unless a writer or compressor thread started or a debt was left.
static void clog_exit_drain(void) {
  if (!atomic_load_explicit(&clog_exit_drain_armed, memory_order_relaxed))
    return;
  ccol__call_once(clog_slot_table.once, clog_slot_table_init_globals);
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  struct timespec deadline =
      atomic_load_explicit(&clog_fatal_bound_armed, memory_order_acquire)
          ? clog_fatal_deadline
          : clog_ts_after_ms(&start, clog_exit_drain_budget_ms());
  struct timespec table_at = clog_realtime_at(&deadline);
  if (ccol__rw_lock_timedwrlock(clog_slot_table.rwlock, table_at) != 0) return;
  if (clog_slot_table.live_shareds) {
    size_t n = cvector_size(clog_slot_table.live_shareds);
    for (size_t i = 0; i < n; i++) {
      clog_shared *sh =
          *(clog_shared **)cvector_at(clog_slot_table.live_shareds, i);
      if (sh == clog_thread_serves) continue;
      if (sh->async_enabled) {
        if (!clog_exit_drain_async(sh, &deadline)) {
#ifdef RUNNING_UNIT_TESTS
          clog_test_note_left_target(sh);
#endif
          continue;
        }
      }
      if (sh->rotation_enabled && sh->rotation.compress_rotated)
        clog_exit_drain_compressions(sh, &deadline);
      if (atomic_load_explicit(&sh->owes_repair, memory_order_relaxed) &&
          ccol__mutex_trylock(sh->mutex) == 0) {
        int left = clog_ms_until(&deadline);
        (void)clog_pay_owed(sh, sh->format,
                            left < CLOG_SPLIT_RECORD_DRAIN_WAIT_MS
                                ? left
                                : CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
        ccol__mutex_unlock(sh->mutex);
      }
    }
  }
  ccol__rw_lock_unlock(clog_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS

// Runs the exit drain now, so a test can check it without ending the process.
void clog__test_run_exit_drain(void) { clog_exit_drain(); }
#endif

// Registers the exit drain with atexit() at load, so it runs after every exit
// handler the application registers later and drains what those log.
__attribute__((constructor)) static void clog_register_exit_drain(void) {
  clog_exit_drain_registered = atexit(clog_exit_drain) == 0;
}

// Arms the deadline of the stop once, at the first CLOG_FATAL call.
static ccol__once_flag clog_fatal_once = CCOL__ONCE_INIT;

// Fixes the deadline of the whole stop (now plus the drain budget) and arms the
// bounded writes, once, when the first CLOG_FATAL call starts.
static void clog_fatal_arm(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  clog_fatal_deadline = clog_ts_after_ms(&now, clog_exit_drain_budget_ms());
  atomic_store_explicit(&clog_fatal_bound_armed, true, memory_order_release);
}

// Says on stderr, with a bounded write, that a CLOG_FATAL record couldn't be
// written, unless stderr is the same output. It reads fd without the lock,
// since a stuck thread may hold it.
static void clog_fatal_report_loss(clog_shared *sh) {
  static const char msg[] =
      "clogger: a CLOG_FATAL record could not be written to its log output "
      "in time; the process stops without it\n";
  int fd = __atomic_load_n(&sh->fd, __ATOMIC_RELAXED);
  struct stat out, err;
  if (fstat(STDERR_FILENO, &err) != 0) return;
  if (fd >= 0 && fstat(fd, &out) == 0 && out.st_dev == err.st_dev &&
      out.st_ino == err.st_ino)
    return;
  struct pollfd pfd = {.fd = STDERR_FILENO,
                       .events = clog_pollout_events(STDERR_FILENO),
                       .revents = 0};
  if (poll(&pfd, 1, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS) > 0 &&
      (pfd.revents & POLLOUT)) {
    ssize_t w = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)w;
  }
}

// Writes a CLOG_FATAL record and stops the process; it never returns. One
// deadline, the drain budget from the first fatal call, bounds the whole stop:
// writes to sinks that aren't regular files, every wait, and the mutex wait
// (plus its grace). An async target is drained first so earlier records reach
// the log. A record that can't go out is reported on stderr and the process
// stops anyway. lg is pinned; it's unpinned before exit.
static __attribute__((noinline, cold, noreturn, format(printf, 8, 0))) void
clog_write_fatal(struct clogger *lg, const char *file, int line,
                 const char *func, bool with_backtrace, char **bt_syms,
                 int bt_depth, const char *fmt, va_list ap) {
  ccol__call_once(clog_fatal_once, clog_fatal_arm);
  clog_shared *sh = lg->shared;
  if (sh->async_enabled) (void)clog_exit_drain_async(sh, &clog_fatal_deadline);
  va_list ap2;
  va_copy(ap2, ap);
  bool delivered = clog_write_sync_body(lg, CLOG_FATAL, file, line, func,
                                        with_backtrace, bt_syms, bt_depth, fmt,
                                        ap, ap2, true, &clog_fatal_deadline);
  va_end(ap2);
  if (!delivered) clog_fatal_report_loss(sh);
  if (bt_syms) free(bt_syms);
  clog_resolve_unpin(lg);
  clog_fatal_exit();
}

// The function behind every clog_<level>() macro: Checks the level with a
// relaxed load and returns at once for a dropped record, captures the backtrace
// in this frame when asked, and sends the record down the fatal, async or
// synchronous path. async_enabled is read without a lock, since it's written
// once before any reader exists. A message queued just before a concurrent
// level change is still delivered, as clog_set_level(3) says.
void clog__write(clog h, clog_level level, const char *file, int line,
                 const char *func, bool with_backtrace, const char *fmt, ...) {
  struct clogger *lg = clog_resolve(h);
  if (!lg)
    ccol__fatal_err(
        "clog__write: clog handle is invalid, stale, or already closed");

  if (level < atomic_load_explicit(&lg->min_level, memory_order_relaxed) &&
      level != CLOG_FATAL) {
    clog_resolve_unpin(lg);
    return;
  }

  char **bt_syms = NULL;
  int bt_depth = 0;
  if (with_backtrace) capture_backtrace(&bt_syms, &bt_depth);

  va_list ap;
  va_start(ap, fmt);

  if (__builtin_expect(level == CLOG_FATAL, 0))
    clog_write_fatal(lg, file, line, func, with_backtrace, bt_syms, bt_depth,
                     fmt, ap);

  if (lg->shared->async_enabled) {
    clog_write_async(lg, level, file, line, func, with_backtrace, bt_syms,
                     bt_depth, fmt, ap);
    va_end(ap);
    clog_resolve_unpin(lg);
    return;
  }

  clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                  bt_depth, fmt, ap);
  va_end(ap);
  if (bt_syms) free(bt_syms);
  clog_resolve_unpin(lg);
}
