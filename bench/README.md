# Benchmarks

`bench/` measures the modules whose run-time cost is worth keeping an eye
on: the containers, the string, the sort, the memory pools, the LRU cache,
the thread pool, the queues, the logger, both serializers and one HTTP
round trip.

The harness links against the shared library that the root `make` built
(`libccollections.so`, or `libccollections.dylib` on macOS) instead of
compiling the library sources into its own binary. So it measures the code
that really ships, built with the flags it ships with, and every call goes
through the same dynamic linkage that an application goes through.

--------------------------------------------------------------------------

## Quick start

```bash
make                 # build the library first; the harness links against it
make bench_update    # record a baseline on the current machine
make bench           # run everything and report against that baseline
make bench_list      # list the cases without running them
```

`make bench` only reports and never fails the build (see "Gating" below).

Options for the runner go through `BENCH_ARGS`:

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

The harness reports the median and not the mean because the distribution
is lopsided. The scheduler can stall a repetition for any length of time,
but nothing can make a repetition faster than the work it really does.
Setup and teardown run around every repetition without being timed, so a
case that fills a container measures the same amount of work every time
instead of a growing amount.

A row in brackets is a comparison arm, measured in the same run as the case
above it. The arms are `[malloc]` for the pools, `[GHashTable]`, `[GTree]`
and `[uthash]` for the maps, `[GAsyncQueue]` and `[GThreadPool]` for the
concurrency cases, and `[jansson]` and `[libyaml]` for the serializers.
These ratios are the numbers that travel best between machines. When you
divide two rows of the same run, the clock speed, the cache hierarchy and
the thermal state that both rows share cancel out, while a figure in
nanoseconds keeps all of them.

--------------------------------------------------------------------------

## Reproducibility

Whether two runs can be compared at all comes down to three things: where
the threads run, how hot the hardware is, and which machine the baseline
came from.

The harness pins its workers to one logical CPU per physical core. The two
hardware threads of a core share that core's execution resources, so two
workers on one core run at about half speed while the same two workers on
separate cores don't, and if you leave it to the scheduler it decides again
on every run and you can't tell which case you got. On a machine with more
than one kind of core the harness only uses the performance cores, because
a case ends when its slowest worker ends and a single efficiency core would
decide the whole figure. The CPU list is built at run time from
`/sys/devices/cpu_core/cpus` and the sibling list of each core, so there is
nothing to configure and no assumption about how the machine numbers its
CPUs. `--no-pin` turns pinning off. macOS and OpenBSD have no way to bind a
thread to a CPU, so the harness always runs unpinned there and says so in
its header, and you should expect a bigger spread between runs.

A case that asks for more workers than the machine has performance cores
has to double some of them up. The assignment is fixed, so it repeats from
run to run, but such a case is always noisier than one that fits.

Threads that a case's fixture starts, like the workers of a
thread pool or the writer thread of an asynchronous logger, aren't pinned.
A thread inherits the CPU affinity of the thread that creates it, so a
fixture built on a pinned thread would put all its threads on that one CPU,
and a four-worker pool would then measure how the scheduler interleaves four
threads on one core instead of four workers. That's why the harness lets
its own thread run on every CPU in the list while it builds a fixture, so
the fixture's threads can use any performance core, and pins its own thread
again afterwards.

A run also heats up the hardware it measures. A laptop usually reaches its
thermal ceiling about a minute into a run, and the same case can cost much
more at that ceiling than on a cold machine, so the first cases of a run
are measured in a state that the last ones never see. `--warmup=SEC` loads
every core before it measures anything, so that a run starts in the state
that a long run reaches anyway. It is off by default because it adds heat of
its own, which helps a single full run but makes a series of short runs
harder to compare.

The time between two samples heats the hardware too. A case whose fixture
starts threads or a reactor for every sample, such as the `http` cases, pays
its teardown between samples, so a library that tears down faster runs its
samples closer together, keeps the CPU hotter and can read slower for work
that didn't change. Such a case sets `rest_after_teardown_ms`, and the
harness then pauses for that fixed time after each teardown, outside the
timed region, so that every build rests the same amount. The `http` cases
rest 1 s, and since the pause counts against the time budget these cases
take the minimum number of samples.

Finally, a baseline belongs to one machine, and the project doesn't commit
one. An absolute time reflects the cache hierarchy, the clock behavior and
the background load of a particular machine, so a comparison against a
baseline from other hardware reports differences that have nothing to do
with the library. Record your own with `make bench_update` or
`make bench_calibrate`.

For a run you can trust, close other programs, leave the machine alone while
it runs and plug a laptop into mains power, and then compare ratios, not
nanoseconds.

--------------------------------------------------------------------------

## Knowing how much a difference is worth

A percentage means nothing until you know how far the same number moves
by itself, and `make bench_calibrate` measures exactly that:

```bash
make bench_calibrate                       # runs the suite several times over
make bench_calibrate BENCH_ARGS="--calibrate=7"
```

It runs the whole suite K times (5 by default) and records, for each case,
how far its figure moved between those runs while nothing in the library
changed. It repeats whole passes instead of running one case several times
in a row, because part of what separates two runs is where a case sits in
the run, and back-to-back repeats of a single case can't show that.

```
case                              ns/op   run-to-run       gate
  alloc_free_64b                   6.60         4.1%       6.2%
  alloc_free_64b_12t               2.53        42.4%  not gated
```

`run-to-run` is that measured movement, and it's what you compare your own
results against. If a case moves 42 percent with nothing changed, a 30
percent difference in your measurement tells you nothing. The harness
reports a case whose run-to-run figure is above 30 percent but never flags
it, because for such a case the hardware adds more to the figure than the
library does.

Single-threaded cases are much steadier than heavily threaded ones. On a
machine with six performance cores, a single-threaded case repeats within a
few percent while a twelve-thread case moves by tens of percent, so one
shared threshold can't work for both.

--------------------------------------------------------------------------

## Gating

`--gate` turns a flagged case into a non-zero exit status:

```bash
make bench_gate
```

It isn't meant for CI, and this project's CI doesn't use it. Timings on a
shared virtual runner move more than most regressions worth catching, so a
threshold tight enough to be useful fails all the time and one loose enough
to stay quiet catches nothing. CI builds the benchmarks and lists their
cases instead, which catches the ways a benchmark suite rots (a case that
doesn't compile, a fixture that doesn't build) without pretending to measure
anything.

Only use `--gate` on a machine where `make bench_calibrate` has shown that
the figures repeat well enough, and read the run-to-run column before you
trust it.

--------------------------------------------------------------------------

## Optional comparison libraries

The build looks for each of these libraries, and each one it finds adds
its own comparison arms. None of them is required, and a missing library
just removes its own cases and changes nothing else.

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

A row in brackets runs another library doing the nearest equivalent
thing, which isn't always the identical thing. Where the two sides really
differ, the list below names the difference instead of correcting for it,
because a correction would measure something that neither library actually
does. The differences go both ways: the first two cost this library time it
wouldn't spend otherwise, and the last two save it time that the other side
spends. So before you quote a ratio from one of these rows, read its note
below.

- `GHashTable` and `GTree` store the benchmark's keys and values AS
  POINTERS and the caller keeps ownership of them, while this library copies
  both into its own storage, so those rows charge this library for a copy
  the other side never makes. Node allocation is NOT part of that difference
  on the tree rows. Measured with a counting allocator over 100000 inserts,
  `GTree` allocates one node per distinct key and `cbstmap` allocates the
  same number, so the two only differ by the copy. `GHashTable` is the one
  that allocates almost nothing per entry.
- The uthash rows allocate their entry nodes once, in the untimed setup.
  With an integer key that costs neither side anything per entry: over the
  same 100000 inserts `chashmap` makes 14 allocations and uthash makes 13,
  all from table growth inside the timed loop and none from per-entry work.
  With a string key the difference is real. A string key selects the
  separate chaining backend, which allocates one chain node per insert
  inside the clock (measured at 1.00 per insert), so those rows charge this
  library for an allocation that uthash doesn't pay.
- `GAsyncQueue` is a linked queue that allocates one node per push,
  measured at one allocation and one free per round trip. It is reported
  against `circular_queue_roundtrip`, a preallocated ring that allocates
  nothing at all.
- The `[malloc]` rows for `cmempool` get their memory inside the timed
  loop, while the pool allocates its backing block during setup, so the
  pool's pages fault in outside the clock and malloc's inside it.

Two comparisons are like for like, `cjson` against jansson and `cyaml`
against libyaml. In both of them each side parses the same bytes into its
own DOM and owns what it builds. The jansson serialize row also sets
jansson's real number precision to the same 15 significant digits that
`cjson` uses. Jansson defaults to 17 digits, which makes it write about 8
percent more bytes for the same document, and a serializer's cost follows
how much it writes.

## Adding a case

The cases live in `bench_core.c`, `bench_maps.c`, `bench_concurrency.c`,
`bench_cache.c`, `bench_logging.c`, `bench_serialization.c` and
`bench_http.c`. Each case registers itself through `bench_add`, and
`bench_add_mt` registers the same case at several thread counts at once.
`bench.h` defines the case structure and documents each field.

A case owns its fixture. `setup` builds it, `teardown` frees it and the
harness times neither. Keep the timed body to a single kind of work, so when
a figure moves you know what moved.

When the fixture starts threads, a server or a reactor, give the case a
`rest_after_teardown_ms` (see "Reproducibility" above), so that how fast
the fixture tears down doesn't change how hot the next sample runs.
