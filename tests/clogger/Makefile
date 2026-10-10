INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = clogger
# cvector.c is needed for the generation-tagged handle slot table of clog.
# That table is a cvec of slots, a cvec free-index stack, and the live-shareds
# and atfork scratch cvectors. See "Fork safety" in src/clogger.c.
# cthreadcomm.c is needed for the writer-thread queue of async logging, which
# is ccol_circular_queue and ccol_dynamic_queue. cthreadpool.c is a transitive
# dependency of cthreadcomm.c. csort.c is a dependency of cvector.c, whose
# sort runs on csort.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c \
            ../../src/chashmap.c ../../src/cvector.c ../../src/csort.c \
            ../../src/cthreadcomm.c ../../src/cthreadpool.c \
            ../../src/common.c ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
# The build compiles the sources of the library at the optimization level that
# the shipped library uses. These tests therefore drive the code that ships,
# and not a debug build that nobody installs. Only the test file of this
# directory is built at -O0. The ability to debug is worth having there, and
# that object code is not part of the product. Loosen SRC_OPT for a time when
# you must step through a failure.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag. It stays out of CFLAGS_COMMON on
# purpose. SRC_CFLAGS drives invocations that only compile. The driver of
# clang treats an unused linker argument there as an error under -Werror,
# where gcc discards it. That flag belongs on CFLAGS, which does the real
# link, so the build applies it there.
LD_HARDENING = $(CCOL_LD_HARDENING)
CFLAGS_NO_DEFINITIONS = $(INCLUDES) -std=gnu11 $(CCOL_HARDENING_CFLAGS) \
	-Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror
CFLAGS_COMMON = $(CFLAGS_NO_DEFINITIONS) $(DEFINITIONS)
include ../hardening.mk

CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# consumer_view.c stands in for an application translation unit. The build
# makes it from $(CFLAGS_NO_DEFINITIONS), which holds neither
# -D_FILE_OFFSET_BITS=64 nor -DRUNNING_UNIT_TESTS. It therefore sees the
# installed headers the way a program with ordinary default flags sees them.
# Everything that a public header offers must mean the same thing there as it
# does inside the library. consumer_view.h says what this checks and why.
CONSUMER_CFLAGS = $(CFLAGS_NO_DEFINITIONS) $(TEST_OPT) $(EXTRA_CFLAGS)
# The build makes the library objects one time here. It links them into every
# binary of this directory, and does not compile them again for each one.
SRC_OBJS = $(notdir $(SRC_FILES:.c=.o))
TSAN_CFLAGS_NO_DEFINITIONS = $(INCLUDES) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)
TSAN_CFLAGS = $(TSAN_CFLAGS_NO_DEFINITIONS) $(DEFINITIONS)
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage
# -rdynamic exports the symbols of the main binary to the dynamic linker.
# backtrace_symbols() can then resolve function names inside the test
# binary.
LFLAGS = -lpthread -lz -rdynamic $(CCOL_PLATFORM_LIBS)

# An object file carries no record of how the build made it. A change of
# compiler or of flags would therefore link stale objects again. Those objects
# have the wrong architecture, or no instrumentation. A native build followed
# by `make CC="gcc -m32" test`, or by a coverage run, does this. This stamp
# holds the current compiler and flags. The build rewrites it only when they
# really change. The objects therefore rebuild when they must, and stay cached
# when they need not.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# The build makes consumer_view.o from a different set of flags than the
# library objects. It therefore gets its own stamp, which holds the flags that
# really govern it. A stamp that records the flags of something else is worse
# than no stamp. A change to $(TEST_OPT) alone leaves $(SRC_CFLAGS) identical.
# The stamp then does not move, and the object survives a change to how the
# build makes it. Two separate stamps also mean that neither object rebuilds
# for a flag change that cannot reach it.
.build-flags-consumer: FORCE
	@printf '%s\n' '$(CC) $(CONSUMER_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(CONSUMER_CFLAGS) $(COVERAGE_FLAGS)' > $@

# The pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build, and generate_coverage_report sets it. One rule therefore
# serves both. include/internal/ has a line of its own, because a *.h wildcard
# cannot match a subdirectory. Without the second pattern, no internal header
# is a prerequisite. An edit to one of them then recompiles nothing here. A
# later edit to a single .c file gives a binary whose objects disagree about
# that header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

consumer_view.o: consumer_view.c consumer_view.h $(LIB_HEADERS) .build-flags-consumer
	$(CC) $(COVERAGE_FLAGS) $(CONSUMER_CFLAGS) -c consumer_view.c -o $@

build: $(SRC_OBJS) consumer_view.o
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) consumer_view.o -o tests $(LFLAGS)
	$(CC) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) -o tests_sigpipe $(LFLAGS)

# A separate ThreadSanitizer build. It stays out of the default -Werror build.
# The instrumentation of ThreadSanitizer and -O3 do not go well together, and
# this is a separate mode that runs on its own.
build_tsan:
	$(CC) $(TSAN_CFLAGS_NO_DEFINITIONS) -c consumer_view.c -o consumer_view_tsan.o
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) consumer_view_tsan.o -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)
	$(CC) $(TSAN_CFLAGS) tests_sigpipe.c $(SRC_FILES) -o tests_sigpipe_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# tests_sigpipe is a binary of its own because it needs the default
# disposition of SIGPIPE, which the pipe tests of tests.c change for their
# whole process.
# nesting_probe.c is compiled twice and never run. The first build nests the
# logging macros inside the arguments of other macros under -Wshadow, which
# makes a local that a nested expansion hides a hard error. The second build
# is strict ISO C11 with -pedantic-errors, where a call with a format and no
# argument after it must compile.
nesting_probe:
	$(CC) $(INCLUDES) $(DEFINITIONS) -std=gnu11 -Wall -Wextra -Wshadow -Werror \
		-fsyntax-only nesting_probe.c
	$(CC) $(INCLUDES) $(DEFINITIONS) -DCLOGGER_PROBE_STRICT -std=c11 \
		-pedantic-errors -Wall -Wextra -Werror -fsyntax-only nesting_probe.c

test: build nesting_probe
	./tests
	./tests_sigpipe

# die_after_fork=0 is needed here. The tests of this suite for a fatal call
# and for a null handle in a forked child call fork(), and so does the
# fork_safety.* group. Some of them create new pthreads in the child. The
# default die_after_fork=1 of ThreadSanitizer can make such a child exit
# normally. It then never reaches its expected ccol_fatal_err() or SIGABRT,
# and it never drives the forked thread at all. This happens when the parent
# has several threads at the time of the fork(). The comment on the test_tsan
# target of tests/chttpclient/Makefile gives the exact mechanism.
# suppressions=tsan_atfork_lock_order.supp is also needed. The detector
# reports a lock-order inversion between the fork-prepare handler of this
# module and the one of cthreadcomm. The C library serializes prepare
# handlers, which makes that inversion unable to deadlock. That file holds the
# full argument. It also holds an always-on check that catches the case where
# the inversion WOULD deadlock. The same file also suppresses the reports that
# ThreadSanitizer raises because it does not model the re-initialization of a
# write-locked slot-table rwlock in a fork child, with its own argument.
# tests/chttpclient/Makefile sets the same precedent of a suppression with a
# documented reason.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_atfork_lock_order.supp" ./tests_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_atfork_lock_order.supp" ./tests_sigpipe_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --child-silent-after-fork=yes --error-exitcode=1 ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --child-silent-after-fork=yes --error-exitcode=1 ./tests_sigpipe

# --ignore-errors negative is needed. The coverage counters of gcov are plain
# increments that are not atomic. They can race when instrumented code runs
# concurrently, and they then give a negative hit count that is not real. This
# affects more than one module, and also code inside a glibc header. It is a
# limitation of the instrumentation of gcov, and not a defect in the logic of
# this library. See the identical flag in tests/chttpclient/Makefile for the
# full rationale.
# This target recurses with COVERAGE_FLAGS set, so that the pattern rule above
# also instruments the library objects. Those objects rebuild because the
# flags differ from an ordinary build. A `make clean` first is therefore not
# needed.
generate_coverage_report:
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS) consumer_view.o
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests.c $(SRC_OBJS) consumer_view.o -o tests $(LFLAGS) && \
	./tests && \
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) -o tests_sigpipe $(LFLAGS) && \
	./tests_sigpipe && \
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
	rm -f *.o *.gcno *.gcda .build-flags .build-flags-consumer

clean:
	rm -rf .build-flags .build-flags-consumer tests tests_tsan tests_sigpipe tests_sigpipe_tsan coverage *.o *.gcno *.gcda *.gcov *.c.info

default: build
