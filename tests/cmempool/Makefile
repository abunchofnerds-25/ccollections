CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cmempool
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage 
# -O1 rather than the -O3 the ordinary build uses: ThreadSanitizer's own
# instrumentation is what is being exercised here, and the higher level buys
# nothing while making a report harder to read back to source.
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1
LFLAGS = -lpthread

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

test: build
	./tests

# die_after_fork=0: several tests here fork and the child then runs to an
# expected ccol_assert() abort. TSan's default would exit such a child silently
# instead, turning a test that verifies a fatal path into one that verifies
# nothing.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests

# --ignore-errors negative: gcov's own coverage counters are plain (non-atomic)
# increments, which can race under concurrently-executed instrumented code and
# produce a spurious negative hit count. This affects more than one module,
# including inside a glibc header, and is a limitation of gcov's own
# instrumentation rather than a defect in this library's logic; see
# tests/chttpclient/Makefile's identical flag for the full rationale.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test test_tsan memtest generate_coverage_report

clean:
	rm -rf tests tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
