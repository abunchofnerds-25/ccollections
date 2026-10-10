# Testing, fuzzing and benchmarks

This guide is for contributors. It shows how to run:

- the test suites
- the sanitizer and Valgrind builds
- coverage
- the interface checks
- the fuzzers
- the benchmarks

It also lists the jobs that CI runs on every push. Run every command on this
page from the root of the repository unless the text names another
directory.

## Before you start

On Debian or Ubuntu, install the packages that the full set of checks
needs:

```bash
sudo apt-get install build-essential clang libssl-dev zlib1g-dev \
    valgrind lcov abigail-tools
```

`make test` needs only the compiler, OpenSSL and zlib. `make memtest` also
needs Valgrind, coverage needs lcov, the fuzzers need Clang, the structural
half of `make check_abi` needs `abigail-tools`, and `make check_install`
needs `pkg-config`. On FreeBSD and macOS, use `gmake` wherever this page says
`make`. On FreeBSD, also install `binutils` for the interface checks, which
need GNU `readelf` and GNU `nm`. On macOS, Homebrew provides `gmake`, OpenSSL
and `pkg-config` (see [Building](building.md)); Valgrind does not run there,
so neither does `make memtest`.

## Running the tests

```bash
make test       # build and run all suites
make memtest    # all suites under Valgrind; a leak causes a failure
```

Each module has its own suite under `tests/<module>/`, and some directories
build more than one binary (for example, a TLS suite next to the main
suite). Both targets run every binary in every directory and stop at the
first failure. `tests/mixed/` holds the tests that span several modules,
including the compatibility corpus described in
[Compatibility](compatibility.md).

To work on one module, run just its suite:

```bash
make -C tests/chashmap test
make -C tests/chashmap memtest
```

The suites use the single-header framework in `tests/tau/`. A test binary
accepts a filter that matches from the start of `suite.test`, so list the
names first and then use a prefix that is shorter than the full name:

```bash
cd tests/chashmap && make build
./tests --list | head
./tests --filter='chash_maps.insert*'
```

Check the count on the `SUCCESS` line of a filtered run: a filter that
matches no test also reports success, with 0 test suites passed.

The suites compile the library sources directly into each test binary, with
the same `-O3` as the shipped library, so they test the code that ships.
`-DRUNNING_UNIT_TESTS` enables a few white-box helpers that only the suites
use.

**Run only one full suite at a time.** `tests/chttpserver` binds a fixed
port, so two `make test` or `make memtest` runs on the same machine collide on
it and fail with `listen socket setup failed ... port=18765`. Running a single
module's suite alongside a full run is fine, as long as that module is not
`chttpserver`.

## Sanitizers

Pass AddressSanitizer and UndefinedBehaviorSanitizer through
`EXTRA_CFLAGS`, which every suite honours:

```bash
make CC=clang EXTRA_CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined" test
```

ThreadSanitizer has its own target in the modules that run threads:
`cpintable`, `cmempool`, `clrucache`, `mixed`, `clogger`, `cthreadpool`,
`cthreadcomm`, `ctls`, `chttpclient` and `chttpserver`. Run each target with
both GCC and Clang, because GCC's ThreadSanitizer does not find every race
that Clang's finds:

```bash
make -C tests/cthreadpool test_tsan
make -C tests/cthreadpool CC=clang test_tsan
make -C tests/chttpserver test_engine_stop_tsan
```

For 32-bit builds, use the same targets with a 32-bit compiler:

```bash
make CC="gcc -m32" test
make CC="clang -m32" test
```

## Build variants that ship

Three compile-time switches select code that a default build never compiles
(see [Building](building.md#compile-time-configuration)). When you change
code behind one of these switches, run the suites that exercise it:

```bash
make -C tests/cmempool clean
make -C tests/cmempool EXTRA_CFLAGS="-DCCOL_MEMPOOL_COMPACT_LAYOUT=1" test

make clean
make EXTRA_CFLAGS="-DCCOL_FORK_SAFETY_REQUIRED=0" test
```

A build without the optional modules is a variant too:

```bash
make WITH_CJSON=0 WITH_CYAML=0 WITH_CLOGGER=0 \
     WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0 test
```

## Coverage

```bash
make coverage_site    # build all suites again with coverage, merge one report
make coverage_check   # fail if a file has less than 80 percent of its lines
```

`coverage_site` writes a browsable report to `coverage_site/html` and a
summary to `coverage_site/summary.txt`. It merges the runs of all suites,
because many suites exercise a file such as `src/common.c`, and the figures
cover only `src/` and `include/`. `coverage_check` reads the merged data, so
run `coverage_site` first. The threshold, and the short list of files that
have no tests of their own, live in `ci_scripts/check_test_coverages.sh`.

## Checks on the built library

Because the suites link the library sources directly, they never see which
symbols the shared library exports, or what happens when a program loads and
unloads it. These targets check the built artefacts instead:

```bash
make check_namespace       # each exported symbol, public macro, typedef,
                           # enumerator and struct/union/enum tag has
                           # its namespace prefix (shared library and archive)
make check_headers         # each installed header compiles alone under
                           # -std=c11 -pedantic-errors, with GCC and Clang
make check_abi             # the exported ABI matches the baseline in abi/
make update_abi_baseline   # record that baseline again after you add API
make check_filenames       # each installable file name is a literal path
make check_dso_unload      # fork() works after dlopen(), use and
                           # dlclose() of the library
make check_install         # install into a temporary prefix, build the
                           # README example with pkg-config, run it linked
                           # to the shared library and to the archive,
                           # uninstall
make hardening_report      # the hardening flags that this compiler accepts
```

On macOS, the checks read the dylib and the archive with the `nm` from the
developer tools. `check_abi` then compares only the exported symbols, because
its structural half reads DWARF from an ELF file; the ELF build checks the
macOS enumerators and header tags as well. `check_dso_unload` accepts a
library that stays mapped after `dlclose()`, because dyld never unloads a
dylib that has thread-local variables. Since Valgrind does not run on macOS,
`check_exit_reachable` runs on Linux and FreeBSD only.

[Compatibility](compatibility.md#how-the-promise-is-checked) explains
`check_abi` and `update_abi_baseline`. In short, you may add a public
function, but you must record the updated baseline and commit it with the
change; any other difference is a failure that you must explain.

## Fuzzing

Seven libFuzzer targets exercise the parsers and the URL layer of the HTTP
client. They need Clang and are not part of `make test`. CI runs them on
every push and pull request, so you need them only when you work on one of
those parsers.

```bash
# YAML: the document target also serializes the parsed document. Then it
# checks that the output parses again. The path target uses
# cyaml_get/set/delete.
cd tests/cyaml
make fuzz fuzz_path
./fuzz_cyaml fuzz/corpus -max_total_time=900
./fuzz_cyaml_path fuzz/corpus_path -max_total_time=900

# JSON: the document grammar and the path grammar.
cd ../cjson
make fuzz_parse fuzz_path
./fuzz_cjson_parse fuzz/corpus_parse -max_total_time=900
./fuzz_cjson_path  fuzz/corpus_path  -max_total_time=900

# HTTP/1.1 parser, request and response mode, and the URL and redirect
# layer of the client (input: a base URL, a NUL byte, a Location value).
cd ../chttpclient
make fuzz_request fuzz_response fuzz_url
./fuzz_chttp1_request  fuzz/corpus_request  -max_total_time=900
./fuzz_chttp1_response fuzz/corpus_response -max_total_time=900
./fuzz_chttp_url       fuzz/corpus_url      -max_total_time=900
```

Without `-max_total_time`, a fuzzer runs until you press Ctrl-C. It adds
every new input it finds to the corpus directory, so `git status` shows those
inputs after a run. Each finding is written to a `crash-*`, `timeout-*` or
`oom-*` file, and you can replay it exactly by passing that file as the only
argument:

```bash
./fuzz_cyaml crash-<hash>
```

## Benchmarks

`bench/` measures the modules whose speed matters:

- the containers, the strings and the sort
- the memory pools and the LRU cache
- the thread pool and the queues
- the logger and the two serializers
- an HTTP round trip

It links against the shared library that the root `make` builds, so it
measures the shipped code with the shipped flags. macOS cannot bind a thread
to a core, so there the harness runs unpinned and its figures vary more from
run to run.

```bash
make bench_update      # record a baseline for this machine
make bench             # run all cases and compare them with that baseline
make bench_calibrate   # record a baseline and measure how much each case
                       # changes between runs on this machine
make bench_gate        # the same as bench, but exit non-zero on a flagged case
make bench_list        # list the cases
```

Pass options through `BENCH_ARGS`:

```bash
make bench BENCH_ARGS="--filter=chashmap --reps=15"
make bench BENCH_ARGS="--threshold=10"
make bench_calibrate BENCH_ARGS="--calibrate=7"
```

Keep these points in mind when you read the numbers:

- **The baseline belongs to one machine** and is not committed. Compare the
  same machine before and after a change; a figure from different hardware
  tells you nothing about the library.
- **`make bench` reports; it does not fail.** `bench_calibrate` measures how
  much each case varies on its own, and `bench_gate` turns a flagged case into
  a failure. Use the gate only where the calibration shows stable figures.
- **Each figure is a median** of nanoseconds per operation across the
  repetitions. Setup and teardown are not timed.
- **Cases whose names end in `_4t`, `_8t` or `_12t`** run the same work on
  that many threads and report the wall time divided by all operations, so you
  can compare them directly with the single-threaded case. Thread-safe modules
  share one instance between the threads, while each thread gets its own
  instance of an unguarded container.
- **Cases with a library name in brackets** (`[malloc]`, `[glib]`,
  `[uthash]`, `[jansson]`, `[libyaml]`) run the same workload through that
  library when it is installed. Compare the ratios within one run, not the raw
  times.

[bench/README.md](../bench/README.md) is the full guide: what each column
means, how to get a run you can trust, and how to add a case.
[bench/RESULTS.md](../bench/RESULTS.md) records example runs and the machines
that produced them. CI builds the benchmarks and lists their cases but never
times them, because a shared virtual runner is far noisier than the changes
the benchmarks are meant to detect.

## What CI runs

Every push and every pull request runs the following jobs, and any failing
job blocks a merge:

- **Linux x86-64, GCC:** `make test`, `make memtest`, `check_namespace`,
  `check_headers`, `check_abi`, `check_filenames`, `check_dso_unload`,
  `check_exit_reachable`, `check_install`, the compact mempool layout, the
  general thread-local storage model, a full run with
  `CCOL_FORK_SAFETY_REQUIRED=0`, and a build of the benchmarks.
- **Linux x86-64, Clang:** `make test` and `make memtest`.
- **Linux x86-64, macOS code paths:** `make memtest` with every
  `_CCOL_EMULATE_DARWIN_*` switch that Linux can run. Because Valgrind does
  not run on macOS, this job is what checks the macOS-only code for leaks and
  memory errors.
- **Minimal build:** all optional modules turned off, plus a check that the
  library links only pthread and libm.
- **Coverage threshold:** `coverage_site` and `coverage_check`.
- **AddressSanitizer with UndefinedBehaviorSanitizer** (Clang).
- **ThreadSanitizer:** every `test_tsan` target, once with GCC, once with
  Clang, and once more with Clang and the `_CCOL_EMULATE_DARWIN_*` switches.
- **i386:** GCC and Clang builds of every suite, `check_abi` for i386, and an
  UndefinedBehaviorSanitizer run with both compilers.
- **AArch64 and ARM32:** cross builds with GCC and with Clang, every test
  binary run under QEMU, and `check_abi` for each architecture.
- **FreeBSD 14 x86-64:** every suite with the base Clang and with GCC 14,
  `memtest`, `check_abi`, `check_dso_unload`, `check_exit_reachable`,
  `check_install`, and the ThreadSanitizer targets.
- **macOS 15, arm64 and x86-64:** every suite with Apple clang, once as is and
  once with AddressSanitizer and UndefinedBehaviorSanitizer, plus every
  `test_tsan` target, `check_headers`, `check_namespace`, `check_abi`,
  `check_dso_unload`, `check_install`, and a build of the benchmarks.
- **Fuzzing:** every target listed above, for a limited time.

On a push to `main`, a separate workflow builds the merged coverage report
and publishes it.

## Reference

Related guides:
[Building and linking](building.md),
[Compatibility and versioning](compatibility.md),
[Platforms](platforms.md),
[bench/README.md](../bench/README.md)
