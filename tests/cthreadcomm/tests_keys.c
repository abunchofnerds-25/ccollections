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

// ceventloop creation in a process with no thread-specific key left. A dispatch
// records the running loop and queue on two keys of this module, which is what
// makes a callback that destroys its own loop or queue fail loudly, so a loop
// without those keys must not be created. The keys are created once per
// process, so this runs in a binary of its own where no other ceventloop call
// comes first.

#include <cthreadcomm.h>
#include <pthread.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

// The keys the test takes to use up the process's supply.
enum { MAX_TEST_KEYS = 65536 };
static pthread_key_t exhausted[MAX_TEST_KEYS];

// If the create ignored its failed keys it would succeed, and its dispatch
// would set values on whatever keys the unset variables name, which belong to
// someone else. A queue needs no key and works here. Giving the keys back
// doesn't help, since the module never retries the key creation in this
// process.
TEST(ceventloop_keys, create_fails_cleanly_when_no_key_is_left) {
  int n = 0;
  while (n < MAX_TEST_KEYS && pthread_key_create(&exhausted[n], NULL) == 0) n++;

  char *err = NULL;
  ceventloop loop = ceventloop_create(8, 1, 1, &err);
  int created = loop != CEVENTLOOP_NULL;
  if (created) ceventloop_destroy(loop);

  ccircq *cq = ccircq_create(4, NULL);
  int queue_ok = cq != NULL;
  if (cq) {
    cmessage m = {.data = NULL, .size = 0};
    queue_ok = ccircq_send(cq, &m) == ccol_success &&
               ccircq_try_recv(cq, &m) == ccol_success;
    ccircq_destroy(cq);
  }

  for (int i = 0; i < n; i++) pthread_key_delete(exhausted[i]);
  char *err_again = NULL;
  ceventloop again = ceventloop_create(8, 1, 1, &err_again);
  int created_again = again != CEVENTLOOP_NULL;
  if (created_again) ceventloop_destroy(again);

  REQUIRE_GT(n, 0);
  REQUIRE_EQ(created, 0);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "key"), NULL);
  REQUIRE_TRUE(queue_ok);
  REQUIRE_EQ(created_again, 0);
  REQUIRE_NE((void *)err_again, NULL);
}
