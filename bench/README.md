# Benchmarks

`bench/` measures the modules whose run-time cost is worth watching. These are
the containers, the string, the sort, the memory pools, the LRU cache, the
thread pool, the queues, the logger, both serializers, and one HTTP round trip.

The harness links against the `libccollections.so` that the root `make` built.
It does not compile the sources of the library into its own binary. It
therefore measures the code that ships, at the flags that it ships with,
through the same dynamic call that an application makes.

--------------------------------------------------------------------------

## Quick start

```bash
make                 # build the library first; the harness links against it
make bench_update    # record a baseline on the current machine
make bench           # run everything and report against that baseline
make bench_list      # list the cases without running them
```

`make bench` only reports. It does not stop the build. See "Gating" below.

`BENCH_ARGS` sends options through to the runner:

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

The harness reports the median and not the mean, because the distribution has
one long side. The scheduler can stop a repetition for any length of time, and
nothing can make one faster than the work it does. Setup and teardown run for
every repetition, and the harness does not time them. A case that fills a
container therefore measures the same work each time, and not a larger amount
each time.

A row in brackets is a comparison arm. The harness measures it in the same run
as the case above it. The arms are `[malloc]` for the pools, `[GHashTable]`,
`[GTree]` and `[uthash]` for the maps, `[GAsyncQueue]` and `[GThreadPool]` for
the concurrency cases, and `[jansson]` and `[libyaml]` for the serializers.
**These are the numbers here that travel best between machines.** A ratio
between two rows of one run removes the clock, the cache hierarchy and the
thermal state that both rows share. A figure in nanoseconds keeps all three.

--------------------------------------------------------------------------

## Reproducibility

Three things decide whether you can compare two runs at all.

**The harness pins the workers, one logical CPU for each physical core.** The
two simultaneous threads of one core are not two cores. They share the
execution resources of that core. Two workers on one core therefore run at
about half speed, and the same two workers on two cores do not. The scheduler
chooses again on every run, so you cannot know which one you get. Where the
machine has cores of more than one kind, the harness uses only the performance
cores. A case ends when its slowest worker ends, so one efficiency core decides
the whole figure. The harness builds the CPU list at run time from
`/sys/devices/cpu_core/cpus` and from the sibling list of each core. It
therefore needs no configuration, and it assumes nothing about how the machine
numbers its CPUs. `--no-pin` turns the pinning off.

One result of this is worth knowing. A case that asks for more workers than the
machine has performance cores must put two workers on some of them. The
assignment is fixed, so it repeats. But such a case is always noisier than a
case that fits.

Threads that a case's fixture starts are not pinned. The workers of a thread
pool and the writer thread of an asynchronous logger are such threads. The
harness builds each fixture while its own thread may run on every CPU in the
list, so those threads can run on any of the performance cores, and it pins
its own thread again afterwards. A thread inherits the CPU affinity of the
thread that creates it, so a fixture built on a pinned thread would put all of
its threads on that one CPU. A four-worker pool would then measure how the
scheduler interleaves four threads on one core, not four workers.

**A run heats the hardware that it measures.** A portable machine reaches its
thermal ceiling about a minute into a run. The same case can cost much more at
that ceiling than it does when the machine is cold. A run therefore measures
its first group in a state that its last group is never in. `--warmup=SEC`
loads every core before the harness measures anything. A run then starts in the
state that a long run reaches anyway. This option is off by default, because it
adds heat of its own. That helps one full run, and it makes a sequence of short
runs harder to compare, not easier.

**The baseline belongs to one machine, and the project does not commit it.** An
absolute time describes the cache hierarchy, the clock behavior and the
background load of one machine. A run against a baseline from other hardware
therefore reports a difference that has nothing to do with the library. Record
your own baseline with `make bench_update` or `make bench_calibrate`.

Do this for a run that you can trust. Close the other programs. Leave the
machine alone for the length of the run. Put a machine that runs on a battery
on mains power. Then read the ratios and not the nanoseconds.

--------------------------------------------------------------------------

## Knowing how much a difference is worth

A percentage means nothing until you know how far the same number moves on its
own. `make bench_calibrate` measures that:

```bash
make bench_calibrate                       # runs the suite several times over
make bench_calibrate BENCH_ARGS="--calibrate=7"
```

It runs the whole suite K times. The default for K is 5. For each case on its
own, it records how far the figure of that case moved between those runs, while
nothing in the library changed. It runs whole passes and does not repeat one
case several times together. Part of what separates two runs is where a case
sits inside the run, and a repeat of one case cannot show that.

```
case                              ns/op   run-to-run       gate
  alloc_free_64b                   6.60         4.1%       6.2%
  alloc_free_64b_12t               2.53        42.4%  not gated
```

`run-to-run` is that measured movement. Compare your own result against it. A
case that moves 42 percent by itself tells you that a difference of 30 percent
in your own measurement means nothing. The harness reports a case above 30
percent, but it never flags one. On such a case the hardware contributes more
than the library does.

Expect a single-threaded case to be much steadier than a case with many
threads. The first benchmarking machine has six performance cores. There, a
single-threaded case repeats within a few percent, and a twelve-thread case
moves by tens of percent. This is why one shared threshold cannot serve both.

--------------------------------------------------------------------------

## Gating

`--gate` makes a flagged case a non-zero exit:

```bash
make bench_gate
```

**This is not for CI, and the CI of this project does not use it.** The times
on a shared, virtual runner move more than most regressions that are worth
catching. A threshold that is tight enough to be useful therefore fails
constantly, and one that is loose enough to be steady catches nothing. CI here
builds the benchmarks and lists their cases. That catches the ways in which a
benchmark decays, such as a case that no longer compiles or a fixture that no
longer builds. It does not pretend to measure anything.

Use `--gate` only on a machine where `make bench_calibrate` has shown that the
figures repeat well enough. Read the run-to-run column before you trust it.

--------------------------------------------------------------------------

## Optional comparison libraries

The build detects each of these libraries, and each one adds its own comparison
arms. You need none of them. A library that is missing removes its own cases
and changes nothing else.

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

A row in brackets is another library that does the nearest equivalent thing. It
does not do the identical thing. Where the two sides really differ, this list
names the difference. It does not correct the difference, because a correction
would measure something that neither library does. The differences do not all
go the same way. The first two below cost this library time that it would not
otherwise spend. The last two save it time that the other side spends.

- `GHashTable` and `GTree` keep the keys and the values of the benchmark AS
  POINTERS, and the caller keeps ownership of them. This library copies both
  into its own storage. Those rows therefore charge this library for a copy
  that the other side does not make. Node allocation is NOT part of that
  difference on the tree rows. Measured with a counting allocator over 100000
  inserts, `GTree` allocates one node for each distinct key, and `cbstmap`
  allocates the same number. Those two therefore differ by the copy alone.
  `GHashTable` is the one that allocates almost nothing for each entry.
- The uthash rows allocate their entry nodes one time, in the setup, which the
  harness does not time. On the rows with an integer key that costs neither
  side anything for each entry. Over the same 100000 inserts, `chashmap` makes
  28 allocations and uthash makes 13. Both sets come from table growth inside
  the timed loop, and not from work for each entry. On the rows with a string
  key the difference is real. A string key selects the separate chaining
  backend, which allocates one chain node for each insert inside the clock,
  measured at 1.00 for each insert. Those rows therefore charge this library
  for an allocation that uthash does not pay.
- `GAsyncQueue` is a linked queue. It allocates one node for each push,
  measured at one allocation and one free for each round trip. The harness
  reports it against `circular_queue_roundtrip`, which is a preallocated ring
  that allocates nothing at all.
- The `[malloc]` rows for `cmempool` get their memory inside the timed loop.
  The pool allocates its backing block in the setup. The pages of the pool
  therefore fault in outside the clock, and the pages of malloc fault in inside
  it.

Two comparisons are like for like: `cjson` against jansson, and `cyaml` against
libyaml. Both sides parse the same bytes into a DOM of their own, and both own
what they build. The jansson serialize row also sets the real number precision
of jansson to the same 15 significant digits that `cjson` uses. The default for
jansson is 17 digits, which makes it write about 8 percent more bytes for the
same document, and the cost of a serializer follows how much it writes.

## Adding a case

The cases are in `bench_core.c`, `bench_maps.c`, `bench_concurrency.c`,
`bench_cache.c`, `bench_logging.c`, `bench_serialization.c` and `bench_http.c`.
Each case registers itself through `bench_add`. `bench_add_mt` registers the
same case at several thread counts together. `bench.h` holds the case structure
and describes each field.

A case owns its fixture. `setup` builds the fixture and `teardown` frees it.
The harness times neither. Keep the timed body to one kind of work. A figure
that moves then tells you what moved.
