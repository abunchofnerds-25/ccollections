CC = gcc

INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS

# Our source files (compiled with strict flags + -Werror). chttpclient.c owns
# its own static event_loop reactor (cthreadcomm.c) and uses ctls.c for TLS.
# clogger.c is needed for chttpcli_set_engine_logger.
# cthreadpool.c is a transitive dependency of the async engine's DNS/connect
# worker pool (g_client_dns_pool).
#
# One binary, `tests` (tests.c), covers this whole directory's main suite,
# including a real TLS handshake against a genuine self-signed cert generated
# at test-startup via the `openssl` CLI. That TLS coverage used to live in
# its own separate tests_tls binary; ctls.c (unlike the vendored TLS layer it
# replaced) never aborts the process on bad input, so the isolation a
# separate binary bought is no longer needed, and the two were merged.
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
CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-strong -Wstrict-overflow \
	-Wl,-z,relro,-z,now \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror $(EXTRA_CFLAGS)
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fsanitize=thread -Wall -Wextra -g -O1

LFLAGS = -lpthread -lssl -lcrypto -lz -lm

build:
	$(CC) $(CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests $(LFLAGS)

# Separate ThreadSanitizer build of the main suite only (the multi-threaded
# async engine is what this is meant to catch races in), kept out of the
# default -Werror build, mirroring tests/cthreadcomm/Makefile's identical
# build_tsan/test_tsan pattern.
build_tsan:
	$(CC) $(TSAN_CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread

# die_after_fork=0: this suite's own concurrent_double_destroy_is_fatal test
# forks, then creates two NEW pthreads in the child to race a double-destroy.
# chttpclient's async engine keeps a persistent, process-wide multi-threaded
# event_loop reactor alive for the whole suite, so by the time this test
# runs, the parent is multi-threaded at fork() time; exactly the condition
# TSan's own default die_after_fork=1 treats as unsafe for a child that then
# creates further threads, silently making the child exit normally instead
# of reaching this test's own expected fatal_err()/SIGABRT (verified: the
# child's chttpclient_destroy logic itself is correct and race-free under
# gcc/clang, -O0/-O1, TSan or not; only fork()-then-new-threads under TSan's
# own safety net is affected, confirmed via TSAN_OPTIONS=die_after_fork=0
# reproducibly fixing it, and via TSan's own diagnostic output showing this
# process is not single-threaded at fork time, unlike tests/cthreadcomm's
# otherwise-identical test, whose event_loop instances are torn down between
# tests, keeping ITS OWN parent single-threaded by the time it forks).
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
build_default_client:
	$(CC) $(CFLAGS) tests_default_client.c $(COMMON_SRC_FILES) \
		-o tests_default_client $(LFLAGS)

# tests_parser exercises chttp1_parser.c directly (no chttpclient.c at all)
# with hand-crafted byte sequences, including malformed/hostile input the
# real-socket-based tests binary structurally cannot reach (its mock server
# only ever sends well-formed responses); its own "stream" test group uses
# local AF_UNIX socketpairs to exercise chttp1_stream_t, but this remains
# fully independent of any real network traffic. chttp1_parser.c links
# ctls.c (+ its own chashmap.c/common.c dependencies) now that
# chttp1_stream_t optionally drives a TLS connection via ctls_conn_read/
# _write; see chttp1_parser.h's own module doc comment. Kept as its own
# binary in this same directory rather than a new top-level tests/
# directory.
build_parser:
	$(CC) $(CFLAGS) tests_parser.c ../../src/chttp1_parser.c ../../src/ctls.c \
		../../src/chashmap.c ../../src/common.c -o tests_parser $(LFLAGS)

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
build_differential:
	$(CC) $(CFLAGS) tests_differential.c ../../src/chttp1_parser.c \
		../../src/ctls.c ../../src/chashmap.c ../../src/common.c \
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
# counts; reproduced directly (not merely reasoned about) as a NON-
# deterministic failure landing on a different file/line/value on repeated
# runs (once on chttpclient.c, once on cthreadcomm.c). This is gcov's own
# instrumentation limitation for concurrent code, not a defect in this
# library's logic, matching the same tolerate-a-known-tool-quirk rationale
# already applied here to mismatch/inconsistent.
generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests.c \
		$(COMMON_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttpclient.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient.c.info --output-directory coverage/tests && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_parser.c ../../src/chttp1_parser.c \
		../../src/ctls.c ../../src/chashmap.c ../../src/common.c -o tests_parser $(LFLAGS) && \
	./tests_parser && \
	gcov -d -w tests_parser-chttp1_parser && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttp1_parser.c.info && \
	genhtml --ignore-errors inconsistent -s chttp1_parser.c.info --output-directory coverage/tests_parser && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_parser
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_differential.c \
		../../src/chttp1_parser.c ../../src/ctls.c ../../src/chashmap.c \
		../../src/common.c picohttpparser/picohttpparser.c \
		-o tests_differential $(LFLAGS) && \
	./tests_differential && \
	gcov -d -w tests_differential-chttp1_parser && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttp1_parser_differential.c.info && \
	genhtml --ignore-errors inconsistent -s chttp1_parser_differential.c.info --output-directory coverage/tests_differential && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_differential

all: build build_parser build_default_client build_differential build_tsan test test_tsan memtest generate_coverage_report

clean:
	rm -rf tests tests_tls tests_parser tests_default_client tests_differential tests_tsan third_party_obj coverage *.gcno *.gcda *.gcov *.c.info \
		fuzz_chttp1_request fuzz_chttp1_response

default: build build_parser build_default_client build_differential
