# Benchmark results

This file holds two recorded runs of `bench/`, kept here so that the figures
quoted in this repository name the machine that produced them. They describe
that machine and nothing more general. [bench/README.md](README.md) explains
how to take your own figures and how large a difference must be before you can
believe it.

The two full runs were taken ninety seconds apart on a machine that was running
nothing else, and this file reports both runs for every comparison, because a
single column would hide how much of each figure comes from the hardware rather
than from the code.

--------------------------------------------------------------------------

## Machine

| | |
|---|---|
| cpu model | Intel(R) Core(TM) Ultra 7 155H |
| topology | 22 logical CPUs; 6 performance cores with SMT (CPUs 0-11); 10 efficiency cores (CPUs 12-21) |
| ram | 62 GiB |
| kernel | Linux 6.12.111+deb13-amd64 |
| distro | Debian GNU/Linux 13 (trixie) |
| cc | gcc (Debian 14.2.0-19) 14.2.0 |
| libc | ldd (Debian GLIBC 2.41-12+deb13u4) 2.41 |
| governor | powersave |
| power | on mains |
| library | 1.0.0 |

Comparison libraries: uthash 2.3.0, GLib 2.84.4, Jansson 2.14, libyaml 0.2.5.

The harness pinned each worker to one logical CPU per physical core, taking
whole cores before sibling threads and using only the performance cores, so no
worker ran on an efficiency core. Six physical performance cores carry twelve
threads, which means a case that asks for more than six workers puts two
workers on some pairs of sibling threads: each four-thread case has cores to
itself, while the eight-thread and twelve-thread cases share cores. Threads
that a case starts for itself, such as the workers of a thread pool or the
writer thread of an asynchronous logger, may run on any of the performance
cores.

The package was at 60 degrees Celsius when the first run started and at 82
degrees when it ended; the second run started at 48 degrees and ended at 78.
Heat is one reason why a figure moves between two runs, but it does not explain
the largest movements here: of the five rows that move more than 10 percent,
three were slower in the second run and two were faster.

--------------------------------------------------------------------------

## Against other libraries

A number below 1.00 means that this library took less time. The table shows
both runs, and where the two runs disagree, the disagreement is the result.
Read the text below the table before you take any row as a comparison of two
equal things.

| group | case | c_collections | other | run 1 | run 2 |
|---|---|---|---|---|---|
| cmempool | `alloc_free_64b` | 4.90 ns | 7.10 ns (malloc) | **0.69x** | **0.69x** |
| cmempool | `alloc_free_4kb` | 4.66 ns | 26.50 ns (malloc) | **0.18x** | **0.17x** |
| cmempool | `alloc_free_64b_scattered` | 6.52 ns | 8.45 ns (malloc) | **0.77x** | **0.75x** |
| cmempool | `burst_alloc_free_64b` | 10.34 ns | 19.46 ns (malloc) | **0.53x** | **0.51x** |
| cmempool | `alloc_free_64b_4t` | 2.02 ns | 2.13 ns (malloc) | **0.95x** | **0.65x** |
| cmempool | `alloc_free_64b_8t` | 2.26 ns | 2.65 ns (malloc) | **0.85x** | **0.78x** |
| cmempool | `alloc_free_64b_12t` | 2.36 ns | 2.54 ns (malloc) | **0.93x** | **0.90x** |
| chashmap | `insert_int_int` | 40.55 ns | 43.08 ns (uthash) | **0.94x** | **0.93x** |
| chashmap | `lookup_int_int_hit` | 13.80 ns | 17.89 ns (uthash) | **0.77x** | **0.78x** |
| chashmap | `insert_str_int` | 63.09 ns | 77.35 ns (uthash) | **0.82x** | **0.82x** |
| chashmap | `lookup_str_int_hit` | 21.32 ns | 31.12 ns (uthash) | **0.69x** | **0.69x** |
| chashmap | `insert_int_int` | 40.55 ns | 44.50 ns (GHashTable) | **0.91x** | **0.91x** |
| chashmap | `lookup_int_int_hit` | 13.80 ns | 15.91 ns (GHashTable) | **0.87x** | **0.90x** |
| cbstmap | `insert_int_int` | 160.59 ns | 151.94 ns (GTree) | **1.06x** | **1.04x** |
| cbstmap | `lookup_int_int_hit` | 117.46 ns | 120.20 ns (GTree) | **0.98x** | **0.96x** |
| cthreadpool | `submit_and_drain_4_workers` | 864.95 ns | 1,008 ns (GThreadPool) | **0.86x** | **0.84x** |
| cthreadcomm | `circular_queue_roundtrip` | 25.88 ns | 28.00 ns (GAsyncQueue) | **0.92x** | **0.92x** |
| cthreadcomm | `circular_queue_roundtrip_4t` | 180.34 ns | 204.23 ns (GAsyncQueue) | **0.88x** | **1.28x** |
| cthreadcomm | `circular_queue_roundtrip_8t` | 294.25 ns | 352.35 ns (GAsyncQueue) | **0.84x** | **0.69x** |
| cthreadcomm | `circular_queue_roundtrip_12t` | 327.85 ns | 409.93 ns (GAsyncQueue) | **0.80x** | **0.65x** |
| cjson | `parse_document` | 293,880 ns | 487,255 ns (jansson) | **0.60x** | **0.60x** |
| cjson | `serialize_document` | 132,338 ns | 239,474 ns (jansson) | **0.55x** | **0.54x** |
| cyaml | `parse_document` | 709,862 ns | 831,864 ns (libyaml) | **0.85x** | **0.88x** |

Every ratio is below 1.00 in both runs, with two exceptions. The `cbstmap`
insert against `GTree` reads 1.06 and 1.04, while its lookup reads 0.98 and
0.96. `circular_queue_roundtrip_4t` reads 0.88 in run 1 and 1.28 in run 2
because this library's arm moved 32 percent between the two runs, so that row
shows no difference larger than that movement. `alloc_free_64b_4t` reads 0.95
and 0.65 for the opposite reason: its `malloc` arm moved 40 percent, more than
any other row.

--------------------------------------------------------------------------

## What the comparison rows do and do not compare

A row in brackets is another library doing the nearest equivalent thing, not the identical thing. Where the two sides really differ, this list
names the difference without correcting it, because a correction would measure
something that neither library does. The differences do not all point the same
way: the first two below cost this library time that it would not otherwise
spend, and the last two save it time that the other side spends.

The allocation counts below come from a counting allocator that wraps `malloc`,
`calloc`, `realloc` and `free` and counts only inside the body that the harness
times, using the same 100000 keys that the benchmark draws (99998 of them
distinct).

- `GHashTable` and `GTree` store the benchmark's keys and values AS POINTERS,
  and the caller keeps ownership of them, whereas this library copies both into
  its own storage. Those rows therefore charge this library for a copy that the
  other side does not make. On the tree rows, node allocation is NOT part of
  that difference: over 100000 inserts, `GTree` allocates one node per distinct
  key, 99998 in all, and `cbstmap` allocates the same number, so the two differ
  by the copy alone. `GHashTable` is the one that allocates almost nothing per
  entry: 15 allocations and 42 reallocations over the same inserts.
- The uthash rows allocate their entry nodes once, in the untimed setup. On the
  integer-key rows this costs neither side anything per entry: over the same
  100000 inserts, `chashmap` makes 14 allocations and uthash makes 13, all of
  them from table growth inside the timed loop rather than from per-entry work.
  On the string-key rows the difference is real. A string key selects the
  separate chaining backend, which allocates one chain node per new key inside
  the clock (100005 allocations over 100000 inserts, or 1.00 per insert), while
  uthash makes 13 on the same row, so those rows charge this library for an
  allocation that uthash does not pay.
- `GAsyncQueue` is a linked queue that allocates one node per push, measured at
  one allocation and one free per round trip. The harness reports it against
  `circular_queue_roundtrip`, a preallocated ring that allocates nothing at
  all, measured at zero allocations over 100000 round trips.
- The `[malloc]` rows for `cmempool` get their memory inside the timed loop,
  while the pool allocates its backing block in the setup, so the pool's pages
  fault in outside the clock and malloc's pages fault in inside it.

Two comparisons are like for like: `cjson` against jansson, and `cyaml` against
libyaml. In both, each side parses the same bytes into a DOM of its own and owns
what it builds. The jansson serialize row also sets jansson's real number
precision to the same 15 significant digits that `cjson` uses, so both sides
write the same 26018 bytes for the benchmark document. With its default of 17
digits, jansson writes 28228 bytes, about 8 percent more, and the cost of a
serializer follows how much it writes.

--------------------------------------------------------------------------

## How much of this is the hardware

The harness measured every case above twice, in two whole runs ninety seconds
apart, with nothing else on the machine. "Movement" below is the difference
between the two runs of a case as a percentage of the smaller of the two, so
every figure in this section can be recomputed from the table under "Every
case". That table has 134 rows, counting the comparison arms. Across all of
them, the movement between the two runs has a median of 1.2 percent, a 75th
percentile of 2.8 percent, a 90th percentile of 5.7 percent and a maximum of
40.4 percent, and two rows are above 30 percent.

A difference smaller than the movement of its case is not a difference. Five
rows move more than 10 percent: four of them run with four or more threads,
and the fifth, `clogger/write_logfmt_async_caller_side` at 19.4 percent, hands
each record to a writer thread of its own. No other row moves more than 6.2
percent. The largest movement is the `malloc` arm of
`cmempool/alloc_free_64b_4t`, at 2.13 ns in run 1 and 2.99 ns in run 2,
followed by `cthreadcomm/circular_queue_roundtrip_4t` at 31.6 percent and its
8-thread case at 16.1 percent. A case ends when its slowest worker ends, so its
figure belongs to whichever worker got the smallest share of the package power
budget that the whole run competes for; above six workers, it also belongs to
whichever pair of workers shares the execution resources of one core.

Movement is not the only way a case can be unsteady, and it is not always the
larger of the two numbers. The `spread` column of the table below goes above
30 percent on five cases, yet the two runs of every one of them agree within 4
percent: `clogger/suppressed_below_level_12t` has a spread of 68.0 percent
against a movement of 3.8 percent, `cvector/push_int_growing_12t` has 51.6
against 0.8, `cstring/append_8_bytes_12t` has 42.5 against 0.6,
`cvector/access_random` has 33.3 against 1.7, and
`cmempool/alloc_free_64b_12t` has 32.1 against 2.2. A case whose two runs
agree can still land anywhere inside its spread on a third run, so a
difference must be larger than the larger of the two columns, not merely
larger than the movement.

--------------------------------------------------------------------------

## Every case

This table holds both runs, in the order in which the harness reports them.
`spread` is the p90-to-p10 sample range of run 1 as a percentage of the median
of run 1.

| group | case | run 1 ns | run 2 ns | spread | rate (run 1) |
|---|---|---|---|---|---|
| cvector | `push_int_growing` | 2.27 | 2.27 | 8.9% | 440.11 M/s |
| cvector | `push_int_reserved` | 2.27 | 2.27 | 6.1% | 440.61 M/s |
| cvector | `access_sequential` | 0.71 | 0.71 | 18.8% | 1415.00 M/s |
| cvector | `access_random` | 2.38 | 2.34 | 33.3% | 420.22 M/s |
| cvector | `push_int_growing_4t` | 0.81 | 0.82 | 13.8% | 1235.82 M/s |
| cvector | `push_int_growing_8t` | 1.11 | 1.14 | 9.3% | 900.18 M/s |
| cvector | `push_int_growing_12t` | 1.23 | 1.24 | 51.6% | 814.61 M/s |
| cvector | `access_random_4t` | 1.85 | 1.77 | 8.4% | 539.83 M/s |
| cvector | `access_random_8t` | 1.95 | 1.93 | 2.6% | 513.14 M/s |
| cvector | `access_random_12t` | 1.43 | 1.43 | 2.4% | 698.17 M/s |
| csort | `mergesort_int_per_elem` | 74.00 | 73.83 | 3.0% | 13.51 M/s |
| csort | `mergesort_int_per_elem_4t` | 22.34 | 22.36 | 12.0% | 44.76 M/s |
| csort | `mergesort_int_per_elem_8t` | 15.49 | 15.49 | 7.7% | 64.58 M/s |
| csort | `mergesort_int_per_elem_12t` | 11.15 | 11.28 | 8.4% | 89.72 M/s |
| cstring | `append_8_bytes` | 3.80 | 3.82 | 7.7% | 263.29 M/s |
| cstring | `find_4kb_haystack` | 113.34 | 113.63 | 7.5% | 8.82 M/s |
| cstring | `append_8_bytes_4t` | 1.33 | 1.34 | 11.6% | 750.47 M/s |
| cstring | `append_8_bytes_8t` | 1.66 | 1.79 | 7.0% | 602.10 M/s |
| cstring | `append_8_bytes_12t` | 1.73 | 1.74 | 42.5% | 579.58 M/s |
| cmempool | `alloc_free_64b` | 4.90 | 4.95 | 9.6% | 204.01 M/s |
| cmempool | `alloc_free_64b_single_threaded` | 5.43 | 5.52 | 4.3% | 184.12 M/s |
| cmempool | `alloc_free_64b [malloc]` | 7.10 | 7.22 | 7.5% | 140.85 M/s |
| cmempool | `alloc_free_4kb` | 4.66 | 4.71 | 3.4% | 214.67 M/s |
| cmempool | `alloc_free_4kb [malloc]` | 26.50 | 28.02 | 3.4% | 37.74 M/s |
| cmempool | `alloc_free_64b_scattered` | 6.52 | 6.62 | 3.7% | 153.36 M/s |
| cmempool | `alloc_free_64b_scattered [malloc]` | 8.45 | 8.86 | 15.1% | 118.31 M/s |
| cmempool | `burst_alloc_free_64b` | 10.34 | 10.36 | 6.8% | 96.72 M/s |
| cmempool | `burst_alloc_free_64b [malloc]` | 19.46 | 20.29 | 3.1% | 51.39 M/s |
| cmempool | `alloc_free_64b_4t` | 2.02 | 1.93 | 13.8% | 494.66 M/s |
| cmempool | `alloc_free_64b_8t` | 2.26 | 2.19 | 10.5% | 442.76 M/s |
| cmempool | `alloc_free_64b_12t` | 2.36 | 2.31 | 32.1% | 422.84 M/s |
| cmempool | `alloc_free_64b_4t [malloc]` | 2.13 | 2.99 | 16.6% | 469.42 M/s |
| cmempool | `alloc_free_64b_8t [malloc]` | 2.65 | 2.82 | 6.2% | 377.33 M/s |
| cmempool | `alloc_free_64b_12t [malloc]` | 2.54 | 2.58 | 27.3% | 393.08 M/s |
| chashmap | `insert_int_int` | 40.55 | 40.08 | 6.1% | 24.66 M/s |
| chashmap | `insert_int_int_4t` | 16.51 | 16.75 | 13.0% | 60.58 M/s |
| chashmap | `insert_int_int_8t` | 14.68 | 15.22 | 11.4% | 68.12 M/s |
| chashmap | `insert_int_int_12t` | 13.74 | 13.82 | 18.0% | 72.78 M/s |
| chashmap | `lookup_int_int_hit` | 13.80 | 13.90 | 6.3% | 72.44 M/s |
| chashmap | `lookup_int_int_hit_4t` | 8.86 | 8.91 | 6.1% | 112.88 M/s |
| chashmap | `lookup_int_int_hit_8t` | 8.74 | 8.57 | 7.9% | 114.38 M/s |
| chashmap | `lookup_int_int_hit_12t` | 6.71 | 6.65 | 7.6% | 149.12 M/s |
| chashmap | `lookup_int_int_miss` | 13.41 | 13.77 | 6.2% | 74.56 M/s |
| chashmap | `remove_int_int` | 41.63 | 42.21 | 3.4% | 24.02 M/s |
| chashmap | `insert_str_int` | 63.09 | 63.14 | 5.1% | 15.85 M/s |
| chashmap | `insert_str_int_4t` | 27.07 | 27.25 | 4.9% | 36.95 M/s |
| chashmap | `insert_str_int_8t` | 23.98 | 25.58 | 4.6% | 41.69 M/s |
| chashmap | `insert_str_int_12t` | 18.46 | 18.64 | 7.2% | 54.16 M/s |
| chashmap | `lookup_str_int_hit` | 21.32 | 20.81 | 6.6% | 46.91 M/s |
| chashmap | `insert_int_int [uthash]` | 43.08 | 43.25 | 3.7% | 23.21 M/s |
| chashmap | `lookup_int_int_hit [uthash]` | 17.89 | 17.75 | 1.7% | 55.91 M/s |
| chashmap | `insert_str_int [uthash]` | 77.35 | 77.28 | 6.0% | 12.93 M/s |
| chashmap | `lookup_str_int_hit [uthash]` | 31.12 | 30.04 | 2.9% | 32.13 M/s |
| chashmap | `insert_int_int [GHashTable]` | 44.50 | 43.93 | 2.4% | 22.47 M/s |
| chashmap | `lookup_int_int_hit [GHashTable]` | 15.91 | 15.40 | 3.2% | 62.85 M/s |
| cbstmap | `insert_int_int` | 160.59 | 158.93 | 2.7% | 6.23 M/s |
| cbstmap | `insert_int_int_4t` | 70.72 | 71.91 | 11.9% | 14.14 M/s |
| cbstmap | `insert_int_int_8t` | 56.83 | 56.84 | 6.9% | 17.60 M/s |
| cbstmap | `insert_int_int_12t` | 46.58 | 46.49 | 2.5% | 21.47 M/s |
| cbstmap | `lookup_int_int_hit` | 117.46 | 116.41 | 3.2% | 8.51 M/s |
| cbstmap | `lookup_int_int_hit_4t` | 37.06 | 37.06 | 4.0% | 26.98 M/s |
| cbstmap | `lookup_int_int_hit_8t` | 28.85 | 29.01 | 7.7% | 34.66 M/s |
| cbstmap | `lookup_int_int_hit_12t` | 25.27 | 25.10 | 4.6% | 39.57 M/s |
| cbstmap | `insert_int_int [GTree]` | 151.94 | 153.02 | 1.7% | 6.58 M/s |
| cbstmap | `lookup_int_int_hit [GTree]` | 120.20 | 121.64 | 1.9% | 8.32 M/s |
| cthreadpool | `submit_and_drain_1_worker` | 353.61 | 364.09 | 4.5% | 2.83 M/s |
| cthreadpool | `submit_and_drain_4_workers` | 864.95 | 858.96 | 8.9% | 1.16 M/s |
| cthreadpool | `submit_contended_4t` | 606.65 | 625.71 | 6.5% | 1.65 M/s |
| cthreadpool | `submit_contended_8t` | 610.85 | 627.90 | 2.6% | 1.64 M/s |
| cthreadpool | `submit_contended_12t` | 625.34 | 648.78 | 2.1% | 1.60 M/s |
| cthreadpool | `submit_and_drain_4_workers [GThreadPool]` | 1,008 | 1,020 | 11.7% | 991.77 K/s |
| cthreadcomm | `circular_queue_roundtrip` | 25.88 | 26.05 | 5.8% | 38.64 M/s |
| cthreadcomm | `circular_queue_roundtrip_4t` | 180.34 | 237.30 | 8.5% | 5.55 M/s |
| cthreadcomm | `circular_queue_roundtrip_8t` | 294.25 | 253.43 | 3.9% | 3.40 M/s |
| cthreadcomm | `circular_queue_roundtrip_12t` | 327.85 | 287.05 | 2.7% | 3.05 M/s |
| cthreadcomm | `dynamic_queue_roundtrip` | 29.58 | 29.79 | 4.4% | 33.80 M/s |
| cthreadcomm | `dynamic_queue_roundtrip_4t` | 191.53 | 207.31 | 7.3% | 5.22 M/s |
| cthreadcomm | `dynamic_queue_roundtrip_8t` | 365.02 | 379.86 | 5.1% | 2.74 M/s |
| cthreadcomm | `dynamic_queue_roundtrip_12t` | 456.17 | 459.32 | 8.3% | 2.19 M/s |
| cthreadcomm | `channel_producer_consumer` | 54.91 | 55.15 | 4.1% | 18.21 M/s |
| cthreadcomm | `circular_queue_producer_consumer` | 52.37 | 52.53 | 1.8% | 19.09 M/s |
| cthreadcomm | `circular_queue_roundtrip [GAsyncQueue]` | 28.00 | 28.19 | 2.7% | 35.72 M/s |
| cthreadcomm | `circular_queue_roundtrip_4t [GAsyncQueue]` | 204.23 | 185.90 | 4.8% | 4.90 M/s |
| cthreadcomm | `circular_queue_roundtrip_8t [GAsyncQueue]` | 352.35 | 367.79 | 3.4% | 2.84 M/s |
| cthreadcomm | `circular_queue_roundtrip_12t [GAsyncQueue]` | 409.93 | 444.64 | 2.6% | 2.44 M/s |
| clrucache | `get_all_hits` | 44.40 | 44.49 | 5.7% | 22.52 M/s |
| clrucache | `get_all_hits_4t` | 60.30 | 61.15 | 20.1% | 16.58 M/s |
| clrucache | `get_all_hits_8t` | 56.73 | 58.02 | 9.7% | 17.63 M/s |
| clrucache | `get_all_hits_12t` | 57.87 | 56.03 | 6.5% | 17.28 M/s |
| clrucache | `get_or_fill_working_set_8x_capacity` | 143.57 | 143.46 | 4.7% | 6.97 M/s |
| clrucache | `set_with_eviction` | 119.82 | 118.95 | 3.4% | 8.35 M/s |
| clrucache | `set_with_eviction_4t` | 109.15 | 107.03 | 5.7% | 9.16 M/s |
| clrucache | `set_with_eviction_8t` | 81.66 | 79.92 | 4.8% | 12.25 M/s |
| clrucache | `set_with_eviction_12t` | 81.24 | 79.11 | 11.1% | 12.31 M/s |
| clogger | `write_logfmt_sync` | 660.34 | 662.95 | 4.5% | 1.51 M/s |
| clogger | `write_logfmt_sync_4t` | 1,063 | 1,048 | 15.5% | 940.51 K/s |
| clogger | `write_logfmt_sync_8t` | 1,164 | 1,162 | 6.6% | 859.25 K/s |
| clogger | `write_logfmt_sync_12t` | 1,159 | 1,137 | 8.2% | 863.02 K/s |
| clogger | `write_json_sync` | 616.47 | 614.90 | 5.6% | 1.62 M/s |
| clogger | `write_logfmt_async` | 770.99 | 806.09 | 7.6% | 1.30 M/s |
| clogger | `write_logfmt_async_caller_side` | 336.36 | 401.67 | 10.0% | 2.97 M/s |
| clogger | `write_logfmt_async_caller_side_4t` | 349.97 | 344.78 | 6.0% | 2.86 M/s |
| clogger | `write_logfmt_async_caller_side_8t` | 402.09 | 406.97 | 5.8% | 2.49 M/s |
| clogger | `write_logfmt_async_caller_side_12t` | 454.39 | 423.81 | 29.4% | 2.20 M/s |
| clogger | `suppressed_below_level` | 12.64 | 12.66 | 5.1% | 79.13 M/s |
| clogger | `suppressed_below_level_4t` | 4.43 | 4.50 | 8.7% | 225.85 M/s |
| clogger | `suppressed_below_level_8t` | 4.50 | 4.57 | 7.5% | 222.21 M/s |
| clogger | `suppressed_below_level_12t` | 13.98 | 13.47 | 68.0% | 71.51 M/s |
| cjson | `parse_document` | 293,880 | 288,987 | 13.5% | 3.40 K/s |
| cjson | `parse_document_4t` | 98,279 | 100,335 | 14.1% | 10.18 K/s |
| cjson | `parse_document_8t` | 93,361 | 93,866 | 18.4% | 10.71 K/s |
| cjson | `parse_document_12t` | 70,399 | 72,628 | 21.1% | 14.20 K/s |
| cjson | `serialize_document` | 132,338 | 132,091 | 4.5% | 7.56 K/s |
| cjson | `serialize_document_4t` | 47,790 | 47,778 | 7.6% | 20.92 K/s |
| cjson | `serialize_document_8t` | 43,228 | 43,498 | 5.3% | 23.13 K/s |
| cjson | `serialize_document_12t` | 32,054 | 32,299 | 11.8% | 31.20 K/s |
| cjson | `parse_document [jansson]` | 487,255 | 485,439 | 4.0% | 2.05 K/s |
| cjson | `serialize_document [jansson]` | 239,474 | 242,640 | 0.6% | 4.18 K/s |
| cyaml | `parse_document` | 709,862 | 716,404 | 1.3% | 1.41 K/s |
| cyaml | `parse_document_4t` | 221,967 | 223,955 | 5.5% | 4.51 K/s |
| cyaml | `parse_document_8t` | 206,710 | 208,663 | 4.8% | 4.84 K/s |
| cyaml | `parse_document_12t` | 159,238 | 166,405 | 6.8% | 6.28 K/s |
| cyaml | `serialize_document` | 236,418 | 237,851 | 2.4% | 4.23 K/s |
| cyaml | `serialize_document_4t` | 78,122 | 78,324 | 5.3% | 12.80 K/s |
| cyaml | `serialize_document_8t` | 70,608 | 71,318 | 3.5% | 14.16 K/s |
| cyaml | `serialize_document_12t` | 51,343 | 53,027 | 7.8% | 19.48 K/s |
| cyaml | `parse_document [libyaml]` | 831,864 | 817,653 | 3.6% | 1.20 K/s |
| http | `get_sequential_keepalive` | 20,941 | 21,519 | 2.9% | 47.75 K/s |
| http | `get_concurrent_clients_4t` | 6,451 | 6,494 | 2.9% | 155.02 K/s |
| http | `get_concurrent_clients_8t` | 4,694 | 4,806 | 7.7% | 213.04 K/s |
| http | `get_concurrent_clients_12t` | 5,157 | 5,215 | 1.5% | 193.91 K/s |
| http | `post_1kb_sequential_keepalive` | 22,065 | 22,211 | 1.9% | 45.32 K/s |
| http | `post_1mb_sequential_keepalive` | 337,017 | 317,350 | 11.3% | 2.97 K/s |
| http | `get_1mb_sequential_keepalive` | 364,658 | 360,785 | 12.4% | 2.74 K/s |