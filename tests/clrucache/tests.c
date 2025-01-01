#include <clrucache.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

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

  REQUIRE_EQ(clrucache_get_full(NULL, &kp, &val_out), ccol_invalid_args);
  REQUIRE_EQ(clrucache_get_full(cache, NULL, &val_out), ccol_invalid_args);
  REQUIRE_EQ(clrucache_get_full(cache, &kp, NULL), ccol_invalid_args);

  clru_destroy(cache);
}

TEST(basic, invalid_args_set) {
  clru_construct(cache, int, int, 4, NULL, NULL, NULL);

  int k = 1, v = 2;
  cmap_pair kp = {}, vp = {};
  _populate_cmap_pair(&kp, k);
  _populate_cmap_pair(&vp, v);

  REQUIRE_EQ(clrucache_set_full(NULL, &kp, &vp), ccol_invalid_args);
  REQUIRE_EQ(clrucache_set_full(cache, NULL, &vp), ccol_invalid_args);
  REQUIRE_EQ(clrucache_set_full(cache, &kp, NULL), ccol_invalid_args);

  cmap_pair zero_vp = {.ptr = &v, .size = 0};
  REQUIRE_EQ(clrucache_set_full(cache, &kp, &zero_vp), ccol_invalid_args);

  clru_destroy(cache);
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

TEST(eviction, lru_order_respected) {
  eviction_key_captured = -1;
  eviction_val_captured = -1;
  eviction_count = 0;

  clru_construct(cache, int, int, 3, NULL, NULL, record_eviction);

  /* Insert keys 0,1,2 — fills the cache */
  for (int i = 0; i < 3; i++) {
    clru_set(cache, i, i * 10);
  }
  REQUIRE_EQ(clrucache_size(cache), (size_t)3);

  /* Access key 0 so it becomes MRU; key 1 is now LRU */
  int out = 0;
  clru_get(cache, 0, &out);

  /* Insert key 3 — should evict key 1 (LRU) */
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

TEST(remote_getter, re_fetches_after_eviction) {
  remote_get_call_count = 0;
  clru_construct(cache, int, int, 1, simple_remote_getter, NULL, NULL);

  int out = 0;
  clru_get(cache, 10, &out);
  REQUIRE_EQ(out, 20); /* 10 * 2 */
  REQUIRE_EQ(remote_get_call_count, 1);

  /* Fetch a different key — evicts key=10 (only slot available) */
  clru_get(cache, 20, &out);
  REQUIRE_EQ(out, 40); /* 20 * 2 */
  REQUIRE_EQ(remote_get_call_count, 2);

  /* Fetch key=10 again — must call the remote getter (was evicted) */
  clru_get(cache, 10, &out);
  REQUIRE_EQ(out, 20);
  REQUIRE_EQ(remote_get_call_count, 3);

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

/* ========================================================================== */
/*                         CONCURRENT GETTERS — KEY COALESCING                */
/* ========================================================================== */

typedef struct {
  clru_cache cache;
  int key;
  int result;
  ccol_retval_t retval;
} getter_arg_t;

static volatile int coalesce_getter_calls = 0;

static bool slow_remote_getter(const cmap_pair *key, cmap_pair *val) {
  __atomic_fetch_add(&coalesce_getter_calls, 1, __ATOMIC_SEQ_CST);

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
  ga->retval = clru_get(cache, ga->key, &ga->result);
  return NULL;
}

TEST(concurrency, multiple_getters_coalesce_to_single_remote_fetch) {
  coalesce_getter_calls = 0;
  clru_construct(cache, int, int, 16, slow_remote_getter, NULL, NULL);

#define N_THREADS 8
  getter_arg_t args[N_THREADS];
  pthread_t tids[N_THREADS];
  for (int i = 0; i < N_THREADS; i++) {
    args[i].cache = cache;
    args[i].key = 42;
    args[i].result = 0;
    args[i].retval = ccol_unexpected_failure;
    pthread_create(&tids[i], NULL, getter_thread, &args[i]);
  }

  for (int i = 0; i < N_THREADS; i++) {
    pthread_join(tids[i], NULL);
  }

  /* Only one remote call should have been made */
  REQUIRE_EQ(coalesce_getter_calls, 1);

  /* All threads should have received the correct value */
  for (int i = 0; i < N_THREADS; i++) {
    REQUIRE_EQ(args[i].retval, ccol_success);
    REQUIRE_EQ(args[i].result, 1042); /* key 42 + 1000 */
  }
#undef N_THREADS

  clru_destroy(cache);
}

/* ========================================================================== */
/*                  CONCURRENT GETTERS WAIT FOR ACTIVE SETTER                 */
/* ========================================================================== */

static volatile bool setter_started = false;
static volatile bool setter_may_finish = false;
static volatile int setter_key_seen = -1;
static volatile int setter_val_seen = -1;

static bool gating_remote_setter(const cmap_pair *key, const cmap_pair *val) {
  setter_key_seen = *(const int *)key->ptr;
  setter_val_seen = *(const int *)val->ptr;
  setter_started = true;
  /* Spin until the test lets us finish */
  while (!setter_may_finish) {
    usleep(1000);
  }
  return true;
}

typedef struct {
  clru_cache cache;
  int key;
  int expect_val;
  int result;
  ccol_retval_t retval;
  volatile bool done;
} waiter_arg_t;

static void *waiter_thread(void *arg) {
  waiter_arg_t *wa = (waiter_arg_t *)arg;
  clru_cache cache = wa->cache;
  clru_redeclare(cache, int, int);
  wa->retval = clru_get(cache, wa->key, &wa->result);
  wa->done = true;
  return NULL;
}

typedef struct {
  clru_cache cache;
  int key;
  int val;
} sync_setter_arg_t;

static void *sync_setter_thread(void *arg) {
  sync_setter_arg_t *sa = (sync_setter_arg_t *)arg;
  int k = sa->key, v = sa->val;
  clru_set(sa->cache, k, v);
  return NULL;
}

TEST(concurrency, getters_wait_for_sync_setter) {
  setter_started = false;
  setter_may_finish = false;

  clru_construct(cache, int, int, 16, NULL, gating_remote_setter, NULL);

  sync_setter_arg_t sarg = {cache, 10, 99};
  pthread_t stid;
  pthread_create(&stid, NULL, sync_setter_thread, &sarg);

  /* Wait until setter has started the remote call */
  while (!setter_started) usleep(1000);

  /* Start a getter that should block until setter finishes */
  waiter_arg_t warg = {cache, 10, 99, 0, ccol_unexpected_failure, false};
  pthread_t wtid;
  pthread_create(&wtid, NULL, waiter_thread, &warg);

  /* Give getter time to start waiting */
  usleep(50000);
  REQUIRE_EQ((int)warg.done, 0);

  /* Let the setter finish */
  setter_may_finish = true;
  pthread_join(stid, NULL);
  pthread_join(wtid, NULL);

  REQUIRE_EQ(warg.retval, ccol_success);
  REQUIRE_EQ(warg.result, 99);

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
  clru_set(sa->cache, sa->key, sa->val);
  return NULL;
}

TEST(concurrency, setters_for_same_key_are_serialized) {
  serial_setter_idx = 0;
  memset((void *)serial_setter_order, 0, sizeof(serial_setter_order));

  clru_construct(cache, int, int, 16, NULL, ordering_remote_setter, NULL);

#define N_SETTERS 4
  simple_setter_arg_t sargs[N_SETTERS];
  pthread_t stids[N_SETTERS];

  for (int i = 0; i < N_SETTERS; i++) {
    sargs[i].cache = cache;
    sargs[i].key = 7; /* same key for all */
    sargs[i].val = i + 1;
    pthread_create(&stids[i], NULL, simple_setter_thread, &sargs[i]);
    usleep(500); /* stagger slightly so order is deterministic */
  }
  for (int i = 0; i < N_SETTERS; i++) {
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
#undef N_SETTERS

  clru_destroy(cache);
}

TEST(concurrency, concurrent_getters_different_keys) {
  coalesce_getter_calls = 0;
  clru_construct(cache, int, int, 16, slow_remote_getter, NULL, NULL);

#define N_UNIQUE_KEYS 8
  getter_arg_t args[N_UNIQUE_KEYS];
  pthread_t tids[N_UNIQUE_KEYS];
  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    args[i].cache = cache;
    args[i].key = 100 + i; /* distinct keys — no coalescing should happen */
    args[i].result = 0;
    args[i].retval = ccol_unexpected_failure;
    pthread_create(&tids[i], NULL, getter_thread, &args[i]);
  }
  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    pthread_join(tids[i], NULL);
  }

  /* Each unique key must have triggered exactly one remote call */
  REQUIRE_EQ(coalesce_getter_calls, N_UNIQUE_KEYS);

  for (int i = 0; i < N_UNIQUE_KEYS; i++) {
    REQUIRE_EQ(args[i].retval, ccol_success);
    REQUIRE_EQ(args[i].result, 100 + i + 1000); /* key + 1000 */
  }
#undef N_UNIQUE_KEYS

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
/*            VALUE TYPE COVERAGE — clru_get & clrucache_get_full             */
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

/* --- non-char* scalar: double --------------------------------------------- */

TEST(get_val_types, double_value_via_macro) {
  clru_construct(cache, int, double, 8, NULL, NULL, NULL);

  clru_set(cache, 1, 2.5);

  double out = 0.0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2.5);

  clru_destroy(cache);
}

/* --- non-char* struct value ----------------------------------------------- */

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

/* --- clrucache_get_full: each call produces a fresh independent allocation - */

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

/* --- on a miss, val_out fields are not modified ---------------------------- */

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

/* --- overwrite: subsequent get returns the new value ----------------------- */

TEST(get_val_types, overwrite_reflected_in_subsequent_get) {
  clru_construct(cache, int, double, 8, NULL, NULL, NULL);

  clru_set(cache, 1, 1.0);
  clru_set(cache, 1, 2.5);

  double out = 0.0;
  REQUIRE_EQ(clru_get(cache, 1, &out), ccol_success);
  REQUIRE_EQ(out, 2.5);

  clru_destroy(cache);
}

/* --- char* value: macro transfers heap ownership to caller ----------------- */

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

/* --- char* value: two consecutive gets return distinct heap pointers ------- */

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

/* --- char* value via clrucache_get_full: size == strlen + 1 ---------------- */

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

/* --- both key and value are char* ----------------------------------------- */

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

/* --- char* value from remote getter: macro path delivers owned string ------ */

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

/* --- char* value from remote getter: clrucache_get_full direct path -------- */

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
