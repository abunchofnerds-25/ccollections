#!/bin/sh
# This script fails when a part of the public interface of the library has no
# namespace prefix. It checks three surfaces, because each one breaks an
# application in a different way:
#
#   1. The exported dynamic symbols, and every global symbol that the static
#      archive defines. A symbol without a prefix can collide with a symbol of
#      the same name in the application, or the application can interpose it.
#      The script reads both shipped artifacts, because -fvisibility=hidden
#      makes the two disagree. A helper that stays out of the dynamic symbol
#      table of the shared object is still GLOBAL in the archive, and only
#      HIDDEN. A static link resolves it in the same way as an exported symbol.
#      An application that defines its own symbol of that name therefore fails
#      to link as soon as the link pulls that member in.
#   2. The public macros and typedefs in the installed headers. These are
#      worse. A macro rewrites the text of any application code that uses that
#      name. An application with its own log_info() or mutex_t therefore breaks
#      only because it includes one of our headers.
#   3. The enum enumerators, the struct, union and enum tags, and the typedef
#      names in the installed headers. These go into the ordinary identifier name space and
#      the tag name space of the application. A program that uses one of those
#      names for its own purpose therefore fails to COMPILE only because it
#      includes a header. That is worse again than a clash at link time.
#
# Layer 3 reads the debug information of the compiler and not the header text.
# A regular expression over an enum body cannot do this work. It reports the
# first word of every doc comment inside the body as an enumerator, and it
# drops enumerators that are written in a shape it does not know about. A
# namespace gate that reports a problem that is not there is worse than a gap
# that you know about. The extraction must be exact, and only the compiler
# knows exactly what an enum body declares.
#
# On macOS the library is a Mach-O file. Layer 1 then reads both artifacts with
# the nm of the system, and layer 2 runs as everywhere. Layer 3 needs DWARF that
# GNU readelf can read, which a Mach-O object does not carry, so it runs on the
# ELF build only. There it scans the headers twice: as they are, and with
# _CCOL_EMULATE_DARWIN_SYNC, the one switch that changes what an installed
# header declares on macOS. The ELF run therefore checks the names that a macOS
# build sees as well.
#
# Run this from the root of the repository, after `make`. `make check_namespace`
# and CI both use it.
set -eu

SO="${1:-libccollections.so}"
[ -e "$SO" ] || { echo "check_public_namespace: $SO not built; run make first" >&2; exit 2; }

. ci_scripts/object_format.sh
if ! FORMAT=$(ccol_object_format "$SO"); then
  echo "check_public_namespace: $SO is neither an ELF nor a Mach-O file" >&2
  exit 2
fi
if [ "$FORMAT" = elf ]; then
  . ci_scripts/gnu_binutils.sh
fi

# The script reads the archive as well as the shared object. If the archive is
# not there, the script fails and does not skip. In a green log, an artifact
# that is missing and an artifact that is clean look the same. This artifact
# also ships.
AR_LIB="${2:-libccollections.a}"
[ -e "$AR_LIB" ] || {
  echo "check_public_namespace: $AR_LIB not built; run make first" >&2
  exit 2
}

# A prefix is acceptable when it is ccol_ or CCOL_, the full name of the
# library, or the name of the module that owns the symbol. ccollections_
# appears on enum tags. Tags are the one surface that uses the full name of the
# library and not the short form. That prefix is as clearly ours as ccol_ is.
NS='^_*(ccol_|CCOL_|ccollections_|cvec|cvector|chmap|chashmap|cbmap|cbstmap|cstr|cstring|csort|cjson|cyaml|clog|clru|cmap|cmempool|cthreadcomm|cthreadpool|ctpool|chttp|ctls|citer|CVEC|CHMAP|CBMAP|CSTR|CSORT|CJSON|CYAML|CLOG|CLRU|CMAP|CMEMPOOL|CTHREAD|CTPOOL|CHTTP|CTLS|CITER)'

# Struct tags that are already c-prefixed and are not generic English words.
ALLOW='^(cbinarymap|c_message_t|cthread_pool)$'

# The headers that ship are exactly include/*.h. An internal header lives in
# include/internal/ instead. It has no visibility block, and make install
# leaves it out. The glob below therefore separates the two by itself, and no
# list has to stay in step with the Makefile.

fail=0

# The script checks the status of readelf on its own. It does not use the
# status of the pipeline, which would be the status of sort. Without this, an
# artifact that the script cannot read, or that is not an ELF file, gives an
# empty symbol list. The script then reports a fully prefixed interface after
# it examined nothing.
if [ "$FORMAT" = macho ]; then
  if ! all_syms=$(ccol_macho_defined_globals "$SO"); then
    echo "check_public_namespace: could not read the symbols of $SO" >&2
    exit 2
  fi
else
  if ! raw_syms=$("$READELF" --dyn-syms -W "$SO"); then
    echo "check_public_namespace: could not read dynamic symbols from $SO" >&2
    exit 2
  fi
  # _init and _fini come from the C runtime start files (crti.o), not from
  # this library; FreeBSD's give them default visibility, so every FreeBSD
  # shared object exports them. They are the only names left out.
  all_syms=$(printf '%s\n' "$raw_syms" \
    | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK") && $8!="_init" && $8!="_fini"{print $8}' \
    | sed 's/@.*//' | sort -u)
fi
# An empty set is never a correct answer for this library. It is exactly what a
# stripped artifact, a truncated artifact, or an artifact in the wrong format
# gives you.
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

# The script checks the status of nm on its own and not the status of the
# pipeline, for the reason above. An artifact that the script cannot read, or
# that is not an archive, gives an empty symbol list. The script then reports a
# fully prefixed archive after it examined
# nothing. The header line of an archive member, such as
# "libccollections.a[cvector.o]:", holds one field, and the field count test
# drops it. A U entry is a reference and not a definition.
if [ "$FORMAT" = macho ]; then
  if ! ar_syms=$(ccol_macho_defined_globals "$AR_LIB"); then
    echo "check_public_namespace: could not read symbols from $AR_LIB" >&2
    exit 2
  fi
else
  if ! raw_ar_syms=$("$NM" --defined-only --extern-only -P "$AR_LIB"); then
    echo "check_public_namespace: could not read symbols from $AR_LIB" >&2
    exit 2
  fi
  ar_syms=$(printf '%s\n' "$raw_ar_syms" \
    | awk 'NF >= 2 && $2 != "U" { print $1 }' \
    | sed 's/@.*//' | sort -u)
fi
if [ -z "$ar_syms" ]; then
  echo "check_public_namespace: $AR_LIB defines no global symbols; refusing to" >&2
  echo "                        report on an empty archive." >&2
  exit 2
fi
bad_ar_syms=$(printf '%s\n' "$ar_syms" | grep -Ev "$NS" | grep -Ev "$ALLOW" || true)
if [ -n "$bad_ar_syms" ]; then
  echo "Global symbols in $AR_LIB without a namespace prefix:" >&2
  echo "$bad_ar_syms" | sed 's/^/  /' >&2
  echo "  (hidden visibility keeps these out of the shared object, but a static" >&2
  echo "   link still resolves them, so each one can collide with an" >&2
  echo "   application's own symbol of that name. Give it the module's prefix," >&2
  echo "   or make it static if no white-box test calls it.)" >&2
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

# ---------------------------------------------------------------------------
# Layer 3: enum enumerators and struct/union/enum tags, read out of DWARF
# ---------------------------------------------------------------------------
# The script compiles one translation unit that includes every installed
# header. It uses -g3 -fno-eliminate-unused-debug-types, so that the debug
# information also describes a type that nothing references. It then reads that
# debug information back. The debug information describes every declaration
# that the system headers give as well. This is why the script attributes each
# name to the file that it came from, and judges only our own names.
CC="${CC:-cc}"
dwarf_tmp=$(mktemp -d)
trap 'rm -rf "$dwarf_tmp"' EXIT INT TERM

public_headers=''
for h in $headers; do
  m=$(basename "$h" .h)
  public_headers="$public_headers $m.h"
  echo "#include \"$m.h\"" >> "$dwarf_tmp/probe.c"
done

if [ ! -s "$dwarf_tmp/probe.c" ]; then
  echo "check_public_namespace: no public headers to scan for enumerators." >&2
  exit 2
fi

# A Mach-O build has no DWARF that GNU readelf can read. The ELF build runs
# this layer for the macOS headers too; see the top of this file.
if [ "$FORMAT" = macho ]; then
  echo "check_public_namespace: NOTE - enumerators, tags and typedefs are checked" \
    "by the ELF build, for the macOS headers as well."
  if [ "$fail" -eq 0 ]; then
    echo "check_public_namespace: OK (symbols, macros and typedefs are fully namespaced)"
  fi
  exit "$fail"
fi

# Each variant compiles the same probe. "plain" gives the headers of Linux and
# FreeBSD; "darwin" gives the headers of macOS, where common.h declares the
# types of _CCOL_EMULATE_DARWIN_SYNC.
for v in plain darwin; do
case "$v" in
plain) vflags='' ;;
darwin) vflags='-D_CCOL_EMULATE_DARWIN_SYNC=1' ;;
esac

# -w is here because this probe exists to describe types. It does not lint the
# headers again, because the ordinary build already does that under -Werror.
# -std=gnu11 is here because the headers are written to that standard, and
# because the library and every test suite build with it. A toolchain whose
# default is a later standard changes the meaning of bool, of static_assert,
# and of an empty parameter list. A probe on the default of the compiler
# therefore describes a different set of types from the set that ships.
if ! $CC -Iinclude -std=gnu11 -g3 -gdwarf-5 -fno-eliminate-unused-debug-types -w $vflags \
     -c "$dwarf_tmp/probe.c" -o "$dwarf_tmp/probe_$v.o" 2>"$dwarf_tmp/cc_$v.err"; then
  echo "check_public_namespace: could not compile the debug-info probe ($v), so" >&2
  echo "                        enumerators and tags were not checked." >&2
  sed 's/^/  /' "$dwarf_tmp/cc_$v.err" >&2
  exit 2
fi

if ! "$READELF" --debug-dump=rawline "$dwarf_tmp/probe_$v.o" > "$dwarf_tmp/line_$v" 2>/dev/null ||
   ! "$READELF" --debug-dump=info "$dwarf_tmp/probe_$v.o" > "$dwarf_tmp/info_$v" 2>/dev/null; then
  echo "check_public_namespace: could not read debug information back from the" >&2
  echo "                        probe object; enumerators were not checked." >&2
  exit 2
fi

# The file table and the directory table map a DW_AT_decl_file index to a real
# path. Our own headers are the ones that the compiler reaches through the -I
# path, and it records that path as a relative path. The directory of every
# system header is an absolute path.
awk -v incdir="include" -v absinc="$PWD/include" -v pubs=" $public_headers " '
function first_token(s) {
  sub(/^[ \t]+/, "", s)
  sub(/[ \t].*$/, "", s)
  return s
}
function value(s) {
  # "DW_AT_name  : (indirect string, offset: 0x1): foo" -> "foo", and a
  # plain "DW_AT_name  : foo" -> "foo". The greedy match takes the last
  # "): ", so an offset that happens to contain one cannot confuse it.
  if (s ~ /\): /) sub(/^.*\): /, "", s)
  sub(/[ \t]+$/, "", s)
  return s
}
function flush(   f, parent) {
  if (cur_tag == "") return
  if (cur_file != "") filedepth[cur_depth] = cur_file
  if (cur_tag == "DW_TAG_enumerator") {
    parent = filedepth[cur_depth - 1]
    if (ours[parent] && cur_name != "") print "enumerator\t" fname[parent] "\t" cur_name
    enumerators++
  } else if (cur_tag == "DW_TAG_structure_type" ||
             cur_tag == "DW_TAG_union_type" ||
             cur_tag == "DW_TAG_enumeration_type") {
    f = (cur_file != "") ? cur_file : filedepth[cur_depth]
    if (ours[f] && cur_name != "") print "tag\t" fname[f] "\t" cur_name
  } else if (cur_tag == "DW_TAG_typedef") {
    # Layer 2 reads a typedef name off the end of its line, which a
    # function-pointer typedef such as "typedef void (*fn)(void *arg);" does
    # not have. The debug information names every typedef exactly.
    f = (cur_file != "") ? cur_file : filedepth[cur_depth]
    if (ours[f] && cur_name != "") { print "typedef\t" fname[f] "\t" cur_name; typedefs++ }
  }
  cur_tag = ""; cur_name = ""; cur_file = ""
}
FILENAME == linefile {
  if ($0 ~ /The Directory Table/) { mode = "dir";  next }
  if ($0 ~ /The File Name Table/) { mode = "file"; next }
  if ($0 ~ /^[ \t]*$/)            { mode = "";     next }
  if (mode == "" ) next
  n = split($0, f, "\t")
  idx = first_token(f[1])
  if (idx !~ /^[0-9]+$/) next
  if (mode == "dir" && n >= 2) {
    d = value(f[2])
    if (d == incdir || d == absinc) isours_dir[idx] = 1
  } else if (mode == "file" && n >= 3) {
    # The directory is the first token of its column, and the file name is
    # the LAST column. The shape of this table depends on the compiler. With
    # DWARF 5 a compiler can record an MD5 of each file, and readelf prints
    # that checksum inside the directory column. clang records it and gcc does
    # not. Do not read the whole column, and do not assume that the name is in
    # column three. Either form drops every one of our own headers as soon as
    # the checksum is there. The scan then reports that it found no project
    # file at all.
    dir = first_token(f[2])
    base = value(f[n])
    fname[idx] = base
    # Both halves are necessary. The directory proves that this is not a
    # system header with the same base name. The name list proves that this is
    # a header that we install, and not an internal header that the compiler
    # reached through some other path.
    if (isours_dir[dir] && index(pubs, " " base " ") > 0) { ours[idx] = 1; ourfiles++ }
  }
  next
}
{
  if ($0 ~ /^ <[0-9]+><[0-9a-f]+>: Abbrev Number: [0-9]+/) {
    flush()
    d = $0; sub(/^ </, "", d); sub(/>.*$/, "", d)
    cur_depth = d + 0
    if ($0 ~ /\(DW_TAG_/) {
      t = $0; sub(/^.*\(/, "", t); sub(/\).*$/, "", t); cur_tag = t
    } else cur_tag = ""
    next
  }
  if (cur_tag == "") next
  if ($0 ~ /DW_AT_decl_file/) {
    v = $0; sub(/^.*DW_AT_decl_file[ \t]*:[ \t]*/, "", v); sub(/[^0-9].*$/, "", v)
    cur_file = v
  } else if ($0 ~ /DW_AT_name/) {
    v = $0; sub(/^.*DW_AT_name[ \t]*:[ \t]*/, "", v); cur_name = value(v)
  }
}
END {
  flush()
  # Without these two tests, a readelf output that this scan cannot parse
  # makes the scan report a fully prefixed interface after it examined
  # nothing. That is the one failure that you can never tell apart from a
  # green run.
  if (ourfiles == 0)   { print "PROBE-ERROR no installed header was found in the debug info" > "/dev/stderr"; exit 3 }
  if (enumerators == 0){ print "PROBE-ERROR no enumerators were found at all" > "/dev/stderr"; exit 3 }
  if (typedefs == 0)   { print "PROBE-ERROR no typedefs were found in an installed header" > "/dev/stderr"; exit 3 }
}
' linefile="$dwarf_tmp/line_$v" "$dwarf_tmp/line_$v" "$dwarf_tmp/info_$v" > "$dwarf_tmp/names_$v" || {
  echo "check_public_namespace: the debug-info scan failed; enumerators and" >&2
  echo "                        tags were not checked." >&2
  exit 2
}
done
cat "$dwarf_tmp"/names_plain "$dwarf_tmp"/names_darwin > "$dwarf_tmp/names"

bad_dwarf=$(awk -F'\t' -v ns="$NS" -v allow="$ALLOW" '
      $3 !~ ns && $3 !~ /^_/ && $3 !~ allow { print $2 ": " $1 " " $3 }' \
      "$dwarf_tmp/names" | sort -u || true)
if [ -n "$bad_dwarf" ]; then
  echo "Public enumerators/tags/typedefs without a namespace prefix:" >&2
  echo "$bad_dwarf" | sed 's/^/  /' >&2
  fail=1
fi

if [ "$fail" -eq 0 ]; then
  echo "check_public_namespace: OK (public interface is fully namespaced)"
fi
exit "$fail"
