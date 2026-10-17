#!/bin/sh
# Compares the exported ABI of the built shared library with a committed
# baseline, so that an export or a removal nobody meant can't slip into a
# release unnoticed.
#
# The test suites can't check this. Every suite compiles the .c files straight
# into its own binary, where a static link resolves a hidden symbol just like
# an exported one, so a library that exports nothing at all passes the whole
# suite. Only the built .so has the evidence.
#
# There are two layers and each one catches a different mistake:
#
#   1. The set of exported symbols, which readelf reads off the .so. It
#      catches a new symbol that escapes through a missing `static` or through
#      a declaration that doesn't belong inside the visibility block of a
#      public header, and also a symbol that disappears, which breaks every
#      application that is already linked. It only needs binutils, so it
#      always runs. On macOS the library is a Mach-O file and this layer reads
#      the dylib with the system's nm instead.
#   2. The full ABI corpus: the function signatures and the layout of every
#      public struct those signatures reach, compared by abidiff from
#      libabigail. It catches what a list of names can't, like a parameter type
#      that changes or a new field in a public struct that a caller passes by
#      value (a config struct, for example). It needs libabigail on the machine
#      and a baseline for the same architecture, and it says it isn't active
#      when either is missing. abidiff reads only ELF, so this layer doesn't
#      run on a Mach-O dylib. The public types have one layout on every LP64
#      system, and check_public_layout.sh measures it with the clang of macOS
#      and OpenBSD, where this layer can't run.
#
# Layer 2 works per architecture because layout does. A change can leave every
# structure identical byte for byte on one target and break another: widen a
# member from size_t to unsigned long long and nothing changes on LP64, where
# both types have the same width, while on ILP32 every later field moves. Only
# the baseline of the architecture you build can see that, and that's why the
# architecture is read from the ELF header of the library itself.
#
# Usage:
#   check_public_abi.sh <shared-object> <abi-version> [--update]
#
# CCOL_ABI_ARCH overrides the architecture name taken from the ELF header; use
# it for a target this script doesn't know yet.
#
# --update rewrites the baselines from the library you give it instead of
# comparing against them. A new symbol is an ordinary change that keeps the
# ABI, so for one you run `make update_abi_baseline` and commit the result
# together with the new API. The check is there to make each addition a
# deliberate line in a diff a reviewer can see, not to forbid it.
set -eu

# The symbol baseline is a sorted list and sort(1) orders by the locale, so we
# use the C locale to get the same file whoever records it.
LC_ALL=C
export LC_ALL

SO="${1:-}"
ABI_VERSION="${2:-}"
MODE="${3:-check}"

[ -n "$SO" ] && [ -n "$ABI_VERSION" ] || {
	echo "usage: $0 <shared-object> <abi-version> [--update]" >&2
	exit 2
}
[ -e "$SO" ] || {
	echo "check_public_abi: $SO not built; run make first" >&2
	exit 2
}

. ci_scripts/object_format.sh
if ! FORMAT=$(ccol_object_format "$SO"); then
	echo "check_public_abi: $SO is neither an ELF nor a Mach-O file" >&2
	exit 2
fi
if [ "$FORMAT" = elf ]; then
	. ci_scripts/gnu_binutils.sh
fi

ABI_DIR=abi
SYMS_BASELINE="$ABI_DIR/libccollections.so.$ABI_VERSION.symbols"

# The corpus baseline records real struct layouts and parameter sizes, which
# differ between ILP32 and LP64 and between the calling conventions of
# different machines, so every architecture has its own baseline. A build for
# an architecture that has none skips layer 2 rather than compare against a
# baseline that describes some other architecture.
#
# The architecture is read from the library's own ELF header, never from the
# machine running this script. A cross-build makes a library for one
# architecture on a host of another, so taking the name from the host would
# compare every cross-built library against the host's baseline, and --update
# would overwrite that baseline. Both failures are silent, because the
# comparison is against a real baseline of the wrong architecture and it
# reports plausible struct differences instead of an error.
elf_arch() {
	_hdr=$("$READELF" -h -W "$1") || return 1
	_class=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*Class:[[:space:]]*//p')
	_machine=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
	_flags=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*Flags:[[:space:]]*//p')
	_osabi=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*OS\/ABI:[[:space:]]*//p')
	# The target's C library declares every system type the public
	# interface names, and its headers spell them differently: FreeBSD's
	# size_t is the typedef __size_t where glibc's is unsigned long, so
	# abidiff reports every such parameter as a change even where the size
	# and layout are the same. That's why a FreeBSD library gets baselines
	# of its own with a "freebsd-" prefix, while a Linux library carries
	# the System V or the GNU OS/ABI and keeps the plain architecture name.
	_prefix=
	case "$_osabi" in
	*FreeBSD*) _prefix=freebsd- ;;
	esac
	printf '%s' "$_prefix"
	case "$_machine:$_class" in
	"Advanced Micro Devices X86-64:ELF64") echo x86_64 ;;
	"Intel 80386:ELF32") echo i386 ;;
	"AArch64:ELF64") echo aarch64 ;;
	"IBM S/390:ELF64") echo s390x ;;
	# On 32-bit ARM it's the ABI, not the machine, that picks the calling
	# convention for a floating-point argument, so hard-float and
	# soft-float are two different ABIs and each gets its own baseline.
	"ARM:ELF32")
		case "$_flags" in
		*hard-float*) echo armhf ;;
		*) echo armel ;;
		esac
		;;
	*) return 1 ;;
	esac
}

# CCOL_ABI_ARCH names the baseline directly, for a target this mapping doesn't
# cover yet.
ARCH="${CCOL_ABI_ARCH:-}"
if [ "$FORMAT" = macho ]; then
	ARCH=macho
elif [ -z "$ARCH" ]; then
	# The assignment is kept apart from the declaration on purpose. With
	# `ARCH=$(elf_arch ...)` the exit status is the status of the
	# assignment, so a failure inside the function would leave ARCH empty
	# where `set -e` couldn't see it.
	if ! ARCH=$(elf_arch "$SO") || [ -z "$ARCH" ]; then
		echo "check_public_abi: could not identify the architecture of $SO from" >&2
		echo "                  its ELF header. Set CCOL_ABI_ARCH to name the" >&2
		echo "                  baseline explicitly if this is a target the" >&2
		echo "                  script does not yet map." >&2
		exit 1
	fi
fi
CORPUS_BASELINE="$ABI_DIR/libccollections.so.$ABI_VERSION.$ARCH.abi"

# The corpus records the PUBLIC interface, not every type that the library's
# debug information happens to reach.
#
# --headers-dir names the installed headers, so a type counts as public when
# one of them declares it, and --drop-private-types leaves out every other
# type. Without the pair abidw descends into the DEFINITION of each opaque
# struct, which lives in a .c file, and records whatever is in it; for this
# library that pulls in glibc's pthread_mutex_t and pthread_cond_t, since the
# queue and loop structs embed them.
#
# Those types aren't part of the contract with a consumer: no installed header
# names one, and a consumer only ever holds a pointer to the opaque struct
# around them. Recording them would make the corpus a property of the
# TOOLCHAIN rather than of the interface. GCC emits the array-subrange index
# type of their internal char arrays as `unsigned int` and Clang emits
# `__ARRAY_SIZE_TYPE__`, so a baseline recorded with one compiler would fail
# against a build from the other with "type size hasn't changed" on every
# line. That's a difference in a third party's debug information, not in the
# ABI, and it would make the gate useless with Clang on i386.
#
# We lose nothing this gate is for by dropping them. Every type a consumer can
# name, size or lay out is declared in an installed header, so --headers-dir
# keeps all of them, including the structs a public MACRO computes a size or
# an offset from, which are exactly the ones the gate is there to watch.
ABIDW_SCOPE_FLAGS="--drop-private-types --headers-dir include"

extract_symbols() {
	if [ "$FORMAT" = macho ]; then
		ccol_macho_defined_globals "$1"
		return
	fi
	# Take the defined (not UND) GLOBAL and WEAK dynamic symbols and strip
	# any @VERSION suffix, which gives exactly the set an application can
	# link against.
	#
	# We check the status of readelf rather than of the pipeline, which
	# would be the status of sort. readelf can fail when the file isn't ELF,
	# is truncated or is for another architecture, and without this check
	# such a failure gives an empty set with a success status, and an empty
	# set is exactly what --update would then write over the committed
	# baseline. /bin/sh here doesn't always have pipefail, so the read and
	# the filter are separate steps.
	#
	# _init and _fini are left out. They are the entry points of the
	# initialisation and termination sections, which the C runtime start
	# files (crti.o) define, not this library. FreeBSD's start files give
	# them default visibility, so every FreeBSD shared object exports them,
	# while glibc's keep them hidden.
	_raw=$("$READELF" --dyn-syms -W "$1") || return 1
	printf '%s\n' "$_raw" \
		| awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK") && $8!="_init" && $8!="_fini"{print $8}' \
		| sed 's/@.*//' | sort -u
}

mkdir -p "$ABI_DIR"
if ! current=$(extract_symbols "$SO"); then
	echo "check_public_abi: could not read dynamic symbols from $SO." >&2
	exit 1
fi
# An empty set is never a correct answer for this library and both failures
# above produce one, so we refuse it before comparing or writing anything.
if [ -z "$current" ]; then
	echo "check_public_abi: $SO exports no dynamic symbols; refusing to" >&2
	echo "                  compare or record an empty baseline." >&2
	exit 1
fi

# CCOL_MEMPOOL_COMPACT_LAYOUT changes cmempool's entry stride and with it the
# layout of a struct the corpus baseline records, so a compact build really is
# different from the recorded one. It's a supported build configuration, not a
# defect, but the baseline describes the default build, so we report the
# situation and stop instead of printing a page of struct layout differences
# under an "ABI BREAK" heading.
#
# The setting is read off the built library and not from a make variable,
# because it can come in through EXTRA_CFLAGS, a CFLAGS override or an edit to
# the header. The library names the setting it was built with, so the artifact
# can answer this question by itself.
if printf '%s\n' "$current" | grep -qx '_ccol_mempool_built_with_compact_layout'; then
	echo "check_public_abi: skipped, this library was built with"
	echo "                  CCOL_MEMPOOL_COMPACT_LAYOUT=1. That setting changes"
	echo "                  cmempool's entry stride and the layout that goes with"
	echo "                  it, so it is a different ABI by construction, and the"
	echo "                  committed baseline describes the default build."
	echo "                  Nothing was compared, and with --update nothing was"
	echo "                  written: recording from here would replace the"
	echo "                  default build's baseline with a compact one."
	exit 0
fi

# The baselines are recorded from the ELF build, where every layer runs; a
# Mach-O build is compared against them but never writes them.
if [ "$MODE" = "--update" ] && [ "$FORMAT" = macho ]; then
	echo "check_public_abi: refused, $SO is a Mach-O dylib. Record the" >&2
	echo "                  baselines from an ELF build on Linux, where the" >&2
	echo "                  corpus layer runs too." >&2
	exit 2
fi

if [ "$MODE" = "--update" ]; then
	{
		echo "# Exported ABI of libccollections.so.$ABI_VERSION."
		echo "#"
		echo "# There is one symbol on each line, in sorted order. To make"
		echo "# this file again, run \`make update_abi_baseline\`. Commit the"
		echo "# result together with the change whenever the public API gets a"
		echo "# new symbol. Do not remove a line from this file, and do not"
		echo "# change the signature of a symbol that is in it. Either change"
		echo "# breaks every application that is already linked against this"
		echo "# ABI version, and you must then increase VERSION_MAJOR in the"
		echo "# root Makefile."
		echo "$current"
	} > "$SYMS_BASELINE"
	echo "check_public_abi: wrote $SYMS_BASELINE ($(echo "$current" | wc -l | tr -d ' ') symbols)"

	if command -v abidw >/dev/null 2>&1; then
		abidw $ABIDW_SCOPE_FLAGS --out-file "$CORPUS_BASELINE" "$SO"
		echo "check_public_abi: wrote $CORPUS_BASELINE"
	else
		echo "check_public_abi: abidw not installed; $CORPUS_BASELINE not written."
		echo "                  Install libabigail (Debian/Ubuntu: abigail-tools) to"
		echo "                  enable signature and struct-layout checking."
	fi
	exit 0
fi

fail=0

# ---------------------------------------------------------------------------
# Layer 1: exported symbol set
# ---------------------------------------------------------------------------
if [ ! -f "$SYMS_BASELINE" ]; then
	echo "check_public_abi: no baseline at $SYMS_BASELINE" >&2
	echo "                  Run \`make update_abi_baseline\` and commit it." >&2
	exit 2
fi

# Real temporary files instead of process substitution, because the script
# runs under /bin/sh, which is dash on Debian and Ubuntu, and there <(...) is
# just a syntax error.
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT INT TERM
grep -v '^#' "$SYMS_BASELINE" | grep -v '^[[:space:]]*$' | sort -u > "$tmpdir/baseline"
printf '%s\n' "$current" > "$tmpdir/current"

removed=$(comm -23 "$tmpdir/baseline" "$tmpdir/current")
added=$(comm -13 "$tmpdir/baseline" "$tmpdir/current")

if [ -n "$removed" ]; then
	echo "ABI BREAK: symbols in the baseline are no longer exported." >&2
	echo "Every application already linked against libccollections.so.$ABI_VERSION" >&2
	echo "fails to start against this build. Restore them, or increment" >&2
	echo "VERSION_MAJOR in the root Makefile to declare a new ABI." >&2
	printf '%s\n' "$removed" | sed 's/^/  -/' >&2
	fail=1
fi

if [ -n "$added" ]; then
	echo "Newly exported symbols are not in the baseline." >&2
	echo "If these are intended public API, run \`make update_abi_baseline\` and" >&2
	echo "commit the result. If they are internal, they are missing a \`static\`" >&2
	echo "or are declared inside a public header's visibility block by mistake." >&2
	printf '%s\n' "$added" | sed 's/^/  +/' >&2
	fail=1
fi

# ---------------------------------------------------------------------------
# Layer 2: full ABI corpus
# ---------------------------------------------------------------------------
if [ "$FORMAT" = macho ]; then
	echo "check_public_abi: NOTE - $SO is a Mach-O dylib and abidiff reads only" \
		"ELF, so signature and struct-layout checking did not run here; the" \
		"Linux jobs check the same public types."
elif ! command -v abidiff >/dev/null 2>&1; then
	echo "check_public_abi: NOTE - abidiff not installed, so signature and" \
		"struct-layout checking did not run (install abigail-tools to enable it)."
elif [ ! -f "$CORPUS_BASELINE" ]; then
	echo "check_public_abi: NOTE - no $ARCH corpus baseline at $CORPUS_BASELINE," \
		"so signature and struct-layout checking did not run" \
		"(run \`make update_abi_baseline\` on this architecture to create it)."
else
	# abidiff's exit status is a BIT MASK, not a true or false value, so
	# `if ! abidiff` is wrong. Bits 0 and 1 (values 1 and 2) are abidiff's
	# own error and usage failures; they say nothing about the ABI and we
	# must not report them as if they did.
	#
	# Bit 3 (value 8, ABIDIFF_ABI_INCOMPATIBLE_CHANGE) is set only for a
	# change abidiff can PROVE is incompatible. That set is smaller than the
	# set of changes that really break a compiled application, so it can't
	# be the only case that fails. A public struct that grows sets bit 2
	# alone (value 4, ABIDIFF_ABI_CHANGE): one more size_t in cmap_pair, for
	# example, takes it from 16 to 24 bytes, which moves every later field
	# of every public struct holding a cmap_pair and breaks every
	# application built against the old header, and yet abidiff exits 4 for
	# it. So bit 2 is fatal here as well.
	#
	# That makes this layer behave like the symbol name layer above: any
	# change to the ABI, compatible or not, has to be recorded again with
	# `make update_abi_baseline` and show up as a line in the diff a
	# reviewer sees. A new symbol is an ordinary, permitted change; what
	# isn't permitted is a change that arrives unnoticed. The comparison runs
	# abidw on the library with the SAME scope flags that recorded the
	# baseline and then diffs corpus against corpus, instead of handing the
	# raw .so to abidiff as the second operand. abidiff has no --headers-dir
	# of its own, so a raw .so there is a corpus of a different shape from
	# the baseline and every type the baseline leaves out on purpose would
	# come back as a difference.
	abidiff_current=$(mktemp) || exit 1
	# shellcheck disable=SC2064
	trap "rm -f '$abidiff_current'" EXIT
	if ! abidw $ABIDW_SCOPE_FLAGS --out-file "$abidiff_current" "$SO"; then
		echo "check_public_abi: abidw failed on $SO; the corpus layer reported" \
			"nothing either way." >&2
		fail=1
		abidiff_current=""
	fi
	if [ -n "$abidiff_current" ]; then
	abidiff "$CORPUS_BASELINE" "$abidiff_current" || abidiff_rc=$?
	abidiff_rc=${abidiff_rc:-0}
	if [ $((abidiff_rc & 3)) -ne 0 ]; then
		echo "check_public_abi: abidiff failed to run (exit $abidiff_rc); the" \
			"corpus layer reported nothing either way." >&2
		fail=1
	elif [ $((abidiff_rc & 8)) -ne 0 ]; then
		echo "ABI BREAK: abidiff reports an incompatible change against $CORPUS_BASELINE." >&2
		fail=1
	elif [ $((abidiff_rc & 4)) -ne 0 ]; then
		echo "ABI CHANGE: abidiff reports a change against $CORPUS_BASELINE." \
			"Re-record it with \`make update_abi_baseline\` and commit the" \
			"result once the change is intended." >&2
		fail=1
	fi
	fi
fi

if [ "$fail" -eq 0 ]; then
	echo "check_public_abi: OK (exported ABI matches libccollections.so.$ABI_VERSION baseline)"
fi
exit "$fail"
