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

/* A wait for a forked test child that can't outlast the test.
 *
 * A plain waitpid() waits as long as the child lives, so a child stuck on a
 * lock or a lost wakeup holds the whole test binary until the CI job's time
 * limit, and the log never names the test. test_wait_child_bounded() polls
 * the child against CLOCK_MONOTONIC, and once the bound is over it kills the
 * child with SIGKILL, reaps it and says so on stderr. */

#ifndef CCOL_TESTS_TEST_WAIT_H
#define CCOL_TESTS_TEST_WAIT_H

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#define _diffTimeNsec(T1, T2)                          \
  ((long long)(T2.tv_sec - T1.tv_sec) * 1000000000LL + \
   (T2.tv_nsec - T1.tv_nsec))

/* Waits at most bound_ms for the child pid to end. Returns true when it ended
 * on its own, with its wait status in *status. Returns false when the bound
 * ran out (the child is then killed and reaped, and *status holds SIGKILL) or
 * when waitpid() failed with anything but EINTR. */
static inline bool test_wait_child_bounded(pid_t pid, int bound_ms,
                                           int *status) {
  // Convert milliseconds to nanoseconds first.
  long long bound_ns = (long long)bound_ms * 1000000LL;
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);

  for (;;) {
    pid_t r = waitpid(pid, status, WNOHANG);

    if (r == pid) return true;

    if (r < 0 && errno != EINTR) return false;

    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = _diffTimeNsec(start, now);
    if (ns >= bound_ns) break;

    struct timespec nap = {0, 100 * 1000 * 1000};  // sleep for 100 microseconds
    nanosleep(&nap, NULL);
  }

  fprintf(stderr, "child %d still ran after %d ms; killed\n", (int)pid,
          bound_ms);
  kill(pid, SIGKILL);
  while (waitpid(pid, status, 0) < 0 && errno == EINTR) {
  }
  return false;
}

#endif /* CCOL_TESTS_TEST_WAIT_H */
