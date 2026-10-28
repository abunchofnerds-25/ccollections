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
 * @file clogger.h
 * @brief Structured, thread-safe logger with log rotation
 *
 * Output format (logfmt):
 *   ts=<ISO-8601-UTC> level=<L> proc=<name>(<pid>):<tname>(<tid>)
 * src=<file>:<line> func=<fn> [fields] msg=<text>
 *
 * The logger adds a backtrace for the ERROR, ALERT and FATAL levels only. The
 * backtrace goes out as tab-indented continuation lines that don't start with
 * "ts=", so a log aggregator can group them with their parent record:
 *   \t#0 ./bin(fn+0x1a) [0x7f...]
 *   \t#1 ./bin(main+0x42) [0x7f...]
 *
 * Thread safety:
 *   All public functions are fully thread-safe for concurrent calls on
 *   handles that are still open. One lock serialises the writes and the log
 *   rotation of one output target, and the root logger and every logger
 *   derived from it share that lock (see clog_derive()). Each logger has its
 *   own structured fields (clog_set_field() and the related functions) and
 *   its own minimum log level (clog_set_level() and clog_get_level()), and
 *   both are synchronised separately from that lock, so changing either one
 *   never contends with a write in progress on a sibling logger of the same
 *   target.
 *
 *   clog_close() is outside all of that. As for every other handle-based type
 *   of this library, closing a handle safely is up to the caller and the
 *   library doesn't synchronise it for you: a close is safe only when no
 *   other thread holds or uses that same handle. Closing one handle is safe
 *   even while a sibling handle (the root or a derived logger) writes to the
 *   shared target, since the sibling is a different handle and it is still
 *   open.
 *
 * Log rotation:
 *   Only file-backed loggers (clog_create_from_file_mp) rotate. Size rotation
 *   starts after the write that crosses max_file_size, and time rotation on
 *   the first write after the interval ends. A rotated file is renamed to
 *   <path>.<YYYYMMDDHHMMSS>[_N], and each rotation takes a name above every
 *   rotated name before it. The new live file gets the permission bits, the
 *   owner and the group of the file it replaces. When more than
 *   max_rotated_files generations exist, the oldest ones are deleted. All of
 *   this happens in the directory where the logger opened the file, also for
 *   a relative path after the process changes its working directory. With
 *   compress_rotated set to true the rotated file is also compressed with
 *   gzip into a .gz file. The logger's own background thread does that, one
 *   file at a time, so neither a logging thread nor the async writer thread
 *   ever waits for a compression. A deletion pass keeps the newest
 *   max_rotated_files generations and deletes every older one, except an
 *   older one whose compression is running, which goes as soon as that
 *   compression ends; so at most max_rotated_files generations plus the one
 *   under compression stay on disk. The last clog_close() of the file
 *   finishes every queued compression before it returns, and a fatal record
 *   waits for them within the exit drain's budget (see "Process exit"). A
 *   compression writes <rotated>.gz.tmp and renames it to <rotated>.gz only
 *   once the output is complete, so a .gz file always holds a whole gzip
 *   stream. The .gz file gets the permission bits of its source, and its
 *   owner and group where the process is allowed to give them, like the new
 *   live file of a rotation. A rotated file whose <rotated>.gz.tmp name
 *   doesn't fit in the directory stays uncompressed, and a CLOG_WARN record
 *   in the new live file says so. Opening a compressing logger removes the
 *   .gz.tmp files that a stopped process left behind, removes a .gz file that
 *   sits beside its own source, and queues every uncompressed rotated file
 *   for compression. A rotated name whose stamp is more than 30 days above
 *   both the clock and the newest name the logger made isn't a generation:
 *   the library never renames, compresses or deletes it, and a CLOG_WARN
 *   record names it. So after the system clock steps back by more than 30
 *   days, a logger that opens the file after the step leaves the generations
 *   written before the step alone until the clock is back within 30 days of
 *   their stamps; removing or renaming them is the operator's job.
 *
 * Process exit:
 *   A process can end with exit() or a return from main() while a logger is
 *   still open. An exit handler that the library registers when it loads
 *   then delivers what every async logger holds, the same way a fatal record
 *   does, and waits for the queued compressions, all within one budget of 5
 *   seconds. The budget also bounds every lock the drain takes: a logger
 *   whose lock another thread holds past it is skipped, and the records it
 *   still holds can be lost. A compression the budget cuts off keeps its
 *   source and the next open compresses it. Every write of the drain is
 *   bounded too, on a pipe, a socket and a terminal as well, and the drain
 *   takes a logger's lock only when that logger has a queue or a compression
 *   to drain, so a thread blocked for ever inside a write of a synchronous
 *   logger delays no exit and no fatal record. A record that an async logger
 *   gets after its drain (from an exit handler that runs after the library's,
 *   or from a destructor) is written synchronously by the thread that logs
 *   it, and clog_close() works from there. A logger whose drain the budget
 *   cut off keeps its queue, and such a record can then be lost. _exit(), a
 *   signal and abort() run no exit handler.
 *
 *   Build with -rdynamic to get resolved symbols in backtraces.
 */

#include <stdbool.h>
#include <stdint.h> /* int64_t, uint64_t */

#include "common.h"

/* Every declaration from here to the end of this header belongs to the public
 * ABI of libccollections, and the shared library exports all of them. The
 * library is built with -fvisibility=hidden, so a function or object outside
 * these blocks stays internal: its name isn't in the dynamic symbol table, an
 * application that links against the library can't interpose it, and a
 * symbol of the same name in that application can't collide with it. */
#pragma GCC visibility push(default)

/* Remove any macro definition of clog from <complex.h> */
#ifdef clog
#undef clog
#endif

/* ========================================================================== */
/*                         LOG LEVELS                                         */
/* ========================================================================== */

/*
 * Every enumerator below has an explicit numeric value on purpose, instead of
 * one C's auto-increment picks, because the values are used as array indices
 * (see _LEVEL_STR[] and _SYSLOG_SEVERITY[] in clogger.c). Inserting one in
 * the middle without an explicit value would silently shift the value of
 * every level after it and misalign both arrays, and the compiler wouldn't
 * say a word. Any addition must have an explicit value too.
 */
typedef enum {
  CLOG_TRACE = 0,
  CLOG_DEBUG = 1,
  CLOG_INFO = 2,
  CLOG_WARN = 3,
  CLOG_ERROR = 4,
  CLOG_ALERT = 5, /**< You must act immediately. It maps to syslog
                     severity 1 */
  CLOG_FATAL = 6, /**< Adds a backtrace and stops the process with
                 exit(EXIT_FAILURE). This level ignores min_level: the fatal
                 message is always written, whatever level the logger is set
                 to. */
  CLOG_OFF = 7    /**< As min_level it turns off all output except CLOG_FATAL */
} clog_level_t;

/* ========================================================================== */
/*                         ROTATION CONFIGURATION */
/* ========================================================================== */

/** Default maximum log file size before rotation (10 MiB). */
#define CLOG_DEFAULT_MAX_FILE_SIZE ((int64_t)(10 * 1024 * 1024))

/** Default time-rotation interval in microseconds (1 day). */
#define CLOG_DEFAULT_ROTATION_INTERVAL_US ((uint64_t)86400000000ULL)

/** Default number of rotated files to keep. */
#define CLOG_DEFAULT_MAX_ROTATED_FILES 7

/**
 * @brief Log rotation configuration.
 *
 * Pass a pointer to this struct to clog_create_from_file_mp().
 *
 * A zero or negative max_file_size or max_rotated_files, and a zero
 * rotation_interval_us, fall back to the CLOG_DEFAULT_* constant of that
 * member. Deleting old rotated files can't be turned off, so a caller that
 * doesn't set max_rotated_files never piles up an unbounded number of rotated
 * files on disk.
 *
 * Example; rotate at 50 MiB, keep 14 files, gzip each rotated file:
 * @code
 * clog_rotation_cfg_t cfg = {
 *     .size_rotation_enabled  = true,
 *     .max_file_size          = 50 * 1024 * 1024,
 *     .time_rotation_enabled  = false,
 *     .max_rotated_files      = 14,
 *     .compress_rotated       = true,
 * };
 * clog lg = clog_create_from_file_mp("/var/log/app.log", CLOG_INFO, &cfg, NULL,
 * NULL);
 * @endcode
 *
 * With compress_rotated set to true, each rotated file is compressed with
 * zlib (gzip format) after the rotation, giving a
 * "<path>.<YYYYMMDDHHMMSS>[_N].gz" file, and on success the original
 * uncompressed file is removed. If a compression fails the uncompressed file
 * stays on disk, so no data is lost. If the directory can't hold the name
 * "<rotated>.gz.tmp", no compression of that file can ever succeed; the file
 * then stays uncompressed and a CLOG_WARN record about it goes into the new
 * live file. Every other failure is silent. A temporary output that a failed
 * compression couldn't remove is removed by a later attempt, before the next
 * compression or at the close. Standard tools (gunzip, zcat, gzip -d) can
 * decompress the compressed files.
 *
 * The logger starts a background thread at its first compression, and that
 * thread does the compressions one at a time, oldest first. The rotation only
 * puts the file in a queue, so neither the logging thread nor the async
 * writer thread ever waits for gzip. The last clog_close() of the file waits
 * until every queued compression is complete. A program that links the
 * static library must also link with -lz (pkg-config --static adds it); the
 * shared library has its own dependency on zlib.
 */
/*
 * The size member is the fixed-width int64_t and the interval member is a
 * uint64_t count of microseconds, the unit of every duration in the library,
 * instead of the off_t and time_t a size and a time would normally use. That
 * is on purpose. The widths of off_t and time_t are chosen by feature-test
 * macros (_FILE_OFFSET_BITS and _TIME_BITS), and those macros belong to the
 * translation unit being compiled, so a struct built from those types has a
 * layout the application and the library can disagree about whenever the two
 * sides are compiled with different settings. With that layout wrong, every
 * member after the first comes from the wrong offset, and the config silently
 * rotates on the wrong schedule and turns on options the caller never asked
 * for. A fixed-width member has one layout per target whatever the feature
 * macros say, and that's why this struct means the same thing on both sides
 * of the ABI.
 */
typedef struct clog_rotation_cfg {
  bool size_rotation_enabled;
  int64_t max_file_size; /**< Bytes; <= 0 -> CLOG_DEFAULT_MAX_FILE_SIZE */
  bool time_rotation_enabled;
  uint64_t rotation_interval_us; /**< Microseconds; 0 ->
                                    CLOG_DEFAULT_ROTATION_INTERVAL_US. The
                                    check runs on whole seconds, so an
                                    interval rotates at the first whole
                                    second at or after its end. */
  int max_rotated_files;         /**< Files kept on disk; <= 0 ->
                                    CLOG_DEFAULT_MAX_ROTATED_FILES */
  bool compress_rotated; /**< Gzip-compress each rotated file with zlib */
} clog_rotation_cfg_t;

/* ========================================================================== */
/*                         ASYNC LOGGING CONFIGURATION */
/* ========================================================================== */

/** Default flush threshold of the aggregation buffer for async logging
 * (64 KiB). */
#define CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE (64UL * 1024UL)

/** Default flush interval of the aggregation buffer for async logging, in
 * microseconds (200 ms). */
#define CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US ((uint64_t)200000ULL)

/**
 * @brief Async logging configuration.
 *
 * Pass a pointer to this struct to clog_create_from_fd_mp() or
 * clog_create_from_file_mp() (or to their wrappers without the _mp suffix),
 * and the new logger, plus every logger derived from it later, logs
 * asynchronously. The calling thread only formats its own message and
 * captures its own identity, timestamp and fields, and it hands the rest of
 * the work (escaping, serializing the fields and the write() call itself) to
 * a dedicated writer thread. The writer thread collects the records of every
 * handle that shares the same target into one buffer, and flushes that
 * buffer when the records reach flush_buffer_size or when flush_interval_us
 * passes.
 *
 * Unlike clog_rotation_cfg_t, a NULL async_cfg and a non-NULL pointer to an
 * all-zero clog_async_cfg_t are NOT the same thing. ANY non-NULL pointer
 * turns on async mode and each zero field falls back to its documented
 * default below. Only a literal NULL turns async mode off, since this struct
 * has no "enabled" flag you could leave false.
 *
 * These values never apply to CLOG_FMT_SYSLOG, which does no batching: each
 * record and each backtrace frame keeps its own write() call, one per UDP
 * datagram. They don't apply to a message-oriented socket either
 * (SOCK_DGRAM, SOCK_SEQPACKET or SOCK_RDM, such as a UDP socket or a Unix
 * datagram socket). There every record, in any format, goes out with its own
 * write(), and so as a message of its own, exactly as a synchronous logger
 * sends it. A record larger than the socket can carry fails alone: a "log
 * record truncated" record is written in its place and the records after it
 * are still delivered. A socket whose receiver has no room makes the write
 * wait, both where the system blocks the send and where it refuses it with
 * ENOBUFS (FreeBSD does that for a Unix datagram socket): the logger sends
 * again until the message fits, and CLOG_FATAL and the drain at exit wait
 * only within their time budget.
 *
 * A flush whose write fails with an error that a later attempt can outlast
 * (ENOSPC, EDQUOT, EFBIG, ENOBUFS, ENOMEM, or EAGAIN or EINTR that ends a
 * wait) keeps the undelivered bytes for the next flush. Any other write
 * error, EPIPE, ECONNRESET or EBADF for example, fails the same way on every
 * retry, so the flush drops the undelivered bytes and writes a "log record
 * truncated: undelivered batch dropped (records=<n> bytes=<m>): write error
 * <errno>" record in their place. The last flush of clog_close() and the
 * last attempt of a fatal record have no later retry, so whatever they can't
 * deliver is named the same way.
 *
 * The marker of a loss goes to the output that has just failed, and on a
 * full disk that usually fails too. The logger then keeps it, together with
 * the newline that closes a record a write cut, and writes both in front of
 * the next write the output accepts; a record that arrives while that
 * newline is still waiting counts as one more loss instead of continuing the
 * fragment. A rotation closes such a fragment in the generation it retires:
 * it writes the newline there or, when that file still refuses it, cuts the
 * file back to its last whole line and counts the cut bytes in the marker,
 * so every rotated generation ends with a newline.
 * When a rotation is due and the file has no room (ENOSPC, EDQUOT or EFBIG)
 * for the rest of a record that a write cut, the flush gives up that rest,
 * names it and rotates, which deletes the oldest generations. A batch holding
 * more than flush_buffer_size bytes is written in pieces with a size rotation
 * check between them, so such a backlog makes one file grow past
 * max_file_size by one record at most. An ordinary flush is one write, and it
 * can take the file past max_file_size by up to flush_buffer_size bytes,
 * because size rotation is checked after each write. See
 * clog_create_from_fd_mp(3) and clog_create_from_file_mp(3).
 *
 * CLOG_FATAL always goes around the async queue and writes synchronously,
 * and it first drains everything already in the queue, since the process
 * stops right after it. An exit() or a return from main() with the logger
 * still open drains the queue the same way, within a bounded time (see
 * "Process exit" at the top of this header).
 */
typedef struct clog_async_cfg {
  /** Capacity of the message queue. 0 -> an unbounded queue that never blocks
   * a sending caller. A value > 0 -> a bounded queue of this many messages; a
   * full bounded queue BLOCKS the calling thread until space frees up, and a
   * message is never silently dropped. */
  size_t queue_size;
  /** Bytes. The writer thread flushes when its aggregation buffer reaches
   * this size. 0 -> CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE. */
  size_t flush_buffer_size;
  /** Microseconds. The writer thread flushes at least this often, even when
   * the buffer doesn't reach flush_buffer_size and while records keep the
   * queue busy without a pause. 0 ->
   * CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US. Any value of the type is valid:
   * UINT64_MAX, or any interval too long for the clock to reach, means only
   * flush_buffer_size, clog_flush() and clog_close() start a flush. A writer
   * thread with nothing to deliver (an empty buffer and no repair owed to the
   * output) sleeps until a record or a request arrives, whatever the
   * interval. */
  uint64_t flush_interval_us;
} clog_async_cfg_t;

/* ========================================================================== */
/*                         LOGGER TYPE                                        */
/* ========================================================================== */

struct clogger;

/**
 * @brief Opaque logger handle.
 *
 * A `clog` is an opaque, generational value handle that packs an index and a
 * generation counter into a uint64_t. It isn't a pointer: never cast it to or
 * from `void *`, and never compare two handles after casting either one to a
 * pointer. Both `if (!lg)` and `lg == CLOG_NULL` work as you'd expect,
 * because CLOG_NULL is numerically zero.
 *
 * You get a handle from clog_create_from_fd_mp() or clog_create_from_file_mp()
 * and free it with clog_close().
 */
typedef uint64_t clog;

/** @brief Sentinel value that means "no logger". It is numerically zero. */
#define CLOG_NULL ((clog)0)

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

/**
 * @brief Create a logger that writes to a file descriptor that is already open,
 * with custom memory management procs.
 *
 * The logger doesn't own the file descriptor, so clog_close() does NOT close
 * it. An fd-based logger has no log rotation. Every dynamic memory operation
 * of the logger goes through the memory management procs the caller gives,
 * with two exceptions that belong to the library rather than to one logger:
 * the symbol text of a backtrace, which backtrace_symbols() allocates with
 * malloc(), and the small identity cache (program name, process and thread
 * ID, thread name) that each thread fills with malloc() on its first record
 * and that all loggers of the process share; it is freed when the thread
 * exits.
 *
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, ...).
 * @param min_level The logger silently drops a message below this level. It
 *                  must be one of the values of clog_level_t.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or CLOG_NULL on an allocation failure, for
 *         invalid mprocs, for a min_level that clog_level_t doesn't name, or
 *         when fd is closed, read-only or invalid for some other reason. An
 *         open from a destructor that runs after the library's destructor
 *         (which releases the table of loggers at the end of the process)
 *         also gives CLOG_NULL.
 */
clog clog_create_from_fd_mp(int fd, clog_level_t min_level,
                            const clog_async_cfg_t *async_cfg,
                            ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Create a logger that writes to a file descriptor that is already open.
 *
 * The logger doesn't own the file descriptor, so clog_close() does NOT close
 * it. An fd-based logger has no log rotation.
 *
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, ...).
 * @param min_level The logger silently drops a message below this level.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @return New logger handle, or CLOG_NULL on an allocation failure, for a
 *         min_level that clog_level_t doesn't name, or when fd is closed,
 *         read-only or invalid for some other reason.
 */
static inline __attribute__((always_inline)) clog clog_create_from_fd(
    int fd, clog_level_t min_level, const clog_async_cfg_t *async_cfg) {
  return clog_create_from_fd_mp(fd, min_level, async_cfg, NULL);
}

/**
 * @brief Create a file-backed logger with optional log rotation, and with
 * custom memory management procs.
 *
 * The file is created if it doesn't exist (mode 0644, narrowed by the
 * process's umask) and opened in append mode if it does. Every dynamic memory
 * operation of the logger goes through the memory management procs the
 * caller gives, with the two exceptions clog_create_from_fd_mp() names.
 *
 * A rotation creates each new live file with the permission bits of the file
 * it replaces, whatever the umask, and gives it that file's owner and group
 * where the process is allowed to, as logrotate does. When the process can't
 * give it the group, the group bits are cleared so no other group gains
 * access. So a log its operator restricted, to 0600 or 0640 for example,
 * stays restricted in every generation.
 *
 * With rotation on, the logger keeps the file's directory open for its whole
 * life, and every rotation, deletion and compression works in that
 * directory, so a relative path keeps naming the same file after the process
 * changes its working directory.
 *
 * In a process one file has one backing store whenever a logger of it
 * rotates. When a logger of this process already writes the same file (the
 * same directory and the same last component of the path) and either of the
 * two rotates, this call joins that logger the way clog_derive() does: the
 * new handle has its own level and fields and shares the target. It gives
 * CLOG_NULL when the rotation configuration, the async configuration or
 * the allocator differs from that logger's, or when only one of the two
 * rotates. Two opens of one file where neither rotates stay independent.
 *
 * The time rotation interval of a file that already holds data counts from
 * the moment that file began, not from this call. That moment is the stamp of
 * the newest rotated generation on disk, because the rotation that made it
 * also created the current file; if there's none, it is the file's birth
 * time where the filesystem reports one, and its last modification time
 * otherwise. So a process that runs for less than the interval, or restarts
 * often, still rotates. The interval of an empty or new file counts from this
 * call, and so does an interval whose starting moment lies in the future.
 * When the wall clock steps back to a moment before the start of the
 * interval, the interval starts again at that moment, so the next time
 * rotation comes one interval after the step. A rotation that fails is tried
 * again on a later write, at least one second after the failure, and a clock
 * that steps back doesn't make that wait longer.
 *
 * @param path      Path of the destination log file.
 * @param min_level The logger silently drops a message below this level. It
 *                  must be one of the values of clog_level_t.
 * @param cfg       Rotation config, or NULL to turn off rotation.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or CLOG_NULL on a failure, for example a bad
 *         path, a min_level that clog_level_t doesn't name, an allocation
 *         error, a configuration that differs from that of a rotating logger
 *         of this process on the same file, or an open from a destructor that
 *         runs after the library's destructor, which releases the table of
 *         loggers at the end of the process.
 */
clog clog_create_from_file_mp(const char *path, clog_level_t min_level,
                              const clog_rotation_cfg_t *cfg,
                              const clog_async_cfg_t *async_cfg,
                              ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Create a file-backed logger with optional log rotation.
 *
 * The file is created if it doesn't exist (mode 0644, narrowed by the
 * process's umask) and opened in append mode if it does. See
 * clog_create_from_file_mp() for the mode of the files a rotation creates.
 *
 * @param path      Path of the destination log file.
 * @param min_level The logger silently drops a message below this level.
 * @param cfg       Rotation config, or NULL to turn off rotation.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @return New logger handle, or CLOG_NULL on a failure, for example a bad
 *         path, a min_level that clog_level_t doesn't name, or an allocation
 *         error.
 */
static inline __attribute__((always_inline)) clog clog_create_from_file(
    const char *path, clog_level_t min_level, const clog_rotation_cfg_t *cfg,
    const clog_async_cfg_t *async_cfg) {
  return clog_create_from_file_mp(path, min_level, cfg, async_cfg, NULL);
}

/**
 * @brief Flush the logger, close it, and free all the resources that it owns.
 *
 * Don't use the handle after this call. The caller must make sure no other
 * thread uses or closes this exact handle at the same time; the "Thread
 * safety" note at the top of this header gives the scope of that
 * requirement. The file descriptor is closed automatically when this is the
 * last handle sharing an underlying file, that is, once the root logger and
 * every derived logger are closed. When the logger compresses its rotated
 * files, that last close also waits until every compression its rotations
 * queued has finished, and stops the compression thread.
 *
 * A CLOG_NULL, stale or already closed handle is a fatal error. Once the
 * library's destructor has released the table of loggers at the end of the
 * process, every handle is stale.
 */
void clog_close(clog logger);

/**
 * @brief Derive a new logger from an existing one.
 *
 * The derived logger shares the parent's logging target (the file
 * descriptor) and mutex, so one lock serialises every write from the parent
 * and from each derived logger, and they all go to the same destination.
 *
 * The derived logger starts with a snapshot of the parent's fields and
 * minimum log level at the time of the call. After that the two keep their
 * own field maps and level settings, so a change on one isn't visible on the
 * other.
 *
 * If the original file-backed logger was configured to rotate, the rotation
 * belongs to the shared output target: a write through any handle, root or
 * derived, rotates the file when a limit is reached, and rotation goes on as
 * long as any handle stays open, also after the root logger is closed.
 *
 * Free the derived logger with clog_close() once you don't need it. The
 * underlying file stays open until all the handles are closed, meaning the
 * root logger and every derived logger.
 *
 * @param parent Source logger to derive from.
 * @return New derived logger handle, or CLOG_NULL on an allocation failure.
 */
clog clog_derive(clog parent);

/**
 * @brief Block until the logger gives every record in the queue to the
 * operating system.
 *
 * That means the records the queue holds at the time of this call, and the
 * call also hands an aggregation buffer that is only partly full to the
 * operating system. For a synchronous logger (one that isn't async) it does
 * nothing and returns at once. It's safe to call on any handle that is still
 * open, at any time. Another thread can submit a record at the same time as
 * this call but strictly after it, and there's no guarantee the call
 * includes such a record.
 *
 * Two conditions end the call while work is still outstanding, and a caller
 * that uses it as a durability checkpoint has to accept both. First, the
 * async queue can fail to accept the flush request after a rare allocation
 * failure; the call then returns at once rather than wait for an
 * acknowledgement that never comes, because such a wait would hang the
 * caller. A fatal record's pre-drain step takes this same path. Second, a
 * write can stop early with an error that a later attempt can outlast, such
 * as ENOSPC: the bytes the kernel doesn't take stay in the aggregation buffer
 * and the next flush offers them again, which is why the call can return
 * while part of what it flushed is still pending. After any other write
 * error the flush drops those bytes and writes a record naming the loss (see
 * clog_async_cfg_t).
 *
 * @param logger Logger handle.
 */
void clog_flush(clog logger);

/* ========================================================================== */
/*                         LEVEL CONTROL                                      */
/* ========================================================================== */

/**
 * @brief Change the minimum log level. Thread-safe.
 *
 * A level that clog_level_t doesn't name, such as -3 or 12, is refused: the
 * call returns ccol_invalid_args and the level stays as it is. An invalid,
 * stale or closed handle is a fatal error, as for every clog function.
 *
 * @param logger Logger handle.
 * @param level  The new minimum level, from CLOG_TRACE to CLOG_OFF.
 * @return ccol_success when the level is set, or ccol_invalid_args when
 *         clog_level_t doesn't name the value.
 */
ccol_retval_t clog_set_level(clog logger, clog_level_t level);

/** Give the current minimum log level. Thread-safe. */
clog_level_t clog_get_level(clog logger);

/* ========================================================================== */
/*                         OUTPUT FORMAT */
/* ========================================================================== */

/**
 * @brief Log output format.
 *
 * The default is CLOG_FMT_LOGFMT. The format lives on the shared backing
 * store, so every logger handle writing to the same file descriptor (the root
 * logger and every derived logger) shares it, and changing it through any
 * handle takes effect for all of them at once.
 *
 * CLOG_FMT_SYSLOG is only for an fd-based logger (clog_create_from_fd or
 * clog_create_from_fd_mp). Calling clog_set_format() with CLOG_FMT_SYSLOG on
 * a file-backed logger (clog_create_from_file or clog_create_from_file_mp)
 * returns ccol_invalid_args and leaves the format as it is.
 *
 * Every format writes valid UTF-8: a byte that isn't part of a well-formed
 * UTF-8 sequence becomes U+FFFD, and a C1 control (U+0080 to U+009F) becomes
 * the escape \u0080 to \u009f.
 *
 * CLOG_FMT_SYSLOG escapes every control byte of the MSG part, of each
 * SD-PARAM-VALUE and of each backtrace frame with a backslash (\n, \r, \t or
 * \xNN), and a backslash itself as two backslashes, so the escaped text reads
 * back without ambiguity. An SD-PARAM-VALUE also escapes '"' and ']'. The MSG
 * part carries no byte order mark, so it is RFC 5424's MSG-ANY form, whose
 * bytes a receiver must not assume to be UTF-8.
 */
typedef enum {
  CLOG_FMT_LOGFMT = 0, /**< key=value logfmt (default) */
  CLOG_FMT_JSON,       /**< NDJSON; one JSON object per line */
  CLOG_FMT_SYSLOG,     /**< RFC 5424 syslog; for fd-based loggers only (see
                          above) */
} clog_format_t;

/**
 * @brief Change the output format.
 *
 * The format lives on the shared backing store, so every logger handle that
 * shares the same file descriptor sees the change. CLOG_FMT_SYSLOG on a
 * file-backed logger is refused with ccol_invalid_args, and so is a value
 * clog_format_t doesn't name; the format then stays as it is. An invalid,
 * stale or closed handle is a fatal error. Thread-safe.
 *
 * @param logger Logger handle.
 * @param fmt    The output format that you want.
 * @return ccol_success when the format is set, or ccol_invalid_args when
 *         clog_format_t doesn't name the value or the logger can't write
 *         that format.
 */
ccol_retval_t clog_set_format(clog logger, clog_format_t fmt);

/**
 * @brief Give the current output format.
 *
 * @param logger Logger handle.
 * @return The current output format.
 */
clog_format_t clog_get_format(clog logger);

/* ========================================================================== */
/*                         SYSLOG FACILITY                                    */
/* ========================================================================== */

/**
 * @brief Syslog facility codes (RFC 5424 / RFC 3164).
 *
 * Use these codes with clog_set_facility() when the logger's output format is
 * CLOG_FMT_SYSLOG.
 *
 * @par Prerequisites for CLOG_FMT_SYSLOG The fd you give to
 * clog_create_from_fd() or clog_create_from_fd_mp() must be a writable file
 * descriptor that is already connected to a syslog daemon. On Linux that's
 * usually a Unix-domain socket you get like this:
 * @code
 *   int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
 *   struct sockaddr_un sa = { .sun_family = AF_UNIX };
 *   strncpy(sa.sun_path, "/dev/log", sizeof(sa.sun_path) - 1);
 *   if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
 *     return -1;  // close fd if it is open
 *   clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
 *   if (lg == CLOG_NULL) return -1;  // close fd
 *   clog_set_format(lg, CLOG_FMT_SYSLOG);
 * @endcode
 * For SOCK_DGRAM the kernel delivers each write() as one datagram. Keep each
 * message below 2 KiB to stay inside the limits of a typical syslog daemon.
 * An fd-based logger has no log rotation.
 *
 * The facility lives on the shared backing store, so every logger handle
 * writing to the same file descriptor (the root logger and every derived
 * logger) shares it. The default facility is CLOG_SYSLOG_USER.
 */
typedef enum {
  CLOG_SYSLOG_KERN = 0,      /**< Kernel messages */
  CLOG_SYSLOG_USER = 1,      /**< User-level messages (default) */
  CLOG_SYSLOG_MAIL = 2,      /**< Mail system */
  CLOG_SYSLOG_DAEMON = 3,    /**< System daemons */
  CLOG_SYSLOG_AUTH = 4,      /**< Security / authorization messages */
  CLOG_SYSLOG_SYSLOG = 5,    /**< Internal syslogd messages */
  CLOG_SYSLOG_LPR = 6,       /**< Line printer subsystem */
  CLOG_SYSLOG_NEWS = 7,      /**< Network news subsystem */
  CLOG_SYSLOG_UUCP = 8,      /**< UUCP subsystem */
  CLOG_SYSLOG_CRON = 9,      /**< Clock daemon */
  CLOG_SYSLOG_AUTHPRIV = 10, /**< Security / authorization (private) */
  CLOG_SYSLOG_FTP = 11,      /**< FTP daemon */
  CLOG_SYSLOG_LOCAL0 = 16,   /**< Local use 0 */
  CLOG_SYSLOG_LOCAL1 = 17,   /**< Local use 1 */
  CLOG_SYSLOG_LOCAL2 = 18,   /**< Local use 2 */
  CLOG_SYSLOG_LOCAL3 = 19,   /**< Local use 3 */
  CLOG_SYSLOG_LOCAL4 = 20,   /**< Local use 4 */
  CLOG_SYSLOG_LOCAL5 = 21,   /**< Local use 5 */
  CLOG_SYSLOG_LOCAL6 = 22,   /**< Local use 6 */
  CLOG_SYSLOG_LOCAL7 = 23,   /**< Local use 7 */
} clog_syslog_facility_t;

/**
 * @brief Change the syslog facility.
 *
 * It only means something when the output format is CLOG_FMT_SYSLOG. The
 * facility lives on the shared backing store, so every logger handle sharing
 * the same file descriptor sees the change. A value clog_syslog_facility_t
 * doesn't name (12 to 15, or anything outside 0 to 23) is refused with
 * ccol_invalid_args and the facility stays as it is, so every record's PRI
 * stays in the range 0 to 191 that RFC 5424 allows. An invalid, stale or
 * closed handle is a fatal error. Thread-safe.
 *
 * @param logger   Logger handle.
 * @param facility The facility code that you want.
 * @return ccol_success when the facility is set, or ccol_invalid_args when
 *         clog_syslog_facility_t doesn't name the value.
 */
ccol_retval_t clog_set_facility(clog logger, clog_syslog_facility_t facility);

/**
 * @brief Give the current syslog facility.
 *
 * @param logger Logger handle.
 * @return The current facility code.
 */
clog_syslog_facility_t clog_get_facility(clog logger);

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

/**
 * @brief Attach a persistent key=value field to the logger.
 *
 * The field appears in every log line this logger writes after the call.
 * Both the key and the value are copied, so the caller can free them. A call
 * with a key that already exists replaces the value. Thread-safe. A call
 * that fails changes nothing: a new key isn't added and an existing key keeps
 * its value.
 *
 * Like every other function taking a clog handle, it ends the process through
 * _ccol_fatal_err() when the handle is CLOG_NULL, stale or already closed.
 *
 * @param logger Logger handle.
 * @param key    Field name. It must not be NULL or empty, and every character
 *               must be a printable US-ASCII character (0x21-0x7e) other than
 *               '=', ']', '"' and '\'. It also must not name one of the fixed
 *               tokens every log line already carries (ts, level, proc, src,
 *               func, msg, bt and bt_error), because such a key would make a
 *               duplicate key in the output.
 * @param value  Field value. It must not be NULL. Any text is fine, and it
 *               gets quoted when it has to be.
 * @return ccol_success when the field is set; ccol_invalid_args when key or
 *         value is NULL or key breaks the rules above; ccol_not_enough_memory
 *         when the copy of the key or the value can't be allocated.
 */
ccol_retval_t clog_set_field(clog logger, const char *key, const char *value);

/**
 * @brief Remove a field that an earlier call attached.
 *
 * It does nothing if the key isn't there. Thread-safe.
 */
void clog_remove_field(clog logger, const char *key);

/** Remove every attached field. Thread-safe. */
void clog_clear_fields(clog logger);

/* ========================================================================== */
/*                         SIGNALS                                            */
/* ========================================================================== */

/**
 * @brief What the library may do about SIGPIPE and SIGXFSZ, the two signals
 *        that a logger's write can raise.
 *
 * Each enumerator has an explicit value, and any later addition must have
 * one too.
 */
typedef enum {
  /** The default. No logger write ends the process through SIGPIPE or
   * SIGXFSZ, and the library touches the process's signal state only where a
   * pipe or a regular file needs it; see clog_set_signal_policy(). */
  CLOG_SIGNALS_AUTO = 0,
  /** The library never changes the disposition of SIGPIPE or SIGXFSZ, or any
   * signal mask. A write raises whatever the kernel raises for it, and the
   * application's disposition decides what happens. */
  CLOG_SIGNALS_UNTOUCHED = 1,
} clog_signal_policy_t;

/**
 * @brief Choose what the library may do about SIGPIPE and SIGXFSZ, for the
 *        whole process.
 *
 * Two signals can come out of a logger's write, and the default disposition
 * of each one ends the process. A write to a pipe or a socket whose reader is
 * gone fails with EPIPE, and write(2) also raises SIGPIPE. A write to a
 * regular file that reaches the process's file size limit (RLIMIT_FSIZE, set
 * by `ulimit -f` or by systemd's LimitFSIZE= setting) writes what fits and
 * raises SIGXFSZ, and so does every later write to that file. A logger
 * decides what kind its descriptor is once, with fstat(2), when it opens, and
 * a rotation keeps a regular file. What happens then depends on that kind:
 *
 * - A socket is written with send(2) and MSG_NOSIGNAL under either policy,
 *   so it never raises SIGPIPE, and nothing about signals changes.
 * - A pipe or a FIFO, under CLOG_SIGNALS_AUTO: a logger's async writer thread
 *   keeps SIGPIPE blocked in its own signal mask and takes back the SIGPIPE
 *   that its own failed write left pending. Any other thread writing to a
 *   pipe (a synchronous logger, a CLOG_FATAL record, or a record an async
 *   logger writes on the calling thread because its queue refused it) sets
 *   SIGPIPE's disposition to SIG_IGN for the whole process, the first time it
 *   does so.
 * - A regular file, under CLOG_SIGNALS_AUTO: opening the logger, and each
 *   rotation that opens the next file, sets SIGXFSZ's disposition to SIG_IGN
 *   for the whole process, so a write at the file size limit fails with
 *   EFBIG and the logger repairs the file as clog_create_from_file_mp()
 *   describes. The async writer thread and the thread that compresses
 *   rotated files also keep SIGXFSZ blocked in their own signal masks, and
 *   take back the SIGXFSZ that their own failed write left pending.
 * - Under CLOG_SIGNALS_UNTOUCHED: a write raises SIGPIPE or SIGXFSZ as the
 *   kernel decides, and the application's disposition and signal mask decide
 *   what happens. An application that sets a file size limit then must
 *   ignore SIGXFSZ, or block it in every thread that can write a log file,
 *   or the kernel ends the process when a log file reaches the limit.
 * - A terminal, or any descriptor that is neither a pipe, a socket nor a
 *   regular file, raises neither signal, and the library does nothing about
 *   it.
 *
 * The library changes a disposition at most once per signal, and only when
 * it is SIG_DFL at that moment; a handler or an ignore the application
 * installed stays. Like any disposition, SIG_IGN then holds for the rest of
 * the process, and a child started by fork(2) and exec(3) inherits it. A
 * record whose write fails with EPIPE is lost, like a record that meets any
 * other write error the library can't recover from.
 *
 * You may call this at any time and from any thread. The policy governs
 * every write and every open that starts after the call, including those of
 * async writer threads already running: such a thread unblocks SIGPIPE and
 * SIGXFSZ again before its next write under CLOG_SIGNALS_UNTOUCHED, and
 * blocks them again under CLOG_SIGNALS_AUTO. A thread that its creator
 * started with one of the two signals already blocked keeps it blocked under
 * both policies. A SIG_IGN disposition that the library already set under
 * CLOG_SIGNALS_AUTO stays when the policy changes, because the library can't
 * tell whether that disposition is still its own, and the application may
 * already rely on it. If you want the dispositions untouched from the start,
 * call this before the first logger opens.
 *
 * @param policy CLOG_SIGNALS_AUTO or CLOG_SIGNALS_UNTOUCHED.
 * @return ccol_success, or ccol_invalid_args for any other value, which
 *         changes nothing.
 */
ccol_retval_t clog_set_signal_policy(clog_signal_policy_t policy);

/* ========================================================================== */
/*                         THREAD NAME                                        */
/* ========================================================================== */

/**
 * @brief Rename the calling thread, and make its log records carry the new
 *        name.
 *
 * Every record names the thread that wrote it. The logger reads a thread's
 * name once, the first time that thread logs, and keeps it for the thread's
 * life. This function renames the calling thread (prctl(PR_SET_NAME) on
 * Linux, pthread_setname_np() on FreeBSD and macOS, pthread_set_name_np() on
 * OpenBSD) and updates that kept name, so the thread's next record carries
 * the new name. A thread renamed by any other means, pthread_setname_np() for
 * example, after its first record keeps the old name in its records.
 *
 * The function keeps the first 15 bytes of @p thread_name, which is what the
 * Linux kernel stores, on every platform. You may call it before any logger
 * exists and from any thread; it affects only the calling thread.
 *
 * @param thread_name The new name. It must not be NULL or empty.
 * @return ccol_success on success; ccol_invalid_args when @p thread_name is
 *         NULL or empty; ccol_not_permitted when the system refuses the
 *         rename, or on a platform other than Linux, FreeBSD, OpenBSD and
 *         macOS, where the function changes nothing.
 */
ccol_retval_t ccol_set_thread_name(const char *thread_name);

/* ========================================================================== */
/*                         INTERNAL; DO NOT CALL DIRECTLY */
/* ========================================================================== */

/**
 * @brief Internal write function. Use the log_* macros instead.
 *
 * @param logger         Logger handle.
 * @param level          Severity level.
 * @param file           Source file (__FILE__).
 * @param line           Source line (__LINE__).
 * @param func           Function name (__func__).
 * @param with_backtrace When true, this adds a stack trace.
 * @param fmt            printf-style format string.
 */
void _clog_write(clog logger, clog_level_t level, const char *file, int line,
                 const char *func, bool with_backtrace, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));

/* ========================================================================== */
/*                         LOGGING MACROS                                     */
/* ========================================================================== */

/** @defgroup log_macros Logging macros
 *
 * Each macro takes a clog handle followed by a printf-style format string and
 * optional arguments. The file name, the line number and the function name
 * are captured automatically. clog_error and clog_fatal also add a
 * backtrace.
 *
 * The format string is the first argument of each macro's variadic part, so
 * a call with a format and nothing after it, such as clog_info(l, "ready"),
 * is standard C11: it needs neither the GNU comma swallow nor any other
 * extension, and it compiles under -std=c11 -pedantic-errors.
 *
 * @{
 */

#define clog_trace(l, ...) \
  _clog_write((l), CLOG_TRACE, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

#define clog_debug(l, ...) \
  _clog_write((l), CLOG_DEBUG, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

#define clog_info(l, ...) \
  _clog_write((l), CLOG_INFO, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

#define clog_warn(l, ...) \
  _clog_write((l), CLOG_WARN, __FILE__, __LINE__, __func__, false, __VA_ARGS__)

/** Logs at the ERROR level and adds a backtrace. */
#define clog_error(l, ...) \
  _clog_write((l), CLOG_ERROR, __FILE__, __LINE__, __func__, true, __VA_ARGS__)

/** Logs at the ALERT level and adds a backtrace. */
#define clog_alert(l, ...) \
  _clog_write((l), CLOG_ALERT, __FILE__, __LINE__, __func__, true, __VA_ARGS__)

/**
 * Logs at the FATAL level and adds a backtrace, then makes the process die
 * by calling exit(EXIT_FAILURE). exit() runs the library's exit drain, which
 * lets every gzip compression that a rotation of any logger queued finish
 * within its budget, so no rotated file is left half compressed. That drain
 * ends within its budget whatever the threads of other loggers do. One budget
 * of 5 seconds, counted from the start of the first fatal call, bounds the
 * whole stop, the fatal record included: on an async logger the drain of its
 * queue and batch, and every write of the record to an output that isn't a
 * regular file (a pipe, a socket, a terminal). An output that takes nothing
 * more in that time loses the record; a line on stderr says so, unless stderr
 * is that same output, and the process exits anyway. A write to a regular file
 * waits on the disk and has no bound. While a fatal call waits for the logger,
 * the async writer thread takes on no new work, and the wait may run up to 100
 * milliseconds past the budget so that a write already in progress on the
 * logger can end and hand it over. This macro never returns to the caller.
 * clog_fatal differs from every other log_* macro in that it goes around the
 * logger's min_level filter. The message is always written, so the reason the
 * process stopped is never silently hidden.
 */
#define clog_fatal(l, ...) \
  _clog_write((l), CLOG_FATAL, __FILE__, __LINE__, __func__, true, __VA_ARGS__)

/** @} */

#pragma GCC visibility pop
