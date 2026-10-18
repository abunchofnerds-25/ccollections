#!/bin/sh
# Checks the library the way an application meets it after `make install`. It
# installs into a temporary prefix, builds the README.md example with the flags
# pkg-config gives and runs it twice, once linked against the shared library
# and once against the static archive. At the end it runs `make uninstall` and
# checks that no file of the library is left in the prefix.
#
# The install uses PREFIX, not DESTDIR. On macOS the shared library records its
# install directory in its install name and a program finds the library through
# that name, so only a real prefix lets us prove that this works. On the ELF
# systems a program finds a library in a non-standard prefix through an rpath,
# as doc/building.md says, so the dynamic link adds one.
#
# The test suites compile the sources into each test binary, so none of them
# can see a defect of the install, like a header the install leaves out, a
# wrong pkg-config file or a wrong install name.
#
# Run it from the root of the repository; `make check_install` and CI both call
# it. MAKE names the make program (gmake on the BSDs and on macOS).
set -eu

MAKE="${MAKE:-make}"
CC="${CC:-cc}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
prefix="$tmp/prefix"
log="$tmp/make.log"

fail() {
	echo "check_install: $*" >&2
	exit 1
}

# SUDO= stops the install from asking for more rights, the prefix belongs to
# the caller.
if ! "$MAKE" -s install PREFIX="$prefix" SUDO= >"$log" 2>&1; then
	sed 's/^/  /' "$log" >&2
	fail "make install into $prefix failed"
fi

# The compiler also searches the standard directories, and a machine can have
# an older install there, so a build that succeeds says nothing about the
# headers in the prefix. That's why the installed header set has to be exactly
# the set of public headers.
want=$(cd include && ls -- *.h | sort)
have=$(cd "$prefix/include/ccollections" 2>/dev/null && ls -- *.h | sort) || have=""
[ "$want" = "$have" ] ||
	fail "the installed headers in $prefix/include/ccollections are not the public headers of include/"

pc=$(find "$prefix" -name ccollections.pc | head -n 1)
[ -n "$pc" ] || fail "the install wrote no ccollections.pc"
libdir=$(PKG_CONFIG_PATH="$(dirname "$pc")" pkg-config --variable=libdir ccollections) ||
	fail "pkg-config cannot read $pc"
[ "$libdir" = "$prefix/lib" ] ||
	fail "ccollections.pc names libdir $libdir, not $prefix/lib"
# The installed pkg-config directory goes first and any existing
# PKG_CONFIG_PATH after it, so the static link finds the pkg-config files of
# OpenSSL and zlib where the system keeps them as well.
PKG_CONFIG_PATH="$(dirname "$pc")${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

# The README.md example, between its first ```c line and the line that closes
# it.
awk '/^```c$/ { on = 1; next } on && /^```$/ { exit } on { print }' \
	README.md >"$tmp/hello.c"
[ -s "$tmp/hello.c" ] || fail "README.md holds no C example"
# The first line ends in a space because the example prints each number
# followed by a space.
expected=$(printf '74 88 91 \nalice is 31\nno carol')

cflags=$(pkg-config --cflags ccollections) || fail "pkg-config --cflags failed"
libs=$(pkg-config --libs ccollections) || fail "pkg-config --libs failed"

os=$(uname -s)
rpath=""
[ "$os" = Darwin ] || rpath="-Wl,-rpath,$libdir"

# shellcheck disable=SC2086
"$CC" -std=gnu11 $cflags "$tmp/hello.c" -o "$tmp/hello_shared" $libs $rpath \
	2>"$tmp/cc.err" || {
	sed 's/^/  /' "$tmp/cc.err" >&2
	fail "the example does not build against the installed shared library"
}
out=$(env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH "$tmp/hello_shared") ||
	fail "the example, linked against the shared library, did not run"
[ "$out" = "$expected" ] ||
	fail "the shared example printed '$out', not '$expected'"
# The program has to load the installed library, not one that an older install
# left in a standard directory. On macOS it records the library by its install
# name, elsewhere the loader resolves it through the rpath.
if [ "$os" = Darwin ]; then
	otool -L "$tmp/hello_shared" | grep -q "$libdir/libccollections\.[0-9]*\.dylib" ||
		fail "the example does not record $libdir/libccollections.N.dylib"
else
	# Linux and FreeBSD print "name => path" and OpenBSD a table whose
	# last column is the path, so the library's path is there either way.
	# We take the whole output first since OpenBSD's ldd runs the program,
	# and a grep that stops at the first match would kill it with SIGPIPE.
	loaded=$(ldd "$tmp/hello_shared")
	printf '%s\n' "$loaded" | grep -q "$libdir/libccollections" ||
		fail "the example does not load the library from $libdir"
fi

# The static link names the archive by its path, because when a shared library
# and an archive sit in the same directory every linker on these systems picks
# the shared library for -lccollections.
static_libs=$(pkg-config --static --libs ccollections) ||
	fail "pkg-config --static --libs failed"
static_libs=$(printf '%s\n' "$static_libs" | sed 's/-lccollections//')
# shellcheck disable=SC2086
"$CC" -std=gnu11 $cflags "$tmp/hello.c" -o "$tmp/hello_static" \
	"$libdir/libccollections.a" $static_libs 2>"$tmp/cc.err" || {
	sed 's/^/  /' "$tmp/cc.err" >&2
	fail "the example does not build against the installed archive"
}
out=$("$tmp/hello_static") ||
	fail "the example, linked against the archive, did not run"
[ "$out" = "$expected" ] ||
	fail "the static example printed '$out', not '$expected'"
if [ "$os" = Darwin ]; then
	if otool -L "$tmp/hello_static" | grep -q libccollections; then
		fail "the static example loads the shared library"
	fi
elif ldd "$tmp/hello_static" 2>/dev/null | grep -q libccollections; then
	fail "the static example loads the shared library"
fi

if ! "$MAKE" -s uninstall PREFIX="$prefix" SUDO= >"$log" 2>&1; then
	sed 's/^/  /' "$log" >&2
	fail "make uninstall from $prefix failed"
fi
left=$(find "$prefix" ! -type d)
if [ -n "$left" ]; then
	echo "check_install: make uninstall left these files:" >&2
	printf '%s\n' "$left" | sed 's/^/  /' >&2
	exit 1
fi

echo "check_install: OK (installed, built with pkg-config, ran shared and static, uninstalled)"
