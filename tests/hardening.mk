# Shared hardening-flag probe for every test suite Makefile. It first takes
# the default compiler from platform.mk at the root of the repository.
include $(dir $(lastword $(MAKEFILE_LIST)))../platform.mk
#
# A hardening flag is only worth passing where the compiler really implements
# it FOR THIS TARGET. -fstack-clash-protection is the flag that forced this:
# Clang implements it on x86 and s390x and not on 32-bit ARM. On armhf the
# clang driver accepts it, does nothing with it, and then reports "argument
# unused during compilation", which -Werror turns into a hard error. GCC
# accepts and implements it on armhf, so this is a property of the pair
# (compiler, target) and not of either one alone.
#
# The probe compiles an empty translation unit with the flag, with -Werror and
# with EXTRA_CFLAGS, and keeps the flag only when that succeeds. Under
# -fsanitize=address Apple clang predefines _FORTIFY_SOURCE as 0, and
# -D_FORTIFY_SOURCE=3 then redefines it, so the probe drops it there. The
# alternative is -Wno-error=unused-command-line-argument, which silences that
# diagnostic for the WHOLE build, including every place where it reports a
# real mistake.
#
# The root Makefile carries the same probe for the library itself. Keep the
# two in step: a suite that hardens less than the library it tests is
# measuring a different binary from the one that ships.
ccol-cc-option = $(shell printf 'int main(void){return 0;}' > .ccol_probe.c 2>/dev/null && \
	if $(CC) $(EXTRA_CFLAGS) $(1) -Werror -c .ccol_probe.c -o .ccol_probe.o >/dev/null 2>&1; \
	then printf '%s' '$(1)'; fi; rm -f .ccol_probe.c .ccol_probe.o)

# The fortify level depends on what the compiler predefines when it
# optimizes, which a probe without -O cannot see. Alpine's GCC predefines
# _FORTIFY_SOURCE=2 at -O1 and above, and a plain -D_FORTIFY_SOURCE=3 is then
# a redefinition that -Werror refuses; there the flag first undefines the
# default. Apple clang predefines 0 under -fsanitize=address, because the
# sanitizer checks those accesses itself, and there the build keeps that.
CCOL_FORTIFY_PREDEFINED := $(shell $(CC) $(EXTRA_CFLAGS) -O2 -dM -E -x c /dev/null 2>/dev/null | awk '$$2=="_FORTIFY_SOURCE"{print $$3}')
ifeq ($(CCOL_FORTIFY_PREDEFINED),)
  CCOL_FORTIFY_CFLAGS := $(call ccol-cc-option,-D_FORTIFY_SOURCE=3)
else ifeq ($(CCOL_FORTIFY_PREDEFINED),0)
  CCOL_FORTIFY_CFLAGS :=
else
  CCOL_FORTIFY_CFLAGS := $(call ccol-cc-option,-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
endif

CCOL_HARDENING_CFLAGS := $(call ccol-cc-option,-fstack-protector-strong) \
	$(call ccol-cc-option,-fstack-clash-protection) \
	$(CCOL_FORTIFY_CFLAGS)
