include platform.mk

# ---------------------------------------------------------------------------
# Saved build configuration
# ---------------------------------------------------------------------------
# Every build writes the settings that decide what it builds (the compiler,
# EXTRA_CFLAGS and the WITH_* module switches) to $(BUILD_CONFIG_FILE), and
# every later run of this Makefile reads them back, so a later command works
# on the library that the last build made without repeating any option:
#
#   make WITH_CJSON=0 EXTRA_CFLAGS=-DCCOL_MEMPOOL_COMPACT_LAYOUT=1
#   sudo make install ALLOW_REDUCED_INSTALL=1
#
# Without the file the install would see settings that differ from the stamps
# of the build (sudo drops the caller's variables), so it would compile and
# link the library again with the defaults and install something nobody asked
# for. A value on the command line or in the environment always wins over the
# saved one, and the build it starts saves the new value. `make clean` removes
# the file and the next build starts from the defaults.
#
# The file has one conditional block per setting, which assigns the saved
# value only while the setting has no value of its own: undefined, the value
# make predefines (CC is `cc`), or the default of platform.mk. Each value is
# stored as text exactly as the build received it, so a reference like
# $(PREFIX) reads back as it was written. The file has to be read after
# platform.mk, which picks the default compiler, and before anything below
# reads CC, EXTRA_CFLAGS or a WITH_* switch.
OBJECT_DIR = obj
BUILD_CONFIG_FILE = $(OBJECT_DIR)/.config.mk
BUILD_CONFIG_VARIABLES = CC EXTRA_CFLAGS WITH_CJSON WITH_CYAML WITH_CLOGGER \
	WITH_CHTTPCLIENT WITH_CHTTPSERVER
CCOL_DEFAULT_CC := $(value CC)
CCOL_RESTORED_VARIABLES :=
-include $(BUILD_CONFIG_FILE)

# A saved setting that differs from its default changes what this run builds,
# tests and installs, so we print it instead of applying it silently.
ccol_streq = $(if $(subst x$(1),,x$(2))$(subst x$(2),,x$(1)),,1)
ccol_config_default = $(if $(filter CC,$(1)),$(CCOL_DEFAULT_CC),$(if $(filter WITH_%,$(1)),1))
CCOL_RESTORED_CHANGES := $(strip $(foreach v,$(CCOL_RESTORED_VARIABLES),\
	$(if $(call ccol_streq,$(call ccol_config_default,$(v)),$(value $(v))),,$(v))))
ifneq ($(CCOL_RESTORED_CHANGES),)
$(info Saved build configuration ($(BUILD_CONFIG_FILE)), used by the library build, the install and the test suites: $(foreach v,$(CCOL_RESTORED_CHANGES),$(v)="$(value $(v))"); `make clean` resets it)
endif

AR = ar
RANLIB = ranlib

SHORT_LIBRARY_NAME = ccollections

# The release version, and the Application Binary Interface (ABI) version that
# the SONAME of the shared library holds.
#
# VERSION names the release and ABI_VERSION names the binary interface. An
# application depends on the ABI version and not on the release: when you
# build against this library it gets DT_NEEDED
# libccollections.so.$(ABI_VERSION), and the dynamic loader then accepts only
# a library that promises the same interface. Keeping the two numbers apart
# lets a release that only adds symbols or only corrects behavior bump
# VERSION while the ABI stays the same.
#
# ABI_VERSION is the major version and you have to increase it whenever a
# change breaks the interface. The VERSION_MAJOR = 0 branch below covers the
# time before 1.0, when the interface isn't stable yet, so each minor release
# counts as its own ABI.
VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
ifeq ($(VERSION_MAJOR),0)
ABI_VERSION = $(VERSION_MAJOR).$(VERSION_MINOR)
else
ABI_VERSION = $(VERSION_MAJOR)
endif

# The real file carries the full version in its name, and the SONAME and the
# plain development link are symlinks to it, which is the usual ELF layout
# with three names:
#   libccollections.so             -> what -lccollections resolves at link time
#   libccollections.so.1           -> the SONAME, what a built binary records
#   libccollections.so.1.0.0       -> the actual file
#
# macOS puts the version before the suffix, and the library's install name
# plays the part of the SONAME:
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
# Every module is in the build by default. Set any of these to 0, here or on
# the command line, to leave that module out of the library:
#
#   make WITH_CHTTPCLIENT=0 WITH_CHTTPSERVER=0
#
# A module is selected by the sources the build compiles, and its body is
# never wrapped in #ifdef. No other module includes the headers of the four
# optional modules, so each one is a leaf, and that's why leaving one out
# needs no conditional compilation and the sources carry no build
# configuration clutter. The public header of a module you turn off isn't
# installed either, so an application that includes it fails at compile time
# with a missing file instead of at link time with undefined symbols.
#
# These switches exist to drop dependencies. OpenSSL enters the build through
# one file only (src/ctls.c) and only the two HTTP modules use it, so with
# both of them off the build needs neither libssl nor libcrypto. zlib comes in
# through clogger alone. With all five modules off the library needs only
# pthread and libm.
#
# A reduced build is NOT interchangeable with a full one at the ABI level. It
# keeps the same SONAME but exports fewer symbols, so an application linked
# against a full build fails to start against a reduced one. Use a reduced
# build for a library you build and embed yourself and don't distribute it,
# because another program can take it for the complete library. `make
# check_abi` detects a reduced build and skips, since the committed baseline
# describes the full library.
WITH_CJSON ?= 1
WITH_CYAML ?= 1
WITH_CLOGGER ?= 1
WITH_CHTTPCLIENT ?= 1
WITH_CHTTPSERVER ?= 1

# src/ctls.c and src/chttp1_parser.c are internal and only the two HTTP
# modules use them, so they follow those modules and have no switch of their
# own. Each half is tested against 1, like every other switch and not against
# 0, so any other spelling means off. Don't join the two values and match them
# against one literal, because that form reads "no" as "on" for one of them
# and gives a half-configured build that compiles ctls and chttp1_parser,
# links OpenSSL and runs their suites anyway.
WITH_HTTP := 0
ifeq ($(WITH_CHTTPCLIENT),1)
WITH_HTTP := 1
endif
ifeq ($(WITH_CHTTPSERVER),1)
WITH_HTTP := 1
endif

# chttpclient and chttpserver both log through clogger, so you can't drop
# clogger while either of them is in the build. Instead of stopping with an
# error we turn clogger back on, so `make WITH_CLOGGER=0` doesn't fail for
# someone who wants a smaller library and doesn't know about this dependency.
ifeq ($(WITH_HTTP),1)
ifneq ($(WITH_CLOGGER),1)
$(warning WITH_CLOGGER=0 ignored: chttpclient/chttpserver depend on clogger)
override WITH_CLOGGER := 1
endif
endif

SOURCE_DIR = src
INCLUDE_DIR = include
# The HTTP/1.1 response parser of chttpclient (src/chttp1_parser.c) and the
# rest of the HTTP stack (ctls, ccol_event_loop, chttpserver, chttpclient) are
# all our own code, so they need no third-party build rules: the SOURCE_FILES
# wildcard over $(SOURCE_DIR)/*.c below finds them like any other module.
# The library holds no third-party source and builds only its own .c files.
# The test directory of a module you turn off is skipped, and so is
# tests/mixed/ when anything is off, because that suite links across modules
# and can't build against a partial library. tests/ctls/ and tests/chttp/
# follow the HTTP switches since they exercise the code those modules bring
# in.
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
# This one is for ctls only: src/ctls.c goes out with the HTTP modules, so
# its suite has nothing left to compile. tests/chttp stays because src/chttp.c
# (the shared types, the base64 helpers and the RFC 7617 helpers) ships in
# every configuration and that suite needs nothing else. Dropping it would
# leave a shipped file with no suite, and the per-file coverage gate would
# then report that file as not compiled by any suite.
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
# -Wl,-z,relro,-z,now is a *linker* flag (the -Wl, prefix hands it straight
# to ld), so it belongs in SHARED_LDFLAGS, which applies at the link step that
# makes libccollections.so. It doesn't belong here in COMMON_CFLAGS, which
# this Makefile uses only for the `$(CC) -c ...` steps below that compile an
# object without linking. Put here, the flag is gone before any real link and
# gives no RELRO or BIND_NOW hardening at all, and under clang it breaks the
# build too, because the clang driver treats an unused linker argument on a
# compile-only command as an error under -Werror while gcc accepts it
# silently. See SHARED_LDFLAGS below, where the flag takes effect. A static
# archive has no equivalent since `ar` receives STATIC_LDFLAGS and isn't a
# linker. RELRO and BIND_NOW are properties of a real ELF executable or shared
# object, so whatever links libccollections.a into a program has to apply
# them.
# -D_FILE_OFFSET_BITS=64 is for a 32-bit (ILP32) target, where glibc's
# readdir() has to narrow the kernel's 64-bit d_ino into the caller's ino_t.
# Without the flag that ino_t is only 32 bits wide, so readdir() fails with
# EOVERFLOW as soon as a directory holds an entry whose real inode number
# doesn't fit. That's normal on a modern filesystem with 64-bit inodes, not a
# corrupt input or an attack. The flag makes ino_t, off_t, stat and the
# related types 64 bits wide on every target and changes nothing on a 64-bit
# build, where they are 64 bits wide anyway.
# -fvisibility=hidden makes every function and object internal to the library
# by default, and only the declarations inside the `#pragma GCC visibility
# push(default)` blocks of the installed public headers get exported. That
# keeps the dynamic symbol table equal to the documented public API, so an
# internal helper can't slip into the ABI unnoticed and an application symbol
# of the same name can never interpose one of our internal calls. It also
# removes an indirection: a call from inside the library to an exported
# function can be interposed and so goes through the PLT, while a call to a
# hidden function is direct.
# -std=gnu11 pins the language mode of the public headers, which is C11 plus
# the GNU extensions the macro layer needs (typeof, statement expressions and
# __auto_type). Without it the build takes the compiler's default mode, and
# those defaults change over time: a toolchain that defaults to gnu23 changes
# the meaning of `bool`, of `static_assert` and of an empty parameter list. So
# an unset mode lets the build machine decide which language the library is
# compiled as, while a named mode keeps a build the same across compilers and
# across versions of one compiler.
# A hardening flag is only worth passing where the compiler really implements
# it FOR THIS TARGET. -fstack-clash-protection is the one that forced this
# probe: Clang implements it on x86 and s390x but not on 32-bit ARM. On armhf
# the clang driver accepts the flag, does nothing with it and then reports
# "argument unused during compilation", which -Werror turns into a hard error,
# so without the probe the library can't be built with clang on armhf at all.
# GCC accepts the flag on armhf and implements it, so it's a property of the
# pair (compiler, target) and not of either one by itself.
#
# The probe compiles an empty translation unit with the flag, -Werror and
# EXTRA_CFLAGS, and keeps the flag only when that succeeds. EXTRA_CFLAGS has to
# be in the probe because it can change the answer: with -fsanitize=address
# Apple clang predefines _FORTIFY_SOURCE as 0 (the sanitizer checks those
# accesses itself), and -D_FORTIFY_SOURCE=3 then redefines the macro. This
# beats dropping the flag on a hardcoded list of targets, which goes stale as
# compilers gain support, and it beats the
# -Wno-error=unused-command-line-argument that the alternative needs, which
# silences the diagnostic for the whole build, including where it reports a
# real mistake.
#
# The probe must never hide a LOSS of hardening, so `make hardening_report`
# below prints which flags survived for the current compiler and target. See
# also check_hardening in ci_scripts, which reads the built artifact instead
# of the flags, because a flag in the wrong variable disappears silently.
cc-option = $(shell printf 'int main(void){return 0;}' > .ccol_probe.c 2>/dev/null && 	if $(CC) $(EXTRA_CFLAGS) $(1) -Werror -c .ccol_probe.c -o .ccol_probe.o >/dev/null 2>&1; 	then printf '%s' '$(1)'; fi; rm -f .ccol_probe.c .ccol_probe.o)

# The fortify level depends on what the compiler predefines when it
# optimizes, and a probe without -O can't see that. Alpine's GCC predefines
# _FORTIFY_SOURCE=2 at -O1 and above, where a plain -D_FORTIFY_SOURCE=3 is a
# redefinition that -Werror refuses, so there the flag undefines the default
# first. Apple clang predefines 0 under -fsanitize=address because the
# sanitizer checks those accesses itself, and in that case we keep the 0.
FORTIFY_PREDEFINED := $(shell $(CC) $(EXTRA_CFLAGS) -O2 -dM -E -x c /dev/null 2>/dev/null | awk '$$2=="_FORTIFY_SOURCE"{print $$3}')
ifeq ($(FORTIFY_PREDEFINED),)
  FORTIFY_CFLAGS := $(call cc-option,-D_FORTIFY_SOURCE=3)
else ifeq ($(FORTIFY_PREDEFINED),0)
  FORTIFY_CFLAGS :=
else
  FORTIFY_CFLAGS := $(call cc-option,-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
endif

HARDENING_CFLAGS := $(call cc-option,-fstack-protector-strong) \
	$(call cc-option,-fstack-clash-protection) \
	$(FORTIFY_CFLAGS)

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
# Only the libraries that the modules in the build need are named here: zlib
# comes with clogger and OpenSSL with the HTTP modules. A build without those
# modules links neither of them, and `pkg-config --libs --static ccollections`
# doesn't name them either.
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
# The install name is where an application linked against the library looks
# for it at run time, so it points at the installed copy.
SHARED_LDFLAGS = -dynamiclib -install_name $(LIBDIR)/$(SHARED_LIBRARY_SONAME) \
	-compatibility_version $(ABI_VERSION) -current_version $(VERSION) \
	$(EXTERNAL_LIBS)
else
SHARED_LDFLAGS = $(CCOL_LD_HARDENING) -Wl,-soname,$(SHARED_LIBRARY_SONAME) -shared $(EXTERNAL_LIBS)
endif
STATIC_LDFLAGS =

# cdebuglog.c is left out of the library on purpose. It's a diagnostic log
# buffer that a suite turns on for itself, it compiles to nothing unless the
# build defines RUNNING_UNIT_TESTS, and it's there to help you chase a CI
# timing problem or a hang that is hard to reproduce (see the doc comment in
# include/internal/cdebuglog.h). It isn't part of the shipped library, not
# even as the empty translation unit a production build would make of it; a
# test suite that needs it adds src/cdebuglog.c to the SRC_FILES of its own
# Makefile.
# The list below holds the sources that the WITH_* switches above leave out,
# and cdebuglog.c, which never ships, is filtered out together with them.
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

# The list goes through $(sort), which compares bytes. The order $(wildcard)
# gives follows the locale's collation (glob(3) sorts with strcoll), so
# chttp.c and chttp1_parser.c swap places between LANG=C and en_US.UTF-8. The
# object lists are part of the link stamp below, and without the sort an
# install under sudo, which often runs in another locale, would see a changed
# stamp and link the library again.
SOURCE_FILES = $(sort $(filter-out $(SOURCE_DIR)/cdebuglog.c $(DISABLED_SOURCES),$(wildcard $(SOURCE_DIR)/*.c)))
# This reads every header directory: include/ holds the installed public
# headers, include/internal/ the internal ones, and
# include/internal/test_helpers/ the declarations that only test builds use.
# The internal wildcards matter because this list is a prerequisite of every
# object, and an internal header missing from it doesn't trigger a rebuild of
# the sources that include it, so you'd get a stale object with no error
# telling you so.
HEADER_FILES = $(filter-out $(DISABLED_HEADERS),\
                 $(wildcard $(INCLUDE_DIR)/*.h) \
                 $(wildcard $(INCLUDE_DIR)/internal/*.h) \
                 $(wildcard $(INCLUDE_DIR)/internal/test_helpers/*.h))
OBJ_FILES_SHARED = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.o)
OBJ_FILES_STATIC = $(SOURCE_FILES:$(SOURCE_DIR)/%.c=$(OBJECT_DIR)/%.static.o)

default: all

# Prints which hardening flags this compiler and target really accept. A flag
# the probe above dropped is a real loss of hardening for this build, so it
# has to be visible.
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

# The Makefiles under tests/ don't read $(BUILD_CONFIG_FILE), so every call
# into a suite passes the saved compiler and EXTRA_CFLAGS on its command line
# and the suite builds with the same settings as the library. Only a value
# this run took from the file goes this way. One you give on the command line
# reaches the suite through MAKEFLAGS anyway, and one from the environment
# through the environment, so it wins over the saved value there as well. The
# value is passed expanded, with each $ doubled so the suite doesn't expand it
# a second time.
ccol_suite_var = $(call ccol_shell_quote,$(1)=$(subst $$,$$$$,$($(1))))
CCOL_SUITE_MAKE_VARS = $(foreach v,$(filter CC EXTRA_CFLAGS,$(CCOL_RESTORED_VARIABLES)),$(call ccol_suite_var,$(v)))

test:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && $(MAKE) test $(CCOL_SUITE_MAKE_VARS)) &&) true

memtest:
	$(foreach folder,$(TEST_FOLDERS),(cd $(folder) && $(MAKE) memtest $(CCOL_SUITE_MAKE_VARS)) &&) true

# Each module runs in its own subshell and && joins the list, so the first
# failure stops the sweep with a non-zero status. Don't join a
# `cd $(folder) && ... && cd -` chain with `;` instead: that leaves the shell
# in the directory of the module that failed, the relative cd of every later
# module fails too, and sixteen "can't cd" lines bury the one real error.
generate_coverage_report:
	$(foreach folder,$(COVERAGE_TEST_FOLDERS),(cd $(folder) && $(MAKE) generate_coverage_report $(CCOL_SUITE_MAKE_VARS)) &&) true

# ---------------------------------------------------------------------------
# Aggregate coverage site
# ---------------------------------------------------------------------------
# The generate_coverage_report target of each module leaves its lcov
# tracefiles in tests/<module>/coverage/, and this target merges all of them
# into one report for the whole library. The merge is what makes the numbers
# mean anything: a file like src/common.c is compiled into most of the module
# test binaries, and only the union of their runs shows how much of it the
# suite reaches.
#
# A module can leave more than one tracefile (a suite that builds several
# binaries from one directory writes one per binary), and every one of them
# is read.
#
# The merged data is then narrowed to src/ and include/ alone. Coverage of the
# test code, the test framework and the system headers says nothing about how
# well the library is tested and only makes every headline number look better
# than it is. The extract patterns start at $(CURDIR). Don't write them as
# '*/include/*', because that also matches /usr/include/... and pulls six
# glibc and OpenSSL headers into the report, one of them at 0.0 percent.
# This stops the build when any part of the public interface loses its
# namespace prefix. The test suites can't check it since they compile the .c
# files straight into their own binaries, where a symbol without a prefix, or
# one that isn't exported, links just like a correct one.
#
# The check reads both shipped artifacts, because hidden visibility makes them
# disagree about what the interface is. A helper kept out of the shared
# object's dynamic symbol table is still a global definition in the archive,
# and a static link resolves it like an exported symbol, so an application
# that defines its own symbol of that name fails to link. Checking only the
# shared object would report a clean interface for a name that can still
# collide in the archive.
# Every installed header has to compile on its own under a strict -std=c11
# with -pedantic-errors, with both compilers. Code that USES a typed macro
# needs -std=gnu11 because the macros use statement expressions, but merely
# INCLUDING a header must not need it, or a consumer's own header couldn't
# include one without forcing gnu11 on everything downstream. The check also
# compiles each header from the installed layout, behind an application
# directory that shadows every other public name.
.PHONY: check_headers
check_headers:
	@./ci_scripts/check_public_headers_standalone.sh

.PHONY: check_namespace
check_namespace: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME)
	@./ci_scripts/check_public_namespace.sh $(SHARED_LIBRARY_REAL) $(STATIC_LIBRARY_NAME)

# Fails when a file under include/ or man/ has a name that the install would
# paste onto the installation directory as a shell pattern instead of one
# literal path. The install and uninstall recipes build their argument lists
# from these basenames, and a `*`, `?` or `[` in one of them makes `rm -f`
# delete, and `sed -i` rewrite, whatever it matches inside the installation
# directory, which means other packages' files. The recipes run under `set -f`
# so the shell expands nothing. This check is the other half of that guard: it
# keeps such a name out of the tree, so the property doesn't depend on one
# `set -f` surviving every future edit. It also requires every man alias page
# to be the single line ".so <module>/<symbol>.3" that the install rewrites,
# with a target that exists. It needs no build artifact.
.PHONY: check_filenames
check_filenames:
	@./ci_scripts/check_installable_filenames.sh

# Fails when the set of exported symbols has drifted from abi/, which records
# the ABI that libccollections.so.$(ABI_VERSION) promises. A removed symbol
# breaks every application already linked against it. A new one is
# compatible, but it has to show up as a deliberate, reviewed line in a diff
# and not as something that escaped through a missing `static`. Where
# libabigail is installed and a corpus baseline for the architecture exists,
# the same run also compares function signatures and public struct layouts,
# which a symbol list can't describe. Like check_namespace, the test suites
# can't see this, because they link the .c files directly and an unexported
# symbol resolves there exactly like an exported one.
.PHONY: check_abi
check_abi: $(SHARED_LIBRARY_NAME)
ifeq ($(strip $(DISABLED_SOURCES)),)
	@./ci_scripts/check_public_abi.sh $(SHARED_LIBRARY_REAL) $(ABI_VERSION)
else
	@echo "check_abi: skipped, this is a reduced build (modules disabled:$(patsubst $(SOURCE_DIR)/%.c, %,$(DISABLED_SOURCES)))."
	@echo "           The committed baseline describes the full library, so a"
	@echo "           smaller symbol set here is expected rather than a defect."
endif

# Compares the layout of every public struct and union, as this system's
# clang reports it, with the LP64 record in abi/public_layout.lp64. The
# structural layer of check_abi needs ELF and libabigail, but this one needs
# only clang, so it also covers macOS and OpenBSD, where the ABI gate compares
# the symbol list alone. It reads the headers and not the built library, so no
# build is needed. CLANG names the compiler.
CLANG ?= clang
.PHONY: check_layout
check_layout:
	@CLANG="$(CLANG)" ./ci_scripts/check_public_layout.sh

# Records the layout of the public records again. Run it when you change a
# public struct on purpose, and commit the new record together with that
# change.
.PHONY: update_layout_baseline
update_layout_baseline:
	@CLANG="$(CLANG)" ./ci_scripts/check_public_layout.sh --update

# Renders every man page to HTML under $(MAN_HTML_DIR), for the GitHub Pages
# site that the guides under doc/ link to. The troff sources are only read;
# `make install` copies them as they are. It first runs `mandoc -T lint` over
# every page and stops on an error, because such an error (a text line that
# starts with ' or .) also loses words under `man`. It needs mandoc and
# python3.
MAN_HTML_DIR = man_html
PYTHON ?= python3
.PHONY: man_html
man_html:
	@$(PYTHON) ./ci_scripts/build_man_html.py $(MAN_HTML_DIR)

# Fails when README.md or a guide links a man page that doesn't exist, or
# links the troff source under man/ instead of the rendered page.
.PHONY: check_doc_links
check_doc_links:
	@$(PYTHON) ./ci_scripts/check_doc_links.py

# Proves that a process can fork() after it loads this library with dlopen(),
# uses a module that registers pthread_atfork() handlers and unloads the
# library again. Like the two checks above it needs the built .so, since the
# test suites link the .c files directly and no test ever loads or unloads a
# shared object.
.PHONY: check_dso_unload
check_dso_unload: $(SHARED_LIBRARY_NAME)
	@./ci_scripts/check_dso_unload.sh $(SHARED_LIBRARY_REAL)

# check_exit_reachable links a program that uses little of the library
# against the built shared object and fails when valgrind finds memory of the
# library still allocated at exit. The test suites compile the sources into
# their own binaries, so none of them sees what the shared object's
# destructors do at exit.
# check_install installs into a temporary prefix, builds the README.md example
# with the flags of the installed ccollections.pc, runs it linked against the
# shared library and against the archive, and uninstalls. No test suite sees
# the install, because each suite compiles the sources into its own binary.
.PHONY: check_install
check_install: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME)
	@MAKE="$(MAKE)" CC="$(CC)" ./ci_scripts/check_install.sh

.PHONY: check_exit_reachable
check_exit_reachable: $(SHARED_LIBRARY_NAME)
	@CC="$(CC)" CCOL_VALGRIND_SUPP="$(CCOL_VALGRIND_SUPP)" ./ci_scripts/check_exit_reachable.sh $(SHARED_LIBRARY_REAL)

# Records the baseline again from the current build. Run it when the public
# API gets a new symbol, and commit the new baseline with that change.
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
# bench/ links against the shared library this build produces instead of
# compiling the sources into its own binary, so it measures the code that
# ships, with the flags it ships with, through the same dynamic call an
# application makes. The baseline it compares against belongs to one machine
# and we don't commit it: a nanoseconds-per-operation figure describes the
# cache hierarchy and background load of one machine, so comparing against a
# baseline from another machine reports a difference that has nothing to do
# with the library. Record a baseline with bench_update, and bench then
# reports every later run against it.
#
# BENCH_ARGS passes options straight through, for example:
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

# Fails when a file in src/ or include/ covers less than 80 percent of its
# instrumented lines. It's kept apart from coverage_site so you can rebuild
# the report and look at it without the check, and so you can rerun the check
# against an existing report without paying for another instrumented build.
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

# An object file records nothing about the compiler or flags that made it, so
# with an ordinary prerequisite list a build with different flags links the
# objects of the build before it. You end up with a sanitizer that is only
# half applied, or with objects for the wrong architecture after a CC=
# change, and a switch like CCOL_MEMPOOL_COMPACT_LAYOUT looks ignored because
# the object that holds it was never recompiled. The stamp holds the current
# compiler and flags and is rewritten only when one of them changes, so
# nothing is rebuilt without need and everything is rebuilt when it must be.
# It uses FORCE instead of a prerequisite because these values come in on the
# command line, with no input file changing alongside them.
BUILD_FLAGS_STAMP = $(OBJECT_DIR)/.build_flags

# The link has an identity of its own, apart from the compile's. The WITH_*
# switches change which objects go into the library and which libraries it
# links against, but not how any object is compiled. So after a full build
# every object a reduced build needs is already there and up to date, the
# library wouldn't be linked again, and it would keep the modules and NEEDED
# entries of the previous build. For example
# `make WITH_CHTTPSERVER=0 WITH_CHTTPCLIENT=0` after an ordinary build would
# give you a library that exports both modules and links OpenSSL, with nothing
# in the output saying so. A clean tree hides this completely, which is why
# the CI job that checks the dependencies of a reduced build can't catch it.
#
# It's a second stamp instead of a part of the one above, because changing a
# module switch must not recompile every object when the compile itself
# hasn't changed.
LINK_FLAGS_STAMP = $(OBJECT_DIR)/.link_flags

# Writes $(BUILD_CONFIG_FILE), which the top of this Makefile describes. Both
# stamps depend on it, so every command that builds or relinks anything saves
# the settings it builds with, and a command that builds nothing (test,
# clean, hardening_report) saves nothing. The file is rewritten only when its
# text changes. Each value goes in as make received it: `$(value ...)` gives
# its text unexpanded, the shell gets it in single quotes, and a # in it is
# escaped, since a bare # would start a comment where the file is read.
ccol_hash := \#
ccol_shell_quote = '$(subst ','\'',$(1))'
ccol_make_escape = $(subst $(ccol_hash),\$(ccol_hash),$(1))
ccol_config_block = printf '%s\n' \
	'ifneq ($$(filter undefined default file,$$(origin $(1))),)' \
	$(call ccol_shell_quote,$(1) = $(call ccol_make_escape,$(value $(1)))) \
	'CCOL_RESTORED_VARIABLES += $(1)' 'endif';

.PHONY: _build_config
_build_config:
	@mkdir -p $(OBJECT_DIR)
	@{ printf '%s\n' '$(ccol_hash) The build configuration of the last build; the root Makefile' \
		'$(ccol_hash) writes it and reads it. `make clean` removes it.'; \
		$(foreach v,$(BUILD_CONFIG_VARIABLES),$(call ccol_config_block,$(v))) \
	} > $(BUILD_CONFIG_FILE).tmp
	@if cmp -s $(BUILD_CONFIG_FILE).tmp $(BUILD_CONFIG_FILE); then \
		rm -f $(BUILD_CONFIG_FILE).tmp; \
	else \
		mv $(BUILD_CONFIG_FILE).tmp $(BUILD_CONFIG_FILE); \
	fi

$(BUILD_FLAGS_STAMP): FORCE _build_config
	@mkdir -p $(OBJECT_DIR)
	@printf '%s\n' '$(CC)|$(SHARED_CFLAGS)|$(STATIC_CFLAGS)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(LINK_FLAGS_STAMP): FORCE _build_config
	@mkdir -p $(OBJECT_DIR)
	@printf '%s\n' '$(CC)|$(AR)|$(EXTRA_CFLAGS)|$(SHARED_LDFLAGS)|$(STATIC_LDFLAGS)|$(OBJ_FILES_SHARED)|$(OBJ_FILES_STATIC)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

all: $(SHARED_LIBRARY_NAME) $(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE)

# EXTRA_CFLAGS goes to the link too, so a sanitizer it turns on links its
# runtime. A Linux shared object can leave the runtime symbols undefined and
# get them from the program, but -dynamiclib on macOS refuses any undefined
# symbol.
$(SHARED_LIBRARY_REAL): $(OBJ_FILES_SHARED) $(LINK_FLAGS_STAMP)
	$(CC) $(EXTRA_CFLAGS) -o $(SHARED_LIBRARY_REAL) $(OBJ_FILES_SHARED) $(SHARED_LDFLAGS)

$(SHARED_LIBRARY_SONAME): $(SHARED_LIBRARY_REAL)
	ln -sf $(SHARED_LIBRARY_REAL) $(SHARED_LIBRARY_SONAME)

$(SHARED_LIBRARY_NAME): $(SHARED_LIBRARY_SONAME)
	ln -sf $(SHARED_LIBRARY_SONAME) $(SHARED_LIBRARY_NAME)

# The pkg-config metadata is generated by the build and never committed, so
# the version and installation directories in it can't differ from the ones
# this build used. FORCE is what makes that hold: PREFIX, LIBDIR and
# INCLUDEDIR come in on the command line and can change while no input file
# changes with them, so an ordinary prerequisite list would leave an old
# directory in the installed file. The file is rewritten only when the new
# text differs from the old one, so it's generated every time and still
# triggers no other build step.
#
# A LIBDIR or INCLUDEDIR under PREFIX is written relative to ${prefix} or
# ${exec_prefix}, so `pkg-config --define-prefix` and a relocated tree keep
# working. A directory outside PREFIX is written as it is.
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

# `ar rcs` adds to an existing archive instead of replacing it, so a reduced
# build over a full one would keep the members of the modules you dropped even
# after the archive is rebuilt. Removing the archive first is what makes its
# member list follow $(OBJ_FILES_STATIC) exactly.
$(STATIC_LIBRARY_NAME): $(OBJ_FILES_STATIC) $(LINK_FLAGS_STAMP)
	@rm -f $(STATIC_LIBRARY_NAME)
	$(AR) rcs $(STATIC_LIBRARY_NAME) $(OBJ_FILES_STATIC) $(STATIC_LDFLAGS)
	$(RANLIB) $(STATIC_LIBRARY_NAME)

# The objects for the shared library, built with -fPIC
$(OBJECT_DIR)/%.o: $(SOURCE_DIR)/%.c $(HEADER_FILES) $(BUILD_FLAGS_STAMP)
	$(CC) -c $(SHARED_CFLAGS) $< -o $@

# The objects for the static library. They are built with -fPIC as well, so
# you can link the archive into a shared library. An archive isn't a linked
# object, so it records no dependency of its own on pthread, zlib, OpenSSL or
# libm, and you have to name everything the library needs on your own link
# line. $(PKGCONFIG_FILE) holds that list, and `pkg-config --libs --static
# ccollections` hands it to you.
$(OBJECT_DIR)/%.static.o: $(SOURCE_DIR)/%.c $(HEADER_FILES) $(BUILD_FLAGS_STAMP)
	$(CC) -c $(STATIC_CFLAGS) $< -o $@

# tests/cyaml/differential/cyaml_to_json is named directly instead of being
# matched, because it's the only built binary that is neither directly in a
# suite directory nor named tests_ or fuzz_, so the find below can't describe
# it.
#
# The test binaries are found instead of listed, so a directory that gets a
# new binary (like tests/mixed/tests_compat and its ThreadSanitizer twin) is
# cleaned with no edit here. The name filter is what makes that safe:
# tests_compat.c and tests_tls.c sit beside the binaries they build, so a
# plain tests/*/tests_* would delete sources. A binary here never has a dot in
# its name and every other match does, so dropping every match with a dot
# keeps exactly the binaries.
#
# The shared library is removed by glob instead of by its three current
# names, so a build tree holding artifacts of a different VERSION gets cleaned
# too. Naming only the current version would leave an old
# libccollections.so.<other> (libccollections.<other>.dylib on macOS) beside
# the new one, where at run time it can still satisfy the DT_NEEDED of an
# older binary.
clean:
	rm -rf $(SHARED_LIBRARY_NAME) $(SHARED_LIBRARY_ANY_VERSION) \
		$(STATIC_LIBRARY_NAME) $(PKGCONFIG_FILE) $(PKGCONFIG_FILE).tmp $(OBJECT_DIR) \
		tests/*/*.o tests/*/.build-flags* \
		bench/bench bench/*.o bench/.build-flags \
		tests/*/coverage tests/*/third_party_obj $(COVERAGE_SITE_DIR) \
		$(MAN_HTML_DIR) pages_site \
		tests/*/differential/cyaml_to_json \
		tests/*/*.gcno tests/*/*.gcda tests/*/*.gcov tests/*/*.c.info
	@if [ -d tests ]; then \
		find tests -maxdepth 2 -type f \
			\( -name tests -o -name 'tests_*' -o -name 'fuzz_*' \) \
			! -name '*.*' -print0 | xargs -0 -r rm -f; \
	fi

# PREFIX moves a whole install in one step, for a distribution package, a
# one-user install into $HOME/.local or a staged install into a container
# image.
# DESTDIR puts a staging directory in front of every install path without
# showing up inside any installed file, which is what a package build needs.
# DESTDIR must never reach the prefix that $(PKGCONFIG_FILE) records.
#
# LIBDIR, INCLUDEDIR, MANDIR and PKGCONFIGDIR each move one part of the
# install, for a distribution whose layout differs from PREFIX/lib and the
# rest: a Fedora package passes LIBDIR=/usr/lib64 and a Debian package
# LIBDIR=/usr/lib/<triplet>. $(PKGCONFIG_FILE) records LIBDIR and INCLUDEDIR,
# so a pkg-config consumer finds whatever this install chose.
#
# The public headers go into their own directory,
# INCLUDEDIR/$(SHORT_LIBRARY_NAME), never straight into INCLUDEDIR. Names like
# common.h and cstring.h are generic, and in a shared include directory they
# would shadow, or be shadowed by, a header of another package or of the
# application. Each public header includes its siblings with quotes, so it
# always finds its own copy first, in its own directory. An application
# writes #include <ccollections/chashmap.h> and the pkg-config Cflags name
# INCLUDEDIR itself with -I. Since no -I flag ever names the directory of our
# headers, an application header with the same name as one of ours (its own
# common.h, say) never gets shadowed by ours and never shadows ours, whatever
# order the application's -I flags come in.
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
# tab gets split into several words there, so `install -d` and `rm -f` treat
# each word as a path of its own, relative to the source tree, and the install
# creates stray directories inside it. That's why the install and the
# uninstall refuse such a value before touching anything. The test catches a
# blank inside a value (a second word) and a trailing blank (make keeps it and
# strip removes it).
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

# Everything under $(INCLUDE_DIR)/internal/ is internal to the library. No
# public header includes it and the shared library doesn't export the symbols
# it declares, so you couldn't link against an installed copy anyway; these
# headers stay in the source tree and out of the install. The directory itself
# is the boundary, so the wildcard below needs no list of names and a new
# header there stays out of the install by itself. The filter matches the
# directory prefix and not a wildcard of one level, so the headers of a
# subdirectory such as internal/test_helpers/ stay out too.
INTERNAL_HEADER_FILES = $(filter $(INCLUDE_DIR)/internal/%,$(HEADER_FILES))
PUBLIC_HEADER_FILES = $(filter-out $(INCLUDE_DIR)/internal/%,$(HEADER_FILES))
# Every header this project can install, whatever the WITH_* switches say.
# The uninstall target reads this list and not the filtered one above, because
# an uninstall run with different switches from the install would otherwise
# skip the headers its own switches leave out and leave them behind, declaring
# symbols the library doesn't have.
ALL_PUBLIC_HEADER_FILES = $(filter-out $(INCLUDE_DIR)/internal/%,\
                            $(wildcard $(INCLUDE_DIR)/*.h))

# The man/<module>/*.3 pages document the functions and their companion
# type-inferred macros side by side (see man/README). The install puts them
# all into one flat man3 directory, which loses nothing because real symbol
# names never collide across the two kinds of page. The man/<module>/*.7
# pages are the module overview pages. An alias page holds a
# ".so <module>/<symbol>.3" redirect relative to the source tree's layout,
# and the install rewrites it to ".so man3/<symbol>.3", relative to the root
# of the installed MANPATH.
ALL_MAN3_SRC_FILES = $(wildcard man/*/*.3)
ALL_MAN7_SRC_FILES = $(wildcard man/*/*.7)
# The pages of a module you turn off leave the install along with its header,
# for the same reason the header goes: they document functions that no
# installed header declares and the installed library doesn't hold. The
# uninstall target uses the unfiltered lists above, for the reason given
# there.
DISABLED_MAN_DIRS = $(patsubst $(SOURCE_DIR)/%.c,man/%/,$(DISABLED_SOURCES))
DISABLED_MAN_FILES = $(foreach d,$(DISABLED_MAN_DIRS),$(wildcard $(d)*))
MAN3_SRC_FILES = $(filter-out $(DISABLED_MAN_FILES),$(ALL_MAN3_SRC_FILES))
MAN7_SRC_FILES = $(filter-out $(DISABLED_MAN_FILES),$(ALL_MAN7_SRC_FILES))

# Ask for more rights only when the install needs them. An install into a
# prefix the current user owns, like $HOME/.local or a DESTDIR staging tree of
# a package build or a CI job, must not ask for a password. What we test is
# whether the nearest existing ancestor is writable, since the install makes
# the leaf directories itself. You can set the value yourself with
# `make install SUDO=` or `make install SUDO=doas`.
# This uses origin and not ?= so that both `make install SUDO=doas` and a SUDO
# from the environment win, and so that the probe below runs only once. A
# plain `?=` would rerun the whole shell at each of the fifteen or so
# expansions one install makes, and a plain `:=` would ignore the value from
# the environment.
#
# The walk stops at a component with no separator in it, because `$${d%/*}`
# does nothing once there's no slash left to remove, and without that stop a
# relative DESTDIR or PREFIX would make it loop forever.
# `make install DESTDIR=stage PREFIX=/usr` is an ordinary packaging command of
# that kind. The two shapes don't end at the same fallback though: removing
# the last component of an ABSOLUTE path with one component leaves nothing, so
# the nearest existing ancestor there is / and not the working directory. So
# `make install PREFIX=/newroot` has to see that / isn't writable and ask for
# more rights, instead of testing a directory the caller happens to own.
#
# Every installation directory is probed and not PREFIX alone, because
# LIBDIR, INCLUDEDIR, MANDIR and PKGCONFIGDIR can each point outside PREFIX,
# and one directory the user can't write is enough to need the escalation.
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

# ldconfig refreshes the dynamic linker's cache and mandb (makewhatis on
# FreeBSD, see platform.mk) does the same for the man index. Both act on the
# live system, so the install skips them when it stages files into a DESTDIR
# for packaging: the live system is exactly what a staged install leaves
# alone, and running ldconfig there would give a misleading result as well.
# The install succeeds even if either command fails.
REFRESH_SYSTEM_CACHES = \
	if [ -z "$(strip $(DESTDIR))" ]; then \
		$(SUDO) $(CCOL_LDCONFIG) || true; \
		$(SUDO) $(CCOL_MAN_INDEX) || true; \
	fi

# A reduced build has the full build's SONAME but exports fewer symbols, so an
# application linked against a full build fails to start once you install a
# reduced one over it. Nothing in the installed files says which kind of build
# they came from, and the failure shows up later in an unrelated program at
# its next start and not here, so you have to ask for the reduced case
# explicitly. ALLOW_REDUCED_INSTALL=1 is that request. It serves the embedding
# case a reduced build exists for, and a full build doesn't need it.
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
# once it's empty, and leaves it (rmdir prints a message) when it holds a file
# that this install didn't put there.
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
