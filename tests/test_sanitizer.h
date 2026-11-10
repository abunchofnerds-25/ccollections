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

// What a test binary can't run under the sanitizer it was built with.
#ifndef CCOL_TESTS_TEST_SANITIZER_H
#define CCOL_TESTS_TEST_SANITIZER_H

#include <stdio.h>

// Defined to 1 in a ThreadSanitizer build (Clang or GCC), so the macros
// below can tell that build apart.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TEST_SANITIZER_THREAD 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define TEST_SANITIZER_THREAD 1
#endif

// 1 when this build can run the fork tests, 0 for ThreadSanitizer on FreeBSD.
// There the runtime breaks in a forked child that starts threads (a join never
// returns, an event loop hangs), so that build skips the fork tests of the
// suites whose children start threads. A Makefile may define it first.
#ifndef TEST_FORK_RUNS
#if defined(TEST_SANITIZER_THREAD) && defined(__FreeBSD__)
#define TEST_FORK_RUNS 0
#else
#define TEST_FORK_RUNS 1
#endif
#endif

// 1 where pthread_timedjoin_np() exists and the runtime sees it.
// ThreadSanitizer on FreeBSD doesn't intercept it, so a thread joined that way
// stays alive for the runtime and a later thread start fails a CHECK. There a
// bounded join is a plain join.
#if defined(__linux__) || \
    (defined(__FreeBSD__) && !defined(TEST_SANITIZER_THREAD))
#define TEST_TIMEDJOIN_VISIBLE 1
#else
#define TEST_TIMEDJOIN_VISIBLE 0
#endif

// Ends the calling test with a SKIP line on stderr when TEST_FORK_RUNS is 0,
// and does nothing otherwise. Put it first in a test that forks, so a build
// that can't run the fork skips the test instead of hanging. It takes no
// arguments, is a statement (no value) and returns from the test function.
#define TEST_SKIP_FORK_IF_UNSUPPORTED()                                   \
  do {                                                                    \
    if (!TEST_FORK_RUNS) {                                                \
      fprintf(stderr,                                                     \
              "SKIP: ThreadSanitizer on FreeBSD cannot run this fork\n"); \
      return;                                                             \
    }                                                                     \
  } while (0)

// On OpenBSD it ends the calling test with a SKIP line on stderr; elsewhere it
// does nothing. Put it first in a test whose child starts threads and forks
// again: OpenBSD leaves the lock of its fork handler list held in a child of a
// threaded process, so the second fork() waits for ever, and we don't support
// that pattern there (doc/platforms.md). It takes no arguments, is a statement
// and returns from the test function.
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

// In a build with CCOL_FORK_SAFETY_REQUIRED=0 it ends the calling test with a
// SKIP line on stderr; elsewhere it does nothing. Put it first in a test that
// forks while other threads of the library run and then calls the library in
// the child: without the fork handlers that child can inherit a held lock and
// wait for ever. It takes no arguments, is a statement and returns from the
// test function.
#if defined(CCOL_FORK_SAFETY_REQUIRED) && CCOL_FORK_SAFETY_REQUIRED == 0
#define TEST_SKIP_FORK_WITHOUT_FORK_SAFETY()                                \
  do {                                                                      \
    fprintf(stderr,                                                         \
            "SKIP: without the fork handlers this fork can inherit a held " \
            "lock\n");                                                      \
    return;                                                                 \
  } while (0)
#else
#define TEST_SKIP_FORK_WITHOUT_FORK_SAFETY() \
  do {                                       \
  } while (0)
#endif

#endif
