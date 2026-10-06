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

/* This suite checks the accept and reject behavior of cyaml_parse_n against
 * the vendored YAML Test Suite snapshot (see yaml-test-suite/README.md).
 * For each case it reads in.yaml and meta.txt. Both are plain files that
 * PyYAML pre-processed offline, and cyaml itself never touched them. This
 * suite therefore has no circular dependency on the parser that it tests.
 * The suite then checks whether the success or failure of cyaml_parse_n
 * matches the fail=0 or fail=1 expectation of the case. A full comparison
 * at the event-stream level against the expected tree of the suite is out
 * of scope here. Agreement on accept and reject is the part that this
 * suite implements.
 *
 * A small number of cases are known deviations, and each one has its own
 * investigation. KNOWN_DEVIATIONS below lists every one of them
 * explicitly, and the suite does not skip them silently. Every entry is a
 * "reference-parser disagreement". Two independent, widely-used reference
 * implementations disagree with the fail=0 or fail=1 expectation of the
 * case. Those two are PyYAML from Python and Psych from Ruby. Psych is the
 * libyaml-based parser in the standard library of Ruby. Both read the
 * exact vendored bytes of the case, not an approximation that somebody
 * retyped by hand. Both disagree with the expectation of the suite
 * independently. QB6E and DK95-1 are multi-line double-quoted scalars that
 * both accept and the suite marks fail=1. ZYU8-2 is the reverse: both
 * reject a directive line that the suite marks as accepted. Two unrelated,
 * mature implementations therefore agree with each other and against the
 * suite. For these specific cases this suite judges the behavior of cyaml
 * against those two implementations, and not against the suite.
 *
 * Tabs are not on the list. cyaml accepts a tab wherever YAML 1.2 allows
 * separation whitespace, as the suite expects, although PyYAML and Psych
 * refuse several of those cases.
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

/* The vendored snapshot is pinned to one upstream tag (see the header
 * comment of this file). Its case count is therefore a fixed, known
 * quantity and not a moving target. An exact match catches a regression in
 * the load of the corpus that drops some, but not most, of the cases. Such
 * a regression is a bad path, a partial extraction, or a subset of cases
 * that somebody deleted by accident. A loose ">" bound would let that
 * regression through silently. */
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
    {NULL, NULL}};

#define KNOWN_DEVIATIONS_COUNT \
  (sizeof(KNOWN_DEVIATIONS) / sizeof(KNOWN_DEVIATIONS[0]) - 1)

/* Mark KNOWN_DEVIATIONS[i] as consulted in this run. The caller can then
 * detect a STALE entry on its own. A stale entry is one that does not
 * mismatch the expectation of the suite at all, for example because a later
 * fix resolves it. Such an entry must be removed. If it stays, it describes
 * a deviation that does not exist, and it does so silently. */
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

/* Return whether the case expects a parse failure. This function sets
 * *ok_out to false when it cannot read meta_path. It does not default to
 * "expects success" silently. Take a case whose in.yaml did load, where the
 * expectation file is missing or corrupt. That is a hard defect in the corpus
 * or in the harness of the tests, and it does not pass unnoticed. */
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

  /* This is an exact match, not a loose lower bound. It catches a
   * regression in the load of the corpus that silently drops some, but not
   * most, of the cases of the pinned snapshot. See the comment on
   * EXPECTED_CASE_COUNT. */
  REQUIRE_EQ(total, (size_t)EXPECTED_CASE_COUNT);
  REQUIRE_EQ(unexpected_failures, (size_t)0);
  REQUIRE_EQ(stale_deviations, (size_t)0);
}
