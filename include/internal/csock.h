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

/* Sockets that the library creates: non-blocking, closed on exec, and never
 * a source of SIGPIPE.
 *
 * Linux and the BSDs take SOCK_NONBLOCK and SOCK_CLOEXEC at creation, accept4()
 * for an accepted socket, and MSG_NOSIGNAL on each send. macOS has none of
 * these: there the flags are set with fcntl() right after the socket exists,
 * and the socket carries SO_NOSIGPIPE, so CCOL_MSG_NOSIGNAL is 0.
 * _CCOL_EMULATE_DARWIN_SOCK selects the macOS form elsewhere, which is how the
 * test suites of Linux and FreeBSD run it (Linux has no SO_NOSIGPIPE, so it
 * keeps MSG_NOSIGNAL). */

#ifndef CCOL_INTERNAL_CSOCK_H
#define CCOL_INTERNAL_CSOCK_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__) && !defined(_CCOL_EMULATE_DARWIN_SOCK)
#define _CCOL_EMULATE_DARWIN_SOCK 1
#endif

#if defined(_CCOL_EMULATE_DARWIN_SOCK) && defined(SO_NOSIGPIPE)
#define CCOL_MSG_NOSIGNAL 0
#define _CCOL_SOCK_NOSIGPIPE 1
#else
#define CCOL_MSG_NOSIGNAL MSG_NOSIGNAL
#endif

#if defined(_CCOL_EMULATE_DARWIN_SOCK)
/* Makes a new socket non-blocking (when nonblock is true), closed on exec and
 * free of SIGPIPE. It closes fd and gives -1 when a step fails. */
static inline int _ccol_sock_setup(int fd, bool nonblock) {
  if (fd < 0) return -1;
  int fl = fcntl(fd, F_GETFD);
  int st = nonblock ? fcntl(fd, F_GETFL) : 0;
  bool ok = fl >= 0 && fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0 && st >= 0 &&
            (!nonblock || fcntl(fd, F_SETFL, st | O_NONBLOCK) == 0);
#if defined(_CCOL_SOCK_NOSIGPIPE)
  int one = 1;
  ok = ok && setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0;
#endif
  if (!ok) {
    close(fd);
    return -1;
  }
  return fd;
}
#endif

/* Makes a socket that the library did not create free of SIGPIPE, where that
 * is a property of the socket (SO_NOSIGPIPE) and not of each send. Gives 0, or
 * -1 when the option cannot be set. A no-op where CCOL_MSG_NOSIGNAL does the
 * work. */
static inline int ccol_sock_nosigpipe(int fd) {
#if defined(_CCOL_SOCK_NOSIGPIPE)
  int one = 1;
  return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0 ? 0
                                                                          : -1;
#else
  (void)fd;
  return 0;
#endif
}

/* socket(2) for a non-blocking socket that is closed on exec. */
static inline int ccol_socket_nb(int domain, int type, int protocol) {
#if defined(_CCOL_EMULATE_DARWIN_SOCK)
  return _ccol_sock_setup(socket(domain, type, protocol), true);
#else
  return socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#endif
}

/* accept(2) for a non-blocking socket that is closed on exec. A macro, so a
 * file that never accepts needs no declaration of accept4(), which glibc
 * gives only under _GNU_SOURCE. */
#if defined(_CCOL_EMULATE_DARWIN_SOCK)
#define ccol_accept_nb(fd, addr, addrlen) \
  _ccol_sock_setup(accept((fd), (addr), (addrlen)), true)
#else
#define ccol_accept_nb(fd, addr, addrlen) \
  accept4((fd), (addr), (addrlen), SOCK_NONBLOCK | SOCK_CLOEXEC)
#endif

/* socketpair(2) for two non-blocking sockets that are closed on exec. */
static inline int ccol_socketpair_nb(int domain, int type, int protocol,
                                     int sv[2]) {
#if defined(_CCOL_EMULATE_DARWIN_SOCK)
  if (socketpair(domain, type, protocol, sv) != 0) return -1;
  if (_ccol_sock_setup(sv[0], true) < 0) {
    close(sv[1]);
    return -1;
  }
  if (_ccol_sock_setup(sv[1], true) < 0) {
    close(sv[0]);
    return -1;
  }
  return 0;
#else
  return socketpair(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol, sv);
#endif
}

/* A send that never blocks, whatever the mode of the descriptor.
 *
 * Linux and FreeBSD honour MSG_DONTWAIT on send(2). macOS honours it only on
 * recv(2): its send path tests the non-blocking mode of the socket and
 * nothing else, so a send to a blocking socket with too little room waits for
 * the rest of the bytes. There, ccol_send_room() limits the send instead.
 * Where poll(2) reports POLLOUT on a stream socket, the socket has room for at
 * least SO_SNDLOWAT bytes, so a send of that many bytes does not wait. A
 * datagram socket of macOS never queues data in its send buffer, so a whole
 * datagram goes out at once (or fails with ENOBUFS) and is never cut. */
#if defined(_CCOL_EMULATE_DARWIN_SOCK)
#define CCOL_MSG_DONTWAIT 0
#else
#define CCOL_MSG_DONTWAIT MSG_DONTWAIT
#endif

/* How many of the len bytes one send(2) or sendmsg(2) with CCOL_MSG_DONTWAIT
 * can give fd without a wait. It gives len where the flag does the work, or
 * where fd is non-blocking, and 0 when a blocking stream socket has no room
 * now. The caller then reports EAGAIN, as the send itself would. */
#if defined(_CCOL_EMULATE_DARWIN_SOCK)
static inline size_t _ccol_send_room_blocking(int fd, size_t len) {
  int type = 0;
  socklen_t tl = sizeof(type);
  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 ||
      type != SOCK_STREAM)
    return len;
  struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
  int pr;
  do {
    pr = poll(&pfd, 1, 0);
  } while (pr < 0 && errno == EINTR);
  if (pr < 0) return len; /* the send reports the error of fd */
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    return len; /* the send fails at once */
  if (!(pfd.revents & POLLOUT)) return 0;
#if defined(__linux__)
  /* Linux implements no SO_SNDLOWAT (it always reads 1), and its POLLOUT on a
   * stream socket means that a quarter or more of the send buffer is free.
   * The emulation there uses 2048 bytes, the default of macOS. */
  int lowat = 2048;
#else
  int lowat = 0;
  socklen_t ll = sizeof(lowat);
  if (getsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &lowat, &ll) != 0 || lowat < 1)
    lowat = 1;
#endif
  return len < (size_t)lowat ? len : (size_t)lowat;
}
static inline size_t ccol_send_room(int fd, size_t len) {
  int st = fcntl(fd, F_GETFL);
  if (st < 0 || (st & O_NONBLOCK)) return len;
  return _ccol_send_room_blocking(fd, len);
}
#else
static inline size_t ccol_send_room(int fd, size_t len) {
  (void)fd;
  return len;
}
#endif

/* send(2) of at most len bytes that never blocks. It gives -1 with errno set
 * to EAGAIN when the socket has no room now, and otherwise what send(2)
 * gives. A short count is possible on every system, as for any send(2) on a
 * stream socket. */
static inline ssize_t ccol_send_nb(int fd, const void *buf, size_t len,
                                   int flags) {
  size_t room = ccol_send_room(fd, len);
  if (room == 0 && len > 0) {
    errno = EAGAIN;
    return -1;
  }
  return send(fd, buf, room, flags | CCOL_MSG_DONTWAIT);
}

#endif /* CCOL_INTERNAL_CSOCK_H */
