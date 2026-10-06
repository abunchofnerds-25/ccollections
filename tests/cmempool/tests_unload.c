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

/* The thread cache of a thread-safe pool after the module unloads. The unload
 * deletes the thread-specific key of the cache, and that cannot be undone in
 * one process. These tests therefore run in a binary of their own, so that no
 * other test of the suite runs with the cache gone. */

#include <cmempool.h>
#include <pthread.h>
#include <tau/tau.h>

extern size_t _ccol_mempool_live_magazines_for_tests(ccol_mempool *mp);
extern size_t _ccol_mempool_thread_magazines_for_tests(void);
extern size_t _ccol_mempool_reserve_for_tests(ccol_mempool *mp);
extern void _ccol_mempool_unload_cache_for_tests(void);

TAU_MAIN()

typedef struct {
  ccol_mempool *mp;
  int failed_allocs;
  size_t thread_magazines;
} user_args_t;

/* Takes and gives back entries from the pool, and records how many magazines
 * this thread holds before it exits. */
static void *pool_user(void *arg) {
  user_args_t *a = (user_args_t *)arg;
  for (int i = 0; i < 100; i++) {
    void *e = ccol_mempool_alloc_entry(a->mp);
    if (!e) {
      a->failed_allocs++;
      continue;
    }
    ccol_mempool_free_entry(a->mp, e);
  }
  a->thread_magazines = _ccol_mempool_thread_magazines_for_tests();
  return NULL;
}

/* After the unload, no thread builds a magazine. A magazine that a thread
 * built with no value on the deleted key would never be drained at the exit of
 * that thread: the pool would count it as live for ever, and the memtest of
 * this binary would report the magazine as lost. This test is non-vacuous:
 * without the refusal to build a magazine for a thread that cannot arm the
 * key, the thread below holds one magazine, and the pool still counts it after
 * the join. */
TEST(mempool_unload, no_thread_cache_is_built_after_the_module_unloads) {
  char *err = NULL;
  ccol_mempool *mp = ccol_mempool_create(64, 32, false, false, NULL, &err);
  REQUIRE_NE((void *)mp, (void *)NULL);

  /* Before the unload, this thread gets a magazine. */
  void *e = ccol_mempool_alloc_entry(mp);
  bool got_first = e != NULL;
  if (e) ccol_mempool_free_entry(mp, e);
  size_t mags_before_unload = _ccol_mempool_live_magazines_for_tests(mp);

  /* The unload drains this thread and deletes the key. */
  _ccol_mempool_unload_cache_for_tests();
  size_t mags_after_unload = _ccol_mempool_live_magazines_for_tests(mp);
  size_t own_after_unload = _ccol_mempool_thread_magazines_for_tests();

  user_args_t args = {.mp = mp};
  pthread_t t;
  int created = pthread_create(&t, NULL, pool_user, &args);
  if (created == 0) pthread_join(t, NULL);
  size_t mags_after_join = _ccol_mempool_live_magazines_for_tests(mp);

  /* This thread keeps working through the locked path, with no magazine. */
  int own_failed = 0;
  for (int i = 0; i < 100; i++) {
    void *x = ccol_mempool_alloc_entry(mp);
    if (!x) {
      own_failed++;
      continue;
    }
    ccol_mempool_free_entry(mp, x);
  }
  size_t own_after_use = _ccol_mempool_thread_magazines_for_tests();
  size_t used_at_end = ccol_mempool_used_count(mp);

  /* A pool created after the unload has no cache, and so no reserve. */
  ccol_mempool *late = ccol_mempool_create(64, 32, false, false, NULL, &err);
  size_t late_reserve = late ? _ccol_mempool_reserve_for_tests(late) : 1;
  if (late) ccol_mempool_destroy(late);
  ccol_mempool_destroy(mp);

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
