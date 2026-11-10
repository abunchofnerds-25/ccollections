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

// accept(2) for the mock servers of the tests. On OpenBSD a blocking accept(2)
// can miss its wakeup and leave a connection in the listen queue until the next
// one arrives, so these helpers never sleep in accept(2).
#ifndef CCOL_TESTS_TEST_ACCEPT_H
#define CCOL_TESTS_TEST_ACCEPT_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>

// How long one poll(2) waits, so a lost poll wakeup costs at most one slice.
#ifndef TEST_ACCEPT_SLICE_MS
#define TEST_ACCEPT_SLICE_MS 100
#endif

// Accepts one connection on the listener lfd. It replaces a blocking accept(2)
// in every mock server, which can miss its wakeup on OpenBSD. It makes the
// listener non-blocking (for good), waits with poll(2) and accepts. The new
// socket is blocking (the BSDs and macOS inherit O_NONBLOCK from the listener,
// so we clear it). It gives the descriptor, or -1 with errno from accept(2) or
// poll(2) (EINTR for a signal), or EBADF once the listener is closed. It waits
// as long as no connection comes; stop it with a connection
// (test_wake_listener), shutdown(2) or a signal. Every accept on the listener
// must go through here, because the listener stays non-blocking.
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

#endif
