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

/* accept(2) for the mock servers of the tests, without a sleep in accept(2).
 *
 * On OpenBSD 7.9 a blocking accept(2) can miss its wakeup, as a blocking
 * connect(2) can (see test_connect.h): both sleep on the so_timeo channel of
 * the socket. A connection then sits in the listen queue while the server
 * thread sleeps in accept(2), until the next connection to the listener wakes
 * it. A program of a few lines with one blocking acceptor and four loopback
 * clients hits it about once in ten million connects. In a test the client
 * waits for an answer that nobody writes, and the test fails on its timeout
 * or holds the CI job until the job's time limit.
 *
 * So test_accept() never sleeps in accept(2). It makes the listener
 * non-blocking, waits for a connection with poll(2), in slices of
 * TEST_ACCEPT_SLICE_MS so that even a lost poll(2) wakeup costs one slice,
 * and then accepts. The connection it hands out is blocking, as one from a
 * blocking listener is: the BSDs and macOS let an accepted socket inherit
 * O_NONBLOCK from the listener, so the helper clears it there.
 *
 * It returns the new descriptor, or -1 with errno set: the error of
 * accept(2), or of poll(2) (EINTR for a signal, so a loop sees a signal the
 * same way it sees one in accept(2)), or EBADF once the listener is closed.
 * It waits as long as no connection comes, like accept(2); a mock server
 * stops it with a connection to the listener (test_wake_listener), with
 * shutdown(2), or with a signal. Every accept on the listener must go through
 * this helper, because the listener stays non-blocking. */

#ifndef CCOL_TESTS_TEST_ACCEPT_H
#define CCOL_TESTS_TEST_ACCEPT_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>

#ifndef TEST_ACCEPT_SLICE_MS
#define TEST_ACCEPT_SLICE_MS 100
#endif

static inline int test_accept(int lfd) {
  int flags = fcntl(lfd, F_GETFL);
  if (flags < 0) return -1;
  if (!(flags & O_NONBLOCK) && fcntl(lfd, F_SETFL, flags | O_NONBLOCK) != 0)
    return -1;
  for (;;) {
    int fd = accept(lfd, NULL, NULL);
    if (fd >= 0) {
      int fl = fcntl(fd, F_GETFL);
      if (fl >= 0 && (fl & O_NONBLOCK))
        (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
      return fd;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    struct pollfd p = {.fd = lfd, .events = POLLIN, .revents = 0};
    int pr = poll(&p, 1, TEST_ACCEPT_SLICE_MS);
    if (pr < 0) return -1;
    if (pr > 0 && (p.revents & POLLNVAL)) {
      errno = EBADF;
      return -1;
    }
  }
}

#endif /* CCOL_TESTS_TEST_ACCEPT_H */
