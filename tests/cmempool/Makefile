INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cmempool
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage -DCCOL_TEST_COVERAGE
# -O1 here instead of the ordinary -O3, because what this build exercises is
# the ThreadSanitizer instrumentation. A higher level buys nothing and makes a
# report harder to map back to the source.
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)
# The wrap sends every pthread_mutex_lock that the library and the tests make
# through a counter in tests.c, which then calls the real function. The
# thread-cache tests assert on how many locks an operation takes, which depends
# on the code and not on the machine.
LFLAGS = -lpthread $(call ccol_ld_wrap,pthread_mutex_lock) $(CCOL_PLATFORM_LIBS) $(CCOL_DL_LIBS)

# tests_unload.c runs what the module runs when it unloads, and that deletes
# the thread cache's thread-specific key for the rest of the process, so it gets
# a binary of its own.
build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)
	$(CC) $(CFLAGS) tests_unload.c $(SRC_FILES) -o tests_unload $(CCOL_PLATFORM_LIBS)

build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)
	$(CC) $(TSAN_CFLAGS) tests_unload.c $(SRC_FILES) -o tests_unload_tsan $(CCOL_PLATFORM_LIBS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test: build
	./tests
	./tests_unload

# die_after_fork=0 is needed because several tests here fork and the child then
# runs into a _ccol_assert() abort that the test expects. With the
# ThreadSanitizer default such a child would exit silently, and a test of a
# fatal path would end up checking nothing.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan
	./tests_unload_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_unload

# --ignore-errors negative is there because gcov's coverage counters are
# plain, non-atomic increments, so instrumented code running on more than one
# thread at a time can race on them and end up with a negative hit count that
# isn't real. It happens in more than one module and inside a glibc header as
# well; it's a limit of the gcov instrumentation, not a bug in the library's
# logic. tests/chttpclient/Makefile has the same flag with the full reason.
# genhtml gets --ignore-errors inconsistent because at -O3 the compiler
# inlines a function into a caller in the same file, so gcov counts its lines
# as run while the out-of-line copy, which this suite never calls, has no
# calls (ccol_procs_is_interned here).
# Every binary that `test` runs is built and run here too, with the same
# instrumentation, so the tracefile counts what each of them reaches.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) tests_unload.c $(SRC_FILES) -o tests_unload $(CCOL_PLATFORM_LIBS) && \
	./tests_unload && \
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test test_tsan memtest generate_coverage_report

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
	rm -rf tests tests_tsan tests_unload tests_unload_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
