CC = gcc

INCLUDES = -I.. -I../../include -I../../third_party/facio -I../../third_party/llhttp
DEFINITIONS = -DRUNNING_UNIT_TESTS -DHAVE_OPENSSL=1

# Our source files (compiled with strict flags + -Werror). cfio_engine.c is
# the shared facio reactor lifecycle module both chttpserver.c and
# chttpclient.c acquire/release references to -- see cfio_engine.h.
#
# Three binaries are built from this shared source set:
#   tests            (tests.c) - the main suite.
#   tests_tls        (tests_tls.c) - drives a real TLS handshake against a
#                     genuine self-signed cert generated at test-startup via
#                     the `openssl` CLI. Kept as a SEPARATE BINARY because a
#                     missing/broken openssl CLI or an invalid cert file makes
#                     the vendored facio TLS layer call FIO_LOG_FATAL, which
#                     aborts the whole process -- isolating it means that
#                     failure mode can only take down the TLS suite, not the
#                     rest of this directory's tests.
#   tests_mem_mgmt   (tests_mem_mgmt.c) - calls
#                     chttpsvr_set_engine_mem_mgmt_procs() before the first
#                     chttpsvr_start() in the process, a process-wide,
#                     set-once-ever requirement. tests.c's own shared _setup()
#                     already calls chttpsvr_start(), so the "install procs,
#                     then start" happy path can only be exercised in a
#                     process where nothing else has started the engine
#                     first -- hence its own SEPARATE BINARY.
COMMON_SRC_FILES = ../../src/chttpserver.c \
	../../src/chttpclient.c \
	../../src/chttp.c \
	../../src/cfio_engine.c \
	../../src/cthreadpool.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cvector.c \
	../../src/common.c

# third_party/facio source files (compiled without -Werror; several warnings are
# expected from the third party facil.io library).
THIRD_PARTY_DIR = ../../third_party/facio
THIRD_PARTY_SRC_FILES = \
	$(THIRD_PARTY_DIR)/fio.c \
	$(THIRD_PARTY_DIR)/fiobject.c \
	$(THIRD_PARTY_DIR)/fiobj_str.c \
	$(THIRD_PARTY_DIR)/fiobj_hash.c \
	$(THIRD_PARTY_DIR)/fiobj_ary.c \
	$(THIRD_PARTY_DIR)/fiobj_numbers.c \
	$(THIRD_PARTY_DIR)/fiobj_data.c \
	$(THIRD_PARTY_DIR)/http.c \
	$(THIRD_PARTY_DIR)/http1.c \
	$(THIRD_PARTY_DIR)/http_internal.c \
	$(THIRD_PARTY_DIR)/fio_tls_openssl.c

# llhttp (HTTP/1.1 parser used by chttpclient.c, which this suite links
# directly). Kept in its own object subdirectory: llhttp's http.c and facio's
# http.c share a basename, so a shared flat object dir would clobber one with
# the other depending on build order.
LLHTTP_DIR = ../../third_party/llhttp
LLHTTP_SRC_FILES = $(LLHTTP_DIR)/llhttp.c $(LLHTTP_DIR)/api.c $(LLHTTP_DIR)/http.c
LLHTTP_HEADER_FILES = $(wildcard $(LLHTTP_DIR)/*.h)
LLHTTP_OBJS = $(LLHTTP_SRC_FILES:$(LLHTTP_DIR)/%.c=third_party_obj/llhttp/%.o)

CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror -fPIC

THIRD_PARTY_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fPIC -g -O0 \
	-Wno-deprecated-declarations -Wno-cast-function-type \
	-Wno-unused-parameter -Wno-sign-compare -Wno-type-limits

# llhttp's generated state machine only ever triggers -Wunused-parameter
# (verified under -Wall -Wextra); reusing facio's broader suppression list
# would silently mask a real regression on a future llhttp re-vendor.
LLHTTP_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fPIC -g -O0 -Wno-unused-parameter

LFLAGS = -lpthread -lssl -lcrypto -lm -lz

THIRD_PARTY_OBJS = $(THIRD_PARTY_SRC_FILES:$(THIRD_PARTY_DIR)/%.c=third_party_obj/facio/%.o)
THIRD_PARTY_HEADER_FILES = $(wildcard $(THIRD_PARTY_DIR)/*.h)

third_party_obj/facio third_party_obj/llhttp:
	mkdir -p $@

third_party_obj/facio/%.o: $(THIRD_PARTY_DIR)/%.c $(THIRD_PARTY_HEADER_FILES) | third_party_obj/facio
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

third_party_obj/llhttp/%.o: $(LLHTTP_DIR)/%.c $(LLHTTP_HEADER_FILES) | third_party_obj/llhttp
	$(CC) -c $(LLHTTP_CFLAGS) $< -o $@

build: $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) tests.c $(COMMON_SRC_FILES) -o tests $(LFLAGS)

build_tls: $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) tests_tls.c $(COMMON_SRC_FILES) -o tests_tls $(LFLAGS)

build_mem_mgmt: $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) tests_mem_mgmt.c $(COMMON_SRC_FILES) -o tests_mem_mgmt $(LFLAGS)

test: build build_tls build_mem_mgmt
	./tests
	./tests_tls
	./tests_mem_mgmt

memtest: build build_tls build_mem_mgmt
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_tls
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_mem_mgmt

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests.c $(COMMON_SRC_FILES) \
		$(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver.c.info \
		--output-directory coverage/tests && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_tls.c $(COMMON_SRC_FILES) \
		$(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) -o tests_tls $(LFLAGS) && \
	./tests_tls && \
	gcov -d -w tests_tls-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_tls.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_tls.c.info \
		--output-directory coverage/tests_tls && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_tls
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_mem_mgmt.c $(COMMON_SRC_FILES) \
		$(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) -o tests_mem_mgmt $(LFLAGS) && \
	./tests_mem_mgmt && \
	gcov -d -w tests_mem_mgmt-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_mem_mgmt.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_mem_mgmt.c.info \
		--output-directory coverage/tests_mem_mgmt && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_mem_mgmt

all: build build_tls build_mem_mgmt test memtest generate_coverage_report

clean:
	rm -rf tests tests_tls tests_mem_mgmt third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_tls build_mem_mgmt
