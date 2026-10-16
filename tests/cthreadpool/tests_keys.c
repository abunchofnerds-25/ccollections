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

/* Pool creation in a process that has no thread-specific key left to give.
 * A pool's workers record themselves on a key of this module, and that key
 * is what makes a task that destroys its own pool fail loudly instead of
 * freeing the pool under its own worker. A pool that can't get the key must
 * therefore not be created at all. The key is created once per process, so
 * this runs in its own binary where no other ctpool call comes first. */

#include <cthreadpool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

static atomic_int app_destructor_calls;

static void app_destructor(void *value) {
  (void)value;
  atomic_fetch_add(&app_destructor_calls, 1);
}

static void noop_task(void *arg) { (void)arg; }

enum { MAX_TEST_KEYS = 65536 };
static pthread_key_t exhausted[MAX_TEST_KEYS];

/* If the pool ignored the failure to create its key, the create would
 * succeed and the workers would set their pool on whatever key the unset
 * key variable happens to name. That is somebody else's key (here the one
 * this test created first), and its destructor would get the pool when each
 * worker exits. */
TEST(ctpool_worker_key, create_fails_cleanly_when_no_key_is_left) {
  pthread_key_t app_key;
  int app_key_ok = pthread_key_create(&app_key, app_destructor) == 0;
  int n = 0;
  while (n < MAX_TEST_KEYS && pthread_key_create(&exhausted[n], NULL) == 0) n++;

  char *err = NULL;
  ctpool p = ctpool_create(1, 0, &err);
  /* ctpool_destroy clears the handle, so we keep the outcome first. */
  int created = p != CTPOOL_INVALID;
  if (created) {
    ctpool_submit(p, noop_task, NULL, NULL);
    ctpool_shutdown_drain(p);
    ctpool_destroy(p);
  }

  /* The keys are free again but the failure stays, because the module
   * doesn't try to create its key a second time in this process. */
  for (int i = 0; i < n; i++) pthread_key_delete(exhausted[i]);
  char *err_again = NULL;
  ctpool again = ctpool_create(1, 0, &err_again);
  int created_again = again != CTPOOL_INVALID;
  if (created_again) ctpool_destroy(again);
  if (app_key_ok) pthread_key_delete(app_key);

  REQUIRE_TRUE(app_key_ok);
  REQUIRE_GT(n, 0);
  REQUIRE_EQ(created, 0);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "key"), NULL);
  REQUIRE_EQ(atomic_load(&app_destructor_calls), 0);
  REQUIRE_EQ(created_again, 0);
  REQUIRE_NE((void *)err_again, NULL);
}
