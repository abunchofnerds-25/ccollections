#include <clrucache.h>
#include <fcntl.h>
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
 * Verify that when a getter-populated entry is evicted the eviction callback
 * receives the exact value the getter produced, not the original key or
 * garbage. This exercises the path where entry->value was heap-allocated by the
 * remote getter (not via clru_set) and the LRU list then selects that entry as
 * victim.
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

  /* Fetch keys 1, 2, 3 via getter: values 2, 4, 6; cache full */
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
  /* Poison out with a recognizable, non-zero pattern first: if the
   * undersized value were ever silently partially copied instead of
   * rejected, this sentinel would survive in the untouched high bytes and
   * the test would still pass by coincidence on some platforms/values, so
   * assert the whole int stays exactly the sentinel, byte for byte. */
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
 * char* key path exercises a different branch of _populate_cmap_pair and a
 * different chmap key-type (ccol_string) for the internal map.  Verify that
 * the remote getter is called exactly once per unique string key, the returned
 * value is cached and served on subsequent accesses without another fetch, and
 * distinct string keys each trigger their own independent fetch.
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
 * A successful remote setter for a char* key must call through to the remote,
 * receive the correct key string and value, update the cache, and make the
 * entry retrievable via clru_get.
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
 * When the remote setter fails for a brand-new char* key the placeholder must
 * be cleaned up entirely: the cache stays empty and a subsequent get returns
 * ccol_key_not_found.
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

typedef struct {
  clru_cache cache;
  int key;
  int result;
  ccol_retval_t retval;
  /* Waited on, if non-NULL, immediately before calling clru_get: see this
   * field's own use at its multi-thread-racing-the-same-key call sites
   * below for why a fixed remote-getter sleep alone is not a reliable way
   * to get every thread actually contending for the cache's own mutex
   * before the first one's fetch completes. NULL (the default for any
   * aggregate-initialized instance with fewer initializers than members)
   * for every single-thread or distinct-key use, where no such race exists. */
  pthread_barrier_t *start_barrier;
} getter_arg_t;

static _Atomic int coalesce_getter_calls = 0;

static bool slow_remote_getter(const cmap_pair *key, cmap_pair *val) {
  /* atomic_fetch_add (the C11 <stdatomic.h> API), not the GCC/Clang
   * __atomic_fetch_add builtin: the latter requires a pointer to a plain,
   * non-_Atomic-qualified object under Clang (a stricter requirement than
   * GCC enforces for the same builtin), while coalesce_getter_calls is
   * declared _Atomic int, matching every other access to it below (all of
   * which already go through atomic_load()). */
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
  if (ga->start_barrier) pthread_barrier_wait(ga->start_barrier);
  ga->retval = clru_get(cache, ga->key, &ga->result);
  return NULL;
}

TEST(concurrency, multiple_getters_coalesce_to_single_remote_fetch) {
  coalesce_getter_calls = 0;
  clru_construct(cache, int, int, 16, slow_remote_getter, NULL, NULL);

#define N_THREADS 8
  pthread_barrier_t barrier;
  REQUIRE_EQ(pthread_barrier_init(&barrier, NULL, N_THREADS), 0);
  getter_arg_t args[N_THREADS];
  pthread_t tids[N_THREADS];
  int created = 0;
  for (int i = 0; i < N_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 42;
    args[i].result = 0;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_barrier = &barrier;
    /* A partial failure here must not leave the join loop below joining an
     * uninitialized tids[i] slot (undefined behavior, possibly hanging on
     * garbage pthread_t data), so only the threads actually created are
     * joined. */
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  REQUIRE_EQ(created, (int)N_THREADS);

  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  pthread_barrier_destroy(&barrier);

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

static _Atomic bool setter_started = false;
static _Atomic bool setter_may_finish = false;
static volatile bool setter_should_fail = false;
static volatile int setter_key_seen = -1;
static volatile int setter_val_seen = -1;

static bool gating_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  setter_key_seen = *(const int *)key->ptr;
  setter_val_seen = *(const int *)val->ptr;
  atomic_store(&setter_started, true);
  /* Spin until the test lets us finish */
  while (!atomic_load(&setter_may_finish)) {
    usleep(1000);
  }
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
  while (!atomic_load(&setter_started)) usleep(1000);

  /* Start a getter that should block until setter finishes */
  waiter_arg_t warg = {cache, 10, 99, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  REQUIRE_EQ(pthread_create(&wtid, NULL, waiter_thread, &warg), 0);

  /* Give getter time to start waiting */
  usleep(50000);
  REQUIRE_EQ((int)atomic_load(&warg.done), 0);

  /* Let the setter finish */
  atomic_store(&setter_may_finish, true);
  pthread_join(stid, NULL);
  pthread_join(wtid, NULL);

  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 99);

  clru_destroy(cache);
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
  while (!atomic_load(&setter_started)) usleep(1000);
  atomic_store(&setter_may_finish, true);
  pthread_join(stid1, NULL);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Phase 2: overwrite key 55 = 999 while blocking a concurrent getter */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);

  sync_setter_arg_t sarg2 = {cache, 55, 999};
  pthread_t stid2;
  REQUIRE_EQ(pthread_create(&stid2, NULL, sync_setter_thread, &sarg2), 0);
  while (!atomic_load(&setter_started)) usleep(1000);

  /* Getter arrives while the setter holds the set_in_progress flag */
  waiter_arg_t warg = {cache, 55, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  REQUIRE_EQ(pthread_create(&wtid, NULL, waiter_thread, &warg), 0);

  /* Confirm the getter is blocked */
  usleep(50000);
  REQUIRE_EQ((int)atomic_load(&warg.done), 0);

  /* Release the setter */
  atomic_store(&setter_may_finish, true);
  pthread_join(stid2, NULL);
  pthread_join(wtid, NULL);

  /* Getter must have received the new value, not the old one */
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 999);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  clru_destroy(cache);
}

/*
 * A getter that was blocked while a setter ran for a BRAND-NEW key should
 * receive ccol_key_not_found when the setter fails (no old value to fall back
 * on, and the placeholder is cleaned up).
 */
TEST(concurrency, getter_sees_key_not_found_when_new_key_setter_fails) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  clru_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg = {cache, 77, 777};
  pthread_t stid;
  REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);

  while (!atomic_load(&setter_started)) usleep(1000);

  /* Getter arrives while the setter placeholder is live */
  waiter_arg_t warg = {cache, 77, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  REQUIRE_EQ(pthread_create(&wtid, NULL, waiter_thread, &warg), 0);

  /* Give the getter time to enter the wait loop */
  usleep(50000);
  REQUIRE_EQ((int)atomic_load(&warg.done), 0);

  atomic_store(&setter_may_finish, true);
  pthread_join(stid, NULL);
  pthread_join(wtid, NULL);

  /* No old value exists: getter must return key_not_found */
  REQUIRE_EQ(warg.retval, ccol_key_not_found);
  /* Placeholder was cleaned up: cache must be empty */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  clru_destroy(cache);
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
  while (!atomic_load(&setter_started)) usleep(1000);
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
  while (!atomic_load(&setter_started)) usleep(1000);

  /* Getter arrives while the failing setter is in progress */
  waiter_arg_t warg = {cache, 33, 330, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  REQUIRE_EQ(pthread_create(&wtid, NULL, waiter_thread, &warg), 0);

  usleep(50000);
  REQUIRE_EQ((int)atomic_load(&warg.done), 0);

  atomic_store(&setter_may_finish, true);
  pthread_join(stid2, NULL);
  pthread_join(wtid, NULL);

  /* Old value must survive the failed overwrite */
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 330);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  clru_destroy(cache);
}

/*
 * Multiple getters blocked while a setter for an existing key is in progress
 * should ALL receive the old value when the setter fails, not just the first
 * one to wake up.
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
  while (!atomic_load(&setter_started)) usleep(1000);
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
  while (!atomic_load(&setter_started)) usleep(1000);

#define N_OLD_VAL_WAITERS 4
  waiter_arg_t wargs[N_OLD_VAL_WAITERS];
  pthread_t wtids[N_OLD_VAL_WAITERS];
  int wtids_created = 0;
  for (int i = 0; i < N_OLD_VAL_WAITERS; i++) {
    wargs[i] = (waiter_arg_t){cache, 42, 0, 0, ccol_unexpected_failure, false};
    /* A partial failure here must not leave the join loop below joining an
     * uninitialized wtids[i] slot. */
    if (pthread_create(&wtids[i], NULL, waiter_thread, &wargs[i]) != 0) break;
    wtids_created++;
  }
  REQUIRE_EQ(wtids_created, (int)N_OLD_VAL_WAITERS);

  usleep(50000); /* give all getters time to enter the wait loop */
  for (int i = 0; i < N_OLD_VAL_WAITERS; i++) {
    REQUIRE_EQ((int)atomic_load(&wargs[i].done), 0);
  }

  atomic_store(&setter_may_finish, true);
  pthread_join(stid2, NULL);
  for (int i = 0; i < wtids_created; i++) {
    pthread_join(wtids[i], NULL);
  }

  /* All getters must have received the old value */
  for (int i = 0; i < N_OLD_VAL_WAITERS; i++) {
    REQUIRE_EQ(wargs[i].retval, ccol_success);
    REQUIRE_EQ(wargs[i].result, 420);
  }
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
#undef N_OLD_VAL_WAITERS

  clru_destroy(cache);
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
    /* A partial failure here must not leave the join loop below joining an
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
    args[i].start_barrier = NULL; /* no shared-key race here to remove */
    /* A partial failure here must not leave the join loop below joining an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  REQUIRE_EQ(created, (int)N_UNIQUE_KEYS);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }

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
/*           IN-PROGRESS SETTER NOT EVICTABLE (regression for the bug        */
/*           where a live entry with set_in_progress could be evicted)        */
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
    while (!atomic_load(&val_gate_open)) usleep(1000);
  }
  return true;
}

/*
 * With capacity=1 and key 1 already live, a gated setter for key 1 (value=999)
 * removes key 1 from the LRU before releasing the mutex.  A concurrent set for
 * key 2 therefore sees size==0, inserts without triggering eviction, and key 2
 * becomes live.  When the setter for key 1 completes, entry_store_value evicts
 * key 2 to make room and key 1 becomes live with value 999.  The getter that
 * was blocked on the setter must see ccol_success with value 999, not
 * ccol_key_not_found (which was the pre-fix behaviour).
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
  while (!atomic_load(&val_gate_started)) usleep(1000);

  /* Thread B: getter for key 1 (must block until setter finishes) */
  waiter_arg_t warg = {cache, 1, 0, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  REQUIRE_EQ(pthread_create(&wtid, NULL, waiter_thread, &warg), 0);
  usleep(50000); /* give Thread B time to enter the wait loop */
  REQUIRE_EQ((int)atomic_load(&warg.done), 0);

  /* Main thread acts as Thread C: insert key 2 while Thread A is gated.
   * Before the fix: key 1 was still in LRU (size=1 >= capacity=1) so this
   * evicted key 1, causing Thread B to see ccol_key_not_found.
   * After the fix: key 1 was removed from LRU (size=0 < capacity=1) so
   * key 2 is inserted without triggering eviction. */
  clru_set(cache, 2, 200);

  /* Release Thread A */
  atomic_store(&val_gate_open, true);
  pthread_join(stid, NULL);
  pthread_join(wtid, NULL);

  /* Thread B must have received key 1's new value */
  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 999);

  clru_destroy(cache);
}

/* ========================================================================== */
/*        COALESCED WAITERS SURVIVE A RACING EVICTION OF THE ENTRY          */
/* ========================================================================== */

/*
 * Regression tests for a real bug: a getter coalesced onto an in-flight
 * fetch or set for the same key used to receive ccol_key_not_found if the
 * just-published entry was evicted by an unrelated, concurrent cache
 * operation before the waiter woke up and observed the result; even
 * though the fetch/set it coalesced onto had genuinely succeeded and the
 * entry's value was still sitting right there, kept alive by the waiter's
 * own reference. clru_test_set_post_publish_delay_us() widens the window
 * between an entry being published and the publishing thread
 * broadcasting/unlocking, so a concurrent evictor reliably queues up on
 * the mutex ahead of the woken waiter instead of depending on rare
 * scheduling luck; see that hook's own doc comment in clrucache.c.
 */

static _Atomic bool fetch_gate_started = false;
static _Atomic bool fetch_gate_open = false;

static bool gating_remote_getter(const cmap_pair *key, cmap_pair *val) {
  atomic_store(&fetch_gate_started, true);
  while (!atomic_load(&fetch_gate_open)) usleep(1000);
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
 * Winning the mutex race against a woken condition-variable waiter is not
 * something POSIX guarantees outright (see
 * clru_test_set_post_publish_delay_us's own doc comment in clrucache.c); it is
 * merely made overwhelmingly likely by widening the window. Repeating the race
 * several times, with a fresh cache each time, and requiring correctness on
 * every single attempt turns that "overwhelmingly likely per attempt" into an
 * effectively certain failure against a regression, without the test needing a
 * hard scheduling guarantee it cannot actually have.
 */
#define COALESCE_EVICTION_RACE_ITERATIONS 15

TEST(concurrency,
     coalesced_fetch_waiter_receives_value_despite_racing_eviction) {
  for (int iter = 0; iter < COALESCE_EVICTION_RACE_ITERATIONS; iter++) {
    atomic_store(&fetch_gate_started, false);
    atomic_store(&fetch_gate_open, false);
    clru_test_set_post_publish_delay_us(100000); /* 100ms */

    clru_construct(cache, int, int, 1, gating_remote_getter, NULL, NULL);

    /* Thread A: the fetcher for key 1 (via clru_get -> __clrucache_get_into);
     * gates inside the remote getter, well outside the cache mutex. */
    getter_arg_t farg = {cache, 1, 0, ccol_unexpected_failure, NULL};
    pthread_t ftid;
    REQUIRE_EQ(pthread_create(&ftid, NULL, getter_thread, &farg), 0);
    while (!atomic_load(&fetch_gate_started)) usleep(1000);

    /* Thread B: a getter coalesced onto Thread A's in-flight fetch, this
     * time via the clrucache_get_full() full-API entry point directly; must
     * block on Thread A's placeholder. */
    full_api_getter_arg_t warg = {cache, 1, 0, ccol_unexpected_failure};
    pthread_t wtid;
    REQUIRE_EQ(pthread_create(&wtid, NULL, full_api_getter_thread, &warg), 0);
    usleep(50000); /* give Thread B time to enter the wait loop */

    /* Release Thread A: it re-locks the mutex, publishes key 1 as LIVE
     * (capacity=1, so it is simultaneously the LRU tail), then spins inside
     * clru_test_set_post_publish_delay_us() for 100ms while STILL HOLDING
     * the mutex; well before it ever broadcasts to wake Thread B. */
    atomic_store(&fetch_gate_open, true);
    /* Wait until Thread A has actually re-locked the mutex and entered
     * its post-publish delay (still holding the mutex), rather than
     * guessing at a fixed sleep: polling this flag needs no lock of its
     * own, so it cannot itself be blocked behind Thread A's hold. */
    while (!clru_test_post_publish_delay_entered()) usleep(200);

    /* Thread C (this thread): insert an unrelated key while Thread A holds
     * the mutex mid-delay. This blocks on the mutex and queues up long
     * before Thread A ever broadcasts, so it is very likely to get the
     * mutex ahead of Thread B once Thread A finally releases it,
     * evicting key 1 (the only, and therefore LRU tail, entry) before
     * Thread B ever gets a chance to observe it. */
    REQUIRE_EQ(clru_set(cache, 2, 200), ccol_success);

    pthread_join(ftid, NULL);
    pthread_join(wtid, NULL);
    clru_test_set_post_publish_delay_us(0);

    /* Thread A (the fetcher) always gets its own result regardless of the
     * race, since it captures its own copy before ever releasing the
     * mutex. */
    REQUIRE_EQ(farg.retval, ccol_success);
    REQUIRE_EQ(farg.result, 1001);

    /* Thread B (the coalesced waiter) must receive the SAME result the
     * fetcher did (not ccol_key_not_found) even though key 1 may have
     * been evicted by Thread C before Thread B woke up. */
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

    /* Thread A: sets a brand-new key 1 = 100; gates inside the remote
     * setter, well outside the cache mutex. */
    sync_setter_arg_t sarg = {cache, 1, 100};
    pthread_t stid;
    REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);
    while (!atomic_load(&setter_started)) usleep(1000);

    /* Thread B: a getter coalesced onto Thread A's in-flight set (via
     * clru_get -> __clrucache_get_into); must block on Thread A's
     * placeholder. */
    waiter_arg_t warg = {cache, 1, 100, 0, ccol_unexpected_failure, false};
    pthread_t wtid;
    REQUIRE_EQ(pthread_create(&wtid, NULL, waiter_thread, &warg), 0);
    usleep(50000); /* give Thread B time to enter the wait loop */

    /* Release Thread A: it re-locks the mutex, stores key 1 as LIVE
     * (capacity=1, so it is simultaneously the LRU tail), then spins inside
     * clru_test_set_post_publish_delay_us() for 100ms while STILL HOLDING
     * the mutex; well before it ever broadcasts to wake Thread B. */
    atomic_store(&setter_may_finish, true);
    /* Wait until Thread A has actually re-locked the mutex and entered
     * its post-publish delay (still holding the mutex), rather than
     * guessing at a fixed sleep: polling this flag needs no lock of its
     * own, so it cannot itself be blocked behind Thread A's hold. */
    while (!clru_test_post_publish_delay_entered()) usleep(200);

    /* Thread C (this thread): insert an unrelated key while Thread A holds
     * the mutex mid-delay; very likely evicts key 1 before Thread B ever
     * gets a chance to observe it, for the same reason as the fetch test
     * above. */
    REQUIRE_EQ(clru_set(cache, 2, 200), ccol_success);

    pthread_join(stid, NULL);
    pthread_join(wtid, NULL);
    clru_test_set_post_publish_delay_us(0);

    /* Thread B (the coalesced getter) must receive the value Thread A's
     * set stored (not ccol_key_not_found) even though key 1 may have
     * been evicted by Thread C before Thread B woke up. */
    REQUIRE_EQ(warg.retval, ccol_success);
    REQUIRE_EQ(warg.result, 100);

    clru_destroy(cache);
  }
}
#undef COALESCE_EVICTION_RACE_ITERATIONS

/*
 * While a remote setter is executing for an existing key, that entry is removed
 * from the LRU list; clrucache_size() returns 0 during that window.  Once the
 * setter completes, the entry is re-inserted and size returns to 1.
 */
TEST(concurrency, size_zero_while_set_in_progress_for_existing_key) {
  atomic_store(&val_gate_started, false);
  atomic_store(&val_gate_open, false);

  clru_construct(cache, int, int, 10, NULL, value_gating_setter, NULL);

  clru_set(cache, 1, 100);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  sync_setter_arg_t sarg = {cache, 1, 999};
  pthread_t stid;
  REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);
  while (!atomic_load(&val_gate_started)) usleep(1000);

  /* Entry is removed from LRU while setter is in progress */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  atomic_store(&val_gate_open, true);
  pthread_join(stid, NULL);

  /* Entry re-inserted after setter completes */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 999);

  clru_destroy(cache);
}

/*
 * If the remote setter fails for an existing key while that entry was removed
 * from the LRU, the old value must be restored to the LRU so the entry
 * remains accessible with its previous value.
 */
TEST(concurrency, failed_setter_restores_existing_entry_to_lru) {
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;

  clru_construct(cache, int, int, 4, NULL, gating_remote_setter, NULL);

  /* Phase 1: seed key 5 = 50 synchronously */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = false;
  sync_setter_arg_t sarg1 = {cache, 5, 50};
  pthread_t stid1;
  REQUIRE_EQ(pthread_create(&stid1, NULL, sync_setter_thread, &sarg1), 0);
  while (!atomic_load(&setter_started)) usleep(1000);
  atomic_store(&setter_may_finish, true);
  pthread_join(stid1, NULL);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Phase 2: failing overwrite of key 5 */
  atomic_store(&setter_started, false);
  atomic_store(&setter_may_finish, false);
  setter_should_fail = true;
  sync_setter_arg_t sarg2 = {cache, 5, 999};
  pthread_t stid2;
  REQUIRE_EQ(pthread_create(&stid2, NULL, sync_setter_thread, &sarg2), 0);
  while (!atomic_load(&setter_started)) usleep(1000);

  /* Entry is being set (removed from LRU); size is 0 during the call */
  REQUIRE_EQ(clrucache_size(cache), (size_t)0);

  atomic_store(&setter_may_finish, true);
  pthread_join(stid2, NULL);

  /* After the failed setter, the old value must still be accessible */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 5, &out), ccol_success);
  REQUIRE_EQ(out, 50);

  clru_destroy(cache);
}

/*
 * When a setter fails for an existing key and other threads have filled the
 * cache while the mutex was released, restoring the old value must not push
 * size above capacity.  The restoration path must call make_room() before
 * re-inserting the entry into the LRU list.
 */
static _Atomic bool cap_overflow_gate_started = false;
static _Atomic bool cap_overflow_gate_open = false;

static bool cap_overflow_setter(const cmap_pair *key, const cmap_pair *val) {
  (void)key;
  if (*(const int *)val->ptr == 999) {
    atomic_store(&cap_overflow_gate_started, true);
    while (!atomic_load(&cap_overflow_gate_open)) usleep(1000);
    return false; /* fail this specific value */
  }
  return true; /* all other values succeed immediately */
}

TEST(concurrency, failed_setter_restores_without_exceeding_capacity) {
  atomic_store(&cap_overflow_gate_started, false);
  atomic_store(&cap_overflow_gate_open, false);

  /* capacity=1 so that one concurrent insert fills the cache while the
   * setter is gated and the original entry is out of the LRU. */
  clru_construct(cache, int, int, 1, NULL, cap_overflow_setter, NULL);

  clru_set(cache, 1, 100);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Thread A: attempt to overwrite key 1 = 999; setter gates then fails */
  sync_setter_arg_t sarg = {cache, 1, 999};
  pthread_t stid;
  REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);
  while (!atomic_load(&cap_overflow_gate_started)) usleep(1000);

  /* Thread A removed key 1 from the LRU; size is now 0.
   * Insert key 2 (setter returns true immediately); fills the cache. */
  clru_set(cache, 2, 200);
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* Release Thread A: setter fails, restoration must call make_room() */
  atomic_store(&cap_overflow_gate_open, true);
  pthread_join(stid, NULL);

  /* Size must not exceed capacity after the failed overwrite */
  REQUIRE_EQ(clrucache_size(cache), (size_t)1);

  /* The old value for key 1 must still be accessible */
  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 100);

  clru_destroy(cache);
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
 * Multiple concurrent getters for the same key should coalesce onto a single
 * remote call even when that call FAILS: only one fetch executes; all waiters
 * receive ccol_key_not_found; and the cache is left empty afterwards.
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
  pthread_barrier_t barrier;
  REQUIRE_EQ(pthread_barrier_init(&barrier, NULL, N_FAIL_THREADS), 0);
  getter_arg_t args[N_FAIL_THREADS];
  pthread_t tids[N_FAIL_THREADS];
  int created = 0;
  for (int i = 0; i < N_FAIL_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 11;
    args[i].result = 0;
    args[i].retval = ccol_success; /* sentinel; must be overwritten */
    args[i].start_barrier = &barrier;
    /* A partial failure here must not leave the join loop below joining an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, getter_thread, &args[i]) != 0) break;
    created++;
  }
  REQUIRE_EQ(created, (int)N_FAIL_THREADS);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  pthread_barrier_destroy(&barrier);

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
 * Regression test for a real bug: __clrucache_get_into() (the non-char*
 * clru_get() path) rejects a remote getter's fetched value when its size
 * does not match the caller's own fixed-size buffer, reporting
 * ccol_unexpected_failure; but only to the thread that actually ran the
 * getter. A concurrent caller coalesced onto that same in-flight fetch
 * (here, via clrucache_get_full()'s raw API, which has no fixed-size
 * destination of its own) used to see only ccol_key_not_found instead,
 * violating this module's own documented "all others ... receive the same
 * result" coalescing contract (clrucache.h's file-level doc comment) for
 * this one specific failure reason. Every coalesced caller must now learn
 * the fetch failed for the same reason the fetching thread did.
 */
static _Atomic bool mismatch_gate_started = false;
static _Atomic bool mismatch_gate_open = false;

static bool gating_oversized_getter(const cmap_pair *key, cmap_pair *val) {
  (void)key;
  atomic_store(&mismatch_gate_started, true);
  while (!atomic_load(&mismatch_gate_open)) usleep(1000);
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

  /* Thread A: clru_get -> __clrucache_get_into for an int-valued cache;
   * gates inside the remote getter, well outside the cache mutex. */
  getter_arg_t farg = {cache, 1, 0, ccol_success, NULL};
  pthread_t ftid;
  REQUIRE_EQ(pthread_create(&ftid, NULL, getter_thread, &farg), 0);
  while (!atomic_load(&mismatch_gate_started)) usleep(1000);

  /* Thread B: coalesces onto Thread A's in-flight fetch via
   * clrucache_get_full() directly, which has no buf_size of its own to
   * compare against. */
  full_api_getter_arg_t warg = {cache, 1, 0, ccol_success};
  pthread_t wtid;
  REQUIRE_EQ(pthread_create(&wtid, NULL, full_api_getter_thread, &warg), 0);
  usleep(50000); /* give Thread B time to enter the wait loop */

  atomic_store(&mismatch_gate_open, true);
  pthread_join(ftid, NULL);
  pthread_join(wtid, NULL);

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
 * A setter that arrives while a remote getter (fetch_in_progress) is executing
 * for the same key must block until the fetch completes, then overwrite the
 * newly cached value.  This exercises the fetch_in_progress wait path inside
 * clrucache_set_full.
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
  REQUIRE_EQ(pthread_create(&gtid, NULL, getter_thread, &garg), 0);

  /* Wait until the fetch has actually started (placeholder created, mutex
   * released, slow getter running). */
  while (atomic_load(&coalesce_getter_calls) == 0) usleep(1000);

  /* Thread B: set key=7; must find fetch_in_progress=true and block */
  sync_setter_arg_t sarg = {cache, 7, 999};
  pthread_t stid;
  REQUIRE_EQ(pthread_create(&stid, NULL, sync_setter_thread, &sarg), 0);

  pthread_join(gtid, NULL);
  pthread_join(stid, NULL);

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
  /* Waited on, if non-NULL, immediately before calling clru_get: see this
   * field's own use at the two call sites below for why a fixed remote-
   * getter sleep alone is not a reliable way to get every thread racing the
   * same key actually contending for the cache's own mutex before the
   * first one's fetch completes. */
  pthread_barrier_t *start_barrier;
} str_getter_arg_t;

static void *str_getter_thread(void *arg) {
  str_getter_arg_t *ga = (str_getter_arg_t *)arg;
  clru_cache cache = ga->cache;
  clru_redeclare(cache, int, char *);
  if (ga->start_barrier) pthread_barrier_wait(ga->start_barrier);
  ga->retval = clru_get(cache, ga->key, &ga->result);
  return NULL;
}

TEST(concurrency, multiple_char_ptr_getters_coalesce) {
  char_ptr_coalesce_calls = 0;
  clru_construct(cache, int, char *, 16, slow_char_ptr_getter, NULL, NULL);

#define N_STR_THREADS 6
  pthread_barrier_t barrier;
  REQUIRE_EQ(pthread_barrier_init(&barrier, NULL, N_STR_THREADS), 0);
  str_getter_arg_t args[N_STR_THREADS];
  pthread_t tids[N_STR_THREADS];
  int created = 0;
  for (int i = 0; i < N_STR_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 77;
    args[i].result = NULL;
    args[i].retval = ccol_unexpected_failure;
    args[i].start_barrier = &barrier;
    /* A partial failure here must not leave the join loop below joining an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, str_getter_thread, &args[i]) != 0) break;
    created++;
  }
  REQUIRE_EQ(created, (int)N_STR_THREADS);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  pthread_barrier_destroy(&barrier);

  /* Every cleanup (cache + any allocated result string) runs unconditionally
   * before the assertions below: REQUIRE_* returns from this function
   * immediately on the first failure, which would otherwise leak the cache
   * and every args[i].result the loop had not yet freed. */
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
  pthread_barrier_t barrier;
  REQUIRE_EQ(pthread_barrier_init(&barrier, NULL, N_FAIL_STR_THREADS), 0);
  str_getter_arg_t args[N_FAIL_STR_THREADS];
  pthread_t tids[N_FAIL_STR_THREADS];
  int created = 0;
  for (int i = 0; i < N_FAIL_STR_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 22;
    args[i].result = NULL;
    args[i].retval = ccol_success; /* sentinel; must be overwritten */
    args[i].start_barrier = &barrier;
    /* A partial failure here must not leave the join loop below joining an
     * uninitialized tids[i] slot. */
    if (pthread_create(&tids[i], NULL, str_getter_thread, &args[i]) != 0) break;
    created++;
  }
  REQUIRE_EQ(created, (int)N_FAIL_STR_THREADS);
  for (int i = 0; i < created; i++) {
    pthread_join(tids[i], NULL);
  }
  pthread_barrier_destroy(&barrier);

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
 * set_in_progress wait loop before the first call completes.
 * The 30 ms sleep in call 1 gives thread 3 time to re-check the map (with
 * the fix) or race to create a duplicate placeholder (without the fix) while
 * thread 2's remote call is still in progress.
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
 * Three threads all try to set the same brand-new key.
 *
 * Thread 1 (first to acquire the set slot) calls the remote setter; it takes
 * 30 ms and then FAILS.  Threads 2 and 3 block on set_in_progress during
 * that window.
 *
 * Without the fix: after Thread 1 fails, Threads 2 and 3 both wake up.
 * Thread 2 creates a new placeholder and releases the mutex for its 30 ms
 * remote call.  Thread 3 then calls create_and_insert_placeholder for the
 * same key, which hits chmap_insert_elem with an already-existing key and
 * gets ccol_key_already_present; so the function returns NULL and Thread 3
 * incorrectly returns ccol_not_enough_memory, silently dropping the set.
 * Result: only 2 remote setter calls are made instead of 3.
 *
 * With the fix: Thread 3 loops back to map_lookup, finds Thread 2's
 * placeholder (set_in_progress = true), and waits for it.  After Thread 2
 * succeeds, Thread 3 takes over the same entry and runs the remote setter
 * itself.  Result: all 3 remote setter calls are made, size == 1.
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
    /* A partial failure here must not leave the join loop below joining an
     * uninitialized stids[i] slot. */
    if (pthread_create(&stids[i], NULL, simple_setter_thread, &sargs[i]) != 0)
      break;
    created++;
    usleep(1000); /* stagger so thread 1 acquires the set slot first */
  }
  REQUIRE_EQ(created, (int)N_RACING_SETTERS);
  for (int i = 0; i < created; i++) {
    pthread_join(stids[i], NULL);
  }

  /* All 3 remote setter calls must have been made.  Without the fix,
   * thread 3 short-circuits with ccol_not_enough_memory and only 2 calls
   * are observed. */
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
  /* Both live entries must have been evicted via the destructor */
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

/* overwrite: subsequent get returns the new value */

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

/* char* value via clrucache_get_full: size == strlen + 1 */

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
 *     We reach the LIVE path by storing a value via clrucache_set_full
 *     directly (bypassing the type-safe macros, which always keep buf_size
 *     and the stored size in sync), then calling __clrucache_get_into with a
 *     differently-sized buffer. */

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

  /* Reading into an 8-byte buffer while the cached value is only 1 byte
   * must fail gracefully rather than silently leaving the remaining 7
   * bytes of the caller's buffer untouched while reporting success. */
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
 * Regression test: a failed __clrucache_get_into call (rejected because the
 * caller's buffer size does not match the cached value's size) must not
 * still promote the entry to the front of the LRU order. Before the fix,
 * lru_move_to_front() ran unconditionally before the size check, so an
 * entry that was never successfully read could outlive a genuinely
 * more-recently-set entry purely because someone had queried it with the
 * wrong buffer size.
 */
TEST(get_val_types, get_into_size_mismatch_failure_does_not_promote_lru) {
  clru_cache cache = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL,
                                           NULL, NULL, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  /* k1 (LRU/oldest) and k2 (MRU/newest) both hold 8-byte values via the raw
   * API, so LRU order after these two sets is: k1 (LRU) -> k2 (MRU). */
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

  /* Attempt to read k1 (the current LRU entry) into an undersized buffer:
   * this must fail without promoting k1's LRU position. */
  int wrong_size_out = 0;
  REQUIRE_EQ(__clrucache_get_into(cache, &kp1, &wrong_size_out,
                                  sizeof(wrong_size_out)),
             ccol_unexpected_failure);

  /* Insert a third key: capacity=2 forces exactly one eviction. k1 must
   * still be the LRU victim (its failed read must not have promoted it),
   * so k2 (never touched again after its own set) must survive. */
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
 * Regression tests: clru_set(name, key, val) must convert val (and key) to
 * the cache's own declared ValT/KeyT the same way a plain C assignment
 * would, not merely capture val's/key's own natural expression type and
 * store its raw bytes. A same-size-but-differently-typed val (e.g. a float
 * stored into an int-valued cache) previously had its raw bit pattern
 * copied verbatim; __clrucache_get_into's size check cannot catch this,
 * since the stored size and the requested buffer size coincidentally match
 * even though the underlying types differ, so the wrong (reinterpreted)
 * value was silently returned as a "success". This mirrors the cvec_push /
 * cvec_push_rvalue bug this codebase has already found and fixed for
 * cvector's own type-safe push macros.
 */
TEST(type_conversion, set_float_into_int_cache_converts_not_reinterprets) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  float f = 7.0f;
  REQUIRE_EQ(clru_set(cache, 1, f), ccol_success);

  int out = 0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  /* A reinterpreted 7.0f's raw bits read back as int would be 1088421888,
   * not 7; a genuine conversion (as a plain `int x = 7.0f;` would perform)
   * produces 7. */
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

  /* Reading an int-valued cache into a double* out-param must convert the
   * stored int to a double (as `double x = some_int;` would), not reject it
   * (sizeof(double) != sizeof(int), so a size-mismatch rejection would also
   * have been "safe" but wrong: this must actually succeed and convert). */
  double out = 0.0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 9.0);

  clru_destroy(cache);
}

TEST(type_conversion,
     get_into_float_out_param_does_not_reinterpret_stored_int) {
  clru_construct(cache, int, int, 8, NULL, NULL, NULL);

  REQUIRE_EQ(clru_set(cache, 1, 12), ccol_success);

  /* float and int are the same size on every mainstream platform, so this
   * is exactly the "coincidentally same size" case __clrucache_get_into's
   * own size check cannot detect on its own; clru_get must still convert
   * (via the cache's own declared ValT, int) rather than reinterpret. */
  float out = 0.0f;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 12.0f);

  clru_destroy(cache);
}

TEST(type_conversion, set_string_key_from_local_char_array_variable) {
  /* A char[] variable (not a string literal) used as the key: the
   * type-tracking KeyT conversion must still decay it to char* correctly. */
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
 * A "budget" allocator: succeeds normally while budget < 0 (unlimited),
 * fails every call once budget reaches exactly 0, and otherwise decrements
 * budget once per successful allocation. Lets a test force a SPECIFIC,
 * later allocation (e.g. clrucache_get_full's own copy-for-the-caller
 * allocation on an already-cached hit, which is the very first allocation
 * that call makes) to fail deterministically, without needing to count
 * every allocation the setup phase itself performs.
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

  /* The copy returned by clrucache_get_full was allocated with the custom
   * allocator; free it via that same allocator */
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

  /* char* copy was allocated with the custom allocator; free via it */
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
 * Regression test: a clrucache_get_full() call on an already-cached LIVE
 * entry that fails only because the caller's own copy allocation hits OOM
 * must not still promote that entry to the front of the LRU order. Before
 * the fix, lru_move_to_front() ran unconditionally before the copy
 * allocation was attempted, so a hit that failed purely due to transient
 * memory pressure could keep an entry alive at the expense of a genuinely
 * more-recently-set one.
 */
TEST(custom_alloc, get_full_copy_oom_failure_does_not_promote_lru) {
  _lru_fault_alloc_budget = -1; /* unlimited while seeding the cache */
  ccol_memmgmt_procs_t mprocs = {_lru_fault_malloc, _lru_fault_free,
                                 _lru_fault_calloc, _lru_fault_realloc};

  clru_cache cache = clrucache_create_full(2, ccol_int, ccol_int, NULL, NULL,
                                           NULL, &mprocs, NULL);
  REQUIRE_NE(cache, CLRU_CACHE_INVALID);

  /* k1 (LRU/oldest) and k2 (MRU/newest). */
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

  /* Force the very next allocation to fail: a cache hit's only allocation
   * is the copy handed back to the caller, so this deterministically fails
   * just that one call without needing to count every setup allocation. */
  _lru_fault_alloc_budget = 0;
  cmap_pair val_out = {};
  REQUIRE_EQ(clrucache_get_full(cache, &kp1, &val_out), ccol_not_enough_memory);
  REQUIRE_NULL(val_out.ptr);

  /* Allocations succeed again for everything from here on. */
  _lru_fault_alloc_budget = -1;

  /* Insert a third key: capacity=2 forces exactly one eviction. k1 must
   * still be the LRU victim (its failed, OOM-only read must not have
   * promoted it), so k2 (never touched again after its own set) must
   * survive. */
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

/* ========================================================================== */
/*        CLRU_CACHE HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)          */
/* ========================================================================== */

/* Mirrors the already-implemented, already-verified chttpcli_handle_lifecycle
 * / event_loop_handle_lifecycle test groups, adapted for clru_cache's own
 * lock-protected pin mechanism (see src/clrucache.c's own struct clrucache.
 * pending_resolve_count / pin_cv field comments: unlike event_loop's fully
 * lock-free pin, clru_cache reuses the chttpcli/chttpsvr-style
 * lock-protected decrement+broadcast, since this module is already a
 * single-global-mutex design with no new-contention concern from adding one
 * more brief mutex-protected step). */

/* A fully completed destroy, followed later by a second destroy call on an
 * independently-held copy of the same original handle value, must be a
 * fatal error. Run in a forked child (mirroring tests/clogger/tests.c's own
 * fork-test precedent for process-terminating misuse) since fatal_err
 * aborts the whole process. */
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
    _exit(0); /* unreachable if fatal_err() aborted as expected */
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

/* Two threads calling destroy on two independently-held copies of the SAME,
 * still-valid handle at (as close to) the same moment as possible must also
 * be fatal; regression coverage for the same class of concurrent double-free
 * this whole redesign exists to close for chttpcli/chttpsvr/event_loop. */
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
    /* Inside a forked child: REQUIRE_* would be unsafe here (its early
     * return would skip this branch's own _exit() and fall back into the
     * harness's test loop a second time), so a create failure instead
     * falls through to a distinct, non-SIGABRT exit the parent's
     * WIFSIGNALED/SIGABRT check below already turns into a clean test
     * failure, rather than joining a garbage, never-created pthread_t. */
    if (pthread_create(&t1, NULL, clru_concurrent_destroy_thread, &a1) != 0)
      _exit(2);
    if (pthread_create(&t2, NULL, clru_concurrent_destroy_thread, &a2) != 0)
      _exit(2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* unreachable: whichever of the two destroy calls loses the
                  race must hit fatal_err() */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* Reuses the pre-existing slow_remote_getter (defined above, in the
 * COALESCING GETTERS section: sleeps 100ms before returning) to hold a
 * clrucache_get_full call's pin open for a long, directly-controlled
 * duration; unlike event_loop (which has no naturally-occurring slow public
 * entry point and needed a dedicated resolve_pin_and_sleep_for_tests test
 * hook), clru_cache's own remote-getter mechanism already gives every
 * caller a way to block for an arbitrary, application-controlled duration
 * while still holding a resolve's pin, so no new test-only accessor is
 * needed for this test specifically. */

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

/* The resolve-then-use race fix actually works: races a thread blocked
 * inside clrucache_get_full's remote-getter call (still holding its pin)
 * against a concurrent clru_destroy on the same handle. destroy must block
 * until the pin is released, not race ahead and free the cache out from
 * under the still-resolved pointer. */
TEST(clrucache_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  clru_construct(cache, int, int, 8, slow_remote_getter, NULL, NULL);

  clru_slow_get_arg_t get_arg = {.h = cache, .rv = ccol_success};
  pthread_t get_thread;
  REQUIRE_EQ(pthread_create(&get_thread, NULL, clru_slow_get_thread, &get_arg),
             0);

  /* Give the getter thread a brief head start so its resolve (and therefore
   * its pin) has definitely already happened before destroy fires. */
  struct timespec startup = {.tv_sec = 0, .tv_nsec = 10000000}; /* 10 ms */
  nanosleep(&startup, NULL);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  clru_destroy(cache); /* must block until the getter thread's 100ms
                            remote_getter call (still holding the pin) has
                            fully completed */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(get_thread, NULL);
  REQUIRE_EQ(get_arg.rv, ccol_success);
  /* The getter thread slept ~100ms while pinned; destroy returning in well
   * under that would mean it did NOT actually wait for the pin, i.e. the
   * resolve-then-use protection failed. */
  REQUIRE_GT(elapsed_ms, 50L);
}

/* A remote setter that simply sleeps before succeeding, giving a
 * clrucache_set_full call a long, directly-controlled window during which
 * it still holds its resolve's pin (mirroring slow_remote_getter's role in
 * resolve_then_use_race_destroy_waits above). The set path has a
 * meaningfully different control flow around the pin than the get path
 * (LRU removal/restore, its own waiters++/-- bracketing the remote call),
 * so it is exercised by its own dedicated test rather than assumed to be
 * covered by the get-side one. */
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

  /* Give the setter thread a brief head start so its resolve (and therefore
   * its pin) has definitely already happened before destroy fires. */
  struct timespec startup = {.tv_sec = 0, .tv_nsec = 10000000}; /* 10 ms */
  nanosleep(&startup, NULL);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  clru_destroy(cache); /* must block until the setter thread's 100ms
                            remote_setter call (still holding the pin) has
                            fully completed */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(set_thread, NULL);
  REQUIRE_EQ(set_arg.rv, ccol_success);
  /* The setter thread slept ~100ms while pinned; destroy returning in well
   * under that would mean it did NOT actually wait for the pin, i.e. the
   * resolve-then-use protection failed for the set path. */
  REQUIRE_GT(elapsed_ms, 50L);
}

typedef struct {
  clru_cache h;
} clru_capacity_arg_t;

static void *clru_capacity_thread(void *arg) {
  clru_capacity_arg_t *a = (clru_capacity_arg_t *)arg;
  /* Return value intentionally ignored: a legitimate race with a concurrent
   * destroy can make this resolve fail (returning 0) instead of succeeding;
   * both outcomes are correct. This thread exists purely to generate
   * resolve/pin/unpin traffic concurrent with the destroy thread below. */
  clrucache_capacity(a->h);
  return NULL;
}

/* Distinct from resolve_then_use_race_destroy_waits above, and not
 * redundant with it: that test's long, deliberately-held pin guarantees
 * pending_resolve_count > 0 for the whole race window; this test needs the
 * opposite shape: a fast, non-blocking entry point (clrucache_capacity:
 * resolve, one mutex-free field read, unpin, return) raced against a
 * concurrent destroy, repeated under stress, since the failure window for
 * a fast pin/unpin pair is only a handful of instructions wide and will not
 * reproduce reliably under a single unstressed run. A fresh cache is used
 * each iteration so every repetition gets its own independent race rather
 * than reusing one already-destroyed handle. */
TEST(clrucache_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    clru_cache cache = clrucache_create_full(8, ccol_int, ccol_int, NULL, NULL,
                                             NULL, NULL, NULL);
    REQUIRE_NE(cache, CLRU_CACHE_INVALID);

    clru_capacity_arg_t capacity_arg = {.h = cache};
    clru_concurrent_destroy_arg_t destroy_arg = {.h = cache};
    pthread_t capacity_tid, destroy_tid;
    REQUIRE_EQ(pthread_create(&capacity_tid, NULL, clru_capacity_thread,
                              &capacity_arg),
               0);
    REQUIRE_EQ(pthread_create(&destroy_tid, NULL,
                              clru_concurrent_destroy_thread, &destroy_arg),
               0);
    pthread_join(capacity_tid, NULL);
    pthread_join(destroy_tid, NULL);
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

/* The slot table is bounded, not ever-growing: a create/destroy churn loop
 * with only a single slot ever in flight at a time must reuse that one
 * freed slot on every iteration rather than growing the table further.
 * Captures capacity right after the first create/destroy pair (rather than
 * asserting a fixed absolute value like 1) since other tests earlier in
 * this same process may have already grown the table to some N > 1; what
 * this test actually needs to prove is that ITS OWN churn adds no further
 * growth, not what the table's absolute size happens to be when it runs. */
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
