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
 * The baseline is a JSON file read and written with this library's own cjson,
 * and it is deliberately local rather than committed. An absolute
 * nanoseconds-per-operation figure describes one machine's cache hierarchy,
 * clock behavior and background load; comparing a run here against a figure
 * recorded on different hardware reports a difference that has nothing to do
 * with any change to the library. What is worth measuring is the same machine
 * before and after a change, which is what `make bench_update` followed by
 * `make bench` gives.
 *
 * Cases that measure a third-party library are reported for comparison but
 * never checked against the baseline: their timings track that project's
 * performance and the version of it installed, neither of which this
 * repository controls.
 */

#include "bench.h"

#include <cjson.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BENCH_MAX_CASES 256
/* Sampling is bounded by wall-clock time rather than a fixed repetition count.
 * A fixed count treats a case costing microseconds and one costing seconds
 * identically, which either under-samples the cheap case into uselessness or
 * makes the expensive one unrunnable. A time budget instead spends the same
 * effort everywhere and lets each case take as many samples as it can afford.
 *
 * It also survives a change in machine speed. The number of samples adapts to
 * whatever the CPU is doing, so a run on a faster clock collects more of them
 * rather than silently measuring something incomparable to the last run.
 *
 * The floor guarantees enough samples for a median and a spread to mean
 * anything, even where that overruns the budget; the ceiling stops a very cheap
 * case spending its whole budget long after the numbers stopped moving. */
#define BENCH_DEFAULT_BUDGET_SEC 5.0
#define BENCH_MIN_SAMPLES 10
#define BENCH_MAX_SAMPLES 2000
/* A regression has to clear this to be reported. Repeated runs of an unchanged
 * library on an otherwise idle machine stay inside a few percent, but a shared
 * or virtualized host is far noisier, and a threshold tight enough to catch
 * every real 5 percent regression reports a dozen false ones per run on such a
 * host, which is how a performance check stops being read. */
#define BENCH_DEFAULT_THRESHOLD_PCT 20.0

/* A case is held to the run-to-run variation it actually exhibits on this
 * machine, recorded by --calibrate, rather than to one constant shared by every
 * case. One constant cannot serve both ends of the range. A laptop with cores
 * of two different speeds sharing one package power budget runs a twelve-thread
 * case whose median moves by more than half between two runs of an unchanged
 * library, while a single-threaded case on the same machine repeats to a few
 * percent; a threshold wide enough to keep the first quiet cannot catch a real
 * regression in the second. Where the measurement is dominated by the machine
 * rather than by the library, the honest answer is to report the case and gate
 * nothing on it, which is what BENCH_UNGATEABLE_PCT below decides. */
/* Every figure here depends on the die temperature at the instant it is taken,
 * and a run heats the machine it is measuring. Measured on a laptop with this
 * suite: the package reaches its ceiling roughly a minute into a run, and one
 * unchanged case costs about 15 percent more at 98C than at 55C, with the
 * multithreaded cases considerably worse because their clock is bounded by a
 * package power budget shared across every active core. A run that starts cold
 * therefore measures its first group in a state its last group can never be in,
 * and two runs are comparable only when both start from the same state.
 *
 * --warmup loads every core first so a run begins in the state a long run
 * settles into anyway. It is off by default because it is not free of its own
 * side effect: it adds heat, so back-to-back short runs each start hotter than
 * the last, and on a machine that sheds heat slowly it makes a sequence of
 * runs less comparable rather than more. It helps a single full run started on
 * a cold machine. Measure whether it helps on the machine in question before
 * turning it on, rather than assuming. */
#define BENCH_DEFAULT_WARMUP_SEC 0.0
#define BENCH_DEFAULT_CALIBRATE_PASSES 5
/* Measured variation is widened by this before it becomes a limit: a handful of
 * passes samples the spread rather than bounding it, so a later run can land
 * outside the range those passes happened to cover without anything having
 * changed. */
#define BENCH_GATE_SAFETY 1.5
/* No case is held tighter than this however quiet its calibration was, so a
 * machine that happened to be undisturbed for the calibration cannot pin a
 * threshold that no later run can meet. */
#define BENCH_GATE_FLOOR_PCT 5.0
/* Above this a case is reported but never gated. A real regression in such a
 * case still shows up as a number that moved, and is still worth reading; it
 * just cannot be an automatic failure without failing on an unchanged tree. */
#define BENCH_UNGATEABLE_PCT 30.0

/* What one case measured. spread_pct is the p90-to-p10 range as a percentage of
 * the median: robust to a single outlier, unlike max-minus-min, and it is what
 * makes a reported difference interpretable. A change smaller than a case's own
 * spread is not a result. */
typedef struct {
  double median;
  double spread_pct;
  size_t samples;
} bench_result_t;

typedef struct {
  bench_case_t bc;
  bench_result_t result;
  bool ran;
  /* Distinguished from !ran: a case a --filter excluded keeps whatever the
     baseline already recorded for it, while one that ran and could not be
     measured has no current figure and must not keep a stale one. */
  bool skipped;
  /* Filled by --calibrate: how far this case's own median moved across whole
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

/* Names for the variants bench_add_mt generates. bench_add copies the case
 * struct but not the strings it points at, so the generated name needs storage
 * that outlives the caller's stack. */
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

/* A stored value whose fractional part happens to be zero serializes without a
 * decimal point and parses back as an integer node, so both number types are
 * accepted. Returns 0 for anything else, including a missing key. */
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
 * Returns a negative value when the case could not run, which happens when its
 * setup reports failure. A benchmark that cannot allocate its fixture is not a
 * measurement of anything, and skipping it leaves every other case's numbers
 * intact where aborting the process would discard them.
 */
/* One worker of a threaded case. */
typedef struct {
  const bench_case_t *bc;
  void *state;
  unsigned index;
  atomic_uint *ready;
  atomic_int *go;
} bench_worker_t;

static __thread unsigned g_thread_index;

unsigned bench_thread_index(void) { return g_thread_index; }

/* One logical CPU per physical core, fastest first, so no measured thread ever
 * shares a core with another. A core's two SMT threads are not two cores: they
 * share execution resources, so a pair of workers landing on one runs at
 * roughly half speed while an identical pair on two cores does not, and which
 * of the two happens is the scheduler's choice afresh every run. Measured on a
 * six-P-core machine, pinning four workers one per physical core took a
 * compute-bound reference from 23 percent variation between runs to 3.6.
 *
 * Derived from sysfs rather than assumed from CPU numbering, which does not
 * follow one convention: siblings are adjacent on some machines (1-2, 3-4) and
 * half a table apart on others, so "use the first half of the CPUs" picks a set
 * of nothing but sibling pairs on the former.
 *
 * Restricted to the performance cores where the machine has two kinds. A
 * heterogeneous machine runs its efficiency cores around a fifth slower, and a
 * case ends when its slowest worker does, so letting one worker land on an
 * efficiency core sets the whole figure by that core while which worker it is
 * changes from run to run. /sys/devices/cpu_core/cpus names them, the same list
 * perf reads to separate the two PMUs; without that file every core is the same
 * kind and all of them are used. Ordered so that the one-per-core entries come
 * first and the second thread of each core only after every core has one, which
 * keeps a case narrower than the core count entirely free of sharing and makes
 * a wider one share deterministically rather than differently each run. */
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

static long read_long_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  long v = -1;
  if (fscanf(f, "%ld", &v) != 1) v = -1;
  fclose(f);
  return v;
}

/* The first entry of thread_siblings_list identifies the core; every sibling
 * of one core reports the same first entry, so keeping only the CPUs that name
 * themselves there keeps exactly one per core. */
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

/* Parses a sysfs cpu list ("0-11", "0,5,8-10") into set. Returns false when the
 * file is absent, which is how a machine with one kind of core reports itself
 * and is not an error. */
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

  /* Two sweeps: every core's first thread, then the remaining threads. A case
   * asking for no more workers than there are cores therefore never shares. */
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

/* slot is the worker's index. Wraps only when a case asks for more threads than
 * the machine has cores, which is the one situation where sharing is the point
 * rather than an accident. */
static void bench_pin(size_t slot) {
  if (!g_pin_enabled || g_cpu_count == 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(g_cpu_order[slot % g_cpu_count].cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
    atomic_store_explicit(&g_pin_failed, true, memory_order_relaxed);
}

static void *bench_worker_main(void *arg) {
  bench_worker_t *w = arg;
  g_thread_index = w->index;
  /* Before the gate, so the clock never covers the move and every sample of
   * this case runs on the same cores as the last one. */
  bench_pin(w->index);
  /* Announce, then wait to be released. The clock starts on the other side of
   * this gate, so it covers the measured work and not the cost of spawning
   * threads: a real caller creates its threads once and runs many operations on
   * them, and charging thread creation to the operation count would tax the
   * wider variants for something they do not do per operation.
   *
   * The wait cannot hang on a thread that was never created, because the
   * releasing side sets the flag for however many threads it actually
   * started. */
  atomic_fetch_add_explicit(w->ready, 1, memory_order_relaxed);
  while (!atomic_load_explicit(w->go, memory_order_acquire)) sched_yield();
  w->bc->run(w->state, w->bc->n);
  return NULL;
}

/* Builds a case's fixture, runs its body once, and tears it down, returning the
 * wall time spent in the body alone. For a threaded case the body runs on
 * bc->threads workers, all released together once every one of them is up, and
 * the elapsed time runs from that release to the last worker finishing.
 *
 * Returns a negative time if the fixture could not be built, which skips the
 * case rather than reporting a measurement of nothing. */
static double bench_one_pass(const bench_case_t *bc) {
  unsigned threads = bc->threads > 1 ? bc->threads : 1;

  if (threads == 1) {
    void *state = bc->setup ? bc->setup(bc->n) : bc->setup_mt(bc->n, 1);
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

  /* A shared fixture is built once and handed to every worker, which is what
   * puts them in contention. A private fixture is built per worker, which is
   * the only valid arrangement for a type with no internal locking. */
  void *shared = NULL;
  if (bc->shared_fixture) {
    shared = bc->setup_mt(bc->n, threads);
    if (!shared) {
      free(th);
      free(w);
      free(owned);
      return -1.0;
    }
  }
  for (unsigned i = 0; i < threads; i++) {
    if (!bc->shared_fixture) {
      owned[i] = bc->setup_mt(bc->n, threads);
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
  /* A case that could not start every worker measured something other than
   * what it claims to, so it is reported as skipped rather than as a fast
   * result. */
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

/* Deliberately compute-bound and library-free: what it has to reproduce is the
 * thermal and clock state a run settles into, not any particular workload. */
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
  /* The calling thread spins as well, so a machine where no worker could be
   * started is still warmed rather than silently left cold. */
  warmup_spin(&deadline);
  for (unsigned i = 0; i < started; i++) pthread_join(th[i], NULL);
  free(th);
}

/* Collects samples until the budget is spent, the ceiling is reached, or (when
 * fixed_reps is non-zero) that many have been taken. Never stops below
 * BENCH_MIN_SAMPLES, even if that overruns the budget: a median of three
 * numbers is not worth reporting. */
static bool run_case(const bench_case_t *bc, double budget_sec,
                     size_t fixed_reps, double *samples, bench_result_t *out) {
  /* One untimed repetition first. It pays the first-touch page faults for the
   * fixture, populates the branch predictors and warms the instruction cache,
   * all of which otherwise land entirely on the first timed repetition and
   * make it the slowest of the set for reasons unrelated to the code. */
  if (bench_one_pass(bc) < 0.0) return false; /* untimed warm-up */

  size_t ceiling = fixed_reps ? fixed_reps : BENCH_MAX_SAMPLES;
  size_t floor_n = fixed_reps ? fixed_reps : BENCH_MIN_SAMPLES;
  double deadline = bench_now_ns() + budget_sec * 1e9;
  size_t count = 0;

  /* Divided by the total operations across all workers, so a threaded figure
   * is directly comparable to the same case run on one thread. */
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
  /* A baseline is a few kilobytes of JSON. Anything past this ceiling is not
   * one, and feeding an unbounded ftell() result straight to an allocator
   * turns a mistyped path into a multi-gigabyte request. */
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

/* Attaches one value to entry, and owns it either way.
 *
 * cjson_dictionary_set transfers ownership of the child unconditionally: on a
 * failed insert it destroys the child itself rather than handing it back, which
 * is what its own documentation promises. So the child is freed here only when
 * it was never handed over, and destroying it after a failed set would be a
 * double free rather than cleanup. */
static bool baseline_put(cjson entry, const char *key, cjson value) {
  if (!value) return false;
  return cjson_dictionary_set(entry, key, value) == ccol_success;
}

/* Builds one baseline entry. Every node is attached as soon as it exists, so an
 * allocation that fails partway leaves nothing unowned: destroying the entry
 * frees what was attached, the failed set has already freed what it refused,
 * and what was never created needs no freeing. */
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
  /* Absent rather than zero when the baseline was recorded without calibrating,
   * so the comparison can tell "measured as quiet" from "never measured" and
   * fall back to the flat floor for the latter instead of gating everything at
   * zero tolerance. */
  if (have_gate &&
      !baseline_put(entry, "gate_pct", cjson_create_double(gate_pct))) {
    cjson_destroy(entry);
    return NULL;
  }
  return entry;
}

/* previous is whatever was loaded from the file, or NULL. A case that did not
 * run in this invocation (one a --filter excluded) keeps the figure already
 * recorded for it rather than disappearing: the file is rewritten whole, so
 * writing only what ran would silently discard every other case's baseline. */
static int write_baseline(const char *path, cjson previous) {
  cjson root = cjson_create_dictionary();
  if (!root) return -1;

  int rc = -1;
  for (size_t i = 0; i < g_case_count; i++) {
    if (g_cases[i].bc.vs) continue;
    char key[256];
    snprintf(key, sizeof(key), "%s/%s", g_cases[i].bc.group,
             g_cases[i].bc.name);
    /* The spread is recorded with the median because a later comparison needs
     * to know what "different" means for this case. A flat percentage applied
     * to every case either drowns the quiet ones in false alarms or lets a real
     * regression hide inside a noisy one. */
    cjson entry;
    if (g_cases[i].ran) {
      entry = baseline_entry(
          g_cases[i].result.median, g_cases[i].result.spread_pct,
          g_cases[i].result.samples, g_cases[i].gate_pct, g_cases[i].have_gate);
    } else if (g_cases[i].skipped) {
      /* Ran and could not be measured. Dropping it is the honest answer: a
         carried-over figure would be compared against on the next run as
         though it described this build. */
      continue;
    } else {
      cjson prev = previous ? cjson_dictionary_get(previous, key) : NULL;
      if (!prev) continue;
      /* A baseline recorded before spreads were kept stores a bare number
         rather than a dictionary, which is what the comparison path below
         also accepts; reading it through cjson_dictionary_get alone would
         rewrite it as zero. */
      cjson prev_ns = cjson_dictionary_get(prev, "ns");
      cjson prev_gate = cjson_dictionary_get(prev, "gate_pct");
      entry = baseline_entry(
          bench_number(prev_ns ? prev_ns : prev),
          bench_number(cjson_dictionary_get(prev, "spread_pct")),
          (size_t)bench_number(cjson_dictionary_get(prev, "samples")),
          bench_number(prev_gate), prev_gate != NULL);
    }
    if (!entry) goto out;
    /* Ownership transfers whether or not the insert succeeds, so a failure
     * here needs no destroy of its own; see baseline_put. */
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
  /* Every write is checked, and fclose last: the payload is buffered, so a
   * full disk or a quota is reported by the flush rather than by the fputs.
   * Reporting success over a truncated baseline would make the next run
   * compare against a file that describes nothing. */
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
  /* Reporting and failing are separate decisions, because whether this machine
   * can support the second is an empirical question about the machine. Measured
   * here over full-suite runs of an unchanged library, with every case held to
   * the variation it exhibited during calibration, about half of all runs still
   * produced at least one flagged case, and half of the suite varied too much
   * to be gated at all. A check that fails that often stops being read, which
   * costs more than the regressions it would have caught. So the numbers are
   * always reported and --gate decides whether a flagged case is also a
   * non-zero exit, for a machine that has been calibrated and shown to hold
   * it. */
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
  /* Bounded before the multiply: --reps comes straight off the command line,
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

  /* The baseline is loaded before the run so that a malformed or missing one
   * is reported up front rather than after several minutes of measurement. */
  cjson baseline = NULL;
  if (!update) {
    char *text = read_whole_file(baseline_path);
    if (text) {
      char *err = NULL;
      baseline = cjson_parse(text, &err);
      free(text);
      if (!baseline) {
        fprintf(stderr, "bench: cannot parse %s (%s); running without it\n",
                baseline_path, err ? err : "unknown error");
        /* The parser's own release entry point, not free(): the string comes
         * from whatever allocator the parse used, which the API documents as
         * the caller's to release through this. */
        cjson_serialize_free(err);
      }
    }
  }

  if (g_pin_enabled && g_cpu_count) {
    /* The single-threaded cases run on this thread, so it is pinned too. */
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
    /* Whole passes over the suite, never repeats of one case back to back.
     * What a gate has to survive is the difference between two independent
     * runs, and a large part of that difference is where in the run a case
     * sits: the machine is cool for the first group and saturated by the last,
     * so a case measured twice in a row reports a steadiness it does not have
     * at the moment the comparison is actually made. */
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
        /* A comparison arm exists to be read next to its own case in the same
         * run; it is never gated, so it needs no calibration. */
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
      /* Floored at how far this case's own samples scatter inside a single
       * pass. Passes that happen to agree with each other do not make a case
       * steady: one whose samples span 80 percent within a pass can land
       * anywhere in that span on the next run, and gating it at the two
       * percent its passes happened to differ by is how a gate fails on an
       * unchanged tree. Whichever of the two is larger is the real answer. */
      e->gate_pct =
          across > e->result.spread_pct ? across : e->result.spread_pct;
      e->have_gate = true;
      /* The figure recorded is the middle pass rather than the last, so the
       * baseline does not encode whichever pass the machine ran hottest for. */
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
        /* A baseline recorded before spreads were kept stores a bare number. */
        old = bench_number(prev);
      }
      if (old > 0.0) {
        double pct = (res.median - old) / old * 100.0;
        compared++;
        double limit;
        bool gateable = true;
        if (have_old_gate) {
          /* The case's own recorded run-to-run variation sets its limit. This
           * is the only quantity that answers the question a gate asks, which
           * is whether this difference is larger than the difference two runs
           * of the same library produce by themselves. */
          limit = old_gate * BENCH_GATE_SAFETY;
          if (limit < threshold) limit = threshold;
          gateable = old_gate <= BENCH_UNGATEABLE_PCT;
        } else {
          /* Recorded without calibrating. The within-run spreads bound
           * sampling error but say nothing about movement between runs, so
           * this fallback is deliberately the looser of the two rules. */
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
    /* Read whatever is already recorded, purely so cases this run did not
     * measure keep their figures. The file is rewritten whole, so a run
     * narrowed by --filter, or one where a case's setup failed, would
     * otherwise erase every case it did not touch. Not loaded earlier because
     * an update run deliberately compares against nothing. */
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

  /* Reported after the run rather than at the banner, because a pin is
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
