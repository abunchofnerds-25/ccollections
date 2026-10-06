INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cjson
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/chashmap.c \
            ../../src/cvector.c ../../src/csort.c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -gdwarf-4 -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = $(CCOL_PLATFORM_LIBS)
build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

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

test: build build_c23 nesting_probe
	./tests
	./tests_c23

memtest: build build_c23
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_c23

# libFuzzer targets for the two hand-written parsers of cjson.c. Each one is
# independent of the other. The first is the JSON grammar itself, which
# fuzz_cjson_parse drives. That target also drives the serializer and
# cjson_clone() over whatever the parser accepts. The second is the
# dot-separated path grammar that cjson_get, cjson_set and cjson_delete read,
# which fuzz_cjson_path drives. Both targets need clang, because libFuzzer is
# a Clang and LLVM feature. The `fuzz` target of tests/cyaml/Makefile and the
# `fuzz_request` and `fuzz_response` targets of tests/chttpclient/Makefile
# have the same shape, and the same reason for .PHONY. A fuzz/corpus_parse
# directory and a fuzz/corpus_path directory already exist on disk. Without
# .PHONY, Make reads a bare `fuzz_parse` or `fuzz_path` target as already
# satisfied and skips the recipe completely. Run ./fuzz_cjson_parse
# fuzz/corpus_parse [-max_total_time=N], and the other target in the same way.
# The libFuzzer documentation lists the flags.
FUZZ_CC = clang
FUZZ_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=fuzzer,address,undefined -g3 -O1
FUZZ_SRC_FILES = $(SRC_FILES)

.PHONY: fuzz_parse fuzz_path
fuzz_parse:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_cjson_parse.c $(FUZZ_SRC_FILES) \
		-o fuzz_cjson_parse $(LFLAGS)

fuzz_path:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_cjson_path.c $(FUZZ_SRC_FILES) \
		-o fuzz_cjson_path $(LFLAGS)

# --ignore-errors negative: the coverage counters of gcov are plain increments
# and are not atomic. Instrumented code that runs on several threads can
# therefore race on them and produce a false negative hit count. This affects
# more than one module, and it also happens inside a glibc header. It is a
# limit of the instrumentation of gcov, and not a defect in the logic of this
# library. The same flag in tests/chttpclient/Makefile gives the full
# reason.
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
	rm -rf tests tests_c23 coverage *.gcno *.gcda *.gcov *.c.info fuzz_cjson_parse fuzz_cjson_path

default: build
