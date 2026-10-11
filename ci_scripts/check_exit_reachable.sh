#!/bin/sh
# This script makes sure that a program which links the shared library, and
# uses only a small part of it, has no memory of the library allocated at its
# exit.
#
# Each __attribute__((destructor)) of the library runs at exit for the full
# shared object, whatever parts of it the program used. A destructor that
# makes new state while the process exits (for example a lazy initialization,
# or a registration in another module) allocates memory that nothing frees,
# because the destructor of the module that owns that memory can have run
# before it. An application that runs valgrind with --errors-for-leak-kinds=all
# (as the test suites of this project do) then gets errors that its own code
# did not cause.
#
# The test suites cannot see this: each suite compiles the library sources that
# it needs directly into its own binary, so its destructors are a different
# set, in a different order, from those of the shared object. This check
# instead links a real consumer against the built shared object.
#
# Run this script from the root of the repository, after `make`;
# `make check_exit_reachable` and CI use it.
set -eu

SO="${1:-}"
[ -n "$SO" ] || { echo "usage: $0 <shared-object>" >&2; exit 2; }
[ -e "$SO" ] || { echo "check_exit_reachable: $SO not built; run make first" >&2; exit 2; }
command -v valgrind >/dev/null 2>&1 || {
  echo "check_exit_reachable: valgrind is required" >&2
  exit 2
}

CC="${CC:-cc}"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT INT TERM

# The linker finds the library through its own directory, in the same way as
# -lccollections finds an installed copy, and the programs include the headers
# as <ccollections/x.h>.
mkdir -p "$dir/inc"
ln -s "$(pwd)/include" "$dir/inc/ccollections"
libdir=$(cd "$(dirname "$SO")" && pwd)

# There is one consumer for each shape:
# - A program that uses only one container, which shows a destructor that
#   initializes its module while the process exits.
# - A program that uses no module, which checks the bare link.
cat >"$dir/vector_only.c" <<'EOF'
#include <ccollections/cvector.h>
int main(void) {
  cvec_construct(v, int);
  cvec_push(v, 1);
  cvec_destroy(v);
  return 0;
}
EOF
cat >"$dir/nothing.c" <<'EOF'
#include <ccollections/common.h>
int main(void) { return 0; }
EOF

failed=0
for prog in vector_only nothing; do
  # --no-as-needed keeps the library loaded in the program that calls none
  # of its functions; if the linker removed the library, that case would
  # check nothing.
  "$CC" -std=gnu11 -I"$dir/inc" "$dir/$prog.c" -L"$libdir" \
    -Wl,--no-as-needed -lccollections -o "$dir/$prog"
  # The check is correct only when the program loads the library, so the
  # script makes sure that it does.
  if ! LD_LIBRARY_PATH="$libdir" ldd "$dir/$prog" | grep -q libccollections; then
    echo "check_exit_reachable: $prog does not load the library" >&2
    exit 2
  fi
  if ! LD_LIBRARY_PATH="$libdir" valgrind -q --leak-check=full \
      --show-leak-kinds=all --errors-for-leak-kinds=all --error-exitcode=9 \
      ${CCOL_VALGRIND_SUPP:-} "$dir/$prog" >"$dir/$prog.log" 2>&1; then
    echo "check_exit_reachable: $prog leaves memory allocated at exit:" >&2
    cat "$dir/$prog.log" >&2
    failed=1
  fi
done

[ "$failed" -eq 0 ] || exit 1
echo "check_exit_reachable: OK (a program that uses little of the library ends with nothing allocated)"
