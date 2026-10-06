INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cyaml
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/chashmap.c \
            ../../src/cvector.c ../../src/csort.c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -gdwarf-4 -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS)

FUZZ_CC = clang
FUZZ_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=fuzzer,address,undefined -g3 -O1
FUZZ_SRC_FILES = fuzz/fuzz_cyaml.c $(SRC_FILES)

build: tests_locale
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

# The number_locale tests need a locale whose decimal point is ','. They use
# an installed one when there is one, and this private copy otherwise. The
# step is best effort: without localedef or the de_DE locale source, the
# directory stays empty and those tests print that they skipped.
tests_locale:
	mkdir -p tests_locale
	-localedef -c -i de_DE -f UTF-8 tests_locale/de_DE.UTF-8 >/dev/null 2>&1

# Standalone differential-testing helper (see differential/compare_pyyaml.py).
# It is not part of build, test, memtest or all. Run it by hand.
build_differential:
	$(CC) $(CFLAGS) differential/cyaml_to_json.c $(SRC_FILES) \
		-o differential/cyaml_to_json $(LFLAGS) -lm

# Separate binary that checks the accept and reject behavior of
# cyaml_parse_n against the vendored YAML Test Suite snapshot
# (yaml-test-suite/). It follows the "additional tests_<suffix>.c binary in
# an existing directory" convention of this project. It stays separate from
# tests.c, because it needs that vendored corpus on disk at a relative path.
# No other case in this directory needs such a corpus.
build_spec_suite:
	$(CC) $(CFLAGS) tests_spec_suite.c $(SRC_FILES) -o tests_spec_suite $(LFLAGS)


# Convenience aliases, so that the name of a binary that this suite produces
# also works as a target. Each alias builds that binary. `make test` builds
# and runs every one of them, and it stays the normal entry point.
.PHONY: tests_spec_suite
tests_spec_suite: build_spec_suite

# tests_c23.c compiles the value macros of this module as C23, where
# nullptr exists and `true` has the type bool. The main suite is C11 and can
# observe neither. The library sources build with the same flags, so the
# binary also shows that they compile as C23.
C23_CFLAGS = $(subst -std=gnu11,-std=gnu2x,$(CFLAGS))
build_c23:
	$(CC) $(C23_CFLAGS) tests_c23.c $(SRC_FILES) -o tests_c23 $(LFLAGS)

.PHONY: tests_c23
tests_c23: build_c23

# nesting_probe.c nests the value macro of this module inside its own
# argument. -Wshadow makes any local that a nested expansion hides a hard
# error, which only the compiler can report, so the probe is compiled and
# never run.
nesting_probe:
	$(CC) $(INCLUDES) $(DEFINITIONS) -std=gnu11 -Wall -Wextra -Wshadow -Werror \
		-fsyntax-only nesting_probe.c

test: build build_spec_suite build_c23 nesting_probe
	./tests
	./tests_spec_suite
	./tests_c23

# libFuzzer harness for cyaml_parse_n. It needs clang, because libFuzzer is
# a Clang and LLVM feature that GCC does not have. Run
# ./fuzz_cyaml [corpus_dir] [-max_total_time=N] and the other flags that the
# libFuzzer docs list. A bare `./fuzz_cyaml fuzz/corpus` mutates and grows
# the seed corpus that this repository holds.
# This target is .PHONY because a fuzz/ directory (the seed corpus) already
# exists on disk. Without .PHONY, Make treats a bare `fuzz` target as
# already satisfied and skips the recipe.
.PHONY: fuzz fuzz_path
fuzz:
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SRC_FILES) -o fuzz_cyaml $(LFLAGS)

# The dot-separated path grammar that cyaml_get, cyaml_set and cyaml_delete
# read is a second parser, and it MUTATES the DOM that the first one built.
# fuzz_cyaml never reaches it. This is the counterpart of the fuzz_path target
# of tests/cjson/Makefile, and it is .PHONY for the same reason as `fuzz`
# above: fuzz/corpus_path already exists on disk, so Make would otherwise read
# a bare `fuzz_path` as already satisfied. Run
# ./fuzz_cyaml_path fuzz/corpus_path [-max_total_time=N].
fuzz_path:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_cyaml_path.c $(SRC_FILES) \
		-o fuzz_cyaml_path $(LFLAGS)

memtest: build build_spec_suite build_c23
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_spec_suite
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_c23

# --ignore-errors negative: the coverage counters of gcov are plain
# increments that are not atomic. They can race when instrumented code runs
# concurrently, and they then give a negative hit count that is not real.
# This affects more than one module, and also code inside a glibc header. It
# is a limitation of the instrumentation of gcov, not a defect in the logic
# of this library. See the identical flag in tests/chttpclient/Makefile for
# the full rationale.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && echo 'GCOV SUCCEEDED, CALLING LCOV' && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

# The steps of `all` rebuild and run the same object files and binaries: the
# coverage step rebuilds them with instrumentation, and the test and memtest
# steps run the binaries that the build step links. Under make -j those steps
# would overlap and link or run a half-rebuilt mix. .NOTPARALLEL runs the
# prerequisites of `all` one after another, in the order listed, while the
# work inside each step still runs in parallel. A make older than 4.4 ignores
# the prerequisite list and runs this whole Makefile serially, which is also
# correct.
.NOTPARALLEL: all

clean:
	rm -rf tests tests_spec_suite tests_c23 tests_locale fuzz_cyaml fuzz_cyaml_path coverage *.gcno *.gcda *.gcov *.c.info \
		differential/cyaml_to_json

default: build
