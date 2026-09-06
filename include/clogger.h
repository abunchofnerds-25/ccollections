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
 * Backtrace (ERROR / ALERT / FATAL only) is appended as tab-indented
 * continuation lines that do not start with "ts=", allowing log aggregators
 * to group them with their parent record:
 *   \t#0 ./bin(fn+0x1a) [0x7f...]
 *   \t#1 ./bin(main+0x42) [0x7f...]
 *
 * Thread safety:
 *   All public functions are fully thread-safe for concurrent calls made
 *   with still-open handles. Writes and log rotation for a given output
 *   target are serialised by one lock shared by the root logger and every
 *   logger derived from it (see clog_derive()). Each logger's own structured
 *   fields (clog_set_field() and friends) and its own minimum log level
 *   (clog_set_level() / clog_get_level()) are both synchronised
 *   independently of that lock, so mutating either one never contends with
 *   a write in progress on a sibling logger sharing the same target.
 *
 *   clog_close() is not covered by the above: as with every other
 *   handle-based type in this library, closing a handle safely (i.e. only
 *   once no other thread holds or is using that same handle) is the
 *   caller's responsibility, not something this library synchronises for
 *   you. Closing one handle concurrently with a sibling handle (root vs.
 *   derived, or two derived loggers) still writing to the shared target
 *   remains safe, since that sibling is a different, still-open handle.
 *
 * Log rotation:
 *   Available only for file-backed loggers (clog_open_file_mp). Size rotation
 *   triggers after the write that crosses max_file_size. Time rotation
 *   triggers on the first write after the interval elapses. Rotated files
 *   are renamed <path>.<YYYYMMDDHHMMSS>[_N]; if more than max_rotated_files
 *   exist the oldest are deleted. When compress_rotated is true the rotated
 *   file is further compressed with gzip (producing .gz) outside the logger
 *   mutex. Build with -rdynamic for resolved symbols in backtraces.
 */

#include <common.h>
#include <stdbool.h>
#include <stdint.h>    /* uint64_t */
#include <sys/types.h> /* off_t */
#include <time.h>      /* time_t */

/* Remove any macro definition of clog from <complex.h> */
#ifdef clog
#undef clog
#endif

/* ========================================================================== */
/*                         LOG LEVELS                                         */
/* ========================================================================== */

/*
 * Every enumerator below is given an explicit numeric value, deliberately,
 * not left to C's auto-increment: these values are load-bearing array
 * indices (see _LEVEL_STR[] / _SYSLOG_SEVERITY[] in clogger.c), so an
 * insertion in the middle without an explicit value would silently shift
 * every subsequent level's numeric value and misalign both arrays with no
 * compiler diagnostic. Any future addition must do the same.
 */
typedef enum {
  CLOG_TRACE = 0,
  CLOG_DEBUG = 1,
  CLOG_INFO = 2,
  CLOG_WARN = 3,
  CLOG_ERROR = 4,
  CLOG_ALERT = 5, /**< Action must be taken immediately; maps to syslog
                     severity 1 */
  CLOG_FATAL = 6, /**< Appends a backtrace and terminates the process via
                 exit(EXIT_FAILURE). Bypasses min_level: the fatal message
                 is always written regardless of the logger's level setting. */
  CLOG_OFF = 7    /**< Disables all output when used as min_level */
} clog_level_t;

/* ========================================================================== */
/*                         ROTATION CONFIGURATION */
/* ========================================================================== */

/** Default maximum log file size before rotation (10 MiB). */
#define CLOG_DEFAULT_MAX_FILE_SIZE ((off_t)(10L * 1024L * 1024L))

/** Default time-rotation interval in seconds (1 day). */
#define CLOG_DEFAULT_ROTATION_INTERVAL ((time_t)86400L)

/** Default number of rotated files to retain. */
#define CLOG_DEFAULT_MAX_ROTATED_FILES 7

/**
 * @brief Log rotation configuration.
 *
 * Pass a pointer to this struct to clog_open_file_mp().
 *
 * Zero or negative values for max_file_size, rotation_interval_secs, and
 * max_rotated_files each fall back to their respective CLOG_DEFAULT_*
 * constant; there is no way to disable pruning of old rotated files
 * entirely, so a caller that leaves max_rotated_files unset never
 * accumulates an unbounded number of rotated files on disk.
 *
 * Example; rotate at 50 MiB, keep 14 files, gzip each rotated file:
 * @code
 * clog_rotation_cfg_t cfg = {
 *     .size_rotation_enabled  = true,
 *     .max_file_size          = 50L * 1024L * 1024L,
 *     .time_rotation_enabled  = false,
 *     .max_rotated_files      = 14,
 *     .compress_rotated       = true,
 * };
 * clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, &cfg, NULL);
 * @endcode
 *
 * When compress_rotated is true, each rotated file is gzip-compressed via
 * zlib immediately after rotation, producing a
 * "<path>.<YYYYMMDDHHMMSS>[_N].gz" file.  The original uncompressed file is
 * removed on success.  Compression failures are silent: the uncompressed file
 * is left on disk so no data is lost.  Compressed files are decompressible
 * with standard tools (gunzip, zcat, gzip -d).  Compression runs outside the
 * logger mutex so log writers are not stalled during the operation.
 * Requires linking with -lz.
 */
typedef struct clog_rotation_cfg {
  bool size_rotation_enabled;
  off_t max_file_size; /**< Bytes; <= 0 -> CLOG_DEFAULT_MAX_FILE_SIZE */
  bool time_rotation_enabled;
  time_t rotation_interval_secs; /**< Seconds; <= 0 ->
                                    CLOG_DEFAULT_ROTATION_INTERVAL */
  int max_rotated_files;         /**< Files kept on disk; <= 0 ->
                                    CLOG_DEFAULT_MAX_ROTATED_FILES */
  bool compress_rotated; /**< Gzip-compress each rotated file via zlib (-lz) */
} clog_rotation_cfg_t;

/* ========================================================================== */
/*                         ASYNC LOGGING CONFIGURATION */
/* ========================================================================== */

/** Default aggregation-buffer flush threshold for async logging (64 KiB). */
#define CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE (64UL * 1024UL)

/** Default aggregation-buffer flush interval for async logging (200 ms). */
#define CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_MS 200UL

/**
 * @brief Async logging configuration.
 *
 * Pass a pointer to this struct to clog_open_fd_mp()/clog_open_file_mp() (or
 * their non-_mp wrappers) to make the resulting logger, and every logger
 * later derived from it, log asynchronously: the calling thread only
 * formats its own message and captures its own identity/timestamp/fields,
 * then hands the rest (escaping, field serialization, and the actual
 * write()) to a dedicated writer thread that aggregates records from every
 * handle sharing the same target into one buffer, flushed once
 * flush_buffer_size or flush_interval_ms is reached.
 *
 * Unlike clog_rotation_cfg_t, a NULL async_cfg and a non-NULL pointer to an
 * all-zero clog_async_cfg_t are NOT equivalent: passing ANY non-NULL pointer
 * enables async mode (every zero-valued field simply falls back to its
 * documented default below); only a literal NULL disables it. There is no
 * "enabled" flag inside this struct to leave false.
 *
 * CLOG_FMT_SYSLOG is exempt from batching regardless of these values: each
 * record and each backtrace frame keeps its own write() call, one per UDP
 * datagram. CLOG_FATAL always bypasses the async queue and writes
 * synchronously (draining anything already queued first), since the process
 * terminates immediately afterward.
 */
typedef struct clog_async_cfg {
  /** Message-queue capacity; 0 -> an unbounded queue that never blocks a
   * caller on send; > 0 -> a bounded queue of this many messages, where a
   * full queue BLOCKS the calling thread until space frees up (a message is
   * never silently dropped). */
  size_t queue_size;
  /** Bytes; the writer thread flushes once its aggregation buffer reaches
   * this size. 0 -> CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE. */
  size_t flush_buffer_size;
  /** Milliseconds; the writer thread flushes at least this often even if
   * flush_buffer_size has not been reached. 0 ->
   * CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_MS. */
  unsigned long flush_interval_ms;
} clog_async_cfg_t;

/* ========================================================================== */
/*                         LOGGER TYPE                                        */
/* ========================================================================== */

struct clogger;

/**
 * @brief Opaque logger handle.
 *
 * A `clog` is an opaque, generational value handle (an index and a
 * generation counter packed into a uint64_t), not a pointer; never cast
 * it to or from `void *`, and never compare two handles by casting either
 * one to a pointer. `if (!lg)` and `lg == CLOG_INVALID` both work as
 * expected, since CLOG_INVALID is numerically zero.
 *
 * Obtain via clog_open_fd_mp() or clog_open_file_mp(). Release via
 * clog_close().
 */
typedef uint64_t clog;

/** @brief Sentinel value denoting no logger; numerically zero. */
#define CLOG_INVALID ((clog)0)

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

/**
 * @brief Create a logger writing to an already-open file descriptor with custom
 * memory management procs.
 *
 * The file descriptor is not owned by the logger and will NOT be closed on
 * clog_close(). Log rotation is unavailable for fd-based loggers. All dynamic
 * memory operations will be performed via the provided memory management procs.
 *
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, ...).
 * @param min_level Messages below this level are silently dropped.
 * @param async_cfg Async logging config, or NULL for synchronous logging (see
 *                  clog_async_cfg_t).
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or CLOG_INVALID on allocation failure, invalid
 *         mprocs, or if fd is closed, read-only, or otherwise invalid.
 */
clog clog_open_fd_mp(int fd, clog_level_t min_level,
                     const clog_async_cfg_t *async_cfg,
                     ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Create a logger writing to an already-open file descriptor.
 *
 * The file descriptor is not owned by the logger and will NOT be closed on
 * clog_close(). Log rotation is unavailable for fd-based loggers.
 *
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, ...).
 * @param min_level Messages below this level are silently dropped.
 * @param async_cfg Async logging config, or NULL for synchronous logging (see
 *                  clog_async_cfg_t).
 * @return New logger handle, or CLOG_INVALID on allocation failure, or if fd
 *         is closed, read-only, or otherwise invalid.
 */
static inline __attribute__((always_inline)) clog clog_open_fd(
    int fd, clog_level_t min_level, const clog_async_cfg_t *async_cfg) {
  return clog_open_fd_mp(fd, min_level, async_cfg, NULL);
}

/**
 * @brief Create a file-backed logger with optional log rotation with custom
 * memory management procs.
 *
 * The file is created if it does not exist (mode 0644). If it exists it is
 * opened for appending. All dynamic memory operations will be performed via the
 * provided memory management procs.
 *
 * @param path      Destination log file path.
 * @param min_level Messages below this level are silently dropped.
 * @param cfg       Rotation config, or NULL to disable rotation.
 * @param async_cfg Async logging config, or NULL for synchronous logging (see
 *                  clog_async_cfg_t).
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or CLOG_INVALID on failure (bad path, alloc
 *         error, etc.).
 */
clog clog_open_file_mp(const char *path, clog_level_t min_level,
                       const clog_rotation_cfg_t *cfg,
                       const clog_async_cfg_t *async_cfg,
                       ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Create a file-backed logger with optional log rotation.
 *
 * The file is created if it does not exist (mode 0644). If it exists it is
 * opened for appending.
 *
 * @param path      Destination log file path.
 * @param min_level Messages below this level are silently dropped.
 * @param cfg       Rotation config, or NULL to disable rotation.
 * @param async_cfg Async logging config, or NULL for synchronous logging (see
 *                  clog_async_cfg_t).
 * @return New logger handle, or CLOG_INVALID on failure (bad path, alloc
 *         error, etc.).
 */
static inline __attribute__((always_inline)) clog clog_open_file(
    const char *path, clog_level_t min_level, const clog_rotation_cfg_t *cfg,
    const clog_async_cfg_t *async_cfg) {
  return clog_open_file_mp(path, min_level, cfg, async_cfg, NULL);
}

/**
 * @brief Flush, close, and free all resources owned by the logger.
 *
 * After this call the handle must not be used. The caller must ensure no
 * other thread is concurrently using (or concurrently closing) this exact
 * handle; see this header's own "Thread safety" note above for the scope of
 * that requirement.
 * If this is the last handle sharing an underlying file (root or all derived
 * loggers closed), the file descriptor is closed automatically.
 */
void clog_close(clog logger);

/**
 * @brief Derive a new logger from an existing one.
 *
 * The derived logger shares the parent's logging target (file descriptor) and
 * synchronisation mutex, so all writes (from the parent and every derived
 * logger) are serialised by the same lock and go to the same destination.
 *
 * The derived logger starts with a snapshot of the parent's fields and
 * minimum log level at the time of the call.  After that, the two loggers
 * maintain independent field maps and level settings: changes on one are not
 * visible on the other.
 *
 * Log rotation (if configured on the original file-backed logger) continues
 * to be managed by the root logger; derived loggers benefit from it
 * transparently.
 *
 * Release the derived logger with clog_close() when it is no longer needed.
 * The underlying file is kept open until all handles (root + every derived
 * logger) have been closed.
 *
 * @param parent Source logger to derive from.
 * @return New derived logger handle, or CLOG_INVALID on allocation failure.
 */
clog clog_derive(clog parent);

/**
 * @brief Block until every record queued as of this call, plus any
 * partially-filled aggregation buffer, has been durably written.
 *
 * A no-op (returns immediately) for a synchronous (non-async) logger. Safe
 * to call on any still-open handle at any time. A record submitted by some
 * other thread concurrently with, but strictly after, this call is not
 * guaranteed to be included.
 *
 * @param logger Logger handle.
 */
void clog_flush(clog logger);

/* ========================================================================== */
/*                         LEVEL CONTROL                                      */
/* ========================================================================== */

/** Change the minimum log level. Thread-safe. */
void clog_set_level(clog logger, clog_level_t level);

/** Return the current minimum log level. Thread-safe. */
clog_level_t clog_get_level(clog logger);

/* ========================================================================== */
/*                         OUTPUT FORMAT */
/* ========================================================================== */

/**
 * @brief Log output format.
 *
 * The default is CLOG_FMT_LOGFMT. The format lives on the shared backing
 * store, so it is shared by all logger handles writing to the same file
 * descriptor (root + all derived). Changing the format via any handle
 * takes effect immediately for all of them.
 *
 * CLOG_FMT_SYSLOG is restricted to fd-based loggers (clog_open_fd /
 * clog_open_fd_mp). Calling clog_set_format() with CLOG_FMT_SYSLOG on a
 * file-backed logger (clog_open_file / clog_open_file_mp) is a silent no-op;
 * the format remains unchanged.
 */
typedef enum {
  CLOG_FMT_LOGFMT = 0, /**< key=value logfmt (default) */
  CLOG_FMT_JSON,       /**< NDJSON; one JSON object per line */
  CLOG_FMT_SYSLOG, /**< RFC 5424 syslog; fd-based loggers only (see above) */
} clog_format_t;

/**
 * @brief Change the output format.
 *
 * Because the format is stored on the shared backing store, the change is
 * visible to every logger handle that shares the same file descriptor.
 * Passing CLOG_FMT_SYSLOG on a file-backed logger is a silent no-op.
 * Thread-safe.
 *
 * @param logger Logger handle.
 * @param fmt    Desired output format.
 */
void clog_set_format(clog logger, clog_format_t fmt);

/**
 * @brief Return the current output format.
 *
 * @param logger Logger handle.
 * @return Current output format.
 */
clog_format_t clog_get_format(clog logger);

/* ========================================================================== */
/*                         SYSLOG FACILITY                                    */
/* ========================================================================== */

/**
 * @brief Syslog facility codes (RFC 5424 / RFC 3164).
 *
 * Used with clog_set_facility() when the logger's output format is
 * CLOG_FMT_SYSLOG.
 *
 * @par Prerequisites for CLOG_FMT_SYSLOG
 * The fd passed to clog_open_fd() / clog_open_fd_mp() must be a writable
 * file descriptor already connected to a syslog daemon.  On Linux this is
 * typically a Unix-domain socket obtained via:
 * @code
 *   int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
 *   struct sockaddr_un sa = { .sun_family = AF_UNIX };
 *   strncpy(sa.sun_path, "/dev/log", sizeof sa.sun_path - 1);
 *   connect(fd, (struct sockaddr *)&sa, sizeof sa);
 *   clog lg = clog_open_fd(fd, CLOG_INFO);
 *   clog_set_format(lg, CLOG_FMT_SYSLOG);
 * @endcode
 * For SOCK_DGRAM the kernel delivers each write() as a single datagram; keep
 * individual messages under 2 KiB to stay within typical syslog daemon
 * limits.  Log rotation is unavailable for fd-based loggers.
 *
 * The facility is stored on the shared backing store, so it is shared by all
 * logger handles writing to the same file descriptor (root + all derived).
 * The default facility is CLOG_SYSLOG_USER.
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
 * Only meaningful when the output format is CLOG_FMT_SYSLOG. Because the
 * facility is stored on the shared backing store, the change is visible to
 * every logger handle that shares the same file descriptor. Thread-safe.
 *
 * @param logger   Logger handle.
 * @param facility Desired facility code.
 */
void clog_set_facility(clog logger, clog_syslog_facility_t facility);

/**
 * @brief Return the current syslog facility.
 *
 * @param logger Logger handle.
 * @return Current facility code.
 */
clog_syslog_facility_t clog_get_facility(clog logger);

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

/**
 * @brief Attach a persistent key=value field to the logger.
 *
 * The field appears in every subsequent log line emitted by this logger.
 * Both key and value are copied internally; the caller may free them.
 * Calling with an existing key replaces the value. Thread-safe.
 *
 * @param logger Logger handle.
 * @param key    Field name (non-empty; must consist entirely of printable
 *               US-ASCII characters (0x21-0x7e) excluding '=', ']', '"', and
 *               '\'; keys that violate this are silently ignored. So is a
 *               key that names one of the fixed tokens every log line
 *               already carries (ts, level, proc, src, func, msg, bt,
 *               bt_error), since allowing one through would produce a
 *               duplicate key in the output).
 * @param value  Field value (arbitrary text; quoted if necessary).
 */
void clog_set_field(clog logger, const char *key, const char *value);

/**
 * @brief Remove a previously attached field.
 *
 * No-op if the key is not present. Thread-safe.
 */
void clog_remove_field(clog logger, const char *key);

/** Remove all attached fields. Thread-safe. */
void clog_clear_fields(clog logger);

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
 * @param with_backtrace Append a stack trace when true.
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
 * Each macro accepts a clog handle followed by a printf-style format string
 * and optional arguments.  The file name, line number, and function name are
 * captured automatically.  log_error and log_fatal also append a backtrace.
 *
 * @{
 */

#define log_trace(l, fmt, ...)                                           \
  _clog_write((l), CLOG_TRACE, __FILE__, __LINE__, __func__, false, fmt, \
              ##__VA_ARGS__)

#define log_debug(l, fmt, ...)                                           \
  _clog_write((l), CLOG_DEBUG, __FILE__, __LINE__, __func__, false, fmt, \
              ##__VA_ARGS__)

#define log_info(l, fmt, ...)                                           \
  _clog_write((l), CLOG_INFO, __FILE__, __LINE__, __func__, false, fmt, \
              ##__VA_ARGS__)

#define log_warn(l, fmt, ...)                                           \
  _clog_write((l), CLOG_WARN, __FILE__, __LINE__, __func__, false, fmt, \
              ##__VA_ARGS__)

/** Logs at ERROR level and appends a backtrace. */
#define log_error(l, fmt, ...)                                          \
  _clog_write((l), CLOG_ERROR, __FILE__, __LINE__, __func__, true, fmt, \
              ##__VA_ARGS__)

/** Logs at ALERT level and appends a backtrace. */
#define log_alert(l, fmt, ...)                                          \
  _clog_write((l), CLOG_ALERT, __FILE__, __LINE__, __func__, true, fmt, \
              ##__VA_ARGS__)

/**
 * Logs at FATAL level, appends a backtrace, then terminates the process by
 * calling exit(EXIT_FAILURE). This macro never returns to the caller.
 * Unlike all other log_* macros, log_fatal bypasses the logger's min_level
 * filter: the message is always written so the cause of termination is never
 * silently suppressed.
 */
#define log_fatal(l, fmt, ...)                                          \
  _clog_write((l), CLOG_FATAL, __FILE__, __LINE__, __func__, true, fmt, \
              ##__VA_ARGS__)

/** @} */

/* ========================================================================== */
/*                         UNIT TEST INTERNALS                                */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Arm (or disarm) an artificial delay inside log rotation's internal
 *        gzip-compression step, for testing.
 *
 * When armed (delay_us != 0), the next rotation's compression step sleeps
 * for delay_us microseconds immediately after creating its ".gz" destination
 * file on disk, before writing any content to it; deterministically
 * widening the window during which a concurrent rotation's own pruning pass
 * must not be able to delete that destination. Calling this also resets the
 * state clog_test_gz_dest_opened() reports. Has no effect outside of a
 * RUNNING_UNIT_TESTS build.
 *
 * @param delay_us Microseconds to sleep, or 0 to disarm.
 */
void clog_test_set_pending_compress_delay_us(unsigned int delay_us);

/**
 * @brief Report whether the compression step armed by the most recent call
 *        to clog_test_set_pending_compress_delay_us() has created its ".gz"
 *        destination file on disk yet, for testing.
 *
 * Intended to be spin-polled so a test can deterministically wait until a
 * concurrent rotation's compressed destination genuinely exists on disk
 * before exercising a second, racing rotation against it, rather than
 * relying on a fixed sleep and hoping the timing lines up.
 *
 * @return true once the destination file has been created for the
 *         most-recently-armed delay; reset to false by
 *         clog_test_set_pending_compress_delay_us().
 */
bool clog_test_gz_dest_opened(void);

/**
 * @brief Reset the counter of real rotation attempts (_rotate() calls) to
 *        zero, for testing.
 *
 * Used to verify that a persistently failing rotation is retried with a
 * bounded backoff rather than reattempted on every single write.
 */
void clog_test_reset_rotate_attempt_count(void);

/**
 * @brief Report how many times _rotate() has actually been invoked since the
 *        last call to clog_test_reset_rotate_attempt_count(), for testing.
 */
size_t clog_test_get_rotate_attempt_count(void);

/**
 * @brief Sanitize `raw` into `out` for use as an RFC 5424 PRINTUSASCII field
 *        (APP-NAME or HOSTNAME), for testing.
 *
 * Exposes the same PRINTUSASCII-filtering logic clog_open_fd_mp() /
 * clog_open_file_mp() use internally to cache both APP-NAME and HOSTNAME
 * from the process's/host's own OS-reported names, decoupled from those
 * OS-specific sources so it can be exercised directly with arbitrary input.
 *
 * @param raw   NUL-terminated input string.
 * @param out   Destination buffer of size outsz; always NUL-terminated.
 * @param outsz Size of out, in bytes; must be >= 2.
 */
void clog_test_sanitize_syslog_appname(const char *raw, char *out,
                                       size_t outsz);

/**
 * @brief Backslash-escape control characters in `raw` into `out`, for
 *        testing.
 *
 * Exposes the same control-character escaping logic used for RFC 5424 MSG
 * content and for backtrace frame text in the logfmt and syslog formats
 * (see _buf_append_ctrl_escaped() in clogger.c), so the escaping itself can
 * be verified directly rather than only indirectly through a full log line.
 *
 * @param raw   NUL-terminated input string.
 * @param out   Destination buffer of size outsz; truncated and always
 *              NUL-terminated if the escaped result would not fit.
 * @param outsz Size of out, in bytes; must be >= 1.
 */
void clog_test_append_ctrl_escaped(const char *raw, char *out, size_t outsz);

/**
 * @brief Force (or stop forcing) every subsequent backtrace capture to
 *        report failure, for testing.
 *
 * While armed, every with_backtrace=true log call behaves exactly as if the
 * platform lacked backtrace support or backtrace_symbols() itself had
 * failed to allocate, without needing either of those genuinely
 * platform-/OOM-specific conditions to be reproduced for real. Affects
 * every logger in the process; disarm it (force == false) once the test no
 * longer needs it.
 *
 * @param force true to force every future capture to fail; false to resume
 *              real backtrace capture.
 */
void clog_test_force_backtrace_capture_failure(bool force);

/**
 * @brief Force (or stop forcing) every frame of a real, successfully
 *        captured backtrace to fail to append inside the syslog backtrace
 *        emitter, for testing.
 *
 * While armed, a CLOG_FMT_SYSLOG logger's backtrace frames (from a genuine,
 * non-NULL captured backtrace) are all treated as failing to fit, without
 * needing a genuine, sustained allocation failure to persist across every
 * one of those small per-frame appends. Affects every logger in the
 * process; disarm it (force == false) once the test no longer needs it.
 *
 * @param force true to force every future frame to fail; false to resume
 *              normal frame emission.
 */
void clog_test_force_all_syslog_backtrace_frames_failure(bool force);

/**
 * @brief Force (or stop forcing) every subsequent backtrace capture to
 *        report a genuinely successful but shallow result, for testing.
 *
 * While armed, _capture_backtrace() still performs a real capture (syms is
 * non-NULL, exactly as an ordinary successful capture would be), but the
 * reported depth is clamped to CLOG_BT_INITIAL_FRAME (2); i.e. no frame
 * beyond the two internal bookkeeping ones (_capture_backtrace itself,
 * _clog_write) is ever reported to the caller, regardless of how deep the
 * real call stack actually is. This exercises the "capture genuinely
 * succeeded but found nothing to show" path deterministically, without
 * depending on a real call stack ever being shallow enough to reach it for
 * real. Affects every logger in the process; disarm it (force == false)
 * once the test no longer needs it.
 *
 * @param force true to force every future capture to report a clamped,
 *              shallow depth; false to resume reporting the real depth.
 */
void clog_test_force_shallow_backtrace_depth(bool force);

/**
 * @brief Force (or stop forcing) the next handle acquisition to grow the
 *        slot table with a brand new slot and then fail that slot's own
 *        live_shareds registration step, for testing.
 *
 * clog_slot_table.slots/free_indices/live_shareds are always allocated via
 * the process's plain default allocator (never a per-logger custom mprocs),
 * so a real allocation failure at exactly this call cannot be forced
 * portably from a test through any public constructor's own mprocs
 * parameter; a test also has no way to know or control whether the slot
 * table's own free_indices list happens to be empty at the moment it runs
 * (earlier tests in the same process may have already left closed loggers'
 * slots there for reuse). This hook lets a test deterministically exercise
 * the corresponding rollback path in _clog_handle_acquire() (used by
 * clog_open_fd_mp()/clog_open_file_mp()/clog_derive() alike) regardless of
 * either of those, without depending on the real global allocator ever
 * actually failing.
 *
 * While armed, the next call into _clog_handle_acquire() grows the slot
 * table with a fresh slot exactly as if free_indices were empty, treats that
 * slot's own live_shareds registration as having failed (without actually
 * attempting it), and returns CLOG_INVALID; then disarms itself
 * automatically so only that one call is affected.
 *
 * @param force true to force the next acquisition to take this path; false
 *              to disarm without waiting for it to fire.
 */
void clog_test_force_next_fresh_slot_registration_failure(bool force);

/**
 * @brief Widen the window inside clog_close() between its own step 3
 *        (waiting for any concurrent, pin-holding resolver to finish) and
 *        step 4 (retiring the slot and releasing the shared target's
 *        reference), for testing.
 *
 * During this window a slot is left with in_use == false but freed ==
 * false; a fork() by some OTHER thread landing inside it is what
 * _clog_atfork_release()'s own child-side handling of "closing" slots
 * exists to make safe (see clog_atfork_closing_t in clogger.c). That window
 * is normally just a handful of instructions (far too narrow to land a
 * real fork() inside deterministically), so this hook lets a test widen it
 * on demand instead of relying on scheduling luck.
 *
 * While armed (delay_us != 0), every clog_close() call sleeps for delay_us
 * microseconds immediately after step 3 completes, before step 4 begins.
 * Affects every clog_close() call in the process until disarmed (delay_us
 * == 0); unlike most other hooks in this section, it does not auto-disarm,
 * since a test needs it to stay armed across the specific clog_close() call
 * under test, called from a background thread while the main test thread
 * arranges the fork() into that widened window.
 *
 * @param delay_us Microseconds to sleep after step 3, or 0 to disarm.
 */
void clog_test_set_close_finalize_delay_us(unsigned int delay_us);

/**
 * @brief Report whether the delay armed by the most recent call to
 *        clog_test_set_close_finalize_delay_us() has actually been entered
 *        by some clog_close() call yet, for testing.
 *
 * Intended to be spin-polled, mirroring clog_test_gz_dest_opened()'s own
 * precedent, so a test can deterministically wait until a concurrent
 * clog_close() call has genuinely entered its widened step-3/4 window
 * before forking into it, rather than relying on a fixed sleep and hoping
 * the timing lines up. Reset to false by
 * clog_test_set_close_finalize_delay_us().
 *
 * @return true once some clog_close() call has entered the currently-armed
 *         delay; reset to false by clog_test_set_close_finalize_delay_us().
 */
bool clog_test_close_finalize_delay_entered(void);

/**
 * @brief Report how many distinct shared logging targets (root loggers, in
 *        the sense of clog_derive()'s own "shares the parent's target"
 *        wording) are currently live in this process, for testing.
 *
 * Reflects clog_slot_table.live_shareds's own element count directly; see
 * that field's doc comment in clogger.c for exactly what "live" means here
 * (a target's entire lifetime, from its first handle's acquisition to its
 * own actual free, independent of any individual handle's own state). Safe
 * to call from inside a freshly forked child (reads this process's own,
 * COW-duplicated copy of the table).
 *
 * @return The current live-shared-target count.
 */
size_t clog_test_live_shareds_count(void);

/**
 * @brief Directly invoke the internal gzip-compression helper used by log
 *        rotation, for testing.
 *
 * Exposes the same helper _rotate() itself calls to compress a rotated file,
 * so its failure-path cleanup can be verified directly: a failure that
 * occurs before the destination file was ever created (src not openable)
 * must leave anything already on disk at dst completely untouched, rather
 * than unconditionally deleting it.
 *
 * @param src Path to the uncompressed source file.
 * @param dst Destination path (conventionally src + ".gz").
 * @return true on success (dst is a valid gzip file, src is unlinked); false
 *         on failure (dst is removed only if this call itself created it;
 *         src is left untouched either way).
 */
bool clog_test_gzip_compress_file(const char *src, const char *dst);

/**
 * @brief Force (or stop forcing) the next clog_flush()-driven mutex
 *        initialization to fail, for testing.
 *
 * clog_flush() (and _clog_write()'s own FATAL-path pre-flush step) build a
 * fresh, stack-local synchronization object per call and initialize it via
 * mutex_init(); with default/NULL attributes this has no real failure path
 * that can be triggered portably from a test (unlike an ordinary heap
 * allocation, it does not go through any per-logger custom allocator), so
 * this hook lets a test exercise the corresponding "fail safely instead of
 * using a not-fully-initialized mutex" path deterministically.
 *
 * While armed, the very next such initialization reports failure without
 * actually calling the underlying primitive, then disarms itself
 * automatically so only that one call is affected.
 *
 * @param force true to force the next initialization to fail; false to
 *              disarm without waiting for it to fire.
 */
void clog_test_force_flush_mutex_init_failure(bool force);

/**
 * @brief Force (or stop forcing) the next clog_flush()-driven condition
 *        variable initialization to fail, for testing.
 *
 * The condition-variable counterpart to
 * clog_test_force_flush_mutex_init_failure(): exercises the path where the
 * mutex portion of clog_flush()'s per-call synchronization object
 * initializes successfully but the condition variable does not, which must
 * still release the already-initialized mutex rather than leaking it.
 *
 * @param force true to force the next initialization to fail; false to
 *              disarm without waiting for it to fire.
 */
void clog_test_force_flush_condvar_init_failure(bool force);

/**
 * @brief Directly invoke the internal last-resort record writer used when
 *        not even a small fallback placeholder fits in the target buffer,
 *        for testing.
 *
 * Exposes the same helper _clog_write() itself falls back to only when
 * _clog_build_record() cannot fit even its own small, fixed-size fallback
 * placeholder into the write buffer; a condition that cannot be reached
 * through the ordinary logger + log_* call path given this library's actual
 * buffer-sizing constants (see clogger.c's own doc comment on that
 * function), so this hook lets a test exercise its behavior (in
 * particular, whether a requested-but-omitted backtrace is still signalled)
 * directly and deterministically instead.
 *
 * @param logger        An open logger handle; this writes to that handle's
 *                       own underlying fd exactly as a real log_* call
 *                       would, honoring log rotation's byte accounting.
 * @param fmt            Output format to render the placeholder record in.
 * @param level          Severity level to render.
 * @param with_backtrace Whether to also signal that a backtrace was
 *                       requested but could not be included.
 */
void clog_test_write_unrepresentable_record(clog logger, clog_format_t fmt,
                                            clog_level_t level,
                                            bool with_backtrace);

/**
 * @brief Force (or stop forcing) the next internal write-buffer append to
 *        report a genuine allocation failure, for testing.
 *
 * clogger.c's internal buffer-append helpers only ever have a real failure
 * path to exercise when growth is actually needed; which this library's
 * own buffer-sizing constants make impossible to reach for a handful of
 * deliberately small, fixed-size records (e.g. the "backtrace unavailable"
 * marker _emit_backtrace_syslog_lines() builds for CLOG_FMT_SYSLOG). This
 * hook lets a test force that one append to fail regardless of whether
 * growth was genuinely required.
 *
 * While armed, the very next internal buffer append reports failure without
 * touching the underlying allocator at all, then disarms itself
 * automatically so only that one call is affected.
 *
 * @param force true to force the next check to fail; false to disarm
 *              without waiting for it to fire.
 */
void clog_test_force_next_buf_ensure_failure(bool force);

/**
 * @brief Directly invoke _emit_backtrace_syslog_lines()'s own
 *        "backtrace capture failed" marker-emission branch against logger's
 *        own shared target, for testing.
 *
 * Reaching this specific branch through the ordinary log_* call path
 * requires forcing backtrace capture to fail (see
 * clog_test_force_backtrace_capture_failure()); this hook goes one step
 * further and lets a test exercise that branch directly and in isolation,
 * so its own internal fallback (armed via
 * clog_test_force_next_buf_ensure_failure()) can be exercised
 * deterministically without any other, unrelated buffer append racing to
 * consume that one-shot hook first.
 *
 * @param logger An open logger handle; this writes to that handle's own
 *               underlying fd exactly as a real log_* call would, honoring
 *               log rotation's byte accounting.
 * @param level  Severity level to render.
 */
void clog_test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                        clog_level_t level);
#endif
