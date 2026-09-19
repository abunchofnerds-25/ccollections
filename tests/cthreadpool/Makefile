CC = gcc
INCLUDES = -I.. -I../../include
# CDEBUGLOG_ENABLED opts this suite into src/cdebuglog.c's own diagnostic
# log buffer (see its doc comment in include/cdebuglog.h for why
# RUNNING_UNIT_TESTS alone is not enough to activate it). It backs
# resolve_then_use_race_destroy_waits's own [DEBUG_TEST] checkpoints, which
# exist because that test has an open, CI-only, qemu-only hang: the
# checkpoints are what make a recurrence diagnosable.
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64 -DCDEBUGLOG_ENABLED
SRC_FILE_PREFIX = cthreadpool
# cvector.c is needed for ctpool's own generation-tagged handle slot table
# (the same mechanism chttpcli/chttpsvr/ccol_event_loop/clru_cache each use).
# cdebuglog.c is an opt-in, RUNNING_UNIT_TESTS-only diagnostic
# log buffer (not part of the root Makefile's default library build; see
# its own doc comment in include/cdebuglog.h).
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/cdebuglog.c \
            ../../src/cvector.c ../../src/common.c ../../src/cpintable.c
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
LFLAGS = -lpthread

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
# verification mode, run separately); mirrors tests/cthreadcomm/Makefile's
# own build_tsan target exactly.
build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

test: build
	./tests

# die_after_fork=0: several of this suite's own ctpool_handle_lifecycle
# tests fork and create new pthreads in the child (and, for
# destroy_from_within_own_task_is_fatal_even_after_forking_first, a nested
# fork from within the pool's own worker thread, i.e. from a genuinely
# multi-threaded process at that inner fork() call); see
# tests/chttpclient/Makefile's own test_tsan target for the exact TSan
# mechanism this works around. This suite's own tests give identical, clean
# results with and without the option, so it is not load-bearing here; it is
# set for consistency with the other fork-and-spawn-new-threads suites
# (cthreadcomm, chttpclient, chttpserver) and as a safeguard against a
# future test addition actually needing it.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests

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
