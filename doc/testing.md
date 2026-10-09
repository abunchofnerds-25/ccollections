# Testing, fuzzing and benchmarks

This guide is for contributors. It shows how to run these checks:

- the test suites
- the sanitizer builds and the Valgrind builds
- coverage
- the interface checks
- the fuzzers
- the benchmarks

It also lists the jobs that CI runs on each push. Run each command on this
page from the root of the repository, unless the text gives a different
directory.

## Before you start

On Debian or Ubuntu, install the packages that the full set of checks
needs:

```bash
sudo apt-get install build-essential clang libssl-dev zlib1g-dev \
    valgrind lcov abigail-tools
```

`make test` needs only the compiler, OpenSSL and zlib. `make memtest` needs
Valgrind. Coverage needs lcov. The fuzzers need Clang. The structural half
of `make check_abi` needs `abigail-tools`. On FreeBSD, use `gmake` in all
locations where this page says `make`. Also install `binutils` for the
interface checks, because they need GNU `readelf` and GNU `nm`.

## Running the tests

```bash
make test       # build and run all suites
make memtest    # all suites under Valgrind; a leak causes a failure
```

Each module has its own suite under `tests/<module>/`. Some directories
build more than one binary (for example, a TLS suite next to the main
suite). The two targets run each binary of each directory, and they stop at
the first failure. `tests/mixed/` contains tests that use more than one
module. This includes the compatibility corpus that
[Compatibility](compatibility.md) describes.

To work on one module, run only its suite:

```bash
make -C tests/chashmap test
make -C tests/chashmap memtest
```

The suites use the single-header framework in `tests/tau/`. A test binary
accepts a filter. The filter compares from the start of `suite.test`. First,
list the names. Then use a prefix that is shorter than the full name:

```bash
cd tests/chashmap && make build
./tests --list | head
./tests --filter='chash_maps.insert*'
```

Read the count on the `SUCCESS` line of a filtered run. A filter that finds
no test also reports success, with 0 test suites passed.

The suites compile the library sources directly into each test binary. They
use the same `-O3` as the shipped library. Therefore, they test the code
that ships. `-DRUNNING_UNIT_TESTS` enables some white-box helpers that only
the suites use.

**Run only one full suite at a time.** `tests/chttpserver` binds a fixed
port. Therefore, two `make test` or `make memtest` runs on the same machine
use the same port, and they fail with
`listen socket setup failed ... port=18765`. You can run the suite of one
module together with a full run, if that module is not `chttpserver`.

## Sanitizers

Give AddressSanitizer and UndefinedBehaviorSanitizer through
`EXTRA_CFLAGS`. All suites use this variable:

```bash
make CC=clang EXTRA_CFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined" test
```

ThreadSanitizer has its own target in the modules that run threads. These
modules are `cpintable`, `cmempool`, `clrucache`, `mixed`, `clogger`,
`cthreadpool`, `cthreadcomm`, `ctls`, `chttpclient` and `chttpserver`. Run
each target with GCC and with Clang. The reason is that the ThreadSanitizer
of GCC does not find all the races that the ThreadSanitizer of Clang finds:

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
the code of one of these switches, run the suites that test it:

```bash
make -C tests/cmempool clean
make -C tests/cmempool EXTRA_CFLAGS="-DCCOL_MEMPOOL_COMPACT_LAYOUT=1" test

make clean
make EXTRA_CFLAGS="-DCCOL_FORK_SAFETY_REQUIRED=0" test
```

A build without the optional modules is also a variant:

```bash
make WITH_CJSON=0 WITH_CYAML=0 WITH_CLOGGER=0 \
     WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0 test
```

## Coverage

```bash
make coverage_site    # build all suites again with coverage, merge one report
make coverage_check   # fail if a file has less than 80 percent of its lines
```

`coverage_site` writes a report that you can read in a browser to
`coverage_site/html`. It writes a summary to `coverage_site/summary.txt`. It
merges the runs of all suites, because many suites test a file such as
`src/common.c`. The figures include only `src/` and `include/`.
`coverage_check` reads the merged data. Therefore, run `coverage_site`
first. `ci_scripts/check_test_coverages.sh` contains the threshold, and the
short list of files that have no tests of their own.

## Checks on the built library

The suites link the library sources directly. Therefore, they never see the
symbols that the shared library exports. They also never see what occurs
when a program loads and unloads the library. These targets check the built
artefacts:

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
make hardening_report      # the hardening flags that this compiler accepts
```

[Compatibility](compatibility.md#how-the-promise-is-checked) explains
`check_abi` and `update_abi_baseline`. In summary: you can add a public
function, but you must record the updated baseline and commit it with the
change. Each other difference is a failure that you must explain.

## Fuzzing

Seven libFuzzer targets test the parsers and the URL layer of the HTTP
client. They need Clang, and they are not part of `make test`. CI runs them
on each push and each pull request. Therefore, you need them only when you
work on one of those parsers.

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

If you do not give `-max_total_time`, the fuzzer runs until you push
Ctrl-C. The fuzzer adds each input that it finds to the corpus directory.
Therefore, `git status` shows these inputs after the run. The fuzzer writes
each finding to a `crash-*`, `timeout-*` or `oom-*` file. To run a finding
again in the same way, give that file as the only argument:

```bash
./fuzz_cyaml crash-<hash>
```

## Benchmarks

`bench/` measures the modules whose speed is important:

- the containers, the strings and the sort
- the memory pools and the LRU cache
- the thread pool and the queues
- the logger and the two serializers
- an HTTP round trip

It links with the `libccollections.so` that the root `make` builds.
Therefore, it measures the shipped code with the shipped flags.

```bash
make bench_update      # record a baseline for this machine
make bench             # run all cases and compare them with that baseline
make bench_calibrate   # record a baseline and measure how much each case
                       # changes between runs on this machine
make bench_gate        # the same as bench, but exit non-zero on a flagged case
make bench_list        # list the cases
```

Give options through `BENCH_ARGS`:

```bash
make bench BENCH_ARGS="--filter=chashmap --reps=15"
make bench BENCH_ARGS="--threshold=10"
make bench_calibrate BENCH_ARGS="--calibrate=7"
```

Remember these points when you read the numbers:

- **The baseline is for one machine.** The project does not commit it.
  Compare the same machine before and after a change. A figure from
  different hardware tells you nothing about the library.
- **`make bench` reports. It does not fail.** `bench_calibrate` measures how
  much each case changes alone. `bench_gate` changes a flagged case into a
  failure. Use the gate only where the calibration shows stable figures.
- **Each figure is a median** of nanoseconds for each operation, over the
  repetitions. The clock does not include the setup and the teardown.
- **Cases with a name that ends in `_4t`, `_8t` or `_12t`** run the same
  work on that number of threads. They report the wall time divided by all
  operations. Therefore, you can compare them directly with the
  single-threaded case. Thread-safe modules share one instance between the
  threads. For the unguarded containers, each thread has its own instance.
- **Cases with a library name in brackets** (`[malloc]`, `[glib]`,
  `[uthash]`, `[jansson]`, `[libyaml]`) run the same workload through that
  library, if it is installed. Compare the ratios in one run, not the raw
  times.

[bench/README.md](../bench/README.md) is the full guide. It tells you what
each column means, how to get a run that you can trust, and how to add a
case. [bench/RESULTS.md](../bench/RESULTS.md) records example runs and the
machines that made them. CI builds the benchmarks and lists their cases, but
it never measures their times. The reason is that a shared virtual runner
has much more noise than the changes that the benchmarks must find.

## What CI runs

Each push and each pull request runs these jobs. Each job blocks a merge
when it fails:

- **Linux x86-64, GCC:** `make test`, `make memtest`, `check_namespace`,
  `check_headers`, `check_abi`, `check_filenames`, `check_dso_unload`, the
  compact mempool layout, the general thread-local storage model, a full run
  with `CCOL_FORK_SAFETY_REQUIRED=0`, and a build of the benchmarks.
- **Linux x86-64, Clang:** `make test` and `make memtest`.
- **Linux x86-64, macOS code paths:** `make memtest` with every
  `_CCOL_EMULATE_DARWIN_*` switch that Linux can run. Valgrind does not run
  on macOS, so this job checks the code that only macOS compiles for leaks
  and memory errors.
- **Minimal build:** all optional modules off, and a check that the library
  links only pthread and libm.
- **Coverage threshold:** `coverage_site` and `coverage_check`.
- **AddressSanitizer with UndefinedBehaviorSanitizer** (Clang).
- **ThreadSanitizer:** each `test_tsan` target, one time with GCC and one
  time with Clang.
- **i386:** GCC and Clang builds of each suite, `check_abi` for i386, and an
  UndefinedBehaviorSanitizer run with the two compilers.
- **AArch64 and ARM32:** cross builds with GCC and with Clang, each test
  binary run under QEMU, and `check_abi` for each architecture.
- **FreeBSD 14 x86-64:** each suite with the base Clang and with GCC 14,
  `memtest`, `check_abi`, `check_dso_unload`, and the ThreadSanitizer
  targets.
- **Fuzzing:** each target above, for a limited time.

On a push to `main`, a separate workflow builds the merged coverage report
and publishes it.

## Reference

Related guides:
[Building and linking](building.md),
[Compatibility and versioning](compatibility.md),
[Platforms](platforms.md),
[bench/README.md](../bench/README.md)
