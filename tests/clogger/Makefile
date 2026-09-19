CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = clogger
# cvector.c is needed for clog's own generation-tagged handle slot table (a
# cvec of slots plus a cvec free-index stack, plus the live-shareds and
# atfork scratch cvectors; see "Fork safety" in src/clogger.c). cthreadcomm.c
# is needed for async logging's writer-thread queue (ccol_circular_queue /
# ccol_dynamic_queue); cthreadpool.c is cthreadcomm.c's own transitive dependency.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c \
            ../../src/chashmap.c ../../src/cvector.c \
            ../../src/cthreadcomm.c ../../src/cthreadpool.c \
            ../../src/common.c ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
# The library's own sources are compiled at the optimization level the shipped
# library uses, so what these tests exercise is the code that ships rather than
# a debug build nobody installs. Only this directory's own test file is built
# at -O0, where the extra debuggability is worth having and the object code is
# not part of the product. Loosen SRC_OPT temporarily when a failure needs to
# be stepped through.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag and is kept out of CFLAGS_COMMON on
# purpose: SRC_CFLAGS drives compile-only invocations, and clang's driver
# treats an unused linker argument there as an error under -Werror, where gcc
# merely discards it. It belongs on CFLAGS, which is what performs the real
# link, and is applied there instead.
LD_HARDENING = -Wl,-z,relro,-z,now
CFLAGS_COMMON = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong \
	-Wstrict-overflow \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror
CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The library objects are built once here and linked into every binary this
# directory produces, rather than recompiled per binary.
SRC_OBJS = $(notdir $(SRC_FILES:.c=.o))
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage
# -rdynamic exports the main binary's symbols to the dynamic linker so that
# backtrace_symbols() can resolve function names inside the test binary.
LFLAGS = -lpthread -lz -rdynamic

# An object file carries no record of how it was built, so switching compiler
# or flags (a native build followed by `make CC="gcc -m32" test`, or by a
# coverage run) would otherwise relink stale objects of the wrong architecture
# or without instrumentation. This stamp holds the current compiler and flags
# and is rewritten only when they actually change, so the objects rebuild when
# they must and stay cached when they need not.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# Pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build and set by generate_coverage_report, so one rule serves both.
LIB_HEADERS = $(wildcard ../../include/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

build: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)

# Separate ThreadSanitizer build, kept out of the default -Werror build
# (TSan instrumentation and -O3 don't mix well, and this is a distinct
# verification mode, run separately).
build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

test: build
	./tests

# die_after_fork=0: this suite's own forked-child fatal/null-handle tests
# (and the fork_safety.* group) fork and, in some cases, create new pthreads
# in the child; TSan's default die_after_fork=1 can otherwise make such a
# child exit normally instead of reaching its expected ccol_fatal_err()/SIGABRT
# or exercising the forked thread at all, specifically when the parent is
# multi-threaded at fork() time (see tests/chttpclient/Makefile's fuller
# comment on its own test_tsan target for the exact mechanism).
# suppressions=tsan_atfork_lock_order.supp: a lock-order-inversion the
# detector raises between this module's fork-prepare handler and
# cthreadcomm's, which the C library's own serialization of prepare handlers
# makes undeadlockable; see that file's own comment for the full argument and
# for the always-on check that catches the case where it would NOT be, and
# tests/chttpclient/Makefile for the same "suppress with a documented reason"
# precedent.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_atfork_lock_order.supp" ./tests_tsan

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --child-silent-after-fork=yes --error-exitcode=1 ./tests

# --ignore-errors negative: gcov's own coverage counters are plain (non-atomic)
# increments, which can race under concurrently-executed instrumented code and
# produce a spurious negative hit count. This affects more than one module,
# including inside a glibc header, and is a limitation of gcov's own
# instrumentation rather than a defect in this library's logic; see
# tests/chttpclient/Makefile's identical flag for the full rationale.
# Recurses with COVERAGE_FLAGS set so the pattern rule above instruments the
# library objects too; the objects are rebuilt because the flags differ from an
# ordinary build, so `make clean` first is not required.
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

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags tests tests_tsan coverage *.o *.gcno *.gcda *.gcov *.c.info

default: build
