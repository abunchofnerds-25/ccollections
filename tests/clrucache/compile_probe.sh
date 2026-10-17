#!/bin/sh
# Compile-time probe for the array refusal of the clrucache declaring macros.
#
# The cache macros copy a key or a value by assignment, and C can't assign an
# array, so an array type would reach the cache as a pointer to the caller's
# array and the cache would store the address instead of the elements. That's
# why clru_declare, clru_declare_scoped, clru_construct,
# clru_construct_scoped and clru_redeclare refuse an array key type and an
# array value type, with a message that says to wrap the array in a struct.
# The accept case pins that scalar, const, pointer and struct types compile,
# including a struct that holds an array.
#
# A reject case has to fail for the RIGHT reason, so every reject case
# carries the diagnostic it expects, matched against the compiler's own
# output.
#
# Usage: ./compile_probe.sh [compiler ...]   (default: GCC and clang)

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The in-tree headers by default. CCOL_PROBE_INCLUDE_DIR points the probe at
# another header tree, and that's how we check that the probe isn't vacuous:
# the guarantee lives in the headers, so you confirm that a case can fail by
# running it against a copy of them with the qualifier removed.
INCLUDE_DIR=${CCOL_PROBE_INCLUDE_DIR:-"$SCRIPT_DIR/../../include"}
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# The default list takes gcc, or else the newest versioned gccNN in PATH
# (FreeBSD installs GCC only under a versioned name such as gcc14).
probe_gcc() {
  if command -v gcc > /dev/null 2>&1; then
    echo gcc
    return
  fi
  found=$(IFS=:; for d in $PATH; do ls "$d" 2>/dev/null; done |
    grep -E '^gcc[0-9]+$' | sed 's/^gcc//' | sort -n | tail -1)
  if [ -n "$found" ]; then echo "gcc$found"; else echo gcc; fi
}

COMPILERS=${*:-"$(probe_gcc) clang"}
FLAGS="-std=gnu11 -Wall -Wextra -Werror -fsyntax-only -I$INCLUDE_DIR"

# A reject case matches the compiler's own wording, so we run the compiler in
# the C locale. There GCC quotes identifiers with plain apostrophes, and in
# other locales with typographic quotes.
LC_ALL=C
export LC_ALL

failures=0
checks=0

# $1: case name
# $2: "accept" or "reject"
# $3: for a reject case, an extended regular expression the compiler's output
#     must match; "-" for an accept case
# $4: body statements placed inside ccol_probe
# $5: optional file-scope text placed ahead of ccol_probe, for a case that
#     needs a callback of its own rather than a statement
probe() {
  name=$1
  expectation=$2
  expected_diag=$3
  body=$4
  prelude=${5:-}

  if [ "$expectation" = reject ] && [ "$expected_diag" = "-" ]; then
    echo "compile_probe: FAIL  $name (a reject case needs an expected diagnostic)"
    failures=$((failures + 1))
    return
  fi

  cat > "$WORK_DIR/probe.c" <<EOF
#include <stdlib.h>
#include <string.h>

#include "clrucache.h"

void ccol_probe(void);

$prelude

void ccol_probe(void) {
$body
}
EOF

  for cc in $COMPILERS; do
    command -v "$cc" > /dev/null 2>&1 || {
      printf 'compile_probe: SKIP  %-52s (%s not installed)\n' "$name" "$cc"
      continue
    }
    checks=$((checks + 1))
    if $cc $FLAGS "$WORK_DIR/probe.c" > "$WORK_DIR/out.txt" 2>&1; then
      result=accept
    else
      result=reject
    fi

    if [ "$result" != "$expectation" ]; then
      printf 'compile_probe: FAIL  %-52s (%s: expected %s, got %s)\n' \
        "$name" "$cc" "$expectation" "$result"
      sed 's/^/compile_probe:       /' "$WORK_DIR/out.txt"
      failures=$((failures + 1))
    elif [ "$expectation" = reject ] &&
         ! grep -Eq "$expected_diag" "$WORK_DIR/out.txt"; then
      printf 'compile_probe: FAIL  %-52s (%s: rejected, but not for the expected reason)\n' \
        "$name" "$cc"
      printf 'compile_probe:       expected to match: %s\n' "$expected_diag"
      sed 's/^/compile_probe:       /' "$WORK_DIR/out.txt"
      failures=$((failures + 1))
    else
      printf 'compile_probe: ok    %-52s (%s)\n' "$name" "$cc"
    fi
  done
}

ARRAY_KEY="clru: an array key type is not supported; wrap the array in a struct"
ARRAY_VAL="clru: an array value type is not supported; wrap the array in a struct"

probe 'clru_declare with an array value' reject "$ARRAY_VAL" '
  clru_declare(a, int, char[16]);'

probe 'clru_declare_scoped with an array key' reject "$ARRAY_KEY" '
  clru_declare_scoped(a, int[2], int);'

probe 'clru_construct with an array value' reject "$ARRAY_VAL" '
  clru_construct(a, int, int[2], 8, NULL, NULL, NULL);'

probe 'clru_construct_scoped with an array key' reject "$ARRAY_KEY" '
  clru_construct_scoped(a, char[8], int, 8, NULL, NULL, NULL);'

probe 'clru_redeclare with a const array value' reject "$ARRAY_VAL" '
  clru_cache c = NULL;
  (void)c;
  clru_redeclare(c, int, const char[16]);'

probe 'scalar, const, pointer and struct types still compile' accept - '
  struct ccol_probe_wrapped { int v[2]; };
  clru_construct(a, int, int, 8, NULL, NULL, NULL);
  clru_destroy(a);
  clru_construct(b, int, const double, 8, NULL, NULL, NULL);
  clru_destroy(b);
  clru_construct(c, char *, struct ccol_probe_wrapped, 8, NULL, NULL, NULL);
  clru_destroy(c);
  clru_construct_scoped(d, long, void *, 8, NULL, NULL, NULL);
  clru_declare(e, int, int);
  (void)e;'

if [ "$checks" -eq 0 ]; then
  echo 'compile_probe: FAIL  no compiler was available to run any check'
  exit 1
fi

if [ "$failures" -ne 0 ]; then
  echo "compile_probe: FAILED ($failures of $checks checks)"
  exit 1
fi

echo "compile_probe: SUCCESS ($checks checks passed)"
