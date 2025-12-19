CC = gcc

INCLUDES = -I.. -I../../include -I../../third_party/facio -I../../third_party/llhttp
DEFINITIONS = -DRUNNING_UNIT_TESTS -DHAVE_OPENSSL=1

# Our source files (compiled with strict flags + -Werror). This is the first
# binary in the codebase to link chttpserver.c and chttpclient.c together
# specifically to exercise both of their engines running simultaneously in
# the same process -- see src/cfio_engine.c/.h, the shared facio reactor
# lifecycle module both now acquire/release references to instead of each
# owning an independent (mutually exclusive) reactor.
#
# Two binaries are built from this shared source set:
#   tests                        (tests.c) - chttpclient's async engine calls
#                                 _cfio_engine_acquire() first, chttpserver
#                                 second, plus the rest of this suite's
#                                 engine-unification coverage.
#   tests_starts_engine_first    (tests_starts_engine_first.c) - the other
#                                 half of the "either order" claim tests.c
#                                 cannot cover by itself in one process:
#                                 cfio_engine.c's one-time global init is
#                                 pthread_once-guarded for the whole process
#                                 lifetime, so only ONE ordering can ever be
#                                 genuinely "first" per binary. Kept as a
#                                 SEPARATE BINARY so chttpserver gets its own
#                                 genuinely fresh process to be the first-ever
#                                 caller into the shared engine.
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

build_starts_engine_first: $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS)
	$(CC) $(CFLAGS) $(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) tests_starts_engine_first.c \
		$(COMMON_SRC_FILES) -o tests_starts_engine_first $(LFLAGS)

test: build build_starts_engine_first
	./tests
	./tests_starts_engine_first

memtest: build build_starts_engine_first
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests
	valgrind --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_starts_engine_first

generate_coverage_report:
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests.c $(COMMON_SRC_FILES) \
		$(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-chttp_server_and_client && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttp_server_and_client.c.info && \
	genhtml --ignore-errors inconsistent -s chttp_server_and_client.c.info \
		--output-directory coverage/tests && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests
	$(CC) -fprofile-arcs -ftest-coverage $(CFLAGS) tests_starts_engine_first.c $(COMMON_SRC_FILES) \
		$(THIRD_PARTY_OBJS) $(LLHTTP_OBJS) -o tests_starts_engine_first $(LFLAGS) && \
	./tests_starts_engine_first && \
	gcov -d -w tests_starts_engine_first-chttp_server_and_client && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent \
		--capture --directory . --output-file chttpserver_starts_engine_first.c.info && \
	genhtml --ignore-errors inconsistent -s chttpserver_starts_engine_first.c.info \
		--output-directory coverage/tests_starts_engine_first && \
	rm -rf *.gcno *.gcda *.gcov *.c.info tests_starts_engine_first

all: build build_starts_engine_first test memtest generate_coverage_report

clean:
	rm -rf tests tests_starts_engine_first third_party_obj coverage *.gcno *.gcda *.gcov *.c.info

default: build build_starts_engine_first
