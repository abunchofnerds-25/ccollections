#!/bin/sh
# This script compares the exported ABI of the built shared library against a
# committed baseline. An export that nobody intended, and a removal that nobody
# intended, therefore cannot reach a release without notice.
#
# The test suites cannot check this. Every suite compiles the .c files straight
# into its own binary. A static link there resolves a hidden symbol in the same
# way as an exported one. A library that exports nothing at all therefore still
# passes the whole suite. Only the built .so holds the evidence.
#
# Two layers run, and each one catches a different mistake:
#
#   1. The set of exported symbols, which readelf reads off the .so. This
#      catches a new symbol that escapes through a missing `static`, or
#      through a declaration that does not belong inside the visibility block
#      of a public header. It also catches a symbol that disappears, which
#      breaks every application that is already linked. This layer needs only
#      binutils, so it always runs.
#   2. The full ABI corpus. This holds the function signatures, and the layout
#      of every public struct that those signatures reach. abidiff from
#      libabigail does this work. This layer catches what a list of names
#      cannot: a parameter type that changes, or a new field in a public
#      struct that a caller passes by value, such as a config struct. This
#      layer needs libabigail on the machine, and a baseline for the same
#      architecture. It reports that it is not active when one of the two is
#      missing.
#
# Layer 2 works for each architecture on its own, because layout does. A change
# can leave every structure identical byte for byte on one target and break
# another. If you widen a member from size_t to unsigned long long, nothing
# changes on LP64, where the two types have the same width. On ILP32 the same
# change moves every later field. Only the baseline for the architecture that
# you build can see this. This is why the architecture comes from the ELF
# header of the library itself.
#
# Usage:
#   check_public_abi.sh <shared-object> <abi-version> [--update]
#
# CCOL_ABI_ARCH replaces the architecture name that comes from the ELF header.
# Use it for a target that this script does not yet know.
#
# --update writes the baselines again from the library that you give it. It
# does not compare against them. A new symbol is an ordinary change that keeps
# the ABI, so the correct steps for one are to run `make update_abi_baseline`
# and to commit the result together with the new API. The check exists to make
# each addition a deliberate line in a diff that a reviewer can see. It does
# not exist to forbid one.
set -eu

# The baseline of the symbols is a sorted list, and the order of sort(1)
# follows the locale. The C locale makes the file the same whoever records it.
LC_ALL=C
export LC_ALL

. ci_scripts/gnu_binutils.sh

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

ABI_DIR=abi
SYMS_BASELINE="$ABI_DIR/libccollections.so.$ABI_VERSION.symbols"

# The corpus baseline records real struct layouts and real parameter sizes.
# Both differ between ILP32 and LP64, and both differ between the calling
# convention of one machine and that of another. Each architecture therefore
# has its own baseline. A build for an architecture with no baseline of its own
# skips layer 2. It does not compare against a baseline that describes some
# other architecture.
#
# The architecture comes out of the ELF header of the library itself. It never
# comes from the machine that runs this script. A cross-build makes a library
# for one architecture on a host of another. A name that came from the host
# would therefore compare every cross-built library against the baseline of the
# host, and --update would write over that baseline. Both failures are silent.
# The comparison is against a real baseline for the wrong architecture, so it
# reports struct differences that look correct, and not an error.
elf_arch() {
	_hdr=$("$READELF" -h -W "$1") || return 1
	_class=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*Class:[[:space:]]*//p')
	_machine=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')
	_flags=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*Flags:[[:space:]]*//p')
	_osabi=$(printf '%s\n' "$_hdr" | sed -n 's/^[[:space:]]*OS\/ABI:[[:space:]]*//p')
	# The C library of the target declares every system type that the public
	# interface names, and its headers spell them differently: FreeBSD's
	# size_t is the typedef __size_t where glibc's is unsigned long, so
	# abidiff reports every such parameter as a change even where the size
	# and the layout are the same. A FreeBSD library therefore gets baselines
	# of its own, named with a "freebsd-" prefix. A Linux library carries the
	# System V or the GNU OS/ABI, and keeps the plain architecture name.
	_prefix=
	case "$_osabi" in
	*FreeBSD*) _prefix=freebsd- ;;
	esac
	printf '%s' "$_prefix"
	case "$_machine:$_class" in
	"Advanced Micro Devices X86-64:ELF64") echo x86_64 ;;
	"Intel 80386:ELF32") echo i386 ;;
	"AArch64:ELF64") echo aarch64 ;;
	# On 32-bit ARM, the ABI chooses the calling convention for a
	# floating-point argument, and the machine does not. The hard-float ABI
	# and the soft-float ABI are therefore two different ABIs, and each one
	# gets its own baseline.
	"ARM:ELF32")
		case "$_flags" in
		*hard-float*) echo armhf ;;
		*) echo armel ;;
		esac
		;;
	*) return 1 ;;
	esac
}

# CCOL_ABI_ARCH names the baseline directly. Use it for a target that this
# mapping does not yet cover.
ARCH="${CCOL_ABI_ARCH:-}"
if [ -z "$ARCH" ]; then
	# The assignment is a separate step from the declaration. With
	# `ARCH=$(elf_arch ...)`, the status of the assignment becomes the exit
	# status. A failure inside the function would then leave ARCH empty, and
	# `set -e` could not see it.
	if ! ARCH=$(elf_arch "$SO") || [ -z "$ARCH" ]; then
		echo "check_public_abi: could not identify the architecture of $SO from" >&2
		echo "                  its ELF header. Set CCOL_ABI_ARCH to name the" >&2
		echo "                  baseline explicitly if this is a target the" >&2
		echo "                  script does not yet map." >&2
		exit 1
	fi
fi
CORPUS_BASELINE="$ABI_DIR/libccollections.so.$ABI_VERSION.$ARCH.abi"

# The corpus records the PUBLIC interface, and not every type that the debug
# information of the library happens to reach.
#
# --headers-dir names the installed headers, so a type counts as public when
# one of those headers declares it. --drop-private-types then leaves every
# other type out. Without the pair, abidw descends into the DEFINITION of each
# opaque struct, which lives in a .c file, and records whatever that definition
# contains. For this library that pulls in glibc's pthread_mutex_t and
# pthread_cond_t, because the queue and loop structs embed them.
#
# Those types are not part of the contract with a consumer. No installed
# header names one, and a consumer only ever holds a pointer to the opaque
# struct around them. Recording them made the corpus a property of the
# TOOLCHAIN rather than of the interface: GCC emits the array-subrange index
# type of their internal char arrays as `unsigned int` and Clang emits
# `__ARRAY_SIZE_TYPE__`, so a baseline recorded with one compiler failed
# against a build from the other with "type size hasn't changed" on every
# line. That is a difference in the debug information of a third party and not
# an ABI difference, and it made the gate unusable with Clang on i386.
#
# Dropping them loses no coverage that this gate is for. Every type that a
# consumer can name, size or lay out is declared in an installed header, so
# --headers-dir keeps all of them. That includes the structs that a public
# MACRO computes a size or an offset from, which are the ones the gate exists
# to watch.
ABIDW_SCOPE_FLAGS="--drop-private-types --headers-dir include"

extract_symbols() {
	# This takes the GLOBAL and WEAK dynamic symbols that are defined, which
	# means the ones that are not UND. It removes any @VERSION suffix. The
	# result is exactly the set that an application can link against.
	#
	# The script checks the status of readelf and not the status of the
	# pipeline, which would be the status of sort. readelf can fail to read
	# its input when the file is not an ELF file, when it is truncated, or
	# when it is for another architecture. Without this check, such a failure
	# gives an empty set and a success status. An empty set is exactly what
	# --update would then write over the committed baseline. /bin/sh here does
	# not always have pipefail, so the read and the filter are separate
	# steps.
	#
	# _init and _fini are left out. They are the entry points of the
	# initialisation and termination sections, and the C runtime start files
	# (crti.o) define them, not this library. FreeBSD's start files give them
	# default visibility, so every FreeBSD shared object exports them;
	# glibc's keep them hidden.
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
# An empty set is never a correct answer for this library, and both of the
# failures above give one. The script therefore refuses an empty set before it
# compares anything and before it writes anything.
if [ -z "$current" ]; then
	echo "check_public_abi: $SO exports no dynamic symbols; refusing to" >&2
	echo "                  compare or record an empty baseline." >&2
	exit 1
fi

# CCOL_MEMPOOL_COMPACT_LAYOUT changes the entry stride of cmempool. It
# therefore changes the layout of a struct that the corpus baseline records. A
# compact build really does differ from the recorded one. It is a supported
# build configuration and not a defect, and the baseline describes the default
# build. This script therefore reports the situation and stops. It does not
# print a page of struct layout differences under an "ABI BREAK" heading.
#
# The script reads this setting off the built library and not from a make
# variable. The setting can come in through EXTRA_CFLAGS, through a CFLAGS
# override, or from an edit to the header. The library names the setting that
# it was built with, so that you can answer this question from the artifact.
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

# This uses real temporary files and not process substitution. The script runs
# under /bin/sh. On Debian and Ubuntu that shell is dash, where <(...) is a
# syntax error and not a supported construct.
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
if ! command -v abidiff >/dev/null 2>&1; then
	echo "check_public_abi: NOTE - abidiff not installed, so signature and" \
		"struct-layout checking did not run (install abigail-tools to enable it)."
elif [ ! -f "$CORPUS_BASELINE" ]; then
	echo "check_public_abi: NOTE - no $ARCH corpus baseline at $CORPUS_BASELINE," \
		"so signature and struct-layout checking did not run" \
		"(run \`make update_abi_baseline\` on this architecture to create it)."
else
	# The exit status of abidiff is a BIT MASK and not a true or false value.
	# `if ! abidiff` is therefore wrong. Bit 0 and bit 1, with the values 1
	# and 2, are the error and usage failures of abidiff itself. They say
	# nothing about the ABI, and this script must not report them as if they
	# did.
	#
	# Bit 3, with the value 8 and the name ABIDIFF_ABI_INCOMPATIBLE_CHANGE,
	# is only for a change that abidiff can PROVE is incompatible. That set
	# is smaller than the set of changes that really break a compiled
	# application, so it cannot be the only case that fails. A public struct
	# that grows reports bit 2 alone, with the value 4 and the name
	# ABIDIFF_ABI_CHANGE. For example, one more size_t in cmap_pair takes it
	# from 16 to 24 bytes. It moves every later field of every public struct
	# that holds a cmap_pair, and so it breaks every application that was
	# built against the old header. abidiff exits 4 for that change. Bit 2 is
	# therefore also fatal here.
	#
	# This makes this layer behave in the same way as the symbol name layer
	# above. You must record any change to the ABI again with `make
	# update_abi_baseline`, whether the change is compatible or not, and it
	# must appear as a line in the diff that a reviewer sees. A new symbol
	# stays an ordinary, permitted change. What is not permitted is a change
	# that arrives without notice.
	# The comparison runs abidw on the library with the SAME scope flags that
	# recorded the baseline, and then diffs corpus against corpus. It does not
	# hand the raw .so to abidiff as the second operand. abidiff has no
	# --headers-dir of its own, so a raw .so there is a corpus of a different
	# shape from the baseline, and every type that the baseline deliberately
	# leaves out comes back as a difference.
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
