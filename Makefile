CC = gcc
AR = ar
RANLIB = ranlib

SHORT_LIBRARY_NAME = ccollections

# Release version, and the ABI version encoded in the shared library's SONAME.
#
# VERSION names the release. ABI_VERSION names the binary interface, and is
# what a linked application actually records a dependency on: an application
# built against this library gets DT_NEEDED libccollections.so.$(ABI_VERSION),
# so the dynamic loader will only ever satisfy it with a library promising
# that same interface. The two are deliberately separate numbers, because a
# release that only adds symbols or fixes behavior keeps the same ABI while
# VERSION moves on.
#
# ABI_VERSION is the major version, and a change that breaks the interface
# requires incrementing it. The VERSION_MAJOR = 0 branch below covers a
# pre-1.0 major, where an interface is still settling and every minor release
# counts as an ABI of its own.
VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
ifeq ($(VERSION_MAJOR),0)
ABI_VERSION = $(VERSION_MAJOR).$(VERSION_MINOR)
else
ABI_VERSION = $(VERSION_MAJOR)
endif

# The real file carries the full version; the SONAME and the bare development
# link are symlinks onto it, which is the usual three-name ELF layout:
#   libccollections.so             -> what -lccollections resolves at link time
#   libccollections.so.1           -> the SONAME, what a built binary records
#   libccollections.so.1.0.0       -> the actual file
SHARED_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).so
SHARED_LIBRARY_SONAME = $(SHARED_LIBRARY_NAME).$(ABI_VERSION)
SHARED_LIBRARY_REAL = $(SHARED_LIBRARY_NAME).$(VERSION)
STATIC_LIBRARY_NAME = lib$(SHORT_LIBRARY_NAME).a
PKGCONFIG_FILE = $(SHORT_LIBRARY_NAME).pc

# ---------------------------------------------------------------------------
# Optional modules
# ---------------------------------------------------------------------------
# Every module is built by default. Set any of these to 0, here or on the
# command line, to leave that module out of the library entirely:
#
#   make WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0
#
# Modules are selected by choosing which sources to compile rather than by
# wrapping their bodies in #ifdef. The four optional modules are leaves (no
# other module includes their headers), so leaving one out needs no conditional
# compilation anywhere, and the sources stay free of build-configuration
# clutter. A disabled module's public header is not installed either, so
# including it fails at compile time with a missing file rather than at link
# time with undefined symbols.
#
# What this is actually for is dropping dependencies. OpenSSL enters the build
# through exactly one file (src/ctls.c) which only the two HTTP modules use, so
# turning both off removes libssl and libcrypto completely. zlib enters through
# clogger alone. With all five off, the library needs nothing beyond pthread and
# libm.
#
# A reduced build is NOT ABI-interchangeable with a full one. It carries the
# same SONAME while exporting fewer symbols, so an application linked against a
# full build fails to start against a reduced one. Reduced builds are for
# embedding a library you build yourself, not for distributing something another
# program might mistake for the complete one. `make check_abi` recognises a
# reduced build and skips, since the committed baseline describes the full
# library.
WITH_CJSON ?= 1
WITH_CYAML ?= 1
WITH_CLOGGER ?= 1
WITH_CHTTPCLIENT ?= 1
WITH_CHTTPSERVER ?= 1

# src/ctls.c and src/chttp1_parser.c are internal and used only by the two HTTP
# modules, so they follow rather than carry a switch of their own.
# Each half is tested the same way every other switch is tested, against 1
# rather than against 0: any other spelling means off, and matching the two
# concatenated against a single literal would read "no" as "on" for one of them
# and leave a half-configured build that still compiles ctls and chttp1_parser,
# still links OpenSSL, and still runs their suites.
WITH_HTTP := 0
ifeq ($(WITH_CHTTPCLIENT),1)
WITH_HTTP := 1
endif
ifeq ($(WITH_CHTTPSERVER),1)
WITH_HTTP := 1
endif

# chttpclient and chttpserver both log through clogger, so it cannot be dropped
# while either of them is present. Forcing it rather than failing keeps
# `make WITH_CLOGGER=0` from being a build error for anyone who simply wanted
# less and did not know the dependency.
ifeq ($(WITH_HTTP),1)
ifneq ($(WITH_CLOGGER),1)
$(warning WITH_CLOGGER=0 ignored: chttpclient/chttpserver depend on clogger)
override WITH_CLOGGER := 1
endif
endif

SOURCE_DIR = src
INCLUDE_DIR = include
OBJECT_DIR = obj
# chttpclient's HTTP/1.1 response parser (src/chttp1_parser.c) and the rest of
# the c_collections-native HTTP stack (ctls, ccol_event_loop, chttpserver,
# chttpclient) are first-party modules, needing no vendor build rules at all:
# they are picked up automatically by SOURCE_FILES' wildcard over
# $(SOURCE_DIR)/*.c below, exactly like any other module in this library.
# The library vendors no third-party source: every .c it builds is its own.
# A disabled module's own test directory is skipped, along with tests/mixed/
# whenever anything is disabled: that suite links across modules and cannot
# build against a partial library. tests/ctls/ and tests/chttp/ follow the HTTP
# switches, since they exercise code those modules bring in.
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
# ctls only: src/ctls.c goes with the HTTP modules, so its suite has nothing
# left to compile. tests/chttp stays, because src/chttp.c (the shared types,
# base64 and RFC 7617 helpers) ships in every configuration and that suite
# needs nothing else; dropping it would leave a shipped file with no suite,
# which the per-file coverage gate reports as not compiled by any suite.
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
# -Wl,-z,relro,-z,now is a *linker* flag (the -Wl, prefix passes it straight
# through to ld), not a compiler flag: it belongs in SHARED_LDFLAGS, applied
# at the actual link step that produces libccollections.so, not here in
# COMMON_CFLAGS, which this Makefile only ever uses for the separate,
# link-less `$(CC) -c ...` object-compile steps below. Put here instead, it
# is silently discarded before reaching any real link invocation, providing
# no RELRO/BIND_NOW hardening at all, and under clang it breaks the build
# outright: clang's driver treats an unused linker argument passed to a
# compile-only invocation as an error under -Werror, where gcc tolerates it
# silently. See SHARED_LDFLAGS below for where it takes effect. A static
# archive (STATIC_LDFLAGS, built via `ar`, not a real linker invocation at
# all) has no equivalent: RELRO/BIND_NOW are properties of a real ELF
# executable/shared object, and applying them is the responsibility of
# whatever a caller of libccollections.a itself ultimately links into.
# -D_FILE_OFFSET_BITS=64: on a 32-bit (ILP32) target, glibc's readdir() must
# narrow the kernel's 64-bit d_ino into the caller's own ino_t; without this
# flag that ino_t is only 32 bits wide, so readdir() fails with EOVERFLOW
# the moment a directory contains an entry whose real inode number does not
# fit (routine on a modern 64-bit-inode filesystem, not a corrupt or
# adversarial input). This flag makes ino_t (and off_t, stat, etc.) 64 bits
# wide on every target, a no-op on a 64-bit build where they already are.
# -fvisibility=hidden makes every function and object internal to the library
# by default; only the declarations inside the `#pragma GCC visibility
# push(default)` blocks of the installed public headers are exported. This
# keeps the dynamic symbol table equal to the documented public API, so an
# internal helper never silently becomes part of the ABI, and an application
# symbol of the same name can never interpose one of the library's own
# internal calls. It also removes a real indirection: a call to an exported
# function from elsewhere inside the library is preemptible and therefore
# routed through the PLT, while a hidden one is called directly.
COMMON_CFLAGS = -I$(INCLUDE_DIR) \
	-fvisibility=hidden \
	-fstack-protector-strong \
	-fstack-clash-protection \
	-D_FORTIFY_SOURCE=3 \
	-D_FILE_OFFSET_BITS=64 \
	-Wstrict-overflow -Wformat=2 -Wformat-security -Wall -Wextra \
	-g -O3 -Werror -fPIC $(EXTRA_CFLAGS)

# Separate cflags for shared and static builds
SHARED_CFLAGS = $(COMMON_CFLAGS)
STATIC_CFLAGS = $(COMMON_CFLAGS)

# Separate ldflags for shared and static builds
# Only the libraries the enabled modules actually need. zlib arrives with
# clogger and OpenSSL with the HTTP modules, so a build without them links
# neither, and `pkg-config --libs --static ccollections` stops naming them too.
EXTERNAL_LIBS = -lpthread -lm
PC_REQUIRES_PRIVATE =
ifeq ($(WITH_CLOGGER),1)
EXTERNAL_LIBS += -lz
PC_REQUIRES_PRIVATE += zlib
endif
ifeq ($(WITH_HTTP),1)
EXTERNAL_LIBS += -lssl -lcrypto
PC_REQUIRES_PRIVATE += libssl libcrypto
endif

SHARED_LDFLAGS = -Wl,-z,relro,-z,now -Wl,-soname,$(SHARED_LIBRARY_SONAME) -shared $(EXTERNAL_LIBS)
STATIC_LDFLAGS =

# cdebuglog.c (an opt-in, RUNNING_UNIT_TESTS-only diagnostic log buffer for
# chasing hard-to-reproduce CI timing/hang issues; see its own doc comment
# in include/cdebuglog.h) is deliberately excluded here: it is not part of
# the shipped library, even as the empty translation unit it would compile
# to in a production (non-RUNNING_UNIT_TESTS) build. A test suite that wants
# it adds src/cdebuglog.c to its own Makefile's SRC_FILES explicitly.
# Sources left out by the WITH_* switches above, alongside cdebuglog.c which is
# never part of the shipped library.
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
HEADER_FILES = $(filter-out $(DISABLED_HEADERS),$(wildcard $(INCLUDE_DIR)/*.h))
OBJ_FILES_SHARED = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.o)
OBJ_FILES_STATIC = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.static.o)

default: all

test:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && make test) &&) true

memtest:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && make memtest) &&) true

# Each module runs in its own subshell, and the list is chained with && so the
# first failure stops the sweep and propagates. A `cd $(folder) && ... && cd -`
# chain joined by `;` instead would leave the shell parked in the failing
# module's directory, so every following module's own relative cd fails too and
# sixteen misleading "can't cd" lines bury the one real error.
generate_coverage_report:
	$(foreach folder,$(COVERAGE_TEST_FOLDERS),(cd $(folder) && make generate_coverage_report) &&) true

# ---------------------------------------------------------------------------
# Aggregate coverage site
# ---------------------------------------------------------------------------
# Each module's own generate_coverage_report leaves its lcov tracefiles in
# tests/<module>/coverage/. This target merges every one of them into a single
# report covering the library as a whole. Merging is what makes the numbers
# meaningful: a file such as src/common.c is compiled into most of the module
# test binaries, and only the union of their runs describes how much of it the
# suite actually reaches.
#
# A module may leave more than one tracefile behind, and every one of them is
# picked up: a suite that builds several binaries from one directory emits one
# per binary.
#
# The merged data is then narrowed to src/ and include/ alone. Coverage of the
# test code itself, of the vendored test framework, and of system headers says
# nothing about how well the library is tested, and leaving it in inflates
# every headline number. The extract patterns are anchored to $(CURDIR) rather
# than written as '*/include/*', which also matches /usr/include/... and pulls
# six glibc and OpenSSL headers into the report, one of them at 0.0 percent.
# Fails the build if anything in the public interface loses its namespace
# prefix. This cannot be checked by the test suites: they compile the .c files
# straight into their own binaries, where an unprefixed or unexported symbol
# links exactly like a correct one.
.PHONY: check_namespace
check_namespace: $(SHARED_LIBRARY_NAME)
	@./ci_scripts/check_public_namespace.sh $(SHARED_LIBRARY_REAL)

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

# Re-records the baseline from the current build. Run this when the public API
# gains a symbol, and commit the result with it.
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
# bench/ links against the shared library this build produces rather than
# compiling the sources into its own binary, so what it measures is the code
# that ships, at the flags it ships with, reached through the same dynamic call
# an application makes. The baseline it compares against is per machine and is
# not committed: an absolute nanoseconds-per-operation figure describes one
# machine's cache hierarchy and background load, so comparing a run here
# against one recorded elsewhere reports a difference that has nothing to do
# with the library. Record a baseline with bench_update, then bench reports
# every later run against it.
#
# BENCH_ARGS passes options straight through, e.g.
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

# Fails if any file in src/ or include/ is covered by less than 80 percent of
# its instrumented lines. Separate from coverage_site so that the report can be
# regenerated and inspected without the check, and so the check can be re-run
# against an existing report without paying for the instrumented rebuild.
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

# An object file records nothing about the compiler or the flags that produced
# it, so an ordinary prerequisite list lets a build with different ones relink
# objects compiled for the previous build: a sanitizer that is half applied, a
# CC= change that leaves objects of the wrong architecture, or a switch such as
# CCOL_MEMPOOL_COMPACT_LAYOUT that appears to have been ignored because the
# object holding it was never recompiled. The stamp carries the current compiler
# and flags and is rewritten only when they actually differ, so nothing rebuilds
# needlessly and everything rebuilds when it must. FORCE rather than a file
# prerequisite because these values arrive on the command line, with no input
# file changing alongside them.
BUILD_FLAGS_STAMP = $(OBJECT_DIR)/.build_flags

# The link has its own identity, separate from the compile's, because the
# WITH_* switches change which objects go into the library and which libraries
# it is linked against without changing how any object is compiled. Every
# object a reduced build needs is therefore already present and up to date
# after a full one, so the library is not relinked and keeps the modules and
# the NEEDED entries of the build before it: `make WITH_CHTTPSERVER=0
# WITH_CHTTPCLIENT=0` after an ordinary build produces a library that still
# exports both modules and still links OpenSSL, with nothing in the output to
# say so. A clean tree hides this completely, which is why the CI job that
# checks a reduced build's dependencies cannot catch it.
#
# Kept as a second stamp rather than folded into the one above so that
# toggling a module does not also force every object to be recompiled for a
# compile that has not changed.
LINK_FLAGS_STAMP = $(OBJECT_DIR)/.link_flags

$(BUILD_FLAGS_STAMP): FORCE
	@mkdir -p $(OBJECT_DIR)
	@printf '%s\n' '$(CC)|$(SHARED_CFLAGS)|$(STATIC_CFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(LINK_FLAGS_STAMP): FORCE
	@mkdir -p $(OBJECT_DIR)
	@printf '%s\n' '$(CC)|$(AR)|$(SHARED_LDFLAGS)|$(STATIC_LDFLAGS)|$(OBJ_FILES_SHARED)|$(OBJ_FILES_STATIC)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

all: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE)

$(SHARED_LIBRARY_REAL): $(OBJ_FILES_SHARED) $(LINK_FLAGS_STAMP)
	$(CC) -o $(SHARED_LIBRARY_REAL) $(OBJ_FILES_SHARED) $(SHARED_LDFLAGS)

$(SHARED_LIBRARY_SONAME): $(SHARED_LIBRARY_REAL)
	ln -sf $(SHARED_LIBRARY_REAL) $(SHARED_LIBRARY_SONAME)

$(SHARED_LIBRARY_NAME): $(SHARED_LIBRARY_SONAME)
	ln -sf $(SHARED_LIBRARY_SONAME) $(SHARED_LIBRARY_NAME)

# The pkg-config metadata is generated rather than committed so that the
# version and the install prefix can never drift from the ones this build
# actually used. FORCE is what makes that true: PREFIX arrives on the command
# line, so it can change with no input file changing with it, and an ordinary
# prerequisite list would leave a stale prefix baked into the installed file.
# The file is only rewritten when the rendered text actually differs, so
# regenerating every time still does not make anything downstream rebuild.
.PHONY: FORCE
FORCE:

$(PKGCONFIG_FILE): $(PKGCONFIG_FILE).in FORCE
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     -e 's|@REQUIRES_PRIVATE@|$(strip $(PC_REQUIRES_PRIVATE))|g' \
	     $(PKGCONFIG_FILE).in > $(PKGCONFIG_FILE).tmp
	@if cmp -s $(PKGCONFIG_FILE).tmp $(PKGCONFIG_FILE); then \
		rm -f $(PKGCONFIG_FILE).tmp; \
	else \
		mv $(PKGCONFIG_FILE).tmp $(PKGCONFIG_FILE); \
		echo "generated $(PKGCONFIG_FILE) (prefix=$(PREFIX) version=$(VERSION))"; \
	fi

# `ar rcs` adds to an existing archive rather than replacing it, so a reduced
# build over a full one would leave the dropped modules' members in place even
# once the archive is rebuilt. Removing it first is what makes the member list
# follow $(OBJ_FILES_STATIC) exactly.
$(STATIC_LIBRARY_NAME): $(OBJ_FILES_STATIC) $(LINK_FLAGS_STAMP)
	@rm -f $(STATIC_LIBRARY_NAME)
	$(AR) rcs $(STATIC_LIBRARY_NAME) $(OBJ_FILES_STATIC) $(STATIC_LDFLAGS)
	$(RANLIB) $(STATIC_LIBRARY_NAME)

# Objects for shared library (with -fPIC)
$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES) $(BUILD_FLAGS_STAMP)
	$(CC) -c $(SHARED_CFLAGS) $< -o $@

# Objects for the static library. Still built with -fPIC so the archive can be
# linked into a shared library by a consumer. Note that an archive is not a
# linked object and records no dependency of its own on pthread, zlib, OpenSSL
# or libm: everything the library needs must be named on the consumer's own
# link line. $(PKGCONFIG_FILE) carries that list so `pkg-config --libs
# --static ccollections` produces it automatically.
$(OBJECT_DIR)/%.static.o: $(SOURCE_DIR)/%.c $(HEADER_FILES) $(BUILD_FLAGS_STAMP)
	$(CC) -c $(STATIC_CFLAGS) $< -o $@

# The shared library is removed by glob rather than by its three current
# names, so that a build tree carrying artifacts from a different VERSION is
# cleaned out as well. Naming only the current version leaves a stale
# libccollections.so.<other> sitting next to the new one, where it can still
# satisfy an older binary's own DT_NEEDED at run time.
clean:
	rm -rf $(SHARED_LIBRARY_NAME) $(SHARED_LIBRARY_NAME).* \
		$(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE) $(PKGCONFIG_FILE).tmp $(OBJECT_DIR) \
		tests/*/tests tests/*/tests_tls tests/*/tests_mem_mgmt \
		tests/*/tests_parser tests/*/tests_default_client \
		tests/*/tests_engine_stop tests/*/tests_engine_stop_tsan \
		tests/*/tests_spec_suite tests/*/tests_differential \
		tests/*/*.o tests/*/.build-flags \
		bench/bench bench/*.o bench/.build-flags \
		tests/*/tests_tsan tests/*/fuzz_* \
		tests/*/coverage tests/*/third_party_obj $(COVERAGE_SITE_DIR) \
		tests/*/*.gcno tests/*/*.gcda tests/*/*.gcov tests/*/*.c.info

# PREFIX relocates an entire install in one step (a distribution package, a
# per-user install into $HOME/.local, a staged install into a container image).
# DESTDIR prepends a staging directory to every install path without appearing
# anywhere in the installed files' own contents, which is what a package build
# needs: it must never leak into $(PKGCONFIG_FILE)'s recorded prefix.
PREFIX ?= /usr/local
DESTDIR ?=
HEADER_INSTALL_DIR = $(DESTDIR)$(PREFIX)/include
LIBRARY_INSTALL_DIR = $(DESTDIR)$(PREFIX)/lib
MAN_INSTALL_DIR = $(DESTDIR)$(PREFIX)/share/man
PKGCONFIG_INSTALL_DIR = $(DESTDIR)$(PREFIX)/lib/pkgconfig

# chashkey.h, chttp1_parser.h, ctls.h, cdebuglog.h and cpintable.h are internal
# to the library: no public header includes them, and the symbols they declare
# are not exported from the shared library, so an installed copy could not be
# linked against. They stay in the source tree and out of the install.
INTERNAL_HEADER_FILES = $(INCLUDE_DIR)/chashkey.h \
                        $(INCLUDE_DIR)/chttp1_parser.h \
                        $(INCLUDE_DIR)/ctls.h \
                        $(INCLUDE_DIR)/cdebuglog.h \
                        $(INCLUDE_DIR)/cpintable.h
PUBLIC_HEADER_FILES = $(filter-out $(INTERNAL_HEADER_FILES),$(HEADER_FILES))
# Every header this project would ever install, whatever the WITH_* switches
# say. uninstall reads this rather than the switch-filtered list above: an
# uninstall run with different switches from the install that placed the files
# would otherwise walk past the headers it does not currently build and leave
# them behind, declaring symbols the library no longer has.
ALL_PUBLIC_HEADER_FILES = $(filter-out $(INTERNAL_HEADER_FILES),\
                            $(wildcard $(INCLUDE_DIR)/*.h))

# man/<module>/*.3 (functions and their companion type-safe macros, side by
# side, see man/README) install flat into one man3 dir; real symbol names
# never collide across the two, so nothing is lost by flattening. man/<module>/*.7
# are the module overview pages. Alias pages contain a ".so <module>/<symbol>.3"
# redirect that is relative to the source tree layout, so it is rewritten to
# ".so man3/<symbol>.3" (relative to the installed MANPATH root) as part of install.
ALL_MAN3_SRC_FILES = $(wildcard man/*/*.3)
ALL_MAN7_SRC_FILES = $(wildcard man/*/*.7)
# A disabled module's pages follow its header out of the install: they document
# functions that are neither declared in an installed header nor present in the
# installed library, which is the same reason the header itself is dropped.
# uninstall uses the unfiltered lists above, for the reason given there.
DISABLED_MAN_DIRS = $(patsubst $(SOURCE_DIR)/%.c,man/%/,$(DISABLED_SOURCES))
DISABLED_MAN_FILES = $(foreach d,$(DISABLED_MAN_DIRS),$(wildcard $(d)*))
MAN3_SRC_FILES = $(filter-out $(DISABLED_MAN_FILES),$(ALL_MAN3_SRC_FILES))
MAN7_SRC_FILES = $(filter-out $(DISABLED_MAN_FILES),$(ALL_MAN7_SRC_FILES))

# Escalate only when the install actually needs it. Installing into a prefix
# the current user already owns ($HOME/.local, a DESTDIR staging tree used by
# a package build or a CI job) must not prompt for a password. The nearest
# existing ancestor of the install root is the thing to test for writability,
# since the leaf directories are created by the install itself. Override
# explicitly with `make install SUDO=` or `make install SUDO=doas`.
# Guarded by origin rather than ?=, so that both `make install SUDO=doas` and an
# environment SUDO still win while the probe below runs exactly once. A plain
# `?=` would re-run the whole shell on each of the fifteen or so expansions one
# install performs, and a plain `:=` would silently ignore the environment form.
#
# The walk stops at a component with no separator left in it, because `$${d%/*}`
# is a no-op once there is no slash to strip and a relative DESTDIR or PREFIX
# (`make install DESTDIR=stage PREFIX=/usr`, an ordinary packaging invocation)
# would otherwise spin forever. Which fallback that ends at is not the same for
# both shapes: stripping the last component of a single-component ABSOLUTE path
# leaves nothing, and the nearest existing ancestor there is /, not the working
# directory, so `make install PREFIX=/newroot` has to see an unwritable / and
# escalate rather than testing a directory the caller happens to own.
ifeq ($(origin SUDO),undefined)
SUDO := $(shell d="$(DESTDIR)$(PREFIX)"; \
                root=.; \
                if [ "$${d#/}" != "$$d" ]; then root=/; fi; \
                while [ -n "$$d" ] && [ ! -e "$$d" ]; do \
                  nd=$${d%/*}; \
                  if [ "$$nd" = "$$d" ]; then nd=""; fi; \
                  d="$$nd"; \
                done; \
                [ -n "$$d" ] || d="$$root"; \
                if [ "$$(id -u)" -eq 0 ] || [ -w "$$d" ]; then echo ""; else echo "sudo"; fi)
endif

# ldconfig refreshes the dynamic linker's cache and mandb the man index. Both
# act on the live system, so both are skipped (ldconfig actively misleadingly
# so) when the install is being staged into a DESTDIR for packaging. Neither
# failing is a reason to fail the install.
REFRESH_SYSTEM_CACHES = \
	if [ -z "$(strip $(DESTDIR))" ]; then \
		$(SUDO) ldconfig || true; \
		$(SUDO) mandb -q || true; \
	fi

install: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE)
	$(SUDO) install -d $(HEADER_INSTALL_DIR) $(LIBRARY_INSTALL_DIR) $(PKGCONFIG_INSTALL_DIR)
	$(SUDO) install -m 644 $(PUBLIC_HEADER_FILES) $(HEADER_INSTALL_DIR)
	$(SUDO) install -m 755 $(SHARED_LIBRARY_REAL) $(LIBRARY_INSTALL_DIR)
	$(SUDO) ln -sf $(SHARED_LIBRARY_REAL) $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_SONAME)
	$(SUDO) ln -sf $(SHARED_LIBRARY_SONAME) $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_NAME)
	$(SUDO) install -m 644 $(STATIC_LIBRARY_NAME) $(LIBRARY_INSTALL_DIR)
	$(SUDO) install -m 644 $(PKGCONFIG_FILE) $(PKGCONFIG_INSTALL_DIR)
	$(SUDO) install -d $(MAN_INSTALL_DIR)/man3 $(MAN_INSTALL_DIR)/man7
	if [ -n "$(MAN3_SRC_FILES)" ]; then $(SUDO) install -m 644 $(MAN3_SRC_FILES) $(MAN_INSTALL_DIR)/man3; fi
	if [ -n "$(MAN7_SRC_FILES)" ]; then $(SUDO) install -m 644 $(MAN7_SRC_FILES) $(MAN_INSTALL_DIR)/man7; fi
	$(SUDO) sed -i -E 's#^\.so [A-Za-z0-9_]+/#.so man3/#' $(addprefix $(MAN_INSTALL_DIR)/man3/,$(notdir $(MAN3_SRC_FILES)))
	@$(REFRESH_SYSTEM_CACHES)

uninstall:
	$(SUDO) rm -f $(addprefix $(HEADER_INSTALL_DIR)/,$(notdir $(ALL_PUBLIC_HEADER_FILES)))
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_NAME)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_SONAME)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_REAL)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(STATIC_LIBRARY_NAME)
	$(SUDO) rm -f $(PKGCONFIG_INSTALL_DIR)/$(PKGCONFIG_FILE)
	$(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man3/,$(notdir $(ALL_MAN3_SRC_FILES)))
	$(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man7/,$(notdir $(ALL_MAN7_SRC_FILES)))
	@$(REFRESH_SYSTEM_CACHES)
