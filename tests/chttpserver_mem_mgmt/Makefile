CC = gcc

INCLUDES = -I.. -I../../include -I../../third_party/facio -I../../third_party/llhttp
DEFINITIONS = -DRUNNING_UNIT_TESTS -DHAVE_OPENSSL=1

# Our source files (compiled with strict flags + -Werror).
OUR_SRC_FILES = tests.c \
	../../src/chttpserver.c \
	../../src/chttpclient.c \
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

LFLAGS = -lpthread -lssl -lcrypto -lm -lz

THIRD_PARTY_OBJS = $(THIRD_PARTY_SRC_FILES:$(THIRD_PARTY_DIR)/%.c=third_party_obj/facio/%.o)
THIRD_PARTY_HEADER_FILES = $(wildcard $(THIRD_PARTY_DIR)/*.h)

third_party_obj/facio third_party_obj/llhttp:
	mkdir -p $@

third_party_obj/facio/%.o: $(THIRD_PARTY_DIR)/%.c $(THIRD_PARTY_HEADER_FILES) | third_party_obj/facio
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

third_party_obj/llhttp/%.o: $(LLHTTP_DIR)/%.c $(LLHTTP_HEADER_FILES) | third_party_obj/llhttp
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

build: $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) $(OUR_SRC_FILES) -o tests $(LFLAGS)

# This suite calls chttpsvr_set_engine_mem_mgmt_procs() before the first
# chttpsvr_start() in the process, which is a process-wide, set-once-ever
# requirement -- it is kept in its own binary (separate from tests/chttpserver)
# so it cannot conflict with the many chttpsvr_start() calls already made by
# the rest of the chttpserver test suite.
test: build
	./tests

memtest: build
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) $(OUR_SRC_FILES) \
		$(THIRD_PARTY_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpserver_mem_mgmt && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_mem_mgmt.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_mem_mgmt.c.info \
		--output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build
