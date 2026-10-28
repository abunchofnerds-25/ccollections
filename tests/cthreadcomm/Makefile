INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cthreadcomm
# chashmap.c is pulled in indirectly because the internal fd registry of
# ccol_event_loop (int fd -> event_entry*) is a chmap. cthreadpool.c comes in
# the same way, since the dispatch_pool of ccol_event_loop
# (num_reactor_threads > 1) is a ctpool. cvector.c is needed as well, because
# ccol_event_loop has its own generation-tagged handle slot table, a cvec of
# slots plus a cvec free-index stack. csort.c comes in through cvector.c, whose
# sort is the mergesort of csort.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c \
            ../../src/chashmap.c ../../src/cthreadpool.c \
            ../../src/cvector.c ../../src/csort.c ../../src/common.c \
            ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage -DCCOL_TEST_COVERAGE
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS)

# tests_keys uses up every thread-specific key of the process before the
# first ccol_event_loop call, so it needs a process of its own too.
# tests_exit_order runs in a process of its own because it checks the exit-time
# destructor order of a whole process: an application destructor that runs
# after the destructors of cthreadcomm and still owns live queues and a live
# loop at that point. Its source is first on the link line, which puts its
# destructor last in the .fini_array walk.
build: build_main build_exit_order build_keys

build_main:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

build_exit_order:
	$(CC) $(CFLAGS) tests_exit_order.c $(SRC_FILES) -o tests_exit_order $(LFLAGS)

build_keys:
	$(CC) $(CFLAGS) tests_keys.c $(SRC_FILES) -o tests_keys $(LFLAGS)

# A separate ThreadSanitizer build, kept out of the default -Werror build. TSan
# instrumentation and -O3 don't get along well, and this is also a different
# verification mode that runs on its own.
build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test: build
	./tests
	./tests_exit_order
	./tests_keys

# die_after_fork=0: the ccol_event_loop_handle_lifecycle tests of this suite
# fork and create new pthreads in the child; the test_tsan target of
# tests/chttpclient/Makefile explains the exact TSan mechanism at more length.
# This module destroys its ccol_event_loop instances between tests, so the
# parent is single-threaded when it calls fork() and the option doesn't carry
# any weight here, while it does for the persistent, process-wide reactors of
# chttpclient and chttpserver. We set it anyway so the three suites that fork
# and then start new threads are consistent, and so a later test that leaves
# this parent multi-threaded at fork time is covered too.
# suppressions=tsan_rwlock_reinit.supp: one "thread leak" that ThreadSanitizer
# reports in the child of one fork test, for a thread that the parent joins.
# The comment in that file gives the reason and the independent reproduction.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_rwlock_reinit.supp" ./tests_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests_exit_order
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_keys

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
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) tests_exit_order.c $(SRC_FILES) -o tests_exit_order $(LFLAGS) && \
	./tests_exit_order && \
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) tests_keys.c $(SRC_FILES) -o tests_keys $(LFLAGS) && \
	./tests_keys && \
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build build_tsan test test_tsan memtest generate_coverage_report

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
	rm -rf tests tests_exit_order tests_keys tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
