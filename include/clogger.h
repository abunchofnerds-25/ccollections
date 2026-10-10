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
 * The logger adds a backtrace for the ERROR, ALERT and FATAL levels only. It
 * writes the backtrace as tab-indented continuation lines that do not start
 * with "ts=", so that a log aggregator can group them with their parent record:
 *   \t#0 ./bin(fn+0x1a) [0x7f...]
 *   \t#1 ./bin(main+0x42) [0x7f...]
 *
 * Thread safety:
 *   All public functions are fully thread-safe for concurrent calls that use
 *   still-open handles. One lock serialises the writes and the log rotation for
 *   one output target, and the root logger and every logger derived from it
 *   share that lock (see clog_derive()). Each logger has its own structured
 *   fields (clog_set_field() and the related functions) and its own minimum log
 *   level (clog_set_level() and clog_get_level()), and the library synchronises
 *   both of them independently of that lock, so a change to either one never
 *   contends with a write in progress on a sibling logger that shares the same
 *   target.
 *
 *   The text above does not cover clog_close(). As for every other handle-based
 *   type in this library, a safe close of a handle is the caller's
 *   responsibility, and the library does not synchronise it for you: a close is
 *   safe only when no other thread holds or uses that same handle. A close of
 *   one handle is safe even when a sibling handle (the root or a derived
 *   logger) writes to the shared target at the same time, because the sibling
 *   is a different handle that is still open.
 *
 * Log rotation:
 *   Only file-backed loggers (clog_open_file_mp) have log rotation. Size
 *   rotation starts after the write that crosses max_file_size, and time
 *   rotation starts on the first write after the interval ends. The library
 *   renames a rotated file to <path>.<YYYYMMDDHHMMSS>[_N], and every rotation
 *   takes a name above every rotated name before it. The new live file gets the
 *   permission bits, the owner and the group of the file that it replaces. If
 *   more than max_rotated_files generations exist, the library deletes the
 *   oldest ones. All of this happens in the directory in which the logger
 *   opened the file, also for a relative path after the process changes its
 *   working directory. When compress_rotated is true, the library also
 *   compresses the rotated file with gzip and makes a .gz file. A background
 *   thread of the logger does that, one file at a time, so neither a logging
 *   thread nor the async writer thread ever waits for a compression. A deletion
 *   pass keeps the newest max_rotated_files generations and deletes every older
 *   one, except an older one whose compression is running, which the library
 *   deletes as soon as that compression ends, so at most max_rotated_files
 *   generations plus the one under compression stay on disk. The last
 *   clog_close() of the file finishes every queued compression before it
 *   returns, and a fatal record waits for them within the budget of the exit
 *   drain (see "Process exit"). A compression writes <rotated>.gz.tmp and
 *   renames it to <rotated>.gz only when the output is complete, so a .gz file
 *   always holds a whole gzip stream. The .gz file gets the permission bits of
 *   its source, and its owner and group where the process may give them, as the
 *   new live file of a rotation does. A rotated file whose <rotated>.gz.tmp
 *   name does not fit in the directory stays uncompressed, and a CLOG_WARN
 *   record in the new live file says so. An open of a compressing logger
 *   removes the .gz.tmp files that a process which stopped left, removes a .gz
 *   file beside its own source, and queues every uncompressed rotated file for
 *   compression.
 *
 * Process exit:
 *   A process can end with exit() or a return from main() while a logger is
 *   still open. An exit handler that the library registers when it loads then
 *   delivers what every async logger holds, as a fatal record does, and waits
 *   for the queued compressions, all within one budget of 5 seconds. The budget
 *   also bounds every lock that the drain takes: a logger whose lock another
 *   thread holds past it is skipped, and the records that it still holds can be
 *   lost. A compression that the budget cuts off keeps its source, and the next
 *   open compresses it. Every write of the drain is bounded as well, on a pipe,
 *   a socket and a terminal too, and the drain takes the lock of a logger only
 *   when that logger has a queue or a compression to drain, so a thread that
 *   blocks for ever inside a write of a synchronous logger delays no exit and
 *   no fatal record. A record that an async logger receives after its drain,
 *   from an exit handler that runs after the one of the library or from a
 *   destructor, is written synchronously by the thread that logs it, and a
 *   clog_close() from there works. A logger whose drain the budget cut off
 *   keeps its queue, and such a record can then be lost. _exit(), a signal and
 *   abort() run no exit handler.
 *
 *   Build with -rdynamic to get resolved symbols in backtraces.
 */

#include <stdbool.h>
#include <stdint.h> /* int64_t, uint64_t */

#include "common.h"

/* Every declaration from here to the end of this header is part of the public
 * Application Binary Interface (ABI) of libccollections, and the shared library
 * exports all of them. The build of the library uses -fvisibility=hidden, so a
 * function or an object that one of these blocks does not cover stays internal
 * to the library: its name is absent from the dynamic symbol table, the
 * application that links against the library cannot interpose it, and a symbol
 * with the same name in that application cannot collide with it. */
#pragma GCC visibility push(default)

/* Remove any macro definition of clog from <complex.h> */
#ifdef clog
#undef clog
#endif

/* ========================================================================== */
/*                         LOG LEVELS                                         */
/* ========================================================================== */

/*
 * Each enumerator below deliberately has an explicit numeric value instead of
 * one that C's auto-increment chooses, because these values are load-bearing
 * array indices (see _LEVEL_STR[] and _SYSLOG_SEVERITY[] in clogger.c). An
 * insertion in the middle without an explicit value would silently shift the
 * numeric value of every level after it and misalign both arrays, with no
 * diagnostic from the compiler. Any future addition must also have an explicit
 * value.
 */
typedef enum {
  CLOG_TRACE = 0,
  CLOG_DEBUG = 1,
  CLOG_INFO = 2,
  CLOG_WARN = 3,
  CLOG_ERROR = 4,
  CLOG_ALERT = 5, /**< You must act immediately. This maps to syslog
                     severity 1 */
  CLOG_FATAL = 6, /**< Adds a backtrace and stops the process with a call to
                 exit(EXIT_FAILURE). This level ignores min_level: the library
                 always writes the fatal message, whatever the level setting of
                 the logger is. */
  CLOG_OFF = 7    /**< As min_level, this turns off all output */
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
 * Pass a pointer to this struct to clog_open_file_mp().
 *
 * A zero or negative value for max_file_size or max_rotated_files, and a zero
 * rotation_interval_us, falls back to the CLOG_DEFAULT_* constant for that
 * member. You cannot turn off the deletion of old rotated files, so a caller
 * that does not set max_rotated_files never collects an unbounded number of
 * rotated files on disk.
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
 * clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, &cfg, NULL, NULL);
 * @endcode
 *
 * When compress_rotated is true, the library compresses each rotated file with
 * zlib (gzip format) after the rotation, which gives a
 * "<path>.<YYYYMMDDHHMMSS>[_N].gz" file. On success, the library removes the
 * original uncompressed file. If a compression fails, the uncompressed file
 * stays on disk, so no data is lost. If the directory cannot hold the name
 * "<rotated>.gz.tmp", no compression of that file can succeed; the file then
 * stays uncompressed, and the library writes a CLOG_WARN record about it into
 * the new live file. Each other failure is silent. Standard tools (gunzip,
 * zcat, gzip -d) can decompress the compressed files.
 *
 * The logger starts a background thread at its first compression, which does
 * the compressions one at a time, the oldest first. The rotation only puts the
 * file in a queue, so the thread that logs and the async writer thread never
 * wait for gzip. The last clog_close() of the file waits until each queued
 * compression is complete. A program that links the static library must also
 * link with -lz (pkg-config --static adds it); the shared library has its own
 * dependency on zlib.
 */
/*
 * The size member is the fixed-width type int64_t, and the interval member is a
 * uint64_t count of microseconds, the unit of every duration of the library,
 * instead of the off_t and time_t types that a size and a time otherwise use.
 * This is deliberate. Feature-test macros (_FILE_OFFSET_BITS and _TIME_BITS)
 * choose the widths of off_t and time_t, and those macros belong to the
 * translation unit that the compiler compiles at that moment, so a struct built
 * from those types has a layout that the application and the library can
 * disagree about whenever a build compiles the two sides with different
 * settings. An application that gets that layout wrong gives a config in which
 * every member after the first comes from the wrong offset, and such a config
 * silently rotates on the wrong schedule and turns on options that the caller
 * never asked for. A fixed-width member has one layout for each target,
 * whatever the feature macros say, which is why this struct means the same
 * thing on both sides of the ABI.
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
 * Give a pointer to this struct to clog_open_fd_mp() or clog_open_file_mp() (or
 * to their wrappers without the _mp suffix), and the new logger, and every
 * logger later derived from it, logs asynchronously. The thread that calls only
 * formats its own message and captures its own identity, timestamp and fields;
 * it then hands the rest of the work (the escape step, the serialization of the
 * fields, and the write() call itself) to a dedicated writer thread. The writer
 * thread collects the records from every handle that shares the same target
 * into one buffer, and it flushes that buffer when the records reach
 * flush_buffer_size, or when flush_interval_us passes.
 *
 * Unlike clog_rotation_cfg_t, a NULL async_cfg and a non-NULL pointer to an
 * all-zero clog_async_cfg_t are NOT the same thing: ANY non-NULL pointer turns
 * on async mode, and each field with a zero value falls back to its documented
 * default below. Only a literal NULL turns async mode off; this struct has no
 * "enabled" flag that you can leave false.
 *
 * These values never apply to CLOG_FMT_SYSLOG, because that format does no
 * batching: each record and each backtrace frame keeps its own write() call,
 * one for each UDP datagram. They do not apply to a message-oriented socket
 * either (SOCK_DGRAM, SOCK_SEQPACKET or SOCK_RDM, such as a UDP socket or a
 * Unix datagram socket). There every record, in any format, goes out with its
 * own write(), and so as one message of its own, exactly as a synchronous
 * logger sends it. A record larger than the socket can carry fails alone: the
 * library writes a "log record truncated" record in its place, and the records
 * after it are still delivered. A socket whose receiver has no room makes the
 * write wait, both where the system blocks the send and where it refuses it
 * with ENOBUFS (FreeBSD does that for a Unix datagram socket): the logger sends
 * again until the message fits, and CLOG_FATAL and the drain at exit wait only
 * within their time budget.
 *
 * A flush whose write fails with an error that a later attempt can outlast
 * (ENOSPC, EDQUOT, EFBIG, ENOBUFS, ENOMEM, or EAGAIN or EINTR that ends a wait)
 * keeps the undelivered bytes for the next flush. Any other write error, such
 * as EPIPE, ECONNRESET or EBADF, fails the same way on every retry, so the
 * flush drops the undelivered bytes and writes a "log record truncated:
 * undelivered batch dropped (records=<n> bytes=<m>): write error <errno>"
 * record in their place. The last flush of clog_close(), and the last attempt
 * of a fatal record, have no later retry, so what they cannot deliver is named
 * the same way.
 *
 * CLOG_FATAL always goes around the async queue and writes synchronously, and
 * it first drains everything that is already in the queue, because the process
 * stops immediately after it. An exit() or a return from main() with the logger
 * still open drains the queue in the same way, within a bounded time (see
 * "Process exit" at the top of this header).
 */
typedef struct clog_async_cfg {
  /** Capacity of the message queue. 0 -> an unbounded queue that never blocks a
   * caller that sends. A value > 0 -> a bounded queue of this many messages; a
   * full bounded queue BLOCKS the thread that calls until space becomes free,
   * and the library never silently drops a message. */
  size_t queue_size;
  /** Bytes. The writer thread flushes when its aggregation buffer reaches
   * this size. 0 -> CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE. */
  size_t flush_buffer_size;
  /** Microseconds. The writer thread flushes at least this often, even when the
   * buffer does not reach flush_buffer_size. 0 ->
   * CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US. Any value of the type is valid:
   * UINT64_MAX, or any interval too long for the clock to reach, means that
   * only flush_buffer_size, clog_flush() and clog_close() start a flush, and
   * the idle writer thread then sleeps until a record arrives. */
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
 * generation counter into a uint64_t. It is not a pointer: never cast it to
 * `void *` or from `void *`, and never compare two handles after a cast of
 * either one to a pointer. Both `if (!lg)` and `lg == CLOG_INVALID` work as you
 * expect, because CLOG_INVALID is numerically zero.
 *
 * Get a handle with clog_open_fd_mp() or clog_open_file_mp(), and free it with
 * clog_close().
 */
typedef uint64_t clog;

/** @brief Sentinel value that means "no logger". It is numerically zero. */
#define CLOG_INVALID ((clog)0)

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

/**
 * @brief Create a logger that writes to a file descriptor that is already open,
 * with custom memory management procs.
 *
 * The logger does not own the file descriptor, so clog_close() does NOT close
 * it. An fd-based logger has no log rotation. The logger does every dynamic
 * memory operation with the memory management procs that the caller gives.
 *
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, ...).
 * @param min_level The logger silently drops a message below this level.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or CLOG_INVALID on an allocation failure, for
 *         invalid mprocs, or when fd is closed, read-only or invalid for
 *         another reason.
 */
clog clog_open_fd_mp(int fd, clog_level_t min_level,
                     const clog_async_cfg_t *async_cfg,
                     ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Create a logger that writes to a file descriptor that is already open.
 *
 * The logger does not own the file descriptor, so clog_close() does NOT close
 * it. An fd-based logger has no log rotation.
 *
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, ...).
 * @param min_level The logger silently drops a message below this level.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @return New logger handle, or CLOG_INVALID on an allocation failure, or when
 *         fd is closed, read-only or invalid for another reason.
 */
static inline __attribute__((always_inline)) clog clog_open_fd(
    int fd, clog_level_t min_level, const clog_async_cfg_t *async_cfg) {
  return clog_open_fd_mp(fd, min_level, async_cfg, NULL);
}

/**
 * @brief Create a file-backed logger with optional log rotation, and with
 * custom memory management procs.
 *
 * The library creates the file if it does not exist (mode 0644, which the umask
 * of the process narrows), and opens it in append mode if it exists. The logger
 * does every dynamic memory operation with the memory management procs that the
 * caller gives.
 *
 * A rotation creates each new live file with the permission bits of the file
 * that it replaces, whatever the umask, and gives it the owner and the group of
 * that file where the process may, as logrotate does. When the process cannot
 * give it the group, the group bits are cleared, so that no other group gains
 * access. A log that its operator restricted, to 0600 or 0640 for example,
 * therefore stays restricted in every generation.
 *
 * When rotation is on, the logger keeps the directory of the file open for its
 * whole life, and every rotation, deletion and compression works in that
 * directory, so a relative path keeps naming the same file after the process
 * changes its working directory.
 *
 * One file has one backing store in a process whenever a logger of it rotates.
 * When a logger of this process already writes the same file (the same
 * directory and the same last component of the path) and either of the two
 * rotates, this call joins that logger as clog_derive() does: the new handle
 * has its own level and fields and shares the target. It gives CLOG_INVALID
 * when the rotation configuration, the async configuration or the allocator
 * differs from the one of that logger, or when only one of the two rotates. Two
 * opens of one file that both do not rotate stay independent.
 *
 * The time rotation interval of a file that already holds data counts from the
 * moment at which that file began, not from this call. That moment is the stamp
 * of the newest rotated generation on disk, because the rotation that made it
 * also created the current file; without one, it is the birth time of the file
 * where the filesystem reports it, and the last modification time otherwise. A
 * process that runs for less than the interval, or restarts often, therefore
 * still rotates. The interval of an empty or a new file counts from this call,
 * and so does an interval whose starting moment lies in the future.
 *
 * @param path      Path of the destination log file.
 * @param min_level The logger silently drops a message below this level.
 * @param cfg       Rotation config, or NULL to turn off rotation.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or CLOG_INVALID on a failure, for example a bad
 *         path, an allocation error, or a configuration that differs from the
 *         one of a rotating logger of this process on the same file.
 */
clog clog_open_file_mp(const char *path, clog_level_t min_level,
                       const clog_rotation_cfg_t *cfg,
                       const clog_async_cfg_t *async_cfg,
                       ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Create a file-backed logger with optional log rotation.
 *
 * The library creates the file if it does not exist (mode 0644, which the umask
 * of the process narrows), and opens it in append mode if it exists. See
 * clog_open_file_mp() for the mode of the files that a rotation creates.
 *
 * @param path      Path of the destination log file.
 * @param min_level The logger silently drops a message below this level.
 * @param cfg       Rotation config, or NULL to turn off rotation.
 * @param async_cfg Async logging config, or NULL for synchronous logging
 *                  (see clog_async_cfg_t).
 * @return New logger handle, or CLOG_INVALID on a failure, for example a bad
 *         path or an allocation error.
 */
static inline __attribute__((always_inline)) clog clog_open_file(
    const char *path, clog_level_t min_level, const clog_rotation_cfg_t *cfg,
    const clog_async_cfg_t *async_cfg) {
  return clog_open_file_mp(path, min_level, cfg, async_cfg, NULL);
}

/**
 * @brief Flush the logger, close it, and free all the resources that it owns.
 *
 * Do not use the handle after this call. The caller must make sure that no
 * other thread uses, or closes, this exact handle at the same time; see the
 * "Thread safety" note above in this header for the scope of that requirement.
 * The library closes the file descriptor automatically when this is the last
 * handle that shares an underlying file, which is the state after a close of
 * the root logger and of every derived logger. When the logger compresses its
 * rotated files, that last close also waits until every compression that its
 * rotations queued has finished, and stops the compression thread.
 */
void clog_close(clog logger);

/**
 * @brief Derive a new logger from an existing one.
 *
 * The derived logger shares the logging target (the file descriptor) and the
 * mutex of the parent, so the same lock serialises all the writes from the
 * parent and from every derived logger, and they all go to the same
 * destination.
 *
 * The derived logger starts with a snapshot of the fields and of the minimum
 * log level of the parent at the time of the call. After that, the two loggers
 * keep independent field maps and level settings, so a change on one logger is
 * not visible on the other.
 *
 * If the caller configured rotation on the original file-backed logger, the
 * root logger continues to manage the log rotation, and a derived logger gets
 * the benefit of it transparently.
 *
 * Free the derived logger with clog_close() when you do not need it any more.
 * The library keeps the underlying file open until it closes all the handles,
 * which are the root logger and every derived logger.
 *
 * @param parent Source logger to derive from.
 * @return New derived logger handle, or CLOG_INVALID on an allocation failure.
 */
clog clog_derive(clog parent);

/**
 * @brief Block until the logger gives every record in the queue to the
 * operating system.
 *
 * The queue holds the records that exist at the time of this call, and the call
 * also gives the operating system an aggregation buffer that is only partly
 * full. For a synchronous logger (one that is not async) this call does nothing
 * and returns immediately. It is safe to call on any still-open handle at any
 * time. Another thread can submit a record at the same time as this call but
 * strictly after it, and there is no guarantee that the call includes such a
 * record.
 *
 * Two conditions end the call while work is still outstanding, and a caller
 * that uses the call as a durability checkpoint must accept both of them.
 * First, the async queue can fail to accept the flush request, after a rare
 * allocation failure; the call then returns immediately instead of waiting for
 * an acknowledgement that never arrives, because such a wait hangs the caller.
 * The pre-drain step of a fatal record takes this same path. Second, a write
 * can stop early with an error that a later attempt can outlast, such as
 * ENOSPC: the bytes that the kernel does not take stay in the aggregation
 * buffer, and the next flush offers them again, which is why the call can
 * return while part of what it flushed is still pending. After any other write
 * error the flush drops those bytes and writes a record that names the loss
 * (see clog_async_cfg_t).
 *
 * @param logger Logger handle.
 */
void clog_flush(clog logger);

/* ========================================================================== */
/*                         LEVEL CONTROL                                      */
/* ========================================================================== */

/** Change the minimum log level. Thread-safe. */
void clog_set_level(clog logger, clog_level_t level);

/** Give the current minimum log level. Thread-safe. */
clog_level_t clog_get_level(clog logger);

/* ========================================================================== */
/*                         OUTPUT FORMAT */
/* ========================================================================== */

/**
 * @brief Log output format.
 *
 * The default is CLOG_FMT_LOGFMT. The format lives on the shared backing store,
 * so every logger handle that writes to the same file descriptor (the root
 * logger and every derived logger) shares it, and a change of the format
 * through any handle takes effect immediately for all of them.
 *
 * You can use CLOG_FMT_SYSLOG only for an fd-based logger (clog_open_fd or
 * clog_open_fd_mp). A call to clog_set_format() with CLOG_FMT_SYSLOG on a
 * file-backed logger (clog_open_file or clog_open_file_mp) does nothing and
 * reports nothing, and the format stays as it is.
 *
 * CLOG_FMT_SYSLOG escapes every control byte of the MSG part, of each
 * SD-PARAM-VALUE and of each backtrace frame with a backslash (\n, \r, \t or
 * \xNN), and a backslash itself as two backslashes, so the escaped text reads
 * back unambiguously. An SD-PARAM-VALUE also escapes '"' and ']', and it is
 * always valid UTF-8: a byte that is not part of a well-formed UTF-8 sequence
 * becomes U+FFFD. The MSG part carries no byte order mark, so it is the MSG-ANY
 * form of RFC 5424, whose bytes a receiver must not assume to be UTF-8.
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
 * The shared backing store holds the format, so the change is visible to every
 * logger handle that shares the same file descriptor. CLOG_FMT_SYSLOG on a
 * file-backed logger does nothing and reports nothing. Thread-safe.
 *
 * @param logger Logger handle.
 * @param fmt    The output format that you want.
 */
void clog_set_format(clog logger, clog_format_t fmt);

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
 * Use these codes with clog_set_facility() when the output format of the logger
 * is CLOG_FMT_SYSLOG.
 *
 * @par Prerequisites for CLOG_FMT_SYSLOG The fd that you give to clog_open_fd()
 * or clog_open_fd_mp() must be a writable file descriptor that already has a
 * connection to a syslog daemon. On Linux this is usually a Unix-domain socket
 * that you get like this:
 * @code
 *   int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
 *   struct sockaddr_un sa = { .sun_family = AF_UNIX };
 *   strncpy(sa.sun_path, "/dev/log", sizeof(sa.sun_path) - 1);
 *   if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
 *     return -1;  // close fd if it is open
 *   clog lg = clog_open_fd(fd, CLOG_INFO, NULL);
 *   if (lg == CLOG_INVALID) return -1;  // close fd
 *   clog_set_format(lg, CLOG_FMT_SYSLOG);
 * @endcode
 * For SOCK_DGRAM the kernel delivers each write() as one datagram. Keep each
 * message below 2 KiB to stay inside the limits of a typical syslog daemon. An
 * fd-based logger has no log rotation.
 *
 * The shared backing store holds the facility, so every logger handle that
 * writes to the same file descriptor (the root logger and every derived logger)
 * shares it. The default facility is CLOG_SYSLOG_USER.
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
 * This has a meaning only when the output format is CLOG_FMT_SYSLOG. The shared
 * backing store holds the facility, so the change is visible to every logger
 * handle that shares the same file descriptor. Thread-safe.
 *
 * @param logger   Logger handle.
 * @param facility The facility code that you want.
 */
void clog_set_facility(clog logger, clog_syslog_facility_t facility);

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
 * The field is in every log line that this logger writes after the call. The
 * library copies both the key and the value internally, so the caller can free
 * them. A call with a key that exists replaces the value. Thread-safe.
 *
 * @param logger Logger handle.
 * @param key    Field name. It must not be empty, and every character must be a
 *               printable US-ASCII character (0x21-0x7e) other than '=', ']',
 *               '"' and '\'. The library silently ignores a key that breaks
 *               these rules. It also ignores a key that names one of the fixed
 *               tokens that every log line already carries (ts, level, proc,
 *               src, func, msg, bt and bt_error), because such a key makes a
 *               duplicate key in the output.
 * @param value  Field value. It is any text, which the library quotes if it
 *               must.
 */
void clog_set_field(clog logger, const char *key, const char *value);

/**
 * @brief Remove a field that an earlier call attached.
 *
 * This does nothing if the key is not present. Thread-safe.
 */
void clog_remove_field(clog logger, const char *key);

/** Remove every attached field. Thread-safe. */
void clog_clear_fields(clog logger);

/* ========================================================================== */
/*                         SIGPIPE                                            */
/* ========================================================================== */

/**
 * @brief What the library may do about SIGPIPE when a logger writes to a
 *        pipe whose reader is gone.
 *
 * Each enumerator has an explicit value, and a later addition must have one
 * too.
 */
typedef enum {
  /** The default. A write to a pipe or a socket never raises SIGPIPE, and the
   * library touches the signal state of the process only where a pipe needs it;
   * see clog_set_sigpipe_policy(). */
  CLOG_SIGPIPE_AUTO = 0,
  /** The library never changes the disposition of SIGPIPE or any signal mask. A
   * write to a pipe whose reader is gone raises SIGPIPE, and the disposition of
   * the application decides what happens. */
  CLOG_SIGPIPE_UNTOUCHED = 1,
} clog_sigpipe_policy_t;

/**
 * @brief Choose what the library may do about SIGPIPE, for the whole process.
 *
 * A write to a pipe or a socket whose reader is gone fails with EPIPE, and
 * write(2) also raises SIGPIPE, whose default disposition ends the process. A
 * logger decides the kind of its descriptor once, with fstat(2), when it opens,
 * and a rotation keeps a regular file. What happens then depends on that kind:
 *
 * - A regular file, a terminal, or any descriptor that is neither a pipe nor a
 *   socket cannot raise SIGPIPE, and the library does nothing about it.
 * - A socket is written with send(2) and MSG_NOSIGNAL, under either policy, so
 *   it never raises SIGPIPE, and nothing about signals changes.
 * - A pipe or a FIFO, under CLOG_SIGPIPE_AUTO: the async writer thread of a
 *   logger keeps SIGPIPE blocked in its own signal mask, and takes back the
 *   SIGPIPE that its own failed write left pending. Any other thread that
 *   writes to a pipe (a synchronous logger, a CLOG_FATAL record, or a record
 *   that an async logger writes on the calling thread because its queue refused
 *   it) sets the disposition of SIGPIPE to SIG_IGN for the whole process, the
 *   first time it does so. The library makes that change at most once, and only
 *   when the disposition is SIG_DFL at that moment; a handler or an ignore that
 *   the application installed stays. Like any disposition, SIG_IGN then holds
 *   for the rest of the process, and a child that fork(2) and exec(3) start
 *   inherits it.
 * - A pipe or a FIFO, under CLOG_SIGPIPE_UNTOUCHED: the write raises SIGPIPE,
 *   and the disposition and the signal mask of the application decide what
 *   happens.
 *
 * A record whose write fails with EPIPE is lost, like a record that meets any
 * other write error that the library cannot recover from.
 *
 * You may call this at any time, from any thread. The policy governs every
 * write that starts after the call, including those of async writer threads
 * that already run: such a thread unblocks SIGPIPE again before its next write
 * to a pipe under CLOG_SIGPIPE_UNTOUCHED, and blocks it again under
 * CLOG_SIGPIPE_AUTO. A writer thread that its creator started with SIGPIPE
 * already blocked keeps that mask under both policies. A disposition of SIG_IGN
 * that the library already set under CLOG_SIGPIPE_AUTO stays in place when the
 * policy changes, because the library cannot tell whether that disposition is
 * still its own, and the application may already rely on it. To keep the
 * disposition untouched from the start, call this before the first write to a
 * pipe.
 *
 * @param policy CLOG_SIGPIPE_AUTO or CLOG_SIGPIPE_UNTOUCHED.
 * @return ccol_success, or ccol_invalid_args for any other value, which changes
 *         nothing.
 */
ccol_retval_t clog_set_sigpipe_policy(clog_sigpipe_policy_t policy);

/* ========================================================================== */
/*                         THREAD NAME                                        */
/* ========================================================================== */

/**
 * @brief Rename the calling thread, and make its log records carry the new
 *        name.
 *
 * Every record names the thread that wrote it. The logger reads the name of a
 * thread once, the first time that thread logs, and keeps it for the life of
 * the thread. This function renames the calling thread (prctl(PR_SET_NAME) on
 * Linux, pthread_setname_np() on FreeBSD and macOS) and updates that kept name,
 * so the next record of the thread carries the new name. A thread renamed by
 * any other means, such as pthread_setname_np(), after its first record keeps
 * the old name in its records.
 *
 * The function keeps the first 15 bytes of @p thread_name, which is what the
 * Linux kernel stores, on every platform. You may call it before any logger
 * exists, and from any thread; it affects only the calling thread.
 *
 * @param thread_name The new name. It must not be NULL or empty.
 * @return ccol_success on success; ccol_invalid_args when @p thread_name is
 *         NULL or empty; ccol_not_permitted when the system refuses the rename,
 *         or on a platform other than Linux, FreeBSD and macOS, where the
 *         function changes nothing.
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
 * Each macro takes a clog handle, followed by a printf-style format string and
 * optional arguments. The macro captures the file name, the line number and the
 * function name automatically. clog_error and clog_fatal also add a
 * backtrace.
 *
 * The format string is the first argument of the variadic part of each macro,
 * so a call with a format and nothing after it, such as clog_info(l,
 * "ready"), is standard C11: it needs neither the GNU comma swallow nor any
 * other extension, and it compiles under -std=c11 -pedantic-errors.
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
 * Logs at the FATAL level and adds a backtrace, then stops the process with a
 * call to exit(EXIT_FAILURE). exit() runs the exit drain of the library, which
 * lets every gzip compression that a rotation of any logger has queued finish
 * within its budget, so that no rotated file is left half compressed. That
 * drain ends within its budget whatever the threads of other loggers do.
 * One budget of 5 seconds, from the start of the first fatal call, bounds the
 * whole stop, including the fatal record itself: on an async logger the drain
 * of its queue and batch, and every write of the record to an output that is
 * not a regular file (a pipe, a socket, a terminal). An output that takes
 * nothing more in that time loses the record; a line on stderr says so, unless
 * stderr is that same output, and the process stops anyway. A write to a
 * regular file waits on the disk and keeps no bound. The writer thread of an
 * async logger takes on no new work while a fatal call waits for the logger,
 * and that wait may run up to 100 milliseconds past the budget, so that a write
 * which is in progress on the logger ends and hands it over.
 * This macro never returns to the caller.
 * clog_fatal is different from every other log_* macro: it goes around the
 * min_level filter of the logger. The library always writes the message, so it
 * never silently hides the reason that the process stopped.
 */
#define clog_fatal(l, ...) \
  _clog_write((l), CLOG_FATAL, __FILE__, __LINE__, __func__, true, __VA_ARGS__)

/** @} */

/* ========================================================================== */
/*                         UNIT TEST INTERNALS                                */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Arm (or disarm) an artificial delay inside the internal
 *        gzip-compression step of the log rotation, for a test.
 *
 * When this hook is armed (delay_us != 0), the compression step of the next
 * rotation sleeps for delay_us microseconds, immediately after it creates its
 * ".gz" destination file on disk and before it writes any content to that file.
 * This deterministically widens the window inside which the deletion pass of a
 * concurrent rotation must not be able to delete that destination. This call
 * also resets the state that clog_test_gz_dest_opened() reports. It has no
 * effect outside a RUNNING_UNIT_TESTS build.
 *
 * @param delay_us Microseconds to sleep, or 0 to disarm.
 */
void clog_test_set_pending_compress_delay_us(unsigned int delay_us);

/**
 * @brief Report whether the compression step created its ".gz" destination file
 *        on disk, for a test.
 *
 * The most recent call to clog_test_set_pending_compress_delay_us() arms that
 * compression step. A test spin-polls this function, so that it can wait
 * deterministically until the compressed destination of a concurrent rotation
 * truly exists on disk before it starts a second rotation that races against
 * the first one, instead of using a fixed sleep and hoping that the timing is
 * correct.
 *
 * @return true after the library creates the destination file for the delay
 *         that the most recent call armed. A call to
 *         clog_test_set_pending_compress_delay_us() resets it to false.
 */
bool clog_test_gz_dest_opened(void);

/**
 * @brief Hold or release every gzip compression of a rotated file, for a test.
 *
 * While the gate is held, each compression pauses right after it creates its
 * ".gz" destination, until the gate is released. The pause gives up after 30
 * seconds, so that a test whose expectation fails does not hang. Holding the
 * gate also resets the state that clog_test_gz_dest_opened() reports.
 *
 * @param hold true to hold the gate, false to release it.
 */
void clog_test_hold_compressions(bool hold);

/**
 * @brief Report how many gzip compressions ran to their end, whatever their
 *        outcome, since the process started, for a test.
 */
size_t clog_test_compressions_finished(void);

/**
 * @brief Hold or release the recovery of unfinished compressions that the open
 *        of a compressing logger runs, for a test.
 *
 * While the gate is held, each recovery pauses before it takes the mutex of its
 * logger, until the gate is released. The pause gives up after 3 seconds, so
 * that a recovery that runs where no test can release it does not hang. Holding
 * the gate also resets the state that clog_test_compression_recovery_entered()
 * reports.
 *
 * @param hold true to hold the gate, false to release it.
 */
void clog_test_hold_compression_recovery(bool hold);

/**
 * @brief Report whether a recovery reached the gate that
 *        clog_test_hold_compression_recovery() holds, for a test.
 */
bool clog_test_compression_recovery_entered(void);

/**
 * @brief Report how many times the timed wait of an async writer thread
 *        ended with no job, since the process started, for a test.
 */
size_t clog_test_writer_timeout_wakeups(void);

/**
 * @brief Reset the counter of real rotation attempts (_rotate() calls) to zero,
 *        for a test.
 *
 * A test uses this to verify the retry policy for a rotation that fails again
 * and again: the library must retry such a rotation with a bounded backoff
 * instead of trying again on every single write.
 */
void clog_test_reset_rotate_attempt_count(void);

/**
 * @brief Report how many times the library called _rotate() after the last
 *        call to clog_test_reset_rotate_attempt_count(), for a test.
 */
size_t clog_test_get_rotate_attempt_count(void);

/**
 * @brief Reset to zero the three maxima below, which the deletion passes of
 *        rotation record, for a test.
 */
void clog_test_reset_max_rotated_generations(void);

/**
 * @brief Report the largest number of rotated generations that any deletion
 *        pass has left on disk since the last call to
 *        clog_test_reset_max_rotated_generations(), for a test.
 */
int clog_test_get_max_rotated_generations(void);

/**
 * @brief Report the largest number of compressions in flight that any deletion
 *        pass saw since the last reset, for a test. A compression is in flight
 *        from the rotation that queues it until it finishes, whether it runs or
 *        waits in the queue.
 */
int clog_test_get_max_compressions_in_flight(void);

/**
 * @brief Report the largest number of rotated generations that any deletion
 *        pass left on disk beyond the older generations that it kept only
 *        because they were still under compression, since the last reset,
 *        for a test.
 */
int clog_test_get_max_generations_beyond_old_compressions(void);

/**
 * @brief Report how many times the calling thread read its process ID,
 *        thread ID and thread name from the system, for a test.
 */
unsigned clog_test_thread_identity_fill_count(void);

/**
 * @brief Sanitize `raw` into `out` for use as an RFC 5424 PRINTUSASCII field
 *        (APP-NAME or HOSTNAME), for a test.
 *
 * clog_open_fd_mp() and clog_open_file_mp() use the same PRINTUSASCII filter
 * internally, to cache both APP-NAME and HOSTNAME from the names that the OS
 * reports for the process and for the host. This function separates that filter
 * from those OS-specific sources, so that a test can exercise it directly with
 * any input.
 *
 * @param raw   NUL-terminated input string.
 * @param out   Destination buffer of size outsz. It is always NUL-terminated.
 * @param outsz Size of out, in bytes. It must be >= 2.
 */
void clog_test_sanitize_syslog_appname(const char *raw, char *out,
                                       size_t outsz);

/**
 * @brief Backslash-escape the control characters in `raw` into `out`, for a
 *        test.
 *
 * The library uses the same escape logic for RFC 5424 MSG content, and also for
 * the text of a backtrace frame in the logfmt format and in the syslog format
 * (see _buf_append_ctrl_escaped() in clogger.c). This function lets a test
 * verify the escape logic directly instead of indirectly through a complete log
 * line.
 *
 * @param raw   NUL-terminated input string.
 * @param out   Destination buffer of size outsz. If the escaped result does not
 *              fit, the function truncates it. It is always NUL-terminated.
 * @param outsz Size of out, in bytes. It must be >= 1.
 */
void clog_test_append_ctrl_escaped(const char *raw, char *out, size_t outsz);

/**
 * @brief Force (or stop forcing) every backtrace capture after this call to
 *        report a failure, for a test.
 *
 * While this hook is armed, every log call with with_backtrace=true behaves as
 * if the platform had no backtrace support, or as if backtrace_symbols() itself
 * had an allocation failure, so a test does not need to reproduce either of
 * those real conditions: one depends on the platform, and the other on a real
 * out-of-memory state. This hook affects every logger in the process; disarm it
 * (force == false) when the test does not need it any more.
 *
 * @param force true to force every future capture to fail. false to start real
 *              backtrace capture again.
 */
void clog_test_force_backtrace_capture_failure(bool force);

/**
 * @brief Force (or stop forcing) every frame of a real backtrace to fail to
 *        append inside the syslog backtrace emitter, for a test.
 *
 * While this hook is armed, the library treats every backtrace frame of a
 * CLOG_FMT_SYSLOG logger as too large to fit; those frames come from a real
 * captured backtrace that is not NULL. A test therefore does not need a real
 * allocation failure that lasts across every one of those small appends, one
 * for each frame. This hook affects every logger in the process; disarm it
 * (force == false) when the test does not need it any more.
 *
 * @param force true to force every future frame to fail. false to write the
 *              frames in the normal way again.
 */
void clog_test_force_all_syslog_backtrace_frames_failure(bool force);

/**
 * @brief Force (or stop forcing) every backtrace capture after this call to
 *        report a real but shallow result, for a test.
 *
 * While this hook is armed, _capture_backtrace() still does a real capture, and
 * syms is not NULL, exactly as for an ordinary capture that succeeds, but the
 * function clamps the depth that it reports to CLOG_BT_INITIAL_FRAME (2). That
 * is, whatever the real depth of the call stack is, it never reports a frame
 * beyond the two internal bookkeeping frames, which are _capture_backtrace
 * itself and _clog_write. This deterministically exercises the path where the
 * capture truly succeeds but finds nothing to show, without needing a real call
 * stack that is shallow enough to reach that path. This hook affects every
 * logger in the process; disarm it (force == false) when the test does not need
 * it any more.
 *
 * @param force true to force every future capture to report a clamped, shallow
 *              depth. false to report the real depth again.
 */
void clog_test_force_shallow_backtrace_depth(bool force);

/**
 * @brief Force the next handle acquisition to grow the slot table with a new
 *        slot, and then fail the live_shareds registration step of that slot,
 *        for a test. It also stops that force.
 *
 * The library always allocates clog_slot_table.slots, free_indices and
 * live_shareds with the plain default allocator of the process, never with the
 * custom mprocs of one logger, so a test cannot force a real allocation failure
 * at this exact call through the mprocs parameter of a public constructor. Nor
 * can a test know or control the state of the free_indices list of the slot
 * table at the moment that it runs, because earlier tests in the same process
 * may already have put the slots of closed loggers there for reuse. This hook
 * lets a test deterministically exercise the matching rollback path in
 * _clog_handle_acquire(), which clog_open_fd_mp(), clog_open_file_mp() and
 * clog_derive() all use, without depending on either of those two problems and
 * without needing the real global allocator to fail.
 *
 * While this hook is armed, the next call into _clog_handle_acquire() does
 * three things: it grows the slot table with a fresh slot, exactly as if
 * free_indices were empty; it treats the live_shareds registration of that slot
 * as a failure, without any real attempt at it; and it returns CLOG_INVALID.
 * The hook then disarms itself automatically, so it affects that one call only.
 *
 * @param force true to force the next acquisition to take this path. false to
 *              disarm without a wait for it to fire.
 */
void clog_test_force_next_fresh_slot_registration_failure(bool force);

/**
 * @brief Widen the window inside clog_close() between its step 3 and its step
 *        4, for a test.
 *
 * Step 3 waits for any concurrent resolver that holds a pin to finish, and step
 * 4 retires the slot and frees the reference to the shared target. Inside this
 * window, a slot has in_use == false but freed == false, and some OTHER thread
 * can call fork(); the child-side handling of "closing" slots in
 * _clog_atfork_release() exists to make that safe (see clog_atfork_closing_t in
 * clogger.c). The window is normally only a few instructions, far too narrow
 * for a test to land a real fork() inside it deterministically, so this hook
 * lets a test widen it on demand instead of depending on scheduling luck.
 *
 * While this hook is armed (delay_us != 0), every clog_close() call sleeps for
 * delay_us microseconds, immediately after step 3 finishes and before step 4
 * starts. This affects every clog_close() call in the process until you disarm
 * it (delay_us == 0). Unlike most other hooks in this section, this one does
 * not disarm itself, because a test needs it to stay armed across the one
 * clog_close() call under test, which a background thread makes while the main
 * test thread arranges the fork() into the widened window.
 *
 * @param delay_us Microseconds to sleep after step 3, or 0 to disarm.
 */
void clog_test_set_close_finalize_delay_us(unsigned int delay_us);

/**
 * @brief Test-only: widen the window in which a close cleared its slot but
 *        still needs the slot table.
 *
 * Partway through, a clog_close() stops being visible as a live slot, and it
 * then takes the write lock of the table again to finish. While this hook is
 * armed (delay_us != 0), every clog_close() sleeps for delay_us microseconds
 * inside exactly that span, so that a test can let a second close finish in the
 * middle of it on demand instead of depending on scheduling luck. This hook
 * does not disarm itself, for the same reason as the hook above.
 *
 * @param delay_us Microseconds to sleep inside that window, or 0 to disarm.
 */
void clog_test_set_close_release_window_us(unsigned int delay_us);

/**
 * @brief Test-only: report whether any clog_close() entered that window after
 *        the last call that armed it.
 *
 * This lets a test wait until a call truly occupies the window instead of
 * sleeping for a guessed interval, so a thread that starts slowly cannot leave
 * the test with a pass that exercised nothing.
 *
 * @return true after a clog_close() enters the armed window.
 */
bool clog_test_close_release_window_entered(void);

/**
 * @brief Report whether some clog_close() call entered the armed delay, for a
 *        test.
 *
 * The most recent call to clog_test_set_close_finalize_delay_us() arms that
 * delay. A test spin-polls this function, in the same way as
 * clog_test_gz_dest_opened(), so that it can wait deterministically until a
 * concurrent clog_close() call truly enters its widened step-3 to step-4 window
 * before it forks into that window, instead of using a fixed sleep and hoping
 * that the timing is correct. A call to clog_test_set_close_finalize_delay_us()
 * resets this to false.
 *
 * @return true after some clog_close() call enters the currently armed delay. A
 *         call to clog_test_set_close_finalize_delay_us() resets it to false.
 */
bool clog_test_close_finalize_delay_entered(void);

/**
 * @brief Report how many different shared logging targets are live in this
 *        process at this moment, for a test.
 *
 * A shared logging target is a root logger, in the sense of the words "shares
 * the parent's target" in clog_derive(). This count is directly the element
 * count of clog_slot_table.live_shareds; see the doc comment of that field in
 * clogger.c for the exact meaning of "live" here. It covers the complete
 * lifetime of a target, which starts when the library acquires the first handle
 * of the target and ends when the library frees the target, and it does not
 * depend on the state of any one handle. It is safe to call this function
 * inside a child that a fork just made, because it reads the copy-on-write copy
 * of the table that belongs to this process.
 *
 * @return The current count of live shared targets.
 */
size_t clog_test_live_shareds_count(void);

/**
 * @brief Call the internal gzip-compression helper of the log rotation
 *        directly, for a test.
 *
 * _rotate() calls this same helper to compress a rotated file. This function
 * lets a test verify the cleanup on the failure path directly. A failure can
 * happen before the library creates the destination file, for example when it
 * cannot open src, and such a failure must leave anything that is already on
 * disk at dst completely untouched instead of deleting that content
 * unconditionally.
 *
 * @param src Path of the uncompressed source file.
 * @param dst Destination path. By convention this is src plus ".gz".
 * @return true on success, in which case dst is a valid gzip file and the
 *         library unlinks src. It gives false on a failure, and then removes
 *         dst only if this call itself created dst. It leaves src untouched in
 *         both cases.
 */
bool clog_test_gzip_compress_file(const char *src, const char *dst);

/**
 * @brief Test-only: clog_test_gzip_compress_file() for a log whose live file
 *        live_uid owns.
 *
 * A rotation compresses a generation that the effective user owns, or that the
 * owner of the live file owns. This hook gives that owner, as a rotation does,
 * so that a test that runs as root can give the source to a different user and
 * still see it compressed.
 *
 * @param src Path of the uncompressed source file.
 * @param dst Destination path.
 * @param live_uid The owner of the live file of the log.
 * @return The same as clog_test_gzip_compress_file().
 */
bool clog_test_gzip_compress_file_for_owner(const char *src, const char *dst,
                                            uid_t live_uid);

/**
 * @brief Test-only: call hook inside the window of a rotation in which the live
 *        name names neither the old file nor the new one.
 *
 * The rotation calls hook(dir_fd, base) after it has moved the old file to its
 * rotated name and before the new live file takes the live name. dir_fd is the
 * directory of the log, and base is the last component of its path. NULL
 * disarms it. It has no effect outside a RUNNING_UNIT_TESTS build.
 */
void clog_test_set_rotate_window_hook(void (*hook)(int dir_fd,
                                                   const char *base));

/**
 * @brief Test-only: make the next fsync() of a compressed output report err.
 *
 * The next compression then sees err from the call that brings its output to
 * the disk, instead of the answer of the real call. The hook disarms itself
 * when it fires, and 0 disarms it.
 *
 * @param err The errno to report, or 0.
 */
void clog_test_force_next_fsync_error(int err);

/**
 * @brief Test-only: make the writer thread of every async logger hold the mutex
 *        of its target for delay_us microseconds on each record.
 *
 * The writer thread sleeps that long after it takes the mutex for a record and
 * before it writes the record. A test uses it to keep the writer thread busy
 * under the lock while another thread waits for that lock. 0 turns it off.
 *
 * @param delay_us The delay in microseconds, or 0.
 */
void clog_test_set_writer_job_delay_us(unsigned int delay_us);

/**
 * @brief Test-only: set the time budget of the drain that runs at process exit,
 *        in milliseconds.
 *
 * The drain delivers what async loggers still hold, and finishes the queued
 * compressions, within this budget. A test sets a short budget to observe what
 * the drain does when the budget runs out. 0 restores the default.
 *
 * @param budget_ms The budget in milliseconds, or 0 for the default.
 */
void clog_test_set_exit_drain_budget_ms(unsigned int budget_ms);

/**
 * @brief Test-only: make the flush request that the exit drain sends to a
 *        bounded queue wait for 0 microseconds, as it does once the budget
 *        of the drain is spent.
 *
 * @param on true to force the send without a wait, false to restore it.
 */
void clog_test_force_exit_drain_send_without_wait(bool on);

/**
 * @brief Test-only: run the drain that runs at process exit, now.
 *
 * A test calls it in a forked child, which then ends with _exit().
 */
void clog_test_run_exit_drain(void);

/**
 * @brief Test-only: whether the exit drain left the target of h alone,
 *        because its writer thread did not answer in time.
 *
 * @param h A logger handle.
 * @return true when a drain of this process left the target of h.
 */
bool clog_test_exit_drain_left_target(clog h);

/**
 * @brief Test-only: lock the mutex of the target of h, and keep it locked.
 *
 * The calling thread takes the mutex that every write, rotation and compression
 * step of the target takes, and returns with it held; nothing unlocks it. A
 * test calls this from a thread that then never returns, to show that the drain
 * at process exit gives up on that target within its budget.
 *
 * @param h An open logger handle. An invalid handle does nothing.
 */
void clog_test_lock_target_forever(clog h);

/**
 * @brief Test-only: take the write lock of the table of loggers, and keep it.
 *
 * The same as clog_test_lock_target_forever(), for the lock that the drain
 * at process exit takes before it visits any target.
 */
void clog_test_lock_table_forever(void);

/**
 * @brief Force (or stop forcing) the next mutex initialization that
 *        clog_flush() drives to fail, for a test.
 *
 * clog_flush() builds a fresh synchronization object on the stack for each
 * call, and the pre-flush step on the FATAL path of _clog_write() does the
 * same; both initialize that object with ccol_mutex_init(). With default or
 * NULL attributes, this call has no real failure path that a test can trigger
 * portably, and unlike an ordinary heap allocation, it does not go through the
 * custom allocator of a logger. This hook lets a test deterministically
 * exercise the matching path, on which the library fails safely instead of
 * using a mutex that is not fully initialized.
 *
 * While this hook is armed, the very next such initialization reports a failure
 * without calling the underlying primitive at all. The hook then disarms itself
 * automatically, so it affects that one call only.
 *
 * @param force true to force the next initialization to fail. false to disarm
 *              without a wait for it to fire.
 */
void clog_test_force_flush_mutex_init_failure(bool force);

/**
 * @brief Force (or stop forcing) the next condition variable initialization
 *        that clog_flush() drives to fail, for a test.
 *
 * This is the condition variable equivalent of
 * clog_test_force_flush_mutex_init_failure(). It exercises the path on which
 * the mutex part of the synchronization object that clog_flush() builds for
 * each call initializes with success but the condition variable does not; the
 * library must then free the mutex that it already initialized instead of
 * leaking it.
 *
 * @param force true to force the next initialization to fail. false to disarm
 *              without a wait for it to fire.
 */
void clog_test_force_flush_condvar_init_failure(bool force);

/**
 * @brief Call the internal last-resort record writer directly, for a test.
 *
 * The library uses that writer when not even a small fallback placeholder fits
 * in the target buffer: _clog_write() falls back to this same helper in one
 * case alone, when _clog_build_record() cannot fit its own small, fixed-size
 * fallback placeholder into the write buffer. The buffer-size constants of this
 * library make that condition unreachable through the ordinary path of a logger
 * and a log_* call; see the doc comment on that function in clogger.c. This
 * hook lets a test drive the behavior of the helper directly and
 * deterministically, and in particular check whether the helper still signals a
 * backtrace that the caller asked for but the helper left out.
 *
 * @param logger         An open logger handle. This writes to the underlying fd
 *                       of that handle exactly as a real log_* call does, and
 *                       it obeys the byte accounting of the log rotation.
 * @param fmt            The output format for the placeholder record.
 * @param level          The severity level to render.
 * @param with_backtrace Whether to also signal that the caller asked for a
 *                       backtrace that the library could not include.
 */
void clog_test_write_unrepresentable_record(clog logger, clog_format_t fmt,
                                            clog_level_t level,
                                            bool with_backtrace);

/**
 * @brief Force (or stop forcing) the next append to the internal write buffer
 *        to report a real allocation failure, for a test.
 *
 * The internal append helpers in clogger.c have a real failure path only when
 * the buffer must grow, and the buffer-size constants of this library make that
 * growth impossible for a few deliberately small records of a fixed size, such
 * as the "backtrace unavailable" marker that _emit_backtrace_syslog_lines()
 * builds for CLOG_FMT_SYSLOG. This hook lets a test force that one append to
 * fail without needing the buffer to grow.
 *
 * While this hook is armed, the very next internal buffer append reports a
 * failure without touching the underlying allocator at all. The hook then
 * disarms itself automatically, so it affects that one call only.
 *
 * @param force true to force the next check to fail. false to disarm without a
 *              wait for it to fire.
 */
void clog_test_force_next_buf_ensure_failure(bool force);

/**
 * @brief Make each of the next `write_count` internal writes deliver only its
 *        first `accepted_bytes` bytes, for a test.
 *
 * This reproduces a short write, in which the kernel accepts only the first
 * part of a record, as a filesystem with no free space or a process-wide
 * RLIMIT_FSIZE ceiling makes it do, without putting the whole test process
 * under a real resource limit. It also differs from a real limit in one way:
 * the writes after the last capped write still work, which lets a test observe
 * how the logger restores the record framing afterwards. A count above one
 * reproduces a descriptor that cannot take the remainder of a record across
 * more than one delivery attempt.
 *
 * Each capped write consumes one of the count, so this hook affects only the
 * writes that a test targets. Give 0 bytes to disarm without a wait for the
 * rest of the count to fire.
 *
 * @param accepted_bytes Number of bytes that each capped write can deliver.
 * @param write_count    How many writes in a row the library caps.
 */
void clog_test_force_short_writes(size_t accepted_bytes,
                                  unsigned int write_count);

/**
 * @brief Make each of the next write_count writes of the library fail with
 *        errno err, before it delivers a byte.
 *
 * This reproduces a write error of any class on a descriptor that otherwise
 * works: an error that a later attempt can outlast, such as ENOSPC, or one that
 * no retry outlasts, such as EPIPE. The writes after the last forced failure
 * work again, so a test can observe what the logger wrote about the bytes that
 * did not go out. Give 0 as err to disarm.
 *
 * @param err         The errno of each forced failure.
 * @param write_count How many writes in a row fail.
 */
void clog_test_force_write_errors(int err, unsigned int write_count);

/**
 * @brief Call the "backtrace capture failed" marker branch of
 *        _emit_backtrace_syslog_lines() directly against the shared target of
 *        logger, for a test.
 *
 * To reach this branch through the ordinary log_* call path, a test must force
 * the backtrace capture to fail (see
 * clog_test_force_backtrace_capture_failure()). This hook goes one step further
 * and lets a test exercise that branch directly and on its own, so that the
 * test can deterministically exercise the internal fallback of that branch,
 * which clog_test_force_next_buf_ensure_failure() arms, without any other
 * unrelated buffer append racing to consume that one-shot hook first.
 *
 * @param logger An open logger handle. This writes to the underlying fd of that
 *              handle exactly as a real log_* call does, and it obeys the byte
 *              accounting of the log rotation.
 * @param level  The severity level to render.
 */
void clog_test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                        clog_level_t level);
#endif

#pragma GCC visibility pop
