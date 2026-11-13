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

// Every socket the library creates is non-blocking, closed on exec and never
// raises SIGPIPE. Linux and the BSDs take SOCK_NONBLOCK and SOCK_CLOEXEC at
// creation, have accept4() and MSG_NOSIGNAL. macOS has none of them, so there
// we set the flags with fcntl() right after the socket exists and put
// SO_NOSIGPIPE on it, which makes CCOL_MSG_NOSIGNAL 0.
// CCOL__EMULATE_DARWIN_SOCK picks the macOS form on other systems too, which is
// how the Linux and FreeBSD suites test it (Linux has no SO_NOSIGPIPE, so it
// keeps MSG_NOSIGNAL). When the flags need a second call, both calls run inside
// the close-on-exec gate of common.h, so a fork() from another thread can't
// hand a child the socket before it's closed on exec. Every new socket of the
// library should come from here: A socket() call with the Linux flags doesn't
// build on macOS, and one without them loses these properties everywhere.

#ifndef CCOL_INTERNAL_CSOCK_H
#define CCOL_INTERNAL_CSOCK_H

#include <common.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
// C23 has bool, true and false as keywords.
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// macOS lacks the Linux socket flags, so it always takes the emulated path.
#if defined(__APPLE__) && !defined(CCOL__EMULATE_DARWIN_SOCK)
#define CCOL__EMULATE_DARWIN_SOCK 1
#endif

// CCOL_MSG_NOSIGNAL is the flag every send of the library passes so a gone
// peer raises no SIGPIPE. It's 0 where SO_NOSIGPIPE on the socket does the job,
// and CCOL__SOCK_NOSIGPIPE says so.
#if defined(CCOL__EMULATE_DARWIN_SOCK) && defined(SO_NOSIGPIPE)
#define CCOL_MSG_NOSIGNAL 0
#define CCOL__SOCK_NOSIGPIPE 1
#else
#define CCOL_MSG_NOSIGNAL MSG_NOSIGNAL
#endif

#if defined(CCOL__EMULATE_DARWIN_SOCK)
// Makes a new socket closed on exec, free of SIGPIPE and, when nonblock is
// true, non-blocking, with fcntl() and setsockopt(); the emulated helpers below
// call it on each socket they create. If any step fails it closes fd and gives
// -1, else it gives fd.
static inline int ccol__sock_setup(int fd, bool nonblock) {
  if (fd < 0) return -1;
  int fl = fcntl(fd, F_GETFD);
  int st = nonblock ? fcntl(fd, F_GETFL) : 0;
  bool ok = fl >= 0 && fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0 && st >= 0 &&
            (!nonblock || fcntl(fd, F_SETFL, st | O_NONBLOCK) == 0);
#if defined(CCOL__SOCK_NOSIGPIPE)
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

// Makes a socket we didn't create free of SIGPIPE where that's a property of
// the socket (SO_NOSIGPIPE). Gives 0, or -1 when the option can't be set. Where
// CCOL_MSG_NOSIGNAL does the job it does nothing.
static inline int ccol_sock_nosigpipe(int fd) {
#if defined(CCOL__SOCK_NOSIGPIPE)
  int one = 1;
  return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0 ? 0
                                                                          : -1;
#else
  (void)fd;
  return 0;
#endif
}

#if defined(CCOL__EMULATE_DARWIN_SOCK)
// Leaves the close-on-exec gate and gives fd back, keeping the errno that the
// steps inside the gate left; every emulated helper ends with it.
static inline int ccol__sock_gate_leave(int fd) {
  int saved = errno;
  ccol__cloexec_gate_leave();
  errno = saved;
  return fd;
}
#endif

// socket(2) for a non-blocking socket that is closed on exec; every socket the
// library creates comes from here. Gives the fd or -1 with errno set.
static inline int ccol_socket_nb(int domain, int type, int protocol) {
#if defined(CCOL__EMULATE_DARWIN_SOCK)
  ccol__cloexec_gate_enter();
  return ccol__sock_gate_leave(
      ccol__sock_setup(socket(domain, type, protocol), true));
#else
  return socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
#endif
}

#if defined(CCOL__EMULATE_DARWIN_SOCK)
// accept(2) for a non-blocking socket that is closed on exec, for the servers
// of the library. ccol_accept_nb is a macro that evaluates to the new fd or -1,
// so that a file which never accepts doesn't need a declaration of accept4(),
// which glibc gives only under _GNU_SOURCE.
static inline int ccol__accept_nb_gated(int fd, struct sockaddr *addr,
                                        socklen_t *addrlen) {
  ccol__cloexec_gate_enter();
  return ccol__sock_gate_leave(
      ccol__sock_setup(accept(fd, addr, addrlen), true));
}
#define ccol_accept_nb(fd, addr, addrlen) \
  ccol__accept_nb_gated((fd), (addr), (addrlen))
#else
#define ccol_accept_nb(fd, addr, addrlen) \
  accept4((fd), (addr), (addrlen), SOCK_NONBLOCK | SOCK_CLOEXEC)
#endif

// socketpair(2) for two non-blocking sockets that are closed on exec, filling
// sv. Gives 0, or -1 with errno set and no socket left open.
static inline int ccol_socketpair_nb(int domain, int type, int protocol,
                                     int sv[2]) {
#if defined(CCOL__EMULATE_DARWIN_SOCK)
  ccol__cloexec_gate_enter();
  if (socketpair(domain, type, protocol, sv) != 0)
    return ccol__sock_gate_leave(-1);
  if (ccol__sock_setup(sv[0], true) < 0) {
    close(sv[1]);
    return ccol__sock_gate_leave(-1);
  }
  if (ccol__sock_setup(sv[1], true) < 0) {
    close(sv[0]);
    return ccol__sock_gate_leave(-1);
  }
  return ccol__sock_gate_leave(0);
#else
  return socketpair(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol, sv);
#endif
}

#if defined(CCOL__EMULATE_DARWIN_SOCK)
// The flag for a send that must not block. Linux and FreeBSD honour
// MSG_DONTWAIT on send(2); macOS honours it only on recv(2), so its send to a
// blocking socket with too little room waits for the rest. There the flag is 0
// and ccol_send_room() limits the send instead.
#define CCOL_MSG_DONTWAIT 0
#else
#define CCOL_MSG_DONTWAIT MSG_DONTWAIT
#endif

#if defined(CCOL__EMULATE_DARWIN_SOCK)
// How many of len bytes one send(2) can hand to a blocking fd without waiting.
// After POLLOUT a stream socket has room for at least SO_SNDLOWAT bytes, so we
// give that many, or 0 when there's no room (the caller reports EAGAIN). Any
// other socket type gets len: A macOS datagram socket never queues in its send
// buffer, so a datagram goes out whole or fails with ENOBUFS. An error or a
// hang-up also gives len, so the send itself reports it. Linux has no real
// SO_SNDLOWAT and its POLLOUT means a quarter of the buffer is free, so the
// emulation uses 2048 there, the macOS default.
static inline size_t ccol__send_room_blocking(int fd, size_t len) {
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
  if (pr < 0) return len;
  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return len;
  if (!(pfd.revents & POLLOUT)) return 0;
#if defined(__linux__)
  int lowat = 2048;
#else
  int lowat = 0;
  socklen_t ll = sizeof(lowat);
  if (getsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &lowat, &ll) != 0 || lowat < 1)
    lowat = 1;
#endif
  return len < (size_t)lowat ? len : (size_t)lowat;
}

// Gives how many of the len bytes a send with CCOL_MSG_DONTWAIT can hand to fd
// without waiting, for ccol_send_nb() and the callers that use sendmsg(): len
// where the flag does the job or fd is non-blocking, otherwise what
// ccol__send_room_blocking() finds.
static inline size_t ccol_send_room(int fd, size_t len) {
  int st = fcntl(fd, F_GETFL);
  if (st < 0 || (st & O_NONBLOCK)) return len;
  return ccol__send_room_blocking(fd, len);
}
#else
// Without the emulation MSG_DONTWAIT does the job, so a send may hand over all
// len bytes.
static inline size_t ccol_send_room(int fd, size_t len) {
  (void)fd;
  return len;
}
#endif

// send(2) of at most len bytes that never blocks, for the HTTP stream writes
// and the logger's bounded writes. With no room right now it gives -1 and errno
// EAGAIN, otherwise what send(2) gives. A short count can happen on every
// system, as with any send on a stream socket.
static inline ssize_t ccol_send_nb(int fd, const void *buf, size_t len,
                                   int flags) {
  size_t room = ccol_send_room(fd, len);
  if (room == 0 && len > 0) {
    errno = EAGAIN;
    return -1;
  }
  return send(fd, buf, room, flags | CCOL_MSG_DONTWAIT);
}

#endif
