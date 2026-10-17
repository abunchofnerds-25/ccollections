#!/bin/sh
# Compile-time probe for the rule behind every cmap_pair accessor that the
# map hands out: the key_pair/val_pair an iterator carries, and the stored
# char* that cbmap_get_ptr() reaches for a map with string values. It also
# pins the signature of cbmap_get_elem_ref(), which fills a cmap_pair that the
# caller owns and hands out no accessor.
#
# The map keeps one {ptr, size} accessor per entry and gives its address out
# through those surfaces. The two fields belong together: size is the length
# of the string or object that ptr points at, and the bytes belong to the map.
# If you assign to one field, the other describes something else, so the next
# read of that entry (a get_elem_copy, an iterator, cbmap_get) walks the wrong
# length, and a pointer the map didn't allocate is one it can't own.
#
# So each of those surfaces names the accessor through a const cmap_pair *,
# which turns those assignments into a compile error but keeps every
# legitimate use available (reading ptr and size, writing the bytes that ptr
# addresses within size). This script checks exactly that under every
# compiler it is given, since the guarantee is a property of the type and no
# run-time test can see it.
#
# A reject case also has to fail for the RIGHT reason. A probe that happens
# to be rejected over an unrelated mistake (a missing companion type
# variable, a typo) checks nothing but reads as a pass, so every reject case
# carries the diagnostic it expects, matched against the compiler's own
# output, and a rejection that doesn't match counts as a failure.
#
# Usage: ./compile_probe.sh [compiler ...]   (default: GCC and clang)

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The in-tree headers by default. CCOL_PROBE_INCLUDE_DIR points the probe at
# another header tree, and that's how we check the probe itself isn't
# vacuous: the guarantee lives in the headers, so to confirm a case can fail
# you run it against a copy of them with the qualifier removed.
INCLUDE_DIR=${CCOL_PROBE_INCLUDE_DIR:-"$SCRIPT_DIR/../../include"}
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# The GCC of the default list is gcc, or else the newest versioned gccNN in
# PATH (FreeBSD installs GCC only under a versioned name such as gcc14).
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
# the C locale. GCC quotes identifiers with plain apostrophes there and with
# typographic quotes in other locales.
LC_ALL=C
export LC_ALL

failures=0
checks=0

# Diagnostics shared by several cases. GCC names the member being assigned
# while Clang names the const-qualified object the member is reached
# through, so each pattern carries both spellings.
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
#
# The function fills a cmap_pair that belongs to the caller. That pair is the
# caller's own copy, so changing its fields changes nothing in the map, and
# the bytes that ptr addresses may be read and written within size.
# -------------------------------------------------------------------------

probe 'the address of a const cmap_pair * out-parameter' reject \
"incompatible pointer type|discards qualifiers" '
  const cmap_pair *vp = NULL;
  (void)vp;
  (void)cbmap_get_elem_ref(m, kp, &vp);'

probe 'reading the filled pair and its bytes' accept - '
  cmap_pair vp;
  if (cbmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    unsigned char first = *(const unsigned char *)vp.ptr;
    size_t stored = vp.size;
    (void)first;
    (void)stored;
  }'

probe 'writing the bytes the filled pair describes' accept - '
  cmap_pair vp;
  if (cbmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    memset(vp.ptr, 0, vp.size);
    *(unsigned char *)vp.ptr = 7u;
  }'

probe 'changing the fields of the caller own pair' accept - '
  cmap_pair vp;
  if (cbmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    vp.ptr = replacement;
    vp.size = 1;
  }'

probe 'replacing a stored string through cbmap_get_ptr' reject \
"assignment of read-only location|read-only variable is not assignable" '
  cbmap_redeclare(m, int, char *);
  int key = 1;
  char *const *sptr = cbmap_get_ptr(m, key);
  if (sptr) *sptr = replacement;'

probe 'editing a stored string through cbmap_get_ptr' accept - '
  cbmap_redeclare(m, int, char *);
  int key = 1;
  char *const *sptr = cbmap_get_ptr(m, key);
  if (sptr) (*sptr)[0] = 0;'

probe 'in-place update through cbmap_get_ptr' accept - '
  cbmap_redeclare(m, int, int);
  int key = 1;
  int *slot = cbmap_get_ptr(m, key);
  if (slot) *slot = 2;'

# -------------------------------------------------------------------------
# cbstmap has no cbmap_destroy_with_dtor(). Only chashmap offers an
# allocation-free destroy-with-callback walk, so there is nothing here like
# the accessor rule of chmap_destroy_with_dtor() to check. The two modules are
# parallel everywhere else, which is why we write the difference down instead
# of leaving it for somebody to find and "harmonise". If cbstmap ever gets
# such a function, its callback takes a const cmap_pair * and the three cases
# that tests/chashmap/compile_probe.sh carries for it belong here too.
# -------------------------------------------------------------------------

# -------------------------------------------------------------------------
# cmap_iterator
#
# ccol_iter_key_ptr()/ccol_iter_val_ptr() are const-correct by themselves,
# so these cases go around them and reach the iterator's key_pair/val_pair
# members directly. Those members address the container's own accessor for
# the current entry, so the same rule applies to them.
#
# The damage is more direct here than for chashmap, whose accessor is a field
# derived from the entry. A cbstmap iterator's val_pair IS the node's
# authoritative pair and destroy_bmap_node() releases the value through it,
# so a pointer stored there is one the map never allocated but frees at
# teardown, while the buffer it did allocate leaks.
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

# -------------------------------------------------------------------------
# Array key and value types
#
# The macros copy a key or a value through a temporary of the declared type,
# and C can't assign an array, so an array type would reach the map as a
# pointer to the caller's array and the map would store the address instead
# of the elements. That's why every macro that declares the companion type
# variables refuses an array key type and an array value type, with a message
# that says to wrap the array in a struct. The accept cases pin that the
# refusal stays narrow: const types, pointer types, function pointer types
# and struct types that hold an array all compile.
# -------------------------------------------------------------------------

ARRAY_KEY="cbmap: an array key type is not supported; wrap the array in a struct"
ARRAY_VAL="cbmap: an array value type is not supported; wrap the array in a struct"

probe 'cbmap_construct with an int[2] key' reject "$ARRAY_KEY" '
  (void)m;
  cbmap_construct(a, int[2], int);
  cbmap_destroy(a);'

probe 'cbmap_construct with an int[2] value' reject "$ARRAY_VAL" '
  (void)m;
  cbmap_construct(a, int, int[2]);
  cbmap_destroy(a);'

probe 'cbmap_construct with a char[16] value' reject "$ARRAY_VAL" '
  (void)m;
  cbmap_construct(a, int, char[16]);
  cbmap_destroy(a);'

probe 'cbmap_construct with a const char[16] key' reject "$ARRAY_KEY" '
  (void)m;
  cbmap_construct(a, const char[16], int);
  cbmap_destroy(a);'

probe 'cbmap_construct_scoped with an array value' reject "$ARRAY_VAL" '
  (void)m;
  cbmap_construct_scoped(a, int, int[2]);'

probe 'cbmap_construct_mp with an array key' reject "$ARRAY_KEY" '
  (void)m;
  cbmap_construct_mp(a, int[2], int, NULL);
  cbmap_destroy(a);'

probe 'cbmap_construct_mp_scoped with an array value' reject "$ARRAY_VAL" '
  (void)m;
  cbmap_construct_mp_scoped(a, int, int[2], NULL);'

probe 'cbmap_construct_full with an array value' reject "$ARRAY_VAL" '
  (void)m;
  cbmap_construct_full(a, int, int[2], NULL, NULL);
  cbmap_destroy(a);'

probe 'cbmap_construct_full_scoped with an array key' reject "$ARRAY_KEY" '
  (void)m;
  cbmap_construct_full_scoped(a, int[2], int, NULL, NULL);'

probe 'cbmap_declare with an array value' reject "$ARRAY_VAL" '
  (void)m;
  cbmap_declare(a, int, int[2]);
  cbmap_init(a);
  cbmap_destroy(a);'

probe 'cbmap_declare_scoped with an array key' reject "$ARRAY_KEY" '
  (void)m;
  cbmap_declare_scoped(a, int[2], int);
  cbmap_init(a);'

probe 'cbmap_redeclare with an array value' reject "$ARRAY_VAL" '
  cbmap_redeclare(m, int, char[16]);'

probe 'cbmap_redeclare with an array key' reject "$ARRAY_KEY" '
  cbmap_redeclare(m, int[2], int);'

probe 'cbmap_redeclare with an array typedef value' reject "$ARRAY_VAL" '
  typedef int ccol_probe_pair[2];
  cbmap_redeclare(m, int, ccol_probe_pair);'

probe 'cbmap_construct_cc with an array value' reject "$ARRAY_VAL" '
  (void)m;
  cbmap_construct_cc(a, int, int[2], NULL);
  cbmap_destroy(a);'

probe 'cbmap_construct_cc_scoped with an array key' reject "$ARRAY_KEY" '
  (void)m;
  cbmap_construct_cc_scoped(a, int[2], int, NULL);'

probe 'scalar, const, pointer and struct types still compile' accept - '
  struct ccol_probe_wrapped { int v[2]; };
  typedef void (*ccol_probe_fn)(void);
  (void)m;
  cbmap_construct(a, int, int);
  cbmap_destroy(a);
  cbmap_construct(b, const int, const double);
  cbmap_destroy(b);
  cbmap_construct(c, char *, const char *);
  cbmap_destroy(c);
  cbmap_construct(d, int, struct ccol_probe_wrapped);
  struct ccol_probe_wrapped w = {{1, 2}};
  cbmap_insert(d, 1, w);
  cbmap_destroy(d);
  cbmap_construct(e, int, ccol_probe_fn);
  cbmap_destroy(e);
  cbmap_construct(f, unsigned char *, void *);
  cbmap_destroy(f);
  cbmap_construct_scoped(g, long, struct ccol_probe_wrapped);
  cbmap_declare(h, int, int);
  cbmap_init(h);
  cbmap_destroy(h);
  cbmap_redeclare(m, int, int);'

if [ "$checks" -eq 0 ]; then
  echo 'compile_probe: FAIL  no compiler was available to run any check'
  exit 1
fi

if [ "$failures" -ne 0 ]; then
  echo "compile_probe: FAILED ($failures of $checks checks)"
  exit 1
fi

echo "compile_probe: SUCCESS ($checks checks passed)"
