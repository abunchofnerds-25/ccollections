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

#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__) || \
    defined(__OpenBSD__)
#include <execinfo.h>
#define CLOG_HAS_BACKTRACE 1
#else
#define CLOG_HAS_BACKTRACE 0
#endif

/* ========================================================================== */
/*                         CONSTANTS                                          */
/* ========================================================================== */

#define CLOG_BUF_INITIAL 4096U
#define CLOG_BUF_MAX (16U * 1024U * 1024U) /* 16 MiB hard cap */
#define CLOG_BACKTRACE_DEPTH 64
/* Rotation suffix format: ".YYYYMMDDHHMMSS" = 15 chars */
#define CLOG_ROTATION_FMT ".%Y%m%d%H%M%S"
#define CLOG_ROTATION_FMT_LEN 15
/* A rotated name's collision suffix is "_" and a decimal sequence number,
 * zero-padded to 4 digits; a number above 9999 takes as many digits as it
 * needs, up to CLOG_ROTATION_SEQ_MAX_DIGITS. */
#define CLOG_ROTATION_SEQ_MAX_DIGITS 9
#define CLOG_ROTATION_SEQ_MAX 999999999UL
/* Extra room for the collision suffix: "_" plus the widest number. */
#define CLOG_ROTATION_EXTRA (1 + CLOG_ROTATION_SEQ_MAX_DIGITS)
/* How many digits the stamp of a rotated name has, YYYYMMDDHHMMSS. */
#define CLOG_ROTATION_STAMP_DIGITS 14
/* A rotated-looking name whose stamp is more than this many seconds above
 * both the clock and the newest name the target made isn't one of its
 * generations (see _clog_gen_filter_init()). A clock that steps back keeps
 * the target's names above the clock by the size of the step, and the
 * biggest steps a working system takes (a hardware clock kept in local time
 * and read as UTC is off by 26 hours at most) are far inside 30 days. A
 * stamp beyond that came from no clock this target trusted. */
#define CLOG_ROTATION_FUTURE_TOLERANCE_SECS ((int64_t)30 * 24 * 60 * 60)
/* The private name a rotation creates the next live file under before it
 * renames it onto the live name: "<base>" + ".tmp" + 11 lowercase hex
 * digits. It's exactly as long as a rotated name without a collision suffix,
 * so a directory that can hold a rotated name can hold it too. It never
 * parses as a rotated name, because there a digit must follow "<base>.". */
#define CLOG_ROTATION_TMP_TAG ".tmp"
#define CLOG_ROTATION_TMP_HEX_DIGITS 11
#define CLOG_ROTATION_TMP_LEN \
  (sizeof(CLOG_ROTATION_TMP_TAG) - 1 + CLOG_ROTATION_TMP_HEX_DIGITS)
_Static_assert(CLOG_ROTATION_TMP_LEN == CLOG_ROTATION_FMT_LEN,
               "the private name of a new live file must be no longer than a "
               "rotated name");
/* A directory entry that looks like one of the target's rotated files but
 * isn't one of its generations gets one WARN record, and at most this many
 * such records go out per target in any window of this many seconds. */
#define CLOG_IGNORED_NOTE_BURST 8U
#define CLOG_IGNORED_NOTE_WINDOW_SECS ((time_t)60)
/* A rotation attempt can fail, because of a permission error for example,
 * or because the file path is too long for the internal buffer of the
 * rotated name. After such a failure later attempts back off by this many
 * seconds instead of retrying on every single write. Without the backoff a
 * failure that persists costs every later write the full syscall overhead
 * of rename() and open() for as long as the condition lasts, and size-based
 * rotation keeps failing against its own trigger with no bound on how often
 * it tries. */
#define CLOG_ROTATE_RETRY_BACKOFF_SECS ((time_t)1)

/* The wall clock rotation reads: the stamp of a rotated name, the start of
 * the time-rotation interval and the retry backoff. A unit-test build adds
 * the offset _clog_test_set_wall_clock_offset() sets, so a test can step the
 * clock forward and back without touching the system clock. Every other
 * build reads time(NULL) directly. */
#ifdef RUNNING_UNIT_TESTS
static _Atomic int64_t _clog_test_wall_offset = 0;

void _clog_test_set_wall_clock_offset(int64_t secs) {
  atomic_store(&_clog_test_wall_offset, secs);
}

static time_t _clog_wall_now(void) {
  return (time_t)((int64_t)time(NULL) + atomic_load(&_clog_test_wall_offset));
}

/* How many of the next _rotate() calls fail at once, before they touch the
 * directory; see _clog_test_force_rotate_failures(). */
static _Atomic unsigned int _clog_test_rotate_failures = 0;

void _clog_test_force_rotate_failures(unsigned int count) {
  atomic_store(&_clog_test_rotate_failures, count);
}

static bool _clog_test_consume_rotate_failure(void) {
  unsigned int n = atomic_load(&_clog_test_rotate_failures);
  while (n > 0) {
    if (atomic_compare_exchange_weak(&_clog_test_rotate_failures, &n, n - 1))
      return true;
  }
  return false;
}
#else
static inline time_t _clog_wall_now(void) { return time(NULL); }
static inline bool _clog_test_consume_rotate_failure(void) { return false; }
#endif

static const char *const _LEVEL_STR[] = {"TRACE", "DEBUG", "INFO",  "WARN",
                                         "ERROR", "ALERT", "FATAL", "OFF"};
/* A clog_level_t value indexes the array above and the array below
 * directly, so the size of each one is tied to the number of values in the
 * enum. That way inserting a level and forgetting to grow one of them fails
 * to compile instead of silently using a wrong index at run time. */
_Static_assert(sizeof(_LEVEL_STR) / sizeof(_LEVEL_STR[0]) == CLOG_OFF + 1,
               "_LEVEL_STR must have exactly one entry per clog_level_t value");

/*
 * The keys clog_set_field() rejects: the fixed output keys that _clog_write()
 * itself always writes, in every format. A user field must not reuse one,
 * since it would make a duplicate "key":.. pair in JSON, a duplicate key=
 * token in logfmt, and in syslog output a duplicate SD-PARAM-NAME inside one
 * SD-ELEMENT, while RFC 5424 wants each SD-PARAM-NAME unique per SD-ELEMENT.
 */
static const char *const _RESERVED_FIELD_KEYS[] = {
    "ts", "level", "proc", "src", "func", "msg", "bt", "bt_error"};
#define _RESERVED_FIELD_KEYS_COUNT \
  (sizeof(_RESERVED_FIELD_KEYS) / sizeof(_RESERVED_FIELD_KEYS[0]))

static bool _is_reserved_field_key(const char *key) {
  for (size_t i = 0; i < _RESERVED_FIELD_KEYS_COUNT; i++)
    if (strcmp(key, _RESERVED_FIELD_KEYS[i]) == 0) return true;
  return false;
}

/* RFC 5424 severity codes. A clog_level_t value indexes this array. */
static const int _SYSLOG_SEVERITY[] = {
    7, /* TRACE -> debug         */
    7, /* DEBUG -> debug         */
    6, /* INFO  -> informational */
    4, /* WARN  -> warning       */
    3, /* ERROR -> error         */
    1, /* ALERT -> alert         */
    0, /* FATAL -> emergency     */
    7, /* OFF; a sentinel. Never use CLOG_OFF as a message level */
};
_Static_assert(
    sizeof(_SYSLOG_SEVERITY) / sizeof(_SYSLOG_SEVERITY[0]) == CLOG_OFF + 1,
    "_SYSLOG_SEVERITY must have exactly one entry per clog_level_t value");

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/*
 * One gzip compression of a rotated file. _rotate() allocates it and hands
 * it to the shared target's compressor thread (see "BACKGROUND COMPRESSION"
 * below), and from then on the job owns its names and the protection that
 * retention gives its generation. Both names are relative to
 * clog_shared_t.dir_fd: `src` is the rotated file the compression reads, and
 * `dst` is `src` plus ".gz", which it writes.
 *
 * A job waits in the shared target's queue until the compressor thread
 * takes it, and from then until the compression ends it is
 * clog_shared_t.compress_running. _prune_rotated() never deletes a file of
 * the running job, because the compression may not have read all of the
 * source or written all of the destination yet; a deletion pass can see the
 * destination on disk from the moment the compression creates it, long
 * before the write finishes. A job still waiting in the queue has no such
 * protection, since nothing reads or writes its files yet, so a pass that
 * finds its generation older than the newest max_rotated_files deletes it
 * like any other, and that job's compression then finds no source and does
 * nothing. The queue and compress_running are read and changed only while
 * clog_shared_t.mutex is held.
 */
typedef struct clog_compress_job {
  struct clog_compress_job *next;
  char *src;
  char *dst;
  /* The live file's owner when the job was queued. The compression reads
   * src only when src is a regular file owned by this user or by the
   * process's effective user (see clog_gen_filter_t). */
  uid_t live_uid;
  /* Device and inode of the temporary output of a compression that failed
   * and whose temporary output couldn't be removed; see
   * _clog_compressor_main(). */
  dev_t tmp_dev;
  ino_t tmp_ino;
} clog_compress_job_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  /* Hard growth ceiling of THIS buffer, in bytes. The default is
   * CLOG_BUF_MAX, which _buf_init() sets, and it's raised above that only for
   * clog_shared_t.async_buf, and only as far as the caller's
   * clog_async_cfg_t.flush_buffer_size needs (see _shared_async_init()).
   * Every other clog_buf_t in this file, each logger's lg->buf and every
   * scratch buffer included, keeps the CLOG_BUF_MAX default. The limit is
   * there to bound one record that is too large, which is a different
   * concern from how big a caller wants the async batch to get before a
   * flush. */
  size_t cap_limit;
  const ccol_memmgmt_procs_t *m_procs; /* borrowed from clog_shared_t */
  /* _buf_ensure() sets this when a growth attempt fails because the
   * allocator call itself failed, a real out-of-memory state. That's
   * different from a growth attempt that only hits this buffer's cap_limit,
   * which happens for a record that is legitimately too large or when a
   * shared batch buffer has too little room left. Both report the same
   * failure to _buf_ensure()'s caller, so this flag is the only thing that
   * tells them apart. _clog_build_record() resets it to false at the start
   * of every attempt to build a record and reads it afterwards, whatever
   * nested append set it, so it can report the right cause to
   * _clog_build_fallback_record(). Without the flag the size always gets the
   * blame. */
  bool oom;
} clog_buf_t;

/*
 * The shared backing store, holding the fd, the rotation state and the
 * mutex. It's reference-counted, so the root logger and every derived logger
 * share one instance, and the fd is closed when ref_count reaches zero if
 * the store owns it.
 */
/* Values of clog_shared_t.async_partial_record. A record can span more than
 * one line (a logfmt record carries its backtrace as tab-indented
 * continuation lines after its primary line), so a short write can split a
 * record in the middle of a line, or exactly at the newline that ends one of
 * its lines while more of its lines are still undelivered. The two need
 * different repairs, because only the first one leaves a line open on
 * disk. */
enum {
  CLOG_SPLIT_NONE = 0,     /* the bytes on disk end on a record boundary */
  CLOG_SPLIT_MID_LINE = 1, /* they end inside a line of a record */
  CLOG_SPLIT_AT_LINE = 2,  /* they end at a newline inside a record */
  CLOG_SPLIT_MASK = 3,     /* the bits that hold one of the three above */
  /* A bit next to the split value: the target owes its output a repair that
   * a failed write couldn't make, which is a newline closing a line left
   * open on disk, a marker naming records the output lost, or both. See
   * clog_shared_t.owe_newline and _clog_pay_owed(). */
  CLOG_OWED_REPAIR = 4,
};

typedef struct clog_shared {
  int fd;
  bool owns_fd;
  /* Not CLOG_SPLIT_NONE while the leading bytes of async_buf continue a
   * record whose first half is already on disk. A write the kernel takes
   * only in part leaves this state behind (see _writer_flush_now()), but only
   * when the accepted prefix stops inside a record; a prefix that stops
   * exactly on a record boundary splits nothing, however much of the batch it
   * leaves behind. A newline followed by a tab-indented continuation line is
   * inside a record, not on its boundary.
   *
   * Both rotation checks refuse to rotate while this is set, so the two
   * halves always land in the same file. Without that rule a rotation could
   * happen between the short write and the retry: it renames away the file
   * holding the first half, the continuation opens the fresh file, and one
   * record spans two files with nothing marking the split. Every line-framed
   * format this module writes (logfmt, JSON lines and RFC 5424) then parses
   * both ends wrongly. Every path that would write a DIFFERENT record
   * straight to the fd settles this flag first (see _clog_flush_and_settle()),
   * so nothing can land between the two halves of a record. The first flush
   * whose write the kernel takes in full clears it, and so does the repair
   * that stands in for such a flush.
   *
   * It sits here, in padding the fields around it already leave, rather than
   * next to the async block it belongs to, because every write path tests it
   * right after it tests fd and so the two share a cache line. Only an async
   * logger sets the split bits (CLOG_SPLIT_MASK).
   *
   * The same byte carries CLOG_OWED_REPAIR, for a synchronous logger as well
   * as an async one. It's set while the target owes its output a newline or
   * a loss marker that a write couldn't deliver. A record never goes out
   * while that newline is owed, so it never joins another record's fragment.
   * So every path that tests the byte for a split masks it with
   * CLOG_SPLIT_MASK, while the paths that only ask "is anything outstanding"
   * keep the plain test, and a target with nothing outstanding pays one test
   * of one byte for both questions. */
  unsigned char async_partial_record;
  /* What fd is, as a clog_sink_kind_t: CLOG_SINK_PLAIN for a regular file and
   * any other descriptor that can't raise SIGPIPE, CLOG_SINK_PIPE for a pipe
   * or a FIFO, and CLOG_SINK_SOCKET for a socket. fstat() decides it when the
   * logger opens, and again when a rotation opens a new file. Every write
   * reads it (see _write_all()), so it sits in padding next to fd on the same
   * cache line, and no other field moves. */
  unsigned char sink_kind;
  /* True when fd is a message-oriented socket (SOCK_DGRAM, SOCK_SEQPACKET or
   * SOCK_RDM). Such a socket keeps the boundary of every write: one write is
   * one message, and a message over the socket's limit fails as a whole. So
   * the async writer thread writes each record of such a sink with its own
   * write and never puts two records in one batch. It's decided together with
   * sink_kind and sits in the padding next to it. */
  bool sink_per_record;
  char *file_path; /* NULL for fd-based loggers */

  bool rotation_enabled;
  clog_rotation_cfg_t rotation;
  off_t bytes_written;
  time_t last_rotation;
  /* The earliest time a rotation may be tried again after a _rotate() call
   * that failed. 0, which calloc gives by default, means no backoff is in
   * effect; see CLOG_ROTATE_RETRY_BACKOFF_SECS. */
  time_t rotate_retry_after;
  /* The compression the compressor thread is running right now, or NULL
   * (see clog_compress_job_t above). _prune_rotated() never deletes a file of
   * this job. */
  clog_compress_job_t *compress_running;

  _ccol_mutex_t mutex;
  int ref_count;
  clog_format_t format;
  clog_syslog_facility_t syslog_facility; /* PRI facility for CLOG_FMT_SYSLOG */
  char syslog_hostname[256]; /* hostname cached at creation       */
  char syslog_appname[49];   /* APP-NAME cached at creation        */
  ccol_memmgmt_procs_t
      *m_procs; /* heap-allocated copy; NULL = default allocator */

  /* Async logging (see "ASYNC LOGGING" below). For a synchronous logger
   * (async_enabled == false) every field in this block is zero and unused,
   * so a synchronous logger pays nothing for async support existing. The
   * library writes async_enabled exactly once: the thread that constructs
   * this shared object writes it before any other thread can see the object,
   * or, in a forked child, the downgrade in _clog_atfork_child writes it
   * instead (see the comment on that function). Since nothing ever changes
   * the field while somebody reads it, every other reader, the dispatch
   * check inside _clog_write() included, can read it without a lock. */
  bool async_enabled;
  bool is_bounded_queue; /* meaningful only if async_enabled */
  union {
    ccol_circular_queue *circq;
    ccol_dynamic_queue *dynmq;
  } q;
  /* Deadline, in CLOCK_MONOTONIC nanoseconds, of the exit drain or CLOG_FATAL
   * drain that turned q's sends off. The drain stores it before turning them
   * off, under the queue mutex, so a thread whose send the queue refused
   * reads that drain's value. */
  _Atomic uint64_t drain_deadline_ns;
  /* Set when an exit drain or a CLOG_FATAL drain left this target because
   * its writer thread didn't answer in time. Nothing drains such a target's
   * queue any more, so from then on a record that a full bounded queue can't
   * take at once is dropped instead of waiting, and a flush returns at once
   * instead of waiting for the writer thread. */
  _Atomic bool drain_left;
  /* Set when the target starts owing its output a repair and cleared when
   * the repair is paid, so the exit drain can see a debt without the mutex
   * and take that mutex only for a target that owes something (see
   * _clog_exit_drain()). If it stays set after the repair, all it costs is
   * that bounded lock. */
  _Atomic bool owes_repair;
  clog_async_cfg_t async_cfg; /* resolved copy, defaults already applied */
  _ccol_thread_id_t writer_thread;
  clog_buf_t async_buf; /* The aggregation buffer the writer thread owns.
      It's separate from a single handle's lg->buf because the jobs of every
      handle sharing this target land in ONE buffer.
      clog_shared_t.async_partial_record, up next to fd, says whether this
      buffer's leading bytes continue a record that is already half
      written. */
  /* CLOCK_MONOTONIC, on purpose, not gettimeofday() or CLOCK_REALTIME. It's
   * only internal bookkeeping of the time since the last flush, which the
   * writer thread's main loop uses to work out how long to wait before its
   * next chance to flush (see _clog_writer_thread_main() below); it's never
   * given to a caller and never put in a record. A wall-clock reading is
   * wrong here because the system clock can step back between two flushes
   * (an NTP step, a manual date change, or a clock correction after a live
   * migration of a virtual machine). The elapsed time then goes negative by
   * any amount, the writer thread's next wait grows far past
   * flush_interval_us, and the guarantee clog_async_cfg_t.flush_interval_us
   * documents (a flush at least that often) silently breaks.
   * CLOCK_MONOTONIC isn't affected by this kind of adjustment. */
  struct timespec last_flush_monotonic;

  /* The rotation state below is read only while a rotation runs, and it sits
   * at the end so it moves no field that each write reads. */
  /* The name of the newest rotated file this shared target made, or found on
   * disk before its first rotation: the 14 stamp digits, with no NUL, and the
   * collision sequence number, 0 for a name without a suffix.
   * rot_name_known says whether the two hold a name. _rotate() gives every
   * new rotated name a (stamp, sequence) pair above this one. Retention
   * orders rotated files by that pair, so a later rotation always sorts as
   * newer. See _rotate(). */
  char rot_last_stamp[CLOG_ROTATION_STAMP_DIGITS];
  bool rot_name_known;
  unsigned long rot_last_seq;
  /* Rate limit of the WARN records about directory entries that look like
   * this target's rotated files but aren't its generations: the start of the
   * current window and the records written in it. See
   * _clog_note_ignored_entry(). */
  time_t ignored_note_window;
  unsigned ignored_note_count;
  /* The last component of file_path. Every rename, open, listing,
   * compression and deletion of a rotation works relative to dir_fd below
   * and to this name. */
  const char *file_base;
  /* Background compression state (see "BACKGROUND COMPRESSION"). The queue
   * holds the jobs the compressor thread hasn't taken yet, oldest first.
   * compress_cv wakes that thread for a new job or a stop, and wakes every
   * thread waiting for the queue to drain. The thread exists, and
   * compress_cv is initialized, exactly while compressor_live is true. mutex
   * guards all of it. */
  clog_compress_job_t *compress_head;
  clog_compress_job_t *compress_tail;
  _ccol_cond_var_t compress_cv;
  _ccol_thread_id_t compressor_thread;
  /* The directory holding the log file, opened once by
   * clog_create_from_file_mp() when rotation is on, and -1 otherwise.
   * Through it a relative path keeps naming the directory where the logger
   * opened the file, whatever the process's working directory becomes. */
  int dir_fd;
  bool compressor_live;
  bool compressor_stop;
  /* True when a deletion pass kept an old generation only because its
   * compression was running. The compressor thread then runs the pass again
   * once that compression ends; see _clog_compressor_main(). */
  bool compress_prune_deferred;
  /* The exit drain sets this when its time budget runs out while a
   * compression is still running. The compression then stops at its next
   * block, removes its temporary output and keeps the source; see
   * _gzip_compress_file() and _clog_exit_drain(). */
  _Atomic bool compress_abort;
  /* The directory entry a file-backed target writes: the device and inode of
   * its directory, plus file_base. A second clog_create_from_file_mp() of the
   * same entry finds this target through clog_slot_table.file_shareds; see
   * _clog_file_register_or_attach(). file_id_known is false for an fd-based
   * target and when the directory can't be examined. */
  bool file_id_known;
  dev_t file_dir_dev;
  ino_t file_dir_ino;
  /* How many CLOG_FATAL calls are waiting for mutex. While it isn't zero the
   * writer thread takes the mutex for no further work, so the fatal call
   * gets it as soon as the work in hand ends; see _clog_writer_lock(). */
  atomic_uint fatal_waiters;
  /* The repair this target owes its output, guarded by mutex. A loss marker,
   * and the newline that closes the fragment of a record a short write cut,
   * are written at the moment of the loss. When that write fails too, these
   * fields remember what's owed, CLOG_OWED_REPAIR is set in
   * async_partial_record, and the repair goes out in front of the next write
   * the output accepts (see _clog_pay_owed()). They sit at the end so no
   * field that each write reads moves.
   *
   * owe_newline is true while the bytes on disk end inside a line no record
   * will continue. owed_losses counts the losses no marker has named yet, and
   * owed_records and owed_bytes add up what they lost. owed_err is the errno
   * of the last failed write, and owed_detail holds the marker text of the
   * first loss, which the repair writes as it is while that's the only loss
   * owed. */
  bool owe_newline;
  int owed_err;
  size_t owed_losses;
  size_t owed_records;
  size_t owed_bytes;
  char owed_detail[128];
  /* errno of the last write of the async batch that the output took only in
   * part, while the batch keeps the bytes for a retry. */
  int retain_err;
#ifdef RUNNING_UNIT_TESTS
  /* Timeouts of the timed wait of this target's async writer thread; each
   * one is a wakeup with no job, which flushes the batch buffer. */
  _Atomic size_t test_writer_timeout_wakeups;
  /* Flushes of this target's async writer thread that were started by an
   * interval running out, with or without a timed wait before them. */
  _Atomic size_t test_writer_interval_flushes;
#endif
} clog_shared_t;

struct clogger {
  clog_shared_t *shared; /* shared output backing store              */
  /* This logger's level filter. It's _Atomic so _clog_write() can check it
   * cheaply before locking shared->mutex (see the fast-path check there):
   * another thread can call clog_set_level() on the same handle at the same
   * time, and the _Atomic type keeps that unlocked read from being a data
   * race with such a call. */
  _Atomic clog_level_t min_level;
  /* Guards `fields` only. It's separate from shared->mutex, which serialises
   * the fd writes and the rotation across a whole derive tree, because each
   * logger's field map is otherwise completely independent of every sibling
   * sharing the same `shared`: clog_set_field(), clog_remove_field() or
   * clog_clear_fields() on one derived logger must not contend with an
   * unrelated write in progress on a sibling. _clog_write() and clog_derive()
   * take this lock only for the short window in which they read `fields`.
   * When a function needs both this lock and shared->mutex, as _clog_write()
   * does while it holds shared->mutex for the whole call, shared->mutex is
   * always locked first and never the other way round, so no lock-order
   * cycle is possible anywhere in this file. */
  _ccol_mutex_t fields_mutex;
  chmap fields;   /* chmap(char* -> char*); per-logger fields */
  clog_buf_t buf; /* per-logger reusable write buffer          */
  /* This logger's handle, so an unpin can find the slot holding its pin
   * without the caller carrying one. Written once, before the handle is
   * published, and never again. */
  clog self_handle;
};

/* ========================================================================== */
/*                         HANDLE SLOT TABLE                                  */
/* ========================================================================== */

/*
 * A clog is an opaque value handle made of a slot index and a generation,
 * not a pointer. The table closely follows chttpsvr_slot_table and
 * chttpcli_slot_table (in src/chttpserver.c and src/chttpclient.c), with one
 * difference on purpose: clogger's resolve step is on the hot path of every
 * single log_* call, including the ones the level filter drops, so it runs
 * far more often than the resolve sites of those two modules. A
 * reader-writer lock guards the table for the cold paths that change it (an
 * acquire, a close and the fork walk), while the resolve and pin steps go
 * through the lock-free handle index in cpintable.h and take no lock of
 * this table at all.
 */
typedef struct {
  struct clogger *ptr; /* NULL when free */
  uint32_t generation; /* 0 before the first use, 1 after the first acquire */
  bool in_use;         /* false from step 2 of clog_close() on. It gates
      _clog_resolve only and does NOT mean ptr is safe to dereference under
      the protection of fields_mutex; see `freed` below */
  bool freed;          /* false until _logger_free(ptr) really runs; see
               "Fork safety" below. _clog_atfork_prepare's fields_mutex walk
               gates on THIS field, not on in_use, because in_use goes false
               long before ptr->fields_mutex is destroyed */
} clog_slot_t;

/* The hot half of the table above. It maps a handle to a pointer and holds
 * the pin that keeps a logger alive for the length of a call. It's separate
 * because a resolve runs on every single log call and must not write
 * anything another thread reads. Everything else this table does (fork
 * handling, live_shareds and slot reuse) is cold and stays under the
 * rwlock. */
static ccol_pintable clog_pintable;

static struct {
  _ccol_rw_lock_t rwlock;
  _ccol_once_flag_t once;
  cvec slots;        /* cvector of clog_slot_t */
  cvec free_indices; /* cvector of uint32_t; LIFO */
  cvec live_shareds; /* A cvector of clog_shared_t*. It covers the whole
      life of a clog_shared_t, from the acquisition of the object's first
      handle until the object is freed, whatever the in_use flag of any one
      handle says. "Fork safety" below explains why this registry exists
      instead of deriving the set of live clog_shared_t objects from the
      handle table. */
  /* How many clog_close calls have passed the point where they stop being
     visible as a live slot but aren't done with this table yet. A close
     clears its own slot->ptr partway through and then takes the write lock
     again to remove its shared object from live_shareds, so slot->ptr alone
     doesn't cover the whole window in which the table must survive. In one
     thread that's harmless, since the later step always follows, but across
     threads it isn't: without this counter a second close that finishes in
     between destroys live_shareds, and the first close then indexes it. */
  size_t closes_in_flight;
  /* Set when the destructor at process exit found a logger still open and
     left this table alone; whichever close is the last one out after that
     does the release the destructor couldn't do. Without this flag, a logger
     that a destructor closes (one the linker put before this one) leaves the
     table and the pin index allocated for the rest of the process, and a
     leak checker that counts still-reachable memory as an error reports it.
     This field is read and written only under the write lock. */
  bool release_deferred;
  /* A cvector of clog_shared_t*: every file-backed target whose directory
     entry is known (clog_shared_t.file_id_known). Opening a path that a
     target here already writes attaches to that target, or fails, instead
     of building a second target on the same file; see
     _clog_file_register_or_attach(). A target stays here until its last
     close has stopped its threads and closed its descriptors, so an open
     can't build a second target while the first one is still rotating or
     compressing. It's read and changed only under the write lock. */
  cvec file_shareds;
} clog_slot_table = {0};

/* Defined with the process-exit teardown below, and declared here because
   the final locked section of clog_close does the release that the
   destructor deferred. */
static bool _clog_any_slot_live_locked(void);
static void _clog_release_slot_table_locked(void);
static void _clog_release_slot_table_if_deferred_locked(void);

#if CCOL_FORK_SAFETY_REQUIRED
/*
 * One slot that a fork() catches in the middle of a clog_close(), with
 * in_use == false and freed == false. _clog_atfork_prepare() records these
 * slots and the child side of _clog_atfork_release() finishes them, below;
 * those two functions explain why this needs tracking of its own, which the
 * ordinary locked_fields protection every live slot gets doesn't give.
 *
 * With CCOL_FORK_SAFETY_REQUIRED at 0 (see the doc comment of that macro in
 * common.h) the compiler drops this type, _clog_atfork_state below, and
 * every function and registration site that touches either of them.
 * clog_slot_table.slots, free_indices and live_shareds stay compiled, and so
 * does clog_slot_t.freed, because they also serve this module's ordinary
 * handle lifecycle, which has nothing to do with a fork, and not only fork
 * safety.
 */
typedef struct {
  uint32_t idx;
  struct clogger *raw;
} clog_atfork_closing_t;

/*
 * Records every lock _clog_atfork_prepare takes, so _clog_atfork_parent and
 * _clog_atfork_child can unlock exactly what the prepare step took. Only the
 * forking thread reads or writes this state, and only between the prepare
 * call and the matching parent or child call; nothing touches it
 * concurrently, so it needs no lock of its own. _clog_atfork_prepare walks
 * clog_slot_table.live_shareds to protect shared->mutex; that vector isn't a
 * field here.
 */
static struct {
  cvec locked_fields; /* cvector of struct clogger* */
  cvec closing_slots; /* A cvector of clog_atfork_closing_t. See the doc
     comment of that type */
} _clog_atfork_state = {0};

static void _clog_atfork_prepare(void);
static void _clog_atfork_parent(void);
static void _clog_atfork_child(void);
#endif /* CCOL_FORK_SAFETY_REQUIRED */
static void _clog_slot_table_init_globals(void);

/* Removes sh from v, a cvector of clog_shared_t*, if v holds it. The order of
 * such a vector means nothing, so the last entry takes the removed one's
 * place. The caller holds the slot table's write lock. */
static void _clog_shareds_remove(cvec v, clog_shared_t *sh) {
  size_t n = cvector_elem_count(v);
  for (size_t i = 0; i < n; i++) {
    if (*(clog_shared_t **)cvector_at(v, i) != sh) continue;
    clog_shared_t *last = *(clog_shared_t **)cvector_at(v, n - 1);
    *(clog_shared_t **)cvector_at(v, i) = last;
    cvector_pop_back(v, &last);
    return;
  }
}

/* Forward declarations; the definitions are much further down, in the
 * "CONSTRUCTOR HELPERS" section. The child side of _clog_atfork_release()
 * has to call all three to finish a clog_close() call that a fork() left in
 * the middle of its teardown; see the doc comment of that function. */
static void _logger_free(struct clogger *lg);
static void _shared_close_owned_fd(clog_shared_t *sh);
static void _shared_free_partial(clog_shared_t *sh);

#ifdef RUNNING_UNIT_TESTS
/*
 * Lets a test force the NEXT _clog_handle_acquire() call deterministically,
 * in two ways. First, that call grows clog_slot_table.slots with a brand new
 * slot, exactly as if free_indices were empty, whatever slots earlier tests
 * in the same process left there for reuse. Second, the same call's
 * live_shareds registration step fails without the process's plain default
 * allocator having to really fail; that allocator is the only one
 * clog_slot_table.slots, free_indices and live_shareds ever use, whatever a
 * logger's custom mprocs are (see _clog_slot_table_init_globals()). It takes
 * both effects together to reach one rollback path, which can leave a slot
 * with freed == false and ptr == NULL at the same time (see the comment on
 * that rollback in _clog_handle_acquire()). A slot reused from free_indices
 * already has freed == true from its previous occupant's clog_close(), so
 * only a fresh slot whose OWN registration then fails can get there. The
 * hook disarms itself the moment it fires, so it affects only the one
 * acquisition a test targets.
 */
static _Atomic bool _clog_test_force_next_fresh_slot_reg_failure = false;

static bool _clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return atomic_exchange(&_clog_test_force_next_fresh_slot_reg_failure, false);
}

void _clog_test_force_next_fresh_slot_registration_failure(bool force) {
  atomic_store(&_clog_test_force_next_fresh_slot_reg_failure, force);
}

/* See the doc comment of _clog_test_set_close_finalize_delay_us() in
 * clogger.h. Unlike the hook just above, this one does NOT disarm itself on
 * purpose, because a test needs it armed across the whole clog_close() call
 * under test and not just for its first use. */
static _Atomic unsigned int _clog_test_close_finalize_delay_us = 0;
static _Atomic bool _clog_test_close_finalize_delay_entered_state = false;

void _clog_test_set_close_finalize_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_close_finalize_delay_entered_state, false);
  atomic_store(&_clog_test_close_finalize_delay_us, delay_us);
}

bool _clog_test_close_finalize_delay_entered(void) {
  return atomic_load(&_clog_test_close_finalize_delay_entered_state);
}

/* See the doc comment of _clog_test_set_close_release_window_us() in
 * clogger.h. Like the hook above, it doesn't disarm itself, on purpose. */
static _Atomic unsigned int _clog_test_close_release_window_us = 0;
static _Atomic bool _clog_test_close_release_window_entered_state = false;

void _clog_test_set_close_release_window_us(unsigned int delay_us) {
  atomic_store(&_clog_test_close_release_window_entered_state, false);
  atomic_store(&_clog_test_close_release_window_us, delay_us);
}

bool _clog_test_close_release_window_entered(void) {
  return atomic_load(&_clog_test_close_release_window_entered_state);
}

size_t _clog_test_live_shareds_count(void) {
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  _ccol_rw_lock_rdlock(clog_slot_table.rwlock);
  size_t n = clog_slot_table.live_shareds
                 ? cvector_elem_count(clog_slot_table.live_shareds)
                 : 0;
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  return n;
}
#else
static inline bool _clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return false;
}
#endif

static void _clog_slot_table_init_globals(void) {
  if (_ccol_rw_lock_init(clog_slot_table.rwlock) != 0)
    _ccol_fatal_err("clog slot table: failed to initialize rwlock");
  clog_slot_table.slots = cvector_create(sizeof(clog_slot_t), NULL);
  if (!clog_slot_table.slots)
    _ccol_fatal_err("clog slot table: failed to allocate slots vector");
  clog_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clog_slot_table.free_indices)
    _ccol_fatal_err("clog slot table: failed to allocate free-index vector");
  clog_slot_table.live_shareds = cvector_create(sizeof(clog_shared_t *), NULL);
  if (!clog_slot_table.live_shareds)
    _ccol_fatal_err("clog slot table: failed to allocate live-shareds vector");
  clog_slot_table.file_shareds = cvector_create(sizeof(clog_shared_t *), NULL);
  if (!clog_slot_table.file_shareds)
    _ccol_fatal_err("clog slot table: failed to allocate file-shareds vector");
#if CCOL_FORK_SAFETY_REQUIRED
  _clog_atfork_state.locked_fields =
      cvector_create(sizeof(struct clogger *), NULL);
  if (!_clog_atfork_state.locked_fields)
    _ccol_fatal_err(
        "clog slot table: failed to allocate atfork scratch vector");
  _clog_atfork_state.closing_slots =
      cvector_create(sizeof(clog_atfork_closing_t), NULL);
  if (!_clog_atfork_state.closing_slots)
    _ccol_fatal_err(
        "clog slot table: failed to allocate atfork closing-slots vector");
  _ccol_at_fork(_clog_atfork_prepare, _clog_atfork_parent, _clog_atfork_child);
#endif
}

/* Not in clogger.h (internal/catfork.h declares it) and not part of the
 * public API: a narrow escape hatch, kept on purpose, that follows
 * _cthreadcomm_ensure_atfork_registered_before_caller() in cthreadcomm.c and
 * _ctpool_ensure_atfork_registered_before_caller() in cthreadpool.c exactly.
 * It's there for a dependent module, chttpserver.c, which needs to make sure
 * this module's _ccol_at_fork() triple is registered BEFORE its own.
 * pthread_atfork runs prepare handlers in the reverse order of registration,
 * so the CALLER's prepare handler then runs FIRST at every future fork(),
 * before this module's prepare handler, _clog_atfork_prepare, can lock
 * clog_slot_table.rwlock, the mutex of a live clog_shared_t, or a
 * fields_mutex.
 *
 * chttpserver.c holds its own srv_engine_bundler.mutex across a call into
 * this module in two places: the clog_info() or clog_warn() call inside
 * _SRV_ENGINE_LOG, and the direct clog_create_from_fd_mp call in
 * _engine_acquire for the engine's fallback logger. Without this function
 * the real lock order at fork() time would be an accident of which module
 * the embedding application happens to use FIRST. Take a caller that
 * resolves a chttpsvr handle before this process uses clog for anything
 * else; the handle can even be invalid, since every public chttpsvr_*
 * function that resolves a handle accepts an invalid one and reports a plain
 * error, without the caller having to guarantee validity first. Such a
 * caller registers chttpserver's _ccol_at_fork() triple with no forced
 * clogger registration before it, and a later, unrelated first use of clog
 * then registers this module's triple AFTER chttpserver's. That inverts the
 * order the code needs and gives exactly the AB-BA fork() deadlock that the
 * same forced registration helpers in chttpserver, cthreadcomm and
 * cthreadpool are there to keep closed (see the doc comment of
 * _chttpsvr_slot_table_init_globals in chttpserver.c). A caller that never
 * uses this function isn't affected at all: this module's lazy registration,
 * guarded by _ccol_call_once, happens whenever the library first creates a
 * clog handle for its own use. */
void _clog_ensure_atfork_registered_before_caller(void) {
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
}

/*
 * Resolve h and pin the result against a concurrent clog_close(). Returns
 * NULL for h == 0, for an h out of range, and for an h naming a free slot or
 * a slot with the wrong generation. After a successful resolve the caller
 * must call _clog_resolve_unpin() exactly once on every path. It takes no
 * lock at all and writes nothing another thread reads: it runs on every
 * log_* call, including one that its own level check drops, so every core
 * would pay for any shared write here.
 */
static struct clogger *_clog_resolve(clog h) {
  /* No lock and no write to anything another thread reads. The pin index is
   * a static that starts at zero, so a handle that shows up before any logger
   * was ever opened finds no chunk and resolves to NULL, the same answer an
   * empty slot table gives. */
  return (struct clogger *)ccol_pintable_pin(&clog_pintable, h);
}

/*
 * Lock-free on purpose, and safe: it releases the pin and touches nothing
 * else. clog_close() never sleeps on a condition variable waiting for this
 * function to wake it, it polls the pin count instead (see step 3 of
 * clog_close()), so there's no wakeup to deliver. Do NOT add a lock, a
 * broadcast or any other use of raw here "to be safe". After the pin is
 * released the object can be freed at any instant, so anything after the
 * release is a use-after-free, and anything before it is a shared write on a
 * path whose whole point is to have none.
 *
 * Reading raw->self_handle before the release is safe, because the pin is
 * still held then.
 */
static void _clog_resolve_unpin(struct clogger *raw) {
  ccol_pintable_unpin(&clog_pintable, raw->self_handle);
}

/*
 * Makes a fresh handle for a fully constructed lg. It's called once from the
 * constructor path, after lg and sh are otherwise completely built, and also
 * from clog_derive(). It gives CLOG_INVALID when the slot table runs out of
 * memory, which is an ordinary failure and not a fatal one. It also
 * registers lg->shared in clog_slot_table.live_shareds (see "Fork safety"
 * below) without adding an entry that is already there: a brand new root
 * construction and a sibling handle from clog_derive() both call this same
 * function, and only the FIRST one naming a given `shared` adds it.
 *
 * It closely follows _chttpsvr_handle_slot_acquire (in src/chttpserver.c).
 * Index and generation encode as (idx << 32) | generation, and the
 * generation wraps around and skips 0, which is reserved: it means "never
 * used yet" and tells CLOG_INVALID apart from a real handle. One thing is
 * added to that shape, which is that every acquire sets slot->freed = false
 * unconditionally. A slot reused from free_indices has freed == true from
 * its previous occupant's close, so without this reset a handle born from
 * such a slot would silently inherit freed == true from an unrelated
 * predecessor that is long gone, and _clog_atfork_prepare's fields_mutex
 * walk would then never protect that handle at all.
 */
static clog _clog_handle_acquire(struct clogger *lg) {
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  /* The module destructor releases the table once nothing needs it, and an
   * application destructor or an exit handler can still open a logger after
   * that. Such an open is refused like any other failed open, since the
   * table's vectors are gone. */
  if (__builtin_expect(clog_slot_table.slots == NULL, 0)) {
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_INVALID;
  }

  /* Outside RUNNING_UNIT_TESTS this disappears completely: in a non-test
   * build the helper is just `return false;`, so it costs nothing on the hot
   * handle-acquisition path of a real build. */
  bool force_fresh_slot_reg_failure =
      _clog_test_consume_forced_fresh_slot_reg_failure();

  uint32_t idx;
  clog_slot_t *slot;
  if (!force_fresh_slot_reg_failure &&
      cvector_elem_count(clog_slot_table.free_indices) > 0) {
    cvector_pop_back(clog_slot_table.free_indices, &idx);
    slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
  } else {
    /* The pin table can hold only so many slots, and a slot whose index is
     * past that limit can never be published. So we refuse such a slot here
     * instead of claiming it and rolling back, because the rollback would put
     * an index no later publish can use onto the free list every acquire pops
     * from. It's an ordinary failure, which every caller already treats
     * like a table that can't grow. */
    if (cvector_elem_count(clog_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      _ccol_rw_lock_unlock(clog_slot_table.rwlock);
      return CLOG_INVALID;
    }
    clog_slot_t fresh = {0};
    if (cvector_push_back(clog_slot_table.slots, &fresh) != ccol_success) {
      _ccol_rw_lock_unlock(clog_slot_table.rwlock);
      return CLOG_INVALID;
    }
    idx = (uint32_t)cvector_elem_count(clog_slot_table.slots) - 1;
    slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
  }

  /* Register lg->shared in live_shareds, which is exactly the set
   * _clog_atfork_prepare() walks to decide which shared->mutex objects to
   * lock before a fork(). It happens BEFORE this slot is changed or handed
   * out, so a failed registration can roll back while the index is still
   * untouched. A handle whose shared object failed to register silently
   * escapes _clog_atfork_prepare()'s protection: a fork() can race a write or
   * a rotation in progress on that exact logger, and the child then keeps a
   * shared->mutex locked for ever, which is the class of hang the
   * fork-safety machinery in this file is there to prevent. */
  bool already_registered = false;
  size_t n = cvector_elem_count(clog_slot_table.live_shareds);
  for (size_t i = 0; i < n; i++) {
    clog_shared_t *seen =
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
    if (seen == lg->shared) {
      already_registered = true;
      break;
    }
  }
  /* force_fresh_slot_reg_failure forces this outcome unconditionally and
   * ignores already_registered. For clog_create_from_fd_mp() and
   * clog_create_from_file_mp() lg->shared is always fresh, so
   * already_registered is always false there and it makes no difference. For
   * clog_derive(), lg->shared is the PARENT's already-registered shared
   * object, which is certain to be in live_shareds as long as the parent
   * itself is a live, pinned handle, so already_registered is always true
   * there. Without this unconditional override the forced failure hook could
   * never fire for a clog_derive() call: it would silently use itself up,
   * since it disarms itself on every call (see
   * _clog_test_consume_forced_fresh_slot_reg_failure() above), and the call
   * would make a valid handle anyway. That contradicts what the hook
   * documents, which is to work the same way at all three call sites. */
  bool live_shareds_registration_failed = force_fresh_slot_reg_failure;
  if (!live_shareds_registration_failed && !already_registered) {
    live_shareds_registration_failed =
        cvector_push_back(clog_slot_table.live_shareds, &lg->shared) !=
        ccol_success;
  }
  if (live_shareds_registration_failed) {
    /* A slot from free_indices is safe to leave as it is here, because it
     * already has freed == true and ptr == NULL from its previous occupant's
     * clog_close(). A slot from the branch above that pushes a brand new slot
     * is NOT: its own fresh = {0} initialization leaves freed == false with
     * ptr == NULL, and the walk in _clog_atfork_prepare() reads exactly that
     * combination as "this slot has a live ptr, lock its fields_mutex",
     * because the walk only ever skips a slot with
     * `if (slot->freed) continue;`. Without a correction here, a fork() can
     * land while this free index sits unused in free_indices, and the walk
     * then dereferences a NULL ptr in
     * _ccol_mutex_lock(slot->ptr->fields_mutex) and crashes. So we set both
     * fields back explicitly, giving the slot the same "freed" shape a slot
     * always has once clog_close() retires it, which makes the rollback safe
     * whichever of the two branches above produced `slot`. */
    slot->freed = true;
    slot->ptr = NULL;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_INVALID;
  }

  slot->generation++;
  if (slot->generation == 0) slot->generation++; /* skip the sentinel value */

  clog h = ((clog)idx << 32) | (clog)slot->generation;

  /* Written before the handle is published, so a resolver that finds this
   * logger also finds the handle its own unpin needs. */
  lg->self_handle = h;

  /* The publish step can allocate a chunk or a stripe block the first time,
   * and if that fails the slot stays unpublished and the handle resolves to
   * nothing. Roll the slot back exactly like the live_shareds failure above,
   * rather than hand out a handle no call could ever resolve. */
  if (!ccol_pintable_publish(&clog_pintable, idx, slot->generation, lg)) {
    /* The one failure that can happen after lg->shared has already been
     * registered, so that registration is undone here. Otherwise a
     * pointer to a shared object stays in the set _clog_atfork_prepare walks
     * and locks while the caller gets CLOG_INVALID and goes on to free that
     * object, and the process's next fork() locks a mutex inside freed
     * memory.
     *
     * Popping the tail is exactly right and needs no search: live_shareds is
     * only ever changed under the write lock this function holds for its
     * whole body, so if this call pushed at all (which it did exactly when
     * the entry wasn't already registered), its entry is still the last
     * one. */
    if (!already_registered) {
      clog_shared_t *rolled_back = NULL;
      cvector_pop_back(clog_slot_table.live_shareds, &rolled_back);
    }
    /* Cleared because the slot goes back on the free list: a handle left
     * here would name a slot that belongs to another object, and an unpin
     * carrying such a handle would charge that object's count for a pin
     * nobody took. */
    lg->self_handle = 0;
    slot->freed = true;
    slot->ptr = NULL;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_INVALID;
  }

  slot->ptr = lg;
  slot->in_use = true;
  slot->freed = false;

  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         FORK SAFETY                                       */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/*
 * fork() copies only the calling thread, so if some OTHER thread holds a lock
 * at that exact instant (clog_slot_table.rwlock, a shared->mutex or a
 * fields_mutex), the child inherits it locked for ever, because the thread
 * that would unlock it doesn't exist there. The pthread_atfork idiom below
 * is the standard, correct answer. _prepare locks every such lock, so fork()
 * goes on only at a moment when no thread holds one. _parent unlocks them
 * all after fork() returns in the parent. _child unlocks every mutex too:
 * the forking thread's logical execution continues as the child's only
 * thread, so for a mutex this is a normal, valid unlock and not a trick to
 * reset a lock a dead thread owns. The one exception is
 * clog_slot_table.rwlock, which the child initializes again; the comment
 * above _clog_atfork_release says why.
 *
 * shared->mutex is protected through clog_slot_table.live_shareds rather
 * than a walk over the handles with in_use set. A handle's slot can already
 * show in_use == false while clog_close() is still locking or holding
 * shared->mutex a few steps later (see the numbered sequence in
 * clog_close()), so a walk gated on in_use would miss that shared object's
 * mutex in exactly that window. fields_mutex is protected by the freed flag
 * instead (see the doc comment of clog_slot_t.freed), for the same reason:
 * in_use goes false before _logger_free() destroys fields_mutex, and an
 * operation on a third thread that holds a pin at that moment
 * (clog_set_field() and its siblings are such operations) can legitimately
 * still hold that mutex inside the same window.
 */
static void _clog_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(_ccol_atfork_module_clogger);
#endif
  /* This file has a standing rule for the pthread wrappers: every function
   * that touches clog_slot_table.rwlock directly guards it with this same
   * _ccol_call_once. pthread_atfork() can only call this function after
   * _clog_slot_table_init_globals() has registered it, so that function has
   * already run, but the guard doesn't rely on that call-graph reasoning,
   * which breaks silently the moment a future change adds a call path nobody
   * expected. So the guard stays unconditional. */
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  /* A fork after the module destructor released the table finds nothing to
   * lock, and the release handler then only gives the lock back. glibc
   * withdraws an object's fork handlers while it runs its destructors, so no
   * fork() there gets to this test; it keeps a C library that keeps the
   * handlers around longer from indexing the released vectors. */
  if (clog_slot_table.slots == NULL) return;

  size_t ns = cvector_elem_count(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared_t *sh =
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
    _ccol_mutex_lock(sh->mutex);
  }

  size_t n = cvector_elem_count(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, i);
    if (slot->freed) continue;
    _ccol_mutex_lock(slot->ptr->fields_mutex);
    if (cvector_push_back(_clog_atfork_state.locked_fields, &slot->ptr) !=
        ccol_success) {
      /* Recording this lock failed because memory ran out, so we unlock it
       * again at once instead of leaving it held with nothing in
       * locked_fields; _clog_atfork_release() would then never unlock it,
       * and this one logger's field operations would deadlock for ever, in
       * the parent and in the child. Skipping the fields_mutex protection of
       * this single logger for this one fork() is the safe, bounded
       * degradation. Since fork() only ever copies the calling thread, the
       * window this leaves open is the same narrow race only an out-of-memory
       * state can reach, which we already accept for a `freed` slot (skipped
       * by the loop above) and for a shared object that never made it into
       * live_shareds at all. */
      _ccol_mutex_unlock(slot->ptr->fields_mutex);
    }

    if (!slot->in_use) {
      /* A clog_close() on this exact handle is suspended on some OTHER thread
       * right now, between its step 2, which clears in_use, and its step 4,
       * which retires the slot and frees sh's reference (see clog_close()'s
       * numbered steps). That thread doesn't exist in a child a fork just
       * made, so without this record nothing in the child would ever retire
       * this slot or free this handle's share of sh->ref_count: the child
       * would leak raw for ever, and sh as well once every other handle for
       * sh is closed. The record lets _clog_atfork_release() finish this
       * close for the vanished thread, in the child only; in the parent the
       * real closing thread is still there and unaffected, so it needs no
       * help. Recording can fail when memory runs out, a bounded degradation
       * of the same shape as the locked_fields push failure just above: we
       * accept leaking this one slot in a future child rather than risk
       * anything in the parent to avoid it. */
      clog_atfork_closing_t c = {.idx = (uint32_t)i, .raw = slot->ptr};
      cvector_push_back(_clog_atfork_state.closing_slots, &c);
    }
  }
}

/*
 * In the child we initialize the slot table's rwlock again instead of
 * unlocking it. The child's only thread can call a plain
 * pthread_rwlock_unlock() on the write lock, but that call does NOT release
 * a lock that the same thread's continuation took in the parent: glibc's
 * rwlock write lock tracks its owner by TID internally, and after the fork
 * the child's thread has a different TID from the parent thread that forked.
 * So the unlock fails silently, and every later resolve in the child hangs
 * for ever in _ccol_rw_lock_rdlock on this exact rwlock. A plain mutex of
 * the default type doesn't behave this way because it has no TID tracking
 * (this codebase uses the fast and normal mutex type everywhere; see
 * _ccol_mutex_init() in common.h), so only the rwlock needs this, not
 * sh->mutex and fields_mutex.
 *
 * Initializing the lock afresh in the child instead of unlocking it is the
 * standard, well-established way to handle exactly this case, and glibc's
 * malloc arena locks do the same. It's safe because the child has exactly
 * one thread and nobody else can wait on the lock, so there's nobody for a
 * fresh initialization to race.
 */
static void _clog_atfork_release(bool in_child) {
  /* The same guard as at the top of _clog_atfork_prepare(), and its doc
   * comment applies. This function also touches clog_slot_table.rwlock,
   * further down and for every caller, so it carries the same
   * _ccol_call_once guard for the same reason, without relying on
   * _clog_atfork_prepare() having run the guard moments earlier for this
   * exact fork(). */
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  if (clog_slot_table.slots == NULL) {
    /* The table was released before this fork() (see the same test in
     * _clog_atfork_prepare()), so all that's left is to give the lock back. */
    if (in_child) {
      if (_ccol_rw_lock_reinit_in_child(clog_slot_table.rwlock) != 0)
        _ccol_fatal_err(
            "clog atfork release: failed to reinit slot table rwlock");
    } else {
      _ccol_rw_lock_unlock(clog_slot_table.rwlock);
    }
    return;
  }
  size_t nf = cvector_elem_count(_clog_atfork_state.locked_fields);
  for (size_t i = nf; i-- > 0;) {
    struct clogger *lg =
        *(struct clogger **)cvector_at(_clog_atfork_state.locked_fields, i);
    if (in_child) {
      /* A pin stays held as long as SOME thread holds a resolved pointer to
       * this handle, as any thread inside one of the log_ macros does, and so
       * does a thread inside one of the other public clog_ functions on that
       * handle. clog_close() polls the count until it reaches 0, with no
       * upper bound on the wait. fork() copies only the calling thread, so a
       * pin that any OTHER thread holds at fork() time can never be released:
       * only that thread's later _clog_resolve_unpin() ever decrements the
       * count, and that thread doesn't exist in this child. The child
       * inherits the pin, so without this reset the first clog_close() on
       * that exact handle in the child hangs for ever. This isn't a rare
       * corner case but what normally happens when another thread is in the
       * middle of a log call on a logger that is still open at the fork; the
       * scenario of fork_safety.concurrent_fork_during_churn_does_not_hang is
       * one example. A child a fork just made has exactly one thread, so no
       * resolver really in flight can exist here. There is one exception, a
       * fork() called from inside such a call on this same handle and this
       * same thread, where this reset counts that thread's own pin too low.
       * That case isn't supported, on purpose, and the rest of this file's
       * atfork handling doesn't chase that kind of reentrancy either: see the
       * handling of sh->async_enabled and of the compression state below,
       * which also assumes the forking thread isn't in the middle of an
       * operation on the shared target. */
      ccol_pintable_reset_for(&clog_pintable, lg->self_handle);
    }
    _ccol_mutex_unlock(lg->fields_mutex);
  }

  size_t ns = cvector_elem_count(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared_t *sh =
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
    if (in_child) {
      /* This shared target's compressor thread doesn't exist in the child,
       * since fork() copies only the calling thread. The compression it is
       * running at fork() time, and every job in its queue, belong to the
       * parent, whose compressor thread finishes them there on the same
       * files. So the child forgets all of them: it must not compress them a
       * second time, and it must not treat the generation of a job that no
       * thread of the child runs as protected from pruning for ever. The jobs
       * and compress_cv stay allocated and untouched, for the same reason as
       * the async queue below. compress_cv can have the vanished thread as a
       * waiter, so a later rotation in the child initializes it afresh when
       * it starts a compressor thread of its own. */
      sh->compress_running = NULL;
      sh->compress_head = NULL;
      sh->compress_tail = NULL;
      sh->compressor_live = false;
      sh->compressor_stop = false;
      sh->compress_prune_deferred = false;
    }
    if (in_child && sh->async_enabled) {
      /* This shared target depends on a writer thread, which doesn't exist in
       * the child at all because fork() copies only the calling thread. The
       * internals of ccol_circular_queue and ccol_dynamic_queue are opaque
       * and there's no safe way to reach in and reset whatever lock state
       * the child inherited, so we don't try. Every code path in this file
       * already gates ALL access to sh->q, sh->writer_thread and
       * sh->async_buf behind this one flag, so flipping it here means nothing
       * in the child ever touches that queue, buffer and thread state again;
       * their locks may be inconsistent, and they stay untouched for the rest
       * of this process. So a clog_close() on this handle in the child takes
       * the plain synchronous teardown path, because its own check,
       * "if (sh->async_enabled) _shared_async_teardown(sh);", is false then.
       * It never tries to join a thread the fork never copied into this
       * process, and never sends a sentinel into a queue nobody reads. The
       * child never frees the queue's and the buffer's backing memory,
       * because no thread is left to do that job. That's an accepted,
       * inherent leak for the rest of the child process, like any other
       * kernel-level resource a forked child abandons instead of
       * reclaiming. */
      sh->async_enabled = false;
      /* Nothing in this child ever delivers what async_buf holds, so the
       * missing continuation of a record never arrives. Clearing the flag
       * lets the child's synchronous writes rotate; left set, it would
       * suppress every rotation for the rest of this process, each one
       * waiting on a flush that can't happen. */
      sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
    }
    _ccol_mutex_unlock(sh->mutex);
  }

  if (in_child) {
    /* Finish every clog_close() call a fork() suspended in the middle of its
     * teardown, on behalf of the thread that never resumes that call in this
     * child (see the doc comment of clog_atfork_closing_t and where
     * _clog_atfork_prepare() records these calls). This follows steps 3, 4,
     * 5 and 6 of clog_close() in order, one after another, not concurrently.
     * The child has exactly one thread and nothing else can observe this
     * state yet, so it needs no lock beyond the slot table's write lock,
     * which the caller of this function (the pthread_atfork machinery)
     * already holds across the whole prepare, parent and child sequence. It
     * runs only after the two loops above, so by the time it can tear down a
     * shared target, that target's async_enabled and compression state have
     * already been downgraded; so it never tries to join a writer thread or
     * a compressor thread that doesn't exist in this child, or to drain a
     * queue that doesn't exist there. */
    size_t nc = cvector_elem_count(_clog_atfork_state.closing_slots);
    for (size_t i = 0; i < nc; i++) {
      clog_atfork_closing_t *c = (clog_atfork_closing_t *)cvector_at(
          _clog_atfork_state.closing_slots, i);
      clog_slot_t *slot =
          (clog_slot_t *)cvector_at(clog_slot_table.slots, c->idx);
      /* Only this loop itself could have retired the slot, since the real
       * closing thread never resumes in this child, so with the call graph as
       * it is this condition can never be true. We check it anyway instead
       * of assuming, following this file's standing rule of not relying on
       * call-graph reasoning alone. */
      if (slot->freed) continue;

      struct clogger *raw = c->raw;
      clog_shared_t *sh = raw->shared;

      /* The answer to step 3: a resolver that doesn't exist in this child can
       * never release this handle's pin, for the same reason as the pin
       * count reset in the locked_fields loop above. It's done again here for
       * every such slot because a slot the prepare step recorded as
       * "closing" isn't certain to have made it into locked_fields too: the
       * same out-of-memory degradation documented at the push above can stop
       * it. */
      ccol_pintable_reset_for(&clog_pintable, raw->self_handle);

      /* Step 4: retire the slot. fields_mutex has already been unlocked
       * above, before _logger_free() destroys it here: the locked_fields loop
       * unlocks it for every slot, and if _clog_atfork_prepare() failed to
       * record that lock, that function unlocked it inline instead. */
      _logger_free(raw);
      slot->freed = true;
      slot->ptr = NULL;
      slot->generation++;
      if (slot->generation == 0) slot->generation++; /* skip the sentinel */
      cvector_push_back(clog_slot_table.free_indices, &c->idx);

      /* Steps 5 and 6: free this handle's share of sh->ref_count and tear sh
       * down if it was the last share, exactly as the vanished thread would
       * have. It leaves out joining the async writer thread and tearing down
       * the queue, which aren't needed here: the loop above already forced
       * sh->async_enabled to false for this child, and after a fork there's no
       * writer thread and so no job in flight to drain. */
      if (--sh->ref_count == 0) {
        _clog_shareds_remove(clog_slot_table.live_shareds, sh);
        _clog_shareds_remove(clog_slot_table.file_shareds, sh);
        _shared_close_owned_fd(sh);
        _ccol_mutex_destroy(sh->mutex);
        _shared_free_partial(sh);
      }
    }
  }

  if (in_child) {
    /* A target in file_shareds that isn't in live_shareds is in the middle
     * of its last close on a parent thread, which this child doesn't have.
     * Nothing in the child ever finishes that close and its mutex may stay
     * locked, so an open of the same file in the child must not wait for it,
     * and the child forgets it. */
    size_t nfs = cvector_elem_count(clog_slot_table.file_shareds);
    for (size_t i = nfs; i-- > 0;) {
      clog_shared_t *fs =
          *(clog_shared_t **)cvector_at(clog_slot_table.file_shareds, i);
      bool live = false;
      size_t nl = cvector_elem_count(clog_slot_table.live_shareds);
      for (size_t j = 0; j < nl && !live; j++)
        live = *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, j) ==
               fs;
      if (!live) _clog_shareds_remove(clog_slot_table.file_shareds, fs);
    }
    if (_ccol_rw_lock_reinit_in_child(clog_slot_table.rwlock) != 0)
      _ccol_fatal_err(
          "clog atfork release: failed to reinit slot table rwlock");
  } else {
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }
  cvector_reset(_clog_atfork_state.locked_fields);
  /* This runs for every caller, not only in the child. The parent branch
   * never processes closing_slots, since the real closing thread is there and
   * does its own teardown normally, but prepare() fills this vector before
   * every fork() whether or not the entries end up being needed. Stale
   * entries left after a release on the parent side would stay around for
   * the next fork(), whose prepare() appends on top of them, and a later fork
   * that really needs this list would wrongly "finish" slots that were
   * retired long ago and that a different logger may be using by then. */
  cvector_reset(_clog_atfork_state.closing_slots);
}

static void _clog_atfork_parent(void) { _clog_atfork_release(false); }
static void _clog_atfork_child(void) { _clog_atfork_release(true); }
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/*
 * A defensive cleanup at process exit. It follows the defensive variant,
 * _cleanup_default_client in chttpclient.c, rather than the unconditional
 * one in chttpserver.c, because a process is expected to hold many clog
 * handles, each owned independently, with no single obvious owner that has
 * to close everything before exit. So this function frees the slot table's
 * bookkeeping vectors only once nothing can still reach them. A handle may
 * still be open, or a close still in flight, and a destructor or an atexit
 * handler in another translation unit can run later; freeing the vectors
 * under such a handler risks a use-after-free. In that case the release is
 * handed to whichever close is the last one out instead of being skipped. So
 * a program that does close its loggers leaves nothing behind whatever order
 * the destructors ran in, and a program that doesn't leaves them for the OS
 * to reclaim. */
/* Answers whether any slot still names a logger. The caller holds the write
 * lock.
 *
 * The scan reads slot->ptr, not slot->in_use. A close clears in_use as its
 * first step, so a second close or a new resolve gets rejected as early as
 * possible, and the rest of the teardown (draining the pins and the final
 * locked release of the index) comes after that. A scan that trusted in_use
 * alone would free this table under a close still in that window, and that
 * close's last step would then index the table. ptr is written only once a
 * slot is fully acquired and cleared only in that final locked step.
 *
 * Even ptr doesn't cover the whole window: a close clears ptr and then takes
 * the write lock again to remove its shared object from live_shareds, and
 * between those two points it is invisible here while it still needs the
 * table. closes_in_flight covers exactly that remainder, and that's why
 * every decision to release goes through _clog_table_still_needed_locked()
 * and never calls this function directly. */
/* Answers whether anything still needs this table: a slot that still names
 * a logger, or a close that has already cleared its slot but isn't done
 * with the table's other vectors. The caller holds the write lock. Both
 * deciders use this rather than the slot scan alone, because a close in its
 * own final steps is invisible to that scan by construction. */
static bool _clog_table_still_needed_locked(void);

static bool _clog_any_slot_live_locked(void) {
  size_t n = cvector_elem_count(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* Frees the table's bookkeeping and the pin index. The caller holds the
 * write lock and has established that nothing still needs this table.
 *
 * The pin index's slot storage is never freed while the process runs, on
 * purpose, because a resolve indexes it with no lock held, so a leak checker
 * that counts still-reachable memory as an error reports it at exit unless
 * it's freed here. Each vector is set to NULL as we go, so a later call
 * answers "already released" instead of indexing a freed vector. The rwlock
 * isn't destroyed, because this can run from an ordinary close that still
 * holds that lock. */
/* The check and the release both sit behind one call that isn't inlined, so
 * the destroy path that has to make that call keeps the code shape it would
 * have without any of this. Cold code in a hot object file isn't free:
 * inlined here, the same few instructions measurably slow an unrelated
 * container's push path, because they shift what the linker lays out around
 * that path, while the instruction count stays the same. */
static __attribute__((noinline)) void
_clog_release_slot_table_if_deferred_locked(void) {
  if (clog_slot_table.release_deferred && !_clog_table_still_needed_locked()) {
    _clog_release_slot_table_locked();
  }
}

static bool _clog_table_still_needed_locked(void) {
  return clog_slot_table.closes_in_flight != 0 || _clog_any_slot_live_locked();
}

static void _clog_release_slot_table_locked(void) {
  cvector_destroy(clog_slot_table.slots);
  cvector_destroy(clog_slot_table.free_indices);
  cvector_destroy(clog_slot_table.live_shareds);
  cvector_destroy(clog_slot_table.file_shareds);
  ccol_pintable_dispose(&clog_pintable);
#if CCOL_FORK_SAFETY_REQUIRED
  cvector_destroy(_clog_atfork_state.locked_fields);
  cvector_destroy(_clog_atfork_state.closing_slots);
#endif
  clog_slot_table.release_deferred = false;
}

/* How long the release of the table at exit or unload waits for its lock. A
 * thread holding the lock longer than that is stuck inside this module, and
 * the release then leaves the table allocated rather than hold up the end of
 * the process for ever. */
#define CLOG_TABLE_RELEASE_WAIT_MS 1000

__attribute__((destructor)) static void _cleanup_clog_slot_table(void) {
  if (!clog_slot_table.slots)
    return; /* never initialized, or already released */
  /* We hold this lock across the scan and everything it decides, as every
   * other module's equivalent teardown holds its own lock. A thread can still
   * call into this module while the process tears itself down, and without
   * the lock such a thread could be inside an acquire and publish a chunk
   * into the very index this function frees. The wait is bounded; see
   * CLOG_TABLE_RELEASE_WAIT_MS. */
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += CLOG_TABLE_RELEASE_WAIT_MS / 1000;
  until.tv_nsec += (long)(CLOG_TABLE_RELEASE_WAIT_MS % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000L;
  }
  if (_ccol_rw_lock_timedwrlock(clog_slot_table.rwlock, until) != 0) return;
  /* This runs only once nothing is left that could still resolve a handle.
   * When a logger is still open, the release is handed to whichever close is
   * the last one out instead of being skipped, so an application that does
   * close its loggers leaves nothing behind whatever order the destructors
   * ran in. */
  if (_clog_table_still_needed_locked()) {
    clog_slot_table.release_deferred = true;
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
    return;
  }
  _clog_release_slot_table_locked();
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  /* We don't destroy the rwlock itself, on purpose. It has static storage
     duration, so a lock left behind holds nothing a leak checker reports, and
     the destructor can't own its lifetime anyway: the deferred branch above
     returns while the table is still live, and the later release runs while
     holding this very lock, so there's no path on which every user is
     provably done with it. A destroy here would leave the other branch's
     entry points taking a lock on a destroyed object, which is undefined;
     without the destroy those entry points reach a defined outcome. An open
     or a derive after the table's release finds its vectors gone and fails
     with CLOG_INVALID, a fork() then locks and unlocks nothing else, and a
     call on a stale handle stops at this module's _ccol_fatal_err. The order
     of destructors across translation units isn't ours to decide, so no
     arrangement of who runs first rules out either outcome. */
}

/* ========================================================================== */
/*                         BUFFER HELPERS                                     */
/* ========================================================================== */

static int _buf_init(clog_buf_t *b, const ccol_memmgmt_procs_t *m_procs) {
  b->m_procs = m_procs;
  b->data = _ccol_mem_alloc(m_procs, CLOG_BUF_INITIAL);
  if (!b->data) return -1;
  b->data[0] = '\0';
  b->len = 0;
  b->cap = CLOG_BUF_INITIAL;
  b->cap_limit = CLOG_BUF_MAX;
  b->oom = false;
  return 0;
}

/* Raises b's growth ceiling above its CLOG_BUF_MAX default, and never lowers
 * it below that; see the doc comment of clog_buf_t.cap_limit.
 * _shared_async_init() calls it once, right after initializing
 * clog_shared_t.async_buf, so a caller who configures a
 * clog_async_cfg_t.flush_buffer_size larger than CLOG_BUF_MAX really gets
 * that much room for the batch, instead of having it silently capped at the
 * smaller default, which is meant for one record. */
static void _buf_raise_cap_limit(clog_buf_t *b, size_t new_limit) {
  if (new_limit > b->cap_limit) b->cap_limit = new_limit;
}

static void _buf_reset(clog_buf_t *b) { b->len = 0; }

static void _buf_free(clog_buf_t *b) {
  _ccol_mem_free(b->m_procs, b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Lets a test force the NEXT _buf_append() or _buf_appendf() call to report
 * a real allocation failure, deterministically, whether or not the call
 * really needs the buffer to grow. The check sits at the top of both
 * functions rather than inside _buf_ensure(), which _buf_appendf() calls only
 * after its own first attempt to fit in place fails. That position is on
 * purpose: it lets the hook force a failure even for a small fixed-size
 * append that never has to grow the buffer, such as the "backtrace
 * unavailable" marker of _emit_backtrace_syslog_lines(), which this
 * codebase's real CLOG_BUF_INITIAL already fits easily. Without the hook a
 * test could never observe a real allocator failure there. It disarms
 * itself the moment it fires, so it affects only the one call a test
 * targets.
 */
static _Atomic bool _clog_test_force_next_buf_ensure_failure_state = false;

static bool _clog_test_consume_forced_buf_ensure_failure(void) {
  return atomic_exchange(&_clog_test_force_next_buf_ensure_failure_state,
                         false);
}

void _clog_test_force_next_buf_ensure_failure(bool force) {
  atomic_store(&_clog_test_force_next_buf_ensure_failure_state, force);
}
#else
static inline bool _clog_test_consume_forced_buf_ensure_failure(void) {
  return false;
}
#endif

/* Makes sure at least `need` free bytes are available. Two different causes
 * of failure report -1 to the caller the same way, and only the second one
 * sets b->oom. Reaching b->cap_limit is an ordinary size rejection, which
 * happens for a record that is too large and for a shared batch buffer with
 * too little room left; it's never a sign of real memory pressure, and a
 * caller further up must never take it as one (see the doc comment of
 * clog_buf_t.oom). */
static int _buf_ensure(clog_buf_t *b, size_t need) {
  if (b->cap - b->len >= need) return 0;
  size_t new_cap = b->cap ? b->cap : CLOG_BUF_INITIAL;
  while (new_cap - b->len < need) {
    if (new_cap >= b->cap_limit) return -1;
    /* b->cap_limit can be a large value the caller configures (see
     * _buf_raise_cap_limit()), not always a fixed compile-time constant, so
     * the doubling has to be guarded against a size_t overflow rather than
     * assume new_cap*2 always fits.
     * Clamping straight to cap_limit here must NOT skip the loop's
     * sufficiency check the way an unconditional `break` would: cap_limit can
     * be reachable and still be smaller than `need`, success would then
     * falsely report the buffer as big enough, and the caller would run
     * memcpy or vsnprintf past the end of the allocation. So we go back to
     * the `while` condition instead, where the ordinary
     * `new_cap >= b->cap_limit` check catches a cap_limit that is too small on
     * the very next iteration, just as the ordinary doubling path (the one
     * without overflow) relies on that same check. */
    if (new_cap > SIZE_MAX / 2) {
      new_cap = b->cap_limit;
    } else {
      new_cap *= 2;
      if (new_cap > b->cap_limit) new_cap = b->cap_limit;
    }
  }
  char *p = _ccol_mem_realloc(b->m_procs, b->data, new_cap);
  if (!p) {
    b->oom = true;
    return -1;
  }
  b->data = p;
  b->cap = new_cap;
  return 0;
}

static int _buf_append(clog_buf_t *b, const char *s, size_t n) {
  if (_clog_test_consume_forced_buf_ensure_failure()) {
    b->oom = true;
    return -1;
  }
  if (_buf_ensure(b, n + 1) != 0) return -1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 0;
}

static int __attribute__((format(printf, 2, 3))) _buf_appendf(clog_buf_t *b,
                                                              const char *fmt,
                                                              ...) {
  if (_clog_test_consume_forced_buf_ensure_failure()) {
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
    if (_buf_ensure(b, (size_t)n + 1) != 0) {
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

/*
 * Fills esc, a buffer of 5 bytes or more, with the backslash escape sequence
 * of a control byte c (c < 0x20 or c == 0x7f), and sets *esc_len to its
 * length: 2 for \n, \r and \t, and 4 for the general \xXX form. Every value
 * escaper in this file (logfmt, an RFC 5424 SD-PARAM-VALUE, a syslog MSG)
 * uses it, so a control byte is always neutralised the same way whatever the
 * format.
 */
static void _ctrl_escape(unsigned char c, char esc[5], int *esc_len) {
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

/*
 * Classifies the UTF-8 multi-byte sequence starting at the non-ASCII lead
 * byte p[0] (0x80 to 0xff). Returns the sequence's total length (2, 3 or 4)
 * when it is complete and well formed: no overlong encoding, no encoded
 * surrogate half (U+D800 to U+DFFF), no codepoint past U+10FFFF, and every
 * continuation byte present and in range. Returns 0 when p[0] is itself an
 * invalid lead byte (a lone continuation byte, 0x80 to 0xbf; an overlong
 * 2-byte lead, 0xc0 or 0xc1; or 0xf5 to 0xff, which can never encode a valid
 * codepoint), and when the sequence is truncated or malformed some other way.
 * It never reads past p's NUL terminator, because each continuation byte is
 * checked against '\0' before anything else, so a sequence cut off by the
 * end of the string is reported as invalid without reading beyond it.
 */
static int _utf8_valid_seq_len(const unsigned char *p) {
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
    lo1 = (c0 == 0xe0) ? 0xa0 : 0x80; /* reject the overlong e0 80..9f range */
    hi1 = (c0 == 0xed) ? 0x9f : 0xbf; /* reject the ed a0..bf range, which
                                          encodes a surrogate half */
  } else {
    len = 4;
    lo1 = (c0 == 0xf0) ? 0x90 : 0x80; /* reject the overlong f0 80..8f range */
    hi1 = (c0 == 0xf4) ? 0x8f : 0xbf; /* reject the f4 90..bf range, which
                                          lies beyond U+10FFFF */
  }

  unsigned char c1 = p[1];
  if (c1 == '\0' || c1 < lo1 || c1 > hi1) return 0;
  for (int i = 2; i < len; i++) {
    unsigned char ci = p[i];
    if (ci == '\0' || ci < 0x80 || ci > 0xbf) return 0;
  }
  return len;
}

/*
 * Classifies the bytes at p, where p[0] is a non-ASCII byte (0x80 to 0xff),
 * for every value escaper in this file. Returns the length of a well-formed
 * UTF-8 sequence that passes through as it is, or 0 when the escaper needs to
 * write the bytes at p with _append_utf8_rewrite() instead: a C1 control
 * (U+0080 to U+009F, encoded as 0xc2 0x80 to 0xc2 0x9f), which a terminal
 * can read as a control sequence (0xc2 0x9b is the 8-bit CSI), and a byte
 * that isn't part of a well-formed UTF-8 sequence.
 */
static inline int _utf8_pass_len(const unsigned char *p) {
  if (p[0] == 0xc2 && p[1] <= 0x9f) return 0;
  return _utf8_valid_seq_len(p);
}

/*
 * Appends the replacement of the bytes at p for which _utf8_pass_len() gave
 * 0, and returns how many input bytes the replacement stands for, or -1 when
 * the append fails. A C1 control becomes the escape \u0080 to \u009f and
 * stands for its 2 bytes. A byte that isn't part of a well-formed UTF-8
 * sequence becomes U+FFFD and stands for that one byte, written as the
 * escape \ufffd when json is true and as the character's 3 UTF-8 bytes
 * otherwise.
 */
static int _append_utf8_rewrite(clog_buf_t *b, const unsigned char *p,
                                bool json) {
  if (p[0] == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) {
    static const char hex[] = "0123456789abcdef";
    char esc[6] = {'\\', 'u', '0', '0', hex[p[1] >> 4], hex[p[1] & 0x0f]};
    return _buf_append(b, esc, sizeof(esc)) == 0 ? 2 : -1;
  }
  if (json) return _buf_append(b, "\\ufffd", 6) == 0 ? 1 : -1;
  return _buf_append(b, "\xef\xbf\xbd", 3) == 0 ? 1 : -1;
}

/* Class of each byte for _buf_append_lv(): 0 for a printable ASCII byte a
 * logfmt value carries as it is, 1 for a byte that forces quoting (a space,
 * '=', '"', '\\', a control byte and DEL), 2 for a byte of 0x80 or more,
 * which starts a UTF-8 sequence that has to be checked, and 3 for the NUL
 * that ends the string. */
static const unsigned char _clog_lv_class[256] = {
    [0x00] = 3, [0x01 ... 0x1F] = 1, [' '] = 1,  ['='] = 1,
    ['"'] = 1,  ['\\'] = 1,          [0x7F] = 1, [0x80 ... 0xFF] = 2};

/*
 * Appends a value in a form that is safe for logfmt. A value holding a
 * space, '=', '"', '\\', a control character (C0, DEL or C1) or a byte that
 * isn't part of well-formed UTF-8 goes in double quotes, backslash-escaped:
 * a C1 control becomes \u0080 to \u009f and an invalid byte becomes U+FFFD
 * (see _append_utf8_rewrite()). An empty string is written as "". The record
 * is built under the target's mutex, so whatever this function costs adds to
 * the time other logging threads wait.
 */
static int _buf_append_lv(clog_buf_t *b, const char *s) {
  if (!s) return _buf_append(b, "null", 4);

  /* One table load decides each byte and the scan stops at the NUL, so an
   * unquoted value needs no strlen() of its own. */
  const unsigned char *p = (const unsigned char *)s;
  bool quote = false;
  for (;;) {
    unsigned char k = _clog_lv_class[*p];
    if (__builtin_expect(k == 0, 1)) {
      p++;
      continue;
    }
    if (k == 3) break;
    if (k == 2) {
      int n = _utf8_pass_len(p);
      if (n > 0) {
        p += n;
        continue;
      }
    }
    quote = true;
    break;
  }
  if (!quote && p != (const unsigned char *)s)
    return _buf_append(b, s, (size_t)(p - (const unsigned char *)s));

  if (_buf_append(b, "\"", 1) != 0) return -1;

  const char *run = s;
  for (const char *q = s;; q++) {
    unsigned char c = (unsigned char)*q;
    unsigned char k = _clog_lv_class[c];
    if (__builtin_expect(k == 0, 1)) continue;

    if (k == 3) {
      if (q > run && _buf_append(b, run, (size_t)(q - run)) != 0) return -1;
      break;
    }
    if (c == '"' || c == '\\') {
      if (q > run && _buf_append(b, run, (size_t)(q - run)) != 0) return -1;
      char esc[2] = {'\\', (char)c};
      if (_buf_append(b, esc, 2) != 0) return -1;
      run = q + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (q > run && _buf_append(b, run, (size_t)(q - run)) != 0) return -1;
      char esc[5];
      int esc_len;
      _ctrl_escape(c, esc, &esc_len);
      if (_buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = q + 1;
    } else if (c >= 0x80) {
      int n = _utf8_pass_len((const unsigned char *)q);
      if (n == 0) {
        if (q > run && _buf_append(b, run, (size_t)(q - run)) != 0) return -1;
        n = _append_utf8_rewrite(b, (const unsigned char *)q, false);
        if (n < 0) return -1;
        run = q + n;
      }
      q += n - 1; /* the loop steps past the last byte */
    }
    /* A space and '=' only need the quotes, so they stay in the run. */
  }

  return _buf_append(b, "\"", 1);
}

/* ========================================================================== */
/*                         JSON HELPERS                                       */
/* ========================================================================== */

/*
 * Appends the JSON-escaped content of s, without quotes around it. The
 * escapes are '"' -> '\"', '\' -> '\\', '\n'->'\n', '\r'->'\r',
 * '\t'->'\t', and every other control character, the C1 controls U+0080 to
 * U+009F included, -> '\uXXXX'. A byte or byte sequence that isn't
 * well-formed UTF-8 (a lone continuation byte, an overlong or out-of-range
 * lead byte, an encoded surrogate half, or part of a truncated multi-byte
 * sequence) is replaced, one invalid byte at a time, with the Unicode
 * replacement character (U+FFFD). So a message or field value built from any
 * data, binary data included, can never make this function write invalid
 * Unicode inside a JSON string; RFC 8259 wants JSON text to be valid
 * Unicode, and a strict parser or log aggregator downstream is entitled to
 * reject a document holding raw bytes that aren't UTF-8. Any other
 * well-formed multi-byte sequence (anything but a C1 control) passes through
 * unchanged and unescaped, exactly like any other printable byte. s must not
 * be NULL.
 */
static int _buf_append_json_content(clog_buf_t *b, const char *s) {
  const char *run = s;
  const char *p = s;
  for (;;) {
    unsigned char c = (unsigned char)*p;
    if (c == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (c == '"' || c == '\\') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[2] = {'\\', (char)c};
      if (_buf_append(b, esc, 2) != 0) return -1;
      p++;
      run = p;
      continue;
    }
    if (c < 0x20 || c == 0x7f) {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
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
      if (_buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      p++;
      run = p;
      continue;
    }
    if (c >= 0x80) {
      int seq_len = _utf8_pass_len((const unsigned char *)p);
      if (seq_len > 0) {
        p += seq_len;
        continue;
      }
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      seq_len = _append_utf8_rewrite(b, (const unsigned char *)p, true);
      if (seq_len < 0) return -1;
      p += seq_len;
      run = p;
      continue;
    }
    p++;
  }
  return 0;
}

/*
 * Appends a JSON key-value pair with a comma in front: ,"key":"value"
 * key must not be NULL. A val of NULL gives ,"key":null
 */
static int _buf_append_json_kv(clog_buf_t *b, const char *key,
                               const char *val) {
  if (_buf_append(b, ",\"", 2) != 0) return -1;
  if (_buf_append_json_content(b, key) != 0) return -1;
  if (_buf_append(b, "\":", 2) != 0) return -1;
  if (!val) return _buf_append(b, "null", 4);
  if (_buf_append(b, "\"", 1) != 0) return -1;
  if (_buf_append_json_content(b, val) != 0) return -1;
  return _buf_append(b, "\"", 1);
}

/* ========================================================================== */
/*                         SYSLOG HELPERS                                     */
/* ========================================================================== */

/*
 * Appends an RFC 5424 SD-PARAM-VALUE. '"', '\' and ']' are escaped as '\"',
 * '\\' and '\]', and every control character gets a backslash escape too,
 * the same as in the logfmt and JSON output formats: a C0 control and DEL as
 * in _ctrl_escape(), and a C1 control as \u0080 to \u009f. That includes
 * '\n' and '\r': the formal SD-PARAM-VALUE grammar doesn't restrict those
 * two, but without the escape they would split one syslog record across
 * several lines. RFC 5424 section 6.3.3 wants the value to be UTF-8, so a
 * well-formed UTF-8 sequence other than a C1 control passes through as it is
 * and each byte that isn't part of one becomes U+FFFD, encoded as UTF-8,
 * exactly where the JSON format writes the same character as an escape (see
 * _buf_append_json_content()). An s of NULL is written as the bare word
 * "null", just as _buf_append_lv() and _buf_append_json_kv() treat a NULL
 * value in their own formats. The caller already puts quotes around this
 * call, so "null" here looks unquoted inside an otherwise quoted
 * SD-PARAM-VALUE, exactly like logfmt's unquoted bare word `null`.
 */
static int _buf_append_sd_value(clog_buf_t *b, const char *s) {
  if (!s) return _buf_append(b, "null", 4);
  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;
    if (*p == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '"' || *p == '\\' || *p == ']') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[2] = {'\\', *p};
      if (_buf_append(b, esc, 2) != 0) return -1;
      run = p + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[5];
      int esc_len;
      _ctrl_escape(c, esc, &esc_len);
      if (_buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = p + 1;
    } else if (c >= 0x80) {
      int n = _utf8_pass_len((const unsigned char *)p);
      if (n == 0) {
        if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
        n = _append_utf8_rewrite(b, (const unsigned char *)p, false);
        if (n < 0) return -1;
        run = p + n;
      }
      p += n - 1; /* the loop steps past the last byte */
    }
  }
  return 0;
}

/*
 * Fills name, a buffer of 33 bytes or more, with an RFC 5424 SD-PARAM-NAME
 * derived from key. A key that already fits in the 32-character limit goes
 * in unchanged. A longer key gives its first 23 characters, then '~', then
 * an FNV-1a hash of the FULL key as 8 hex digits, 32 characters in all. Just
 * truncating isn't enough, because two different keys sharing a 32-character
 * prefix would then collide into the same SD-PARAM-NAME, which RFC 5424
 * forbids inside one SD-ELEMENT. Since the whole key feeds the suffix, such
 * a collision takes a real hash collision and not only a shared prefix.
 */
static void _sd_param_name(const char *key, char name[33]) {
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

/*
 * Appends free-form text with every control character backslash-escaped. A
 * backslash is written as '\\' too, as logfmt does inside a quoted value, so
 * the escaped text reads back without ambiguity: a real newline appears as
 * '\n', and the two characters '\' and 'n' as '\\n'. No quotes and no other
 * structure go around the text. It's used for content that has no escape
 * syntax of its own but still must not be able to split a record across
 * lines, such as RFC 5424 MSG content and, in the logfmt and syslog formats,
 * the text of a backtrace frame from backtrace_symbols(). JSON doesn't use it
 * for a backtrace frame; it puts each frame in as an ordinary JSON array
 * element through _buf_append_json_content(), which has its own escapes.
 * Unescaped, a control byte can desynchronize the output stream, '\n' and
 * '\r' most of all, so this function neutralises every one of them the same
 * way the logfmt and JSON formats do in their own message and value fields:
 * a C1 control becomes \u0080 to \u009f and a byte that isn't part of
 * well-formed UTF-8 becomes U+FFFD, so the output is always valid UTF-8. s
 * must not be NULL.
 */
static int _buf_append_ctrl_escaped(clog_buf_t *b, const char *s) {
  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;
    if (*p == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '\\') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      if (_buf_append(b, "\\\\", 2) != 0) return -1;
      run = p + 1;
    } else if (c < 0x20 || c == 0x7f) {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      char esc[5];
      int esc_len;
      _ctrl_escape(c, esc, &esc_len);
      if (_buf_append(b, esc, (size_t)esc_len) != 0) return -1;
      run = p + 1;
    } else if (c >= 0x80) {
      int n = _utf8_pass_len((const unsigned char *)p);
      if (n == 0) {
        if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
        n = _append_utf8_rewrite(b, (const unsigned char *)p, false);
        if (n < 0) return -1;
        run = p + n;
      }
      p += n - 1; /* the loop steps past the last byte */
    }
  }
  return 0;
}

/*
 * Sanitizes raw into out, a buffer of size outsz, for use as an RFC 5424
 * PRINTUSASCII field; APP-NAME and HOSTNAME share the same
 * "1*NNNPRINTUSASCII" grammar. Only the PRINTUSASCII bytes (0x21 to 0x7e)
 * are kept, and any other byte is dropped rather than stopping at the first
 * one, so a single byte that doesn't qualify (a space, a control character,
 * a non-ASCII byte) doesn't throw away everything after it in raw. At most
 * outsz-1 bytes of output are written, and when raw gives no PRINTUSASCII
 * byte at all (an empty raw, for example) the result falls back to "-"; that
 * fallback needs room, which means outsz >= 2.
 *
 * An outsz of 0 is allowed and does nothing: out is never touched, since it
 * has no byte where even a NUL terminator could safely go. For every
 * outsz >= 1, out is certain to be NUL-terminated within its first outsz
 * bytes and never beyond. The loop bound below is `i + 1 < outsz` on
 * purpose, not the `i < outsz - 1` that looks the same: outsz is a size_t,
 * so outsz - 1 underflows to SIZE_MAX for outsz == 0, which silently defeats
 * the bound and would let the loop write any distance past an out whose size
 * really is zero.
 */
static void _sanitize_syslog_printusascii_field(const char *raw, char *out,
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

/*
 * Returns the short program name the platform reports, or NULL when the
 * platform has no such facility or reports an empty name. The two callers
 * below share it and differ only in what they fall back to for NULL: the
 * general "proc" field every log line carries uses "unknown", and RFC 5424's
 * APP-NAME field uses "-", which is the "unavailable" sentinel of its own
 * grammar.
 */
static const char *_raw_progname(void) {
#if defined(__linux__)
  /* glibc and musl both keep this name. */
  if (program_invocation_short_name && program_invocation_short_name[0])
    return program_invocation_short_name;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  const char *p = getprogname();
  if (p && p[0]) return p;
#endif
  return NULL;
}

/* Gives the process name for the RFC 5424 APP-NAME field, or "-" when that
 * name isn't available. */
static const char *_syslog_appname(void) {
  const char *p = _raw_progname();
  return p ? p : "-";
}

/* Gives the executable's basename, or "unknown" when it isn't
 * available. */
static const char *_get_progname(void) {
  const char *p = _raw_progname();
  return p ? p : "unknown";
}

/* Gives the calling thread's OS-level thread ID. */
static pid_t _get_tid(void) {
#if defined(__linux__)
  return (pid_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
  uint64_t tid64 = 0;
  _ccol_get_thread_id_np(NULL, &tid64);
  return (pid_t)tid64;
#elif defined(__FreeBSD__)
  return (pid_t)_ccol_get_own_thread_os_id_np();
#elif defined(__OpenBSD__)
  return getthrid();
#else
  /* This platform has no portable way to get a real OS-level thread ID, so
   * we fall back to the pthread handle itself. It's unique per thread, so a
   * reader can still tell different threads' log lines apart, but unlike the
   * branches above the value doesn't match what OS-level tools show
   * (ps -eLf, /proc/<pid>/task, top -H and others). */
  return (pid_t)(uintptr_t)_ccol_get_thread_id();
#endif
}

/* Fills buf with the calling thread's name, writing at most bufsz-1
 * characters. */
static void _get_thread_name(char *buf, size_t bufsz) {
#if defined(__linux__)
  char name[16]; /* prctl writes at most 16 bytes including null */
  if (prctl(PR_GET_NAME, name) == 0 && name[0]) {
    snprintf(buf, bufsz, "%s", name);
    return;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  if (_ccol_get_thread_name_np(_ccol_get_thread_id(), buf, bufsz) == 0 &&
      buf[0])
    return;
  /* A thread nobody named has an empty name here, while Linux gives it the
   * program's name, which ps(1) shows for it as well. */
  snprintf(buf, bufsz, "%.15s", _get_progname());
  return;
#endif
  snprintf(buf, bufsz, "unknown");
}

/* ========================================================================== */
/*                         THREAD IDENTITY CACHE                              */
/* ========================================================================== */

/*
 * Every record names the process and thread that wrote it, as
 * "<progname>(<pid>):<thread name>(<tid>)". Reading each of those costs a
 * system call (getpid(), gettid() and prctl(PR_GET_NAME)), and apart from
 * the thread name none of them changes while a thread runs. So each thread
 * reads them once, the first time it logs, and keeps the formatted string in
 * thread-local storage, and after that a record costs no system call for its
 * identity.
 *
 * Two events change the identity, and each one refreshes the cache:
 *
 *   - fork(). The child has a new PID and its one thread a new TID. A fork
 *     child handler clears the cache of the forking thread, which is the
 *     child's only thread. The handler takes no lock, so where it sits among
 *     the other modules' handlers doesn't matter. It's registered whatever
 *     CCOL_FORK_SAFETY_REQUIRED says, because it keeps a record's content
 *     correct and protects no lock.
 *   - ccol_set_thread_name(). It renames the thread and updates the cache.
 *
 * A rename by any other means after the thread first logged,
 * pthread_setname_np() or prctl(PR_SET_NAME) for example, goes unseen: the
 * cache keeps the name the thread had when it first logged. So a thread that
 * logs should be renamed with ccol_set_thread_name(), and then its records
 * follow the new name.
 *
 * The cache lives on the heap and the thread holds a pointer to it. The
 * thread's first record allocates it, a thread-specific key frees it when
 * the thread exits, and unloading the library frees the unloading thread's
 * cache and deletes the key. A thread-local object this big would sit in
 * the library's thread-local block, which the loader must fit into a small
 * reserve when a process loads the library with dlopen(). The pointer itself
 * uses the initial-exec model, like the other thread-local fast paths (see
 * internal/ctlsmodel.h), so a record reads its identity with two loads and
 * no call. When the allocation fails, the record carries a fixed identity
 * that names no thread, and the thread's next record tries again.
 */
#define CLOG_PROC_VAL_LEN 256
/* The kernel stores a thread name of at most this many bytes. */
#define CLOG_THREAD_NAME_MAX 15

typedef struct {
  bool valid;
  pid_t pid;
  pid_t tid;
  char name[CLOG_THREAD_NAME_MAX + 1];
  char proc[CLOG_PROC_VAL_LEN]; /* "<progname>(<pid>):<name>(<tid>)" */
} clog_thread_ident_t;

#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread clog_thread_ident_t *_clog_tident;
#else
static __thread clog_thread_ident_t *_clog_tident
    __attribute__((tls_model("initial-exec")));
#endif
static _ccol_once_flag_t _clog_tident_once = _CCOL_ONCE_INIT;

/* Identity of a record whose thread couldn't get its cache. */
static const clog_thread_ident_t _clog_tident_unknown = {
    .valid = true, .pid = 0, .tid = 0, .name = "", .proc = "unknown"};

/* The key whose destructor frees the cache of an exiting thread. The module
 * destructor deletes it under _clog_tident_rwlock's write lock, and a thread
 * sets its value under the read lock after checking _clog_tident_key_live,
 * so a set never runs on a deleted key. _clog_tident_key_live stays false
 * when the key or the lock couldn't be created; then a cache is never freed
 * at thread exit, which costs one cache per thread that ends. */
static _ccol_thread_ls_key_t _clog_tident_key;
static _ccol_rw_lock_t _clog_tident_rwlock;
static atomic_bool _clog_tident_key_live;
/* Set by the module destructor. A record logged after it (from a later
 * destructor of a program that links the library statically) gets
 * _clog_tident_late, one identity for the whole process with no thread part,
 * because a cache allocated at that point has no destructor left to free
 * it. */
static atomic_bool _clog_tident_finalized;
static clog_thread_ident_t _clog_tident_late;
static _ccol_once_flag_t _clog_tident_late_once = _CCOL_ONCE_INIT;

#ifdef RUNNING_UNIT_TESTS
/* How many times the calling thread read its identity from the system. */
static __thread unsigned _clog_test_tident_fills;

unsigned _clog_test_thread_identity_fill_count(void) {
  return _clog_test_tident_fills;
}

const void *_clog_test_thread_identity_block(void) { return _clog_tident; }

void _clog_test_forget_thread_identity_pointer(void) { _clog_tident = NULL; }

static void _clog_tident_fini(void);
void _clog_test_run_identity_fini(void) { _clog_tident_fini(); }
#endif

static void _clog_tident_atfork_child(void) {
  if (_clog_tident) _clog_tident->valid = false;
}

/* Destructor of _clog_tident_key; arg is the thread's cache. */
static void _clog_tident_free(void *arg) {
  free(arg);
  _clog_tident = NULL;
}

static void _clog_tident_register_atfork(void) {
  /* If the registration fails, a forked child's records carry the parent's
   * identity; that's all it costs, and nothing else here depends on the
   * handler. */
  (void)_ccol_at_fork(NULL, NULL, _clog_tident_atfork_child);
  if (_ccol_rw_lock_init(_clog_tident_rwlock) != 0) return;
  if (_ccol_thread_ls_key_create(_clog_tident_key, _clog_tident_free) != 0)
    return;
  atomic_store(&_clog_tident_key_live, true);
}

#if defined(_CCOL_ATFORK_AT_LOAD)
/* See _CCOL_ATFORK_AT_LOAD in common.h. */
__attribute__((constructor)) static void _clog_atfork_at_load(void) {
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  _ccol_call_once(_clog_tident_once, _clog_tident_register_atfork);
}
#endif

/* Runs when the library unloads, and at exit. It frees the calling thread's
 * cache, which no key destructor frees for the thread that ends the process,
 * and deletes the key, whose destructor would otherwise point into unmapped
 * code for every thread that exits after a dlclose(). */
__attribute__((destructor)) static void _clog_tident_fini(void) {
  atomic_store(&_clog_tident_finalized, true);
  free(_clog_tident);
  _clog_tident = NULL;
  if (!atomic_load(&_clog_tident_key_live)) return;
  _ccol_rw_lock_wrlock(_clog_tident_rwlock);
  atomic_store(&_clog_tident_key_live, false);
  _ccol_thread_ls_key_delete(_clog_tident_key);
  _ccol_rw_lock_unlock(_clog_tident_rwlock);
}

static void _clog_tident_format(clog_thread_ident_t *ti) {
  if (snprintf(ti->proc, sizeof(ti->proc), "%.200s(%d):%.15s(%d)",
               _get_progname(), (int)ti->pid, ti->name, (int)ti->tid) < 0)
    ti->proc[0] = '\0';
}

static void _clog_tident_late_init(void) {
  _clog_tident_late.pid = getpid();
  _clog_tident_late.tid = 0;
  _clog_tident_late.name[0] = '\0';
  if (snprintf(_clog_tident_late.proc, sizeof(_clog_tident_late.proc),
               "%.200s(%d)", _get_progname(), (int)_clog_tident_late.pid) < 0)
    _clog_tident_late.proc[0] = '\0';
  _clog_tident_late.valid = true;
}

/* Reads the calling thread's identity from the system and caches it. It
 * stays out of line because each thread runs it only once.
 *
 * The value of _clog_tident_key owns the thread's cache. macOS releases a
 * thread's __thread storage before the key destructors run, so a record that
 * another key's destructor logs at thread exit finds _clog_tident NULL while
 * the key still holds the cache. We then take the cache back from the key,
 * because a new block would replace the key's value and leak the old one. */
static __attribute__((noinline, cold)) const clog_thread_ident_t *
_clog_tident_fill(void) {
  _ccol_call_once(_clog_tident_once, _clog_tident_register_atfork);
  clog_thread_ident_t *ti = _clog_tident;
  if (!ti && atomic_load(&_clog_tident_key_live)) {
    _ccol_rw_lock_rdlock(_clog_tident_rwlock);
    if (atomic_load(&_clog_tident_key_live))
      ti = _ccol_thread_ls_get(_clog_tident_key);
    _ccol_rw_lock_unlock(_clog_tident_rwlock);
    if (ti) _clog_tident = ti;
  }
  if (!ti && atomic_load(&_clog_tident_finalized)) {
    _ccol_call_once(_clog_tident_late_once, _clog_tident_late_init);
    return &_clog_tident_late;
  }
  if (!ti) {
    ti = malloc(sizeof(*ti));
    if (!ti) return &_clog_tident_unknown;
    if (atomic_load(&_clog_tident_key_live)) {
      _ccol_rw_lock_rdlock(_clog_tident_rwlock);
      if (atomic_load(&_clog_tident_key_live))
        (void)_ccol_thread_ls_set(_clog_tident_key, ti);
      _ccol_rw_lock_unlock(_clog_tident_rwlock);
    }
    _clog_tident = ti;
  }
#ifdef RUNNING_UNIT_TESTS
  _clog_test_tident_fills++;
#endif
  ti->pid = getpid();
  ti->tid = _get_tid();
  _get_thread_name(ti->name, sizeof(ti->name));
  _clog_tident_format(ti);
  ti->valid = true;
  return ti;
}

/* Gives the calling thread's cached identity. The pointer stays valid as long
 * as the calling thread runs, and only that thread may read through it. */
static inline const clog_thread_ident_t *_clog_tident_get(void) {
  const clog_thread_ident_t *ti = _clog_tident;
  if (__builtin_expect(!ti || !ti->valid, 0)) return _clog_tident_fill();
  return ti;
}

ccol_retval_t ccol_set_thread_name(const char *thread_name) {
  if (!thread_name || thread_name[0] == '\0') return ccol_invalid_args;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__) || \
    defined(__OpenBSD__)
  /* Linux keeps at most 15 bytes of a thread name and every platform keeps
   * the same 15, so a name reads back the same everywhere. */
  char name[CLOG_THREAD_NAME_MAX + 1];
  size_t n = strnlen(thread_name, CLOG_THREAD_NAME_MAX);
  memcpy(name, thread_name, n);
  name[n] = '\0';
#if defined(__linux__)
  if (prctl(PR_SET_NAME, name) != 0) return ccol_not_permitted;
#else
  if (_ccol_set_own_thread_name_np(name) != 0) return ccol_not_permitted;
#endif
  clog_thread_ident_t *ti = _clog_tident;
  if (!ti || !ti->valid) {
    /* The first read of the identity picks up the name the kernel holds at
     * that point, which is this one. */
    (void)_clog_tident_fill();
  } else {
    memcpy(ti->name, name, n + 1);
    _clog_tident_format(ti);
  }
  return ccol_success;
#else
  /* The library supports no rename of the calling thread on this
   * platform. */
  return ccol_not_permitted;
#endif
}

/* ========================================================================== */
/*                         I/O HELPER                                         */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/*
 * Cap on how many bytes each of the next _clog_test_write_cap_uses calls of
 * _write_all() can deliver, so a test can reproduce a short write, where the
 * kernel takes only the first part of a record (as a full filesystem with
 * ENOSPC, or an RLIMIT_FSIZE ceiling, makes it do), without putting the
 * whole test process under a real resource limit. The writes after the last
 * capped one succeed, and that's what lets a test watch the framing get
 * repaired. A use count of 0 means the cap isn't armed. Each capped write
 * uses up one, so the cap affects only the writes a test targets, and a
 * count above one reproduces a descriptor that can't take the rest of a
 * record across more than one attempt.
 */
static _Atomic size_t _clog_test_write_cap = 0;
static _Atomic unsigned int _clog_test_write_cap_uses = 0;

static size_t _clog_test_consume_next_write_cap(void) {
  unsigned int uses = atomic_load(&_clog_test_write_cap_uses);
  while (uses > 0) {
    if (atomic_compare_exchange_weak(&_clog_test_write_cap_uses, &uses,
                                     uses - 1))
      return atomic_load(&_clog_test_write_cap);
  }
  return 0;
}

void _clog_test_force_short_writes(size_t accepted_bytes,
                                   unsigned int write_count) {
  atomic_store(&_clog_test_write_cap, accepted_bytes);
  atomic_store(&_clog_test_write_cap_uses, accepted_bytes ? write_count : 0u);
}

/*
 * The errno each of the next _clog_test_write_error_uses calls of
 * _write_all() fails with before writing a byte, so a test can reproduce a
 * write error of any class on a descriptor that otherwise works and see what
 * the logger does with the bytes that didn't go out.
 */
static _Atomic int _clog_test_write_error = 0;
static _Atomic unsigned int _clog_test_write_error_uses = 0;

static int _clog_test_consume_next_write_error(void) {
  unsigned int uses = atomic_load(&_clog_test_write_error_uses);
  while (uses > 0) {
    if (atomic_compare_exchange_weak(&_clog_test_write_error_uses, &uses,
                                     uses - 1))
      return atomic_load(&_clog_test_write_error);
  }
  return 0;
}

void _clog_test_force_write_errors(int err, unsigned int write_count) {
  atomic_store(&_clog_test_write_error, err);
  atomic_store(&_clog_test_write_error_uses, err ? write_count : 0u);
}

/*
 * Room left on every target's output while _clog_test_room_err isn't 0, so a
 * test can reproduce a full disk: writes take bytes until the room is used
 * up, a write that needs more takes what's left, and every write after that
 * fails with the errno. A write that gets less than it asks for is a
 * refusal; after _clog_test_room_refusals of them the output has room again,
 * and while that count is 0 the output stays full until the test gives it
 * room again.
 */
static _Atomic int _clog_test_room_err = 0;
static _Atomic size_t _clog_test_room = 0;
static _Atomic unsigned int _clog_test_room_refusals = 0;

void _clog_test_set_output_room(int err, size_t room, unsigned int refusals) {
  atomic_store(&_clog_test_room, room);
  atomic_store(&_clog_test_room_refusals, refusals);
  atomic_store(&_clog_test_room_err, err);
}

/* Returns how many of len bytes the output takes, and sets *err to the full
 * output's errno when that's fewer than len, and to 0 otherwise. */
static size_t _clog_test_room_clip(size_t len, int *err) {
  int e = atomic_load(&_clog_test_room_err);
  *err = 0;
  if (e == 0) return len;
  size_t room = atomic_load(&_clog_test_room);
  for (;;) {
    size_t take = len < room ? len : room;
    if (atomic_compare_exchange_weak(&_clog_test_room, &room, room - take)) {
      if (take < len) {
        *err = e;
        unsigned int r = atomic_load(&_clog_test_room_refusals);
        if (r == 1) atomic_store(&_clog_test_room_err, 0);
        if (r > 0) atomic_store(&_clog_test_room_refusals, r - 1);
      }
      return take;
    }
  }
}

/* While set, a socket sink is classified as a pipe, the way a system without
 * MSG_NOSIGNAL writes it, so a test can run that path. */
static _Atomic bool _clog_test_sockets_as_pipes = false;

void _clog_test_classify_sockets_as_pipes(bool on) {
  atomic_store(&_clog_test_sockets_as_pipes, on);
}

static bool _clog_test_socket_is_pipe(void) {
  return atomic_load(&_clog_test_sockets_as_pipes);
}
#else
static inline bool _clog_test_socket_is_pipe(void) { return false; }
static inline size_t _clog_test_consume_next_write_cap(void) { return 0; }
static inline int _clog_test_consume_next_write_error(void) { return 0; }
static inline size_t _clog_test_room_clip(size_t len, int *err) {
  *err = 0;
  return len;
}
#endif

/* ========================================================================== */
/*                         SIGPIPE AND SIGXFSZ                                */
/* ========================================================================== */

/*
 * Two signals can end the process from inside a logger's write, since the
 * default disposition of both kills it, and that's a nasty way for a log
 * call to stop an otherwise healthy application. Depending on the kind of
 * descriptor, the library arranges for its own writes never to raise them,
 * and it touches the process's signal state only where nothing cheaper
 * works.
 *
 * SIGPIPE: a write(2) to a pipe or a socket whose reading end is gone fails
 * with EPIPE and also raises SIGPIPE.
 *
 *   - A regular file, a terminal and any other descriptor that is neither a
 *     pipe nor a socket can't raise SIGPIPE.
 *   - A socket is written with send(2) and MSG_NOSIGNAL, which fails with
 *     EPIPE and raises nothing. That changes no signal state, so it applies
 *     under every policy.
 *   - A pipe or a FIFO has no such flag. Under CLOG_SIGNALS_AUTO the async
 *     writer thread keeps SIGPIPE blocked in its own mask and consumes the
 *     SIGPIPE its own failed write left pending on it. Any other thread that
 *     writes to a pipe sink sets the whole process's disposition to SIG_IGN,
 *     once, the first time it does so, and only when the disposition is
 *     still SIG_DFL; a handler or an ignore the application installed stays
 *     as it is.
 *
 * SIGXFSZ: a write(2) to a regular file that would go past the process's
 * RLIMIT_FSIZE limit writes what fits and raises SIGXFSZ, and a write that
 * starts at the limit writes nothing, fails with EFBIG and raises it. The
 * repair of such a target (the newline that closes a fragment, the loss
 * marker) is itself a write to the same file, so a logger at the limit has
 * to survive the signal. Linux sends it to the writing thread, but FreeBSD
 * raises it on the whole process, so a blocked mask in one thread doesn't
 * protect the process there. Under CLOG_SIGNALS_AUTO, opening a regular
 * file (and a rotation opening the next one) sets SIGXFSZ's disposition to
 * SIG_IGN for the whole process, once, and only while it's still SIG_DFL.
 * It's done at the open and not after a first short write, because a record
 * that ends exactly at the limit leaves the next write to hit the limit with
 * no short write before it. The async writer thread and the compressor
 * thread also keep SIGXFSZ blocked in their own masks, and consume the one
 * their own failed write left pending.
 *
 * Under CLOG_SIGNALS_UNTOUCHED the library changes no disposition and no
 * signal mask, and each write raises whatever the kernel raises for it,
 * which the application's disposition then handles.
 *
 * A write that fails with EPIPE drops its record, like any other write error
 * the library can't recover from; a write that fails with EFBIG leaves a
 * debt that the next write the file accepts repairs.
 */
#if defined(MSG_NOSIGNAL)
#define CLOG_HAS_MSG_NOSIGNAL 1
#else
#define CLOG_HAS_MSG_NOSIGNAL 0
#endif

typedef enum {
  CLOG_SINK_PLAIN = 0,
  CLOG_SINK_PIPE = 1,
  CLOG_SINK_SOCKET = 2,
  /* A flag on top of one of the kinds above, set only by a CLOG_FATAL call,
   * under the target's mutex, for a sink that isn't a regular file: every
   * write to the sink then ends by _clog_fatal_deadline (see
   * _clog_write_fatal()). CLOG_SINK_PLAIN with this flag is a terminal or
   * some other device. */
  CLOG_SINK_FATAL_BOUND = 4,
} clog_sink_kind_t;

/* The policy clog_set_signal_policy() sets. It's read relaxed, because each
 * reader acts on the value it reads and orders nothing else by it. */
static _Atomic int _clog_signal_policy = CLOG_SIGNALS_AUTO;

static _ccol_once_flag_t _clog_sigpipe_once = _CCOL_ONCE_INIT;
/* Set once _clog_sigpipe_once has run, so a later pipe write skips the call.
 * The disposition is kernel state, so the flag orders nothing. */
static atomic_bool _clog_sigpipe_once_done;

static _ccol_once_flag_t _clog_sigxfsz_once = _CCOL_ONCE_INIT;
/* The same for SIGXFSZ, which the open of a regular file sets. */
static atomic_bool _clog_sigxfsz_once_done;

/* The calling thread's signal state, as a set of bits. It's 0 on every
 * thread except this module's async writer threads and compressor threads
 * (CLOG_SIGT_OWN), which record there, for SIGPIPE and SIGXFSZ each,
 * whether this module blocked it in the thread's mask or the thread
 * inherited it blocked from its creator. */
enum {
  CLOG_SIGT_OWN = 1,            /* a writer or compressor thread */
  CLOG_SIGT_PIPE_BLOCKED = 2,   /* this module blocked SIGPIPE */
  CLOG_SIGT_PIPE_INHERITED = 4, /* SIGPIPE came blocked from the creator */
  CLOG_SIGT_XFSZ_BLOCKED = 8,   /* this module blocked SIGXFSZ */
  CLOG_SIGT_XFSZ_INHERITED = 16 /* SIGXFSZ came blocked from the creator */
};
/* Initial-exec model, as for the thread-local state of cpintable and
 * cmempool: under the general model every access from a shared library is a
 * __tls_get_addr call, and this one sits on every write to a pipe. */
#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread unsigned char _clog_sig_thread;
#else
static __thread unsigned char _clog_sig_thread
    __attribute__((tls_model("initial-exec")));
#endif

/* Sets sig's disposition to SIG_IGN when it is SIG_DFL. A handler or an
 * ignore the application installed is the application's decision, so the
 * library leaves it alone. */
static void _clog_ignore_if_default(int sig) {
  struct sigaction cur;
  if (sigaction(sig, NULL, &cur) != 0) return;
  if ((cur.sa_flags & SA_SIGINFO) || cur.sa_handler != SIG_DFL) return;
  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  (void)sigaction(sig, &ign, NULL);
}

/* Each runs once for the process, under its own once flag. */
static void _clog_ignore_sigpipe(void) { _clog_ignore_if_default(SIGPIPE); }
static void _clog_ignore_sigxfsz(void) { _clog_ignore_if_default(SIGXFSZ); }

static __attribute__((noinline, cold)) void _clog_ignore_sigxfsz_once(void) {
  _ccol_call_once(_clog_sigxfsz_once, _clog_ignore_sigxfsz);
  atomic_store_explicit(&_clog_sigxfsz_once_done, true, memory_order_relaxed);
}

/* Runs when a logger starts writing a regular file: at its open, and when a
 * rotation opens the next file. See the section comment above. */
static inline void _clog_file_sink_prepare(void) {
  if (atomic_load_explicit(&_clog_signal_policy, memory_order_relaxed) ==
          CLOG_SIGNALS_AUTO &&
      !atomic_load_explicit(&_clog_sigxfsz_once_done, memory_order_relaxed))
    _clog_ignore_sigxfsz_once();
}

/* Returns fd's clog_sink_kind_t, and prepares the process's signal state for
 * a regular file (see _clog_file_sink_prepare()). A descriptor fstat() can't
 * examine is CLOG_SINK_PLAIN, since every write to it fails with EBADF,
 * which raises nothing. Without MSG_NOSIGNAL a socket takes the pipe path. */
static unsigned char _clog_classify_sink(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return CLOG_SINK_PLAIN;
  if (S_ISFIFO(st.st_mode)) return CLOG_SINK_PIPE;
  if (S_ISSOCK(st.st_mode))
    return CLOG_HAS_MSG_NOSIGNAL && !_clog_test_socket_is_pipe()
               ? CLOG_SINK_SOCKET
               : CLOG_SINK_PIPE;
  if (S_ISREG(st.st_mode)) _clog_file_sink_prepare();
  return CLOG_SINK_PLAIN;
}

/* Returns true when fd is a message-oriented socket, which keeps the boundary
 * of each write (see clog_shared_t.sink_per_record), and false for any other
 * descriptor, including one that isn't a socket at all. */
static bool _clog_sink_is_message(int fd) {
  int type = 0;
  socklen_t type_len = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0) return false;
  if (type == SOCK_DGRAM || type == SOCK_SEQPACKET) return true;
#if defined(SOCK_RDM)
  if (type == SOCK_RDM) return true;
#endif
  return false;
}

/* Blocks or unblocks SIGPIPE and SIGXFSZ in the calling module thread's mask
 * so that exactly the signals in `want` (a set of CLOG_SIGT_*_BLOCKED bits)
 * are the ones this module blocks, and records the result. See
 * _clog_thread_sync_signal_mask(). */
static __attribute__((noinline, cold)) void _clog_thread_set_signal_mask(
    unsigned char want) {
  unsigned char st = _clog_sig_thread;
  unsigned char cur = st & (CLOG_SIGT_PIPE_BLOCKED | CLOG_SIGT_XFSZ_BLOCKED);
  unsigned char add = want & (unsigned char)~cur;
  unsigned char drop = cur & (unsigned char)~want;
  sigset_t set;
  if (add != 0) {
    sigemptyset(&set);
    if (add & CLOG_SIGT_PIPE_BLOCKED) sigaddset(&set, SIGPIPE);
    if (add & CLOG_SIGT_XFSZ_BLOCKED) sigaddset(&set, SIGXFSZ);
    if (_ccol_thread_sigmask(SIG_BLOCK, &set, NULL) == 0) st |= add;
  }
  if (drop != 0) {
    sigemptyset(&set);
    if (drop & CLOG_SIGT_PIPE_BLOCKED) sigaddset(&set, SIGPIPE);
    if (drop & CLOG_SIGT_XFSZ_BLOCKED) sigaddset(&set, SIGXFSZ);
    if (_ccol_thread_sigmask(SIG_UNBLOCK, &set, NULL) == 0)
      st &= (unsigned char)~drop;
  }
  _clog_sig_thread = st;
}

/* Brings a writer or compressor thread's mask in line with the current
 * policy: SIGPIPE and SIGXFSZ blocked under CLOG_SIGNALS_AUTO, and as the
 * thread inherited them under CLOG_SIGNALS_UNTOUCHED. The thread unblocks
 * only what this module blocked, and never leaves a signal of its own
 * pending (see _clog_consume_own_signal()), so an unblock delivers nothing
 * this module caused. A signal the thread inherited blocked keeps the mask
 * its creator gave it. */
static inline void _clog_thread_sync_signal_mask(void) {
  unsigned char st = _clog_sig_thread;
  unsigned char want = 0;
  if (atomic_load_explicit(&_clog_signal_policy, memory_order_relaxed) ==
      CLOG_SIGNALS_AUTO) {
    if (!(st & CLOG_SIGT_PIPE_INHERITED)) want |= CLOG_SIGT_PIPE_BLOCKED;
    if (!(st & CLOG_SIGT_XFSZ_INHERITED)) want |= CLOG_SIGT_XFSZ_BLOCKED;
  }
  if ((st & (CLOG_SIGT_PIPE_BLOCKED | CLOG_SIGT_XFSZ_BLOCKED)) == want) return;
  _clog_thread_set_signal_mask(want);
}

/* The first statement of an async writer thread and of a compressor thread.
 * It marks the thread as one of this module's and applies the current policy
 * to its mask. */
static __attribute__((noinline, cold)) void _clog_thread_signal_init(void) {
  unsigned char st = CLOG_SIGT_OWN;
  sigset_t cur;
  sigemptyset(&cur);
  if (_ccol_thread_sigmask(SIG_BLOCK, NULL, &cur) == 0) {
    if (sigismember(&cur, SIGPIPE) == 1) st |= CLOG_SIGT_PIPE_INHERITED;
    if (sigismember(&cur, SIGXFSZ) == 1) st |= CLOG_SIGT_XFSZ_INHERITED;
  }
  _clog_sig_thread = st;
  _clog_thread_sync_signal_mask();
}

static __attribute__((noinline, cold)) void _clog_ignore_sigpipe_once(void) {
  _ccol_call_once(_clog_sigpipe_once, _clog_ignore_sigpipe);
  atomic_store_explicit(&_clog_sigpipe_once_done, true, memory_order_relaxed);
}

/* Runs before every write to a pipe sink. See the section comment above. */
static inline void _clog_pipe_write_prepare(void) {
  if (_clog_sig_thread != 0) {
    _clog_thread_sync_signal_mask();
    return;
  }
  if (atomic_load_explicit(&_clog_signal_policy, memory_order_relaxed) ==
          CLOG_SIGNALS_AUTO &&
      !atomic_load_explicit(&_clog_sigpipe_once_done, memory_order_relaxed))
    _clog_ignore_sigpipe_once();
}

#if defined(F_SETNOSIGPIPE)
/* macOS raises the SIGPIPE of a failed pipe write on the whole process, not
 * on the writing thread, so the async writer thread's mask doesn't keep it
 * away from a thread with the default disposition. There the writer thread
 * sets F_SETNOSIGPIPE on the descriptor for the length of each write under
 * CLOG_SIGNALS_AUTO, and such a write fails with EPIPE without raising any
 * signal. The begin returns the value to restore, or -1 when it changed
 * nothing; the end puts the earlier value back, so the application's
 * descriptor keeps its own setting. */
static int _clog_pipe_nosigpipe_begin(int fd) {
  if (_clog_sig_thread == 0 ||
      atomic_load_explicit(&_clog_signal_policy, memory_order_relaxed) !=
          CLOG_SIGNALS_AUTO)
    return -1;
  int prev = fcntl(fd, F_GETNOSIGPIPE);
  if (prev != 0) return -1;
  return fcntl(fd, F_SETNOSIGPIPE, 1) == 0 ? 0 : -1;
}

static void _clog_pipe_nosigpipe_end(int fd, int prev) {
  if (prev < 0) return;
  int saved_errno = errno;
  (void)fcntl(fd, F_SETNOSIGPIPE, prev);
  errno = saved_errno;
}
#endif

/* Runs after a write failed in a way that raised sig for the calling thread:
 * SIGPIPE after EPIPE on a pipe sink, SIGXFSZ after EFBIG on a regular file.
 * `bit` is sig's CLOG_SIGT_*_BLOCKED bit. When this module blocked sig on
 * this thread the signal is pending on it, and every pending instance is
 * consumed here (Linux keeps one pending, but FreeBSD queues one per failed
 * write); otherwise an unblock after a policy change would deliver one and
 * end the process for a write the library already handled. A signal a write
 * raised for this thread is directed at it, and a thread-directed signal is
 * taken before a process-directed one. */
static __attribute__((noinline, cold)) void _clog_consume_own_signal(
    int sig, unsigned char bit) {
  if (!(_clog_sig_thread & bit)) return;
  /* The failed write's errno stays in errno for the caller of the write
   * loop, which decides from it whether the undelivered bytes can ever go
   * out (see _clog_write_error_is_transient()). */
  int saved_errno = errno;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
#if defined(__APPLE__) || defined(__OpenBSD__)
  /* macOS and OpenBSD have no sigtimedwait(), and their sigpending() also
   * reports a signal pending for the whole process, which another thread can
   * take first, so a sigwait() for that one could block for ever. Directing
   * one instance at this thread before the sigwait() makes sure it has one to
   * take, and neither system queues a second pending instance on a thread,
   * so this takes the failed write's instance. An instance pending for the
   * process stays pending, since this write didn't raise it. */
  sigset_t pending;
  sigemptyset(&pending);
  int got;
  if (sigpending(&pending) == 0 && sigismember(&pending, sig) == 1 &&
      _ccol_thread_kill(_ccol_get_thread_id(), sig) == 0)
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

/* The two users of _clog_consume_own_signal(). */
static inline void _clog_consume_own_sigpipe(void) {
  _clog_consume_own_signal(SIGPIPE, CLOG_SIGT_PIPE_BLOCKED);
}
static inline void _clog_consume_own_sigxfsz(void) {
  _clog_consume_own_signal(SIGXFSZ, CLOG_SIGT_XFSZ_BLOCKED);
}

ccol_retval_t clog_set_signal_policy(clog_signal_policy_t policy) {
  if ((int)policy != CLOG_SIGNALS_AUTO && (int)policy != CLOG_SIGNALS_UNTOUCHED)
    return ccol_invalid_args;
  atomic_store_explicit(&_clog_signal_policy, (int)policy,
                        memory_order_relaxed);
  return ccol_success;
}

/*
 * A write() loop that retries after EINTR and after a partial write, like a
 * plain retry loop, and also waits with poll() for the fd to become writable
 * again after EAGAIN or EWOULDBLOCK, instead of treating a temporary "would
 * block" as a permanent failure. That matters because
 * clog_create_from_fd_mp() puts no restriction on the blocking mode of the
 * caller's fd, and the documented use of CLOG_FMT_SYSLOG in the header is a
 * UNIX datagram socket, which is exactly the kind of descriptor that can
 * legitimately give EAGAIN under load when its send buffer fills for a
 * moment. Without the wait, a non-blocking fd would silently lose the rest of
 * a record for good the instant the peer can't keep up, which is exactly
 * when log visibility matters most. The block happens while the caller holds
 * shared->mutex, and it makes a non-blocking fd behave the way a blocking one
 * already does when its peer can't keep up, so it adds no risk that a plain
 * blocking descriptor doesn't already have.
 *
 * poll_timeout_ms says whether the write may wait without a bound. Every
 * write of a record's bytes passes CLOG_WRITE_WAIT_FOREVER, because this
 * logger accepted that record and so delivers it however long the
 * descriptor takes to take it. A recovery step, which runs only so that a
 * DIFFERENT record can go out or so the process can stop, passes a number of
 * milliseconds instead, and then the whole write ends within that time on
 * every descriptor whose readiness poll(2) reports; see
 * _write_all_bounded(). Such a step drains the continuation of a record
 * that is already half written (see _clog_flush_and_settle()), settles a
 * batch on the fatal path or at exit, or writes the marker naming a loss. An
 * ordinary log call can get there, and so can CLOG_FATAL, and neither may
 * wait without a bound on bytes an earlier call left behind.
 *
 * Returns how many bytes it really wrote, which can be less than len: a
 * really unrecoverable error, a "no progress" condition (w == 0), or a bound
 * that expires can each stop it partway. A caller that tracks bytes_written
 * for rotation must use this return value and not len, so that counter never
 * runs ahead of the file's real size on disk.
 */
#define CLOG_WRITE_WAIT_FOREVER (-1)
/* Long enough for a descriptor whose peer is only behind for a moment to
 * take the continuation, and short enough that a descriptor that can't take
 * it holds up no log call, fatal or otherwise. */
#define CLOG_SPLIT_RECORD_DRAIN_WAIT_MS 100

/* Events of a poll(2) that waits for room in fd. OpenBSD never wakes a
 * POLLOUT wait on an anonymous pipe (a FIFO with no device and no inode)
 * when the reader closes, so a writer whose reader is gone would wait for
 * ever; asking for POLLIN as well makes the close report POLLHUP. The write
 * end of a pipe never has data to read, so POLLIN reports nothing else. */
static short _clog_pollout_events(int fd) {
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

/* After EAGAIN or EWOULDBLOCK, waits without a bound until fd is writable
 * again. Returns true when the write should be tried again. It's out of line
 * because it is the only part of the write loop that needs an object in
 * memory, and that way the loop itself keeps no stack canary on its
 * one-write-per-record path. */
static __attribute__((noinline)) bool _clog_wait_writable(int fd) {
  struct pollfd pfd = {
      .fd = fd, .events = _clog_pollout_events(fd), .revents = 0};
  int pr = poll(&pfd, 1, -1);
  /* An interruption just waits again, since an unbounded wait has no time to
   * keep. */
  if (pr < 0 && errno == EINTR) return true;
  /* The kernel reports POLLERR, POLLHUP and POLLNVAL on every call whatever
   * `events` mask the caller asks for, so pr > 0 alone doesn't mean the fd
   * is writable. We retry the write only once POLLOUT itself is set, and
   * otherwise stop, since the state really is unrecoverable, rather than
   * call write() again on an fd that only reported an error or a hangup. */
  return pr > 0 && (pfd.revents & POLLOUT);
}

/* A socket whose receiver has no room for a datagram can refuse the send
 * with ENOBUFS instead of blocking: FreeBSD does that for an AF_UNIX
 * datagram socket, and poll(2) still reports POLLOUT there because the room
 * that's missing is on the receiver's side. All you can do is try again
 * later, as FreeBSD's own syslog(3) does. *step_us holds the next pause; it
 * starts at 0, and each call doubles it from 50 microseconds up to 10
 * milliseconds. limit_ms bounds the pause when it isn't negative. */
static __attribute__((noinline, cold)) void _clog_enobufs_backoff(
    unsigned *step_us, int limit_ms) {
  unsigned us = *step_us ? *step_us : 50u;
  *step_us = us >= 5000u ? 10000u : us * 2u;
  if (limit_ms >= 0 && (long long)us > (long long)limit_ms * 1000LL)
    us = (unsigned)limit_ms * 1000u;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)us * 1000L};
  nanosleep(&ts, NULL);
}

/* The write loop of CLOG_WRITE_WAIT_FOREVER. A bounded write never gets here;
 * see _write_all_bounded(). */
static inline __attribute__((always_inline)) size_t
_write_all_body(int fd, clog_sink_kind_t kind, const char *data, size_t len) {
  size_t total = 0;
  int forced_err = _clog_test_consume_next_write_error();
  if (forced_err != 0) {
    errno = forced_err;
    return 0;
  }
  size_t forced_cap = _clog_test_consume_next_write_cap();
  if (forced_cap > 0 && forced_cap < len) len = forced_cap;
  int room_err;
  len = _clog_test_room_clip(len, &room_err);
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
      /* An unbounded write waits for the receiver, as a blocking send does
       * where the system blocks it; see _clog_enobufs_backoff(). */
      if (kind == CLOG_SINK_SOCKET && errno == ENOBUFS) {
        _clog_enobufs_backoff(&nobufs_step_us, -1);
        continue;
      }
      if (kind == CLOG_SINK_PIPE && errno == EPIPE) {
        _clog_consume_own_sigpipe();
        break; /* the reader is gone, which is unrecoverable */
      }
      if (kind == CLOG_SINK_PLAIN && errno == EFBIG) {
        _clog_consume_own_sigxfsz();
        break; /* the file size limit; the repair waits for room */
      }
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && _clog_wait_writable(fd))
        continue;
      break; /* really unrecoverable, so stop and report what went out */
    }
    if (w == 0)
      break; /* no progress, from a quota or an fd limit; don't
                spin */
    data += (size_t)w;
    len -= (size_t)w;
    total += (size_t)w;
  }
  if (room_err != 0) errno = room_err;
  return total;
}

/* The path of a regular file and of any other sink that can't raise SIGPIPE.
 * A regular file can raise SIGXFSZ; see "SIGPIPE AND SIGXFSZ". */
static __attribute__((noinline)) size_t _write_all_plain(int fd,
                                                         const char *data,
                                                         size_t len) {
  return _write_all_body(fd, CLOG_SINK_PLAIN, data, len);
}

/* Set once a CLOG_FATAL call starts, and never cleared; see
 * _clog_write_fatal(). _clog_fatal_deadline is written once before it, and a
 * write that CLOG_SINK_FATAL_BOUND sends down the bounded path reads the
 * deadline under the target's mutex, the same mutex under which the fatal
 * call set that flag after fixing the deadline. */
static _Atomic bool _clog_fatal_bound_armed;
static struct timespec _clog_fatal_deadline;

static __attribute__((noinline, cold)) size_t
_write_all_fatal_bound(clog_shared_t *sh, const char *data, size_t len);

/* The path of a pipe sink and of a socket sink, and of any sink carrying
 * CLOG_SINK_FATAL_BOUND. The test of that flag sits behind the pipe test, so
 * a pipe pays nothing for it and a socket pays one compare. */
static __attribute__((noinline)) size_t _write_all_nonplain(clog_shared_t *sh,
                                                            const char *data,
                                                            size_t len) {
  if (sh->sink_kind == CLOG_SINK_PIPE) {
    _clog_pipe_write_prepare();
#if defined(F_SETNOSIGPIPE)
    int nosig = _clog_pipe_nosigpipe_begin(sh->fd);
    size_t n = _write_all_body(sh->fd, CLOG_SINK_PIPE, data, len);
    _clog_pipe_nosigpipe_end(sh->fd, nosig);
    return n;
#else
    return _write_all_body(sh->fd, CLOG_SINK_PIPE, data, len);
#endif
  }
  if (__builtin_expect(sh->sink_kind != CLOG_SINK_SOCKET, 0))
    return _write_all_fatal_bound(sh, data, len);
  return _write_all_body(sh->fd, CLOG_SINK_SOCKET, data, len);
}

/* Milliseconds from now until deadline, rounded up, and 0 once it has passed.
 * Rounding up keeps a wait from ending before the deadline. */
static int _clog_ms_until(const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long long ns = (long long)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
                 (long long)(deadline->tv_nsec - now.tv_nsec);
  if (ns <= 0) return 0;
  long long ms = (ns + 999999LL) / 1000000LL;
  return ms > INT_MAX ? INT_MAX : (int)ms;
}

/*
 * The write of a recovery step: the same contract as _write_all_body(), but
 * the whole write ends within timeout_ms. poll(2) alone doesn't give that
 * bound, because it bounds only a wait after EAGAIN, and a descriptor in
 * blocking mode never reports EAGAIN: a write(2) bigger than the room a pipe
 * has left blocks until a reader makes room, however long that takes. So no
 * call here may block past the deadline:
 *
 *   - A socket is written with ccol_send_nb(), which never blocks whatever
 *     the descriptor's mode (MSG_DONTWAIT, or on macOS a send limited to the
 *     room poll(2) reports), and a poll with the time left waits for room.
 *   - Every other descriptor poll(2) reports on, a pipe or a FIFO above all,
 *     is polled for POLLOUT with the time left before each write, and each
 *     write gives at most PIPE_BUF bytes. A pipe that reports POLLOUT has
 *     room for PIPE_BUF bytes, so such a write returns at once; only another
 *     process writing into the same pipe between the poll and the write can
 *     take that room first.
 *   - A terminal is written through a second open file description of the
 *     same device, opened with O_NONBLOCK (see _clog_open_tty_nonblocking()).
 *     POLLOUT on a terminal only means some room is free, and a blocking
 *     write(2) to it waits until every byte fits, however small the write: a
 *     pseudo-terminal whose reader stopped, such as a frozen remote session,
 *     holds it for ever. A non-blocking write takes what fits and returns,
 *     and a poll with the time left waits for more. The flag lives on the new
 *     description only, so no other holder of the terminal (a shell, most of
 *     all) sees its descriptor turn non-blocking. Where that open fails, each
 *     write after POLLOUT gives at most CLOG_TTY_FALLBACK_CHUNK bytes, which a
 *     pseudo-terminal takes at once since it counts its room in blocks of 256
 *     bytes.
 *   - A regular file or a block device is exempt: poll(2) always reports it
 *     writable, and a write to a disk waits on the disk and never on a
 *     reader, so the ordinary loop writes it.
 *
 * If the deadline passes before everything went out, the write ends with
 * errno set to EAGAIN, which _clog_write_error_is_transient() treats as
 * something a later attempt can outlast. It's out of line and cold because
 * only recovery steps call it.
 */
#define CLOG_TTY_FALLBACK_CHUNK 128

/* Opens the terminal fd a second time for writing, with O_NONBLOCK, and
 * returns the new descriptor, or -1. It goes through /proc/self/fd where
 * that exists, and otherwise through the name ttyname_r(3) gives, which is
 * how FreeBSD and macOS reach it. A pseudo-terminal master is left alone,
 * because opening its path makes a new pseudo-terminal, or reaches the slave,
 * instead of giving a second description of the same device; the device
 * number of what got opened must match fd's, which rules that out. */
static int _clog_open_tty_nonblocking(int fd) {
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

static __attribute__((noinline, cold)) size_t _write_all_bounded(
    clog_shared_t *sh, const char *data, size_t len, int timeout_ms) {
  int fd = sh->fd;
  unsigned char raw_kind = sh->sink_kind;
  unsigned char kind = raw_kind & (unsigned char)~CLOG_SINK_FATAL_BOUND;
  /* The descriptor the loop writes, and the most one write after POLLOUT may
   * give it. own_fd is a second description of a terminal that this call
   * opens and closes. */
  int wfd = fd;
  int own_fd = -1;
  /* A message socket (a socket without MSG_NOSIGNAL takes the pipe path)
   * delivers each write as one message, so a record has to go out in one
   * write; cutting it would split it into several messages. */
  size_t chunk = sh->sink_per_record ? SIZE_MAX : PIPE_BUF;
  if (kind == CLOG_SINK_PLAIN) {
    struct stat st;
    if (fstat(fd, &st) == 0) {
      if (raw_kind == CLOG_SINK_PLAIN &&
          (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)))
        return _write_all_plain(fd, data, len);
      if (S_ISCHR(st.st_mode) && isatty(fd)) {
        own_fd = _clog_open_tty_nonblocking(fd);
        if (own_fd >= 0) {
          wfd = own_fd;
          chunk = SIZE_MAX;
        } else {
          chunk = CLOG_TTY_FALLBACK_CHUNK;
        }
      }
    }
  }
  int forced_err = _clog_test_consume_next_write_error();
  if (forced_err != 0) {
    if (own_fd >= 0) close(own_fd);
    errno = forced_err;
    return 0;
  }
  size_t forced_cap = _clog_test_consume_next_write_cap();
  if (forced_cap > 0 && forced_cap < len) len = forced_cap;
  int room_err;
  len = _clog_test_room_clip(len, &room_err);
  if (kind == CLOG_SINK_PIPE) _clog_pipe_write_prepare();
#if defined(F_SETNOSIGPIPE)
  int nosig = kind == CLOG_SINK_PIPE ? _clog_pipe_nosigpipe_begin(wfd) : -1;
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
          .fd = wfd, .events = _clog_pollout_events(wfd), .revents = 0};
      int pr = poll(&pfd, 1, _clog_ms_until(&deadline));
      if (pr < 0) {
        if (errno == EINTR) continue; /* the deadline stays where it is */
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
      /* POLLERR or POLLHUP without POLLOUT: the write below fails at once
       * with the descriptor's error, and the loop reports it. */
      w = write(wfd, data, len < chunk ? len : chunk);
    }
    if (w < 0) {
      if (errno == EINTR) continue;
      if (kind == CLOG_SINK_PIPE && errno == EPIPE) {
        _clog_consume_own_sigpipe();
        break; /* the reader is gone, which is unrecoverable */
      }
      if (kind == CLOG_SINK_SOCKET && errno == ENOBUFS) {
        int left = _clog_ms_until(&deadline);
        if (left > 0) {
          _clog_enobufs_backoff(&nobufs_step_us, left);
          continue;
        }
        errno = EAGAIN;
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
#if CLOG_HAS_MSG_NOSIGNAL
        if (kind == CLOG_SINK_SOCKET) {
          struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
          int pr = poll(&pfd, 1, _clog_ms_until(&deadline));
          if (pr > 0 && (pfd.revents & POLLOUT)) continue;
          if (pr < 0 && errno == EINTR) continue;
          errno = EAGAIN;
          break;
        }
#endif
        /* A non-blocking descriptor that took nothing although it reported
         * room. The poll at the top of the loop waits again, and the
         * deadline bounds the loop. */
        if (_clog_ms_until(&deadline) > 0) continue;
        errno = EAGAIN;
      }
      break;
    }
    if (w == 0) break; /* no progress, so don't spin */
    data += (size_t)w;
    len -= (size_t)w;
    total += (size_t)w;
  }
#if defined(F_SETNOSIGPIPE)
  _clog_pipe_nosigpipe_end(wfd, nosig);
#endif
  if (room_err != 0) errno = room_err;
  if (own_fd >= 0) {
    int saved = errno;
    close(own_fd);
    errno = saved;
  }
  return total;
}

/* The write to a sink carrying CLOG_SINK_FATAL_BOUND. */
static __attribute__((noinline, cold)) size_t
_write_all_fatal_bound(clog_shared_t *sh, const char *data, size_t len) {
  return _write_all_bounded(sh, data, len,
                            _clog_ms_until(&_clog_fatal_deadline));
}

/* Writes to sh->fd; see _write_all_body() above for the contract. The sink's
 * kind decides the write call and the signal handling (see "SIGPIPE AND
 * SIGXFSZ" above). The choice sits in the caller so each sink calls its own
 * loop directly, and a regular file pays one test of a byte on the same
 * cache line as sh->fd. A bounded write goes to _write_all_bounded(); every
 * caller on a record's path passes CLOG_WRITE_WAIT_FOREVER as a constant, so
 * that test folds away there. The caller must hold sh->mutex. */
static inline __attribute__((always_inline)) size_t _write_all(
    clog_shared_t *sh, const char *data, size_t len, int poll_timeout_ms) {
  if (poll_timeout_ms != CLOG_WRITE_WAIT_FOREVER)
    return _write_all_bounded(sh, data, len, poll_timeout_ms);
  if (__builtin_expect(sh->sink_kind != CLOG_SINK_PLAIN, 0))
    return _write_all_nonplain(sh, data, len);
  return _write_all_plain(sh->fd, data, len);
}

/*
 * Renders one record the library makes itself, at `level`, with `head`
 * followed by `detail` as its message; the loss markers, "log record
 * truncated: <detail>", are records of this kind. It uses the output format
 * `fmt` and puts the lead-in `nl` in front of the record. That lead-in is
 * "\n" when the bytes already on disk stop in the middle of a line and need
 * the missing newline, and "" otherwise; one write carries both, so nothing
 * can land between them. Returns the length of the text in `note`, which
 * always ends with a newline.
 *
 * `head` and `detail` are always text from this file and never something a
 * caller supplied, so they need no escape in any of the three formats.
 */
static size_t _clog_note_render(const clog_shared_t *sh, clog_format_t fmt,
                                clog_level_t level, const char *nl,
                                const char *head, const char *detail,
                                char *note, size_t cap) {
  int n;
  if (fmt == CLOG_FMT_JSON) {
    n = snprintf(note, cap, "%s{\"level\":\"%s\",\"msg\":\"%s%s\"}\n", nl,
                 _LEVEL_STR[level], head, detail);
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    n = snprintf(note, cap, "%s<%d>1 - - - - - - %s%s\n", nl, pri, head,
                 detail);
  } else {
    n = snprintf(note, cap, "%slevel=%s msg=\"%s%s\"\n", nl, _LEVEL_STR[level],
                 head, detail);
  }
  if (n <= 0) return 0;
  if ((size_t)n >= cap) {
    note[cap - 2] = '\n';
    return cap - 1;
  }
  return (size_t)n;
}

/* Room for a rendered note: this file's longest head and detail, the lead-in
 * and the framing of the longest format. */
#define CLOG_NOTE_CAP 320

/* The head of every loss marker. */
#define CLOG_LOSS_HEAD "log record truncated: "

/*
 * Writes what the target owes its output (see clog_shared_t.owe_newline):
 * the newline that closes a line left open on disk, plus one marker naming
 * every loss no marker has named yet. While one loss is owed the marker
 * carries that loss's text; for more than one it says how many there were,
 * the records and bytes they lost, and the last write error. Everything goes
 * out in one write. If the output takes all of it nothing is owed any more;
 * if it takes a part, the part it took is judged by its last byte, which
 * tells whether a line is still open.
 *
 * It writes nothing while the bytes on disk end inside a record whose
 * continuation the async batch still holds, since the repair would then
 * land between the two halves of that record.
 *
 * A write failing with an error no later attempt outlasts, such as EPIPE
 * from a pipe whose reader is gone, ends the debt instead: the output that
 * holds the fragment is gone, and every later attempt would fail the same
 * way (and on a pipe raise SIGPIPE again).
 *
 * Returns true when the output stands at the start of a line afterwards, so
 * a record may go out, and false while a newline is still owed.
 * poll_timeout_ms bounds the write like any other write (see _write_all()).
 * The caller must hold sh->mutex.
 */
static bool _clog_write_error_is_transient(int err);

static __attribute__((noinline, cold)) bool _clog_pay_owed(
    clog_shared_t *sh, clog_format_t fmt, int poll_timeout_ms) {
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
    nlen = _clog_note_render(sh, fmt, CLOG_ERROR, nl, CLOG_LOSS_HEAD,
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
    nlen = _clog_note_render(sh, fmt, CLOG_ERROR, nl, CLOG_LOSS_HEAD, detail,
                             note, sizeof(note));
  }
  if (nlen == 0) return !sh->owe_newline;
  errno = 0;
  size_t w = _write_all(sh, note, nlen, poll_timeout_ms);
  int err = errno;
  if (sh->rotation_enabled) sh->bytes_written += (off_t)w;
  if (w == nlen || !_clog_write_error_is_transient(err)) {
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

/*
 * Adds one loss to what the target owes its output: `records` records and
 * `bytes` bytes the output never got, the errno of the failed write (0 when
 * there is none), and `detail`, the text naming the loss when it's the only
 * one owed. open_line says the bytes on disk now end inside a line, which
 * the repair has to close first. It then tries the repair immediately with
 * the bound poll_timeout_ms, so an output that still takes bytes gets the
 * marker at the moment of the loss.
 *
 * A record this logger can't deliver is always reported, never silently
 * dropped (see _clog_write_unrepresentable_record()). An output that refuses
 * the marker too still gets it, in front of the first write it accepts
 * later, so the log names the loss once the output recovers.
 *
 * The caller must hold sh->mutex, and the split bits of async_partial_record
 * must be clear.
 */
static void _clog_exit_drain_arm(void);

static __attribute__((noinline, cold)) void _clog_report_loss(
    clog_shared_t *sh, clog_format_t fmt, bool open_line, size_t records,
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
  /* A debt that nothing writes again gets paid at exit, so the drain runs
   * even in a process that has only synchronous loggers. */
  _clog_exit_drain_arm();
  (void)_clog_pay_owed(sh, fmt, poll_timeout_ms);
}

/*
 * The test every record write makes when the target owes its output a
 * repair. It tries the repair first, and returns false when a newline is
 * still owed afterwards. The record must not go out then, since it would
 * continue the fragment on disk as one line; it counts as a loss instead,
 * and the repair's marker names it once the output recovers. The caller must
 * hold sh->mutex.
 */
static __attribute__((noinline, cold)) bool _clog_owed_gate(
    clog_shared_t *sh, clog_format_t fmt, size_t len, int poll_timeout_ms) {
  if (_clog_pay_owed(sh, fmt, poll_timeout_ms)) return true;
  _clog_report_loss(sh, fmt, false, 1, len, 0,
                    "a record was not written while a line stayed open",
                    poll_timeout_ms);
  return false;
}

/*
 * Writes one record the library makes itself, at `level`, with `head`
 * followed by `detail` as its message, such as a WARN record about the
 * rotation. Like every other record it waits for a repair the target owes,
 * and a short write of it is a loss like any other.
 *
 * poll_timeout_ms is the bound of the step writing it (see _write_all()).
 * The caller must hold sh->mutex.
 */
static void _clog_note_record(clog_shared_t *sh, clog_format_t fmt,
                              clog_level_t level, const char *head,
                              const char *detail, int poll_timeout_ms) {
  char note[CLOG_NOTE_CAP];
  size_t nlen =
      _clog_note_render(sh, fmt, level, "", head, detail, note, sizeof(note));
  if (nlen == 0) return;
  if ((sh->async_partial_record & CLOG_OWED_REPAIR) &&
      !_clog_owed_gate(sh, fmt, nlen, poll_timeout_ms))
    return;
  size_t w = _write_all(sh, note, nlen, poll_timeout_ms);
  int err = errno;
  if (sh->rotation_enabled) sh->bytes_written += (off_t)w;
  if (w < nlen) {
    char d[96];
    snprintf(d, sizeof(d), "%zu of %zu bytes written", w, nlen);
    _clog_report_loss(sh, fmt, w > 0 && note[w - 1] != '\n', 1, nlen - w, err,
                      d, poll_timeout_ms);
  }
}

/*
 * Restores the record framing and reports the loss after a write the kernel
 * took only in part, as happens with a filesystem full with ENOSPC, with an
 * RLIMIT_FSIZE ceiling, or with a peer that goes away in the middle of a
 * record. The accepted bytes are on disk without the record's terminating
 * newline, and without the repair the next record would go straight onto
 * them, so every consumer of the file would read the two as one line. The
 * repair writes that newline only when the accepted prefix doesn't already
 * end on a record boundary, and the target keeps owing it until the output
 * takes it (see _clog_report_loss()).
 *
 * The caller must hold sh->mutex.
 */
static void _clog_note_truncated_record(clog_shared_t *sh, clog_format_t fmt,
                                        const char *data, size_t written,
                                        size_t len, int err) {
  char detail[96];
  snprintf(detail, sizeof(detail), "%zu of %zu bytes written", written, len);
  _clog_report_loss(sh, fmt, written > 0 && data[written - 1] != '\n', 1,
                    len - written, err, detail, CLOG_WRITE_WAIT_FOREVER);
}

/*
 * Reports that the library gives up on the continuation of a record that is
 * already half written. When the half on disk left a line open
 * (CLOG_SPLIT_MID_LINE), the repair also closes that line, so the next
 * record lands on a line of its own and not inside another record. This
 * marker is written only for a split record, so `split` is never
 * CLOG_SPLIT_NONE here, and the caller has already cleared the split bits of
 * async_partial_record.
 *
 * The caller must hold sh->mutex.
 */
static void _clog_note_dropped_continuation(clog_shared_t *sh,
                                            clog_format_t fmt,
                                            unsigned char split, size_t dropped,
                                            int err, int poll_timeout_ms) {
  char detail[96];
  snprintf(detail, sizeof(detail), "%zu undelivered bytes dropped", dropped);
  _clog_report_loss(sh, fmt, split == CLOG_SPLIT_MID_LINE, 1, dropped, err,
                    detail, poll_timeout_ms);
}

/*
 * Reports that the library gives up on `len` undelivered bytes of the async
 * batch, starting at `data`. `split` says where the bytes already on disk
 * end, as a value of async_partial_record's split bits, which the caller has
 * already cleared. The marker names how many records the loss touches and
 * `why`, which is text from this file. A record counts once, whether all of
 * it or only its continuation is lost: every line that doesn't start with a
 * tab starts a record, and a continuation starting with a tab belongs to the
 * record the bytes on disk left unfinished.
 *
 * The caller must hold sh->mutex, and it discards the bytes itself.
 */
static __attribute__((noinline, cold)) void _clog_note_dropped_batch(
    clog_shared_t *sh, const char *data, size_t len, unsigned char split,
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
  _clog_report_loss(sh, sh->format, split == CLOG_SPLIT_MID_LINE, records, len,
                    err, detail, poll_timeout_ms);
}

/*
 * Writes one already framed record to sh->fd, every byte of it including the
 * terminating newline. It charges what the kernel really took to
 * sh->bytes_written, so size-based rotation never runs ahead of the file's
 * real size on disk, and it repairs the framing when the write stops short.
 * Every synchronous write of a whole record in this file goes through here,
 * so no single path can be the one that leaves half a record behind, and
 * none can write a record onto an earlier one's fragment while the target
 * still owes the newline that closes it. The caller must hold sh->mutex.
 *
 * It's kept out of line on purpose. There's one call per record, on a path
 * where the write(2) underneath dominates the cost, and its callers build
 * records and are large: if the compiler folded this function into them,
 * their code shape and register allocation would shift with every edit made
 * here.
 */
static __attribute__((noinline)) void _clog_write_record(clog_shared_t *sh,
                                                         clog_format_t fmt,
                                                         const char *data,
                                                         size_t len) {
  if (__builtin_expect(sh->async_partial_record & CLOG_OWED_REPAIR, 0) &&
      !_clog_owed_gate(sh, fmt, len, CLOG_WRITE_WAIT_FOREVER))
    return;
  size_t written = _write_all(sh, data, len, CLOG_WRITE_WAIT_FOREVER);
  if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  if (written < len)
    _clog_note_truncated_record(sh, fmt, data, written, len, errno);
}

/* ========================================================================== */
/*                         TIMESTAMP                                          */
/* ========================================================================== */

/* Renders a timestamp the library has already captured. It's a function of
 * its own so a record can be built with the timestamp of its SUBMISSION
 * rather than of its write, because the build can happen much later, on the
 * async writer thread for example. */
static int _buf_append_ts_at(clog_buf_t *b, const struct timeval *tv) {
  struct tm tm;
  gmtime_r(&tv->tv_sec, &tm);
  return _buf_appendf(b, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                      tm.tm_min, tm.tm_sec, (long)tv->tv_usec);
}

/* ========================================================================== */
/*                         PATH UTILITIES                                     */
/* ========================================================================== */

/* Flags of the directory descriptor a rotating logger keeps. The descriptor
 * only names the directory for the *at() calls, so O_PATH is enough where the
 * platform has it, and then it needs no read permission on the directory. A
 * listing opens "." relative to it; see _clog_opendir_at(). */
#if defined(O_PATH)
#define CLOG_DIR_OPEN_FLAGS (O_PATH | O_DIRECTORY | O_CLOEXEC)
#else
#define CLOG_DIR_OPEN_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif

/* Opens the directory part of path: "." for a path with no '/', and "/" for
 * a path right under the root. Returns the descriptor, or -1 with errno
 * set. */
static int _clog_open_log_dir(const char *path) {
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

/* Opens a listing of the directory dir_fd names. The listing gets its own
 * descriptor, because closedir() closes the descriptor it reads and dir_fd
 * has to stay open. Returns NULL on failure. */
static DIR *_clog_opendir_at(int dir_fd) {
  int fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return NULL;
  DIR *d = fdopendir(fd);
  if (!d) close(fd);
  return d;
}

/* Returns a pointer into path just after the last '/'. */
static const char *_path_base(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* True when the directory dir_fd names holds an entry called name, of any
 * type. It never follows a symbolic link, so a dangling link counts as an
 * entry. A name that can't be examined, one too long for the directory for
 * example, gives false; the step that then uses the name fails on the same
 * condition and never replaces an entry. */
static bool _clog_entry_exists(int dir_fd, const char *name) {
  struct stat st;
  return fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
}

/* Removes the entry name, relative to dir_fd, only while it still names the
 * file *st describes (same device and inode, examined without following a
 * link). A rotation and a compression remove only what they made or have
 * already checked, never an entry some other program put at that name in
 * the meantime. Returns true when the name doesn't hold that file
 * afterwards, and false when the removal failed. */
static bool _clog_unlink_if_same(int dir_fd, const char *name,
                                 const struct stat *st) {
  struct stat cur;
  if (fstatat(dir_fd, name, &cur, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT;
  if (cur.st_dev != st->st_dev || cur.st_ino != st->st_ino) return true;
  return unlinkat(dir_fd, name, 0) == 0 || errno == ENOENT;
}

/*
 * Which directory entries are generations of a rotating target. A name that
 * parses as one of the target's rotated names (see _rotated_name_parse()) is
 * a generation only when the entry, examined without following a link, is a
 * regular file, its owner is the process's effective user or the live
 * file's owner (a rotation gives every generation the live file's owner; see
 * _clog_copy_owner_and_mode()), and its stamp isn't above `limit`. Naming,
 * retention, compression and compression recovery act on generations only;
 * any other entry is never opened, compressed, renamed or deleted, and gets
 * a WARN record (see _clog_note_ignored_entry()). So another user who can
 * write to the directory can't make the logger write, read, archive or
 * delete a file through a link, block it on a FIFO, stop its naming with a
 * stamp nothing can sort above, or take its retention slots.
 */
typedef struct {
  uid_t euid;
  uid_t live_uid;
  char limit[CLOG_ROTATION_STAMP_DIGITS];
} clog_gen_filter_t;

static bool _clog_gen_uid_ok(uid_t uid, uid_t euid, uid_t live_uid) {
  return uid == euid || uid == live_uid;
}

/* Parses the 14 stamp digits d, YYYYMMDDHHMMSS in UTC. Returns false when the
 * digits name no representable time. */
static bool _clog_stamp_to_time(const char *d, time_t *out) {
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

/* Writes the 14 stamp digits of time t, in UTC, to out. A time time_t can't
 * hold, or one past the year 9999, gives the highest stamp, which nothing
 * sorts above. */
static void _clog_time_to_stamp(int64_t t,
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

/* ========================================================================== */
/*                        GZIP COMPRESSION (with zlib)                        */
/* ========================================================================== */

#define _GZ_BUF_SIZE 65536U

#ifdef RUNNING_UNIT_TESTS
/*
 * Test-only synchronization hooks, gone from every non-test build. They let
 * a test widen and watch one window deterministically: from the moment
 * _gzip_compress_file() creates its destination file on disk (when gzopen()
 * returns) until that function has finished writing the file. Inside that
 * exact window a concurrent rotation's _prune_rotated() pass must never be
 * able to delete that destination. The window is normally under a
 * millisecond, so with these hooks a test doesn't depend on scheduling luck
 * to land a racing deletion inside it.
 */
static _Atomic unsigned int _clog_test_pending_compress_delay_us = 0;
static _Atomic bool _clog_test_gz_dest_opened_state = false;

void _clog_test_set_pending_compress_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_gz_dest_opened_state, false);
  atomic_store(&_clog_test_pending_compress_delay_us, delay_us);
}

bool _clog_test_gz_dest_opened(void) {
  bool opened = atomic_load(&_clog_test_gz_dest_opened_state);
  return opened;
}

/*
 * A test gate. While it's held, every compression pauses right after
 * creating its destination until the test releases the gate. The pause has a
 * safety bound of CLOG_TEST_COMPRESS_HOLD_MAX_US, so a thread that shouldn't
 * be the one compressing, but is, fails its test instead of hanging it.
 * _clog_test_compressions_finished_state counts every compression that ran
 * to its end, whatever the outcome.
 */
#define CLOG_TEST_COMPRESS_HOLD_MAX_US 30000000U
static _Atomic bool _clog_test_compress_hold = false;
static _Atomic size_t _clog_test_compressions_finished_state = 0;

void _clog_test_hold_compressions(bool hold) {
  if (hold) atomic_store(&_clog_test_gz_dest_opened_state, false);
  atomic_store(&_clog_test_compress_hold, hold);
}

size_t _clog_test_compressions_finished(void) {
  return atomic_load(&_clog_test_compressions_finished_state);
}

/* How many upcoming compressions fail their publish and then also leave their
 * temporary output behind, as in a directory that lost its write permission
 * while they ran. */
static _Atomic unsigned int _clog_test_gz_publish_failures = 0;

void _clog_test_force_gz_publish_failures(unsigned int count) {
  atomic_store(&_clog_test_gz_publish_failures, count);
}

static bool _clog_test_peek_gz_publish_failure(void) {
  return atomic_load(&_clog_test_gz_publish_failures) > 0;
}

static bool _clog_test_consume_gz_publish_failure(void) {
  unsigned int n = atomic_load(&_clog_test_gz_publish_failures);
  while (n > 0) {
    if (atomic_compare_exchange_weak(&_clog_test_gz_publish_failures, &n,
                                     n - 1))
      return true;
  }
  return false;
}

/* A test gate like the one above, which pauses the recovery of unfinished
 * compressions before it lists the directory. */
#define CLOG_TEST_RECOVER_HOLD_MAX_US 3000000U
static _Atomic bool _clog_test_recover_hold = false;
static _Atomic bool _clog_test_recover_entered = false;

void _clog_test_hold_compression_recovery(bool hold) {
  if (hold) atomic_store(&_clog_test_recover_entered, false);
  atomic_store(&_clog_test_recover_hold, hold);
}

bool _clog_test_compression_recovery_entered(void) {
  return atomic_load(&_clog_test_recover_entered);
}

static void _clog_test_recover_gate(void) {
  if (!atomic_load(&_clog_test_recover_hold)) return;
  atomic_store(&_clog_test_recover_entered, true);
  for (unsigned int waited = 0; atomic_load(&_clog_test_recover_hold) &&
                                waited < CLOG_TEST_RECOVER_HOLD_MAX_US;
       waited += 1000)
    usleep(1000);
}

/* Timeouts of the timed wait of the async writer thread of the target behind
 * logger, or 0 for a handle that doesn't resolve. The count belongs to the
 * target, so the writer threads of other loggers in the process don't add to
 * it. */
size_t _clog_test_writer_timeout_wakeups(clog logger) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return 0;
  size_t n = atomic_load(&lg->shared->test_writer_timeout_wakeups);
  _clog_resolve_unpin(lg);
  return n;
}

/* Flushes the async writer thread of the target behind logger started
 * because flush_interval_us ran out, or 0 for a handle that doesn't
 * resolve. */
size_t _clog_test_writer_interval_flushes(clog logger) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return 0;
  size_t n = atomic_load(&lg->shared->test_writer_interval_flushes);
  _clog_resolve_unpin(lg);
  return n;
}

/*
 * Counts the real _rotate() calls, meaning every attempt that gets to
 * rename() and open(), not every write that would like to rotate. With it a
 * test can check the retry policy for a rotation that keeps failing: such a
 * rotation must be retried with a bounded backoff, not on every single
 * write.
 */
static _Atomic size_t _clog_test_rotate_attempt_count = 0;

void _clog_test_reset_rotate_attempt_count(void) {
  atomic_store(&_clog_test_rotate_attempt_count, 0);
}

size_t _clog_test_get_rotate_attempt_count(void) {
  return atomic_load(&_clog_test_rotate_attempt_count);
}

void _clog_test_sanitize_syslog_appname(const char *raw, char *out,
                                        size_t outsz) {
  _sanitize_syslog_printusascii_field(raw, out, outsz);
}

void _clog_test_append_ctrl_escaped(const char *raw, char *out, size_t outsz) {
  if (outsz == 0) return;
  clog_buf_t b;
  if (_buf_init(&b, NULL) != 0) {
    out[0] = '\0';
    return;
  }
  if (_buf_append_ctrl_escaped(&b, raw) != 0) {
    out[0] = '\0';
    _buf_free(&b);
    return;
  }
  size_t n = b.len < outsz - 1 ? b.len : outsz - 1;
  memcpy(out, b.data, n);
  out[n] = '\0';
  _buf_free(&b);
}

/*
 * Lets a test force _capture_backtrace() to report a capture failure
 * deterministically, with the same result as a platform without backtrace
 * support or a backtrace_symbols() call whose own allocation fails. The test
 * then doesn't depend on either real condition, one of which belongs to the
 * platform and the other to a real out-of-memory state.
 */
static _Atomic bool _clog_test_force_bt_capture_failure = false;

void _clog_test_force_backtrace_capture_failure(bool force) {
  atomic_store(&_clog_test_force_bt_capture_failure, force);
}

/*
 * Lets a test force EVERY frame of a real backtrace (one whose syms isn't
 * NULL) to fail to append inside _emit_backtrace_syslog_lines(), without
 * relying on a real allocation failure that lasts across every small
 * per-frame append the function makes. The library's buffer-size constants
 * make that condition impractical to reproduce for real, because each frame
 * gets a scratch buffer that has just been reset and is already big enough.
 * Unlike _clog_test_force_next_buf_ensure_failure(), which fires once and
 * disarms itself, this hook stays armed across every frame of one call until
 * a caller disarms it, the same shape as
 * _clog_test_force_backtrace_capture_failure().
 */
static _Atomic bool _clog_test_force_all_syslog_bt_frames_fail = false;

void _clog_test_force_all_syslog_backtrace_frames_failure(bool force) {
  atomic_store(&_clog_test_force_all_syslog_bt_frames_fail, force);
}

/*
 * Lets a test force _capture_backtrace() to report, deterministically, a
 * capture that really succeeds: syms isn't NULL and comes from a real
 * backtrace(), and the reported depth is clamped to CLOG_BT_INITIAL_FRAME.
 * That exercises the path where the capture succeeded but found no frame
 * beyond the two internal bookkeeping ones, which the emitters have to treat
 * as a real, accurate, empty backtrace and not as a failed capture. With the
 * hook the test doesn't need a real call stack shallow enough to get there.
 */
static _Atomic bool _clog_test_force_shallow_bt_depth = false;

void _clog_test_force_shallow_backtrace_depth(bool force) {
  atomic_store(&_clog_test_force_shallow_bt_depth, force);
}

bool _clog_test_backtrace_supported(void) { return CLOG_HAS_BACKTRACE != 0; }
#endif

/*
 * Gives the file behind fd, which this module has just created with mode
 * 0600, the permission bits, owner and group that *like describes. Owner and
 * group come first and the bits are widened only after them, so the file is
 * never readable by more than *like allows. Owner and group are best effort,
 * the way logrotate does them: a process that isn't allowed to give the file
 * the owner keeps its own and still tries the group, and one that can't give
 * it the group either keeps its own group and clears the group bits, so the
 * process's group doesn't gain a read that *like's group had. A failed
 * fchmod() leaves the 0600 from creation. A rotation's new live file and a
 * compressed generation both go through here, so every generation of a log
 * has the same access.
 */
static void _clog_copy_owner_and_mode(int fd, const struct stat *like) {
  mode_t mode = like->st_mode & 0777;
  struct stat cur;
  if (fstat(fd, &cur) == 0 &&
      (cur.st_uid != like->st_uid || cur.st_gid != like->st_gid)) {
    if (fchown(fd, like->st_uid, like->st_gid) != 0 &&
        fchown(fd, (uid_t)-1, like->st_gid) != 0)
      mode &= ~(mode_t)0070;
  }
  if (fchmod(fd, mode) != 0) {
    /* The file keeps the 0600 it was created with. */
  }
}

#ifdef RUNNING_UNIT_TESTS
/* The errno the next fsync() of a compressed output reports instead of the
 * real call, for a test; 0 lets the real call run. It disarms itself when it
 * fires. */
static _Atomic int _clog_test_next_fsync_error = 0;

void _clog_test_force_next_fsync_error(int err) {
  atomic_store(&_clog_test_next_fsync_error, err);
}
#endif

/* Brings the data of the compressed output fd to the disk. Returns 0 when
 * the data is there, or when the filesystem offers no way to ask for it:
 * fsync(2) fails with EINVAL or ENOSYS on a filesystem that doesn't
 * implement it, and a compression that failed on that answer would leave
 * every rotated file uncompressed for ever on such a filesystem. Any other
 * failure, EIO above all, gives -1. */
static int _clog_sync_output(int fd) {
  int rc;
#ifdef RUNNING_UNIT_TESTS
  int forced = atomic_exchange(&_clog_test_next_fsync_error, 0);
  if (forced != 0) {
    errno = forced;
    rc = -1;
  } else
#endif
    rc = fsync(fd);
  if (rc == 0 || errno == EINVAL || errno == ENOSYS) return 0;
  return -1;
}

/* Suffix of the name a compression writes its output under until the output
 * is complete. "<rotated>.gz.tmp" never parses as a rotated name (see
 * _rotated_name_parse()), so neither a deletion pass nor the choice of a new
 * rotated name ever sees it. */
#define CLOG_GZ_TMP_SUFFIX ".tmp"

/* Publishes the finished temporary output tmp under the name dst, both
 * relative to dir_fd. It never replaces a file already sitting at dst.
 * Returns 0 on success. */
static int _clog_publish_no_replace(int dir_fd, const char *tmp,
                                    const char *dst) {
#if defined(RENAME_NOREPLACE) && defined(__linux__)
  if (renameat2(dir_fd, tmp, dir_fd, dst, RENAME_NOREPLACE) == 0) return 0;
  if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
  /* A hard link fails with EEXIST when dst exists, in one step, like the
   * rename above. It's the fallback for a filesystem that doesn't know
   * RENAME_NOREPLACE. */
  if (linkat(dir_fd, tmp, dir_fd, dst, 0) == 0) {
    unlinkat(dir_fd, tmp, 0);
    return 0;
  }
  if (errno == EEXIST) return -1;
  /* A filesystem without hard links. Here the check and the rename are two
   * steps, and only another program creating dst in between can lose its
   * file. */
  if (_clog_entry_exists(dir_fd, dst)) {
    errno = EEXIST;
    return -1;
  }
  return renameat(dir_fd, tmp, dir_fd, dst);
}

/*
 * Gzip-compresses the file src into dst, which is src plus ".gz", with zlib;
 * all names are relative to dir_fd. The output goes to dst +
 * CLOG_GZ_TMP_SUFFIX first, and only a complete, flushed gzip stream is
 * renamed to dst, so a process that stops in the middle of a compression
 * leaves at most that temporary file and never a truncated dst; the log's
 * next open removes it and compresses the source again (see
 * _clog_recover_compressions()). The output gets src's permission bits,
 * owner and group, the way gzip(1) gives them (see
 * _clog_copy_owner_and_mode()).
 *
 * On success it returns 0, dst is a valid gzip file and src is unlinked. On
 * failure it returns -1, removes its temporary output and leaves src
 * untouched. When that removal fails too, as in a directory that lost its
 * write permission, a non-NULL tmp_left gets the device and inode of the
 * temporary output so the caller can remove it later, and its st_ino is 0
 * otherwise. It never replaces a file already sitting at dst. A non-NULL
 * abort_flag that turns true stops the compression at its next block, which
 * counts as a failure.
 *
 * It reads src only when the descriptor it opens is a regular file owned by
 * the effective user or by live_uid (see clog_gen_filter_t). The open never
 * follows a link and never waits for a FIFO or a device, and type and owner
 * are checked on the descriptor itself, so an entry another program put at
 * the name after it was listed is never read.
 */
static int _gzip_compress_file(int dir_fd, const char *src, const char *dst,
                               _Atomic bool *abort_flag, uid_t live_uid,
                               struct stat *tmp_left) {
  FILE *in = NULL;
  gzFile out = NULL;
  int out_fd = -1;
  int rc = -1;
  bool tmp_created = false; /* tmp is unlinked below only if this call
      really created it */
  struct stat st;           /* the source, as opened */
  struct stat tmp_st;       /* the temporary output, once tmp_created */

  /* The caller reads *tmp_left on every return, including the early one. */
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
      !_clog_gen_uid_ok(st.st_uid, geteuid(), live_uid) ||
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

  /* Never overwrite something already sitting at dst: an unrelated file or a
   * stale leftover can occupy this exact gzip destination path. The publish
   * step refuses it as well; this check only saves the work. */
  if (_clog_entry_exists(dir_fd, dst)) goto done;

  /* The temporary name belongs to this compression alone. A regular file
   * there owned by the effective user or live_uid is the leftover of a
   * compression that stopped partway, and it holds nothing that isn't also
   * in src. Anything else at that name is left alone, and the source stays
   * uncompressed. O_EXCL makes checking and creating one step, and it never
   * follows a link. */
  out_fd = openat(dir_fd, tmp,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (out_fd < 0 && errno == EEXIST) {
    struct stat old_tmp;
    if (fstatat(dir_fd, tmp, &old_tmp, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(old_tmp.st_mode) &&
        _clog_gen_uid_ok(old_tmp.st_uid, geteuid(), live_uid) &&
        unlinkat(dir_fd, tmp, 0) == 0)
      out_fd =
          openat(dir_fd, tmp,
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  }
  if (out_fd < 0) goto done;
  if (fstat(out_fd, &tmp_st) != 0) {
    /* The file exists but can't be examined, so it couldn't be told apart
     * from another entry at the same name later, and it goes right away. */
    (void)unlinkat(dir_fd, tmp, 0);
    goto done;
  }
  tmp_created = true;
  /* The source's access, which the umask doesn't narrow a second time. */
  _clog_copy_owner_and_mode(out_fd, &st);
  out = gzdopen(out_fd, "wb");
  if (!out) goto done;

#ifdef RUNNING_UNIT_TESTS
  {
    /* Copy the armed delay into a local before announcing that the output
     * file exists. A test can spin-wait on _clog_test_gz_dest_opened() and,
     * the instant it sees this compression, arm the delay again or clear it
     * for a DIFFERENT, later compression, and that must never race the read
     * this call already has in progress. */
    unsigned int delay = atomic_load(&_clog_test_pending_compress_delay_us);
    atomic_store(&_clog_test_gz_dest_opened_state, true);
    if (delay) usleep(delay);
    for (unsigned int waited = 0; atomic_load(&_clog_test_compress_hold) &&
                                  !(abort_flag && atomic_load(abort_flag)) &&
                                  waited < CLOG_TEST_COMPRESS_HOLD_MAX_US;
         waited += 1000)
      usleep(1000);
  }
#endif

  unsigned char buf[_GZ_BUF_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (abort_flag && atomic_load_explicit(abort_flag, memory_order_relaxed))
      goto done;
    if (gzwrite(out, buf, (unsigned)n) != (int)n) goto done;
  }
  if (ferror(in)) goto done;

  /* The trailer goes out and the data reaches the disk before the output
   * takes its final name, so a crash after the rename never finds a short
   * dst next to a deleted src. */
  if (gzflush(out, Z_FINISH) != Z_OK || _clog_sync_output(out_fd) != 0)
    goto done;
  int close_rc = gzclose(out);
  out = NULL;
  out_fd = -1;
  if (close_rc != Z_OK) goto done;
#ifdef RUNNING_UNIT_TESTS
  if (_clog_test_peek_gz_publish_failure()) goto done;
#endif
  if (_clog_publish_no_replace(dir_fd, tmp, dst) != 0) goto done;
  tmp_created = false;
  rc = 0;

done:
  if (in) fclose(in);
  if (out)
    gzclose(out);
  else if (out_fd >= 0)
    close(out_fd);
  /* Remove the unfinished output, and on success the uncompressed original,
   * each only while the name still holds the file this call made or read. */
  if (tmp_left) tmp_left->st_ino = 0;
  if (tmp_created) {
    bool removed;
#ifdef RUNNING_UNIT_TESTS
    if (_clog_test_consume_gz_publish_failure())
      removed = false;
    else
#endif
      removed = _clog_unlink_if_same(dir_fd, tmp, &tmp_st);
    if (!removed && tmp_left) *tmp_left = tmp_st;
  }
  if (rc == 0) (void)_clog_unlink_if_same(dir_fd, src, &st);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&_clog_test_compressions_finished_state, 1);
#endif
  return rc;
}

#ifdef RUNNING_UNIT_TESTS
bool _clog_test_gzip_compress_file(const char *src, const char *dst) {
  return _gzip_compress_file(AT_FDCWD, src, dst, NULL, geteuid(), NULL) == 0;
}

bool _clog_test_gzip_compress_file_for_owner(const char *src, const char *dst,
                                             uid_t live_uid) {
  return _gzip_compress_file(AT_FDCWD, src, dst, NULL, live_uid, NULL) == 0;
}
#endif

/* ========================================================================== */
/*                         LOG ROTATION                                       */
/* ========================================================================== */

/*
 * Parses the part of a candidate rotated-file name after "<base>.". A name
 * this logger made has three parts: the mandatory 14-digit YYYYMMDDHHMMSS
 * stamp that CLOG_ROTATION_FMT produces, an optional collision suffix
 * "_<seq>" whose number is zero-padded to 4 digits (a number above 9999
 * takes as many digits as it needs, up to CLOG_ROTATION_SEQ_MAX_DIGITS, with
 * no leading zero), and an optional ".gz". A name without a suffix has
 * sequence 0, and _rotate() never writes a suffix of 0.
 *
 * For a name of exactly that shape it returns true, points *stamp at the 14
 * stamp digits and stores the sequence number in *seq. Anything else (extra
 * digits, an unrelated word at the end) only happens to start with something
 * that looks like <base>.<14 digits>, and it returns false, since the library
 * must never treat such a name as a file it owns. This check is what keeps
 * _prune_rotated() from calling unlink() on an unrelated file a user or
 * another tool put in the same directory.
 */
static bool _rotated_name_parse(const char *rest, const char **stamp,
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

/* Matches one directory entry against "<base>.<rotated name>" and parses it
 * with _rotated_name_parse(). */
static bool _rotated_entry_match(const char *d_name, const char *base,
                                 size_t base_len, const char **stamp,
                                 unsigned long *seq) {
  if (strncmp(d_name, base, base_len) != 0 || d_name[base_len] != '.')
    return false;
  return _rotated_name_parse(d_name + base_len + 1, stamp, seq);
}

/* Orders two rotated names by the pair (stamp, sequence number). */
static int _rotated_key_cmp(const char *stamp_a, unsigned long seq_a,
                            const char *stamp_b, unsigned long seq_b) {
  int c = memcmp(stamp_a, stamp_b, CLOG_ROTATION_STAMP_DIGITS);
  if (c != 0) return c;
  return (seq_a > seq_b) - (seq_a < seq_b);
}

/*
 * Fills *f for one pass over sh's directory. The caller holds sh->mutex, or
 * owns sh alone. A stamp's limit is CLOG_ROTATION_FUTURE_TOLERANCE_SECS
 * above the later of the clock and the newest name sh made (rot_last_stamp).
 * Every name sh makes is at or below rot_last_stamp, so a clock that steps
 * back by any amount never turns one of sh's generations into an entry to
 * ignore, and names keep sorting in rotation order.
 */
static void _clog_gen_filter_init(const clog_shared_t *sh,
                                  clog_gen_filter_t *f) {
  f->euid = geteuid();
  struct stat st;
  f->live_uid = (sh->fd >= 0 && fstat(sh->fd, &st) == 0) ? st.st_uid : f->euid;
  int64_t ref = (int64_t)_clog_wall_now();
  time_t last;
  if (sh->rot_name_known && _clog_stamp_to_time(sh->rot_last_stamp, &last) &&
      (int64_t)last > ref)
    ref = (int64_t)last;
  _clog_time_to_stamp(ref + CLOG_ROTATION_FUTURE_TOLERANCE_SECS, f->limit);
}

/* What one rotated-looking directory entry is; see clog_gen_filter_t. */
typedef enum {
  CLOG_ENTRY_GENERATION, /* a generation of the target */
  CLOG_ENTRY_IGNORED,    /* anything else that is there */
  CLOG_ENTRY_GONE        /* the entry is gone */
} clog_entry_kind_t;

/* Classifies the entry name, relative to dir_fd, whose stamp digits sit at
 * stamp. It never follows a link and never opens the entry. */
static clog_entry_kind_t _clog_entry_classify(int dir_fd, const char *name,
                                              const char *stamp,
                                              const clog_gen_filter_t *f) {
  struct stat st;
  if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? CLOG_ENTRY_GONE : CLOG_ENTRY_IGNORED;
  if (!S_ISREG(st.st_mode) ||
      !_clog_gen_uid_ok(st.st_uid, f->euid, f->live_uid))
    return CLOG_ENTRY_IGNORED;
  if (memcmp(stamp, f->limit, CLOG_ROTATION_STAMP_DIGITS) > 0)
    return CLOG_ENTRY_IGNORED;
  return CLOG_ENTRY_GENERATION;
}

/*
 * Writes the WARN record naming a directory entry that looks like one of
 * sh's rotated files but isn't one of its generations. The caller holds
 * sh->mutex, or owns sh alone, and no record of sh is half written. At most
 * CLOG_IGNORED_NOTE_BURST such records go out in any window of
 * CLOG_IGNORED_NOTE_WINDOW_SECS, so a directory full of such entries can't
 * flood the log. The name is a directory entry anybody who can write to the
 * directory gets to choose, so every byte outside printable ASCII, and every
 * quote and backslash, becomes '?', and a long name is cut, which keeps the
 * record one well-formed line in every format.
 */
static void _clog_note_ignored_entry(clog_shared_t *sh, const char *name) {
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
  _clog_note_record(sh, sh->format, CLOG_WARN,
                    "log rotation: ignored an entry that is not a rotated "
                    "file of this log: ",
                    safe, CLOG_WRITE_WAIT_FOREVER);
}

/* Classifies one rotated-looking entry the way _clog_entry_classify() does,
 * and notes an ignored one when may_note is true. */
static clog_entry_kind_t _clog_entry_check(clog_shared_t *sh, const char *name,
                                           const char *stamp,
                                           const clog_gen_filter_t *f,
                                           bool may_note) {
  clog_entry_kind_t k = _clog_entry_classify(sh->dir_fd, name, stamp, f);
  if (k == CLOG_ENTRY_IGNORED && may_note) _clog_note_ignored_entry(sh, name);
  return k;
}

/*
 * Finds sh's newest generation already on disk (see clog_gen_filter_t), and
 * when there is one returns true and fills stamp_out and *seq_out. The
 * caller holds sh->mutex, or owns sh alone, and sh->rot_name_known is false.
 * _rotate() calls it once, before a shared target's first rotation, so a
 * logger that starts in a directory already holding rotated files numbers
 * its own rotations above them; a process restarting within the same second
 * is the common case. An entry that isn't a generation plays no part in the
 * choice, and gets a WARN record when may_note is true.
 */
static bool _rotated_newest_on_disk(clog_shared_t *sh, bool may_note,
                                    char stamp_out[CLOG_ROTATION_STAMP_DIGITS],
                                    unsigned long *seq_out) {
  const char *base = sh->file_base;
  size_t base_len = strlen(base);
  clog_gen_filter_t f;
  _clog_gen_filter_init(sh, &f);

  DIR *d = _clog_opendir_at(sh->dir_fd);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const char *stamp;
    unsigned long seq;
    if (!_rotated_entry_match(e->d_name, base, base_len, &stamp, &seq))
      continue;
    if (_clog_entry_check(sh, e->d_name, stamp, &f, may_note) !=
        CLOG_ENTRY_GENERATION)
      continue;
    if (!found || _rotated_key_cmp(stamp, seq, stamp_out, *seq_out) > 0) {
      memcpy(stamp_out, stamp, CLOG_ROTATION_STAMP_DIGITS);
      *seq_out = seq;
      found = true;
    }
  }
  closedir(d);
  return found;
}

/* One rotated file _prune_rotated() found on disk. stamp points into
 * path. */
typedef struct {
  char *path;
  const char *stamp;
  unsigned long seq;
  bool is_protected;
} clog_rotated_file_t;

/*
 * Orders the rotated files oldest first, by the (stamp, sequence number)
 * pair of the name _rotate() gave each one. _rotate() gives every rotation
 * of a shared target a pair above every pair it gave before and every pair
 * that was on disk before its first rotation (see there), so the order of the
 * pairs is the order the rotations happened in. The sequence number compares
 * as a number, so "_10000" is newer than "_9999".
 *
 * The modification time can't order these files. The compression step
 * writes a rotated file it compresses, so such a file's mtime is when that
 * compression FINISHED, not how old the generation inside it is. A
 * compression runs with the shared target's mutex unlocked, so a later
 * generation whose compression finishes fast carries an older mtime than an
 * earlier one whose compression is still working through a big file, and
 * ordering by mtime would then delete the newest generation and keep the
 * stalest.
 */
static int _cmp_rotated_oldest_first(const void *a, const void *b) {
  const clog_rotated_file_t *x = (const clog_rotated_file_t *)a;
  const clog_rotated_file_t *y = (const clog_rotated_file_t *)b;
  int c = _rotated_key_cmp(x->stamp, x->seq, y->stamp, y->seq);
  if (c != 0) return c;
  /* The uncompressed file and the ".gz" file of one generation end up next
   * to each other either way; this only makes the order total. */
  return strcmp(x->path, y->path);
}

#ifdef RUNNING_UNIT_TESTS
/* Since the last reset: the most rotated generations a deletion pass left on
 * disk, the most compressions in flight a pass saw, and the most generations
 * a pass left beyond the ones it kept only because they were older than the
 * newest max_keep and still under compression. The on-disk count comes from
 * a directory listing of its own. */
static _Atomic int _clog_test_max_rotated_generations = 0;
static _Atomic int _clog_test_max_compressions_in_flight = 0;
static _Atomic int _clog_test_max_generations_beyond_old_compressions = 0;

/* Counts base's distinct rotated generations on disk from a directory
 * listing of its own, so the count doesn't depend on what the pass itself
 * listed. */
static int _clog_test_count_generations_on_disk(int dir_fd, const char *base) {
  size_t base_len = strlen(base);
  DIR *d = _clog_opendir_at(dir_fd);
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
    if (!_rotated_entry_match(e->d_name, base, base_len, &stamp, &seq))
      continue;
    bool dup = false;
    for (int i = 0; i < n && !dup; i++)
      dup = _rotated_key_cmp(seen[i].stamp, seen[i].seq, stamp, seq) == 0;
    if (dup || n == (int)(sizeof(seen) / sizeof(seen[0]))) continue;
    memcpy(seen[n].stamp, stamp, CLOG_ROTATION_STAMP_DIGITS);
    seen[n].seq = seq;
    n++;
  }
  closedir(d);
  return n;
}

static void _clog_test_raise_to(_Atomic int *slot, int value) {
  int prev = atomic_load(slot);
  while (value > prev && !atomic_compare_exchange_weak(slot, &prev, value)) {
  }
}

void _clog_test_reset_max_rotated_generations(void) {
  atomic_store(&_clog_test_max_rotated_generations, 0);
  atomic_store(&_clog_test_max_compressions_in_flight, 0);
  atomic_store(&_clog_test_max_generations_beyond_old_compressions, 0);
}

int _clog_test_get_max_rotated_generations(void) {
  return atomic_load(&_clog_test_max_rotated_generations);
}

int _clog_test_get_max_compressions_in_flight(void) {
  return atomic_load(&_clog_test_max_compressions_in_flight);
}

int _clog_test_get_max_generations_beyond_old_compressions(void) {
  return atomic_load(&_clog_test_max_generations_beyond_old_compressions);
}
#endif

/*
 * Deletes sh's oldest rotated generations beyond
 * sh->rotation.max_rotated_files. The caller holds sh->mutex. A generation
 * is one rotation, which on disk is one file, or two while its compression
 * runs (the uncompressed source and the ".gz" destination), and the pass
 * counts and deletes whole generations. Every name here is relative to
 * sh->dir_fd.
 *
 * The pass never deletes a file of sh->compress_running, the generation the
 * compressor thread is compressing right now, because the compression may
 * not have read all of the source or written all of the destination yet, and
 * deleting either would lose all of that generation's log data. A generation
 * whose job is still waiting in the queue gets no such protection; see
 * clog_compress_job_t.
 *
 * `just_rotated` is the destination the calling _rotate() has just created,
 * or NULL when there is none. The pass never deletes it, or its ".gz" form,
 * because that file holds the log content this very rotation moved aside,
 * and nothing has read it yet.
 *
 * Every generation counts against max_keep, protected or not. The pass keeps
 * the newest max_keep generations and deletes every older one that isn't
 * protected, while a protected generation older than the newest max_keep
 * stays until a later pass. `just_rotated` is always the newest generation,
 * since _rotate() names it above all the others, so it's always among the
 * newest max_keep, and only the running compression can stay older than the
 * newest max_keep. So a pass leaves exactly the newest max_keep generations,
 * or all of them when there are fewer, plus at most that one. A rotation
 * never waits for a compression to finish; a pass that keeps the running
 * compression this way marks the target instead, and the compressor thread
 * runs the pass again as soon as that compression ends, so the old
 * generation doesn't outlive it.
 *
 * A pass that can't list every rotated file because an allocation fails
 * deletes nothing, since a partial list can miss the newest generations and
 * the pass would then delete generations the full list keeps. Keeping a few
 * extra files until the next pass is a much smaller harm than deleting the
 * wrong ones.
 *
 * Only generations count and only generations get deleted (see
 * clog_gen_filter_t). An entry that isn't one gets a WARN record when
 * may_note is true; the pass the compressor thread runs passes false,
 * because a record of the target can be half written while it runs.
 */
static void _prune_rotated(clog_shared_t *sh, const char *just_rotated,
                           bool may_note) {
  int max_keep = sh->rotation.max_rotated_files;
  if (max_keep <= 0) return;

  const ccol_memmgmt_procs_t *m_procs = sh->m_procs;
  const clog_compress_job_t *running = sh->compress_running;
  const char *base = sh->file_base;
  size_t base_len = strlen(base);

  clog_gen_filter_t f;
  _clog_gen_filter_init(sh, &f);
  DIR *d = _clog_opendir_at(sh->dir_fd);
  if (!d) return;

  clog_rotated_file_t *files = NULL;
  int fc = 0, cap = 0;
  bool complete = true;
  size_t just_rotated_len = just_rotated ? strlen(just_rotated) : 0;
  struct dirent *e;

  while ((e = readdir(d)) != NULL) {
    const char *n = e->d_name;
    const char *name_stamp;
    unsigned long seq;
    if (!_rotated_entry_match(n, base, base_len, &name_stamp, &seq)) continue;
    if (_clog_entry_check(sh, n, name_stamp, &f, may_note) !=
        CLOG_ENTRY_GENERATION)
      continue;

    size_t n_len = strlen(n);
    char *name = _ccol_mem_alloc(m_procs, n_len + 1);
    if (!name) {
      complete = false;
      break;
    }
    memcpy(name, n, n_len + 1); /* + NUL */

    bool is_protected = running && (strcmp(running->src, name) == 0 ||
                                    strcmp(running->dst, name) == 0);
    if (just_rotated && (strcmp(name, just_rotated) == 0 ||
                         (strncmp(name, just_rotated, just_rotated_len) == 0 &&
                          strcmp(name + just_rotated_len, ".gz") == 0)))
      is_protected = true;

    if (fc >= cap) {
      int nc = cap ? cap * 2 : 16;
      clog_rotated_file_t *nf =
          _ccol_mem_realloc(m_procs, files, (size_t)nc * sizeof(*files));
      if (!nf) {
        _ccol_mem_free(m_procs, name);
        complete = false;
        break;
      }
      files = nf;
      cap = nc;
    }
    files[fc].path = name;
    /* The stamp sits at the same offset in the copy as in the entry. */
    files[fc].stamp = name + (size_t)(name_stamp - n);
    files[fc].seq = seq;
    files[fc].is_protected = is_protected;
    fc++;
  }
  closedir(d);

  if (complete) {
    if (fc > 1)
      qsort(files, (size_t)fc, sizeof(*files), _cmp_rotated_oldest_first);

    /* Count the generations. The files of one generation share a
     * (stamp, sequence number) pair, and the sort put them side by side. */
    int generations = 0;
    for (int i = 0; i < fc; i++)
      if (i == 0 || _rotated_key_cmp(files[i].stamp, files[i].seq,
                                     files[i - 1].stamp, files[i - 1].seq) != 0)
        generations++;

    /* Walk the generations oldest first, and delete every one older than the
     * newest max_keep unless one of its files is protected. */
    int delete_below = generations - max_keep;
    int g = 0;
#ifdef RUNNING_UNIT_TESTS
    int old_under_compression = 0;
#endif
    for (int start = 0; start < fc && g < delete_below; g++) {
      int end = start + 1;
      while (end < fc &&
             _rotated_key_cmp(files[end].stamp, files[end].seq,
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
    /* Every compression handed to the compressor thread and not finished
     * yet, the running one plus the queued ones. */
    int in_flight = running ? 1 : 0;
    for (const clog_compress_job_t *j = sh->compress_head; j; j = j->next)
      in_flight++;
    int on_disk = _clog_test_count_generations_on_disk(sh->dir_fd, base);
    _clog_test_raise_to(&_clog_test_max_rotated_generations, on_disk);
    _clog_test_raise_to(&_clog_test_max_compressions_in_flight, in_flight);
    _clog_test_raise_to(&_clog_test_max_generations_beyond_old_compressions,
                        on_disk - old_under_compression);
#endif
  }

  for (int i = 0; i < fc; i++) _ccol_mem_free(m_procs, files[i].path);
  _ccol_mem_free(m_procs, files);
}

/*
 * Returns true when `candidate` can't be used as a fresh name for a rotation
 * destination, which is the case when the name itself already exists on
 * disk, and when its compressed ".gz" form already exists while check_gz is
 * true because compress_rotated is on. The second check matters because
 * _gzip_compress_file() unlinks its uncompressed source the instant the
 * compression succeeds. When a LATER rotation within the same second runs
 * its own collision check, a bare existence check of candidate would make
 * the earlier rotation's name look free again, since that rotation
 * compressed and deleted it, while "<candidate>.gz" sits on disk holding
 * that earlier rotation's real content. Reusing the name would hand the
 * earlier rotation's ".gz" destination straight to a second, unrelated
 * rotation.
 */
static bool _rotated_name_taken(int dir_fd, const char *candidate,
                                bool check_gz) {
  if (_clog_entry_exists(dir_fd, candidate)) return true;
  if (!check_gz) return false;
  /* This buffer has room beyond PATH_MAX, so there's no need to work out
   * exactly how much of _rotate()'s CLOG_ROTATION_EXTRA slack is left where
   * this gets called. candidate is always a NUL-terminated string that fit
   * in a PATH_MAX-byte buffer itself, so clen is below PATH_MAX and this
   * buffer always has plenty of room for clen + strlen(".gz") + 1. */
  char gz[PATH_MAX + 8];
  size_t clen = strlen(candidate);
  memcpy(gz, candidate, clen);
  memcpy(gz + clen, ".gz", 4); /* includes NUL */
  return _clog_entry_exists(dir_fd, gz);
}

/* ========================================================================== */
/*                         BACKGROUND COMPRESSION                             */
/* ========================================================================== */

/*
 * A shared target that compresses its rotated files owns one compressor
 * thread, started by the first rotation that needs a compression, so a
 * target that never compresses never pays for it. _rotate() only queues a
 * job and returns, so the gzip work never runs on the rotating thread (a
 * logging thread for a synchronous logger, the writer thread for an async
 * one), and neither a log call nor the async queue behind it ever waits for
 * a compression.
 *
 * One thread per target runs the jobs one at a time, oldest first, so at
 * most one compression of a target is in flight at any moment, and that is
 * what bounds the files on disk to max_rotated_files generations plus one
 * (see _prune_rotated()). The thread shares nothing with another target's
 * compressor, so a close or a fork only ever deals with its own target's
 * thread, under the mutex the fork handling already locks for that target.
 *
 * The target's last clog_close() stops the thread once it has finished
 * every job in the queue. The exit drain, which a CLOG_FATAL record runs as
 * well, waits for the queue within its budget; see _clog_exit_drain().
 */
/* The shared target whose writer thread or compressor thread the calling
 * thread is, or NULL. The exit drain skips that target: such a thread calling
 * exit() can't also serve the drain it would wait for, and it may hold that
 * target's mutex. See _clog_exit_drain(). */
static __thread const clog_shared_t *_clog_thread_serves;

/* True once this process has started a writer thread or a compressor thread,
 * the only state the exit drain has any work for. */
static _Atomic bool _clog_exit_drain_armed;

static void _clog_exit_drain_arm(void) {
  atomic_store_explicit(&_clog_exit_drain_armed, true, memory_order_relaxed);
}

/* Tries again to remove the temporary output of every compression in `left`
 * (see _gzip_compress_file()), each only while its name still holds the file
 * the compression made, and frees the jobs whose output is gone. Returns the
 * jobs whose removal failed again. */
static clog_compress_job_t *_clog_retry_tmp_removals(
    int dir_fd, clog_compress_job_t *left,
    const ccol_memmgmt_procs_t *m_procs) {
  clog_compress_job_t *keep = NULL;
  while (left) {
    clog_compress_job_t *job = left;
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
      gone = _clog_unlink_if_same(dir_fd, tmp, &st);
    }
    if (gone) {
      _ccol_mem_free(m_procs, job);
    } else {
      job->next = keep;
      keep = job;
    }
  }
  return keep;
}

/*
 * sh's compressor thread. A compression that fails and can't remove its
 * temporary output, as in a directory that lost its write permission, leaves
 * that output to this thread, which retries the removal before each later
 * compression and once more when it stops. Whatever is still left after that
 * gets removed by the recovery of the log's next open (see
 * _clog_recover_compressions()).
 */
static void *_clog_compressor_main(void *arg) {
  clog_shared_t *sh = (clog_shared_t *)arg;
  _clog_thread_serves = sh;
  _clog_thread_signal_init();
  clog_compress_job_t *left = NULL;
  _ccol_mutex_lock(sh->mutex);
  for (;;) {
    while (!sh->compress_head && !sh->compressor_stop)
      _ccol_cond_var_wait(sh->compress_cv, sh->mutex);
    clog_compress_job_t *job = sh->compress_head;
    if (!job) break; /* stop requested and nothing left to do */
    sh->compress_head = job->next;
    if (!sh->compress_head) sh->compress_tail = NULL;
    job->next = NULL;
    sh->compress_running = job;
    int dir_fd = sh->dir_fd;
    _ccol_mutex_unlock(sh->mutex);

    if (left) left = _clog_retry_tmp_removals(dir_fd, left, sh->m_procs);
    /* A compression's output is a regular file, which the process's file
     * size limit bounds like the log itself (see "SIGPIPE AND SIGXFSZ"). */
    _clog_thread_sync_signal_mask();
    struct stat tmp_left;
    if (_gzip_compress_file(dir_fd, job->src, job->dst, &sh->compress_abort,
                            job->live_uid, &tmp_left) != 0)
      _clog_consume_own_sigxfsz();

    _ccol_mutex_lock(sh->mutex);
    sh->compress_running = NULL;
    if (tmp_left.st_ino != 0) {
      job->tmp_dev = tmp_left.st_dev;
      job->tmp_ino = tmp_left.st_ino;
      job->next = left;
      left = job;
    } else {
      _ccol_mem_free(sh->m_procs, job);
    }
    /* A pass that ran during the compression kept this generation only
     * because it was running. It's older than the newest max_rotated_files,
     * and without this pass it would stay on disk next to the newer
     * generations until the next rotation. */
    if (sh->compress_prune_deferred) {
      sh->compress_prune_deferred = false;
      _prune_rotated(sh, NULL, false);
    }
    /* Wake every thread waiting for the queue to drain. */
    _ccol_cond_var_broadcast(sh->compress_cv);
  }
  int dir_fd = sh->dir_fd;
  _ccol_mutex_unlock(sh->mutex);
  if (left) left = _clog_retry_tmp_removals(dir_fd, left, sh->m_procs);
  while (left) {
    clog_compress_job_t *job = left;
    left = job->next;
    _ccol_mem_free(sh->m_procs, job);
  }
  return NULL;
}

/* Starts sh's compressor thread. The caller holds sh->mutex, and
 * sh->compressor_live is false. Returns false when the thread can't start,
 * and then leaves nothing behind. */
static bool _clog_compressor_start(clog_shared_t *sh) {
  /* CLOCK_MONOTONIC, so the exit drain's bounded wait on this condition
   * variable doesn't move with the wall clock. */
  _ccol_cond_var_attr_t ca;
  if (_ccol_cond_var_attr_init(ca) != 0) return false;
  int cv_rc = _ccol_cond_var_attr_setclock(ca, CLOCK_MONOTONIC);
  if (cv_rc == 0) cv_rc = _ccol_cond_var_init_ca(sh->compress_cv, ca);
  _ccol_cond_var_attr_destroy(ca);
  if (cv_rc != 0) return false;
  _clog_exit_drain_arm();
  sh->compressor_stop = false;
  if (_ccol_thread_create(sh->compressor_thread, _clog_compressor_main, sh) !=
      0) {
    _ccol_cond_var_destroy(sh->compress_cv);
    return false;
  }
  sh->compressor_live = true;
  return true;
}

/*
 * Queues the compression of the rotated file `rotated`, a name relative to
 * sh->dir_fd. The caller holds sh->mutex. The job is one allocation holding
 * both of its names. When the allocation fails, or the compressor thread
 * can't start, the rotated file stays uncompressed, which is the documented
 * outcome of a failed compression, and no log data is lost. live_uid is the
 * live file's owner (see clog_gen_filter_t).
 */
static void _clog_compress_enqueue(clog_shared_t *sh, const char *rotated,
                                   uid_t live_uid) {
  size_t n = strlen(rotated);
  clog_compress_job_t *job =
      _ccol_mem_alloc(sh->m_procs, sizeof(*job) + (n + 1) + (n + 4));
  if (!job) return;
  job->next = NULL;
  job->live_uid = live_uid;
  job->src = (char *)(job + 1);
  memcpy(job->src, rotated, n + 1);
  job->dst = job->src + n + 1;
  memcpy(job->dst, rotated, n);
  memcpy(job->dst + n, ".gz", 4); /* includes NUL */

  if (!sh->compressor_live && !_clog_compressor_start(sh)) {
    _ccol_mem_free(sh->m_procs, job);
    return;
  }
  if (sh->compress_tail)
    sh->compress_tail->next = job;
  else
    sh->compress_head = job;
  sh->compress_tail = job;
  _ccol_cond_var_broadcast(sh->compress_cv);
}

/*
 * Queues again every compression that a stopped process left unfinished in
 * sh's directory. The caller holds sh->mutex, and sh rotates and compresses.
 * A compression that stops partway leaves its temporary output behind (see
 * _gzip_compress_file()), and this removes it. A rotated file that is still
 * uncompressed was never compressed, or its compression never finished,
 * because a compression removes its source only after the finished output
 * has taken the ".gz" name. So a ".gz" file next to such a source is an
 * output not known to be complete while the source holds everything, and
 * this removes the ".gz" file. Every uncompressed rotated file then goes to
 * the compressor thread, oldest first.
 *
 * It acts only on sh's generations (see clog_gen_filter_t): a temporary
 * output, a source and a ".gz" file are each removed or compressed only when
 * they are one, and every other entry gets a WARN record. A source whose
 * ".gz" name holds an entry that isn't a generation stays uncompressed.
 */
static void _clog_recover_compressions(clog_shared_t *sh) {
  const ccol_memmgmt_procs_t *m_procs = sh->m_procs;
  const char *base = sh->file_base;
  size_t base_len = strlen(base);
  static const char tmp_tail[] = ".gz" CLOG_GZ_TMP_SUFFIX;
  size_t tmp_tail_len = sizeof(tmp_tail) - 1;
  clog_gen_filter_t f;
  _clog_gen_filter_init(sh, &f);

  DIR *d = _clog_opendir_at(sh->dir_fd);
  if (!d) return;
  clog_rotated_file_t *files = NULL;
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
      if (_rotated_entry_match(gz_name, base, base_len, &stamp, &seq) &&
          _clog_entry_check(sh, n, stamp, &f, true) == CLOG_ENTRY_GENERATION)
        unlinkat(sh->dir_fd, n, 0);
      continue;
    }
    if (!_rotated_entry_match(n, base, base_len, &stamp, &seq)) continue;
    if (n_len >= 3 && strcmp(n + n_len - 3, ".gz") == 0) continue;
    if (_clog_entry_check(sh, n, stamp, &f, true) != CLOG_ENTRY_GENERATION)
      continue;
    if (fc >= cap) {
      int nc = cap ? cap * 2 : 16;
      clog_rotated_file_t *nf =
          _ccol_mem_realloc(m_procs, files, (size_t)nc * sizeof(*files));
      if (!nf) break;
      files = nf;
      cap = nc;
    }
    char *name = _ccol_mem_alloc(m_procs, n_len + 1);
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
    qsort(files, (size_t)fc, sizeof(*files), _cmp_rotated_oldest_first);
  for (int i = 0; i < fc; i++) {
    char gz[PATH_MAX + 8];
    size_t plen = strlen(files[i].path);
    if (plen + 4 <= sizeof(gz)) {
      memcpy(gz, files[i].path, plen);
      memcpy(gz + plen, ".gz", 4); /* includes NUL */
      clog_entry_kind_t gz_kind =
          _clog_entry_check(sh, gz, files[i].stamp, &f, true);
      if (gz_kind == CLOG_ENTRY_GENERATION) unlinkat(sh->dir_fd, gz, 0);
      if (gz_kind != CLOG_ENTRY_IGNORED)
        _clog_compress_enqueue(sh, files[i].path, f.live_uid);
    }
    _ccol_mem_free(m_procs, files[i].path);
  }
  _ccol_mem_free(m_procs, files);
}

/*
 * Stops and joins sh's compressor thread, if it runs. The thread first
 * finishes every job in its queue, so every rotation of the target is
 * compressed by the time this returns. The caller must not hold sh->mutex,
 * and no handle of sh that could rotate may remain: the last clog_close()
 * calls this once the writer thread (which can still rotate while it
 * drains) has stopped. A constructor rollback calls it too, and there it does
 * nothing.
 */
static void _shared_compressor_teardown(clog_shared_t *sh) {
  _ccol_mutex_lock(sh->mutex);
  bool live = sh->compressor_live;
  if (live) {
    sh->compressor_stop = true;
    _ccol_cond_var_broadcast(sh->compress_cv);
  }
  _ccol_mutex_unlock(sh->mutex);
  if (!live) return;
  _ccol_thread_join(sh->compressor_thread);
  _ccol_cond_var_destroy(sh->compress_cv);
  sh->compressor_live = false;
}

/* ========================================================================== */
/*                         ROTATE                                             */
/* ========================================================================== */

/* 64 bits another user can't predict, for the private name of a new live
 * file. A system where getrandom(2) can't answer at once gets a mix of
 * clocks, the pid, a counter and addresses; the O_EXCL creation stays
 * correct either way, and only how well it resists a name planted in advance
 * depends on this. */
static uint64_t _clog_mix64(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static uint64_t _clog_random64(void) {
  uint64_t r;
  if (ccol_random_bytes(&r, sizeof(r))) return r;
  static _Atomic uint64_t counter;
  struct timespec rt = {0}, mt = {0};
  clock_gettime(CLOCK_REALTIME, &rt);
  clock_gettime(CLOCK_MONOTONIC, &mt);
  r = _clog_mix64(atomic_fetch_add(&counter, 1) ^ (uint64_t)getpid());
  r = _clog_mix64(r ^
                  ((uint64_t)rt.tv_sec * 1000000000ULL + (uint64_t)rt.tv_nsec));
  r = _clog_mix64(r ^
                  ((uint64_t)mt.tv_sec * 1000000000ULL + (uint64_t)mt.tv_nsec));
  r = _clog_mix64(r ^ (uint64_t)(uintptr_t)&r);
  return r;
}

/* True when name is "<base>" CLOG_ROTATION_TMP_TAG followed by
 * CLOG_ROTATION_TMP_HEX_DIGITS lowercase hex digits, the private name of a
 * new live file (see _clog_create_fresh()). */
static bool _clog_is_live_tmp_name(const char *name, const char *base,
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

/*
 * Creates a rotation's next live file under a private name of its own,
 * relative to dir_fd, and writes that name to tmp (tmp_size bytes). The name
 * is "<base>" CLOG_ROTATION_TMP_TAG plus random hex digits, and O_EXCL with
 * O_NOFOLLOW either creates a new regular file there or fails, so the file
 * can never be an entry somebody else put at that name. When old isn't
 * NULL, the file gets the permission bits, owner and group of the file it
 * replaces, which *old describes, so a log its operator restricted (mode
 * 0600 or 0640, say) stays restricted in every later generation, and a
 * compressed generation takes its source's access (see
 * _gzip_compress_file()). The file is created with mode 0600 and
 * _clog_copy_owner_and_mode() gives it the rest. With old NULL it's created
 * with mode 0644, narrowed by the umask. Returns the descriptor and fills
 * *st_out with the finished file, or returns -1.
 */
static int _clog_create_fresh(int dir_fd, const char *base,
                              const struct stat *old, char *tmp,
                              size_t tmp_size, struct stat *st_out) {
  size_t blen = strlen(base);
  if (blen + CLOG_ROTATION_TMP_LEN + 1 > tmp_size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  for (int attempt = 0; attempt < 8; attempt++) {
    uint64_t r = _clog_random64() &
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
    if (old) _clog_copy_owner_and_mode(fd, old);
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

/*
 * Removes the private name of a new live file that a rotation left behind
 * when its process stopped between creating the file and renaming it onto
 * the live name (see _rotate()). Such a file is empty, since nothing writes
 * it before that rename, and only an empty regular file owned by the
 * effective user or the live file's owner, at a name of exactly that shape,
 * gets removed. The caller holds sh->mutex, or owns sh alone, and sh
 * rotates.
 */
static void _clog_remove_stale_live_tmps(clog_shared_t *sh) {
  const char *base = sh->file_base;
  size_t base_len = strlen(base);
  clog_gen_filter_t f;
  _clog_gen_filter_init(sh, &f);
  DIR *d = _clog_opendir_at(sh->dir_fd);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (!_clog_is_live_tmp_name(e->d_name, base, base_len)) continue;
    struct stat st;
    if (fstatat(sh->dir_fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(st.st_mode) && st.st_size == 0 &&
        _clog_gen_uid_ok(st.st_uid, f.euid, f.live_uid))
      (void)unlinkat(sh->dir_fd, e->d_name, 0);
  }
  closedir(d);
}

#ifdef RUNNING_UNIT_TESTS
/* A hook _rotate() calls between the moment the live name stops naming the
 * old file and the moment the new live file takes the name, for a test that
 * puts an entry at the live name inside that window. */
static void (*_Atomic _clog_test_rotate_window_hook)(int dir_fd,
                                                     const char *base);

void _clog_test_set_rotate_window_hook(void (*hook)(int dir_fd,
                                                    const char *base)) {
  atomic_store(&_clog_test_rotate_window_hook, hook);
}
#endif

/*
 * Closes the line a rotation leaves open at the end of the file it retires,
 * so every generation ends with a newline. old_fd is that file's descriptor,
 * old_st its fstat() taken during the rotation, and rotated its name in
 * sh->dir_fd, or NULL when the file has no name any more. It first writes
 * the newline; when the file refuses it (a file size limit the fragment
 * reaches, or a disk that is still full), it cuts the file back to the end
 * of its last whole line instead. The bytes the cut removes belong to a
 * record whose loss the target already owes, so they're added to the bytes
 * the marker names. The logger writes through a descriptor that can't read,
 * so the scan for that line end reads through a second descriptor of the
 * same name, which still has to hold old_st's dev/ino. Only a regular file
 * is closed this way, since nothing else can be cut, and a write to a
 * regular file never blocks. Returns true when the file ends with a newline,
 * is empty, or has no name anybody could read it through. The caller must
 * hold sh->mutex.
 */
static __attribute__((noinline, cold)) bool _clog_close_retired_line(
    clog_shared_t *sh, int old_fd, const struct stat *old_st,
    const char *rotated) {
  if (!S_ISREG(old_st->st_mode)) return false;
  ssize_t w;
  do {
    w = write(old_fd, "\n", 1);
  } while (w < 0 && errno == EINTR);
  if (w == 1) return true;
  if (w < 0 && errno == EFBIG) _clog_consume_own_sigxfsz();
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
  /* Read backward from the end in blocks until we find the newline that
   * ends the last whole line. */
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

/*
 * Rotates the current log file. The caller must hold shared->mutex. Every
 * name is relative to sh->dir_fd, the directory where the logger opened the
 * file.
 *
 * Steps:
 *   1. Create the next live file under a private name (see
 *      _clog_create_fresh()).
 *   2. Build a destination name with a timestamp and, when needed, a
 *      collision suffix. The name sorts above every rotated name before it.
 *   3. Rename the current file to the destination, never replacing an entry
 *      already sitting there.
 *   4. Rename the new file onto the live name, and close the old fd.
 *   5. Delete the old rotated files beyond max_rotated_files.
 *   6. Queue the compression of the rotated file, when compression is on.
 *
 * The rotation never opens the live name or a rotated name. The new live
 * file is one step 1 made, and step 4 puts it at the live name with a
 * rename, which replaces whatever entry sits there without following it. So
 * another user who can write to the directory and puts a link, a FIFO or a
 * file at the live name between steps 3 and 4 never gets a record and never
 * blocks the rotation: the live name belongs to the logger.
 */
static int _rotate(clog_shared_t *sh) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&_clog_test_rotate_attempt_count, 1);
#endif

  if (sh->dir_fd < 0 || sh->fd < 0) {
    return 0;
  }
  if (_clog_test_consume_rotate_failure()) return -1;

  time_t now = _clog_wall_now();
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
  /* The 14 stamp digits follow the '.' that CLOG_ROTATION_FMT writes. */
  char *stamp = rotated + plen + 1;

  /* Choose the name. _prune_rotated() keeps the rotated files whose
   * (stamp, sequence number) pairs are newest (see
   * _cmp_rotated_oldest_first()), so every new name must sort above every
   * name this shared target made before, and above every generation that was
   * on disk before its first rotation. A name the deletion pass freed must
   * never come back, since it sorts as the oldest and the next pass would
   * delete the newest log records in its place.
   *
   * A clock reading the same second as the newest name continues its
   * sequence. A clock reading an earlier second, because it stepped back,
   * keeps the newest name's stamp and continues its sequence too, so the
   * order of the names stays the order of the rotations. This target's
   * derived loggers share these fields under sh->mutex. An entry that isn't a
   * generation (see clog_gen_filter_t) plays no part here, so an entry with
   * a stamp far in the future can't hold the names at a stamp whose sequence
   * is used up. */
  if (!sh->rot_name_known)
    sh->rot_name_known = _rotated_newest_on_disk(sh, true, sh->rot_last_stamp,
                                                 &sh->rot_last_seq);
  unsigned long seq = 0;
  if (sh->rot_name_known &&
      memcmp(stamp, sh->rot_last_stamp, CLOG_ROTATION_STAMP_DIGITS) <= 0) {
    memcpy(stamp, sh->rot_last_stamp, CLOG_ROTATION_STAMP_DIGITS);
    seq = sh->rot_last_seq + 1;
  }

  /* The fstat() describes the file the logger writes, which the new live
   * file takes its access from. It reads the descriptor, never the name. */
  struct stat old_st;
  bool have_old_st = fstat(sh->fd, &old_st) == 0;
  char tmp[PATH_MAX];
  struct stat new_st;
  int new_fd = _clog_create_fresh(sh->dir_fd, sh->file_base,
                                  have_old_st ? &old_st : NULL, tmp,
                                  sizeof(tmp), &new_st);
  if (new_fd < 0) return -1;

  /* The name can still be taken by an entry this target didn't make, and
   * then the sequence moves on until the name is free. check_gz applies the
   * same test to the compressed form of the name whenever compression is on
   * (see the doc comment of _rotated_name_taken()). The rename never replaces
   * an entry at the destination, and an entry that shows up there between the
   * test and the rename moves the sequence on as well.
   *
   * The rename happens while the old fd is still open, which POSIX allows for
   * an open file, and the old fd is closed only once the new file holds the
   * live name. So a failed step leaves the logger alive and writes go on into
   * the old file. ENOENT means something outside this process deleted the
   * file; we treat that as a clean slate and the new file takes the live
   * name. Any other rename error is a hard failure, and the logger keeps
   * writing to the original fd, which is still open. */
  bool check_gz = sh->rotation.compress_rotated;
  size_t base = plen + slen;
  int rename_rv = -1;
  int rename_errno = 0;
  for (;; seq++) {
    if (seq > CLOG_ROTATION_SEQ_MAX) {
      rename_errno = EEXIST;
      break;
    }
    rotated[base] = '\0';
    if (seq > 0 &&
        snprintf(rotated + base, sizeof(rotated) - base, "_%04lu", seq) < 0) {
      rename_errno = EINVAL;
      break;
    }
    if (_rotated_name_taken(sh->dir_fd, rotated, check_gz)) continue;
    rename_rv = _clog_publish_no_replace(sh->dir_fd, sh->file_base, rotated);
    rename_errno = errno;
    if (rename_rv == 0 || rename_errno != EEXIST) break;
  }
  if (rename_rv != 0 && rename_errno != ENOENT) {
    (void)_clog_unlink_if_same(sh->dir_fd, tmp, &new_st);
    close(new_fd);
    return -1;
  }
  /* `rotated` exists on disk only when the rename above really succeeded;
   * on the ENOENT clean-slate path there's nothing at that path to delete or
   * to compress. */
  bool did_rename = (rename_rv == 0);

#ifdef RUNNING_UNIT_TESTS
  {
    void (*hook)(int, const char *) =
        atomic_load(&_clog_test_rotate_window_hook);
    if (hook) hook(sh->dir_fd, sh->file_base);
  }
#endif

  if (renameat(sh->dir_fd, tmp, sh->dir_fd, sh->file_base) != 0) {
    /* Recovery: put the original name back, so the fd that is still open
     * stays the live file, unless an entry has already taken the name. */
    if (did_rename)
      (void)_clog_publish_no_replace(sh->dir_fd, rotated, sh->file_base);
    (void)_clog_unlink_if_same(sh->dir_fd, tmp, &new_st);
    close(new_fd);
    return -1;
  }

  /* The old file stays open until the deletion pass below has run, so a
   * newline it still owes can use the room the pass frees (see
   * _clog_close_retired_line()). The new file starts at the start of a line,
   * so the new file's repair owes no newline, and a loss the target still
   * owes gets named in the new file. */
  int old_fd = sh->fd;
  bool old_line_open = sh->owe_newline;
  sh->fd = new_fd;
  sh->sink_kind = _clog_classify_sink(new_fd);
  sh->sink_per_record = _clog_sink_is_message(new_fd);
  sh->bytes_written = 0;
  sh->last_rotation = now;
  if (old_line_open) sh->owe_newline = false;
  memcpy(sh->rot_last_stamp, stamp, CLOG_ROTATION_STAMP_DIGITS);
  sh->rot_last_seq = seq;
  sh->rot_name_known = true;

  /* The deletion pass runs only once the rotation has certainly succeeded,
   * meaning the new live file holds the live name. Deleting earlier would let
   * a rotation that then fails for good delete older rotated files for an
   * attempt that gets rolled back. */
  if (sh->rotation.max_rotated_files > 0) {
    _prune_rotated(sh, did_rename ? rotated : NULL, true);
  }
  if (old_line_open &&
      !(have_old_st && _clog_close_retired_line(sh, old_fd, &old_st,
                                                did_rename ? rotated : NULL))) {
    /* Neither the newline nor the cut reached the old file, so the new
     * file's marker starts with the newline, and the generations read one
     * after another keep the marker on a line of its own. */
    sh->owe_newline = true;
  }
  if (!(sh->owe_newline || sh->owed_losses > 0)) {
    sh->async_partial_record &= (unsigned char)~CLOG_OWED_REPAIR;
    atomic_store_explicit(&sh->owes_repair, false, memory_order_relaxed);
  }
  close(old_fd);

  /* Gated on did_rename because on the ENOENT clean-slate path above
   * `rotated` was never created, so there's nothing to compress. A
   * compression writes "<rotated>.gz" + CLOG_GZ_TMP_SUFFIX and then
   * "<rotated>.gz". When the directory can't hold a name that long, no
   * compression of this file can succeed; the file stays uncompressed, and a
   * record in the new live file says so rather than the compression failing
   * unseen. */
  if (did_rename && sh->rotation.compress_rotated) {
    long name_max = fpathconf(sh->dir_fd, _PC_NAME_MAX);
    size_t gz_tmp_len = strlen(rotated) + sizeof(".gz" CLOG_GZ_TMP_SUFFIX) - 1;
    if (name_max > 0 && gz_tmp_len > (size_t)name_max)
      _clog_note_record(sh, sh->format, CLOG_WARN,
                        "log rotation: a rotated file stays uncompressed: ",
                        "its compressed name exceeds the name length limit "
                        "of the directory",
                        CLOG_WRITE_WAIT_FOREVER);
    else
      _clog_compress_enqueue(sh, rotated, new_st.st_uid);
  }

  return 0;
}

/*
 * The time-based rotation check that runs before a write. It rotates
 * when time rotation is on and the interval is over, unless the retry
 * backoff stops it. Every code path about to write a record's real bytes to
 * sh->fd calls it, so no single path can silently skip time-based rotation
 * while all the others run it. The caller must hold sh->mutex.
 */
/* Returns true when the time rotation interval is over at `now`. Both
 * operands are widened to 64 bits before the subtraction, because time_t is
 * only 32 bits on some targets, where the difference of two distant
 * timestamps can overflow it. The elapsed whole seconds are compared in
 * microseconds with rotation_interval_us, so an interval that isn't a whole
 * number of seconds rotates at the next whole second after it ends. A count
 * of seconds too big to multiply is past every interval. A clock that
 * stepped back makes the difference negative, which isn't due;
 * _clog_time_rotate_if_due() then restarts the interval at `now`, so the next
 * rotation comes one interval later and not when the clock gets back to the
 * earlier reading. */
static inline bool _clog_time_rotation_elapsed(const clog_shared_t *sh,
                                               time_t now) {
  int64_t since = (int64_t)now - (int64_t)sh->last_rotation;
  return since >= 0 && ((uint64_t)since >= UINT64_MAX / UINT64_C(1000000) ||
                        (uint64_t)since * UINT64_C(1000000) >=
                            sh->rotation.rotation_interval_us);
}

/* Returns true when the retry backoff after a failed rotation allows another
 * attempt at `now`. A backoff ending more than
 * CLOG_ROTATE_RETRY_BACKOFF_SECS after `now` was set before the wall clock
 * stepped back, so it bounds nothing and the attempt goes ahead. Without
 * this, a step back would block every rotation until the clock got back to
 * its earlier reading. */
static inline bool _clog_rotate_retry_allowed(const clog_shared_t *sh,
                                              time_t now) {
  int64_t left = (int64_t)sh->rotate_retry_after - (int64_t)now;
  return left <= 0 || left > (int64_t)CLOG_ROTATE_RETRY_BACKOFF_SECS;
}

static void _clog_time_rotate_if_due(clog_shared_t *sh) {
  if (!(sh->rotation_enabled && sh->rotation.time_rotation_enabled)) return;
  /* Put the rotation off while the async batch buffer holds the
   * continuation of a record whose first half is already on disk, because
   * rotating here would put the two halves in two different files. See
   * clog_shared_t.async_partial_record. */
  if (sh->async_partial_record & CLOG_SPLIT_MASK) return;
  time_t now = _clog_wall_now();
  if (now < sh->last_rotation) {
    /* The wall clock stepped back past the start of the interval. */
    sh->last_rotation = now;
    return;
  }
  if (_clog_time_rotation_elapsed(sh, now) &&
      _clog_rotate_retry_allowed(sh, now)) {
    int rrv = _rotate(sh);
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

/*
 * The size-based rotation check that runs after a write. It rotates at
 * once when size rotation is on and bytes_written crosses max_file_size,
 * unless the retry backoff stops it. Every path shares it for the same reason
 * they all share _clog_time_rotate_if_due(). The caller must hold sh->mutex
 * and must call it only once sh->bytes_written already covers the write that
 * just happened.
 */
static void _clog_size_rotate_if_due(clog_shared_t *sh) {
  /* bytes_written, this file's internal counter, is an off_t, while
   * max_file_size is a fixed-width int64_t in the public config's layout.
   * Widening the counter keeps the comparison right whatever width off_t has
   * in this build. */
  if (!(sh->rotation_enabled && sh->rotation.size_rotation_enabled &&
        (int64_t)sh->bytes_written >= sh->rotation.max_file_size))
    return;
  /* Put the rotation off for the same reason _clog_time_rotate_if_due()
   * does. The flush that clears the flag runs this check again, so the
   * rotation skipped here happens as soon as the record on disk is whole
   * again. See clog_shared_t.async_partial_record. */
  if (sh->async_partial_record & CLOG_SPLIT_MASK) return;
  time_t now = _clog_wall_now();
  if (_clog_rotate_retry_allowed(sh, now)) {
    int rrv = _rotate(sh);
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

/* Returns true when a size or time rotation of sh is due now and the retry
 * backoff allows it, whatever async_partial_record's split bits say. The
 * caller must hold sh->mutex. */
static bool _clog_rotation_due(const clog_shared_t *sh) {
  if (!sh->rotation_enabled) return false;
  time_t now = _clog_wall_now();
  if (!_clog_rotate_retry_allowed(sh, now)) return false;
  if (sh->rotation.size_rotation_enabled &&
      (int64_t)sh->bytes_written >= sh->rotation.max_file_size)
    return true;
  return sh->rotation.time_rotation_enabled &&
         _clog_time_rotation_elapsed(sh, now);
}

/* ========================================================================== */
/*                         BACKTRACE                                          */
/* ========================================================================== */

/* Frame 0 is _capture_backtrace, frame 1 is _clog_write, and frame 2 is the
 * caller, the first frame the user sees. Every format below that writes a
 * backtrace uses this value. */
#define CLOG_BT_INITIAL_FRAME 2

/*
 * Captures the calling thread's backtrace. It's noinline, and _clog_write's
 * frame calls it directly on every path (synchronous, async and FATAL),
 * never from a helper one level deeper. CLOG_BT_INITIAL_FRAME above is right
 * only as long as that holds: capturing from inside the async submission
 * path, for example, would silently show that function as if it were the
 * caller. It returns false, and leaves *out_syms and *out_depth alone, on a
 * platform without backtrace support and when backtrace_symbols() fails its
 * own allocation. A caller must treat false exactly like "no frames".
 */
static bool __attribute__((noinline)) _capture_backtrace(char ***out_syms,
                                                         int *out_depth) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&_clog_test_force_bt_capture_failure)) return false;
#endif
#if CLOG_HAS_BACKTRACE
  void *ptrs[CLOG_BACKTRACE_DEPTH];
  int depth = backtrace(ptrs, CLOG_BACKTRACE_DEPTH);
  char **syms = backtrace_symbols(ptrs, depth);
  if (!syms) return false;
#ifdef RUNNING_UNIT_TESTS
  /* See the doc comment of _clog_test_force_shallow_backtrace_depth(). syms
   * stays a real, non-NULL capture and only the reported depth is clamped,
   * so every emitter sees the same shape as for a real shallow call stack:
   * the capture really succeeded and found nothing past the first two
   * frames. */
  if (atomic_load(&_clog_test_force_shallow_bt_depth) &&
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

/*
 * A fixed marker written in place of the usual "\t#N ..." continuation lines
 * whenever the real backtrace couldn't be captured at all, which happens on a
 * platform without support and when the allocation inside
 * backtrace_symbols() fails. Without it, a NULL syms makes the emitter do
 * nothing and say nothing, so a reader of a logfmt record that asked for a
 * backtrace couldn't tell "the library left the backtrace out" from "nothing
 * ever asked for a backtrace". JSON carries the same distinction with
 * _BT_JSON_ERROR_MARKER.
 */
static const char _BT_LINE_ERROR_MARKER[] = "\t#error backtrace unavailable\n";

/*
 * Appends the backtrace frames to out as tab-indented continuation lines; it
 * never writes anything to a file itself. A NULL syms means no backtrace is
 * available, or _capture_backtrace() itself failed, and then it appends the
 * single fixed _BT_LINE_ERROR_MARKER line instead, so a missing backtrace is
 * never silent.
 *
 * Returns true when `out` gives an accurate picture of the backtrace: the
 * frames that fit, no frame because none existed, or a marker showing that a
 * requested backtrace is missing. There are three such cases. In the first,
 * at least one real frame line went in: a longer symbol name that doesn't fit
 * is silently skipped while a later, shorter one that fits goes in, instead
 * of giving up on the whole backtrace the moment the first frame doesn't
 * fit. A frame skipped like that carries no marker of its own, which matches
 * this function's best-effort-per-frame contract. In the second, depth is at
 * most CLOG_BT_INITIAL_FRAME: the capture really succeeded but found no frame
 * beyond the first two bookkeeping ones, so there's nothing to append and
 * nothing to report as missing. _emit_backtrace_json() makes the same
 * distinction with its own array_content_is_accurate flag, so a real but
 * shallow capture is never reported as an unavailable one; without this case
 * it would fall through into "not a single frame fit". In the third, real
 * frames existed, not one of them fit, and the fixed-size
 * _BT_LINE_ERROR_MARKER went in their place.
 *
 * It returns false only when `out` has almost no room left at all, not even
 * for the marker of about 30 bytes. That case is residual, and this file's
 * buffer-size constants make it unreachable in practice, since the caller
 * already has plenty of room for the primary record and this call's
 * record-start position sits after that record. The call site in
 * _clog_build_record() shows how this very narrow case is handled.
 */
static bool _emit_backtrace_lines(clog_buf_t *out, char *const *syms,
                                  int depth) {
  if (!syms)
    return _buf_append(out, _BT_LINE_ERROR_MARKER,
                       sizeof(_BT_LINE_ERROR_MARKER) - 1) == 0;

  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    size_t entry_start = out->len;
    /* syms[i] comes from backtrace_symbols(), and for this format's
     * line-based framing it is untrusted text of any shape. So it goes
     * through the same control-character escaper every other dynamic field
     * of this format uses, and an odd symbol name, an embedded newline above
     * all, can never desynchronize the log stream the way an unescaped name
     * could. Building this frame's line can fail when growing the buffer
     * fails to allocate, and then we roll back to entry_start and skip the
     * frame; without the rollback a truncated fragment with no newline would
     * stay behind, and the next frame's text would go straight onto it. */
    bool ok = _buf_appendf(out, "\t#%d ", i - CLOG_BT_INITIAL_FRAME) == 0;
    ok = ok && _buf_append_ctrl_escaped(out, syms[i]) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
    if (!ok) {
      out->len = entry_start;
      continue;
    }
    any_frame_written = true;
  }
  /* A depth of at most CLOG_BT_INITIAL_FRAME means no frame beyond the first
   * two ever existed, so writing nothing here is a real and accurate picture
   * of a successful capture, silent as it is, and not a truncation.
   * _emit_backtrace_json()'s array_content_is_accurate check covers the same
   * case the same way. */
  if (any_frame_written || depth <= CLOG_BT_INITIAL_FRAME) return true;

  /* Real frames existed but not a single one fit, because even one frame's
   * line needed more room than was left. Fall back to the same small
   * fixed-size marker the syms == NULL branch above uses, so a real
   * backtrace the record shows nothing of never looks the same as one
   * nothing asked for. */
  return _buf_append(out, _BT_LINE_ERROR_MARKER,
                     sizeof(_BT_LINE_ERROR_MARKER) - 1) == 0;
}

/*
 * A fixed-size marker written in place of the "bt" array whenever the real
 * backtrace can't go into the record: after a backtrace_symbols() failure,
 * and after an allocation failure while growing the buffer to append the
 * array itself or the structure around one of its frames. The marker is
 * there so the record shows that a requested backtrace is missing instead of
 * staying silent. A caller reading an ERROR, ALERT or FATAL record with
 * with_backtrace set needs to be able to tell "backtrace left out" from
 * "backtrace never asked for", and must never see this record's real ts,
 * level and msg content replaced by the general oversized-record fallback,
 * which would be pointless when only the backtrace text, usually much
 * bigger, didn't fit.
 */
static const char _BT_JSON_ERROR_MARKER[] = ",\"bt_error\":\"unavailable\"";

/*
 * Appends the backtrace frames as a JSON array:
 * ,"bt":["#0 sym","#1 sym",...]
 * The caller calls it while the JSON object is still open, before the
 * closing "}\n". A NULL syms is a capture failure here, exactly as for every
 * other backtrace emitter in this file.
 *
 * Returns true when the record can safely be closed as it is, which is when
 * a complete "bt" array went in (possibly short of some frames), and when
 * the fixed-size _BT_JSON_ERROR_MARKER went in instead of the array. It
 * returns false only when not even that small marker would fit because the
 * buffer has almost no room left, and the caller must then fall back to the
 * general oversized-record placeholder.
 *
 * It follows _emit_backtrace_lines()'s best-effort-per-frame contract in two
 * ways a plain "append until something fails" loop doesn't give for free.
 * First, a frame that doesn't fit is skipped with `continue`, not `break`, so
 * a later frame with shorter symbol text still gets its chance, instead of
 * everything being given up the instant an earlier frame doesn't fit.
 * Second, an empty array is closed as "]" only when the emptiness is real,
 * that is, when no frame beyond the first two was ever captured. When real
 * frames exist (depth > CLOG_BT_INITIAL_FRAME) and not one of them fits, an
 * empty "bt":[] would be byte for byte the same as the really shallow case,
 * which silently defeats the reason this function exists, so the omission
 * is shown with the same _BT_JSON_ERROR_MARKER every other zero-frames-fit
 * case in this file uses.
 */
static bool _emit_backtrace_json(clog_buf_t *b, char *const *syms, int depth) {
  if (!syms)
    return _buf_append(b, _BT_JSON_ERROR_MARKER,
                       sizeof(_BT_JSON_ERROR_MARKER) - 1) == 0;

  size_t bt_start = b->len;
  if (_buf_append(b, ",\"bt\":[", 7) != 0) {
    /* Nothing went in, so bt_start equals b->len. Show the omission instead
     * of quietly leaving the record without any "bt" key. */
    return _buf_append(b, _BT_JSON_ERROR_MARKER,
                       sizeof(_BT_JSON_ERROR_MARKER) - 1) == 0;
  }

  bool first = true;
  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    size_t entry_start = b->len;
    char prefix[20];
    int pl =
        snprintf(prefix, sizeof(prefix), "#%d ", i - CLOG_BT_INITIAL_FRAME);
    if ((!first && _buf_append(b, ",", 1) != 0) ||
        _buf_append(b, "\"", 1) != 0 ||
        (pl > 0 && _buf_append(b, prefix, (size_t)pl) != 0) ||
        _buf_append_json_content(b, syms[i]) != 0 ||
        _buf_append(b, "\"", 1) != 0) {
      b->len = entry_start; /* Roll the partial entry back and try the next
          frame rather than give up on the rest of the array */
      continue;
    }
    first = false;
    any_frame_written = true;
  }

  /* A depth of at most CLOG_BT_INITIAL_FRAME means no frame beyond the first
   * two ever existed, so an empty array here is real and not a truncation.
   * any_frame_written covers the other case, where real frames existed and
   * at least one of them fit. Either way it's safe to close the array,
   * because it shows accurately what happened. */
  bool array_content_is_accurate =
      any_frame_written || depth <= CLOG_BT_INITIAL_FRAME;
  if (array_content_is_accurate && _buf_append(b, "]", 1) == 0) return true;

  /* Two things get us here: either appending "]" itself failed, which means
   * almost no room is left at all, or real frames existed, none of them fit,
   * and a closed empty array would look like a really shallow backtrace.
   * Either way we discard whatever partial content `b` holds, because a "bt"
   * key with no closing bracket corrupts the record and a misleading empty
   * key misrepresents it, and show the omission with the marker instead. */
  b->len = bt_start;
  return _buf_append(b, _BT_JSON_ERROR_MARKER,
                     sizeof(_BT_JSON_ERROR_MARKER) - 1) == 0;
}

/*
 * Writes the backtrace frames as separate RFC 5424 syslog messages, one per
 * frame, never batched whatever the async mode, which matches the
 * one-write()-per-UDP-datagram contract clog_syslog_facility_t documents.
 * out is used only as scratch space: it's reset before each frame and left
 * reset after each frame. out can be one logger's buffer, on the synchronous
 * path, or the writer thread's shared aggregation buffer, on the async path;
 * both are safe, since they're only ever touched while sh->mutex is held.
 * The caller must hold sh->mutex. A NULL syms means no backtrace is
 * available, or _capture_backtrace() itself failed, and then one fixed
 * marker record is written instead of nothing, for the same reason
 * _emit_backtrace_lines() does it for logfmt: without the marker a reader
 * can't tell "the backtrace was left out" from "no backtrace was ever asked
 * for". A non-NULL syms with a depth of at most CLOG_BT_INITIAL_FRAME writes
 * nothing at all, because the capture really succeeded but found no frame
 * beyond the first two bookkeeping ones; _emit_backtrace_json()'s
 * array_content_is_accurate check knows the same really empty case. Only
 * real frames that all failed to go out count as an omission worth a
 * marker.
 *
 * ts is the SAME captured timestamp as the primary record these frames
 * continue (the caller's `ts` or `job->ts`), and it's never derived again
 * here. For an async logger the writer thread may build these lines long
 * after the record was submitted, and every other piece of per-record data
 * in this file works the same way: the fields, proc_val and the symbols
 * themselves all come from one capture at submission time and aren't read
 * again at write time. A backtrace frame's TIMESTAMP field must agree with
 * its primary record's, not show whatever the wall clock says when the
 * writer thread finally gets to the job.
 */
/* The HOSTNAME field of every RFC 5424 record of sh, a record's header and
 * each backtrace line alike: the cached name, or "-", the grammar's
 * NILVALUE, when the cache is empty. */
static inline const char *_clog_syslog_hostname(const clog_shared_t *sh) {
  return sh->syslog_hostname[0] ? sh->syslog_hostname : "-";
}

/*
 * Builds and writes the one fixed "backtrace unavailable" marker record,
 * which _emit_backtrace_syslog_lines() below uses for a failed capture,
 * where syms == NULL, and for a capture that succeeded but where not a single
 * frame could go out because every frame's append failed (under a lasting
 * allocation failure, for example). It resets `out` first, as each frame
 * does with its scratch space before use, and leaves `out` reset afterwards
 * on every path, as _emit_backtrace_syslog_lines()'s documented contract
 * says (leave it reset after use).
 */
static void _write_backtrace_unavailable_syslog_marker(
    clog_buf_t *out, clog_shared_t *sh, clog_level_t level,
    const struct timeval *ts) {
  int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
  const char *hostname = _clog_syslog_hostname(sh);
  const char *appname = sh->syslog_appname;
  const char *msgid = _LEVEL_STR[level];

  _buf_reset(out);
  bool ok = _buf_appendf(out, "<%d>1 ", pri) == 0;
  ok = ok && _buf_append_ts_at(out, ts) == 0;
  ok = ok && _buf_appendf(out,
                          " %s %s %d %s - #error backtrace "
                          "unavailable\n",
                          hostname, appname, (int)_clog_tident_get()->pid,
                          msgid) == 0;
  if (ok) {
    _clog_write_record(sh, CLOG_FMT_SYSLOG, out->data, out->len);
  } else {
    /* Not even this small marker record fit in `out`: a real allocation
     * failure on top of a backtrace capture that already failed, or on top
     * of every single frame that already failed to append for the same
     * reason. Fall back to a minimal write straight to the fd with no
     * allocation, following the last-resort pattern of
     * _clog_write_unrepresentable_record(), which can't fail itself: it
     * builds a fixed-size stack buffer with snprintf and never touches
     * clog_buf_t or the heap at all. So even this double failure never makes
     * a record loss completely silent. The buffer is much bigger than the
     * worst case (a full-length hostname and appname plus the literal text
     * around them), so this snprintf can't truncate in practice. */
    struct tm tm;
    gmtime_r(&ts->tv_sec, &tm);
    char fallback[512];
    int n = snprintf(fallback, sizeof(fallback),
                     "<%d>1 %04d-%02d-%02dT%02d:%02d:%02d.%06ldZ %s %s %d %s - "
                     "#error backtrace unavailable\n",
                     pri, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (long)ts->tv_usec,
                     hostname, appname, (int)_clog_tident_get()->pid, msgid);
    if (n > 0) {
      size_t len =
          (size_t)n < sizeof(fallback) ? (size_t)n : sizeof(fallback) - 1;
      _clog_write_record(sh, CLOG_FMT_SYSLOG, fallback, len);
    }
  }
  /* Leave `out` reset on every return path, as this function's documented
   * contract says (leave it reset after use). `out` can be sh->async_buf,
   * whose length is also the async writer thread's signal that "data is
   * pending and not flushed yet" (see _writer_flush_now()). The marker
   * bytes just written must not stay in `out->len`, or the next unrelated
   * flush would write those same bytes to sh->fd a second time although
   * they've already been sent. Three things trigger such a flush: an idle
   * flush_interval_us timeout, an explicit clog_flush(), and clog_close()'s
   * final drain. */
  _buf_reset(out);
}

static void _emit_backtrace_syslog_lines(clog_buf_t *out, clog_shared_t *sh,
                                         clog_level_t level,
                                         const struct timeval *ts,
                                         char *const *syms, int depth) {
  int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
  const char *hostname = _clog_syslog_hostname(sh);
  const char *appname = sh->syslog_appname;
  const char *msgid = _LEVEL_STR[level];

  if (!syms) {
    _write_backtrace_unavailable_syslog_marker(out, sh, level, ts);
    return;
  }

  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    _buf_reset(out);
    /* Each frame's line is built from several separate appends, and one of
     * them can fail partway, when the timestamp fits but the rest doesn't for
     * example. That would leave a truncated fragment without a newline in
     * the buffer, which _write_all() would still write, and it would then
     * merge into whatever goes out next and desynchronize the syslog stream.
     * So we track success across all the appends and skip the frame entirely
     * rather than write part of it. syms[i] comes from backtrace_symbols()
     * and goes through the same control-character escaper as the primary
     * record's MSG content, so an odd symbol name, an embedded newline above
     * all, can never desynchronize the syslog stream the way an unescaped
     * name could. */
    bool ok = _buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_appendf(out, " %s %s %d %s - #%d ", hostname, appname,
                            (int)_clog_tident_get()->pid, msgid,
                            i - CLOG_BT_INITIAL_FRAME) == 0;
    ok = ok && _buf_append_ctrl_escaped(out, syms[i]) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&_clog_test_force_all_syslog_bt_frames_fail)) ok = false;
#endif
    if (!ok) continue;
    _clog_write_record(sh, CLOG_FMT_SYSLOG, out->data, out->len);
    any_frame_written = true;
  }
  /* See the same note above. Leave `out` reset after the LAST frame too, not
   * just before each one, because the loop resets it only on the way INTO an
   * iteration and never after the final write on the way out. */
  _buf_reset(out);

  /* A depth of at most CLOG_BT_INITIAL_FRAME means no frame beyond the first
   * two ever existed, so writing no frame record here is a real and accurate
   * picture of a successful capture, not an omission.
   * _emit_backtrace_json()'s array_content_is_accurate check makes the same
   * exception, and so does _emit_backtrace_lines(). Only real frames
   * (depth > CLOG_BT_INITIAL_FRAME) that ALL failed to go out count as a real
   * omission that needs a marker. */
  if (!any_frame_written && depth > CLOG_BT_INITIAL_FRAME) {
    /* The capture succeeded, since syms is real, but every single frame's
     * append failed. Only a lasting real allocation failure can get here:
     * each frame gets an `out` that was just reset, so the buffer can't run
     * out of room because of unrelated earlier content, the way logfmt's
     * shared batch buffer can. Without this branch a real backtrace the
     * record shows nothing of would look the same as one nothing ever asked
     * for; the capture-failure branch above, where syms == NULL, gives the
     * same guarantee. */
    _write_backtrace_unavailable_syslog_marker(out, sh, level, ts);
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Calls _emit_backtrace_syslog_lines()'s syms == NULL branch, the one for a
 * failed backtrace capture, directly against the shared target of logger,
 * for a test. _clog_test_gzip_compress_file() is the precedent for calling a
 * hard-to-reach internal helper directly. This branch has its own
 * last-resort fallback for when even its small marker record fails to
 * append, and a test can only reach that fallback by forcing a backtrace
 * capture failure and a buffer append failure together, at exactly this
 * call, because the library's buffer-size constants otherwise give the
 * process no way to fail such a small append. See the doc comment of
 * _clog_test_force_next_buf_ensure_failure().
 */
void _clog_test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                         clog_level_t level) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return;
  struct timeval ts;
  gettimeofday(&ts, NULL);
  _ccol_mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    _emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, NULL, 0);
  _ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}
#endif

/* ========================================================================== */
/*                         ASYNC MESSAGE TYPES                                */
/* ========================================================================== */

/* CLOG_ASYNC_MSG_WAKE asks the writer thread for a plain flush and carries
 * nothing; see _clog_wake_writer(). */
typedef enum {
  CLOG_ASYNC_MSG_JOB,
  CLOG_ASYNC_MSG_FLUSH,
  CLOG_ASYNC_MSG_WAKE
} clog_async_msg_kind_t;

/*
 * Offsets into a clog_async_job_t's field_pool, NOT raw pointers. The pool
 * is a ccol_growbuf_t that _snapshot_fields() builds piece by piece, and a
 * growbuf's backing store can move on a realloc in the middle of that build.
 * An offset recorded during the build stays valid whatever moves happen,
 * and a raw pointer taken in the middle of it doesn't.
 */
typedef struct {
  size_t key_offset;
  size_t value_offset;
} clog_field_view_t;

/*
 * One unit of work for the writer thread. It goes into a queue and is fully
 * self-contained: it holds everything _clog_build_record() needs, all
 * captured by the submitting thread at submission time. The writer thread
 * never derives any of it again, since it may run much later and it's a
 * different thread from the one that formatted the message.
 */
typedef struct clog_async_job {
  clog_level_t level;
  const char *file; /* __FILE__ literal, never copied */
  int line;
  const char *func; /* __func__ literal, never copied */
  bool with_backtrace;
  struct timeval ts; /* captured at SUBMISSION time, not at write time */
  /* progname(pid):tname(tid), captured at submission, because the thread's
   * identity comes from here and not from the writer thread. */
  char proc_val[CLOG_PROC_VAL_LEN];

  char msg_inline[256]; /* A stack buffer that spills to the heap and then
      moves into the job. It's smaller than the synchronous path's 1024-byte
      stack buffer, because here it's a heap cost per queued message, not a
      cost on a thread's stack */
  char *msg_heap;       /* NULL unless the message spilled past msg_inline */
  const char *msg;      /* == msg_inline or msg_heap. It is never NULL */

  clog_field_view_t *fields; /* An array of field_count views, or NULL when
      field_count == 0. One allocation of the exact size given by
      chmap_elem_count(), a count that is O(1) and known before the walk, so
      no separate counting pass is needed */
  size_t field_count;
  ccol_growbuf_t field_pool;   /* The key and value bytes, separated by NUL.
        See the comment of clog_field_view_t. Built in one pass under
        fields_mutex */
  bool fields_snapshot_failed; /* True only when the snapshot couldn't be
      taken at all, because of field_pool's out-of-memory latch or because
      the views array itself failed to allocate. That's different from a
      logger that really has 0 fields */

  char **bt_syms; /* The result of backtrace_symbols(), captured by the
      ORIGINAL calling thread, since a backtrace from the writer thread's
      stack would mean nothing */
  int bt_depth;
} clog_async_job_t;

/* The rendezvous clog_flush() blocks on. The asking thread waits on ctrl.cv,
 * while the writer thread first flushes everything queued ahead of this
 * request and then sets ctrl.done and broadcasts. This object always lives on
 * the asking thread's stack frame (see _clog_flush_pinned()), and the writer
 * thread never frees it. */
typedef struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  bool done;
} clog_async_ctrl_t;

/*
 * The envelope that actually goes through the queue. kind is an explicit
 * discriminator, not an implicit check on two sizeof values happening to
 * differ, so the writer thread can always tell a real job from a flush
 * request, even if a future field addition makes two struct sizes equal by
 * accident.
 */
typedef struct clog_async_msg {
  clog_async_msg_kind_t kind;
  union {
    clog_async_job_t job;    /* For kind == CLOG_ASYNC_MSG_JOB. It's on the
          heap, and the writer thread owns and frees it; when the message
          never made it into the queue at all, _clog_write_async()'s
          failed-enqueue fallback frees it instead. The job sits BY VALUE in
          this union, so it has no allocation of its own, only the one for
          the whole envelope */
    clog_async_ctrl_t *ctrl; /* For kind == CLOG_ASYNC_MSG_FLUSH. It points
        at the ASKING thread's stack frame, and since clog_flush() blocks
        until ctrl->done, the frame outlives every use of the pointer. The
        exit drain, whose wait is bounded, keeps its request on the heap
        instead (see _clog_exit_drain_async()). The writer thread never frees
        it */
  } u;
} clog_async_msg_t;

/* ========================================================================== */
/*                         RECORD BUILDING                                    */
/* ========================================================================== */

/*
 * A source of fields for a record, which is one of two things. One is a
 * logger's LIVE chmap, used by the synchronous and FATAL paths, which read
 * the fields directly under fields_mutex at the moment they build the
 * record. The other is an array of {key,value} offsets into a flat pool of
 * bytes captured earlier, used by the async path: _snapshot_fields() captures
 * those fields at submission time (its doc comment says why offsets and not
 * raw pointers are stored). One shared loop, _clog_emit_fields, drives both
 * sources, so the three per-format primitives below run exactly the same
 * way whichever mode produced the key and value pairs.
 */
typedef struct {
  bool is_live;
  union {
    struct {
      chmap fields;
      _ccol_mutex_t *mutex;
    } live;
    struct {
      const clog_field_view_t *views;
      size_t count;
      const char *pool;
      bool failed; /* true iff the snapshot itself couldn't be taken (an
          allocation failure in _snapshot_fields()), which is different from
          "this logger really has zero fields" (count == 0, failed == false) */
    } snap;
  } u;
} clog_field_source_t;

static int _append_field_logfmt(clog_buf_t *b, const char *k, const char *v) {
  if (_buf_appendf(b, " %s=", k) != 0) return -1;
  return _buf_append_lv(b, v);
}

static int _append_field_syslog_sd(clog_buf_t *b, const char *k,
                                   const char *v) {
  /* RFC 5424 SD-PARAM-NAME is at most 32 PRINTUSASCII chars. */
  char param_name[33];
  _sd_param_name(k, param_name);
  if (_buf_append(b, " ", 1) != 0) return -1;
  if (_buf_append(b, param_name, strlen(param_name)) != 0) return -1;
  if (_buf_append(b, "=\"", 2) != 0) return -1;
  if (_buf_append_sd_value(b, v) != 0) return -1;
  return _buf_append(b, "\"", 1);
}

/*
 * Emits one field at a time from either field source above, using whichever
 * of the three append primitives matches fmt. Returns false only when the
 * fields can't be listed or emitted at all, which has two causes: an
 * allocation failure, or an append failure in the middle of a field that
 * leaves the field list incomplete. *out_alloc_failure tells "a transient
 * allocation failure" apart from "the record's content is too large", which
 * _clog_build_fallback_record()'s diagnostic message needs to know.
 */
static bool _clog_emit_fields(clog_buf_t *out, clog_format_t fmt,
                              const clog_field_source_t *src,
                              bool *out_alloc_failure) {
  *out_alloc_failure = false;
  int (*append_fn)(clog_buf_t *, const char *, const char *) =
      (fmt == CLOG_FMT_JSON)     ? _buf_append_json_kv
      : (fmt == CLOG_FMT_SYSLOG) ? _append_field_syslog_sd
                                 : _append_field_logfmt;

  if (src->is_live) {
    chmap fields = src->u.live.fields;
    /* The walk runs under the same mutex as every other access to the field
     * map, so a concurrent clog_set_field(), clog_remove_field() or
     * clog_clear_fields() on the same handle can't change the map during it.
     * ccol_chmap_walk_next() allocates nothing and can't fail, so a record
     * with fields costs no allocation here and is never lost to a failed
     * iterator allocation. The fields come out in the order their keys were
     * first set. */
    _ccol_mutex_lock(*src->u.live.mutex);
    ccol_chmap_walk walk = {0};
    const void *kv;
    const void *vv;
    bool ok = true;
    while (ccol_chmap_walk_next(fields, &walk, &kv, &vv)) {
      size_t field_start = out->len;
      if (append_fn(out, (const char *)kv, (const char *)vv) != 0) {
        out->len = field_start; /* roll back partial field */
        ok = false;
        break;
      }
    }
    _ccol_mutex_unlock(*src->u.live.mutex);
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

/*
 * Appends ts, level, proc, src and func in each format's shape. For JSON and
 * logfmt it appends exactly those five fields and leaves nothing open. For
 * syslog it also appends the RFC 5424 preamble (PRI, TIMESTAMP, HOSTNAME,
 * APP-NAME, PROCID and MSGID) and the opening of the
 * "[ccol proc=... src=... func=..." structured data block. That bracket is
 * left open on purpose, because the field loop appends more key="value"
 * params into it, and _clog_build_msg_and_close() below closes it. No I/O
 * happens here. Precondition: the caller holds sh->mutex.
 */
static bool _clog_build_header(clog_buf_t *out, clog_shared_t *sh,
                               clog_format_t fmt, clog_level_t level,
                               const struct timeval *ts, const char *proc_val,
                               const char *file, int line, const char *func) {
  bool ok = true;

  if (fmt == CLOG_FMT_JSON) {
    ok = ok && _buf_append(out, "{\"ts\":\"", 7) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_append(out, "\"", 1) == 0;
    ok = ok && _buf_appendf(out, ",\"level\":\"%s\"", _LEVEL_STR[level]) == 0;
    ok = ok && _buf_append(out, ",\"proc\":\"", 9) == 0;
    ok = ok && _buf_append_json_content(out, proc_val) == 0;
    ok = ok && _buf_append(out, "\"", 1) == 0;
    ok = ok && _buf_append(out, ",\"src\":\"", 8) == 0;
    ok = ok && _buf_append_json_content(out, file) == 0;
    ok = ok && _buf_appendf(out, ":%d\"", line) == 0;
    ok = ok && _buf_append(out, ",\"func\":\"", 9) == 0;
    ok = ok && _buf_append_json_content(out, func) == 0;
    ok = ok && _buf_append(out, "\"", 1) == 0;
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    const char *hostname = _clog_syslog_hostname(sh);
    const char *appname = sh->syslog_appname;
    ok = ok && _buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok &&
         _buf_appendf(out, " %s %s %d %s", hostname, appname,
                      (int)_clog_tident_get()->pid, _LEVEL_STR[level]) == 0;
    ok = ok && _buf_append(out, " [ccol proc=\"", 13) == 0;
    ok = ok && _buf_append_sd_value(out, proc_val) == 0;
    ok = ok && _buf_append(out, "\" src=\"", 7) == 0;
    ok = ok && _buf_append_sd_value(out, file) == 0;
    ok = ok && _buf_appendf(out, ":%d\" func=\"", line) == 0;
    ok = ok && _buf_append_sd_value(out, func) == 0;
    ok = ok && _buf_append(out, "\"", 1) == 0;
  } else {
    ok = ok && _buf_append(out, "ts=", 3) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_appendf(out, " level=%s", _LEVEL_STR[level]) == 0;
    ok = ok && _buf_append(out, " proc=", 6) == 0;
    ok = ok && _buf_append_lv(out, proc_val) == 0;
    ok = ok && _buf_append(out, " src=", 5) == 0;
    {
      /* A long __FILE__ path can make "%s:%d" outgrow this stack buffer, and
       * then it moves to a heap buffer instead of being appended raw and
       * unescaped, so the value still goes through _buf_append_lv()'s logfmt
       * quoting and escaping like every other dynamic field here. */
      char src_stack[512];
      char *src_val = src_stack;
      char *src_heap = NULL;
      int slen = snprintf(src_stack, sizeof(src_stack), "%s:%d", file, line);
      if (slen < 0) {
        ok = false;
      } else if ((size_t)slen >= sizeof(src_stack)) {
        src_heap = _ccol_mem_alloc(sh->m_procs, (size_t)slen + 1);
        if (!src_heap) {
          /* A real allocator failure, not a CLOG_BUF_MAX-style size
           * rejection. Flag it the way _buf_ensure() flags one, so
           * _clog_build_record()'s later out->oom check reports the real
           * cause instead of always blaming the record's size. */
          out->oom = true;
          ok = false;
        } else if (snprintf(src_heap, (size_t)slen + 1, "%s:%d", file, line) <
                   0) {
          ok = false;
        } else {
          src_val = src_heap;
        }
      }
      ok = ok && _buf_append_lv(out, src_val) == 0;
      _ccol_mem_free(sh->m_procs, src_heap);
    }
    ok = ok && _buf_append(out, " func=", 6) == 0;
    ok = ok && _buf_append_lv(out, func) == 0;
  }

  return ok;
}

/*
 * Appends msg and the punctuation that closes the record, and then, when
 * with_backtrace is true, the inline JSON backtrace array. The backtrace
 * frames of logfmt and syslog aren't handled here: the caller appends or
 * writes them strictly after the primary record is written, with
 * _emit_backtrace_lines() for logfmt and _emit_backtrace_syslog_lines() for
 * syslog. Only JSON puts its backtrace inline in the record this function
 * closes. The caller calls this only after appending the fields, so no
 * write() call ever splits the structured data. Precondition: the caller
 * holds sh->mutex.
 */
static bool _clog_build_msg_and_close(clog_buf_t *out, clog_format_t fmt,
                                      const char *msg, bool with_backtrace,
                                      char *const *bt_syms, int bt_depth) {
  bool ok = true;

  if (fmt == CLOG_FMT_JSON) {
    ok = ok && _buf_append(out, ",\"msg\":\"", 8) == 0;
    ok = ok && _buf_append_json_content(out, msg) == 0;
    ok = ok && _buf_append(out, "\"", 1) == 0;
    if (ok && with_backtrace) ok = _emit_backtrace_json(out, bt_syms, bt_depth);
    ok = ok && _buf_append(out, "}\n", 2) == 0;
  } else if (fmt == CLOG_FMT_SYSLOG) {
    ok = ok && _buf_append(out, "] ", 2) == 0;
    ok = ok && _buf_append_ctrl_escaped(out, msg) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
  } else {
    ok = ok && _buf_append(out, " msg=", 5) == 0;
    ok = ok && _buf_append_lv(out, msg) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
  }

  return ok;
}

/*
 * Called after the ordinary record-building sequence fails, which has two
 * separate causes: the record's content can exceed the target buffer's
 * cap_limit (an oversized message or a very large set of field values will
 * do that), or a transient allocation failure, which has nothing to do with
 * size, can keep the record from being completed safely. `alloc_failure`
 * tells the two apart, so the note below reports the real cause instead of
 * always blaming the size.
 *
 * The build starts at the current out->len, which the caller has already
 * rolled back to where this record started. It doesn't reset out to 0
 * itself, because out can be a batch buffer already holding several earlier
 * records from other jobs (the writer thread's async_buf, for example), and
 * resetting would destroy all of them as soon as one later job in the same
 * batch needs the fallback. The two callers that build a single record (the
 * synchronous and FATAL write path, and the failed async enqueue fallback)
 * each reset their own buffer to empty exactly once before building their
 * one record, so for them this placeholder is all the buffer holds.
 *
 * `with_backtrace` matters only for JSON. Logfmt and syslog emit backtrace
 * frames whether or not the primary record fell back to this placeholder, so
 * a requested backtrace is never lost in those two formats. JSON puts its
 * backtrace inline in the record this function replaces, so when
 * with_backtrace is true the same _BT_JSON_ERROR_MARKER the rest of this file
 * uses is appended, and a reader can still tell "backtrace omitted" from
 * "backtrace never requested".
 *
 * Returns true when this fixed-size placeholder went into `out` in full, and
 * false when even the placeholder doesn't fit in the room `out` has left.
 * That can only happen when `out` is the async writer thread's shared
 * aggregation buffer and that buffer already sits right at its own
 * cap_limit. Then `out` is left completely untouched, rolled back to its
 * length on entry, so the caller can flush what's already safely in `out`
 * and try again against an empty buffer. The library never writes a
 * truncated or malformed record.
 */
static bool _clog_build_fallback_record(clog_buf_t *out, clog_shared_t *sh,
                                        clog_format_t fmt, clog_level_t level,
                                        const struct timeval *ts,
                                        const char *msg, const char *proc_val,
                                        bool alloc_failure,
                                        bool with_backtrace) {
  size_t fallback_start = out->len;
  char note[160];
  if (alloc_failure) {
    /* Generic on purpose, because any real allocator failure while building
     * this record raises alloc_failure: the header, a field's value, the
     * message, the field snapshot and the iterator can each raise it, not
     * just a failure to list the fields. See _clog_build_record()'s out->oom
     * check. */
    snprintf(note, sizeof(note),
             "log record dropped: a transient allocation failure prevented "
             "it from being fully built");
  } else {
    /* out->cap_limit and not the fixed CLOG_BUF_MAX constant, because a
     * clog_async_cfg_t.flush_buffer_size the caller sets above CLOG_BUF_MAX
     * raises the async batch buffer's limit above the default (see the doc
     * comment of clog_buf_t.cap_limit). The diagnostic reports the
     * limit in effect for THIS buffer, not the single-record default every
     * other buffer in this file uses. */
    snprintf(note, sizeof(note),
             "log record too large to emit (%zu byte message; %zu byte "
             "limit)",
             strlen(msg), out->cap_limit);
  }

  bool ok = true;
  if (fmt == CLOG_FMT_JSON) {
    ok = ok && _buf_append(out, "{\"ts\":\"", 7) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_appendf(out, "\",\"level\":\"%s\"", _LEVEL_STR[level]) == 0;
    ok = ok && _buf_append(out, ",\"proc\":\"", 9) == 0;
    ok = ok && _buf_append_json_content(out, proc_val) == 0;
    ok = ok && _buf_append(out, "\",\"msg\":\"", 9) == 0;
    ok = ok && _buf_append_json_content(out, note) == 0;
    ok = ok && _buf_append(out, "\"", 1) == 0;
    if (ok && with_backtrace)
      ok = _buf_append(out, _BT_JSON_ERROR_MARKER,
                       sizeof(_BT_JSON_ERROR_MARKER) - 1) == 0;
    ok = ok && _buf_append(out, "}\n", 2) == 0;
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    const char *hostname = _clog_syslog_hostname(sh);
    const char *appname = sh->syslog_appname;
    ok = ok && _buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok &&
         _buf_appendf(out, " %s %s %d %s [ccol proc=\"", hostname, appname,
                      (int)_clog_tident_get()->pid, _LEVEL_STR[level]) == 0;
    ok = ok && _buf_append_sd_value(out, proc_val) == 0;
    ok = ok && _buf_append(out, "\"] ", 3) == 0;
    ok = ok && _buf_append_ctrl_escaped(out, note) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
  } else {
    ok = ok && _buf_append(out, "ts=", 3) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_appendf(out, " level=%s proc=", _LEVEL_STR[level]) == 0;
    ok = ok && _buf_append_lv(out, proc_val) == 0;
    ok = ok && _buf_append(out, " msg=", 5) == 0;
    ok = ok && _buf_append_lv(out, note) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
  }

  if (!ok) out->len = fallback_start; /* leave `out` exactly as found */
  return ok;
}

/*
 * The shared record-building sequence. It drives _clog_build_header(),
 * _clog_emit_fields() and _clog_build_msg_and_close() into out, and on any
 * failure falls back to _clog_build_fallback_record(). Two paths use it: the
 * synchronous and FATAL write path passes a handle's lg->buf as out with
 * field_src in "live" mode, and the async writer thread's per-job work passes
 * sh->async_buf as out with field_src in "snap" mode.
 *
 * For logfmt it appends the requested backtrace frames into the same out
 * buffer right after the primary record, so they land in the same write()
 * or flush as the record. JSON has already put its backtrace inline through
 * _clog_build_msg_and_close(). Syslog's backtrace frames are NEVER appended
 * here, because those frames are exempt from batching; the caller must
 * call _emit_backtrace_syslog_lines() itself, strictly after writing out.
 * Precondition: the caller holds sh->mutex.
 *
 * Returns true once a full record went into `out`, either the real one or
 * the fallback placeholder. It returns false only in the narrow case where
 * not even the fixed-size fallback placeholder fits in the room `out` has
 * left (see the doc comment of _clog_build_fallback_record()), and then it
 * leaves `out` completely untouched: the caller must flush or empty `out`
 * and call this again, and must never treat `out` as holding a usable
 * record.
 *
 * out_used_fallback is set to true whenever the fallback placeholder stands
 * in for the real record, whether or not that substitution itself succeeds,
 * and to false whenever the real record is built as it is.
 * out_used_fallback may be NULL when the caller doesn't need it.
 *
 * A caller building into a batch buffer needs this flag, because that buffer
 * (the async writer thread's sh->async_buf, for example) may already hold
 * OTHER, unrelated records. A true here does NOT always mean the record
 * itself is too large: a small record can also fail to fit in the room that
 * earlier content in the same buffer leaves. The caller should then flush
 * that earlier content and build this record again from an empty buffer,
 * because the library must not silently replace a small, ordinary record
 * with a "too large" note.
 *
 * The two callers that build a single record (the synchronous and FATAL
 * write path, and the failed async enqueue fallback) always build into a
 * buffer they reset to empty right before the call, so for them
 * out_used_fallback can only be true for a record that really is oversized.
 * Both pass NULL.
 */
static bool _clog_build_record(clog_buf_t *out, clog_shared_t *sh,
                               clog_format_t fmt, clog_level_t level,
                               const struct timeval *ts, const char *proc_val,
                               const char *file, int line, const char *func,
                               const char *msg,
                               const clog_field_source_t *field_src,
                               bool with_backtrace, char *const *bt_syms,
                               int bt_depth, bool *out_used_fallback) {
  size_t record_start = out->len;
  /* Reset the flag before this attempt, because an EARLIER, unrelated build
   * on this same buffer, which can live for a long time and serve many
   * builds, may have flagged a real allocator failure. Without the reset that
   * stale flag would leak into this record's alloc_failure verdict below. */
  out->oom = false;
  bool ok =
      _clog_build_header(out, sh, fmt, level, ts, proc_val, file, line, func);
  bool alloc_failure = false;
  if (ok) ok = _clog_emit_fields(out, fmt, field_src, &alloc_failure);
  if (ok)
    ok = _clog_build_msg_and_close(out, fmt, msg, with_backtrace, bt_syms,
                                   bt_depth);
  /* _clog_emit_fields() reports alloc_failure only for a field snapshot or
   * iterator it can't get at all. Two other failures, building the header
   * and one field append in the middle of the loop, go through _buf_ensure()
   * instead, which sets out->oom directly on a real allocator failure and
   * never when only out->cap_limit was hit. Folding out->oom in here, after
   * every build step has had its chance to set it, keeps
   * _clog_build_fallback_record()'s diagnostic accurate whichever of the
   * three build steps ran out of memory, and whichever append inside that
   * step did. */
  if (!alloc_failure) alloc_failure = out->oom;

  /* For LOGFMT the backtrace frames go in as trailing continuation lines in
   * this SAME buffer, right after the primary record the step above closed.
   * The other two formats differ: JSON puts its backtrace INSIDE the record
   * _clog_build_msg_and_close() closed, so a backtrace too large to embed has
   * already made `ok` false through that call, and syslog's frames are
   * always separate records the caller writes itself, which never get here.
   *
   * We try this only while the primary record is still intact, which is what
   * `ok` reports. If the primary content already failed for its own,
   * unrelated reasons, the generic fallback placeholder below replaces it
   * anyway, and the backtrace gets its own separate attempt after THAT
   * placeholder (see below), because an attempt here would run against a
   * buffer state that is about to be thrown away. */
  bool logfmt_bt = with_backtrace && fmt == CLOG_FMT_LOGFMT;
  if (ok && logfmt_bt && !_emit_backtrace_lines(out, bt_syms, bt_depth)) {
    /* Not a single frame, not even the small fixed-size "unavailable"
     * marker, fits in the room left after the primary record (see the doc
     * comment of _emit_backtrace_lines()). That can only happen when `out` is
     * within a few dozen bytes of its own cap_limit. The primary record is
     * fine, but a record that asks for a backtrace must never end up without
     * one and with no trace anywhere, which this function already makes sure
     * of for every OTHER failure mode, so we force the same fallback-record
     * recovery every other build failure gets. `out->oom` shows whether this
     * was a real allocator failure or only the capacity cap, the same as for
     * every other build step above, so the placeholder's diagnostic stays
     * accurate. */
    ok = false;
    if (!alloc_failure) alloc_failure = out->oom;
  }

  if (out_used_fallback) *out_used_fallback = !ok;
  if (!ok) {
    out->len = record_start;
    if (!_clog_build_fallback_record(out, sh, fmt, level, ts, msg, proc_val,
                                     alloc_failure, with_backtrace)) {
      out->len = record_start; /* neither the record nor its fallback fit */
      return false;
    }
    if (logfmt_bt) {
      /* A primary record that is oversized, or failed to build for some
       * other reason, must not also cost the caller its backtrace, so we try
       * again, this time appending the backtrace right after the small
       * fallback placeholder that replaced the primary content, where the
       * buffer has much more room. Failing once more here means not even the
       * marker fits after a placeholder that did fit, which the library
       * accepts as a small gap that can't happen in practice; the doc comment
       * of _clog_write_unrepresentable_record() accepts the same kind of gap
       * for the size of the fallback placeholder. */
      (void)_emit_backtrace_lines(out, bt_syms, bt_depth);
    }
  }
  return true;
}

/*
 * The last resort for the one case _clog_build_record() has no further
 * fallback for: a small, fixed-size placeholder that can't be appended to
 * `out`.
 *
 * For a single-record buffer this can't happen in practice. Both callers
 * below use lg->buf, which is such a buffer: lg->buf is always reset to
 * empty right before the build, and its capacity can never drop below
 * CLOG_BUF_INITIAL, which is much bigger than any fallback record this file
 * builds, so _clog_build_fallback_record() never has to grow the buffer here
 * and can't fail.
 *
 * The function is there so a future change, a smaller CLOG_BUF_INITIAL or a
 * differently sized buffer reused for this purpose, can't turn that
 * guarantee into a silent, zero-byte record loss with no trace anywhere. It
 * writes a minimal line DIRECTLY to the fd, still correct for the format and
 * ending with a newline. It builds the line in a fixed-size stack buffer,
 * and snprintf never allocates, so it bypasses clog_buf_t and the heap
 * machinery completely and can't fail the same way. Precondition: the caller
 * holds sh->mutex, and sh->fd >= 0.
 *
 * with_backtrace keeps the same guarantee _clog_build_record(),
 * _emit_backtrace_lines() and _emit_backtrace_json() give an ordinary
 * record: a caller who asks for a backtrace must never see it vanish without
 * a trace.
 *
 * CLOG_FMT_SYSLOG needs nothing here, because every caller of this function
 * emits syslog's backtrace lines itself with a separate call to
 * _emit_backtrace_syslog_lines() right next to its own call into this
 * function, whatever happens here. CLOG_FMT_JSON puts its own "bt_error"
 * marker straight into the one JSON line built below, which matches
 * _clog_build_fallback_record()'s with_backtrace handling for that format.
 * CLOG_FMT_LOGFMT is the one format whose backtrace lines normally go into
 * the SAME buffer as the primary record, through _emit_backtrace_lines(),
 * but that route is closed once we get here, since getting here means not
 * even the small fallback placeholder fit in that buffer. So logfmt gets its
 * own marker line, which needs no allocation, right after the primary line.
 */
static void _clog_write_unrepresentable_record(clog_shared_t *sh,
                                               clog_format_t fmt,
                                               clog_level_t level,
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
                                  _LEVEL_STR[level], note)
                       : snprintf(line, sizeof(line),
                                  "{\"level\":\"%s\",\"msg\":\"%s\"}\n",
                                  _LEVEL_STR[level], note);
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    n = snprintf(line, sizeof(line), "<%d>1 - - - - - - %s\n", pri, note);
  } else {
    n = snprintf(line, sizeof(line), "level=%s msg=\"%s\"\n", _LEVEL_STR[level],
                 note);
  }
  if (n > 0) {
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    _clog_write_record(sh, fmt, line, len);
  }

  if (with_backtrace && fmt == CLOG_FMT_LOGFMT) {
    _clog_write_record(sh, fmt, _BT_LINE_ERROR_MARKER,
                       sizeof(_BT_LINE_ERROR_MARKER) - 1);
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Calls _clog_write_unrepresentable_record() directly against the shared
 * target of logger, for a test. It follows the same pattern as
 * _clog_test_gzip_compress_file(), exposing a hard-to-reach internal helper
 * rather than trying to build the real conditions that reach it. Getting to
 * this function through the ordinary log_* call path needs not even
 * _clog_build_record()'s small fallback placeholder to fit in the target
 * buffer, and that function's doc comment explains why the current value of
 * CLOG_BUF_INITIAL makes that impossible. So a test that wants to exercise
 * this function's behaviour on its own needs a direct call like this one,
 * without having to really break that invariant.
 */
void _clog_test_write_unrepresentable_record(clog logger, clog_format_t fmt,
                                             clog_level_t level,
                                             bool with_backtrace) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return;
  _ccol_mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    _clog_write_unrepresentable_record(lg->shared, fmt, level, with_backtrace);
  _ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}
#endif

/* ========================================================================== */
/*                         CONSTRUCTOR HELPERS                                */
/* ========================================================================== */

static chmap _fields_create(ccol_memmgmt_procs_t *m_procs) {
  char *err = NULL;
  chmap m = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                            ccol_string, m_procs, &err);
  return m; /* NULL on failure */
}

/*
 * Frees a clog_shared_t that has no live mutex and no logger children. It's
 * safe to call at any point of the initialization inside _shared_alloc()
 * after sh->m_procs is set, which happens right after sh is allocated.
 *
 * It never touches sh->fd itself, whatever sh->owns_fd says, because
 * _shared_alloc()'s early failure paths get here too, and then the caller of
 * _shared_alloc() (_alloc(), and above it clog_create_from_fd_mp() or
 * clog_create_from_file_mp()) already owns closing the fd on ITS OWN failure
 * branch; closing it here as well would close it twice. Every other caller
 * that owns the fd and gets here after that point must close it first,
 * with _shared_close_owned_fd() below.
 */
static void _shared_free_partial(clog_shared_t *sh) {
  _ccol_mem_free(sh->m_procs, sh->file_path);
  ccol_memmgmt_procs_t *mp = sh->m_procs;
  if (mp) {
    ccol_free_t free_fn = mp->free;
    free_fn(mp);
    free_fn(sh);
  } else {
    free(sh);
  }
}

/*
 * Closes sh->fd when this shared object owns it and it's still open, and
 * then marks the fd closed. It also closes sh->dir_fd, a rotating logger's
 * directory descriptor, which the shared object always owns. A file-backed
 * logger from clog_create_from_file_mp() owns its fd. Nothing happens for an
 * fd the caller owns, or for an fd that is already closed;
 * clog_create_from_fd_mp() makes the first case, where owns_fd is false and
 * the caller alone owns the fd's life.
 *
 * clog_close()'s teardown uses it, and so does every constructor rollback
 * path, which must not leak the fd it just opened when a LATER construction
 * step fails (setting up async logging, or acquiring a handle-table slot,
 * for example). _shared_free_partial() itself never does this; see its own
 * doc comment.
 */
static void _shared_close_owned_fd(clog_shared_t *sh) {
  if (sh->owns_fd && sh->fd >= 0) {
    close(sh->fd);
    sh->fd = -1;
  }
  if (sh->dir_fd >= 0) {
    close(sh->dir_fd);
    sh->dir_fd = -1;
  }
}

static clog_shared_t *_shared_alloc(int fd, bool owns_fd, const char *file_path,
                                    int dir_fd, ccol_memmgmt_procs_t *m_procs) {
  clog_shared_t *sh = _ccol_mem_calloc(m_procs, 1, sizeof(*sh));
  if (!sh) return NULL;

  sh->fd = fd;
  sh->owns_fd = owns_fd;
  sh->sink_kind = _clog_classify_sink(fd);
  sh->sink_per_record = _clog_sink_is_message(fd);
  sh->ref_count = 1;
  /* Like fd, dir_fd stays owned by the caller until this function succeeds;
   * see _shared_free_partial(). */
  sh->dir_fd = -1;

  if (m_procs) {
    sh->m_procs = (ccol_memmgmt_procs_t *)m_procs->malloc(sizeof(*m_procs));
    if (!sh->m_procs) {
      m_procs->free(sh);
      return NULL;
    }
    memcpy(sh->m_procs, m_procs, sizeof(*m_procs));
  }

  if (file_path) {
    size_t len = strlen(file_path) + 1;
    sh->file_path = _ccol_mem_alloc(sh->m_procs, len);
    if (!sh->file_path) {
      _shared_free_partial(sh);
      return NULL;
    }
    memcpy(sh->file_path, file_path, len);
    sh->file_base = _path_base(sh->file_path);
  }

  if (_ccol_mutex_init(sh->mutex) != 0) {
    _shared_free_partial(sh);
    return NULL;
  }

  sh->dir_fd = dir_fd;
  sh->last_rotation = _clog_wall_now();

  sh->syslog_facility = CLOG_SYSLOG_USER; /* calloc zeroes to KERN; override */

  /* Cache HOSTNAME for RFC 5424, sanitized the same way as APP-NAME below
   * (see the doc comment of _sanitize_syslog_printusascii_field()). The
   * kernel doesn't guarantee that the raw gethostname(2) value holds only
   * PRINTUSASCII bytes. Without this step a space, a control character or a
   * newline in it would reach the wire completely unescaped, although every
   * other dynamic field in a syslog record is escaped, and a wrong or hostile
   * hostname could then split one record into two. */
  char raw_hostname[256];
  if (gethostname(raw_hostname, sizeof(raw_hostname)) != 0)
    raw_hostname[0] = '\0';
  raw_hostname[sizeof(raw_hostname) - 1] = '\0';
  _sanitize_syslog_printusascii_field(raw_hostname, sh->syslog_hostname,
                                      sizeof(sh->syslog_hostname));

  /* Cache APP-NAME for RFC 5424. The doc comment of
   * _sanitize_syslog_printusascii_field() has the exact filter rule. */
  _sanitize_syslog_printusascii_field(_syslog_appname(), sh->syslog_appname,
                                      sizeof(sh->syslog_appname));

  return sh;
}

static struct clogger *_logger_alloc(clog_shared_t *shared,
                                     clog_level_t min_level) {
  struct clogger *lg = _ccol_mem_calloc(shared->m_procs, 1, sizeof(*lg));
  if (!lg) return NULL;

  if (_buf_init(&lg->buf, shared->m_procs) != 0) {
    _ccol_mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->fields = _fields_create(shared->m_procs);
  if (!lg->fields) {
    _buf_free(&lg->buf);
    _ccol_mem_free(shared->m_procs, lg);
    return NULL;
  }

  if (_ccol_mutex_init(lg->fields_mutex) != 0) {
    __chmap_destroy(lg->fields);
    _buf_free(&lg->buf);
    _ccol_mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->shared = shared;
  lg->min_level = min_level;
  return lg;
}

/* Frees a fully initialised logger without touching the shared store. */
static void _logger_free(struct clogger *lg) {
  _ccol_mutex_destroy(lg->fields_mutex);
  __chmap_destroy(lg->fields);
  lg->fields = NULL;
  _buf_free(&lg->buf);
  _ccol_mem_free(lg->shared->m_procs, lg);
}

static struct clogger *_alloc(int fd, bool owns_fd, const char *file_path,
                              int dir_fd, clog_level_t min_level,
                              ccol_memmgmt_procs_t *m_procs) {
  clog_shared_t *sh = _shared_alloc(fd, owns_fd, file_path, dir_fd, m_procs);
  if (!sh) return NULL;

  struct clogger *lg = _logger_alloc(sh, min_level);
  if (!lg) {
    /* On this failure the caller still owns fd and dir_fd, as on a failure
     * of _shared_alloc(). */
    _ccol_mutex_destroy(sh->mutex);
    _shared_free_partial(sh);
    return NULL;
  }

  return lg;
}

/* ========================================================================== */
/*                         ASYNC LOGGING                                      */
/* ========================================================================== */

/*
 * Copies lg->fields as they are at this exact call into out_fields,
 * out_count and out_pool, under lg->fields_mutex, so a later change or
 * removal on lg can never change the fields already recorded for a
 * submitted job. *out_failed is set to true only on a real allocation
 * failure, and *out_fields stays NULL then. A logger with zero fields
 * succeeds with *out_count == 0.
 */
static void _snapshot_fields(struct clogger *lg, clog_field_view_t **out_fields,
                             size_t *out_count, ccol_growbuf_t *out_pool,
                             bool *out_failed) {
  *out_fields = NULL;
  *out_count = 0;
  *out_failed = false;

  _ccol_mutex_lock(lg->fields_mutex);
  size_t n = chmap_elem_count(lg->fields);
  if (n == 0) {
    _ccol_mutex_unlock(lg->fields_mutex);
    /* ccol_growbuf_init() always allocates a 256-byte backing store, and
     * the common case is a logger with no persistent fields at all, so this
     * branch skips that allocation for it; otherwise every async log call
     * would pay for it only to find nothing to copy. We build the same
     * "valid, empty, nothing appended" shape directly instead.
     * _clog_async_job_release() always calls ccol_growbuf_destroy() later,
     * and that function is documented as safe on exactly this shape, where
     * b->buf can be NULL.
     *
     * It also closes a failure mode, not just the cost on the success path:
     * calling ccol_growbuf_init() here could fail under real memory pressure
     * and leave out_pool->oom set, and since this n == 0 branch returns
     * before checking that flag, the failure would have no way to reach
     * *out_failed. Building the empty shape directly can't fail. */
    out_pool->buf = NULL;
    out_pool->len = 0;
    out_pool->cap = 0;
    out_pool->oom = false;
    out_pool->m_procs = lg->shared->m_procs;
    return;
  }
  ccol_growbuf_init(out_pool, lg->shared->m_procs);

  clog_field_view_t *views =
      _ccol_mem_calloc(lg->shared->m_procs, n, sizeof(*views));
  if (!views) {
    _ccol_mutex_unlock(lg->fields_mutex);
    *out_failed = true;
    return;
  }

  /* fields_mutex is held for the whole walk, so the map can't change and the
   * walk yields exactly n entries. The walk allocates nothing and can't
   * fail. */
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
  _ccol_mutex_unlock(lg->fields_mutex);

  if (out_pool->oom) {
    /* out_pool, which is job->field_pool, is left alone here and must
     * not be destroyed. This function's only caller, _clog_write_async(),
     * always routes the job through _clog_async_job_release() on every
     * success and failure path, and that always destroys job->field_pool
     * exactly once, so destroying it here too would be a double free:
     * ccol_growbuf_destroy() frees out_pool->buf and doesn't set the pointer
     * to NULL afterwards, so the later destroy in _clog_async_job_release()
     * would free the same block a second time. The early return above (the
     * failed allocation of views) leaves out_pool alone for the same
     * reason. */
    _ccol_mem_free(lg->shared->m_procs, views);
    *out_failed = true;
    return;
  }

  *out_fields = views;
  *out_count = n;
}

/* Returns true for a write error that a later attempt on the same
 * descriptor can outlast: the resource is used up for now (ENOSPC, EDQUOT,
 * EFBIG from an RLIMIT_FSIZE ceiling, ENOBUFS, ENOMEM), or the write only had
 * to wait (EAGAIN or EINTR after a bounded wait ran out). 0 stands for a
 * write that stopped without an error, one that made no progress for
 * example. Every other error, EPIPE, ECONNRESET, EBADF or EMSGSIZE for
 * example, stays the same on every retry. */
static bool _clog_write_error_is_transient(int err) {
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

/* Returns where the first `written` bytes of data end, as a value of
 * clog_shared_t.async_partial_record. The caller guarantees
 * 0 < written < len, so data[written] exists. */
static unsigned char _clog_split_after(const char *data, size_t written) {
  if (data[written - 1] != '\n') return CLOG_SPLIT_MID_LINE;
  return data[written] == '\t' ? CLOG_SPLIT_AT_LINE : CLOG_SPLIT_NONE;
}

/*
 * Flushes sh->async_buf to disk when it holds anything, with the same
 * rotation checks as the synchronous write path: a time-based check before
 * the write and a size-based check after it. poll_timeout_ms goes straight
 * to _write_all(); see there for which callers bound it and why.
 *
 * Precondition: the caller holds sh->mutex and calls this only for a target
 * that isn't syslog, because a syslog job flushes itself at once and never
 * accumulates in async_buf; see _clog_writer_thread_main().
 *
 * It never touches sh->last_flush_monotonic. That field belongs to the
 * writer thread's flush schedule, which reads it outside sh->mutex, so only
 * the writer thread may write it, while this function runs on any thread
 * that has to settle a half-written record before it writes (see
 * _clog_flush_and_settle()). _writer_flush_now() is the writer thread's
 * entry point, and it advances the field there.
 *
 * The kernel can take only part of a write, as a filesystem full with
 * ENOSPC, or an RLIMIT_FSIZE ceiling, makes it do. When the error that
 * stopped the write is one a later attempt can outlast (see
 * _clog_write_error_is_transient()), the bytes the kernel didn't take stay
 * in this buffer, moved to the front, and the next flush tries them again.
 * Those bytes are the exact continuation of what landed on disk, so offering
 * them again restores the framing of the record the short write cut in two,
 * and also delivers every record behind it once the condition clears, while
 * throwing them away would lose a whole batch just when a visible log matters
 * most.
 *
 * Any other error is one no retry outlasts: a peer that's gone, an invalid
 * descriptor, a message the sink can never take. Offering the batch again
 * would just fail again while every later record piles up behind it until
 * the buffer hits its ceiling, so the flush drops the undelivered bytes and
 * writes a marker naming the loss (see _clog_note_dropped_batch()), the same
 * thing the synchronous path does for a record it can't deliver.
 *
 * What the buffer keeps can never exceed its own growth ceiling,
 * clog_buf_t.cap_limit, since the kept bytes are bytes the buffer already
 * holds. A record that doesn't fit behind them takes the same path as any
 * other record too large for the batch buffer, which reports itself instead
 * of letting the record vanish.
 *
 * Because the retry is what restores the framing, a kept remainder gets no
 * truncation marker. That only holds while the two halves reach the same
 * file with nothing in between, so a short write that stops inside a record
 * sets clog_shared_t.async_partial_record: both rotation checks then stand
 * down, and any path about to write a different record straight to the fd
 * settles the continuation first (see _clog_flush_and_settle()). The flush
 * that finally drains the buffer clears the flag again.
 *
 * The continuation waits for the retry only while the retry can still come.
 * When the output refused it for lack of room (ENOSPC, EDQUOT, or EFBIG from
 * an RLIMIT_FSIZE ceiling) and a rotation is due, the rotation is what frees
 * room, since it's the step that deletes old generations, and it must not
 * wait for a continuation that only the room it frees can deliver. The flush
 * then gives the continuation up, which the repair's marker names (see
 * _clog_repair_split_record()), and lets the rotation run. A rotation never
 * happens in the middle of a record that will still be delivered.
 *
 * A backlog a refused write left behind can grow far beyond what one file
 * may hold. When the buffer holds more than one batch and its bytes would
 * take the file past max_file_size, the flush writes it in pieces that end
 * on record boundaries, each up to the first record that reaches
 * max_file_size, with the size rotation check between the pieces, so size
 * rotation and retention keep working while the backlog drains. An ordinary
 * flush is one write.
 *
 * While the target owes its output a newline a failed write couldn't
 * deliver (see _clog_pay_owed()), the flush writes nothing and keeps the
 * batch, because the batch would continue the fragment on disk.
 *
 * In every other case the buffer is reset whenever it holds anything,
 * whether or not sh->fd was valid at the time. An sh->fd below 0 can't be
 * seen here in practice, because the fd is closed only after this shared
 * target's writer thread has been joined (see the order in
 * _shared_async_teardown() and clog_close()). Unlike the write and rotation
 * check block below it, that reset doesn't test sh->fd >= 0: should a future
 * change ever break that invariant, the safe failure mode is to drop what's
 * pending, and the unsafe one is an unbounded amount of content the library
 * can never flush, sitting in this buffer forever.
 */
static void _clog_repair_split_record(clog_shared_t *sh, clog_format_t fmt,
                                      int poll_timeout_ms);

/* Returns true for a write error saying the output has no room: a full
 * filesystem, a quota, or an RLIMIT_FSIZE ceiling. */
static bool _clog_write_error_is_no_room(int err) {
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

/* Length of the first piece of the n backlog bytes at data (see
 * _writer_flush_buffer()): up to the first record boundary where the file
 * reaches max_file_size, or all n bytes when the file doesn't reach it, or
 * when it has reached it and the retry backoff keeps the rotation from
 * running. A boundary is a newline not followed by a tab-indented
 * continuation line. The caller holds sh->mutex, and rotation is on. */
static __attribute__((noinline, cold)) size_t
_clog_backlog_piece(const clog_shared_t *sh, const char *data, size_t n) {
  if (!sh->rotation.size_rotation_enabled) return n;
  int64_t room = sh->rotation.max_file_size - (int64_t)sh->bytes_written;
  if (room >= (int64_t)n) return n;
  size_t from = 0;
  if (room > 0)
    from = (size_t)room - 1;
  else if (!_clog_rotate_retry_allowed(sh, _clog_wall_now()))
    return n;
  const char *p = data + from;
  while ((p = memchr(p, '\n', n - (size_t)(p - data))) != NULL) {
    size_t b = (size_t)(p - data) + 1;
    if (b == n || data[b] != '\t') return b;
    p++;
  }
  return n;
}

static void _writer_flush_buffer(clog_shared_t *sh, int poll_timeout_ms) {
  /* A rotation inside this flush, after the old file refused a write, gives
   * the bytes it kept one more try in the new file. */
  bool may_retry = true;
again:
  if (__builtin_expect(sh->async_buf.len == 0 &&
                           (sh->async_partial_record & CLOG_OWED_REPAIR),
                       0)) {
    /* A debt with no batch behind it (a synchronous fallback record that a
     * full output cut, for example) gets paid here too, so it never waits
     * for a record that may never come. */
    if (sh->fd >= 0) (void)_clog_pay_owed(sh, sh->format, poll_timeout_ms);
  }
  if (sh->async_buf.len > 0) {
    bool retained = false;
    int fd_before = sh->fd;
    if (sh->fd >= 0) {
      _clog_time_rotate_if_due(sh);

      if (__builtin_expect(sh->async_partial_record & CLOG_OWED_REPAIR, 0) &&
          !_clog_pay_owed(sh, sh->format, poll_timeout_ms)) {
        /* The newline closing the fragment on disk is still owed, so the
         * batch stays exactly as it is for a later flush. */
        retained = true;
      } else {
        char *data = sh->async_buf.data;
        size_t len = sh->async_buf.len;
        /* off counts how many bytes of the buffer went out. */
        size_t off = 0;
        int write_err;
        for (;;) {
          size_t piece = len - off;
          if (__builtin_expect(sh->rotation_enabled, 0) &&
              piece > sh->async_cfg.flush_buffer_size)
            piece = _clog_backlog_piece(sh, data + off, piece);
          errno = 0;
          size_t written = _write_all(sh, data + off, piece, poll_timeout_ms);
          write_err = errno;
          if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
          off += written;
          if (written < piece || off == len) break;
          /* A piece written whole ends on a record boundary, so the rotation
           * splits no record, and the record an earlier short write split is
           * complete: its split bits are cleared before the check, which
           * stands down while they're set. */
          sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
          _clog_size_rotate_if_due(sh);
        }

        if (off < len) {
          /* The bytes that landed decide whether anything is split at all,
           * so read them here, before the remainder is moved over them. A
           * prefix ending on a record boundary leaves whole records behind
           * and splits nothing, so both rotation checks are free to run. A
           * prefix ending anywhere else is the first part of a record whose
           * remaining bytes are still in this buffer, so both checks have to
           * stand down until the retry delivers them; see
           * clog_shared_t.async_partial_record. A write the kernel took none
           * of moves nothing: the whole batch is still here exactly as it
           * was, so the split bits keep whatever an earlier short write gave
           * them. */
          unsigned char split =
              off > 0
                  ? _clog_split_after(data, off)
                  : (unsigned char)(sh->async_partial_record & CLOG_SPLIT_MASK);
          if (_clog_write_error_is_transient(write_err)) {
            sh->async_partial_record =
                (unsigned char)((sh->async_partial_record & ~CLOG_SPLIT_MASK) |
                                split);
            sh->retain_err = write_err;
            memmove(data, data + off, len - off);
            sh->async_buf.len = len - off;
            retained = true;
            if (split != CLOG_SPLIT_NONE &&
                _clog_write_error_is_no_room(write_err) &&
                _clog_rotation_due(sh)) {
              _clog_repair_split_record(sh, sh->format, poll_timeout_ms);
              _clog_time_rotate_if_due(sh);
            }
          } else {
            char why[48];
            snprintf(why, sizeof(why), "write error %d", write_err);
            sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
            _clog_note_dropped_batch(sh, data + off, len - off, split, why,
                                     write_err, poll_timeout_ms);
          }
        }
      }
    }
    if (!retained) {
      /* Nothing is left undelivered here: either the write took all of it,
       * or the rest can never go out and the marker above names it, or there
       * was no usable fd to offer it to. So no record waits for a
       * continuation that is still on its way. */
      _buf_reset(&sh->async_buf);
      sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
    }
    if (sh->fd >= 0) _clog_size_rotate_if_due(sh);
    if (retained && may_retry && sh->fd >= 0 && sh->fd != fd_before) {
      may_retry = false;
      goto again;
    }
  }
}

/*
 * The writer thread's flush: _writer_flush_buffer() plus the timer
 * bookkeeping the thread's main loop needs. The timestamp advances even when
 * there was nothing to write, because that loop works out its next
 * timed-receive timeout from (now - last_flush_monotonic) on every pass;
 * without the advance, an idle logger would compute a remaining time near
 * zero for ever after an empty timeout and spin instead of really sleeping
 * for flush_interval_us.
 *
 * Precondition: the caller holds sh->mutex. Only the writer thread calls it,
 * since it's the only thread that may write sh->last_flush_monotonic, which
 * it reads outside the mutex.
 */
static void _writer_flush_now(clog_shared_t *sh, int poll_timeout_ms) {
  _writer_flush_buffer(sh, poll_timeout_ms);
  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);
}

/*
 * Gives up on the continuation of a record whose first part is already on
 * disk, and repairs the framing the first part left open. One write carries
 * both the missing newline, when a line is open, and a marker naming the
 * bytes given up on, so a consumer reading the file line by line sees the
 * truncated record and the marker as well-formed lines, and whatever the
 * library writes next starts a line of its own.
 *
 * Only the continuation is given up. Whole records sitting in the batch
 * behind it stay in the buffer and a later flush delivers them: nothing
 * about them is malformed, and what makes them unwritable right now is the
 * descriptor, not the framing.
 *
 * Precondition: the caller holds sh->mutex, sh->fd >= 0, and
 * sh->async_partial_record is set, which means async_buf's leading bytes are
 * that continuation and what's on disk stops inside a record. A flush with
 * no usable descriptor leaves nothing outstanding, so the flag being set is
 * itself proof that the descriptor is usable.
 */
static void _clog_repair_split_record(clog_shared_t *sh, clog_format_t fmt,
                                      int poll_timeout_ms) {
  size_t len = sh->async_buf.len;
  const char *data = sh->async_buf.data;
  /* The continuation ends at the newline of the record's last line. Every
   * line starting with a tab is a continuation line of the record before it
   * (a logfmt backtrace frame), so the continuation runs on through each
   * such line. A remainder with no further newline is the record's tail and
   * nothing else, so all of it is the continuation. */
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
    _buf_reset(&sh->async_buf);
  sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;

  _clog_note_dropped_continuation(sh, fmt, split, dropped, sh->retain_err,
                                  poll_timeout_ms);
}

/*
 * Gives up on every record async_buf still holds after this target's last
 * delivery attempt, and writes a marker naming the loss. A flush keeps the
 * undelivered part of a batch after an error a later attempt can outlast
 * (see _writer_flush_buffer()), but when no later attempt follows, because
 * the target closes or the process stops, those records would otherwise
 * vanish with the buffer with nothing naming them. The caller has already
 * settled any split record, so the bytes on disk end on a record boundary.
 *
 * The caller must hold sh->mutex.
 */
static __attribute__((noinline, cold)) void _clog_drop_undelivered(
    clog_shared_t *sh, const char *why, int poll_timeout_ms) {
  if (sh->async_buf.len == 0) return;
  sh->async_partial_record &= (unsigned char)~CLOG_SPLIT_MASK;
  if (sh->fd >= 0)
    _clog_note_dropped_batch(sh, sh->async_buf.data, sh->async_buf.len,
                             CLOG_SPLIT_NONE, why, 0, poll_timeout_ms);
  _buf_reset(&sh->async_buf);
}

/*
 * Repairs the framing when the flush before it leaves a record split across
 * the write that follows. Every path that writes a record STRAIGHT to sh->fd
 * runs this after its own flush, because the async writer thread's buffer
 * can hold the continuation of a half-written record; without it, that
 * record would land between the two halves of another one, and a consumer of
 * any of this module's three line-framed formats would read three malformed
 * lines where there should be two well-formed ones.
 *
 * The flush before it is the only chance the continuation gets, because a
 * descriptor that just refused the rest of a record won't take it a moment
 * later. Two paths get here, an ordinary log call and CLOG_FATAL on its way
 * to exit(), and neither may wait on a descriptor that never takes it.
 *
 * Then it tries the repair the target owes its output, if any (see
 * _clog_pay_owed()), so a loss marker goes out in front of the record that
 * follows.
 *
 * The caller must hold sh->mutex.
 */
static void _clog_settle_after_flush(clog_shared_t *sh, clog_format_t fmt,
                                     int poll_timeout_ms) {
  if (sh->async_partial_record & CLOG_SPLIT_MASK)
    _clog_repair_split_record(sh, fmt, poll_timeout_ms);
  if (sh->async_partial_record & CLOG_OWED_REPAIR)
    (void)_clog_pay_owed(sh, fmt, poll_timeout_ms);
}

/*
 * _clog_settle_after_flush() plus the delivery attempt it settles after. Two
 * write paths need it, because they don't belong to the writer thread and so
 * have no flush of their own to run first. poll_timeout_ms bounds the one
 * wait inside that attempt that can block on a descriptor with a slow peer;
 * see _write_all().
 *
 * It's out of line on purpose. The check that gets here is one byte of a
 * cache line every write path reads anyway (see
 * clog_shared_t.async_partial_record), and the recovery itself stays out of
 * the caller, so the code shape of the ordinary log path doesn't change.
 *
 * The caller must hold sh->mutex.
 */
static __attribute__((noinline)) void _clog_flush_and_settle(
    clog_shared_t *sh, clog_format_t fmt, int poll_timeout_ms) {
  _writer_flush_buffer(sh, poll_timeout_ms);
  _clog_settle_after_flush(sh, fmt, poll_timeout_ms);
}

/* Frees everything a CLOG_ASYNC_MSG_JOB envelope owns, the envelope itself
 * included. The writer thread's ordinary per-job work and _clog_write_async()'s
 * failed-enqueue fallback both use it. */
static void _clog_async_job_release(clog_shared_t *sh,
                                    clog_async_msg_t *envelope) {
  clog_async_job_t *job = &envelope->u.job;
  if (job->bt_syms)
    free(job->bt_syms); /* This block comes from the malloc inside
backtrace_symbols() and never goes through _ccol_mem_free or a custom
allocator, like every other free of backtrace symbols in this
file */
  ccol_growbuf_destroy(&job->field_pool);
  _ccol_mem_free(sh->m_procs, job->fields);
  _ccol_mem_free(sh->m_procs, job->msg_heap);
  _ccol_mem_free(sh->m_procs, envelope); /* This frees job's storage too,
             since the envelope holds job by value and job is never
             allocated on its own */
}

#ifdef RUNNING_UNIT_TESTS
static _Atomic unsigned int _clog_test_writer_job_delay_us = 0;

void _clog_test_set_writer_job_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_writer_job_delay_us, delay_us);
}

static _Atomic unsigned int _clog_test_writer_unlocked_job_delay_us = 0;

void _clog_test_set_writer_unlocked_job_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_writer_unlocked_job_delay_us, delay_us);
}

/* Holds the writer thread for delay_us. A test comparing a short hold with a
 * bound needs the hold to end on time, and a sleep on a busy runner can end
 * a hundred milliseconds or more late, so a hold of up to 50 ms waits on the
 * clock. A longer one stands in for a slow output, where being late does no
 * harm, and just sleeps. */
static void _clog_test_writer_job_delay(unsigned int delay_us) {
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

/* Waits until no CLOG_FATAL call is waiting for sh's mutex. The fatal call
 * lowers the count as soon as it holds the mutex or gives up on it, which
 * its own bounded wait makes happen in time; nothing else signals here, so
 * the wait polls the count and touches no lock while doing so. */
static __attribute__((noinline, cold)) void _clog_writer_yield_to_fatal(
    clog_shared_t *sh) {
  while (atomic_load(&sh->fatal_waiters) != 0) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 200000L};
    nanosleep(&pause, NULL);
  }
}

/* Locks sh's mutex for each piece of the writer thread's work. A CLOG_FATAL
 * call needs that mutex for its record, but a mutex hands itself to
 * whoever asks again first: a writer thread with a backlog unlocks and locks
 * again at once, so a waiting fatal call would see the mutex free only by
 * luck. So the writer thread steps aside while a fatal call waits. It also
 * brings the thread's signal mask in line with the policy, so a policy
 * change reaches the mask before the next write, whatever kind the sink
 * is. */
static inline void _clog_writer_lock(clog_shared_t *sh) {
  _clog_thread_sync_signal_mask();
  if (__builtin_expect(
          atomic_load_explicit(&sh->fatal_waiters, memory_order_relaxed) != 0,
          0))
    _clog_writer_yield_to_fatal(sh);
  _ccol_mutex_lock(sh->mutex);
}

/*
 * Tells whether the writer thread has nothing to deliver: the batch is
 * empty, no continuation of a split record is waiting, and no repair is owed
 * to the output. The writer thread then waits for a message without a time
 * limit, because a flush at the end of the interval would find nothing to
 * do, and an idle logger never wakes. Every other state keeps the timed
 * wait, so kept bytes and an owed repair get retried once per interval. The
 * caller holds sh->mutex.
 */
static inline bool _clog_writer_has_no_work(const clog_shared_t *sh) {
  return sh->async_buf.len == 0 && sh->async_partial_record == 0;
}

static void *_clog_writer_thread_main(void *arg) {
  clog_shared_t *sh = (clog_shared_t *)arg;
  _clog_thread_serves = sh;
  _clog_thread_signal_init();
  /* true right after a flush started at the top of the loop, so the next
   * pass takes a message even when the interval is already over again, as it
   * is with an interval shorter than one flush. */
  bool interval_flushed = false;
  /* What _clog_writer_has_no_work() said at the end of this thread's last
   * section under sh->mutex. Another thread that leaves work for this one
   * under that mutex wakes it afterwards (see _clog_wake_writer()), so a
   * value read before that work appeared never hides it. */
  bool no_work = true;

  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed_us =
        (long long)(now.tv_sec - sh->last_flush_monotonic.tv_sec) * 1000000LL +
        (now.tv_nsec - sh->last_flush_monotonic.tv_nsec) / 1000L;
    /* The arithmetic stays unsigned and never narrows the interval to a
     * signed type: flush_interval_us is a uint64_t, and a value above
     * LLONG_MAX, such as UINT64_MAX for "flush on size only", would turn into
     * a negative long long, which makes every wait a zero wait so the thread
     * spins on an idle logger. The remaining time is clamped to 0, because
     * the interval may already be over when this pass starts (after a slow
     * job that took longer than the whole interval, for example), and that
     * must mean "check right now". The receive call saturates the deadline
     * it builds from a very long remaining time, so a wait that long is a
     * wait for ever in practice. */
    uint64_t interval_us = sh->async_cfg.flush_interval_us;
    uint64_t remaining_us = elapsed_us <= 0 ? interval_us
                            : (uint64_t)elapsed_us >= interval_us
                                ? 0
                                : interval_us - (uint64_t)elapsed_us;

    /* The interval is over, so flush before taking the next message. A queue
     * producers keep non-empty always has a message for the receive, so a
     * flush that waited for an empty queue or a timeout would never run under
     * sustained load, and only flush_buffer_size would start a write. It's a
     * plain flush for the same reason as the one after a timed-out receive
     * below. */
    if (remaining_us == 0 && !interval_flushed) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&sh->test_writer_interval_flushes, 1);
#endif
      _clog_writer_lock(sh);
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = _clog_writer_has_no_work(sh);
      _ccol_mutex_unlock(sh->mutex);
      interval_flushed = true;
      continue;
    }
    interval_flushed = false;

    /* With nothing to deliver the wait has no limit (UINT64_MAX saturates the
     * deadline), so an idle logger sleeps until a message comes. */
    if (no_work) remaining_us = UINT64_MAX;
    c_message_t m = {0};
    ccol_retval_t rv =
        sh->is_bounded_queue
            ? ccol_circq_timed_recv_zc(sh->q.circq, &m, remaining_us)
            : ccol_dynmq_timed_recv_zc(sh->q.dynmq, &m, remaining_us);

    /* A wait that timed out means the interval is over. A remaining time of 0
     * (right after an interval flush, with an interval shorter than that
     * flush) is a receive that doesn't wait, and an empty queue then answers
     * ccol_container_empty, which means the same thing. */
    if (rv == ccol_timed_out || rv == ccol_container_empty) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&sh->test_writer_timeout_wakeups, 1);
      atomic_fetch_add(&sh->test_writer_interval_flushes, 1);
#endif
      _clog_writer_lock(sh);
      /* A plain flush, never a settle, because this is one of the retries
       * the buffer keeps a continuation for, and nothing is about to be
       * written past it. Giving up on it here would throw away the delivery
       * this wakeup is there to try. */
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = _clog_writer_has_no_work(sh);
      _ccol_mutex_unlock(sh->mutex);
      continue;
    }
    if (rv != ccol_success) {
      /* A real, documented return value, not a theoretical one: the header
       * doc of ccol_circq_timed_recv_zc() and ccol_dynmq_timed_recv_zc() says
       * "ccol_unexpected_failure on system error (check errno)".
       *
       * Just looping back to the timed receive does NOT stop a tight spin by
       * itself, because a receive that fails at once on a system error fails
       * at once again on the next pass. Without the sleep, a persistent
       * system error would spin this thread at 100% CPU on one core for ever,
       * with no backoff.
       *
       * usleep() isn't a pthread or sem primitive, so it needs no wrapper in
       * common.h, like the other direct usleep() calls in this file. A short,
       * fixed sleep bounds the retry rate however long the condition behind
       * it lasts. */
      usleep(1000);
      continue;
    }

    if (m.data == NULL && m.size == 0) {
      /* The shutdown sentinel; see _shared_async_teardown(). The thread
       * drains everything already built and then exits. The sentinel is sent
       * only after every earlier job and flush request was enqueued, so FIFO
       * order makes sure the thread gets to it only after everything ahead of
       * it. */
      _clog_writer_lock(sh);
      /* Settle, not just flush, because this is the last delivery attempt
       * this target ever makes, and a continuation still outstanding after it
       * has no later retry to restore its framing. Without the settle the
       * file would end in the middle of a record and nothing would name the
       * loss. Whole records the flush kept for a retry have no later retry
       * either, so a marker names them. */
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      _clog_settle_after_flush(sh, sh->format, CLOG_WRITE_WAIT_FOREVER);
      _clog_drop_undelivered(sh, "the logger closed", CLOG_WRITE_WAIT_FOREVER);
      _ccol_mutex_unlock(sh->mutex);
      return NULL;
    }

    clog_async_msg_t *envelope = (clog_async_msg_t *)m.data;

    if (envelope->kind == CLOG_ASYNC_MSG_WAKE) {
      /* Another thread left work under the mutex; a plain flush retries it,
       * and the state read here decides how the next wait ends. The wake
       * envelope is static, so there's nothing to free. */
      _clog_writer_lock(sh);
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = _clog_writer_has_no_work(sh);
      _ccol_mutex_unlock(sh->mutex);
      continue;
    }

    if (envelope->kind == CLOG_ASYNC_MSG_FLUSH) {
      clog_async_ctrl_t *ctrl = envelope->u.ctrl;
      _clog_writer_lock(sh);
      /* A plain flush, for the same reason as the timeout branch above: an
       * explicit flush is a delivery attempt, not a deadline. */
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      no_work = _clog_writer_has_no_work(sh);
      _ccol_mutex_unlock(sh->mutex);
      _ccol_mutex_lock(ctrl->mutex);
      ctrl->done = true;
      _ccol_cond_var_broadcast(ctrl->cv);
      _ccol_mutex_unlock(ctrl->mutex);
      /* The asking thread owns a FLUSH envelope; see _clog_flush_pinned() and
       * _clog_exit_drain_async(). Never free it. */
      continue;
    }

    /* CLOG_ASYNC_MSG_JOB */
    clog_async_job_t *job = &envelope->u.job;
#ifdef RUNNING_UNIT_TESTS
    {
      unsigned int delay =
          atomic_load(&_clog_test_writer_unlocked_job_delay_us);
      if (delay) usleep(delay);
    }
#endif
    _clog_writer_lock(sh);
#ifdef RUNNING_UNIT_TESTS
    {
      unsigned int delay = atomic_load(&_clog_test_writer_job_delay_us);
      if (delay) _clog_test_writer_job_delay(delay);
    }
#endif
    if (sh->fd >= 0) {
      clog_format_t fmt = sh->format;
      clog_field_source_t fsrc;
      fsrc.is_live = false;
      fsrc.u.snap.views = job->fields;
      fsrc.u.snap.count = job->field_count;
      fsrc.u.snap.pool = job->field_pool.buf;
      fsrc.u.snap.failed = job->fields_snapshot_failed;

      if (fmt == CLOG_FMT_SYSLOG || sh->sink_per_record) {
        /* Syslog, and every format on a message-oriented socket, is exempt
         * from batching: this one record is built and written directly and
         * never goes into async_buf. A message-oriented socket delivers each
         * write as one message, so a batch would put many records into one
         * message, which a receiver reads as one malformed record, and a
         * batch over the socket's message limit gets refused as a whole. For
         * syslog the job's backtrace frames are then written, each with its
         * own write() call, while logfmt and JSON already carry them inside
         * the record. This matches the synchronous write path exactly, except
         * that the writer thread does the work instead of the submitting
         * thread.
         *
         * async_buf can still hold LOGFMT or JSON batch content no flush has
         * written yet, older than the clog_set_format() call that pointed
         * this shared target at CLOG_FMT_SYSLOG: the format is read fresh for
         * each job rather than captured at submission, and clog_set_format()
         * never touches async_buf. Flushing here, rather than a bare
         * _buf_reset() that would throw that content away unwritten, makes
         * sure a format switch can reorder records already in the batch
         * ahead of this one but can never lose them.
         *
         * When async_buf is already empty, the common case, the call only
         * updates last_flush_monotonic. It's safe here because rotation can
         * never be on for a shared target that gets to this branch: rotation
         * needs a file-backed logger with owns_fd == true, clog_set_format()
         * allows CLOG_FMT_SYSLOG only when owns_fd == false, and a path the
         * library opens is never a socket. So the rotation checks inside
         * _writer_flush_now() always do nothing on this path.
         *
         * Settle, not just flush, because the record goes straight to the fd
         * a few lines below; without the settle, a continuation the flush
         * couldn't deliver would get this record written into the middle of
         * it. */
        _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
        _clog_settle_after_flush(sh, fmt, CLOG_WRITE_WAIT_FOREVER);

        /* A write the kernel takes only in part leaves the bytes it didn't
         * take at the front of async_buf, and the next flush offers them
         * again (see _writer_flush_now()). Those bytes are still
         * undelivered, so this record is built in a private scratch buffer
         * instead of being appended onto them and then reset over them, which
         * would throw them away unwritten. The scratch buffer is allocated
         * only on that path; the common case, an async_buf the flush above
         * emptied, builds straight into async_buf like every other record
         * build in this file. */
        clog_buf_t scratch;
        clog_buf_t *rec_buf = &sh->async_buf;
        if (sh->async_buf.len > 0)
          rec_buf = _buf_init(&scratch, sh->m_procs) == 0 ? &scratch : NULL;

        if (!rec_buf) {
          /* Every buffer we could build into here would destroy the
           * undelivered bytes, so the record is reported the same way as an
           * unrepresentable one, which needs no allocation. The undelivered
           * bytes stay in async_buf and the next flush tries them again. */
          _clog_write_unrepresentable_record(sh, fmt, job->level,
                                             job->with_backtrace);
        } else {
          bool built = _clog_build_record(
              rec_buf, sh, fmt, job->level, &job->ts, job->proc_val, job->file,
              job->line, job->func, job->msg, &fsrc, job->with_backtrace,
              job->bt_syms, job->bt_depth, NULL);
          if (built) {
            _clog_write_record(sh, fmt, rec_buf->data, rec_buf->len);
          } else {
            /* Not even _clog_build_record()'s small, fixed-size fallback
             * placeholder fits in an empty buffer. The doc comment of
             * _clog_write_unrepresentable_record() explains why the record
             * is never silently dropped to zero bytes written instead; every
             * other call site in this file that builds into a buffer does the
             * same. */
            _clog_write_unrepresentable_record(sh, fmt, job->level,
                                               job->with_backtrace);
          }
          _buf_reset(rec_buf);
          if (job->with_backtrace && fmt == CLOG_FMT_SYSLOG)
            _emit_backtrace_syslog_lines(rec_buf, sh, job->level, &job->ts,
                                         job->bt_syms, job->bt_depth);
          if (rec_buf != &sh->async_buf) _buf_free(rec_buf);
        }
      } else {
        /* For logfmt and JSON on a stream sink (a file, a pipe or a stream
         * socket) we append to the batch without resetting it first (the doc
         * comment of _clog_build_fallback_record() says why), and flush once
         * the content reaches the size threshold. */
        size_t record_start = sh->async_buf.len;
        bool batch_had_prior_content = record_start > 0;
        bool used_fallback = false;
        bool built = _clog_build_record(
            &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
            job->file, job->line, job->func, job->msg, &fsrc,
            job->with_backtrace, job->bt_syms, job->bt_depth, &used_fallback);
        if (!built || (used_fallback && batch_had_prior_content)) {
          /* One of two things happened: not even the small, fixed-size
           * fallback placeholder fit in the room async_buf had left, or the
           * fallback was used only because unrelated content from an earlier
           * job already sat in the batch buffer and left too little room for
           * this job's real record, which then needn't be oversized at all.
           * Both can only happen when one record, or what the batch already
           * holds, comes close to async_buf's cap_limit.
           *
           * First roll back to record_start, which throws away the fallback
           * placeholder this call may already have appended (a `built` of
           * true means the placeholder is really in the buffer, not just
           * attempted). THEN flush what was safely accumulated ahead of it and
           * try again. An empty buffer always has more than enough room for an
           * ordinary record, and if the record really is too large on its own,
           * the retry produces the same "too large" fallback from
           * record_start == 0, where that message is accurate.
           *
           * Without the rollback, the flush below would write the misleading
           * fallback placeholder to disk BEFORE the retry runs, and the file
           * would keep both it and the real record instead of the real record
           * alone. The rollback is what keeps one guarantee true for every
           * value of flush_buffer_size: a misleading placeholder never stands
           * in for a record that would have fit on its own. */
          sh->async_buf.len = record_start;
          /* Settle, not just flush, because the retry below can end with a
           * record that goes straight to the fd, and without the settle that
           * record would split a continuation the flush couldn't deliver. */
          _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
          _clog_settle_after_flush(sh, fmt, CLOG_WRITE_WAIT_FOREVER);
          /* This retry runs against a buffer _writer_flush_now() emptied,
           * except after a flush whose own write was cut short, which leaves
           * the undelivered remainder in place (see that function). async_buf's
           * cap stays exactly as it was, since _buf_reset() never shrinks it,
           * and that cap is always at least CLOG_BUF_INITIAL, much bigger than
           * the small fallback placeholder, so the retry always succeeds in
           * practice. The doc comment of _clog_write_unrepresentable_record()
           * gives the same reason for lg->buf on the synchronous and
           * enqueue-failure paths: a single-record buffer that is always reset
           * to empty first, and whose capacity can never drop below
           * CLOG_BUF_INITIAL, never has to grow to fit that placeholder. So
           * this call can't fail with the current constants.
           *
           * The return value gets checked anyway, because ignoring it would
           * drop the record, and any backtrace with it, without a trace as
           * soon as a future change broke that "can't fail" guarantee, as a
           * smaller CLOG_BUF_INITIAL, or a differently sized buffer that got
           * shrunk and then reused here, would. That's the kind of failure
           * _clog_write_unrepresentable_record() is there to guard against at
           * its OTHER two call sites, and checking here, with the same
           * allocation-free last resort that can't fail itself, gives this
           * call site the same guarantee. */
          size_t retry_start = sh->async_buf.len;
          bool retry_fallback = false;
          if (!_clog_build_record(
                  &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
                  job->file, job->line, job->func, job->msg, &fsrc,
                  job->with_backtrace, job->bt_syms, job->bt_depth,
                  retry_start > 0 ? &retry_fallback : NULL)) {
            /* This writes real bytes straight to sh->fd and bypasses
             * async_buf completely, so unlike every other write in this
             * function the size check below never follows it: that check
             * fires only off async_buf's length, which this path never
             * touches. So we check explicitly here; without it this corner
             * case, which can't happen in practice, would let size-based
             * rotation fall behind the file's real size on disk. Every other
             * direct-write call site in this file (_clog_write_sync() and
             * _clog_write_async()'s enqueue-failure fallback) does the same,
             * and both check after their own write, whichever path produced
             * it. */
            _clog_write_unrepresentable_record(sh, fmt, job->level,
                                               job->with_backtrace);
            _clog_size_rotate_if_due(sh);
          } else if (retry_fallback && !sh->async_buf.oom) {
            /* The batch still holds bytes a refused write keeps, and the
             * record didn't fit behind them, so the placeholder would call an
             * ordinary record too large. The placeholder goes, and the record
             * counts as a loss, which a marker names in front of the next
             * write the output accepts. */
            sh->async_buf.len = retry_start;
            size_t mlen = strlen(job->msg);
            char detail[112];
            snprintf(detail, sizeof(detail),
                     "a record did not fit behind the undelivered batch "
                     "(%zu byte message)",
                     mlen);
            _clog_report_loss(sh, fmt, false, 1, mlen, 0, detail,
                              CLOG_WRITE_WAIT_FOREVER);
          }
        }
        if (sh->async_buf.len >= sh->async_cfg.flush_buffer_size)
          _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      }
    }
    no_work = _clog_writer_has_no_work(sh);
    _ccol_mutex_unlock(sh->mutex);

    _clog_async_job_release(sh, envelope);
  }
}

/*
 * Resolves the configuration's defaults, creates the queue, initializes the
 * aggregation buffer and starts the writer thread, rolling back everything
 * already built the moment one step fails. The queue is created first, so
 * there's nothing to unwind when that is the step that fails, and
 * sh->async_enabled is set to true only after every step succeeds, so a
 * half-initialized state is never visible as "enabled".
 *
 * Each public constructor calls this as the very last thing it does, once
 * every other part of its setup is complete, which for
 * clog_create_from_file_mp() includes the block that configures rotation.
 * The writer thread doesn't exist until the rest of the construction is
 * finished, so it never sees sh's fields while the constructing thread is
 * still filling them in.
 */
static bool _shared_async_init(clog_shared_t *sh,
                               const clog_async_cfg_t *async_cfg) {
  sh->async_cfg.queue_size = async_cfg->queue_size;
  sh->async_cfg.flush_buffer_size = async_cfg->flush_buffer_size
                                        ? async_cfg->flush_buffer_size
                                        : CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE;
  sh->async_cfg.flush_interval_us = async_cfg->flush_interval_us
                                        ? async_cfg->flush_interval_us
                                        : CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US;
  sh->is_bounded_queue = (async_cfg->queue_size > 0);

  char *err = NULL; /* Thrown away, because clog_create_from_*_mp gives no error
        string on any failure, which is this file's convention */
  if (sh->is_bounded_queue) {
    sh->q.circq =
        ccol_circular_queue_create_mp(async_cfg->queue_size, sh->m_procs, &err);
    if (!sh->q.circq) return false;
  } else {
    sh->q.dynmq = ccol_dynamic_queue_create_mp(sh->m_procs, &err);
    if (!sh->q.dynmq) return false;
  }

  if (_buf_init(&sh->async_buf, sh->m_procs) != 0) {
    if (sh->is_bounded_queue)
      ccol_circular_queue_destroy(sh->q.circq);
    else
      ccol_dynamic_queue_destroy(sh->q.dynmq);
    return false;
  }
  /* A flush_buffer_size the caller sets above CLOG_BUF_MAX must be
   * honoured, not capped at the smaller default every other clog_buf_t in
   * this file uses, because that default is meant for a single record. See
   * the doc comment of clog_buf_t.cap_limit. */
  _buf_raise_cap_limit(&sh->async_buf, sh->async_cfg.flush_buffer_size);

  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);

  if (_ccol_thread_create(sh->writer_thread, _clog_writer_thread_main, sh) !=
      0) {
    _buf_free(&sh->async_buf);
    if (sh->is_bounded_queue)
      ccol_circular_queue_destroy(sh->q.circq);
    else
      ccol_dynamic_queue_destroy(sh->q.dynmq);
    return false;
  }

  sh->async_enabled = true;
  _clog_exit_drain_arm();
  return true;
}

/*
 * Sends the shutdown sentinel, retrying with a bounded backoff until the
 * queue takes it, and never giving up after one attempt.
 *
 * For a BOUNDED queue one unchecked attempt would do in practice:
 * ccol_circq_send_zc() blocks until space frees up, and the writer thread is
 * still draining at this point, so space always shows up. For the UNBOUNDED
 * queue it wouldn't: ccol_dynmq_send_zc() never blocks, and it can fail with
 * ccol_not_enough_memory when its own internal node allocation fails under
 * memory pressure, which is exactly the situation that can make a caller
 * close loggers.
 *
 * Ignoring that failure and going straight to _ccol_thread_join() would hang
 * for ever, since the writer thread never gets the sentinel it waits for and
 * nothing else would ever send one for this function.
 *
 * The retry is on purpose, because giving up here has no safe fallback. The
 * writer thread can't be abandoned without a join() first: this function's
 * caller is about to free sh and, through _buf_free() and the queue destroy,
 * also the memory the writer thread's next loop pass still touches, so
 * abandoning the thread would turn a hang into a use-after-free and a crash.
 * clog_close() uses the same unbounded poll-wait to wait out a concurrent
 * resolver (see its step 3). Both are there because giving up would corrupt
 * state something else still uses, even though an unbounded wait has its
 * own cost.
 */
static void _clog_send_shutdown_sentinel_blocking(clog_shared_t *sh) {
  unsigned int delay_us = 1000; /* 1ms */
  for (;;) {
    c_message_t sentinel = {.data = NULL, .size = 0};
    ccol_retval_t rv = sh->is_bounded_queue
                           ? ccol_circq_send_zc(sh->q.circq, &sentinel)
                           : ccol_dynmq_send_zc(sh->q.dynmq, &sentinel);
    if (rv == ccol_success) return;
    /* Two failures get here: a transient failure of the unbounded queue's
     * node allocation, and ccol_not_permitted, which _shared_async_teardown()
     * rules out before calling this. We try again, because without the retry
     * the writer thread would wait for a sentinel that never arrives.
     * usleep() isn't a pthread or sem primitive, so it needs no wrapper in
     * common.h; clog_close() uses it the same way. */
    usleep(delay_us);
    if (delay_us < 100000) delay_us *= 2; /* cap backoff at 100ms */
  }
}

/*
 * Joins the writer thread and destroys the queue and async_buf, after first
 * making sure the writer thread was really told to stop (see
 * _clog_send_shutdown_sentinel_blocking() above).
 *
 * Two paths use it: the teardown of the last handle in clog_close(), and
 * each constructor's rollback path, which runs when _clog_handle_acquire()
 * fails after _shared_async_init() has already succeeded. Nobody can submit a
 * job through a handle that doesn't exist yet, so on that path the sentinel
 * is always the only thing in the queue.
 */
static void _shared_async_teardown(clog_shared_t *sh) {
  /* The exit drain turns sends off (see _clog_exit_drain()), and a close
   * after it, from a destructor, still has to reach the writer thread. This
   * is the target's last handle, so no other send can race it. */
  if (sh->is_bounded_queue)
    (void)ccol_circq_enable_sending(sh->q.circq);
  else
    (void)ccol_dynmq_enable_sending(sh->q.dynmq);
  _clog_send_shutdown_sentinel_blocking(sh);
  _ccol_thread_join(sh->writer_thread);
  if (sh->is_bounded_queue)
    ccol_circular_queue_destroy(sh->q.circq);
  else
    ccol_dynamic_queue_destroy(sh->q.dynmq);
  _buf_free(&sh->async_buf);
}

/* ========================================================================== */
/*                         PUBLIC LIFECYCLE                                   */
/* ========================================================================== */

/* The enum setters and the open calls accept only the values their enums
 * name. The comparisons go through int because the compiler may give an enum
 * whose values are all non-negative an unsigned type, and a negative value
 * would then become a large one. */
static bool _clog_level_is_valid(clog_level_t level) {
  int v = (int)level;
  return v >= (int)CLOG_TRACE && v <= (int)CLOG_OFF;
}

static bool _clog_format_is_valid(clog_format_t fmt) {
  int v = (int)fmt;
  return v == (int)CLOG_FMT_LOGFMT || v == (int)CLOG_FMT_JSON ||
         v == (int)CLOG_FMT_SYSLOG;
}

static bool _clog_facility_is_valid(clog_syslog_facility_t facility) {
  int v = (int)facility;
  return (v >= (int)CLOG_SYSLOG_KERN && v <= (int)CLOG_SYSLOG_FTP) ||
         (v >= (int)CLOG_SYSLOG_LOCAL0 && v <= (int)CLOG_SYSLOG_LOCAL7);
}

clog clog_create_from_fd_mp(int fd, clog_level_t min_level,
                            const clog_async_cfg_t *async_cfg,
                            ccol_memmgmt_procs_t *mprocs) {
  if (fd < 0 || !_clog_level_is_valid(min_level)) return CLOG_INVALID;
  int _fd_flags = fcntl(fd, F_GETFL);
  if (_fd_flags == -1) return CLOG_INVALID;
  int _accmode = _fd_flags & O_ACCMODE;
  if (_accmode != O_WRONLY && _accmode != O_RDWR) return CLOG_INVALID;
  if (!_ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) return CLOG_INVALID;
  struct clogger *lg = _alloc(fd, false, NULL, -1, min_level, mprocs);
  if (!lg) return CLOG_INVALID;

  if (async_cfg && !_shared_async_init(lg->shared, async_cfg)) {
    /* Copy this pointer before _logger_free(lg) below frees `lg` itself,
     * because reading lg->shared after that call would be a
     * use-after-free. */
    clog_shared_t *sh = lg->shared;
    _shared_close_owned_fd(sh);
    _ccol_mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }

  clog h = _clog_handle_acquire(lg);
  if (h == CLOG_INVALID) {
    clog_shared_t *sh = lg->shared; /* see the identical note above */
    if (sh->async_enabled) _shared_async_teardown(sh);
    _shared_compressor_teardown(sh);
    _shared_close_owned_fd(sh);
    _ccol_mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }
  return h;
}

/*
 * Seeds sh->last_rotation for a time-rotating logger that opens a file
 * already holding records, so the interval counts from the moment the
 * current file began and not from this open, and a process that runs for
 * less than the interval, or restarts often, still rotates. The best answer
 * available wins:
 *   1. The stamp of the newest rotated generation on disk. The rotation that
 *      made it also created the current file, at that moment.
 *   2. The file's birth time, where the kernel and the filesystem report
 *      one.
 *   3. The file's last modification time.
 * A seed in the future, from a clock that stepped back, starts the interval
 * at the time of the open, so it never runs longer than asked. The newest
 * rotated name step 1 finds is also the one _rotate() needs before its first
 * rotation, so it's recorded here, which saves that listing. Only a
 * generation counts (see clog_gen_filter_t). This listing writes no WARN
 * record, because the open can still end up joining another target of the
 * same file, and the first rotation's deletion pass names every entry it
 * ignores.
 */
static void _clog_seed_last_rotation(clog_shared_t *sh, int fd,
                                     const struct stat *st) {
  time_t seed = st->st_mtime;
  bool seeded = false;
  if (!sh->rot_name_known)
    sh->rot_name_known = _rotated_newest_on_disk(sh, false, sh->rot_last_stamp,
                                                 &sh->rot_last_seq);
  time_t stamp;
  if (sh->rot_name_known && _clog_stamp_to_time(sh->rot_last_stamp, &stamp)) {
    seed = stamp;
    seeded = true;
  }
  /* The birth time comes from statx(2) on Linux and from struct stat on
   * macOS and FreeBSD. A filesystem that keeps none reports 0 on macOS and
   * -1 on FreeBSD, and then the seed stays the modification time. Other
   * systems have no public field for a birth time, so there the seed is the
   * modification time. */
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

static clog _clog_file_register_or_attach(clog h, clog_shared_t *sh,
                                          clog_level_t min_level);

clog clog_create_from_file_mp(const char *path, clog_level_t min_level,
                              const clog_rotation_cfg_t *cfg,
                              const clog_async_cfg_t *async_cfg,
                              ccol_memmgmt_procs_t *mprocs) {
  if (!path || !_clog_level_is_valid(min_level)) return CLOG_INVALID;
  if (!_ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) return CLOG_INVALID;

  /* A rotating logger opens the file's directory first and then the file
   * relative to it, keeps that directory open for its whole life, and every
   * rotation works relative to it. So a relative path names the same file
   * for as long as the logger lives, whatever the process's working
   * directory becomes after this call. */
  int dir_fd = -1;
  int fd;
  if (cfg && (cfg->size_rotation_enabled || cfg->time_rotation_enabled)) {
    dir_fd = _clog_open_log_dir(path);
    if (dir_fd < 0) return CLOG_INVALID;
    fd = openat(dir_fd, _path_base(path),
                O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  } else {
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  }
  if (fd < 0) {
    if (dir_fd >= 0) close(dir_fd);
    return CLOG_INVALID;
  }

  struct clogger *lg = _alloc(fd, true, path, dir_fd, min_level, mprocs);
  if (!lg) {
    close(fd);
    if (dir_fd >= 0) close(dir_fd);
    return CLOG_INVALID;
  }

  /* Identity of the directory entry this target writes; see
   * _clog_file_register_or_attach(). If the directory can't be examined it
   * stays unknown, and the target then stays on its own. */
  {
    int id_fd = dir_fd >= 0 ? dir_fd : _clog_open_log_dir(path);
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

    /* The two substitutions above each sit behind size_rotation_enabled or
     * time_rotation_enabled, but this one doesn't, because pruning applies
     * to the rotated files either mechanism produces, so it's a property of
     * the whole rotation configuration. Most callers get a 0 here by leaving
     * the field at its zero-initialized default, not by choice, and such a
     * caller must not keep rotated files for ever and fill the disk; falling
     * back to CLOG_DEFAULT_MAX_ROTATED_FILES here prevents that. It matches
     * the <= 0 fallback of the other two rotation fields exactly. */
    if (cfg->max_rotated_files <= 0)
      lg->shared->rotation.max_rotated_files = CLOG_DEFAULT_MAX_ROTATED_FILES;

    /* Start the byte counter from the current size on disk, which size-based
     * rotation needs in order to count a file that already exists, isn't
     * empty and gets opened for append; without it the counter would assume
     * the file starts empty.
     *
     * fstat() can hardly fail on an fd this function has just opened, but we
     * don't treat that failure as unreachable: an lseek() to the end of the
     * file is the fallback, which is harmless under O_APPEND, since O_APPEND
     * always appends at the real end of the file whatever offset the fd
     * caches. When size rotation is on and BOTH calls fail, this constructor
     * fails; otherwise it would under-count a possibly large file and let it
     * grow far past the caller's max_file_size before the first rotation
     * fires. */
    struct stat st;
    bool have_size = false;
    if (fstat(fd, &st) == 0) {
      lg->shared->bytes_written = st.st_size;
      have_size = true;
      if (st.st_size > 0 && cfg->time_rotation_enabled)
        _clog_seed_last_rotation(lg->shared, fd, &st);
    } else {
      off_t end = lseek(fd, 0, SEEK_END);
      if (end != (off_t)-1) {
        lg->shared->bytes_written = end;
        have_size = true;
      }
    }
    if (!have_size && cfg->size_rotation_enabled) {
      clog_shared_t *sh = lg->shared; /* see the identical note below */
      _shared_close_owned_fd(sh);
      _ccol_mutex_destroy(sh->mutex);
      _logger_free(lg);
      _shared_free_partial(sh);
      return CLOG_INVALID;
    }
  }

  /* The async setup runs strictly after the rotation configuration block
   * above, never at the same time, because the writer thread must never see
   * sh's rotation fields while this constructing thread is still filling
   * them in. */
  if (async_cfg && !_shared_async_init(lg->shared, async_cfg)) {
    /* Copy this pointer before _logger_free(lg) below frees `lg` itself,
     * because reading lg->shared after that call would be a
     * use-after-free. */
    clog_shared_t *sh = lg->shared;
    _shared_close_owned_fd(sh);
    _ccol_mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }

  clog h = _clog_handle_acquire(lg);
  if (h == CLOG_INVALID) {
    clog_shared_t *sh = lg->shared; /* see the identical note above */
    if (sh->async_enabled) _shared_async_teardown(sh);
    _shared_compressor_teardown(sh);
    _shared_close_owned_fd(sh);
    _ccol_mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }
  return _clog_file_register_or_attach(h, lg->shared, min_level);
}

/*
 * Tears down a shared target whose last handle is gone. clog_close() runs
 * it, and so does an open that attached to a target and then failed after
 * every other handle of that target had closed. The caller holds no lock.
 */
static void _clog_shared_finish_last(clog_shared_t *sh) {
  /* Remove sh from live_shareds under a fresh write lock, strictly BEFORE
   * anything else here touches sh->mutex again; "Fork safety" explains why
   * that order matters. */
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  _clog_shareds_remove(clog_slot_table.live_shareds, sh);
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);

  /* This runs while sh->mutex is NOT held. The writer thread's loop locks
   * that same mutex itself to process everything ahead of the shutdown
   * sentinel and to do the sentinel's final flush, so holding it here across
   * the _ccol_thread_join() inside _shared_async_teardown() would be a
   * self-deadlock: the writer thread could never lock it to get to the
   * sentinel at all.
   *
   * It must also run BEFORE the fd is closed below, because sibling
   * handles that are already closed can still have jobs in the queue, which
   * the writer thread needs to drain and write right now. */
  if (sh->async_enabled) _shared_async_teardown(sh);

  /* This runs after the writer thread has stopped, because that thread can
   * still rotate, and queue a compression, while it drains. It finishes every
   * queued compression before returning, and it runs before dir_fd is closed
   * below, which those compressions use. */
  _shared_compressor_teardown(sh);

  /* A repair the output still owes gets one last bounded attempt, so a loss
   * that happened while the output was full gets named in the log if the
   * output has room again by the time the target closes. */
  _ccol_mutex_lock(sh->mutex);
  if (sh->async_partial_record & CLOG_OWED_REPAIR)
    (void)_clog_pay_owed(sh, sh->format, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
  _ccol_mutex_unlock(sh->mutex);

  _shared_close_owned_fd(sh);
  /* The target leaves file_shareds only once its threads have stopped and
   * its descriptors are closed. Until then, an open of the same file waits
   * for it instead of building a second target that writes the file while
   * this one is still rotating or compressing it. */
  if (sh->file_id_known) {
    _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
    _clog_shareds_remove(clog_slot_table.file_shareds, sh);
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }
  _ccol_mutex_destroy(sh->mutex);
  _shared_free_partial(sh);
}

/*
 * Whether a second target on the same file can join the first one, a: both
 * have the same rotation, async configuration and allocator once defaults
 * are applied.
 */
static bool _clog_file_targets_compatible(const clog_shared_t *a,
                                          const clog_shared_t *b) {
  if (a->rotation_enabled != b->rotation_enabled) return false;
  if (a->rotation_enabled) {
    const clog_rotation_cfg_t *x = &a->rotation, *y = &b->rotation;
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

/* Gives up one reference to sh, which must be alive and referenced, and
 * tears sh down if that was the last one. */
static void _clog_shared_unref(clog_shared_t *sh) {
  _ccol_mutex_lock(sh->mutex);
  int remaining = --sh->ref_count;
  _ccol_mutex_unlock(sh->mutex);
  if (remaining != 0) return;
  /* The teardown uses the slot table's vectors, so it counts as a close in
   * flight, exactly as clog_close() does. */
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight++;
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  _clog_shared_finish_last(sh);
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight--;
  _clog_release_slot_table_if_deferred_locked();
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
}

/*
 * The last step of clog_create_from_file_mp(). h is lg's handle, a new
 * logger whose new target sh writes the file. Two targets writing one file,
 * where at least one of them rotates, lose records, because each one
 * renames, prunes and compresses files the other is still writing. So in a
 * process one file has one target whenever either open rotates.
 *
 * When no other target writes the same directory entry, sh joins
 * file_shareds and h is the result. When one does and either of the two
 * rotates, the new target is closed again, and the open joins the existing
 * target with a new logger at its own level, exactly as clog_derive() joins
 * a target, if the two configurations match (see
 * _clog_file_targets_compatible()); if they differ it fails with
 * CLOG_INVALID. Two targets that both don't rotate stay independent: each
 * appends whole records to the file and neither renames it. An existing
 * target whose last close is still running gets waited for, since it can
 * still rotate and compress.
 */
static clog _clog_file_register_or_attach(clog h, clog_shared_t *sh,
                                          clog_level_t min_level) {
  if (!sh->file_id_known) return h;
  unsigned int delay_us = 1000;
  for (;;) {
    _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
    clog_shared_t *match = NULL;
    size_t n = cvector_elem_count(clog_slot_table.file_shareds);
    for (size_t i = 0; i < n && !match; i++) {
      clog_shared_t *fs =
          *(clog_shared_t **)cvector_at(clog_slot_table.file_shareds, i);
      if (fs->file_dir_dev == sh->file_dir_dev &&
          fs->file_dir_ino == sh->file_dir_ino &&
          strcmp(fs->file_base, sh->file_base) == 0 &&
          (fs->rotation_enabled || sh->rotation_enabled))
        match = fs;
    }
    if (!match) {
      /* The recovery runs before the target is published, under the table
       * lock another opener of the same file needs to find it, so no other
       * handle can rotate this file, and no compression of this target can
       * publish a ".gz" file, while the recovery lists the directory and
       * removes the ".gz" file of each source it saw. Once the target is
       * published, a joined handle could rotate and its compression finish
       * between that listing and that removal, and the removal would then
       * delete a finished output whose source is already gone. */
      if (sh->rotation_enabled) {
        _ccol_mutex_lock(sh->mutex);
        _clog_remove_stale_live_tmps(sh);
        _ccol_mutex_unlock(sh->mutex);
      }
      if (sh->rotation_enabled && sh->rotation.compress_rotated) {
#ifdef RUNNING_UNIT_TESTS
        _clog_test_recover_gate();
#endif
        _ccol_mutex_lock(sh->mutex);
        _clog_recover_compressions(sh);
        _ccol_mutex_unlock(sh->mutex);
      }
      bool ok =
          cvector_push_back(clog_slot_table.file_shareds, &sh) == ccol_success;
      _ccol_rw_lock_unlock(clog_slot_table.rwlock);
      if (!ok) {
        clog_close(h);
        return CLOG_INVALID;
      }
      return h;
    }
    /* The write lock keeps match alive: its last close takes it out of
     * file_shareds under this lock before freeing it. */
    _ccol_mutex_lock(match->mutex);
    int refs = match->ref_count;
    bool compatible = refs > 0 && _clog_file_targets_compatible(match, sh);
    if (compatible) match->ref_count++;
    _ccol_mutex_unlock(match->mutex);
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
    if (refs == 0) {
      /* usleep() isn't a pthread or sem primitive, so it needs no wrapper in
       * common.h; clog_close() waits the same way. */
      usleep(delay_us);
      if (delay_us < 100000) delay_us *= 2;
      continue;
    }
    clog_close(h);
    if (!compatible) return CLOG_INVALID;
    struct clogger *joined = _logger_alloc(match, min_level);
    if (!joined) {
      _clog_shared_unref(match);
      return CLOG_INVALID;
    }
    clog jh = _clog_handle_acquire(joined);
    if (jh == CLOG_INVALID) {
      _logger_free(joined);
      _clog_shared_unref(match);
    }
    return jh;
  }
}

static bool _clog_exit_lock_target(clog_shared_t *sh,
                                   const struct timespec *deadline);
static struct timespec _clog_ts_after_ms(const struct timespec *base,
                                         unsigned int ms);

/* Steps 5 and 6 of clog_close() for a target that an exit drain or a
 * CLOG_FATAL drain left behind because its writer thread didn't answer: that
 * thread may hold sh's mutex for ever, inside a write that never returns.
 * The close takes the mutex only within a short bound, and the last handle
 * leaves the target where it is rather than joining its writer thread; it
 * only takes the target out of the module's tables so nothing finds it
 * again. The process is ending, and whatever the stuck thread still uses
 * stays allocated. */
static void _clog_close_left_target(clog_shared_t *sh) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec deadline =
      _clog_ts_after_ms(&now, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
  if (!_clog_exit_lock_target(sh, &deadline)) return;
  int remaining = --sh->ref_count;
  _ccol_mutex_unlock(sh->mutex);
  if (remaining != 0) return;
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  _clog_shareds_remove(clog_slot_table.live_shareds, sh);
  if (sh->file_id_known) _clog_shareds_remove(clog_slot_table.file_shareds, sh);
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
}

void clog_close(clog h) {
  struct clogger *raw;
  clog_shared_t *sh;

  /* Steps 1-2: resolve and validate, and mark in_use = false, both under the
   * table's write lock, which has to keep a concurrent resolve from starting
   * once the close has begun. A double close is a fatal programming error,
   * the same explicit design as in chttpsvr. */
  {
    _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
    if (h == 0)
      _ccol_fatal_err(
          "clog_close: clog handle is invalid, stale, or already closed");
    uint32_t idx = (uint32_t)(h >> 32);
    uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
    _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
    clog_slot_t *slot = NULL;
    /* Once the module destructor has released the table (slots is NULL), no
     * handle names a logger any more, so the handle is stale. */
    if (clog_slot_table.slots != NULL &&
        idx < cvector_elem_count(clog_slot_table.slots))
      slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
    if (!slot || !slot->in_use || slot->generation != gen) {
      _ccol_rw_lock_unlock(clog_slot_table.rwlock);
      _ccol_fatal_err(
          "clog_close: clog handle is invalid, stale, or already closed");
    }
    raw = slot->ptr;
    sh = raw->shared;
    slot->in_use = false;
    /* The charge runs from here, where nothing can find this close, to the
     * function's last statement, the whole span over which the close still
     * reads the table's vectors. Every exit between those two points is a
     * fatal abort, so the count can't be left stranded. */
    clog_slot_table.closes_in_flight++;
    /* The same step, under the same lock. From here on no new resolve can
     * find this handle, and that's what lets the pin count below reach zero
     * and stay there. */
    ccol_pintable_retire(&clog_pintable, idx);
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }

  /* Step 3: poll-wait until every in-flight operation holding a pin on this
   * exact handle has finished, including one that may still hold
   * raw->fields_mutex, which step 4 below destroys. There's no upper bound,
   * because giving up here would free memory a live resolver still points
   * at. usleep() isn't a pthread or sem primitive, so it needs no wrapper in
   * common.h. */
  {
    uint32_t idx = (uint32_t)(h >> 32);
    unsigned int delay_us = 1;
    while (ccol_pintable_pins(&clog_pintable, idx) > 0) {
      usleep(delay_us);
      if (delay_us < 1000) delay_us *= 2;
    }
  }

#ifdef RUNNING_UNIT_TESTS
  /* Widens the window where in_use is false and freed is false, from here to
   * step 4 below, deterministically and only for a test; see the doc comment
   * of _clog_test_set_close_finalize_delay_us(). Outside a test that arms it,
   * it reads a plain 0 and does nothing. */
  {
    unsigned int d = atomic_load(&_clog_test_close_finalize_delay_us);
    if (d) {
      atomic_store(&_clog_test_close_finalize_delay_entered_state, true);
      usleep(d);
    }
  }
#endif

  /* Step 4: take the write lock again and, under it, destroy raw's
   * per-handle state, fields_mutex included, and retire its slot. This must
   * happen under the SAME lock _clog_atfork_prepare needs before it can even
   * try to lock the fields_mutex of a slot with freed == false, so the two
   * operations can't interleave; "Fork safety" has the full reasoning.
   *
   * This step also does the slot's final bookkeeping: it clears ptr, bumps
   * the generation again and pushes the index onto the free list. There's no
   * reason to put that off until after the teardown of the shared object
   * below, which can be slow. */
  {
    _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
    uint32_t idx = (uint32_t)(h >> 32);
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
    _logger_free(raw);
    slot->freed = true;
    slot->ptr = NULL;
    slot->generation++;
    if (slot->generation == 0) slot->generation++; /* skip the sentinel */
    cvector_push_back(clog_slot_table.free_indices, &idx);
    _ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }

#ifdef RUNNING_UNIT_TESTS
  /* Widens the window where this close has already cleared its own
   * slot->ptr but isn't done with the table yet, deterministically and only
   * for a test; see the doc comment of
   * _clog_test_set_close_release_window_us(). Outside a test that arms it,
   * it reads a plain 0 and does nothing. */
  {
    unsigned int w = atomic_load(&_clog_test_close_release_window_us);
    if (w) {
      atomic_store(&_clog_test_close_release_window_entered_state, true);
      usleep(w);
    }
  }
#endif

  /* Step 5: decrement the shared ref count. The slot table's write lock and
   * shared->mutex are never held together here, because step 4 releases the
   * write lock before this step takes shared->mutex. */
  if (atomic_load_explicit(&sh->drain_left, memory_order_acquire)) {
    _clog_close_left_target(sh);
  } else {
    _ccol_mutex_lock(sh->mutex);
    int remaining = --sh->ref_count;
    _ccol_mutex_unlock(sh->mutex);

    /* Step 6: only the last handle sharing sh really frees it. */
    if (remaining == 0) _clog_shared_finish_last(sh);
  }

  /* This comes last of all. The destructor that runs at process exit can
   * find a logger still open, and then it can't release the table, so the
   * release falls to whichever close is the last one out, and that may be
   * this call. It comes after everything above, because releasing the
   * table destroys live_shareds along with the slot vectors, and step 6
   * walks live_shareds.
   *
   * Doing the release in step 4 instead would merge two independent
   * conditions into one, and an attempt there could never fire: this close
   * charged itself against the table in step 1 and still counts as one of
   * its users, so the table would never be released.
   *
   * Position alone only covers one thread. A concurrent close that has
   * already cleared its own slot is invisible to the "is any slot still live"
   * scan while it sits between its steps 4 and 6, so this call could destroy
   * live_shareds under it; the closes_in_flight count this decrement belongs
   * to is what makes the guard cover that case as well. A sibling handle that
   * is still open keeps the table alive either way. */
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight--;
  _clog_release_slot_table_if_deferred_locked();
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
}

clog clog_derive(clog parent_h) {
  ccol_retval_t rv = ccol_success;
  clog h = _clog_derive_checked(parent_h, &rv);
  if (rv == ccol_invalid_args)
    _ccol_fatal_err(
        "clog_derive: clog handle is invalid, stale, or already closed");
  return h;
}

/* The body of clog_derive(). The parent's pin, taken by the resolve below,
 * keeps the parent and its shared store alive for the whole derive, so a
 * concurrent clog_close() of the parent either comes before the resolve,
 * which then fails, or waits for the unpin at the end. */
clog _clog_derive_checked(clog parent_h, ccol_retval_t *rv) {
  *rv = ccol_invalid_args;
  struct clogger *parent = _clog_resolve(parent_h);
  if (!parent) return CLOG_INVALID;
  *rv = ccol_not_enough_memory;

  /* min_level is _Atomic, so reading it directly here, as _clog_write()'s
   * fast path already does, needs no lock of its own. */
  struct clogger *child = _logger_alloc(parent->shared, parent->min_level);
  if (!child) {
    _clog_resolve_unpin(parent);
    return CLOG_INVALID;
  }

  /* Copy parent's fields into the child's own field map under
   * parent->fields_mutex alone, not shared->mutex. That's the same lock
   * clog_set_field(), clog_remove_field() and clog_clear_fields() take, so
   * the copy is synchronised against a concurrent change to parent's field
   * map without contending with an unrelated write in progress on a sibling
   * logger that shares the same shared->mutex. */
  _ccol_mutex_lock(parent->fields_mutex);
  {
    /* The walk allocates nothing and can't fail; it gives the fields in the
     * order their keys were first set, so the child keeps that order. */
    ccol_chmap_walk walk = {0};
    const void *kv;
    const void *vv;
    while (ccol_chmap_walk_next(parent->fields, &walk, &kv, &vv)) {
      const char *k = (const char *)kv;
      const char *v = (const char *)vv;
      cmap_pair kp = {.ptr = (void *)k, .size = strlen(k) + 1};
      cmap_pair vp = {.ptr = (void *)v, .size = strlen(v) + 1};
      /* ccol_key_already_present is a documented success, meaning the map
       * wrote the value in place, not a failure. Only a real failure, such as
       * ccol_invalid_args or ccol_not_enough_memory, stops the derive. */
      ccol_retval_t rv = chmap_insert_elem(child->fields, &kp, &vp);
      if (rv != ccol_success && rv != ccol_key_already_present) {
        _ccol_mutex_unlock(parent->fields_mutex);
        _logger_free(child);
        _clog_resolve_unpin(parent);
        return CLOG_INVALID;
      }
    }
  }
  _ccol_mutex_unlock(parent->fields_mutex);

  /* The whole derive tree shares ref_count, so it still needs shared->mutex,
   * but the mutex is held only for this short increment. parent stays alive
   * for the whole call, since its own pin is held throughout, so
   * parent->shared isn't destroyed yet and it's safe to take this lock here
   * without holding it from the top of the function. */
  _ccol_mutex_lock(parent->shared->mutex);
  parent->shared->ref_count++;
  _ccol_mutex_unlock(parent->shared->mutex);

  clog h = _clog_handle_acquire(child);
  if (h == CLOG_INVALID) {
    /* child->shared == parent->shared, and parent->shared is still fully
     * alive here because parent's pin is held through this whole function,
     * so reading child->shared->m_procs inside _logger_free(child) below is
     * always safe. */
    _ccol_mutex_lock(parent->shared->mutex);
    parent->shared->ref_count--;
    _ccol_mutex_unlock(parent->shared->mutex);
    _logger_free(child);
    _clog_resolve_unpin(parent);
    return CLOG_INVALID;
  }

  _clog_resolve_unpin(parent);
  *rv = ccol_success;
  return h;
}

/* ========================================================================== */
/*                         LEVEL CONTROL                                      */
/* ========================================================================== */

/*
 * min_level lives on struct clogger rather than clog_shared_t because it's
 * one logger's setting, completely independent of every sibling logger
 * sharing the same target; see the comment on the field itself.
 *
 * Reading or writing it through lg->shared->mutex would serialise
 * clog_set_level() and clog_get_level() on ONE logger against a write, a
 * rotation or a gzip compression handoff on an unrelated SIBLING logger, for
 * no gain in correctness. min_level being atomic is what makes an unlocked
 * read of it race-free, and both _clog_write()'s fast-path check and
 * _clog_write_sync()'s locked re-check rely on that property, not on a lock.
 *
 * A plain read or write of an _Atomic lvalue is sequentially consistent by
 * itself, so this needs no lock at all, not even a lighter one.
 * clog_set_field(), clog_remove_field() and clog_clear_fields() stay off
 * shared->mutex for their own per-logger state the same way, through
 * fields_mutex.
 */
ccol_retval_t clog_set_level(clog h, clog_level_t level) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_set_level: clog handle is invalid, stale, or already closed");

  ccol_retval_t rv = ccol_invalid_args;
  if (_clog_level_is_valid(level)) {
    lg->min_level = level;
    rv = ccol_success;
  }
  _clog_resolve_unpin(lg);
  return rv;
}

clog_level_t clog_get_level(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_get_level: clog handle is invalid, stale, or already closed");

  clog_level_t l = lg->min_level;
  _clog_resolve_unpin(lg);
  return l;
}

/* ========================================================================== */
/*                         OUTPUT FORMAT */
/* ========================================================================== */

ccol_retval_t clog_set_format(clog h, clog_format_t fmt) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_set_format: clog handle is invalid, stale, or already closed");

  if (!_clog_format_is_valid(fmt)) {
    _clog_resolve_unpin(lg);
    return ccol_invalid_args;
  }
  ccol_retval_t rv = ccol_invalid_args;
  _ccol_mutex_lock(lg->shared->mutex);
  /* CLOG_FMT_SYSLOG requires a caller-supplied fd (owns_fd == false). */
  if (fmt != CLOG_FMT_SYSLOG || !lg->shared->owns_fd) {
    lg->shared->format = fmt;
    rv = ccol_success;
  }
  _ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return rv;
}

clog_format_t clog_get_format(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_get_format: clog handle is invalid, stale, or already closed");

  _ccol_mutex_lock(lg->shared->mutex);
  clog_format_t f = lg->shared->format;
  _ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return f;
}

ccol_retval_t clog_set_facility(clog h, clog_syslog_facility_t facility) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_set_facility: clog handle is invalid, stale, or already closed");

  if (!_clog_facility_is_valid(facility)) {
    _clog_resolve_unpin(lg);
    return ccol_invalid_args;
  }
  _ccol_mutex_lock(lg->shared->mutex);
  lg->shared->syslog_facility = facility;
  _ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return ccol_success;
}

clog_syslog_facility_t clog_get_facility(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_get_facility: clog handle is invalid, stale, or already closed");

  _ccol_mutex_lock(lg->shared->mutex);
  clog_syslog_facility_t f = lg->shared->syslog_facility;
  _ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return f;
}

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

ccol_retval_t clog_set_field(clog h, const char *key, const char *value) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_set_field: clog handle is invalid, stale, or already closed");

  /* Reject a key that would corrupt logfmt or RFC 5424 SD output. An RFC 5424
   * SD-PARAM-NAME needs at least one character, and every character must be
   * PRINTUSASCII (0x21 to 0x7e). The c < 0x21 test rejects both a control
   * character and a space in one comparison, and c > 0x7e rejects DEL and
   * every non-ASCII byte from 0x80 to 0xff, which PRINTUSASCII leaves out as
   * well. */
  bool args_ok = key && value && *key != '\0' && !_is_reserved_field_key(key);
  for (const char *p = key; args_ok && *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x21 || c > 0x7e || *p == '=' || *p == '"' || *p == '\\' ||
        *p == ']')
      args_ok = false;
  }
  if (!args_ok) {
    _clog_resolve_unpin(lg);
    return ccol_invalid_args;
  }

  /* This logger's fields_mutex guards this, not shared->mutex, because this
   * logger's field map is completely independent of every sibling sharing the
   * same backing store, so changing a field here must not contend with an
   * unrelated write in progress on a sibling. See the doc comment of
   * fields_mutex on struct clogger. A failed insert leaves the map as it was:
   * a new key isn't added and an existing key keeps its value.
   * ccol_key_already_present reports a replaced value, which counts as
   * success here. */
  _ccol_mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};
  ccol_retval_t rv = chmap_insert_elem(lg->fields, &kp, &vp);

  _ccol_mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
  return rv == ccol_key_already_present ? ccol_success : rv;
}

void clog_remove_field(clog h, const char *key) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_remove_field: clog handle is invalid, stale, or already closed");

  if (!key) {
    _clog_resolve_unpin(lg);
    return;
  }

  _ccol_mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  chmap_delete_elem(lg->fields, &kp);

  _ccol_mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

void clog_clear_fields(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_clear_fields: clog handle is invalid, stale, or already closed");

  _ccol_mutex_lock(lg->fields_mutex);
  chmap_reset(lg->fields, 0);
  _ccol_mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

/* ========================================================================== */
/*                         CORE WRITE                                         */
/* ========================================================================== */

/*
 * The real synchronous body, which builds and writes one record immediately.
 * It walks the live fields under lg->fields_mutex and takes a fresh
 * (fmt, ap) pair, making its own internal va_copy for the vsnprintf retry
 * that moves from the stack buffer to a heap buffer.
 *
 * Two callers use it: _clog_write()'s FATAL and non-async branches, and
 * _clog_write_async()'s fallback for a failed envelope allocation, the one
 * case where nothing async-specific has happened yet, so ap is still
 * completely fresh.
 *
 * bt_syms and bt_depth are inputs the caller has already captured (the doc
 * comment of _capture_backtrace() says why this function never captures them
 * itself). It only reads them and never frees bt_syms; whichever caller
 * captured bt_syms keeps that job.
 *
 * Precondition: NONE. This function takes and releases shared->mutex itself,
 * around its whole body, so a caller must never wrap a call to it in its own
 * lock. Doing that makes the thread wait on a mutex it already holds, and
 * it's an easy mistake to make here because many helpers of this file expect
 * the caller to hold that lock.
 *
 * The __attribute__((format(printf, 9, 0))) lets the internal
 * vsnprintf(msg_stack or msg_heap, ..., fmt, ap) calls below pass fmt and ap
 * straight through. In that attribute fmt is parameter 9, and the 0 says the
 * variadic arguments are already in the va_list ap, so there's no first
 * variadic parameter to name. Without the attribute, Clang's
 * -Wformat-nonliteral warns that fmt isn't a literal at those call sites.
 *
 * fmt really is never a literal here, since it's a parameter of this
 * function that _clog_write() forwards unchanged from its own variadic call,
 * where the compiler already checks the format because clogger.h puts the
 * same attribute on that function. That moves the real check of the format
 * string and the argument types to _clog_write()'s call sites, where fmt is
 * a literal, instead of the two other options: losing the check entirely,
 * or silencing it with a pragma. Both of those are worse.
 */

static bool _clog_exit_lock_target(clog_shared_t *sh,
                                   const struct timespec *deadline);
/* Deadline of a thread whose send a drain refused: the drain's own deadline,
 * and at least CLOG_SPLIT_RECORD_DRAIN_WAIT_MS from now. */
static struct timespec _clog_after_drain_deadline(clog_shared_t *sh);
static bool _clog_fatal_lock_target(clog_shared_t *sh,
                                    const struct timespec *deadline);

/* Writes a CLOG_FATAL call's record: _clog_write_record(), plus whether
 * every byte went out. By then the writes are bounded; see
 * _clog_write_fatal(). */
static __attribute__((noinline, cold)) bool _clog_fatal_write_record(
    clog_shared_t *sh, clog_format_t fmt, const char *data, size_t len) {
  if ((sh->async_partial_record & CLOG_OWED_REPAIR) &&
      !_clog_owed_gate(sh, fmt, len, CLOG_WRITE_WAIT_FOREVER))
    return false;
  size_t written = _write_all(sh, data, len, CLOG_WRITE_WAIT_FOREVER);
  if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  if (written < len)
    _clog_note_truncated_record(sh, fmt, data, written, len, errno);
  return written == len;
}

/*
 * The body of the synchronous write of one record, instantiated twice with
 * `fatal` as a constant: once for every ordinary record and once for a
 * CLOG_FATAL call's record (see _clog_write_fatal()). The fatal
 * instantiation takes the target's mutex within `deadline`, sends a sink
 * that isn't a regular file down the bounded path, and returns false when
 * the record didn't go out whole. The ordinary instantiation carries none of
 * that code.
 */
static inline __attribute__((always_inline, format(printf, 9, 0))) bool
_clog_write_sync_body(struct clogger *lg, clog_level_t level, const char *file,
                      int line, const char *func, bool with_backtrace,
                      char *const *bt_syms, int bt_depth, const char *fmt,
                      va_list ap, va_list ap2, bool fatal,
                      const struct timespec *deadline) {
  bool delivered = true;
  /* The calling thread's identity needs no lock, since it comes from this
   * thread's own cache. */
  const char *proc_val = _clog_tident_get()->proc;

  if (fatal) {
    if (!_clog_fatal_lock_target(lg->shared, deadline)) return false;
    /* From here on, every write to a sink that isn't a regular file ends by
     * the deadline: a pipe, a socket, a terminal and any other device poll(2)
     * reports on can take nothing more. A regular file keeps the ordinary
     * path. */
    struct stat st;
    if (lg->shared->sink_kind != CLOG_SINK_PLAIN ||
        (lg->shared->fd >= 0 && fstat(lg->shared->fd, &st) == 0 &&
         !S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)))
      lg->shared->sink_kind |= CLOG_SINK_FATAL_BOUND;
  } else {
    _ccol_mutex_lock(lg->shared->mutex);
  }

  /* CLOG_FATAL skips the level filter, because the reason for a stop must
   * always be recorded, whatever min_level says.
   *
   * In the common case this re-check repeats _clog_write()'s fast, unlocked
   * check. Both read the same _Atomic min_level, so neither needs
   * shared->mutex to be race-free against a concurrent clog_set_level(). The
   * re-check is there so a level change landing between the fast-path check
   * and this point still applies to this one call, the same rare
   * boundary-line race the fast path itself documents and accepts. */
  if (level < lg->min_level && level != CLOG_FATAL) {
    _ccol_mutex_unlock(lg->shared->mutex);
    return true;
  }

  if (lg->shared->fd < 0) {
    _ccol_mutex_unlock(lg->shared->mutex);
    return false;
  }

  const clog_format_t log_fmt = lg->shared->format;

  /* ----------------------------------------------------------------------- */
  /* Format the user message into a stack buffer; spill to heap if needed.   */
  /* ----------------------------------------------------------------------- */
  char msg_stack[1024];
  char *msg = msg_stack;
  char *msg_heap = NULL;

  int mlen = vsnprintf(msg_stack, sizeof(msg_stack), fmt, ap);

  if (mlen < 0) {
    static const char fmt_err_msg[] = "<log message formatting failed>";
    memcpy(msg_stack, fmt_err_msg, sizeof(fmt_err_msg));
  } else if ((size_t)mlen >= sizeof(msg_stack)) {
    msg_heap = _ccol_mem_alloc(lg->shared->m_procs, (size_t)mlen + 1);
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

  /* ----------------------------------------------------------------------- */
  /* Settle any half-written record before writing on top of it.             */
  /* ----------------------------------------------------------------------- */
  /* This function writes its record straight to the fd. For an async target
   * the last write to that fd may have stopped in the middle of a record,
   * with that record's continuation kept for a later flush, so writing here
   * without settling first would put this record between the two halves of
   * another one. The same byte says when the target owes its output a repair
   * a failed write couldn't make, which the settle tries first. The byte is
   * zero for every target with nothing outstanding, so the common case costs
   * one byte of a cache line this function has already read, plus a branch
   * that falls through. See clog_shared_t.async_partial_record. */
  if (lg->shared->async_partial_record)
    _clog_flush_and_settle(lg->shared, log_fmt,
                           CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);

  /* ----------------------------------------------------------------------- */
  /* Time-based rotation check (before the write).                           */
  /* ----------------------------------------------------------------------- */
  _clog_time_rotate_if_due(lg->shared);

  struct timeval ts;
  gettimeofday(&ts, NULL);

  _buf_reset(&lg->buf);
  clog_field_source_t fsrc;
  fsrc.is_live = true;
  fsrc.u.live.fields = lg->fields;
  fsrc.u.live.mutex = &lg->fields_mutex;
  bool built = _clog_build_record(&lg->buf, lg->shared, log_fmt, level, &ts,
                                  proc_val, file, line, func, msg, &fsrc,
                                  with_backtrace, bt_syms, bt_depth, NULL);

  _ccol_mem_free(lg->shared->m_procs, msg_heap);

  /* ----------------------------------------------------------------------- */
  /* Emit.                                                                    */
  /* ----------------------------------------------------------------------- */
  if (lg->shared->fd >= 0) {
    if (built) {
      if (fatal)
        delivered = _clog_fatal_write_record(lg->shared, log_fmt, lg->buf.data,
                                             lg->buf.len);
      else
        _clog_write_record(lg->shared, log_fmt, lg->buf.data, lg->buf.len);
    } else {
      /* Not even _clog_build_record()'s small, fixed-size fallback
       * placeholder fits in lg->buf. The doc comment of
       * _clog_write_unrepresentable_record() explains why the record is
       * never silently dropped to zero bytes written instead. */
      _clog_write_unrepresentable_record(lg->shared, log_fmt, level,
                                         with_backtrace);
    }
  }

  /* Syslog's backtrace frames are exempt from batching and
   * _clog_build_record() above did NOT append them, so they're written here,
   * strictly after the primary record. JSON has already put its backtrace
   * inline, and logfmt's lines already went into lg->buf above. */
  if (with_backtrace && lg->shared->fd >= 0 && log_fmt == CLOG_FMT_SYSLOG)
    _emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, bt_syms,
                                 bt_depth);

  /* ----------------------------------------------------------------------- */
  /* Size-based rotation check (after the write).                             */
  /* ----------------------------------------------------------------------- */
  _clog_size_rotate_if_due(lg->shared);

  _ccol_mutex_unlock(lg->shared->mutex);
  return delivered;
}

static void __attribute__((format(printf, 9, 0))) _clog_write_sync(
    struct clogger *lg, clog_level_t level, const char *file, int line,
    const char *func, bool with_backtrace, char *const *bt_syms, int bt_depth,
    const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  (void)_clog_write_sync_body(lg, level, file, line, func, with_backtrace,
                              bt_syms, bt_depth, fmt, ap, ap2, false, NULL);
  va_end(ap2);
}

/* A forward declaration: the definition comes further down because it needs
 * clog_async_ctrl_t, clog_async_msg_t and its own test hooks, which appear
 * later in this file. _clog_write_async() below must call it; that
 * function's doc comment says why. */
static void _clog_flush_pinned(struct clogger *lg);

/*
 * The writer thread waits without a time limit while it has nothing to
 * deliver (see _clog_writer_has_no_work()). A thread that writes a record
 * itself, because the record couldn't be queued, can leave work for the
 * writer thread under sh->mutex: whole records a transient write error keeps
 * in the batch, or a repair the output owes. This wakes the writer thread
 * afterwards, so the work gets retried once per interval. The envelope is
 * static and carries nothing, so the send allocates nothing on a bounded
 * queue. A refused send needs nothing more: a full queue wakes the writer
 * thread anyway, sends being off belongs to an exit drain or a teardown that
 * settles the target itself, and when an unbounded queue can't allocate its
 * node, the work waits for the target's next record, flush, close or exit
 * drain.
 */
static clog_async_msg_t _clog_wake_envelope = {.kind = CLOG_ASYNC_MSG_WAKE};

static __attribute__((cold)) void _clog_wake_writer(clog_shared_t *sh) {
  c_message_t m = {.data = &_clog_wake_envelope,
                   .size = sizeof(_clog_wake_envelope)};
  if (sh->is_bounded_queue)
    (void)ccol_circq_try_send_zc(sh->q.circq, &m);
  else
    (void)ccol_dynmq_send_zc(sh->q.dynmq, &m);
}

/*
 * Formats one job and puts it on the writer thread's queue.
 *
 * When allocating the envelope fails, nothing async-specific has happened
 * yet and ap is still fresh, so the whole call goes to _clog_write_sync().
 *
 * A later enqueue can fail too, either because the unbounded queue failed to
 * allocate a node, or with ccol_not_permitted, which every send gets once
 * the exit drain has emptied the target (see _clog_exit_drain()). By then
 * this function's vsnprintf has already consumed ap and ap2, so
 * _clog_write_sync() can't be reused. Instead the record is built and
 * written directly from the job, fully populated by then, under
 * lg->shared->mutex, through the CALLING handle's lg->buf, except that a
 * record joins sh->async_buf behind whole records a transient write error
 * left there (see below). A record is dropped only on the exit paths: when
 * an exit drain left the target (its writer thread doesn't answer), and when
 * a direct write after a refused send can't take the mutex before the
 * drain's deadline. On every other path the record is written, along with
 * any backtrace the caller asked for.
 *
 * Before that direct write, _clog_flush_pinned() drains everything already
 * queued ahead of this job. Without it, this job's record could win the race
 * for lg->shared->mutex against the writer thread and land on disk BEFORE an
 * earlier job the queue has already accepted (from this same handle or a
 * sibling on the same target) that the writer thread hasn't got to yet. That
 * would reorder the log output against submission order, and it's most
 * likely under the same memory pressure that made THIS job's enqueue fail.
 *
 * The queue send inside that flush can fail under the same condition, and
 * then _clog_flush_pinned() returns at once instead of hanging (see its own
 * doc comment). So the flush stays best effort, and a queue that stays broken
 * gets no new way to hang a log call.
 *
 * The __attribute__((format(printf, 9, 0))) matches the one on
 * _clog_write_sync(), for the same reason: fmt is parameter 9, which this
 * function passes on, and it's never a literal at the internal
 * vsnprintf(job->msg_inline or job->msg_heap, ..., fmt, ap) call sites below,
 * so the real check belongs at _clog_write()'s call sites, where the compiler
 * already checks the format.
 */
static void __attribute__((format(printf, 9, 0))) _clog_write_async(
    struct clogger *lg, clog_level_t level, const char *file, int line,
    const char *func, bool with_backtrace, char **bt_syms, int bt_depth,
    const char *fmt, va_list ap) {
  clog_async_msg_t *envelope =
      _ccol_mem_calloc(lg->shared->m_procs, 1, sizeof(*envelope));
  if (!envelope) {
    if (atomic_load_explicit(&lg->shared->drain_left, memory_order_acquire)) {
      /* Same as on the failed send below: the writer thread of a target an
       * exit drain left doesn't answer and may hold the mutex, so a direct
       * write would wait for ever. */
      if (bt_syms) free(bt_syms);
      return;
    }
    /* The queue can hold earlier records of this thread; they go out first,
     * as on the fallback below, so this direct write never lands ahead of
     * them. */
    _clog_flush_pinned(lg);
    _clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                     bt_depth, fmt, ap);
    if (bt_syms) free(bt_syms);
    _clog_wake_writer(lg->shared);
    return;
  }
  envelope->kind = CLOG_ASYNC_MSG_JOB;
  clog_async_job_t *job = &envelope->u.job;

  job->bt_syms = bt_syms; /* the job owns it from here on */
  job->bt_depth = bt_depth;
  job->level = level;
  job->file = file;
  job->line = line;
  job->func = func;
  job->with_backtrace = with_backtrace;
  gettimeofday(&job->ts, NULL);
  _Static_assert(sizeof(job->proc_val) == CLOG_PROC_VAL_LEN,
                 "a job holds the whole cached identity string");
  memcpy(job->proc_val, _clog_tident_get()->proc, CLOG_PROC_VAL_LEN);

  job->msg_heap = NULL;
  va_list ap2;
  va_copy(ap2, ap); /* the fresh copy this function boundary needs, separate
      from _clog_write_sync()'s own internal one; a va_list that crosses a
      function boundary and gets used twice (the stack attempt here, the heap
      retry below) needs its own fresh copy, taken before the first use
      consumes it */
  int mlen = vsnprintf(job->msg_inline, sizeof(job->msg_inline), fmt, ap);
  if (mlen < 0) {
    static const char fmt_err_msg[] = "<log message formatting failed>";
    memcpy(job->msg_inline, fmt_err_msg, sizeof(fmt_err_msg));
  } else if ((size_t)mlen >= sizeof(job->msg_inline)) {
    job->msg_heap = _ccol_mem_alloc(lg->shared->m_procs, (size_t)mlen + 1);
    if (job->msg_heap) {
      int mlen2 = vsnprintf(job->msg_heap, (size_t)mlen + 1, fmt, ap2);
      if (mlen2 < 0) {
        _ccol_mem_free(lg->shared->m_procs, job->msg_heap);
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

  _snapshot_fields(lg, &job->fields, &job->field_count, &job->field_pool,
                   &job->fields_snapshot_failed);

  c_message_t m = {.data = envelope, .size = sizeof(*envelope)};
  ccol_retval_t rv = lg->shared->is_bounded_queue
                         ? ccol_circq_send_zc(lg->shared->q.circq, &m)
                         : ccol_dynmq_send_zc(lg->shared->q.dynmq, &m);
  if (rv != ccol_success &&
      atomic_load_explicit(&lg->shared->drain_left, memory_order_acquire)) {
    /* An exit drain left this target: its writer thread doesn't answer and
     * holds the mutex, so the fallback below would wait for ever. */
    _clog_async_job_release(lg->shared, envelope);
    return;
  }
  if (rv != ccol_success) {
    /* Drain everything already on the queue, from this handle or from any
     * sibling sharing lg->shared, before this job's direct write below, so
     * that write can never land on disk ahead of an earlier job the queue
     * has already accepted and the writer thread hasn't got to yet. See this
     * function's doc comment.
     *
     * The call is safe here: _clog_write(), this function's caller, only gets
     * to _clog_write_async() for a level that isn't FATAL, so this can never
     * nest inside _clog_write()'s FATAL-path flush. */
    _clog_flush_pinned(lg);

    if (rv == ccol_not_permitted) {
      /* A drain turned the sends off, and its writer thread may be stuck in a
       * write that holds the mutex. The wait for that mutex is bounded the
       * same way the flush above is: without the bound this thread would wait
       * for ever, still pinned, and a later clog_close() of the handle (from a
       * destructor, for example) would wait for ever for that pin too. If the
       * lock doesn't come, the record is dropped, as for a target a drain
       * left. */
      struct timespec dl = _clog_after_drain_deadline(lg->shared);
      if (!_clog_exit_lock_target(lg->shared, &dl)) {
        _clog_async_job_release(lg->shared, envelope);
        return;
      }
    } else {
      _ccol_mutex_lock(lg->shared->mutex);
    }
    if (lg->shared->fd >= 0) {
      clog_format_t fmt2 = lg->shared->format;
      clog_field_source_t fsrc;
      fsrc.is_live = false;
      fsrc.u.snap.views = job->fields;
      fsrc.u.snap.count = job->field_count;
      fsrc.u.snap.pool = job->field_pool.buf;
      fsrc.u.snap.failed = job->fields_snapshot_failed;

      /* This record goes straight to sh->fd, like every other real write in
       * this file, so it runs the same rotation checks as every other
       * write path: the time-based check before the write and the size-based
       * one after it. Without them a run of enqueue failures would leave the
       * file far past max_file_size or rotation_interval_us without anything
       * noticing; a single failure is enough in an otherwise idle logger,
       * because this record never touches sh->async_buf, so no later flush
       * can catch it.
       *
       * This record also must not land between the two halves of a record the
       * flush above couldn't finish delivering, the same reason
       * _clog_write_sync() settles before its own write.
       *
       * Nor may it land ahead of whole records the batch still holds, which a
       * flush keeps after a transient write error. The batch gets one more
       * bounded delivery attempt first, and the records it still holds after
       * that stay ahead of this one: for logfmt and JSON on a stream sink this
       * record joins the batch behind them, where the writer thread delivers
       * it in order. A syslog record, and any record for a message-oriented
       * socket, is never batched (see _clog_writer_thread_main()); such a
       * record goes out directly, and so does a record too large to join the
       * batch, and only those can go ahead of records a transient error
       * keeps. */
      clog_shared_t *wsh = lg->shared;
      if (wsh->async_buf.len > 0)
        _clog_flush_and_settle(wsh, fmt2, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
      if (wsh->async_buf.len > 0 && fmt2 != CLOG_FMT_SYSLOG &&
          !wsh->sink_per_record) {
        size_t record_start = wsh->async_buf.len;
        bool used_fallback = false;
        if (_clog_build_record(&wsh->async_buf, wsh, fmt2, job->level, &job->ts,
                               job->proc_val, job->file, job->line, job->func,
                               job->msg, &fsrc, job->with_backtrace,
                               job->bt_syms, job->bt_depth, &used_fallback) &&
            !used_fallback) {
          _ccol_mutex_unlock(wsh->mutex);
          _clog_async_job_release(wsh, envelope);
          return;
        }
        wsh->async_buf.len = record_start;
      }

      _clog_time_rotate_if_due(lg->shared);

      _buf_reset(&lg->buf);
      bool built = _clog_build_record(
          &lg->buf, lg->shared, fmt2, job->level, &job->ts, job->proc_val,
          job->file, job->line, job->func, job->msg, &fsrc, job->with_backtrace,
          job->bt_syms, job->bt_depth, NULL);
      if (built) {
        _clog_write_record(lg->shared, fmt2, lg->buf.data, lg->buf.len);
      } else {
        /* See the doc comment of _clog_write_unrepresentable_record().
         * _clog_write_sync() handles the same condition the same way, and it
         * can't happen in practice. */
        _clog_write_unrepresentable_record(lg->shared, fmt2, job->level,
                                           job->with_backtrace);
      }
      if (job->with_backtrace && fmt2 == CLOG_FMT_SYSLOG)
        _emit_backtrace_syslog_lines(&lg->buf, lg->shared, job->level, &job->ts,
                                     job->bt_syms, job->bt_depth);

      _clog_size_rotate_if_due(lg->shared);
    }
    _ccol_mutex_unlock(lg->shared->mutex);

    _clog_async_job_release(lg->shared, envelope);
    _clog_wake_writer(lg->shared);
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Lets a test force the _ccol_mutex_init() or _ccol_cond_var_init() of the
 * NEXT _clog_flush_pinned() call, on that call's stack-local
 * clog_async_ctrl_t, to fail deterministically, without making
 * pthread_mutex_init() or pthread_cond_init() really fail: with default or
 * NULL attributes, glibc's implementation of those two has no failure path a
 * test could trigger portably. Each hook disarms itself the moment it fires,
 * so it affects only the one call a test targets.
 */
static _Atomic bool _clog_test_force_flush_mutex_init_failure_state = false;
static _Atomic bool _clog_test_force_flush_condvar_init_failure_state = false;

static bool _clog_test_consume_forced_flush_mutex_init_failure(void) {
  return atomic_exchange(&_clog_test_force_flush_mutex_init_failure_state,
                         false);
}
static bool _clog_test_consume_forced_flush_condvar_init_failure(void) {
  return atomic_exchange(&_clog_test_force_flush_condvar_init_failure_state,
                         false);
}

void _clog_test_force_flush_mutex_init_failure(bool force) {
  atomic_store(&_clog_test_force_flush_mutex_init_failure_state, force);
}
void _clog_test_force_flush_condvar_init_failure(bool force) {
  atomic_store(&_clog_test_force_flush_condvar_init_failure_state, force);
}
#else
static inline bool _clog_test_consume_forced_flush_mutex_init_failure(void) {
  return false;
}
static inline bool _clog_test_consume_forced_flush_condvar_init_failure(void) {
  return false;
}
#endif

/*
 * The core of clog_flush(), a function of its own so _clog_write()'s FATAL
 * path can also call it with an lg the caller has ALREADY resolved and
 * pinned. It does no resolve and no unpin of its own, and assumes the caller
 * already knows lg->shared->async_enabled is true.
 *
 * The pin is held for this whole call, the blocking wait below included,
 * just as _clog_write() and _clog_write_async() each hold their own pin
 * across their own blocking ccol_circq_send_zc or ccol_dynmq_send_zc call.
 * That's what lets clog_close()'s poll-wait serialise against a flush in
 * flight, as it already does for a write in flight.
 */
static void _clog_flush_after_drain(clog_shared_t *sh);

static void _clog_flush_pinned(struct clogger *lg) {
  /* Nothing answers a flush of a target an exit drain left. */
  if (atomic_load_explicit(&lg->shared->drain_left, memory_order_acquire))
    return;
  clog_async_ctrl_t ctrl; /* On the stack, one per call, and
              _ccol_mutex_init and _ccol_cond_var_init run on a fresh instance
              every time. No static or constant initializer is ever used,
              which is this file's standing pthread-wrapper rule */
  /* Both of these get checked. The other pthread init calls in this file are
   * ordinary calls that hardly ever fail, but this one is different: calling
   * _ccol_mutex_lock() or _ccol_cond_var_wait() on a mutex or condition
   * variable that is NOT fully initialized is undefined behaviour, not a
   * graceful degradation, and "it almost certainly succeeded" isn't good
   * enough.
   *
   * In that case this function's envelope must also never reach the writer
   * thread, because there would be no properly initialized ctrl.mutex and
   * ctrl.cv for that thread to lock and broadcast on at the other end.
   *
   * On such a failure we treat the call as if it enqueued nothing, the same
   * as the "very unlikely enqueue failure" branch below does. That's also
   * what keeps this function's other caller, _clog_write()'s FATAL path,
   * from hanging on a call that is supposed to stop the process. */
  if (_clog_test_consume_forced_flush_mutex_init_failure() ||
      _ccol_mutex_init(ctrl.mutex) != 0)
    return;
  if (_clog_test_consume_forced_flush_condvar_init_failure() ||
      _ccol_cond_var_init(ctrl.cv) != 0) {
    _ccol_mutex_destroy(ctrl.mutex);
    return;
  }
  ctrl.done = false;

  clog_async_msg_t envelope;
  envelope.kind = CLOG_ASYNC_MSG_FLUSH;
  envelope.u.ctrl = &ctrl;
  c_message_t m = {.data = &envelope, .size = sizeof(envelope)};
  ccol_retval_t rv = lg->shared->is_bounded_queue
                         ? ccol_circq_send_zc(lg->shared->q.circq, &m)
                         : ccol_dynmq_send_zc(lg->shared->q.dynmq, &m);

  if (rv == ccol_success) {
    _ccol_mutex_lock(ctrl.mutex);
    while (!ctrl.done) _ccol_cond_var_wait(ctrl.cv, ctrl.mutex);
    _ccol_mutex_unlock(ctrl.mutex);
  }
  /* The other branches are ccol_not_permitted and ccol_not_enough_memory.
   * Nothing reached the queue for the writer thread to find and acknowledge,
   * so ctrl.done can never become true, and we skip the wait instead of
   * hanging. ccol_not_permitted means an exit drain or a CLOG_FATAL drain
   * turned the sends off. The queue may then still hold records it accepted
   * earlier, which that drain is writing, so a bounded flush through the
   * owner's send waits for them: a record the caller writes directly after
   * this call then never lands ahead of an earlier record of the same
   * thread. */
  _ccol_mutex_destroy(ctrl.mutex);
  _ccol_cond_var_destroy(ctrl.cv);
  if (rv == ccol_not_permitted) _clog_flush_after_drain(lg->shared);
}

void clog_flush(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "clog_flush: clog handle is invalid, stale, or already closed");
  if (lg->shared->async_enabled) _clog_flush_pinned(lg);
  _clog_resolve_unpin(lg);
}

/* The last delivery attempt for an async target's batch, on the fatal path
 * and at exit. The drain before it is a flush that keeps what a transient
 * write error left undelivered, for a retry the process never gets to, so
 * this makes one more delivery attempt with a bounded wait, settles a split
 * record, and names every record still undelivered after it. The caller
 * holds sh's mutex. timeout_ms bounds each write of the attempt and each
 * marker (see _write_all()), so a descriptor that takes nothing (a blocking
 * pipe whose reader stopped reading, among others) can't hold up the
 * process. */
static void _clog_settle_batch_locked(clog_shared_t *sh, int timeout_ms) {
  if (sh->async_buf.len > 0) {
    _writer_flush_buffer(sh, timeout_ms);
    _clog_settle_after_flush(sh, sh->format, timeout_ms);
    _clog_drop_undelivered(sh, "the process stopped", timeout_ms);
  }
}

static void _clog_exit_drain(void);

/* True once the exit drain has been registered with atexit(). */
static bool _clog_exit_drain_registered;

/* The end of a CLOG_FATAL call, after its record is written and its handle
 * unpinned. It takes nothing from the handle, so it runs after the unpin,
 * and every other level's path keeps its shape. exit() runs the exit drain
 * (see _clog_exit_drain()), which gives every other async logger its queue
 * and its batch and lets every queued compression finish, so no rotated file
 * is left as a half-written ".gz" next to its source. That drain bounds each
 * of its waits and takes only the mutex of a target that has something to
 * drain, so a thread that holds another logger's mutex for ever (one whose
 * write to a pipe nobody reads blocks, say) never keeps a fatal error from
 * stopping the process. The drain runs here directly only when its
 * registration failed. */
static __attribute__((noinline, noreturn)) void _clog_fatal_exit(void) {
  if (!_clog_exit_drain_registered) _clog_exit_drain();
  exit(EXIT_FAILURE);
}

/* ========================================================================== */
/*                         EXIT DRAIN                                         */
/* ========================================================================== */

/*
 * A process can end with exit(), or a return from main(), while a logger is
 * still open. Nothing closes it then, and without this drain every record
 * still waiting in an async logger's queue, or in its writer thread's batch,
 * would be lost and a running compression cut off. So an exit handler the
 * library registers when it loads runs, after every exit handler the
 * application registers later, the same drain a fatal record runs: for each
 * async target the queue, then the batch, with a marker for whatever can't
 * be delivered; then every queued and running compression.
 *
 * One budget for the whole drain bounds every wait, including every lock the
 * drain takes (the lock of the table of loggers, and a target's mutex), so a
 * writer thread that can't deliver, other threads that keep logging, a
 * thread that holds a logger's lock and never lets go, or a compression of a
 * big file delay the process's exit by that budget at most. A target whose
 * mutex can't be taken within the budget is skipped, and the whole drain is
 * skipped when the table lock can't be taken in time; whatever records such
 * a target still holds can then be lost. A compression the budget cuts off
 * stops and removes its temporary output, and the log's next open
 * compresses the source again. The target whose writer thread or compressor
 * thread calls exit() is skipped, because that thread can't serve the drain
 * it would wait for.
 */
#define CLOG_EXIT_DRAIN_BUDGET_MS 5000U
/* How long an abandoned compression gets beyond the budget to stop and
 * remove its output. It stops at its next block of input. */
#define CLOG_EXIT_ABANDON_GRACE_MS 1000U

#ifdef RUNNING_UNIT_TESTS
static _Atomic unsigned int _clog_test_exit_drain_budget_ms = 0;

void _clog_test_set_exit_drain_budget_ms(unsigned int budget_ms) {
  atomic_store(&_clog_test_exit_drain_budget_ms, budget_ms);
}

static unsigned int _clog_exit_drain_budget_ms(void) {
  unsigned int b = atomic_load(&_clog_test_exit_drain_budget_ms);
  return b ? b : CLOG_EXIT_DRAIN_BUDGET_MS;
}

void _clog_test_lock_target_forever(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg) return;
  clog_shared_t *sh = lg->shared;
  _clog_resolve_unpin(lg);
  _ccol_mutex_lock(sh->mutex);
}

void _clog_test_lock_table_forever(void) {
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  _ccol_rw_lock_wrlock(clog_slot_table.rwlock);
}

/* While set, the exit drain's send to a bounded queue waits 0 microseconds,
 * as it does once the budget is spent. */
static _Atomic bool _clog_test_exit_drain_send_no_wait = false;

void _clog_test_force_exit_drain_send_without_wait(bool on) {
  atomic_store(&_clog_test_exit_drain_send_no_wait, on);
}

/* The targets the exit drain left alone because their writer thread didn't
 * answer in time. A fixed table is enough for a test. */
#define CLOG_TEST_LEFT_TARGETS_MAX 16
static clog_shared_t *_clog_test_left_targets[CLOG_TEST_LEFT_TARGETS_MAX];
static _Atomic size_t _clog_test_left_targets_count = 0;

static void _clog_test_note_left_target(clog_shared_t *sh) {
  size_t i = atomic_fetch_add(&_clog_test_left_targets_count, 1);
  if (i < CLOG_TEST_LEFT_TARGETS_MAX) _clog_test_left_targets[i] = sh;
}

bool _clog_test_exit_drain_left_target(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg) return false;
  clog_shared_t *sh = lg->shared;
  _clog_resolve_unpin(lg);
  size_t n = atomic_load(&_clog_test_left_targets_count);
  if (n > CLOG_TEST_LEFT_TARGETS_MAX) n = CLOG_TEST_LEFT_TARGETS_MAX;
  for (size_t i = 0; i < n; i++)
    if (_clog_test_left_targets[i] == sh) return true;
  return false;
}
#else
static inline unsigned int _clog_exit_drain_budget_ms(void) {
  return CLOG_EXIT_DRAIN_BUDGET_MS;
}
#endif

/* The monotonic time ms milliseconds after base. */
static struct timespec _clog_ts_after_ms(const struct timespec *base,
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

/* Time from now until deadline, clamped at zero. */
static struct timespec _clog_ts_until(const struct timespec *deadline) {
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

/* Time from now until the monotonic deadline in microseconds, rounded up; 0
 * once the deadline has passed. */
static uint64_t _clog_us_until(const struct timespec *deadline) {
  struct timespec left = _clog_ts_until(deadline);
  return (uint64_t)left.tv_sec * UINT64_C(1000000) +
         ((uint64_t)left.tv_nsec + 999u) / 1000u;
}

/* The CLOCK_REALTIME time at which the monotonic deadline passes. POSIX timed
 * locks measure their timeout on CLOCK_REALTIME, so the drain turns the time
 * it has left into such a time right before each lock. */
static struct timespec _clog_realtime_at(const struct timespec *deadline) {
  struct timespec left = _clog_ts_until(deadline);
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

/* Locks sh's mutex within the deadline. A free lock is taken even when the
 * deadline has already passed. */
static bool _clog_exit_lock_target(clog_shared_t *sh,
                                   const struct timespec *deadline) {
  struct timespec at = _clog_realtime_at(deadline);
  return _ccol_mutex_timedlock(sh->mutex, at) == 0;
}

/* How long a CLOG_FATAL call keeps waiting for its target's mutex once the
 * stop's deadline has passed. The target's writer thread steps aside while
 * the call waits (see _clog_writer_lock()), so a holder that is making
 * progress, a batch write to a regular file for example, hands the mutex over
 * well within this time, while a holder whose write blocks keeps it, and then
 * the call gives up after this long. */
#define CLOG_FATAL_LOCK_GRACE_MS 100U

/* Takes sh's mutex for a CLOG_FATAL call's record, within the deadline or,
 * when that's sooner, within CLOG_FATAL_LOCK_GRACE_MS. The drain before it can
 * use the whole budget on a backlog, and a wait ending at the deadline would
 * then be a single try, which a writer thread in the middle of its work wins,
 * so the fatal record would be lost even against an output that takes it at
 * once. */
static bool _clog_fatal_lock_target(clog_shared_t *sh,
                                    const struct timespec *deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec grace = _clog_ts_after_ms(&now, CLOG_FATAL_LOCK_GRACE_MS);
  const struct timespec *until =
      (deadline->tv_sec > grace.tv_sec ||
       (deadline->tv_sec == grace.tv_sec && deadline->tv_nsec > grace.tv_nsec))
          ? deadline
          : &grace;
  atomic_fetch_add(&sh->fatal_waiters, 1u);
  bool locked = _clog_exit_lock_target(sh, until);
  atomic_fetch_sub(&sh->fatal_waiters, 1u);
  return locked;
}

/* The exit drain's flush request. It lives on the heap because a request the
 * budget gives up on stays in the queue, and the writer thread answers it
 * later. */
typedef struct {
  clog_async_ctrl_t ctrl;
  clog_async_msg_t envelope;
} clog_exit_flush_t;

/* The outcome of _clog_owner_flush(). */
typedef enum {
  CLOG_OWNER_FLUSH_DONE,     /* the writer thread answered */
  CLOG_OWNER_FLUSH_PENDING,  /* it holds the request and did not answer */
  CLOG_OWNER_FLUSH_NOT_SENT, /* the queue stayed full, or refused it */
  CLOG_OWNER_FLUSH_NO_MEMORY /* the request could not be built */
} clog_owner_flush_t;

/* Puts a flush request behind everything the async target sh's queue holds,
 * whether its sends are on or off, and waits until the deadline for the
 * writer thread to answer it. The request lives on the heap, from the
 * target's allocator, because a writer thread answering after the deadline
 * still writes to it; it's then left to the writer thread, and the process
 * ends a moment later. */
static clog_owner_flush_t _clog_owner_flush(clog_shared_t *sh,
                                            const struct timespec *deadline) {
  clog_exit_flush_t *req = _ccol_mem_alloc(sh->m_procs, sizeof(*req));
  if (!req) return CLOG_OWNER_FLUSH_NO_MEMORY;
  _ccol_cond_var_attr_t ca;
  bool ready = false;
  if (_ccol_mutex_init(req->ctrl.mutex) == 0) {
    if (_ccol_cond_var_attr_init(ca) == 0) {
      ready = _ccol_cond_var_attr_setclock(ca, CLOCK_MONOTONIC) == 0 &&
              _ccol_cond_var_init_ca(req->ctrl.cv, ca) == 0;
      _ccol_cond_var_attr_destroy(ca);
    }
    if (!ready) _ccol_mutex_destroy(req->ctrl.mutex);
  }
  if (!ready) {
    _ccol_mem_free(sh->m_procs, req);
    return CLOG_OWNER_FLUSH_NO_MEMORY;
  }
  req->ctrl.done = false;
  req->envelope.kind = CLOG_ASYNC_MSG_FLUSH;
  req->envelope.u.ctrl = &req->ctrl;
  c_message_t m = {.data = &req->envelope, .size = sizeof(req->envelope)};
  ccol_retval_t rv;
  if (sh->is_bounded_queue) {
    /* Once the deadline has passed the send doesn't wait, and a full queue
     * answers ccol_container_full. */
    uint64_t left_us = _clog_us_until(deadline);
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&_clog_test_exit_drain_send_no_wait)) left_us = 0;
#endif
    rv = ccol_circq_owner_send_zc(sh->q.circq, &m, left_us);
  } else {
    rv = ccol_dynmq_owner_send_zc(sh->q.dynmq, &m);
  }
  bool done = false;
  if (rv == ccol_success) {
    _ccol_mutex_lock(req->ctrl.mutex);
    while (!req->ctrl.done &&
           _ccol_cond_var_timedwait(req->ctrl.cv, req->ctrl.mutex, *deadline) ==
               0) {
    }
    done = req->ctrl.done;
    _ccol_mutex_unlock(req->ctrl.mutex);
    if (!done) return CLOG_OWNER_FLUSH_PENDING;
  }
  _ccol_mutex_destroy(req->ctrl.mutex);
  _ccol_cond_var_destroy(req->ctrl.cv);
  _ccol_mem_free(sh->m_procs, req);
  return done ? CLOG_OWNER_FLUSH_DONE : CLOG_OWNER_FLUSH_NOT_SENT;
}

/* Drains the async target sh's queue, then its batch, within the deadline.
 * Returns false when the writer thread didn't answer in time, and then that
 * thread can hold sh's mutex for as long as one of its writes blocks. The
 * caller keeps sh alive: the exit drain holds the slot table's write lock,
 * and a CLOG_FATAL call holds a pin on its handle.
 *
 * Sends are turned off BEFORE the flush request goes in, and the request
 * goes in through the owner's send, so it's the last message in the queue:
 * every record the queue accepted lies ahead of it and gets written, and on
 * a target the drain empties every later record takes _clog_write_async()'s
 * synchronous fallback. In the opposite order, records other threads log
 * between the request and the switch would wait behind the request, and the
 * end of the process would throw them away. */
static bool _clog_exit_drain_async(clog_shared_t *sh,
                                   const struct timespec *deadline) {
  atomic_store_explicit(
      &sh->drain_deadline_ns,
      (uint64_t)deadline->tv_sec * 1000000000ull + (uint64_t)deadline->tv_nsec,
      memory_order_relaxed);
  if (sh->is_bounded_queue)
    (void)ccol_circq_disable_sending(sh->q.circq);
  else
    (void)ccol_dynmq_disable_sending(sh->q.dynmq);
  clog_owner_flush_t r = _clog_owner_flush(sh, deadline);
  if (r == CLOG_OWNER_FLUSH_NO_MEMORY) return true;
  /* A writer thread that didn't answer, or a bounded queue that stayed full
   * until the deadline, means a writer thread that can't keep up, and the
   * batch is left to it too. A thread holding sh's mutex past the deadline
   * keeps the batch as well, and the drain then leaves the target the same
   * way. A record logged after this point must not take the synchronous
   * fallback, whose lock of sh's mutex has no bound while the stuck thread
   * holds that mutex, and must not wait for room in a full bounded queue
   * either. An unbounded queue gets its sends back and takes every later
   * record at once. A bounded queue keeps its sends off, so a later send
   * fails at once and drain_left makes _clog_write_async() drop that
   * record. */
  if (r != CLOG_OWNER_FLUSH_DONE || !_clog_exit_lock_target(sh, deadline)) {
    atomic_store_explicit(&sh->drain_left, true, memory_order_release);
    if (!sh->is_bounded_queue) (void)ccol_dynmq_enable_sending(sh->q.dynmq);
    return false;
  }
  int left = _clog_ms_until(deadline);
  int wait_ms = left < CLOG_SPLIT_RECORD_DRAIN_WAIT_MS
                    ? left
                    : CLOG_SPLIT_RECORD_DRAIN_WAIT_MS;
  _clog_settle_batch_locked(sh, wait_ms);
  /* A debt with no batch behind it gets paid here, under the lock the drain
   * holds, so it never depends on a later write. */
  if (sh->async_partial_record & CLOG_OWED_REPAIR)
    (void)_clog_pay_owed(sh, sh->format, wait_ms);
  _ccol_mutex_unlock(sh->mutex);
  return true;
}

/* The flush of a thread whose send a drain refused: it waits for the records
 * sh's queue accepted before the drain turned its sends off, until that
 * drain's deadline and for at least CLOG_SPLIT_RECORD_DRAIN_WAIT_MS, so a
 * record logged after the drain (from a destructor, for example) still finds
 * a writer thread that answers right away. */
static struct timespec _clog_after_drain_deadline(clog_shared_t *sh) {
  uint64_t ns =
      atomic_load_explicit(&sh->drain_deadline_ns, memory_order_relaxed);
  struct timespec deadline = {(time_t)(ns / 1000000000ull),
                              (long)(ns % 1000000000ull)};
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  struct timespec floor_at =
      _clog_ts_after_ms(&now, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
  if (floor_at.tv_sec > deadline.tv_sec ||
      (floor_at.tv_sec == deadline.tv_sec &&
       floor_at.tv_nsec > deadline.tv_nsec))
    deadline = floor_at;
  return deadline;
}

static void _clog_flush_after_drain(clog_shared_t *sh) {
  struct timespec deadline = _clog_after_drain_deadline(sh);
  (void)_clog_owner_flush(sh, &deadline);
}

/* Waits within the deadline until sh's compressor thread has finished every
 * queued and running compression. At the deadline it drops the queued jobs,
 * whose sources stay uncompressed, and stops the running one. The caller
 * holds the slot table's write lock. */
static void _clog_exit_drain_compressions(clog_shared_t *sh,
                                          const struct timespec *deadline) {
  /* A thread holding the mutex past the deadline leaves the queued and the
   * running compressions to the compressor thread, which the end of the
   * process stops wherever it happens to be. */
  if (!_clog_exit_lock_target(sh, deadline)) return;
  if (sh->compressor_live) {
    while ((sh->compress_head || sh->compress_running) &&
           _ccol_cond_var_timedwait(sh->compress_cv, sh->mutex, *deadline) ==
               0) {
    }
    if (sh->compress_head || sh->compress_running) {
      clog_compress_job_t *j = sh->compress_head;
      while (j) {
        clog_compress_job_t *next = j->next;
        _ccol_mem_free(sh->m_procs, j);
        j = next;
      }
      sh->compress_head = NULL;
      sh->compress_tail = NULL;
      atomic_store(&sh->compress_abort, true);
      struct timespec grace =
          _clog_ts_after_ms(deadline, CLOG_EXIT_ABANDON_GRACE_MS);
      while (sh->compress_running &&
             _ccol_cond_var_timedwait(sh->compress_cv, sh->mutex, grace) == 0) {
      }
    }
  }
  _ccol_mutex_unlock(sh->mutex);
}

static void _clog_exit_drain(void) {
  if (!atomic_load_explicit(&_clog_exit_drain_armed, memory_order_relaxed))
    return;
  _ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  /* After a CLOG_FATAL call its deadline bounds this drain too, so one
   * budget bounds the whole stop. */
  struct timespec deadline =
      atomic_load_explicit(&_clog_fatal_bound_armed, memory_order_acquire)
          ? _clog_fatal_deadline
          : _clog_ts_after_ms(&start, _clog_exit_drain_budget_ms());
  /* The table lock keeps every target in live_shareds alive for the walk,
   * because a close removes its target under that lock before freeing it.
   * The walk takes it for writing because a forked child can get here, where
   * the fork handling has initialized the lock afresh over the prepare
   * step's write lock, and ThreadSanitizer reports a read lock there as a
   * read lock of a write-locked mutex. The lock order (table lock, then a
   * target's mutex) is the one the fork handling uses. A target's queue comes
   * before its compressions, because a writer thread draining the queue can
   * still rotate, and a rotation queues a compression. After that, a target
   * whose writer thread didn't answer in time is left alone, since the thread
   * may hold its mutex. A thread that holds the table lock past the deadline,
   * one that stopped inside a clog_create_from_* call for example, leaves
   * nothing to drain in time. */
  struct timespec table_at = _clog_realtime_at(&deadline);
  if (_ccol_rw_lock_timedwrlock(clog_slot_table.rwlock, table_at) != 0) return;
  if (clog_slot_table.live_shareds) {
    size_t n = cvector_elem_count(clog_slot_table.live_shareds);
    for (size_t i = 0; i < n; i++) {
      clog_shared_t *sh =
          *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
      if (sh == _clog_thread_serves) continue;
      if (sh->async_enabled) {
        /* The drain leaves the target's sends off: a record that reached the
         * queue after it (from a destructor, or from an exit handler that
         * runs after this one) would wait for a flush the end of the process
         * cuts off, so _clog_write_async() writes every such record itself,
         * synchronously, exactly as after a failed enqueue. */
        if (!_clog_exit_drain_async(sh, &deadline)) {
#ifdef RUNNING_UNIT_TESTS
          _clog_test_note_left_target(sh);
#endif
          continue;
        }
      }
      /* Only a target that compresses has a compressor thread. The test reads
       * configuration that never changes after the open, so it takes no lock,
       * because the mutex of a target that doesn't rotate may be held by a
       * thread whose write to a pipe is blocked. */
      if (sh->rotation_enabled && sh->rotation.compress_rotated)
        _clog_exit_drain_compressions(sh, &deadline);
      /* A repair the output still owes (an open fragment, a loss no marker has
       * named yet) gets paid now, when nothing will write again: with the
       * output recovered, the file then ends with whole lines and the loss is
       * named. The lock is only tried, never waited for: a thread holding it
       * is either on a path that pays the debt itself, or blocked for ever in
       * a write, which must delay no exit. */
      if (atomic_load_explicit(&sh->owes_repair, memory_order_relaxed) &&
          _ccol_mutex_trylock(sh->mutex) == 0) {
        int left = _clog_ms_until(&deadline);
        (void)_clog_pay_owed(sh, sh->format,
                             left < CLOG_SPLIT_RECORD_DRAIN_WAIT_MS
                                 ? left
                                 : CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
        _ccol_mutex_unlock(sh->mutex);
      }
    }
  }
  _ccol_rw_lock_unlock(clog_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
void _clog_test_run_exit_drain(void) { _clog_exit_drain(); }
#endif

/* Registered when the library loads, so it runs after every exit handler the
 * application registers while it runs, and a record such a handler logs gets
 * drained too. It does nothing in a process that never starts a writer
 * thread or a compressor thread. */
__attribute__((constructor)) static void _clog_register_exit_drain(void) {
  _clog_exit_drain_registered = atexit(_clog_exit_drain) == 0;
}

static _ccol_once_flag_t _clog_fatal_once = _CCOL_ONCE_INIT;

/* Fixes the deadline of the whole stop, once, when the process's first
 * CLOG_FATAL call starts. */
static void _clog_fatal_arm(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  _clog_fatal_deadline = _clog_ts_after_ms(&now, _clog_exit_drain_budget_ms());
  atomic_store_explicit(&_clog_fatal_bound_armed, true, memory_order_release);
}

/* Reports on stderr that a CLOG_FATAL call's record couldn't be written,
 * unless stderr is the very output that didn't take it. The write to stderr
 * is bounded too, since stderr can be a pipe nobody reads. */
static void _clog_fatal_report_loss(clog_shared_t *sh) {
  static const char msg[] =
      "clogger: a CLOG_FATAL record could not be written to its log output "
      "in time; the process stops without it\n";
  /* The target may still be locked by a thread whose write never returns, so
   * fd is read without the lock. */
  int fd = __atomic_load_n(&sh->fd, __ATOMIC_RELAXED);
  struct stat out, err;
  if (fstat(STDERR_FILENO, &err) != 0) return;
  if (fd >= 0 && fstat(fd, &out) == 0 && out.st_dev == err.st_dev &&
      out.st_ino == err.st_ino)
    return;
  struct pollfd pfd = {.fd = STDERR_FILENO,
                       .events = _clog_pollout_events(STDERR_FILENO),
                       .revents = 0};
  if (poll(&pfd, 1, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS) > 0 &&
      (pfd.revents & POLLOUT)) {
    ssize_t w = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)w;
  }
}

/*
 * The path of a CLOG_FATAL record, which never returns. One deadline, the
 * exit drain's budget counted from the moment the first fatal call starts,
 * bounds the whole stop: every write of the target to a sink that isn't a
 * regular file ends by that deadline (see CLOG_SINK_FATAL_BOUND), and so does
 * every wait on this path, except that the wait for the target's mutex may
 * run CLOG_FATAL_LOCK_GRACE_MS past it (see _clog_fatal_lock_target()). For
 * an async target it first drains the queue and the batch, exactly as the
 * exit drain does, so a message logged moments before the fatal one reaches
 * the log, and then it writes the fatal record synchronously. So an output
 * that takes nothing more can't keep the process from stopping: the record
 * is lost then, a line on stderr says so unless stderr is that same output,
 * and the process stops anyway. A write to a regular file waits on the disk,
 * not on a reader, and has no bound.
 */
static __attribute__((noinline, cold, noreturn, format(printf, 8, 0))) void
_clog_write_fatal(struct clogger *lg, const char *file, int line,
                  const char *func, bool with_backtrace, char **bt_syms,
                  int bt_depth, const char *fmt, va_list ap) {
  _ccol_call_once(_clog_fatal_once, _clog_fatal_arm);
  clog_shared_t *sh = lg->shared;
  if (sh->async_enabled)
    (void)_clog_exit_drain_async(sh, &_clog_fatal_deadline);
  va_list ap2;
  va_copy(ap2, ap);
  bool delivered = _clog_write_sync_body(lg, CLOG_FATAL, file, line, func,
                                         with_backtrace, bt_syms, bt_depth, fmt,
                                         ap, ap2, true, &_clog_fatal_deadline);
  va_end(ap2);
  if (!delivered) _clog_fatal_report_loss(sh);
  if (bt_syms) free(bt_syms);
  _clog_resolve_unpin(lg);
  _clog_fatal_exit();
}

void _clog_write(clog h, clog_level_t level, const char *file, int line,
                 const char *func, bool with_backtrace, const char *fmt, ...) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    _ccol_fatal_err(
        "_clog_write: clog handle is invalid, stale, or already closed");

  /* The fast path. Most log_* call sites are at a level the filter drops
   * (clog_trace and clog_debug when a logger runs at CLOG_INFO or higher,
   * for example), and they hit this check on every call. Since the min_level
   * check comes first, such a call returns at once: on the sync path it
   * never contends for shared->mutex, and on the async path it never formats
   * or copies a message nobody reads.
   *
   * A relaxed load is enough, because this check only saves work in the
   * usual case and doesn't make the final decision for the synchronous path
   * (see the locked second check in _clog_write_sync()). The async path has
   * no second check of its own, so a message this path queued before a
   * concurrent clog_set_level() took effect gets delivered. clog_set_level(3)
   * documents that boundary as intended, for this path and for the
   * synchronous path's unlocked fast check. CLOG_FATAL always skips the
   * filter. */
  if (level < atomic_load_explicit(&lg->min_level, memory_order_relaxed) &&
      level != CLOG_FATAL) {
    _clog_resolve_unpin(lg);
    return;
  }

  /* The capture happens HERE, in this function's frame, the same way for
   * every path below. The doc comment of _capture_backtrace() explains why it
   * must never happen one level deeper, inside _clog_write_sync() or
   * _clog_write_async(): a deeper capture would silently skip one more
   * frame. */
  char **bt_syms = NULL;
  int bt_depth = 0;
  if (with_backtrace) _capture_backtrace(&bt_syms, &bt_depth);

  va_list ap;
  va_start(ap, fmt);

  /* A fatal record has a path of its own, which never returns. */
  if (__builtin_expect(level == CLOG_FATAL, 0))
    _clog_write_fatal(lg, file, line, func, with_backtrace, bt_syms, bt_depth,
                      fmt, ap);

  /* lg->shared->async_enabled is read here without a lock. Only two places
   * write it, each exactly once: the construction, before any reader can see
   * this shared object, and the downgrade inside _clog_atfork_child(), which
   * runs on the only thread a freshly forked child has, strictly before any
   * other code in that child can read the field. Nothing changes the field
   * while another thread can read it, so this read needs no lock. */
  if (lg->shared->async_enabled) {
    _clog_write_async(lg, level, file, line, func, with_backtrace, bt_syms,
                      bt_depth, fmt, ap);
    va_end(ap);
    _clog_resolve_unpin(lg);
    return;
  }

  /* Async isn't configured, so the call goes to the synchronous body and ap
   * passes straight through. va_end(ap) must come AFTER this call, not
   * before, because ap is still live and this call uses it. */
  _clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                   bt_depth, fmt, ap);
  va_end(ap);
  if (bt_syms)
    free(bt_syms); /* The sync path frees what was captured above, because
_clog_write_sync() only reads bt_syms and never frees it */
  _clog_resolve_unpin(lg);
}
