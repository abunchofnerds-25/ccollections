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

SOURCE_DIR = src
INCLUDE_DIR = include
OBJECT_DIR = obj
# chttpclient's HTTP/1.1 response parser (src/chttp1_parser.c) and the rest of
# the c_collections-native HTTP stack (ctls, ccol_event_loop, chttpserver,
# chttpclient) are first-party modules, needing no vendor build rules at all:
# they are picked up automatically by SOURCE_FILES' wildcard over
# $(SOURCE_DIR)/*.c below, exactly like any other module in this library.
# The library vendors no third-party source: every .c it builds is its own.
TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -v /tau/)
COVERAGE_TEST_FOLDERS = $(shell ls -1d tests/*/ | grep -vE '/tau/|/mixed/')

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
SHARED_LDFLAGS = -Wl,-z,relro,-z,now -Wl,-soname,$(SHARED_LIBRARY_SONAME) -shared -lpthread -lz -lssl -lcrypto -lm
STATIC_LDFLAGS =

# cdebuglog.c (an opt-in, RUNNING_UNIT_TESTS-only diagnostic log buffer for
# chasing hard-to-reproduce CI timing/hang issues; see its own doc comment
# in include/cdebuglog.h) is deliberately excluded here: it is not part of
# the shipped library, even as the empty translation unit it would compile
# to in a production (non-RUNNING_UNIT_TESTS) build. A test suite that wants
# it adds src/cdebuglog.c to its own Makefile's SRC_FILES explicitly.
SOURCE_FILES = $(filter-out $(SOURCE_DIR)/cdebuglog.c,$(wildcard $(SOURCE_DIR)/*.c))
HEADER_FILES = $(wildcard $(INCLUDE_DIR)/*.h)
OBJ_FILES_SHARED = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.o)
OBJ_FILES_STATIC = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.static.o)

default: all

test:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && make test) &&) true

memtest:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && make memtest) &&) true

generate_coverage_report:
	$(foreach folder,$(COVERAGE_TEST_FOLDERS),cd $(folder) && make generate_coverage_report && cd -;)

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
# The merged data is then narrowed to src/ and include/ alone. Coverage of the
# test code itself, of the vendored test framework, and of system headers says
# nothing about how well the library is tested, and leaving it in inflates
# every headline number.
# Fails the build if anything in the public interface loses its namespace
# prefix. This cannot be checked by the test suites: they compile the .c files
# straight into their own binaries, where an unprefixed or unexported symbol
# links exactly like a correct one.
.PHONY: check_namespace
check_namespace: $(SHARED_LIBRARY_NAME)
	@./check_public_namespace.sh $(SHARED_LIBRARY_REAL)

COVERAGE_SITE_DIR = coverage_site
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
		'*/$(SOURCE_DIR)/*' '*/$(INCLUDE_DIR)/*' $(LCOV_QUIRK_FLAGS) \
		--output-file $(COVERAGE_SITE_DIR)/library.info; \
	genhtml $(COVERAGE_SITE_DIR)/library.info $(LCOV_QUIRK_FLAGS) \
		--legend --show-details --title "c_collections $(VERSION)" \
		--output-directory $(COVERAGE_SITE_DIR)/html; \
	lcov --summary $(COVERAGE_SITE_DIR)/library.info \
		$(LCOV_QUIRK_FLAGS) 2>&1 | tee $(COVERAGE_SITE_DIR)/summary.txt

# view_coverage_report:
# 	firefox $(for i in $(ls -1 ./tests/ | grep -vE 'mixed|tau'); do s=$(basename $i); echo ./tests/$s/coverage/src/$s.c.gcov.html; done | xargs)

all: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE)

$(SHARED_LIBRARY_REAL): $(OBJ_FILES_SHARED)
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
	     $(PKGCONFIG_FILE).in > $(PKGCONFIG_FILE).tmp
	@if cmp -s $(PKGCONFIG_FILE).tmp $(PKGCONFIG_FILE); then \
		rm -f $(PKGCONFIG_FILE).tmp; \
	else \
		mv $(PKGCONFIG_FILE).tmp $(PKGCONFIG_FILE); \
		echo "generated $(PKGCONFIG_FILE) (prefix=$(PREFIX) version=$(VERSION))"; \
	fi

$(STATIC_LIBRARY_NAME): $(OBJ_FILES_STATIC)
	$(AR) rcs $(STATIC_LIBRARY_NAME) $(OBJ_FILES_STATIC) $(STATIC_LDFLAGS)
	$(RANLIB) $(STATIC_LIBRARY_NAME)

# Objects for shared library (with -fPIC)
$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
	$(CC) -c $(SHARED_CFLAGS) $< -o $@

# Objects for the static library. Still built with -fPIC so the archive can be
# linked into a shared library by a consumer. Note that an archive is not a
# linked object and records no dependency of its own on pthread, zlib, OpenSSL
# or libm: everything the library needs must be named on the consumer's own
# link line. $(PKGCONFIG_FILE) carries that list so `pkg-config --libs
# --static ccollections` produces it automatically.
$(OBJECT_DIR)/%.static.o: $(SOURCE_DIR)/%.c $(HEADER_FILES)
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
		tests/*/tests_tsan tests/*/fuzz_* \
		tests/*/coverage tests/*/third_party_obj \
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

# chttp1_parser.h, ctls.h and cdebuglog.h are internal to the library: no
# public header includes them, and the symbols they declare are not exported
# from the shared library, so an installed copy could not be linked against.
# They stay in the source tree and out of the install.
INTERNAL_HEADER_FILES = $(INCLUDE_DIR)/chttp1_parser.h \
                        $(INCLUDE_DIR)/ctls.h \
                        $(INCLUDE_DIR)/cdebuglog.h
PUBLIC_HEADER_FILES = $(filter-out $(INTERNAL_HEADER_FILES),$(HEADER_FILES))

# man/<module>/*.3 (functions and their companion type-safe macros, side by
# side, see man/README) install flat into one man3 dir; real symbol names
# never collide across the two, so nothing is lost by flattening. man/<module>/*.7
# are the module overview pages. Alias pages contain a ".so <module>/<symbol>.3"
# redirect that is relative to the source tree layout, so it is rewritten to
# ".so man3/<symbol>.3" (relative to the installed MANPATH root) as part of install.
MAN3_SRC_FILES = $(wildcard man/*/*.3)
MAN7_SRC_FILES = $(wildcard man/*/*.7)

# Escalate only when the install actually needs it. Installing into a prefix
# the current user already owns ($HOME/.local, a DESTDIR staging tree used by
# a package build or a CI job) must not prompt for a password. The nearest
# existing ancestor of the install root is the thing to test for writability,
# since the leaf directories are created by the install itself. Override
# explicitly with `make install SUDO=` or `make install SUDO=doas`.
SUDO ?= $(shell d="$(DESTDIR)$(PREFIX)"; \
                while [ -n "$$d" ] && [ ! -e "$$d" ]; do d=$${d%/*}; done; \
                [ -n "$$d" ] || d=/; \
                if [ "$$(id -u)" -eq 0 ] || [ -w "$$d" ]; then echo ""; else echo "sudo"; fi)

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
	$(SUDO) rm -f $(addprefix $(HEADER_INSTALL_DIR)/,$(notdir $(PUBLIC_HEADER_FILES)))
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_NAME)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_SONAME)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(SHARED_LIBRARY_REAL)
	$(SUDO) rm -f $(LIBRARY_INSTALL_DIR)/$(STATIC_LIBRARY_NAME)
	$(SUDO) rm -f $(PKGCONFIG_INSTALL_DIR)/$(PKGCONFIG_FILE)
	$(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man3/,$(notdir $(MAN3_SRC_FILES)))
	$(SUDO) rm -f $(addprefix $(MAN_INSTALL_DIR)/man7/,$(notdir $(MAN7_SRC_FILES)))
	@$(REFRESH_SYSTEM_CACHES)
