
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64

# Our source files, which the build compiles with strict flags and -Werror.
# chttpserver.c and chttpclient.c each own a fully separate static
# ccol_event_loop reactor.
#
# This shared source set builds six binaries:
#   tests            (tests.c) - the main suite.
#   tests_tls        (tests_tls.c) - drives a real TLS handshake against a
#                     real self-signed certificate, which the `openssl`
#                     command line tool generates as the tests start. It is
#                     a SEPARATE BINARY only as a choice of test layout,
#                     which keeps the cases about the handshake together
#                     and out of the main suite.
#   tests_mem_mgmt   (tests_mem_mgmt.c) - calls
#                     chttpsvr_set_engine_mem_mgmt_procs() before the first
#                     chttpsvr_start() in the process. That order is a rule
#                     for the whole process, and you can set the procs only
#                     one time ever. The shared _setup() of tests.c already
#                     calls chttpsvr_start(), so the happy path of "install
#                     the procs, then start" needs a process where nothing
#                     else started the engine first, which is why it has
#                     its own SEPARATE BINARY.
#   tests_engine_stop (tests_engine_stop.c) - exercises
#                     chttpsvr_engine_stop(), the forced engine shutdown
#                     path that a signal handler drives. That path tears
#                     the one shared ccol_event_loop reactor of the process
#                     down, under every chttpsvr instance that is started
#                     at that moment. The g_srv, g_srv2 and other servers
#                     of tests.c stay started for its whole run, so a forced
#                     stop of the shared engine there would pull the reactor
#                     out from under them and fail unrelated tests. That is
#                     why it has its own SEPARATE BINARY, following the same
#                     isolation reasoning as tests_tls and tests_mem_mgmt
#                     above.
#   tests_worker_key (tests_worker_key.c) - takes every thread-specific key
#                     that the process can have before the first
#                     chttpsvr_start() creates the key that marks worker
#                     threads. The shared _setup() of tests.c already created
#                     that key, so this needs a fresh process, and it is a
#                     SEPARATE BINARY for that reason.
#   tests_sigpipe    (tests_sigpipe.c) - runs with the default disposition of
#                     SIGPIPE, which ends the process. Every other binary
#                     here ignores SIGPIPE, and an ignored SIGPIPE is
#                     discarded when it is raised, so only a binary of its
#                     own can see a write of the server that raises it, or a
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
# The build compiles the sources of the library at the optimization level of
# the shipped library, so these tests exercise the code that ships instead of
# a debug build that nobody installs. It builds only the test files of this
# directory at -O0, where the extra debuggability is worth having and the
# object code is not a part of the product. Loosen SRC_OPT for a short time
# when you must step through a failure.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag, and it stays out of CFLAGS_COMMON on
# purpose: SRC_CFLAGS drives calls that only compile, and the driver of clang
# treats an unused linker argument there as an error under -Werror, while gcc
# only discards it. This flag belongs on CFLAGS, which does the real link, so
# it goes there instead.
LD_HARDENING = $(CCOL_LD_HARDENING)
CFLAGS_COMMON = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) \
	-Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror -fPIC
include ../hardening.mk

CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The build makes the library objects one time for this directory and links
# them into every binary that it produces, instead of recompiling them for
# each binary.
SRC_OBJS = $(notdir $(COMMON_SRC_FILES:.c=.o))
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 -fPIC $(EXTRA_CFLAGS)

LFLAGS = -lpthread -lssl -lcrypto -lm -lz -rdynamic $(CCOL_PLATFORM_LIBS)

# This is empty for an ordinary build. generate_coverage_report passes
# COVERAGE_ON in, so that the same pattern rule instruments the library
# objects.
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage

# An object file carries no record of how the build made it, so without this
# stamp a change of compiler or of flags relinks stale objects of the wrong
# architecture, or objects with no instrumentation in them; a native build
# followed by `make CC="gcc -m32" test`, or by a coverage run, does that. This
# stamp holds the current compiler and flags, and the build rewrites it only
# when they really change, so the objects rebuild when they must and stay
# cached when they need not.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# Pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build, and generate_coverage_report sets it, so one rule serves
# both. include/internal/ has its own line because a *.h wildcard cannot match
# a subdirectory. Without the second pattern, no internal header is a
# prerequisite: an edit to one of them then recompiles nothing here, and a
# later edit to a single .c file gives a binary whose objects disagree about
# that header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

build: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)

# A separate ThreadSanitizer build of the main suite alone, which stays out of
# the default -Werror build. tests/cthreadcomm/Makefile has the same
# build_tsan and test_tsan pattern.
build_tsan:
	$(CC) $(TSAN_CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# die_after_fork=0: tests/chttpclient/Makefile has the same comment on its own
# test_tsan target, and the same root cause, verified the same way, applies
# here: a persistent, multi-threaded ccol_event_loop reactor for the whole
# process, alive at the moment of a fork(). It trips the die_after_fork=1
# default of TSan as soon as the forked child of
# concurrent_double_destroy_is_fatal creates its own two racing pthreads.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan

# A separate ThreadSanitizer build of tests_engine_stop.c, which follows
# build_tsan and test_tsan above exactly. Of the six binaries in this
# directory, this one holds by far the most races over forks and threads:
# reaper against destroy, start against stop, listener against dispatch, and
# a concurrent destroy against engine_stop. Dedicated white-box hooks that
# arm, wait and release drive each one, and those hooks exist to manufacture
# data races in a deterministic way, so this is the suite that gains the most
# from the one tool that best catches the class of bug that it reproduces.
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


# Aliases for convenience, so that the name of a binary that this suite
# produces also works as a target. Each one builds that binary, while
# `make test` builds and runs every one of them and stays the normal entry
# point.
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
		--error-exitcode=1 ./tests_worker_key
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_sigpipe

# --ignore-errors negative: the coverage counters of gcov are plain, non-atomic
# increments, so they can race when instrumented code runs concurrently and
# then give a false negative hit count. This affects more than one module and
# also happens inside a glibc header; it is a limit of the gcov
# instrumentation, not a defect in the logic of this library. See the same
# flag in tests/chttpclient/Makefile for the full reasons.
generate_coverage_report:
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

all: build build_tls build_mem_mgmt build_engine_stop build_worker_key build_sigpipe build_tsan test test_tsan build_engine_stop_tsan test_engine_stop_tsan memtest generate_coverage_report

# The steps of `all` rebuild and run the same object files and binaries: the
# coverage step rebuilds them with instrumentation, and the test and memtest
# steps run the binaries that the build step links. Under make -j those steps
# would overlap and link or run a half-rebuilt mix. .NOTPARALLEL runs the
# prerequisites of `all` one after another, in the order listed, while the
# work inside each step still runs in parallel. A make older than 4.4 ignores
# the prerequisite list and runs this whole Makefile serially, which is also
# correct.
.NOTPARALLEL: all

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags *.o tests tests_tls tests_mem_mgmt tests_engine_stop tests_worker_key tests_sigpipe tests_tsan tests_engine_stop_tsan third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_tls build_mem_mgmt build_engine_stop build_worker_key build_sigpipe
