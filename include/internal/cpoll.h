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

/*
 * INTERNAL ONLY. The readiness interface of the event loop and of
 * ccol_select(): epoll(7) on Linux, kqueue(2) on the BSDs and macOS.
 *
 * The interface IS the epoll interface. The event loop is written against
 * epoll semantics, so on Linux every name below is the epoll name and the
 * compiled code is exactly what direct epoll calls give. Elsewhere the same
 * names emulate those semantics on kqueue, with these rules:
 *
 * - An interest in CCOL_POLL_IN, CCOL_POLL_RDHUP, CCOL_POLL_ERR or
 *   CCOL_POLL_HUP registers the read filter of the fd, and CCOL_POLL_OUT the
 *   write filter. A mask with neither kind deletes both filters, so a MOD to
 *   a hollow mask stops every report, as an epoll DEL does.
 * - Without CCOL_POLL_IN, the read filter of a socket carries a low-water
 *   mark that no read reaches, so it reports EOF and errors only, which is
 *   what epoll reports for such a mask. A pipe or FIFO ignores the low-water
 *   mark, so for one of those the read filter is left out and only the write
 *   filter (when wanted) remains.
 * - CCOL_POLL_ONESHOT becomes EV_DISPATCH on each filter: a filter that
 *   reports is disabled until the next ADD or MOD. The two filters of one fd
 *   are separate, so the second one can still report while a dispatch for
 *   the first is in flight; the event loop drops an event for an entry whose
 *   job is in flight and re-arms both filters from live state when the job
 *   ends, which is the same rule that covers a late epoll event.
 * - Each kevent becomes one event. A report for the read filter gives
 *   CCOL_POLL_IN. With EV_EOF it gives what epoll gives at EOF: for a socket
 *   CCOL_POLL_IN and CCOL_POLL_RDHUP, CCOL_POLL_ERR when the socket reports
 *   an error, and CCOL_POLL_HUP when poll(2) reports POLLHUP (both
 *   directions down); for a pipe or FIFO CCOL_POLL_HUP, with CCOL_POLL_IN
 *   only while data is still queued. A report for the write filter gives
 *   CCOL_POLL_OUT, and on EV_EOF CCOL_POLL_ERR or CCOL_POLL_HUP. Two filters
 *   of one fd that report in one batch arrive as two events.
 * - The EOF bits of the read filter depend on what the fd is, and only an
 *   fstat(2) and a poll(2) of the fd can tell. ccol_poll_wait() therefore
 *   makes no system call on an fd: it marks the event, and the caller calls
 *   ccol_poll_refine() with the fd while it can prove that the fd is not
 *   closed. The event loop does that under the stripe lock, for an entry
 *   that is not removed: the removal takes the same lock, and an fd is closed
 *   only after its removal. Another thread can close an fd as soon as the
 *   wait returns, and the number can then name a different object. An event
 *   that nobody refines carries no readiness bit. On Linux ccol_poll_refine()
 *   does nothing.
 * - An ADD of a regular file or a directory fails with EPERM, as epoll_ctl
 *   does. kqueue would accept them and report them ready for ever.
 * - CCOL_POLL_CTL_DEL deletes both filters and succeeds when either existed.
 *
 * The event array that ccol_poll_wait() fills is also the buffer that
 * kevent() writes into, so ccol_poll_event is as large as struct kevent there
 * and the wait allocates nothing.
 */

#ifndef CCOL_INTERNAL_CPOLL_H
#define CCOL_INTERNAL_CPOLL_H

#include <errno.h>
#include <stdint.h>

#if defined(__linux__)

#include <sys/epoll.h>

typedef struct epoll_event ccol_poll_event;

#define CCOL_POLL_IN EPOLLIN
#define CCOL_POLL_OUT EPOLLOUT
#define CCOL_POLL_RDHUP EPOLLRDHUP
#define CCOL_POLL_ERR EPOLLERR
#define CCOL_POLL_HUP EPOLLHUP
#define CCOL_POLL_ONESHOT EPOLLONESHOT

#define CCOL_POLL_CTL_ADD EPOLL_CTL_ADD
#define CCOL_POLL_CTL_MOD EPOLL_CTL_MOD
#define CCOL_POLL_CTL_DEL EPOLL_CTL_DEL

#define ccol_poll_create() epoll_create1(EPOLL_CLOEXEC)
#define ccol_poll_ctl(pfd, op, fd, ev) epoll_ctl((pfd), (op), (fd), (ev))
#define ccol_poll_wait(pfd, evs, max, timeout_ms) \
  epoll_wait((pfd), (evs), (max), (timeout_ms))

static inline void ccol_poll_refine(ccol_poll_event *ev, int fd) {
  (void)ev;
  (void)fd;
}

#elif defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The 64-bit tag of an event travels in the pointer-sized udata of its
 * kevent. */
_Static_assert(sizeof(void *) >= sizeof(uint64_t),
               "the kqueue backend needs 64-bit pointers");

typedef union ccol_poll_event {
  struct {
    uint32_t events;
    union {
      void *ptr;
      uint64_t u64;
    } data;
  };
  struct kevent _kev; /* the room that kevent() fills in place */
} ccol_poll_event;

#define CCOL_POLL_IN 0x001u
#define CCOL_POLL_OUT 0x004u
#define CCOL_POLL_ERR 0x008u
#define CCOL_POLL_HUP 0x010u
#define CCOL_POLL_RDHUP 0x2000u
#define CCOL_POLL_ONESHOT (1u << 30)

/* The marks that ccol_poll_wait() puts on an EOF of the read filter, for
 * ccol_poll_refine(): the EOF itself, an error that the filter reported, and
 * data that is still queued. */
#define _CCOL_POLL_EOF (1u << 28)
#define _CCOL_POLL_EOF_ERR (1u << 27)
#define _CCOL_POLL_EOF_DATA (1u << 26)

#define CCOL_POLL_CTL_ADD 1
#define CCOL_POLL_CTL_DEL 2
#define CCOL_POLL_CTL_MOD 3

/* No read reaches this low-water mark, so a read filter that carries it
 * reports only EOF and errors. */
#define _CCOL_POLL_LOWAT_NEVER INT_MAX

/* ThreadSanitizer intercepts epoll_ctl() and epoll_wait() and records that a
 * registration happens before every event that a wait returns for it; it
 * intercepts no kevent(). A ThreadSanitizer build therefore states the same
 * ordering here, keyed on the kqueue as the Linux runtime keys it on the
 * epoll descriptor: a release before each change, an acquire after each wait
 * that returns events. Without it, every object that one thread registers and
 * the poller then reaches through the udata of an event reads as a race,
 * although the kernel orders the two. Any other build compiles nothing of
 * this. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define _CCOL_POLL_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define _CCOL_POLL_TSAN 1
#endif
#ifdef _CCOL_POLL_TSAN
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
static inline void *_ccol_poll_tsan_sync(int kq) {
  static char sync[256];
  return &sync[(unsigned)kq & 255u];
}
#define _CCOL_POLL_TSAN_RELEASE(kq) __tsan_release(_ccol_poll_tsan_sync(kq))
#define _CCOL_POLL_TSAN_ACQUIRE(kq) __tsan_acquire(_ccol_poll_tsan_sync(kq))
#else
#define _CCOL_POLL_TSAN_RELEASE(kq) ((void)0)
#define _CCOL_POLL_TSAN_ACQUIRE(kq) ((void)0)
#endif

static inline int ccol_poll_create(void) {
  int kq = kqueue();
  if (kq >= 0) (void)fcntl(kq, F_SETFD, FD_CLOEXEC);
  return kq;
}

static inline int ccol_poll_ctl(int kq, int op, int fd,
                                const ccol_poll_event *ev) {
  struct kevent ch[2];
  if (op == CCOL_POLL_CTL_DEL) {
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    bool any = false;
    for (int i = 0; i < 2; i++) {
      if (kevent(kq, &ch[i], 1, NULL, 0, NULL) == 0) any = true;
    }
    if (any) return 0;
    errno = ENOENT;
    return -1;
  }

  struct stat st;
  if (op == CCOL_POLL_CTL_ADD) {
    if (fstat(fd, &st) != 0) return -1;
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
      errno = EPERM;
      return -1;
    }
  }

  uint32_t m = ev->events;
  unsigned short dispatch = (m & CCOL_POLL_ONESHOT) ? EV_DISPATCH : 0;
  void *udata = (void *)(uintptr_t)ev->data.u64;
  bool want_read =
      (m & (CCOL_POLL_IN | CCOL_POLL_RDHUP | CCOL_POLL_ERR | CCOL_POLL_HUP)) !=
      0;
  bool want_write = (m & CCOL_POLL_OUT) != 0;
  unsigned int rfflags = 0;
  int64_t rdata = 0;
  if (want_read && !(m & CCOL_POLL_IN)) {
    /* Errors and EOF only: a low-water mark on a socket, and nothing on a
     * descriptor that ignores it. */
    if (op != CCOL_POLL_CTL_ADD && fstat(fd, &st) != 0) return -1;
    if (S_ISSOCK(st.st_mode)) {
      rfflags = NOTE_LOWAT;
      rdata = _CCOL_POLL_LOWAT_NEVER;
    } else {
      want_read = false;
    }
  }

  unsigned short receipt = EV_RECEIPT;
  if (want_read) {
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ,
           EV_ADD | EV_ENABLE | dispatch | receipt, rfflags, rdata, udata);
  } else {
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE | receipt, 0, 0, NULL);
  }
  if (want_write) {
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE,
           EV_ADD | EV_ENABLE | dispatch | receipt, 0, 0, udata);
  } else {
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE | receipt, 0, 0,
           NULL);
  }
  /* EV_RECEIPT makes kevent() apply both changes in one call and report the
   * result of each as an EV_ERROR record whose data is 0 or an errno. A
   * delete of a filter that is not there reports ENOENT, which is no error
   * here. */
  struct kevent res[2];
  struct timespec zero = {0, 0};
  _CCOL_POLL_TSAN_RELEASE(kq);
  int n = kevent(kq, ch, 2, res, 2, &zero);
  if (n < 0) return -1;
  for (int i = 0; i < n; i++) {
    if (!(res[i].flags & EV_ERROR) || res[i].data == 0) continue;
    bool was_delete = (res[i].filter == EVFILT_READ) ? !want_read : !want_write;
    if (was_delete && res[i].data == ENOENT) continue;
    errno = (int)res[i].data;
    return -1;
  }
  return 0;
}

static inline int ccol_poll_wait(int kq, ccol_poll_event *evs, int max,
                                 int timeout_ms) {
  struct timespec ts, *tsp = NULL;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    tsp = &ts;
  }
  /* kevent() writes its records straight into evs: each element is a
   * struct kevent in size and layout. Each record is copied out and then
   * translated in place. */
  _Static_assert(sizeof(ccol_poll_event) == sizeof(struct kevent),
                 "ccol_poll_event must have the stride of struct kevent");
  int n = kevent(kq, NULL, 0, (struct kevent *)evs, max, tsp);
  if (n > 0) _CCOL_POLL_TSAN_ACQUIRE(kq);
  for (int i = 0; i < n; i++) {
    struct kevent k = evs[i]._kev;
    uint32_t bits = 0;
    if (k.filter == EVFILT_READ) {
      /* A report without EOF is readable data, or, for a filter that
       * carries the unreachable low-water mark, a pending socket error;
       * the event loop routes a readable report that no reader takes to
       * on_error, so both arrive where epoll sends them. */
      if (!(k.flags & EV_EOF)) {
        bits |= CCOL_POLL_IN;
      } else {
        bits |= _CCOL_POLL_EOF;
        if (k.fflags != 0) bits |= _CCOL_POLL_EOF_ERR;
        if (k.data > 0) bits |= _CCOL_POLL_EOF_DATA;
      }
    } else if (k.filter == EVFILT_WRITE) {
      bits |= CCOL_POLL_OUT;
      if (k.flags & EV_EOF)
        bits |= (k.fflags != 0) ? CCOL_POLL_ERR : CCOL_POLL_HUP;
    }
    evs[i].events = bits;
    evs[i].data.u64 = (uint64_t)(uintptr_t)k.udata;
  }
  return n;
}

/* Turns the marks of an EOF of the read filter into the bits that epoll gives
 * for the fd. The caller must prove that fd is still open and is still the fd
 * of the registration; see the rules at the top of this file. An event
 * without the marks stays as it is. */
static inline void ccol_poll_refine(ccol_poll_event *ev, int fd) {
  uint32_t m = ev->events;
  if (!(m & _CCOL_POLL_EOF)) return;
  uint32_t bits =
      m & ~(_CCOL_POLL_EOF | _CCOL_POLL_EOF_ERR | _CCOL_POLL_EOF_DATA);
  /* EV_EOF says only that no more data comes; epoll says more, and the event
   * loop routes on it. A socket at EOF is readable with EPOLLRDHUP, and
   * EPOLLHUP joins them when both directions are down (a closed Unix socket,
   * not a TCP close or half-close), which poll(2) reports as POLLHUP. A pipe
   * or FIFO whose writer is gone is EPOLLHUP alone, with EPOLLIN only while
   * data is still queued. */
  struct stat est;
  if (fstat(fd, &est) == 0 && S_ISSOCK(est.st_mode)) {
    bits |= CCOL_POLL_IN | CCOL_POLL_RDHUP;
    if (m & _CCOL_POLL_EOF_ERR) bits |= CCOL_POLL_ERR;
    struct pollfd pfd = {.fd = fd, .events = 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLHUP)) bits |= CCOL_POLL_HUP;
  } else {
    bits |= CCOL_POLL_HUP;
    if (m & _CCOL_POLL_EOF_DATA) bits |= CCOL_POLL_IN;
  }
  ev->events = bits;
}

#else
#error "c_collections needs epoll(7) or kqueue(2)"
#endif

/* A wake descriptor: one descriptor that a poller watches for readability,
 * that ccol_wakefd_notify() makes readable and ccol_wakefd_drain() makes
 * quiet again. It is an eventfd(2) where the system has one (Linux, FreeBSD).
 * macOS has none, and there it is a kqueue of its own that holds one
 * EVFILT_USER event: a trigger makes the kqueue readable, and collecting the
 * event, which carries EV_CLEAR, makes it quiet. _CCOL_EMULATE_DARWIN_WAKEFD
 * selects that form on FreeBSD too, which is how its test suites run it.
 * ccol_wakefd_create() gives the descriptor, closed on exec and
 * non-blocking, or -1 with errno set; close(2) releases it. */
#if defined(__APPLE__) && !defined(_CCOL_EMULATE_DARWIN_WAKEFD)
#define _CCOL_EMULATE_DARWIN_WAKEFD 1
#endif

#if defined(_CCOL_EMULATE_DARWIN_WAKEFD)
#include <sys/event.h>
#include <unistd.h>

static inline int ccol_wakefd_create(void) {
  int kq = kqueue();
  if (kq < 0) return -1;
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  if (fcntl(kq, F_SETFD, FD_CLOEXEC) != 0 ||
      kevent(kq, &ev, 1, NULL, 0, NULL) != 0) {
    int saved = errno;
    close(kq);
    errno = saved;
    return -1;
  }
  return kq;
}

static inline void ccol_wakefd_notify(int fd) {
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  while (kevent(fd, &ev, 1, NULL, 0, NULL) < 0 && errno == EINTR) {
  }
}

static inline void ccol_wakefd_drain(int fd) {
  struct kevent ev;
  struct timespec zero = {0, 0};
  while (kevent(fd, NULL, 0, &ev, 1, &zero) < 0 && errno == EINTR) {
  }
}
#else
#include <sys/eventfd.h>
#include <unistd.h>

static inline int ccol_wakefd_create(void) {
  return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

/* EAGAIN means the counter is already at its ceiling, which the small and
 * bounded counts of this library never reach; EBADF can only be a bug of
 * the caller. Every call site is best effort, so only EINTR is retried. */
static inline void ccol_wakefd_notify(int fd) {
  uint64_t one = 1;
  while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}

/* EAGAIN says that nothing is pending, which is the expected answer. */
static inline void ccol_wakefd_drain(int fd) {
  uint64_t val;
  while (read(fd, &val, sizeof(val)) < 0 && errno == EINTR) {
  }
}
#endif

#endif /* CCOL_INTERNAL_CPOLL_H */
