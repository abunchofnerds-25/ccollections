INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = clogger
# cvector.c is here for clog's generation-tagged handle slot table, which is
# a cvec of slots, a cvec free-index stack, and the live-shareds and atfork
# scratch cvectors (see "Fork safety" in src/clogger.c). cthreadcomm.c is
# here for the writer-thread queue of async logging (ccol_circular_queue and
# ccol_dynamic_queue), and cthreadpool.c comes in as a dependency of
# cthreadcomm.c. csort.c is needed by cvector.c, whose sort runs on csort.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c \
            ../../src/chashmap.c ../../src/cvector.c ../../src/csort.c \
            ../../src/cthreadcomm.c ../../src/cthreadpool.c \
            ../../src/common.c ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
# The library sources are built at the optimization level of the shipped
# library, so these tests drive the code that ships and not a debug build
# that nobody installs. Only the test file of this directory is built at -O0,
# where being able to debug it is worth it and the object code isn't part of
# the product. Loosen SRC_OPT for a while when you have to step through a
# failure.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag and is kept out of CFLAGS_COMMON on
# purpose. SRC_CFLAGS drives compile-only invocations, and clang's driver
# treats an unused linker argument there as an error under -Werror, where gcc
# discards it, so the flag goes on CFLAGS, which does the real link.
LD_HARDENING = $(CCOL_LD_HARDENING)
CFLAGS_NO_DEFINITIONS = $(INCLUDES) -std=gnu11 $(CCOL_HARDENING_CFLAGS) \
	-Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror
CFLAGS_COMMON = $(CFLAGS_NO_DEFINITIONS) $(DEFINITIONS)
include ../hardening.mk

CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# consumer_view.c stands in for an application translation unit. It is built
# with $(CFLAGS_NO_DEFINITIONS), which has neither -D_FILE_OFFSET_BITS=64 nor
# -DRUNNING_UNIT_TESTS, so it sees the installed headers the way a program
# with ordinary default flags sees them, and everything a public header
# offers has to mean the same thing there as inside the library.
# consumer_view.h says what this checks and why.
CONSUMER_CFLAGS = $(CFLAGS_NO_DEFINITIONS) $(TEST_OPT) $(EXTRA_CFLAGS)
# The library objects are built once here and linked into every binary of
# this directory, instead of being compiled again for each one.
SRC_OBJS = $(notdir $(SRC_FILES:.c=.o))
TSAN_CFLAGS_NO_DEFINITIONS = $(INCLUDES) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)
TSAN_CFLAGS = $(TSAN_CFLAGS_NO_DEFINITIONS) $(DEFINITIONS)
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage
# -rdynamic exports the main binary's symbols to the dynamic linker, so that
# backtrace_symbols() can resolve function names inside the test binary.
LFLAGS = -lpthread -lz -rdynamic $(CCOL_PLATFORM_LIBS)

# An object file keeps no record of how it was built, so without this stamp
# a change of compiler or flags would link stale objects again, with the
# wrong architecture or with no instrumentation; a native build followed by
# `make CC="gcc -m32" test`, or by a coverage run, does exactly that. The
# stamp holds the current compiler and flags and is rewritten only when they
# really change, so the objects get rebuilt when they have to be and stay
# cached otherwise.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# consumer_view.o is built with different flags from the library objects, so
# it has its own stamp holding the flags that really govern it. A stamp that
# records somebody else's flags is worse than no stamp at all: a change to
# $(TEST_OPT) alone leaves $(SRC_CFLAGS) identical, so the stamp doesn't move
# and the object survives a change to how it is built. With two separate
# stamps, neither object gets rebuilt for a flag change that can't reach it.
.build-flags-consumer: FORCE
	@printf '%s\n' '$(CC) $(CONSUMER_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(CONSUMER_CFLAGS) $(COVERAGE_FLAGS)' > $@

# Pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build and generate_coverage_report sets it, so one rule serves
# both. include/internal/ gets a line of its own because a *.h wildcard can't
# match a subdirectory. Without that second pattern no internal header is a
# prerequisite, so editing one recompiles nothing here, and a later edit to a
# single .c file gives a binary whose objects disagree about that header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h) \
              $(wildcard ../../include/internal/test_helpers/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

consumer_view.o: consumer_view.c consumer_view.h $(LIB_HEADERS) .build-flags-consumer
	$(CC) $(COVERAGE_FLAGS) $(CONSUMER_CFLAGS) -c consumer_view.c -o $@

build: $(SRC_OBJS) consumer_view.o
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) consumer_view.o -o tests $(LFLAGS)
	$(CC) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) -o tests_sigpipe $(LFLAGS)
	$(CC) $(CFLAGS) tests_sigxfsz.c $(SRC_OBJS) -o tests_sigxfsz $(LFLAGS)

# A separate ThreadSanitizer build, kept out of the default -Werror build.
# ThreadSanitizer's instrumentation and -O3 don't go well together, so this
# is a mode of its own that runs separately.
build_tsan:
	$(CC) $(TSAN_CFLAGS_NO_DEFINITIONS) -c consumer_view.c -o consumer_view_tsan.o
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) consumer_view_tsan.o -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)
	$(CC) $(TSAN_CFLAGS) tests_sigpipe.c $(SRC_FILES) -o tests_sigpipe_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)
	$(CC) $(TSAN_CFLAGS) tests_sigxfsz.c $(SRC_FILES) -o tests_sigxfsz_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# tests_sigpipe is a binary of its own because it needs SIGPIPE at its
# default disposition, which the pipe tests of tests.c change for their whole
# process. tests_sigxfsz is separate for the same reason: it needs SIGXFSZ at
# its default disposition, which tests.c ignores for its whole process, and
# its own process must never open a logger before its forked scenarios run.
# nesting_probe.c is compiled twice and never run. The first build nests the
# logging macros inside the arguments of other macros under -Wshadow, so a
# local hidden by a nested expansion becomes a hard error. The second build
# is strict ISO C11 with -pedantic-errors, where a call with a format and no
# argument after it has to compile.
nesting_probe:
	$(CC) $(INCLUDES) $(DEFINITIONS) -std=gnu11 -Wall -Wextra -Wshadow -Werror \
		-fsyntax-only nesting_probe.c
	$(CC) $(INCLUDES) $(DEFINITIONS) -DCLOGGER_PROBE_STRICT -std=c11 \
		-pedantic-errors -Wall -Wextra -Werror -fsyntax-only nesting_probe.c

test: build nesting_probe
	./tests
	./tests_sigpipe
	./tests_sigxfsz

# die_after_fork=0 is needed here. This suite's tests for a fatal call and
# for a null handle in a forked child call fork(), and so does the
# fork_safety.* group, and some of them create new pthreads in the child.
# When the parent has several threads at the moment of the fork(),
# ThreadSanitizer's default of die_after_fork=1 can make such a child exit
# normally, so it never reaches its expected _ccol_fatal_err() or SIGABRT and
# never drives the forked thread at all. The comment on the test_tsan target
# of tests/chttpclient/Makefile gives the exact mechanism.
# suppressions=tsan_atfork_lock_order.supp is needed too, because the
# detector reports a lock-order inversion between this module's fork-prepare
# handler and cthreadcomm's. The C library serializes prepare handlers, so
# that inversion can't deadlock. The file has the full argument, together
# with an always-on check that catches the case where the inversion WOULD
# deadlock. The same file also suppresses, with its own argument, the reports
# ThreadSanitizer raises because it doesn't model the re-initialization of a
# write-locked slot-table rwlock in a fork child. tests/chttpclient/Makefile
# does the same, a suppression that comes with a documented reason.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_atfork_lock_order.supp" ./tests_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_atfork_lock_order.supp" ./tests_sigpipe_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_atfork_lock_order.supp" ./tests_sigxfsz_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --child-silent-after-fork=yes --error-exitcode=1 ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --child-silent-after-fork=yes --error-exitcode=1 ./tests_sigpipe
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --child-silent-after-fork=yes --error-exitcode=1 ./tests_sigxfsz

# --ignore-errors negative is there because gcov's coverage counters are
# plain, non-atomic increments, so instrumented code running on more than one
# thread at a time can race on them and end up with a negative hit count that
# isn't real. It happens in more than one module and inside a glibc header as
# well; it's a limit of the gcov instrumentation, not a bug in the library's
# logic. tests/chttpclient/Makefile has the same flag with the full reason.
# This target recurses with COVERAGE_FLAGS set, so that the pattern rule above
# also instruments the library objects. Those objects rebuild because the
# flags differ from an ordinary build, so a `make clean` first is not needed.
generate_coverage_report:
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS) consumer_view.o
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests.c $(SRC_OBJS) consumer_view.o -o tests $(LFLAGS) && \
	./tests && \
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) -o tests_sigpipe $(LFLAGS) && \
	./tests_sigpipe && \
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_sigxfsz.c $(SRC_OBJS) -o tests_sigxfsz $(LFLAGS) && \
	./tests_sigxfsz && \
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
	rm -f *.o *.gcno *.gcda .build-flags .build-flags-consumer

clean:
	rm -rf .build-flags .build-flags-consumer tests tests_tsan tests_sigpipe tests_sigpipe_tsan tests_sigxfsz tests_sigxfsz_tsan coverage *.o *.gcno *.gcda *.gcov *.c.info

default: build
