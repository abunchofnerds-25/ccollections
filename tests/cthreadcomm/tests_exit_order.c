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

/* An application destructor that runs AFTER the process-exit destructors of
 * cthreadcomm, while it still owns live queues and a live ccol_event_loop.
 *
 * The library frees its process-wide bookkeeping (the queue mutex registry
 * and the event-loop slot table) at exit, but while anything is still live
 * it defers that release to the destroy that removes the last live object.
 * This binary checks four things from such a late destructor:
 *
 *   1. Destroying the queues and the loop that were live at exit is safe.
 *      Without the deferred release of the queue registry, the destroy of a
 *      queue indexes a freed vector, which valgrind reports as an invalid
 *      read (make memtest).
 *   2. Once the last object is gone and the bookkeeping is released, a new
 *      queue and a new loop fail cleanly: NULL and CCOL_EVENT_LOOP_INVALID.
 *      Without that handling, a queue create pushes into a freed vector and
 *      succeeds, and a loop create asserts inside cvector_elem_count.
 *   3. A stale loop handle resolves to nothing, and the call reports
 *      ccol_invalid_args instead of asserting.
 *   4. A fork() after the release runs the fork handlers of the module over
 *      the released tables without an assertion.
 *
 * The check runs in a process of its own because it needs the exit-time
 * destructor order of a whole process. The destructor carries priority 101,
 * which places it after every destructor with no priority, and this file is
 * also first on the link line; either one alone puts it last in the
 * .fini_array walk. Since the run is already past main, a failure writes a
 * line to stderr and ends the process with _exit(1). */

#include <cthreadcomm.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static ccol_circular_queue *g_cq;
static ccol_dynamic_queue *g_dq;
static ccol_event_loop g_loop = CCOL_EVENT_LOOP_INVALID;
static ccol_event_reg g_reg = CCOL_EVENT_REG_INVALID;
static bool g_main_ran;

static int g_failures;

static void check(bool ok, const char *what) {
  if (!ok) {
    fprintf(stderr, "tests_exit_order: FAILED: %s\n", what);
    g_failures++;
  }
}

static void on_readable_noop(ccol_event_loop loop, ccol_event_reg reg,
                             ccol_selectable *sel, void *arg) {
  (void)reg;
  (void)loop;
  (void)sel;
  (void)arg;
}

__attribute__((destructor(101))) static void late_application_destructor(void) {
  if (!g_main_ran) _exit(1);

  /* 1. Tear down what was live when the library destructors ran. */
  check(ccol_event_loop_remove(g_loop, g_reg) == ccol_success,
        "remove of a registration that was live at exit");
  ccol_event_loop stale_loop = g_loop;
  ccol_event_loop_destroy(g_loop);
  ccol_circular_queue_destroy(g_cq);
  ccol_dynamic_queue_destroy(g_dq);

  /* 3. A stale handle after the slot table is released. */
  check(ccol_event_loop_reg_count(stale_loop) == ccol_invalid_size,
        "reg_count of a stale loop handle after the release");
  check(ccol_event_loop_pause(stale_loop, g_reg) == ccol_invalid_args,
        "pause through a stale loop handle after the release");

  /* 2. New objects fail cleanly once the bookkeeping is released. */
  char *err = NULL;
  ccol_event_loop late_loop = ccol_event_loop_create(8, 1, 1, &err);
  check(late_loop == CCOL_EVENT_LOOP_INVALID,
        "a loop created after the release fails");
  check(err != NULL, "a loop created after the release sets err_str");
  if (late_loop != CCOL_EVENT_LOOP_INVALID) ccol_event_loop_destroy(late_loop);

#if CCOL_FORK_SAFETY_REQUIRED
  ccol_circular_queue *late_cq = ccol_circular_queue_create(4, NULL);
  check(late_cq == NULL, "a circular queue created after the release fails");
  if (late_cq) ccol_circular_queue_destroy(late_cq);
  ccol_dynamic_queue *late_dq = ccol_dynamic_queue_create(NULL);
  check(late_dq == NULL, "a dynamic queue created after the release fails");
  if (late_dq) ccol_dynamic_queue_destroy(late_dq);
#endif

  /* 4. The fork handlers walk the released tables. */
  pid_t pid = fork();
  if (pid == 0) _exit(0);
  check(pid > 0, "fork after the release");
  if (pid > 0) {
    int status = 0;
    pid_t w;
    do {
      w = waitpid(pid, &status, 0);
    } while (w < 0);
    check(WIFEXITED(status), "the child of a fork after the release exits");
  }

  if (g_failures) _exit(1);
  fprintf(stderr, "tests_exit_order: OK\n");
}

int main(void) {
  g_cq = ccol_circular_queue_create(4, NULL);
  g_dq = ccol_dynamic_queue_create(NULL);
  g_loop = ccol_event_loop_create(8, 1, 1, NULL);
  if (!g_cq || !g_dq || g_loop == CCOL_EVENT_LOOP_INVALID) {
    fprintf(stderr, "tests_exit_order: FAILED: setup\n");
    return 1;
  }
  ccol_event_handlers_t handlers = {.on_readable = on_readable_noop};
  g_reg = ccol_event_loop_add(
      g_loop, ccol_selectable_from_circq(g_cq, ccol_select_read), handlers,
      NULL, NULL);
  if (g_reg == CCOL_EVENT_REG_INVALID) {
    fprintf(stderr, "tests_exit_order: FAILED: setup of the registration\n");
    return 1;
  }
  g_main_ran = true;
  /* Everything stays live on purpose, so that the destructor above tears it
   * down after the library destructors have run. */
  return 0;
}
