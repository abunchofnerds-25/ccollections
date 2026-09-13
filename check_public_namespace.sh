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
ALLOW='^(cbinarymap|c_message_t|cthread_pool|__internal_entry_header)$'

PUBLIC_HEADERS='cbstmap chashmap chttp chttpclient chttpserver citerators cjson
clogger clrucache cmempool common csort cstring cthreadcomm cthreadpool cvector cyaml'

fail=0

bad_syms=$(readelf --dyn-syms -W "$SO" \
  | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK"){print $8}' \
  | sed 's/@.*//' | sort -u | grep -Ev "$NS" || true)
if [ -n "$bad_syms" ]; then
  echo "Exported symbols without a namespace prefix:" >&2
  echo "$bad_syms" | sed 's/^/  /' >&2
  fail=1
fi

for m in $PUBLIC_HEADERS; do
  h="include/$m.h"
  [ -f "$h" ] || continue
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
