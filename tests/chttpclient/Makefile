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
SRC_FILE_PREFIX = chttpclient
OUR_SRC_FILES = tests.c \
	../../src/$(SRC_FILE_PREFIX).c \
	../../src/cfio_engine.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cthreadpool.c \
	../../src/cvector.c \
	../../src/common.c

# cfio_engine.c's one-time global init unconditionally calls
# http_lib_constructor() (see cfio_engine.h for why), so this suite now
# transitively needs the same fiobj_*/http*/http_internal.c vendor files
# chttpserver's own suites need, not just fio.c + fio_tls_openssl.c.
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

COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lpthread -lssl -lcrypto -lz -lm

third_party_obj/facio third_party_obj/llhttp:
	mkdir -p $@

third_party_obj/facio/%.o: $(FACIO_DIR)/%.c $(FACIO_HEADER_FILES) | third_party_obj/facio
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

third_party_obj/llhttp/%.o: $(LLHTTP_DIR)/%.c $(LLHTTP_HEADER_FILES) | third_party_obj/llhttp
	$(CC) -c $(LLHTTP_CFLAGS) $< -o $@

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
