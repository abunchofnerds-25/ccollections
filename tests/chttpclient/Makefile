CC = gcc

INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS

# Our source files (compiled with strict flags + -Werror). chttpclient.c owns
# its own static event_loop reactor (cthreadcomm.c) and uses ctls.c for TLS;
# no third_party/facio dependency at all (deleted). clogger.c is needed for
# chttpcli_set_engine_logger.
# cthreadpool.c is a transitive dependency of the async engine's DNS/connect
# worker pool (g_client_dns_pool).
#
# Two binaries are built from this shared source set: `tests` (the main
# suite, tests.c) and `tests_tls` (tests_tls.c), a dedicated suite that
# drives a real TLS handshake against a genuine self-signed cert generated
# at test-startup via the `openssl` CLI. tests_tls remains its own SEPARATE
# BINARY, matching this project's established precedent for a TLS-handshake-
# focused suite, even though ctls.c itself never aborts the process on bad
# input (unlike the vendored facio TLS layer this module replaced).
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

CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror

LFLAGS = -lpthread -lssl -lcrypto -lz -lm

build:
	$(CC) $(CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests $(LFLAGS)

build_tls:
	$(CC) $(CFLAGS) tests_tls.c $(COMMON_SRC_FILES) -o tests_tls $(LFLAGS)

# tests_parser exercises chttp1_parser.c directly (no chttpclient.c at all)
# with hand-crafted byte sequences, including malformed/hostile input the
# real-socket-based tests/tests_tls binaries structurally cannot reach
# (their mock servers only ever send well-formed responses); its own
# "stream" test group uses local AF_UNIX socketpairs to exercise
# chttp1_stream_t, but this remains fully independent of any real network
# traffic. chttp1_parser.c links ctls.c (+ its own chashmap.c/common.c
# dependencies) now that chttp1_stream_t optionally drives a TLS connection
# via ctls_conn_read/_write -- see chttp1_parser.h's own module doc comment.
# Kept as its own binary in this same directory, mirroring the tests_tls
# precedent, rather than a new top-level tests/ directory.
build_parser:
	$(CC) $(CFLAGS) tests_parser.c ../../src/chttp1_parser.c ../../src/ctls.c \
		../../src/chashmap.c ../../src/common.c -o tests_parser $(LFLAGS)

test: build build_tls build_parser
	./tests
	./tests_tls
	./tests_parser

memtest: build build_tls build_parser
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=ld_so_dlopen.supp ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_tls
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=openssl_internal.supp ./tests_parser

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests.c \
		$(COMMON_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file chttpclient.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient.c.info --output-directory coverage/tests && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_tls.c \
		$(COMMON_SRC_FILES) -o tests_tls $(LFLAGS) && \
	./tests_tls && \
	gcov -d -w tests_tls-chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file chttpclient_tls.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient_tls.c.info --output-directory coverage/tests_tls && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_tls
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_parser.c ../../src/chttp1_parser.c \
		../../src/ctls.c ../../src/chashmap.c ../../src/common.c -o tests_parser $(LFLAGS) && \
	./tests_parser && \
	gcov -d -w tests_parser-chttp1_parser && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file chttp1_parser.c.info && \
	genhtml --ignore-errors inconsistent -s chttp1_parser.c.info --output-directory coverage/tests_parser && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_parser

all: build build_tls build_parser test memtest generate_coverage_report

clean:
	rm -rf tests tests_tls tests_parser third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_tls build_parser
