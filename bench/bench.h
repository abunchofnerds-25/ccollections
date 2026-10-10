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
 * @file bench.h
 * @brief Measurement harness shared by every benchmark group.
 *
 * A benchmark is a bench_case_t. It has a setup, which builds the state that
 * the measured work needs. It has a run, which does exactly n operations. It
 * also has a teardown. The harness times the run only. It repeats the setup and
 * the teardown for every repetition. Some cases change their own state, for
 * example one that fills a vector or one that inserts into a map. Such a case
 * therefore measures the same work every time. It does not measure a container
 * that grows.
 *
 * The harness reports a result in nanoseconds for each operation. It takes the
 * median across the repetitions, and not the mean, because the distribution has
 * one long side. The scheduler can stop a repetition, or move it to another
 * CPU, for any length of time. Nothing can make a repetition faster than the
 * work it does. One outlier therefore moves a mean and leaves a median alone.
 *
 * The benchmarks link against the shipped form of the library. This is the real
 * shared library, built at the flags that it ships with. They do not
 * compile its sources into the benchmark binary. A call from here therefore
 * pays the same dynamic call cost that an application pays, and that is the
 * number worth reporting.
 */

#ifndef CCOL_BENCH_H
#define CCOL_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** The number of operations for each repetition, for a case that does not
 * choose its own size. */
#define BENCH_DEFAULT_N 100000

/**
 * @brief One benchmark.
 *
 * @var bench_case_t::group  The module that the case belongs to, for example
 *                           "cvector".
 * @var bench_case_t::name   The name of the case inside the group, for example
 *                           "push_int".
 * @var bench_case_t::vs     This is not NULL when the case measures a
 *                           third-party library for a comparison. It then names
 *                           that library, for example "uthash". The harness
 *                           reports such a case beside the c_collections case
 *                           of the same name. It never compares such a case
 *                           against the regression baseline, because the case
 *                           follows the performance of another project and not
 *                           of this one.
 * @var bench_case_t::setup  This builds the state. It returns NULL when an
 *                           allocation fails. The harness then skips the case
 *                           and does not stop the run.
 * @var bench_case_t::run    This does exactly n operations against that state.
 * @var bench_case_t::n      The number of operations for each repetition, for
 *                           each thread.
 * @var bench_case_t::threads The number of worker threads for the body. A value
 *                           of 0 or 1 gives an ordinary case with one thread.
 *                           Above that, the harness builds every worker and
 *                           parks it before any worker starts. The timed region
 *                           therefore holds the work and not the creation of
 *                           the threads. The reported figure is that time
 *                           divided by n multiplied by threads. You can
 *                           therefore compare it directly with the case that
 *                           has one thread, and it falls as the work spreads
 *                           out.
 * @var bench_case_t::setup_mt The fixture builder for a case with threads. The
 *                           harness tells it how many threads will use what it
 *                           returns. Give this instead of setup. Never give
 *                           both.
 * @var bench_case_t::shared_fixture This is for a case with threads. Set it to
 *                           true when all the threads share one fixture, which
 *                           measures contention on a thread-safe type. Set it
 *                           to false when each thread gets its own fixture.
 *                           False is the only correct form for the containers
 *                           that have no lock of their own, and it measures
 *                           parallel scaling instead.
 */
typedef struct bench_case {
  const char *group;
  const char *name;
  const char *vs;
  void *(*setup)(size_t n);
  void *(*setup_mt)(size_t n, unsigned threads);
  void (*run)(void *state, size_t n);
  void (*teardown)(void *state);
  size_t n;
  unsigned threads;
  bool shared_fixture;
} bench_case_t;

/** @brief Register a case. The harness copies the struct. */
void bench_add(const bench_case_t *bc);

/**
 * @brief Define a setup_mt adapter for a case whose fixture does not depend on
 *        how many threads will use it.
 */
#define BENCH_MT_SETUP(fn)                           \
  static void *fn##_mt(size_t n, unsigned threads) { \
    (void)threads;                                   \
    return fn(n);                                    \
  }

/** The thread counts at which the harness runs every form that uses more than
 * one thread. */
#define BENCH_MT_THREADS {4u, 8u, 12u}

/**
 * @brief Register one case once per entry in BENCH_MT_THREADS.
 *
 * The template gives everything except the thread count. Each registered form
 * takes the name of the template with a "_<count>t" suffix. Every form
 * therefore has its own baseline entry, and you can select each one on its own
 * with --filter.
 */
void bench_add_mt(const bench_case_t *tmpl);

/**
 * @brief Which worker of a threaded case is calling, counting from zero.
 *
 * This is always 0 for a case with one thread. A case whose threads share one
 * fixture uses this index to reach its own part of that fixture. The threads
 * then share the subject under test without also sharing the scratch space
 * around it.
 */
unsigned bench_thread_index(void);

/** @brief Monotonic clock in nanoseconds. */
double bench_now_ns(void);

/**
 * @brief Force a value to be treated as observable.
 *
 * Without this, the optimizer may remove a computation whose result nothing
 * reads. At the -O3 of the library, that turns several of these loops into
 * nothing at all. Each one then reports a figure in nanoseconds that you cannot
 * believe.
 */
static inline void bench_sink(const void *p) {
  __asm__ volatile("" : : "r"(p) : "memory");
}

/**
 * @brief Keep a computed value alive without forcing it into memory.
 *
 * A loop that accumulates into a local and passes the address of that local
 * to bench_sink() makes the compiler keep the accumulator in memory, because
 * the address escapes. Every iteration then pays a store and a reload of it,
 * and the case measures that latency and not the operation. Sink the value
 * of an accumulator through this function instead, once, after the loop.
 */
static inline void bench_sink_value(long long v) {
  __asm__ volatile("" : : "r"(v));
}

/**
 * @brief Abort the run with a message.
 *
 * Use this for a rule whose breach makes a measurement meaningless, and not
 * merely slow. A benchmark that keeps measuring after its subject stops working
 * still reports a number. That number usually flatters the subject, because a
 * fast path that fails costs less than one that works.
 */
void bench_die(const char *msg);

/** @brief A deterministic 64-bit PRNG, so every run measures the same work. */
static inline uint64_t bench_rand(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

/* Every group registers its own cases. main() calls each of these in turn. */
void bench_register_core(void);
void bench_register_maps(void);
void bench_register_concurrency(void);
void bench_register_cache(void);
void bench_register_logging(void);
void bench_register_serialization(void);
void bench_register_http(void);

#endif /* CCOL_BENCH_H */
