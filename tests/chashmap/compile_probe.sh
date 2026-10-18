#!/bin/sh
# Compile-time probe for the rule behind every cmap_pair accessor that the
# map hands out: the callback argument of chmap_destroy_with_dtor(), the
# key_pair/val_pair an iterator carries, and the stored char* that
# chmap_get_ptr() reaches for a map with string values. It also pins the
# signature of chmap_get_elem_ref(), which fills a cmap_pair that the caller
# owns and hands out no accessor.
#
# The map keeps one {ptr, size} accessor per entry and gives its address out
# through those surfaces. The two fields belong together: size is the length
# of the string or object that ptr points at, and the bytes belong to the map.
# Once you assign to one field the other describes something else, so the
# next read of that entry (a get_elem_copy, an iterator, chmap_get) walks the
# wrong length, and a pointer the map didn't allocate is one it can't own.
#
# Each of those surfaces names the accessor through a const cmap_pair *
# for that reason. The assignments become a compile error, and every
# legitimate use (reading ptr and size, writing the bytes that ptr addresses
# within size) is left available. This script checks exactly that under every
# compiler it is given, because the guarantee is a property of the type and
# no run-time test can see it.
#
# A reject case must also fail for the RIGHT reason. A probe rejected over an
# unrelated mistake (a missing companion type variable, a typo) checks
# nothing but looks like a pass, so each reject case carries the diagnostic
# it expects, matched against the compiler's own output, and a rejection that
# doesn't match is counted as a failure.
#
# Usage: ./compile_probe.sh [compiler ...]   (default: GCC and clang)

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The in-tree headers by default. CCOL_PROBE_INCLUDE_DIR points the probe at
# a different header tree, which is how we make sure the probe isn't
# vacuous: the guarantee lives in the headers, so you confirm that a case can
# fail by running it against a copy of them with the qualifier removed.
INCLUDE_DIR=${CCOL_PROBE_INCLUDE_DIR:-"$SCRIPT_DIR/../../include"}
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# The default list uses gcc, or else the newest versioned gccNN in PATH
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

# Reject cases match the compiler's own wording, so the compiler runs in the
# C locale, where GCC quotes identifiers with plain apostrophes (other
# locales get typographic quotes).
LC_ALL=C
export LC_ALL

failures=0
checks=0

# Diagnostics shared by several cases. GCC names the member being assigned
# and Clang names the const-qualified object that the member is reached
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

#include "chashmap.h"
#include "citerators.h"

void ccol_probe(chmap m, const cmap_pair *kp, void *replacement);

$prelude

void ccol_probe(chmap m, const cmap_pair *kp, void *replacement) {
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
# chmap_get_elem_ref()
#
# The function fills a cmap_pair that the caller owns. It's the caller's own
# copy, so changing its fields changes nothing in the map, and the bytes that
# ptr addresses may be read and written within size.
# -------------------------------------------------------------------------

probe 'the address of a const cmap_pair * out-parameter' reject \
"incompatible pointer type|discards qualifiers" '
  const cmap_pair *vp = NULL;
  (void)vp;
  (void)chmap_get_elem_ref(m, kp, &vp);'

probe 'reading the filled pair and its bytes' accept - '
  cmap_pair vp;
  if (chmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    unsigned char first = *(const unsigned char *)vp.ptr;
    size_t stored = vp.size;
    (void)first;
    (void)stored;
  }'

probe 'writing the bytes the filled pair describes' accept - '
  cmap_pair vp;
  if (chmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    memset(vp.ptr, 0, vp.size);
    *(unsigned char *)vp.ptr = 7u;
  }'

probe 'changing the fields of the caller own pair' accept - '
  cmap_pair vp;
  if (chmap_get_elem_ref(m, kp, &vp) == ccol_success) {
    vp.ptr = replacement;
    vp.size = 1;
  }'

probe 'replacing a stored string through chmap_get_ptr' reject \
"assignment of read-only location|read-only variable is not assignable" '
  chmap_redeclare(m, int, char *);
  int key = 1;
  char *const *sptr = chmap_get_ptr(m, key);
  if (sptr) *sptr = replacement;'

probe 'editing a stored string through chmap_get_ptr' accept - '
  chmap_redeclare(m, int, char *);
  int key = 1;
  char *const *sptr = chmap_get_ptr(m, key);
  if (sptr) (*sptr)[0] = 0;'

probe 'in-place update through chmap_get_ptr' accept - '
  chmap_redeclare(m, int, int);
  int key = 1;
  int *slot = chmap_get_ptr(m, key);
  if (slot) *slot = 2;'

# -------------------------------------------------------------------------
# chmap_destroy_with_dtor()
#
# On the separate-chaining backend the callback gets the live node's own
# accessor instead of a copy, so what the callback sees is the map's
# bookkeeping itself and not just something on the stack of that call.
# -------------------------------------------------------------------------

probe 'a destructor assigning to the accessor pointer' reject "$ASSIGN_PTR" '
  (void)m;
  chmap_destroy_with_dtor(m, ccol_probe_dtor, NULL);' \
'static void ccol_probe_dtor(const cmap_pair *val_pair, void *dtor_ctx) {
  (void)dtor_ctx;
  val_pair->ptr = NULL;
}'

probe 'a destructor with a plain cmap_pair * parameter' reject \
"incompatible pointer type|incompatible function pointer types" '
  (void)m;
  chmap_destroy_with_dtor(m, ccol_probe_dtor, NULL);' \
'static void ccol_probe_dtor(cmap_pair *val_pair, void *dtor_ctx) {
  (void)val_pair;
  (void)dtor_ctx;
}'

probe 'a destructor freeing what the value points at' accept - '
  (void)m;
  chmap_destroy_with_dtor(m, ccol_probe_dtor, NULL);' \
'static void ccol_probe_dtor(const cmap_pair *val_pair, void *dtor_ctx) {
  void *owned = NULL;
  (void)dtor_ctx;
  if (val_pair->size == sizeof(owned)) {
    memcpy(&owned, val_pair->ptr, sizeof(owned));
    free(owned);
  }
}'

# -------------------------------------------------------------------------
# cmap_iterator
#
# ccol_iter_key_ptr()/ccol_iter_val_ptr() are const-correct by themselves,
# so these cases get around them by reaching the iterator's key_pair/val_pair
# members directly. Those members address the container's own accessor for
# the current entry, so the same rule covers them.
# -------------------------------------------------------------------------

probe 'an iterator assigning to val_pair->ptr' reject "$ASSIGN_PTR" '
  chmap_redeclare(m, int, int);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it))
    it->val_pair->ptr = replacement;'

probe 'an iterator assigning to val_pair->size' reject "$ASSIGN_SIZE" '
  chmap_redeclare(m, int, int);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it))
    it->val_pair->size = 1;'

probe 'an iterator assigning to key_pair->ptr' reject "$ASSIGN_PTR" '
  chmap_redeclare(m, int, int);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it))
    it->key_pair->ptr = replacement;'

probe 'iterating, reading keys and editing values' accept - '
  chmap_redeclare(m, int, int);
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
# The macros copy a key or a value through a temporary of the declared type.
# C can't assign an array, so an array type would reach the map as a pointer
# to the caller's array, and the map would store that address instead of the
# elements. So every macro that declares the companion type variables refuses
# an array key type and an array value type, with a message that says to wrap
# the array in a struct. The accept cases pin that the refusal stays narrow:
# const types, pointer types, function pointer types and struct types that
# hold an array all compile.
# -------------------------------------------------------------------------

ARRAY_KEY="chmap: an array key type is not supported; wrap the array in a struct"
ARRAY_VAL="chmap: an array value type is not supported; wrap the array in a struct"

probe 'chmap_construct with an int[2] key' reject "$ARRAY_KEY" '
  (void)m;
  chmap_construct(a, int[2], int);
  chmap_destroy(a);'

probe 'chmap_construct with an int[2] value' reject "$ARRAY_VAL" '
  (void)m;
  chmap_construct(a, int, int[2]);
  chmap_destroy(a);'

probe 'chmap_construct with a char[16] value' reject "$ARRAY_VAL" '
  (void)m;
  chmap_construct(a, int, char[16]);
  chmap_destroy(a);'

probe 'chmap_construct with a const char[16] key' reject "$ARRAY_KEY" '
  (void)m;
  chmap_construct(a, const char[16], int);
  chmap_destroy(a);'

probe 'chmap_construct_scoped with an array value' reject "$ARRAY_VAL" '
  (void)m;
  chmap_construct_scoped(a, int, int[2]);'

probe 'chmap_construct_mp with an array key' reject "$ARRAY_KEY" '
  (void)m;
  chmap_construct_mp(a, int[2], int, NULL);
  chmap_destroy(a);'

probe 'chmap_construct_mp_scoped with an array value' reject "$ARRAY_VAL" '
  (void)m;
  chmap_construct_mp_scoped(a, int, int[2], NULL);'

probe 'chmap_construct_full with an array value' reject "$ARRAY_VAL" '
  (void)m;
  chmap_construct_full(a, int, int[2], NULL, NULL, NULL);
  chmap_destroy(a);'

probe 'chmap_construct_full_scoped with an array key' reject "$ARRAY_KEY" '
  (void)m;
  chmap_construct_full_scoped(a, int[2], int, NULL, NULL, NULL);'

probe 'chmap_declare with an array value' reject "$ARRAY_VAL" '
  (void)m;
  chmap_declare(a, int, int[2]);
  chmap_init(a);
  chmap_destroy(a);'

probe 'chmap_declare_scoped with an array key' reject "$ARRAY_KEY" '
  (void)m;
  chmap_declare_scoped(a, int[2], int);
  chmap_init(a);'

probe 'chmap_redeclare with an array value' reject "$ARRAY_VAL" '
  chmap_redeclare(m, int, char[16]);'

probe 'chmap_redeclare with an array key' reject "$ARRAY_KEY" '
  chmap_redeclare(m, int[2], int);'

probe 'chmap_redeclare with an array typedef value' reject "$ARRAY_VAL" '
  typedef int ccol_probe_pair[2];
  chmap_redeclare(m, int, ccol_probe_pair);'

probe 'chmap_construct_ch with an array value' reject "$ARRAY_VAL" '
  (void)m;
  chmap_construct_ch(a, int, int[2], NULL);
  chmap_destroy(a);'

probe 'chmap_construct_ch_scoped with an array key' reject "$ARRAY_KEY" '
  (void)m;
  chmap_construct_ch_scoped(a, int[2], int, NULL);'

probe 'scalar, const, pointer and struct types still compile' accept - '
  struct ccol_probe_wrapped { int v[2]; };
  typedef void (*ccol_probe_fn)(void);
  (void)m;
  chmap_construct(a, int, int);
  chmap_destroy(a);
  chmap_construct(b, const int, const double);
  chmap_destroy(b);
  chmap_construct(c, char *, const char *);
  chmap_destroy(c);
  chmap_construct(d, int, struct ccol_probe_wrapped);
  struct ccol_probe_wrapped w = {{1, 2}};
  chmap_insert(d, 1, w);
  chmap_destroy(d);
  chmap_construct(e, int, ccol_probe_fn);
  chmap_destroy(e);
  chmap_construct(f, unsigned char *, void *);
  chmap_destroy(f);
  chmap_construct_scoped(g, long, struct ccol_probe_wrapped);
  chmap_declare(h, int, int);
  chmap_init(h);
  chmap_destroy(h);
  chmap_redeclare(m, int, int);'

if [ "$checks" -eq 0 ]; then
  echo 'compile_probe: FAIL  no compiler was available to run any check'
  exit 1
fi

if [ "$failures" -ne 0 ]; then
  echo "compile_probe: FAILED ($failures of $checks checks)"
  exit 1
fi

echo "compile_probe: SUCCESS ($checks checks passed)"
