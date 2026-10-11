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
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage 
# This build uses -O1 instead of the -O3 of the ordinary build, because what
# it exercises is the instrumentation of ThreadSanitizer: a higher level buys
# nothing here and makes a report harder to read back to source.
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)
# The wrap routes every pthread_mutex_lock that the library and the tests
# make through a counter in tests.c, which forwards to the real function. The
# thread-cache tests assert on how many locks an operation takes, which is a
# property of the code and not of the machine.
LFLAGS = -lpthread $(call ccol_ld_wrap,pthread_mutex_lock) $(CCOL_PLATFORM_LIBS)

# tests_unload.c runs what the module runs when it unloads, which deletes the
# thread-specific key of the thread cache for the rest of the process, so it
# is a binary of its own.
build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)
	$(CC) $(CFLAGS) tests_unload.c $(SRC_FILES) -o tests_unload $(CCOL_PLATFORM_LIBS)

build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)
	$(CC) $(TSAN_CFLAGS) tests_unload.c $(SRC_FILES) -o tests_unload_tsan $(CCOL_PLATFORM_LIBS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test: build
	./tests
	./tests_unload

# die_after_fork=0 is needed because several tests here fork, and the child
# then runs to a ccol_assert() abort that the test expects. The default of
# ThreadSanitizer would instead exit such a child in silence, and a test that
# verifies a fatal path would then verify nothing.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan
	./tests_unload_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_unload

# --ignore-errors negative is needed because the coverage counters of gcov
# are plain, non-atomic increments: they can race when instrumented code runs
# on several threads, and then report a negative hit count that is not real.
# This affects more than one module and also happens inside a glibc header;
# it is a limit of the gcov instrumentation, not a defect in the logic of
# this library. See the identical flag in tests/chttpclient/Makefile for the
# full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test test_tsan memtest generate_coverage_report

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
	rm -rf tests tests_tsan tests_unload tests_unload_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
