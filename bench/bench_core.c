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
 * @file bench_core.c
 * @brief Benchmarks for cvector, cstring, csort and cmempool.
 */

#include <cmempool.h>
#include <csort.h>
#include <cstring.h>
#include <cvector.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

/* ------------------------------------------------------------------------ */
/* cvector                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
  cvec v;
  size_t *idx;
} vec_state_t;

static void vec_teardown(void *state) {
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_destroy(v);
  free(st->idx);
  free(st);
}

static void *vec_empty_setup(size_t n) {
  (void)n;
  vec_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  cvec_construct(v, int);
  st->v = v;
  return st;
}

/* Growing from the minimum capacity is part of what this case measures. A
 * push that reallocates is the expensive case, and spreading that cost over
 * the whole run is the honest way to report what it costs to build a
 * vector. */
static void vec_push_run(void *state, size_t n) {
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_redeclare(v, int);
  for (size_t i = 0; i < n; i++) cvec_push(v, (int)i);
  bench_sink(v);
}

/* The same work with the final capacity reserved up front, so it separates
 * the per-element cost from the reallocation and copying the case above
 * includes. */
static void *vec_reserved_setup(size_t n) {
  vec_state_t *st = vec_empty_setup(n);
  if (!st) return NULL;
  cvec v = st->v;
  cvec_redeclare(v, int);
  cvec_reserve(v, n);
  return st;
}

static void *vec_filled_setup(size_t n) {
  vec_state_t *st = vec_empty_setup(n);
  if (!st) return NULL;
  cvec v = st->v;
  cvec_redeclare(v, int);
  cvec_reserve(v, n);
  uint64_t seed = 0x9e3779b97f4a7c15ULL;
  for (size_t i = 0; i < n; i++) cvec_push(v, (int)bench_rand(&seed));
  st->idx = malloc(sizeof(size_t) * n);
  if (!st->idx) {
    vec_teardown(st);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) st->idx[i] = (size_t)(bench_rand(&seed) % n);
  return st;
}

/* The indices are computed first and then shuffled, so this measures how the
 * container itself indexes and not an in-order sweep the prefetcher helps
 * with. */
static void vec_random_access_run(void *state, size_t n) {
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_redeclare(v, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) acc += cvec_at(v, st->idx[i]);
  bench_sink_value((long long)acc);
}

static void vec_sequential_access_run(void *state, size_t n) {
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_redeclare(v, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) acc += cvec_at(v, i);
  bench_sink_value((long long)acc);
}

/* Reported per element and not per sort, so the figure stays comparable when
 * the number of elements changes. */
static void vec_sort_run(void *state, size_t n) {
  (void)n;
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_redeclare(v, int);
  cvec_sort(v);
  bench_sink(v);
}

/* ------------------------------------------------------------------------ */
/* cstring                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
  cstr s;
} str_state_t;

static void str_teardown(void *state) {
  str_state_t *st = state;
  cstr s = st->s;
  cstr_destroy(s);
  free(st);
}

static void *str_empty_setup(size_t n) {
  (void)n;
  str_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  cstr_construct(s, "");
  st->s = s;
  return st;
}

static void str_append_run(void *state, size_t n) {
  str_state_t *st = state;
  cstr s = st->s;
  for (size_t i = 0; i < n; i++) cstr_append(s, "abcdefgh");
  bench_sink(s);
}

static void *str_haystack_setup(size_t n) {
  str_state_t *st = str_empty_setup(n);
  if (!st) return NULL;
  cstr s = st->s;
  /* This haystack has no match near its start, so find() scans all of it
   * every time instead of returning after the first few bytes. */
  for (size_t i = 0; i < 512; i++) cstr_append(s, "abcdefgh");
  cstr_append(s, "needle");
  return st;
}

static void str_find_run(void *state, size_t n) {
  str_state_t *st = state;
  cstr s = st->s;
  size_t acc = 0;
  for (size_t i = 0; i < n; i++) acc += (size_t)(cstr_find(s, "needle") + 1);
  bench_sink_value((long long)acc);
}

/* ------------------------------------------------------------------------ */
/* cmempool, against the system allocator                                    */
/* ------------------------------------------------------------------------ */

BENCH_MT_SETUP(vec_empty_setup)
BENCH_MT_SETUP(vec_filled_setup)
BENCH_MT_SETUP(str_empty_setup)

typedef struct {
  ccol_mempool *pool;
  void **slots;
  size_t count;
} pool_state_t;

#define BENCH_POOL_ELEM_SIZE 64
#define BENCH_POOL_LIVE 1024

/* A per-connection read buffer, a protocol frame and a database page are all
   about this size, which is the size people most often want a pool for. In
   this range the system allocator also stops serving from its per-thread
   cache and starts merging and splitting chunks on every release. */
#define BENCH_POOL_BUFFER_SIZE 4096

/* These objects have lifetimes that don't nest: a set of connections,
   sessions or graph nodes frees its objects in an order that has nothing to do
   with the allocation order. A rolling replacement never exercises that,
   because there the entry just released is always the next one the pool hands
   back. */
#define BENCH_POOL_SCATTERED_LIVE 4096

static uint64_t bench_scatter_state = 88172645463325252u;
static inline size_t bench_scatter_next(size_t modulus) {
  bench_scatter_state ^= bench_scatter_state << 13;
  bench_scatter_state ^= bench_scatter_state >> 7;
  bench_scatter_state ^= bench_scatter_state << 17;
  return (size_t)(bench_scatter_state % modulus);
}

static void pool_teardown(void *state) {
  pool_state_t *st = state;
  if (st->pool) {
    ccol_mempool *p = st->pool;
    ccol_mempool_destroy(p);
  }
  free(st->slots);
  free(st);
}

static void *pool_setup_generic(bool single_threaded) {
  pool_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->count = BENCH_POOL_LIVE;
  st->slots = calloc(st->count, sizeof(void *));
  if (!st->slots) {
    pool_teardown(st);
    return NULL;
  }
  /* This pool has no dynamic fallback and the working set fits inside it. A
   * fallback allocation would mean the benchmark had silently started to
   * measure malloc instead of the pool. */
  st->pool = ccol_mempool_create(st->count, BENCH_POOL_ELEM_SIZE,
                                 /*fallback_to_dynamic_memory=*/false,
                                 single_threaded, NULL, NULL);
  if (!st->pool) {
    pool_teardown(st);
    return NULL;
  }
  return st;
}

static void *pool_setup(size_t n) {
  (void)n;
  return pool_setup_generic(false);
}

/* The same pool with its lock taken off the path, which separates the
 * allocator's own cost from the synchronization around it. */
static void *pool_st_setup(size_t n) {
  (void)n;
  return pool_setup_generic(true);
}

/* A rolling working set instead of allocating everything and then freeing
 * everything: a slot is freed right before it's allocated again. That's the
 * shape people use a pool in, and the one that keeps its free list hot.
 *
 * The live set is the size of the pool's capacity and the pool has no
 * dynamic fallback, so a NULL return means the pool doesn't honour the
 * capacity you gave it. The abort on that return isn't a defensive habit. A
 * NULL return is cheaper than an allocation, so without this check a pool
 * that had started failing would report a *better* figure than one that
 * works, and the times would become meaningless in the flattering direction.
 *
 * This function does NOT free the entries that are out, on purpose.
 * pool_teardown destroys the pool, which releases the whole backing buffer in
 * one go, and the matching malloc case cleans up in its own teardown too. A
 * free here would put st->count more frees inside the timed region that the
 * malloc case doesn't pay for, which would bias the very comparison this case
 * is there to make, and would also make the reported ns/op depend on n. */
static void pool_alloc_free_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    size_t slot = i % st->count;
    if (st->slots[slot]) ccol_mempool_free_entry(st->pool, st->slots[slot]);
    st->slots[slot] = ccol_mempool_alloc_entry(st->pool);
    if (!st->slots[slot]) bench_die("cmempool returned NULL below capacity");
    bench_sink(st->slots[slot]);
  }
}

/* Allocates a whole batch and then frees the whole batch instead of rolling
 * one slot at a time, because the two shapes exercise different code. A
 * caller that alternates moves a thread cache by one entry, and after its
 * first refill it may never touch the shared free list again, so it reports
 * the fast path and little else. A batch much bigger than one magazine forces
 * the refill and flush paths on every burst, which is where a per-thread cache
 * either pays for itself or doesn't.
 *
 * The batch is the whole live set while the unit stays one allocation and
 * free pair, so the figure compares directly with the alternating case above
 * and the malloc case next to it. A last, partial batch is allocated and
 * freed in full, so no entry stays out between two batches. */
static void pool_burst_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t done = 0; done < n;) {
    size_t batch = n - done;
    if (batch > st->count) batch = st->count;
    for (size_t i = 0; i < batch; i++) {
      st->slots[i] = ccol_mempool_alloc_entry(st->pool);
      if (!st->slots[i]) bench_die("cmempool returned NULL below capacity");
      bench_sink(st->slots[i]);
    }
    for (size_t i = 0; i < batch; i++) {
      ccol_mempool_free_entry(st->pool, st->slots[i]);
      st->slots[i] = NULL;
    }
    done += batch;
  }
}

/* The 4 KiB forms, with the same rolling replacement as the 64-byte cases;
   only the element size differs. */
static void *pool_buffer_setup(size_t n) {
  (void)n;
  pool_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->count = BENCH_POOL_LIVE;
  st->slots = calloc(st->count, sizeof(void *));
  if (!st->slots) {
    pool_teardown(st);
    return NULL;
  }
  st->pool = ccol_mempool_create(st->count, BENCH_POOL_BUFFER_SIZE,
                                 /*fallback_to_dynamic_memory=*/false,
                                 /*single_threaded=*/false, NULL, NULL);
  if (!st->pool) {
    pool_teardown(st);
    return NULL;
  }
  return st;
}

static void malloc_buffer_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    size_t slot = i % st->count;
    free(st->slots[slot]);
    st->slots[slot] = malloc(BENCH_POOL_BUFFER_SIZE);
    if (!st->slots[slot]) bench_die("malloc returned NULL");
    bench_sink(st->slots[slot]);
  }
}

/* The scattered-lifetime variants: the slot replaced on each iteration is
   picked at random and not in order, so a released entry is rarely the next
   one handed back. Both arms draw from the same generator in the same
   sequence.

   These two write to the entry they get, which the other cases don't on
   purpose. What separates the two allocators here is how densely the live set
   is packed: pool entries are one stride apart with nothing in between,
   while every chunk from the system allocator carries its own header, so the
   same number of live objects spans more cache lines. You only see that
   difference from a caller that touches what it allocated, which any real one
   does. Both arms write the same eight bytes, so the write itself cancels out
   and only the locality it exposes is left. */
static void *pool_scattered_setup(size_t n) {
  (void)n;
  pool_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->count = BENCH_POOL_SCATTERED_LIVE;
  st->slots = calloc(st->count, sizeof(void *));
  if (!st->slots) {
    pool_teardown(st);
    return NULL;
  }
  st->pool = ccol_mempool_create(st->count, BENCH_POOL_ELEM_SIZE,
                                 /*fallback_to_dynamic_memory=*/false,
                                 /*single_threaded=*/false, NULL, NULL);
  if (!st->pool) {
    pool_teardown(st);
    return NULL;
  }
  bench_scatter_state = 88172645463325252u;
  return st;
}

static void pool_scattered_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    size_t slot = bench_scatter_next(st->count);
    if (st->slots[slot]) ccol_mempool_free_entry(st->pool, st->slots[slot]);
    st->slots[slot] = ccol_mempool_alloc_entry(st->pool);
    if (!st->slots[slot]) bench_die("cmempool returned NULL below capacity");
    *(volatile uint64_t *)st->slots[slot] = (uint64_t)i;
    bench_sink(st->slots[slot]);
  }
}

static void *malloc_scattered_setup(size_t n) {
  (void)n;
  pool_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->count = BENCH_POOL_SCATTERED_LIVE;
  st->slots = calloc(st->count, sizeof(void *));
  if (!st->slots) {
    free(st);
    return NULL;
  }
  bench_scatter_state = 88172645463325252u;
  return st;
}

static void malloc_scattered_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    size_t slot = bench_scatter_next(st->count);
    free(st->slots[slot]);
    st->slots[slot] = malloc(BENCH_POOL_ELEM_SIZE);
    if (!st->slots[slot]) bench_die("malloc returned NULL");
    *(volatile uint64_t *)st->slots[slot] = (uint64_t)i;
    bench_sink(st->slots[slot]);
  }
}

static void *malloc_setup(size_t n) {
  (void)n;
  pool_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->count = BENCH_POOL_LIVE;
  st->slots = calloc(st->count, sizeof(void *));
  if (!st->slots) {
    free(st);
    return NULL;
  }
  return st;
}

static void malloc_teardown(void *state) {
  pool_state_t *st = state;
  for (size_t i = 0; i < st->count; i++) free(st->slots[i]);
  free(st->slots);
  free(st);
}

static void malloc_alloc_free_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    size_t slot = i % st->count;
    free(st->slots[slot]);
    st->slots[slot] = malloc(BENCH_POOL_ELEM_SIZE);
    /* Checked for the same reason as in the pool arm: a failing allocator
     * costs much less than a working one, so an unchecked malloc arm would
     * keep measuring and win the comparison it belongs to. */
    if (!st->slots[slot]) bench_die("malloc returned NULL");
    bench_sink(st->slots[slot]);
  }
}

/* The malloc counterpart of pool_burst_run, with the same shape so the two
 * compare. */
static void malloc_burst_run(void *state, size_t n) {
  pool_state_t *st = state;
  for (size_t done = 0; done < n;) {
    size_t batch = n - done;
    if (batch > st->count) batch = st->count;
    for (size_t i = 0; i < batch; i++) {
      st->slots[i] = malloc(BENCH_POOL_ELEM_SIZE);
      /* See malloc_alloc_free_run. */
      if (!st->slots[i]) bench_die("malloc returned NULL");
      bench_sink(st->slots[i]);
    }
    for (size_t i = 0; i < batch; i++) {
      free(st->slots[i]);
      st->slots[i] = NULL;
    }
    done += batch;
  }
}

/* The same workload with several threads: every thread shares one pool and
 * one malloc heap, and each thread rolls its own part of the live set, which
 * doesn't overlap the others'. The threads have to share the subject under
 * test, since that's the point, but not the scratch array around it, so each
 * thread keeps its own slots and what's left is the contention inside the
 * allocator.
 *
 * The pool keeps the same total capacity it has on one thread, so what
 * changes between the single-threaded run and the multi-threaded one is the
 * contention and not the shape of the pool. */
typedef struct {
  ccol_mempool *pool;
  void **slots;
  size_t count;
  size_t per_thread;
} pool_mt_state_t;

static void pool_mt_teardown(void *state) {
  pool_mt_state_t *st = state;
  if (st->pool) {
    ccol_mempool *p = st->pool;
    ccol_mempool_destroy(p);
  }
  free(st->slots);
  free(st);
}

static void *pool_mt_setup_generic(unsigned threads, bool with_pool) {
  pool_mt_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  st->count = BENCH_POOL_LIVE;
  st->per_thread = st->count / threads;
  if (st->per_thread == 0) {
    free(st);
    return NULL;
  }
  st->slots = calloc(st->count, sizeof(void *));
  if (!st->slots) {
    pool_mt_teardown(st);
    return NULL;
  }
  if (with_pool) {
    st->pool = ccol_mempool_create(st->count, BENCH_POOL_ELEM_SIZE,
                                   /*fallback_to_dynamic_memory=*/false,
                                   /*single_threaded=*/false, NULL, NULL);
    if (!st->pool) {
      pool_mt_teardown(st);
      return NULL;
    }
  }
  return st;
}

static void *pool_mt_setup(size_t n, unsigned threads) {
  (void)n;
  return pool_mt_setup_generic(threads, true);
}

static void *malloc_mt_setup(size_t n, unsigned threads) {
  (void)n;
  return pool_mt_setup_generic(threads, false);
}

static void malloc_mt_teardown(void *state) {
  pool_mt_state_t *st = state;
  for (size_t i = 0; i < st->count; i++) free(st->slots[i]);
  free(st->slots);
  free(st);
}

static void pool_alloc_free_mt_run(void *state, size_t n) {
  pool_mt_state_t *st = state;
  void **mine = st->slots + (size_t)bench_thread_index() * st->per_thread;
  for (size_t i = 0; i < n; i++) {
    size_t slot = i % st->per_thread;
    if (mine[slot]) ccol_mempool_free_entry(st->pool, mine[slot]);
    mine[slot] = ccol_mempool_alloc_entry(st->pool);
    if (!mine[slot]) bench_die("cmempool returned NULL below capacity");
    bench_sink(mine[slot]);
  }
}

static void malloc_alloc_free_mt_run(void *state, size_t n) {
  pool_mt_state_t *st = state;
  void **mine = st->slots + (size_t)bench_thread_index() * st->per_thread;
  for (size_t i = 0; i < n; i++) {
    size_t slot = i % st->per_thread;
    free(mine[slot]);
    mine[slot] = malloc(BENCH_POOL_ELEM_SIZE);
    /* See malloc_alloc_free_run: an unchecked malloc arm keeps timing a
     * failing allocator and wins the comparison it's part of. */
    if (!mine[slot]) bench_die("malloc returned NULL");
    bench_sink(mine[slot]);
  }
}

/* ------------------------------------------------------------------------ */

void bench_register_core(void) {
  bench_add(&(bench_case_t){.group = "cvector",
                            .name = "push_int_growing",
                            .setup = vec_empty_setup,
                            .run = vec_push_run,
                            .teardown = vec_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cvector",
                            .name = "push_int_reserved",
                            .setup = vec_reserved_setup,
                            .run = vec_push_run,
                            .teardown = vec_teardown,
                            .n = BENCH_DEFAULT_N});
  /* Two million ints are 8 MB of payload plus a 16 MB index array, past a
   * usual L2 cache and at or past L3, so the random case really pays for its
   * cache misses. At BENCH_DEFAULT_N the whole working set fits in the cache,
   * so the two cases would only measure the loop and random access would
   * report the same speed as in-order access. */
  bench_add(&(bench_case_t){.group = "cvector",
                            .name = "access_sequential",
                            .setup = vec_filled_setup,
                            .run = vec_sequential_access_run,
                            .teardown = vec_teardown,
                            .n = 2000000});
  bench_add(&(bench_case_t){.group = "cvector",
                            .name = "access_random",
                            .setup = vec_filled_setup,
                            .run = vec_random_access_run,
                            .teardown = vec_teardown,
                            .n = 2000000});
  /* cvector, cstring and csort hold no locks by design, so a shared instance
   * would be a data race and not a benchmark. Each thread drives its own
   * instance instead, which is how they're meant to be used across threads,
   * and that measures whether the work scales in parallel. */
  bench_add_mt(&(bench_case_t){.group = "cvector",
                               .name = "push_int_growing",
                               .setup_mt = vec_empty_setup_mt,
                               .run = vec_push_run,
                               .teardown = vec_teardown,
                               .n = BENCH_DEFAULT_N});
  /* The same two million elements per thread as the single-threaded case. A
   * smaller vector would fit in cache and the case would stop measuring the
   * cache misses it's there to measure, which would make it look faster
   * rather than more parallel. */
  bench_add_mt(&(bench_case_t){.group = "cvector",
                               .name = "access_random",
                               .setup_mt = vec_filled_setup_mt,
                               .run = vec_random_access_run,
                               .teardown = vec_teardown,
                               .n = 2000000});
  bench_add(&(bench_case_t){.group = "csort",
                            .name = "mergesort_int_per_elem",
                            .setup = vec_filled_setup,
                            .run = vec_sort_run,
                            .teardown = vec_teardown,
                            .n = 100000});
  bench_add_mt(&(bench_case_t){.group = "csort",
                               .name = "mergesort_int_per_elem",
                               .setup_mt = vec_filled_setup_mt,
                               .run = vec_sort_run,
                               .teardown = vec_teardown,
                               .n = 100000});

  bench_add(&(bench_case_t){.group = "cstring",
                            .name = "append_8_bytes",
                            .setup = str_empty_setup,
                            .run = str_append_run,
                            .teardown = str_teardown,
                            .n = 100000});
  bench_add(&(bench_case_t){.group = "cstring",
                            .name = "find_4kb_haystack",
                            .setup = str_haystack_setup,
                            .run = str_find_run,
                            .teardown = str_teardown,
                            .n = 20000});
  bench_add_mt(&(bench_case_t){.group = "cstring",
                               .name = "append_8_bytes",
                               .setup_mt = str_empty_setup_mt,
                               .run = str_append_run,
                               .teardown = str_teardown,
                               .n = 100000});

  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "alloc_free_64b",
                            .setup = pool_setup,
                            .run = pool_alloc_free_run,
                            .teardown = pool_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "alloc_free_64b_single_threaded",
                            .setup = pool_st_setup,
                            .run = pool_alloc_free_run,
                            .teardown = pool_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "alloc_free_64b",
                            .vs = "malloc",
                            .setup = malloc_setup,
                            .run = malloc_alloc_free_run,
                            .teardown = malloc_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "alloc_free_4kb",
                            .setup = pool_buffer_setup,
                            .run = pool_alloc_free_run,
                            .teardown = pool_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "alloc_free_4kb",
                            .vs = "malloc",
                            .setup = malloc_setup,
                            .run = malloc_buffer_run,
                            .teardown = malloc_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "alloc_free_64b_scattered",
                            .setup = pool_scattered_setup,
                            .run = pool_scattered_run,
                            .teardown = pool_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "alloc_free_64b_scattered",
                            .vs = "malloc",
                            .setup = malloc_scattered_setup,
                            .run = malloc_scattered_run,
                            .teardown = malloc_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "burst_alloc_free_64b",
                            .setup = pool_setup,
                            .run = pool_burst_run,
                            .teardown = pool_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add(&(bench_case_t){.group = "cmempool",
                            .name = "burst_alloc_free_64b",
                            .vs = "malloc",
                            .setup = malloc_setup,
                            .run = malloc_burst_run,
                            .teardown = malloc_teardown,
                            .n = BENCH_DEFAULT_N});
  bench_add_mt(&(bench_case_t){.group = "cmempool",
                               .name = "alloc_free_64b",
                               .setup_mt = pool_mt_setup,
                               .run = pool_alloc_free_mt_run,
                               .teardown = pool_mt_teardown,
                               .n = BENCH_DEFAULT_N,
                               .shared_fixture = true});
  bench_add_mt(&(bench_case_t){.group = "cmempool",
                               .name = "alloc_free_64b",
                               .vs = "malloc",
                               .setup_mt = malloc_mt_setup,
                               .run = malloc_alloc_free_mt_run,
                               .teardown = malloc_mt_teardown,
                               .n = BENCH_DEFAULT_N,
                               .shared_fixture = true});
}
