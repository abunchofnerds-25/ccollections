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

/* What the sanitizer that a test binary is built with cannot run.
 *
 * ThreadSanitizer does not support a child of a multithreaded fork() that
 * starts threads of its own (its die_after_fork option, on by default, ends
 * such a child); every ThreadSanitizer process is multithreaded, because the
 * runtime runs a thread of its own. The suites turn that option off, and on
 * Linux such children run. On FreeBSD the runtime's thread registry breaks in
 * them: a join of a thread that the child started never returns, and a child
 * that starts an event loop hangs at random. A ThreadSanitizer build on
 * FreeBSD therefore skips the tests that fork, in the suites whose children
 * start threads; every other build runs them. The same build cannot see a
 * pthread_timedjoin_np(); see TEST_TIMEDJOIN_VISIBLE below. */

#ifndef CCOL_TESTS_TEST_SANITIZER_H
#define CCOL_TESTS_TEST_SANITIZER_H

#include <stdio.h>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TEST_SANITIZER_THREAD 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define TEST_SANITIZER_THREAD 1
#endif

#ifndef TEST_FORK_RUNS
#if defined(TEST_SANITIZER_THREAD) && defined(__FreeBSD__)
#define TEST_FORK_RUNS 0
#else
#define TEST_FORK_RUNS 1
#endif
#endif

/* ThreadSanitizer on FreeBSD intercepts pthread_join() and pthread_detach()
 * and not pthread_timedjoin_np(). A thread joined through the latter stays
 * live for the runtime: it sees no ordering between that thread and what the
 * joiner does next, and the next thread that libthr starts in the same thread
 * structure stops the process with a CHECK failure of the runtime. A bounded
 * join therefore joins without its bound in such a build. */
#if defined(TEST_SANITIZER_THREAD) && defined(__FreeBSD__)
#define TEST_TIMEDJOIN_VISIBLE 0
#else
#define TEST_TIMEDJOIN_VISIBLE 1
#endif

/* The first statement of a test that forks: it ends the test with a SKIP
 * line in a build that cannot run it. */
#define TEST_SKIP_FORK_IF_UNSUPPORTED()                                   \
  do {                                                                    \
    if (!TEST_FORK_RUNS) {                                                \
      fprintf(stderr,                                                     \
              "SKIP: ThreadSanitizer on FreeBSD cannot run this fork\n"); \
      return;                                                             \
    }                                                                     \
  } while (0)

#endif /* CCOL_TESTS_TEST_SANITIZER_H */
