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

/* Parsing in a process that has no thread-specific key left to give. Each
 * thread's node pool and error buffer are freed by the destructor of a key
 * of this module, so a thread that can't arm that key must keep neither of
 * them. Every node goes straight back to the allocator and a failed parse
 * reports a fixed message. The key is created once per process, which is
 * why this runs in its own binary where no other cjson call comes first.
 * The memtest target runs it under valgrind, and valgrind reports any node
 * or buffer that a thread left behind. */

#include <cjson.h>
#include <internal/test_helpers/cjson.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

enum { MAX_TEST_KEYS = 65536, KEYS_TEST_THREADS = 50 };
static pthread_key_t exhausted[MAX_TEST_KEYS];

/* Where the compiler emulates thread-local storage (clang and GCC on
 * OpenBSD), the runtime creates one pthread key for all the thread-locals of
 * the process on the first access to any of them, and it aborts when no key
 * is left. A real program has touched some thread-local long before it could
 * use up every key, so the test touches one of its own first. That access
 * creates the runtime's key and has nothing to do with the key of this
 * module. */
static __thread volatile int runtime_tls_probe;

typedef struct {
  int parsed;
  size_t pool_after_destroy;
  int fixed_message;
} keys_thread_result_t;

static void *parse_and_destroy(void *arg) {
  keys_thread_result_t *r = (keys_thread_result_t *)arg;
  for (int round = 0; round < 4; round++) {
    cjson root =
        cjson_parse("{\"a\":[1,2,3,{\"b\":\"text\"}],\"c\":[true,null]}", NULL);
    if (root) r->parsed++;
    cjson_destroy(root);
  }
  r->pool_after_destroy = cjson_debug_pool_size();
  char *err = NULL;
  cjson bad = cjson_parse("[1,", &err);
  r->fixed_message =
      bad == NULL && err != NULL &&
      strcmp(err, "parse failed; the error message could not be stored") == 0;
  cjson_destroy(bad);
  return NULL;
}

/* A pool that breaks the rule shows up here in two ways. If node_free() pools
 * a node for a thread whose key is not armed, cjson_debug_pool_size() is above
 * zero after the destroy and the population counter never gets back to zero,
 * and if the error path keeps a buffer for such a thread we get the real
 * message back. */
TEST(cjson_pool_keys, threads_without_a_key_keep_no_nodes_and_no_buffer) {
  runtime_tls_probe = 1;
  int n = 0;
  while (n < MAX_TEST_KEYS && pthread_key_create(&exhausted[n], NULL) == 0) n++;

  keys_thread_result_t results[KEYS_TEST_THREADS];
  pthread_t tids[KEYS_TEST_THREADS];
  memset(results, 0, sizeof(results));
  int started = 0;
  for (int i = 0; i < KEYS_TEST_THREADS; i++) {
    if (pthread_create(&tids[i], NULL, parse_and_destroy, &results[i]) != 0)
      break;
    started++;
  }
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);

  /* The main thread takes the same path. */
  keys_thread_result_t main_result;
  memset(&main_result, 0, sizeof(main_result));
  parse_and_destroy(&main_result);

  for (int i = 0; i < n; i++) pthread_key_delete(exhausted[i]);

  REQUIRE_GT(n, 0);
  REQUIRE_EQ(started, KEYS_TEST_THREADS);
  for (int i = 0; i < started; i++) {
    REQUIRE_EQ(results[i].parsed, 4);
    REQUIRE_EQ(results[i].pool_after_destroy, (size_t)0);
    REQUIRE_TRUE(results[i].fixed_message);
  }
  REQUIRE_EQ(main_result.parsed, 4);
  REQUIRE_EQ(main_result.pool_after_destroy, (size_t)0);
  REQUIRE_TRUE(main_result.fixed_message);
  REQUIRE_EQ(atomic_load(&_cjson_pool_population_for_tests), 0L);
}
