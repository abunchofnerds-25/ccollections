/*
Copyright: Boaz Segev, 2018-2019
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

#ifndef H_FACIL_IO_H
/**
"facil.h" is the main header for the facil.io server platform.
*/
#define H_FACIL_IO_H

/* *****************************************************************************
 * Table of contents (find by subject):
 * =================
 * Version and helper macros
 * Helper String Information Type
 * Memory pool / custom allocator for short lived objects
 * Logging and testing helpers
 *
 * Connection Callback (Protocol) Management
 * Listening to Incoming Connections
 * Connecting to remote servers as a client
 * Starting the IO reactor and reviewing it's state
 * Socket / Connection Functions
 * Connection Read / Write Hooks, for overriding the system calls
 * Concurrency overridable functions
 * Connection Task scheduling
 * Event / Task scheduling
 * Startup / State Callbacks (fork, start up, idle, etc')
 * Lower Level API - for special circumstances, use with care under
 *
 * Pub/Sub / Cluster Messages API
 * Cluster Messages and Pub/Sub
 * Cluster / Pub/Sub Middleware and Extensions ("Engines")
 *
 * Atomic Operations and Spin Locking Helper Functions
 * Simple Constant Time Operations
 * Byte Swapping and Network Order
 *
 * Converting Numbers to Strings (and back)
 * Strings to Numbers
 * Numbers to Strings* Random Generator Functions
 *
 * SipHash
 * SHA-1
 * SHA-2
 * Base64 (URL) encoding
 *
 * Memory Allocator Details
 *
 * Spin locking Implementation
 *
 ******** facil.io Data Types (String, Set / Hash Map, Linked Lists, etc')
 *
 * These types can be included by defining the macros and (re)including fio.h.
 *
 *
 *
 *                      #ifdef FIO_INCLUDE_LINKED_LIST
 *
 * Linked List Helpers
 * Independent Linked List API
 * Embedded Linked List API* Independent Linked List Implementation
 * Embeded Linked List Implementation
 *
 *
 *
 *                      #ifdef FIO_INCLUDE_STR
 *
 * String Helpers
 * String API - Initialization and Destruction
 * String API - String state (data pointers, length, capacity, etc')
 * String API - Memory management
 * String API - UTF-8 State
 * String Implementation - state (data pointers, length, capacity, etc')
 * String Implementation - Memory management
 * String Implementation - UTF-8 State
 * String Implementation - Content Manipulation and Review
 *
 *
 *
 *            #ifdef FIO_ARY_NAME - can be included more than once
 *
 * Dynamic Array Data-Store
 * Array API
 * Array Type
 * Array Memory Management
 * Array API implementation
 * Array Testing
 *
 *
 *
 *            #ifdef FIO_SET_NAME - can be included more than once
 *
 * Set / Hash Map Data-Store
 * Set / Hash Map API
 * Set / Hash Map Internal Data Structures
 * Set / Hash Map Internal Helpers
 * Set / Hash Map Implementation
 *
 *****************************************************************************
 */

/* *****************************************************************************
Version and helper macros
***************************************************************************** */

#define FIO_VERSION_MAJOR 0
#define FIO_VERSION_MINOR 7
#define FIO_VERSION_PATCH 4
#define FIO_VERSION_BETA 0

/* Automatically convert version data to a string constant - ignore these two */
#define FIO_MACRO2STR_STEP2(macro) #macro
#define FIO_MACRO2STR(macro) FIO_MACRO2STR_STEP2(macro)

/** The facil.io version as a String literal */
#if FIO_VERSION_BETA
#define FIO_VERSION_STRING                                \
  FIO_MACRO2STR(FIO_VERSION_MAJOR)                        \
  "." FIO_MACRO2STR(FIO_VERSION_MINOR) "." FIO_MACRO2STR( \
      FIO_VERSION_PATCH) ".beta" FIO_MACRO2STR(FIO_VERSION_BETA)
#else
#define FIO_VERSION_STRING         \
  FIO_MACRO2STR(FIO_VERSION_MAJOR) \
  "." FIO_MACRO2STR(FIO_VERSION_MINOR) "." FIO_MACRO2STR(FIO_VERSION_PATCH)
#endif

#ifndef FIO_MAX_SOCK_CAPACITY
/**
 * The maximum number of connections per worker process.
 */
#define FIO_MAX_SOCK_CAPACITY 131072
#endif

#ifndef FIO_CPU_CORES_LIMIT
/**
 * If facil.io detects more CPU cores than the number of cores stated in the
 * FIO_CPU_CORES_LIMIT, it will assume an error and cap the number of cores
 * detected to the assigned limit.
 *
 * This is only relevant to automated values, when running facil.io with zero
 * threads and processes, which invokes a large matrix of workers and threads
 * (see {facil_run})
 *
 * The default auto-detection cap is set at 8 cores. The number is arbitrary
 * (historically the number 7 was used after testing `malloc` race conditions on
 * a MacBook Pro).
 *
 * This does NOT effect manually set (non-zero) worker/thread values.
 */
#define FIO_CPU_CORES_LIMIT 8
#endif

#ifndef FIO_DEFER_THROTTLE_PROGRESSIVE
/**
 * The progressive throttling model makes concurrency and parallelism more
 * likely.
 *
 * Otherwise threads are assumed to be intended for "fallback" in case of slow
 * user code, where a single thread should be active most of the time and other
 * threads are activated only when that single thread is slow to perform.
 */
#define FIO_DEFER_THROTTLE_PROGRESSIVE 1
#endif

#ifndef FIO_PRINT_STATE
/**
 * Enables the depraceted FIO_LOG_STATE(msg,...) macro, which prints information
 * level messages to stderr.
 */
#define FIO_PRINT_STATE 0
#endif

#ifndef FIO_LOG_LENGTH_LIMIT
/**
 * Since logging uses stack memory rather than dynamic allocation, it's memory
 * usage must be limited to avoid exploding the stack. The following sets the
 * memory used for a logging event.
 */
#define FIO_LOG_LENGTH_LIMIT 2048
#endif

#ifndef FIO_IGNORE_MACRO
/**
 * This is used internally to ignore macros that shadow functions (avoiding
 * named arguments when required).
 */
#define FIO_IGNORE_MACRO
#endif

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if !defined(__GNUC__) && !defined(__clang__) && !defined(FIO_GNUC_BYPASS)
#define __attribute__(...)
#define __has_include(...) 0
#define __has_builtin(...) 0
#define FIO_GNUC_BYPASS 1
#elif !defined(__clang__) && !defined(__has_builtin)
/* E.g: GCC < 6.0 doesn't support __has_builtin */
#define __has_builtin(...) 0
#define FIO_GNUC_BYPASS 1
#endif

#if defined(__GNUC__) && (__GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 5))
/* GCC < 4.5 doesn't support deprecation reason string */
#define deprecated(reason) deprecated
#endif

#ifndef FIO_FUNC
#define FIO_FUNC static __attribute__((unused))
#endif

#if defined(__FreeBSD__)
#include <netinet/in.h>
#include <sys/socket.h>
#endif

/* *****************************************************************************
Patch for OSX version < 10.12 from https://stackoverflow.com/a/9781275/4025095
***************************************************************************** */
#if defined(__MACH__) && !defined(CLOCK_REALTIME)
#include <sys/time.h>
#define CLOCK_REALTIME 0
#define clock_gettime patch_clock_gettime
// clock_gettime is not implemented on older versions of OS X (< 10.12).
// If implemented, CLOCK_REALTIME will have already been defined.
static inline int patch_clock_gettime(int clk_id, struct timespec *t) {
  struct timeval now;
  int rv = gettimeofday(&now, NULL);
  if (rv) return rv;
  t->tv_sec = now.tv_sec;
  t->tv_nsec = now.tv_usec * 1000;
  return 0;
  (void)clk_id;
}
#endif

/* *****************************************************************************
C++ extern start
***************************************************************************** */
/* support C++ */
#ifdef __cplusplus
extern "C" {
/* C++ keyword was deprecated */
#define register
#endif

/* *****************************************************************************
Helper String Information Type
***************************************************************************** */

#ifndef FIO_STR_INFO_TYPE
/** A string information type, reports information about a C string. */
typedef struct fio_str_info_s {
  size_t capa; /* Buffer capacity, if the string is writable. */
  size_t len;  /* String length. */
  char *data;  /* String's first byte. */
} fio_str_info_s;
#define FIO_STR_INFO_TYPE
#endif

/* *****************************************************************************












Memory pool / custom allocator for short lived objects












***************************************************************************** */

/* inform the compiler that the returned value is aligned on 16 byte marker.
 * NOTE: fio_malloc/fio_calloc/fio_realloc/fio_realloc2/fio_mmap can be
 * redirected at runtime to a caller-supplied custom allocator (see
 * fio_set_mem_mgmt_procs / chttpsvr_set_engine_mem_mgmt_procs); this
 * attribute is NOT conditioned on whether that redirection is active, so
 * every custom allocator installed this way must itself return memory
 * aligned to at least 16 bytes, or every call site that sees these
 * declarations is building on a false compiler assumption. See the
 * "Hard requirement" note on chttpsvr_set_engine_mem_mgmt_procs
 * (chttpserver.h) and the runtime check in fio.c's
 * _fio_mem_procs_check_align16. */
#if FIO_FORCE_MALLOC
#define FIO_ALIGN
#define FIO_ALIGN_NEW
#elif __clang__ || __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 8)
#define FIO_ALIGN __attribute__((assume_aligned(16)))
#define FIO_ALIGN_NEW __attribute__((malloc, assume_aligned(16)))
#else
#define FIO_ALIGN
#define FIO_ALIGN_NEW
#endif

/**
 * Allocates memory using a per-CPU core block memory pool.
 * Memory is zeroed out.
 *
 * Allocations above FIO_MEMORY_BLOCK_ALLOC_LIMIT (16Kb when using 32Kb blocks)
 * will be redirected to `mmap`, as if `fio_mmap` was called.
 */
void *FIO_ALIGN_NEW fio_malloc(size_t size);

/**
 * same as calling `fio_malloc(size_per_unit * unit_count)`;
 *
 * Allocations above FIO_MEMORY_BLOCK_ALLOC_LIMIT (16Kb when using 32Kb blocks)
 * will be redirected to `mmap`, as if `fio_mmap` was called.
 */
void *FIO_ALIGN_NEW fio_calloc(size_t size_per_unit, size_t unit_count);

/** Frees memory that was allocated using this library. */
void fio_free(void *ptr);

/**
 * Re-allocates memory. An attempt to avoid copying the data is made only for
 * big memory allocations (larger than FIO_MEMORY_BLOCK_ALLOC_LIMIT).
 */
void *FIO_ALIGN fio_realloc(void *ptr, size_t new_size);

/**
 * Re-allocates memory. An attempt to avoid copying the data is made only for
 * big memory allocations (larger than FIO_MEMORY_BLOCK_ALLOC_LIMIT).
 *
 * This variation is slightly faster as it might copy less data.
 */
void *FIO_ALIGN fio_realloc2(void *ptr, size_t new_size, size_t copy_length);

/**
 * Allocates memory directly using `mmap`, this is prefered for objects that
 * both require almost a page of memory (or more) and expect a long lifetime.
 *
 * However, since this allocation will invoke the system call (`mmap`), it will
 * be inherently slower.
 *
 * `fio_free` can be used for deallocating the memory.
 */
void *FIO_ALIGN_NEW fio_mmap(size_t size);

/**
 * When forking is called manually, call this function to reset the facil.io
 * memory allocator's locks.
 */
void fio_malloc_after_fork(void);

/* ===========================================================================
 * Custom memory management via ccol_memmgmt_procs_t
 *
 * By default fio_malloc/fio_calloc/fio_free/fio_realloc/fio_realloc2/fio_mmap
 * are backed by facil.io's own per-CPU block/mmap arena.  Calling
 * fio_set_mem_mgmt_procs() with a non-NULL ccol_memmgmt_procs_t redirects all
 * of the above (and every internal facio allocation that goes through them)
 * to the supplied malloc/free/calloc/realloc instead, bypassing the arena
 * entirely.  Passing NULL reverts to the default arena behavior.
 *
 * fio.h intentionally does NOT include common.h; struct ccol_memmgmt_procs_t
 * is forward-declared so the prototypes compile without pulling in the full
 * ccol API.
 * =========================================================================*/
struct ccol_memmgmt_procs_t;
void fio_set_mem_mgmt_procs(const struct ccol_memmgmt_procs_t *mp);
bool fio_has_mem_mgmt_procs(void);

#undef FIO_ALIGN

/* *****************************************************************************












Logging and testing helpers












***************************************************************************** */

/** Logging level of zero (no logging). */
#define FIO_LOG_LEVEL_NONE 0
/** Log fatal errors. */
#define FIO_LOG_LEVEL_FATAL 1
/** Log errors and fatal errors. */
#define FIO_LOG_LEVEL_ERROR 2
/** Log warnings, errors and fatal errors. */
#define FIO_LOG_LEVEL_WARNING 3
/** Log every message (info, warnings, errors and fatal errors). */
#define FIO_LOG_LEVEL_INFO 4
/** Log everything, including debug messages. */
#define FIO_LOG_LEVEL_DEBUG 5

#if FIO_LOG_LENGTH_LIMIT > 128
#define FIO_LOG____LENGTH_ON_STACK FIO_LOG_LENGTH_LIMIT
#define FIO_LOG____LENGTH_BORDER (FIO_LOG_LENGTH_LIMIT - 32)
#else
#define FIO_LOG____LENGTH_ON_STACK (FIO_LOG_LENGTH_LIMIT + 32)
#define FIO_LOG____LENGTH_BORDER FIO_LOG_LENGTH_LIMIT
#endif

/* ===========================================================================
 * Logging via clogger
 *
 * All facio log output is routed through the clog handle registered with
 * fio_set_logger().  The handle is protected by an internal rwlock so that
 * fio_set_logger() and fio_has_logger() are safe to call concurrently with
 * active logging.  fio_set_logger() closes the previously registered handle
 * (if any) after swapping it out.
 *
 * The __fio_log_* functions are implemented in fio.c; they forward each
 * log call to the registered clog handle.  fio.h intentionally does NOT
 * include clogger.h; struct clogger is forward-declared so that the
 * function prototypes compile without pulling in the full clogger API.
 * =========================================================================*/
struct clogger;
void fio_set_logger(struct clogger *cl);
bool fio_has_logger(void);

void __fio_log_debug(const char *file, int line, const char *func,
                     const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void __fio_log_info(const char *file, int line, const char *func,
                    const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void __fio_log_warn(const char *file, int line, const char *func,
                    const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void __fio_log_error(const char *file, int line, const char *func,
                     const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void __fio_log_fatal(const char *file, int line, const char *func,
                     const char *fmt, ...)
    __attribute__((noreturn, format(printf, 4, 5)));

#undef FIO_LOG_DEBUG
#define FIO_LOG_DEBUG(...) \
  __fio_log_debug(__FILE__, __LINE__, __func__, __VA_ARGS__)
#undef FIO_LOG_INFO
#define FIO_LOG_INFO(...) \
  __fio_log_info(__FILE__, __LINE__, __func__, __VA_ARGS__)
#undef FIO_LOG_WARNING
#define FIO_LOG_WARNING(...) \
  __fio_log_warn(__FILE__, __LINE__, __func__, __VA_ARGS__)
#undef FIO_LOG_ERROR
#define FIO_LOG_ERROR(...) \
  __fio_log_error(__FILE__, __LINE__, __func__, __VA_ARGS__)
#undef FIO_LOG_FATAL
#define FIO_LOG_FATAL(...) \
  __fio_log_fatal(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define FIO_LOG_STATE(...)

#undef FIO_ASSERT
#define FIO_ASSERT(cond, ...)                                     \
  do {                                                            \
    if (!(cond)) {                                                \
      __fio_log_fatal(__FILE__, __LINE__, __func__, __VA_ARGS__); \
    }                                                             \
  } while (0)

#ifndef FIO_ASSERT_ALLOC
/** Tests for an allocation failure. */
#define FIO_ASSERT_ALLOC(ptr)                       \
  do {                                              \
    if (!(ptr)) {                                   \
      __fio_log_fatal(__FILE__, __LINE__, __func__, \
                      "memory allocation failed."); \
    }                                               \
  } while (0)
#endif

#if DEBUG
#define FIO_ASSERT_DEBUG(cond, ...) \
  if (!(cond)) {                    \
    FIO_LOG_DEBUG(__VA_ARGS__);     \
    perror("     errno");           \
    exit(-1);                       \
  }
#else
#define FIO_ASSERT_DEBUG(...)
#endif

/* *****************************************************************************












Connection Callback (Protocol) Management












***************************************************************************** */

typedef struct fio_protocol_s fio_protocol_s;
/**************************************************************************/ /**
* The Protocol

The Protocol struct defines the callbacks used for the connection and sets it's
behaviour. The Protocol struct is part of facil.io's core design.

For concurrency reasons, a protocol instance SHOULD be unique to each
connections. Different connections shouldn't share a single protocol object
(callbacks and data can obviously be shared).

All the callbacks receive a unique connection ID (a localized UUID) that can be
converted to the original file descriptor when in need.

This allows facil.io to prevent old connection handles from sending data
to new connections after a file descriptor is "recycled" by the OS.
*/
struct fio_protocol_s {
  /** Called when a data is available, but will not run concurrently */
  void (*on_data)(intptr_t uuid, fio_protocol_s *protocol);
  /** called once all pending `fio_write` calls are finished. */
  void (*on_ready)(intptr_t uuid, fio_protocol_s *protocol);
  /**
   * Called when the server is shutting down, immediately before closing the
   * connection.
   *
   * The callback runs within a {FIO_PR_LOCK_TASK} lock, so it will never run
   * concurrently with {on_data} or other connection specific tasks.
   *
   * The `on_shutdown` callback should return 0 to close the socket or a number
   * between 1..254 to delay the socket closure by that amount of time.
   *
   * Once the socket wass marked for closure, facil.io will allow 8 seconds for
   * all the data to be sent before forcfully closing the socket (regardless of
   * state).
   *
   * If the `on_shutdown` returns 255, the socket is ignored and it will be
   * abruptly terminated when all other sockets have finished their graceful
   * shutdown procedure.
   */
  uint8_t (*on_shutdown)(intptr_t uuid, fio_protocol_s *protocol);
  /** Called when the connection was closed, but will not run concurrently */
  void (*on_close)(intptr_t uuid, fio_protocol_s *protocol);
  /** called when a connection's timeout was reached */
  void (*ping)(intptr_t uuid, fio_protocol_s *protocol);
  /** private metadata used by facil. */
  size_t rsv;
};

/**
 * Attaches (or updates) a protocol object to a socket UUID.
 *
 * The new protocol object can be NULL, which will detach ("hijack"), the
 * socket .
 *
 * The old protocol's `on_close` (if any) will be scheduled.
 *
 * On error, the new protocol's `on_close` callback will be called immediately.
 */
void fio_attach(intptr_t uuid, fio_protocol_s *protocol);

/**
 * Sets a socket to non blocking state.
 *
 * This will also set the O_CLOEXEC flag for the file descriptor.
 *
 * This function is called automatically for the new socket, when using
 * `fio_accept` or `fio_connect`.
 */
int fio_set_non_block(int fd);

/**
 * Returns the maximum number of open files facil.io can handle per worker
 * process.
 *
 * Total OS limits might apply as well but aren't shown.
 *
 * The value of 0 indicates either that the facil.io library wasn't initialized
 * yet or that it's resources were released.
 */
size_t fio_capa(void);

/** Sets a timeout for a specific connection (only when running and valid). */
void fio_timeout_set(intptr_t uuid, uint8_t timeout);

/**
 * "Touches" a socket connection, resetting it's timeout counter.
 */
void fio_touch(intptr_t uuid);

enum fio_io_event {
  FIO_EVENT_ON_DATA,
  FIO_EVENT_ON_READY,
  FIO_EVENT_ON_TIMEOUT
};
/** Schedules an IO event, even if it did not occur. */
void fio_force_event(intptr_t uuid, enum fio_io_event);

/**
 * Unconditionally re-arms the reactor's read interest for `uuid` (an
 * idempotent `epoll_ctl`-equivalent MOD/ADD), independent of the
 * `scheduled` bookkeeping that guards `fio_force_event(FIO_EVENT_ON_DATA)`.
 *
 * `fio_force_event(FIO_EVENT_ON_DATA)` both (a) schedules an immediate
 * follow-up read attempt and (b) claims the per-connection `scheduled` lock
 * as a side effect. When called from *within* an active `on_data`
 * invocation (as `fio_tls_handshake` does right after a handshake
 * completes), that side effect wins a race against the *current*
 * invocation's own post-callback re-arm check in `deferred_on_data`,
 * causing it to skip re-arming the (edge-triggered, one-shot) poll
 * interest entirely; on the assumption that the scheduled follow-up will
 * handle it. If that follow-up's own read attempt finds no data yet
 * available, nothing is left to ever re-arm the socket, even once data
 * later arrives: the reactor was never told to watch for it again. Call
 * this alongside `fio_force_event(FIO_EVENT_ON_DATA)` in such cases to
 * close that window unconditionally.
 */
void fio_force_read_rearm(intptr_t uuid);

/**
 * Unconditionally re-arms the reactor's write interest for `uuid` (an
 * idempotent `epoll_ctl`-equivalent MOD/ADD); the write-side mirror of
 * `fio_force_read_rearm`.
 *
 * `on_ready` is normally only re-armed as a side effect of `fio_write2()`
 * queueing a packet: `deferred_on_ready`'s post-flush re-arm path
 * (`fio_poll_add_write`) only runs when there is still packet data pending,
 * never merely because a caller wants to be notified once more. Code that
 * drives its own raw writes directly on the fd instead of going through
 * `fio_write2` (e.g. client-mode TLS via `fio_tls_connection_write`/
 * `fio_tls_client_handshake_step`, which write straight through OpenSSL's
 * BIO layer) therefore has no other way to ask for a follow-up `on_ready`
 * once `WANT_WRITE`/a partial write is hit. Call this to arrange one.
 */
void fio_force_write_rearm(intptr_t uuid);

/**
 * Marks `uuid`'s underlying fd as "busy" for the duration of an external
 * raw read/wait operation performed OUTSIDE of `fio_read`/`fio_write`/
 * `fio_write2` (e.g. a worker thread's own `poll()`+`read()` loop that
 * bypasses the reactor entirely, such as `http1_stream_read`'s
 * worker-driven body ingestion).
 *
 * `fio_read` already marks its own read window busy internally (see
 * `fio_clear_fd`'s `rw_busy` field), which is what stops `fio_clear_fd`/
 * `fio_force_close` from synchronously `close()`ing the fd while a call is
 * still using it - critical because a synchronous close lets the OS hand
 * that exact fd number to a brand-new, unrelated connection before the
 * still-in-flight caller notices, corrupting whichever connection reads
 * next. Code that reads the fd directly, without going through `fio_read`,
 * gets none of that protection by default: in particular, a raw blocking
 * `poll()` call sitting idle on the fd is invisible to `fio_review_timeout`'s
 * idle-connection sweep only in the sense that the *sweep* doesn't know
 * a caller is mid-wait - it can still decide the connection has been idle
 * longer than its configured timeout and force-close it out from under
 * that `poll()`.
 *
 * Call this immediately before such a raw operation and pair it with
 * exactly one `fio_rw_busy_unmark` call afterward, on every return path
 * (including error paths) - the same discipline `fio_read` itself follows
 * internally. Safe to call on an already-invalid `uuid` (a no-op).
 */
void fio_rw_busy_mark(intptr_t uuid);

/** Releases one busy mark set by `fio_rw_busy_mark`. Safe to call on an
 * already-invalid `uuid` (a no-op). */
void fio_rw_busy_unmark(intptr_t uuid);

/**
 * Temporarily prevents `on_data` events from firing.
 *
 * The `on_data` event will be automatically rescheduled when (if) the socket's
 * outgoing buffer fills up or when `fio_force_event` is called with
 * `FIO_EVENT_ON_DATA`.
 *
 * Note: the function will work as expected when called within the protocol's
 * `on_data` callback and the `uuid` refers to a valid socket. Otherwise the
 * function might quietly fail.
 */
void fio_suspend(intptr_t uuid);

/* *****************************************************************************
Listening to Incoming Connections
***************************************************************************** */

/* Arguments for the fio_listen function */
struct fio_listen_args {
  /**
   * Called whenever a new connection is accepted.
   *
   * Should either call `fio_attach` or close the connection.
   */
  void (*on_open)(intptr_t uuid, void *udata);
  /** The network service / port. Defaults to "3000". */
  const char *port;
  /** The socket binding address. Defaults to the recommended NULL. */
  const char *address;
  /** a pointer to a `fio_tls_s` object, for SSL/TLS support (fio_tls.h). */
  void *tls;
  /** Opaque user data. */
  void *udata;
  /**
   * Called when the server starts (or a worker process is respawned), allowing
   * for further initialization, such as timed event scheduling or VM
   * initialization.
   *
   * This will be called separately for every worker process whenever it is
   * spawned.
   */
  void (*on_start)(intptr_t uuid, void *udata);
  /**
   * Called when the server is done, usable for cleanup.
   *
   * This will be called separately for every process. */
  void (*on_finish)(intptr_t uuid, void *udata);
};

/**
 * Sets up a network service on a listening socket.
 *
 * Returns the listening socket's uuid or -1 (on error).
 *
 * See the `fio_listen` Macro for details.
 */
intptr_t fio_listen(struct fio_listen_args args);

/************************************************************************ */ /**
Listening to Incoming Connections
===

Listening to incoming connections is pretty straight forward.

After a new connection is accepted, the `on_open` callback is called. `on_open`
should allocate the new connection's protocol and call `fio_attach` to attach
the protocol to the connection's uuid.

The protocol's `on_close` callback is expected to handle any cleanup required.

The following is an example echo server using facil.io:

```c
#include <fio.h>

// A callback to be called whenever data is available on the socket
static void echo_on_data(intptr_t uuid, fio_protocol_s *prt) {
  (void)prt; // we can ignore the unused argument
  // echo buffer
  char buffer[1024] = {'E', 'c', 'h', 'o', ':', ' '};
  ssize_t len;
  // Read to the buffer, starting after the "Echo: "
  while ((len = fio_read(uuid, buffer + 6, 1018)) > 0) {
    fprintf(stderr, "Read: %.*s", (int)len, buffer + 6);
    // Write back the message
    fio_write(uuid, buffer, len + 6);
    // Handle goodbye
    if ((buffer[6] | 32) == 'b' && (buffer[7] | 32) == 'y' &&
        (buffer[8] | 32) == 'e') {
      fio_write(uuid, "Goodbye.\n", 9);
      fio_close(uuid);
      return;
    }
  }
}

// A callback called whenever a timeout is reach
static void echo_ping(intptr_t uuid, fio_protocol_s *prt) {
  (void)prt; // we can ignore the unused argument
  fio_write(uuid, "Server: Are you there?\n", 23);
}

// A callback called if the server is shutting down...
// ... while the connection is still open
static uint8_t echo_on_shutdown(intptr_t uuid, fio_protocol_s *prt) {
  (void)prt; // we can ignore the unused argument
  fio_write(uuid, "Echo server shutting down\nGoodbye.\n", 35);
  return 0;
}

static void echo_on_close(intptr_t uuid, fio_protocol_s *proto) {
  fprintf(stderr, "Connection %p closed.\n", (void *)proto);
  free(proto);
  (void)uuid;
}

// A callback called for new connections
static void echo_on_open(intptr_t uuid, void *udata) {
  (void)udata; // ignore this
  // Protocol objects MUST be dynamically allocated when multi-threading.
  fio_protocol_s *echo_proto = malloc(sizeof(*echo_proto));
  *echo_proto = (fio_protocol_s){.service = "echo",
                                 .on_data = echo_on_data,
                                 .on_shutdown = echo_on_shutdown,
                                 .on_close = echo_on_close,
                                 .ping = echo_ping};
  fprintf(stderr, "New Connection %p received from %s\n", (void *)echo_proto,
          fio_peer_addr(uuid).data);
  fio_attach(uuid, echo_proto);
  fio_write2(uuid, .data.buffer = "Echo Service: Welcome\n", .length = 22,
             .after.dealloc = FIO_DEALLOC_NOOP);
  fio_timeout_set(uuid, 5);
}

int main() {
  // Setup a listening socket
  if (fio_listen(.port = "3000", .on_open = echo_on_open) == -1) {
    perror("No listening socket available on port 3000");
    exit(-1);
  }
  // Run the server and hang until a stop signal is received.
  fio_start(.threads = 4, .workers = 1);
}
```
*/
#define fio_listen(...) fio_listen((struct fio_listen_args){__VA_ARGS__})

/* *****************************************************************************
Starting the IO reactor and reviewing it's state
***************************************************************************** */

struct fio_start_args {
  /**
   * The number of threads to run in the thread pool. Has "smart" defaults.
   *
   *
   * A positive value will indicate a set number of threads (or workers).
   *
   * Zeros and negative values are fun and include an interesting shorthand:
   *
   * * Negative values indicate a fraction of the number of CPU cores. i.e.
   *   -2 will normally indicate "half" (1/2) the number of cores.
   *
   * * If the other option (i.e. `.workers` when setting `.threads`) is zero,
   *   it will be automatically updated to reflect the option's absolute value.
   *   i.e.:
   *   if .threads == -2 and .workers == 0,
   *   than facil.io will run 2 worker processes with (cores/2) threads per
   *   process.
   */
  int16_t threads;
  /** The number of worker processes to run. See `threads`. */
  int16_t workers;
};

/**
 * Starts the facil.io event loop. This function will return after facil.io is
 * done (after shutdown).
 *
 * See the `struct fio_start_args` details for any possible named arguments.
 *
 * This method blocks the current thread until the server is stopped (when a
 * SIGINT/SIGTERM is received).
 */
void fio_start(struct fio_start_args args);
#define fio_start(...) fio_start((struct fio_start_args){__VA_ARGS__})

/**
 * Attempts to stop the facil.io application. This only works within the Root
 * process. A worker process will simply respawn itself.
 */
void fio_stop(void);

/**
 * Returns the number of expected threads / processes to be used by facil.io.
 *
 * The pointers should start with valid values that match the expected threads /
 * processes values passed to `fio_start`.
 *
 * The data in the pointers will be overwritten with the result.
 */
void fio_expected_concurrency(int16_t *threads, int16_t *workers);

/**
 * Returns the number of worker processes if facil.io is running.
 *
 * (1 is returned when in single process mode, otherwise the number of workers)
 */
int16_t fio_is_running(void);

/**
 * Returns 1 if the current process is the master (root) process.
 *
 * Otherwise returns 0.
 */
int fio_is_master(void);

/**
 * Resets any existing signal handlers, restoring their state to before they
 * were set by facil.io.
 *
 * This stops both child reaping (`fio_reap_children`) and the default facil.io
 * signal handlers (i.e., CTRL-C).
 *
 * This function will be called automatically by facil.io whenever facil.io
 * stops.
 */
void fio_signal_handler_reset(void);

/**
 * Returns the last time the server reviewed any pending IO events.
 */
struct timespec fio_last_tick(void);

/**
 * Returns a C string detailing the IO engine selected during compilation.
 *
 * Valid values are "kqueue", "epoll" and "poll".
 */
char const *fio_engine(void);

/* *****************************************************************************
Socket / Connection Functions
***************************************************************************** */

/**
 * Creates a Unix or a TCP/IP socket and returns it's unique identifier.
 *
 * For TCP/IP server sockets (`is_server` is `1`), a NULL `address` variable is
 * recommended. Use "localhost" or "127.0.0.1" to limit access to the server
 * application.
 *
 * For TCP/IP client sockets (`is_server` is `0`), a remote `address` and `port`
 * combination will be required
 *
 * For Unix server or client sockets, set the `port` variable to NULL or `0`.
 *
 * Returns -1 on error. Any other value is a valid unique identifier.
 *
 * Note: facil.io uses unique identifiers to protect sockets from collisions.
 *       However these identifiers can be converted to the underlying file
 *       descriptor using the `fio_uuid2fd` macro.
 */
intptr_t fio_socket(const char *address, const char *port, uint8_t is_server);

/**
 * `fio_accept` accepts a new socket connection from a server socket - see the
 * server flag on `fio_socket`.
 *
 * Accepted connection are automatically set to non-blocking mode and the
 * O_CLOEXEC flag is set.
 *
 * NOTE: this function does NOT attach the socket to the IO reactor - see
 * `fio_attach`.
 */
intptr_t fio_accept(intptr_t srv_uuid);

/**
 * Returns 1 if the uuid refers to a valid and open, socket.
 *
 * Returns 0 if not.
 */
int fio_is_valid(intptr_t uuid);

/**
 * Returns 1 if the uuid is invalid or the socket is flagged to be closed.
 *
 * Returns 0 if the socket is valid, open and isn't flagged to be closed.
 */
int fio_is_closed(intptr_t uuid);

/**
 * `fio_close` marks the connection for disconnection once all the data was
 * sent. The actual disconnection will be managed by the `fio_flush` function.
 *
 * `fio_flash` will be automatically scheduled.
 */
void fio_close(intptr_t uuid);

/**
 * `fio_force_close` closes the connection immediately, without adhering to any
 * protocol restrictions and without sending any remaining data in the
 * connection buffer.
 */
void fio_force_close(intptr_t uuid);

/**
 * Returns the information available about the socket's peer address.
 *
 * If no information is available, the struct will be initialized with zero
 * (`addr == NULL`).
 * The information is only available when the socket was accepted using
 * `fio_accept` or opened using `fio_connect`.
 */
fio_str_info_s fio_peer_addr(intptr_t uuid);

/**
 * `fio_read` attempts to read up to count bytes from the socket into the
 * buffer starting at `buffer`.
 *
 * `fio_read`'s return values are wildly different then the native return
 * values and they aim at making far simpler sense.
 *
 * `fio_read` returns the number of bytes read (0 is a valid return value which
 * simply means that no bytes were read from the buffer).
 *
 * On a fatal connection error that leads to the connection being closed (or if
 * the connection is already closed), `fio_read` returns -1.
 *
 * The value 0 is the valid value indicating no data was read.
 *
 * Data might be available in the kernel's buffer while it is not available to
 * be read using `fio_read` (i.e., when using a transport layer, such as TLS).
 */
ssize_t fio_read(intptr_t uuid, void *buffer, size_t count);

/** The following structure is used for `fio_write2_fn` function arguments. */
typedef struct {
  union {
    /** The in-memory data to be sent. */
    const void *buffer;
    /** The data to be sent, if this is a file. */
    const intptr_t fd;
  } data;
  union {
    /**
     * This deallocation callback will be called when the packet is finished
     * with the buffer.
     *
     * If no deallocation callback is set, `free` (or `close`) will be used.
     *
     * Note: socket library functions MUST NEVER be called by a callback, or a
     * deadlock might occur.
     */
    void (*dealloc)(void *buffer);
    /**
     * This is an alternative deallocation callback accessor (same memory space
     * as `dealloc`) for conveniently setting the file `close` callback.
     *
     * Note: `sock` library functions MUST NEVER be called by a callback, or a
     * deadlock might occur.
     */
    void (*close)(intptr_t fd);
  } after;
  /** The length (size) of the buffer, or the amount of data to be sent from the
   * file descriptor.
   */
  uintptr_t length;
  /** Starting point offset from the buffer or file descriptor's beginning. */
  uintptr_t offset;
  /** The packet will be sent as soon as possible. */
  unsigned urgent : 1;
  /**
   * The data union contains the value of a file descriptor (`int`). i.e.:
   *  `.data.fd = fd` or `.data.buffer = (void*)fd;`
   */
  unsigned is_fd : 1;
  /** for internal use */
  unsigned rsv : 1;
  /** for internal use */
  unsigned rsv2 : 1;
} fio_write_args_s;

/**
 * `fio_write2_fn` is the actual function behind the macro `fio_write2`.
 */
ssize_t fio_write2_fn(intptr_t uuid, fio_write_args_s options);

/**
 * Schedules data to be written to the socket.
 *
 * `fio_write2` is similar to `fio_write`, except that it allows far more
 * flexibility.
 *
 * On error, -1 will be returned. Otherwise returns 0.
 *
 * See the `fio_write_args_s` structure for details.
 *
 * NOTE: The data is "moved" to the ownership of the socket, not copied. The
 * data will be deallocated according to the `.after.dealloc` function.
 */
#define fio_write2(uuid, ...) \
  fio_write2_fn(uuid, (fio_write_args_s){__VA_ARGS__})

/**
 * `fio_write` copies `legnth` data from the buffer and schedules the data to
 * be sent over the socket.
 *
 * The data isn't necessarily written to the socket. The actual writing to the
 * socket is handled by the IO reactor.
 *
 * On error, -1 will be returned. Otherwise returns 0.
 *
 * Returns the same values as `fio_write2`.
 */
// ssize_t fio_write(uintptr_t uuid, void *buffer, size_t legnth);
inline FIO_FUNC ssize_t fio_write(const intptr_t uuid, const void *buffer,
                                  const size_t length) {
  if (!length || !buffer) return 0;
  void *cpy = fio_malloc(length);
  if (!cpy) return -1;
  memcpy(cpy, buffer, length);
  return fio_write2(uuid, .data.buffer = cpy, .length = length,
                    .after.dealloc = fio_free);
}

/**
 * Sends data from a file as if it were a single atomic packet (sends up to
 * length bytes or until EOF is reached).
 *
 * Once the file was sent, the `source_fd` will be closed using `close`.
 *
 * The file will be buffered to the socket chunk by chunk, so that memory
 * consumption is capped. The system's `sendfile` might be used if conditions
 * permit.
 *
 * `offset` dictates the starting point for the data to be sent and length sets
 * the maximum amount of data to be sent.
 *
 * Returns -1 and closes the file on error. Returns 0 on success.
 */
inline FIO_FUNC ssize_t fio_sendfile(intptr_t uuid, intptr_t source_fd,
                                     off_t offset, size_t length) {
  return fio_write2(uuid, .data.fd = source_fd, .length = length, .is_fd = 1,
                    .offset = (uintptr_t)offset);
}

/**
 * Returns the number of `fio_write` calls that are waiting in the socket's
 * queue and haven't been processed.
 */
size_t fio_pending(intptr_t uuid);

/**
 * `fio_flush` attempts to write any remaining data in the internal buffer to
 * the underlying file descriptor and closes the underlying file descriptor once
 * if it's marked for closure (and all the data was sent).
 *
 * Return values: 1 will be returned if data remains in the buffer. 0
 * will be returned if the buffer was fully drained. -1 will be returned on an
 * error or when the connection is closed.
 *
 * errno will be set to EWOULDBLOCK if the socket's lock is busy.
 */
ssize_t fio_flush(intptr_t uuid);

/** Blocks until all the data was flushed from the buffer */
#define fio_flush_strong(uuid) \
  do {                         \
    errno = 0;                 \
  } while (fio_flush(uuid) > 0 || errno == EWOULDBLOCK)

/**
 * Convert between a facil.io connection's identifier (uuid) and system's fd.
 */
#define fio_uuid2fd(uuid) ((int)((uintptr_t)uuid >> 8))

/**
 * `fio_fd2uuid` takes an existing file decriptor `fd` and returns it's active
 * `uuid`.
 *
 * If the file descriptor was closed, __it will be registered as open__.
 *
 * If the file descriptor was closed directly (not using `fio_close`) or the
 * closure event hadn't been processed, a false positive will be possible. This
 * is not an issue, since the use of an invalid fd will result in the registry
 * being updated and the fd being closed.
 *
 * Returns -1 on error. Returns a valid socket (non-random) UUID.
 */
intptr_t fio_fd2uuid(int fd);

/**
 * `fio_fd2uuid` takes an existing file decriptor `fd` and returns it's active
 * `uuid`.
 *
 * If the file descriptor is marked as closed (wasn't opened / registered with
 * facil.io) the function returns -1;
 *
 * If the file descriptor was closed directly (not using `fio_close`) or the
 * closure event hadn't been processed, a false positive will be possible. This
 * is not an issue, since the use of an invalid fd will result in the registry
 * being updated and the fd being closed.
 *
 * Returns -1 on error. Returns a valid socket (non-random) UUID.
 */
intptr_t fio_fd2uuid(int fd);

/* *****************************************************************************
Connection Read / Write Hooks, for overriding the system calls
***************************************************************************** */

/**
 * The following struct is used for setting a the read/write hooks that will
 * replace the default system calls to `recv` and `write`.
 *
 * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
 * deadlock might occur.
 */
typedef struct fio_rw_hook_s {
  /**
   * Implement reading from a file descriptor. Should behave like the file
   * system `read` call, including the setup or errno to EAGAIN / EWOULDBLOCK.
   *
   * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
   * deadlock might occur.
   */
  ssize_t (*read)(intptr_t uuid, void *udata, void *buf, size_t count);
  /**
   * Implement writing to a file descriptor. Should behave like the file system
   * `write` call.
   *
   * If an internal buffer is implemented and it is full, errno should be set to
   * EWOULDBLOCK and the function should return -1.
   *
   * The function is expected to call the `flush` callback (or it's logic)
   * internally. Either `write` OR `flush` are called.
   *
   * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
   * deadlock might occur.
   */
  ssize_t (*write)(intptr_t uuid, void *udata, const void *buf, size_t count);
  /**
   * When implemented, this function will be called to flush any data remaining
   * in the internal buffer.
   *
   * The function should return the number of bytes remaining in the internal
   * buffer (0 is a valid response) or -1 (on error).
   *
   * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
   * deadlock might occur.
   */
  ssize_t (*flush)(intptr_t uuid, void *udata);
  /**
   * The `before_close` callback is called only once before closing the `uuid`
   * and it might not get called at all if an abnormal closure is detected.
   *
   * If the function returns a non-zero value, than closure will be delayed
   * until the `flush` returns 0 (or less). This allows a closure signal to be
   * sent by the read/write hook when such a signal is required.
   *
   * Note: facil.io library functions MUST NEVER be called by any r/w hook, or a
   * deadlock might occur.
   * */
  ssize_t (*before_close)(intptr_t uuid, void *udata);
  /**
   * Called to perform cleanup after the socket was closed or a new read/write
   * hook was set using `fio_rw_hook_set`.
   *
   * This callback is always called, even if `fio_rw_hook_set` fails.
   * */
  void (*cleanup)(void *udata);
} fio_rw_hook_s;

/** Sets a socket hook state (a pointer to the struct). */
int fio_rw_hook_set(intptr_t uuid, fio_rw_hook_s *rw_hooks, void *udata);

/**
 * Replaces an existing read/write hook with another from within a read/write
 * hook callback.
 *
 * Does NOT call any cleanup callbacks.
 *
 * Replaces existing udata. Call with the existing udata to keep it.
 *
 * Returns -1 on error, 0 on success.
 *
 * Note: this function is marked as unsafe, since it should only be called from
 *       within an existing read/write hook callback. Otherwise, data corruption
 *       might occur.
 */
int fio_rw_hook_replace_unsafe(intptr_t uuid, fio_rw_hook_s *rw_hooks,
                               void *udata);

/** The default Read/Write hooks used for system Read/Write (udata == NULL). */
extern const fio_rw_hook_s FIO_DEFAULT_RW_HOOKS;

/* *****************************************************************************
Concurrency overridable functions

These functions can be overridden so as to adjust for different environments.
***************************************************************************** */

/**
 * OVERRIDE THIS to replace the default pthread implementation.
 *
 * Accepts a pointer to a function and a single argument that should be executed
 * within a new thread.
 *
 * The function should allocate memory for the thread object and return a
 * pointer to the allocated memory that identifies the thread.
 *
 * On error NULL should be returned.
 */
void *fio_thread_new(void *(*thread_func)(void *), void *arg);

/**
 * OVERRIDE THIS to replace the default pthread implementation.
 *
 * Accepts a pointer returned from `fio_thread_new` (should also free any
 * allocated memory) and joins the associated thread.
 *
 * Return value is ignored.
 */
int fio_thread_join(void *p_thr);

/* *****************************************************************************
Connection Task scheduling
***************************************************************************** */

/**
 * This is used to lock the protocol againste concurrency collisions and
 * concurrent memory deallocation.
 *
 * However, there are three levels of protection that allow non-coliding tasks
 * to protect the protocol object from being deallocated while in use:
 *
 * * `FIO_PR_LOCK_TASK` - a task lock locks might change data owned by the
 *    protocol object. This task is used for tasks such as `on_data`.
 *
 * * `FIO_PR_LOCK_WRITE` - a lock that promises only to use static data (data
 *    that tasks never changes) in order to write to the underlying socket.
 *    This lock is used for tasks such as `on_ready` and `ping`
 *
 * * `FIO_PR_LOCK_STATE` - a lock that promises only to retrieve static data
 *    (data that tasks never changes), performing no actions. This usually
 *    isn't used for client side code (used internally by facil) and is only
 *     meant for very short locks.
 */
enum fio_protocol_lock_e {
  FIO_PR_LOCK_TASK = 0,
  FIO_PR_LOCK_WRITE = 1,
  FIO_PR_LOCK_STATE = 2
};

/** Named arguments for the `fio_defer` function. */
typedef struct {
  /** The type of task to be performed. Defaults to `FIO_PR_LOCK_TASK` but could
   * also be seto to `FIO_PR_LOCK_WRITE`. */
  enum fio_protocol_lock_e type;
  /** The task (function) to be performed. This is required. */
  void (*task)(intptr_t uuid, fio_protocol_s *, void *udata);
  /** An opaque user data that will be passed along to the task. */
  void *udata;
  /** A fallback task, in case the connection was lost. Good for cleanup. */
  void (*fallback)(intptr_t uuid, void *udata);
} fio_defer_iotask_args_s;

/**
 * Schedules a protected connection task. The task will run within the
 * connection's lock.
 *
 * If an error ocuurs or the connection is closed before the task can run, the
 * `fallback` task wil be called instead, allowing for resource cleanup.
 */
void fio_defer_io_task(intptr_t uuid, fio_defer_iotask_args_s args);
#define fio_defer_io_task(uuid, ...) \
  fio_defer_io_task((uuid), (fio_defer_iotask_args_s){__VA_ARGS__})

/* *****************************************************************************
Event / Task scheduling
***************************************************************************** */

/**
 * Defers a task's execution.
 *
 * Tasks are functions of the type `void task(void *, void *)`, they return
 * nothing (void) and accept two opaque `void *` pointers, user-data 1
 * (`udata1`) and user-data 2 (`udata2`).
 *
 * Returns -1 or error, 0 on success.
 */
int fio_defer(void (*task)(void *, void *), void *udata1, void *udata2);

/**
 * Performs all deferred tasks.
 */
void fio_defer_perform(void);

/** Returns true if there are deferred functions waiting for execution. */
int fio_defer_has_queue(void);

/* *****************************************************************************
Startup / State Callbacks (fork, start up, idle, etc')
***************************************************************************** */

/** a callback type signifier */
typedef enum {
  /** Called once during library initialization. */
  FIO_CALL_ON_INITIALIZE,
  /** Called once before starting up the IO reactor. */
  FIO_CALL_PRE_START,
  /** Called before each time the IO reactor forks a new worker. */
  FIO_CALL_BEFORE_FORK,
  /** Called after each fork (both in parent and workers). */
  FIO_CALL_AFTER_FORK,
  /** Called by a worker process right after forking. */
  FIO_CALL_IN_CHILD,
  /** Called by the master process after spawning a worker (after forking). */
  FIO_CALL_IN_MASTER,
  /** Called every time a *Worker* proceess starts. */
  FIO_CALL_ON_START,
  /** Called when facil.io enters idling mode. */
  FIO_CALL_ON_IDLE,
  /** Called before starting the shutdown sequence. */
  FIO_CALL_ON_SHUTDOWN,
  /** Called just before finishing up (both on chlid and parent processes). */
  FIO_CALL_ON_FINISH,
  /** Called by each worker the moment it detects the master process crashed. */
  FIO_CALL_ON_PARENT_CRUSH,
  /** Called by the parent (master) after a worker process crashed. */
  FIO_CALL_ON_CHILD_CRUSH,
  /** An alternative to the system's at_exit. */
  FIO_CALL_AT_EXIT,
  /** used for testing. */
  FIO_CALL_NEVER
} callback_type_e;

/** Adds a callback to the list of callbacks to be called for the event. */
void fio_state_callback_add(callback_type_e, void (*func)(void *), void *arg);

/** Removes a callback from the list of callbacks to be called for the event. */
int fio_state_callback_remove(callback_type_e, void (*func)(void *), void *arg);

/**
 * Forces all the existing callbacks to run, as if the event occurred.
 *
 * Callbacks are called from last to first (last callback executes first).
 *
 * During an event, changes to the callback list are ignored (callbacks can't
 * remove other callbacks for the same event).
 */
void fio_state_callback_force(callback_type_e);

/** Clears all the existing callbacks for the event. */
void fio_state_callback_clear(callback_type_e);

/* *****************************************************************************
Lower Level API - for special circumstances, use with care.
***************************************************************************** */

/**
 * This function allows out-of-task access to a connection's `fio_protocol_s`
 * object by attempting to acquire a locked pointer.
 *
 * CAREFUL: mostly, the protocol object will be locked and a pointer will be
 * sent to the connection event's callback. However, if you need access to the
 * protocol object from outside a running connection task, you might need to
 * lock the protocol to prevent it from being closed / freed in the background.
 *
 * facil.io uses three different locks:
 *
 * * FIO_PR_LOCK_TASK locks the protocol for normal tasks (i.e. `on_data`,
 * `fio_defer`, `fio_every`).
 *
 * * FIO_PR_LOCK_WRITE locks the protocol for high priority `fio_write`
 * oriented tasks (i.e. `ping`, `on_ready`).
 *
 * * FIO_PR_LOCK_STATE locks the protocol for quick operations that need to copy
 * data from the protocol's data structure.
 *
 * IMPORTANT: Remember to call `fio_protocol_unlock` using the same lock type.
 *
 * Returns NULL on error (lock busy == EWOULDBLOCK, connection invalid == EBADF)
 * and a pointer to a protocol object on success.
 *
 * On error, consider calling `fio_defer` or `defer` instead of busy waiting.
 * Busy waiting SHOULD be avoided whenever possible.
 */
fio_protocol_s *fio_protocol_try_lock(intptr_t uuid, enum fio_protocol_lock_e);
/** Don't unlock what you don't own... see `fio_protocol_try_lock` for
 * details. */
void fio_protocol_unlock(fio_protocol_s *pr, enum fio_protocol_lock_e);

/* *****************************************************************************











              Atomic Operations and Spin Locking Helper Functions











***************************************************************************** */

/* C11 Atomics are defined? */
#if defined(__ATOMIC_RELAXED)
/** An atomic exchange operation, returns previous value */
#define fio_atomic_xchange(p_obj, value) \
  __atomic_exchange_n((p_obj), (value), __ATOMIC_SEQ_CST)
/** An atomic addition operation */
#define fio_atomic_add(p_obj, value) \
  __atomic_add_fetch((p_obj), (value), __ATOMIC_SEQ_CST)
/** An atomic subtraction operation */
#define fio_atomic_sub(p_obj, value) \
  __atomic_sub_fetch((p_obj), (value), __ATOMIC_SEQ_CST)
/* Note: __ATOMIC_SEQ_CST is probably safer and __ATOMIC_ACQ_REL may be faster
 */

/* Select the correct compiler builtin method. */
#elif __has_builtin(__sync_add_and_fetch)
/** An atomic exchange operation, returns previous value */
/* A single `__sync_val_compare_and_swap((p_obj), *(p_obj), (value))` is NOT
 * an atomic exchange: the `*(p_obj)` read happens outside the CAS, so under
 * real contention another thread's write between that read and the CAS
 * makes the "expected" value stale, the CAS silently fails to store
 * `value`, and the macro nonetheless returns as if an exchange happened.
 * Retry the CAS with the freshly-observed value until it actually succeeds,
 * which is what a correct exchange requires. This path is currently dead
 * code on any C11-atomics-capable toolchain (see the `__ATOMIC_RELAXED`
 * branch above, which is what's actually compiled here) but every lock
 * primitive in this file (fio_trylock/fio_unlock, and everything built on
 * them) is defined in terms of fio_atomic_xchange, so it must be correct if
 * this branch is ever the one selected. */
#define fio_atomic_xchange(p_obj, value)                                    \
  ({                                                                        \
    __typeof__(*(p_obj)) __fio_ax_expected = *(p_obj);                      \
    __typeof__(*(p_obj)) __fio_ax_actual;                                   \
    while ((__fio_ax_actual = __sync_val_compare_and_swap(                  \
                (p_obj), __fio_ax_expected, (value))) != __fio_ax_expected) \
      __fio_ax_expected = __fio_ax_actual;                                  \
    __fio_ax_expected;                                                      \
  })
/** An atomic addition operation */
#define fio_atomic_add(p_obj, value) __sync_add_and_fetch((p_obj), (value))
/** An atomic subtraction operation */
#define fio_atomic_sub(p_obj, value) __sync_sub_and_fetch((p_obj), (value))

#elif __GNUC__ > 3
/** An atomic exchange operation, returns previous value */
/* A single `__sync_val_compare_and_swap((p_obj), *(p_obj), (value))` is NOT
 * an atomic exchange: the `*(p_obj)` read happens outside the CAS, so under
 * real contention another thread's write between that read and the CAS
 * makes the "expected" value stale, the CAS silently fails to store
 * `value`, and the macro nonetheless returns as if an exchange happened.
 * Retry the CAS with the freshly-observed value until it actually succeeds,
 * which is what a correct exchange requires. This path is currently dead
 * code on any C11-atomics-capable toolchain (see the `__ATOMIC_RELAXED`
 * branch above, which is what's actually compiled here) but every lock
 * primitive in this file (fio_trylock/fio_unlock, and everything built on
 * them) is defined in terms of fio_atomic_xchange, so it must be correct if
 * this branch is ever the one selected. */
#define fio_atomic_xchange(p_obj, value)                                    \
  ({                                                                        \
    __typeof__(*(p_obj)) __fio_ax_expected = *(p_obj);                      \
    __typeof__(*(p_obj)) __fio_ax_actual;                                   \
    while ((__fio_ax_actual = __sync_val_compare_and_swap(                  \
                (p_obj), __fio_ax_expected, (value))) != __fio_ax_expected) \
      __fio_ax_expected = __fio_ax_actual;                                  \
    __fio_ax_expected;                                                      \
  })
/** An atomic addition operation */
#define fio_atomic_add(p_obj, value) __sync_add_and_fetch((p_obj), (value))
/** An atomic subtraction operation */
#define fio_atomic_sub(p_obj, value) __sync_sub_and_fetch((p_obj), (value))

#else
#error Required builtin "__sync_add_and_fetch" not found.
#endif

/** An atomic based spinlock. */
typedef uint8_t volatile fio_lock_i;

/** The initail value of an unlocked spinlock. */
#define FIO_LOCK_INIT 0

/** returns 0 if the lock was acquired and a non-zero value on failure. */
FIO_FUNC inline int fio_trylock(fio_lock_i *lock);

/**
 * Releases a spinlock. Releasing an unacquired lock will break it.
 *
 * Returns a non-zero value on success, or 0 if the lock was in an unloacked
 * state.
 */
FIO_FUNC inline int fio_unlock(fio_lock_i *lock);

/** Returns a spinlock's state (non 0 == Busy). */
FIO_FUNC inline int fio_is_locked(fio_lock_i *lock);

/** Busy waits for the spinlock (CAREFUL). */
FIO_FUNC inline void fio_lock(fio_lock_i *lock);

/**
 * Nanosleep seems to be the most effective and efficient thread rescheduler.
 */
FIO_FUNC inline void fio_reschedule_thread(void);

/** Nanosleep the thread - a blocking throttle. */
FIO_FUNC inline void fio_throttle_thread(size_t nano_sec);

/* *****************************************************************************










                         Simple Constant Time Operations
                         ( boolean true / false and if )











***************************************************************************** */

/** Returns 1 if the expression is true (input isn't zero). */
FIO_FUNC inline uintptr_t fio_ct_true(uintptr_t cond) {
  // promise that the highest bit is set if any bits are set, than shift.
  return ((cond | (0 - cond)) >> ((sizeof(cond) << 3) - 1));
}

/** Returns 1 if the expression is false (input is zero). */
FIO_FUNC inline uintptr_t fio_ct_false(uintptr_t cond) {
  // fio_ct_true returns only one bit, XOR will inverse that bit.
  return fio_ct_true(cond) ^ 1;
}

/** Returns `a` if `cond` is boolean and true, returns b otherwise. */
FIO_FUNC inline uintptr_t fio_ct_if(uint8_t cond, uintptr_t a, uintptr_t b) {
  // b^(a^b) cancels b out. 0-1 => sets all bits.
  return (b ^ ((0 - (cond & 1)) & (a ^ b)));
}

/** Returns `a` if `cond` isn't zero (uses fio_ct_true), returns b otherwise. */
FIO_FUNC inline uintptr_t fio_ct_if2(uintptr_t cond, uintptr_t a, uintptr_t b) {
  // b^(a^b) cancels b out. 0-1 => sets all bits.
  return fio_ct_if(fio_ct_true(cond), a, b);
}

/* *****************************************************************************










                         Byte Swapping and Network Order
                       (Big Endian v.s Little Endian etc')











***************************************************************************** */

/** inplace byte swap 16 bit integer */
#if __has_builtin(__builtin_bswap16)
#define fio_bswap16(i) __builtin_bswap16((uint16_t)(i))
#else
/* A macro form of this (`((((i) & 0xFFU) << 8) | (((i) & 0xFF00U) >> 8))`)
 * would evaluate `i` twice, silently misbehaving for any side-effecting
 * argument (e.g. `fio_bswap16(x++)`). Defined as an inline function instead
 * - only reached when the compiler lacks __builtin_bswap16. */
FIO_FUNC inline uint16_t fio_bswap16(uint16_t i) {
  return (uint16_t)(((i & 0xFFU) << 8) | ((i & 0xFF00U) >> 8));
}
#endif
/** inplace byte swap 32 bit integer */
#if __has_builtin(__builtin_bswap32)
#define fio_bswap32(i) __builtin_bswap32((uint32_t)(i))
#else
/* see fio_bswap16 above for why this is an inline function, not a macro. */
FIO_FUNC inline uint32_t fio_bswap32(uint32_t i) {
  return (uint32_t)(((i & 0xFFUL) << 24) | ((i & 0xFF00UL) << 8) |
                    ((i & 0xFF0000UL) >> 8) | ((i & 0xFF000000UL) >> 24));
}
#endif
/** inplace byte swap 64 bit integer */
#if __has_builtin(__builtin_bswap64)
#define fio_bswap64(i) __builtin_bswap64((uint64_t)(i))
#else
/* see fio_bswap16 above for why this is an inline function, not a macro. */
FIO_FUNC inline uint64_t fio_bswap64(uint64_t i) {
  return (uint64_t)(((i & 0xFFULL) << 56) | ((i & 0xFF00ULL) << 40) |
                    ((i & 0xFF0000ULL) << 24) | ((i & 0xFF000000ULL) << 8) |
                    ((i & 0xFF00000000ULL) >> 8) |
                    ((i & 0xFF0000000000ULL) >> 24) |
                    ((i & 0xFF000000000000ULL) >> 40) |
                    ((i & 0xFF00000000000000ULL) >> 56));
}
#endif

/* Note: using BIG_ENDIAN invokes false positives on some systems */
#if !defined(__BIG_ENDIAN__)
/* nothing to do */
#elif (defined(__LITTLE_ENDIAN__) && !__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
#define __BIG_ENDIAN__ 1
#elif !defined(__BIG_ENDIAN__) && !defined(__BYTE_ORDER__) && \
    !defined(__LITTLE_ENDIAN__)
#error Could not detect byte order on this system.
#endif

#if __BIG_ENDIAN__

/** Local byte order to Network byte order, 16 bit integer */
#define fio_lton16(i) (i)
/** Local byte order to Network byte order, 32 bit integer */
#define fio_lton32(i) (i)
/** Local byte order to Network byte order, 62 bit integer */
#define fio_lton64(i) (i)

/** Network byte order to Local byte order, 16 bit integer */
#define fio_ntol16(i) (i)
/** Network byte order to Local byte order, 32 bit integer */
#define fio_ntol32(i) (i)
/** Network byte order to Local byte order, 62 bit integer */
#define fio_ntol64(i) (i)

#else /* Little Endian */

/** Local byte order to Network byte order, 16 bit integer */
#define fio_lton16(i) fio_bswap16((i))
/** Local byte order to Network byte order, 32 bit integer */
#define fio_lton32(i) fio_bswap32((i))
/** Local byte order to Network byte order, 62 bit integer */
#define fio_lton64(i) fio_bswap64((i))

/** Network byte order to Local byte order, 16 bit integer */
#define fio_ntol16(i) fio_bswap16((i))
/** Network byte order to Local byte order, 32 bit integer */
#define fio_ntol32(i) fio_bswap32((i))
/** Network byte order to Local byte order, 62 bit integer */
#define fio_ntol64(i) fio_bswap64((i))

#endif

/* fio_lrot32/64 and fio_rrot32/64 used to be macros that expanded `i` twice
 * and `bits` three times each - any call with a side-effecting argument
 * (`fio_lrot64(x++, n)`, a function call, etc.) would silently misbehave.
 * No current call site does that (they're all plain variable reads in
 * hashing hot paths), but defined as inline functions removes the hazard
 * entirely at no performance cost (trivially inlined by any optimizing
 * build, and this project always builds -O3). */
/** 32Bit left rotation, inlined. */
FIO_FUNC inline uint32_t fio_lrot32(uint32_t i, size_t bits) {
  return (i << (bits & 31UL)) | (i >> ((0 - bits) & 31UL));
}
/** 32Bit right rotation, inlined. */
FIO_FUNC inline uint32_t fio_rrot32(uint32_t i, size_t bits) {
  return (i >> (bits & 31UL)) | (i << ((0 - bits) & 31UL));
}

/** 64Bit left rotation, inlined. */
FIO_FUNC inline uint64_t fio_lrot64(uint64_t i, size_t bits) {
  return (i << (bits & 63UL)) | (i >> ((0 - bits) & 63UL));
}
/** 64Bit right rotation, inlined. */
FIO_FUNC inline uint64_t fio_rrot64(uint64_t i, size_t bits) {
  return (i >> (bits & 63UL)) | (i << ((0 - bits) & 63UL));
}

/** unknown size element - left rotation, inlined.
 * NOTE: unlike fio_lrot32/64 above, this one genuinely can't be a plain
 * inline function - `sizeof((i))` has to see the caller's own original
 * type to pick the right rotation width, which a function parameter of a
 * fixed type would lose. `i` and `bits` are each still expanded more than
 * once; only ever called (as of this writing) with plain variable
 * arguments with no side effects - keep it that way, or add a _Generic
 * dispatch to type-specific inline functions instead. */
#define fio_lrot(i, bits)                         \
  (((i) << ((bits) & ((sizeof((i)) << 3) - 1))) | \
   ((i) >> ((-(bits)) & ((sizeof((i)) << 3) - 1))))
/** unknown size element - right rotation, inlined. See fio_lrot's note. */
#define fio_rrot(i, bits)                         \
  (((i) >> ((bits) & ((sizeof((i)) << 3) - 1))) | \
   ((i) << ((-(bits)) & ((sizeof((i)) << 3) - 1))))

/** Converts an unaligned network ordered byte stream to a 16 bit number. */
#define fio_str2u16(c)                                 \
  ((uint16_t)(((uint16_t)(((uint8_t *)(c))[0]) << 8) | \
              (uint16_t)(((uint8_t *)(c))[1])))
/** Converts an unaligned network ordered byte stream to a 32 bit number. */
#define fio_str2u32(c)                                  \
  ((uint32_t)(((uint32_t)(((uint8_t *)(c))[0]) << 24) | \
              ((uint32_t)(((uint8_t *)(c))[1]) << 16) | \
              ((uint32_t)(((uint8_t *)(c))[2]) << 8) |  \
              (uint32_t)(((uint8_t *)(c))[3])))

/** Converts an unaligned network ordered byte stream to a 64 bit number. */
#define fio_str2u64(c)                                  \
  ((uint64_t)((((uint64_t)((uint8_t *)(c))[0]) << 56) | \
              (((uint64_t)((uint8_t *)(c))[1]) << 48) | \
              (((uint64_t)((uint8_t *)(c))[2]) << 40) | \
              (((uint64_t)((uint8_t *)(c))[3]) << 32) | \
              (((uint64_t)((uint8_t *)(c))[4]) << 24) | \
              (((uint64_t)((uint8_t *)(c))[5]) << 16) | \
              (((uint64_t)((uint8_t *)(c))[6]) << 8) | (((uint8_t *)(c))[7])))

/* fio_u2str16/32/64 used to be macros that expanded `buffer` 2/4/8 times and
 * `i` the same number of times - any call with a side-effecting argument
 * would silently misbehave (e.g. write through the wrong offset, or convert
 * a different value in each expansion). No current call site does that,
 * but defined as inline functions removes the hazard entirely at no
 * performance cost. */
/** Writes a local 16 bit number to an unaligned buffer in network order. */
FIO_FUNC inline void fio_u2str16(void *buffer, uint16_t i) {
  ((uint8_t *)buffer)[0] = (uint8_t)((i >> 8) & 0xFF);
  ((uint8_t *)buffer)[1] = (uint8_t)(i & 0xFF);
}

/** Writes a local 32 bit number to an unaligned buffer in network order. */
FIO_FUNC inline void fio_u2str32(void *buffer, uint32_t i) {
  ((uint8_t *)buffer)[0] = (uint8_t)((i >> 24) & 0xFF);
  ((uint8_t *)buffer)[1] = (uint8_t)((i >> 16) & 0xFF);
  ((uint8_t *)buffer)[2] = (uint8_t)((i >> 8) & 0xFF);
  ((uint8_t *)buffer)[3] = (uint8_t)(i & 0xFF);
}

/** Writes a local 64 bit number to an unaligned buffer in network order. */
FIO_FUNC inline void fio_u2str64(void *buffer, uint64_t i) {
  ((uint8_t *)buffer)[0] = (uint8_t)((i >> 56) & 0xFF);
  ((uint8_t *)buffer)[1] = (uint8_t)((i >> 48) & 0xFF);
  ((uint8_t *)buffer)[2] = (uint8_t)((i >> 40) & 0xFF);
  ((uint8_t *)buffer)[3] = (uint8_t)((i >> 32) & 0xFF);
  ((uint8_t *)buffer)[4] = (uint8_t)((i >> 24) & 0xFF);
  ((uint8_t *)buffer)[5] = (uint8_t)((i >> 16) & 0xFF);
  ((uint8_t *)buffer)[6] = (uint8_t)((i >> 8) & 0xFF);
  ((uint8_t *)buffer)[7] = (uint8_t)(i & 0xFF);
}

/* *****************************************************************************










                       Converting Numbers to Strings (and back)











***************************************************************************** */

/* *****************************************************************************
Strings to Numbers
***************************************************************************** */

/**
 * A helper function that converts between String data to a signed int64_t.
 *
 * Numbers are assumed to be in base 10. Octal (`0###`), Hex (`0x##`/`x##`) and
 * binary (`0b##`/ `b##`) are recognized as well. For binary Most Significant
 * Bit must come first.
 *
 * The most significant difference between this function and `strtol` (aside of
 * API design), is the added support for binary representations.
 */
int64_t fio_atol(char **pstr);

/** A helper function that converts between String data to a signed double. */
double fio_atof(char **pstr);

/* *****************************************************************************
Numbers to Strings
***************************************************************************** */

/**
 * A helper function that writes a signed int64_t to a string.
 *
 * No overflow guard is provided, make sure there's at least 68 bytes
 * available (for base 2).
 *
 * Offers special support for base 2 (binary), base 8 (octal), base 10 and base
 * 16 (hex). An unsupported base will silently default to base 10. Prefixes
 * aren't added (i.e., no "0x" or "0b" at the beginning of the string).
 *
 * Returns the number of bytes actually written (excluding the NUL
 * terminator).
 */
size_t fio_ltoa(char *dest, int64_t num, uint8_t base);

/**
 * A helper function that converts between a double to a string.
 *
 * No overflow guard is provided, make sure there's at least 130 bytes
 * available (for base 2).
 *
 * Supports base 2, base 10 and base 16. An unsupported base will silently
 * default to base 10. Prefixes aren't added (i.e., no "0x" or "0b" at the
 * beginning of the string).
 *
 * Returns the number of bytes actually written (excluding the NUL
 * terminator).
 */
size_t fio_ftoa(char *dest, double num, uint8_t base);

/* *****************************************************************************







                      Random Generator Functions

                  Probably not cryptographically safe







***************************************************************************** */

/* *****************************************************************************







                              Hash Functions and Friends







***************************************************************************** */

/* defines the secret seed to be used by keyd hashing functions*/
#ifndef FIO_HASH_SECRET_SEED64_1
uint8_t __attribute__((weak)) fio_hash_secret_marker1;
uint8_t __attribute__((weak)) fio_hash_secret_marker2;
#define FIO_HASH_SECRET_SEED64_1 ((uintptr_t)&fio_hash_secret_marker1)
#define FIO_HASH_SECRET_SEED64_2 ((uintptr_t)&fio_hash_secret_marker2)
#endif

#if FIO_USE_RISKY_HASH
#define FIO_HASH_FN(data, length, key1, key2) \
  fio_risky_hash((data), (length),            \
                 ((uint64_t)(key1) >> 19) | ((uint64_t)(key2) << 27))
#else
#define FIO_HASH_FN(data, length, key1, key2) \
  fio_siphash13((data), (length), (uint64_t)(key1), (uint64_t)(key2))
#endif

/* *****************************************************************************
Risky Hash (always available, even if using only the fio.h header)
***************************************************************************** */

/* Risky Hash primes */
#define RISKY_PRIME_0 0xFBBA3FA15B22113B
#define RISKY_PRIME_1 0xAB137439982B86C9

/* Risky Hash consumption round, accepts a state word s and an input word w */
#define fio_risky_consume(v, w) \
  (v) += (w);                   \
  (v) = fio_lrot64((v), 33);    \
  (v) += (w);                   \
  (v) *= RISKY_PRIME_0;

/*  Computes a facil.io Risky Hash. */
FIO_FUNC inline uint64_t fio_risky_hash(const void *data_, size_t len,
                                        uint64_t seed) {
  /* reading position */
  const uint8_t *data = (uint8_t *)data_;

  /* The consumption vectors initialized state */
  register uint64_t v0 = seed ^ RISKY_PRIME_1;
  register uint64_t v1 = ~seed + RISKY_PRIME_1;
  register uint64_t v2 =
      fio_lrot64(seed, 17) ^ ((~RISKY_PRIME_1) + RISKY_PRIME_0);
  register uint64_t v3 = fio_lrot64(seed, 33) + (~RISKY_PRIME_1);

  /* consume 256 bit blocks */
  for (size_t i = len >> 5; i; --i) {
    fio_risky_consume(v0, fio_str2u64(data));
    fio_risky_consume(v1, fio_str2u64(data + 8));
    fio_risky_consume(v2, fio_str2u64(data + 16));
    fio_risky_consume(v3, fio_str2u64(data + 24));
    data += 32;
  }

  /* Consume any remaining 64 bit words. */
  switch (len & 24) {
    case 24:
      fio_risky_consume(v2, fio_str2u64(data + 16));
      /* fallthrough */
    case 16:
      fio_risky_consume(v1, fio_str2u64(data + 8));
      /* fallthrough */
    case 8:
      fio_risky_consume(v0, fio_str2u64(data));
      data += len & 24;
  }

  uint64_t tmp = 0;
  /* consume leftover bytes, if any */
  switch ((len & 7)) {
    case 7:
      tmp |= ((uint64_t)data[6]) << 8;
      /* fallthrough */
    case 6:
      tmp |= ((uint64_t)data[5]) << 16;
      /* fallthrough */
    case 5:
      tmp |= ((uint64_t)data[4]) << 24;
      /* fallthrough */
    case 4:
      tmp |= ((uint64_t)data[3]) << 32;
      /* fallthrough */
    case 3:
      tmp |= ((uint64_t)data[2]) << 40;
      /* fallthrough */
    case 2:
      tmp |= ((uint64_t)data[1]) << 48;
      /* fallthrough */
    case 1:
      tmp |= ((uint64_t)data[0]) << 56;
      /* ((len >> 3) & 3) is a 0...3 value indicating consumption vector */
      switch ((len >> 3) & 3) {
        case 3:
          fio_risky_consume(v3, tmp);
          break;
        case 2:
          fio_risky_consume(v2, tmp);
          break;
        case 1:
          fio_risky_consume(v1, tmp);
          break;
        case 0:
          fio_risky_consume(v0, tmp);
          break;
      }
  }

  /* merge and mix */
  uint64_t result = fio_lrot64(v0, 17) + fio_lrot64(v1, 13) +
                    fio_lrot64(v2, 47) + fio_lrot64(v3, 57);

  len ^= (len << 33);
  result += len;

  result += v0 * RISKY_PRIME_1;
  result ^= fio_lrot64(result, 13);
  result += v1 * RISKY_PRIME_1;
  result ^= fio_lrot64(result, 29);
  result += v2 * RISKY_PRIME_1;
  result ^= fio_lrot64(result, 33);
  result += v3 * RISKY_PRIME_1;
  result ^= fio_lrot64(result, 51);

  /* irreversible avalanche... I think */
  result ^= (result >> 29) * RISKY_PRIME_0;
  return result;
}

#undef fio_risky_consume
#undef FIO_RISKY_PRIME_0
#undef FIO_RISKY_PRIME_1

/* *****************************************************************************
SipHash
***************************************************************************** */

/**
 * A SipHash 1-3 variation.
 */
uint64_t fio_siphash13(const void *data, size_t len, uint64_t key1,
                       uint64_t key2);

/**
 * The Hashing function used by dynamic facil.io objects.
 *
 * Currently implemented using SipHash 1-3.
 */
#define fio_siphash(data, length, k1, k2) \
  fio_siphash13((data), (length), (k1), (k2))

/* *****************************************************************************
C++ extern end
***************************************************************************** */
#ifdef __cplusplus
} /* extern "C" */
#endif

/* *****************************************************************************








                             Memory Allocator Details








***************************************************************************** */

/**
 * This is a custom memory allocator the utilizes memory pools to allow for
 * concurrent memory allocations across threads.
 *
 * Allocated memory is always zeroed out and aligned on a 16 byte boundary.
 *
 * Reallocated memory is always aligned on a 16 byte boundary but it might be
 * filled with junk data after the valid data (this is true also for
 * `fio_realloc2`).
 *
 * The memory allocator assumes multiple concurrent allocation/deallocation,
 * short life spans (memory is freed shortly, but not immediately, after it was
 * allocated) as well as small allocations (realloc almost always copies data).
 *
 * These assumptions allow the allocator to avoid lock contention by ignoring
 * fragmentation within a memory "block" and waiting for the whole "block" to be
 * freed before it's memory is recycled (no per-allocation "free list").
 *
 * An "arena" is allocated per-CPU core during initialization - there's no
 * dynamic allocation of arenas. This allows threads to minimize lock contention
 * by cycling through the arenas until a free arena is detected.
 *
 * There should be a free arena at any given time (statistically speaking) and
 * the thread will only be deferred in the unlikely event in which there's no
 * available arena.
 *
 * By avoiding the "free-list", the need for allocation "headers" is also
 * avoided and allocations are performed with practically zero overhead (about
 * 32 bytes overhead per 32KB memory, that's 1 bit per 1Kb).
 *
 * However, the lack of a "free list" means that memory "leaks" are more
 * expensive and small long-life allocations could cause fragmentation if
 * performed periodically (rather than performed during startup).
 *
 * This allocator should NOT be used for objects with a long life-span, because
 * even a single persistent object will prevent the re-use of the whole memory
 * block from which it was allocated (see FIO_MEMORY_BLOCK_SIZE for size).
 *
 * Some more details:
 *
 * Allocation and deallocations and (usually) managed by "blocks".
 *
 * A memory "block" can include any number of memory pages that are a multiple
 * of 2 (up to 1Mb of memory). However, the default value, set by the value of
 * FIO_MEMORY_BLOCK_SIZE_LOG, is 32Kb (see value at the end of this header).
 *
 * Each block includes a 32 byte header that uses reference counters and
 * position markers (24 bytes are required padding).
 *
 * The block's position marker (`pos`) marks the next available byte (counted in
 * multiples of 16 bytes).
 *
 * The block's reference counter (`ref`) counts how many allocations reference
 * memory in the block (including the "arena" that "owns" the block).
 *
 * Except for the position marker (`pos`) that acts the same as `sbrk`, there's
 * no way to know which "slices" are allocated and which "slices" are available.
 *
 * The allocator uses `mmap` when requesting memory from the system and for
 * allocations bigger than MEMORY_BLOCK_ALLOC_LIMIT (37.5% of the block).
 *
 * Small allocations are differentiated from big allocations by their memory
 * alignment.
 *
 * If a memory allocation is placed 16 bytes after whole block alignment (within
 * a block's padding zone), the memory was allocated directly using `mmap` as a
 * "big allocation". The 16 bytes include an 8 byte header and an 8 byte
 * padding.
 *
 * To replace the system's `malloc` function family compile with the
 * `FIO_OVERRIDE_MALLOC` defined (`-DFIO_OVERRIDE_MALLOC`).
 *
 * When using tcmalloc or jemalloc, it's possible to define `FIO_FORCE_MALLOC`
 * to prevent the facil.io allocator from compiling (`-DFIO_FORCE_MALLOC`).
 */

#ifndef FIO_MEMORY_BLOCK_SIZE_LOG
/**
 * The logarithmic value for a memory block, 15 == 32Kb, 16 == 64Kb, etc'
 *
 * By default, a block of memory is 32Kb silce from an 8Mb allocation.
 *
 * A value of 16 will make this a 64Kb silce from a 16Mb allocation.
 */
#define FIO_MEMORY_BLOCK_SIZE_LOG (15)
#endif

#undef FIO_MEMORY_BLOCK_SIZE
/** The resulting memoru block size, depends on `FIO_MEMORY_BLOCK_SIZE_LOG` */
#define FIO_MEMORY_BLOCK_SIZE ((uintptr_t)1 << FIO_MEMORY_BLOCK_SIZE_LOG)

/**
 * The maximum allocation size, after which `mmap` will be called instead of the
 * facil.io allocator.
 *
 * Defaults to 50% of the block (16Kb), after which `mmap` is used instead
 */
#ifndef FIO_MEMORY_BLOCK_ALLOC_LIMIT
#define FIO_MEMORY_BLOCK_ALLOC_LIMIT (FIO_MEMORY_BLOCK_SIZE >> 1)
#endif

/* *****************************************************************************









                           Spin locking Implementation









***************************************************************************** */

/**
 * Nanosleep seems to be the most effective and efficient thread rescheduler.
 */
FIO_FUNC inline void fio_reschedule_thread(void) {
  const struct timespec tm = {.tv_nsec = 1};
  nanosleep(&tm, NULL);
}

/** Nanosleep the thread - a blocking throttle. */
FIO_FUNC inline void fio_throttle_thread(size_t nano_sec) {
  const struct timespec tm = {.tv_nsec = (long)(nano_sec % 1000000000),
                              .tv_sec = (time_t)(nano_sec / 1000000000)};
  nanosleep(&tm, NULL);
}

/** returns 0 if the lock was acquired and another value on failure. */
FIO_FUNC inline int fio_trylock(fio_lock_i *lock) {
  __asm__ volatile("" ::: "memory");
  fio_lock_i ret = fio_atomic_xchange(lock, 1);
  __asm__ volatile("" ::: "memory");
  return ret;
}

/**
 * Releases a spinlock. Releasing an unacquired lock will break it.
 *
 * Returns a non-zero value on success, or 0 if the lock was in an unloacked
 * state.
 */
FIO_FUNC inline int fio_unlock(fio_lock_i *lock) {
  __asm__ volatile("" ::: "memory");
  fio_lock_i ret = fio_atomic_xchange(lock, 0);
  return ret;
}

/** Returns a spinlock's state (non 0 == Busy). */
FIO_FUNC inline int fio_is_locked(fio_lock_i *lock) {
  __asm__ volatile("" ::: "memory");
  return *lock;
}

/** Busy waits for the spinlock (CAREFUL). */
FIO_FUNC inline void fio_lock(fio_lock_i *lock) {
  while (fio_trylock(lock)) {
    fio_reschedule_thread();
  }
}

#if DEBUG_SPINLOCK
/** Busy waits for a lock, reports contention. */
FIO_FUNC inline void fio_lock_dbg(fio_lock_i *lock, const char *file,
                                  int line) {
  size_t lock_cycle_count = 0;
  while (fio_trylock(lock)) {
    if (lock_cycle_count >= 8 &&
        (lock_cycle_count == 8 || !(lock_cycle_count & 511)))
      fprintf(stderr, "[DEBUG] fio-spinlock spin %s:%d round %zu\n", file, line,
              lock_cycle_count);
    ++lock_cycle_count;
    fio_reschedule_thread();
  }
  if (lock_cycle_count >= 8)
    fprintf(stderr, "[DEBUG] fio-spinlock spin %s:%d total = %zu\n", file, line,
            lock_cycle_count);
}
#define fio_lock(lock) fio_lock_dbg((lock), __FILE__, __LINE__)

FIO_FUNC inline int fio_trylock_dbg(fio_lock_i *lock, const char *file,
                                    int line) {
  static int last_line = 0;
  static size_t count = 0;
  int result = fio_trylock(lock);
  if (!result) {
    count = 0;
    last_line = 0;
  } else if (line == last_line) {
    ++count;
    if (count >= 2)
      fprintf(stderr, "[DEBUG] trying fio-spinlock %s:%d attempt %zu\n", file,
              line, count);
  } else {
    count = 0;
    last_line = line;
  }
  return result;
}
#define fio_trylock(lock) fio_trylock_dbg((lock), __FILE__, __LINE__)
#endif /* DEBUG_SPINLOCK */

#endif /* H_FACIL_IO_H */

/* *****************************************************************************






                    Memory allocation macros for helper types






***************************************************************************** */

#undef FIO_MALLOC
#undef FIO_CALLOC
#undef FIO_REALLOC
#undef FIO_FREE

#if FIO_FORCE_MALLOC || FIO_FORCE_MALLOC_TMP
#define FIO_MALLOC(size) calloc((size), 1)
#define FIO_CALLOC(size, units) calloc((size), (units))
#define FIO_REALLOC(ptr, new_length, existing_data_length) \
  realloc((ptr), (new_length))
#define FIO_FREE free

#else
#define FIO_MALLOC(size) fio_malloc((size))
#define FIO_CALLOC(size, units) fio_calloc((size), (units))
#define FIO_REALLOC(ptr, new_length, existing_data_length) \
  fio_realloc2((ptr), (new_length), (existing_data_length))
#define FIO_FREE fio_free
#endif /* FIO_FORCE_MALLOC || FIO_FORCE_MALLOC_TMP */

/* *****************************************************************************






                           Linked List Helpers

        exposes internally used inline helpers for linked lists






***************************************************************************** */

#if !defined(H_FIO_LINKED_LIST_H) && defined(FIO_INCLUDE_LINKED_LIST)

#define H_FIO_LINKED_LIST_H
#undef FIO_INCLUDE_LINKED_LIST
/* *****************************************************************************
Data Structure and Initialization.
***************************************************************************** */

/** an embeded linked list. */
typedef struct fio_ls_embd_s {
  struct fio_ls_embd_s *prev;
  struct fio_ls_embd_s *next;
} fio_ls_embd_s;

/** an independent linked list. */
typedef struct fio_ls_s {
  struct fio_ls_s *prev;
  struct fio_ls_s *next;
  const void *obj;
} fio_ls_s;

#define FIO_LS_INIT(name) {.next = &(name), .prev = &(name)}

/* *****************************************************************************
Embedded Linked List API
***************************************************************************** */

/** Adds a node to the list's head. */
FIO_FUNC inline void fio_ls_embd_push(fio_ls_embd_s *dest, fio_ls_embd_s *node);

/** Adds a node to the list's tail. */
FIO_FUNC inline void fio_ls_embd_unshift(fio_ls_embd_s *dest,
                                         fio_ls_embd_s *node);

/** Removes a node from the list's head. */
FIO_FUNC inline fio_ls_embd_s *fio_ls_embd_pop(fio_ls_embd_s *list);

/** Removes a node from the list's tail. */
FIO_FUNC inline fio_ls_embd_s *fio_ls_embd_shift(fio_ls_embd_s *list);

/** Removes a node from the containing node. */
FIO_FUNC inline fio_ls_embd_s *fio_ls_embd_remove(fio_ls_embd_s *node);

/** Tests if the list is empty. */
FIO_FUNC inline int fio_ls_embd_is_empty(fio_ls_embd_s *list);

/** Tests if the list is NOT empty (contains any nodes). */
FIO_FUNC inline int fio_ls_embd_any(fio_ls_embd_s *list);

/**
 * Iterates through the list using a `for` loop.
 *
 * Access the data with `pos->obj` (`pos` can be named however you please).
 */
#define FIO_LS_EMBD_FOR(list, node)

/**
 * Takes a list pointer `plist` and returns a pointer to it's container.
 *
 * This uses pointer offset calculations and can be used to calculate any
 * struct's pointer (not just list containers) as an offset from a pointer of
 * one of it's members.
 *
 * Very useful.
 */
#define FIO_LS_EMBD_OBJ(type, member, plist) \
  ((type *)((uintptr_t)(plist) - (uintptr_t)(&(((type *)0)->member))))

/* *****************************************************************************
Independent Linked List API
***************************************************************************** */

/** Adds an object to the list's head, returnin's the object's location. */
FIO_FUNC inline fio_ls_s *fio_ls_push(fio_ls_s *pos, const void *obj);

/** Adds an object to the list's tail, returnin's the object's location. */
FIO_FUNC inline fio_ls_s *fio_ls_unshift(fio_ls_s *pos, const void *obj);

/** Removes an object from the list's head. */
FIO_FUNC inline void *fio_ls_pop(fio_ls_s *list);

/** Removes an object from the list's tail. */
FIO_FUNC inline void *fio_ls_shift(fio_ls_s *list);

/** Removes a node from the list, returning the contained object. */
FIO_FUNC inline void *fio_ls_remove(fio_ls_s *node);

/** Tests if the list is empty. */
FIO_FUNC inline int fio_ls_is_empty(fio_ls_s *list);

/** Tests if the list is NOT empty (contains any nodes). */
FIO_FUNC inline int fio_ls_any(fio_ls_s *list);

/**
 * Iterates through the list using a `for` loop.
 *
 * Access the data with `pos->obj` (`pos` can be named however you please).
 */
#define FIO_LS_FOR(list, pos)

/* *****************************************************************************


                             Linked List Helpers

                               IMPLEMENTATION


***************************************************************************** */

/* *****************************************************************************
Embeded Linked List Implementation
***************************************************************************** */

/** Removes a node from the containing node. */
FIO_FUNC inline fio_ls_embd_s *fio_ls_embd_remove(fio_ls_embd_s *node) {
  if (!node->next || node->next == node) {
    /* never remove the list's head */
    return NULL;
  }
  node->next->prev = node->prev;
  node->prev->next = node->next;
  node->prev = node->next = node;
  return node;
}

/** Adds a node to the list's head. */
FIO_FUNC inline void fio_ls_embd_push(fio_ls_embd_s *dest,
                                      fio_ls_embd_s *node) {
  if (!dest || !node) return;
  node->prev = dest->prev;
  node->next = dest;
  dest->prev->next = node;
  dest->prev = node;
}

/** Adds a node to the list's tail. */
FIO_FUNC inline void fio_ls_embd_unshift(fio_ls_embd_s *dest,
                                         fio_ls_embd_s *node) {
  fio_ls_embd_push(dest->next, node);
}

/** Removes a node from the list's head. */
FIO_FUNC inline fio_ls_embd_s *fio_ls_embd_pop(fio_ls_embd_s *list) {
  return fio_ls_embd_remove(list->prev);
}

/** Removes a node from the list's tail. */
FIO_FUNC inline fio_ls_embd_s *fio_ls_embd_shift(fio_ls_embd_s *list) {
  return fio_ls_embd_remove(list->next);
}

/** Tests if the list is empty. */
FIO_FUNC inline int fio_ls_embd_is_empty(fio_ls_embd_s *list) {
  return list->next == list;
}

/** Tests if the list is NOT empty (contains any nodes). */
FIO_FUNC inline int fio_ls_embd_any(fio_ls_embd_s *list) {
  return list->next != list;
}

#undef FIO_LS_EMBD_FOR
#define FIO_LS_EMBD_FOR(list, node) \
  for (fio_ls_embd_s *node = (list)->next; node != (list); node = node->next)

/* *****************************************************************************
Independent Linked List Implementation
***************************************************************************** */

/** Removes an object from the containing node. */
FIO_FUNC inline void *fio_ls_remove(fio_ls_s *node) {
  if (!node || node->next == node) {
    /* never remove the list's head */
    return NULL;
  }
  const void *ret = node->obj;
  node->next->prev = node->prev;
  node->prev->next = node->next;
  FIO_FREE(node);
  return (void *)ret;
}

/** Adds an object to the list's head. */
FIO_FUNC inline fio_ls_s *fio_ls_push(fio_ls_s *pos, const void *obj) {
  if (!pos) return NULL;
  /* prepare item */
  fio_ls_s *item = (fio_ls_s *)FIO_MALLOC(sizeof(*item));
  FIO_ASSERT_ALLOC(item);
  *item = (fio_ls_s){.prev = pos->prev, .next = pos, .obj = obj};
  /* inject item */
  pos->prev->next = item;
  pos->prev = item;
  return item;
}

/** Adds an object to the list's tail. */
FIO_FUNC inline fio_ls_s *fio_ls_unshift(fio_ls_s *pos, const void *obj) {
  return fio_ls_push(pos->next, obj);
}

/** Removes an object from the list's head. */
FIO_FUNC inline void *fio_ls_pop(fio_ls_s *list) {
  return fio_ls_remove(list->prev);
}

/** Removes an object from the list's tail. */
FIO_FUNC inline void *fio_ls_shift(fio_ls_s *list) {
  return fio_ls_remove(list->next);
}

/** Tests if the list is empty. */
FIO_FUNC inline int fio_ls_is_empty(fio_ls_s *list) {
  return list->next == list;
}

/** Tests if the list is NOT empty (contains any nodes). */
FIO_FUNC inline int fio_ls_any(fio_ls_s *list) { return list->next != list; }

#undef FIO_LS_FOR
#define FIO_LS_FOR(list, pos) \
  for (fio_ls_s *pos = (list)->next; pos != (list); pos = pos->next)

#endif /* FIO_INCLUDE_LINKED_LIST */

/* *****************************************************************************







                             String Helpers

          exposes internally used inline helpers for binary Strings







***************************************************************************** */

#if !defined(H_FIO_STR_H) && defined(FIO_INCLUDE_STR)

#define H_FIO_STR_H
#undef FIO_INCLUDE_STR

/* *****************************************************************************
String API - Initialization and Destruction
***************************************************************************** */

/**
 * The `fio_str_s` type should be considered opaque.
 *
 * The type's attributes should be accessed ONLY through the accessor functions:
 * `fio_str_info`, `fio_str_len`, `fio_str_data`, `fio_str_capa`, etc'.
 *
 * Note: when the `small` flag is present, the structure is ignored and used as
 * raw memory for a small String (no additional allocation). This changes the
 * String's behavior drastically and requires that the accessor functions be
 * used.
 */
typedef struct {
#ifndef FIO_STR_NO_REF
  volatile uint32_t ref; /* reference counter for fio_str_dup */
#endif
  uint8_t small;  /* Flag indicating the String is small and self-contained */
  uint8_t frozen; /* Flag indicating the String is frozen (don't edit) */
#ifdef FIO_STR_NO_REF
  uint8_t reserved[14]; /* Align struct on 16 byte allocator boundary */
#else
  uint8_t reserved[10]; /* Align struct on 16 byte allocator boundary */
#endif
  uint64_t capa;           /* Known capacity for longer Strings */
  uint64_t len;            /* String length for longer Strings */
  void (*dealloc)(void *); /* Data deallocation function (NULL for static) */
  char *data;              /* Data for longer Strings */
#if UINTPTR_MAX != UINT64_MAX
  uint8_t padding[2 * (sizeof(uint64_t) -
                       sizeof(void *))]; /* 16 byte  boundary for 32bit OS */
#endif
} fio_str_s;

/**
 * This value should be used for initialization. For example:
 *
 *      // on the stack
 *      fio_str_s str = FIO_STR_INIT;
 *
 *      // or on the heap
 *      fio_str_s *str = malloc(sizeof(*str);
 *      *str = FIO_STR_INIT;
 *
 * Remember to cleanup:
 *
 *      // on the stack
 *      fio_str_free(&str);
 *
 *      // or on the heap
 *      fio_str_free(str);
 *      free(str);
 */
#define FIO_STR_INIT ((fio_str_s){.data = NULL, .small = 1})

/**
 * This macro allows the container to be initialized with existing data, as long
 * as it's memory was allocated using `fio_malloc`.
 *
 * The `capacity` value should exclude the NUL character (if exists).
 */
#define FIO_STR_INIT_EXISTING(buffer, length, capacity) \
  ((fio_str_s){.data = (buffer),                        \
               .len = (length),                         \
               .capa = (capacity),                      \
               .dealloc = FIO_FREE})

/**
 * This macro allows the container to be initialized with existing static data,
 * that shouldn't be freed.
 */
#define FIO_STR_INIT_STATIC(buffer) \
  ((fio_str_s){                     \
      .data = (char *)(buffer), .len = strlen((buffer)), .dealloc = NULL})

/**
 * This macro allows the container to be initialized with existing static data,
 * that shouldn't be freed.
 */
#define FIO_STR_INIT_STATIC2(buffer, length) \
  ((fio_str_s){.data = (char *)(buffer), .len = (length), .dealloc = NULL})

/**
 * Allocates a new fio_str_s object on the heap and initializes it.
 *
 * Use `fio_str_free` then `fio_free` to free both the String data and the
 * container.
 *
 * NOTE: This makes the allocation and reference counting logic more intuitive.
 */
inline FIO_FUNC fio_str_s *fio_str_new2(void);

/**
 * Allocates a new fio_str_s object on the heap, initializes it and copies the
 * original (`src`) string into the new string.
 *
 * Use `fio_str_free` then `fio_free` to free the new string's data and it's
 * container.
 */
inline FIO_FUNC fio_str_s *fio_str_new_copy2(fio_str_s *src);

/**
 * Adds a references to the current String object and returns itself.
 *
 * If refecrence counting was disabled (FIO_STR_NO_REF was defined), returns a
 * copy of the String (free with `fio_str_free` then `fio_free`).
 *
 * NOTE: Nothing is copied, reference Strings are referencing the same String.
 *       Editing one reference will effect the other.
 *
 *       The original's String's container should remain in scope (if on the
 *       stack) or remain allocated (if on the heap) until all the references
 *       were freed using `fio_str_free` or discarded.
 */
inline FIO_FUNC fio_str_s *fio_str_dup(fio_str_s *s);

/**
 * Frees the String's resources and reinitializes the container.
 *
 * Note: if the container isn't allocated on the stack, it should be freed
 * separately using `free(s)`.
 *
 * Returns 0 if the data was freed and -1 if the String is NULL or has un-freed
 * references (see fio_str_dup).
 */
inline FIO_FUNC int fio_str_free(fio_str_s *s);

/* *****************************************************************************
String API - String state (data pointers, length, capacity, etc')
***************************************************************************** */

/*
 * String state information, defined above as:
typedef struct {
  size_t capa;
  size_t len;
  char *data;
} fio_str_info_s;
*/

/** Returns the String's complete state (capacity, length and pointer).  */
inline FIO_FUNC fio_str_info_s fio_str_info(const fio_str_s *s);

/** Returns the String's length in bytes. */
inline FIO_FUNC size_t fio_str_len(fio_str_s *s);

/** Returns a pointer (`char *`) to the String's content. */
inline FIO_FUNC char *fio_str_data(fio_str_s *s);

/** Returns a byte pointer (`uint8_t *`) to the String's unsigned content. */
#define fio_str_bytes(s) ((uint8_t *)fio_str_data((s)))

/** Returns the String's existing capacity (total used & available memory). */
inline FIO_FUNC size_t fio_str_capa(fio_str_s *s);

/**
 * Sets the new String size without reallocating any memory (limited by
 * existing capacity).
 *
 * Returns the updated state of the String.
 *
 * Note: When shrinking, any existing data beyond the new size may be corrupted.
 */
inline FIO_FUNC fio_str_info_s fio_str_resize(fio_str_s *s, size_t size);

/**
 * Clears the string (retaining the existing capacity).
 */
#define fio_str_clear(s) fio_str_resize((s), 0)

/**
 * Returns the string's Risky Hash value.
 *
 * Note: Hash algorithm might change without notice.
 */
FIO_FUNC uint64_t fio_str_hash(const fio_str_s *s);

/* *****************************************************************************
String API - Memory management
***************************************************************************** */

/**
 * Requires the String to have at least `needed` capacity. Returns the current
 * state of the String.
 */
FIO_FUNC fio_str_info_s fio_str_capa_assert(fio_str_s *s, size_t needed);

/* *****************************************************************************
String API - Content Manipulation and Review
***************************************************************************** */

/**
 * Writes data at the end of the String (similar to `fio_str_insert` with the
 * argument `pos == -1`).
 */
inline FIO_FUNC fio_str_info_s fio_str_write(fio_str_s *s, const void *src,
                                             size_t src_len);

/**
 * Writes a number at the end of the String using normal base 10 notation.
 */
inline FIO_FUNC fio_str_info_s fio_str_write_i(fio_str_s *s, int64_t num);

/**
 * Appens the `src` String to the end of the `dest` String.
 *
 * If `dest` is empty, the resulting Strings will be equal.
 */
inline FIO_FUNC fio_str_info_s fio_str_concat(fio_str_s *dest,
                                              fio_str_s const *src);

/** Alias for fio_str_concat */
#define fio_str_join(dest, src) fio_str_concat((dest), (src))

/**
 * Opens the file `filename` and pastes it's contents (or a slice ot it) at the
 * end of the String. If `limit == 0`, than the data will be read until EOF.
 *
 * If the file can't be located, opened or read, or if `start_at` is beyond
 * the EOF position, NULL is returned in the state's `data` field.
 *
 * Works on POSIX only.
 */
FIO_FUNC fio_str_info_s fio_str_readfile(fio_str_s *s, const char *filename,
                                         intptr_t start_at, intptr_t limit);

/**
 * Prevents further manipulations to the String's content.
 */
inline FIO_FUNC void fio_str_freeze(fio_str_s *s);

/**
 * Binary comparison returns `1` if both strings are equal and `0` if not.
 */
inline FIO_FUNC int fio_str_iseq(const fio_str_s *str1, const fio_str_s *str2);

/* *****************************************************************************


                             String Implementation

                               IMPLEMENTATION


***************************************************************************** */

/* *****************************************************************************
String Implementation - state (data pointers, length, capacity, etc')
***************************************************************************** */

typedef struct {
#ifndef FIO_STR_NO_REF
  volatile uint32_t ref; /* reference counter for fio_str_dup */
#endif
  uint8_t small;  /* Flag indicating the String is small and self-contained */
  uint8_t frozen; /* Flag indicating the String is frozen (don't edit) */
} fio_str__small_s;

#define FIO_STR_SMALL_DATA(s) ((char *)((&(s)->frozen) + 1))

/* the capacity when the string is stored in the container itself */
#define FIO_STR_SMALL_CAPA \
  (sizeof(fio_str_s) - (size_t)((&((fio_str_s *)0)->frozen) + 1))

/** Returns the String's state (capacity, length and pointer). */
inline FIO_FUNC fio_str_info_s fio_str_info(const fio_str_s *s) {
  if (!s) return (fio_str_info_s){.len = 0};
  return (s->small || !s->data)
             ? (fio_str_info_s){.capa =
                                    (s->frozen ? 0 : (FIO_STR_SMALL_CAPA - 1)),
                                .len = (size_t)(s->small >> 1),
                                .data = FIO_STR_SMALL_DATA(s)}
             : (fio_str_info_s){.capa = (s->frozen ? 0 : s->capa),
                                .len = s->len,
                                .data = s->data};
}

/**
 * Allocates a new fio_str_s object on the heap and initializes it.
 *
 * Use `fio_str_free` then `fio_free` to free both the String data and the
 * container.
 *
 * NOTE: This makes the allocation and reference counting logic more intuitive.
 */
inline FIO_FUNC fio_str_s *fio_str_new2(void) {
  fio_str_s *str = FIO_MALLOC(sizeof(*str));
  FIO_ASSERT_ALLOC(str);
  *str = FIO_STR_INIT;
  return str;
}

/**
 * Allocates a new fio_str_s object on the heap, initializes it and copies the
 * original (`src`) string into the new string.
 *
 * Use `fio_str_free` then `fio_free` to free the new string's data and it's
 * container.
 */
inline FIO_FUNC fio_str_s *fio_str_new_copy2(fio_str_s *src) {
  fio_str_s *cpy = fio_str_new2();
  fio_str_concat(cpy, src);
  return cpy;
}

/**
 * Adds a references to the current String object and returns itself.
 *
 * If refecrence counting was disabled (FIO_STR_NO_REF was defined), returns a
 * copy of the String (free with `fio_str_free` then `fio_free`).
 *
 * NOTE: Nothing is copied, reference Strings are referencing the same String.
 *       Editing one reference will effect the other.
 *
 *       The original's String's container should remain in scope (if on the
 *       stack) or remain allocated (if on the heap) until all the references
 *       were freed using `fio_str_free` or discarded.
 */
inline FIO_FUNC fio_str_s *fio_str_dup(fio_str_s *s) {
#ifdef FIO_STR_NO_REF
  fio_str_s *s2 = fio_str_new2();
  fio_str_concat(s2, s);
  return s2;
#else
  if (s) fio_atomic_add(&s->ref, 1);
  return s;
#endif
}

/**
 * Frees the String's resources and reinitializes the container.
 *
 * Note: if the container isn't allocated on the stack, it should be freed
 * separately using `free(s)`.
 *
 * Returns 0 if the data was freed and -1 if the String is NULL or has un-freed
 * references (see fio_str_dup).
 */
inline FIO_FUNC int fio_str_free(fio_str_s *s) {
#ifndef FIO_STR_NO_REF
  if (!s || fio_atomic_sub(&s->ref, 1) != (uint32_t)-1) return -1;
#endif
  if (!s->small && s->dealloc) s->dealloc(s->data);
  *s = FIO_STR_INIT;
  return 0;
}

/** Returns the String's length in bytes. */
inline FIO_FUNC size_t fio_str_len(fio_str_s *s) {
  return (s->small || !s->data) ? (s->small >> 1) : s->len;
}

/** Returns a pointer (`char *`) to the String's content. */
inline FIO_FUNC char *fio_str_data(fio_str_s *s) {
  return (s->small || !s->data) ? FIO_STR_SMALL_DATA(s) : s->data;
}

/** Returns the String's existing capacity (allocated memory). */
inline FIO_FUNC size_t fio_str_capa(fio_str_s *s) {
  if (s->frozen) return 0;
  return (s->small || !s->data) ? (FIO_STR_SMALL_CAPA - 1) : s->capa;
}

/**
 * Sets the new String size without reallocating any memory (limited by
 * existing capacity).
 *
 * Returns the updated state of the String.
 *
 * Note: When shrinking, any existing data beyond the new size may be corrupted.
 *
 * Note: When providing a new size that is grater then the current string
 * capacity, any data that was written beyond the current (previous) size might
 * be replaced with NUL bytes.
 */
inline FIO_FUNC fio_str_info_s fio_str_resize(fio_str_s *s, size_t size) {
  if (!s || s->frozen) {
    return fio_str_info(s);
  }
  if (s->small || !s->data) {
    if (size < FIO_STR_SMALL_CAPA) {
      s->small = (uint8_t)(((size << 1) | 1) & 0xFF);
      FIO_STR_SMALL_DATA(s)[size] = 0;
      return (fio_str_info_s){.capa = (FIO_STR_SMALL_CAPA - 1),
                              .len = size,
                              .data = FIO_STR_SMALL_DATA(s)};
    }
    s->small = (uint8_t)((((FIO_STR_SMALL_CAPA - 1) << 1) | 1) & 0xFF);
    fio_str_capa_assert(s, size);
    goto big;
  }
  if (size >= s->capa) {
    s->len = fio_ct_if2((uintptr_t)s->dealloc, s->capa, s->len);
    fio_str_capa_assert(s, size);
  }

big:
  s->len = size;
  s->data[size] = 0;
  return (fio_str_info_s){.capa = s->capa, .len = size, .data = s->data};
}

/* *****************************************************************************
String Implementation - Hashing
***************************************************************************** */

/**
 * Return's the String's Risky Hash (see fio_risky_hash).
 *
 * This value is machine/instance specific (hash seed is a memory address).
 *
 * NOTE: the hashing function might be changed at any time without notice. It
 * wasn't cryptographically analyzed and safety against malicious data can't be
 * guaranteed. Use fio_siphash13 or fio_siphash24 when hashing data from
 * external sources.
 */
FIO_FUNC uint64_t fio_str_hash(const fio_str_s *s) {
  fio_str_info_s state = fio_str_info(s);
  return fio_risky_hash(state.data, state.len, FIO_HASH_SECRET_SEED64_1);
}

/* *****************************************************************************
String Implementation - Memory management
***************************************************************************** */

/**
 * Rounds up allocated capacity to the closest 2 words byte boundary (leaving 1
 * byte space for the NUL byte).
 *
 * This shouldn't effect actual allocation size and should only minimize the
 * effects of the memory allocator's alignment rounding scheme.
 *
 * To clarify:
 *
 * Memory allocators are required to allocate memory on the minimal alignment
 * required by the largest type (`long double`), which usually results in memory
 * allocations using this alignment as a minimal spacing.
 *
 * For example, on 64 bit architectures, it's likely that `malloc(18)` will
 * allocate the same amount of memory as `malloc(32)` due to alignment concerns.
 *
 * In fact, with some allocators (i.e., jemalloc), spacing increases for larger
 * allocations - meaning the allocator will round up to more than 16 bytes, as
 * noted here: http://jemalloc.net/jemalloc.3.html#size_classes
 *
 * Note that this increased spacing, doesn't occure with facil.io's allocator,
 * since it uses 16 byte alignment right up until allocations are routed
 * directly to `mmap` (due to their size, usually over 12KB).
 */
#define ROUND_UP_CAPA2WORDS(num) (((num) + 1) | (sizeof(long double) - 1))

/**
 * Requires the String to have at least `needed` capacity. Returns the current
 * state of the String.
 */
FIO_FUNC fio_str_info_s fio_str_capa_assert(fio_str_s *s, size_t needed) {
  if (!s || s->frozen) {
    return fio_str_info(s);
  }
  char *tmp;
  if (s->small || !s->data) {
    if (needed < FIO_STR_SMALL_CAPA) {
      return (fio_str_info_s){.capa = (FIO_STR_SMALL_CAPA - 1),
                              .len = (size_t)(s->small >> 1),
                              .data = FIO_STR_SMALL_DATA(s)};
    }
    goto is_small;
  }
  if (needed < s->capa) {
    return (fio_str_info_s){.capa = s->capa, .len = s->len, .data = s->data};
  }
  needed = ROUND_UP_CAPA2WORDS(needed);
  if (s->dealloc == FIO_FREE) {
    tmp = (char *)FIO_REALLOC(s->data, needed + 1, s->len + 1);
    FIO_ASSERT_ALLOC(tmp);
  } else {
    tmp = (char *)FIO_MALLOC(needed + 1);
    FIO_ASSERT_ALLOC(tmp);
    memcpy(tmp, s->data, s->len + 1);
    if (s->dealloc) s->dealloc(s->data);
    s->dealloc = FIO_FREE;
  }
  s->capa = needed;
  s->data = tmp;
  s->data[needed] = 0;
  return (fio_str_info_s){.capa = s->capa, .len = s->len, .data = s->data};

is_small:
  /* small string (string data is within the container) */
  needed = ROUND_UP_CAPA2WORDS(needed);
  tmp = (char *)FIO_MALLOC(needed + 1);
  FIO_ASSERT_ALLOC(tmp);
  const size_t existing_len = (size_t)((s->small >> 1) & 0xFF);
  if (existing_len) {
    memcpy(tmp, FIO_STR_SMALL_DATA(s), existing_len + 1);
  } else {
    tmp[0] = 0;
  }
#ifdef FIO_STR_NO_REF
  *s = (fio_str_s){
      .small = 0,
      .capa = needed,
      .len = existing_len,
      .dealloc = FIO_FREE,
      .data = tmp,
  };
#else
  *s = (fio_str_s){
      .ref = s->ref,
      .small = 0,
      .capa = needed,
      .len = existing_len,
      .dealloc = FIO_FREE,
      .data = tmp,
  };
#endif
  return (fio_str_info_s){.capa = needed, .len = existing_len, .data = s->data};
}

/* *****************************************************************************
String Implementation - Content Manipulation and Review
***************************************************************************** */

/**
 * Writes data at the end of the String (similar to `fio_str_insert` with the
 * argument `pos == -1`).
 */
inline FIO_FUNC fio_str_info_s fio_str_write(fio_str_s *s, const void *src,
                                             size_t src_len) {
  if (!s || !src_len || !src || s->frozen) return fio_str_info(s);
  fio_str_info_s state = fio_str_resize(s, src_len + fio_str_len(s));
  memcpy(state.data + (state.len - src_len), src, src_len);
  return state;
}

/**
 * Writes a number at the end of the String using normal base 10 notation.
 */
inline FIO_FUNC fio_str_info_s fio_str_write_i(fio_str_s *s, int64_t num) {
  if (!s || s->frozen) return fio_str_info(s);
  fio_str_info_s i;
  if (!num) goto zero;
  char buf[22];
  uint64_t l = 0;
  uint8_t neg;
  /* Compute the magnitude in unsigned arithmetic rather than `num = 0 -
   * num` on the signed int64_t: the latter is UB (and, in practice, a
   * no-op that leaves num negative) when num == INT64_MIN, which has no
   * positive int64_t representation. Unsigned negation is well-defined
   * (wraps modulo 2^64) and yields the correct magnitude for every
   * representable num, INT64_MIN included. */
  uint64_t mag;
  if ((neg = (num < 0))) {
    mag = (uint64_t)0 - (uint64_t)num;
    neg = 1;
  } else {
    mag = (uint64_t)num;
  }
  while (mag) {
    uint64_t t = mag / 10;
    buf[l++] = '0' + (mag - (t * 10));
    mag = t;
  }
  if (neg) {
    buf[l++] = '-';
  }
  i = fio_str_resize(s, fio_str_len(s) + l);

  while (l) {
    --l;
    i.data[i.len - (l + 1)] = buf[l];
  }
  return i;
zero:
  i = fio_str_resize(s, fio_str_len(s) + 1);
  i.data[i.len - 1] = '0';
  return i;
}

/**
 * Appens the `src` String to the end of the `dest` String.
 */
inline FIO_FUNC fio_str_info_s fio_str_concat(fio_str_s *dest,
                                              fio_str_s const *src) {
  if (!dest || !src || dest->frozen) return fio_str_info(dest);
  fio_str_info_s src_state = fio_str_info(src);
  if (!src_state.len) return fio_str_info(dest);
  fio_str_info_s state =
      fio_str_resize(dest, src_state.len + fio_str_len(dest));
  memcpy(state.data + state.len - src_state.len, src_state.data, src_state.len);
  return state;
}

/**
 * Opens the file `filename` and pastes it's contents (or a slice ot it) at the
 * end of the String. If `limit == 0`, than the data will be read until EOF.
 *
 * If the file can't be located, opened or read, or if `start_at` is beyond
 * the EOF position, NULL is returned in the state's `data` field.
 */
FIO_FUNC fio_str_info_s fio_str_readfile(fio_str_s *s, const char *filename,
                                         intptr_t start_at, intptr_t limit) {
  fio_str_info_s state = {.data = NULL};
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__) || \
    defined(__CYGWIN__)
  /* POSIX implementations. */
  if (filename == NULL || !s) return state;
  struct stat f_data;
  int file = -1;
  char *path = NULL;
  size_t path_len = 0;

  if (filename[0] == '~' && (filename[1] == '/' || filename[1] == '\\')) {
    char *home = getenv("HOME");
    if (home) {
      size_t filename_len = strlen(filename);
      size_t home_len = strlen(home);
      if ((home_len + filename_len) >= (1 << 16)) {
        /* too long */
        return state;
      }
      if (home[home_len - 1] == '/' || home[home_len - 1] == '\\') --home_len;
      path_len = home_len + filename_len - 1;
      path = FIO_MALLOC(path_len + 1);
      FIO_ASSERT_ALLOC(path);
      memcpy(path, home, home_len);
      memcpy(path + home_len, filename + 1, filename_len);
      path[path_len] = 0;
      filename = path;
    }
  }

  if (stat(filename, &f_data)) {
    goto finish;
  }

  if (f_data.st_size <= 0 || start_at >= f_data.st_size) {
    state = fio_str_info(s);
    goto finish;
  }

  file = open(filename, O_RDONLY);
  if (-1 == file) goto finish;

  if (start_at < 0) {
    start_at = f_data.st_size + start_at;
    if (start_at < 0) start_at = 0;
  }

  if (limit <= 0 || f_data.st_size < (limit + start_at))
    limit = f_data.st_size - start_at;

  const size_t org_len = fio_str_len(s);
  state = fio_str_resize(s, org_len + limit);
  if (pread(file, state.data + org_len, limit, start_at) != (ssize_t)limit) {
    fio_str_resize(s, org_len);
    state.data = NULL;
    state.len = state.capa = 0;
  }
  close(file);
finish:
  FIO_FREE(path);
  return state;
#else
  /* TODO: consider adding non POSIX implementations. */
  FIO_LOG_ERROR("File reading requires a posix system (ignored!).\n");
  return state;
#endif
}

/**
 * Prevents further manipulations to the String's content.
 */
inline FIO_FUNC void fio_str_freeze(fio_str_s *s) {
  if (!s) return;
  s->frozen = 1;
}

/**
 * Binary comparison returns `1` if both strings are equal and `0` if not.
 */
inline FIO_FUNC int fio_str_iseq(const fio_str_s *str1, const fio_str_s *str2) {
  if (str1 == str2) return 1;
  if (!str1 || !str2) return 0;
  fio_str_info_s s1 = fio_str_info(str1);
  fio_str_info_s s2 = fio_str_info(str2);
  return (s1.len == s2.len && !memcmp(s1.data, s2.data, s1.len));
}

#undef ROUND_UP_CAPA2WORDS
#undef FIO_STR_SMALL_DATA
#undef FIO_STR_NO_REF

#endif /* H_FIO_STR_H */

/* *****************************************************************************











                               Dynamic Array Data-Store











***************************************************************************** */

#ifdef FIO_ARY_NAME
/**
 * A simple typed dynamic array with a minimal API.
 *
 * To create an Array type, define the macro FIO_ARY_NAME. i.e.:
 *
 *         #define FIO_ARY_NAME fio_cstr_ary
 *         #define FIO_ARY_TYPE char *
 *         #define FIO_ARY_COMPARE(k1, k2) (!strcmp((k1), (k2)))
 *         #include <fio.h>
 *
 * It's possible to create a number of Array types by reincluding the fio.h
 * header. i.e.:
 *
 *
 *         #define FIO_INCLUDE_STR
 *         #include <fio.h> // adds the fio_str_s types and functions
 *
 *         #define FIO_ARY_NAME fio_int_ary
 *         #define FIO_ARY_TYPE int
 *         #include <fio.h> // creates the fio_int_ary_s Array and functions
 *
 *         #define FIO_ARY_NAME fio_str_ary
 *         #define FIO_ARY_TYPE fio_str_s *
 *         #define FIO_ARY_COMPARE(k1, k2) (fio_str_iseq((k1), (k2)))
 *         #define FIO_ARY_COPY(key) fio_str_dup((key))
 *         #define FIO_ARY_DESTROY(key) fio_str_free((key))
 *         #include <fio.h> // creates the fio_str_ary_s Array and functions
 *
 * Note: Before freeing the Array, FIO_ARY_DESTROY will be automatically called
 *       for every existing object, including any invalid objects (if any).
 */

/* Used for naming functions and types, prefixing FIO_ARY_NAME to the name */
#define FIO_NAME_FROM_MACRO_STEP2(name, postfix) name##_##postfix
#define FIO_NAME_FROM_MACRO_STEP1(name, postfix) \
  FIO_NAME_FROM_MACRO_STEP2(name, postfix)
#define FIO_NAME(postfix) FIO_NAME_FROM_MACRO_STEP1(FIO_ARY_NAME, postfix)

/* Used for naming the `free` function */
#define FIO_NAME_FROM_MACRO_STEP4(name) name##_free
#define FIO_NAME_FROM_MACRO_STEP3(name) FIO_NAME_FROM_MACRO_STEP4(name)
#define FIO_NAME_FREE() FIO_NAME_FROM_MACRO_STEP3(FIO_ARY_NAME)

/* The default Array object type is `void *` */
#if !defined(FIO_ARY_TYPE)
#define FIO_ARY_TYPE void *
#endif

/* An invalid object has all bytes set to 0 - a static constant will do. */
#if !defined(FIO_ARY_INVALID)
static FIO_ARY_TYPE const FIO_NAME(s___const_invalid_object);
#define FIO_ARY_INVALID FIO_NAME(s___const_invalid_object)
#endif

/* The default Array comparison assumes a simple type */
#if !defined(FIO_ARY_COMPARE)
#define FIO_ARY_COMPARE(o1, o2) ((o1) == (o2))
#endif

/** object copy required? */
#ifndef FIO_ARY_COPY
#define FIO_ARY_COPY_IS_SIMPLE 1
#define FIO_ARY_COPY(dest, obj) ((dest) = (obj))
#endif

/** object destruction required? */
#ifndef FIO_ARY_DESTROY
#define FIO_ARY_DESTROY(obj) ((void)0)
#endif

/* Customizable memory management */
#ifndef FIO_ARY_MALLOC /* NULL ptr indicates new allocation */
#define FIO_ARY_MALLOC(size) FIO_MALLOC((size))
#endif

/* Customizable memory management */
#ifndef FIO_ARY_REALLOC /* NULL ptr indicates new allocation */
#define FIO_ARY_REALLOC(ptr, original_size, new_size, valid_data_length) \
  FIO_REALLOC((ptr), (new_size), (valid_data_length))
#endif

#ifndef FIO_ARY_DEALLOC
#define FIO_ARY_DEALLOC(ptr, size) FIO_FREE((ptr))
#endif

/* padding to be assumed for future expansion. */
#ifndef FIO_ARY_PADDING
#define FIO_ARY_PADDING 4
#endif

/* minimizes allocation "dead space" by alligning allocated length to 16bytes */
#undef FIO_ARY_SIZE2WORDS
#define FIO_ARY_SIZE2WORDS(size)                          \
  ((sizeof(FIO_ARY_TYPE) & 1)   ? (((size) & (~15)) + 16) \
   : (sizeof(FIO_ARY_TYPE) & 2) ? (((size) & (~7)) + 8)   \
   : (sizeof(FIO_ARY_TYPE) & 4) ? (((size) & (~3)) + 4)   \
   : (sizeof(FIO_ARY_TYPE) & 8) ? (((size) & (~1)) + 2)   \
                                : (size))

/* *****************************************************************************
Array API
***************************************************************************** */

/** The Array container type. */
typedef struct FIO_NAME(s) FIO_NAME(s);

#ifndef FIO_ARY_INIT
/** Initializes the Array */
#define FIO_ARY_INIT {.capa = 0}
#endif

/** Frees the array's internal data. */
FIO_FUNC inline void FIO_NAME_FREE()(FIO_NAME(s) * ary);

/** Returns the number of elements in the Array. */
FIO_FUNC inline size_t FIO_NAME(count)(FIO_NAME(s) * ary);

/** Returns the current, temporary, array capacity (it's dynamic). */
FIO_FUNC inline size_t FIO_NAME(capa)(FIO_NAME(s) * ary);

/**
 * Adds all the items in the `src` Array to the end of the `dest` Array.
 *
 * The `src` Array remain untouched.
 */
FIO_FUNC inline void FIO_NAME(concat)(FIO_NAME(s) * dest, FIO_NAME(s) * src);

/**
 * Sets `index` to the value in `data`.
 *
 * If `index` is negative, it will be counted from the end of the Array (-1 ==
 * last element).
 *
 * If `old` isn't NULL, the existing data will be copied to the location pointed
 * to by `old` before the copy in the Array is destroyed.
 */
FIO_FUNC inline void FIO_NAME(set)(FIO_NAME(s) * ary, intptr_t index,
                                   FIO_ARY_TYPE data, FIO_ARY_TYPE *old);

/**
 * Returns the value located at `index` (no copying is peformed).
 *
 * If `index` is negative, it will be counted from the end of the Array (-1 ==
 * last element).
 */
FIO_FUNC inline FIO_ARY_TYPE FIO_NAME(get)(FIO_NAME(s) * ary, intptr_t index);

/**
 * Returns the index of the object or -1 if the object wasn't found.
 */
FIO_FUNC inline intptr_t FIO_NAME(find)(FIO_NAME(s) * ary, FIO_ARY_TYPE data);

/**
 * Removes an object from the array, MOVING all the other objects to prevent
 * "holes" in the data.
 *
 * If `old` is set, the data is copied to the location pointed to by `old`
 * before the data in the array is destroyed.
 *
 * Returns 0 on success and -1 on error.
 */
FIO_FUNC inline int FIO_NAME(remove)(FIO_NAME(s) * ary, intptr_t index,
                                     FIO_ARY_TYPE *old);

/**
 * Removes an object from the array, if it exists, MOVING all the other objects
 * to prevent "holes" in the data.
 *
 * Returns -1 if the object wasn't found or 0 if the object was successfully
 * removed.
 */
FIO_FUNC inline int FIO_NAME(remove2)(FIO_NAME(s) * ary, FIO_ARY_TYPE data,
                                      FIO_ARY_TYPE *old);

/**
 * Returns a pointer to the C array containing the objects.
 */
FIO_FUNC inline FIO_ARY_TYPE *FIO_NAME(to_a)(FIO_NAME(s) * ary);

/**
 * Pushes an object to the end of the Array. Returns -1 on error.
 */
FIO_FUNC inline int FIO_NAME(push)(FIO_NAME(s) * ary, FIO_ARY_TYPE data);

/**
 * Removes an object from the end of the Array.
 *
 * If `old` is set, the data is copied to the location pointed to by `old`
 * before the data in the array is destroyed.
 *
 * Returns -1 on error (Array is empty) and 0 on success.
 */
FIO_FUNC inline int FIO_NAME(pop)(FIO_NAME(s) * ary, FIO_ARY_TYPE *old);

/**
 * Unshifts an object to the beginning of the Array. Returns -1 on error.
 *
 * This could be expensive, causing `memmove`.
 */
FIO_FUNC inline int FIO_NAME(unshift)(FIO_NAME(s) * ary, FIO_ARY_TYPE data);

/**
 * Removes an object from the beginning of the Array.
 *
 * If `old` is set, the data is copied to the location pointed to by `old`
 * before the data in the array is destroyed.
 *
 * Returns -1 on error (Array is empty) and 0 on success.
 */
FIO_FUNC inline int FIO_NAME(shift)(FIO_NAME(s) * ary, FIO_ARY_TYPE *old);

/**
 * Iteration using a callback for each entry in the array.
 *
 * The callback task function must accept an the entry data as well as an opaque
 * user pointer.
 *
 * If the callback returns -1, the loop is broken. Any other value is ignored.
 *
 * Returns the relative "stop" position, i.e., the number of items processed +
 * the starting point.
 */
FIO_FUNC inline size_t FIO_NAME(each)(FIO_NAME(s) * ary, size_t start_at,
                                      int (*task)(FIO_ARY_TYPE pt, void *arg),
                                      void *arg);
/**
 * Removes any FIO_ARY_TYPE_INVALID object from an Array (NULL pointers by
 * default), keeping all other data in the array.
 *
 * This action is O(n) where n in the length of the array.
 * It could get expensive.
 */
FIO_FUNC inline void FIO_NAME(compact)(FIO_NAME(s) * ary);

/**
 * Iterates through the list using a `for` loop.
 *
 * Access the object with the pointer `pos`. The `pos` variable can be named
 * however you please.
 *
 * Avoid editing the array during a FOR loop, although I hope it's possible, I
 * wouldn't count on it.
 */
#ifndef FIO_ARY_FOR
#define FIO_ARY_FOR(ary, pos)                                        \
  if ((ary)->arry)                                                   \
    for (__typeof__((ary)->arry) start__tmp__ = (ary)->arry,         \
                                 pos = ((ary)->arry + (ary)->start); \
         pos < (ary)->arry + (ary)->end;                             \
         (pos = (ary)->arry + (pos - start__tmp__) + 1),             \
                                 (start__tmp__ = (ary)->arry))
#endif

/* *****************************************************************************
Array Type
***************************************************************************** */

struct FIO_NAME(s) {
  size_t start;       /* first index where data was already written */
  size_t end;         /* next spot to write at tail */
  size_t capa;        /* existing capacity */
  FIO_ARY_TYPE *arry; /* the actual array's memory, if any */
};

/* *****************************************************************************
Array Memory Management
***************************************************************************** */

FIO_FUNC inline void FIO_NAME_FREE()(FIO_NAME(s) * ary) {
  if (ary) {
    const size_t count = ary->end;
    for (size_t i = ary->start; i < count; ++i) {
      FIO_ARY_DESTROY((ary->arry[i]));
    }
    FIO_ARY_DEALLOC(ary->arry, ary->capa * sizeof(*ary->arry));
    *ary = (FIO_NAME(s))FIO_ARY_INIT;
  }
}

/** Converts between a relative index to an absolute index. */
FIO_FUNC inline intptr_t FIO_NAME(__rel2absolute)(FIO_NAME(s) * ary,
                                                  intptr_t index) {
  if (index >= 0) return index;
  index += ary->end - ary->start;
  if (index >= 0) return index;
  return 0;
}

/** Makes sure that `len` positions are available at the Array's end. */
FIO_FUNC void FIO_NAME(__require_on_top)(FIO_NAME(s) * ary, size_t len) {
  /* `<=`, not `<`: when end+len == capa exactly, the array already fits
   * with zero slack and a reallocation would be pure unnecessary work. */
  if (ary->end + len <= ary->capa) return;
  len = FIO_ARY_SIZE2WORDS((len + ary->end));
  /* reallocate enough memory */
  ary->arry = FIO_ARY_REALLOC(ary->arry, sizeof(*ary->arry) * ary->capa,
                              (len) * sizeof(*ary->arry),
                              ary->end * sizeof(*ary->arry));
  FIO_ASSERT_ALLOC(ary->arry);
  ary->capa = len;
}

/** Makes sure that `len` positions are available at the Array's head. */
FIO_FUNC void FIO_NAME(__require_on_bottom)(FIO_NAME(s) * ary, size_t len) {
  if (ary->start >= len) return;
  FIO_ARY_TYPE *tmp = ary->arry;
  len = FIO_ARY_SIZE2WORDS((len - ary->start) + ary->end);
  if (ary->capa <= len) {
    /* no room - allocate and copy */
    ary->arry = FIO_ARY_MALLOC(len * sizeof(*ary->arry));
    FIO_ASSERT_ALLOC(ary->arry);
    ary->capa = len;
  }
  /* move existing data to the end of the existing space */
  len = ary->end - ary->start;
  ary->end = ary->capa;
  if (len)
    memmove(ary->arry + (ary->capa - len), tmp + ary->start,
            len * sizeof(*ary->arry));
  ary->start = ary->end - len;
  if (tmp != ary->arry) {
    FIO_FREE(tmp);
  }
}

/* *****************************************************************************
Array API implementation
***************************************************************************** */

/** Returns the number of elements in the Array. */
FIO_FUNC inline size_t FIO_NAME(count)(FIO_NAME(s) * ary) {
  return ary ? (ary->end - ary->start) : 0;
}

/** Returns the current, temporary, array capacity (it's dynamic). */
FIO_FUNC inline size_t FIO_NAME(capa)(FIO_NAME(s) * ary) {
  return ary ? ary->capa : 0;
}

/**
 * Returns a pointer to the C array containing the objects.
 */
FIO_FUNC inline FIO_ARY_TYPE *FIO_NAME(to_a)(FIO_NAME(s) * ary) {
  return ary ? (ary->arry + ary->start) : NULL;
}

/**
 * Adds all the items in the `src` Array to the end of the `dest` Array.
 *
 * The `src` Array remain untouched.
 */
FIO_FUNC inline void FIO_NAME(concat)(FIO_NAME(s) * dest, FIO_NAME(s) * src) {
  if (!src) return;
  const size_t added = FIO_NAME(count)(src);
  if (!added || !dest) return;
  FIO_NAME(__require_on_top)(dest, added);
#if FIO_ARY_COPY_IS_SIMPLE
  memcpy(dest->arry + dest->end, src->arry + src->start,
         added * sizeof(*dest->arry));
#else
  /* don't use memcpy, in case copying has side-effects (see macro) */
  for (size_t i = 0; i < added; ++i) {
    FIO_ARY_COPY(((dest->arry + dest->end)[i]), ((src->arry + src->start)[i]));
  }
#endif
  dest->end += added;
}

/**
 * Sets `index` to the value in `data`.
 *
 * If `index` is negative, it will be counted from the end of the Array (-1 ==
 * last element).
 *
 * If `old` isn't NULL, the existing data will be copied to the location pointed
 * to by `old` before the copy in the Array is destroyed.
 */
FIO_FUNC inline void FIO_NAME(set)(FIO_NAME(s) * ary, intptr_t index,
                                   FIO_ARY_TYPE data, FIO_ARY_TYPE *old) {
  if (!ary) return;
  if (ary->start == ary->end) /* reset memory starting point? */
    ary->start = ary->end = 0;

  index = FIO_NAME(__rel2absolute)(ary, index);

  const intptr_t spaces = index - (ary->end - ary->start);
  if (spaces < 0) {
    /* likely */
    if (old) FIO_ARY_COPY((*old), ((ary->arry + ary->start)[index]));
    FIO_ARY_DESTROY(((ary->arry + ary->start)[index]));
    FIO_ARY_COPY(((ary->arry + ary->start)[index]), data);
    return;
  }

  /* fill empty spaces with zero */
  FIO_NAME(__require_on_top)(ary, spaces + 1);
  if (spaces) {
    memset(ary->arry + ary->end, 0, sizeof(*ary->arry) * spaces);
  }
  FIO_ARY_COPY(((ary->arry + ary->start)[index]), data);
  ary->end = index + 1;
}

/**
 * Returns the value located at `index` (no copying is peformed).
 *
 * If `index` is negative, it will be counted from the end of the Array (-1 ==
 * last element).
 */
FIO_FUNC inline FIO_ARY_TYPE FIO_NAME(get)(FIO_NAME(s) * ary, intptr_t index) {
  if (!ary) return FIO_ARY_INVALID;
  index = FIO_NAME(__rel2absolute)(ary, index);
  if ((size_t)index >= ary->end - ary->start) return FIO_ARY_INVALID;
  return (ary->arry + ary->start)[index];
}

/**
 * Returns the index of the object or -1 if the object wasn't found.
 */
FIO_FUNC inline intptr_t FIO_NAME(find)(FIO_NAME(s) * ary, FIO_ARY_TYPE data) {
  const size_t count = FIO_NAME(count)(ary);
  if (!count) {
    return -1;
  }
  size_t pos = ary->start;
  register const size_t end = ary->end;
  while (pos < end && !FIO_ARY_COMPARE(data, ary->arry[pos])) {
    ++pos;
  }
  if (pos == end) return -1;
  return (pos - ary->start);
}

/**
 * Removes an object from the array, MOVING all the other objects to prevent
 * "holes" in the data.
 *
 * If `old` is set, the data is copied to the location pointed to by `old`
 * before the data in the array is destroyed.
 *
 * Returns 0 on success and -1 on error.
 */
FIO_FUNC inline int FIO_NAME(remove)(FIO_NAME(s) * ary, intptr_t index,
                                     FIO_ARY_TYPE *old) {
  index = FIO_NAME(__rel2absolute)(ary, index);
  const size_t count = FIO_NAME(count)(ary);
  if (!count || (size_t)index >= count) {
    return -1;
  }
  index += ary->start;
  if (old) FIO_ARY_COPY((*old), (ary->arry[index]));
  FIO_ARY_DESTROY((ary->arry[index]));
  if ((size_t)index == ary->start) {
    ++ary->start;
    return 0;
  }
  --ary->end;
  if ((size_t)index < ary->end) {
    memmove(ary->arry + index, ary->arry + index + 1,
            (ary->end - index) * sizeof(*ary->arry));
  }
  return 0;
}

/**
 * Removes an object from the array, if it exists, MOVING all the other objects
 * to prevent "holes" in the data.
 *
 * Returns -1 if the object wasn't found or 0 if the object was successfully
 * removed.
 */
FIO_FUNC inline int FIO_NAME(remove2)(FIO_NAME(s) * ary, FIO_ARY_TYPE data,
                                      FIO_ARY_TYPE *old) {
  intptr_t index = FIO_NAME(find)(ary, data);
  if (index == -1) {
    return -1;
  }
  return FIO_NAME(remove)(ary, index, old);
}

/**
 * Pushes an object to the end of the Array. Returns -1 on error.
 */
FIO_FUNC inline int FIO_NAME(push)(FIO_NAME(s) * ary, FIO_ARY_TYPE data) {
  if (!ary) return -1;
  if (ary->capa <= ary->end)
    FIO_NAME(__require_on_top)(ary, 1 + FIO_ARY_PADDING);
  if (ary->start == ary->end) /* reset memory starting point? */
    ary->start = ary->end = 0;
  FIO_ARY_COPY(ary->arry[ary->end], data);
  ++ary->end;
  return 0;
}

/**
 * Removes an object from the end of the Array.
 *
 * If `old` is set, the data is copied to the location pointed to by `old`
 * before the data in the array is destroyed.
 *
 * Returns -1 on error (Array is empty) and 0 on success.
 */
FIO_FUNC inline int FIO_NAME(pop)(FIO_NAME(s) * ary, FIO_ARY_TYPE *old) {
  if (!FIO_NAME(count)(ary)) return -1;
  --ary->end;
  if (old) FIO_ARY_COPY((*old), (ary->arry[ary->end]));
  FIO_ARY_DESTROY((ary->arry[ary->end]));
  return 0;
}

/**
 * Unshifts an object to the beginning of the Array. Returns -1 on error.
 *
 * This could be expensive, causing `memmove`.
 */
FIO_FUNC inline int FIO_NAME(unshift)(FIO_NAME(s) * ary, FIO_ARY_TYPE data) {
  if (!ary) return -1;
  if (ary->start == 0) FIO_NAME(__require_on_bottom)(ary, 8);
  --ary->start;
  FIO_ARY_COPY(ary->arry[ary->start], data);
  return 0;
}

/**
 * Removes an object from the beginning of the Array.
 *
 * If `old` is set, the data is copied to the location pointed to by `old`
 * before the data in the array is destroyed.
 *
 * Returns -1 on error (Array is empty) and 0 on success.
 */
FIO_FUNC inline int FIO_NAME(shift)(FIO_NAME(s) * ary, FIO_ARY_TYPE *old) {
  if (!FIO_NAME(count)(ary)) return -1;
  if (old) FIO_ARY_COPY((*old), (ary->arry[ary->start]));
  FIO_ARY_DESTROY((ary->arry[ary->start]));
  ++ary->start;
  return 0;
}

/**
 * Iteration using a callback for each entry in the array.
 *
 * The callback task function must accept an the entry data as well as an opaque
 * user pointer.
 *
 * If the callback returns -1, the loop is broken. Any other value is ignored.
 *
 * Returns the relative "stop" position, i.e., the number of items processed +
 * the starting point.
 */
FIO_FUNC inline size_t FIO_NAME(each)(FIO_NAME(s) * ary, size_t start_at,
                                      int (*task)(FIO_ARY_TYPE pt, void *arg),
                                      void *arg) {
  const size_t count = FIO_NAME(count)(ary);
  if (!count || start_at >= count) {
    return count;
  }
  while (start_at < count &&
         task(ary->arry[ary->start + (start_at++)], arg) != -1);
  return start_at;
}
/**
 * Removes any FIO_ARY_TYPE_INVALID object from an Array (NULL pointers by
 * default), keeping all other data in the array.
 *
 * This action is O(n) where n in the length of the array.
 * It could get expensive.
 */
FIO_FUNC inline void FIO_NAME(compact)(FIO_NAME(s) * ary) {
  const size_t count = FIO_NAME(count)(ary);
  if (!count) return;
  register FIO_ARY_TYPE *pos = ary->arry + ary->start;
  register FIO_ARY_TYPE *reader = ary->arry + ary->start;
  register FIO_ARY_TYPE *stop = ary->arry + ary->end;
  while (reader < stop) {
    if (!FIO_ARY_COMPARE((*reader), FIO_ARY_INVALID)) {
      *pos = *reader;
      pos += 1;
    }
    reader += 1;
  }
  ary->end = (size_t)(pos - ary->arry);
}

/* *****************************************************************************
Array Testing
***************************************************************************** */

#if DEBUG
#include <stdio.h>
#define TEST_LIMIT 1016
/**
 * Removes any FIO_ARY_TYPE_INVALID  *pointers* from an Array, keeping all other
 * data in the array.
 *
 * This action is O(n) where n in the length of the array.
 * It could get expensive.
 */
FIO_FUNC inline void FIO_NAME(_test)(void) {
  union {
    FIO_ARY_TYPE obj;
    uintptr_t i;
  } mem;
  FIO_NAME(s) ary = FIO_ARY_INIT;
  fprintf(stderr, "=== Testing Core Array features for type " FIO_MACRO2STR(
                      FIO_ARY_TYPE) "\n");

  for (uintptr_t i = 0; i < TEST_LIMIT; ++i) {
    mem.i = i + 1;
    FIO_NAME(push)(&ary, mem.obj);
  }
  fprintf(stderr,
          "* Array populated using `push` with %zu items,\n"
          "  with capacity limit of %zu and start index %zu\n",
          (size_t)FIO_NAME(count)(&ary), (size_t)FIO_NAME(capa)(&ary),
          ary.start);
  FIO_ASSERT(FIO_NAME(count)(&ary) == TEST_LIMIT,
             "Wrong object count for array %zu", (size_t)FIO_NAME(count)(&ary));
  for (uintptr_t i = 0; i < TEST_LIMIT; ++i) {
    FIO_ASSERT(!FIO_NAME(shift)(&ary, &mem.obj), "Array shift failed at %lu.",
               i);
    FIO_ASSERT(mem.i == i + 1, "Array shift value error %lu != %lu", mem.i,
               i + 1);
    FIO_ARY_DESTROY(mem.obj);
  }

  FIO_NAME_FREE()(&ary);
  FIO_ASSERT(!ary.arry, "Array not reset after fio_ary_free");

  for (uintptr_t i = 0; i < TEST_LIMIT; ++i) {
    mem.i = TEST_LIMIT - i;
    FIO_NAME(unshift)(&ary, mem.obj);
  }
  fprintf(stderr,
          "* Array populated using `unshift` with %zu items,\n"
          "  with capacity limit of %zu and start index %zu\n",
          (size_t)FIO_NAME(count)(&ary), (size_t)FIO_NAME(capa)(&ary),
          ary.start);

  FIO_ASSERT(FIO_NAME(count)(&ary) == TEST_LIMIT,
             "Wrong object count for array %zu", (size_t)FIO_NAME(count)(&ary));
  for (uintptr_t i = 0; i < TEST_LIMIT; ++i) {
    FIO_NAME(pop)(&ary, &mem.obj);
    FIO_ASSERT(mem.i == TEST_LIMIT - i, "Array pop value error");
    FIO_ARY_DESTROY(mem.obj);
  }
  FIO_NAME_FREE()(&ary);
  FIO_ASSERT(!ary.arry, "Array not reset after fio_ary_free");

  for (uintptr_t i = 0; i < TEST_LIMIT; ++i) {
    mem.i = TEST_LIMIT - i;
    FIO_NAME(unshift)(&ary, mem.obj);
  }

  for (size_t i = 0; i < TEST_LIMIT; ++i) {
    mem.i = i + 1;
    FIO_ASSERT(FIO_NAME(find)(&ary, mem.obj) == (intptr_t)i,
               "Wrong object index - ary[%zd] != %zu",
               (ssize_t)FIO_NAME(find)(&ary, mem.obj), (size_t)mem.i);
    mem.obj = FIO_NAME(get)(&ary, i);
    FIO_ASSERT(mem.i == (uintptr_t)(i + 1),
               "Wrong object returned from fio_ary_index - ary[%zu] != %zu", i,
               i + 1);
  }

  FIO_ASSERT((FIO_NAME(count)(&ary) == TEST_LIMIT),
             "Wrong object count before pop %zu",
             (size_t)FIO_NAME(count)(&ary));
  FIO_ASSERT(!FIO_NAME(pop)(&ary, &mem.obj), "Couldn't pop element.");
  FIO_ASSERT(mem.i == TEST_LIMIT, "Element value error (%zu).", (size_t)mem.i);
  FIO_ASSERT((FIO_NAME(count)(&ary) == TEST_LIMIT - 1),
             "Wrong object count after pop %zu", (size_t)FIO_NAME(count)(&ary));
  FIO_ARY_DESTROY(mem.obj);

  mem.i = (TEST_LIMIT >> 1);
  FIO_ASSERT(!FIO_NAME(remove2)(&ary, mem.obj, NULL),
             "Couldn't fio_ary_remove2 object from Array (%zu)", (size_t)mem.i);
  FIO_ASSERT(FIO_NAME(count)(&ary) == TEST_LIMIT - 2,
             "Wrong object count after remove2 %zu",
             (size_t)FIO_NAME(count)(&ary));
  mem.i = (TEST_LIMIT >> 1) + 1;
  FIO_ASSERT(FIO_NAME(find)(&ary, mem.obj) != (TEST_LIMIT >> 1) + 1,
             "fio_ary_remove2 didn't clear holes from Array (%zu)",
             (size_t)FIO_NAME(find)(&ary, mem.obj));
  FIO_ARY_DESTROY(mem.obj);

  FIO_ASSERT(!FIO_NAME(remove)(&ary, 0, &mem.obj),
             "fio_ary_remove failed (at %zd)", (ssize_t)mem.i);
  FIO_ASSERT(mem.i == 1, "Couldn't fio_ary_remove object from Array (%zd)",
             (ssize_t)mem.i);
  FIO_ASSERT(FIO_NAME(count)(&ary) == TEST_LIMIT - 3,
             "Wrong object count after remove %zu != %d",
             (size_t)FIO_NAME(count)(&ary), TEST_LIMIT - 3);
  FIO_ASSERT(FIO_NAME(find)(&ary, mem.obj) == -1,
             "fio_ary_find should have failed after fio_ary_remove (%zd)",
             (ssize_t)FIO_NAME(find)(&ary, mem.obj));
  FIO_ARY_DESTROY(mem.obj);

  mem.i = 2;
  FIO_ASSERT(FIO_NAME(find)(&ary, mem.obj) == 0,
             "fio_ary_remove didn't clear holes from Array (%zu)",
             (size_t)FIO_NAME(find)(&ary, mem.obj));

  FIO_NAME_FREE()(&ary);

  FIO_NAME(s) ary2 = FIO_ARY_INIT;
  for (uintptr_t i = 0; i < (TEST_LIMIT >> 1); ++i) {
    mem.i = ((TEST_LIMIT >> 1) << 1) - i;
    FIO_NAME(unshift)(&ary2, mem.obj);
    mem.i = (TEST_LIMIT >> 1) - i;
    FIO_NAME(unshift)(&ary, mem.obj);
  }
  FIO_NAME(concat)(&ary, &ary2);
  FIO_NAME_FREE()(&ary2);
  FIO_ASSERT(FIO_NAME(count)(&ary) == ((TEST_LIMIT >> 1) << 1),
             "Wrong object count after fio_ary_concat %zu",
             (size_t)FIO_NAME(count)(&ary));
  for (int i = 0; i < ((TEST_LIMIT >> 1) << 1); ++i) {
    mem.obj = FIO_NAME(get)(&ary, i);
    FIO_ASSERT(
        mem.i == (uintptr_t)(i + 1),
        "Wrong object returned from fio_ary_index after concat - ary[%d] != %d",
        i, i + 1);
  }
  mem.i = 0;
  while (FIO_NAME(pop)(&ary, &mem.obj)) {
    ++mem.i;
    FIO_ARY_DESTROY(mem.obj);
  }
  FIO_ASSERT(mem.i == ((TEST_LIMIT >> 1) << 1), "fio_ary_pop overflow (%zu)?",
             (size_t)mem.i);
  FIO_NAME_FREE()(&ary);
}
#undef TEST_LIMIT
#else
FIO_FUNC inline void FIO_NAME(_test)(void) {}
#endif

/* *****************************************************************************
Done
***************************************************************************** */

#undef FIO_NAME_FROM_MACRO_STEP2
#undef FIO_NAME_FROM_MACRO_STEP1
#undef FIO_NAME
#undef FIO_NAME_FROM_MACRO_STEP4
#undef FIO_NAME_FROM_MACRO_STEP3
#undef FIO_NAME_FREE
#undef FIO_ARY_NAME
#undef FIO_ARY_TYPE
#undef FIO_ARY_INVALID
#undef FIO_ARY_COMPARE
#undef FIO_ARY_COPY
#undef FIO_ARY_COPY_IS_SIMPLE
#undef FIO_ARY_DESTROY
#undef FIO_ARY_REALLOC
#undef FIO_ARY_DEALLOC
#undef FIO_ARY_SIZE2WORDS

#endif

/* *****************************************************************************











                               Set / Hash Map Data-Store











***************************************************************************** */

#ifdef FIO_SET_NAME

/**
 * A simple ordered Set / Hash Map implementation, with a minimal API.
 *
 * A Set is basically a Hash Map where the keys are also the values, it's often
 * used for caching objects.
 *
 * The Set's object type and behavior is controlled by the FIO_SET_OBJ_* marcos.
 *
 * A Hash Map is basically a set where the objects in the Set are key-value
 * couplets and only the keys are tested when searching the Set.
 *
 * To create a Set or a Hash Map, the macro FIO_SET_NAME must be defined. i.e.:
 *
 *         #define FIO_SET_NAME cstr_set
 *         #define FIO_SET_OBJ_TYPE char *
 *         #define FIO_SET_OBJ_COMPARE(k1, k2) (!strcmp((k1), (k2)))
 *         #include <fio.h>
 *
 * To create a Hash Map, rather than a pure Set, the macro FIO_SET_KEY_TYPE must
 * be defined. i.e.:
 *
 *         #define FIO_SET_KEY_TYPE char *
 *
 * This allows the FIO_SET_KEY_* macros to be defined as well. For example:
 *
 *         #define FIO_SET_NAME cstr_hashmap
 *         #define FIO_SET_KEY_TYPE char *
 *         #define FIO_SET_KEY_COMPARE(k1, k2) (!strcmp((k1), (k2)))
 *         #define FIO_SET_OBJ_TYPE char *
 *         #include <fio.h>
 *
 * It's possible to create a number of Set or HasMap types by reincluding the
 * fio.h header. i.e.:
 *
 *
 *         #define FIO_INCLUDE_STR
 *         #include <fio.h> // adds the fio_str_s types and functions
 *
 *         #define FIO_SET_NAME fio_str_set
 *         #define FIO_SET_OBJ_TYPE fio_str_s *
 *         #define FIO_SET_OBJ_COMPARE(k1, k2) (fio_str_iseq((k1), (k2)))
 *         #define FIO_SET_OBJ_COPY(key) fio_str_dup((key))
 *         #define FIO_SET_OBJ_DESTROY(key) fio_str_free((key))
 *         #include <fio.h> // creates the fio_str_set_s Set and functions
 *
 *         #define FIO_SET_NAME fio_str_hash
 *         #define FIO_SET_KEY_TYPE fio_str_s *
 *         #define FIO_SET_KEY_COMPARE(k1, k2) (fio_str_iseq((k1), (k2)))
 *         #define FIO_SET_KEY_COPY(key) fio_str_dup((key))
 *         #define FIO_SET_KEY_DESTROY(key) fio_str_free((key))
 *         #define FIO_SET_OBJ_TYPE fio_str_s *
 *         #define FIO_SET_OBJ_COMPARE(k1, k2) (fio_str_iseq((k1), (k2)))
 *         #define FIO_SET_OBJ_COPY(key) fio_str_dup((key))
 *         #define FIO_SET_OBJ_DESTROY(key) fio_str_free((key))
 *         #include <fio.h> // creates the fio_str_hash_s Hash Map and functions
 *
 * The default integer Hash used is a pointer length type (uintptr_t). This can
 * be changed by defining ALL of the following macros:
 * * FIO_SET_HASH_TYPE              - the type of the hash value.
 * * FIO_SET_HASH2UINTPTR(hash, i)  - converts the hash value to a uintptr_t.
 * * FIO_SET_HASH_COMPARE(h1, h2)   - compares two hash values (1 == equal).
 * * FIO_SET_HASH_INVALID           - an invalid Hash value, all bytes are 0.
 * * FIO_SET_HASH_FORCE             - an always valid Hash value, all bytes 0xFF
 *
 *
 * Note: FIO_SET_HASH_TYPE should, normaly be left alone (uintptr_t is
 *       enough). Also, the hash value 0 is reserved to indicate an empty slot.
 *
 * Note: the FIO_SET_OBJ_COMPARE or the FIO_SET_KEY_COMPARE will be used to
 *       compare against invalid as well as valid objects. Invalid objects have
 *       their bytes all zero. FIO_SET_*_DESTROY should somehow mark them as
 *       invalid.
 *
 * Note: Before freeing the Set, FIO_SET_OBJ_DESTROY will be automatically
 *       called for every existing object.
 */

/* Used for naming functions and types, prefixing FIO_SET_NAME to the name */
#define FIO_NAME_FROM_MACRO_STEP2(name, postfix) name##_##postfix
#define FIO_NAME_FROM_MACRO_STEP1(name, postfix) \
  FIO_NAME_FROM_MACRO_STEP2(name, postfix)
#define FIO_NAME(postfix) FIO_NAME_FROM_MACRO_STEP1(FIO_SET_NAME, postfix)

/* Used for naming the `free` function */
#define FIO_NAME_FROM_MACRO_STEP4(name) name##_free
#define FIO_NAME_FROM_MACRO_STEP3(name) FIO_NAME_FROM_MACRO_STEP4(name)
#define FIO_NAME_FREE() FIO_NAME_FROM_MACRO_STEP3(FIO_SET_NAME)

/* The default Set object / value type is `void *` */
#if !defined(FIO_SET_OBJ_TYPE)
#define FIO_SET_OBJ_TYPE void *
#elif !defined(FIO_SET_NO_TEST)
#define FIO_SET_NO_TEST 1
#endif

/* The default Set has opaque objects that can't be compared */
#if !defined(FIO_SET_OBJ_COMPARE)
#define FIO_SET_OBJ_COMPARE(o1, o2) (1)
#endif

/** object copy required? */
#ifndef FIO_SET_OBJ_COPY
#define FIO_SET_OBJ_COPY(dest, obj) ((dest) = (obj))
#endif

/** object destruction required? */
#ifndef FIO_SET_OBJ_DESTROY
#define FIO_SET_OBJ_DESTROY(obj) ((void)0)
#endif

/** test for a pre-defined hash type, must be numerical (i.e. __int128_t)*/
#ifndef FIO_SET_HASH_TYPE
#define FIO_SET_HASH_TYPE uintptr_t
#endif

/** test for a pre-defined hash to integer conversion */
#ifndef FIO_SET_HASH2UINTPTR
#define FIO_SET_HASH2UINTPTR(hash, bits_used) \
  (fio_rrot(hash, bits_used) ^ fio_ct_if2(bits_used, hash, 0))
#endif

/** test for a pre-defined hash to integer conversion */
#ifndef FIO_SET_HASH_FORCE
#define FIO_SET_HASH_FORCE (~(uintptr_t)0)
#endif

/** test for a pre-defined invalid hash value (all bytes are 0) */
#ifndef FIO_SET_HASH_INVALID
#define FIO_SET_HASH_INVALID ((FIO_SET_HASH_TYPE)0)
#endif

/** test for a pre-defined hash comparison */
#ifndef FIO_SET_HASH_COMPARE
#define FIO_SET_HASH_COMPARE(h1, h2) ((h1) == (h2))
#endif

/* Customizable memory management */
#ifndef FIO_SET_REALLOC /* NULL ptr indicates new allocation */
#define FIO_SET_REALLOC(ptr, original_size, new_size, valid_data_length) \
  FIO_REALLOC((ptr), (new_size), (valid_data_length))
#endif

#ifndef FIO_SET_CALLOC
#define FIO_SET_CALLOC(size, count) FIO_CALLOC((size), (count))
#endif

#ifndef FIO_SET_FREE
#define FIO_SET_FREE(ptr, size) FIO_FREE((ptr))
#endif

/* The maximum number of bins to rotate when (partial/full) collisions occure */
#ifndef FIO_SET_MAX_MAP_SEEK
#define FIO_SET_MAX_MAP_SEEK (96)
#endif

/* The maximum number of full hash collisions that can be consumed */
#ifndef FIO_SET_MAX_MAP_FULL_COLLISIONS
#define FIO_SET_MAX_MAP_FULL_COLLISIONS (96)
#endif

/* Prime numbers are better */
#ifndef FIO_SET_CUCKOO_STEPS
#define FIO_SET_CUCKOO_STEPS 11
#endif

#ifdef FIO_SET_KEY_TYPE
typedef struct {
  FIO_SET_KEY_TYPE key;
  FIO_SET_OBJ_TYPE obj;
} FIO_NAME(couplet_s);

#define FIO_SET_TYPE FIO_NAME(couplet_s)

/** key copy required? */
#ifndef FIO_SET_KEY_COPY
#define FIO_SET_KEY_COPY(dest, obj) ((dest) = (obj))
#endif

/** key destruction required? */
#ifndef FIO_SET_KEY_DESTROY
#define FIO_SET_KEY_DESTROY(obj) ((void)0)
#endif

/* The default Hash Map-Set has will use straight euqality operators */
#ifndef FIO_SET_KEY_COMPARE
#define FIO_SET_KEY_COMPARE(o1, o2) ((o1) == (o2))
#endif

/** Internal macros for object actions in Hash mode */
#define FIO_SET_COMPARE(o1, o2) FIO_SET_KEY_COMPARE((o1).key, (o2).key)
#define FIO_SET_COPY(dest, src)              \
  do {                                       \
    FIO_SET_OBJ_COPY((dest).obj, (src).obj); \
    FIO_SET_KEY_COPY((dest).key, (src).key); \
  } while (0);
#define FIO_SET_DESTROY(couplet)        \
  do {                                  \
    FIO_SET_KEY_DESTROY((couplet).key); \
    FIO_SET_OBJ_DESTROY((couplet).obj); \
  } while (0);

#else /* a pure Set, not a Hash Map*/
/** Internal macros for object actions in Set mode */
#define FIO_SET_COMPARE(o1, o2) FIO_SET_OBJ_COMPARE((o1), (o2))
#define FIO_SET_COPY(dest, obj) FIO_SET_OBJ_COPY((dest), (obj))
#define FIO_SET_DESTROY(obj) FIO_SET_OBJ_DESTROY((obj))
#define FIO_SET_TYPE FIO_SET_OBJ_TYPE
#endif

/* *****************************************************************************
Set / Hash Map API
***************************************************************************** */

/** The Set container type. By default: fio_ptr_set_s */
typedef struct FIO_NAME(s) FIO_NAME(s);

#ifndef FIO_SET_INIT
/** Initializes the set */
#define FIO_SET_INIT {.capa = 0}
#endif

/** Frees all the objects in the set and deallocates any internal resources. */
FIO_FUNC void FIO_NAME_FREE()(FIO_NAME(s) * set);

#ifdef FIO_SET_KEY_TYPE

/**
 * Locates an object in the Hash Map, if it exists.
 *
 * NOTE: This is the function's Hash Map variant. See FIO_SET_KEY_TYPE.
 */
FIO_FUNC inline FIO_SET_OBJ_TYPE FIO_NAME(find)(
    FIO_NAME(s) * set, const FIO_SET_HASH_TYPE hash_value,
    FIO_SET_KEY_TYPE key);

/**
 * Inserts an object to the Hash Map, rehashing if required, returning the new
 * object's location using a pointer.
 *
 * If an object already exists in the Hash Map, it will be destroyed.
 *
 * If `old` is set, the existing object (if any) will be copied to the location
 * pointed to by `old` before it is destroyed.
 *
 * NOTE: This is the function's Hash Map variant. See FIO_SET_KEY_TYPE.
 */
FIO_FUNC inline void FIO_NAME(insert)(FIO_NAME(s) * set,
                                      const FIO_SET_HASH_TYPE hash_value,
                                      FIO_SET_KEY_TYPE key,
                                      FIO_SET_OBJ_TYPE obj,
                                      FIO_SET_OBJ_TYPE *old);

/**
 * Removes an object from the Hash Map, rehashing if required.
 *
 * Returns 0 on success and -1 if the object wasn't found.
 *
 * If `old` is set, the existing object (if any) will be copied to the location
 * pointed to by `old`.
 *
 * NOTE: This is the function's Hash Map variant. See FIO_SET_KEY_TYPE.
 */
FIO_FUNC inline int FIO_NAME(remove)(FIO_NAME(s) * set,
                                     const FIO_SET_HASH_TYPE hash_value,
                                     FIO_SET_KEY_TYPE key,
                                     FIO_SET_OBJ_TYPE *old);

#else

/**
 * Locates an object in the Set, if it exists.
 *
 * NOTE: This is the function's pure Set variant (no FIO_SET_KEY_TYPE).
 */
FIO_FUNC inline FIO_SET_OBJ_TYPE FIO_NAME(find)(
    FIO_NAME(s) * set, const FIO_SET_HASH_TYPE hash_value,
    FIO_SET_OBJ_TYPE obj);

/**
 * Inserts an object to the Set only if it's missing, rehashing if required,
 * returning the new (or old) object.
 *
 * If the object already exists in the set, than the new object will be
 * destroyed and the old object will be returned.
 *
 * NOTE: This is the function's pure Set variant (no FIO_SET_KEY_TYPE).
 */
FIO_FUNC inline FIO_SET_OBJ_TYPE FIO_NAME(insert)(
    FIO_NAME(s) * set, const FIO_SET_HASH_TYPE hash_value,
    FIO_SET_OBJ_TYPE obj);

/**
 * Inserts an object to the Set, rehashing if required, returning the new
 * object.
 *
 * If the object already exists in the set, it will be destroyed and
 * overwritten.
 *
 * When setting `old` to NULL, the function behaves the same as `overwrite`.
 */
FIO_FUNC FIO_SET_OBJ_TYPE
    FIO_NAME(overwrite)(FIO_NAME(s) * set, const FIO_SET_HASH_TYPE hash_value,
                        FIO_SET_OBJ_TYPE obj, FIO_SET_OBJ_TYPE *old);

/**
 * Removes an object from the Set, rehashing if required.
 *
 * Returns 0 on success and -1 if the object wasn't found.
 *
 * NOTE: This is the function's pure Set variant (no FIO_SET_KEY_TYPE).
 */
FIO_FUNC inline int FIO_NAME(remove)(FIO_NAME(s) * set,
                                     const FIO_SET_HASH_TYPE hash_value,
                                     FIO_SET_OBJ_TYPE obj,
                                     FIO_SET_OBJ_TYPE *old);

#endif
/**
 * Allows a peak at the Set's last element.
 *
 * Remember that objects might be destroyed if the Set is altered
 * (`FIO_SET_OBJ_DESTROY` / `FIO_SET_KEY_DESTROY`).
 */
FIO_FUNC inline FIO_SET_TYPE FIO_NAME(last)(FIO_NAME(s) * set);

/**
 * Allows the Hash to be momentarily used as a stack, destroying the last
 * object added (`FIO_SET_OBJ_DESTROY` / `FIO_SET_KEY_DESTROY`).
 */
FIO_FUNC inline void FIO_NAME(pop)(FIO_NAME(s) * set);

/** Returns the number of object currently in the Set. */
FIO_FUNC inline size_t FIO_NAME(count)(const FIO_NAME(s) * set);

/**
 * Returns a temporary theoretical Set capacity.
 * This could be used for testing performance and memory consumption.
 */
FIO_FUNC inline size_t FIO_NAME(capa)(const FIO_NAME(s) * set);

/**
 * Requires that a Set contains the minimal requested theoretical capacity.
 *
 * Returns the actual (temporary) theoretical capacity.
 */
FIO_FUNC inline size_t FIO_NAME(capa_require)(FIO_NAME(s) * set,
                                              size_t min_capa);

/**
 * Returns non-zero if the Set is fragmented (more than 50% holes).
 */
FIO_FUNC inline size_t FIO_NAME(is_fragmented)(const FIO_NAME(s) * set);

/**
 * Attempts to minimize memory usage by removing empty spaces caused by deleted
 * items and rehashing the Set.
 *
 * Returns the updated Set capacity.
 */
FIO_FUNC inline size_t FIO_NAME(compact)(FIO_NAME(s) * set);

/** Forces a rehashing of the Set. */
FIO_FUNC void FIO_NAME(rehash)(FIO_NAME(s) * set);

#ifndef FIO_SET_FOR_LOOP
/**
 * A macro for a `for` loop that iterates over all the Set's objects (in
 * order).
 *
 * `set` is a pointer to the Set variable and `pos` is a temporary variable
 * name to be created for iteration.
 *
 * `pos->hash` is the hashing value and `pos->obj` is the object's data.
 *
 * NOTICE: Since the Set might have "holes" (objects that were removed), it is
 * important to skip any `pos->hash == 0` or the equivalent of
 * `FIO_SET_HASH_COMPARE(pos->hash, FIO_SET_HASH_INVALID)`.
 */
#define FIO_SET_FOR_LOOP(set, pos)
#endif

/* *****************************************************************************
Set / Hash Map Internal Data Structures
***************************************************************************** */

typedef struct FIO_NAME(_ordered_s_) {
  FIO_SET_HASH_TYPE hash;
  FIO_SET_TYPE obj;
} FIO_NAME(_ordered_s_);

typedef struct FIO_NAME(_map_s_) {
  FIO_SET_HASH_TYPE hash; /* another copy for memory cache locality */
  FIO_NAME(_ordered_s_) * pos;
} FIO_NAME(_map_s_);

/* the information in the Hash Map structure should be considered READ ONLY. */
struct FIO_NAME(s) {
  uintptr_t count;
  uintptr_t capa;
  uintptr_t pos;
  FIO_NAME(_ordered_s_) * ordered;
  FIO_NAME(_map_s_) * map;
  uint8_t has_collisions;
  uint8_t used_bits;
  uint8_t under_attack;
};

#undef FIO_SET_FOR_LOOP
#define FIO_SET_FOR_LOOP(set, container)                      \
  for (__typeof__((set)->ordered) container = (set)->ordered; \
       container && (container < ((set)->ordered + (set)->pos)); ++container)

/* *****************************************************************************
Set / Hash Map Internal Helpers
***************************************************************************** */

/** Locates an object's map position in the Set, if it exists. */
FIO_FUNC inline FIO_NAME(_map_s_) *
    FIO_NAME(_find_map_pos_)(FIO_NAME(s) * set, FIO_SET_HASH_TYPE hash_value,
                             FIO_SET_TYPE obj) {
  if (FIO_SET_HASH_COMPARE(hash_value, FIO_SET_HASH_INVALID))
    hash_value = FIO_SET_HASH_FORCE;
  if (set->map) {
    /* make sure collisions don't effect seeking */
    if (set->has_collisions && set->pos != set->count) {
      FIO_NAME(rehash)(set);
    }
    size_t full_collisions_counter = 0;
    FIO_NAME(_map_s_) * pos;
    /*
     * Commonly, the hash is rotated, depending on it's state.
     * Different bits are used for each mapping, instead of a single new bit.
     */
    const uintptr_t mask = (1ULL << set->used_bits) - 1;

    uintptr_t i;
    const uintptr_t hash_value_i = FIO_SET_HASH2UINTPTR(hash_value, 0);
    uintptr_t hash_alt = FIO_SET_HASH2UINTPTR(hash_value, set->used_bits);

    /* O(1) access to object */
    pos = set->map + (hash_alt & mask);
    if (FIO_SET_HASH_COMPARE(FIO_SET_HASH_INVALID, pos->hash)) return pos;
    if (FIO_SET_HASH_COMPARE(pos->hash, hash_value_i)) {
      if (!pos->pos || (FIO_SET_COMPARE(pos->pos->obj, obj))) return pos;
      /* full hash value collision detected */
      set->has_collisions = 1;
      ++full_collisions_counter;
    }

    /* Handle partial / full collisions with cuckoo steps O(x) access time */
    i = 0;
    const uintptr_t limit =
        FIO_SET_CUCKOO_STEPS * (set->capa > (FIO_SET_MAX_MAP_SEEK << 2)
                                    ? FIO_SET_MAX_MAP_SEEK
                                    : (set->capa >> 2));
    while (i < limit) {
      i += FIO_SET_CUCKOO_STEPS;
      pos = set->map + ((hash_alt + i) & mask);
      if (FIO_SET_HASH_COMPARE(FIO_SET_HASH_INVALID, pos->hash)) return pos;
      if (FIO_SET_HASH_COMPARE(pos->hash, hash_value_i)) {
        if (!pos->pos || (FIO_SET_COMPARE(pos->pos->obj, obj))) return pos;
        /* full hash value collision detected */
        set->has_collisions = 1;
        if (++full_collisions_counter >= FIO_SET_MAX_MAP_FULL_COLLISIONS) {
          /* is the hash under attack? */
          FIO_LOG_WARNING(
              "(fio hash map) too many full collisions - under attack?");
          set->under_attack = 1;
        }
        if (set->under_attack) {
          return pos;
        }
      }
    }
  }
  return NULL;
  (void)obj; /* in cases where FIO_SET_OBJ_COMPARE does nothing */
}
#undef FIO_SET_CUCKOO_STEPS

/** Removes "holes" from the Set's internal Array - MUST re-hash afterwards.
 */
FIO_FUNC inline void FIO_NAME(_compact_ordered_array_)(FIO_NAME(s) * set) {
  if (set->count == set->pos) return;
  FIO_NAME(_ordered_s_) *reader = set->ordered;
  FIO_NAME(_ordered_s_) *writer = set->ordered;
  const FIO_NAME(_ordered_s_) *end = set->ordered + set->pos;
  for (; reader && (reader < end); ++reader) {
    if (FIO_SET_HASH_COMPARE(reader->hash, FIO_SET_HASH_INVALID)) {
      continue;
    }
    *writer = *reader;
    ++writer;
  }
  /* fix any possible counting errors as well as resetting position */
  set->pos = set->count = (writer - set->ordered);
}

/** (Re)allocates the set's internal, invalidatint the mapping (must rehash) */
FIO_FUNC inline void FIO_NAME(_reallocate_set_mem_)(FIO_NAME(s) * set) {
  const uintptr_t new_capa = 1ULL << set->used_bits;
  FIO_SET_FREE(set->map, set->capa * sizeof(*set->map));
  set->map = (FIO_NAME(_map_s_) *)FIO_SET_CALLOC(sizeof(*set->map), new_capa);
  set->ordered = (FIO_NAME(_ordered_s_) *)FIO_SET_REALLOC(
      set->ordered, (set->capa * sizeof(*set->ordered)),
      (new_capa * sizeof(*set->ordered)), (set->pos * sizeof(*set->ordered)));
  if (!set->map || !set->ordered) {
    perror("FATAL ERROR: couldn't allocate memory for Set data");
    exit(errno);
  }
  set->capa = new_capa;
}

/**
 * Inserts an object to the Set, rehashing if required, returning the new
 * object's pointer.
 *
 * If the object already exists in the set, it will be destroyed and
 * overwritten.
 */
FIO_FUNC inline FIO_SET_TYPE FIO_NAME(_insert_or_overwrite_)(
    FIO_NAME(s) * set, FIO_SET_HASH_TYPE hash_value, FIO_SET_TYPE obj,
    int overwrite, FIO_SET_OBJ_TYPE *old) {
  if (FIO_SET_HASH_COMPARE(hash_value, FIO_SET_HASH_INVALID))
    hash_value = FIO_SET_HASH_FORCE;

  /* automatic fragmentation protection */
  if (FIO_NAME(is_fragmented)(set)) FIO_NAME(rehash)(set);
  /* automatic capacity validation (we can never be at 100% capacity) */
  else if (set->pos >= set->capa) {
    ++set->used_bits;
    FIO_NAME(rehash)(set);
  }

  /* locate future position */
  FIO_NAME(_map_s_) *pos = FIO_NAME(_find_map_pos_)(set, hash_value, obj);

  if (!pos) {
    /* inserting a new object, with too many holes in the map */
    FIO_SET_COPY(set->ordered[set->pos].obj, obj);
    set->ordered[set->pos].hash = hash_value;
    ++set->pos;
    ++set->count;
    FIO_NAME(rehash)(set);
    return set->ordered[set->pos - 1].obj;
  }

  /* overwriting / new */
  if (pos->pos) {
    /* overwrite existing object */
    if (!overwrite) {
      FIO_SET_DESTROY(obj);
      return pos->pos->obj;
    }
#ifdef FIO_SET_KEY_TYPE
    if (old) {
      FIO_SET_OBJ_COPY((*old), pos->pos->obj.obj);
    }
    /* no need to recreate the key object, just the value object */
    FIO_SET_OBJ_DESTROY(pos->pos->obj.obj);
    FIO_SET_OBJ_COPY(pos->pos->obj.obj, obj.obj);
    return pos->pos->obj;
#else
    if (old) {
      FIO_SET_COPY((*old), pos->pos->obj);
    }
    FIO_SET_DESTROY(pos->pos->obj);
#endif
  } else {
    /* insert into new slot */
    pos->pos = set->ordered + set->pos;
    ++set->pos;
    ++set->count;
  }
  /* store object at position */
  pos->hash = hash_value;
  pos->pos->hash = hash_value;
  FIO_SET_COPY(pos->pos->obj, obj);

  return pos->pos->obj;
}

/* *****************************************************************************
Set / Hash Map Implementation
***************************************************************************** */

/** Frees all the objects in the set and deallocates any internal resources. */
FIO_FUNC void FIO_NAME_FREE()(FIO_NAME(s) * s) {
  /* destroy existing valid objects */
  const FIO_NAME(_ordered_s_) *const end = s->ordered + s->pos;
  if (s->ordered && s->ordered != end) {
    for (FIO_NAME(_ordered_s_) *pos = s->ordered; pos < end; ++pos) {
      if (!FIO_SET_HASH_COMPARE(FIO_SET_HASH_INVALID, pos->hash)) {
        FIO_SET_DESTROY(pos->obj);
      }
    }
  }
  /* free ordered array and hash mapping */
  FIO_SET_FREE(s->map, s->capa * sizeof(*s->map));
  FIO_SET_FREE(s->ordered, s->capa * sizeof(*s->ordered));
  *s = (FIO_NAME(s)){.map = NULL};
}

#ifdef FIO_SET_KEY_TYPE

/* Hash Map unique implementation */

/**
 * Locates an object in the Set, if it exists.
 *
 * NOTE: This is the function's Hash Map variant. See FIO_SET_KEY_TYPE.
 */
FIO_FUNC FIO_SET_OBJ_TYPE FIO_NAME(find)(FIO_NAME(s) * set,
                                         const FIO_SET_HASH_TYPE hash_value,
                                         FIO_SET_KEY_TYPE key) {
  FIO_NAME(_map_s_) *pos =
      FIO_NAME(_find_map_pos_)(set, hash_value, (FIO_SET_TYPE){.key = key});
  if (!pos || !pos->pos) {
    FIO_SET_OBJ_TYPE empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
  }
  return pos->pos->obj.obj;
}

/**
 * Inserts an object to the Hash Map, rehashing if required, returning the new
 * object's location using a pointer.
 *
 * If an object already exists in the Hash Map, it will be destroyed.
 *
 * If `old` is set, the existing object (if any) will be copied to the location
 * pointed to by `old` before it is destroyed.
 *
 * NOTE: This is the function's Hash Map variant. See FIO_SET_KEY_TYPE.
 */
FIO_FUNC void FIO_NAME(insert)(FIO_NAME(s) * set,
                               const FIO_SET_HASH_TYPE hash_value,
                               FIO_SET_KEY_TYPE key, FIO_SET_OBJ_TYPE obj,
                               FIO_SET_OBJ_TYPE *old) {
  FIO_NAME(_insert_or_overwrite_)
  (set, hash_value, (FIO_SET_TYPE){.key = key, .obj = obj}, 1, old);
}

/**
 * Removes an object from the Hash Map, rehashing if required.
 *
 * Returns 0 on success and -1 if the object wasn't found.
 *
 * If `old` is set, the existing object (if any) will be copied to the location
 * pointed to by `old`.
 *
 * NOTE: This is the function's Hash Map variant. See FIO_SET_KEY_TYPE.
 */
FIO_FUNC inline int FIO_NAME(remove)(FIO_NAME(s) * set,
                                     const FIO_SET_HASH_TYPE hash_value,
                                     FIO_SET_KEY_TYPE key,
                                     FIO_SET_OBJ_TYPE *old) {
  FIO_NAME(_map_s_) *pos =
      FIO_NAME(_find_map_pos_)(set, hash_value, (FIO_SET_TYPE){.key = key});
  if (!pos || !pos->pos) return -1;
  if (old) FIO_SET_OBJ_COPY((*old), pos->pos->obj.obj);
  FIO_SET_DESTROY(pos->pos->obj);
  --set->count;
  pos->pos->hash = FIO_SET_HASH_INVALID;
  if (pos->pos == set->pos + set->ordered - 1) {
    /* removing last item inserted */
    pos->hash = FIO_SET_HASH_INVALID; /* no need for a "hole" */
    do {
      --set->pos;
    } while (set->pos && FIO_SET_HASH_COMPARE(set->ordered[set->pos - 1].hash,
                                              FIO_SET_HASH_INVALID));
  }
  pos->pos = NULL; /* leave pos->hash set to mark "hole" */
  return 0;
}

#else /* FIO_SET_KEY_TYPE */

/* Set unique implementation */

/** Locates an object in the Set, if it exists. */
FIO_FUNC FIO_SET_OBJ_TYPE FIO_NAME(find)(FIO_NAME(s) * set,
                                         const FIO_SET_HASH_TYPE hash_value,
                                         FIO_SET_OBJ_TYPE obj) {
  FIO_NAME(_map_s_) *pos = FIO_NAME(_find_map_pos_)(set, hash_value, obj);
  if (!pos || !pos->pos) {
    FIO_SET_OBJ_TYPE empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
  }
  return pos->pos->obj;
}

/**
 * Inserts an object to the Set, rehashing if required, returning the new
 * object's pointer.
 *
 * If the object already exists in the set, than the new object will be
 * destroyed and the old object's address will be returned.
 */
FIO_FUNC FIO_SET_OBJ_TYPE FIO_NAME(insert)(FIO_NAME(s) * set,
                                           const FIO_SET_HASH_TYPE hash_value,
                                           FIO_SET_OBJ_TYPE obj) {
  return FIO_NAME(_insert_or_overwrite_)(set, hash_value, obj, 0, NULL);
}

/**
 * Inserts an object to the Set, rehashing if required, returning the new
 * object's pointer.
 *
 * If the object already exists in the set, it will be destroyed and
 * overwritten.
 *
 * When setting `old` to NULL, the function behaves the same as `overwrite`.
 */
FIO_FUNC FIO_SET_OBJ_TYPE
FIO_NAME(overwrite)(FIO_NAME(s) * set, const FIO_SET_HASH_TYPE hash_value,
                    FIO_SET_OBJ_TYPE obj, FIO_SET_OBJ_TYPE *old) {
  return FIO_NAME(_insert_or_overwrite_)(set, hash_value, obj, 1, old);
}

/**
 * Removes an object from the Set, rehashing if required.
 */
FIO_FUNC int FIO_NAME(remove)(FIO_NAME(s) * set,
                              const FIO_SET_HASH_TYPE hash_value,
                              FIO_SET_OBJ_TYPE obj, FIO_SET_OBJ_TYPE *old) {
  if (FIO_SET_HASH_COMPARE(hash_value, FIO_SET_HASH_INVALID)) return -1;
  FIO_NAME(_map_s_) *pos = FIO_NAME(_find_map_pos_)(set, hash_value, obj);
  if (!pos || !pos->pos) return -1;
  if (old) FIO_SET_COPY((*old), pos->pos->obj);
  FIO_SET_DESTROY(pos->pos->obj);
  --set->count;
  pos->pos->hash = FIO_SET_HASH_INVALID;
  if (pos->pos == set->pos + set->ordered - 1) {
    /* removing last item inserted */
    pos->hash = FIO_SET_HASH_INVALID; /* no need for a "hole" */
    do {
      --set->pos;
    } while (set->pos && FIO_SET_HASH_COMPARE(set->ordered[set->pos - 1].hash,
                                              FIO_SET_HASH_INVALID));
  }
  pos->pos = NULL; /* leave pos->hash set to mark "hole" */
  return 0;
}

#endif

/**
 * Allows a peak at the Set's last element.
 *
 * Remember that objects might be destroyed if the Set is altered
 * (`FIO_SET_OBJ_DESTROY` / `FIO_SET_KEY_DESTROY`).
 */
FIO_FUNC inline FIO_SET_TYPE FIO_NAME(last)(FIO_NAME(s) * set) {
  if (!set->ordered || !set->pos) {
    FIO_SET_TYPE empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
  }
  return set->ordered[set->pos - 1].obj;
}

/**
 * Allows the Hash to be momentarily used as a stack, destroying the last
 * object added (`FIO_SET_OBJ_DESTROY` / `FIO_SET_KEY_DESTROY`).
 */
FIO_FUNC void FIO_NAME(pop)(FIO_NAME(s) * set) {
  if (!set->ordered || !set->pos) return;
  FIO_SET_DESTROY(set->ordered[set->pos - 1].obj);
  set->ordered[set->pos - 1].hash = FIO_SET_HASH_INVALID;
  --(set->count);
  do {
    --(set->pos);
  } while (set->pos && FIO_SET_HASH_COMPARE(set->ordered[set->pos - 1].hash,
                                            FIO_SET_HASH_INVALID));
}

/** Returns the number of objects currently in the Set. */
FIO_FUNC inline size_t FIO_NAME(count)(const FIO_NAME(s) * set) {
  return (size_t)set->count;
}

/**
 * Returns a temporary theoretical Set capacity.
 * This could be used for testing performance and memory consumption.
 */
FIO_FUNC inline size_t FIO_NAME(capa)(const FIO_NAME(s) * set) {
  return (size_t)set->capa;
}

/**
 * Requires that a Set contains the minimal requested theoretical capacity.
 *
 * Returns the actual (temporary) theoretical capacity.
 */
FIO_FUNC inline size_t FIO_NAME(capa_require)(FIO_NAME(s) * set,
                                              size_t min_capa) {
  if (min_capa <= FIO_NAME(capa)(set)) return FIO_NAME(capa)(set);
  set->used_bits = 2;
  while (min_capa > (1ULL << set->used_bits)) {
    ++set->used_bits;
  }
  FIO_NAME(rehash)(set);
  return FIO_NAME(capa)(set);
}

/**
 * Returns non-zero if the Set is fragmented (more than 50% holes).
 */
FIO_FUNC inline size_t FIO_NAME(is_fragmented)(const FIO_NAME(s) * set) {
  return ((set->pos - set->count) > (set->count >> 1));
}

/**
 * Attempts to minimize memory usage by removing empty spaces caused by deleted
 * items and rehashing the Set.
 *
 * Returns the updated Set capacity.
 */
FIO_FUNC inline size_t FIO_NAME(compact)(FIO_NAME(s) * set) {
  FIO_NAME(_compact_ordered_array_)(set);
  set->used_bits = 2;
  while (set->count >= (1ULL << set->used_bits)) {
    ++set->used_bits;
  }
  FIO_NAME(rehash)(set);
  return FIO_NAME(capa)(set);
}

/** Forces a rehashing of the Set. */
FIO_FUNC void FIO_NAME(rehash)(FIO_NAME(s) * set) {
  FIO_NAME(_compact_ordered_array_)(set);
  set->has_collisions = 0;
  uint8_t attempts = 0;
restart:
  if (set->used_bits >= 16 && ++attempts >= 3 && set->has_collisions) {
    FIO_LOG_FATAL(
        "facil.io Set / Hash Map has too many collisions (%zu/%zu)."
        "\n\t\tthis is a fatal implementation error,"
        "please report this issue at facio.io's open source project"
        "\n\t\tNote: hash maps and sets should never reach this point."
        "\n\t\tThey should be guarded against collision attacks.",
        set->pos, set->capa);
    exit(-1);
  }
  FIO_NAME(_reallocate_set_mem_)(set);
  {
    FIO_NAME(_ordered_s_) const *const end = set->ordered + set->pos;
    for (FIO_NAME(_ordered_s_) *pos = set->ordered; pos < end; ++pos) {
      FIO_NAME(_map_s_) *mp =
          FIO_NAME(_find_map_pos_)(set, pos->hash, pos->obj);
      if (!mp) {
        ++set->used_bits;
        goto restart;
      }
      mp->pos = pos;
      mp->hash = pos->hash;
    }
  }
}

#undef FIO_SET_OBJ_TYPE
#undef FIO_SET_OBJ_COMPARE
#undef FIO_SET_OBJ_COPY
#undef FIO_SET_OBJ_DESTROY
#undef FIO_SET_HASH_TYPE
#undef FIO_SET_HASH2UINTPTR
#undef FIO_SET_HASH_COMPARE
#undef FIO_SET_HASH_INVALID
#undef FIO_SET_KEY_TYPE
#undef FIO_SET_KEY_COPY
#undef FIO_SET_KEY_DESTROY
#undef FIO_SET_KEY_COMPARE
#undef FIO_SET_TYPE
#undef FIO_SET_COMPARE
#undef FIO_SET_COPY
#undef FIO_SET_DESTROY
#undef FIO_SET_MAX_MAP_SEEK
#undef FIO_SET_MAX_MAP_FULL_COLLISIONS
#undef FIO_SET_REALLOC
#undef FIO_SET_CALLOC
#undef FIO_SET_FREE
#undef FIO_NAME
#undef FIO_NAME_FROM_MACRO_STEP2
#undef FIO_NAME_FROM_MACRO_STEP1
#undef FIO_NAME_FROM_MACRO_STEP4
#undef FIO_NAME_FROM_MACRO_STEP3
#undef FIO_NAME_FREE
#undef FIO_SET_NAME
#undef FIO_FORCE_MALLOC_TMP

#endif
