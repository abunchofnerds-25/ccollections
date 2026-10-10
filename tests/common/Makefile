INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILES = ../../src/common.c
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

# tests_destroy_macros checks the destroy, free and release macros of every
# public header on their own. It defines the functions that those macros call
# as recording stubs, so it links none of the library. -Wshadow makes a
# temporary that one expansion hides in another a hard error.
build_destroy_macros:
	$(CC) $(CFLAGS) -Wshadow tests_destroy_macros.c -o tests_destroy_macros $(LFLAGS)

# pedantic_probe.c includes the container headers under -std=c11
# -pedantic-errors. Whether a header uses a GNU extension at inclusion time is
# a property of the compiler, and clang reports cases that gcc accepts, so the
# probe runs under $(CC) and under clang when it is installed.
PEDANTIC_PROBE_FLAGS = -I../../include -std=c11 -pedantic-errors -Wall -Wextra \
	-Werror -D_FILE_OFFSET_BITS=64 -fsyntax-only
pedantic_probe:
	$(CC) $(PEDANTIC_PROBE_FLAGS) pedantic_probe.c
	if command -v clang >/dev/null 2>&1; then \
		clang $(PEDANTIC_PROBE_FLAGS) pedantic_probe.c; \
	fi

test: build build_destroy_macros pedantic_probe
	./tests
	./tests_destroy_macros

memtest: build build_destroy_macros
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests_destroy_macros

# --ignore-errors negative: the coverage counters of gcov are plain, non-atomic
# increments, so instrumented code that runs concurrently can race on them and
# produce a false negative hit count. This affects more than one module and
# also happens inside a glibc header; it is a limit of the gcov
# instrumentation, not a defect in the logic of this library. See the
# identical flag in tests/chttpclient/Makefile for the full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov tests-common && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file common.c.info && \
	genhtml common.c.info --output-directory coverage && \
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
	rm -rf tests tests_destroy_macros coverage *.gcno *.gcda *.gcov *.c.info

default: build
