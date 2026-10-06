#!/bin/sh
# This script formats the C sources exactly as the editor configuration of this
# project does: clang-format with the Google style and with sorted #include
# blocks. The repository has no .clang-format file. Therefore, this script
# names the style.
#
# Usage, from the root of the repository:
#   ci_scripts/format.sh                 format each C file in place
#   ci_scripts/format.sh --check         list the files that formatting
#                                        changes, and exit with 1 if there
#                                        are any
#   ci_scripts/format.sh FILE...         format only the named files
#   ci_scripts/format.sh --check FILE... check only the named files
#
# CLANG_FORMAT selects the binary (default: clang-format). Two major versions
# of clang-format can format the same code differently. Therefore, use the
# version that the editor uses.
#
# The files are the .c and .h files that git tracks, and the untracked files
# that git does not ignore. Vendored code (tests/tau, picohttpparser) keeps
# the layout of its upstream, and the script does not change it.
set -eu

CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
STYLE="-style=Google"

check=0
if [ "${1:-}" = "--check" ]; then
  check=1
  shift
fi

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  echo "format.sh: $CLANG_FORMAT not found" >&2
  exit 2
fi

if [ "$#" -eq 0 ]; then
  if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
    echo "format.sh: run it inside the repository, or name the files" >&2
    exit 2
  fi
  cd "$(git rev-parse --show-toplevel)"
  files=$(git ls-files --cached --others --exclude-standard -- '*.c' '*.h' |
    grep -v -e '^tests/tau/' -e '/picohttpparser/' || true)
  if [ -z "$files" ]; then
    echo "format.sh: no C files found" >&2
    exit 2
  fi
  set -f
  # shellcheck disable=SC2086
  set -- $files
  set +f
fi

changed=0
for f in "$@"; do
  if [ ! -f "$f" ]; then
    echo "format.sh: $f: no such file" >&2
    exit 2
  fi
  if "$CLANG_FORMAT" "$STYLE" --sort-includes "$f" | cmp -s - "$f"; then
    continue
  fi
  changed=$((changed + 1))
  if [ "$check" -eq 1 ]; then
    echo "$f"
  else
    "$CLANG_FORMAT" "$STYLE" --sort-includes -i "$f"
    echo "formatted $f"
  fi
done

if [ "$check" -eq 1 ] && [ "$changed" -gt 0 ]; then
  echo "format.sh: $changed file(s) need formatting" >&2
  exit 1
fi
exit 0
