INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cthreadcomm
# chashmap.c is a transitive dependency. The internal fd registry of
# ccol_event_loop (int fd -> event_entry*) uses a chmap. cthreadpool.c is a
# transitive dependency too. The dispatch_pool of ccol_event_loop
# (num_reactor_threads > 1) is a ctpool. cvector.c is also necessary, because
# ccol_event_loop has its own generation-tagged handle slot table. That table
# is a cvec of slots plus a cvec free-index stack. csort.c comes in through
# cvector.c, whose sort runs on the mergesort of csort.
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
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS)

# tests_keys uses up every thread-specific key of the process before the
# first ccol_event_loop call, so it needs a process of its own too.
# tests_exit_order runs in a process of its own, because it checks the
# exit-time destructor order of a whole process: an application destructor
# that runs after the destructors of cthreadcomm and still owns live queues
# and a live loop. Its source comes first on the link line, which puts its
# destructor last in the .fini_array walk.
build: build_main build_exit_order build_keys

build_main:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

build_exit_order:
	$(CC) $(CFLAGS) tests_exit_order.c $(SRC_FILES) -o tests_exit_order $(LFLAGS)

build_keys:
	$(CC) $(CFLAGS) tests_keys.c $(SRC_FILES) -o tests_keys $(LFLAGS)

# This is a separate ThreadSanitizer build. It stays out of the default
# -Werror build. TSan instrumentation and -O3 do not work well together.
# This build is also a different verification mode, and it runs separately.
build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test: build
	./tests
	./tests_exit_order
	./tests_keys

# die_after_fork=0: the ccol_event_loop_handle_lifecycle tests of this suite
# fork and create new pthreads in the child. The test_tsan target of
# tests/chttpclient/Makefile has a longer comment on the exact TSan mechanism.
# This module destroys its ccol_event_loop instances between tests. The parent
# is therefore single-threaded at fork() time. The option is not load-bearing
# here. It is load-bearing for the persistent, process-wide reactors of
# chttpclient and chttpserver. The option is set for consistency across all
# three suites that fork and then start new threads. It also protects against
# a future test that makes this parent multi-threaded at fork time.
# suppressions=tsan_rwlock_reinit.supp: this is a known ThreadSanitizer
# limitation, and it is confirmed to be a non-bug. It happens around the
# rwlock reinit that gives this module fork() safety. The comment in that file
# shows the independent reproduction that proves it, and that reproduction
# uses no code from cthreadcomm.c. tests/clogger/Makefile has the same
# precedent: a known non-bug that a suppression hides for a documented reason.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_rwlock_reinit.supp" ./tests_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests_exit_order
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_keys

# --ignore-errors negative: the coverage counters of gcov are plain increments
# and are not atomic. They can race when instrumented code runs concurrently.
# They then produce a false negative hit count. This affects more than one
# module, and it also happens inside a glibc header. It is a limitation of the
# instrumentation of gcov. It is not a defect in the logic of this library.
# tests/chttpclient/Makefile uses the same flag and gives the full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build build_tsan test test_tsan memtest generate_coverage_report

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
	rm -rf tests tests_exit_order tests_keys tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
