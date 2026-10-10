
INCLUDES = -I.. -I../../include
DEFINITIONS = -DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64

# Our source files. The build compiles them with the strict flags and with
# -Werror. chttpclient.c owns its own static ccol_event_loop reactor, which
# lives in cthreadcomm.c. It uses ctls.c for TLS. clogger.c is there for
# chttpcli_set_engine_logger. cthreadpool.c comes in through the worker pool
# of the async engine for DNS and for connect (g_client_dns_pool).
#
# One binary, `tests`, from tests.c, covers the main suite of this whole
# directory. This includes a real TLS handshake against a real self-signed
# certificate. The suite makes that certificate with the `openssl` command
# line tool when the tests start. That TLS coverage needs no binary of its
# own. ctls.c never aborts the process on bad input, and reports a failure
# with a return value instead. There is therefore nothing to keep apart.
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
# The build compiles the sources of the library at the optimization level of
# the shipped library. These tests therefore exercise the code that ships, and
# not a debug build that nobody installs. The build uses -O0 only for the test
# files of this directory. There the extra help for a debugger is worth the
# cost, and the object code is not part of the product. Do you need to step
# through a failure? Then loosen SRC_OPT for that time.
SRC_OPT = -O3
TEST_OPT = -O0
# -Wl,-z,relro,-z,now is a linker flag, and it stays out of CFLAGS_COMMON on
# purpose. SRC_CFLAGS drives calls that only compile. The driver of clang
# treats an unused linker argument there as an error under -Werror, and gcc
# only drops it. The flag belongs on CFLAGS, which does the real link. It sits
# there instead.
LD_HARDENING = $(CCOL_LD_HARDENING)
CFLAGS_COMMON = $(INCLUDES) $(DEFINITIONS) -std=gnu11 $(CCOL_HARDENING_CFLAGS) \
	-Wstrict-overflow \
	-Wformat=2 -Wformat-security -Wall -Wextra -g3 -Werror
include ../hardening.mk

CFLAGS = $(CFLAGS_COMMON) $(LD_HARDENING) $(TEST_OPT) $(EXTRA_CFLAGS)
SRC_CFLAGS = $(CFLAGS_COMMON) $(SRC_OPT) $(EXTRA_CFLAGS)
# The build makes the library objects once for this directory. It links them
# into every binary of this directory, and does not compile them again for
# each binary. PARSER_OBJS is the part of them that the two parser-only
# binaries need.
SRC_OBJS = $(notdir $(COMMON_SRC_FILES:.c=.o))
PARSER_OBJS = chttp1_parser.o ctls.o chashmap.o common.o
TSAN_CFLAGS = $(INCLUDES) $(DEFINITIONS) -std=gnu11 -fsanitize=thread -Wall -Wextra -Werror -g -O1 $(EXTRA_CFLAGS)

LFLAGS = -lpthread -lssl -lcrypto -lz -lm $(CCOL_PLATFORM_LIBS)

# This is empty for an ordinary build. generate_coverage_report gives
# COVERAGE_ON here, so the same pattern rule instruments the library objects.
COVERAGE_FLAGS =
COVERAGE_ON = -fprofile-arcs -ftest-coverage

# An object file holds no record of the way the build made it. You can change
# the compiler or the flags, for example with a native build and then
# `make CC="gcc -m32" test`, or with a coverage run. Without this stamp, the
# build then links stale objects of the wrong architecture, or objects with no
# instrumentation. This stamp holds the current compiler and the current
# flags. The build rewrites it only when they change. The objects therefore
# build again when they must, and stay in the cache when they need not.
.PHONY: FORCE
FORCE:

.build-flags: FORCE
	@printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' | cmp -s - $@ || \
		printf '%s\n' '$(CC) $(SRC_CFLAGS) $(COVERAGE_FLAGS)' > $@

# The pattern rule for the library objects. $(COVERAGE_FLAGS) is empty for an
# ordinary build, and generate_coverage_report sets it. One rule therefore
# serves both. include/internal/ has a line of its own, because a *.h wildcard
# cannot match a subdirectory. Without the second pattern, no internal header
# is a prerequisite. An edit to such a header then compiles nothing here. A
# later edit to one .c file then gives a binary whose objects disagree about
# that header.
LIB_HEADERS = $(wildcard ../../include/*.h) \
              $(wildcard ../../include/internal/*.h)

%.o: ../../src/%.c $(LIB_HEADERS) .build-flags
	$(CC) $(COVERAGE_FLAGS) $(SRC_CFLAGS) -c $< -o $@

build: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests.c $(SRC_OBJS) -o tests $(LFLAGS)

# A separate ThreadSanitizer build of the main suite alone. It exists to catch
# a race in the async engine, which has many threads. It stays out of the
# default -Werror build. The Makefile of tests/cthreadcomm uses the same
# build_tsan and test_tsan pattern.
build_tsan:
	$(CC) $(TSAN_CFLAGS) tests.c $(COMMON_SRC_FILES) -o tests_tsan $(LFLAGS) -fsanitize=thread $(CCOL_TSAN_EXTRA_SRC)

# die_after_fork=0 is there for one test of this suite,
# concurrent_double_destroy_is_fatal. That test forks. It then makes two NEW
# pthreads in the child to race a double destroy. The async engine of
# chttpclient keeps one ccol_event_loop reactor alive for the whole suite.
# That reactor has many threads and covers the whole process. The parent is
# therefore multi-threaded at the moment of the fork(). The default of TSan,
# die_after_fork=1, treats that state as unsafe for a child that then makes
# more threads. With that default the child exits in the normal way, without
# a word, and never reaches the ccol_fatal_err() and the SIGABRT that this
# test wants. Only a fork() and then new threads under the safety net of TSan
# hits this. The chttpclient_destroy logic of the child is correct, and it has
# no race. That holds under gcc and clang, at -O0 and at -O1, and with TSan
# and without it. The test of tests/cthreadcomm looks the same and needs no
# such option. Its own
# ccol_event_loop instances go away between tests, so ITS OWN parent is
# single-threaded at the moment it forks.
#
# suppressions=tsan_openssl.supp: see the comment in that file. It describes
# the one known race that it suppresses. That race is inside OpenSSL alone:
# client handshakes that run at the same time and verify against one shared
# trust store.
#
# print_suppressions=1: a suppression that matches nothing gives exactly the
# output of one that works. Every run therefore prints how many times each
# anchor fired. Does the count of an anchor stay at zero? Then derive that
# anchor again from a real report, and do not leave it as it is.
test_tsan: build_tsan check_tsan_supp
	TSAN_OPTIONS="die_after_fork=0 suppressions=tsan_openssl.supp print_suppressions=1" ./tests_tsan

# A suppression of ThreadSanitizer matches on any frame of either access in
# the race. An anchor that names a function of this project therefore silences
# a race in the code of this library. It does that as easily as it silences
# the third-party race that somebody wrote the anchor for. It does so without
# a word. This target makes that
# a hard failure. Every race: anchor in tsan_openssl.supp must name a frame
# that the test binary does not define. For a third-party race, that means a
# symbol inside libcrypto or libssl, and not one from here. The target also
# fails just as loudly when the file gives no anchor at all. An empty file, a
# file with a new name and a file that it cannot read therefore cannot pass
# while it examines nothing.
# The symbol table of Mach-O puts one underscore in front of every C name, and
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

# A destroy of the handle from chttp_default_client() happens once, covers the
# whole process, and cannot be undone. The library never builds
# default_client_bundler.client again after it clears that field. See the doc
# comment of chttp_default_client. Such a destroy must therefore not run
# inside tests.c. There it breaks the chttp_do and chttp_get calls of every
# other test for the rest of that process. tests_parser set the precedent for
# one more binary inside this same directory, instead of a new top-level
# directory under tests/.
build_default_client: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_default_client.c $(SRC_OBJS) \
		-o tests_default_client $(LFLAGS)

# tests_sigpipe runs with the default disposition of SIGPIPE, which ends the
# process. tests.c ignores SIGPIPE for its mock server, and an ignored SIGPIPE
# is discarded when it is raised, so only a binary of its own can see a write
# of the client that raises it, or an engine that changes the disposition.
build_sigpipe: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_sigpipe.c $(SRC_OBJS) -o tests_sigpipe $(LFLAGS)

# tests_exit ends each scenario with exit() in a child of fork(), and checks
# what is still running once every exit handler ran. Its parent must fork
# with one thread and with the library untouched, so that each child starts
# the async engine afresh, which only a binary of its own guarantees.
build_exit: $(SRC_OBJS)
	$(CC) $(CFLAGS) tests_exit.c $(SRC_OBJS) -o tests_exit $(LFLAGS)

# tests_parser drives chttp1_parser.c directly, with no chttpclient.c at all.
# It uses byte sequences that somebody wrote by hand. Those sequences include
# malformed and hostile input. The tests binary sits on a real socket, and its
# structure cannot reach such input. Its mock server only ever sends a
# response that is well formed. The "stream" group of tests_parser uses
# local AF_UNIX socketpairs to drive chttp1_stream_t. But it stays fully
# independent of real network traffic. chttp1_parser.c links ctls.c, and with
# it chashmap.c and common.c. The reason is that chttp1_stream_t can drive a
# TLS connection with ctls_conn_read and ctls_conn_write. See the module doc
# comment of chttp1_parser.h. tests_parser is a binary of its own in this same
# directory, and not a new top-level directory under tests/.
build_parser: $(PARSER_OBJS)
	$(CC) $(CFLAGS) tests_parser.c $(PARSER_OBJS) -o tests_parser $(LFLAGS)

# The libFuzzer targets for chttp1_parser.c. There is one target for the
# request mode and one for the response mode. See fuzz/fuzz_common.h for the
# reason. The grammar of a request line and the grammar of a status line
# differ enough that each one wants a corpus of its own. One shared pile does
# not serve both.
# These targets need clang, because libFuzzer is a feature of Clang and LLVM.
# The Makefile of tests/cyaml has the same `fuzz` target, with the same reason
# for .PHONY. A directory fuzz/corpus_request or fuzz/corpus_response is
# already on disk. Without .PHONY, Make takes a bare `fuzz_request` or
# `fuzz_response` target for one that is already done, and skips the whole
# recipe. Run ./fuzz_chttp1_request fuzz/corpus_request [-max_total_time=N]
# and the other binary in the same way. See the documentation of libFuzzer for
# the flags.
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
# Its input is just as attacker-influenced: a Location header comes from
# whatever server the previous hop reached. This target therefore needs the
# whole client, and not only the parser sources.
fuzz_url:
	$(FUZZ_CC) $(FUZZ_CFLAGS) fuzz/fuzz_chttp_url.c $(COMMON_SRC_FILES) \
		-o fuzz_chttp_url $(LFLAGS)

# tests_differential checks chttp1_parser.c against picohttpparser on the same
# raw byte sequences. picohttpparser lives under picohttpparser/. It is an
# independent HTTP/1.x parser, and the h2o web server uses it in production.
# This check catches a bug in the framing or in the extraction that the test
# suite of neither parser alone shows. Each suite agrees only with its own
# parser. tests_differential is a binary of its own in this same
# directory, and not a new top-level directory under tests/. tests_parser and
# tests_default_client above set that precedent. Only this one binary needs
# picohttpparser.c, so COMMON_SRC_FILES does not hold it. picohttpparser is a
# vendored implementation for the comparison, and not a source of the library.
# It therefore keeps the flags of the test side, and not the flags of the
# shipped library.
build_differential: $(PARSER_OBJS)
	$(CC) $(CFLAGS) tests_differential.c $(PARSER_OBJS) \
		picohttpparser/picohttpparser.c -o tests_differential $(LFLAGS)


# These aliases let the name of a binary of this suite work as a target too.
# Each alias builds that binary. `make test` builds and runs every one of
# them, and it stays the normal way in.
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

# Each binary gets only the suppression files whose subject it can reach. The
# comment in each file describes that subject. tests resolves a name that the
# hosts file does not answer. It therefore loads an NSS module and needs
# ld_so_dlopen.supp. It also runs real handshakes against certificates, and
# that is the subject of openssl_internal.supp. tests_parser makes a key
# inside the process with ctls_ctx_cert_add, and that is what fills the caches
# of libcrypto. tests_default_client only addresses 127.0.0.1 by number and
# never reaches a certificate, so it needs neither file. tests_differential
# drives the parser alone. Add -v to any of these lines to make valgrind print
# how many times each rule fired. Does a rule fire zero times everywhere? Then
# derive it again, and do not keep it.
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

# The flag "--ignore-errors negative" is there for one reason. The coverage
# counters of gcov are plain increments and are not atomic. The async engine
# of this suite uses many threads. Two of them can increment one counter at
# the same time. gcov then sometimes reports a delta below zero when it
# reconciles the counts. The failure is not deterministic. It lands on a
# different file, a different line and a different value from run to run. This
# is a limit of the instrumentation of gcov for concurrent code, and not a
# defect in the logic of this library. The flags for mismatch and for
# inconsistent are here for the same reason: a known quirk of a tool.
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

# The steps of `all` rebuild and run the same object files and binaries: the
# coverage step rebuilds them with instrumentation, and the test and memtest
# steps run the binaries that the build step links. Under make -j those steps
# would overlap and link or run a half-rebuilt mix. .NOTPARALLEL runs the
# prerequisites of `all` one after another, in the order listed, while the
# work inside each step still runs in parallel. A make older than 4.4 ignores
# the prerequisite list and runs this whole Makefile serially, which is also
# correct.
.NOTPARALLEL: all

.PHONY: clean_objs
clean_objs:
	rm -f *.o *.gcno *.gcda .build-flags

clean:
	rm -rf .build-flags *.o tests tests_tls tests_parser tests_default_client tests_differential tests_sigpipe tests_exit tests_tsan third_party_obj coverage *.gcno *.gcda *.gcov *.c.info \
		fuzz_chttp1_request fuzz_chttp1_response fuzz_chttp_url

default: build build_parser build_default_client build_differential
