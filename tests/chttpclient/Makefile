CC = gcc

INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64

# Our source files (compiled with strict flags + -Werror). chttpclient.c owns
# its own static ccol_event_loop reactor (cthreadcomm.c) and uses ctls.c for TLS.
# clogger.c is needed for chttpcli_set_engine_logger.
# cthreadpool.c is a transitive dependency of the async engine's DNS/connect
# worker pool (g_client_dns_pool).
#
# One binary, `tests` (tests.c), covers this whole directory's main suite,
# including a real TLS handshake against a genuine self-signed cert generated
# at test-startup via the `openssl` CLI. That TLS coverage needs no separate
# binary of its own: ctls.c never aborts the process on bad input (it reports
# failure via return value), so there is no blast radius to isolate.
COMMON_SRC_FILES = ../../src/chttpclient.c \
	../../src/chttp1_parser.c \
	../../src/chttp.c \
	../../src/cthreadcomm.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cthreadpool.c \
	../../src/cvector.c \
	../../src/ctls.c \
	../../src/common.c

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
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror
CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The library objects are built once for this directory and linked into every
# binary it produces, rather than recompiled once per binary. PARSER_OBJS is
# the subset the two parser-only binaries need.
SRC_OBJS = $(notdir $(COMMON_SRC_FILES:.c=.o))
PARSER_OBJS = chttp1_parser.o ctls.o chashmap.o common.o
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1

LFLAGS = -lpthread -lssl -lcrypto -lz -lm

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
%.o: ../../src/%.c .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

build: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)

# Separate ThreadSanitizer build of the main suite only (the multi-threaded
# async engine is what this is meant to catch races in), kept out of the
# default -Werror build, mirroring tests/cthreadcomm/Makefile's identical
# build_tsan/test_tsan pattern.
build_tsan:
	$(CC) $(TSAN_CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

# die_after_fork=0: this suite's own concurrent_double_destroy_is_fatal test
# forks, then creates two NEW pthreads in the child to race a double-destroy.
# chttpclient's async engine keeps a persistent, process-wide multi-threaded
# ccol_event_loop reactor alive for the whole suite, so by the time this test
# runs, the parent is multi-threaded at fork() time; exactly the condition
# TSan's own default die_after_fork=1 treats as unsafe for a child that then
# creates further threads, silently making the child exit normally instead
# of reaching this test's own expected ccol_fatal_err()/SIGABRT. Only
# fork()-then-new-threads under TSan's own safety net is affected; the
# child's chttpclient_destroy logic is correct and race-free under gcc/clang,
# -O0/-O1, TSan or not. tests/cthreadcomm's otherwise-identical test needs no
# such option because its ccol_event_loop instances are torn down between
# tests, leaving ITS OWN parent single-threaded by the time it forks.
#
# suppressions=tsan_openssl.supp: see that file's own comment for the one
# known, third-party-only OpenSSL race it suppresses (concurrent client
# handshakes verifying against one shared trust store).
test_tsan: build_tsan
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_openssl.supp" ./tests_tsan

# Destroying the handle chttp_default_client() returns is a one-shot,
# process-global, unrecoverable action (default_client_bundler.client is
# never rebuilt once cleared; see chttp_default_client's own doc comment),
# so it must not run inside tests.c, where it would permanently break every
# other test's use of chttp_do/chttp_get for the rest of that process.
# Mirrors the tests_parser precedent of an additional binary within this
# same directory rather than a new top-level tests/ directory.
build_default_client: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_default_client.c $(SRC_OBJS) \
		-o tests_default_client $(LFLAGS)

# tests_parser exercises chttp1_parser.c directly (no chttpclient.c at all)
# with hand-crafted byte sequences, including malformed/hostile input the
# real-socket-based tests binary structurally cannot reach (its mock server
# only ever sends well-formed responses); its own "stream" test group uses
# local AF_UNIX socketpairs to exercise chttp1_stream_t, but this remains
# fully independent of any real network traffic. chttp1_parser.c links
# ctls.c (+ its own chashmap.c/common.c dependencies) because
# chttp1_stream_t optionally drives a TLS connection via ctls_conn_read/
# _write; see chttp1_parser.h's own module doc comment. Kept as its own
# binary in this same directory rather than a new top-level tests/
# directory.
build_parser: $(PARSER_OBJS)
	$(CC) $(CFLAGS) tests_parser.c $(PARSER_OBJS) -o tests_parser $(LFLAGS)

# libFuzzer targets for chttp1_parser.c, request and response mode
# separately (see fuzz/fuzz_common.h for why: request-line and status-line
# grammars are different enough to warrant separate corpora, not one shared
# pile). Requires clang (libFuzzer is a Clang/LLVM feature). Mirrors
# tests/cyaml/Makefile's own `fuzz` target exactly, including its own
# .PHONY rationale: a fuzz/corpus_request or fuzz/corpus_response directory
# already exists on disk, which would otherwise make Make consider a bare
# `fuzz_request`/`fuzz_response` target already satisfied and skip the
# recipe entirely. Run ./fuzz_chttp1_request fuzz/corpus_request
# [-max_total_time=N] etc; see the libFuzzer docs for flags.
FUZZ_CC = clang
FUZZ_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=fuzzer,address,undefined -g3 -O1
FUZZ_PARSER_SRC_FILES = ../../src/chttp1_parser.c ../../src/ctls.c \
	../../src/chashmap.c ../../src/common.c

.PHONY: fuzz_request fuzz_response
fuzz_request:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_chttp1_request.c $(FUZZ_PARSER_SRC_FILES) \
		-o fuzz_chttp1_request $(LFLAGS)

fuzz_response:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_chttp1_response.c $(FUZZ_PARSER_SRC_FILES) \
		-o fuzz_chttp1_response $(LFLAGS)

# tests_differential cross-checks chttp1_parser.c against picohttpparser
# (vendored under picohttpparser/, an independent, production-deployed
# HTTP/1.x parser used by the h2o web server) on the same raw byte sequences,
# to catch a framing/extraction bug neither parser's own self-consistent test
# suite alone would surface. Kept as its own binary in this same directory
# rather than a new top-level tests/ directory, matching the tests_parser/
# tests_default_client precedent above; picohttpparser.c is only needed by
# this one binary, so it is not added to COMMON_SRC_FILES.
# picohttpparser is a vendored comparison implementation, not library source,
# so it stays with the test-side flags rather than the shipped ones.
build_differential: $(PARSER_OBJS)
	$(CC) $(CFLAGS) tests_differential.c $(PARSER_OBJS) \
		picohttpparser/picohttpparser.c -o tests_differential $(LFLAGS)

test: build build_parser build_default_client build_differential
	./tests
	./tests_parser
	./tests_default_client
	./tests_differential

memtest: build build_parser build_default_client build_differential
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes --suppressions=ld_so_dlopen.supp \
		--suppressions=openssl_internal.supp ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=openssl_internal.supp ./tests_parser
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=ld_so_dlopen.supp ./tests_default_client
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_differential

# --ignore-errors negative: gcov's own coverage counters are plain
# (non-atomic) increments, so this suite's own heavily multi-threaded async
# engine can race two threads incrementing the same counter concurrently,
# occasionally producing an apparently negative delta when gcov reconciles
# counts. The failure is non-deterministic, landing on a different
# file/line/value from run to run. This is gcov's own
# instrumentation limitation for concurrent code, not a defect in this
# library's logic, matching the same tolerate-a-known-tool-quirk rationale
# already applied here to mismatch/inconsistent.
generate_coverage_report:
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttpclient.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient.c.info --output-directory coverage/tests && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(PARSER_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_parser.c $(PARSER_OBJS) \
		-o tests_parser $(LFLAGS) && \
	./tests_parser && \
	gcov -d -w chttp1_parser && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttp1_parser.c.info && \
	genhtml --ignore-errors inconsistent -s chttp1_parser.c.info --output-directory coverage/tests_parser && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_parser
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(PARSER_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_differential.c $(PARSER_OBJS) \
		picohttpparser/picohttpparser.c -o tests_differential $(LFLAGS) && \
	./tests_differential && \
	gcov -d -w chttp1_parser && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttp1_parser_differential.c.info && \
	genhtml --ignore-errors inconsistent -s chttp1_parser_differential.c.info --output-directory coverage/tests_differential && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_differential

all: build build_parser build_default_client build_differential build_tsan test test_tsan memtest generate_coverage_report

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags *.o tests tests_tls tests_parser tests_default_client tests_differential tests_tsan third_party_obj coverage *.gcno *.gcda *.gcov *.c.info \
		fuzz_chttp1_request fuzz_chttp1_response

default: build build_parser build_default_client build_differential
