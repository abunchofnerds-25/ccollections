INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64
SRC_FILE_PREFIX = ctls
SRC_FILES = ../../src/$(SRC_FILE_PREFIX).c ../../src/chashmap.c ../../src/common.c
ALL_SRC_FILES = tests.c $(SRC_FILES)
EXTRA_CFLAGS ?=
include ../hardening.mk

CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) -Wstrict-overflow \
	$(CCOL_LD_HARDENING) \
	-Wformat=2 -Wformat-security -Wall -Wextra -gdwarf-4 -g3 -O3 -Werror $(EXTRA_CFLAGS)
COVERAGE_FLAGS = -fprofile-arcs -ftest-coverage
LFLAGS = -lssl -lcrypto -lpthread $(CCOL_PLATFORM_LIBS)

# A separate ThreadSanitizer build that stays out of the default -Werror
# build; tests/cthreadcomm/Makefile has the same build_tsan and test_tsan
# pattern. This suite drives one shared ctls_ctx_t from several handshake
# threads while another thread keeps changing the configuration of that
# context, and each change rebuilds and frees every SSL_CTX that the context
# owns. This build checks the order between the last read of a connection
# through a shared context and the destruction of that same object.
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread \
	-Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)

# tests_sigpipe is a binary of its own because it needs the default
# disposition of SIGPIPE, and tests ignores SIGPIPE for the whole process.
build:
	$(CC) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS)
	$(CC) $(CFLAGS) tests_sigpipe.c $(SRC_FILES) -o tests_sigpipe $(LFLAGS)

test: build
	./tests
	./tests_sigpipe

build_tsan:
	$(CC) $(TSAN_CFLAGS) $(ALL_SRC_FILES) -o tests_tsan $(LFLAGS) \
		-fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# There is no suppression file: every ThreadSanitizer report from this suite
# names a frame in the code of this module, and this project does not
# suppress a finding over its own code.
test_tsan: build_tsan
	./tests_tsan

memtest: build
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=openssl_internal.supp ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=openssl_internal.supp ./tests_sigpipe

# --ignore-errors negative: the coverage counters of gcov are plain, non-atomic
# increments, so instrumented code that runs on more than one thread at a time
# can race on them and produce a false negative hit count. This affects more
# than one module and also happens inside a glibc header; it is a limit of the
# gcov instrumentation, not a defect in the logic of this library. The same
# flag in tests/chttpclient/Makefile gives the full reason.
generate_coverage_report:
	$(CC)  $(COVERAGE_FLAGS) $(CFLAGS) $(ALL_SRC_FILES) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w tests-$(SRC_FILE_PREFIX) && echo 'GCOV SUCCEEDED, CALLING LCOV' && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file $(SRC_FILE_PREFIX).c.info && \
	genhtml --ignore-errors inconsistent -s $(SRC_FILE_PREFIX).c.info --output-directory coverage && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info

all: build build_tsan test test_tsan memtest generate_coverage_report

# The steps of `all` rebuild and run the same object files and binaries: the
# coverage step rebuilds them with instrumentation, and the test and memtest
# steps run the binaries that the build step links. Under make -j those steps
# would overlap and link or run a half-rebuilt mix. .NOTPARALLEL runs the
# prerequisites of `all` one after another, in the order listed, while the
# work inside each step still runs in parallel. A make older than 4.4 ignores
# the prerequisite list and runs this whole Makefile serially, which is also
# correct.
.NOTPARALLEL: all

clean:
	rm -rf tests tests_sigpipe tests_tsan coverage *.gcno *.gcda *.gcov *.c.info

default: build
