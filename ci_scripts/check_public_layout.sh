#!/bin/sh
# Fails when the layout of a public struct or union on this system differs
# from the recorded LP64 layout in abi/public_layout.lp64.
#
# The structural layer of check_public_abi.sh reads ELF debug information with
# libabigail, so it only runs where both exist. On macOS (Mach-O) and OpenBSD
# (no libabigail) we rely instead on every public record having the same layout
# on every LP64 target, and that holds as long as each member is a fixed-width
# integer, size_t, int, unsigned, _Bool, an enum, a pointer or another public
# record. A member whose type has a width that depends on the system (long
# double, a pthread type, time_t, off_t, a struct from a system header) breaks
# this on some target and on no other, so this script asks the local compiler
# for the layout and compares it with the record.
#
# The layout comes from the compiler itself (clang's -fdump-record-layouts), so
# nothing here repeats a size or an offset by hand. For every public record the
# file holds its size, its alignment and the offset of every member, nested
# members included. It holds no type spelling, because different compiler
# versions spell the same type differently; libabigail compares the types on
# the Linux jobs.
#
# A record is public when its tag (or, for an anonymous record, the name of
# the typedef that names it) carries one of the library's namespace prefixes.
# A name starting with an underscore is an internal record that an installed
# header has to declare, like the synchronisation wrappers of common.h which
# wrap pthread types and so differ by system on purpose, and the script leaves
# those out.
#
# Usage: ci_scripts/check_public_layout.sh [--update]
#   Run it from the root of the repository. CLANG names the compiler (default
#   clang) and LAYOUT_CFLAGS adds flags (a --target, for example). --update
#   writes the record instead of comparing against it.

set -eu

CLANG=${CLANG:-clang}
LAYOUT_CFLAGS=${LAYOUT_CFLAGS:-}
RECORD=abi/public_layout.lp64
UPDATE=0
if [ "${1:-}" = "--update" ]; then UPDATE=1; fi

if ! "$CLANG" --version 2>/dev/null | grep -qi clang; then
	echo "check_public_layout: $CLANG is not clang; set CLANG to a clang" \
		"compiler, because the layout dump is a clang feature." >&2
	exit 1
fi

# The record is for LP64 targets. An ILP32 target has a layout of its own,
# and the structural layer of check_public_abi.sh compares it on the Linux jobs.
# shellcheck disable=SC2086 # LAYOUT_CFLAGS is a list of words.
if ! printf '' | "$CLANG" $LAYOUT_CFLAGS -dM -E -x c - | grep -q '__LP64__'; then
	echo "check_public_layout: skipped, $CLANG does not target an LP64 system."
	exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

HEADERS=$(ls include/*.h)
if [ -z "$HEADERS" ]; then
	echo "check_public_layout: no header found under include/" >&2
	exit 1
fi
for h in $HEADERS; do
	printf '#include "%s"\n' "${h#include/}"
done >"$TMP/all.c"

# shellcheck disable=SC2086
if ! "$CLANG" $LAYOUT_CFLAGS -Iinclude -std=gnu11 -D_FILE_OFFSET_BITS=64 -fsyntax-only \
	-Xclang -fdump-record-layouts-complete "$TMP/all.c" >"$TMP/dump.txt" \
	2>"$TMP/err.txt"; then
	cat "$TMP/err.txt" >&2
	echo "check_public_layout: $CLANG could not compile the public headers" >&2
	exit 1
fi

# Written for POSIX awk, because mawk and the one-true-awk of the BSDs and
# macOS don't have the three-argument match().
awk '
# The name of the typedef that ends the anonymous record whose keyword is at
# line ln, column col of file. Comments are skipped, and the braces of the
# record are counted to its end; the identifier before the next ";" is the
# name. An anonymous member of another record has no such identifier, and
# the function gives "".
function typedef_name(file, ln, col,    line, n, i, c, depth, started, incmt, tail, rest) {
	n = 0; depth = 0; started = 0; incmt = 0; tail = ""
	while ((getline line < file) > 0) {
		n++
		if (n < ln) continue
		i = (n == ln) ? col : 1
		for (; i <= length(line); i++) {
			c = substr(line, i, 1)
			if (incmt) {
				if (c == "*" && substr(line, i + 1, 1) == "/") { incmt = 0; i++ }
				continue
			}
			if (c == "/" && substr(line, i + 1, 1) == "*") { incmt = 1; i++; continue }
			if (c == "/" && substr(line, i + 1, 1) == "/") break
			if (started && depth == 0) {
				if (c == ";") {
					close(file)
					rest = tail
					gsub(/[^A-Za-z0-9_]+/, " ", rest)
					sub(/ +$/, "", rest)
					sub(/^.* /, "", rest)
					return rest
				}
				tail = tail c
				continue
			}
			if (c == "{") { depth++; started = 1 }
			else if (c == "}") depth--
		}
		if (started && depth == 0) tail = tail " "
	}
	close(file)
	return ""
}

function public(name) {
	if (name ~ /^_/) return 0
	if (name ~ /^(cbinarymap|c_message_t|cthread_pool)$/) return 1
	return name ~ /^(ccol_|CCOL_|ccollections_|cvec|cvector|chmap|chashmap|cbmap|cbstmap|cstr|cstring|csort|cjson|cyaml|clog|clru|cmap|cmempool|cthreadcomm|cthreadpool|ctpool|chttp|ctls|citer)/
}

# "(unnamed at include/x.h:12:9)" or "(anonymous struct at ...)" is a record
# without a tag; the location tells where its declaration starts.
function anon_loc(s,    t) {
	if (!match(s, /\((unnamed|anonymous)[a-z ]* at [^)]*\)/)) return ""
	t = substr(s, RSTART, RLENGTH)
	sub(/^.* at /, "", t)
	sub(/\)$/, "", t)
	return t
}

function flush(    i) {
	if (keep) {
		nrec++
		seq++
		print name "\t" seq "\trecord " kind " " name " sizeof=" size " align=" align
		for (i = 1; i <= nf; i++) { seq++; print name "\t" seq "\t" field[i] }
	}
	keep = 0; nf = 0; inrec = 0
}

/^\*\*\* Dumping AST Record Layout/ { flush(); inrec = 1; header = 1; next }

inrec && header && /\|/ {
	header = 0
	s = $0
	sub(/^[^|]*\| /, "", s)
	kind = s; sub(/ .*$/, "", kind)
	rest = s; sub(/^[a-z]+ /, "", rest)
	loc = anon_loc(rest)
	if (loc != "") {
		if (loc !~ /^include\//) next
		split(loc, p, ":")
		name = typedef_name(p[1], p[2] + 0, p[3] + 0)
		if (name == "") next
	} else {
		name = rest
		sub(/ +$/, "", name)
	}
	keep = public(name)
	next
}

inrec && /\[sizeof=/ {
	size = $0; sub(/^.*sizeof=/, "", size); sub(/[^0-9].*$/, "", size)
	align = $0; sub(/^.*[ ,]align=/, "", align); sub(/[^0-9].*$/, "", align)
	flush()
	next
}

inrec && /\|/ {
	off = $0; sub(/\|.*$/, "", off); gsub(/ /, "", off)
	s = $0; sub(/^[^|]*\|/, "", s)
	ind = s; sub(/[^ ].*$/, "", ind)
	depth = (length(ind) - 1) / 2
	sub(/ +$/, "", s)
	if (anon_loc(s) != "" && s ~ /\)$/) fname = "(anonymous)"
	else { fname = s; sub(/^.* /, "", fname) }
	nf++
	field[nf] = "  " off " " depth " " fname
	next
}

END {
	flush()
	if (nrec == 0) exit 3
}
' "$TMP/dump.txt" >"$TMP/rows.txt" || {
	echo "check_public_layout: no public record found in the layout dump;" \
		"the output of $CLANG has a form that this script does not read." >&2
	exit 1
}

LC_ALL=C sort -t "$(printf '\t')" -k1,1 -k2,2n "$TMP/rows.txt" | cut -f3- \
	>"$TMP/layout.txt"

if ! grep -q '^record struct ccol_memmgmt_procs_t sizeof=32 align=8$' \
	"$TMP/layout.txt"; then
	echo "check_public_layout: the dump does not hold ccol_memmgmt_procs_t" \
		"with its LP64 layout, so the parse is not trustworthy." >&2
	exit 1
fi

count=$(grep -c '^record ' "$TMP/layout.txt")

if [ "$UPDATE" = 1 ]; then
	{
		echo "# The layout of every public struct and union on an LP64 target:"
		echo "# size, alignment, and the offset, nesting depth and name of every"
		echo "# member. ci_scripts/check_public_layout.sh writes this file with"
		echo "# --update and compares the layout of the system against it."
		cat "$TMP/layout.txt"
	} >"$RECORD"
	echo "check_public_layout: recorded $count public records in $RECORD"
	exit 0
fi

if [ ! -f "$RECORD" ]; then
	echo "check_public_layout: $RECORD is missing" >&2
	exit 1
fi

grep -v '^#' "$RECORD" >"$TMP/expected.txt"
if ! diff -u "$TMP/expected.txt" "$TMP/layout.txt" >"$TMP/diff.txt"; then
	cat "$TMP/diff.txt"
	echo "check_public_layout: the layout of the public records on this system" \
		"differs from $RECORD (lines with - are recorded, + are this system)." >&2
	exit 1
fi
echo "check_public_layout: OK ($count public records match $RECORD)"
