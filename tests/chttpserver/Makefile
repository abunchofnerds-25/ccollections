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

third_party_obj:
	mkdir -p third_party_obj

third_party_obj/%.o: $(THIRD_PARTY_DIR)/%.c | third_party_obj
	$(CC) -c $(THIRD_PARTY_CFLAGS) $< -o $@

build: $(THIRD_PARTY_OBJS)
	$(CC) $(CFLAGS) $(THIRD_PARTY_OBJS) $(OUR_SRC_FILES) -o tests $(LFLAGS)

test: build
	./tests

memtest: build
	valgrind --leak-check=full -s --show-leak-kinds=all --error-exitcode=1 \
		./tests

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) $(OUR_SRC_FILES) \
		$(THIRD_PARTY_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttpserver && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver.c.info \
		--output-directory coverage && \
	rm -rf *.gcno *.gcda *.gcov *.c.info

all: build test memtest generate_coverage_report

clean:
	rm -rf tests third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build
