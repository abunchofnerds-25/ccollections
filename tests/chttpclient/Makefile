CC = gcc

INCLUDES = -I.. -I../../include -I../../third_party/facio -I../../third_party/llhttp
DEFINITIONS = -DRUNNING_UNIT_TESTS -DHAVE_OPENSSL=1

# Our source files (compiled with strict flags + -Werror). clogger.c is a
# transitive dependency: facio's fio.c resolves its FIO_LOG_* macros there.
# cthreadpool.c is a transitive dependency of the async engine's DNS/connect
# worker pool (g_client_dns_pool). cfio_engine.c is the shared facio reactor
# lifecycle module chttpclient's async engine (Tier 2/3) now acquires/
# releases references to, instead of owning its own private reactor --
# see cfio_engine.h.
#
# Two binaries are built from this shared source set: `tests` (the main
# suite, tests.c) and `tests_tls` (tests_tls.c), a dedicated suite that
# drives a real TLS handshake against a genuine self-signed cert generated
# at test-startup via the `openssl` CLI. tests_tls is kept as a SEPARATE
# BINARY (not folded into tests.c) because a missing/broken openssl CLI or
# an invalid cert file makes the vendored facio TLS layer call
# FIO_LOG_FATAL, which aborts the whole process -- isolating it into its own
# binary means that failure mode can only take down the TLS suite, not the
# rest of tests/chttpclient.
COMMON_SRC_FILES = ../../src/chttpclient.c \
	../../src/chttp.c \
	../../src/cfio_engine.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cthreadpool.c \
	../../src/cvector.c \
	../../src/common.c

# cfio_engine.c's one-time global init unconditionally calls
# http_lib_constructor() (see cfio_engine.h), so this suite now transitively
# needs the same fiobj_*/http*/http_internal.c vendor files chttpserver's own
# suites need, not just fio.c + fio_tls_openssl.c. tests_tls.c's own mock TLS
# server remains a hand-rolled raw-OpenSSL server rather than reusing
# chttpserver.c -- simpler to keep this TLS-handshake-focused suite free of
# chttpserver's routing/dispatch machinery, not because of any per-process
# engine restriction (chttpserver and chttpclient's async engine safely
# share one process-wide reactor via cfio_engine.c).
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
	$(CC) $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) tests.c $(COMMON_SRC_FILES) -o tests $(LFLAGS)

build_tls: $(FACIO_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) tests_tls.c $(COMMON_SRC_FILES) -o tests_tls $(LFLAGS)

test: build build_tls
	./tests
	./tests_tls

memtest: build build_tls
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=ld_so_dlopen.supp ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_tls

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) tests.c \
		$(COMMON_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file chttpclient.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient.c.info --output-directory coverage/tests && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) tests_tls.c \
		$(COMMON_SRC_FILES) -o tests_tls $(LFLAGS) && \
	./tests_tls && \
	gcov -d -w tests_tls-chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file chttpclient_tls.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient_tls.c.info --output-directory coverage/tests_tls && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_tls

all: build build_tls test memtest generate_coverage_report

clean:
	rm -rf tests tests_tls third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_tls
