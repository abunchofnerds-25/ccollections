CC = gcc

INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS

# Our source files (compiled with strict flags + -Werror). chttpserver.c and
# chttpclient.c each own a fully independent static event_loop reactor.
#
# Four binaries are built from this shared source set:
#   tests            (tests.c) - the main suite.
#   tests_tls        (tests_tls.c) - drives a real TLS handshake against a
#                     genuine self-signed cert generated at test-startup via
#                     the `openssl` CLI. Kept as a SEPARATE BINARY matching
#                     tests/chttpclient/tests_tls.c's own precedent for a
#                     TLS-handshake-focused suite.
#   tests_mem_mgmt   (tests_mem_mgmt.c) - calls
#                     chttpsvr_set_engine_mem_mgmt_procs() before the first
#                     chttpsvr_start() in the process, a process-wide,
#                     set-once-ever requirement. tests.c's own shared _setup()
#                     already calls chttpsvr_start(), so the "install procs,
#                     then start" happy path can only be exercised in a
#                     process where nothing else has started the engine
#                     first -- hence its own SEPARATE BINARY.
#   tests_engine_stop (tests_engine_stop.c) - exercises chttpsvr_engine_stop()
#                     (the forced, signal-handler-driven engine shutdown
#                     path), which tears down the one shared, process-wide
#                     event_loop reactor out from under every currently
#                     started chttpsvr instance. tests.c's own g_srv/g_srv2/...
#                     stay started across its entire run, so force-stopping
#                     the shared engine there would yank the reactor out from
#                     under them and fail unrelated tests -- hence its own
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
	../../src/common.c

CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror -fPIC

LFLAGS = -lpthread -lssl -lcrypto -lm -lz

build:
	$(CC) $(CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests $(LFLAGS)

build_tls:
	$(CC) $(CFLAGS) tests_tls.c $(COMMON_SRC_FILES) -o tests_tls $(LFLAGS)

build_mem_mgmt:
	$(CC) $(CFLAGS) tests_mem_mgmt.c $(COMMON_SRC_FILES) -o tests_mem_mgmt $(LFLAGS)

build_engine_stop:
	$(CC) $(CFLAGS) tests_engine_stop.c $(COMMON_SRC_FILES) -o tests_engine_stop $(LFLAGS)

test: build build_tls build_mem_mgmt build_engine_stop
	./tests
	./tests_tls
	./tests_mem_mgmt
	./tests_engine_stop

memtest: build build_tls build_mem_mgmt build_engine_stop
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_tls
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_mem_mgmt
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_engine_stop

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests.c $(COMMON_SRC_FILES) \
		-o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver.c.info \
		--output-directory coverage/tests && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_tls.c $(COMMON_SRC_FILES) \
		-o tests_tls $(LFLAGS) && \
	./tests_tls && \
	gcov -d -w tests_tls-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_tls.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_tls.c.info \
		--output-directory coverage/tests_tls && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_tls
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_mem_mgmt.c $(COMMON_SRC_FILES) \
		-o tests_mem_mgmt $(LFLAGS) && \
	./tests_mem_mgmt && \
	gcov -d -w tests_mem_mgmt-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_mem_mgmt.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_mem_mgmt.c.info \
		--output-directory coverage/tests_mem_mgmt && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_mem_mgmt
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_engine_stop.c $(COMMON_SRC_FILES) \
		-o tests_engine_stop $(LFLAGS) && \
	./tests_engine_stop && \
	gcov -d -w tests_engine_stop-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_engine_stop.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_engine_stop.c.info \
		--output-directory coverage/tests_engine_stop && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_engine_stop

all: build build_tls build_mem_mgmt build_engine_stop test memtest generate_coverage_report

clean:
	rm -rf tests tests_tls tests_mem_mgmt tests_engine_stop third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_tls build_mem_mgmt build_engine_stop
