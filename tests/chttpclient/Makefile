CC = gcc

INCLUDES = -I.. -I../../include -I../../third_party/facio -I../../third_party/llhttp
DEFINITIONS = -DRUNNING_UNIT_TESTS -DHAVE_OPENSSL=1

# Our source files (compiled with strict flags + -Werror). clogger.c is a
# transitive dependency: facio's fio.c resolves its FIO_LOG_* macros there.
SRC_FILE_PREFIX = chttpclient
OUR_SRC_FILES = tests.c \
	../../src/$(SRC_FILE_PREFIX).c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cvector.c \
	../../src/common.c

# Minimal facio subset needed for client-mode TLS: fio.c (core types/uuid
# table symbols referenced by fio_tls_openssl.c) + fio_tls_openssl.c itself.
# The fiobj_*/http*/http_internal.c files are exclusively for chttpserver's
# own HTTP/1.1 server engine and are NOT needed here.
FACIO_DIR = ../../third_party/facio
FACIO_SRC_FILES = $(FACIO_DIR)/fio.c $(FACIO_DIR)/fio_tls_openssl.c
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
# flags because facil.io triggers several warnings with GCC/OpenSSL 3.0, and
# llhttp's generated state machine triggers -Wunused-parameter pervasively.
THIRD_PARTY_CFLAGS = $(INCLUDES) $(DEFINITIONS) -g -O0 \
	-Wno-deprecated-declarations -Wno-cast-function-type \
	-Wno-unused-parameter -Wno-sign-compare -Wno-type-limits

COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread -lssl -lcrypto -lz -lm

third_party_obj/facio third_party_obj/llhttp:
	mkdir -p $@

third_party_obj/facio/%.o: $(FACIO_DIR)/%.c $(FACIO_HEADER_FILES) | third_party_obj/facio
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

third_party_obj/llhttp/%.o: $(LLHTTP_DIR)/%.c $(LLHTTP_HEADER_FILES) | third_party_obj/llhttp
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

build: $(FACIO_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) $(OUR_SRC_FILES) -o tests $(LFLAGS)

test: build
	./tests

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=ld_so_dlopen.supp ./tests

generate_coverage_report:
	$(CC) $(COVERAGE_FLAGS) $(CFLAGS) $(FACIO_OBJS) $(LLHTTP_OBJS) $(OUR_SRC_FILES) \
		-o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build
