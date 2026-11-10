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

// An application destructor that runs after the exit destructors of
// cthreadcomm while it still owns live queues and a live loop. The library
// frees its process-wide bookkeeping (the queue mutex registry and the loop
// slot table) at exit only once nothing is live; otherwise the destroy of the
// last live object releases it. The late destructor checks that destroying what
// was live is safe (valgrind sees an invalid read without the deferred
// release), that a stale loop handle gives ccol_invalid_args, that new queues
// and loops then fail cleanly, and that a fork() runs the fork handlers over
// the released tables. It needs a process of its own. Priority 101 and being
// first on the link line each put the destructor last; we're past main there,
// so a failure prints a line and ends the process with _exit(1).

#include <cthreadcomm.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <unistd.h>

// What main leaves live for the destructor, and whether main got that far (a
// failed setup leaves nothing to check).
static ccircq *g_cq;
static cdynmq *g_dq;
static ceventloop g_loop = CEVENTLOOP_NULL;
static ceventloop_reg g_reg = CEVENTLOOP_REG_NULL;
static bool g_main_ran;

// The number of failed checks, reported at the end of the destructor.
static int g_failures;

// Records a failed check and prints what failed. The destructor runs after
// main, where tau can't report, so it keeps its own count in g_failures.
static void check(bool ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "tests_exit_order: FAILED: %s\n", what);
    g_failures++;
  }
}

// A handler that does nothing; the registration only has to exist.
static void on_readable_noop(ceventloop loop, ceventloop_reg reg,
                             cselectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

// Runs the checks after the library destructors.
__attribute__((destructor(101))) static void late_application_destructor(void) {
  if (!g_main_ran) test_child_exit(1);

  check(ceventloop_remove(g_loop, g_reg) == ccol_success,
        "remove of a registration that was live at exit");
  ceventloop stale_loop = g_loop;
  ceventloop_destroy(g_loop);
  ccircq_destroy(g_cq);
  cdynmq_destroy(g_dq);

  check(ceventloop_reg_count(stale_loop) == ccol_invalid_size,
        "reg_count of a stale loop handle after the release");
  check(ceventloop_pause(stale_loop, g_reg) == ccol_invalid_args,
        "pause through a stale loop handle after the release");

  char *err = NULL;
  ceventloop late_loop = ceventloop_create(8, 1, 1, &err);
  check(late_loop == CEVENTLOOP_NULL, "a loop created after the release fails");
  check(err != NULL, "a loop created after the release sets err_str");
  if (late_loop != CEVENTLOOP_NULL) ceventloop_destroy(late_loop);

#if CCOL_FORK_SAFETY_REQUIRED
  ccircq *late_cq = ccircq_create(4, NULL);
  check(late_cq == NULL, "a circular queue created after the release fails");
  if (late_cq) ccircq_destroy(late_cq);
  cdynmq *late_dq = cdynmq_create(NULL);
  check(late_dq == NULL, "a dynamic queue created after the release fails");
  if (late_dq) cdynmq_destroy(late_dq);
#endif

  pid_t pid = fork();
  if (pid == 0) test_child_exit(0);
  check(pid > 0, "fork after the release");
  if (pid > 0) {
    int status = 0;
    pid_t w;
    do {
      w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);
    check(w == pid, "waitpid of the child of a fork after the release");
    check(w == pid && WIFEXITED(status),
          "the child of a fork after the release exits");
  }

  if (g_failures) test_child_exit(1);
  fprintf(stderr, "tests_exit_order: OK\n");
}

// Creates a queue of each kind, a loop and one registration, and leaves them
// live on purpose. The inherited signal state is reset first, because the
// destructor waits for a child.
int main(void) {
  test_reset_inherited_signals();
  g_cq = ccircq_create(4, NULL);
  g_dq = cdynmq_create(NULL);
  g_loop = ceventloop_create(8, 1, 1, NULL);
  if (!g_cq || !g_dq || g_loop == CEVENTLOOP_NULL) {
    fprintf(stderr, "tests_exit_order: FAILED: setup\n");
    return 1;
  }
  ceventloop_handlers handlers = {.on_readable = on_readable_noop};
  g_reg = ceventloop_add(g_loop, ccircq_selectable(g_cq, cselect_read),
                         handlers, NULL, NULL);
  if (g_reg == CEVENTLOOP_REG_NULL) {
    fprintf(stderr, "tests_exit_order: FAILED: setup of the registration\n");
    return 1;
  }
  g_main_ran = true;
  return 0;
}
