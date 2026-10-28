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

/* Coverage helpers for forked test children.
 *
 * gcov writes a binary's counts from an exit-time handler, so a child that
 * leaves through _exit(), or dies by abort() or a fatal error of the library,
 * throws away everything it ran. The parent never ran that code, and the
 * merged report then calls lines uncovered that a test runs and checks. Every
 * coverage target of the test Makefiles defines CCOL_TEST_COVERAGE; the
 * ordinary, sanitizer and valgrind builds don't, and there these helpers
 * compile to a plain _exit() and to nothing. */

#ifndef CCOL_TESTS_TEST_COVERAGE_H
#define CCOL_TESTS_TEST_COVERAGE_H

#include <signal.h>
#include <string.h>
#include <unistd.h>

#ifdef CCOL_TEST_COVERAGE
/* libgcov exports it for GCC and compiler-rt for Clang. */
extern void __gcov_dump(void);
#endif

/* Writes the counts of the calling process to its .gcda files in a coverage
 * build, and does nothing otherwise. */
static inline void test_child_coverage_flush(void) {
#ifdef CCOL_TEST_COVERAGE
  __gcov_dump();
#endif
}

/* What a forked child calls in place of _exit(), and so may any test process
 * that has to leave through _exit(): it flushes the coverage counts first.
 * Don't call it from a signal handler, because writing the .gcda files is not
 * async-signal-safe. */
static inline __attribute__((noreturn)) void test_child_exit(int code) {
  test_child_coverage_flush();
  _exit(code);
}

#ifdef CCOL_TEST_COVERAGE
static inline void test_child_coverage_abort_handler(int sig) {
  /* The dump allocates and takes locks, so in a child where another thread
   * held one of them when the abort came it can wait for ever. The alarm
   * then ends the child by SIGALRM, and the parent's check of SIGABRT fails
   * instead of the coverage run hanging. SIGALRM goes back to its default
   * action first, so a handler the child installed can't keep it alive, and
   * a shorter alarm the child already armed stays, since the parent's own
   * bound was sized for it. */
  signal(SIGALRM, SIG_DFL);
  unsigned left = alarm(30);
  if (left != 0 && left < 30) alarm(left);
  __gcov_dump();
  signal(sig, SIG_DFL);
  /* SIGABRT stays blocked while the handler runs, so the signal raised here
   * is delivered as soon as the handler returns, at its default action. */
  raise(sig);
}
#endif

/* Makes a child that is meant to die by SIGABRT write its coverage counts
 * first, in a coverage build. The child still dies by SIGABRT, so the parent
 * sees WIFSIGNALED and WTERMSIG == SIGABRT exactly as without it. Outside a
 * coverage build it does nothing. test_child_no_core() of test_signals.h
 * calls it, so a child that starts with that call needs nothing more. */
static inline void test_child_flush_coverage_on_abort(void) {
#ifdef CCOL_TEST_COVERAGE
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = test_child_coverage_abort_handler;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(SIGABRT, &sa, NULL);
#endif
}

#endif /* CCOL_TESTS_TEST_COVERAGE_H */
