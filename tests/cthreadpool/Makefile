CC = gcc
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS
SRC_FILE_PREFIX = cthreadpool
# cvector.c is needed for ctpool's own generation-tagged handle slot table
# (the same redesign already applied to chttpcli/chttpsvr/event_loop/
# clru_cache).
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/cvector.c \
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
# mechanism this works around. Empirically this suite's own tests were not
# observed to be affected by TSan's default (checked directly: identical,
# clean results with and without this option across multiple runs), but set
# for consistency with the other fork-and-spawn-new-threads suites
# (cthreadcomm, chttpclient, chttpserver) and as a safeguard against a
# future test addition actually needing it.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=1 --child-silent-after-fork=yes ./tests

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
