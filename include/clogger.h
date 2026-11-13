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

// clogger: A structured, thread-safe logger with rotation, optional async
// batching and three formats (logfmt, JSON lines, RFC 5424 syslog). A logfmt
// record looks like
//
//   ts=<ISO-8601 UTC> level=<L> proc=<name>(<pid>):<thread>(<tid>)
//   src=<file>:<line> func=<fn> [fields] msg=<text>
//
// ERROR, ALERT and FATAL records add a backtrace as tab-indented continuation
// lines; build with -rdynamic to get symbol names in it.
//
// Every function is thread-safe on handles that are open. One lock serializes
// the writes and the rotation of an output target, shared by the root logger
// and every logger derived from it; each logger's level and fields are
// synchronized on their own, so changing them never waits for a sibling's
// write. Closing a handle while another thread uses that same handle is the
// caller's bug, as for every handle type of the library.
//
// Only file loggers rotate (by size after a write, by time on the first write
// after the interval), into <path>.<YYYYMMDDHHMMSS>[_N], optionally gzipped by
// a background thread, keeping max_rotated_files generations. At exit() an exit
// handler drains every async logger and the queued compressions within 5
// seconds. clog_create_from_file_mp(3), clogger(7) and doc/clogger.md have the
// full rules.
#include <stdbool.h>
#include <stdint.h>

#include "common.h"

// Everything from here to the matching pop is public ABI; the library is built
// with -fvisibility=hidden, so nothing else is exported.
#pragma GCC visibility push(default)

// <complex.h> may define clog as a macro, which would rewrite our handle type.
#ifdef clog
#undef clog
#endif

// Severity levels, lowest first. A logger drops records below its min_level.
// CLOG_ALERT means somebody must act now (syslog severity 1). CLOG_FATAL adds a
// backtrace, ignores min_level and ends the process with exit(EXIT_FAILURE).
// CLOG_OFF as min_level turns off everything except CLOG_FATAL. Each value is
// explicit because the library indexes tables with it; a new level needs an
// explicit value too.
typedef enum {
  CLOG_TRACE = 0,
  CLOG_DEBUG = 1,
  CLOG_INFO = 2,
  CLOG_WARN = 3,
  CLOG_ERROR = 4,
  CLOG_ALERT = 5,
  CLOG_FATAL = 6,
  CLOG_OFF = 7
} clog_level;

// Defaults a rotation config falls back to: Rotate at 10 MiB, rotate daily,
// keep 7 rotated files.
#define CLOG_DEFAULT_MAX_FILE_SIZE ((int64_t)(10 * 1024 * 1024))

#define CLOG_DEFAULT_ROTATION_INTERVAL_US ((uint64_t)86400000000ULL)

#define CLOG_DEFAULT_MAX_ROTATED_FILES 7

// How a file logger rotates; pass it to clog_create_from_file_mp(). A zero or
// negative max_file_size or max_rotated_files, and a zero rotation_interval_us,
// take the CLOG_DEFAULT_* value, so old files are always pruned. With
// compress_rotated each rotated file becomes <rotated>.gz on a background
// thread; a failed compression keeps the uncompressed file, and a static link
// needs -lz. The members are fixed-width (bytes, microseconds) so the layout
// doesn't depend on _FILE_OFFSET_BITS or _TIME_BITS. Example: Rotate at 50 MiB,
// keep 14 gzipped files:
//
//   clog_rotation_cfg cfg = {.size_rotation_enabled = true,
//                            .max_file_size = 50 * 1024 * 1024,
//                            .max_rotated_files = 14,
//                            .compress_rotated = true};
typedef struct clog_rotation_cfg {
  bool size_rotation_enabled;
  int64_t max_file_size;
  bool time_rotation_enabled;
  // Checked on whole seconds, so it rotates at the first whole second at or
  // after the end of the interval.
  uint64_t rotation_interval_us;
  int max_rotated_files;
  bool compress_rotated;
} clog_rotation_cfg;

// Defaults of an async config: Flush at 64 KiB, and at least every 200 ms.
#define CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE (64UL * 1024UL)

#define CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US ((uint64_t)200000ULL)

// Turns on async logging when passed to a create call (any non-NULL pointer,
// even to an all-zero struct; zero fields take their defaults). The calling
// thread formats its message and captures identity, time and fields; a writer
// thread does the rest and batches the records of every handle of the target,
// flushing at flush_buffer_size or every flush_interval_us. Syslog and message
// sockets get one write per record, never a batch. A flush that fails with an
// error a later attempt can outlast (ENOSPC, EDQUOT, EFBIG, ENOBUFS, ENOMEM,
// EAGAIN, EINTR) keeps the bytes for the next flush; any other error drops them
// and writes a "log record truncated" marker in their place. A marker the
// output refuses is written in front of the next write it accepts, and every
// rotated generation ends with a newline. CLOG_FATAL drains the queue and
// writes synchronously. clog_create_from_fd_mp(3) has the details.
typedef struct clog_async_cfg {
  // 0 for an unbounded queue that never blocks a sender; otherwise a bounded
  // queue of this many messages, where a full queue blocks the sender and never
  // drops a message.
  size_t queue_size;
  // Bytes; 0 for CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE.
  size_t flush_buffer_size;
  // Microseconds; 0 for CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US. It holds while
  // records keep the queue busy. UINT64_MAX means only flush_buffer_size,
  // clog_flush() and clog_close() flush. A writer with nothing to deliver
  // sleeps until work arrives, whatever the interval.
  uint64_t flush_interval_us;
} clog_async_cfg;

struct clogger;

// A logger handle: An index and a generation packed in a uint64_t, not a
// pointer, so never cast it to or from void *. A create call gives it and
// clog_close() ends it. CLOG_NULL (zero) is "no logger", so if (!lg) works.
typedef uint64_t clog;

#define CLOG_NULL ((clog)0)

// Creates a logger that writes to the open, writable fd (stdout, a pipe, a
// socket). The logger doesn't own fd, clog_close() doesn't close it, and it has
// no rotation. min_level must be a clog_level value. async_cfg turns on async
// logging (NULL for synchronous). Every allocation goes through mprocs (NULL
// for malloc), except the backtrace symbols and the per-thread identity cache,
// which use malloc. Gives the handle, or CLOG_NULL on an allocation failure,
// bad mprocs or min_level, an fd that is closed or read-only, or a call after
// the library's exit destructor. Close the handle with clog_close().
clog clog_create_from_fd_mp(int fd, clog_level min_level,
                            const clog_async_cfg *async_cfg,
                            ccol_memmgmt_procs *mprocs);

// Creates a logger that writes to the open, writable fd (stdout, a pipe, a
// socket), using malloc for its memory. The logger doesn't own fd, clog_close()
// doesn't close it, and it has no rotation. min_level must be a clog_level
// value; async_cfg turns on async logging (NULL for synchronous). Gives the
// handle, or CLOG_NULL on an allocation failure, a bad min_level, an fd that is
// closed or read-only, or a call after the library's exit destructor. Close the
// handle with clog_close().
static inline __attribute__((always_inline)) clog clog_create_from_fd(
    int fd, clog_level min_level, const clog_async_cfg *async_cfg) {
  return clog_create_from_fd_mp(fd, min_level, async_cfg, NULL);
}

// Creates a logger that appends to the file at path, creating it with mode 0644
// under the umask. cfg turns on rotation (NULL for none), async_cfg async
// logging (NULL for synchronous), and every allocation goes through mprocs
// (NULL for malloc), except the backtrace symbols and the per-thread identity
// cache, which use malloc. With
// rotation on, the logger keeps the directory open, so a relative path keeps
// naming the same file after a chdir(), and each new live file gets the mode,
// owner and group of the file it replaces. One file has one target per process
// whenever a logger of it rotates: Opening a file that a logger of this process
// already writes joins that target like clog_derive(), or gives CLOG_NULL when
// either config or the allocator differs. The time interval of a non-empty file
// counts from when the file began (the newest rotated generation, else its
// birth time, else its mtime). A failed rotation is retried on a later write,
// at least a second later. Gives the handle, or CLOG_NULL on a bad path or
// min_level, an allocation failure, a conflicting logger, or a call after the
// library's exit destructor. Close the handle with clog_close().
clog clog_create_from_file_mp(const char *path, clog_level min_level,
                              const clog_rotation_cfg *cfg,
                              const clog_async_cfg *async_cfg,
                              ccol_memmgmt_procs *mprocs);

// Creates a logger that appends to the file at path (created with mode 0644
// under the umask), using malloc for its memory. cfg turns on rotation (NULL
// for none) and async_cfg async logging (NULL for synchronous). With rotation
// on, a relative path keeps naming the same file after a chdir(), and opening a
// file that a rotating logger of this process already writes joins that target
// like clog_derive(), or gives CLOG_NULL when a config or the allocator
// differs. Gives the handle, or CLOG_NULL on a bad path or min_level, an
// allocation failure, a conflicting logger, or a call after the library's exit
// destructor. Close the handle with clog_close().
static inline __attribute__((always_inline)) clog clog_create_from_file(
    const char *path, clog_level min_level, const clog_rotation_cfg *cfg,
    const clog_async_cfg *async_cfg) {
  return clog_create_from_file_mp(path, min_level, cfg, async_cfg, NULL);
}

// Flushes and closes logger. The caller must make sure no other thread uses
// this same handle. The last handle of a target (root and derived) closes the
// file, waits for every queued compression and stops the background threads. A
// CLOG_NULL, stale or closed handle is fatal, and every handle is stale after
// the library's exit destructor.
void clog_close(clog logger);

// Creates a logger that shares parent's target (fd, lock, format, rotation) and
// starts with a copy of its level and fields; afterwards each keeps its own.
// Use it to add context for a component. Rotation goes on while any handle of
// the target is open. Close it with clog_close(). Gives the handle, or
// CLOG_NULL on an allocation failure. A CLOG_NULL, stale or closed parent is
// fatal.
clog clog_derive(clog parent);

// Blocks until an async logger has handed the OS every record queued before the
// call, including a partly full batch; does nothing for a synchronous logger.
// It can return with work left: When the queue can't take the flush request (a
// rare allocation failure), and when a write stops with an error a later flush
// can outlast, which keeps those bytes for that flush. A CLOG_NULL, stale or
// closed handle is fatal.
void clog_flush(clog logger);

// Sets logger's minimum level. Returns ccol_success, or ccol_invalid_args for a
// value clog_level doesn't name (the level stays). A bad handle is fatal.
ccol_retval clog_set_level(clog logger, clog_level level);

// Gives logger's minimum level; records below it are dropped. A CLOG_NULL,
// stale or closed handle is fatal.
clog_level clog_get_level(clog logger);

// Output format of a target, shared by every handle of it; the default is
// logfmt. JSON writes one object per line. Syslog writes RFC 5424 and works
// only for an fd logger (a file logger refuses it). Every format writes valid
// UTF-8: A bad byte becomes U+FFFD and a C1 control the escape \u0080 to
// \u009f. Syslog escapes control bytes and backslashes in MSG, SD-PARAM-VALUEs
// and backtrace frames, and '"' and ']' in values; MSG has no BOM (MSG-ANY).
typedef enum {
  CLOG_FMT_LOGFMT = 0,
  CLOG_FMT_JSON,
  CLOG_FMT_SYSLOG,
} clog_format;

// Sets the output format of logger's target, for every handle of it. Returns
// ccol_success, or ccol_invalid_args for a value the enum doesn't name or
// syslog on a file logger (the format stays). A bad handle is fatal.
ccol_retval clog_set_format(clog logger, clog_format fmt);

// Gives the output format of logger's target (logfmt, JSON or syslog), which
// every handle of the target shares. A CLOG_NULL, stale or closed handle is
// fatal.
clog_format clog_get_format(clog logger);

// Syslog facilities (RFC 5424) for clog_set_facility(); the default is
// CLOG_SYSLOG_USER, and the facility is shared by every handle of a target.
// Syslog output needs an fd already connected to the daemon, on Linux usually:
//
//   int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
//   struct sockaddr_un sa = {.sun_family = AF_UNIX};
//   strncpy(sa.sun_path, "/dev/log", sizeof(sa.sun_path) - 1);
//   connect(fd, (struct sockaddr *)&sa, sizeof(sa));
//   clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
//   clog_set_format(lg, CLOG_FMT_SYSLOG);
//
// Each write is one datagram, so keep a message below 2 KiB.
typedef enum {
  CLOG_SYSLOG_KERN = 0,
  CLOG_SYSLOG_USER = 1,
  CLOG_SYSLOG_MAIL = 2,
  CLOG_SYSLOG_DAEMON = 3,
  CLOG_SYSLOG_AUTH = 4,
  CLOG_SYSLOG_SYSLOG = 5,
  CLOG_SYSLOG_LPR = 6,
  CLOG_SYSLOG_NEWS = 7,
  CLOG_SYSLOG_UUCP = 8,
  CLOG_SYSLOG_CRON = 9,
  CLOG_SYSLOG_AUTHPRIV = 10,
  CLOG_SYSLOG_FTP = 11,
  CLOG_SYSLOG_LOCAL0 = 16,
  CLOG_SYSLOG_LOCAL1 = 17,
  CLOG_SYSLOG_LOCAL2 = 18,
  CLOG_SYSLOG_LOCAL3 = 19,
  CLOG_SYSLOG_LOCAL4 = 20,
  CLOG_SYSLOG_LOCAL5 = 21,
  CLOG_SYSLOG_LOCAL6 = 22,
  CLOG_SYSLOG_LOCAL7 = 23,
} clog_syslog_facility;

// Sets the syslog facility of logger's target, which matters only for syslog
// output. Returns ccol_success, or ccol_invalid_args for a value the enum
// doesn't name (12 to 15, or outside 0 to 23), so the PRI stays within RFC
// 5424. A bad handle is fatal.
ccol_retval clog_set_facility(clog logger, clog_syslog_facility facility);

// Gives the syslog facility of logger's target, CLOG_SYSLOG_USER unless
// clog_set_facility() changed it. A CLOG_NULL, stale or closed handle is fatal.
clog_syslog_facility clog_get_facility(clog logger);

// Adds a key=value field to every later record of logger, or replaces its
// value; both strings are copied. key must be printable US-ASCII (0x21 to 0x7e)
// except '=', ']', '"' and '\\', and not one of the fixed keys (ts, level,
// proc, src, func, msg, bt, bt_error). value may be any text and is quoted when
// needed. Returns ccol_success, ccol_invalid_args for a NULL or bad key or a
// NULL value, or ccol_not_enough_memory; a failure changes nothing. A bad
// handle is fatal.
ccol_retval clog_set_field(clog logger, const char *key, const char *value);

// Removes the field named key from logger, so later records don't carry it;
// does nothing when it isn't there or key is NULL. Derived loggers keep their
// own fields. A CLOG_NULL, stale or closed handle is fatal.
void clog_remove_field(clog logger, const char *key);

// Removes every key=value field of logger, so later records carry none.
// Derived loggers keep their own fields. A CLOG_NULL, stale or closed handle is
// fatal.
void clog_clear_fields(clog logger);

// What the library may do about SIGPIPE and SIGXFSZ, the two signals a log
// write can raise. AUTO (the default) makes sure no log write ends the process
// and touches signal state only where a pipe or a regular file needs it.
// UNTOUCHED never changes a disposition or a mask. Values are explicit; a new
// one needs an explicit value too.
typedef enum {
  CLOG_SIGNALS_AUTO = 0,
  CLOG_SIGNALS_UNTOUCHED = 1,
} clog_signal_policy;

// Sets the signal policy for the whole process, for every later write and open
// (running writer threads included). A socket always gets MSG_NOSIGNAL. Under
// AUTO, an async writer thread blocks SIGPIPE and takes back its own, and any
// other thread writing a pipe sets SIGPIPE to SIG_IGN once; opening a regular
// file (or a rotation's next one) sets SIGXFSZ to SIG_IGN once, so a write at
// RLIMIT_FSIZE fails with EFBIG and gets repaired, and the writer and
// compressor threads block it. A disposition is changed only while it's SIG_DFL
// and isn't changed back when the policy changes. Under UNTOUCHED the kernel's
// signals reach the application, which must then ignore or block SIGXFSZ if it
// sets a file size limit. Call it before the first logger opens to keep every
// disposition untouched. Returns ccol_success, or ccol_invalid_args for another
// value.
ccol_retval clog_set_signal_policy(clog_signal_policy policy);

// Renames the calling thread and updates the name its records carry. The logger
// reads a thread's name once, on its first record, so a rename by other means
// isn't seen. The first 15 bytes are kept on every platform. It may be called
// before any logger exists. Returns ccol_success, ccol_invalid_args for a NULL
// or empty name, or ccol_not_permitted when the system refuses or the platform
// isn't Linux, FreeBSD, OpenBSD or macOS.
ccol_retval ccol_set_thread_name(const char *thread_name);

// Internal: The function behind the clog_<level>() macros, which pass the call
// site and whether to add a backtrace. Use the macros.
void clog__write(clog logger, clog_level level, const char *file, int line,
                 const char *func, bool with_backtrace, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));

// Logs a printf-style message at the TRACE level, the most detailed one: l is
// the logger, then the format and its arguments. The record carries the source
// file, line and function of the call, and is dropped at once when l's minimum
// level is above TRACE. A CLOG_NULL, stale or closed l is fatal.
#define clog_trace(l, ...) \
  clog__write((l), CLOG_TRACE, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

// Logs a printf-style message at the DEBUG level: l is the logger, then the
// format and its arguments. The record carries the source file, line and
// function of the call, and is dropped at once when l's minimum level is above
// DEBUG. A CLOG_NULL, stale or closed l is fatal.
#define clog_debug(l, ...) \
  clog__write((l), CLOG_DEBUG, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

// Logs a printf-style message at the INFO level, for example
// clog_info(lg, "listening on %d", port). The record carries the source file,
// line and function of the call, and is dropped at once when lg's minimum
// level is above INFO. A CLOG_NULL, stale or closed logger is fatal.
#define clog_info(l, ...) \
  clog__write((l), CLOG_INFO, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

// Logs a printf-style message at the WARN level: l is the logger, then the
// format and its arguments. The record carries the source file, line and
// function of the call, and is dropped at once when l's minimum level is above
// WARN. A CLOG_NULL, stale or closed l is fatal.
#define clog_warn(l, ...) \
  clog__write((l), CLOG_WARN, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

// Logs a printf-style message at the ERROR level, followed by a backtrace of
// the call as tab-indented lines (link with -rdynamic for symbol names). l is
// the logger, then the format and its arguments. The record carries the source
// file, line and function, and is dropped at once when l's minimum level is
// above ERROR. A CLOG_NULL, stale or closed l is fatal.
#define clog_error(l, ...) \
  clog__write((l), CLOG_ERROR, __FILE__, __LINE__, __func__, true, __VA_ARGS__)

// Logs a printf-style message at the ALERT level, for a problem somebody must
// act on now, followed by a backtrace of the call as tab-indented lines (link
// with -rdynamic for symbol names). The record carries the source file, line
// and function, and is dropped at once when l's minimum level is above ALERT.
// A CLOG_NULL, stale or closed l is fatal.
#define clog_alert(l, ...) \
  clog__write((l), CLOG_ALERT, __FILE__, __LINE__, __func__, true, __VA_ARGS__)

// Logs a printf-style message at FATAL, with the source location and a
// backtrace, whatever l's level, then ends the process with exit(EXIT_FAILURE);
// it never returns. The exit drain then lets other async loggers and queued
// compressions finish. One 5 second budget from the first fatal call bounds the
// whole stop, this record's writes to a pipe, socket or terminal included (the
// wait for the logger may run 100 ms past it). A record that can't go out in
// time is lost and a line on stderr says so. A write to a regular file has no
// bound. A CLOG_NULL, stale or closed l ends the process with a fatal error
// and no record.
#define clog_fatal(l, ...) \
  clog__write((l), CLOG_FATAL, __FILE__, __LINE__, __func__, true, __VA_ARGS__)

// End of the public ABI.
#pragma GCC visibility pop
