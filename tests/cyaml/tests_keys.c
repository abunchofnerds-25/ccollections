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

// Parsing in a process with no thread-specific key left. The destructor of a
// cyaml key frees each thread's node pool and error buffer, so a thread that
// can't arm that key must keep neither: every node goes back to the allocator
// and a failed parse reports a fixed message. The key is created once per
// process, so this runs in its own binary where no cyaml call comes first; the
// memtest target runs it under valgrind, which reports anything left behind.
#include <cyaml.h>
#include <internal/test_helpers/cyaml.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

// How many keys the test may use up, and how many threads parse without one.
enum { MAX_TEST_KEYS = 65536, KEYS_TEST_THREADS = 50 };
// The keys the test creates to use up the process's supply.
static pthread_key_t exhausted[MAX_TEST_KEYS];

// Where the compiler emulates thread-local storage (OpenBSD), the runtime makes
// one pthread key on the first access to any thread-local and aborts when none
// is left. The test touches this first so that key exists, as it would in any
// real program, before using up the rest.
static __thread volatile int runtime_tls_probe;

// What one thread saw: how many of its parses worked, its pool size after the
// destroys, and whether a failed parse gave the fixed message.
typedef struct {
  int parsed;
  size_t pool_after_destroy;
  int fixed_message;
} keys_thread_result_t;

// Thread body: parses and destroys a document four times, then fails a parse,
// and records the results in the keys_thread_result_t that arg points at.
static void *parse_and_destroy(void *arg) {
  keys_thread_result_t *r = (keys_thread_result_t *)arg;
  for (int round = 0; round < 4; round++) {
    cyaml root =
        cyaml_parse("a: [1, 2, 3, {b: text}]\nc: [true, null]\n", NULL);
    if (root) r->parsed++;
    cyaml_destroy(root);
  }
  r->pool_after_destroy = cyaml_debug_pool_size();
  char *err = NULL;
  cyaml bad = cyaml_parse("[1, ", &err);
  r->fixed_message =
      bad == NULL && err != NULL &&
      strcmp(err, "parse failed; the error message could not be stored") == 0;
  cyaml_destroy(bad);
  return NULL;
}

// If node_free() pooled a node for a thread whose key isn't armed, the pool
// size is above zero after the destroys; if the error path kept a buffer, the
// real message comes back instead of the fixed one.
TEST(cyaml_pool_keys, threads_without_a_key_keep_no_nodes_and_no_buffer) {
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
}
