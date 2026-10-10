INCLUDES = -I.. -I../../include
# CDEBUGLOG_ENABLED opts this suite into the diagnostic log buffer of
# src/cdebuglog.c. See the doc comment in include/internal/cdebuglog.h for
# why RUNNING_UNIT_TESTS alone does not turn it on. It backs the
# [DEBUG_TEST] checkpoints of resolve_then_use_race_destroy_waits. That test
# has an open hang that appears only in CI and only under qemu. The
# checkpoints are what make another such hang diagnosable.
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64 -DCDEBUGLOG_ENABLED
SRC_FILE_PREFIX = cthreadpool
# ctpool needs cvector.c for its generation-tagged handle slot table. That
# is the same mechanism that chttpcli, chttpsvr, ccol_event_loop and
# clru_cache each use. cdebuglog.c is a diagnostic log buffer that you opt
# into, and that only a RUNNING_UNIT_TESTS build holds. The default library
# build of the root Makefile leaves it out. See its doc comment in
# include/internal/cdebuglog.h. csort.c comes in through cvector.c, whose
# sort runs on the mergesort of csort.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/cdebuglog.c \
            ../../src/cvector.c ../../src/csort.c ../../src/common.c \
            ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
# The build compiles the sources of the library at the optimization level
# that the shipped library uses. These tests therefore exercise the code that
# ships, and not a debug build that nobody installs. Only the test file of
# this directory builds at -O0. The ability to debug is worth having there,
# and that object code is not part of the product. Loosen SRC_OPT for a time
# when you must step through a failure.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag, and it stays out of CFLAGS_COMMON on
# purpose. SRC_CFLAGS drives invocations that only compile. The driver of
# clang treats an unused linker argument there as an error under -Werror,
# where gcc discards it in silence. This flag belongs on CFLAGS, which drives
# the real link, so the build applies it there.
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
COVERAGE_ON = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS)

# An object file carries no record of how the build made it. A change of
# compiler or of flags would therefore link stale objects of the wrong
# architecture, or objects with no instrumentation. A native build followed
# by `make CC="gcc -m32" test`, or by a coverage run, is such a change. This
# stamp holds the current compiler and flags. The build rewrites it only when
# they really change. The objects therefore rebuild when they must, and stay
# cached when they need not.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# Pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build and set by generate_coverage_report, so one rule serves both.
# include/internal/ has a line of its own, because a *.h wildcard cannot
# match a subdirectory. Without the second pattern, no internal header is a
# prerequisite. An edit to one of them then recompiles nothing here. A later
# edit to a single .c file gives a binary whose objects disagree about that
# header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

# tests_keys.c uses up every thread-specific key of the process before the
# first pool is created, so it is a binary of its own.
build: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)
	$(CC) $(CFLAGS) tests_keys.c $(SRC_OBJS) -o tests_keys $(LFLAGS)

# This is a separate ThreadSanitizer build. It stays out of the default
# -Werror build. The instrumentation of ThreadSanitizer and -O3 do not mix
# well, and this is a distinct verification mode that runs on its own. It
# mirrors the build_tsan target of tests/cthreadcomm/Makefile exactly.
build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)
	$(CC) $(TSAN_CFLAGS) tests_keys.c $(SRC_FILES) -o tests_keys_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test: build
	./tests
	./tests_keys

# die_after_fork=0 is needed because several ctpool_handle_lifecycle tests
# in this suite fork and then create new pthreads in the child. One of them,
# destroy_from_within_own_task_is_fatal_even_after_forking_first, forks again
# from inside the worker thread of the pool. That is, it forks from a process
# with several threads at that inner fork() call. See the test_tsan target of
# tests/chttpclient/Makefile for the exact ThreadSanitizer mechanism that
# this works around.
#
# The tests of this suite give identical, clean results with the option and
# without it, so it carries nothing here. The build sets it to stay
# consistent with the other suites that fork and then start new threads.
# Those are cthreadcomm, chttpclient and chttpserver. It also guards against
# a test that somebody adds later and that really needs it.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan
	./tests_keys_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_keys

# --ignore-errors negative is needed because the coverage counters of gcov
# are plain increments that are not atomic. They can race when instrumented
# code runs on several threads, and then report a negative hit count that is
# not real. This affects more than one module, and it also happens inside a
# glibc header. It is a limit of the instrumentation of gcov, and not a
# defect in the logic of this library. See the identical flag in
# tests/chttpclient/Makefile for the full reason.
#
# This target recurses with COVERAGE_FLAGS set, so that the pattern rule
# above instruments the library objects too. The build rebuilds those objects
# because the flags differ from an ordinary build. You therefore do not have
# to run `make clean` first.
generate_coverage_report:
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w $(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o

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

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags tests tests_tsan tests_keys tests_keys_tsan coverage *.o *.gcno *.gcda *.gcov *.c.info

default: build
