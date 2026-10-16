#!/bin/sh
# Fails when a library source file or an installed header has coverage below
# MIN_COVERAGE percent of its instrumented lines.
#
# The file list comes from src/*.c, include/*.h and include/internal/*.h on
# disk, not from the tracefile. A file that no test suite compiles has no
# tracefile entry at all, so a check that only read the report would skip it
# silently and you couldn't tell that from a pass. Listing the directory turns
# that case into a failure.
#
# Run this from the root of the repository after `make coverage_site`;
# `make coverage_check` and CI both use it.
#
# CCOL_DISABLED_SOURCES can hold a space-separated list of source paths that
# this build left out on purpose (see the WITH_* switches in the root
# Makefile). Those files are on disk anyway, so without this list the directory
# scan below would report each of them as compiled by no suite, which is the
# very failure the scan is there to catch for a file that fell out by accident.
# Naming them explicitly keeps the check meaningful for every other file.
set -eu

MIN_COVERAGE=80
CCOL_DISABLED_SOURCES="${CCOL_DISABLED_SOURCES:-}"

# Modules with no unit tests of their own. Whatever coverage they show comes by
# chance from another module's suite, so a threshold on them would really be
# measuring that other suite.
#
# cdebuglog is a diagnostic buffer you turn on yourself while chasing a timing
# problem that is hard to reproduce. It isn't part of the shipped library: a
# suite compiles it only when it adds it to its own SRC_FILES for the length
# of one investigation.
#
# Only add a file here when it really has no unit tests; a file tested from
# another module's directory doesn't belong here. There is no
# tests/chttp1_parser/ directory, for example, but tests/chttp1_parser isn't in
# this list because tests/chttpclient/tests_parser.c exercises
# src/chttp1_parser.c directly, so the threshold applies to it like to any
# other file.
NOT_UNIT_TESTED="
src/cdebuglog.c
include/internal/cdebuglog.h
"

INFO="${1:-coverage_site/library.info}"

[ -e "$INFO" ] || {
	echo "check_test_coverages: $INFO not found; run 'make coverage_site' first" >&2
	exit 2
}

# Files this build left out go into the same list as the files with no unit
# tests, since in both cases the file is on disk and it's correct that the
# report doesn't have it.
EXCLUDED="$NOT_UNIT_TESTED
$(printf '%s\n' $CCOL_DISABLED_SOURCES)"

awk -v info="$INFO" -v minimum="$MIN_COVERAGE" -v excluded="$EXCLUDED" '
	# The key for each file, both when the script records it and when the
	# script looks it up, is the path of that file relative to the root of
	# the repository, which is exactly what the directory listing in END
	# gives. Do not keep a fixed number of path components from the end
	# instead: that form cannot describe both include/cvector.h and
	# include/internal/cgrowbuf.h. Whichever depth it takes, the records of
	# the other file go under a key that no listing ever gives, and that
	# file then goes into the "no instrumented lines" group, which does not
	# fail, however low its real coverage is.
	function relative_path(p, r) { return substr(p, length(r) + 2) }

	BEGIN {
		n = split(excluded, e, /[ \t\n]+/)
		for (i = 1; i <= n; i++)
			if (e[i] != "") skip[e[i]] = 1
	}

	/^SF:/ {
		path = substr($0, 4)
		cur = ""
		# This matches only this library; a match with no start point
		# would also take in /usr/include/... and bring glibc and
		# OpenSSL headers into the check.
		if (index(path, root "/src/") == 1 || index(path, root "/include/") == 1)
			cur = relative_path(path, root)
		next
	}
	/^DA:/ && cur != "" {
		split(substr($0, 4), d, ",")
		line = d[1] + 0; hits = d[2] + 0
		k = cur SUBSEP line
		# A line counts as covered when any build in the merged report
		# ran it, which is how the published report makes its own
		# totals.
		if (!(k in seen) || hits > seen[k]) seen[k] = hits
		next
	}
	/^end_of_record/ { cur = "" }

	END {
		for (k in seen) {
			split(k, p, SUBSEP)
			total[p[1]]++
			if (seen[k] > 0) hit[p[1]]++
		}
		status = 0
		nfiles = 0
		while (("ls src/*.c include/*.h include/internal/*.h 2>/dev/null" | getline f) > 0) {
			nfiles++
			if (f in skip) { nskipped++; continue }
			if (!(f in total)) {
				# A header that holds only macros and
				# declarations correctly has no instrumented
				# lines, while a .c file always has some. A .c
				# file with none has fallen out of every suite,
				# which is a failure and not a file to pass
				# over.
				if (f ~ /\.c$/) {
					printf "  NOT COMPILED BY ANY SUITE  %s\n", f
					status = 1
				} else {
					printf "  no instrumented lines      %s\n", f
					nolines++
				}
				continue
			}
			pct = 100.0 * hit[f] / total[f]
			# The 0.05 margin covers a printed value that rounds up
			# to exactly the threshold, and it is much smaller than
			# the 0.08 point spread between two runs.
			if (pct + 0.05 < minimum) {
				printf "  BELOW %d%%             %-24s %5.1f%% (%d/%d)\n", minimum, f, pct, hit[f], total[f]
				status = 1
			} else {
				npassed++
			}
		}
		# An empty listing is the one result that the script must refuse
		# instead of report: a wrong working directory gives an empty
		# listing, and every check below it would then pass after it
		# examined nothing at all.
		if (nfiles == 0) {
			printf "check_test_coverages: found no src/*.c or include/*.h to check;\n"
			printf "                      run this from the repository root.\n"
			exit 2
		}
		printf "check_test_coverages: %d file(s) at or above %d%%, %d with no instrumented lines, %d not unit tested\n", npassed, minimum, nolines, nskipped
		if (status != 0)
			printf "check_test_coverages: FAILED, every file above must reach %d%%\n", minimum
		exit status
	}
' root="$PWD" "$INFO"
