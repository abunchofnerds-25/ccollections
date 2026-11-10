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

// Signal helpers that the tests share, for the systems that differ.
#ifndef CCOL_TESTS_TEST_SIGNALS_H
#define CCOL_TESTS_TEST_SIGNALS_H

#include <signal.h>
#include <sys/resource.h>
#include <time.h>

#include "test_coverage.h"

// Resets the signal state a test binary inherited. It puts SIGCHLD and SIGALRM
// back to their defaults and unblocks SIGCHLD, SIGALRM and SIGUSR1, since
// exec() keeps whatever the shell or the CI runner left (an ignored SIGCHLD
// breaks waitpid(), a blocked SIGALRM disarms alarm()). SIGPIPE stays as each
// binary chose it. Call it from a constructor, before any thread starts.
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

// Turns off core dumps of the calling process (soft limit only). It's for a
// forked child that is meant to die by a signal, because writing the core of
// the test heap can outlast the parent's wait. It also makes a SIGABRT flush
// the coverage counts. Call it as the first statement of the child.
static inline void test_child_no_core(void) {
  struct rlimit lim;
  if (getrlimit(RLIMIT_CORE, &lim) == 0) {
    lim.rlim_cur = 0;
    (void)setrlimit(RLIMIT_CORE, &lim);
  }
  test_child_flush_coverage_on_abort();
}

// Takes every pending SIGPIPE of the thread and the process, so a test that
// blocked SIGPIPE around a write can check that nothing stays pending and leave
// a clean state. SIGPIPE must be blocked. macOS and OpenBSD have no
// sigtimedwait(), so there sigwait() runs only while sigpending() reports one.
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

#endif
