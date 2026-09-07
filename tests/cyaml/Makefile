CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS
SRC_FILE_PREFIX = cyaml
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/chashmap.c \
            ../../src/cvector.c ../../src/csort.c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -gdwarf-4 -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread

FUZZ_CC = clang
FUZZ_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=fuzzer,address,undefined -g3 -O1
FUZZ_SRC_FILES = fuzz/fuzz_cyaml.c $(SRC_FILES)

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

# Standalone differential-testing helper (see differential/compare_pyyaml.py);
# not part of build/test/memtest/all, run manually. -DRUNNING_UNIT_TESTS
# comes from $(DEFINITIONS) above, which this helper relies on for
# cyaml_debug_dictionary_key_at (see that function's own doc comment in
# src/cyaml.c).
build_differential:
	$(CC) $(CFLAGS) differential/cyaml_to_json.c $(SRC_FILES) \
		-o differential/cyaml_to_json $(LFLAGS) -lm

# Separate binary (matches this project's own "additional tests_<suffix>.c
# binary in an existing directory" convention) checking cyaml_parse_n's
# accept/reject behavior against the vendored YAML Test Suite snapshot
# (yaml-test-suite/). Kept separate from tests.c since it depends on that
# vendored corpus being present on disk at a relative path, unlike every
# other case in this directory.
build_spec_suite:
	$(CC) $(CFLAGS) tests_spec_suite.c $(SRC_FILES) -o tests_spec_suite $(LFLAGS)

test: build build_spec_suite
	./tests
	./tests_spec_suite

# libFuzzer harness for cyaml_parse_n. Requires clang (libFuzzer is a
# Clang/LLVM feature, not available via GCC). Run ./fuzz_cyaml [corpus_dir]
# [-max_total_time=N] etc; see the libFuzzer docs for flags. A bare
# `./fuzz_cyaml fuzz/corpus` mutates and grows the checked-in seed corpus.
# .PHONY because a fuzz/ directory (the seed corpus) already exists on disk,
# which would otherwise make Make consider a bare `fuzz` target already
# satisfied and skip the recipe entirely.
.PHONY: fuzz
fuzz:
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SRC_FILES) -o fuzz_cyaml $(LFLAGS)

memtest: build build_spec_suite
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_spec_suite

# --ignore-errors negative: gcov's own coverage counters are plain (non-atomic)
# increments, which can race under concurrently-executed instrumented code and
# has been observed producing a spurious negative hit count (reproduced on
# more than one module, including a glibc header, not this library's own
# logic); see tests/chttpclient/Makefile's identical flag for the full
# rationale and reproduction notes.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && echo 'GCOV SUCCEEDED, CALLING LCOV' && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests tests_spec_suite fuzz_cyaml coverage *.gcno *.gcda *.gcov *.c.info \
		differential/cyaml_to_json

default: build
