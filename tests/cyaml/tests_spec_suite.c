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

// Checks what cyaml_parse_n() accepts and rejects against the vendored YAML
// Test Suite snapshot (see yaml-test-suite/README.md). Each case's in.yaml and
// meta.txt were prepared offline with PyYAML, so the suite doesn't depend on
// the parser it tests; whether a parse succeeds must match the case's fail=0 or
// fail=1. Comparing the event stream is out of scope.
//
// KNOWN_DEVIATIONS lists the cases where both PyYAML and Ruby's Psych, reading
// the exact vendored bytes, disagree with the suite, and cyaml follows them.
// Tabs aren't on the list: cyaml accepts a tab wherever YAML 1.2 allows
// separation, as the suite expects, though both reference parsers refuse some
// of those cases.
#include <cyaml.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

// Where the cases live, relative to the test directory.
#define CASES_DIR "yaml-test-suite/cases"

// The case count of the pinned snapshot. An exact match catches a corpus load
// that drops a few cases (a bad path, a partial extraction), which a loose
// lower bound would miss.
#define EXPECTED_CASE_COUNT 354

// One known deviation: the case id and why cyaml follows the reference parsers
// there.
typedef struct {
  const char *case_id;
  const char *reason;
} known_deviation_t;

// Every case where cyaml deliberately disagrees with the suite, ended by a NULL
// entry.
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

// The number of real entries in KNOWN_DEVIATIONS, without the NULL end.
#define KNOWN_DEVIATIONS_COUNT \
  (sizeof(KNOWN_DEVIATIONS) / sizeof(KNOWN_DEVIATIONS[0]) - 1)

// Tells whether case_id is a known deviation and marks it as seen in
// deviation_seen, so the test can report an entry that doesn't mismatch and
// must be removed.
static bool is_known_deviation(const char *case_id, bool *deviation_seen) {
  for (size_t i = 0; KNOWN_DEVIATIONS[i].case_id != NULL; ++i) {
    if (strcmp(KNOWN_DEVIATIONS[i].case_id, case_id) == 0) {
      deviation_seen[i] = true;
      return true;
    }
  }
  return false;
}

// Reads a whole file into a NUL-terminated heap buffer and stores its length in
// *out_len (may be NULL), for the case files. NULL when the file can't be
// opened or memory runs out.
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

// Tells whether the case's meta.txt expects a parse failure (fail=1). An
// unreadable meta.txt sets *ok_out to false instead of assuming success, since
// that's a corpus or harness defect that must not pass unnoticed.
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

// The explicit return after REQUIRE_TRUE(meta_ok) is needed: tau's REQUIRE
// returns only while a suite runs, and without it the loop would go on with
// in_data freed and the directory closed.
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
    if (!in_data) continue;
    bool meta_ok = false;
    bool expects_fail = parse_meta_expects_fail(meta_path, &meta_ok);
    if (!meta_ok) {
      fprintf(stderr,
              "[spec_suite] case %s has an in.yaml but no readable "
              "meta.txt; treating this as a corpus defect\n",
              entry->d_name);
      free(in_data);
      closedir(d);
      REQUIRE_TRUE(meta_ok);
      return;
    }

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

  REQUIRE_EQ(total, (size_t)EXPECTED_CASE_COUNT);
  REQUIRE_EQ(unexpected_failures, (size_t)0);
  REQUIRE_EQ(stale_deviations, (size_t)0);
}
