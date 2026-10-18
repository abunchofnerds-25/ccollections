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
 * INTERNAL ONLY. The readiness interface behind the event loop and
 * ccol_select(): epoll(7) on Linux, kqueue(2) on the BSDs and macOS.
 *
 * The interface is the epoll interface. The event loop is written against
 * epoll semantics, so on Linux every name below is just the epoll name and
 * the compiled code is the same as calling epoll directly. On the other
 * systems the same names emulate those semantics on top of kqueue, and the
 * emulation follows these rules.
 *
 * - Interest in CCOL_POLL_IN, CCOL_POLL_RDHUP, CCOL_POLL_ERR or
 *   CCOL_POLL_HUP registers the fd's read filter, and CCOL_POLL_OUT its write
 *   filter. A mask with neither kind deletes both filters, so a MOD to such a
 *   hollow mask stops every report the way an epoll DEL does. A mask with
 *   CCOL_POLL_OUT but neither CCOL_POLL_IN nor CCOL_POLL_RDHUP arms only the
 *   write filter, because that filter's EOF already gives CCOL_POLL_ERR or
 *   CCOL_POLL_HUP and a read filter would report the EOF of the receive
 *   direction, which the mask didn't ask for.
 * - Without CCOL_POLL_IN a socket's read filter carries a low-water mark that
 *   no read ever reaches, so it reports only EOF and errors, and it carries
 *   EV_CLEAR so each of them is reported once instead of on every wait. A
 *   report from it without EOF is a pending socket error, and
 *   ccol_poll_refine() turns that into CCOL_POLL_ERR. (macOS cuts the
 *   low-water mark down to the receive buffer size, so a full buffer reports
 *   there too, and ccol_poll_refine() drops that report.)
 *   A pipe, a FIFO or a terminal ignores the low-water mark. If its mask asks
 *   for CCOL_POLL_OUT, the write filter alone watches it. Otherwise (errors
 *   and EOF only) it's watched by the filter of the end it was opened for,
 *   with EV_CLEAR and never one-shot: the read filter for a read end or an
 *   end opened both ways, and the write filter for a write end. The mask
 *   drops every data report of that filter, so only its EOF gets through, as
 *   CCOL_POLL_HUP or CCOL_POLL_ERR.
 * - CCOL_POLL_ONESHOT becomes EV_DISPATCH on every filter except the
 *   error-only read filter (EV_CLEAR already limits that one to one report
 *   per change), so a filter that reports is disabled until the next ADD or
 *   MOD. A MOD that adds or removes CCOL_POLL_IN on an existing read filter
 *   must carry CCOL_POLL_READ_MODE_CHANGE, because kqueue keeps the trigger
 *   flags a filter got when it was created. The two filters of an fd are
 *   separate, so the second one can still report while a dispatch for the
 *   first is in flight. The event loop drops an event for an entry whose job
 *   is in flight and re-arms both filters from live state when the job ends,
 *   which is the same rule that covers a late epoll event.
 * - Each kevent becomes one event. A read filter report gives CCOL_POLL_IN.
 *   With EV_EOF it gives what epoll gives at EOF: for a socket CCOL_POLL_IN
 *   and CCOL_POLL_RDHUP, plus CCOL_POLL_ERR when the socket reports an error
 *   and CCOL_POLL_HUP when poll(2) reports POLLHUP (both directions down);
 *   for a pipe or FIFO CCOL_POLL_HUP, with CCOL_POLL_IN only while data is
 *   still queued. A write filter report gives CCOL_POLL_OUT, and on EV_EOF
 *   CCOL_POLL_ERR or CCOL_POLL_HUP. If both filters of one fd report in the
 *   same batch, they arrive as two events.
 * - The EOF bits of the read filter depend on what the fd is, and only an
 *   fstat(2) and a poll(2) of the fd can tell us that. Even so
 *   ccol_poll_wait() makes no system call on an fd, because another thread
 *   can close it as soon as the wait returns and the number may then name a
 *   different object. It marks the event instead, and the caller calls
 *   ccol_poll_refine() with the fd at a point where it can prove the fd is
 *   not closed. The event loop does that under the stripe lock for an entry
 *   that isn't removed: removal takes the same lock and an fd is closed only
 *   after its removal. Until it's refined, a read filter EOF carries only
 *   internal marks, while every other report already carries CCOL_POLL_IN
 *   (read filter) or CCOL_POLL_OUT (write filter, with CCOL_POLL_ERR or
 *   CCOL_POLL_HUP at its EOF). Only ccol_poll_refine() gives the bits epoll
 *   would report for the fd and the mask, so a caller that doesn't refine an
 *   event must not read its bits. ccol_poll_refine() also takes the
 *   registration's interest mask and keeps only the bits epoll would report
 *   for it (the mask, CCOL_POLL_ERR and CCOL_POLL_HUP), which means an event
 *   can refine to no bit at all, and the caller then ignores it. On Linux
 *   ccol_poll_refine() does nothing.
 * - An ADD of a regular file or a directory fails with EPERM like epoll_ctl
 *   does, where kqueue would accept it and report it ready for ever.
 * - CCOL_POLL_CTL_DEL deletes both filters and succeeds if either existed.
 * - ccol_poll_ctl() and ccol_poll_refine() take a ccol_poll_fdinfo, which
 *   may be NULL. A caller that keeps an fd registered for a long time keeps
 *   one for it, zeroed before the first ADD and kept under the same lock as
 *   every call for that fd. The ADD records what the fd is, so a later MOD or
 *   refinement asks the kernel nothing about it, and ccol_poll_ctl() records
 *   the trigger flags of the write filter it created, so a MOD that changes
 *   them creates the filter again. With NULL every call asks the fd itself.
 *   Linux ignores the argument.
 *
 * The event array that ccol_poll_wait() fills is also the buffer kevent()
 * writes into, so there ccol_poll_event is as large as struct kevent and the
 * wait allocates nothing.
 *
 * When the event loop needs something new from the kernel, add it here in
 * epoll terms and emulate it on kqueue, rather than teaching the loop about
 * kqueue. Keeping one set of semantics in the loop is what lets the Linux
 * build stay plain epoll.
 */

#ifndef CCOL_INTERNAL_CPOLL_H
#define CCOL_INTERNAL_CPOLL_H

#include <errno.h>
#include <stdint.h>

#if defined(__linux__)

#include <sys/epoll.h>

typedef struct epoll_event ccol_poll_event;

/* epoll doesn't need to know anything about an fd; see the kqueue
 * definition below. */
typedef uint8_t ccol_poll_fdinfo;

#define CCOL_POLL_IN EPOLLIN
#define CCOL_POLL_OUT EPOLLOUT
#define CCOL_POLL_RDHUP EPOLLRDHUP
#define CCOL_POLL_ERR EPOLLERR
#define CCOL_POLL_HUP EPOLLHUP
#define CCOL_POLL_ONESHOT EPOLLONESHOT
/* See the kqueue definition below; with epoll there's nothing to rebuild. */
#define CCOL_POLL_READ_MODE_CHANGE 0u

#define CCOL_POLL_CTL_ADD EPOLL_CTL_ADD
#define CCOL_POLL_CTL_MOD EPOLL_CTL_MOD
#define CCOL_POLL_CTL_DEL EPOLL_CTL_DEL

#define ccol_poll_create() epoll_create1(EPOLL_CLOEXEC)
#define ccol_poll_ctl(pfd, op, fd, ev, info) \
  ((void)(info), epoll_ctl((pfd), (op), (fd), (ev)))
#define ccol_poll_wait(pfd, evs, max, timeout_ms) \
  epoll_wait((pfd), (evs), (max), (timeout_ms))

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

/* An event's 64-bit tag rides in the pointer-sized udata of its kevent. */
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
/* Flag for a MOD whose CCOL_POLL_IN differs from the mask of the fd's last
 * ADD or MOD. kqueue keeps the trigger flags (EV_CLEAR, EV_DISPATCH) a filter
 * got at creation, so when a read filter switches between data mode and
 * error-only mode we delete it and create it again. */
#define CCOL_POLL_READ_MODE_CHANGE (1u << 29)

/* Marks that ccol_poll_wait() puts on a read filter EOF for
 * ccol_poll_refine() to read: the EOF itself, an error the filter reported,
 * and data that is still queued. */
#define _CCOL_POLL_EOF (1u << 28)
#define _CCOL_POLL_EOF_ERR (1u << 27)
#define _CCOL_POLL_EOF_DATA (1u << 26)
/* Marks a report from a socket's error-only read filter. Such a report
 * without EOF is a pending socket error, while any other read report is
 * data. */
#define _CCOL_POLL_ERR_ONLY_FILTER (1u << 25)

/* The kevent udata bit that marks the error-only read filter. Every tag a
 * caller registers has it clear, since an event loop entry is a user-space
 * address and ccol_select() registers bit 63 with a descriptor or a small
 * index. */
#define _CCOL_POLL_UDATA_ERR_ONLY ((uint64_t)1 << 62)

#define CCOL_POLL_CTL_ADD 1
#define CCOL_POLL_CTL_DEL 2
#define CCOL_POLL_CTL_MOD 3

/* No read ever reaches this low-water mark, so a read filter carrying it
 * reports only EOF and errors. */
#define _CCOL_POLL_LOWAT_NEVER INT_MAX

/* ThreadSanitizer intercepts epoll_ctl() and epoll_wait() and records that a
 * registration happens before every event a wait returns for it, but it
 * doesn't intercept kevent(). So in a ThreadSanitizer build we state the same
 * ordering here ourselves, keyed on the kqueue the way the Linux runtime keys
 * it on the epoll descriptor: a release before each change and an acquire
 * after each wait that returns events. Without it every object one thread
 * registers and the poller then reaches through an event's udata shows up as
 * a race, although the kernel orders the two. Only that build compiles any of
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

/* Whether the socket fd is listening. FreeBSD and OpenBSD answer
 * SO_ACCEPTCONN, while macOS refuses that option with ENOPROTOOPT and gives
 * the same flag through proc_pidfdinfo(). If the query fails we answer false,
 * so the error-only filter is armed as for any other socket. */
static inline bool _ccol_poll_is_listener(int fd) {
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

/* What ccol_poll_ctl() knows about an fd (see the rules at the top of this
 * file). Zero means nothing is known yet. */
typedef uint16_t ccol_poll_fdinfo;

#define _CCOL_POLL_FI_KNOWN 0x001u        /* the kind bits below are known */
#define _CCOL_POLL_FI_SOCK 0x002u         /* a socket */
#define _CCOL_POLL_FI_RD 0x004u           /* opened for reading */
#define _CCOL_POLL_FI_ANON_PIPE 0x010u    /* a FIFO with no device and inode */
#define _CCOL_POLL_FI_LISTEN_KNOWN 0x020u /* the next bit is known */
#define _CCOL_POLL_FI_LISTENER 0x040u     /* a listening socket */
#define _CCOL_POLL_FI_WFILT 0x080u        /* a write filter is in place */
#define _CCOL_POLL_FI_WCLEAR 0x100u       /* ... and it carries EV_CLEAR */

/* Asks the fd what it is. A regular file or a directory fails with EPERM,
 * since epoll_ctl() refuses those too. */
static inline int _ccol_poll_fdinfo_fill(int fd, ccol_poll_fdinfo *fi) {
  struct stat st;
  if (fstat(fd, &st) != 0) return -1;
  if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
    errno = EPERM;
    return -1;
  }
  ccol_poll_fdinfo f = _CCOL_POLL_FI_KNOWN;
  if (S_ISSOCK(st.st_mode)) {
    f |= _CCOL_POLL_FI_SOCK | _CCOL_POLL_FI_RD;
  } else {
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -1;
    if ((fl & O_ACCMODE) != O_WRONLY) f |= _CCOL_POLL_FI_RD;
    if (S_ISFIFO(st.st_mode) && st.st_dev == 0 && st.st_ino == 0)
      f |= _CCOL_POLL_FI_ANON_PIPE;
  }
  *fi = f;
  return 0;
}

/* Whether the socket of info is listening, answered from the cache when the
 * cache knows. */
static inline bool _ccol_poll_fdinfo_listener(int fd, ccol_poll_fdinfo *fi) {
  if (!(*fi & _CCOL_POLL_FI_LISTEN_KNOWN)) {
    *fi |= _CCOL_POLL_FI_LISTEN_KNOWN;
    if (_ccol_poll_is_listener(fd)) *fi |= _CCOL_POLL_FI_LISTENER;
  }
  return (*fi & _CCOL_POLL_FI_LISTENER) != 0;
}

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
      *info &= (ccol_poll_fdinfo) ~(_CCOL_POLL_FI_WFILT | _CCOL_POLL_FI_WCLEAR);
    if (any) return 0;
    errno = ENOENT;
    return -1;
  }

  /* An ADD always starts with no filter in place. What kind of fd it is
   * can't change while it is registered, so a kind we already know is used
   * again. */
  ccol_poll_fdinfo local = 0;
  ccol_poll_fdinfo *fi = info ? info : &local;
  if (op == CCOL_POLL_CTL_ADD)
    *fi &= (ccol_poll_fdinfo) ~(_CCOL_POLL_FI_WFILT | _CCOL_POLL_FI_WCLEAR);
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
      !(*fi & _CCOL_POLL_FI_KNOWN) && _ccol_poll_fdinfo_fill(fd, fi) != 0)
    return -1;
  bool is_sock = (*fi & _CCOL_POLL_FI_SOCK) != 0;
  if (error_only_read) {
    /* Errors and EOF only. On a socket that's a low-water mark, and on a
     * descriptor that ignores the mark it's the filter of the end the fd was
     * opened for. */
    if (is_sock && want_write && !(m & CCOL_POLL_RDHUP)) {
      /* The write filter reports a socket's errors and its hang-up. */
      want_read = false;
    } else if (is_sock && _ccol_poll_fdinfo_listener(fd, fi)) {
      /* A listening socket's read filter ignores the low-water mark and
       * reports every pending connection, but epoll reports nothing for this
       * mask on a listener. */
      want_read = false;
    } else if (is_sock) {
      rfflags = NOTE_LOWAT;
      rdata = _CCOL_POLL_LOWAT_NEVER;
      rclear = EV_CLEAR;
      rmark = _CCOL_POLL_UDATA_ERR_ONLY;
#if defined(__OpenBSD__)
    } else if (*fi & _CCOL_POLL_FI_ANON_PIPE) {
      /* When the reader of an anonymous pipe closes (no device and no inode;
       * a named FIFO is a socket on OpenBSD and does report the close),
       * OpenBSD doesn't wake a write filter that is already armed on it,
       * although a write filter armed after the close and a read filter on
       * the write end both report it. Both ends of an OpenBSD pipe are
       * opened for reading and writing and the read filter reports the EOF
       * of either end, so the read filter alone watches the pipe, with the
       * write filter next to it when there's a data writer. Data flows both
       * ways in an OpenBSD pipe and this mask drops a report of data written
       * into the other direction, so the filter reports once per change and
       * is never disabled, like the error-only filter of a socket. */
      rclear = EV_CLEAR;
#endif
    } else if (want_write) {
      /* The write filter reports that the reader hung up. */
      want_read = false;
    } else if (!(*fi & _CCOL_POLL_FI_RD)) {
      /* A write end, where the write filter reports that the reader closed.
       * This mask drops its data reports, so it reports once per change and
       * is never disabled. */
      want_read = false;
      want_write = true;
      wclear = EV_CLEAR;
    } else {
      /* A read end, or an end opened both ways, where the read filter
       * reports that the writer closed or that a terminal hung up. This mask
       * drops its data reports, so it reports once per change and is never
       * disabled. */
      rclear = EV_CLEAR;
    }
  }

  if (op == CCOL_POLL_CTL_MOD && (m & CCOL_POLL_READ_MODE_CHANGE)) {
    /* If the filter isn't there this fails with ENOENT, which is the state
     * the delete wants anyway. */
    EV_SET(&ch[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    _CCOL_POLL_TSAN_RELEASE(kq);
    (void)kevent(kq, &ch[0], 1, NULL, 0, NULL);
  }
  if (want_write && (*fi & _CCOL_POLL_FI_WFILT) &&
      ((*fi & _CCOL_POLL_FI_WCLEAR) != 0) != (wclear != 0)) {
    /* kqueue keeps the trigger flags a filter got at creation, so a write
     * filter that switches between data mode and error-only mode has to be
     * deleted and created again. */
    EV_SET(&ch[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    _CCOL_POLL_TSAN_RELEASE(kq);
    (void)kevent(kq, &ch[1], 1, NULL, 0, NULL);
  }
  unsigned short receipt = EV_RECEIPT;
  if (want_read) {
    /* A filter with EV_CLEAR reports once per change of the fd and a report
     * never disables it. An event that refines to no bit gets dropped, so if
     * such an event had disabled the filter it would stay disabled and every
     * later error and EOF would be lost. */
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
  /* With EV_RECEIPT kevent() applies both changes in one call and reports
   * the result of each as an EV_ERROR record whose data is 0 or an errno.
   * Deleting a filter that isn't there reports ENOENT, which we don't treat
   * as an error here. */
  struct kevent res[2];
  struct timespec zero = {0, 0};
  _CCOL_POLL_TSAN_RELEASE(kq);
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
  *fi &= (ccol_poll_fdinfo) ~(_CCOL_POLL_FI_WFILT | _CCOL_POLL_FI_WCLEAR);
  if (write_added)
    *fi |= (ccol_poll_fdinfo)(_CCOL_POLL_FI_WFILT |
                              (wclear ? _CCOL_POLL_FI_WCLEAR : 0));
  if (fail != 0) {
    errno = fail;
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
  /* kevent() writes its records straight into evs, since each element has
   * the size and layout of a struct kevent. We copy each record out and then
   * translate it in place. */
  _Static_assert(sizeof(ccol_poll_event) == sizeof(struct kevent),
                 "ccol_poll_event must have the stride of struct kevent");
  int n = kevent(kq, NULL, 0, (struct kevent *)evs, max, tsp);
  if (n > 0) _CCOL_POLL_TSAN_ACQUIRE(kq);
  for (int i = 0; i < n; i++) {
    struct kevent k = evs[i]._kev;
    uint32_t bits = 0;
    if (k.filter == EVFILT_READ) {
      /* A report without EOF is readable data, except on a filter with the
       * unreachable low-water mark, where it's a pending socket error that
       * ccol_poll_refine() turns into CCOL_POLL_ERR. */
      if ((uint64_t)(uintptr_t)k.udata & _CCOL_POLL_UDATA_ERR_ONLY)
        bits |= _CCOL_POLL_ERR_ONLY_FILTER;
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
    evs[i].data.u64 = (uint64_t)(uintptr_t)k.udata & ~_CCOL_POLL_UDATA_ERR_ONLY;
  }
  return n;
}

/* Turns the marks on a read filter EOF into the bits epoll would give for
 * the fd, and keeps only the bits epoll reports for the interest mask (the
 * mask itself, CCOL_POLL_ERR and CCOL_POLL_HUP). The caller must prove
 * that fd is still open and still belongs to the registration; see the rules
 * at the top of this file. */
static inline void ccol_poll_refine(ccol_poll_event *ev, int fd, uint32_t mask,
                                    ccol_poll_fdinfo *info) {
  uint32_t m = ev->events;
  uint32_t keep = mask | CCOL_POLL_ERR | CCOL_POLL_HUP;
  bool err_only_filter = (m & _CCOL_POLL_ERR_ONLY_FILTER) != 0;
  m &= ~_CCOL_POLL_ERR_ONLY_FILTER;
  if (!(m & _CCOL_POLL_EOF)) {
#if defined(__APPLE__)
    /* macOS cuts the low-water mark down to the receive buffer size, so a
     * full buffer reports too, and a socket that started listening after it
     * was registered reports each pending connection. One proc_pidfdinfo()
     * call tells the three cases apart without clearing the pending error
     * (SO_ERROR would clear it): a listener's report goes, a pending error
     * stays, and anything else is a full buffer, which goes as well. If the
     * call fails, a stream socket at its buffer size or a datagram socket with
     * a datagram queued counts as a full buffer. */
    if (err_only_filter && (m & CCOL_POLL_IN)) {
      struct socket_fdinfo si;
      if (info && (*info & _CCOL_POLL_FI_LISTENER)) {
        m &= ~CCOL_POLL_IN;
      } else if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &si,
                                sizeof(si)) == (int)sizeof(si)) {
        if (si.psi.soi_options & SO_ACCEPTCONN) {
          if (info)
            *info |= _CCOL_POLL_FI_LISTEN_KNOWN | _CCOL_POLL_FI_LISTENER;
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
    /* A socket that started listening after it was registered reports each
     * pending connection on this filter (the filter ignores the low-water
     * mark for a listener), and that isn't an error. We store the answer in
     * info so the next MOD arms no read filter for the listener. */
    if (err_only_filter && (m & CCOL_POLL_IN)) {
      bool listener;
      if (info && (*info & _CCOL_POLL_FI_LISTENER)) {
        listener = true;
      } else {
        listener = _ccol_poll_is_listener(fd);
        if (listener && info)
          *info |= _CCOL_POLL_FI_LISTEN_KNOWN | _CCOL_POLL_FI_LISTENER;
      }
      if (listener) m &= ~CCOL_POLL_IN;
    }
#endif
    /* Whatever is left of an error-only filter report is a pending socket
     * error, which epoll reports as EPOLLERR. Any other read report is data
     * that the mask keeps or drops, so a stale report from a time when the
     * mask asked for data never turns into an error. */
    if (err_only_filter && (m & CCOL_POLL_IN))
      m = (m & ~CCOL_POLL_IN) | CCOL_POLL_ERR;
    ev->events = m & keep;
    return;
  }
  uint32_t bits =
      m & ~(_CCOL_POLL_EOF | _CCOL_POLL_EOF_ERR | _CCOL_POLL_EOF_DATA);
  /* EV_EOF only says no more data is coming, but epoll says more than that
   * and the event loop routes on it. A socket at EOF is readable with
   * EPOLLRDHUP, and EPOLLHUP joins them when both directions are down (a
   * closed Unix socket, not a TCP close or half-close), which poll(2)
   * reports as POLLHUP. A pipe or FIFO whose writer is gone gives EPOLLHUP
   * alone, with EPOLLIN only while data is still queued. */
  bool is_sock;
  if (info && (*info & _CCOL_POLL_FI_KNOWN)) {
    is_sock = (*info & _CCOL_POLL_FI_SOCK) != 0;
  } else {
    struct stat est;
    is_sock = fstat(fd, &est) == 0 && S_ISSOCK(est.st_mode);
  }
  if (is_sock) {
    bits |= CCOL_POLL_IN | CCOL_POLL_RDHUP;
    if (m & _CCOL_POLL_EOF_ERR) bits |= CCOL_POLL_ERR;
    /* We ask for POLLOUT instead of an empty mask because macOS poll(2)
     * reports only on the filters the events ask for, and its POLLHUP is the
     * EOF of those filters. For POLLOUT that means "nothing can be sent any
     * more", and together with the read filter's EOF that's both directions
     * down. Linux and FreeBSD report POLLHUP whatever the events ask for. */
    struct pollfd pfd = {.fd = fd, .events = POLLOUT};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLHUP)) bits |= CCOL_POLL_HUP;
  } else {
    bits |= CCOL_POLL_HUP;
    if (m & _CCOL_POLL_EOF_DATA) bits |= CCOL_POLL_IN;
  }
  ev->events = bits & keep;
}

#else
#error "c_collections needs epoll(7) or kqueue(2)"
#endif

/* A wake descriptor is one descriptor that a poller watches for
 * readability; ccol_wakefd_notify() makes it readable and ccol_wakefd_drain()
 * makes it quiet again. Where the system has eventfd(2) (Linux and FreeBSD)
 * it's an eventfd. macOS and OpenBSD have none, so there it's a kqueue of its
 * own holding one EVFILT_USER event: a trigger makes the kqueue readable, and
 * collecting the event (it carries EV_CLEAR) makes it quiet.
 * _CCOL_EMULATE_DARWIN_WAKEFD selects that form on FreeBSD too, which is how
 * the FreeBSD test suites run it. ccol_wakefd_create() gives the descriptor,
 * closed on exec and non-blocking, or -1 with errno set, and close(2)
 * releases it. */
#if (defined(__APPLE__) || defined(__OpenBSD__)) && \
    !defined(_CCOL_EMULATE_DARWIN_WAKEFD)
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

/* EAGAIN means the counter is already at its ceiling, which the small,
 * bounded counts of this library never get to, and EBADF can only be a bug
 * in the caller. Every call site is best effort, so we retry only EINTR. */
static inline void ccol_wakefd_notify(int fd) {
  uint64_t one = 1;
  while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
  }
}

/* EAGAIN just says nothing is pending, which is what we expect. */
static inline void ccol_wakefd_drain(int fd) {
  uint64_t val;
  while (read(fd, &val, sizeof(val)) < 0 && errno == EINTR) {
  }
}
#endif

#endif /* CCOL_INTERNAL_CPOLL_H */
