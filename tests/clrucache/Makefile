CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = clrucache
# cthreadpool.c is a transitive dependency of cthreadcomm.c: ccol_event_loop's
# dispatch_pool (num_reactor_threads > 1) is a ctpool. cvector.c is needed
# for two reasons: cthreadcomm.c links it (ccol_event_loop's own
# generation-tagged handle slot table) and clrucache.c needs it directly
# for its own, separate slot table of the same shape.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/chashmap.c \
            ../../src/cthreadcomm.c ../../src/cthreadpool.c \
            ../../src/cvector.c ../../src/common.c ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1
LFLAGS = -lpthread

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

test: build
	./tests

# The cache resolves its handle without a lock, so a race on that path would
# not surface as a failed assertion in an ordinary run. die_after_fork=0 because
# this suite forks and then creates threads in the child, which ThreadSanitizer
# otherwise exits silently instead of letting it reach the abort it is checking
# for.
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
	gcov -d -w tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test test_tsan memtest generate_coverage_report

clean:
	rm -rf tests tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
