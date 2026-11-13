#!/bin/sh
# Checks that no other object in a process can take over a call that should
# reach this library, which ELF allows by default and which no compiler or
# linker reports: The dynamic loader binds every reference to the first
# definition of the name that it finds, in the program, in an LD_PRELOAD
# object or in any library loaded ahead of ours.
#
# Two link options of the library close that, and this script proves both on
# the built artifact, because a flag in the wrong variable disappears without
# an error:
#
#   - The version script gives every exported symbol the node
#     CCOL_<abi version>. A program built against the library records
#     name@CCOL_<abi> for each symbol it uses, and the loaders of glibc and
#     FreeBSD bind that reference only to a definition with the same node.
#     A versioned library that happens to export the same name can't take the
#     call. An object with no version information at all still can: glibc
#     and FreeBSD accept an unversioned definition for a versioned reference,
#     for the sake of old binaries. musl and OpenBSD ignore versions, so on
#     those systems this half isn't checked at run time.
#   - -Bsymbolic-functions binds the library's own calls to its exported
#     functions inside the library. No relocation of the library may then name
#     one of its own exported functions, and an impostor preloaded ahead of it
#     catches none of those calls, on every ELF loader.
#
# The run-time half preloads an impostor that defines every function the
# library exports and aborts when one of them runs. A probe that first checks
# the preload took effect (a direct call from the program must reach the
# impostor) then drives the library through dlsym(), so only the library's
# internal calls are left to reach the impostor. Without that control a
# loader that ignored LD_PRELOAD would pass the check while testing nothing.
#
# macOS needs neither option: A Mach-O program records the library of every
# symbol it imports, and a dylib binds its own calls at link time. The script
# checks ELF objects only.
#
# Run this from the root of the repository after `make`;
# `make check_symbol_binding` and CI both use it. CC picks the compiler and
# DL_LIBS the libraries dlopen() needs (-ldl for glibc before 2.34).
set -eu

SO="${1:-}"
ABI="${2:-}"
[ -n "$SO" ] && [ -n "$ABI" ] || { echo "usage: $0 <shared-object> <abi-version>" >&2; exit 2; }
[ -e "$SO" ] || { echo "check_symbol_binding: $SO not built; run make first" >&2; exit 2; }
CC=${CC:-cc}
DL_LIBS=${DL_LIBS:-}
NODE="CCOL_$ABI"

. ci_scripts/object_format.sh
if ! FORMAT=$(ccol_object_format "$SO"); then
  echo "check_symbol_binding: $SO is neither an ELF nor a Mach-O file" >&2
  exit 2
fi
if [ "$FORMAT" != elf ]; then
  echo "check_symbol_binding: skipped, $SO is a Mach-O library (two-level namespace)"
  exit 0
fi
NM=${NM:-unused}
. ci_scripts/gnu_binutils.sh

# The dynamic symbol table of an object. OpenBSD's base readelf (GNU 2.17)
# has no --dyn-syms, so we print every table with -s and keep the .dynsym one.
dynsyms() {
  "$READELF" -sW "$1" | awk '
    /^Symbol table/ { keep = ($0 ~ /\.dynsym/); next }
    keep && $1 ~ /^[0-9]+:$/'
}

fail=0
TMP=$(mktemp -d "${TMPDIR:-/tmp}/ccol_binding.XXXXXX")
trap 'rm -rf "$TMP"' EXIT INT TERM

# --- 1. Every exported definition carries the node --------------------------
# readelf prints a defined, versioned symbol as name@@NODE. The node's own
# absolute symbol and the _init/_fini that some C libraries' crti.o export are
# left out.
dynsyms "$SO" |
  awk '$7 != "UND" && $5 != "LOCAL" && $8 != "" {print $4, $8}' > "$TMP/syms"
[ -s "$TMP/syms" ] || { echo "check_symbol_binding: no exported symbol found in $SO" >&2; exit 1; }
# The library defines no name that begins with an underscore, so such a name
# in the export table comes from the toolchain (a compiler runtime helper
# linked in from a static archive) and lets other objects bind to our copy.
awk '{ n = $2; sub(/@.*/, "", n) } n ~ /^_/ && n != "_init" && n != "_fini" { print "  " n }' \
  "$TMP/syms" > "$TMP/underscored"
if [ -s "$TMP/underscored" ]; then
  echo "check_symbol_binding: the library exports names that begin with an underscore (is -Wl,--exclude-libs,ALL missing?):" >&2
  cat "$TMP/underscored" >&2
  fail=1
fi
awk -v node="$NODE" '
  { name = $2; sub(/@.*/, "", name) }
  name == node || name == "_init" || name == "_fini" { next }
  $2 !~ ("@@" node "$") { print "  " $2; bad = 1 }
  END { exit bad }' "$TMP/syms" > "$TMP/unversioned" || {
  echo "check_symbol_binding: exported symbols without the version node $NODE:" >&2
  cat "$TMP/unversioned" >&2
  fail=1
}
awk '$1 == "FUNC" { n = $2; sub(/@.*/, "", n); print n }' "$TMP/syms" |
  grep -vxE '_init|_fini' | sort -u > "$TMP/funcs"
[ -s "$TMP/funcs" ] || { echo "check_symbol_binding: no exported function found in $SO" >&2; exit 1; }

# --- 2. No library it depends on defines one of its names --------------------
# A name that libc, libssl or another dependency also exports would be bound
# to whichever definition the loader meets first, version node or not when
# the other definition has no versions.
# Linux, FreeBSD and musl print "name => path (address)", OpenBSD a table
# whose last column is the path, and every format names the library itself as
# well, so we keep the last absolute path of each line except our own.
# FreeBSD's ldd looks a bare file name up as a library name, so it gets the
# absolute path.
SO_ABS="$(cd "$(dirname "$SO")" && pwd)/$(basename "$SO")"
ldd "$SO_ABS" 2>/dev/null | awk -v self="$(basename "$SO")" '
  { p = ""; for (i = 1; i <= NF; i++) if ($i ~ /^\//) p = $i; sub(/:$/, "", p) }
  p != "" && p !~ ("/" self "$") { print p }' | sort -u > "$TMP/deps"
[ -s "$TMP/deps" ] || { echo "check_symbol_binding: ldd found no dependency of $SO" >&2; exit 1; }
: > "$TMP/depsyms"
while read -r dep; do
  [ -f "$dep" ] || continue
  dynsyms "$dep" |
    awk '$7 != "UND" && $8 != "" {n = $8; sub(/@.*/, "", n); print n}' >> "$TMP/depsyms"
done < "$TMP/deps"
sort -u -o "$TMP/depsyms" "$TMP/depsyms"
awk '{n = $2; sub(/@.*/, "", n); print n}' "$TMP/syms" | grep -vxE "_init|_fini|$NODE" | sort -u > "$TMP/ours"
if comm -12 "$TMP/ours" "$TMP/depsyms" > "$TMP/shared"; [ -s "$TMP/shared" ]; then
  echo "check_symbol_binding: these exported names are also defined by a library this one depends on:" >&2
  sed 's/^/  /' "$TMP/shared" >&2
  fail=1
fi

# --- 3. No relocation names one of the library's own functions --------------
"$READELF" -rW "$SO" | awk 'NF >= 5 && $1 ~ /^[0-9a-f]+$/ {n = $5; sub(/@.*/, "", n); print n}' |
  sort -u > "$TMP/relocs"
if comm -12 "$TMP/funcs" "$TMP/relocs" > "$TMP/self"; [ -s "$TMP/self" ]; then
  echo "check_symbol_binding: the library reaches these of its own functions through a relocation, so another object can take the call over (is -Bsymbolic-functions missing?):" >&2
  sed 's/^/  /' "$TMP/self" >&2
  fail=1
fi

# --- 4. Run time ------------------------------------------------------------
# The impostor defines every exported function. Each one reports its name and
# aborts, so the probe dies the moment any call reaches it.
{
  echo '#include <stdio.h>'
  echo '#include <stdlib.h>'
  printf '%s\n' 'static void hit(const char *n) { fprintf(stderr, "impostor called: %s\n", n); abort(); }'
  while read -r f; do
    echo "void $f(void) { hit(\"$f\"); }"
  done < "$TMP/funcs"
} > "$TMP/impostor.c"
printf 'CCOL_IMPOSTOR_1 {\n  global: *;\n};\n' > "$TMP/impostor.map"
$CC -shared -fPIC -w -o "$TMP/libimpostor_plain.so" "$TMP/impostor.c"
$CC -shared -fPIC -w -o "$TMP/libimpostor_versioned.so" "$TMP/impostor.c" \
  -Wl,--version-script="$TMP/impostor.map"

LIBDIR=$(cd "$(dirname "$SO")" && pwd)
cat > "$TMP/probe.c" <<'EOF'
/* argv[1]: "direct" makes one call through the program's own reference;
 * "library" drives the library only through dlsym() pointers, so every call
 * that can still reach an impostor is a call the library makes itself. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "cjson.h"

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "direct") == 0) {
    char *err = NULL;
    cjson j = cjson_parse_mp("[1]", &err, NULL);
    cjson__destroy(j);
    puts("direct call reached the library");
    return 0;
  }
  void *h = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
  if (h == NULL) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 3;
  }
  __typeof__(&cjson_parse_mp) parse =
      (__typeof__(&cjson_parse_mp))dlsym(h, "cjson_parse_mp");
  __typeof__(&cjson_serialize) ser =
      (__typeof__(&cjson_serialize))dlsym(h, "cjson_serialize");
  __typeof__(&cjson_serialize_free_mp) sfree =
      (__typeof__(&cjson_serialize_free_mp))dlsym(h, "cjson_serialize_free_mp");
  __typeof__(&cjson__destroy) destroy =
      (__typeof__(&cjson__destroy))dlsym(h, "cjson__destroy");
  if (!parse || !ser || !sfree || !destroy) {
    fprintf(stderr, "dlsym: %s\n", dlerror());
    return 3;
  }
  char *err = NULL;
  cjson j = parse("{\"a\": [1, 2.5, \"x\", {\"b\": null}], \"c\": true}", &err,
                  NULL);
  if (j == NULL) {
    fprintf(stderr, "parse failed: %s\n", err ? err : "?");
    return 3;
  }
  char *s = ser(j);
  int ok = s != NULL && strcmp(s, "{\"a\":[1,2.5,\"x\",{\"b\":null}],\"c\":true}") == 0;
  sfree(s, NULL);
  destroy(j);
  if (!ok) {
    fprintf(stderr, "serialize gave the wrong text\n");
    return 3;
  }
  puts("library calls stayed in the library");
  return 0;
}
EOF
if ! dynsyms "$SO" | awk '{print $8}' | grep -qE '^cjson_parse_mp(@|$)'; then
  echo "check_symbol_binding: cjson is not in this build; the run-time half needs it" >&2
  exit 1
fi
$CC -std=gnu11 -Iinclude -o "$TMP/probe" "$TMP/probe.c" -L"$LIBDIR" -lccollections \
  -Wl,-rpath,"$LIBDIR" $DL_LIBS

run() { # run <expect: real|impostor> <preload> <mode>
  # The subshell waits for the probe (the exit keeps it from running the
  # probe in its own place), so the shell's report of the abort ("Abort trap"
  # on FreeBSD) goes into the captured stderr.
  if (LD_PRELOAD="$2" "$TMP/probe" "$3" "$LIBDIR/$(basename "$SO")"; exit $?) > "$TMP/out" 2> "$TMP/err"; then
    got=real
  elif grep -q '^impostor called:' "$TMP/err"; then
    got=impostor
  else
    got=error
  fi
  if [ "$got" != "$1" ]; then
    echo "check_symbol_binding: $3 run with $(basename "$2") preloaded: expected the $1 function, got $got" >&2
    sed 's/^/  /' "$TMP/out" "$TMP/err" >&2
    fail=1
  fi
}

# Control: The unversioned impostor does catch a call that the program binds
# by name, so the preload is in effect and the runs below test something.
run impostor "$TMP/libimpostor_plain.so" direct
# -Bsymbolic-functions: The library's own calls reach the library, whatever
# is preloaded.
run real "$TMP/libimpostor_plain.so" library
run real "$TMP/libimpostor_versioned.so" library

# Version node: An impostor with another node can't take the program's call.
# musl and OpenBSD ignore symbol versions, so the check means nothing there.
if "$READELF" -d "$SO" 2>/dev/null | grep -q 'libc\.musl'; then
  echo "check_symbol_binding: musl ignores symbol versions; the program-side check is skipped"
elif [ "$(uname -s)" = OpenBSD ]; then
  echo "check_symbol_binding: OpenBSD's ld.so ignores symbol versions; the program-side check is skipped"
else
  run real "$TMP/libimpostor_versioned.so" direct
fi

if [ "$fail" -ne 0 ]; then
  exit 1
fi
echo "check_symbol_binding: OK ($(wc -l < "$TMP/funcs" | tr -d ' ') exported functions, node $NODE)"
