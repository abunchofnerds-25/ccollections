
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64

# Our sources, built with the strict flags and -Werror. chttpclient.c owns its
# own static ccol_event_loop reactor, which lives in cthreadcomm.c, and it
# uses ctls.c for TLS. clogger.c is here for chttpcli_set_engine_logger, and
# cthreadpool.c comes in through the async engine's worker pool for DNS and
# connect (g_client_dns_pool).
#
# One binary, `tests`, built from tests.c, holds the main suite of this
# directory, including a real TLS handshake against a real self-signed
# certificate that the suite makes with the `openssl` command line tool when
# it starts. That TLS coverage doesn't need a binary of its own, because
# ctls.c never aborts the process on bad input (it reports a failure through
# its return value), so there is nothing to keep apart.
COMMON_SRC_FILES = ../../src/chttpclient.c \
	../../src/chttp1_parser.c \
	../../src/chttp.c \
	../../src/cthreadcomm.c \
	../../src/clogger.c \
	../../src/chashmap.c \
	../../src/cthreadpool.c \
	../../src/cvector.c \
	../../src/csort.c \
	../../src/ctls.c \
	../../src/common.c ../../src/cpintable.c

EXTRA_CFLAGS ?=
# The library sources are built at the optimization level of the shipped
# library, so these tests exercise the code that ships and not a debug build
# that nobody installs. Only the test files of this directory get -O0, where
# the extra help for a debugger is worth it and the object code isn't part of
# the product. If you need to step through a failure, loosen SRC_OPT for a
# while.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag and is kept out of CFLAGS_COMMON on
# purpose. SRC_CFLAGS drives compile-only calls, and clang's driver treats an
# unused linker argument there as an error under -Werror (gcc just drops it),
# so the flag goes on CFLAGS, which does the real link.
LD_HARDENING = $(CCOL_LD_HARDENING)
CFLAGS_COMMON = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) \
	-Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror
include ../hardening.mk

CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The library objects are built once for this directory and linked into every
# binary here, instead of being compiled again for each one. PARSER_OBJS is
# the subset that the two parser-only binaries need.
SRC_OBJS = $(notdir $(COMMON_SRC_FILES:.c=.o))
PARSER_OBJS = chttp1_parser.o ctls.o chashmap.o common.o
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)

# The tests call dlsym(3), which glibc before 2.34 keeps in libdl. Every Linux
# C library accepts -ldl (glibc 2.34 and later and musl ship it as an empty
# library), while macOS, FreeBSD and OpenBSD have dlsym in libc itself.
LFLAGS = -lpthread -lssl -lcrypto -lz -lm $(CCOL_PLATFORM_LIBS) $(CCOL_DL_LIBS)

# Empty for an ordinary build. generate_coverage_report passes COVERAGE_ON
# here, so the same pattern rule instruments the library objects.
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage

# An object file keeps no record of how it was built. Without this stamp a
# change of compiler or flags (a native build followed by
# `make CC="gcc -m32" test`, or a coverage run) links stale objects of the
# wrong architecture, or objects with no instrumentation. The stamp holds the
# current compiler and flags and is rewritten only when they change, so the
# objects get rebuilt when they have to be and stay cached otherwise.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# Pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build and generate_coverage_report sets it, so one rule serves
# both. include/internal/ gets a line of its own because a *.h wildcard can't
# match a subdirectory. Without that second pattern no internal header is a
# prerequisite, so editing one compiles nothing here, and a later edit to a
# single .c file gives a binary whose objects disagree about that header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h) \
              $(wildcard ../../include/internal/test_helpers/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

build: $(SRC_OBJS) tests_locale
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)

# The locale_folding tests run the client under a Turkish LC_CTYPE, where
# strcasecmp() and tolower() fold 'I' to a letter other than 'i'. They use
# this private copy of tr_TR.UTF-8, found through LOCPATH. The step is best
# effort: without localedef (macOS has none) or the tr_TR locale source, the
# directory stays empty and those tests print that they skipped.
tests_locale:
	mkdir -p tests_locale
	@if command -v localedef >/dev/null 2>&1; then \
	  localedef -c -i tr_TR -f UTF-8 tests_locale/tr_TR.UTF-8 >/dev/null 2>&1 \
	    || true; \
	fi

# A separate ThreadSanitizer build of the main suite only, there to catch
# races in the async engine and its many threads. It is kept out of the
# default -Werror build; tests/cthreadcomm/Makefile uses the same build_tsan
# and test_tsan pattern.
build_tsan:
	$(CC) $(TSAN_CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# die_after_fork=0 is for one test of this suite,
# concurrent_double_destroy_is_fatal, which forks and then creates two NEW
# pthreads in the child to race a double destroy. chttpclient's async engine
# keeps one ccol_event_loop reactor alive for the whole suite, and that
# reactor has many threads and covers the whole process, so the parent is
# multi-threaded when it calls fork(). TSan's default, die_after_fork=1,
# treats that as unsafe for a child that goes on to create threads, and with
# it the child quietly exits the normal way and never reaches the
# _ccol_fatal_err() and the SIGABRT the test is waiting for. Only a fork()
# followed by new threads under TSan's safety net hits this. The child's
# chttpclient_destroy logic is correct and has no race, under gcc and clang,
# at -O0 and -O1, with TSan and without it. The test in tests/cthreadcomm
# looks the same and needs no such option, because its own ccol_event_loop
# instances go away between tests, so ITS OWN parent is single-threaded when
# it forks.
#
# suppressions=tsan_openssl.supp: the comment in that file describes the one
# known race it suppresses, which is entirely inside OpenSSL (client
# handshakes running at the same time and verifying against one shared trust
# store).
#
# print_suppressions=1: a suppression that matches nothing prints exactly
# what a working one prints, so every run shows how many times each anchor
# fired. If an anchor's count stays at zero, derive it again from a real
# report instead of leaving it there.
test_tsan: build_tsan check_tsan_supp
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_openssl.supp print_suppressions=1" ./tests_tsan

# A ThreadSanitizer suppression matches any frame of either access in the
# race, so an anchor that names one of our own functions silently hides a
# race in this library just as easily as the third-party race it was written
# for. This target turns that into a hard failure: every race: anchor in
# tsan_openssl.supp must name a frame that the test binary doesn't define,
# which for a third-party race means a symbol inside libcrypto or libssl and
# not one of ours. It fails just as loudly when the file gives no anchor at
# all, so an empty file, a renamed file or a file it can't read can't pass
# while checking nothing.
# The Mach-O symbol table puts an underscore in front of every C name, and
# the anchors are C names.
CCOL_NM_C_NAMES = $(if $(filter Darwin,$(CCOL_UNAME_S)),| sed 's/^_//')
.PHONY: check_tsan_supp
check_tsan_supp: build_tsan
	@anchors=`sed -n 's/^race:\(.*\)$$/\1/p' tsan_openssl.supp | tr -d '*' | sort -u`; \
	if [ -z "$$anchors" ]; then \
		echo "check_tsan_supp: tsan_openssl.supp yields no race: anchor" >&2; \
		exit 1; \
	fi; \
	ownsyms=`nm --defined-only tests_tsan | awk 'NF >= 3 { print $$3 }' $(CCOL_NM_C_NAMES) | sort -u`; \
	if [ -z "$$ownsyms" ]; then \
		echo "check_tsan_supp: no symbols read from tests_tsan" >&2; \
		exit 1; \
	fi; \
	rc=0; \
	for a in $$anchors; do \
		if printf '%s\n' "$$ownsyms" | grep -qx -- "$$a"; then \
			echo "check_tsan_supp: anchor '$$a' names a symbol tests_tsan defines;" >&2; \
			echo "  it would suppress races in this project's own code" >&2; \
			rc=1; \
		fi; \
	done; \
	if [ $$rc -eq 0 ]; then \
		echo "check_tsan_supp: OK ($$(printf '%s\n' "$$anchors" | wc -l) anchor(s), none owned by tests_tsan)"; \
	fi; \
	exit $$rc

# Destroying the handle from chttp_default_client() happens once for the
# whole process and can't be undone, because the library never builds
# default_client_bundler.client again after it clears that field (see the doc
# comment of chttp_default_client). Such a destroy must not run inside
# tests.c, where it would break the chttp_do and chttp_get calls of every
# other test for the rest of that process. So, like tests_parser, it gets one
# more binary in this directory instead of a new top-level directory under
# tests/.
build_default_client: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_default_client.c $(SRC_OBJS) \
		-o tests_default_client $(LFLAGS)

# tests_sigpipe runs with SIGPIPE at its default disposition, which ends the
# process. tests.c ignores SIGPIPE for its mock server, and an ignored SIGPIPE
# is discarded when it's raised, so only a binary of its own can see a client
# write that raises it, or an engine that changes the disposition.
build_sigpipe: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) -o tests_sigpipe $(LFLAGS)

# tests_exit ends each scenario with exit() in a child of fork() and checks
# what is left running after every exit handler has run. Its parent has to
# fork with one thread and with the library untouched, so that each child
# starts the async engine from scratch, and only a binary of its own gives
# you that.
build_exit: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_exit.c $(SRC_OBJS) -o tests_exit $(LFLAGS)

# tests_parser drives chttp1_parser.c directly, without chttpclient.c at all,
# through hand-written byte sequences that include malformed and hostile
# input. The tests binary works over a real socket and can't reach such
# input, because its mock server only ever sends well-formed responses. The
# "stream" group of tests_parser drives chttp1_stream_t over local AF_UNIX
# socketpairs but stays fully independent of real network traffic.
# chttp1_parser.c links ctls.c, and with it chashmap.c and common.c, because
# chttp1_stream_t can drive a TLS connection through ctls_conn_read and
# ctls_conn_write (see the module doc comment of chttp1_parser.h). It's a
# binary of its own in this directory, not a new top-level directory under
# tests/.
build_parser: $(PARSER_OBJS)
	$(CC) $(CFLAGS) tests_parser.c $(PARSER_OBJS) -o tests_parser $(LFLAGS)

# libFuzzer targets for chttp1_parser.c, one for request mode and one for
# response mode (fuzz/fuzz_common.h explains why). A request line and a
# status line differ enough in grammar that each wants a corpus of its own,
# and one shared pile doesn't serve both. They need clang, since libFuzzer is
# a feature of Clang and LLVM. tests/cyaml/Makefile has the same `fuzz`
# target, with the same reason for .PHONY: a directory fuzz/corpus_request or
# fuzz/corpus_response is already on disk, and without .PHONY make takes a
# bare `fuzz_request` or `fuzz_response` target as already done and skips the
# whole recipe. Run ./fuzz_chttp1_request fuzz/corpus_request
# [-max_total_time=N], and the other binary the same way; the libFuzzer
# documentation lists the flags.
FUZZ_CC = clang
FUZZ_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=fuzzer,address,undefined -g3 -O1
FUZZ_PARSER_SRC_FILES = ../../src/chttp1_parser.c ../../src/ctls.c \
	../../src/chashmap.c ../../src/common.c

.PHONY: fuzz_request fuzz_response fuzz_url
fuzz_request:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_chttp1_request.c $(FUZZ_PARSER_SRC_FILES) \
		-o fuzz_chttp1_request $(LFLAGS)

fuzz_response:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_chttp1_response.c $(FUZZ_PARSER_SRC_FILES) \
		-o fuzz_chttp1_response $(LFLAGS)

# The URL and redirect layer of chttpclient.c, which sits ABOVE the parser.
# Its input is just as attacker-influenced, since a Location header comes
# from whatever server the previous hop reached, so this target needs the
# whole client and not just the parser sources.
fuzz_url:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_chttp_url.c $(COMMON_SRC_FILES) \
		-o fuzz_chttp_url $(LFLAGS)

# tests_differential checks chttp1_parser.c against picohttpparser on the
# same raw byte sequences. picohttpparser is an independent HTTP/1.x parser
# that the h2o web server uses in production, and it lives under
# picohttpparser/. The comparison catches framing and extraction bugs that
# neither parser's own suite shows, because each suite only agrees with its
# own parser. Like tests_parser and tests_default_client above, it's a binary
# of its own in this directory, not a new top-level directory under tests/.
# Only this binary needs picohttpparser.c, so it isn't in COMMON_SRC_FILES,
# and since it's a vendored implementation for the comparison and not a
# library source, it gets the test-side flags instead of those of the
# shipped library.
build_differential: $(PARSER_OBJS)
	$(CC) $(CFLAGS) tests_differential.c $(PARSER_OBJS) \
		picohttpparser/picohttpparser.c -o tests_differential $(LFLAGS)


# Aliases, so that the name of each binary of this suite works as a target
# too. Each one builds its binary, while `make test` builds and runs all of
# them and stays the normal way in.
.PHONY: tests_parser tests_default_client tests_differential tests_sigpipe tests_exit
tests_parser: build_parser
tests_default_client: build_default_client
tests_sigpipe: build_sigpipe
tests_exit: build_exit
tests_differential: build_differential

test: build build_parser build_default_client build_differential build_sigpipe build_exit
	./tests
	./tests_parser
	./tests_default_client
	./tests_differential
	./tests_sigpipe
	./tests_exit

# Each binary loads only the suppression files whose subject it can reach,
# and the comment in each file describes that subject. tests resolves a name
# that the hosts file doesn't answer, so it loads an NSS module and needs
# ld_so_dlopen.supp; it also runs real handshakes against certificates, which
# is what openssl_internal.supp is about. tests_parser makes a key inside the
# process with ctls_ctx_cert_add, and that is what fills libcrypto's caches.
# tests_default_client only talks to 127.0.0.1 by number and never touches a
# certificate, so it needs neither file, and tests_differential drives the
# parser alone. Add -v to any of these lines and valgrind prints how many
# times each rule fired; a rule that fires zero times everywhere should be
# derived again instead of kept.
memtest: build build_parser build_default_client build_differential build_sigpipe build_exit
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes --suppressions=ld_so_dlopen.supp \
		--suppressions=openssl_internal.supp ./tests
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --suppressions=openssl_internal.supp ./tests_parser
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_default_client
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_differential
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 ./tests_sigpipe
	valgrind $(CCOL_VALGRIND_SUPP) --leak-check=full -s -q --show-leak-kinds=all --errors-for-leak-kinds=all \
		--error-exitcode=1 --child-silent-after-fork=yes \
		--suppressions=openssl_internal.supp ./tests_exit

# "--ignore-errors negative" is there because gcov's coverage counters are
# plain, non-atomic increments, and the async engine of this suite runs many
# threads, two of which can increment the same counter at once. gcov then
# sometimes reports a negative delta when it reconciles the counts, and it
# isn't deterministic: a different file, line and value from one run to the
# next. It's a limit of the gcov instrumentation for concurrent code and has
# nothing to do with the library's logic. The mismatch and inconsistent flags
# are there for the same kind of reason, a known quirk of the tool.
generate_coverage_report:
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(SRC_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS) && \
	./tests && \
	gcov -d -w chttpclient && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttpclient.c.info && \
	genhtml --ignore-errors inconsistent -s chttpclient.c.info --output-directory coverage/tests && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(PARSER_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_parser.c $(PARSER_OBJS) \
		-o tests_parser $(LFLAGS) && \
	./tests_parser && \
	gcov -d -w chttp1_parser && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttp1_parser.c.info && \
	genhtml --ignore-errors inconsistent -s chttp1_parser.c.info --output-directory coverage/tests_parser && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_parser
	$(MAKE) clean_objs
	$(MAKE) COVERAGE_FLAGS="$(COVERAGE_ON)" $(PARSER_OBJS)
	$(CC) $(COVERAGE_ON) $(CFLAGS) tests_differential.c $(PARSER_OBJS) \
		picohttpparser/picohttpparser.c -o tests_differential $(LFLAGS) && \
	./tests_differential && \
	gcov -d -w chttp1_parser && \
	lcov --ignore-errors mismatch --ignore-errors inconsistent --ignore-errors negative --capture --directory . \
	--output-file chttp1_parser_differential.c.info && \
	genhtml --ignore-errors inconsistent -s chttp1_parser_differential.c.info --output-directory coverage/tests_differential && \
	cp -f *.c.info coverage/ && rm -rf *.gcno *.gcda *.gcov *.c.info *.o tests_differential

all: build build_parser build_default_client build_differential build_sigpipe build_exit build_tsan test test_tsan memtest generate_coverage_report

# The steps of `all` rebuild and run the same object files and binaries. The
# coverage step rebuilds them with instrumentation, and test and memtest run
# the binaries that the build step links, so under make -j the steps would
# overlap and link or run a half-rebuilt mix. .NOTPARALLEL runs the
# prerequisites of `all` one after another in the order listed, while the work
# inside each step still runs in parallel. A make older than 4.4 ignores the
# prerequisite list and runs the whole Makefile serially, which is correct
# too.
.NOTPARALLEL: all

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags *.o tests tests_locale tests_tls tests_parser tests_default_client tests_differential tests_sigpipe tests_exit tests_tsan third_party_obj coverage *.gcno *.gcda *.gcov *.c.info \
		fuzz_chttp1_request fuzz_chttp1_response fuzz_chttp_url

default: build build_parser build_default_client build_differential
