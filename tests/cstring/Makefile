INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cstring
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/cvector.c ../../src/csort.c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS)

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

# nesting_probe.c nests the public macros of this module inside their own
# arguments and inside those of other modules. -Wshadow makes any local that
# a nested expansion hides a hard error, which only the compiler can report,
# so the probe is compiled and never run.
nesting_probe:
	$(CC) $(INCLUDES) $(DEFINITIONS) -std=gnu11 -Wall -Wextra -Wshadow -Werror \
		-fsyntax-only nesting_probe.c

test: build nesting_probe
	./tests

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests

# --ignore-errors negative: the coverage counters of gcov are plain
# increments and they are not atomic. Instrumented code that runs on more
# than one thread at a time can therefore race on such a counter. That race
# produces a false negative hit count. This affects more than one module, and
# it also affects code inside a glibc header. This is a limitation of the
# instrumentation of gcov and not a defect in the logic of this library. The
# same flag in tests/chttpclient/Makefile carries the full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

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
	rm -rf tests coverage *.gcno *.gcda *.gcov *.c.info

default: build
