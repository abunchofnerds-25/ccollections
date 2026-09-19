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
 * A benchmark is a bench_case_t: a setup that builds whatever state the
 * measured work needs, a run that performs exactly n operations, and a
 * teardown. Only run is timed, and setup and teardown are repeated for every
 * repetition, so a case that mutates its state (filling a vector, inserting
 * into a map) measures the same work every time rather than an ever larger
 * container.
 *
 * Results are reported as nanoseconds per operation, taken as the median over
 * the repetitions. The median rather than the mean because the distribution is
 * one-sided: a repetition can be arbitrarily slowed by a scheduler preemption
 * or a migration, and nothing makes one arbitrarily fast, so a single outlier
 * moves a mean and leaves a median alone.
 *
 * Benchmarks link against the installed shape of the library (the real
 * libccollections.so, built at the flags it ships with) rather than compiling
 * its sources into the benchmark binary. A call from here therefore pays the
 * same dynamic-call cost an application pays, which is the number worth
 * reporting.
 */

#ifndef CCOL_BENCH_H
#define CCOL_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Operations per repetition for a case that does not choose its own size. */
#define BENCH_DEFAULT_N 100000

/**
 * @brief One benchmark.
 *
 * @var bench_case_t::group  Module the case belongs to, e.g. "cvector".
 * @var bench_case_t::name   Case name within the group, e.g. "push_int".
 * @var bench_case_t::vs     Non-NULL when this case measures a third-party
 *                           library for comparison, naming it (e.g. "uthash").
 *                           Such a case is reported next to the c_collections
 *                           case of the same name but is never compared
 *                           against the regression baseline: it tracks another
 *                           project's performance, not this one's.
 * @var bench_case_t::setup  Builds state; returns NULL on allocation failure,
 *                           which skips the case rather than aborting the run.
 * @var bench_case_t::run    Performs exactly n operations against that state.
 * @var bench_case_t::n      Operations per repetition, per thread.
 * @var bench_case_t::threads Worker threads to run the body on. 0 or 1 is an
 *                           ordinary single-threaded case. Above that, every
 *                           worker is built and parked before any of them
 *                           starts, so the timed region holds the work and not
 *                           the thread creation around it, and the reported
 *                           figure is that time divided by n*threads. It is
 *                           therefore directly comparable to the
 *                           single-threaded case and falls as the work spreads
 *                           out.
 * @var bench_case_t::setup_mt Fixture builder for a threaded case, told how
 * many threads will use what it returns. Supplied instead of setup, never
 * alongside it.
 * @var bench_case_t::shared_fixture For a threaded case: true if all threads
 *                           share one fixture, which is what measures
 *                           contention on a thread-safe type; false if each
 *                           thread gets its own, which is the only valid shape
 *                           for the deliberately unguarded containers and
 *                           measures parallel scaling instead.
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

/** @brief Register a case. The struct is copied. */
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

/** Thread counts every multi-threaded variant is run at. */
#define BENCH_MT_THREADS {4u, 8u, 12u}

/**
 * @brief Register one case once per entry in BENCH_MT_THREADS.
 *
 * The template supplies everything except threads; each registered variant
 * takes the template's name with a "_<count>t" suffix, so every variant has its
 * own baseline entry and can be selected with --filter on its own.
 */
void bench_add_mt(const bench_case_t *tmpl);

/**
 * @brief Which worker of a threaded case is calling, counting from zero.
 *
 * Always 0 for a single-threaded case. A case whose threads share one fixture
 * uses this to reach its own slice of that fixture, so that sharing the subject
 * under test does not also mean sharing the scratch space around it.
 */
unsigned bench_thread_index(void);

/** @brief Monotonic clock in nanoseconds. */
double bench_now_ns(void);

/**
 * @brief Force a value to be treated as observable.
 *
 * Without this the optimizer is free to delete a computation whose result is
 * never read, which at the library's own -O3 turns several of these loops into
 * nothing at all and reports an implausible nanoseconds-per-operation figure.
 */
static inline void bench_sink(const void *p) {
  __asm__ volatile("" : : "r"(p) : "memory");
}

/**
 * @brief Abort the run with a message.
 *
 * For invariants whose violation would make a measurement meaningless rather
 * than merely slow. A benchmark that quietly keeps timing after its subject
 * stopped working reports a number, and the number is usually flattering,
 * because a failing fast path is cheaper than a working one.
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
