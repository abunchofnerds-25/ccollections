CC = gcc

INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64

# Our source files (compiled with strict flags + -Werror). chttpserver.c and
# chttpclient.c each own a fully independent static ccol_event_loop reactor.
#
# Four binaries are built from this shared source set:
#   tests            (tests.c) - the main suite.
#   tests_tls        (tests_tls.c) - drives a real TLS handshake against a
#                     genuine self-signed cert generated at test-startup via
#                     the `openssl` CLI. Kept as a SEPARATE BINARY purely as
#                     a test-layout choice, keeping the handshake-focused
#                     cases together and out of the main suite.
#   tests_mem_mgmt   (tests_mem_mgmt.c) - calls
#                     chttpsvr_set_engine_mem_mgmt_procs() before the first
#                     chttpsvr_start() in the process, a process-wide,
#                     set-once-ever requirement. tests.c's own shared _setup()
#                     already calls chttpsvr_start(), so the "install procs,
#                     then start" happy path can only be exercised in a
#                     process where nothing else has started the engine
#                     first; hence its own SEPARATE BINARY.
#   tests_engine_stop (tests_engine_stop.c) - exercises chttpsvr_engine_stop()
#                     (the forced, signal-handler-driven engine shutdown
#                     path), which tears down the one shared, process-wide
#                     ccol_event_loop reactor out from under every currently
#                     started chttpsvr instance. tests.c's own g_srv/g_srv2/...
#                     stay started across its entire run, so force-stopping
#                     the shared engine there would yank the reactor out from
#                     under them and fail unrelated tests; hence its own
#                     SEPARATE BINARY, matching the same isolation reasoning
#                     as tests_tls/tests_mem_mgmt above.
COMMON_SRC_FILES = ../../src/chttpserver.c \
	../../src/chttpclient.c \
	../../src/chttp1_parser.c \
	../../src/chttp.c \
	../../src/cthreadcomm.c \
	../../src/cthreadpool.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cvector.c \
	../../src/ctls.c \
	../../src/common.c ../../src/cpintable.c

EXTRA_CFLAGS ?=
# The library's own sources are compiled at the optimization level the shipped
# library uses, so what these tests exercise is the code that ships rather than
# a debug build nobody installs. Only this directory's own test files are built
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
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror -fPIC
CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The library objects are built once for this directory and linked into every
# binary it produces, rather than recompiled once per binary.
SRC_OBJS = $(notdir $(COMMON_SRC_FILES:.c=.o))
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1 -fPIC

LFLAGS = -lpthread -lssl -lcrypto -lm -lz -rdynamic

# Empty for an ordinary build; generate_coverage_report passes COVERAGE_ON in
# so the same pattern rule instruments the library objects.
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage

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

# Separate ThreadSanitizer build of the main suite only, kept out of the
# default -Werror build, mirroring tests/cthreadcomm/Makefile's identical
# build_tsan/test_tsan pattern.
build_tsan:
	$(CC) $(TSAN_CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

# die_after_fork=0: see tests/chttpclient/Makefile's identical comment on
# its own test_tsan target; the same root cause (a persistent, process-wide
# multi-threaded ccol_event_loop reactor alive at fork() time, tripping TSan's own
# die_after_fork=1 default the moment concurrent_double_destroy_is_fatal's
# forked child creates its own two racing pthreads) applies here too,
# verified the same way.
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_tsan

# Separate ThreadSanitizer build of tests_engine_stop.c, mirroring build_tsan/
# test_tsan above exactly: this is by far the most fork/thread-race-heavy of
# the four binaries in this directory (reaper-vs-destroy, start-vs-stop,
# listener-dispatch, and concurrent-destroy-vs-engine_stop races, each driven
# by dedicated white-box arm/wait/release hooks built specifically to
# manufacture data races deterministically), so it is exactly the suite most
# worth running under the one tool best suited to catch the class of bug it
# is designed to reproduce.
build_engine_stop_tsan:
	$(CC) $(TSAN_CFLAGS) tests_engine_stop.c $(COMMON_SRC_FILES) -o tests_engine_stop_tsan $(LFLAGS) -fsanitize=thread

test_engine_stop_tsan: build_engine_stop_tsan
	TSAN_OPTIONS="die_after_fork=0" ./tests_engine_stop_tsan

build_tls: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_tls.c $(SRC_OBJS) -o tests_tls $(LFLAGS)

build_mem_mgmt: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_mem_mgmt.c $(SRC_OBJS) -o tests_mem_mgmt $(LFLAGS)

build_engine_stop: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_engine_stop.c $(SRC_OBJS) -o tests_engine_stop $(LFLAGS)

test: build build_tls build_mem_mgmt build_engine_stop
	./tests
	./tests_tls
	./tests_mem_mgmt
	./tests_engine_stop

memtest: build build_tls build_mem_mgmt build_engine_stop
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_tls
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_mem_mgmt
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes ./tests_engine_stop

# --ignore-errors negative: gcov's own coverage counters are plain (non-atomic)
# increments, which can race under concurrently-executed instrumented code and
# produce a spurious negative hit count. This affects more than one module,
# including inside a glibc header, and is a limitation of gcov's own
# instrumentation rather than a defect in this library's logic; see
# tests/chttpclient/Makefile's identical flag for the full rationale.
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

all: build build_tls build_mem_mgmt build_engine_stop build_tsan test test_tsan build_engine_stop_tsan test_engine_stop_tsan memtest generate_coverage_report

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags *.o tests tests_tls tests_mem_mgmt tests_engine_stop tests_tsan tests_engine_stop_tsan third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_tls build_mem_mgmt build_engine_stop
