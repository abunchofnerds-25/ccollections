#!/bin/sh
# Compile-time probes for the cvector macros.
#
# The probes cover properties that only the compiler can show. The first is
# the element-type rejection of cvec_sort. The second is that the value
# macros and cvector_append_array() accept const data under -Werror: a const
# source array, a const-qualified element type and an array element type,
# whose value may be a named array but not a pointer. The third is that the
# iterator accessors compile under -Wcast-qual.
#
# cvec_sort carries a _Static_assert that refuses an element type with no
# default comparison procedure. That is a diagnostic the compiler issues, so
# it cannot be exercised from inside a test binary that has to compile in the
# first place; this script is the regression check for it, and this
# directory's `test` target runs it alongside ./tests.
#
# Two properties are pinned, and both are needed: every element type that has
# no default comparison procedure must FAIL to compile with a diagnostic
# naming cvector_sort_with_comparison_proc (so the message is actionable),
# and every element type that does have one must still compile. A check that
# only asserted the first half would pass against a cvec_sort that rejects
# everything.
#
# Each probe is compiled with -fsyntax-only, which is all a _Static_assert
# needs, and under every compiler in PROBE_CCS (GCC and clang by default). A
# compiler that is not installed is reported and skipped; zero usable
# compilers, or zero probes actually run, is a failure rather than a silent
# pass.
#
# Run from tests/cvector.
set -eu

INCLUDES="-I.. -I../../include"
DEFINITIONS="-DRUNNING_UNIT_TESTS -D_FILE_OFFSET_BITS=64"
STD="-std=gnu11"
PROBE_CFLAGS="$INCLUDES $DEFINITIONS $STD -Wall -Wextra -Werror"

# The diagnostic has to point the caller at the supported alternative; a bare
# "static assertion failed" would be a compile-time abort with no more help
# than the run-time one it replaces.
EXPECTED_HINT="cvector_sort_with_comparison_proc"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

checks_run=0
compilers_used=0
failures=0

# emit <file> <prelude> <element-type> <body>
emit() {
  cat > "$1" <<EOF
#include <cvector.h>
#include <stdbool.h>

$2

void probe_body(void);
void probe_body(void) {
  cvec_construct(probe_vec, $3);
  $4
  cvec_destroy(probe_vec);
}
EOF
}

# expect_reject <cc> <label> <prelude> <element-type>
expect_reject() {
  _cc=$1
  _label=$2
  emit "$WORK/probe.c" "$3" "$4" "cvec_sort(probe_vec);"
  checks_run=$((checks_run + 1))
  if "$_cc" $PROBE_CFLAGS -fsyntax-only "$WORK/probe.c" > "$WORK/out.txt" 2>&1; then
    echo "FAIL [$_cc] cvec_sort on $_label compiled; it must be rejected" >&2
    failures=$((failures + 1))
    return 0
  fi
  if ! grep -q "$EXPECTED_HINT" "$WORK/out.txt"; then
    echo "FAIL [$_cc] cvec_sort on $_label was rejected without naming" >&2
    echo "     $EXPECTED_HINT in the diagnostic:" >&2
    sed 's/^/       /' "$WORK/out.txt" >&2
    failures=$((failures + 1))
    return 0
  fi
  echo "  ok  [$_cc] cvec_sort on $_label is rejected at compile time"
}

# expect_reject_body <cc> <label> <prelude> <element-type> <body> <hint>
expect_reject_body() {
  _cc=$1
  _label=$2
  emit "$WORK/probe.c" "$3" "$4" "$5"
  checks_run=$((checks_run + 1))
  if "$_cc" $PROBE_CFLAGS -fsyntax-only "$WORK/probe.c" > "$WORK/out.txt" 2>&1; then
    echo "FAIL [$_cc] $_label compiled; it must be rejected" >&2
    failures=$((failures + 1))
    return 0
  fi
  if ! grep -q "$6" "$WORK/out.txt"; then
    echo "FAIL [$_cc] $_label was rejected without naming '$6':" >&2
    sed 's/^/       /' "$WORK/out.txt" >&2
    failures=$((failures + 1))
    return 0
  fi
  echo "  ok  [$_cc] $_label is rejected at compile time"
}

# expect_accept <cc> <label> <prelude> <element-type> <body>
expect_accept() {
  _cc=$1
  _label=$2
  emit "$WORK/probe.c" "$3" "$4" "$5"
  checks_run=$((checks_run + 1))
  if "$_cc" $PROBE_CFLAGS -fsyntax-only "$WORK/probe.c" > "$WORK/out.txt" 2>&1; then
    echo "  ok  [$_cc] $_label compiles"
    return 0
  fi
  echo "FAIL [$_cc] $_label must compile but did not:" >&2
  sed 's/^/       /' "$WORK/out.txt" >&2
  failures=$((failures + 1))
}

ENUM_PRELUDE='typedef enum { probe_enum_a = 0, probe_enum_b = 9 } probe_enum;
typedef enum { probe_senum_a = -3, probe_senum_b = 4 } probe_senum;'
AGG_PRELUDE='struct probe_struct { int a; double b; };
union probe_union { int a; double b; };
typedef char probe_char_array[64];'

run_for_compiler() {
  cc=$1
  if ! command -v "$cc" > /dev/null 2>&1; then
    echo "note: $cc is not installed; skipping its probes"
    return 0
  fi
  compilers_used=$((compilers_used + 1))
  echo "== $cc =="

  # Element types with no default comparison procedure: must not compile.
  expect_reject "$cc" "bool" "" "bool"
  expect_reject "$cc" "a struct" "$AGG_PRELUDE" "struct probe_struct"
  expect_reject "$cc" "a union" "$AGG_PRELUDE" "union probe_union"
  expect_reject "$cc" "int *" "" "int *"
  expect_reject "$cc" "void *" "" "void *"
  expect_reject "$cc" "double *" "" "double *"
  expect_reject "$cc" "a fixed-size char array" "$AGG_PRELUDE" "probe_char_array"

  # Every element type that has a default comparison procedure: must compile.
  for t in char "signed char" "unsigned char" short "unsigned short" int \
      "unsigned int" long "unsigned long" "long long" "unsigned long long" \
      float double "long double" "char *" "const char *" "signed char *" \
      "unsigned char *"; do
    expect_accept "$cc" "cvec_sort on $t" "" "$t" "cvec_sort(probe_vec);"
  done
  expect_accept "$cc" "cvec_sort on an enum" "$ENUM_PRELUDE" "probe_enum" \
    "cvec_sort(probe_vec);"
  expect_accept "$cc" "cvec_sort on an enum with a negative enumerator" \
    "$ENUM_PRELUDE" "probe_senum" "cvec_sort(probe_vec);"

  # cvec_find keeps accepting every element type, including the ones
  # cvec_sort rejects: its documented fallback is byte-wise equality, not a
  # refusal.
  expect_accept "$cc" "cvec_find on bool" "" "bool" \
    "(void)cvec_find(probe_vec, true);"
  expect_accept "$cc" "cvec_find on a struct" "$AGG_PRELUDE" \
    "struct probe_struct" \
    "(void)cvec_find(probe_vec, (struct probe_struct){0});"
  expect_accept "$cc" "cvec_find on int *" "" "int *" \
    "(void)cvec_find(probe_vec, (int *)0);"

  # A const source array appends without a discarded-qualifier diagnostic,
  # through the function and through the macro.
  expect_accept "$cc" "cvector_append_array from a const array" \
    "static const int probe_table[] = {1, 2, 3};" "int" \
    "(void)cvector_append_array(probe_vec, probe_table, 3);"
  expect_accept "$cc" "cvec_append_array from a const array" \
    "static const int probe_table[] = {1, 2, 3};" "int" \
    "cvec_append_array(probe_vec, probe_table, 3);"

  # The value macros clear the padding of a temporary that must therefore be
  # modifiable. A const-qualified element type and an array element type
  # both still push and find.
  expect_accept "$cc" "cvec_push and cvec_find on a const struct" \
    "$AGG_PRELUDE" "const struct probe_struct" \
    "struct probe_struct e = {1, 2.0}; cvec_push(probe_vec, e);
  cvec_push(probe_vec, ((struct probe_struct){1, 2.0})); (void)cvec_find(probe_vec, e);"
  expect_accept "$cc" "cvec_push and cvec_find on a fixed-size char array" \
    "$AGG_PRELUDE" "probe_char_array" \
    "cvec_push(probe_vec, \"abc\"); cvec_push(probe_vec, \"de\");
  (void)cvec_find(probe_vec, \"abc\");"
  # A named array is a value of an array element type too, for pushing and
  # for searching, whatever its qualifiers and whether it is shorter or
  # longer than the element.
  expect_accept "$cc" "cvec_push and cvec_find with a named array" \
    "$AGG_PRELUDE" "probe_char_array" \
    "char a[64] = \"abc\"; const char b[8] = \"de\"; char c[80] = \"f\";
  cvec_push(probe_vec, a); cvec_push(probe_vec, b);
  cvec_push(probe_vec, c); (void)cvec_find(probe_vec, a);
  (void)cvec_find(probe_vec, b); (void)cvec_find(probe_vec, c);"
  expect_accept "$cc" "cvec_push and cvec_find on an int array element" \
    "" "int[3]" \
    "int a[3] = {1, 2, 3}; cvec_push(probe_vec, a);
  (void)cvec_find(probe_vec, a);"
  # A pointer carries no size, so it is refused as the value of an array
  # element type rather than read past the end of what it points at.
  expect_reject_body "$cc" "cvec_find with a pointer needle on an array" \
    "$AGG_PRELUDE" "probe_char_array" \
    "const char *p = \"abc\"; (void)cvec_find(probe_vec, p);" \
    "array or a string literal"
  expect_reject_body "$cc" "cvec_push with a pointer on an array" \
    "$AGG_PRELUDE" "probe_char_array" \
    "char *p = 0; cvec_push(probe_vec, p);" \
    "array or a string literal"

  # ccol_begin accepts exactly the three container handles. Any other
  # pointer is a compile error, never a call into another container's begin
  # function; a top-level qualified handle still selects its own container.
  expect_reject_body "$cc" "ccol_begin on an int pointer" "" "int" \
    "int x = 0; int *p = &x; cmap_iterator *it = ccol_begin(p); (void)it;" \
    "compatible"
  expect_reject_body "$cc" "ccol_begin on a void pointer" "" "int" \
    "void *p = probe_vec; cmap_iterator *it = ccol_begin(p); (void)it;" \
    "compatible"
  expect_accept "$cc" "ccol_begin on a const-qualified vector handle" "" \
    "int" \
    "struct cvector *const cv = probe_vec;
  cmap_iterator *it = ccol_begin(cv); if (it) ccol_iter_destroy(it);"

  # The iterator accessors keep the const of the accessor slot of a map
  # entry, so a caller that builds with -Wcast-qual -Werror can use them for
  # every value type: a scalar, a struct and a character pointer, over a
  # vector, a hash map and a BST map.
  checks_run=$((checks_run + 1))
  cat > "$WORK/castqual.c" <<'EOF_CQ'
#include <cbstmap.h>
#include <chashmap.h>
#include <cvector.h>
struct probe_pt { int a; double b; };
int probe_cast_qual(void);
int probe_cast_qual(void) {
  int acc = 0;
  struct probe_pt p = {1, 2.0};
  cvec_construct_scoped(vi, int);
  cvec_push(vi, 1);
  ccol_for_each(vi, i1, { acc += *ccol_iter_val_ptr(i1); });
  cvec_construct_scoped(vs, char *);
  cvec_push(vs, "x");
  ccol_for_each(vs, i2, { acc += **ccol_iter_val_ptr(i2); });
  cvec_construct_scoped(vp, struct probe_pt);
  cvec_push(vp, p);
  ccol_for_each(vp, i3, { ccol_iter_val_ptr(i3)->a = 3; });
  chmap_construct_scoped(m1, char *, char *);
  chmap_insert(m1, "k", "v");
  ccol_for_each(m1, i4, {
    acc += **ccol_iter_val_ptr(i4) + **ccol_iter_key_ptr(i4);
  });
  chmap_construct_scoped(m2, int, double);
  chmap_insert(m2, 1, 2.0);
  ccol_for_each(m2, i5, { *ccol_iter_val_ptr(i5) += 1.0; });
  cbmap_construct_scoped(b1, int, const char *);
  cbmap_insert(b1, 1, "z");
  ccol_for_each(b1, i6, { acc += **ccol_iter_val_ptr(i6); });
  cbmap_construct_scoped(b2, char *, struct probe_pt);
  cbmap_insert(b2, "q", p);
  ccol_for_each(b2, i7, { ccol_iter_val_ptr(i7)->b = 1.0; });
  return acc;
}
EOF_CQ
  if "$cc" $PROBE_CFLAGS -Wcast-qual -fsyntax-only "$WORK/castqual.c" \
      > "$WORK/out.txt" 2>&1; then
    echo "  ok  [$cc] the iterator accessors compile under -Wcast-qual"
  else
    echo "FAIL [$cc] the iterator accessors must compile under -Wcast-qual:" >&2
    sed 's/^/       /' "$WORK/out.txt" >&2
    failures=$((failures + 1))
  fi

  # An unsupported element type is still fully sortable with an explicit
  # comparison procedure, which is exactly what the diagnostic tells the
  # caller to do.
  expect_accept "$cc" "cvector_sort_with_comparison_proc on bool" \
    "int probe_cmp(const void *a, const void *b);" "bool" \
    "cvector_sort_with_comparison_proc(probe_vec, probe_cmp);"
}

# The GCC of the default list: gcc, or else the newest versioned gccNN in
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

for cc in ${PROBE_CCS:-$(probe_gcc) clang}; do
  run_for_compiler "$cc"
done

if [ "$compilers_used" -eq 0 ]; then
  echo "compile_probes: no usable compiler found; refusing to report a pass" >&2
  exit 2
fi
if [ "$checks_run" -eq 0 ]; then
  echo "compile_probes: no probe ran; refusing to report a pass" >&2
  exit 2
fi
if [ "$failures" -ne 0 ]; then
  echo "compile_probes: $failures of $checks_run probes failed" >&2
  exit 1
fi
echo "compile_probes: $checks_run probes passed across $compilers_used compiler(s)"
