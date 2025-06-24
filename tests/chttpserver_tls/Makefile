CC = gcc

INCLUDES = -I.. -I../../include -I../../third_party/facio
DEFINITIONS = -DRUNNING_UNIT_TESTS -DHAVE_OPENSSL=1

# Our source files (compiled with strict flags + -Werror).
OUR_SRC_FILES = tests.c \
	../../src/chttpserver.c \
	../../src/chttpclient.c \
	../../src/cthreadpool.c \
	../../src/clogger.c \
	../../src/chashmap.c \
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

CFLAGS = $(INCLUDES) $(DEFINITIONS) -fstack-protector-all -Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -O0 -Werror -fPIC

THIRD_PARTY_CFLAGS = $(INCLUDES) $(DEFINITIONS) -fPIC -g -O0 \
	-Wno-deprecated-declarations -Wno-cast-function-type \
	-Wno-unused-parameter -Wno-sign-compare -Wno-type-limits

LFLAGS = -lpthread -lcurl -lssl -lcrypto -lm -lz

THIRD_PARTY_OBJS = $(THIRD_PARTY_SRC_FILES:$(THIRD_PARTY_DIR)/%.c=third_party_obj/%.o)
THIRD_PARTY_HEADER_FILES = $(wildcard $(THIRD_PARTY_DIR)/*.h)

third_party_obj:
	mkdir -p third_party_obj

third_party_obj/%.o: $(THIRD_PARTY_DIR)/%.c $(THIRD_PARTY_HEADER_FILES) | third_party_obj
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

build: $(THIRD_PARTY_OBJS)
	$(CC) $(CFLAGS) $(THIRD_PARTY_OBJS) $(OUR_SRC_FILES) -o tests $(LFLAGS)

# This suite drives a real TLS handshake and therefore shells out to the
# `openssl` CLI at startup to generate a throwaway self-signed cert/key pair.
# It is kept in its own binary (separate from tests/chttpserver) specifically
# so that a missing/broken openssl CLI or an invalid certificate -- which
# makes the vendored facio TLS layer call FIO_LOG_FATAL and abort the whole
# process -- cannot take the entire chttpserver test suite down with it.
test: build
	./tests

memtest: build
	valgrind --leak-check=full -s --show-leak-kinds=all --error-exitcode=1 \
		./tests

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) $(OUR_SRC_FILES) \
		$(THIRD_PARTY_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpserver_tls && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_tls.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_tls.c.info \
		--output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build
