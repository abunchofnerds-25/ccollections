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
#include <time.h>

/* Consumes every SIGPIPE that is pending for the calling thread or for the
 * process. SIGPIPE must be blocked. macOS has no sigtimedwait(); there
 * sigwait() takes the signal, and it does not block, because the loop calls
 * it only while sigpending() reports the signal. */
static inline void test_consume_pending_sigpipe(void) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
#if defined(__APPLE__)
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
