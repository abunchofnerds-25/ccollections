#!/bin/sh
# Checks that a program which links the shared library, and uses only a small
# part of it, has none of the library's memory still allocated when it exits.
#
# Every __attribute__((destructor)) of the library runs at exit for the whole
# shared object, whatever parts of it the program used. A destructor that
# creates new state while the process exits (a lazy initialization, say, or a
# registration in another module) allocates memory that nothing frees, because
# the destructor of the module owning that memory may have run before it. An
# application running valgrind with --errors-for-leak-kinds=all (as our test
# suites do) then gets errors its own code didn't cause.
#
# The test suites can't see this. Each suite compiles the library sources it
# needs straight into its own binary, so its set of destructors, and their
# order, differ from those of the shared object. That's why this check links a
# real consumer against the built shared object instead.
#
# Run it from the root of the repository after `make`; `make
# check_exit_reachable` and CI call it.
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

# The linker finds the library in its own directory, the same way
# -lccollections finds an installed copy, and the programs include the headers
# as <ccollections/x.h>.
mkdir -p "$dir/inc"
ln -s "$(pwd)/include" "$dir/inc/ccollections"
libdir=$(cd "$(dirname "$SO")" && pwd)

# One consumer for each shape:
# - a program that uses one container only, which catches a destructor that
#   initializes its module while the process exits;
# - a program that uses no module at all, which checks the bare link.
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
  # --no-as-needed keeps the library loaded in the program that calls none of
  # its functions. If the linker dropped the library, that case would check
  # nothing.
  "$CC" -std=gnu11 -I"$dir/inc" "$dir/$prog.c" -L"$libdir" \
    -Wl,--no-as-needed -lccollections -o "$dir/$prog"
  # The check only means something when the program really loads the library,
  # so we verify that first.
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
