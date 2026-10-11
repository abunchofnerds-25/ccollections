INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = chttp
# chttp1_parser.c is the shared HTTP/1.1 parser that the tests at the end of
# tests.c drive directly; it links against ctls.c, chashmap.c and common.c.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/chttp1_parser.c \
	../../src/ctls.c ../../src/chashmap.c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -gdwarf-4 -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
# The two wraps route every send(2) and read(2) call of the linked sources
# through __wrap_send and __wrap_read in tests.c. Both pass straight through
# unless a test arms them to report one EINTR, which no real socket produces
# on demand.
LFLAGS = -lpthread -lssl -lcrypto $(call ccol_ld_wrap,send read) $(CCOL_PLATFORM_LIBS)

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

test: build
	./tests

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests

# The flag "--ignore-errors negative" is there because the coverage counters
# of gcov are plain, non-atomic increments: in instrumented code that runs
# concurrently, two threads can race on one counter, and gcov then reports a
# false hit count below zero. This happens in more than one module and also
# inside a glibc header; it is a limit of the gcov instrumentation, not a
# defect in the logic of this library. The Makefile of tests/chttpclient
# carries the same flag, and its comment gives the full reason.
generate_coverage_report:
	$(CC)  $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && echo 'GCOV SUCCEEDED, CALLING LCOV' && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
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
