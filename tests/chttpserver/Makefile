
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64

# Our sources, built with strict flags and -Werror. chttpserver.c and
# chttpclient.c each own a fully separate static ccol_event_loop reactor.
#
# This shared source set builds six binaries:
#   tests            (tests.c) - the main suite.
#   tests_tls        (tests_tls.c) - drives a real TLS handshake against a
#                     real self-signed certificate that the `openssl`
#                     command line tool generates when the tests start. It
#                     is a SEPARATE BINARY only as a choice of test layout,
#                     to keep the handshake cases together and out of the
#                     main suite.
#   tests_mem_mgmt   (tests_mem_mgmt.c) - calls
#                     chttpsvr_set_engine_mem_mgmt_procs() before the first
#                     chttpsvr_start() in the process. That order is a rule
#                     for the whole process, and the procs can be set only
#                     once ever. The shared _setup() of tests.c already
#                     calls chttpsvr_start(), so the happy path of "install
#                     the procs, then start" needs a process where nothing
#                     started the engine first, hence its own SEPARATE
#                     BINARY.
#   tests_engine_stop (tests_engine_stop.c) - exercises
#                     chttpsvr_engine_stop(), the forced engine shutdown
#                     path that a signal handler drives. That path tears
#                     down the process's one shared ccol_event_loop reactor
#                     under every chttpsvr instance started at that moment.
#                     g_srv, g_srv2 and the other servers of tests.c stay
#                     started for its whole run, so a forced stop of the
#                     shared engine there would pull the reactor out from
#                     under them and fail unrelated tests. It gets its own
#                     SEPARATE BINARY for the same isolation reason as
#                     tests_tls and tests_mem_mgmt above.
#   tests_worker_key (tests_worker_key.c) - takes every thread-specific key
#                     the process can have before the first chttpsvr_start()
#                     creates the key that marks worker threads. The shared
#                     _setup() of tests.c has already created that key, so
#                     this needs a fresh process and is a SEPARATE BINARY
#                     for that reason.
#   tests_sigpipe    (tests_sigpipe.c) - runs with SIGPIPE at its default
#                     disposition, which ends the process. Every other
#                     binary here ignores SIGPIPE, and an ignored SIGPIPE is
#                     discarded when it's raised, so only a binary of its
#                     own can see a server write that raises it, or a
#                     server that changes the disposition.
COMMON_SRC_FILES = ../../src/chttpserver.c \
	../../src/chttpclient.c \
	../../src/chttp1_parser.c \
	../../src/chttp.c \
	../../src/cthreadcomm.c \
	../../src/cthreadpool.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cvector.c \
	../../src/csort.c \
	../../src/ctls.c \
	../../src/common.c ../../src/cpintable.c

EXTRA_CFLAGS ?=
# The library sources are built at the optimization level of the shipped
# library, so these tests run the code that ships and not a debug build that
# nobody installs. Only the test files of this directory are built at -O0,
# where being easier to debug is worth it and the object code isn't part of
# the product. Loosen SRC_OPT for a while when you have to step through a
# failure.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag and is kept out of CFLAGS_COMMON on
# purpose. SRC_CFLAGS drives compile-only calls, and clang's driver treats an
# unused linker argument there as an error under -Werror while gcc just
# discards it, so the flag goes on CFLAGS, which does the real link.
LD_HARDENING = $(CCOL_LD_HARDENING)
CFLAGS_COMMON = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) \
	-Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror -fPIC
include ../hardening.mk

CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The library objects are built once for this directory and linked into
# every binary it produces, instead of being recompiled for each one.
SRC_OBJS = $(notdir $(COMMON_SRC_FILES:.c=.o))
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 -fPIC $(EXTRA_CFLAGS)

LFLAGS = -lpthread -lssl -lcrypto -lm -lz -rdynamic $(CCOL_PLATFORM_LIBS)

# Empty for an ordinary build. generate_coverage_report passes COVERAGE_ON
# in, so the same pattern rule instruments the library objects.
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage -DCCOL_TEST_COVERAGE

# An object file keeps no record of how it was built, so without this stamp
# a change of compiler or flags relinks stale objects of the wrong
# architecture, or objects with no instrumentation in them; a native build
# followed by `make CC="gcc -m32" test`, or by a coverage run, does exactly
# that. The stamp holds the current compiler and flags and is rewritten only
# when they really change, so the objects get rebuilt when they have to be
# and stay cached otherwise.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# Pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build and generate_coverage_report sets it, so one rule serves
# both. include/internal/ has its own line because a *.h wildcard can't match
# a subdirectory. Without that second pattern no internal header is a
# prerequisite, so editing one recompiles nothing here, and a later edit to a
# single .c file gives a binary whose objects disagree about that header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h) \
              $(wildcard ../../include/internal/test_helpers/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

build: $(SRC_OBJS) tests_locale
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)

# The locale_folding tests run the server under a Turkish LC_CTYPE, where
# strncasecmp() and tolower() fold 'I' to a letter other than 'i'. They use
# this private copy of tr_TR.UTF-8, found through LOCPATH. The step is best
# effort: without localedef (macOS has none) or the tr_TR locale source, the
# directory stays empty and those tests print that they skipped.
tests_locale:
	mkdir -p tests_locale
	@if command -v localedef >/dev/null 2>&1; then \
	  localedef -c -i tr_TR -f UTF-8 tests_locale/tr_TR.UTF-8 >/dev/null 2>&1 \
	    || true; \
	fi

# A separate ThreadSanitizer build of the main suite only, kept out of the
# default -Werror build. tests/cthreadcomm/Makefile has the same build_tsan
# and test_tsan pattern.
build_tsan:
	$(CC) $(TSAN_CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# die_after_fork=0: tests/chttpclient/Makefile explains this on its own
# test_tsan target, and the same root cause, verified the same way, applies
# here. A persistent, multi-threaded ccol_event_loop reactor for the whole
# process is alive when fork() runs, and that trips TSan's default of
# die_after_fork=1 as soon as the forked child of
# concurrent_double_destroy_is_fatal creates its two racing pthreads.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan

# A separate ThreadSanitizer build of tests_engine_stop.c, done exactly like
# build_tsan and test_tsan above. Of the six binaries in this directory this
# one has by far the most fork and thread races: reaper against destroy,
# start against stop, listener against dispatch, and a concurrent destroy
# against engine_stop. Each one is driven by dedicated white-box hooks that
# arm, wait and release, and since those hooks exist to produce data races
# deterministically, this suite gains the most from the tool that is best at
# catching the kind of bug it reproduces.
build_engine_stop_tsan:
	$(CC) $(TSAN_CFLAGS) tests_engine_stop.c $(COMMON_SRC_FILES) -o tests_engine_stop_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

test_engine_stop_tsan: build_engine_stop_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_engine_stop_tsan

build_tls: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_tls.c $(SRC_OBJS) -o tests_tls $(LFLAGS)

build_mem_mgmt: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_mem_mgmt.c $(SRC_OBJS) -o tests_mem_mgmt $(LFLAGS)

build_engine_stop: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_engine_stop.c $(SRC_OBJS) -o tests_engine_stop $(LFLAGS)

build_worker_key: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_worker_key.c $(SRC_OBJS) -o tests_worker_key $(LFLAGS)

build_sigpipe: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) -o tests_sigpipe $(LFLAGS)


# Aliases, so that the name of each binary this suite produces also works as
# a target. Each one builds its binary, while `make test` builds and runs all
# of them and stays the normal entry point.
.PHONY: tests_tls tests_mem_mgmt tests_engine_stop tests_worker_key tests_sigpipe
tests_tls: build_tls
tests_mem_mgmt: build_mem_mgmt
tests_engine_stop: build_engine_stop
tests_worker_key: build_worker_key
tests_sigpipe: build_sigpipe

test: build build_tls build_mem_mgmt build_engine_stop build_worker_key build_sigpipe
	./tests
	./tests_tls
	./tests_mem_mgmt
	./tests_engine_stop
	./tests_worker_key
	./tests_sigpipe

memtest: build build_tls build_mem_mgmt build_engine_stop build_worker_key build_sigpipe
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_tls
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_mem_mgmt
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes ./tests_engine_stop
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes ./tests_worker_key
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_sigpipe

# --ignore-errors negative is there because gcov's coverage counters are
# plain, non-atomic increments, so instrumented code running on more than one
# thread at a time can race on them and end up with a negative hit count that
# isn't real. It happens in more than one module and inside a glibc header as
# well; it's a limit of the gcov instrumentation, not a bug in the library's
# logic. tests/chttpclient/Makefile has the same flag with the full reason.
# Every binary that `test` runs is built and run here too, with the same
# instrumentation, so the tracefile counts what each of them reaches.
generate_coverage_report: tests_locale
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests.c $(SRC_OBJS) \
		-o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative \
		--capture --directory . --output-file chttpserver.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver.c.info \
		--output-directory coverage/tests && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_tls.c $(SRC_OBJS) \
		-o tests_tls $(LFLAGS) && \
	./tests_tls && \
	gcov -d -w chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative \
		--capture --directory . --output-file chttpserver_tls.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_tls.c.info \
		--output-directory coverage/tests_tls && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_tls
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_mem_mgmt.c $(SRC_OBJS) \
		-o tests_mem_mgmt $(LFLAGS) && \
	./tests_mem_mgmt && \
	gcov -d -w chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative \
		--capture --directory . --output-file chttpserver_mem_mgmt.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_mem_mgmt.c.info \
		--output-directory coverage/tests_mem_mgmt && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_mem_mgmt
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_engine_stop.c $(SRC_OBJS) \
		-o tests_engine_stop $(LFLAGS) && \
	./tests_engine_stop && \
	gcov -d -w chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative \
		--capture --directory . --output-file chttpserver_engine_stop.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_engine_stop.c.info \
		--output-directory coverage/tests_engine_stop && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_engine_stop
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_worker_key.c $(SRC_OBJS) \
		-o tests_worker_key $(LFLAGS) && \
	./tests_worker_key && \
	gcov -d -w chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative \
		--capture --directory . --output-file chttpserver_worker_key.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_worker_key.c.info \
		--output-directory coverage/tests_worker_key && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_worker_key
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) \
		-o tests_sigpipe $(LFLAGS) && \
	./tests_sigpipe && \
	gcov -d -w chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative \
		--capture --directory . --output-file chttpserver_sigpipe.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_sigpipe.c.info \
		--output-directory coverage/tests_sigpipe && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_sigpipe

all: build build_tls build_mem_mgmt build_engine_stop build_worker_key build_sigpipe build_tsan test test_tsan build_engine_stop_tsan test_engine_stop_tsan memtest generate_coverage_report

# The steps of `all` rebuild and run the same object files and binaries. The
# coverage step rebuilds them with instrumentation, and test and memtest run
# the binaries that the build step links, so under make -j the steps would
# overlap and link or run a half-rebuilt mix. .NOTPARALLEL runs the
# prerequisites of `all` one after another in the order listed, while the work
# inside each step still runs in parallel. A make older than 4.4 ignores the
# prerequisite list and runs the whole Makefile serially, which is correct
# too.
.NOTPARALLEL: all

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags *.o tests tests_tls tests_mem_mgmt tests_engine_stop tests_worker_key tests_sigpipe tests_tsan tests_engine_stop_tsan tests_locale third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_tls build_mem_mgmt build_engine_stop build_worker_key build_sigpipe
