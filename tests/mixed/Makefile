INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILES = ../../src/cvector.c \
            ../../src/chashmap.c \
            ../../src/cbstmap.c \
            ../../src/cstring.c \
            ../../src/common.c \
            ../../src/csort.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
# tests_compat pins the behavioural half of the 1.x compatibility policy:
# ownership, the order of callbacks and the thread that runs them, the
# stability of pointers and iterators, and which allocator the code uses.
# Nothing else covers it, because `make check_abi` reads the shape of the
# interface and each module's suite tests the module against its own
# expectations rather than against what the policy promises. The corpus reaches
# across more modules than tests.c does, so it has its own source list instead
# of widening that one. It doesn't need process isolation and lives here as a
# second binary in this directory.
COMPAT_SRC_FILES = ../../src/cvector.c \
            ../../src/chashmap.c \
            ../../src/cbstmap.c \
            ../../src/cstring.c \
            ../../src/common.c \
            ../../src/csort.c \
            ../../src/cjson.c \
            ../../src/cyaml.c \
            ../../src/clrucache.c \
            ../../src/cthreadcomm.c \
            ../../src/cthreadpool.c \
            ../../src/cpintable.c
ALL_COMPAT_SRC_FILES = tests_compat.c $(COMPAT_SRC_FILES)
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

build_compat:
	$(CC) $(CFLAGS) $(ALL_COMPAT_SRC_FILES) -o tests_compat $(LFLAGS)

# The corpus drives a thread pool and a thread-safe cache, and it pins
# promises about which thread runs a callback. Those promises need a real race
# detector to check them, reasoning about them isn't enough.
build_compat_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_COMPAT_SRC_FILES) -o tests_compat_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test_tsan: build_compat_tsan
	./tests_compat_tsan

# A convenience alias, so the name of the binary works as a target too.
.PHONY: tests_compat
tests_compat: build_compat

test: build build_compat
	./tests
	./tests_compat

memtest: build build_compat
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_compat

# --ignore-errors negative is there because gcov's coverage counters are
# plain, non-atomic increments, so instrumented code running on more than one
# thread at a time can race on them and end up with a negative hit count that
# isn't real. It happens in more than one module and inside a glibc header as
# well; it's a limit of the gcov instrumentation, not a bug in the library's
# logic. tests/chttpclient/Makefile has the same flag with the full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file mixed.c.info && \
	genhtml --ignore-errors inconsistent mixed.c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest

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
	rm -rf tests tests_compat tests_compat_tsan coverage *.gcda *.gcdo *.gcno *.gcov

default: build
