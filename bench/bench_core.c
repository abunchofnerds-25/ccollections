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
  vec_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  cvec_construct(v, int);
  st->v = v;
  return st;
}

/* Growth from the minimum capacity is part of what this measures: a push that
 * triggers a reallocation is the expensive case, and amortizing it over the
 * run is the honest way to report the cost of building a vector. */
static void vec_push_run(void *state, size_t n) {
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_redeclare(v, int);
  for (size_t i = 0; i < n; i++) cvec_push_rvalue(v, (int)i);
  bench_sink(v);
}

/* The same work with the final capacity reserved up front, which isolates the
 * per-element cost from the reallocation and copying the previous case
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
  for (size_t i = 0; i < n; i++) cvec_push_rvalue(v, (int)bench_rand(&seed));
  st->idx = malloc(sizeof(size_t) * n);
  if (!st->idx) {
    vec_teardown(st);
    return NULL;
  }
  for (size_t i = 0; i < n; i++) st->idx[i] = (size_t)(bench_rand(&seed) % n);
  return st;
}

/* Indices are precomputed and shuffled, so this measures the container's own
 * indexing rather than a sequential prefetch-friendly sweep. */
static void vec_random_access_run(void *state, size_t n) {
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_redeclare(v, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) acc += cvec_at(v, st->idx[i]);
  bench_sink(&acc);
}

static void vec_sequential_access_run(void *state, size_t n) {
  vec_state_t *st = state;
  cvec v = st->v;
  cvec_redeclare(v, int);
  long long acc = 0;
  for (size_t i = 0; i < n; i++) acc += cvec_at(v, i);
  bench_sink(&acc);
}

/* Reported per element rather than per sort, so the figure stays comparable
 * across a change in the element count. */
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
  str_state_t *st = calloc(1, sizeof *st);
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
  /* A haystack with no early match, so find() does the full scan every time
   * rather than returning from the first few bytes. */
  for (size_t i = 0; i < 512; i++) cstr_append(s, "abcdefgh");
  cstr_append(s, "needle");
  return st;
}

static void str_find_run(void *state, size_t n) {
  str_state_t *st = state;
  cstr s = st->s;
  size_t acc = 0;
  for (size_t i = 0; i < n; i++) acc += (size_t)(cstr_find(s, "needle") + 1);
  bench_sink(&acc);
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

/* A per-connection read buffer, a protocol frame, a database page: the sizes a
   pool is most often reached for, and the range where the system allocator
   stops serving from its per-thread cache and starts merging and splitting
   chunks on every release. */
#define BENCH_POOL_BUFFER_SIZE 4096

/* Objects whose lifetimes do not nest. Freeing in an order unrelated to
   allocation is what a set of connections, sessions or graph nodes actually
   does, and it is the case a rolling replacement never exercises: there, the
   entry just released is always the next one handed back. */
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
  pool_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->count = BENCH_POOL_LIVE;
  st->slots = calloc(st->count, sizeof(void *));
  if (!st->slots) {
    pool_teardown(st);
    return NULL;
  }
  /* No dynamic fallback: the working set is sized to fit the pool, so a
   * fallback allocation would mean the benchmark had silently started
   * measuring malloc instead of the pool. */
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

/* The same pool with its locking compiled out of the path, which is what
 * separates the allocator's own cost from the synchronization around it. */
static void *pool_st_setup(size_t n) {
  (void)n;
  return pool_setup_generic(true);
}

/* A rolling working set rather than allocate-all-then-free-all: the slot being
 * reused is freed immediately before it is allocated again, which is the shape
 * a pool is actually used in and the one where its free list stays hot.
 *
 * The live set is sized to the pool's own capacity and there is no dynamic
 * fallback, so a NULL return means the pool stopped honouring the capacity it
 * was created with. Aborting on it is not defensiveness: returning NULL is
 * cheaper than allocating, so without this check a pool that had started
 * failing would report a *better* figure than one that works, and the timing
 * would silently become meaningless in the flattering direction.
 *
 * Outstanding entries are deliberately NOT freed here. pool_teardown destroys
 * the pool, which releases the whole backing buffer at once, and the matching
 * malloc case likewise cleans up in its teardown. Freeing them in this function
 * would put st->count extra frees inside the timed region that the malloc case
 * does not pay, biasing the very comparison this case exists to make, and would
 * make the reported ns/op depend on n. */
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

/* Allocate a whole batch, then free the whole batch, rather than rolling one
 * slot at a time. The two shapes exercise different code: an alternating
 * caller moves a thread cache by one entry and, after its first refill, may
 * never reach the shared free list again, so it reports the fast path and
 * almost nothing else. A batch far larger than one magazine forces the refill
 * and flush paths on every burst, which is where a per-thread cache either
 * earns its keep or does not.
 *
 * The batch is the whole live set, and the unit stays one alloc-and-free pair
 * so the figure is directly comparable with the alternating case above and
 * with the malloc case measured beside it. A trailing partial batch is
 * allocated and freed in full, so no entry is left outstanding between
 * batches. */
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

/* The 4 KiB variants. Same rolling replacement as the 64-byte cases, so the
   only thing that differs between them is the element size. */
static void *pool_buffer_setup(size_t n) {
  (void)n;
  pool_state_t *st = calloc(1, sizeof *st);
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

/* The scattered-lifetime variants: the slot replaced each iteration is chosen
   at random rather than in order, so a released entry is rarely the next one
   handed back. Both arms draw from the same generator in the same sequence.

   These two write to the entry they obtain, which the other cases deliberately
   do not. What separates the two allocators here is how densely the live set is
   packed: pool entries are one stride apart with nothing between them, while
   each chunk from the system allocator carries its own header, so the same
   number of live objects spans more cache lines. That difference is only
   visible to a caller that touches what it allocated, which is what any real
   one does. Both arms write the same eight bytes, so the write itself cancels
   and only the locality it exposes remains. */
static void *pool_scattered_setup(size_t n) {
  (void)n;
  pool_state_t *st = calloc(1, sizeof *st);
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
  pool_state_t *st = calloc(1, sizeof *st);
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
  pool_state_t *st = calloc(1, sizeof *st);
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
    /* Checked for the same reason the pool arm checks its own return: a
     * failing allocator is far cheaper than a working one, so an unchecked
     * malloc arm keeps timing and wins the comparison it is part of. */
    if (!st->slots[slot]) bench_die("malloc returned NULL");
    bench_sink(st->slots[slot]);
  }
}

/* The malloc counterpart of pool_burst_run, in the same shape so the two are
 * comparable. */
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

/* The multi-threaded shape of the same workload: one pool, one malloc heap,
 * shared by every thread, with each thread rolling its own disjoint slice of
 * the live set. Sharing the subject under test is the point; sharing the
 * scratch array around it is not, so the slice keeps each thread's slots to
 * itself and what is left is the allocator's own contention.
 *
 * The pool keeps the same total capacity it has on one thread, so the figure
 * that changes between the single- and multi-threaded runs is contention, not
 * geometry. */
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
  pool_mt_state_t *st = calloc(1, sizeof *st);
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
     * failing allocator and wins the comparison it is part of. */
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
  /* Two million ints is 8 MB of payload plus a 16 MB index array, chosen to
   * sit past a typical L2 and around or past L3, so that the random case
   * actually pays for cache misses. At BENCH_DEFAULT_N the whole working set
   * fits in cache and the two cases measure loop overhead alone, which makes
   * random access report as fast as sequential. */
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
   * would be a data race rather than a benchmark. Each thread drives its own
   * instance instead, which is how they are meant to be used across threads and
   * measures whether the work scales in parallel. */
  bench_add_mt(&(bench_case_t){.group = "cvector",
                               .name = "push_int_growing",
                               .setup_mt = vec_empty_setup_mt,
                               .run = vec_push_run,
                               .teardown = vec_teardown,
                               .n = BENCH_DEFAULT_N});
  /* The same two million elements the single-threaded case uses, per thread.
   * A smaller vector would fit in cache and the case would stop measuring the
   * cache misses it exists to measure, which is what makes it look faster
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
