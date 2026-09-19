#!/bin/sh
# Fails if anything in the library's public interface is missing a namespace
# prefix. Two surfaces are checked, because they break a consumer in different
# ways:
#
#   1. Exported dynamic symbols. An unprefixed one can collide with, or be
#      interposed by, a symbol of the same name in the application.
#   2. Public macros and typedefs in the installed headers. These are worse: a
#      macro textually rewrites any application code using that name, so a
#      consumer with its own log_info() or mutex_t breaks merely by including
#      one of our headers.
#
# Run from the repository root, after `make`. Used by `make check_namespace`
# and by CI.
set -eu

SO="${1:-libccollections.so}"
[ -e "$SO" ] || { echo "check_public_namespace: $SO not built; run make first" >&2; exit 2; }

# A prefix is acceptable if it is ccol_/CCOL_ or the owning module's own name.
NS='^_*(ccol_|CCOL_|cvec|cvector|chmap|chashmap|cbmap|cbstmap|cstr|cstring|csort|cjson|cyaml|clog|clru|cmap|cmempool|cthreadcomm|cthreadpool|ctpool|chttp|ctls|citer|CVEC|CHMAP|CBMAP|CSTR|CSORT|CJSON|CYAML|CLOG|CLRU|CMAP|CMEMPOOL|CTHREAD|CTPOOL|CHTTP|CTLS|CITER)'

# Struct tags that are already c-prefixed and are not generic English words.
ALLOW='^(cbinarymap|c_message_t|cthread_pool)$'

# Headers that ship. Derived from what is there rather than listed, so a header
# added later is checked without anyone remembering to add it here, and an
# internal one (no visibility block, excluded from make install) is skipped by
# naming only those. Keep in step with INTERNAL_HEADER_FILES in the Makefile.
INTERNAL_HEADERS='chashkey chttp1_parser cpintable ctls cdebuglog'

fail=0

# readelf's own status is checked, separately from the pipeline's (which would
# be sort's): an unreadable or non-ELF artifact otherwise yields an empty symbol
# list and this reports a fully namespaced interface having examined nothing.
if ! raw_syms=$(readelf --dyn-syms -W "$SO"); then
  echo "check_public_namespace: could not read dynamic symbols from $SO" >&2
  exit 2
fi
all_syms=$(printf '%s\n' "$raw_syms" \
  | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK"){print $8}' \
  | sed 's/@.*//' | sort -u)
# An empty set is never a legitimate answer for this library, and is exactly
# what a stripped, truncated or wrong-format artifact produces.
if [ -z "$all_syms" ]; then
  echo "check_public_namespace: $SO exports no dynamic symbols; refusing to" >&2
  echo "                        report on an empty interface." >&2
  exit 2
fi
bad_syms=$(printf '%s\n' "$all_syms" | grep -Ev "$NS" || true)
if [ -n "$bad_syms" ]; then
  echo "Exported symbols without a namespace prefix:" >&2
  echo "$bad_syms" | sed 's/^/  /' >&2
  fail=1
fi

headers=$(ls include/*.h) || headers=''
if [ -z "$headers" ]; then
  echo "check_public_namespace: no headers found under include/;" >&2
  echo "                        run this from the repository root." >&2
  exit 2
fi
for h in $headers; do
  m=$(basename "$h" .h)
  skip=0
  for i in $INTERNAL_HEADERS; do
    [ "$m" = "$i" ] && skip=1
  done
  [ "$skip" -eq 1 ] && continue
  bad=$( { grep -hoE '^#define +[A-Za-z_][A-Za-z0-9_]*' "$h" | sed 's/#define *//'
           grep -hoE '^typedef .*\b[A-Za-z_][A-Za-z0-9_]*;$' "$h" | grep -oE '[A-Za-z_][A-Za-z0-9_]*;$' | tr -d ';'
           grep -hoE '^\} *[A-Za-z_][A-Za-z0-9_]*;' "$h" | grep -oE '[A-Za-z_][A-Za-z0-9_]*'
         } | sort -u | grep -Ev "$NS" | grep -Ev '^_' | grep -Ev "$ALLOW" || true)
  if [ -n "$bad" ]; then
    echo "Public macros/types without a namespace prefix in $h:" >&2
    echo "$bad" | sed 's/^/  /' >&2
    fail=1
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "check_public_namespace: OK (public interface is fully namespaced)"
fi
exit "$fail"
