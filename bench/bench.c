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
 * @file bench.c
 * @brief Benchmark driver: runs the registered cases, reports them, and
 *        compares them against a recorded baseline.
 *
 * The baseline is a JSON file. The driver reads and writes it with the cjson
 * module of this library. The file stays on the local machine on purpose, and
 * the project does not commit it. A figure in nanoseconds for each operation
 * describes the cache hierarchy, the clock behavior and the background load of
 * one machine. A run here against a figure from other hardware therefore
 * reports a difference that has nothing to do with a change to the library.
 * What is worth measuring is one machine before a change and after it. `make
 * bench_update`, and then `make bench`, gives you that.
 *
 * The driver reports a case that measures a third-party library for
 * comparison. It never checks such a case against the baseline. The times of
 * that case follow the performance of another project, and the version of that
 * project on this machine. This repository controls neither of them.
 */

#include "bench.h"

#include <cjson.h>
#include <errno.h>
#include <pthread.h>
#if defined(__FreeBSD__)
#include <pthread_np.h> /* pthread_setaffinity_np */
#endif
/* macOS has no call that binds a thread to a CPU. Its affinity tags are only
 * hints, and Apple silicon ignores them. The harness therefore runs unpinned
 * there, and it says so in its header. */
#ifndef BENCH_CAN_PIN
#if defined(__APPLE__)
#define BENCH_CAN_PIN 0
#else
#define BENCH_CAN_PIN 1
#endif
#endif
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BENCH_MAX_CASES 256
/* Real time bounds the sampling. A fixed number of repetitions does not. A
 * fixed number treats a case that costs microseconds and a case that costs
 * seconds in the same way. It therefore takes too few samples of the cheap
 * case to be useful, or it makes the expensive case impossible to run. A time
 * budget spends the same effort everywhere. Each case takes as many samples as
 * that budget pays for.
 *
 * A time budget also survives a change in the speed of the machine. The number
 * of samples follows what the CPU does. A run on a faster clock therefore
 * collects more samples. It does not measure something that you cannot compare
 * with the last run.
 *
 * The floor gives enough samples for a median and a spread to mean something,
 * even when that goes past the budget. The ceiling stops a very cheap case
 * from spending its whole budget long after the numbers stop moving. */
#define BENCH_DEFAULT_BUDGET_SEC 5.0
#define BENCH_MIN_SAMPLES 10
#define BENCH_MAX_SAMPLES 2000
/* A regression must be larger than this before the driver reports it. Repeated
 * runs of an unchanged library on an idle machine stay inside a few percent. A
 * shared or virtual host moves much more. On such a host, a threshold tight
 * enough to catch every real 5 percent regression reports about a dozen false
 * ones in each run. That is how a performance check stops being read. */
#define BENCH_DEFAULT_THRESHOLD_PCT 20.0

/* Each case has its own threshold, from the variation that the case shows
 * between runs on this machine. --calibrate records that variation. One
 * constant shared by every case cannot serve both ends of the range. Take a
 * laptop whose cores have two different speeds and share one package power
 * budget. There, the median of a twelve-thread case moves by more than half
 * between two runs of an unchanged library, and a single-threaded case on the
 * same machine repeats to a few percent. A threshold wide enough to keep the
 * first case quiet cannot catch a real regression in the second. Where the
 * machine contributes more to a measurement than the library does, the honest
 * answer is to report the case and to gate nothing on it.
 * BENCH_UNGATEABLE_PCT below decides that. */
/* Every figure here depends on the temperature of the die at the instant that
 * the driver takes it, and a run heats the machine that it measures. Measured
 * on a laptop with this suite: the package reaches its ceiling about a minute
 * into a run, and one unchanged case costs about 15 percent more at 98C than at
 * 55C. The cases with many threads are much worse, because a package power
 * budget bounds their clock, and every active core shares that budget. A run
 * that starts cold therefore measures its first group in a state that its last
 * group can never be in. You can compare two runs only when both start from
 * the same state.
 *
 * --warmup loads every core first, so that a run starts in the state that a
 * long run reaches anyway. It is off by default, because it has a side effect
 * of its own. It adds heat. A sequence of short runs therefore starts hotter
 * each time, and on a machine that loses heat slowly it makes those runs
 * harder to compare, not easier. It helps one full run that starts on a cold
 * machine. Measure whether it helps on your own machine before you turn it on.
 * Do not assume. */
#define BENCH_DEFAULT_WARMUP_SEC 0.0
#define BENCH_DEFAULT_CALIBRATE_PASSES 5
/* The driver makes the measured variation larger by this amount before it uses
 * it as a limit. A few passes sample the spread. They do not bound it. A later
 * run can therefore land outside the range that those passes covered, while
 * nothing has changed. */
#define BENCH_GATE_SAFETY 1.5
/* No case gets a threshold tighter than this, whatever its calibration
 * showed. A machine that nothing disturbed during the calibration therefore
 * cannot set a threshold that no later run can meet. */
#define BENCH_GATE_FLOOR_PCT 5.0
/* Above this value, the driver reports a case but never gates on it. A real
 * regression in such a case still appears as a number that moved, and it is
 * still worth reading. It cannot be an automatic failure, because such a gate
 * would also fail on a tree that nobody changed. */
#define BENCH_UNGATEABLE_PCT 30.0

/* What one case measured. spread_pct is the p90-to-p10 range as a percentage
 * of the median. One outlier does not move it, and the maximum minus the
 * minimum does move. It is also what lets a reader understand a reported
 * difference. A change smaller than the spread of a case is not a result. */
typedef struct {
  double median;
  double spread_pct;
  size_t samples;
} bench_result_t;

typedef struct {
  bench_case_t bc;
  bench_result_t result;
  bool ran;
  /* This differs from !ran. A case that a --filter left out keeps whatever the
     baseline already recorded for it, while one that ran and could not be
     measured has no current figure and must not keep a stale one. */
  bool skipped;
  /* --calibrate fills this in. It is how far the median of this case moved
   * across whole
     suite passes. Kept per case because it differs by two orders of magnitude
     between a single-threaded case and a twelve-thread one, which is the whole
     reason a single shared threshold does not work. */
  double gate_pct;
  bool have_gate;
} bench_entry_t;

static bench_entry_t g_cases[BENCH_MAX_CASES];
static size_t g_case_count;

void bench_add(const bench_case_t *bc) {
  if (g_case_count >= BENCH_MAX_CASES) {
    fprintf(stderr, "bench: case table full (%d); raise BENCH_MAX_CASES\n",
            BENCH_MAX_CASES);
    exit(2);
  }
  g_cases[g_case_count].bc = *bc;
  g_cases[g_case_count].result = (bench_result_t){0};
  g_cases[g_case_count].ran = false;
  g_cases[g_case_count].skipped = false;
  g_cases[g_case_count].gate_pct = 0.0;
  g_cases[g_case_count].have_gate = false;
  g_case_count++;
}

/* The names for the forms that bench_add_mt generates. bench_add copies the
 * case struct. It does not copy the strings that the struct points at. The
 * generated name therefore needs storage that lives longer than the stack of
 * the caller. */
static char g_mt_names[BENCH_MAX_CASES][64];
static size_t g_mt_name_count;

void bench_add_mt(const bench_case_t *tmpl) {
  static const unsigned counts[] = BENCH_MT_THREADS;

  for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
    if (g_mt_name_count >= BENCH_MAX_CASES) {
      fprintf(stderr, "bench: case table full (%d); raise BENCH_MAX_CASES\n",
              BENCH_MAX_CASES);
      exit(2);
    }
    char *name = g_mt_names[g_mt_name_count++];
    snprintf(name, sizeof(g_mt_names[0]), "%s_%ut", tmpl->name, counts[i]);

    bench_case_t bc = *tmpl;
    bc.name = name;
    bc.threads = counts[i];
    bench_add(&bc);
  }
}

void bench_die(const char *msg) {
  fprintf(stderr, "\nbench: FATAL: %s\n", msg);
  fflush(stderr);
  abort();
}

double bench_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* A stored value with a fractional part of zero serializes with no decimal
 * point. It then parses back as an integer node. This function therefore
 * accepts both number types. It returns 0 for anything else, and also for a
 * key that is not there. */
static double bench_number(cjson node) {
  if (!node) return 0.0;
  cjson_node_type_t t = cjson_type(node);
  if (t == CJSON_FLOAT) return cjson_double_val(node);
  if (t == CJSON_INTEGER) return (double)cjson_int_val(node);
  return 0.0;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/**
 * @brief Run one case and return its median nanoseconds per operation.
 *
 * This returns a negative value when the case could not run. That happens when
 * the setup of the case reports a failure. A benchmark that cannot allocate its
 * fixture measures nothing. The driver therefore skips it, which keeps the
 * numbers of every other case. If the driver stopped the process instead, it
 * would lose all of them.
 */
/* One worker of a case that uses threads. */
typedef struct {
  const bench_case_t *bc;
  void *state;
  unsigned index;
  atomic_uint *ready;
  atomic_int *go;
} bench_worker_t;

static __thread unsigned g_thread_index;

unsigned bench_thread_index(void) { return g_thread_index; }

/* This list holds one logical CPU for each physical core, fastest first. No
 * measured thread then shares a core with another thread. The two SMT threads
 * of one core are not two cores. They share the execution resources of that
 * core. Two workers on one core therefore run at about half speed, and the same
 * two workers on two cores do not. The scheduler chooses again on every run, so
 * you cannot know which one you get. Measured on a machine with six performance
 * cores: four workers, pinned one to each physical core, took a compute-bound
 * reference from 23 percent variation between runs down to 3.6 percent.
 *
 * This list comes from sysfs. Do not derive it from the CPU numbers, which
 * follow no single convention. On some machines the siblings are next to each
 * other, such as 1-2 and 3-4. On other machines they are half a table apart. On
 * the first kind, "use the first half of the CPUs" picks a set that holds
 * nothing but sibling pairs.
 *
 * Where a machine has two kinds of core, this list holds only the performance
 * cores. On such a machine the efficiency cores run about a fifth slower. A
 * case ends when its slowest worker ends. One worker on an efficiency core
 * therefore decides the whole figure, and which worker that is changes from run
 * to run. /sys/devices/cpu_core/cpus names the performance cores. It is the
 * same list that perf reads to separate the two PMUs. Where that file is
 * missing, every core is the same kind and this list holds all of them.
 *
 * The order puts the one-for-each-core entries first. The second thread of each
 * core comes only after every core has one entry. A case with fewer workers
 * than cores therefore shares nothing. A case with more workers shares in the
 * same way on every run, and not in a different way each time. */
#define BENCH_MAX_CPUS 256

typedef struct {
  int cpu;
  long khz;
} bench_cpu_t;

static bench_cpu_t g_cpu_order[BENCH_MAX_CPUS];
static size_t g_cpu_count;
static bool g_pin_enabled = true;
/* Set by any thread whose pin call fails, and by topology discovery when it
   cannot tell cores from sibling threads. Either way the run's figures are not
   what the header claims they are, so this is reported rather than swallowed:
   an unpinned run under a "pinned" banner is a wrong published number. */
static _Atomic bool g_pin_failed = false;
static _Atomic bool g_topology_degraded = false;

#if BENCH_CAN_PIN
static long read_long_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  long v = -1;
  if (fscanf(f, "%ld", &v) != 1) v = -1;
  fclose(f);
  return v;
}

/* The first entry of thread_siblings_list names the core. Every sibling of one
 * core reports the same first entry. Keep only the CPUs that name themselves
 * there, and you keep exactly one CPU for each core. */
static int siblings_leader(int cpu) {
  char path[128];
  snprintf(path, sizeof(path),
           "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  int leader = -1;
  if (fscanf(f, "%d", &leader) != 1) leader = -1;
  fclose(f);
  return leader;
}

static int cmp_cpu_desc(const void *a, const void *b) {
  const bench_cpu_t *x = a, *y = b;
  if (x->khz != y->khz) return x->khz < y->khz ? 1 : -1;
  return x->cpu < y->cpu ? -1 : (x->cpu > y->cpu);
}

/* This parses a sysfs CPU list into set. Two examples of such a list are "0-11"
 * and "0,5,8-10". It returns false when the file is not there. A machine with
 * one kind of core reports itself in that way, and that is not an error. */
static bool read_cpu_list(const char *path, cpu_set_t *set) {
  FILE *f = fopen(path, "r");
  if (!f) return false;
  CPU_ZERO(set);
  int a, b;
  bool any = false;
  for (;;) {
    int n = fscanf(f, "%d", &a);
    if (n != 1) break;
    b = a;
    int c = fgetc(f);
    if (c == '-') {
      if (fscanf(f, "%d", &b) != 1) break;
      c = fgetc(f);
    }
    for (int cpu = a; cpu <= b && cpu < CPU_SETSIZE; cpu++) {
      CPU_SET(cpu, set);
      any = true;
    }
    if (c != ',') break;
  }
  fclose(f);
  return any;
}

static void bench_topology_init(void) {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    g_cpu_count = 0;
    return;
  }
  cpu_set_t perf_cores;
  bool have_perf = read_cpu_list("/sys/devices/cpu_core/cpus", &perf_cores);

  /* This makes two passes. The first pass takes the first thread of every core.
   * The second pass takes the other threads. A case that asks for no more
   * workers than there are cores therefore never shares a core. */
  for (int pass = 0; pass < 2; pass++) {
    for (int cpu = 0; cpu < CPU_SETSIZE && cpu < BENCH_MAX_CPUS; cpu++) {
      if (!CPU_ISSET(cpu, &allowed)) continue;
      if (have_perf && !CPU_ISSET(cpu, &perf_cores)) continue;
      int sib = siblings_leader(cpu);
      /* Absent on some containers and older sysfs layouts. Every CPU then
         fails the leader test, so the first sweep takes nothing and the second
         takes every CPU: the list is still non-empty and still gets pinned, but
         the cores-before-sibling-threads ordering it is printed as is gone, and
         a two-worker case can land both workers on one core's two threads. */
      if (sib < 0)
        atomic_store_explicit(&g_topology_degraded, true, memory_order_relaxed);
      bool leader = (sib == cpu);
      if (leader != (pass == 0)) continue;
      char path[128];
      snprintf(path, sizeof(path),
               "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
      if (g_cpu_count >= BENCH_MAX_CPUS) break;
      g_cpu_order[g_cpu_count].cpu = cpu;
      /* A machine without cpufreq reports nothing; every core then sorts equal
         and the order falls back to CPU number, which is the right answer when
         there is no evidence that the cores differ. */
      g_cpu_order[g_cpu_count].khz = read_long_file(path);
      g_cpu_count++;
    }
    /* Sorted within the sweep, so the fastest cores are used first and the
       leaders-before-siblings split above is preserved. */
    size_t base = 0;
    if (pass == 1) {
      for (size_t i = 0; i < g_cpu_count; i++)
        if (siblings_leader(g_cpu_order[i].cpu) == g_cpu_order[i].cpu) base++;
      qsort(g_cpu_order + base, g_cpu_count - base, sizeof(g_cpu_order[0]),
            cmp_cpu_desc);
    } else {
      qsort(g_cpu_order, g_cpu_count, sizeof(g_cpu_order[0]), cmp_cpu_desc);
    }
  }
}

/* slot is the index of the worker. It goes back to the start only when a case
 * asks for more threads than the machine has cores. That is the one situation
 * where sharing is the purpose and not an accident. */
static void bench_pin(size_t slot) {
  if (!g_pin_enabled || g_cpu_count == 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(g_cpu_order[slot % g_cpu_count].cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
    atomic_store_explicit(&g_pin_failed, true, memory_order_relaxed);
}

/* A thread inherits the CPU affinity of the thread that creates it. A fixture
 * that starts threads of its own, such as the workers of a thread pool or the
 * writer of an asynchronous logger, would otherwise start them on the single
 * CPU that the driver is pinned to. Every one of them would then time-share
 * that core with the driver, and with the benchmark worker pinned to the same
 * CPU, and the case would measure the order in which the scheduler wakes them
 * rather than the library. The driver therefore lets a fixture's own threads
 * use every CPU in the pin list while it builds the fixture, and pins itself
 * again afterwards. */
static void bench_widen_for_setup(void) {
  if (!g_pin_enabled || g_cpu_count == 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (size_t i = 0; i < g_cpu_count; i++) CPU_SET(g_cpu_order[i].cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
    atomic_store_explicit(&g_pin_failed, true, memory_order_relaxed);
}

#else
static void bench_topology_init(void) { g_cpu_count = 0; }
static void bench_pin(size_t slot) { (void)slot; }
static void bench_widen_for_setup(void) {}
#endif

static void bench_restore_after_setup(void) { bench_pin(0); }

static void *bench_worker_main(void *arg) {
  bench_worker_t *w = arg;
  g_thread_index = w->index;
  /* This happens before the gate. The clock therefore never covers the move,
   * and every sample of this case runs on the same cores as the sample before
   * it. */
  bench_pin(w->index);
  /* Each worker reports that it is ready, and then waits for the release. The
   * clock starts on the other side of this gate. It therefore covers the
   * measured work and not the cost of creating the threads. A real caller
   * creates its threads one time and then runs many operations on them. If the
   * driver charged thread creation to the operation count, it would tax the
   * forms with many threads for work that they do not do for each operation.
   *
   * This wait cannot hang on a thread that nothing created. The side that
   * releases the gate sets the flag for the number of threads that it really
   * started. */
  atomic_fetch_add_explicit(w->ready, 1, memory_order_relaxed);
  while (!atomic_load_explicit(w->go, memory_order_acquire)) sched_yield();
  w->bc->run(w->state, w->bc->n);
  return NULL;
}

/* This builds the fixture of a case, runs its body one time, and then frees the
 * fixture. It returns the real time inside the body alone. For a case with
 * threads, the body runs on bc->threads workers. The driver releases all of
 * them together, once every one of them is ready. The measured time runs from
 * that release to the end of the last worker.
 *
 * This returns a negative time when it could not build the fixture. The driver
 * then skips the case. It does not report a measurement of nothing. */
static double bench_one_pass(const bench_case_t *bc) {
  unsigned threads = bc->threads > 1 ? bc->threads : 1;

  if (threads == 1) {
    bench_widen_for_setup();
    void *state = bc->setup ? bc->setup(bc->n) : bc->setup_mt(bc->n, 1);
    bench_restore_after_setup();
    if (!state) return -1.0;
    double t0 = bench_now_ns();
    bc->run(state, bc->n);
    double t1 = bench_now_ns();
    bc->teardown(state);
    return t1 - t0;
  }

  pthread_t *th = calloc(threads, sizeof(*th));
  bench_worker_t *w = calloc(threads, sizeof(*w));
  void **owned = calloc(threads, sizeof(void *));
  if (!th || !w || !owned) {
    free(th);
    free(w);
    free(owned);
    return -1.0;
  }

  atomic_uint ready = 0;
  atomic_int go = 0;

  /* The driver builds a shared fixture one time and gives it to every worker.
   * That is what makes the workers contend. It builds a private fixture for
   * each worker. A private fixture is the only correct form for a type that has
   * no lock of its own. */
  void *shared = NULL;
  if (bc->shared_fixture) {
    bench_widen_for_setup();
    shared = bc->setup_mt(bc->n, threads);
    bench_restore_after_setup();
    if (!shared) {
      free(th);
      free(w);
      free(owned);
      return -1.0;
    }
  }
  for (unsigned i = 0; i < threads; i++) {
    if (!bc->shared_fixture) {
      bench_widen_for_setup();
      owned[i] = bc->setup_mt(bc->n, threads);
      bench_restore_after_setup();
      if (!owned[i]) {
        for (unsigned j = 0; j < i; j++) bc->teardown(owned[j]);
        free(th);
        free(w);
        free(owned);
        return -1.0;
      }
    }
    w[i].bc = bc;
    w[i].state = bc->shared_fixture ? shared : owned[i];
    w[i].index = i;
    w[i].ready = &ready;
    w[i].go = &go;
  }

  unsigned started = 0;
  for (unsigned i = 0; i < threads; i++) {
    if (pthread_create(&th[i], NULL, bench_worker_main, &w[i]) != 0) break;
    started++;
  }
  while (atomic_load_explicit(&ready, memory_order_relaxed) < started)
    sched_yield();
  double t0 = bench_now_ns();
  atomic_store_explicit(&go, 1, memory_order_release);
  for (unsigned i = 0; i < started; i++) pthread_join(th[i], NULL);
  double t1 = bench_now_ns();

  if (bc->shared_fixture) {
    bc->teardown(shared);
  } else {
    for (unsigned i = 0; i < threads; i++) bc->teardown(owned[i]);
  }
  free(th);
  free(w);
  free(owned);
  /* A case that could not start every worker measured something other than what
   * it claims to measure. The driver therefore reports it as skipped, and not
   * as a fast result. */
  return started == threads ? (t1 - t0) : -1.0;
}

static volatile uint64_t g_warmup_sink;

static void *warmup_spin(void *arg) {
  double deadline = *(double *)arg;
  uint64_t x = 0x9e3779b97f4a7c15ULL;
  while (bench_now_ns() < deadline) {
    for (int i = 0; i < 4096; i++) {
      x ^= x >> 30;
      x *= 0xbf58476d1ce4e5b9ULL;
      x ^= x >> 27;
    }
  }
  g_warmup_sink = x;
  return NULL;
}

/* This loop is compute-bound and uses no part of the library, on purpose. What
 * it must reproduce is the thermal state and the clock state that a run
 * reaches. It does not have to reproduce any particular workload. */
static void bench_warmup(double seconds) {
  if (seconds <= 0.0) return;
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) n = 1;
  if (n > 64) n = 64;
  double deadline = bench_now_ns() + seconds * 1e9;
  pthread_t *th = calloc((size_t)n, sizeof(pthread_t));
  unsigned started = 0;
  if (th) {
    for (long i = 0; i < n; i++)
      if (pthread_create(&th[i], NULL, warmup_spin, &deadline) == 0) started++;
  }
  /* The calling thread runs the loop as well. A machine where no worker could
   * start therefore still gets warm. It is not left cold without a word. */
  warmup_spin(&deadline);
  for (unsigned i = 0; i < started; i++) pthread_join(th[i], NULL);
  free(th);
}

/* This collects samples until the budget is gone, or until it reaches the
 * ceiling. When fixed_reps is not zero, it stops after that many samples. It
 * never stops below BENCH_MIN_SAMPLES, even when that goes past the budget. A
 * median of three numbers is not worth reporting. */
static bool run_case(const bench_case_t *bc, double budget_sec,
                     size_t fixed_reps, double *samples, bench_result_t *out) {
  /* One repetition runs first, and the driver does not time it. It pays the
   * first-touch page faults of the fixture, fills the branch predictors, and
   * warms the instruction cache. Without it, all of that work lands on the
   * first timed repetition. That repetition is then the slowest of the set, for
   * reasons that have nothing to do with the code. */
  if (bench_one_pass(bc) < 0.0) return false; /* untimed warm-up */

  size_t ceiling = fixed_reps ? fixed_reps : BENCH_MAX_SAMPLES;
  size_t floor_n = fixed_reps ? fixed_reps : BENCH_MIN_SAMPLES;
  double deadline = bench_now_ns() + budget_sec * 1e9;
  size_t count = 0;

  /* The driver divides by the total number of operations across all the
   * workers. You can therefore compare a figure from several threads directly
   * with the figure from the same case on one thread. */
  double ops = (double)bc->n * (bc->threads > 1 ? bc->threads : 1);

  while (count < ceiling) {
    double elapsed = bench_one_pass(bc);
    if (elapsed < 0.0) return false;
    samples[count++] = elapsed / ops;
    if (count >= floor_n && !fixed_reps && bench_now_ns() >= deadline) break;
  }

  qsort(samples, count, sizeof(double), cmp_double);
  out->median = samples[count / 2];
  out->samples = count;
  double p10 = samples[(size_t)(count * 0.10)];
  double p90 =
      samples[(size_t)(count * 0.90 > count - 1 ? count - 1 : count * 0.90)];
  out->spread_pct =
      (out->median > 0.0) ? 100.0 * (p90 - p10) / out->median : 0.0;
  return true;
}

static void format_rate(double ns_per_op, char *buf, size_t buflen) {
  if (ns_per_op <= 0.0) {
    snprintf(buf, buflen, "%s", "n/a");
    return;
  }
  double per_sec = 1e9 / ns_per_op;
  if (per_sec >= 1e6)
    snprintf(buf, buflen, "%.2f M/s", per_sec / 1e6);
  else if (per_sec >= 1e3)
    snprintf(buf, buflen, "%.2f K/s", per_sec / 1e3);
  else
    snprintf(buf, buflen, "%.1f /s", per_sec);
}

/* ------------------------------------------------------------------------ */
/* Baseline I/O                                                             */
/* ------------------------------------------------------------------------ */

static char *read_whole_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  /* A baseline is a few kilobytes of JSON. A file past this ceiling is not a
   * baseline. Never give an allocator an ftell() result with no bound. A path
   * with a typing mistake then becomes a request for several gigabytes. */
  if (len < 0 || len > 16L * 1024L * 1024L) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)len, f);
  fclose(f);
  buf[got] = '\0';
  return buf;
}

/* This attaches one value to entry, and it owns that value in both cases.
 *
 * cjson_dictionary_set takes ownership of the child whatever happens. When the
 * insert fails, it destroys the child itself. It does not give the child back.
 * Its own documentation promises this. This function therefore frees the child
 * only when it never gave the child away. A destroy after a failed set is a
 * double free and not a cleanup. */
static bool baseline_put(cjson entry, const char *key, cjson value) {
  if (!value) return false;
  return cjson_dictionary_set(entry, key, value) == ccol_success;
}

/* This builds one baseline entry. It attaches every node as soon as that node
 * exists. An allocation that fails part way therefore leaves nothing without an
 * owner. A destroy of the entry frees what this function attached. The set that
 * failed has already freed what it refused. What nothing created needs no
 * free. */
static cjson baseline_entry(double ns, double spread_pct, size_t samples,
                            double gate_pct, bool have_gate) {
  cjson entry = cjson_create_dictionary();
  if (!entry) return NULL;

  if (!baseline_put(entry, "ns", cjson_create_double(ns)) ||
      !baseline_put(entry, "spread_pct", cjson_create_double(spread_pct)) ||
      !baseline_put(entry, "samples", cjson_create_int((long long)samples))) {
    cjson_destroy(entry);
    return NULL;
  }
  /* This key is missing, and not zero, when somebody recorded the baseline
   * without a calibration. The comparison can then tell "measured and quiet"
   * from "never measured". For the second case it uses the flat floor. Without
   * this, it would gate every case at a tolerance of zero. */
  if (have_gate &&
      !baseline_put(entry, "gate_pct", cjson_create_double(gate_pct))) {
    cjson_destroy(entry);
    return NULL;
  }
  return entry;
}

/* previous is what the driver loaded from the file, or NULL. A case that did
 * not run this time, because a --filter left it out, keeps the figure that the
 * file already holds for it. It does not disappear. The driver writes the whole
 * file again. If it wrote only the cases that ran, it would quietly lose the
 * baseline of every other case. */
static int write_baseline(const char *path, cjson previous) {
  cjson root = cjson_create_dictionary();
  if (!root) return -1;

  int rc = -1;
  for (size_t i = 0; i < g_case_count; i++) {
    if (g_cases[i].bc.vs) continue;
    char key[256];
    snprintf(key, sizeof(key), "%s/%s", g_cases[i].bc.group,
             g_cases[i].bc.name);
    /* The driver records the spread beside the median, because a later
     * comparison must know what "different" means for this case. One flat
     * percentage for every case either fills the quiet cases with false alarms,
     * or lets a real regression hide inside a noisy one. */
    cjson entry;
    if (g_cases[i].ran) {
      entry = baseline_entry(
          g_cases[i].result.median, g_cases[i].result.spread_pct,
          g_cases[i].result.samples, g_cases[i].gate_pct, g_cases[i].have_gate);
    } else if (g_cases[i].skipped) {
      /* The case ran and the driver could not measure it. To drop it is the
       * honest answer. A
         carried-over figure would be compared against on the next run as
         though it described this build. */
      continue;
    } else {
      cjson prev = previous ? cjson_dictionary_get(previous, key) : NULL;
      if (!prev) continue;
      /* A baseline entry can be a plain number and not a dictionary. The
         comparison path below accepts that form too. A read through
         cjson_dictionary_get alone would write it back as zero. */
      cjson prev_ns = cjson_dictionary_get(prev, "ns");
      cjson prev_gate = cjson_dictionary_get(prev, "gate_pct");
      entry = baseline_entry(
          bench_number(prev_ns ? prev_ns : prev),
          bench_number(cjson_dictionary_get(prev, "spread_pct")),
          (size_t)bench_number(cjson_dictionary_get(prev, "samples")),
          bench_number(prev_gate), prev_gate != NULL);
    }
    if (!entry) goto out;
    /* Ownership passes to the callee whether the insert succeeds or not. A
     * failure here therefore needs no destroy of its own. See baseline_put. */
    if (cjson_dictionary_set(root, key, entry) != ccol_success) goto out;
  }

  char *text = cjson_serialize_pretty(root, 2);
  if (!text) goto out;

  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "bench: cannot write %s: %s\n", path, strerror(errno));
    cjson_serialize_free(text);
    goto out;
  }
  /* The driver checks every write, and it checks fclose last. The data sits in
   * a buffer, so a full disk or a quota appears at the flush and not at the
   * fputs. A report of success over a truncated baseline would make the next
   * run compare against a file that describes nothing. */
  bool written = (fputs(text, f) >= 0);
  written = written && (fputc('\n', f) != EOF);
  if (fclose(f) != 0) written = false;
  cjson_serialize_free(text);
  if (!written) {
    fprintf(stderr, "bench: writing %s failed: %s\n", path, strerror(errno));
    goto out;
  }
  rc = 0;

out:
  cjson_destroy(root);
  return rc;
}

/* ------------------------------------------------------------------------ */

static void usage(const char *argv0) {
  printf(
      "usage: %s [options]\n"
      "  --list                 list the registered cases and exit\n"
      "  --filter=SUBSTR        run only cases whose group/name contains "
      "SUBSTR\n"
      "  --budget=SEC           sampling time per case (default %.1f)\n"
      "  --reps=N               fixed sample count, overriding --budget\n"
      "  --warmup=SEC           load every core for this long before "
      "measuring\n"
      "                         (default %.0f, meaning off), so a "
      "cold-started\n"
      "                         run begins in the thermal state a long run\n"
      "                         settles into anyway. It adds heat of its own,\n"
      "                         so it helps one full run and makes a sequence\n"
      "                         of short ones less comparable, not more\n"
      "  --baseline=PATH        baseline file (default bench/baseline.json)\n"
      "  --update               record this run as the baseline\n"
      "  --no-pin               do not pin workers to cores. Pinning is on by\n"
      "                         default: one logical CPU per physical core,\n"
      "                         fastest first, so two workers never share one\n"
      "                         core's execution resources by accident\n"
      "  --gate                 exit non-zero when a gated case regresses.\n"
      "                         Off by default: report, do not fail. Turn it\n"
      "                         on only where --calibrate has shown the\n"
      "                         machine repeats well enough to carry it\n"
      "  --calibrate[=K]        run the whole suite K times (default %d) and\n"
      "                         record, per case, how far its median moved\n"
      "                         across those passes; that becomes the case's\n"
      "                         own gate. Writes the baseline, so it replaces\n"
      "                         --update rather than accompanying it.\n"
      "  --threshold=PCT        smallest limit any case may be held to\n"
      "                         (default %.0f); a calibrated case whose own\n"
      "                         measured variation is wider is held to that\n"
      "                         instead, and one above %.0f%% is reported but\n"
      "                         never gated\n",
      argv0, BENCH_DEFAULT_BUDGET_SEC, BENCH_DEFAULT_WARMUP_SEC,
      BENCH_DEFAULT_CALIBRATE_PASSES, BENCH_GATE_FLOOR_PCT,
      BENCH_UNGATEABLE_PCT);
}

int main(int argc, char **argv) {
  const char *filter = NULL;
  const char *baseline_path = "bench/baseline.json";
  size_t reps = 0; /* 0 means "use the time budget" */
  double budget = BENCH_DEFAULT_BUDGET_SEC;
  double threshold = BENCH_GATE_FLOOR_PCT;
  bool update = false, list_only = false;
  size_t calibrate = 0;
  double warmup = BENCH_DEFAULT_WARMUP_SEC;
  /* To report and to fail are two separate decisions. Whether a machine can
   * carry the second is a question that you answer by measurement. Measured
   * here over full-suite runs of an unchanged library, with each case held to
   * the variation from its own calibration: about half of all runs still
   * flagged at least one case, and half of the suite moved too much to gate at
   * all. A check that fails that often stops being read, and that costs more
   * than the regressions it would have caught. The driver therefore always
   * reports the numbers. --gate decides whether a flagged case is also a
   * non-zero exit, for a machine that somebody has calibrated and shown to
   * hold it. */
  bool gate = false;

  bench_topology_init();
  bench_register_core();
  bench_register_maps();
  bench_register_concurrency();
  bench_register_cache();
  bench_register_logging();
  bench_register_serialization();
  bench_register_http();

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strncmp(a, "--filter=", 9))
      filter = a + 9;
    else if (!strncmp(a, "--reps=", 7))
      reps = (size_t)strtoul(a + 7, NULL, 10);
    else if (!strncmp(a, "--budget=", 9))
      budget = strtod(a + 9, NULL);
    else if (!strncmp(a, "--warmup=", 9))
      warmup = strtod(a + 9, NULL);
    else if (!strncmp(a, "--baseline=", 11))
      baseline_path = a + 11;
    else if (!strncmp(a, "--threshold=", 12))
      threshold = strtod(a + 12, NULL);
    else if (!strcmp(a, "--update"))
      update = true;
    else if (!strncmp(a, "--calibrate", 11)) {
      calibrate = (a[11] == '=') ? (size_t)strtoul(a + 12, NULL, 10)
                                 : BENCH_DEFAULT_CALIBRATE_PASSES;
      if (calibrate < 2) {
        fprintf(
            stderr,
            "bench: --calibrate needs at least 2 passes; one pass measures\n"
            "       no variation at all and would gate every case at the\n"
            "       floor\n");
        return 2;
      }
      update = true;
    } else if (!strcmp(a, "--no-pin"))
      g_pin_enabled = false;
    else if (!strcmp(a, "--gate"))
      gate = true;
    else if (!strcmp(a, "--list"))
      list_only = true;
    else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "bench: unknown option %s\n", a);
      usage(argv[0]);
      return 2;
    }
  }
  if (list_only) {
    for (size_t i = 0; i < g_case_count; i++)
      printf("%s/%s%s%s\n", g_cases[i].bc.group, g_cases[i].bc.name,
             g_cases[i].bc.vs ? "  vs " : "",
             g_cases[i].bc.vs ? g_cases[i].bc.vs : "");
    return 0;
  }

  size_t sample_cap = reps ? reps : BENCH_MAX_SAMPLES;
  /* The driver bounds this before the multiply. --reps comes straight from the
   * command line,
     and a value above this wraps the product to a small allocation that the
     sample loop then writes past. */
  if (sample_cap > SIZE_MAX / sizeof(double)) {
    fprintf(stderr, "bench: --reps is too large\n");
    return 2;
  }
  double *samples = malloc(sizeof(double) * sample_cap);
  if (!samples) {
    fprintf(stderr, "bench: out of memory\n");
    return 2;
  }

  /* The driver loads the baseline before the run. A baseline that is missing,
   * or that it cannot parse, therefore appears at once. It does not appear
   * after several minutes of measurement. */
  cjson baseline = NULL;
  if (!update) {
    char *text = read_whole_file(baseline_path);
    if (text) {
      char *err = NULL;
      baseline = cjson_parse(text, &err);
      free(text);
      if (!baseline) {
        /* The message belongs to the library. It lives in storage for each
         * thread that the next parse on this thread reuses, so nothing here
         * frees it. */
        fprintf(stderr, "bench: cannot parse %s (%s); running without it\n",
                baseline_path, err ? err : "unknown error");
      }
    }
  }

  if (g_pin_enabled && g_cpu_count) {
    /* The cases with one thread run on this thread, so the driver pins it as
     * well. */
    bench_pin(0);
    if (atomic_load_explicit(&g_topology_degraded, memory_order_relaxed))
      printf(
          "pinned to %zu CPU(s), but this machine does not report thread\n"
          "       siblings, so these are NOT one per core and two workers\n"
          "       may share one core's threads:",
          g_cpu_count);
    else
      printf("pinned to %zu CPU(s), cores before sibling threads:",
             g_cpu_count);
    for (size_t i = 0; i < g_cpu_count && i < 16; i++)
      printf(" %d", g_cpu_order[i].cpu);
    printf("%s\n", g_cpu_count > 16 ? " ..." : "");
  } else if (g_pin_enabled && !BENCH_CAN_PIN) {
    printf(
        "bench: this system cannot bind a thread to a core; running\n"
        "       unpinned, so a worker may share a core with another and\n"
        "       figures will vary more between runs\n");
  } else if (g_pin_enabled) {
    printf(
        "bench: could not read this machine's core topology; running\n"
        "       unpinned, so a worker may share a core with another and\n"
        "       figures will vary more between runs\n");
  }

  if (warmup > 0.0) {
    printf(
        "warming up for %.0fs so this run starts from the same thermal\n"
        "state it will spend most of itself in\n",
        warmup);
    fflush(stdout);
    bench_warmup(warmup);
  }

  if (calibrate) {
    /* The driver makes whole passes over the suite. It never repeats one case
     * several times together. A gate must survive the difference between two
     * independent runs, and a large part of that difference is where in the
     * run a case sits. The machine is cool for the first group and fully hot
     * by the last one. A case that the driver measures two times together
     * therefore reports a steadiness that it does not have at the moment of
     * the real comparison. */
    double *meds = calloc(g_case_count * calibrate, sizeof(double));
    if (!meds) {
      fprintf(stderr, "bench: out of memory\n");
      free(samples);
      return 2;
    }
    for (size_t pass = 0; pass < calibrate; pass++) {
      printf("calibration pass %zu of %zu\n", pass + 1, calibrate);
      fflush(stdout);
      for (size_t i = 0; i < g_case_count; i++) {
        bench_entry_t *e = &g_cases[i];
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", e->bc.group, e->bc.name);
        if (filter && !strstr(full, filter)) continue;
        /* A comparison arm exists so that you can read it beside its own case
         * in the same run. The driver never gates on it, so it needs no
         * calibration. */
        if (e->bc.vs) continue;
        bench_result_t res = {0};
        if (!run_case(&e->bc, budget, reps, samples, &res)) {
          e->skipped = true;
          continue;
        }
        meds[i * calibrate + pass] = res.median;
        e->result = res;
        e->ran = true;
      }
    }

    printf("\n%-42s %12s %12s %10s\n", "case", "ns/op", "run-to-run", "gate");
    printf(
        "---------------------------------------------------------------"
        "--------------\n");
    size_t ungateable = 0;
    for (size_t i = 0; i < g_case_count; i++) {
      bench_entry_t *e = &g_cases[i];
      if (!e->ran) continue;
      double lo = 0.0, hi = 0.0;
      bool complete = true;
      for (size_t k = 0; k < calibrate; k++) {
        double v = meds[i * calibrate + k];
        if (v <= 0.0) {
          complete = false;
          break;
        }
        if (lo == 0.0 || v < lo) lo = v;
        if (v > hi) hi = v;
      }
      if (!complete) continue;
      double across = (hi - lo) / lo * 100.0;
      /* The floor for this limit is how far the samples of this case scatter
       * inside one pass. Passes that agree with each other do not make a case
       * steady. A case whose samples span 80 percent inside one pass can land
       * anywhere in that span on the next run. A gate at the two percent by
       * which its passes differed is how a gate fails on a tree that nobody
       * changed. The larger of the two numbers is the real answer. */
      e->gate_pct =
          across > e->result.spread_pct ? across : e->result.spread_pct;
      e->have_gate = true;
      /* The driver records the middle pass and not the last one. The baseline
       * therefore does not hold the pass for which the machine was hottest. */
      double *row = &meds[i * calibrate];
      qsort(row, calibrate, sizeof(double), cmp_double);
      e->result.median = row[calibrate / 2];

      char gate[32];
      if (e->gate_pct > BENCH_UNGATEABLE_PCT) {
        snprintf(gate, sizeof(gate), "%s", "not gated");
        ungateable++;
      } else {
        double lim = e->gate_pct * BENCH_GATE_SAFETY;
        snprintf(gate, sizeof(gate), "%.1f%%",
                 lim > threshold ? lim : threshold);
      }
      printf("  %-40s %12.2f %11.1f%% %10s\n", e->bc.name, e->result.median,
             e->gate_pct, gate);
    }
    free(meds);
    printf(
        "\nbench: calibrated over %zu passes; %zu case(s) vary by more than\n"
        "       %.0f%% between runs of this unchanged library and are "
        "reported\n"
        "       but never gated. That is a property of this machine, not of\n"
        "       the library: widening one shared threshold to cover them is\n"
        "       what makes a gate stop catching anything.\n",
        calibrate, ungateable, BENCH_UNGATEABLE_PCT);
  }

  if (!calibrate) {
    printf("%-42s %12s %13s %8s %6s %12s\n", "case", "ns/op", "rate", "spread",
           "n", "vs baseline");
    printf(
        "---------------------------------------------------------------"
        "--------------------------------\n");
  }

  size_t regressions = 0, compared = 0, skipped = 0, ungated = 0;
  const char *current_group = NULL;

  for (size_t i = 0; !calibrate && i < g_case_count; i++) {
    bench_entry_t *e = &g_cases[i];
    char full[256];
    snprintf(full, sizeof(full), "%s/%s", e->bc.group, e->bc.name);
    if (filter && !strstr(full, filter)) continue;

    if (!current_group || strcmp(current_group, e->bc.group) != 0) {
      current_group = e->bc.group;
      printf("\n%s\n", current_group);
    }

    bench_result_t res = {0};
    bool ok = run_case(&e->bc, budget, reps, samples, &res);
    char label[256];
    if (e->bc.vs)
      snprintf(label, sizeof(label), "  %s [%s]", e->bc.name, e->bc.vs);
    else
      snprintf(label, sizeof(label), "  %s", e->bc.name);

    if (!ok) {
      printf("%-42s %12s %13s %8s %6s %12s\n", label, "skipped", "", "", "",
             "");
      e->skipped = true;
      skipped++;
      continue;
    }
    e->result = res;
    e->ran = true;

    char rate[32], spread[16], nbuf[24];
    format_rate(res.median, rate, sizeof(rate));
    snprintf(spread, sizeof(spread), "%.1f%%", res.spread_pct);
    snprintf(nbuf, sizeof(nbuf), "%zu", res.samples);

    char delta[32] = "";
    if (baseline && !e->bc.vs) {
      cjson prev = cjson_dictionary_get(baseline, full);
      double old = 0.0, old_spread = 0.0, old_gate = 0.0;
      bool have_old_gate = false;
      if (prev && cjson_type(prev) == CJSON_DICTIONARY) {
        old = bench_number(cjson_dictionary_get(prev, "ns"));
        old_spread = bench_number(cjson_dictionary_get(prev, "spread_pct"));
        cjson g = cjson_dictionary_get(prev, "gate_pct");
        have_old_gate = (g != NULL);
        old_gate = bench_number(g);
      } else {
        /* A baseline entry can be a plain number and not a dictionary. */
        old = bench_number(prev);
      }
      if (old > 0.0) {
        double pct = (res.median - old) / old * 100.0;
        compared++;
        double limit;
        bool gateable = true;
        if (have_old_gate) {
          /* The recorded variation between runs, for this case, sets its
           * limit. That is the only quantity that answers the question a gate
           * asks. The question is whether this difference is larger than the
           * difference that two runs of the same library give by
           * themselves. */
          limit = old_gate * BENCH_GATE_SAFETY;
          if (limit < threshold) limit = threshold;
          gateable = old_gate <= BENCH_UNGATEABLE_PCT;
        } else {
          /* Somebody recorded this baseline without a calibration. A spread
           * inside one run bounds the sampling error. It says nothing about
           * how much a figure moves between two runs. This fallback is
           * therefore the looser of the two rules, on purpose. */
          double noise =
              res.spread_pct > old_spread ? res.spread_pct : old_spread;
          limit = noise > BENCH_DEFAULT_THRESHOLD_PCT
                      ? noise
                      : BENCH_DEFAULT_THRESHOLD_PCT;
        }
        bool bad = gateable && pct > limit;
        snprintf(delta, sizeof(delta), "%+.1f%%%s", pct,
                 bad ? " !" : (gateable ? "" : " ~"));
        if (bad) regressions++;
        if (!gateable) ungated++;
      }
    }
    printf("%-42s %12.2f %13s %8s %6s %12s\n", label, res.median, rate, spread,
           nbuf, delta);
    fflush(stdout);
  }

  printf("\n");
  free(samples);

  if (update) {
    /* This reads what the file already holds, so that a case which this run did
     * not measure keeps its figure. The driver writes the whole file again. A
     * run that --filter narrowed, or a run where the setup of a case failed,
     * would otherwise erase every case that it did not touch. The driver does
     * not load the file earlier, because an update run compares against
     * nothing, on purpose. */
    cjson previous = NULL;
    char *prev_text = read_whole_file(baseline_path);
    if (prev_text) {
      char *prev_err = NULL;
      previous = cjson_parse(prev_text, &prev_err);
      free(prev_text);
      cjson_serialize_free(prev_err);
    }
    int write_rc = write_baseline(baseline_path, previous);
    if (previous) cjson_destroy(previous);
    if (write_rc == 0)
      printf("bench: baseline written to %s\n", baseline_path);
    else {
      fprintf(stderr, "bench: failed to write %s\n", baseline_path);
      if (baseline) cjson_destroy(baseline);
      return 2;
    }
  } else if (!baseline) {
    printf(
        "bench: no baseline at %s. Run `make bench_update` on this machine to\n"
        "       record one, then `make bench` reports every later run against "
        "it.\n",
        baseline_path);
  } else {
    printf(
        "bench: %zu case(s) compared against %s, %zu regression(s) past each\n"
        "       case's own gate\n",
        compared, baseline_path, regressions);
    if (ungated)
      printf(
          "bench: %zu case(s) marked ~ are reported but not gated: their\n"
          "       median moves by more than %.0f%% between runs of an\n"
          "       unchanged library on this machine, so no threshold can\n"
          "       separate a regression from the machine itself. Read the\n"
          "       number; do not rely on it failing.\n",
          ungated, BENCH_UNGATEABLE_PCT);
    if (compared && !ungated && regressions == 0)
      printf(
          "bench: run `make bench_calibrate` if this baseline predates\n"
          "       calibration; without it every case falls back to one flat\n"
          "       %.0f%% threshold.\n",
          BENCH_DEFAULT_THRESHOLD_PCT);
  }
  if (skipped)
    printf(
        "bench: %zu case(s) skipped: the fixture could not be built, or a\n"
        "       threaded case could not start every worker. A comparison\n"
        "       against a library that is not installed is compiled out\n"
        "       entirely and never appears here.\n",
        skipped);

  if (regressions && !gate)
    printf(
        "bench: reporting only; --gate makes a flagged case a non-zero exit.\n"
        "       Read the cases marked ! against their own calibrated limits\n"
        "       before treating any of them as real.\n");

  /* The driver reports this after the run and not in the banner, because a pin
   * is
     attempted by every worker as it starts and can fail long after the header
     claiming the run is pinned has been printed. */
  if (atomic_load_explicit(&g_pin_failed, memory_order_relaxed))
    printf(
        "bench: at least one thread could not be pinned, so some figures\n"
        "       above were measured unpinned despite the header. Treat this\n"
        "       run as unpinned rather than comparing it against a pinned\n"
        "       one.\n");

  if (baseline) cjson_destroy(baseline);
  return (regressions && gate) ? 1 : 0;
}
