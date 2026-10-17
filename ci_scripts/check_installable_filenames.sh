#!/bin/sh
# Refuses a repository file name that the install would paste into a command
# line as a pathname.
#
# `make install` and `make uninstall` build their argument lists from the
# basenames of the files in man/ and include/. They put the installation
# directory in front of each basename and the shell running the recipe then
# expands the list. So a basename holding a shell pathname-expansion character
# stops naming one of our files and becomes a PATTERN that matches inside the
# installation directory, and `rm -f` deletes (or `sed -i` rewrites) files that
# belong to some other package.
#
# The recipes run under `set -f`, which turns that expansion off. This script
# is the other half of the guard: it keeps such a name out of the tree in the
# first place, so we don't depend on that one `set -f` surviving every later
# edit of the Makefile. A name with a space or a tab is refused for the same
# reason, since the shell splits the argument list on it.
#
# It also checks the shape of every man alias page, and that every other page
# names its own file stem; the comments at those checks say more.
#
# Run it from the root of the repository; `make check_filenames` and CI both
# call it.
set -eu

status=0
alias_status=0

# The character class covers every pathname-expansion character of POSIX sh
# (`*`, `?`, `[`), the two whitespace characters that split an argument list,
# and the characters that would need quoting inside the recipes. find -name
# looks at the basename only, which is exactly what the recipes paste onto the
# installation directory.
for dir in include man; do
  [ -d "$dir" ] || continue
  bad=$(find "$dir" \( -name '*[*?[]*' -o -name '* *' -o -name '*	*' \
                       -o -name '*"*' -o -name "*'*" -o -name '*\\*' \
                       -o -name '*$*' -o -name '*`*' \) -print)
  if [ -n "$bad" ]; then
    echo "check_installable_filenames: these names are not safe to install:" >&2
    echo "$bad" >&2
    status=1
  fi
done

# An alias page is one line, ".so <module>/<symbol>.3", and nothing else. The
# install rewrites that line to ".so man3/<symbol>.3" for the installed MANPATH,
# so it has to have exactly that shape. A ".TH" line in front of it makes man
# print its own header and footer around the target page, and makes mandb
# record the alias as a page instead of a link. The target has to exist too, or
# the installed alias points at nothing.
if [ -d man ]; then
  for page in $(grep -l '^\.so' man/*/*.3 man/*/*.7 2>/dev/null || true); do
    lines=$(wc -l < "$page")
    target=$(sed -n 's/^\.so \([A-Za-z0-9_]*\/[A-Za-z0-9_]*\.[37]\)$/\1/p' "$page")
    if [ "$lines" -ne 1 ] || [ -z "$target" ]; then
      echo "check_installable_filenames: $page must hold exactly one line," \
           "'.so <module>/<symbol>.3'" >&2
      alias_status=1
    elif [ ! -f "man/$target" ]; then
      echo "check_installable_filenames: $page redirects to man/$target," \
           "which does not exist" >&2
      alias_status=1
    fi
  done
fi

# Every page that isn't an alias has to name its own file stem in its NAME
# section. `man <stem>` finds the page by its file name while whatis(1) and
# apropos(1) find it by the names in NAME, and a reader following a SEE ALSO
# reference needs the two to agree. The comparison ignores case, like
# whatis(1). A page that documents a family of constants may use the common
# prefix of all of them as its stem instead, as long as every name in NAME
# starts with that stem and an underscore.
name_status=0
if [ -d man ]; then
  for page in man/*/*.3 man/*/*.7; do
    [ -f "$page" ] || continue
    if grep -q '^\.so' "$page"; then continue; fi
    stem=$(basename "$page")
    stem=${stem%.*}
    names=$(awk 'f { print; exit } /^\.SH NAME/ { f = 1 }' "$page" |
            sed 's/ *\\-.*//; s/,/ /g')
    found=0
    family=1
    count=0
    lstem=$(printf '%s' "$stem" | tr 'A-Z' 'a-z')
    for n in $names; do
      count=$((count + 1))
      ln=$(printf '%s' "$n" | tr 'A-Z' 'a-z')
      [ "$ln" = "$lstem" ] && found=1
      case "$ln" in
        "${lstem}"_*) ;;
        *) family=0 ;;
      esac
    done
    if [ "$found" -eq 0 ] && { [ "$family" -eq 0 ] || [ "$count" -eq 0 ]; }; then
      echo "check_installable_filenames: $page does not name '$stem' in its" \
           "NAME section (it names: $names)" >&2
      name_status=1
    fi
  done
fi

if [ "$status" -ne 0 ]; then
  echo "" >&2
  echo "Rename each one. A basename that the install pastes onto the" >&2
  echo "installation directory must name exactly one file and never a" >&2
  echo "pattern. Give a man page the name of a real symbol, and add a" >&2
  echo "'.so <module>/<symbol>.3' alias page for each further symbol that" >&2
  echo "it documents." >&2
  exit 1
fi

if [ "$alias_status" -ne 0 ] || [ "$name_status" -ne 0 ]; then
  exit 1
fi

echo "check_installable_filenames: OK (every installable name is a literal path," \
     "every alias page is one .so line to a page that exists, every page" \
     "names its own file stem)"
