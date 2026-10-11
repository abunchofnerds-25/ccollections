/*
 * MIT License
 *
 * Copyright (c) 2026 - A bunch of nerds
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file bench_cache.c
 * @brief Benchmarks for clrucache.
 *
 * There are three access patterns, because an LRU cache behaves very
 * differently under each one. A stream of hits alone touches the recency list
 * on every access and evicts nothing. A working set larger than the capacity
 * misses most of the time and then fills what it missed, so it evicts on
 * almost every access, which is the expensive path. A stream of set calls
 * measures an insert together with an eviction.
 *
 * These cases configure no remote getter and no remote setter, so they
 * measure the bookkeeping of the cache itself instead of a fetch that this
 * repository does not control. This is also why the working-set case fills the
 * cache itself on a miss: with no getter, a miss returns at once and stores
 * nothing, so a stream of get calls alone over a working set of this size would
 * never evict, and it would report the cost of a failed lookup under the name
 * of the eviction path.
 */

#include <clrucache.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#define BENCH_LRU_CAPACITY 4096
#define BENCH_LRU_N 200000

/* The forms with contention do fewer operations in each repetition. These
 * cases are a plain loop over the operations, with no fixed cost to spread
 * out, so a shorter repetition measures the same thing; what it gives you is
 * many repetitions inside the budget of the sampler instead of a few very long
 * ones. */
#define BENCH_LRU_MT_N 20000

typedef struct {
  clru_cache cache;
  int *keys;
  /* This is the total that lru_get_run reaches when every lookup hits. The
     setup computes it once, so the timed loop needs no counter of its own. */
  long long expected_acc;
} lru_state_t;

static void lru_teardown(void *state) {
  lru_state_t *st = state;
  if (st->cache != CLRU_CACHE_INVALID) {
    clru_cache c = st->cache;
    clru_destroy(c);
  }
  free(st->keys);
  free(st);
}

/* @param key_span  The number of distinct keys that the access stream draws
 *                  from. At or below the capacity, every access hits; above
 *                  it, the cache evicts all the time. */
static lru_state_t *lru_make(size_t n, size_t key_span, bool prefill) {
  lru_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->keys = malloc(sizeof(int) * n);
  if (!st->keys) {
    free(st);
    return NULL;
  }
  uint64_t seed = 0x2545f4914f6cdd1dULL;
  for (size_t i = 0; i < n; i++) {
    st->keys[i] = (int)(bench_rand(&seed) % key_span);
    st->expected_acc += st->keys[i];
  }

  clru_construct(c, int, int, BENCH_LRU_CAPACITY, NULL, NULL, NULL);
  if (c == CLRU_CACHE_INVALID) {
    lru_teardown(st);
    return NULL;
  }
  st->cache = c;
  if (prefill) {
    for (size_t k = 0; k < key_span && k < BENCH_LRU_CAPACITY; k++)
      clru_set(c, (int)k, (int)k);
  }
  return st;
}

/* This uses half the capacity instead of all of it, so that this case really
   does hit every time. A cache with segments does not keep every key when you
   fill it to exactly its capacity: the keys spread across segments, each
   segment has its own bound, and a segment that draws more than its share
   evicts while the whole cache is still below its capacity. Measured with a
   span equal to the capacity, 4009 of 4096 keys stay and 2.16 percent of the
   lookups miss, which is neither the hit path that this case is named for nor
   a steady mixture. At half the capacity every key stays, and the check inside
   lru_get_run holds the case to that. */
static void *lru_hit_setup(size_t n) {
  return lru_make(n, BENCH_LRU_CAPACITY / 2, true);
}

static void *lru_thrash_setup(size_t n) {
  return lru_make(n, BENCH_LRU_CAPACITY * 8, true);
}

static void *lru_set_setup(size_t n) {
  return lru_make(n, BENCH_LRU_CAPACITY * 8, false);
}

static void lru_get_run(void *state, size_t n) {
  lru_state_t *st = state;
  clru_cache c = st->cache;
  clru_redeclare(c, int, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    int out = 0;
    if (clru_get(c, st->keys[i], &out) == ccol_success) acc += out;
  }
  /* Every key here comes from below the capacity, and the setup put every one
     of them into the cache, so a miss is impossible and this case really does
     measure hits. The code checks this instead of assuming it, because a miss
     costs much less than a hit: a defect that stopped the cache from finding a
     key would report a large improvement under a name that promises the
     opposite, and nobody questions a result of that shape. */
  /* The check reads the sum that the loop already accumulates, so the timed
     body carries no counter of its own. Every value equals its key, so a
     complete run reaches exactly the total computed in setup, and any miss
     falls short. */
  if (acc != st->expected_acc)
    bench_die("clrucache get_all_hits: a lookup missed");
  bench_sink_value((long long)acc);
}

/* This gets a key and, on a miss, puts that key in, which is how a caller
 * drives a cache that has no remote getter. The working set is eight times the
 * capacity, so most passes miss, insert and evict. */
static void lru_get_or_fill_run(void *state, size_t n) {
  lru_state_t *st = state;
  clru_cache c = st->cache;
  clru_redeclare(c, int, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) {
    int out = 0;
    if (clru_get(c, st->keys[i], &out) == ccol_success) {
      acc += out;
    } else {
      clru_set(c, st->keys[i], st->keys[i]);
    }
  }
  bench_sink_value((long long)acc);
}

static void lru_set_run(void *state, size_t n) {
  lru_state_t *st = state;
  clru_cache c = st->cache;
  clru_redeclare(c, int, int);
  for (size_t i = 0; i < n; i++) clru_set(c, st->keys[i], (int)i);
  bench_sink(&c);
}

/* clrucache holds its own lock, so the threads here share one cache, and the
 * figure that these cases report is what that lock costs under real
 * contention. Nothing writes to the key stream after the setup builds it, so
 * the threads can share it without adding a cost that the cache does not
 * already pay. */
BENCH_MT_SETUP(lru_hit_setup)
BENCH_MT_SETUP(lru_set_setup)

void bench_register_cache(void) {
  bench_add(&(bench_case_t){.group = "clrucache",
                            .name = "get_all_hits",
                            .setup = lru_hit_setup,
                            .run = lru_get_run,
                            .teardown = lru_teardown,
                            .n = BENCH_LRU_N});
  bench_add_mt(&(bench_case_t){.group = "clrucache",
                               .name = "get_all_hits",
                               .setup_mt = lru_hit_setup_mt,
                               .run = lru_get_run,
                               .teardown = lru_teardown,
                               .n = BENCH_LRU_MT_N,
                               .shared_fixture = true});
  bench_add(&(bench_case_t){.group = "clrucache",
                            .name = "get_or_fill_working_set_8x_capacity",
                            .setup = lru_thrash_setup,
                            .run = lru_get_or_fill_run,
                            .teardown = lru_teardown,
                            .n = BENCH_LRU_N});
  bench_add(&(bench_case_t){.group = "clrucache",
                            .name = "set_with_eviction",
                            .setup = lru_set_setup,
                            .run = lru_set_run,
                            .teardown = lru_teardown,
                            .n = BENCH_LRU_N});
  bench_add_mt(&(bench_case_t){.group = "clrucache",
                               .name = "set_with_eviction",
                               .setup_mt = lru_set_setup_mt,
                               .run = lru_set_run,
                               .teardown = lru_teardown,
                               .n = BENCH_LRU_MT_N,
                               .shared_fixture = true});
}
