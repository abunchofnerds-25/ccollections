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

// connect(2) for the tests, with a bound on the wait. On OpenBSD a blocking TCP
// connect(2) can miss its wakeup and sleep for ever on a connection that
// already exists, so a TCP connect here runs in non-blocking mode under
// poll(2).
#ifndef CCOL_TESTS_TEST_CONNECT_H
#define CCOL_TESTS_TEST_CONNECT_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <sys/socket.h>

// The longest test_connect() waits for a TCP connect.
#ifndef TEST_CONNECT_TIMEOUT_MS
#define TEST_CONNECT_TIMEOUT_MS 30000
#endif

// Connects fd to addr, like connect(2) but bounded. Tests use it in place of
// connect(2) because a blocking TCP connect can hang for ever on OpenBSD. It
// connects a blocking TCP socket in non-blocking mode, waits at most
// TEST_CONNECT_TIMEOUT_MS and hands the socket back blocking. It gives 0, or -1
// with errno set (ETIMEDOUT when the wait ran out). Any other socket (Unix,
// datagram, or already non-blocking) goes straight to connect(2).
static inline int test_connect(int fd, const struct sockaddr *addr,
                               socklen_t len) {
  int type = 0;
  socklen_t tl = sizeof(type);
  int flags = fcntl(fd, F_GETFL);
  bool tcp =
      addr && (addr->sa_family == AF_INET || addr->sa_family == AF_INET6);
  if (!tcp || flags < 0 || (flags & O_NONBLOCK) ||
      getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 ||
      type != SOCK_STREAM)
    return connect(fd, addr, len);
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    return connect(fd, addr, len);
  int err = 0;
  if (connect(fd, addr, len) != 0) err = errno;
  if (err == EINPROGRESS || err == EINTR) {
    struct pollfd p = {.fd = fd, .events = POLLOUT, .revents = 0};
    int pr;
    do {
      pr = poll(&p, 1, TEST_CONNECT_TIMEOUT_MS);
    } while (pr < 0 && errno == EINTR);
    if (pr == 0) {
      err = ETIMEDOUT;
    } else if (pr < 0) {
      err = errno;
    } else {
      socklen_t el = sizeof(err);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) err = errno;
    }
  }
  (void)fcntl(fd, F_SETFL, flags);
  if (err != 0) {
    errno = err;
    return -1;
  }
  return 0;
}

#endif
