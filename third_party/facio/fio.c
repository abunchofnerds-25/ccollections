/* *****************************************************************************
Copyright: Boaz Segev, 2018-2019
License: MIT

Feel free to copy, use and enjoy according to the license provided.
***************************************************************************** */

#include <fio.h>

#define FIO_INCLUDE_STR
#include <fio.h>

#define FIO_FORCE_MALLOC_TMP 1
#define FIO_INCLUDE_LINKED_LIST
#include <arpa/inet.h>
#include <clogger.h>
#include <common.h>
#include <ctype.h>
#include <errno.h>
#include <fio.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#if HAVE_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

/* force poll for testing? */
#ifndef FIO_ENGINE_POLL
#define FIO_ENGINE_POLL 0
#endif

#if !FIO_ENGINE_POLL && !FIO_ENGINE_EPOLL && !FIO_ENGINE_KQUEUE
#if defined(__linux__)
#define FIO_ENGINE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__bsdi__) || defined(__DragonFly__)
#define FIO_ENGINE_KQUEUE 1
#else
#define FIO_ENGINE_POLL 1
#endif
#endif

/* for kqueue and epoll only */
#ifndef FIO_POLL_MAX_EVENTS
#define FIO_POLL_MAX_EVENTS 64
#endif

#ifndef FIO_POLL_TICK
#define FIO_POLL_TICK 1000
#endif

#ifndef FIO_USE_URGENT_QUEUE
#define FIO_USE_URGENT_QUEUE 1
#endif

#ifndef DEBUG_SPINLOCK
#define DEBUG_SPINLOCK 0
#endif

/* Slowloris mitigation  (must be less than 1<<16) */
#ifndef FIO_SLOWLORIS_LIMIT
#define FIO_SLOWLORIS_LIMIT (1 << 10)
#endif

#if !defined(__clang__) && !defined(__GNUC__)
#define __thread _Thread_value
#endif

#ifndef FIO_TLS_WEAK
#define FIO_TLS_WEAK __attribute__((weak))
#endif

/* Mitigates MAP_ANONYMOUS not being defined on older versions of MacOS */
#if !defined(MAP_ANONYMOUS)
#if defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0
#endif
#endif

/* *****************************************************************************
Event deferring (declarations)
***************************************************************************** */

static void deferred_on_close(void *uuid_, void *pr_);
static void deferred_on_shutdown(void *arg, void *arg2);
static void deferred_on_ready(void *arg, void *arg2);
static void deferred_on_data(void *uuid, void *arg2);
static void deferred_ping(void *arg, void *arg2);

/* *****************************************************************************
Section Start Marker











                       Main State Machine Data Structures












***************************************************************************** */

typedef void (*fio_uuid_link_fn)(void *);
#define FIO_SET_NAME fio_uuid_links
#define FIO_SET_OBJ_TYPE fio_uuid_link_fn
#define FIO_SET_OBJ_COMPARE(o1, o2) 1
#include <fio.h>

/** User-space socket buffer data */
typedef struct fio_packet_s fio_packet_s;
struct fio_packet_s {
  fio_packet_s *next;
  int (*write_func)(int fd, struct fio_packet_s *packet);
  void (*dealloc)(void *buffer);
  union {
    void *buffer;
    intptr_t fd;
  } data;
  uintptr_t offset;
  uintptr_t length;
};

/** Connection data (fd_data) */
typedef struct {
  /* current data to be send */
  fio_packet_s *packet;
  /** the last packet in the queue. */
  fio_packet_s **packet_last;
  /* Data sent so far */
  size_t sent;
  /* fd protocol */
  fio_protocol_s *protocol;
  /* timer handler */
  time_t active;
  /** The number of pending packets that are in the queue. */
  uint16_t packet_count;
  /* timeout settings */
  uint8_t timeout;
  /* indicates that the fd should be considered scheduled (added to poll) */
  fio_lock_i scheduled;
  /* protocol lock */
  fio_lock_i protocol_lock;
  /* used to convert `fd` to `uuid` and validate connections */
  uint8_t counter;
  /* socket lock */
  fio_lock_i sock_lock;
  /** Connection is open */
  uint8_t open;
  /** indicated that the connection should be closed. */
  uint8_t close;
  /** peer address length */
  uint8_t addr_len;
  /** peer address length */
  uint8_t addr[48];
  /** RW hooks. */
  fio_rw_hook_s *rw_hooks;
  /** RW udata. */
  void *rw_udata;
  /** Number of external calls (fio_read / before_close) currently mid-call
   * with a snapshot of rw_hooks/rw_udata taken outside of sock_lock. Guards
   * against fio_clear_fd freeing rw_udata (e.g. a TLS SSL/BIO object) while
   * one of those calls is still using it; see fio_read and fio_clear_fd. */
  uint16_t rw_busy;
  /** Set by fio_clear_fd when it had to defer the rw_hooks cleanup (and the
   * fd close) because rw_busy was non-zero; the last matching busy-release
   * finishes the job once rw_busy reaches zero. */
  uint8_t rw_cleanup_pending;
  /* Objects linked to the UUID */
  fio_uuid_links_s links;
} fio_fd_data_s;

typedef struct {
  struct timespec last_cycle;
  /* connection capacity */
  uint32_t capa;
  /* connections counted towards shutdown (NOT while running) */
  uint32_t connection_count;
  /* thread list */
  fio_ls_s thread_ids;
  /* active workers */
  uint16_t workers;
  /* timer handler */
  uint16_t threads;
  /* timeout review loop flag */
  uint8_t need_review;
  /* spinning down process */
  uint8_t volatile active;
  /* worker process flag - true also for single process */
  uint8_t is_worker;
  /* polling and global lock */
  fio_lock_i lock;
  /* The highest active fd with a protocol object. Every read and write of
   * this field must go through max_protocol_fd_lock: fio_clear_fd is called
   * concurrently, on different fds, from different threads under nothing
   * but that fd's own per-fd sock_lock, which does not serialize a
   * different fd's concurrent update to this shared field. Without a
   * dedicated lock, one thread's raise-and-scan-down sequence can be torn
   * by another thread's concurrent raise, silently lowering this value
   * below the true high-water mark - after which fio_review_timeout's idle
   * sweep and fio_worker_cleanup's shutdown loop both stop covering the
   * fd(s) above the clobbered value. */
  fio_lock_i max_protocol_fd_lock;
  uint32_t max_protocol_fd;
  /* timer handler */
  pid_t parent;
#if FIO_ENGINE_POLL
  struct pollfd *poll;
#endif
  fio_fd_data_s info[];
} fio_data_s;

static fio_data_s *fio_data = NULL;

/* Engine-level clog handle; protected by g_fio_logger_rwlock. */
static struct clogger *g_fio_logger = NULL;
static rw_lock_t g_fio_logger_rwlock;
/* g_fio_logger_rwlock used to carry a static PTHREAD_RWLOCK_INITIALIZER
 * initializer; converted to lazy, call_once-guarded runtime init (see
 * _fio_logger_init_globals), mirroring the identical treatment already
 * applied to src/cfio_engine.c's and src/chttpclient.c's own mutex/condvar
 * pairs, for the same reason: a non-pthread backend's equivalent primitive
 * may need real setup work a compile-time constant can't provide. Every
 * function below that touches this rwlock calls
 * call_once(g_fio_logger_globals_once, ...) as its first statement. */
static once_flag_t g_fio_logger_globals_once = ONCE_INIT;
static void _fio_logger_init_globals(void) {
  rw_lock_init(g_fio_logger_rwlock);
}

void fio_set_logger(struct clogger *cl) {
  call_once(g_fio_logger_globals_once, _fio_logger_init_globals);
  rw_lock_wrlock(g_fio_logger_rwlock);
  struct clogger *old = g_fio_logger;
  g_fio_logger = cl;
  rw_lock_unlock(g_fio_logger_rwlock);
  /* Close outside the lock so _fio_vlog readers cannot observe a freed
   * pointer: the write lock above waited for all active readers to drain
   * before the swap, so no reader can hold a reference to 'old' now. */
  if (old) clog_close(old);
}

bool fio_has_logger(void) {
  call_once(g_fio_logger_globals_once, _fio_logger_init_globals);
  rw_lock_rdlock(g_fio_logger_rwlock);
  bool has = (g_fio_logger != NULL);
  rw_lock_unlock(g_fio_logger_rwlock);
  return has;
}

/* Internal helper: format into a stack buffer then dispatch via _clog_write. */
static void _fio_vlog(clog_level_t level, const char *file, int line,
                      const char *func, bool trace, const char *fmt,
                      va_list ap) {
  call_once(g_fio_logger_globals_once, _fio_logger_init_globals);
  rw_lock_rdlock(g_fio_logger_rwlock);
  struct clogger *cl = g_fio_logger;
  if (!cl) {
    rw_lock_unlock(g_fio_logger_rwlock);
    return;
  }
  char buf[FIO_LOG_LENGTH_LIMIT];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  _clog_write(cl, level, file, line, func, trace, "%s", buf);
  rw_lock_unlock(g_fio_logger_rwlock);
}

void __fio_log_debug(const char *file, int line, const char *func,
                     const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _fio_vlog(CLOG_DEBUG, file, line, func, false, fmt, ap);
  va_end(ap);
}

void __fio_log_info(const char *file, int line, const char *func,
                    const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _fio_vlog(CLOG_INFO, file, line, func, false, fmt, ap);
  va_end(ap);
}

void __fio_log_warn(const char *file, int line, const char *func,
                    const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _fio_vlog(CLOG_WARN, file, line, func, false, fmt, ap);
  va_end(ap);
}

void __fio_log_error(const char *file, int line, const char *func,
                     const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _fio_vlog(CLOG_ERROR, file, line, func, true, fmt, ap);
  va_end(ap);
}

void __fio_log_fatal(const char *file, int line, const char *func,
                     const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  _fio_vlog(CLOG_FATAL, file, line, func, true, fmt, ap);
  va_end(ap);
  /* _clog_write at CLOG_FATAL calls exit(EXIT_FAILURE) and never returns.
   * If g_fio_logger is NULL the vlog call above is a no-op; exit here so
   * the noreturn guarantee holds regardless. */
  exit(EXIT_FAILURE);
}

/* used for protocol locking by task type. */
typedef struct {
  fio_lock_i locks[3];
  unsigned rsv : 8;
} protocol_metadata_s;

/* used for accessing the protocol locking in a safe byte aligned way. */
union protocol_metadata_union_u {
  size_t opaque;
  protocol_metadata_s meta;
};

#define fd_data(fd) (fio_data->info[(uintptr_t)(fd)])
#define uuid_data(uuid) fd_data(fio_uuid2fd((uuid)))
#define fd2uuid(fd) \
  ((intptr_t)((((uintptr_t)(fd)) << 8) | fd_data((fd)).counter))

/**
 * Returns the maximum number of open files facil.io can handle per worker
 * process.
 *
 * Total OS limits might apply as well but aren't shown.
 *
 * The value of 0 indicates either that the facil.io library wasn't initialized
 * yet or that it's resources were released.
 */
size_t fio_capa(void) {
  if (fio_data) return fio_data->capa;
  return 0;
}

/* *****************************************************************************
Packet allocation (for socket's user-buffer)
***************************************************************************** */

static inline void fio_packet_free(fio_packet_s *packet) {
  packet->dealloc(packet->data.buffer);
  fio_free(packet);
}
static inline fio_packet_s *fio_packet_alloc(void) {
  fio_packet_s *packet = fio_malloc(sizeof(*packet));
  FIO_ASSERT_ALLOC(packet);
  return packet;
}

/* *****************************************************************************
Core Connection Data Clearing
***************************************************************************** */

/* Finishes an rw_hooks cleanup (+ the fd close it gates) that fio_clear_fd
 * had to defer because a fio_read / before_close call was still using
 * rw_hooks/rw_udata when it ran. Called both from fio_clear_fd itself (when
 * nothing was busy, so it can finish immediately) and from
 * fio_rw_busy_release (when the last busy caller finishes later). */
static inline void _fio_finalize_rw_cleanup(intptr_t fd,
                                            fio_rw_hook_s *rw_hooks,
                                            void *rw_udata) {
  if (rw_hooks && rw_hooks->cleanup) rw_hooks->cleanup(rw_udata);
}

/* resets connection data, marking it as either open or closed. Returns 1 if
 * the rw_hooks cleanup (and the caller's fd close) had to be deferred
 * because a fio_read / before_close call was still mid-use of rw_udata;
 * returns 0 if the caller may clean up / close(fd) immediately, as before. */
static inline int fio_clear_fd(intptr_t fd, uint8_t is_open) {
  fio_packet_s *packet;
  fio_protocol_s *protocol;
  fio_rw_hook_s *rw_hooks;
  void *rw_udata;
  uint16_t rw_busy;
  fio_uuid_links_s links;
  int deferred;
  fio_lock(&(fd_data(fd).sock_lock));
  links = fd_data(fd).links;
  packet = fd_data(fd).packet;
  protocol = fd_data(fd).protocol;
  rw_hooks = fd_data(fd).rw_hooks;
  rw_udata = fd_data(fd).rw_udata;
  rw_busy = fd_data(fd).rw_busy;
  fd_data(fd) = (fio_fd_data_s){
      .open = is_open,
      .sock_lock = fd_data(fd).sock_lock,
      .protocol_lock = fd_data(fd).protocol_lock,
      .rw_hooks = (fio_rw_hook_s *)&FIO_DEFAULT_RW_HOOKS,
      .counter = fd_data(fd).counter + 1,
      .packet_last = &fd_data(fd).packet,
  };
  deferred = rw_busy != 0;
  if (deferred) {
    /* A fio_read / before_close call elsewhere already snapshotted this
     * exact rw_hooks/rw_udata pair (outside of this lock, by design; see
     * fio_read) and is still mid-call using it. Freeing it now (or letting
     * the caller close(fd)) would race with that call; e.g. fio_tls_
     * cleanup's SSL_free landing under fio_tls_read's SSL_read. Restore
     * them so the busy call keeps a live target, and defer the actual
     * cleanup + fd close to fio_rw_busy_release, once rw_busy hits zero. */
    fd_data(fd).rw_hooks = rw_hooks;
    fd_data(fd).rw_udata = rw_udata;
    fd_data(fd).rw_busy = rw_busy;
    fd_data(fd).rw_cleanup_pending = 1;
  }
  fio_lock(&fio_data->max_protocol_fd_lock);
  if (fio_data->max_protocol_fd < fd) {
    fio_data->max_protocol_fd = fd;
  } else {
    while (fio_data->max_protocol_fd &&
           !fd_data(fio_data->max_protocol_fd).open)
      --fio_data->max_protocol_fd;
  }
  fio_unlock(&fio_data->max_protocol_fd_lock);
  fio_unlock(&(fd_data(fd).sock_lock));
  if (!deferred) _fio_finalize_rw_cleanup(fd, rw_hooks, rw_udata);
  while (packet) {
    fio_packet_s *tmp = packet;
    packet = packet->next;
    fio_packet_free(tmp);
  }
  if (fio_uuid_links_count(&links)) {
    FIO_SET_FOR_LOOP(&links, pos) {
      if (pos->hash) pos->obj((void *)pos->hash);
    }
  }
  fio_uuid_links_free(&links);
  if (protocol && protocol->on_close) {
    fio_defer(deferred_on_close, (void *)fd2uuid(fd), protocol);
  }
  FIO_LOG_DEBUG("FD %d re-initialized (state: %p-%s).", (int)fd,
                (void *)fd2uuid(fd), (is_open ? "open" : "closed"));
  return deferred;
}

/* Releases one "busy" use of rw_hooks/rw_udata previously registered by the
 * caller (fio_read or fio_force_close's before_close call) under sock_lock.
 * If this was the last busy user and fio_clear_fd deferred its cleanup +
 * fd close while waiting for it, performs that deferred work now.
 * rw_cleanup_pending is only ever set by fio_clear_fd as part of an actual
 * close, so closing the fd here whenever that flag is found set is always
 * correct, regardless of which caller happens to drain the last busy use. */
static void fio_rw_busy_release(intptr_t uuid) {
  intptr_t fd = fio_uuid2fd(uuid);
  fio_rw_hook_s *finalize_hooks = NULL;
  void *finalize_udata = NULL;
  fio_lock(&fd_data(fd).sock_lock);
  if (fd_data(fd).rw_busy) --fd_data(fd).rw_busy;
  if (!fd_data(fd).rw_busy && fd_data(fd).rw_cleanup_pending) {
    finalize_hooks = fd_data(fd).rw_hooks;
    finalize_udata = fd_data(fd).rw_udata;
    fd_data(fd).rw_hooks = (fio_rw_hook_s *)&FIO_DEFAULT_RW_HOOKS;
    fd_data(fd).rw_udata = NULL;
    fd_data(fd).rw_cleanup_pending = 0;
  }
  fio_unlock(&fd_data(fd).sock_lock);
  if (!finalize_hooks) return;
  _fio_finalize_rw_cleanup(fd, finalize_hooks, finalize_udata);
  close(fd);
#if FIO_ENGINE_POLL
  fio_poll_remove_fd(fd);
#endif
}

static inline void fio_force_close_in_poll(intptr_t uuid) {
  uuid_data(uuid).close = 2;
  fio_force_close(uuid);
}

/* *****************************************************************************
Protocol Locking and UUID validation
***************************************************************************** */

/* Macro for accessing the protocol locking / metadata. */
#define prt_meta(prt) (((union protocol_metadata_union_u *)(&(prt)->rsv))->meta)

/** locks a connection's protocol returns a pointer that need to be unlocked. */
inline static fio_protocol_s *protocol_try_lock(intptr_t fd,
                                                enum fio_protocol_lock_e type) {
  errno = 0;
  if (fio_trylock(&fd_data(fd).protocol_lock)) goto would_block;
  fio_protocol_s *pr = fd_data(fd).protocol;
  if (!pr) {
    fio_unlock(&fd_data(fd).protocol_lock);
    goto invalid;
  }
  if (fio_trylock(&prt_meta(pr).locks[type])) {
    fio_unlock(&fd_data(fd).protocol_lock);
    goto would_block;
  }
  fio_unlock(&fd_data(fd).protocol_lock);
  return pr;
would_block:
  errno = EWOULDBLOCK;
  return NULL;
invalid:
  errno = EBADF;
  return NULL;
}
/** See `fio_protocol_try_lock` for details. */
inline static void protocol_unlock(fio_protocol_s *pr,
                                   enum fio_protocol_lock_e type) {
  fio_unlock(&prt_meta(pr).locks[type]);
}

/** returns 1 if the UUID is valid and 0 if it isn't. */
#define uuid_is_valid(uuid)                            \
  ((intptr_t)(uuid) >= 0 &&                            \
   ((uint32_t)fio_uuid2fd((uuid))) < fio_data->capa && \
   ((uintptr_t)(uuid) & 0xFF) == uuid_data((uuid)).counter)

/* public API. */
fio_protocol_s *fio_protocol_try_lock(intptr_t uuid,
                                      enum fio_protocol_lock_e type) {
  if (!uuid_is_valid(uuid)) {
    errno = EBADF;
    return NULL;
  }
  return protocol_try_lock(fio_uuid2fd(uuid), type);
}

/* public API. */
void fio_protocol_unlock(fio_protocol_s *pr, enum fio_protocol_lock_e type) {
  protocol_unlock(pr, type);
}

/* *****************************************************************************
UUID validation and state
***************************************************************************** */

/* public API. */
intptr_t fio_fd2uuid(int fd) {
  if (fd < 0 || (size_t)fd >= fio_data->capa) return -1;
  if (!fd_data(fd).open) {
    fio_lock(&fd_data(fd).protocol_lock);
    fio_clear_fd(fd, 1);
    fio_unlock(&fd_data(fd).protocol_lock);
  }
  return fd2uuid(fd);
}

/* public API. */
int fio_is_valid(intptr_t uuid) { return uuid_is_valid(uuid); }

/* public API. */
int fio_is_closed(intptr_t uuid) {
  return !uuid_is_valid(uuid) || !uuid_data(uuid).open || uuid_data(uuid).close;
}

void fio_stop(void) {
  if (fio_data) fio_data->active = 0;
}

/* public API. */
int16_t fio_is_running(void) { return fio_data && fio_data->active; }

/* public API. */
struct timespec fio_last_tick(void) { return fio_data->last_cycle; }

#define touchfd(fd) fd_data((fd)).active = fio_data->last_cycle.tv_sec

/* public API. */
void fio_touch(intptr_t uuid) {
  if (uuid_is_valid(uuid)) touchfd(fio_uuid2fd(uuid));
}

/* public API. */
fio_str_info_s fio_peer_addr(intptr_t uuid) {
  if (fio_is_closed(uuid) || !uuid_data(uuid).addr_len)
    return (fio_str_info_s){.data = NULL, .len = 0, .capa = 0};
  return (fio_str_info_s){.data = (char *)uuid_data(uuid).addr,
                          .len = uuid_data(uuid).addr_len,
                          .capa = 0};
}

/* *****************************************************************************
Section Start Marker











                         Default Thread / Fork handler

                           And Concurrency Helpers












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
#pragma weak fio_thread_new
void *__attribute__((weak)) fio_thread_new(void *(*thread_func)(void *),
                                           void *arg) {
  thread_id_t *thread = fio_malloc(sizeof(*thread));
  FIO_ASSERT_ALLOC(thread);
  if (thread_create(*thread, thread_func, arg)) goto error;
  return thread;
error:
  fio_free(thread);
  return NULL;
}

/**
 * OVERRIDE THIS to replace the default pthread implementation.
 *
 * Accepts a pointer returned from `fio_thread_new` (should also free any
 * allocated memory) and joins the associated thread.
 *
 * Return value is ignored.
 */
#pragma weak fio_thread_join
int __attribute__((weak)) fio_thread_join(void *p_thr) {
  if (!p_thr || !(*((thread_id_t *)p_thr))) return -1;
  thread_join(*((thread_id_t *)p_thr));
  *((thread_id_t *)p_thr) = (thread_id_t)NULL;
  fio_free(p_thr);
  return 0;
}

/* *****************************************************************************
Suspending and renewing thread execution (signaling events)
***************************************************************************** */

#ifndef DEFER_THROTTLE
#define DEFER_THROTTLE 2097148UL
#endif
#ifndef FIO_DEFER_THROTTLE_LIMIT
#define FIO_DEFER_THROTTLE_LIMIT 134217472UL
#endif

/**
 * The polling throttling model will use pipes to suspend and resume threads...
 *
 * However, it seems the approach is currently broken, at least on macOS.
 * I don't know why.
 *
 * If polling is disabled, the progressive throttling model will be used.
 *
 * The progressive throttling makes concurrency and parallelism likely, but uses
 * progressive nano-sleep throttling system that is less exact.
 */
#ifndef FIO_DEFER_THROTTLE_POLL
#define FIO_DEFER_THROTTLE_POLL 0
#endif

typedef struct fio_thread_queue_s {
  fio_ls_embd_s node;
  int fd_wait;   /* used for weaiting (read signal) */
  int fd_signal; /* used for signalling (write) */
} fio_thread_queue_s;

fio_ls_embd_s fio_thread_queue = FIO_LS_INIT(fio_thread_queue);
fio_lock_i fio_thread_lock = FIO_LOCK_INIT;
static __thread fio_thread_queue_s fio_thread_data = {.fd_wait = -1,
                                                      .fd_signal = -1};

FIO_FUNC inline void fio_thread_make_suspendable(void) {
  if (fio_thread_data.fd_signal >= 0) return;
  int fd[2] = {0, 0};
  int ret = pipe(fd);
  FIO_ASSERT(ret == 0, "`pipe` failed.");
  FIO_ASSERT(fio_set_non_block(fd[0]) == 0,
             "(fio) couldn't set internal pipe to non-blocking mode.");
  FIO_ASSERT(fio_set_non_block(fd[1]) == 0,
             "(fio) couldn't set internal pipe to non-blocking mode.");
  fio_thread_data.fd_wait = fd[0];
  fio_thread_data.fd_signal = fd[1];
}

FIO_FUNC inline void fio_thread_cleanup(void) {
  if (fio_thread_data.fd_signal < 0) return;
  close(fio_thread_data.fd_wait);
  close(fio_thread_data.fd_signal);
  fio_thread_data.fd_wait = -1;
  fio_thread_data.fd_signal = -1;
}

/* suspend thread execution (might be resumed unexpectedly) */
FIO_FUNC void fio_thread_suspend(void) {
  fio_lock(&fio_thread_lock);
  fio_ls_embd_push(&fio_thread_queue, &fio_thread_data.node);
  fio_unlock(&fio_thread_lock);
  struct pollfd list = {
      .events = (POLLPRI | POLLIN),
      .fd = fio_thread_data.fd_wait,
  };
  if (poll(&list, 1, 5000) > 0) {
    /* thread was removed from the list through signal */
    uint64_t data;
    int r = read(fio_thread_data.fd_wait, &data, sizeof(data));
    (void)r;
  } else {
    /* remove self from list */
    fio_lock(&fio_thread_lock);
    fio_ls_embd_remove(&fio_thread_data.node);
    fio_unlock(&fio_thread_lock);
  }
}

/* wake up a single thread */
FIO_FUNC void fio_thread_signal(void) {
  fio_thread_queue_s *t;
  int fd = -2;
  fio_lock(&fio_thread_lock);
  t = (fio_thread_queue_s *)fio_ls_embd_shift(&fio_thread_queue);
  if (t) fd = t->fd_signal;
  fio_unlock(&fio_thread_lock);
  if (fd >= 0) {
    uint64_t data = 1;
    int r = write(fd, (void *)&data, sizeof(data));
    (void)r;
  } else if (fd == -1) {
    /* hardly the best way, but there's a thread sleeping on air */
    kill(getpid(), SIGCONT);
  }
}

/* wake up all threads */
FIO_FUNC void fio_thread_broadcast(void) {
  while (fio_ls_embd_any(&fio_thread_queue)) {
    fio_thread_signal();
  }
}

static size_t fio_poll(void);
/**
 * A thread entering this function should wait for new evennts.
 */
static void fio_defer_thread_wait(void) {
#if FIO_ENGINE_POLL
  fio_poll();
  return;
#endif
  if (FIO_DEFER_THROTTLE_POLL) {
    fio_thread_suspend();
  } else {
    /* keeps threads active (concurrent), but reduces performance */
    static __thread size_t static_throttle = 262143UL;
    fio_throttle_thread(static_throttle);
    if (fio_defer_has_queue())
      static_throttle = 1;
    else if (static_throttle < FIO_DEFER_THROTTLE_LIMIT)
      static_throttle = (static_throttle << 1);
  }
}

static inline void fio_defer_on_thread_start(void) {
  if (FIO_DEFER_THROTTLE_POLL) fio_thread_make_suspendable();
}
static inline void fio_defer_thread_signal(void) {
  if (FIO_DEFER_THROTTLE_POLL) fio_thread_signal();
}
static inline void fio_defer_on_thread_end(void) {
  if (FIO_DEFER_THROTTLE_POLL) {
    fio_thread_broadcast();
    fio_thread_cleanup();
  }
}

/* *****************************************************************************
Section Start Marker














                             Task Management

                  Task / Event schduling and execution















***************************************************************************** */

#ifndef DEFER_QUEUE_BLOCK_COUNT
#if UINTPTR_MAX <= 0xFFFFFFFF
/* Almost a page of memory on most 32 bit machines: ((4096/4)-8)/3 */
#define DEFER_QUEUE_BLOCK_COUNT 338
#else
/* Almost a page of memory on most 64 bit machines: ((4096/8)-8)/3 */
#define DEFER_QUEUE_BLOCK_COUNT 168
#endif
#endif

/* task node data */
typedef struct {
  void (*func)(void *, void *);
  void *arg1;
  void *arg2;
} fio_defer_task_s;

/* task queue block */
typedef struct fio_defer_queue_block_s fio_defer_queue_block_s;
struct fio_defer_queue_block_s {
  fio_defer_task_s tasks[DEFER_QUEUE_BLOCK_COUNT];
  fio_defer_queue_block_s *next;
  size_t write;
  size_t read;
  unsigned char state;
};

/* task queue object */
typedef struct { /* a lock for the state machine, used for multi-threading
                    support */
  fio_lock_i lock;
  /* current active block to pop tasks */
  fio_defer_queue_block_s *reader;
  /* current active block to push tasks */
  fio_defer_queue_block_s *writer;
  /* static, built-in, queue */
  fio_defer_queue_block_s static_queue;
} fio_task_queue_s;

/* the state machine - this holds all the data about the task queue and pool */
static fio_task_queue_s task_queue_normal = {
    .reader = &task_queue_normal.static_queue,
    .writer = &task_queue_normal.static_queue};

static fio_task_queue_s task_queue_urgent = {
    .reader = &task_queue_urgent.static_queue,
    .writer = &task_queue_urgent.static_queue};

/* *****************************************************************************
Internal Task API
***************************************************************************** */

#if DEBUG
static size_t fio_defer_count_alloc, fio_defer_count_dealloc;
#define COUNT_ALLOC fio_atomic_add(&fio_defer_count_alloc, 1)
#define COUNT_DEALLOC fio_atomic_add(&fio_defer_count_dealloc, 1)
#define COUNT_RESET                                      \
  do {                                                   \
    fio_defer_count_alloc = fio_defer_count_dealloc = 0; \
  } while (0)
#else
#define COUNT_ALLOC
#define COUNT_DEALLOC
#define COUNT_RESET
#endif

static inline void fio_defer_push_task_fn(fio_defer_task_s task,
                                          fio_task_queue_s *queue) {
  fio_lock(&queue->lock);

  /* test if full */
  if (queue->writer->state && queue->writer->write == queue->writer->read) {
    /* return to static buffer or allocate new buffer */
    if (queue->static_queue.state == 2) {
      queue->writer->next = &queue->static_queue;
    } else {
      queue->writer->next = fio_malloc(sizeof(*queue->writer->next));
      COUNT_ALLOC;
      if (!queue->writer->next) goto critical_error;
    }
    queue->writer = queue->writer->next;
    queue->writer->write = 0;
    queue->writer->read = 0;
    queue->writer->state = 0;
    queue->writer->next = NULL;
  }

  /* place task and finish */
  queue->writer->tasks[queue->writer->write++] = task;
  /* cycle buffer */
  if (queue->writer->write == DEFER_QUEUE_BLOCK_COUNT) {
    queue->writer->write = 0;
    queue->writer->state = 1;
  }
  fio_unlock(&queue->lock);
  return;

critical_error:
  fio_unlock(&queue->lock);
  FIO_ASSERT_ALLOC(NULL);
}

#define fio_defer_push_task(func_, arg1_, arg2_)                         \
  do {                                                                   \
    fio_defer_push_task_fn(                                              \
        (fio_defer_task_s){.func = func_, .arg1 = arg1_, .arg2 = arg2_}, \
        &task_queue_normal);                                             \
    fio_defer_thread_signal();                                           \
  } while (0)

#if FIO_USE_URGENT_QUEUE
#define fio_defer_push_urgent(func_, arg1_, arg2_)                     \
  fio_defer_push_task_fn(                                              \
      (fio_defer_task_s){.func = func_, .arg1 = arg1_, .arg2 = arg2_}, \
      &task_queue_urgent)
#else
#define fio_defer_push_urgent(func_, arg1_, arg2_) \
  fio_defer_push_task(func_, arg1_, arg2_)
#endif

static inline fio_defer_task_s fio_defer_pop_task(fio_task_queue_s *queue) {
  fio_defer_task_s ret = (fio_defer_task_s){.func = NULL};
  fio_defer_queue_block_s *to_free = NULL;
  /* lock the state machine, grab/create a task and place it at the tail */
  fio_lock(&queue->lock);

  /* empty? */
  if (queue->reader->write == queue->reader->read && !queue->reader->state)
    goto finish;
  /* collect task */
  ret = queue->reader->tasks[queue->reader->read++];
  /* cycle */
  if (queue->reader->read == DEFER_QUEUE_BLOCK_COUNT) {
    queue->reader->read = 0;
    queue->reader->state = 0;
  }
  /* did we finish the queue in the buffer? */
  if (queue->reader->write == queue->reader->read) {
    if (queue->reader->next) {
      to_free = queue->reader;
      queue->reader = queue->reader->next;
    } else {
      if (queue->reader != &queue->static_queue &&
          queue->static_queue.state == 2) {
        to_free = queue->reader;
        queue->writer = &queue->static_queue;
        queue->reader = &queue->static_queue;
      }
      queue->reader->write = queue->reader->read = queue->reader->state = 0;
    }
  }

finish:
  if (to_free == &queue->static_queue) {
    queue->static_queue.state = 2;
    queue->static_queue.next = NULL;
  }
  fio_unlock(&queue->lock);

  if (to_free && to_free != &queue->static_queue) {
    fio_free(to_free);
    COUNT_DEALLOC;
  }
  return ret;
}

/* same as fio_defer_clear_queue , just inlined */
static inline void fio_defer_clear_tasks_for_queue(fio_task_queue_s *queue) {
  fio_lock(&queue->lock);
  while (queue->reader) {
    fio_defer_queue_block_s *tmp = queue->reader;
    queue->reader = queue->reader->next;
    if (tmp != &queue->static_queue) {
      COUNT_DEALLOC;
      fio_free(tmp);
    }
  }
  queue->static_queue = (fio_defer_queue_block_s){.next = NULL};
  queue->reader = queue->writer = &queue->static_queue;
  fio_unlock(&queue->lock);
}

/**
 * Performs a single task from the queue, returning -1 if the queue was empty.
 */
static inline int fio_defer_perform_single_task_for_queue(
    fio_task_queue_s *queue) {
  fio_defer_task_s task = fio_defer_pop_task(queue);
  if (!task.func) return -1;
  task.func(task.arg1, task.arg2);
  return 0;
}

static inline void fio_defer_clear_tasks(void) {
  fio_defer_clear_tasks_for_queue(&task_queue_normal);
#if FIO_USE_URGENT_QUEUE
  fio_defer_clear_tasks_for_queue(&task_queue_urgent);
#endif
}

static void fio_defer_on_fork(void) {
  task_queue_normal.lock = FIO_LOCK_INIT;
#if FIO_USE_URGENT_QUEUE
  task_queue_urgent.lock = FIO_LOCK_INIT;
#endif
}

/* *****************************************************************************
External Task API
***************************************************************************** */

/** Defer an execution of a function for later. */
int fio_defer(void (*func)(void *, void *), void *arg1, void *arg2) {
  /* must have a task to defer */
  if (!func) goto call_error;
  fio_defer_push_task(func, arg1, arg2);
  return 0;

call_error:
  return -1;
}

/** Performs all deferred functions until the queue had been depleted. */
void fio_defer_perform(void) {
#if FIO_USE_URGENT_QUEUE
  while (fio_defer_perform_single_task_for_queue(&task_queue_urgent) == 0 ||
         fio_defer_perform_single_task_for_queue(&task_queue_normal) == 0);
#else
  while (fio_defer_perform_single_task_for_queue(&task_queue_normal) == 0);
#endif
  //   for (;;) {
  // #if FIO_USE_URGENT_QUEUE
  //     fio_defer_task_s task = fio_defer_pop_task(&task_queue_urgent);
  //     if (!task.func)
  //       task = fio_defer_pop_task(&task_queue_normal);
  // #else
  //     fio_defer_task_s task = fio_defer_pop_task(&task_queue_normal);
  // #endif
  //     if (!task.func)
  //       return;
  //     task.func(task.arg1, task.arg2);
  //   }
}

/** Returns true if there are deferred functions waiting for execution. */
int fio_defer_has_queue(void) {
#if FIO_USE_URGENT_QUEUE
  return task_queue_urgent.reader != task_queue_urgent.writer ||
         task_queue_urgent.reader->write != task_queue_urgent.reader->read ||
         task_queue_normal.reader != task_queue_normal.writer ||
         task_queue_normal.reader->write != task_queue_normal.reader->read;
#else
  return task_queue_normal.reader != task_queue_normal.writer ||
         task_queue_normal.reader->write != task_queue_normal.reader->read;
#endif
}

/* Thread pool task */
static void *fio_defer_cycle(void *ignr) {
  fio_defer_on_thread_start();
  for (;;) {
    fio_defer_perform();
    if (!fio_is_running()) break;
    fio_defer_thread_wait();
  }
  fio_defer_on_thread_end();
  return ignr;
}

/* thread pool type */
typedef struct {
  size_t thread_count;
  void *threads[];
} fio_defer_thread_pool_s;

/* joins a thread pool */
static void fio_defer_thread_pool_join(fio_defer_thread_pool_s *pool) {
  for (size_t i = 0; i < pool->thread_count; ++i) {
    fio_thread_join(pool->threads[i]);
  }
  fio_free(pool);
}

/* creates a thread pool */
static fio_defer_thread_pool_s *fio_defer_thread_pool_new(size_t count) {
  if (!count) count = 1;
  fio_defer_thread_pool_s *pool =
      fio_malloc(sizeof(*pool) + (count * sizeof(void *)));
  FIO_ASSERT_ALLOC(pool);
  pool->thread_count = count;
  for (size_t i = 0; i < count; ++i) {
    pool->threads[i] = fio_thread_new(fio_defer_cycle, NULL);
    if (!pool->threads[i]) {
      pool->thread_count = i;
      goto error;
    }
  }
  return pool;
error:
  /* Drain first so in-flight tasks finish before the process dies. */
  fio_stop();
  fio_defer_thread_pool_join(pool);
  FIO_LOG_FATAL("couldn't spawn threads for thread pool, attempting shutdown.");
}

/* *****************************************************************************
Section Start Marker









                                     Timers










***************************************************************************** */

typedef struct {
  fio_ls_embd_s node;
  struct timespec due;
  size_t interval; /*in ms */
  size_t repetitions;
  void (*task)(void *);
  void *arg;
  void (*on_finish)(void *);
} fio_timer_s;

static fio_ls_embd_s fio_timers = FIO_LS_INIT(fio_timers);

static fio_lock_i fio_timer_lock = FIO_LOCK_INIT;

/** Marks the current time as facil.io's cycle time */
static inline void fio_mark_time(void) {
  clock_gettime(CLOCK_REALTIME, &fio_data->last_cycle);
}

/** Calculates the due time for a task, given it's interval */
static struct timespec fio_timer_calc_due(size_t interval) {
  struct timespec now = fio_last_tick();
  if (interval >= 1000) {
    unsigned long long secs = interval / 1000;
    now.tv_sec += secs;
    interval -= secs * 1000;
  }
  now.tv_nsec += (interval * 1000000UL);
  if (now.tv_nsec >= 1000000000L) {
    now.tv_nsec -= 1000000000L;
    now.tv_sec += 1;
  }
  return now;
}

/** Returns the number of miliseconds until the next event, up to FIO_POLL_TICK
 */
static size_t fio_timer_calc_first_interval(void) {
  if (fio_defer_has_queue()) return 0;
  if (fio_ls_embd_is_empty(&fio_timers)) {
    return FIO_POLL_TICK;
  }
  struct timespec now = fio_last_tick();
  struct timespec due =
      FIO_LS_EMBD_OBJ(fio_timer_s, node, fio_timers.next)->due;
  if (due.tv_sec < now.tv_sec ||
      (due.tv_sec == now.tv_sec && due.tv_nsec <= now.tv_nsec))
    return 0;
  size_t interval = 1000L * (due.tv_sec - now.tv_sec);
  if (due.tv_nsec >= now.tv_nsec) {
    interval += (due.tv_nsec - now.tv_nsec) / 1000000L;
  } else {
    interval -= (now.tv_nsec - due.tv_nsec) / 1000000L;
  }
  if (interval > FIO_POLL_TICK) interval = FIO_POLL_TICK;
  return interval;
}

/* simple a<=>b if "a" is bigger a negative result is returned, eq == 0. */
static int fio_timer_compare(struct timespec a, struct timespec b) {
  if (a.tv_sec == b.tv_sec) {
    if (a.tv_nsec < b.tv_nsec) return 1;
    if (a.tv_nsec > b.tv_nsec) return -1;
    return 0;
  }
  if (a.tv_sec < b.tv_sec) return 1;
  return -1;
}

/** Places a timer in an ordered linked list. */
static void fio_timer_add_order(fio_timer_s *timer) {
  timer->due = fio_timer_calc_due(timer->interval);
  // fio_ls_embd_s *pos = &fio_timers;
  fio_lock(&fio_timer_lock);
  FIO_LS_EMBD_FOR(&fio_timers, node) {
    fio_timer_s *t2 = FIO_LS_EMBD_OBJ(fio_timer_s, node, node);
    if (fio_timer_compare(timer->due, t2->due) >= 0) {
      fio_ls_embd_push(node, &timer->node);
      goto finish;
    }
  }
  fio_ls_embd_push(&fio_timers, &timer->node);
finish:
  fio_unlock(&fio_timer_lock);
}

/** Performs a timer task and re-adds it to the queue (or cleans it up) */
static void fio_timer_perform_single(void *timer_, void *ignr) {
  fio_timer_s *timer = timer_;
  timer->task(timer->arg);
  if (!timer->repetitions || fio_atomic_sub(&timer->repetitions, 1))
    goto reschedule;
  if (timer->on_finish) timer->on_finish(timer->arg);
  fio_free(timer);
  return;
  (void)ignr;
reschedule:
  fio_timer_add_order(timer);
}

/** schedules all timers that are due to be performed. */
static void fio_timer_schedule(void) {
  struct timespec now = fio_last_tick();
  fio_lock(&fio_timer_lock);
  while (fio_ls_embd_any(&fio_timers) &&
         fio_timer_compare(
             FIO_LS_EMBD_OBJ(fio_timer_s, node, fio_timers.next)->due, now) >=
             0) {
    fio_ls_embd_s *tmp = fio_ls_embd_remove(fio_timers.next);
    fio_defer(fio_timer_perform_single, FIO_LS_EMBD_OBJ(fio_timer_s, node, tmp),
              NULL);
  }
  fio_unlock(&fio_timer_lock);
}

static void fio_timer_clear_all(void) {
  fio_lock(&fio_timer_lock);
  while (fio_ls_embd_any(&fio_timers)) {
    fio_timer_s *timer =
        FIO_LS_EMBD_OBJ(fio_timer_s, node, fio_ls_embd_pop(&fio_timers));
    if (timer->on_finish) timer->on_finish(timer->arg);
    fio_free(timer);
  }
  fio_unlock(&fio_timer_lock);
}

/* *****************************************************************************
Section Start Marker











                               Concurrency Helpers












***************************************************************************** */

volatile fio_lock_i fio_signal_set_flag = 0;
/* store old signal handlers to propegate signal handling */
static struct sigaction fio_old_sig_chld;
static struct sigaction fio_old_sig_pipe;
static struct sigaction fio_old_sig_term;
static struct sigaction fio_old_sig_int;

void fio_signal_handler_reset(void) {
  struct sigaction old;
  if (fio_signal_set_flag) return;
  fio_unlock(&fio_signal_set_flag);
  memset(&old, 0, sizeof(old));
  sigaction(SIGINT, &fio_old_sig_int, &old);
  sigaction(SIGTERM, &fio_old_sig_term, &old);
  sigaction(SIGPIPE, &fio_old_sig_pipe, &old);
  if (fio_old_sig_chld.sa_handler) sigaction(SIGCHLD, &fio_old_sig_chld, &old);
  memset(&fio_old_sig_int, 0, sizeof(fio_old_sig_int));
  memset(&fio_old_sig_term, 0, sizeof(fio_old_sig_term));
  memset(&fio_old_sig_pipe, 0, sizeof(fio_old_sig_pipe));
  memset(&fio_old_sig_chld, 0, sizeof(fio_old_sig_chld));
}

/**
 * Returns 1 if the current process is the master (root) process.
 *
 * Otherwise returns 0.
 */
int fio_is_master(void) {
  return fio_data->is_worker == 0 || fio_data->workers == 1;
}

static inline size_t fio_detect_cpu_cores(void) {
  ssize_t cpu_count = 0;
#ifdef _SC_NPROCESSORS_ONLN
  cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpu_count < 0) {
    FIO_LOG_WARNING("CPU core count auto-detection failed.");
    return 0;
  }
#else
  FIO_LOG_WARNING("CPU core count auto-detection failed.");
#endif
  return cpu_count;
}

/**
 * Returns the number of expected threads / processes to be used by facil.io.
 *
 * The pointers should start with valid values that match the expected threads /
 * processes values passed to `fio_run`.
 *
 * The data in the pointers will be overwritten with the result.
 */
void fio_expected_concurrency(int16_t *threads, int16_t *processes) {
  if (!threads || !processes) return;
  if (!*threads && !*processes) {
    /* both options set to 0 - default to cores*cores matrix */
    ssize_t cpu_count = fio_detect_cpu_cores();
#if FIO_CPU_CORES_LIMIT
    if (cpu_count > FIO_CPU_CORES_LIMIT) {
      static int print_cores_warning = 1;
      if (print_cores_warning) {
        FIO_LOG_WARNING(
            "Detected %zu cores. Capping auto-detection of cores to %zu.\n"
            "      Avoid this message by setting threads / workers manually.\n"
            "      To increase auto-detection limit, recompile with:\n"
            "             -DFIO_CPU_CORES_LIMIT=%zu",
            (size_t)cpu_count, (size_t)FIO_CPU_CORES_LIMIT, (size_t)cpu_count);
        print_cores_warning = 0;
      }
      cpu_count = FIO_CPU_CORES_LIMIT;
    }
#endif
    *threads = *processes = (int16_t)cpu_count;
    if (cpu_count > 3) {
      /* leave a core available for the kernel */
      --(*processes);
    }
  } else if (*threads < 0 || *processes < 0) {
    /* Set any option that is less than 0 be equal to cores/value */
    /* Set any option equal to 0 be equal to the other option in value */
    ssize_t cpu_count = fio_detect_cpu_cores();
    size_t thread_cpu_adjust = (*threads <= 0 ? 1 : 0);
    size_t worker_cpu_adjust = (*processes <= 0 ? 1 : 0);

    if (cpu_count > 0) {
      int16_t tmp = 0;
      if (*threads < 0)
        tmp = (int16_t)(cpu_count / (*threads * -1));
      else if (*threads == 0) {
        tmp = -1 * *processes;
        thread_cpu_adjust = 0;
      } else
        tmp = *threads;
      if (*processes < 0)
        *processes = (int16_t)(cpu_count / (*processes * -1));
      else if (*processes == 0) {
        *processes = -1 * *threads;
        worker_cpu_adjust = 0;
      }
      *threads = tmp;
      tmp = *processes;
      if (worker_cpu_adjust && (*processes * *threads) >= cpu_count &&
          cpu_count > 3) {
        /* leave a resources available for the kernel */
        --*processes;
      }
      if (thread_cpu_adjust && (*threads * tmp) >= cpu_count && cpu_count > 3) {
        /* leave a resources available for the kernel */
        --*threads;
      }
    }
  }

  /* make sure we have at least one process and at least one thread */
  if (*processes <= 0) *processes = 1;
  if (*threads <= 0) *threads = 1;
}

/* *****************************************************************************
Section Start Marker













                       Polling State Machine - epoll














***************************************************************************** */
#if FIO_ENGINE_EPOLL
#include <sys/epoll.h>

/**
 * Returns a C string detailing the IO engine selected during compilation.
 *
 * Valid values are "kqueue", "epoll" and "poll".
 */
char const *fio_engine(void) { return "epoll"; }

/* epoll tester, in and out */
static int evio_fd[3] = {-1, -1, -1};

static void fio_poll_close(void) {
  for (int i = 0; i < 3; ++i) {
    if (evio_fd[i] != -1) {
      close(evio_fd[i]);
      evio_fd[i] = -1;
    }
  }
}

static void fio_poll_init(void) {
  fio_poll_close();
  for (int i = 0; i < 3; ++i) {
    evio_fd[i] = epoll_create1(EPOLL_CLOEXEC);
    if (evio_fd[i] == -1) goto error;
  }
  for (int i = 1; i < 3; ++i) {
    struct epoll_event chevent = {
        .events = (EPOLLOUT | EPOLLIN),
        .data.fd = evio_fd[i],
    };
    if (epoll_ctl(evio_fd[0], EPOLL_CTL_ADD, evio_fd[i], &chevent) == -1)
      goto error;
  }
  return;
error:
  FIO_LOG_FATAL("couldn't initialize epoll.");
  fio_poll_close();
  exit(errno);
  return;
}

static inline int fio_poll_add2(int fd, uint32_t events, int ep_fd) {
  struct epoll_event chevent;
  int ret;
  do {
    errno = 0;
    chevent = (struct epoll_event){
        .events = events,
        .data.fd = fd,
    };
    ret = epoll_ctl(ep_fd, EPOLL_CTL_MOD, fd, &chevent);
    if (ret == -1 && errno == ENOENT) {
      errno = 0;
      chevent = (struct epoll_event){
          .events = events,
          .data.fd = fd,
      };
      ret = epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &chevent);
    }
  } while (errno == EINTR);

  return ret;
}

static inline void fio_poll_add_read(intptr_t fd) {
  fio_poll_add2(fd, (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                evio_fd[1]);
  return;
}

static inline void fio_poll_add_write(intptr_t fd) {
  fio_poll_add2(fd, (EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                evio_fd[2]);
  return;
}

static inline void fio_poll_add(intptr_t fd) {
  if (fio_poll_add2(fd, (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                    evio_fd[1]) == -1)
    return;
  fio_poll_add2(fd, (EPOLLOUT | EPOLLRDHUP | EPOLLHUP | EPOLLONESHOT),
                evio_fd[2]);
  return;
}

FIO_FUNC inline void fio_poll_remove_fd(intptr_t fd) {
  struct epoll_event chevent = {.events = (EPOLLOUT | EPOLLIN), .data.fd = fd};
  epoll_ctl(evio_fd[1], EPOLL_CTL_DEL, fd, &chevent);
  epoll_ctl(evio_fd[2], EPOLL_CTL_DEL, fd, &chevent);
}

static size_t fio_poll(void) {
  int timeout_millisec = fio_timer_calc_first_interval();
  struct epoll_event internal[2];
  struct epoll_event events[FIO_POLL_MAX_EVENTS];
  int total = 0;
  /* wait for events and handle them */
  int internal_count = epoll_wait(evio_fd[0], internal, 2, timeout_millisec);
  if (internal_count == 0) return internal_count;
  for (int j = 0; j < internal_count; ++j) {
    int active_count =
        epoll_wait(internal[j].data.fd, events, FIO_POLL_MAX_EVENTS, 0);
    if (active_count > 0) {
      for (int i = 0; i < active_count; i++) {
        /* A peer that closes its end shortly after writing its final bytes
         * routinely produces a combined EPOLLIN|EPOLLRDHUP (or |EPOLLHUP)
         * event; data still sitting in the kernel receive buffer, reported
         * in the same epoll_wait() return as the hangup. Treating any
         * non-IN/OUT bit as an unconditional "discard as error" (the
         * previous behaviour here) silently drops that already-delivered
         * data: the connection is force-closed before deferred_on_data ever
         * runs, so a still-unread response tail (or, on a listener-side
         * connection, a request tail) is lost even though the kernel
         * successfully delivered it. Dispatch on_data/on_ready for whichever
         * of IN/OUT are actually set first, exactly as for a clean event;
         * fio_read()'s own EOF/error handling (already relied on throughout
         * this file, e.g. a plain recv()==0 with no EPOLLRDHUP at all) then
         * correctly discovers and force-closes the connection once the
         * already-buffered data has actually been drained. Only fall back to
         * closing immediately here when the event carries neither IN nor
         * OUT; a pure error/hangup with nothing to read or write, where
         * there is no buffered data this could discard. */
        if (events[i].events & (EPOLLIN | EPOLLOUT)) {
          if (events[i].events & EPOLLOUT) {
            fio_defer_push_urgent(deferred_on_ready,
                                  (void *)fd2uuid(events[i].data.fd), NULL);
          }
          if (events[i].events & EPOLLIN)
            fio_defer_push_task(deferred_on_data,
                                (void *)fd2uuid(events[i].data.fd), NULL);
        } else {
          // pure error/hangup, nothing to read or write: disconnect (on_close)
          fio_force_close_in_poll(fd2uuid(events[i].data.fd));
        }
      }  // end for loop
      total += active_count;
    }
  }
  return total;
}

#endif
/* *****************************************************************************
Section Start Marker













                       Polling State Machine - kqueue














***************************************************************************** */
#if FIO_ENGINE_KQUEUE
#include <sys/event.h>

/**
 * Returns a C string detailing the IO engine selected during compilation.
 *
 * Valid values are "kqueue", "epoll" and "poll".
 */
char const *fio_engine(void) { return "kqueue"; }

static int evio_fd = -1;

static void fio_poll_close(void) { close(evio_fd); }

static void fio_poll_init(void) {
  fio_poll_close();
  evio_fd = kqueue();
  if (evio_fd == -1) {
    FIO_LOG_FATAL("couldn't open kqueue.\n");
    exit(errno);
  }
}

static inline void fio_poll_add_read(intptr_t fd) {
  struct kevent chevent[1];
  EV_SET(chevent, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT,
         0, 0, ((void *)fd));
  do {
    errno = 0;
    kevent(evio_fd, chevent, 1, NULL, 0, NULL);
  } while (errno == EINTR);
  return;
}

static inline void fio_poll_add_write(intptr_t fd) {
  struct kevent chevent[1];
  EV_SET(chevent, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT,
         0, 0, ((void *)fd));
  do {
    errno = 0;
    kevent(evio_fd, chevent, 1, NULL, 0, NULL);
  } while (errno == EINTR);
  return;
}

static inline void fio_poll_add(intptr_t fd) {
  struct kevent chevent[2];
  EV_SET(chevent, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT,
         0, 0, ((void *)fd));
  EV_SET(chevent + 1, fd, EVFILT_WRITE,
         EV_ADD | EV_ENABLE | EV_CLEAR | EV_ONESHOT, 0, 0, ((void *)fd));
  do {
    errno = 0;
    kevent(evio_fd, chevent, 2, NULL, 0, NULL);
  } while (errno == EINTR);
  return;
}

FIO_FUNC inline void fio_poll_remove_fd(intptr_t fd) {
  if (evio_fd < 0) return;
  struct kevent chevent[2];
  EV_SET(chevent, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  EV_SET(chevent + 1, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
  do {
    errno = 0;
    kevent(evio_fd, chevent, 2, NULL, 0, NULL);
  } while (errno == EINTR);
}

static size_t fio_poll(void) {
  if (evio_fd < 0) return -1;
  int timeout_millisec = fio_timer_calc_first_interval();
  struct kevent events[FIO_POLL_MAX_EVENTS] = {{0}};

  const struct timespec timeout = {
      .tv_sec = (timeout_millisec / 1000),
      .tv_nsec = ((timeout_millisec & (~1023UL)) * 1000000)};
  /* wait for events and handle them */
  int active_count =
      kevent(evio_fd, NULL, 0, events, FIO_POLL_MAX_EVENTS, &timeout);

  if (active_count > 0) {
    for (int i = 0; i < active_count; i++) {
      // test for event(s) type
      if (events[i].filter == EVFILT_WRITE) {
        fio_defer_push_urgent(deferred_on_ready,
                              ((void *)fd2uuid(events[i].udata)), NULL);
      } else if (events[i].filter == EVFILT_READ) {
        fio_defer_push_task(deferred_on_data, (void *)fd2uuid(events[i].udata),
                            NULL);
      }
      if (events[i].flags & (EV_EOF | EV_ERROR)) {
        fio_force_close_in_poll(fd2uuid(events[i].udata));
      }
    }
  } else if (active_count < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  return active_count;
}

#endif
/* *****************************************************************************
Section Start Marker













                       Polling State Machine - poll














***************************************************************************** */

#if FIO_ENGINE_POLL

/**
 * Returns a C string detailing the IO engine selected during compilation.
 *
 * Valid values are "kqueue", "epoll" and "poll".
 */
char const *fio_engine(void) { return "poll"; }

#define FIO_POLL_READ_EVENTS (POLLPRI | POLLIN)
#define FIO_POLL_WRITE_EVENTS (POLLOUT)

static void fio_poll_close(void) {}

static void fio_poll_init(void) {}

static inline void fio_poll_remove_fd(int fd) {
  fio_data->poll[fd].fd = -1;
  fio_data->poll[fd].events = 0;
}

static inline void fio_poll_add_read(int fd) {
  fio_data->poll[fd].fd = fd;
  fio_data->poll[fd].events |= FIO_POLL_READ_EVENTS;
}

static inline void fio_poll_add_write(int fd) {
  fio_data->poll[fd].fd = fd;
  fio_data->poll[fd].events |= FIO_POLL_WRITE_EVENTS;
}

static inline void fio_poll_add(int fd) {
  fio_data->poll[fd].fd = fd;
  fio_data->poll[fd].events = FIO_POLL_READ_EVENTS | FIO_POLL_WRITE_EVENTS;
}

static inline void fio_poll_remove_read(int fd) {
  fio_lock(&fio_data->lock);
  if (fio_data->poll[fd].events & FIO_POLL_WRITE_EVENTS)
    fio_data->poll[fd].events = FIO_POLL_WRITE_EVENTS;
  else {
    fio_poll_remove_fd(fd);
  }
  fio_unlock(&fio_data->lock);
}

static inline void fio_poll_remove_write(int fd) {
  fio_lock(&fio_data->lock);
  if (fio_data->poll[fd].events & FIO_POLL_READ_EVENTS)
    fio_data->poll[fd].events = FIO_POLL_READ_EVENTS;
  else {
    fio_poll_remove_fd(fd);
  }
  fio_unlock(&fio_data->lock);
}

/** returns non-zero if events were scheduled, 0 if idle */
static size_t fio_poll(void) {
  /* shrink fd poll range */
  size_t end = fio_data->capa;  // max_protocol_fd might break TLS
  size_t start = 0;
  struct pollfd *list = NULL;
  fio_lock(&fio_data->lock);
  while (start < end && fio_data->poll[start].fd == -1) ++start;
  while (start < end && fio_data->poll[end - 1].fd == -1) --end;
  if (start != end) {
    /* copy poll list for multi-threaded poll */
    list = fio_malloc(sizeof(struct pollfd) * end);
    memcpy(list + start, fio_data->poll + start,
           (sizeof(struct pollfd)) * (end - start));
  }
  fio_unlock(&fio_data->lock);

  int timeout = fio_timer_calc_first_interval();
  size_t count = 0;

  if (start == end) {
    fio_throttle_thread((timeout * 1000000UL));
  } else if (poll(list + start, end - start, timeout) == -1) {
    goto finish;
  }
  for (size_t i = start; i < end; ++i) {
    if (list[i].revents) {
      touchfd(i);
      ++count;
      if (list[i].revents & FIO_POLL_WRITE_EVENTS) {
        // FIO_LOG_DEBUG("Poll Write %zu => %p", i, (void *)fd2uuid(i));
        fio_poll_remove_write(i);
        fio_defer_push_urgent(deferred_on_ready, (void *)fd2uuid(i), NULL);
      }
      if (list[i].revents & FIO_POLL_READ_EVENTS) {
        // FIO_LOG_DEBUG("Poll Read %zu => %p", i, (void *)fd2uuid(i));
        fio_poll_remove_read(i);
        fio_defer_push_task(deferred_on_data, (void *)fd2uuid(i), NULL);
      }
      if (list[i].revents & (POLLHUP | POLLERR)) {
        // FIO_LOG_DEBUG("Poll Hangup %zu => %p", i, (void *)fd2uuid(i));
        fio_poll_remove_fd(i);
        fio_force_close_in_poll(fd2uuid(i));
      }
      if (list[i].revents & POLLNVAL) {
        // FIO_LOG_DEBUG("Poll Invalid %zu => %p", i, (void *)fd2uuid(i));
        fio_poll_remove_fd(i);
        fio_lock(&fd_data(i).protocol_lock);
        fio_clear_fd(i, 0);
        fio_unlock(&fd_data(i).protocol_lock);
      }
    }
  }
finish:
  fio_free(list);
  return count;
}

#endif /* FIO_ENGINE_POLL */

/* *****************************************************************************
Section Start Marker












                         IO Callbacks / Event Handling













***************************************************************************** */

/* *****************************************************************************
Mock Protocol Callbacks and Service Funcions
***************************************************************************** */
static void mock_on_ev(intptr_t uuid, fio_protocol_s *protocol) {
  (void)uuid;
  (void)protocol;
}

static void mock_on_data(intptr_t uuid, fio_protocol_s *protocol) {
  fio_suspend(uuid);
  (void)protocol;
}

static uint8_t mock_on_shutdown(intptr_t uuid, fio_protocol_s *protocol) {
  return 0;
  (void)protocol;
  (void)uuid;
}

static void mock_ping(intptr_t uuid, fio_protocol_s *protocol) {
  (void)protocol;
  fio_force_close(uuid);
}
static void mock_ping2(intptr_t uuid, fio_protocol_s *protocol) {
  (void)protocol;
  touchfd(fio_uuid2fd(uuid));
  if (uuid_data(uuid).timeout == 255) return;
  protocol->ping = mock_ping;
  uuid_data(uuid).timeout = 8;
  fio_close(uuid);
}

FIO_FUNC void mock_ping_eternal(intptr_t uuid, fio_protocol_s *protocol) {
  (void)protocol;
  fio_touch(uuid);
}

/* *****************************************************************************
Deferred event handlers - these tasks safely forward the events to the Protocol
***************************************************************************** */

static void deferred_on_close(void *uuid_, void *pr_) {
  fio_protocol_s *pr = pr_;
  if (pr->rsv) goto postpone;
  pr->on_close((intptr_t)uuid_, pr);
  return;
postpone:
  /* pr->rsv (one of the FIO_PR_LOCK_* bytes) is held by an in-flight task
   * elsewhere; most commonly a worker's deferred http_resume, still
   * waiting its turn on this same shared queue. Re-pushing immediately with
   * no backoff keeps this task (and every other thread that happens to pop
   * it) spinning at the front of task_queue_normal: fio_defer_perform's
   * while-loop never sees the queue empty, so it never returns, and the
   * owning fio_defer_cycle thread never reaches its fio_is_running() check;
   * under enough concurrent closes this starves the very task that would
   * clear pr->rsv, and also blocks that thread from ever noticing fio_stop().
   * fio_reschedule_thread() (already used the same way in fio_lock's own
   * spin-retry, just above) gives the real lock-holder a chance to run. */
  fio_reschedule_thread();
  fio_defer_push_task(deferred_on_close, uuid_, pr_);
}

static void deferred_on_shutdown(void *arg, void *arg2) {
  if (!uuid_data(arg).protocol) {
    return;
  }
  fio_protocol_s *pr = protocol_try_lock(fio_uuid2fd(arg), FIO_PR_LOCK_TASK);
  if (!pr) {
    if (errno == EBADF) return;
    goto postpone;
  }
  touchfd(fio_uuid2fd(arg));
  uint8_t r = pr->on_shutdown ? pr->on_shutdown((intptr_t)arg, pr) : 0;
  if (r) {
    if (r == 255) {
      uuid_data(arg).timeout = 0;
    } else {
      fio_atomic_add(&fio_data->connection_count, 1);
      uuid_data(arg).timeout = r;
    }
    pr->ping = mock_ping2;
    protocol_unlock(pr, FIO_PR_LOCK_TASK);
  } else {
    fio_atomic_add(&fio_data->connection_count, 1);
    uuid_data(arg).timeout = 8;
    pr->ping = mock_ping;
    protocol_unlock(pr, FIO_PR_LOCK_TASK);
    fio_close((intptr_t)arg);
  }
  return;
postpone:
  fio_defer_push_task(deferred_on_shutdown, arg, NULL);
  (void)arg2;
}

static void deferred_on_ready_usr(void *arg, void *arg2) {
  errno = 0;
  fio_protocol_s *pr = protocol_try_lock(fio_uuid2fd(arg), FIO_PR_LOCK_WRITE);
  if (!pr) {
    if (errno == EBADF) return;
    goto postpone;
  }
  pr->on_ready((intptr_t)arg, pr);
  protocol_unlock(pr, FIO_PR_LOCK_WRITE);
  return;
postpone:
  fio_defer_push_task(deferred_on_ready, arg, NULL);
  (void)arg2;
}

static void deferred_on_ready(void *arg, void *arg2) {
  errno = 0;
  if (fio_flush((intptr_t)arg) > 0 || errno == EWOULDBLOCK || errno == EAGAIN) {
    if (arg2)
      fio_defer_push_urgent(deferred_on_ready, arg, NULL);
    else
      fio_poll_add_write(fio_uuid2fd(arg));
    return;
  }
  if (!uuid_data(arg).protocol) {
    return;
  }

  fio_defer_push_task(deferred_on_ready_usr, arg, NULL);
}

static void deferred_on_data(void *uuid, void *arg2) {
  if (fio_is_closed((intptr_t)uuid)) {
    return;
  }
  if (!uuid_data(uuid).protocol) goto no_protocol;
  fio_protocol_s *pr = protocol_try_lock(fio_uuid2fd(uuid), FIO_PR_LOCK_TASK);
  if (!pr) {
    if (errno == EBADF) {
      return;
    }
    goto postpone;
  }
  fio_unlock(&uuid_data(uuid).scheduled);
  pr->on_data((intptr_t)uuid, pr);
  protocol_unlock(pr, FIO_PR_LOCK_TASK);
  if (!fio_trylock(&uuid_data(uuid).scheduled)) {
    fio_poll_add_read(fio_uuid2fd((intptr_t)uuid));
  }
  return;

postpone:
  if (arg2) {
    /* the event is being forced, so force rescheduling */
    fio_defer_push_task(deferred_on_data, (void *)uuid, (void *)1);
  } else {
    /* the protocol was locked, so there might not be any need for the event */
    fio_poll_add_read(fio_uuid2fd((intptr_t)uuid));
  }
  return;

no_protocol:
  /* a missing protocol might still want to invoke the RW hook flush */
  deferred_on_ready(uuid, arg2);
  return;
}

static void deferred_ping(void *arg, void *arg2) {
  if (!uuid_data(arg).protocol ||
      (uuid_data(arg).timeout &&
       (uuid_data(arg).timeout + uuid_data(arg).active >
        (fio_data->last_cycle.tv_sec)))) {
    return;
  }
  fio_protocol_s *pr = protocol_try_lock(fio_uuid2fd(arg), FIO_PR_LOCK_WRITE);
  if (!pr) goto postpone;
  pr->ping((intptr_t)arg, pr);
  protocol_unlock(pr, FIO_PR_LOCK_WRITE);
  return;
postpone:
  fio_defer_push_task(deferred_ping, arg, NULL);
  (void)arg2;
}

/* *****************************************************************************
Forcing / Suspending IO events
***************************************************************************** */

void fio_force_event(intptr_t uuid, enum fio_io_event ev) {
  if (!uuid_is_valid(uuid)) return;
  switch (ev) {
    case FIO_EVENT_ON_DATA:
      fio_trylock(&uuid_data(uuid).scheduled);
      fio_defer_push_task(deferred_on_data, (void *)uuid, (void *)1);
      break;
    case FIO_EVENT_ON_TIMEOUT:
      fio_defer_push_task(deferred_ping, (void *)uuid, NULL);
      break;
    case FIO_EVENT_ON_READY:
      fio_defer_push_urgent(deferred_on_ready, (void *)uuid, NULL);
      break;
  }
}

void fio_suspend(intptr_t uuid) {
  if (uuid_is_valid(uuid)) fio_trylock(&uuid_data(uuid).scheduled);
}

void fio_force_read_rearm(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) return;
  fio_poll_add_read(fio_uuid2fd(uuid));
}

void fio_force_write_rearm(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) return;
  fio_poll_add_write(fio_uuid2fd(uuid));
}

/* *****************************************************************************
Section Start Marker












                               IO Socket Layer

                     Read / Write / Accept / Connect / etc'













***************************************************************************** */

/* *****************************************************************************
Internal socket initialization functions
***************************************************************************** */

/**
Sets a socket to non blocking state.

This function is called automatically for the new socket, when using
`fio_accept` or `fio_connect`.
*/
int fio_set_non_block(int fd) {
/* If they have O_NONBLOCK, use the Posix way to do it */
#if defined(O_NONBLOCK)
  /* Fixme: O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5. */
  int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0))) flags = 0;
#ifdef O_CLOEXEC
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_CLOEXEC);
#else
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
#elif defined(FIONBIO)
  /* Otherwise, use the old way of doing it */
  static int flags = 1;
  return ioctl(fd, FIONBIO, &flags);
#else
#error No functions / argumnet macros for non-blocking sockets.
#endif
}

static void fio_tcp_addr_cpy(int fd, int family, struct sockaddr *addrinfo) {
  const char *result =
      inet_ntop(family,
                family == AF_INET
                    ? (void *)&(((struct sockaddr_in *)addrinfo)->sin_addr)
                    : (void *)&(((struct sockaddr_in6 *)addrinfo)->sin6_addr),
                (char *)fd_data(fd).addr, sizeof(fd_data(fd).addr));
  if (result) {
    fd_data(fd).addr_len = strlen((char *)fd_data(fd).addr);
  } else {
    fd_data(fd).addr_len = 0;
    fd_data(fd).addr[0] = 0;
  }
}

/**
 * `fio_accept` accepts a new socket connection from a server socket - see the
 * server flag on `fio_socket`.
 *
 * NOTE: this function does NOT attach the socket to the IO reactor -see
 * `fio_attach`.
 */
intptr_t fio_accept(intptr_t srv_uuid) {
  struct sockaddr_in6 addrinfo[2]; /* grab a slice of stack (aligned) */
  socklen_t addrlen = sizeof(addrinfo);
  int client;
#ifdef SOCK_NONBLOCK
  client = accept4(fio_uuid2fd(srv_uuid), (struct sockaddr *)addrinfo, &addrlen,
                   SOCK_NONBLOCK | SOCK_CLOEXEC);
  /* accept4/accept can legitimately return fd 0 (e.g. when the process was
   * started with stdin closed); only a negative return is a real failure.
   * A `<= 0` check here would silently leak the just-accepted fd 0. */
  if (client < 0) return -1;
#else
  client = accept(fio_uuid2fd(srv_uuid), (struct sockaddr *)addrinfo, &addrlen);
  if (client < 0) return -1;
  if (fio_set_non_block(client) == -1) {
    close(client);
    return -1;
  }
#endif
  // avoid the TCP delay algorithm.
  {
    int optval = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
  }
  // handle socket buffers.
  {
    int optval = 0;
    socklen_t size = (socklen_t)sizeof(optval);
    if (!getsockopt(client, SOL_SOCKET, SO_SNDBUF, &optval, &size) &&
        optval <= 131072) {
      optval = 131072;
      setsockopt(client, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval));
      optval = 131072;
      setsockopt(client, SOL_SOCKET, SO_RCVBUF, &optval, sizeof(optval));
    }
  }

  fio_lock(&fd_data(client).protocol_lock);
  fio_clear_fd(client, 1);
  /* fio_clear_fd zero-initializes fd_data(client), leaving `.active` at 0
   * until a protocol is attached (fio_attach) or the first successful
   * fio_read/fio_write touches it. Until then, fio_review_timeout's idle
   * check (`fd_data(fd).active + timeout >= review`) treats `active == 0`
   * as "idle since the epoch"; true on its very first sweep, regardless
   * of how recently the connection was actually accepted. For a connection
   * whose low-level rw_hooks have already been replaced (e.g. a TLS
   * handshake in progress or just completed) but which has no protocol
   * attached yet (attachment happens slightly later, e.g. after ALPN
   * negotiation), that first sweep's "no protocol, non-default rw_hooks"
   * branch calls fio_close() on a connection that isn't idle at all; it's
   * brand new. Touching it here starts the idle clock at accept time,
   * matching the invariant the review sweep is meant to enforce. */
  touchfd(client);
  fio_unlock(&fd_data(client).protocol_lock);
  /* copy peer address */
  if (((struct sockaddr *)addrinfo)->sa_family == AF_UNIX) {
    fd_data(client).addr_len = uuid_data(srv_uuid).addr_len;
    if (uuid_data(srv_uuid).addr_len) {
      memcpy(fd_data(client).addr, uuid_data(srv_uuid).addr,
             uuid_data(srv_uuid).addr_len + 1);
    }
  } else {
    fio_tcp_addr_cpy(client, ((struct sockaddr *)addrinfo)->sa_family,
                     (struct sockaddr *)addrinfo);
  }

  return fd2uuid(client);
}

/* Creates a Unix socket - returning it's uuid (or -1) */
static intptr_t fio_unix_socket(const char *address, uint8_t server) {
  /* Unix socket */
  struct sockaddr_un addr = {0};
  size_t addr_len = strlen(address);
  if (addr_len >= sizeof(addr.sun_path)) {
    FIO_LOG_ERROR("(fio_unix_socket) address too long (%zu bytes > %zu bytes).",
                  addr_len, sizeof(addr.sun_path) - 1);
    errno = ENAMETOOLONG;
    return -1;
  }
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, address, addr_len + 1); /* copy the NUL byte. */
#if defined(__APPLE__)
  addr.sun_len = addr_len;
#endif
  // get the file descriptor
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == -1) {
    return -1;
  }
  if (fio_set_non_block(fd) == -1) {
    close(fd);
    return -1;
  }
  if (server) {
    unlink(addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
      // perror("couldn't bind unix socket");
      close(fd);
      return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
      // perror("couldn't start listening to unix socket");
      close(fd);
      return -1;
    }
    /* chmod for foriegn connections */
    fchmod(fd, 0777);
  } else {
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1 &&
        errno != EINPROGRESS) {
      close(fd);
      return -1;
    }
  }
  fio_lock(&fd_data(fd).protocol_lock);
  fio_clear_fd(fd, 1);
  fio_unlock(&fd_data(fd).protocol_lock);
  if (addr_len < sizeof(fd_data(fd).addr)) {
    memcpy(fd_data(fd).addr, address, addr_len + 1); /* copy the NUL byte. */
    fd_data(fd).addr_len = addr_len;
  }
  return fd2uuid(fd);
}

/* Creates a TCP/IP socket - returning it's uuid (or -1) */
static intptr_t fio_tcp_socket(const char *address, const char *port,
                               uint8_t server) {
  /* TCP/IP socket */
  // setup the address
  struct addrinfo hints = {0};
  struct addrinfo *addrinfo;        // will point to the results
  memset(&hints, 0, sizeof hints);  // make sure the struct is empty
  hints.ai_family = AF_UNSPEC;      // don't care IPv4 or IPv6
  hints.ai_socktype = SOCK_STREAM;  // TCP stream sockets
  hints.ai_flags = AI_PASSIVE;      // fill in my IP for me
  if (getaddrinfo(address, port, &hints, &addrinfo)) {
    // perror("addr err");
    return -1;
  }
  // get the file descriptor
  int fd =
      socket(addrinfo->ai_family, addrinfo->ai_socktype, addrinfo->ai_protocol);
  /* socket() can legitimately return fd 0 (e.g. when the process was
   * started with stdin closed); only a negative return is a real failure.
   * A `<= 0` check here would silently leak the just-created fd 0. */
  if (fd < 0) {
    freeaddrinfo(addrinfo);
    return -1;
  }
  // make sure the socket is non-blocking
  if (fio_set_non_block(fd) < 0) {
    freeaddrinfo(addrinfo);
    close(fd);
    return -1;
  }
  if (server) {
    {
      // avoid the "address taken"
      int optval = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    }
    // bind the address to the socket
    int bound = 0;
    for (struct addrinfo *i = addrinfo; i != NULL; i = i->ai_next) {
      if (!bind(fd, i->ai_addr, i->ai_addrlen)) bound = 1;
    }
    if (!bound) {
      // perror("bind err");
      freeaddrinfo(addrinfo);
      close(fd);
      return -1;
    }
#ifdef TCP_FASTOPEN
    {
      // support TCP Fast Open when available
      int optval = 128;
      setsockopt(fd, addrinfo->ai_protocol, TCP_FASTOPEN, &optval,
                 sizeof(optval));
    }
#endif
    if (listen(fd, SOMAXCONN) < 0) {
      freeaddrinfo(addrinfo);
      close(fd);
      return -1;
    }
  } else {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    errno = 0;
    for (struct addrinfo *i = addrinfo; i; i = i->ai_next) {
      if (connect(fd, i->ai_addr, i->ai_addrlen) == 0 || errno == EINPROGRESS)
        goto socket_okay;
    }
    freeaddrinfo(addrinfo);
    close(fd);
    return -1;
  }
socket_okay:
  fio_lock(&fd_data(fd).protocol_lock);
  fio_clear_fd(fd, 1);
  fio_unlock(&fd_data(fd).protocol_lock);
  /* fio_tcp_addr_cpy expects a `struct sockaddr *` (it reads sin_addr/
   * sin6_addr straight out of it) but this line was instead handing it
   * `addrinfo` itself; a `struct addrinfo *`, an unrelated wrapper struct
   * with a completely different layout (ai_flags/ai_family/ai_socktype/...
   * before its ai_addr field, which is the actual `struct sockaddr *`).
   * Reinterpreting a struct addrinfo's bytes as a struct sockaddr_in6 reads
   * whatever ai_addrlen/ai_addr/ai_canonname/ai_next happen to hold as if
   * they were address octets; for AF_INET the (smaller) sockaddr_in
   * layout happened to overlap only with always-initialized int fields, so
   * this was silently wrong instead of crashing; for AF_INET6 it read past
   * into padding/uninitialized bytes, caught by valgrind
   * ("Conditional jump or move depends on uninitialised value(s)" inside
   * inet_ntop6) the first time any code path in this codebase actually
   * connected out over IPv6 via fio_socket. This corrupted the human-
   * readable peer-address string (fd_data(fd).addr, exposed publicly via
   * fio_peer_addr()) for every TCP socket (server or client) opened
   * through fio_tcp_socket, not just IPv6 ones. Fixed by passing
   * addrinfo->ai_addr, the actual struct sockaddr, exactly like fio_accept's
   * own (correct) call to fio_tcp_addr_cpy a few dozen lines above. */
  fio_tcp_addr_cpy(fd, addrinfo->ai_family, addrinfo->ai_addr);
  freeaddrinfo(addrinfo);
  return fd2uuid(fd);
}

/* PUBLIC API: opens a server or client socket */
intptr_t fio_socket(const char *address, const char *port, uint8_t server) {
  intptr_t uuid;
  if (port) {
    char *pos = (char *)port;
    int64_t n = fio_atol(&pos);
    /* make sure port is only numerical */
    if (*pos) {
      FIO_LOG_ERROR("(fio_socket) port %s is not a number.", port);
      errno = EINVAL;
      return -1;
    }
    /* a negative port number will revert to a Unix socket. */
    if (n <= 0) {
      if (n < -1)
        FIO_LOG_WARNING("(fio_socket) negative port number %s is ignored.",
                        port);
      port = NULL;
    }
  }
  if (!address && !port) {
    FIO_LOG_ERROR("(fio_socket) both address and port are missing or invalid.");
    errno = EINVAL;
    return -1;
  }
  if (!port) {
    do {
      errno = 0;
      uuid = fio_unix_socket(address, server);
    } while (errno == EINTR);
  } else {
    do {
      errno = 0;
      uuid = fio_tcp_socket(address, port, server);
    } while (errno == EINTR);
  }
  return uuid;
}

/* *****************************************************************************
Internal socket flushing related functions
***************************************************************************** */

#ifndef BUFFER_FILE_READ_SIZE
#define BUFFER_FILE_READ_SIZE 49152
#endif

#if !defined(USE_SENDFILE) && !defined(USE_SENDFILE_LINUX) && \
    !defined(USE_SENDFILE_BSD) && !defined(USE_SENDFILE_APPLE)
#if defined(__linux__) /* linux sendfile works  */
#define USE_SENDFILE_LINUX 1
#elif defined(__FreeBSD__) /* FreeBSD sendfile should work - not tested */
#define USE_SENDFILE_BSD 1
#elif defined(__APPLE__) /* Is the apple sendfile still broken? */
#define USE_SENDFILE_APPLE 2
#else /* sendfile might not be available - always set to 0 */
#define USE_SENDFILE 0
#endif

#endif

static void fio_sock_perform_close_fd(intptr_t fd) { close(fd); }

static inline void fio_sock_packet_rotate_unsafe(uintptr_t fd) {
  fio_packet_s *packet = fd_data(fd).packet;
  fd_data(fd).packet = packet->next;
  fio_atomic_sub(&fd_data(fd).packet_count, 1);
  if (!packet->next) {
    fd_data(fd).packet_last = &fd_data(fd).packet;
    fd_data(fd).packet_count = 0;
  } else if (&packet->next == fd_data(fd).packet_last) {
    fd_data(fd).packet_last = &fd_data(fd).packet;
  }
  fio_packet_free(packet);
}

static int fio_sock_write_buffer(int fd, fio_packet_s *packet) {
  int written = fd_data(fd).rw_hooks->write(
      fd2uuid(fd), fd_data(fd).rw_udata,
      ((uint8_t *)packet->data.buffer + packet->offset), packet->length);
  if (written > 0) {
    packet->length -= written;
    packet->offset += written;
    if (!packet->length) {
      fio_sock_packet_rotate_unsafe(fd);
    }
  }
  return written;
}

static int fio_sock_write_from_fd(int fd, fio_packet_s *packet) {
  ssize_t asked = 0;
  ssize_t sent = 0;
  ssize_t total = 0;
  char buff[BUFFER_FILE_READ_SIZE];
  do {
    packet->offset += sent;
    packet->length -= sent;
  retry:
    asked = pread(
        packet->data.fd, buff,
        ((packet->length < BUFFER_FILE_READ_SIZE) ? packet->length
                                                  : BUFFER_FILE_READ_SIZE),
        packet->offset);
    if (asked <= 0) goto read_error;
    sent = fd_data(fd).rw_hooks->write(fd2uuid(fd), fd_data(fd).rw_udata, buff,
                                       asked);
  } while (sent == asked && packet->length);
  if (sent >= 0) {
    packet->offset += sent;
    packet->length -= sent;
    total += sent;
    if (!packet->length) {
      fio_sock_packet_rotate_unsafe(fd);
      return 1;
    }
  }
  return total;

read_error:
  if (sent == 0) {
    fio_sock_packet_rotate_unsafe(fd);
    return 1;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) goto retry;
  return -1;
}

#if USE_SENDFILE_LINUX /* linux sendfile API */
#include <sys/sendfile.h>

static int fio_sock_sendfile_from_fd(int fd, fio_packet_s *packet) {
  ssize_t sent;
  sent =
      sendfile64(fd, packet->data.fd, (off_t *)&packet->offset, packet->length);
  if (sent < 0) return -1;
  packet->length -= sent;
  if (!packet->length) fio_sock_packet_rotate_unsafe(fd);
  return sent;
}

#elif USE_SENDFILE_BSD || USE_SENDFILE_APPLE /* FreeBSD / Apple API */
#include <sys/uio.h>

static int fio_sock_sendfile_from_fd(int fd, fio_packet_s *packet) {
  off_t act_sent = 0;
  ssize_t ret = 0;
  while (packet->length) {
    act_sent = packet->length;
#if USE_SENDFILE_APPLE
    ret = sendfile(packet->data.fd, fd, packet->offset, &act_sent, NULL, 0);
#else
    ret = sendfile(packet->data.fd, fd, packet->offset, (size_t)act_sent, NULL,
                   &act_sent, 0);
#endif
    if (ret < 0) goto error;
    packet->length -= act_sent;
    packet->offset += act_sent;
  }
  fio_sock_packet_rotate_unsafe(fd);
  return act_sent;
error:
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    packet->length -= act_sent;
    packet->offset += act_sent;
  }
  return -1;
}

#else
static int (*fio_sock_sendfile_from_fd)(int fd, fio_packet_s *packet) =
    fio_sock_write_from_fd;

#endif

/* *****************************************************************************
Socket / Connection Functions
***************************************************************************** */

/**
 * Returns the information available about the socket's peer address.
 *
 * If no information is available, the struct will be initialized with zero
 * (`addr == NULL`).
 * The information is only available when the socket was accepted using
 * `fio_accept` or opened using `fio_connect`.
 */

/* See the declarations in fio.h for the full contract. Mirrors exactly what
 * fio_read (below) does internally around its own read call: bump rw_busy
 * under sock_lock before the external operation, release it (via the
 * existing fio_rw_busy_release, which also finishes any close that
 * fio_clear_fd had to defer while busy) after. */
void fio_rw_busy_mark(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) return;
  fio_lock(&uuid_data(uuid).sock_lock);
  ++uuid_data(uuid).rw_busy;
  fio_unlock(&uuid_data(uuid).sock_lock);
}

void fio_rw_busy_unmark(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) return;
  fio_rw_busy_release(uuid);
}

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
ssize_t fio_read(intptr_t uuid, void *buffer, size_t count) {
  if (!uuid_is_valid(uuid) || !uuid_data(uuid).open) {
    errno = EBADF;
    return -1;
  }
  if (count == 0) return 0;
  fio_lock(&uuid_data(uuid).sock_lock);
  ssize_t (*rw_read)(intptr_t, void *, void *, size_t) =
      uuid_data(uuid).rw_hooks->read;
  void *udata = uuid_data(uuid).rw_udata;
  /* Mark rw_udata as busy atomically with this snapshot (same sock_lock
   * fio_clear_fd uses to decide whether it may free rw_udata immediately),
   * so a concurrent close can't free it out from under the call below. */
  ++uuid_data(uuid).rw_busy;
  fio_unlock(&uuid_data(uuid).sock_lock);
  int old_errno = errno;
  ssize_t ret;
retry_int:
  ret = rw_read(uuid, udata, buffer, count);
  if (ret < 0 && errno == EINTR) goto retry_int;
  fio_rw_busy_release(uuid);
  if (ret > 0) {
    fio_touch(uuid);
    return ret;
  }
  if (ret < 0 &&
      (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOTCONN)) {
    errno = old_errno;
    return 0;
  }
  fio_force_close(uuid);
  return -1;
}

/**
 * `fio_write2_fn` is the actual function behind the macro `fio_write2`.
 */
ssize_t fio_write2_fn(intptr_t uuid, fio_write_args_s options) {
  if (!uuid_is_valid(uuid)) goto error;

  /* create packet */
  fio_packet_s *packet = fio_packet_alloc();
  *packet = (fio_packet_s){
      .length = options.length,
      .offset = options.offset,
      .data.buffer = (void *)options.data.buffer,
  };
  if (options.is_fd) {
    packet->write_func = (uuid_data(uuid).rw_hooks == &FIO_DEFAULT_RW_HOOKS)
                             ? fio_sock_sendfile_from_fd
                             : fio_sock_write_from_fd;
    packet->dealloc =
        (options.after.dealloc ? options.after.dealloc
                               : (void (*)(void *))fio_sock_perform_close_fd);
  } else {
    packet->write_func = fio_sock_write_buffer;
    packet->dealloc = (options.after.dealloc ? options.after.dealloc : free);
  }
  /* add packet to outgoing list */
  uint8_t was_empty = 1;
  fio_lock(&uuid_data(uuid).sock_lock);
  if (!uuid_is_valid(uuid)) {
    goto locked_error;
  }
  if (uuid_data(uuid).packet) was_empty = 0;
  if (options.urgent == 0) {
    *uuid_data(uuid).packet_last = packet;
    uuid_data(uuid).packet_last = &packet->next;
  } else {
    fio_packet_s **pos = &uuid_data(uuid).packet;
    if (*pos) pos = &(*pos)->next;
    packet->next = *pos;
    *pos = packet;
    if (!packet->next) {
      uuid_data(uuid).packet_last = &packet->next;
    }
  }
  fio_atomic_add(&uuid_data(uuid).packet_count, 1);
  fio_unlock(&uuid_data(uuid).sock_lock);

  if (was_empty) {
    touchfd(fio_uuid2fd(uuid));
    deferred_on_ready((void *)uuid, (void *)1);
  }
  return 0;
locked_error:
  fio_unlock(&uuid_data(uuid).sock_lock);
  fio_packet_free(packet);
  errno = EBADF;
  return -1;
error:
  if (options.after.dealloc) {
    options.after.dealloc((void *)options.data.buffer);
  }
  errno = EBADF;
  return -1;
}

/**
 * Returns the number of `fio_write` calls that are waiting in the socket's
 * queue and haven't been processed.
 */
size_t fio_pending(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) return 0;
  return uuid_data(uuid).packet_count;
}

/**
 * `fio_close` marks the connection for disconnection once all the data was
 * sent. The actual disconnection will be managed by the `fio_flush` function.
 *
 * `fio_flash` will be automatically scheduled.
 */
void fio_close(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) {
    errno = EBADF;
    return;
  }
  if (uuid_data(uuid).packet || uuid_data(uuid).sock_lock) {
    uuid_data(uuid).close = 1;
    fio_force_event(uuid, FIO_EVENT_ON_READY);
    return;
  }
  fio_force_close(uuid);
}

/**
 * `fio_force_close` closes the connection immediately, without adhering to any
 * protocol restrictions and without sending any remaining data in the
 * connection buffer.
 */
void fio_force_close(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) {
    errno = EBADF;
    return;
  }
  // FIO_LOG_DEBUG("fio_force_close called for uuid %p", (void *)uuid);
  /* clear away any packets in case we want to cut the connection short. */
  fio_packet_s *packet = NULL;
  fio_lock(&uuid_data(uuid).sock_lock);
  packet = uuid_data(uuid).packet;
  uuid_data(uuid).packet = NULL;
  uuid_data(uuid).packet_last = &uuid_data(uuid).packet;
  uuid_data(uuid).sent = 0;
  fio_unlock(&uuid_data(uuid).sock_lock);
  while (packet) {
    fio_packet_s *tmp = packet;
    packet = packet->next;
    fio_packet_free(tmp);
  }
  /* Claim the right to run the rw-hooks termination callback (before_close)
   * atomically under sock_lock: only the thread that wins the 0/1 -> 2
   * transition runs it, and rw_busy is held for its duration exactly like
   * fio_read does, so a concurrent fio_clear_fd can't free rw_udata (e.g. a
   * TLS SSL/BIO object) out from under it. Without this lock, two threads
   * racing to close the same uuid (e.g. a read error on one thread and a
   * poll-detected hangup on another) could both see close & 1 and both
   * invoke before_close (SSL_shutdown) on the same connection at once.
   *
   * rw_busy != 0 here means a read (e.g. a worker thread inside fio_read,
   * per chttpserver's worker-driven body ingestion) is already in flight
   * against this same rw_udata. before_close (e.g. SSL_shutdown) is not
   * safe to run concurrently with that call; unlike fio_clear_fd, which
   * can safely defer freeing rw_udata until the busy count drains,
   * before_close needs to actually run its hook *now* to be useful, and
   * there is no safe way to defer just the hook call without blocking this
   * thread (which, when fio_force_close runs on the reactor thread, would
   * stall every other connection on it for as long as the other read
   * blocks). Skip the graceful before_close entirely in that case: the
   * connection still gets torn down immediately below via fio_clear_fd, it
   * just does so without sending a clean TLS close_notify; the same
   * outcome as any other abrupt disconnect (a crash, a network failure),
   * which every well-behaved peer already has to tolerate. */
  fio_rw_hook_s *hooks_for_before_close = NULL;
  void *udata_for_before_close = NULL;
  fio_lock(&uuid_data(uuid).sock_lock);
  if (!uuid_data(uuid).close) uuid_data(uuid).close = 1;
  if (uuid_data(uuid).open && (uuid_data(uuid).close & 1)) {
    uuid_data(uuid).close = 2; /* don't repeat the before_close callback */
    if (!uuid_data(uuid).rw_busy) {
      hooks_for_before_close = uuid_data(uuid).rw_hooks;
      udata_for_before_close = uuid_data(uuid).rw_udata;
      ++uuid_data(uuid).rw_busy;
    }
  }
  fio_unlock(&uuid_data(uuid).sock_lock);
  if (hooks_for_before_close) {
    int should_wait =
        hooks_for_before_close->before_close(uuid, udata_for_before_close);
    fio_rw_busy_release(uuid);
    if (should_wait) {
      fio_touch(uuid);
      fio_poll_add_write(fio_uuid2fd(uuid));
      return;
    }
  }
  fio_lock(&uuid_data(uuid).protocol_lock);
  int deferred = fio_clear_fd(fio_uuid2fd(uuid), 0);
  fio_unlock(&uuid_data(uuid).protocol_lock);
  if (!deferred) {
    close(fio_uuid2fd(uuid));
#if FIO_ENGINE_POLL
    fio_poll_remove_fd(fio_uuid2fd(uuid));
#endif
  }
  if (fio_data->connection_count)
    fio_atomic_sub(&fio_data->connection_count, 1);
}

/**
 * `fio_flush` attempts to write any remaining data in the internal buffer to
 * the underlying file descriptor and closes the underlying file descriptor once
 * if it's marked for closure (and all the data was sent).
 *
 * Return values: 1 will be returned if data remains in the buffer. 0
 * will be returned if the buffer was fully drained. -1 will be returned on an
 * error or when the connection is closed.
 */
ssize_t fio_flush(intptr_t uuid) {
  if (!uuid_is_valid(uuid)) goto invalid;
  errno = 0;
  ssize_t flushed = 0;
  int tmp;
  /* start critical section */
  if (fio_trylock(&uuid_data(uuid).sock_lock)) goto would_block;

  if (!uuid_data(uuid).packet) goto flush_rw_hook;

  const fio_packet_s *old_packet = uuid_data(uuid).packet;
  const size_t old_sent = uuid_data(uuid).sent;

  tmp = uuid_data(uuid).packet->write_func(fio_uuid2fd(uuid),
                                           uuid_data(uuid).packet);
  if (tmp <= 0) {
    goto test_errno;
  }
  /* Track real forward progress so the Slowloris check below has something
   * to compare against. Without this, `.sent` was only ever reset to 0 (in
   * fio_force_close) and never incremented, making `sent >= old_sent` a
   * tautology and collapsing the "did we make progress" guard entirely -
   * any connection with a deep write queue (packet_count >= the limit)
   * whose head packet needed more than one fio_flush call to drain (which
   * is ordinary TCP backpressure, not an attack) was killed as a
   * false-positive Slowloris attack. */
  uuid_data(uuid).sent += tmp;

  if (uuid_data(uuid).packet_count >= FIO_SLOWLORIS_LIMIT &&
      uuid_data(uuid).packet == old_packet &&
      uuid_data(uuid).sent >= old_sent &&
      (uuid_data(uuid).sent - old_sent) < 32768) {
    /* Slowloris attack assumed */
    goto attacked;
  }

  /* end critical section */
  fio_unlock(&uuid_data(uuid).sock_lock);

  /* test for fio_close marker */
  if (!uuid_data(uuid).packet && uuid_data(uuid).close) goto closed;

  /* return state */
  return uuid_data(uuid).open && uuid_data(uuid).packet != NULL;

would_block:
  errno = EWOULDBLOCK;
  return -1;

closed:
  fio_force_close(uuid);
  return -1;

flush_rw_hook:
  flushed = uuid_data(uuid).rw_hooks->flush(uuid, uuid_data(uuid).rw_udata);
  fio_unlock(&uuid_data(uuid).sock_lock);
  if (!flushed) return 0;
  if (flushed < 0) {
    goto test_errno;
  }
  touchfd(fio_uuid2fd(uuid));
  return 1;

test_errno:
  fio_unlock(&uuid_data(uuid).sock_lock);
  switch (errno) {
    case EWOULDBLOCK: /* fallthrough */
#if EWOULDBLOCK != EAGAIN
    case EAGAIN: /* fallthrough */
#endif
    case ENOTCONN:      /* fallthrough */
    case EINPROGRESS:   /* fallthrough */
    case ENOSPC:        /* fallthrough */
    case EADDRNOTAVAIL: /* fallthrough */
    case EINTR:
    case 0:
      return 1;
    case EFAULT:
      FIO_LOG_ERROR(
          "fio_flush EFAULT - possible memory address error sent to "
          "Unix socket.");
      /* fallthrough */
    case EPIPE:  /* fallthrough */
    case EIO:    /* fallthrough */
    case EINVAL: /* fallthrough */
    case EBADF:
      uuid_data(uuid).close = 1;
      fio_force_close(uuid);
      return -1;
  }
  FIO_LOG_DEBUG("UUID error: %p (%d): %s\n", (void *)uuid, errno,
                strerror(errno));
  return 0;

invalid:
  /* bad UUID */
  errno = EBADF;
  return -1;

attacked:
  /* don't close, just detach from facil.io and mark uuid as invalid.
   * Note: this intent only holds if fio_clear_fd finds rw_busy == 0 at this
   * exact moment. If some other call (e.g. a worker thread's fio_read) is
   * concurrently in flight against this same connection, fio_clear_fd
   * defers its cleanup, and fio_rw_busy_release *does* close(fd) once that
   * deferred cleanup finally runs; silently closing the fd anyway, purely
   * as a function of unrelated thread timing rather than anything about
   * this Slowloris path itself. Narrow window, and the connection is being
   * torn down either way, so this doesn't change the outcome for the
   * caller in practice; noted here since it does contradict the comment
   * above under that specific race. */
  FIO_LOG_WARNING("(facil.io) possible Slowloris attack from %.*s",
                  (int)fio_peer_addr(uuid).len, fio_peer_addr(uuid).data);
  fio_unlock(&uuid_data(uuid).sock_lock);
  fio_clear_fd(fio_uuid2fd(uuid), 0);
  return -1;
}

/* *****************************************************************************
Connection Read / Write Hooks, for overriding the system calls
***************************************************************************** */

static ssize_t fio_hooks_default_read(intptr_t uuid, void *udata, void *buf,
                                      size_t count) {
  return read(fio_uuid2fd(uuid), buf, count);
  (void)(udata);
}
static ssize_t fio_hooks_default_write(intptr_t uuid, void *udata,
                                       const void *buf, size_t count) {
  return write(fio_uuid2fd(uuid), buf, count);
  (void)(udata);
}

static ssize_t fio_hooks_default_before_close(intptr_t uuid, void *udata) {
  return 0;
  (void)udata;
  (void)uuid;
}

static ssize_t fio_hooks_default_flush(intptr_t uuid, void *udata) {
  return 0;
  (void)(uuid);
  (void)(udata);
}

static void fio_hooks_default_cleanup(void *udata) { (void)(udata); }

const fio_rw_hook_s FIO_DEFAULT_RW_HOOKS = {
    .read = fio_hooks_default_read,
    .write = fio_hooks_default_write,
    .flush = fio_hooks_default_flush,
    .before_close = fio_hooks_default_before_close,
    .cleanup = fio_hooks_default_cleanup,
};

static inline void fio_rw_hook_validate(fio_rw_hook_s *rw_hooks) {
  if (!rw_hooks->read) rw_hooks->read = fio_hooks_default_read;
  if (!rw_hooks->write) rw_hooks->write = fio_hooks_default_write;
  if (!rw_hooks->flush) rw_hooks->flush = fio_hooks_default_flush;
  if (!rw_hooks->before_close)
    rw_hooks->before_close = fio_hooks_default_before_close;
  if (!rw_hooks->cleanup) rw_hooks->cleanup = fio_hooks_default_cleanup;
}

/**
 * Replaces an existing read/write hook with another from within a read/write
 * hook callback.
 *
 * Does NOT call any cleanup callbacks.
 *
 * Returns -1 on error, 0 on success.
 */
int fio_rw_hook_replace_unsafe(intptr_t uuid, fio_rw_hook_s *rw_hooks,
                               void *udata) {
  int replaced = -1;
  uint8_t was_locked;
  intptr_t fd = fio_uuid2fd(uuid);
  fio_rw_hook_validate(rw_hooks);
  /* protect against some fulishness... but not all of it. */
  was_locked = fio_trylock(&fd_data(fd).sock_lock);
  if (uuid_is_valid(uuid)) {
    fd_data(fd).rw_hooks = rw_hooks;
    fd_data(fd).rw_udata = udata;
    replaced = 0;
  }
  if (!was_locked) fio_unlock(&fd_data(fd).sock_lock);
  return replaced;
}

/** Sets a socket hook state (a pointer to the struct). */
int fio_rw_hook_set(intptr_t uuid, fio_rw_hook_s *rw_hooks, void *udata) {
  if (fio_is_closed(uuid)) goto invalid_uuid;
  fio_rw_hook_validate(rw_hooks);
  intptr_t fd = fio_uuid2fd(uuid);
  fio_rw_hook_s *old_rw_hooks;
  void *old_udata;
  fio_lock(&fd_data(fd).sock_lock);
  if (fd2uuid(fd) != uuid) {
    fio_unlock(&fd_data(fd).sock_lock);
    goto invalid_uuid;
  }
  if (fd_data(fd).rw_busy) {
    /* A fio_read / before_close call elsewhere already snapshotted the
     * current rw_hooks/rw_udata (outside of this lock, by design; see
     * fio_read) and is still mid-call using them. Swapping hooks now would
     * be fine on its own (that in-flight call doesn't re-read this field),
     * but calling old_rw_hooks->cleanup(old_udata) below could free the
     * very udata (e.g. a TLS SSL/BIO object) that call is still using;
     * the same use-after-free fio_clear_fd's own rw_cleanup_pending defers
     * against. Unlike fio_clear_fd, there is no fd close happening here to
     * hang a deferred completion off of, so refuse the swap outright rather
     * than risk it; the only current caller (fio_tls_attach2uuid) runs
     * immediately after accept, before any read is possible, so this is not
     * expected to trigger in practice. */
    fio_unlock(&fd_data(fd).sock_lock);
    goto invalid_uuid;
  }
  old_rw_hooks = fd_data(fd).rw_hooks;
  old_udata = fd_data(fd).rw_udata;
  fd_data(fd).rw_hooks = rw_hooks;
  fd_data(fd).rw_udata = udata;
  fio_unlock(&fd_data(fd).sock_lock);
  if (old_rw_hooks && old_rw_hooks->cleanup) old_rw_hooks->cleanup(old_udata);
  return 0;
invalid_uuid:
  /* Per this function's contract (see fio.h): rw_hooks->cleanup is always
   * called, even on failure, so the caller's udata is never leaked just
   * because the hook couldn't be installed. */
  if (rw_hooks->cleanup) rw_hooks->cleanup(udata);
  return -1;
}

/* *****************************************************************************
Section Start Marker












                           IO Protocols and Attachment













***************************************************************************** */

/* *****************************************************************************
Setting the protocol
***************************************************************************** */

/* managing the protocol pointer array and the `on_close` callback */
static int fio_attach__internal(void *uuid_, void *protocol_) {
  intptr_t uuid = (intptr_t)uuid_;
  fio_protocol_s *protocol = (fio_protocol_s *)protocol_;
  if (protocol) {
    if (!protocol->on_close) {
      protocol->on_close = mock_on_ev;
    }
    if (!protocol->on_data) {
      protocol->on_data = mock_on_data;
    }
    if (!protocol->on_ready) {
      protocol->on_ready = mock_on_ev;
    }
    if (!protocol->ping) {
      protocol->ping = mock_ping;
    }
    if (!protocol->on_shutdown) {
      protocol->on_shutdown = mock_on_shutdown;
    }
    prt_meta(protocol) = (protocol_metadata_s){.rsv = 0};
  }
  if (!uuid_is_valid(uuid)) goto invalid_uuid_unlocked;
  fio_lock(&uuid_data(uuid).protocol_lock);
  if (!uuid_is_valid(uuid)) {
    goto invalid_uuid;
  }
  fio_protocol_s *old_pr = uuid_data(uuid).protocol;
  uuid_data(uuid).open = 1;
  uuid_data(uuid).protocol = protocol;
  touchfd(fio_uuid2fd(uuid));
  fio_unlock(&uuid_data(uuid).protocol_lock);
  if (old_pr) {
    /* protocol replacement */
    fio_defer_push_task(deferred_on_close, (void *)uuid, old_pr);
    if (!protocol) {
      /* hijacking */
      fio_poll_remove_fd(fio_uuid2fd(uuid));
      fio_poll_add_write(fio_uuid2fd(uuid));
    }
  } else if (protocol) {
    /* adding a new uuid to the reactor */
    fio_poll_add(fio_uuid2fd(uuid));
  }
  return 0;

invalid_uuid:
  fio_unlock(&uuid_data(uuid).protocol_lock);
invalid_uuid_unlocked:
  // FIO_LOG_DEBUG("fio_attach failed for invalid uuid %p", (void *)uuid);
  if (protocol) fio_defer_push_task(deferred_on_close, (void *)uuid, protocol);
  if (uuid == -1)
    errno = EBADF;
  else
    errno = ENOTCONN;
  return -1;
}

/**
 * Attaches (or updates) a protocol object to a socket UUID.
 * Returns -1 on error and 0 on success.
 */
void fio_attach(intptr_t uuid, fio_protocol_s *protocol) {
  fio_attach__internal((void *)uuid, protocol);
}

/** Sets a timeout for a specific connection (only when running and valid). */
void fio_timeout_set(intptr_t uuid, uint8_t timeout) {
  if (uuid_is_valid(uuid)) {
    touchfd(fio_uuid2fd(uuid));
    uuid_data(uuid).timeout = timeout;
  } else {
    FIO_LOG_DEBUG("Called fio_timeout_set for invalid uuid %p", (void *)uuid);
  }
}

/* *****************************************************************************
Core Callbacks for forking / starting up / cleaning up
***************************************************************************** */

typedef struct {
  fio_ls_embd_s node;
  void (*func)(void *);
  void *arg;
} callback_data_s;

typedef struct {
  fio_lock_i lock;
  fio_ls_embd_s callbacks;
} callback_collection_s;

static callback_collection_s callback_collection[FIO_CALL_NEVER + 1];

static void fio_state_on_idle_perform(void *task, void *arg) {
  ((void (*)(void *))(uintptr_t)task)(arg);
}

static inline void fio_state_callback_ensure(callback_collection_s *c) {
  if (c->callbacks.next) return;
  c->callbacks = (fio_ls_embd_s)FIO_LS_INIT(c->callbacks);
}

/** Adds a callback to the list of callbacks to be called for the event. */
void fio_state_callback_add(callback_type_e c_type, void (*func)(void *),
                            void *arg) {
  if (c_type == FIO_CALL_ON_INITIALIZE && fio_data) {
    func(arg);
    return;
  }
  if (!func || (int)c_type < 0 || c_type > FIO_CALL_NEVER) return;
  fio_lock(&callback_collection[c_type].lock);
  fio_state_callback_ensure(&callback_collection[c_type]);
  /* Deliberately plain malloc, not fio_malloc/procs: this can run (via
   * FIO_CALL_ON_INITIALIZE) from http_lib_constructor, which is invoked
   * before fio_lib_init/fio_mem_init has initialized the arena and before
   * chttpsvr_set_engine_mem_mgmt_procs could ever have been called; see
   * _fio_global_init in chttpserver.c. Freed with plain free() below and in
   * fio_state_callback_clear(), matching this allocator. */
  callback_data_s *tmp = malloc(sizeof(*tmp));
  FIO_ASSERT_ALLOC(tmp);
  *tmp = (callback_data_s){.func = func, .arg = arg};
  fio_ls_embd_push(&callback_collection[c_type].callbacks, &tmp->node);
  fio_unlock(&callback_collection[c_type].lock);
}

/** Removes a callback from the list of callbacks to be called for the event. */
int fio_state_callback_remove(callback_type_e c_type, void (*func)(void *),
                              void *arg) {
  if ((int)c_type < 0 || c_type > FIO_CALL_NEVER) return -1;
  fio_lock(&callback_collection[c_type].lock);
  FIO_LS_EMBD_FOR(&callback_collection[c_type].callbacks, pos) {
    callback_data_s *tmp = (FIO_LS_EMBD_OBJ(callback_data_s, node, pos));
    if (tmp->func == func && tmp->arg == arg) {
      fio_ls_embd_remove(&tmp->node);
      free(tmp); /* matches the plain malloc() in fio_state_callback_add */
      goto success;
    }
  }
  fio_unlock(&callback_collection[c_type].lock);
  return -1;
success:
  fio_unlock(&callback_collection[c_type].lock);
  return -0;
}

/** Forces all the existing callbacks to run, as if the event occurred. */
void fio_state_callback_force(callback_type_e c_type) {
  if ((int)c_type < 0 || c_type > FIO_CALL_NEVER) return;
  /* copy collection */
  fio_ls_embd_s copy = FIO_LS_INIT(copy);
  fio_lock(&callback_collection[c_type].lock);
  fio_state_callback_ensure(&callback_collection[c_type]);
  switch (c_type) { /* the difference between `unshift` and `push` */
    case FIO_CALL_ON_INITIALIZE: /* fallthrough */
    case FIO_CALL_PRE_START:     /* fallthrough */
    case FIO_CALL_BEFORE_FORK:   /* fallthrough */
    case FIO_CALL_AFTER_FORK:    /* fallthrough */
    case FIO_CALL_IN_CHILD:      /* fallthrough */
    case FIO_CALL_IN_MASTER:     /* fallthrough */
    case FIO_CALL_ON_START:      /* fallthrough */
      FIO_LS_EMBD_FOR(&callback_collection[c_type].callbacks, pos) {
        callback_data_s *tmp = fio_malloc(sizeof(*tmp));
        FIO_ASSERT_ALLOC(tmp);
        *tmp = *(FIO_LS_EMBD_OBJ(callback_data_s, node, pos));
        fio_ls_embd_unshift(&copy, &tmp->node);
      }
      break;

    case FIO_CALL_ON_IDLE: /* idle callbacks are orderless and evented */
      FIO_LS_EMBD_FOR(&callback_collection[c_type].callbacks, pos) {
        callback_data_s *tmp = FIO_LS_EMBD_OBJ(callback_data_s, node, pos);
        fio_defer_push_task(fio_state_on_idle_perform,
                            (void *)(uintptr_t)tmp->func, tmp->arg);
      }
      break;

    case FIO_CALL_ON_SHUTDOWN:     /* fallthrough */
    case FIO_CALL_ON_FINISH:       /* fallthrough */
    case FIO_CALL_ON_PARENT_CRUSH: /* fallthrough */
    case FIO_CALL_ON_CHILD_CRUSH:  /* fallthrough */
    case FIO_CALL_AT_EXIT:         /* fallthrough */
    case FIO_CALL_NEVER:           /* fallthrough */
    default:
      FIO_LS_EMBD_FOR(&callback_collection[c_type].callbacks, pos) {
        callback_data_s *tmp = fio_malloc(sizeof(*tmp));
        FIO_ASSERT_ALLOC(tmp);
        *tmp = *(FIO_LS_EMBD_OBJ(callback_data_s, node, pos));
        fio_ls_embd_push(&copy, &tmp->node);
      }
      break;
  }

  fio_unlock(&callback_collection[c_type].lock);
  /* run callbacks + free data */
  while (fio_ls_embd_any(&copy)) {
    callback_data_s *tmp =
        FIO_LS_EMBD_OBJ(callback_data_s, node, fio_ls_embd_pop(&copy));
    if (tmp->func) {
      tmp->func(tmp->arg);
    }
    fio_free(tmp);
  }
}

/** Clears all the existing callbacks for the event. */
void fio_state_callback_clear(callback_type_e c_type) {
  if ((int)c_type < 0 || c_type > FIO_CALL_NEVER) return;
  fio_lock(&callback_collection[c_type].lock);
  fio_state_callback_ensure(&callback_collection[c_type]);
  while (fio_ls_embd_any(&callback_collection[c_type].callbacks)) {
    callback_data_s *tmp = FIO_LS_EMBD_OBJ(
        callback_data_s, node,
        fio_ls_embd_shift(&callback_collection[c_type].callbacks));
    free(tmp); /* matches the plain malloc() in fio_state_callback_add */
  }
  fio_unlock(&callback_collection[c_type].lock);
}

void fio_state_callback_on_fork(void) {
  for (size_t i = 0; i < (FIO_CALL_NEVER + 1); ++i) {
    callback_collection[i].lock = FIO_LOCK_INIT;
  }
}
void fio_state_callback_clear_all(void) {
  for (size_t i = 0; i < (FIO_CALL_NEVER + 1); ++i) {
    fio_state_callback_clear((callback_type_e)i);
  }
}

/* *****************************************************************************
IO bound tasks
***************************************************************************** */

// typedef struct {
//   enum fio_protocol_lock_e type;
//   void (*task)(intptr_t uuid, fio_protocol_s *, void *udata);
//   void *udata;
//   void (*fallback)(intptr_t uuid, void *udata);
// } fio_defer_iotask_args_s;

static void fio_io_task_perform(void *uuid_, void *args_) {
  fio_defer_iotask_args_s *args = args_;
  intptr_t uuid = (intptr_t)uuid_;
  fio_protocol_s *pr = fio_protocol_try_lock(uuid, args->type);
  if (!pr) goto postpone;
  args->task(uuid, pr, args->udata);
  fio_protocol_unlock(pr, args->type);
  fio_free(args);
  return;
postpone:
  if (errno == EBADF) {
    if (args->fallback) args->fallback(uuid, args->udata);
    fio_free(args);
    return;
  }
  fio_defer_push_task(fio_io_task_perform, uuid_, args_);
}
/**
 * Schedules a protected connection task. The task will run within the
 * connection's lock.
 *
 * If an error ocuurs or the connection is closed before the task can run, the
 * `fallback` task wil be called instead, allowing for resource cleanup.
 */
void fio_defer_io_task FIO_IGNORE_MACRO(intptr_t uuid,
                                        fio_defer_iotask_args_s args) {
  if (!args.task) {
    if (args.fallback)
      fio_defer_push_task((void (*)(void *, void *))args.fallback, (void *)uuid,
                          args.udata);
    return;
  }
  fio_defer_iotask_args_s *cpy = fio_malloc(sizeof(*cpy));
  FIO_ASSERT_ALLOC(cpy);
  *cpy = args;
  fio_defer_push_task(fio_io_task_perform, (void *)uuid, cpy);
}

/* *****************************************************************************
Initialize the library
***************************************************************************** */

/* Called within a child process after it starts. */
static void fio_on_fork(void) {
  fio_timer_lock = FIO_LOCK_INIT;
  fio_data->lock = FIO_LOCK_INIT;
  fio_data->max_protocol_fd_lock = FIO_LOCK_INIT;
  fio_defer_on_fork();
  fio_malloc_after_fork();
  fio_poll_init();
  fio_state_callback_on_fork();

  /* don't pass open connections belonging to the parent onto the child. */
  const size_t limit = fio_data->capa;
  for (size_t i = 0; i < limit; ++i) {
    fd_data(i).sock_lock = FIO_LOCK_INIT;
    fd_data(i).protocol_lock = FIO_LOCK_INIT;
    if (fd_data(i).protocol && fd_data(i).open) {
      /* open without protocol might be waiting for the child (listening) */
      fd_data(i).protocol->rsv = 0;
      fio_force_close(fd2uuid(i));
    }
  }

  uint16_t old_active = fio_data->active;
  fio_data->active = 0;
  fio_defer_perform();
  fio_data->active = old_active;
  fio_data->is_worker = 1;
}

static void fio_mem_destroy(void);
void fio_lib_destroy(void) {
  fio_data->active = 0;
  fio_on_fork();
  fio_defer_perform();
  fio_timer_clear_all();
  fio_defer_perform();
  fio_state_callback_force(FIO_CALL_AT_EXIT);
  fio_state_callback_clear_all();
  fio_defer_perform();
  fio_poll_close();
  fio_free(fio_data);
  /* memory library destruction must be last */
  fio_mem_destroy();
  FIO_LOG_DEBUG("(%d) facil.io resources released, exit complete.",
                (int)getpid());
}

static void fio_mem_init(void);
void fio_lib_init(void) {
  /* detect socket capacity - MUST be first...*/
  ssize_t capa = 0;
  {
#ifdef _SC_OPEN_MAX
    capa = sysconf(_SC_OPEN_MAX);
#elif defined(FOPEN_MAX)
    capa = FOPEN_MAX;
#endif
    // try to maximize limits - collect max and set to max
    struct rlimit rlim = {.rlim_max = 0};
    if (getrlimit(RLIMIT_NOFILE, &rlim) == -1) {
      FIO_LOG_WARNING("`getrlimit` failed in `fio_lib_init`.");
      perror("\terrno:");
    } else {
      rlim_t original = rlim.rlim_cur;
      rlim.rlim_cur = rlim.rlim_max;
      if (rlim.rlim_cur > FIO_MAX_SOCK_CAPACITY) {
        rlim.rlim_cur = rlim.rlim_max = FIO_MAX_SOCK_CAPACITY;
      }
      while (setrlimit(RLIMIT_NOFILE, &rlim) == -1 && rlim.rlim_cur > original)
        --rlim.rlim_cur;
      getrlimit(RLIMIT_NOFILE, &rlim);
      capa = rlim.rlim_cur;
      if (capa > 1024) /* leave a slice of room */
        capa -= 16;
    }
    /* initialize memory allocator */
    fio_mem_init();
    /* initialize polling engine */
    fio_poll_init();
#if DEBUG
#if FIO_ENGINE_POLL
    FIO_LOG_INFO("facil.io " FIO_VERSION_STRING
                 " capacity initialization:\n"
                 "*    Meximum open files %zu out of %zu\n"
                 "*    Allocating %zu bytes for state handling.\n"
                 "*    %zu bytes per connection + %zu for state handling.",
                 capa, (size_t)rlim.rlim_max,
                 (sizeof(*fio_data) + (capa * (sizeof(*fio_data->poll))) +
                  (capa * (sizeof(*fio_data->info)))),
                 (sizeof(*fio_data->poll) + sizeof(*fio_data->info)),
                 sizeof(*fio_data));
#else
    FIO_LOG_INFO("facil.io " FIO_VERSION_STRING
                 " capacity initialization:\n"
                 "*    Meximum open files %zu out of %zu\n"
                 "*    Allocating %zu bytes for state handling.\n"
                 "*    %zu bytes per connection + %zu for state handling.",
                 capa, (size_t)rlim.rlim_max,
                 (sizeof(*fio_data) + (capa * (sizeof(*fio_data->info)))),
                 (sizeof(*fio_data->info)), sizeof(*fio_data));
#endif
#endif
  }

#if FIO_ENGINE_POLL
  /* allocate and initialize main data structures by detected capacity */
  fio_data = fio_mmap(sizeof(*fio_data) + (capa * (sizeof(*fio_data->poll))) +
                      (capa * (sizeof(*fio_data->info))));
  FIO_ASSERT_ALLOC(fio_data);
  fio_data->capa = capa;
  fio_data->poll =
      (void *)((uintptr_t)(fio_data + 1) + (sizeof(fio_data->info[0]) * capa));
#else
  /* allocate and initialize main data structures by detected capacity */
  fio_data = fio_mmap(sizeof(*fio_data) + (capa * (sizeof(*fio_data->info))));
  FIO_ASSERT_ALLOC(fio_data);
  fio_data->capa = capa;
#endif
  fio_data->parent = getpid();
  fio_data->connection_count = 0;
  fio_mark_time();

  for (ssize_t i = 0; i < capa; ++i) {
    fio_clear_fd(i, 0);
#if FIO_ENGINE_POLL
    fio_data->poll[i].fd = -1;
#endif
  }

  /* call initialization callbacks */
  fio_state_callback_force(FIO_CALL_ON_INITIALIZE);
  fio_state_callback_clear(FIO_CALL_ON_INITIALIZE);
}

/* *****************************************************************************
Section Start Marker












                             Running the IO Reactor













***************************************************************************** */

static void fio_review_timeout(void *arg, void *ignr) {
  // TODO: Fix review for connections with no protocol?
  (void)ignr;
  fio_protocol_s *tmp;
  time_t review = fio_data->last_cycle.tv_sec;
  intptr_t fd = (intptr_t)arg;

  uint16_t timeout = fd_data(fd).timeout;
  if (!timeout) timeout = 300; /* enforced timout settings */
  if (!fd_data(fd).open || fd_data(fd).active + timeout >= review) goto finish;
  if (fd_data(fd).protocol) {
    tmp = protocol_try_lock(fd, FIO_PR_LOCK_STATE);
    if (!tmp) {
      if (errno == EBADF) goto finish;
      goto reschedule;
    }
    if (prt_meta(tmp).locks[FIO_PR_LOCK_TASK] ||
        prt_meta(tmp).locks[FIO_PR_LOCK_WRITE])
      goto unlock;
    fio_defer_push_task(deferred_ping, (void *)fio_fd2uuid((int)fd), NULL);
  unlock:
    protocol_unlock(tmp, FIO_PR_LOCK_STATE);
  } else {
    /* open FD but no protocol? RW hook thing or listening sockets? */
    if (fd_data(fd).rw_hooks != &FIO_DEFAULT_RW_HOOKS) fio_close(fd2uuid(fd));
  }
finish:
  /* Snapshot under max_protocol_fd_lock: concurrent fio_clear_fd calls on
   * other fds update this field without holding this fd's own sock_lock, so
   * an unsynchronized read here is a data race. Using a single snapshot for
   * the whole scan (rather than re-reading the live field on every
   * iteration) also fixes a separate, pre-existing bug: the old loop
   * condition evaluated `fd_data(fd).open` before checking `fd <=
   * max_protocol_fd`, so once `fd` walked past a live snapshot's bound it
   * could read one entry past the allocated fd table. */
  fio_lock(&fio_data->max_protocol_fd_lock);
  {
    const uint32_t max_fd = fio_data->max_protocol_fd;
    fio_unlock(&fio_data->max_protocol_fd_lock);
    do {
      fd++;
    } while (fd <= max_fd && !fd_data(fd).open);

    if ((uint32_t)fd > max_fd) {
      fio_data->need_review = 1;
      return;
    }
  }
reschedule:
  fio_defer_push_task(fio_review_timeout, (void *)fd, NULL);
}

/* reactor pattern cycling - common actions */
static void fio_cycle_schedule_events(void) {
  static int idle = 0;
  static time_t last_to_review = 0;
  fio_mark_time();
  fio_timer_schedule();
  int events = fio_poll();
  if (events < 0) {
    return;
  }
  if (events > 0) {
    idle = 1;
  } else {
    /* events == 0 */
    if (idle) {
      fio_state_callback_force(FIO_CALL_ON_IDLE);
      idle = 0;
    }
  }
  if (fio_data->need_review && fio_data->last_cycle.tv_sec != last_to_review) {
    last_to_review = fio_data->last_cycle.tv_sec;
    fio_data->need_review = 0;
    fio_defer_push_task(fio_review_timeout, (void *)0, NULL);
  }
}

/* reactor pattern cycling during cleanup */
static void fio_cycle_unwind(void *ignr, void *ignr2) {
  if (fio_data->connection_count) {
    fio_cycle_schedule_events();
    fio_defer_push_task(fio_cycle_unwind, ignr, ignr2);
    return;
  }
  fio_stop();
  return;
}

/* reactor pattern cycling */
static void fio_cycle(void *ignr, void *ignr2) {
  fio_cycle_schedule_events();
  if (fio_data->active) {
    fio_defer_push_task(fio_cycle, ignr, ignr2);
    return;
  }
  return;
}

/* TODO: fixme */
static void fio_worker_startup(void) {
  /* Call the on_start callbacks for worker processes. */
  if (fio_data->workers == 1 || fio_data->is_worker) {
    fio_state_callback_force(FIO_CALL_ON_START);
    fio_state_callback_clear(FIO_CALL_ON_START);
  }

  if (fio_data->workers == 1) {
    /* Single Process - the root is also a worker */
    fio_data->is_worker = 1;
  } else if (fio_data->is_worker) {
    /* Worker Process */
    FIO_LOG_INFO("%d is running.", (int)getpid());
  } else {
    /* Root Process should run in single thread mode */
    fio_data->threads = 1;
  }

  /* require timeout review */
  fio_data->need_review = 1;

  /* the cycle task will loop by re-scheduling until it's time to finish */
  fio_defer_push_task(fio_cycle, NULL, NULL);

  /* A single thread doesn't need a pool. */
  if (fio_data->threads > 1) {
    fio_defer_thread_pool_join(fio_defer_thread_pool_new(fio_data->threads));
  } else {
    fio_defer_perform();
  }
}

/* performs all clean-up / shutdown requirements except for the exit sequence */
static void fio_worker_cleanup(void) {
  /* switch to winding down */
  if (fio_data->is_worker)
    FIO_LOG_INFO("(%d) detected exit signal.", (int)getpid());
  else
    FIO_LOG_INFO("Server Detected exit signal.");
  fio_state_callback_force(FIO_CALL_ON_SHUTDOWN);
  for (size_t i = 0; i <= fio_data->max_protocol_fd; ++i) {
    if (fd_data(i).protocol) {
      fio_defer_push_task(deferred_on_shutdown, (void *)fd2uuid(i), NULL);
    }
  }
  fio_defer_push_task(fio_cycle_unwind, NULL, NULL);
  fio_defer_perform();
  for (size_t i = 0; i <= fio_data->max_protocol_fd; ++i) {
    if (fd_data(i).protocol || fd_data(i).open) {
      fio_force_close(fd2uuid(i));
    }
  }
  fio_timer_clear_all();
  fio_defer_perform();
  if (!fio_data->is_worker) {
    fio_defer_perform();
    while (wait(NULL) != -1);
  }
  fio_defer_perform();
  fio_state_callback_force(FIO_CALL_ON_FINISH);
  fio_defer_perform();
  fio_signal_handler_reset();
  if (fio_data->parent == getpid()) {
    FIO_LOG_INFO("   ---  Shutdown Complete  ---\n");
  } else {
    FIO_LOG_INFO("(%d) cleanup complete.", (int)getpid());
  }
}

/**
 * Starts the facil.io event loop. This function will return after facil.io is
 * done (after shutdown).
 *
 * See the `struct fio_start_args` details for any possible named arguments.
 *
 * This method blocks the current thread until the server is stopped (when a
 * SIGINT/SIGTERM is received).
 */
void fio_start FIO_IGNORE_MACRO(struct fio_start_args args) {
  fio_expected_concurrency(&args.threads, &args.workers);
  /* SIGPIPE must be suppressed for all TCP servers; do it unconditionally.
   * All other signal handling (SIGINT, SIGTERM, etc.) is left to the caller. */
  signal(SIGPIPE, SIG_IGN);

  fio_data->workers = (uint16_t)args.workers;
  fio_data->threads = (uint16_t)args.threads;
  fio_data->active = 1;
  fio_data->is_worker = 0;

  fio_state_callback_force(FIO_CALL_PRE_START);
  FIO_LOG_INFO(
      "Server is running %u %s X %u %s with facil.io " FIO_VERSION_STRING
      " (%s)\n"
#if HAVE_OPENSSL
      "* Linked to %s\n"
#endif
      "* Detected capacity: %d open file limit\n"
      "* Root pid: %d\n"
      "* Press ^C to stop\n",
      fio_data->workers, fio_data->workers > 1 ? "workers" : "worker",
      fio_data->threads, fio_data->threads > 1 ? "threads" : "thread",
      fio_engine(),
#if HAVE_OPENSSL
      OpenSSL_version(0),
#endif
      fio_data->capa, (int)fio_data->parent);

  fio_worker_startup();
  fio_worker_cleanup();
}

/* *****************************************************************************
Section Start Marker















                       Converting Numbers to Strings (and back)
















***************************************************************************** */

/* *****************************************************************************
Strings to Numbers
***************************************************************************** */

FIO_FUNC inline size_t fio_atol_skip_zero(char **pstr) {
  char *const start = *pstr;
  while (**pstr == '0') {
    ++(*pstr);
  }
  return (size_t)(*pstr - *start);
}

/* consumes any digits in the string (base 2-10), returning their value */
FIO_FUNC inline uint64_t fio_atol_consume(char **pstr, uint8_t base) {
  uint64_t result = 0;
  const uint64_t limit = UINT64_MAX - (base * base);
  while (**pstr >= '0' && **pstr < ('0' + base) && result <= (limit)) {
    result = (result * base) + (**pstr - '0');
    ++(*pstr);
  }
  return result;
}

/* returns true if there's data to be skipped */
FIO_FUNC inline uint8_t fio_atol_skip_test(char **pstr, uint8_t base) {
  return (**pstr >= '0' && **pstr < ('0' + base));
}

/* consumes any digits in the string (base 2-10), returning the count skipped */
FIO_FUNC inline uint64_t fio_atol_skip(char **pstr, uint8_t base) {
  uint64_t result = 0;
  while (fio_atol_skip_test(pstr, base)) {
    ++result;
    ++(*pstr);
  }
  return result;
}

/* consumes any hex data in the string, returning their value */
FIO_FUNC inline uint64_t fio_atol_consume_hex(char **pstr) {
  uint64_t result = 0;
  const uint64_t limit = UINT64_MAX - (16 * 16);
  for (; result <= limit;) {
    uint8_t tmp;
    if (**pstr >= '0' && **pstr <= '9')
      tmp = **pstr - '0';
    else if (**pstr >= 'A' && **pstr <= 'F')
      tmp = **pstr - ('A' - 10);
    else if (**pstr >= 'a' && **pstr <= 'f')
      tmp = **pstr - ('a' - 10);
    else
      return result;
    result = (result << 4) | tmp;
    ++(*pstr);
  }
  return result;
}

/* returns true if there's data to be skipped */
FIO_FUNC inline uint8_t fio_atol_skip_hex_test(char **pstr) {
  return (**pstr >= '0' && **pstr <= '9') || (**pstr >= 'A' && **pstr <= 'F') ||
         (**pstr >= 'a' && **pstr <= 'f');
}

/* consumes any digits in the string (base 2-10), returning the count skipped */
FIO_FUNC inline uint64_t fio_atol_skip_hex(char **pstr) {
  uint64_t result = 0;
  while (fio_atol_skip_hex_test(pstr)) {
    ++result;
    ++(*pstr);
  }
  return result;
}

/* caches a up to 8*8 */
// static inline fio_atol_pow_10_cache(size_t ex) {}

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
int64_t fio_atol(char **pstr) {
  /* No binary representation in strtol */
  char *str = *pstr;
  uint64_t result = 0;
  uint8_t invert = 0;
  while (isspace(*str)) ++(str);
  if (str[0] == '-') {
    invert ^= 1;
    ++str;
  } else if (*str == '+') {
    ++(str);
  }

  if (str[0] == 'B' || str[0] == 'b' ||
      (str[0] == '0' && (str[1] == 'b' || str[1] == 'B'))) {
    /* base 2 */
    if (str[0] == '0') str++;
    str++;
    fio_atol_skip_zero(&str);
    while (str[0] == '0' || str[0] == '1') {
      result = (result << 1) | (str[0] - '0');
      str++;
    }
    goto sign; /* no overlow protection, since sign might be embedded */

  } else if (str[0] == 'x' || str[0] == 'X' ||
             (str[0] == '0' && (str[1] == 'x' || str[1] == 'X'))) {
    /* base 16 */
    if (str[0] == '0') str++;
    str++;
    fio_atol_skip_zero(&str);
    result = fio_atol_consume_hex(&str);
    if (fio_atol_skip_hex_test(&str)) /* too large for a number */
      return 0;
    goto sign; /* no overlow protection, since sign might be embedded */
  } else if (str[0] == '0') {
    fio_atol_skip_zero(&str);
    /* base 8 */
    result = fio_atol_consume(&str, 8);
    if (fio_atol_skip_test(&str, 8)) /* too large for a number */
      return 0;
  } else {
    /* base 10 */
    result = fio_atol_consume(&str, 10);
    if (fio_atol_skip_test(&str, 10)) /* too large for a number */
      return 0;
  }
  if (result & ((uint64_t)1 << 63))
    result = INT64_MAX; /* signed overflow protection */
sign:
  if (invert) result = 0 - result;
  *pstr = str;
  return (int64_t)result;
}

/** A helper function that converts between String data to a signed double. */
double fio_atof(char **pstr) { return strtold(*pstr, pstr); }

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
 * are automatically added (i.e., "0x" for hex and "0b" for base 2).
 *
 * Returns the number of bytes actually written (excluding the NUL
 * terminator).
 */
size_t fio_ltoa(char *dest, int64_t num, uint8_t base) {
  const char notation[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                           '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

  size_t len = 0;
  char buf[48]; /* we only need up to 20 for base 10, but base 3 needs 41... */

  if (!num) goto zero;

  switch (base) {
    case 1: /* fallthrough */
    case 2:
      /* Base 2 */
      {
        uint64_t n =
            num;       /* avoid bit shifting inconsistencies with signed bit */
        uint8_t i = 0; /* counting bits */
        dest[len++] = '0';
        dest[len++] = 'b';

        while ((i < 64) && (n & 0x8000000000000000) == 0) {
          n = n << 1;
          i++;
        }
        /* make sure the Binary representation doesn't appear signed. */
        if (i) {
          dest[len++] = '0';
        }
        /* write to dest. */
        while (i < 64) {
          dest[len++] = ((n & 0x8000000000000000) ? '1' : '0');
          n = n << 1;
          i++;
        }
        dest[len] = 0;
        return len;
      }
    case 8:
      /* Base 8 */
      {
        uint64_t l = 0;
        /* `0 - num` on a signed int64_t is UB (and, in practice, a no-op)
         * when num == INT64_MIN, since INT64_MIN has no positive int64_t
         * representation. Compute the magnitude in unsigned arithmetic
         * instead, which is well-defined (wraps modulo 2^64) and yields the
         * correct value for every representable num, INT64_MIN included. */
        uint64_t mag;
        if (num < 0) {
          dest[len++] = '-';
          mag = (uint64_t)0 - (uint64_t)num;
        } else {
          mag = (uint64_t)num;
        }
        dest[len++] = '0';

        while (mag) {
          buf[l++] = '0' + (mag & 7);
          mag = mag >> 3;
        }
        while (l) {
          --l;
          dest[len++] = buf[l];
        }
        dest[len] = 0;
        return len;
      }

    case 16:
      /* Base 16 */
      {
        uint64_t n =
            num;       /* avoid bit shifting inconsistencies with signed bit */
        uint8_t i = 0; /* counting bits */
        dest[len++] = '0';
        dest[len++] = 'x';
        while (i < 8 && (n & 0xFF00000000000000) == 0) {
          n = n << 8;
          i++;
        }
        /* make sure the Hex representation doesn't appear misleadingly signed.
         */
        if (i && (n & 0x8000000000000000)) {
          dest[len++] = '0';
          dest[len++] = '0';
        }
        /* write the damn thing, high to low */
        while (i < 8) {
          uint8_t tmp = (n & 0xF000000000000000) >> 60;
          dest[len++] = notation[tmp];
          tmp = (n & 0x0F00000000000000) >> 56;
          dest[len++] = notation[tmp];
          i++;
          n = n << 8;
        }
        dest[len] = 0;
        return len;
      }
    case 3: /* fallthrough */
    case 4: /* fallthrough */
    case 5: /* fallthrough */
    case 6: /* fallthrough */
    case 7: /* fallthrough */
    case 9: /* fallthrough */
      /* rare bases */
      {
        /* see the base-8 case above for why this can't be `num = 0 - num`
         * on the signed int64_t directly (UB / wrong result for
         * INT64_MIN). */
        uint64_t mag;
        if (num < 0) {
          dest[len++] = '-';
          mag = (uint64_t)0 - (uint64_t)num;
        } else {
          mag = (uint64_t)num;
        }
        uint64_t l = 0;
        while (mag) {
          uint64_t t = mag / base;
          buf[l++] = '0' + (mag - (t * base));
          mag = t;
        }
        while (l) {
          --l;
          dest[len++] = buf[l];
        }
        dest[len] = 0;
        return len;
      }

    default:
      break;
  }
  /* Base 10, the default base */

  {
    /* see the base-8 case above for why this can't be `num = 0 - num` on
     * the signed int64_t directly (UB / wrong result for INT64_MIN). */
    uint64_t mag;
    if (num < 0) {
      dest[len++] = '-';
      mag = (uint64_t)0 - (uint64_t)num;
    } else {
      mag = (uint64_t)num;
    }
    uint64_t l = 0;
    while (mag) {
      uint64_t t = mag / 10;
      buf[l++] = '0' + (mag - (t * 10));
      mag = t;
    }
    while (l) {
      --l;
      dest[len++] = buf[l];
    }
    dest[len] = 0;
    return len;
  }

zero:
  switch (base) {
    case 1:
    case 2:
      dest[len++] = '0';
      dest[len++] = 'b';
      break;
    case 8:
      dest[len++] = '0';
      break;
    case 16:
      dest[len++] = '0';
      dest[len++] = 'x';
      dest[len++] = '0';
      break;
  }
  dest[len++] = '0';
  dest[len] = 0;
  return len;
}

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
size_t fio_ftoa(char *dest, double num, uint8_t base) {
  if (base == 2 || base == 16) {
    /* handle the binary / Hex representation the same as if it were an
     * int64_t
     */
    int64_t *i = (void *)&num;
    return fio_ltoa(dest, *i, base);
  }

  size_t written = sprintf(dest, "%g", num);
  uint8_t need_zero = 1;
  char *start = dest;
  while (*start) {
    if (*start == ',')  // locale issues?
      *start = '.';
    if (*start == '.' || *start == 'e') {
      need_zero = 0;
      break;
    }
    start++;
  }
  if (need_zero) {
    dest[written++] = '.';
    dest[written++] = '0';
  }
  return written;
}

/* *****************************************************************************
Section Start Marker







                       SSL/TLS Weak Symbols for TLS Support








***************************************************************************** */

/**
 * Returns the number of registered ALPN protocol names.
 *
 * This could be used when deciding if protocol selection should be delegated to
 * the ALPN mechanism, or whether a protocol should be immediately assigned.
 *
 * If no ALPN protocols are registered, zero (0) is returned.
 */
uintptr_t FIO_TLS_WEAK fio_tls_alpn_count(void *tls) {
  return 0;
  (void)tls;
}

/**
 * Establishes an SSL/TLS connection as an SSL/TLS Server, using the specified
 * context / settings object.
 *
 * The `uuid` should be a socket UUID that is already connected to a peer (i.e.,
 * the result of `fio_accept`).
 *
 * The `udata` is an opaque user data pointer that is passed along to the
 * protocol selected (if any protocols were added using `fio_tls_alpn_add`).
 */
void FIO_TLS_WEAK fio_tls_accept(intptr_t uuid, void *tls, void *udata) {
  FIO_LOG_FATAL("No supported SSL/TLS library available.");
  exit(-1);
  return;
  (void)uuid;
  (void)tls;
  (void)udata;
}

/**
 * Increase the reference count for the TLS object.
 *
 * Decrease with `fio_tls_destroy`.
 */
void FIO_TLS_WEAK fio_tls_dup(void *tls) {
  FIO_LOG_FATAL("No supported SSL/TLS library available.");
  exit(-1);
  return;
  (void)tls;
}

/**
 * Destroys the SSL/TLS context / settings object and frees any related
 * resources / memory.
 */
void FIO_TLS_WEAK fio_tls_destroy(void *tls) {
  FIO_LOG_FATAL("No supported SSL/TLS library available.");
  exit(-1);
  return;
  (void)tls;
}

/* *****************************************************************************
Section Start Marker















                       Listening to Incoming Connections
















***************************************************************************** */

/* *****************************************************************************
The listening protocol (use the facil.io API to make a socket and attach it)
***************************************************************************** */

typedef struct {
  fio_protocol_s pr;
  intptr_t uuid;
  void *udata;
  void (*on_open)(intptr_t uuid, void *udata);
  void (*on_start)(intptr_t uuid, void *udata);
  void (*on_finish)(intptr_t uuid, void *udata);
  char *port;
  char *addr;
  size_t port_len;
  size_t addr_len;
  void *tls;
} fio_listen_protocol_s;

static void fio_listen_cleanup_task(void *pr_) {
  fio_listen_protocol_s *pr = pr_;
  /* on_finish runs before fio_tls_destroy, not after: for a TLS listener,
   * on_finish's udata (http.c's http_settings_s) can be freed as a side
   * effect of fio_tls_destroy itself; it's registered as the "http/1.1"
   * ALPN entry's on_cleanup hook, which fio_tls_destroy fires synchronously
   * the moment it releases the last reference to this tls object. Calling
   * on_finish afterward would then read (and, for a non-TLS listener,
   * separately free) already-freed memory. Neither on_finish nor
   * fio_force_close below depends on tls having been torn down first, so
   * this order costs nothing. */
  if (pr->on_finish) {
    pr->on_finish(pr->uuid, pr->udata);
  }
  if (pr->tls) fio_tls_destroy(pr->tls);
  fio_force_close(pr->uuid);
  if (pr->addr &&
      (!pr->port || *pr->port == 0 ||
       (pr->port[0] == '0' && pr->port[1] == 0)) &&
      fio_is_master()) {
    /* delete Unix sockets */
    unlink(pr->addr);
  }
  fio_free(pr_);
}

static void fio_listen_on_startup(void *pr_) {
  fio_state_callback_remove(FIO_CALL_ON_SHUTDOWN, fio_listen_cleanup_task, pr_);
  fio_listen_protocol_s *pr = pr_;
  fio_attach(pr->uuid, &pr->pr);
  if (pr->port_len)
    FIO_LOG_DEBUG("(%d) started listening on port %s", (int)getpid(), pr->port);
  else
    FIO_LOG_DEBUG("(%d) started listening on Unix Socket at %s", (int)getpid(),
                  pr->addr);
}

static void fio_listen_on_close(intptr_t uuid, fio_protocol_s *pr_) {
  fio_listen_cleanup_task(pr_);
  (void)uuid;
}

static void fio_listen_on_data(intptr_t uuid, fio_protocol_s *pr_) {
  fio_listen_protocol_s *pr = (fio_listen_protocol_s *)pr_;
  for (int i = 0; i < 4; ++i) {
    intptr_t client = fio_accept(uuid);
    if (client == -1) return;
    pr->on_open(client, pr->udata);
  }
}

static void fio_listen_on_data_tls(intptr_t uuid, fio_protocol_s *pr_) {
  fio_listen_protocol_s *pr = (fio_listen_protocol_s *)pr_;
  for (int i = 0; i < 4; ++i) {
    intptr_t client = fio_accept(uuid);
    if (client == -1) return;
    fio_tls_accept(client, pr->tls, pr->udata);
    pr->on_open(client, pr->udata);
  }
}

static void fio_listen_on_data_tls_alpn(intptr_t uuid, fio_protocol_s *pr_) {
  fio_listen_protocol_s *pr = (fio_listen_protocol_s *)pr_;
  for (int i = 0; i < 4; ++i) {
    intptr_t client = fio_accept(uuid);
    if (client == -1) return;
    fio_tls_accept(client, pr->tls, pr->udata);
  }
}

/* stub for editor - unused */
void fio_listen____(void);
/**
 * Schedule a network service on a listening socket.
 *
 * Returns the listening socket or -1 (on error).
 */
intptr_t fio_listen FIO_IGNORE_MACRO(struct fio_listen_args args) {
  // ...
  if ((!args.on_open && (!args.tls || !fio_tls_alpn_count(args.tls))) ||
      (!args.address && !args.port)) {
    errno = EINVAL;
    goto error;
  }

  size_t addr_len = 0;
  size_t port_len = 0;
  if (args.address) addr_len = strlen(args.address);
  if (args.port) {
    port_len = strlen(args.port);
    char *tmp = (char *)args.port;
    if (!fio_atol(&tmp)) {
      port_len = 0;
      args.port = NULL;
    }
    if (*tmp) {
      /* port format was invalid, should be only numerals */
      errno = EINVAL;
      goto error;
    }
  }
  const intptr_t uuid = fio_socket(args.address, args.port, 1);
  if (uuid == -1) goto error;

  fio_listen_protocol_s *pr = fio_malloc(sizeof(*pr) + addr_len + port_len +
                                         ((addr_len + port_len) ? 2 : 0));
  FIO_ASSERT_ALLOC(pr);

  if (args.tls) fio_tls_dup(args.tls);

  *pr = (fio_listen_protocol_s){
      .pr =
          {
              .on_close = fio_listen_on_close,
              .ping = mock_ping_eternal,
              .on_data = (args.tls ? (fio_tls_alpn_count(args.tls)
                                          ? fio_listen_on_data_tls_alpn
                                          : fio_listen_on_data_tls)
                                   : fio_listen_on_data),
          },
      .uuid = uuid,
      .udata = args.udata,
      .on_open = args.on_open,
      .on_start = args.on_start,
      .on_finish = args.on_finish,
      .tls = args.tls,
      .addr_len = addr_len,
      .port_len = port_len,
      .addr = (char *)(pr + 1),
      .port = ((char *)(pr + 1) + addr_len + 1),
  };

  if (addr_len) memcpy(pr->addr, args.address, addr_len + 1);
  if (port_len) memcpy(pr->port, args.port, port_len + 1);

  if (fio_is_running()) {
    fio_attach(pr->uuid, &pr->pr);
  } else {
    fio_state_callback_add(FIO_CALL_ON_START, fio_listen_on_startup, pr);
    fio_state_callback_add(FIO_CALL_ON_SHUTDOWN, fio_listen_cleanup_task, pr);
  }

  if (args.port)
    FIO_LOG_INFO("Listening on port %s", args.port);
  else
    FIO_LOG_INFO("Listening on Unix Socket at %s", args.address);

  return uuid;
error:
  if (args.on_finish) {
    args.on_finish(-1, args.udata);
  }
  return -1;
}

/* *****************************************************************************
Section Start Marker






















                   Memory Allocator Details & Implementation























***************************************************************************** */

/* *****************************************************************************
Allocator default settings
***************************************************************************** */

/* doun't change these */
#undef FIO_MEMORY_BLOCK_SLICES
#undef FIO_MEMORY_BLOCK_HEADER_SIZE
#undef FIO_MEMORY_BLOCK_START_POS
#undef FIO_MEMORY_MAX_SLICES_PER_BLOCK
#undef FIO_MEMORY_BLOCK_MASK

/* The number of blocks pre-allocated each system call, 256 ==8Mb */
#ifndef FIO_MEMORY_BLOCKS_PER_ALLOCATION
#define FIO_MEMORY_BLOCKS_PER_ALLOCATION 256
#endif

#define FIO_MEMORY_BLOCK_MASK (FIO_MEMORY_BLOCK_SIZE - 1) /* 0b0...1... */

#define FIO_MEMORY_BLOCK_SLICES (FIO_MEMORY_BLOCK_SIZE >> 4) /* 16B slices */

/* must be divisable by 16 bytes, bigger than min(sizeof(block_s), 16) */
#define FIO_MEMORY_BLOCK_HEADER_SIZE 32

/* allocation counter position (start) */
#define FIO_MEMORY_BLOCK_START_POS (FIO_MEMORY_BLOCK_HEADER_SIZE >> 4)

#define FIO_MEMORY_MAX_SLICES_PER_BLOCK \
  (FIO_MEMORY_BLOCK_SLICES - FIO_MEMORY_BLOCK_START_POS)

/* *****************************************************************************
Custom memory management via ccol_memmgmt_procs_t

Redirects fio_malloc/fio_calloc/fio_free/fio_realloc/fio_realloc2/fio_mmap to
a caller-supplied ccol_memmgmt_procs_t instead of the arena (or, under
FIO_FORCE_MALLOC, instead of plain calloc/realloc/free) below.  See the
comment on fio_set_mem_mgmt_procs() in fio.h for the contract.
***************************************************************************** */

static ccol_memmgmt_procs_t g_fio_mem_procs;
static bool g_fio_mem_procs_set = false;

/* fio_malloc/fio_calloc/fio_realloc/fio_realloc2/fio_mmap are all declared
 * FIO_ALIGN_NEW/FIO_ALIGN in fio.h (`__attribute__((assume_aligned(16)))`
 * where the compiler supports it), which tells every caller across this
 * codebase's translation units it may emit aligned loads/stores against the
 * returned pointer. That is a safe promise as long as these functions are
 * backed by facio's own arena (always 16-byte aligned by construction), but
 * once a caller redirects them to custom procs via fio_set_mem_mgmt_procs
 * (see chttpsvr_set_engine_mem_mgmt_procs in chttpserver.h), the returned
 * pointer's alignment is only whatever that custom allocator happens to
 * provide - ccol_memmgmt_procs_t's own contract makes no alignment
 * guarantee at all. A misaligned pointer handed to an assume_aligned(16)
 * call site is undefined behavior under -O3 (silent corruption or a SIGSEGV
 * that looks nothing like a memory bug), not merely a missed optimization.
 * This check converts that into an immediate, diagnosable failure instead. */
static inline void *_fio_mem_procs_check_align16(void *ptr, const char *fn) {
  FIO_ASSERT(!ptr || (((uintptr_t)ptr) & 15) == 0,
             "custom memory management procs returned a pointer from %s "
             "that isn't 16-byte aligned (%p) - fio_malloc/fio_calloc/"
             "fio_realloc/fio_realloc2/fio_mmap require this of any "
             "installed custom allocator (see chttpsvr_set_engine_mem_"
             "mgmt_procs).",
             fn, ptr);
  return ptr;
}

void fio_set_mem_mgmt_procs(const struct ccol_memmgmt_procs_t *mp) {
  if (mp) {
    g_fio_mem_procs = *(const ccol_memmgmt_procs_t *)mp;
    g_fio_mem_procs_set = true;
  } else {
    g_fio_mem_procs_set = false;
  }
}

bool fio_has_mem_mgmt_procs(void) { return g_fio_mem_procs_set; }

/* *****************************************************************************
FIO_FORCE_MALLOC handler
***************************************************************************** */

#if FIO_FORCE_MALLOC

void *fio_malloc(size_t size) {
  if (g_fio_mem_procs_set) return _mem_calloc(&g_fio_mem_procs, 1, size);
  return calloc(size, 1);
}

/* Note: under FIO_FORCE_MALLOC, FIO_ALIGN/FIO_ALIGN_NEW (fio.h) expand to
 * nothing (no assume_aligned attribute is applied to these declarations in
 * this build mode - see fio.h), so, unlike the default arena-backed build
 * below, a custom allocator's return value here carries no alignment
 * assumption for the compiler to violate; no alignment check is needed. */
void *fio_calloc(size_t size_per_unit, size_t unit_count) {
  if (g_fio_mem_procs_set)
    return _mem_calloc(&g_fio_mem_procs, unit_count, size_per_unit);
  return calloc(size_per_unit, unit_count);
}

void fio_free(void *ptr) {
  if (g_fio_mem_procs_set) {
    _mem_free(&g_fio_mem_procs, ptr);
    return;
  }
  free(ptr);
}

void *fio_realloc(void *ptr, size_t new_size) {
  if (g_fio_mem_procs_set) return _mem_realloc(&g_fio_mem_procs, ptr, new_size);
  return realloc((ptr), (new_size));
}

void *fio_realloc2(void *ptr, size_t new_size, size_t copy_length) {
  if (g_fio_mem_procs_set) return _mem_realloc(&g_fio_mem_procs, ptr, new_size);
  return realloc((ptr), (new_size));
  (void)copy_length;
}

void *fio_mmap(size_t size) {
  if (g_fio_mem_procs_set) return _mem_calloc(&g_fio_mem_procs, 1, size);
  return calloc(size, 1);
}

void fio_malloc_after_fork(void) {}
void fio_mem_destroy(void) {}
void fio_mem_init(void) {}

#else

/* *****************************************************************************
Memory Copying by 16 byte units
***************************************************************************** */

/** used internally, only when memory addresses are known to be aligned */
static inline void fio_memcpy(void *__restrict dest_, void *__restrict src_,
                              size_t units) {
#if __SIZEOF_INT128__ == 9 /* a 128bit type exists... but tests favor 64bit */
  register __uint128_t *dest = dest_;
  register __uint128_t *src = src_;
#elif SIZE_MAX == 0xFFFFFFFFFFFFFFFF /* 64 bit size_t */
  register size_t *dest = dest_;
  register size_t *src = src_;
  units = units << 1;
#elif SIZE_MAX == 0xFFFFFFFF         /* 32 bit size_t */
  register size_t *dest = dest_;
  register size_t *src = src_;
  units = units << 2;
#else                                /* unknow... assume 16 bit? */
  register size_t *dest = dest_;
  register size_t *src = src_;
  units = units << 3;
#endif
  while (units >= 16) { /* unroll loop */
    dest[0] = src[0];
    dest[1] = src[1];
    dest[2] = src[2];
    dest[3] = src[3];
    dest[4] = src[4];
    dest[5] = src[5];
    dest[6] = src[6];
    dest[7] = src[7];
    dest[8] = src[8];
    dest[9] = src[9];
    dest[10] = src[10];
    dest[11] = src[11];
    dest[12] = src[12];
    dest[13] = src[13];
    dest[14] = src[14];
    dest[15] = src[15];
    dest += 16;
    src += 16;
    units -= 16;
  }
  switch (units) {
    case 15:
      *(dest++) = *(src++); /* fallthrough */
    case 14:
      *(dest++) = *(src++); /* fallthrough */
    case 13:
      *(dest++) = *(src++); /* fallthrough */
    case 12:
      *(dest++) = *(src++); /* fallthrough */
    case 11:
      *(dest++) = *(src++); /* fallthrough */
    case 10:
      *(dest++) = *(src++); /* fallthrough */
    case 9:
      *(dest++) = *(src++); /* fallthrough */
    case 8:
      *(dest++) = *(src++); /* fallthrough */
    case 7:
      *(dest++) = *(src++); /* fallthrough */
    case 6:
      *(dest++) = *(src++); /* fallthrough */
    case 5:
      *(dest++) = *(src++); /* fallthrough */
    case 4:
      *(dest++) = *(src++); /* fallthrough */
    case 3:
      *(dest++) = *(src++); /* fallthrough */
    case 2:
      *(dest++) = *(src++); /* fallthrough */
    case 1:
      *(dest++) = *(src++);
  }
}

/* *****************************************************************************
System Memory wrappers
***************************************************************************** */

/*
 * allocates memory using `mmap`, but enforces block size alignment.
 * requires page aligned `len`.
 *
 * `align_shift` is used to move the memory page alignment to allow for a single
 * page allocation header. align_shift MUST be either 0 (normal) or 1 (single
 * page header). Other values might cause errors.
 */
static inline void *sys_alloc(size_t len, uint8_t is_indi) {
  void *result;
  static void *next_alloc = NULL;
/* hope for the best? */
#ifdef MAP_ALIGNED
  result =
      mmap(next_alloc, len, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_ALIGNED(FIO_MEMORY_BLOCK_SIZE_LOG),
           -1, 0);
#else
  result = mmap(next_alloc, len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
  if (result == MAP_FAILED) return NULL;
  if (((uintptr_t)result & FIO_MEMORY_BLOCK_MASK)) {
    munmap(result, len);
    result = mmap(NULL, len + FIO_MEMORY_BLOCK_SIZE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
      return NULL;
    }
    const uintptr_t offset =
        (FIO_MEMORY_BLOCK_SIZE - ((uintptr_t)result & FIO_MEMORY_BLOCK_MASK));
    if (offset) {
      munmap(result, offset);
      result = (void *)((uintptr_t)result + offset);
    }
    munmap((void *)((uintptr_t)result + len), FIO_MEMORY_BLOCK_SIZE - offset);
  }
  if (is_indi ==
      0) /* advance by a block's allocation size for next allocation */
    next_alloc =
        (void *)((uintptr_t)result +
                 (FIO_MEMORY_BLOCK_SIZE * (FIO_MEMORY_BLOCKS_PER_ALLOCATION)));
  else /* add 1TB for realloc */
    next_alloc = (void *)((uintptr_t)result + (is_indi * ((uintptr_t)1 << 30)));
  return result;
}

/* frees memory using `munmap`. requires exact, page aligned, `len` */
static inline void sys_free(void *mem, size_t len) { munmap(mem, len); }

static void *sys_realloc(void *mem, size_t prev_len, size_t new_len) {
  if (new_len > prev_len) {
    void *result;
#if defined(__linux__)
    result = mremap(mem, prev_len, new_len, 0);
    if (result != MAP_FAILED) return result;
#endif
    result = mmap((void *)((uintptr_t)mem + prev_len), new_len - prev_len,
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == (void *)((uintptr_t)mem + prev_len)) {
      result = mem;
    } else {
      /* copy and free */
      munmap(result, new_len - prev_len); /* free the failed attempt */
      result = sys_alloc(new_len, 1);     /* allocate new memory */
      if (!result) {
        return NULL;
      }
      fio_memcpy(result, mem, prev_len >> 4); /* copy data */
      // memcpy(result, mem, prev_len);
      munmap(mem, prev_len); /* free original memory */
    }
    return result;
  }
  if (new_len + 4096 < prev_len) /* more than a single dangling page */
    munmap((void *)((uintptr_t)mem + new_len), prev_len - new_len);
  return mem;
}

/** Rounds up any size to the nearest page alignment (assumes 4096 bytes per
 * page) */
static inline size_t sys_round_size(size_t size) {
  return (size & (~4095)) + (4096 * (!!(size & 4095)));
}

/* *****************************************************************************
Data Types
***************************************************************************** */

/* The basic block header. Starts a 32Kib memory block */
typedef struct block_s block_s;

struct block_s {
  block_s *parent;   /* REQUIRED, root == point to self */
  uint16_t ref;      /* reference count (per memory page) */
  uint16_t pos;      /* position into the block */
  uint16_t max;      /* available memory count */
  uint16_t root_ref; /* root reference memory padding */
};

typedef struct block_node_s block_node_s;
struct block_node_s {
  block_s dont_touch; /* prevent block internal data from being corrupted */
  fio_ls_embd_s node; /* next block */
};

/* a per-CPU core "arena" for memory allocations  */
typedef struct {
  block_s *block;
  fio_lock_i lock;
} arena_s;

/* The memory allocators persistent state */
static struct {
  fio_ls_embd_s available; /* free list for memory blocks */
  // intptr_t count;          /* free list counter */
  size_t cores;    /* the number of detected CPU cores*/
  fio_lock_i lock; /* a global lock */
  uint8_t forked;  /* a forked collection indicator. */
} memory = {
    .cores = 1,
    .lock = FIO_LOCK_INIT,
    .available = FIO_LS_INIT(memory.available),
};

/* The per-CPU arena array. */
static arena_s *arenas;

/* The per-CPU arena array. */
static long double on_malloc_zero;

#if DEBUG
/* The per-CPU arena array. */
static size_t fio_mem_block_count_max;
/* The per-CPU arena array. */
static size_t fio_mem_block_count;
#define FIO_MEMORY_ON_BLOCK_ALLOC()                    \
  do {                                                 \
    fio_atomic_add(&fio_mem_block_count, 1);           \
    if (fio_mem_block_count > fio_mem_block_count_max) \
      fio_mem_block_count_max = fio_mem_block_count;   \
  } while (0)
#define FIO_MEMORY_ON_BLOCK_FREE()           \
  do {                                       \
    fio_atomic_sub(&fio_mem_block_count, 1); \
  } while (0)
#define FIO_MEMORY_PRINT_BLOCK_STAT()                                  \
  FIO_LOG_INFO(                                                        \
      "(fio) Total memory blocks allocated before cleanup %zu\n"       \
      "       Maximum memory blocks allocated at a single time %zu\n", \
      fio_mem_block_count, fio_mem_block_count_max)
#define FIO_MEMORY_PRINT_BLOCK_STAT_END()    \
  FIO_LOG_INFO(                              \
      "(fio) Total memory blocks allocated " \
      "after cleanup (possible leak) %zu\n", \
      fio_mem_block_count)
#else
#define FIO_MEMORY_ON_BLOCK_ALLOC()
#define FIO_MEMORY_ON_BLOCK_FREE()
#define FIO_MEMORY_PRINT_BLOCK_STAT()
#define FIO_MEMORY_PRINT_BLOCK_STAT_END()
#endif
/* *****************************************************************************
Per-CPU Arena management
***************************************************************************** */

/* returned a locked arena. Attempts the preffered arena first. */
static inline arena_s *arena_lock(arena_s *preffered) {
  if (!preffered) preffered = arenas;
  if (!fio_trylock(&preffered->lock)) return preffered;
  do {
    arena_s *arena = preffered;
    for (size_t i = (size_t)(arena - arenas); i < memory.cores; ++i) {
      if ((preffered == arenas || arena != preffered) &&
          !fio_trylock(&arena->lock))
        return arena;
      ++arena;
    }
    if (preffered == arenas) fio_reschedule_thread();
    preffered = arenas;
  } while (1);
}

static __thread arena_s *arena_last_used;

static void arena_enter(void) { arena_last_used = arena_lock(arena_last_used); }

static inline void arena_exit(void) { fio_unlock(&arena_last_used->lock); }

/** Clears any memory locks, in case of a system call to `fork`. */
void fio_malloc_after_fork(void) {
  arena_last_used = NULL;
  if (!arenas) {
    return;
  }
  memory.lock = FIO_LOCK_INIT;
  memory.forked = 1;
  for (size_t i = 0; i < memory.cores; ++i) {
    arenas[i].lock = FIO_LOCK_INIT;
  }
}

/* *****************************************************************************
Block management / allocation
***************************************************************************** */

static inline void block_init_root(block_s *blk, block_s *parent) {
  *blk = (block_s){
      .parent = parent,
      .ref = 1,
      .pos = FIO_MEMORY_BLOCK_START_POS,
      .root_ref = 1,
  };
}

/* intializes the block header for an available block of memory. */
static inline void block_init(block_s *blk) {
  /* initialization shouldn't effect `parent` or `root_ref`*/
  blk->ref = 1;
  blk->pos = FIO_MEMORY_BLOCK_START_POS;
  /* zero out linked list memory (everything else is already zero) */
  ((block_node_s *)blk)->node.next = NULL;
  ((block_node_s *)blk)->node.prev = NULL;
  /* bump parent reference count */
  fio_atomic_add(&blk->parent->root_ref, 1);
}

/* intializes the block header for an available block of memory. */
static inline void block_free(block_s *blk) {
  if (fio_atomic_sub(&blk->ref, 1)) return;

  memset(blk + 1, 0, (FIO_MEMORY_BLOCK_SIZE - sizeof(*blk)));
  fio_lock(&memory.lock);
  fio_ls_embd_push(&memory.available, &((block_node_s *)blk)->node);

  blk = blk->parent;

  if (fio_atomic_sub(&blk->root_ref, 1)) {
    fio_unlock(&memory.lock);
    return;
  }
  // fio_unlock(&memory.lock);
  // return;

  /* remove all of the root block's children (slices) from the memory pool */
  for (size_t i = 0; i < FIO_MEMORY_BLOCKS_PER_ALLOCATION; ++i) {
    block_node_s *pos =
        (block_node_s *)((uintptr_t)blk + (i * FIO_MEMORY_BLOCK_SIZE));
    fio_ls_embd_remove(&pos->node);
  }

  fio_unlock(&memory.lock);
  sys_free(blk, FIO_MEMORY_BLOCK_SIZE * FIO_MEMORY_BLOCKS_PER_ALLOCATION);
  FIO_LOG_DEBUG("memory allocator returned %p to the system", (void *)blk);
  FIO_MEMORY_ON_BLOCK_FREE();
}

/* intializes the block header for an available block of memory. */
static inline block_s *block_new(void) {
  block_s *blk = NULL;

  fio_lock(&memory.lock);
  blk = (block_s *)fio_ls_embd_pop(&memory.available);
  if (blk) {
    blk = (block_s *)FIO_LS_EMBD_OBJ(block_node_s, node, blk);
    FIO_ASSERT(((uintptr_t)blk & FIO_MEMORY_BLOCK_MASK) == 0,
               "Memory allocator error! double `fio_free`?\n");
    block_init(blk); /* must be performed within lock */
    fio_unlock(&memory.lock);
    return blk;
  }
  /* collect memory from the system */
  blk = sys_alloc(FIO_MEMORY_BLOCK_SIZE * FIO_MEMORY_BLOCKS_PER_ALLOCATION, 0);
  if (!blk) {
    fio_unlock(&memory.lock);
    return NULL;
  }
  FIO_LOG_DEBUG("memory allocator allocated %p from the system", (void *)blk);
  FIO_MEMORY_ON_BLOCK_ALLOC();
  block_init_root(blk, blk);
  /* the extra memory goes into the memory pool. initialize + linke-list. */
  block_node_s *tmp = (block_node_s *)blk;
  for (int i = 1; i < FIO_MEMORY_BLOCKS_PER_ALLOCATION; ++i) {
    tmp = (block_node_s *)((uintptr_t)tmp + FIO_MEMORY_BLOCK_SIZE);
    block_init_root((block_s *)tmp, blk);
    fio_ls_embd_push(&memory.available, &tmp->node);
  }
  fio_unlock(&memory.lock);
  /* return the root block (which isn't in the memory pool). */
  return blk;
}

/* allocates memory from within a block - called within an arena's lock */
static inline void *block_slice(uint16_t units) {
  block_s *blk = arena_last_used->block;
  if (!blk) {
    /* arena is empty */
    blk = block_new();
    arena_last_used->block = blk;
  } else if (blk->pos + units > FIO_MEMORY_MAX_SLICES_PER_BLOCK) {
    /* not enough memory in the block - rotate */
    block_free(blk);
    blk = block_new();
    arena_last_used->block = blk;
  }
  if (!blk) {
    /* no system memory available? */
    errno = ENOMEM;
    return NULL;
  }
  /* slice block starting at blk->pos and increase reference count */
  const void *mem = (void *)((uintptr_t)blk + ((uintptr_t)blk->pos << 4));
  fio_atomic_add(&blk->ref, 1);
  blk->pos += units;
  if (blk->pos >= FIO_MEMORY_MAX_SLICES_PER_BLOCK) {
    /* ... the block was fully utilized, clear arena */
    block_free(blk);
    arena_last_used->block = NULL;
  }
  return (void *)mem;
}

/* handle's a bock's reference count - called without a lock */
static inline void block_slice_free(void *mem) {
  /* locate block boundary */
  block_s *blk = (block_s *)((uintptr_t)mem & (~FIO_MEMORY_BLOCK_MASK));
  block_free(blk);
}

/* *****************************************************************************
Non-Block allocations (direct from the system)
***************************************************************************** */

/* allocates directly from the system adding size header - no lock required. */
static inline void *big_alloc(size_t size) {
  size = sys_round_size(size + 16);
  size_t *mem = sys_alloc(size, 1);
  if (!mem) goto error;
  *mem = size;
  return (void *)(((uintptr_t)mem) + 16);
error:
  return NULL;
}

/* reads size header and frees memory back to the system */
static inline void big_free(void *ptr) {
  size_t *mem = (void *)(((uintptr_t)ptr) - 16);
  sys_free(mem, *mem);
}

/* reallocates memory using the system, resetting the size header */
static inline void *big_realloc(void *ptr, size_t new_size) {
  size_t *mem = (void *)(((uintptr_t)ptr) - 16);
  new_size = sys_round_size(new_size + 16);
  mem = sys_realloc(mem, *mem, new_size);
  if (!mem) goto error;
  *mem = new_size;
  return (void *)(((uintptr_t)mem) + 16);
error:
  return NULL;
}

/* *****************************************************************************
Allocator Initialization (initialize arenas and allocate a block for each CPU)
***************************************************************************** */

#if DEBUG
void fio_memory_dump_missing(void) {
  fprintf(stderr, "\n ==== Attempting Memory Dump (will crash) ====\n");
  if (fio_ls_embd_is_empty(&memory.available)) {
    fprintf(stderr, "- Memory dump attempt canceled\n");
    return;
  }
  block_node_s *smallest =
      FIO_LS_EMBD_OBJ(block_node_s, node, memory.available.next);
  FIO_LS_EMBD_FOR(&memory.available, node) {
    block_node_s *tmp = FIO_LS_EMBD_OBJ(block_node_s, node, node);
    if (smallest > tmp) smallest = tmp;
  }

  for (size_t i = 0;
       i < FIO_MEMORY_BLOCK_SIZE * FIO_MEMORY_BLOCKS_PER_ALLOCATION; ++i) {
    if ((((uintptr_t)smallest + i) & FIO_MEMORY_BLOCK_MASK) == 0) {
      i += 32;
      fprintf(stderr, "---block jump---\n");
      continue;
    }
    if (((char *)smallest)[i]) fprintf(stderr, "%c", ((char *)smallest)[i]);
  }
}
#else
#define fio_memory_dump_missing()
#endif

static void fio_mem_init(void) {
  if (arenas) return;

  ssize_t cpu_count = 0;
#ifdef _SC_NPROCESSORS_ONLN
  cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
#else
#warning Dynamic CPU core count is unavailable - assuming 8 cores for memory allocation pools.
#endif
  if (cpu_count <= 0) cpu_count = 8;
  memory.cores = cpu_count;
  arenas = big_alloc(sizeof(*arenas) * cpu_count);
  FIO_ASSERT_ALLOC(arenas);
  block_free(block_new());
  at_fork(NULL, NULL, fio_malloc_after_fork);
}

static void fio_mem_destroy(void) {
  if (!arenas) return;

  FIO_MEMORY_PRINT_BLOCK_STAT();

  for (size_t i = 0; i < memory.cores; ++i) {
    if (arenas[i].block) block_free(arenas[i].block);
    arenas[i].block = NULL;
  }
  if (!memory.forked && fio_ls_embd_any(&memory.available)) {
    FIO_LOG_WARNING(
        "facil.io detected memory traces remaining after cleanup"
        " - memory leak?");
    FIO_MEMORY_PRINT_BLOCK_STAT_END();
    size_t count = 0;
    FIO_LS_EMBD_FOR(&memory.available, node) { ++count; }
    FIO_LOG_DEBUG("Memory blocks in pool: %zu (%zu blocks per allocation).",
                  count, (size_t)FIO_MEMORY_BLOCKS_PER_ALLOCATION);
#if FIO_MEM_DUMP
    fio_memory_dump_missing();
#endif
  }
  big_free(arenas);
  arenas = NULL;
}
/* *****************************************************************************
Memory allocation / deacclocation API
***************************************************************************** */

void *fio_malloc(size_t size) {
  if (g_fio_mem_procs_set) {
    if (!size) {
      /* changed behavior prevents "allocation failed" test for `malloc(0)` */
      return (void *)(&on_malloc_zero);
    }
    /* fio_calloc simply delegates to fio_malloc and relies on the result
     * being pre-zeroed (see fio_calloc below); preserve that guarantee by
     * calloc'ing through the custom procs instead of malloc'ing. */
    return _fio_mem_procs_check_align16(_mem_calloc(&g_fio_mem_procs, 1, size),
                                        "fio_malloc");
  }
  /* Lazily initialize the arena on first real use, regardless of
   * FIO_OVERRIDE_MALLOC: fio_lib_init (which used to be the only caller of
   * fio_mem_init) is not guaranteed to have run yet by the time this is
   * reached; chttpsvr_start() creates its TLS context (which allocates via
   * fio_tls_new -> fio_calloc/fio_malloc) before triggering the engine's own
   * lazy pthread_once init. fio_mem_init() is idempotent (returns
   * immediately if arenas is already set), so this costs nothing on the
   * common path. Not safe against a genuine concurrent race between two
   * threads racing this same first call; same as the pre-existing
   * FIO_OVERRIDE_MALLOC code below ever was. */
  if (!arenas) fio_mem_init();
  if (!size) {
    /* changed behavior prevents "allocation failed" test for `malloc(0)` */
    return (void *)(&on_malloc_zero);
  }
  if (size >= FIO_MEMORY_BLOCK_ALLOC_LIMIT) {
    /* system allocation - must be block aligned */
    // FIO_LOG_WARNING("fio_malloc re-routed to mmap - big allocation");
    return big_alloc(size);
  }
  /* ceiling for 16 byte alignement, translated to 16 byte units */
  size = (size >> 4) + (!!(size & 15));
  arena_enter();
  void *mem = block_slice(size);
  arena_exit();
  return mem;
}

void *fio_calloc(size_t size, size_t count) {
  /* `size * count` can silently wrap on overflow; without this check a
   * caller (e.g. FIO_SET_CALLOC-instantiated containers) would receive a
   * far smaller buffer than the `size`/`count` it asked for and go on to
   * write/index past its true end - a heap buffer overflow, not merely a
   * missed allocation. Matches the standard calloc(3) contract of failing
   * outright on overflow. */
  if (size && count > (SIZE_MAX / size)) return NULL;
  return fio_malloc(size *
                    count);  // memory is pre-initialized by mmap or pool.
}

void fio_free(void *ptr) {
  if (!ptr || ptr == (void *)&on_malloc_zero) return;
  if (g_fio_mem_procs_set) {
    _mem_free(&g_fio_mem_procs, ptr);
    return;
  }
  if (((uintptr_t)ptr & FIO_MEMORY_BLOCK_MASK) == 16) {
    /* big allocation - direct from the system */
    big_free(ptr);
    return;
  }
  /* allocated within block */
  block_slice_free(ptr);
}

/**
 * Re-allocates memory. An attept to avoid copying the data is made only for big
 * memory allocations.
 *
 * This variation is slightly faster as it might copy less data
 */
void *fio_realloc2(void *ptr, size_t new_size, size_t copy_length) {
  if (!ptr || ptr == (void *)&on_malloc_zero) {
    return fio_malloc(new_size);
  }
  if (!new_size) {
    goto zero_size;
  }
  if (g_fio_mem_procs_set) {
    /* Mirror the arena's actual behavior: only `copy_length` bytes of old
     * data are guaranteed to survive a fio_realloc2 call (the arena achieves
     * this by allocating a fresh zeroed block and memcpy-ing just that much);
     * a real realloc() would otherwise happily preserve more, so the tail
     * beyond copy_length is explicitly zeroed to match. */
    void *new_mem = _fio_mem_procs_check_align16(
        _mem_realloc(&g_fio_mem_procs, ptr, new_size), "fio_realloc2");
    if (new_mem && new_size > copy_length)
      memset((uint8_t *)new_mem + copy_length, 0, new_size - copy_length);
    return new_mem;
  }
  if (((uintptr_t)ptr & FIO_MEMORY_BLOCK_MASK) == 16) {
    /* big reallocation - direct from the system */
    return big_realloc(ptr, new_size);
  }
  /* allocated within block - don't even try to expand the allocation */
  /* ceiling for 16 byte alignement, translated to 16 byte units */
  void *new_mem = fio_malloc(new_size);
  if (!new_mem) return NULL;
  new_size = ((new_size >> 4) + (!!(new_size & 15)));
  copy_length = ((copy_length >> 4) + (!!(copy_length & 15)));
  fio_memcpy(new_mem, ptr, copy_length > new_size ? new_size : copy_length);

  block_slice_free(ptr);
  return new_mem;
zero_size:
  fio_free(ptr);
  return fio_malloc(0);
}

void *fio_realloc(void *ptr, size_t new_size) {
  if (g_fio_mem_procs_set) {
    /* Unlike fio_realloc2, plain fio_realloc has no caller-supplied
     * copy_length and must preserve all old data, like standard realloc;
     * delegate directly instead of going through fio_realloc2's zero-fill
     * logic (which would discard everything beyond copy_length). */
    if (!ptr || ptr == (void *)&on_malloc_zero) return fio_malloc(new_size);
    if (!new_size) {
      fio_free(ptr);
      return fio_malloc(0);
    }
    return _fio_mem_procs_check_align16(
        _mem_realloc(&g_fio_mem_procs, ptr, new_size), "fio_realloc");
  }
  const size_t max_old =
      FIO_MEMORY_BLOCK_SIZE - ((uintptr_t)ptr & FIO_MEMORY_BLOCK_MASK);
  return fio_realloc2(ptr, new_size, max_old);
}

/**
 * Allocates memory directly using `mmap`, this is prefered for larger objects
 * that have a long lifetime.
 *
 * `fio_free` can be used for deallocating the memory.
 */
void *fio_mmap(size_t size) {
  if (!size) {
    return NULL;
  }
  if (g_fio_mem_procs_set)
    return _fio_mem_procs_check_align16(_mem_calloc(&g_fio_mem_procs, 1, size),
                                        "fio_mmap");
  return big_alloc(size);
}

/* *****************************************************************************
FIO_OVERRIDE_MALLOC - override glibc / library malloc
***************************************************************************** */
#if FIO_OVERRIDE_MALLOC
void *malloc(size_t size) { return fio_malloc(size); }
void *calloc(size_t size, size_t count) { return fio_calloc(size, count); }
void free(void *ptr) { fio_free(ptr); }
void *realloc(void *ptr, size_t new_size) { return fio_realloc(ptr, new_size); }
#endif

#endif

/* *****************************************************************************
Section Start Marker












                             Hash Functions and Base64

                  SipHash / SHA-1 / SHA-2 / Base64 / Hex encoding













***************************************************************************** */

/* *****************************************************************************
SipHash
***************************************************************************** */

#if __BIG_ENDIAN__ /* SipHash is Little Endian */
#define sip_local64(i) fio_bswap64((i))
#else
#define sip_local64(i) (i)
#endif

static inline uint64_t fio_siphash_xy(const void *data, size_t len, size_t x,
                                      size_t y, uint64_t key1, uint64_t key2) {
  /* initialize the 4 words */
  uint64_t v0 = (0x0706050403020100ULL ^ 0x736f6d6570736575ULL) ^ key1;
  uint64_t v1 = (0x0f0e0d0c0b0a0908ULL ^ 0x646f72616e646f6dULL) ^ key2;
  uint64_t v2 = (0x0706050403020100ULL ^ 0x6c7967656e657261ULL) ^ key1;
  uint64_t v3 = (0x0f0e0d0c0b0a0908ULL ^ 0x7465646279746573ULL) ^ key2;
  const uint8_t *w8 = data;
  uint8_t len_mod = len & 255;
  union {
    uint64_t i;
    uint8_t str[8];
  } word;

#define hash_map_SipRound         \
  do {                            \
    v2 += v3;                     \
    v3 = fio_lrot64(v3, 16) ^ v2; \
    v0 += v1;                     \
    v1 = fio_lrot64(v1, 13) ^ v0; \
    v0 = fio_lrot64(v0, 32);      \
    v2 += v1;                     \
    v0 += v3;                     \
    v1 = fio_lrot64(v1, 17) ^ v2; \
    v3 = fio_lrot64(v3, 21) ^ v0; \
    v2 = fio_lrot64(v2, 32);      \
  } while (0);

  while (len >= 8) {
    word.i = sip_local64(fio_str2u64(w8));
    v3 ^= word.i;
    /* Sip Rounds */
    for (size_t i = 0; i < x; ++i) {
      hash_map_SipRound;
    }
    v0 ^= word.i;
    w8 += 8;
    len -= 8;
  }
  word.i = 0;
  uint8_t *pos = word.str;
  switch (len) { /* fallthrough is intentional */
    case 7:
      pos[6] = w8[6];
      /* fallthrough */
    case 6:
      pos[5] = w8[5];
      /* fallthrough */
    case 5:
      pos[4] = w8[4];
      /* fallthrough */
    case 4:
      pos[3] = w8[3];
      /* fallthrough */
    case 3:
      pos[2] = w8[2];
      /* fallthrough */
    case 2:
      pos[1] = w8[1];
      /* fallthrough */
    case 1:
      pos[0] = w8[0];
  }
  word.str[7] = len_mod;

  /* last round */
  v3 ^= word.i;
  hash_map_SipRound;
  hash_map_SipRound;
  v0 ^= word.i;
  /* Finalization */
  v2 ^= 0xff;
  /* d iterations of SipRound */
  for (size_t i = 0; i < y; ++i) {
    hash_map_SipRound;
  }
  hash_map_SipRound;
  hash_map_SipRound;
  hash_map_SipRound;
  hash_map_SipRound;
  /* XOR it all together */
  v0 ^= v1 ^ v2 ^ v3;
#undef hash_map_SipRound
  return v0;
}

uint64_t fio_siphash13(const void *data, size_t len, uint64_t key1,
                       uint64_t key2) {
  return fio_siphash_xy(data, len, 1, 3, key1, key2);
}
