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
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage -DCCOL_TEST_COVERAGE
LFLAGS = $(CCOL_PLATFORM_LIBS)
build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

# tests_keys.c uses up every thread-specific key of the process before the
# first cjson call, so no thread's node pool can ever arm its key. The key is
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

test: build build_c23 build_keys nesting_probe
	./tests
	./tests_c23
	./tests_keys

memtest: build build_c23 build_keys
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_c23
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_keys

# libFuzzer targets for the two hand-written parsers of cjson.c, which don't
# depend on each other. fuzz_cjson_parse drives the JSON grammar itself, plus
# the serializer and cjson_clone() over whatever the parser accepts.
# fuzz_cjson_path drives the dot-separated path grammar that cjson_get,
# cjson_set and cjson_delete read. Both need clang, because libFuzzer is a Clang
# and LLVM feature. The `fuzz` target of tests/cyaml/Makefile and the
# `fuzz_request` and `fuzz_response` targets of tests/chttpclient/Makefile have
# the same shape and the same reason for .PHONY: a fuzz/corpus_parse and a
# fuzz/corpus_path directory exist on disk, and without .PHONY Make takes a
# bare `fuzz_parse` or `fuzz_path` target as already satisfied and skips the
# recipe completely. Run ./fuzz_cjson_parse fuzz/corpus_parse
# [-max_total_time=N], and the other target the same way; the libFuzzer
# documentation lists the flags.
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

# --ignore-errors negative is there because gcov's coverage counters are
# plain, non-atomic increments, so instrumented code running on more than one
# thread at a time can race on them and end up with a negative hit count that
# isn't real. It happens in more than one module and inside a glibc header as
# well; it's a limit of the gcov instrumentation, not a bug in the library's
# logic. tests/chttpclient/Makefile has the same flag with the full reason.
# Every binary that `test` runs is built and run here too, with the same
# instrumentation, so the tracefile counts what each of them reaches.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	$(CC) $(COVERAGE_FLAGS) $(C23_CFLAGS) tests_c23.c $(SRC_FILES) -o tests_c23 $(LFLAGS) && \
	./tests_c23 && \
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) tests_keys.c $(SRC_FILES) -o tests_keys $(LFLAGS) && \
	./tests_keys && \
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
	rm -rf tests tests_c23 tests_keys coverage *.gcno *.gcda *.gcov *.c.info fuzz_cjson_parse fuzz_cjson_path

default: build
