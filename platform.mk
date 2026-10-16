# Everything that differs by system, shared by the root Makefile, bench/Makefile
# and every test suite Makefile (through tests/hardening.mk).
#
# The build needs GNU make, which FreeBSD calls gmake; FreeBSD's own make reads
# BSDmakefile, and that file only tells you so.
#
# make predefines CC as `cc`, so a `CC ?= gcc` would never take effect. The
# origin test below is what tells the predefined value from one the caller
# picked, and a CC on the command line or in the environment always wins.
#
# On Linux the default is gcc, since the build and the CI matrix are written
# around it. Every other supported system ships its own compiler as cc (clang
# on FreeBSD and macOS) and may not have gcc at all.
CCOL_UNAME_S := $(shell uname -s)

ifeq ($(origin CC),default)
  ifeq ($(CCOL_UNAME_S),Linux)
    CC = gcc
  else
    CC = cc
  endif
endif

# The libraries that POSIX keeps outside libc. glibc has the thread and math
# functions in libc itself, so a Linux link that forgets them works anyway and
# the same link then fails on FreeBSD. backtrace(3) is in libexecinfo on the
# BSDs and in libc on glibc and macOS.
CCOL_PLATFORM_LIBS := -lpthread -lm
ifneq ($(filter FreeBSD OpenBSD,$(CCOL_UNAME_S)),)
  CCOL_PLATFORM_LIBS += -lexecinfo
endif

# A test that calls dlsym(3) or dlopen(3) links this. glibc before 2.34 has
# them in libdl and every Linux C library accepts -ldl, while macOS and the BSDs
# have them in libc.
CCOL_DL_LIBS := $(if $(filter Linux,$(CCOL_UNAME_S)),-ldl)

# On FreeBSD memtest also reads tests/valgrind_freebsd.supp, which explains
# what it accepts and why. No other system passes anything extra.
CCOL_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
CCOL_VALGRIND_SUPP :=
ifeq ($(CCOL_UNAME_S),FreeBSD)
  CCOL_VALGRIND_SUPP := --suppressions=$(CCOL_ROOT)/tests/valgrind_freebsd.supp
endif

# The `make install` defaults that differ by system. FreeBSD's pkgconf looks in
# $(PREFIX)/libdata/pkgconfig, not in $(LIBDIR)/pkgconfig. Linux ldconfig
# rebuilds the dynamic linker's cache from its configured directories, but
# FreeBSD's ldconfig with no argument would replace its hints with only the
# built-in directories, so there we use -R, which rescans the configured ones.
# OpenBSD's ldconfig also replaces its hints with the directories you give it,
# and -m merges one directory into them instead. FreeBSD and OpenBSD index man
# pages with makewhatis(1) and don't have mandb.
ifeq ($(CCOL_UNAME_S),FreeBSD)
  CCOL_DEFAULT_PKGCONFIGDIR = $(PREFIX)/libdata/pkgconfig
  CCOL_LDCONFIG = ldconfig -R
  CCOL_MAN_INDEX = makewhatis $(MAN_INSTALL_DIR)
else ifeq ($(CCOL_UNAME_S),OpenBSD)
  CCOL_DEFAULT_PKGCONFIGDIR = $(LIBDIR)/pkgconfig
  CCOL_LDCONFIG = ldconfig -m $(LIBDIR)
  CCOL_MAN_INDEX = makewhatis $(MAN_INSTALL_DIR)
else ifeq ($(CCOL_UNAME_S),Darwin)
  # macOS has no dynamic linker cache to refresh, and the system rebuilds
  # its man index on its own schedule.
  CCOL_DEFAULT_PKGCONFIGDIR = $(LIBDIR)/pkgconfig
  CCOL_LDCONFIG = true
  CCOL_MAN_INDEX = true
else
  CCOL_DEFAULT_PKGCONFIGDIR = $(LIBDIR)/pkgconfig
  CCOL_LDCONFIG = ldconfig
  CCOL_MAN_INDEX = mandb -q
endif

# On FreeBSD the ThreadSanitizer test binaries link tests/tsan_freebsd.c, which
# sets the runtime option and the suppression FreeBSD needs (it explains why).
ifeq ($(CCOL_UNAME_S),FreeBSD)
  CCOL_TSAN_EXTRA_SRC := $(CCOL_ROOT)/tests/tsan_freebsd.c
else
  CCOL_TSAN_EXTRA_SRC :=
endif

# Link flags that send every call to each function in $(1) to __wrap_<name> in
# a test, with __real_<name> for the original. Apple's linker has no --wrap, so
# there it gives TEST_NO_LD_WRAP=1 instead and the test defines the function
# under its own name (see the test).
ccol_ld_wrap = $(if $(filter Darwin,$(CCOL_UNAME_S)),-DTEST_NO_LD_WRAP=1,$(foreach s,$(1),-Wl,--wrap=$(s)))

# The shared library suffix: libccollections.dylib on macOS and
# libccollections.so everywhere else. The root Makefile and bench/ both name the
# library with it.
ifeq ($(CCOL_UNAME_S),Darwin)
  CCOL_SHARED_LIBRARY_SUFFIX := dylib
else
  CCOL_SHARED_LIBRARY_SUFFIX := so
endif

# Linker hardening for every link, RELRO and immediate binding. Both are ELF
# linker options; Apple's linker has neither (a Mach-O image binds its pointers
# read-only by itself) and rejects them.
ifeq ($(CCOL_UNAME_S),Darwin)
  CCOL_LD_HARDENING :=
else
  CCOL_LD_HARDENING := -Wl,-z,relro,-z,now
endif
