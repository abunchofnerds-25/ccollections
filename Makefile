include platform.mk
AR = ar
RANLIB = ranlib

SHORT_LIBRARY_NAME = ccollections

# The release version, and the Application Binary Interface (ABI) version that
# the SONAME of the shared library holds.
#
# VERSION names the release and ABI_VERSION names the binary interface. An
# application records a dependency on the ABI version, not on the release
# version: an application that you build against this library gets DT_NEEDED
# libccollections.so.$(ABI_VERSION), and the dynamic loader then accepts only a
# library that promises that same interface. The two numbers are separate on
# purpose, so a release that only adds symbols, or only corrects behavior,
# keeps the same ABI while VERSION goes up.
#
# ABI_VERSION is the major version, and you must increase it when a change
# breaks the interface. The VERSION_MAJOR = 0 branch below is for a major
# version before 1.0: an interface is not yet stable at that time, so each
# minor release counts as its own ABI.
VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
ifeq ($(VERSION_MAJOR),0)
ABI_VERSION = $(VERSION_MAJOR).$(VERSION_MINOR)
else
ABI_VERSION = $(VERSION_MAJOR)
endif

# The real file has the full version in its name, and the SONAME and the plain
# development link are symlinks to it. This is the usual ELF layout with three
# names:
#   libccollections.so             -> what -lccollections resolves at link time
#   libccollections.so.1           -> the SONAME, what a built binary records
#   libccollections.so.1.0.0       -> the actual file
#
# macOS puts the version before the suffix, and the install name of the
# library plays the part of the SONAME:
#   libccollections.dylib, libccollections.1.dylib, libccollections.1.0.0.dylib
SHARED_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).$(CCOL_SHARED_LIBRARY_SUFFIX)
ifeq ($(CCOL_UNAME_S),Darwin)
SHARED_LIBRARY_SONAME = lib$(SHORT_LIBRARY_NAME).$(ABI_VERSION).dylib
SHARED_LIBRARY_REAL = lib$(SHORT_LIBRARY_NAME).$(VERSION).dylib
SHARED_LIBRARY_ANY_VERSION = lib$(SHORT_LIBRARY_NAME).*.dylib
else
SHARED_LIBRARY_SONAME = $(SHARED_LIBRARY_NAME).$(ABI_VERSION)
SHARED_LIBRARY_REAL = $(SHARED_LIBRARY_NAME).$(VERSION)
SHARED_LIBRARY_ANY_VERSION = $(SHARED_LIBRARY_NAME).*
endif
STATIC_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).a
PKGCONFIG_FILE = $(SHORT_LIBRARY_NAME).pc

# ---------------------------------------------------------------------------
# Optional modules
# ---------------------------------------------------------------------------
# The build includes every module by default. Set any of these to 0, here or on
# the command line, to leave that module out of the library:
#
#   make WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0
#
# The build selects a module by the sources that it compiles, instead of
# wrapping the body of a module in #ifdef. No other module includes the headers
# of the four optional modules, so each one is a leaf; this is why the build
# needs no conditional compilation to leave one out, and why the sources hold
# no build configuration clutter. The build also does not install the public
# header of a module that you turn off, so an application that includes that
# header fails at compile time with a missing file instead of at link time
# with undefined symbols.
#
# The purpose of these switches is to drop dependencies. OpenSSL comes into the
# build through one file only (src/ctls.c), and only the two HTTP modules use
# it, so with both of them off the build needs neither libssl nor libcrypto.
# zlib comes in through clogger alone. With all five modules off, the library
# needs only pthread and libm.
#
# A reduced build is NOT interchangeable with a full build at the ABI level: it
# keeps the same SONAME but exports fewer symbols, so an application that you
# link against a full build fails to start against a reduced one. Use a reduced
# build for a library that you build and embed yourself, and do not distribute
# one, because another program can mistake it for the complete library. `make
# check_abi` detects a reduced build and skips, because the committed baseline
# describes the full library.
WITH_CJSON ?= 1
WITH_CYAML ?= 1
WITH_CLOGGER ?= 1
WITH_CHTTPCLIENT ?= 1
WITH_CHTTPSERVER ?= 1

# src/ctls.c and src/chttp1_parser.c are internal. Only the two HTTP modules
# use them, so they follow those modules and have no switch of their own.
# The build tests each half against 1, in the same way as every other switch,
# and not against 0, so any other spelling means off. Do not join the two
# values and match them against one literal: that form reads "no" as "on" for
# one of them, and the result is a half-configured build that still compiles
# ctls and chttp1_parser, still links OpenSSL, and still runs their suites.
WITH_HTTP := 0
ifeq ($(WITH_CHTTPCLIENT),1)
WITH_HTTP := 1
endif
ifeq ($(WITH_CHTTPSERVER),1)
WITH_HTTP := 1
endif

# chttpclient and chttpserver both write their logs through clogger, so you
# cannot drop clogger while one of them is in the build. Instead of stopping
# with an error, the build turns clogger back on. This keeps
# `make WITH_CLOGGER=0` from being a build error for a user who wants a smaller
# library and does not know about this dependency.
ifeq ($(WITH_HTTP),1)
ifneq ($(WITH_CLOGGER),1)
$(warning WITH_CLOGGER=0 ignored: chttpclient/chttpserver depend on clogger)
override WITH_CLOGGER := 1
endif
endif

SOURCE_DIR = src
INCLUDE_DIR = include
OBJECT_DIR = obj
# The HTTP/1.1 response parser of chttpclient (src/chttp1_parser.c) and the
# other parts of the HTTP stack (ctls, ccol_event_loop, chttpserver,
# chttpclient) all belong to this project, so they need no build rules for a
# third party: the SOURCE_FILES wildcard over $(SOURCE_DIR)/*.c below finds
# them in the same way as it finds every other module of this library.
# The library holds no third-party source: it builds only its own .c files.
# The build skips the test directory of a module that you turn off. It also
# skips tests/mixed/ when you turn off anything, because that suite links
# across modules and cannot build against a partial library. tests/ctls/ and
# tests/chttp/ follow the HTTP switches, because they exercise the code that
# those modules bring in.
DISABLED_TEST_DIRS :=
ifneq ($(WITH_CJSON),1)
DISABLED_TEST_DIRS += cjson
endif
ifneq ($(WITH_CYAML),1)
DISABLED_TEST_DIRS += cyaml
endif
ifneq ($(WITH_CLOGGER),1)
DISABLED_TEST_DIRS += clogger
endif
ifneq ($(WITH_CHTTPCLIENT),1)
DISABLED_TEST_DIRS += chttpclient
endif
ifneq ($(WITH_CHTTPSERVER),1)
DISABLED_TEST_DIRS += chttpserver
endif
# This is for ctls only: src/ctls.c goes out with the HTTP modules, so its
# suite has nothing left to compile. tests/chttp stays, because src/chttp.c
# holds the shared types, the base64 helpers and the RFC 7617 helpers and ships
# in every configuration, and that suite needs nothing else. If the build
# dropped it, a shipped file would have no suite, and the coverage gate for
# each file would then report that file as not compiled by any suite.
ifneq ($(WITH_HTTP),1)
DISABLED_TEST_DIRS += ctls
endif
ifneq ($(strip $(DISABLED_TEST_DIRS)),)
DISABLED_TEST_DIRS += mixed
endif

empty :=
space := $(empty) $(empty)
TEST_DIR_FILTER = $(if $(strip $(DISABLED_TEST_DIRS)),| grep -vE '$(subst $(space),|,$(patsubst %,/%/,$(strip $(DISABLED_TEST_DIRS))))')

TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -v /tau/ $(TEST_DIR_FILTER))
COVERAGE_TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -vE '/tau/|/mixed/' $(TEST_DIR_FILTER))

_create_object_dir := $(shell mkdir -p $(OBJECT_DIR))

EXTRA_CFLAGS ?=
# -Wl,-z,relro,-z,now is a *linker* flag: the -Wl, prefix sends it straight to
# ld, and it is not a compiler flag. It belongs in SHARED_LDFLAGS, which
# applies at the link step that makes libccollections.so, and not here in
# COMMON_CFLAGS, which this Makefile uses only for the `$(CC) -c ...` steps
# below that compile an object and do not link. A linker flag here goes away
# before it reaches a real link, so it gives no RELRO or BIND_NOW hardening at
# all. Under clang it also breaks the build: the clang driver treats an unused
# linker argument on a compile-only command as an error under -Werror, while
# gcc accepts it silently. See SHARED_LDFLAGS below, where the flag takes
# effect. A static archive has no equivalent, because `ar` receives
# STATIC_LDFLAGS and `ar` is not a linker. RELRO and BIND_NOW are properties of
# a real ELF executable or shared object, so whatever links libccollections.a
# into a program must apply them.
# -D_FILE_OFFSET_BITS=64 is for a 32-bit (ILP32) target, where readdir() in
# glibc must narrow the 64-bit d_ino of the kernel into the ino_t of the
# caller. Without this flag that ino_t is only 32 bits wide, and readdir() then
# fails with EOVERFLOW as soon as a directory holds an entry whose real inode
# number does not fit. This is normal on a modern filesystem with 64-bit
# inodes; it is not a corrupt input and not an attack. This flag makes ino_t,
# off_t, stat and the related types 64 bits wide on every target, and it
# changes nothing on a 64-bit build, where they are already 64 bits wide.
# -fvisibility=hidden makes every function and object internal to the library
# by default, and the build exports only the declarations inside the `#pragma
# GCC visibility push(default)` blocks of the installed public headers. This
# keeps the dynamic symbol table equal to the documented public API, so an
# internal helper never becomes part of the ABI without notice, and an
# application symbol of the same name can never interpose an internal call of
# the library. The flag also removes one indirection: a call to an exported
# function from inside the library can be interposed, so it goes through the
# PLT, while a call to a hidden function is direct.
# -std=gnu11 pins the language mode of the public headers: C11 plus the GNU
# extensions that the macro layer needs (typeof, statement expressions and
# __auto_type). Without this flag the build takes the default mode of the
# compiler, and those defaults change over time; a toolchain whose default is
# gnu23 changes the meaning of `bool`, of `static_assert`, and of an empty
# parameter list. An unset mode therefore makes the build machine decide which
# language the library is compiled as, while a named mode keeps one build the
# same across compilers, and across versions of one compiler.
# A hardening flag is only worth passing where the compiler really implements
# it FOR THIS TARGET. -fstack-clash-protection is the one that forced this
# probe: Clang implements it on x86 and s390x and not on 32-bit ARM. On armhf
# the clang driver accepts the flag, does nothing with it, and then reports
# "argument unused during compilation", which -Werror turns into a hard error,
# so without the probe the library cannot be built at all with clang on armhf.
# GCC accepts the flag on armhf and implements it, so this is a property of
# the pair (compiler, target) and not of either one alone.
#
# The probe compiles an empty translation unit with the flag, with -Werror and
# with EXTRA_CFLAGS, and keeps the flag only when that succeeds. EXTRA_CFLAGS
# belongs in the probe because it can change the answer: with
# -fsanitize=address, Apple clang predefines _FORTIFY_SOURCE as 0 (the
# sanitizer checks those accesses itself), and -D_FORTIFY_SOURCE=3 then
# redefines the macro. This is strictly better than dropping the flag on a
# hardcoded list of targets, which goes stale as compilers gain support, and
# better than the -Wno-error=unused-command-line-argument that the
# alternative needs, which silences the diagnostic for the whole build,
# including the places where it reports a real mistake.
#
# The probe must never hide the LOSS of hardening, so `make hardening_report`
# below prints which flags survived for the current compiler and target. See
# also check_hardening in ci_scripts, which reads the built artifact rather
# than the flags, because a flag in the wrong variable disappears in silence.
cc-option = $(shell printf 'int main(void){return 0;}' > .ccol_probe.c 2>/dev/null && 	if $(CC) $(EXTRA_CFLAGS) $(1) -Werror -c .ccol_probe.c -o .ccol_probe.o >/dev/null 2>&1; 	then printf '%s' '$(1)'; fi; rm -f .ccol_probe.c .ccol_probe.o)

HARDENING_CFLAGS := $(call cc-option,-fstack-protector-strong) \
	$(call cc-option,-fstack-clash-protection) \
	$(call cc-option,-D_FORTIFY_SOURCE=3)

COMMON_CFLAGS = -I$(INCLUDE_DIR) \
	-std=gnu11 \
	-fvisibility=hidden \
	$(HARDENING_CFLAGS) \
	-D_FILE_OFFSET_BITS=64 \
	-Wstrict-overflow -Wformat=2 -Wformat-security -Wall -Wextra \
	-g -O3 -Werror -fPIC $(EXTRA_CFLAGS)


# Different cflags for the shared build and the static build
SHARED_CFLAGS = $(COMMON_CFLAGS)
STATIC_CFLAGS = $(COMMON_CFLAGS)

# Different ldflags for the shared build and the static build.
# This names only the libraries that the modules in the build need: zlib comes
# with clogger, and OpenSSL comes with the HTTP modules. A build without those
# modules links neither library, and `pkg-config --libs --static ccollections`
# then does not name them either.
EXTERNAL_LIBS = $(CCOL_PLATFORM_LIBS)
PC_REQUIRES_PRIVATE =
ifeq ($(WITH_CLOGGER),1)
EXTERNAL_LIBS += -lz
PC_REQUIRES_PRIVATE += zlib
endif
ifeq ($(WITH_HTTP),1)
EXTERNAL_LIBS += -lssl -lcrypto
PC_REQUIRES_PRIVATE += libssl libcrypto
endif

ifeq ($(CCOL_UNAME_S),Darwin)
# The install name is where an application that links the library looks for
# it at run time, so it names the installed copy.
SHARED_LDFLAGS = -dynamiclib -install_name $(LIBDIR)/$(SHARED_LIBRARY_SONAME) \
	-compatibility_version $(ABI_VERSION) -current_version $(VERSION) \
	$(EXTERNAL_LIBS)
else
SHARED_LDFLAGS = $(CCOL_LD_HARDENING) -Wl,-soname,$(SHARED_LIBRARY_SONAME) -shared $(EXTERNAL_LIBS)
endif
STATIC_LDFLAGS =

# cdebuglog.c is not in this list on purpose. It is a diagnostic log buffer
# that a suite turns on for itself, it compiles to nothing unless the build
# defines RUNNING_UNIT_TESTS, and its purpose is to help you find a CI timing
# problem or a hang that is hard to reproduce. See the doc comment in
# include/internal/cdebuglog.h. It is not part of the shipped library, not
# even as the empty translation unit that a production build would compile it
# to; a test suite that needs it adds src/cdebuglog.c to the SRC_FILES of its
# own Makefile.
# The list below holds the sources that the WITH_* switches above leave out,
# together with cdebuglog.c, which is never part of the shipped library.
DISABLED_SOURCES :=
DISABLED_HEADERS :=
ifneq ($(WITH_CJSON),1)
DISABLED_SOURCES += $(SOURCE_DIR)/cjson.c
DISABLED_HEADERS += $(INCLUDE_DIR)/cjson.h
endif
ifneq ($(WITH_CYAML),1)
DISABLED_SOURCES += $(SOURCE_DIR)/cyaml.c
DISABLED_HEADERS += $(INCLUDE_DIR)/cyaml.h
endif
ifneq ($(WITH_CLOGGER),1)
DISABLED_SOURCES += $(SOURCE_DIR)/clogger.c
DISABLED_HEADERS += $(INCLUDE_DIR)/clogger.h
endif
ifneq ($(WITH_CHTTPCLIENT),1)
DISABLED_SOURCES += $(SOURCE_DIR)/chttpclient.c
DISABLED_HEADERS += $(INCLUDE_DIR)/chttpclient.h
endif
ifneq ($(WITH_CHTTPSERVER),1)
DISABLED_SOURCES += $(SOURCE_DIR)/chttpserver.c
DISABLED_HEADERS += $(INCLUDE_DIR)/chttpserver.h
endif
ifneq ($(WITH_HTTP),1)
DISABLED_SOURCES += $(SOURCE_DIR)/ctls.c $(SOURCE_DIR)/chttp1_parser.c
endif

SOURCE_FILES = $(filter-out $(SOURCE_DIR)/cdebuglog.c $(DISABLED_SOURCES),$(wildcard $(SOURCE_DIR)/*.c))
# This reads both directories: include/ holds the installed public headers and
# include/internal/ holds the internal ones. The second wildcard is necessary,
# not only tidy, because this list is a prerequisite of every object. An
# internal header that is missing from the list does not start a rebuild of
# the sources that include it, and you then get a stale object with no error
# to tell you so.
HEADER_FILES = $(filter-out $(DISABLED_HEADERS),\
                 $(wildcard $(INCLUDE_DIR)/*.h) \
                 $(wildcard $(INCLUDE_DIR)/internal/*.h))
OBJ_FILES_SHARED = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.o)
OBJ_FILES_STATIC = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.static.o)

default: all

# Prints which hardening flags this compiler and target really accept. A flag
# that the probe above dropped is a real loss of hardening for this build, and
# it must be visible rather than silent.
.PHONY: hardening_report
hardening_report:
	@echo "CC                = $(CC)"
	@echo "HARDENING_CFLAGS  = $(HARDENING_CFLAGS)"
	@for f in -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3; do \
		case " $(HARDENING_CFLAGS) " in \
			*" $$f "*) echo "  ok       $$f" ;; \
			*) echo "  DROPPED  $$f (this compiler, with EXTRA_CFLAGS, does not take it for this target)" ;; \
		esac; \
	done

test:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && $(MAKE) test) &&) true

memtest:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && $(MAKE) memtest) &&) true

# Each module runs in its own subshell and && joins the list, so the first
# failure stops the sweep and gives a non-zero status. Do not join a
# `cd $(folder) && ... && cd -` chain with `;` instead: that form leaves the
# shell in the directory of the module that failed, the relative cd of every
# module after it then fails too, and sixteen "can't cd" lines hide the one
# real error.
generate_coverage_report:
	$(foreach folder,$(COVERAGE_TEST_FOLDERS),(cd $(folder) && $(MAKE) generate_coverage_report) &&) true

# ---------------------------------------------------------------------------
# Aggregate coverage site
# ---------------------------------------------------------------------------
# The generate_coverage_report target of each module leaves its lcov
# tracefiles in tests/<module>/coverage/. This target merges all of them into
# one report for the whole library. The merge is what makes the numbers
# mean something: a file such as src/common.c is compiled into most of the
# module test binaries, and only the union of their runs shows how much of
# that file the suite reaches.
#
# A module can leave more than one tracefile (a suite that builds several
# binaries from one directory writes one tracefile for each binary), and this
# target reads every one of them.
#
# This target then narrows the merged data to src/ and include/ alone. The
# coverage of the test code, of the test framework, and of the system headers
# says nothing about how well the library is tested, and it makes every
# headline number look better than it is. The extract patterns start at
# $(CURDIR). Do not write them as '*/include/*': that pattern also matches
# /usr/include/... and brings six glibc and OpenSSL headers into the report,
# one of them at 0.0 percent.
# This stops the build if any part of the public interface loses its namespace
# prefix. The test suites cannot check this, because they compile the .c files
# straight into their own binaries, where a symbol without a prefix, or a
# symbol that is not exported, links in the same way as a correct one.
#
# The check reads both shipped artifacts, because hidden visibility makes the
# two disagree about what the interface is. A helper that stays out of the
# dynamic symbol table of the shared object is still a global definition in
# the archive, and a static link resolves it in the same way as an exported
# symbol, so an application that defines its own symbol of that name fails to
# link. A check of the shared object alone reports a clean interface for a
# name that can still collide in the archive.
# Every installed header must compile on its own under a strict -std=c11 with
# -pedantic-errors, with both compilers. Code that USES a typed macro still
# needs -std=gnu11, because the macros use statement expressions, but merely
# INCLUDING a header must not, or a consumer's own header cannot include one
# without forcing gnu11 on everything downstream of it. The check also
# compiles each header from the installed layout, behind an application
# directory that shadows every other public name.
.PHONY: check_headers
check_headers:
	@./ci_scripts/check_public_headers_standalone.sh

.PHONY: check_namespace
check_namespace: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME)
	@./ci_scripts/check_public_namespace.sh $(SHARED_LIBRARY_REAL) $(STATIC_LIBRARY_NAME)

# Fails if a file under include/ or man/ carries a name that the install would
# paste onto the installation directory as a shell pattern rather than as one
# literal path. The install and uninstall recipes build their argument lists
# from these basenames. A `*`, `?` or `[` in one of them makes `rm -f` delete,
# and `sed -i` rewrite, whatever it matches inside the installation directory,
# which is other packages' files. The recipes run under `set -f` so that the
# shell expands nothing; this check is the other half of the same guard, and it
# keeps such a name out of the tree so that the property does not rest on one
# `set -f` surviving a future edit. It also requires every man alias page to
# be the single line ".so <module>/<symbol>.3" that the install rewrites, with
# a target that exists. It needs no build artifact.
.PHONY: check_filenames
check_filenames:
	@./ci_scripts/check_installable_filenames.sh

# Fails if the set of exported symbols has drifted from abi/, which records the
# ABI that libccollections.so.$(ABI_VERSION) promises. A removed symbol breaks
# every application already linked against it; a newly added one is compatible,
# but has to be a deliberate, reviewed line in a diff rather than something that
# escaped through a missing `static`. Where libabigail is installed and an
# architecture-matched corpus baseline exists, the same run also compares
# function signatures and public struct layouts, which a symbol list cannot
# describe. Like check_namespace, this is invisible to the test suites: they
# link the .c files directly, where an unexported symbol resolves exactly as an
# exported one does.
.PHONY: check_abi
check_abi: $(SHARED_LIBRARY_NAME)
ifeq ($(strip $(DISABLED_SOURCES)),)
	@./ci_scripts/check_public_abi.sh $(SHARED_LIBRARY_REAL) $(ABI_VERSION)
else
	@echo "check_abi: skipped, this is a reduced build (modules disabled:$(patsubst $(SOURCE_DIR)/%.c, %,$(DISABLED_SOURCES)))."
	@echo "           The committed baseline describes the full library, so a"
	@echo "           smaller symbol set here is expected rather than a defect."
endif

# This check proves that a process can still fork() after it loads this
# library with dlopen(), uses a module that registers pthread_atfork()
# handlers, and then unloads the library again. Like the two checks above, it
# needs the built .so: the test suites link the .c files directly, so no test
# ever loads or unloads a shared object.
.PHONY: check_dso_unload
check_dso_unload: $(SHARED_LIBRARY_NAME)
	@./ci_scripts/check_dso_unload.sh $(SHARED_LIBRARY_REAL)

# This target links a program that uses little of the library against the
# built shared object, and fails when valgrind finds memory of the library
# that is allocated at exit. The test suites compile the sources into their
# own binaries, so no suite sees what the destructors of the shared object do
# at exit.
# Installs into a temporary prefix, builds the example of README.md with the
# flags of the installed ccollections.pc, runs it linked against the shared
# library and against the archive, and uninstalls. No test suite sees the
# install, because each suite compiles the sources into its own binary.
.PHONY: check_install
check_install: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME)
	@MAKE="$(MAKE)" CC="$(CC)" ./ci_scripts/check_install.sh

.PHONY: check_exit_reachable
check_exit_reachable: $(SHARED_LIBRARY_NAME)
	@CC="$(CC)" CCOL_VALGRIND_SUPP="$(CCOL_VALGRIND_SUPP)" ./ci_scripts/check_exit_reachable.sh $(SHARED_LIBRARY_REAL)

# This records the baseline again from the current build. Run it when the
# public API gets a new symbol, and commit the new baseline with that change.
.PHONY: update_abi_baseline
update_abi_baseline: $(SHARED_LIBRARY_NAME)
ifeq ($(strip $(DISABLED_SOURCES)),)
	@./ci_scripts/check_public_abi.sh $(SHARED_LIBRARY_REAL) $(ABI_VERSION) --update
else
	@echo "update_abi_baseline: refused, this is a reduced build (modules disabled:$(patsubst $(SOURCE_DIR)/%.c, %,$(DISABLED_SOURCES)))." >&2
	@echo "                     Recording it would replace the committed contract with" >&2
	@echo "                     a truncated one, and the next full build would then" >&2
	@echo "                     report every dropped symbol as newly added." >&2
	@false
endif

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
# bench/ links against the shared library that this build produces instead of
# compiling the sources into its own binary, so it measures the code that
# ships, at the flags that it ships with, through the same dynamic call that
# an application makes. The baseline that it compares against belongs to one
# machine, and the project does not commit it: a figure in nanoseconds for
# each operation describes the cache hierarchy and the background load of one
# machine, so a run here against a baseline from another machine reports a
# difference that has nothing to do with the library. Record a baseline with
# bench_update, and bench then reports every later run against it.
#
# BENCH_ARGS sends options straight through. For example:
#   make bench BENCH_ARGS="--filter=chashmap --reps=15"
BENCH_ARGS ?=

.PHONY: bench
bench: $(SHARED_LIBRARY_NAME)
	@$(MAKE) --no-print-directory -C bench run BENCH_ARGS="$(BENCH_ARGS)"

.PHONY: bench_update
bench_update: $(SHARED_LIBRARY_NAME)
	@$(MAKE) --no-print-directory -C bench update BENCH_ARGS="$(BENCH_ARGS)"

.PHONY: bench_gate
bench_gate: $(SHARED_LIBRARY_NAME)
	@$(MAKE) --no-print-directory -C bench gate BENCH_ARGS="$(BENCH_ARGS)"

.PHONY: bench_calibrate
bench_calibrate: $(SHARED_LIBRARY_NAME)
	@$(MAKE) --no-print-directory -C bench calibrate BENCH_ARGS="$(BENCH_ARGS)"

.PHONY: bench_list
bench_list: $(SHARED_LIBRARY_NAME)
	@$(MAKE) --no-print-directory -C bench list

COVERAGE_SITE_DIR = coverage_site

# This fails when a file in src/ or include/ has coverage below 80 percent of
# its instrumented lines. It is a separate target from coverage_site for two
# reasons: you can make the report again and look at it without the check,
# and you can run the check again against a report that exists, without the
# cost of another instrumented build.
.PHONY: coverage_check
coverage_check:
	@CCOL_DISABLED_SOURCES="$(DISABLED_SOURCES) $(DISABLED_HEADERS)" \
		./ci_scripts/check_test_coverages.sh $(COVERAGE_SITE_DIR)/library.info
LCOV_QUIRK_FLAGS = --ignore-errors inconsistent --ignore-errors empty \
                   --ignore-errors unused --ignore-errors corrupt

coverage_site: generate_coverage_report
	@rm -rf $(COVERAGE_SITE_DIR)
	@mkdir -p $(COVERAGE_SITE_DIR)
	@set -e; \
	tracefiles=""; \
	for f in tests/*/coverage/*.info; do \
		[ -e "$$f" ] || continue; \
		tracefiles="$$tracefiles --add-tracefile $$f"; \
	done; \
	if [ -z "$$tracefiles" ]; then \
		echo "coverage_site: no lcov tracefiles under tests/*/coverage/" >&2; \
		exit 1; \
	fi; \
	lcov $$tracefiles $(LCOV_QUIRK_FLAGS) \
		--output-file $(COVERAGE_SITE_DIR)/merged.info; \
	lcov --extract $(COVERAGE_SITE_DIR)/merged.info \
		'$(CURDIR)/$(SOURCE_DIR)/*' '$(CURDIR)/$(INCLUDE_DIR)/*' \
		$(LCOV_QUIRK_FLAGS) \
		--output-file $(COVERAGE_SITE_DIR)/library.info; \
	genhtml $(COVERAGE_SITE_DIR)/library.info $(LCOV_QUIRK_FLAGS) \
		--legend --show-details --title "c_collections $(VERSION)" \
		--output-directory $(COVERAGE_SITE_DIR)/html; \
	lcov --summary $(COVERAGE_SITE_DIR)/library.info \
		$(LCOV_QUIRK_FLAGS) 2>&1 | tee $(COVERAGE_SITE_DIR)/summary.txt

# view_coverage_report:
# 	firefox $(for i in $(ls -1 ./tests/ | grep -vE 'mixed|tau'); do s=$(basename $i); echo ./tests/$s/coverage/src/$s.c.gcov.html; done | xargs)

# An object file records nothing about the compiler or the flags that made it,
# so an ordinary prerequisite list lets a build with different flags link
# objects from the build before it. You then get a sanitizer that is only half
# applied or, after a CC= change, objects for the wrong architecture, and a
# switch such as CCOL_MEMPOOL_COMPACT_LAYOUT looks as if the build ignored it,
# because the object that holds it was never compiled again. The stamp holds
# the current compiler and the current flags, and the build writes it again
# only when one of them changes, so nothing is built again without need and
# everything is built again when it must be. The stamp uses FORCE and not a
# prerequisite because these values arrive on the command line, with no input
# file changing alongside them.
BUILD_FLAGS_STAMP = $(OBJECT_DIR)/.build_flags

# The link has its own identity, separate from the identity of the compile.
# The WITH_* switches change which objects go into the library and which
# libraries the build links against, but not how the build compiles any
# object. After a full build, every object that a reduced build needs is
# therefore already there and up to date, so the build does not link the
# library again, and the library keeps the modules and the NEEDED entries of
# the build before it. For example,
# `make WITH_CHTTPSERVER=0 WITH_CHTTPCLIENT=0` after an ordinary build gives
# you a library that still exports both modules and still links OpenSSL, and
# nothing in the output tells you so. A clean tree hides this completely,
# which is why the CI job that checks the dependencies of a reduced build
# cannot catch it.
#
# This is a second stamp and not part of the stamp above, because a change to
# a module switch must not compile every object again for a compile that has
# not changed.
LINK_FLAGS_STAMP = $(OBJECT_DIR)/.link_flags

$(BUILD_FLAGS_STAMP): FORCE
	@mkdir -p $(OBJECT_DIR)
	@printf '%s\n' '$(CC)|$(SHARED_CFLAGS)|$(STATIC_CFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(LINK_FLAGS_STAMP): FORCE
	@mkdir -p $(OBJECT_DIR)
	@printf '%s\n' '$(CC)|$(AR)|$(EXTRA_CFLAGS)|$(SHARED_LDFLAGS)|$(STATIC_LDFLAGS)|$(OBJ_FILES_SHARED)|$(OBJ_FILES_STATIC)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

all: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE)

# EXTRA_CFLAGS goes to the link too, so that a sanitizer that it turns on
# links its runtime. A Linux shared object may keep the runtime symbols
# undefined and get them from the program, but -dynamiclib on macOS refuses
# any undefined symbol.
$(SHARED_LIBRARY_REAL): $(OBJ_FILES_SHARED) $(LINK_FLAGS_STAMP)
	$(CC) $(EXTRA_CFLAGS) -o $(SHARED_LIBRARY_REAL) $(OBJ_FILES_SHARED) $(SHARED_LDFLAGS)

$(SHARED_LIBRARY_SONAME): $(SHARED_LIBRARY_REAL)
	ln -sf $(SHARED_LIBRARY_REAL) $(SHARED_LIBRARY_SONAME)

$(SHARED_LIBRARY_NAME): $(SHARED_LIBRARY_SONAME)
	ln -sf $(SHARED_LIBRARY_SONAME) $(SHARED_LIBRARY_NAME)

# The build generates the pkg-config metadata and the project does not commit
# it, so the version and the installation directories in that file can never
# differ from the ones that this build used. FORCE is what makes this true:
# PREFIX, LIBDIR and INCLUDEDIR come in on the command line, so they can change
# while no input file changes with them, and an ordinary prerequisite list
# would leave an old directory in the installed file. The build writes the
# file again only when the new text differs from the old text, so it generates
# the file every time and still starts no other build step.
#
# A LIBDIR or an INCLUDEDIR under PREFIX is written relative to ${prefix} or
# ${exec_prefix}, so that `pkg-config --define-prefix` and a relocated tree
# keep working. A directory outside PREFIX is written as it is.
.PHONY: FORCE
FORCE:

PC_LIBDIR = $(patsubst $(PREFIX)/%,$${exec_prefix}/%,$(LIBDIR))
PC_INCLUDEDIR = $(patsubst $(PREFIX)/%,$${prefix}/%,$(INCLUDEDIR))

$(PKGCONFIG_FILE): $(PKGCONFIG_FILE).in FORCE
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(PC_LIBDIR)|g' \
	     -e 's|@INCLUDEDIR@|$(PC_INCLUDEDIR)|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     -e 's|@REQUIRES_PRIVATE@|$(strip $(PC_REQUIRES_PRIVATE))|g' \
	     -e 's|@LIBS_PRIVATE@|$(strip $(CCOL_PLATFORM_LIBS))|g' \
	     $(PKGCONFIG_FILE).in > $(PKGCONFIG_FILE).tmp
	@if cmp -s $(PKGCONFIG_FILE).tmp $(PKGCONFIG_FILE); then \
		rm -f $(PKGCONFIG_FILE).tmp; \
	else \
		mv $(PKGCONFIG_FILE).tmp $(PKGCONFIG_FILE); \
		echo "generated $(PKGCONFIG_FILE) (prefix=$(PREFIX) libdir=$(LIBDIR) includedir=$(INCLUDEDIR) version=$(VERSION))"; \
	fi

# `ar rcs` adds to an archive that exists instead of replacing it, so a
# reduced build over a full one would keep the members of the modules that
# you dropped, even after the build makes the archive again. The build
# removes the archive first, which is what makes the member list follow
# $(OBJ_FILES_STATIC) exactly.
$(STATIC_LIBRARY_NAME): $(OBJ_FILES_STATIC) $(LINK_FLAGS_STAMP)
	@rm -f $(STATIC_LIBRARY_NAME)
	$(AR) rcs $(STATIC_LIBRARY_NAME) $(OBJ_FILES_STATIC) $(STATIC_LDFLAGS)
	$(RANLIB) $(STATIC_LIBRARY_NAME)

# The objects for the shared library, built with -fPIC
$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES) $(BUILD_FLAGS_STAMP)
	$(CC) -c $(SHARED_CFLAGS) $< -o $@

# The objects for the static library. The build still uses -fPIC, so that a
# user can link the archive into a shared library. Note: an archive is not a
# linked object, so it records no dependency of its own on pthread, zlib,
# OpenSSL or libm, and the user must name everything that the library needs
# on their own link line. $(PKGCONFIG_FILE) holds that list, so `pkg-config
# --libs --static ccollections` gives it to them automatically.
$(OBJECT_DIR)/%.static.o: $(SOURCE_DIR)/%.c $(HEADER_FILES) $(BUILD_FLAGS_STAMP)
	$(CC) -c $(STATIC_CFLAGS) $< -o $@

# This recipe names tests/cyaml/differential/cyaml_to_json directly instead of
# matching it: that binary is the only built binary that is neither directly
# in a suite directory nor named tests_ or fuzz_, so the find below cannot
# describe it.
#
# The recipe finds the test binaries instead of listing them, so a directory
# that gets a new binary, such as tests/mixed/tests_compat and its
# ThreadSanitizer twin, is cleaned with no edit to this recipe. The name
# filter is what makes that safe: tests_compat.c and tests_tls.c sit beside
# the binaries that they build, so a plain tests/*/tests_* would delete
# sources. A binary here never has a dot in its name and every other match
# does, so the filter refuses every match whose name holds a dot and keeps
# exactly the binaries.
#
# The recipe removes the shared library by glob and not by its three current
# names, so it also cleans a build tree that holds artifacts from a different
# VERSION. If the recipe named only the current version, an old
# libccollections.so.<other> (libccollections.<other>.dylib on macOS) would
# stay beside the new one, where at run time it can still satisfy the
# DT_NEEDED of an older binary.
clean:
	rm -rf $(SHARED_LIBRARY_NAME) $(SHARED_LIBRARY_ANY_VERSION) \
		$(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE) $(PKGCONFIG_FILE).tmp $(OBJECT_DIR) \
		tests/*/*.o tests/*/.build-flags* \
		bench/bench bench/*.o bench/.build-flags \
		tests/*/coverage tests/*/third_party_obj $(COVERAGE_SITE_DIR) \
		tests/*/differential/cyaml_to_json \
		tests/*/*.gcno tests/*/*.gcda tests/*/*.gcov tests/*/*.c.info
	@if [ -d tests ]; then \
		find tests -maxdepth 2 -type f \
			\( -name tests -o -name 'tests_*' -o -name 'fuzz_*' \) \
			! -name '*.*' -print0 | xargs -0 -r rm -f; \
	fi

# PREFIX moves a whole install in one step. Use it for a distribution package,
# for an install into $HOME/.local for one user, or for a staged install into a
# container image.
# DESTDIR puts a staging directory in front of every install path without
# appearing in the contents of any installed file, which is what a package
# build needs. DESTDIR must never reach the prefix that $(PKGCONFIG_FILE)
# records.
#
# LIBDIR, INCLUDEDIR, MANDIR and PKGCONFIGDIR each move one part of the
# install, for a distribution whose layout differs from PREFIX/lib and the
# rest: a Fedora package passes LIBDIR=/usr/lib64, and a Debian package passes
# LIBDIR=/usr/lib/<triplet>. $(PKGCONFIG_FILE) records LIBDIR and INCLUDEDIR,
# so a pkg-config consumer finds whatever this install chose.
#
# The public headers go into a directory of their own,
# INCLUDEDIR/$(SHORT_LIBRARY_NAME), and never straight into INCLUDEDIR. Names
# such as common.h and cstring.h are generic, and in a shared include
# directory they would shadow, or be shadowed by, a header of another package
# or of the application. Each public header includes its sibling headers with
# quotes, so it always finds its own copy first, in its own directory. An
# application writes #include <ccollections/chashmap.h>, and the pkg-config
# Cflags name INCLUDEDIR itself with -I. Because no -I flag ever names the
# directory of the library headers, an application header of the same name as
# one of ours, such as its own common.h, is never shadowed by ours and never
# shadows ours, whatever order the -I flags of the application come in.
PREFIX ?= /usr/local
DESTDIR ?=
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
MANDIR ?= $(PREFIX)/share/man
PKGCONFIGDIR ?= $(CCOL_DEFAULT_PKGCONFIGDIR)
HEADER_INSTALL_DIR = $(DESTDIR)$(INCLUDEDIR)/$(SHORT_LIBRARY_NAME)
LIBRARY_INSTALL_DIR = $(DESTDIR)$(LIBDIR)
MAN_INSTALL_DIR = $(DESTDIR)$(MANDIR)
PKGCONFIG_INSTALL_DIR = $(DESTDIR)$(PKGCONFIGDIR)

# The recipes paste these directories into shell command lines unquoted, and
# the pkg-config rule pastes them into a sed script. A value with a space or a
# tab in it is split into several words there, so `install -d` and `rm -f` act
# on each word as a path of its own, relative to the source tree, and the
# install creates stray directories inside it. The install and the uninstall
# therefore refuse such a value before they touch anything. The test catches
# a blank inside a value (a second word) and a trailing blank (make keeps it,
# and strip removes it).
INSTALL_PATH_VARIABLES = DESTDIR PREFIX LIBDIR INCLUDEDIR MANDIR PKGCONFIGDIR
BLANK_INSTALL_PATH_VARIABLES = $(strip $(foreach v,$(INSTALL_PATH_VARIABLES),\
	$(if $(or $(word 2,$($(v))),$(subst $(strip $($(v))),,$($(v)))),$(v))))

.PHONY: _install_path_guard
_install_path_guard:
ifneq ($(BLANK_INSTALL_PATH_VARIABLES),)
	@echo "install, uninstall: refused, these directories contain a space or a tab:" \
	      "$(BLANK_INSTALL_PATH_VARIABLES)." >&2
	@echo "         The recipes cannot pass such a path through intact." >&2
	@echo "         Choose a directory whose path holds no blank." >&2
	@false
endif
	@:

# Everything under $(INCLUDE_DIR)/internal/ is internal to the library: no
# public header includes it, and the shared library does not export the
# symbols that it declares, so you could not link against an installed copy.
# These headers stay in the source tree and out of the install. The directory
# itself is the boundary, which is why the wildcard below needs no list of
# names beside it, and why a new header there is left out of the install
# automatically.
INTERNAL_HEADER_FILES = $(wildcard $(INCLUDE_DIR)/internal/*.h)
PUBLIC_HEADER_FILES = $(filter-out $(INTERNAL_HEADER_FILES),$(HEADER_FILES))
# Every header that this project can install, whatever the WITH_* switches say.
# The uninstall target reads this list and not the filtered list above.
# Otherwise an uninstall with different switches from the install that put the
# files there would step past the headers that its own switches leave out and
# leave them behind, where they would declare symbols that the library does
# not have.
ALL_PUBLIC_HEADER_FILES = $(filter-out $(INTERNAL_HEADER_FILES),\
                            $(wildcard $(INCLUDE_DIR)/*.h))

# The man/<module>/*.3 pages document the functions and their companion
# type-inferred macros, side by side; see man/README. The install puts them
# all into one flat man3 directory, and this flat layout loses nothing,
# because real symbol names never collide across the two kinds of page. The
# man/<module>/*.7 pages are the module overview pages. An alias page holds a
# ".so <module>/<symbol>.3" redirect, which is relative to the layout of the
# source tree, and the install rewrites it to ".so man3/<symbol>.3", which is
# relative to the root of the installed MANPATH.
ALL_MAN3_SRC_FILES = $(wildcard man/*/*.3)
ALL_MAN7_SRC_FILES = $(wildcard man/*/*.7)
# The pages of a module that you turn off leave the install with its header:
# they document functions that no installed header declares and that the
# installed library does not hold, which is the same reason that the build
# drops the header itself. The uninstall target uses the unfiltered lists
# above, for the reason given there.
DISABLED_MAN_DIRS = $(patsubst $(SOURCE_DIR)/%.c,man/%/,$(DISABLED_SOURCES))
DISABLED_MAN_FILES = $(foreach d,$(DISABLED_MAN_DIRS),$(wildcard $(d)*))
MAN3_SRC_FILES = $(filter-out $(DISABLED_MAN_FILES),$(ALL_MAN3_SRC_FILES))
MAN7_SRC_FILES = $(filter-out $(DISABLED_MAN_FILES),$(ALL_MAN7_SRC_FILES))

# Ask for more rights only when the install needs them. An install into a
# prefix that the current user owns must not ask for a password; $HOME/.local
# is one such prefix, and so is a DESTDIR staging tree that a package build or
# a CI job uses. The thing to test is whether the nearest ancestor that exists
# is writable, because the install itself makes the leaf directories. Set the
# value yourself with `make install SUDO=` or `make install SUDO=doas`.
# This uses origin and not ?=, for two reasons: both `make install SUDO=doas`
# and a SUDO from the environment still win, and the probe below runs one time
# only. A plain `?=` would run the whole shell again at each of the fifteen or
# so expansions that one install makes, and a plain `:=` would ignore the form
# that comes from the environment.
#
# The walk stops at a component that holds no separator: `$${d%/*}` does
# nothing once there is no slash left to remove, and without that stop a
# relative DESTDIR or PREFIX would make the walk run forever.
# `make install DESTDIR=stage PREFIX=/usr` is one such ordinary packaging
# command. The two shapes do not end at the same fallback. If you remove the
# last component of an ABSOLUTE path that has one component, nothing is left,
# so the nearest ancestor that exists there is /, not the working directory.
# `make install PREFIX=/newroot` must therefore see that / is not writable and
# ask for more rights, instead of testing a directory that the caller happens
# to own.
#
# The probe tests every installation directory and not PREFIX alone, because
# LIBDIR, INCLUDEDIR, MANDIR and PKGCONFIGDIR can each point outside PREFIX,
# and one directory that the user cannot write is enough to need the
# escalation.
ifeq ($(origin SUDO),undefined)
SUDO := $(shell need=""; \
                for d in "$(HEADER_INSTALL_DIR)" "$(LIBRARY_INSTALL_DIR)" \
                         "$(MAN_INSTALL_DIR)" "$(PKGCONFIG_INSTALL_DIR)"; do \
                  root=.; \
                  if [ "$${d#/}" != "$$d" ]; then root=/; fi; \
                  while [ -n "$$d" ] && [ ! -e "$$d" ]; do \
                    nd=$${d%/*}; \
                    if [ "$$nd" = "$$d" ]; then nd=""; fi; \
                    d="$$nd"; \
                  done; \
                  [ -n "$$d" ] || d="$$root"; \
                  if [ "$$(id -u)" -ne 0 ] && [ ! -w "$$d" ]; then need=sudo; fi; \
                done; \
                echo "$$need")
endif

# ldconfig makes the cache of the dynamic linker current, and mandb (makewhatis
# on FreeBSD; see platform.mk) does the same for the man index. Both act on the
# live system, and the install skips both when it stages files into a DESTDIR
# for packaging, because the live system is exactly what a staged install does
# not touch; a run of ldconfig there would also give a misleading result. If
# either command fails, the install still succeeds.
REFRESH_SYSTEM_CACHES = \
	if [ -z "$(strip $(DESTDIR))" ]; then \
		$(SUDO) $(CCOL_LDCONFIG) || true; \
		$(SUDO) $(CCOL_MAN_INDEX) || true; \
	fi

# A reduced build has the SONAME of the full build but exports fewer symbols,
# so an application that you linked against a full build fails to start after
# you install a reduced build over it. Nothing in the installed files says
# which kind of build they came from, and the failure also appears in an
# unrelated program at its next start, not here, so you must ask for the
# reduced case. ALLOW_REDUCED_INSTALL=1 is that request: it serves the
# embedding case that a reduced build exists for, and a full build does not
# need it.
ALLOW_REDUCED_INSTALL ?= 0

.PHONY: _install_reduced_guard
_install_reduced_guard:
ifneq ($(strip $(DISABLED_SOURCES)),)
ifneq ($(ALLOW_REDUCED_INSTALL),1)
	@echo "install: refused, this is a reduced build (modules disabled:$(patsubst $(SOURCE_DIR)/%.c, %,$(DISABLED_SOURCES)))." >&2
	@echo "         It exports fewer symbols than the full library while carrying the" >&2
	@echo "         same SONAME ($(SHARED_LIBRARY_SONAME)), so installing it where a full" >&2
	@echo "         build is expected stops every application already linked against" >&2
	@echo "         that SONAME from starting." >&2
	@echo "" >&2
	@echo "         Re-run with ALLOW_REDUCED_INSTALL=1 if this prefix is meant to hold" >&2
	@echo "         a reduced build, and prefer a PREFIX or DESTDIR of its own over a" >&2
	@echo "         shared system path." >&2
	@false
endif
endif
	@:

install: _install_path_guard _install_reduced_guard $(SHARED_LIBRARY_NAME) \
         $(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE)
	$(SUDO) install -d $(HEADER_INSTALL_DIR) $(LIBRARY_INSTALL_DIR) $(PKGCONFIG_INSTALL_DIR)
	$(SUDO) install -m 644 $(PUBLIC_HEADER_FILES) $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 755 $(SHARED_LIBRARY_REAL) $(LIBRARY_INSTALL_DIR)
	$(SUDO) ln -sf $(SHARED_LIBRARY_REAL) $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_SONAME)
	$(SUDO) ln -sf $(SHARED_LIBRARY_SONAME) $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_NAME)
	$(SUDO) install -m 644 $(STATIC_LIBRARY_NAME) $(LIBRARY_INSTALL_DIR)
	$(SUDO) install -m 644 $(PKGCONFIG_FILE) $(PKGCONFIG_INSTALL_DIR)
	$(SUDO) install -d $(MAN_INSTALL_DIR)/man3 $(MAN_INSTALL_DIR)/man7
	if [ -n "$(MAN3_SRC_FILES)" ]; then \
		tmp=$$(mktemp -d) || exit 1; \
		for f in $(MAN3_SRC_FILES); do \
			sed -E 's#^\.so [A-Za-z0-9_]+/#.so man3/#' "$$f" > "$$tmp/$${f##*/}" || { rm -rf "$$tmp"; exit 1; }; \
		done; \
		$(SUDO) install -m 644 "$$tmp"/*.3 $(MAN_INSTALL_DIR)/man3; rc=$$?; rm -rf "$$tmp"; exit $$rc; \
	fi
	if [ -n "$(MAN7_SRC_FILES)" ]; then $(SUDO) install -m 644 $(MAN7_SRC_FILES) $(MAN_INSTALL_DIR)/man7; fi
	@$(REFRESH_SYSTEM_CACHES)

# The header directory belongs to this library alone. The uninstall removes it
# once it is empty, and leaves it, with a message from rmdir, when it still
# holds a file that this install did not put there.
uninstall: _install_path_guard
	set -f; $(SUDO) rm -f $(addprefix $(HEADER_INSTALL_DIR)/,$(notdir $(ALL_PUBLIC_HEADER_FILES)))
	if [ -d $(HEADER_INSTALL_DIR) ]; then $(SUDO) rmdir $(HEADER_INSTALL_DIR) || true; fi
	set -f; $(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_NAME)
	set -f; $(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_SONAME)
	set -f; $(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_REAL)
	set -f; $(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(STATIC_LIBRARY_NAME)
	set -f; $(SUDO) rm -f $(PKGCONFIG_INSTALL_DIR)/$(PKGCONFIG_FILE)
	set -f; $(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man3/,$(notdir $(ALL_MAN3_SRC_FILES)))
	set -f; $(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man7/,$(notdir $(ALL_MAN7_SRC_FILES)))
	@$(REFRESH_SYSTEM_CACHES)
