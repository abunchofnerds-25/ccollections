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

#include <fcntl.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <sys/socket.h>
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

#endif /* CCOL_INTERNAL_CSOCK_H */
