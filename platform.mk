# The compiler default shared by the root Makefile, bench/Makefile and every
# test suite Makefile (through tests/hardening.mk).
#
# The build needs GNU make, which FreeBSD calls gmake; FreeBSD's own make reads
# BSDmakefile, which only says so.
#
# make predefines CC as `cc`, so `CC ?= gcc` never takes effect; the origin
# test below is what tells the predefined value apart from one that the caller
# chose. A CC on the command line, or in the environment, always wins.
#
# On Linux the default is gcc, the compiler that the build and the CI matrix
# are written around. Every other supported system ships its own compiler as
# cc (clang on FreeBSD and macOS) and may have no gcc at all.
CCOL_UNAME_S := $(shell uname -s)

ifeq ($(origin CC),default)
  ifeq ($(CCOL_UNAME_S),Linux)
    CC = gcc
  else
    CC = cc
  endif
endif

# The libraries that POSIX puts outside libc. glibc keeps the thread and math
# functions in libc itself, so a Linux link that forgets them still works,
# while the same link fails on FreeBSD. backtrace(3) lives in libexecinfo on
# the BSDs and in libc on glibc and macOS.
CCOL_PLATFORM_LIBS := -lpthread -lm
ifneq ($(filter FreeBSD NetBSD OpenBSD DragonFly,$(CCOL_UNAME_S)),)
  CCOL_PLATFORM_LIBS += -lexecinfo
endif

# memtest on FreeBSD also reads tests/valgrind_freebsd.supp, which explains
# what it accepts and why. Every other system passes nothing extra.
CCOL_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
CCOL_VALGRIND_SUPP :=
ifeq ($(CCOL_UNAME_S),FreeBSD)
  CCOL_VALGRIND_SUPP := --suppressions=$(CCOL_ROOT)/tests/valgrind_freebsd.supp
endif

# The defaults of `make install` that differ by system. FreeBSD's pkgconf
# searches $(PREFIX)/libdata/pkgconfig and not $(LIBDIR)/pkgconfig. Linux
# ldconfig rebuilds the cache of the dynamic linker from its configured
# directories, while FreeBSD's ldconfig with no argument would replace its
# hints with the built-in directories alone; -R is the rescan of the configured
# ones. FreeBSD indexes man pages with makewhatis(1) and has no mandb.
ifeq ($(CCOL_UNAME_S),FreeBSD)
  CCOL_DEFAULT_PKGCONFIGDIR = $(PREFIX)/libdata/pkgconfig
  CCOL_LDCONFIG = ldconfig -R
  CCOL_MAN_INDEX = makewhatis $(MAN_INSTALL_DIR)
else ifeq ($(CCOL_UNAME_S),Darwin)
  # macOS keeps no cache of the dynamic linker to refresh, and its man index
  # is rebuilt by the system on its own schedule.
  CCOL_DEFAULT_PKGCONFIGDIR = $(LIBDIR)/pkgconfig
  CCOL_LDCONFIG = true
  CCOL_MAN_INDEX = true
else
  CCOL_DEFAULT_PKGCONFIGDIR = $(LIBDIR)/pkgconfig
  CCOL_LDCONFIG = ldconfig
  CCOL_MAN_INDEX = mandb -q
endif

# The ThreadSanitizer test binaries of FreeBSD link tests/tsan_freebsd.c, which
# sets the runtime option and the suppression that FreeBSD needs; it says why.
ifeq ($(CCOL_UNAME_S),FreeBSD)
  CCOL_TSAN_EXTRA_SRC := $(CCOL_ROOT)/tests/tsan_freebsd.c
else
  CCOL_TSAN_EXTRA_SRC :=
endif

# The link flags that route every call to each function in $(1) through
# __wrap_<name> in a test, with __real_<name> for the original. Apple's linker
# has no --wrap, so there the call gives TEST_NO_LD_WRAP=1 instead, and the
# test defines the function under its own name (see the test).
ccol_ld_wrap = $(if $(filter Darwin,$(CCOL_UNAME_S)),-DTEST_NO_LD_WRAP=1,$(foreach s,$(1),-Wl,--wrap=$(s)))

# The suffix of a shared library: libccollections.dylib on macOS, and
# libccollections.so elsewhere. The root Makefile and bench/ both name the
# library with it.
ifeq ($(CCOL_UNAME_S),Darwin)
  CCOL_SHARED_LIBRARY_SUFFIX := dylib
else
  CCOL_SHARED_LIBRARY_SUFFIX := so
endif

# The linker hardening of every link: RELRO and immediate binding. Both are
# options of the ELF linkers; Apple's linker has neither (a Mach-O image binds
# its pointers read-only on its own), and rejects them.
ifeq ($(CCOL_UNAME_S),Darwin)
  CCOL_LD_HARDENING :=
else
  CCOL_LD_HARDENING := -Wl,-z,relro,-z,now
endif
