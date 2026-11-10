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

// Internal only. The readiness interface behind the event loop and cselect():
// epoll(7) on Linux and kqueue(2) on the BSDs and macOS. The interface is
// epoll's. On Linux every name below is the epoll name, so the code compiles to
// plain epoll; elsewhere the same names emulate epoll on top of kqueue:
//
// A mask with IN, RDHUP, ERR or HUP registers the read filter, OUT the write
// filter, and a mask with neither deletes both (like an epoll DEL). OUT without
// IN or RDHUP arms the write filter alone, so the EOF of the receive direction
// isn't reported. Without IN a socket's read filter has a low-water mark that
// no read reaches, so it reports only EOF and errors, with EV_CLEAR; a pipe,
// FIFO or terminal ignores that mark and is watched by the filter of the end it
// was opened for. ONESHOT becomes EV_DISPATCH (never on an EV_CLEAR filter). A
// MOD that flips IN must carry CCOL_POLL_READ_MODE_CHANGE, because kqueue keeps
// the trigger flags a filter got at creation.
//
// ccol_poll_wait() makes no system call on an fd, since another thread can
// close it as soon as the wait returns. It marks a read filter EOF instead, and
// the caller calls ccol_poll_refine() where it can prove the fd is still open
// (the event loop does it under the stripe lock that removal takes). Only a
// refined event carries the bits epoll would give for that fd and mask, and it
// can refine to no bit at all, which the caller ignores. An ADD of a regular
// file or a directory fails with EPERM as in epoll. A ccol_poll_fdinfo (may be
// NULL) caches what the fd is and how its write filter was created, so a MOD
// asks the kernel nothing; keep it under the same lock as every call for the
// fd.
//
// When the event loop needs something new from the kernel, add it here in epoll
// terms and emulate it on kqueue, so the loop keeps one set of semantics.

#ifndef CCOL_INTERNAL_CPOLL_H
#define CCOL_INTERNAL_CPOLL_H

#include <errno.h>
#include <stdint.h>

#if defined(__linux__)

#include <sys/epoll.h>

typedef struct epoll_event ccol_poll_event;

// epoll needs to know nothing about an fd; see the kqueue version below.
typedef uint8_t ccol_poll_fdinfo;

// Interest and report bits, control operations and the calls themselves are
// the epoll ones, so the Linux build is plain epoll. ccol_poll_create(),
// ccol_poll_ctl() and ccol_poll_wait() evaluate to what the epoll call gives;
// ccol_poll_ctl() ignores info. CCOL_POLL_READ_MODE_CHANGE is 0 since epoll
// rebuilds nothing.
#define CCOL_POLL_IN EPOLLIN
#define CCOL_POLL_OUT EPOLLOUT
#define CCOL_POLL_RDHUP EPOLLRDHUP
#define CCOL_POLL_ERR EPOLLERR
#define CCOL_POLL_HUP EPOLLHUP
#define CCOL_POLL_ONESHOT EPOLLONESHOT
#define CCOL_POLL_READ_MODE_CHANGE 0u

#define CCOL_POLL_CTL_ADD EPOLL_CTL_ADD
#define CCOL_POLL_CTL_MOD EPOLL_CTL_MOD
#define CCOL_POLL_CTL_DEL EPOLL_CTL_DEL

#define ccol_poll_create() epoll_create1(EPOLL_CLOEXEC)
#define ccol_poll_ctl(pfd, op, fd, ev, info) \
  ((void)(info), epoll_ctl((pfd), (op), (fd), (ev)))
#define ccol_poll_wait(pfd, evs, max, timeout_ms) \
  epoll_wait((pfd), (evs), (max), (timeout_ms))

// Does nothing with epoll, whose events already carry the right bits; it
// exists so the callers need no #ifdef.
static inline void ccol_poll_refine(ccol_poll_event *ev, int fd, uint32_t mask,
                                    ccol_poll_fdinfo *info) {
  (void)ev;
  (void)fd;
  (void)mask;
  (void)info;
}

#elif defined(__FreeBSD__) || defined(__APPLE__) || defined(__OpenBSD__)

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// An event's 64-bit tag rides in the pointer-sized udata of its kevent.
_Static_assert(sizeof(void *) >= sizeof(uint64_t),
               "the kqueue backend needs 64-bit pointers");

// The epoll-shaped event, overlaid on a struct kevent so the wait writes into
// the caller's array and allocates nothing.
typedef union ccol_poll_event {
  struct {
    uint32_t events;
    union {
      void *ptr;
      uint64_t u64;
    } data;
  };
  struct kevent ccol__kev;
} ccol_poll_event;

// The epoll bit values.
#define CCOL_POLL_IN 0x001u
#define CCOL_POLL_OUT 0x004u
#define CCOL_POLL_ERR 0x008u
#define CCOL_POLL_HUP 0x010u
#define CCOL_POLL_RDHUP 0x2000u
#define CCOL_POLL_ONESHOT (1u << 30)
// Flag for a MOD whose CCOL_POLL_IN differs from the previous mask: the read
// filter is deleted and created again, since kqueue keeps its trigger flags.
#define CCOL_POLL_READ_MODE_CHANGE (1u << 29)

// Marks that ccol_poll_wait() leaves for ccol_poll_refine(): a read filter
// EOF, an error it reported, data still queued, and a report from a socket's
// error-only read filter (which, without EOF, is a pending socket error).
#define CCOL__POLL_EOF (1u << 28)
#define CCOL__POLL_EOF_ERR (1u << 27)
#define CCOL__POLL_EOF_DATA (1u << 26)
#define CCOL__POLL_ERR_ONLY_FILTER (1u << 25)

// The udata bit that marks the error-only read filter. Every caller tag has it
// clear: an event loop entry is a user-space address and cselect() uses bit 63
// with a descriptor or a small index.
#define CCOL__POLL_UDATA_ERR_ONLY ((uint64_t)1 << 62)

// The control operations.
#define CCOL_POLL_CTL_ADD 1
#define CCOL_POLL_CTL_DEL 2
#define CCOL_POLL_CTL_MOD 3

// A low-water mark no read reaches, for a read filter of errors and EOF only.
#define CCOL__POLL_LOWAT_NEVER INT_MAX

// ThreadSanitizer intercepts epoll but not kevent(), so a TSan build states
// the ordering itself: a release on the kqueue before each change and an
// acquire after each wait that returns events. Without it every object reached
// through an event's udata looks like a race. Only that build compiles this.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CCOL__POLL_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define CCOL__POLL_TSAN 1
#endif
#ifdef CCOL__POLL_TSAN
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);

// Gives the address that stands for kq in ThreadSanitizer's model, one of 256
// static bytes picked by the descriptor, for the release and acquire macros.
static inline void *ccol__poll_tsan_sync(int kq) {
  static char sync[256];
  return &sync[(unsigned)kq & 255u];
}
#define CCOL__POLL_TSAN_RELEASE(kq) __tsan_release(ccol__poll_tsan_sync(kq))
#define CCOL__POLL_TSAN_ACQUIRE(kq) __tsan_acquire(ccol__poll_tsan_sync(kq))
#else
#define CCOL__POLL_TSAN_RELEASE(kq) ((void)0)
#define CCOL__POLL_TSAN_ACQUIRE(kq) ((void)0)
#endif

// Creates a kqueue, closed on exec, for the event loop and cselect(). Gives
// its fd or -1.
static inline int ccol_poll_create(void) {
  int kq = kqueue();
  if (kq >= 0) (void)fcntl(kq, F_SETFD, FD_CLOEXEC);
  return kq;
}

// Says whether the socket fd listens, since a listener's read filter ignores
// the low-water mark and the error-only filter must not arm it. FreeBSD and
// OpenBSD answer SO_ACCEPTCONN; macOS refuses it and gives the flag through
// proc_pidfdinfo(). A failed query answers false, so the error-only filter is
// armed as for any other socket.
static inline bool ccol__poll_is_listener(int fd) {
#if defined(__APPLE__)
  struct socket_fdinfo si;
  if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &si, sizeof(si)) ==
      (int)sizeof(si))
    return (si.psi.soi_options & SO_ACCEPTCONN) != 0;
#else
  int listening = 0;
  socklen_t ll = sizeof(listening);
  if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &ll) == 0)
    return listening != 0;
#endif
  return false;
}

// What ccol_poll_ctl() knows about an fd. Zero means nothing yet.
typedef uint16_t ccol_poll_fdinfo;

// The bits of ccol_poll_fdinfo: KNOWN says the kind bits (SOCK, RD for opened
// for reading, ANON_PIPE for a FIFO with no device and inode) are filled;
// LISTEN_KNOWN says LISTENER is; WFILT says a write filter is in place and
// WCLEAR that it carries EV_CLEAR.
#define CCOL__POLL_FI_KNOWN 0x001u
#define CCOL__POLL_FI_SOCK 0x002u
#define CCOL__POLL_FI_RD 0x004u
#define CCOL__POLL_FI_ANON_PIPE 0x010u
#define CCOL__POLL_FI_LISTEN_KNOWN 0x020u
#define CCOL__POLL_FI_LISTENER 0x040u
#define CCOL__POLL_FI_WFILT 0x080u
#define CCOL__POLL_FI_WCLEAR 0x100u

// Asks the fd what it is (socket, read end, anonymous pipe) and fills *fi, so
// a MOD or a refine needn't ask again. A regular file or a directory fails with
// EPERM, as epoll_ctl() does. Gives 0 or -1 with errno set.
static inline int ccol__poll_fdinfo_fill(int fd, ccol_poll_fdinfo *fi) {
  struct stat st;
  if (fstat(fd, &st) != 0) return -1;
  if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
    errno = EPERM;
    return -1;
  }
  ccol_poll_fdinfo f = CCOL__POLL_FI_KNOWN;
  if (S_ISSOCK(st.st_mode)) {
    f |= CCOL__POLL_FI_SOCK | CCOL__POLL_FI_RD;
  } else {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -1;
    if ((fl & O_ACCMODE) != O_WRONLY) f |= CCOL__POLL_FI_RD;
    if (S_ISFIFO(st.st_mode) && st.st_dev == 0 && st.st_ino == 0)
      f |= CCOL__POLL_FI_ANON_PIPE;
  }
  *fi = f;
  return 0;
}

// Says whether the socket listens, from the cache when *fi knows and asking
// the fd (and caching the answer) otherwise.
static inline bool ccol__poll_fdinfo_listener(int fd, ccol_poll_fdinfo *fi) {
  if (!(*fi & CCOL__POLL_FI_LISTEN_KNOWN)) {
    *fi |= CCOL__POLL_FI_LISTEN_KNOWN;
    if (ccol__poll_is_listener(fd)) *fi |= CCOL__POLL_FI_LISTENER;
  }
  return (*fi & CCOL__POLL_FI_LISTENER) != 0;
}

// epoll_ctl() on kqueue. DEL deletes both filters and succeeds when either
// existed. Otherwise both filter changes go in one kevent() call with
// EV_RECEIPT, and a delete of a missing filter (ENOENT) isn't an error. For an
// error-only mask: a socket that also wants OUT uses the write filter alone; a
// listener arms no read filter (it would report every pending connection); any
// other socket gets the low-water mark with EV_CLEAR; a write end uses its
// write filter and a read end its read filter, both with EV_CLEAR. An OpenBSD
// anonymous pipe always uses the read filter, since OpenBSD doesn't wake a
// write filter armed before the reader closes. An EV_CLEAR filter is never
// one-shot: an event that refines to nothing would leave it disabled for good.
// A write filter that switches EV_CLEAR on or off is recreated. Updates the
// write filter bits of *info.
static inline int ccol_poll_ctl(int kq, int op, int fd,
                                const ccol_poll_event *ev,
                                ccol_poll_fdinfo *info) {
  struct kevent ch[2];
  if (op == CCOL_POLL_CTL_DEL) {
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    bool any = false;
    for (int i = 0; i < 2; i++) {
      if (kevent(kq, &ch[i], 1, NULL, 0, NULL) == 0) any = true;
    }
    if (info)
      *info &= (ccol_poll_fdinfo) ~(CCOL__POLL_FI_WFILT | CCOL__POLL_FI_WCLEAR);
    if (any) return 0;
    errno = ENOENT;
    return -1;
  }

  ccol_poll_fdinfo local = 0;
  ccol_poll_fdinfo *fi = info ? info : &local;
  if (op == CCOL_POLL_CTL_ADD)
    *fi &= (ccol_poll_fdinfo) ~(CCOL__POLL_FI_WFILT | CCOL__POLL_FI_WCLEAR);
  uint32_t m = ev->events;
  unsigned short dispatch = (m & CCOL_POLL_ONESHOT) ? EV_DISPATCH : 0;
  void *udata = (void *)(uintptr_t)ev->data.u64;
  bool want_read =
      (m & (CCOL_POLL_IN | CCOL_POLL_RDHUP | CCOL_POLL_ERR | CCOL_POLL_HUP)) !=
      0;
  bool want_write = (m & CCOL_POLL_OUT) != 0;
  unsigned int rfflags = 0;
  int64_t rdata = 0;
  unsigned short rclear = 0;
  unsigned short wclear = 0;
  uint64_t rmark = 0;
  bool error_only_read = want_read && !(m & CCOL_POLL_IN);
  if ((op == CCOL_POLL_CTL_ADD || error_only_read) &&
      !(*fi & CCOL__POLL_FI_KNOWN) && ccol__poll_fdinfo_fill(fd, fi) != 0)
    return -1;
  bool is_sock = (*fi & CCOL__POLL_FI_SOCK) != 0;
  if (error_only_read) {
    if (is_sock && want_write && !(m & CCOL_POLL_RDHUP)) {
      want_read = false;
    } else if (is_sock && ccol__poll_fdinfo_listener(fd, fi)) {
      want_read = false;
    } else if (is_sock) {
      rfflags = NOTE_LOWAT;
      rdata = CCOL__POLL_LOWAT_NEVER;
      rclear = EV_CLEAR;
      rmark = CCOL__POLL_UDATA_ERR_ONLY;
#if defined(__OpenBSD__)
    } else if (*fi & CCOL__POLL_FI_ANON_PIPE) {
      rclear = EV_CLEAR;
#endif
    } else if (want_write) {
      want_read = false;
    } else if (!(*fi & CCOL__POLL_FI_RD)) {
      want_read = false;
      want_write = true;
      wclear = EV_CLEAR;
    } else {
      rclear = EV_CLEAR;
    }
  }

  if (op == CCOL_POLL_CTL_MOD && (m & CCOL_POLL_READ_MODE_CHANGE)) {
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    CCOL__POLL_TSAN_RELEASE(kq);
    (void)kevent(kq, &ch[0], 1, NULL, 0, NULL);
  }
  if (want_write && (*fi & CCOL__POLL_FI_WFILT) &&
      ((*fi & CCOL__POLL_FI_WCLEAR) != 0) != (wclear != 0)) {
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    CCOL__POLL_TSAN_RELEASE(kq);
    (void)kevent(kq, &ch[1], 1, NULL, 0, NULL);
  }
  unsigned short receipt = EV_RECEIPT;
  if (want_read) {
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ,
           EV_ADD | EV_ENABLE | (rclear ? 0 : dispatch) | rclear | receipt,
           rfflags, rdata,
           (void *)(uintptr_t)((uint64_t)(uintptr_t)udata | rmark));
  } else {
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE | receipt, 0, 0, NULL);
  }
  if (want_write) {
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE,
           EV_ADD | EV_ENABLE | (wclear ? 0 : dispatch) | wclear | receipt, 0,
           0, udata);
  } else {
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE | receipt, 0, 0,
           NULL);
  }
  struct kevent res[2];
  struct timespec zero = {0, 0};
  CCOL__POLL_TSAN_RELEASE(kq);
  int n = kevent(kq, ch, 2, res, 2, &zero);
  if (n < 0) return -1;
  int fail = 0;
  bool write_added = want_write;
  for (int i = 0; i < n; i++) {
    if (!(res[i].flags & EV_ERROR) || res[i].data == 0) continue;
    bool was_delete = (res[i].filter == EVFILT_READ) ? !want_read : !want_write;
    if (was_delete && res[i].data == ENOENT) continue;
    if (res[i].filter == EVFILT_WRITE) write_added = false;
    fail = (int)res[i].data;
  }
  *fi &= (ccol_poll_fdinfo) ~(CCOL__POLL_FI_WFILT | CCOL__POLL_FI_WCLEAR);
  if (write_added)
    *fi |= (ccol_poll_fdinfo)(CCOL__POLL_FI_WFILT |
                              (wclear ? CCOL__POLL_FI_WCLEAR : 0));
  if (fail != 0) {
    errno = fail;
    return -1;
  }
  return 0;
}

// epoll_wait() on kqueue. kevent() writes straight into evs and each record
// is translated in place: a read report gives IN (or EOF marks), a write report
// gives OUT, with ERR or HUP at its EOF. No call is made on any fd.
static inline int ccol_poll_wait(int kq, ccol_poll_event *evs, int max,
                                 int timeout_ms) {
  struct timespec ts, *tsp = NULL;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    tsp = &ts;
  }
  _Static_assert(sizeof(ccol_poll_event) == sizeof(struct kevent),
                 "ccol_poll_event must have the stride of struct kevent");
  int n = kevent(kq, NULL, 0, (struct kevent *)evs, max, tsp);
  if (n > 0) CCOL__POLL_TSAN_ACQUIRE(kq);
  for (int i = 0; i < n; i++) {
    struct kevent k = evs[i].ccol__kev;
    uint32_t bits = 0;
    if (k.filter == EVFILT_READ) {
      if ((uint64_t)(uintptr_t)k.udata & CCOL__POLL_UDATA_ERR_ONLY)
        bits |= CCOL__POLL_ERR_ONLY_FILTER;
      if (!(k.flags & EV_EOF)) {
        bits |= CCOL_POLL_IN;
      } else {
        bits |= CCOL__POLL_EOF;
        if (k.fflags != 0) bits |= CCOL__POLL_EOF_ERR;
        if (k.data > 0) bits |= CCOL__POLL_EOF_DATA;
      }
    } else if (k.filter == EVFILT_WRITE) {
      bits |= CCOL_POLL_OUT;
      if (k.flags & EV_EOF)
        bits |= (k.fflags != 0) ? CCOL_POLL_ERR : CCOL_POLL_HUP;
    }
    evs[i].events = bits;
    evs[i].data.u64 = (uint64_t)(uintptr_t)k.udata & ~CCOL__POLL_UDATA_ERR_ONLY;
  }
  return n;
}

// Turns the marks of a read filter report into the bits epoll would give, and
// keeps only mask, ERR and HUP. The caller must prove fd is still open and
// still belongs to the registration. A socket EOF gives IN|RDHUP, plus ERR for
// an error and HUP when poll(POLLOUT) reports POLLHUP (both directions down);
// macOS poll reports POLLHUP only for the filters asked for, hence POLLOUT. A
// pipe EOF gives HUP, with IN while data is queued. A report from the
// error-only filter is dropped for a listener (and the listener is cached in
// *info) and on macOS for a full buffer, which its clamped low-water mark
// reports; one proc_pidfdinfo() call tells those apart without clearing the
// pending error. What remains of it becomes ERR.
static inline void ccol_poll_refine(ccol_poll_event *ev, int fd, uint32_t mask,
                                    ccol_poll_fdinfo *info) {
  uint32_t m = ev->events;
  uint32_t keep = mask | CCOL_POLL_ERR | CCOL_POLL_HUP;
  bool err_only_filter = (m & CCOL__POLL_ERR_ONLY_FILTER) != 0;
  m &= ~CCOL__POLL_ERR_ONLY_FILTER;
  if (!(m & CCOL__POLL_EOF)) {
#if defined(__APPLE__)
    if (err_only_filter && (m & CCOL_POLL_IN)) {
      struct socket_fdinfo si;
      if (info && (*info & CCOL__POLL_FI_LISTENER)) {
        m &= ~CCOL_POLL_IN;
      } else if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &si,
                                sizeof(si)) == (int)sizeof(si)) {
        if (si.psi.soi_options & SO_ACCEPTCONN) {
          if (info)
            *info |= CCOL__POLL_FI_LISTEN_KNOWN | CCOL__POLL_FI_LISTENER;
          m &= ~CCOL_POLL_IN;
        } else if (si.psi.soi_error == 0) {
          m &= ~CCOL_POLL_IN;
        }
      } else {
        int nread = 0, rcvbuf = 0, type = 0;
        socklen_t nl = sizeof(nread), rl = sizeof(rcvbuf), tl = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_NREAD, &nread, &nl) == 0 &&
            getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &rl) == 0 &&
            getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) == 0 &&
            (type == SOCK_STREAM ? nread >= rcvbuf : nread > 0))
          m &= ~CCOL_POLL_IN;
      }
    }
#else
    if (err_only_filter && (m & CCOL_POLL_IN)) {
      bool listener;
      if (info && (*info & CCOL__POLL_FI_LISTENER)) {
        listener = true;
      } else {
        listener = ccol__poll_is_listener(fd);
        if (listener && info)
          *info |= CCOL__POLL_FI_LISTEN_KNOWN | CCOL__POLL_FI_LISTENER;
      }
      if (listener) m &= ~CCOL_POLL_IN;
    }
#endif
    if (err_only_filter && (m & CCOL_POLL_IN))
      m = (m & ~CCOL_POLL_IN) | CCOL_POLL_ERR;
    ev->events = m & keep;
    return;
  }
  uint32_t bits =
      m & ~(CCOL__POLL_EOF | CCOL__POLL_EOF_ERR | CCOL__POLL_EOF_DATA);
  bool is_sock;
  if (info && (*info & CCOL__POLL_FI_KNOWN)) {
    is_sock = (*info & CCOL__POLL_FI_SOCK) != 0;
  } else {
    struct stat est;
    is_sock = fstat(fd, &est) == 0 && S_ISSOCK(est.st_mode);
  }
  if (is_sock) {
    bits |= CCOL_POLL_IN | CCOL_POLL_RDHUP;
    if (m & CCOL__POLL_EOF_ERR) bits |= CCOL_POLL_ERR;
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLHUP)) bits |= CCOL_POLL_HUP;
  } else {
    bits |= CCOL_POLL_HUP;
    if (m & CCOL__POLL_EOF_DATA) bits |= CCOL_POLL_IN;
  }
  ev->events = bits & keep;
}

#else
#error "c_collections needs epoll(7) or kqueue(2)"
#endif

// A wake descriptor is one fd that a poller watches for readability;
// ccol_wakefd_notify() makes it readable and ccol_wakefd_drain() makes it
// quiet. It's an eventfd on Linux and FreeBSD. macOS and OpenBSD have none, so
// there it's a kqueue of its own with one EVFILT_USER event that carries
// EV_CLEAR. CCOL__EMULATE_DARWIN_WAKEFD selects that form on FreeBSD too, for
// the tests. ccol_wakefd_create() gives it closed on exec and non-blocking, or
// -1 with errno set; close(2) releases it.
#if (defined(__APPLE__) || defined(__OpenBSD__)) && \
    !defined(CCOL__EMULATE_DARWIN_WAKEFD)
#define CCOL__EMULATE_DARWIN_WAKEFD 1
#endif

#if defined(CCOL__EMULATE_DARWIN_WAKEFD)
#include <sys/event.h>
#include <unistd.h>

// Creates the wake descriptor (a kqueue with its user event) that the loop and
// the queue bridges wait on. Gives its fd or -1 with errno set.
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

// Makes the wake descriptor readable by triggering the user event.
static inline void ccol_wakefd_notify(int fd) {
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  while (kevent(fd, &ev, 1, NULL, 0, NULL) < 0 && errno == EINTR) {
  }
}

// Makes the wake descriptor quiet by collecting the event without waiting,
// which clears it.
static inline void ccol_wakefd_drain(int fd) {
  struct kevent ev;
  struct timespec zero = {0, 0};
  while (kevent(fd, NULL, 0, &ev, 1, &zero) < 0 && errno == EINTR) {
  }
}
#else
#include <sys/eventfd.h>
#include <unistd.h>

// Creates the wake descriptor (an eventfd) that the loop and the queue bridges
// wait on. Gives its fd or -1 with errno set.
static inline int ccol_wakefd_create(void) {
  return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

// Makes the wake descriptor readable by writing 1. EAGAIN means the counter
// is at its ceiling, which our small counts never reach, and every call site is
// best effort, so only EINTR is retried.
static inline void ccol_wakefd_notify(int fd) {
  uint64_t one = 1;
  while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}

// Makes the wake descriptor quiet by reading the counter. EAGAIN just says
// nothing is pending.
static inline void ccol_wakefd_drain(int fd) {
  uint64_t val;
  while (read(fd, &val, sizeof(val)) < 0 && errno == EINTR) {
  }
}
#endif

#endif
