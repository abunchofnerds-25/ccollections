# Benchmark results

Two recorded runs of `bench/`, kept so the figures in this repository have a
machine attached to them. They describe the machine recorded below and nothing more
general. [bench/README.md](README.md) explains how to take your own and how
much of a difference is worth believing.

Two full runs were taken, ninety seconds apart, on an otherwise idle machine.
Both are reported for every comparison, because a single column would hide how
much of a figure belongs to the hardware rather than to the code.

--------------------------------------------------------------------------

## Machine

| | |
|---|---|
| cpu model | Intel(R) Core(TM) Ultra 7 155H |
| topology | 22 logical CPUs; 6 performance cores with SMT (CPUs 0-11); 10 efficiency cores (CPUs 12-21) |
| ram | 62 GiB |
| kernel | Linux 6.12.107+deb13-amd64 |
| distro | Debian GNU/Linux 13 (trixie) |
| cc | gcc (Debian 14.2.0-19) 14.2.0 |
| libc | ldd (Debian GLIBC 2.41-12+deb13u4) 2.41 |
| governor | powersave |
| power | on mains |
| library | 1.0.0 |

Comparison libraries: uthash 2.3.0, GLib 2.84.4, Jansson 2.14, libyaml 0.2.5.

Workers were pinned one logical CPU per physical core, cores before sibling
threads, and only to the performance cores, so no worker ran on an efficiency
core. Six physical performance cores carry twelve threads, so a case asking for
more than six workers doubles up on sibling threads: the four-thread cases hold
a core each, the eight- and twelve-thread ones share. The package was at 79
degrees Celsius when the first run started and 75 when it finished, having been
at 67 before the tree was built; a run reaches that machine's thermal ceiling
within about a minute, which is the single largest reason a figure here moves
between runs.

--------------------------------------------------------------------------

## Against other libraries

Lower than 1.00 means this library took less time. Both runs are shown; where
they disagree, the disagreement is the result. Read the disclosure below the
table before taking any row as a like-for-like comparison.

| group | case | c_collections | other | run 1 | run 2 |
|---|---|---|---|---|---|
| cmempool | `alloc_free_64b` | 6.20 ns | 8.01 ns (malloc) | **0.77x** | **0.88x** |
| cmempool | `alloc_free_4kb` | 6.46 ns | 30.26 ns (malloc) | **0.21x** | **0.22x** |
| cmempool | `alloc_free_64b_scattered` | 8.54 ns | 9.38 ns (malloc) | **0.91x** | **0.84x** |
| cmempool | `burst_alloc_free_64b` | 12.01 ns | 21.18 ns (malloc) | **0.57x** | **0.55x** |
| cmempool | `alloc_free_64b_4t` | 2.27 ns | 2.45 ns (malloc) | **0.93x** | **0.95x** |
| cmempool | `alloc_free_64b_8t` | 2.70 ns | 2.95 ns (malloc) | **0.92x** | **0.92x** |
| cmempool | `alloc_free_64b_12t` | 2.26 ns | 2.57 ns (malloc) | **0.88x** | **0.98x** |
| chashmap | `insert_int_int` | 51.14 ns | 46.18 ns (uthash) | **1.11x** | **1.00x** |
| chashmap | `lookup_int_int_hit` | 14.64 ns | 18.43 ns (uthash) | **0.79x** | **0.78x** |
| chashmap | `insert_str_int` | 68.83 ns | 84.29 ns (uthash) | **0.82x** | **0.83x** |
| chashmap | `lookup_str_int_hit` | 22.93 ns | 32.96 ns (uthash) | **0.70x** | **0.72x** |
| chashmap | `insert_int_int` | 51.14 ns | 46.48 ns (GHashTable) | **1.10x** | **1.01x** |
| chashmap | `lookup_int_int_hit` | 14.64 ns | 16.09 ns (GHashTable) | **0.91x** | **0.89x** |
| cbstmap | `insert_int_int` | 238.08 ns | 155.77 ns (GTree) | **1.53x** | **1.49x** |
| cbstmap | `lookup_int_int_hit` | 162.64 ns | 122.10 ns (GTree) | **1.33x** | **1.30x** |
| cthreadpool | `submit_and_drain_4_workers` | 207.62 ns | 528.76 ns (GThreadPool) | **0.39x** | **0.40x** |
| cthreadcomm | `circular_queue_roundtrip` | 27.57 ns | 29.82 ns (GAsyncQueue) | **0.92x** | **0.90x** |
| cthreadcomm | `circular_queue_roundtrip_4t` | 186.86 ns | 178.81 ns (GAsyncQueue) | **1.05x** | **0.92x** |
| cthreadcomm | `circular_queue_roundtrip_8t` | 312.89 ns | 400.43 ns (GAsyncQueue) | **0.78x** | **0.79x** |
| cthreadcomm | `circular_queue_roundtrip_12t` | 326.79 ns | 419.63 ns (GAsyncQueue) | **0.78x** | **0.74x** |
| cjson | `parse_document` | 336,036 ns | 489,851 ns (jansson) | **0.69x** | **0.70x** |
| cjson | `serialize_document` | 136,000 ns | 241,901 ns (jansson) | **0.56x** | **0.56x** |
| cyaml | `parse_document` | 798,283 ns | 835,698 ns (libyaml) | **0.96x** | **0.99x** |

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

--------------------------------------------------------------------------

## How much of this is the hardware

Every case above was measured twice, in two whole runs ninety seconds apart,
with nothing else running. Movement below is the difference between a case's two
runs as a percentage of the smaller of them, so every figure in this section is
recomputable from the table under "Every case". Across all 131 rows of that
table, comparison arms included, the run-to-run movement has a median of 2.8
percent, a 75th percentile of 7.5 percent, a 90th percentile of 16.8 percent and
a maximum of 119.5 percent, with 6 rows above 30 percent.

A difference smaller than the case's own movement is not a difference. Those 6
rows are the ones worth knowing about before reading anything into them:

| case | run 1 | run 2 | movement | spread (run 1) |
|---|---|---|---|---|
| cstring/`append_8_bytes_4t` | 1.23 ns | 2.70 ns | 119.5% | 305.9% |
| chashmap/`insert_int_int_4t` | 30.20 ns | 15.78 ns | 91.4% | 41.9% |
| cmempool/`alloc_free_64b_12t [malloc]` | 2.57 ns | 1.73 ns | 48.6% | 50.5% |
| clogger/`suppressed_below_level_4t` | 3.53 ns | 5.17 ns | 46.5% | 7.8% |
| cmempool/`alloc_free_64b_12t` | 2.26 ns | 1.69 ns | 33.7% | 178.1% |
| csort/`mergesort_int_per_elem_8t` | 59.03 ns | 45.03 ns | 31.1% | 70.8% |

They are all multi-threaded, and one of them is glibc `malloc` rather than this
library, which is the plainest evidence that what moves on these rows is the
machine. A case ends when its slowest worker does, so its figure belongs to
whichever worker got the least of a package power budget the whole run competes
for, and, above six workers, to whichever pair is sharing one core's execution
resources. The two largest movers are four-thread cases, which hold a core each,
so on those the power budget is the whole of it.

Movement is not the only way a case can be unsteady, and it is not always the
larger of the two. The `spread` column of the table below passes 100 percent on
three further cases whose two runs agree closely: `cstring/append_8_bytes_12t`
at 288.4 percent against 24.3 percent movement, `chashmap/insert_int_int_12t` at
120.2 against 8.5, and `clogger/suppressed_below_level_12t` at 101.2 against
8.9. A case whose two runs agree can still land anywhere inside its spread on a
third, so what a difference has to clear is the larger of the two columns, not
the movement alone.

--------------------------------------------------------------------------

## Every case

Both runs, in the order the harness reports them. `spread` is run 1's own
p90-to-p10 sample range as a percentage of its median.

| group | case | run 1 ns | run 2 ns | spread | rate (run 1) |
|---|---|---|---|---|---|
| cvector | `push_int_growing` | 2.51 | 2.51 | 24.4% | 399.09 M/s |
| cvector | `push_int_reserved` | 2.61 | 2.37 | 3.3% | 382.94 M/s |
| cvector | `access_sequential` | 1.48 | 1.41 | 27.7% | 676.17 M/s |
| cvector | `access_random` | 2.64 | 2.67 | 11.3% | 379.44 M/s |
| cvector | `push_int_growing_4t` | 2.98 | 2.58 | 47.4% | 335.05 M/s |
| cvector | `push_int_growing_8t` | 1.53 | 1.48 | 12.6% | 655.16 M/s |
| cvector | `push_int_growing_12t` | 1.30 | 1.30 | 13.5% | 768.99 M/s |
| cvector | `access_random_4t` | 2.57 | 2.59 | 13.8% | 389.09 M/s |
| cvector | `access_random_8t` | 2.94 | 2.82 | 8.7% | 340.39 M/s |
| cvector | `access_random_12t` | 2.15 | 2.07 | 10.9% | 465.71 M/s |
| csort | `mergesort_int_per_elem` | 227.42 | 209.69 | 10.2% | 4.40 M/s |
| csort | `mergesort_int_per_elem_4t` | 72.23 | 62.65 | 21.6% | 13.85 M/s |
| csort | `mergesort_int_per_elem_8t` | 59.03 | 45.03 | 70.8% | 16.94 M/s |
| csort | `mergesort_int_per_elem_12t` | 40.89 | 32.73 | 24.4% | 24.46 M/s |
| cstring | `append_8_bytes` | 3.67 | 3.29 | 2.6% | 272.46 M/s |
| cstring | `find_4kb_haystack` | 124.77 | 113.48 | 5.4% | 8.01 M/s |
| cstring | `append_8_bytes_4t` | 1.23 | 2.70 | 305.9% | 810.19 M/s |
| cstring | `append_8_bytes_8t` | 2.02 | 1.61 | 17.9% | 495.99 M/s |
| cstring | `append_8_bytes_12t` | 1.74 | 1.40 | 288.4% | 575.96 M/s |
| cmempool | `alloc_free_64b` | 6.20 | 6.04 | 19.7% | 161.41 M/s |
| cmempool | `alloc_free_64b_single_threaded` | 6.72 | 6.37 | 16.1% | 148.86 M/s |
| cmempool | `alloc_free_64b [malloc]` | 8.01 | 6.86 | 93.7% | 124.83 M/s |
| cmempool | `alloc_free_4kb` | 6.46 | 5.74 | 9.4% | 154.81 M/s |
| cmempool | `alloc_free_4kb [malloc]` | 30.26 | 26.40 | 12.9% | 33.05 M/s |
| cmempool | `alloc_free_64b_scattered` | 8.54 | 7.95 | 19.9% | 117.16 M/s |
| cmempool | `alloc_free_64b_scattered [malloc]` | 9.38 | 9.47 | 29.9% | 106.64 M/s |
| cmempool | `burst_alloc_free_64b` | 12.01 | 10.78 | 11.5% | 83.29 M/s |
| cmempool | `burst_alloc_free_64b [malloc]` | 21.18 | 19.62 | 3.1% | 47.21 M/s |
| cmempool | `alloc_free_64b_4t` | 2.27 | 2.02 | 25.1% | 440.73 M/s |
| cmempool | `alloc_free_64b_8t` | 2.70 | 2.18 | 44.1% | 371.02 M/s |
| cmempool | `alloc_free_64b_12t` | 2.26 | 1.69 | 178.1% | 442.99 M/s |
| cmempool | `alloc_free_64b_4t [malloc]` | 2.45 | 2.12 | 6.0% | 407.65 M/s |
| cmempool | `alloc_free_64b_8t [malloc]` | 2.95 | 2.38 | 3.6% | 339.20 M/s |
| cmempool | `alloc_free_64b_12t [malloc]` | 2.57 | 1.73 | 50.5% | 389.31 M/s |
| chashmap | `insert_int_int` | 51.14 | 45.29 | 12.1% | 19.55 M/s |
| chashmap | `insert_int_int_4t` | 30.20 | 15.78 | 41.9% | 33.11 M/s |
| chashmap | `insert_int_int_8t` | 16.93 | 13.92 | 93.4% | 59.05 M/s |
| chashmap | `insert_int_int_12t` | 12.88 | 11.87 | 120.2% | 77.64 M/s |
| chashmap | `lookup_int_int_hit` | 14.64 | 14.06 | 4.9% | 68.32 M/s |
| chashmap | `lookup_int_int_hit_4t` | 8.64 | 8.64 | 9.6% | 115.80 M/s |
| chashmap | `lookup_int_int_hit_8t` | 8.30 | 8.53 | 3.4% | 120.52 M/s |
| chashmap | `lookup_int_int_hit_12t` | 6.56 | 6.57 | 2.0% | 152.41 M/s |
| chashmap | `lookup_int_int_miss` | 14.69 | 14.22 | 3.0% | 68.05 M/s |
| chashmap | `remove_int_int` | 38.91 | 37.95 | 2.4% | 25.70 M/s |
| chashmap | `insert_str_int` | 68.83 | 69.16 | 16.7% | 14.53 M/s |
| chashmap | `insert_str_int_4t` | 38.40 | 35.95 | 6.7% | 26.04 M/s |
| chashmap | `insert_str_int_8t` | 28.28 | 26.55 | 4.1% | 35.36 M/s |
| chashmap | `insert_str_int_12t` | 21.79 | 22.98 | 45.8% | 45.89 M/s |
| chashmap | `lookup_str_int_hit` | 22.93 | 22.90 | 8.7% | 43.61 M/s |
| chashmap | `insert_int_int [uthash]` | 46.18 | 45.46 | 3.8% | 21.65 M/s |
| chashmap | `lookup_int_int_hit [uthash]` | 18.43 | 17.98 | 1.1% | 54.27 M/s |
| chashmap | `insert_str_int [uthash]` | 84.29 | 83.56 | 4.2% | 11.86 M/s |
| chashmap | `lookup_str_int_hit [uthash]` | 32.96 | 31.93 | 9.4% | 30.34 M/s |
| chashmap | `insert_int_int [GHashTable]` | 46.48 | 45.01 | 3.1% | 21.51 M/s |
| chashmap | `lookup_int_int_hit [GHashTable]` | 16.09 | 15.75 | 6.4% | 62.15 M/s |
| cbstmap | `insert_int_int` | 238.08 | 234.05 | 3.2% | 4.20 M/s |
| cbstmap | `insert_int_int_4t` | 103.87 | 97.36 | 4.7% | 9.63 M/s |
| cbstmap | `insert_int_int_8t` | 80.66 | 78.28 | 16.9% | 12.40 M/s |
| cbstmap | `insert_int_int_12t` | 63.30 | 60.52 | 2.7% | 15.80 M/s |
| cbstmap | `lookup_int_int_hit` | 162.64 | 159.92 | 1.8% | 6.15 M/s |
| cbstmap | `lookup_int_int_hit_4t` | 56.42 | 54.35 | 5.2% | 17.72 M/s |
| cbstmap | `lookup_int_int_hit_8t` | 39.35 | 38.38 | 6.2% | 25.41 M/s |
| cbstmap | `lookup_int_int_hit_12t` | 30.14 | 29.83 | 3.9% | 33.18 M/s |
| cbstmap | `insert_int_int [GTree]` | 155.77 | 156.80 | 2.4% | 6.42 M/s |
| cbstmap | `lookup_int_int_hit [GTree]` | 122.10 | 122.95 | 2.3% | 8.19 M/s |
| cthreadpool | `submit_and_drain_1_worker` | 95.08 | 77.05 | 23.6% | 10.52 M/s |
| cthreadpool | `submit_and_drain_4_workers` | 207.62 | 216.79 | 4.3% | 4.82 M/s |
| cthreadpool | `submit_contended_4t` | 304.39 | 297.27 | 11.8% | 3.29 M/s |
| cthreadpool | `submit_contended_8t` | 384.31 | 415.06 | 20.1% | 2.60 M/s |
| cthreadpool | `submit_contended_12t` | 405.19 | 442.49 | 17.4% | 2.47 M/s |
| cthreadpool | `submit_and_drain_4_workers [GThreadPool]` | 528.76 | 535.48 | 1.4% | 1.89 M/s |
| cthreadcomm | `circular_queue_roundtrip` | 27.57 | 26.81 | 2.8% | 36.28 M/s |
| cthreadcomm | `circular_queue_roundtrip_4t` | 186.86 | 173.80 | 12.8% | 5.35 M/s |
| cthreadcomm | `circular_queue_roundtrip_8t` | 312.89 | 307.58 | 3.5% | 3.20 M/s |
| cthreadcomm | `circular_queue_roundtrip_12t` | 326.79 | 320.08 | 2.5% | 3.06 M/s |
| cthreadcomm | `dynamic_queue_roundtrip` | 32.48 | 32.29 | 6.2% | 30.79 M/s |
| cthreadcomm | `dynamic_queue_roundtrip_4t` | 203.84 | 203.46 | 18.2% | 4.91 M/s |
| cthreadcomm | `dynamic_queue_roundtrip_8t` | 381.56 | 393.95 | 5.4% | 2.62 M/s |
| cthreadcomm | `dynamic_queue_roundtrip_12t` | 431.51 | 449.07 | 3.9% | 2.32 M/s |
| cthreadcomm | `channel_producer_consumer` | 59.44 | 58.20 | 8.5% | 16.82 M/s |
| cthreadcomm | `circular_queue_producer_consumer` | 55.83 | 55.60 | 2.1% | 17.91 M/s |
| cthreadcomm | `circular_queue_roundtrip [GAsyncQueue]` | 29.82 | 29.78 | 1.5% | 33.54 M/s |
| cthreadcomm | `circular_queue_roundtrip_4t [GAsyncQueue]` | 178.81 | 189.13 | 17.0% | 5.59 M/s |
| cthreadcomm | `circular_queue_roundtrip_8t [GAsyncQueue]` | 400.43 | 390.88 | 3.1% | 2.50 M/s |
| cthreadcomm | `circular_queue_roundtrip_12t [GAsyncQueue]` | 419.63 | 431.71 | 4.3% | 2.38 M/s |
| clrucache | `get_all_hits` | 47.82 | 48.23 | 9.3% | 20.91 M/s |
| clrucache | `get_all_hits_4t` | 64.88 | 62.24 | 21.5% | 15.41 M/s |
| clrucache | `get_all_hits_8t` | 60.40 | 57.74 | 12.2% | 16.56 M/s |
| clrucache | `get_all_hits_12t` | 56.46 | 54.91 | 4.8% | 17.71 M/s |
| clrucache | `get_or_fill_working_set_8x_capacity` | 158.75 | 160.76 | 6.7% | 6.30 M/s |
| clrucache | `set_with_eviction` | 130.99 | 133.44 | 4.1% | 7.63 M/s |
| clrucache | `set_with_eviction_4t` | 103.32 | 102.28 | 10.4% | 9.68 M/s |
| clrucache | `set_with_eviction_8t` | 75.43 | 76.11 | 62.3% | 13.26 M/s |
| clrucache | `set_with_eviction_12t` | 72.14 | 72.34 | 36.3% | 13.86 M/s |
| clogger | `write_logfmt_sync` | 1,099 | 1,105 | 10.3% | 910.07 K/s |
| clogger | `write_logfmt_sync_4t` | 1,546 | 1,541 | 18.5% | 646.83 K/s |
| clogger | `write_logfmt_sync_8t` | 1,602 | 1,595 | 12.9% | 624.34 K/s |
| clogger | `write_logfmt_sync_12t` | 1,581 | 1,554 | 9.1% | 632.32 K/s |
| clogger | `write_json_sync` | 1,063 | 1,057 | 3.9% | 941.12 K/s |
| clogger | `write_logfmt_async` | 1,114 | 1,117 | 15.4% | 897.82 K/s |
| clogger | `write_logfmt_async_caller_side` | 1,037 | 1,033 | 12.5% | 964.32 K/s |
| clogger | `write_logfmt_async_caller_side_4t` | 690.72 | 660.93 | 67.7% | 1.45 M/s |
| clogger | `write_logfmt_async_caller_side_8t` | 710.06 | 628.67 | 40.5% | 1.41 M/s |
| clogger | `write_logfmt_async_caller_side_12t` | 583.69 | 602.48 | 40.6% | 1.71 M/s |
| clogger | `suppressed_below_level` | 13.53 | 14.42 | 5.0% | 73.91 M/s |
| clogger | `suppressed_below_level_4t` | 3.53 | 5.17 | 7.8% | 283.29 M/s |
| clogger | `suppressed_below_level_8t` | 3.57 | 3.39 | 5.8% | 280.49 M/s |
| clogger | `suppressed_below_level_12t` | 12.58 | 11.55 | 101.2% | 79.52 M/s |
| cjson | `parse_document` | 336,036 | 338,532 | 6.4% | 2.98 K/s |
| cjson | `parse_document_4t` | 122,364 | 117,930 | 10.9% | 8.17 K/s |
| cjson | `parse_document_8t` | 111,277 | 109,288 | 5.9% | 8.99 K/s |
| cjson | `parse_document_12t` | 85,661 | 84,336 | 6.0% | 11.67 K/s |
| cjson | `serialize_document` | 136,000 | 135,596 | 3.0% | 7.35 K/s |
| cjson | `serialize_document_4t` | 52,766 | 51,402 | 28.4% | 18.95 K/s |
| cjson | `serialize_document_8t` | 47,411 | 46,435 | 7.9% | 21.09 K/s |
| cjson | `serialize_document_12t` | 36,332 | 35,543 | 8.9% | 27.52 K/s |
| cjson | `parse_document [jansson]` | 489,851 | 484,392 | 2.5% | 2.04 K/s |
| cjson | `serialize_document [jansson]` | 241,901 | 240,439 | 1.2% | 4.13 K/s |
| cyaml | `parse_document` | 798,283 | 832,434 | 0.9% | 1.25 K/s |
| cyaml | `parse_document_4t` | 325,816 | 333,899 | 5.9% | 3.07 K/s |
| cyaml | `parse_document_8t` | 325,654 | 320,886 | 5.4% | 3.07 K/s |
| cyaml | `parse_document_12t` | 295,165 | 284,250 | 6.7% | 3.39 K/s |
| cyaml | `serialize_document` | 312,141 | 310,728 | 3.6% | 3.20 K/s |
| cyaml | `serialize_document_4t` | 114,871 | 112,113 | 11.9% | 8.71 K/s |
| cyaml | `serialize_document_8t` | 97,134 | 96,910 | 2.4% | 10.30 K/s |
| cyaml | `serialize_document_12t` | 73,937 | 73,194 | 5.0% | 13.53 K/s |
| cyaml | `parse_document [libyaml]` | 835,698 | 838,047 | 3.8% | 1.20 K/s |
| http | `get_sequential_keepalive` | 28,075 | 29,479 | 28.7% | 35.62 K/s |
| http | `get_concurrent_clients_4t` | 13,989 | 13,990 | 10.9% | 71.49 K/s |
| http | `get_concurrent_clients_8t` | 14,242 | 14,115 | 2.5% | 70.21 K/s |
| http | `get_concurrent_clients_12t` | 14,660 | 14,575 | 8.5% | 68.21 K/s |

