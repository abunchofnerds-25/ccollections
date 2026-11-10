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

// Pool creation in a process with no thread-specific key left. Workers mark
// themselves on a key of this module, which is what makes a task that destroys
// its own pool fail loudly, so a pool that can't get the key must not be
// created. The key is created once per process, so this runs in its own binary
// where no other cthrpool call comes first.
#include <cthreadpool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

// Counts the calls of app_destructor, the destructor of the test's own key.
static atomic_int app_destructor_calls;

// Counts its calls; it must never run, since a worker that set its pool on the
// test's key by mistake would make it run at thread exit.
static void app_destructor(void *value) {
  (void)value;
  atomic_fetch_add(&app_destructor_calls, 1);
}

// A task that does nothing, submitted to a pool that the create
// should have refused.
static void noop_task(void *arg) { (void)arg; }

// The keys the test takes to use up every key of the process.
enum { MAX_TEST_KEYS = 65536 };
static pthread_key_t exhausted[MAX_TEST_KEYS];

// Without the check, the create succeeds and the workers set their pool on
// whatever key the unset key variable names, here the test's own key, whose
// destructor then gets the pool. The failure must also stay after the keys are
// free again, since the module never retries the key.
TEST(cthrpool_worker_key, create_fails_cleanly_when_no_key_is_left) {
  pthread_key_t app_key;
  int app_key_ok = pthread_key_create(&app_key, app_destructor) == 0;
  int n = 0;
  while (n < MAX_TEST_KEYS && pthread_key_create(&exhausted[n], NULL) == 0) n++;

  char *err = NULL;
  cthrpool p = cthrpool_create(1, 0, &err);
  int created = p != CTHRPOOL_NULL;
  if (created) {
    cthrpool_submit(p, noop_task, NULL, NULL);
    cthrpool_shutdown_drain(p);
    cthrpool_destroy(p);
  }

  for (int i = 0; i < n; i++) pthread_key_delete(exhausted[i]);
  char *err_again = NULL;
  cthrpool again = cthrpool_create(1, 0, &err_again);
  int created_again = again != CTHRPOOL_NULL;
  if (created_again) cthrpool_destroy(again);
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
