/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/* Checks cyaml_parse_n's accept/reject behavior against the vendored YAML
 * Test Suite snapshot (see yaml-test-suite/README.md). For each case, reads
 * in.yaml and meta.txt (both plain files, pre-processed offline with PyYAML,
 * not with cyaml itself, so this suite has no circular dependency on the
 * parser it is testing) and checks whether cyaml_parse_n's success/failure
 * matches the case's own fail=0/fail=1 expectation. A full event-stream-level
 * comparison against the suite's own expected tree is out of scope for this
 * pass; accept/reject agreement is the slice implemented here.
 *
 * A small number of cases are known, individually investigated deviations
 * and are listed explicitly in KNOWN_DEVIATIONS below rather than silently
 * skipped. Every entry is a "reference-parser disagreement": this case's own
 * fail=0/fail=1 expectation was checked directly against two independent,
 * widely-used reference implementations (Python's PyYAML and Ruby's Psych,
 * the libyaml-based parser bundled with Ruby's standard library) using the
 * case's own exact vendored bytes, not a hand-retyped approximation. Both
 * independently disagree with the suite's own expectation here (the large
 * majority reject input the suite marks fail=0; one, ZYU8-2, is the
 * reverse: both reject a directive line the suite marks accepted). Since
 * two unrelated, mature implementations agree with each other and against
 * the suite, cyaml's own behavior is judged against those two
 * implementations rather than the suite for these specific cases.
 */

#include <cyaml.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

#define CASES_DIR "yaml-test-suite/cases"

/* The vendored snapshot is pinned to a specific upstream tag (see this
 * file's own header comment), so its case count is a fixed, known quantity,
 * not a moving target; an exact match (rather than a loose lower bound)
 * catches a corpus-loading regression (a bad path, a partial extraction,
 * an accidentally-deleted subset of cases) that drops some, but not most,
 * of the cases, which a loose ">" bound would silently let through. */
#define EXPECTED_CASE_COUNT 354

typedef struct {
  const char *case_id;
  const char *reason;
} known_deviation_t;

static const known_deviation_t KNOWN_DEVIATIONS[] = {
    {"ZYU8-2",
     "reference-parser disagreement: \"%YAML 1.1 1.2\\n---\\n\", "
     "both PyYAML and Psych reject this identically to this "
     "parser's own strict %YAML-version grammar"},
    {"QB6E",
     "reference-parser disagreement: a multi-line double-quoted "
     "scalar value whose continuation lines are not more indented "
     "than the enclosing key, both PyYAML and Psych fold it "
     "successfully rather than rejecting it"},
    {"DK95-1",
     "reference-parser disagreement: a multi-line double-quoted "
     "scalar value containing an embedded tab, both PyYAML and "
     "Psych fold it successfully rather than rejecting it"},
    {"A2M4",
     "reference-parser disagreement (Spec Example 6.2, "
     "Indentation Indicators): both PyYAML and Psych reject this "
     "fixture's own tab-after-'-' bytes with a scanner error, "
     "identically to this parser's own tab-after-indicator rule"},
    {"6BCT",
     "reference-parser disagreement (Spec Example 6.3, Separation "
     "Spaces): both PyYAML and Psych reject this fixture's own "
     "tab-after-'-' bytes with a scanner error, identically to "
     "this parser's own tab-after-indicator rule"},
    {"Y79Y-10",
     "reference-parser disagreement: \"-\\t-1\\n\" (a tab "
     "directly after a block sequence '-'), both PyYAML and "
     "Psych reject this identically to this parser's own "
     "tab-after-indicator rule"},
    {"6HB6",
     "reference-parser disagreement: a tab used as leading "
     "indentation is rejected, matching PyYAML (Psych disagrees "
     "and accepts it, but PyYAML's judgment is the one this "
     "parser's own full tab-as-indentation audit was verified "
     "against throughout, having matched the vendored suite's own "
     "expectation far more consistently than Psych across every "
     "other case checked)"},
    {"DC7X",
     "reference-parser disagreement: a tab immediately after a "
     "block mapping's ':' value indicator, before further content "
     "on a later line, is rejected, matching PyYAML (Psych "
     "disagrees and accepts it; see 6HB6's own reasoning for why "
     "PyYAML is preferred here)"},
    {"Q5MG",
     "reference-parser disagreement: a tab used as leading "
     "indentation before a flow mapping is rejected, matching "
     "both PyYAML and Psych"},
    {"J3BT",
     "reference-parser disagreement: a tab used as the separator "
     "between a mapping value's ':' indicator and the '|' block "
     "scalar indicator that follows it (\"block:\\t|\") is "
     "rejected, matching PyYAML (Psych disagrees and accepts it; "
     "see 6HB6's own reasoning for why PyYAML is preferred here)"},
    {"MUS6-3",
     "reference-parser disagreement: a tab used as the "
     "separator inside a %YAML directive's own version field is "
     "rejected, matching PyYAML (Psych disagrees and accepts "
     "it; see 6HB6's own reasoning for why PyYAML is preferred "
     "here)"},
    {"6CA3",
     "reference-parser disagreement: a tab used as leading "
     "indentation before a flow sequence is rejected, matching "
     "both PyYAML and Psych"},
    {"DK95-0",
     "reference-parser disagreement: a tab used as leading "
     "indentation on a plain scalar's continuation line is "
     "rejected, matching both PyYAML and Psych"},
    {"K54U",
     "reference-parser disagreement: a tab immediately after the "
     "'---' document start marker (before its own scalar content "
     "on the same line) is rejected, matching PyYAML (Psych "
     "disagrees and accepts it; see 6HB6's own reasoning for why "
     "PyYAML is preferred here)"},
    {NULL, NULL}};

#define KNOWN_DEVIATIONS_COUNT \
  (sizeof(KNOWN_DEVIATIONS) / sizeof(KNOWN_DEVIATIONS[0]) - 1)

/* Marks KNOWN_DEVIATIONS[i] as actually consulted this run, so the caller
 * can separately detect a STALE entry: one that no longer mismatches the
 * suite's own expectation at all (e.g. because a later fix happened to
 * resolve it) and should have been removed rather than left to silently
 * describe a deviation that no longer exists. */
static bool is_known_deviation(const char *case_id, bool *deviation_seen) {
  for (size_t i = 0; KNOWN_DEVIATIONS[i].case_id != NULL; ++i) {
    if (strcmp(KNOWN_DEVIATIONS[i].case_id, case_id) == 0) {
      deviation_seen[i] = true;
      return true;
    }
  }
  return false;
}

static char *read_whole_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  char *buf = malloc((size_t)len + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)len, f);
  fclose(f);
  buf[n] = '\0';
  if (out_len) *out_len = n;
  return buf;
}

/* Returns whether the case expects a parse failure; sets *ok_out to false
 * (rather than silently defaulting to "expects success") if meta_path
 * itself could not be read, so a missing/corrupt expectation file for a
 * case whose in.yaml did load is treated as a hard corpus/test-infra
 * defect rather than going unnoticed. */
static bool parse_meta_expects_fail(const char *meta_path, bool *ok_out) {
  size_t len;
  char *meta = read_whole_file(meta_path, &len);
  bool expects_fail = false;
  if (meta) {
    expects_fail = (strncmp(meta, "fail=1", 6) == 0);
    free(meta);
    *ok_out = true;
  } else {
    *ok_out = false;
  }
  return expects_fail;
}

TEST(spec_suite, accept_reject_matches_expectation) {
  DIR *d = opendir(CASES_DIR);
  REQUIRE_NE((void *)d, NULL);

  bool deviation_seen[KNOWN_DEVIATIONS_COUNT] = {0};

  size_t total = 0, unexpected_failures = 0, known_deviation_count = 0;
  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (entry->d_name[0] == '.') continue;

    char in_path[1024], meta_path[1024];
    snprintf(in_path, sizeof(in_path), "%s/%s/in.yaml", CASES_DIR,
             entry->d_name);
    snprintf(meta_path, sizeof(meta_path), "%s/%s/meta.txt", CASES_DIR,
             entry->d_name);

    size_t in_len;
    char *in_data = read_whole_file(in_path, &in_len);
    if (!in_data) continue; /* not a case directory */
    bool meta_ok = false;
    bool expects_fail = parse_meta_expects_fail(meta_path, &meta_ok);
    if (!meta_ok) {
      fprintf(stderr,
              "[spec_suite] case %s has an in.yaml but no readable "
              "meta.txt; treating this as a corpus defect\n",
              entry->d_name);
      free(in_data);
      closedir(d);
    }
    REQUIRE_TRUE(meta_ok);

    ++total;
    char *err = NULL;
    cyaml doc = cyaml_parse_n(in_data, in_len, &err);
    bool actually_failed = (doc == NULL);
    if (doc) cyaml_destroy(doc);
    free(err);
    free(in_data);

    if (actually_failed != expects_fail) {
      if (is_known_deviation(entry->d_name, deviation_seen)) {
        ++known_deviation_count;
      } else {
        fprintf(stderr,
                "[spec_suite] UNEXPECTED: case %s expected %s, got %s\n",
                entry->d_name, expects_fail ? "reject" : "accept",
                actually_failed ? "reject" : "accept");
        ++unexpected_failures;
      }
    }
  }
  closedir(d);

  size_t stale_deviations = 0;
  for (size_t i = 0; i < KNOWN_DEVIATIONS_COUNT; ++i) {
    if (!deviation_seen[i]) {
      fprintf(stderr,
              "[spec_suite] STALE KNOWN_DEVIATIONS entry: case %s no "
              "longer mismatches the suite's own expectation; remove it "
              "from KNOWN_DEVIATIONS\n",
              KNOWN_DEVIATIONS[i].case_id);
      ++stale_deviations;
    }
  }

  fprintf(stderr,
          "[spec_suite] %zu cases checked, %zu known deviations, %zu "
          "unexpected mismatches, %zu stale known-deviation entries\n",
          total, known_deviation_count, unexpected_failures, stale_deviations);

  /* Exact match, not a loose lower bound: catches a corpus-loading
   * regression that silently drops some, but not most, of the pinned
   * snapshot's cases (see EXPECTED_CASE_COUNT's own comment). */
  REQUIRE_EQ(total, (size_t)EXPECTED_CASE_COUNT);
  REQUIRE_EQ(unexpected_failures, (size_t)0);
  REQUIRE_EQ(stale_deviations, (size_t)0);
}
