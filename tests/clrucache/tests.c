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

#include <clrucache.h>
#include <fcntl.h>
#include <float.h>
#include <internal/test_helpers/clrucache.h>
#include <internal/test_helpers/cpintable.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// Resets the signal state inherited from the parent process, since the forked
// children of this binary report through their wait status.
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

TEST(basic, set_and_get_int_key_int_value) {
  ccache_construct_scoped(cache, int, int, 10, NULL, NULL, NULL);

  int k = 42, v = 100;
  ccol_retval r = ccache_set(cache, k, v);
  REQUIRE_EQ(r, ccol_success);

  int out = 0;
  r = ccache_get(cache, k, &out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(out, 100);

  ccache_destroy(cache);
}

TEST(basic, char_ptr_get_miss_does_not_modify_output) {
  ccache_construct_scoped(cache, int, char *, 8, NULL, NULL, NULL);

  char sentinel_buf[1] = {0};
  char *out = sentinel_buf;
  REQUIRE_EQ(ccache_get(cache, 99, &out), ccol_key_not_found);
  REQUIRE_PTR_EQ(out, sentinel_buf);

  ccache_destroy(cache);
}

TEST(basic, get_missing_key_returns_not_found) {
  ccache_construct_scoped(cache, int, int, 10, NULL, NULL, NULL);

  int out = 0;
  ccol_retval r = ccache_get(cache, 99, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  ccache_destroy(cache);
}

TEST(basic, overwrite_value) {
  ccache_construct_scoped(cache, int, int, 10, NULL, NULL, NULL);

  int k = 1, v1 = 10, v2 = 20;
  ccache_set(cache, k, v1);
  ccache_set(cache, k, v2);

  int out = 0;
  ccache_get(cache, k, &out);
  REQUIRE_EQ(out, 20);

  ccache_destroy(cache);
}

TEST(basic, string_key_int_value) {
  ccache_construct_scoped(cache, char *, int, 8, NULL, NULL, NULL);

  int v = 55;
  ccol_retval r = ccache_set(cache, "hello", v);
  REQUIRE_EQ(r, ccol_success);

  int out = 0;
  r = ccache_get(cache, "hello", &out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(out, 55);

  int miss = 0;
  r = ccache_get(cache, "world", &miss);
  REQUIRE_EQ(r, ccol_key_not_found);

  ccache_destroy(cache);
}

TEST(basic, size_and_capacity) {
  ccache_construct_scoped(cache, int, int, 5, NULL, NULL, NULL);

  REQUIRE_EQ(clrucache_capacity(cache), (size_t)5);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  for (int i = 0; i < 3; i++) {
    ccache_set(cache, i, i * 10);
  }
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  ccache_destroy(cache);
}

TEST(basic, invalid_args_get) {
  ccache_construct_scoped(cache, int, int, 4, NULL, NULL, NULL);

  int k = 1;
  cmap_pair kp = {};
  ccol__populate_cmap_pair(&kp, k);
  cmap_pair val_out = {};

  REQUIRE_EQ(clrucache_get_full(CCACHE_NULL, &kp, &val_out), ccol_invalid_args);
  REQUIRE_EQ(clrucache_get_full(cache, NULL, &val_out), ccol_invalid_args);
  REQUIRE_EQ(clrucache_get_full(cache, &kp, NULL), ccol_invalid_args);

  cmap_pair null_ptr_kp = {.ptr = NULL, .size = sizeof(int)};
  REQUIRE_EQ(clrucache_get_full(cache, &null_ptr_kp, &val_out),
             ccol_invalid_args);

  cmap_pair zero_size_kp = {.ptr = &k, .size = 0};
  REQUIRE_EQ(clrucache_get_full(cache, &zero_size_kp, &val_out),
             ccol_invalid_args);

  ccache_destroy(cache);
}

TEST(basic, invalid_args_set) {
  ccache_construct_scoped(cache, int, int, 4, NULL, NULL, NULL);

  int k = 1, v = 2;
  cmap_pair kp = {}, vp = {};
  ccol__populate_cmap_pair(&kp, k);
  ccol__populate_cmap_pair(&vp, v);

  REQUIRE_EQ(clrucache_set_full(CCACHE_NULL, &kp, &vp), ccol_invalid_args);
  REQUIRE_EQ(clrucache_set_full(cache, NULL, &vp), ccol_invalid_args);
  REQUIRE_EQ(clrucache_set_full(cache, &kp, NULL), ccol_invalid_args);

  cmap_pair zero_vp = {.ptr = &v, .size = 0};
  REQUIRE_EQ(clrucache_set_full(cache, &kp, &zero_vp), ccol_invalid_args);

  cmap_pair null_ptr_kp = {.ptr = NULL, .size = sizeof(int)};
  REQUIRE_EQ(clrucache_set_full(cache, &null_ptr_kp, &vp), ccol_invalid_args);

  cmap_pair null_ptr_vp = {.ptr = NULL, .size = sizeof(int)};
  REQUIRE_EQ(clrucache_set_full(cache, &kp, &null_ptr_vp), ccol_invalid_args);

  cmap_pair zero_size_kp = {.ptr = &k, .size = 0};
  REQUIRE_EQ(clrucache_set_full(cache, &zero_size_kp, &vp), ccol_invalid_args);

  ccache_destroy(cache);
}

TEST(basic, capacity_zero_returns_null) {
  char *err = NULL;
  ccache cache = clrucache_create_full(0, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, &err);
  REQUIRE_EQ(cache, CCACHE_NULL);
  REQUIRE_NOT_NULL(err);
}

TEST(basic, null_cache_size_and_capacity_return_zero) {
  REQUIRE_EQ(clrucache_size(CCACHE_NULL), (size_t)0);
  REQUIRE_EQ(clrucache_capacity(CCACHE_NULL), (size_t)0);
}

TEST(basic, destroy_null_cache_is_noop) {
  ccache cache = CCACHE_NULL;
  ccache_destroy(cache);
  REQUIRE_EQ(cache, CCACHE_NULL);
}

TEST(basic, destroy_sets_handle_null) {
  ccache_construct_scoped(cache, int, int, 4, NULL, NULL, NULL);
  ccache_set(cache, 1, 10);
  REQUIRE_NE(cache, CCACHE_NULL);
  ccache_destroy(cache);
  REQUIRE_EQ(cache, CCACHE_NULL);
}

TEST(basic, create_full_null_err_on_failure) {
  ccache cache = clrucache_create_full(0, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, NULL);
  REQUIRE_EQ(cache, CCACHE_NULL);
}

TEST(basic, size_unchanged_on_overwrite) {
  ccache_construct_scoped(cache, int, int, 10, NULL, NULL, NULL);

  int k = 7;
  ccache_set(cache, k, 100);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  ccache_set(cache, k, 200);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  int out = 0;
  ccache_get(cache, k, &out);
  REQUIRE_EQ(out, 200);

  ccache_destroy(cache);
}

// What record_eviction saw: The last evicted int key and value, and how many
// evictions there were.
static int eviction_key_captured = -1;
static int eviction_val_captured = -1;
static int eviction_count = 0;

// An eviction callback of int caches that records the evicted key and value, so
// a test can check which entry the cache chose.
static void record_eviction(const cmap_pair *key, const cmap_pair *val) {
  eviction_key_captured = *(const int *)key->ptr;
  eviction_val_captured = *(const int *)val->ptr;
  eviction_count++;
}

// What record_str_eviction saw: The last evicted string key and value, and how
// many evictions there were.
static char str_eviction_key[64];
static char str_eviction_val[64];
static int str_eviction_count = 0;

// An eviction callback of string caches that copies the evicted key and value,
// so a test can check what the callback gets.
static void record_str_eviction(const cmap_pair *key, const cmap_pair *val) {
  strncpy(str_eviction_key, (const char *)key->ptr,
          sizeof(str_eviction_key) - 1);
  strncpy(str_eviction_val, (const char *)val->ptr,
          sizeof(str_eviction_val) - 1);
  str_eviction_count++;
}

TEST(eviction, lru_order_respected) {
  eviction_key_captured = -1;
  eviction_val_captured = -1;
  eviction_count = 0;

  ccache_construct_scoped(cache, int, int, 3, NULL, NULL, record_eviction);

  for (int i = 0; i < 3; i++) {
    ccache_set(cache, i, i * 10);
  }
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  int out = 0;
  ccache_get(cache, 0, &out);

  ccache_set(cache, 3, 30);

  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);
  REQUIRE_EQ(eviction_val_captured, 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  ccol_retval r = ccache_get(cache, 1, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  ccache_get(cache, 0, &out);
  REQUIRE_EQ(out, 0);
  ccache_get(cache, 2, &out);
  REQUIRE_EQ(out, 20);
  ccache_get(cache, 3, &out);
  REQUIRE_EQ(out, 30);

  ccache_destroy(cache);
}

TEST(eviction, eviction_callback_called_on_destroy) {
  eviction_count = 0;

  ccache_construct(cache, int, int, 10, NULL, NULL, record_eviction);
  for (int i = 0; i < 5; i++) {
    ccache_set(cache, i, i);
  }

  ccache_destroy(cache);
  REQUIRE_EQ(eviction_count, 5);
}

TEST(eviction, size_tracked_correctly_across_evictions) {
  eviction_count = 0;
  ccache_construct_scoped(cache, int, int, 3, NULL, NULL, record_eviction);

  for (int i = 0; i < 3; i++) ccache_set(cache, i, i);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(eviction_count, 0);

  for (int i = 3; i < 7; i++) ccache_set(cache, i, i);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(eviction_count, 4);

  ccache_destroy(cache);
  REQUIRE_EQ(eviction_count, 7);
}

TEST(eviction, set_promotes_existing_entry_to_mru) {
  eviction_count = 0;
  eviction_key_captured = -1;

  ccache_construct_scoped(cache, int, int, 3, NULL, NULL, record_eviction);

  for (int i = 0; i < 3; i++) ccache_set(cache, i, i * 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  ccache_set(cache, 0, 99);
  REQUIRE_EQ(eviction_count, 0);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  ccache_set(cache, 3, 30);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_key_not_found);
  REQUIRE_EQ(ccache_get(cache, 0, &out), ccol_success);
  REQUIRE_EQ(out, 99);
  REQUIRE_EQ(ccache_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(ccache_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 30);

  ccache_destroy(cache);
}

// A remote setter that, once armed, rejects exactly rejecting_setter_key, so a
// test can seed the key first and make a later set of it fail. It never
// blocks, which keeps the tests that use it deterministic.
static int rejecting_setter_calls = 0;
static bool rejecting_setter_armed = false;
static int rejecting_setter_key = 0;
// Counts its call and fails only for the armed key.
static bool rejecting_remote_setter(const cmap_pair *key,
                                    const cmap_pair *val) {
  (void)val;
  rejecting_setter_calls++;
  if (!rejecting_setter_armed) return true;
  return *(const int *)key->ptr != rejecting_setter_key;
}

TEST(eviction, failed_set_leaves_the_eviction_order_untouched) {
  eviction_count = 0;
  eviction_key_captured = -1;
  eviction_val_captured = -1;
  rejecting_setter_calls = 0;
  rejecting_setter_armed = false;
  rejecting_setter_key = 1;

  ccache_construct_scoped(cache, int, int, 3, NULL, rejecting_remote_setter,
                          record_eviction);

  REQUIRE_EQ(ccache_set(cache, 1, 10), ccol_success);
  REQUIRE_EQ(ccache_set(cache, 2, 20), ccol_success);
  REQUIRE_EQ(ccache_set(cache, 3, 30), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(eviction_count, 0);

  rejecting_setter_armed = true;

  REQUIRE_EQ(ccache_set(cache, 1, 99), ccol_unexpected_failure);
  REQUIRE_EQ(eviction_count, 0);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  REQUIRE_EQ(ccache_set(cache, 4, 40), ccol_success);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);
  REQUIRE_EQ(eviction_val_captured, 10);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_key_not_found);
  REQUIRE_EQ(ccache_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(ccache_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 30);
  REQUIRE_EQ(ccache_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 40);

  ccache_destroy(cache);
}

TEST(eviction, failed_set_does_not_move_a_mid_list_key_to_the_front) {
  eviction_count = 0;
  eviction_key_captured = -1;
  rejecting_setter_calls = 0;
  rejecting_setter_armed = false;
  rejecting_setter_key = 2;

  ccache_construct_scoped(cache, int, int, 3, NULL, rejecting_remote_setter,
                          record_eviction);

  REQUIRE_EQ(ccache_set(cache, 1, 10), ccol_success);
  REQUIRE_EQ(ccache_set(cache, 2, 20), ccol_success);
  REQUIRE_EQ(ccache_set(cache, 3, 30), ccol_success);

  rejecting_setter_armed = true;
  REQUIRE_EQ(ccache_set(cache, 2, 99), ccol_unexpected_failure);
  REQUIRE_EQ(eviction_count, 0);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  rejecting_setter_armed = false;

  REQUIRE_EQ(ccache_set(cache, 4, 40), ccol_success);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);

  REQUIRE_EQ(ccache_set(cache, 5, 50), ccol_success);
  REQUIRE_EQ(eviction_count, 2);
  REQUIRE_EQ(eviction_key_captured, 2);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 2, &out), ccol_key_not_found);
  REQUIRE_EQ(ccache_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 30);
  REQUIRE_EQ(ccache_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 40);
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(out, 50);

  ccache_destroy(cache);
}

TEST(eviction, capacity_one_always_evicts) {
  eviction_count = 0;
  eviction_key_captured = -1;
  eviction_val_captured = -1;

  ccache_construct_scoped(cache, int, int, 1, NULL, NULL, record_eviction);

  ccache_set(cache, 10, 100);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(eviction_count, 0);

  ccache_set(cache, 20, 200);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 10);
  REQUIRE_EQ(eviction_val_captured, 100);

  ccache_set(cache, 30, 300);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(eviction_count, 2);
  REQUIRE_EQ(eviction_key_captured, 20);
  REQUIRE_EQ(eviction_val_captured, 200);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 10, &out), ccol_key_not_found);
  REQUIRE_EQ(ccache_get(cache, 20, &out), ccol_key_not_found);
  REQUIRE_EQ(ccache_get(cache, 30, &out), ccol_success);
  REQUIRE_EQ(out, 300);

  ccache_destroy(cache);
}

TEST(eviction, callback_receives_correct_string_key_and_value) {
  str_eviction_count = 0;
  str_eviction_key[0] = '\0';
  str_eviction_val[0] = '\0';

  ccache_construct_scoped(cache, char *, char *, 2, NULL, NULL,
                          record_str_eviction);

  ccache_set(cache, "key1", "val1");
  ccache_set(cache, "key2", "val2");
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);
  REQUIRE_EQ(str_eviction_count, 0);

  ccache_set(cache, "key3", "val3");
  REQUIRE_EQ(str_eviction_count, 1);
  REQUIRE_STREQ(str_eviction_key, "key1");
  REQUIRE_STREQ(str_eviction_val, "val1");
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);

  char *out = NULL;
  REQUIRE_EQ(ccache_get(cache, "key1", &out), ccol_key_not_found);
  REQUIRE_EQ(ccache_get(cache, "key2", &out), ccol_success);
  REQUIRE_STREQ(out, "val2");
  free(out);

  out = NULL;
  REQUIRE_EQ(ccache_get(cache, "key3", &out), ccol_success);
  REQUIRE_STREQ(out, "val3");
  free(out);

  ccache_destroy(cache);
}

TEST(eviction, lru_full_order_after_multiple_gets) {
  eviction_key_captured = -1;
  eviction_count = 0;

  ccache_construct_scoped(cache, int, int, 5, NULL, NULL, record_eviction);

  for (int i = 0; i < 5; i++) ccache_set(cache, i, i * 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)5);

  int out = 0;
  ccache_get(cache, 0, &out);
  ccache_get(cache, 1, &out);

  ccache_set(cache, 5, 50);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 2);
  REQUIRE_EQ(clrucache_size(cache), (size_t)5);

  ccache_set(cache, 6, 60);
  REQUIRE_EQ(eviction_count, 2);
  REQUIRE_EQ(eviction_key_captured, 3);
  REQUIRE_EQ(clrucache_size(cache), (size_t)5);

  REQUIRE_EQ(ccache_get(cache, 2, &out), ccol_key_not_found);
  REQUIRE_EQ(ccache_get(cache, 3, &out), ccol_key_not_found);

  REQUIRE_EQ(ccache_get(cache, 0, &out), ccol_success);
  REQUIRE_EQ(out, 0);
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 10);
  REQUIRE_EQ(ccache_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 40);
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(out, 50);
  REQUIRE_EQ(ccache_get(cache, 6, &out), ccol_success);
  REQUIRE_EQ(out, 60);

  ccache_destroy(cache);
}

// What record_getter_eviction saw, and the calls of double_key_getter.
static int getter_eviction_key_seen = -1;
static int getter_eviction_val_seen = -1;
static int getter_eviction_count = 0;
static int double_key_getter_calls = 0;

// A remote getter that gives key * 2, for the eviction test of a fetched
// entry.
static bool double_key_getter(const cmap_pair *key, cmap_pair *val) {
  double_key_getter_calls++;
  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k * 2;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// An eviction callback that records the evicted int key and value of a fetched
// entry.
static void record_getter_eviction(const cmap_pair *key, const cmap_pair *val) {
  getter_eviction_key_seen = *(const int *)key->ptr;
  getter_eviction_val_seen = *(const int *)val->ptr;
  getter_eviction_count++;
}

// The callback must get exactly the value the remote getter produced; with
// capacity 3, inserting key 4 evicts exactly key 1.
TEST(eviction, eviction_callback_fires_for_getter_populated_entry) {
  getter_eviction_key_seen = -1;
  getter_eviction_val_seen = -1;
  getter_eviction_count = 0;
  double_key_getter_calls = 0;

  ccache_construct_scoped(cache, int, int, 3, double_key_getter, NULL,
                          record_getter_eviction);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2);
  REQUIRE_EQ(ccache_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 4);
  REQUIRE_EQ(ccache_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 6);
  REQUIRE_EQ(double_key_getter_calls, 3);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(getter_eviction_count, 0);

  REQUIRE_EQ(ccache_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(double_key_getter_calls, 3);

  REQUIRE_EQ(ccache_set(cache, 4, 99), ccol_success);
  REQUIRE_EQ(getter_eviction_count, 1);
  REQUIRE_EQ(getter_eviction_key_seen, 1);
  REQUIRE_EQ(getter_eviction_val_seen, 2);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  REQUIRE_EQ(ccache_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 4);
  REQUIRE_EQ(ccache_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 6);
  REQUIRE_EQ(ccache_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 99);
  REQUIRE_EQ(double_key_getter_calls, 3);

  ccache_destroy(cache);
}

// The calls of simple_remote_getter.
static int remote_get_call_count = 0;

// A remote getter that gives key * 2 and counts its calls, so a test sees when
// the cache fetches.
static bool simple_remote_getter(const cmap_pair *key, cmap_pair *val) {
  remote_get_call_count++;
  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k * 2;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// A remote getter that always fails, for the miss paths.
static bool failing_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  return false;
}

// retry_remote_getter fails on its first call and gives the key on later ones,
// so a test sees that a failed fetch is tried again.
static int retry_getter_call_count = 0;
// Fails once, then gives the key itself as the value.
static bool retry_remote_getter(const cmap_pair *key, cmap_pair *val) {
  retry_getter_call_count++;
  if (retry_getter_call_count == 1) return false;
  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

TEST(remote_getter, fetches_on_cache_miss) {
  remote_get_call_count = 0;
  ccache_construct_scoped(cache, int, int, 10, simple_remote_getter, NULL,
                          NULL);

  int out = 0;
  ccol_retval r = ccache_get(cache, 7, &out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(out, 14);
  REQUIRE_EQ(remote_get_call_count, 1);

  ccache_get(cache, 7, &out);
  REQUIRE_EQ(remote_get_call_count, 1);

  ccache_destroy(cache);
}

TEST(remote_getter, returns_not_found_on_getter_failure) {
  ccache_construct_scoped(cache, int, int, 10, failing_remote_getter, NULL,
                          NULL);

  int out = 0;
  ccol_retval r = ccache_get(cache, 5, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  ccache_destroy(cache);
}

TEST(remote_getter, val_out_populated) {
  remote_get_call_count = 0;
  ccache_construct_scoped(cache, int, int, 10, simple_remote_getter, NULL,
                          NULL);

  int k = 5;
  cmap_pair kp = {};
  ccol__populate_cmap_pair(&kp, k);

  cmap_pair val_out = {};
  ccol_retval r = clrucache_get_full(cache, &kp, &val_out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(*(int *)val_out.ptr, 10);
  REQUIRE_EQ(val_out.size, sizeof(int));
  free(val_out.ptr);

  ccache_destroy(cache);
}

TEST(remote_getter, retry_after_failure_triggers_new_fetch) {
  retry_getter_call_count = 0;
  ccache_construct_scoped(cache, int, int, 10, retry_remote_getter, NULL, NULL);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(retry_getter_call_count, 1);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(retry_getter_call_count, 2);
  REQUIRE_EQ(out, 5);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(retry_getter_call_count, 2);

  ccache_destroy(cache);
}

TEST(remote_getter, re_fetches_after_eviction) {
  remote_get_call_count = 0;
  ccache_construct_scoped(cache, int, int, 1, simple_remote_getter, NULL, NULL);

  int out = 0;
  ccache_get(cache, 10, &out);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(remote_get_call_count, 1);

  ccache_get(cache, 20, &out);
  REQUIRE_EQ(out, 40);
  REQUIRE_EQ(remote_get_call_count, 2);

  ccache_get(cache, 10, &out);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(remote_get_call_count, 3);

  ccache_destroy(cache);
}

// A getter that reports success with a NULL value, which the cache must treat
// as a miss.
static bool null_ptr_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  val->ptr = NULL;
  val->size = sizeof(int);
  return true;
}

// A getter that reports success with a value of size 0, which the cache must
// treat as a miss and free.
static bool zero_size_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  val->ptr = malloc(sizeof(int));
  val->size = 0;
  return true;
}

// A getter that allocates a value and then reports failure, so a test sees the
// cache free it.
static bool false_getter_with_alloc(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  val->ptr = malloc(sizeof(int));
  val->size = sizeof(int);
  return false;
}

TEST(remote_getter, getter_returns_true_with_null_ptr_treated_as_miss) {
  ccache_construct_scoped(cache, int, int, 10, null_ptr_remote_getter, NULL,
                          NULL);
  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  ccache_destroy(cache);
}

TEST(remote_getter, getter_returns_true_with_zero_size_treated_as_miss) {
  ccache_construct_scoped(cache, int, int, 10, zero_size_remote_getter, NULL,
                          NULL);
  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  ccache_destroy(cache);
}

TEST(remote_getter, getter_returns_false_with_allocated_ptr_no_leak) {
  ccache_construct_scoped(cache, int, int, 10, false_getter_with_alloc, NULL,
                          NULL);
  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  ccache_destroy(cache);
}

// A getter that gives a value larger than an int, for the size checks of the
// typed get.
static bool oversized_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  void *buf = malloc(sizeof(long long) * 2);
  if (!buf) return false;
  val->ptr = buf;
  val->size = sizeof(long long) * 2;
  return true;
}

TEST(remote_getter, char_ptr_returns_not_found_on_getter_failure) {
  ccache_construct_scoped(cache, int, char *, 10, failing_remote_getter, NULL,
                          NULL);

  char sentinel[] = "unchanged";
  char *out = sentinel;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_PTR_EQ(out, sentinel);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  ccache_destroy(cache);
}

// The char * value versions of the three tests above: A char * cache gets
// through clrucache_get_full, a separate code path.
TEST(remote_getter,
     char_ptr_getter_returns_true_with_null_ptr_treated_as_miss) {
  ccache_construct_scoped(cache, int, char *, 10, null_ptr_remote_getter, NULL,
                          NULL);

  char *out = NULL;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_NULL(out);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  ccache_destroy(cache);
}

TEST(remote_getter,
     char_ptr_getter_returns_true_with_zero_size_treated_as_miss) {
  ccache_construct_scoped(cache, int, char *, 10, zero_size_remote_getter, NULL,
                          NULL);

  char *out = NULL;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_NULL(out);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  ccache_destroy(cache);
}

TEST(remote_getter, char_ptr_getter_returns_false_with_allocated_ptr_no_leak) {
  ccache_construct_scoped(cache, int, char *, 10, false_getter_with_alloc, NULL,
                          NULL);

  char *out = NULL;
  REQUIRE_EQ(ccache_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_NULL(out);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  ccache_destroy(cache);
}

TEST(remote_getter, getter_returns_oversized_value_no_crash) {
  ccache_construct_scoped(cache, int, int, 10, oversized_getter, NULL, NULL);
  int out = 0;
  ccol_retval r = ccache_get(cache, 5, &out);
  REQUIRE_EQ(r, ccol_unexpected_failure);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  ccache_destroy(cache);
}

// A getter that gives a one-byte value, which the typed get must refuse
// without copying part of it.
static bool undersized_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  void *buf = malloc(1);
  if (!buf) return false;
  memset(buf, 0x7A, 1);
  val->ptr = buf;
  val->size = 1;
  return true;
}

TEST(remote_getter,
     getter_returns_undersized_value_rejected_not_partially_copied) {
  ccache_construct_scoped(cache, int, int, 10, undersized_getter, NULL, NULL);
  int out = 0x11223344;
  ccol_retval r = ccache_get(cache, 5, &out);
  REQUIRE_EQ(r, ccol_unexpected_failure);
  REQUIRE_EQ(out, 0x11223344);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  ccache_destroy(cache);
}

// The calls of the string key getters.
static int char_key_getter_call_count = 0;

// A remote getter for a string key that gives the key's length, so a test sees
// one fetch per distinct key.
static bool strlen_value_getter(const cmap_pair *key, cmap_pair *val) {
  char_key_getter_call_count++;
  const char *s = (const char *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = (int)strlen(s);
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// A remote getter for string keys that always fails.
static bool failing_char_key_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  return false;
}

TEST(remote_getter, char_ptr_key_fetches_and_caches_via_getter) {
  char_key_getter_call_count = 0;
  ccache_construct_scoped(cache, char *, int, 8, strlen_value_getter, NULL,
                          NULL);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, "hello", &out), ccol_success);
  REQUIRE_EQ(out, 5);
  REQUIRE_EQ(char_key_getter_call_count, 1);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  out = 0;
  REQUIRE_EQ(ccache_get(cache, "hello", &out), ccol_success);
  REQUIRE_EQ(out, 5);
  REQUIRE_EQ(char_key_getter_call_count, 1);

  out = 0;
  REQUIRE_EQ(ccache_get(cache, "world!", &out), ccol_success);
  REQUIRE_EQ(out, 6);
  REQUIRE_EQ(char_key_getter_call_count, 2);
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);

  ccache_destroy(cache);
}

TEST(remote_getter, char_ptr_key_failing_getter_returns_not_found) {
  ccache_construct_scoped(cache, char *, int, 8, failing_char_key_getter, NULL,
                          NULL);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, "anything", &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  ccache_destroy(cache);
}

// What recording_remote_setter returns, and what it saw: Its calls and the last
// key and value.
static bool remote_set_should_succeed = true;
static int remote_set_call_count = 0;
static int remote_set_last_key = -1;
static int remote_set_last_val = -1;

// A remote setter for int caches that records its call and gives
// remote_set_should_succeed.
static bool recording_remote_setter(const cmap_pair *key,
                                    const cmap_pair *val) {
  remote_set_call_count++;
  remote_set_last_key = *(const int *)key->ptr;
  remote_set_last_val = *(const int *)val->ptr;
  return remote_set_should_succeed;
}

TEST(sync_setter, success_updates_cache) {
  remote_set_should_succeed = true;
  remote_set_call_count = 0;

  ccache_construct_scoped(cache, int, int, 10, NULL, recording_remote_setter,
                          NULL);

  int k = 1, v = 42;
  ccol_retval r = ccache_set(cache, k, v);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(remote_set_call_count, 1);
  REQUIRE_EQ(remote_set_last_key, 1);
  REQUIRE_EQ(remote_set_last_val, 42);

  int out = 0;
  ccache_get(cache, k, &out);
  REQUIRE_EQ(out, 42);

  ccache_destroy(cache);
}

TEST(sync_setter, failure_does_not_update_cache) {
  remote_set_should_succeed = false;
  remote_set_call_count = 0;

  ccache_construct_scoped(cache, int, int, 10, NULL, recording_remote_setter,
                          NULL);

  int k = 1, v = 99;
  ccol_retval r = ccache_set(cache, k, v);
  REQUIRE_EQ(r, ccol_unexpected_failure);

  int out = 0;
  r = ccache_get(cache, k, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  ccache_destroy(cache);
}

TEST(sync_setter, successive_overwrites_with_remote_setter) {
  remote_set_should_succeed = true;
  remote_set_call_count = 0;

  ccache_construct_scoped(cache, int, int, 10, NULL, recording_remote_setter,
                          NULL);

  int k = 3;
  ccache_set(cache, k, 10);
  REQUIRE_EQ(remote_set_call_count, 1);
  REQUIRE_EQ(remote_set_last_val, 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  ccache_set(cache, k, 20);
  REQUIRE_EQ(remote_set_call_count, 2);
  REQUIRE_EQ(remote_set_last_val, 20);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, k, &out), ccol_success);
  REQUIRE_EQ(out, 20);

  ccache_destroy(cache);
}

TEST(sync_setter, failure_on_new_key_followed_by_success) {
  remote_set_should_succeed = false;
  remote_set_call_count = 0;

  ccache_construct_scoped(cache, int, int, 10, NULL, recording_remote_setter,
                          NULL);

  int k = 42, v1 = 11;
  REQUIRE_EQ(ccache_set(cache, k, v1), ccol_unexpected_failure);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  int out = 0;
  REQUIRE_EQ(ccache_get(cache, k, &out), ccol_key_not_found);

  remote_set_should_succeed = true;
  int v2 = 22;
  REQUIRE_EQ(ccache_set(cache, k, v2), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(ccache_get(cache, k, &out), ccol_success);
  REQUIRE_EQ(out, v2);

  ccache_destroy(cache);
}

TEST(sync_setter, failure_preserves_existing_value) {
  remote_set_should_succeed = true;
  ccache_construct_scoped(cache, int, int, 10, NULL, recording_remote_setter,
                          NULL);

  int k = 5, v1 = 10;
  ccache_set(cache, k, v1);

  remote_set_should_succeed = false;
  int v2 = 20;
  ccol_retval r = ccache_set(cache, k, v2);
  REQUIRE_EQ(r, ccol_unexpected_failure);

  int out = 0;
  ccache_get(cache, k, &out);
  REQUIRE_EQ(out, 10);

  ccache_destroy(cache);
}

// What recording_char_key_setter returns, and what it saw.
static bool char_key_setter_should_succeed = true;
static int char_key_setter_call_count = 0;
static char char_key_setter_last_key[64];
static int char_key_setter_last_val = -1;

// A remote setter for string keys that records the key and value and gives
// char_key_setter_should_succeed.
static bool recording_char_key_setter(const cmap_pair *key,
                                      const cmap_pair *val) {
  char_key_setter_call_count++;
  strncpy(char_key_setter_last_key, (const char *)key->ptr,
          sizeof(char_key_setter_last_key) - 1);
  char_key_setter_last_val = *(const int *)val->ptr;
  return char_key_setter_should_succeed;
}

TEST(sync_setter, char_ptr_key_setter_success) {
  char_key_setter_should_succeed = true;
  char_key_setter_call_count = 0;
  char_key_setter_last_key[0] = '\0';
  char_key_setter_last_val = -1;

  ccache_construct_scoped(cache, char *, int, 8, NULL,
                          recording_char_key_setter, NULL);

  REQUIRE_EQ(ccache_set(cache, "mykey", 77), ccol_success);
  REQUIRE_EQ(char_key_setter_call_count, 1);
  REQUIRE_STREQ(char_key_setter_last_key, "mykey");
  REQUIRE_EQ(char_key_setter_last_val, 77);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, "mykey", &out), ccol_success);
  REQUIRE_EQ(out, 77);

  ccache_destroy(cache);
}

TEST(sync_setter, char_ptr_key_setter_failure_leaves_cache_empty) {
  char_key_setter_should_succeed = false;
  char_key_setter_call_count = 0;

  ccache_construct_scoped(cache, char *, int, 8, NULL,
                          recording_char_key_setter, NULL);

  REQUIRE_EQ(ccache_set(cache, "mykey", 88), ccol_unexpected_failure);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, "mykey", &out), ccol_key_not_found);

  ccache_destroy(cache);
}

// A counted start gate, so several threads reach ccache_get at the same moment.
// A pthread barrier fixes its count at creation and parks the started threads
// for good when a pthread_create fails; this gate opens for the threads that
// really started.
typedef struct {
  _Atomic int ready;
  _Atomic bool go;
} ccache_start_gate_t;

// Sleeps 1 us, the step of the gate's waits.
static void ccache_gate_park(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
  nanosleep(&ts, NULL);
}

// Counts the calling worker in and waits for the gate to open; a worker calls
// it right before the operation under test. A NULL gate is a no-op.
static void ccache_gate_arrive(ccache_start_gate_t *g) {
  if (!g) return;
  atomic_fetch_add_explicit(&g->ready, 1, memory_order_release);
  while (!atomic_load_explicit(&g->go, memory_order_acquire))
    ccache_gate_park();
}

// Waits until started workers have arrived and opens the gate. The creating
// thread must call it on every path, or the workers park for good.
static void ccache_gate_release(ccache_start_gate_t *g, int started) {
  while (atomic_load_explicit(&g->ready, memory_order_acquire) < started)
    ccache_gate_park();
  atomic_store_explicit(&g->go, true, memory_order_release);
}

// One getter thread of the coalescing tests, with its result.
typedef struct {
  ccache cache;
  int key;
  int result;
  ccol_retval retval;
  ccache_start_gate_t *start_gate;
} getter_arg_t;

// The calls of slow_remote_getter.
static _Atomic int coalesce_getter_calls = 0;

// A remote getter that sleeps 100 ms and gives key + 1000, so concurrent
// getters of the key have time to join its fetch.
static bool slow_remote_getter(const cmap_pair *key, cmap_pair *val) {
  atomic_fetch_add(&coalesce_getter_calls, 1);

  usleep(100000);

  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k + 1000;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// Waits at the start gate and gets the key.
static void *getter_thread(void *arg) {
  getter_arg_t *ga = (getter_arg_t *)arg;
  ccache cache = ga->cache;
  ccache_redeclare(cache, int, int);
  ccache_gate_arrive(ga->start_gate);
  ga->retval = ccache_get(cache, ga->key, &ga->result);
  return NULL;
}

TEST(concurrency, multiple_getters_coalesce_to_single_remote_fetch) {
  coalesce_getter_calls = 0;
  ccache_construct(cache, int, int, 16, slow_remote_getter, NULL, NULL);

#define N_THREADS 8
  ccache_start_gate_t gate = {0};
  getter_arg_t args[N_THREADS];
  pthread_t tids[N_THREADS];
  int created = 0;
  for (int i = 0; i < N_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 42;
    args[i].result = 0;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_gate = &gate;
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  ccache_gate_release(&gate, created);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }

  int calls = coalesce_getter_calls;
  ccol_retval retvals[N_THREADS] = {0};
  int results[N_THREADS] = {0};
  for (int i = 0; i < created; i++) {
    retvals[i] = args[i].retval;
    results[i] = args[i].result;
  }
  ccache_destroy(cache);

  REQUIRE_EQ(created, (int)N_THREADS);
  REQUIRE_EQ(calls, 1);

  for (int i = 0; i < N_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_success);
    REQUIRE_EQ(results[i], 1042);
  }
#undef N_THREADS
}

// The bound of every wait on another thread, in milliseconds. It's a safety net
// against a hang, never the property under test.
#define CCACHE_GATE_WAIT_MS 30000

// Waits up to CCACHE_GATE_WAIT_MS for *flag and tells whether it was set. It
// sleeps instead of yielding, since valgrind runs one thread at a time.
static bool ccache_await_flag(_Atomic bool *flag) {
  for (int waited_ms = 0; waited_ms < CCACHE_GATE_WAIT_MS; waited_ms++) {
    if (atomic_load(flag)) return true;
    usleep(1000);
  }
  return atomic_load(flag);
}

// Waits up to CCACHE_GATE_WAIT_MS until the entry of key holds want references:
// the thread in the remote call holds one and each waiter one more.
static bool failset_await_waiters(ccache cache, int key, int want) {
  cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
  for (int waited_ms = 0; waited_ms < CCACHE_GATE_WAIT_MS; waited_ms++) {
    if (clrucache__key_waiters_for_tests(cache, &kp) == want) return true;
    usleep(1000);
  }
  return clrucache__key_waiters_for_tests(cache, &kp) == want;
}

// gating_remote_setter: It records the key and value, sets setter_started,
// waits for setter_may_finish and fails when setter_should_fail is set.
static _Atomic bool setter_started = false;
static _Atomic bool setter_may_finish = false;
static volatile bool setter_should_fail = false;
static volatile int setter_key_seen = -1;
static volatile int setter_val_seen = -1;

// A remote setter that holds until the test lets it finish, so getters can be
// made to wait on a set in flight.
static bool gating_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  setter_key_seen = *(const int *)key->ptr;
  setter_val_seen = *(const int *)val->ptr;
  atomic_store(&setter_started, true);
  (void)ccache_await_flag(&setter_may_finish);
  return !setter_should_fail;
}

// A getter thread that blocks on a set in flight, with its result and a done
// flag.
typedef struct {
  ccache cache;
  int key;
  int expect_val;
  int result;
  ccol_retval retval;
  _Atomic bool done;
} waiter_arg_t;

// Waits until the n getters of w have joined the remote call on key (the entry
// holds 1 + n references) or one of them has returned, so a later check of
// done sees a getter that really blocked.
static void ccache_await_blocked_getters(ccache cache, int key, waiter_arg_t *w,
                                         int n) {
  cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
  for (int waited_ms = 0; waited_ms < CCACHE_GATE_WAIT_MS; waited_ms++) {
    if (clrucache__key_waiters_for_tests(cache, &kp) == 1 + n) return;
    for (int i = 0; i < n; i++)
      if (atomic_load(&w[i].done)) return;
    usleep(1000);
  }
}

// Gets the key and sets done.
static void *waiter_thread(void *arg) {
  waiter_arg_t *wa = (waiter_arg_t *)arg;
  ccache cache = wa->cache;
  ccache_redeclare(cache, int, int);
  wa->retval = ccache_get(cache, wa->key, &wa->result);
  atomic_store(&wa->done, true);
  return NULL;
}

// A setter thread for the tests where getters wait on a set.
typedef struct {
  ccache cache;
  int key;
  int val;
} sync_setter_arg_t;

// Sets key to val.
static void *sync_setter_thread(void *arg) {
  sync_setter_arg_t *sa = (sync_setter_arg_t *)arg;
  ccache cache = sa->cache;
  ccache_redeclare(cache, int, int);
  int k = sa->key, v = sa->val;
  ccache_set(cache, k, v);
  return NULL;
}

TEST(concurrency, getters_wait_for_sync_setter) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  ccache_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg = {cache, 10, 99};
  pthread_t stid;
  bool stid_started =
      (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);

  if (stid_started) (void)ccache_await_flag(&setter_started);

  waiter_arg_t warg = {cache, 10, 99, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      stid_started && (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  int done_while_blocked = -1;
  if (waiter_started) {
    ccache_await_blocked_getters(cache, 10, &warg, 1);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  atomic_store(&setter_may_finish, true);
  if (stid_started) pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  ccache_destroy(cache);

  REQUIRE_TRUE(stid_started);
  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 99);
}

TEST(concurrency, getter_blocked_by_setter_for_existing_key_succeeds) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  ccache_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg1 = {cache, 55, 550};
  pthread_t stid1;
  bool stid1_started =
      (pthread_create(&stid1, NULL, sync_setter_thread, &sarg1) == 0);
  if (stid1_started) {
    (void)ccache_await_flag(&setter_started);
    atomic_store(&setter_may_finish, true);
    pthread_join(stid1, NULL);
  }
  size_t size_seeded = clrucache_size(cache);

  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);

  sync_setter_arg_t sarg2 = {cache, 55, 999};
  pthread_t stid2;
  bool stid2_started =
      stid1_started &&
      (pthread_create(&stid2, NULL, sync_setter_thread, &sarg2) == 0);
  if (stid2_started) (void)ccache_await_flag(&setter_started);

  waiter_arg_t warg = {cache, 55, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      stid2_started && (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  int done_while_blocked = -1;
  if (waiter_started) {
    ccache_await_blocked_getters(cache, 55, &warg, 1);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  atomic_store(&setter_may_finish, true);
  if (stid2_started) pthread_join(stid2, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  size_t size_after = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_TRUE(stid1_started);
  REQUIRE_EQ(size_seeded, (size_t)1);
  REQUIRE_TRUE(stid2_started);
  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 999);
  REQUIRE_EQ(size_after, (size_t)1);
}

TEST(concurrency, getter_sees_key_not_found_when_new_key_setter_fails) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  ccache_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg = {cache, 77, 777};
  pthread_t stid;
  bool stid_started =
      (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);

  if (stid_started) (void)ccache_await_flag(&setter_started);

  waiter_arg_t warg = {cache, 77, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      stid_started && (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  int done_while_blocked = -1;
  if (waiter_started) {
    ccache_await_blocked_getters(cache, 77, &warg, 1);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  atomic_store(&setter_may_finish, true);
  if (stid_started) pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  size_t size_after = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_TRUE(stid_started);
  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  REQUIRE_EQ(warg.retval, ccol_key_not_found);
  REQUIRE_EQ(size_after, (size_t)0);
}

TEST(concurrency, getter_gets_old_value_when_setter_fails_for_existing_key) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  ccache_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg1 = {cache, 33, 330};
  pthread_t stid1;
  bool stid1_started =
      (pthread_create(&stid1, NULL, sync_setter_thread, &sarg1) == 0);
  if (stid1_started) {
    (void)ccache_await_flag(&setter_started);
    atomic_store(&setter_may_finish, true);
    pthread_join(stid1, NULL);
  }
  size_t size_seeded = clrucache_size(cache);

  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  sync_setter_arg_t sarg2 = {cache, 33, 999};
  pthread_t stid2;
  bool stid2_started =
      stid1_started &&
      (pthread_create(&stid2, NULL, sync_setter_thread, &sarg2) == 0);
  if (stid2_started) (void)ccache_await_flag(&setter_started);

  waiter_arg_t warg = {cache, 33, 330, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      stid2_started && (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  int done_while_blocked = -1;
  if (waiter_started) {
    ccache_await_blocked_getters(cache, 33, &warg, 1);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  atomic_store(&setter_may_finish, true);
  if (stid2_started) pthread_join(stid2, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  size_t size_after = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_TRUE(stid1_started);
  REQUIRE_EQ(size_seeded, (size_t)1);
  REQUIRE_TRUE(stid2_started);
  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 330);
  REQUIRE_EQ(size_after, (size_t)1);
}

TEST(concurrency, multiple_getters_see_old_value_when_setter_fails) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  ccache_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg1 = {cache, 42, 420};
  pthread_t stid1;
  bool stid1_started =
      (pthread_create(&stid1, NULL, sync_setter_thread, &sarg1) == 0);
  if (stid1_started) {
    (void)ccache_await_flag(&setter_started);
    atomic_store(&setter_may_finish, true);
    pthread_join(stid1, NULL);
  }
  size_t size_seeded = clrucache_size(cache);

  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  sync_setter_arg_t sarg2 = {cache, 42, 999};
  pthread_t stid2;
  bool stid2_started =
      stid1_started &&
      (pthread_create(&stid2, NULL, sync_setter_thread, &sarg2) == 0);
  if (stid2_started) (void)ccache_await_flag(&setter_started);

#define N_OLD_VAL_WAITERS 4
  waiter_arg_t wargs[N_OLD_VAL_WAITERS];
  pthread_t wtids[N_OLD_VAL_WAITERS];
  int wtids_created = 0;
  for (int i = 0; stid2_started && i < N_OLD_VAL_WAITERS; i++) {
    wargs[i] = (waiter_arg_t){cache, 42, 0, 0, ccol_unexpected_failure, false};
    if (pthread_create(&wtids[i], NULL, waiter_thread, &wargs[i]) != 0) break;
    wtids_created++;
  }
  int still_blocked = 0;
  if (wtids_created > 0) {
    ccache_await_blocked_getters(cache, 42, wargs, wtids_created);
    for (int i = 0; i < wtids_created; i++) {
      if (atomic_load(&wargs[i].done) == 0) still_blocked++;
    }
  }

  atomic_store(&setter_may_finish, true);
  if (stid2_started) pthread_join(stid2, NULL);
  for (int i = 0; i < wtids_created; i++) {
    pthread_join(wtids[i], NULL);
  }

  int got_old_value = 0;
  for (int i = 0; i < wtids_created; i++) {
    if (wargs[i].retval == ccol_success && wargs[i].result == 420)
      got_old_value++;
  }
  size_t size_after = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_TRUE(stid1_started);
  REQUIRE_EQ(size_seeded, (size_t)1);
  REQUIRE_TRUE(stid2_started);
  REQUIRE_EQ(wtids_created, (int)N_OLD_VAL_WAITERS);
  REQUIRE_EQ(still_blocked, (int)N_OLD_VAL_WAITERS);
  REQUIRE_EQ(got_old_value, (int)N_OLD_VAL_WAITERS);
  REQUIRE_EQ(size_after, (size_t)1);
#undef N_OLD_VAL_WAITERS
}

// The values ordering_remote_setter received, in the order it got them.
static volatile int serial_setter_order[8];
static volatile int serial_setter_idx = 0;

// A remote setter that sleeps 5 ms and records its value, so overlapping calls
// would show up in the order.
static bool ordering_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  usleep(5000);
  int v = *(const int *)val->ptr;
  int idx = __atomic_fetch_add(&serial_setter_idx, 1, __ATOMIC_SEQ_CST);
  serial_setter_order[idx] = v;
  return true;
}

// A setter thread for the serialized setter test.
typedef struct {
  ccache cache;
  int key;
  int val;
} simple_setter_arg_t;

// Sets key to val.
static void *simple_setter_thread(void *arg) {
  simple_setter_arg_t *sa = (simple_setter_arg_t *)arg;
  ccache cache = sa->cache;
  ccache_redeclare(cache, int, int);
  ccache_set(cache, sa->key, sa->val);
  return NULL;
}

TEST(concurrency, setters_for_same_key_are_serialized) {
  serial_setter_idx = 0;
  memset((void *)serial_setter_order, 0, sizeof(serial_setter_order));

  ccache_construct(cache, int, int, 16, NULL, ordering_remote_setter, NULL);

#define N_SETTERS 4
  simple_setter_arg_t sargs[N_SETTERS];
  pthread_t stids[N_SETTERS];

  int stids_created = 0;
  for (int i = 0; i < N_SETTERS; i++) {
    sargs[i].cache = cache;
    sargs[i].key = 7;
    sargs[i].val = i + 1;
    if (pthread_create(&stids[i], NULL, simple_setter_thread, &sargs[i]) != 0)
      break;
    stids_created++;
    usleep(500);
  }
  for (int i = 0; i < stids_created; i++) {
    pthread_join(stids[i], NULL);
  }
  int calls = serial_setter_idx;
  size_t final_size = clrucache_size(cache);
  int final_val = 0;
  ccol_retval final_r = ccache_get(cache, 7, &final_val);
  ccache_destroy(cache);

  REQUIRE_EQ(stids_created, (int)N_SETTERS);

  REQUIRE_EQ(calls, N_SETTERS);

  bool seen[N_SETTERS + 1] = {false};
  for (int i = 0; i < N_SETTERS; i++) {
    int v = serial_setter_order[i];
    REQUIRE_GE(v, 1);
    REQUIRE_LE(v, N_SETTERS);
    seen[v] = true;
  }
  for (int i = 1; i <= N_SETTERS; i++) {
    REQUIRE_EQ((int)seen[i], 1);
  }

  REQUIRE_EQ(final_size, (size_t)1);
  REQUIRE_EQ(final_r, ccol_success);
  REQUIRE_EQ(final_val, serial_setter_order[N_SETTERS - 1]);
#undef N_SETTERS
}

TEST(concurrency, concurrent_getters_different_keys) {
  coalesce_getter_calls = 0;
  ccache_construct(cache, int, int, 16, slow_remote_getter, NULL, NULL);

#define N_UNIQUE_KEYS 8
  getter_arg_t args[N_UNIQUE_KEYS];
  pthread_t tids[N_UNIQUE_KEYS];
  int created = 0;
  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    args[i].cache = cache;
    args[i].key = 100 + i;
    args[i].result = 0;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_gate = NULL;
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }

  int calls = coalesce_getter_calls;
  ccol_retval retvals[N_UNIQUE_KEYS] = {0};
  int results[N_UNIQUE_KEYS] = {0};
  for (int i = 0; i < created; i++) {
    retvals[i] = args[i].retval;
    results[i] = args[i].result;
  }
  ccache_destroy(cache);

  REQUIRE_EQ(created, (int)N_UNIQUE_KEYS);
  REQUIRE_EQ(calls, N_UNIQUE_KEYS);

  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    REQUIRE_EQ(retvals[i], ccol_success);
    REQUIRE_EQ(results[i], 100 + i + 1000);
  }
#undef N_UNIQUE_KEYS
}

// value_gating_setter holds only a set of the value 999, so the same cache can
// be seeded with a set that finishes at once.
static _Atomic bool val_gate_started = false;
static _Atomic bool val_gate_open = false;

// Holds a set of 999 until val_gate_open and lets every other set through.
static bool value_gating_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  if (*(const int *)val->ptr == 999) {
    atomic_store(&val_gate_started, true);
    (void)ccache_await_flag(&val_gate_open);
  }
  return true;
}

// With capacity 1, the held set of key 1 leaves the key evictable, so a set of
// key 2 evicts it; the successful set then stores 999 and evicts key 2. The
// getter that waited on the set must get 999.
TEST(concurrency, getter_sees_new_value_after_set_with_concurrent_insertion) {
  atomic_store(&val_gate_started, false);
  atomic_store(&val_gate_open, false);

  ccache_construct(cache, int, int, 1, NULL, value_gating_setter, NULL);

  ccache_set(cache, 1, 100);
  size_t size_seeded = clrucache_size(cache);

  sync_setter_arg_t sarg = {cache, 1, 999};
  pthread_t stid;
  bool stid_started =
      (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);
  if (stid_started) (void)ccache_await_flag(&val_gate_started);

  waiter_arg_t warg = {cache, 1, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      stid_started && (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);
  int done_while_blocked = -1;
  if (waiter_started) {
    ccache_await_blocked_getters(cache, 1, &warg, 1);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  ccache_set(cache, 2, 200);

  atomic_store(&val_gate_open, true);
  if (stid_started) pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  ccache_destroy(cache);

  REQUIRE_EQ(size_seeded, (size_t)1);
  REQUIRE_TRUE(stid_started);
  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 999);
}

// A getter that joined a fetch or set must get its value even when an unrelated
// operation evicts the entry before the getter wakes. gating_remote_getter
// holds the fetch until fetch_gate_open.
static _Atomic bool fetch_gate_started = false;
static _Atomic bool fetch_gate_open = false;

// Holds until the gate opens and gives key + 1000.
static bool gating_remote_getter(const cmap_pair *key, cmap_pair *val) {
  atomic_store(&fetch_gate_started, true);
  (void)ccache_await_flag(&fetch_gate_open);
  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k + 1000;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// A getter thread that goes through clrucache_get_full, with its result.
typedef struct {
  ccache cache;
  int key;
  int result;
  ccol_retval retval;
} full_api_getter_arg_t;

// Gets the key through clrucache_get_full and frees the copy.
static void *full_api_getter_thread(void *arg) {
  full_api_getter_arg_t *ga = (full_api_getter_arg_t *)arg;
  cmap_pair kp = {};
  ccol__populate_cmap_pair(&kp, ga->key);
  cmap_pair val_out = {};
  ga->retval = clrucache_get_full(ga->cache, &kp, &val_out);
  if (ga->retval == ccol_success) {
    ga->result = *(int *)val_out.ptr;
    free(val_out.ptr);
  }
  return NULL;
}

// How many times the eviction race runs. The delay hook only makes the evictor
// likely to win the mutex, so repeating the race turns that into a near
// certain failure against a regression.
#define COALESCE_EVICTION_RACE_ITERATIONS 15

// Waits until a publisher is in its post-publish delay, holding the mutex, so
// the evictor can queue for it. The poll takes no lock.
static bool ccache_await_post_publish_delay(void) {
  for (int waited = 0; waited < CCACHE_GATE_WAIT_MS * 5; waited++) {
    if (ccache_test_post_publish_delay_entered()) return true;
    usleep(200);
  }
  return ccache_test_post_publish_delay_entered();
}

TEST(concurrency,
     coalesced_fetch_waiter_receives_value_despite_racing_eviction) {
  for (int iter = 0; iter < COALESCE_EVICTION_RACE_ITERATIONS; iter++) {
    atomic_store(&fetch_gate_started, false);
    atomic_store(&fetch_gate_open, false);
    ccache_test_set_post_publish_delay_us(100000);

    ccache_construct(cache, int, int, 1, gating_remote_getter, NULL, NULL);

    getter_arg_t farg = {cache, 1, 0, ccol_unexpected_failure, NULL};
    pthread_t ftid;
    bool started_ftid =
        (pthread_create(&ftid, NULL, getter_thread, &farg) == 0);
    (void)ccache_await_flag(&fetch_gate_started);

    full_api_getter_arg_t warg = {cache, 1, 0, ccol_unexpected_failure};
    pthread_t wtid;
    bool started_wtid =
        (pthread_create(&wtid, NULL, full_api_getter_thread, &warg) == 0);
    bool joined_wtid = started_wtid && failset_await_waiters(cache, 1, 2);

    atomic_store(&fetch_gate_open, true);
    bool delay_entered = started_ftid && ccache_await_post_publish_delay();

    ccol_retval set_r = ccache_set(cache, 2, 200);

    if (started_ftid) pthread_join(ftid, NULL);
    if (started_wtid) pthread_join(wtid, NULL);
    ccache_test_set_post_publish_delay_us(0);
    ccache_destroy(cache);

    REQUIRE_TRUE(delay_entered);
    REQUIRE_EQ(set_r, ccol_success);
    REQUIRE_TRUE(started_ftid);
    REQUIRE_TRUE(started_wtid);
    REQUIRE_TRUE(joined_wtid);

    REQUIRE_EQ(farg.retval, ccol_success);
    REQUIRE_EQ(farg.result, 1001);

    REQUIRE_EQ(warg.retval, ccol_success);
    REQUIRE_EQ(warg.result, 1001);
  }
}

TEST(concurrency, coalesced_set_waiter_receives_value_despite_racing_eviction) {
  for (int iter = 0; iter < COALESCE_EVICTION_RACE_ITERATIONS; iter++) {
    atomic_store(&setter_started, false);
    atomic_store(&setter_may_finish, false);
    setter_should_fail = false;
    ccache_test_set_post_publish_delay_us(100000);

    ccache_construct(cache, int, int, 1, NULL, gating_remote_setter, NULL);

    sync_setter_arg_t sarg = {cache, 1, 100};
    pthread_t stid;
    bool started_stid =
        (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);
    (void)ccache_await_flag(&setter_started);

    waiter_arg_t warg = {cache, 1, 100, 0, ccol_unexpected_failure, false};
    pthread_t wtid;
    bool started_wtid =
        (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);
    bool joined_wtid = started_wtid && failset_await_waiters(cache, 1, 2);

    atomic_store(&setter_may_finish, true);
    bool delay_entered = started_stid && ccache_await_post_publish_delay();

    ccol_retval set_r = ccache_set(cache, 2, 200);

    if (started_stid) pthread_join(stid, NULL);
    if (started_wtid) pthread_join(wtid, NULL);
    ccache_test_set_post_publish_delay_us(0);
    ccache_destroy(cache);

    REQUIRE_TRUE(delay_entered);
    REQUIRE_EQ(set_r, ccol_success);
    REQUIRE_TRUE(started_stid);
    REQUIRE_TRUE(started_wtid);
    REQUIRE_TRUE(joined_wtid);

    REQUIRE_EQ(warg.retval, ccol_success);
    REQUIRE_EQ(warg.result, 100);
  }
}
#undef COALESCE_EVICTION_RACE_ITERATIONS

// A set takes effect when its setter returns, so the existing key stays counted
// for the whole window.
TEST(concurrency, size_counts_an_existing_key_while_its_set_is_in_flight) {
  atomic_store(&val_gate_started, false);
  atomic_store(&val_gate_open, false);

  ccache_construct(cache, int, int, 10, NULL, value_gating_setter, NULL);

  ccache_set(cache, 1, 100);
  size_t size_before = clrucache_size(cache);

  sync_setter_arg_t sarg = {cache, 1, 999};
  pthread_t stid;
  bool started = (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);
  bool gated = started && ccache_await_flag(&val_gate_started);
  size_t size_during = clrucache_size(cache);

  atomic_store(&val_gate_open, true);
  if (started) pthread_join(stid, NULL);

  size_t size_after = clrucache_size(cache);
  int out = 0;
  ccol_retval get_r = ccache_get(cache, 1, &out);
  ccache_destroy(cache);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(gated);
  REQUIRE_EQ(size_before, (size_t)1);
  REQUIRE_EQ(size_during, (size_t)1);
  REQUIRE_EQ(size_after, (size_t)1);
  REQUIRE_EQ(get_r, ccol_success);
  REQUIRE_EQ(out, 999);
}

TEST(concurrency, failed_setter_keeps_existing_entry_live) {
  ccache_construct(cache, int, int, 4, NULL, gating_remote_setter, NULL);

  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, true);
  setter_should_fail = false;
  ccol_retval seed_r = ccache_set(cache, 5, 50);

  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;
  sync_setter_arg_t sarg2 = {cache, 5, 999};
  pthread_t stid2;
  bool started =
      (pthread_create(&stid2, NULL, sync_setter_thread, &sarg2) == 0);
  bool gated = started && ccache_await_flag(&setter_started);
  size_t size_during = clrucache_size(cache);

  atomic_store(&setter_may_finish, true);
  if (started) pthread_join(stid2, NULL);
  setter_should_fail = false;

  size_t size_after = clrucache_size(cache);
  int out = 0;
  ccol_retval get_r = ccache_get(cache, 5, &out);
  ccache_destroy(cache);

  REQUIRE_EQ(seed_r, ccol_success);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(gated);
  REQUIRE_EQ(size_during, (size_t)1);
  REQUIRE_EQ(size_after, (size_t)1);
  REQUIRE_EQ(get_r, ccol_success);
  REQUIRE_EQ(out, 50);
}

// A set of an existing key in flight while an insert needs room: The insert
// must evict the real oldest key, even the one being set, and the segment never
// exceeds its capacity. cap_overflow_setter holds and fails a set of 999.
static _Atomic bool cap_overflow_gate_started = false;
static _Atomic bool cap_overflow_gate_open = false;

// Holds a set of 999 until the gate opens and fails it.
static bool cap_overflow_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  if (*(const int *)val->ptr == 999) {
    atomic_store(&cap_overflow_gate_started, true);
    (void)ccache_await_flag(&cap_overflow_gate_open);
    return false;
  }
  return true;
}

// A setter thread that keeps its result.
typedef struct {
  ccache cache;
  int key;
  int val;
  ccol_retval retval;
} checked_setter_arg_t;

// Sets key to val and stores the result.
static void *checked_setter_thread(void *arg) {
  checked_setter_arg_t *sa = (checked_setter_arg_t *)arg;
  ccache cache = sa->cache;
  ccache_redeclare(cache, int, int);
  int k = sa->key, v = sa->val;
  sa->retval = ccache_set(cache, k, v);
  return NULL;
}

TEST(concurrency, failed_set_in_flight_lets_an_insert_evict_the_true_lru) {
  atomic_store(&cap_overflow_gate_started, false);
  atomic_store(&cap_overflow_gate_open, false);
  eviction_count = 0;
  eviction_key_captured = -1;
  eviction_val_captured = -1;

  ccache_construct(cache, int, int, 2, NULL, cap_overflow_setter,
                   record_eviction);

  ccol_retval seed1 = ccache_set(cache, 1, 10);
  ccol_retval seed2 = ccache_set(cache, 2, 20);

  checked_setter_arg_t sarg = {cache, 1, 999, ccol_success};
  pthread_t stid;
  bool started =
      (pthread_create(&stid, NULL, checked_setter_thread, &sarg) == 0);
  bool gated = started && ccache_await_flag(&cap_overflow_gate_started);

  ccol_retval ins3 = ccache_set(cache, 3, 30);
  int evictions_in_window = eviction_count;
  int evicted_key_in_window = eviction_key_captured;
  int evicted_val_in_window = eviction_val_captured;
  size_t size_in_window = clrucache_size(cache);

  waiter_arg_t warg = {cache, 1, 0, 0, ccol_success, false};
  pthread_t wtid;
  bool wstarted = (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  atomic_store(&cap_overflow_gate_open, true);
  if (started) pthread_join(stid, NULL);
  if (wstarted) pthread_join(wtid, NULL);

  int evictions_after = eviction_count;
  size_t size_after = clrucache_size(cache);
  int out1 = 0, out2 = 0, out3 = 0;
  ccol_retval get1 = ccache_get(cache, 1, &out1);
  ccol_retval get2 = ccache_get(cache, 2, &out2);
  ccol_retval get3 = ccache_get(cache, 3, &out3);
  ccache_destroy(cache);

  REQUIRE_EQ(seed1, ccol_success);
  REQUIRE_EQ(seed2, ccol_success);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(gated);
  REQUIRE_TRUE(wstarted);
  REQUIRE_EQ(ins3, ccol_success);
  REQUIRE_EQ(evictions_in_window, 1);
  REQUIRE_EQ(evicted_key_in_window, 1);
  REQUIRE_EQ(evicted_val_in_window, 10);
  REQUIRE_EQ(size_in_window, (size_t)2);

  REQUIRE_EQ(sarg.retval, ccol_unexpected_failure);
  REQUIRE_EQ(warg.retval, ccol_key_not_found);
  REQUIRE_EQ(evictions_after, 1);
  REQUIRE_EQ(size_after, (size_t)2);
  REQUIRE_EQ(get1, ccol_key_not_found);
  REQUIRE_EQ(get2, ccol_success);
  REQUIRE_EQ(out2, 20);
  REQUIRE_EQ(get3, ccol_success);
  REQUIRE_EQ(out3, 30);
}

TEST(concurrency,
     successful_set_in_flight_reinserts_a_key_that_an_insert_evicted) {
  atomic_store(&val_gate_started, false);
  atomic_store(&val_gate_open, false);
  eviction_count = 0;
  eviction_key_captured = -1;
  eviction_val_captured = -1;

  ccache_construct(cache, int, int, 2, NULL, value_gating_setter,
                   record_eviction);

  ccol_retval seed1 = ccache_set(cache, 1, 10);
  ccol_retval seed2 = ccache_set(cache, 2, 20);

  checked_setter_arg_t sarg = {cache, 1, 999, ccol_unexpected_failure};
  pthread_t stid;
  bool started =
      (pthread_create(&stid, NULL, checked_setter_thread, &sarg) == 0);
  bool gated = started && ccache_await_flag(&val_gate_started);

  ccol_retval ins3 = ccache_set(cache, 3, 30);
  int evictions_in_window = eviction_count;
  int evicted_key_in_window = eviction_key_captured;
  int evicted_val_in_window = eviction_val_captured;
  size_t size_in_window = clrucache_size(cache);

  waiter_arg_t warg = {cache, 1, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool wstarted = (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  atomic_store(&val_gate_open, true);
  if (started) pthread_join(stid, NULL);
  if (wstarted) pthread_join(wtid, NULL);

  int evictions_after_set = eviction_count;
  int evicted_key_by_set = eviction_key_captured;
  int evicted_val_by_set = eviction_val_captured;
  size_t size_after = clrucache_size(cache);

  ccol_retval ins4 = ccache_set(cache, 4, 40);
  int evicted_key_by_4 = eviction_key_captured;
  int out1 = 0, out2 = 0, out4 = 0;
  ccol_retval get1 = ccache_get(cache, 1, &out1);
  ccol_retval get2 = ccache_get(cache, 2, &out2);
  ccol_retval get4 = ccache_get(cache, 4, &out4);
  ccache_destroy(cache);

  REQUIRE_EQ(seed1, ccol_success);
  REQUIRE_EQ(seed2, ccol_success);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(gated);
  REQUIRE_TRUE(wstarted);
  REQUIRE_EQ(ins3, ccol_success);
  REQUIRE_EQ(evictions_in_window, 1);
  REQUIRE_EQ(evicted_key_in_window, 1);
  REQUIRE_EQ(evicted_val_in_window, 10);
  REQUIRE_EQ(size_in_window, (size_t)2);

  REQUIRE_EQ(sarg.retval, ccol_success);
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 999);
  REQUIRE_EQ(evictions_after_set, 2);
  REQUIRE_EQ(evicted_key_by_set, 2);
  REQUIRE_EQ(evicted_val_by_set, 20);
  REQUIRE_EQ(size_after, (size_t)2);

  REQUIRE_EQ(ins4, ccol_success);
  REQUIRE_EQ(evicted_key_by_4, 3);
  REQUIRE_EQ(get1, ccol_success);
  REQUIRE_EQ(out1, 999);
  REQUIRE_EQ(get2, ccol_key_not_found);
  REQUIRE_EQ(get4, ccol_success);
  REQUIRE_EQ(out4, 40);
}

// During a fetch the entry is a placeholder outside the LRU list, so the size
// is 0 until the fetch completes.
TEST(concurrency, size_zero_while_fetch_in_progress) {
  atomic_store(&fetch_gate_started, false);
  atomic_store(&fetch_gate_open, false);
  ccache_construct(cache, int, int, 10, gating_remote_getter, NULL, NULL);

  size_t size_before = clrucache_size(cache);

  getter_arg_t arg = {cache, 55, 0, ccol_unexpected_failure, NULL};
  pthread_t tid;
  bool started = (pthread_create(&tid, NULL, getter_thread, &arg) == 0);

  bool entered = started && ccache_await_flag(&fetch_gate_started);
  size_t size_during = clrucache_size(cache);

  atomic_store(&fetch_gate_open, true);
  if (started) pthread_join(tid, NULL);
  size_t size_after = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_EQ(size_before, (size_t)0);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(entered);
  REQUIRE_EQ(size_during, (size_t)0);

  REQUIRE_EQ(arg.retval, ccol_success);
  REQUIRE_EQ(arg.result, 1055);
  REQUIRE_EQ(size_after, (size_t)1);
}

// Getters of one key must join one fetch even when it fails: One call, every
// getter gets ccol_key_not_found. slow_failing_getter holds until the test has
// seen every thread join.
static volatile int fail_getter_call_count = 0;

static _Atomic bool fail_fetch_gate_started = false;
static _Atomic bool fail_fetch_gate_open = false;

// Counts its call, holds until the gate opens and fails.
static bool slow_failing_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  __atomic_fetch_add(&fail_getter_call_count, 1, __ATOMIC_SEQ_CST);
  atomic_store(&fail_fetch_gate_started, true);
  (void)ccache_await_flag(&fail_fetch_gate_open);
  return false;
}

TEST(concurrency, multiple_getters_coalesce_on_failed_fetch) {
  fail_getter_call_count = 0;
  atomic_store(&fail_fetch_gate_started, false);
  atomic_store(&fail_fetch_gate_open, false);
  ccache_construct(cache, int, int, 16, slow_failing_getter, NULL, NULL);

#define N_FAIL_THREADS 6
  ccache_start_gate_t gate = {0};
  getter_arg_t args[N_FAIL_THREADS];
  pthread_t tids[N_FAIL_THREADS];
  int created = 0;
  for (int i = 0; i < N_FAIL_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 11;
    args[i].result = 0;
    args[i].retval = ccol_success;
    args[i].start_gate = &gate;
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  ccache_gate_release(&gate, created);
  bool all_joined = failset_await_waiters(cache, 11, created);
  atomic_store(&fail_fetch_gate_open, true);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }

  int calls = fail_getter_call_count;
  ccol_retval retvals[N_FAIL_THREADS] = {0};
  for (int i = 0; i < created; i++) retvals[i] = args[i].retval;
  size_t final_size = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_EQ(created, (int)N_FAIL_THREADS);
  REQUIRE_TRUE(all_joined);
  REQUIRE_EQ(calls, 1);

  for (int i = 0; i < N_FAIL_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_key_not_found);
  }
  REQUIRE_EQ(final_size, (size_t)0);
#undef N_FAIL_THREADS
}

// A fetch that clrucache__get_into refuses for its size must give the same
// ccol_unexpected_failure to a caller that joined it through
// clrucache_get_full.
static _Atomic bool mismatch_gate_started = false;
static _Atomic bool mismatch_gate_open = false;

// Holds until the gate opens and gives an oversized value.
static bool gating_oversized_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  atomic_store(&mismatch_gate_started, true);
  (void)ccache_await_flag(&mismatch_gate_open);
  void *buf = malloc(sizeof(long long) * 2);
  if (!buf) return false;
  val->ptr = buf;
  val->size = sizeof(long long) * 2;
  return true;
}

TEST(concurrency,
     coalesced_full_api_waiter_sees_unexpected_failure_on_size_mismatch) {
  atomic_store(&mismatch_gate_started, false);
  atomic_store(&mismatch_gate_open, false);

  ccache_construct(cache, int, int, 8, gating_oversized_getter, NULL, NULL);

  getter_arg_t farg = {cache, 1, 0, ccol_success, NULL};
  pthread_t ftid;
  bool started_ftid = (pthread_create(&ftid, NULL, getter_thread, &farg) == 0);
  (void)ccache_await_flag(&mismatch_gate_started);

  full_api_getter_arg_t warg = {cache, 1, 0, ccol_success};
  pthread_t wtid;
  bool started_wtid =
      (pthread_create(&wtid, NULL, full_api_getter_thread, &warg) == 0);
  bool joined_wtid = started_wtid && failset_await_waiters(cache, 1, 2);

  atomic_store(&mismatch_gate_open, true);
  if (started_ftid) pthread_join(ftid, NULL);
  if (started_wtid) pthread_join(wtid, NULL);
  size_t final_size = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_TRUE(started_ftid);
  REQUIRE_TRUE(started_wtid);
  REQUIRE_TRUE(joined_wtid);

  REQUIRE_EQ(farg.retval, ccol_unexpected_failure);

  REQUIRE_EQ(warg.retval, ccol_unexpected_failure);

  REQUIRE_EQ(final_size, (size_t)0);
}

TEST(concurrency, setter_waits_for_active_fetch_then_succeeds) {
  coalesce_getter_calls = 0;
  remote_set_should_succeed = true;
  remote_set_call_count = 0;

  ccache_construct(cache, int, int, 16, slow_remote_getter,
                   recording_remote_setter, NULL);

  getter_arg_t garg = {cache, 7, 0, ccol_unexpected_failure, NULL};
  pthread_t gtid;
  bool started_gtid = (pthread_create(&gtid, NULL, getter_thread, &garg) == 0);

  bool fetch_started = false;
  for (int waited_ms = 0; started_gtid && waited_ms < CCACHE_GATE_WAIT_MS;
       waited_ms++) {
    if (atomic_load(&coalesce_getter_calls) != 0) {
      fetch_started = true;
      break;
    }
    usleep(1000);
  }

  sync_setter_arg_t sarg = {cache, 7, 999};
  pthread_t stid;
  bool started_stid =
      (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);

  if (started_gtid) pthread_join(gtid, NULL);
  if (started_stid) pthread_join(stid, NULL);
  int out = 0;
  ccol_retval final_r = ccache_get(cache, 7, &out);
  ccache_destroy(cache);

  REQUIRE_TRUE(started_gtid);
  REQUIRE_TRUE(fetch_started);
  REQUIRE_TRUE(started_stid);

  REQUIRE_EQ(garg.retval, ccol_success);
  REQUIRE_EQ(garg.result, 1007);

  REQUIRE_EQ(remote_set_call_count, 1);
  REQUIRE_EQ(remote_set_last_key, 7);
  REQUIRE_EQ(remote_set_last_val, 999);

  REQUIRE_EQ(final_r, ccol_success);
  REQUIRE_EQ(out, 999);
}

// Coalescing for char * value caches, which go through clrucache_get_full;
// every thread gets its own copy.
static volatile int char_ptr_coalesce_calls = 0;

// Sleeps 100 ms and gives the string str_<key>.
static bool slow_char_ptr_getter(const cmap_pair *key, cmap_pair *val) {
  __atomic_fetch_add(&char_ptr_coalesce_calls, 1, __ATOMIC_SEQ_CST);
  usleep(100000);
  int k = *(const int *)key->ptr;
  char buf[64];
  snprintf(buf, sizeof(buf), "str_%d", k);
  size_t len = strlen(buf) + 1;
  char *s = (char *)malloc(len);
  if (!s) return false;
  memcpy(s, buf, len);
  val->ptr = s;
  val->size = len;
  return true;
}

// A getter thread of a char * value cache, with its result.
typedef struct {
  ccache cache;
  int key;
  char *result;
  ccol_retval retval;
  ccache_start_gate_t *start_gate;
} str_getter_arg_t;

// Waits at the start gate and gets the key.
static void *str_getter_thread(void *arg) {
  str_getter_arg_t *ga = (str_getter_arg_t *)arg;
  ccache cache = ga->cache;
  ccache_redeclare(cache, int, char *);
  ccache_gate_arrive(ga->start_gate);
  ga->retval = ccache_get(cache, ga->key, &ga->result);
  return NULL;
}

TEST(concurrency, multiple_char_ptr_getters_coalesce) {
  char_ptr_coalesce_calls = 0;
  ccache_construct(cache, int, char *, 16, slow_char_ptr_getter, NULL, NULL);

#define N_STR_THREADS 6
  ccache_start_gate_t gate = {0};
  str_getter_arg_t args[N_STR_THREADS];
  pthread_t tids[N_STR_THREADS];
  int created = 0;
  for (int i = 0; i < N_STR_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 77;
    args[i].result = NULL;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_gate = &gate;
    if (pthread_create(&tids[i], NULL, str_getter_thread, &args[i]) != 0) break;
    created++;
  }
  ccache_gate_release(&gate, created);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }

  int calls = char_ptr_coalesce_calls;
  ccol_retval retvals[N_STR_THREADS] = {0};
  char results_copy[N_STR_THREADS][64] = {{0}};
  bool has_result[N_STR_THREADS] = {false};
  for (int i = 0; i < created; i++) {
    retvals[i] = args[i].retval;
    has_result[i] = args[i].result != NULL;
    if (has_result[i]) {
      snprintf(results_copy[i], sizeof(results_copy[i]), "%s", args[i].result);
      free(args[i].result);
    }
  }
  ccache_destroy(cache);

  REQUIRE_EQ(created, (int)N_STR_THREADS);
  REQUIRE_EQ(calls, 1);
  for (int i = 0; i < N_STR_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_success);
    REQUIRE_TRUE(has_result[i]);
    REQUIRE_STREQ(results_copy[i], "str_77");
  }
#undef N_STR_THREADS
}

// Coalescing on a failed fetch for char * value caches.
static volatile int slow_fail_char_ptr_getter_calls = 0;

// Counts its call, holds until the gate opens and fails.
static bool slow_failing_char_ptr_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  __atomic_fetch_add(&slow_fail_char_ptr_getter_calls, 1, __ATOMIC_SEQ_CST);
  atomic_store(&fail_fetch_gate_started, true);
  (void)ccache_await_flag(&fail_fetch_gate_open);
  return false;
}

TEST(concurrency, multiple_char_ptr_getters_coalesce_on_failed_fetch) {
  slow_fail_char_ptr_getter_calls = 0;
  atomic_store(&fail_fetch_gate_started, false);
  atomic_store(&fail_fetch_gate_open, false);
  ccache_construct(cache, int, char *, 16, slow_failing_char_ptr_getter, NULL,
                   NULL);

#define N_FAIL_STR_THREADS 6
  ccache_start_gate_t gate = {0};
  str_getter_arg_t args[N_FAIL_STR_THREADS];
  pthread_t tids[N_FAIL_STR_THREADS];
  int created = 0;
  for (int i = 0; i < N_FAIL_STR_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 22;
    args[i].result = NULL;
    args[i].retval = ccol_success;
    args[i].start_gate = &gate;
    if (pthread_create(&tids[i], NULL, str_getter_thread, &args[i]) != 0) break;
    created++;
  }
  ccache_gate_release(&gate, created);
  bool all_joined = failset_await_waiters(cache, 22, created);
  atomic_store(&fail_fetch_gate_open, true);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }

  int calls = slow_fail_char_ptr_getter_calls;
  ccol_retval retvals[N_FAIL_STR_THREADS] = {0};
  bool has_result[N_FAIL_STR_THREADS] = {false};
  for (int i = 0; i < created; i++) {
    retvals[i] = args[i].retval;
    has_result[i] = args[i].result != NULL;
    if (has_result[i]) free(args[i].result);
  }
  size_t final_size = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_EQ(created, (int)N_FAIL_STR_THREADS);
  REQUIRE_TRUE(all_joined);
  REQUIRE_EQ(calls, 1);
  for (int i = 0; i < N_FAIL_STR_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_key_not_found);
    REQUIRE_FALSE(has_result[i]);
  }
  REQUIRE_EQ(final_size, (size_t)0);
#undef N_FAIL_STR_THREADS
}

// phased_setter_fn: The first call sleeps 30 ms and fails, the second sleeps 30
// ms and succeeds, later ones succeed at once. The sleeps let the other setters
// reach the wait loop.
static volatile int phased_set_count = 0;

// Fails the first call and lets the others through, as described above.
static bool phased_setter_fn(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  (void)val;
  int call = __atomic_fetch_add(&phased_set_count, 1, __ATOMIC_SEQ_CST);
  if (call == 0) {
    usleep(30000);
    return false;
  }
  if (call == 1) {
    usleep(30000);
    return true;
  }
  return true;
}

// Three setters of a new key; the first one's set fails. The others must find
// each other's placeholder instead of one failing with a false
// ccol_not_enough_memory on an insert of a key that is already there.
TEST(concurrency, multiple_setters_race_after_failed_new_key_set) {
  phased_set_count = 0;

  ccache_construct(cache, int, int, 16, NULL, phased_setter_fn, NULL);

#define N_RACING_SETTERS 3
  simple_setter_arg_t sargs[N_RACING_SETTERS];
  pthread_t stids[N_RACING_SETTERS];
  int created = 0;
  for (int i = 0; i < N_RACING_SETTERS; i++) {
    sargs[i].cache = cache;
    sargs[i].key = 55;
    sargs[i].val = i + 1;
    if (pthread_create(&stids[i], NULL, simple_setter_thread, &sargs[i]) != 0)
      break;
    created++;
    usleep(1000);
  }
  for (int i = 0; i < created; i++) {
    pthread_join(stids[i], NULL);
  }
  int calls = phased_set_count;
  size_t final_size = clrucache_size(cache);
  int out = 0;
  ccol_retval final_r = ccache_get(cache, 55, &out);
  ccache_destroy(cache);

  REQUIRE_EQ(created, (int)N_RACING_SETTERS);

  REQUIRE_EQ(calls, N_RACING_SETTERS);

  REQUIRE_EQ(final_size, (size_t)1);

  REQUIRE_EQ(final_r, ccol_success);
  REQUIRE_GE(out, 1);
  REQUIRE_LE(out, N_RACING_SETTERS);
#undef N_RACING_SETTERS
}

// A small struct, used as a const value type.
struct ccache_const_pt {
  int x, y;
};
TEST(macros, a_const_value_type_reads_back) {
  ccache_construct(ci, int, const int, 4, NULL, NULL, NULL);
  ccache_construct(cs, int, const struct ccache_const_pt, 4, NULL, NULL, NULL);
  int got = 0;
  struct ccache_const_pt pt = {3, 4};
  struct ccache_const_pt pt_got = {0, 0};
  ccol_retval set_i = ccache_set(ci, 1, 7);
  ccol_retval get_i = ccache_get(ci, 1, &got);
  ccol_retval set_s = ccache_set(cs, 2, pt);
  ccol_retval get_s = ccache_get(cs, 2, &pt_got);
  ccache_destroy(ci);
  ccache_destroy(cs);
  REQUIRE_EQ(set_i, ccol_success);
  REQUIRE_EQ(get_i, ccol_success);
  REQUIRE_EQ(got, 7);
  REQUIRE_EQ(set_s, ccol_success);
  REQUIRE_EQ(get_s, ccol_success);
  REQUIRE_EQ(pt_got.x, 3);
  REQUIRE_EQ(pt_got.y, 4);
}

TEST(macros, scoped_construct_auto_destroys) {
  {
    ccache_construct_scoped(cache, int, int, 4, NULL, NULL, NULL);
    int k = 1, v = 2;
    ccache_set(cache, k, v);
    int out = 0;
    ccache_get(cache, k, &out);
    REQUIRE_EQ(out, 2);
  }
  REQUIRE_EQ(1, 1);
}

TEST(macros, declare_and_init) {
  ccache_declare(cache, int, int);
  ccache_init(cache, 8, NULL, NULL, NULL);

  int k = 10, v = 20;
  ccache_set(cache, k, v);

  int out = 0;
  ccol_retval get_rv = ccache_get(cache, k, &out);
  size_t size = clrucache_size(cache);

  ccache_destroy(cache);

  REQUIRE_EQ(get_rv, ccol_success);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(size, (size_t)1);
}

TEST(macros, declare_scoped_auto_destroys) {
  eviction_count = 0;
  {
    ccache_declare_scoped(cache, int, int);
    ccache_init(cache, 4, NULL, NULL, record_eviction);

    ccache_set(cache, 1, 111);
    ccache_set(cache, 2, 222);

    int out = 0;
    REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
    REQUIRE_EQ(out, 111);
    REQUIRE_EQ(clrucache_size(cache), (size_t)2);
  }
  REQUIRE_EQ(eviction_count, 2);
}

// Gets key from a cache handle passed in, after ccache_redeclare, to show that
// the typed macros work across scopes.
static ccol_retval use_cache_in_other_scope(ccache cache, int key, int *out) {
  ccache_redeclare(cache, int, int);
  return ccache_get(cache, key, out);
}

TEST(macros, redeclare_works_across_scope) {
  ccache_construct_scoped(cache, int, int, 8, NULL, NULL, NULL);

  int k = 3, v = 33;
  ccache_set(cache, k, v);

  int result = 0;
  ccol_retval r = use_cache_in_other_scope(cache, k, &result);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(result, 33);

  ccache_destroy(cache);
}

// A struct value type.
typedef struct {
  int x;
  double y;
} point_t;

// A remote getter that gives the string remote_<key>.
static bool str_val_remote_getter(const cmap_pair *key, cmap_pair *val) {
  int k = *(const int *)key->ptr;
  char buf[64];
  snprintf(buf, sizeof(buf), "remote_%d", k);
  size_t len = strlen(buf) + 1;
  char *s = (char *)malloc(len);
  if (!s) return false;
  memcpy(s, buf, len);
  val->ptr = s;
  val->size = len;
  return true;
}

TEST(get_val_types, non_char_ptr_get_miss_does_not_modify_buf) {
  ccache_construct_scoped(cache, int, double, 8, NULL, NULL, NULL);

  double sentinel = 3.14;
  double out = sentinel;
  REQUIRE_EQ(ccache_get(cache, 99, &out), ccol_key_not_found);
  REQUIRE_EQ(out, sentinel);

  ccache_destroy(cache);
}

TEST(get_val_types, double_value_via_macro) {
  ccache_construct_scoped(cache, int, double, 8, NULL, NULL, NULL);

  ccache_set(cache, 1, 2.5);

  double out = 0.0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2.5);

  ccache_destroy(cache);
}

TEST(get_val_types, struct_value_via_macro) {
  ccache_construct_scoped(cache, int, point_t, 8, NULL, NULL, NULL);

  point_t v = {.x = 7, .y = 2.5};
  ccache_set(cache, 42, v);

  point_t out = {};
  REQUIRE_EQ(ccache_get(cache, 42, &out), ccol_success);
  REQUIRE_EQ(out.x, 7);
  REQUIRE_EQ(out.y, 2.5);

  ccache_destroy(cache);
}

TEST(get_val_types, full_api_returns_independent_copies) {
  ccache_construct_scoped(cache, int, int, 8, NULL, NULL, NULL);

  ccache_set(cache, 5, 99);

  int k = 5;
  cmap_pair kp = {};
  ccol__populate_cmap_pair(&kp, k);

  cmap_pair a = {}, b = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &a), ccol_success);
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &b), ccol_success);

  REQUIRE_PTR_NE(a.ptr, b.ptr);
  REQUIRE_EQ(*(int *)a.ptr, 99);
  REQUIRE_EQ(*(int *)b.ptr, 99);
  REQUIRE_EQ(a.size, sizeof(int));
  REQUIRE_EQ(b.size, sizeof(int));
  free(a.ptr);
  free(b.ptr);

  ccache_destroy(cache);
}

TEST(get_val_types, missing_key_val_out_untouched) {
  ccache_construct_scoped(cache, int, int, 8, NULL, NULL, NULL);

  int k = 99;
  cmap_pair kp = {};
  ccol__populate_cmap_pair(&kp, k);

  void *sentinel_ptr = (void *)0x1;
  size_t sentinel_sz = 0xBEEFUL;
  cmap_pair val_out = {.ptr = sentinel_ptr, .size = sentinel_sz};

  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_key_not_found);
  REQUIRE_PTR_EQ(val_out.ptr, sentinel_ptr);
  REQUIRE_EQ(val_out.size, sentinel_sz);

  ccache_destroy(cache);
}

TEST(get_val_types, overwrite_reflected_in_subsequent_get) {
  ccache_construct_scoped(cache, int, double, 8, NULL, NULL, NULL);

  ccache_set(cache, 1, 1.0);
  ccache_set(cache, 1, 2.5);

  double out = 0.0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2.5);

  ccache_destroy(cache);
}

TEST(get_val_types, char_ptr_value_macro_transfers_ownership) {
  ccache_construct_scoped(cache, int, char *, 8, NULL, NULL, NULL);

  ccache_set(cache, 10, "hello");

  char *out = NULL;
  REQUIRE_EQ(ccache_get(cache, 10, &out), ccol_success);
  REQUIRE_NOT_NULL(out);
  REQUIRE_STREQ(out, "hello");
  free(out);

  ccache_destroy(cache);
}

TEST(get_val_types, char_ptr_value_two_gets_are_independent_copies) {
  ccache_construct_scoped(cache, int, char *, 8, NULL, NULL, NULL);

  ccache_set(cache, 7, "world");

  char *a = NULL, *b = NULL;
  REQUIRE_EQ(ccache_get(cache, 7, &a), ccol_success);
  REQUIRE_EQ(ccache_get(cache, 7, &b), ccol_success);

  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_PTR_NE(a, b);
  REQUIRE_STREQ(a, "world");
  REQUIRE_STREQ(b, "world");
  free(a);
  free(b);

  ccache_destroy(cache);
}

TEST(get_val_types, char_ptr_value_full_api_size_includes_null_terminator) {
  ccache_construct_scoped(cache, int, char *, 8, NULL, NULL, NULL);

  ccache_set(cache, 3, "test_string");

  int k = 3;
  cmap_pair kp = {};
  ccol__populate_cmap_pair(&kp, k);

  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_success);
  REQUIRE_NOT_NULL(val_out.ptr);
  REQUIRE_EQ(val_out.size, strlen("test_string") + 1);
  REQUIRE_STREQ((char *)val_out.ptr, "test_string");
  free(val_out.ptr);

  ccache_destroy(cache);
}

TEST(get_val_types, char_ptr_key_and_char_ptr_value) {
  ccache_construct_scoped(cache, char *, char *, 8, NULL, NULL, NULL);

  ccache_set(cache, "k1", "v1");
  ccache_set(cache, "k2", "v2");

  char *out = NULL;
  REQUIRE_EQ(ccache_get(cache, "k1", &out), ccol_success);
  REQUIRE_STREQ(out, "v1");
  free(out);

  out = NULL;
  REQUIRE_EQ(ccache_get(cache, "k2", &out), ccol_success);
  REQUIRE_STREQ(out, "v2");
  free(out);

  REQUIRE_EQ(ccache_get(cache, "missing", &out), ccol_key_not_found);

  ccache_destroy(cache);
}

TEST(get_val_types, char_ptr_value_from_remote_getter_macro) {
  ccache_construct_scoped(cache, int, char *, 8, str_val_remote_getter, NULL,
                          NULL);

  char *out = NULL;
  REQUIRE_EQ(ccache_get(cache, 42, &out), ccol_success);
  REQUIRE_NOT_NULL(out);
  REQUIRE_STREQ(out, "remote_42");
  free(out);

  out = NULL;
  REQUIRE_EQ(ccache_get(cache, 42, &out), ccol_success);
  REQUIRE_STREQ(out, "remote_42");
  free(out);

  ccache_destroy(cache);
}

TEST(get_val_types, char_ptr_value_from_remote_getter_full_api) {
  ccache_construct_scoped(cache, int, char *, 8, str_val_remote_getter, NULL,
                          NULL);

  int k = 7;
  cmap_pair kp = {};
  ccol__populate_cmap_pair(&kp, k);

  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_success);
  REQUIRE_NOT_NULL(val_out.ptr);
  REQUIRE_EQ(val_out.size, strlen("remote_7") + 1);
  REQUIRE_STREQ((char *)val_out.ptr, "remote_7");
  free(val_out.ptr);

  ccache_destroy(cache);
}

// The live entry branch of the size check; the fetch branch is covered by the
// oversized and undersized getter tests.
TEST(get_val_types, get_into_rejects_live_value_too_large_for_buffer) {
  ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  int k = 1;
  long long v = 12345LL;
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};

  ccol_retval set_rv = clrucache_set_full(cache, &kp, &vp);
  size_t size_after_set = clrucache_size(cache);

  int small_out = 0;
  ccol_retval small_rv =
      clrucache__get_into(cache, &kp, &small_out, sizeof(small_out));

  size_t size_after_small = clrucache_size(cache);

  long long big_out = 0;
  ccol_retval big_rv =
      clrucache__get_into(cache, &kp, &big_out, sizeof(big_out));

  clrucache__destroy(cache);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(size_after_set, (size_t)1);
  REQUIRE_EQ(small_rv, ccol_unexpected_failure);
  REQUIRE_EQ(small_out, 0);
  REQUIRE_EQ(size_after_small, (size_t)1);
  REQUIRE_EQ(big_rv, ccol_success);
  REQUIRE_EQ(big_out, 12345LL);
}

TEST(get_val_types, get_into_rejects_live_value_too_small_for_buffer) {
  ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  int k = 1;
  char v = 0x5A;
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};

  ccol_retval set_rv = clrucache_set_full(cache, &kp, &vp);
  size_t size_after_set = clrucache_size(cache);

  long long big_out = 0x1122334455667788LL;
  ccol_retval big_rv =
      clrucache__get_into(cache, &kp, &big_out, sizeof(big_out));

  size_t size_after_big = clrucache_size(cache);

  char small_out = 0;
  ccol_retval small_rv =
      clrucache__get_into(cache, &kp, &small_out, sizeof(small_out));

  clrucache__destroy(cache);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(size_after_set, (size_t)1);
  REQUIRE_EQ(big_rv, ccol_unexpected_failure);
  REQUIRE_EQ(big_out, 0x1122334455667788LL);
  REQUIRE_EQ(size_after_big, (size_t)1);
  REQUIRE_EQ(small_rv, ccol_success);
  REQUIRE_EQ(small_out, 0x5A);
}

// A get refused for its buffer size must not move the entry to the front, or a
// never-read entry would outlive one set more recently.
TEST(get_val_types, get_into_size_mismatch_failure_does_not_promote_lru) {
  ccache cache = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  int k1 = 1;
  long long v1 = 111;
  cmap_pair kp1 = {.ptr = &k1, .size = sizeof(k1)};
  cmap_pair vp1 = {.ptr = &v1, .size = sizeof(v1)};
  ccol_retval set1_rv = clrucache_set_full(cache, &kp1, &vp1);

  int k2 = 2;
  long long v2 = 222;
  cmap_pair kp2 = {.ptr = &k2, .size = sizeof(k2)};
  cmap_pair vp2 = {.ptr = &v2, .size = sizeof(v2)};
  ccol_retval set2_rv = clrucache_set_full(cache, &kp2, &vp2);

  int wrong_size_out = 0;
  ccol_retval wrong_size_rv =
      clrucache__get_into(cache, &kp1, &wrong_size_out, sizeof(wrong_size_out));

  int k3 = 3;
  long long v3 = 333;
  cmap_pair kp3 = {.ptr = &k3, .size = sizeof(k3)};
  cmap_pair vp3 = {.ptr = &v3, .size = sizeof(v3)};
  ccol_retval set3_rv = clrucache_set_full(cache, &kp3, &vp3);
  size_t size_after_set3 = clrucache_size(cache);

  long long out = 0;
  ccol_retval get1_rv = clrucache__get_into(cache, &kp1, &out, sizeof(out));
  ccol_retval get2_rv = clrucache__get_into(cache, &kp2, &out, sizeof(out));

  clrucache__destroy(cache);

  REQUIRE_EQ(set1_rv, ccol_success);
  REQUIRE_EQ(set2_rv, ccol_success);
  REQUIRE_EQ(wrong_size_rv, ccol_unexpected_failure);
  REQUIRE_EQ(set3_rv, ccol_success);
  REQUIRE_EQ(size_after_set3, (size_t)2);
  REQUIRE_EQ(get1_rv, ccol_key_not_found);
  REQUIRE_EQ(get2_rv, ccol_success);
  REQUIRE_EQ(out, 222LL);
}

// ccache_set and ccache_get convert keys and values to KeyT and ValT as by
// assignment; copying the raw bits of a same-sized value of another type would
// pass every size check.
TEST(type_conversion, set_float_into_int_cache_converts_not_reinterprets) {
  ccache_construct_scoped(cache, int, int, 8, NULL, NULL, NULL);

  float f = 7.0f;
  REQUIRE_EQ(ccache_set(cache, 1, f), ccol_success);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 7);

  ccache_destroy(cache);
}

TEST(type_conversion,
     set_negative_float_into_int_cache_truncates_like_plain_assignment) {
  ccache_construct_scoped(cache, int, int, 8, NULL, NULL, NULL);

  float f = -3.75f;
  REQUIRE_EQ(ccache_set(cache, 1, f), ccol_success);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, -3);

  ccache_destroy(cache);
}

TEST(type_conversion, set_int_literal_into_double_cache_converts) {
  ccache_construct_scoped(cache, int, double, 8, NULL, NULL, NULL);

  REQUIRE_EQ(ccache_set(cache, 1, 5), ccol_success);

  double out = 0.0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 5.0);

  ccache_destroy(cache);
}

TEST(type_conversion, get_into_double_out_param_converts_stored_int) {
  ccache_construct_scoped(cache, int, int, 8, NULL, NULL, NULL);

  REQUIRE_EQ(ccache_set(cache, 1, 9), ccol_success);

  double out = 0.0;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 9.0);

  ccache_destroy(cache);
}

TEST(type_conversion,
     get_into_float_out_param_does_not_reinterpret_stored_int) {
  ccache_construct_scoped(cache, int, int, 8, NULL, NULL, NULL);

  REQUIRE_EQ(ccache_set(cache, 1, 12), ccol_success);

  float out = 0.0f;
  REQUIRE_EQ(ccache_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 12.0f);

  ccache_destroy(cache);
}

TEST(type_conversion, set_string_key_from_local_char_array_variable) {
  ccache_construct_scoped(cache, char *, int, 8, NULL, NULL, NULL);

  char key_buf[16];
  strcpy(key_buf, "arraykey");
  REQUIRE_EQ(ccache_set(cache, key_buf, 42), ccol_success);

  int out = 0;
  REQUIRE_EQ(ccache_get(cache, "arraykey", &out), ccol_success);
  REQUIRE_EQ(out, 42);

  ccache_destroy(cache);
}

// An allocator that counts allocations and frees, to see that the cache uses
// the procs it was given.
static size_t _lru_custom_alloc_count = 0;
static size_t _lru_custom_free_count = 0;

static void *_lru_custom_malloc(size_t sz) {
  _lru_custom_alloc_count++;
  return malloc(sz);
}
static void _lru_custom_free(void *p) {
  if (p) _lru_custom_free_count++;
  free(p);
}
static void *_lru_custom_calloc(size_t n, size_t sz) {
  _lru_custom_alloc_count++;
  return calloc(n, sz);
}
static void *_lru_custom_realloc(void *p, size_t sz) {
  _lru_custom_alloc_count++;
  return realloc(p, sz);
}

// An allocator with a budget: Below 0 no limit, at 0 every call fails, else
// each success takes one off. A test can then fail one particular later
// allocation, such as the copy of a hit.
static int _lru_fault_alloc_budget = -1;

static void *_lru_fault_malloc(size_t sz) {
  if (_lru_fault_alloc_budget == 0) return NULL;
  if (_lru_fault_alloc_budget > 0) _lru_fault_alloc_budget--;
  return malloc(sz);
}
static void _lru_fault_free(void *p) { free(p); }
static void *_lru_fault_calloc(size_t n, size_t sz) {
  if (_lru_fault_alloc_budget == 0) return NULL;
  if (_lru_fault_alloc_budget > 0) _lru_fault_alloc_budget--;
  return calloc(n, sz);
}
static void *_lru_fault_realloc(void *p, size_t sz) {
  if (_lru_fault_alloc_budget == 0) return NULL;
  if (_lru_fault_alloc_budget > 0) _lru_fault_alloc_budget--;
  return realloc(p, sz);
}

TEST(custom_alloc, get_full_copy_uses_custom_allocator) {
  _lru_custom_alloc_count = 0;
  _lru_custom_free_count = 0;
  ccol_memmgmt_procs mprocs = {_lru_custom_malloc, _lru_custom_free,
                               _lru_custom_calloc, _lru_custom_realloc};

  char *err = NULL;
  ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                       &mprocs, &err);
  REQUIRE_NE(cache, CCACHE_NULL);

  int k = 1, v = 42;
  cmap_pair kp = {}, vp = {};
  ccol__populate_cmap_pair(&kp, k);
  ccol__populate_cmap_pair(&vp, v);
  ccol_retval set_rv = clrucache_set_full(cache, &kp, &vp);

  cmap_pair val_out = {};
  ccol_retval get_rv = clrucache_get_full(cache, &kp, &val_out);
  bool got_copy = val_out.ptr != NULL;
  int got = got_copy ? *(int *)val_out.ptr : -1;

  _lru_custom_free(val_out.ptr);

  clrucache__destroy(cache);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(get_rv, ccol_success);
  REQUIRE_TRUE(got_copy);
  REQUIRE_EQ(got, 42);
  REQUIRE_EQ(_lru_custom_alloc_count, _lru_custom_free_count);
}

TEST(custom_alloc, char_ptr_get_full_uses_custom_allocator) {
  _lru_custom_alloc_count = 0;
  _lru_custom_free_count = 0;
  ccol_memmgmt_procs mprocs = {_lru_custom_malloc, _lru_custom_free,
                               _lru_custom_calloc, _lru_custom_realloc};

  char *err = NULL;
  ccache cache = clrucache_create_full(8, ccol_string, ccol_string, NULL, NULL,
                                       NULL, &mprocs, &err);
  REQUIRE_NE(cache, CCACHE_NULL);

  char *k = "hello", *v = "world";
  cmap_pair kp = {}, vp = {};
  ccol__populate_cmap_pair(&kp, k);
  ccol__populate_cmap_pair(&vp, v);
  ccol_retval set_rv = clrucache_set_full(cache, &kp, &vp);

  cmap_pair val_out = {};
  ccol_retval get_rv = clrucache_get_full(cache, &kp, &val_out);
  bool got_copy = val_out.ptr != NULL;
  bool matches = got_copy && strcmp((char *)val_out.ptr, "world") == 0;

  _lru_custom_free(val_out.ptr);

  clrucache__destroy(cache);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(get_rv, ccol_success);
  REQUIRE_TRUE(got_copy);
  REQUIRE_TRUE(matches);
  REQUIRE_EQ(_lru_custom_alloc_count, _lru_custom_free_count);
}

TEST(custom_alloc, invalid_mprocs_returns_null) {
  ccol_memmgmt_procs bad = {_lru_custom_malloc, NULL, _lru_custom_calloc,
                            _lru_custom_realloc};
  char *err = NULL;
  ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                       &bad, &err);
  REQUIRE_EQ(cache, CCACHE_NULL);
}

// A hit whose copy can't be allocated must not move the entry to the front.
TEST(custom_alloc, get_full_copy_oom_failure_does_not_promote_lru) {
  _lru_fault_alloc_budget = -1;
  ccol_memmgmt_procs mprocs = {_lru_fault_malloc, _lru_fault_free,
                               _lru_fault_calloc, _lru_fault_realloc};

  ccache cache = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL, NULL,
                                       &mprocs, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  int k1 = 1, v1 = 111;
  cmap_pair kp1 = {}, vp1 = {};
  ccol__populate_cmap_pair(&kp1, k1);
  ccol__populate_cmap_pair(&vp1, v1);
  ccol_retval set1_rv = clrucache_set_full(cache, &kp1, &vp1);

  int k2 = 2, v2 = 222;
  cmap_pair kp2 = {}, vp2 = {};
  ccol__populate_cmap_pair(&kp2, k2);
  ccol__populate_cmap_pair(&vp2, v2);
  ccol_retval set2_rv = clrucache_set_full(cache, &kp2, &vp2);

  _lru_fault_alloc_budget = 0;
  cmap_pair val_out = {};
  ccol_retval oom_rv = clrucache_get_full(cache, &kp1, &val_out);
  void *oom_ptr = val_out.ptr;

  _lru_fault_alloc_budget = -1;

  int k3 = 3, v3 = 333;
  cmap_pair kp3 = {}, vp3 = {};
  ccol__populate_cmap_pair(&kp3, k3);
  ccol__populate_cmap_pair(&vp3, v3);
  ccol_retval set3_rv = clrucache_set_full(cache, &kp3, &vp3);
  size_t size_after_set3 = clrucache_size(cache);

  cmap_pair out = {};
  ccol_retval get1_rv = clrucache_get_full(cache, &kp1, &out);
  ccol_retval get2_rv = clrucache_get_full(cache, &kp2, &out);
  bool got_copy = out.ptr != NULL;
  int got = got_copy ? *(int *)out.ptr : -1;
  free(out.ptr);
  free(oom_ptr);

  clrucache__destroy(cache);

  REQUIRE_EQ(set1_rv, ccol_success);
  REQUIRE_EQ(set2_rv, ccol_success);
  REQUIRE_EQ(oom_rv, ccol_not_enough_memory);
  REQUIRE_NULL(oom_ptr);
  REQUIRE_EQ(set3_rv, ccol_success);
  REQUIRE_EQ(size_after_set3, (size_t)2);
  REQUIRE_EQ(get1_rv, ccol_key_not_found);
  REQUIRE_EQ(get2_rv, ccol_success);
  REQUIRE_TRUE(got_copy);
  REQUIRE_EQ(got, 222);
}

// The publish into the pin table can fail on the first use of an index; the
// rollback must put the slot back and clear the cache's handle.
TEST(clrucache_handle_lifecycle, handle_publish_failure_rolls_the_slot_back) {
  enum { CYCLES = 8 };
  size_t before = clrucache__slot_table_capacity_for_tests();
  size_t free_before = clrucache__free_index_count_for_tests();

  bool all_failed = true, all_created = true;
  ccol_retval get_rv = ccol_unexpected_failure;
  int out = 0;

  for (int i = 0; i < CYCLES; i++) {
    ccol__pintable_force_next_publish_failure_for_tests();
    char *err = NULL;
    ccache failed = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                          NULL, NULL, &err);
    if (failed != CCACHE_NULL || err == NULL) {
      all_failed = false;
      if (failed != CCACHE_NULL) clrucache__destroy(failed);
      break;
    }

    ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                         NULL, NULL, NULL);
    if (cache == CCACHE_NULL) {
      all_created = false;
      break;
    }
    ccache_redeclare(cache, int, int);
    int k = 7, v = 42;
    out = 0;
    ccache_set(cache, k, v);
    get_rv = ccache_get(cache, k, &out);
    clrucache__destroy(cache);
  }

  size_t after = clrucache__slot_table_capacity_for_tests();
  size_t free_after = clrucache__free_index_count_for_tests();

  REQUIRE_TRUE(all_failed);
  REQUIRE_TRUE(all_created);
  REQUIRE_EQ(get_rv, ccol_success);
  REQUIRE_EQ(out, 42);
  REQUIRE_LE(after, before + 2);
  REQUIRE_GE(free_after + 2, free_before);
}

// Fills and overfills a cache of capacity entries and checks that it reports
// capacity, keeps at least 3/4 of it and never more, and returns every value it
// holds correctly. Covers the segment choice, per-segment eviction, the size
// sum and the teardown of a split cache.
static void ccache_check_segmented_capacity(size_t capacity) {
  ccache_construct(cache, int, int, capacity, NULL, NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  size_t reported = clrucache_capacity(cache);
  size_t fresh_size = clrucache_size(cache);

  for (size_t i = 0; i < capacity; i++) {
    int k = (int)i, v = (int)i * 7;
    ccache_set(cache, k, v);
  }
  size_t filled = clrucache_size(cache);

  size_t readable = 0, wrong_value = 0;
  for (size_t i = 0; i < capacity; i++) {
    int k = (int)i, out = -1;
    if (ccache_get(cache, k, &out) == ccol_success) {
      readable++;
      if (out != (int)i * 7) wrong_value++;
    }
  }

  for (size_t i = capacity; i < capacity * 3; i++) {
    int k = (int)i, v = (int)i;
    ccache_set(cache, k, v);
  }
  size_t after_overfill = clrucache_size(cache);

  ccache_destroy(cache);

  REQUIRE_EQ(reported, capacity);
  REQUIRE_EQ(fresh_size, (size_t)0);
  REQUIRE_EQ(wrong_value, (size_t)0);
  REQUIRE_EQ(readable, filled);
  REQUIRE_LE(filled, capacity);
  REQUIRE_LE(after_overfill, capacity);
  REQUIRE_GE(filled, capacity - capacity / 4);
}

// Checks that a cache below the split threshold keeps exactly capacity entries,
// since it has one exact eviction order.
static void ccache_check_unsplit_capacity_is_exact(size_t capacity) {
  ccache_construct(cache, int, int, capacity, NULL, NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);
  for (size_t i = 0; i < capacity; i++) ccache_set(cache, (int)i, (int)i);
  size_t filled = clrucache_size(cache);
  ccache_destroy(cache);
  REQUIRE_EQ(filled, capacity);
}

TEST(segmented, capacity_below_the_split_threshold_is_one_segment) {
  ccache_check_unsplit_capacity_is_exact(64);
  ccache_check_unsplit_capacity_is_exact(127);
  ccache_check_segmented_capacity(64);
  ccache_check_segmented_capacity(127);
}

TEST(segmented, capacity_at_and_above_the_split_threshold) {
  ccache_check_segmented_capacity(128);
  ccache_check_segmented_capacity(256);
  ccache_check_segmented_capacity(2048);
}

TEST(segmented, a_capacity_that_does_not_divide_evenly_keeps_its_remainder) {
  static const size_t capacities[] = {200, 1000, 130};
  static const size_t expected_segments[] = {3, 15, 2};

  for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
    ccache_construct(cache, int, int, capacities[i], NULL, NULL, NULL);
    size_t segments = 0;
    size_t sum = clrucache__segment_capacity_sum_for_tests(cache, &segments);
    size_t reported = clrucache_capacity(cache);
    ccache_destroy(cache);

    REQUIRE_EQ(segments, expected_segments[i]);
    REQUIRE_EQ(sum, capacities[i]);
    REQUIRE_EQ(reported, capacities[i]);
  }

  ccache_check_segmented_capacity(200);
}

TEST(segmented, a_key_always_lands_in_the_same_segment) {
  enum { CAP = 512, KEYS = CAP / 2, ROUNDS = 4 };
  ccache_construct(cache, int, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  size_t misses = 0, wrong = 0;
  for (int round = 0; round < ROUNDS; round++) {
    for (int i = 0; i < KEYS; i++) {
      int k = i, v = i * 100 + round;
      ccache_set(cache, k, v);
    }
    for (int i = 0; i < KEYS; i++) {
      int k = i, out = -1;
      if (ccache_get(cache, k, &out) != ccol_success)
        misses++;
      else if (out != i * 100 + round)
        wrong++;
    }
  }

  ccache_destroy(cache);

  REQUIRE_EQ(wrong, (size_t)0);
  REQUIRE_EQ(misses, (size_t)0);
}

// A xorshift step: A deterministic key stream with no structure, next to the
// sequential and scaled key sets.
static uint32_t ccache_spread_next(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

TEST(segmented, keys_sharing_their_low_bits_still_use_every_segment) {
  static const long long scales[] = {16, 64, 4096, 65536};
  enum { CAP = 1024 };

  for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
    ccache_construct(cache, long long, int, CAP, NULL, NULL, NULL);
    REQUIRE_NE(cache, CCACHE_NULL);

    for (int i = 0; i < CAP; i++) {
      ccache_set(cache, (long long)i * scales[s], i);
    }
    size_t filled = clrucache_size(cache);
    ccache_destroy(cache);

    REQUIRE_LE(filled, (size_t)CAP);
    REQUIRE_GE(filled, (size_t)(CAP - CAP / 8));
  }
}

TEST(segmented, an_unevenly_spread_key_set_keeps_nearly_all_of_its_capacity) {
  enum { CAP = 4096 };
  ccache_construct(cache, int, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  uint32_t state = 0x1234567u;
  for (int i = 0; i < CAP; i++) {
    ccache_set(cache, (int)(ccache_spread_next(&state) & 0x7fffffff), i);
  }

  size_t filled = clrucache_size(cache);
  size_t capacity = clrucache_capacity(cache);
  ccache_destroy(cache);

  REQUIRE_EQ(capacity, (size_t)CAP);
  REQUIRE_LE(filled, (size_t)CAP);
  REQUIRE_GE(filled, (size_t)CAP - (size_t)CAP / 16);
}

TEST(segmented, a_long_double_key_is_one_key_however_many_segments) {
  static const size_t capacities[] = {64, 256};

  for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
    ccache cache =
        clrucache_create_full(capacities[c], ccol_long_double, ccol_int, NULL,
                              NULL, NULL, NULL, NULL);
    REQUIRE_NE(cache, CCACHE_NULL);

    unsigned char image_a[sizeof(long double)];
    unsigned char image_b[sizeof(long double)];
    memset(image_a, 0x00, sizeof(image_a));
    memset(image_b, 0xAB, sizeof(image_b));

    long double value = 3.5L;
    size_t significant = sizeof(long double);
#if LDBL_MANT_DIG == 64
    significant = 10;
#endif
    memcpy(image_a, &value, significant);
    memcpy(image_b, &value, significant);

    cmap_pair key_a = {.ptr = image_a, .size = sizeof(long double)};
    cmap_pair key_b = {.ptr = image_b, .size = sizeof(long double)};
    int stored = 91, read_back = 0;
    cmap_pair val_in = {.ptr = &stored, .size = sizeof(stored)};

    ccol_retval set_rv = clrucache_set_full(cache, &key_a, &val_in);
    ccol_retval get_rv =
        clrucache__get_into(cache, &key_b, &read_back, sizeof(read_back));
    size_t entries = clrucache_size(cache);

    clrucache__destroy(cache);

    REQUIRE_EQ(set_rv, ccol_success);
    REQUIRE_EQ(get_rv, ccol_success);
    REQUIRE_EQ(read_back, 91);
    REQUIRE_EQ(entries, (size_t)1);
  }
}

TEST(segmented, a_key_size_that_disagrees_with_the_key_type_is_rejected) {
  static const size_t capacities[] = {64, 256};

  for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
    ccache cache = clrucache_create_full(capacities[c], ccol_int, ccol_int,
                                         NULL, NULL, NULL, NULL, NULL);
    REQUIRE_NE(cache, CCACHE_NULL);

    int *key_bytes = (int *)malloc(sizeof(int));
    if (key_bytes) *key_bytes = 5;

    int value = 1, read_back = 0;
    cmap_pair oversized = {.ptr = key_bytes, .size = 4096};
    cmap_pair val_in = {.ptr = &value, .size = sizeof(value)};
    cmap_pair val_out = {.ptr = &read_back, .size = sizeof(read_back)};

    ccol_retval get_rv = clrucache_get_full(cache, &oversized, &val_out);
    ccol_retval set_rv = clrucache_set_full(cache, &oversized, &val_in);
    ccol_retval into_rv =
        clrucache__get_into(cache, &oversized, &read_back, sizeof(read_back));

    bool key_allocated = (key_bytes != NULL);
    free(key_bytes);
    clrucache__destroy(cache);

    REQUIRE_TRUE(key_allocated);
    REQUIRE_EQ(get_rv, ccol_invalid_args);
    REQUIRE_EQ(set_rv, ccol_invalid_args);
    REQUIRE_EQ(into_rv, ccol_invalid_args);
  }
}

TEST(segmented, string_keys_spread_across_segments_and_round_trip) {
  enum { CAP = 512, KEYS = CAP / 2 };
  ccache_construct(cache, char *, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  size_t misses = 0, wrong = 0;
  for (int i = 0; i < KEYS; i++) {
    char key[32];
    snprintf(key, sizeof(key), "session:%d", i);
    int v = i * 3;
    ccache_set(cache, key, v);
  }
  for (int i = 0; i < KEYS; i++) {
    char key[32];
    snprintf(key, sizeof(key), "session:%d", i);
    int out = -1;
    if (ccache_get(cache, key, &out) != ccol_success)
      misses++;
    else if (out != i * 3)
      wrong++;
  }

  ccache_destroy(cache);

  REQUIRE_EQ(wrong, (size_t)0);
  REQUIRE_EQ(misses, (size_t)0);
}

// Gives the inverse of the odd multiplier a modulo 2^64, to build keys that all
// share one home slot of the fast hash.
static uint64_t ccache_mul_inverse(uint64_t a) {
  uint64_t x = a;
  for (int i = 0; i < 6; i++) x *= 2 - a * x;
  return x;
}

// Keys i * M^-1 share home slot 0 of every map; the segment chooser uses the
// keyed hash, so each segment map gets a flood and must switch to keyed mode
// while the cache loses no key.
TEST(segmented, a_flood_switches_every_segment_map_and_loses_no_key) {
  enum { CAP = 8192, KEYS = 6000 };
  const uint64_t inv = ccache_mul_inverse(0x9E3779B97F4A7C15ULL);
  ccache_construct(cache, long long, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);
  size_t misses = 0, wrong = 0;
  for (uint64_t i = 0; i < KEYS; i++) {
    long long k = (long long)(i * inv);
    int v = (int)i;
    ccache_set(cache, k, v);
  }
  size_t segments = 0;
  size_t keyed = clrucache__keyed_segment_maps_for_tests(cache, &segments);
  for (uint64_t i = 0; i < KEYS; i++) {
    long long k = (long long)(i * inv);
    int out = -1;
    if (ccache_get(cache, k, &out) != ccol_success)
      misses++;
    else if (out != (int)i)
      wrong++;
  }
  size_t size = clrucache_size(cache);
  ccache_destroy(cache);

  ccache_construct(dcache, double, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(dcache, CCACHE_NULL);
  for (uint64_t i = 1; i <= KEYS; i++) {
    uint64_t bits = i * inv;
    double k;
    memcpy(&k, &bits, sizeof(k));
    if (k != k) continue;
    int v = (int)i;
    ccache_set(dcache, k, v);
  }
  size_t dsegments = 0;
  size_t dkeyed = clrucache__keyed_segment_maps_for_tests(dcache, &dsegments);
  double negative_zero = -0.0, positive_zero = 0.0;
  int zero_val = 77, zero_out = -1;
  ccache_set(dcache, negative_zero, zero_val);
  ccol_retval zr = ccache_get(dcache, positive_zero, &zero_out);
  ccache_destroy(dcache);

  REQUIRE_EQ(segments, (size_t)16);
  REQUIRE_EQ(keyed, segments);
  REQUIRE_EQ(misses, (size_t)0);
  REQUIRE_EQ(wrong, (size_t)0);
  REQUIRE_EQ(size, (size_t)KEYS);
  REQUIRE_EQ(dsegments, (size_t)16);
  REQUIRE_EQ(dkeyed, dsegments);
  REQUIRE_EQ(zr, ccol_success);
  REQUIRE_EQ(zero_out, 77);
}

TEST(clrucache_handle_lifecycle, sequential_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                         NULL, NULL, NULL);
    if (cache == CCACHE_NULL) test_child_exit(2);
    ccache stale = cache;
    ccache_destroy(cache);
    clrucache__destroy(stale);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The pipe that ccache_create_after_the_release reports through in the child of
// a_create_after_the_table_is_released_fails; -1 everywhere else.
static int g_late_create_report_fd = -1;

// Creates a cache after the library's module destructor released the table (a
// destructor with a priority runs after those without one) and writes 'R' when
// it's refused with "released", 'E' for another error, 'O' when it worked.
__attribute__((destructor(101))) static void ccache_create_after_the_release(
    void) {
  if (g_late_create_report_fd < 0) return;
  char *err = NULL;
  ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, &err);
  char verdict = 'O';
  if (cache == CCACHE_NULL)
    verdict = (err != NULL && strstr(err, "released") != NULL) ? 'R' : 'E';
  else
    ccache_destroy(cache);
  if (write(g_late_create_report_fd, &verdict, 1) != 1) test_child_exit(4);
  test_child_exit(0);
}

// A create that indexed the released table would stop the child at an assertion
// instead. OpenBSD runs no destructor of a program that links libpthread, so
// the test skips there.
TEST(clrucache_handle_lifecycle, a_create_after_the_table_is_released_fails) {
#if defined(__OpenBSD__)
  return;
#endif
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccache first = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                         NULL, NULL, NULL);
    if (first == CCACHE_NULL) test_child_exit(5);
    ccache_destroy(first);
    g_late_create_report_fd = p[1];
    exit(0);
  }
  close(p[1]);
  char verdict = 0;
  ssize_t got = pid > 0 ? read(p[0], &verdict, 1) : -1;
  close(p[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  REQUIRE_NE(pid, -1);
  REQUIRE_EQ(got, (ssize_t)1);
  REQUIRE_EQ(verdict, 'R');
  REQUIRE_TRUE(WIFEXITED(status));
}

// A thread that destroys handle h, so two threads can race on one handle.
typedef struct {
  ccache h;
} ccache_concurrent_destroy_arg_t;

// Destroys a->h.
static void *ccache_concurrent_destroy_thread(void *arg) {
  ccache_concurrent_destroy_arg_t *a = (ccache_concurrent_destroy_arg_t *)arg;
  clrucache__destroy(a->h);
  return NULL;
}

TEST(clrucache_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                         NULL, NULL, NULL);
    if (cache == CCACHE_NULL) test_child_exit(2);
    ccache_concurrent_destroy_arg_t a1 = {.h = cache};
    ccache_concurrent_destroy_arg_t a2 = {.h = cache};
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, ccache_concurrent_destroy_thread, &a1) != 0)
      test_child_exit(2);
    if (pthread_create(&t2, NULL, ccache_concurrent_destroy_thread, &a2) != 0)
      test_child_exit(2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A destroy must wait for a pin in flight. The gated remote calls hold until
// the test opens ccache_gate_open; ccache_gate_entered tells that one is
// inside.
static atomic_int ccache_gate_entered;
static atomic_int ccache_gate_open;

// Waits at most 10 s for *flag and gives false on a timeout.
static bool ccache_wait_flag(atomic_int *flag) {
  for (int i = 0; i < 10000; i++) {
    if (atomic_load(flag)) return true;
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
  }
  return false;
}

// Marks the call as inside and holds until the gate opens (10 s at most).
static void ccache_gate_hold(void) {
  atomic_store(&ccache_gate_entered, 1);
  (void)ccache_wait_flag(&ccache_gate_open);
}

// A remote getter that holds at the gate and gives key + 1000.
static bool gated_remote_getter(const cmap_pair *key, cmap_pair *val) {
  ccache_gate_hold();
  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k + 1000;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// A remote setter that holds at the gate and succeeds.
static bool gated_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  (void)val;
  ccache_gate_hold();
  return true;
}

// A gated get or set on its own thread, with its result.
typedef struct {
  ccache h;
  ccol_retval rv;
} ccache_gated_call_arg_t;

// Gets key 42, which runs the gated getter.
static void *ccache_gated_get_thread(void *arg) {
  ccache_gated_call_arg_t *a = (ccache_gated_call_arg_t *)arg;
  ccache c = a->h;
  ccache_redeclare(c, int, int);
  int out = 0;
  a->rv = ccache_get(c, 42, &out);
  return NULL;
}

// Sets key 42, which runs the gated setter.
static void *ccache_gated_set_thread(void *arg) {
  ccache_gated_call_arg_t *a = (ccache_gated_call_arg_t *)arg;
  ccache c = a->h;
  ccache_redeclare(c, int, int);
  int k = 42, v = 100;
  a->rv = ccache_set(c, k, v);
  return NULL;
}

// A destroy on its own thread, with a flag set when it returns.
typedef struct {
  ccache h;
  atomic_int returned;
} ccache_destroy_arg_t;

// Destroys h and sets returned.
static void *ccache_destroy_thread(void *arg) {
  ccache_destroy_arg_t *a = (ccache_destroy_arg_t *)arg;
  ccache c = a->h;
  ccache_redeclare(c, int, int);
  ccache_destroy(c);
  atomic_store(&a->returned, 1);
  return NULL;
}

// Runs one race: call_thread enters the gated remote call, a destroy starts,
// and 200 ms later the destroy must still wait while the gate is shut.
// *entered, *waited, *ended and *call_rv come back once every thread is joined;
// when *ended is false the cache still lives and the caller destroys it.
static void ccache_run_gated_destroy_race(ccache cache,
                                          void *(*call_thread)(void *),
                                          bool *entered, bool *waited,
                                          bool *ended, ccol_retval *call_rv) {
  atomic_store(&ccache_gate_entered, 0);
  atomic_store(&ccache_gate_open, 0);
  *entered = *waited = *ended = false;
  *call_rv = ccol_unexpected_failure;

  ccache_gated_call_arg_t call = {.h = cache, .rv = ccol_unexpected_failure};
  pthread_t call_tid;
  if (pthread_create(&call_tid, NULL, call_thread, &call) != 0) return;
  *entered = ccache_wait_flag(&ccache_gate_entered);

  ccache_destroy_arg_t d = {.h = cache};
  atomic_init(&d.returned, 0);
  pthread_t destroy_tid;
  bool destroy_started =
      *entered &&
      pthread_create(&destroy_tid, NULL, ccache_destroy_thread, &d) == 0;
  if (destroy_started) {
    struct timespec ts = {0, 200000000};
    nanosleep(&ts, NULL);
    *waited = !atomic_load(&d.returned);
  }
  atomic_store(&ccache_gate_open, 1);
  pthread_join(call_tid, NULL);
  if (destroy_started) {
    pthread_join(destroy_tid, NULL);
    *ended = atomic_load(&d.returned) != 0;
  }
  *call_rv = call.rv;
}

TEST(clrucache_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  ccache_construct(cache, int, int, 8, gated_remote_getter, NULL, NULL);
  bool entered, waited, ended;
  ccol_retval rv;
  ccache_run_gated_destroy_race(cache, ccache_gated_get_thread, &entered,
                                &waited, &ended, &rv);
  if (!ended) ccache_destroy(cache);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(waited);
  REQUIRE_TRUE(ended);
  REQUIRE_EQ(rv, ccol_success);
}

// The set path handles the entry and its waiters differently around the pin, so
// it needs its own race.
TEST(clrucache_handle_lifecycle, resolve_then_use_race_destroy_waits_for_set) {
  ccache_construct(cache, int, int, 8, NULL, gated_remote_setter, NULL);
  bool entered, waited, ended;
  ccol_retval rv;
  ccache_run_gated_destroy_race(cache, ccache_gated_set_thread, &entered,
                                &waited, &ended, &rv);
  if (!ended) ccache_destroy(cache);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(waited);
  REQUIRE_TRUE(ended);
  REQUIRE_EQ(rv, ccol_success);
}

// A thread that calls clrucache_capacity, a fast call that races a destroy.
typedef struct {
  ccache h;
} ccache_capacity_arg_t;

// Calls clrucache_capacity once.
static void *ccache_capacity_thread(void *arg) {
  ccache_capacity_arg_t *a = (ccache_capacity_arg_t *)arg;
  clrucache_capacity(a->h);
  return NULL;
}

// A fast resolve and unpin raced against a destroy; the window is a few
// instructions wide, so it repeats under stress.
TEST(clrucache_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                         NULL, NULL, NULL);
    REQUIRE_NE(cache, CCACHE_NULL);

    ccache_capacity_arg_t capacity_arg = {.h = cache};
    ccache_concurrent_destroy_arg_t destroy_arg = {.h = cache};
    pthread_t capacity_tid, destroy_tid;
    bool started_capacity_tid =
        (pthread_create(&capacity_tid, NULL, ccache_capacity_thread,
                        &capacity_arg) == 0);
    bool started_destroy_tid =
        (pthread_create(&destroy_tid, NULL, ccache_concurrent_destroy_thread,
                        &destroy_arg) == 0);
    if (started_capacity_tid) pthread_join(capacity_tid, NULL);
    if (started_destroy_tid) pthread_join(destroy_tid, NULL);
    if (!started_destroy_tid) clrucache__destroy(cache);
    REQUIRE_TRUE(started_capacity_tid);
    REQUIRE_TRUE(started_destroy_tid);
  }
}

TEST(clrucache_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  ccache a = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                   NULL, NULL);
  bool a_created = a != CCACHE_NULL;
  ccache stale_a = a;
  ccache_destroy(a);

  ccache b = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                   NULL, NULL);
  bool b_created = b != CCACHE_NULL;

  size_t b_capacity = clrucache_capacity(b);

  void *stale_resolved = (void *)clrucache__resolve_for_tests(stale_a);

  ccache_destroy(b);

  REQUIRE_TRUE(a_created);
  REQUIRE_TRUE(b_created);
  REQUIRE_EQ(b_capacity, (size_t)8);
  REQUIRE_EQ(stale_resolved, NULL);
}

// The capacity is read after the first cycle, since earlier tests may have
// grown the table; this test's own churn must add nothing.
TEST(clrucache_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  ccache cache0 = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                        NULL, NULL);
  REQUIRE_NE(cache0, CCACHE_NULL);
  ccache_destroy(cache0);
  size_t capacity_after_first = clrucache__slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                         NULL, NULL, NULL);
    REQUIRE_NE(cache, CCACHE_NULL);
    ccache_destroy(cache);
  }

  REQUIRE_EQ(clrucache__slot_table_capacity_for_tests(), capacity_after_first);
}

// An allocator that fails the Nth allocation of any kind (g_lru_fail_at, 0
// disarms), so a sweep reaches every unwind point of cache construction.
// g_lru_alloc_seen counts the allocations since the last arm.
static atomic_int g_lru_alloc_seen = 0;
static atomic_int g_lru_fail_at = 0;

// Counts one allocation and tells whether it's the one to fail.
static bool _lru_should_fail(void) {
  int at = atomic_load(&g_lru_fail_at);
  if (at == 0) return false;
  return (atomic_fetch_add(&g_lru_alloc_seen, 1) + 1) == at;
}
static void *_lru_sweep_malloc(size_t n) {
  return _lru_should_fail() ? NULL : malloc(n);
}
static void _lru_sweep_free(void *p) { free(p); }
static void *_lru_sweep_calloc(size_t a, size_t b) {
  return _lru_should_fail() ? NULL : calloc(a, b);
}
static void *_lru_sweep_realloc(void *p, size_t n) {
  return _lru_should_fail() ? NULL : realloc(p, n);
}
// The procs table of the sweep allocator.
static ccol_memmgmt_procs g_lru_sweep_procs = {
    _lru_sweep_malloc, _lru_sweep_free, _lru_sweep_calloc, _lru_sweep_realloc};

// Arms the sweep allocator to fail allocation number nth.
static void _lru_arm(int nth) {
  atomic_store(&g_lru_alloc_seen, 0);
  atomic_store(&g_lru_fail_at, nth);
}
// Disarms the sweep allocator.
static void _lru_disarm(void) { atomic_store(&g_lru_fail_at, 0); }

// Deep enough to get past the last allocation of a construction.
#define LRU_SWEEP_DEPTH 24

TEST(clrucache_oom, integral_cache_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= LRU_SWEEP_DEPTH; n++) {
    _lru_arm(n);
    char *err = NULL;
    ccache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                         NULL, &g_lru_sweep_procs, &err);
    _lru_disarm();
    if (cache != CCACHE_NULL) {
      clrucache__destroy(cache);
    } else if (!err) {
      all_handled = false;
    }
  }
  REQUIRE_TRUE(all_handled);
}

TEST(clrucache_oom,
     string_keyed_cache_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= LRU_SWEEP_DEPTH; n++) {
    _lru_arm(n);
    char *err = NULL;
    ccache cache = clrucache_create_full(8, ccol_string, ccol_string, NULL,
                                         NULL, NULL, &g_lru_sweep_procs, &err);
    _lru_disarm();
    if (cache != CCACHE_NULL)
      clrucache__destroy(cache);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(clrucache_oom, a_large_capacity_cache_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= 40; n++) {
    _lru_arm(n);
    ccache cache = clrucache_create_full(256, ccol_long_long, ccol_string, NULL,
                                         NULL, NULL, &g_lru_sweep_procs, NULL);
    _lru_disarm();
    if (cache != CCACHE_NULL) clrucache__destroy(cache);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(clrucache_oom, a_cache_built_under_a_late_failure_still_stores_and_reads) {
  bool built = false, works = false;
  for (int n = 12; n <= 60 && !built; n++) {
    _lru_arm(n);
    ccache cache = clrucache_create_full(4, ccol_int, ccol_int, NULL, NULL,
                                         NULL, &g_lru_sweep_procs, NULL);
    _lru_disarm();
    if (cache != CCACHE_NULL) {
      built = true;
      int key = 7, val = 42;
      cmap_pair kp = {}, vp = {};
      ccol__populate_cmap_pair(&kp, key);
      ccol__populate_cmap_pair(&vp, val);
      if (clrucache_set_full(cache, &kp, &vp) == ccol_success) {
        cmap_pair val_out = {};
        if (clrucache_get_full(cache, &kp, &val_out) == ccol_success &&
            val_out.ptr && val_out.size == sizeof(int)) {
          works = (*(int *)val_out.ptr == 42);
        }
        free(val_out.ptr);
      }
      clrucache__destroy(cache);
    }
  }
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(works);
}

// A remote store for keys 0 to 7: A value per key and whether the store holds
// it.
static int g_lru_remote_val[8];
static bool g_lru_remote_has[8];

// Stores the value of key in the remote store.
static bool _lru_remote_store_setter(const cmap_pair *key,
                                     const cmap_pair *val) {
  int k = 0, v = 0;
  memcpy(&k, key->ptr, sizeof(k));
  memcpy(&v, val->ptr, sizeof(v));
  g_lru_remote_val[k] = v;
  g_lru_remote_has[k] = true;
  return true;
}

// Gives the stored value of key, or fails when the store lacks it.
static bool _lru_remote_store_getter(const cmap_pair *key, cmap_pair *val) {
  int k = 0;
  memcpy(&k, key->ptr, sizeof(k));
  if (!g_lru_remote_has[k]) return false;
  int *v = malloc(sizeof(int));
  if (!v) return false;
  *v = g_lru_remote_val[k];
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// Reads key k through the cache (with the failing allocator disarmed) and tells
// whether the cache serves what the store holds, or nothing for a key the store
// lacks.
static bool _lru_cache_agrees_with_remote(ccache cache, int k) {
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair out = {0};
  ccol_retval r = clrucache_get_full(cache, &kp, &out);
  bool agrees;
  if (g_lru_remote_has[k]) {
    int got = 0;
    if (r == ccol_success && out.ptr && out.size == sizeof(int))
      memcpy(&got, out.ptr, sizeof(got));
    agrees = (r == ccol_success && got == g_lru_remote_val[k]);
  } else {
    agrees = (r == ccol_key_not_found);
  }
  free(out.ptr);
  return agrees;
}

// Fails each allocation of an update and of an insert in turn. After a remote
// setter stored a value, the cache must never keep serving the old one, which
// copying the value after the setter would allow.
TEST(clrucache_oom, a_failed_set_never_leaves_the_cache_older_than_the_remote) {
  memset(g_lru_remote_val, 0, sizeof(g_lru_remote_val));
  memset(g_lru_remote_has, 0, sizeof(g_lru_remote_has));
  ccache cache = clrucache_create_full(
      8, ccol_int, ccol_int, _lru_remote_store_getter, _lru_remote_store_setter,
      NULL, &g_lru_sweep_procs, NULL);
  REQUIRE_NE(cache, CCACHE_NULL);

  int key1 = 1, first = 100;
  cmap_pair kp1 = {.ptr = &key1, .size = sizeof(key1)};
  cmap_pair vp1 = {.ptr = &first, .size = sizeof(first)};
  ccol_retval r0 = clrucache_set_full(cache, &kp1, &vp1);

  bool agrees = true, saw_failure = false;
  for (int n = 1; n <= 12; n++) {
    int val = 200 + n;
    cmap_pair vp = {.ptr = &val, .size = sizeof(val)};
    _lru_arm(n);
    ccol_retval r = clrucache_set_full(cache, &kp1, &vp);
    _lru_disarm();
    if (r != ccol_success) saw_failure = true;
    if (!_lru_cache_agrees_with_remote(cache, 1)) agrees = false;

    if (n <= 6) {
      int key = n + 1, nval = 300 + n;
      cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
      cmap_pair nvp = {.ptr = &nval, .size = sizeof(nval)};
      _lru_arm(n);
      r = clrucache_set_full(cache, &kp, &nvp);
      _lru_disarm();
      if (r != ccol_success) saw_failure = true;
      if (!_lru_cache_agrees_with_remote(cache, key)) agrees = false;
    }
  }
  clrucache__destroy(cache);

  REQUIRE_EQ(r0, ccol_success);
  REQUIRE_TRUE(saw_failure);
  REQUIRE_TRUE(agrees);
}

// Each destroy loses its index to the forced push failure, so a table that
// never reclaims grows once per iteration.
TEST(clrucache_slot_table, a_lost_free_index_is_reclaimed_rather_than_growing) {
  {
    ccache_construct(warm, int, int, 8, NULL, NULL, NULL);
    ccache_destroy(warm);
  }
  size_t before = clrucache__slot_table_capacity_for_tests();

  bool all_built = true;
  for (int i = 0; i < 4; i++) {
    ccache_construct(cache, int, int, 8, NULL, NULL, NULL);
    if (!cache) all_built = false;
    clrucache__force_next_free_index_push_failure_for_tests();
    ccache_destroy(cache);
  }

  bool works = false;
  {
    ccache_construct(cache, int, int, 8, NULL, NULL, NULL);
    if (cache) {
      int k = 7, v = 99, out = 0;
      works = (ccache_set(cache, k, v) == ccol_success &&
               ccache_get(cache, k, &out) == ccol_success && out == 99);
    }
    ccache_destroy(cache);
  }

  size_t after = clrucache__slot_table_capacity_for_tests();

  REQUIRE_TRUE(all_built);
  REQUIRE_TRUE(works);
  REQUIRE_LE(after, before + 1);
}

// A key with padding between tag and id. CCACHE_TEST_COMPILER_CLEARS_PADDING is
// defined when the compiler has __builtin_clear_padding, which the macros need
// to clear the padding of their copy.
typedef struct {
  char tag;
  long id;
} ccache_padded_key;

#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define CCACHE_TEST_COMPILER_CLEARS_PADDING 1
#endif
#endif

#ifdef CCACHE_TEST_COMPILER_CLEARS_PADDING
// The calls of ccache_padded_getter and whether it saw a padding byte that
// isn't zero.
static _Atomic int ccache_padded_getter_calls = 0;
static _Atomic bool ccache_padded_getter_saw_dirty_padding = false;

// Answers every miss with -1, a value no set stores, so a miss that should have
// been a hit shows; it also records whether the key copy it got had dirty
// padding.
static bool ccache_padded_getter(const cmap_pair *key, cmap_pair *val) {
  atomic_fetch_add(&ccache_padded_getter_calls, 1);
  const unsigned char *b = (const unsigned char *)key->ptr;
  for (size_t j = offsetof(ccache_padded_key, tag) + 1;
       j < offsetof(ccache_padded_key, id); j++) {
    if (b[j] != 0) atomic_store(&ccache_padded_getter_saw_dirty_padding, true);
  }
  int *v = malloc(sizeof(int));
  if (!v) return false;
  *v = -1;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// Builds a key in storage whose padding holds fill and gives it back through a
// pointer the compiler can't see through, so a struct copy carries the fill.
static __attribute__((noinline)) const ccache_padded_key *ccache_padded_make(
    ccache_padded_key *storage, unsigned char fill, long id) {
  memset(storage, fill, sizeof(*storage));
  storage->tag = 'k';
  storage->id = id;
  const ccache_padded_key *p = storage;
  __asm__ volatile("" : "+r"(p) : : "memory");
  return p;
}
#endif

// The segment choice and the map read every byte of a struct key, so the macros
// must clear the padding of their copy. Without that, the set stores 0xAA
// padding, the get looks up 0x55 padding and every lookup misses.
TEST(padded_keys, macros_find_a_struct_key_whatever_its_padding_holds) {
#ifdef CCACHE_TEST_COMPILER_CLEARS_PADDING
  atomic_store(&ccache_padded_getter_calls, 0);
  atomic_store(&ccache_padded_getter_saw_dirty_padding, false);
  ccache_construct(cache, ccache_padded_key, int, 1024, ccache_padded_getter,
                   NULL, NULL);
  size_t segments = 0;
  (void)clrucache__segment_capacity_sum_for_tests(cache, &segments);

  ccache_padded_key storage;
  bool all_set = true;
  for (long i = 0; i < 100; i++) {
    int v = (int)i;
    if (ccache_set(cache, *ccache_padded_make(&storage, 0xAA, i), v) !=
        ccol_success)
      all_set = false;
  }
  int found = 0;
  for (long i = 0; i < 100; i++) {
    int out = -2;
    if (ccache_get(cache, *ccache_padded_make(&storage, 0x55, i), &out) ==
            ccol_success &&
        out == (int)i)
      found++;
  }
  int miss_out = 0;
  ccol_retval miss_r =
      ccache_get(cache, *ccache_padded_make(&storage, 0x5A, 5000), &miss_out);
  size_t size = clrucache_size(cache);
  ccache_destroy(cache);

  REQUIRE_GT(segments, (size_t)1);
  REQUIRE_TRUE(all_set);
  REQUIRE_EQ(found, 100);
  REQUIRE_EQ(miss_r, ccol_success);
  REQUIRE_EQ(miss_out, -1);
  REQUIRE_EQ(atomic_load(&ccache_padded_getter_calls), 1);
  REQUIRE_FALSE(atomic_load(&ccache_padded_getter_saw_dirty_padding));
  REQUIRE_EQ(size, (size_t)101);
#endif
}

// Under -Werror a macro that copied such an argument into a char * temporary
// fails with "discards const qualifier".
TEST(const_char_args, const_char_key_and_value_need_no_cast) {
  const char *const_key = "alpha";
  const char *const_val = "one";
  char *const pinned_key = "beta";

  ccache_construct(by_name, char *, int, 16, NULL, NULL, NULL);
  int v = 7, out = 0, out2 = 0;
  ccol_retval r_set = ccache_set(by_name, const_key, v);
  ccol_retval r_get = ccache_get(by_name, const_key, &out);
  ccol_retval r_set2 = ccache_set(by_name, pinned_key, 8);
  ccol_retval r_get2 = ccache_get(by_name, "beta", &out2);
  ccache_destroy(by_name);

  ccache_construct(names, int, char *, 16, NULL, NULL, NULL);
  int k = 1;
  char *s = NULL;
  ccol_retval r_set3 = ccache_set(names, k, const_val);
  ccol_retval r_get3 = ccache_get(names, k, &s);
  bool matches = s && strcmp(s, "one") == 0;
  free(s);
  ccache_destroy(names);

  REQUIRE_EQ(r_set, ccol_success);
  REQUIRE_EQ(r_get, ccol_success);
  REQUIRE_EQ(out, 7);
  REQUIRE_EQ(r_set2, ccol_success);
  REQUIRE_EQ(r_get2, ccol_success);
  REQUIRE_EQ(out2, 8);
  REQUIRE_EQ(r_set3, ccol_success);
  REQUIRE_EQ(r_get3, ccol_success);
  REQUIRE_TRUE(matches);
}

// A getter that waited on a set that failed. The remote store holds 100 for
// every key, and failset_remote_getter counts its calls.
static _Atomic int failset_getter_calls = 0;
// Gives 100 for any key and counts the call.
static bool failset_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  atomic_fetch_add(&failset_getter_calls, 1);
  int *v = malloc(sizeof(int));
  if (!v) return false;
  *v = 100;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

// failset_remote_setter: Once armed, a set of failset_gated_key holds until
// released and fails; every other set succeeds.
static _Atomic bool failset_armed = false;
static _Atomic int failset_gated_key = 0;
static _Atomic bool failset_setter_entered = false;
static _Atomic bool failset_setter_release = false;
// Holds and fails a set of the armed key, as described above.
static bool failset_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)val;
  int k;
  memcpy(&k, key->ptr, sizeof(k));
  if (!atomic_load(&failset_armed) || k != atomic_load(&failset_gated_key))
    return true;
  atomic_store(&failset_setter_entered, true);
  (void)ccache_await_flag(&failset_setter_release);
  return false;
}

// A setter thread of the failed set tests.
typedef struct {
  ccache cache;
  int key;
  int val;
  ccol_retval retval;
} failset_setter_arg_t;

// Sets key to val and stores the result.
static void *failset_setter_thread(void *arg) {
  failset_setter_arg_t *a = (failset_setter_arg_t *)arg;
  ccache cache = a->cache;
  ccache_redeclare(cache, int, int);
  a->retval = ccache_set(cache, a->key, a->val);
  return NULL;
}

// A getter thread that goes through clrucache_get_full, keeping the copy.
typedef struct {
  ccache cache;
  int key;
  cmap_pair out;
  ccol_retval retval;
} full_waiter_arg_t;

// Gets the key and stores the result and the copy.
static void *full_waiter_thread(void *arg) {
  full_waiter_arg_t *a = (full_waiter_arg_t *)arg;
  cmap_pair kp = {.ptr = &a->key, .size = sizeof(a->key)};
  a->retval = clrucache_get_full(a->cache, &kp, &a->out);
  return NULL;
}

// Resets the failed set fixture for a test that gates gated_key.
static void failset_reset(int gated_key) {
  atomic_store(&failset_getter_calls, 0);
  atomic_store(&failset_armed, false);
  atomic_store(&failset_gated_key, gated_key);
  atomic_store(&failset_setter_entered, false);
  atomic_store(&failset_setter_release, false);
}

// A get coalesced onto a set that creates the key, and the set fails. Nobody
// asked the remote getter, so the get must go on as a miss instead of reporting
// ccol_key_not_found. Both get paths are covered.
TEST(failed_set_waiter, getter_on_a_new_key_runs_the_remote_getter) {
  for (int use_full = 0; use_full < 2; use_full++) {
    failset_reset(1);
    ccache_construct(cache, int, int, 16, failset_remote_getter,
                     failset_remote_setter, NULL);
    atomic_store(&failset_armed, true);

    failset_setter_arg_t sarg = {cache, 1, 200, ccol_success};
    pthread_t stid;
    bool setter_started =
        pthread_create(&stid, NULL, failset_setter_thread, &sarg) == 0;
    bool entered = setter_started && ccache_await_flag(&failset_setter_entered);

    waiter_arg_t warg = {cache, 1, 0, -1, ccol_invalid_args, false};
    full_waiter_arg_t farg = {cache, 1, {NULL, 0}, ccol_invalid_args};
    pthread_t wtid;
    bool waiter_started = false;
    if (entered)
      waiter_started =
          pthread_create(&wtid, NULL,
                         use_full ? full_waiter_thread : waiter_thread,
                         use_full ? (void *)&farg : (void *)&warg) == 0;
    bool coalesced = waiter_started && failset_await_waiters(cache, 1, 2);

    atomic_store(&failset_setter_release, true);
    if (setter_started) pthread_join(stid, NULL);
    if (waiter_started) pthread_join(wtid, NULL);

    int after = -1;
    ccol_retval after_r = ccache_get(cache, 1, &after);
    int calls = atomic_load(&failset_getter_calls);
    ccache_destroy(cache);

    int got = -1;
    ccol_retval got_r = use_full ? farg.retval : warg.retval;
    if (use_full && farg.retval == ccol_success) {
      got = farg.out.size == sizeof(int) ? *(int *)farg.out.ptr : -1;
      free(farg.out.ptr);
    } else if (!use_full) {
      got = warg.result;
    }

    REQUIRE_TRUE(entered);
    REQUIRE_TRUE(coalesced);
    REQUIRE_EQ(sarg.retval, ccol_unexpected_failure);
    REQUIRE_EQ(got_r, ccol_success);
    REQUIRE_EQ(got, 100);
    REQUIRE_EQ(calls, 1);
    REQUIRE_EQ(after_r, ccol_success);
    REQUIRE_EQ(after, 100);
  }
}

// The same for a key the cache held: An insert evicts it while the set runs,
// then the set fails, and the waiter must ask the remote getter.
TEST(failed_set_waiter,
     getter_on_a_key_evicted_during_the_set_runs_the_getter) {
  failset_reset(1);
  ccache_construct(cache, int, int, 1, failset_remote_getter,
                   failset_remote_setter, NULL);
  int k1 = 1, seed = 10;
  ccol_retval seed_r = ccache_set(cache, k1, seed);
  atomic_store(&failset_armed, true);

  failset_setter_arg_t sarg = {cache, 1, 200, ccol_success};
  pthread_t stid;
  bool setter_started =
      pthread_create(&stid, NULL, failset_setter_thread, &sarg) == 0;
  bool entered = setter_started && ccache_await_flag(&failset_setter_entered);

  waiter_arg_t warg = {cache, 1, 0, -1, ccol_invalid_args, false};
  pthread_t wtid;
  bool waiter_started = false;
  if (entered)
    waiter_started = pthread_create(&wtid, NULL, waiter_thread, &warg) == 0;
  bool coalesced = waiter_started && failset_await_waiters(cache, 1, 2);

  ccol_retval evict_r = ccol_invalid_args;
  if (coalesced) evict_r = ccache_set(cache, 2, 20);

  atomic_store(&failset_setter_release, true);
  if (setter_started) pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  int calls = atomic_load(&failset_getter_calls);
  ccache_destroy(cache);

  REQUIRE_EQ(seed_r, ccol_success);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(coalesced);
  REQUIRE_EQ(evict_r, ccol_success);
  REQUIRE_EQ(sarg.retval, ccol_unexpected_failure);
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 100);
  REQUIRE_EQ(calls, 1);
}

// A get coalesced onto a fetch that failed keeps that answer, since the getter
// already answered. failfetch_remote_getter holds until released and fails.
static _Atomic bool failfetch_entered = false;
static _Atomic bool failfetch_release = false;
static _Atomic int failfetch_calls = 0;
// Counts its call, holds until released and fails.
static bool failfetch_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  atomic_fetch_add(&failfetch_calls, 1);
  atomic_store(&failfetch_entered, true);
  (void)ccache_await_flag(&failfetch_release);
  return false;
}

TEST(failed_set_waiter, getter_on_a_failed_fetch_keeps_its_answer) {
  atomic_store(&failfetch_entered, false);
  atomic_store(&failfetch_release, false);
  atomic_store(&failfetch_calls, 0);
  ccache_construct(cache, int, int, 16, failfetch_remote_getter, NULL, NULL);

  waiter_arg_t first = {cache, 3, 0, -1, ccol_invalid_args, false};
  waiter_arg_t second = {cache, 3, 0, -1, ccol_invalid_args, false};
  pthread_t t1, t2;
  bool first_started = pthread_create(&t1, NULL, waiter_thread, &first) == 0;
  bool entered = first_started && ccache_await_flag(&failfetch_entered);
  bool second_started = false;
  if (entered)
    second_started = pthread_create(&t2, NULL, waiter_thread, &second) == 0;
  bool coalesced = second_started && failset_await_waiters(cache, 3, 2);

  atomic_store(&failfetch_release, true);
  if (first_started) pthread_join(t1, NULL);
  if (second_started) pthread_join(t2, NULL);
  int calls = atomic_load(&failfetch_calls);
  ccache_destroy(cache);

  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(coalesced);
  REQUIRE_EQ(first.retval, ccol_key_not_found);
  REQUIRE_EQ(second.retval, ccol_key_not_found);
  REQUIRE_EQ(calls, 1);
}

// An environment variable that is never set, so getenv() gives a NULL char *
// the compiler can't see coming.
#define CCACHE_NULL_TEST_UNSET_VAR "CCOL_CCACHE_TEST_SURELY_UNSET_VARIABLE"

// Runs probe in a forked child and gives the byte it reported through a pipe,
// or 250 when the child died (a NULL dereference) and -1 when the fork failed.
// valgrind can replace a child's exit status, so the pipe carries the answer.
static int ccache_null_run_probe(int (*probe)(void)) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    close(fds[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    unsigned char byte = (unsigned char)probe();
    ssize_t w = write(fds[1], &byte, 1);
    (void)w;
    test_child_exit(0);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return -1;
  }
  unsigned char byte = 0;
  ssize_t n = read(fds[0], &byte, 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return n == 1 ? byte : 250;
}

// Tells whether a NULL char * key gives ccol_invalid_args to both macros and
// leaves the cache as it was.
static int ccache_null_probe_key(void) {
  unsetenv(CCACHE_NULL_TEST_UNSET_VAR);
  ccache_construct(cache, char *, int, 8, NULL, NULL, NULL);
  ccol_retval rs = ccache_set(cache, "HOME", 1);
  int v = 0;
  ccol_retval rg = ccache_get(cache, getenv(CCACHE_NULL_TEST_UNSET_VAR), &v);
  ccol_retval rs2 = ccache_set(cache, getenv(CCACHE_NULL_TEST_UNSET_VAR), 2);
  size_t size = clrucache_size(cache);
  ccache_destroy(cache);
  return rs == ccol_success && rg == ccol_invalid_args && v == 0 &&
         rs2 == ccol_invalid_args && size == 1;
}

// Tells whether a NULL char * value gives ccol_invalid_args and stores
// nothing.
static int ccache_null_probe_value(void) {
  unsetenv(CCACHE_NULL_TEST_UNSET_VAR);
  ccache_construct(cache, int, char *, 8, NULL, NULL, NULL);
  ccol_retval rs = ccache_set(cache, 1, getenv(CCACHE_NULL_TEST_UNSET_VAR));
  size_t size = clrucache_size(cache);
  ccache_destroy(cache);
  return rs == ccol_invalid_args && size == 0;
}

// Without the NULL test in ccol__populate_cmap_pair both probes die with
// SIGSEGV inside strlen().
TEST(ccache_null_char_ptr, a_null_key_is_invalid_args) {
  REQUIRE_EQ(ccache_null_run_probe(ccache_null_probe_key), 1);
}

TEST(ccache_null_char_ptr, a_null_value_is_invalid_args) {
  REQUIRE_EQ(ccache_null_run_probe(ccache_null_probe_value), 1);
}

// ccache_destroy(a[--k]) must destroy and clear every cache; evaluating the
// argument twice would skip every second one.
TEST(ccache_macros, destroy_evaluates_its_argument_once) {
  ccache caches[2] = {clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                            NULL, NULL, NULL),
                      clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                            NULL, NULL, NULL)};
  bool created = caches[0] != CCACHE_NULL && caches[1] != CCACHE_NULL;
  int k = 2;
  while (k > 0) ccache_destroy(caches[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ(caches[0], CCACHE_NULL);
  REQUIRE_EQ(caches[1], CCACHE_NULL);
}

// Two allocators for the procs copy test: The first counts calls and live
// blocks, the poison one counts the calls that reach it. The test writes the
// poison procs over the caller's struct after the create, so any poison call
// means the cache read the caller's struct later.
static atomic_long _lru_pl_live;
static atomic_long _lru_pl_calls;
static atomic_long _lru_pl_poison_calls;

static void *_lru_pl_malloc(size_t n) {
  _lru_pl_calls++;
  void *p = malloc(n);
  if (p) _lru_pl_live++;
  return p;
}
static void _lru_pl_free(void *p) {
  if (p) _lru_pl_live--;
  free(p);
}
static void *_lru_pl_calloc(size_t a, size_t b) {
  _lru_pl_calls++;
  void *p = calloc(a, b);
  if (p) _lru_pl_live++;
  return p;
}
static void *_lru_pl_realloc(void *p, size_t n) {
  _lru_pl_calls++;
  void *q = realloc(p, n);
  if (!p && q) _lru_pl_live++;
  return q;
}
static void *_lru_pl_poison_malloc(size_t n) {
  _lru_pl_poison_calls++;
  return malloc(n);
}
static void _lru_pl_poison_free(void *p) {
  _lru_pl_poison_calls++;
  free(p);
}
static void *_lru_pl_poison_calloc(size_t a, size_t b) {
  _lru_pl_poison_calls++;
  return calloc(a, b);
}
static void *_lru_pl_poison_realloc(void *p, size_t n) {
  _lru_pl_poison_calls++;
  return realloc(p, n);
}

// Every later allocation and free, through every segment, must still reach the
// allocator named at create time.
TEST(custom_alloc, cache_does_not_read_the_procs_struct_of_the_caller_later) {
  const ccol_memmgmt_procs honest = {_lru_pl_malloc, _lru_pl_free,
                                     _lru_pl_calloc, _lru_pl_realloc};
  const ccol_memmgmt_procs poison = {_lru_pl_poison_malloc, _lru_pl_poison_free,
                                     _lru_pl_poison_calloc,
                                     _lru_pl_poison_realloc};
  ccol_memmgmt_procs *mp = malloc(sizeof(*mp));
  REQUIRE_NOT_NULL(mp);
  *mp = honest;
  _lru_pl_live = 0;
  _lru_pl_calls = 0;
  _lru_pl_poison_calls = 0;

  ccache cache = clrucache_create_full(256, ccol_long_long, ccol_long_long,
                                       NULL, NULL, NULL, mp, NULL);
  *mp = poison;
  long calls_at_overwrite = _lru_pl_calls;

  bool created = cache != CCACHE_NULL;
  size_t stored = 0, found = 0;
  if (created) {
    for (long long k = 0; k < 600; k++) {
      long long v = k * 3;
      cmap_pair kp = {&k, sizeof(k)}, vp = {&v, sizeof(v)};
      if (clrucache_set_full(cache, &kp, &vp) == ccol_success) stored++;
    }
    for (long long k = 344; k < 600; k++) {
      long long v = 0;
      cmap_pair kp = {&k, sizeof(k)};
      if (clrucache__get_into(cache, &kp, &v, sizeof(v)) == ccol_success &&
          v == k * 3)
        found++;
    }
    clrucache__destroy(cache);
  }
  long poison_calls = _lru_pl_poison_calls;
  long later_calls = _lru_pl_calls - calls_at_overwrite;
  long live = _lru_pl_live;
  free(mp);

  REQUIRE_TRUE(created);
  REQUIRE_EQ(stored, (size_t)600);
  REQUIRE_GT(found, (size_t)0);
  REQUIRE_EQ(poison_calls, 0L);
  REQUIRE_GT(later_calls, 0L);
  REQUIRE_EQ(live, 0L);
}

// The raw macros and the short names of the typed layer are one implementation
// under two names; each pair must behave the same.
TEST(ccache_aliases, raw_destroy_evaluates_its_argument_once) {
  ccache caches[2] = {clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                            NULL, NULL, NULL),
                      clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                            NULL, NULL, NULL)};
  bool created = caches[0] != CCACHE_NULL && caches[1] != CCACHE_NULL;
  int k = 2;
  while (k > 0) clrucache_destroy(caches[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ(caches[0], CCACHE_NULL);
  REQUIRE_EQ(caches[1], CCACHE_NULL);
}

TEST(ccache_aliases, size_and_capacity_follow_the_raw_layer) {
  ccache_construct(c, int, int, 16, NULL, NULL, NULL);
  for (int i = 0; i < 5; i++) ccache_set(c, i, i * 3);
  size_t typed_size = ccache_size(c);
  size_t raw_size = clrucache_size(c);
  size_t typed_cap = ccache_capacity(c);
  size_t raw_cap = clrucache_capacity(c);
  ccache_destroy(c);
  REQUIRE_EQ(typed_size, (size_t)5);
  REQUIRE_EQ(raw_size, (size_t)5);
  REQUIRE_EQ(typed_cap, raw_cap);
  REQUIRE_GE(typed_cap, (size_t)16);
  REQUIRE_EQ(c, CCACHE_NULL);
  REQUIRE_EQ(ccache_size(c), (size_t)0);
}
