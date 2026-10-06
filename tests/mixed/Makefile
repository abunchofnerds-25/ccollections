INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILES = ../../src/cvector.c \
            ../../src/chashmap.c \
            ../../src/cbstmap.c \
            ../../src/cstring.c \
            ../../src/common.c \
            ../../src/csort.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
# tests_compat pins the behavioural half of the 1.x compatibility policy. That
# half covers ownership, the order of a callback and the thread that runs it,
# the stability of a pointer and of an iterator, and which allocator the code
# uses. No other check covers it. `make check_abi` reads the shape of the
# interface. The suite of each module tests that module against its own
# expectations, and not against the promise that the policy makes. The corpus
# reaches across more modules than tests.c does, so it carries its own source
# list, and does not widen that one. It needs no process isolation, and lives
# here as a second binary in this directory.
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
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1
LFLAGS = -lpthread $(CCOL_PLATFORM_LIBS)

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

build_compat:
	$(CC) $(CFLAGS) $(ALL_COMPAT_SRC_FILES) -o tests_compat $(LFLAGS)

# The corpus drives a thread pool and a thread-safe cache. It pins promises
# about the thread that runs a callback. A real race detector is the right way
# to check those promises, and reasoning about them is not.
build_compat_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_COMPAT_SRC_FILES) -o tests_compat_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test_tsan: build_compat_tsan
	./tests_compat_tsan

# An alias for convenience, so that the name of the binary also works as a
# target.
.PHONY: tests_compat
tests_compat: build_compat

test: build build_compat
	./tests
	./tests_compat

memtest: build build_compat
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 ./tests_compat

# --ignore-errors negative: the coverage counters of gcov are plain increments
# that are not atomic. Instrumented code that runs concurrently can race on
# them and produce a false negative hit count. This affects more than one
# module, and it also happens inside a glibc header. It is a limit of the
# instrumentation of gcov, and not a defect in the logic of this library. See
# the identical flag in tests/chttpclient/Makefile for the full reason.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file mixed.c.info && \
	genhtml --ignore-errors inconsistent mixed.c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest

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
	rm -rf tests tests_compat tests_compat_tsan coverage *.gcda *.gcdo *.gcno *.gcov

default: build
