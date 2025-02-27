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
 *   ts=<ISO-8601-UTC> level=<L> src=<file>:<line> func=<fn> [fields] msg=<text>
 *
 * Backtrace (ERROR / ALERT / FATAL only) is appended as tab-indented
 * continuation lines that do not start with "ts=", allowing log aggregators
 * to group them with their parent record:
 *   \t#0 ./bin(fn+0x1a) [0x7f...]
 *   \t#1 ./bin(main+0x42) [0x7f...]
 *
 * Thread safety:
 *   All public functions are fully thread-safe. A single per-logger mutex
 *   serialises writes and state changes.
 *
 * Log rotation:
 *   Available only for file-backed loggers (clog_open_file_mp). Size rotation
 *   triggers after the write that crosses max_file_size. Time rotation
 *   triggers on the first write after the interval elapses. Rotated files
 *   are renamed <path>.<YYYYMMDDHHMMSS>; if more than max_rotated_files
 *   exist the oldest are deleted. Build with -rdynamic for resolved symbols
 *   in backtraces.
 */

#include <common.h>
#include <stdbool.h>
#include <sys/types.h> /* off_t */
#include <time.h>      /* time_t */

/* Remove any macro definition of clog from <complex.h> */
#ifdef clog
#undef clog
#endif

/* ========================================================================== */
/*                         LOG LEVELS                                         */
/* ========================================================================== */

typedef enum {
  CLOG_TRACE = 0,
  CLOG_DEBUG,
  CLOG_INFO,
  CLOG_WARN,
  CLOG_ERROR,
  CLOG_ALERT, /**< Action must be taken immediately; maps to syslog severity 1
               */
  CLOG_FATAL,
  CLOG_OFF /**< Disables all output when used as min_level */
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
 * Pass a pointer to this struct to clog_open_file_mp(). Fields with value 0
 * use their respective CLOG_DEFAULT_* constant.
 *
 * Example — rotate at 50 MiB, keep 14 files:
 * @code
 * clog_rotation_cfg_t cfg = {
 *     .size_rotation_enabled  = true,
 *     .max_file_size          = 50L * 1024L * 1024L,
 *     .time_rotation_enabled  = false,
 *     .max_rotated_files      = 14,
 * };
 * clog lg = clog_open_file_mp("/var/log/app.log", CLOG_INFO, &cfg, NULL);
 * @endcode
 */
typedef struct clog_rotation_cfg {
  bool size_rotation_enabled;
  off_t max_file_size; /**< Bytes; 0 → CLOG_DEFAULT_MAX_FILE_SIZE */
  bool time_rotation_enabled;
  time_t rotation_interval_secs; /**< Seconds; 0 →
                                    CLOG_DEFAULT_ROTATION_INTERVAL */
  int max_rotated_files;         /**< Files kept on disk; 0 → no limit */
} clog_rotation_cfg_t;

/* ========================================================================== */
/*                         LOGGER TYPE                                        */
/* ========================================================================== */

struct clogger;

/**
 * @brief Opaque logger handle.
 *
 * Obtain via clog_open_fd_mp() or clog_open_file_mp(). Release via
 * clog_close(). NULL is safe to pass to all functions — they are no-ops.
 */
typedef struct clogger *clog;

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
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, …).
 * @param min_level Messages below this level are silently dropped.
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or NULL on allocation failure, invalid mprocs,
 *         or if fd is closed, read-only, or otherwise invalid.
 */
clog clog_open_fd_mp(int fd, clog_level_t min_level,
                     ccol_memmgmt_procs_t *mprocs);

/**
 * @brief Create a logger writing to an already-open file descriptor.
 *
 * The file descriptor is not owned by the logger and will NOT be closed on
 * clog_close(). Log rotation is unavailable for fd-based loggers.
 *
 * @param fd        Open, writable file descriptor (1=stdout, 2=stderr, …).
 * @param min_level Messages below this level are silently dropped.
 * @return New logger handle, or NULL on allocation failure, or if fd is
 *         closed, read-only, or otherwise invalid.
 */
static inline __attribute__((always_inline)) clog
clog_open_fd(int fd, clog_level_t min_level) {
  return clog_open_fd_mp(fd, min_level, NULL);
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
 * @param mprocs    Custom allocator, or NULL to use malloc/free.
 * @return New logger handle, or NULL on failure (bad path, alloc error, etc.).
 */
clog clog_open_file_mp(const char *path, clog_level_t min_level,
                       const clog_rotation_cfg_t *cfg,
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
 * @return New logger handle, or NULL on failure (bad path, alloc error, etc.).
 */
static inline __attribute__((always_inline)) clog clog_open_file(
    const char *path, clog_level_t min_level, const clog_rotation_cfg_t *cfg) {
  return clog_open_file_mp(path, min_level, cfg, NULL);
}

/**
 * @brief Flush, close, and free all resources owned by the logger.
 *
 * After this call the handle must not be used. Safe to call with NULL.
 * If this is the last handle sharing an underlying file (root or all derived
 * loggers closed), the file descriptor is closed automatically.
 */
void clog_close(clog logger);

/**
 * @brief Derive a new logger from an existing one.
 *
 * The derived logger shares the parent's logging target (file descriptor) and
 * synchronisation mutex, so all writes — from the parent and every derived
 * logger — are serialised by the same lock and go to the same destination.
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
 * @param parent Source logger to derive from. NULL → returns NULL.
 * @return New derived logger handle, or NULL on allocation failure.
 */
clog clog_derive(clog parent);

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
  CLOG_FMT_JSON,       /**< NDJSON — one JSON object per line */
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
 * @param logger Logger handle. NULL is a no-op.
 * @param fmt    Desired output format.
 */
void clog_set_format(clog logger, clog_format_t fmt);

/**
 * @brief Return the current output format.
 *
 * @param logger Logger handle. NULL returns CLOG_FMT_LOGFMT.
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
 * @param logger   Logger handle. NULL is a no-op.
 * @param facility Desired facility code.
 */
void clog_set_facility(clog logger, clog_syslog_facility_t facility);

/**
 * @brief Return the current syslog facility.
 *
 * @param logger Logger handle. NULL returns CLOG_SYSLOG_USER.
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
 * @param key    Field name (non-empty; must not contain '=', ']', whitespace,
 *               '"', '\', or control characters; keys that violate this are
 *               silently ignored).
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
/*                         INTERNAL — DO NOT CALL DIRECTLY                    */
/* ========================================================================== */

/**
 * @brief Internal write function. Use the log_* macros instead.
 *
 * @param logger         Logger handle (may be NULL — produces no output).
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

/** Logs at FATAL level and appends a backtrace. */
#define log_fatal(l, fmt, ...)                                          \
  _clog_write((l), CLOG_FATAL, __FILE__, __LINE__, __func__, true, fmt, \
              ##__VA_ARGS__)

/** @} */
