# Hardening-flag probe shared by every test suite Makefile. It takes the
# default compiler from platform.mk at the root of the repository first.
include $(dir $(lastword $(MAKEFILE_LIST)))../platform.mk
#
# A hardening flag is only worth passing where the compiler really implements
# it FOR THIS TARGET. -fstack-clash-protection is the flag that needs this:
# Clang implements it on x86 and s390x but not on 32-bit ARM. On armhf the
# clang driver accepts it, does nothing with it and then reports "argument
# unused during compilation", which -Werror turns into a hard error. GCC
# accepts and implements it on armhf, so it's a property of the (compiler,
# target) pair and not of either one by itself.
#
# The probe compiles an empty translation unit with the flag, -Werror and
# EXTRA_CFLAGS, and keeps the flag only if that works. Under -fsanitize=address
# Apple clang predefines _FORTIFY_SOURCE as 0 and -D_FORTIFY_SOURCE=3 would
# redefine it, so the probe drops it there. The other way out would be
# -Wno-error=unused-command-line-argument, but that silences the diagnostic for
# the WHOLE build, including every place where it reports a real mistake.
#
# The root Makefile has the same probe for the library itself. Keep the two in
# step, because a suite that hardens less than the library it tests is
# measuring a different binary from the one that ships.
# The probe works in a directory of its own, so two makes that read this file
# at the same time never delete each other's probe, and a read-only checkout
# still gets its flags.
ccol-cc-option = $(shell d=$$(mktemp -d 2>/dev/null) && \
	printf 'int main(void){return 0;}' > "$$d/probe.c" && \
	if $(CC) $(EXTRA_CFLAGS) $(1) -Werror -c "$$d/probe.c" -o "$$d/probe.o" >/dev/null 2>&1; \
	then printf '%s' '$(1)'; fi; [ -z "$$d" ] || rm -rf "$$d")

# The fortify level depends on what the compiler predefines when it optimizes,
# and a probe without -O can't see that. Alpine's GCC predefines
# _FORTIFY_SOURCE=2 at -O1 and above, so a plain -D_FORTIFY_SOURCE=3 is a
# redefinition that -Werror refuses, and there the flag undefines the default
# first. Apple clang predefines 0 under -fsanitize=address, because the
# sanitizer checks those accesses itself, and in that case the build keeps it.
CCOL_FORTIFY_PREDEFINED := $(shell $(CC) $(EXTRA_CFLAGS) -O2 -dM -E -x c /dev/null 2>/dev/null | awk '$$2=="_FORTIFY_SOURCE"{print $$3}')
ifeq ($(CCOL_FORTIFY_PREDEFINED),)
  CCOL_FORTIFY_CFLAGS := $(call ccol-cc-option,-D_FORTIFY_SOURCE=3)
else ifeq ($(CCOL_FORTIFY_PREDEFINED),0)
  CCOL_FORTIFY_CFLAGS :=
else
  CCOL_FORTIFY_CFLAGS := $(call ccol-cc-option,-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
endif

# Every probe answers "no" when mktemp can't make its directory, which would
# drop all of the hardening without a word, so say so once.
ifneq ($(shell d=$$(mktemp -d 2>/dev/null) && rm -rf "$$d" && echo ok),ok)
  $(warning mktemp -d fails (check TMPDIR); the suite gets no hardening flags)
endif
CCOL_HARDENING_CFLAGS := $(call ccol-cc-option,-fstack-protector-strong) \
	$(call ccol-cc-option,-fstack-clash-protection) \
	$(CCOL_FORTIFY_CFLAGS)
