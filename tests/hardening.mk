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

CCOL_HARDENING_CFLAGS := $(call ccol-cc-option,-fstack-protector-strong) \
	$(call ccol-cc-option,-fstack-clash-protection) \
	$(call ccol-cc-option,-D_FORTIFY_SOURCE=3)
