INCLUDES = -I.. -I../../include
# CDEBUGLOG_ENABLED opts this suite into the diagnostic log buffer of
# src/cdebuglog.c (the doc comment in include/internal/cdebuglog.h explains
# why RUNNING_UNIT_TESTS alone doesn't turn it on). It backs the [DEBUG_TEST]
# checkpoints of resolve_then_use_race_destroy_waits, a test with an open
# hang that shows up only in CI and only under qemu, and those checkpoints
# are what let us diagnose another such hang.
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64 -DCDEBUGLOG_ENABLED
SRC_FILE_PREFIX = cthreadpool
# ctpool needs cvector.c for its generation-tagged handle slot table, the
# same mechanism that chttpcli, chttpsvr, ccol_event_loop and clru_cache use.
# cdebuglog.c is an opt-in diagnostic log buffer that only a
# RUNNING_UNIT_TESTS build has, and the root Makefile's default library build
# leaves it out (see its doc comment in include/internal/cdebuglog.h).
# csort.c comes in through cvector.c, whose sort runs on csort's mergesort.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/cdebuglog.c \
            ../../src/cvector.c ../../src/csort.c ../../src/common.c \
            ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
# The library sources are built at the optimization level of the shipped
# library, so these tests exercise the code that ships and not a debug build
# that nobody installs. Only the test file of this directory is built at
# -O0, where being able to debug it is worth it and the object code isn't
# part of the product. Loosen SRC_OPT for a while when you have to step
# through a failure.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag and is kept out of CFLAGS_COMMON on
# purpose. SRC_CFLAGS drives compile-only invocations, and clang's driver
# treats an unused linker argument there as an error under -Werror while gcc
# silently discards it, so the flag goes on CFLAGS, which drives the real
# link.
LD_HARDENING = $(CCOL_LD_HARDENING)
CFLAGS_COMMON = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) \
	-Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror
include ../hardening.mk

CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The library objects are built once here and linked into every binary this
# directory produces, rather than recompiled per binary.
SRC_OBJS = $(notdir $(SRC_FILES:.c=.o))
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage -DCCOL_TEST_COVERAGE
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS) $(CCOL_DL_LIBS)

# An object file keeps no record of how it was built, so without this stamp
# a change of compiler or flags would link stale objects of the wrong
# architecture, or objects with no instrumentation; a native build followed
# by `make CC="gcc -m32" test`, or by a coverage run, is such a change. The
# stamp holds the current compiler and flags and is rewritten only when they
# really change, so the objects get rebuilt when they have to be and stay
# cached otherwise.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# Pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build and set by generate_coverage_report, so one rule serves both.
# include/internal/ gets a line of its own because a *.h wildcard can't match
# a subdirectory. Without that second pattern no internal header is a
# prerequisite, so editing one recompiles nothing here, and a later edit to a
# single .c file gives a binary whose objects disagree about that header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h) \
              $(wildcard ../../include/internal/test_helpers/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

# tests_keys.c uses up every thread-specific key of the process before the
# first pool is created, so it is a binary of its own.
build: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)
	$(CC) $(CFLAGS) tests_keys.c $(SRC_OBJS) -o tests_keys $(LFLAGS)

# A separate ThreadSanitizer build, kept out of the default -Werror build.
# ThreadSanitizer's instrumentation and -O3 don't mix well, so this is a
# verification mode of its own that runs separately, done exactly like the
# build_tsan target of tests/cthreadcomm/Makefile.
build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)
	$(CC) $(TSAN_CFLAGS) tests_keys.c $(SRC_FILES) -o tests_keys_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test: build
	./tests
	./tests_keys

# die_after_fork=0 is set because several ctpool_handle_lifecycle tests in
# this suite fork and then create new pthreads in the child. One of them,
# destroy_from_within_own_task_is_fatal_even_after_forking_first, forks again
# from inside the pool's worker thread, so at that inner fork() call the
# process has several threads. The test_tsan target of
# tests/chttpclient/Makefile describes the exact ThreadSanitizer mechanism
# this works around.
#
# This suite's tests give identical, clean results with and without the
# option, so it changes nothing here. We set it to stay consistent with the
# other suites that fork and then start new threads (cthreadcomm, chttpclient
# and chttpserver), and it also covers a test that somebody adds later and
# that really needs it.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan
	./tests_keys_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_keys

# --ignore-errors negative is there because gcov's coverage counters are
# plain, non-atomic increments, so instrumented code running on more than one
# thread at a time can race on them and end up with a negative hit count that
# isn't real. It happens in more than one module and inside a glibc header as
# well; it's a limit of the gcov instrumentation, not a bug in the library's
# logic. tests/chttpclient/Makefile has the same flag with the full reason.
#
# This target recurses with COVERAGE_FLAGS set, so that the pattern rule
# above instruments the library objects too. The build rebuilds those objects
# because the flags differ from an ordinary build, so you do not have to run
# `make clean` first.
# Every binary that `test` runs is built and run here too, with the same
# instrumentation, so the tracefile counts what each of them reaches.
generate_coverage_report:
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_keys.c $(SRC_OBJS) -o tests_keys $(LFLAGS) && \
	./tests_keys && \
	gcov -d -w $(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o

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

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags tests tests_tsan tests_keys tests_keys_tsan coverage *.o *.gcno *.gcda *.gcov *.c.info

default: build
