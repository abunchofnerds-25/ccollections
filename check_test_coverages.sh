#!/bin/sh
# Fails if any library source file or installed header is covered by less than
# MIN_COVERAGE percent of its instrumented lines.
#
# The file list is taken from src/*.c and include/*.h on disk rather than from
# whatever the tracefile happens to contain. A file compiled into no test suite
# at all produces no tracefile entry, and a check that iterated over the
# report's own contents would skip it silently, which is indistinguishable from
# passing. Reading the directory instead turns that case into a failure.
#
# Run from the repository root, after `make coverage_site`. Used by
# `make coverage_check` and by CI.
set -eu

MIN_COVERAGE=80

# Modules with no unit tests of their own. Whatever coverage they show comes
# incidentally from another module's suite, so holding them to the threshold
# would be measuring that other suite rather than them.
#
# cdebuglog is an opt-in diagnostic buffer for chasing hard-to-reproduce timing
# issues; it is excluded from the shipped library entirely and only compiled
# when a suite adds it to its own SRC_FILES for the duration of one
# investigation.
#
# Add a file here only when it genuinely has no unit tests. A file that is
# tested from another module's directory does not belong: src/chttp1_parser.c
# has no tests/chttp1_parser/ but is exercised directly by
# tests/chttpclient/tests_parser.c, and is held to the threshold like any other.
NOT_UNIT_TESTED="
src/cdebuglog.c
include/cdebuglog.h
"

INFO="${1:-coverage_site/library.info}"

[ -e "$INFO" ] || {
	echo "check_test_coverages: $INFO not found; run 'make coverage_site' first" >&2
	exit 2
}

awk -v info="$INFO" -v minimum="$MIN_COVERAGE" -v excluded="$NOT_UNIT_TESTED" '
	function basename(p,   n, a) { n = split(p, a, "/"); return a[n - 1] "/" a[n] }

	BEGIN {
		n = split(excluded, e, /[ \t\n]+/)
		for (i = 1; i <= n; i++)
			if (e[i] != "") skip[e[i]] = 1
	}

	/^SF:/ {
		path = substr($0, 4)
		cur = ""
		# Only this library. An unanchored match would also take in
		# /usr/include/..., pulling glibc and OpenSSL headers into the check.
		if (index(path, root "/src/") == 1 || index(path, root "/include/") == 1)
			cur = basename(path)
		next
	}
	/^DA:/ && cur != "" {
		split(substr($0, 4), d, ",")
		line = d[1] + 0; hits = d[2] + 0
		k = cur SUBSEP line
		# A line counts as covered if any build in the merged report ran it,
		# matching how the published report computes its own totals.
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
		while (("ls src/*.c include/*.h 2>/dev/null" | getline f) > 0) {
			if (f in skip) { nskipped++; continue }
			if (!(f in total)) {
				# A header made only of macros and declarations legitimately
				# has no instrumented lines. A .c file never does: if one has
				# none it has dropped out of every suite, which is a failure
				# rather than an exemption.
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
			# The 0.05 slack absorbs the printed value rounding up to exactly
			# the threshold; it is far below the 0.08-point run-to-run spread.
			if (pct + 0.05 < minimum) {
				printf "  BELOW %d%%             %-24s %5.1f%% (%d/%d)\n", minimum, f, pct, hit[f], total[f]
				status = 1
			} else {
				npassed++
			}
		}
		printf "check_test_coverages: %d file(s) at or above %d%%, %d with no instrumented lines, %d not unit tested\n", npassed, minimum, nolines, nskipped
		if (status != 0)
			printf "check_test_coverages: FAILED, every file above must reach %d%%\n", minimum
		exit status
	}
' root="$PWD" "$INFO"
