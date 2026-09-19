CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = cthreadcomm
# chashmap.c is a transitive dependency: ccol_event_loop's internal fd registry
# (int fd -> event_entry*) is backed by chmap. cthreadpool.c is a transitive
# dependency too: ccol_event_loop's dispatch_pool (num_reactor_threads > 1) is a
# ctpool. cvector.c is needed for ccol_event_loop's own generation-tagged handle
# slot table (a cvec of slots plus a cvec free-index stack).
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c \
            ../../src/chashmap.c ../../src/cthreadpool.c \
            ../../src/cvector.c ../../src/common.c ../../src/cpintable.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O3 -Werror $(EXTRA_CFLAGS)
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread

build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)

# Separate ThreadSanitizer build, kept out of the default -Werror build
# (TSan instrumentation and -O3 don't mix well, and this is a distinct
# verification mode, run separately).
build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

test: build
	./tests

# die_after_fork=0: this suite's own ccol_event_loop_handle_lifecycle tests fork
# and create new pthreads in the child (see tests/chttpclient/Makefile's
# fuller comment on its own test_tsan target for the exact TSan mechanism).
# This module's ccol_event_loop instances are torn down between tests, which
# keeps the parent single-threaded at fork() time, so the option is not
# load-bearing here the way it is for chttpclient/chttpserver's own
# persistent, process-wide reactors; set for consistency across all
# three fork-and-spawn-new-threads suites, and as a safeguard against a
# future test addition making this parent multi-threaded at fork time too.
# suppressions=tsan_rwlock_reinit.supp: a known, confirmed-non-bug
# ThreadSanitizer limitation around this module's own fork()-safety rwlock
# reinit; see that file's own comment for the independent, cthreadcomm.c-free
# reproduction proving it, mirroring tests/clogger/Makefile's identical
# "known non-bug, suppress with a documented reason" precedent.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_rwlock_reinit.supp" ./tests_tsan

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
	gcov tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build build_tsan test test_tsan memtest generate_coverage_report

clean:
	rm -rf tests tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
