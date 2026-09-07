CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS
SRC_FILE_PREFIX = cjson
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
LFLAGS =

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

test: build
	./tests

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests

# libFuzzer targets for cjson.c's two independent hand-written parsers: the
# JSON grammar itself (fuzz_cjson_parse, also exercising the serializer and
# cjson_clone() on whatever the parser accepts) and the dot-separated path
# grammar consumed by cjson_get/cjson_set/cjson_delete (fuzz_cjson_path).
# Requires clang (libFuzzer is a Clang/LLVM feature). Mirrors
# tests/cyaml/Makefile's and tests/chttpclient/Makefile's own `fuzz`/
# `fuzz_request`/`fuzz_response` targets, including their .PHONY rationale: a
# fuzz/corpus_parse or fuzz/corpus_path directory already exists on disk,
# which would otherwise make Make consider a bare `fuzz_parse`/`fuzz_path`
# target already satisfied and skip the recipe entirely. Run
# ./fuzz_cjson_parse fuzz/corpus_parse [-max_total_time=N] etc; see the
# libFuzzer docs for flags.
FUZZ_CC = clang
FUZZ_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=fuzzer,address,undefined -g3 -O1
FUZZ_SRC_FILES = $(SRC_FILES)

.PHONY: fuzz_parse fuzz_path
fuzz_parse:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_cjson_parse.c $(FUZZ_SRC_FILES) \
		-o fuzz_cjson_parse $(LFLAGS)

fuzz_path:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_cjson_path.c $(FUZZ_SRC_FILES) \
		-o fuzz_cjson_path $(LFLAGS)

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
	rm -rf tests coverage *.gcno *.gcda *.gcov *.c.info fuzz_cjson_parse fuzz_cjson_path

default: build
