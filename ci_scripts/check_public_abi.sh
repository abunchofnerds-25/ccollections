#!/bin/sh
# Compares the built shared library's exported ABI against a committed
# baseline, so that neither an accidental export nor an accidental removal can
# reach a release unnoticed.
#
# This cannot be checked by the test suites. Every suite compiles the .c files
# straight into its own binary, where static linking resolves a hidden symbol
# exactly as it resolves an exported one, so a library that exports nothing at
# all still passes the entire suite. Only the built .so carries the evidence.
#
# Two layers run, and they catch different mistakes:
#
#   1. The exported symbol set, read off the .so with readelf. Catches a new
#      symbol escaping through a missing `static` or a stray declaration
#      inside a public header's visibility block, and catches a symbol
#      disappearing, which breaks every already-linked application. This layer
#      needs nothing but binutils, so it always runs.
#   2. The full ABI corpus (function signatures, and the layout of every
#      public struct reachable from them), via libabigail's abidiff. Catches
#      what a name list structurally cannot: a parameter type changing, or a
#      field being added to a by-value public struct such as a config struct.
#      This layer needs libabigail installed and an architecture-matched
#      baseline, and reports that it is inactive when either is absent.
#
# Usage:
#   check_public_abi.sh <shared-object> <abi-version> [--update]
#
# --update rewrites the baselines from the supplied library instead of
# checking against them. Adding a symbol is an ordinary, ABI-compatible
# change, so the intended workflow for one is to run `make
# update_abi_baseline` and commit the result alongside the new API: the check
# exists to make the addition a deliberate, reviewable line in a diff rather
# than to forbid it.
set -eu

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
# The corpus baseline records real struct layouts and parameter sizes, both of
# which differ between ILP32 and LP64 and between one machine's calling
# convention and another's. It is therefore per-architecture, and a build for
# an architecture with no baseline of its own skips layer 2 rather than
# comparing against a baseline that was never about it.
ARCH=$(uname -m)
CORPUS_BASELINE="$ABI_DIR/libccollections.so.$ABI_VERSION.$ARCH.abi"

extract_symbols() {
	# Defined (not UND) GLOBAL/WEAK dynamic symbols, with any @VERSION suffix
	# stripped, which is exactly the set an application can link against.
	#
	# readelf's own status is checked rather than the pipeline's, which would
	# be sort's: a readelf that cannot read its input (not an ELF file, a
	# truncated artifact, a mismatched architecture) otherwise yields an empty
	# set and a success status, and an empty set is exactly what --update would
	# then write over the committed baseline. /bin/sh here is not guaranteed to
	# have pipefail, so the read and the filtering are separate steps.
	_raw=$(readelf --dyn-syms -W "$1") || return 1
	printf '%s\n' "$_raw" \
		| awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK"){print $8}' \
		| sed 's/@.*//' | sort -u
}

mkdir -p "$ABI_DIR"
if ! current=$(extract_symbols "$SO"); then
	echo "check_public_abi: could not read dynamic symbols from $SO." >&2
	exit 1
fi
# An empty set is never a legitimate answer for this library and is what both
# of the failure modes above produce, so it is refused before anything is
# compared or written.
if [ -z "$current" ]; then
	echo "check_public_abi: $SO exports no dynamic symbols; refusing to" >&2
	echo "                  compare or record an empty baseline." >&2
	exit 1
fi

# CCOL_MEMPOOL_COMPACT_LAYOUT changes cmempool's entry stride, and with it the
# layout of a struct the corpus baseline records, so a compact build genuinely
# differs from the recorded one. It is a supported build configuration rather
# than a defect, and the baseline describes the default, so this reports and
# stops instead of printing a page of struct-layout differences under an "ABI
# BREAK" heading.
#
# Read off the built library rather than from a make variable: the setting can
# arrive through EXTRA_CFLAGS, through a CFLAGS override, or by editing the
# header, and the library names the one it was built with precisely so that the
# question can be answered from the artifact.
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
		echo "# One symbol per line, sorted. Regenerate with"
		echo "# \`make update_abi_baseline\` and commit the result whenever the"
		echo "# public API gains a symbol. Removing a line from this file, or"
		echo "# changing an existing symbol's signature, breaks every"
		echo "# application already linked against this ABI version and"
		echo "# requires incrementing VERSION_MAJOR in the root Makefile."
		echo "$current"
	} > "$SYMS_BASELINE"
	echo "check_public_abi: wrote $SYMS_BASELINE ($(echo "$current" | wc -l | tr -d ' ') symbols)"

	if command -v abidw >/dev/null 2>&1; then
		abidw --out-file "$CORPUS_BASELINE" "$SO"
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

# Real temporary files rather than process substitution: this script runs
# under /bin/sh, which is dash on Debian and Ubuntu, where <(...) is a syntax
# error rather than a supported construct.
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
	# abidiff's exit status is a BIT MASK, not a boolean, so `if ! abidiff`
	# would be wrong: bits 0 and 1 (values 1 and 2) are abidiff's own error
	# and usage failures, which say nothing about the ABI and must not be
	# reported as if they did.
	#
	# Bit 3 (value 8, ABIDIFF_ABI_INCOMPATIBLE_CHANGE) is reserved for changes
	# abidiff can PROVE incompatible, which is a strictly smaller set than the
	# changes that actually break a compiled application, so it cannot be the
	# only failing case. Growing a public struct reports bit 2 (value 4,
	# ABIDIFF_ABI_CHANGE) alone: adding one size_t to cmap_pair takes it from
	# 16 to 24 bytes and relocates every later field of every public struct
	# embedding it, breaking every application built against the old header,
	# and abidiff exits 4. Bit 2 is therefore fatal here too.
	#
	# That makes this layer behave exactly like the symbol-name layer above:
	# any change to the recorded ABI, compatible or not, has to be re-recorded
	# with `make update_abi_baseline` and land as a reviewable line in the
	# diff. Adding a symbol stays an allowed, ordinary change; what is not
	# allowed is one arriving unnoticed.
	abidiff "$CORPUS_BASELINE" "$SO" || abidiff_rc=$?
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

if [ "$fail" -eq 0 ]; then
	echo "check_public_abi: OK (exported ABI matches libccollections.so.$ABI_VERSION baseline)"
fi
exit "$fail"
