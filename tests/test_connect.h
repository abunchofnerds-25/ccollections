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

/* connect(2) for the tests, with a bound on the wait.
 *
 * A blocking connect(2) of a TCP socket on OpenBSD 7.9 can miss its wakeup:
 * the connection is established, and even closed again by the peer, while
 * the caller sleeps in connect(2) for ever, because the connect timer stops
 * once the connection exists. A program of a few lines that makes blocking
 * loopback connects from four threads reproduces it within a few million
 * connects; the same program with a non-blocking connect(2) and poll(2) makes
 * twelve million without a hang. A test that sleeps there holds its CI job
 * until the job's time limit.
 *
 * test_connect() therefore connects a blocking TCP socket in non-blocking
 * mode, waits for the result with poll(2) for at most
 * TEST_CONNECT_TIMEOUT_MS, and gives the socket back in blocking mode. It
 * returns 0 on success, and -1 with errno set on failure, ETIMEDOUT when the
 * wait ran out, exactly as connect(2) reports a failure. Any other socket (a
 * Unix socket, a datagram socket, or one that is already non-blocking) goes
 * straight to connect(2), so a test keeps the behavior that it asks for. */

#ifndef CCOL_TESTS_TEST_CONNECT_H
#define CCOL_TESTS_TEST_CONNECT_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <sys/socket.h>

#ifndef TEST_CONNECT_TIMEOUT_MS
#define TEST_CONNECT_TIMEOUT_MS 30000
#endif

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

#endif /* CCOL_TESTS_TEST_CONNECT_H */
