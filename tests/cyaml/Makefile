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
# an installed one if there is one and this private copy otherwise. The step is
# best effort: without localedef (macOS has none) or the de_DE locale source
# the directory stays empty, and those tests use an installed locale or print
# that they skipped.
tests_locale:
	mkdir -p tests_locale
	@if command -v localedef >/dev/null 2>&1; then \
	  localedef -c -i de_DE -f UTF-8 tests_locale/de_DE.UTF-8 >/dev/null 2>&1 \
	    || true; \
	fi

# Standalone differential-testing helper (see differential/compare_pyyaml.py).
# It isn't part of build, test, memtest or all, you run it by hand.
build_differential:
	$(CC) $(CFLAGS) differential/cyaml_to_json.c $(SRC_FILES) \
		-o differential/cyaml_to_json $(LFLAGS) -lm

# A separate binary that checks what cyaml_parse_n accepts and rejects against
# the vendored YAML Test Suite snapshot (yaml-test-suite/), following this
# project's convention of an extra tests_<suffix>.c binary in an existing
# directory. It's kept apart from tests.c because it needs that vendored corpus
# on disk at a relative path, and nothing else in this directory does.
build_spec_suite:
	$(CC) $(CFLAGS) tests_spec_suite.c $(SRC_FILES) -o tests_spec_suite $(LFLAGS)


# Convenience aliases, so the name of each binary this suite builds also works
# as a target. An alias only builds its binary, while `make test` builds and
# runs all of them and is the normal entry point.
.PHONY: tests_spec_suite
tests_spec_suite: build_spec_suite

# tests_keys.c uses up every thread-specific key of the process before the
# first cyaml call, so no thread's node pool can ever arm its key. The key is
# created once per process, so this case needs a process of its own.
build_keys:
	$(CC) $(CFLAGS) tests_keys.c $(SRC_FILES) -o tests_keys $(LFLAGS)

.PHONY: tests_keys
tests_keys: build_keys

# tests_c23.c compiles this module's value macros as C23, where nullptr exists
# and `true` has the type bool; the main suite is C11 and sees neither. The
# library sources are built with the same flags, so the binary also shows that
# they compile as C23.
C23_CFLAGS = $(subst -std=gnu11,-std=gnu2x,$(CFLAGS))
build_c23:
	$(CC) $(C23_CFLAGS) tests_c23.c $(SRC_FILES) -o tests_c23 $(LFLAGS)

.PHONY: tests_c23
tests_c23: build_c23

# nesting_probe.c nests this module's value macro inside its own argument.
# -Wshadow turns any local that a nested expansion hides into a hard error, and
# only the compiler can report that, so the probe is compiled but never run.
nesting_probe:
	$(CC) $(INCLUDES) $(DEFINITIONS) -std=gnu11 -Wall -Wextra -Wshadow -Werror \
		-fsyntax-only nesting_probe.c

test: build build_spec_suite build_c23 build_keys nesting_probe
	./tests
	./tests_spec_suite
	./tests_c23
	./tests_keys

# libFuzzer harness for cyaml_parse_n. It needs clang, because libFuzzer is a
# Clang and LLVM feature that GCC doesn't have. Run
# ./fuzz_cyaml [corpus_dir] [-max_total_time=N] with any other flags from the
# libFuzzer docs; a bare `./fuzz_cyaml fuzz/corpus` mutates and grows the seed
# corpus kept in this repository.
# The target is .PHONY because a fuzz/ directory (the seed corpus) exists on
# disk, and without .PHONY Make would take a bare `fuzz` target as already
# satisfied and skip the recipe.
.PHONY: fuzz fuzz_path
fuzz:
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SRC_FILES) -o fuzz_cyaml $(LFLAGS)

# The dot-separated path grammar that cyaml_get, cyaml_set and cyaml_delete
# read is a second parser, it MUTATES the DOM that the first one built, and
# fuzz_cyaml never gets there. It's the counterpart of the fuzz_path target of
# tests/cjson/Makefile and it's .PHONY for the same reason as `fuzz` above:
# fuzz/corpus_path exists on disk, so Make would otherwise take a bare
# `fuzz_path` as already satisfied. Run
# ./fuzz_cyaml_path fuzz/corpus_path [-max_total_time=N].
fuzz_path:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_cyaml_path.c $(SRC_FILES) \
		-o fuzz_cyaml_path $(LFLAGS)

memtest: build build_spec_suite build_c23 build_keys
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_spec_suite
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_c23
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_keys

# --ignore-errors negative is there because gcov's coverage counters are
# plain, non-atomic increments, so instrumented code running on more than one
# thread at a time can race on them and end up with a negative hit count that
# isn't real. It happens in more than one module and inside a glibc header as
# well; it's a limit of the gcov instrumentation, not a bug in the library's
# logic. tests/chttpclient/Makefile has the same flag with the full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && echo 'GCOV SUCCEEDED, CALLING LCOV' && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

# The steps of `all` rebuild and run the same object files and binaries. The
# coverage step rebuilds them with instrumentation, and test and memtest run
# the binaries that the build step links, so under make -j the steps would
# overlap and link or run a half-rebuilt mix. .NOTPARALLEL runs the
# prerequisites of `all` one after another in the order listed, while the work
# inside each step still runs in parallel. A make older than 4.4 ignores the
# prerequisite list and runs the whole Makefile serially, which is correct
# too.
.NOTPARALLEL: all

clean:
	rm -rf tests tests_spec_suite tests_c23 tests_keys tests_locale fuzz_cyaml fuzz_cyaml_path coverage *.gcno *.gcda *.gcov *.c.info \
		differential/cyaml_to_json

default: build
