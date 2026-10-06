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

/* ccol_event_loop creation in a process that has no thread-specific key left
 * to give. The dispatch of a loop records the running loop and the running
 * queue on two keys of this module, which is what lets a callback that
 * destroys its own loop or its own queue fail loudly instead of freeing it
 * under its own dispatch. A loop that cannot have those keys must therefore
 * not be created. The keys are created once for each process, so this runs in
 * a binary of its own: no other ccol_event_loop call may come first. */

#include <cthreadcomm.h>
#include <pthread.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

enum { MAX_TEST_KEYS = 65536 };
static pthread_key_t exhausted[MAX_TEST_KEYS];

/* This test is non-vacuous: when the loop ignores the failure to create its
 * keys, the create succeeds, and its dispatch sets the running loop and queue
 * on whatever key the unset key variables name, which is a key of another
 * component. */
TEST(ccol_event_loop_keys, create_fails_cleanly_when_no_key_is_left) {
  int n = 0;
  while (n < MAX_TEST_KEYS && pthread_key_create(&exhausted[n], NULL) == 0) n++;

  char *err = NULL;
  ccol_event_loop loop = ccol_event_loop_create(8, 1, 1, &err);
  int created = loop != CCOL_EVENT_LOOP_INVALID;
  if (created) ccol_event_loop_destroy(loop);

  /* A queue needs no key: it still works, and its destroy is safe. */
  ccol_circular_queue *cq = ccol_circular_queue_create(4, NULL);
  int queue_ok = cq != NULL;
  if (cq) {
    c_message_t m = {.data = NULL, .size = 0};
    queue_ok = ccol_circq_send_zc(cq, &m) == ccol_success &&
               ccol_circq_try_recv_zc(cq, &m) == ccol_success;
    ccol_circular_queue_destroy(cq);
  }

  /* The keys come back, and the failure stays: the module does not retry
   * the creation of its keys in this process. */
  for (int i = 0; i < n; i++) pthread_key_delete(exhausted[i]);
  char *err_again = NULL;
  ccol_event_loop again = ccol_event_loop_create(8, 1, 1, &err_again);
  int created_again = again != CCOL_EVENT_LOOP_INVALID;
  if (created_again) ccol_event_loop_destroy(again);

  REQUIRE_GT(n, 0);
  REQUIRE_EQ(created, 0);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "key"), NULL);
  REQUIRE_TRUE(queue_ok);
  REQUIRE_EQ(created_again, 0);
  REQUIRE_NE((void *)err_again, NULL);
}
