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

/* What a test binary can't run under the sanitizer it was built with.
 *
 * ThreadSanitizer doesn't support a child of a multithreaded fork() that
 * starts threads of its own (its die_after_fork option, on by default, ends
 * such a child), and every ThreadSanitizer process is multithreaded since
 * the runtime has a thread of its own. The suites turn that option off and
 * on Linux those children run fine. On FreeBSD the runtime's thread registry
 * breaks in them: joining a thread that the child started never returns,
 * and a child that starts an event loop hangs at random. So a
 * ThreadSanitizer build on FreeBSD skips the tests that fork in the suites
 * whose children start threads, and every other build runs them. That same
 * build can't see a pthread_timedjoin_np() either, see
 * TEST_TIMEDJOIN_VISIBLE below. */

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

/* glibc, musl and FreeBSD have pthread_timedjoin_np(), macOS and OpenBSD
 * don't.
 * ThreadSanitizer on FreeBSD intercepts pthread_join() and pthread_detach()
 * but not pthread_timedjoin_np(), so a thread joined through the latter
 * stays alive as far as the runtime knows. It sees no ordering between that
 * thread and what the joiner does next, and the next thread that libthr
 * starts in the same thread structure stops the process with a CHECK
 * failure of the runtime. In every other build a bounded join just joins
 * without its bound, and the CI job's timeout bounds a hang. */
#if defined(__linux__) || \
    (defined(__FreeBSD__) && !defined(TEST_SANITIZER_THREAD))
#define TEST_TIMEDJOIN_VISIBLE 1
#else
#define TEST_TIMEDJOIN_VISIBLE 0
#endif

/* Put this first in a test that forks; in a build that can't run the test it
 * ends it with a SKIP line. */
#define TEST_SKIP_FORK_IF_UNSUPPORTED()                                   \
  do {                                                                    \
    if (!TEST_FORK_RUNS) {                                                \
      fprintf(stderr,                                                     \
              "SKIP: ThreadSanitizer on FreeBSD cannot run this fork\n"); \
      return;                                                             \
    }                                                                     \
  } while (0)

/* Put this first in a test whose forked child starts threads and then forks
 * again. In a child of a threaded process OpenBSD's fork() leaves the lock
 * of its fork handler list held, so once the child has started a thread the
 * second fork() waits there for ever. We don't support that pattern on
 * OpenBSD (doc/platforms.md). */
#if defined(__OpenBSD__)
#define TEST_SKIP_FORK_AGAIN_IN_A_THREADED_CHILD()                         \
  do {                                                                     \
    fprintf(stderr,                                                        \
            "SKIP: on OpenBSD a child of a threaded process cannot start " \
            "threads and fork again\n");                                   \
    return;                                                                \
  } while (0)
#else
#define TEST_SKIP_FORK_AGAIN_IN_A_THREADED_CHILD() \
  do {                                             \
  } while (0)
#endif

#endif /* CCOL_TESTS_TEST_SANITIZER_H */
