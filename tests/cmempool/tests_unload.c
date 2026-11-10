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

// What the thread cache of a thread-safe pool does after the module unloads.
// The unload deletes the cache's thread-specific key and a process can't undo
// that, so these tests have a binary of their own.

#include <cmempool.h>
#include <internal/test_helpers/cmempool.h>
#include <pthread.h>
#include <tau/tau.h>

TAU_MAIN()

// Arguments and results of pool_user().
typedef struct {
  cmempool *mp;
  int failed_allocs;
  size_t thread_magazines;
} user_args_t;

// Thread body of the unload test: allocates and frees 100 entries of a->mp,
// counting failures, then records how many magazines this thread holds before
// it exits.
static void *pool_user(void *arg) {
  user_args_t *a = (user_args_t *)arg;
  for (int i = 0; i < 100; i++) {
    void *e = cmempool_alloc_entry(a->mp);
    if (!e) {
      a->failed_allocs++;
      continue;
    }
    cmempool_free_entry(a->mp, e);
  }
  a->thread_magazines = cmempool__thread_magazines_for_tests();
  return NULL;
}

// After the unload no thread may build a magazine: nothing would drain it at
// thread exit, the pool would count it as live forever and memtest would
// report it lost.
TEST(mempool_unload, no_thread_cache_is_built_after_the_module_unloads) {
  char *err = NULL;
  cmempool *mp = cmempool_create(64, 32, false, false, &err);
  REQUIRE_NE((void *)mp, (void *)NULL);

  void *e = cmempool_alloc_entry(mp);
  bool got_first = e != NULL;
  if (e) cmempool_free_entry(mp, e);
  size_t mags_before_unload = cmempool__live_magazines_for_tests(mp);

  cmempool__unload_cache_for_tests();
  size_t mags_after_unload = cmempool__live_magazines_for_tests(mp);
  size_t own_after_unload = cmempool__thread_magazines_for_tests();

  user_args_t args = {.mp = mp};
  pthread_t t;
  int created = pthread_create(&t, NULL, pool_user, &args);
  if (created == 0) pthread_join(t, NULL);
  size_t mags_after_join = cmempool__live_magazines_for_tests(mp);

  int own_failed = 0;
  for (int i = 0; i < 100; i++) {
    void *x = cmempool_alloc_entry(mp);
    if (!x) {
      own_failed++;
      continue;
    }
    cmempool_free_entry(mp, x);
  }
  size_t own_after_use = cmempool__thread_magazines_for_tests();
  size_t used_at_end = cmempool_used_count(mp);

  cmempool *late = cmempool_create(64, 32, false, false, &err);
  size_t late_reserve = late ? cmempool__reserve_for_tests(late) : 1;
  if (late) cmempool_destroy(late);
  cmempool_destroy(mp);

  REQUIRE_TRUE(got_first);
  REQUIRE_EQ(mags_before_unload, (size_t)1);
  REQUIRE_EQ(mags_after_unload, (size_t)0);
  REQUIRE_EQ(own_after_unload, (size_t)0);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(args.failed_allocs, 0);
  REQUIRE_EQ(args.thread_magazines, (size_t)0);
  REQUIRE_EQ(mags_after_join, (size_t)0);
  REQUIRE_EQ(own_failed, 0);
  REQUIRE_EQ(own_after_use, (size_t)0);
  REQUIRE_EQ(used_at_end, (size_t)0);
  REQUIRE_EQ(late_reserve, (size_t)0);
}
