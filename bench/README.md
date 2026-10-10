# Benchmarks

`bench/` measures the modules whose run-time cost is worth watching: the
containers, the string, the sort, the memory pools, the LRU cache, the
thread
pool, the queues, the logger, both serializers, and one HTTP round trip.

The harness links against the shared library that the root `make` built
(`libccollections.so`, or `libccollections.dylib` on macOS) instead of
compiling the library sources into its own binary. That way it measures the
code that actually ships, built with the flags it ships with, and every call
goes through the same dynamic linkage that an application uses.

--------------------------------------------------------------------------

## Quick start

```bash
make                 # build the library first; the harness links against it
make bench_update    # record a baseline on the current machine
make bench           # run everything and report against that baseline
make bench_list      # list the cases without running them
```

`make bench` only reports; it never fails the build (see "Gating" below).

Use `BENCH_ARGS` to pass options through to the runner:

```bash
make bench BENCH_ARGS="--filter=cmempool"
make bench BENCH_ARGS="--filter=chashmap --reps=15"
make bench BENCH_ARGS="--budget=10"
./bench/bench --help
```

--------------------------------------------------------------------------

## Reading the output

```
case                              ns/op          rate   spread      n  vs baseline
  alloc_free_64b                   6.60    151.45 M/s     8.5%   2000        +1.2%
  alloc_free_64b [malloc]          7.64    130.97 M/s     8.5%   2000
```

| column | meaning |
|---|---|
| `ns/op` | nanoseconds per operation, the median across repetitions |
| `rate` | the same figure as operations per second |
| `spread` | the p90-to-p10 range as a percentage of the median |
| `n` | how many repetitions were taken |
| `vs baseline` | change against the recorded baseline for this case |

The harness reports the median rather than the mean because the distribution
is lopsided: the scheduler can stall a repetition for any length of time,
but
nothing can make a repetition faster than the work it actually does. Setup
and
teardown run around every repetition without being timed, so a case that
fills a container measures the same amount of work each time instead of a
growing amount.

A row in brackets is a comparison arm, measured in the same run as the case
above it. The arms are `[malloc]` for the pools, `[GHashTable]`, `[GTree]`
and
`[uthash]` for the maps, `[GAsyncQueue]` and `[GThreadPool]` for the
concurrency cases, and `[jansson]` and `[libyaml]` for the serializers.
**These ratios are the numbers that travel best between machines.** Dividing
two rows of the same run cancels out the clock speed, the cache hierarchy
and
the thermal state that both rows share, whereas a figure in nanoseconds
keeps
all three.

--------------------------------------------------------------------------

## Reproducibility

Three things decide whether two runs can be compared at all.

**The harness pins its workers to one logical CPU per physical core.** The
two
hardware threads of a core are not two cores: they share that core's
execution resources, so two workers on one core run at roughly half speed,
while the same two workers on separate cores do not. Left alone, the
scheduler
decides afresh on every run, and you cannot tell which case you got. On a
machine with more than one kind of core, the harness uses only the
performance cores, because a case ends when its slowest worker ends, and a
single efficiency core would decide the whole figure. The harness builds its
CPU list at run time from `/sys/devices/cpu_core/cpus` and the sibling list
of
each core, so it needs no configuration and makes no assumption about how
the
machine numbers its CPUs. `--no-pin` turns pinning off. macOS offers no way
to
bind a thread to a CPU, so there the harness always runs unpinned and says
so
in its header; expect a larger spread between runs on macOS.

One consequence is worth knowing: a case that asks for more workers than the
machine has performance cores has to double some of them up. The assignment
is fixed, so it repeats from run to run, but such a case is always noisier
than one that fits.

Threads that a case's fixture starts itself, such as the workers of a thread
pool or the writer thread of an asynchronous logger, are not pinned. A
thread
inherits the CPU affinity of the thread that creates it, so if a fixture
were
built on a pinned thread, all of its threads would land on that one CPU, and
a four-worker pool would measure how the scheduler interleaves four threads
on
one core rather than four workers. The harness therefore lets its own thread
run on every CPU in the list while it builds a fixture, so the fixture's
threads can use any performance core, and pins its own thread again
afterwards.

**A run heats up the hardware it is measuring.** A laptop typically reaches
its thermal ceiling about a minute into a run, and the same case can cost
much more at that ceiling than on a cold machine, so the first cases of
a run are measured in a state that the last ones never see. `--warmup=SEC`
loads every core before measuring anything, so that a run starts in the
state
a long run reaches anyway. It is off by default because it adds heat of its
own: that helps a single full run, but it makes a series of short runs
harder
to compare, not easier.

**A baseline belongs to one machine, and the project does not commit one.**
An
absolute time reflects the cache hierarchy, the clock behavior and the
background load of a particular machine, so comparing against a baseline
from
other hardware reports differences that have nothing to do with the library.
Record your own with `make bench_update` or `make bench_calibrate`.

For a run you can trust, close other programs, leave the machine alone while
it runs, and plug a laptop into mains power. Then compare ratios, not
nanoseconds.

--------------------------------------------------------------------------

## Knowing how much a difference is worth

A percentage means nothing until you know how far the same number moves on
its
own. `make bench_calibrate` measures exactly that:

```bash
make bench_calibrate                       # runs the suite several times over
make bench_calibrate BENCH_ARGS="--calibrate=7"
```

It runs the whole suite K times (5 by default) and records, for each case,
how
far its figure moved between those runs while nothing in the library
changed.
It repeats whole passes rather than running one case several times in a row,
because part of what separates two runs is where a case sits within the run,
and back-to-back repeats of a single case cannot show that.

```
case                              ns/op   run-to-run       gate
  alloc_free_64b                   6.60         4.1%       6.2%
  alloc_free_64b_12t               2.53        42.4%  not gated
```

`run-to-run` is that measured movement; compare your own results against it.
If a case moves 42 percent on its own, a 30 percent difference in your
measurement tells you nothing. The harness reports a case whose run-to-run
figure is above 30 percent but never flags it, because for such a case the
hardware contributes more to the figure than the library does.

Expect single-threaded cases to be far steadier than heavily threaded ones.
On
the first benchmarking machine, which has six performance cores, a
single-threaded case repeats within a few percent while a twelve-thread case
moves by tens of percent, which is why one shared threshold cannot serve
both.

--------------------------------------------------------------------------

## Gating

`--gate` turns a flagged case into a non-zero exit status:

```bash
make bench_gate
```

**This is not meant for CI, and this project's CI does not use it.** Timings
on a shared virtual runner move more than most regressions worth catching,
so
a threshold tight enough to be useful fails constantly, and one loose enough
to stay quiet catches nothing. Instead, CI builds the benchmarks and lists
their cases. That catches the ways a benchmark suite rots, such as a case
that
no longer compiles or a fixture that no longer builds, without pretending to
measure anything.

Only use `--gate` on a machine where `make bench_calibrate` has shown that
the
figures repeat well enough, and read the run-to-run column before you trust
it.

--------------------------------------------------------------------------

## Optional comparison libraries

The build detects each of these libraries, and each one it finds adds its
own
comparison arms. None of them is required: a missing library simply removes
its own cases and changes nothing else.

| library | Debian or Ubuntu package | what it compares against |
|---|---|---|
| uthash | `uthash-dev` | `chashmap` |
| GLib | `libglib2.0-dev` | `chashmap`, `cbstmap`, `cthreadcomm`, `cthreadpool` |
| Jansson | `libjansson-dev` | `cjson` |
| libyaml | `libyaml-dev` | `cyaml` |

```bash
make -C bench config     # shows which were found
```

--------------------------------------------------------------------------

## What the comparison rows do and do not compare

A row in brackets runs another library doing the nearest equivalent thing,
not the identical thing. Where the two sides genuinely differ,
the list below names the difference rather than correcting for it, since a
correction would measure something that neither library actually does. The
differences cut both ways: the first two cost this library time it would not
otherwise spend, and the last two save it time that the other side spends.

- `GHashTable` and `GTree` store the benchmark's keys and values AS
  POINTERS,
  and the caller keeps ownership of them, whereas this library copies both
  into its own storage. Those rows therefore charge this library for a copy
  the other side never makes. Node allocation is NOT part of that difference
  on the tree rows: measured with a counting allocator over 100000 inserts,
  `GTree` allocates one node per distinct key and `cbstmap` allocates the
  same
  number, so the two differ by the copy alone. `GHashTable` is the one that
  allocates almost nothing per entry.
- The uthash rows allocate their entry nodes once, during the untimed setup.
  With an integer key this costs neither side anything per entry: over the
  same 100000 inserts, `chashmap` makes 28 allocations and uthash makes 13,
  all of them from table growth inside the timed loop rather than per-entry
  work. With a string key the difference is real. A string key selects the
  separate chaining backend, which allocates one chain node per insert
  inside
  the clock (measured at 1.00 per insert), so those rows charge this
  library for an allocation that uthash does not pay.
- `GAsyncQueue` is a linked queue that allocates one node per push, measured
  at one allocation and one free per round trip. It is reported against
  `circular_queue_roundtrip`, a preallocated ring that allocates nothing at
  all.
- The `[malloc]` rows for `cmempool` obtain their memory inside the timed
  loop, while the pool allocates its backing block during setup. The pool's
  pages therefore fault in outside the clock, and malloc's inside it.

Two comparisons are like for like: `cjson` against jansson, and `cyaml`
against libyaml. In both, each side parses the same bytes into its own DOM
and
owns what it builds. The jansson serialize row also sets jansson's real
number
precision to the same 15 significant digits that `cjson` uses. Jansson
defaults to 17 digits, which makes it write about 8 percent more bytes for
the
same document, and a serializer's cost follows how much it writes.

## Adding a case

The cases live in `bench_core.c`, `bench_maps.c`, `bench_concurrency.c`,
`bench_cache.c`, `bench_logging.c`, `bench_serialization.c` and
`bench_http.c`.
Each case registers itself through `bench_add`, and `bench_add_mt` registers
the same case at several thread counts at once. `bench.h` defines the case
structure and documents each field.

A case owns its fixture: `setup` builds it and `teardown` frees it, and the
harness times neither. Keep the timed body to a single kind of work, so that
when a figure moves, you know what moved.
