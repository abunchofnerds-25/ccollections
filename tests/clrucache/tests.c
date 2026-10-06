#include <clrucache.h>
#include <fcntl.h>
#include <float.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

extern struct clrucache *_clrucache_resolve_for_tests(clru_cache h);
extern size_t _clrucache_slot_table_capacity_for_tests(void);
extern size_t _clrucache_free_index_count_for_tests(void);
extern size_t _clrucache_segment_capacity_sum_for_tests(clru_cache cache,
                                                        size_t *out_segments);
extern void clru_test_set_post_publish_delay_us(unsigned int delay_us);
extern bool clru_test_post_publish_delay_entered(void);

/* ========================================================================== */
/*                         BASIC OPERATIONS                                   */
/* ========================================================================== */

TEST(basic, set_and_get_int_key_int_value) {
  clru_construct(cache, int, int, 10, NULL, NULL, NULL);

  int k = 42, v = 100;
  ccol_retval_t r = clru_set(cache, k, v);
  REQUIRE_EQ(r, ccol_success);

  int out = 0;
  r = clru_get(cache, k, &out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(out, 100);

  clru_destroy(cache);
}

TEST(basic, char_ptr_get_miss_does_not_modify_output) {
  clru_construct(cache, int, char *, 8, NULL, NULL, NULL);

  char sentinel_buf[1] = {0};
  char *out = sentinel_buf;
  REQUIRE_EQ(clru_get(cache, 99, &out), ccol_key_not_found);
  REQUIRE_PTR_EQ(out, sentinel_buf); /* must be untouched on miss */

  clru_destroy(cache);
}

TEST(basic, get_missing_key_returns_not_found) {
  clru_construct(cache, int, int, 10, NULL, NULL, NULL);

  int out = 0;
  ccol_retval_t r = clru_get(cache, 99, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  clru_destroy(cache);
}

TEST(basic, overwrite_value) {
  clru_construct(cache, int, int, 10, NULL, NULL, NULL);

  int k = 1, v1 = 10, v2 = 20;
  clru_set(cache, k, v1);
  clru_set(cache, k, v2);

  int out = 0;
  clru_get(cache, k, &out);
  REQUIRE_EQ(out, 20);

  clru_destroy(cache);
}

TEST(basic, string_key_int_value) {
  clru_construct(cache, char *, int, 8, NULL, NULL, NULL);

  int v = 55;
  ccol_retval_t r = clru_set(cache, "hello", v);
  REQUIRE_EQ(r, ccol_success);

  int out = 0;
  r = clru_get(cache, "hello", &out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(out, 55);

  int miss = 0;
  r = clru_get(cache, "world", &miss);
  REQUIRE_EQ(r, ccol_key_not_found);

  clru_destroy(cache);
}

TEST(basic, size_and_capacity) {
  clru_construct(cache, int, int, 5, NULL, NULL, NULL);

  REQUIRE_EQ(clrucache_capacity(cache), (size_t)5);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  for (int i = 0; i < 3; i++) {
    clru_set(cache, i, i * 10);
  }
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  clru_destroy(cache);
}

TEST(basic, invalid_args_get) {
  clru_construct(cache, int, int, 4, NULL, NULL, NULL);

  int k = 1;
  cmap_pair kp = {};
  _populate_cmap_pair(&kp, k);
  cmap_pair val_out = {};

  REQUIRE_EQ(clrucache_get_full(CLRU_CACHE_INVALID, &kp, &val_out),
             ccol_invalid_args);
  REQUIRE_EQ(clrucache_get_full(cache, NULL, &val_out), ccol_invalid_args);
  REQUIRE_EQ(clrucache_get_full(cache, &kp, NULL), ccol_invalid_args);

  cmap_pair null_ptr_kp = {.ptr = NULL, .size = sizeof(int)};
  REQUIRE_EQ(clrucache_get_full(cache, &null_ptr_kp, &val_out),
             ccol_invalid_args);

  cmap_pair zero_size_kp = {.ptr = &k, .size = 0};
  REQUIRE_EQ(clrucache_get_full(cache, &zero_size_kp, &val_out),
             ccol_invalid_args);

  clru_destroy(cache);
}

TEST(basic, invalid_args_set) {
  clru_construct(cache, int, int, 4, NULL, NULL, NULL);

  int k = 1, v = 2;
  cmap_pair kp = {}, vp = {};
  _populate_cmap_pair(&kp, k);
  _populate_cmap_pair(&vp, v);

  REQUIRE_EQ(clrucache_set_full(CLRU_CACHE_INVALID, &kp, &vp),
             ccol_invalid_args);
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

  clru_destroy(cache);
}

TEST(basic, capacity_zero_returns_null) {
  char *err = NULL;
  clru_cache cache = clrucache_create_full(0, ccol_int, ccol_int, NULL, NULL,
                                           NULL, NULL, &err);
  REQUIRE_EQ(cache, CLRU_CACHE_INVALID);
  REQUIRE_NOT_NULL(err);
}

TEST(basic, null_cache_size_and_capacity_return_zero) {
  REQUIRE_EQ(clrucache_size(CLRU_CACHE_INVALID), (size_t)0);
  REQUIRE_EQ(clrucache_capacity(CLRU_CACHE_INVALID), (size_t)0);
}

TEST(basic, destroy_null_cache_is_noop) {
  clru_cache cache = CLRU_CACHE_INVALID;
  clru_destroy(cache); /* must not crash */
  REQUIRE_EQ(cache, CLRU_CACHE_INVALID);
}

TEST(basic, destroy_sets_handle_null) {
  clru_construct(cache, int, int, 4, NULL, NULL, NULL);
  clru_set(cache, 1, 10);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);
  clru_destroy(cache);
  REQUIRE_EQ(cache, CLRU_CACHE_INVALID);
}

TEST(basic, create_full_null_err_on_failure) {
  /* Passing NULL for err must not crash when creation fails */
  clru_cache cache = clrucache_create_full(0, ccol_int, ccol_int, NULL, NULL,
                                           NULL, NULL, NULL);
  REQUIRE_EQ(cache, CLRU_CACHE_INVALID);
}

TEST(basic, size_unchanged_on_overwrite) {
  clru_construct(cache, int, int, 10, NULL, NULL, NULL);

  int k = 7;
  clru_set(cache, k, 100);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  clru_set(cache, k, 200);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  int out = 0;
  clru_get(cache, k, &out);
  REQUIRE_EQ(out, 200);

  clru_destroy(cache);
}

/* ========================================================================== */
/*                         LRU EVICTION                                       */
/* ========================================================================== */

static int eviction_key_captured = -1;
static int eviction_val_captured = -1;
static int eviction_count = 0;

static void record_eviction(const cmap_pair *key, const cmap_pair *val) {
  eviction_key_captured = *(const int *)key->ptr;
  eviction_val_captured = *(const int *)val->ptr;
  eviction_count++;
}

static char str_eviction_key[64];
static char str_eviction_val[64];
static int str_eviction_count = 0;

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

  clru_construct(cache, int, int, 3, NULL, NULL, record_eviction);

  /* Insert keys 0,1,2; fills the cache */
  for (int i = 0; i < 3; i++) {
    clru_set(cache, i, i * 10);
  }
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  /* Access key 0 so it becomes MRU; key 1 is now LRU */
  int out = 0;
  clru_get(cache, 0, &out);

  /* Insert key 3; should evict key 1 (LRU) */
  clru_set(cache, 3, 30);

  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);
  REQUIRE_EQ(eviction_val_captured, 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  /* Key 1 should now be gone */
  ccol_retval_t r = clru_get(cache, 1, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  /* Keys 0, 2, 3 should still be present */
  clru_get(cache, 0, &out);
  REQUIRE_EQ(out, 0);
  clru_get(cache, 2, &out);
  REQUIRE_EQ(out, 20);
  clru_get(cache, 3, &out);
  REQUIRE_EQ(out, 30);

  clru_destroy(cache);
}

TEST(eviction, eviction_callback_called_on_destroy) {
  eviction_count = 0;

  clru_construct(cache, int, int, 10, NULL, NULL, record_eviction);
  for (int i = 0; i < 5; i++) {
    clru_set(cache, i, i);
  }

  clru_destroy(cache);
  REQUIRE_EQ(eviction_count, 5);
}

TEST(eviction, size_tracked_correctly_across_evictions) {
  eviction_count = 0;
  clru_construct(cache, int, int, 3, NULL, NULL, record_eviction);

  for (int i = 0; i < 3; i++) clru_set(cache, i, i);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(eviction_count, 0);

  /* Four more inserts, each must evict one */
  for (int i = 3; i < 7; i++) clru_set(cache, i, i);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(eviction_count, 4);

  clru_destroy(cache);
  /* Remaining 3 live entries evicted on destroy */
  REQUIRE_EQ(eviction_count, 7);
}

TEST(eviction, set_promotes_existing_entry_to_mru) {
  eviction_count = 0;
  eviction_key_captured = -1;

  clru_construct(cache, int, int, 3, NULL, NULL, record_eviction);

  /* Fill cache: LRU order after inserts is 0 (LRU) -> 1 -> 2 (MRU) */
  for (int i = 0; i < 3; i++) clru_set(cache, i, i * 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  /* Overwrite key 0 -> should move it to MRU; order becomes 1 (LRU) -> 2 -> 0
   */
  clru_set(cache, 0, 99);
  REQUIRE_EQ(eviction_count, 0);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  /* Insert key 3 -> must evict key 1 (new LRU), not key 0 */
  clru_set(cache, 3, 30);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_key_not_found);
  REQUIRE_EQ(clru_get(cache, 0, &out), ccol_success);
  REQUIRE_EQ(out, 99);
  REQUIRE_EQ(clru_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(clru_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 30);

  clru_destroy(cache);
}

/* This setter rejects exactly one key, which each test chooses. It rejects
   only after a test arms it, so a test can seed that key before it makes
   the key fail. The setter runs entirely on the thread that calls it, which
   is what makes the two tests below deterministic. Note that
   clrucache_set_full calls a remote setter with the mutex of the segment
   unlocked, but nothing here blocks. */
static int rejecting_setter_calls = 0;
static bool rejecting_setter_armed = false;
static int rejecting_setter_key = 0;
static bool rejecting_remote_setter(const cmap_pair *key,
                                    const cmap_pair *val) {
  (void)val;
  rejecting_setter_calls++;
  if (!rejecting_setter_armed) return true;
  return *(const int *)key->ptr != rejecting_setter_key;
}

TEST(eviction, failed_set_leaves_the_eviction_order_untouched) {
  /* A set that returns a failure must leave the cache unchanged for its key,
     which includes that key's place in the eviction order: an operation that
     stored nothing must not buy the key a reprieve at another key's expense.

     Capacity 3 with a single segment (a cache this small is never split), so
     the order below is exact rather than probabilistic. */
  eviction_count = 0;
  eviction_key_captured = -1;
  eviction_val_captured = -1;
  rejecting_setter_calls = 0;
  rejecting_setter_armed = false;
  rejecting_setter_key = 1;

  clru_construct(cache, int, int, 3, NULL, rejecting_remote_setter,
                 record_eviction);

  /* Fills the cache; order is 1 (LRU) -> 2 -> 3 (MRU), so key 1 is the next
     victim. */
  REQUIRE_EQ(clru_set(cache, 1, 10), ccol_success);
  REQUIRE_EQ(clru_set(cache, 2, 20), ccol_success);
  REQUIRE_EQ(clru_set(cache, 3, 30), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(eviction_count, 0);

  rejecting_setter_armed = true;

  /* The remote setter rejects this one. Nothing is stored, nothing is
     evicted, and key 1 stays the next victim. */
  REQUIRE_EQ(clru_set(cache, 1, 99), ccol_unexpected_failure);
  REQUIRE_EQ(eviction_count, 0);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  /* The victim, not the timing, is what this test is about: promoting key 1
     on the way out of the failed set makes key 2 the least-recently-used
     entry and evicts it here instead. */
  REQUIRE_EQ(clru_set(cache, 4, 40), ccol_success);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);
  REQUIRE_EQ(eviction_val_captured, 10);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_key_not_found);
  /* The failed set left key 1's own value alone too, right up to the point
     an ordinary eviction took it. */
  REQUIRE_EQ(clru_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(clru_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 30);
  REQUIRE_EQ(clru_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 40);

  clru_destroy(cache);
}

TEST(eviction, failed_set_does_not_move_a_mid_list_key_to_the_front) {
  /* The same rule as the test above, read off a key that is neither the next
     victim nor the most recently used one, so the whole order after it is
     what the assertions distinguish. Re-inserting a failed key at the
     most-recently-used end reverses the order of everything that was ahead
     of it, and the first eviction alone cannot tell that apart. */
  eviction_count = 0;
  eviction_key_captured = -1;
  rejecting_setter_calls = 0;
  rejecting_setter_armed = false;
  rejecting_setter_key = 2;

  clru_construct(cache, int, int, 3, NULL, rejecting_remote_setter,
                 record_eviction);

  /* Order is 1 (LRU) -> 2 -> 3 (MRU). */
  REQUIRE_EQ(clru_set(cache, 1, 10), ccol_success);
  REQUIRE_EQ(clru_set(cache, 2, 20), ccol_success);
  REQUIRE_EQ(clru_set(cache, 3, 30), ccol_success);

  rejecting_setter_armed = true;
  REQUIRE_EQ(clru_set(cache, 2, 99), ccol_unexpected_failure);
  REQUIRE_EQ(eviction_count, 0);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  rejecting_setter_armed = false;

  /* Key 1 is the next victim either way. */
  REQUIRE_EQ(clru_set(cache, 4, 40), ccol_success);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 1);

  /* This is the one that separates the two orders: key 2 is next only if the
     failed set left it where it was. Promoting it makes key 3 the victim
     here instead. */
  REQUIRE_EQ(clru_set(cache, 5, 50), ccol_success);
  REQUIRE_EQ(eviction_count, 2);
  REQUIRE_EQ(eviction_key_captured, 2);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 2, &out), ccol_key_not_found);
  REQUIRE_EQ(clru_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 30);
  REQUIRE_EQ(clru_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 40);
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(out, 50);

  clru_destroy(cache);
}

TEST(eviction, capacity_one_always_evicts) {
  eviction_count = 0;
  eviction_key_captured = -1;
  eviction_val_captured = -1;

  clru_construct(cache, int, int, 1, NULL, NULL, record_eviction);

  clru_set(cache, 10, 100);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(eviction_count, 0);

  clru_set(cache, 20, 200);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 10);
  REQUIRE_EQ(eviction_val_captured, 100);

  clru_set(cache, 30, 300);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(eviction_count, 2);
  REQUIRE_EQ(eviction_key_captured, 20);
  REQUIRE_EQ(eviction_val_captured, 200);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 10, &out), ccol_key_not_found);
  REQUIRE_EQ(clru_get(cache, 20, &out), ccol_key_not_found);
  REQUIRE_EQ(clru_get(cache, 30, &out), ccol_success);
  REQUIRE_EQ(out, 300);

  clru_destroy(cache);
}

TEST(eviction, callback_receives_correct_string_key_and_value) {
  str_eviction_count = 0;
  str_eviction_key[0] = '\0';
  str_eviction_val[0] = '\0';

  clru_construct(cache, char *, char *, 2, NULL, NULL, record_str_eviction);

  clru_set(cache, "key1", "val1");
  clru_set(cache, "key2", "val2");
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);
  REQUIRE_EQ(str_eviction_count, 0);

  /* key1 is LRU; inserting key3 must evict it */
  clru_set(cache, "key3", "val3");
  REQUIRE_EQ(str_eviction_count, 1);
  REQUIRE_STREQ(str_eviction_key, "key1");
  REQUIRE_STREQ(str_eviction_val, "val1");
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);

  char *out = NULL;
  REQUIRE_EQ(clru_get(cache, "key1", &out), ccol_key_not_found);
  REQUIRE_EQ(clru_get(cache, "key2", &out), ccol_success);
  REQUIRE_STREQ(out, "val2");
  free(out);

  out = NULL;
  REQUIRE_EQ(clru_get(cache, "key3", &out), ccol_success);
  REQUIRE_STREQ(out, "val3");
  free(out);

  clru_destroy(cache);
}

TEST(eviction, lru_full_order_after_multiple_gets) {
  eviction_key_captured = -1;
  eviction_count = 0;

  clru_construct(cache, int, int, 5, NULL, NULL, record_eviction);

  /* Fill cache; insertion order is 0..4, so 0 is LRU, 4 is MRU */
  for (int i = 0; i < 5; i++) clru_set(cache, i, i * 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)5);

  /* Promote 0 and 1 to MRU: order becomes 2(LRU) 3 4 0 1(MRU) */
  int out = 0;
  clru_get(cache, 0, &out);
  clru_get(cache, 1, &out);

  /* Insert key 5: must evict key 2 (current LRU) */
  clru_set(cache, 5, 50);
  REQUIRE_EQ(eviction_count, 1);
  REQUIRE_EQ(eviction_key_captured, 2);
  REQUIRE_EQ(clrucache_size(cache), (size_t)5);

  /* Insert key 6: must evict key 3 (new LRU) */
  clru_set(cache, 6, 60);
  REQUIRE_EQ(eviction_count, 2);
  REQUIRE_EQ(eviction_key_captured, 3);
  REQUIRE_EQ(clrucache_size(cache), (size_t)5);

  REQUIRE_EQ(clru_get(cache, 2, &out), ccol_key_not_found);
  REQUIRE_EQ(clru_get(cache, 3, &out), ccol_key_not_found);

  REQUIRE_EQ(clru_get(cache, 0, &out), ccol_success);
  REQUIRE_EQ(out, 0);
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 10);
  REQUIRE_EQ(clru_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 40);
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(out, 50);
  REQUIRE_EQ(clru_get(cache, 6, &out), ccol_success);
  REQUIRE_EQ(out, 60);

  clru_destroy(cache);
}

static int getter_eviction_key_seen = -1;
static int getter_eviction_val_seen = -1;
static int getter_eviction_count = 0;
static int double_key_getter_calls = 0;

/* Returns key * 2 (same formula as simple_remote_getter below, defined here
 * to avoid a forward-reference from the eviction section into remote_getter).
 */
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

static void record_getter_eviction(const cmap_pair *key, const cmap_pair *val) {
  getter_eviction_key_seen = *(const int *)key->ptr;
  getter_eviction_val_seen = *(const int *)val->ptr;
  getter_eviction_count++;
}

/*
 * Verify that the eviction callback receives the exact value that the getter
 * produced when the cache evicts an entry that the getter filled. It must not
 * receive the original key or garbage. This exercises the path where the
 * remote getter allocated entry->value on the heap, and not clru_set. The LRU
 * list then selects that entry as the victim.
 *
 * Capacity 3 is used so that inserting key 4 forces exactly one eviction (key
 * 1, the LRU) without cascading evictions when we later read the surviving
 * keys.
 */
TEST(eviction, eviction_callback_fires_for_getter_populated_entry) {
  getter_eviction_key_seen = -1;
  getter_eviction_val_seen = -1;
  getter_eviction_count = 0;
  double_key_getter_calls = 0;

  clru_construct(cache, int, int, 3, double_key_getter, NULL,
                 record_getter_eviction);

  /* Fetch keys 1, 2, 3 with the getter: values 2, 4, 6. The cache is full. */
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2);
  REQUIRE_EQ(clru_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 4);
  REQUIRE_EQ(clru_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 6);
  REQUIRE_EQ(double_key_getter_calls, 3);
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);
  REQUIRE_EQ(getter_eviction_count, 0);

  /* Promote key 2 to MRU; LRU order is now: 1 (LRU) -> 3 -> 2 (MRU) */
  REQUIRE_EQ(clru_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(double_key_getter_calls, 3); /* cache hit, no remote call */

  /* Set key 4 directly: must evict key 1 (LRU) */
  REQUIRE_EQ(clru_set(cache, 4, 99), ccol_success);
  REQUIRE_EQ(getter_eviction_count, 1);
  REQUIRE_EQ(getter_eviction_key_seen, 1);
  REQUIRE_EQ(getter_eviction_val_seen, 2); /* value that the getter produced */
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  /* Surviving keys 2, 3, 4 must be readable without new remote calls */
  REQUIRE_EQ(clru_get(cache, 2, &out), ccol_success);
  REQUIRE_EQ(out, 4);
  REQUIRE_EQ(clru_get(cache, 3, &out), ccol_success);
  REQUIRE_EQ(out, 6);
  REQUIRE_EQ(clru_get(cache, 4, &out), ccol_success);
  REQUIRE_EQ(out, 99);
  REQUIRE_EQ(double_key_getter_calls, 3); /* no additional remote calls */

  clru_destroy(cache);
}

/* ========================================================================== */
/*                         REMOTE GETTER                                      */
/* ========================================================================== */

static int remote_get_call_count = 0;

static bool simple_remote_getter(const cmap_pair *key, cmap_pair *val) {
  remote_get_call_count++;
  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k * 2; /* value = key * 2 */
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

static bool failing_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  return false; /* always fails */
}

static int retry_getter_call_count = 0;
static bool retry_remote_getter(const cmap_pair *key, cmap_pair *val) {
  retry_getter_call_count++;
  if (retry_getter_call_count == 1) return false; /* fail on first call */
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
  clru_construct(cache, int, int, 10, simple_remote_getter, NULL, NULL);

  int out = 0;
  ccol_retval_t r = clru_get(cache, 7, &out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(out, 14); /* 7 * 2 */
  REQUIRE_EQ(remote_get_call_count, 1);

  /* Second access should hit the cache (no remote call) */
  clru_get(cache, 7, &out);
  REQUIRE_EQ(remote_get_call_count, 1);

  clru_destroy(cache);
}

TEST(remote_getter, returns_not_found_on_getter_failure) {
  clru_construct(cache, int, int, 10, failing_remote_getter, NULL, NULL);

  int out = 0;
  ccol_retval_t r = clru_get(cache, 5, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  clru_destroy(cache);
}

TEST(remote_getter, val_out_populated) {
  remote_get_call_count = 0;
  clru_construct(cache, int, int, 10, simple_remote_getter, NULL, NULL);

  int k = 5;
  cmap_pair kp = {};
  _populate_cmap_pair(&kp, k);

  cmap_pair val_out = {};
  ccol_retval_t r = clrucache_get_full(cache, &kp, &val_out);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(*(int *)val_out.ptr, 10); /* 5 * 2 */
  REQUIRE_EQ(val_out.size, sizeof(int));
  free(val_out.ptr);

  clru_destroy(cache);
}

TEST(remote_getter, retry_after_failure_triggers_new_fetch) {
  retry_getter_call_count = 0;
  clru_construct(cache, int, int, 10, retry_remote_getter, NULL, NULL);

  int out = 0;
  /* First attempt: getter fails, placeholder is cleaned up */
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(retry_getter_call_count, 1);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  /* Second attempt: getter succeeds; must reach the remote (no stale
   * placeholder) */
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(retry_getter_call_count, 2);
  REQUIRE_EQ(out, 5);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Third attempt: hits cache, no remote call */
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(retry_getter_call_count, 2);

  clru_destroy(cache);
}

TEST(remote_getter, re_fetches_after_eviction) {
  remote_get_call_count = 0;
  clru_construct(cache, int, int, 1, simple_remote_getter, NULL, NULL);

  int out = 0;
  clru_get(cache, 10, &out);
  REQUIRE_EQ(out, 20); /* 10 * 2 */
  REQUIRE_EQ(remote_get_call_count, 1);

  /* Fetch a different key; evicts key=10 (only slot available) */
  clru_get(cache, 20, &out);
  REQUIRE_EQ(out, 40); /* 20 * 2 */
  REQUIRE_EQ(remote_get_call_count, 2);

  /* Fetch key=10 again; must call the remote getter (was evicted) */
  clru_get(cache, 10, &out);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(remote_get_call_count, 3);

  clru_destroy(cache);
}

static bool null_ptr_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  val->ptr = NULL;
  val->size = sizeof(int);
  return true; /* claims success but leaves ptr NULL */
}

static bool zero_size_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  val->ptr = malloc(sizeof(int)); /* allocate but report size 0 */
  val->size = 0;
  return true;
}

static bool false_getter_with_alloc(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  val->ptr = malloc(sizeof(int)); /* allocate memory but report failure */
  val->size = sizeof(int);
  return false;
}

TEST(remote_getter, getter_returns_true_with_null_ptr_treated_as_miss) {
  clru_construct(cache, int, int, 10, null_ptr_remote_getter, NULL, NULL);
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  clru_destroy(cache);
}

TEST(remote_getter, getter_returns_true_with_zero_size_treated_as_miss) {
  clru_construct(cache, int, int, 10, zero_size_remote_getter, NULL, NULL);
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  clru_destroy(cache);
}

TEST(remote_getter, getter_returns_false_with_allocated_ptr_no_leak) {
  /* Primary assertion: valgrind / make memtest catches the leak if the
   * cache fails to free val->ptr when the getter returns false. */
  clru_construct(cache, int, int, 10, false_getter_with_alloc, NULL, NULL);
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  clru_destroy(cache);
}

static bool oversized_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  /* Returns sizeof(long long)*2 bytes for a cache declared with int values.
   * sizeof(long long)*2 > sizeof(int) on all supported platforms. */
  void *buf = malloc(sizeof(long long) * 2);
  if (!buf) return false;
  val->ptr = buf;
  val->size = sizeof(long long) * 2;
  return true;
}

TEST(remote_getter, char_ptr_returns_not_found_on_getter_failure) {
  /* clru_get for char* values routes through clrucache_get_full (not
   * __clrucache_get_into).  Verify that a failing remote getter returns
   * ccol_key_not_found and leaves the caller's pointer untouched. */
  clru_construct(cache, int, char *, 10, failing_remote_getter, NULL, NULL);

  char sentinel[] = "unchanged";
  char *out = sentinel;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_PTR_EQ(out, sentinel); /* must not be modified on miss */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  clru_destroy(cache);
}

/*
 * The three tests below are the char*-value equivalents of
 * getter_returns_true_with_null_ptr_treated_as_miss,
 * getter_returns_true_with_zero_size_treated_as_miss, and
 * getter_returns_false_with_allocated_ptr_no_leak.  For char* value caches
 * clru_get routes through clrucache_get_full (not __clrucache_get_into), so
 * this exercises a distinct code path.
 */
TEST(remote_getter,
     char_ptr_getter_returns_true_with_null_ptr_treated_as_miss) {
  clru_construct(cache, int, char *, 10, null_ptr_remote_getter, NULL, NULL);

  char *out = NULL;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_NULL(out);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  clru_destroy(cache);
}

TEST(remote_getter,
     char_ptr_getter_returns_true_with_zero_size_treated_as_miss) {
  clru_construct(cache, int, char *, 10, zero_size_remote_getter, NULL, NULL);

  char *out = NULL;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_NULL(out);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  clru_destroy(cache);
}

TEST(remote_getter, char_ptr_getter_returns_false_with_allocated_ptr_no_leak) {
  /* Primary assertion: valgrind / make memtest catches the leak if the
   * cache fails to free val->ptr when the getter returns false (char* path). */
  clru_construct(cache, int, char *, 10, false_getter_with_alloc, NULL, NULL);

  char *out = NULL;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_key_not_found);
  REQUIRE_NULL(out);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  clru_destroy(cache);
}

TEST(remote_getter, getter_returns_oversized_value_no_crash) {
  clru_construct(cache, int, int, 10, oversized_getter, NULL, NULL);
  int out = 0;
  /* The oversized value must be rejected without a buffer overflow. */
  ccol_retval_t r = clru_get(cache, 5, &out);
  REQUIRE_EQ(r, ccol_unexpected_failure);
  /* The placeholder must be cleaned up: cache stays empty. */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  clru_destroy(cache);
}

static bool undersized_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  /* Returns a single byte for a cache declared with int values: smaller
   * than sizeof(int) on every supported platform. */
  void *buf = malloc(1);
  if (!buf) return false;
  memset(buf, 0x7A, 1);
  val->ptr = buf;
  val->size = 1;
  return true;
}

TEST(remote_getter,
     getter_returns_undersized_value_rejected_not_partially_copied) {
  clru_construct(cache, int, int, 10, undersized_getter, NULL, NULL);
  /* Poison out with a pattern that is easy to recognise and is not zero.
   * A silent partial copy of the undersized value, in place of a rejection,
   * would leave this sentinel in the high bytes. The copy does not touch
   * them. The test would then pass by coincidence on some platforms and
   * for some values. This is why the assertion below demands the whole int,
   * byte for byte. */
  int out = 0x11223344;
  ccol_retval_t r = clru_get(cache, 5, &out);
  REQUIRE_EQ(r, ccol_unexpected_failure);
  REQUIRE_EQ(out, 0x11223344); /* buf must be left completely untouched */
  /* A mis-sized value must not be cached at all. */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  clru_destroy(cache);
}

/* char* key with remote getter */

static int char_key_getter_call_count = 0;

/* Returns the strlen of the key string as the cached int value. */
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

static bool failing_char_key_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  return false;
}

/*
 * The char* key path exercises a different branch of _populate_cmap_pair. It
 * also exercises a different chmap key type (ccol_string) for the internal
 * map. Verify three things. The cache calls the remote getter exactly one
 * time for each unique string key. The cache keeps the value that the getter
 * gives, and serves later reads from the cache with no second fetch. And each
 * distinct string key starts its own independent fetch.
 */
TEST(remote_getter, char_ptr_key_fetches_and_caches_via_getter) {
  char_key_getter_call_count = 0;
  clru_construct(cache, char *, int, 8, strlen_value_getter, NULL, NULL);

  /* "hello" has 5 chars */
  int out = 0;
  REQUIRE_EQ(clru_get(cache, "hello", &out), ccol_success);
  REQUIRE_EQ(out, 5);
  REQUIRE_EQ(char_key_getter_call_count, 1);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Second access for "hello": cache hit, no remote call */
  out = 0;
  REQUIRE_EQ(clru_get(cache, "hello", &out), ccol_success);
  REQUIRE_EQ(out, 5);
  REQUIRE_EQ(char_key_getter_call_count, 1);

  /* Different key "world!" (6 chars): triggers a new fetch */
  out = 0;
  REQUIRE_EQ(clru_get(cache, "world!", &out), ccol_success);
  REQUIRE_EQ(out, 6);
  REQUIRE_EQ(char_key_getter_call_count, 2);
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);

  clru_destroy(cache);
}

/*
 * A failing remote getter with a char* key must return ccol_key_not_found and
 * leave the cache empty (placeholder cleaned up), same as for integral keys.
 */
TEST(remote_getter, char_ptr_key_failing_getter_returns_not_found) {
  clru_construct(cache, char *, int, 8, failing_char_key_getter, NULL, NULL);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, "anything", &out), ccol_key_not_found);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  clru_destroy(cache);
}

/* ========================================================================== */
/*                         REMOTE SETTER                                      */
/* ========================================================================== */

static bool remote_set_should_succeed = true;
static int remote_set_call_count = 0;
static int remote_set_last_key = -1;
static int remote_set_last_val = -1;

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

  clru_construct(cache, int, int, 10, NULL, recording_remote_setter, NULL);

  int k = 1, v = 42;
  ccol_retval_t r = clru_set(cache, k, v);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(remote_set_call_count, 1);
  REQUIRE_EQ(remote_set_last_key, 1);
  REQUIRE_EQ(remote_set_last_val, 42);

  int out = 0;
  clru_get(cache, k, &out);
  REQUIRE_EQ(out, 42);

  clru_destroy(cache);
}

TEST(sync_setter, failure_does_not_update_cache) {
  remote_set_should_succeed = false;
  remote_set_call_count = 0;

  clru_construct(cache, int, int, 10, NULL, recording_remote_setter, NULL);

  int k = 1, v = 99;
  ccol_retval_t r = clru_set(cache, k, v);
  REQUIRE_EQ(r, ccol_unexpected_failure);

  int out = 0;
  r = clru_get(cache, k, &out);
  REQUIRE_EQ(r, ccol_key_not_found);

  clru_destroy(cache);
}

TEST(sync_setter, successive_overwrites_with_remote_setter) {
  remote_set_should_succeed = true;
  remote_set_call_count = 0;

  clru_construct(cache, int, int, 10, NULL, recording_remote_setter, NULL);

  int k = 3;
  /* First set: placeholder created, remote called, entry becomes LIVE */
  clru_set(cache, k, 10);
  REQUIRE_EQ(remote_set_call_count, 1);
  REQUIRE_EQ(remote_set_last_val, 10);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Second set on the same key: entry already LIVE (created_new=false),
   * remote setter called again, value overwritten in place */
  clru_set(cache, k, 20);
  REQUIRE_EQ(remote_set_call_count, 2);
  REQUIRE_EQ(remote_set_last_val, 20);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, k, &out), ccol_success);
  REQUIRE_EQ(out, 20);

  clru_destroy(cache);
}

TEST(sync_setter, failure_on_new_key_followed_by_success) {
  remote_set_should_succeed = false;
  remote_set_call_count = 0;

  clru_construct(cache, int, int, 10, NULL, recording_remote_setter, NULL);

  /* First set fails: placeholder must be removed entirely */
  int k = 42, v1 = 11;
  REQUIRE_EQ(clru_set(cache, k, v1), ccol_unexpected_failure);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);
  int out = 0;
  REQUIRE_EQ(clru_get(cache, k, &out), ccol_key_not_found);

  /* Second set on the same key must succeed and produce a live entry */
  remote_set_should_succeed = true;
  int v2 = 22;
  REQUIRE_EQ(clru_set(cache, k, v2), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  REQUIRE_EQ(clru_get(cache, k, &out), ccol_success);
  REQUIRE_EQ(out, v2);

  clru_destroy(cache);
}

TEST(sync_setter, failure_preserves_existing_value) {
  remote_set_should_succeed = true;
  clru_construct(cache, int, int, 10, NULL, recording_remote_setter, NULL);

  /* First set succeeds */
  int k = 5, v1 = 10;
  clru_set(cache, k, v1);

  /* Second set fails */
  remote_set_should_succeed = false;
  int v2 = 20;
  ccol_retval_t r = clru_set(cache, k, v2);
  REQUIRE_EQ(r, ccol_unexpected_failure);

  /* Old value should still be there */
  int out = 0;
  clru_get(cache, k, &out);
  REQUIRE_EQ(out, 10);

  clru_destroy(cache);
}

/* char* key with remote setter */

static bool char_key_setter_should_succeed = true;
static int char_key_setter_call_count = 0;
static char char_key_setter_last_key[64];
static int char_key_setter_last_val = -1;

static bool recording_char_key_setter(const cmap_pair *key,
                                      const cmap_pair *val) {
  char_key_setter_call_count++;
  strncpy(char_key_setter_last_key, (const char *)key->ptr,
          sizeof(char_key_setter_last_key) - 1);
  char_key_setter_last_val = *(const int *)val->ptr;
  return char_key_setter_should_succeed;
}

/*
 * A remote setter for a char* key that succeeds must call through to the
 * remote. It must receive the correct key string and value. It must also
 * update the cache, so that clru_get then reads the entry back.
 */
TEST(sync_setter, char_ptr_key_setter_success) {
  char_key_setter_should_succeed = true;
  char_key_setter_call_count = 0;
  char_key_setter_last_key[0] = '\0';
  char_key_setter_last_val = -1;

  clru_construct(cache, char *, int, 8, NULL, recording_char_key_setter, NULL);

  REQUIRE_EQ(clru_set(cache, "mykey", 77), ccol_success);
  REQUIRE_EQ(char_key_setter_call_count, 1);
  REQUIRE_STREQ(char_key_setter_last_key, "mykey");
  REQUIRE_EQ(char_key_setter_last_val, 77);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, "mykey", &out), ccol_success);
  REQUIRE_EQ(out, 77);

  clru_destroy(cache);
}

/*
 * The cache must clean the placeholder up completely when the remote setter
 * fails for a char* key that is new. The cache then stays empty, and a later
 * get returns ccol_key_not_found.
 */
TEST(sync_setter, char_ptr_key_setter_failure_leaves_cache_empty) {
  char_key_setter_should_succeed = false;
  char_key_setter_call_count = 0;

  clru_construct(cache, char *, int, 8, NULL, recording_char_key_setter, NULL);

  REQUIRE_EQ(clru_set(cache, "mykey", 88), ccol_unexpected_failure);
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, "mykey", &out), ccol_key_not_found);

  clru_destroy(cache);
}

/* ========================================================================== */
/*                         CONCURRENT GETTERS - KEY COALESCING                */
/* ========================================================================== */

/* A counted start gate. It serves the tests where several threads must reach
 * clru_get at the same instant, so that they race for one key.
 *
 * A pthread_barrier cannot do this. Its participant count is fixed when the
 * test creates it. A pthread_create that fails part way therefore parks every
 * thread that DID start on a barrier that nothing can satisfy. Nothing joins
 * those threads, and they hold the fixture open. This gate counts only the
 * threads that really started, which keeps that case an ordinary failed
 * assertion.
 *
 * A waiter parks with a short sleep, and not with sched_yield. Under valgrind
 * only one thread runs at a time, and sched_yield does not reliably hand the
 * scheduler over. A spin on a yield therefore burns whole quanta while the
 * thread that it depends on cannot run. */
typedef struct {
  _Atomic int ready;
  _Atomic bool go;
} clru_start_gate_t;

static void clru_gate_park(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
  nanosleep(&ts, NULL);
}

/* Called by each worker just before the operation under test. */
static void clru_gate_arrive(clru_start_gate_t *g) {
  if (!g) return;
  atomic_fetch_add_explicit(&g->ready, 1, memory_order_release);
  while (!atomic_load_explicit(&g->go, memory_order_acquire)) clru_gate_park();
}

/* The thread that creates the workers calls this after it knows how many
 * workers really started. It must release the gate on every path out. Without
 * that, a worker parks here with nothing left to wake it. */
static void clru_gate_release(clru_start_gate_t *g, int started) {
  while (atomic_load_explicit(&g->ready, memory_order_acquire) < started)
    clru_gate_park();
  atomic_store_explicit(&g->go, true, memory_order_release);
}

typedef struct {
  clru_cache cache;
  int key;
  int result;
  ccol_retval_t retval;
  /* The worker waits on this gate, when it is not NULL, immediately before
   * it calls clru_get. The call sites below, where several threads race for
   * the same key, show why this gate is needed. Every thread must contend
   * for the mutex of the cache before the fetch of the first thread ends.
   * A fixed sleep inside the remote getter cannot reliably do that.
   * This field is NULL for every use with one thread, and for every use with
   * distinct keys, because no such race exists there. NULL is the default
   * for any aggregate initializer that holds fewer values than members. */
  clru_start_gate_t *start_gate;
} getter_arg_t;

static _Atomic int coalesce_getter_calls = 0;

static bool slow_remote_getter(const cmap_pair *key, cmap_pair *val) {
  /* This uses atomic_fetch_add, which is the C11 <stdatomic.h> API. It does
   * not use the GCC and Clang __atomic_fetch_add builtin. That builtin needs
   * a pointer to a plain object with no _Atomic qualifier under Clang. That
   * is stricter than what GCC demands for the same builtin. The declared
   * type of coalesce_getter_calls is _Atomic int. That matches every other
   * access to it below, which all go through atomic_load(). */
  atomic_fetch_add(&coalesce_getter_calls, 1);

  /* Simulate a slow remote call */
  usleep(100000); /* 100 ms */

  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k + 1000;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

static void *getter_thread(void *arg) {
  getter_arg_t *ga = (getter_arg_t *)arg;
  clru_cache cache = ga->cache;
  clru_redeclare(cache, int, int);
  clru_gate_arrive(ga->start_gate);
  ga->retval = clru_get(cache, ga->key, &ga->result);
  return NULL;
}

TEST(concurrency, multiple_getters_coalesce_to_single_remote_fetch) {
  coalesce_getter_calls = 0;
  clru_construct(cache, int, int, 16, slow_remote_getter, NULL, NULL);

#define N_THREADS 8
  clru_start_gate_t gate = {0};
  getter_arg_t args[N_THREADS];
  pthread_t tids[N_THREADS];
  int created = 0;
  for (int i = 0; i < N_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 42;
    args[i].result = 0;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_gate = &gate;
    /* A partial failure here must not let the join loop below join an
     * uninitialized tids[i] slot. That is undefined behavior, and it can
     * hang on garbage pthread_t data. This is why the loop joins only the
     * threads that it really created. */
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  /* The release comes before the assertion, and not after it. A partial
   * start must still let every thread that did start finish, so that the
   * loop can join it. Without that, the assertion below returns with those
   * threads parked and the cache leaked. */
  clru_gate_release(&gate, created);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  REQUIRE_EQ(created, (int)N_THREADS);

  /* Cleanup runs unconditionally before the assertions below: REQUIRE_*
   * returns from this function immediately on the first failure, which
   * would otherwise leak the cache. */
  int calls = coalesce_getter_calls;
  ccol_retval_t retvals[N_THREADS];
  int results[N_THREADS];
  for (int i = 0; i < N_THREADS; i++) {
    retvals[i] = args[i].retval;
    results[i] = args[i].result;
  }
  clru_destroy(cache);

  /* Only one remote call should have been made */
  REQUIRE_EQ(calls, 1);

  /* All threads should have received the correct value */
  for (int i = 0; i < N_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_success);
    REQUIRE_EQ(results[i], 1042); /* key 42 + 1000 */
  }
#undef N_THREADS
}

/* ========================================================================== */
/*                  CONCURRENT GETTERS WAIT FOR ACTIVE SETTER                 */
/* ========================================================================== */

/* Every wait on a flag another thread sets is bounded. An unbounded spin turns
   an ordinary regression into a hang of the whole binary rather than a failure
   of one test, and the thread being waited for can simply be slow under
   valgrind or on a loaded runner. The bound is a hang-safety net and never the
   property under test: each caller reports whether the state was reached, and
   the assertions run only after every started thread has been joined.

   Parks on usleep rather than sched_yield, for the reason this suite's other
   drain loops already give: under valgrind one thread runs at a time and a
   yield-spin burns whole quanta while the thread it waits for cannot run. */
#define CLRU_GATE_WAIT_MS 30000

static bool clru_await_flag(_Atomic bool *flag) {
  for (int waited_ms = 0; waited_ms < CLRU_GATE_WAIT_MS; waited_ms++) {
    if (atomic_load(flag)) return true;
    usleep(1000);
  }
  return atomic_load(flag);
}

static _Atomic bool setter_started = false;
static _Atomic bool setter_may_finish = false;
static volatile bool setter_should_fail = false;
static volatile int setter_key_seen = -1;
static volatile int setter_val_seen = -1;

static bool gating_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  setter_key_seen = *(const int *)key->ptr;
  setter_val_seen = *(const int *)val->ptr;
  atomic_store(&setter_started, true);
  /* Bounded, like every other wait here: a test that returns before releasing
     this gate must not park the thread forever, because nothing would then
     join it and a later test's own release would wake it into a dead frame. */
  (void)clru_await_flag(&setter_may_finish);
  return !setter_should_fail;
}

typedef struct {
  clru_cache cache;
  int key;
  int expect_val;
  int result;
  ccol_retval_t retval;
  _Atomic bool done;
} waiter_arg_t;

static void *waiter_thread(void *arg) {
  waiter_arg_t *wa = (waiter_arg_t *)arg;
  clru_cache cache = wa->cache;
  clru_redeclare(cache, int, int);
  wa->retval = clru_get(cache, wa->key, &wa->result);
  atomic_store(&wa->done, true);
  return NULL;
}

typedef struct {
  clru_cache cache;
  int key;
  int val;
} sync_setter_arg_t;

static void *sync_setter_thread(void *arg) {
  sync_setter_arg_t *sa = (sync_setter_arg_t *)arg;
  clru_cache cache = sa->cache;
  clru_redeclare(cache, int, int);
  int k = sa->key, v = sa->val;
  clru_set(cache, k, v);
  return NULL;
}

TEST(concurrency, getters_wait_for_sync_setter) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  clru_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg = {cache, 10, 99};
  pthread_t stid;
  REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);

  /* Wait until setter has started the remote call */
  (void)clru_await_flag(&setter_started);

  /* Start a getter that should block until setter finishes */
  waiter_arg_t warg = {cache, 10, 99, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  /* Give getter time to start waiting */
  /* The test captures this value here, and asserts on it later. A REQUIRE_
     that fails between the start of these threads and their release returns
     from the test on the spot. A regression in the blocking contract
     produces exactly that. The setter would stay parked with nothing left to
     join it. The teardown of the fixture would then run under both threads
     while they still run, and a release in a later test would wake the
     orphan into a dead frame. */
  int done_while_blocked = -1;
  if (waiter_started) {
    usleep(50000);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  /* Let the setter finish */
  atomic_store(&setter_may_finish, true);
  pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  clru_destroy(cache);

  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 99);
}

/*
 * A getter that arrives while a setter is updating an EXISTING key should block
 * and then receive the NEW value once the setter succeeds.  This exercises the
 * set_in_progress wait path for an entry that already had a value.
 */
TEST(concurrency, getter_blocked_by_setter_for_existing_key_succeeds) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  clru_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  /* Phase 1: seed key 55 = 550 using the gating setter (let it complete) */
  sync_setter_arg_t sarg1 = {cache, 55, 550};
  pthread_t stid1;
  REQUIRE_EQ(pthread_create(&stid1, NULL, sync_setter_thread, &sarg1), 0);
  (void)clru_await_flag(&setter_started);
  atomic_store(&setter_may_finish, true);
  pthread_join(stid1, NULL);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Phase 2: overwrite key 55 = 999 while blocking a concurrent getter */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);

  sync_setter_arg_t sarg2 = {cache, 55, 999};
  pthread_t stid2;
  REQUIRE_EQ(pthread_create(&stid2, NULL, sync_setter_thread, &sarg2), 0);
  (void)clru_await_flag(&setter_started);

  /* Getter arrives while the setter holds the set_in_progress flag */
  waiter_arg_t warg = {cache, 55, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  /* Confirm the getter is blocked */
  /* The test captures this value here, and asserts on it later. A REQUIRE_
     that fails between the start of these threads and their release returns
     from the test on the spot. A regression in the blocking contract
     produces exactly that. The setter would stay parked with nothing left to
     join it. The teardown of the fixture would then run under both threads
     while they still run, and a release in a later test would wake the
     orphan into a dead frame. */
  int done_while_blocked = -1;
  if (waiter_started) {
    usleep(50000);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  /* Release the setter */
  atomic_store(&setter_may_finish, true);
  pthread_join(stid2, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  size_t size_after = clrucache_size(cache);
  clru_destroy(cache);

  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  /* Getter must have received the new value, not the old one */
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 999);
  REQUIRE_EQ(size_after, (size_t)1);
}

/*
 * A getter blocks while a setter runs for a BRAND-NEW key. That getter must
 * receive ccol_key_not_found when the setter fails. There is no old value to
 * fall back on, and the cache cleans the placeholder up.
 */
TEST(concurrency, getter_sees_key_not_found_when_new_key_setter_fails) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  clru_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg = {cache, 77, 777};
  pthread_t stid;
  REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);

  (void)clru_await_flag(&setter_started);

  /* Getter arrives while the setter placeholder is live */
  waiter_arg_t warg = {cache, 77, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  /* Give the getter time to enter the wait loop */
  /* The test captures this value here, and asserts on it later. A REQUIRE_
     that fails between the start of these threads and their release returns
     from the test on the spot. A regression in the blocking contract
     produces exactly that. The setter would stay parked with nothing left to
     join it. The teardown of the fixture would then run under both threads
     while they still run, and a release in a later test would wake the
     orphan into a dead frame. */
  int done_while_blocked = -1;
  if (waiter_started) {
    usleep(50000);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  atomic_store(&setter_may_finish, true);
  pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  size_t size_after = clrucache_size(cache);
  clru_destroy(cache);

  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  /* No old value exists: getter must return key_not_found */
  REQUIRE_EQ(warg.retval, ccol_key_not_found);
  /* Placeholder was cleaned up: cache must be empty */
  REQUIRE_EQ(size_after, (size_t)0);
}

/*
 * A getter blocked while a setter overwrites an EXISTING key should receive
 * the previous value when the setter fails (old value is preserved).
 */
TEST(concurrency, getter_gets_old_value_when_setter_fails_for_existing_key) {
  /* Phase 1: establish key 33 = 330 with a successful set */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  clru_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg1 = {cache, 33, 330};
  pthread_t stid1;
  REQUIRE_EQ(pthread_create(&stid1, NULL, sync_setter_thread, &sarg1), 0);
  (void)clru_await_flag(&setter_started);
  atomic_store(&setter_may_finish, true);
  pthread_join(stid1, NULL);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Phase 2: attempt to overwrite 330 with 999; setter will fail */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  sync_setter_arg_t sarg2 = {cache, 33, 999};
  pthread_t stid2;
  REQUIRE_EQ(pthread_create(&stid2, NULL, sync_setter_thread, &sarg2), 0);
  (void)clru_await_flag(&setter_started);

  /* Getter arrives while the failing setter is in progress */
  waiter_arg_t warg = {cache, 33, 330, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  /* The test captures this value here, and asserts on it later. A REQUIRE_
     that fails between the start of these threads and their release returns
     from the test on the spot. A regression in the blocking contract
     produces exactly that. The setter would stay parked with nothing left to
     join it. The teardown of the fixture would then run under both threads
     while they still run, and a release in a later test would wake the
     orphan into a dead frame. */
  int done_while_blocked = -1;
  if (waiter_started) {
    usleep(50000);
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  atomic_store(&setter_may_finish, true);
  pthread_join(stid2, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  size_t size_after = clrucache_size(cache);
  clru_destroy(cache);

  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);

  /* Old value must survive the failed overwrite */
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 330);
  REQUIRE_EQ(size_after, (size_t)1);
}

/*
 * Many getters block while a setter for a key that exists runs. ALL of them
 * must receive the old value when the setter fails, and not only the first
 * one that wakes up.
 */
TEST(concurrency, multiple_getters_see_old_value_when_setter_fails) {
  /* Phase 1: seed key 42 = 420 */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;

  clru_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg1 = {cache, 42, 420};
  pthread_t stid1;
  REQUIRE_EQ(pthread_create(&stid1, NULL, sync_setter_thread, &sarg1), 0);
  (void)clru_await_flag(&setter_started);
  atomic_store(&setter_may_finish, true);
  pthread_join(stid1, NULL);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Phase 2: failing overwrite of key 42 */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  sync_setter_arg_t sarg2 = {cache, 42, 999};
  pthread_t stid2;
  REQUIRE_EQ(pthread_create(&stid2, NULL, sync_setter_thread, &sarg2), 0);
  (void)clru_await_flag(&setter_started);

#define N_OLD_VAL_WAITERS 4
  waiter_arg_t wargs[N_OLD_VAL_WAITERS];
  pthread_t wtids[N_OLD_VAL_WAITERS];
  int wtids_created = 0;
  for (int i = 0; i < N_OLD_VAL_WAITERS; i++) {
    wargs[i] = (waiter_arg_t){cache, 42, 0, 0, ccol_unexpected_failure, false};
    /* A partial failure here must not let the join loop below join an
     * uninitialized wtids[i] slot. */
    if (pthread_create(&wtids[i], NULL, waiter_thread, &wargs[i]) != 0) break;
    wtids_created++;
  }
  /* The test counts and captures here. It never asserts before the release
     below. A REQUIRE_ that fails here returns from the test with the setter
     still parked, and with every getter that started still blocked on it.
     Nothing joins them, and the teardown of the fixture runs underneath
     them. The count that really started bounds this loop, and never
     N_OLD_VAL_WAITERS. */
  int still_blocked = 0;
  if (wtids_created > 0) {
    usleep(50000); /* give all getters time to enter the wait loop */
    for (int i = 0; i < wtids_created; i++) {
      if (atomic_load(&wargs[i].done) == 0) still_blocked++;
    }
  }

  atomic_store(&setter_may_finish, true);
  pthread_join(stid2, NULL);
  for (int i = 0; i < wtids_created; i++) {
    pthread_join(wtids[i], NULL);
  }

  int got_old_value = 0;
  for (int i = 0; i < wtids_created; i++) {
    if (wargs[i].retval == ccol_success && wargs[i].result == 420)
      got_old_value++;
  }
  size_t size_after = clrucache_size(cache);
  clru_destroy(cache);

  REQUIRE_EQ(wtids_created, (int)N_OLD_VAL_WAITERS);
  REQUIRE_EQ(still_blocked, (int)N_OLD_VAL_WAITERS);
  REQUIRE_EQ(got_old_value, (int)N_OLD_VAL_WAITERS);
  REQUIRE_EQ(size_after, (size_t)1);
#undef N_OLD_VAL_WAITERS
}

/* ========================================================================== */
/*                         SERIALIZED SETTERS                                 */
/* ========================================================================== */

static volatile int serial_setter_order[8];
static volatile int serial_setter_idx = 0;

static bool ordering_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  usleep(5000); /* 5 ms, simulate latency */
  int v = *(const int *)val->ptr;
  int idx = __atomic_fetch_add(&serial_setter_idx, 1, __ATOMIC_SEQ_CST);
  serial_setter_order[idx] = v;
  return true;
}

typedef struct {
  clru_cache cache;
  int key;
  int val;
} simple_setter_arg_t;

static void *simple_setter_thread(void *arg) {
  simple_setter_arg_t *sa = (simple_setter_arg_t *)arg;
  clru_cache cache = sa->cache;
  clru_redeclare(cache, int, int);
  clru_set(cache, sa->key, sa->val);
  return NULL;
}

TEST(concurrency, setters_for_same_key_are_serialized) {
  serial_setter_idx = 0;
  memset((void *)serial_setter_order, 0, sizeof(serial_setter_order));

  clru_construct(cache, int, int, 16, NULL, ordering_remote_setter, NULL);

#define N_SETTERS 4
  simple_setter_arg_t sargs[N_SETTERS];
  pthread_t stids[N_SETTERS];

  int stids_created = 0;
  for (int i = 0; i < N_SETTERS; i++) {
    sargs[i].cache = cache;
    sargs[i].key = 7; /* same key for all */
    sargs[i].val = i + 1;
    /* A partial failure here must not let the join loop below join an
     * uninitialized stids[i] slot. */
    if (pthread_create(&stids[i], NULL, simple_setter_thread, &sargs[i]) != 0)
      break;
    stids_created++;
    usleep(500); /* stagger slightly so order is deterministic */
  }
  REQUIRE_EQ(stids_created, (int)N_SETTERS);
  for (int i = 0; i < stids_created; i++) {
    pthread_join(stids[i], NULL);
  }

  /* All N_SETTERS remote calls should have been made */
  REQUIRE_EQ(serial_setter_idx, N_SETTERS);

  /* Each setter value appears exactly once */
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

  /* The final cached value must be whatever the last setter stored.
   * Setters are strictly serialized, so the last element of serial_setter_order
   * is the value that wins in the cache. */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  int final_val = 0;
  REQUIRE_EQ(clru_get(cache, 7, &final_val), ccol_success);
  REQUIRE_EQ(final_val, serial_setter_order[N_SETTERS - 1]);
#undef N_SETTERS

  clru_destroy(cache);
}

TEST(concurrency, concurrent_getters_different_keys) {
  coalesce_getter_calls = 0;
  clru_construct(cache, int, int, 16, slow_remote_getter, NULL, NULL);

#define N_UNIQUE_KEYS 8
  getter_arg_t args[N_UNIQUE_KEYS];
  pthread_t tids[N_UNIQUE_KEYS];
  int created = 0;
  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    args[i].cache = cache;
    args[i].key = 100 + i; /* distinct keys; no coalescing should happen */
    args[i].result = 0;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_gate = NULL; /* no shared-key race here to remove */
    /* A partial failure here must not let the join loop below join an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  /* The join comes before the assertion, and not after it. A REQUIRE_* that
   * fires here returns from this function. The threads that did start then
   * still run against args[] and tids[], which live on this frame. */
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  REQUIRE_EQ(created, (int)N_UNIQUE_KEYS);

  /* Cleanup runs unconditionally before the assertions below: REQUIRE_*
   * returns from this function immediately on the first failure, which
   * would otherwise leak the cache. */
  int calls = coalesce_getter_calls;
  ccol_retval_t retvals[N_UNIQUE_KEYS];
  int results[N_UNIQUE_KEYS];
  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    retvals[i] = args[i].retval;
    results[i] = args[i].result;
  }
  clru_destroy(cache);

  /* Each unique key must have triggered exactly one remote call */
  REQUIRE_EQ(calls, N_UNIQUE_KEYS);

  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    REQUIRE_EQ(retvals[i], ccol_success);
    REQUIRE_EQ(results[i], 100 + i + 1000); /* key + 1000 */
  }
#undef N_UNIQUE_KEYS
}

/* ========================================================================== */
/*     IN-PROGRESS SETTER NOT EVICTABLE (a live entry whose remote setter     */
/*     is still running must never be chosen as an eviction victim)           */
/* ========================================================================== */

/*
 * A setter that gates only when the value being stored equals 999, so the
 * first set (value=100) completes instantly and only the second (value=999)
 * blocks.  This lets a single cache instance be used for both the
 * pre-seeding step and the gated concurrent test.
 */
static _Atomic bool val_gate_started = false;
static _Atomic bool val_gate_open = false;

static bool value_gating_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  if (*(const int *)val->ptr == 999) {
    atomic_store(&val_gate_started, true);
    (void)clru_await_flag(&val_gate_open);
  }
  return true;
}

/*
 * With capacity=1 and key 1 already live, a gated setter for key 1 (value=999)
 * leaves key 1 in the eviction order while the remote call runs. A concurrent
 * set for key 2 therefore evicts key 1, which is the least recently used
 * entry. The set of key 1 then succeeds, stores 999 and evicts key 2 to make
 * room. The getter that was blocked on the setter must see ccol_success with
 * value 999, not ccol_key_not_found.
 */
TEST(concurrency, getter_sees_new_value_after_set_with_concurrent_insertion) {
  atomic_store(&val_gate_started, false);
  atomic_store(&val_gate_open, false);

  clru_construct(cache, int, int, 1, NULL, value_gating_setter, NULL);

  /* Seed key 1 = 100; value_gating_setter passes through immediately */
  clru_set(cache, 1, 100);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Thread A: slow setter for key 1 = 999 */
  sync_setter_arg_t sarg = {cache, 1, 999};
  pthread_t stid;
  REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);
  (void)clru_await_flag(&val_gate_started);

  /* Thread B: getter for key 1 (must block until setter finishes) */
  waiter_arg_t warg = {cache, 1, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool waiter_started =
      (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);
  /* Captured, not asserted: a failing REQUIRE_ here would return with Thread A
     gated and Thread B blocked on it, neither joined. */
  int done_while_blocked = -1;
  if (waiter_started) {
    usleep(50000); /* give Thread B time to enter the wait loop */
    done_while_blocked = (int)atomic_load(&warg.done);
  }

  /* Main thread acts as Thread C: insert key 2 while Thread A is gated.
   * This evicts key 1. The set of Thread A has not taken effect yet, and
   * when it does it inserts key 1 again with the new value. */
  clru_set(cache, 2, 200);

  /* Release Thread A */
  atomic_store(&val_gate_open, true);
  pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  clru_destroy(cache);

  REQUIRE_TRUE(waiter_started);
  REQUIRE_EQ(done_while_blocked, 0);
  /* Thread B must have received key 1's new value */
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 999);
}

/* ========================================================================== */
/*        COALESCED WAITERS SURVIVE A RACING EVICTION OF THE ENTRY          */
/* ========================================================================== */

/*
 * A getter joins an in-flight fetch or set for the same key. An unrelated
 * cache operation on another thread can evict the entry that the fetch just
 * published, before the waiter wakes up and reads the result. That getter
 * must still not receive ccol_key_not_found. The fetch or the set that it
 * joined succeeded, and the reference of the waiter keeps the value of the
 * entry alive and reachable.
 *
 * clru_test_set_post_publish_delay_us() widens the window between the
 * moment a thread publishes an entry and the moment that thread broadcasts
 * and unlocks. An evictor on another thread therefore queues up on the
 * mutex ahead of the woken waiter. The test does not depend on rare
 * scheduling luck. See the doc comment of that hook in clrucache.c.
 */

static _Atomic bool fetch_gate_started = false;
static _Atomic bool fetch_gate_open = false;

static bool gating_remote_getter(const cmap_pair *key, cmap_pair *val) {
  atomic_store(&fetch_gate_started, true);
  (void)clru_await_flag(&fetch_gate_open);
  int k = *(const int *)key->ptr;
  int *v = (int *)malloc(sizeof(int));
  if (!v) return false;
  *v = k + 1000;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

typedef struct {
  clru_cache cache;
  int key;
  int result;
  ccol_retval_t retval;
} full_api_getter_arg_t;

static void *full_api_getter_thread(void *arg) {
  full_api_getter_arg_t *ga = (full_api_getter_arg_t *)arg;
  cmap_pair kp = {};
  _populate_cmap_pair(&kp, ga->key);
  cmap_pair val_out = {};
  ga->retval = clrucache_get_full(ga->cache, &kp, &val_out);
  if (ga->retval == ccol_success) {
    ga->result = *(int *)val_out.ptr;
    free(val_out.ptr);
  }
  return NULL;
}

/*
 * POSIX does not guarantee that a thread wins the mutex race against a woken
 * waiter on a condition variable. See the doc comment of
 * clru_test_set_post_publish_delay_us in clrucache.c. The wider window only
 * makes that win overwhelmingly likely.
 *
 * This test runs the race several times, with a fresh cache each time, and
 * demands correctness on every attempt. That turns "overwhelmingly likely
 * for each attempt" into a near certain failure against a regression. The
 * test therefore needs no hard scheduling guarantee, which it cannot have.
 */
#define COALESCE_EVICTION_RACE_ITERATIONS 15

TEST(concurrency,
     coalesced_fetch_waiter_receives_value_despite_racing_eviction) {
  for (int iter = 0; iter < COALESCE_EVICTION_RACE_ITERATIONS; iter++) {
    atomic_store(&fetch_gate_started, false);
    atomic_store(&fetch_gate_open, false);
    clru_test_set_post_publish_delay_us(100000); /* 100ms */

    clru_construct(cache, int, int, 1, gating_remote_getter, NULL, NULL);

    /* Thread A: the fetcher for key 1, through clru_get and then
     * __clrucache_get_into. It gates inside the remote getter, which is
     * well outside the mutex of the cache. */
    getter_arg_t farg = {cache, 1, 0, ccol_unexpected_failure, NULL};
    pthread_t ftid;
    bool started_ftid =
        (pthread_create(&ftid, NULL, getter_thread, &farg) == 0);
    (void)clru_await_flag(&fetch_gate_started);

    /* Thread B: a getter that joins the in-flight fetch of Thread A. This
     * time it enters through the full clrucache_get_full() API directly.
     * It must block on the placeholder of Thread A. */
    full_api_getter_arg_t warg = {cache, 1, 0, ccol_unexpected_failure};
    pthread_t wtid;
    bool started_wtid =
        (pthread_create(&wtid, NULL, full_api_getter_thread, &warg) == 0);
    usleep(50000); /* give Thread B time to enter the wait loop */

    /* Release Thread A. It locks the mutex again and publishes key 1 as
     * LIVE. The capacity is 1, so that key is also the LRU tail. Thread A
     * then spins inside clru_test_set_post_publish_delay_us() for 100ms
     * and STILL HOLDS the mutex. This is well before it broadcasts to
     * wake Thread B. */
    atomic_store(&fetch_gate_open, true);
    /* Wait until Thread A really locks the mutex again and enters its
     * post-publish delay, where it still holds the mutex. A fixed sleep
     * here would be a guess. A poll of this flag needs no lock of its own,
     * so the hold of Thread A cannot block the poll. */
    while (!clru_test_post_publish_delay_entered()) usleep(200);

    /* Thread C is this thread. It inserts an unrelated key while Thread A
     * holds the mutex in the middle of its delay. This call blocks on the
     * mutex and queues up long before Thread A broadcasts. It is therefore
     * very likely to get the mutex ahead of Thread B once Thread A
     * releases it. That evicts key 1, which is the only entry and so the
     * LRU tail, before Thread B can read it. */
    REQUIRE_EQ(clru_set(cache, 2, 200), ccol_success);

    if (started_ftid) pthread_join(ftid, NULL);
    if (started_wtid) pthread_join(wtid, NULL);
    REQUIRE_TRUE(started_ftid);
    REQUIRE_TRUE(started_wtid);
    clru_test_set_post_publish_delay_us(0);

    /* Thread A is the fetcher. It always gets its own result, whatever the
     * race does, because it captures its own copy before it unlocks the
     * mutex. */
    REQUIRE_EQ(farg.retval, ccol_success);
    REQUIRE_EQ(farg.result, 1001);

    /* Thread B is the waiter that joined the fetch. It must receive the
     * SAME result as the fetcher, and never ccol_key_not_found. This holds
     * even when Thread C evicted key 1 before Thread B woke up. */
    REQUIRE_EQ(warg.retval, ccol_success);
    REQUIRE_EQ(warg.result, 1001);

    clru_destroy(cache);
  }
}

TEST(concurrency, coalesced_set_waiter_receives_value_despite_racing_eviction) {
  for (int iter = 0; iter < COALESCE_EVICTION_RACE_ITERATIONS; iter++) {
    atomic_store(&setter_started, false);
    atomic_store(&setter_may_finish, false);
    setter_should_fail = false;
    clru_test_set_post_publish_delay_us(100000); /* 100ms */

    clru_construct(cache, int, int, 1, NULL, gating_remote_setter, NULL);

    /* Thread A sets a new key 1 = 100. It gates inside the remote setter,
     * which is well outside the mutex of the cache. */
    sync_setter_arg_t sarg = {cache, 1, 100};
    pthread_t stid;
    bool started_stid =
        (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);
    (void)clru_await_flag(&setter_started);

    /* Thread B: a getter that joins the in-flight set of Thread A, through
     * clru_get and then __clrucache_get_into. It must block on the
     * placeholder of Thread A. */
    waiter_arg_t warg = {cache, 1, 100, 0, ccol_unexpected_failure, false};
    pthread_t wtid;
    bool started_wtid =
        (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);
    usleep(50000); /* give Thread B time to enter the wait loop */

    /* Release Thread A. It locks the mutex again and stores key 1 as LIVE.
     * The capacity is 1, so that key is also the LRU tail. Thread A then
     * spins inside clru_test_set_post_publish_delay_us() for 100ms and
     * STILL HOLDS the mutex. This is well before it broadcasts to wake
     * Thread B. */
    atomic_store(&setter_may_finish, true);
    /* Wait until Thread A really locks the mutex again and enters its
     * post-publish delay, where it still holds the mutex. A fixed sleep
     * here would be a guess. A poll of this flag needs no lock of its own,
     * so the hold of Thread A cannot block the poll. */
    while (!clru_test_post_publish_delay_entered()) usleep(200);

    /* Thread C is this thread. It inserts an unrelated key while Thread A
     * holds the mutex in the middle of its delay. That very likely evicts
     * key 1 before Thread B can read it, for the same reason as the fetch
     * test above. */
    REQUIRE_EQ(clru_set(cache, 2, 200), ccol_success);

    if (started_stid) pthread_join(stid, NULL);
    if (started_wtid) pthread_join(wtid, NULL);
    REQUIRE_TRUE(started_stid);
    REQUIRE_TRUE(started_wtid);
    clru_test_set_post_publish_delay_us(0);

    /* Thread B is the getter that joined the set. It must receive the value
     * that the set of Thread A stored, and never ccol_key_not_found. This
     * holds even when Thread C evicted key 1 before Thread B woke up. */
    REQUIRE_EQ(warg.retval, ccol_success);
    REQUIRE_EQ(warg.result, 100);

    clru_destroy(cache);
  }
}
#undef COALESCE_EVICTION_RACE_ITERATIONS

/*
 * A set takes effect when its remote setter returns. Until then a key that is
 * already in the cache stays in it: clrucache_size() counts it for the whole
 * window, and the count does not change when the set completes.
 */
TEST(concurrency, size_counts_an_existing_key_while_its_set_is_in_flight) {
  atomic_store(&val_gate_started, false);
  atomic_store(&val_gate_open, false);

  clru_construct(cache, int, int, 10, NULL, value_gating_setter, NULL);

  clru_set(cache, 1, 100);
  size_t size_before = clrucache_size(cache);

  sync_setter_arg_t sarg = {cache, 1, 999};
  pthread_t stid;
  bool started = (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);
  bool gated = started && clru_await_flag(&val_gate_started);
  size_t size_during = clrucache_size(cache);

  atomic_store(&val_gate_open, true);
  if (started) pthread_join(stid, NULL);

  size_t size_after = clrucache_size(cache);
  int out = 0;
  ccol_retval_t get_r = clru_get(cache, 1, &out);
  clru_destroy(cache);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(gated);
  REQUIRE_EQ(size_before, (size_t)1);
  REQUIRE_EQ(size_during, (size_t)1);
  REQUIRE_EQ(size_after, (size_t)1);
  REQUIRE_EQ(get_r, ccol_success);
  REQUIRE_EQ(out, 999);
}

/*
 * A failed set of a key that exists, with no concurrent operation, leaves the
 * key live with its old value. The key stays counted for the whole window.
 */
TEST(concurrency, failed_setter_keeps_existing_entry_live) {
  clru_construct(cache, int, int, 4, NULL, gating_remote_setter, NULL);

  /* Phase 1: seed key 5 = 50 */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, true);
  setter_should_fail = false;
  ccol_retval_t seed_r = clru_set(cache, 5, 50);

  /* Phase 2: failing overwrite of key 5 */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;
  sync_setter_arg_t sarg2 = {cache, 5, 999};
  pthread_t stid2;
  bool started =
      (pthread_create(&stid2, NULL, sync_setter_thread, &sarg2) == 0);
  bool gated = started && clru_await_flag(&setter_started);
  size_t size_during = clrucache_size(cache);

  atomic_store(&setter_may_finish, true);
  if (started) pthread_join(stid2, NULL);
  setter_should_fail = false;

  size_t size_after = clrucache_size(cache);
  int out = 0;
  ccol_retval_t get_r = clru_get(cache, 5, &out);
  clru_destroy(cache);

  REQUIRE_EQ(seed_r, ccol_success);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(gated);
  REQUIRE_EQ(size_during, (size_t)1);
  REQUIRE_EQ(size_after, (size_t)1);
  REQUIRE_EQ(get_r, ccol_success);
  REQUIRE_EQ(out, 50);
}

/*
 * The two tests below pin what a concurrent insert does while a set of a key
 * that exists is in flight. The set has not happened yet, so the insert must
 * evict the true least recently used key, even when that is the key that the
 * set targets, and the segment must never hold more entries than its
 * capacity. The outcome of the set then decides the rest:
 *
 *   - A failed set leaves the cache exactly as the insert alone left it. The
 *     targeted key stays evicted and every other key keeps its place.
 *   - A successful set inserts the targeted key again, with the new value, at
 *     the most recently used end, and evicts whatever is then the least
 *     recently used key.
 *
 * Both are the result of the same operations run one after the other: the
 * insert first, then the set. The remote setter blocks on a gate that this
 * thread opens, so the insert lands inside the window every time.
 */
static _Atomic bool cap_overflow_gate_started = false;
static _Atomic bool cap_overflow_gate_open = false;

static bool cap_overflow_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  if (*(const int *)val->ptr == 999) {
    atomic_store(&cap_overflow_gate_started, true);
    (void)clru_await_flag(&cap_overflow_gate_open);
    return false; /* fail this specific value */
  }
  return true; /* all other values succeed immediately */
}

typedef struct {
  clru_cache cache;
  int key;
  int val;
  ccol_retval_t retval;
} checked_setter_arg_t;

static void *checked_setter_thread(void *arg) {
  checked_setter_arg_t *sa = (checked_setter_arg_t *)arg;
  clru_cache cache = sa->cache;
  clru_redeclare(cache, int, int);
  int k = sa->key, v = sa->val;
  sa->retval = clru_set(cache, k, v);
  return NULL;
}

TEST(concurrency, failed_set_in_flight_lets_an_insert_evict_the_true_lru) {
  atomic_store(&cap_overflow_gate_started, false);
  atomic_store(&cap_overflow_gate_open, false);
  eviction_count = 0;
  eviction_key_captured = -1;
  eviction_val_captured = -1;

  /* Capacity 2 is a single segment, so the eviction order is exact. */
  clru_construct(cache, int, int, 2, NULL, cap_overflow_setter,
                 record_eviction);

  /* Order: 1 (LRU) -> 2 (MRU). */
  ccol_retval_t seed1 = clru_set(cache, 1, 10);
  ccol_retval_t seed2 = clru_set(cache, 2, 20);

  /* Thread A overwrites key 1 with 999. Its remote setter blocks, then
     fails. */
  checked_setter_arg_t sarg = {cache, 1, 999, ccol_success};
  pthread_t stid;
  bool started =
      (pthread_create(&stid, NULL, checked_setter_thread, &sarg) == 0);
  bool gated = started && clru_await_flag(&cap_overflow_gate_started);

  /* Inside the window: this insert must evict key 1, the true least
     recently used key, and nothing else. */
  ccol_retval_t ins3 = clru_set(cache, 3, 30);
  int evictions_in_window = eviction_count;
  int evicted_key_in_window = eviction_key_captured;
  int evicted_val_in_window = eviction_val_captured;
  size_t size_in_window = clrucache_size(cache);

  /* A getter for key 1 joins the set in flight, or arrives after it. Either
     way the key is gone once the set fails. */
  waiter_arg_t warg = {cache, 1, 0, 0, ccol_success, false};
  pthread_t wtid;
  bool wstarted = (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  atomic_store(&cap_overflow_gate_open, true);
  if (started) pthread_join(stid, NULL);
  if (wstarted) pthread_join(wtid, NULL);

  int evictions_after = eviction_count;
  size_t size_after = clrucache_size(cache);
  int out1 = 0, out2 = 0, out3 = 0;
  ccol_retval_t get1 = clru_get(cache, 1, &out1);
  ccol_retval_t get2 = clru_get(cache, 2, &out2);
  ccol_retval_t get3 = clru_get(cache, 3, &out3);
  clru_destroy(cache);

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
  /* The failed set evicted nothing of its own. */
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

  clru_construct(cache, int, int, 2, NULL, value_gating_setter,
                 record_eviction);

  /* Order: 1 (LRU) -> 2 (MRU). */
  ccol_retval_t seed1 = clru_set(cache, 1, 10);
  ccol_retval_t seed2 = clru_set(cache, 2, 20);

  /* Thread A overwrites key 1 with 999. Its remote setter blocks, then
     succeeds. */
  checked_setter_arg_t sarg = {cache, 1, 999, ccol_unexpected_failure};
  pthread_t stid;
  bool started =
      (pthread_create(&stid, NULL, checked_setter_thread, &sarg) == 0);
  bool gated = started && clru_await_flag(&val_gate_started);

  /* Inside the window: key 1 is still the least recently used key. */
  ccol_retval_t ins3 = clru_set(cache, 3, 30);
  int evictions_in_window = eviction_count;
  int evicted_key_in_window = eviction_key_captured;
  int evicted_val_in_window = eviction_val_captured;
  size_t size_in_window = clrucache_size(cache);

  /* A getter for key 1 joins the set in flight, or arrives after it. Either
     way it reads the value that the set stored. */
  waiter_arg_t warg = {cache, 1, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  bool wstarted = (pthread_create(&wtid, NULL, waiter_thread, &warg) == 0);

  atomic_store(&val_gate_open, true);
  if (started) pthread_join(stid, NULL);
  if (wstarted) pthread_join(wtid, NULL);

  /* The set inserted key 1 at the most recently used end and evicted key 2,
     the least recently used key at that point. */
  int evictions_after_set = eviction_count;
  int evicted_key_by_set = eviction_key_captured;
  int evicted_val_by_set = eviction_val_captured;
  size_t size_after = clrucache_size(cache);

  /* The order is now 3 (LRU) -> 1 (MRU). The getter above touched key 1,
     which is already the most recently used key. */
  ccol_retval_t ins4 = clru_set(cache, 4, 40);
  int evicted_key_by_4 = eviction_key_captured;
  int out1 = 0, out2 = 0, out4 = 0;
  ccol_retval_t get1 = clru_get(cache, 1, &out1);
  ccol_retval_t get2 = clru_get(cache, 2, &out2);
  ccol_retval_t get4 = clru_get(cache, 4, &out4);
  clru_destroy(cache);

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

/*
 * While a remote getter is executing, the entry exists as a placeholder that
 * is NOT in the LRU list.  clrucache_size() must return 0 during that window
 * and 1 only after the fetch completes and the entry becomes live.
 */
TEST(concurrency, size_zero_while_fetch_in_progress) {
  coalesce_getter_calls = 0;
  clru_construct(cache, int, int, 10, slow_remote_getter, NULL, NULL);

  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  getter_arg_t arg = {cache, 55, 0, ccol_unexpected_failure, NULL};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, getter_thread, &arg), 0);

  /* spin until the remote getter has been entered (placeholder in map) */
  while (atomic_load(&coalesce_getter_calls) == 0) usleep(1000);

  /* placeholder is not LIVE, so size must still be 0 */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  pthread_join(tid, NULL);

  /* after the fetch the entry is live */
  REQUIRE_EQ(arg.retval, ccol_success);
  REQUIRE_EQ(arg.result, 1055); /* 55 + 1000 */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  clru_destroy(cache);
}

/*
 * Many getters for the same key run at the same time. They must join one
 * single remote call even when that call FAILS. Only one fetch runs. Every
 * waiter receives ccol_key_not_found. The cache is then empty.
 */
static volatile int fail_getter_call_count = 0;

static bool slow_failing_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  __atomic_fetch_add(&fail_getter_call_count, 1, __ATOMIC_SEQ_CST);
  usleep(100000); /* 100 ms; gives all threads time to block on the placeholder
                   */
  return false;
}

TEST(concurrency, multiple_getters_coalesce_on_failed_fetch) {
  fail_getter_call_count = 0;
  clru_construct(cache, int, int, 16, slow_failing_getter, NULL, NULL);

#define N_FAIL_THREADS 6
  clru_start_gate_t gate = {0};
  getter_arg_t args[N_FAIL_THREADS];
  pthread_t tids[N_FAIL_THREADS];
  int created = 0;
  for (int i = 0; i < N_FAIL_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 11;
    args[i].result = 0;
    args[i].retval = ccol_success; /* sentinel; must be overwritten */
    args[i].start_gate = &gate;
    /* A partial failure here must not let the join loop below join an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  /* The release comes before the assertion, and not after it. A partial
   * start must still let every thread that did start finish, so that the
   * loop can join it. Without that, the assertion below returns with those
   * threads parked and the fixture leaked. */
  clru_gate_release(&gate, created);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  REQUIRE_EQ(created, (int)N_FAIL_THREADS);

  /* Cleanup runs unconditionally before the assertions below: REQUIRE_*
   * returns from this function immediately on the first failure, which
   * would otherwise leak the cache. */
  int calls = fail_getter_call_count;
  ccol_retval_t retvals[N_FAIL_THREADS];
  for (int i = 0; i < N_FAIL_THREADS; i++) retvals[i] = args[i].retval;
  size_t final_size = clrucache_size(cache);
  clru_destroy(cache);

  /* Only one remote call, regardless of how many threads entered */
  REQUIRE_EQ(calls, 1);

  for (int i = 0; i < N_FAIL_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_key_not_found);
  }
  REQUIRE_EQ(final_size, (size_t)0);
#undef N_FAIL_THREADS
}

/*
 * __clrucache_get_into() is the clru_get() path for a value that is not a
 * char*. It rejects the value that a remote getter fetched when the size of
 * that value does not match the fixed-size buffer of the caller. It then
 * reports ccol_unexpected_failure.
 *
 * That exact diagnostic must reach every caller that joined the same
 * in-flight fetch. Here one such caller arrives through the raw API of
 * clrucache_get_full(), which has no fixed-size destination of its own. The
 * diagnostic must not reach only the thread that ran the getter. To hand the
 * joined callers the general ccol_key_not_found instead would break the
 * documented contract of this module for this one failure reason. That
 * contract says that all others receive the same result. The file-level doc
 * comment of clrucache.h states it.
 */
static _Atomic bool mismatch_gate_started = false;
static _Atomic bool mismatch_gate_open = false;

static bool gating_oversized_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  atomic_store(&mismatch_gate_started, true);
  (void)clru_await_flag(&mismatch_gate_open);
  /* Larger than sizeof(int), the cache's declared value size. */
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

  clru_construct(cache, int, int, 8, gating_oversized_getter, NULL, NULL);

  /* Thread A: clru_get and then __clrucache_get_into, for a cache whose
   * values are int. It gates inside the remote getter, which is well
   * outside the mutex of the cache. */
  getter_arg_t farg = {cache, 1, 0, ccol_success, NULL};
  pthread_t ftid;
  bool started_ftid = (pthread_create(&ftid, NULL, getter_thread, &farg) == 0);
  (void)clru_await_flag(&mismatch_gate_started);

  /* Thread B joins the in-flight fetch of Thread A. It calls
   * clrucache_get_full() directly, which has no buf_size of its own to
   * compare against. */
  full_api_getter_arg_t warg = {cache, 1, 0, ccol_success};
  pthread_t wtid;
  bool started_wtid =
      (pthread_create(&wtid, NULL, full_api_getter_thread, &warg) == 0);
  usleep(50000); /* give Thread B time to enter the wait loop */

  atomic_store(&mismatch_gate_open, true);
  if (started_ftid) pthread_join(ftid, NULL);
  if (started_wtid) pthread_join(wtid, NULL);
  REQUIRE_TRUE(started_ftid);
  REQUIRE_TRUE(started_wtid);

  /* Thread A (the fetcher) gets the specific diagnostic. */
  REQUIRE_EQ(farg.retval, ccol_unexpected_failure);

  /* Thread B (the coalesced waiter) must receive the SAME diagnostic, not
   * the generic ccol_key_not_found every other fetch-failure reason
   * produces. */
  REQUIRE_EQ(warg.retval, ccol_unexpected_failure);

  /* The mis-sized value must not have been cached. */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  clru_destroy(cache);
}

/*
 * A setter arrives while a remote getter runs for the same key, which is the
 * fetch_in_progress state. That setter must block until the fetch ends. It
 * must then overwrite the value that the fetch cached. This exercises the
 * fetch_in_progress wait path inside clrucache_set_full.
 */
TEST(concurrency, setter_waits_for_active_fetch_then_succeeds) {
  coalesce_getter_calls = 0;
  remote_set_should_succeed = true;
  remote_set_call_count = 0;

  /* Cache has both a slow getter (100 ms) and a recording setter. */
  clru_construct(cache, int, int, 16, slow_remote_getter,
                 recording_remote_setter, NULL);

  /* Thread A: get key=7; triggers a 100 ms remote fetch */
  getter_arg_t garg = {cache, 7, 0, ccol_unexpected_failure, NULL};
  pthread_t gtid;
  bool started_gtid = (pthread_create(&gtid, NULL, getter_thread, &garg) == 0);

  /* Wait until the fetch has actually started (placeholder created, mutex
   * released, slow getter running). */
  while (atomic_load(&coalesce_getter_calls) == 0) usleep(1000);

  /* Thread B: set key=7; must find fetch_in_progress=true and block */
  sync_setter_arg_t sarg = {cache, 7, 999};
  pthread_t stid;
  bool started_stid =
      (pthread_create(&stid, NULL, sync_setter_thread, &sarg) == 0);

  if (started_gtid) pthread_join(gtid, NULL);
  if (started_stid) pthread_join(stid, NULL);
  REQUIRE_TRUE(started_gtid);
  REQUIRE_TRUE(started_stid);

  /* Thread A received the value from the remote getter (7 + 1000 = 1007) */
  REQUIRE_EQ(garg.retval, ccol_success);
  REQUIRE_EQ(garg.result, 1007);

  /* The setter ran exactly once after the fetch completed */
  REQUIRE_EQ(remote_set_call_count, 1);
  REQUIRE_EQ(remote_set_last_key, 7);
  REQUIRE_EQ(remote_set_last_val, 999);

  /* Final cached value must reflect the setter's write */
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 7, &out), ccol_success);
  REQUIRE_EQ(out, 999);

  clru_destroy(cache);
}

/*
 * Same coalescing guarantee as multiple_getters_coalesce_to_single_remote_fetch
 * but for char* value caches, which route through clrucache_get_full instead of
 * __clrucache_get_into. Each thread must receive an independent heap copy of
 * the fetched string.
 */
static volatile int char_ptr_coalesce_calls = 0;

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

typedef struct {
  clru_cache cache;
  int key;
  char *result;
  ccol_retval_t retval;
  /* The worker waits on this gate, when it is not NULL, immediately before
   * it calls clru_get. The two call sites below show why this gate is
   * needed. Every thread that races for the same key must contend for the
   * mutex of the cache before the fetch of the first thread ends. A fixed
   * sleep inside the remote getter cannot reliably do that. */
  clru_start_gate_t *start_gate;
} str_getter_arg_t;

static void *str_getter_thread(void *arg) {
  str_getter_arg_t *ga = (str_getter_arg_t *)arg;
  clru_cache cache = ga->cache;
  clru_redeclare(cache, int, char *);
  clru_gate_arrive(ga->start_gate);
  ga->retval = clru_get(cache, ga->key, &ga->result);
  return NULL;
}

TEST(concurrency, multiple_char_ptr_getters_coalesce) {
  char_ptr_coalesce_calls = 0;
  clru_construct(cache, int, char *, 16, slow_char_ptr_getter, NULL, NULL);

#define N_STR_THREADS 6
  clru_start_gate_t gate = {0};
  str_getter_arg_t args[N_STR_THREADS];
  pthread_t tids[N_STR_THREADS];
  int created = 0;
  for (int i = 0; i < N_STR_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 77;
    args[i].result = NULL;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_gate = &gate;
    /* A partial failure here must not let the join loop below join an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, str_getter_thread, &args[i]) != 0) break;
    created++;
  }
  /* The release comes before the assertion, and not after it. A partial
   * start must still let every thread that did start finish, so that the
   * loop can join it. Without that, the assertion below returns with those
   * threads parked and the fixture leaked. */
  clru_gate_release(&gate, created);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  REQUIRE_EQ(created, (int)N_STR_THREADS);

  /* Every cleanup step runs on every path, before the assertions below.
   * That covers the cache and each result string that the test allocated.
   * A REQUIRE_* returns from this function at once on the first failure.
   * Without the cleanup first, that would leak the cache and every
   * args[i].result that the loop has not yet freed. */
  int calls = char_ptr_coalesce_calls;
  ccol_retval_t retvals[N_STR_THREADS];
  char results_copy[N_STR_THREADS][64];
  bool has_result[N_STR_THREADS];
  for (int i = 0; i < N_STR_THREADS; i++) {
    retvals[i] = args[i].retval;
    has_result[i] = args[i].result != NULL;
    if (has_result[i]) {
      snprintf(results_copy[i], sizeof(results_copy[i]), "%s", args[i].result);
      free(args[i].result);
    }
  }
  clru_destroy(cache);

  REQUIRE_EQ(calls, 1);
  for (int i = 0; i < N_STR_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_success);
    REQUIRE_TRUE(has_result[i]);
    REQUIRE_STREQ(results_copy[i], "str_77");
  }
#undef N_STR_THREADS
}

/*
 * Same coalescing guarantee as multiple_getters_coalesce_on_failed_fetch, but
 * for char* value caches.  Those route through clrucache_get_full (not
 * __clrucache_get_into) and have their own waiter/broadcast path for the
 * failure case.  All threads must receive ccol_key_not_found and only one
 * remote call must have been made.
 */
static volatile int slow_fail_char_ptr_getter_calls = 0;

static bool slow_failing_char_ptr_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  __atomic_fetch_add(&slow_fail_char_ptr_getter_calls, 1, __ATOMIC_SEQ_CST);
  usleep(100000); /* 100 ms; gives all threads time to block on the
                     placeholder */
  return false;
}

TEST(concurrency, multiple_char_ptr_getters_coalesce_on_failed_fetch) {
  slow_fail_char_ptr_getter_calls = 0;
  clru_construct(cache, int, char *, 16, slow_failing_char_ptr_getter, NULL,
                 NULL);

#define N_FAIL_STR_THREADS 6
  clru_start_gate_t gate = {0};
  str_getter_arg_t args[N_FAIL_STR_THREADS];
  pthread_t tids[N_FAIL_STR_THREADS];
  int created = 0;
  for (int i = 0; i < N_FAIL_STR_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 22;
    args[i].result = NULL;
    args[i].retval = ccol_success; /* sentinel; must be overwritten */
    args[i].start_gate = &gate;
    /* A partial failure here must not let the join loop below join an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, str_getter_thread, &args[i]) != 0) break;
    created++;
  }
  /* The release comes before the assertion, and not after it. A partial
   * start must still let every thread that did start finish, so that the
   * loop can join it. Without that, the assertion below returns with those
   * threads parked and the fixture leaked. */
  clru_gate_release(&gate, created);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  REQUIRE_EQ(created, (int)N_FAIL_STR_THREADS);

  /* Every cleanup runs unconditionally before the assertions below:
   * REQUIRE_* returns from this function immediately on the first failure,
   * which would otherwise leak the cache. */
  int calls = slow_fail_char_ptr_getter_calls;
  ccol_retval_t retvals[N_FAIL_STR_THREADS];
  bool has_result[N_FAIL_STR_THREADS];
  for (int i = 0; i < N_FAIL_STR_THREADS; i++) {
    retvals[i] = args[i].retval;
    has_result[i] = args[i].result != NULL;
    if (has_result[i]) free(args[i].result);
  }
  size_t final_size = clrucache_size(cache);
  clru_destroy(cache);

  /* Only one remote call, regardless of how many threads entered */
  REQUIRE_EQ(calls, 1);
  for (int i = 0; i < N_FAIL_STR_THREADS; i++) {
    REQUIRE_EQ(retvals[i], ccol_key_not_found);
    REQUIRE_FALSE(has_result[i]);
  }
  REQUIRE_EQ(final_size, (size_t)0);
#undef N_FAIL_STR_THREADS
}

/* ========================================================================== */
/*  MULTIPLE SETTERS RACING AFTER A FAILED IN-PROGRESS SET (regression)      */
/* ========================================================================== */

/*
 * Remote setter that:
 *   - First call  (call index 0): sleeps 30 ms then FAILS.
 *   - Second call (call index 1): sleeps 30 ms then SUCCEEDS.
 *   - Any further calls          : succeed immediately.
 *
 * The 30 ms sleep in call 0 gives threads 2 and 3 time to enter the
 * set_in_progress wait loop before the first call ends.
 *
 * The 30 ms sleep in call 1 gives thread 3 time to act while the remote call
 * of thread 2 still runs. With the guard, thread 3 checks the map again.
 * Without the guard, thread 3 races to create a second placeholder.
 */
static volatile int phased_set_count = 0;

static bool phased_setter_fn(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  (void)val;
  int call = __atomic_fetch_add(&phased_set_count, 1, __ATOMIC_SEQ_CST);
  if (call == 0) {
    usleep(30000); /* 30 ms: give waiters time to queue */
    return false;  /* first call always fails */
  }
  if (call == 1) {
    usleep(30000); /* 30 ms: give thread 3 time to race or wait */
    return true;
  }
  return true;
}

/*
 * Three threads all try to set the same key, which is new.
 *
 * Thread 1 takes the set slot first and calls the remote setter. That call
 * takes 30 ms and then FAILS. Threads 2 and 3 block on set_in_progress for
 * that window.
 *
 * Without the guard that joins the callers, Threads 2 and 3 both wake up
 * after Thread 1 fails. Thread 2 creates a new placeholder and unlocks the
 * mutex for its own 30 ms remote call. Thread 3 then calls
 * create_and_insert_placeholder for the same key. That reaches
 * chmap_insert_elem with a key that is already there, and gets
 * ccol_key_already_present. The function therefore returns NULL, and Thread
 * 3 wrongly returns ccol_not_enough_memory. It drops the set in silence.
 * The result is 2 remote setter calls, and not 3.
 *
 * With the guard, Thread 3 loops back to map_lookup. It finds the
 * placeholder of Thread 2, where set_in_progress is true, and waits for it.
 * Thread 3 takes over the same entry after Thread 2 succeeds, and runs the
 * remote setter itself. The result is all 3 remote setter calls, and a size
 * of 1.
 */
TEST(concurrency, multiple_setters_race_after_failed_new_key_set) {
  phased_set_count = 0;

  clru_construct(cache, int, int, 16, NULL, phased_setter_fn, NULL);

#define N_RACING_SETTERS 3
  simple_setter_arg_t sargs[N_RACING_SETTERS];
  pthread_t stids[N_RACING_SETTERS];
  int created = 0;
  for (int i = 0; i < N_RACING_SETTERS; i++) {
    sargs[i].cache = cache;
    sargs[i].key = 55;
    sargs[i].val = i + 1;
    /* A partial failure here must not let the join loop below join an
     * uninitialized stids[i] slot. */
    if (pthread_create(&stids[i], NULL, simple_setter_thread, &sargs[i]) != 0)
      break;
    created++;
    usleep(1000); /* stagger so thread 1 acquires the set slot first */
  }
  /* The join comes before the assertion, and not after it. A REQUIRE_* that
   * fires here returns from this function. The threads that did start then
   * still run against sargs[] and stids[], which live on this frame. */
  for (int i = 0; i < created; i++) {
    pthread_join(stids[i], NULL);
  }
  REQUIRE_EQ(created, (int)N_RACING_SETTERS);

  /* All 3 remote setter calls must have been made.  Without the coalescing
   * guard, thread 3 short-circuits with ccol_not_enough_memory and only 2
   * calls are observed. */
  REQUIRE_EQ(phased_set_count, N_RACING_SETTERS);

  /* Exactly one live entry must exist; no orphaned entries. */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* The key must be readable with a valid value. */
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 55, &out), ccol_success);
  REQUIRE_GE(out, 1);
  REQUIRE_LE(out, N_RACING_SETTERS);

#undef N_RACING_SETTERS
  clru_destroy(cache);
}

/* ========================================================================== */
/*                         SCOPED CONSTRUCT                                   */
/* ========================================================================== */

TEST(macros, scoped_construct_auto_destroys) {
  {
    clru_construct_scoped(cache, int, int, 4, NULL, NULL, NULL);
    int k = 1, v = 2;
    clru_set(cache, k, v);
    int out = 0;
    clru_get(cache, k, &out);
    REQUIRE_EQ(out, 2);
    /* cache is destroyed automatically when block exits */
  }
  /* If we reach here without crashing, the destructor ran. */
  REQUIRE_EQ(1, 1);
}

TEST(macros, declare_and_init) {
  clru_declare(cache, int, int);
  clru_init(cache, 8, NULL, NULL, NULL);

  int k = 10, v = 20;
  clru_set(cache, k, v);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, k, &out), ccol_success);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  clru_destroy(cache);
}

TEST(macros, declare_scoped_auto_destroys) {
  eviction_count = 0;
  {
    clru_declare_scoped(cache, int, int);
    clru_init(cache, 4, NULL, NULL, record_eviction);

    clru_set(cache, 1, 111);
    clru_set(cache, 2, 222);

    int out = 0;
    REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
    REQUIRE_EQ(out, 111);
    REQUIRE_EQ(clrucache_size(cache), (size_t)2);
    /* cache destroyed automatically when this block exits */
  }
  /* The destructor must evict both live entries */
  REQUIRE_EQ(eviction_count, 2);
}

/* ========================================================================== */
/*                         REDECLARE                                          */
/* ========================================================================== */

static ccol_retval_t use_cache_in_other_scope(clru_cache cache, int key,
                                              int *out) {
  clru_redeclare(cache, int, int);
  return clru_get(cache, key, out);
}

TEST(macros, redeclare_works_across_scope) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  int k = 3, v = 33;
  clru_set(cache, k, v);

  int result = 0;
  ccol_retval_t r = use_cache_in_other_scope(cache, k, &result);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(result, 33);

  clru_destroy(cache);
}

/* ========================================================================== */
/*            VALUE TYPE COVERAGE - clru_get & clrucache_get_full             */
/* ========================================================================== */

typedef struct {
  int x;
  double y;
} point_t;

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

/* non-char* get miss: output buffer must not be modified */

/*
 * For non-char* value types clru_get routes through __clrucache_get_into,
 * which returns ccol_key_not_found on a miss without touching the buffer.
 * Use a non-zero sentinel to make the guarantee explicit.
 */
TEST(get_val_types, non_char_ptr_get_miss_does_not_modify_buf) {
  clru_construct(cache, int, double, 8, NULL, NULL, NULL);

  double sentinel = 3.14;
  double out = sentinel;
  REQUIRE_EQ(clru_get(cache, 99, &out), ccol_key_not_found);
  REQUIRE_EQ(out, sentinel); /* buffer must be untouched on miss */

  clru_destroy(cache);
}

/* non-char* scalar: double */

TEST(get_val_types, double_value_via_macro) {
  clru_construct(cache, int, double, 8, NULL, NULL, NULL);

  clru_set(cache, 1, 2.5);

  double out = 0.0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2.5);

  clru_destroy(cache);
}

/* non-char* struct value */

TEST(get_val_types, struct_value_via_macro) {
  clru_construct(cache, int, point_t, 8, NULL, NULL, NULL);

  point_t v = {.x = 7, .y = 2.5};
  clru_set(cache, 42, v);

  point_t out = {};
  REQUIRE_EQ(clru_get(cache, 42, &out), ccol_success);
  REQUIRE_EQ(out.x, 7);
  REQUIRE_EQ(out.y, 2.5);

  clru_destroy(cache);
}

/* clrucache_get_full: each call produces a fresh independent allocation */

TEST(get_val_types, full_api_returns_independent_copies) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  clru_set(cache, 5, 99);

  int k = 5;
  cmap_pair kp = {};
  _populate_cmap_pair(&kp, k);

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

  clru_destroy(cache);
}

/* on a miss, val_out fields are not modified */

TEST(get_val_types, missing_key_val_out_untouched) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  int k = 99;
  cmap_pair kp = {};
  _populate_cmap_pair(&kp, k);

  void *sentinel_ptr = (void *)0x1;
  size_t sentinel_sz = 0xBEEFUL;
  cmap_pair val_out = {.ptr = sentinel_ptr, .size = sentinel_sz};

  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_key_not_found);
  REQUIRE_PTR_EQ(val_out.ptr, sentinel_ptr);
  REQUIRE_EQ(val_out.size, sentinel_sz);

  clru_destroy(cache);
}

/* overwrite: a later get returns the new value */

TEST(get_val_types, overwrite_reflected_in_subsequent_get) {
  clru_construct(cache, int, double, 8, NULL, NULL, NULL);

  clru_set(cache, 1, 1.0);
  clru_set(cache, 1, 2.5);

  double out = 0.0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2.5);

  clru_destroy(cache);
}

/* char* value: macro transfers heap ownership to caller */

TEST(get_val_types, char_ptr_value_macro_transfers_ownership) {
  clru_construct(cache, int, char *, 8, NULL, NULL, NULL);

  clru_set(cache, 10, "hello");

  char *out = NULL;
  REQUIRE_EQ(clru_get(cache, 10, &out), ccol_success);
  REQUIRE_NOT_NULL(out);
  REQUIRE_STREQ(out, "hello");
  free(out);

  clru_destroy(cache);
}

/* char* value: two consecutive gets return distinct heap pointers */

TEST(get_val_types, char_ptr_value_two_gets_are_independent_copies) {
  clru_construct(cache, int, char *, 8, NULL, NULL, NULL);

  clru_set(cache, 7, "world");

  char *a = NULL, *b = NULL;
  REQUIRE_EQ(clru_get(cache, 7, &a), ccol_success);
  REQUIRE_EQ(clru_get(cache, 7, &b), ccol_success);

  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_PTR_NE(a, b);
  REQUIRE_STREQ(a, "world");
  REQUIRE_STREQ(b, "world");
  free(a);
  free(b);

  clru_destroy(cache);
}

/* char* value through clrucache_get_full: size == strlen + 1 */

TEST(get_val_types, char_ptr_value_full_api_size_includes_null_terminator) {
  clru_construct(cache, int, char *, 8, NULL, NULL, NULL);

  clru_set(cache, 3, "test_string");

  int k = 3;
  cmap_pair kp = {};
  _populate_cmap_pair(&kp, k);

  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_success);
  REQUIRE_NOT_NULL(val_out.ptr);
  REQUIRE_EQ(val_out.size, strlen("test_string") + 1);
  REQUIRE_STREQ((char *)val_out.ptr, "test_string");
  free(val_out.ptr);

  clru_destroy(cache);
}

/* both key and value are char* */

TEST(get_val_types, char_ptr_key_and_char_ptr_value) {
  clru_construct(cache, char *, char *, 8, NULL, NULL, NULL);

  clru_set(cache, "k1", "v1");
  clru_set(cache, "k2", "v2");

  char *out = NULL;
  REQUIRE_EQ(clru_get(cache, "k1", &out), ccol_success);
  REQUIRE_STREQ(out, "v1");
  free(out);

  out = NULL;
  REQUIRE_EQ(clru_get(cache, "k2", &out), ccol_success);
  REQUIRE_STREQ(out, "v2");
  free(out);

  REQUIRE_EQ(clru_get(cache, "missing", &out), ccol_key_not_found);

  clru_destroy(cache);
}

/* char* value from remote getter: macro path delivers owned string */

TEST(get_val_types, char_ptr_value_from_remote_getter_macro) {
  clru_construct(cache, int, char *, 8, str_val_remote_getter, NULL, NULL);

  char *out = NULL;
  REQUIRE_EQ(clru_get(cache, 42, &out), ccol_success);
  REQUIRE_NOT_NULL(out);
  REQUIRE_STREQ(out, "remote_42");
  free(out);

  /* Second call hits the cache; content must still match */
  out = NULL;
  REQUIRE_EQ(clru_get(cache, 42, &out), ccol_success);
  REQUIRE_STREQ(out, "remote_42");
  free(out);

  clru_destroy(cache);
}

/* char* value from remote getter: clrucache_get_full direct path */

TEST(get_val_types, char_ptr_value_from_remote_getter_full_api) {
  clru_construct(cache, int, char *, 8, str_val_remote_getter, NULL, NULL);

  int k = 7;
  cmap_pair kp = {};
  _populate_cmap_pair(&kp, k);

  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_success);
  REQUIRE_NOT_NULL(val_out.ptr);
  REQUIRE_EQ(val_out.size, strlen("remote_7") + 1);
  REQUIRE_STREQ((char *)val_out.ptr, "remote_7");
  free(val_out.ptr);

  clru_destroy(cache);
}

/* __clrucache_get_into safety check: cached value size mismatching the
 *     caller's buffer, in either direction (LIVE entry path).
 *
 *     The size-mismatch-reject logic in __clrucache_get_into has two
 *     distinct branches:
 *       - FETCH path: a freshly-fetched value is rejected (and not cached)
 *         before it is ever copied out. Exercised by
 *         getter_returns_oversized_value_no_crash and
 *         getter_returns_undersized_value_rejected_not_partially_copied
 *         above.
 *       - LIVE path: the value is already in the cache and the caller's
 *         buf_size does not exactly match it. The two tests below cover
 *         both directions of that branch.
 *
 *     The test reaches the LIVE path in two steps. It first stores a value
 *     with clrucache_set_full directly, which goes around the type-inferred
 *     macros. Those macros always keep buf_size and the stored size in
 *     step. The test then calls __clrucache_get_into with a buffer of a
 *     different size. */

TEST(get_val_types, get_into_rejects_live_value_too_large_for_buffer) {
  clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                           NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  int k = 1;
  long long v = 12345LL;
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v,
                  .size = sizeof(v)}; /* sizeof(long long) > sizeof(int) */

  REQUIRE_EQ(clrucache_set_full(cache, &kp, &vp), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Reading into an int buffer (4 bytes) while the cached value is 8 bytes
   * must fail gracefully without corrupting the output buffer. */
  int small_out = 0;
  REQUIRE_EQ(__clrucache_get_into(cache, &kp, &small_out, sizeof(small_out)),
             ccol_unexpected_failure);
  REQUIRE_EQ(small_out, 0); /* buffer must be untouched */

  /* The entry must still be in the cache. */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Reading with a correctly-sized buffer must succeed. */
  long long big_out = 0;
  REQUIRE_EQ(__clrucache_get_into(cache, &kp, &big_out, sizeof(big_out)),
             ccol_success);
  REQUIRE_EQ(big_out, 12345LL);

  __clrucache_destroy(cache);
}

TEST(get_val_types, get_into_rejects_live_value_too_small_for_buffer) {
  clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                           NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  int k = 1;
  char v = 0x5A;
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v,
                  .size = sizeof(v)}; /* 1 byte < sizeof(long long) */

  REQUIRE_EQ(clrucache_set_full(cache, &kp, &vp), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* A read into an 8-byte buffer, where the cached value is only 1 byte,
   * must fail cleanly. It must never report success and leave the other 7
   * bytes of the buffer of the caller untouched. */
  long long big_out = 0x1122334455667788LL;
  REQUIRE_EQ(__clrucache_get_into(cache, &kp, &big_out, sizeof(big_out)),
             ccol_unexpected_failure);
  REQUIRE_EQ(big_out, 0x1122334455667788LL); /* buffer must be untouched */

  /* The entry must still be in the cache: a size mismatch against ONE
   * caller's buffer does not evict an otherwise-valid cached value. */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Reading with a correctly-sized buffer must succeed. */
  char small_out = 0;
  REQUIRE_EQ(__clrucache_get_into(cache, &kp, &small_out, sizeof(small_out)),
             ccol_success);
  REQUIRE_EQ(small_out, 0x5A);

  __clrucache_destroy(cache);
}

/*
 * A __clrucache_get_into call fails when the buffer size of the caller does
 * not match the size of the cached value. Such a call must not promote the
 * entry to the front of the LRU order. Consider a call to
 * lru_move_to_front() on every path, before the size check. An entry that no
 * caller ever read would then outlive an entry that a caller really set more
 * recently. The only cause would be a query with the wrong buffer size.
 */
TEST(get_val_types, get_into_size_mismatch_failure_does_not_promote_lru) {
  clru_cache cache = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL,
                                           NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  /* k1 is the LRU, or oldest, entry. k2 is the MRU, or newest, one. Both
   * hold 8-byte values that the raw API stored. The LRU order after these
   * two sets is therefore k1 (LRU) -> k2 (MRU). */
  int k1 = 1;
  long long v1 = 111;
  cmap_pair kp1 = {.ptr = &k1, .size = sizeof(k1)};
  cmap_pair vp1 = {.ptr = &v1, .size = sizeof(v1)};
  REQUIRE_EQ(clrucache_set_full(cache, &kp1, &vp1), ccol_success);

  int k2 = 2;
  long long v2 = 222;
  cmap_pair kp2 = {.ptr = &k2, .size = sizeof(k2)};
  cmap_pair vp2 = {.ptr = &v2, .size = sizeof(v2)};
  REQUIRE_EQ(clrucache_set_full(cache, &kp2, &vp2), ccol_success);

  /* Try to read k1, which is the current LRU entry, into a buffer that is
   * too small. This must fail, and it must not promote k1 in the LRU
   * order. */
  int wrong_size_out = 0;
  REQUIRE_EQ(__clrucache_get_into(cache, &kp1, &wrong_size_out,
                                  sizeof(wrong_size_out)),
             ccol_unexpected_failure);

  /* Insert a third key. A capacity of 2 forces exactly one eviction. k1
   * must still be the LRU victim, because its failed read must not have
   * promoted it. k2 must survive, because nothing touched it again after
   * its own set. */
  int k3 = 3;
  long long v3 = 333;
  cmap_pair kp3 = {.ptr = &k3, .size = sizeof(k3)};
  cmap_pair vp3 = {.ptr = &v3, .size = sizeof(v3)};
  REQUIRE_EQ(clrucache_set_full(cache, &kp3, &vp3), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);

  long long out = 0;
  REQUIRE_EQ(__clrucache_get_into(cache, &kp1, &out, sizeof(out)),
             ccol_key_not_found); /* correctly evicted as the true LRU entry */
  REQUIRE_EQ(__clrucache_get_into(cache, &kp2, &out, sizeof(out)),
             ccol_success);
  REQUIRE_EQ(out, 222LL);

  __clrucache_destroy(cache);
}

/* ========================================================================== */
/*         clru_set/clru_get CONVERT TO KeyT/ValT, NOT REINTERPRET            */
/* ========================================================================== */

/*
 * These tests pin one contract. clru_set(name, key, val) must convert val
 * and key to the declared ValT and KeyT of the cache. It must convert them
 * exactly as a plain C assignment does. It must not store the raw bytes of
 * val or key with their own expression types.
 *
 * Consider a val of the same size but of a different type, for example a
 * float that goes into a cache whose values are int. Without the
 * conversion, the cache copies its raw bit pattern byte for byte. The size
 * check of __clrucache_get_into cannot find that. The stored size and the
 * requested buffer size are equal by chance, but the types are different.
 * The cache then gives an incorrect, reinterpreted value and reports
 * success. The same hazard is the reason for the typed temporary of
 * cvec_push in the type-inferred push macro of cvector.
 */
TEST(type_conversion, set_float_into_int_cache_converts_not_reinterprets) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  float f = 7.0f;
  REQUIRE_EQ(clru_set(cache, 1, f), ccol_success);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  /* A read of the raw bits of 7.0f as an int gives 1088421888, and not 7.
   * A real conversion gives 7, exactly as a plain `int x = 7.0f;` does. */
  REQUIRE_EQ(out, 7);

  clru_destroy(cache);
}

TEST(type_conversion,
     set_negative_float_into_int_cache_truncates_like_plain_assignment) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  float f = -3.75f;
  REQUIRE_EQ(clru_set(cache, 1, f), ccol_success);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, -3); /* truncates toward zero, exactly like int x = -3.75f; */

  clru_destroy(cache);
}

TEST(type_conversion, set_int_literal_into_double_cache_converts) {
  clru_construct(cache, int, double, 8, NULL, NULL, NULL);

  /* An unsuffixed int literal has type int, a different (and smaller) type
   * than the cache's declared double ValT. */
  REQUIRE_EQ(clru_set(cache, 1, 5), ccol_success);

  double out = 0.0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 5.0);

  clru_destroy(cache);
}

TEST(type_conversion, get_into_double_out_param_converts_stored_int) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  REQUIRE_EQ(clru_set(cache, 1, 9), ccol_success);

  /* A read of a cache whose values are int, into a double* out parameter,
   * must convert the stored int to a double. It converts exactly as
   * `double x = some_int;` does. It must not reject the read. Note that
   * sizeof(double) != sizeof(int), so a rejection on a size mismatch would
   * be safe and still wrong. This call must succeed and convert. */
  double out = 0.0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 9.0);

  clru_destroy(cache);
}

TEST(type_conversion,
     get_into_float_out_param_does_not_reinterpret_stored_int) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  REQUIRE_EQ(clru_set(cache, 1, 12), ccol_success);

  /* float and int are the same size on every mainstream platform. This is
   * therefore exactly the case where the sizes match by coincidence, which
   * the size check of __clrucache_get_into cannot detect on its own.
   * clru_get must still convert through the declared ValT of the cache,
   * which is int. It must not reinterpret. */
  float out = 0.0f;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 12.0f);

  clru_destroy(cache);
}

TEST(type_conversion, set_string_key_from_local_char_array_variable) {
  /* The key here is a char[] variable, and not a string literal. The KeyT
   * conversion, which tracks the type, must still decay it to char*
   * correctly. */
  clru_construct(cache, char *, int, 8, NULL, NULL, NULL);

  char key_buf[16];
  strcpy(key_buf, "arraykey");
  REQUIRE_EQ(clru_set(cache, key_buf, 42), ccol_success);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, "arraykey", &out), ccol_success);
  REQUIRE_EQ(out, 42);

  clru_destroy(cache);
}

/* ========================================================================== */
/*                         CUSTOM ALLOCATOR                                   */
/* ========================================================================== */

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

/*
 * A "budget" allocator. It succeeds as usual while budget is below 0, which
 * means no limit. It fails every call once budget reaches exactly 0.
 * Otherwise it takes one off budget for each allocation that succeeds.
 *
 * This lets a test force one SPECIFIC later allocation to fail every time.
 * On a hit that the cache already holds, clrucache_get_full makes a copy
 * for the caller. That copy is one such allocation, and it is the first
 * allocation that the call makes. The test therefore does not have to count
 * every allocation that its own setup phase makes.
 */
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
  ccol_memmgmt_procs_t mprocs = {_lru_custom_malloc, _lru_custom_free,
                                 _lru_custom_calloc, _lru_custom_realloc};

  char *err = NULL;
  clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                           NULL, &mprocs, &err);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  int k = 1, v = 42;
  cmap_pair kp = {}, vp = {};
  _populate_cmap_pair(&kp, k);
  _populate_cmap_pair(&vp, v);
  REQUIRE_EQ(clrucache_set_full(cache, &kp, &vp), ccol_success);

  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_success);
  REQUIRE_NOT_NULL(val_out.ptr);
  REQUIRE_EQ(*(int *)val_out.ptr, 42);

  /* The custom allocator allocated the copy that clrucache_get_full
   * returned. Free it with that same allocator. */
  _lru_custom_free(val_out.ptr);

  __clrucache_destroy(cache);
  REQUIRE_EQ(_lru_custom_alloc_count, _lru_custom_free_count);
}

TEST(custom_alloc, char_ptr_get_full_uses_custom_allocator) {
  _lru_custom_alloc_count = 0;
  _lru_custom_free_count = 0;
  ccol_memmgmt_procs_t mprocs = {_lru_custom_malloc, _lru_custom_free,
                                 _lru_custom_calloc, _lru_custom_realloc};

  char *err = NULL;
  clru_cache cache = clrucache_create_full(8, ccol_string, ccol_string, NULL,
                                           NULL, NULL, &mprocs, &err);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  char *k = "hello", *v = "world";
  cmap_pair kp = {}, vp = {};
  _populate_cmap_pair(&kp, k);
  _populate_cmap_pair(&vp, v);
  REQUIRE_EQ(clrucache_set_full(cache, &kp, &vp), ccol_success);

  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp, &val_out), ccol_success);
  REQUIRE_NOT_NULL(val_out.ptr);
  REQUIRE_STREQ((char *)val_out.ptr, "world");

  /* The custom allocator allocated the char* copy. Free it with that same
   * allocator. */
  _lru_custom_free(val_out.ptr);

  __clrucache_destroy(cache);
  REQUIRE_EQ(_lru_custom_alloc_count, _lru_custom_free_count);
}

TEST(custom_alloc, invalid_mprocs_returns_null) {
  ccol_memmgmt_procs_t bad = {_lru_custom_malloc, NULL, _lru_custom_calloc,
                              _lru_custom_realloc};
  char *err = NULL;
  clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                           NULL, &bad, &err);
  REQUIRE_EQ(cache, CLRU_CACHE_INVALID);
}

/*
 * A clrucache_get_full() call can run against a LIVE entry that the cache
 * already holds. Such a call can fail only because the copy for the caller
 * runs out of memory. It must not promote that entry to the front of the LRU
 * order. Consider a call to lru_move_to_front() on every path, before the
 * copy allocation runs. A hit that failed only because of short-lived
 * memory pressure would then keep an entry alive. It would do so at the
 * cost of an entry that a caller really set more recently.
 */
TEST(custom_alloc, get_full_copy_oom_failure_does_not_promote_lru) {
  _lru_fault_alloc_budget = -1; /* unlimited while seeding the cache */
  ccol_memmgmt_procs_t mprocs = {_lru_fault_malloc, _lru_fault_free,
                                 _lru_fault_calloc, _lru_fault_realloc};

  clru_cache cache = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL,
                                           NULL, &mprocs, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  /* k1 is the LRU, or oldest, entry. k2 is the MRU, or newest, one. */
  int k1 = 1, v1 = 111;
  cmap_pair kp1 = {}, vp1 = {};
  _populate_cmap_pair(&kp1, k1);
  _populate_cmap_pair(&vp1, v1);
  REQUIRE_EQ(clrucache_set_full(cache, &kp1, &vp1), ccol_success);

  int k2 = 2, v2 = 222;
  cmap_pair kp2 = {}, vp2 = {};
  _populate_cmap_pair(&kp2, k2);
  _populate_cmap_pair(&vp2, v2);
  REQUIRE_EQ(clrucache_set_full(cache, &kp2, &vp2), ccol_success);

  /* Force the next allocation to fail. The only allocation that a cache hit
   * makes is the copy that it hands back to the caller. This therefore
   * fails that one call every time. The test does not have to count every
   * allocation of its setup. */
  _lru_fault_alloc_budget = 0;
  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp1, &val_out), ccol_not_enough_memory);
  REQUIRE_NULL(val_out.ptr);

  /* Allocations succeed again for everything from here on. */
  _lru_fault_alloc_budget = -1;

  /* Insert a third key. A capacity of 2 forces exactly one eviction. k1
   * must still be the LRU victim, because its read failed only for lack of
   * memory and must not have promoted it. k2 must survive, because nothing
   * touched it again after its own set. */
  int k3 = 3, v3 = 333;
  cmap_pair kp3 = {}, vp3 = {};
  _populate_cmap_pair(&kp3, k3);
  _populate_cmap_pair(&vp3, v3);
  REQUIRE_EQ(clrucache_set_full(cache, &kp3, &vp3), ccol_success);
  REQUIRE_EQ(clrucache_size(cache), (size_t)2);

  cmap_pair out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp1, &out),
             ccol_key_not_found); /* correctly evicted as the true LRU entry */
  REQUIRE_EQ(clrucache_get_full(cache, &kp2, &out), ccol_success);
  REQUIRE_EQ(*(int *)out.ptr, 222);
  free(out.ptr);

  __clrucache_destroy(cache);
}

/* The last step of clrucache_create_full publishes the handle into the pin
 * index, and that step can fail. The index allocates a chunk and a stripe
 * block on its first use, with plain calloc. No allocator that a caller
 * supplies reaches that call.
 *
 * The rollback that the failure runs must push the half-claimed slot back
 * onto the free list. It must also clear the record that the cache keeps of
 * the handle. Without that, the next cache to take that slot inherits a
 * stale handle. Without the hook below, this path needs a real shortage of
 * memory, so an ordinary test run cannot reach it. */
extern void _ccol_pintable_force_next_publish_failure_for_tests(void);

TEST(clrucache_handle_lifecycle, handle_publish_failure_rolls_the_slot_back) {
  /* This test repeats the cycle, and asserts on how much the table grows
   * over the whole run. One cycle cannot tell a rollback apart from no
   * rollback. One lost slot makes the next create grow the table by one.
   * Nothing separates that from a table with no free slot at the start.
   * Over CYCLES rounds a rollback that works grows the table by
   * nothing. A missing push onto the free list grows it by one for each
   * round. */
  enum { CYCLES = 8 };
  size_t before = _clrucache_slot_table_capacity_for_tests();
  size_t free_before = _clrucache_free_index_count_for_tests();

  bool all_failed = true, all_created = true;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  int out = 0;

  for (int i = 0; i < CYCLES; i++) {
    _ccol_pintable_force_next_publish_failure_for_tests();
    char *err = NULL;
    clru_cache failed = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                              NULL, NULL, &err);
    if (failed != CLRU_CACHE_INVALID || err == NULL) {
      all_failed = false;
      if (failed != CLRU_CACHE_INVALID) __clrucache_destroy(failed);
      break;
    }

    /* The cache that takes the slot back from the rollback must be fully
     * usable. The set and the get below check that. Consider an index that
     * goes back onto the free list while its slot stays in a state that a
     * later acquire cannot build on. That shows up here as a get that
     * fails, and not as a lost slot. */
    clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                             NULL, NULL, NULL);
    if (cache == CLRU_CACHE_INVALID) {
      all_created = false;
      break;
    }
    clru_redeclare(cache, int, int);
    int k = 7, v = 42;
    out = 0;
    clru_set(cache, k, v);
    get_rv = clru_get(cache, k, &out);
    __clrucache_destroy(cache);
  }

  size_t after = _clrucache_slot_table_capacity_for_tests();
  size_t free_after = _clrucache_free_index_count_for_tests();

  REQUIRE_TRUE(all_failed);
  REQUIRE_TRUE(all_created);
  REQUIRE_EQ(get_rv, ccol_success);
  REQUIRE_EQ(out, 42);
  /* Growth alone is not enough: a lost slot only grows the table while the free
     list is empty, so a run in which earlier tests left several free indices
     behind would absorb every loss silently. The free list has to come back to
     where it started too, which holds however deep it was. */
  REQUIRE_LE(after, before + 2);
  REQUIRE_GE(free_after + 2, free_before);
}

/* ========================================================================== */
/*                       SEGMENTED (MULTI-SHARD) CACHES                       */
/* ========================================================================== */

/* The library divides a cache into segments with independent locks once the
 * capacity reaches 128. That is the first capacity that gives two segments of
 * 64 entries each. Every capacity below that threshold is one segment, and
 * that is what every other test in this file exercises.
 *
 * Without these tests, four things stay unreached. They are the hash that
 * chooses a segment, the eviction inside each segment, the sum of the sizes,
 * and the teardown of several segments. The hash loop in particular runs zero
 * times across the whole suite otherwise.
 *
 * These capacities straddle the threshold. One of them does not divide
 * evenly, because 200 over three segments is 67 + 67 + 66. How the code
 * spreads that remainder is what keeps the total exactly what the caller
 * asked for. */
static void clru_check_segmented_capacity(size_t capacity) {
  clru_construct(cache, int, int, capacity, NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  /* The reported capacity is the number of the caller. That holds whether
   * or not the library divided it, and whether or not it divided evenly. */
  size_t reported = clrucache_capacity(cache);
  size_t fresh_size = clrucache_size(cache);

  for (size_t i = 0; i < capacity; i++) {
    int k = (int)i, v = (int)i * 7;
    clru_set(cache, k, v);
  }
  size_t filled = clrucache_size(cache);

  /* Every key that the cache still holds must read back its own value.
   * Consider a segment chosen one way on an insert and another way on a
   * lookup. That shows up here as a miss, or worse as the value of another
   * key. */
  size_t readable = 0, wrong_value = 0;
  for (size_t i = 0; i < capacity; i++) {
    int k = (int)i, out = -1;
    if (clru_get(cache, k, &out) == ccol_success) {
      readable++;
      if (out != (int)i * 7) wrong_value++;
    }
  }

  /* Go well past the capacity. The total across the segments must stay at or
   * below that capacity. */
  for (size_t i = capacity; i < capacity * 3; i++) {
    int k = (int)i, v = (int)i;
    clru_set(cache, k, v);
  }
  size_t after_overfill = clrucache_size(cache);

  /* The destroy comes before the assertions. A REQUIRE_* returns from this
   * function on the spot. A cache left alive here would be reported as a
   * leak, on top of the real failure. */
  clru_destroy(cache);

  REQUIRE_EQ(reported, capacity);
  REQUIRE_EQ(fresh_size, (size_t)0);
  REQUIRE_EQ(wrong_value, (size_t)0);
  REQUIRE_EQ(readable, filled);
  REQUIRE_LE(filled, capacity);
  REQUIRE_LE(after_overfill, capacity);
  /* A hash spreads the keys, so segments fill unevenly. A cache filled to
   * exactly its capacity can therefore evict a few entries that one global
   * order would have kept. The bound below is what the documentation
   * promises. The loss stays small, and it is not proportional to the
   * number of segments. */
  /* The segment of a key comes from the hash of the map, which a secret of
     the process keys, so the shortfall differs from one run to the next.
     Over 100000 random spreads of these keys it averages 3.5 percent at a
     capacity of 128 and 4.3 at 256, and its largest values are 18 and 14
     percent; at 2048 the largest is 6.8. The bound is 25 percent, which no
     spread reached. A collapse of every key into one segment loses at least
     half of a cache of two segments, and the bound catches it. */
  REQUIRE_GE(filled, capacity - capacity / 4);
}

/* The library does not split a cache below the split threshold. Such a cache
 * keeps one exact global eviction order. It must keep every entry, and not
 * merely nearly all of them. The bound of the shared helper lets a few
 * percent through. That is right for a cache that the library split, and too
 * loose here. */
static void clru_check_unsplit_capacity_is_exact(size_t capacity) {
  clru_construct(cache, int, int, capacity, NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);
  for (size_t i = 0; i < capacity; i++) clru_set(cache, (int)i, (int)i);
  size_t filled = clrucache_size(cache);
  clru_destroy(cache);
  REQUIRE_EQ(filled, capacity);
}

TEST(segmented, capacity_below_the_split_threshold_is_one_segment) {
  /* 64 and 127 both divide to one segment. Each therefore keeps one exact,
   * global least-recently-used order, and must keep every entry. The check
   * here uses that exact number, and not the shared helper. The bound of
   * that helper is the one that a cache with several segments needs. */
  clru_check_unsplit_capacity_is_exact(64);
  clru_check_unsplit_capacity_is_exact(127);
  clru_check_segmented_capacity(64);
  clru_check_segmented_capacity(127);
}

TEST(segmented, capacity_at_and_above_the_split_threshold) {
  clru_check_segmented_capacity(128);  /* two segments   */
  clru_check_segmented_capacity(256);  /* four segments  */
  clru_check_segmented_capacity(2048); /* sixteen, the ceiling */
}

TEST(segmented, a_capacity_that_does_not_divide_evenly_keeps_its_remainder) {
  /* 200 over three segments is 67 + 67 + 66. To drop the remainder would
   * give three segments of 66. The cache would then hold 198 while
   * clrucache_capacity() still claims 200.
   *
   * This test asserts against the capacities of the segments themselves,
   * because nothing that a caller can observe would catch the loss.
   * clrucache_capacity() reports the requested number either way. The
   * eviction slack of a filled cache is also far wider than the one or two
   * entries at stake. This test is not vacuous: give every segment `base`
   * and discard the remainder, and the sum becomes 198.
   *
   * The expectations here are literal. The test does not compute them again
   * from the split of the implementation, so they cannot move with it. */
  static const size_t capacities[] = {200, 1000, 130};
  static const size_t expected_segments[] = {3, 15, 2};

  for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
    clru_construct(cache, int, int, capacities[i], NULL, NULL, NULL);
    size_t segments = 0;
    size_t sum = _clrucache_segment_capacity_sum_for_tests(cache, &segments);
    size_t reported = clrucache_capacity(cache);
    clru_destroy(cache);

    REQUIRE_EQ(segments, expected_segments[i]);
    REQUIRE_EQ(sum, capacities[i]);
    REQUIRE_EQ(reported, capacities[i]);
  }

  clru_check_segmented_capacity(200);
}

TEST(segmented, a_key_always_lands_in_the_same_segment) {
  /* The bytes of the key choose the segment. The same key must therefore
   * resolve to the same segment on every call. Consider a hash that reads
   * uninitialised padding, or that depends on anything but those bytes.
   * That shows up as a set that a later get cannot find.
   *
   * This test uses only half the capacity, on purpose. A hash spreads the
   * keys. A cache filled to exactly its capacity therefore evicts, quite
   * correctly, from whichever segments ran over their share. A miss then
   * says nothing about which segment a key landed in. Well under the limit,
   * no segment can overflow. Every miss here is therefore the defect that
   * this test looks for. */
  enum { CAP = 512, KEYS = CAP / 2, ROUNDS = 4 };
  clru_construct(cache, int, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  size_t misses = 0, wrong = 0;
  for (int round = 0; round < ROUNDS; round++) {
    for (int i = 0; i < KEYS; i++) {
      int k = i, v = i * 100 + round;
      clru_set(cache, k, v);
    }
    for (int i = 0; i < KEYS; i++) {
      int k = i, out = -1;
      if (clru_get(cache, k, &out) != ccol_success)
        misses++;
      else if (out != i * 100 + round)
        wrong++;
    }
  }

  clru_destroy(cache);

  REQUIRE_EQ(wrong, (size_t)0);
  REQUIRE_EQ(misses, (size_t)0);
}

/* A deterministic pseudo-random key stream: a key set with no structure,
 * beside the sequential and scaled sets of the checks around it. */
static uint32_t clru_spread_next(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

TEST(segmented, keys_sharing_their_low_bits_still_use_every_segment) {
  /* Real key sets hold their low bits constant all the time. Every pointer
   * from malloc is aligned to at least 16 bytes. An identifier that
   * something scaled by a block size or a page size carries that many
   * trailing zeroes.
   *
   * The segment that a key picks is a reduction of a hash, and a reduction
   * reads low bits. A hash whose low bits depend only on the low bits of the
   * key therefore sends such a set entirely to one segment.
   *
   * That is more than a wobble in the distribution. A segment holds only its
   * own share of the capacity. The cache therefore caps at that share in
   * silence, and evicts everything past it, while every other segment stays
   * empty. Every operation also serialises on the lock of that one segment.
   *
   * A hash whose low bits depend only on the low bits of the key, such as a
   * bare multiply, reduced with no finalizer, makes a cache of capacity 1024
   * hold 64 of these keys.
   *
   * The expectations here are literal numbers. The test derives nothing from
   * the segment count, so they cannot move with the implementation. */
  static const long long scales[] = {16, 64, 4096, 65536};
  enum { CAP = 1024 };

  for (size_t s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
    clru_construct(cache, long long, int, CAP, NULL, NULL, NULL);
    REQUIRE_NE(cache, CLRU_CACHE_INVALID);

    for (int i = 0; i < CAP; i++) {
      clru_set(cache, (long long)i * scales[s], i);
    }
    size_t filled = clrucache_size(cache);
    clru_destroy(cache);

    /* The hash of the map is keyed with a secret of the process, so these
     * keys spread over the 16 segments as random keys do. Over 100000 random
     * spreads of 1024 keys the shortfall averages 4.8 percent and its
     * largest value is 9.4. The bound is 12.5 percent, and the collapse
     * onto one segment loses 94. */
    REQUIRE_LE(filled, (size_t)CAP);
    REQUIRE_GE(filled, (size_t)(CAP - CAP / 8));
  }
}

TEST(segmented, an_unevenly_spread_key_set_keeps_nearly_all_of_its_capacity) {
  /* This is the shortfall that the documentation records. A filled cache
   * with several segments can evict a few entries that one global order
   * would have kept, because some segments run fuller than others. What must
   * hold is that the loss stays a small fraction of the capacity. It must
   * not grow with the number of segments.
   *
   * At this size, over 100000 random spreads, the shortfall averages 2.4
   * percent and its largest value is 4.8. The bound here is 6.25 percent,
   * and it still catches a real regression. */
  enum { CAP = 4096 };
  clru_construct(cache, int, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  uint32_t state = 0x1234567u;
  for (int i = 0; i < CAP; i++) {
    clru_set(cache, (int)(clru_spread_next(&state) & 0x7fffffff), i);
  }

  size_t filled = clrucache_size(cache);
  size_t capacity = clrucache_capacity(cache);
  clru_destroy(cache);

  REQUIRE_EQ(capacity, (size_t)CAP);
  REQUIRE_LE(filled, (size_t)CAP);
  REQUIRE_GE(filled, (size_t)CAP - (size_t)CAP / 16);
}

TEST(segmented, a_long_double_key_is_one_key_however_many_segments) {
  /* A long double is one key by VALUE, and never by representation. On an
   * ABI where the type carries padding, those bytes hold whatever was on the
   * stack. Segments are independent maps. The segment that a key picks must
   * therefore agree with that rule. Without that agreement, one logical key
   * becomes two entries in two segments, and a later get cannot find what a
   * set stored.
   *
   * Two byte images of the same number that differ only in their padding
   * must therefore behave the same way at every capacity. This test is not
   * vacuous. A segment chosen from the raw bytes of the key makes the case
   * with several segments report the key as missing.
   *
   * Only x87 extended precision has padding that can differ. Where the type
   * uses every byte that it has, the two images below are identical. IEEE
   * binary128 is one such type, and so is a long double that is a double.
   * The test then degenerates into a plain round trip. That must still pass,
   * and it proves nothing more.
   *
   * This test uses the raw function layer. Only a cmap_pair that the test
   * builds by hand can present two distinct byte images of one value. */
  static const size_t capacities[] = {64, 256};

  for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
    clru_cache cache =
        clrucache_create_full(capacities[c], ccol_long_double, ccol_int, NULL,
                              NULL, NULL, NULL, NULL);
    REQUIRE_NE(cache, CLRU_CACHE_INVALID);

    unsigned char image_a[sizeof(long double)];
    unsigned char image_b[sizeof(long double)];
    memset(image_a, 0x00, sizeof(image_a));
    memset(image_b, 0xAB, sizeof(image_b));

    /* Copy only the bytes that the value itself occupies. The two images
     * then differ exactly in the padding that the comparison of the type
     * ignores. */
    long double value = 3.5L;
    size_t significant = sizeof(long double);
#if LDBL_MANT_DIG == 64
    significant = 10; /* x87 extended precision: 10 bytes of 12 or 16 */
#endif
    memcpy(image_a, &value, significant);
    memcpy(image_b, &value, significant);

    cmap_pair key_a = {.ptr = image_a, .size = sizeof(long double)};
    cmap_pair key_b = {.ptr = image_b, .size = sizeof(long double)};
    int stored = 91, read_back = 0;
    cmap_pair val_in = {.ptr = &stored, .size = sizeof(stored)};

    ccol_retval_t set_rv = clrucache_set_full(cache, &key_a, &val_in);
    ccol_retval_t get_rv =
        __clrucache_get_into(cache, &key_b, &read_back, sizeof(read_back));
    size_t entries = clrucache_size(cache);

    __clrucache_destroy(cache);

    REQUIRE_EQ(set_rv, ccol_success);
    REQUIRE_EQ(get_rv, ccol_success);
    REQUIRE_EQ(read_back, 91);
    REQUIRE_EQ(entries, (size_t)1);
  }
}

TEST(segmented, a_key_size_that_disagrees_with_the_key_type_is_rejected) {
  /* The raw function layer takes a cmap_pair that the caller builds. The
   * size of that pair can therefore disagree with the width of the declared
   * key type. The choice of a segment reads the key. The code must answer
   * that disagreement before the read, and not after it. The answer must
   * also be the same whether or not the cache is large enough for the
   * library to split it into segments.
   *
   * This test is not vacuous in two ways. Without the check, the case with
   * several segments reads past the allocation of the caller.
   * AddressSanitizer reports a heap-buffer-overflow in the choice of the
   * segment. The call also reports the key as missing, and not as bad
   * arguments. */
  static const size_t capacities[] = {64, 256};

  for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
    clru_cache cache = clrucache_create_full(capacities[c], ccol_int, ccol_int,
                                             NULL, NULL, NULL, NULL, NULL);
    REQUIRE_NE(cache, CLRU_CACHE_INVALID);

    int *key_bytes = (int *)malloc(sizeof(int));
    /* Not asserted here: the cache is already built, and returning now would
       leak it. An allocation failure surfaces below as three invalid_args
       results that never happened. */
    if (key_bytes) *key_bytes = 5;

    int value = 1, read_back = 0;
    /* One int, described as far larger than one int. */
    cmap_pair oversized = {.ptr = key_bytes, .size = 4096};
    cmap_pair val_in = {.ptr = &value, .size = sizeof(value)};
    cmap_pair val_out = {.ptr = &read_back, .size = sizeof(read_back)};

    ccol_retval_t get_rv = clrucache_get_full(cache, &oversized, &val_out);
    ccol_retval_t set_rv = clrucache_set_full(cache, &oversized, &val_in);
    ccol_retval_t into_rv =
        __clrucache_get_into(cache, &oversized, &read_back, sizeof(read_back));

    bool key_allocated = (key_bytes != NULL);
    free(key_bytes);
    __clrucache_destroy(cache);

    REQUIRE_TRUE(key_allocated);
    REQUIRE_EQ(get_rv, ccol_invalid_args);
    REQUIRE_EQ(set_rv, ccol_invalid_args);
    REQUIRE_EQ(into_rv, ccol_invalid_args);
  }
}

TEST(segmented, string_keys_spread_across_segments_and_round_trip) {
  /* A char* key carries its bytes plus the terminator. Such a key therefore
   * puts a different length through the same hash than the integer keys of
   * fixed width above do. These keys also spread unevenly, where sequential
   * integers happen not to. This test uses half the capacity, for the reason
   * that the integer test above gives. */
  enum { CAP = 512, KEYS = CAP / 2 };
  clru_construct(cache, char *, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  size_t misses = 0, wrong = 0;
  for (int i = 0; i < KEYS; i++) {
    char key[32];
    snprintf(key, sizeof(key), "session:%d", i);
    int v = i * 3;
    clru_set(cache, key, v);
  }
  for (int i = 0; i < KEYS; i++) {
    char key[32];
    snprintf(key, sizeof(key), "session:%d", i);
    int out = -1;
    if (clru_get(cache, key, &out) != ccol_success)
      misses++;
    else if (out != i * 3)
      wrong++;
  }

  clru_destroy(cache);

  REQUIRE_EQ(wrong, (size_t)0);
  REQUIRE_EQ(misses, (size_t)0);
}

extern size_t _clrucache_keyed_segment_maps_for_tests(clru_cache cache,
                                                      size_t *out_segments);

/* The inverse of the odd multiplier a, modulo 2^64. */
static uint64_t clru_mul_inverse(uint64_t a) {
  uint64_t x = a;
  for (int i = 0; i < 6; i++) x *= 2 - a * x;
  return x;
}

/* Keys i * M^-1, where M is the multiplier of the fast hash of a chashmap,
 * all share home slot 0 of every map. The segment chooser hashes with the
 * keyed hash of the map, so the keys still spread over the segments, and
 * each segment map receives a flood of its own. Every segment map switches
 * to the keyed mode, and the cache loses no key: each one is found in the
 * segment that the chooser gives, before and after the switch. A double key
 * of -0.0 and of 0.0 is one key in a cache whose maps have switched. This
 * test is non-vacuous: without the detection of chashmap, no segment map
 * switches. */
TEST(segmented, a_flood_switches_every_segment_map_and_loses_no_key) {
  enum { CAP = 8192, KEYS = 6000 };
  const uint64_t inv = clru_mul_inverse(0x9E3779B97F4A7C15ULL);
  clru_construct(cache, long long, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);
  size_t misses = 0, wrong = 0;
  for (uint64_t i = 0; i < KEYS; i++) {
    long long k = (long long)(i * inv);
    int v = (int)i;
    clru_set(cache, k, v);
  }
  size_t segments = 0;
  size_t keyed = _clrucache_keyed_segment_maps_for_tests(cache, &segments);
  for (uint64_t i = 0; i < KEYS; i++) {
    long long k = (long long)(i * inv);
    int out = -1;
    if (clru_get(cache, k, &out) != ccol_success)
      misses++;
    else if (out != (int)i)
      wrong++;
  }
  size_t size = clrucache_size(cache);
  clru_destroy(cache);

  /* The same flood as doubles, then -0.0 and 0.0. */
  clru_construct(dcache, double, int, CAP, NULL, NULL, NULL);
  REQUIRE_NE(dcache, CLRU_CACHE_INVALID);
  for (uint64_t i = 1; i <= KEYS; i++) {
    uint64_t bits = i * inv;
    double k;
    memcpy(&k, &bits, sizeof(k));
    if (k != k) continue; /* every NaN is its own key; leave them out */
    int v = (int)i;
    clru_set(dcache, k, v);
  }
  size_t dsegments = 0;
  size_t dkeyed = _clrucache_keyed_segment_maps_for_tests(dcache, &dsegments);
  double negative_zero = -0.0, positive_zero = 0.0;
  int zero_val = 77, zero_out = -1;
  clru_set(dcache, negative_zero, zero_val);
  ccol_retval_t zr = clru_get(dcache, positive_zero, &zero_out);
  clru_destroy(dcache);

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

/* ========================================================================== */
/*        CLRU_CACHE HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)          */
/* ========================================================================== */

/* This group mirrors the chttpcli_handle_lifecycle and
 * ccol_event_loop_handle_lifecycle test groups. It is adapted for the pin
 * mechanism of clru_cache. A resolve takes no lock, and it writes nothing
 * that another thread reads. A destroy waits out every pin that the library
 * granted before it frees anything. */

/* A destroy completes in full. A second destroy call then runs on a copy of
 * the same handle value that the test holds on its own. That must be a fatal
 * error. This test runs in a forked child, because ccol_fatal_err stops the
 * whole process. tests/clogger/tests.c sets the precedent for a fork test of
 * misuse that stops the process. */
TEST(clrucache_handle_lifecycle, sequential_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                             NULL, NULL, NULL);
    if (cache == CLRU_CACHE_INVALID) _exit(2);
    clru_cache stale = cache;   /* an independently-held copy of the handle
          value, distinct from the local the macro below invalidates */
    clru_destroy(cache);        /* completes normally; the local `cache` is now
               CLRU_CACHE_INVALID, but `stale` still holds the original value */
    __clrucache_destroy(stale); /* the actual misuse under test: a second,
        purely sequential destroy of a handle already fully torn down */
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  clru_cache h;
} clru_concurrent_destroy_arg_t;

static void *clru_concurrent_destroy_thread(void *arg) {
  clru_concurrent_destroy_arg_t *a = (clru_concurrent_destroy_arg_t *)arg;
  __clrucache_destroy(a->h);
  return NULL;
}

/* Two threads each hold their own copy of the SAME handle, which is still
 * valid. Both call destroy, as close to the same moment as the test can
 * arrange. That must also be fatal. It is the same class of concurrent
 * double free that the generation-tagged slot table closes for chttpcli,
 * for chttpsvr and for ccol_event_loop. */
TEST(clrucache_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                             NULL, NULL, NULL);
    if (cache == CLRU_CACHE_INVALID) _exit(2);
    clru_concurrent_destroy_arg_t a1 = {.h = cache};
    clru_concurrent_destroy_arg_t a2 = {.h = cache};
    pthread_t t1, t2;
    /* This code runs inside a forked child, where REQUIRE_* is unsafe. Its
     * early return would skip the _exit() of this branch, and fall back
     * into the test loop of the harness a second time. A create that fails
     * therefore falls through to a distinct exit that is not SIGABRT. The
     * WIFSIGNALED and SIGABRT check of the parent below turns that into a
     * clean test failure. The child never joins a pthread_t that holds
     * garbage because nothing created it. */
    if (pthread_create(&t1, NULL, clru_concurrent_destroy_thread, &a1) != 0)
      _exit(2);
    if (pthread_create(&t2, NULL, clru_concurrent_destroy_thread, &a2) != 0)
      _exit(2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* Nothing reaches this. Whichever of the two destroy calls
                  loses the race must reach ccol_fatal_err(). */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* This test reuses slow_remote_getter, which the COALESCING GETTERS section
 * above defines. That getter sleeps 100ms before it returns. It therefore
 * holds the pin of a clrucache_get_full call open for a long time that the
 * test controls directly.
 *
 * ccol_event_loop has no slow public entry point of its own, so it carries a
 * dedicated resolve_pin_and_sleep_for_tests hook. clru_cache needs no such
 * hook. Its remote-getter mechanism already gives every caller a way to
 * block for as long as the application wants. The caller still holds the pin
 * of its resolve for that whole time. */

typedef struct {
  clru_cache h;
  ccol_retval_t rv;
} clru_slow_get_arg_t;

static void *clru_slow_get_thread(void *arg) {
  clru_slow_get_arg_t *a = (clru_slow_get_arg_t *)arg;
  clru_cache c = a->h;
  clru_redeclare(c, int, int);
  int out = 0;
  a->rv = clru_get(c, 42, &out);
  return NULL;
}

/* A destroy must respect a pin that is in flight. This test races a thread
 * that blocks inside the remote-getter call of clrucache_get_full, and still
 * holds its pin, against a clru_destroy on the same handle. The destroy must
 * block until that thread releases the pin. It must not run ahead and free
 * the cache out from under a pointer that is still resolved. */
TEST(clrucache_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  clru_construct(cache, int, int, 8, slow_remote_getter, NULL, NULL);

  clru_slow_get_arg_t get_arg = {.h = cache, .rv = ccol_success};
  pthread_t get_thread;
  REQUIRE_EQ(pthread_create(&get_thread, NULL, clru_slow_get_thread, &get_arg),
             0);

  /* Give the getter thread a short head start. Its resolve, and therefore
   * its pin, then certainly happens before the destroy fires. */
  struct timespec startup = {.tv_sec = 0, .tv_nsec = 10000000}; /* 10 ms */
  nanosleep(&startup, NULL);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  clru_destroy(cache); /* This must block until the 100ms remote_getter call
                            of the getter thread ends in full. That thread
                            still holds the pin. */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(get_thread, NULL);
  REQUIRE_EQ(get_arg.rv, ccol_success);
  /* The getter thread slept about 100ms while it held the pin. A destroy
   * that returns well under that did NOT wait for the pin. That is, the
   * resolve-then-use protection failed. */
  REQUIRE_GT(elapsed_ms, 50L);
}

/* A remote setter that sleeps and then succeeds. It gives a
 * clrucache_set_full call a long window that the test controls directly.
 * The call still holds the pin of its resolve for that window. This
 * mirrors the role of slow_remote_getter in
 * resolve_then_use_race_destroy_waits above.
 *
 * The control flow of the set path around the pin differs in a real way from
 * that of the get path. The set path removes an entry from the LRU and
 * restores it, and it brackets the remote call with its own waiters++ and
 * waiters--. This is why the set path has a test of its own. The test for
 * the get side does not stand in for it. */
static bool slow_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  (void)val;
  usleep(100000); /* 100 ms */
  return true;
}

typedef struct {
  clru_cache h;
  ccol_retval_t rv;
} clru_slow_set_arg_t;

static void *clru_slow_set_thread(void *arg) {
  clru_slow_set_arg_t *a = (clru_slow_set_arg_t *)arg;
  clru_cache c = a->h;
  clru_redeclare(c, int, int);
  int k = 42, v = 100;
  a->rv = clru_set(c, k, v);
  return NULL;
}

/* Set-side counterpart to resolve_then_use_race_destroy_waits: races a
 * thread blocked inside clrucache_set_full's remote-setter call (still
 * holding its pin) against a concurrent clru_destroy on the same handle.
 * destroy must block until the pin is released here too, not just for the
 * get path. */
TEST(clrucache_handle_lifecycle, resolve_then_use_race_destroy_waits_for_set) {
  clru_construct(cache, int, int, 8, NULL, slow_remote_setter, NULL);

  clru_slow_set_arg_t set_arg = {.h = cache, .rv = ccol_unexpected_failure};
  pthread_t set_thread;
  REQUIRE_EQ(pthread_create(&set_thread, NULL, clru_slow_set_thread, &set_arg),
             0);

  /* Give the setter thread a short head start. Its resolve, and therefore
   * its pin, then certainly happens before the destroy fires. */
  struct timespec startup = {.tv_sec = 0, .tv_nsec = 10000000}; /* 10 ms */
  nanosleep(&startup, NULL);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  clru_destroy(cache); /* This must block until the 100ms remote_setter call
                            of the setter thread ends in full. That thread
                            still holds the pin. */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(set_thread, NULL);
  REQUIRE_EQ(set_arg.rv, ccol_success);
  /* The setter thread slept about 100ms while it held the pin. A destroy
   * that returns well under that did NOT wait for the pin. That is, the
   * resolve-then-use protection failed for the set path. */
  REQUIRE_GT(elapsed_ms, 50L);
}

typedef struct {
  clru_cache h;
} clru_capacity_arg_t;

static void *clru_capacity_thread(void *arg) {
  clru_capacity_arg_t *a = (clru_capacity_arg_t *)arg;
  /* This code ignores the return value on purpose. A legitimate race with a
   * destroy on another thread can make this resolve fail and return 0, in
   * place of a success. Both outcomes are correct. This thread exists only
   * to generate resolve, pin and unpin traffic beside the destroy thread
   * below. */
  clrucache_capacity(a->h);
  return NULL;
}

/* This test differs from resolve_then_use_race_destroy_waits above, and it
 * does not repeat it. That test holds a long pin on purpose, which keeps the
 * pin count above zero for the whole race window.
 *
 * This test needs the opposite shape. It races a fast entry point that never
 * blocks against a destroy on another thread. clrucache_capacity is that
 * entry point. It resolves, reads one field with no lock, unpins and
 * returns. The test repeats the race under stress, because the failure
 * window of a fast pin and unpin pair is only a handful of instructions
 * wide. One run with no stress does not reproduce it reliably.
 *
 * Each iteration builds a fresh cache. Every repetition therefore gets its
 * own independent race, and none of them reuses a handle that a destroy
 * already took down. */
TEST(clrucache_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                             NULL, NULL, NULL);
    REQUIRE_NE(cache, CLRU_CACHE_INVALID);

    clru_capacity_arg_t capacity_arg = {.h = cache};
    clru_concurrent_destroy_arg_t destroy_arg = {.h = cache};
    pthread_t capacity_tid, destroy_tid;
    bool started_capacity_tid =
        (pthread_create(&capacity_tid, NULL, clru_capacity_thread,
                        &capacity_arg) == 0);
    bool started_destroy_tid =
        (pthread_create(&destroy_tid, NULL, clru_concurrent_destroy_thread,
                        &destroy_arg) == 0);
    if (started_capacity_tid) pthread_join(capacity_tid, NULL);
    if (started_destroy_tid) pthread_join(destroy_tid, NULL);
    REQUIRE_TRUE(started_capacity_tid);
    REQUIRE_TRUE(started_destroy_tid);
  }
}

/* Legitimate slot reuse must never be confused with a stale handle to the
 * slot's previous occupant; the whole point of the generation counter. */
TEST(clrucache_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  clru_cache a = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, NULL);
  REQUIRE_NE(a, CLRU_CACHE_INVALID);
  clru_cache stale_a = a;
  clru_destroy(a);

  clru_cache b = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL, NULL,
                                       NULL, NULL);
  REQUIRE_NE(b, CLRU_CACHE_INVALID);

  /* B's operations must succeed normally regardless of whether the
   * allocator happened to reuse A's exact address for B. */
  REQUIRE_EQ(clrucache_capacity(b), (size_t)8);

  /* A's stale handle must never resolve to B, even if it reused the same
   * underlying address; the whole point of the generation counter. */
  REQUIRE_EQ((void *)_clrucache_resolve_for_tests(stale_a), NULL);

  clru_destroy(b);
}

/* The slot table is bounded, and it does not grow for ever. Consider a loop
 * that creates and destroys, with only one slot ever in flight at a time.
 * That loop must reuse the one freed slot on every iteration. It must not
 * grow the table further.
 *
 * This test captures the capacity right after the first create and destroy
 * pair. It does not assert a fixed absolute value such as 1. Earlier tests
 * in this same process may already have grown the table to some N above 1.
 * What this test must prove is that ITS OWN churn adds no more
 * growth. The absolute size of the table when it runs does not matter. */
TEST(clrucache_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  clru_cache cache0 = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                            NULL, NULL, NULL);
  REQUIRE_NE(cache0, CLRU_CACHE_INVALID);
  clru_destroy(cache0);
  size_t capacity_after_first = _clrucache_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                             NULL, NULL, NULL);
    REQUIRE_NE(cache, CLRU_CACHE_INVALID);
    clru_destroy(cache);
  }

  REQUIRE_EQ(_clrucache_slot_table_capacity_for_tests(), capacity_after_first);
}

/* ========================================================================== */
/* Allocation-failure sweep over cache construction                           */
/*                                                                            */
/* clrucache_create_full builds four things before it returns. They are a */
/* slot entry, the backing map, the records of the eviction list, and the */
/* condition variable of each entry. Each failure point unwinds a different */
/* amount of that work. Nothing else in this suite runs those branches. */
/* Without this sweep, the documented CLRU_CACHE_INVALID return, and the */
/* frees that go with it, stay unverified. */
/*                                                                            */
/* One counter covers all four procs. The cache struct and the map both come */
/* from calloc. A sweep that failed only malloc could not reach the code */
/* that unwinds them. */
/* ========================================================================== */

static atomic_int g_lru_alloc_seen = 0;
static atomic_int g_lru_fail_at = 0; /* 0 disarms */

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
static ccol_memmgmt_procs_t g_lru_sweep_procs = {
    _lru_sweep_malloc, _lru_sweep_free, _lru_sweep_calloc, _lru_sweep_realloc};

static void _lru_arm(int nth) {
  atomic_store(&g_lru_alloc_seen, 0);
  atomic_store(&g_lru_fail_at, nth);
}
static void _lru_disarm(void) { atomic_store(&g_lru_fail_at, 0); }

#define LRU_SWEEP_DEPTH 24

TEST(clrucache_oom, integral_cache_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= LRU_SWEEP_DEPTH; n++) {
    _lru_arm(n);
    char *err = NULL;
    clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                             NULL, &g_lru_sweep_procs, &err);
    _lru_disarm();
    if (cache != CLRU_CACHE_INVALID) {
      __clrucache_destroy(cache);
    } else if (!err) {
      all_handled = false;
    }
  }
  REQUIRE_TRUE(all_handled);
}

TEST(clrucache_oom,
     string_keyed_cache_construction_unwinds_at_every_allocation) {
  /* A string key type selects the separate-chaining map rather than the
   * open-addressed one, which allocates a different shape. */
  bool all_handled = true;
  for (int n = 1; n <= LRU_SWEEP_DEPTH; n++) {
    _lru_arm(n);
    char *err = NULL;
    clru_cache cache =
        clrucache_create_full(8, ccol_string, ccol_string, NULL, NULL, NULL,
                              &g_lru_sweep_procs, &err);
    _lru_disarm();
    if (cache != CLRU_CACHE_INVALID)
      __clrucache_destroy(cache);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(clrucache_oom, a_large_capacity_cache_unwinds_at_every_allocation) {
  /* A bigger capacity pushes construction further before it can fail, so the
   * later failure points become reachable at all. */
  bool all_handled = true;
  for (int n = 1; n <= 40; n++) {
    _lru_arm(n);
    clru_cache cache =
        clrucache_create_full(256, ccol_long_long, ccol_string, NULL, NULL,
                              NULL, &g_lru_sweep_procs, NULL);
    _lru_disarm();
    if (cache != CLRU_CACHE_INVALID) __clrucache_destroy(cache);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(clrucache_oom, a_cache_built_under_a_late_failure_still_stores_and_reads) {
  /* Separates "unwound correctly" from "returned a corpse": whatever survives
   * has to behave like a fully built cache. */
  bool built = false, works = false;
  for (int n = 12; n <= 60 && !built; n++) {
    _lru_arm(n);
    clru_cache cache = clrucache_create_full(4, ccol_int, ccol_int, NULL, NULL,
                                             NULL, &g_lru_sweep_procs, NULL);
    _lru_disarm();
    if (cache != CLRU_CACHE_INVALID) {
      built = true;
      int key = 7, val = 42;
      cmap_pair kp = {}, vp = {};
      _populate_cmap_pair(&kp, key);
      _populate_cmap_pair(&vp, val);
      if (clrucache_set_full(cache, &kp, &vp) == ccol_success) {
        /* get_full hands back storage it allocated; the caller owns it. */
        cmap_pair val_out = {};
        if (clrucache_get_full(cache, &kp, &val_out) == ccol_success &&
            val_out.ptr && val_out.size == sizeof(int)) {
          works = (*(int *)val_out.ptr == 42);
        }
        free(val_out.ptr);
      }
      __clrucache_destroy(cache);
    }
  }
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(works);
}

/* ========================================================================== */
/*                     SLOT TABLE LOST-FREE-INDEX RECOVERY                    */
/* ========================================================================== */

extern void _clrucache_force_next_free_index_push_failure_for_tests(void);
extern size_t _clrucache_slot_table_capacity_for_tests(void);

/*
 * A release of a slot pushes its index back onto the free list. That push
 * allocates, so it can fail. Without recovery, the index is stranded for the
 * life of the process, and every later create grows the table again.
 *
 * The hook that forces a failure makes each destroy below lose its index. A
 * table that never reclaims therefore grows one time for each iteration.
 *
 * This test is not vacuous. Remove the reclaim branch from
 * _clrucache_handle_slot_acquire, and the final capacity goes past the
 * tolerance below by the number of iterations.
 */
TEST(clrucache_slot_table, a_lost_free_index_is_reclaimed_rather_than_growing) {
  /* Warm the table first, so `before` reflects a steady state rather than
     first-use growth. */
  {
    clru_construct(warm, int, int, 8, NULL, NULL, NULL);
    clru_destroy(warm);
  }
  size_t before = _clrucache_slot_table_capacity_for_tests();

  bool all_built = true;
  for (int i = 0; i < 4; i++) {
    clru_construct(cache, int, int, 8, NULL, NULL, NULL);
    if (!cache) all_built = false;
    _clrucache_force_next_free_index_push_failure_for_tests();
    clru_destroy(cache);
  }

  /* A cache that must come from a reclaimed slot, and must be fully usable:
     an index handed back while still carrying stale bookkeeping would fail
     on its first real operation rather than at acquire time. */
  bool works = false;
  {
    clru_construct(cache, int, int, 8, NULL, NULL, NULL);
    if (cache) {
      int k = 7, v = 99, out = 0;
      works = (clru_set(cache, k, v) == ccol_success &&
               clru_get(cache, k, &out) == ccol_success && out == 99);
    }
    clru_destroy(cache);
  }

  size_t after = _clrucache_slot_table_capacity_for_tests();

  REQUIRE_TRUE(all_built);
  REQUIRE_TRUE(works);
  /* One extra slot is tolerated: a create and its destroy can interleave with
     the warm-up's own release on the same table. */
  REQUIRE_LE(after, before + 1);
}

/* ========================================================================== */
/*                    KEYS WITH PADDING THROUGH THE MACROS                    */
/* ========================================================================== */

typedef struct {
  char tag;
  long id; /* the padding between tag and id is what this section is about */
} clru_padded_key;

#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define CLRU_TEST_COMPILER_CLEARS_PADDING 1
#endif
#endif

#ifdef CLRU_TEST_COMPILER_CLEARS_PADDING
static _Atomic int clru_padded_getter_calls = 0;
static _Atomic bool clru_padded_getter_saw_dirty_padding = false;

/* Answers every miss with a value that no set in the test stores, so a miss
 * that should have been a hit shows up as a wrong value. It also reports
 * whether the key that the cache hands it carries a padding byte that is not
 * zero. That key is the macro's own copy. */
static bool clru_padded_getter(const cmap_pair *key, cmap_pair *val) {
  atomic_fetch_add(&clru_padded_getter_calls, 1);
  const unsigned char *b = (const unsigned char *)key->ptr;
  for (size_t j = offsetof(clru_padded_key, tag) + 1;
       j < offsetof(clru_padded_key, id); j++) {
    if (b[j] != 0) atomic_store(&clru_padded_getter_saw_dirty_padding, true);
  }
  int *v = malloc(sizeof(int));
  if (!v) return false;
  *v = -1;
  val->ptr = v;
  val->size = sizeof(int);
  return true;
}

/* Builds a key in storage whose padding holds `fill`, and hands it back
 * through a pointer that the compiler cannot see through. A struct copy out
 * of that storage then copies the fill bytes with the members, so a macro
 * that does not clear the padding of its copy passes the fill to the cache.
 */
static __attribute__((noinline)) const clru_padded_key *clru_padded_make(
    clru_padded_key *storage, unsigned char fill, long id) {
  memset(storage, fill, sizeof(*storage));
  storage->tag = 'k';
  storage->id = id;
  const clru_padded_key *p = storage;
  __asm__ volatile("" : "+r"(p) : : "memory");
  return p;
}
#endif

/* Both the choice of segment and the map of a segment read every byte of a
 * struct key. The macros clear the padding of their own copy of the key, so a
 * key that clru_set() stored is found again by clru_get() with a key of equal
 * members, whatever the padding of the caller's object holds. The cache is
 * large enough to have several segments, so the choice of segment is covered
 * as well as the map. This test is non-vacuous under GCC: without the
 * clearing, the set stores the 0xAA padding and the get looks up the 0x55
 * padding, every lookup misses, and the getter sees dirty padding. */
TEST(padded_keys, macros_find_a_struct_key_whatever_its_padding_holds) {
#ifdef CLRU_TEST_COMPILER_CLEARS_PADDING
  atomic_store(&clru_padded_getter_calls, 0);
  atomic_store(&clru_padded_getter_saw_dirty_padding, false);
  clru_construct(cache, clru_padded_key, int, 1024, clru_padded_getter, NULL,
                 NULL);
  size_t segments = 0;
  (void)_clrucache_segment_capacity_sum_for_tests(cache, &segments);

  clru_padded_key storage;
  bool all_set = true;
  for (long i = 0; i < 100; i++) {
    int v = (int)i;
    if (clru_set(cache, *clru_padded_make(&storage, 0xAA, i), v) !=
        ccol_success)
      all_set = false;
  }
  int found = 0;
  for (long i = 0; i < 100; i++) {
    int out = -2;
    if (clru_get(cache, *clru_padded_make(&storage, 0x55, i), &out) ==
            ccol_success &&
        out == (int)i)
      found++;
  }
  /* One genuine miss, so the getter inspects a key copy at least once. */
  int miss_out = 0;
  ccol_retval_t miss_r =
      clru_get(cache, *clru_padded_make(&storage, 0x5A, 5000), &miss_out);
  size_t size = clrucache_size(cache);
  clru_destroy(cache);

  REQUIRE_GT(segments, (size_t)1);
  REQUIRE_TRUE(all_set);
  REQUIRE_EQ(found, 100);
  REQUIRE_EQ(miss_r, ccol_success);
  REQUIRE_EQ(miss_out, -1);
  REQUIRE_EQ(atomic_load(&clru_padded_getter_calls), 1);
  REQUIRE_FALSE(atomic_load(&clru_padded_getter_saw_dirty_padding));
  REQUIRE_EQ(size, (size_t)101);
#endif
}

/* ========================================================================== */
/*                    CONST CHARACTER-POINTER ARGUMENTS                       */
/* ========================================================================== */

/* A cache declared with a character-pointer key or value type accepts a
 * const char * key and value without a cast. This suite builds with -Werror,
 * so a macro that copied such an argument into a char * temporary fails to
 * compile here with "discards const qualifier". */
TEST(const_char_args, const_char_key_and_value_need_no_cast) {
  const char *const_key = "alpha";
  const char *const_val = "one";
  char *const pinned_key = "beta";

  clru_construct(by_name, char *, int, 16, NULL, NULL, NULL);
  int v = 7, out = 0, out2 = 0;
  ccol_retval_t r_set = clru_set(by_name, const_key, v);
  ccol_retval_t r_get = clru_get(by_name, const_key, &out);
  ccol_retval_t r_set2 = clru_set(by_name, pinned_key, 8);
  ccol_retval_t r_get2 = clru_get(by_name, "beta", &out2);
  clru_destroy(by_name);

  clru_construct(names, int, char *, 16, NULL, NULL, NULL);
  int k = 1;
  char *s = NULL;
  ccol_retval_t r_set3 = clru_set(names, k, const_val);
  ccol_retval_t r_get3 = clru_get(names, k, &s);
  bool matches = s && strcmp(s, "one") == 0;
  free(s);
  clru_destroy(names);

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

/* ========================================================================== */
/*              A GETTER THAT WAITED ON A SET THAT FAILED                     */
/* ========================================================================== */

extern int _clrucache_key_waiters_for_tests(clru_cache cache,
                                            const cmap_pair *key_pair);

/* The backing store of this section holds 100 for every key. */
static _Atomic int failset_getter_calls = 0;
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

/* Fails a set of failset_gated_key once the gate is armed, and parks inside
 * that call until the test releases it. Every other set succeeds at once. */
static _Atomic bool failset_armed = false;
static _Atomic int failset_gated_key = 0;
static _Atomic bool failset_setter_entered = false;
static _Atomic bool failset_setter_release = false;
static bool failset_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)val;
  int k;
  memcpy(&k, key->ptr, sizeof(k));
  if (!atomic_load(&failset_armed) || k != atomic_load(&failset_gated_key))
    return true;
  atomic_store(&failset_setter_entered, true);
  (void)clru_await_flag(&failset_setter_release);
  return false;
}

typedef struct {
  clru_cache cache;
  int key;
  int val;
  ccol_retval_t retval;
} failset_setter_arg_t;

static void *failset_setter_thread(void *arg) {
  failset_setter_arg_t *a = (failset_setter_arg_t *)arg;
  clru_cache cache = a->cache;
  clru_redeclare(cache, int, int);
  a->retval = clru_set(cache, a->key, a->val);
  return NULL;
}

typedef struct {
  clru_cache cache;
  int key;
  cmap_pair out;
  ccol_retval_t retval;
} full_waiter_arg_t;

static void *full_waiter_thread(void *arg) {
  full_waiter_arg_t *a = (full_waiter_arg_t *)arg;
  cmap_pair kp = {.ptr = &a->key, .size = sizeof(a->key)};
  a->retval = clrucache_get_full(a->cache, &kp, &a->out);
  return NULL;
}

/* Waits, bounded, until the entry of key holds `want` waiter references. */
static bool failset_await_waiters(clru_cache cache, int key, int want) {
  cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
  for (int waited_ms = 0; waited_ms < CLRU_GATE_WAIT_MS; waited_ms++) {
    if (_clrucache_key_waiters_for_tests(cache, &kp) == want) return true;
    usleep(1000);
  }
  return _clrucache_key_waiters_for_tests(cache, &kp) == want;
}

static void failset_reset(int gated_key) {
  atomic_store(&failset_getter_calls, 0);
  atomic_store(&failset_armed, false);
  atomic_store(&failset_gated_key, gated_key);
  atomic_store(&failset_setter_entered, false);
  atomic_store(&failset_setter_release, false);
}

/* A get of an absent key coalesces onto a set that creates the key, and that
 * set fails. Nobody asked the remote getter, so the get must not answer "not
 * found": it continues as the miss that it is, and the remote getter answers
 * it. The test waits for the getter's waiter reference on the entry (the
 * setter holds the other one) before it lets the set fail, so the get really
 * is coalesced onto the set. This test is non-vacuous: when the waiter takes
 * the failed set as its answer, it reports ccol_key_not_found and the remote
 * getter is never called. Both clru_get() paths are covered, the one that
 * reads into a fixed-size value and the one for clrucache_get_full(). */
TEST(failed_set_waiter, getter_on_a_new_key_runs_the_remote_getter) {
  for (int use_full = 0; use_full < 2; use_full++) {
    failset_reset(1);
    clru_construct(cache, int, int, 16, failset_remote_getter,
                   failset_remote_setter, NULL);
    atomic_store(&failset_armed, true);

    failset_setter_arg_t sarg = {cache, 1, 200, ccol_success};
    pthread_t stid;
    bool setter_started =
        pthread_create(&stid, NULL, failset_setter_thread, &sarg) == 0;
    bool entered = setter_started && clru_await_flag(&failset_setter_entered);

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
    ccol_retval_t after_r = clru_get(cache, 1, &after);
    int calls = atomic_load(&failset_getter_calls);
    clru_destroy(cache);

    int got = -1;
    ccol_retval_t got_r = use_full ? farg.retval : warg.retval;
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
    /* The fetched value is cached: a later get is a hit. */
    REQUIRE_EQ(after_r, ccol_success);
    REQUIRE_EQ(after, 100);
  }
}

/* The same rule for a key that the cache held when the set began. While the
 * remote setter runs, an insert of another key evicts the key of the set, and
 * the set then fails, so the key is absent. The get that waited on that set
 * continues as a miss and the remote getter answers it. This test is
 * non-vacuous: when the waiter takes the failed set as its answer, it reports
 * ccol_key_not_found and the remote getter is never called. */
TEST(failed_set_waiter,
     getter_on_a_key_evicted_during_the_set_runs_the_getter) {
  failset_reset(1);
  /* Capacity 1: one segment, and any second key evicts the first. */
  clru_construct(cache, int, int, 1, failset_remote_getter,
                 failset_remote_setter, NULL);
  int k1 = 1, seed = 10;
  ccol_retval_t seed_r = clru_set(cache, k1, seed);
  atomic_store(&failset_armed, true);

  failset_setter_arg_t sarg = {cache, 1, 200, ccol_success};
  pthread_t stid;
  bool setter_started =
      pthread_create(&stid, NULL, failset_setter_thread, &sarg) == 0;
  bool entered = setter_started && clru_await_flag(&failset_setter_entered);

  waiter_arg_t warg = {cache, 1, 0, -1, ccol_invalid_args, false};
  pthread_t wtid;
  bool waiter_started = false;
  if (entered)
    waiter_started = pthread_create(&wtid, NULL, waiter_thread, &warg) == 0;
  bool coalesced = waiter_started && failset_await_waiters(cache, 1, 2);

  /* Evict key 1 while its set is still in flight. */
  ccol_retval_t evict_r = ccol_invalid_args;
  if (coalesced) evict_r = clru_set(cache, 2, 20);

  atomic_store(&failset_setter_release, true);
  if (setter_started) pthread_join(stid, NULL);
  if (waiter_started) pthread_join(wtid, NULL);
  int calls = atomic_load(&failset_getter_calls);
  clru_destroy(cache);

  REQUIRE_EQ(seed_r, ccol_success);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(coalesced);
  REQUIRE_EQ(evict_r, ccol_success);
  REQUIRE_EQ(sarg.retval, ccol_unexpected_failure);
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 100);
  REQUIRE_EQ(calls, 1);
}

/* A get that coalesced onto a FETCH that failed keeps the answer of that
 * fetch. The remote getter already answered for the key, so the waiter must
 * not call it a second time. */
static _Atomic bool failfetch_entered = false;
static _Atomic bool failfetch_release = false;
static _Atomic int failfetch_calls = 0;
static bool failfetch_remote_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  (void)val;
  atomic_fetch_add(&failfetch_calls, 1);
  atomic_store(&failfetch_entered, true);
  (void)clru_await_flag(&failfetch_release);
  return false;
}

TEST(failed_set_waiter, getter_on_a_failed_fetch_keeps_its_answer) {
  atomic_store(&failfetch_entered, false);
  atomic_store(&failfetch_release, false);
  atomic_store(&failfetch_calls, 0);
  clru_construct(cache, int, int, 16, failfetch_remote_getter, NULL, NULL);

  waiter_arg_t first = {cache, 3, 0, -1, ccol_invalid_args, false};
  waiter_arg_t second = {cache, 3, 0, -1, ccol_invalid_args, false};
  pthread_t t1, t2;
  bool first_started = pthread_create(&t1, NULL, waiter_thread, &first) == 0;
  bool entered = first_started && clru_await_flag(&failfetch_entered);
  bool second_started = false;
  if (entered)
    second_started = pthread_create(&t2, NULL, waiter_thread, &second) == 0;
  bool coalesced = second_started && failset_await_waiters(cache, 3, 2);

  atomic_store(&failfetch_release, true);
  if (first_started) pthread_join(t1, NULL);
  if (second_started) pthread_join(t2, NULL);
  int calls = atomic_load(&failfetch_calls);
  clru_destroy(cache);

  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(coalesced);
  REQUIRE_EQ(first.retval, ccol_key_not_found);
  REQUIRE_EQ(second.retval, ccol_key_not_found);
  REQUIRE_EQ(calls, 1);
}

/* ------------------------------------------------------------------------ */
/* A NULL character pointer through clru_get() and clru_set()                */
/* ------------------------------------------------------------------------ */

#define CLRU_NULL_TEST_UNSET_VAR "CCOL_CLRU_TEST_SURELY_UNSET_VARIABLE"

/* The probe runs in a forked child and reports through a pipe, because a
 * child that dereferences NULL dies with SIGSEGV and would otherwise take the
 * whole test binary with it, and valgrind replaces the exit status of a
 * child that exits with memory still reachable. */
static int clru_null_run_probe(int (*probe)(void)) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    unsigned char byte = (unsigned char)probe();
    ssize_t w = write(fds[1], &byte, 1);
    (void)w;
    _exit(0);
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

static int clru_null_probe_key(void) {
  unsetenv(CLRU_NULL_TEST_UNSET_VAR);
  clru_construct(cache, char *, int, 8, NULL, NULL, NULL);
  ccol_retval_t rs = clru_set(cache, "HOME", 1);
  int v = 0;
  ccol_retval_t rg = clru_get(cache, getenv(CLRU_NULL_TEST_UNSET_VAR), &v);
  ccol_retval_t rs2 = clru_set(cache, getenv(CLRU_NULL_TEST_UNSET_VAR), 2);
  size_t size = clrucache_size(cache);
  clru_destroy(cache);
  return rs == ccol_success && rg == ccol_invalid_args && v == 0 &&
         rs2 == ccol_invalid_args && size == 1;
}

static int clru_null_probe_value(void) {
  unsetenv(CLRU_NULL_TEST_UNSET_VAR);
  clru_construct(cache, int, char *, 8, NULL, NULL, NULL);
  ccol_retval_t rs = clru_set(cache, 1, getenv(CLRU_NULL_TEST_UNSET_VAR));
  size_t size = clrucache_size(cache);
  clru_destroy(cache);
  return rs == ccol_invalid_args && size == 0;
}

/* A NULL character pointer is not a string. The pair that the macros build
 * for it has no bytes, and the raw layer refuses it with ccol_invalid_args,
 * which both macros return. This test is non-vacuous: without the NULL test
 * in _populate_cmap_pair(), both probes die with SIGSEGV inside strlen(). */
TEST(clru_null_char_ptr, a_null_key_is_invalid_args) {
  REQUIRE_EQ(clru_null_run_probe(clru_null_probe_key), 1);
}

TEST(clru_null_char_ptr, a_null_value_is_invalid_args) {
  REQUIRE_EQ(clru_null_run_probe(clru_null_probe_value), 1);
}

/* clru_destroy evaluates its argument once, so a walk backwards over an
 * array with `clru_destroy(a[--k])` destroys and clears every cache. This
 * test is non-vacuous: a macro that evaluates its argument twice destroys
 * only every second cache and clears the others without destroying them. */
TEST(clru_macros, destroy_evaluates_its_argument_once) {
  clru_cache caches[2] = {clrucache_create_full(8, ccol_int, ccol_int, NULL,
                                                NULL, NULL, NULL, NULL),
                          clrucache_create_full(8, ccol_int, ccol_int, NULL,
                                                NULL, NULL, NULL, NULL)};
  bool created =
      caches[0] != CLRU_CACHE_INVALID && caches[1] != CLRU_CACHE_INVALID;
  int k = 2;
  while (k > 0) clru_destroy(caches[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ(caches[0], CLRU_CACHE_INVALID);
  REQUIRE_EQ(caches[1], CLRU_CACHE_INVALID);
}

/* ------------------------------------------------------------------ */
/* The allocator procs struct of the caller                           */
/* ------------------------------------------------------------------ */

static atomic_long _lru_pl_live;
static atomic_long _lru_pl_calls;
/* Written over the struct of the caller after the cache exists. It forwards
 * to the C library, so a call through it corrupts nothing; any count above
 * zero means that the cache read the struct of the caller after
 * clrucache_create_full returned. */
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

/* The cache keeps its own copy of the procs struct. The caller overwrites
 * its struct right after the create, and every later allocation and free
 * of the cache, through every segment, still reaches the allocator that the
 * struct named at create time. This test is non-vacuous: a cache that keeps
 * the pointer of the caller sends the sets, the evictions and the destroy
 * through the poison allocator. */
TEST(custom_alloc, cache_does_not_read_the_procs_struct_of_the_caller_later) {
  const ccol_memmgmt_procs_t honest = {_lru_pl_malloc, _lru_pl_free,
                                       _lru_pl_calloc, _lru_pl_realloc};
  const ccol_memmgmt_procs_t poison = {
      _lru_pl_poison_malloc, _lru_pl_poison_free, _lru_pl_poison_calloc,
      _lru_pl_poison_realloc};
  ccol_memmgmt_procs_t *mp = malloc(sizeof(*mp));
  REQUIRE_NOT_NULL(mp);
  *mp = honest;
  _lru_pl_live = 0;
  _lru_pl_calls = 0;
  _lru_pl_poison_calls = 0;

  /* A capacity of 256 gives the cache more than one segment. */
  clru_cache cache = clrucache_create_full(256, ccol_long_long, ccol_long_long,
                                           NULL, NULL, NULL, mp, NULL);
  *mp = poison;
  long calls_at_overwrite = _lru_pl_calls;

  bool created = cache != CLRU_CACHE_INVALID;
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
      if (__clrucache_get_into(cache, &kp, &v, sizeof(v)) == ccol_success &&
          v == k * 3)
        found++;
    }
    __clrucache_destroy(cache);
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
