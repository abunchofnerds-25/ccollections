# Benchmarks

`bench/` measures the modules with a run-time cost worth tracking: the
containers, the string, the sort, the memory pools, the LRU cache, the thread
pool and queues, the logger, both serializers, and an HTTP round trip.

It links against the `libccollections.so` that the root `make` produced rather
than compiling the library's sources into its own binary, so what it measures
is the code that ships, at the flags it ships with, reached through the same
dynamic call an application makes.

--------------------------------------------------------------------------

## Quick start

```bash
make                 # build the library first; the harness links against it
make bench_update    # record a baseline on the current machine
make bench           # run everything and report against that baseline
make bench_list      # list the cases without running them
```

`make bench` reports. It does not fail the build; see "Gating" below.

`BENCH_ARGS` passes options through to the runner:

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

The median rather than the mean, because the distribution is one sided: a
repetition can be arbitrarily slowed by a scheduler preemption, and nothing
makes one arbitrarily fast. Setup and teardown run for every repetition and are
not timed, so a case that fills a container measures the same work each time
rather than an ever larger one.

A row in brackets is a comparison arm measured in the same run as the case
above it: `[malloc]` for the pools, `[GHashTable]`, `[GTree]` or `[uthash]` for the
maps, `[GAsyncQueue]` and `[GThreadPool]` for the concurrency cases, and
`[jansson]` or `[libyaml]` for the serializers. **These are the most portable
numbers here.** A ratio between two rows of one run cancels the clock, the
cache hierarchy and the thermal state they were both measured under, so it
travels between machines in a way that a nanosecond figure does not.

--------------------------------------------------------------------------

## Reproducibility

Three things decide whether two runs can be compared at all.

**Workers are pinned, one logical CPU per physical core.** A core's two
simultaneous threads are not two cores: they share execution resources, so a
pair of workers landing on one core runs at roughly half speed while the same
pair on two cores does not, and which happens is the scheduler's choice afresh
every run. Where the current machine has cores of more than one kind, only the
performance cores are used, because a case ends when its slowest worker does
and an efficiency core sets the whole figure. The CPU list is derived at run
time from `/sys/devices/cpu_core/cpus` and each core's sibling list, so it
needs no configuration and makes no assumption about CPU numbering. `--no-pin`
turns it off.

A consequence worth knowing: a case asking for more workers than the current machine
has performance cores has to double up on some of them. The assignment is
fixed, so it repeats, but such a case is inherently noisier than one that fits.

**A run heats the hardware it measures.** A portable machine reaches its
thermal ceiling roughly a minute into a run, and the same case can cost
noticeably more at that ceiling than it does cold. A run therefore measures its
first group in a state its last group is never in. `--warmup=SEC` loads every
core before measuring so a run starts in the state a long run settles into
anyway; it is off by default
because it adds heat of its own, which helps one full run and makes a sequence
of short ones less comparable rather than more.

**The baseline is per machine and is not committed.** An absolute timing
describes one particular machine's cache hierarchy, clock behavior and background load, so
comparing a run against a baseline recorded on different hardware reports a
difference that has nothing to do with the library. Record your own with
`make bench_update` or `make bench_calibrate`.

For a run worth trusting: close what else is running, leave the current machine
alone for its duration, and run a battery-powered one on mains power. Then read
the ratios in preference to the nanoseconds.

--------------------------------------------------------------------------

## Knowing how much a difference is worth

A percentage is meaningless without knowing how far the same number moves on
its own. `make bench_calibrate` measures that:

```bash
make bench_calibrate                       # runs the suite several times over
make bench_calibrate BENCH_ARGS="--calibrate=7"
```

It runs the whole suite K times (default 5) and records, for each case
separately, how far that case's own figure moved between those runs with
nothing about the library changing. Whole passes rather than repeats of one
case back to back, because part of what separates two runs is where in the run
a case sits, which a back-to-back repeat cannot see.

```
case                              ns/op   run-to-run       gate
  alloc_free_64b                   6.60         4.1%       6.2%
  alloc_free_64b_12t               2.53        42.4%  not gated
```

`run-to-run` is that measured movement, and it is the number to compare a
result against: a case that moves 42 percent by itself has told you that a 30
percent difference in your own measurement means nothing. A case above 30
percent is reported but never flagged, because on it the hardware contributes
more than the library does.

Expect the single-threaded cases to be much steadier than the wide ones. On the original
benchmarking machine, which has six performance cores, the single-threaded cases
repeat within a few percent while the twelve-thread cases move by tens of
percent, which is why one shared threshold cannot serve both.

--------------------------------------------------------------------------

## Gating

`--gate` makes a flagged case a non-zero exit:

```bash
make bench_gate
```

**This is not for CI, and this project's own CI does not use it.** A shared,
virtualized runner's timing variance is wider than most regressions worth
catching, so a threshold tight enough to be useful fails constantly and one
loose enough to be stable catches nothing. CI here builds the benchmarks and
lists their cases, which catches the ways a benchmark can rot (a case that no
longer compiles, a fixture that no longer builds) without pretending to
measure anything.

Use `--gate` only on a machine that `make bench_calibrate` has shown repeats
well enough to carry it, and read the run-to-run column before trusting it.

--------------------------------------------------------------------------

## Optional comparison libraries

Each is detected at build time and contributes its own comparison arms. None is
required; a missing one removes its cases and nothing else.

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

A bracketed row is another library doing the nearest equivalent thing, not the
identical thing. Where the two sides genuinely differ, the difference is listed
here rather than corrected, because correcting it would mean measuring
something neither library actually does. They do not all lean the same way: the
first two below cost this library time it would not otherwise spend, and the
last two spare it time the other side spends.

- `GHashTable` and `GTree` store the benchmark's keys and values AS POINTERS
  and leave ownership to the caller, where this library copies both into its
  own storage, so those rows charge this library a copy the other side does not
  make. Node allocation is NOT part of that difference on the tree rows:
  measured with a counting allocator over 100000 inserts, `GTree` allocates one
  node per distinct key and `cbstmap` allocates exactly the same number, so
  those two differ only by the copy. `GHashTable` is the one that allocates
  almost nothing per entry.
- The uthash rows allocate their entry nodes once in untimed setup. On the
  integer-keyed rows that costs neither side anything per entry: over the same
  100000 inserts `chashmap` makes 28 allocations and uthash 13, both of them
  table growth inside the timed loop rather than per-entry work. On the
  string-keyed rows it is a real difference: a string key selects the
  separate-chaining backend, which allocates a chain node per insert inside the
  clock, measured at 1.00 per insert, so those rows charge this library an
  allocation uthash is not charged.
- `GAsyncQueue` is a linked queue and allocates a node per push, measured at
  one allocation and one free per round trip. It is reported against
  `circular_queue_roundtrip`, a preallocated ring that allocates nothing at
  all.
- The `[malloc]` rows for `cmempool` obtain their memory inside the timed loop,
  where the pool's backing block is allocated in setup, so the pool's pages are
  faulted in untimed and malloc's are not.

The rows that are like for like are `cjson` against jansson and `cyaml` against
libyaml: both sides parse the same bytes into a DOM of their own, and both own
what they build. The jansson serialize row additionally pins jansson's real
number precision to the same 15 significant digits `cjson` uses, since its
default of 17 makes it emit about 8 percent more bytes for the same document
and a serializer's cost tracks how much it writes.

## Adding a case

Cases live in `bench_core.c`, `bench_maps.c`, `bench_concurrency.c`,
`bench_cache.c`, `bench_logging.c`, `bench_serialization.c` and
`bench_http.c`, and register themselves through `bench_add`. `bench_add_mt`
registers the same case at several thread counts at once. `bench.h` carries the
case structure and what each field means.

A case owns its fixture: `setup` builds it, `teardown` releases it, and neither
is timed. Keep the timed body doing one kind of work, so that a figure that
moves names something.
