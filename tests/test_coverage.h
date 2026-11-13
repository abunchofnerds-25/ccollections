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

// Coverage helpers for forked test children. gcov writes the counts from an
// exit handler, so a child that leaves by _exit(), abort() or a fatal error of
// the library loses them. Coverage builds define CCOL_TEST_COVERAGE; in every
// other build these helpers are a plain _exit() and nothing.
#ifndef CCOL_TESTS_TEST_COVERAGE_H
#define CCOL_TESTS_TEST_COVERAGE_H

#include <signal.h>
#include <string.h>
#include <unistd.h>

#ifdef CCOL_TEST_COVERAGE
// libgcov exports it for GCC and compiler-rt for Clang.
extern void __gcov_dump(void);
#endif

// Writes the coverage counts of this process to its .gcda files, for a process
// that won't reach the exit handler of gcov. It does nothing outside a coverage
// build.
static inline void test_child_coverage_flush(void) {
#ifdef CCOL_TEST_COVERAGE
  __gcov_dump();
#endif
}

// What a forked child calls in place of _exit(): It flushes the coverage counts
// first. Don't call it from a signal handler, since the flush isn't
// async-signal-safe.
static inline __attribute__((noreturn)) void test_child_exit(int code) {
  test_child_coverage_flush();
  _exit(code);
}

#ifdef CCOL_TEST_COVERAGE
// SIGABRT handler of a coverage build: Dumps the counts and raises sig again at
// its default action, so a child that aborts on purpose keeps its counts and
// still dies by SIGABRT. It resets SIGALRM and sig to their defaults. The dump
// can block on a lock that another thread held at the abort, so a 30 s SIGALRM
// (at its default action, keeping a shorter alarm the child armed) ends the
// child rather than the run hanging.
static inline void test_child_coverage_abort_handler(int sig) {
  signal(SIGALRM, SIG_DFL);
  unsigned left = alarm(30);
  if (left != 0 && left < 30) alarm(left);
  __gcov_dump();
  signal(sig, SIG_DFL);
  raise(sig);
}
#endif

// Installs the SIGABRT handler above, so a child that is meant to die by
// SIGABRT writes its coverage counts first. It does nothing outside a coverage
// build. The child still dies by SIGABRT, so the parent sees the same status.
// test_child_no_core() calls it.
static inline void test_child_flush_coverage_on_abort(void) {
#ifdef CCOL_TEST_COVERAGE
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = test_child_coverage_abort_handler;
  sigemptyset(&sa.sa_mask);
  (void)sigaction(SIGABRT, &sa, NULL);
#endif
}

#endif
