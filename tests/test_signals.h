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

/* Signal helpers that the tests share, for the systems that differ. */

#ifndef CCOL_TESTS_TEST_SIGNALS_H
#define CCOL_TESTS_TEST_SIGNALS_H

#include <signal.h>
#include <sys/resource.h>
#include <time.h>

#include "test_coverage.h"

/* Puts SIGCHLD and SIGALRM back to their default dispositions and unblocks
 * SIGCHLD, SIGALRM and SIGUSR1. exec() keeps an ignored disposition and the
 * blocked mask, so a test binary inherits whatever the shell, make or the CI
 * runner left behind. With SIGCHLD ignored the kernel reaps every child at
 * once, waitpid() fails with ECHILD and system() reports a failure; a blocked
 * or ignored SIGALRM disarms the alarm() that bounds a forked child; and a
 * blocked SIGUSR1 never reaches the handler of a test that raises it. Call it
 * from a constructor, before any thread starts. SIGPIPE is left alone, because
 * each binary chooses its own SIGPIPE disposition on purpose. */
static inline void test_reset_inherited_signals(void) {
  signal(SIGCHLD, SIG_DFL);
  signal(SIGALRM, SIG_DFL);
  sigset_t inherited;
  sigemptyset(&inherited);
  sigaddset(&inherited, SIGCHLD);
  sigaddset(&inherited, SIGALRM);
  sigaddset(&inherited, SIGUSR1);
  sigprocmask(SIG_UNBLOCK, &inherited, NULL);
}

/* Turns off core dumps in a forked child that is meant to die by a signal (an
 * abort, a fatal error of the library). Where cores are on (FreeBSD and
 * OpenBSD by default, Linux with a core_pattern or systemd-coredump), the
 * kernel writes the whole heap of the test binary before the parent can reap
 * the child, which can take longer than the bound the parent waits with. In a
 * coverage build it also makes a SIGABRT write the child's coverage counts
 * before the child dies (test_child_flush_coverage_on_abort() of
 * test_coverage.h). Call it as the first statement of the child. */
static inline void test_child_no_core(void) {
  /* Only the soft limit, so a process that calls this by mistake can still
   * raise it again. */
  struct rlimit lim;
  if (getrlimit(RLIMIT_CORE, &lim) == 0) {
    lim.rlim_cur = 0;
    (void)setrlimit(RLIMIT_CORE, &lim);
  }
  test_child_flush_coverage_on_abort();
}

/* Takes every SIGPIPE that is pending for the calling thread or for the
 * process, and SIGPIPE has to be blocked when you call it. macOS and OpenBSD
 * have no sigtimedwait(), so there sigwait() takes the signal; it doesn't
 * block because the loop only calls it while sigpending() reports one. */
static inline void test_consume_pending_sigpipe(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
#if defined(__APPLE__) || defined(__OpenBSD__)
  sigset_t pending;
  int sig;
  while (sigemptyset(&pending) == 0 && sigpending(&pending) == 0 &&
         sigismember(&pending, SIGPIPE) == 1)
    (void)sigwait(&set, &sig);
#else
  const struct timespec zero = {0, 0};
  while (sigtimedwait(&set, NULL, &zero) == SIGPIPE) {
  }
#endif
}

#endif /* CCOL_TESTS_TEST_SIGNALS_H */
