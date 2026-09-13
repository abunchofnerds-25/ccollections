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
            ../../src/cvector.c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror $(EXTRA_CFLAGS)
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

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
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build build_tsan test test_tsan memtest generate_coverage_report

clean:
	rm -rf tests tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
