#!/bin/sh
# Compile-time probe for the rule that governs every cmap_pair the map hands
# back: cbmap_get_elem_ref()'s out-parameter and the key_pair/val_pair an
# iterator carries.
#
# The map keeps one {ptr, size} accessor per entry and reports its address
# through all three. The two fields describe one another: size describes the
# string or object ptr currently points at, and the bytes themselves belong to
# the map. Assigning to either field leaves the other describing something
# else, so the next read of that entry (a get_elem_copy, an iterator,
# cbmap_get) walks the wrong length; and a pointer the map did not allocate is
# one it cannot own.
#
# Each of those surfaces therefore names the accessor through a const
# cmap_pair *, which makes those assignments a compile error while leaving
# every legitimate use (read ptr and size, write to the bytes ptr addresses
# within size) available. This script asserts exactly that, under every
# compiler it is given, because the guarantee is a property of the type and no
# run-time test can observe it.
#
# A reject case additionally has to fail for the RIGHT reason: a probe that
# happens to be rejected over an unrelated mistake (a missing companion type
# variable, a typo) examines nothing while reading as a pass. Every reject
# case therefore carries the diagnostic it expects, matched against the
# compiler's own output, and a rejection that does not match is a failure.
#
# Usage: ./compile_probe.sh [compiler ...]   (default: gcc clang)

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The in-tree headers by default. CCOL_PROBE_INCLUDE_DIR points the probe at
# another header tree instead, which is how its own non-vacuity is checked:
# the guarantee lives in the headers, so the way to confirm a case can fail is
# to run it against a copy of them with the qualifier taken back out.
INCLUDE_DIR=${CCOL_PROBE_INCLUDE_DIR:-"$SCRIPT_DIR/../../include"}
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

COMPILERS=${*:-"gcc clang"}
FLAGS="-std=gnu11 -Wall -Wextra -Werror -fsyntax-only -I$INCLUDE_DIR"

# A reject case matches the compiler's own wording, so the compiler is run in
# the C locale: GCC quotes identifiers with plain apostrophes there and with
# typographic quotes elsewhere.
LC_ALL=C
export LC_ALL

failures=0
checks=0

# Diagnostics shared by several cases. GCC names the member being assigned;
# Clang names the const-qualified object the member is reached through, so
# each pattern carries both spellings.
ASSIGN_PTR="assignment of member 'ptr' in read-only object|cannot assign to .* with const-qualified type"
ASSIGN_SIZE="assignment of member 'size' in read-only object|cannot assign to .* with const-qualified type"

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

#include "cbstmap.h"
#include "citerators.h"

void ccol_probe(cbmap m, const cmap_pair *kp, void *replacement);

$prelude

void ccol_probe(cbmap m, const cmap_pair *kp, void *replacement) {
  (void)kp;
  (void)replacement;
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

# -------------------------------------------------------------------------
# cbmap_get_elem_ref()
# -------------------------------------------------------------------------

probe 'plain cmap_pair * out-parameter' reject \
"incompatible pointer type|discards qualifiers in nested pointer types" '
  cmap_pair *vp = NULL;
  (void)cbmap_get_elem_ref(m, kp, &vp);'

probe 'assigning to the accessor pointer' reject "$ASSIGN_PTR" '
  const cmap_pair *vp = NULL;
  if (cbmap_get_elem_ref(m, kp, &vp) == ccol_success) vp->ptr = replacement;'

probe 'assigning to the accessor size' reject "$ASSIGN_SIZE" '
  const cmap_pair *vp = NULL;
  if (cbmap_get_elem_ref(m, kp, &vp) == ccol_success) vp->size = 1;'

probe 'reading the accessor and its bytes' accept - '
  const cmap_pair *vp = NULL;
  if (cbmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    unsigned char first = *(const unsigned char *)vp->ptr;
    size_t stored = vp->size;
    (void)first;
    (void)stored;
  }'

probe 'writing the bytes the accessor describes' accept - '
  const cmap_pair *vp = NULL;
  if (cbmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    memset(vp->ptr, 0, vp->size);
    *(unsigned char *)vp->ptr = 7u;
  }'

probe 'in-place update through cbmap_get_ptr' accept - '
  cbmap_redeclare(m, int, int);
  int key = 1;
  int *slot = cbmap_get_ptr(m, key);
  if (slot) *slot = 2;'

# -------------------------------------------------------------------------
# cbstmap has no cbmap_destroy_with_dtor(); chashmap alone offers an
# allocation-free destroy-with-callback walk, so there is no sibling of
# chmap_destroy_with_dtor()'s accessor rule to assert here. The two modules
# are parallel everywhere else, so that difference is stated rather than left
# to be discovered and "harmonised". If such a function is ever added to
# cbstmap, its callback takes a const cmap_pair * and the three cases
# tests/chashmap/compile_probe.sh carries for it belong here too.
# -------------------------------------------------------------------------

# -------------------------------------------------------------------------
# cmap_iterator
#
# ccol_iter_key_ptr()/ccol_iter_val_ptr() are const-correct on their own, so
# these cases reach the iterator's key_pair/val_pair members directly, which
# is the way around them. What the members address is the container's own
# accessor for the current entry, so the rule that governs it is the same one.
#
# The consequence is more direct here than it is for chashmap, whose accessor
# is a field derived from the entry: a cbstmap iterator's val_pair IS the
# node's authoritative pair, and destroy_bmap_node() releases the value
# through it. A pointer stored there is one the map never allocated and then
# frees at teardown, while the buffer it did allocate leaks.
# -------------------------------------------------------------------------

probe 'an iterator assigning to val_pair->ptr' reject "$ASSIGN_PTR" '
  cbmap_redeclare(m, int, int);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it))
    it->val_pair->ptr = replacement;'

probe 'an iterator assigning to val_pair->size' reject "$ASSIGN_SIZE" '
  cbmap_redeclare(m, int, int);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it))
    it->val_pair->size = 1;'

probe 'an iterator assigning to key_pair->ptr' reject "$ASSIGN_PTR" '
  cbmap_redeclare(m, int, int);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it))
    it->key_pair->ptr = replacement;'

probe 'iterating, reading keys and editing values' accept - '
  cbmap_redeclare(m, int, int);
  int total = 0;
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
    const int *k = ccol_iter_key_ptr(it);
    int *v = ccol_iter_val_ptr(it);
    total += *k;
    *v = 9;
  }
  ccol_for_each(m, each, { total += *ccol_iter_val_ptr(each); });
  (void)total;'

if [ "$checks" -eq 0 ]; then
  echo 'compile_probe: FAIL  no compiler was available to run any check'
  exit 1
fi

if [ "$failures" -ne 0 ]; then
  echo "compile_probe: FAILED ($failures of $checks checks)"
  exit 1
fi

echo "compile_probe: SUCCESS ($checks checks passed)"
