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
 * Three access patterns, because an LRU cache behaves very differently under
 * each. A pure hit stream touches the recency list on every access and does no
 * eviction; a working set larger than the capacity misses most of the time and
 * fills what it missed, so it evicts on almost every access, which is the
 * expensive path; and a set stream measures insertion with eviction. No remote
 * getter or setter is configured, so what is measured is the cache's own
 * bookkeeping rather than a fetch this repository does not control. That is
 * also why the working-set case fills on a miss itself: with no getter a miss
 * returns immediately and stores nothing, so a get-only stream over a working
 * set this size would never evict once and would report the cost of a failed
 * lookup under the name of the eviction path.
 */

#include <clrucache.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#define BENCH_LRU_CAPACITY 4096
#define BENCH_LRU_N 200000

/* Fewer operations per repetition in the contended variants. The cases are a
 * plain per-operation loop with no fixed cost to amortize, so a shorter
 * repetition measures the same thing; what it buys is many repetitions inside
 * the sampler's budget instead of a handful of very long ones. */
#define BENCH_LRU_MT_N 20000

typedef struct {
  clru_cache cache;
  int *keys;
  /* The total lru_get_run reaches when every lookup hits, computed once in
     setup so the timed loop needs no counter of its own. */
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

/* @param key_span  How many distinct keys the access stream draws from. At or
 *                  below the capacity every access hits; above it, the cache
 *                  evicts continuously. */
static lru_state_t *lru_make(size_t n, size_t key_span, bool prefill) {
  lru_state_t *st = calloc(1, sizeof *st);
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

/* Half the capacity, not all of it, so that this case really is all hits.
   Filling a sharded cache to exactly its capacity does not leave every key
   resident: keys are spread across independently bounded segments, so a
   segment that draws more than its share evicts while the cache as a whole is
   still under capacity. Measured at a span equal to the capacity, 4009 of 4096
   keys survive and 2.16 percent of lookups miss, which is neither the hit path
   this case is named for nor a stable mix. At half the capacity every key
   stays, and lru_get_run's own check holds the case to it. */
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
  /* Every key here is drawn from below the capacity and every one of them was
     put in during setup, so a miss is impossible and this case genuinely
     measures hits. Checked rather than assumed, because a miss is much cheaper
     than a hit: a defect that stopped keys being found would report as a large
     improvement under a name promising the opposite, which is the shape of
     result nobody questions. */
  /* Checked from the sum the loop already accumulates, so the timed body
     carries no counter of its own: every value equals its key, so a complete
     run reaches exactly the total computed in setup, and any miss falls
     short. */
  if (acc != st->expected_acc)
    bench_die("clrucache get_all_hits: a lookup missed");
  bench_sink(&acc);
}

/* Get, and on a miss put the key in, which is how a cache with no remote getter
 * is actually driven. The working set is eight times the capacity, so most
 * iterations miss, insert and evict. */
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
  bench_sink(&acc);
}

static void lru_set_run(void *state, size_t n) {
  lru_state_t *st = state;
  clru_cache c = st->cache;
  clru_redeclare(c, int, int);
  for (size_t i = 0; i < n; i++) clru_set(c, st->keys[i], (int)i);
  bench_sink(&c);
}

/* clrucache is internally locked, so its threads share one cache: the figure
 * these cases report is what that lock costs under real contention. The key
 * stream is read-only once built, so sharing it adds nothing the cache itself
 * does not already pay for. */
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
