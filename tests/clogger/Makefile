CC = gcc
INCLUDES = -I.. -I../../include
# CDEBUGLOG_ENABLED opts this suite into src/cdebuglog.c's own diagnostic
# log buffer (see its doc comment in include/cdebuglog.h for why
# RUNNING_UNIT_TESTS alone is not enough to activate it).
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64 -DCDEBUGLOG_ENABLED
SRC_FILE_PREFIX = clogger
# cvector.c is needed for clog's own generation-tagged handle slot table (a
# cvec of slots plus a cvec free-index stack, plus the live-shareds and
# atfork scratch cvectors; see "Fork safety" in src/clogger.c). cthreadcomm.c
# is needed for async logging's writer-thread queue (circular_queue /
# dynamic_queue); cthreadpool.c is cthreadcomm.c's own transitive dependency.
# cdebuglog.c is an opt-in, RUNNING_UNIT_TESTS-only diagnostic log buffer
# (not part of the root Makefile's default library build; see its own doc
# comment in include/cdebuglog.h) that clogger.c's own DEBUG_ROTATE/
# DEBUG_ROTATE_TIMING instrumentation currently uses while chasing a
# CI-only, qemu-only rotation/compression timing mystery.
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/cdebuglog.c \
            ../../src/chashmap.c ../../src/cvector.c \
            ../../src/cthreadcomm.c ../../src/cthreadpool.c \
            ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror $(EXTRA_CFLAGS)
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
# -rdynamic exports the main binary's symbols to the dynamic linker so that
# backtrace_symbols() can resolve function names inside the test binary.
LFLAGS = -lpthread -lz -rdynamic

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

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
# child exit normally instead of reaching its expected fatal_err()/SIGABRT
# or exercising the forked thread at all, specifically when the parent is
# multi-threaded at fork() time (see tests/chttpclient/Makefile's fuller
# comment on its own test_tsan target for the exact mechanism).
# suppressions=tsan_rwlock_reinit.supp: a known, confirmed-non-bug
# ThreadSanitizer limitation around this module's own fork()-safety rwlock
# reinit; see that file's own comment for the independent, clogger.c-free
# reproduction proving it, mirroring tests/chttpclient/Makefile's identical
# "known non-bug, suppress with a documented reason" precedent.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_rwlock_reinit.supp" ./tests_tsan

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --child-silent-after-fork=yes --error-exitcode=1 ./tests

# --ignore-errors negative: gcov's own coverage counters are plain (non-atomic)
# increments, which can race under concurrently-executed instrumented code and
# has been observed producing a spurious negative hit count (reproduced on
# more than one module, including a glibc header, not this library's own
# logic); see tests/chttpclient/Makefile's identical flag for the full
# rationale and reproduction notes.
generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build build_tsan test test_tsan memtest generate_coverage_report

clean:
	rm -rf tests tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
