#!/bin/sh
# Every installed header must compile ON ITS OWN, under a strict -std=c11 with
# -pedantic-errors, with -Wall -Wextra -Werror, under both compilers.
#
# "On its own" catches a header that only works because some other header came
# first. "Strict c11" catches a header that expands a GNU extension at file
# scope: the macros of this library use statement expressions and __typeof__,
# so code that USES them needs -std=gnu11, but a caller who merely INCLUDES a
# header must not be forced into that mode. -pedantic-errors turns every
# construct that ISO C11 does not allow, such as `, ##__VA_ARGS__` or an empty
# translation unit, into an error, so that a header stays includable from a
# project that builds with -pedantic.
#
# The script compiles each header in two layouts.
#
# The source tree: -Iinclude and #include <NAME.h>, the way the library and
# its test suites include the headers.
#
# The installed layout: the headers in STAGE/include/ccollections/, as `make install` lays them out, reached the way
# an application reaches them: -ISTAGE/include and #include
# <ccollections/NAME.h>. Beside it sits an application directory that holds a
# header of its own for every public name (common.h, cstring.h, ...). Each
# probe compiles twice, with that directory first and last on the command line,
# and in each order the probe must get the library copy through
# <ccollections/NAME.h> and its own copy through <NAME.h>:
#   - an application header that a library header reaches fails with #error,
#     because only the probe itself sets the macro that the application
#     headers require;
#   - a library header that the probe's own <NAME.h> reaches instead of the
#     application copy leaves the marker of the application copy undefined,
#     and the probe fails with #error.
# This pins the property that the install relies on: an application may have
# its own common.h, and neither side ever gets the other's.
#
# Usage: check_public_headers_standalone.sh
#
# Run this from the root of the repository.
set -eu

INCLUDE_DIR=include
TMPDIR_=$(mktemp -d) || exit 1
trap 'rm -rf "$TMPDIR_"' EXIT

STRICT="-std=c11 -pedantic-errors -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64"

fail=0
checked=0

headers=$(ls "$INCLUDE_DIR"/*.h)
if [ -z "$headers" ]; then
	echo "check_public_headers_standalone: no header under $INCLUDE_DIR/" >&2
	exit 1
fi

STAGE="$TMPDIR_/stage/include"
APP="$TMPDIR_/app"
mkdir -p "$STAGE/ccollections" "$APP"
for hdr in $headers; do
	cp "$hdr" "$STAGE/ccollections/"
	b=$(basename "$hdr")
	m=$(basename "$hdr" .h)
	{
		printf '#ifndef CCOL_CHECK_APPLICATION_INCLUDE\n'
		printf '#error "a library header reached the application header %s"\n' "$b"
		printf '#endif\n'
		printf '#define APP_OWN_%s 1\n' "$m"
	} > "$APP/$b"
done

compile() {
	# compile <label> <compiler> <source> <flags...>
	_label="$1"
	_cc="$2"
	_src="$3"
	shift 3
	# shellcheck disable=SC2086
	if ! "$_cc" $STRICT "$@" -c "$_src" -o "$TMPDIR_/probe.o" \
		2>"$TMPDIR_/err"; then
		echo "check_public_headers_standalone: FAIL ($_cc, $_label)" >&2
		sed 's/^/    /' "$TMPDIR_/err" >&2
		fail=1
	fi
	checked=$((checked + 1))
}

# installed_probe <output.c> <header names...>: a translation unit that
# includes the library headers from the installed layout, then every
# application header of a public name, and checks that each one of those is
# the application copy.
installed_probe() {
	_out="$1"
	shift
	: > "$_out"
	for _n in "$@"; do
		printf '#include <ccollections/%s>\n' "$_n" >> "$_out"
	done
	printf '#define CCOL_CHECK_APPLICATION_INCLUDE 1\n' >> "$_out"
	for _h in $headers; do
		_m=$(basename "$_h" .h)
		printf '#include <%s.h>\n#ifndef APP_OWN_%s\n' "$_m" "$_m" >> "$_out"
		printf '#error "<%s.h> did not reach the application copy"\n#endif\n' \
			"$_m" >> "$_out"
	done
	printf 'int main(void){return 0;}\n' >> "$_out"
}

for cc in ${CCOL_HEADER_CHECK_CCS:-gcc clang}; do
	command -v "$cc" >/dev/null 2>&1 || {
		echo "check_public_headers_standalone: $cc not installed, skipping it."
		continue
	}
	all_names=""
	for hdr in $headers; do
		base=$(basename "$hdr")
		all_names="$all_names $base"
		printf '#include <%s>\nint main(void){return 0;}\n' "$base" \
			> "$TMPDIR_/probe.c"
		compile "source tree: $base" "$cc" "$TMPDIR_/probe.c" \
			-I"$INCLUDE_DIR"
		installed_probe "$TMPDIR_/inst.c" "$base"
		compile "installed layout, application -I first: $base" "$cc" \
			"$TMPDIR_/inst.c" -I"$APP" -I"$STAGE"
		compile "installed layout, application -I last: $base" "$cc" \
			"$TMPDIR_/inst.c" -I"$STAGE" -I"$APP"
	done

	# All of them together, in one translation unit, must also work.
	: > "$TMPDIR_/all.c"
	for hdr in $headers; do
		echo "#include <$(basename "$hdr")>" >> "$TMPDIR_/all.c"
	done
	echo 'int main(void){return 0;}' >> "$TMPDIR_/all.c"
	compile "source tree: all headers together" "$cc" "$TMPDIR_/all.c" \
		-I"$INCLUDE_DIR"
	# shellcheck disable=SC2086
	installed_probe "$TMPDIR_/inst_all.c" $all_names
	compile "installed layout, application -I first: all headers" "$cc" \
		"$TMPDIR_/inst_all.c" -I"$APP" -I"$STAGE"
	compile "installed layout, application -I last: all headers" "$cc" \
		"$TMPDIR_/inst_all.c" -I"$STAGE" -I"$APP"
done

# A run that compiled nothing must not report success. Without this, a missing
# compiler or an empty glob passes in silence.
if [ "$checked" -eq 0 ]; then
	echo "check_public_headers_standalone: examined no header at all; refusing" \
		"to report success." >&2
	exit 1
fi

if [ "$fail" -eq 0 ]; then
	echo "check_public_headers_standalone: OK ($checked compilations, strict" \
		"-std=c11 -pedantic-errors)"
fi
exit "$fail"
