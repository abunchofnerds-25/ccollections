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
 * A benchmark is a bench_case_t: a setup that builds the state the measured
 * work needs, a run that does exactly n operations, and a teardown. The
 * harness only times the run, and it repeats the setup and the teardown for
 * every repetition, so a case that changes its own state, like one that fills
 * a vector or inserts into a map, measures the same work every time instead
 * of a container that keeps growing.
 *
 * Results are in nanoseconds per operation. The harness takes the median
 * across the repetitions and not the mean, because the distribution has one
 * long side: the scheduler can stop a repetition or move it to another CPU
 * for any length of time, but nothing can make a repetition faster than the
 * work it does, so one outlier moves a mean and leaves a median alone.
 *
 * The benchmarks link against the library in the form it ships in, the real
 * shared library built with its shipping flags, instead of compiling its
 * sources into the benchmark binary. A call from here then pays the same
 * dynamic call cost an application pays, and that's the number worth
 * reporting.
 */

#ifndef CCOL_BENCH_H
#define CCOL_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** The number of operations per repetition, for a case that doesn't pick its
 * own size. */
#define BENCH_DEFAULT_N 100000

/**
 * @brief One benchmark.
 *
 * @var bench_case_t::group  The module the case belongs to, for example
 *                           "cvector".
 * @var bench_case_t::name   The case's name inside the group, for example
 *                           "push_int".
 * @var bench_case_t::vs     Not NULL when the case measures a third-party
 *                           library for comparison, and then that library's
 *                           name, for example "uthash". The harness reports
 *                           such a case next to the c_collections case of the
 *                           same name but never compares it with the
 *                           regression baseline, since it follows another
 *                           project's performance and not this one's.
 * @var bench_case_t::setup  Builds the state, or returns NULL when an
 *                           allocation fails, and then the harness skips the
 *                           case instead of stopping the run.
 * @var bench_case_t::run    Does exactly n operations against that state.
 * @var bench_case_t::n      The number of operations per repetition, per
 *                           thread.
 * @var bench_case_t::threads The number of worker threads for the body. 0 or
 *                           1 gives an ordinary single-threaded case. Above
 *                           that the harness builds every worker and parks it
 *                           before any of them starts, so the timed region
 *                           holds the work and not the thread creation. The
 *                           reported figure is that time divided by n times
 *                           threads, so you can compare it directly with the
 *                           single-threaded case, and it drops as the work
 *                           spreads out.
 * @var bench_case_t::setup_mt The fixture builder for a threaded case; the
 *                           harness tells it how many threads will use what
 *                           it returns. Give this instead of setup, never
 *                           both.
 * @var bench_case_t::shared_fixture For a threaded case: true when all threads
 *                           share one fixture, which measures contention on a
 *                           thread-safe type, and false when each thread gets
 *                           its own, which measures parallel scaling instead.
 *                           False is the only correct form for containers
 *                           that have no lock of their own.
 * @var bench_case_t::rest_after_teardown_ms A fixed pause in milliseconds
 *                           after each teardown, outside the timed region.
 *                           Give it to a case that builds threads or a
 *                           reactor for every sample. Without it the time
 *                           between two samples is whatever the teardown
 *                           takes, so a library whose teardown gets faster
 *                           runs its samples closer together, heats the CPU
 *                           more, and reads slower for a reason that has
 *                           nothing to do with the work. 0 means no pause.
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
  unsigned rest_after_teardown_ms;
} bench_case_t;

/** @brief Register a case. The harness keeps its own copy of the struct. */
void bench_add(const bench_case_t *bc);

/**
 * @brief Define a setup_mt adapter for a case whose fixture doesn't depend on
 *        how many threads will use it.
 */
#define BENCH_MT_SETUP(fn)                           \
  static void *fn##_mt(size_t n, unsigned threads) { \
    (void)threads;                                   \
    return fn(n);                                    \
  }

/** The thread counts the harness runs every multi-threaded form at. */
#define BENCH_MT_THREADS {4u, 8u, 12u}

/**
 * @brief Register one case once per entry in BENCH_MT_THREADS.
 *
 * The template gives everything but the thread count. Each registered form
 * gets the template's name with a "_<count>t" suffix, so every form has its
 * own baseline entry and you can pick each one on its own with
 * --filter.
 */
void bench_add_mt(const bench_case_t *tmpl);

/**
 * @brief Which worker of a threaded case is calling, counting from zero.
 *
 * Always 0 for a single-threaded case. A case whose threads share one
 * fixture uses the index to get to its own part of that fixture, so the
 * threads share the subject under test without also sharing the scratch
 * space around it.
 */
unsigned bench_thread_index(void);

/** @brief Monotonic clock in nanoseconds. */
double bench_now_ns(void);

/**
 * @brief Force a value to be treated as observable.
 *
 * Without it the optimizer may remove a computation whose result nothing
 * reads. At the library's -O3 that turns several of these loops into nothing
 * at all, and each of them then reports a nanosecond figure you can't
 * believe.
 */
static inline void bench_sink(const void *p) {
  __asm__ volatile("" : : "r"(p) : "memory");
}

/**
 * @brief Keep a computed value alive without forcing it into memory.
 *
 * A loop that accumulates into a local and passes that local's address to
 * bench_sink() makes the compiler keep the accumulator in memory, because the
 * address escapes. Every iteration then pays a store and a reload of it, and
 * the case measures that latency and not the operation. Sink an
 * accumulator's value through this function instead, once, after the loop.
 */
static inline void bench_sink_value(long long v) {
  __asm__ volatile("" : : "r"(v));
}

/**
 * @brief Abort the run with a message.
 *
 * Use it for a rule whose breach makes a measurement meaningless and not
 * just slow. A benchmark that keeps measuring after its subject stopped
 * working still reports a number, and that number usually flatters the
 * subject, because a fast path that fails costs less than one that works.
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

/* Every group registers its own cases, and main() calls each of these. */
void bench_register_core(void);
void bench_register_maps(void);
void bench_register_concurrency(void);
void bench_register_cache(void);
void bench_register_logging(void);
void bench_register_serialization(void);
void bench_register_http(void);

#endif /* CCOL_BENCH_H */
