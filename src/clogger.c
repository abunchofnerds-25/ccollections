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

#define _GNU_SOURCE

#include <cdebuglog.h>
#include <chashmap.h>
#include <clogger.h>
#include <common.h>
#include <cthreadcomm.h>
#include <ctype.h>
#include <cvector.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif
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
/* Extra headroom for collision suffix "_0001"-"_9999" */
#define CLOG_ROTATION_EXTRA 8
/* Once a rotation attempt fails (e.g. a permission error or a file path too
 * long for the internal rotated-name buffer), further attempts are backed
 * off by this many seconds rather than retried on every single write; a
 * persistent failure would otherwise cost every subsequent write the full
 * rename()/open() syscall overhead for as long as the underlying condition
 * remains, in addition to leaving size-based rotation permanently unable to
 * make progress against its own trigger with no bound on the attempt rate. */
#define CLOG_ROTATE_RETRY_BACKOFF_SECS ((time_t)1)

static const char *const _LEVEL_STR[] = {"TRACE", "DEBUG", "INFO",  "WARN",
                                         "ERROR", "ALERT", "FATAL", "OFF"};
/* Both arrays above/below are indexed directly by clog_level_t; keep their
 * sizes tied to the enum's cardinality so a future level insertion that
 * forgets to grow one of them fails to compile instead of silently
 * misindexing at runtime. */
_Static_assert(sizeof(_LEVEL_STR) / sizeof(_LEVEL_STR[0]) == CLOG_OFF + 1,
               "_LEVEL_STR must have exactly one entry per clog_level_t value");

/*
 * Keys clog_set_field() rejects: every fixed output key _clog_write() itself
 * always emits (in every format).  Allowing a user field to reuse one of
 * these would produce a duplicate "key":.. pair in JSON, a duplicate key=
 * token in logfmt, and a duplicate SD-PARAM-NAME within the same SD-ELEMENT
 * in syslog output (RFC 5424 requires SD-PARAM-NAME to be unique per
 * SD-ELEMENT).
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

/* RFC 5424 severity codes indexed by clog_level_t. */
static const int _SYSLOG_SEVERITY[] = {
    7, /* TRACE -> debug         */
    7, /* DEBUG -> debug         */
    6, /* INFO  -> informational */
    4, /* WARN  -> warning       */
    3, /* ERROR -> error         */
    1, /* ALERT -> alert         */
    0, /* FATAL -> emergency     */
    7, /* OFF; sentinel; CLOG_OFF must never be used as a message level */
};
_Static_assert(
    sizeof(_SYSLOG_SEVERITY) / sizeof(_SYSLOG_SEVERITY[0]) == CLOG_OFF + 1,
    "_SYSLOG_SEVERITY must have exactly one entry per clog_level_t value");

/* ========================================================================== */
/*                         INTERNAL STRUCTURES                                */
/* ========================================================================== */

/*
 * A single node of an intrusive, stack-allocated singly-linked list of rotated
 * files whose gzip compression is currently in flight (i.e. _rotate() has
 * released shared->mutex to run _gzip_compress_file() on this exact
 * path/gz_path pair). Every node lives on the stack frame of the _rotate()
 * call that owns it for the entire time it is linked into
 * clog_shared_t.pending_compress (pushed just before the mutex is released,
 * popped just after it is re-acquired), so no heap allocation is needed and
 * no lifetime hazard exists: the list is only ever read or mutated while
 * shared->mutex is held.
 *
 * Both `path` (the uncompressed source _gzip_compress_file() is reading) and
 * `gz_path` (the compressed destination it is writing) must be protected: a
 * concurrent _prune_rotated() pass can see the destination on disk from the
 * moment gzopen() creates it, well before the write finishes, so omitting it
 * here would let a tight max_rotated_files quota delete an in-progress .gz
 * file out from under its own writer.
 */
typedef struct clog_pending_compress {
  const char *path;
  const char *gz_path;
  struct clog_pending_compress *next;
} clog_pending_compress_t;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
  /* Hard growth ceiling for THIS buffer, in bytes. Defaults to CLOG_BUF_MAX
   * (set by _buf_init()); raised above that default only for
   * clog_shared_t.async_buf, and only as far as a caller's own configured
   * clog_async_cfg_t.flush_buffer_size actually requires (see
   * _shared_async_init()); every other clog_buf_t in this file (every
   * per-logger lg->buf, and every scratch buffer) keeps the CLOG_BUF_MAX
   * default, since that limit is about bounding a single oversized record,
   * a concern independent of how large a caller wants the async batch itself
   * to grow before flushing. */
  size_t cap_limit;
  const ccol_memmgmt_procs_t *m_procs; /* borrowed from clog_shared_t */
  /* Set by _buf_ensure() when a growth attempt failed because the
   * underlying allocator call itself failed (a genuine OOM), as opposed to
   * merely hitting this buffer's own cap_limit (a legitimately oversized
   * record, or simply not enough room left in a shared batch buffer);
   * both report failure identically to _buf_ensure()'s own caller, so this
   * is the only place that distinguishes them. Reset to false by
   * _clog_build_record() at the start of every record-build attempt, and
   * consulted there afterward (regardless of which nested append call
   * actually set it) to report an accurate cause to
   * _clog_build_fallback_record() instead of always blaming size. */
  bool oom;
} clog_buf_t;

/*
 * Shared backing store: fd, rotation state, and mutex.
 * Reference-counted so the root logger and all derived loggers share one
 * instance.  The fd is closed (if owned) when ref_count reaches zero.
 */
typedef struct clog_shared {
  int fd;
  bool owns_fd;
  char *file_path; /* NULL for fd-based loggers */

  bool rotation_enabled;
  clog_rotation_cfg_t rotation;
  off_t bytes_written;
  time_t last_rotation;
  /* Earliest time a rotation may be attempted again after a failed
   * _rotate() call; 0 (the calloc'd default) means no backoff is in effect.
   * See CLOG_ROTATE_RETRY_BACKOFF_SECS. */
  time_t rotate_retry_after;
  /* Rotated files currently being gzip-compressed by some in-progress
   * _rotate() call (see clog_pending_compress_t above); consulted by
   * _prune_rotated() so a concurrent rotation's pruning pass can never
   * delete a file another rotation has not finished reading yet. */
  clog_pending_compress_t *pending_compress;

  mutex_t mutex;
  int ref_count;
  clog_format_t format;
  clog_syslog_facility_t syslog_facility; /* PRI facility for CLOG_FMT_SYSLOG */
  char syslog_hostname[256]; /* hostname cached at creation       */
  char syslog_appname[49];   /* APP-NAME cached at creation        */
  ccol_memmgmt_procs_t
      *m_procs; /* heap-allocated copy; NULL = default allocator */

  /* Async logging (see "ASYNC LOGGING" below); every field in this block is
   * zeroed/unused for a synchronous logger (async_enabled == false), so a
   * synchronous logger's behavior is byte-for-byte unchanged from before
   * async logging existed. async_enabled itself is written exactly once, by
   * the constructing thread before this shared object is ever published to
   * any other thread (or, in a forked child, by _clog_atfork_child's own
   * downgrade; see that function's own comment), so every other reader
   * (including _clog_write()'s own dispatch check) may read it without a
   * lock: it is never mutated concurrently with a read. */
  bool async_enabled;
  bool is_bounded_queue; /* meaningful only if async_enabled */
  union {
    circular_queue *circq;
    dynamic_queue *dynmq;
  } q;
  clog_async_cfg_t async_cfg; /* resolved copy, defaults already applied */
  thread_id_t writer_thread;
  clog_buf_t async_buf; /* writer-thread-owned aggregation buffer; distinct
      from any individual handle's own lg->buf, since jobs from every handle
      sharing this shared target land in ONE buffer */
  /* CLOCK_MONOTONIC, deliberately NOT gettimeofday()/CLOCK_REALTIME: this is
   * purely an internal "how long has it been since our last flush"
   * bookkeeping value the writer thread's own main loop uses to compute how
   * long to wait before its next opportunity to flush (see
   * _clog_writer_thread_main() below), not a value ever surfaced to a caller
   * or embedded in a record. A wall-clock-based measurement here would let a
   * backward system clock adjustment (an NTP step correction, a manual date
   * change, a VM live-migration clock correction) between two flushes make
   * the elapsed-time computation swing arbitrarily negative and the
   * writer thread's next wait balloon far past flush_interval_ms, silently
   * violating the "flushes at least this often" guarantee
   * clog_async_cfg_t.flush_interval_ms documents. CLOCK_MONOTONIC is immune
   * to exactly this class of adjustment. */
  struct timespec last_flush_monotonic;
} clog_shared_t;

struct clogger {
  clog_shared_t *shared; /* shared output backing store              */
  /* Per-logger level filter. _Atomic so _clog_write() can cheaply check it
   * before acquiring shared->mutex (see the fast-path check there), without
   * that unlocked read being a data race against a concurrent
   * clog_set_level() call on the same handle from another thread. */
  _Atomic clog_level_t min_level;
  /* Guards `fields` only, decoupled from shared->mutex (which serialises fd
   * writes/rotation across an entire derive tree): each logger's field map
   * is otherwise fully independent of every sibling logger sharing the same
   * `shared`, so clog_set_field()/_remove_field()/_clear_fields() on one
   * derived logger must not contend with an unrelated write in progress on
   * a sibling. _clog_write() and clog_derive() take this lock only for the
   * brief window where they actually read `fields`; whenever both this lock
   * and shared->mutex are needed together (as in _clog_write(), which
   * already holds shared->mutex for the whole call), shared->mutex is always
   * acquired first, never the reverse, so no lock-ordering cycle is
   * possible anywhere in this file. */
  mutex_t fields_mutex;
  chmap fields;   /* chmap(char* -> char*); per-logger fields */
  clog_buf_t buf; /* per-logger reusable write buffer          */
  /* Pinned by _clog_resolve() (lock-free atomic increment) for as long as a
   * caller holds a resolved pointer to this handle; clog_close() poll-waits
   * on this reaching 0 (see _clog_resolve_unpin()'s own doc comment for why
   * that decrement is a bare atomic op with no lock/broadcast, unlike the
   * chttpsvr/chttpcli slot tables' own analogous field) before touching any
   * per-handle state. */
  _Atomic size_t pending_resolve_count;
};

/* ========================================================================== */
/*                         HANDLE SLOT TABLE                                  */
/* ========================================================================== */

/*
 * clog is an opaque {slot index, generation} value handle, not a pointer.
 * This table mirrors chttpsvr_slot_table/chttpcli_slot_table
 * (src/chttpserver.c, src/chttpclient.c) closely, with one deliberate
 * deviation: clogger's own resolve is on the hot path of every single
 * log_* call (including filtered-out ones), far more frequent than either
 * of those modules' own resolve sites, so this table uses a reader-writer
 * lock (concurrent resolvers never serialise against each other) instead of
 * a plain mutex, and a lock-free unpin (a bare atomic decrement, no
 * mutex+condvar broadcast) instead of the mutex-guarded decrement those two
 * modules use.
 */
typedef struct {
  struct clogger *ptr; /* NULL when free */
  uint32_t generation; /* 0 pre-first-use, becomes 1 on first acquire */
  bool in_use;         /* false from clog_close()'s step 2 onward; gates
      _clog_resolve only; does NOT mean ptr is safe to dereference for
      fields_mutex protection purposes, see `freed` below */
  bool freed;          /* false until _logger_free(ptr) has actually run; see
               "Fork safety" below; _clog_atfork_prepare's fields_mutex walk gates
               on THIS field, not in_use, precisely because in_use goes false well
               before ptr->fields_mutex is actually destroyed */
} clog_slot_t;

static struct {
  rw_lock_t rwlock;
  once_flag_t once;
  cvec slots;        /* cvector of clog_slot_t */
  cvec free_indices; /* cvector of uint32_t; LIFO */
  cvec live_shareds; /* cvector of clog_shared_t*; a clog_shared_t's whole
      lifetime, from its first handle's acquisition to its own actual free,
      independent of any individual handle's in_use flag; see "Fork
      safety" below for why this dedicated registry exists rather than
      deriving the set of live clog_shared_t objects from the handle table */
} clog_slot_table = {0};

#if FORK_SAFETY_REQUIRED
/*
 * One slot caught mid-clog_close() (in_use == false, freed == false) at
 * fork() time; see _clog_atfork_prepare()'s own recording of these and
 * _clog_atfork_release()'s own child-side finishing of them, below, for why
 * this needs tracking distinct from the ordinary locked_fields protection
 * every live slot already gets.
 *
 * This type, _clog_atfork_state below, and every function/registration site
 * that touches either of them are compiled out entirely when
 * FORK_SAFETY_REQUIRED is 0 (see that macro's own doc comment in common.h);
 * clog_slot_table.slots/free_indices/live_shareds and clog_slot_t.freed
 * themselves stay unconditionally compiled, since they also serve this
 * module's ordinary (non-fork) handle lifecycle, not fork-safety alone.
 */
typedef struct {
  uint32_t idx;
  struct clogger *raw;
} clog_atfork_closing_t;

/*
 * Every lock acquired by _clog_atfork_prepare, recorded here so
 * _clog_atfork_parent/_child can release exactly what was taken. Accessed
 * only by the forking thread, only between prepare and the matching
 * parent/child call; never concurrently, so it needs no lock of its own.
 * clog_slot_table.live_shareds itself (not a field here) is what
 * _clog_atfork_prepare walks for shared->mutex protection.
 */
static struct {
  cvec locked_fields; /* cvector of struct clogger* */
  cvec closing_slots; /* cvector of clog_atfork_closing_t; see that type's
     own doc comment */
} _clog_atfork_state = {0};

static void _clog_atfork_prepare(void);
static void _clog_atfork_parent(void);
static void _clog_atfork_child(void);
#endif /* FORK_SAFETY_REQUIRED */
static void _clog_slot_table_init_globals(void);

/* Forward-declared: defined much further down (in the "CONSTRUCTOR HELPERS"
 * section), but _clog_atfork_release()'s own child-side handling needs to
 * call all three of them to finish a clog_close() call abandoned mid-
 * teardown by a fork(); see that function's own doc comment. */
static void _logger_free(struct clogger *lg);
static void _shared_close_owned_fd(clog_shared_t *sh);
static void _shared_free_partial(clog_shared_t *sh);

#ifdef RUNNING_UNIT_TESTS
/*
 * Lets a test deterministically force the NEXT _clog_handle_acquire() call to
 * (1) grow clog_slot_table.slots with a brand new slot, exactly as if
 * free_indices were empty, regardless of whatever slots earlier tests in
 * this same process may have already left sitting there for reuse, and (2)
 * fail that same call's own live_shareds registration step, without
 * depending on the process's plain default allocator (the only allocator
 * clog_slot_table.slots/free_indices/live_shareds ever use, regardless of
 * any per-logger custom mprocs; see _clog_slot_table_init_globals()) ever
 * actually failing for real. Both effects are needed together to reach the
 * one rollback path that used to leave a slot behind with freed == false and
 * ptr == NULL simultaneously (see _clog_handle_acquire()'s own comment on
 * that rollback): a reused slot popped off free_indices is already
 * freed == true from its previous occupant's own clog_close(), so only a
 * freshly-grown slot whose OWN registration step then fails can reach it.
 * Auto-disarms itself the moment it fires, so only the one acquisition a
 * test is targeting is affected.
 */
static _Atomic bool _clog_test_force_next_fresh_slot_reg_failure = false;

static bool _clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return atomic_exchange(&_clog_test_force_next_fresh_slot_reg_failure, false);
}

void clog_test_force_next_fresh_slot_registration_failure(bool force) {
  atomic_store(&_clog_test_force_next_fresh_slot_reg_failure, force);
}

/* See clog_test_set_close_finalize_delay_us()'s own doc comment in
 * clogger.h. Deliberately does NOT auto-disarm (unlike the hook just above):
 * a test needs this armed across the whole clog_close() call under test,
 * not just its own first consumption. */
static _Atomic unsigned int _clog_test_close_finalize_delay_us = 0;
static _Atomic bool _clog_test_close_finalize_delay_entered = false;

void clog_test_set_close_finalize_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_close_finalize_delay_entered, false);
  atomic_store(&_clog_test_close_finalize_delay_us, delay_us);
}

bool clog_test_close_finalize_delay_entered(void) {
  return atomic_load(&_clog_test_close_finalize_delay_entered);
}

size_t clog_test_live_shareds_count(void) {
  call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  rw_lock_rdlock(clog_slot_table.rwlock);
  size_t n = cvector_elem_count(clog_slot_table.live_shareds);
  rw_lock_unlock(clog_slot_table.rwlock);
  return n;
}
#else
static inline bool _clog_test_consume_forced_fresh_slot_reg_failure(void) {
  return false;
}
#endif

static void _clog_slot_table_init_globals(void) {
  rw_lock_init(clog_slot_table.rwlock);
  clog_slot_table.slots = cvector_create(sizeof(clog_slot_t), NULL);
  if (!clog_slot_table.slots)
    fatal_err("clog slot table: failed to allocate slots vector");
  clog_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!clog_slot_table.free_indices)
    fatal_err("clog slot table: failed to allocate free-index vector");
  clog_slot_table.live_shareds = cvector_create(sizeof(clog_shared_t *), NULL);
  if (!clog_slot_table.live_shareds)
    fatal_err("clog slot table: failed to allocate live-shareds vector");
#if FORK_SAFETY_REQUIRED
  _clog_atfork_state.locked_fields =
      cvector_create(sizeof(struct clogger *), NULL);
  if (!_clog_atfork_state.locked_fields)
    fatal_err("clog slot table: failed to allocate atfork scratch vector");
  _clog_atfork_state.closing_slots =
      cvector_create(sizeof(clog_atfork_closing_t), NULL);
  if (!_clog_atfork_state.closing_slots)
    fatal_err(
        "clog slot table: failed to allocate atfork closing-slots vector");
  at_fork(_clog_atfork_prepare, _clog_atfork_parent, _clog_atfork_child);
#endif
}

/* Not declared in clogger.h (not part of the public API): a narrow,
 * deliberate escape hatch, mirroring cthreadcomm.c's own _cthreadcomm_
 * ensure_atfork_registered_before_caller() and cthreadpool.c's own
 * _ctpool_ensure_atfork_registered_before_caller() exactly, for a dependent
 * module (chttpserver.c) that must guarantee this module's own at_fork()
 * triple is registered BEFORE its own, so that pthread_atfork's LIFO
 * prepare-handler ordering makes the CALLER's own prepare handler run FIRST
 * at every future fork() (i.e. before this module's own prepare handler,
 * _clog_atfork_prepare, ever gets a chance to lock clog_slot_table.rwlock or
 * any live clog_shared_t's own mutex/fields_mutex).
 *
 * chttpserver.c holds its own srv_engine_bundler.mutex across a call into
 * this module twice (_SRV_ENGINE_LOG's own log_info/log_warn/... call, and
 * _engine_acquire's own direct clog_open_fd_mp call for the engine's
 * fallback logger), so without this, whichever of the two modules happens to
 * be used FIRST by the embedding application determines the real fork()-time
 * lock order purely by accident: a caller that resolves a chttpsvr handle
 * (even an invalid one, which every handle-resolving public chttpsvr_*
 * function tolerates and reports as a plain error rather than requiring the
 * caller to guarantee validity first) before this process has ever used clog
 * for anything else registers chttpserver's own at_fork() triple with no
 * forced clogger registration first; a later, independent first use of clog
 * then registers this module's own at_fork() AFTER chttpserver's, inverting
 * the required order and reopening the identical AB-BA fork()-deadlock shape
 * already found and fixed once for chttpserver/cthreadcomm/cthreadpool (see
 * _chttpsvr_slot_table_init_globals's own doc comment in chttpserver.c for
 * that account). A caller that never uses this function is entirely
 * unaffected: this module's own lazy, call_once-guarded registration happens
 * exactly as it always has, whenever a clog handle is first created on its
 * own. */
void _clog_ensure_atfork_registered_before_caller(void) {
  call_once(clog_slot_table.once, _clog_slot_table_init_globals);
}

/*
 * Resolve h, pinning the result against a concurrent clog_close(). Returns
 * NULL if h == 0, out of range, or references a free/wrong-generation slot.
 * Caller must call _clog_resolve_unpin() exactly once on every path once
 * resolution succeeded. Takes the table's READ lock only; concurrent
 * resolvers never serialise against each other, only against a concurrent
 * _clog_handle_acquire/clog_close (both take the WRITE lock).
 */
static struct clogger *_clog_resolve(clog h) {
  call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  rw_lock_rdlock(clog_slot_table.rwlock);
  struct clogger *raw = NULL;
  if (idx < cvector_elem_count(clog_slot_table.slots)) {
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  rw_lock_unlock(clog_slot_table.rwlock);
  return raw;
}

/*
 * Lock-free, deliberately, and safely; NOT the same bare-atomic-decrement
 * design _chttpsvr_resolve_unpin/_chttpcli_resolve_unpin explicitly document
 * as having caused a real use-after-free when they themselves once tried
 * it: an earlier lock-free version of THEIR decrement could bring their own
 * pending_resolve_count to 0 before their own function acquired their own
 * lock to broadcast a condvar-waiting destroyer, letting that destroyer
 * free the object before the delayed lock/broadcast call ever ran.
 *
 * This function's body is, and must remain, exactly the one
 * atomic_fetch_sub statement below, with no lock acquisition and no
 * broadcast afterward, because clog_close() never sleeps on a condvar
 * waiting for this function to wake it; it polls instead (see
 * clog_close()'s own step 3), so there is no wakeup left to deliver and
 * therefore nothing left for this function to do once the decrement
 * completes. Do NOT add a lock/broadcast/any other touch of raw to this
 * function later "to be safe"; doing so would reintroduce the exact
 * touch-after-decrement window the cited historical bug came from.
 */
static void _clog_resolve_unpin(struct clogger *raw) {
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
}

/*
 * Mints a fresh handle for a fully-constructed lg. Called once, from the
 * constructor path (after lg/sh are otherwise completely built) and from
 * clog_derive(). Returns CLOG_INVALID on slot-table OOM (ordinary, not
 * fatal). Also registers lg->shared into clog_slot_table.live_shareds (see
 * "Fork safety" below), deduplicating against an already-registered entry;
 * both a brand-new root construction and a clog_derive()-minted sibling
 * handle call this same function, and only the FIRST one to reference a
 * given `shared` should actually add it.
 *
 * Mirrors _chttpsvr_handle_slot_acquire (src/chttpserver.c) closely:
 * index/generation encode (idx << 32) | generation, and generation wraps
 * around skipping the value 0 (0 is reserved to mean "never yet used" /
 * distinguish CLOG_INVALID). One addition beyond that mirrored shape: every
 * acquire resets slot->freed = false unconditionally (a slot popped off
 * free_indices was left with freed == true by its previous occupant's own
 * close; omitting this reset would make a handle born from a recycled slot
 * silently inherit freed == true from an unrelated, already-gone
 * predecessor, and _clog_atfork_prepare's fields_mutex walk would then
 * never protect it at all).
 */
static clog _clog_handle_acquire(struct clogger *lg) {
  call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  rw_lock_wrlock(clog_slot_table.rwlock);

  /* Compiles away entirely (the non-test build's own helper is a bare
   * `return false;`) outside RUNNING_UNIT_TESTS, so this costs nothing on
   * the hot handle-acquisition path in a real build. */
  bool force_fresh_slot_reg_failure =
      _clog_test_consume_forced_fresh_slot_reg_failure();

  uint32_t idx;
  clog_slot_t *slot;
  if (!force_fresh_slot_reg_failure &&
      cvector_elem_count(clog_slot_table.free_indices) > 0) {
    cvector_pop_back(clog_slot_table.free_indices, &idx);
    slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
  } else {
    clog_slot_t fresh = {0};
    if (cvector_push_back(clog_slot_table.slots, &fresh) != ccol_success) {
      rw_lock_unlock(clog_slot_table.rwlock);
      return CLOG_INVALID;
    }
    idx = (uint32_t)cvector_elem_count(clog_slot_table.slots) - 1;
    slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
  }

  /* Register lg->shared into live_shareds (the exact set
   * _clog_atfork_prepare() walks to decide which shared->mutex objects to
   * lock before a fork()) BEFORE this slot is actually mutated/handed out,
   * so a registration failure can be rolled back by simply returning the
   * index untouched. Minting a handle whose shared object failed to
   * register would let it silently escape _clog_atfork_prepare()'s own
   * protection: a fork() racing an in-progress write/rotation on that exact
   * logger could then leave the child with a permanently locked
   * shared->mutex, the exact class of hang this file's fork-safety
   * machinery exists to prevent. */
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
  /* force_fresh_slot_reg_failure forces this outcome unconditionally,
   * regardless of already_registered: for clog_open_fd_mp()/
   * clog_open_file_mp(), lg->shared is always fresh, so already_registered is
   * always false there and this makes no difference either way; but for
   * clog_derive(), lg->shared is the PARENT's already-registered shared
   * object (guaranteed already present in live_shareds for as long as parent
   * itself is a live, pinned handle), so already_registered is always true
   * there, and without this unconditional override the forced-failure hook
   * could never actually fire for a clog_derive() call at all; silently
   * consuming itself (the hook auto-disarms on every call, see
   * _clog_test_consume_forced_fresh_slot_reg_failure() above) while still
   * minting a valid handle, contradicting this hook's own documented promise
   * to work "alike" across all three call sites. */
  bool live_shareds_registration_failed = force_fresh_slot_reg_failure;
  if (!live_shareds_registration_failed && !already_registered) {
    live_shareds_registration_failed =
        cvector_push_back(clog_slot_table.live_shareds, &lg->shared) !=
        ccol_success;
  }
  if (live_shareds_registration_failed) {
    /* "Untouched" is only actually safe here for a slot popped off
     * free_indices (already freed == true, ptr == NULL from its previous
     * occupant's own clog_close()); NOT for a slot that just got here via
     * the "push a brand new slot" branch above, whose own fresh = {0}
     * initialization leaves freed == false with ptr == NULL. That specific
     * combination is exactly what _clog_atfork_prepare()'s own walk treats
     * as "this slot has a live ptr, lock its fields_mutex" (it only ever
     * skips a slot via `if (slot->freed) continue;`), so leaving it behind
     * uncorrected here would make a fork() landing while this now-"free"
     * index sits unused in free_indices dereference a NULL ptr via
     * mutex_lock(slot->ptr->fields_mutex) and crash. Explicitly restoring
     * both fields to the same "freed" shape a slot retired by clog_close()
     * itself always ends up in makes this rollback safe, regardless of
     * which of the two branches above produced `slot`. */
    slot->freed = true;
    slot->ptr = NULL;
    cvector_push_back(clog_slot_table.free_indices, &idx);
    rw_lock_unlock(clog_slot_table.rwlock);
    return CLOG_INVALID;
  }

  slot->generation++;
  if (slot->generation == 0) slot->generation++; /* skip the sentinel value */
  slot->ptr = lg;
  slot->in_use = true;
  slot->freed = false;

  clog h = ((clog)idx << 32) | (clog)slot->generation;

  rw_lock_unlock(clog_slot_table.rwlock);
  return h;
}

/* ========================================================================== */
/*                         FORK SAFETY                                       */
/* ========================================================================== */

#if FORK_SAFETY_REQUIRED
/*
 * fork() duplicates only the calling thread. Any lock some OTHER thread
 * happened to be holding at that exact instant (clog_slot_table.rwlock,
 * shared->mutex, fields_mutex) is inherited by the child in a permanently
 * locked state, since the thread that would unlock it does not exist there.
 * The standard, correct pthread_atfork idiom: _prepare acquires every such
 * lock (so fork() only proceeds once no thread is transiently holding one),
 * _parent releases them all once fork() returns in the parent, and _child
 * also releases them all (the forking thread's own logical execution
 * continues as the child's sole thread, so this is a normal, valid unlock,
 * not a "reset a lock owned by a dead thread" hack).
 *
 * shared->mutex is protected via clog_slot_table.live_shareds specifically
 * (not derived from walking in_use handles): a handle's own slot can
 * already show in_use == false while clog_close() is still actively
 * acquiring/holding shared->mutex a few steps later (see clog_close()'s own
 * numbered sequence), so an in_use-gated walk would miss protecting that
 * shared object's mutex during exactly that window. fields_mutex is
 * protected via the freed flag instead (see clog_slot_t.freed's own doc
 * comment) for the analogous reason: in_use goes false before
 * _logger_free() actually destroys fields_mutex, and a third thread's
 * already-in-flight, pin-holding operation (clog_set_field() and friends)
 * may still legitimately hold it during that same window.
 */
static void _clog_atfork_prepare(void) {
  /* Per this file's own standing pthread-wrapper rule: every function that
   * directly touches clog_slot_table.rwlock must guard it with this same
   * call_once, even though pthread_atfork() can only ever invoke this
   * function after _clog_slot_table_init_globals() has already registered
   * it (and therefore already run); relying on that call-graph reasoning
   * instead of guarding here unconditionally is exactly the class of gap
   * that has silently broken other modules in this codebase before, the
   * moment some future change adds a new, unanticipated call path. */
  call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  rw_lock_wrlock(clog_slot_table.rwlock);

  size_t ns = cvector_elem_count(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared_t *sh =
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
    mutex_lock(sh->mutex);
  }

  size_t n = cvector_elem_count(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, i);
    if (slot->freed) continue;
    mutex_lock(slot->ptr->fields_mutex);
    if (cvector_push_back(_clog_atfork_state.locked_fields, &slot->ptr) !=
        ccol_success) {
      /* Recording this lock failed (OOM); release it right back immediately
       * rather than leaving it locked with nothing in locked_fields to ever
       * unlock it again in _clog_atfork_release(); that would deadlock
       * this one logger's field operations permanently in both the parent
       * and the child. Skipping this single logger's fields_mutex
       * protection for this one fork() is the safe, bounded degradation:
       * fork() only ever duplicates the calling thread, so the window this
       * leaves open is the same kind of narrow, OOM-only race already
       * accepted for a `freed` slot (skipped above) or a shared object that
       * never made it into live_shareds at all. */
      mutex_unlock(slot->ptr->fields_mutex);
    }

    if (!slot->in_use) {
      /* clog_close() on this exact handle is suspended, on some OTHER
       * thread, between its own step 2 (in_use cleared) and step 4 (slot
       * retired, sh's reference released) right now; see clog_close()'s
       * own numbered steps. That thread does not exist in a freshly forked
       * child, so nothing would otherwise ever finish retiring this slot or
       * releasing this handle's share of sh->ref_count there, permanently
       * leaking both raw and (once every other handle for sh is eventually
       * closed too) sh itself in the child. Record it so
       * _clog_atfork_release() can finish this close on that vanished
       * thread's behalf, in the child only; the real closing thread is
       * still present and unaffected in the parent, and needs no help. A
       * failure to record here (OOM) is a bounded, same-shape degradation as
       * the locked_fields push failure just above: this one slot's leak in
       * a future child is accepted rather than risking anything in the
       * parent to avoid it. */
      clog_atfork_closing_t c = {.idx = (uint32_t)i, .raw = slot->ptr};
      cvector_push_back(_clog_atfork_state.closing_slots, &c);
    }
  }
}

/*
 * A real, empirically-confirmed correction to this module's own original
 * design reasoning: a plain pthread_rwlock_unlock() on the write lock,
 * called from the child's own sole thread, was assumed to be a valid
 * release of a lock that thread's own forking continuation had acquired in
 * the parent. Verified directly (gdb on a hung child process, stuck in
 * rw_lock_rdlock on this exact rwlock) that glibc's rwlock write-lock
 * tracks ownership by TID internally, and the child's post-fork thread has
 * a different TID than the parent's forking thread did; so the unlock
 * silently fails to release it, permanently hanging every subsequent
 * resolve in the child. Plain (default-type) mutexes do not exhibit this
 * (no TID tracking for the fast/normal mutex type this codebase uses
 * throughout, per common.h's own mutex_init()), so only the rwlock needs
 * the fix below, not sh->mutex/fields_mutex.
 *
 * The standard, well-established fix (used by e.g. glibc's own malloc
 * arena locks for this exact scenario) is to re-initialize the lock in the
 * child instead of unlocking it; safe specifically because the child has
 * exactly one thread and no one else can possibly be waiting on it, so
 * there is no other party for a fresh init to race.
 */
static void _clog_atfork_release(bool in_child) {
  /* See the identical guard (and its own doc comment) at the top of
   * _clog_atfork_prepare(): this function touches clog_slot_table.rwlock
   * too (further down, and unconditionally for every caller), and must
   * carry the same call_once guard for the same reason, rather than relying
   * on _clog_atfork_prepare() having already run it moments earlier for
   * this exact fork(). */
  call_once(clog_slot_table.once, _clog_slot_table_init_globals);
  size_t nf = cvector_elem_count(_clog_atfork_state.locked_fields);
  for (size_t i = nf; i-- > 0;) {
    struct clogger *lg =
        *(struct clogger **)cvector_at(_clog_atfork_state.locked_fields, i);
    if (in_child) {
      /* pending_resolve_count is pinned (lock-free atomic increment) by
       * _clog_resolve() for as long as SOME thread holds a resolved pointer
       * to this handle, e.g. any thread currently inside a call to one of
       * the log_ macros or one of the other public clog_ functions on it,
       * and clog_close() poll-waits on it reaching 0 with no upper bound
       * (see that field's own doc comment). fork() duplicates only the
       * calling thread, so a pin held by any OTHER thread at fork() time can
       * now never be released: that thread's own eventual
       * _clog_resolve_unpin() call, the only thing that would ever decrement
       * it, does not exist in this child at all. Left as inherited, this
       * makes clog_close() on that exact handle hang forever in the child
       * the very first time it is called; not a rare corner case, but the
       * ordinary result of forking while any other thread is mid-log-call on
       * a still-open logger (see e.g. fork_safety.concurrent_fork_during_
       * churn_does_not_hang's own scenario). A freshly forked child has
       * exactly one thread, so no genuinely still-in-flight resolver can
       * exist here except (in the deliberately unsupported case of calling
       * fork() itself from within such a call on this same handle, on this
       * same thread) the forking thread's own, which this reset would then
       * under-count; the same class of not-worth-supporting reentrancy
       * this file's other atfork handling already declines to chase (see
       * the sh->async_enabled/sh->pending_compress handling below, which
       * similarly assumes the forking thread is not itself mid-operation on
       * the shared target). */
      atomic_store(&lg->pending_resolve_count, 0);
    }
    mutex_unlock(lg->fields_mutex);
  }

  size_t ns = cvector_elem_count(clog_slot_table.live_shareds);
  for (size_t i = 0; i < ns; i++) {
    clog_shared_t *sh =
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
    if (in_child) {
      /* Any rotation whose gzip compression was in flight at fork() time
       * (sh->mutex deliberately released for the duration; see _rotate())
       * belongs to a thread that does not exist in this child at all (fork()
       * duplicates only the calling thread); that thread's own post-
       * compression cleanup, which unlinks its stack-allocated
       * clog_pending_compress_t node from this list, will now never run. The
       * node's path/gz_path strings remain valid, readable (frozen,
       * COW-duplicated stack) bytes, so left as-is this would not crash;
       * it would instead make _prune_rotated() treat those two exact
       * filenames as permanently "still being compressed" and exempt them
       * from deletion forever in this child, silently defeating
       * max_rotated_files for that one generation. No compression is
       * actually in flight in this child any more (the thread that would
       * ever finish or unlink it is gone), so the correct fix is to clear
       * the list outright, not merely accept the exemption as a leak. */
      sh->pending_compress = NULL;
    }
    if (in_child && sh->async_enabled) {
      /* The writer thread this shared target relied on does not exist in
       * the child at all (fork() duplicates only the calling thread), and
       * there is no safe way to reach into circular_queue/dynamic_queue's
       * own opaque internals to reset whatever lock state they were
       * inherited in. Rather than attempting that, every code path in this
       * file already gates ALL access to sh->q/sh->writer_thread/
       * sh->async_buf behind this one flag; flipping it here means
       * nothing in the child ever touches that (possibly lock-inconsistent)
       * queue/buffer/thread state again, for the rest of this process's
       * lifetime. clog_close() on this handle in the child correctly takes
       * the plain synchronous teardown path as a direct consequence (its
       * own "if (sh->async_enabled) _shared_async_teardown(sh);" check is
       * now false), never attempting to join a thread that was never
       * duplicated into this process or send a sentinel into a queue with
       * no reader. The queue/buffer's own backing memory is never freed in
       * the child (there is no thread left to hand that job to); an
       * accepted, inherent leak for the remaining lifetime of the child
       * process specifically, no different in kind from any other
       * kernel-level resource a forked child similarly abandons rather
       * than reclaims. */
      sh->async_enabled = false;
    }
    mutex_unlock(sh->mutex);
  }

  if (in_child) {
    /* Finish, on behalf of the thread that will never resume it in this
     * child, every clog_close() call that was suspended mid-teardown at
     * fork() time (see clog_atfork_closing_t's own doc comment and the
     * recording of these in _clog_atfork_prepare()). This mirrors
     * clog_close()'s own steps 3, 4, 5, and 6 in order, run here
     * sequentially rather than concurrently since this child has exactly
     * one thread and nothing else can be observing this state yet; no
     * lock is needed beyond the slot-table write lock this function's
     * caller (pthread_atfork's own machinery) already holds throughout this
     * entire prepare/parent/child sequence. Run only after the two loops
     * above so that, by the time a shared target might be torn down here,
     * its own async_enabled/pending_compress downgrade has already happened
     * (never attempting to join a writer thread or drain a queue that does
     * not exist in this child). */
    size_t nc = cvector_elem_count(_clog_atfork_state.closing_slots);
    for (size_t i = 0; i < nc; i++) {
      clog_atfork_closing_t *c = (clog_atfork_closing_t *)cvector_at(
          _clog_atfork_state.closing_slots, i);
      clog_slot_t *slot =
          (clog_slot_t *)cvector_at(clog_slot_table.slots, c->idx);
      /* Can only ever have been retired by this very loop (the real closing
       * thread never resumes in this child at all), so this can never
       * actually be true today; guarded anyway rather than assumed, per
       * this file's own standing "don't rely on call-graph reasoning alone"
       * discipline. */
      if (slot->freed) continue;

      struct clogger *raw = c->raw;
      clog_shared_t *sh = raw->shared;

      /* Step 3's own resolution: this handle's own pin can never be
       * released by a resolver that does not exist in this child either
       * (identical reasoning to the locked_fields loop's own pending_
       * resolve_count reset above; done again here, unconditionally,
       * since a slot recorded as "closing" is not guaranteed to also have
       * made it into locked_fields, e.g. under the same OOM degradation
       * documented on that push above). */
      atomic_store(&raw->pending_resolve_count, 0);

      /* Step 4: retire the slot. fields_mutex was already unlocked above
       * (unconditionally, by the locked_fields loop, or immediately inline
       * in _clog_atfork_prepare() if recording it there had itself failed)
       * before _logger_free() destroys it here. */
      _logger_free(raw);
      slot->freed = true;
      slot->ptr = NULL;
      slot->generation++;
      if (slot->generation == 0) slot->generation++; /* skip the sentinel */
      cvector_push_back(clog_slot_table.free_indices, &c->idx);

      /* Steps 5-6: release this handle's own share of sh's ref_count, and
       * tear sh down if this was the last one; exactly what the vanished
       * closing thread would have done, minus the async writer-thread
       * join/queue teardown, which is unnecessary here: the loop above has
       * already forced sh->async_enabled to false for this child (no
       * writer thread, and therefore no in-flight jobs to drain, ever
       * exists post-fork). */
      if (--sh->ref_count == 0) {
        size_t nls = cvector_elem_count(clog_slot_table.live_shareds);
        for (size_t j = 0; j < nls; j++) {
          clog_shared_t *seen =
              *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, j);
          if (seen == sh) {
            clog_shared_t *last = *(clog_shared_t **)cvector_at(
                clog_slot_table.live_shareds, nls - 1);
            *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, j) =
                last;
            cvector_pop_back(clog_slot_table.live_shareds, &last);
            break;
          }
        }
        _shared_close_owned_fd(sh);
        mutex_destroy(sh->mutex);
        _shared_free_partial(sh);
      }
    }
  }

  if (in_child) {
    rw_lock_init(clog_slot_table.rwlock);
  } else {
    rw_lock_unlock(clog_slot_table.rwlock);
  }
  cvector_reset(_clog_atfork_state.locked_fields);
  /* Unconditionally, not just in_child: the parent branch never processes
   * closing_slots (the real closing thread is still present and handles its
   * own teardown normally there), but prepare() populates this vector
   * before every fork() regardless of whether it turns out to be needed;
   * leaving stale entries here after a parent-side release would let the
   * NEXT fork()'s prepare() append on top of them, so a later fork() that
   * actually needs this list would wrongly "finish" long-since-retired (and
   * possibly reused-by-a-different-logger) slots. */
  cvector_reset(_clog_atfork_state.closing_slots);
}

static void _clog_atfork_parent(void) { _clog_atfork_release(false); }
static void _clog_atfork_child(void) { _clog_atfork_release(true); }
#endif /* FORK_SAFETY_REQUIRED */

/*
 * Defensive process-exit cleanup, mirroring chttpclient.c's own
 * _cleanup_default_client (the defensive variant, not chttpserver.c's
 * unconditional one): clog handles are expected to be plentiful and
 * independently owned across a process, rather than having one obvious
 * owner responsible for closing everything before exit, so this only frees
 * the slot table's own bookkeeping vectors if every slot has already been
 * closed; if any handle is still open at process exit, freeing them out
 * from under a destructor/atexit handler in some other translation unit
 * that runs later would risk a use-after-free, so this leaves them (and
 * whatever they still reference) for the OS to reclaim instead. */
__attribute__((destructor)) static void _cleanup_clog_slot_table(void) {
  if (!clog_slot_table.slots) return; /* never initialized; nothing to do */
  size_t n = cvector_elem_count(clog_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, i);
    if (slot->in_use) return;
  }
  cvector_destroy(clog_slot_table.slots);
  cvector_destroy(clog_slot_table.free_indices);
  cvector_destroy(clog_slot_table.live_shareds);
#if FORK_SAFETY_REQUIRED
  cvector_destroy(_clog_atfork_state.locked_fields);
  cvector_destroy(_clog_atfork_state.closing_slots);
#endif
  rw_lock_destroy(clog_slot_table.rwlock);
}

/* ========================================================================== */
/*                         BUFFER HELPERS                                     */
/* ========================================================================== */

static int _buf_init(clog_buf_t *b, const ccol_memmgmt_procs_t *m_procs) {
  b->m_procs = m_procs;
  b->data = _mem_alloc(m_procs, CLOG_BUF_INITIAL);
  if (!b->data) return -1;
  b->data[0] = '\0';
  b->len = 0;
  b->cap = CLOG_BUF_INITIAL;
  b->cap_limit = CLOG_BUF_MAX;
  b->oom = false;
  return 0;
}

/* Raises b's own growth ceiling above its CLOG_BUF_MAX default, never below
 * it; see clog_buf_t.cap_limit's own doc comment. Called once, by
 * _shared_async_init(), right after initializing clog_shared_t.async_buf, so
 * a caller-configured clog_async_cfg_t.flush_buffer_size larger than
 * CLOG_BUF_MAX genuinely gets that much room to batch into instead of being
 * silently capped at the smaller, single-record-oriented default. */
static void _buf_raise_cap_limit(clog_buf_t *b, size_t new_limit) {
  if (new_limit > b->cap_limit) b->cap_limit = new_limit;
}

static void _buf_reset(clog_buf_t *b) { b->len = 0; }

static void _buf_free(clog_buf_t *b) {
  _mem_free(b->m_procs, b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Lets a test deterministically force the NEXT _buf_append()/_buf_appendf()
 * call to report a genuine allocation failure, regardless of whether growth
 * was actually needed to satisfy it. Checked at the top of both functions
 * (not inside _buf_ensure(), which _buf_appendf() only calls when its own
 * first, fits-in-place attempt already failed) specifically so this can
 * force a failure even for a small, fixed-size append that would otherwise
 * never need to grow the buffer at all; e.g. _emit_backtrace_syslog_
 * lines()'s own "backtrace unavailable" marker, which this codebase's
 * actual CLOG_BUF_INITIAL value already comfortably fits without any growth
 * whatsoever, so a real allocator failure could otherwise never be observed
 * there. Auto-disarms itself the moment it fires, so only the one call a
 * test is targeting is affected.
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

/* Ensure at least `need` free bytes are available. Two distinct failure
 * causes both report -1 identically to the caller, but only the second one
 * sets b->oom: hitting b->cap_limit is an ordinary, size-related rejection
 * (an oversized record, or insufficient room left in a shared batch buffer),
 * never a sign of real memory pressure, so it must never be mistaken for one
 * by a caller further up (see clog_buf_t.oom's own doc comment). */
static int _buf_ensure(clog_buf_t *b, size_t need) {
  if (b->cap - b->len >= need) return 0;
  size_t new_cap = b->cap ? b->cap : CLOG_BUF_INITIAL;
  while (new_cap - b->len < need) {
    if (new_cap >= b->cap_limit) return -1;
    /* b->cap_limit may be a large, caller-configured value (see
     * _buf_raise_cap_limit()), unlike the fixed compile-time constant this
     * loop used to double against exclusively; guard the doubling itself
     * against size_t overflow rather than assuming new_cap*2 always fits.
     * Clamping straight to cap_limit here must NOT bypass the loop's own
     * sufficiency check the way an unconditional `break` would: cap_limit
     * being reachable at all does not guarantee it is actually >= what
     * `need` requires, and returning success (falsely reporting the buffer
     * as big enough) would let the caller memcpy/vsnprintf past the end of
     * the allocation. Looping back to the `while` condition instead means a
     * still-insufficient cap_limit is caught by the ordinary
     * `new_cap >= b->cap_limit` check on the very next iteration, exactly
     * like the ordinary (non-overflow) doubling path already relies on. */
    if (new_cap > SIZE_MAX / 2) {
      new_cap = b->cap_limit;
    } else {
      new_cap *= 2;
      if (new_cap > b->cap_limit) new_cap = b->cap_limit;
    }
  }
  char *p = _mem_realloc(b->m_procs, b->data, new_cap);
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
 * Fill esc (a >=5-byte buffer) with the backslash escape sequence for a
 * control byte c (c < 0x20 or c == 0x7f), and set *esc_len to its length (2
 * for \n, \r, \t; 4 for the generic \xXX form).  Shared by every value
 * escaper in this file (logfmt, RFC 5424 SD-PARAM-VALUE, and syslog MSG) so
 * a control byte is always neutralised the same way regardless of format.
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
 * Append a logfmt-safe value.  Values that contain spaces, '=', '"', '\\',
 * or control characters are double-quoted with backslash escaping.
 * Empty strings are emitted as "".
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
 * Classifies the UTF-8 multi-byte sequence starting at the non-ASCII lead
 * byte p[0] (0x80-0xff). Returns its total length (2, 3, or 4) if it is a
 * complete, well-formed sequence (no overlong encoding, no encoded
 * surrogate half (U+D800-U+DFFF), no codepoint beyond U+10FFFF, and every
 * continuation byte present and in range) or 0 if p[0] is itself an
 * invalid lead byte (a lone continuation byte 0x80-0xbf, an overlong 2-byte
 * lead 0xc0/0xc1, or a byte 0xf5-0xff that can never encode a valid
 * codepoint) or the sequence is truncated or otherwise malformed. Never
 * reads past p's own NUL terminator: each continuation byte is checked
 * against '\0' before being examined further, so a sequence truncated by
 * the end of the string is correctly reported as invalid rather than
 * reading beyond it.
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
    hi1 = (c0 == 0xed) ? 0x9f : 0xbf; /* reject the surrogate-half ed a0..bf
                                          range */
  } else {
    len = 4;
    lo1 = (c0 == 0xf0) ? 0x90 : 0x80; /* reject the overlong f0 80..8f range */
    hi1 = (c0 == 0xf4) ? 0x8f : 0xbf; /* reject the beyond-U+10FFFF f4 90..bf
                                          range */
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
 * Append the JSON-escaped content of s without surrounding quotes.
 * Escaping: '"' -> '\"', '\' -> '\\', '\n'->'\n', '\r'->'\r', '\t'->'\t',
 * other control chars -> '\uXXXX'. A byte or byte sequence that is not
 * well-formed UTF-8 (a lone continuation byte, an overlong or out-of-range
 * lead byte, an encoded surrogate half, or a truncated multi-byte sequence)
 * is replaced one invalid byte at a time with the Unicode replacement
 * character (U+FFFD), so a message or field value built from arbitrary or
 * binary data can never make this function emit invalid Unicode inside a
 * JSON string; RFC 8259 requires JSON text to be valid Unicode, and a
 * strict downstream parser or log aggregator is entitled to reject a
 * document that embeds raw non-UTF-8 bytes. A well-formed multi-byte
 * sequence is otherwise passed through verbatim, unescaped, exactly like any
 * other printable byte. s must not be NULL.
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
        esc_len = snprintf(esc, sizeof esc, "\\u%04x", c);
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
 * Append a comma-prefixed JSON key-value pair: ,"key":"value"
 * key must not be NULL.  val NULL -> ,"key":null
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
 * Append an RFC 5424 SD-PARAM-VALUE: any UTF-8 text with '"', '\', and ']'
 * escaped as '\"', '\\', '\]'.  Control characters (including '\n' and '\r',
 * which the formal SD-PARAM-VALUE grammar does not restrict but which would
 * otherwise split one syslog record across multiple lines) are also
 * backslash-escaped, mirroring the logfmt and JSON output formats. A NULL s
 * is rendered as the bareword "null", mirroring how _buf_append_lv() and
 * _buf_append_json_kv() each treat a NULL value in their own formats; the
 * caller already wraps this call in its own surrounding quotes, so "null"
 * here lands as an unquoted-looking word inside an otherwise-quoted
 * SD-PARAM-VALUE, exactly like logfmt's own unquoted `null` bareword.
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
    }
  }
  return 0;
}

/*
 * Fill name (a >=33-byte buffer) with an RFC 5424 SD-PARAM-NAME derived from
 * key: verbatim if key already fits within the 32-character limit;
 * otherwise the first 23 characters of key followed by '~' and an 8-hex-digit
 * FNV-1a hash of the FULL key (32 characters total).  A bare prefix
 * truncation would make any two distinct keys sharing a common 32-character
 * prefix collide into the identical SD-PARAM-NAME, which RFC 5424 forbids
 * within a single SD-ELEMENT; folding the whole key into the suffix makes
 * that collision require an actual hash collision, not just a shared prefix.
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
 * Append free-form text with control characters backslash-escaped, with no
 * quoting or other structural wrapping applied. Used for content that has no
 * formally defined escape syntax of its own but which must not be able to
 * split a record across multiple lines: RFC 5424 MSG content, and backtrace
 * frame text (from backtrace_symbols()) in the logfmt and syslog formats
 * (JSON embeds backtrace frames as ordinary JSON array elements instead, via
 * _buf_append_json_content(), which already has its own escaping). Every
 * control byte that could otherwise desynchronize the output stream (most
 * importantly '\n' and '\r') is neutralised the same way the logfmt and JSON
 * formats already neutralise it in their own message/value fields. s must
 * not be NULL.
 */
static int _buf_append_ctrl_escaped(clog_buf_t *b, const char *s) {
  const char *run = s;
  for (const char *p = s;; p++) {
    unsigned char c = (unsigned char)*p;
    if (*p == '\0') {
      if (p > run && _buf_append(b, run, (size_t)(p - run)) != 0) return -1;
      break;
    }
    if (c < 0x20 || c == 0x7f) {
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
 * Sanitize raw into out (a buffer of size outsz) for use as an RFC 5424
 * PRINTUSASCII field (APP-NAME or HOSTNAME both share the identical
 * "1*NNNPRINTUSASCII" grammar): keep only PRINTUSASCII bytes (0x21-0x7e),
 * dropping any other byte rather than stopping at the first one (so a single
 * disqualifying byte (a space, a control character, a non-ASCII byte)
 * anywhere in raw does not discard everything after it), up to outsz-1 bytes
 * of output. Falls back to "-" if raw yields no PRINTUSASCII bytes at all
 * (including an empty raw) and there is room for it (outsz >= 2).
 *
 * outsz == 0 is tolerated as a no-op (out is never touched, since there is no
 * byte in it to safely write even a NUL terminator to); every outsz >= 1 is
 * guaranteed to leave out NUL-terminated within the first outsz bytes, never
 * beyond them. The loop bound below is deliberately written as `i + 1 <
 * outsz` rather than the equivalent-looking `i < outsz - 1`: outsz is
 * size_t, so outsz - 1 underflows to SIZE_MAX for outsz == 0, which would
 * silently defeat the bound entirely and let the loop below write arbitrarily
 * far past a genuinely zero-sized out.
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
 * Return the platform-reported short program name, or NULL if the platform
 * has no such facility or reports an empty one. Shared by the two callers
 * below, which differ only in what they fall back to when this returns
 * NULL: the general "proc" field emitted on every log line falls back to
 * "unknown", while RFC 5424's own APP-NAME field falls back to "-" per its
 * own grammar's documented sentinel for "unavailable".
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

/* Return the process name for RFC 5424 APP-NAME, or "-" if unavailable. */
static const char *_syslog_appname(void) {
  const char *p = _raw_progname();
  return p ? p : "-";
}

/* Return the basename of the executable, or "unknown" if unavailable. */
static const char *_get_progname(void) {
  const char *p = _raw_progname();
  return p ? p : "unknown";
}

/* Return the OS-level thread ID for the calling thread. */
static pid_t _get_tid(void) {
#if defined(__linux__)
  return (pid_t)syscall(SYS_gettid);
#elif defined(__APPLE__)
  uint64_t tid64 = 0;
  get_thread_id_np(NULL, &tid64);
  return (pid_t)tid64;
#elif defined(__FreeBSD__)
  return (pid_t)pthread_getthreadid_np();
#else
  /* No portable way to obtain a genuine OS-level thread ID on this platform;
   * fall back to the pthread handle itself. This is still unique per thread
   * (so e.g. distinguishing log lines from different threads still works),
   * but unlike the branches above it is not comparable against OS-level
   * tools (ps -eLf, /proc/<pid>/task, top -H, ...). */
  return (pid_t)(uintptr_t)get_thread_id();
#endif
}

/* Fill buf with the name of the calling thread (at most bufsz-1 chars). */
static void _get_thread_name(char *buf, size_t bufsz) {
#if defined(__linux__)
  char name[16]; /* prctl writes at most 16 bytes including null */
  if (prctl(PR_GET_NAME, name) == 0 && name[0]) {
    snprintf(buf, bufsz, "%s", name);
    return;
  }
#elif defined(__APPLE__) || defined(__FreeBSD__)
  if (get_thread_name_np(get_thread_id(), buf, bufsz) == 0 && buf[0]) return;
#endif
  snprintf(buf, bufsz, "unknown");
}

/* ========================================================================== */
/*                         I/O HELPER                                         */
/* ========================================================================== */

/*
 * write() loop that retries on EINTR and partial writes, and (unlike a
 * plain retry loop) also waits for the fd to become writable again (via
 * poll()) on EAGAIN/EWOULDBLOCK rather than treating a transient "would
 * block" condition as a permanent failure. This matters because
 * clog_open_fd_mp() places no restriction on the caller-supplied fd's own
 * blocking mode (and the header's own documented CLOG_FMT_SYSLOG usage is a
 * UNIX datagram socket, exactly the kind of descriptor that can legitimately
 * return EAGAIN under load if its send buffer is momentarily full); without
 * this, a non-blocking fd would silently and permanently lose the remainder
 * of a record the instant the peer could not keep up, precisely when
 * logging visibility matters most. Blocking here (while the caller holds
 * shared->mutex) makes a non-blocking fd behave the same way a blocking one
 * already does when its peer cannot keep up; not a new risk, just parity
 * with the existing behavior for a plain blocking descriptor.
 *
 * Returns the number of bytes actually written, which may be less than len
 * if a genuinely unrecoverable error, or a "no progress" (w == 0) condition,
 * is hit partway through. Callers that track bytes_written for rotation
 * purposes must use this return value, not len, so that counter never
 * drifts ahead of the file's real on-disk size.
 */
static size_t _write_all(int fd, const char *data, size_t len) {
  size_t total = 0;
  while (len > 0) {
    ssize_t w = write(fd, data, len);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
        int pr = poll(&pfd, 1, -1);
        if (pr < 0 && errno == EINTR) continue;
        /* POLLERR/POLLHUP/POLLNVAL are reported by the kernel unconditionally,
         * regardless of the requested `events` mask, so pr > 0 alone does not
         * mean the fd is actually writable; only retry the write once
         * POLLOUT itself is confirmed set; otherwise fall through to the
         * genuinely-unrecoverable break below rather than retrying write()
         * against an fd that merely reported an error/hangup condition. */
        if (pr > 0 && (pfd.revents & POLLOUT)) continue;
      }
      break; /* genuinely unrecoverable; stop, reporting what was written */
    }
    if (w == 0) break; /* no progress (quota / fd limit); avoid spinning */
    data += (size_t)w;
    len -= (size_t)w;
    total += (size_t)w;
  }
  return total;
}

/* ========================================================================== */
/*                         TIMESTAMP                                          */
/* ========================================================================== */

/* Renders an already-captured timestamp. Factored out so a record can be
 * built (possibly well after the fact, e.g. by the async writer thread)
 * using the timestamp captured at SUBMISSION time rather than write time. */
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

/* Fill dirbuf with the directory component of path (no heap allocation). */
static void _path_dir(const char *path, char *dirbuf, size_t bufsz) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    strncpy(dirbuf, ".", bufsz - 1);
    dirbuf[bufsz - 1] = '\0';
    return;
  }
  size_t len = (size_t)(slash - path);
  if (len == 0) len = 1; /* root "/" */
  if (len >= bufsz) len = bufsz - 1;
  memcpy(dirbuf, path, len);
  dirbuf[len] = '\0';
}

/* Return pointer into path just after the last '/'. */
static const char *_path_base(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

/* ========================================================================== */
/*                         GZIP COMPRESSION (via zlib)                        */
/* ========================================================================== */

#define _GZ_BUF_SIZE 65536U

#ifdef RUNNING_UNIT_TESTS
/*
 * Test-only synchronization hooks, gated out of every non-test build.  Let a
 * test deterministically widen and observe the window between
 * _gzip_compress_file() creating its destination file on disk (gzopen()
 * returning) and finishing writing it (the exact window during which a
 * concurrent rotation's own _prune_rotated() pass must never be able to
 * delete that destination) instead of relying on real thread-scheduling
 * luck to land a racing prune inside such a normally sub-millisecond window.
 */
static _Atomic unsigned int _clog_test_pending_compress_delay_us = 0;
static _Atomic bool _clog_test_gz_dest_opened = false;

void clog_test_set_pending_compress_delay_us(unsigned int delay_us) {
  atomic_store(&_clog_test_gz_dest_opened, false);
  atomic_store(&_clog_test_pending_compress_delay_us, delay_us);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
  cdebuglog_write(
      "[DEBUG_ROTATE_TIMING] pid=%d tid=%d "
      "clog_test_set_pending_compress_delay_us(%u): armed, "
      "gz_dest_opened cleared\n",
      (int)getpid(), (int)_get_tid(), delay_us);
#endif
}

bool clog_test_gz_dest_opened(void) {
  bool opened = atomic_load(&_clog_test_gz_dest_opened);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
  if (opened)
    cdebuglog_write(
        "[DEBUG_ROTATE_TIMING] pid=%d tid=%d "
        "clog_test_gz_dest_opened() observed true\n",
        (int)getpid(), (int)_get_tid());
#endif
  return opened;
}

/*
 * Counts real invocations of _rotate() (i.e. every attempt that actually
 * reaches rename()/open(), not merely every write that would like to
 * rotate), so a test can verify a persistently failing rotation is retried
 * with a bounded backoff rather than reattempted on every single write.
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
 * Lets a test deterministically force _capture_backtrace() to report a
 * capture failure (as if the platform lacked backtrace support, or
 * backtrace_symbols() itself had failed to allocate) without depending on
 * either of those genuinely platform-/OOM-specific conditions.
 */
static _Atomic bool _clog_test_force_bt_capture_failure = false;

void clog_test_force_backtrace_capture_failure(bool force) {
  atomic_store(&_clog_test_force_bt_capture_failure, force);
}

/*
 * Lets a test deterministically force EVERY frame of a real (non-NULL syms)
 * backtrace to fail to append inside _emit_backtrace_syslog_lines(),
 * without depending on a genuine, sustained allocation failure persisting
 * across every one of that function's own small per-frame appends; a
 * condition this library's own buffer-sizing constants make impractical to
 * reproduce for real (each frame gets a freshly reset, already-adequately-
 * sized scratch buffer). Unlike clog_test_force_next_buf_ensure_failure()
 * (single-shot, auto-disarming), this stays armed across every frame in one
 * call until explicitly disarmed, mirroring
 * clog_test_force_backtrace_capture_failure()'s own persistent-until-
 * disarmed shape.
 */
static _Atomic bool _clog_test_force_all_syslog_bt_frames_fail = false;

void clog_test_force_all_syslog_backtrace_frames_failure(bool force) {
  atomic_store(&_clog_test_force_all_syslog_bt_frames_fail, force);
}

/*
 * Lets a test deterministically force _capture_backtrace() to report a
 * genuinely successful capture (syms non-NULL, from a real backtrace())
 * whose depth is clamped to CLOG_BT_INITIAL_FRAME; exercising the "capture
 * succeeded but found no frame beyond the two internal bookkeeping ones"
 * path the emitters must treat as a genuine, silent-but-accurate empty
 * backtrace, not a capture failure, without depending on a real call stack
 * ever being shallow enough to reach it for real.
 */
static _Atomic bool _clog_test_force_shallow_bt_depth = false;

void clog_test_force_shallow_backtrace_depth(bool force) {
  atomic_store(&_clog_test_force_shallow_bt_depth, force);
}
#endif

/*
 * Gzip-compress the file at src into dst (src + ".gz") using zlib.
 * On success: dst is a valid gzip file, src is unlinked; returns 0.
 * On failure: dst is removed if partially written, src is untouched; returns
 * -1.
 */
static int _gzip_compress_file(const char *src, const char *dst) {
  FILE *in = NULL;
  gzFile out = NULL;
  int rc = -1;
  bool dst_created = false; /* only unlink dst below if gzopen() actually
      created it; an earlier failure (src not even openable) must leave
      whatever, if anything, already exists at dst completely untouched */

  in = fopen(src, "rb");
  if (!in) goto done;

  /* Never truncate/overwrite something already sitting at dst: an unrelated
   * file (or a stale leftover) occupying this exact gzip-destination path
   * must be left completely alone, the same guarantee this function already
   * gives when src itself isn't even openable. gzopen()'s "wb" mode has no
   * O_EXCL-equivalent of its own, so this has to be a separate, explicit
   * check before ever calling it; otherwise a colliding pre-existing file
   * would be silently truncated the instant gzopen() succeeds, and unlinked
   * outright if the compression then failed partway through. */
  if (access(dst, F_OK) == 0) goto done;

  out = gzopen(dst, "wb");
  if (!out) goto done;
  dst_created = true;

#ifdef RUNNING_UNIT_TESTS
  {
    /* Capture the armed delay into a local before announcing that the
     * destination file now exists: a test spin-waiting on
     * clog_test_gz_dest_opened() may re-arm (or clear) the delay for a
     * *different*, later compression the instant it observes this one, and
     * that must never race this call's own already-in-progress read of it. */
    unsigned int delay = atomic_load(&_clog_test_pending_compress_delay_us);
    atomic_store(&_clog_test_gz_dest_opened, true);
#if defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE_TIMING] pid=%d tid=%d _gzip_compress_file(dst=%s) "
        "set gz_dest_opened=true, delay_us=%u\n",
        (int)getpid(), (int)_get_tid(), dst, delay);
#endif
    if (delay) usleep(delay);
  }
#endif

  unsigned char buf[_GZ_BUF_SIZE];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
    if (gzwrite(out, buf, (unsigned)n) != (int)n) goto done;
  }
  if (ferror(in)) goto done;

  if (gzclose(out) != Z_OK) {
    out = NULL;
    goto done;
  }
  out = NULL;
  rc = 0;

done:
  if (in) fclose(in);
  if (out) gzclose(out);
  if (rc != 0 && dst_created) unlink(dst); /* remove truncated output */
  if (rc == 0) unlink(src);                /* remove uncompressed original */
  return rc;
}

#ifdef RUNNING_UNIT_TESTS
bool clog_test_gzip_compress_file(const char *src, const char *dst) {
  return _gzip_compress_file(src, dst) == 0;
}
#endif

/* ========================================================================== */
/*                         LOG ROTATION                                       */
/* ========================================================================== */

static int _cmp_strptr(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Validate the portion of a candidate rotated-file name that follows the
 * mandatory 14-digit timestamp produced by CLOG_ROTATION_FMT.  A file this
 * logger actually produced has nothing there at all, exactly ".gz" (a
 * compressed rotation with no collision), exactly "_NNNN" (a same-second
 * collision suffix, always 4 zero-padded digits; see _rotate()), or
 * "_NNNN.gz" (a compressed collision).  Anything else (extra digits, a
 * unrelated trailing word, ...) merely happens to start with something that
 * looks like <base>.<14 digits> and must never be treated as a file this
 * logger owns: unlike a bare prefix/length check, this stops _prune_rotated()
 * from unlink()-ing an unrelated file a user (or another tool) placed in the
 * same directory with a coincidentally similar name.
 */
static bool _rotated_name_suffix_is_valid(const char *rest) {
  if (*rest == '\0') return true;
  if (strcmp(rest, ".gz") == 0) return true;
  if (rest[0] != '_') return false;
  for (int i = 1; i <= 4; i++)
    if (!isdigit((unsigned char)rest[i])) return false;
  const char *after = rest + 5;
  return *after == '\0' || strcmp(after, ".gz") == 0;
}

/*
 * Delete the oldest rotated files beyond max_keep.  Called with mutex held.
 * `pending` lists rotated files (if any) whose gzip compression a concurrent
 * _rotate() call currently has in flight; neither the uncompressed source
 * nor the compressed destination of such a pair is ever a candidate for
 * deletion here, since the source may not have been fully read yet and the
 * destination may not have been fully written yet; deleting either one out
 * from under the in-progress compression would lose that generation's log
 * data entirely instead of merely leaving it uncompressed.
 */
static void _prune_rotated(const char *file_path, int max_keep,
                           const ccol_memmgmt_procs_t *m_procs,
                           const clog_pending_compress_t *pending) {
  if (max_keep <= 0) return;

  char dir[PATH_MAX];
  _path_dir(file_path, dir, sizeof dir);

  const char *base = _path_base(file_path);
  size_t base_len = strlen(base);
  size_t file_path_len = strlen(file_path);

  DIR *d = opendir(dir);
  if (!d) return;

  char **matches = NULL;
  int mc = 0, cap = 0;
  struct dirent *e;

  while ((e = readdir(d)) != NULL) {
    const char *n = e->d_name;

    /* Match: <base>.<YYYYMMDDHHMMSS>[_NNNN][.gz], nothing else. */
    if (strncmp(n, base, base_len) != 0 || n[base_len] != '.') continue;
    const char *ts_start = n + base_len + 1;
    if (strlen(ts_start) < 14) continue;

    bool ts_ok = true;
    for (int i = 0; i < 14; i++) {
      if (!isdigit((unsigned char)ts_start[i])) {
        ts_ok = false;
        break;
      }
    }
    if (!ts_ok) continue;
    if (!_rotated_name_suffix_is_valid(ts_start + 14)) continue;

    /* Build this candidate's full path by appending its own <base>-relative
     * suffix (n + base_len, e.g. ".20260101120000.gz", always starting with
     * the '.' matched above) directly onto file_path itself; the exact
     * same construction _rotate() uses for `rotated`/`gz_path` (a literal
     * file_path prefix with a timestamp+suffix appended, never a separately
     * reconstructed directory). Reconstructing the prefix via dir/base
     * instead (dir + "/" + n) is NOT guaranteed to reproduce file_path's own
     * bytes: _path_dir() returns "." for a file_path with no '/' at all
     * (synthesizing a "./" prefix _rotate() never actually wrote), and
     * returns "/" for a root-level path like "/app.log" (producing a
     * doubled "//" once the separating slash below is added back). Either
     * mismatch would make the strcmp() comparisons below never match a
     * genuinely pending path/gz_path, silently defeating the in-flight-
     * compression protection those entries exist to provide. Appending
     * directly onto file_path guarantees a byte-identical match for any
     * file_path shape (bare filename, "./relative", root-level, or a
     * normal "dir/file" path) without needing to special-case any of them.
     */
    const char *suffix = n + base_len; /* starts with '.' */
    size_t suffix_len = strlen(suffix);
    char *full = _mem_alloc(m_procs, file_path_len + suffix_len + 1);
    if (!full) continue;
    memcpy(full, file_path, file_path_len);
    memcpy(full + file_path_len, suffix, suffix_len + 1); /* + NUL */

    bool is_pending = false;
    for (const clog_pending_compress_t *pc = pending; pc; pc = pc->next) {
      if (strcmp(pc->path, full) == 0 || strcmp(pc->gz_path, full) == 0) {
        is_pending = true;
        break;
      }
    }
    if (is_pending) {
      _mem_free(m_procs, full);
      continue;
    }

    if (mc >= cap) {
      int nc = cap ? cap * 2 : 16;
      char **nm = _mem_realloc(m_procs, matches, (size_t)nc * sizeof *matches);
      if (!nm) {
        _mem_free(m_procs, full);
        break;
      }
      matches = nm;
      cap = nc;
    }
    matches[mc++] = full;
  }
  closedir(d);

  if (mc > 1) qsort(matches, (size_t)mc, sizeof *matches, _cmp_strptr);

  /* Delete oldest entries that exceed the quota */
  int to_del = mc - max_keep;
  for (int i = 0; i < to_del; i++) unlink(matches[i]);

  for (int i = 0; i < mc; i++) _mem_free(m_procs, matches[i]);
  _mem_free(m_procs, matches);
}

/*
 * Returns true if `candidate` is not usable as a fresh rotation destination
 * name: either the name itself already exists on disk, or (when check_gz is
 * true, i.e. compress_rotated is enabled) its own compressed ".gz" form
 * already exists. The second check matters because _gzip_compress_file()
 * unlinks its uncompressed source the instant compression succeeds; so by
 * the time a LATER, same-second rotation runs its own collision check, an
 * earlier rotation's now-fully-compressed-and-deleted candidate name looks
 * completely free again via a bare access(candidate, F_OK) alone, even
 * though "<candidate>.gz" is still sitting on disk holding that earlier
 * rotation's real content. Reusing that name would then hand the earlier
 * rotation's own ".gz" destination straight back out to a second, unrelated
 * rotation.
 */
static bool _rotated_name_taken(const char *candidate, bool check_gz) {
  if (access(candidate, F_OK) == 0) return true;
  if (!check_gz) return false;
  /* Sized with headroom above PATH_MAX rather than reasoning precisely about
   * how much of _rotate()'s own CLOG_ROTATION_EXTRA slack is left at the
   * point this is called from; candidate is always a NUL-terminated
   * string that itself fit inside a PATH_MAX buffer, so clen < PATH_MAX,
   * and this buffer has ample room for clen + strlen(".gz") + 1 regardless. */
  char gz[PATH_MAX + 8];
  size_t clen = strlen(candidate);
  memcpy(gz, candidate, clen);
  memcpy(gz + clen, ".gz", 4); /* includes NUL */
  return access(gz, F_OK) == 0;
}

#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
/* Derives file_path's containing directory into out (bounded, NUL
 * terminated), shared by every _rotate() diagnostic call site below so the
 * dirname parsing (and its edge cases: root, no slash) is expressed once. */
static void _debug_dir_of(const char *file_path, char *out, size_t outsz) {
  size_t path_len = strlen(file_path);
  size_t dir_len = path_len;
  while (dir_len > 0 && file_path[dir_len - 1] != '/') dir_len--;
  if (dir_len > 1) dir_len--; /* drop the trailing slash, keep root as "/" */
  if (dir_len >= outsz) dir_len = outsz - 1;
  memcpy(out, file_path, dir_len);
  out[dir_len] = '\0';
}
#endif

/*
 * Rotate the current log file.  Must be called with shared->mutex held.
 *
 * Steps:
 *   1. Build a timestamped destination name, handling same-second collisions.
 *   2. Close the current fd.
 *   3. Rename the current file to the destination (atomic on POSIX).
 *   4. Prune old rotated files if max_rotated_files is set.
 *   5. Open a fresh log file.
 */
static int _rotate(clog_shared_t *sh) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&_clog_test_rotate_attempt_count, 1);
#endif

  if (!sh->file_path || sh->fd < 0) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE] pid=%d entry-guard early return: file_path=%s "
        "fd=%d\n",
        (int)getpid(), sh->file_path ? sh->file_path : "(null)", sh->fd);
#endif
    return 0;
  }

  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);

  size_t plen = strlen(sh->file_path);
  char rotated[PATH_MAX];

  if (plen + CLOG_ROTATION_FMT_LEN + CLOG_ROTATION_EXTRA + 1 > sizeof rotated) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE] pid=%d path too long: plen=%zu fmt_len=%d "
        "extra=%d bufsz=%zu\n",
        (int)getpid(), plen, (int)CLOG_ROTATION_FMT_LEN,
        (int)CLOG_ROTATION_EXTRA, sizeof rotated);
#endif
    return -1;
  }

  memcpy(rotated, sh->file_path, plen);
  size_t slen =
      strftime(rotated + plen, sizeof(rotated) - plen, CLOG_ROTATION_FMT, &tm);
  if (slen == 0) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write("[DEBUG_ROTATE] pid=%d strftime returned 0\n",
                    (int)getpid());
#endif
    return -1;
  }

  /* Resolve collisions: append _0001, _0002, ... until the name is free.
   * Zero-padded so alphabetical sort in _prune_rotated matches creation order.
   * check_gz mirrors this same check against the name's own compressed form
   * (see _rotated_name_taken()'s own doc comment) whenever compression is
   * enabled, so a same-second rotation can never reuse a base name whose
   * ".gz" file already exists just because its uncompressed source has
   * since been deleted by an earlier rotation's own successful compression.
   */
  bool check_gz = sh->rotation.compress_rotated;
  if (_rotated_name_taken(rotated, check_gz)) {
    size_t base = plen + slen;
    bool found = false;
    for (int n = 1; n < 10000; n++) {
      int w = snprintf(rotated + base, sizeof(rotated) - base, "_%04d", n);
      if (w < 0) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
        cdebuglog_write(
            "[DEBUG_ROTATE] pid=%d snprintf collision-suffix failed, "
            "errno=%d (%s)\n",
            (int)getpid(), errno, strerror(errno));
#endif
        return -1;
      }
      if (!_rotated_name_taken(rotated, check_gz)) {
        found = true;
        break;
      }
    }
    if (!found) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
      cdebuglog_write(
          "[DEBUG_ROTATE] pid=%d exhausted 10000 collision-suffix "
          "attempts for base name %.*s\n",
          (int)getpid(), (int)(plen + slen), rotated);
#endif
      return -1;
    }
  }

  /* Rename while the old fd is still open (POSIX allows renaming open files).
   * We only close the old fd once we have a replacement; this way a failed
   * open() leaves the logger alive; writes continue to the rotated file.
   * ENOENT means the file was deleted externally; treat it as a clean slate
   * (O_CREAT below will create a fresh file).  Any other rename error is a
   * hard failure; leave the logger writing to the still-open original fd. */
  int rename_rv = rename(sh->file_path, rotated);
  if (rename_rv != 0 && errno != ENOENT) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    /* Capture errno before any diagnostic call below (lstat, etc.) has a
     * chance to clobber it. */
    int rename_errno = errno;
    /* rename() needs write+execute on the directory containing both
     * oldpath and newpath (the same directory here); lstat that directory
     * (derived from sh->file_path, not `rotated`, since both live in it)
     * plus the source file itself, alongside the calling thread's own
     * effective ids, to see whether the kernel's view of the permissions
     * actually differs from what the test expects, rather than guessing.
     * ino is included so it can be directly compared against the
     * DEBUG_ROTATE_PRE line just above and DEBUG_MKTMPDIR at directory
     * creation: a changed inode across those means the directory was
     * removed and recreated, not merely had its mode changed. */
    char dir_buf[PATH_MAX];
    _debug_dir_of(sh->file_path, dir_buf, sizeof dir_buf);

    struct stat dir_st, src_st;
    int dir_stat_rv = lstat(dir_buf[0] ? dir_buf : ".", &dir_st);
    int src_stat_rv = lstat(sh->file_path, &src_st);
    cdebuglog_write(
        "[DEBUG_ROTATE] pid=%d tid=%d rename(%s -> %s) failed, errno=%d "
        "(%s)\n",
        (int)getpid(), (int)_get_tid(), sh->file_path, rotated, rename_errno,
        strerror(rename_errno));
    cdebuglog_write(
        "[DEBUG_ROTATE] pid=%d euid=%d egid=%d dir=%s dir_stat_rv=%d "
        "dir_mode=%o dir_ino=%llu dir_uid=%d dir_gid=%d\n",
        (int)getpid(), (int)geteuid(), (int)getegid(), dir_buf, dir_stat_rv,
        dir_stat_rv == 0 ? (unsigned int)(dir_st.st_mode & 07777) : 0u,
        dir_stat_rv == 0 ? (unsigned long long)dir_st.st_ino : 0ull,
        dir_stat_rv == 0 ? (int)dir_st.st_uid : -1,
        dir_stat_rv == 0 ? (int)dir_st.st_gid : -1);
    cdebuglog_write(
        "[DEBUG_ROTATE] pid=%d src_stat_rv=%d src_mode=%o src_uid=%d "
        "src_gid=%d\n",
        (int)getpid(), src_stat_rv,
        src_stat_rv == 0 ? (unsigned int)(src_st.st_mode & 07777) : 0u,
        src_stat_rv == 0 ? (int)src_st.st_uid : -1,
        src_stat_rv == 0 ? (int)src_st.st_gid : -1);
#endif
    return -1;
  }
  /* `rotated` only actually exists on disk when the rename above genuinely
   * succeeded; on the ENOENT "clean slate" path there is nothing at that
   * path to prune or compress. */
  bool did_rename = (rename_rv == 0);

  int new_fd =
      open(sh->file_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (new_fd < 0) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE] pid=%d open(%s, O_CREAT) failed, errno=%d (%s), "
        "did_rename=%d\n",
        (int)getpid(), sh->file_path, errno, strerror(errno), (int)did_rename);
#endif
    /* Recovery: restore the original path so the still-open fd remains useful.
     */
    (void)rename(rotated, sh->file_path);
    return -1;
  }

  close(sh->fd);
  sh->fd = new_fd;
  sh->bytes_written = 0;
  sh->last_rotation = now;

  /* Prune only once the rotation has definitively succeeded (the new live
   * file is open); pruning before this point would let a subsequently
   * failed rotation (open() failing above) permanently delete older rotated
   * files for an attempt that ends up being rolled back.  Any file another,
   * concurrent _rotate() call is still busy compressing (sh->pending_compress)
   * is excluded from deletion, so this prune pass can never race that other
   * call's own not-yet-finished read of it. */
  if (sh->rotation.max_rotated_files > 0) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    struct timespec _prune_t0, _prune_t1;
    clock_gettime(CLOCK_MONOTONIC, &_prune_t0);
#endif
    _prune_rotated(sh->file_path, sh->rotation.max_rotated_files, sh->m_procs,
                   sh->pending_compress);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    clock_gettime(CLOCK_MONOTONIC, &_prune_t1);
    double _prune_ms = (_prune_t1.tv_sec - _prune_t0.tv_sec) * 1000.0 +
                       (_prune_t1.tv_nsec - _prune_t0.tv_nsec) / 1e6;
    cdebuglog_write("[DEBUG_ROTATE_TIMING] pid=%d _prune_rotated took %.3fms\n",
                    (int)getpid(), _prune_ms);
#endif
  }

  /*
   * Compress the rotated file outside the mutex so log writers are not stalled
   * during what can be a slow I/O operation.  The critical shared state (fd,
   * bytes_written, last_rotation) is already committed above; releasing the
   * mutex here is safe.
   *
   * Both `rotated` (the source) and `gz_path` (the destination) are published
   * into sh->pending_compress (a stack-allocated node, safe since this
   * function's frame outlives every access to it, all of which happen only
   * while sh->mutex is held) before the mutex is released, and removed again
   * immediately after it is re-acquired.  This is what prevents a concurrent
   * rotation's own _prune_rotated() call, running during this exact window,
   * from deleting either one before this call has finished reading the
   * source and writing the destination: without this, that rotation's
   * generation of log data could be lost outright (source AND destination
   * both gone) rather than merely left uncompressed.
   *
   * did_rename gates this: on the ENOENT "clean slate" path above, `rotated`
   * was never actually created, so there is nothing here for
   * _gzip_compress_file() to read; skip it rather than attempting (and
   * harmlessly failing) a compression of a file that was never produced.
   */
  if (did_rename && sh->rotation.compress_rotated) {
    char gz_path[PATH_MAX];
    size_t rlen = strlen(rotated);
    if (rlen + 3 < sizeof gz_path) {
      memcpy(gz_path, rotated, rlen);
      memcpy(gz_path + rlen, ".gz", 4); /* includes NUL */

      clog_pending_compress_t node = {
          .path = rotated, .gz_path = gz_path, .next = sh->pending_compress};
      sh->pending_compress = &node;

      mutex_unlock(sh->mutex);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
      struct timespec _gz_t0, _gz_t1;
      clock_gettime(CLOCK_MONOTONIC, &_gz_t0);
      cdebuglog_write(
          "[DEBUG_ROTATE_TIMING] pid=%d tid=%d _gzip_compress_file(%s) "
          "START\n",
          (int)getpid(), (int)_get_tid(), rotated);
#endif
      _gzip_compress_file(rotated, gz_path);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
      clock_gettime(CLOCK_MONOTONIC, &_gz_t1);
      double _gz_ms = (_gz_t1.tv_sec - _gz_t0.tv_sec) * 1000.0 +
                      (_gz_t1.tv_nsec - _gz_t0.tv_nsec) / 1e6;
      cdebuglog_write(
          "[DEBUG_ROTATE_TIMING] pid=%d tid=%d _gzip_compress_file(%s) DONE, "
          "took %.3fms\n",
          (int)getpid(), (int)_get_tid(), rotated, _gz_ms);
      struct timespec _relock_t0, _relock_t1;
      clock_gettime(CLOCK_MONOTONIC, &_relock_t0);
#endif
      mutex_lock(sh->mutex);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
      clock_gettime(CLOCK_MONOTONIC, &_relock_t1);
      double _relock_ms = (_relock_t1.tv_sec - _relock_t0.tv_sec) * 1000.0 +
                          (_relock_t1.tv_nsec - _relock_t0.tv_nsec) / 1e6;
      cdebuglog_write(
          "[DEBUG_ROTATE_TIMING] pid=%d re-acquiring sh->mutex after "
          "compress took %.3fms\n",
          (int)getpid(), _relock_ms);
#endif

      /* Unlink `node` from the list. A concurrent rotation may have pushed
       * further nodes onto the head while we were unlocked, so `node` is not
       * necessarily still the head; search for it instead of assuming so. */
      clog_pending_compress_t **link = &sh->pending_compress;
      while (*link && *link != &node) link = &(*link)->next;
      if (*link == &node) *link = node.next;
    }
  }

  return 0;
}

/*
 * Pre-write, time-based rotation check: if time rotation is enabled and the
 * interval has elapsed, rotate now (subject to the retry backoff). Shared by
 * every code path that is about to write real record bytes to sh->fd, so
 * time-based rotation can never be silently skipped by one such path while
 * every other one still runs it. Must be called with sh->mutex held.
 */
static void _clog_time_rotate_if_due(clog_shared_t *sh) {
  if (!(sh->rotation_enabled && sh->rotation.time_rotation_enabled)) return;
  time_t now = time(NULL);
  if (now - sh->last_rotation >= sh->rotation.rotation_interval_secs &&
      now >= sh->rotate_retry_after) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE_DECISION] pid=%d tid=%d src=time path=%s now=%lld "
        "last_rotation=%lld interval=%lld retry_after=%lld\n",
        (int)getpid(), (int)_get_tid(), sh->file_path ? sh->file_path : "(fd)",
        (long long)now, (long long)sh->last_rotation,
        (long long)sh->rotation.rotation_interval_secs,
        (long long)sh->rotate_retry_after);
#endif
    int rrv = _rotate(sh);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE_DECISION] pid=%d tid=%d src=time _rotate() "
        "returned %d\n",
        (int)getpid(), (int)_get_tid(), rrv);
#endif
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

/*
 * Post-write, size-based rotation check: if size rotation is enabled and
 * bytes_written has crossed max_file_size, rotate now (subject to the retry
 * backoff). Shared for the same reason _clog_time_rotate_if_due() is. Must
 * be called with sh->mutex held, after sh->bytes_written already reflects
 * the write that just happened.
 */
static void _clog_size_rotate_if_due(clog_shared_t *sh) {
  if (!(sh->rotation_enabled && sh->rotation.size_rotation_enabled &&
        sh->bytes_written >= sh->rotation.max_file_size))
    return;
  time_t now = time(NULL);
  if (now >= sh->rotate_retry_after) {
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE_DECISION] pid=%d tid=%d src=size path=%s "
        "bytes_written=%lld max_file_size=%lld now=%lld retry_after=%lld\n",
        (int)getpid(), (int)_get_tid(), sh->file_path ? sh->file_path : "(fd)",
        (long long)sh->bytes_written, (long long)sh->rotation.max_file_size,
        (long long)now, (long long)sh->rotate_retry_after);
#endif
    int rrv = _rotate(sh);
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)
    cdebuglog_write(
        "[DEBUG_ROTATE_DECISION] pid=%d tid=%d src=size _rotate() "
        "returned %d\n",
        (int)getpid(), (int)_get_tid(), rrv);
#endif
    if (rrv != 0) sh->rotate_retry_after = now + CLOG_ROTATE_RETRY_BACKOFF_SECS;
  }
}

/* ========================================================================== */
/*                         BACKTRACE                                          */
/* ========================================================================== */

/* Frame 0 = _capture_backtrace, frame 1 = _clog_write, frame 2 = caller
 * (the first frame shown to the user); shared by every backtrace-emitting
 * format below. */
#define CLOG_BT_INITIAL_FRAME 2

/*
 * Captures the calling thread's own backtrace. noinline, and called
 * directly from _clog_write's own frame on every path (sync, async, and
 * FATAL alike), never from a helper one level deeper; CLOG_BT_INITIAL_FRAME
 * above is only correct as long as that stays true, since capturing it from
 * inside e.g. the async submission path would silently show that function
 * itself as if it were the caller. Returns false (leaving *out_syms/
 * *out_depth untouched) if backtrace support is unavailable on this
 * platform or backtrace_symbols() itself failed to allocate; callers must
 * treat that identically to "no frames".
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
  /* See clog_test_force_shallow_backtrace_depth()'s own doc comment: syms
   * stays a real, non-NULL capture; only the reported depth is clamped, so
   * every emitter sees exactly the same "genuinely succeeded, nothing past
   * the initial two frames" shape a real shallow call stack would produce. */
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
 * Fixed marker appended in place of the usual "\t#N ..." continuation lines
 * whenever the real backtrace could not be captured at all (unsupported
 * platform, or backtrace_symbols() itself failing to allocate). Without
 * this, a NULL syms was a silent no-op here, so a reader of a logfmt record
 * with with_backtrace requested had no way to tell "the backtrace was
 * omitted" apart from "no backtrace was ever requested"; unlike JSON,
 * which already carries this same distinction via _BT_JSON_ERROR_MARKER.
 */
static const char _BT_LINE_ERROR_MARKER[] = "\t#error backtrace unavailable\n";

/*
 * Append backtrace frames as tab-indented continuation lines into out
 * (never writes anything itself). A NULL syms (backtrace unavailable, or
 * _capture_backtrace() itself failed) appends the single fixed
 * _BT_LINE_ERROR_MARKER line instead, so the omission is never silent.
 *
 * Returns true if the omission of a requested backtrace is guaranteed to be
 * visible in `out`; either because at least one real frame line was
 * appended (a longer symbol name that didn't fit is silently skipped in
 * favour of a later, shorter one that does, rather than giving up on the
 * whole backtrace the moment the first frame doesn't fit; a frame skipped
 * this way carries no marker of its own, matching this function's
 * long-standing per-frame-best-effort contract), because depth <=
 * CLOG_BT_INITIAL_FRAME means capture genuinely succeeded but found no frame
 * beyond the initial bookkeeping two (nothing to append, and nothing to
 * report missing; mirroring _emit_backtrace_json()'s identical
 * array_content_is_accurate distinction, so a genuinely shallow-but-real
 * capture is never misreported as an unavailable one the way it would be if
 * this case fell through into "not a single frame fit"), or because real
 * frames existed but not a single one fit and the fixed-size
 * _BT_LINE_ERROR_MARKER was appended in their place instead. Returns false
 * only when `out` had essentially no room left at all (not even for the
 * ~30-byte marker), a residual, expected-unreachable-in-practice case given
 * this file's own buffer-sizing constants (the caller already has plenty of
 * room for the primary record this call's own record-start position sits
 * after; see _clog_build_record()'s own call site for how this vanishingly
 * narrow case is handled).
 */
static bool _emit_backtrace_lines(clog_buf_t *out, char *const *syms,
                                  int depth) {
  if (!syms)
    return _buf_append(out, _BT_LINE_ERROR_MARKER,
                       sizeof _BT_LINE_ERROR_MARKER - 1) == 0;

  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    size_t entry_start = out->len;
    /* syms[i] (from backtrace_symbols()) is untrusted, arbitrary text as far
     * as this format's own line-based framing is concerned; route it through
     * the same control-character escaper every other dynamic field in this
     * format uses, so an unusual symbol name (an embedded newline, in
     * particular) can never desynchronize the log stream the way an
     * unescaped one could. If this frame's line can't be built (buffer-growth
     * allocation failure), roll back to entry_start and skip it rather than
     * leave a truncated, newline-less fragment for the next frame's own text
     * to be appended directly onto. */
    bool ok = _buf_appendf(out, "\t#%d ", i - CLOG_BT_INITIAL_FRAME) == 0;
    ok = ok && _buf_append_ctrl_escaped(out, syms[i]) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
    if (!ok) {
      out->len = entry_start;
      continue;
    }
    any_frame_written = true;
  }
  /* depth <= CLOG_BT_INITIAL_FRAME means there was never any frame beyond
   * the initial two to begin with, so writing nothing here is a genuine,
   * accurate (if silent) representation of a real, successful capture
   * (not a truncation), exactly mirroring _emit_backtrace_json()'s own
   * array_content_is_accurate check for the identical case. */
  if (any_frame_written || depth <= CLOG_BT_INITIAL_FRAME) return true;

  /* Real frames existed but not one single frame fit (every frame's own
   * line, individually, needed more room than was left); fall back to the
   * same small, fixed-size marker the syms == NULL branch above already
   * uses, so a real but entirely-unrepresented backtrace is never
   * indistinguishable from one that was never requested at all. */
  return _buf_append(out, _BT_LINE_ERROR_MARKER,
                     sizeof _BT_LINE_ERROR_MARKER - 1) == 0;
}

/*
 * Fixed-size marker appended in place of the "bt" array whenever the real
 * backtrace could not be embedded (backtrace_symbols() failure, or a buffer-
 * growth allocation failure while appending the array itself or one of its
 * frames' surrounding structure). Its whole point is to make the omission of
 * an explicitly-requested backtrace visible in the record rather than silent:
 * a caller reading an ERROR/ALERT/FATAL record with with_backtrace requested
 * must be able to tell "backtrace omitted" apart from "backtrace never
 * requested", and never see this record's own real ts/level/msg content
 * needlessly replaced by the generic oversized-record fallback just because
 * the (typically much larger) backtrace text didn't fit.
 */
static const char _BT_JSON_ERROR_MARKER[] = ",\"bt_error\":\"unavailable\"";

/*
 * Append backtrace frames as a JSON array: ,"bt":["#0 sym","#1 sym",...]
 * Called while the JSON object is still open (before the closing "}\n").
 * A NULL syms is treated as a capture failure, same as every other
 * backtrace emitter in this file.
 *
 * Returns true if the record can safely be closed as-is (either a complete,
 * possibly frame-truncated "bt" array was appended, or the fixed-size
 * _BT_JSON_ERROR_MARKER was appended in its place); returns false only if
 * even that small marker could not be appended, meaning the buffer has
 * essentially no room left at all and the caller must fall back to the
 * generic oversized-record placeholder instead.
 *
 * Mirrors _emit_backtrace_lines()'s own per-frame-best-effort contract in two
 * ways a naive port of the "append until something fails" shape does not get
 * for free: a frame that fails to fit is skipped via `continue`, not `break`,
 * so a later frame with shorter symbol text still gets its own chance to fit
 * rather than being given up on the instant an earlier one does not; and an
 * empty array is only ever closed as "]" when that emptiness is genuine (no
 * frames beyond the initial two were ever captured to begin with); if real
 * frames existed (depth > CLOG_BT_INITIAL_FRAME) but not one of them fit, an
 * empty "bt":[] would be byte-for-byte indistinguishable from that genuinely
 * shallow case, silently defeating this function's whole reason for
 * existing; the omission is surfaced via the same _BT_JSON_ERROR_MARKER used
 * for every other zero-frames-fit case in this file instead.
 */
static bool _emit_backtrace_json(clog_buf_t *b, char *const *syms, int depth) {
  if (!syms)
    return _buf_append(b, _BT_JSON_ERROR_MARKER,
                       sizeof _BT_JSON_ERROR_MARKER - 1) == 0;

  size_t bt_start = b->len;
  if (_buf_append(b, ",\"bt\":[", 7) != 0) {
    /* Nothing was written (bt_start == b->len still); surface the omission
     * instead of silently leaving the record without a "bt" key at all. */
    return _buf_append(b, _BT_JSON_ERROR_MARKER,
                       sizeof _BT_JSON_ERROR_MARKER - 1) == 0;
  }

  bool first = true;
  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    size_t entry_start = b->len;
    char prefix[20];
    int pl = snprintf(prefix, sizeof prefix, "#%d ", i - CLOG_BT_INITIAL_FRAME);
    if ((!first && _buf_append(b, ",", 1) != 0) ||
        _buf_append(b, "\"", 1) != 0 ||
        (pl > 0 && _buf_append(b, prefix, (size_t)pl) != 0) ||
        _buf_append_json_content(b, syms[i]) != 0 ||
        _buf_append(b, "\"", 1) != 0) {
      b->len = entry_start; /* roll back partial entry; try the next frame
          instead of giving up on the rest of the array */
      continue;
    }
    first = false;
    any_frame_written = true;
  }

  /* depth <= CLOG_BT_INITIAL_FRAME means there was never any frame beyond
   * the initial two to begin with, so an empty array here is genuine, not a
   * truncation; any_frame_written covers the case where real frames existed
   * and at least one of them fit. Either way the array is safe to close as
   * accurately representing what happened. */
  bool array_content_is_accurate =
      any_frame_written || depth <= CLOG_BT_INITIAL_FRAME;
  if (array_content_is_accurate && _buf_append(b, "]", 1) == 0) return true;

  /* Either appending "]" itself failed (essentially no room left at all), or
   * real frames existed but not one of them fit and closing an empty array
   * here would have been indistinguishable from a genuinely shallow
   * backtrace; either way, whatever partial content is in `b` is discarded
   * (a "bt" key with no closing bracket, or an empty-but-misleading one,
   * would corrupt/misrepresent the record) and the omission is surfaced via
   * the marker instead. */
  b->len = bt_start;
  return _buf_append(b, _BT_JSON_ERROR_MARKER,
                     sizeof _BT_JSON_ERROR_MARKER - 1) == 0;
}

/*
 * Emit backtrace frames as separate RFC 5424 syslog messages, one per frame;
 * exempt from batching regardless of async mode, matching the one-
 * write()-per-UDP-datagram contract clog_syslog_facility_t documents. out is
 * used purely as scratch (reset before, and left reset after, each frame);
 * it may be a handle's own per-logger buffer (sync path) or the writer
 * thread's shared aggregation buffer (async path); either is safe, since
 * both are only ever touched while sh->mutex is held. Must be called with
 * sh->mutex held. A NULL syms (backtrace unavailable, or _capture_backtrace()
 * itself failed) emits a single fixed marker record instead of nothing, for
 * the same reason _emit_backtrace_lines() does for logfmt: without it, a
 * reader had no way to tell "the backtrace was omitted" apart from "no
 * backtrace was ever requested". A non-NULL syms with depth <=
 * CLOG_BT_INITIAL_FRAME (capture genuinely succeeded but found no frame
 * beyond the initial bookkeeping two) emits nothing at all, the same
 * genuinely-empty case _emit_backtrace_json()'s own array_content_is_
 * accurate check already recognizes; only real frames that all failed to
 * be written count as an omission worth marking.
 *
 * ts is the SAME captured timestamp used for the primary record these frames
 * continue (the caller's own `ts`/`job->ts`), not re-derived here: for an
 * async logger, the writer thread may not get around to building these
 * lines until well after the record was originally submitted, and every
 * other piece of per-record data in this file (fields, proc_val, the
 * symbols themselves) is already captured once at submission time rather
 * than re-read at write time for exactly this reason; a backtrace frame's
 * own TIMESTAMP field must agree with its primary record's, not show
 * whatever the wall clock happens to read when the writer thread finally
 * processes the job.
 */
/*
 * Builds and writes the single fixed "backtrace unavailable" marker record
 * used by _emit_backtrace_syslog_lines() below, both when capture itself
 * failed (syms == NULL) and when capture succeeded but not one single frame
 * could be written (every frame's own append failed, e.g. under sustained
 * allocation failure). Resets `out` first (matching every frame's own
 * "scratch, reset before use" contract) and leaves it reset afterward on
 * every path, per _emit_backtrace_syslog_lines()'s own documented "left
 * reset after" contract.
 */
static void _write_backtrace_unavailable_syslog_marker(
    clog_buf_t *out, clog_shared_t *sh, clog_level_t level,
    const struct timeval *ts) {
  int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
  const char *hostname = sh->syslog_hostname;
  const char *appname = sh->syslog_appname;
  const char *msgid = _LEVEL_STR[level];

  _buf_reset(out);
  bool ok = _buf_appendf(out, "<%d>1 ", pri) == 0;
  ok = ok && _buf_append_ts_at(out, ts) == 0;
  ok = ok && _buf_appendf(out,
                          " %s %s %d %s - \t#error backtrace "
                          "unavailable\n",
                          hostname, appname, (int)getpid(), msgid) == 0;
  if (ok) {
    size_t written = _write_all(sh->fd, out->data, out->len);
    if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  } else {
    /* Not even this small marker record fit into `out` (a genuine
     * allocation failure stacked on top of backtrace capture already
     * having failed, or on top of every individual frame having already
     * failed to append for the same reason); fall back to a minimal,
     * allocation-free write directly to the fd, mirroring
     * _clog_write_unrepresentable_record()'s own "cannot itself fail"
     * last-resort pattern (a fixed-size stack buffer built via snprintf,
     * never touching clog_buf_t/heap-allocation machinery at all), so this
     * double-failure edge case is never a fully silent record loss either.
     * Sized generously above the worst case (a full-length hostname +
     * appname + the surrounding literal text), so this snprintf itself
     * cannot truncate in practice. */
    struct tm tm;
    gmtime_r(&ts->tv_sec, &tm);
    char fallback[512];
    int n = snprintf(fallback, sizeof fallback,
                     "<%d>1 %04d-%02d-%02dT%02d:%02d:%02d.%06ldZ %s %s %d %s - "
                     "\t#error backtrace unavailable\n",
                     pri, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (long)ts->tv_usec,
                     hostname, appname, (int)getpid(), msgid);
    if (n > 0) {
      size_t len =
          (size_t)n < sizeof fallback ? (size_t)n : sizeof fallback - 1;
      size_t written = _write_all(sh->fd, fallback, len);
      if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
    }
  }
  /* Leave `out` reset on every return path, matching this function's own
   * documented "left reset after" contract: `out` may be sh->async_buf,
   * whose length is also the async writer thread's own signal (see
   * _writer_flush_now()) for "there is unflushed data pending"; leaving
   * the just-written marker's bytes sitting in `out->len` here would make
   * the next unrelated flush trigger (an idle flush_interval_ms timeout,
   * an explicit clog_flush(), or clog_close()'s own final drain) rewrite
   * those same already-transmitted bytes to sh->fd a second time. */
  _buf_reset(out);
}

static void _emit_backtrace_syslog_lines(clog_buf_t *out, clog_shared_t *sh,
                                         clog_level_t level,
                                         const struct timeval *ts,
                                         char *const *syms, int depth) {
  int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
  const char *hostname = sh->syslog_hostname;
  const char *appname = sh->syslog_appname;
  const char *msgid = _LEVEL_STR[level];

  if (!syms) {
    _write_backtrace_unavailable_syslog_marker(out, sh, level, ts);
    return;
  }

  bool any_frame_written = false;
  for (int i = CLOG_BT_INITIAL_FRAME; i < depth; i++) {
    _buf_reset(out);
    /* Each frame's line is built from several separate appends; a failure
     * partway through (e.g. the timestamp fitting but the rest not) would
     * otherwise leave a truncated, newline-less fragment in the buffer that
     * _write_all() would still write, merging into whatever gets written
     * next and desynchronizing the syslog stream. Track success across all
     * of them and skip the frame entirely rather than write a partial one.
     * syms[i] (from backtrace_symbols()) is routed through the same
     * control-character escaper the primary record's own MSG content uses,
     * so an unusual symbol name (an embedded newline, in particular) can
     * never desynchronize the syslog stream the way an unescaped one could. */
    bool ok = _buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok &&
         _buf_appendf(out, " %s %s %d %s - \t#%d ", hostname, appname,
                      (int)getpid(), msgid, i - CLOG_BT_INITIAL_FRAME) == 0;
    ok = ok && _buf_append_ctrl_escaped(out, syms[i]) == 0;
    ok = ok && _buf_append(out, "\n", 1) == 0;
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&_clog_test_force_all_syslog_bt_frames_fail)) ok = false;
#endif
    if (!ok) continue;
    size_t written = _write_all(sh->fd, out->data, out->len);
    if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
    any_frame_written = true;
  }
  /* See the identical note above: leave `out` reset after the LAST frame
   * too, not just before each one, since the loop only ever resets it going
   * INTO an iteration, never after the final write coming out of the loop. */
  _buf_reset(out);

  /* depth <= CLOG_BT_INITIAL_FRAME means there was never any frame beyond
   * the initial two to begin with, so writing no frame records here is a
   * genuine, accurate representation of a real, successful capture, not an
   * omission; mirroring _emit_backtrace_json()'s own array_content_is_
   * accurate check and _emit_backtrace_lines()'s identical carve-out for the
   * same case; only real frames (depth > CLOG_BT_INITIAL_FRAME) that ALL
   * failed to be written count as a genuine omission worth marking. */
  if (!any_frame_written && depth > CLOG_BT_INITIAL_FRAME) {
    /* Capture succeeded (syms is real), but every single frame's own append
     * failed (only reachable under genuine, sustained allocation failure;
     * each frame gets a freshly reset `out`, so ordinary capacity exhaustion
     * from unrelated prior content, unlike logfmt's shared batch buffer,
     * cannot be the cause here). Without this, a real but entirely
     * unwritten backtrace would be indistinguishable from one that was
     * never requested at all; the same guarantee capture failure (the
     * syms == NULL branch above) already provides. */
    _write_backtrace_unavailable_syslog_marker(out, sh, level, ts);
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Directly invokes _emit_backtrace_syslog_lines()'s own syms == NULL
 * ("backtrace capture failed") branch against logger's own shared target,
 * for testing; mirroring clog_test_gzip_compress_file()'s own precedent of
 * exposing a hard-to-reach internal helper directly. This branch's own
 * last-resort fallback (for when even its small marker record fails to
 * append) can only be reached by combining a forced backtrace-capture
 * failure with a forced buffer-append failure at exactly this call, in a
 * process that otherwise has no way to fail such a small append at all
 * given this library's actual buffer-sizing constants; see
 * clog_test_force_next_buf_ensure_failure()'s own doc comment.
 */
void clog_test_emit_backtrace_syslog_unavailable_marker(clog logger,
                                                        clog_level_t level) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return;
  struct timeval ts;
  gettimeofday(&ts, NULL);
  mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    _emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, NULL, 0);
  mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}
#endif

/* ========================================================================== */
/*                         ASYNC MESSAGE TYPES                                */
/* ========================================================================== */

typedef enum { CLOG_ASYNC_MSG_JOB, CLOG_ASYNC_MSG_FLUSH } clog_async_msg_kind_t;

/*
 * Offsets into a clog_async_job_t's own field_pool, NOT raw pointers: the
 * pool is a ccol_growbuf_t built incrementally by _snapshot_fields(), and a
 * growbuf's backing store can move on realloc mid-build; offsets recorded
 * during that same build remain valid regardless of any such move, while
 * raw pointers captured mid-build would not.
 */
typedef struct {
  size_t key_offset;
  size_t value_offset;
} clog_field_view_t;

/*
 * A queued, fully self-contained unit of work for the writer thread: every
 * piece of information _clog_build_record() needs, captured by the
 * submitting thread at submission time (never re-derived by the writer
 * thread itself, which may run well after the fact and on a different
 * thread than the one that formatted the message).
 */
typedef struct clog_async_job {
  clog_level_t level;
  const char *file; /* __FILE__ literal, never copied */
  int line;
  const char *func; /* __func__ literal, never copied */
  bool with_backtrace;
  struct timeval ts;  /* captured at SUBMISSION time, not write time */
  char proc_val[256]; /* progname(pid):tname(tid), captured at submission;
      thread identity must be captured here, not by the writer thread */

  char msg_inline[256]; /* stack-buffer-then-heap-spill, relocated into the
      job; smaller than the sync path's 1024-byte stack buffer since this is
      now a heap cost paid per queued message, not a thread-stack cost */
  char *msg_heap;       /* NULL unless the message spilled past msg_inline */
  const char *msg;      /* == msg_inline or msg_heap; never NULL */

  clog_field_view_t *fields; /* array of field_count views; NULL if
      field_count == 0. Exact-sized single alloc via chmap_elem_count(), which
      is O(1) and known before iterating, so no separate counting pass is
      needed */
  size_t field_count;
  ccol_growbuf_t field_pool;   /* NUL-separated key/value bytes; see
        clog_field_view_t's own comment. Built in one pass under fields_mutex */
  bool fields_snapshot_failed; /* true iff the snapshot itself could not be
      taken (field_pool's own OOM latch, or the views array itself failing to
      allocate); distinct from "genuinely has 0 fields" */

  char **bt_syms; /* backtrace_symbols() result, captured on the ORIGINAL
      calling thread; a backtrace from the writer thread's own stack would
      be meaningless */
  int bt_depth;
} clog_async_job_t;

/* Blocking rendezvous for clog_flush(): the requester waits on ctrl.cv until
 * the writer thread sets ctrl.done and broadcasts, after flushing everything
 * queued ahead of this request. Always stack-allocated on the requester's
 * own frame (see _clog_flush_pinned()), never freed by the writer thread. */
typedef struct {
  mutex_t mutex;
  cond_var_t cv;
  bool done;
} clog_async_ctrl_t;

/*
 * The envelope actually sent through the queue. kind is an explicit
 * discriminator (not an implicit sizeof-coincidence check) so the writer
 * thread can always tell a real job apart from a flush request, robust
 * against a future field addition accidentally colliding two struct sizes.
 */
typedef struct clog_async_msg {
  clog_async_msg_kind_t kind;
  union {
    clog_async_job_t job;    /* kind == CLOG_ASYNC_MSG_JOB; heap-allocated,
          owned/freed by the writer thread (or by the enqueue-failure fallback
          in _clog_write_async(), if the message never reached the queue at
          all); job is embedded BY VALUE inside this union, so there is no
          separate allocation for it, only for the envelope as a whole */
    clog_async_ctrl_t *ctrl; /* kind == CLOG_ASYNC_MSG_FLUSH; points at the
        REQUESTER's own stack frame (clog_flush() blocks until ctrl->done, so
        it outlives use); never freed by the writer thread */
  } u;
} clog_async_msg_t;

/* ========================================================================== */
/*                         RECORD BUILDING                                    */
/* ========================================================================== */

/*
 * A field-emission source: either a logger's own LIVE chmap (the
 * synchronous/FATAL write path, where fields are read directly under
 * fields_mutex at the moment the record is built) or a pre-snapshotted
 * array of {key,value} offsets into a flat byte pool (the async path, where
 * fields were captured at submission time by _snapshot_fields(); see that
 * function's own doc comment for why offsets, not raw pointers, are stored).
 * One shared field-driving loop (_clog_emit_fields) handles both, so the
 * three per-format field-append primitives below are exercised identically
 * regardless of which mode produced the key/value pairs.
 */
typedef struct {
  bool is_live;
  union {
    struct {
      chmap fields;
      mutex_t *mutex;
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
 * Drives per-field emission for either field source above, using whichever
 * of the three append primitives matches fmt. Returns false only when the
 * fields could not be enumerated/emitted at all (an allocation failure, or a
 * mid-field append failure that leaves the field list genuinely incomplete);
 * *out_alloc_failure distinguishes "a transient allocation failure" from
 * "the record's own content is simply too large" for
 * _clog_build_fallback_record()'s own diagnostic message.
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
    /* chmap_elem_count() reads lg->fields's own internal element count with
     * no locking of its own (chashmap is an externally-synchronized
     * container in this codebase); the empty-map disambiguation check below
     * (needed because chashmap_begin_iter() returns NULL for both "empty"
     * and "iterator allocation failed") must therefore run under the same
     * mutex as every other lg->fields access, not before acquiring it;
     * otherwise it races a concurrent clog_set_field()/_remove_field()/
     * _clear_fields() call on the same handle from another thread, which is
     * exactly the kind of access this lock exists to serialize against. */
    mutex_lock(*src->u.live.mutex);
    if (chmap_elem_count(fields) == 0) {
      mutex_unlock(*src->u.live.mutex);
      return true;
    }
    cmap_iterator *it = chashmap_begin_iter(fields, NULL);
    bool ok = true;
    /* A NULL iterator here means its own allocation failed, not that the
     * field map is empty (already handled above); surface it via the
     * alloc_failure flag rather than silently dropping every field. */
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
    mutex_unlock(*src->u.live.mutex);
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
 * Appends ts/level/proc/src/func in whatever shape each format uses: for
 * JSON and logfmt this is exactly those five fields with nothing left open;
 * for syslog it also includes the RFC 5424 PRI/TIMESTAMP/HOSTNAME/APP-NAME/
 * PROCID/MSGID preamble and the "[ccol proc=... src=... func=..." structured-
 * data block's own opening, deliberately left unclosed since the field loop
 * appends further key="value" params into that same bracket (closed by
 * _clog_build_msg_and_close() below). Never performs any I/O. Precondition:
 * sh->mutex held.
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
    const char *hostname = sh->syslog_hostname[0] ? sh->syslog_hostname : "-";
    const char *appname = sh->syslog_appname;
    ok = ok && _buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_appendf(out, " %s %s %d %s", hostname, appname,
                            (int)getpid(), _LEVEL_STR[level]) == 0;
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
      /* See the identical block this was extracted from for why a long
       * __FILE__ path spills to a heap buffer rather than being appended
       * raw and unescaped. */
      char src_stack[512];
      char *src_val = src_stack;
      char *src_heap = NULL;
      int slen = snprintf(src_stack, sizeof src_stack, "%s:%d", file, line);
      if (slen < 0) {
        ok = false;
      } else if ((size_t)slen >= sizeof src_stack) {
        src_heap = _mem_alloc(sh->m_procs, (size_t)slen + 1);
        if (!src_heap) {
          /* A genuine allocator failure, not a CLOG_BUF_MAX-style size
           * rejection; flag it the same way _buf_ensure() flags one, so
           * _clog_build_record()'s later out->oom check reports this
           * accurately instead of always blaming record size. */
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
      _mem_free(sh->m_procs, src_heap);
    }
    ok = ok && _buf_append(out, " func=", 6) == 0;
    ok = ok && _buf_append_lv(out, func) == 0;
  }

  return ok;
}

/*
 * Appends msg + record-closing punctuation, then (if with_backtrace) the
 * JSON-inline backtrace array. Logfmt/syslog backtrace frames are NOT
 * handled here; they are appended/written by the caller, strictly after
 * the primary record's own write, via _emit_backtrace_lines()/
 * _emit_backtrace_syslog_lines() respectively; only JSON embeds its
 * backtrace inline in the very record this function closes. Called only
 * after fields have already been appended by the caller, so structured data
 * is never split across write() calls. Precondition: sh->mutex held.
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
 * Called when the ordinary record-building sequence failed. This has two
 * distinct causes: the record's own content genuinely exceeded the target
 * buffer's own cap_limit (an oversized message, or a very large accumulation
 * of field values), or
 * a transient, size-unrelated allocation failure prevented the record from
 * being safely completed regardless of its actual size. `alloc_failure`
 * distinguishes the two so the note below reports the real cause rather
 * than always blaming size.
 *
 * Builds starting from whatever out->len already is (the caller has already
 * rolled it back to where this record started) rather than resetting out to
 * 0 itself: out may be a batch buffer already holding several earlier,
 * successfully-built records from other jobs (the async writer thread's own
 * async_buf), and resetting it here would destroy all of them the instant
 * any single later job in the same batch needed to fall back. The two
 * single-record callers (the synchronous/FATAL write path, and the async
 * enqueue-failure fallback) each reset their own buffer to empty exactly
 * once, before building their one record, so this ends up being that
 * buffer's only content for them, matching this function's original,
 * pre-batching behavior exactly.
 *
 * `with_backtrace` matters only for JSON: logfmt and syslog emit backtrace
 * frames independently of whether the primary record itself fell back to
 * this placeholder, so a requested backtrace is never lost for those two
 * formats. JSON instead embeds its backtrace inline in the very record this
 * function replaces; when with_backtrace is true this appends the same
 * _BT_JSON_ERROR_MARKER used elsewhere in this file so a reader can still
 * tell "backtrace omitted" apart from "backtrace never requested".
 *
 * Returns true if this fixed-size placeholder was appended to `out` in full;
 * false if even IT could not fit in whatever room `out` had left (only
 * reachable when `out` is the async writer thread's shared aggregation
 * buffer and it is already sitting right up against its own cap_limit),
 * in which case `out` is left completely untouched
 * (rolled back to its own length on entry) so the caller can flush what is
 * already safely in `out` and retry against a freshly emptied buffer,
 * rather than ever writing a truncated or malformed record.
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
    /* Deliberately generic: alloc_failure is now raised by any genuine
     * allocator failure encountered while building this record (the header,
     * a field's value, the message, or the field snapshot/iterator itself),
     * not only a failure to enumerate fields; see _clog_build_record()'s
     * own out->oom check. */
    snprintf(note, sizeof note,
             "log record dropped: a transient allocation failure prevented "
             "it from being fully built");
  } else {
    /* out->cap_limit, not the fixed CLOG_BUF_MAX constant: a caller-
     * configured clog_async_cfg_t.flush_buffer_size larger than CLOG_BUF_MAX
     * raises the async batch buffer's own limit above the default (see
     * clog_buf_t.cap_limit's own doc comment), and this diagnostic must
     * report the limit actually in effect for THIS buffer, not the
     * single-record default every other buffer in this file still uses. */
    snprintf(note, sizeof note,
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
                       sizeof _BT_JSON_ERROR_MARKER - 1) == 0;
    ok = ok && _buf_append(out, "}\n", 2) == 0;
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    const char *hostname = sh->syslog_hostname[0] ? sh->syslog_hostname : "-";
    const char *appname = sh->syslog_appname;
    ok = ok && _buf_appendf(out, "<%d>1 ", pri) == 0;
    ok = ok && _buf_append_ts_at(out, ts) == 0;
    ok = ok && _buf_appendf(out, " %s %s %d %s [ccol proc=\"", hostname,
                            appname, (int)getpid(), _LEVEL_STR[level]) == 0;
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
 * The shared record-building sequence, driving _clog_build_header() +
 * _clog_emit_fields() + _clog_build_msg_and_close() into out, falling back
 * to _clog_build_fallback_record() on any failure; used by both the
 * synchronous/FATAL write path (out == a handle's own lg->buf, field_src in
 * "live" mode) and the async writer thread's per-job processing (out ==
 * sh->async_buf, field_src in "snap" mode). For logfmt, backtrace frames
 * (if requested) are appended into the same out buffer immediately after
 * the primary record, so they land in the same eventual write()/flush as
 * the record itself; JSON already embedded its own backtrace inline via
 * _clog_build_msg_and_close(). Syslog's own backtrace frames are NEVER
 * appended here (they are exempt from batching); the caller must invoke
 * _emit_backtrace_syslog_lines() itself, strictly after writing out.
 * Precondition: sh->mutex held.
 *
 * Returns true once a record (either the real one, or the fallback
 * placeholder) has been fully appended to `out`. Returns false only in the
 * narrow case where even the fixed-size fallback placeholder could not fit
 * in whatever room `out` had left (see _clog_build_fallback_record()'s own
 * doc comment); `out` is left completely untouched in that case, and the
 * caller must flush/empty it and call this function again rather than ever
 * treat `out` as holding a usable record.
 *
 * out_used_fallback (may be NULL if the caller doesn't need it) is set true
 * whenever the fallback placeholder was substituted for the real record,
 * regardless of whether that substitution itself succeeded, and false
 * whenever the real record was built as-is. A caller building into a batch
 * buffer that may already hold OTHER, unrelated records (the async writer
 * thread's own sh->async_buf) needs this: a true here does NOT necessarily
 * mean the record itself is too large; it may simply not have fit in
 * whatever headroom was left by prior content sharing the same buffer, in
 * which case the caller should flush that prior content and rebuild this
 * record again from a freshly emptied buffer rather than let a small,
 * perfectly ordinary record be silently replaced by a "too large" note. The
 * two single-record callers (the synchronous/FATAL write path, and the
 * async enqueue-failure fallback) always build into a buffer they reset to
 * empty immediately beforehand, so out_used_fallback can only ever be true
 * there for a genuinely oversized record; both pass NULL.
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
  /* Reset before this attempt so a real allocator failure flagged by an
   * EARLIER, unrelated build against this same (possibly long-lived, reused)
   * buffer can never leak into this record's own alloc_failure verdict
   * below. */
  out->oom = false;
  bool ok =
      _clog_build_header(out, sh, fmt, level, ts, proc_val, file, line, func);
  bool alloc_failure = false;
  if (ok) ok = _clog_emit_fields(out, fmt, field_src, &alloc_failure);
  if (ok)
    ok = _clog_build_msg_and_close(out, fmt, msg, with_backtrace, bt_syms,
                                   bt_depth);
  /* _clog_emit_fields() only ever reports alloc_failure for a field
   * snapshot/iterator that could not be obtained at all; a header-build
   * failure, or a per-field append failing mid-loop, both go through
   * _buf_ensure() instead, which flags out->oom directly on a genuine
   * allocator failure (never on merely hitting out->cap_limit). Folding it in
   * here, after every build step has had a chance to set it, is what makes
   * _clog_build_fallback_record()'s own diagnostic accurate regardless of
   * which of the three build steps (or which specific append within any
   * of them) was the one that actually ran out of memory. */
  if (!alloc_failure) alloc_failure = out->oom;

  /* For LOGFMT, backtrace frames are appended as trailing continuation lines
   * into this SAME buffer, immediately following the primary record just
   * closed above; unlike JSON (whose backtrace is embedded INSIDE the very
   * record _clog_build_msg_and_close() just closed, so a too-large-to-embed
   * backtrace already forces `ok` false through that call) and syslog (whose
   * frames are always separate records the caller writes independently, and
   * never reach this function at all). Attempted only while the primary
   * record itself is still genuinely intact (`ok`): if the primary content
   * has already failed for its own, unrelated reasons, it is about to be
   * replaced by the generic fallback placeholder below regardless, and the
   * backtrace gets its own, separate attempt to follow THAT placeholder
   * instead (see below) rather than being attempted here against a buffer
   * state that is about to be discarded anyway. */
  bool logfmt_bt = with_backtrace && fmt == CLOG_FMT_LOGFMT;
  if (ok && logfmt_bt && !_emit_backtrace_lines(out, bt_syms, bt_depth)) {
    /* Not one single frame, nor even the small fixed-size "unavailable"
     * marker (see _emit_backtrace_lines()'s own doc comment), fit in
     * whatever room was left after the primary record; reachable only when
     * `out` is sitting within a few dozen bytes of its own cap_limit. The
     * primary record was otherwise perfectly fine, but a record
     * that explicitly requested a backtrace must never silently end up
     * missing it with zero trace anywhere (the exact, previously-real bug
     * this function's own callers no longer reproduce for every OTHER
     * failure mode). Force the same fallback-record recovery every other
     * build failure already gets: `out->oom` reflects whether this specific
     * failure was itself a genuine allocator failure or merely hit the
     * capacity cap, exactly as it already does for every other build step
     * above, so the resulting placeholder's own diagnostic stays accurate. */
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
      /* A genuinely oversized (or otherwise build-failed) primary record
       * must never also cost the caller its backtrace: attempt it again, now
       * appended right after the small fallback placeholder that just
       * replaced the primary content, in a buffer with far more headroom
       * than moments ago. A further failure here (not even the marker fits
       * immediately after a placeholder that itself just fit) is accepted
       * as a residual, expected-unreachable-in-practice gap, the same class
       * _clog_write_unrepresentable_record()'s own doc comment already
       * accepts for the fallback placeholder's own size. */
      (void)_emit_backtrace_lines(out, bt_syms, bt_depth);
    }
  }
  return true;
}

/*
 * Last-resort emission for the one case _clog_build_record() itself has no
 * further fallback for: not even its own small, fixed-size placeholder could
 * be appended to `out`. For a single-record buffer (lg->buf, used by both of
 * this function's callers below) this is expected to be unreachable in
 * practice; lg->buf is always reset to empty immediately beforehand, and
 * its capacity can never shrink below CLOG_BUF_INITIAL (comfortably larger
 * than any fallback record this file ever builds), so _clog_build_fallback_
 * record() never actually needs to grow the buffer at all here and cannot
 * fail. This function exists so that guarantee is not something a future
 * change (a smaller CLOG_BUF_INITIAL, a differently-sized buffer reused for
 * this purpose, ...) could silently turn into a fully silent, zero-byte
 * record loss with no trace anywhere: it writes a minimal, still
 * format-correct, newline-terminated line DIRECTLY to the fd via a
 * fixed-size stack buffer (snprintf never allocates), bypassing the
 * clog_buf_t/heap-allocation machinery entirely so it cannot itself fail the
 * same way. Precondition: sh->mutex held; sh->fd >= 0.
 *
 * with_backtrace mirrors the same guarantee _clog_build_record()/
 * _emit_backtrace_lines()/_emit_backtrace_json() already give an ordinary
 * (non-unrepresentable) record: a caller that asked for a backtrace must
 * never see one silently vanish. CLOG_FMT_SYSLOG needs no handling here;
 * every caller of this function already emits syslog's own backtrace lines
 * unconditionally, via a separate _emit_backtrace_syslog_lines() call, right
 * next to its own call into this function, regardless of what happens here.
 * CLOG_FMT_JSON embeds its own "bt_error" marker directly into the one JSON
 * line built below, mirroring _clog_build_fallback_record()'s identical
 * with_backtrace handling for that format. CLOG_FMT_LOGFMT is the one format
 * whose backtrace lines are normally appended into the SAME buffer as the
 * primary record by _emit_backtrace_lines() (unreachable from there once
 * this function is reached at all; reaching it means not even the small
 * fallback placeholder fit in that buffer), so it gets its own explicit,
 * allocation-free marker line appended right after the primary one.
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
    n = with_backtrace
            ? snprintf(line, sizeof line,
                       "{\"level\":\"%s\",\"msg\":\"%s\","
                       "\"bt_error\":\"unavailable\"}\n",
                       _LEVEL_STR[level], note)
            : snprintf(line, sizeof line, "{\"level\":\"%s\",\"msg\":\"%s\"}\n",
                       _LEVEL_STR[level], note);
  } else if (fmt == CLOG_FMT_SYSLOG) {
    int pri = (int)sh->syslog_facility * 8 + _SYSLOG_SEVERITY[level];
    n = snprintf(line, sizeof line, "<%d>1 - - - - - - %s\n", pri, note);
  } else {
    n = snprintf(line, sizeof line, "level=%s msg=\"%s\"\n", _LEVEL_STR[level],
                 note);
  }
  if (n > 0) {
    size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    size_t written = _write_all(sh->fd, line, len);
    if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  }

  if (with_backtrace && fmt == CLOG_FMT_LOGFMT) {
    size_t written = _write_all(sh->fd, _BT_LINE_ERROR_MARKER,
                                sizeof _BT_LINE_ERROR_MARKER - 1);
    if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Directly invokes _clog_write_unrepresentable_record() against logger's own
 * shared target, for testing; mirroring clog_test_gzip_compress_file()'s
 * own precedent of exposing a hard-to-reach internal helper directly rather
 * than trying to contrive real-world conditions that reach it. Reaching this
 * function through the ordinary log_* call path requires not even
 * _clog_build_record()'s own small fallback placeholder to fit in the
 * target buffer, which (as that function's own doc comment explains)
 * cannot happen today given CLOG_BUF_INITIAL's actual value; a test that
 * wants to exercise this function's own behavior in isolation, without
 * inventing a way to violate that invariant for real, needs a direct call
 * like this one instead.
 */
void clog_test_write_unrepresentable_record(clog logger, clog_format_t fmt,
                                            clog_level_t level,
                                            bool with_backtrace) {
  struct clogger *lg = _clog_resolve(logger);
  if (!lg) return;
  mutex_lock(lg->shared->mutex);
  if (lg->shared->fd >= 0)
    _clog_write_unrepresentable_record(lg->shared, fmt, level, with_backtrace);
  mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}
#endif

/* ========================================================================== */
/*                         CONSTRUCTOR HELPERS                                */
/* ========================================================================== */

/*
 * A logger's underlying fd may be a pipe or a socket (the CLOG_FMT_SYSLOG
 * usage documented in clogger.h connects to a UNIX datagram socket, and any
 * fd-based logger may just as easily be handed a pipe, e.g. stdout piped
 * into a reader that exits early). If the peer end goes away, the next
 * write() would otherwise raise SIGPIPE and, under its default disposition,
 * terminate the whole process (a surprising way for a logging call to kill
 * an otherwise-healthy application). Ignored once, process-wide, the
 * first time any logger is created; _write_all()'s existing EPIPE handling
 * (folded into its generic "unrecoverable write error" path) then takes
 * over gracefully instead.
 */
static once_flag_t _clog_sigpipe_once = ONCE_INIT;
static void _clog_ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }

static chmap _fields_create(ccol_memmgmt_procs_t *m_procs) {
  char *err = NULL;
  chmap m = chmap_create_mp(DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                            ccol_string, m_procs, &err);
  return m; /* NULL on failure */
}

/*
 * Free a clog_shared_t that has no live mutex and no logger children.
 * Safe to call at any point during _shared_alloc's initialization after
 * sh->m_procs has been set (which happens immediately after allocating sh).
 * Deliberately never touches sh->fd itself, regardless of sh->owns_fd: this
 * function is also reached from _shared_alloc()'s own early failure paths,
 * whose caller (_alloc(), and beyond it clog_open_fd_mp()/clog_open_file_mp())
 * already owns closing the fd on ITS OWN failure branch in that case, so
 * closing it here too would double-close it. Every other caller that owns
 * the fd and reaches this function after that point must close it itself
 * first, via _shared_close_owned_fd() below.
 */
static void _shared_free_partial(clog_shared_t *sh) {
  _mem_free(sh->m_procs, sh->file_path);
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
 * Closes sh->fd if this shared object owns it (a file-backed logger created
 * via clog_open_file_mp()) and it is still open, marking it closed
 * afterward. A no-op for a caller-owned fd (clog_open_fd_mp(), where
 * owns_fd is false and the caller alone is responsible for the fd's
 * lifetime) or an fd already closed. Shared by clog_close()'s own teardown
 * and every constructor rollback path that must not leak the fd it just
 * opened when a LATER construction step (async logging setup, handle-table
 * acquisition) fails; _shared_free_partial() itself deliberately never
 * does this (see its own doc comment).
 */
static void _shared_close_owned_fd(clog_shared_t *sh) {
  if (sh->owns_fd && sh->fd >= 0) {
    close(sh->fd);
    sh->fd = -1;
  }
}

static clog_shared_t *_shared_alloc(int fd, bool owns_fd, const char *file_path,
                                    ccol_memmgmt_procs_t *m_procs) {
  call_once(_clog_sigpipe_once, _clog_ignore_sigpipe);

  clog_shared_t *sh = _mem_calloc(m_procs, 1, sizeof *sh);
  if (!sh) return NULL;

  sh->fd = fd;
  sh->owns_fd = owns_fd;
  sh->ref_count = 1;

  if (m_procs) {
    sh->m_procs = (ccol_memmgmt_procs_t *)m_procs->malloc(sizeof *m_procs);
    if (!sh->m_procs) {
      m_procs->free(sh);
      return NULL;
    }
    memcpy(sh->m_procs, m_procs, sizeof *m_procs);
  }

  if (file_path) {
    size_t len = strlen(file_path) + 1;
    sh->file_path = _mem_alloc(sh->m_procs, len);
    if (!sh->file_path) {
      _shared_free_partial(sh);
      return NULL;
    }
    memcpy(sh->file_path, file_path, len);
  }

  if (mutex_init(sh->mutex) != 0) {
    _shared_free_partial(sh);
    return NULL;
  }

  sh->last_rotation = time(NULL);

  sh->syslog_facility = CLOG_SYSLOG_USER; /* calloc zeroes to KERN; override */

  /* Cache HOSTNAME for RFC 5424; sanitized the same way as APP-NAME below
   * (see _sanitize_syslog_printusascii_field()'s own doc comment), since the
   * raw value from gethostname(2) is not guaranteed by the kernel to consist
   * only of PRINTUSASCII bytes; an unsanitized space, control character, or
   * embedded newline here would otherwise reach the wire completely
   * unescaped (unlike every other dynamic field in a syslog record), letting
   * a misconfigured or adversarial hostname split one record into two. */
  char raw_hostname[256];
  if (gethostname(raw_hostname, sizeof raw_hostname) != 0)
    raw_hostname[0] = '\0';
  raw_hostname[sizeof raw_hostname - 1] = '\0';
  _sanitize_syslog_printusascii_field(raw_hostname, sh->syslog_hostname,
                                      sizeof sh->syslog_hostname);

  /* Cache APP-NAME for RFC 5424; see
   * _sanitize_syslog_printusascii_field()'s own doc comment for the exact
   * filtering rule. */
  _sanitize_syslog_printusascii_field(_syslog_appname(), sh->syslog_appname,
                                      sizeof sh->syslog_appname);

  return sh;
}

static struct clogger *_logger_alloc(clog_shared_t *shared,
                                     clog_level_t min_level) {
  struct clogger *lg = _mem_calloc(shared->m_procs, 1, sizeof *lg);
  if (!lg) return NULL;

  if (_buf_init(&lg->buf, shared->m_procs) != 0) {
    _mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->fields = _fields_create(shared->m_procs);
  if (!lg->fields) {
    _buf_free(&lg->buf);
    _mem_free(shared->m_procs, lg);
    return NULL;
  }

  if (mutex_init(lg->fields_mutex) != 0) {
    __chmap_destroy(lg->fields);
    _buf_free(&lg->buf);
    _mem_free(shared->m_procs, lg);
    return NULL;
  }

  lg->shared = shared;
  lg->min_level = min_level;
  return lg;
}

/* Free a fully-initialised logger without touching the shared backing store. */
static void _logger_free(struct clogger *lg) {
  mutex_destroy(lg->fields_mutex);
  __chmap_destroy(lg->fields);
  lg->fields = NULL;
  _buf_free(&lg->buf);
  _mem_free(lg->shared->m_procs, lg);
}

static struct clogger *_alloc(int fd, bool owns_fd, const char *file_path,
                              clog_level_t min_level,
                              ccol_memmgmt_procs_t *m_procs) {
  clog_shared_t *sh = _shared_alloc(fd, owns_fd, file_path, m_procs);
  if (!sh) return NULL;

  struct clogger *lg = _logger_alloc(sh, min_level);
  if (!lg) {
    mutex_destroy(sh->mutex);
    _shared_free_partial(sh);
    return NULL;
  }

  return lg;
}

/* ========================================================================== */
/*                         ASYNC LOGGING                                      */
/* ========================================================================== */

/*
 * Snapshots lg->fields as of this exact call into out_fields/out_count/
 * out_pool, under lg->fields_mutex, so a later mutation/removal on lg can
 * never retroactively change an already-submitted job's recorded fields.
 * *out_failed is set true (with *out_fields left NULL) only on a genuine
 * allocation failure; a logger with zero fields returns success with
 * *out_count == 0.
 */
static void _snapshot_fields(struct clogger *lg, clog_field_view_t **out_fields,
                             size_t *out_count, ccol_growbuf_t *out_pool,
                             bool *out_failed) {
  *out_fields = NULL;
  *out_count = 0;
  *out_failed = false;

  mutex_lock(lg->fields_mutex);
  size_t n = chmap_elem_count(lg->fields);
  if (n == 0) {
    mutex_unlock(lg->fields_mutex);
    /* Skip ccol_growbuf_init()'s own unconditional 256-byte backing-store
     * allocation entirely for the common case of a logger with no
     * persistent fields at all (paid on every single async log call
     * otherwise, purely to discover there is nothing to snapshot): construct
     * the same "valid, empty, nothing appended" shape directly instead.
     * ccol_growbuf_destroy() (always called later by
     * _clog_async_job_release(), unconditionally) is documented safe on
     * exactly this shape (b->buf may be NULL), so this is not merely an
     * optimization of the success path; it also closes the gap the
     * previous unconditional call had: ccol_growbuf_init() failing here
     * under real memory pressure (leaving out_pool->oom set) had no way to
     * surface as *out_failed, since this n == 0 branch returned before ever
     * checking it. */
    out_pool->buf = NULL;
    out_pool->len = 0;
    out_pool->cap = 0;
    out_pool->oom = false;
    out_pool->m_procs = lg->shared->m_procs;
    return;
  }
  ccol_growbuf_init(out_pool, lg->shared->m_procs);

  clog_field_view_t *views = _mem_calloc(lg->shared->m_procs, n, sizeof *views);
  if (!views) {
    mutex_unlock(lg->fields_mutex);
    *out_failed = true;
    return;
  }

  cmap_iterator *it = chashmap_begin_iter(lg->fields, NULL);
  if (!it) {
    mutex_unlock(lg->fields_mutex);
    _mem_free(lg->shared->m_procs, views);
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
  /* Defensive, not load-bearing under normal operation: chmap_elem_count()
   * and chashmap_begin_iter()'s own traversal are expected to always agree
   * on the number of live entries while fields_mutex is held throughout, so
   * `it` should already be NULL (auto-destroyed by its own _next_fn on the
   * last entry) by the time i == n stops the loop above. This call exists
   * purely so a violation of that invariant leaks nothing rather than
   * silently leaking cmap_iterator's own internal allocation on every single
   * field-carrying async submission; this function runs on clogger's own
   * hot path. */
  if (it) ccol_iter_destroy(it);
  mutex_unlock(lg->fields_mutex);

  if (out_pool->oom) {
    /* out_pool (job->field_pool) is deliberately left alone here, not
     * destroyed: this function's sole caller (_clog_write_async) always
     * routes the job (on every success and failure path alike) through
     * _clog_async_job_release(), which unconditionally destroys
     * job->field_pool exactly once. Destroying it here too was a real
     * double-free: ccol_growbuf_destroy() frees out_pool->buf without ever
     * nulling the pointer afterward, so _clog_async_job_release()'s later,
     * unconditional destroy would free the same already-freed block again.
     * The other two early-return failure paths above (views/iterator
     * allocation failure) already get this right by never touching out_pool
     * at all; this path now matches them. */
    _mem_free(lg->shared->m_procs, views);
    *out_failed = true;
    return;
  }

  *out_fields = views;
  *out_count = n;
}

/*
 * Flushes sh->async_buf to disk if it holds anything, applying the same
 * pre-write time-based / post-write size-based rotation checks the
 * synchronous write path uses. Always advances sh->last_flush_monotonic,
 * even when there was nothing to write: the writer thread's own main loop
 * recomputes its next timed-receive timeout from (now - last_flush_monotonic)
 * on every iteration, so leaving this stale after an empty timeout would make
 * an idle logger recompute a near-zero remaining time forever after and
 * busy-spin instead of genuinely sleeping for flush_interval_ms.
 * Precondition: sh->mutex held; caller owns calling this only for a
 * non-syslog target (syslog jobs self-flush immediately, never accumulating
 * into async_buf at all; see _clog_writer_thread_main()).
 *
 * The buffer is reset whenever it holds anything, whether or not sh->fd was
 * actually valid at the time (sh->fd < 0 is not expected to be observable
 * here in practice, since the fd is only ever closed after this shared
 * target's writer thread has already been joined; see
 * _shared_async_teardown()/clog_close()'s own ordering). Deliberately not
 * gating the reset on sh->fd >= 0 the same way the write/rotation-check
 * block below it is: if that invariant were ever violated by some future
 * change, the safe failure mode is dropping whatever was pending, exactly
 * like _write_all()'s own documented "silently drop on unrecoverable write
 * error" behavior, not silently accumulating an unbounded amount of
 * unflushable content in this buffer forever.
 */
static void _writer_flush_now(clog_shared_t *sh) {
  if (sh->async_buf.len > 0) {
    if (sh->fd >= 0) {
      _clog_time_rotate_if_due(sh);

      size_t written =
          _write_all(sh->fd, sh->async_buf.data, sh->async_buf.len);
      if (sh->rotation_enabled) sh->bytes_written += (off_t)written;

      _clog_size_rotate_if_due(sh);
    }
    _buf_reset(&sh->async_buf);
  }
  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);
}

/* Frees everything a CLOG_ASYNC_MSG_JOB envelope owns, including the
 * envelope itself. Shared by the writer thread's own normal per-job
 * processing and _clog_write_async()'s enqueue-failure fallback. */
static void _clog_async_job_release(clog_shared_t *sh,
                                    clog_async_msg_t *envelope) {
  clog_async_job_t *job = &envelope->u.job;
  if (job->bt_syms)
    free(job->bt_syms); /* backtrace_symbols()'s own malloc'd
block; never routed through _mem_free/a custom allocator, mirroring
every other backtrace-symbol free in this file */
  ccol_growbuf_destroy(&job->field_pool);
  _mem_free(sh->m_procs, job->fields);
  _mem_free(sh->m_procs, job->msg_heap);
  _mem_free(sh->m_procs, envelope); /* reclaims job's own storage too, since
      job is embedded by value inside envelope, never separately allocated */
}

static void *_clog_writer_thread_main(void *arg) {
  clog_shared_t *sh = (clog_shared_t *)arg;

  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms =
        (now.tv_sec - sh->last_flush_monotonic.tv_sec) * 1000L +
        (now.tv_nsec - sh->last_flush_monotonic.tv_nsec) / 1000000L;
    long remaining_ms = (long)sh->async_cfg.flush_interval_ms - elapsed_ms;
    /* Clamp to 0 rather than handing a negative relative duration to the
     * queue's own timed-receive call, which expects a non-negative
     * timespec and has no defined behavior for one that isn't; the
     * interval already having elapsed by the time this iteration starts
     * (e.g. after a slow job took longer to process than the whole
     * interval) must mean "check right now". */
    if (remaining_ms < 0) remaining_ms = 0;
    struct timespec timeout;
    timeout.tv_sec = remaining_ms / 1000;
    timeout.tv_nsec = (remaining_ms % 1000) * 1000000L;

    c_message_t m = {0};
    ccol_retval_t rv = sh->is_bounded_queue
                           ? circq_timed_recv_zc(sh->q.circq, &m, &timeout)
                           : dynmq_timed_recv_zc(sh->q.dynmq, &m, &timeout);

    if (rv == ccol_timed_out) {
      mutex_lock(sh->mutex);
      _writer_flush_now(sh);
      mutex_unlock(sh->mutex);
      continue;
    }
    if (rv != ccol_success) {
      /* A genuine, documented return value (circq_timed_recv_zc()/
       * dynmq_timed_recv_zc()'s own header doc: "ccol_unexpected_failure on
       * system error (check errno)"), not merely a theoretical one; and
       * merely looping back to the timed receive does NOT by itself avoid a
       * tight spin loop, since nothing on this path touches
       * sh->last_flush_monotonic: once flush_interval_ms has already elapsed
       * since the last real flush (an entirely ordinary steady state for an
       * otherwise-idle logger), `remaining_ms` above clamps to 0 and stays 0
       * on every subsequent iteration, so a persistent system error would
       * otherwise busy-spin this thread at 100% CPU on one core indefinitely,
       * with no backoff. usleep() is not a pthread/sem primitive, so it needs
       * no common.h wrapper (matching this file's other direct usleep()
       * calls); a short, fixed sleep bounds the retry rate regardless of how
       * long the underlying condition persists. */
      usleep(1000);
      continue;
    }

    if (m.data == NULL && m.size == 0) {
      /* Shutdown sentinel (see _shared_async_teardown()): drain everything
       * already built, then exit. Sent only after every prior job/flush
       * request has already been enqueued, and FIFO order guarantees this
       * is processed only once everything ahead of it has been. */
      mutex_lock(sh->mutex);
      _writer_flush_now(sh);
      mutex_unlock(sh->mutex);
      return NULL;
    }

    clog_async_msg_t *envelope = (clog_async_msg_t *)m.data;

    if (envelope->kind == CLOG_ASYNC_MSG_FLUSH) {
      clog_async_ctrl_t *ctrl = envelope->u.ctrl;
      mutex_lock(sh->mutex);
      _writer_flush_now(sh);
      mutex_unlock(sh->mutex);
      mutex_lock(ctrl->mutex);
      ctrl->done = true;
      cond_var_broadcast(ctrl->cv);
      mutex_unlock(ctrl->mutex);
      /* A FLUSH envelope is always stack-allocated on the requester's own
       * frame (see _clog_flush_pinned()); never free it. */
      continue;
    }

    /* CLOG_ASYNC_MSG_JOB */
    clog_async_job_t *job = &envelope->u.job;
    mutex_lock(sh->mutex);
    if (sh->fd >= 0) {
      clog_format_t fmt = sh->format;
      clog_field_source_t fsrc;
      fsrc.is_live = false;
      fsrc.u.snap.views = job->fields;
      fsrc.u.snap.count = job->field_count;
      fsrc.u.snap.pool = job->field_pool.buf;
      fsrc.u.snap.failed = job->fields_snapshot_failed;

      if (fmt == CLOG_FMT_SYSLOG) {
        /* Exempt from batching: build and write this one record immediately
         * (never accumulated into async_buf), then its backtrace frames (if
         * any), each with its own write() call; exactly mirroring the
         * synchronous write path's own syslog behavior, just executed by
         * the writer thread instead of the submitting thread.
         *
         * async_buf may still be holding un-flushed LOGFMT/JSON batch
         * content from before clog_set_format() retargeted this shared
         * target to CLOG_FMT_SYSLOG (format is read fresh per job, not
         * captured at submission time, and clog_set_format() itself never
         * touches async_buf); flushing it here (rather than a bare
         * _buf_reset(), which would silently discard it unwritten) is what
         * guarantees a format switch can reorder already-batched records
         * ahead of this one but never lose them outright. A no-op beyond
         * updating last_flush_monotonic when async_buf is already empty, the
         * overwhelmingly common case. Safe to call here: rotation can never
         * be enabled for a shared target that has ever used CLOG_FMT_SYSLOG
         * (rotation requires a file-backed, owns_fd == true logger, and
         * clog_set_format() only ever allows CLOG_FMT_SYSLOG when
         * owns_fd == false), so _writer_flush_now()'s own rotation checks
         * are always no-ops on this path. */
        _writer_flush_now(sh);
        bool built = _clog_build_record(
            &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
            job->file, job->line, job->func, job->msg, &fsrc,
            job->with_backtrace, job->bt_syms, job->bt_depth, NULL);
        if (built) {
          size_t written =
              _write_all(sh->fd, sh->async_buf.data, sh->async_buf.len);
          if (sh->rotation_enabled) sh->bytes_written += (off_t)written;
        } else {
          /* Not even _clog_build_record()'s own small, fixed-size fallback
           * placeholder fit in a freshly-flushed async_buf; see
           * _clog_write_unrepresentable_record()'s own doc comment for why
           * this never silently drops the record to zero written bytes
           * instead, mirroring every other build-into-a-buffer call site in
           * this file. */
          _clog_write_unrepresentable_record(sh, fmt, job->level,
                                             job->with_backtrace);
        }
        _buf_reset(&sh->async_buf);
        if (job->with_backtrace)
          _emit_backtrace_syslog_lines(&sh->async_buf, sh, job->level, &job->ts,
                                       job->bt_syms, job->bt_depth);
      } else {
        /* logfmt/JSON: append to the batch (never reset first; see
         * _clog_build_fallback_record()'s own doc comment for why), flush
         * once the size threshold is reached. */
        size_t record_start = sh->async_buf.len;
        bool batch_had_prior_content = record_start > 0;
        bool used_fallback = false;
        bool built = _clog_build_record(
            &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
            job->file, job->line, job->func, job->msg, &fsrc,
            job->with_backtrace, job->bt_syms, job->bt_depth, &used_fallback);
        if (!built || (used_fallback && batch_had_prior_content)) {
          /* Either not even the small, fixed-size fallback placeholder fit
           * in whatever room async_buf had left, or the fallback WAS used
           * but only because unrelated content already sitting in the batch
           * buffer (from an earlier job) left too little room for this
           * job's own real record; the record itself need not be
           * oversized at all. Both are only reachable when a single record
           * (or the batch's own already-accumulated prior content) comes
           * close to async_buf's own cap_limit. Roll back to
           * record_start first (discarding the fallback placeholder this
           * call may have already appended; `built` true means it's really
           * there in the buffer, not merely attempted), THEN flush what was
           * already safely accumulated ahead of it and retry: an empty
           * buffer always has more than enough room for an ordinary record,
           * and if the record genuinely is too large even on its own, this
           * retry correctly reproduces that same, now-accurate "too large"
           * fallback starting from record_start == 0. Without the rollback,
           * the flush below would durably write the misleading fallback
           * placeholder to disk BEFORE the retry ever ran, leaving both it
           * and the real record behind instead of only the real record. This
           * is what keeps the "never silently substitute a misleading
           * placeholder for a record that would have fit fine on its own"
           * guarantee true regardless of how flush_buffer_size is
           * configured. */
          sh->async_buf.len = record_start;
          _writer_flush_now(sh);
          /* This retry runs against a buffer _writer_flush_now() just reset
           * to len == 0, with async_buf's own cap left exactly as it was
           * (_buf_reset() never shrinks it); always at least
           * CLOG_BUF_INITIAL, comfortably larger than even the fallback
           * placeholder's own small footprint, so in practice this retry is
           * expected to always succeed, for the exact same reason
           * _clog_write_unrepresentable_record()'s own doc comment already
           * gives for lg->buf on the synchronous/enqueue-failure paths: a
           * single-record buffer that is always reset to empty beforehand
           * and whose capacity can never shrink below CLOG_BUF_INITIAL never
           * actually needs to grow just to fit that placeholder, so this
           * call cannot itself fail today. This return value used to be
           * discarded outright regardless, which would have silently
           * dropped the record (and its backtrace, if any) with zero trace
           * the moment a future change (a smaller CLOG_BUF_INITIAL, or a
           * differently-sized/pre-shrunk buffer reused for this purpose)
           * ever made that "cannot fail" guarantee stop holding; the exact
           * class of regression _clog_write_unrepresentable_record() exists
           * to guard against on its OTHER two call sites. Checking it here
           * too, and falling back to that same allocation-free, cannot-
           * itself-fail last resort, closes the one call site in this file
           * that was missing this guard. */
          if (!_clog_build_record(
                  &sh->async_buf, sh, fmt, job->level, &job->ts, job->proc_val,
                  job->file, job->line, job->func, job->msg, &fsrc,
                  job->with_backtrace, job->bt_syms, job->bt_depth, NULL)) {
            /* This writes real bytes straight to sh->fd, bypassing
             * async_buf entirely; unlike every other write in this
             * function, it is never followed by the size check below (that
             * check only ever fires off of async_buf's own length, which
             * this path never touches). Check explicitly here so this
             * still-unreachable-in-practice corner case does not silently
             * let size-based rotation fall behind the file's real on-disk
             * size, mirroring every other direct-write call site in this
             * file (_clog_write_sync, _clog_write_async's enqueue-failure
             * fallback), both of which already check unconditionally after
             * their own write regardless of which path produced it. */
            _clog_write_unrepresentable_record(sh, fmt, job->level,
                                               job->with_backtrace);
            _clog_size_rotate_if_due(sh);
          }
        }
        if (sh->async_buf.len >= sh->async_cfg.flush_buffer_size)
          _writer_flush_now(sh);
      }
    }
    mutex_unlock(sh->mutex);

    _clog_async_job_release(sh, envelope);
  }
}

/*
 * Resolves config defaults, creates the queue, inits the aggregation
 * buffer, and spawns the writer thread, rolling back everything already
 * built the moment any one step fails (queue creation is attempted first
 * specifically so there is nothing to unwind if it's the one that fails).
 * sh->async_enabled is set true only once every step has fully succeeded,
 * so a partially-initialized state is never observable as "enabled".
 * Called by each public constructor as the very last thing it does, once
 * every other piece of that constructor's own setup (including, for
 * clog_open_file_mp(), the rotation-config block) is already complete;
 * this guarantees the writer thread never observes sh's own fields still
 * being populated by the constructing thread, since the thread simply does
 * not exist yet until construction is otherwise finished.
 */
static bool _shared_async_init(clog_shared_t *sh,
                               const clog_async_cfg_t *async_cfg) {
  sh->async_cfg.queue_size = async_cfg->queue_size;
  sh->async_cfg.flush_buffer_size = async_cfg->flush_buffer_size
                                        ? async_cfg->flush_buffer_size
                                        : CLOG_DEFAULT_ASYNC_FLUSH_BUFFER_SIZE;
  sh->async_cfg.flush_interval_ms = async_cfg->flush_interval_ms
                                        ? async_cfg->flush_interval_ms
                                        : CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_MS;
  sh->is_bounded_queue = (async_cfg->queue_size > 0);

  char *err = NULL; /* discarded; clog_open_*_mp surfaces no error string
      on any failure today, matching existing convention */
  if (sh->is_bounded_queue) {
    sh->q.circq = circular_queue_create_with_mprocs(async_cfg->queue_size,
                                                    sh->m_procs, &err);
    if (!sh->q.circq) return false;
  } else {
    sh->q.dynmq = dynamic_queue_create_with_mprocs(sh->m_procs, &err);
    if (!sh->q.dynmq) return false;
  }

  if (_buf_init(&sh->async_buf, sh->m_procs) != 0) {
    if (sh->is_bounded_queue)
      circular_queue_destroy(sh->q.circq);
    else
      dynamic_queue_destroy(sh->q.dynmq);
    return false;
  }
  /* A caller-configured flush_buffer_size larger than CLOG_BUF_MAX must
   * actually be honored, not silently capped at the smaller, single-record-
   * oriented default every other clog_buf_t in this file uses; see
   * clog_buf_t.cap_limit's own doc comment. */
  _buf_raise_cap_limit(&sh->async_buf, sh->async_cfg.flush_buffer_size);

  clock_gettime(CLOCK_MONOTONIC, &sh->last_flush_monotonic);

  if (thread_create(sh->writer_thread, _clog_writer_thread_main, sh) != 0) {
    _buf_free(&sh->async_buf);
    if (sh->is_bounded_queue)
      circular_queue_destroy(sh->q.circq);
    else
      dynamic_queue_destroy(sh->q.dynmq);
    return false;
  }

  sh->async_enabled = true;
  return true;
}

/*
 * Sends the shutdown sentinel, retrying with a bounded backoff until it is
 * genuinely accepted by the queue rather than giving up after one attempt.
 *
 * A single unchecked attempt used to be enough in practice for a BOUNDED
 * queue (circq_send_zc() blocks until space frees, and the writer thread is
 * always still draining at this point, so space always eventually appears)
 * but not for the UNBOUNDED queue: dynmq_send_zc() never blocks, and can
 * genuinely fail with ccol_not_enough_memory if its own internal node
 * allocation fails under memory pressure; exactly the condition a caller
 * may be closing loggers in response to. Silently ignoring that failure and
 * proceeding straight to thread_join() (the original, buggy shape of this
 * function) hangs forever: the writer thread never receives the sentinel it
 * is blocked waiting for, and nothing else will ever send one on this
 * function's behalf.
 *
 * Retrying instead of giving up is deliberate, not merely convenient:
 * giving up here has no safe fallback. The writer thread cannot be abandoned
 * without join()ing it first, since the caller of this function is about to
 * free sh (and, via _buf_free/queue destroy, the very memory the writer
 * thread's own next loop iteration would still touch); abandoning it would
 * turn a hang into a genuine use-after-free/crash instead. This mirrors the
 * unbounded poll-wait clog_close() already uses to wait out a concurrent
 * resolver (see its own step 3): both exist because giving up would corrupt
 * state that is still legitimately in use elsewhere, not because an
 * unbounded wait is free of cost.
 */
static void _clog_send_shutdown_sentinel_blocking(clog_shared_t *sh) {
  unsigned int delay_us = 1000; /* 1ms */
  for (;;) {
    c_message_t sentinel = {.data = NULL, .size = 0};
    ccol_retval_t rv = sh->is_bounded_queue
                           ? circq_send_zc(sh->q.circq, &sentinel)
                           : dynmq_send_zc(sh->q.dynmq, &sentinel);
    if (rv == ccol_success) return;
    /* A transient allocation failure (the unbounded queue's own node
     * allocation), or the vanishingly unlikely ccol_not_permitted case (this
     * queue's send side is never disabled by anything in this file); retry
     * rather than leave the writer thread waiting for a sentinel that will
     * now never arrive. usleep() is not a pthread/sem primitive, so it needs
     * no common.h wrapper (matching clog_close()'s own identical use). */
    usleep(delay_us);
    if (delay_us < 100000) delay_us *= 2; /* cap backoff at 100ms */
  }
}

/*
 * Joins the writer thread and destroys the queue + async_buf, after first
 * guaranteeing the writer thread has actually been told to stop (see
 * _clog_send_shutdown_sentinel_blocking() above). Shared by clog_close()'s
 * last-handle teardown and each constructor's own rollback path (if
 * _clog_handle_acquire() fails after _shared_async_init() already
 * succeeded; no jobs could have been submitted through a not-yet-existing
 * handle, so the sentinel is always the only thing in the queue there).
 */
static void _shared_async_teardown(clog_shared_t *sh) {
  _clog_send_shutdown_sentinel_blocking(sh);
  thread_join(sh->writer_thread);
  if (sh->is_bounded_queue)
    circular_queue_destroy(sh->q.circq);
  else
    dynamic_queue_destroy(sh->q.dynmq);
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
  struct clogger *lg = _alloc(fd, false, NULL, min_level, mprocs);
  if (!lg) return CLOG_INVALID;

  if (async_cfg && !_shared_async_init(lg->shared, async_cfg)) {
    /* Captured before _logger_free(lg) below, which frees `lg` itself;
     * reading lg->shared afterward would be a use-after-free. */
    clog_shared_t *sh = lg->shared;
    _shared_close_owned_fd(sh);
    mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }

  clog h = _clog_handle_acquire(lg);
  if (h == CLOG_INVALID) {
    clog_shared_t *sh = lg->shared; /* see the identical note above */
    if (sh->async_enabled) _shared_async_teardown(sh);
    _shared_close_owned_fd(sh);
    mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }
  return h;
}

clog clog_open_file_mp(const char *path, clog_level_t min_level,
                       const clog_rotation_cfg_t *cfg,
                       const clog_async_cfg_t *async_cfg,
                       ccol_memmgmt_procs_t *mprocs) {
  if (!path) return CLOG_INVALID;
  if (!ccol_verify_memmgmt_procs(mprocs, (char **)NULL)) return CLOG_INVALID;

  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) return CLOG_INVALID;

  struct clogger *lg = _alloc(fd, true, path, min_level, mprocs);
  if (!lg) {
    close(fd);
    return CLOG_INVALID;
  }

  if (cfg) {
    lg->shared->rotation_enabled =
        cfg->size_rotation_enabled || cfg->time_rotation_enabled;
    lg->shared->rotation = *cfg;

    if (cfg->size_rotation_enabled && cfg->max_file_size <= 0)
      lg->shared->rotation.max_file_size = CLOG_DEFAULT_MAX_FILE_SIZE;

    if (cfg->time_rotation_enabled && cfg->rotation_interval_secs <= 0)
      lg->shared->rotation.rotation_interval_secs =
          CLOG_DEFAULT_ROTATION_INTERVAL;

    /* Unlike the two substitutions above, this one is not gated behind
     * either size_rotation_enabled or time_rotation_enabled individually:
     * pruning applies to whatever rotated files either mechanism produces,
     * so it is a property of the rotation config as a whole. A caller that
     * leaves this field at its zero-initialized default (by far the most
     * common way to reach 0 here, not a deliberate choice) must not end up
     * retaining rotated files forever and filling the disk; falling back to
     * CLOG_DEFAULT_MAX_ROTATED_FILES here is what makes that the case,
     * mirroring the other two rotation fields' own <=0 fallback exactly. */
    if (cfg->max_rotated_files <= 0)
      lg->shared->rotation.max_rotated_files = CLOG_DEFAULT_MAX_ROTATED_FILES;

    /* Bootstrap byte counter from the current on-disk size, needed so
     * size-based rotation correctly accounts for a pre-existing, non-empty
     * file opened for appending rather than assuming it starts empty.
     * fstat() essentially cannot fail on an fd this function itself just
     * opened successfully, but is not treated as unreachable: lseek() to the
     * end of the file is tried as a fallback (harmless under O_APPEND, which
     * always appends at the true end of file regardless of the fd's cached
     * offset), and if size rotation is actually enabled and BOTH fail, this
     * constructor fails outright rather than silently under-counting a
     * possibly large pre-existing file and letting it grow well past the
     * caller's own configured max_file_size before the first rotation ever
     * fires. */
    struct stat st;
    bool have_size = false;
    if (fstat(fd, &st) == 0) {
      lg->shared->bytes_written = st.st_size;
      have_size = true;
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
      mutex_destroy(sh->mutex);
      _logger_free(lg);
      _shared_free_partial(sh);
      return CLOG_INVALID;
    }
  }

  /* Async setup runs strictly after the rotation-config block above, never
   * concurrently with it: the writer thread must never observe sh's own
   * rotation fields still being populated by this constructing thread. */
  if (async_cfg && !_shared_async_init(lg->shared, async_cfg)) {
    /* Captured before _logger_free(lg) below, which frees `lg` itself;
     * reading lg->shared afterward would be a use-after-free. */
    clog_shared_t *sh = lg->shared;
    _shared_close_owned_fd(sh);
    mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }

  clog h = _clog_handle_acquire(lg);
  if (h == CLOG_INVALID) {
    clog_shared_t *sh = lg->shared; /* see the identical note above */
    if (sh->async_enabled) _shared_async_teardown(sh);
    _shared_close_owned_fd(sh);
    mutex_destroy(sh->mutex);
    _logger_free(lg);
    _shared_free_partial(sh);
    return CLOG_INVALID;
  }
  return h;
}

void clog_close(clog h) {
  struct clogger *raw;
  clog_shared_t *sh;

  /* Step 1-2: resolve+validate and mark in_use = false, both under the
   * table's write lock (must exclude a concurrent resolve from starting
   * once the close begins). Double-close is a fatal programming error,
   * matching chttpsvr's own explicit design. */
  {
    call_once(clog_slot_table.once, _clog_slot_table_init_globals);
    if (h == 0)
      fatal_err("clog_close: clog handle is invalid, stale, or already closed");
    uint32_t idx = (uint32_t)(h >> 32);
    uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
    rw_lock_wrlock(clog_slot_table.rwlock);
    clog_slot_t *slot = NULL;
    if (idx < cvector_elem_count(clog_slot_table.slots))
      slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
    if (!slot || !slot->in_use || slot->generation != gen) {
      rw_lock_unlock(clog_slot_table.rwlock);
      fatal_err("clog_close: clog handle is invalid, stale, or already closed");
    }
    raw = slot->ptr;
    sh = raw->shared;
    slot->in_use = false;
    rw_lock_unlock(clog_slot_table.rwlock);
  }

  /* Step 3: poll-wait for any in-flight, pin-holding operation on this exact
   * handle to finish; including one that may still be holding
   * raw->fields_mutex, which step 4 below is about to destroy. No upper
   * bound: giving up would mean freeing memory a live resolver still points
   * at. usleep() is not a pthread/sem primitive, so it needs no common.h
   * wrapper. */
  {
    unsigned int delay_us = 1;
    while (atomic_load(&raw->pending_resolve_count) > 0) {
      usleep(delay_us);
      if (delay_us < 1000) delay_us *= 2;
    }
  }

#ifdef RUNNING_UNIT_TESTS
  /* Test-only, deterministic widening of the in_use == false / freed ==
   * false window between here and step 4 below; see
   * clog_test_set_close_finalize_delay_us()'s own doc comment. A no-op
   * (reads a plain 0) outside of a test that has armed it. */
  {
    unsigned int d = atomic_load(&_clog_test_close_finalize_delay_us);
    if (d) {
      atomic_store(&_clog_test_close_finalize_delay_entered, true);
      usleep(d);
    }
  }
#endif

  /* Step 4: re-acquire the write lock to destroy raw's own per-handle state
   * (including fields_mutex) and retire its slot. This must happen under
   * the SAME lock _clog_atfork_prepare needs before it can even attempt to
   * lock a freed == false slot's fields_mutex, so the two operations cannot
   * interleave; see "Fork safety" for the full reasoning. Folds in the
   * slot's own final bookkeeping (clear ptr, bump generation again, push
   * the index onto the free list) since there is no reason to defer it
   * until after the (possibly slow) shared-object teardown below. */
  {
    rw_lock_wrlock(clog_slot_table.rwlock);
    uint32_t idx = (uint32_t)(h >> 32);
    clog_slot_t *slot = (clog_slot_t *)cvector_at(clog_slot_table.slots, idx);
    _logger_free(raw);
    slot->freed = true;
    slot->ptr = NULL;
    slot->generation++;
    if (slot->generation == 0) slot->generation++; /* skip the sentinel */
    cvector_push_back(clog_slot_table.free_indices, &idx);
    rw_lock_unlock(clog_slot_table.rwlock);
  }

  /* Step 5: decrement the shared ref count. Never overlaps holding both the
   * slot table's write lock and shared->mutex at once (step 4 has already
   * released the write lock before this step ever acquires shared->mutex). */
  mutex_lock(sh->mutex);
  int remaining = --sh->ref_count;
  mutex_unlock(sh->mutex);

  /* Step 6: only the last handle sharing sh actually frees it. */
  if (remaining == 0) {
    /* Remove sh from live_shareds under a fresh acquisition of the write
     * lock, strictly BEFORE anything else here touches sh->mutex again;
     * see "Fork safety" for why this ordering matters. */
    rw_lock_wrlock(clog_slot_table.rwlock);
    size_t n = cvector_elem_count(clog_slot_table.live_shareds);
    for (size_t i = 0; i < n; i++) {
      clog_shared_t *seen =
          *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i);
      if (seen == sh) {
        /* Swap-remove: order within live_shareds carries no meaning. */
        clog_shared_t *last =
            *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, n - 1);
        *(clog_shared_t **)cvector_at(clog_slot_table.live_shareds, i) = last;
        cvector_pop_back(clog_slot_table.live_shareds, &last);
        break;
      }
    }
    rw_lock_unlock(clog_slot_table.rwlock);

    /* Runs with sh->mutex NOT held: the writer thread's own loop acquires
     * that same mutex internally to process everything ahead of the
     * shutdown sentinel (including the sentinel's own final flush);
     * holding it here across thread_join() (inside
     * _shared_async_teardown()) would be a genuine self-deadlock, since the
     * writer thread could never acquire it to reach the sentinel at all.
     * Must run BEFORE the fd is closed below: jobs already queued by other,
     * already-closed sibling handles are still waiting to be drained and
     * written by the writer thread at this exact instant. */
    if (sh->async_enabled) _shared_async_teardown(sh);

    _shared_close_owned_fd(sh);
    mutex_destroy(sh->mutex);
    _shared_free_partial(sh);
  }
}

clog clog_derive(clog parent_h) {
  struct clogger *parent = _clog_resolve(parent_h);
  if (!parent)
    fatal_err("clog_derive: clog handle is invalid, stale, or already closed");

  /* min_level is _Atomic; reading it directly here (as _clog_write()'s own
   * fast path already does) needs no lock of its own. */
  struct clogger *child = _logger_alloc(parent->shared, parent->min_level);
  if (!child) {
    _clog_resolve_unpin(parent);
    return CLOG_INVALID;
  }

  /* Copy parent's fields into the child's independent field map. Guarded by
   * parent->fields_mutex alone (not shared->mutex), the same lock
   * clog_set_field()/_remove_field()/_clear_fields() now use, so this
   * genuinely synchronises against a concurrent mutation of parent's own
   * field map without contending with an unrelated write in progress on a
   * sibling logger sharing the same shared->mutex. */
  mutex_lock(parent->fields_mutex);
  if (chmap_elem_count(parent->fields) > 0) {
    cmap_iterator *it = chashmap_begin_iter(parent->fields, NULL);
    if (!it) {
      mutex_unlock(parent->fields_mutex);
      _logger_free(child);
      _clog_resolve_unpin(parent);
      return CLOG_INVALID;
    }
    while (it) {
      const char *k = (const char *)it->key_pair->ptr;
      const char *v = (const char *)it->val_pair->ptr;
      cmap_pair kp = {.ptr = (void *)k, .size = strlen(k) + 1};
      cmap_pair vp = {.ptr = (void *)v, .size = strlen(v) + 1};
      /* ccol_key_already_present is a documented successful upsert (the
       * value was written in place), not a failure; only genuine failures
       * (ccol_invalid_args, ccol_not_enough_memory, ...) should abort the
       * derive. */
      ccol_retval_t rv = chmap_insert_elem(child->fields, &kp, &vp);
      if (rv != ccol_success && rv != ccol_key_already_present) {
        ccol_iter_destroy(it);
        mutex_unlock(parent->fields_mutex);
        _logger_free(child);
        _clog_resolve_unpin(parent);
        return CLOG_INVALID;
      }
      it = it->_next_fn(it);
    }
  }
  mutex_unlock(parent->fields_mutex);

  /* ref_count is genuinely shared across the whole derive tree; still needs
   * shared->mutex, held only for this brief increment. parent is guaranteed
   * alive (and therefore parent->shared is guaranteed not yet destroyed) for
   * the entire duration of this call (its own pin is held throughout), so it
   * is safe to acquire this lock without having held it continuously since
   * the top of the function. */
  mutex_lock(parent->shared->mutex);
  parent->shared->ref_count++;
  mutex_unlock(parent->shared->mutex);

  clog h = _clog_handle_acquire(child);
  if (h == CLOG_INVALID) {
    /* child->shared == parent->shared, and parent->shared is guaranteed
     * still fully alive here (parent's own pin is still held throughout
     * this whole function); so reading child->shared->m_procs inside
     * _logger_free(child) below is always safe. */
    mutex_lock(parent->shared->mutex);
    parent->shared->ref_count--;
    mutex_unlock(parent->shared->mutex);
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
 * min_level lives on struct clogger, not clog_shared_t: it is a per-logger
 * setting, completely independent of every sibling logger sharing the same
 * shared target (see its own field comment). Reading/writing it through
 * lg->shared->mutex would serialise clog_set_level()/clog_get_level() on ONE
 * logger against a write/rotation/gzip-compression handoff currently in
 * progress on a completely unrelated SIBLING logger, for no correctness
 * benefit: min_level's own atomicity is what already makes an unlocked read
 * of it race-free (see _clog_write()'s own fast-path check, and
 * _clog_write_sync()'s locked re-check, both of which already rely on this
 * exact property rather than on any lock). Plain reads/writes of an _Atomic
 * lvalue are themselves sequentially consistent, so no lock is needed here
 * at all; mirroring how clog_set_field()/_remove_field()/_clear_fields()
 * already avoid shared->mutex for their own per-logger state (fields_mutex),
 * this needs no lock whatsoever, not even a lighter one.
 */
void clog_set_level(clog h, clog_level_t level) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
        "clog_set_level: clog handle is invalid, stale, or already closed");

  lg->min_level = level;
  _clog_resolve_unpin(lg);
}

clog_level_t clog_get_level(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
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
    fatal_err(
        "clog_set_format: clog handle is invalid, stale, or already closed");

  mutex_lock(lg->shared->mutex);
  /* CLOG_FMT_SYSLOG requires a caller-supplied fd (owns_fd == false). */
  if (fmt == CLOG_FMT_SYSLOG && lg->shared->owns_fd) {
    mutex_unlock(lg->shared->mutex);
    _clog_resolve_unpin(lg);
    return;
  }
  lg->shared->format = fmt;
  mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}

clog_format_t clog_get_format(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
        "clog_get_format: clog handle is invalid, stale, or already closed");

  mutex_lock(lg->shared->mutex);
  clog_format_t f = lg->shared->format;
  mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return f;
}

void clog_set_facility(clog h, clog_syslog_facility_t facility) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
        "clog_set_facility: clog handle is invalid, stale, or already closed");

  mutex_lock(lg->shared->mutex);
  lg->shared->syslog_facility = facility;
  mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
}

clog_syslog_facility_t clog_get_facility(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
        "clog_get_facility: clog handle is invalid, stale, or already closed");

  mutex_lock(lg->shared->mutex);
  clog_syslog_facility_t f = lg->shared->syslog_facility;
  mutex_unlock(lg->shared->mutex);
  _clog_resolve_unpin(lg);
  return f;
}

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

void clog_set_field(clog h, const char *key, const char *value) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
        "clog_set_field: clog handle is invalid, stale, or already closed");

  if (!key || !value) {
    _clog_resolve_unpin(lg);
    return;
  }

  /* Reject keys that would corrupt logfmt or RFC 5424 SD output.
   * RFC 5424 SD-PARAM-NAME requires at least one character, drawn from
   * PRINTUSASCII (0x21-0x7e) only; c < 0x21 rejects both control characters
   * and space in one comparison, and c > 0x7e rejects both DEL and every
   * non-ASCII byte (0x80-0xff), which PRINTUSASCII also excludes. */
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

  /* Guarded by this logger's own fields_mutex, not shared->mutex: this
   * logger's field map is fully independent of every sibling logger sharing
   * the same shared backing store, so a field mutation here must not
   * contend with an unrelated write in progress on a sibling (see
   * fields_mutex's own doc comment on struct clogger). */
  mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  cmap_pair vp = {.ptr = (void *)value, .size = strlen(value) + 1};
  chmap_insert_elem(lg->fields, &kp, &vp);

  mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

void clog_remove_field(clog h, const char *key) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
        "clog_remove_field: clog handle is invalid, stale, or already closed");

  if (!key) {
    _clog_resolve_unpin(lg);
    return;
  }

  mutex_lock(lg->fields_mutex);

  cmap_pair kp = {.ptr = (void *)key, .size = strlen(key) + 1};
  chmap_delete_elem(lg->fields, &kp);

  mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

void clog_clear_fields(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err(
        "clog_clear_fields: clog handle is invalid, stale, or already closed");

  mutex_lock(lg->fields_mutex);
  chmap_reset(lg->fields, 0);
  mutex_unlock(lg->fields_mutex);
  _clog_resolve_unpin(lg);
}

/* ========================================================================== */
/*                         CORE WRITE                                         */
/* ========================================================================== */

/*
 * The true synchronous body: builds and writes one record right now, using
 * live field iteration (under lg->fields_mutex) and a fresh (fmt, ap) pair,
 * with its own internal va_copy for the stack-then-heap vsnprintf retry.
 * Used by _clog_write()'s own FATAL/non-async branch, and by
 * _clog_write_async()'s own envelope-allocation-failure fallback (the one
 * case where nothing async-specific has happened yet and ap is still
 * completely fresh).
 *
 * bt_syms/bt_depth are already-captured inputs (see _capture_backtrace()'s
 * own doc comment for why this function never captures them itself); this
 * function only reads them, it never frees bt_syms; that responsibility
 * stays with whichever caller captured it.
 *
 * Precondition: NONE; takes and releases shared->mutex itself, for its
 * entire body; callers must never wrap a call to this in their own lock.
 *
 * __attribute__((format(printf, 9, 0))) (fmt is parameter 9; 0 means the
 * variadic arguments are already collected into the va_list ap, so there is
 * no separate first-vararg parameter to name) is what lets this function's
 * own internal vsnprintf(msg_stack/msg_heap, ..., fmt, ap) calls below pass
 * fmt/ap straight through without Clang's -Wformat-nonliteral complaining
 * that fmt is not a literal at those call sites; fmt genuinely never is a
 * literal here, since it is this function's own parameter, forwarded
 * verbatim from _clog_write()'s own already-format-checked (see
 * clogger.h's identical attribute on that function) variadic call. This
 * moves the real format-string/argument-type check to _clog_write()'s own
 * call sites, where fmt is actually a literal, rather than either losing
 * the check entirely or suppressing it via a pragma.
 */
static void __attribute__((format(printf, 9, 0))) _clog_write_sync(
    struct clogger *lg, clog_level_t level, const char *file, int line,
    const char *func, bool with_backtrace, char *const *bt_syms, int bt_depth,
    const char *fmt, va_list ap) {
  mutex_lock(lg->shared->mutex);

  /* CLOG_FATAL bypasses the level filter: the cause of termination must
   * always be recorded, regardless of min_level. This re-check is redundant
   * with _clog_write()'s own fast, unlocked check for the common case (both
   * read the same _Atomic min_level, so neither needs shared->mutex to be
   * race-free against a concurrent clog_set_level() call); it exists so a
   * level change that lands between the fast-path check and this point is
   * still honored for this one call, the same rare, intentionally-accepted
   * boundary-line race already documented for the fast path itself. */
  if (level < lg->min_level && level != CLOG_FATAL) {
    mutex_unlock(lg->shared->mutex);
    return;
  }

  if (lg->shared->fd < 0) {
    mutex_unlock(lg->shared->mutex);
    return;
  }

  const clog_format_t log_fmt = lg->shared->format;

  /* ----------------------------------------------------------------------- */
  /* Format the user message into a stack buffer; spill to heap if needed.   */
  /* ----------------------------------------------------------------------- */
  char msg_stack[1024];
  char *msg = msg_stack;
  char *msg_heap = NULL;

  va_list ap2;
  va_copy(ap2, ap);

  int mlen = vsnprintf(msg_stack, sizeof msg_stack, fmt, ap);

  if (mlen < 0) {
    static const char fmt_err_msg[] = "<log message formatting failed>";
    memcpy(msg_stack, fmt_err_msg, sizeof fmt_err_msg);
  } else if ((size_t)mlen >= sizeof msg_stack) {
    msg_heap = _mem_alloc(lg->shared->m_procs, (size_t)mlen + 1);
    if (msg_heap) {
      int mlen2 = vsnprintf(msg_heap, (size_t)mlen + 1, fmt, ap2);
      if (mlen2 >= 0) msg = msg_heap;
    }
    if (msg == msg_stack) {
      static const char trunc_marker[] = "...[truncated]";
      size_t mark_len = sizeof trunc_marker - 1;
      if (mark_len < sizeof msg_stack - 1)
        memcpy(msg_stack + sizeof msg_stack - 1 - mark_len, trunc_marker,
               mark_len);
    }
  }
  va_end(ap2);

  /* ----------------------------------------------------------------------- */
  /* Time-based rotation check (before the write).                           */
  /* ----------------------------------------------------------------------- */
  _clog_time_rotate_if_due(lg->shared);

  /* ----------------------------------------------------------------------- */
  /* Build proc identifier: progname(pid):tname(tid)                         */
  /* ----------------------------------------------------------------------- */
  char proc_val[256];
  {
    char tname[16];
    _get_thread_name(tname, sizeof tname);
    snprintf(proc_val, sizeof proc_val, "%.200s(%d):%.15s(%d)", _get_progname(),
             (int)getpid(), tname, (int)_get_tid());
  }

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

  _mem_free(lg->shared->m_procs, msg_heap);

  /* ----------------------------------------------------------------------- */
  /* Emit.                                                                    */
  /* ----------------------------------------------------------------------- */
  if (lg->shared->fd >= 0) {
    if (built) {
      size_t written = _write_all(lg->shared->fd, lg->buf.data, lg->buf.len);
      if (lg->shared->rotation_enabled)
        lg->shared->bytes_written += (off_t)written;
    } else {
      /* Not even _clog_build_record()'s own small, fixed-size fallback
       * placeholder fit in lg->buf; see _clog_write_unrepresentable_record()'s
       * own doc comment for why this never silently drops the record to zero
       * written bytes instead. */
      _clog_write_unrepresentable_record(lg->shared, log_fmt, level,
                                         with_backtrace);
    }
  }

  /* Syslog's own backtrace frames are exempt from batching and were NOT
   * appended by _clog_build_record() above; write them now, strictly after
   * the primary record's own write. JSON already embedded its backtrace
   * inline; logfmt's own lines were already appended into lg->buf above. */
  if (with_backtrace && lg->shared->fd >= 0 && log_fmt == CLOG_FMT_SYSLOG)
    _emit_backtrace_syslog_lines(&lg->buf, lg->shared, level, &ts, bt_syms,
                                 bt_depth);

  /* ----------------------------------------------------------------------- */
  /* Size-based rotation check (after the write).                             */
  /* ----------------------------------------------------------------------- */
  _clog_size_rotate_if_due(lg->shared);

  mutex_unlock(lg->shared->mutex);
}

/* Forward-declared: defined further down (needs clog_async_ctrl_t/
 * clog_async_msg_t, and its own test hooks, all introduced later in this
 * file), but _clog_write_async() below needs to call it; see that
 * function's own doc comment for why. */
static void _clog_flush_pinned(struct clogger *lg);

/*
 * Formats and queues one job for the writer thread. On envelope-allocation
 * failure, nothing async-specific has happened yet (ap is still fresh);
 * delegate entirely to _clog_write_sync(). On a later enqueue failure (the
 * unbounded queue's own node allocation failing, or the vanishingly
 * unlikely ccol_not_permitted case), ap/ap2 have already been consumed by
 * this function's own vsnprintf call, so _clog_write_sync() cannot be
 * reused; the record is instead built and written directly from the
 * already-fully-populated job, under lg->shared->mutex, through the
 * CALLING handle's own lg->buf (never sh->async_buf, which is exclusively
 * the writer thread's own buffer). A message (and its backtrace, if
 * requested) is never silently dropped by this function on any path.
 *
 * Before that direct write, _clog_flush_pinned() is used to drain anything
 * already queued ahead of this job: without it, this job's own record could
 * win the race for lg->shared->mutex against the writer thread and land on
 * disk BEFORE an earlier, already-successfully-enqueued job (from this same
 * handle or a sibling sharing the same target) that the writer thread simply
 * has not gotten around to yet, silently reordering log output relative to
 * submission order; exactly the kind of thing most likely to happen under
 * the same memory pressure that made THIS job's own enqueue fail in the
 * first place. The flush's own queue send can itself fail under the same
 * condition; _clog_flush_pinned() already returns promptly without hanging
 * in that case (see its own doc comment), so this remains best-effort rather
 * than a new way for a persistently broken queue to hang a log call.
 *
 * __attribute__((format(printf, 9, 0))), mirroring _clog_write_sync()'s own
 * identical attribute and for the identical reason: fmt (parameter 9) is
 * this function's own forwarded parameter, never a literal at this
 * function's own internal vsnprintf(job->msg_inline/msg_heap, ..., fmt, ap)
 * call sites below, so the real check belongs at _clog_write()'s own
 * already-format-checked call sites instead.
 */
static void __attribute__((format(printf, 9, 0))) _clog_write_async(
    struct clogger *lg, clog_level_t level, const char *file, int line,
    const char *func, bool with_backtrace, char **bt_syms, int bt_depth,
    const char *fmt, va_list ap) {
  clog_async_msg_t *envelope =
      _mem_calloc(lg->shared->m_procs, 1, sizeof *envelope);
  if (!envelope) {
    _clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                     bt_depth, fmt, ap);
    if (bt_syms) free(bt_syms);
    return;
  }
  envelope->kind = CLOG_ASYNC_MSG_JOB;
  clog_async_job_t *job = &envelope->u.job;

  job->bt_syms = bt_syms; /* ownership transferred into the job */
  job->bt_depth = bt_depth;
  job->level = level;
  job->file = file;
  job->line = line;
  job->func = func;
  job->with_backtrace = with_backtrace;
  gettimeofday(&job->ts, NULL);
  {
    char tname[16];
    _get_thread_name(tname, sizeof tname);
    snprintf(job->proc_val, sizeof job->proc_val, "%.200s(%d):%.15s(%d)",
             _get_progname(), (int)getpid(), tname, (int)_get_tid());
  }

  job->msg_heap = NULL;
  va_list ap2;
  va_copy(ap2, ap); /* the new copy this function boundary requires, distinct
      from _clog_write_sync()'s own internal one; crossing a function
      boundary with a va_list that gets used twice (the stack attempt here,
      the heap retry below) needs its own fresh copy taken before the first
      use consumes it */
  int mlen = vsnprintf(job->msg_inline, sizeof job->msg_inline, fmt, ap);
  if (mlen < 0) {
    static const char fmt_err_msg[] = "<log message formatting failed>";
    memcpy(job->msg_inline, fmt_err_msg, sizeof fmt_err_msg);
  } else if ((size_t)mlen >= sizeof job->msg_inline) {
    job->msg_heap = _mem_alloc(lg->shared->m_procs, (size_t)mlen + 1);
    if (job->msg_heap) {
      int mlen2 = vsnprintf(job->msg_heap, (size_t)mlen + 1, fmt, ap2);
      if (mlen2 < 0) {
        _mem_free(lg->shared->m_procs, job->msg_heap);
        job->msg_heap = NULL;
      }
    }
    if (!job->msg_heap) {
      static const char trunc_marker[] = "...[truncated]";
      size_t mark_len = sizeof trunc_marker - 1;
      if (mark_len < sizeof job->msg_inline - 1)
        memcpy(job->msg_inline + sizeof job->msg_inline - 1 - mark_len,
               trunc_marker, mark_len);
    }
  }
  va_end(ap2);
  job->msg = job->msg_heap ? job->msg_heap : job->msg_inline;

  _snapshot_fields(lg, &job->fields, &job->field_count, &job->field_pool,
                   &job->fields_snapshot_failed);

  c_message_t m = {.data = envelope, .size = sizeof *envelope};
  ccol_retval_t rv = lg->shared->is_bounded_queue
                         ? circq_send_zc(lg->shared->q.circq, &m)
                         : dynmq_send_zc(lg->shared->q.dynmq, &m);
  if (rv != ccol_success) {
    /* Drain anything already queued (from this handle or any sibling
     * sharing lg->shared) before this job's own direct write below, so that
     * write can never land on disk ahead of an earlier, already-enqueued
     * job the writer thread simply hasn't reached yet; see this
     * function's own doc comment. Safe to call here: this function's caller
     * (_clog_write()) only reaches _clog_write_async() for a non-FATAL
     * level, so this can never nest inside _clog_write()'s own FATAL-path
     * flush. */
    _clog_flush_pinned(lg);

    mutex_lock(lg->shared->mutex);
    if (lg->shared->fd >= 0) {
      clog_format_t fmt2 = lg->shared->format;
      clog_field_source_t fsrc;
      fsrc.is_live = false;
      fsrc.u.snap.views = job->fields;
      fsrc.u.snap.count = job->field_count;
      fsrc.u.snap.pool = job->field_pool.buf;
      fsrc.u.snap.failed = job->fields_snapshot_failed;

      /* This record is being written directly to sh->fd right here, exactly
       * like every other real write in this file; it must be bracketed by
       * the same pre-write time-based / post-write size-based rotation
       * checks every other write path applies, or a run of enqueue failures
       * (or even a single one, in an otherwise idle logger) could leave the
       * file well past max_file_size / rotation_interval_secs with nothing
       * left to ever notice, since this record never touches sh->async_buf
       * for a later flush to catch. */
      _clog_time_rotate_if_due(lg->shared);

      _buf_reset(&lg->buf);
      bool built = _clog_build_record(
          &lg->buf, lg->shared, fmt2, job->level, &job->ts, job->proc_val,
          job->file, job->line, job->func, job->msg, &fsrc, job->with_backtrace,
          job->bt_syms, job->bt_depth, NULL);
      if (built) {
        size_t written = _write_all(lg->shared->fd, lg->buf.data, lg->buf.len);
        if (lg->shared->rotation_enabled)
          lg->shared->bytes_written += (off_t)written;
      } else {
        /* See _clog_write_unrepresentable_record()'s own doc comment; this
         * mirrors _clog_write_sync()'s identical handling of the same,
         * expected-unreachable-in-practice condition. */
        _clog_write_unrepresentable_record(lg->shared, fmt2, job->level,
                                           job->with_backtrace);
      }
      if (job->with_backtrace && fmt2 == CLOG_FMT_SYSLOG)
        _emit_backtrace_syslog_lines(&lg->buf, lg->shared, job->level, &job->ts,
                                     job->bt_syms, job->bt_depth);

      _clog_size_rotate_if_due(lg->shared);
    }
    mutex_unlock(lg->shared->mutex);

    _clog_async_job_release(lg->shared, envelope);
  }
}

#ifdef RUNNING_UNIT_TESTS
/*
 * Lets a test deterministically force the NEXT _clog_flush_pinned() call's
 * own mutex_init()/cond_var_init() on its stack-local clog_async_ctrl_t to
 * fail, without depending on pthread_mutex_init()/pthread_cond_init() ever
 * actually failing for real (with default/NULL attributes, glibc's own
 * implementation has no real failure path to trigger portably from a test).
 * Each hook auto-disarms itself the moment it fires, so only the one call a
 * test is targeting is affected.
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
 * Core of clog_flush(), factored out so it can also be called from
 * _clog_write()'s own FATAL path against an lg the caller has ALREADY
 * resolved/pinned; this function does no resolve/unpin of its own, and
 * assumes lg->shared->async_enabled is already known true by the caller.
 * The pin is held for this entire call, including the blocking wait below,
 * mirroring how _clog_write()/_clog_write_async() already hold their own
 * pin across their own blocking circq_send_zc/dynmq_send_zc call; this is
 * what lets clog_close()'s poll-wait naturally serialize against an
 * in-flight flush the same way it already does for an in-flight write.
 */
static void _clog_flush_pinned(struct clogger *lg) {
  clog_async_ctrl_t ctrl; /* stack-scoped, per-call; mutex_init/cond_var_init
      on a fresh instance each time, never a static/constant initializer,
      per this file's own standing pthread-wrapper rule */
  /* Both checked, unlike an ordinary "this basically never fails" pthread
   * init elsewhere in this file: proceeding to mutex_lock()/cond_var_wait()
   * on a NOT-fully-initialized mutex/condvar on the strength of "it almost
   * certainly succeeded" would be undefined behavior, not a graceful
   * degradation, and this function's own envelope must never be handed to
   * the writer thread in that case; there would be no correctly-initialized
   * ctrl.mutex/ctrl.cv left for it to safely lock/broadcast on the other end.
   * Failing this open (treat it as nothing having been enqueued, exactly the
   * existing "vanishingly unlikely enqueue failure" branch below already
   * does) is also what keeps this function's other caller, _clog_write()'s
   * own FATAL path, from ever hanging indefinitely on a call meant to
   * terminate the process. */
  if (_clog_test_consume_forced_flush_mutex_init_failure() ||
      mutex_init(ctrl.mutex) != 0)
    return;
  if (_clog_test_consume_forced_flush_condvar_init_failure() ||
      cond_var_init(ctrl.cv) != 0) {
    mutex_destroy(ctrl.mutex);
    return;
  }
  ctrl.done = false;

  clog_async_msg_t envelope;
  envelope.kind = CLOG_ASYNC_MSG_FLUSH;
  envelope.u.ctrl = &ctrl;
  c_message_t m = {.data = &envelope, .size = sizeof envelope};
  ccol_retval_t rv = lg->shared->is_bounded_queue
                         ? circq_send_zc(lg->shared->q.circq, &m)
                         : dynmq_send_zc(lg->shared->q.dynmq, &m);

  if (rv == ccol_success) {
    mutex_lock(ctrl.mutex);
    while (!ctrl.done) cond_var_wait(ctrl.cv, ctrl.mutex);
    mutex_unlock(ctrl.mutex);
  }
  /* else: the vanishingly unlikely ccol_not_permitted/ccol_not_enough_memory
   * failure path (sending already disabled, or the unbounded queue's own
   * node allocation failing); nothing was ever enqueued for the writer
   * thread to find and acknowledge, so ctrl.done can never become true;
   * skip the wait entirely rather than hang forever. This matters beyond
   * clog_flush() itself: this same function is also _clog_write()'s own
   * FATAL-path pre-flush step, and an unconditional wait here could hang a
   * process indefinitely instead of terminating after a fatal error. */

  mutex_destroy(ctrl.mutex);
  cond_var_destroy(ctrl.cv);
}

void clog_flush(clog h) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err("clog_flush: clog handle is invalid, stale, or already closed");
  if (lg->shared->async_enabled) _clog_flush_pinned(lg);
  _clog_resolve_unpin(lg);
}

void _clog_write(clog h, clog_level_t level, const char *file, int line,
                 const char *func, bool with_backtrace, const char *fmt, ...) {
  struct clogger *lg = _clog_resolve(h);
  if (!lg)
    fatal_err("_clog_write: clog handle is invalid, stale, or already closed");

  /* Fast path: most log_* call sites at a filtered-out level (e.g.
   * log_trace/log_debug once a logger is running at CLOG_INFO or above) hit
   * this on every call. Checking min_level here, before doing anything
   * else, lets a disabled call return without ever contending for
   * shared->mutex (sync path) or paying the cost of formatting/snapshotting
   * a message no one will ever see (async path). A relaxed load is enough:
   * this is purely an optimization to skip the work for the common case,
   * not the authoritative decision for the synchronous path (see
   * _clog_write_sync()'s own locked re-check); the async path has no
   * second, more authoritative check of its own, so a message it has
   * already queued by the time a concurrent clog_set_level() takes effect
   * is delivered anyway, the same kind of rare boundary-line race this
   * file's own README already documents as intentional for the synchronous
   * path's unlocked fast check. CLOG_FATAL always bypasses the filter. */
  if (level < atomic_load_explicit(&lg->min_level, memory_order_relaxed) &&
      level != CLOG_FATAL) {
    _clog_resolve_unpin(lg);
    return;
  }

  /* Captured HERE, in this function's own frame, uniformly for every path
   * below; see _capture_backtrace()'s own doc comment for why this must
   * never be captured one level deeper (inside _clog_write_sync()/
   * _clog_write_async()), which would silently skip an extra frame. */
  char **bt_syms = NULL;
  int bt_depth = 0;
  if (with_backtrace) _capture_backtrace(&bt_syms, &bt_depth);

  va_list ap;
  va_start(ap, fmt);

  /* lg->shared->async_enabled is read here without a lock: it is written
   * exactly once, either at construction (before this shared object is
   * ever published to any reader) or by _clog_atfork_child()'s own
   * downgrade (run by the only thread that exists in a freshly forked
   * child, strictly before any other code in that child could read it
   * concurrently); never mutated while any other thread could be reading
   * it, so no lock is needed for this read. */
  if (lg->shared->async_enabled) {
    if (level == CLOG_FATAL) {
      /* Drain anything already queued from earlier, non-fatal calls BEFORE
       * this fatal record is written: without this, a message logged
       * moments before a fatal error, still sitting unflushed in the
       * queue, would be silently lost the instant exit() runs a few lines
       * below. This cannot deadlock against the writer thread:
       * _clog_flush_pinned() only ever waits on its own private,
       * stack-local ctrl.mutex/ctrl.cv, never shared->mutex, and fully
       * completes before _clog_write_sync() below ever touches
       * shared->mutex itself. */
      _clog_flush_pinned(lg);
    } else {
      _clog_write_async(lg, level, file, line, func, with_backtrace, bt_syms,
                        bt_depth, fmt, ap);
      va_end(ap);
      _clog_resolve_unpin(lg);
      return;
    }
  }

  /* FATAL, or async not configured: delegate to the synchronous body,
   * passing ap straight through; va_end(ap) must come AFTER this call,
   * not before, since ap is still live and about to be used here. */
  _clog_write_sync(lg, level, file, line, func, with_backtrace, bt_syms,
                   bt_depth, fmt, ap);
  va_end(ap);
  if (bt_syms)
    free(bt_syms); /* the sync path's own responsibility to free
what was captured above; _clog_write_sync() only reads bt_syms, it
never frees it */
  _clog_resolve_unpin(lg);
  if (level == CLOG_FATAL) exit(EXIT_FAILURE);
}
