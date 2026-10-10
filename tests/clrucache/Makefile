INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = clrucache
# cthreadpool.c is a transitive dependency of cthreadcomm.c. The
# dispatch_pool of ccol_event_loop is a ctpool, and it exists when
# num_reactor_threads > 1. cvector.c is needed for two reasons.
# cthreadcomm.c links it, for the generation-tagged handle slot table
# of ccol_event_loop. clrucache.c needs it directly, for its own separate
# slot table of the same shape. csort.c is a dependency of cvector.c, which
# sorts through it.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/chashmap.c \
            ../../src/cthreadcomm.c ../../src/cthreadpool.c \
            ../../src/cvector.c ../../src/csort.c ../../src/common.c \
            ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS)

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# nesting_probe.c nests the public macros of this module inside their own
# arguments and inside those of other modules. -Wshadow makes any local that
# a nested expansion hides a hard error, which only the compiler can report,
# so the probe is compiled and never run.
nesting_probe:
	$(CC) $(INCLUDES) $(DEFINITIONS) -std=gnu11 -Wall -Wextra -Wshadow -Werror \
		-fsyntax-only nesting_probe.c

test: build nesting_probe
	./tests

# The cache resolves its handle without a lock. A race on that path therefore
# does not show up as a failed assertion in an ordinary run. This target sets
# die_after_fork=0 because this suite forks and then creates threads in the
# child. Without that option, ThreadSanitizer exits such a child in silence.
# The child then never reaches the abort that the test checks for.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests

# --ignore-errors negative is needed because the coverage counters of gcov
# are plain increments that are not atomic. They can race when instrumented
# code runs on several threads, and then report a negative hit count that is
# not real. This affects more than one module, and it also happens inside a
# glibc header. It is a limit of the instrumentation of gcov, and not a
# defect in the logic of this library. See the identical flag in
# tests/chttpclient/Makefile for the full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
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
	rm -rf tests tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
