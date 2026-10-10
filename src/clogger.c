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
#include <internal/cgrowbuf.h>
#include <internal/cpintable.h>
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
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif
#include <internal/crandom.h>
#include <internal/csock.h>
#include <internal/ctlsmodel.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
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
/* The collision suffix of a rotated name is "_" and a decimal sequence
 * number, zero-padded to 4 digits; a number above 9999 takes as many digits
 * as it needs, up to CLOG_ROTATION_SEQ_MAX_DIGITS. */
#define CLOG_ROTATION_SEQ_MAX_DIGITS 9
#define CLOG_ROTATION_SEQ_MAX 999999999UL
/* Extra headroom for the collision suffix: "_" plus the widest number. */
#define CLOG_ROTATION_EXTRA (1 + CLOG_ROTATION_SEQ_MAX_DIGITS)
/* The number of digits in the stamp of a rotated name, YYYYMMDDHHMMSS. */
#define CLOG_ROTATION_STAMP_DIGITS 14
/* A rotated-looking name whose stamp lies more than this many seconds above
 * both the clock and the newest name that the target made is not one of its
 * generations (see _clog_gen_filter_init()). A clock that steps back keeps
 * the names of the target above the clock by the size of the step, and the
 * largest steps that a working system takes (a hardware clock kept in local
 * time and read as UTC is off by at most 26 hours) stay far inside 30 days.
 * A stamp beyond that came from no clock that this target trusted. */
#define CLOG_ROTATION_FUTURE_TOLERANCE_SECS ((int64_t)30 * 24 * 60 * 60)
/* The private name under which a rotation creates the next live file, before
 * it renames the file onto the live name: "<base>" + ".tmp" + 11 lowercase
 * hex digits. It is exactly as long as a rotated name with no collision
 * suffix, so a directory that can hold a rotated name can hold it. It never
 * parses as a rotated name, because a digit must follow "<base>." there. */
#define CLOG_ROTATION_TMP_TAG ".tmp"
#define CLOG_ROTATION_TMP_HEX_DIGITS 11
#define CLOG_ROTATION_TMP_LEN \
  (sizeof(CLOG_ROTATION_TMP_TAG) - 1 + CLOG_ROTATION_TMP_HEX_DIGITS)
_Static_assert(CLOG_ROTATION_TMP_LEN == CLOG_ROTATION_FMT_LEN,
               "the private name of a new live file must be no longer than a "
               "rotated name");
/* A directory entry that looks like a rotated file of the target but is not
 * one of its generations gets one WARN record, and at most this many such
 * records go out for each target in any window of this many seconds. */
#define CLOG_IGNORED_NOTE_BURST 8U
#define CLOG_IGNORED_NOTE_WINDOW_SECS ((time_t)60)
/* A rotation attempt can fail, for example because of a permission error,
 * or because the file path is too long for the internal buffer of the
 * rotated name. After such a failure the library backs off later attempts
 * by this many seconds instead of trying again on every single write.
 * Without this backoff, a failure that persists costs every later write the
 * full syscall overhead of rename() and open() for as long as the
 * underlying condition lasts, and size-based rotation keeps failing against
 * its own trigger with no bound on the rate of the attempts. */
#define CLOG_ROTATE_RETRY_BACKOFF_SECS ((time_t)1)

static const char *const _LEVEL_STR[] = {"TRACE", "DEBUG", "INFO",  "WARN",
                                         "ERROR", "ALERT", "FATAL", "OFF"};
/* A clog_level_t value indexes the array above and the array below
 * directly, so keep the size of each one tied to the number of values in
 * the enum. Then a future insertion of a level that forgets to grow one of
 * them fails to compile, instead of silently using a wrong index at run
 * time. */
_Static_assert(sizeof(_LEVEL_STR) / sizeof(_LEVEL_STR[0]) == CLOG_OFF + 1,
               "_LEVEL_STR must have exactly one entry per clog_level_t value");

/*
 * These are the keys that clog_set_field() rejects: the fixed output keys
 * that _clog_write() itself always writes, in every format. A user field
 * must not reuse one of them, because it would make a duplicate "key":..
 * pair in JSON, a duplicate key= token in logfmt, and, in syslog output, a
 * duplicate SD-PARAM-NAME inside one SD-ELEMENT, while RFC 5424 needs an
 * SD-PARAM-NAME that is unique for each SD-ELEMENT.
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
 * it to the compressor thread of the shared target (see "BACKGROUND
 * COMPRESSION" below), and from then on the job owns its names and the
 * protection that retention gives to its generation. Both names are
 * relative to clog_shared_t.dir_fd: `src` is the rotated file that the
 * compression reads, and `dst` is `src` plus ".gz", which it writes.
 *
 * A job waits in the queue of the shared target until the compressor thread
 * takes it, and from that moment until the compression ends it is
 * clog_shared_t.compress_running. _prune_rotated() never deletes a file of
 * the running job, because the compression may not have read all of the
 * source or written all of the destination yet; a deletion pass can see the
 * destination on disk from the moment that the compression creates it,
 * long before the write finishes. A job that is still waiting in the queue
 * has no such protection: nothing reads or writes its files yet, so a pass
 * that finds its generation older than the newest max_rotated_files deletes
 * it like any other, and the compression of such a job then finds no
 * source and does nothing. The library reads and changes the queue and
 * compress_running only while it holds clog_shared_t.mutex.
 */
typedef struct clog_compress_job {
  struct clog_compress_job *next;
  char *src;
  char *dst;
  /* The owner of the live file when the job was queued. The compression
   * reads src only when src is a regular file owned by this user or by the
   * effective user of the process (see clog_gen_filter_t). */
  uid_t live_uid;
} clog_compress_job_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  /* Hard growth ceiling for THIS buffer, in bytes. The default is
   * CLOG_BUF_MAX, which _buf_init() sets, and the library raises it above
   * that default only for clog_shared_t.async_buf, and only as far as the
   * clog_async_cfg_t.flush_buffer_size of the caller needs (see
   * _shared_async_init()). Every other clog_buf_t in this file, including
   * the lg->buf of each logger and every scratch buffer, keeps the
   * CLOG_BUF_MAX default. The limit exists to bound one record that is too
   * large, which is a separate concern from how large a caller wants the
   * async batch itself to grow before a flush. */
  size_t cap_limit;
  const ccol_memmgmt_procs_t *m_procs; /* borrowed from clog_shared_t */
  /* _buf_ensure() sets this when a growth attempt fails because the
   * underlying allocator call itself fails, which is a real out-of-memory
   * state. That is different from a growth attempt that only reaches the
   * cap_limit of this buffer, as happens for a record that is legitimately
   * too large or when a shared batch buffer has too little room left. Both
   * cases report the same failure to the caller of _buf_ensure(), so this
   * flag is the only place that separates them. _clog_build_record() resets
   * it to false at the start of every attempt to build a record and reads it
   * afterwards, whatever nested append call set it, so that it can report an
   * accurate cause to _clog_build_fallback_record(). Without the flag, the
   * library always blames the size. */
  bool oom;
} clog_buf_t;

/*
 * The shared backing store, which holds the fd, the rotation state and the
 * mutex. It is reference-counted, so the root logger and every derived
 * logger share one instance, and the library closes the fd when ref_count
 * reaches zero if the store owns that fd.
 */
/* The values of clog_shared_t.async_partial_record. A record can span more
 * than one line (a logfmt record carries its backtrace as tab-indented
 * continuation lines after its primary line), so a short write can split a
 * record in the middle of a line, or exactly at the newline that ends one
 * of its lines while more of its lines are still undelivered. The two need
 * different repairs, because only the first leaves a line open on disk. */
enum {
  CLOG_SPLIT_NONE = 0,     /* the bytes on disk end on a record boundary */
  CLOG_SPLIT_MID_LINE = 1, /* they end inside a line of a record */
  CLOG_SPLIT_AT_LINE = 2,  /* they end at a newline inside a record */
};

typedef struct clog_shared {
  int fd;
  bool owns_fd;
  /* This is not CLOG_SPLIT_NONE while the leading bytes of async_buf are the
   * continuation of a record whose first half is already on disk. A write
   * that the kernel accepts only in part leaves this state behind (see
   * _writer_flush_now()), but only when the accepted prefix stops inside a
   * record; a prefix that stops exactly on a record boundary splits nothing,
   * however much of the batch it leaves behind. A newline that a
   * tab-indented continuation line follows is inside a record, not on its
   * boundary.
   *
   * Both rotation checks refuse to rotate while this flag is set, so the two
   * halves always land in the same file. Without that rule, a rotation can
   * happen between the short write and the retry: it renames away the file
   * that holds the first half, the continuation opens the fresh file, and
   * one record then spans two files with no marker that names the split.
   * Every line-framed format that this module writes (logfmt, JSON lines
   * and RFC 5424) then parses both ends wrongly. Every path that would write
   * a DIFFERENT record straight to the fd settles this flag first (see
   * _clog_flush_and_settle()), so nothing can land between the two halves
   * of a record. The first flush whose write the kernel accepts in full
   * clears the flag, and so does the repair that stands in for such a flush.
   *
   * This field sits here, in padding that the fields around it already
   * leave, instead of beside the async block that it belongs to: every write
   * path tests it immediately after it tests fd, so the two share a cache
   * line. A synchronous logger can never set this flag, and it pays nothing
   * for the test beyond a load that it already makes. */
  unsigned char async_partial_record;
  /* What fd is, as a clog_sink_kind_t: CLOG_SINK_PLAIN for a regular file
   * and every other descriptor that cannot raise SIGPIPE, CLOG_SINK_PIPE for
   * a pipe or a FIFO, and CLOG_SINK_SOCKET for a socket. fstat() decides it
   * when the logger opens, and again when a rotation opens a new file. Every
   * write reads it (see _write_all()), so it sits in padding beside fd and
   * shares its cache line, and no other field moves. */
  unsigned char sink_kind;
  /* True when fd is a message-oriented socket (SOCK_DGRAM, SOCK_SEQPACKET or
   * SOCK_RDM). Such a socket keeps the boundary of every write: one write is
   * one message, and a message that exceeds the limit of the socket fails as
   * a whole. So the async writer thread writes every record of such a sink
   * with its own write and never puts two records into one batch. It is
   * decided together with sink_kind, and sits in the padding beside it. */
  bool sink_per_record;
  char *file_path; /* NULL for fd-based loggers */

  bool rotation_enabled;
  clog_rotation_cfg_t rotation;
  off_t bytes_written;
  time_t last_rotation;
  /* The earliest time at which the library can try a rotation again after a
   * _rotate() call that failed. The value 0, which calloc gives by default,
   * means that no backoff is in effect; see
   * CLOG_ROTATE_RETRY_BACKOFF_SECS. */
  time_t rotate_retry_after;
  /* The compression that the compressor thread runs at this moment, or NULL
   * (see clog_compress_job_t above). _prune_rotated() never deletes a file of
   * this job. */
  clog_compress_job_t *compress_running;

  ccol_mutex_t mutex;
  int ref_count;
  clog_format_t format;
  clog_syslog_facility_t syslog_facility; /* PRI facility for CLOG_FMT_SYSLOG */
  char syslog_hostname[256]; /* hostname cached at creation       */
  char syslog_appname[49];   /* APP-NAME cached at creation        */
  ccol_memmgmt_procs_t
      *m_procs; /* heap-allocated copy; NULL = default allocator */

  /* Async logging (see "ASYNC LOGGING" below). For a synchronous logger
   * (async_enabled == false) every field in this block is zero and unused,
   * so a synchronous logger pays nothing for the presence of async support.
   * The library writes async_enabled itself exactly once: the thread that
   * constructs this shared object writes it before any other thread can see
   * the object, or, in a forked child, the downgrade in _clog_atfork_child
   * writes it instead (see the comment on that function). Because nothing
   * ever changes the field at the same time as a read, every other reader,
   * including the dispatch check inside _clog_write(), can read it without
   * a lock. */
  bool async_enabled;
  bool is_bounded_queue; /* meaningful only if async_enabled */
  union {
    ccol_circular_queue *circq;
    ccol_dynamic_queue *dynmq;
  } q;
  clog_async_cfg_t async_cfg; /* resolved copy, defaults already applied */
  ccol_thread_id_t writer_thread;
  clog_buf_t async_buf; /* The aggregation buffer that the writer thread
      owns. It is separate from the lg->buf of a single handle, because the
      jobs from every handle that shares this shared target land in ONE
      buffer. clog_shared_t.async_partial_record, above next to fd, says
      whether the leading bytes of this buffer continue a record that is
      already half written. */
  /* This is CLOCK_MONOTONIC, deliberately not gettimeofday() or
   * CLOCK_REALTIME. It is only an internal bookkeeping value for the time
   * since the last flush, which the main loop of the writer thread uses to
   * compute how long to wait before its next chance to flush (see
   * _clog_writer_thread_main() below); the library never gives it to a
   * caller and never puts it in a record. A wall-clock measurement is wrong
   * here, because the system clock can step backward between two flushes
   * (an NTP step correction, a manual date change, or a clock correction
   * after a live migration of a virtual machine). The computed elapsed time
   * then becomes negative by any amount, the next wait of the writer thread
   * grows far past flush_interval_us, and the guarantee that
   * clog_async_cfg_t.flush_interval_us documents (a flush at least that
   * often) silently breaks. CLOCK_MONOTONIC is immune to this class of
   * adjustment. */
  struct timespec last_flush_monotonic;

  /* The rotation state below is read only while a rotation runs, and it sits
   * at the end so that it moves no field that each write reads. */
  /* The name of the newest rotated file that this shared target made, or
   * found on disk before its first rotation: the 14 stamp digits, with no
   * NUL, and the collision sequence number, which is 0 for a name with no
   * suffix. rot_name_known says whether the two hold a name. _rotate()
   * gives every new rotated name a (stamp, sequence) pair above this one.
   * Retention orders rotated files by that pair, so a later rotation always
   * sorts as newer. See _rotate(). */
  char rot_last_stamp[CLOG_ROTATION_STAMP_DIGITS];
  bool rot_name_known;
  unsigned long rot_last_seq;
  /* The rate limit of the WARN records about directory entries that look
   * like rotated files of this target and are not its generations: the
   * start of the current window and the records written in it. See
   * _clog_note_ignored_entry(). */
  time_t ignored_note_window;
  unsigned ignored_note_count;
  /* The last component of file_path. Every rename, open, listing,
   * compression and deletion of a rotation works relative to dir_fd below
   * and to this name. */
  const char *file_base;
  /* The background compression state (see "BACKGROUND COMPRESSION"). The
   * queue holds the jobs that the compressor thread has not taken yet, the
   * oldest first. compress_cv wakes that thread for a new job or for a stop,
   * and it wakes every thread that waits for the queue to drain. The thread
   * exists, and compress_cv is initialized, exactly while compressor_live is
   * true. All of it is guarded by mutex. */
  clog_compress_job_t *compress_head;
  clog_compress_job_t *compress_tail;
  ccol_cond_var_t compress_cv;
  ccol_thread_id_t compressor_thread;
  /* The directory that holds the log file, opened once by
   * clog_open_file_mp() when rotation is on, and -1 otherwise. Through it, a
   * relative path keeps naming the directory in which the logger opened the
   * file, whatever the working directory of the process becomes later. */
  int dir_fd;
  bool compressor_live;
  bool compressor_stop;
  /* True when a deletion pass kept an old generation only because its
   * compression was running. The compressor thread then runs the pass again
   * once that compression ends; see _clog_compressor_main(). */
  bool compress_prune_deferred;
  /* The exit drain sets this when its time budget runs out while a
   * compression still runs. The compression then stops at its next block,
   * removes its temporary output and keeps the source; see
   * _gzip_compress_file() and _clog_exit_drain(). */
  _Atomic bool compress_abort;
  /* The directory entry that a file-backed target writes: the device and
   * inode of its directory, and file_base. A second clog_open_file_mp() of
   * the same entry finds this target through clog_slot_table.file_shareds;
   * see _clog_file_register_or_attach(). file_id_known is false for an
   * fd-based target and when the directory cannot be examined. */
  bool file_id_known;
  dev_t file_dir_dev;
  ino_t file_dir_ino;
  /* The number of CLOG_FATAL calls that wait for mutex. While it is not
   * zero, the writer thread takes the mutex for no further work, so that
   * the fatal call gets the mutex as soon as the work in hand ends; see
   * _clog_writer_lock(). */
  atomic_uint fatal_waiters;
} clog_shared_t;

struct clogger {
  clog_shared_t *shared; /* shared output backing store              */
  /* The level filter of this logger. It is _Atomic so that _clog_write() can
   * check it cheaply before it locks shared->mutex (see the fast-path check
   * there): another thread can call clog_set_level() on the same handle at
   * the same time, and the _Atomic type keeps that unlocked read from being
   * a data race against such a call. */
  _Atomic clog_level_t min_level;
  /* This mutex guards `fields` only. It is separate from shared->mutex,
   * which serialises the fd writes and the rotation across a whole derive
   * tree, because the field map of each logger is otherwise fully
   * independent of every sibling logger that shares the same `shared`: a
   * call to clog_set_field(), clog_remove_field() or clog_clear_fields() on
   * one derived logger must not contend with an unrelated write in progress
   * on a sibling. _clog_write() and clog_derive() take this lock only for
   * the short window in which they read `fields`. When a function needs both
   * this lock and shared->mutex, as _clog_write() does while it holds
   * shared->mutex for the whole call, the code always locks shared->mutex
   * first and never the other way round, so no lock-order cycle is possible
   * anywhere in this file. */
  ccol_mutex_t fields_mutex;
  chmap fields;   /* chmap(char* -> char*); per-logger fields */
  clog_buf_t buf; /* per-logger reusable write buffer          */
  /* The handle of this logger, so that an unpin can find the slot that
   * holds its pin without the caller carrying one. The library writes this
   * once, before it publishes the handle, and never again. */
  clog self_handle;
};

/* ========================================================================== */
/*                         HANDLE SLOT TABLE                                  */
/* ========================================================================== */

/*
 * A clog is an opaque value handle made of a slot index and a generation,
 * not a pointer. This table closely follows chttpsvr_slot_table and
 * chttpcli_slot_table (in src/chttpserver.c and src/chttpclient.c), with
 * one deliberate difference: the resolve step of clogger is on the hot path
 * of every single log_* call, including the calls that the level filter
 * drops, so it runs far more often than the resolve sites of those two
 * modules. A reader-writer lock guards this table for the cold paths that
 * change it (an acquire, a close and the fork walk), while the resolve step
 * and the pin step go through the lock-free handle index in cpintable.h and
 * take no lock of this table at all.
 */
typedef struct {
  struct clogger *ptr; /* NULL when free */
  uint32_t generation; /* 0 before the first use, 1 after the first acquire */
  bool in_use;         /* false from step 2 of clog_close() onward. It gates
      _clog_resolve only and does NOT mean that it is safe to dereference ptr
      for the protection of fields_mutex; see `freed` below */
  bool freed;          /* false until _logger_free(ptr) truly runs; see
               "Fork safety" below. The fields_mutex walk of _clog_atfork_prepare
               gates on THIS field, not on in_use, because in_use goes false
               long before the library destroys ptr->fields_mutex */
} clog_slot_t;

/* The hot half of the table above: it maps a handle to a pointer and holds
 * the pin that keeps a logger alive for the duration of a call. It is
 * separate because a resolve runs on every single log call and must not
 * write anything that another thread reads. Everything else that this table
 * does (the fork handling, live_shareds and the reuse of a slot) is cold and
 * stays under the rwlock. */
static ccol_pintable clog_pintable;

static struct {
  ccol_rw_lock_t rwlock;
  ccol_once_flag_t once;
  cvec slots;        /* cvector of clog_slot_t */
  cvec free_indices; /* cvector of uint32_t; LIFO */
  cvec live_shareds; /* A cvector of clog_shared_t*. It covers the whole
      lifetime of a clog_shared_t, from the moment the library acquires the
      first handle of the object until the library frees the object,
      independently of the in_use flag of any one handle. See "Fork safety"
      below for why this dedicated registry exists instead of deriving the
      set of live clog_shared_t objects from the handle table. */
  /* How many clog_close calls have passed the point at which they stop being
     visible as a live slot but are not yet finished with this table. A close
     clears its own slot->ptr partway through and then takes the write lock
     again to remove its shared object from live_shareds, so slot->ptr alone
     does not cover the whole window in which the table must survive. Inside
     one thread that is harmless, because the later step always follows, but
     across threads it is not: without this counter, a second close that
     finishes in between destroys live_shareds, and the first close then
     indexes it. */
  size_t closes_in_flight;
  /* The library sets this when the destructor at process exit found a logger
     that was still open and left this table alone; whichever close is the
     last one out afterwards then does the release that the destructor could
     not do. Without this flag, a logger that a destructor closes (one that
     the linker put before this one) leaves the table and the pin index
     allocated for the rest of the process, and a leak checker that treats
     still-reachable memory as an error reports that memory. The library
     reads and writes this field only under the write lock. */
  bool release_deferred;
  /* A cvector of clog_shared_t*: every file-backed target whose directory
     entry is known (clog_shared_t.file_id_known). An open of a path that a
     target here already writes attaches to that target, or fails, instead
     of building a second target on the same file; see
     _clog_file_register_or_attach(). A target stays here until its last
     close has stopped its threads and closed its descriptors, so that an
     open cannot build a second target while the first one still rotates or
     compresses. The library reads and changes it only under the write
     lock. */
  cvec file_shareds;
} clog_slot_table = {0};

/* These are defined with the teardown at process exit below, and declared
   here because the final locked section of clog_close does the release
   that the destructor deferred. */
static bool _clog_any_slot_live_locked(void);
static void _clog_release_slot_table_locked(void);
static void _clog_release_slot_table_if_deferred_locked(void);

#if CCOL_FORK_SAFETY_REQUIRED
/*
 * One slot that a fork() catches in the middle of a clog_close(), with
 * in_use == false and freed == false. _clog_atfork_prepare() records these
 * slots, and the child side of _clog_atfork_release() finishes them, below;
 * read those two functions for why this needs its own tracking, which the
 * ordinary locked_fields protection that every live slot gets does not
 * provide.
 *
 * When CCOL_FORK_SAFETY_REQUIRED is 0 (see the doc comment of that macro in
 * common.h), the compiler removes this type, _clog_atfork_state below, and
 * every function and registration site that touches either of them.
 * clog_slot_table.slots, free_indices and live_shareds stay compiled, and
 * so does clog_slot_t.freed, because they also serve the ordinary handle
 * lifecycle of this module, which has nothing to do with a fork, and not
 * fork safety alone.
 */
typedef struct {
  uint32_t idx;
  struct clogger *raw;
} clog_atfork_closing_t;

/*
 * This records every lock that _clog_atfork_prepare takes, so that
 * _clog_atfork_parent and _clog_atfork_child can unlock exactly what the
 * prepare step took. Only the thread that forks reads or writes this state,
 * and only between the prepare call and the matching parent or child call;
 * since nothing touches it concurrently, it needs no lock of its own.
 * _clog_atfork_prepare walks clog_slot_table.live_shareds for the
 * protection of shared->mutex; that vector is not a field here.
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

/* Removes sh from v, a cvector of clog_shared_t*, when v holds it. The order
 * of such a vector carries no meaning, so the last entry takes the place of
 * the removed one. The caller holds the write lock of the slot table. */
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
 * "CONSTRUCTOR HELPERS" section. The child-side handling of
 * _clog_atfork_release() must call all three of them to finish a
 * clog_close() call that a fork() left in the middle of its teardown; see
 * the doc comment of that function. */
static void _logger_free(struct clogger *lg);
static void _shared_close_owned_fd(clog_shared_t *sh);
static void _shared_free_partial(clog_shared_t *sh);

#ifdef RUNNING_UNIT_TESTS
/*
 * This lets a test force the NEXT _clog_handle_acquire() call
 * deterministically, in two ways. First, that call grows
 * clog_slot_table.slots with a brand new slot, exactly as if free_indices
 * were empty, whatever slots earlier tests in this same process left there
 * for reuse. Second, the live_shareds registration step of that same call
 * fails, without the plain default allocator of the process having to fail
 * for real; that allocator is the only one that clog_slot_table.slots,
 * free_indices and live_shareds ever use, whatever the custom mprocs of a
 * logger are (see _clog_slot_table_init_globals()). Both effects together
 * are needed to reach one rollback path, which can leave a slot behind with
 * freed == false and ptr == NULL at the same time (see the comment on that
 * rollback in _clog_handle_acquire()). A slot that comes off free_indices
 * for reuse already has freed == true from the clog_close() of its earlier
 * occupant, so only a fresh slot whose OWN registration step then fails can
 * reach that path. This hook disarms itself the moment that it fires, so it
 * affects only the one acquisition that a test targets.
 */
static _Atomic bool _clog_test_force_next_fresh_slot_reg_failure = false;

static bool _clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return atomic_exchange(&_clog_test_force_next_fresh_slot_reg_failure, false);
}

void clog_test_force_next_fresh_slot_registration_failure(bool force) {
  atomic_store(&_clog_test_force_next_fresh_slot_reg_failure, force);
}

/* See the doc comment of clog_test_set_close_finalize_delay_us() in
 * clogger.h. Unlike the hook just above, this hook deliberately does NOT
 * disarm itself, because a test needs it armed across the whole
 * clog_close() call under test, not only for its first use. */
static _Atomic unsigned int _clog_test_close_finalize_delay_us = 0;
static _Atomic bool _clog_test_close_finalize_delay_entered = false;

void clog_test_set_close_finalize_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_close_finalize_delay_entered, false);
  atomic_store(&_clog_test_close_finalize_delay_us, delay_us);
}

bool clog_test_close_finalize_delay_entered(void) {
  return atomic_load(&_clog_test_close_finalize_delay_entered);
}

/* See the doc comment of clog_test_set_close_release_window_us() in
 * clogger.h. Like the hook above, this one deliberately does not disarm
 * itself. */
static _Atomic unsigned int _clog_test_close_release_window_us = 0;
static _Atomic bool _clog_test_close_release_window_entered = false;

void clog_test_set_close_release_window_us(unsigned int delay_us) {
  atomic_store(&_clog_test_close_release_window_entered, false);
  atomic_store(&_clog_test_close_release_window_us, delay_us);
}

bool clog_test_close_release_window_entered(void) {
  return atomic_load(&_clog_test_close_release_window_entered);
}

size_t clog_test_live_shareds_count(void) {
  ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  ccol_rw_lock_rdlock(clog_slot_table.rwlock);
  size_t n = cvector_elem_count(clog_slot_table.live_shareds);
  ccol_rw_lock_unlock(clog_slot_table.rwlock);
  return n;
}
#else
static inline bool _clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return false;
}
#endif

static void _clog_slot_table_init_globals(void) {
  if (ccol_rw_lock_init(clog_slot_table.rwlock) != 0)
    ccol_fatal_err("clog slot table: failed to initialize rwlock");
  clog_slot_table.slots = cvector_create(sizeof(clog_slot_t), NULL);
  if (!clog_slot_table.slots)
    ccol_fatal_err("clog slot table: failed to allocate slots vector");
  clog_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clog_slot_table.free_indices)
    ccol_fatal_err("clog slot table: failed to allocate free-index vector");
  clog_slot_table.live_shareds = cvector_create(sizeof(clog_shared_t *), NULL);
  if (!clog_slot_table.live_shareds)
    ccol_fatal_err("clog slot table: failed to allocate live-shareds vector");
  clog_slot_table.file_shareds = cvector_create(sizeof(clog_shared_t *), NULL);
  if (!clog_slot_table.file_shareds)
    ccol_fatal_err("clog slot table: failed to allocate file-shareds vector");
#if CCOL_FORK_SAFETY_REQUIRED
  _clog_atfork_state.locked_fields =
      cvector_create(sizeof(struct clogger *), NULL);
  if (!_clog_atfork_state.locked_fields)
    ccol_fatal_err("clog slot table: failed to allocate atfork scratch vector");
  _clog_atfork_state.closing_slots =
      cvector_create(sizeof(clog_atfork_closing_t), NULL);
  if (!_clog_atfork_state.closing_slots)
    ccol_fatal_err(
        "clog slot table: failed to allocate atfork closing-slots vector");
  ccol_at_fork(_clog_atfork_prepare, _clog_atfork_parent, _clog_atfork_child);
#endif
}

/* This function is not in clogger.h and is not part of the public API: it is
 * a narrow, deliberate escape hatch that follows
 * _cthreadcomm_ensure_atfork_registered_before_caller() in cthreadcomm.c and
 * _ctpool_ensure_atfork_registered_before_caller() in cthreadpool.c exactly.
 * It exists for a dependent module, chttpserver.c, which must guarantee that
 * the ccol_at_fork() triple of this module is registered BEFORE its own.
 * pthread_atfork runs the prepare handlers in the reverse order of their
 * registration, so the prepare handler of the CALLER runs FIRST at every
 * future fork(), before the prepare handler of this module,
 * _clog_atfork_prepare, can lock clog_slot_table.rwlock, the mutex of a live
 * clog_shared_t, or a fields_mutex.
 *
 * chttpserver.c holds its own srv_engine_bundler.mutex across a call into
 * this module in two places: the clog_info() or clog_warn() call
 * inside _SRV_ENGINE_LOG, and the direct clog_open_fd_mp call in
 * _engine_acquire for the fallback logger of the engine. Without this
 * function, the real lock order at fork() time is an accident that depends
 * on which of the two modules the embedding application uses FIRST.
 * Consider a caller that resolves a chttpsvr handle before this process ever
 * uses clog for anything else; that handle can even be an invalid one,
 * because every public chttpsvr_* function that resolves a handle accepts an
 * invalid handle and reports a plain error, without needing the caller to
 * guarantee validity first. Such a caller registers the ccol_at_fork()
 * triple of chttpserver with no forced clogger registration before it, and
 * a later, independent first use of clog then registers the ccol_at_fork()
 * triple of this module AFTER the one of chttpserver. That inverts the order
 * that the code needs and makes exactly the AB-BA fork() deadlock shape that
 * the same forced registration helpers in chttpserver, cthreadcomm and
 * cthreadpool exist to keep closed (see the doc comment of
 * _chttpsvr_slot_table_init_globals in chttpserver.c). A caller that never
 * uses this function is not affected at all: the lazy registration of this
 * module, which ccol_call_once guards, happens whenever the library first
 * creates a clog handle on its own. */
void _clog_ensure_atfork_registered_before_caller(void) {
  ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
}

/*
 * Resolve h and pin the result against a concurrent clog_close(). It returns
 * NULL for h == 0, for an h that is out of range, and for an h that names a
 * free slot or a slot with the wrong generation. After a resolve that
 * succeeds, the caller must call _clog_resolve_unpin() exactly once on every
 * path. This function takes no lock at all and writes nothing that another
 * thread reads: it runs on every log_* call, including a call that its own
 * level check drops, so every core would pay for any shared write here.
 */
static struct clogger *_clog_resolve(clog h) {
  /* No lock, and no write to anything that another thread reads. The pin
   * index is a static that starts at zero, so a handle that arrives before
   * the library ever opens a logger finds no chunk and resolves to NULL,
   * which is the same answer that an empty slot table gives. */
  return (struct clogger *)ccol_pintable_pin(&clog_pintable, h);
}

/*
 * This function is lock-free, deliberately and safely: it releases the pin
 * and touches nothing else. clog_close() never sleeps on a condition
 * variable waiting for this function to wake it; it polls the pin count
 * instead (see step 3 of clog_close()), so there is no wakeup to deliver.
 * Do NOT add a lock, a broadcast or any other use of raw here to be safe.
 * After the release of the pin the library can free the object at any
 * instant, so anything after the release is a use-after-free, and anything
 * before the release is a shared write on a path whose whole purpose is to
 * have none.
 *
 * A read of raw->self_handle before the release is safe, because the pin is
 * still held at that point.
 */
static void _clog_resolve_unpin(struct clogger *raw) {
  ccol_pintable_unpin(&clog_pintable, raw->self_handle);
}

/*
 * This makes a fresh handle for an lg that is fully constructed. The library
 * calls it once from the constructor path, after lg and sh are otherwise
 * completely built, and also from clog_derive(). It gives CLOG_INVALID when
 * the slot table runs out of memory, which is an ordinary failure and not a
 * fatal one. It also registers lg->shared into clog_slot_table.live_shareds
 * (see "Fork safety" below) without adding an entry that is already there:
 * a brand new root construction and a sibling handle from clog_derive() both
 * call this same function, and only the FIRST one that names a given
 * `shared` adds it.
 *
 * This function closely follows _chttpsvr_handle_slot_acquire (in
 * src/chttpserver.c). The index and the generation encode as
 * (idx << 32) | generation, and the generation wraps around and skips the
 * value 0, which is reserved: it means "never used yet" and separates
 * CLOG_INVALID from a real handle. There is one addition to that shape:
 * every acquire sets slot->freed = false unconditionally. A slot that comes
 * off free_indices has freed == true from the close of its earlier
 * occupant, so without this reset a handle born from such a slot silently
 * keeps freed == true from an unrelated predecessor that is already gone,
 * and the fields_mutex walk of _clog_atfork_prepare then never protects
 * that handle at all.
 */
static clog _clog_handle_acquire(struct clogger *lg) {
  ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  ccol_rw_lock_wrlock(clog_slot_table.rwlock);

  /* Outside RUNNING_UNIT_TESTS the compiler removes this completely: in a
   * build that is not a test build, the helper is only `return false;`, so
   * this costs nothing on the hot handle-acquisition path of a real
   * build. */
  bool force_fresh_slot_reg_failure =
      _clog_test_consume_forced_fresh_slot_reg_failure();

  uint32_t idx;
  clog_slot_t *slot;
  if (!force_fresh_slot_reg_failure &&
      cvector_elem_count(clog_slot_table.free_indices) > 0) {
    cvector_pop_back(clog_slot_table.free_indices, &idx);
    slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
  } else {
    /* The pin table can hold only so many slots, and the library can never
     * publish a slot whose index is beyond that limit. So it refuses such a
     * slot here instead of claiming the slot and rolling it back, because a
     * rollback would put an index that no later publish can use onto the
     * free list, and every acquire pops from that list. This is an ordinary
     * failure, which a caller already has to treat in the same way as a
     * table that cannot grow. */
    if (cvector_elem_count(clog_slot_table.slots) >= CCOL_PIN_MAX_SLOTS) {
      ccol_rw_lock_unlock(clog_slot_table.rwlock);
      return CLOG_INVALID;
    }
    clog_slot_t fresh = {0};
    if (cvector_push_back(clog_slot_table.slots, &fresh) != ccol_success) {
      ccol_rw_lock_unlock(clog_slot_table.rwlock);
      return CLOG_INVALID;
    }
    idx = (uint32_t)cvector_elem_count(clog_slot_table.slots) - 1;
    slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
  }

  /* This registers lg->shared into live_shareds, which is the exact set that
   * _clog_atfork_prepare() walks to decide which shared->mutex objects to
   * lock before a fork(). The registration happens BEFORE the library
   * changes this slot or hands it out, so that a registration that fails can
   * roll back while the index is still untouched. A handle whose shared
   * object failed to register silently escapes the protection of
   * _clog_atfork_prepare(): a fork() can race a write or a rotation in
   * progress on that exact logger, and the child then keeps a shared->mutex
   * that stays locked for ever, which is the class of hang that the
   * fork-safety machinery in this file exists to prevent. */
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
   * ignores already_registered. For clog_open_fd_mp() and
   * clog_open_file_mp(), lg->shared is always fresh, so already_registered
   * is always false there and this makes no difference. For clog_derive(),
   * lg->shared is the already-registered shared object of the PARENT, which
   * is certain to be in live_shareds for as long as the parent itself is a
   * live, pinned handle, so already_registered is always true there.
   * Without this unconditional override, the forced failure hook can never
   * fire for a clog_derive() call at all: it consumes itself silently,
   * because the hook disarms itself on every call (see
   * _clog_test_consume_forced_fresh_slot_reg_failure() above), and the call
   * makes a valid handle anyway. That contradicts the documented promise of
   * this hook, which is to work the same way at all three call sites. */
  bool live_shareds_registration_failed = force_fresh_slot_reg_failure;
  if (!live_shareds_registration_failed && !already_registered) {
    live_shareds_registration_failed =
        cvector_push_back(clog_slot_table.live_shareds, &lg->shared) !=
        ccol_success;
  }
  if (live_shareds_registration_failed) {
    /* A slot that comes off free_indices is safe to leave untouched here,
     * because it already has freed == true and ptr == NULL from the
     * clog_close() of its earlier occupant. A slot that comes from the branch
     * above that pushes a brand new slot is NOT safe to leave untouched: its
     * own fresh = {0} initialization leaves freed == false with ptr == NULL,
     * and the walk in _clog_atfork_prepare() reads that exact combination as
     * "this slot has a live ptr, so lock its fields_mutex", because that walk
     * only ever skips a slot with `if (slot->freed) continue;`. Without a
     * correction here, a fork() can land while this free index sits unused
     * in free_indices, and the walk then dereferences a NULL ptr in
     * ccol_mutex_lock(slot->ptr->fields_mutex) and crashes. So the code
     * below restores both fields explicitly, giving the slot the same
     * "freed" shape that a slot always has after clog_close() retires it,
     * which makes the rollback safe whichever of the two branches above made
     * `slot`. */
    slot->freed = true;
    slot->ptr = NULL;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_INVALID;
  }

  slot->generation++;
  if (slot->generation == 0) slot->generation++; /* skip the sentinel value */

  clog h = ((clog)idx << 32) | (clog)slot->generation;

  /* The library writes this before it publishes the handle, so a resolver
   * that finds this logger also finds the handle that its own unpin
   * needs. */
  lg->self_handle = h;

  /* The publish step can allocate a chunk or a stripe block on its first use,
   * and a failure leaves the slot unpublished, so the handle then resolves to
   * nothing. Roll the slot back exactly as the live_shareds failure above does,
   * instead of handing out a handle that no call can ever resolve. */
  if (!ccol_pintable_publish(&clog_pintable, idx, slot->generation, lg)) {
    /* This is the one failure that can happen after the library has already
     * registered lg->shared, so the code must undo that registration here.
     * Without that, a pointer to a shared object stays in the set that
     * _clog_atfork_prepare walks and locks, while the caller gets
     * CLOG_INVALID and goes on to free that object; the next fork() in the
     * process then locks a mutex inside freed memory.
     *
     * A pop of the tail is exactly right here, and no search is needed: the
     * library only ever changes live_shareds under the write lock that this
     * function holds for its whole body, so if this call pushed at all
     * (which happens exactly when the entry was not already registered), its
     * entry is still the last one. */
    if (!already_registered) {
      clog_shared_t *rolled_back = NULL;
      cvector_pop_back(clog_slot_table.live_shareds, &rolled_back);
    }
    /* The code clears this because the slot goes back on the free list: a
     * handle that stayed here would name a slot that belongs to another
     * object, and an unpin that carries such a handle charges the count of
     * that object for a pin that nobody took. */
    lg->self_handle = 0;
    slot->freed = true;
    slot->ptr = NULL;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_INVALID;
  }

  slot->ptr = lg;
  slot->in_use = true;
  slot->freed = false;

  ccol_rw_lock_unlock(clog_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         FORK SAFETY                                       */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/*
 * fork() duplicates only the thread that calls it, so if some OTHER thread
 * holds a lock at that exact instant (clog_slot_table.rwlock, a
 * shared->mutex or a fields_mutex), the child inherits that lock in a state
 * that stays locked for ever, because the thread that would unlock it does
 * not exist there. The pthread_atfork idiom below is the standard, correct
 * answer. _prepare locks every such lock, so fork() continues only at a
 * moment when no thread holds one. _parent unlocks them all after fork()
 * returns in the parent. _child unlocks every mutex too: the logical
 * execution of the thread that forked continues as the only thread of the
 * child, so for a mutex this is a normal, valid unlock and not a trick that
 * resets a lock that a dead thread owns. The one exception is
 * clog_slot_table.rwlock, which the child initializes again; see the
 * comment above _clog_atfork_release for why.
 *
 * clog_slot_table.live_shareds is what protects shared->mutex, instead of a
 * walk over the handles that have in_use set. The slot of a handle can
 * already show in_use == false while clog_close() still locks or holds
 * shared->mutex a few steps later (see the numbered sequence in
 * clog_close()), so a walk that gates on in_use misses the mutex of that
 * shared object inside exactly that window. The freed flag protects
 * fields_mutex instead (see the doc comment of clog_slot_t.freed), for the
 * same reason: in_use goes false before _logger_free() destroys
 * fields_mutex, and an operation on a third thread that holds a pin at that
 * moment (clog_set_field() and the related functions are such operations)
 * can legitimately still hold that mutex inside the same window.
 */
static void _clog_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(ccol_atfork_module_clogger);
#endif
  /* This file has a standing rule for the pthread wrappers: every function
   * that touches clog_slot_table.rwlock directly must guard it with this
   * same ccol_call_once. pthread_atfork() can call this function only after
   * _clog_slot_table_init_globals() registers it, so that function has
   * already run, but the guard does not depend on that call-graph
   * reasoning, which breaks silently the moment that a future change adds a
   * call path that nobody expected. So the guard stays unconditional. */
  ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  ccol_rw_lock_wrlock(clog_slot_table.rwlock);

  size_t ns = cvector_elem_count(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared_t *sh =
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
    ccol_mutex_lock(sh->mutex);
  }

  size_t n = cvector_elem_count(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, i);
    if (slot->freed) continue;
    ccol_mutex_lock(slot->ptr->fields_mutex);
    if (cvector_push_back(_clog_atfork_state.locked_fields, &slot->ptr) !=
        ccol_success) {
      /* The record of this lock failed because memory ran out, so the code
       * unlocks it again immediately instead of leaving the lock held with
       * nothing in locked_fields; _clog_atfork_release() would then never
       * unlock it, which deadlocks the field operations of this one logger
       * for ever, in the parent and in the child. Skipping the fields_mutex
       * protection of this single logger for this one fork() is the safe,
       * bounded degradation. Because fork() only ever duplicates the thread
       * that calls it, the window that this leaves open is the same narrow
       * race that only an out-of-memory state can reach, which the code
       * already accepts for a `freed` slot (which the loop skips above) and
       * for a shared object that never reached live_shareds at all. */
      ccol_mutex_unlock(slot->ptr->fields_mutex);
    }

    if (!slot->in_use) {
      /* A clog_close() on this exact handle is suspended on some OTHER thread
       * at this moment, between its own step 2, which clears in_use, and its
       * own step 4, which retires the slot and frees the reference of sh (see
       * the numbered steps of clog_close()). That thread does not exist in a
       * child that a fork just made, so without this record nothing in the
       * child ever retires this slot or frees the share that this handle
       * holds of sh->ref_count: the child leaks raw for ever, and also sh
       * itself once the library closes every other handle for sh. This
       * record lets _clog_atfork_release() finish this close for the thread
       * that vanished, in the child only; the real thread that closes is
       * present in the parent and is not affected, so it needs no help. A
       * record here can fail when memory runs out, which is a bounded
       * degradation of the same shape as the locked_fields push failure just
       * above: the code accepts the leak of this one slot in a future child
       * instead of risking anything in the parent to avoid it. */
      clog_atfork_closing_t c = {.idx = (uint32_t)i, .raw = slot->ptr};
      cvector_push_back(_clog_atfork_state.closing_slots, &c);
    }
  }
}

/*
 * In the child, the code initializes the rwlock of the slot table again
 * instead of unlocking it. The only thread of the child can call a plain
 * pthread_rwlock_unlock() on the write lock, but that call does NOT release
 * a lock that the continuation of that same thread took in the parent: the
 * rwlock write lock of glibc tracks its owner by TID internally, and the
 * thread of the child after the fork has a different TID from the thread of
 * the parent that forked. So the unlock fails silently, and every later
 * resolve in the child then hangs for ever in ccol_rw_lock_rdlock on this
 * exact rwlock. A plain mutex of the default type does not behave this way,
 * because it has no TID tracking (this codebase uses the fast and normal
 * mutex type everywhere; see ccol_mutex_init() in common.h), so only the
 * rwlock needs this treatment, and sh->mutex and fields_mutex do not.
 *
 * A fresh initialization of the lock in the child, in place of an unlock, is
 * the standard and well-established handling for this exact scenario; the
 * malloc arena locks of glibc do the same. It is safe because the child has
 * exactly one thread and nobody else can wait on the lock, so there is no
 * other party for a fresh initialization to race.
 */
static void _clog_atfork_release(bool in_child) {
  /* See the same guard, and its doc comment, at the top of
   * _clog_atfork_prepare(). This function also touches
   * clog_slot_table.rwlock, further down and for every caller, so it must
   * carry the same ccol_call_once guard for the same reason, without
   * depending on _clog_atfork_prepare() having run the guard moments earlier
   * for this exact fork(). */
  ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  size_t nf = cvector_elem_count(_clog_atfork_state.locked_fields);
  for (size_t i = nf; i-- > 0;) {
    struct clogger *lg =
        *(struct clogger **)cvector_at(_clog_atfork_state.locked_fields, i);
    if (in_child) {
      /* A pin stays held for as long as SOME thread holds a resolved pointer
       * to this handle, as any thread inside a call to one of the log_
       * macros does, and so does a thread inside one of the other public
       * clog_ functions on that handle. clog_close() polls the count until
       * it reaches 0, with no upper bound on the wait. Because fork()
       * duplicates only the thread that calls it, a pin that any OTHER
       * thread holds at fork() time can never be released: only the later
       * _clog_resolve_unpin() call of that thread ever decrements the count,
       * and that thread does not exist in this child at all. The child
       * inherits the pin, so without this reset the first clog_close() on
       * that exact handle in the child hangs for ever. This is not a rare
       * corner case but the ordinary result of a fork while any other
       * thread is in the middle of a log call on a logger that is still
       * open; the scenario of
       * fork_safety.concurrent_fork_during_churn_does_not_hang is one
       * example. A child that a fork just made has exactly one thread, so no
       * resolver that is truly still in flight can exist here. There is one
       * exception: a call to fork() itself from inside such a call on this
       * same handle and on this same thread, where this reset counts that
       * thread's own pin too low. That case is deliberately not supported,
       * and the other atfork handling in this file declines to chase the
       * same class of reentrancy: see the handling of sh->async_enabled and
       * of the compression state below, which also assumes that the thread
       * that forks is not itself in the middle of an operation on the shared
       * target. */
      ccol_pintable_reset_for(&clog_pintable, lg->self_handle);
    }
    ccol_mutex_unlock(lg->fields_mutex);
  }

  size_t ns = cvector_elem_count(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared_t *sh =
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
    if (in_child) {
      /* The compressor thread of this shared target does not exist in the
       * child, because fork() duplicates only the thread that calls it. The
       * compression that it runs at fork() time, and every job in its queue,
       * belong to the parent, whose compressor thread finishes them there on
       * the same files. So the child forgets all of them: it must not
       * compress them a second time, and it must not treat the generation of
       * a job that no thread of the child runs as protected from pruning for
       * ever. The jobs and compress_cv stay allocated and untouched, for the
       * same reason as the async queue below. Since compress_cv can have the
       * vanished thread as a waiter, a later rotation in the child
       * initializes it afresh when it starts a compressor thread of its
       * own. */
      sh->compress_running = NULL;
      sh->compress_head = NULL;
      sh->compress_tail = NULL;
      sh->compressor_live = false;
      sh->compressor_stop = false;
      sh->compress_prune_deferred = false;
    }
    if (in_child && sh->async_enabled) {
      /* This shared target depends on a writer thread, which does not exist in
       * the child at all because fork() duplicates only the thread that
       * calls it. The internals of ccol_circular_queue and
       * ccol_dynamic_queue are opaque, and there is no safe way to reach
       * into them and reset whatever lock state the child inherited, so the
       * code does not try. Every code path in this file already gates ALL
       * access to sh->q, sh->writer_thread and sh->async_buf behind this one
       * flag, so changing the flag here means that nothing in the child ever
       * touches that queue, buffer and thread state again; their locks may
       * be inconsistent, and they stay untouched for the rest of the life of
       * this process. A clog_close() on this handle in the child therefore
       * takes the plain synchronous teardown path, because its own check,
       * "if (sh->async_enabled) _shared_async_teardown(sh);", is false at
       * that point. It never tries to join a thread that the fork never
       * duplicated into this process, and never sends a sentinel into a
       * queue with no reader. The child never frees the backing memory of
       * the queue and the buffer, because no thread is left to do that job.
       * This is an accepted, inherent leak for the rest of the life of the
       * child process, of the same kind as any other kernel-level resource
       * that a forked child abandons instead of reclaiming. */
      sh->async_enabled = false;
      /* Nothing in this child ever delivers the contents of async_buf, so the
       * missing continuation of a record never arrives. This line clears the
       * flag, which lets the synchronous writes of the child rotate; a flag
       * that stays set suppresses every rotation for the rest of the life of
       * this process, each one waiting on a flush that cannot happen. */
      sh->async_partial_record = CLOG_SPLIT_NONE;
    }
    ccol_mutex_unlock(sh->mutex);
  }

  if (in_child) {
    /* This finishes every clog_close() call that a fork() suspended in the
     * middle of its teardown, on behalf of the thread that never resumes that
     * call in this child (see the doc comment of clog_atfork_closing_t and
     * the record of these calls in _clog_atfork_prepare()). This code follows
     * steps 3, 4, 5 and 6 of clog_close() in order, running them one after
     * another rather than concurrently. Because the child has exactly one
     * thread and nothing else can observe this state yet, it needs no lock
     * beyond the write lock of the slot table, which the caller of this
     * function (the machinery of pthread_atfork) already holds across the
     * whole prepare, parent and child sequence. This code runs only after
     * the two loops above, so by the time that it can tear down a shared
     * target, the downgrade of async_enabled and of the compression state
     * for that target has already happened; it therefore never tries to join
     * a writer thread or a compressor thread that does not exist in this
     * child, or to drain a queue that does not exist there. */
    size_t nc = cvector_elem_count(_clog_atfork_state.closing_slots);
    for (size_t i = 0; i < nc; i++) {
      clog_atfork_closing_t *c = (clog_atfork_closing_t *)cvector_at(
          _clog_atfork_state.closing_slots, i);
      clog_slot_t *slot =
          (clog_slot_t *)cvector_at(clog_slot_table.slots, c->idx);
      /* Only this loop itself can have retired the slot, because the real
       * thread that closes never resumes in this child at all, so this
       * condition can never be true with the current call graph. The code
       * guards it anyway instead of assuming it, following the standing
       * discipline of this file: do not depend on call-graph reasoning
       * alone. */
      if (slot->freed) continue;

      struct clogger *raw = c->raw;
      clog_shared_t *sh = raw->shared;

      /* This is the answer to step 3: a resolver that does not exist in this
       * child can never release the pin of this handle, for the same reason
       * as the reset of the pin count in the locked_fields loop above. The
       * code does it again here for every such slot, because a slot that the
       * prepare step records as "closing" is not certain to have reached
       * locked_fields as well: the same out-of-memory degradation that the
       * push above documents can stop it. */
      ccol_pintable_reset_for(&clog_pintable, raw->self_handle);

      /* Step 4: retire the slot. The code has already unlocked fields_mutex
       * above, before _logger_free() destroys it here: the locked_fields
       * loop unlocks it for every slot, and if the record of that lock in
       * _clog_atfork_prepare() failed, that function unlocks it inline
       * instead. */
      _logger_free(raw);
      slot->freed = true;
      slot->ptr = NULL;
      slot->generation++;
      if (slot->generation == 0) slot->generation++; /* skip the sentinel */
      cvector_push_back(clog_slot_table.free_indices, &c->idx);

      /* Steps 5 and 6: free the share that this handle holds of
       * sh->ref_count, and tear sh down if this was the last share, exactly
       * as the thread that vanished would have done. It leaves out the join
       * of the async writer thread and the teardown of the queue, which are
       * not needed here: the loop above already forced sh->async_enabled to
       * false for this child, and after a fork there is no writer thread and
       * so no job in flight to drain. */
      if (--sh->ref_count == 0) {
        _clog_shareds_remove(clog_slot_table.live_shareds, sh);
        _clog_shareds_remove(clog_slot_table.file_shareds, sh);
        _shared_close_owned_fd(sh);
        ccol_mutex_destroy(sh->mutex);
        _shared_free_partial(sh);
      }
    }
  }

  if (in_child) {
    /* A target in file_shareds that is not in live_shareds is in the middle
     * of its last close on a thread of the parent, which this child does not
     * have. Nothing in the child ever finishes that close, and its mutex can
     * stay locked, so an open of the same file in the child must not wait
     * for it, and the child forgets it. */
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
    if (ccol_rw_lock_reinit_in_child(clog_slot_table.rwlock) != 0)
      ccol_fatal_err("clog atfork release: failed to reinit slot table rwlock");
  } else {
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }
  cvector_reset(_clog_atfork_state.locked_fields);
  /* This runs for every caller, not only in the child. The parent branch
   * never processes closing_slots, because the real thread that closes is
   * present there and does its own teardown in the normal way, but
   * prepare() fills this vector before every fork() whether or not the
   * entries turn out to be needed. Stale entries left here after a release
   * on the parent side would stay for the next fork(), whose prepare() then
   * appends on top of them, and a later fork that truly needs this list
   * would wrongly "finish" slots that the library retired long ago and that
   * a different logger may already use. */
  cvector_reset(_clog_atfork_state.closing_slots);
}

static void _clog_atfork_parent(void) { _clog_atfork_release(false); }
static void _clog_atfork_child(void) { _clog_atfork_release(true); }
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/*
 * This is a defensive cleanup at process exit. It follows the defensive
 * variant, _cleanup_default_client in chttpclient.c, and not the
 * unconditional variant in chttpserver.c, because a process is expected to
 * hold many clog handles and to own each one independently, with no one
 * obvious owner that must close everything before exit. So this function
 * frees the bookkeeping vectors of the slot table only once nothing can
 * still reach them. A handle can still be open, or a close can still be in
 * flight, and a destructor or an atexit handler in another translation unit
 * can run later; freeing the vectors under such a handler risks a
 * use-after-free. In that case the code hands the release to whichever close
 * is the last one out instead of skipping the release. A program that does
 * close its loggers therefore leaves nothing behind, whatever order the
 * destructors ran in, and a program that does not close them leaves them for
 * the OS to reclaim. */
/* This answers whether any slot still names a logger. The caller holds the
 * write lock.
 *
 * The scan reads slot->ptr, not slot->in_use. A close clears in_use as its
 * first step, so that it rejects a second close or a new resolve as early as
 * possible, and the rest of the teardown (which drains the pins and does the
 * final locked release of the index) runs after that step. A scan that
 * trusted in_use alone would free this table under a close that is still in
 * that window, and the last step of that close would then index the table.
 * The library writes ptr only once it fully acquires a slot, and clears ptr
 * only in that final locked step.
 *
 * Even ptr does not cover the whole window: a close clears ptr and then
 * takes the write lock again to remove its shared object from live_shareds,
 * and between those two points it is invisible here while it still needs the
 * table. closes_in_flight covers exactly that remainder, which is why every
 * decision to release goes through _clog_table_still_needed_locked() and not
 * through a direct call to this function. */
/* This answers whether anything still needs this table: either a slot that
 * still names a logger, or a close that has already cleared its slot but is
 * not finished with the other vectors of the table. The caller holds the
 * write lock. Both deciders use this function rather than the slot scan
 * alone, because a close in its own final steps is invisible to that scan by
 * construction. */
static bool _clog_table_still_needed_locked(void);

static bool _clog_any_slot_live_locked(void) {
  size_t n = cvector_elem_count(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, i);
    if (slot->ptr != NULL) return true;
  }
  return false;
}

/* This frees the bookkeeping of the table and the pin index. The caller
 * holds the write lock and has established that nothing still needs this
 * table.
 *
 * The library deliberately never frees the slot storage of the pin index
 * while the process runs, because a resolve indexes that storage with no
 * lock held, so a leak checker that treats still-reachable memory as an
 * error reports it at exit unless this function frees it here. The code sets
 * each vector to NULL as it goes, which makes a later call answer "already
 * released" instead of indexing a freed vector. This function does not
 * destroy the rwlock, because it can run from an ordinary close that still
 * holds that lock. */
/* The check and the release both sit behind one call that is not inlined,
 * so the destroy path that must make that call keeps the code shape that it
 * has without any of this. Cold code in a hot object file is not free:
 * inlined here, the same few instructions measurably slow the push path of
 * an unrelated container, because they shift what the linker lays out
 * around that path, while the instruction count does not change. */
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
 * thread that holds the lock longer is stuck inside this module, and the
 * release then leaves the table allocated instead of holding up the end of
 * the process for ever. */
#define CLOG_TABLE_RELEASE_WAIT_MS 1000

__attribute__((destructor)) static void _cleanup_clog_slot_table(void) {
  if (!clog_slot_table.slots)
    return; /* never initialized, or already released */
  /* The code holds this lock across the scan and everything that the scan
   * decides, as the equivalent teardown of every other module holds its own
   * lock. A thread can still call into this module while the process tears
   * itself down, and without the lock such a thread can be inside an acquire
   * and publish a chunk into the very index that this function frees. The
   * wait is bounded; see CLOG_TABLE_RELEASE_WAIT_MS. */
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += CLOG_TABLE_RELEASE_WAIT_MS / 1000;
  until.tv_nsec += (long)(CLOG_TABLE_RELEASE_WAIT_MS % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000L;
  }
  if (ccol_rw_lock_timedwrlock(clog_slot_table.rwlock, until) != 0) return;
  /* This runs only once nothing is left that can still resolve a handle.
   * When a logger is still open, the code hands the release to whichever
   * close is the last one out instead of skipping the release, so an
   * application that does close its loggers leaves nothing behind, whatever
   * order the destructors ran in. */
  if (_clog_table_still_needed_locked()) {
    clog_slot_table.release_deferred = true;
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
    return;
  }
  _clog_release_slot_table_locked();
  ccol_rw_lock_unlock(clog_slot_table.rwlock);
  /* The code deliberately does not destroy the rwlock itself. It has static
     storage duration, so a lock left behind holds nothing that a leak
     checker reports, and the destructor cannot own its lifetime in any case:
     the deferred branch above returns while the table is still live, and the
     release that happens later runs while it holds this very lock, so there
     is no path on which every user is provably finished with it. A destroy
     here would leave the entry points of the other branch taking a lock on
     an object that has been destroyed, which is undefined; without the
     destroy, those entry points reach a defined abort instead. A call that
     happens after the release of the table stops at an assertion inside the
     vector that it indexes, and a call on a stale handle stops at the
     ccol_fatal_err of this module. The order of the destructors across
     translation units is not for this library to decide, so neither outcome
     can be ruled out by an arrangement of who runs first. */
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

/* This raises the growth ceiling of b above its CLOG_BUF_MAX default, and
 * never lowers it below that default; see the doc comment of
 * clog_buf_t.cap_limit. _shared_async_init() calls it once, immediately
 * after it initializes clog_shared_t.async_buf, so that a caller who
 * configures a clog_async_cfg_t.flush_buffer_size larger than CLOG_BUF_MAX
 * truly gets that much room for the batch, instead of having it silently
 * capped at the smaller default, which is meant for one record. */
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
 * This lets a test force the NEXT _buf_append() or _buf_appendf() call to
 * report a real allocation failure, deterministically, whether or not the
 * call truly needs the buffer to grow. The check sits at the top of both
 * functions, not inside _buf_ensure(), which _buf_appendf() calls only after
 * its own first attempt to fit in place fails. The position is deliberate:
 * it lets this hook force a failure even for a small append of a fixed size
 * that never needs to grow the buffer at all, such as the "backtrace
 * unavailable" marker of _emit_backtrace_syslog_lines(), which the real
 * CLOG_BUF_INITIAL value of this codebase already fits easily with no
 * growth at all. Without this hook, a test can never observe a real
 * allocator failure there. The hook disarms itself the moment that it fires,
 * so it affects only the one call that a test targets.
 */
static _Atomic bool _clog_test_force_next_buf_ensure_failure = false;

static bool _clog_test_consume_forced_buf_ensure_failure(void) {
  return atomic_exchange(&_clog_test_force_next_buf_ensure_failure, false);
}

void clog_test_force_next_buf_ensure_failure(bool force) {
  atomic_store(&_clog_test_force_next_buf_ensure_failure, force);
}
#else
static inline bool _clog_test_consume_forced_buf_ensure_failure(void) {
  return false;
}
#endif

/* This makes sure that at least `need` free bytes are available. Two
 * different causes of failure both report -1 to the caller in the same way,
 * and only the second one sets b->oom. Reaching b->cap_limit is an ordinary
 * rejection about size, which happens for a record that is too large and for
 * a shared batch buffer with too little room left; it is never a sign of
 * real memory pressure, and a caller further up must never read it as one
 * (see the doc comment of clog_buf_t.oom). */
static int _buf_ensure(clog_buf_t *b, size_t need) {
  if (b->cap - b->len >= need) return 0;
  size_t new_cap = b->cap ? b->cap : CLOG_BUF_INITIAL;
  while (new_cap - b->len < need) {
    if (new_cap >= b->cap_limit) return -1;
    /* b->cap_limit can be a large value that the caller configures (see
     * _buf_raise_cap_limit()), not always a fixed compile-time constant, so
     * the code must guard the doubling against a size_t overflow instead of
     * assuming that new_cap*2 always fits.
     * A clamp straight to cap_limit here must NOT skip the sufficiency check of
     * the loop, as an unconditional `break` would: cap_limit can be reachable
     * and still be smaller than what `need` asks for, a return of success would
     * then falsely report the buffer as large enough, and the caller would run
     * memcpy or vsnprintf past the end of the allocation. The code goes back to
     * the `while` condition instead, where the ordinary `new_cap >=
     * b->cap_limit` check catches a cap_limit that is too small on the very
     * next iteration, just as the ordinary doubling path, the one with no
     * overflow, depends on that same check. */
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
 * This fills esc, a buffer of 5 bytes or more, with the backslash escape
 * sequence for a control byte c (one with c < 0x20 or c == 0x7f), and sets
 * *esc_len to the length of the sequence: 2 for \n, \r and \t, and 4 for the
 * general \xXX form. Every value escaper in this file (the ones for logfmt,
 * for an RFC 5424 SD-PARAM-VALUE and for a syslog MSG) uses this function,
 * so a control byte is always neutralised in the same way, whatever the
 * format is.
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
 * This appends a value that is safe for logfmt. When a value contains a
 * space, '=', '"', '\\' or a control character, the function puts it in
 * double quotes and escapes it with backslashes. It writes an empty string
 * as "".
 */
static int _buf_append_lv(clog_buf_t *b, const char *s) {
  if (!s) return _buf_append(b, "null", 4);

  bool quote = (s[0] == '\0');
  if (!quote) {
    for (const char *p = s; *p && !quote; p++) {
      unsigned char c = (unsigned char)*p;
      if (c < 0x20 || c == 0x7f || *p == ' ' || *p == '=' || *p == '"' ||
          *p == '\\')
        quote = true;
    }
  }

  if (!quote) return _buf_append(b, s, strlen(s));

  if (_buf_append(b, "\"", 1) != 0) return -1;

  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;

    if (*p == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (*p == '"' || *p == '\\') {
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
    }
  }

  return _buf_append(b, "\"", 1);
}

/* ========================================================================== */
/*                         JSON HELPERS                                       */
/* ========================================================================== */

/*
 * This classifies the UTF-8 multi-byte sequence that starts at the
 * non-ASCII lead byte p[0], which is in the range 0x80 to 0xff. It gives the
 * total length of the sequence (2, 3 or 4) when the sequence is complete and
 * well formed: no overlong encoding, no encoded surrogate half (U+D800 to
 * U+DFFF), no codepoint beyond U+10FFFF, and every continuation byte present
 * and in range. It gives 0 in two cases: when p[0] is itself an invalid lead
 * byte (a lone continuation byte, 0x80 to 0xbf; an overlong 2-byte lead,
 * 0xc0 or 0xc1; or a byte from 0xf5 to 0xff, which can never encode a valid
 * codepoint), and when the sequence is truncated or malformed in another
 * way. The function never reads past the NUL terminator of p, because it
 * checks each continuation byte against '\0' before it examines that byte
 * further, so a sequence that the end of the string truncates is correctly
 * reported as invalid without a read beyond the string.
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
    hi1 = (c0 == 0xed) ? 0x9f : 0xbf; /* reject the ed a0..bf range, which is
                                          a surrogate half */
  } else {
    len = 4;
    lo1 = (c0 == 0xf0) ? 0x90 : 0x80; /* reject the overlong f0 80..8f range */
    hi1 = (c0 == 0xf4) ? 0x8f : 0xbf; /* reject the f4 90..bf range, which is
                                          beyond U+10FFFF */
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
 * This appends the JSON-escaped content of s, with no quotes around that
 * content. The escapes are: '"' -> '\"', '\' -> '\\', '\n'->'\n',
 * '\r'->'\r', '\t'->'\t', and every other control character -> '\uXXXX'. A
 * byte or byte sequence that is not well-formed UTF-8 (a lone continuation
 * byte, a lead byte that is overlong or out of range, an encoded surrogate
 * half, or part of a truncated multi-byte sequence) is replaced, one invalid
 * byte at a time, with the Unicode replacement character (U+FFFD). So a
 * message or a field value built from any data, binary data included, can
 * never make this function write invalid Unicode inside a JSON string; RFC
 * 8259 needs JSON text to be valid Unicode, and a strict parser or log
 * aggregator downstream has the right to reject a document that holds raw
 * bytes that are not UTF-8. A well-formed multi-byte sequence otherwise
 * passes through with no change and no escape, exactly like any other
 * printable byte. s must not be NULL.
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
      int seq_len = _utf8_valid_seq_len((const unsigned char *)p);
      if (seq_len > 0) {
        p += seq_len;
        continue;
      }
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      if (_buf_append(b, "\\ufffd", 6) != 0) return -1;
      p++;
      run = p;
      continue;
    }
    p++;
  }
  return 0;
}

/*
 * This appends a JSON key-value pair with a comma in front: ,"key":"value"
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
 * This appends an RFC 5424 SD-PARAM-VALUE. The function escapes '"', '\' and
 * ']' as '\"', '\\' and '\]', and also escapes every control character with
 * a backslash, in the same way as the logfmt and JSON output formats. That
 * includes '\n' and '\r': the formal SD-PARAM-VALUE grammar does not
 * restrict those two, but without the escape they split one syslog record
 * across more than one line. RFC 5424 section 6.3.3 requires the value to be
 * UTF-8, so the function passes a well-formed UTF-8 sequence through as it
 * is and replaces each byte that is not part of one with U+FFFD, encoded as
 * UTF-8, exactly where the JSON format writes the same character as an
 * escape (see _buf_append_json_content()). The function writes an s of NULL
 * as the bare word "null", just as _buf_append_lv() and
 * _buf_append_json_kv() each treat a NULL value in their own formats. Since
 * the caller already puts quotes around this call, the word "null" here
 * looks unquoted inside an SD-PARAM-VALUE that is otherwise quoted, exactly
 * like the unquoted bare word `null` of logfmt.
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
      int seq_len = _utf8_valid_seq_len((const unsigned char *)p);
      if (seq_len > 0) {
        p += seq_len - 1; /* the loop steps past the last byte */
        continue;
      }
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      if (_buf_append(b, "\xef\xbf\xbd", 3) != 0) return -1;
      run = p + 1;
    }
  }
  return 0;
}

/*
 * This fills name, a buffer of 33 bytes or more, with an RFC 5424
 * SD-PARAM-NAME that it derives from key. A key that already fits inside the
 * limit of 32 characters goes in with no change. A longer key gives the
 * first 23 characters of key, then '~', then an FNV-1a hash of the FULL key
 * as 8 hexadecimal digits, which is 32 characters in total. A plain
 * truncation of the prefix is not enough, because two different keys that
 * share a prefix of 32 characters would then collide into the same
 * SD-PARAM-NAME, which RFC 5424 forbids inside one SD-ELEMENT. Since the
 * whole key goes into the suffix, such a collision needs a real hash
 * collision and not only a shared prefix.
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
 * This appends free-form text and escapes every control character with a
 * backslash. It also writes a backslash as '\\', as logfmt does inside a
 * quoted value, so the escaped text reads back unambiguously: a real newline
 * appears as '\n', and the two characters '\' and 'n' appear as '\\n'. It
 * adds no quotes and no other structure around the text. The library uses it
 * for content that has no formal escape syntax of its own but must still not
 * be able to split a record across more than one line, such as RFC 5424 MSG
 * content and, in the logfmt and syslog formats, the text of a backtrace
 * frame, which comes from backtrace_symbols(). JSON does not use this
 * function for a backtrace frame: it puts each frame in as an ordinary JSON
 * array element, through _buf_append_json_content(), which has its own
 * escapes. Without escaping, a control byte can desynchronize the output
 * stream, '\n' and '\r' most of all, so this function neutralises every one
 * of them in the same way as the logfmt and JSON formats do in their own
 * message fields and value fields. s must not be NULL.
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
    }
  }
  return 0;
}

/*
 * This sanitizes raw into out, a buffer of size outsz, for use as an RFC
 * 5424 PRINTUSASCII field; APP-NAME and HOSTNAME share the same
 * "1*NNNPRINTUSASCII" grammar. The function keeps only the PRINTUSASCII
 * bytes (0x21 to 0x7e) and drops any other byte instead of stopping at the
 * first one, so one byte that does not qualify, such as a space, a control
 * character or a non-ASCII byte, does not discard everything after it in
 * raw. The function writes at most outsz-1 bytes of output, and falls back
 * to "-" when raw gives no PRINTUSASCII byte at all (an empty raw is one such
 * case); that fallback needs room, which means outsz >= 2.
 *
 * An outsz of 0 is allowed and does nothing: the function never touches out,
 * because out has no byte in which it can safely write even a NUL
 * terminator. For every outsz >= 1, out is certain to be NUL-terminated
 * inside its first outsz bytes and never beyond them. The loop bound below
 * is deliberately `i + 1 < outsz`, not the `i < outsz - 1` that looks
 * equivalent: outsz is a size_t, so outsz - 1 underflows to SIZE_MAX for
 * outsz == 0, which silently defeats the bound completely and lets the loop
 * below write any distance past an out whose size is truly zero.
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
 * This gives the short program name that the platform reports, or NULL when
 * the platform has no such facility or reports an empty name. The two
 * callers below share it and differ only in their fallback for a result of
 * NULL: the general "proc" field, which every log line carries, falls back
 * to "unknown", and the APP-NAME field of RFC 5424 falls back to "-", which
 * is the sentinel for "unavailable" in its own grammar.
 */
static const char *_raw_progname(void) {
#if defined(__GLIBC__)
  if (program_invocation_short_name && program_invocation_short_name[0])
    return program_invocation_short_name;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  const char *p = getprogname();
  if (p && p[0]) return p;
#endif
  return NULL;
}

/* Give the process name for the RFC 5424 APP-NAME field, or "-" when that
 * name is not available. */
static const char *_syslog_appname(void) {
  const char *p = _raw_progname();
  return p ? p : "-";
}

/* Give the basename of the executable, or "unknown" when it is not
 * available. */
static const char *_get_progname(void) {
  const char *p = _raw_progname();
  return p ? p : "unknown";
}

/* Give the OS-level thread ID of the thread that calls. */
static pid_t _get_tid(void) {
#if defined(__linux__)
  return (pid_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
  uint64_t tid64 = 0;
  ccol_get_thread_id_np(NULL, &tid64);
  return (pid_t)tid64;
#elif defined(__FreeBSD__)
  return (pid_t)pthread_getthreadid_np();
#else
  /* This platform has no portable way to get a real OS-level thread ID, so
   * the code falls back to the pthread handle itself. That handle is unique
   * for each thread, so a reader can tell the log lines of different threads
   * apart, but unlike the branches above it gives a value that does not
   * match the OS-level tools (ps -eLf, /proc/<pid>/task, top -H and
   * others). */
  return (pid_t)(uintptr_t)ccol_get_thread_id();
#endif
}

/* Fill buf with the name of the thread that calls, writing at most bufsz-1
 * characters. */
static void _get_thread_name(char *buf, size_t bufsz) {
#if defined(__linux__)
  char name[16]; /* prctl writes at most 16 bytes including null */
  if (prctl(PR_GET_NAME, name) == 0 && name[0]) {
    snprintf(buf, bufsz, "%s", name);
    return;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__)
  if (ccol_get_thread_name_np(ccol_get_thread_id(), buf, bufsz) == 0 && buf[0])
    return;
  /* A thread that nobody named has an empty name here, while Linux gives it
   * the name of the program, which ps(1) shows for it too. */
  snprintf(buf, bufsz, "%.15s", _get_progname());
  return;
#endif
  snprintf(buf, bufsz, "unknown");
}

/* ========================================================================== */
/*                         THREAD IDENTITY CACHE                              */
/* ========================================================================== */

/*
 * Every record names the process and the thread that wrote it, as
 * "<progname>(<pid>):<thread name>(<tid>)". Each of those three numbers and
 * names costs a system call to read (getpid(), gettid() and
 * prctl(PR_GET_NAME)), and none of them changes while a thread runs, except
 * the thread name. So each thread reads them once, the first time that it
 * logs, and keeps the formatted string in thread-local storage, and a record
 * then costs no system call for its identity.
 *
 * Two events change the identity, and each one refreshes the cache:
 *
 *   - fork(). The child has a new PID, and its one thread has a new TID. A
 *     fork child handler clears the cache of the thread that forked, which is
 *     the only thread of the child. That handler takes no lock, so its place
 *     among the handlers of the other modules does not matter. It is
 *     registered whatever CCOL_FORK_SAFETY_REQUIRED says, because it keeps the
 *     content of a record correct and protects no lock.
 *   - ccol_set_thread_name(). It renames the thread and updates the cache.
 *
 * A rename by any other means, such as pthread_setname_np() or
 * prctl(PR_SET_NAME), after the thread first logged, is not seen: the cache
 * holds the name that the thread had when it first logged.
 *
 * The cache lives on the heap, and the thread holds a pointer to it. The
 * first record of the thread allocates it, a thread-specific key frees it
 * when the thread exits, and the unload of the library frees the one of the
 * thread that unloads and deletes the key. A thread-local object of this
 * size would sit in the thread-local block of the library, which the loader
 * must fit into a small reserve when a process loads the library with
 * dlopen(). The pointer itself takes the initial-exec model, as the other
 * thread-local fast paths do (see internal/ctlsmodel.h), so a record reads
 * its identity with two loads and no call. When the allocation fails, a
 * record carries a fixed identity that names no thread, and the next record
 * of the thread tries again.
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
static ccol_once_flag_t _clog_tident_once = CCOL_ONCE_INIT;

/* The identity of a record whose thread could not get its cache. */
static const clog_thread_ident_t _clog_tident_unknown = {
    .valid = true, .pid = 0, .tid = 0, .name = "", .proc = "unknown"};

/* The key whose destructor frees the cache of a thread that exits. The
 * module destructor deletes it under the write lock of _clog_tident_rwlock,
 * and a thread sets its value under the read lock, after it checks
 * _clog_tident_key_live, so a set never runs on a key that was deleted.
 * _clog_tident_key_live stays false when the key or the lock could not be
 * created; a cache is then never freed at thread exit, which costs one
 * cache for each thread that ends. */
static ccol_thread_ls_key_t _clog_tident_key;
static ccol_rw_lock_t _clog_tident_rwlock;
static atomic_bool _clog_tident_key_live;

#ifdef RUNNING_UNIT_TESTS
/* How many times the calling thread read its identity from the system. */
static __thread unsigned _clog_test_tident_fills;

unsigned clog_test_thread_identity_fill_count(void) {
  return _clog_test_tident_fills;
}
#endif

static void _clog_tident_atfork_child(void) {
  if (_clog_tident) _clog_tident->valid = false;
}

/* The destructor of _clog_tident_key; arg is the cache of the thread. */
static void _clog_tident_free(void *arg) {
  free(arg);
  _clog_tident = NULL;
}

static void _clog_tident_register_atfork(void) {
  /* A registration that fails leaves a forked child with the identity of the
   * parent in its records; that is the only cost, and nothing else here
   * depends on the handler. */
  (void)ccol_at_fork(NULL, NULL, _clog_tident_atfork_child);
  if (ccol_rw_lock_init(_clog_tident_rwlock) != 0) return;
  if (ccol_thread_ls_key_create(_clog_tident_key, _clog_tident_free) != 0)
    return;
  atomic_store(&_clog_tident_key_live, true);
}

/* Runs when the library unloads, and at exit. It frees the cache of the
 * calling thread, which no key destructor frees for the thread that ends the
 * process, and deletes the key, whose destructor would otherwise point into
 * unmapped code for every thread that exits after a dlclose(). */
__attribute__((destructor)) static void _clog_tident_fini(void) {
  free(_clog_tident);
  _clog_tident = NULL;
  if (!atomic_load(&_clog_tident_key_live)) return;
  ccol_rw_lock_wrlock(_clog_tident_rwlock);
  atomic_store(&_clog_tident_key_live, false);
  ccol_thread_ls_key_delete(_clog_tident_key);
  ccol_rw_lock_unlock(_clog_tident_rwlock);
}

static void _clog_tident_format(clog_thread_ident_t *ti) {
  if (snprintf(ti->proc, sizeof(ti->proc), "%.200s(%d):%.15s(%d)",
               _get_progname(), (int)ti->pid, ti->name, (int)ti->tid) < 0)
    ti->proc[0] = '\0';
}

/* This reads the identity of the calling thread from the system and caches
 * it. It stays out of line because each thread runs it only once. */
static __attribute__((noinline, cold)) const clog_thread_ident_t *
_clog_tident_fill(void) {
  ccol_call_once(_clog_tident_once, _clog_tident_register_atfork);
  clog_thread_ident_t *ti = _clog_tident;
  if (!ti) {
    ti = malloc(sizeof(*ti));
    if (!ti) return &_clog_tident_unknown;
    if (atomic_load(&_clog_tident_key_live)) {
      ccol_rw_lock_rdlock(_clog_tident_rwlock);
      if (atomic_load(&_clog_tident_key_live))
        (void)ccol_thread_ls_set(_clog_tident_key, ti);
      ccol_rw_lock_unlock(_clog_tident_rwlock);
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

/* This gives the cached identity of the calling thread. The pointer stays
 * valid for as long as the calling thread runs, and only that thread may
 * read through it. */
static inline const clog_thread_ident_t *_clog_tident_get(void) {
  const clog_thread_ident_t *ti = _clog_tident;
  if (__builtin_expect(!ti || !ti->valid, 0)) return _clog_tident_fill();
  return ti;
}

ccol_retval_t ccol_set_thread_name(const char *thread_name) {
  if (!thread_name || thread_name[0] == '\0') return ccol_invalid_args;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
  /* Linux keeps at most 15 bytes of a thread name, and every platform keeps
   * the same 15, so a name reads back the same everywhere. */
  char name[CLOG_THREAD_NAME_MAX + 1];
  size_t n = strnlen(thread_name, CLOG_THREAD_NAME_MAX);
  memcpy(name, thread_name, n);
  name[n] = '\0';
#if defined(__linux__)
  if (prctl(PR_SET_NAME, name) != 0) return ccol_not_permitted;
#else
  if (ccol_set_own_thread_name_np(name) != 0) return ccol_not_permitted;
#endif
  clog_thread_ident_t *ti = _clog_tident;
  if (!ti || !ti->valid) {
    /* The first read of the identity takes the name that the kernel holds at
     * that point, which is this one. */
    (void)_clog_tident_fill();
  } else {
    memcpy(ti->name, name, n + 1);
    _clog_tident_format(ti);
  }
  return ccol_success;
#else
  /* This platform has no rename of the calling thread that the library
   * supports. */
  return ccol_not_permitted;
#endif
}

/* ========================================================================== */
/*                         I/O HELPER                                         */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/*
 * This is the cap on how many bytes each of the next
 * _clog_test_write_cap_uses calls of _write_all() can deliver, so that a
 * test can reproduce a short write, in which the kernel accepts only the
 * first part of a record (as a filesystem that is full with ENOSPC, or an
 * RLIMIT_FSIZE ceiling, makes it do), without putting the whole test
 * process under a real resource limit. The writes after the last capped
 * write succeed, which is what lets a test observe the repair of the
 * framing. A use count of 0 means that the cap is not armed. Each capped
 * write consumes one use, so the cap affects only the writes that a test
 * targets, and a count above one reproduces a descriptor that cannot take
 * the remainder of a record across more than one attempt.
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

void clog_test_force_short_writes(size_t accepted_bytes,
                                  unsigned int write_count) {
  atomic_store(&_clog_test_write_cap, accepted_bytes);
  atomic_store(&_clog_test_write_cap_uses, accepted_bytes ? write_count : 0u);
}

/*
 * The errno that each of the next _clog_test_write_error_uses calls of
 * _write_all() fails with, before it writes a byte, so that a test can
 * reproduce a write error of any class on a descriptor that otherwise works
 * and observe what the logger does with the bytes that did not go out.
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

void clog_test_force_write_errors(int err, unsigned int write_count) {
  atomic_store(&_clog_test_write_error, err);
  atomic_store(&_clog_test_write_error_uses, err ? write_count : 0u);
}
#else
static inline size_t _clog_test_consume_next_write_cap(void) { return 0; }
static inline int _clog_test_consume_next_write_error(void) { return 0; }
#endif

/* ========================================================================== */
/*                         SIGPIPE                                            */
/* ========================================================================== */

/*
 * A write(2) to a pipe or a socket whose reading end is gone fails with EPIPE
 * and also raises SIGPIPE for the writing thread. The default disposition of
 * SIGPIPE ends the process, which is a surprising way for a log call to stop
 * a healthy application, so the library arranges, by the kind of the
 * descriptor, that its own writes never raise it, and it touches the signal
 * state of the process only where no cheaper means exists:
 *
 *   - A regular file, a terminal and every other descriptor that is neither
 *     a pipe nor a socket cannot raise SIGPIPE, so their writes change
 *     nothing.
 *   - A socket is written with send(2) and MSG_NOSIGNAL, which fails with
 *     EPIPE and raises nothing. This changes no signal state, so it applies
 *     under every policy.
 *   - A pipe or a FIFO has no such flag. Under CLOG_SIGPIPE_AUTO the async
 *     writer thread keeps SIGPIPE blocked in its own mask and consumes the
 *     SIGPIPE that its own failed write left pending on it. Any other thread
 *     that writes to a pipe sink sets the disposition of the whole process to
 *     SIG_IGN, once, the first time it does so, and only when that
 *     disposition is still SIG_DFL; a handler or an ignore that the
 *     application installed stays as it is.
 *
 * Under CLOG_SIGPIPE_UNTOUCHED the library changes no disposition and no
 * signal mask, and a write to a broken pipe raises SIGPIPE as the
 * disposition of the application says.
 *
 * Every write that fails with EPIPE drops its record, like every other write
 * error that the library cannot recover from.
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
  /* A flag over one of the kinds above, which only a CLOG_FATAL call sets,
   * under the mutex of the target, for a sink that is not a regular file:
   * every write to the sink then ends by _clog_fatal_deadline (see
   * _clog_write_fatal()). CLOG_SINK_PLAIN with this flag is a terminal or
   * another device. */
  CLOG_SINK_FATAL_BOUND = 4,
} clog_sink_kind_t;

/* The policy that clog_set_sigpipe_policy() sets. It is read only on the
 * write path of a pipe sink, relaxed, because each write acts on the value
 * that it reads and orders nothing else by it. */
static _Atomic int _clog_sigpipe_policy = CLOG_SIGPIPE_AUTO;

static ccol_once_flag_t _clog_sigpipe_once = CCOL_ONCE_INIT;
/* Set once _clog_sigpipe_once has run, so that a later pipe write skips the
 * call. The disposition is kernel state, so the flag orders nothing. */
static atomic_bool _clog_sigpipe_once_done;

/* The SIGPIPE state of the calling thread. It is CLOG_WRITER_NONE on every
 * thread except an async writer thread of this module, which records there
 * whether SIGPIPE is blocked in its mask and who blocked it. */
enum {
  CLOG_WRITER_NONE = 0,      /* not a writer thread */
  CLOG_WRITER_OPEN = 1,      /* a writer thread; SIGPIPE is not blocked */
  CLOG_WRITER_BLOCKED = 2,   /* a writer thread; this module blocked it */
  CLOG_WRITER_INHERITED = 3, /* a writer thread that inherited it blocked */
};
/* The model is initial-exec, as for the thread-local state of cpintable and
 * cmempool: under the general model every access from a shared library is a
 * __tls_get_addr call, and this one sits on every write to a pipe. */
#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread unsigned char _clog_writer_sigpipe;
#else
static __thread unsigned char _clog_writer_sigpipe
    __attribute__((tls_model("initial-exec")));
#endif

/* Gives the clog_sink_kind_t of fd. A descriptor that fstat() cannot
 * examine is CLOG_SINK_PLAIN, because every write to it fails with EBADF,
 * which raises nothing. Without MSG_NOSIGNAL, a socket takes the path of a
 * pipe. */
static unsigned char _clog_classify_sink(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return CLOG_SINK_PLAIN;
  if (S_ISFIFO(st.st_mode)) return CLOG_SINK_PIPE;
  if (S_ISSOCK(st.st_mode))
    return CLOG_HAS_MSG_NOSIGNAL ? CLOG_SINK_SOCKET : CLOG_SINK_PIPE;
  return CLOG_SINK_PLAIN;
}

/* Gives true when fd is a message-oriented socket, which keeps the boundary
 * of each write (see clog_shared_t.sink_per_record), and false for every
 * other descriptor, including one that is not a socket at all. */
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

/* Sets the disposition of SIGPIPE to SIG_IGN when it is SIG_DFL. It runs
 * once for the process, under _clog_sigpipe_once. A handler or an ignore
 * that the application installed is its own decision, so the library leaves
 * it in place. */
static void _clog_ignore_sigpipe(void) {
  struct sigaction cur;
  if (sigaction(SIGPIPE, NULL, &cur) != 0) return;
  if ((cur.sa_flags & SA_SIGINFO) || cur.sa_handler != SIG_DFL) return;
  struct sigaction ign;
  memset(&ign, 0, sizeof(ign));
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  (void)sigaction(SIGPIPE, &ign, NULL);
}

/* Blocks or unblocks SIGPIPE in the mask of the calling async writer thread,
 * and records the result. See _clog_writer_sync_sigpipe_mask(). */
static __attribute__((noinline, cold)) void _clog_writer_set_sigpipe_mask(
    bool blocked) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  if (ccol_thread_sigmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL) == 0)
    _clog_writer_sigpipe = blocked ? CLOG_WRITER_BLOCKED : CLOG_WRITER_OPEN;
}

/* Brings the SIGPIPE mask of an async writer thread in line with the current
 * policy: blocked under CLOG_SIGPIPE_AUTO, as it inherited it under
 * CLOG_SIGPIPE_UNTOUCHED. The thread unblocks only what this module
 * blocked, and it never leaves a SIGPIPE of its own pending (see
 * _clog_consume_own_sigpipe()), so an unblock delivers nothing that this
 * module caused. A thread that inherited SIGPIPE blocked keeps the mask that
 * its creator gave it. */
static inline void _clog_writer_sync_sigpipe_mask(void) {
  unsigned char st = _clog_writer_sigpipe;
  if (st == CLOG_WRITER_INHERITED) return;
  bool want_blocked =
      atomic_load_explicit(&_clog_sigpipe_policy, memory_order_relaxed) ==
      CLOG_SIGPIPE_AUTO;
  if (want_blocked == (st == CLOG_WRITER_BLOCKED)) return;
  _clog_writer_set_sigpipe_mask(want_blocked);
}

/* The first statement of an async writer thread: it marks the thread as a
 * writer thread and applies the current policy to its mask. */
static __attribute__((noinline, cold)) void _clog_writer_sigpipe_init(void) {
  sigset_t cur;
  sigemptyset(&cur);
  if (ccol_thread_sigmask(SIG_BLOCK, NULL, &cur) == 0 &&
      sigismember(&cur, SIGPIPE) == 1) {
    _clog_writer_sigpipe = CLOG_WRITER_INHERITED;
    return;
  }
  _clog_writer_sigpipe = CLOG_WRITER_OPEN;
  _clog_writer_sync_sigpipe_mask();
}

static __attribute__((noinline, cold)) void _clog_ignore_sigpipe_once(void) {
  ccol_call_once(_clog_sigpipe_once, _clog_ignore_sigpipe);
  atomic_store_explicit(&_clog_sigpipe_once_done, true, memory_order_relaxed);
}

/* Runs before every write to a pipe sink. See the section comment above. */
static inline void _clog_pipe_write_prepare(void) {
  if (_clog_writer_sigpipe != CLOG_WRITER_NONE) {
    _clog_writer_sync_sigpipe_mask();
    return;
  }
  if (atomic_load_explicit(&_clog_sigpipe_policy, memory_order_relaxed) ==
          CLOG_SIGPIPE_AUTO &&
      !atomic_load_explicit(&_clog_sigpipe_once_done, memory_order_relaxed))
    _clog_ignore_sigpipe_once();
}

#if defined(F_SETNOSIGPIPE)
/* macOS raises the SIGPIPE of a failed write to a pipe on the whole process,
 * not on the writing thread, so the mask of the async writer thread does not
 * keep it from a thread that has the default disposition. There the writer
 * thread sets F_SETNOSIGPIPE on the descriptor for the length of each write
 * under CLOG_SIGPIPE_AUTO, and such a write fails with EPIPE and raises no
 * signal. The begin gives the value to restore, or -1 when it changed
 * nothing; the end puts the earlier value back, so the descriptor of the
 * application keeps its own setting. */
static int _clog_pipe_nosigpipe_begin(int fd) {
  if (_clog_writer_sigpipe == CLOG_WRITER_NONE ||
      atomic_load_explicit(&_clog_sigpipe_policy, memory_order_relaxed) !=
          CLOG_SIGPIPE_AUTO)
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

/* Runs after a write to a pipe sink fails with EPIPE, a write that raised
 * SIGPIPE for the calling thread. When this module blocked SIGPIPE on this
 * thread, the signal is pending on it, and every pending one is consumed
 * here (Linux keeps one pending SIGPIPE, but FreeBSD queues one for each
 * failed write); otherwise an unblock after a policy change would deliver
 * one and end the process for a record that the library already dropped.
 * The SIGPIPE that the write raised is directed at this thread, and a
 * thread-directed signal is taken before a process-directed one. */
static __attribute__((noinline, cold)) void _clog_consume_own_sigpipe(void) {
  if (_clog_writer_sigpipe != CLOG_WRITER_BLOCKED) return;
  /* The EPIPE of the failed write stays in errno for the caller of the write
   * loop, which decides from it whether the undelivered bytes can ever go
   * out (see _clog_write_error_is_transient()). */
  int saved_errno = errno;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
#if defined(__APPLE__)
  /* No sigtimedwait() here: sigwait() does not block for a signal that
   * sigpending() reports, because the thread that raised it is this one. */
  sigset_t pending;
  sigemptyset(&pending);
  int sig;
  while (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
    (void)sigwait(&set, &sig);
#else
  const struct timespec zero = {0, 0};
  for (;;) {
    int sig = sigtimedwait(&set, NULL, &zero);
    if (sig == SIGPIPE || (sig < 0 && errno == EINTR)) continue;
    break;
  }
#endif
  errno = saved_errno;
}

ccol_retval_t clog_set_sigpipe_policy(clog_sigpipe_policy_t policy) {
  if (policy != CLOG_SIGPIPE_AUTO && policy != CLOG_SIGPIPE_UNTOUCHED)
    return ccol_invalid_args;
  atomic_store_explicit(&_clog_sigpipe_policy, (int)policy,
                        memory_order_relaxed);
  return ccol_success;
}

/*
 * This is a write() loop that tries again after EINTR and after a partial
 * write, as a plain retry loop does, and also waits with poll() for the fd
 * to become writable again after EAGAIN or EWOULDBLOCK, instead of treating
 * a temporary "would block" condition as a permanent failure. This matters
 * because clog_open_fd_mp() puts no restriction on the blocking mode of the
 * fd that the caller gives, and the documented use of CLOG_FMT_SYSLOG in the
 * header is a UNIX datagram socket, which is exactly the kind of descriptor
 * that can legitimately give EAGAIN under load, when its send buffer is full
 * for a moment. Without this wait, a non-blocking fd permanently and
 * silently loses the remainder of a record the instant that the peer cannot
 * keep up, which is exactly when log visibility matters most. The block here
 * happens while the caller holds shared->mutex, and it makes a non-blocking
 * fd behave the same way as a blocking one already behaves when its peer
 * cannot keep up: this is not a new risk, but parity with the behavior that
 * a plain blocking descriptor has.
 *
 * poll_timeout_ms says whether the write may wait without a bound. Every
 * write of the bytes of a record gives CLOG_WRITE_WAIT_FOREVER, because this
 * logger accepted that record and so delivers it however long the
 * descriptor takes to accept it. A recovery step, which runs only so that a
 * DIFFERENT record can go out or so that the process can stop, gives a
 * number of milliseconds instead, and the whole write then ends within that
 * time on every descriptor whose readiness poll(2) reports; see
 * _write_all_bounded(). Such a step drains the continuation of a record that
 * is already half written (see _clog_flush_and_settle()), settles a batch on
 * the fatal path or at exit, or writes the marker that names a loss. An
 * ordinary log call reaches that path, and so does CLOG_FATAL, and neither
 * of them may wait without a bound on bytes that an earlier call left
 * behind.
 *
 * The function gives the number of bytes that it truly wrote, which can be
 * less than len: an error that is truly unrecoverable, a "no progress"
 * condition (w == 0), or a bound that expires can each stop it partway
 * through. A caller that tracks bytes_written for the rotation must use this
 * return value and not len, so that counter never drifts ahead of the real
 * size of the file on disk.
 */
#define CLOG_WRITE_WAIT_FOREVER (-1)
/* This wait is long enough for a descriptor whose peer is only behind for a
 * moment to take the continuation, and short enough that a descriptor which
 * cannot take the continuation holds up no log call, fatal or otherwise. */
#define CLOG_SPLIT_RECORD_DRAIN_WAIT_MS 100

/* Waits, after EAGAIN or EWOULDBLOCK, until fd is writable again, without a
 * bound. It gives true when the write should be tried again. It is out of
 * line because it is the only part of the write loop that needs an object
 * in memory, so the loop itself keeps no stack canary on its path of one
 * write per record. */
static __attribute__((noinline)) bool _clog_wait_writable(int fd) {
  struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
  int pr = poll(&pfd, 1, -1);
  /* An interruption only waits again: an unbounded wait has no time to
   * keep. */
  if (pr < 0 && errno == EINTR) return true;
  /* The kernel reports POLLERR, POLLHUP and POLLNVAL for every call,
   * whatever `events` mask the caller asks for, so a pr > 0 alone does not
   * mean that the fd is writable. The write is tried again only once POLLOUT
   * itself is set; otherwise the loop stops, because the state is truly
   * unrecoverable, instead of calling write() again against an fd that only
   * reported an error or a hangup. */
  return pr > 0 && (pfd.revents & POLLOUT);
}

/* A socket whose receiver has no room for a datagram can refuse the send
 * with ENOBUFS instead of blocking: FreeBSD does that for an AF_UNIX
 * datagram socket, and poll(2) still reports POLLOUT there, because the
 * room that is missing is the receiver's. The only way to wait is to try
 * again later, as FreeBSD's own syslog(3) does. *step_us holds the next
 * pause; it starts at 0, and each call doubles it from 50 microseconds up
 * to 10 milliseconds. limit_ms bounds the pause when it is not negative. */
static __attribute__((noinline, cold)) void _clog_enobufs_backoff(
    unsigned *step_us, int limit_ms) {
  unsigned us = *step_us ? *step_us : 50u;
  *step_us = us >= 5000u ? 10000u : us * 2u;
  if (limit_ms >= 0 && (long long)us > (long long)limit_ms * 1000LL)
    us = (unsigned)limit_ms * 1000u;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)us * 1000L};
  nanosleep(&ts, NULL);
}

/* The write loop of CLOG_WRITE_WAIT_FOREVER. A bounded write never reaches
 * it; see _write_all_bounded(). */
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
      /* An unbounded write waits for the receiver, as a blocking send
       * does where the system blocks it; see _clog_enobufs_backoff(). */
      if (kind == CLOG_SINK_SOCKET && errno == ENOBUFS) {
        _clog_enobufs_backoff(&nobufs_step_us, -1);
        continue;
      }
      if (kind == CLOG_SINK_PIPE && errno == EPIPE) {
        _clog_consume_own_sigpipe();
        break; /* the reader is gone, which is unrecoverable */
      }
      if ((errno == EAGAIN || errno == EWOULDBLOCK) && _clog_wait_writable(fd))
        continue;
      break; /* truly unrecoverable. Stop and report what went out */
    }
    if (w == 0)
      break; /* no progress, from a quota or an fd limit; do not
                spin */
    data += (size_t)w;
    len -= (size_t)w;
    total += (size_t)w;
  }
  return total;
}

/* The path of a regular file, and of every other sink that cannot raise
 * SIGPIPE. */
static __attribute__((noinline)) size_t _write_all_plain(int fd,
                                                         const char *data,
                                                         size_t len) {
  return _write_all_body(fd, CLOG_SINK_PLAIN, data, len);
}

/* Set, and never cleared, once a CLOG_FATAL call starts; see
 * _clog_write_fatal(). _clog_fatal_deadline is written once before it, and
 * a write that CLOG_SINK_FATAL_BOUND sends to the bounded path reads the
 * deadline under the mutex of the target, under which the fatal call set
 * that flag after it fixed the deadline. */
static _Atomic bool _clog_fatal_bound_armed;
static struct timespec _clog_fatal_deadline;

static __attribute__((noinline, cold)) size_t
_write_all_fatal_bound(clog_shared_t *sh, const char *data, size_t len);

/* The path of a pipe sink and of a socket sink, and of a sink that carries
 * CLOG_SINK_FATAL_BOUND. The test of that flag sits behind the test for a
 * pipe, so a pipe pays nothing for it and a socket pays one compare. */
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

/* The milliseconds from now until deadline, rounded up, and 0 once it has
 * passed. The rounding keeps a wait from ending before the deadline. */
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
 * The write of a recovery step: the same contract as _write_all_body(), with
 * the whole write ending within timeout_ms. poll(2) alone does not give that
 * bound, because it bounds only a wait after EAGAIN, and a descriptor in
 * blocking mode never reports EAGAIN: a write(2) larger than the room that a
 * pipe has left blocks until a reader makes room, however long that takes.
 * So no call here can block past the deadline:
 *
 *   - A socket is written with ccol_send_nb(), which never blocks whatever
 *     the mode of the descriptor (MSG_DONTWAIT, or on macOS a send limited
 *     to the room that poll(2) reports), and a poll with the time that is
 *     left waits for room.
 *   - Every other descriptor that poll(2) reports on, a pipe or a FIFO
 *     first of all, is polled for POLLOUT with the time that is left before
 *     each write, and each write gives at most PIPE_BUF bytes. A pipe that
 *     reports POLLOUT has room for PIPE_BUF bytes, so such a write returns
 *     at once; only another process that writes into the same pipe between
 *     the poll and the write can take that room first.
 *   - A terminal is written through a second open file description of the
 *     same device, opened with O_NONBLOCK (see _clog_open_tty_nonblocking()).
 *     POLLOUT on a terminal means only that some room is free, and a
 *     blocking write(2) to it waits until every byte fits, however small
 *     the write: a pseudo-terminal whose reader stopped, such as a frozen
 *     remote session, keeps it for ever. A non-blocking write takes what
 *     fits and returns, and a poll with the time that is left waits for
 *     more. The flag lives on the new description alone, so no other
 *     holder of the terminal (a shell above all) sees its descriptor turn
 *     non-blocking. Where that open fails, each write gives at most
 *     CLOG_TTY_FALLBACK_CHUNK bytes after POLLOUT, which a pseudo-terminal
 *     takes at once, since it counts its room in blocks of 256 bytes.
 *   - A regular file or a block device is exempt: poll(2) reports it
 *     writable at all times, and a write to a disk waits on the disk and
 *     never on a reader, so the ordinary loop writes it.
 *
 * A deadline that passes before everything went out ends the write with
 * errno set to EAGAIN, which _clog_write_error_is_transient() treats as a
 * condition that a later attempt can outlast. The function is out of line
 * and cold, because only recovery steps call it.
 */
#define CLOG_TTY_FALLBACK_CHUNK 128

/* Opens the terminal fd a second time for writing, with O_NONBLOCK, and
 * gives the new descriptor, or -1. It goes through /proc/self/fd where that
 * exists, and otherwise through the name that ttyname_r(3) gives, which is
 * how FreeBSD and macOS reach it. A pseudo-terminal master is left alone,
 * because an open of its path makes a new pseudo-terminal, or reaches the
 * slave, rather than a second description of the same device; the device
 * number of what opened must match the one of fd, which rules that out. */
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
  /* The descriptor that the loop writes, and the most that one write after
   * POLLOUT may give it. own_fd is a second description of a terminal that
   * this call opened and closes. */
  int wfd = fd;
  int own_fd = -1;
  size_t chunk = PIPE_BUF;
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
      struct pollfd pfd = {.fd = wfd, .events = POLLOUT, .revents = 0};
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
       * with the error of the descriptor, which the loop reports. */
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
        /* A descriptor in non-blocking mode that took nothing although it
         * reported room. The poll at the top of the loop waits again, and
         * the deadline bounds the loop. */
        if (_clog_ms_until(&deadline) > 0) continue;
        errno = EAGAIN;
      }
      break;
    }
    if (w == 0) break; /* no progress. Do not spin */
    data += (size_t)w;
    len -= (size_t)w;
    total += (size_t)w;
  }
#if defined(F_SETNOSIGPIPE)
  _clog_pipe_nosigpipe_end(wfd, nosig);
#endif
  if (own_fd >= 0) {
    int saved = errno;
    close(own_fd);
    errno = saved;
  }
  return total;
}

/* The write to a sink that carries CLOG_SINK_FATAL_BOUND. */
static __attribute__((noinline, cold)) size_t
_write_all_fatal_bound(clog_shared_t *sh, const char *data, size_t len) {
  return _write_all_bounded(sh, data, len,
                            _clog_ms_until(&_clog_fatal_deadline));
}

/* Writes to sh->fd; see _write_all_body() above for the contract. The kind
 * of the sink decides the write call and the SIGPIPE handling (see
 * "SIGPIPE" above). The choice sits in the caller so that each sink calls its
 * own loop directly, and a regular file pays one test of a byte that shares
 * the cache line of sh->fd. A bounded write goes to _write_all_bounded();
 * every caller on the path of a record passes CLOG_WRITE_WAIT_FOREVER as a
 * constant, so that test folds away there. The caller must hold
 * sh->mutex. */
static inline __attribute__((always_inline)) size_t _write_all(
    clog_shared_t *sh, const char *data, size_t len, int poll_timeout_ms) {
  if (poll_timeout_ms != CLOG_WRITE_WAIT_FOREVER)
    return _write_all_bounded(sh, data, len, poll_timeout_ms);
  if (__builtin_expect(sh->sink_kind != CLOG_SINK_PLAIN, 0))
    return _write_all_nonplain(sh, data, len);
  return _write_all_plain(sh->fd, data, len);
}

/*
 * This writes one record that the library itself makes, at `level`, whose
 * message is `head` followed by `detail`; the markers of a loss, "log record
 * truncated: <detail>", are such records. It uses the output format that is
 * in force at that moment, and puts the lead-in `nl` of the caller in front
 * of it. That lead-in is "\n" when the bytes already on disk stop in the
 * middle of a record and need the missing newline of that record, and ""
 * otherwise. Both go out in one write(), so nothing can land between them.
 *
 * The marker is what keeps every loss that this module reports consistent
 * with the rest of the module: the library reports a record that this logger
 * cannot deliver and never drops such a record in silence (see
 * _clog_write_unrepresentable_record()). The write of the marker itself is
 * best-effort, and the library never repairs it, because whatever stopped
 * the record from going out very probably stops the marker too, and a marker
 * that describes a marker says nothing about the log content that was truly
 * lost.
 *
 * `head` and `detail` are always text from this file, never content that a
 * caller gives, so they need no escape in any of the three formats.
 *
 * poll_timeout_ms is the bound of the step that reports the loss (see
 * _write_all()): a marker that a recovery step writes waits no longer than
 * the step itself may.
 *
 * The caller must hold sh->mutex.
 */
static void _clog_note_record(clog_shared_t *sh, clog_format_t fmt,
                              clog_level_t level, const char *nl,
                              const char *head, const char *detail,
                              int poll_timeout_ms) {
  char note[256];
  int n;
  if (fmt == CLOG_FMT_JSON) {
    n = snprintf(note, sizeof(note), "%s{\"level\":\"%s\",\"msg\":\"%s%s\"}\n",
                 nl, _LEVEL_STR[level], head, detail);
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    n = snprintf(note, sizeof(note), "%s<%d>1 - - - - - - %s%s\n", nl, pri,
                 head, detail);
  } else {
    n = snprintf(note, sizeof(note), "%slevel=%s msg=\"%s%s\"\n", nl,
                 _LEVEL_STR[level], head, detail);
  }
  if (n > 0) {
    size_t nlen = (size_t)n < sizeof(note) ? (size_t)n : sizeof(note) - 1;
    size_t w = _write_all(sh, note, nlen, poll_timeout_ms);
    if (sh->rotation_enabled) sh->bytes_written += (off_t)w;
  }
}

/* The marker "log record truncated: <detail>" at level ERROR; see
 * _clog_note_record(). */
static void _clog_note_record_loss(clog_shared_t *sh, clog_format_t fmt,
                                   const char *nl, const char *detail,
                                   int poll_timeout_ms) {
  _clog_note_record(sh, fmt, CLOG_ERROR, nl, "log record truncated: ", detail,
                    poll_timeout_ms);
}

/*
 * This restores the record framing and reports the loss after a write that
 * the kernel accepted only in part, as happens with a filesystem that is
 * full with ENOSPC, with an RLIMIT_FSIZE ceiling, or with a peer that goes
 * away in the middle of a record. The accepted bytes are on disk without the
 * terminating newline of the record, and without this function the next
 * record goes straight onto them, so that every consumer of the file reads
 * the two as one line. The function writes that newline only when the
 * accepted prefix does not already end on a record boundary.
 *
 * The caller must hold sh->mutex.
 */
static void _clog_note_truncated_record(clog_shared_t *sh, clog_format_t fmt,
                                        const char *data, size_t written,
                                        size_t len) {
  char detail[96];
  snprintf(detail, sizeof(detail), "%zu of %zu bytes written", written, len);
  _clog_note_record_loss(sh, fmt,
                         (written > 0 && data[written - 1] != '\n') ? "\n" : "",
                         detail, CLOG_WRITE_WAIT_FOREVER);
}

/*
 * This reports that the library gives up on the continuation of a record
 * that is already half written. When the half on disk left a line open
 * (CLOG_SPLIT_MID_LINE), it also closes that line, so that the record that
 * goes out next lands on a line of its own and not inside another record.
 * The library writes this marker only when async_partial_record is set, so
 * `split` is never CLOG_SPLIT_NONE here.
 *
 * The caller must hold sh->mutex.
 */
static void _clog_note_dropped_continuation(clog_shared_t *sh,
                                            clog_format_t fmt,
                                            unsigned char split, size_t dropped,
                                            int poll_timeout_ms) {
  char detail[96];
  snprintf(detail, sizeof(detail), "%zu undelivered bytes dropped", dropped);
  _clog_note_record_loss(sh, fmt, split == CLOG_SPLIT_MID_LINE ? "\n" : "",
                         detail, poll_timeout_ms);
}

/*
 * Reports that the library gives up on `len` undelivered bytes of the async
 * batch, which start at `data`. `split` says where the bytes already on disk
 * end, as a value of async_partial_record. The marker names the number of
 * records that the loss touches and `why`, which is text from this file.
 * A record counts once, whether all of it or only its continuation is lost:
 * every line that does not start with a tab starts a record, and a
 * continuation that starts with a tab belongs to the record that the bytes
 * on disk left unfinished.
 *
 * The caller must hold sh->mutex, and it discards the bytes itself.
 */
static __attribute__((noinline, cold)) void _clog_note_dropped_batch(
    clog_shared_t *sh, const char *data, size_t len, unsigned char split,
    const char *why, int poll_timeout_ms) {
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
  _clog_note_record_loss(sh, sh->format,
                         split == CLOG_SPLIT_MID_LINE ? "\n" : "", detail,
                         poll_timeout_ms);
}

/*
 * This writes one record that already has its framing to sh->fd: every byte of
 * that record, including the terminating newline. It charges what the kernel
 * truly accepted to sh->bytes_written, so size-based rotation never runs ahead
 * of the real size of the file on disk, and it repairs the framing when the
 * write stops short. Every synchronous write of a whole record in this file
 * goes through this function, so that no single path can be the one that leaves
 * half a record behind. The caller must hold sh->mutex.
 *
 * The code deliberately keeps this function out of line. There is one call
 * for each record, on a path where the write(2) beneath it dominates the
 * cost, and the functions that call it build records and are large: if the
 * compiler folded this function into them, their code shape and their
 * register allocation would move with every edit made here.
 */
static __attribute__((noinline)) void _clog_write_record(clog_shared_t *sh,
                                                         clog_format_t fmt,
                                                         const char *data,
                                                         size_t len) {
  size_t written = _write_all(sh, data, len, CLOG_WRITE_WAIT_FOREVER);
  if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  if (written < len) _clog_note_truncated_record(sh, fmt, data, written, len);
}

/* ========================================================================== */
/*                         TIMESTAMP                                          */
/* ========================================================================== */

/* This renders a timestamp that the library has already captured. It is a
 * separate function so that the library can build a record with the
 * timestamp of the SUBMISSION time instead of the write time, because the
 * build can happen much later, for example on the async writer thread. */
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

/* The flags of the directory descriptor that a rotating logger keeps. The
 * descriptor only names the directory for the *at() calls, so O_PATH is
 * enough where the platform has it, and it needs no read permission on the
 * directory. A listing opens "." relative to it; see _clog_opendir_at(). */
#if defined(O_PATH)
#define CLOG_DIR_OPEN_FLAGS (O_PATH | O_DIRECTORY | O_CLOEXEC)
#else
#define CLOG_DIR_OPEN_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif

/* Opens the directory part of path: "." for a path with no '/', and "/" for
 * a path directly under the root. It gives the descriptor, or -1 with errno
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

/* Opens a listing of the directory that dir_fd names. The listing gets a
 * descriptor of its own, because closedir() closes the descriptor that it
 * reads, while dir_fd must stay open. It gives NULL on a failure. */
static DIR *_clog_opendir_at(int dir_fd) {
  int fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return NULL;
  DIR *d = fdopendir(fd);
  if (!d) close(fd);
  return d;
}

/* Give a pointer into path, just after the last '/'. */
static const char *_path_base(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* True when the directory that dir_fd names holds an entry called name, of
 * any type. It never follows a symbolic link, so a dangling link counts as
 * an entry. A name that cannot be examined, one too long for the directory
 * for example, gives false; the step that then uses the name fails on the
 * same condition and never replaces an entry. */
static bool _clog_entry_exists(int dir_fd, const char *name) {
  struct stat st;
  return fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
}

/* Removes the entry name, relative to dir_fd, only while it still names the
 * file that *st describes (the same device and inode, examined without
 * following a link). A rotation and a compression remove only what they
 * made or what they already checked, and never an entry that another
 * program put at that name in the meantime. */
static void _clog_unlink_if_same(int dir_fd, const char *name,
                                 const struct stat *st) {
  struct stat cur;
  if (fstatat(dir_fd, name, &cur, AT_SYMLINK_NOFOLLOW) == 0 &&
      cur.st_dev == st->st_dev && cur.st_ino == st->st_ino)
    (void)unlinkat(dir_fd, name, 0);
}

/*
 * Which directory entries are generations of a rotating target. A name that
 * parses as a rotated name of the target (see _rotated_name_parse()) is one
 * of its generations only when the entry, examined without following a
 * link, is a regular file, its owner is the effective user of the process or
 * the owner of the live file (a rotation gives every generation the owner of
 * the live file; see _clog_copy_owner_and_mode()), and its stamp is not
 * above `limit`. Naming, retention, compression and the recovery of
 * compressions act only on generations; every other entry is never opened,
 * compressed, renamed or deleted, and gets a WARN record (see
 * _clog_note_ignored_entry()). So another user who can write the directory
 * cannot make the logger write, read, archive or delete a file through a
 * link, block it on a FIFO, stop its naming with a stamp that nothing can
 * sort above, or take its retention slots.
 */
typedef struct {
  uid_t euid;
  uid_t live_uid;
  char limit[CLOG_ROTATION_STAMP_DIGITS];
} clog_gen_filter_t;

static bool _clog_gen_uid_ok(uid_t uid, uid_t euid, uid_t live_uid) {
  return uid == euid || uid == live_uid;
}

/* Parses the 14 stamp digits d, YYYYMMDDHHMMSS in UTC. It gives false when
 * the digits name no representable time. */
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

/* Writes the 14 stamp digits of the time t, in UTC, to out. A time that
 * time_t cannot hold, or that lies past the year 9999, gives the highest
 * stamp, above which no stamp sorts. */
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
 * These are synchronization hooks for a test only, which every build that is
 * not a test build removes. They let a test widen and observe one window
 * deterministically: the window from the moment _gzip_compress_file()
 * creates its destination file on disk (when gzopen() returns) to the moment
 * that function finishes the write of the file. Inside that exact window,
 * the _prune_rotated() pass of a concurrent rotation must never be able to
 * delete that destination. The window is normally shorter than a
 * millisecond, so with these hooks a test does not depend on real scheduling
 * luck to land a racing deletion inside it.
 */
static _Atomic unsigned int _clog_test_pending_compress_delay_us = 0;
static _Atomic bool _clog_test_gz_dest_opened = false;

void clog_test_set_pending_compress_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_gz_dest_opened, false);
  atomic_store(&_clog_test_pending_compress_delay_us, delay_us);
}

bool clog_test_gz_dest_opened(void) {
  bool opened = atomic_load(&_clog_test_gz_dest_opened);
  return opened;
}

/*
 * A gate for a test. While it is held, every compression pauses right after
 * it creates its destination, until the test releases the gate. The pause
 * has a safety bound of CLOG_TEST_COMPRESS_HOLD_MAX_US, so that a thread
 * that must not be the one to compress, but is, fails its test instead of
 * hanging it. _clog_test_compressions_finished counts every compression that
 * ran to its end, whatever its outcome.
 */
#define CLOG_TEST_COMPRESS_HOLD_MAX_US 30000000U
static _Atomic bool _clog_test_compress_hold = false;
static _Atomic size_t _clog_test_compressions_finished = 0;

void clog_test_hold_compressions(bool hold) {
  if (hold) atomic_store(&_clog_test_gz_dest_opened, false);
  atomic_store(&_clog_test_compress_hold, hold);
}

size_t clog_test_compressions_finished(void) {
  return atomic_load(&_clog_test_compressions_finished);
}

/* A gate for a test, like the one above, that pauses the recovery of
 * unfinished compressions before it lists the directory. */
#define CLOG_TEST_RECOVER_HOLD_MAX_US 3000000U
static _Atomic bool _clog_test_recover_hold = false;
static _Atomic bool _clog_test_recover_entered = false;

void clog_test_hold_compression_recovery(bool hold) {
  if (hold) atomic_store(&_clog_test_recover_entered, false);
  atomic_store(&_clog_test_recover_hold, hold);
}

bool clog_test_compression_recovery_entered(void) {
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

/* Counts the timeouts of the timed wait of the async writer thread; each one
 * is a wakeup with no job, which flushes the batch buffer. */
static _Atomic size_t _clog_test_writer_timeout_wakeups = 0;

size_t clog_test_writer_timeout_wakeups(void) {
  return atomic_load(&_clog_test_writer_timeout_wakeups);
}

/*
 * This counts the real calls of _rotate(), that is, every attempt that
 * reaches rename() and open(), not every write that would like to rotate. A
 * test can therefore verify the retry policy for a rotation that fails again
 * and again: the library must retry such a rotation with a bounded backoff
 * instead of trying again on every single write.
 */
static _Atomic size_t _clog_test_rotate_attempt_count = 0;

void clog_test_reset_rotate_attempt_count(void) {
  atomic_store(&_clog_test_rotate_attempt_count, 0);
}

size_t clog_test_get_rotate_attempt_count(void) {
  return atomic_load(&_clog_test_rotate_attempt_count);
}

void clog_test_sanitize_syslog_appname(const char *raw, char *out,
                                       size_t outsz) {
  _sanitize_syslog_printusascii_field(raw, out, outsz);
}

void clog_test_append_ctrl_escaped(const char *raw, char *out, size_t outsz) {
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
 * This lets a test force _capture_backtrace() to report a capture failure
 * deterministically, with the same result as a platform with no backtrace
 * support, or as a backtrace_symbols() call whose own allocation fails. The
 * test then does not depend on either of those real conditions, one of which
 * belongs to the platform and the other to a real out-of-memory state.
 */
static _Atomic bool _clog_test_force_bt_capture_failure = false;

void clog_test_force_backtrace_capture_failure(bool force) {
  atomic_store(&_clog_test_force_bt_capture_failure, force);
}

/*
 * This lets a test force EVERY frame of a real backtrace (one whose syms is
 * not NULL) to fail to append inside _emit_backtrace_syslog_lines(), without
 * depending on a real allocation failure that lasts across every small
 * append that the function makes for each frame. The buffer-size constants
 * of this library make that condition impractical to reproduce for real,
 * because each frame gets a scratch buffer that the code has just reset and
 * that is already large enough.
 * Unlike clog_test_force_next_buf_ensure_failure(), which fires once and
 * then disarms itself, this hook stays armed across every frame in one call
 * until a caller disarms it, which is the same shape as
 * clog_test_force_backtrace_capture_failure().
 */
static _Atomic bool _clog_test_force_all_syslog_bt_frames_fail = false;

void clog_test_force_all_syslog_backtrace_frames_failure(bool force) {
  atomic_store(&_clog_test_force_all_syslog_bt_frames_fail, force);
}

/*
 * This lets a test force _capture_backtrace() to report,
 * deterministically, a capture that truly succeeds: syms is not NULL and
 * comes from a real backtrace(), and the reported depth is clamped to
 * CLOG_BT_INITIAL_FRAME. This exercises the path where the capture
 * succeeded but found no frame beyond the two internal bookkeeping ones,
 * which the emitters must treat as a real, accurate, empty backtrace and
 * not as a capture failure. With this hook, the test does not depend on a
 * real call stack that is shallow enough to reach the path.
 */
static _Atomic bool _clog_test_force_shallow_bt_depth = false;

void clog_test_force_shallow_backtrace_depth(bool force) {
  atomic_store(&_clog_test_force_shallow_bt_depth, force);
}
#endif

/*
 * Gives the file behind fd, which this module has just created with mode
 * 0600, the permission bits, the owner and the group that *like describes.
 * The owner and the group come first, and the bits are widened only after
 * them, so the file is never readable by more than *like allows. The owner
 * and the group are a best effort, as logrotate makes them: a process that
 * may not give the file the owner keeps its own and still tries the group,
 * and a process that may not give it the group either keeps its own group
 * and clears the group bits, so that the group of the process does not
 * gain a read that the group of *like had. A failed fchmod() leaves the
 * 0600 of the creation. A new live file of a rotation and a compressed
 * generation both go through here, so every generation of a log carries
 * the same access.
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
    /* The file keeps the 0600 that it was created with. */
  }
}

#ifdef RUNNING_UNIT_TESTS
/* The errno that the next fsync() of a compressed output reports instead of
 * the real call, for a test; 0 lets the real call run. It disarms itself
 * when it fires. */
static _Atomic int _clog_test_next_fsync_error = 0;

void clog_test_force_next_fsync_error(int err) {
  atomic_store(&_clog_test_next_fsync_error, err);
}
#endif

/* Brings the data of the compressed output fd to the disk. It gives 0 when
 * the data is there, or when the filesystem offers no way to ask for it:
 * fsync(2) fails with EINVAL or ENOSYS on a filesystem that does not
 * implement it, and a compression that failed on that answer would leave
 * every rotated file uncompressed for ever on such a filesystem. Every
 * other failure, EIO above all, gives -1. */
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

/* The suffix of the name under which a compression writes its output until
 * the output is complete. "<rotated>.gz.tmp" never parses as a rotated name
 * (see _rotated_name_parse()), so a deletion pass and the choice of a new
 * rotated name never see it. */
#define CLOG_GZ_TMP_SUFFIX ".tmp"

/* Publishes the finished temporary output tmp under the name dst, both
 * relative to dir_fd. It never replaces a file that already sits at dst. It
 * gives 0 on success. */
static int _clog_publish_no_replace(int dir_fd, const char *tmp,
                                    const char *dst) {
#if defined(RENAME_NOREPLACE) && defined(__GLIBC__)
  if (renameat2(dir_fd, tmp, dir_fd, dst, RENAME_NOREPLACE) == 0) return 0;
  if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
  /* A hard link fails with EEXIST when dst exists, in one step, like the
   * rename above. It is the fallback for a filesystem that does not know
   * RENAME_NOREPLACE. */
  if (linkat(dir_fd, tmp, dir_fd, dst, 0) == 0) {
    unlinkat(dir_fd, tmp, 0);
    return 0;
  }
  if (errno == EEXIST) return -1;
  /* A filesystem without hard links. The check and the rename are two
   * steps here, and only another program that creates dst in between can
   * lose its file. */
  if (_clog_entry_exists(dir_fd, dst)) {
    errno = EEXIST;
    return -1;
  }
  return renameat(dir_fd, tmp, dir_fd, dst);
}

/*
 * This gzip-compresses the file src into dst, which is src plus ".gz", with
 * zlib; all names are relative to dir_fd. The output goes to
 * dst + CLOG_GZ_TMP_SUFFIX first, and only a complete, flushed gzip stream
 * is renamed to dst, so a process that stops in the middle of a compression
 * leaves at most that temporary file, never a truncated dst; the next open
 * of the log removes it and compresses the source again (see
 * _clog_recover_compressions()). The output gets the permission bits, the
 * owner and the group of src, as gzip(1) gives them (see
 * _clog_copy_owner_and_mode()).
 *
 * On success it gives 0, dst is a valid gzip file, and the function unlinks
 * src. On a failure it gives -1, removes its temporary output, and leaves
 * src untouched. It never replaces a file that already sits at dst. A
 * non-NULL abort_flag that becomes true stops the compression at its next
 * block, which counts as a failure.
 *
 * It reads src only when the descriptor that it opens is a regular file
 * owned by the effective user or by live_uid (see clog_gen_filter_t). The
 * open never follows a link and never waits for a FIFO or a device, and the
 * type and the owner are checked on the descriptor itself, so an entry that
 * another program put at the name after it was listed is never read.
 */
static int _gzip_compress_file(int dir_fd, const char *src, const char *dst,
                               _Atomic bool *abort_flag, uid_t live_uid) {
  FILE *in = NULL;
  gzFile out = NULL;
  int out_fd = -1;
  int rc = -1;
  bool tmp_created = false; /* The code below unlinks tmp only if this call
      truly created it */
  struct stat st;           /* the source, as opened */
  struct stat tmp_st;       /* the temporary output, once tmp_created */

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

  /* Never overwrite something that already sits at dst: an unrelated file,
   * or a stale leftover, can occupy this exact gzip destination path. The
   * publish step refuses it too; this check only saves the work. */
  if (_clog_entry_exists(dir_fd, dst)) goto done;

  /* The temporary name belongs to this compression alone. A regular file
   * there that the effective user or live_uid owns is the leftover of a
   * compression that stopped part way, and it holds nothing that is not also
   * in src. Anything else at that name is left alone, and the source stays
   * uncompressed. O_EXCL makes the check and the creation one step, and it
   * never follows a link. */
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
    /* The file exists but cannot be examined, so it cannot be told apart from
     * another entry at the same name later, and it is removed at once. */
    (void)unlinkat(dir_fd, tmp, 0);
    goto done;
  }
  tmp_created = true;
  /* The access of the source, which the umask does not narrow a second
   * time. */
  _clog_copy_owner_and_mode(out_fd, &st);
  out = gzdopen(out_fd, "wb");
  if (!out) goto done;

#ifdef RUNNING_UNIT_TESTS
  {
    /* Copy the armed delay into a local before the code announces that the
     * output file exists. A test can spin-wait on clog_test_gz_dest_opened()
     * and, the instant that it sees this compression, arm the delay again or
     * clear it for a DIFFERENT and later compression, which must never race
     * the read that this call already has in progress. */
    unsigned int delay = atomic_load(&_clog_test_pending_compress_delay_us);
    atomic_store(&_clog_test_gz_dest_opened, true);
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

  /* The trailer goes out, and the data reaches the disk, before the output
   * takes its final name, so a crash after the rename never finds a short
   * dst beside a deleted src. */
  if (gzflush(out, Z_FINISH) != Z_OK || _clog_sync_output(out_fd) != 0)
    goto done;
  int close_rc = gzclose(out);
  out = NULL;
  out_fd = -1;
  if (close_rc != Z_OK) goto done;
  if (_clog_publish_no_replace(dir_fd, tmp, dst) != 0) goto done;
  tmp_created = false;
  rc = 0;

done:
  if (in) fclose(in);
  if (out)
    gzclose(out);
  else if (out_fd >= 0)
    close(out_fd);
  /* The unfinished output, and on success the uncompressed original, each
   * only while the name still holds the file that this call made or read. */
  if (tmp_created) _clog_unlink_if_same(dir_fd, tmp, &tmp_st);
  if (rc == 0) _clog_unlink_if_same(dir_fd, src, &st);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&_clog_test_compressions_finished, 1);
#endif
  return rc;
}

#ifdef RUNNING_UNIT_TESTS
bool clog_test_gzip_compress_file(const char *src, const char *dst) {
  return _gzip_compress_file(AT_FDCWD, src, dst, NULL, geteuid()) == 0;
}

bool clog_test_gzip_compress_file_for_owner(const char *src, const char *dst,
                                            uid_t live_uid) {
  return _gzip_compress_file(AT_FDCWD, src, dst, NULL, live_uid) == 0;
}
#endif

/* ========================================================================== */
/*                         LOG ROTATION                                       */
/* ========================================================================== */

/*
 * This parses the part of a candidate rotated-file name that follows
 * "<base>.". A name that this logger made has three parts: the mandatory
 * 14-digit YYYYMMDDHHMMSS stamp that CLOG_ROTATION_FMT produces; an optional
 * collision suffix "_<seq>", whose number is zero-padded to 4 digits (a
 * number above 9999 takes as many digits as it needs, up to
 * CLOG_ROTATION_SEQ_MAX_DIGITS, with no leading zero); and an optional
 * ".gz". A name with no suffix has sequence 0, and _rotate() never writes a
 * suffix of 0.
 *
 * On a name of exactly that shape, this gives true, points *stamp at the 14
 * stamp digits and stores the sequence number in *seq. Anything else, such
 * as extra digits or an unrelated word at the end, only happens to start
 * with something that looks like <base>.<14 digits>, and this gives false,
 * because the library must never treat such a name as a file that it owns.
 * This check stops _prune_rotated() from calling unlink() on an unrelated
 * file that a user, or another tool, put in the same directory.
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

/* This matches one directory entry against "<base>.<rotated name>" and
 * parses it with _rotated_name_parse(). */
static bool _rotated_entry_match(const char *d_name, const char *base,
                                 size_t base_len, const char **stamp,
                                 unsigned long *seq) {
  if (strncmp(d_name, base, base_len) != 0 || d_name[base_len] != '.')
    return false;
  return _rotated_name_parse(d_name + base_len + 1, stamp, seq);
}

/* This orders two rotated names by the pair (stamp, sequence number). */
static int _rotated_key_cmp(const char *stamp_a, unsigned long seq_a,
                            const char *stamp_b, unsigned long seq_b) {
  int c = memcmp(stamp_a, stamp_b, CLOG_ROTATION_STAMP_DIGITS);
  if (c != 0) return c;
  return (seq_a > seq_b) - (seq_a < seq_b);
}

/*
 * Fills *f for one pass over the directory of sh. The caller holds
 * sh->mutex, or owns sh alone. The limit of a stamp is
 * CLOG_ROTATION_FUTURE_TOLERANCE_SECS above the later of the clock and the
 * newest name that sh made (rot_last_stamp). Since every name that sh makes
 * is at or below rot_last_stamp, a clock that steps back by any amount never
 * turns a generation of sh into an entry to ignore, and names keep sorting
 * in the order of the rotations.
 */
static void _clog_gen_filter_init(const clog_shared_t *sh,
                                  clog_gen_filter_t *f) {
  f->euid = geteuid();
  struct stat st;
  f->live_uid = (sh->fd >= 0 && fstat(sh->fd, &st) == 0) ? st.st_uid : f->euid;
  int64_t ref = (int64_t)time(NULL);
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

/* Classifies the entry name, relative to dir_fd, whose stamp digits are at
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
 * Writes the WARN record that names a directory entry which looks like a
 * rotated file of sh but is not one of its generations. The caller holds
 * sh->mutex, or owns sh alone, and no record of sh is half written. At most
 * CLOG_IGNORED_NOTE_BURST such records go out in any window of
 * CLOG_IGNORED_NOTE_WINDOW_SECS, so a directory full of such entries cannot
 * flood the log. The name is a directory entry that anybody who can write
 * the directory chooses, so every byte outside printable ASCII, and every
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
  _clog_note_record(sh, sh->format, CLOG_WARN, "",
                    "log rotation: ignored an entry that is not a rotated "
                    "file of this log: ",
                    safe, CLOG_WRITE_WAIT_FOREVER);
}

/* Classifies one rotated-looking entry, as _clog_entry_classify() does, and
 * notes an ignored one when may_note is true. */
static clog_entry_kind_t _clog_entry_check(clog_shared_t *sh, const char *name,
                                           const char *stamp,
                                           const clog_gen_filter_t *f,
                                           bool may_note) {
  clog_entry_kind_t k = _clog_entry_classify(sh->dir_fd, name, stamp, f);
  if (k == CLOG_ENTRY_IGNORED && may_note) _clog_note_ignored_entry(sh, name);
  return k;
}

/*
 * This finds the newest generation of sh (see clog_gen_filter_t) that is
 * already on disk, and gives true and fills stamp_out and *seq_out when
 * there is one. The caller holds sh->mutex, or owns sh alone, and
 * sh->rot_name_known is false.
 * _rotate() calls it once, before the first rotation of a shared target, so
 * that a logger which starts in a directory that already holds rotated files
 * numbers its own rotations above them; a process that restarts inside the
 * same second is the common case. An entry that is not a generation plays
 * no part in the choice, and it gets a WARN record when may_note is true.
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

/* One rotated file that _prune_rotated() found on disk. stamp points into
 * path. */
typedef struct {
  char *path;
  const char *stamp;
  unsigned long seq;
  bool is_protected;
} clog_rotated_file_t;

/*
 * This orders the rotated files with the oldest first, by the
 * (stamp, sequence number) pair of the name that _rotate() gave to each one.
 * _rotate() gives every rotation of a shared target a pair above every pair
 * that it gave before and every pair that was on disk before its first
 * rotation (see there), so the order of the pairs is the order in which the
 * rotations happened. The sequence number compares as a number, so "_10000"
 * is newer than "_9999".
 *
 * The modification time does not order these files. The compression step
 * writes a rotated file that it compresses, so the mtime of such a file is
 * the time at which that compression FINISHED, not the age of the
 * generation inside the file. A compression runs while the shared target
 * has its mutex unlocked, so a later generation whose compression finishes
 * quickly carries an older mtime than an earlier generation whose
 * compression is still working through a large file, and an order by mtime
 * then deletes the newest generation and keeps the stalest one.
 */
static int _cmp_rotated_oldest_first(const void *a, const void *b) {
  const clog_rotated_file_t *x = (const clog_rotated_file_t *)a;
  const clog_rotated_file_t *y = (const clog_rotated_file_t *)b;
  int c = _rotated_key_cmp(x->stamp, x->seq, y->stamp, y->seq);
  if (c != 0) return c;
  /* The uncompressed file and the ".gz" file of one generation are next to
   * each other either way; this only makes the order total. */
  return strcmp(x->path, y->path);
}

#ifdef RUNNING_UNIT_TESTS
/* Since the last reset: the largest number of rotated generations that a
 * deletion pass left on disk, the largest number of compressions in flight
 * that a pass saw, and the largest number of generations that a pass left
 * beyond the generations that it kept only because they were older than the
 * newest max_keep and still under compression. The on-disk count comes from
 * a directory listing of its own. */
static _Atomic int _clog_test_max_rotated_generations = 0;
static _Atomic int _clog_test_max_compressions_in_flight = 0;
static _Atomic int _clog_test_max_generations_beyond_old_compressions = 0;

/* Counts the distinct rotated generations of base on disk, from its own
 * directory listing, so that the count does not depend on what the pass
 * itself listed. */
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

void clog_test_reset_max_rotated_generations(void) {
  atomic_store(&_clog_test_max_rotated_generations, 0);
  atomic_store(&_clog_test_max_compressions_in_flight, 0);
  atomic_store(&_clog_test_max_generations_beyond_old_compressions, 0);
}

int clog_test_get_max_rotated_generations(void) {
  return atomic_load(&_clog_test_max_rotated_generations);
}

int clog_test_get_max_compressions_in_flight(void) {
  return atomic_load(&_clog_test_max_compressions_in_flight);
}

int clog_test_get_max_generations_beyond_old_compressions(void) {
  return atomic_load(&_clog_test_max_generations_beyond_old_compressions);
}
#endif

/*
 * This deletes the oldest rotated generations of sh beyond
 * sh->rotation.max_rotated_files. The caller holds sh->mutex. A generation
 * is one rotation, which on disk is one file, or two while its compression
 * runs (the uncompressed source and the ".gz" destination), and the pass
 * counts and deletes whole generations. Every name here is relative to
 * sh->dir_fd.
 *
 * The pass never deletes a file of sh->compress_running, the generation that
 * the compressor thread compresses at this moment, because the compression
 * may not have read all of the source or written all of the destination
 * yet, and deleting either one loses all of the log data of that
 * generation. A generation whose job still waits in the queue gets no such
 * protection; see clog_compress_job_t.
 *
 * `just_rotated` is the destination that the calling _rotate() itself just
 * created, or NULL when there is none. The pass never deletes it, or its own
 * ".gz" form, because that file holds the log content that this very
 * rotation moved aside, and nothing has read it yet.
 *
 * Every generation counts against max_keep, protected or not. The pass keeps
 * the newest max_keep generations and deletes every older generation that
 * is not protected, while a protected generation that is older than the
 * newest max_keep stays until a later pass. `just_rotated` is always the
 * newest generation, because _rotate() names it above every other one, so
 * it is always among the newest max_keep, and only the running compression
 * can stay older than the newest max_keep. A pass therefore leaves exactly
 * the newest max_keep generations, or all of them when there are fewer, plus
 * at most that one. A rotation never waits for a compression to finish:
 * instead, a pass that keeps the running compression in this way marks the
 * target, and the compressor thread runs the pass again as soon as that
 * compression ends, so the old generation does not outlive it.
 *
 * A pass that cannot list every rotated file, because an allocation fails,
 * deletes nothing, since a partial list can miss the newest generations,
 * and the pass would then delete generations that the full list keeps.
 *
 * Only generations count and only generations are deleted (see
 * clog_gen_filter_t). An entry that is not one gets a WARN record when
 * may_note is true; the pass that the compressor thread runs passes false,
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

    /* Count the generations. The files of one generation share one
     * (stamp, sequence number) pair, and the sort put them side by side. */
    int generations = 0;
    for (int i = 0; i < fc; i++)
      if (i == 0 || _rotated_key_cmp(files[i].stamp, files[i].seq,
                                     files[i - 1].stamp, files[i - 1].seq) != 0)
        generations++;

    /* Walk the generations with the oldest first. Delete every generation
     * older than the newest max_keep, unless one of its files is protected. */
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
    /* Every compression handed to the compressor thread and not finished:
     * the running one and the queued ones. */
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
 * This gives true when the library cannot use `candidate` as a fresh name
 * for a rotation destination, which happens in two cases: when the name
 * itself already exists on disk, and when its own compressed ".gz" form
 * already exists while check_gz is true because compress_rotated is on. The
 * second check matters because _gzip_compress_file() unlinks its
 * uncompressed source the instant that the compression succeeds. When a
 * LATER rotation inside the same second runs its own collision check, a
 * bare existence check of candidate makes the candidate name of the earlier
 * rotation look completely free again, because that rotation compressed and
 * deleted it, while "<candidate>.gz" sits on disk and holds the real content
 * of that earlier rotation. Reusing that name would hand the ".gz"
 * destination of the earlier rotation straight back to a second, unrelated
 * rotation.
 */
static bool _rotated_name_taken(int dir_fd, const char *candidate,
                                bool check_gz) {
  if (_clog_entry_exists(dir_fd, candidate)) return true;
  if (!check_gz) return false;
  /* This buffer has room above PATH_MAX, so the code does not need to reason
   * exactly about how much of the CLOG_ROTATION_EXTRA slack of _rotate() is
   * left at the point that calls this function. candidate is always a
   * NUL-terminated string that itself fit inside a buffer of PATH_MAX bytes,
   * so clen is below PATH_MAX, and this buffer has plenty of room for
   * clen + strlen(".gz") + 1 in every case. */
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
 * thread, which the first rotation that needs a compression starts, so a
 * target that never compresses never pays for it. _rotate() only queues a
 * job and returns, so the gzip work never runs on the thread that rotates
 * (a logging thread for a synchronous logger, and the writer thread for an
 * async one), and neither a log call nor the async queue behind it ever
 * waits for a compression.
 *
 * One thread for each target runs the jobs one at a time, the oldest first,
 * so at most one compression of a target is in flight at any moment, which
 * is what bounds the files on disk to max_rotated_files generations plus one
 * (see _prune_rotated()). The thread shares nothing with the compressor of
 * another target, so a close or a fork only ever deals with the thread of
 * its own target, under the mutex that the fork handling already locks for
 * that target.
 *
 * The last clog_close() of the target stops the thread after the thread
 * finishes every job in the queue. The exit drain, which a CLOG_FATAL record
 * runs too, waits for the queue within its budget; see _clog_exit_drain().
 */
/* The shared target whose writer thread or compressor thread the calling
 * thread is, or NULL. The exit drain skips that target: such a thread that
 * calls exit() cannot also serve the drain it would wait for, and it can
 * hold the mutex of that target. See _clog_exit_drain(). */
static __thread const clog_shared_t *_clog_thread_serves;

/* True once this process has started a writer thread or a compressor thread,
 * which is the only state that the exit drain has work for. */
static _Atomic bool _clog_exit_drain_armed;

static void _clog_exit_drain_arm(void) {
  atomic_store_explicit(&_clog_exit_drain_armed, true, memory_order_relaxed);
}

static void *_clog_compressor_main(void *arg) {
  clog_shared_t *sh = (clog_shared_t *)arg;
  _clog_thread_serves = sh;
  ccol_mutex_lock(sh->mutex);
  for (;;) {
    while (!sh->compress_head && !sh->compressor_stop)
      ccol_cond_var_wait(sh->compress_cv, sh->mutex);
    clog_compress_job_t *job = sh->compress_head;
    if (!job) break; /* stop requested and nothing left to do */
    sh->compress_head = job->next;
    if (!sh->compress_head) sh->compress_tail = NULL;
    job->next = NULL;
    sh->compress_running = job;
    int dir_fd = sh->dir_fd;
    ccol_mutex_unlock(sh->mutex);

    _gzip_compress_file(dir_fd, job->src, job->dst, &sh->compress_abort,
                        job->live_uid);

    ccol_mutex_lock(sh->mutex);
    sh->compress_running = NULL;
    _ccol_mem_free(sh->m_procs, job);
    /* A pass that ran during the compression kept this generation only
     * because it was running. It is older than the newest max_rotated_files,
     * and without this pass it would stay on disk beside the newer
     * generations until the next rotation. */
    if (sh->compress_prune_deferred) {
      sh->compress_prune_deferred = false;
      _prune_rotated(sh, NULL, false);
    }
    /* Wakes every thread that waits for the queue to drain. */
    ccol_cond_var_broadcast(sh->compress_cv);
  }
  ccol_mutex_unlock(sh->mutex);
  return NULL;
}

/* Starts the compressor thread of sh. The caller holds sh->mutex, and
 * sh->compressor_live is false. It gives false when the thread cannot start,
 * and then leaves nothing behind. */
static bool _clog_compressor_start(clog_shared_t *sh) {
  /* CLOCK_MONOTONIC, so that the bounded wait of the exit drain on this
   * condition variable does not move with the wall clock. */
  ccol_cond_var_attr_t ca;
  if (ccol_cond_var_attr_init(ca) != 0) return false;
  int cv_rc = ccol_cond_var_attr_setclock(ca, CLOCK_MONOTONIC);
  if (cv_rc == 0) cv_rc = ccol_cond_var_init_ca(sh->compress_cv, ca);
  ccol_cond_var_attr_destroy(ca);
  if (cv_rc != 0) return false;
  _clog_exit_drain_arm();
  sh->compressor_stop = false;
  if (ccol_thread_create(sh->compressor_thread, _clog_compressor_main, sh) !=
      0) {
    ccol_cond_var_destroy(sh->compress_cv);
    return false;
  }
  sh->compressor_live = true;
  return true;
}

/*
 * Queues the compression of the rotated file `rotated`, a name relative to
 * sh->dir_fd. The caller holds sh->mutex. The job is one allocation that
 * holds both of its names. When the allocation fails, or the compressor
 * thread cannot start, the rotated file stays uncompressed, which is the
 * documented outcome of a failed compression, and no log data is lost.
 * live_uid is the owner of the live file (see clog_gen_filter_t).
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
  ccol_cond_var_broadcast(sh->compress_cv);
}

/*
 * Queues again every compression that a process which stopped left
 * unfinished in the directory of sh. The caller holds sh->mutex, and sh
 * rotates and compresses. A compression that stops part way leaves its
 * temporary output (see _gzip_compress_file()), which this removes. A
 * rotated file that is still uncompressed was never compressed, or its
 * compression never finished, because a compression removes its source only
 * after the finished output took the ".gz" name. So a ".gz" file beside such
 * a source is an output that is not known to be complete, while the source
 * holds everything, and this removes the ".gz" file. Every uncompressed
 * rotated file then goes to the compressor thread, the oldest first.
 *
 * It acts only on generations of sh (see clog_gen_filter_t): a temporary
 * output, a source and a ".gz" file are each removed or compressed only
 * when they are one, and every other entry gets a WARN record. A source
 * whose ".gz" name holds an entry that is not a generation stays
 * uncompressed.
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
 * Stops and joins the compressor thread of sh, if it runs. The thread first
 * finishes every job in its queue, so every rotation of the target is
 * compressed by the time that this returns. The caller must not hold
 * sh->mutex, and no handle of sh may remain that could rotate: the last
 * clog_close() calls this after the writer thread (which can still rotate
 * while it drains) has stopped. A constructor rollback calls it too, where
 * it does nothing.
 */
static void _shared_compressor_teardown(clog_shared_t *sh) {
  ccol_mutex_lock(sh->mutex);
  bool live = sh->compressor_live;
  if (live) {
    sh->compressor_stop = true;
    ccol_cond_var_broadcast(sh->compress_cv);
  }
  ccol_mutex_unlock(sh->mutex);
  if (!live) return;
  ccol_thread_join(sh->compressor_thread);
  ccol_cond_var_destroy(sh->compress_cv);
  sh->compressor_live = false;
}

/* ========================================================================== */
/*                         ROTATE                                             */
/* ========================================================================== */

/* 64 bits that another user cannot predict, for the private name of a new
 * live file. A system that cannot answer getrandom(2) at once gets a mix of
 * clocks, the pid, a counter and addresses; the O_EXCL creation stays
 * correct either way, and only its resistance to a name planted in advance
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

/* True when name is "<base>" CLOG_ROTATION_TMP_TAG and
 * CLOG_ROTATION_TMP_HEX_DIGITS lowercase hex digits: the private name of a
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
 * Creates the next live file of a rotation under a private name of its own,
 * relative to dir_fd, and writes that name to tmp (tmp_size bytes). The name
 * is "<base>" CLOG_ROTATION_TMP_TAG plus random hex digits, and O_EXCL with
 * O_NOFOLLOW creates a new regular file there or fails, so the file can
 * never be an entry that somebody else put at the name. When old is not
 * NULL, the file gets the permission bits, the owner and the group of the
 * file that it replaces, which *old describes, so a log that its operator
 * restricted, such as one with mode 0600 or 0640, stays restricted in every
 * later generation, and a compressed generation takes the access of its
 * source (see _gzip_compress_file()). The file is created with mode 0600,
 * and _clog_copy_owner_and_mode() gives it the rest. With old NULL it is
 * created with mode 0644, which the umask narrows. It gives the descriptor
 * and fills *st_out with the finished file, or gives -1.
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
 * Removes the private name of a new live file that a rotation left when its
 * process stopped between the creation of the file and its rename onto the
 * live name (see _rotate()). Such a file is empty, because nothing writes it
 * before that rename, and only an empty regular file that the effective
 * user or the owner of the live file owns, at a name of exactly that shape,
 * is removed. The caller holds sh->mutex, or owns sh alone, and sh rotates.
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
/* A hook that _rotate() calls between the moment the live name stops naming
 * the old file and the moment the new live file takes the name, for a test
 * that puts an entry at the live name inside that window. */
static void (*_Atomic _clog_test_rotate_window_hook)(int dir_fd,
                                                     const char *base);

void clog_test_set_rotate_window_hook(void (*hook)(int dir_fd,
                                                   const char *base)) {
  atomic_store(&_clog_test_rotate_window_hook, hook);
}
#endif

/*
 * This rotates the current log file. The caller must hold shared->mutex.
 * Every name is relative to sh->dir_fd, the directory in which the logger
 * opened the file.
 *
 * Steps:
 *   1. Create the next live file under a private name (see
 *      _clog_create_fresh()).
 *   2. Build a destination name with a timestamp and, when needed, a
 *      collision suffix. The name sorts above every rotated name before it.
 *   3. Rename the current file to the destination, never replacing an entry
 *      that sits there.
 *   4. Rename the new file onto the live name, and close the old fd.
 *   5. Delete the old rotated files beyond max_rotated_files.
 *   6. Queue the compression of the rotated file, when it is on.
 *
 * The rotation never opens the live name or a rotated name. The new live
 * file is a file that step 1 made, and step 4 puts it at the live name with
 * a rename, which replaces whatever entry sits there without following it.
 * So another user who can write the directory and puts a link, a FIFO or a
 * file at the live name between steps 3 and 4 never receives a record and
 * never blocks the rotation: the live name belongs to the logger.
 */
static int _rotate(clog_shared_t *sh) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&_clog_test_rotate_attempt_count, 1);
#endif

  if (sh->dir_fd < 0 || sh->fd < 0) {
    return 0;
  }

  time_t now = time(NULL);
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
   * (stamp, sequence number) pairs are the newest (see
   * _cmp_rotated_oldest_first()), so every new name must sort above every
   * name that this shared target made before, and above every generation
   * that was on disk before its first rotation. A name that the deletion
   * pass freed must never come back, because it sorts as the oldest, and the
   * next pass would delete the newest log records in its place.
   *
   * A clock that reads the same second as the newest name continues its
   * sequence. A clock that reads an earlier second, because it stepped
   * backward, keeps the stamp of the newest name and continues its sequence
   * too, so the order of the names stays the order of the rotations. The
   * derived loggers of this target share these fields under sh->mutex. An
   * entry that is not a generation (see clog_gen_filter_t) plays no part
   * here, so an entry with a stamp far in the future cannot hold the names
   * at a stamp whose sequence is used up. */
  if (!sh->rot_name_known)
    sh->rot_name_known = _rotated_newest_on_disk(sh, true, sh->rot_last_stamp,
                                                 &sh->rot_last_seq);
  unsigned long seq = 0;
  if (sh->rot_name_known &&
      memcmp(stamp, sh->rot_last_stamp, CLOG_ROTATION_STAMP_DIGITS) <= 0) {
    memcpy(stamp, sh->rot_last_stamp, CLOG_ROTATION_STAMP_DIGITS);
    seq = sh->rot_last_seq + 1;
  }

  /* The fstat() describes the file that the logger writes, from which the
   * new live file takes its access. It reads the descriptor and never the
   * name. */
  struct stat old_st;
  bool have_old_st = fstat(sh->fd, &old_st) == 0;
  char tmp[PATH_MAX];
  struct stat new_st;
  int new_fd = _clog_create_fresh(sh->dir_fd, sh->file_base,
                                  have_old_st ? &old_st : NULL, tmp,
                                  sizeof(tmp), &new_st);
  if (new_fd < 0) return -1;

  /* The name can still be taken by an entry that this target did not make,
   * and the sequence then moves on until the name is free. check_gz applies
   * the same test to the compressed form of the name whenever compression is
   * on (see the doc comment of _rotated_name_taken()). The rename never
   * replaces an entry at the destination, and an entry that appears there
   * between the test and the rename moves the sequence on too.
   *
   * The rename happens while the old fd is still open, which POSIX allows
   * for an open file, and the code closes the old fd only once the new file
   * holds the live name. So a step that fails leaves the logger alive, and
   * the writes continue into the old file. ENOENT means that something
   * outside this process deleted the file; the code treats that as a clean
   * slate, and the new file takes the live name. Any other rename error is a
   * hard failure, and the logger then continues to write to the original fd,
   * which is still open. */
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
    _clog_unlink_if_same(sh->dir_fd, tmp, &new_st);
    close(new_fd);
    return -1;
  }
  /* `rotated` exists on disk only when the rename above truly succeeded; on
   * the ENOENT clean-slate path there is nothing at that path to delete and
   * nothing there to compress. */
  bool did_rename = (rename_rv == 0);

#ifdef RUNNING_UNIT_TESTS
  {
    void (*hook)(int, const char *) =
        atomic_load(&_clog_test_rotate_window_hook);
    if (hook) hook(sh->dir_fd, sh->file_base);
  }
#endif

  if (renameat(sh->dir_fd, tmp, sh->dir_fd, sh->file_base) != 0) {
    /* Recovery: restore the original name, so that the fd that is still
     * open stays the live file, unless an entry already took the name. */
    if (did_rename)
      (void)_clog_publish_no_replace(sh->dir_fd, rotated, sh->file_base);
    _clog_unlink_if_same(sh->dir_fd, tmp, &new_st);
    close(new_fd);
    return -1;
  }

  close(sh->fd);
  sh->fd = new_fd;
  sh->sink_kind = _clog_classify_sink(new_fd);
  sh->sink_per_record = _clog_sink_is_message(new_fd);
  sh->bytes_written = 0;
  sh->last_rotation = now;
  memcpy(sh->rot_last_stamp, stamp, CLOG_ROTATION_STAMP_DIGITS);
  sh->rot_last_seq = seq;
  sh->rot_name_known = true;

  /* The deletion pass runs only once the rotation has certainly succeeded,
   * which means that the new live file holds the live name. A deletion
   * before this point would let a rotation that then fails permanently
   * delete older rotated files for an attempt that the code rolls back. */
  if (sh->rotation.max_rotated_files > 0) {
    _prune_rotated(sh, did_rename ? rotated : NULL, true);
  }

  /* did_rename gates this, because on the ENOENT clean-slate path above the
   * code never created `rotated`, so there is nothing to compress. A
   * compression writes "<rotated>.gz" + CLOG_GZ_TMP_SUFFIX and then
   * "<rotated>.gz". When the directory cannot hold a name that long, no
   * compression of this file can succeed; the file stays uncompressed, and a
   * record in the new live file says so instead of the compression failing
   * unseen. */
  if (did_rename && sh->rotation.compress_rotated) {
    long name_max = fpathconf(sh->dir_fd, _PC_NAME_MAX);
    size_t gz_tmp_len = strlen(rotated) + sizeof(".gz" CLOG_GZ_TMP_SUFFIX) - 1;
    if (name_max > 0 && gz_tmp_len > (size_t)name_max)
      _clog_note_record(sh, sh->format, CLOG_WARN, "",
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
 * This is the time-based rotation check that runs before a write. It rotates
 * at once when time rotation is on and the interval ends, unless the retry
 * backoff stops it. Every code path that is about to write the real bytes of
 * a record to sh->fd calls this function, so time-based rotation can never
 * be silently skipped by one such path while every other path runs it. The
 * caller must hold sh->mutex.
 */
static void _clog_time_rotate_if_due(clog_shared_t *sh) {
  if (!(sh->rotation_enabled && sh->rotation.time_rotation_enabled)) return;
  /* The code defers the rotation while the async batch buffer holds the
   * continuation of a record whose first half is already on disk, because a
   * rotation here puts the two halves in two different files. See
   * clog_shared_t.async_partial_record. */
  if (sh->async_partial_record) return;
  time_t now = time(NULL);
  /* The code widens both operands to 64 bits before the subtraction, because
   * time_t is as narrow as 32 bits on some targets, where the difference of
   * two distant timestamps can overflow it. The elapsed whole seconds are
   * compared in microseconds with rotation_interval_us, so an interval that
   * is not a whole number of seconds rotates at the next whole second after
   * it ends. A count of seconds too large to multiply is past every
   * interval, and a clock that stepped back makes the difference negative,
   * which is not yet due. */
  int64_t since = (int64_t)now - (int64_t)sh->last_rotation;
  if (since >= 0 &&
      ((uint64_t)since >= UINT64_MAX / UINT64_C(1000000) ||
       (uint64_t)since * UINT64_C(1000000) >=
           sh->rotation.rotation_interval_us) &&
      now >= sh->rotate_retry_after) {
    int rrv = _rotate(sh);
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

/*
 * This is the size-based rotation check that runs after a write. It rotates
 * at once when size rotation is on and bytes_written crosses max_file_size,
 * unless the retry backoff stops it. Every path shares this function for the
 * same reason that every path shares _clog_time_rotate_if_due(). The caller
 * must hold sh->mutex, and must call it only after sh->bytes_written already
 * covers the write that just happened.
 */
static void _clog_size_rotate_if_due(clog_shared_t *sh) {
  /* bytes_written, the internal counter of this file, is an off_t, while
   * max_file_size is a fixed-width int64_t that is part of the layout of the
   * public config. The code widens the counter, which keeps the comparison
   * correct whatever width off_t has in this build. */
  if (!(sh->rotation_enabled && sh->rotation.size_rotation_enabled &&
        (int64_t)sh->bytes_written >= sh->rotation.max_file_size))
    return;
  /* The code defers the rotation for the same reason that
   * _clog_time_rotate_if_due() defers it. The flush that clears the flag
   * evaluates this check again, so the rotation that this line skips happens
   * as soon as the record on disk is whole again. See
   * clog_shared_t.async_partial_record. */
  if (sh->async_partial_record) return;
  time_t now = time(NULL);
  if (now >= sh->rotate_retry_after) {
    int rrv = _rotate(sh);
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

/* ========================================================================== */
/*                         BACKTRACE                                          */
/* ========================================================================== */

/* Frame 0 is _capture_backtrace, frame 1 is _clog_write, and frame 2 is the
 * caller, which is the first frame that the user sees. Every format below
 * that writes a backtrace uses this value. */
#define CLOG_BT_INITIAL_FRAME 2

/*
 * This captures the backtrace of the thread that calls. It is noinline, and
 * the frame of _clog_write calls it directly on every path (the synchronous
 * path, the async path and the FATAL path), never from a helper one level
 * deeper. CLOG_BT_INITIAL_FRAME above is correct only while that stays true:
 * a capture from inside the async submission path, for example, silently
 * shows that function itself as if it were the caller. This function gives
 * false, and leaves *out_syms and *out_depth untouched, in two cases: on a
 * platform with no backtrace support, and when a backtrace_symbols() call
 * fails its own allocation. A caller must treat false in exactly the same
 * way as "no frames".
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
  /* See the doc comment of clog_test_force_shallow_backtrace_depth(). syms
   * stays a real capture and is not NULL, and the code clamps only the depth
   * that it reports, so every emitter sees the same shape as for a real
   * shallow call stack: the capture truly succeeded, and it found nothing
   * past the first two frames. */
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
 * This is a fixed marker that the library writes in place of the usual
 * "\t#N ..." continuation lines whenever it could not capture the real
 * backtrace at all, which happens on a platform with no support and when
 * the allocation inside backtrace_symbols() fails. Without this marker, a
 * syms of NULL makes the emitter do nothing and say nothing, so a reader of
 * a logfmt record that asked for a backtrace cannot tell apart the state in
 * which the library left the backtrace out from the state in which nothing
 * ever asked for a backtrace. JSON carries that same distinction with
 * _BT_JSON_ERROR_MARKER.
 */
static const char _BT_LINE_ERROR_MARKER[] = "\t#error backtrace unavailable\n";

/*
 * This appends the backtrace frames into out as tab-indented continuation
 * lines; it never writes anything to a file itself. A syms of NULL means
 * that no backtrace is available, or that _capture_backtrace() itself
 * failed, and the function then appends the single fixed
 * _BT_LINE_ERROR_MARKER line instead, so the absence of the backtrace is
 * never silent.
 *
 * The function gives true when `out` gives an accurate picture of the
 * backtrace: the frames that fit, no frame because none existed, or a marker
 * that shows that a requested backtrace is missing. That happens in three
 * cases. In the first, the function appended at least one real frame line: a
 * symbol name that is longer and does not fit is silently skipped while a
 * later, shorter one that fits goes in, instead of the function giving up on
 * the whole backtrace the moment that the first frame does not fit. A frame
 * skipped in this way carries no marker of its own, which matches the contract
 * of this function, best-effort for each frame. In the second case, depth is at
 * most CLOG_BT_INITIAL_FRAME: the capture truly succeeded but found no frame
 * beyond the first two bookkeeping ones, so there is nothing to append and
 * nothing to report as missing. _emit_backtrace_json() makes the same
 * distinction with its own array_content_is_accurate flag, so a capture that is
 * real but shallow is never reported as an unavailable one; without this case,
 * it would fall through into "not a single frame fit". In the third case, real
 * frames existed, not a single one fit, and the function appended the
 * fixed-size _BT_LINE_ERROR_MARKER in their place.
 *
 * The function gives false only when `out` has almost no room left at all,
 * not even for the marker of about 30 bytes. That case is residual, and the
 * buffer-size constants of this file make it unreachable in practice,
 * because the caller already has plenty of room for the primary record and
 * the record-start position of this call sits after that record. See the
 * call site in _clog_build_record() for how the code handles this very
 * narrow case.
 */
static bool _emit_backtrace_lines(clog_buf_t *out, char *const *syms,
                                  int depth) {
  if (!syms)
    return _buf_append(out, _BT_LINE_ERROR_MARKER,
                       sizeof(_BT_LINE_ERROR_MARKER) - 1) == 0;

  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    size_t entry_start = out->len;
    /* syms[i] comes from backtrace_symbols(), and for the line-based framing
     * of this format it is untrusted text of any shape. So the code routes it
     * through the same control-character escaper that every other dynamic
     * field of this format uses, which means that an unusual symbol name,
     * and an embedded newline in particular, can never desynchronize the log
     * stream, as an unescaped name can. The build of the line for this frame
     * can fail when an allocation for the growth of the buffer fails, and the
     * code then rolls back to entry_start and skips the frame; without that
     * rollback, a truncated fragment with no newline stays behind, and the
     * text of the next frame goes straight onto it. */
    bool ok = _buf_appendf(out, "\t#%d ", i - CLOG_BT_INITIAL_FRAME) == 0;
    ok = ok && _buf_append_ctrl_escaped(out, syms[i]) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
    if (!ok) {
      out->len = entry_start;
      continue;
    }
    any_frame_written = true;
  }
  /* A depth of at most CLOG_BT_INITIAL_FRAME means that no frame beyond the
   * first two ever existed, so writing nothing here is a real and accurate
   * picture of a capture that succeeded, even though it is silent, and not a
   * truncation. The array_content_is_accurate check of
   * _emit_backtrace_json() covers the same case in the same way. */
  if (any_frame_written || depth <= CLOG_BT_INITIAL_FRAME) return true;

  /* Real frames existed, but not one single frame fit, because the line of
   * each frame on its own needed more room than was left. The code falls
   * back to the same small marker of a fixed size that the branch for
   * syms == NULL above uses, so a backtrace that is real but of which the
   * record shows nothing is never the same as one that nothing ever asked
   * for. */
  return _buf_append(out, _BT_LINE_ERROR_MARKER,
                     sizeof(_BT_LINE_ERROR_MARKER) - 1) == 0;
}

/*
 * This is a marker of a fixed size that the library writes in place of the
 * "bt" array whenever it cannot put the real backtrace into the record:
 * after a backtrace_symbols() failure, and after an allocation failure
 * during the growth of the buffer while the code appends the array itself or
 * the structure around one of its frames. The whole purpose of the marker is
 * to make the record show that a requested backtrace is missing, rather than
 * leave it silent. A caller that reads an ERROR, ALERT or FATAL record for
 * which with_backtrace was true must be able to tell "backtrace left out"
 * apart from "backtrace never asked for", and must also never see the real
 * ts, level and msg content of this record replaced by the general fallback
 * for an oversized record, a replacement that is needless when only the
 * backtrace text, which is usually much larger, did not fit.
 */
static const char _BT_JSON_ERROR_MARKER[] = ",\"bt_error\":\"unavailable\"";

/*
 * This appends the backtrace frames as a JSON array:
 * ,"bt":["#0 sym","#1 sym",...]
 * The caller calls it while the JSON object is still open, before the
 * closing "}\n". A syms of NULL is a capture failure here, exactly as for
 * every other backtrace emitter in this file.
 *
 * The function gives true when the code can safely close the record as it
 * is, which is so in two cases: when it appended a complete "bt" array,
 * which can be short of some frames, and when it appended the fixed-size
 * _BT_JSON_ERROR_MARKER in place of the array. It gives false only when it
 * could not append even that small marker, because the buffer has almost no
 * room left at all, and the caller must then fall back to the general
 * placeholder for an oversized record.
 *
 * This function follows the best-effort-for-each-frame contract of
 * _emit_backtrace_lines() in two ways that a plain "append until something
 * fails" shape does not give for free. First, a frame that does not fit is
 * skipped with `continue` and not with `break`, so a later frame with
 * shorter symbol text still gets its own chance to fit, instead of the code
 * giving up the instant that an earlier frame does not fit. Second, the code
 * closes an empty array as "]" only when that emptiness is real, that is,
 * when nothing ever captured a frame beyond the first two. When real frames
 * exist (depth > CLOG_BT_INITIAL_FRAME) and not one of them fits, an empty
 * "bt":[] would be byte for byte the same as the truly shallow case, which
 * silently defeats the whole reason that this function exists, so the code
 * shows the omission with the same _BT_JSON_ERROR_MARKER that every other
 * zero-frames-fit case in this file uses.
 */
static bool _emit_backtrace_json(clog_buf_t *b, char *const *syms, int depth) {
  if (!syms)
    return _buf_append(b, _BT_JSON_ERROR_MARKER,
                       sizeof(_BT_JSON_ERROR_MARKER) - 1) == 0;

  size_t bt_start = b->len;
  if (_buf_append(b, ",\"bt\":[", 7) != 0) {
    /* Nothing went in, so bt_start is equal to b->len. Show the omission
     * instead of silently leaving the record with no "bt" key at all. */
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
          frame instead of giving up on the rest of the array */
      continue;
    }
    first = false;
    any_frame_written = true;
  }

  /* A depth of at most CLOG_BT_INITIAL_FRAME means that no frame beyond the
   * first two ever existed, so an empty array here is real and not a
   * truncation. any_frame_written covers the other case, in which real
   * frames existed and at least one of them fit. In both cases it is safe to
   * close the array, because it shows accurately what happened. */
  bool array_content_is_accurate =
      any_frame_written || depth <= CLOG_BT_INITIAL_FRAME;
  if (array_content_is_accurate && _buf_append(b, "]", 1) == 0) return true;

  /* Two things bring the code here: either the append of "]" itself failed,
   * which means that almost no room is left at all, or real frames existed,
   * not one of them fit, and a closed empty array here would look the same
   * as a truly shallow backtrace. In both cases the code discards whatever
   * partial content is in `b`, because a "bt" key with no closing bracket
   * corrupts the record and an empty key that misleads the reader
   * misrepresents it, and shows the omission with the marker instead. */
  b->len = bt_start;
  return _buf_append(b, _BT_JSON_ERROR_MARKER,
                     sizeof(_BT_JSON_ERROR_MARKER) - 1) == 0;
}

/*
 * This writes the backtrace frames as separate RFC 5424 syslog messages, one
 * for each frame, and never batches them, whatever the async mode is, which
 * matches the contract of one write() for each UDP datagram that
 * clog_syslog_facility_t documents. The function uses out only as scratch
 * space: it resets out before each frame and leaves it reset after each
 * frame. out can be the buffer of one logger, on the synchronous path, or
 * the shared aggregation buffer of the writer thread, on the async path;
 * both are safe, because the code only ever touches them while it holds
 * sh->mutex. The caller must hold sh->mutex. A syms of NULL means that no
 * backtrace is available, or that _capture_backtrace() itself failed, and
 * the function then writes one fixed marker record instead of nothing, for
 * the same reason as _emit_backtrace_lines() does for logfmt: without the
 * marker, a reader has no way to tell "the backtrace was left out" apart
 * from "no backtrace was ever asked for". A syms that is not NULL, with a
 * depth of at most CLOG_BT_INITIAL_FRAME, writes nothing at all, because
 * the capture truly succeeded but found no frame beyond the first two
 * bookkeeping ones; the array_content_is_accurate check of
 * _emit_backtrace_json() knows that same truly empty case. Only real frames
 * that all failed to go out count as an omission worth a marker.
 *
 * ts is the SAME captured timestamp as the primary record that these frames
 * continue (the `ts` or the `job->ts` of the caller), and this function
 * never derives it again. For an async logger, the writer thread may build
 * these lines much later than the time at which something submitted the
 * record, and every other piece of per-record data in this file works the
 * same way: the fields, proc_val and the symbols themselves all come from
 * one capture at submission time, and the code does not read them again at
 * write time. The TIMESTAMP field of a backtrace frame must agree with the
 * one of its primary record, and must not show whatever the wall clock reads
 * when the writer thread finally processes the job.
 */
/* The HOSTNAME field of every RFC 5424 record of sh, the header of a record
 * and each backtrace line alike: the cached name, or "-", the NILVALUE of
 * the grammar, when the cache is empty. */
static inline const char *_clog_syslog_hostname(const clog_shared_t *sh) {
  return sh->syslog_hostname[0] ? sh->syslog_hostname : "-";
}

/*
 * This builds and writes the one fixed "backtrace unavailable" marker
 * record, which _emit_backtrace_syslog_lines() below uses in two cases: for
 * a capture that failed, where syms == NULL, and for a capture that
 * succeeded but where not one single frame could go out, because the append
 * of every frame failed (for example under an allocation failure that
 * lasts). This function resets `out` first, which matches the contract that
 * each frame has for its scratch space (reset it before use), and leaves
 * `out` reset afterwards on every path, which matches the documented
 * contract of _emit_backtrace_syslog_lines() (leave it reset after use).
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
                          " %s %s %d %s - \t#error backtrace "
                          "unavailable\n",
                          hostname, appname, (int)_clog_tident_get()->pid,
                          msgid) == 0;
  if (ok) {
    _clog_write_record(sh, CLOG_FMT_SYSLOG, out->data, out->len);
  } else {
    /* Not even this small marker record fit into `out`: a real allocation
     * failure sits on top of a backtrace capture that already failed, or on
     * top of every single frame that already failed to append for the same
     * reason. The code falls back to a minimal write straight to the fd,
     * with no allocation, following the last-resort pattern of
     * _clog_write_unrepresentable_record(), which cannot itself fail: it
     * builds a stack buffer of a fixed size with snprintf and never touches
     * clog_buf_t or the heap allocation machinery at all. So this double
     * failure is never a record loss that is completely silent. The buffer
     * is much larger than the worst case (a hostname and an appname of full
     * length plus the literal text around them), so this snprintf cannot
     * truncate in practice. */
    struct tm tm;
    gmtime_r(&ts->tv_sec, &tm);
    char fallback[512];
    int n = snprintf(fallback, sizeof(fallback),
                     "<%d>1 %04d-%02d-%02dT%02d:%02d:%02d.%06ldZ %s %s %d %s - "
                     "\t#error backtrace unavailable\n",
                     pri, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (long)ts->tv_usec,
                     hostname, appname, (int)_clog_tident_get()->pid, msgid);
    if (n > 0) {
      size_t len =
          (size_t)n < sizeof(fallback) ? (size_t)n : sizeof(fallback) - 1;
      _clog_write_record(sh, CLOG_FMT_SYSLOG, fallback, len);
    }
  }
  /* Leave `out` reset on every return path, which matches the documented
   * contract of this function (leave it reset after use). `out` can be
   * sh->async_buf, and the length of that buffer is also the signal that the
   * async writer thread uses for "data is pending and not yet flushed" (see
   * _writer_flush_now()). The bytes of the marker that the code just wrote
   * must not stay in `out->len` here, or the next unrelated flush writes
   * those same bytes to sh->fd a second time, although the library already
   * sent them. Three things trigger such a flush: an idle flush_interval_us
   * timeout, an explicit clog_flush(), and the final drain of
   * clog_close(). */
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
    /* The code builds the line of each frame from several separate appends,
     * and one of them can fail partway through, for example when the
     * timestamp fits but the rest does not. That would leave a truncated
     * fragment with no newline in the buffer, which _write_all() still
     * writes, and the fragment would then merge into whatever goes out next
     * and desynchronize the syslog stream. So the code tracks success across
     * all of the appends and skips the frame completely rather than write a
     * partial one. syms[i] comes from backtrace_symbols(), and the code
     * routes it through the same control-character escaper that the MSG
     * content of the primary record uses, so an unusual symbol name, and an
     * embedded newline in particular, can never desynchronize the syslog
     * stream, as an unescaped name can. */
    bool ok = _buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_appendf(out, " %s %s %d %s - \t#%d ", hostname, appname,
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
  /* See the same note above. Leave `out` reset after the LAST frame too, and
   * not only before each one, because the loop resets it only on the way
   * INTO an iteration and never after the final write on the way out of the
   * loop. */
  _buf_reset(out);

  /* A depth of at most CLOG_BT_INITIAL_FRAME means that no frame beyond the
   * first two ever existed, so writing no frame record here is a real and
   * accurate picture of a capture that succeeded, not an omission. The
   * array_content_is_accurate check of _emit_backtrace_json() makes the same
   * exception, and so does _emit_backtrace_lines(). Only real frames (depth
   * > CLOG_BT_INITIAL_FRAME) that ALL failed to go out count as a real
   * omission that needs a marker. */
  if (!any_frame_written && depth > CLOG_BT_INITIAL_FRAME) {
    /* The capture succeeded, because syms is real, but the append of every
     * single frame failed. Only a real allocation failure that lasts can
     * reach this state: each frame gets an `out` that the code just reset,
     * so the buffer cannot run out of room because of unrelated earlier
     * content, as can happen to the shared batch buffer of logfmt. Without
     * this branch, a backtrace that is real but of which the record shows
     * nothing would look the same as one that nothing ever asked for; the
     * branch for a capture failure above, where syms == NULL, gives that same
     * guarantee. */
    _write_backtrace_unavailable_syslog_marker(out, sh, level, ts);
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * This calls the syms == NULL branch of _emit_backtrace_syslog_lines(), the
 * one for a backtrace capture that failed, directly against the shared
 * target of logger, for a test. clog_test_gzip_compress_file() sets the
 * precedent for a direct call into an internal helper that is hard to
 * reach. This branch has its own last-resort fallback for when even its
 * small marker record fails to append, and a test can reach that fallback
 * only when it forces a backtrace capture failure and a buffer append
 * failure together, at exactly this call, because the buffer-size constants
 * of this library otherwise give the process no way to fail such a small
 * append at all. See the doc comment of
 * clog_test_force_next_buf_ensure_failure().
 */
void clog_test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                        clog_level_t level) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return;
  struct timeval ts;
  gettimeofday(&ts, NULL);
  ccol_mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    _emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, NULL, 0);
  ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}
#endif

/* ========================================================================== */
/*                         ASYNC MESSAGE TYPES                                */
/* ========================================================================== */

typedef enum { CLOG_ASYNC_MSG_JOB, CLOG_ASYNC_MSG_FLUSH } clog_async_msg_kind_t;

/*
 * These are offsets into the field_pool of a clog_async_job_t, NOT raw
 * pointers. The pool is a ccol_growbuf_t that _snapshot_fields() builds one
 * piece at a time, and the backing store of a growbuf can move on a realloc
 * in the middle of that build. An offset that the code records during that
 * same build stays valid whatever moves happen, while a raw pointer captured
 * in the middle of the build does not.
 */
typedef struct {
  size_t key_offset;
  size_t value_offset;
} clog_field_view_t;

/*
 * This is one unit of work for the writer thread. It goes into a queue and
 * is fully self-contained: it holds every piece of information that
 * _clog_build_record() needs, all captured by the submitting thread at
 * submission time. The writer thread never derives any of it again, because
 * that thread can run much later, and it is a different thread from the
 * one that formatted the message.
 */
typedef struct clog_async_job {
  clog_level_t level;
  const char *file; /* __FILE__ literal, never copied */
  int line;
  const char *func; /* __func__ literal, never copied */
  bool with_backtrace;
  struct timeval ts; /* captured at SUBMISSION time, not at write time */
  /* progname(pid):tname(tid), captured at submission, because the identity
   * of the thread must come from here and not from the writer thread. */
  char proc_val[CLOG_PROC_VAL_LEN];

  char msg_inline[256]; /* A stack buffer that spills to the heap and then
      moves into the job. It is smaller than the 1024-byte stack buffer of
      the synchronous path, because here it is a heap cost for each queued
      message rather than a cost on the stack of a thread */
  char *msg_heap;       /* NULL unless the message spilled past msg_inline */
  const char *msg;      /* == msg_inline or msg_heap. It is never NULL */

  clog_field_view_t *fields; /* An array of field_count views, or NULL when
      field_count == 0. It is one allocation of the exact size from
      chmap_elem_count(), a count that is O(1) and known before the
      iteration, so the code needs no separate pass to count */
  size_t field_count;
  ccol_growbuf_t field_pool;   /* The key and value bytes, separated by NUL.
        See the comment of clog_field_view_t. The code builds it in one pass
        under fields_mutex */
  bool fields_snapshot_failed; /* True only when the code could not take the
      snapshot at all, because of the out-of-memory latch of field_pool or
      because the views array itself failed to allocate. That is different
      from a logger that truly has 0 fields */

  char **bt_syms; /* The result of backtrace_symbols(), captured
      by the ORIGINAL thread that calls, because a backtrace from the stack
      of the writer thread would mean nothing */
  int bt_depth;
} clog_async_job_t;

/* This is the rendezvous on which clog_flush() blocks. The thread that asks
 * waits on ctrl.cv, while the writer thread first flushes everything that
 * sits in the queue ahead of this request and then sets ctrl.done and
 * broadcasts. This object always sits on the stack frame of the thread that
 * asks (see _clog_flush_pinned()), and the writer thread never frees it. */
typedef struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  bool done;
} clog_async_ctrl_t;

/*
 * This is the envelope that truly goes through the queue. kind is an
 * explicit discriminator, not an implicit check on a coincidence of sizeof
 * values, so the writer thread can always tell a real job apart from a
 * flush request, even if a future field addition makes two struct sizes
 * equal by accident.
 */
typedef struct clog_async_msg {
  clog_async_msg_kind_t kind;
  union {
    clog_async_job_t job;    /* For kind == CLOG_ASYNC_MSG_JOB. It is on the
          heap, and the writer thread owns it and frees it; when the message
          never reached the queue at all, the fallback in _clog_write_async()
          for a failed enqueue frees it instead. The job sits BY VALUE inside
          this union, so there is no separate allocation for it, only one for
          the whole envelope */
    clog_async_ctrl_t *ctrl; /* For kind == CLOG_ASYNC_MSG_FLUSH. It points
        at the stack frame of the thread that ASKS, and since clog_flush()
        blocks until ctrl->done, the frame outlives the use of the pointer.
        The exit drain, whose wait is bounded, keeps its request on the heap
        instead (see _clog_exit_drain_async()). The writer thread never frees
        it */
  } u;
} clog_async_msg_t;

/* ========================================================================== */
/*                         RECORD BUILDING                                    */
/* ========================================================================== */

/*
 * This is a source of fields for a record, which is one of two things. The
 * first is the LIVE chmap of a logger, which the synchronous path and the
 * FATAL path use: they read the fields directly under fields_mutex at the
 * moment that they build the record. The second is an array of {key,value}
 * offsets into a flat pool of bytes, captured earlier, which the async path
 * uses: _snapshot_fields() captures those fields at submission time (see
 * the doc comment of that function for why the code stores offsets and not
 * raw pointers). One shared loop, _clog_emit_fields, drives both sources, so
 * the three primitives below, one for each format, run in exactly the same
 * way whichever mode made the key and value pairs.
 */
typedef struct {
  bool is_live;
  union {
    struct {
      chmap fields;
      ccol_mutex_t *mutex;
    } live;
    struct {
      const clog_field_view_t *views;
      size_t count;
      const char *pool;
      bool failed; /* true iff the snapshot itself could not be taken (an
          allocation failure in _snapshot_fields()), distinct from "this
          logger genuinely has zero fields" (count == 0, failed == false) */
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
 * Emits one field at a time from either field source above, picking the one
 * of the three append primitives that matches fmt. It returns false only
 * when it cannot list or emit the fields at all, for one of two causes: an
 * allocation failure, or an append failure in the middle of a field that
 * leaves the field list incomplete. *out_alloc_failure separates "a
 * transient allocation failure" from "the content of the record is too
 * large", a separation that the diagnostic message of
 * _clog_build_fallback_record() needs.
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
    /* chmap_elem_count() reads the internal element count of lg->fields and
     * takes no lock of its own, because chashmap is a container that this
     * codebase synchronises from the outside. The empty-map check below is
     * needed because chashmap_begin_iter() returns NULL both for "empty" and
     * for "the allocation of the iterator failed", and that check must run
     * under the same mutex as every other access to lg->fields, not before
     * the code locks it. Otherwise it races a concurrent call to
     * clog_set_field(), clog_remove_field() or clog_clear_fields() on the
     * same handle from another thread, which is exactly the kind of access
     * that this lock exists to serialise. */
    ccol_mutex_lock(*src->u.live.mutex);
    if (chmap_elem_count(fields) == 0) {
      ccol_mutex_unlock(*src->u.live.mutex);
      return true;
    }
    cmap_iterator *it = chashmap_begin_iter(fields, NULL);
    bool ok = true;
    /* A NULL iterator here means that its own allocation failed, not that the
     * field map is empty, because the check above already handles that.
     * Report it with the alloc_failure flag instead of dropping every field
     * without a trace. */
    if (!it) {
      *out_alloc_failure = true;
      ok = false;
    } else {
      while (it) {
        const char *k = (const char *)it->key_pair->ptr;
        const char *v = (const char *)it->val_pair->ptr;
        size_t field_start = out->len;
        if (append_fn(out, k, v) != 0) {
          out->len = field_start; /* roll back partial field */
          ccol_iter_destroy(it);
          ok = false;
          break;
        }
        it = it->_next_fn(it);
      }
    }
    ccol_mutex_unlock(*src->u.live.mutex);
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
 * Appends ts, level, proc, src and func in the shape that each format uses.
 * For JSON and for logfmt it appends exactly those five fields and leaves
 * nothing open. For syslog it also appends the RFC 5424 preamble, which
 * holds PRI, TIMESTAMP, HOSTNAME, APP-NAME, PROCID and MSGID, and the
 * opening of the "[ccol proc=... src=... func=..." structured data block.
 * That bracket stays open on purpose, because the field loop appends more
 * key="value" params into it, and _clog_build_msg_and_close() below closes
 * it. This function does no I/O at all. Precondition: the caller holds
 * sh->mutex.
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
       * the code then moves it to a heap buffer instead of appending the value
       * raw and unescaped, so the value still goes through the logfmt
       * quoting and escaping of _buf_append_lv(), like every other dynamic
       * field here. */
      char src_stack[512];
      char *src_val = src_stack;
      char *src_heap = NULL;
      int slen = snprintf(src_stack, sizeof(src_stack), "%s:%d", file, line);
      if (slen < 0) {
        ok = false;
      } else if ((size_t)slen >= sizeof(src_stack)) {
        src_heap = _ccol_mem_alloc(sh->m_procs, (size_t)slen + 1);
        if (!src_heap) {
          /* This is a genuine allocator failure, not a size rejection of the
           * CLOG_BUF_MAX kind. Flag it in the same way as _buf_ensure() flags
           * one, so that the later out->oom check of _clog_build_record()
           * reports the real cause instead of always blaming the size of the
           * record. */
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
 * with_backtrace is true, the inline JSON backtrace array. This function
 * does not handle the backtrace frames of logfmt and syslog: the caller
 * appends or writes those strictly after the write of the primary record,
 * with _emit_backtrace_lines() for logfmt and _emit_backtrace_syslog_lines()
 * for syslog. Only JSON puts its backtrace inline in the record that this
 * function closes. The caller calls this function only after it appends the
 * fields, so no write() call ever splits the structured data. Precondition:
 * the caller holds sh->mutex.
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
 * The caller calls this function after the ordinary sequence that builds a
 * record fails, which has two separate causes: the content of the record can
 * exceed the cap_limit of the target buffer (as an oversized message or a
 * very large set of field values does), or a transient allocation failure,
 * which has nothing to do with the size, can stop the code from completing
 * the record safely. `alloc_failure` separates the two, so that the note
 * below reports the real cause instead of always blaming the size.
 *
 * The build starts from the current value of out->len, which the caller has
 * already rolled back to the point where this record started. This function
 * does not reset out to 0 itself, because out can be a batch buffer that
 * already holds several earlier records from other jobs, such as the
 * async_buf of the writer thread, and a reset here would destroy all of
 * those records as soon as one later job in the same batch needs the
 * fallback. The two callers that build a single record (the synchronous and
 * FATAL write path, and the fallback for a failed async enqueue) each reset
 * their own buffer to empty exactly once, before they build their one
 * record, so for them this placeholder is the only content of the buffer.
 *
 * `with_backtrace` matters only for JSON. Logfmt and syslog emit backtrace
 * frames whether or not the primary record fell back to this placeholder,
 * so a backtrace that the caller asks for is never lost for those two
 * formats. JSON instead puts its backtrace inline in the record that this
 * function replaces, so when with_backtrace is true, this function appends
 * the same _BT_JSON_ERROR_MARKER that the rest of this file uses, and a
 * reader can still tell "backtrace omitted" apart from "backtrace never
 * requested".
 *
 * It returns true when it appends this fixed-size placeholder to `out` in
 * full, and false when even the placeholder does not fit in the room that
 * `out` has left. That is reachable only when `out` is the shared
 * aggregation buffer of the async writer thread and that buffer already sits
 * right against its own cap_limit. In that case it leaves `out` completely
 * untouched, rolled back to its length on entry, so that the caller can
 * flush what is already safely in `out` and try again against an empty
 * buffer. The library never writes a truncated or malformed record.
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
    /* This message is generic on purpose, because any genuine allocator
     * failure during the build of this record raises alloc_failure: the
     * header, the value of a field, the message, the field snapshot and the
     * iterator can each raise it, not only a failure to list the fields. See
     * the out->oom check of _clog_build_record(). */
    snprintf(note, sizeof(note),
             "log record dropped: a transient allocation failure prevented "
             "it from being fully built");
  } else {
    /* This uses out->cap_limit and not the fixed CLOG_BUF_MAX constant,
     * because a clog_async_cfg_t.flush_buffer_size that the caller sets above
     * CLOG_BUF_MAX raises the limit of the async batch buffer above the
     * default (see the doc comment of clog_buf_t.cap_limit). This diagnostic
     * must report the limit that is in effect for THIS buffer, not the
     * single-record default that every other buffer in this file uses. */
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
 * This is the shared sequence that builds a record. It drives
 * _clog_build_header(), _clog_emit_fields() and _clog_build_msg_and_close()
 * into out, and on any failure falls back to _clog_build_fallback_record().
 * Two paths use it: the synchronous and FATAL write path passes the lg->buf
 * of a handle as out, with field_src in "live" mode, and the per-job work of
 * the async writer thread passes sh->async_buf as out, with field_src in
 * "snap" mode.
 *
 * For logfmt, this function appends the backtrace frames that the caller
 * asks for into the same out buffer, directly after the primary record, so
 * they land in the same write() or flush as the record itself. JSON has
 * already put its own backtrace inline through _clog_build_msg_and_close().
 * This function NEVER appends the backtrace frames of syslog, because those
 * frames are exempt from the batch; the caller must call
 * _emit_backtrace_syslog_lines() itself, strictly after it writes out.
 * Precondition: the caller holds sh->mutex.
 *
 * It returns true after it appends a full record to `out`, either the real
 * one or the fallback placeholder. It returns false only in the narrow case
 * where even the fixed-size fallback placeholder does not fit in the room
 * that `out` has left (see the doc comment of
 * _clog_build_fallback_record()), and it then leaves `out` completely
 * untouched: the caller must flush or empty `out` and call this function
 * again, and must never treat `out` as a buffer that holds a usable record.
 *
 * The code sets out_used_fallback to true whenever it substitutes the
 * fallback placeholder for the real record, whether or not the substitution
 * itself succeeds, and to false whenever it builds the real record as it is.
 * out_used_fallback can be NULL when the caller does not need it.
 *
 * A caller that builds into a batch buffer needs this flag, because that
 * buffer, such as the sh->async_buf of the async writer thread, can already
 * hold OTHER, unrelated records. A true here does NOT always mean that the
 * record itself is too large: the record can also be small and fail to fit
 * in the room that earlier content in the same buffer leaves. The caller
 * should then flush that earlier content and build this record again from
 * an empty buffer, because the library must not silently replace a small,
 * ordinary record with a "too large" note.
 *
 * The two callers that build a single record (the synchronous and FATAL
 * write path, and the fallback for a failed async enqueue) always build into
 * a buffer that they reset to empty directly before the call, so for them
 * out_used_fallback can only be true for a record that is genuinely
 * oversized. Both pass NULL.
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
  /* The code resets this flag before this attempt, because an EARLIER,
   * unrelated build against this same buffer, which can live for a long time
   * and serve many builds, can have flagged a real allocator failure.
   * Without the reset, that old flag leaks into the alloc_failure verdict of
   * this record below. */
  out->oom = false;
  bool ok =
      _clog_build_header(out, sh, fmt, level, ts, proc_val, file, line, func);
  bool alloc_failure = false;
  if (ok) ok = _clog_emit_fields(out, fmt, field_src, &alloc_failure);
  if (ok)
    ok = _clog_build_msg_and_close(out, fmt, msg, with_backtrace, bt_syms,
                                   bt_depth);
  /* _clog_emit_fields() reports alloc_failure only for a field snapshot or
   * iterator that it cannot get at all. Two other failures, a failure to
   * build the header and a failure of one field append in the middle of the
   * loop, go through _buf_ensure() instead, which flags out->oom directly on
   * a genuine allocator failure and never when the code only reaches
   * out->cap_limit. The code folds out->oom in here, after every build step
   * has had its chance to set it, which keeps the diagnostic of
   * _clog_build_fallback_record() accurate whichever of the three build
   * steps ran out of memory, and whichever append inside that step did
   * so. */
  if (!alloc_failure) alloc_failure = out->oom;

  /* For LOGFMT, the code appends the backtrace frames as trailing
   * continuation lines into this SAME buffer, directly after the primary
   * record that the step above closed. The other two formats differ: JSON
   * puts its backtrace INSIDE the record that _clog_build_msg_and_close()
   * closed, so a backtrace that is too large to embed has already made `ok`
   * false through that call, and the frames of syslog are always separate
   * records that the caller writes on its own, which never reach this
   * function.
   *
   * The code tries this only while the primary record is still intact, which
   * is what `ok` reports. If the primary content has already failed for its
   * own, unrelated reasons, the generic fallback placeholder below replaces
   * it in any case, and the backtrace then gets its own, separate attempt
   * after THAT placeholder (see below), because an attempt here would run
   * against a buffer state that the code is about to discard. */
  bool logfmt_bt = with_backtrace && fmt == CLOG_FMT_LOGFMT;
  if (ok && logfmt_bt && !_emit_backtrace_lines(out, bt_syms, bt_depth)) {
    /* Not one frame, not even the small fixed-size "unavailable" marker, fits
     * in the room that is left after the primary record (see the doc comment
     * of _emit_backtrace_lines()). This is reachable only when `out` sits
     * within a few dozen bytes of its own cap_limit. The primary record is
     * fine, but a record that asks for a backtrace must never end up without
     * one and with no trace anywhere, which this function already guarantees
     * for every OTHER failure mode, so the code forces the same
     * fallback-record recovery that every other build failure gets.
     * `out->oom` shows whether this failure was a genuine allocator failure
     * or only reached the capacity cap, in the same way as for every other
     * build step above, so the diagnostic of the placeholder stays
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
      /* A primary record that is oversized, or that failed to build for
       * another reason, must not also cost the caller its backtrace, so the
       * code tries again, this time appending the backtrace directly after
       * the small fallback placeholder that replaced the primary content,
       * where the buffer has much more room than before. One more failure
       * here means that not even the marker fits after a placeholder that
       * did fit, which the library accepts as a small gap that is not
       * reachable in practice; the doc comment of
       * _clog_write_unrepresentable_record() accepts the same class of gap
       * for the size of the fallback placeholder. */
      (void)_emit_backtrace_lines(out, bt_syms, bt_depth);
    }
  }
  return true;
}

/*
 * This is the last resort for the one case for which _clog_build_record()
 * has no further fallback: a small, fixed-size placeholder that the code
 * cannot append to `out`.
 *
 * For a single-record buffer this is not reachable in practice. Both callers
 * of this function below use lg->buf, which is such a buffer: the code
 * always resets lg->buf to empty directly before the build, and its capacity
 * can never drop below CLOG_BUF_INITIAL, which is much larger than any
 * fallback record that this file builds, so _clog_build_fallback_record()
 * never needs to grow the buffer here, and it cannot fail.
 *
 * This function exists so that a future change, such as a smaller
 * CLOG_BUF_INITIAL or a buffer of another size reused for this purpose,
 * cannot turn that guarantee into a silent, zero-byte record loss with no
 * trace anywhere. The function writes a minimal line DIRECTLY to the fd,
 * which is still correct for the format and ends with a newline. It builds
 * the line in a fixed-size stack buffer, and snprintf never allocates, so
 * it skips the clog_buf_t and heap allocation machinery completely and
 * cannot fail in the same way. Precondition: the caller holds sh->mutex,
 * and sh->fd >= 0.
 *
 * with_backtrace keeps the same guarantee that _clog_build_record(),
 * _emit_backtrace_lines() and _emit_backtrace_json() give an ordinary
 * record: a caller that asks for a backtrace must never see one vanish
 * without a trace.
 *
 * CLOG_FMT_SYSLOG needs no handling here, because every caller of this
 * function emits the backtrace lines of syslog itself, with a separate call
 * to _emit_backtrace_syslog_lines() right next to its own call into this
 * function, whatever happens here. CLOG_FMT_JSON puts its own "bt_error"
 * marker directly into the one JSON line that the code builds below, which
 * matches the with_backtrace handling of _clog_build_fallback_record() for
 * that format. CLOG_FMT_LOGFMT is the one format whose backtrace lines
 * normally go into the SAME buffer as the primary record, through
 * _emit_backtrace_lines(), but that route is not reachable once the code
 * reaches this function, because reaching it means that not even the small
 * fallback placeholder fit in that buffer. So logfmt gets its own marker
 * line, which needs no allocation, directly after the primary line.
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
 * target of logger, for a test. This follows the same pattern as
 * clog_test_gzip_compress_file(), which exposes an internal helper that is
 * hard to reach instead of attempting to build real conditions that reach
 * it. To reach this function through the ordinary log_* call path, not even
 * the small fallback placeholder of _clog_build_record() may fit in the
 * target buffer, and the doc comment of that function explains why the
 * current value of CLOG_BUF_INITIAL makes that impossible. So a test that
 * wants to exercise the behaviour of this function on its own needs a
 * direct call like this one, without having to break that invariant for
 * real.
 */
void clog_test_write_unrepresentable_record(clog logger, clog_format_t fmt,
                                            clog_level_t level,
                                            bool with_backtrace) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return;
  ccol_mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    _clog_write_unrepresentable_record(lg->shared, fmt, level, with_backtrace);
  ccol_mutex_unlock(lg->shared->mutex);
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
 * Frees a clog_shared_t that has no live mutex and no logger children. It is
 * safe to call at any point during the initialization inside
 * _shared_alloc() after the code sets sh->m_procs, which happens directly
 * after the allocation of sh.
 *
 * This function never touches sh->fd itself, whatever the value of
 * sh->owns_fd, because the early failure paths of _shared_alloc() also reach
 * it, and in that case the caller of _shared_alloc() (_alloc(), and above it
 * clog_open_fd_mp() or clog_open_file_mp()) already owns the close of the fd
 * on ITS OWN failure branch; a close here as well would close the fd twice.
 * Every other caller that owns the fd and reaches this function after that
 * point must close it first, with _shared_close_owned_fd() below.
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
 * Closes sh->fd when this shared object owns it and the fd is still open,
 * and then marks the fd closed. It also closes sh->dir_fd, the directory
 * descriptor of a rotating logger, which the shared object always owns. A
 * file-backed logger from clog_open_file_mp() owns its fd. This function does
 * nothing for an fd that the caller owns, and nothing for an fd that is
 * already closed; clog_open_fd_mp() creates the first case, where owns_fd is
 * false and the caller alone owns the life of the fd.
 *
 * The teardown of clog_close() uses this function, and so does every
 * constructor rollback path, which must not leak the fd that it just opened
 * when a LATER construction step fails (such as the setup of async logging
 * or the acquisition of a handle-table slot). _shared_free_partial() itself
 * never does this; see its own doc comment.
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
  /* The caller keeps ownership of dir_fd until this function succeeds, like
   * fd; see _shared_free_partial(). */
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

  if (ccol_mutex_init(sh->mutex) != 0) {
    _shared_free_partial(sh);
    return NULL;
  }

  sh->dir_fd = dir_fd;
  sh->last_rotation = time(NULL);

  sh->syslog_facility = CLOG_SYSLOG_USER; /* calloc zeroes to KERN; override */

  /* This caches HOSTNAME for RFC 5424, sanitized in the same way as APP-NAME
   * below (see the doc comment of _sanitize_syslog_printusascii_field()). The
   * kernel does not guarantee that the raw value from gethostname(2) holds only
   * PRINTUSASCII bytes. Without this step, a space, a control character or a
   * newline inside it would reach the wire completely unescaped, although every
   * other dynamic field in a syslog record is escaped, and a hostname that is
   * wrong or hostile would then split one record into two. */
  char raw_hostname[256];
  if (gethostname(raw_hostname, sizeof(raw_hostname)) != 0)
    raw_hostname[0] = '\0';
  raw_hostname[sizeof(raw_hostname) - 1] = '\0';
  _sanitize_syslog_printusascii_field(raw_hostname, sh->syslog_hostname,
                                      sizeof(sh->syslog_hostname));

  /* This caches APP-NAME for RFC 5424. See the doc comment of
   * _sanitize_syslog_printusascii_field() for the exact filter rule. */
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

  if (ccol_mutex_init(lg->fields_mutex) != 0) {
    __chmap_destroy(lg->fields);
    _buf_free(&lg->buf);
    _ccol_mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->shared = shared;
  lg->min_level = min_level;
  return lg;
}

/* Free a fully-initialised logger without touching the shared backing store. */
static void _logger_free(struct clogger *lg) {
  ccol_mutex_destroy(lg->fields_mutex);
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
    /* The caller still owns fd and dir_fd on this failure, as it does on a
     * failure of _shared_alloc(). */
    ccol_mutex_destroy(sh->mutex);
    _shared_free_partial(sh);
    return NULL;
  }

  return lg;
}

/* ========================================================================== */
/*                         ASYNC LOGGING                                      */
/* ========================================================================== */

/*
 * Copies lg->fields as of this exact call into out_fields, out_count and
 * out_pool, under lg->fields_mutex, so a later change or removal on lg can
 * never change the fields that the library has already recorded for a
 * submitted job. The code sets *out_failed to true only on a genuine
 * allocation failure, and leaves *out_fields NULL in that case. A logger
 * with zero fields succeeds with *out_count == 0.
 */
static void _snapshot_fields(struct clogger *lg, clog_field_view_t **out_fields,
                             size_t *out_count, ccol_growbuf_t *out_pool,
                             bool *out_failed) {
  *out_fields = NULL;
  *out_count = 0;
  *out_failed = false;

  ccol_mutex_lock(lg->fields_mutex);
  size_t n = chmap_elem_count(lg->fields);
  if (n == 0) {
    ccol_mutex_unlock(lg->fields_mutex);
    /* ccol_growbuf_init() always allocates a 256-byte backing store, and the
     * common case is a logger with no persistent fields at all, so this
     * branch skips that allocation for it; without the skip, every async log
     * call pays for that allocation only to find that there is nothing to
     * copy. The code builds the same "valid, empty, nothing appended" shape
     * directly instead. _clog_async_job_release() always calls
     * ccol_growbuf_destroy() later, and that function is documented as safe
     * on exactly this shape, where b->buf can be NULL.
     *
     * This is more than a speed-up of the success path: a call to
     * ccol_growbuf_init() here can fail under real memory pressure and leave
     * out_pool->oom set, and since this n == 0 branch returns before it can
     * check that flag, the failure would have no way to reach *out_failed.
     * The direct build of the empty shape has no such failure mode. */
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
    ccol_mutex_unlock(lg->fields_mutex);
    *out_failed = true;
    return;
  }

  cmap_iterator *it = chashmap_begin_iter(lg->fields, NULL);
  if (!it) {
    ccol_mutex_unlock(lg->fields_mutex);
    _ccol_mem_free(lg->shared->m_procs, views);
    *out_failed = true;
    return;
  }

  size_t i = 0;
  while (it && i < n) {
    const char *k = (const char *)it->key_pair->ptr;
    const char *v = (const char *)it->val_pair->ptr;
    views[i].key_offset = out_pool->len;
    ccol_growbuf_append_cstr(out_pool, k);
    ccol_growbuf_append_c(out_pool, '\0');
    views[i].value_offset = out_pool->len;
    ccol_growbuf_append_cstr(out_pool, v);
    ccol_growbuf_append_c(out_pool, '\0');
    i++;
    it = it->_next_fn(it);
  }
  /* This call is a defence that carries no load in normal operation:
   * chmap_elem_count() and the traversal of chashmap_begin_iter() always
   * agree on the number of live entries while the code holds fields_mutex
   * for the whole walk, so `it` is already NULL when i == n stops the loop
   * above, because its own _next_fn destroys it on the last entry. The call
   * exists so that a break of that invariant leaks nothing; without it, such
   * a break leaks the internal allocation of cmap_iterator on every async
   * submission that carries fields, and this function runs on the hot path
   * of clogger. */
  if (it) ccol_iter_destroy(it);
  ccol_mutex_unlock(lg->fields_mutex);

  if (out_pool->oom) {
    /* The code leaves out_pool, which is job->field_pool, alone here and must
     * not destroy it. The only caller of this function, _clog_write_async(),
     * always routes the job through _clog_async_job_release() on every
     * success path and every failure path, and _clog_async_job_release()
     * always destroys job->field_pool exactly once, so a destroy here as well
     * is a double free: ccol_growbuf_destroy() frees out_pool->buf and never
     * sets the pointer to NULL after that, so the later destroy in
     * _clog_async_job_release() would free the same block a second time. The
     * two other early return paths above (the allocation failure of views and
     * the allocation failure of the iterator) leave out_pool alone for the
     * same reason. */
    _ccol_mem_free(lg->shared->m_procs, views);
    *out_failed = true;
    return;
  }

  *out_fields = views;
  *out_count = n;
}

/* Gives true for a write error that a later attempt on the same descriptor
 * can outlast: the resource is exhausted for now (ENOSPC, EDQUOT, EFBIG from
 * an RLIMIT_FSIZE ceiling, ENOBUFS, ENOMEM), or the write only had to wait
 * (EAGAIN or EINTR after a bounded wait ran out). 0 stands for a write that
 * stopped without an error, such as one that made no progress. Every other
 * error, such as EPIPE, ECONNRESET, EBADF or EMSGSIZE, stays the same on
 * every retry. */
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

/* Gives where the first `written` bytes of data end, as a value of
 * clog_shared_t.async_partial_record. The caller guarantees
 * 0 < written < len, so data[written] exists. */
static unsigned char _clog_split_after(const char *data, size_t written) {
  if (data[written - 1] != '\n') return CLOG_SPLIT_MID_LINE;
  return data[written] == '\t' ? CLOG_SPLIT_AT_LINE : CLOG_SPLIT_NONE;
}

/*
 * Flushes sh->async_buf to disk when it holds anything, with the same
 * rotation checks as the synchronous write path: a time-based check before
 * the write, and a size-based check after it. The code hands
 * poll_timeout_ms straight to _write_all(); see that function for which
 * callers bound it and why.
 *
 * Precondition: the caller holds sh->mutex, and calls this only for a
 * target that is not syslog, because a syslog job flushes itself at once and
 * never accumulates into async_buf; see _clog_writer_thread_main().
 *
 * This function never touches sh->last_flush_monotonic. That field belongs
 * to the flush schedule of the writer thread, which reads it outside
 * sh->mutex and so is the only thread that may write it, while this
 * function runs on any thread that must settle a half-written record before
 * it writes (see _clog_flush_and_settle()). _writer_flush_now() is the entry
 * point of the writer thread, and it advances the field there.
 *
 * The kernel can accept only a part of a write, as a filesystem that is full
 * with ENOSPC, or an RLIMIT_FSIZE ceiling, makes it do. When the error that
 * stopped the write is one that a later attempt can outlast (see
 * _clog_write_error_is_transient()), the bytes that the kernel did not
 * accept stay in this buffer, moved to the front, and the next flush tries
 * them again. Those bytes are the exact continuation of what landed on
 * disk, so offering them again restores the framing of the record that the
 * short write cut in half, and also delivers every record behind that one
 * once the condition clears, whereas discarding them would lose a whole
 * batch at the moment when a visible log matters most.
 *
 * Any other error is one that no retry outlasts: a peer that is gone, a
 * descriptor that is not valid, a message that the sink can never take.
 * Offering the batch again would then fail again, while every later record
 * piles up behind it until the buffer reaches its ceiling, so the flush
 * drops the undelivered bytes and writes a marker that names the loss (see
 * _clog_note_dropped_batch()), which is what the synchronous path does for
 * a record that it cannot deliver.
 *
 * What the buffer keeps can never exceed its own growth ceiling,
 * clog_buf_t.cap_limit, because the kept bytes are bytes that the buffer
 * already holds. A record that does not fit behind them takes the same path
 * as any other record that is too large for the batch buffer, which reports
 * itself instead of letting the record vanish.
 *
 * Because the retry is what restores the framing, a kept remainder gets no
 * truncation marker. That holds only while the two halves reach the same
 * file with nothing between them, so a short write that stops inside a
 * record sets clog_shared_t.async_partial_record: both rotation checks then
 * stand down, and any path that is about to write a different record
 * straight to the fd settles the continuation first (see
 * _clog_flush_and_settle()). The flush that finally drains the buffer clears
 * the flag again.
 *
 * In every other case the code resets the buffer whenever it holds
 * anything, whether or not sh->fd was valid at that time. An sh->fd below 0
 * is not observable here in practice, because the library closes the fd
 * only after it joins the writer thread of this shared target (see the
 * order in _shared_async_teardown() and clog_close()). Unlike the write and
 * rotation check block below it, that reset does not test sh->fd >= 0: if a
 * future change ever breaks that invariant, the safe failure mode is to drop
 * what is pending, and the unsafe one is an unbounded amount of content that
 * the library can never flush and that stays in this buffer forever.
 */
static void _writer_flush_buffer(clog_shared_t *sh, int poll_timeout_ms) {
  if (sh->async_buf.len > 0) {
    bool retained = false;
    if (sh->fd >= 0) {
      _clog_time_rotate_if_due(sh);

      size_t len = sh->async_buf.len;
      errno = 0;
      size_t written = _write_all(sh, sh->async_buf.data, len, poll_timeout_ms);
      int write_err = errno;
      if (sh->rotation_enabled) sh->bytes_written += (off_t)written;

      if (written < len) {
        /* The bytes that landed decide whether anything is split at all, so
         * the code reads them here, before it moves the remainder over them.
         * A prefix that ends on a record boundary leaves whole records
         * behind and splits nothing, so both rotation checks are free to
         * run. A prefix that ends anywhere else is the first part of a
         * record whose remaining bytes are still in this buffer, so both
         * checks must stand down until the retry delivers them; see
         * clog_shared_t.async_partial_record. A write that the kernel
         * accepted none of moves nothing: the whole batch is still here
         * exactly as it was, so the flag keeps the value that an earlier
         * short write gave it. */
        unsigned char split =
            written > 0 ? _clog_split_after(sh->async_buf.data, written)
                        : sh->async_partial_record;
        if (_clog_write_error_is_transient(write_err)) {
          sh->async_partial_record = split;
          memmove(sh->async_buf.data, sh->async_buf.data + written,
                  len - written);
          sh->async_buf.len = len - written;
          retained = true;
        } else {
          char why[48];
          snprintf(why, sizeof(why), "write error %d", write_err);
          _clog_note_dropped_batch(sh, sh->async_buf.data + written,
                                   len - written, split, why, poll_timeout_ms);
        }
      }
    }
    if (!retained) {
      /* Nothing is left undelivered here: either the write took all of it, or
       * the rest can never go out and the marker above names it, or there
       * was no usable fd to offer it to. So no record waits for a
       * continuation that is still on its way. */
      _buf_reset(&sh->async_buf);
      sh->async_partial_record = CLOG_SPLIT_NONE;
    }
    if (sh->fd >= 0) _clog_size_rotate_if_due(sh);
  }
}

/*
 * This is the flush of the writer thread: _writer_flush_buffer() plus the
 * timer bookkeeping that the main loop of that thread needs. The code
 * advances the timestamp even when there was nothing to write, because that
 * loop computes its next timed-receive timeout from (now -
 * last_flush_monotonic) on every iteration; without the advance, an idle
 * logger computes a remaining time near zero forever after an empty timeout
 * and spins instead of really sleeping for flush_interval_us.
 *
 * Precondition: the caller holds sh->mutex. Only the writer thread calls
 * this, because it is the only thread that may write
 * sh->last_flush_monotonic, which it reads outside the mutex.
 */
static void _writer_flush_now(clog_shared_t *sh, int poll_timeout_ms) {
  _writer_flush_buffer(sh, poll_timeout_ms);
  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);
}

/*
 * Gives up on the continuation of a record whose first part is already on
 * disk, and repairs the framing that the first part left open. One write
 * carries both the missing newline, when a line is open, and a marker that
 * names the bytes that the library gave up on, so a consumer that reads the
 * file line by line sees the truncated record and the marker as well-formed
 * lines, and whatever the library writes next starts a line of its own.
 *
 * The library gives up on the continuation only. Whole records that sit in
 * the batch behind it stay in the buffer, and a later flush delivers them:
 * nothing about them is malformed, and what makes them unwritable right now
 * is the descriptor, not the framing.
 *
 * Precondition: the caller holds sh->mutex, sh->fd >= 0, and
 * sh->async_partial_record is set, which means that the leading bytes of
 * async_buf are that continuation and that what is on disk stops inside a
 * record. A flush with no usable descriptor leaves nothing outstanding, so
 * the set flag is itself the proof that the descriptor is usable.
 */
static void _clog_repair_split_record(clog_shared_t *sh, clog_format_t fmt,
                                      int poll_timeout_ms) {
  size_t len = sh->async_buf.len;
  const char *data = sh->async_buf.data;
  /* The continuation ends at the newline of the record's last line. Every
   * line that starts with a tab is a continuation line of the record before
   * it (a logfmt backtrace frame), so the continuation runs on through each
   * such line. A remainder that holds no further newline is the record's
   * tail and nothing else, so all of it is the continuation. */
  size_t dropped = 0;
  do {
    const char *nl = memchr(data + dropped, '\n', len - dropped);
    dropped = nl ? (size_t)(nl - data) + 1 : len;
  } while (dropped < len && data[dropped] == '\t');

  _clog_note_dropped_continuation(sh, fmt, sh->async_partial_record, dropped,
                                  poll_timeout_ms);

  sh->async_buf.len = len - dropped;
  if (sh->async_buf.len > 0)
    memmove(sh->async_buf.data, sh->async_buf.data + dropped,
            sh->async_buf.len);
  else
    _buf_reset(&sh->async_buf);
  sh->async_partial_record = CLOG_SPLIT_NONE;
}

/*
 * Gives up on every record that async_buf still holds after the last
 * delivery attempt that this target makes, and writes a marker that names
 * the loss. A flush keeps the undelivered part of a batch after an error
 * that a later attempt can outlast (see _writer_flush_buffer()), but when no
 * later attempt follows, because the target closes or the process stops,
 * those records would otherwise vanish with the buffer with nothing to name
 * them. The caller has already settled any split record, so the bytes on
 * disk end on a record boundary.
 *
 * The caller must hold sh->mutex.
 */
static __attribute__((noinline, cold)) void _clog_drop_undelivered(
    clog_shared_t *sh, const char *why, int poll_timeout_ms) {
  if (sh->async_buf.len == 0) return;
  if (sh->fd >= 0)
    _clog_note_dropped_batch(sh, sh->async_buf.data, sh->async_buf.len,
                             CLOG_SPLIT_NONE, why, poll_timeout_ms);
  _buf_reset(&sh->async_buf);
  sh->async_partial_record = CLOG_SPLIT_NONE;
}

/*
 * Repairs the framing when the flush that ran before it leaves a record
 * split across the write that follows. Every path that writes a record
 * STRAIGHT to sh->fd runs this after its own flush, because the buffer of
 * the async writer thread can hold the continuation of a half-written
 * record; without it, that record lands between the two halves of another
 * record, and a consumer of any of the three line-framed formats of this
 * module then reads three malformed lines where there should be two
 * well-formed ones.
 *
 * The flush ahead of it is the only chance that the continuation gets,
 * because a descriptor that just refused the remainder of a record does not
 * accept it a moment later. Two paths reach this function, an ordinary log
 * call and CLOG_FATAL on its way to exit(), and neither may wait on a
 * descriptor that never accepts it.
 *
 * The caller must hold sh->mutex.
 */
static void _clog_settle_after_flush(clog_shared_t *sh, clog_format_t fmt,
                                     int poll_timeout_ms) {
  if (sh->async_partial_record)
    _clog_repair_split_record(sh, fmt, poll_timeout_ms);
}

/*
 * This is _clog_settle_after_flush() plus the delivery attempt that it
 * settles after. Two write paths need it, because they do not belong to the
 * writer thread and so have no flush of their own to run first.
 * poll_timeout_ms bounds the one wait inside that attempt that can block on
 * a descriptor with a slow peer; see _write_all().
 *
 * This function is out of line on purpose. The check that reaches it is one
 * byte of a cache line that every write path already reads (see
 * clog_shared_t.async_partial_record), while the recovery itself stays out
 * of the caller, so the code shape of the ordinary log path does not change.
 *
 * The caller must hold sh->mutex.
 */
static __attribute__((noinline)) void _clog_flush_and_settle(
    clog_shared_t *sh, clog_format_t fmt, int poll_timeout_ms) {
  _writer_flush_buffer(sh, poll_timeout_ms);
  _clog_settle_after_flush(sh, fmt, poll_timeout_ms);
}

/* Frees everything that a CLOG_ASYNC_MSG_JOB envelope owns, including the
 * envelope itself. Both the ordinary per-job work of the writer thread and
 * the fallback of _clog_write_async() for a failed enqueue use it. */
static void _clog_async_job_release(clog_shared_t *sh,
                                    clog_async_msg_t *envelope) {
  clog_async_job_t *job = &envelope->u.job;
  if (job->bt_syms)
    free(job->bt_syms); /* This block comes from the malloc inside
backtrace_symbols() and never goes through _ccol_mem_free or a custom
allocator, as with every other free of backtrace symbols in this
file */
  ccol_growbuf_destroy(&job->field_pool);
  _ccol_mem_free(sh->m_procs, job->fields);
  _ccol_mem_free(sh->m_procs, job->msg_heap);
  _ccol_mem_free(sh->m_procs, envelope); /* This frees the storage of job too,
             because the envelope holds job by value, and the library never
             allocates job on its own */
}

#ifdef RUNNING_UNIT_TESTS
static _Atomic unsigned int _clog_test_writer_job_delay_us = 0;

void clog_test_set_writer_job_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_writer_job_delay_us, delay_us);
}

/* Holds the writer thread for delay_us. A test that compares a short hold
 * with a bound needs the hold to end on time, and a sleep of a busy runner
 * can end a hundred milliseconds or more late, so a hold of up to 50 ms
 * waits on the clock. A longer one stands for a slow output, where lateness
 * does no harm, and sleeps. */
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

/* Waits until no CLOG_FATAL call waits for the mutex of sh. The fatal call
 * lowers the count as soon as it holds the mutex or gives up on it, which
 * its own bounded wait ends in time; since nothing else here signals, the
 * wait polls the count, and it touches no lock while it does. */
static __attribute__((noinline, cold)) void _clog_writer_yield_to_fatal(
    clog_shared_t *sh) {
  while (atomic_load(&sh->fatal_waiters) != 0) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 200000L};
    nanosleep(&pause, NULL);
  }
}

/* The lock of the mutex of sh for each piece of work of the writer thread.
 * A CLOG_FATAL call must get that mutex for its record, but a mutex hands
 * itself to the thread that asks again first: a writer thread with a
 * backlog unlocks and locks again at once, so a fatal call that waits would
 * see the mutex free only by chance. The writer thread therefore steps
 * aside while a fatal call waits. */
static inline void _clog_writer_lock(clog_shared_t *sh) {
  if (__builtin_expect(
          atomic_load_explicit(&sh->fatal_waiters, memory_order_relaxed) != 0,
          0))
    _clog_writer_yield_to_fatal(sh);
  ccol_mutex_lock(sh->mutex);
}

static void *_clog_writer_thread_main(void *arg) {
  clog_shared_t *sh = (clog_shared_t *)arg;
  _clog_thread_serves = sh;
  _clog_writer_sigpipe_init();

  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long elapsed_us =
        (long long)(now.tv_sec - sh->last_flush_monotonic.tv_sec) * 1000000LL +
        (now.tv_nsec - sh->last_flush_monotonic.tv_nsec) / 1000L;
    /* The arithmetic stays unsigned and never narrows the interval to a
     * signed type: flush_interval_us is a uint64_t, and a value above
     * LLONG_MAX, such as UINT64_MAX for "flush on size only", would convert
     * to a negative long long, which makes every wait a zero wait so that the
     * thread spins on an idle logger. The remaining time is clamped to 0,
     * because the interval can already be over when this iteration starts
     * (for example after a slow job that took longer than the whole
     * interval), and that case must mean "check right now". The receive call
     * saturates the deadline that it builds from a very long remaining time,
     * so a wait that long is a wait for ever in practice. */
    uint64_t interval_us = sh->async_cfg.flush_interval_us;
    uint64_t remaining_us = elapsed_us <= 0 ? interval_us
                            : (uint64_t)elapsed_us >= interval_us
                                ? 0
                                : interval_us - (uint64_t)elapsed_us;

    c_message_t m = {0};
    ccol_retval_t rv =
        sh->is_bounded_queue
            ? ccol_circq_timed_recv_zc(sh->q.circq, &m, remaining_us)
            : ccol_dynmq_timed_recv_zc(sh->q.dynmq, &m, remaining_us);

    /* A remaining time of 0 is a receive that does not wait, and an empty
     * queue then answers ccol_container_empty, which is the same outcome as
     * a wait that timed out: the interval is over. */
    if (rv == ccol_timed_out || rv == ccol_container_empty) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&_clog_test_writer_timeout_wakeups, 1);
#endif
      _clog_writer_lock(sh);
      /* This is a plain flush, never a settle, because it is one of the retries
       * for which the buffer keeps a continuation, and nothing is about to be
       * written past it. Giving up on it here would throw away the delivery
       * that this wakeup exists to try. */
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      ccol_mutex_unlock(sh->mutex);
      continue;
    }
    if (rv != ccol_success) {
      /* This is a real, documented return value and not a theoretical one: the
       * header doc of ccol_circq_timed_recv_zc() and
       * ccol_dynmq_timed_recv_zc() says "ccol_unexpected_failure on system
       * error (check errno)".
       *
       * A plain loop back to the timed receive does NOT stop a tight spin on
       * its own, because nothing on this path touches
       * sh->last_flush_monotonic. An idle logger reaches a steady state in
       * which flush_interval_us has passed since the last real flush, so
       * `remaining_us` above clamps to 0 and stays 0 on every later
       * iteration. Without the sleep, a system error that persists spins
       * this thread at 100% CPU on one core forever, with no backoff.
       *
       * usleep() is not a pthread or sem primitive, so it needs no wrapper in
       * common.h, as with the other direct usleep() calls in this file. A
       * short, fixed sleep bounds the retry rate however long the condition
       * under it lasts. */
      usleep(1000);
      continue;
    }

    if (m.data == NULL && m.size == 0) {
      /* This is the shutdown sentinel; see _shared_async_teardown(). The
       * thread drains everything that is already built, then exits. The
       * library sends the sentinel only after it enqueues every earlier job
       * and flush request, so FIFO order guarantees that the thread processes
       * it only after everything ahead of it. */
      _clog_writer_lock(sh);
      /* This settles and does not only flush, because it is the last delivery
       * attempt that this target ever makes, and a continuation that is still
       * outstanding after it has no later retry to restore its framing.
       * Without the settle, the file ends in the middle of a record, and
       * nothing names the loss. Whole records that the flush kept for a retry
       * have no later retry either, so a marker names them. */
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      _clog_settle_after_flush(sh, sh->format, CLOG_WRITE_WAIT_FOREVER);
      _clog_drop_undelivered(sh, "the logger closed", CLOG_WRITE_WAIT_FOREVER);
      ccol_mutex_unlock(sh->mutex);
      return NULL;
    }

    clog_async_msg_t *envelope = (clog_async_msg_t *)m.data;

    if (envelope->kind == CLOG_ASYNC_MSG_FLUSH) {
      clog_async_ctrl_t *ctrl = envelope->u.ctrl;
      _clog_writer_lock(sh);
      /* This is a plain flush, for the same reason as the timeout branch above:
       * an explicit flush is a delivery attempt, not a deadline. */
      _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      ccol_mutex_unlock(sh->mutex);
      ccol_mutex_lock(ctrl->mutex);
      ctrl->done = true;
      ccol_cond_var_broadcast(ctrl->cv);
      ccol_mutex_unlock(ctrl->mutex);
      /* The thread that asks owns a FLUSH envelope; see
       * _clog_flush_pinned() and _clog_exit_drain_async(). Never free it. */
      continue;
    }

    /* CLOG_ASYNC_MSG_JOB */
    clog_async_job_t *job = &envelope->u.job;
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
         * from the batch: the code builds and writes this one record at once
         * and never puts it into async_buf. A message-oriented socket
         * delivers each write as one message, so a batch would put many
         * records into one message, which a receiver reads as one malformed
         * record, and a batch above the message limit of the socket is
         * refused as a whole. For syslog the code then writes the backtrace
         * frames that the job carries, each with its own write() call, while
         * logfmt and JSON already carry them inside the record. This matches
         * the synchronous write path exactly, except that the writer thread
         * does the work instead of the thread that submits.
         *
         * async_buf can still hold LOGFMT or JSON batch content that no
         * flush has written yet, content that is older than the
         * clog_set_format() call that pointed this shared target at
         * CLOG_FMT_SYSLOG: the code reads the format fresh for each job
         * instead of capturing it at submission time, and clog_set_format()
         * never touches async_buf. The flush here, instead of a bare
         * _buf_reset() that would discard that content unwritten, guarantees
         * that a format switch can move records that are already in the
         * batch ahead of this one, but can never lose them.
         *
         * When async_buf is already empty, which is the common case, this
         * call only updates last_flush_monotonic. It is safe to call here,
         * because rotation can never be on for a shared target that reaches
         * this branch: rotation needs a file-backed logger with
         * owns_fd == true, clog_set_format() allows CLOG_FMT_SYSLOG only when
         * owns_fd == false, and a path that the library opens is never a
         * socket. The rotation checks inside _writer_flush_now() therefore
         * always do nothing on this path.
         *
         * This settles and does not only flush, because the record goes
         * straight to the fd a few lines below; without the settle, a
         * continuation that the flush could not deliver would get this record
         * written into the middle of it. */
        _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
        _clog_settle_after_flush(sh, fmt, CLOG_WRITE_WAIT_FOREVER);

        /* A write that the kernel accepts only in part leaves the bytes that it
         * did not take at the front of async_buf, and the next flush offers
         * them again (see _writer_flush_now()). Those bytes are still
         * undelivered, so the code builds this record into a private scratch
         * buffer instead of appending the record onto them and then resetting
         * over them, which would discard them without a write. The code
         * allocates the scratch buffer only on that path; the common case, an
         * async_buf that the flush above emptied, builds straight into
         * async_buf, like every other record build in this file. */
        clog_buf_t scratch;
        clog_buf_t *rec_buf = &sh->async_buf;
        if (sh->async_buf.len > 0)
          rec_buf = _buf_init(&scratch, sh->m_procs) == 0 ? &scratch : NULL;

        if (!rec_buf) {
          /* Every buffer that the code could build into here would destroy the
           * undelivered bytes, so the code reports the record in the same
           * way as an unrepresentable one, which needs no allocation. The
           * undelivered bytes stay in async_buf, and the next flush tries
           * them again. */
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
            /* Not even the small, fixed-size fallback placeholder of
             * _clog_build_record() fits in an empty buffer. See the doc
             * comment of _clog_write_unrepresentable_record() for why the
             * library never drops the record to zero written bytes instead;
             * every other call site in this file that builds into a buffer
             * does the same. */
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
         * socket) the code appends to the batch without resetting it first
         * (see the doc comment of _clog_build_fallback_record() for the
         * reason), and flushes once the content reaches the size
         * threshold. */
        size_t record_start = sh->async_buf.len;
        bool batch_had_prior_content = record_start > 0;
        bool used_fallback = false;
        bool built = _clog_build_record(
            &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
            job->file, job->line, job->func, job->msg, &fsrc,
            job->with_backtrace, job->bt_syms, job->bt_depth, &used_fallback);
        if (!built || (used_fallback && batch_had_prior_content)) {
          /* One of two things happened: either not even the small, fixed-size
           * fallback placeholder fit in the room that async_buf had left, or
           * the code used the fallback only because unrelated content from
           * an earlier job already sat in the batch buffer and left too
           * little room for the real record of this job, in which case the
           * record itself need not be oversized at all. Both are reachable
           * only when one record, or the content that the batch already
           * holds, comes close to the cap_limit of async_buf.
           *
           * The code rolls back to record_start first, which discards the
           * fallback placeholder that this call may have appended already (a
           * `built` of true means that the placeholder is really in the
           * buffer, not only attempted). It THEN flushes what it accumulated
           * safely ahead of it, and tries again. An empty buffer always has
           * more than enough room for an ordinary record, and if the record
           * is genuinely too large on its own, this retry produces the same
           * "too large" fallback from record_start == 0, where that message
           * is accurate.
           *
           * Without the rollback, the flush below writes the misleading
           * fallback placeholder to disk BEFORE the retry runs, and the file
           * then keeps both it and the real record, instead of the real
           * record alone. The rollback is what keeps one guarantee true for
           * every value of flush_buffer_size: the library never substitutes
           * a misleading placeholder for a record that would have fit on its
           * own. */
          sh->async_buf.len = record_start;
          /* This settles and does not only flush, because the retry below can
           * end with a record that goes straight to the fd, and without the
           * settle, that record would split a continuation that the flush
           * could not deliver. */
          _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
          _clog_settle_after_flush(sh, fmt, CLOG_WRITE_WAIT_FOREVER);
          /* This retry runs against a buffer that _writer_flush_now() emptied,
           * except after a flush whose own write was cut short, which leaves
           * the undelivered remainder in place (see that function). The cap
           * of async_buf stays exactly as it was, because _buf_reset() never
           * shrinks it, and that cap is always at least CLOG_BUF_INITIAL,
           * which is much larger than the small fallback placeholder, so this
           * retry always succeeds in practice. The doc comment of
           * _clog_write_unrepresentable_record() gives the same reason for
           * lg->buf on the synchronous path and on the enqueue-failure path:
           * a single-record buffer that the code always resets to empty
           * first, and whose capacity can never drop below
           * CLOG_BUF_INITIAL, never needs to grow to fit that placeholder. So
           * this call cannot fail with the current constants.
           *
           * The code checks the return value anyway, because discarding it
           * would drop the record, and any backtrace with it, without a
           * trace as soon as a future change breaks that "cannot fail"
           * guarantee, as a smaller CLOG_BUF_INITIAL, or a buffer of another
           * size that the code shrank and then reused here, would do. That
           * is the class of failure that _clog_write_unrepresentable_record()
           * exists to guard against at its OTHER two call sites, and the
           * check here, with the same last resort that needs no allocation
           * and cannot fail itself, gives this call site the same
           * guarantee. */
          if (!_clog_build_record(
                  &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
                  job->file, job->line, job->func, job->msg, &fsrc,
                  job->with_backtrace, job->bt_syms, job->bt_depth, NULL)) {
            /* This writes real bytes straight to sh->fd and skips async_buf
             * completely, so, unlike every other write in this function, the
             * size check below never follows it: that check fires only off
             * the length of async_buf, which this path never touches. So the
             * code checks explicitly here; without the check, this corner
             * case, which is not reachable in practice, lets size-based
             * rotation fall behind the real size of the file on disk. Every
             * other direct-write call site in this file (_clog_write_sync()
             * and the enqueue-failure fallback of _clog_write_async()) does
             * the same, and both check after their own write, whichever path
             * produced it. */
            _clog_write_unrepresentable_record(sh, fmt, job->level,
                                               job->with_backtrace);
            _clog_size_rotate_if_due(sh);
          }
        }
        if (sh->async_buf.len >= sh->async_cfg.flush_buffer_size)
          _writer_flush_now(sh, CLOG_WRITE_WAIT_FOREVER);
      }
    }
    ccol_mutex_unlock(sh->mutex);

    _clog_async_job_release(sh, envelope);
  }
}

/*
 * Resolves the defaults of the configuration, creates the queue,
 * initializes the aggregation buffer and starts the writer thread, rolling
 * back everything that it has already built the moment that one step fails.
 * The code creates the queue first, so that there is nothing to unwind when
 * that step is the one that fails, and it sets sh->async_enabled to true
 * only after every step succeeds, so a state that is only part-initialized
 * is never visible as "enabled".
 *
 * Each public constructor calls this as the very last thing that it does,
 * when every other part of its setup is complete, which for
 * clog_open_file_mp() includes the block that configures rotation. The
 * writer thread does not exist until the rest of the construction is
 * finished, so it never sees the fields of sh while the thread that
 * constructs is still filling them in.
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

  char *err = NULL; /* The code discards this, because clog_open_*_mp gives no
      error string on any failure, which matches the convention of this file */
  if (sh->is_bounded_queue) {
    sh->q.circq = ccol_circular_queue_create_with_mprocs(async_cfg->queue_size,
                                                         sh->m_procs, &err);
    if (!sh->q.circq) return false;
  } else {
    sh->q.dynmq = ccol_dynamic_queue_create_with_mprocs(sh->m_procs, &err);
    if (!sh->q.dynmq) return false;
  }

  if (_buf_init(&sh->async_buf, sh->m_procs) != 0) {
    if (sh->is_bounded_queue)
      ccol_circular_queue_destroy(sh->q.circq);
    else
      ccol_dynamic_queue_destroy(sh->q.dynmq);
    return false;
  }
  /* The library must honour a flush_buffer_size that the caller sets above
   * CLOG_BUF_MAX instead of capping it at the smaller default that every
   * other clog_buf_t in this file uses, because that default is meant for a
   * single record. See the doc comment of clog_buf_t.cap_limit. */
  _buf_raise_cap_limit(&sh->async_buf, sh->async_cfg.flush_buffer_size);

  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);

  if (ccol_thread_create(sh->writer_thread, _clog_writer_thread_main, sh) !=
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
 * Sends the shutdown sentinel, trying again with a bounded backoff until the
 * queue accepts it, and never giving up after one attempt.
 *
 * One unchecked attempt is enough in practice for a BOUNDED queue:
 * ccol_circq_send_zc() blocks until space frees, and the writer thread still
 * drains at this point, so space always appears. It is not enough for the
 * UNBOUNDED queue: ccol_dynmq_send_zc() never blocks, and it can fail with
 * ccol_not_enough_memory when its own internal node allocation fails under
 * memory pressure, which is exactly the condition that can make a caller
 * close loggers.
 *
 * Ignoring that failure and going straight to ccol_thread_join() hangs
 * forever, because the writer thread never receives the sentinel that it
 * waits for, and nothing else ever sends one for this function.
 *
 * The retry is deliberate, because giving up here has no safe fallback. The
 * library cannot abandon the writer thread without a join() first: the
 * caller of this function is about to free sh, and, through _buf_free() and
 * the queue destroy, also the memory that the next loop iteration of the
 * writer thread still touches, so abandoning the thread turns a hang into a
 * use-after-free and a crash. clog_close() uses the same unbounded poll-wait
 * to wait out a concurrent resolver (see its own step 3). Both exist because
 * a give-up corrupts state that something else still uses, although an
 * unbounded wait is not free of cost.
 */
static void _clog_send_shutdown_sentinel_blocking(clog_shared_t *sh) {
  unsigned int delay_us = 1000; /* 1ms */
  for (;;) {
    c_message_t sentinel = {.data = NULL, .size = 0};
    ccol_retval_t rv = sh->is_bounded_queue
                           ? ccol_circq_send_zc(sh->q.circq, &sentinel)
                           : ccol_dynmq_send_zc(sh->q.dynmq, &sentinel);
    if (rv == ccol_success) return;
    /* Two failures reach here: a transient allocation failure in the node
     * allocation of the unbounded queue, and ccol_not_permitted, which
     * _shared_async_teardown() rules out before it calls this. The code tries
     * again, because without the retry, the writer thread waits for a
     * sentinel that never arrives. usleep() is not a pthread or sem
     * primitive, so it needs no wrapper in common.h; clog_close() uses it in
     * the same way. */
    usleep(delay_us);
    if (delay_us < 100000) delay_us *= 2; /* cap backoff at 100ms */
  }
}

/*
 * Joins the writer thread and destroys the queue and async_buf, after first
 * making sure that the library really told the writer thread to stop (see
 * _clog_send_shutdown_sentinel_blocking() above).
 *
 * Two paths use this function: the teardown of the last handle in
 * clog_close(), and the rollback path of each constructor, which runs when
 * _clog_handle_acquire() fails after _shared_async_init() has already
 * succeeded. No caller can submit a job through a handle that does not
 * exist yet, so on that path the sentinel is always the only thing in the
 * queue.
 */
static void _shared_async_teardown(clog_shared_t *sh) {
  /* The exit drain turns sends off (see _clog_exit_drain()); a close after
   * it, from a destructor, still has to reach the writer thread. This is
   * the last handle of the target, so no other send can race it. */
  if (sh->is_bounded_queue)
    (void)ccol_circq_enable_sending(sh->q.circq);
  else
    (void)ccol_dynmq_enable_sending(sh->q.dynmq);
  _clog_send_shutdown_sentinel_blocking(sh);
  ccol_thread_join(sh->writer_thread);
  if (sh->is_bounded_queue)
    ccol_circular_queue_destroy(sh->q.circq);
  else
    ccol_dynamic_queue_destroy(sh->q.dynmq);
  _buf_free(&sh->async_buf);
}

/* ========================================================================== */
/*                         PUBLIC LIFECYCLE                                   */
/* ========================================================================== */

clog clog_open_fd_mp(int fd, clog_level_t min_level,
                     const clog_async_cfg_t *async_cfg,
                     ccol_memmgmt_procs_t *mprocs) {
  if (fd < 0) return CLOG_INVALID;
  int _fd_flags = fcntl(fd, F_GETFL);
  if (_fd_flags == -1) return CLOG_INVALID;
  int _accmode = _fd_flags & O_ACCMODE;
  if (_accmode != O_WRONLY && _accmode != O_RDWR) return CLOG_INVALID;
  if (!ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) return CLOG_INVALID;
  struct clogger *lg = _alloc(fd, false, NULL, -1, min_level, mprocs);
  if (!lg) return CLOG_INVALID;

  if (async_cfg && !_shared_async_init(lg->shared, async_cfg)) {
    /* The code copies this pointer before _logger_free(lg) below, which frees
     * `lg` itself, because a read of lg->shared after that call is a
     * use-after-free. */
    clog_shared_t *sh = lg->shared;
    _shared_close_owned_fd(sh);
    ccol_mutex_destroy(sh->mutex);
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
    ccol_mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }
  return h;
}

/*
 * Seeds sh->last_rotation for a time-rotating logger that opens a file which
 * already holds records, so that the interval counts from the moment at
 * which the current file began, not from this open, and a process that runs
 * for less than the interval, or restarts often, still rotates. The best
 * available answer wins:
 *   1. The stamp of the newest rotated generation on disk. The rotation that
 *      made it also created the current file, at that moment.
 *   2. The birth time of the file, where the kernel and the filesystem
 *      report one.
 *   3. The last modification time of the file.
 * A seed in the future, from a clock that stepped back, starts the interval
 * at the time of the open, so that the interval never runs longer than
 * asked. The newest rotated name that step 1 finds is also the one that
 * _rotate() needs before its first rotation, so this records it and saves
 * that listing. Only a generation counts (see clog_gen_filter_t). This
 * listing writes no WARN record, because the open can still join another
 * target of the same file, and the deletion pass of the first rotation names
 * every entry that it ignores.
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
   * -1 on FreeBSD, and the seed then stays the modification time. */
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

clog clog_open_file_mp(const char *path, clog_level_t min_level,
                       const clog_rotation_cfg_t *cfg,
                       const clog_async_cfg_t *async_cfg,
                       ccol_memmgmt_procs_t *mprocs) {
  if (!path) return CLOG_INVALID;
  if (!ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) return CLOG_INVALID;

  /* A rotating logger opens the directory of the file first, and then the
   * file relative to it, and keeps that directory open for its whole life,
   * with every rotation working relative to it. So a relative path names the
   * same file for as long as the logger lives, whatever the working
   * directory of the process becomes after this call. */
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

  /* The identity of the directory entry that this target writes; see
   * _clog_file_register_or_attach(). A directory that cannot be examined
   * leaves it unknown, and the target then stays on its own. */
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
     * time_rotation_enabled, but this one does not, because the prune
     * applies to the rotated files that either mechanism produces, so it is
     * a property of the whole rotation configuration. Most callers reach a 0
     * here because they leave the field at its zero-initialized default, not
     * by choice, and such a caller must not keep rotated files forever and
     * fill the disk; the fallback to CLOG_DEFAULT_MAX_ROTATED_FILES here
     * prevents that. It matches the <= 0 fallback of the other two rotation
     * fields exactly. */
    if (cfg->max_rotated_files <= 0)
      lg->shared->rotation.max_rotated_files = CLOG_DEFAULT_MAX_ROTATED_FILES;

    /* This starts the byte counter from the current size on disk, which
     * size-based rotation needs so that it counts a file that already exists,
     * is not empty, and is opened by the library for append; without it, the
     * counter assumes that the file starts empty.
     *
     * fstat() can hardly fail on an fd that this function just opened, but
     * the code does not treat that failure as unreachable: it tries an
     * lseek() to the end of the file as a fallback, which is harmless under
     * O_APPEND, because O_APPEND always appends at the true end of the file
     * whatever offset the fd caches. When size rotation is on and BOTH calls
     * fail, this constructor fails; otherwise it would under-count a file
     * that can be large and let it grow far past the max_file_size that the
     * caller configured before the first rotation fires. */
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
      ccol_mutex_destroy(sh->mutex);
      _logger_free(lg);
      _shared_free_partial(sh);
      return CLOG_INVALID;
    }
  }

  /* The async setup runs strictly after the rotation configuration block
   * above, and never at the same time as it, because the writer thread must
   * never see the rotation fields of sh while this constructing thread is
   * still filling them in. */
  if (async_cfg && !_shared_async_init(lg->shared, async_cfg)) {
    /* The code copies this pointer before _logger_free(lg) below, which frees
     * `lg` itself, because a read of lg->shared after that call is a
     * use-after-free. */
    clog_shared_t *sh = lg->shared;
    _shared_close_owned_fd(sh);
    ccol_mutex_destroy(sh->mutex);
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
    ccol_mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }
  return _clog_file_register_or_attach(h, lg->shared, min_level);
}

/*
 * The teardown of a shared target whose last handle is gone. clog_close()
 * runs it, and so does an open that attached to a target and then failed
 * after every other handle of that target closed. The caller holds no lock.
 */
static void _clog_shared_finish_last(clog_shared_t *sh) {
  /* This removes sh from live_shareds under a fresh acquisition of the
   * write lock, strictly BEFORE anything else here touches sh->mutex again;
   * see "Fork safety" for why that order matters. */
  ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  _clog_shareds_remove(clog_slot_table.live_shareds, sh);
  ccol_rw_lock_unlock(clog_slot_table.rwlock);

  /* This runs while the code does NOT hold sh->mutex. The loop of the
   * writer thread locks that same mutex itself to process everything ahead
   * of the shutdown sentinel and the final flush of the sentinel, so holding
   * it here across the ccol_thread_join() inside _shared_async_teardown() is
   * a self-deadlock: the writer thread could never lock it to reach the
   * sentinel at all.
   *
   * This must also run BEFORE the code closes the fd below, because other
   * sibling handles that are already closed can still have jobs in the
   * queue, which the writer thread must drain and write at this exact
   * instant. */
  if (sh->async_enabled) _shared_async_teardown(sh);

  /* This runs after the writer thread stops, because that thread can still
   * rotate, and queue a compression, while it drains. It finishes every
   * queued compression before it returns, and it runs before the close of
   * dir_fd below, which those compressions use. */
  _shared_compressor_teardown(sh);

  _shared_close_owned_fd(sh);
  /* The target leaves file_shareds only once its threads are stopped and
   * its descriptors are closed. Until that point, an open of the same file
   * waits for it instead of building a second target that writes the file
   * while this one still rotates or compresses it. */
  if (sh->file_id_known) {
    ccol_rw_lock_wrlock(clog_slot_table.rwlock);
    _clog_shareds_remove(clog_slot_table.file_shareds, sh);
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }
  ccol_mutex_destroy(sh->mutex);
  _shared_free_partial(sh);
}

/*
 * Whether a second target on the same file can join the first one, a: both
 * have the same rotation, the same async configuration and the same
 * allocator, after the defaults are applied.
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
 * tears sh down when that was the last one. */
static void _clog_shared_unref(clog_shared_t *sh) {
  ccol_mutex_lock(sh->mutex);
  int remaining = --sh->ref_count;
  ccol_mutex_unlock(sh->mutex);
  if (remaining != 0) return;
  /* The teardown uses the vectors of the slot table, so it counts as a
   * close in flight, exactly as clog_close() does. */
  ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight++;
  ccol_rw_lock_unlock(clog_slot_table.rwlock);
  _clog_shared_finish_last(sh);
  ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight--;
  _clog_release_slot_table_if_deferred_locked();
  ccol_rw_lock_unlock(clog_slot_table.rwlock);
}

/*
 * The last step of clog_open_file_mp(). h is the handle of lg, a new logger
 * whose new target sh writes the file. Two targets that write one file,
 * where at least one of them rotates, lose records, because each renames,
 * prunes and compresses files that the other one still writes. So one file
 * has one target in a process whenever either open rotates.
 *
 * When no other target writes the same directory entry, sh joins
 * file_shareds and h is the result. When a target that writes it exists and
 * either of the two rotates, the new target is closed again, and the open
 * joins the existing target with a new logger of its own level, exactly as
 * clog_derive() joins a target, when the two configurations are the same
 * (see _clog_file_targets_compatible()); it fails with CLOG_INVALID when
 * they differ. Two targets that both do not rotate stay independent: each
 * appends whole records to the file, and neither renames it. An existing
 * target whose last close is still running is waited for, because it can
 * still rotate and compress.
 */
static clog _clog_file_register_or_attach(clog h, clog_shared_t *sh,
                                          clog_level_t min_level) {
  if (!sh->file_id_known) return h;
  unsigned int delay_us = 1000;
  for (;;) {
    ccol_rw_lock_wrlock(clog_slot_table.rwlock);
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
      /* The recovery runs before the target is published, under the table lock
       * that another opener of the same file needs to find it, so no other
       * handle can rotate this file, and no compression of this target can
       * publish a ".gz" file, while the recovery lists the directory and
       * removes the ".gz" file of each source that it saw. Once the target
       * is published, a joined handle could rotate and its compression could
       * finish between that listing and that removal, and the removal would
       * then delete a finished output whose source is already gone. */
      if (sh->rotation_enabled) {
        ccol_mutex_lock(sh->mutex);
        _clog_remove_stale_live_tmps(sh);
        ccol_mutex_unlock(sh->mutex);
      }
      if (sh->rotation_enabled && sh->rotation.compress_rotated) {
#ifdef RUNNING_UNIT_TESTS
        _clog_test_recover_gate();
#endif
        ccol_mutex_lock(sh->mutex);
        _clog_recover_compressions(sh);
        ccol_mutex_unlock(sh->mutex);
      }
      bool ok =
          cvector_push_back(clog_slot_table.file_shareds, &sh) == ccol_success;
      ccol_rw_lock_unlock(clog_slot_table.rwlock);
      if (!ok) {
        clog_close(h);
        return CLOG_INVALID;
      }
      return h;
    }
    /* The write lock keeps match alive: its last close removes it from
     * file_shareds under this lock before it frees it. */
    ccol_mutex_lock(match->mutex);
    int refs = match->ref_count;
    bool compatible = refs > 0 && _clog_file_targets_compatible(match, sh);
    if (compatible) match->ref_count++;
    ccol_mutex_unlock(match->mutex);
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
    if (refs == 0) {
      /* usleep() is not a pthread or sem primitive, so it needs no wrapper
       * in common.h; clog_close() waits in the same way. */
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

void clog_close(clog h) {
  struct clogger *raw;
  clog_shared_t *sh;

  /* Steps 1-2: resolve and validate, and mark in_use = false, both under the
   * write lock of the table, which must keep a concurrent resolve from
   * starting once the close begins. A double close is a fatal programming
   * error, matching the explicit design of chttpsvr. */
  {
    ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
    if (h == 0)
      ccol_fatal_err(
          "clog_close: clog handle is invalid, stale, or already closed");
    uint32_t idx = (uint32_t)(h >> 32);
    uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
    ccol_rw_lock_wrlock(clog_slot_table.rwlock);
    clog_slot_t *slot = NULL;
    if (idx < cvector_elem_count(clog_slot_table.slots))
      slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
    if (!slot || !slot->in_use || slot->generation != gen) {
      ccol_rw_lock_unlock(clog_slot_table.rwlock);
      ccol_fatal_err(
          "clog_close: clog handle is invalid, stale, or already closed");
    }
    raw = slot->ptr;
    sh = raw->shared;
    slot->in_use = false;
    /* The charge runs from here, where nothing can find this close, to the
     * last statement of the function, which is the whole span over which the
     * close still reads the vectors of the table. Every exit between the two
     * points is a fatal abort, so the count cannot be stranded. */
    clog_slot_table.closes_in_flight++;
    /* This is the same step, under the same lock. From here on no new resolve
     * can find this handle, which is what lets the pin count below reach zero
     * and stay there. */
    ccol_pintable_retire(&clog_pintable, idx);
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }

  /* Step 3: poll-wait until every in-flight operation that holds a pin on
   * this exact handle finishes, including one that may still hold
   * raw->fields_mutex, which step 4 below destroys. There is no upper bound,
   * because giving up here would free memory that a live resolver still
   * points at. usleep() is not a pthread or sem primitive, so it needs no
   * wrapper in common.h. */
  {
    uint32_t idx = (uint32_t)(h >> 32);
    unsigned int delay_us = 1;
    while (ccol_pintable_pins(&clog_pintable, idx) > 0) {
      usleep(delay_us);
      if (delay_us < 1000) delay_us *= 2;
    }
  }

#ifdef RUNNING_UNIT_TESTS
  /* This widens the window in which in_use is false and freed is false, from
   * here to step 4 below, deterministically and for a test only; see the
   * doc comment of clog_test_set_close_finalize_delay_us(). Outside a test
   * that arms it, it reads a plain 0 and does nothing. */
  {
    unsigned int d = atomic_load(&_clog_test_close_finalize_delay_us);
    if (d) {
      atomic_store(&_clog_test_close_finalize_delay_entered, true);
      usleep(d);
    }
  }
#endif

  /* Step 4: take the write lock again, and under it destroy the per-handle
   * state of raw, which includes fields_mutex, and retire its slot. This
   * must happen under the SAME lock that _clog_atfork_prepare needs before it
   * can even try to lock the fields_mutex of a slot with freed == false, so
   * the two operations cannot interleave; see "Fork safety" for the full
   * reasoning.
   *
   * This step also does the final bookkeeping of the slot: it clears ptr,
   * increments the generation again, and pushes the index onto the free
   * list. There is no reason to defer that work until after the teardown of
   * the shared object below, which can be slow. */
  {
    ccol_rw_lock_wrlock(clog_slot_table.rwlock);
    uint32_t idx = (uint32_t)(h >> 32);
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
    _logger_free(raw);
    slot->freed = true;
    slot->ptr = NULL;
    slot->generation++;
    if (slot->generation == 0) slot->generation++; /* skip the sentinel */
    cvector_push_back(clog_slot_table.free_indices, &idx);
    ccol_rw_lock_unlock(clog_slot_table.rwlock);
  }

#ifdef RUNNING_UNIT_TESTS
  /* This widens the window in which this close has already cleared its own
   * slot->ptr but is not yet finished with the table, deterministically and
   * for a test only; see the doc comment of
   * clog_test_set_close_release_window_us(). Outside a test that arms it, it
   * reads a plain 0 and does nothing. */
  {
    unsigned int w = atomic_load(&_clog_test_close_release_window_us);
    if (w) {
      atomic_store(&_clog_test_close_release_window_entered, true);
      usleep(w);
    }
  }
#endif

  /* Step 5: decrement the shared ref count. This never holds the write lock
   * of the slot table and shared->mutex at the same time, because step 4
   * releases the write lock before this step takes shared->mutex. */
  ccol_mutex_lock(sh->mutex);
  int remaining = --sh->ref_count;
  ccol_mutex_unlock(sh->mutex);

  /* Step 6: only the last handle sharing sh actually frees it. */
  if (remaining == 0) _clog_shared_finish_last(sh);

  /* This comes last of all. The destructor that runs at process exit can
   * find a logger that is still open, and it cannot release the table in
   * that case, so the release falls to whichever close is the last one out,
   * which can be this call. It has to happen after everything above,
   * because a release of the table destroys live_shareds together with the
   * slot vectors, and step 6 walks live_shareds.
   *
   * A release in step 4 instead would make the two conditions one condition
   * rather than two independent ones, and an attempt there could never fire,
   * because this close charged itself against the table in step 1 and still
   * counts as a user of it, so the library would never release the table.
   *
   * Position alone is only enough for one thread. A concurrent close that
   * has already cleared its own slot is invisible to the "is any slot still
   * live" scan while it sits between its own steps 4 and 6, so this call
   * could destroy live_shareds under it; the closes_in_flight count that
   * this decrement belongs to is what makes the guard cover that case too. A
   * sibling handle that is still open keeps the table either way. */
  ccol_rw_lock_wrlock(clog_slot_table.rwlock);
  clog_slot_table.closes_in_flight--;
  _clog_release_slot_table_if_deferred_locked();
  ccol_rw_lock_unlock(clog_slot_table.rwlock);
}

clog clog_derive(clog parent_h) {
  struct clogger *parent = _clog_resolve(parent_h);
  if (!parent)
    ccol_fatal_err(
        "clog_derive: clog handle is invalid, stale, or already closed");

  /* min_level is _Atomic, so reading it directly here, as the fast path of
   * _clog_write() already does, needs no lock of its own. */
  struct clogger *child = _logger_alloc(parent->shared, parent->min_level);
  if (!child) {
    _clog_resolve_unpin(parent);
    return CLOG_INVALID;
  }

  /* This copies the fields of parent into the independent field map of the
   * child under parent->fields_mutex alone, not shared->mutex. That is the
   * same lock that clog_set_field(), clog_remove_field() and
   * clog_clear_fields() take, so the copy synchronises against a concurrent
   * change to the field map of parent without contending with an unrelated
   * write in progress on a sibling logger that shares the same
   * shared->mutex. */
  ccol_mutex_lock(parent->fields_mutex);
  if (chmap_elem_count(parent->fields) > 0) {
    cmap_iterator *it = chashmap_begin_iter(parent->fields, NULL);
    if (!it) {
      ccol_mutex_unlock(parent->fields_mutex);
      _logger_free(child);
      _clog_resolve_unpin(parent);
      return CLOG_INVALID;
    }
    while (it) {
      const char *k = (const char *)it->key_pair->ptr;
      const char *v = (const char *)it->val_pair->ptr;
      cmap_pair kp = {.ptr = (void *)k, .size = strlen(k) + 1};
      cmap_pair vp = {.ptr = (void *)v, .size = strlen(v) + 1};
      /* ccol_key_already_present is a documented success, meaning that the map
       * wrote the value in place, not a failure. Only a genuine failure, such
       * as ccol_invalid_args or ccol_not_enough_memory, stops the derive. */
      ccol_retval_t rv = chmap_insert_elem(child->fields, &kp, &vp);
      if (rv != ccol_success && rv != ccol_key_already_present) {
        ccol_iter_destroy(it);
        ccol_mutex_unlock(parent->fields_mutex);
        _logger_free(child);
        _clog_resolve_unpin(parent);
        return CLOG_INVALID;
      }
      it = it->_next_fn(it);
    }
  }
  ccol_mutex_unlock(parent->fields_mutex);

  /* The whole derive tree shares ref_count, so it still needs
   * shared->mutex, but the code holds that mutex only for this short
   * increment. parent stays alive for the whole duration of this call,
   * because its own pin is held throughout, so parent->shared is not
   * destroyed yet, and it is safe to take this lock here without holding it
   * from the top of the function. */
  ccol_mutex_lock(parent->shared->mutex);
  parent->shared->ref_count++;
  ccol_mutex_unlock(parent->shared->mutex);

  clog h = _clog_handle_acquire(child);
  if (h == CLOG_INVALID) {
    /* child->shared == parent->shared, and parent->shared is still fully alive
     * here, because the pin of parent is held through this whole function, so
     * a read of child->shared->m_procs inside _logger_free(child) below is
     * always safe. */
    ccol_mutex_lock(parent->shared->mutex);
    parent->shared->ref_count--;
    ccol_mutex_unlock(parent->shared->mutex);
    _logger_free(child);
    _clog_resolve_unpin(parent);
    return CLOG_INVALID;
  }

  _clog_resolve_unpin(parent);
  return h;
}

/* ========================================================================== */
/*                         LEVEL CONTROL                                      */
/* ========================================================================== */

/*
 * min_level lives on struct clogger and not on clog_shared_t, because it is
 * a setting of one logger, fully independent of every sibling logger that
 * shares the same target; see the comment on the field itself.
 *
 * Reading or writing it through lg->shared->mutex would serialise
 * clog_set_level() and clog_get_level() on ONE logger against a write, a
 * rotation, or a gzip compression handoff on an unrelated SIBLING logger,
 * with no correctness benefit. The atomicity of min_level is what makes an
 * unlocked read of it free of a race, and the fast-path check of
 * _clog_write() and the locked re-check of _clog_write_sync() both rely on
 * that property, not on a lock.
 *
 * A plain read or write of an _Atomic lvalue is itself sequentially
 * consistent, so this needs no lock at all, not even a lighter one.
 * clog_set_field(), clog_remove_field() and clog_clear_fields() avoid
 * shared->mutex for their own per-logger state in the same way, through
 * fields_mutex.
 */
void clog_set_level(clog h, clog_level_t level) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_set_level: clog handle is invalid, stale, or already closed");

  lg->min_level = level;
  _clog_resolve_unpin(lg);
}

clog_level_t clog_get_level(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_get_level: clog handle is invalid, stale, or already closed");

  clog_level_t l = lg->min_level;
  _clog_resolve_unpin(lg);
  return l;
}

/* ========================================================================== */
/*                         OUTPUT FORMAT */
/* ========================================================================== */

void clog_set_format(clog h, clog_format_t fmt) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_set_format: clog handle is invalid, stale, or already closed");

  ccol_mutex_lock(lg->shared->mutex);
  /* CLOG_FMT_SYSLOG requires a caller-supplied fd (owns_fd == false). */
  if (fmt == CLOG_FMT_SYSLOG && lg->shared->owns_fd) {
    ccol_mutex_unlock(lg->shared->mutex);
    _clog_resolve_unpin(lg);
    return;
  }
  lg->shared->format = fmt;
  ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}

clog_format_t clog_get_format(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_get_format: clog handle is invalid, stale, or already closed");

  ccol_mutex_lock(lg->shared->mutex);
  clog_format_t f = lg->shared->format;
  ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return f;
}

void clog_set_facility(clog h, clog_syslog_facility_t facility) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_set_facility: clog handle is invalid, stale, or already closed");

  ccol_mutex_lock(lg->shared->mutex);
  lg->shared->syslog_facility = facility;
  ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}

clog_syslog_facility_t clog_get_facility(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_get_facility: clog handle is invalid, stale, or already closed");

  ccol_mutex_lock(lg->shared->mutex);
  clog_syslog_facility_t f = lg->shared->syslog_facility;
  ccol_mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return f;
}

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

void clog_set_field(clog h, const char *key, const char *value) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_set_field: clog handle is invalid, stale, or already closed");

  if (!key || !value) {
    _clog_resolve_unpin(lg);
    return;
  }

  /* This rejects a key that would corrupt logfmt output or RFC 5424 SD
   * output. An RFC 5424 SD-PARAM-NAME needs at least one character, and
   * every character must come from PRINTUSASCII (0x21 to 0x7e). The test
   * c < 0x21 rejects both a control character and a space in one comparison,
   * and the test c > 0x7e rejects DEL and every non-ASCII byte from 0x80 to
   * 0xff, which PRINTUSASCII also excludes. */
  if (*key == '\0' || _is_reserved_field_key(key)) {
    _clog_resolve_unpin(lg);
    return;
  }
  for (const char *p = key; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x21 || c > 0x7e || *p == '=' || *p == '"' || *p == '\\' ||
        *p == ']') {
      _clog_resolve_unpin(lg);
      return;
    }
  }

  /* The fields_mutex of this logger guards this, not shared->mutex, because
   * the field map of this logger is fully independent of every sibling
   * logger that shares the same backing store, so a change to a field here
   * must not contend with an unrelated write in progress on a sibling. See
   * the doc comment of fields_mutex on struct clogger. */
  ccol_mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};
  chmap_insert_elem(lg->fields, &kp, &vp);

  ccol_mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

void clog_remove_field(clog h, const char *key) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_remove_field: clog handle is invalid, stale, or already closed");

  if (!key) {
    _clog_resolve_unpin(lg);
    return;
  }

  ccol_mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  chmap_delete_elem(lg->fields, &kp);

  ccol_mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

void clog_clear_fields(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_clear_fields: clog handle is invalid, stale, or already closed");

  ccol_mutex_lock(lg->fields_mutex);
  chmap_reset(lg->fields, 0);
  ccol_mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

/* ========================================================================== */
/*                         CORE WRITE                                         */
/* ========================================================================== */

/*
 * This is the real synchronous body, which builds and writes one record
 * immediately. It walks the live fields under lg->fields_mutex and uses a
 * fresh (fmt, ap) pair, making its own internal va_copy for the vsnprintf
 * retry that goes from the stack buffer to a heap buffer.
 *
 * Two callers use it: the FATAL branch and the non-async branch of
 * _clog_write(), and the fallback of _clog_write_async() for a failed
 * envelope allocation, which is the one case where nothing specific to the
 * async path has happened yet, so ap is still completely fresh.
 *
 * bt_syms and bt_depth are inputs that the caller has already captured (see
 * the doc comment of _capture_backtrace() for why this function never
 * captures them itself). This function only reads them and never frees
 * bt_syms; whichever caller captured bt_syms keeps that responsibility.
 *
 * Precondition: NONE. This function takes and releases shared->mutex itself,
 * for its whole body, so a caller must never wrap a call to it in its own
 * lock.
 *
 * The attribute __attribute__((format(printf, 9, 0))) lets the internal
 * vsnprintf(msg_stack or msg_heap, ..., fmt, ap) calls below pass fmt and ap
 * straight through. In that attribute fmt is parameter 9, and the 0 means
 * that the variadic arguments already sit in the va_list ap, so there is no
 * first variadic parameter to name. Without the attribute, the
 * -Wformat-nonliteral warning of Clang reports that fmt is not a literal at
 * those call sites.
 *
 * fmt is indeed never a literal here, because it is a parameter of this
 * function that _clog_write() forwards unchanged from its own variadic call,
 * where the compiler already checks the format, because clogger.h puts the
 * same attribute on that function. This moves the real check of the format
 * string and the argument types to the call sites of _clog_write(), where
 * fmt is a literal, instead of the two other options: losing the check
 * completely, or suppressing it with a pragma.
 */
static bool _clog_exit_lock_target(clog_shared_t *sh,
                                   const struct timespec *deadline);
static bool _clog_fatal_lock_target(clog_shared_t *sh,
                                    const struct timespec *deadline);

/* The write of the record of a CLOG_FATAL call: _clog_write_record(), plus
 * the answer whether every byte went out. By then the writes are bounded;
 * see _clog_write_fatal(). */
static __attribute__((noinline, cold)) bool _clog_fatal_write_record(
    clog_shared_t *sh, clog_format_t fmt, const char *data, size_t len) {
  size_t written = _write_all(sh, data, len, CLOG_WRITE_WAIT_FOREVER);
  if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  if (written < len) _clog_note_truncated_record(sh, fmt, data, written, len);
  return written == len;
}

/*
 * The body of the synchronous write of one record, instantiated twice with
 * `fatal` as a constant: once for every ordinary record, and once for the
 * record of a CLOG_FATAL call (see _clog_write_fatal()). The fatal
 * instantiation takes the mutex of the target within `deadline`, gives a
 * sink that is not a regular file the bounded path, and gives false when
 * the record did not go out whole. The ordinary instantiation keeps none of
 * that code.
 */
static inline __attribute__((always_inline, format(printf, 9, 0))) bool
_clog_write_sync_body(struct clogger *lg, clog_level_t level, const char *file,
                      int line, const char *func, bool with_backtrace,
                      char *const *bt_syms, int bt_depth, const char *fmt,
                      va_list ap, va_list ap2, bool fatal,
                      const struct timespec *deadline) {
  bool delivered = true;
  /* The identity of the calling thread needs no lock, because it comes from
   * the cache of this thread. */
  const char *proc_val = _clog_tident_get()->proc;

  if (fatal) {
    if (!_clog_fatal_lock_target(lg->shared, deadline)) return false;
    /* From this point, every write to a sink that is not a regular file ends
     * by the deadline: a pipe, a socket, a terminal and every other device
     * that poll(2) reports on can take nothing more. A regular file keeps the
     * ordinary path. */
    struct stat st;
    if (lg->shared->sink_kind != CLOG_SINK_PLAIN ||
        (lg->shared->fd >= 0 && fstat(lg->shared->fd, &st) == 0 &&
         !S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)))
      lg->shared->sink_kind |= CLOG_SINK_FATAL_BOUND;
  } else {
    ccol_mutex_lock(lg->shared->mutex);
  }

  /* CLOG_FATAL skips the level filter, because the library must always
   * record the cause of a stop, whatever min_level says.
   *
   * For the common case this re-check repeats the fast, unlocked check of
   * _clog_write(). Both read the same _Atomic min_level, so neither needs
   * shared->mutex to be free of a race against a concurrent clog_set_level()
   * call. The re-check exists so that a level change which lands between the
   * fast-path check and this point still applies to this one call, which is
   * the same rare boundary-line race that the fast path itself documents and
   * accepts. */
  if (level < lg->min_level && level != CLOG_FATAL) {
    ccol_mutex_unlock(lg->shared->mutex);
    return true;
  }

  if (lg->shared->fd < 0) {
    ccol_mutex_unlock(lg->shared->mutex);
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
   * the last write to that fd can have stopped in the middle of a record,
   * with the continuation of that record kept for a later flush, so a write
   * here without a settle first would put this record between the two halves
   * of another one. The flag is false for every synchronous logger, and for
   * every async one with nothing outstanding, so the common case costs one
   * byte of a cache line that this function already read, plus a branch that
   * falls through. See clog_shared_t.async_partial_record. */
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
      /* Not even the small, fixed-size fallback placeholder of
       * _clog_build_record() fits in lg->buf. See the doc comment of
       * _clog_write_unrepresentable_record() for why the library never drops
       * the record to zero written bytes instead. */
      _clog_write_unrepresentable_record(lg->shared, log_fmt, level,
                                         with_backtrace);
    }
  }

  /* The backtrace frames of syslog are exempt from the batch, and
   * _clog_build_record() above did NOT append them, so the code writes them
   * here, strictly after the write of the primary record. JSON has already
   * put its backtrace inline, and the lines of logfmt have already gone into
   * lg->buf above. */
  if (with_backtrace && lg->shared->fd >= 0 && log_fmt == CLOG_FMT_SYSLOG)
    _emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, bt_syms,
                                 bt_depth);

  /* ----------------------------------------------------------------------- */
  /* Size-based rotation check (after the write).                             */
  /* ----------------------------------------------------------------------- */
  _clog_size_rotate_if_due(lg->shared);

  ccol_mutex_unlock(lg->shared->mutex);
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

/* A forward declaration: the definition sits further down, because it needs
 * clog_async_ctrl_t, clog_async_msg_t and its own test hooks, which this
 * file introduces later. _clog_write_async() below must call it; see the doc
 * comment of that function for the reason. */
static void _clog_flush_pinned(struct clogger *lg);

/*
 * Formats one job and puts it on the queue for the writer thread.
 *
 * When the allocation of the envelope fails, nothing specific to the async
 * path has happened yet and ap is still fresh, so the whole call goes to
 * _clog_write_sync().
 *
 * A later enqueue can also fail, either because of a failed node allocation
 * in the unbounded queue, or with ccol_not_permitted, which every send gets
 * once the exit drain has emptied the target (see _clog_exit_drain()). By
 * then the vsnprintf call of this function has already consumed ap and ap2,
 * so the code cannot reuse _clog_write_sync(). Instead it builds and writes
 * the record directly from the job, which is fully populated by then, under
 * lg->shared->mutex, through the lg->buf of the CALLING handle, except that
 * a record joins sh->async_buf behind whole records that a transient write
 * error left there (see below). This function never drops a message, or a
 * backtrace that the caller asked for, on any path.
 *
 * Before that direct write, the code calls _clog_flush_pinned() to drain
 * everything that is already queued ahead of this job. Without that call,
 * the record of this job can win the race for lg->shared->mutex against the
 * writer thread and land on disk BEFORE an earlier job that the queue has
 * already accepted (from this same handle or from a sibling that shares the
 * same target) and that the writer thread has not reached yet. That
 * reorders the log output against the order of submission, and it is most
 * likely under the same memory pressure that made the enqueue of THIS job
 * fail.
 *
 * The queue send inside that flush can fail under the same condition, in
 * which case _clog_flush_pinned() returns at once instead of hanging (see
 * its own doc comment). So the flush stays a best effort, and does not give
 * a queue that stays broken a new way to hang a log call.
 *
 * The attribute __attribute__((format(printf, 9, 0))) matches the attribute
 * on _clog_write_sync(), for the same reason: fmt is parameter 9, which this
 * function forwards, and it is never a literal at the internal
 * vsnprintf(job->msg_inline or job->msg_heap, ..., fmt, ap) call sites
 * below, so the real check belongs at the call sites of _clog_write(), where
 * the compiler already checks the format.
 */
static void __attribute__((format(printf, 9, 0))) _clog_write_async(
    struct clogger *lg, clog_level_t level, const char *file, int line,
    const char *func, bool with_backtrace, char **bt_syms, int bt_depth,
    const char *fmt, va_list ap) {
  clog_async_msg_t *envelope =
      _ccol_mem_calloc(lg->shared->m_procs, 1, sizeof(*envelope));
  if (!envelope) {
    _clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                     bt_depth, fmt, ap);
    if (bt_syms) free(bt_syms);
    return;
  }
  envelope->kind = CLOG_ASYNC_MSG_JOB;
  clog_async_job_t *job = &envelope->u.job;

  job->bt_syms = bt_syms; /* the job owns this from here */
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
  va_copy(ap2, ap); /* the new copy this function boundary requires, distinct
      from _clog_write_sync()'s own internal one; crossing a function
      boundary with a va_list that gets used twice (the stack attempt here,
      the heap retry below) needs its own fresh copy taken before the first
      use consumes it */
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
  if (rv != ccol_success) {
    /* This drains everything that is already on the queue, from this handle
     * or from any sibling that shares lg->shared, before the direct write of
     * this job below, so that write can never land on disk ahead of an
     * earlier job that the queue has already accepted and that the writer
     * thread has not reached yet. See the doc comment of this function.
     *
     * The call is safe here: _clog_write(), the caller of this function,
     * reaches _clog_write_async() only for a level that is not FATAL, so this
     * can never nest inside the FATAL-path flush of _clog_write(). */
    _clog_flush_pinned(lg);

    ccol_mutex_lock(lg->shared->mutex);
    if (lg->shared->fd >= 0) {
      clog_format_t fmt2 = lg->shared->format;
      clog_field_source_t fsrc;
      fsrc.is_live = false;
      fsrc.u.snap.views = job->fields;
      fsrc.u.snap.count = job->field_count;
      fsrc.u.snap.pool = job->field_pool.buf;
      fsrc.u.snap.failed = job->fields_snapshot_failed;

      /* The code writes this record directly to sh->fd here, like every other
       * real write in this file, so it must run the same rotation checks as
       * every other write path: the time-based check before the write, and
       * the size-based check after it. Without them, a run of enqueue
       * failures leaves the file far past max_file_size or
       * rotation_interval_us with nothing noticing; one failure is enough in
       * a logger that is otherwise idle, because this record never touches
       * sh->async_buf, so no later flush can catch it.
       *
       * This record must also not land between the two halves of a record
       * that the flush above could not finish delivering, which is the same
       * reason why _clog_write_sync() settles before its own write.
       *
       * Nor may it land ahead of whole records that the batch still holds,
       * which a flush keeps after a transient write error. The batch gets
       * one more bounded delivery attempt first, and records that it still
       * holds after that stay ahead of this one: for logfmt and JSON on a
       * stream sink, this record joins the batch behind them, where the
       * writer thread delivers it in order. A syslog record, and any record
       * for a message-oriented socket, is never batched (see
       * _clog_writer_thread_main()); such a record goes out directly, and
       * so does a record too large to join the batch, and only those can
       * precede records that a transient error keeps. */
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
          ccol_mutex_unlock(wsh->mutex);
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
         * _clog_write_sync() handles the same condition in the same way, and
         * that condition is not reachable in practice. */
        _clog_write_unrepresentable_record(lg->shared, fmt2, job->level,
                                           job->with_backtrace);
      }
      if (job->with_backtrace && fmt2 == CLOG_FMT_SYSLOG)
        _emit_backtrace_syslog_lines(&lg->buf, lg->shared, job->level, &job->ts,
                                     job->bt_syms, job->bt_depth);

      _clog_size_rotate_if_due(lg->shared);
    }
    ccol_mutex_unlock(lg->shared->mutex);

    _clog_async_job_release(lg->shared, envelope);
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Lets a test force the ccol_mutex_init() or ccol_cond_var_init() of the
 * NEXT _clog_flush_pinned() call, which runs on the stack-local
 * clog_async_ctrl_t of that call, to fail deterministically, without making
 * pthread_mutex_init() or pthread_cond_init() fail for real: with default or
 * NULL attributes, the glibc implementation of those two has no real failure
 * path that a test can trigger portably. Each hook disarms itself the moment
 * that it fires, so it affects only the one call that a test targets.
 */
static _Atomic bool _clog_test_force_flush_mutex_init_failure = false;
static _Atomic bool _clog_test_force_flush_condvar_init_failure = false;

static bool _clog_test_consume_forced_flush_mutex_init_failure(void) {
  return atomic_exchange(&_clog_test_force_flush_mutex_init_failure, false);
}
static bool _clog_test_consume_forced_flush_condvar_init_failure(void) {
  return atomic_exchange(&_clog_test_force_flush_condvar_init_failure, false);
}

void clog_test_force_flush_mutex_init_failure(bool force) {
  atomic_store(&_clog_test_force_flush_mutex_init_failure, force);
}
void clog_test_force_flush_condvar_init_failure(bool force) {
  atomic_store(&_clog_test_force_flush_condvar_init_failure, force);
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
 * This is the core of clog_flush(), kept as a separate function so that the
 * FATAL path of _clog_write() can also call it with an lg that the caller
 * has ALREADY resolved and pinned. This function does no resolve and no
 * unpin of its own, and it assumes that the caller already knows that
 * lg->shared->async_enabled is true.
 *
 * The pin is held for this whole call, including the blocking wait below,
 * just as _clog_write() and _clog_write_async() each hold their own pin
 * across their own blocking ccol_circq_send_zc or ccol_dynmq_send_zc call.
 * This is what lets the poll-wait of clog_close() serialise against a flush
 * that is in flight, as it already does for a write that is in flight.
 */
static void _clog_flush_pinned(struct clogger *lg) {
  clog_async_ctrl_t ctrl; /* This lives on the stack, one per call, and
              ccol_mutex_init and ccol_cond_var_init run on a fresh instance
              each time. The code never uses a static or constant initializer,
              which is the standing pthread-wrapper rule of this file */
  /* The code checks both of these. Other pthread init calls in this file are
   * ordinary calls that hardly ever fail, but this one is different: a call
   * to ccol_mutex_lock() or ccol_cond_var_wait() on a mutex or condition
   * variable that is NOT fully initialized is undefined behaviour, not a
   * graceful degradation, and "it almost certainly succeeded" is not
   * enough.
   *
   * In that case the envelope of this function must also never reach the
   * writer thread, because there would be no correctly initialized
   * ctrl.mutex and ctrl.cv for that thread to lock and broadcast on at the
   * other end.
   *
   * On such a failure the code treats the call as if it enqueued nothing, as
   * the "very unlikely enqueue failure" branch below does. That is also what
   * keeps the other caller of this function, the FATAL path of
   * _clog_write(), from hanging on a call that is meant to stop the
   * process. */
  if (_clog_test_consume_forced_flush_mutex_init_failure() ||
      ccol_mutex_init(ctrl.mutex) != 0)
    return;
  if (_clog_test_consume_forced_flush_condvar_init_failure() ||
      ccol_cond_var_init(ctrl.cv) != 0) {
    ccol_mutex_destroy(ctrl.mutex);
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
    ccol_mutex_lock(ctrl.mutex);
    while (!ctrl.done) ccol_cond_var_wait(ctrl.cv, ctrl.mutex);
    ccol_mutex_unlock(ctrl.mutex);
  }
  /* The other branch is the failure path of ccol_not_permitted or
   * ccol_not_enough_memory, which mean that the send side is already off
   * (after the exit drain) or that the node allocation of the unbounded queue
   * failed. Nothing reached the queue for the writer thread to find and
   * acknowledge, so ctrl.done can never become true, and the code skips the
   * wait completely instead of hanging forever. This matters beyond
   * clog_flush() itself, because this same function is also the pre-flush
   * step of the FATAL path of _clog_write(), and an unconditional wait here
   * would hang a process forever instead of stopping it after a fatal
   * error. */

  ccol_mutex_destroy(ctrl.mutex);
  ccol_cond_var_destroy(ctrl.cv);
}

void clog_flush(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    ccol_fatal_err(
        "clog_flush: clog handle is invalid, stale, or already closed");
  if (lg->shared->async_enabled) _clog_flush_pinned(lg);
  _clog_resolve_unpin(lg);
}

/* The last delivery attempt for the batch of an async target, on the fatal
 * path and at exit. The drain before it is a flush that keeps what a
 * transient write error left undelivered, for a retry that the process
 * never reaches, so this makes one more delivery attempt with a bounded
 * wait, settles a split record, and names every record that is still
 * undelivered after it. The caller holds the mutex of sh.
 * timeout_ms bounds each write of the attempt and each marker (see
 * _write_all()), so a descriptor that takes nothing (a blocking pipe whose
 * reader stopped reading, among others) cannot hold the process. */
static void _clog_settle_batch_locked(clog_shared_t *sh, int timeout_ms) {
  if (sh->async_buf.len > 0) {
    _writer_flush_buffer(sh, timeout_ms);
    _clog_settle_after_flush(sh, sh->format, timeout_ms);
    _clog_drop_undelivered(sh, "the process stopped", timeout_ms);
  }
}

static void _clog_exit_drain(void);

/* True once the exit drain is registered with atexit(). */
static bool _clog_exit_drain_registered;

/* The end of a CLOG_FATAL call, after its record is written and its handle
 * is unpinned. It takes nothing from the handle, so it runs after the unpin,
 * and the path of every other level keeps its shape. exit() runs the exit
 * drain (see _clog_exit_drain()), which gives every other async logger its
 * queue and its batch and lets every queued compression finish, so that no
 * rotated file is left as a half-written ".gz" file beside its source. That
 * drain bounds each of its waits and takes only the mutex of a target that
 * has something to drain, so a thread that holds the mutex of another
 * logger for ever (for example one whose write to a pipe that nobody reads
 * blocks) never keeps a fatal error from stopping the process. The drain
 * runs here directly only when its registration failed. */
static __attribute__((noinline, noreturn)) void _clog_fatal_exit(void) {
  if (!_clog_exit_drain_registered) _clog_exit_drain();
  exit(EXIT_FAILURE);
}

/* ========================================================================== */
/*                         EXIT DRAIN                                         */
/* ========================================================================== */

/*
 * A process can end with exit(), or a return from main(), while a logger is
 * still open. Nothing then closes it, and without this drain every record
 * that still waits in the queue of an async logger, or in the batch of its
 * writer thread, is lost, and a running compression is cut off. So an exit
 * handler that the library registers when it loads runs, after every exit
 * handler that the application registers later, the same drain that a fatal
 * record runs: for every async target the queue, then the batch, with a
 * marker for what cannot be delivered; then every queued and running
 * compression.
 *
 * Every wait is bounded by one budget for the whole drain, including every
 * lock that the drain takes (the lock of the table of loggers, and the mutex
 * of a target), so a writer thread that cannot deliver, other threads that
 * keep logging, a thread that holds a lock of a logger and never lets it go,
 * or a compression of a large file delay the exit of the process by that
 * budget at most. The drain skips a target whose mutex it cannot take within
 * the budget, and skips the whole drain when it cannot take the table lock
 * in time; the records that such a target still holds can then be lost. A
 * compression that the budget cuts off stops and removes its temporary
 * output, and the next open of the log compresses the source again. The
 * drain skips the target whose writer thread or compressor thread calls
 * exit(), because that thread cannot serve the drain that it would wait for.
 */
#define CLOG_EXIT_DRAIN_BUDGET_MS 5000U
/* How long an abandoned compression gets to stop and remove its output,
 * beyond the budget. It stops at its next block of input. */
#define CLOG_EXIT_ABANDON_GRACE_MS 1000U

#ifdef RUNNING_UNIT_TESTS
static _Atomic unsigned int _clog_test_exit_drain_budget_ms = 0;

void clog_test_set_exit_drain_budget_ms(unsigned int budget_ms) {
  atomic_store(&_clog_test_exit_drain_budget_ms, budget_ms);
}

static unsigned int _clog_exit_drain_budget_ms(void) {
  unsigned int b = atomic_load(&_clog_test_exit_drain_budget_ms);
  return b ? b : CLOG_EXIT_DRAIN_BUDGET_MS;
}

void clog_test_lock_target_forever(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg) return;
  clog_shared_t *sh = lg->shared;
  _clog_resolve_unpin(lg);
  ccol_mutex_lock(sh->mutex);
}

void clog_test_lock_table_forever(void) {
  ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  ccol_rw_lock_wrlock(clog_slot_table.rwlock);
}

/* While set, the send of the exit drain to a bounded queue waits for 0
 * microseconds, as it does once the budget is spent. */
static _Atomic bool _clog_test_exit_drain_send_no_wait = false;

void clog_test_force_exit_drain_send_without_wait(bool on) {
  atomic_store(&_clog_test_exit_drain_send_no_wait, on);
}

/* The targets that the exit drain left alone because their writer thread
 * did not answer in time. A fixed table is enough for a test. */
#define CLOG_TEST_LEFT_TARGETS_MAX 16
static clog_shared_t *_clog_test_left_targets[CLOG_TEST_LEFT_TARGETS_MAX];
static _Atomic size_t _clog_test_left_targets_count = 0;

static void _clog_test_note_left_target(clog_shared_t *sh) {
  size_t i = atomic_fetch_add(&_clog_test_left_targets_count, 1);
  if (i < CLOG_TEST_LEFT_TARGETS_MAX) _clog_test_left_targets[i] = sh;
}

bool clog_test_exit_drain_left_target(clog h) {
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

/* The time from now until deadline, clamped at zero. */
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

/* The time from now until the monotonic deadline, in microseconds, rounded
 * up; 0 once the deadline has passed. */
static uint64_t _clog_us_until(const struct timespec *deadline) {
  struct timespec left = _clog_ts_until(deadline);
  return (uint64_t)left.tv_sec * UINT64_C(1000000) +
         ((uint64_t)left.tv_nsec + 999u) / 1000u;
}

/* The CLOCK_REALTIME time at which the monotonic deadline passes. The timed
 * locks of POSIX measure their timeout on CLOCK_REALTIME, so the drain turns
 * the time that it has left into such a time just before each lock. */
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

/* Locks the mutex of sh within the deadline. A lock that is free is taken
 * even when the deadline has already passed. */
static bool _clog_exit_lock_target(clog_shared_t *sh,
                                   const struct timespec *deadline) {
  struct timespec at = _clog_realtime_at(deadline);
  return ccol_mutex_timedlock(sh->mutex, at) == 0;
}

/* How long a CLOG_FATAL call still waits for the mutex of its target once
 * the deadline of the stop has passed. The writer thread of the target
 * steps aside while the call waits (see _clog_writer_lock()), so a holder
 * that makes progress, such as a write of a batch to a regular file, hands
 * the mutex over well within this time, while a holder whose write blocks
 * keeps it, and the call then gives up after this long. */
#define CLOG_FATAL_LOCK_GRACE_MS 100U

/* Takes the mutex of sh for the record of a CLOG_FATAL call, within the
 * deadline or, when that is sooner, within CLOG_FATAL_LOCK_GRACE_MS. The
 * drain before it can use the whole budget on a backlog, and a wait that
 * ended at the deadline would then be a single try, which a writer thread in
 * the middle of its work wins, so the fatal record would be lost even
 * against an output that takes it at once. */
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

/* The flush request of the exit drain. It lives on the heap because a
 * request that the budget gives up on stays in the queue, and the writer
 * thread answers it later. */
typedef struct {
  clog_async_ctrl_t ctrl;
  clog_async_msg_t envelope;
} clog_exit_flush_t;

/* Drains the queue of the async target sh, then its batch, within the
 * deadline. It gives false when the writer thread did not answer in time,
 * in which case that thread can hold the mutex of sh for as long as a write
 * of it blocks. The caller keeps sh alive: the exit drain holds the write
 * lock of the slot table, and a CLOG_FATAL call holds a pin on its handle. */
static bool _clog_exit_drain_async(clog_shared_t *sh,
                                   const struct timespec *deadline) {
  clog_exit_flush_t *req = malloc(sizeof(*req));
  if (!req) return true;
  ccol_cond_var_attr_t ca;
  bool ready = false;
  if (ccol_mutex_init(req->ctrl.mutex) == 0) {
    if (ccol_cond_var_attr_init(ca) == 0) {
      ready = ccol_cond_var_attr_setclock(ca, CLOCK_MONOTONIC) == 0 &&
              ccol_cond_var_init_ca(req->ctrl.cv, ca) == 0;
      ccol_cond_var_attr_destroy(ca);
    }
    if (!ready) ccol_mutex_destroy(req->ctrl.mutex);
  }
  if (!ready) {
    free(req);
    return true;
  }
  req->ctrl.done = false;
  req->envelope.kind = CLOG_ASYNC_MSG_FLUSH;
  req->envelope.u.ctrl = &req->ctrl;
  c_message_t m = {.data = &req->envelope, .size = sizeof(req->envelope)};
  ccol_retval_t rv;
  if (sh->is_bounded_queue) {
    /* Once the deadline has passed, the send does not wait, and a full queue
     * answers ccol_container_full. */
    uint64_t left_us = _clog_us_until(deadline);
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&_clog_test_exit_drain_send_no_wait)) left_us = 0;
#endif
    rv = ccol_circq_timed_send_zc(sh->q.circq, &m, left_us);
  } else {
    rv = ccol_dynmq_send_zc(sh->q.dynmq, &m);
  }
  bool done = false;
  if (rv == ccol_success) {
    ccol_mutex_lock(req->ctrl.mutex);
    while (!req->ctrl.done &&
           ccol_cond_var_timedwait(req->ctrl.cv, req->ctrl.mutex, *deadline) ==
               0) {
    }
    done = req->ctrl.done;
    ccol_mutex_unlock(req->ctrl.mutex);
  }
  if (rv == ccol_success && !done) {
    /* The writer thread still holds the request, in its queue or while at work
     * on the queue ahead of it, and answers it later, so the request stays
     * allocated; the process ends in a moment. Since the writer thread did
     * not keep up, the batch is left to it too. */
    return false;
  }
  ccol_mutex_destroy(req->ctrl.mutex);
  ccol_cond_var_destroy(req->ctrl.cv);
  free(req);
  /* A bounded queue that stayed full until the deadline has a writer
   * thread that does not keep up, as above. */
  if (rv == ccol_timed_out || rv == ccol_container_full) return false;
  if (done) {
    /* A thread that holds the mutex of sh past the deadline keeps the batch,
     * and the drain then leaves the target, as for a writer thread that did
     * not answer. */
    if (!_clog_exit_lock_target(sh, deadline)) return false;
    int left = _clog_ms_until(deadline);
    _clog_settle_batch_locked(sh, left < CLOG_SPLIT_RECORD_DRAIN_WAIT_MS
                                      ? left
                                      : CLOG_SPLIT_RECORD_DRAIN_WAIT_MS);
    ccol_mutex_unlock(sh->mutex);
  }
  return true;
}

/* Waits within the deadline until the compressor thread of sh has finished
 * every queued and running compression. At the deadline it drops the queued
 * jobs, whose sources stay uncompressed, and stops the running one. The
 * caller holds the write lock of the slot table. */
static void _clog_exit_drain_compressions(clog_shared_t *sh,
                                          const struct timespec *deadline) {
  /* A thread that holds the mutex past the deadline leaves the queued and
   * the running compressions to the compressor thread, which the end of the
   * process stops wherever it is. */
  if (!_clog_exit_lock_target(sh, deadline)) return;
  if (sh->compressor_live) {
    while ((sh->compress_head || sh->compress_running) &&
           ccol_cond_var_timedwait(sh->compress_cv, sh->mutex, *deadline) ==
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
             ccol_cond_var_timedwait(sh->compress_cv, sh->mutex, grace) == 0) {
      }
    }
  }
  ccol_mutex_unlock(sh->mutex);
}

static void _clog_exit_drain(void) {
  if (!atomic_load_explicit(&_clog_exit_drain_armed, memory_order_relaxed))
    return;
  ccol_call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  /* After a CLOG_FATAL call, its deadline bounds this drain too, so that one
   * budget bounds the whole stop. */
  struct timespec deadline =
      atomic_load_explicit(&_clog_fatal_bound_armed, memory_order_acquire)
          ? _clog_fatal_deadline
          : _clog_ts_after_ms(&start, _clog_exit_drain_budget_ms());
  /* The table lock keeps every target in live_shareds alive for the walk,
   * because a close removes its target under that lock before it frees it.
   * The walk takes it for writing because a forked child can reach it, where
   * the fork handling has initialized that lock afresh over the write lock of
   * the prepare step, and ThreadSanitizer reports a read lock there as a read
   * lock of a write-locked mutex. The lock order (the table lock, then a
   * mutex of a target) is the order that the fork handling uses. The queue
   * of a target comes before its compressions, because a writer thread that
   * drains the queue can still rotate, and a rotation queues a compression.
   * A target whose writer thread did not answer in time is left alone after
   * that, because the thread can hold its mutex. A thread that holds the
   * table lock past the deadline, for example one that stopped inside
   * clog_open, leaves nothing to drain in time. */
  struct timespec table_at = _clog_realtime_at(&deadline);
  if (ccol_rw_lock_timedwrlock(clog_slot_table.rwlock, table_at) != 0) return;
  if (clog_slot_table.live_shareds) {
    size_t n = cvector_elem_count(clog_slot_table.live_shareds);
    for (size_t i = 0; i < n; i++) {
      clog_shared_t *sh =
          *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
      if (sh == _clog_thread_serves) continue;
      if (sh->async_enabled) {
        if (!_clog_exit_drain_async(sh, &deadline)) {
#ifdef RUNNING_UNIT_TESTS
          _clog_test_note_left_target(sh);
#endif
          continue;
        }
        /* The queue and the batch are empty at this point, and nothing drains
         * them again: a record that reaches the queue from here on (from a
         * destructor, or from an exit handler that runs after this one)
         * would wait for a flush that the end of the process cuts off. With
         * sends turned off, _clog_write_async() writes each such record
         * itself, synchronously, exactly as after a failed enqueue. */
        if (sh->is_bounded_queue)
          (void)ccol_circq_disable_sending(sh->q.circq);
        else
          (void)ccol_dynmq_disable_sending(sh->q.dynmq);
      }
      /* Only a target that compresses has a compressor thread. The test reads
       * configuration that never changes after the open, so it takes no
       * lock, because the mutex of a target that does not rotate can be held
       * by a thread whose write to a pipe blocks. */
      if (sh->rotation_enabled && sh->rotation.compress_rotated)
        _clog_exit_drain_compressions(sh, &deadline);
    }
  }
  ccol_rw_lock_unlock(clog_slot_table.rwlock);
}

#ifdef RUNNING_UNIT_TESTS
void clog_test_run_exit_drain(void) { _clog_exit_drain(); }
#endif

/* The handler is registered when the library loads, so that it runs after
 * every exit handler that the application registers while it runs; a record
 * that such a handler logs is drained too. It does nothing in a process
 * that never starts a writer thread or a compressor thread. */
__attribute__((constructor)) static void _clog_register_exit_drain(void) {
  _clog_exit_drain_registered = atexit(_clog_exit_drain) == 0;
}

static ccol_once_flag_t _clog_fatal_once = CCOL_ONCE_INIT;

/* Fixes the deadline of the whole stop, once, when the first CLOG_FATAL call
 * of the process starts. */
static void _clog_fatal_arm(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  _clog_fatal_deadline = _clog_ts_after_ms(&now, _clog_exit_drain_budget_ms());
  atomic_store_explicit(&_clog_fatal_bound_armed, true, memory_order_release);
}

/* Reports on stderr that the record of a CLOG_FATAL call could not be
 * written, unless stderr is the output that did not take it. The write to
 * stderr is bounded too, since stderr can be a pipe that nobody reads. */
static void _clog_fatal_report_loss(clog_shared_t *sh) {
  static const char msg[] =
      "clogger: a CLOG_FATAL record could not be written to its log output "
      "in time; the process stops without it\n";
  /* The target can still be locked by a thread whose write never returns,
   * so fd is read without the lock. */
  int fd = __atomic_load_n(&sh->fd, __ATOMIC_RELAXED);
  struct stat out, err;
  if (fstat(STDERR_FILENO, &err) != 0) return;
  if (fd >= 0 && fstat(fd, &out) == 0 && out.st_dev == err.st_dev &&
      out.st_ino == err.st_ino)
    return;
  struct pollfd pfd = {.fd = STDERR_FILENO, .events = POLLOUT, .revents = 0};
  if (poll(&pfd, 1, CLOG_SPLIT_RECORD_DRAIN_WAIT_MS) > 0 &&
      (pfd.revents & POLLOUT)) {
    ssize_t w = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)w;
  }
}

/*
 * The path of a CLOG_FATAL record, which never returns. One deadline, the
 * budget of the exit drain from the moment that the first fatal call starts,
 * bounds the whole stop: every write of the target to a sink that is not a
 * regular file ends by that deadline (see CLOG_SINK_FATAL_BOUND), and so
 * does every wait of this path, except that the wait for the mutex of the
 * target may run CLOG_FATAL_LOCK_GRACE_MS past it (see
 * _clog_fatal_lock_target()). For an async target it first drains the
 * queue and the batch, exactly as the exit drain does, so that a message
 * logged moments before the fatal one reaches the log, and it then writes
 * the fatal record synchronously. So an output that takes nothing more
 * cannot keep the process from stopping: the record is then lost, a line on
 * stderr says so unless stderr is that same output, and the process stops
 * anyway. A write to a regular file waits on the disk and not on a reader,
 * and keeps no bound.
 */
static __attribute__((noinline, cold, noreturn, format(printf, 8, 0))) void
_clog_write_fatal(struct clogger *lg, const char *file, int line,
                  const char *func, bool with_backtrace, char **bt_syms,
                  int bt_depth, const char *fmt, va_list ap) {
  ccol_call_once(_clog_fatal_once, _clog_fatal_arm);
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
    ccol_fatal_err(
        "_clog_write: clog handle is invalid, stale, or already closed");

  /* This is the fast path. Most log_* call sites are at a level that the
   * filter drops (for example, clog_trace and clog_debug when a
   * logger runs at CLOG_INFO or higher), and they reach this check on each
   * call. Because this check of min_level comes first, such a call returns
   * immediately: on the sync path it never contends for shared->mutex, and
   * on the async path it never formats or copies a message that nobody
   * reads.
   *
   * A relaxed load is enough, because this check only prevents work in the
   * usual case and does not make the final decision for the synchronous path
   * (see the locked second check in _clog_write_sync()). The async path has
   * no second check of its own, so a message that this path put on the
   * queue before a concurrent clog_set_level() took effect is delivered.
   * clog_set_level(3) documents that boundary as intentional, for this path
   * and for the unlocked fast check of the synchronous path. CLOG_FATAL
   * always skips the filter. */
  if (level < atomic_load_explicit(&lg->min_level, memory_order_relaxed) &&
      level != CLOG_FATAL) {
    _clog_resolve_unpin(lg);
    return;
  }

  /* The capture happens HERE, in the frame of this function, in the same way
   * for every path below. See the doc comment of _capture_backtrace() for
   * why the code must never capture one level deeper, inside
   * _clog_write_sync() or _clog_write_async(): a deeper capture skips one
   * more frame without a trace. */
  char **bt_syms = NULL;
  int bt_depth = 0;
  if (with_backtrace) _capture_backtrace(&bt_syms, &bt_depth);

  va_list ap;
  va_start(ap, fmt);

  /* A fatal record takes a path of its own, which never returns. */
  if (__builtin_expect(level == CLOG_FATAL, 0))
    _clog_write_fatal(lg, file, line, func, with_backtrace, bt_syms, bt_depth,
                      fmt, ap);

  /* The code reads lg->shared->async_enabled here without a lock. Only two
   * places write it, each exactly once: the construction, before any reader
   * can see this shared object, and the downgrade inside
   * _clog_atfork_child(), which runs on the only thread that exists in a
   * freshly forked child, strictly before any other code in that child can
   * read the field. Since nothing changes the field while another thread can
   * read it, this read needs no lock. */
  if (lg->shared->async_enabled) {
    _clog_write_async(lg, level, file, line, func, with_backtrace, bt_syms,
                      bt_depth, fmt, ap);
    va_end(ap);
    _clog_resolve_unpin(lg);
    return;
  }

  /* Async is not configured, so the call goes to the synchronous body, and ap
   * passes straight through. va_end(ap) must come AFTER this call and not
   * before, because ap is still live and this call uses it. */
  _clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                   bt_depth, fmt, ap);
  va_end(ap);
  if (bt_syms)
    free(bt_syms); /* The sync path owns the free of what the code
captured above, because _clog_write_sync() only reads bt_syms and never
frees it */
  _clog_resolve_unpin(lg);
}
