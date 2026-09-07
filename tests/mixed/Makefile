CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS
SRC_FILES = ../../src/cvector.c \
            ../../src/chashmap.c \
            ../../src/cbstmap.c \
            ../../src/cstring.c \
            ../../src/common.c \
            ../../src/csort.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

test: build
	./tests

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests

# --ignore-errors negative: gcov's own coverage counters are plain (non-atomic)
# increments, which can race under concurrently-executed instrumented code and
# has been observed producing a spurious negative hit count (reproduced on
# more than one module, including a glibc header, not this library's own
# logic); see tests/chttpclient/Makefile's identical flag for the full
# rationale and reproduction notes.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-mixed && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file mixed.c.info && \
	genhtml --ignore-errors inconsistent mixed.c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest

clean:
	rm -rf tests coverage *.gcda *.gcdo *.gcno *.gcov

default: build
