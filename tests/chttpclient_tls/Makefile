CC = gcc

INCLUDES = -I.. -I../../include -I../../third_party/facio -I../../third_party/llhttp
DEFINITIONS = -DRUNNING_UNIT_TESTS -DHAVE_OPENSSL=1

# Our source files (compiled with strict flags + -Werror). clogger.c is a
# transitive dependency: facio's fio.c resolves its FIO_LOG_* macros there.
# cthreadpool.c is a transitive dependency of chttpclient's async engine's
# DNS/connect worker pool. cfio_engine.c is the shared facio reactor
# lifecycle module chttpclient's async engine acquires/releases references
# to -- see cfio_engine.h.
OUR_SRC_FILES = tests.c \
	../../src/chttpclient.c \
	../../src/cfio_engine.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cthreadpool.c \
	../../src/cvector.c \
	../../src/common.c

# cfio_engine.c's one-time global init unconditionally calls
# http_lib_constructor() (see cfio_engine.h), so this suite now transitively
# needs the same fiobj_*/http*/http_internal.c vendor files chttpserver's own
# suites need, not just fio.c + fio_tls_openssl.c. This suite's own mock TLS
# server (in tests.c) remains a hand-rolled raw-OpenSSL server rather than
# reusing chttpserver.c -- simpler to keep this TLS-handshake-focused suite
# free of chttpserver's routing/dispatch machinery, not because of any
# per-process engine restriction (chttpserver and chttpclient's async engine
# now safely share one process-wide reactor via cfio_engine.c).
FACIO_DIR = ../../third_party/facio
FACIO_SRC_FILES = $(FACIO_DIR)/fio.c \
	$(FACIO_DIR)/fiobject.c \
	$(FACIO_DIR)/fiobj_str.c \
	$(FACIO_DIR)/fiobj_hash.c \
	$(FACIO_DIR)/fiobj_ary.c \
	$(FACIO_DIR)/fiobj_numbers.c \
	$(FACIO_DIR)/fiobj_data.c \
	$(FACIO_DIR)/http.c \
	$(FACIO_DIR)/http1.c \
	$(FACIO_DIR)/http_internal.c \
	$(FACIO_DIR)/fio_tls_openssl.c
FACIO_HEADER_FILES = $(wildcard $(FACIO_DIR)/*.h)
FACIO_OBJS = $(FACIO_SRC_FILES:$(FACIO_DIR)/%.c=third_party_obj/facio/%.o)

# llhttp (HTTP/1.1 parser), vendored verbatim (see ../../third_party/llhttp).
LLHTTP_DIR = ../../third_party/llhttp
LLHTTP_SRC_FILES = $(LLHTTP_DIR)/llhttp.c $(LLHTTP_DIR)/api.c $(LLHTTP_DIR)/http.c
LLHTTP_HEADER_FILES = $(wildcard $(LLHTTP_DIR)/*.h)
LLHTTP_OBJS = $(LLHTTP_SRC_FILES:$(LLHTTP_DIR)/%.c=third_party_obj/llhttp/%.o)

CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror

# Third party sources are compiled without -Werror and with extra suppression
# flags because facil.io triggers several warnings with GCC/OpenSSL 3.0.
THIRD_PARTY_CFLAGS = $(INCLUDES) $(DEFINITIONS) -g -O0 \
	-Wno-deprecated-declarations -Wno-cast-function-type \
	-Wno-unused-parameter -Wno-sign-compare -Wno-type-limits

# llhttp's generated state machine only ever triggers -Wunused-parameter
# (verified under -Wall -Wextra); reusing facio's broader suppression list
# would silently mask a real regression on a future llhttp re-vendor.
LLHTTP_CFLAGS = $(INCLUDES) $(DEFINITIONS) -g -O0 -Wno-unused-parameter

LFLAGS = -lpthread -lssl -lcrypto -lz -lm

third_party_obj/facio third_party_obj/llhttp:
	mkdir -p $@

third_party_obj/facio/%.o: $(FACIO_DIR)/%.c $(FACIO_HEADER_FILES) | third_party_obj/facio
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

third_party_obj/llhttp/%.o: $(LLHTTP_DIR)/%.c $(LLHTTP_HEADER_FILES) | third_party_obj/llhttp
	$(CC) -c $(LLHTTP_CFLAGS) $< -o $@

build: $(FACIO_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) $(OUR_SRC_FILES) -o tests $(LFLAGS)

# This suite drives a real TLS handshake against a genuine self-signed cert
# generated at test-startup via the `openssl` CLI. Kept isolated in its own
# binary (mirroring tests/chttpserver_tls) so a missing/broken openssl CLI
# or an invalid cert file cannot take the rest of the chttpclient test suite
# down with it.
test: build
	./tests

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) $(OUR_SRC_FILES) \
		-o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file chttpclient.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient.c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build
