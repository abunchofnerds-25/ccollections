#!/bin/sh
# This script refuses a repository filename that the install would paste into a
# command line as a pathname.
#
# `make install` and `make uninstall` build their argument lists from the
# basenames of the files in man/ and include/. They put the installation
# directory in front of each basename, and the shell that runs the recipe then
# expands that argument list. A basename that holds a shell pathname-expansion
# character therefore stops naming one file of this project and starts naming
# a PATTERN that matches inside the installation directory, so `rm -f` deletes,
# and `sed -i` rewrites, files that belong to some other package.
#
# The recipes already run under `set -f`, which turns that expansion off. This
# script is the second half of the same guard: it keeps such a name out of the
# tree in the first place, so that the property does not depend on one `set -f`
# surviving a future edit of the Makefile. A name with a space or a tab is
# refused for the same reason, because the shell splits the argument list on
# it.
#
# The script also checks the shape of every man alias page; see the comment at
# that check.
#
# Run this from the root of the repository; `make check_filenames` and CI both
# use it.
set -eu

status=0
alias_status=0

# The character class covers every pathname-expansion character of POSIX sh
# (`*`, `?`, `[`), plus the two whitespace characters that split an argument
# list, plus the characters that would need quoting inside the recipes.
# find -name works on the basename alone, which is exactly what the recipes
# paste onto the installation directory.
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
# install rewrites that line to ".so man3/<symbol>.3" for the installed
# MANPATH, so the line must have exactly that shape. A ".TH" line in front of
# it makes man print a header and a footer of its own around the target page,
# and makes mandb record the alias as a page of its own instead of as a link.
# The target must also exist, or the installed alias points at nothing.
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

if [ "$status" -ne 0 ]; then
  echo "" >&2
  echo "Rename each one. A basename that the install pastes onto the" >&2
  echo "installation directory must name exactly one file and never a" >&2
  echo "pattern. Give a man page the name of a real symbol, and add a" >&2
  echo "'.so <module>/<symbol>.3' alias page for each further symbol that" >&2
  echo "it documents." >&2
  exit 1
fi

if [ "$alias_status" -ne 0 ]; then
  exit 1
fi

echo "check_installable_filenames: OK (every installable name is a literal path," \
     "every alias page is one .so line to a page that exists)"
