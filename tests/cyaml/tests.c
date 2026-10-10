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

#include <cyaml.h>
#include <internal/cprocsintern.h>
#include <internal/cstrutil.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h> /* nl_langinfo_l */
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <time.h>

TAU_MAIN()

/* Counting allocator: it lets exactly g_alloc_remaining calls to malloc,
 * calloc and realloc succeed and then returns NULL. A value of -1 means that
 * there is no limit, which is the normal behaviour. */
static int g_alloc_remaining = -1;

static void *counting_malloc(size_t sz) {
  if (g_alloc_remaining == 0) return NULL;
  if (g_alloc_remaining > 0) g_alloc_remaining--;
  return malloc(sz);
}
static void *counting_calloc(size_t n, size_t sz) {
  if (g_alloc_remaining == 0) return NULL;
  if (g_alloc_remaining > 0) g_alloc_remaining--;
  return calloc(n, sz);
}
static void *counting_realloc(void *p, size_t sz) {
  if (g_alloc_remaining == 0) return NULL;
  if (g_alloc_remaining > 0) g_alloc_remaining--;
  return realloc(p, sz);
}
static ccol_memmgmt_procs_t g_counting_mp = {.malloc = counting_malloc,
                                             .calloc = counting_calloc,
                                             .realloc = counting_realloc,
                                             .free = free};

/* A plain tally, unlike g_counting_mp above: that one exists to REFUSE an
   allocation once its budget runs out, while this one never refuses and only
   records how many allocations happened. A test that pins how much work a
   parse does needs that count, which it cannot get from a budget. */
static long g_tally = 0;
static void *tally_malloc(size_t sz) {
  g_tally++;
  return malloc(sz);
}
static void *tally_calloc(size_t n, size_t sz) {
  g_tally++;
  return calloc(n, sz);
}
static void *tally_realloc(void *p, size_t sz) {
  g_tally++;
  return realloc(p, sz);
}
static ccol_memmgmt_procs_t g_tally_mp = {.malloc = tally_malloc,
                                          .calloc = tally_calloc,
                                          .realloc = tally_realloc,
                                          .free = free};

/* Allocator that injects one fault. g_counting_mp above fails every call
 * after its budget hits zero, so a message that the parser allocates AFTER
 * the failure that made the whole parse fail can never succeed there. This
 * allocator instead fails exactly the g_single_fail_at allocation call and
 * lets every other call, before and after it, succeed normally. That is what
 * exercises the failure of one specific allocation while it leaves enough
 * "budget" for the error message that the parser builds afterward.
 * tests/chttp/tests.c and tests/chttpclient/tests.c use the identical
 * pattern. -1 means that this allocator never fails. */
static int g_single_fail_at = -1;
static int g_single_call_idx;

static void *single_fault_malloc(size_t sz) {
  int idx = g_single_call_idx++;
  if (g_single_fail_at >= 0 && idx == g_single_fail_at) return NULL;
  return malloc(sz);
}
static void *single_fault_calloc(size_t n, size_t sz) {
  int idx = g_single_call_idx++;
  if (g_single_fail_at >= 0 && idx == g_single_fail_at) return NULL;
  return calloc(n, sz);
}
static void *single_fault_realloc(void *p, size_t sz) {
  int idx = g_single_call_idx++;
  if (g_single_fail_at >= 0 && idx == g_single_fail_at) return NULL;
  return realloc(p, sz);
}
static ccol_memmgmt_procs_t g_single_fault_mp = {
    .malloc = single_fault_malloc,
    .calloc = single_fault_calloc,
    .realloc = single_fault_realloc,
    .free = free};

/* ========================================================================== */
/*                         CONSTRUCTION                                       */
/* ========================================================================== */

/* The parser hands a string key to the dictionary by moving the buffer of
   the scalar node instead of copying it, so a parse makes one allocation
   fewer for each entry than a copy would. The bound is a count and not a
   time, so it is exact and it repeats: on this document a move makes 457
   allocations and a copy makes 521, exactly one more for each entry. This
   test is non-vacuous: copying the key instead takes the count past 8 for
   each entry, and the test fails. */
TEST(cyaml_key_storage, a_string_key_is_moved_into_the_dictionary_not_copied) {
  enum { N = 64 };
  char doc[8192];
  size_t off = 0;
  for (int i = 0; i < N; i++)
    off +=
        (size_t)snprintf(doc + off, sizeof(doc) - off, "key_%03d: %d\n", i, i);

  char *err = NULL;
  g_tally = 0;
  cyaml parsed = cyaml_parse_mp(doc, &err, &g_tally_mp);
  long allocations = g_tally;
  bool parsed_ok = (parsed != NULL && err == NULL);
  size_t entries = parsed ? cyaml_dictionary_size(parsed) : 0;
  /* The tree is destroyed before any assertion, so a failure here leaks
     nothing. err is not NULL only on a parse failure, which an assertion
     below checks for. */
  if (parsed) cyaml_destroy(parsed);

  REQUIRE_TRUE(parsed_ok);
  REQUIRE_EQ(entries, (size_t)N);
  /* The measured counts are 457 with the move and 521 without it, identical
     on gcc and clang, at every optimization level and at both widths. The
     bound sits between the two counts rather than directly under the higher
     one, so a change that adds or removes a few allocations does not
     silently flip the verdict of this test.
   */
  REQUIRE_LT(allocations, (long)(8 * N - N / 2));
}

/* A byte-order mark shifts every offset in the document by three but shifts
   no column, so the parser rewrites the line starts that it works from.
   Because columns decide block nesting, the symptom is a document that keeps
   its structure with a BOM and loses it without one, or the reverse. The two
   documents below are byte-identical except for the BOM, and the parse has
   to agree on every column of both.

   This test does NOT pin the start-of-life sentinel of the memo: the BOM
   branch moves the parse position past the mark before any parse function
   runs, so nothing ever asks about offset zero on a BOM document, and
   nothing consults a memo that answers there. The sentinel is defensive. */
static cyaml _cyaml_dict_child(cyaml parent, const char *key) {
  if (!parent || cyaml_type(parent) != CYAML_DICTIONARY) return NULL;
  return cyaml_dictionary_get(parent, key);
}

static void _cyaml_probe(cyaml doc, size_t *root_size, long long *leaf,
                         long long *deep) {
  if (!doc || cyaml_type(doc) != CYAML_DICTIONARY) return;
  cyaml r = cyaml_dictionary_get(doc, "root");
  if (!r || cyaml_type(r) != CYAML_DICTIONARY) return;
  *root_size = cyaml_dictionary_size(r);

  cyaml leaf_node = _cyaml_dict_child(_cyaml_dict_child(r, "inner"), "leaf");
  if (leaf_node && cyaml_type(leaf_node) == CYAML_INTEGER)
    *leaf = cyaml_int_val(leaf_node);

  cyaml sib = cyaml_dictionary_get(r, "sibling");
  cyaml third =
      (sib && cyaml_type(sib) == CYAML_LIST && cyaml_list_len(sib) > 2)
          ? cyaml_list_get(sib, 2)
          : NULL;
  cyaml deep_node = _cyaml_dict_child(third, "deep");
  if (deep_node && cyaml_type(deep_node) == CYAML_INTEGER)
    *deep = cyaml_int_val(deep_node);
}

/* The convention that this suite uses to reach an accessor that exists only
   in a RUNNING_UNIT_TESTS build of the module under test. */
extern bool cyaml_test_force_line_cache_disabled;

/* The same BOM document with the line cache turned off, which is the fallback
 * that a growth failure of line_starts latches. That path computes a line
 * start with a backward scan, and a scan that walks past the byte-order mark
 * reports the first line as three bytes early. Every column on that line is
 * then three too large, which is enough to break the block structure that the
 * columns decide, not only to report a wrong position.
 *
 * The test above cannot reach this path: it exercises the cached path, where
 * the seed of the cache accounts for the mark. The real fallback needs an
 * allocation failure after 64 line boundaries, and by that point no query
 * lands on the first line at all, so the hook is what makes this class
 * testable.
 *
 * This test is non-vacuous: without the floor of the scan, the BOM document
 * fails to parse with "trailing content". */
TEST(cyaml_line_cache, a_bom_is_honoured_when_the_line_cache_is_disabled) {
  static const char body[] =
      "root:\n"
      "  inner:\n"
      "    leaf: 7\n";
  char with_bom[sizeof(body) + 3];
  memcpy(with_bom, "\xEF\xBB\xBF", 3);
  memcpy(with_bom + 3, body, sizeof(body));

  cyaml_test_force_line_cache_disabled = true;
  char *err_a = NULL, *err_b = NULL;
  cyaml plain = cyaml_parse(body, &err_a);
  cyaml bom = cyaml_parse(with_bom, &err_b);
  /* The hook is disarmed before any assertion, so an early return cannot
     leave every later test in this binary on the degraded path. */
  cyaml_test_force_line_cache_disabled = false;

  size_t plain_root = 0, bom_root = 0;
  long long plain_leaf = -1, bom_leaf = -1, ignored = -1;
  _cyaml_probe(plain, &plain_root, &plain_leaf, &ignored);
  _cyaml_probe(bom, &bom_root, &bom_leaf, &ignored);
  bool both_parsed =
      (plain != NULL && bom != NULL && err_a == NULL && err_b == NULL);
  if (plain) cyaml_destroy(plain);
  if (bom) cyaml_destroy(bom);

  REQUIRE_TRUE(both_parsed);
  REQUIRE_EQ(plain_root, (size_t)1);
  REQUIRE_EQ(bom_root, (size_t)1);
  REQUIRE_EQ(plain_leaf, 7LL);
  REQUIRE_EQ(bom_leaf, 7LL);
}

TEST(cyaml_line_cache, a_bom_does_not_shift_the_columns_the_memo_reports) {
  static const char body[] =
      "root:\n"
      "  inner:\n"
      "    leaf: 7\n"
      "  sibling: [1, 2, {deep: 9}]\n";
  char with_bom[sizeof(body) + 3];
  memcpy(with_bom, "\xEF\xBB\xBF", 3);
  memcpy(with_bom + 3, body, sizeof(body));

  char *err_a = NULL, *err_b = NULL;
  cyaml plain = cyaml_parse(body, &err_a);
  cyaml bom = cyaml_parse(with_bom, &err_b);

  long long plain_leaf = -1, bom_leaf = -1, plain_deep = -1, bom_deep = -1;
  size_t plain_root = 0, bom_root = 0;
  /* Every accessor below is gated on the type of the node, not only on the
     node being non-NULL, because a nesting regression is exactly what turns
     one of these nodes into a scalar. cyaml_dictionary_size and
     cyaml_int_val call ccol_fatal_err on a node of the wrong type, which
     aborts the whole binary and destroys the result of every other test
     instead of failing this one cleanly. */
  _cyaml_probe(plain, &plain_root, &plain_leaf, &plain_deep);
  _cyaml_probe(bom, &bom_root, &bom_leaf, &bom_deep);
  bool both_parsed =
      (plain != NULL && bom != NULL && err_a == NULL && err_b == NULL);
  if (plain) cyaml_destroy(plain);
  if (bom) cyaml_destroy(bom);

  REQUIRE_TRUE(both_parsed);
  REQUIRE_EQ(plain_root, (size_t)2);
  REQUIRE_EQ(bom_root, (size_t)2);
  REQUIRE_EQ(plain_leaf, 7LL);
  REQUIRE_EQ(bom_leaf, 7LL);
  REQUIRE_EQ(plain_deep, 9LL);
  REQUIRE_EQ(bom_deep, 9LL);
}

/* Rewrite every '\n' in `body` as `eol`. Put a byte-order mark in front of
   the result when the caller asks for one. The caller owns the result. */
static char *_cyaml_reline(const char *body, const char *eol, bool bom) {
  size_t eol_len = strlen(eol);
  size_t out_cap = strlen(body) * (eol_len + 1) + 4;
  char *out = (char *)malloc(out_cap);
  if (!out) return NULL;
  size_t w = 0;
  if (bom) {
    memcpy(out, "\xEF\xBB\xBF", 3);
    w = 3;
  }
  for (const char *r = body; *r; r++) {
    if (*r == '\n') {
      memcpy(out + w, eol, eol_len);
      w += eol_len;
    } else {
      out[w++] = *r;
    }
  }
  out[w] = '\0';
  return out;
}

/* The parser decides whether a token covers more than one physical line from
   the start of the line that it has reached, without scanning the bytes of
   the token, so the answer is only as good as the line starts that the parser
   tracks. The matrix below makes that answer visible from outside: an
   implicit key must fit on one line, so each `multi` document must be
   rejected and each `single` one accepted. This holds for every line ending
   that this parser knows, with or without a byte-order mark that shifts
   every offset by three.

   The bare-CR document is the case worth having, because a line-break test
   that looks only for '\n' accepts every `multi` document in that form. The
   BOM columns matter for the same reason as two tests above: the line start
   that a span is compared against has the start of the first line as its
   floor.

   This test is non-vacuous in both directions. Reporting "crossed" for a
   span that did not cross makes every `single` document fail, and reporting
   "did not cross" for one that did makes every `multi` document parse. */
static void _cyaml_check_implicit_key_span_matrix(bool *all_multi_rejected,
                                                  bool *all_single_accepted) {
  static const char *const eols[] = {"\n", "\r\n", "\r"};
  static const char *const multi[] = {"\"a\nb\": 1\n", "'a\nb': 1\n",
                                      "[1,\n2]: v\n", "a\nb: 1\n"};
  static const char *const single[] = {"\"ab\": 1\n", "'ab': 1\n",
                                       "[1, 2]: v\n", "ab: 1\n"};
  *all_multi_rejected = true;
  *all_single_accepted = true;
  for (size_t e = 0; e < sizeof(eols) / sizeof(eols[0]); e++) {
    for (int bom = 0; bom < 2; bom++) {
      for (size_t i = 0; i < sizeof(multi) / sizeof(multi[0]); i++) {
        char *doc = _cyaml_reline(multi[i], eols[e], bom != 0);
        if (!doc) {
          *all_multi_rejected = false;
          continue;
        }
        char *err = NULL;
        cyaml parsed = cyaml_parse(doc, &err);
        if (parsed) {
          *all_multi_rejected = false;
          cyaml_destroy(parsed);
        }
        free(doc);
      }
      for (size_t i = 0; i < sizeof(single) / sizeof(single[0]); i++) {
        char *doc = _cyaml_reline(single[i], eols[e], bom != 0);
        if (!doc) {
          *all_single_accepted = false;
          continue;
        }
        char *err = NULL;
        cyaml parsed = cyaml_parse(doc, &err);
        if (!parsed) *all_single_accepted = false;
        if (parsed) cyaml_destroy(parsed);
        free(doc);
      }
    }
  }
}

TEST(cyaml_line_cache, an_implicit_key_spanning_lines_is_rejected_per_ending) {
  bool multi_rejected = false, single_accepted = false;
  _cyaml_check_implicit_key_span_matrix(&multi_rejected, &single_accepted);
  REQUIRE_TRUE(multi_rejected);
  REQUIRE_TRUE(single_accepted);
}

/* The same matrix with the line cache turned off, which is the degraded state
   that a growth failure of line_starts latches. A span decision then comes
   from a backward scan instead of the cache, and the two must agree. The BOM
   documents are what separate them, because only the scan needs the start of
   the first line as an explicit floor. */
TEST(cyaml_line_cache, an_implicit_key_span_agrees_when_the_cache_is_off) {
  bool multi_rejected = false, single_accepted = false;
  cyaml_test_force_line_cache_disabled = true;
  _cyaml_check_implicit_key_span_matrix(&multi_rejected, &single_accepted);
  /* The hook is disarmed before any assertion, so an early return cannot
     leave every later test in this binary on the degraded path. */
  cyaml_test_force_line_cache_disabled = false;
  REQUIRE_TRUE(multi_rejected);
  REQUIRE_TRUE(single_accepted);
}

/* A speculative key parse rewinds the position that it advanced to, and the
   parser then asks about offsets that the line cache has already scanned
   past. That is the one shape where the cache cannot answer from its last
   entry and must search, and a span decision after such a rewind must come
   out the same as one taken on the way forward. Each document below drives a
   rewind and then makes span decisions on later lines; a rewind happens when
   a token looks like a key until the ':' does not appear where a key needs
   it. */
TEST(cyaml_line_cache, span_decisions_survive_a_speculative_rewind) {
  static const char *const bodies[] = {
      "- \"looks like a key\"\n- second\n- \"third\": 1\n",
      "a: &anc [1, 2]\nb: *anc\nc: {d: e}\n",
      "? explicit\n: value\nplain key: 1\n\"quoted key\": 2\n",
      "top:\n  - {k: v}\n  - \"x\": 1\n  - plain\nnext: 3\n",
  };
  static const char *const eols[] = {"\n", "\r\n", "\r"};
  bool all_parsed = true;
  for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
    for (size_t e = 0; e < sizeof(eols) / sizeof(eols[0]); e++) {
      for (int bom = 0; bom < 2; bom++) {
        char *doc = _cyaml_reline(bodies[i], eols[e], bom != 0);
        if (!doc) {
          all_parsed = false;
          continue;
        }
        char *err = NULL;
        cyaml parsed = cyaml_parse(doc, &err);
        if (!parsed) all_parsed = false;
        if (parsed) cyaml_destroy(parsed);
        free(doc);
      }
    }
  }
  REQUIRE_TRUE(all_parsed);
}

TEST(construction, null) {
  cyaml n = cyaml_create_null();
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(construction, bool_true) {
  cyaml n = cyaml_create_bool(true);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_BOOL);
  REQUIRE_TRUE(cyaml_bool_val(n));
  cyaml_destroy(n);
}

TEST(construction, bool_false) {
  cyaml n = cyaml_create_bool(false);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_BOOL);
  REQUIRE_FALSE(cyaml_bool_val(n));
  cyaml_destroy(n);
}

TEST(construction, integer) {
  cyaml n = cyaml_create_int(-9876543210LL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), -9876543210LL);
  cyaml_destroy(n);
}

TEST(construction, float_finite) {
  cyaml n = cyaml_create_double(2.718281828);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(n), 2.718281828);
  cyaml_destroy(n);
}

TEST(construction, float_inf) {
  cyaml n = cyaml_create_double(__builtin_inf());
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_TRUE(__builtin_isinf(cyaml_double_val(n)));
  cyaml_destroy(n);
}

TEST(construction, float_nan) {
  cyaml n = cyaml_create_double(__builtin_nan(""));
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_TRUE(__builtin_isnan(cyaml_double_val(n)));
  cyaml_destroy(n);
}

TEST(construction, string) {
  cyaml n = cyaml_create_string("hello yaml");
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello yaml");
  cyaml_destroy(n);
}

TEST(construction, string_null_becomes_null_node) {
  cyaml n = cyaml_create_string(NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
}

TEST(construction, empty_sequence) {
  cyaml s = cyaml_create_list();
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cyaml_type(s), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(s), (size_t)0);
  cyaml_destroy(s);
}

TEST(construction, sequence_push_get) {
  cyaml s = cyaml_create_list();
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cyaml_list_push(s, cyaml_create_int(1)), ccol_success);
  REQUIRE_EQ(cyaml_list_push(s, cyaml_create_int(2)), ccol_success);
  REQUIRE_EQ(cyaml_list_push(s, cyaml_create_int(3)), ccol_success);
  REQUIRE_EQ(cyaml_list_len(s), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(s, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(s, 1)), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(s, 2)), 3LL);
  REQUIRE_EQ((void *)cyaml_list_get(s, 3), NULL);
  cyaml_destroy(s);
}

TEST(construction, empty_mapping) {
  cyaml m = cyaml_create_dictionary();
  REQUIRE_NE((void *)m, NULL);
  REQUIRE_EQ(cyaml_type(m), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(m), (size_t)0);
  cyaml_destroy(m);
}

TEST(construction, mapping_set_get) {
  cyaml m = cyaml_create_dictionary();
  REQUIRE_NE((void *)m, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(m, "host", cyaml_create_string("localhost")),
             ccol_success);
  REQUIRE_EQ(cyaml_dictionary_set(m, "port", cyaml_create_int(8080)),
             ccol_success);
  REQUIRE_EQ(cyaml_dictionary_size(m), (size_t)2);
  cyaml host = cyaml_dictionary_get(m, "host");
  REQUIRE_NE((void *)host, NULL);
  REQUIRE_STREQ(cyaml_str_val(host), "localhost");
  cyaml port = cyaml_dictionary_get(m, "port");
  REQUIRE_NE((void *)port, NULL);
  REQUIRE_EQ(cyaml_int_val(port), 8080LL);
  REQUIRE_EQ((void *)cyaml_dictionary_get(m, "missing"), NULL);
  cyaml_destroy(m);
}

TEST(construction, mapping_replace) {
  cyaml m = cyaml_create_dictionary();
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", cyaml_create_int(1)), ccol_success);
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", cyaml_create_int(2)), ccol_success);
  REQUIRE_EQ(cyaml_dictionary_size(m), (size_t)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(m, "k")), 2LL);
  cyaml_destroy(m);
}

TEST(construction, mapping_set_self_assignment_does_not_corrupt_value) {
  /* cyaml_dictionary_get gives a borrowed reference, and a caller can hand
   * that exact pointer back to cyaml_dictionary_set for the SAME key, which
   * makes old_child == child. That call must not destroy the node that the
   * dictionary slot points to after it stores the same pointer again.
   * Without that self-assignment check, the live node that the dictionary
   * reaches is corrupted, or, for a custom allocator with no thread-local
   * pool, freed. */
  cyaml m = cyaml_create_dictionary();
  REQUIRE_NE((void *)m, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", cyaml_create_int(1)), ccol_success);

  cyaml v = cyaml_dictionary_get(m, "k");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", v), ccol_success);

  cyaml v2 = cyaml_dictionary_get(m, "k");
  REQUIRE_NE((void *)v2, NULL);
  REQUIRE_EQ((void *)v2, (void *)v);
  REQUIRE_EQ(cyaml_type(v2), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(v2), 1LL);
  REQUIRE_EQ(cyaml_dictionary_size(m), (size_t)1);

  cyaml_destroy(m);
}

TEST(construction, mapping_set_replaces_container) {
  /* Replacing a container value (CYAML_LIST) with a scalar must free the old
   * subtree recursively through node_clear and then store the new value with
   * no leak and no dangling pointer. */
  cyaml m = cyaml_create_dictionary();
  REQUIRE_NE((void *)m, NULL);

  cyaml lst = cyaml_create_list();
  REQUIRE_NE((void *)lst, NULL);
  REQUIRE_EQ(cyaml_list_push(lst, cyaml_create_int(1)), ccol_success);
  REQUIRE_EQ(cyaml_list_push(lst, cyaml_create_int(2)), ccol_success);
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", lst), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(m, "k")), CYAML_LIST);

  /* Replace the list with a scalar; the two-element list must be freed. */
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", cyaml_create_string("replaced")),
             ccol_success);
  REQUIRE_EQ(cyaml_dictionary_size(m), (size_t)1);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(m, "k")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(m, "k")), "replaced");

  /* Replace the scalar with a nested dictionary. */
  cyaml sub = cyaml_create_dictionary();
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(sub, "x", cyaml_create_int(99)),
             ccol_success);
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", sub), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(m, "k")), CYAML_DICTIONARY);
  REQUIRE_EQ(
      cyaml_int_val(cyaml_dictionary_get(cyaml_dictionary_get(m, "k"), "x")),
      99LL);

  cyaml_destroy(m);
}

/* ========================================================================== */
/*                         IMPLICIT TYPE RESOLUTION                           */
/* ========================================================================== */

TEST(implicit_types, null_tilde) {
  char *err = NULL;
  cyaml n = cyaml_parse("~\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
}

TEST(implicit_types, null_word) {
  char *err = NULL;
  cyaml n = cyaml_parse("null\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
}

TEST(implicit_types, null_NULL) {
  char *err = NULL;
  cyaml n = cyaml_parse("NULL\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
}

TEST(implicit_types, null_empty_input) {
  /* An empty document of zero bytes is a null value, because an empty plain
   * scalar resolves to null under the YAML 1.2 core schema. */
  char *err = NULL;
  cyaml n = cyaml_parse("", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
}

TEST(implicit_types, bool_true) {
  char *err = NULL;
  cyaml n = cyaml_parse("true\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_BOOL);
  REQUIRE_TRUE(cyaml_bool_val(n));
  cyaml_destroy(n);
}

TEST(implicit_types, bool_false) {
  char *err = NULL;
  cyaml n = cyaml_parse("false\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_BOOL);
  REQUIRE_FALSE(cyaml_bool_val(n));
  cyaml_destroy(n);
}

TEST(implicit_types, bool_True) {
  char *err = NULL;
  cyaml n = cyaml_parse("True\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_BOOL);
  REQUIRE_TRUE(cyaml_bool_val(n));
  cyaml_destroy(n);
}

TEST(implicit_types, integer_decimal) {
  char *err = NULL;
  cyaml n = cyaml_parse("42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 42LL);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_decimal_leading_zero) {
  /* The decimal int grammar of the YAML 1.2 core schema is [-+]?[0-9]+, with
   * no restriction on a leading zero (the stricter JSON schema has one, with
   * its own (0|[1-9][0-9]*)). "007" is therefore a plain, clear integer 7 and
   * not a string. This test pins the behaviour of try_parse_int_scalar for
   * this form. */
  char *err = NULL;
  cyaml n = cyaml_parse("007\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 7LL);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_negative) {
  char *err = NULL;
  cyaml n = cyaml_parse("-100\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), -100LL);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_llong_min) {
  /* LLONG_MIN = -9223372036854775808. strtoll on the substring
   * "9223372036854775808" (= 2^63), with the sign removed, overflows long
   * long, so the parser must give the full signed string to strtoll instead
   * of p after the '-'. Without that, the value silently becomes a
   * CYAML_FLOAT and loses precision. */
  char *err = NULL;
  cyaml n = cyaml_parse("-9223372036854775808\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), LLONG_MIN);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_llong_min_round_trip) {
  /* Serialize LLONG_MIN and parse it again; the type and the value must
   * survive intact. */
  cyaml n = cyaml_create_int(LLONG_MIN);
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(back), LLONG_MIN);
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(implicit_types, integer_hex) {
  char *err = NULL;
  cyaml n = cyaml_parse("0xFF\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 255LL);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_octal) {
  char *err = NULL;
  cyaml n = cyaml_parse("0o17\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 15LL);
  cyaml_destroy(n);
}

TEST(implicit_types, hex_with_embedded_sign_is_not_a_valid_integer) {
  /* The grammar of the core schema for this form is exactly
   * "0x" [0-9a-fA-F]+, with no room for a sign between the prefix and the
   * digits. strtoull() is more permissive, because it accepts an optional
   * leading '+' or '-' before the digits that it reads, so without an
   * explicit rejection a malformed literal such as "0x-0" is silently
   * accepted as a valid CYAML_INTEGER with magnitude 0. It must instead fall
   * back to CYAML_STRING, like any other plain scalar that is not a number. */
  char *err = NULL;
  cyaml n = cyaml_parse("0x-0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0x-0");
  cyaml_destroy(n);
}

TEST(implicit_types, hex_with_embedded_plus_sign_is_not_a_valid_integer) {
  char *err = NULL;
  cyaml n = cyaml_parse("0x+5\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0x+5");
  cyaml_destroy(n);
}

TEST(implicit_types, octal_with_embedded_sign_is_not_a_valid_integer) {
  char *err = NULL;
  cyaml n = cyaml_parse("0o-0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0o-0");
  cyaml_destroy(n);
}

TEST(implicit_types, octal_with_embedded_plus_sign_is_not_a_valid_integer) {
  char *err = NULL;
  cyaml n = cyaml_parse("0o+7\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0o+7");
  cyaml_destroy(n);
}

TEST(implicit_types, hex_with_leading_sign_before_prefix_is_a_valid_integer) {
  /* A sign at the very start of the whole literal, before "0x", is
   * legitimate, unlike the sign inside the literal that the test above
   * rejects. The literal "-0x1" is a valid negative hex integer and must
   * parse as one. */
  char *err = NULL;
  cyaml n = cyaml_parse("-0x1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), -1LL);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_hex_overflow_falls_back_to_float) {
  /* 0xFFFFFFFFFFFFFFFF (2^64-1) does not fit in a signed 64-bit long long, so
   * it must fall back to CYAML_FLOAT, just like a decimal literal that is too
   * wide. The parser must not silently read the bit pattern as a negative
   * long long. */
  char *err = NULL;
  cyaml n = cyaml_parse("0xFFFFFFFFFFFFFFFF\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_hex_exactly_two_pow_63_falls_back_to_float) {
  /* 0x8000000000000000 (2^63) is a POSITIVE literal that does not fit in a
   * signed 64-bit long long; it must not silently become negative. */
  char *err = NULL;
  cyaml n = cyaml_parse("0x8000000000000000\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_GT(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_negative_hex_two_pow_63_is_llong_min) {
  /* -0x8000000000000000 (-2^63) is exactly LLONG_MIN, and it DOES fit. The
   * parser must accept it as a CYAML_INTEGER without reaching signed-overflow
   * undefined behavior when it computes the negation. */
  char *err = NULL;
  cyaml n = cyaml_parse("-0x8000000000000000\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), LLONG_MIN);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_octal_overflow_falls_back_to_float) {
  /* 0o1777777777777777777777 = 2^64-1 in octal: the same overflow class as
   * the hex case above, so it must fall back to CYAML_FLOAT in the same way.
   * The parser must neither silently read the bit pattern as a negative long
   * long nor drop to CYAML_STRING because it finds no other
   * representation. */
  char *err = NULL;
  cyaml n = cyaml_parse("0o1777777777777777777777\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_GT(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_negative_octal_overflow_falls_back_to_float) {
  char *err = NULL;
  cyaml n = cyaml_parse("-0o1777777777777777777777\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_LT(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, uppercase_hex_prefix_is_a_string) {
  /* The grammar of the core schema for the int and float hex fallback forms
   * is exactly the lowercase "0x" [0-9a-fA-F]+. An uppercase "0X" prefix has
   * no numeric representation in the core schema at all, so it must resolve
   * to an ordinary CYAML_STRING, like any other plain scalar that is not a
   * number. A reference parser agrees. */
  char *err = NULL;
  cyaml n = cyaml_parse("0X10\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0X10");
  cyaml_destroy(n);
}

TEST(implicit_types, uppercase_octal_prefix_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("0O17\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0O17");
  cyaml_destroy(n);
}

TEST(implicit_types, uppercase_hex_prefix_with_leading_sign_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("-0X10\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "-0X10");
  cyaml_destroy(n);
}

TEST(implicit_types, uppercase_hex_prefix_overflow_stays_a_string_not_float) {
  /* A lowercase "0x..." literal that is in range but overflows int64 falls
   * back to CYAML_FLOAT (see integer_hex_overflow_falls_back_to_float
   * above). The identical magnitude with an uppercase "0X" prefix has no
   * valid numeric meaning of ANY kind, not even the float fallback, so it
   * must stay a CYAML_STRING. This test also guards against the native
   * hex-float extension of strtod(), which recognizes "0X" without regard to
   * letter case, as the C standard says: without the rejection of the
   * uppercase form in try_parse_float_scalar, strtod() silently reads this
   * as a hex float. */
  char *err = NULL;
  cyaml n = cyaml_parse("0XFFFFFFFFFFFFFFFF\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0XFFFFFFFFFFFFFFFF");
  cyaml_destroy(n);
}

TEST(implicit_types, float_decimal) {
  char *err = NULL;
  cyaml n = cyaml_parse("3.14\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(n), 3.14);
  cyaml_destroy(n);
}

TEST(implicit_types, float_exponent) {
  char *err = NULL;
  cyaml n = cyaml_parse("1.5e3\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(n), 1500.0);
  cyaml_destroy(n);
}

TEST(implicit_types, float_subnormal_underflow_is_still_a_float) {
  /* 5e-324 is a real, valid IEEE-754 denormal double, yet strtod() sets
   * errno=ERANGE on this legitimate underflow exactly as it does on a true
   * overflow. So do not reject the value with a bare errno==ERANGE check:
   * only a result that clamped to +infinity or -infinity is a real
   * failure. */
  char *err = NULL;
  cyaml n = cyaml_parse("5e-324\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_GT(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, float_underflow_to_zero_is_still_a_float) {
  /* 1e-400 legitimately underflows all the way to 0.0, but it is a
   * CYAML_FLOAT with a correct value, not a CYAML_STRING. */
  char *err = NULL;
  cyaml n = cyaml_parse("1e-400\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, hex_float_syntax_is_not_a_yaml_float) {
  /* strtod() accepts the C99 hex-float syntax (0x1p3) as a GNU and C99
   * extension, but that syntax has no place in the core schema float grammar
   * of YAML 1.2, which is decimal only. It must fall through to CYAML_STRING
   * like any other scalar that does not look like a number, and the parser
   * must not silently read it as the float value 8.0. */
  char *err = NULL;
  cyaml n = cyaml_parse("0x1p3\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0x1p3");
  cyaml_destroy(n);
}

TEST(implicit_types,
     hex_float_syntax_with_fraction_and_exponent_is_not_a_yaml_float) {
  char *err = NULL;
  cyaml n = cyaml_parse("0x1.8p10\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0x1.8p10");
  cyaml_destroy(n);
}

TEST(implicit_types,
     hex_integer_overflow_without_exponent_still_falls_back_to_float) {
  /* A run of hex digits with no p or P exponent is never real hex-float
   * syntax, so the hex_float_syntax_is_not_a_yaml_float guard above must not
   * disturb the integer_hex_overflow_falls_back_to_float behaviour that this
   * case needs. This test uses 0x8000000000000001 (2^63+1) instead of the
   * "0xFFFFFFFFFFFFFFFF" (2^64-1) of that other test, so it exercises a
   * different overflow magnitude, one that is safely above 2^63 and at most
   * 2^64-1, which makes the value really take the float-fallback path. One
   * more hex digit would reach the wider case, where the value does not even
   * fit in a uint64 and falls back to CYAML_STRING instead. */
  char *err = NULL;
  cyaml n = cyaml_parse("0x8000000000000001\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  cyaml_destroy(n);
}

TEST(implicit_types, float_inf) {
  char *err = NULL;
  cyaml n = cyaml_parse(".inf\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_TRUE(__builtin_isinf(cyaml_double_val(n)));
  REQUIRE_GT(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, float_neg_inf) {
  char *err = NULL;
  cyaml n = cyaml_parse("-.inf\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_TRUE(__builtin_isinf(cyaml_double_val(n)));
  REQUIRE_LT(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, float_nan) {
  char *err = NULL;
  cyaml n = cyaml_parse(".nan\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_TRUE(__builtin_isnan(cyaml_double_val(n)));
  cyaml_destroy(n);
}

TEST(implicit_types, string_plain) {
  char *err = NULL;
  cyaml n = cyaml_parse("hello\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello");
  cyaml_destroy(n);
}

TEST(implicit_types, string_not_bool) {
  /* "trueish" must be a string, not a bool. */
  char *err = NULL;
  cyaml n = cyaml_parse("trueish\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "trueish");
  cyaml_destroy(n);
}

/* ========================================================================== */
/*                 C99 NAN/INF BARE FORMS ARE STRINGS (YAML 1.2)              */
/* ========================================================================== */

TEST(implicit_types, string_nan_lowercase) {
  /* In the YAML 1.2 core schema, only ".nan", ".NaN" and ".NAN" are a float
   * NaN, and a bare "nan" is a plain string. strtod() on a C99 platform
   * accepts it as NaN, so make_typed_scalar must filter these out first. */
  char *err = NULL;
  cyaml n = cyaml_parse("nan\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "nan");
  cyaml_destroy(n);
}

TEST(implicit_types, string_nan_mixedcase) {
  char *err = NULL;
  cyaml n = cyaml_parse("NaN\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "NaN");
  cyaml_destroy(n);
}

TEST(implicit_types, string_nan_uppercase) {
  char *err = NULL;
  cyaml n = cyaml_parse("NAN\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "NAN");
  cyaml_destroy(n);
}

TEST(implicit_types, string_nan_with_parenthesized_payload) {
  /* The strtod() of glibc also accepts the C99 "nan(n-char-sequence)" syntax,
   * for example "nan(123)", which is not part of the core schema float grammar
   * of YAML 1.2 (only the dot-prefixed forms are; the bare forms above are
   * strings too). Without an explicit filter in front, the generic strtod()
   * fallback silently accepts this as a NaN float and discards the original
   * text. */
  char *err = NULL;
  cyaml n = cyaml_parse("nan(123)\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "nan(123)");
  cyaml_destroy(n);
}

TEST(implicit_types, string_nan_with_empty_parens_and_sign) {
  /* The same class, for the forms with a sign in front and with an empty
   * payload, which strtod() also accepts on its own. */
  char *err = NULL;
  cyaml n1 = cyaml_parse("nan()\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n1, NULL);
  REQUIRE_EQ(cyaml_type(n1), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n1), "nan()");
  cyaml_destroy(n1);

  cyaml n2 = cyaml_parse("-nan(x)\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n2, NULL);
  REQUIRE_EQ(cyaml_type(n2), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n2), "-nan(x)");
  cyaml_destroy(n2);
}

TEST(implicit_types, tagged_float_nan_with_parenthesized_payload_rejected) {
  /* An explicit !!float tag reaches try_parse_float_scalar() directly,
   * without going through the int-first cascade of make_typed_scalar(), so
   * this test exercises the same guard from a second call site. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float nan(3)\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(implicit_types, flow_dict_nan_parenthesized_keys_do_not_collide) {
  /* Regression guard for the consequence on key collision: if "nan(1)" and
   * "nan(2)" both silently canonicalized to the "nan" dictionary key text of
   * the float NaN, the second entry would destroy the first. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{nan(1): x, nan(2): y}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "nan(1)")), "x");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "nan(2)")), "y");
  cyaml_destroy(doc);
}

TEST(implicit_types, string_inf_lowercase) {
  /* A bare "inf" is a string under YAML 1.2; only ".inf", ".Inf" and ".INF"
   * are a float infinity. */
  char *err = NULL;
  cyaml n = cyaml_parse("inf\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "inf");
  cyaml_destroy(n);
}

TEST(implicit_types, string_inf_mixedcase) {
  char *err = NULL;
  cyaml n = cyaml_parse("Inf\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "Inf");
  cyaml_destroy(n);
}

TEST(implicit_types, string_inf_uppercase) {
  char *err = NULL;
  cyaml n = cyaml_parse("INF\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "INF");
  cyaml_destroy(n);
}

TEST(implicit_types, string_infinity) {
  char *err = NULL;
  cyaml n = cyaml_parse("infinity\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "infinity");
  cyaml_destroy(n);
}

TEST(implicit_types, string_plus_inf) {
  char *err = NULL;
  cyaml n = cyaml_parse("+inf\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "+inf");
  cyaml_destroy(n);
}

TEST(implicit_types, string_minus_inf) {
  char *err = NULL;
  cyaml n = cyaml_parse("-inf\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "-inf");
  cyaml_destroy(n);
}

TEST(implicit_types, c99_nan_inf_round_trips) {
  /* Verify the full round trip for the bare C99 forms: create a string node,
   * serialize it and parse it again. The value and the type must survive. */
  const char *cases[] = {"nan", "NaN",      "NAN",  "inf",  "Inf",
                         "INF", "infinity", "+inf", "-inf", NULL};
  for (int i = 0; cases[i]; i++) {
    cyaml n = cyaml_create_string(cases[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char *err = NULL;
    cyaml back = cyaml_parse(s, &err);
    REQUIRE_EQ((void *)err, NULL);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(back), cases[i]);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
    cyaml_destroy(back);
  }
}

/* ========================================================================== */
/*                         QUOTED SCALARS                                     */
/* ========================================================================== */

TEST(quoted, double_quoted_basic) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"hello world\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello world");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escapes) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\\nline2\\ttab\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1\nline2\ttab");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_unicode_escape) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"caf\\u00e9\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "caf\xc3\xa9");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_full_escape_table) {
  /* double_quoted_escapes above covers only \n and \t, so this test drives
   * every other single-character escape of YAML 1.2 sec. 5.7 that no other
   * test in this suite covers on its own (\x, \u and \U have their own
   * tests). The escapes here are \a \b \v \f \r \e, an escaped space, \" ,
   * \/, and the three Unicode line and space separator escapes \N \_ \L
   * \P. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\a\\b\\v\\f\\r\\e\\ \\\"\\/\\N\\_\\L\\P\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n),
                "\a\b\v\f\r\x1B \"/\xC2\x85\xC2\xA0\xE2\x80\xA8\xE2\x80\xA9");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_preserves_null_like) {
  /* A double-quoted "null" is always a string. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"null\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "null");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_basic) {
  char *err = NULL;
  cyaml n = cyaml_parse("'hello world'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello world");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_escaped_quote) {
  char *err = NULL;
  cyaml n = cyaml_parse("'it''s a test'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "it's a test");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_no_escape_processing) {
  /* \n inside single quotes is NOT an escape. */
  char *err = NULL;
  cyaml n = cyaml_parse("'\\n'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\\n");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_hex_escape) {
  /* \xXX: two-digit hex codepoint encoded as UTF-8. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"H\\x65llo\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "Hello");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_surrogate_pair) {
  /* U+1F600 (grinning face emoji) encoded as a UTF-16 surrogate pair. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\uD83D\\uDE00\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  /* U+1F600 in UTF-8 is F0 9F 98 80. */
  REQUIRE_STREQ(cyaml_str_val(n), "\xF0\x9F\x98\x80");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_big_unicode_escape) {
  /* \U takes 8 hex digits directly for a codepoint above the Basic
   * Multilingual Plane and needs no surrogate pair. U+1F600 in UTF-8 is
   * F0 9F 98 80, which matches the \u surrogate-pair test above. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\U0001F600\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xF0\x9F\x98\x80");
  cyaml_destroy(n);
}

/* A \U escape whose value is not a Unicode scalar value (above U+10FFFF, or
 * a UTF-16 surrogate) names no character. The parser refuses the document and
 * never stores a stand-in character. */
TEST(quoted, double_quoted_big_unicode_escape_out_of_range_is_rejected) {
  static const char *const docs[] = {"\"\\UFFFFFFFF\"\n", "\"\\U00110000\"\n",
                                     "\"\\U0000D800\"\n", "\"\\U0000DFFF\"\n"};
  for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
    char *err = NULL;
    cyaml n = cyaml_parse(docs[i], &err);
    bool refused = n == NULL && err != NULL &&
                   strstr(err, "not a Unicode scalar value") != NULL;
    cyaml_destroy(n);
    REQUIRE_TRUE(refused);
  }
  /* The largest scalar value and the first one past the surrogates are
   * accepted. */
  char *err = NULL;
  cyaml n = cyaml_parse("[\"\\U0010FFFF\", \"\\U0000E000\"]\n", &err);
  bool ok =
      n != NULL && err == NULL &&
      strcmp(cyaml_str_val(cyaml_list_get(n, 0)), "\xF4\x8F\xBF\xBF") == 0 &&
      strcmp(cyaml_str_val(cyaml_list_get(n, 1)), "\xEE\x80\x80") == 0;
  cyaml_destroy(n);
  REQUIRE_TRUE(ok);
}

/* A \u escape of a surrogate is valid only as the high half of a pair that
 * a low-surrogate \u escape completes at once. Every other surrogate escape
 * is a lone surrogate, which names no character, so the document is
 * refused. */
TEST(quoted, double_quoted_lone_surrogate_escapes_are_rejected) {
  static const struct {
    const char *doc;
    const char *msg;
  } cases[] = {
      {"\"\\uD800\"\n", "high surrogate U+D800"},
      {"\"\\uD800\\u0041\"\n", "high surrogate U+D800"},
      {"\"\\uD800\\uD800\"\n", "high surrogate U+D800"},
      {"\"\\uD800\\uD83D\\uDE00\"\n", "high surrogate U+D800"},
      {"\"a\\uDBFFb\"\n", "high surrogate U+DBFF"},
      {"\"\\uDC00\"\n", "low surrogate U+DC00"},
      {"\"\\uDFFF\\uD800\"\n", "low surrogate U+DFFF"},
      {"k: \"x\\uD83D\"\n", "high surrogate U+D83D"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml n = cyaml_parse(cases[i].doc, &err);
    bool refused = n == NULL && err != NULL &&
                   strstr(err, cases[i].msg) != NULL &&
                   strstr(err, "lone surrogate") != NULL;
    cyaml_destroy(n);
    REQUIRE_TRUE(refused);
  }
}

TEST(quoted, double_quoted_surrogate_pair_decodes_to_one_code_point) {
  /* U+1F600 written as the pair \uD83D\uDE00, as JSON writes it. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\uD83D\\uDE00b\"\n", &err);
  bool ok = n != NULL && err == NULL && cyaml_type(n) == CYAML_STRING &&
            strcmp(cyaml_str_val(n),
                   "a\xF0\x9F\x98\x80"
                   "b") == 0;
  cyaml_destroy(n);
  REQUIRE_TRUE(ok);
}

TEST(quoted, block_mapping_double_quoted_first_key) {
  /* A double-quoted scalar as the first (and only) key of a block dictionary.
   */
  const char *yaml =
      "\"host\": localhost\n"
      "\"port\": 8080\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "host")), "localhost");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

TEST(quoted, block_mapping_mixed_keys) {
  /* A plain first key followed by a double-quoted second key. */
  const char *yaml =
      "plain: 1\n"
      "\"quoted key\": 2\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "plain")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "quoted key")), 2LL);
  cyaml_destroy(doc);
}

TEST(quoted, single_quoted_key_in_mapping) {
  /* A single-quoted key that holds spaces, which a plain scalar cannot give. */
  char *err = NULL;
  cyaml doc = cyaml_parse("'key with spaces': value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key with spaces")),
                "value");
  cyaml_destroy(doc);
}

TEST(quoted, double_quoted_multiline_fold_to_space) {
  /* A double-quoted scalar that covers two lines: the one line break folds to
   * a space, as YAML 1.2 section 6.5 says. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\nline2\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 line2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_multiline_blank_line_preserved) {
  /* One blank line between two content lines is two newlines in a row, which
   * the value must keep as one newline, as YAML 1.2 section 6.5 says. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\n\nline2\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1\nline2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_multiline_leading_whitespace_stripped) {
  /* The parser strips the leading whitespace of a continuation line after
   * the fold. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\n   continuation\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 continuation");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_multiline_fold_to_space) {
  /* A single-quoted scalar on more than one line obeys the same line-folding
   * rules as a double-quoted one. */
  char *err = NULL;
  cyaml n = cyaml_parse("'line1\nline2'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 line2");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_multiline_blank_line_preserved) {
  /* A single-quoted scalar must keep one blank line as one newline. */
  char *err = NULL;
  cyaml n = cyaml_parse("'line1\n\nline2'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1\nline2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_trailing_whitespace_stripped_before_fold) {
  /* YAML 1.2 sec. 8.1.2 keeps trailing white space out of the content on the
   * line where the fold happens, so "hello   \nworld" must give
   * "hello world": the parser strips the three spaces before the newline and
   * puts one fold space in their place. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"hello   \nworld\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello world");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_trailing_whitespace_stripped_before_blank_fold) {
  /* When a blank line follows the fold, trailing whitespace on the first line
   * must still be stripped before the blank-line newline is emitted. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"hello   \n\nworld\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello\nworld");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_trailing_whitespace_stripped_before_fold) {
  /* Same rule applies to single-quoted scalars. */
  char *err = NULL;
  cyaml n = cyaml_parse("'hello   \nworld'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello world");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escaped_newline_joins_lines) {
  /* A backslash directly before a literal newline discards that newline and
   * all the leading whitespace on the continuation line (YAML 1.2
   * sec. 8.1.1.2), so the result must hold neither the newline nor any space
   * around it. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\\\n   cont\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1cont");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escaped_newline_then_blank_line_joins_with_one_lf) {
  /* The s-double-escaped production of YAML 1.2 sec. 8.1.2 is
   * ("\" b-non-content l-empty(n,flow-in)* s-flow-line-prefix(n)). Under it,
   * only the escaped break ITSELF is not content, while each l-empty blank
   * line after it ends in a real b-as-line-feed, exactly as an ordinary
   * break with no escape folds its own blank lines. So "a", an escaped
   * newline, one truly blank line and then "b" must join with exactly one
   * literal newline, which the blank line contributes, and never with a
   * space or with nothing at all. Two independent reference parsers agree on
   * "a\nb": PyYAML, and Psych with libyaml from Ruby. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\\n\nb\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "a\nb");
  cyaml_destroy(n);
}

TEST(quoted,
     double_quoted_escaped_newline_then_two_blank_lines_joins_with_two_lfs) {
  /* The same case as above, but with two blank lines in a row between the
   * escaped break and the next real content. Each blank line contributes its
   * own literal newline, so two blank lines join with two newlines; this
   * confirms that the blank-line loop counts each line instead of emitting
   * one fixed newline whatever the count is. PyYAML and Psych both agree on
   * "a\n\nb". */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\\n\n\nb\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "a\n\nb");
  cyaml_destroy(n);
}

/* ========================================================================== */
/*                         BLOCK SCALARS                                      */
/* ========================================================================== */

TEST(block_scalars, literal_basic) {
  const char *yaml =
      "|\n"
      "  line one\n"
      "  line two\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line one\nline two\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_strip) {
  const char *yaml =
      "|-\n"
      "  line one\n"
      "  line two\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line one\nline two");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_keep) {
  const char *yaml =
      "|+\n"
      "  line one\n"
      "  line two\n"
      "\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line one\nline two\n\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_basic) {
  const char *yaml =
      ">\n"
      "  line one\n"
      "  line two\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  /* Single newline between lines is folded to a space. */
  REQUIRE_STREQ(cyaml_str_val(n), "line one line two\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_strip) {
  const char *yaml =
      ">-\n"
      "  line one\n"
      "  line two\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line one line two");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_blank_line_kept) {
  const char *yaml =
      ">\n"
      "  line one\n"
      "\n"
      "  line two\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  /* Blank line becomes a literal newline in folded output. */
  REQUIRE_STREQ(cyaml_str_val(n), "line one\nline two\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_blank_line_bare_cr_does_not_swallow_next_line) {
  /* A blank line inside a literal block scalar can end with a lone '\r' and
   * no '\n' after it, and the parser must read that as its own blank line,
   * one line long. If skip_to_eol stopped only at '\n', it would scan
   * straight through the next content line looking for the next real '\n',
   * and would then silently discard that whole line from the value of the
   * scalar. */
  const char *yaml = "|\n  x\n\r  y\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "x\n\ny\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_bare_cr_line_ending_does_not_swallow_next_line) {
  /* The same over-consumption hazard as in
   * literal_blank_line_bare_cr_does_not_swallow_next_line above, but in the
   * folded scalar path, where both a blank line AND a content line share the
   * same per-line skip_to_eol call. The equivalent call in
   * parse_block_scalar_content is reached only from its blank-line
   * branch. */
  const char *yaml = ">\n  x\r  y\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  /* Two folded lines join with a single space, exactly like an ordinary
   * '\n'-separated pair. */
  REQUIRE_STREQ(cyaml_str_val(n), "x y\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_keep) {
  const char *yaml =
      ">+\n"
      "  line one\n"
      "  line two\n"
      "\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  /* Folded style: one newline between two lines folds to a space, and
   * CHOMP_KEEP keeps the trailing blank line, which gives two trailing
   * newlines in total. */
  REQUIRE_STREQ(cyaml_str_val(n), "line one line two\n\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_keep_content_no_trailing_blank) {
  /* >+ with content but NO trailing blank line: CHOMP_KEEP must write
   * exactly one newline at the end, after the last content line. This is the
   * "trailing_blanks = 0" branch of the CHOMP_KEEP logic. */
  char *err = NULL;
  cyaml n = cyaml_parse(">+\n  hello\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_in_mapping) {
  const char *yaml =
      "description: |\n"
      "  This is a\n"
      "  multiline description.\n"
      "version: 1\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);

  cyaml desc = cyaml_dictionary_get(doc, "description");
  REQUIRE_NE((void *)desc, NULL);
  REQUIRE_EQ(cyaml_type(desc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(desc), "This is a\nmultiline description.\n");

  cyaml ver = cyaml_dictionary_get(doc, "version");
  REQUIRE_NE((void *)ver, NULL);
  REQUIRE_EQ(cyaml_type(ver), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(ver), 1LL);

  cyaml_destroy(doc);
}

TEST(block_scalars, empty_literal) {
  /* A literal block scalar with no content lines produces an empty string. */
  char *err = NULL;
  cyaml n = cyaml_parse("|\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_leading_blank) {
  /* A blank line can come before the first content line of a literal block
   * scalar, and the value must then start with a newline, as the YAML 1.2
   * spec says. */
  const char *yaml =
      "|\n"
      "\n"
      "  line one\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\nline one\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_leading_blank) {
  /* A folded block scalar has the same contract: a blank line at the start
   * gives a literal newline at the start of the value. */
  const char *yaml =
      ">\n"
      "\n"
      "  line one\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\nline one\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_explicit_indent) {
  /* |2 sets block_indent = parent_indent(0) + 2 = 2, so a line with more
   * indentation than the block indent carries its extra spaces into the
   * value. */
  const char *yaml =
      "|2\n"
      "  line one\n"
      "    indented\n"
      "  line two\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line one\n  indented\nline two\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_explicit_indent) {
  /* >2 with two normal content lines: the one newline between them folds to
   * a space, and CHOMP_CLIP adds one newline at the end. */
  const char *yaml =
      ">2\n"
      "  line one\n"
      "  line two\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line one line two\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_keep_no_content_trailing_blank) {
  /* |+ with no content line but one trailing blank line must give "\n",
   * because under YAML 1.2 sec. 8.1.1.2 CHOMP_KEEP keeps a trailing empty
   * line whether or not a content line with text is present. */
  char *err = NULL;
  cyaml n = cyaml_parse("|+\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_keep_no_content_trailing_blank) {
  /* >+ with no content line but one trailing blank line must give "\n", by
   * the same CHOMP_KEEP rule as in the literal case. */
  char *err = NULL;
  cyaml n = cyaml_parse(">+\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_chomp_indent_both_orders) {
  /* |2- and |-2 must give the same result, because YAML 1.2 lets the
   * chomping indicator and the indentation indicator come in either order. */
  char *err = NULL;

  cyaml a = cyaml_parse("|2-\n  hello\n  world\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)a, NULL);

  cyaml b = cyaml_parse("|-2\n  hello\n  world\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)b, NULL);

  REQUIRE_STREQ(cyaml_str_val(a), cyaml_str_val(b));
  REQUIRE_STREQ(cyaml_str_val(a), "hello\nworld");

  cyaml_destroy(a);
  cyaml_destroy(b);
}

TEST(errors, block_scalar_duplicate_indentation_indicator_rejected) {
  /* c-b-block-header lets a header have at most one indentation indicator,
   * so the parser must reject a second digit instead of silently letting it
   * overwrite the first one, which is a "last one wins" rule. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: |24\n    x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_duplicate_chomping_indicator_rejected) {
  /* c-b-block-header lets a header have at most one chomping indicator, so
   * the parser must reject two indicators of the same kind as well as two
   * different kinds together, and must not give either case "last one wins"
   * semantics. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: |--\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);

  err = NULL;
  doc = cyaml_parse("a: |++\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);

  err = NULL;
  doc = cyaml_parse("a: |+-\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);

  err = NULL;
  doc = cyaml_parse("a: |-+\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_scalars, folded_chomp_indent_both_orders) {
  /* >2- and >-2 must give the same result. */
  char *err = NULL;

  cyaml a = cyaml_parse(">2-\n  hello\n  world\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)a, NULL);

  cyaml b = cyaml_parse(">-2\n  hello\n  world\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)b, NULL);

  REQUIRE_STREQ(cyaml_str_val(a), cyaml_str_val(b));
  REQUIRE_STREQ(cyaml_str_val(a), "hello world");

  cyaml_destroy(a);
  cyaml_destroy(b);
}

TEST(block_scalars, folded_more_indented_block) {
  /* Under YAML 1.2 spec s8.1.1.2, a line break next to a more-indented line,
   * on either side, must stay a newline instead of folding to a space. This
   * test drives the change from normal to more-indented and back to
   * normal. */
  const char *yaml =
      ">\n"
      "  normal\n"
      "    more indented\n"
      "  back\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "normal\n  more indented\nback\n");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_more_indented_block_adjacent) {
  /* Many more-indented lines come one after the other, and the parser keeps
   * each line break among them, since each such break touches a neighbour
   * that is more indented. */
  const char *yaml =
      ">\n"
      "  normal\n"
      "    more1\n"
      "    more2\n"
      "  back\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "normal\n  more1\n  more2\nback\n");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_blank_line_adjacent_to_more_indented_line) {
  /* YAML 1.2 section 8.1.3 defines a run of lines that are more indented. A
   * change into such a run, or out of it, costs one literal newline, which
   * the parser adds ON TOP OF every blank line between the two chunks: the
   * two counts add together rather than one replacing the other. A reference
   * parser agrees. */
  const char *yaml =
      ">\n"
      "  a\n"
      "\n"
      "   more\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "a\n\n more\n");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_more_indented_line_adjacent_to_blank_line) {
  /* The same rule, where the two counts add together, but here the content
   * changes out of a more-indented run back to normal content, with one
   * blank line between the two. */
  const char *yaml =
      ">\n"
      "  a\n"
      "   more\n"
      "\n"
      "  b\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "a\n more\n\nb\n");
  cyaml_destroy(doc);
}

TEST(block_scalars,
     folded_leading_blank_lines_before_more_indented_unaffected) {
  /* The have_content guard controls the newline that the parser adds on top,
   * and it must not fire for blank lines that come before the very first
   * content line of the scalar: no content line comes before such a break,
   * so the break touches nothing.
   *
   * This document gives an explicit indentation indicator, so the first
   * content line ("more") is genuinely more indented than the DECLARED
   * indent of 2. Without the indicator the parser detects the indent on its
   * own, takes the column of "more" as the base, and "more" is then not more
   * indented than itself.
   *
   * A reference parser agrees with the result: the parser writes only the
   * newline of the blank line at the start and adds no second newline for a
   * change of run on top of it. */
  const char *yaml =
      ">2\n"
      "\n"
      "   more\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "\n more\n");
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         BLOCK MAPPINGS                                     */
/* ========================================================================== */

TEST(block_mapping, simple) {
  const char *yaml =
      "name: alice\n"
      "age: 30\n"
      "active: true\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)3);

  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "name")), "alice");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "age")), 30LL);
  REQUIRE_TRUE(cyaml_bool_val(cyaml_dictionary_get(doc, "active")));

  cyaml_destroy(doc);
}

TEST(block_mapping, nested) {
  const char *yaml =
      "server:\n"
      "  host: localhost\n"
      "  port: 9090\n"
      "debug: false\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);

  cyaml server = cyaml_dictionary_get(doc, "server");
  REQUIRE_NE((void *)server, NULL);
  REQUIRE_EQ(cyaml_type(server), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(server, "host")),
                "localhost");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(server, "port")), 9090LL);

  REQUIRE_FALSE(cyaml_bool_val(cyaml_dictionary_get(doc, "debug")));

  cyaml_destroy(doc);
}

TEST(block_mapping, null_value) {
  const char *yaml =
      "present: value\n"
      "missing:\n"
      "also_present: 1\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml missing = cyaml_dictionary_get(doc, "missing");
  REQUIRE_NE((void *)missing, NULL);
  REQUIRE_EQ(cyaml_type(missing), CYAML_NULL);

  cyaml_destroy(doc);
}

TEST(block_mapping, implicit_key_canonicalizes_like_every_other_key_notation) {
  /* The parser must resolve the core-schema type of an implicit key before it
   * stores the key, as a flow dictionary key, a "key: value" shorthand in a
   * flow sequence and an explicit "? key" block key already do.
   *
   * Under YAML 1.2, "~", "null" and an explicit "? ~" key are all spellings
   * of one value, so they must collide on the one "null" dictionary key.
   * Without this rule, "? ~\n: 1\n~: 2\n" keeps two separate entries ("~"
   * and "null"), where a consistent implementation gives only one. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? ~\n: 1\n~: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "null")), 2LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, implicit_bool_and_int_keys_canonicalize) {
  /* More spellings of the same rule: an implicit "TRUE" key canonicalizes to
   * "true", as an explicit "? TRUE" key does, and an implicit "0x10" key
   * canonicalizes to "16", as "? 0x10" does. */
  char *err = NULL;
  cyaml doc1 = cyaml_parse("TRUE: a\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc1, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc1, "true")), "a");
  cyaml_destroy(doc1);

  cyaml doc2 = cyaml_parse("0x10: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc2, "16")), "b");
  cyaml_destroy(doc2);
}

TEST(block_mapping, implicit_key_canonicalization_applies_to_every_entry) {
  /* One code path, the top-level plain-scalar dispatch of parse_node, finds
   * the first entry of a mapping, and a different one,
   * parse_one_dict_entry_key, finds every later entry. This test drives the
   * behaviour at both positions in one document, not only at the first. */
  char *err = NULL;
  cyaml doc = cyaml_parse("~: first\nNull: second\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "null")), "second");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_implicit_key_canonicalizes_and_alias_resolves) {
  /* An anchored implicit key must canonicalize its own stored key text
   * exactly like a key with no anchor, while the anchor itself must resolve
   * to the real, typed value of the key for a later alias (an integer here,
   * not the string "16").
   *
   * Two separate code paths do this work: one clones a typed node for the
   * alias, and the other canonicalizes a string for the dictionary storage.
   * The two paths must stay consistent. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&k 0x10: v\nback: *k\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "16")), "v");
  cyaml aliased = cyaml_dictionary_get(doc, "back");
  REQUIRE_NE((void *)aliased, NULL);
  REQUIRE_EQ(cyaml_type(aliased), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(aliased), 16LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, document_start_marker) {
  const char *yaml =
      "---\n"
      "key: value\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(block_mapping, utf8_bom_skipped) {
  /* The parser must consume a UTF-8 BOM (EF BB BF) at the start without any
   * report and then parse the rest of the document in the normal way. */
  static const char yaml[] = "\xEF\xBB\xBFkey: value\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(block_mapping, utf8_bom_skipped_multiline) {
  /* The BOM must not shift the point where line_start_pos() puts the start of
   * the first physical line. A document with one key (see utf8_bom_skipped
   * above) cannot catch this error, because it never compares the column of
   * a SECOND line against anything.
   *
   * A second key at column 0 needs the first line to start at byte 3,
   * directly after the BOM, and not at byte 0. Otherwise every column on
   * that line comes out 3 bytes too high, and the parser rejects this
   * document as "trailing content". */
  static const char yaml[] =
      "\xEF\xBB\xBF"
      "a: 1\nb: 2\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), 2LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, utf8_bom_skipped_before_explicit_doc_marker) {
  /* The same hazard, reached here through the column-0 rule of at_doc_marker()
   * instead of the ordinary comparison of indentation of the test above. A
   * "---" directly after the BOM must be a real document-start marker, and
   * the parser must not silently take it as three bytes of plain-scalar
   * text. */
  static const char yaml[] = "\xEF\xBB\xBF---\na: 1\nb: 2\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), 2LL);
  cyaml_destroy(doc);
}

TEST(multi_document, utf8_bom_skipped_before_first_document_marker) {
  /* The most severe form of the same hazard: the 3 bytes of the BOM must not
   * count as part of the indentation of line 1. When they do, the first
   * "---" fails the column-0 check of at_doc_marker(), the parser silently
   * takes it as plain-scalar text ("--- a"), and two separate documents
   * become one with no error reported at all. */
  static const char yaml[] = "\xEF\xBB\xBF---\na\n---\nb\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "a");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "b");
  cyaml_destroy(doc);
}

static void require_tab_rejected(const char *yaml) {
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

/* Parse yaml, require success, and require that the flow serialization of
 * the result is exactly want. */
static bool _tab_accepted_as(const char *yaml, const char *want) {
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  char *flow = doc ? cyaml_serialize_flow(doc) : NULL;
  bool ok = err == NULL && flow && strcmp(flow, want) == 0;
  if (!ok)
    fprintf(stderr, "tab case %s: err=%s flow=%s want=%s\n", yaml,
            err ? err : "(none)", flow ? flow : "(null)", want);
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(doc);
  return ok;
}

TEST(bom_prefix, comment_directly_after_bom) {
  /* A '#' at the start of the stream opens a comment. With a byte order mark
   the stream starts after the mark, so a '#' right after it is still at the
   start of the stream and still opens a comment; a text editor that saves
   with a BOM produces exactly this for a file whose first line is a
   comment. This test is non-vacuous: when the comment test treats only
   offset 0 as the start of the stream, the parse fails with "unexpected
   '#' at position 3". */
  static const char yaml[] = "\xEF\xBB\xBF# c\na: 1\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  cyaml_destroy(doc);

  /* A stream that is only a BOM and a comment is an empty document. */
  static const char only[] = "\xEF\xBB\xBF#";
  doc = cyaml_parse(only, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(bom_prefix, comment_after_bom_on_value_line) {
  /* rest_of_line_is_blank() makes the same start-of-stream test, so a BOM
   followed directly by "# c" and then a document must parse through that
   path too, while a '#' that touches content after the mark stays
   content. */
  static const char yaml[] = "\xEF\xBB\xBF#c\nk: # v\n  x\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "k")), "x");
  cyaml_destroy(doc);

  static const char content[] =
      "\xEF\xBB\xBF"
      "a#b: 1\n";
  doc = cyaml_parse(content, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a#b")), 1LL);
  cyaml_destroy(doc);
}

TEST(bom_prefix, bom_before_a_later_document) {
  /* YAML 1.2 allows a byte order mark in the prefix of every document of a
   stream, not only the first, and a concatenation of files that each start
   with one produces this. The mark is not content, and the "---" after it
   sits at column 0. This test is non-vacuous: without the prefix skip, the
   mark and the rest of its line become a third document holding the plain
   scalar "\xEF\xBB\xBF# c", or the "---" after the mark is not a marker. */
  static const char yaml[] =
      "a: 1\n...\n\xEF\xBB\xBF# c\n--- b\n...\n\xEF\xBB\xBF--- {k: v}\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(cyaml_list_get(doc, 0), "a")),
             1LL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "b");
  REQUIRE_STREQ(
      cyaml_str_val(cyaml_dictionary_get(cyaml_list_get(doc, 2), "k")), "v");
  cyaml_destroy(doc);
}

TEST(bom_prefix, bom_before_a_later_block_document_keeps_columns) {
  /* The columns of the line after a later mark count from the end of the
   mark, as they do for a mark at the start of the stream, so the second key
   of the block mapping sits at column 0 and must join the first one. */
  static const char yaml[] =
      "x\n...\n\xEF\xBB\xBF"
      "a: 1\nb: 2\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  cyaml second = cyaml_list_get(doc, 1);
  REQUIRE_EQ(cyaml_dictionary_size(second), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(second, "a"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(second, "a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(second, "b")), 2LL);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         BLOCK SEQUENCES                                    */
/* ========================================================================== */

TEST(block_sequence, simple_strings) {
  const char *yaml =
      "- alpha\n"
      "- beta\n"
      "- gamma\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)3);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "alpha");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "beta");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 2)), "gamma");
  cyaml_destroy(doc);
}

TEST(block_sequence, mixed_types) {
  const char *yaml =
      "- 1\n"
      "- true\n"
      "- hello\n"
      "- ~\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)4);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 0)), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 1)), CYAML_BOOL);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 2)), CYAML_STRING);
  cyaml elem3 = cyaml_list_get(doc, 3);
  REQUIRE_NE((void *)elem3, NULL);
  REQUIRE_EQ(cyaml_type(elem3), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(block_sequence, of_mappings) {
  const char *yaml =
      "- name: alice\n"
      "  age: 30\n"
      "- name: bob\n"
      "  age: 25\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);

  cyaml alice = cyaml_list_get(doc, 0);
  REQUIRE_NE((void *)alice, NULL);
  REQUIRE_EQ(cyaml_type(alice), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(alice, "name")), "alice");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(alice, "age")), 30LL);

  cyaml bob = cyaml_list_get(doc, 1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(bob, "name")), "bob");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(bob, "age")), 25LL);

  cyaml_destroy(doc);
}

TEST(block_sequence, nested_sequence) {
  const char *yaml =
      "matrix:\n"
      "  - - 1\n"
      "    - 2\n"
      "  - - 3\n"
      "    - 4\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);

  cyaml matrix = cyaml_dictionary_get(doc, "matrix");
  REQUIRE_NE((void *)matrix, NULL);
  REQUIRE_EQ(cyaml_type(matrix), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(matrix), (size_t)2);

  cyaml row0 = cyaml_list_get(matrix, 0);
  REQUIRE_EQ(cyaml_type(row0), CYAML_LIST);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(row0, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(row0, 1)), 2LL);

  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         FLOW COLLECTIONS                                   */
/* ========================================================================== */

TEST(flow, sequence_basic) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[1, 2, 3]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc, 1)), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc, 2)), 3LL);
  cyaml_destroy(doc);
}

TEST(flow, sequence_empty) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)0);
  cyaml_destroy(doc);
}

TEST(flow, sequence_mixed) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[1, \"two\", true, ~]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)4);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 0)), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 1)), CYAML_STRING);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 2)), CYAML_BOOL);
  cyaml elem3 = cyaml_list_get(doc, 3);
  REQUIRE_NE((void *)elem3, NULL);
  REQUIRE_EQ(cyaml_type(elem3), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(flow, mapping_basic) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{host: localhost, port: 8080}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "host")), "localhost");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

TEST(flow, mapping_empty) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)0);
  cyaml_destroy(doc);
}

TEST(flow, nested_flow) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{a: [1, 2], b: {x: true}}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_type(a), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(a), (size_t)2);

  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_EQ(cyaml_type(b), CYAML_DICTIONARY);
  REQUIRE_TRUE(cyaml_bool_val(cyaml_dictionary_get(b, "x")));

  cyaml_destroy(doc);
}

TEST(flow, flow_value_in_block_mapping) {
  const char *yaml =
      "ports:\n"
      "  - {host: 80, container: 8080}\n"
      "  - {host: 443, container: 8443}\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml ports = cyaml_dictionary_get(doc, "ports");
  REQUIRE_NE((void *)ports, NULL);
  REQUIRE_EQ(cyaml_type(ports), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(ports), (size_t)2);

  cyaml p0 = cyaml_list_get(ports, 0);
  REQUIRE_EQ(cyaml_type(p0), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(p0, "host")), 80LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(p0, "container")), 8080LL);

  cyaml_destroy(doc);
}

TEST(flow, mapping_integer_key) {
  /* The parser turns an integer key of a flow mapping into a string before
   * storing it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{42: the_answer}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "42")), "the_answer");
  cyaml_destroy(doc);
}

TEST(flow, mapping_non_string_keys) {
  /* The parser also turns a null key and a bool key of a flow dictionary into
   * a string. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{null: 1, true: 2, false: 3}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "null")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "true")), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "false")), 3LL);
  cyaml_destroy(doc);
}

TEST(flow, value_with_bare_colon) {
  /* A plain scalar value in a flow dictionary can hold a ':' with no
   * whitespace and no flow terminator after it, usually in a URL, so the
   * parser must not truncate "http://example.com" at the first ':'. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{url: http://example.com, port: 80}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "url")),
                "http://example.com");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "port")), 80LL);
  cyaml_destroy(doc);
}

TEST(flow, plain_scalar_with_colon_in_sequence) {
  /* The same rule holds inside a flow list. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[http://a.b, ftp://x.y]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "http://a.b");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "ftp://x.y");
  cyaml_destroy(doc);
}

TEST(flow, value_with_bare_colon_serialize_round_trip) {
  /* For a dictionary with a value that holds a bare ':', the output of
   * cyaml_serialize_flow must parse again into the original string. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "url",
                                  cyaml_create_string("http://example.com")),
             ccol_success);
  char *s = cyaml_serialize_flow(doc);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc2, "url")),
                "http://example.com");
  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

/* ========================================================================== */
/*                         ANCHORS AND ALIASES                                */
/* ========================================================================== */

TEST(anchors, basic_alias) {
  /* The "production: *def" line must resolve the alias to the content of the
   * anchored mapping. That reference is what keeps this test non-vacuous,
   * because a document that defines &def but never dereferences it cannot
   * detect a regression in the resolution of an alias. */
  const char *yaml =
      "default: &def\n"
      "  timeout: 30\n"
      "  retries: 3\n"
      "production: *def\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);

  cyaml def = cyaml_dictionary_get(doc, "default");
  REQUIRE_EQ(cyaml_type(def), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(def, "timeout")), 30LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(def, "retries")), 3LL);

  cyaml production = cyaml_dictionary_get(doc, "production");
  REQUIRE_EQ(cyaml_type(production), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(production, "timeout")), 30LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(production, "retries")), 3LL);

  cyaml_destroy(doc);
}

TEST(anchors, alias_resolves_to_anchor_value) {
  /* An alias must give a node whose content matches the anchored value. */
  const char *yaml =
      "base: &base 42\n"
      "copy: *base\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);

  cyaml base = cyaml_dictionary_get(doc, "base");
  cyaml copy = cyaml_dictionary_get(doc, "copy");
  REQUIRE_NE((void *)base, NULL);
  REQUIRE_NE((void *)copy, NULL);
  REQUIRE_EQ(cyaml_type(base), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_type(copy), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(base), 42LL);
  REQUIRE_EQ(cyaml_int_val(copy), 42LL);

  cyaml_destroy(doc);
}

TEST(anchors, alias_is_independent_copy) {
  /* An alias must give a deep clone, and not a shared reference. */
  const char *yaml =
      "src: &a\n"
      "  val: 1\n"
      "dst: *a\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml src = cyaml_dictionary_get(doc, "src");
  cyaml dst = cyaml_dictionary_get(doc, "dst");
  REQUIRE_NE((void *)src, NULL);
  REQUIRE_NE((void *)dst, NULL);
  /* The two are separate nodes. */
  REQUIRE_NE((void *)src, (void *)dst);
  REQUIRE_EQ(cyaml_type(src), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_type(dst), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(src, "val")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(dst, "val")), 1LL);

  cyaml_destroy(doc);
}

TEST(anchors, scalar_anchor_and_alias) {
  const char *yaml =
      "base: &base_port 9090\n"
      "alt: *base_port\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml base = cyaml_dictionary_get(doc, "base");
  REQUIRE_NE((void *)base, NULL);
  cyaml alt = cyaml_dictionary_get(doc, "alt");
  REQUIRE_NE((void *)alt, NULL);
  REQUIRE_EQ(cyaml_type(base), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_type(alt), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(base), 9090LL);
  REQUIRE_EQ(cyaml_int_val(alt), 9090LL);
  /* Must be separate nodes. */
  REQUIRE_NE((void *)base, (void *)alt);

  cyaml_destroy(doc);
}

TEST(anchors, sequence_anchor) {
  const char *yaml =
      "shared: &ports\n"
      "  - 80\n"
      "  - 443\n"
      "copy: *ports\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml shared = cyaml_dictionary_get(doc, "shared");
  REQUIRE_NE((void *)shared, NULL);
  cyaml copy = cyaml_dictionary_get(doc, "copy");
  REQUIRE_NE((void *)copy, NULL);
  REQUIRE_EQ(cyaml_type(shared), CYAML_LIST);
  REQUIRE_EQ(cyaml_type(copy), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(shared), (size_t)2);
  REQUIRE_EQ(cyaml_list_len(copy), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(copy, 0)), 80LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(copy, 1)), 443LL);

  cyaml_destroy(doc);
}

TEST(anchors, anchor_name_reuse) {
  /* The second definition of &a must replace the first, so the alias *a
   * resolves to the most recent binding. */
  const char *yaml =
      "first: &a 100\n"
      "second: &a 200\n"
      "alias: *a\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "first")), 100LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "second")), 200LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "alias")), 200LL);
  cyaml_destroy(doc);
}

TEST(anchors, many_linear_aliases_to_small_anchor_still_works) {
  /* A guard for the budget of node allocations (CYAML_MAX_PARSE_NODES; see
   * exponential_alias_expansion_rejected_not_exhausted). Many aliases to the
   * SAME small anchor are ordinary, legitimate, linear reuse: each *a clone
   * is independent but small, and the total stays far below the budget, so
   * the parser must not reject this document. */
  size_t n = 2000;
  size_t cap = 64 + n * 8;
  char *input = malloc(cap);
  REQUIRE_NE((void *)input, NULL);
  size_t off = (size_t)snprintf(input, cap, "root: &a 42\nlist: [");
  for (size_t i = 0; i < n; i++)
    off += (size_t)snprintf(input + off, cap - off, "*a,");
  off += (size_t)snprintf(input + off, cap - off, "]\n");
  char *err = NULL;
  cyaml doc = cyaml_parse(input, &err);
  free(input);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(doc, "list")), n);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         PATH NAVIGATION (cyaml_get)                       */
/* ========================================================================== */

TEST(path, get_mapping_key) {
  const char *yaml =
      "server:\n"
      "  host: example.com\n"
      "  port: 443\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);

  cyaml host = cyaml_get(doc, "server.host");
  REQUIRE_NE((void *)host, NULL);
  REQUIRE_STREQ(cyaml_str_val(host), "example.com");

  cyaml port = cyaml_get(doc, "server.port");
  REQUIRE_NE((void *)port, NULL);
  REQUIRE_EQ(cyaml_int_val(port), 443LL);

  REQUIRE_EQ((void *)cyaml_get(doc, "server.missing"), NULL);
  REQUIRE_EQ((void *)cyaml_get(doc, "nope"), NULL);

  cyaml_destroy(doc);
}

TEST(path, get_sequence_index) {
  const char *yaml =
      "items:\n"
      "  - first\n"
      "  - second\n"
      "  - third\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);

  cyaml first = cyaml_get(doc, "items.#0");
  REQUIRE_NE((void *)first, NULL);
  REQUIRE_STREQ(cyaml_str_val(first), "first");

  cyaml third = cyaml_get(doc, "items.#2");
  REQUIRE_NE((void *)third, NULL);
  REQUIRE_STREQ(cyaml_str_val(third), "third");

  REQUIRE_EQ((void *)cyaml_get(doc, "items.#99"), NULL);

  cyaml_destroy(doc);
}

TEST(path, get_deep_nested) {
  const char *yaml =
      "a:\n"
      "  b:\n"
      "    c:\n"
      "      d: found\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);

  cyaml found = cyaml_get(doc, "a.b.c.d");
  REQUIRE_NE((void *)found, NULL);
  REQUIRE_STREQ(cyaml_str_val(found), "found");

  cyaml_destroy(doc);
}

TEST(path, get_root_empty_path) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key: val\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  /* An empty path returns the root. */
  REQUIRE_EQ((void *)cyaml_get(doc, ""), (void *)doc);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         PATH MUTATION (cyaml_set)                         */
/* ========================================================================== */

TEST(path, set_existing_scalar) {
  char *err = NULL;
  cyaml doc = cyaml_parse("port: 8080\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "port", 9090), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "port")), 9090LL);

  cyaml_destroy(doc);
}

TEST(path, set_new_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "b", 2), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "b")), 2LL);

  cyaml_destroy(doc);
}

TEST(path, set_string_literal) {
  char *err = NULL;
  cyaml doc = cyaml_parse("env: dev\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "env", "prod"), ccol_success);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "env")), "prod");

  cyaml_destroy(doc);
}

TEST(path, set_nested) {
  char *err = NULL;
  cyaml doc = cyaml_parse("server:\n  port: 80\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "server.port", 443), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "server.port")), 443LL);

  cyaml_destroy(doc);
}

TEST(path, set_sequence_element) {
  char *err = NULL;
  cyaml doc = cyaml_parse("nums:\n  - 1\n  - 2\n  - 3\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "nums.#1", 99), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "nums.#1")), 99LL);

  cyaml_destroy(doc);
}

TEST(path, set_bool_type) {
  /* Calling cyaml_set with a bool literal drives the CYAML_BOOL branch of
   * _cyaml_type_of and of node_reinit_scalar. */
  char *err = NULL;
  cyaml doc = cyaml_parse("flag: false\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_set(doc, "flag", (bool)true), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "flag")), CYAML_BOOL);
  REQUIRE_TRUE(cyaml_bool_val(cyaml_get(doc, "flag")));
  cyaml_destroy(doc);
}

TEST(path, set_float_type) {
  /* Calling cyaml_set with a float literal drives the float branch of
   * _cyaml_type_of in node_reinit_scalar, which tests
   * raw_size == sizeof(float). */
  char *err = NULL;
  cyaml doc = cyaml_parse("scale: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_set(doc, "scale", 1.5f), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "scale")), CYAML_FLOAT);
  /* A float holds 1.5 exactly, so the change from float to double loses
   * nothing. */
  REQUIRE_EQ(cyaml_double_val(cyaml_get(doc, "scale")), 1.5);
  cyaml_destroy(doc);
}

TEST(path, set_integer_various_widths_and_signs_round_trip) {
  /* Every other integer test of cyaml_set() gives a bare int literal, which
   * drives only one case of node_reinit_scalar (raw_size is 4 and is_signed
   * is true). This test drives every other combination of that switch, which
   * covers raw_size (1, 2, 4 and 8) against is_signed (true and false): the
   * widths that _cyaml_type_of() and _cyaml_is_signed() dispatch on. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 0\nb: 0\nc: 0\nd: 0\ne: 0\nf: 0\ng: 0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  REQUIRE_EQ(cyaml_set(doc, "a", (signed char)-42), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "a")), -42LL);

  REQUIRE_EQ(cyaml_set(doc, "b", (short)-1234), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "b")), -1234LL);

  REQUIRE_EQ(cyaml_set(doc, "c", (long long)-9000000000LL), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "c")), -9000000000LL);

  REQUIRE_EQ(cyaml_set(doc, "d", (unsigned char)200), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "d")), 200LL);

  REQUIRE_EQ(cyaml_set(doc, "e", (unsigned short)50000), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "e")), 50000LL);

  REQUIRE_EQ(cyaml_set(doc, "f", (unsigned int)3000000000U), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "f")), 3000000000LL);

  REQUIRE_EQ(cyaml_set(doc, "g", (unsigned long long)9000000000000000000ULL),
             ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "g")), 9000000000000000000LL);

  cyaml_destroy(doc);
}

TEST(path, set_sequence_element_out_of_bounds) {
  /* Calling cyaml_set on a sequence index past the last element must fail
   * with ccol_key_not_found and change no existing element. A "#N" component
   * with a valid syntax but an index out of range is an absent path
   * component, the same kind of absent component as a dictionary key that is
   * not there. The function cyaml_delete documents the same distinction; see
   * delete.path_out_of_range_list_index_returns_key_not_found. */
  char *err = NULL;
  cyaml doc = cyaml_parse("nums:\n  - 1\n  - 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "nums.#99", 42), ccol_key_not_found);

  /* Existing elements must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "nums.#0")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "nums.#1")), 2LL);

  cyaml_destroy(doc);
}

TEST(path, set_on_list_root) {
  /* The call cyaml_set(list_root, "#N", val) must work when the root node is
   * itself a CYAML_LIST and not a dictionary. */
  char *err = NULL;
  cyaml root = cyaml_parse("- 1\n- 2\n- 3\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);

  REQUIRE_EQ(cyaml_set(root, "#1", 99), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 1)), 99LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 2)), 3LL);

  cyaml_destroy(root);
}

TEST(path, set_fails_on_scalar_parent) {
  /* cyaml_set must fail when the middle node is a scalar. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  /* "key" is an integer node, so the path "key.sub" has no valid parent
   * container. */
  REQUIRE_EQ(cyaml_set(doc, "key.sub", 1), ccol_invalid_args);

  /* The original value must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, set_missing_parent) {
  /* cyaml_set must fail when a middle node in the path is not there, and it
   * must change nothing in the original tree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "missing.sub", 1), ccol_key_not_found);

  /* The original key must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path,
     set_malformed_index_syntax_in_non_leaf_component_returns_invalid_args) {
  /* A malformed "#N" index is an error in the path that the caller built, not
   * a question about the data that is there, and this must hold at ANY
   * position in the path, not only at the leaf.
   *
   * navigate_y() must therefore keep apart a malformed index in the middle of
   * a path and a component in the middle that is truly absent, instead of
   * giving one NULL result for both. With one shared NULL result, the caller
   * gets ccol_key_not_found here in place of ccol_invalid_args. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "items.#xyz.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(doc, "items")), (size_t)2);

  cyaml_destroy(doc);
}

TEST(path, get_index_with_leading_whitespace_is_malformed) {
  /* A bare call to strtol() silently accepts whitespace before the digits, as
   * well as an explicit '+' at the start, so the library must reject
   * "items.#  0" and "items.#+0" instead of silently taking them as index 0.
   * The documented "#N" grammar needs digits only, directly after the '#'. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ((void *)cyaml_get(doc, "items.#  0.a"), NULL);
  REQUIRE_EQ((void *)cyaml_get(doc, "items.#+0.a"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#0.a")), 1LL);

  cyaml_destroy(doc);
}

TEST(path, set_index_with_leading_whitespace_returns_invalid_args) {
  /* This test mirrors the two forms of
   * path.get_index_with_leading_whitespace_is_malformed (whitespace at the
   * start and an explicit '+' at the start): the set path must not accept a
   * sign in the style of strtol(). The get path and the delete path have
   * their own tests for the '+' form below, and this test catches such a
   * regression on the set path. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "items.# 0.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_set(doc, "items.#+0.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#0.a")), 1LL);

  cyaml_destroy(doc);
}

TEST(delete, path_index_with_leading_sign_returns_invalid_args) {
  /* This test mirrors the two forms of
   * path.get_index_with_leading_whitespace_is_malformed (an explicit '+' at
   * the start and whitespace at the start): the delete path must not accept
   * whitespace in the style of strtol(), and this test catches such a
   * regression on the delete path. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_delete(doc, "items.#+0"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_delete(doc, "items.# 0"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(doc, "items")), (size_t)2);

  cyaml_destroy(doc);
}

TEST(path, set_wrong_type_in_non_leaf_component_returns_invalid_args) {
  /* A scalar can appear in the middle of a path, with more components after
   * it, but it has no children to walk into. That is an error of the wrong
   * type in the path (ccol_invalid_args), not an error about an absent
   * component (ccol_key_not_found). Here "a" is the scalar 1, so "a.b.c" must
   * fail inside navigate_y() itself, while that function tries to resolve
   * the parent segment "a.b". */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "a.b.c", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "a")), 1LL);

  cyaml_destroy(doc);
}

TEST(path, set_null_value) {
  /* Calling cyaml_set with a NULL literal must change the type of the leaf to
   * CYAML_NULL. The type of NULL is void *, and _cyaml_type_of maps void * to
   * CYAML_NULL with its own explicit association, separate from the default
   * branch, which gives _CYAML_TYPE_UNSUPPORTED for a type that the library
   * truly does not support (see set_unsupported_type_is_rejected below). */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "key", NULL), ccol_success);
  cyaml leaf = cyaml_get(doc, "key");
  REQUIRE_NE((void *)leaf, NULL);
  REQUIRE_EQ(cyaml_type(leaf), CYAML_NULL);

  cyaml_destroy(doc);
}

TEST(path, set_unsupported_type_is_rejected) {
  /* _cyaml_type_of() does not know every C type. A value of a type that it
   * does not know, for example long double, must fail with ccol_invalid_args
   * and leave the existing leaf completely untouched, instead of quietly
   * succeeding and writing CYAML_NULL over the leaf. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  long double ld = 3.14L;
  REQUIRE_EQ(cyaml_set(doc, "key", ld), ccol_invalid_args);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "key")), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  /* The call also fails when the leaf is not there yet: that is the
   * node_make_scalar path, as opposed to the node_reinit_scalar path above,
   * which works on a leaf that already exists. */
  REQUIRE_EQ(cyaml_set(doc, "brand_new_key", ld), ccol_invalid_args);
  REQUIRE_EQ((void *)cyaml_get(doc, "brand_new_key"), NULL);

  cyaml_destroy(doc);
}

TEST(path, set_string_variable) {
  /* Calling cyaml_set with a char * variable instead of a string literal. The
   * value raw names a const char *, which node_reinit_scalar dereferences
   * directly. */
  char *err = NULL;
  cyaml doc = cyaml_parse("env: dev\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  char *val = "prod";
  REQUIRE_EQ(cyaml_set(doc, "env", val), ccol_success);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "env")), "prod");

  cyaml_destroy(doc);
}

TEST(path, set_replaces_container_leaf) {
  /* A call to cyaml_set can name a path whose leaf is a container, and the
   * call must replace that container in place with the new scalar, through
   * node_clear followed by a new initialization of the node. */
  char *err = NULL;
  cyaml doc = cyaml_parse("data:\n  - 1\n  - 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "data")), CYAML_LIST);

  REQUIRE_EQ(cyaml_set(doc, "data", 42), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "data")), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "data")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, degenerate_path_trailing_dot) {
  /* A path that ends with a dot gives an empty leaf component: cyaml_set must
   * reject such a path, and cyaml_get must return NULL for it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "key.", 1), ccol_invalid_args);
  REQUIRE_EQ((void *)cyaml_get(doc, "key."), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, degenerate_path_consecutive_dots) {
  /* A path with two dots together gives an empty component in the middle, so
   * the walk through the path must fail and return NULL. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a:\n  b: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ((void *)cyaml_get(doc, "a..b"), NULL);

  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                       PATH ESCAPE SEQUENCE TESTS                           */
/* ========================================================================== */

TEST(path, get_escaped_dot_flat_key) {
  /* The key "a.b" holds a literal dot; reach it with "a\\.b". */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a.b\": 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\.b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 42LL);
  /* A dot with no escape must NOT find the key. */
  REQUIRE_EQ((void *)cyaml_get(doc, "a.b"), NULL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_backslash_flat_key) {
  /* The key "a\b" holds a literal backslash; reach it with "a\\\\b". */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\\\\b\": 7\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\\\b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 7LL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_dot_nested_path) {
  /* The outer key is the plain "outer", and the inner key "k.ey" holds a
   * literal dot. */
  char *err = NULL;
  cyaml doc = cyaml_parse("outer:\n  \"k.ey\": 99\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "outer.k\\.ey");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 99LL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_dot_both_components) {
  /* Both levels have a key that holds a dot: "a.b" -> "c.d". */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a.b\":\n  \"c.d\": 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\.b.c\\.d");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 1LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_creates_new_key) {
  /* Create a new key "x.y", which holds a literal dot, at the root. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "x\\.y", 55), ccol_success);
  cyaml node = cyaml_get(doc, "x\\.y");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 55LL);
  /* The call must not create a nested "x" key that nobody asked for. */
  REQUIRE_EQ((void *)cyaml_get(doc, "x"), NULL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_updates_existing_key) {
  /* Change the value of the existing key "p.q", which holds a literal dot. */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"p.q\": 0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "p\\.q", 123), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "p\\.q")), 123LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_nested_path) {
  /* Set "outer"."k.ey" with the escaped path "outer.k\\.ey". */
  char *err = NULL;
  cyaml doc = cyaml_parse("outer:\n  \"k.ey\": 0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "outer.k\\.ey", 77), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "outer.k\\.ey")), 77LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_backslash_creates_key) {
  /* Create the key "a\\b", which holds a literal backslash, with "a\\\\b". */
  char *err = NULL;
  cyaml doc = cyaml_parse("{}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "a\\\\b", 9), ccol_success);
  REQUIRE_NE((void *)cyaml_get(doc, "a\\\\b"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "a\\\\b")), 9LL);
  cyaml_destroy(doc);
}

TEST(path, set_replaces_container_list_element) {
  /* A call to cyaml_set can name a sequence index whose current value is a
   * container, and the call must replace that container in place with the
   * new scalar, through node_clear followed by a new initialization of the
   * node. This test checks the same node_reinit_scalar path as
   * set_replaces_container_leaf, but reaches it through the list branch of
   * _cyaml_set_typed. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "items:\n"
      "  - - 1\n"
      "    - 2\n"
      "  - 99\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "items.#0")), CYAML_LIST);

  REQUIRE_EQ(cyaml_set(doc, "items.#0", 42), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "items.#0")), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#0")), 42LL);
  /* The element beside it must stay untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#1")), 99LL);

  cyaml_destroy(doc);
}

TEST(path, set_single_component_oom) {
  /* _cyaml_set_typed makes two ccol_strdup calls for a path of one component
   * (a path with no dots): the first makes the scratch copy that the
   * function searches for dots, and the second makes leaf_copy. When the
   * second strdup fails because memory runs out, the function must return
   * ccol_not_enough_memory and leave the document whole, instead of giving
   * NULL to path_unescape_component. */
  g_alloc_remaining = -1;
  char *err = NULL;
  cyaml doc = cyaml_parse_mp("port: 8080\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  /* Let exactly one malloc succeed, so the first strdup (the scratch copy)
   * succeeds and the second strdup (leaf_copy) fails. */
  g_alloc_remaining = 1;
  long long new_val = 9090LL;
  ccol_retval_t r = _cyaml_set_typed(doc, "port", CYAML_INTEGER, &new_val,
                                     sizeof(new_val), true);
  /* Reset the budget before checking the result. A REQUIRE_* failure returns
   * from this function at once, so a reset after the check would leave
   * g_alloc_remaining at its empty budget, and every later test in this
   * binary that uses &g_counting_mp would then fail as well, turning one
   * clear failure here into a chain of failures that look unrelated. */
  g_alloc_remaining = -1;
  REQUIRE_EQ(r, ccol_not_enough_memory);

  /* The document must stay unchanged. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

TEST(path, delete_single_component_oom) {
  /* _cyaml_delete makes two ccol_strdup calls for a path of one component (a
   * path with no dots): the first makes the scratch copy that the function
   * searches for dots, and the second makes leaf_copy. When the second
   * strdup fails because memory runs out, the function must return
   * ccol_not_enough_memory and leave the document whole, instead of giving
   * NULL to path_unescape_component. */
  g_alloc_remaining = -1;
  char *err = NULL;
  cyaml doc = cyaml_parse_mp("port: 8080\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  /* Let exactly one malloc succeed, so the first strdup (the scratch copy)
   * succeeds and the second strdup (leaf_copy) fails. */
  g_alloc_remaining = 1;
  ccol_retval_t r = _cyaml_delete(doc, "port");
  /* The same comment in set_single_component_oom above explains why the reset
   * happens before this check and not after it. */
  g_alloc_remaining = -1;
  REQUIRE_EQ(r, ccol_not_enough_memory);

  /* The document must stay unchanged. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/* Collect the keys of dict, in iteration order, joined by ','. */
static void _dict_keys(cyaml dict, char *out, size_t cap) {
  size_t len = 0;
  out[0] = '\0';
  cyaml_dictionary_iter it;
  for (bool more = cyaml_dictionary_first(dict, &it); more;
       more = cyaml_dictionary_next(&it)) {
    int w = snprintf(out + len, cap - len, "%s%s", len ? "," : "", it.key);
    if (w < 0 || (size_t)w >= cap - len) break;
    len += (size_t)w;
  }
}

TEST(dict_order, parse_keeps_document_order_in_both_serializers) {
  /* A dictionary keeps its members in the order of the document, and both
   * serializers write that order. The keys are neither sorted nor in any
   * order that a hash or a reversal could produce, so this test is
   * non-vacuous: a serializer that follows any other order fails on the
   * exact text. */
  const char *src =
      "zeta: 1\nalpha: 2\nmid: {q: 1, b: 2, x: 3}\nbeta: [1, 2]\ngamma: 5\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(src, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  char *block = cyaml_serialize(doc);
  char *flow = cyaml_serialize_flow(doc);
  bool block_ok =
      block && strcmp(block,
                      "zeta: 1\nalpha: 2\nmid:\n  q: 1\n  b: 2\n  x: 3\n"
                      "beta:\n  - 1\n  - 2\ngamma: 5\n") == 0;
  bool flow_ok = flow && strcmp(flow,
                                "{zeta: 1, alpha: 2, mid: {q: 1, b: 2, x: 3}, "
                                "beta: [1, 2], gamma: 5}") == 0;
  if (block) cyaml_serialize_free(block);
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(doc);
  REQUIRE_TRUE(block_ok);
  REQUIRE_TRUE(flow_ok);
}

TEST(dict_order, many_keys_keep_their_order) {
  /* Enough members to force several growths of the map, none of which may
   * disturb the order. */
  char src[8192];
  size_t len = 0;
  for (int i = 0; i < 300; i++)
    len += (size_t)snprintf(src + len, sizeof(src) - len, "k%d: %d\n",
                            (i * 7919) % 1000, i);
  char *err = NULL;
  cyaml doc = cyaml_parse(src, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  int i = 0;
  bool ordered = true;
  cyaml_dictionary_iter it;
  for (bool more = cyaml_dictionary_first(doc, &it); more;
       more = cyaml_dictionary_next(&it), i++) {
    char want[16];
    snprintf(want, sizeof(want), "k%d", (i * 7919) % 1000);
    if (strcmp(it.key, want) != 0 || cyaml_int_val(it.value) != i)
      ordered = false;
  }
  cyaml copy = cyaml_clone(doc);
  char *a = cyaml_serialize(doc);
  char *b = copy ? cyaml_serialize(copy) : NULL;
  bool clone_same = a && b && strcmp(a, b) == 0;
  bool text_same = a && strcmp(a, src) == 0;
  if (a) cyaml_serialize_free(a);
  if (b) cyaml_serialize_free(b);
  cyaml_destroy(copy);
  cyaml_destroy(doc);
  REQUIRE_EQ(i, 300);
  REQUIRE_TRUE(ordered);
  REQUIRE_TRUE(clone_same);
  REQUIRE_TRUE(text_same);
}

TEST(dict_order, duplicate_key_keeps_first_place_and_last_value) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\nb: 2\na: 3\nc: 4\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  char keys[64];
  _dict_keys(doc, keys, sizeof(keys));
  long long a = cyaml_int_val(cyaml_dictionary_get(doc, "a"));
  cyaml_destroy(doc);
  REQUIRE_STREQ(keys, "a,b,c");
  REQUIRE_EQ(a, 3LL);
}

TEST(dict_order, set_replace_keeps_place_and_remove_then_add_appends) {
  cyaml d = cyaml_create_dictionary();
  cyaml_dictionary_set(d, "x", cyaml_create_int(1));
  cyaml_dictionary_set(d, "y", cyaml_create_int(2));
  cyaml_dictionary_set(d, "z", cyaml_create_int(3));
  char keys[64];
  cyaml_dictionary_set(d, "x", cyaml_create_int(10));
  _dict_keys(d, keys, sizeof(keys));
  bool replace_ok = strcmp(keys, "x,y,z") == 0 &&
                    cyaml_int_val(cyaml_dictionary_get(d, "x")) == 10;
  cyaml_dictionary_remove(d, "x");
  cyaml_dictionary_set(d, "x", cyaml_create_int(11));
  _dict_keys(d, keys, sizeof(keys));
  bool readd_ok = strcmp(keys, "y,z,x") == 0;
  /* A path set of a new key appends too, and a path set of a present key
   * keeps its place. */
  cyaml_set(d, "w", 4);
  cyaml_set(d, "y", 20);
  _dict_keys(d, keys, sizeof(keys));
  bool path_ok = strcmp(keys, "y,z,x,w") == 0;
  cyaml_destroy(d);
  REQUIRE_TRUE(replace_ok);
  REQUIRE_TRUE(readd_ok);
  REQUIRE_TRUE(path_ok);
}

TEST(dict_order, merged_members_take_the_place_of_the_merge_key) {
  /* The members that a "<<" entry brings in take its place: after the
   * explicit members before it and before the explicit members after it.
   * Sources arrive in the order in which the entry lists them, each in its
   * own order; an explicit key keeps its own place and value, and an earlier
   * source wins over a later one. */
  const char *src =
      "b1: &b1 {x: 1, y: 2, w: 0}\n"
      "b2: &b2 {v: 7, x: 8, u: 9}\n"
      "svc:\n"
      "  first: 0\n"
      "  <<: [*b1, *b2]\n"
      "  y: 20\n"
      "  last: 5\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(src, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml svc = cyaml_dictionary_get(doc, "svc");
  char keys[128];
  _dict_keys(svc, keys, sizeof(keys));
  long long x = cyaml_int_val(cyaml_dictionary_get(svc, "x"));
  long long y = cyaml_int_val(cyaml_dictionary_get(svc, "y"));
  char *flow = cyaml_serialize_flow(svc);
  bool flow_ok = flow && strcmp(flow,
                                "{first: 0, x: 1, w: 0, v: 7, u: 9, \"y\": 20, "
                                "last: 5}") == 0;
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(doc);
  REQUIRE_STREQ(keys, "first,x,w,v,u,y,last");
  REQUIRE_EQ(x, 1LL);
  REQUIRE_EQ(y, 20LL);
  REQUIRE_TRUE(flow_ok);

  /* A merge key first and a merge key last, in a flow mapping. */
  doc = cyaml_parse(
      "b: &b {p: 1, q: 2}\nf: {<<: *b, r: 3}\ng: {s: 0, <<: *b}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  char fk[64], gk[64];
  _dict_keys(cyaml_dictionary_get(doc, "f"), fk, sizeof(fk));
  _dict_keys(cyaml_dictionary_get(doc, "g"), gk, sizeof(gk));
  cyaml_destroy(doc);
  REQUIRE_STREQ(fk, "p,q,r");
  REQUIRE_STREQ(gk, "s,p,q");
}

TEST(dict_order, alias_and_clone_keep_the_order) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {m: 1, c: 2, k: 3}\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  char ka[64], kb[64], kc[64];
  _dict_keys(cyaml_dictionary_get(doc, "a"), ka, sizeof(ka));
  _dict_keys(cyaml_dictionary_get(doc, "b"), kb, sizeof(kb));
  cyaml copy = cyaml_clone(cyaml_dictionary_get(doc, "a"));
  _dict_keys(copy, kc, sizeof(kc));
  cyaml_destroy(copy);
  cyaml_destroy(doc);
  REQUIRE_STREQ(ka, "m,c,k");
  REQUIRE_STREQ(kb, "m,c,k");
  REQUIRE_STREQ(kc, "m,c,k");
}

TEST(dict_iter, walks_members_in_order_and_ends_cleanly) {
  cyaml d = cyaml_create_dictionary();
  cyaml_dictionary_set(d, "one", cyaml_create_int(1));
  cyaml_dictionary_set(d, "two", cyaml_create_string("2"));
  cyaml_dictionary_set(d, "three", cyaml_create_list());
  cyaml_dictionary_iter it;
  bool ok1 = cyaml_dictionary_first(d, &it);
  bool k1 = ok1 && strcmp(it.key, "one") == 0 &&
            it.value == cyaml_dictionary_get(d, "one");
  bool ok2 = cyaml_dictionary_next(&it);
  bool k2 =
      ok2 && strcmp(it.key, "two") == 0 && cyaml_type(it.value) == CYAML_STRING;
  bool ok3 = cyaml_dictionary_next(&it);
  bool k3 =
      ok3 && strcmp(it.key, "three") == 0 && cyaml_type(it.value) == CYAML_LIST;
  bool end = !cyaml_dictionary_next(&it) && it.key == NULL && it.value == NULL;
  bool end_again = !cyaml_dictionary_next(&it) && it.key == NULL;
  cyaml_destroy(d);
  REQUIRE_TRUE(k1);
  REQUIRE_TRUE(k2);
  REQUIRE_TRUE(k3);
  REQUIRE_TRUE(end);
  REQUIRE_TRUE(end_again);
}

TEST(dict_iter, empty_null_and_wrong_type_give_false) {
  cyaml_dictionary_iter it = {.key = "stale", .value = (cyaml)&it};
  cyaml d = cyaml_create_dictionary();
  cyaml l = cyaml_create_list();
  bool empty =
      !cyaml_dictionary_first(d, &it) && it.key == NULL && it.value == NULL;
  it.key = "stale";
  bool null_dict = !cyaml_dictionary_first(NULL, &it) && it.key == NULL;
  it.key = "stale";
  bool list =
      !cyaml_dictionary_first(l, &it) && it.key == NULL && it.value == NULL;
  bool null_it =
      !cyaml_dictionary_first(d, NULL) && !cyaml_dictionary_next(NULL);
  cyaml_destroy(d);
  cyaml_destroy(l);
  REQUIRE_TRUE(empty);
  REQUIRE_TRUE(null_dict);
  REQUIRE_TRUE(list);
  REQUIRE_TRUE(null_it);
}

TEST(dict_iter, removing_the_current_member_keeps_the_cursor_valid) {
  /* Removing the member that the cursor stands on is allowed, and the walk
   * continues with the member after it. Under valgrind or AddressSanitizer
   * this test also shows that the cursor never reads the freed member. */
  cyaml d = cyaml_create_dictionary();
  const char *names[] = {"a", "b", "c", "d", "e"};
  for (int i = 0; i < 5; i++)
    cyaml_dictionary_set(d, names[i], cyaml_create_int(i));
  char seen[16] = "";
  size_t n = 0;
  cyaml_dictionary_iter it;
  for (bool more = cyaml_dictionary_first(d, &it); more;
       more = cyaml_dictionary_next(&it)) {
    seen[n++] = it.key[0];
    if (cyaml_int_val(it.value) % 2 == 0) {
      char key[2] = {it.key[0], '\0'};
      cyaml_dictionary_remove(d, key);
    }
  }
  seen[n] = '\0';
  char keys[32];
  _dict_keys(d, keys, sizeof(keys));
  cyaml_destroy(d);
  REQUIRE_STREQ(seen, "abcde");
  REQUIRE_STREQ(keys, "b,d");
}

TEST(dict_iter, survives_replacement_of_current_and_other_values) {
  /* The cursor holds the successor of the current member, so a replacement
   * of the value of any member, the current one included, and the removal
   * of a member other than the successor leave it valid, and the walk goes
   * on in the same order. Under valgrind or AddressSanitizer this test also
   * shows that no step reads a freed entry. */
  cyaml d = cyaml_parse("{a: 1, b: 2, c: 3, d: 4, e: 5}", NULL);
  char order[8] = {0};
  size_t visited = 0;
  long long seen_d = 0;
  ccol_retval_t r1 = ccol_success, r2 = ccol_success, r3 = ccol_success;
  ccol_retval_t r4 = ccol_success;
  bool value_is_same_node_after_set = false;
  cyaml_dictionary_iter it;
  for (bool ok = cyaml_dictionary_first(d, &it); ok;
       ok = cyaml_dictionary_next(&it)) {
    if (visited < sizeof(order) - 1) order[visited] = it.key[0];
    visited++;
    if (strcmp(it.key, "a") == 0) {
      /* The current member, through cyaml_dictionary_set(). */
      r1 = cyaml_dictionary_set(d, "a", cyaml_create_int(10));
    } else if (strcmp(it.key, "b") == 0) {
      /* The current member, through cyaml_set(), which keeps the node. */
      cyaml before = it.value;
      r2 = cyaml_set(d, "b", 20);
      value_is_same_node_after_set = cyaml_dictionary_get(d, "b") == before;
      /* Another member, which the walk has not reached yet. */
      r3 = cyaml_dictionary_set(d, "d", cyaml_create_int(40));
    } else if (strcmp(it.key, "c") == 0) {
      /* A member other than the successor ("d"): the predecessor. */
      r4 = cyaml_dictionary_remove(d, "a");
    } else if (strcmp(it.key, "d") == 0) {
      seen_d = cyaml_int_val(it.value);
    }
  }
  char *out = cyaml_serialize_flow(d);
  bool out_ok = out && strcmp(out, "{b: 20, c: 3, d: 40, e: 5}") == 0;
  cyaml_serialize_free(out);
  cyaml_destroy(d);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_EQ(r4, ccol_success);
  REQUIRE_EQ(visited, (size_t)5);
  REQUIRE_STREQ(order, "abcde");
  REQUIRE_TRUE(value_is_same_node_after_set);
  REQUIRE_EQ(seen_d, 40LL);
  REQUIRE_TRUE(out_ok);
}

TEST(clone, deep_independence) {
  const char *yaml =
      "server:\n"
      "  host: localhost\n"
      "  ports:\n"
      "    - 80\n"
      "    - 443\n";
  char *err = NULL;
  cyaml original = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)original, NULL);

  cyaml copy = cyaml_clone(original);
  REQUIRE_NE((void *)copy, NULL);
  REQUIRE_NE((void *)copy, (void *)original);

  /* Change original; the copy must not change. This test asserts the change
   * itself and its effect on original, not only that the value in copy stays
   * the same, because without those two assertions two broken behaviours
   * would pass: a cyaml_set() that does nothing and reports success, and a
   * cyaml_clone() that is fully shallow and shares its nodes. The test exists
   * to separate those from the correct deep copy, which is independent of
   * the source. */
  REQUIRE_EQ(cyaml_set(original, "server.host", "changed"), ccol_success);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(original, "server.host")), "changed");
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(copy, "server.host")), "localhost");

  cyaml_destroy(original);
  cyaml_destroy(copy);
}

TEST(clone, null_safe) { REQUIRE_EQ((void *)cyaml_clone(NULL), NULL); }

TEST(clone, empty_list) {
  cyaml src = cyaml_create_list();
  REQUIRE_NE((void *)src, NULL);
  cyaml copy = cyaml_clone(src);
  REQUIRE_NE((void *)copy, NULL);
  REQUIRE_NE((void *)copy, (void *)src);
  REQUIRE_EQ(cyaml_type(copy), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(copy), (size_t)0);
  cyaml_destroy(src);
  cyaml_destroy(copy);
}

TEST(clone, empty_dictionary) {
  cyaml src = cyaml_create_dictionary();
  REQUIRE_NE((void *)src, NULL);
  cyaml copy = cyaml_clone(src);
  REQUIRE_NE((void *)copy, NULL);
  REQUIRE_NE((void *)copy, (void *)src);
  REQUIRE_EQ(cyaml_type(copy), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(copy), (size_t)0);
  cyaml_destroy(src);
  cyaml_destroy(copy);
}

TEST(clone, oom_returns_null) {
  /* When the first node allocation inside cyaml_clone fails, the function
   * must return NULL and leak nothing. */
  g_alloc_remaining = -1;
  cyaml src = cyaml_create_string_mp("hello", &g_counting_mp);
  REQUIRE_NE((void *)src, NULL);

  g_alloc_remaining = 0;
  cyaml copy = cyaml_clone(src);
  /* The same comment in path.set_single_component_oom explains why the reset
   * happens before this check and not after it. */
  g_alloc_remaining = -1;
  REQUIRE_EQ((void *)copy, NULL);

  /* The node src must be whole, and destroying it must work. */
  REQUIRE_STREQ(cyaml_str_val(src), "hello");
  cyaml_destroy(src);
}

TEST(clone, dictionary_iterator_oom_never_returns_incomplete_clone) {
  /* chashmap_begin_iter() returns NULL in two different cases: for a map that
   * is truly empty, and for a map that is not empty where the small
   * allocation for one iteration fails because memory ran out. Code that
   * reads both cases as "nothing to clone" lets this function return a clone
   * that is not NULL and looks successful, but silently holds no key at all,
   * which breaks the documented contract of "NULL when an allocation fails".
   *
   * This test fails an allocation at every budget, from 0 to well past the
   * real allocation count of the whole clone, and at each budget it checks
   * one property: a dictionary clone that cyaml_clone returns must never
   * hold fewer entries than the source. */
  char *err = NULL;
  cyaml src = cyaml_parse_mp("a: 1\nb: 2\nc: 3\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)src, NULL);
  size_t src_size = cyaml_dictionary_size(src);
  REQUIRE_EQ(src_size, (size_t)3);

  for (int budget = 0; budget < 200; budget++) {
    g_alloc_remaining = budget;
    cyaml copy = cyaml_clone(src);
    g_alloc_remaining = -1;
    if (copy) {
      /* The cleanup happens before the REQUIRE_* check below, not after it,
       * because this test sweeps every allocation budget: a leak of its own
       * copy on an assertion that already fails looks exactly like a real
       * leak in the code under test, Valgrind cannot tell the two apart, and
       * a run of this test under valgrind then says nothing. */
      size_t copy_size = cyaml_dictionary_size(copy);
      cyaml_destroy(copy);
      REQUIRE_EQ(copy_size, src_size);
    }
  }

  g_alloc_remaining = -1;
  REQUIRE_EQ(cyaml_dictionary_size(src), src_size);
  cyaml_destroy(src);
}

TEST(clone, exceeding_max_depth_returns_null_not_crashed) {
  /* A tree that a caller gives to cyaml_clone() does not have to come from
   * cyaml_parse(). CYAML_MAX_PARSE_DEPTH bounds a parse on its own, at a much
   * lower depth, but a caller can instead build a tree directly with
   * cyaml_create_list() and cyaml_list_push(), and construction puts no bound
   * on the depth at all.
   *
   * cyaml_clone() must therefore bound its own recursion, just as
   * cyaml_serialize() does with CYAML_MAX_SERIALIZE_DEPTH (see
   * serialize.excessive_depth_rejected_not_crashed above). cyaml_clone() must
   * report NULL, which is its documented contract of "NULL when an
   * allocation fails", instead of overflowing the stack and crashing on a
   * tree that is merely deep and holds no cycle.
   *
   * The depth of 1000 is the same depth that
   * serialize.excessive_depth_rejected_not_crashed uses. It sits well past
   * the cap of 500 levels while staying shallow enough for the cleanup of
   * this test: that cleanup is the tree walk of cyaml_destroy, which has no
   * depth guard, and it cannot exhaust the stack at this depth. */
  size_t depth = 1000;
  cyaml root = cyaml_create_null();
  REQUIRE_NE((void *)root, NULL);
  for (size_t i = 0; i < depth; i++) {
    cyaml outer = cyaml_create_list();
    REQUIRE_NE((void *)outer, NULL);
    REQUIRE_EQ(cyaml_list_push(outer, root), ccol_success);
    root = outer;
  }

  cyaml copy = cyaml_clone(root);
  REQUIRE_EQ((void *)copy, NULL);

  cyaml_destroy(root);
}

TEST(clone, deep_but_within_limit_list_still_works) {
  /* A guard for the depth cap above: a tree that sits well inside
   * CYAML_CLONE_MAX_DEPTH (500) must clone correctly and completely, and an
   * off-by-one error in the depth check must not reject it. */
  size_t depth = 100;
  cyaml root = cyaml_create_int(42);
  REQUIRE_NE((void *)root, NULL);
  for (size_t i = 0; i < depth; i++) {
    cyaml outer = cyaml_create_list();
    REQUIRE_NE((void *)outer, NULL);
    REQUIRE_EQ(cyaml_list_push(outer, root), ccol_success);
    root = outer;
  }

  cyaml copy = cyaml_clone(root);
  REQUIRE_NE((void *)copy, NULL);

  cyaml leaf = copy;
  for (size_t i = 0; i < depth; i++) {
    REQUIRE_EQ(cyaml_type(leaf), CYAML_LIST);
    REQUIRE_EQ(cyaml_list_len(leaf), (size_t)1);
    leaf = cyaml_list_get(leaf, 0);
  }
  REQUIRE_EQ(cyaml_type(leaf), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(leaf), 42LL);

  cyaml_destroy(root);
  cyaml_destroy(copy);
}

TEST(oom, parsing_a_dictionary_never_leaks_under_sustained_allocation_failure) {
  /* The parser builds a dictionary one entry at a time, calling
   * cyaml_dictionary_set once for each entry across many turns of a loop in
   * parse_flow_dictionary, parse_block_dictionary, parse_flow_list and
   * merge_one_source_into. A LATER parse failure that has no relation to that
   * dictionary then destroys the whole dictionary, and that destroy must
   * never leak an entry that the parser has already inserted.
   *
   * The dictionary cleanup of __cyaml_destroy is node_clear, which reaches
   * every child with chmap_destroy_with_dtor (chashmap.h). That function
   * walks the internal storage of the map directly, so it needs no
   * allocation, whereas a walk that first lists the map with
   * chashmap_begin_iter() does need one, and that small internal allocation
   * can itself fail when memory stays out.
   *
   * This test fails an allocation at every budget, across six documents that
   * drive every loop in the parser that builds a dictionary one entry at a
   * time. They include a dictionary inside the value of a flow dictionary,
   * which finishes correctly and is torn down later only by a failure
   * further out, and a merge key ("<<") that the parser expands.
   *
   * This test cannot find a leak on its own, because tau has no leak checker.
   * Its job is to drive these exact code paths under `make memtest`, which
   * runs valgrind over this whole suite: Valgrind catches a regression here,
   * and no assertion in this function catches one. */
  const char *docs[] = {
      "{z: 1, a: 2, m: 3}\n",
      "z: 1\na: 2\nm: 3\n",
      "a: &base {x: 1, y: 2}\nb:\n  <<: *base\n  y: 99\n",
      "{a: &base {x: 1, y: 2}, b: {<<: *base, y: 99}}\n",
      "[foo: bar, baz: qux]\n",
      "? a\n: 1\n? b\n: 2\n",
  };
  for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); d++) {
    for (int budget = 0; budget < 250; budget++) {
      g_alloc_remaining = budget;
      char *err = NULL;
      cyaml doc = cyaml_parse_mp(docs[d], &err, &g_counting_mp);
      g_alloc_remaining = -1;
      /* The parse error string is library storage and is never freed; see
         the ownership rule in common.h. */
      if (doc) cyaml_destroy(doc);
    }
  }
}

TEST(
    oom,
    dictionary_entry_key_resolution_never_returns_a_truncated_document_under_sustained_allocation_failure) {
  /* Two branches of try_parse_scalar_dict_key() can fail only because memory
   * ran out, and neither one calls parse_err(). The first takes an alias as
   * a key and calls cyaml_clone() on the aliased node; the second takes an
   * anchored flow collection as a key, where parse_flow_list() or
   * parse_flow_dictionary() fails at its very first allocation.
   *
   * Every caller of this function reads ctx->error to tell "this is not a
   * dictionary entry after all" from "this is a real error" (see the doc
   * comment of the function itself), so a failure of memory that the
   * function does not report must never look like a complete document. The
   * wrong outcome is "stop this dictionary here, and give back what is parsed
   * so far as a complete, successful document".
   *
   * This test fails an allocation at every budget, with one document for
   * each of the two branches, and at each budget it checks one property: a
   * result that is not NULL must always be the complete, correct document,
   * never a truncated one that misses a later entry. */
  const char *docs[] = {
      /* Alias resolved directly as a dictionary key (3rd entry). */
      "x: &k v\na: 1\n*k: 2\n",
      /* An anchored flow list used as a dictionary key (2nd entry). */
      "a: 1\n&x [1, 2]: value\n",
  };
  const size_t expected_sizes[] = {3, 2};

  for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); d++) {
    for (int budget = 0; budget < 300; budget++) {
      g_alloc_remaining = budget;
      char *err = NULL;
      cyaml doc = cyaml_parse_mp(docs[d], &err, &g_counting_mp);
      g_alloc_remaining = -1;
      if (doc) {
        /* The cleanup happens before the REQUIRE_* check below, not after it,
         * because this test sweeps every allocation budget: a leak of its
         * own doc on an assertion that already fails looks exactly like a
         * real leak in the code under test, Valgrind cannot tell the two
         * apart, and a run of this test under valgrind then says nothing.
         * Every other allocator-fault test in this file follows the same
         * rule for its own baseline and reset step; see the anchored-key
         * test above that also runs out of memory. */
        size_t actual_size = cyaml_dictionary_size(doc);
        cyaml_destroy(doc);
        REQUIRE_EQ(actual_size, expected_sizes[d]);
      } else {
        /* The value err can legitimately be NULL as well, because the
         * allocation of the error message string can itself fail under a
         * budget that is tight enough. The parsing_a_dictionary_never_leaks...
         * test above has the same else-branch and checks err in the same
         * way. */
      }
    }
  }
}

TEST(oom, scalar_dictionary_key_oom_reports_specific_message) {
  /* node_to_dict_key_string() has five scalar branches (STRING, INTEGER,
   * FLOAT, NULL and BOOL), and each one must report a message of its own when
   * a bare ccol_strdup runs out of memory, as the LIST and DICTIONARY branch
   * right beside them already does.
   *
   * The "[1: a]" shorthand entry of parse_flow_list also has no fallback
   * message of its own, unlike try_parse_scalar_dict_key, and the conversion
   * of the integer key "1" to a string can run out of memory. Without those
   * five messages, that failure falls all the way through to the generic
   * fallback of parse_common instead of giving a real message.
   *
   * This test uses the single-fault allocator rather than the budget style
   * of g_counting_mp. Under a budget, every call after the one that reaches
   * zero also fails, so a budget-style failure leaves no allocation headroom
   * for the error message that the parser builds; the value err would then
   * always be NULL, and this test would say nothing. A failure of exactly
   * one call lets the rest of the parse, including the build of the error
   * string, run normally.
   *
   * The test sweeps every call index and checks that the parser reaches the
   * specific message at some index. It deliberately asserts nothing about
   * every OTHER index that fails: several unrelated allocation sites
   * elsewhere in the parser have the same kind of gap and fall through to
   * the generic fallback, but those sites are out of scope here, and this
   * test is not responsible for them and must not treat them as a
   * regression. */
  const char *doc_src = "[1: a]\n";
  bool saw_specific_oom_message = false;
  for (int fail_at = 0; fail_at < 60; fail_at++) {
    g_single_call_idx = 0;
    g_single_fail_at = fail_at;
    char *err = NULL;
    cyaml doc = cyaml_parse_mp(doc_src, &err, &g_single_fault_mp);
    g_single_fail_at = -1;
    if (doc) {
      cyaml_destroy(doc);
    } else if (err) {
      if (strstr(err, "out of memory converting a scalar key") != NULL)
        saw_specific_oom_message = true;
    }
  }
  REQUIRE_TRUE(saw_specific_oom_message);
}

TEST(oom, anchor_or_tag_decorated_key_oom_never_silently_succeeds) {
  /* The doc comment of try_parse_scalar_dict_key() promises that a return of
   * "this is not a key after all" always leaves ctx->pos back at the exact
   * place it held on entry. A caller reads ctx->error[0] to tell that case
   * from a real error, and that is its only signal.
   *
   * try_parse_scalar_dict_key calls parse_anchor_name() and parse_tag_token()
   * speculatively while it scans a possible "&anchor key:" or "!!tag key:"
   * prefix, so those two functions must not return false on their own
   * failure of _ccol_mem_alloc without calling parse_err(). Such a return
   * breaks the contract: the caller sees ctx->error[0] == 0 and reads it as
   * "not a key, and the position is already restored", while the position is
   * NOT restored, because the scan has already moved it past the anchor text
   * or the tag text. The parse is then corrupt instead of failing.
   *
   * anchors_store() carries the same requirement. The parser calls it once
   * it confirms the key, to register the anchor of the key for a later
   * '*name', and it must report a failure of memory to its two callers
   * (register_key_anchor() and the ordinary branch for a whole-node anchor)
   * instead of silently discarding its clone and letting both callers report
   * success.
   *
   * This document drives the path for a key that carries an anchor: the '&'
   * property-scanning branch of try_parse_scalar_dict_key, followed by
   * register_key_anchor() and anchors_store(). Every allocation of this
   * document is one call to _ccol_mem_alloc, node_alloc or chmap_insert_elem,
   * and nothing retries it; direct inspection confirms this, since the
   * dictionary of this input has one entry, so no retry path of a chmap
   * iterator ever runs.
   *
   * A real allocation failure at any point of this parse must therefore
   * always make the whole document fail: for this call count there is no
   * legitimate outcome where the parse "should still succeed", even though
   * some other allocation sites of this parser can legitimately recover from
   * one transient failure, with the retry that chmap_begin_iter_safe
   * holds. */
  const char *doc_src = "&a key: value\n";

  g_single_call_idx = 0;
  g_single_fail_at = -1;
  char *baseline_err = NULL;
  cyaml baseline = cyaml_parse_mp(doc_src, &baseline_err, &g_single_fault_mp);
  int total_calls = g_single_call_idx;
  /* The cleanup happens before the REQUIRE_* checks below, not after them. In
   * an ordinary test, a leak on an assertion that already fails is the
   * accepted, uniform convention of this file, but this test sweeps every
   * allocation budget, so a leak of its own baseline looks exactly like a
   * real leak in the code under test; Valgrind cannot tell the two apart, and
   * a run of this test under valgrind then says nothing. Every other
   * allocator-fault test in this file follows the same rule for its own
   * baseline and reset step. */
  bool baseline_had_no_error = (baseline_err == NULL);
  bool baseline_succeeded = (baseline != NULL);
  if (baseline) cyaml_destroy(baseline);
  if (baseline_err) free(baseline_err);
  REQUIRE_TRUE(baseline_had_no_error);
  REQUIRE_TRUE(baseline_succeeded);
  REQUIRE_GT(total_calls, 0);

  for (int fail_at = 0; fail_at < total_calls; fail_at++) {
    g_single_call_idx = 0;
    g_single_fail_at = fail_at;
    char *err = NULL;
    cyaml doc = cyaml_parse_mp(doc_src, &err, &g_single_fault_mp);
    g_single_fail_at = -1;
    /* This check guards against a doc that is not NULL although one
     * allocation was made to fail: a correct parser must report a failure
     * here, every time, for this document. The code frees doc before the
     * REQUIRE_EQ, for the reason above, because a failed REQUIRE_EQ returns
     * past this cleanup. */
    bool doc_was_null = (doc == NULL);
    if (doc) cyaml_destroy(doc);
    REQUIRE_TRUE(doc_was_null);
  }
}

TEST(oom, unreported_allocation_failure_reports_honest_fallback_not_vague_one) {
  /* parse_common holds a top-level fallback message, which the parser reaches
   * whenever a document fails to parse while ctx->error is empty. That
   * message must not be the unhelpful "unknown parse error", a string that
   * tells a caller nothing about a real failure of memory.
   *
   * Every real rejection of syntax in this parser calls parse_err() with a
   * specific message before it reports the failure, which is the established
   * convention of this file. So the only thing that can reach the fallback
   * with ctx->error empty is an allocation that failed deep in the call chain
   * with no message of its own: many low-level helpers that build the DOM are
   * shared with the public API outside the parser, and they hold no
   * parse_ctx_t to report through.
   *
   * A sweep of the single-fault allocator across many document shapes
   * confirms this. The shapes cover block and flow mappings, block and flow
   * sequences, scalars of every type, anchors, aliases, tags, merge keys,
   * directives, block scalars and deeply nested structures.
   * With this guard in place, the literal string "unknown parse error" is
   * unreachable in every one of them.
   *
   * This test drives two of those shapes directly, a plain block mapping and
   * a flow sequence; the comment of the guard at its own call site gives the
   * wider reasoning. */
  const char *docs[] = {"a: 1\nb: 2\n", "[1, 2, 3]\n"};
  bool saw_specific_oom_message = false;
  for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); d++) {
    for (int fail_at = 0; fail_at < 40; fail_at++) {
      g_single_call_idx = 0;
      g_single_fail_at = fail_at;
      char *err = NULL;
      cyaml doc = cyaml_parse_mp(docs[d], &err, &g_single_fault_mp);
      g_single_fail_at = -1;
      if (doc) {
        cyaml_destroy(doc);
      } else if (err) {
        /* The cleanup happens before the REQUIRE_* check below, not after it,
         * because this test sweeps every allocation budget: a leak of its
         * own err string on an assertion that already fails looks exactly
         * like a real leak in the code under test, Valgrind cannot tell the
         * two apart, and a run of this test under valgrind then says
         * nothing. */
        bool is_specific = strcmp(err, "unknown parse error") != 0;
        if (strstr(err, "out of memory") != NULL)
          saw_specific_oom_message = true;
        REQUIRE_TRUE(is_specific);
      }
    }
  }
  REQUIRE_TRUE(saw_specific_oom_message);
}

TEST(
    oom,
    plain_scalar_multiline_continuation_never_crashes_under_allocation_failure) {
  /* parse_plain_scalar_multiline holds a loop over the lines that continue a
   * scalar, and that loop scans the content of each such line straight into
   * the shared accumulator buffer b. As the doc comment of that function
   * explains on line_floor, it appends into b directly and trims trailing
   * whitespace back only to a recorded floor offset, so it needs no separate
   * buffer for each line.
   *
   * The allocator can fail while the scan grows b, and the parser must
   * report that as a clean parse failure instead of crashing. This test
   * guards against that whole class of hazard: an allocation failure during
   * the scan of a continuation line must not corrupt the parse or crash it.
   * One such crash is a strlen(NULL) on a scratch buffer whose own
   * allocation failed.
   *
   * The test fails an allocation at every budget, across several plain
   * scalars that continue over more than one line, some at the root of the
   * document and some inside a flow collection.
   *
   * No assertion in this test can find a crash. Its job is to drive this
   * exact path under `make memtest`, which runs valgrind, and under a plain
   * run as well; either one aborts the whole suite on the defect that this
   * test guards against. */
  const char *docs[] = {
      "key: first line\n  second line\n",
      "key: first line\n  second line\n  third line\n",
      "[first line\n  second line]\n",
      "{key: first line\n  second line}\n",
  };
  for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); d++) {
    for (int budget = 0; budget < 60; budget++) {
      g_alloc_remaining = budget;
      char *err = NULL;
      cyaml doc = cyaml_parse_mp(docs[d], &err, &g_counting_mp);
      g_alloc_remaining = -1;
      /* The parse error string is library storage and is never freed; see
         the ownership rule in common.h. */
      if (doc) cyaml_destroy(doc);
    }
  }
}

/* ========================================================================== */
/*                         SERIALIZATION                                      */
/* ========================================================================== */

/* A string whose spelling a YAML 1.1 reader resolves as a boolean must go
   out QUOTED. This module parses it as a string, as YAML 1.2 requires, but
   PyYAML in its default mode, Ruby's Psych and Go's yaml.v2 read a plain
   unquoted "yes" in the output as the boolean true, so a document that
   leaves this serializer and enters one of those tools would silently change
   meaning. This is the "Norway problem", where the country code NO becomes
   false. */
/* The parse error message is LIBRARY storage. One rule covers the err and
   err_str out-parameter of every module here: the library owns the string
   and the caller never frees it (see the note at the top of common.h). A
   message on the heap would give the same-looking parameter a second,
   opposite ownership rule, with nothing in the type or at the call site to
   tell the two apart, and it would allocate on a path that a failed
   allocation can reach. */
TEST(parse_error_string, is_library_storage_and_survives_without_a_free) {
  char *err = NULL;
  cyaml bad = cyaml_parse("a: [1, 2", &err);
  REQUIRE_EQ((void *)bad, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);
  /* Nothing is freed here and nothing leaks; the text stays readable after
     the call that produced it has returned. */
  REQUIRE_TRUE(strlen(err) > 0);
}

TEST(parse_error_string, a_successful_parse_clears_it) {
  char *err = (char *)0x1;
  cyaml ok = cyaml_parse("a: 1\n", &err);
  REQUIRE_NE((void *)ok, NULL);
  REQUIRE_EQ((void *)err, NULL);
  cyaml_destroy(ok);
}

TEST(parse_error_string, the_next_failing_parse_replaces_the_text) {
  /* The documented lifetime, which is also the one that strerror(3) gives:
     the second failure reuses the same storage, so the pointer is the same
     and the text is the new one. */
  char *first = NULL;
  cyaml a = cyaml_parse("a: [1, 2", &first);
  REQUIRE_EQ((void *)a, NULL);
  REQUIRE_NE((void *)first, NULL);
  char kept[512];
  snprintf(kept, sizeof(kept), "%s", first);

  char *second = NULL;
  cyaml b = cyaml_parse("\t- bad tab indent\n", &second);
  REQUIRE_EQ((void *)b, NULL);
  REQUIRE_NE((void *)second, NULL);
  REQUIRE_EQ((void *)second, (void *)first);
  REQUIRE_TRUE(strcmp(second, kept) != 0);
}

TEST(parse_error_string, a_custom_allocator_never_sees_it) {
  /* The message costs the caller's allocator nothing. The same failing parse
     runs twice, once asking for the message and once discarding it, and a
     message on the heap would make the first run cost exactly one allocation
     more than the second. The comparison needs no knowledge of how many
     allocations the parse itself makes, so it does not go stale when that
     number changes. */
  const char *doc = "\t- bad tab indent\n";

  g_tally = 0;
  char *err = NULL;
  cyaml with_msg = cyaml_parse_mp(doc, &err, &g_tally_mp);
  long allocs_with_msg = g_tally;
  REQUIRE_EQ((void *)with_msg, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);

  g_tally = 0;
  cyaml no_msg = cyaml_parse_mp(doc, NULL, &g_tally_mp);
  long allocs_no_msg = g_tally;
  REQUIRE_EQ((void *)no_msg, NULL);

  REQUIRE_EQ(allocs_with_msg, allocs_no_msg);
}

TEST(serialize, yaml_1_1_boolean_spellings_are_quoted) {
  static const char *const words[] = {"y",  "Y",   "yes", "Yes", "YES", "n",
                                      "N",  "no",  "No",  "NO",  "on",  "On",
                                      "ON", "off", "Off", "OFF"};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    cyaml n = cyaml_create_string(words[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char expected[32];
    snprintf(expected, sizeof(expected), "\"%s\"\n", words[i]);
    REQUIRE_STREQ(s, expected);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
  }
}

TEST(serialize, yaml_1_1_boolean_spellings_round_trip_as_strings) {
  /* The quoting must not change what this module reads back: each one is the
     same string after a parse of the serialized form. */
  static const char *const words[] = {"yes", "no", "on", "off", "y", "N"};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    cyaml n = cyaml_create_string(words[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char *err = NULL;
    cyaml back = cyaml_parse(s, &err);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ((int)cyaml_type(back), (int)CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(back), words[i]);
    cyaml_destroy(back);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
  }
}

TEST(serialize, a_word_that_only_starts_like_a_keyword_stays_plain) {
  /* The switch on the first byte must not quote more than the list names:
     "yesterday" and "november" start with a candidate byte but are ordinary
     plain scalars. */
  static const char *const words[] = {"yesterday", "november", "online",
                                      "office",    "typical",  "format"};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    cyaml n = cyaml_create_string(words[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char expected[32];
    snprintf(expected, sizeof(expected), "%s\n", words[i]);
    REQUIRE_STREQ(s, expected);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
  }
}

TEST(serialize, null) {
  cyaml n = cyaml_create_null();
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "~\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, null_handle_block) {
  /* cyaml_serialize(NULL) must give "~\n", not an empty string. */
  char *s = cyaml_serialize(NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "~\n");
  cyaml_serialize_free(s);
}

TEST(serialize, null_handle_flow) {
  /* cyaml_serialize_flow(NULL) must give "~". */
  char *s = cyaml_serialize_flow(NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "~");
  cyaml_serialize_free(s);
}

TEST(serialize, bool_true) {
  cyaml n = cyaml_create_bool(true);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "true\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, integer) {
  cyaml n = cyaml_create_int(42);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "42\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, float_inf) {
  cyaml n = cyaml_create_double(__builtin_inf());
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, ".inf\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, float_nan) {
  cyaml n = cyaml_create_double(__builtin_nan(""));
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, ".nan\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, float_whole_number_gets_dot_zero_suffix) {
  /* yb_append_double formats a double with "%.15g", which for a double that
   * holds a whole number, for example 5.0, gives "5": text with no '.', no
   * 'e' and no 'E', which parses as a CYAML_INTEGER instead of the original
   * CYAML_FLOAT. A branch of its own appends ".0" so that the parse gives a
   * float again, and this test pins the round trip for that branch. */
  cyaml n = cyaml_create_double(5.0);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "5.0\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);

  char *err = NULL;
  cyaml reparsed = cyaml_parse("5.0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(reparsed), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(reparsed), 5.0);
  cyaml_destroy(reparsed);
}

TEST(serialize, plain_string) {
  cyaml n = cyaml_create_string("hello");
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "hello\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, quoted_string_null_like) {
  cyaml n = cyaml_create_string("null");
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  /* The text "null" needs quotes, so that a parse of it does not give
     CYAML_NULL. */
  REQUIRE_NE(strstr(s, "null"), NULL);
  REQUIRE_NE(s[0], 'n'); /* the output has quotes, and is not a plain 'null' */
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, quoted_string_null_variants_round_trip) {
  /* The serializer must put quotes around "Null" and "NULL", so that a parse
   * of the output gives CYAML_STRING instead of CYAML_NULL. */
  const char *cases[] = {"Null", "NULL", NULL};
  for (int i = 0; cases[i]; i++) {
    cyaml n = cyaml_create_string(cases[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char *err = NULL;
    cyaml back = cyaml_parse(s, &err);
    REQUIRE_EQ((void *)err, NULL);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(back), cases[i]);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
    cyaml_destroy(back);
  }
}

TEST(serialize, quoted_string_bool_variants_round_trip) {
  /* The serializer must put quotes around "True", "TRUE", "False" and
   * "FALSE", so that a parse of the output gives CYAML_STRING instead of
   * CYAML_BOOL. */
  const char *cases[] = {"True", "TRUE", "False", "FALSE", NULL};
  for (int i = 0; cases[i]; i++) {
    cyaml n = cyaml_create_string(cases[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char *err = NULL;
    cyaml back = cyaml_parse(s, &err);
    REQUIRE_EQ((void *)err, NULL);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(back), cases[i]);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
    cyaml_destroy(back);
  }
}

TEST(serialize, empty_string_round_trip) {
  /* The serializer must write an empty CYAML_STRING as two quote characters,
   * and a parse of that output must give a CYAML_STRING whose value is
   * empty. */
  cyaml n = cyaml_create_string("");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, mapping_round_trip) {
  const char *yaml = "host: localhost\nport: 8080\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  char *out = cyaml_serialize(doc);
  REQUIRE_NE((void *)out, NULL);
  /* Parse the serialized form again, and check that the values survive. */
  cyaml doc2 = cyaml_parse(out, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc2, "host")), "localhost");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc2, "port")), 8080LL);
  cyaml_serialize_free(out);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(serialize, sequence_round_trip) {
  const char *yaml = "- 1\n- 2\n- 3\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  char *out = cyaml_serialize(doc);
  REQUIRE_NE((void *)out, NULL);
  cyaml doc2 = cyaml_parse(out, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_list_len(doc2), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc2, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc2, 1)), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc2, 2)), 3LL);
  cyaml_serialize_free(out);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(serialize, flow_style) {
  cyaml seq = cyaml_create_list();
  cyaml_list_push(seq, cyaml_create_int(1));
  cyaml_list_push(seq, cyaml_create_string("two"));
  cyaml_list_push(seq, cyaml_create_bool(true));
  char *s = cyaml_serialize_flow(seq);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "[1, two, true]");
  cyaml_serialize_free(s);
  cyaml_destroy(seq);
}

TEST(serialize, flow_mapping) {
  cyaml m = cyaml_create_dictionary();
  cyaml_dictionary_set(m, "a", cyaml_create_int(1));
  char *s = cyaml_serialize_flow(m);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "{a: 1}");
  cyaml_serialize_free(s);
  cyaml_destroy(m);
}

TEST(serialize, multi_key_flow_mapping_preserves_iteration_order) {
  /* The internal canonical mode sorts the keys of a dictionary that is itself
   * used as a key. See
   * flow_collections.non_scalar_multi_key_dictionary_key_is_canonically_
   * sorted. That sort must not reach the output order of the public
   * serializers, which write the members in insertion order and must not
   * sort them by key. The keys go in out of alphabetical order, so a sort, a
   * reversal and the insertion order all differ. */
  cyaml m = cyaml_create_dictionary();
  cyaml_dictionary_set(m, "c", cyaml_create_int(3));
  cyaml_dictionary_set(m, "a", cyaml_create_int(1));
  cyaml_dictionary_set(m, "b", cyaml_create_int(2));
  char *s = cyaml_serialize_flow(m);
  char *block = cyaml_serialize(m);
  bool flow_ok = s && strcmp(s, "{c: 3, a: 1, b: 2}") == 0;
  bool block_ok = block && strcmp(block, "c: 3\na: 1\nb: 2\n") == 0;
  if (s) cyaml_serialize_free(s);
  if (block) cyaml_serialize_free(block);
  cyaml_destroy(m);
  REQUIRE_TRUE(flow_ok);
  REQUIRE_TRUE(block_ok);
}

TEST(serialize, empty_sequence_nested_in_mapping_round_trip) {
  /* The serializer must write a dictionary value that is an empty list with
   * the correct indentation, so that a parse of that output is correct. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "key", cyaml_create_list()),
             ccol_success);

  char *s = cyaml_serialize(doc);
  REQUIRE_NE((void *)s, NULL);

  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);

  cyaml val = cyaml_dictionary_get(doc2, "key");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_type(val), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(val), (size_t)0);

  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(serialize, empty_mapping_nested_in_mapping_round_trip) {
  /* The serializer must write a dictionary value that is an empty dictionary
   * with the correct indentation, so that a parse of that output is
   * correct. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "sub", cyaml_create_dictionary()),
             ccol_success);

  char *s = cyaml_serialize(doc);
  REQUIRE_NE((void *)s, NULL);

  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);

  cyaml val = cyaml_dictionary_get(doc2, "sub");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_type(val), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(val), (size_t)0);

  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(serialize, empty_sequence_as_sequence_element_round_trip) {
  /* An empty list can sit as an element inside an outer list; the serializer
   * must write it with the correct indentation, so that a parse of that
   * output works. */
  cyaml outer = cyaml_create_list();
  REQUIRE_NE((void *)outer, NULL);
  REQUIRE_EQ(cyaml_list_push(outer, cyaml_create_int(1)), ccol_success);
  REQUIRE_EQ(cyaml_list_push(outer, cyaml_create_list()), ccol_success);
  REQUIRE_EQ(cyaml_list_push(outer, cyaml_create_int(2)), ccol_success);

  char *s = cyaml_serialize(outer);
  REQUIRE_NE((void *)s, NULL);

  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc2), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc2, 0)), 1LL);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc2, 1)), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(cyaml_list_get(doc2, 1)), (size_t)0);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc2, 2)), 2LL);

  cyaml_serialize_free(s);
  cyaml_destroy(outer);
  cyaml_destroy(doc2);
}

TEST(
    serialize,
    dictionary_never_serializes_incomplete_under_sustained_allocation_failure) {
  /* The CYAML_DICTIONARY case of serialize_block() and of serialize_flow()
   * walks the dictionary with chmap_begin_iter_safe(), which can
   * legitimately return NULL for a map that is NOT empty when memory stays
   * out (see the doc comment of the helper itself).
   *
   * Code that reads that NULL as "nothing left to walk" lets
   * cyaml_serialize() and cyaml_serialize_flow() return a string that is not
   * NULL and looks successful but misses one or more entries of the
   * dictionary, which breaks the documented contract of "NULL when memory
   * runs out".
   *
   * This test fails an allocation at every budget, and at each budget it
   * checks one property: a result that is not NULL must always hold every
   * original key. */
  char *err = NULL;
  cyaml src =
      cyaml_parse_mp("a: 1\nb: 2\nc: 3\nd: 4\ne: 5\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)src, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(src), (size_t)5);

  /* Every REQUIRE_* below sits inside an "if (block)" or an "if (flow)"
   * guard, and most budgets of this sweep are deliberately too small to
   * succeed at all, so this test also confirms that at least one budget DID
   * succeed. Without that confirmation, a regression could make
   * cyaml_serialize() and cyaml_serialize_flow() return NULL for this tree at
   * every budget, and this test, checking only that nothing crashes, would
   * pass just as it does for the correct behaviour that it exists to pin. */
  bool saw_block_success = false;
  bool saw_flow_success = false;

  for (int budget = 0; budget < 300; budget++) {
    g_alloc_remaining = budget;
    char *block = cyaml_serialize(src);
    g_alloc_remaining = -1;
    if (block) {
      saw_block_success = true;
      /* The cleanup happens before the REQUIRE_* checks below, not after them,
       * because this test sweeps every allocation budget: a leak of its own
       * buffer on an assertion that already fails looks exactly like a real
       * leak in the code under test, Valgrind cannot tell the two apart, and
       * a run of this test under valgrind then says nothing. */
      bool has_a = strstr(block, "a:") != NULL;
      bool has_b = strstr(block, "b:") != NULL;
      bool has_c = strstr(block, "c:") != NULL;
      bool has_d = strstr(block, "d:") != NULL;
      bool has_e = strstr(block, "e:") != NULL;
      cyaml_serialize_free_mp(block, &g_counting_mp);
      REQUIRE_TRUE(has_a);
      REQUIRE_TRUE(has_b);
      REQUIRE_TRUE(has_c);
      REQUIRE_TRUE(has_d);
      REQUIRE_TRUE(has_e);
    }

    g_alloc_remaining = budget;
    char *flow = cyaml_serialize_flow(src);
    g_alloc_remaining = -1;
    if (flow) {
      saw_flow_success = true;
      /* The same comment on the block branch above explains why. */
      bool has_a = strstr(flow, "a:") != NULL;
      bool has_b = strstr(flow, "b:") != NULL;
      bool has_c = strstr(flow, "c:") != NULL;
      bool has_d = strstr(flow, "d:") != NULL;
      bool has_e = strstr(flow, "e:") != NULL;
      cyaml_serialize_free_mp(flow, &g_counting_mp);
      REQUIRE_TRUE(has_a);
      REQUIRE_TRUE(has_b);
      REQUIRE_TRUE(has_c);
      REQUIRE_TRUE(has_d);
      REQUIRE_TRUE(has_e);
    }
  }

  REQUIRE_TRUE(saw_block_success);
  REQUIRE_TRUE(saw_flow_success);

  g_alloc_remaining = -1;
  cyaml_destroy(src);
}

/* ========================================================================== */
/*                         SCOPED LIFECYCLE                                   */
/* ========================================================================== */

TEST(lifecycle, scoped_destroy) {
  cyaml out = NULL;
  {
    cyaml_declare_scoped(doc);
    doc = cyaml_parse("key: 42\n", NULL);
    REQUIRE_NE((void *)doc, NULL);
    out = cyaml_clone(doc);
    /* The scope ends here, and the macro destroys doc by itself. */
  }
  /* The node out must be valid, because it is a clone. */
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(out, "key")), 42LL);
  cyaml_destroy(out);
}

TEST(lifecycle, declare_macro) {
  /* cyaml_declare is a plain declaration of a typed variable, so it must work
   * as a normal lvalue that an assignment, a call to cyaml_get and a call to
   * cyaml_destroy all accept. */
  cyaml_declare(n);
  n = cyaml_create_int(42);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 42LL);
  cyaml_destroy(n);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(lifecycle, list_push_oom) {
  /* Fill a list that g_counting_mp backs up to the first cvector capacity,
   * which is 4, so that the next push needs a realloc. Then block every
   * allocation of g_counting_mp and check that cyaml_list_push destroys the
   * victim child and returns a failure code instead of leaking the child. */
  g_alloc_remaining = -1;
  cyaml list = cyaml_create_list_mp(&g_counting_mp);
  REQUIRE_NE((void *)list, NULL);

  for (int i = 0; i < 4; i++) {
    cyaml elem = cyaml_create_int(i);
    REQUIRE_NE((void *)elem, NULL);
    REQUIRE_EQ(cyaml_list_push(list, elem), ccol_success);
  }

  g_alloc_remaining = 0;
  cyaml victim = cyaml_create_int(99);
  REQUIRE_NE((void *)victim, NULL);
  ccol_retval_t r = cyaml_list_push(list, victim);
  /* Reset the budget before checking the result. The same comment in
   * path.set_single_component_oom above explains why the reset must not
   * happen after a REQUIRE_* check, which can return from this function
   * early. */
  g_alloc_remaining = -1;
  /* list_push owns victim and destroys it, whatever the outcome is. */
  REQUIRE_NE(r, ccol_success);

  REQUIRE_EQ(cyaml_list_len(list), (size_t)4);
  cyaml_destroy(list);
}

TEST(lifecycle, dictionary_set_oom) {
  /* The insert into the chmap behind the dictionary fails when g_counting_mp
   * runs out, and cyaml_dictionary_set must then destroy the child and return
   * a failure code. */
  g_alloc_remaining = -1;
  cyaml doc = cyaml_create_dictionary_mp(&g_counting_mp);
  REQUIRE_NE((void *)doc, NULL);

  g_alloc_remaining = 0;
  cyaml val = cyaml_create_int(42);
  REQUIRE_NE((void *)val, NULL);
  ccol_retval_t r = cyaml_dictionary_set(doc, "key", val);
  /* Reset the budget before checking the result. The same comment in
   * path.set_single_component_oom above explains why the reset must not
   * happen after a REQUIRE_* check, which can return from this function
   * early. */
  g_alloc_remaining = -1;
  /* dictionary_set owns val and destroys it, whatever the outcome is. */
  REQUIRE_NE(r, ccol_success);

  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)0);
  cyaml_destroy(doc);
}

TEST(lifecycle, list_push_refuses_an_already_attached_child) {
  /* The call rejects a node that already has a parent and leaves that node
   * completely alone instead of giving it a second owner. A call that
   * accepts such a node gives it two parents, and both parents free it when
   * they are destroyed.
   *
   * This test is non-vacuous: without the guard, the push succeeds, and
   * destroying the two lists is then a double free, which AddressSanitizer
   * reports as a heap-use-after-free and which an ordinary build turns into a
   * crash or silent heap corruption. */
  cyaml first = cyaml_create_list();
  cyaml second = cyaml_create_list();
  cyaml child = cyaml_create_string("v");
  REQUIRE_NE((void *)first, NULL);
  REQUIRE_NE((void *)second, NULL);
  REQUIRE_NE((void *)child, NULL);
  REQUIRE_EQ(cyaml_list_push(first, child), ccol_success);

  ccol_retval_t r = cyaml_list_push(second, child);
  size_t first_size = cyaml_list_len(first);
  size_t second_size = cyaml_list_len(second);

  cyaml_destroy(second);
  cyaml_destroy(first);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(first_size, (size_t)1); /* the first parent still owns it */
  REQUIRE_EQ(second_size, (size_t)0);
}

TEST(lifecycle, dictionary_set_refuses_an_already_attached_child) {
  /* The same rule on the dictionary side, reached the way a real caller
   * reaches it: cyaml_dictionary_get gives back a borrowed reference, and
   * such a reference always names a node that already has a parent. */
  cyaml owner = cyaml_create_dictionary();
  cyaml other = cyaml_create_dictionary();
  cyaml val = cyaml_create_int(42);
  REQUIRE_NE((void *)owner, NULL);
  REQUIRE_NE((void *)other, NULL);
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(owner, "k", val), ccol_success);

  cyaml borrowed = cyaml_dictionary_get(owner, "k");
  ccol_retval_t r = cyaml_dictionary_set(other, "copy", borrowed);
  size_t other_size = cyaml_dictionary_size(other);
  /* The original mapping stays untouched, and reading it works. */
  cyaml still_there = cyaml_dictionary_get(owner, "k");
  bool original_intact = (still_there == borrowed);

  cyaml_destroy(other);
  cyaml_destroy(owner);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(other_size, (size_t)0);
  REQUIRE_TRUE(original_intact);
}

TEST(lifecycle, dictionary_set_accepts_a_key_set_to_its_own_current_value) {
  /* There is one case where the call accepts a child that is already
   * attached: the caller gives back the borrowed reference that this slot
   * already holds, so the call has nothing to do. The rejection above must
   * not catch this case, and the call must not free the node that the
   * dictionary points at. */
  cyaml doc = cyaml_create_dictionary();
  cyaml val = cyaml_create_int(7);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "k", val), ccol_success);

  cyaml borrowed = cyaml_dictionary_get(doc, "k");
  ccol_retval_t r = cyaml_dictionary_set(doc, "k", borrowed);
  cyaml after = cyaml_dictionary_get(doc, "k");
  bool same_node = (after == borrowed);
  long long v = after ? cyaml_int_val(after) : -1;

  cyaml_destroy(doc);

  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(same_node);
  REQUIRE_EQ(v, 7LL);
}

TEST(lifecycle, a_container_refuses_to_be_attached_to_itself) {
  /* A self attach makes the container its own child, so destroying that
   * container walks into itself. The root of a tree is not attached, so the
   * attached flag alone does not cover this case, but the check on identity
   * does. */
  cyaml seq = cyaml_create_list();
  cyaml map = cyaml_create_dictionary();
  REQUIRE_NE((void *)seq, NULL);
  REQUIRE_NE((void *)map, NULL);

  ccol_retval_t rs = cyaml_list_push(seq, seq);
  ccol_retval_t rm = cyaml_dictionary_set(map, "self", map);
  size_t seq_size = cyaml_list_len(seq);
  size_t map_size = cyaml_dictionary_size(map);

  cyaml_destroy(seq);
  cyaml_destroy(map);

  REQUIRE_EQ(rs, ccol_invalid_args);
  REQUIRE_EQ(rm, ccol_invalid_args);
  REQUIRE_EQ(seq_size, (size_t)0);
  REQUIRE_EQ(map_size, (size_t)0);
}

TEST(lifecycle, a_rejected_attach_into_a_bad_container_spares_an_owned_child) {
  /* The call always takes ownership of a child that the caller owns, which is
   * why a container of the wrong type, or a NULL container, destroys that
   * child. This rule must not reach a child that somebody else owns, because
   * freeing such a child tears a live node out of another tree. */
  cyaml owner = cyaml_create_list();
  cyaml scalar = cyaml_create_string("not a container");
  cyaml child = cyaml_create_string("v");
  REQUIRE_NE((void *)owner, NULL);
  REQUIRE_NE((void *)scalar, NULL);
  REQUIRE_NE((void *)child, NULL);
  REQUIRE_EQ(cyaml_list_push(owner, child), ccol_success);

  /* The node scalar is not a list, so the call rejects it, while the node
   * child belongs to owner and has to survive this call. */
  ccol_retval_t r = cyaml_list_push(scalar, child);
  ccol_retval_t r2 = cyaml_list_push(NULL, child);
  size_t owner_size = cyaml_list_len(owner);
  cyaml still_there = cyaml_list_get(owner, 0);
  bool child_survived = (still_there == child);

  cyaml_destroy(scalar);
  cyaml_destroy(owner);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(r2, ccol_invalid_args);
  REQUIRE_EQ(owner_size, (size_t)1);
  REQUIRE_TRUE(child_survived);
}

TEST(lifecycle, remove_null_args) {
  /* A remove function must return ccol_invalid_args, and not crash, for a
   * NULL map, a NULL seq and a NULL key. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_remove(NULL, "key"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_dictionary_remove(doc, NULL), ccol_invalid_args);
  cyaml_destroy(doc);

  REQUIRE_EQ(cyaml_list_remove(NULL, 0), ccol_invalid_args);
}

/* ========================================================================== */
/*                         COMPLEX / REAL-WORLD SCENARIOS                    */
/* ========================================================================== */

TEST(real_world, docker_compose_like) {
  const char *yaml =
      "version: \"3.8\"\n"
      "services:\n"
      "  web:\n"
      "    image: nginx:latest\n"
      "    ports:\n"
      "      - 80\n"
      "      - 443\n"
      "    environment:\n"
      "      DEBUG: false\n"
      "      LOG_LEVEL: info\n"
      "  db:\n"
      "    image: postgres:14\n"
      "    ports:\n"
      "      - 5432\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);

  cyaml version = cyaml_get(doc, "version");
  REQUIRE_NE((void *)version, NULL);
  REQUIRE_STREQ(cyaml_str_val(version), "3.8");

  cyaml web = cyaml_get(doc, "services.web");
  REQUIRE_NE((void *)web, NULL);
  REQUIRE_EQ(cyaml_type(web), CYAML_DICTIONARY);
  cyaml web_image = cyaml_get(doc, "services.web.image");
  REQUIRE_NE((void *)web_image, NULL);
  REQUIRE_STREQ(cyaml_str_val(web_image), "nginx:latest");

  cyaml ports = cyaml_get(doc, "services.web.ports");
  REQUIRE_NE((void *)ports, NULL);
  REQUIRE_EQ(cyaml_type(ports), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(ports), (size_t)2);
  cyaml port0 = cyaml_list_get(ports, 0);
  REQUIRE_NE((void *)port0, NULL);
  REQUIRE_EQ(cyaml_int_val(port0), 80LL);
  cyaml port1 = cyaml_list_get(ports, 1);
  REQUIRE_NE((void *)port1, NULL);
  REQUIRE_EQ(cyaml_int_val(port1), 443LL);

  cyaml debug = cyaml_get(doc, "services.web.environment.DEBUG");
  REQUIRE_NE((void *)debug, NULL);
  REQUIRE_EQ(cyaml_type(debug), CYAML_BOOL);
  REQUIRE_FALSE(cyaml_bool_val(debug));

  cyaml db_port0 = cyaml_get(doc, "services.db.ports.#0");
  REQUIRE_NE((void *)db_port0, NULL);
  REQUIRE_EQ(cyaml_int_val(db_port0), 5432LL);

  cyaml_destroy(doc);
}

TEST(real_world, github_actions_like) {
  const char *yaml =
      "name: CI\n"
      "on:\n"
      "  push:\n"
      "    branches:\n"
      "      - main\n"
      "      - develop\n"
      "jobs:\n"
      "  build:\n"
      "    runs-on: ubuntu-latest\n"
      "    steps:\n"
      "      - name: Checkout\n"
      "        uses: actions/checkout@v3\n"
      "      - name: Build\n"
      "        run: make\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "name")), "CI");
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "jobs.build.runs-on")),
                "ubuntu-latest");

  cyaml steps = cyaml_get(doc, "jobs.build.steps");
  REQUIRE_NE((void *)steps, NULL);
  REQUIRE_EQ(cyaml_type(steps), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(steps), (size_t)2);

  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "jobs.build.steps.#0.name")),
                "Checkout");
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "jobs.build.steps.#1.run")),
                "make");

  cyaml branches = cyaml_get(doc, "on.push.branches");
  REQUIRE_NE((void *)branches, NULL);
  REQUIRE_EQ(cyaml_type(branches), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(branches), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(branches, 0)), "main");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(branches, 1)), "develop");

  cyaml_destroy(doc);
}

TEST(real_world, ansible_like_with_anchors) {
  /* Both tasks reach &common with a "<<: *common" merge key instead of
   * writing its fields out again, so this test catches a regression inside a
   * realistic document of several levels, covering the registration of an
   * anchor, the resolution of an alias and the expansion of a merge key.
   *
   * The document also has a mapping with an anchor followed directly by a
   * sibling key ("tasks") at the SAME indentation. That shape is the one most
   * at risk, because a parser can swallow the sibling key into the value of
   * the anchor instead of making it a separate entry. */
  const char *yaml =
      "defaults: &common\n"
      "  timeout: 30\n"
      "  max_retries: 3\n"
      "tasks:\n"
      "  - name: fetch data\n"
      "    <<: *common\n"
      "  - name: store result\n"
      "    <<: *common\n"
      "    timeout: 60\n"
      "    max_retries: 5\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml defaults = cyaml_dictionary_get(doc, "defaults");
  REQUIRE_NE((void *)defaults, NULL);
  REQUIRE_EQ(cyaml_type(defaults), CYAML_DICTIONARY);
  cyaml defaults_timeout = cyaml_dictionary_get(defaults, "timeout");
  REQUIRE_NE((void *)defaults_timeout, NULL);
  REQUIRE_EQ(cyaml_int_val(defaults_timeout), 30LL);

  cyaml tasks = cyaml_dictionary_get(doc, "tasks");
  REQUIRE_NE((void *)tasks, NULL);
  REQUIRE_EQ(cyaml_type(tasks), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(tasks), (size_t)2);

  /* The first task takes both of its fields from the merge alone. */
  cyaml task0 = cyaml_list_get(tasks, 0);
  REQUIRE_NE((void *)task0, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(task0, "name")),
                "fetch data");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task0, "timeout")), 30LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task0, "max_retries")), 3LL);

  /* In the second task, the explicit keys replace the values of the merge. */
  cyaml task1 = cyaml_list_get(tasks, 1);
  REQUIRE_NE((void *)task1, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(task1, "name")),
                "store result");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task1, "timeout")), 60LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task1, "max_retries")), 5LL);

  cyaml_destroy(doc);
}

TEST(real_world, kubernetes_like) {
  const char *yaml =
      "apiVersion: apps/v1\n"
      "kind: Deployment\n"
      "metadata:\n"
      "  name: myapp\n"
      "  labels:\n"
      "    app: myapp\n"
      "    tier: backend\n"
      "spec:\n"
      "  replicas: 3\n"
      "  selector:\n"
      "    matchLabels:\n"
      "      app: myapp\n"
      "  template:\n"
      "    metadata:\n"
      "      labels:\n"
      "        app: myapp\n"
      "    spec:\n"
      "      containers:\n"
      "        - name: myapp\n"
      "          image: myapp:latest\n"
      "          ports:\n"
      "            - containerPort: 8080\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "kind")), "Deployment");
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "metadata.name")), "myapp");
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "spec.replicas")), 3LL);
  REQUIRE_STREQ(
      cyaml_str_val(cyaml_get(doc, "spec.template.spec.containers.#0.name")),
      "myapp");
  REQUIRE_EQ(
      cyaml_int_val(cyaml_get(
          doc, "spec.template.spec.containers.#0.ports.#0.containerPort")),
      8080LL);

  cyaml_destroy(doc);
}

TEST(real_world, comments_ignored) {
  const char *yaml =
      "# This is a comment\n"
      "key: value # inline comment\n"
      "# Another comment\n"
      "other: 42\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "other")), 42LL);
  cyaml_destroy(doc);
}

TEST(real_world, comment_terminated_by_bare_cr_does_not_swallow_next_line) {
  /* A lone '\r' with no '\n' after it must end a "# comment", just as a '\n'
   * and a "\r\n" do. skip_ws_comments uses skip_to_eol, and when skip_to_eol
   * knows only '\n', a comment that a bare CR ends swallows the whole next
   * line as part of the comment. */
  const char *yaml = "a: 1 # note\rb: 2\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), 2LL);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         ERROR HANDLING                                     */
/* ========================================================================== */

TEST(errors, unterminated_double_quote) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"unterminated\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, unterminated_single_quote) {
  char *err = NULL;
  cyaml doc = cyaml_parse("'unterminated\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, indented_doc_end_marker_is_plain_scalar_continuation) {
  /* An indented '...' is not a marker for the end of a document, because the
   * YAML spec needs every document marker at column 0. This '...' is
   * indented more than the mapping around it, so it is ordinary content that
   * continues a plain scalar over more than one line, not an error. PyYAML
   * agrees and gives {key: "value ..."}. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: value\n  ...\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value ...");
  cyaml_destroy(doc);
}

TEST(errors, unknown_alias) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key: *nonexistent\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_input) {
  char *err = NULL;
  cyaml doc = cyaml_parse(NULL, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_input_ignored_error) {
  cyaml doc = cyaml_parse(NULL, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(errors, null_input_parse_n) {
  /* Calling cyaml_parse_n with a NULL input must return NULL and set the
   * error string, just as cyaml_parse does when the caller gives an error
   * pointer that is not NULL. */
  char *err = NULL;
  cyaml doc = cyaml_parse_n(NULL, 0, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_list_element_error_cleanup_no_leak) {
  /* An element of a flow list can parse correctly and then meet a
   * continuation that is not valid. Here the continuation line comes after a
   * newline and is indented by no space at all: its tab is separation and
   * not indentation, and the value of "a" needs one space. The error path
   * must free that finished element on its way out, not only the flow list
   * that it has built so far; without that, this input leaks. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: [1\n\tb]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);

  /* The closing bracket may sit at the column of the key only after spaces:
   * a tab in front of it leaves it indented by nothing. */
  err = NULL;
  doc = cyaml_parse("a: [1\n\t]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, deeply_nested_explicit_keys_rejected_not_hung) {
  /* One line can hold a chain of "? " explicit-key indicators, where the key
   * of each one is the whole rest of the chain after it. That shape is a real
   * denial-of-service vector.
   *
   * node_to_dict_key_string canonicalizes a key that is not a scalar into
   * text, putting double quotes again around the text that the level below
   * it has already produced. The length of the canonical string, and the
   * time to build it, therefore grow as O(2^depth) rather than O(depth).
   * With no guard, an input of about 200 bytes with 100 levels eats an
   * unbounded amount of time and memory.
   *
   * Two guards are in play here. The check on the maximum length of a
   * canonical key (CYAML_MAX_CANONICAL_KEY_LEN) rejects this pattern at a
   * shallow depth, so the parse never gets near CYAML_MAX_PARSE_DEPTH for
   * this input.
   *
   * The parse must reject the input and return quickly, without hanging or
   * crashing. This test asserts that directly with clock(), following the
   * precedent of
   * serialize.oom_short_circuits_remaining_siblings_after_depth_exceeded in
   * this same file, because Tau has no timeout for one test: without the
   * assertion, a regression here hangs the whole binary instead of failing
   * visibly. */
  char input[203];
  int off = 0;
  for (int i = 0; i < 100; i++) {
    input[off++] = '?';
    input[off++] = ' ';
  }
  input[off++] = '~';
  input[off++] = '\n';
  input[off] = '\0';
  char *err = NULL;
  clock_t start = clock();
  cyaml doc = cyaml_parse(input, &err);
  double elapsed_s = (double)(clock() - start) / CLOCKS_PER_SEC;
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_LT(elapsed_s, 2.0);
}

TEST(errors,
     repeated_explicit_key_lines_separated_by_bare_cr_flatten_not_nest) {
  /* A guard on current_col() and line_start_pos() for a bare CR. A chain of
   * separate "?" lines at the SAME column holds sibling entries, not nested
   * ones, unlike the chain in deeply_nested_explicit_keys_rejected_not_hung
   * above, whose "? ? ? ..." sits on one line and is truly nested. A '\n' or
   * a bare '\r' between the lines makes no difference here. PyYAML agrees,
   * and it also collapses 100 repeated "?\n" lines at column 0 into one
   * {null: null} entry, because each later bare key defines the same null key
   * again.
   *
   * current_col() and line_indent_has_tab() must therefore treat a bare '\r'
   * as the start of a line in their backward scan. When they know only '\n',
   * a line that ends with '\r' alone puts every later "?" at a column that
   * climbs and is wrong, and the parser then reads this exact shape as real
   * nesting.
   *
   * The parse must finish at once and give the same flat, harmless
   * dictionary of one entry that a '\n'-separated version gives. This test
   * checks the key and the value type of that one entry, not only the size
   * of the top level, because that is what separates the flat parse from the
   * deeply nested misparse: a nested misparse of {null: {null: {...}}} also
   * has exactly one entry at the top level, but the value of that entry
   * would be a CYAML_DICTIONARY instead of a CYAML_NULL. */
  char input[201];
  for (int i = 0; i < 100; i++) {
    input[i * 2] = '?';
    input[i * 2 + 1] = '\r';
  }
  input[200] = '\0';
  char *err = NULL;
  cyaml doc = cyaml_parse(input, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  cyaml null_val = cyaml_dictionary_get(doc, "null");
  REQUIRE_NE((void *)null_val, NULL);
  REQUIRE_EQ(cyaml_type(null_val), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(errors, oversized_flat_non_scalar_key_rejected) {
  /* CYAML_MAX_CANONICAL_KEY_LEN (64 KiB) guards the canonical text of every
   * dictionary key that is not a scalar, not only the O(2^depth) shape of
   * deeply nested explicit keys that
   * deeply_nested_explicit_keys_rejected_not_hung above covers. The parser
   * must also reject one flat key with no recursion in it whose own canonical
   * text alone goes past the limit. */
  size_t str_len = 70000;
  size_t cap = str_len + 64;
  char *input = malloc(cap);
  REQUIRE_NE((void *)input, NULL);
  size_t off = (size_t)snprintf(input, cap, "? [\"");
  memset(input + off, 'a', str_len);
  off += str_len;
  off += (size_t)snprintf(input + off, cap - off, "\"]\n: v\n");
  char *err = NULL;
  cyaml doc = cyaml_parse(input, &err);
  free(input);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, excessive_parse_nesting_depth_rejected_not_crashed) {
  /* An ordinary deep chain of nesting does not grow at an exponential rate.
   * This one holds plain block sequences, not the pattern of explicit keys
   * that are not scalars above, and CYAML_MAX_PARSE_DEPTH alone must bound
   * it. That bound protects against a plain stack overflow from a recursion
   * with no depth limit and works apart from the guard on the length of a
   * canonical key above, which this input never reaches, because none of its
   * keys is a non-scalar.
   *
   * The parse must reject the input and return quickly, without hanging or
   * crashing. This test asserts that directly with clock(), for the reason
   * that deeply_nested_explicit_keys_rejected_not_hung above gives. */
  size_t n = 2000;
  char *input = malloc(n * 2 + 1);
  REQUIRE_NE((void *)input, NULL);
  for (size_t i = 0; i < n; i++) {
    input[i * 2] = '-';
    input[i * 2 + 1] = ' ';
  }
  input[n * 2] = '\0';
  char *err = NULL;
  clock_t start = clock();
  cyaml doc = cyaml_parse(input, &err);
  double elapsed_s = (double)(clock() - start) / CLOCKS_PER_SEC;
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_LT(elapsed_s, 2.0);
  free(input);
}

TEST(errors, exponential_alias_expansion_rejected_not_exhausted) {
  /* Every reference to an alias builds an independent deep clone of the
   * subtree of its anchor with cyaml_clone, and the registration of an
   * anchor also clones the node that carries the anchor. A document can nest
   * aliases of aliases, so the final count of live nodes grows at an
   * exponential rate in the number of anchor levels, while the source text
   * and the recursion depth of the parser stay small. This is the classic
   * "billion laughs" shape of entity expansion, and it is a real
   * denial-of-service vector.
   *
   * It differs from both guards above: this input never nests explicit keys,
   * and its recursion depth never comes near CYAML_MAX_PARSE_DEPTH. The
   * budget of node allocations (CYAML_MAX_PARSE_NODES) must reject it
   * quickly, instead of exhausting memory or hanging, and this test asserts
   * that directly with clock(), for the reason that
   * deeply_nested_explicit_keys_rejected_not_hung above gives.
   *
   * That other test is rejected within microseconds, because the guard on
   * the length of a canonical key catches it at a shallow depth, whereas
   * this guard trips only after the parser really builds and frees
   * CYAML_MAX_PARSE_NODES clones, so the bound here is far more generous.
   * The measured figures are about 0.5 seconds for a plain run and about 13
   * seconds under the per-allocation instrumentation of valgrind, on a
   * typical development machine. A bound of 60 seconds leaves a wide margin
   * for a slower or busier environment while staying nowhere near a hang
   * with no end. */
  const int levels = 30; /* 2^30 nodes if ever fully materialized */
  size_t cap = 64 + (size_t)levels * 40;
  char *input = malloc(cap);
  REQUIRE_NE((void *)input, NULL);
  size_t off = 0;
  off += (size_t)snprintf(input + off, cap - off, "a0: &a0 [0]\n");
  for (int i = 1; i < levels; i++) {
    off += (size_t)snprintf(input + off, cap - off, "a%d: &a%d [*a%d, *a%d]\n",
                            i, i, i - 1, i - 1);
  }
  char *err = NULL;
  clock_t start = clock();
  cyaml doc = cyaml_parse(input, &err);
  double elapsed_s = (double)(clock() - start) / CLOCKS_PER_SEC;
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_LT(elapsed_s, 60.0);
  free(input);
}

extern size_t cyaml_test_parse_node_floor;
extern size_t cyaml_test_parse_byte_floor;
size_t cyaml_debug_parse_budget_for_input(size_t floor, size_t per_byte,
                                          size_t input_len);

/* Build "- []\n" repeated n times. Each element costs one list node and its
 * empty backing vector, about 48 DOM bytes for each input byte. */
static char *_empty_lists_doc(size_t n) {
  char *d = malloc(n * 5 + 1);
  if (!d) return NULL;
  for (size_t i = 0; i < n; i++) memcpy(d + i * 5, "- []\n", 5);
  d[n * 5] = '\0';
  return d;
}

TEST(parse_budget, byte_limit_scales_with_the_input) {
  /* The DOM byte limit of a parse is max(floor, 512 x input length). The test
   * lowers the floor to 1 MiB, so that a document of 100 KB whose DOM takes
   * several MiB shows the scaling. This test is non-vacuous: with a fixed
   * limit equal to the floor, the parse fails with the memory limit. */
  size_t saved = cyaml_test_parse_byte_floor;
  cyaml_test_parse_byte_floor = (size_t)1024 * 1024;
  char *d = _empty_lists_doc(20000);
  char *err = NULL;
  cyaml doc = d ? cyaml_parse(d, &err) : NULL;
  size_t len = doc ? cyaml_list_len(doc) : 0;
  cyaml_destroy(doc);
  cyaml_test_parse_byte_floor = saved;
  free(d);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(len, (size_t)20000);
}

extern __thread size_t cyaml_test_last_parse_nodes_charged;
extern __thread size_t cyaml_test_last_parse_bytes_charged;

TEST(parse_budget, worst_ordinary_shape_parses_above_the_floors) {
  /* A flow sequence of empty pairs, "[:,:,:]", charges the most for each
   * input byte of any document without aliases or nested collection keys:
   * about 331 DOM bytes and 1.5 nodes. The scaled limits (512 bytes and 4
   * nodes for each input byte) leave it a margin of at least 1.5. The test
   * lowers both floors far below what 20000 pairs need, so that the scaled
   * limits alone decide. This test is non-vacuous: with a byte factor of 64
   * or a node factor of 1 the parse fails with the limit. */
  size_t saved_nodes = cyaml_test_parse_node_floor;
  size_t saved_bytes = cyaml_test_parse_byte_floor;
  cyaml_test_parse_node_floor = 1000;
  cyaml_test_parse_byte_floor = (size_t)1024 * 1024;
  size_t n = 20000;
  char *d = malloc(n * 2 + 2);
  size_t len = 0, count = 0, nodes = 0, bytes = 0;
  char *err = NULL;
  if (d) {
    d[0] = '[';
    for (size_t i = 0; i < n; i++) {
      d[1 + i * 2] = ':';
      d[2 + i * 2] = ',';
    }
    d[n * 2] = ']';
    d[n * 2 + 1] = '\0';
    len = n * 2 + 1;
    cyaml doc = cyaml_parse(d, &err);
    count = doc ? cyaml_list_len(doc) : 0;
    nodes = cyaml_test_last_parse_nodes_charged;
    bytes = cyaml_test_last_parse_bytes_charged;
    cyaml_destroy(doc);
  }
  cyaml_test_parse_node_floor = saved_nodes;
  cyaml_test_parse_byte_floor = saved_bytes;
  free(d);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(count, n);
  /* The charges stay within the derivation, at least 1.5 times below each
   * factor, so a new charge that pushes this shape past that margin fails
   * here before it can refuse a real document. */
  REQUIRE_LE(nodes * 3, len * 4 * 2);
  REQUIRE_LE(bytes * 3, len * 512 * 2);
  /* The floors really were below the charge, so the scaled limits decided. */
  REQUIRE_GT(nodes, (size_t)1000);
  REQUIRE_GT(bytes, (size_t)1024 * 1024);
}

TEST(parse_budget, node_limit_scales_with_the_input) {
  /* The node limit of a parse is max(floor, four nodes for each input byte).
   * With the floor lowered to 1000, a flow list of 3000 elements (6000
   * bytes) parses. This test is non-vacuous: with a fixed limit equal to the
   * floor, the parse fails with the node-allocation limit. */
  size_t saved = cyaml_test_parse_node_floor;
  cyaml_test_parse_node_floor = 1000;
  size_t n = 3000;
  char *d = malloc(n * 2 + 2);
  size_t len = 0;
  char *err = NULL;
  if (d) {
    d[0] = '[';
    for (size_t i = 0; i < n; i++) {
      d[1 + i * 2] = '1';
      d[2 + i * 2] = ',';
    }
    d[n * 2] = ']';
    d[n * 2 + 1] = '\0';
    cyaml doc = cyaml_parse(d, &err);
    len = doc ? cyaml_list_len(doc) : 0;
    cyaml_destroy(doc);
  }
  cyaml_test_parse_node_floor = saved;
  free(d);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(len, n);
}

TEST(parse_budget, alias_expansion_of_a_padded_input_is_still_refused) {
  /* The scaled limits grow by a constant factor of the input, so an
   * expansion that grows exponentially with the input meets them anyway. A
   * nested-alias document padded with 200 KB of comment has a byte limit of
   * 12.8 MB, and its expansion is refused quickly, with a diagnostic that
   * names the limit. */
  const int levels = 30;
  size_t pad = 200 * 1024;
  size_t cap = pad + 64 + (size_t)levels * 40;
  char *input = malloc(cap);
  REQUIRE_NE((void *)input, NULL);
  memset(input, ' ', pad);
  input[0] = '#';
  input[pad - 1] = '\n';
  size_t off = pad;
  off += (size_t)snprintf(input + off, cap - off, "a0: &a0 [\"%s\"]\n",
                          "0123456789abcdef0123456789abcdef");
  for (int i = 1; i < levels; i++)
    off += (size_t)snprintf(input + off, cap - off, "a%d: &a%d [*a%d, *a%d]\n",
                            i, i, i - 1, i - 1);
  char *err = NULL;
  clock_t start = clock();
  cyaml doc = cyaml_parse(input, &err);
  double elapsed_s = (double)(clock() - start) / CLOCKS_PER_SEC;
  bool named = err && strstr(err, "limit") != NULL;
  cyaml_destroy(doc);
  free(input);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_TRUE(named);
  REQUIRE_LT(elapsed_s, 60.0);
}

/* Build a nested-alias document (each level holds eight aliases of the
 * level below) preceded by a comment of pad bytes. */
static char *_padded_alias_bomb(size_t pad, size_t *len_out) {
  const int levels = 30;
  size_t cap = pad + 256 + (size_t)levels * 64;
  char *d = malloc(cap);
  if (!d) return NULL;
  size_t off = 0;
  if (pad >= 2) {
    d[0] = '#';
    memset(d + 1, 'p', pad - 2);
    d[pad - 1] = '\n';
    off = pad;
  }
  off += (size_t)snprintf(d + off, cap - off, "a0: &a0 \"%s\"\n",
                          "0123456789abcdef0123456789abcdef0123456789");
  for (int i = 1; i < levels; i++)
    off += (size_t)snprintf(d + off, cap - off,
                            "a%d: &a%d [*a%d,*a%d,*a%d,*a%d,*a%d,*a%d,*a%d,"
                            "*a%d]\n",
                            i, i, i - 1, i - 1, i - 1, i - 1, i - 1, i - 1,
                            i - 1, i - 1);
  *len_out = off;
  return d;
}

TEST(parse_budget, alias_expansion_limit_does_not_grow_with_padding) {
  /* The ordinary limits scale with the input length. What anchors and
   * aliases copy is charged to a separate budget that keeps the fixed
   * floors, so padding a nested-alias document with a long comment does not
   * let its expansion use more memory. The test lowers the floors to 1 MiB
   * and 20000 nodes. Every padding is refused with the alias expansion
   * limit (the unpadded one meets the ordinary limit, which then equals the
   * floor), and the DOM bytes that the parse charged stay near that floor.
   * This test is non-vacuous: with the copies charged only to the scaled
   * budget, the padded documents charge about 512 bytes for each input byte
   * before they fail, far past the bound asserted here, and their diagnostic
   * names the document limit. */
  size_t saved_nodes = cyaml_test_parse_node_floor;
  size_t saved_bytes = cyaml_test_parse_byte_floor;
  cyaml_test_parse_node_floor = 20000;
  cyaml_test_parse_byte_floor = (size_t)1024 * 1024;
  static const size_t pads[] = {0, 4096, 64 * 1024, 512 * 1024};
  size_t n_pads = sizeof(pads) / sizeof(pads[0]);
  size_t refused = 0, named = 0, bounded = 0;
  size_t worst_bytes = 0;
  for (size_t i = 0; i < n_pads; i++) {
    size_t len = 0;
    char *d = _padded_alias_bomb(pads[i], &len);
    if (!d) continue;
    char *err = NULL;
    cyaml doc = cyaml_parse_n(d, len, &err);
    size_t bytes = cyaml_test_last_parse_bytes_charged;
    if (!doc) refused++;
    /* Without padding the ordinary limit equals the floor and binds first;
     * with it, only the expansion limit can refuse the copies. */
    if (err &&
        strstr(err, pads[i] ? "alias expansion memory limit" : "memory limit"))
      named++;
    /* The floor, plus what the ordinary text itself costs. */
    if (bytes <= (size_t)1024 * 1024 + 64 * 1024) bounded++;
    if (bytes > worst_bytes) worst_bytes = bytes;
    cyaml_destroy(doc);
    free(d);
  }
  cyaml_test_parse_node_floor = saved_nodes;
  cyaml_test_parse_byte_floor = saved_bytes;
  REQUIRE_EQ(refused, n_pads);
  REQUIRE_EQ(named, n_pads);
  REQUIRE_EQ(bounded, n_pads);
  REQUIRE_LE(worst_bytes, (size_t)1024 * 1024 + 64 * 1024);
}

TEST(parse_budget, merge_key_documents_meet_the_fixed_expansion_limit) {
  /* A merge key copies every member of its source into its target, and its
   * source is an alias, which is itself a copy. A source merged into many
   * targets is amplification in the same way as a plain alias, and meets the
   * same fixed limit whatever the padding of the input. */
  size_t saved_bytes = cyaml_test_parse_byte_floor;
  cyaml_test_parse_byte_floor = (size_t)1024 * 1024;
  size_t pad = 256 * 1024;
  size_t refs = 400;
  size_t cap = pad + 64 * 1024 + refs * 40;
  char *d = malloc(cap);
  cyaml doc = NULL;
  char *err = NULL;
  size_t bytes = 0;
  if (d) {
    d[0] = '#';
    memset(d + 1, 'p', pad - 2);
    d[pad - 1] = '\n';
    size_t off = pad;
    off += (size_t)snprintf(d + off, cap - off, "src: &s\n");
    for (int k = 0; k < 64; k++)
      off += (size_t)snprintf(d + off, cap - off, "  k%d: [1, 2, 3, 4]\n", k);
    for (size_t r = 0; r < refs; r++)
      off += (size_t)snprintf(d + off, cap - off, "t%zu: {<<: *s}\n", r);
    doc = cyaml_parse_n(d, off, &err);
    bytes = cyaml_test_last_parse_bytes_charged;
  }
  cyaml_test_parse_byte_floor = saved_bytes;
  bool refused = d && !doc;
  bool named = err && strstr(err, "alias expansion memory limit");
  cyaml_destroy(doc);
  free(d);
  REQUIRE_TRUE(refused);
  REQUIRE_TRUE(named);
  REQUIRE_LE(bytes, (size_t)1024 * 1024 + 256 * 1024);
}

TEST(parse_budget, scaled_limit_saturates_below_the_unarmed_sentinel) {
  /* (size_t)-1 means "no parse runs". A limit computed for an input so large
   * that the product overflows saturates one below it, on every width. */
  REQUIRE_EQ(cyaml_debug_parse_budget_for_input(100, 64, 1), (size_t)100);
  REQUIRE_EQ(cyaml_debug_parse_budget_for_input(100, 64, 10), (size_t)640);
  REQUIRE_EQ(cyaml_debug_parse_budget_for_input(100, 64, SIZE_MAX / 64),
             (SIZE_MAX / 64) * 64);
  REQUIRE_EQ(cyaml_debug_parse_budget_for_input(100, 64, SIZE_MAX / 64 + 1),
             SIZE_MAX - 1);
  REQUIRE_EQ(cyaml_debug_parse_budget_for_input(100, 1, SIZE_MAX),
             SIZE_MAX - 1);
}

TEST(errors, trailing_garbage) {
  /* The text "extra junk !!!" on the second line holds no ':' separator, so
   * the block dictionary stops after "key: value" and the parser then
   * reports trailing content.
   */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: value\nextra junk !!!\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

/* ========================================================================== */
/*                         BLOCK SEQUENCE NEXT-LINE VALUE                     */
/* ========================================================================== */

TEST(block_sequence, value_on_next_line_mapping) {
  /* The block-in form: the value of each list entry sits on the next line,
   * which is more indented, instead of on the same line as the '-'. */
  const char *yaml =
      "-\n"
      "  name: alice\n"
      "  age: 30\n"
      "-\n"
      "  name: bob\n"
      "  age: 25\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);

  cyaml alice = cyaml_list_get(doc, 0);
  REQUIRE_EQ(cyaml_type(alice), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(alice, "name")), "alice");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(alice, "age")), 30LL);

  cyaml bob = cyaml_list_get(doc, 1);
  REQUIRE_EQ(cyaml_type(bob), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(bob, "name")), "bob");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(bob, "age")), 25LL);

  cyaml_destroy(doc);
}

TEST(block_sequence, value_on_next_line_scalar) {
  /* A scalar value on the line after the '-'. */
  const char *yaml =
      "-\n"
      "  hello\n"
      "-\n"
      "  world\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "hello");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "world");
  cyaml_destroy(doc);
}

TEST(block_sequence, null_when_sibling_follows_on_next_line) {
  /* A '-', then a newline, then another '-' at the same indent: the first
   * element is null, and the second is not. */
  const char *yaml =
      "-\n"
      "- value\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  cyaml elem0 = cyaml_list_get(doc, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_type(elem0), CYAML_NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "value");
  cyaml_destroy(doc);
}

TEST(block_sequence, nested_sequence_on_next_line) {
  /* An inner list whose '-' entries sit on the line after the outer '-'.
   */
  const char *yaml =
      "matrix:\n"
      "  -\n"
      "    - 1\n"
      "    - 2\n"
      "  -\n"
      "    - 3\n"
      "    - 4\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml matrix = cyaml_dictionary_get(doc, "matrix");
  REQUIRE_NE((void *)matrix, NULL);
  REQUIRE_EQ(cyaml_type(matrix), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(matrix), (size_t)2);

  cyaml row0 = cyaml_list_get(matrix, 0);
  REQUIRE_EQ(cyaml_type(row0), CYAML_LIST);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(row0, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(row0, 1)), 2LL);

  cyaml row1 = cyaml_list_get(matrix, 1);
  REQUIRE_EQ(cyaml_type(row1), CYAML_LIST);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(row1, 0)), 3LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(row1, 1)), 4LL);

  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         MAPPING KEY STARTING WITH '-'                      */
/* ========================================================================== */

TEST(block_mapping, second_key_starting_with_dash) {
  /* A plain scalar key that starts with a '-' with no space after it, as the
   * second key of a block dictionary. The loop over the dictionary must
   * neither read that '-' as a sequence indicator nor stop there. */
  const char *yaml =
      "key: value\n"
      "-key: other\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "-key")), "other");
  cyaml_destroy(doc);
}

TEST(block_mapping, multiple_keys_starting_with_dash) {
  /* Many keys, one after the other, that each start with a '-'. */
  const char *yaml =
      "-a: 1\n"
      "-b: 2\n"
      "-c: 3\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "-a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "-b")), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "-c")), 3LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, duplicate_key_last_wins) {
  /* A mapping can hold the same key twice: the last value wins, and the
   * library frees the earlier value and leaks nothing. The implementation
   * defines this behaviour; see cyaml.h. */
  const char *yaml =
      "key: first\n"
      "key: second\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "second");
  cyaml_destroy(doc);
}

TEST(block_mapping, duplicate_key_replaces_container) {
  /* The first value is a list, which the second value replaces with a
   * scalar. The library must free the whole replaced list, down through
   * every child, and must leak nothing and corrupt nothing. */
  const char *yaml =
      "key:\n"
      "  - 1\n"
      "  - 2\n"
      "key: scalar\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "key")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "scalar");
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         FLOAT-SPECIAL STRING ROUND-TRIPS                   */
/* ========================================================================== */

TEST(serialize, float_special_strings_are_quoted) {
  /* A string node can hold a value that matches a special float token of
   * YAML, and the serializer must put quotes around such a value so that a
   * parse of the output gives a string, not a float. */
  const char *specials[] = {"+.inf", "+.Inf", "+.INF", "-.inf", "-.Inf",
                            "-.INF", ".inf",  ".Inf",  ".INF",  ".nan",
                            ".NaN",  ".NAN",  NULL};
  for (int i = 0; specials[i]; i++) {
    cyaml n = cyaml_create_string(specials[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    /* The first non-whitespace byte must not be the plain token itself. */
    REQUIRE_NE(s[0], specials[i][0]);
    /* A parse of the output must give a STRING, not a FLOAT. */
    char *err = NULL;
    cyaml back = cyaml_parse(s, &err);
    REQUIRE_EQ((void *)err, NULL);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(back), specials[i]);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
    cyaml_destroy(back);
  }
}

/* ========================================================================== */
/*                         TAB-WHITESPACE QUOTING (needs_quoting)             */
/* ========================================================================== */

TEST(serialize, string_with_colon_tab_round_trip) {
  /* A string can hold a ':' with a tab after it, and the serializer must put
   * quotes around such a string: without them the plain scalar ends at the
   * colon, and a parse of that output gives a dictionary instead of a
   * string. */
  cyaml n = cyaml_create_string("proto:\thttp");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  /* The value must NOT start with the plain token 'p' directly followed by a
   * colon with no quotes; the first byte must be a quote character. */
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "proto:\thttp");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, string_with_tab_hash_round_trip) {
  /* A string can hold a tab with a '#' after it, and the serializer must put
   * quotes around such a string. Without the quotes, the plain scalar parser
   * reads the tab and the '#' as the start of a comment on the same line and
   * truncates the value. */
  cyaml n = cyaml_create_string("text\t#comment");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "text\t#comment");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, string_with_hash_after_tab_in_mapping_round_trip) {
  /* The same rule for a tab and a '#', where the string is a dictionary
   * value. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "key", cyaml_create_string("val\t#x")),
             ccol_success);
  char *s = cyaml_serialize(doc);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);
  cyaml val = cyaml_dictionary_get(doc2, "key");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_type(val), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(val), "val\t#x");
  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(serialize, string_starting_with_tab_round_trip) {
  /* The serializer must put quotes around a string whose value starts with a
   * tab; without them, the parser takes that first tab as whitespace and the
   * value changes with no report. */
  cyaml n = cyaml_create_string("\thello");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "\thello");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, string_ending_with_space_round_trip) {
  /* The serializer must put quotes around a string whose value ends with a
   * space, because without them the plain scalar parser removes that
   * trailing space. */
  cyaml n = cyaml_create_string("hello ");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "hello ");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, string_ending_with_tab_round_trip) {
  /* The serializer must put quotes around a string whose value ends with a
   * tab, because without them the plain scalar parser removes that trailing
   * tab. */
  cyaml n = cyaml_create_string("hello\t");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "hello\t");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, plus_word_not_quoted) {
  /* A string such as "+extra" does not start a numeric literal, so
   * needs_quoting() must return false, and the plain scalar then makes a
   * correct round trip. */
  cyaml n = cyaml_create_string("+extra");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  /* The serialized form must hold NO quotes. */
  REQUIRE_STREQ(s, "+extra\n");
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "+extra");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, plus_digit_is_quoted) {
  /* make_typed_scalar parses "+42" as the integer 42, so needs_quoting() must
   * put quotes around it. */
  cyaml n = cyaml_create_string("+42");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "+42");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, plus_dot_digit_is_quoted) {
  /* With no quotes in the output, strtod parses "+.3" as the float 0.3, so
   * the serializer must put quotes around it for it to make a round trip as
   * a CYAML_STRING. */
  cyaml n = cyaml_create_string("+.3");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(back), "+.3");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(serialize, plus_dot_float_strings_are_quoted) {
  /* strtod parses some +.N patterns as floats, so the serializer must put
   * quotes around every one of them for each to make a round trip as a
   * CYAML_STRING. */
  const char *cases[] = {"+.3", "+.5", "+.1e10", "+.0", "+.99e-5", NULL};
  for (int i = 0; cases[i]; i++) {
    cyaml n = cyaml_create_string(cases[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char *err = NULL;
    cyaml back = cyaml_parse(s, &err);
    REQUIRE_EQ((void *)err, NULL);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(back), cases[i]);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
    cyaml_destroy(back);
  }
}

TEST(serialize, plus_dot_underflowing_float_strings_are_quoted) {
  /* strtod() parses both "+.1e-400" and "+.999e-320" and sets errno to
   * ERANGE for each: the first underflows all the way to 0.0, and the second
   * underflows to a legitimate subnormal. Both are nevertheless real
   * CYAML_FLOAT values that the parser accepted, because
   * try_parse_float_scalar() keeps ERANGE apart from a real overflow; see
   * implicit_types.float_subnormal_underflow_is_still_a_float and
   * float_underflow_to_zero_is_still_a_float.
   *
   * needs_quoting() must therefore put quotes around them, exactly as it does
   * for the "+.N" cases above that do not underflow. Without the quotes, a
   * CYAML_STRING that holds this text parses again as a CYAML_FLOAT instead
   * of making a round trip as a string. */
  const char *cases[] = {"+.1e-400", "+.999e-320", NULL};
  for (int i = 0; cases[i]; i++) {
    cyaml n = cyaml_create_string(cases[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    char *err = NULL;
    cyaml back = cyaml_parse(s, &err);
    REQUIRE_EQ((void *)err, NULL);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ(cyaml_type(back), CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(back), cases[i]);
    cyaml_serialize_free(s);
    cyaml_destroy(n);
    cyaml_destroy(back);
  }
}

TEST(serialize, excessive_depth_rejected_not_crashed) {
  /* A tree that a caller gives to cyaml_serialize() or cyaml_serialize_flow()
   * does not have to come from cyaml_parse(). CYAML_MAX_PARSE_DEPTH bounds a
   * parse on its own, at a much lower depth, but a caller can instead build
   * a tree directly with cyaml_create_list() and cyaml_list_push(), and
   * construction puts no bound on the depth at all.
   *
   * Each serializer must therefore bound its own recursion with
   * CYAML_MAX_SERIALIZE_DEPTH and report NULL, its documented contract for a
   * failure of memory, instead of overflowing the stack and crashing on a
   * tree that is merely deep and holds no cycle.
   *
   * The depth of 1000 sits well past CYAML_MAX_SERIALIZE_DEPTH, which is 500,
   * while staying shallow enough for the cleanup of this test: that cleanup
   * is the tree walk of cyaml_destroy, which has no depth guard, and it
   * cannot exhaust the stack at this depth. */
  size_t depth = 1000;
  cyaml root = cyaml_create_null();
  REQUIRE_NE((void *)root, NULL);
  for (size_t i = 0; i < depth; i++) {
    cyaml outer = cyaml_create_list();
    REQUIRE_NE((void *)outer, NULL);
    REQUIRE_EQ(cyaml_list_push(outer, root), ccol_success);
    root = outer;
  }

  char *block = cyaml_serialize(root);
  REQUIRE_EQ((void *)block, NULL);

  char *flow = cyaml_serialize_flow(root);
  REQUIRE_EQ((void *)flow, NULL);

  cyaml_destroy(root);
}

TEST(serialize, oom_short_circuits_remaining_siblings_after_depth_exceeded) {
  /* serialize_block() and serialize_flow() must stop the WHOLE walk the
   * moment b->oom is set, whether because CYAML_MAX_SERIALIZE_DEPTH trips on
   * one branch or because the allocator truly fails, instead of merely
   * stopping the appends to the buffer. Without the full stop, they walk
   * every sibling branch again and find the same failure once more.
   *
   * Both functions therefore need an "if (b->oom) return;" guard at the top.
   * Without it, a wide top-level list of many deep branches costs
   * O(branches * CYAML_MAX_SERIALIZE_DEPTH) after the very first branch trips
   * the depth guard, where the correct cost is O(1); every dictionary that
   * those repeated walks visit pays for a real chmap_begin_iter_safe()
   * allocation, and the walk throws its own output away at once, because
   * b->oom is already true.
   *
   * This test builds branch_count independent, deep chains of dictionaries
   * with one key each, each chain safely past CYAML_MAX_SERIALIZE_DEPTH. The
   * correct behaviour walks only the FIRST branch in full and then stops, so
   * the elapsed time stays about the same for any branch_count; without the
   * guard, the time grows in a straight line with branch_count.
   *
   * These figures come from a build with the guard removed. The correct
   * behaviour finishes this call in under 1 millisecond, while the build
   * with no guard takes about 1.25 seconds for branch_count walks of a chain
   * of about 520 levels. The bound of 0.5 seconds below sits between the
   * two, leaving a wide margin on the fast side for a slower run or a run
   * under instrumentation such as valgrind. */
  size_t branch_count = 3000;
  size_t chain_depth = 520; /* > CYAML_MAX_SERIALIZE_DEPTH (500) */

  cyaml top = cyaml_create_list();
  REQUIRE_NE((void *)top, NULL);
  for (size_t b = 0; b < branch_count; b++) {
    cyaml chain = cyaml_create_null();
    REQUIRE_NE((void *)chain, NULL);
    for (size_t i = 0; i < chain_depth; i++) {
      cyaml outer = cyaml_create_dictionary();
      REQUIRE_NE((void *)outer, NULL);
      REQUIRE_EQ(cyaml_dictionary_set(outer, "child", chain), ccol_success);
      chain = outer;
    }
    REQUIRE_EQ(cyaml_list_push(top, chain), ccol_success);
  }

  clock_t start = clock();
  char *block = cyaml_serialize(top);
  double elapsed_s = (double)(clock() - start) / CLOCKS_PER_SEC;
  REQUIRE_EQ((void *)block, NULL);
  REQUIRE_LT(elapsed_s, 0.5);

  cyaml_destroy(top);
}

TEST(block_mapping, document_start_marker_no_separator) {
  /* YAML 1.2 section 9.1.4 says when the '---' token starts a document: the
   * three dashes must be followed by whitespace, a comment '#' or the end of
   * the input. In the text '---42' the fourth character is '4', which is not
   * a separator, so it is NOT a marker for the start of a document, and the
   * parser must read it as the plain scalar string "---42". */
  char *err = NULL;
  cyaml doc = cyaml_parse("---42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---42");
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         BOUNDED PARSE (cyaml_parse_n)                      */
/* ========================================================================== */

TEST(parse_n, basic_bounded_parse) {
  /* cyaml_parse_n must parse only the first len bytes; the parser cannot see
   * any byte past that length. */
  const char *buf = "key: value\ntrailing garbage";
  char *err = NULL;
  /* "key: value\n" is exactly 11 bytes. */
  cyaml doc = cyaml_parse_n(buf, 11, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(parse_n, not_null_terminated) {
  /* cyaml_parse_n must not read past len, and this holds when no null byte
   * is there. */
  char buf[16];
  memcpy(buf, "42", 2);
  /* The rest of buf stays uninitialised on purpose. */
  char *err = NULL;
  cyaml doc = cyaml_parse_n(buf, 2, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(parse_n, sequence_bounded) {
  /* Check a round trip of a list through the parser with a length bound. */
  const char *yaml = "- 1\n- 2\n- 3\n";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc, 2)), 3LL);
  cyaml_destroy(doc);
}

TEST(parse_n, bom_prefix) {
  /* cyaml_parse_n must step over a UTF-8 BOM (EF BB BF) at the start and then
   * parse the rest in the normal way. */
  const char bom_yaml[] =
      "\xEF\xBB\xBF"
      "key: value\n";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(bom_yaml, sizeof(bom_yaml) - 1, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         DELETE                                             */
/* ========================================================================== */

TEST(delete, list_remove_middle) {
  char *err = NULL;
  cyaml root = cyaml_parse("- 10\n- 20\n- 30\n- 40\n", &err);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)4);

  REQUIRE_EQ(cyaml_list_remove(root, 1), ccol_success);

  REQUIRE_EQ(cyaml_list_len(root), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 0)), 10LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 1)), 30LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 2)), 40LL);
  cyaml_destroy(root);
}

TEST(delete, list_remove_first) {
  char *err = NULL;
  cyaml root = cyaml_parse("- 1\n- 2\n- 3\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_list_remove(root, 0), ccol_success);

  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 0)), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 1)), 3LL);
  cyaml_destroy(root);
}

TEST(delete, list_remove_last) {
  char *err = NULL;
  cyaml root = cyaml_parse("- 1\n- 2\n- 3\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_list_remove(root, 2), ccol_success);

  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 1)), 2LL);
  cyaml_destroy(root);
}

TEST(delete, list_remove_only_element) {
  char *err = NULL;
  cyaml root = cyaml_parse("- 42\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_list_remove(root, 0), ccol_success);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)0);
  cyaml_destroy(root);
}

TEST(delete, list_remove_out_of_bounds) {
  char *err = NULL;
  cyaml root = cyaml_parse("- 1\n- 2\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_list_remove(root, 2), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  cyaml_destroy(root);
}

TEST(delete, list_remove_subtree_freed) {
  /* A list element can itself be a nested mapping, and removing such an
   * element must not leak. */
  char *err = NULL;
  cyaml root = cyaml_parse("- a: 1\n  b:\n    - 10\n    - 20\n- 99\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_list_remove(root, 0), ccol_success);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 0)), 99LL);
  cyaml_destroy(root);
}

TEST(delete, dictionary_remove_existing_key) {
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\nb: 2\nc: 3\n", &err);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(root), (size_t)3);

  REQUIRE_EQ(cyaml_dictionary_remove(root, "b"), ccol_success);

  REQUIRE_EQ(cyaml_dictionary_size(root), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(root, "b"), NULL);
  REQUIRE_NE((void *)cyaml_dictionary_get(root, "a"), NULL);
  REQUIRE_NE((void *)cyaml_dictionary_get(root, "c"), NULL);
  cyaml_destroy(root);
}

TEST(delete, dictionary_remove_missing_key) {
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_dictionary_remove(root, "z"), ccol_key_not_found);
  REQUIRE_EQ(cyaml_dictionary_size(root), (size_t)1);
  cyaml_destroy(root);
}

TEST(delete, dictionary_remove_subtree_freed) {
  /* The value of a key can be a nested container, and removing such a key
   * must not leak. */
  char *err = NULL;
  cyaml root = cyaml_parse("keep: 1\ndrop:\n  x:\n    - 1\n    - 2\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_dictionary_remove(root, "drop"), ccol_success);
  REQUIRE_EQ(cyaml_dictionary_size(root), (size_t)1);
  REQUIRE_NE((void *)cyaml_dictionary_get(root, "keep"), NULL);
  cyaml_destroy(root);
}

TEST(delete, path_delete_dict_key) {
  char *err = NULL;
  cyaml root = cyaml_parse("x: 1\ny: 2\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "x"), ccol_success);
  REQUIRE_EQ(cyaml_dictionary_size(root), (size_t)1);
  REQUIRE_EQ((void *)cyaml_get(root, "x"), NULL);
  REQUIRE_NE((void *)cyaml_get(root, "y"), NULL);
  cyaml_destroy(root);
}

TEST(delete, path_delete_nested_key) {
  char *err = NULL;
  cyaml root = cyaml_parse("a:\n  b: 1\n  c: 2\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a.b"), ccol_success);
  REQUIRE_EQ((void *)cyaml_get(root, "a.b"), NULL);
  REQUIRE_NE((void *)cyaml_get(root, "a.c"), NULL);
  cyaml_destroy(root);
}

TEST(delete, path_delete_list_element) {
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - 10\n  - 20\n  - 30\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "items.#1"), ccol_success);

  cyaml items = cyaml_get(root, "items");
  REQUIRE_NE((void *)items, NULL);
  REQUIRE_EQ(cyaml_list_len(items), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(items, 0)), 10LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(items, 1)), 30LL);
  cyaml_destroy(root);
}

TEST(delete, path_delete_on_list_root) {
  /* The call cyaml_delete(list_root, "#N") must work when the root node is
   * itself a CYAML_LIST and not a dictionary. */
  char *err = NULL;
  cyaml root = cyaml_parse("- 10\n- 20\n- 30\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);

  REQUIRE_EQ(cyaml_delete(root, "#1"), ccol_success);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 0)), 10LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 1)), 30LL);
  cyaml_destroy(root);
}

TEST(delete, path_missing_parent_returns_key_not_found) {
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "x.y"), ccol_key_not_found);
  cyaml_destroy(root);
}

TEST(delete, path_missing_leaf_returns_key_not_found) {
  char *err = NULL;
  cyaml root = cyaml_parse("a:\n  b: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a.z"), ccol_key_not_found);
  cyaml_destroy(root);
}

TEST(delete, path_out_of_range_list_index_returns_key_not_found) {
  /* A "#N" leaf component with a valid syntax and an index out of range is an
   * absent path component, the same kind of absent component as a dictionary
   * key that is not there. The documented contract of cyaml_delete says
   * "ccol_key_not_found if any path component is absent".
   *
   * This case must stay apart from ccol_invalid_args, which cyaml_delete
   * keeps for a malformed path: an empty path, or a leaf that is not a "#N"
   * on a list parent.
   *
   * cyaml_list_remove() is a different function that this rule does not
   * touch: a direct call to it correctly returns ccol_invalid_args for the
   * same index out of range (see list_remove_out_of_bounds above). The
   * distinction belongs only to the higher-level contract that cyaml_delete
   * gives for a path. */
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - 1\n  - 2\n  - 3\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "items.#10"), ccol_key_not_found);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(root, "items")), (size_t)3);
  cyaml_destroy(root);
}

TEST(delete,
     path_malformed_index_syntax_in_non_leaf_component_returns_invalid_args) {
  /* A malformed "#N" index, one that does not match a '#' followed by digits,
   * is an error in the path that the caller built, not a question about the
   * data that is there, and this must hold at ANY position in the path, not
   * only at the leaf.
   *
   * navigate_y() must therefore keep apart a malformed index in the middle of
   * a path and a component in the middle that is truly absent, instead of
   * giving one NULL result for both. With one shared NULL result, the caller
   * gets ccol_key_not_found here in place of ccol_invalid_args. */
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "items.#xyz.a"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(root, "items")), (size_t)2);
  cyaml_destroy(root);
}

TEST(delete, path_wrong_type_in_non_leaf_component_returns_invalid_args) {
  /* A scalar can appear in the middle of a path, with more components after
   * it, which differs from a scalar that sits at the direct parent of the
   * leaf. Such a scalar has no children to walk into, so this is an error of
   * the wrong type in the path (ccol_invalid_args), not an error about an
   * absent component (ccol_key_not_found).
   *
   * Here "a" is the scalar 1, so "a.b.c" must fail inside navigate_y()
   * itself, while that function tries to resolve the parent segment "a.b",
   * and not in the separate check for the wrong type that _cyaml_delete runs
   * on the direct parent afterward. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a.b.c"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(root, "a")), 1LL);
  cyaml_destroy(root);
}

TEST(delete, path_delete_with_escaped_dot_in_key) {
  /* A key whose name is the literal "a.b" must be reachable with "a\\.b". */
  cyaml root = cyaml_create_dictionary();
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(root, "a.b", cyaml_create_int(7)),
             ccol_success);
  REQUIRE_EQ(cyaml_dictionary_set(root, "keep", cyaml_create_int(1)),
             ccol_success);

  REQUIRE_EQ(cyaml_delete(root, "a\\.b"), ccol_success);
  REQUIRE_EQ(cyaml_dictionary_size(root), (size_t)1);
  REQUIRE_NE((void *)cyaml_dictionary_get(root, "keep"), NULL);
  cyaml_destroy(root);
}

TEST(delete, degenerate_path_empty) {
  /* The call must reject an empty path with ccol_invalid_args. */
  char *err = NULL;
  cyaml root = cyaml_parse("key: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, ""), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "key"), NULL);
  cyaml_destroy(root);
}

TEST(delete, degenerate_path_trailing_dot) {
  /* A path that ends with a dot gives an empty leaf component, and
   * cyaml_delete must reject such a path with ccol_invalid_args and leave the
   * document whole. */
  char *err = NULL;
  cyaml root = cyaml_parse("key: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "key."), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "key"), NULL);
  cyaml_destroy(root);
}

TEST(delete, degenerate_path_consecutive_dots) {
  /* A path with two dots together gives an empty component in the middle of
   * the parent path, so navigate_y fails and the call must return
   * ccol_invalid_args, as it does for an empty LEAF component (see "key." in
   * delete.degenerate_path_trailing_dot above).
   *
   * An empty path component has no valid reading anywhere in a path: it is
   * neither a literal key of an empty string nor a "#N" index. It is
   * therefore an error in the path that the caller built, with a malformed
   * syntax, not a component that is well formed but absent, whether the
   * empty component is the leaf or a component in the middle. */
  char *err = NULL;
  cyaml root = cyaml_parse("a:\n  b: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a..b"), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "a.b"), NULL);
  cyaml_destroy(root);
}

/* ========================================================================== */
/*                         MULTI-DOCUMENT PARSING                             */
/* ========================================================================== */

TEST(multi_document, two_scalars_with_markers) {
  /* Two plain scalar documents with a '---' between them. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\nhello\n---\nworld\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(root, 0)), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(root, 0)), "hello");
  REQUIRE_EQ(cyaml_type(cyaml_list_get(root, 1)), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(root, 1)), "world");
  cyaml_destroy(root);
}

TEST(multi_document, two_mappings_no_leading_marker) {
  /* The first document has no '---' in front of it, but the second one needs
   * one. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\nb: 2\n---\nx: 10\ny: 20\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);

  cyaml doc0 = cyaml_list_get(root, 0);
  REQUIRE_EQ(cyaml_type(doc0), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc0, "a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc0, "b")), 2LL);

  cyaml doc1 = cyaml_list_get(root, 1);
  REQUIRE_EQ(cyaml_type(doc1), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc1, "x")), 10LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc1, "y")), 20LL);
  cyaml_destroy(root);
}

TEST(multi_document, two_sequences) {
  /* Two sequence documents. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\n- 1\n- 2\n---\n- 3\n- 4\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);

  cyaml seq0 = cyaml_list_get(root, 0);
  REQUIRE_NE((void *)seq0, NULL);
  REQUIRE_EQ(cyaml_type(seq0), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(seq0), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq0, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq0, 1)), 2LL);

  cyaml seq1 = cyaml_list_get(root, 1);
  REQUIRE_NE((void *)seq1, NULL);
  REQUIRE_EQ(cyaml_type(seq1), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(seq1), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq1, 0)), 3LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq1, 1)), 4LL);
  cyaml_destroy(root);
}

TEST(multi_document, with_document_end_markers) {
  /* Explicit '...' end markers stand between the documents. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\na: 1\n...\n---\nb: 2\n...\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(cyaml_list_get(root, 0), "a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(cyaml_list_get(root, 1), "b")), 2LL);
  cyaml_destroy(root);
}

TEST(multi_document, empty_document_between_markers) {
  /* A '---' with another '---' directly after it gives a null document. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\nhello\n---\n---\nworld\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)3);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(root, 0)), "hello");
  cyaml doc1 = cyaml_list_get(root, 1);
  REQUIRE_NE((void *)doc1, NULL);
  REQUIRE_EQ(cyaml_type(doc1), CYAML_NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(root, 2)), "world");
  cyaml_destroy(root);
}

TEST(multi_document, three_documents) {
  /* Three mapping documents. */
  char *err = NULL;
  cyaml root = cyaml_parse(
      "---\nkind: Service\n---\nkind: Deployment\n---\nkind: ConfigMap\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)3);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(cyaml_list_get(root, 0), "kind")),
                "Service");
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(cyaml_list_get(root, 1), "kind")),
                "Deployment");
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(cyaml_list_get(root, 2), "kind")),
                "ConfigMap");
  cyaml_destroy(root);
}

TEST(multi_document, single_document_not_wrapped) {
  /* The parser gives one document back directly instead of putting it inside
   * a list. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\na: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(root, "a")), 1LL);
  cyaml_destroy(root);
}

TEST(multi_document, anchors_do_not_leak_across_documents) {
  /* An anchor that doc1 defines must not resolve in doc2. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\nval: &anchor 42\n---\nref: *anchor\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)root, NULL);
}

TEST(multi_document, doc_start_marker_in_mapping_value_is_data) {
  /* A '---' can appear as the value of a key on the same line, where it is
   * plain string data and NOT a boundary between two documents. */
  char *err = NULL;
  cyaml root = cyaml_parse("key: ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(root, "key")), "---");
  cyaml_destroy(root);
}

TEST(multi_document, triple_doc_start_in_sequence_value_is_data) {
  /* A '---' after a '- ' (the indicator of a sequence item) is plain string
   * data. */
  char *err = NULL;
  cyaml root = cyaml_parse("- ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(root, 0)), "---");
  cyaml_destroy(root);
}

TEST(multi_document, bare_document_after_end_marker_is_a_new_document) {
  /* YAML 1.2 section 6.9 defines l-yaml-stream, under which a document can
   * leave out its own '---' in one case only: a '...' end marker directly
   * before it. That '...' is enough on its own to start the next document,
   * which can then be bare, with no '---', so such content is not trailing
   * garbage but a second, valid document. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n...\nsome text\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  cyaml doc0 = cyaml_list_get(root, 0);
  cyaml doc1 = cyaml_list_get(root, 1);
  REQUIRE_NE((void *)doc1, NULL);
  REQUIRE_EQ(cyaml_type(doc0), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc0, "a")), 1LL);
  REQUIRE_EQ(cyaml_type(doc1), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc1), "some text");
  cyaml_destroy(root);
}

TEST(multi_document,
     redundant_consecutive_end_markers_do_not_fabricate_a_document) {
  /* The l-yaml-stream rule of YAML 1.2 section 6.9 groups one or more '...'
   * markers that follow each other. That group, l-document-suffix+, is one
   * unit of separator material, not one document boundary for each marker,
   * so a second '...' directly after the first adds nothing: the parser must
   * not read it as "the next document is empty" or build an extra CYAML_NULL
   * document that nobody wrote. PyYAML agrees and parses this as one document
   * with no list around it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n...\n...\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  cyaml_destroy(doc);
}

TEST(multi_document,
     redundant_consecutive_end_markers_then_real_next_document) {
  /* The same shape of a '...' that adds nothing, with a real second document
   * after it. The parse must give exactly two documents with no extra NULL
   * document between them; PyYAML agrees and parses this as exactly two
   * documents. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n...\n...\n---\nb: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  cyaml doc0 = cyaml_list_get(root, 0);
  cyaml doc1 = cyaml_list_get(root, 1);
  REQUIRE_EQ(cyaml_type(doc0), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc0, "a")), 1LL);
  REQUIRE_EQ(cyaml_type(doc1), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc1, "b")), 2LL);
  cyaml_destroy(root);
}

TEST(multi_document, three_consecutive_end_markers_then_bare_document) {
  /* Three '...' markers in a row followed by a valid bare document with no
   * '---', which the rule that lets a document leave out its '---' after a
   * '...' covers. The test confirms that the loop over the extra markers does
   * not eat into the content of the document after them. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n...\n...\n...\nsome text\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  cyaml doc0 = cyaml_list_get(root, 0);
  cyaml doc1 = cyaml_list_get(root, 1);
  REQUIRE_NE((void *)doc1, NULL);
  REQUIRE_EQ(cyaml_type(doc0), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc0, "a")), 1LL);
  REQUIRE_EQ(cyaml_type(doc1), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc1), "some text");
  cyaml_destroy(root);
}

TEST(multi_document, bare_document_without_preceding_end_marker_is_error) {
  /* With no '...' between them, a second document MUST start with '---'. The
   * first document here has no end marker, so bare content directly after it
   * is truly unclear trailing material, not a new document. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\nsome garbage\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)root, NULL);
}

TEST(errors, duplicate_yaml_directive_rejected) {
  /* YAML 1.2 section 6.8.1 says that "it is an error to define more than one
   * YAML directive for the same document, even if both occurrences give the
   * same version", and the SF5V case of the vendored YAML Test Suite confirms
   * it. Two directives for two SEPARATE documents, each with its own '---',
   * are a different case that this rule does not touch. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n%YAML 1.2\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, content_after_end_marker_on_same_line_rejected) {
  /* Only whitespace or a comment can share the line of a '...' marker. Real
   * trailing content there, such as "... invalid", has no valid reading, as
   * the 3HFZ case of the vendored YAML Test Suite confirms. */
  char *err = NULL;
  cyaml doc = cyaml_parse("---\nkey: value\n... invalid\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, hash_glued_directly_onto_start_marker_is_plain_scalar) {
  /* A '#' that sits directly on '---' with no whitespace between them is not
   * a comment, because the s-l-comments grammar of YAML 1.2 needs
   * s-separate-in-line, which is real whitespace, before the '#' of a
   * comment; the skip_ws_comments() and rest_of_line_is_blank() rule of this
   * parser says the same elsewhere.
   *
   * The text "---#x" is therefore ordinary plain-scalar content, not a
   * marker for the start of a document followed by a comment. PyYAML and
   * Psych both agree and parse this as the plain scalar "---#x". */
  char *err = NULL;
  cyaml doc = cyaml_parse("---#x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---#x");
  cyaml_destroy(doc);
}

TEST(errors, hash_glued_directly_onto_end_marker_is_plain_scalar) {
  /* The same rule as above, for a '...': a '#' that sits directly on it does
   * not end the plain scalar content, so the text "...#x" folds together
   * with the next line into one plain scalar over two lines. PyYAML agrees
   * and parses this document as the string "...#x foo". */
  char *err = NULL;
  cyaml doc = cyaml_parse("...#x\nfoo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "...#x foo");
  cyaml_destroy(doc);
}

TEST(errors, hash_after_real_whitespace_on_start_marker_still_a_comment) {
  /* A '#' with real whitespace before it, after a '---', is an ordinary
   * comment, and the marker itself keeps its meaning. This test guards
   * against a rule above that reaches too far. */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- #comment\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml key_node = cyaml_dictionary_get(doc, "key");
  REQUIRE_NE((void *)key_node, NULL);
  REQUIRE_EQ(cyaml_type(key_node), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(key_node), "value");
  cyaml_destroy(doc);
}

TEST(multi_document, parse_n_multi_document) {
  /* cyaml_parse_n also accepts a stream of more than one document. */
  const char buf[] = "---\n1\n---\n2\n";
  char *err = NULL;
  cyaml root = cyaml_parse_n(buf, sizeof(buf) - 1, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(root, 1)), 2LL);
  cyaml_destroy(root);
}

TEST(parse_n, zero_length_input) {
  /* Calling cyaml_parse_n with len=0 must act like an empty document: it
   * returns a CYAML_NULL node and reports no error. */
  char *err = NULL;
  cyaml root = cyaml_parse_n("ignored", 0, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_NULL);
  cyaml_destroy(root);
}

/* ========================================================================== */
/*              FLOW SERIALIZATION ROUND-TRIPS (needs_quoting)                */
/* ========================================================================== */

TEST(flow_serialize, comma_in_string_round_trip) {
  /* A string that holds a bare ',' needs double quotes in flow output;
   * without them, a parse of {key: a,b} gives two separate entries. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "key", cyaml_create_string("a,b")),
             ccol_success);
  char *s = cyaml_serialize_flow(doc);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc2, "key")), "a,b");
  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(flow_serialize, close_bracket_in_string_round_trip) {
  /* A string inside a flow sequence can hold a ']' in the middle of it, and
   * such a string needs quotes, because without them the plain scalar ends
   * early at that ']'. */
  cyaml seq = cyaml_create_list();
  REQUIRE_NE((void *)seq, NULL);
  REQUIRE_EQ(cyaml_list_push(seq, cyaml_create_string("x]y")), ccol_success);
  char *s = cyaml_serialize_flow(seq);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml seq2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)seq2, NULL);
  REQUIRE_EQ(cyaml_type(seq2), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(seq2), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(seq2, 0)), "x]y");
  cyaml_serialize_free(s);
  cyaml_destroy(seq);
  cyaml_destroy(seq2);
}

TEST(flow_serialize, close_brace_in_string_round_trip) {
  /* A string inside a flow mapping can hold a '}' in the middle of it, and
   * such a string needs quotes, because without them the plain scalar ends
   * early at that '}'. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "key", cyaml_create_string("p}q")),
             ccol_success);
  char *s = cyaml_serialize_flow(doc);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc2, "key")), "p}q");
  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(flow_serialize, colon_before_comma_round_trip) {
  /* A colon with a ',' directly after it needs quotes, because the plain
   * scalar parser reads ':,' as the end of the indicator for the value. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "k", cyaml_create_string("v:,rest")),
             ccol_success);
  char *s = cyaml_serialize_flow(doc);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc2, "k")), "v:,rest");
  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

TEST(flow_serialize, colon_before_close_bracket_round_trip) {
  /* A colon with a ']' directly after it needs quotes in a flow sequence. */
  cyaml seq = cyaml_create_list();
  REQUIRE_NE((void *)seq, NULL);
  REQUIRE_EQ(cyaml_list_push(seq, cyaml_create_string("v:]rest")),
             ccol_success);
  char *s = cyaml_serialize_flow(seq);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml seq2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)seq2, NULL);
  REQUIRE_EQ(cyaml_type(seq2), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(seq2), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(seq2, 0)), "v:]rest");
  cyaml_serialize_free(s);
  cyaml_destroy(seq);
  cyaml_destroy(seq2);
}

TEST(flow_serialize, colon_before_close_brace_round_trip) {
  /* A colon with a '}' directly after it needs quotes in a flow mapping. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "k", cyaml_create_string("v:}rest")),
             ccol_success);
  char *s = cyaml_serialize_flow(doc);
  REQUIRE_NE((void *)s, NULL);
  char *err = NULL;
  cyaml doc2 = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc2, NULL);
  REQUIRE_EQ(cyaml_type(doc2), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc2, "k")), "v:}rest");
  cyaml_serialize_free(s);
  cyaml_destroy(doc);
  cyaml_destroy(doc2);
}

/* ========================================================================== */
/*                         YAML DIRECTIVES AND TAGS                           */
/* ========================================================================== */

TEST(directives, yaml_directive_ignored) {
  /* The parser must accept a %YAML directive line before a '---' without
   * reporting anything for it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, tag_directive_ignored) {
  /* The parser must accept a %TAG directive line before a '---' without
   * reporting anything for it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%TAG ! foo:\n---\nval: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "val")), 1LL);
  cyaml_destroy(doc);
}

TEST(directives, multiple_directives_ignored) {
  /* The parser must accept many directive lines before a '---' without
   * reporting anything for any of them. */
  const char *yaml =
      "%YAML 1.2\n"
      "%TAG ! foo:\n"
      "%TAG !! bar:\n"
      "---\n"
      "answer: 42\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "answer")), 42LL);
  cyaml_destroy(doc);
}

TEST(directives, custom_node_tag_preserved_and_has_no_typing_effect) {
  /* A scalar can carry a custom local tag such as !foo, which the parser
   * keeps and a caller can read with cyaml_node_tag(). The tag never forces a
   * type of its own: the rules for implicit types resolve the bare value in
   * the normal way. */
  char *err = NULL;
  cyaml doc = cyaml_parse("value: !foo 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml value = cyaml_dictionary_get(doc, "value");
  REQUIRE_NE((void *)value, NULL);
  REQUIRE_STREQ(cyaml_node_tag(value), "!foo");
  REQUIRE_EQ(cyaml_type(value), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(value), 42LL);
  cyaml_destroy(doc);
}

TEST(directives, double_exclamation_tag_forces_string_type) {
  /* A secondary tag such as !!str on a scalar forces CYAML_STRING, whatever
   * type the bare value resolves to on its own. */
  char *err = NULL;
  cyaml doc = cyaml_parse("val: !!str 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "val")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "val")), "42");
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "val")),
                CYAML_TAG_STR);
  cyaml_destroy(doc);
}

TEST(errors, yaml_directive_extra_word_rejected) {
  /* YAML 1.2 section 6.8.1 defines l-yaml-directive as "YAML", then
   * s-separate-in-line, then ns-yaml-version: exactly one version token and
   * nothing else, so a word after the version has no valid reading. PyYAML
   * agrees and rejects this in the same way. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2 foo\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, yaml_directive_comment_without_separating_space_rejected) {
  /* A '#' that sits directly on the version, with no whitespace between them,
   * is not a valid comment, since the general rule of this codebase says
   * that a comment needs whitespace before it. The '#' is therefore malformed
   * trailing content on the directive line; PyYAML agrees and rejects this
   * in the same way. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.1#comment\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(directives, yaml_directive_extra_spaces_accepted) {
  /* The parser accepts any number of spaces between "YAML" and the version
   * token, not only one. PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML  1.1\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, yaml_directive_tab_separator_accepted) {
  /* The separators of a directive are s-separate-in-line (YAML 1.2 section
   * 6.8), which may hold tabs. The YAML Test Suite case MUS6-3 holds
   * "%YAML \t 1.1". This test is non-vacuous: a parser that refuses a tab
   * there fails every case below. */
  REQUIRE_TRUE(
      _tab_accepted_as("%YAML\t1.1\n---\nkey: value\n", "{key: value}"));
  REQUIRE_TRUE(
      _tab_accepted_as("%YAML \t1.1\n---\nkey: value\n", "{key: value}"));
  REQUIRE_TRUE(_tab_accepted_as("%YAML 1.2\t# comment\n---\nk: v\n", "{k: v}"));
  /* A tab does not make a malformed directive well-formed. */
  require_tab_rejected("%YAML\t\n---\nk: v\n");
  require_tab_rejected("%YAML 1.2\tx\n---\nk: v\n");
}

TEST(directives, tag_directive_tab_separator_accepted) {
  /* Both separators of a %TAG directive, and the one before a comment after
   * its prefix, may hold tabs, for the reason in
   * yaml_directive_tab_separator_accepted. */
  const char *cases[] = {
      "%TAG\t!e! tag:e.com,2000:\n--- !e!x y\n",
      "%TAG \t!e! tag:e.com,2000:\n--- !e!x y\n",
      "%TAG !e!\ttag:e.com,2000:\n--- !e!x y\n",
      "%TAG !e! \ttag:e.com,2000:\n--- !e!x y\n",
      "%TAG !e! tag:e.com,2000:\t# comment\n--- !e!x y\n",
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(cases[i], &err);
    bool ok = err == NULL && doc &&
              strcmp(cyaml_node_tag(doc), "tag:e.com,2000:x") == 0 &&
              strcmp(cyaml_str_val(doc), "y") == 0;
    cyaml_destroy(doc);
    REQUIRE_TRUE(ok);
  }
  require_tab_rejected("%TAG !e!\t\n---\nk: v\n");
}

TEST(directives, yaml_directive_trailing_comment_with_separator_accepted) {
  /* A comment with correct whitespace between it and the version is a valid,
   * ordinary trailing comment; PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.1  # comment\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, unrecognized_directive_name_with_extra_words_accepted) {
  /* Only the name "YAML" takes the strict grammar above with its fixed number
   * of tokens. Every other directive name, even one that looks like "YAML"
   * such as "YAM" or "YAMLL", falls under ns-reserved-directive of YAML 1.2
   * section 6.8.2, which lets a directive carry any number of trailing
   * parameters. PyYAML agrees and accepts both names. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAM 1.1\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);

  err = NULL;
  doc = cyaml_parse("%YAMLL 1.1\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                    DOCUMENT-START MARKER EDGE CASES                        */
/* ========================================================================== */

TEST(block_mapping, indented_triple_dash_is_plain_scalar) {
  /* A '---' that comes after whitespace, at a column above 0, is NOT a marker
   * for the start of a document, so the parser must read it as the plain
   * string "---". */
  char *err = NULL;
  cyaml doc = cyaml_parse("   ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---");
  cyaml_destroy(doc);
}

TEST(errors, block_mapping_cannot_start_on_document_marker_line) {
  /* Content can sit on the same physical line as an explicit '---', but the
   * s-l+block-node grammar of YAML 1.2 gives such content only one choice,
   * "flow-in-block", which covers a plain scalar, a quoted scalar and a flow
   * collection and never covers "block-in-block".
   *
   * A block mapping needs "block-in-block", and that choice needs
   * s-l-comments directly after the '---', which is only whitespace, a
   * comment or a newline, so a block mapping must start on its OWN line. Two
   * independent reference parsers agree: both reject this document, and both
   * accept the same content with no '---' at all, or with a '---' and a
   * newline before it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- a: b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_sequence_cannot_start_on_document_marker_line) {
  /* The same restriction as in
   * block_mapping_cannot_start_on_document_marker_line, for a block sequence,
   * which has no "flow-in-block" choice either. Two independent reference
   * parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- - a\n    - b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, anchored_mapping_cannot_start_on_document_marker_line) {
  /* The same restriction again, reached here through an anchor on the text
   * that would be the key: the document "--- &anchor a: b" has no valid
   * reading either. The same "&anchor a: b" with no marker at all is an
   * ordinary, valid key with an anchor, and so is one with the marker on its
   * own line. Two independent reference parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- &anchor a: b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, mapping_on_own_line_after_document_marker_still_works) {
  /* A block mapping that starts on the line AFTER a '---' is entirely
   * ordinary; content on the same line as the '---' is the restricted case.
   * Two independent reference parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("---\na: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  cyaml_destroy(doc);
}

TEST(block_mapping, scalar_on_document_marker_line_still_works) {
  /* A plain scalar, which is neither a mapping nor a sequence, can sit
   * directly on the same line as a '---' and keeps working, with or without
   * an anchor on it. Only "block-in-block" content, a mapping or a sequence,
   * must move to a later line, because a scalar has the "flow-in-block"
   * choice open to it. Two independent reference parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(doc), "foo");
  cyaml_destroy(doc);

  err = NULL;
  doc = cyaml_parse("--- &x foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(doc), "foo");
  cyaml_destroy(doc);
}

/* Explicit "? key" / ": value" block mapping entries */

TEST(explicit_block_mapping, simple_key_and_value) {
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\n: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, explicit_key_with_no_value_is_null) {
  /* YAML 1.2 section 8.2.2 says that an explicit key with no ':' after it at
   * the same indent has a null value. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\nb: c\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_type(a), CYAML_NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "c");
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, mixed_explicit_and_implicit_entries) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n? b\n: 2\nc: 3\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_int_val(a), 1);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_int_val(b), 2);
  cyaml c = cyaml_dictionary_get(doc, "c");
  REQUIRE_NE((void *)c, NULL);
  REQUIRE_EQ(cyaml_int_val(c), 3);
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, block_scalar_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("? |\n  block key\n: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml val = cyaml_dictionary_get(doc, "block key\n");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(cyaml_str_val(val), "value");
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, anchored_key_and_alias_value) {
  /* A later alias can point at an anchor on an explicit key, such as "&a a".
   *
   * The third line is a bare ": *a" with nothing before the ':'. It is its
   * own separate entry, whose implicit key is an empty plain scalar, and it
   * does not join the "&b b" entry above it.
   *
   * Under the core schema of YAML 1.2, an empty scalar resolves to null, just
   * as the spellings "~" and "null" do, so this key canonicalizes to the same
   * "null" string key that those spellings give (see the doc comment of
   * node_to_dict_key_string()). Every site that captures a key, implicit or
   * explicit, canonicalizes through the same core-schema typing, including
   * the keys that are empty. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? &a a\n: &b b\n: *a\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml aval = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)aval, NULL);
  REQUIRE_STREQ(cyaml_str_val(aval), "b");
  cyaml empty_key_val = cyaml_dictionary_get(doc, "null");
  REQUIRE_NE((void *)empty_key_val, NULL);
  REQUIRE_STREQ(cyaml_str_val(empty_key_val), "a");
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, anchored_implicit_key_at_second_entry) {
  /* An anchor can sit on an ordinary key in the implicit style, not only on
   * an explicit '?' key, and this holds for an entry after the first one as
   * well. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n&anchor c: 3\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "c")), 3);
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, non_scalar_key_canonicalized_to_flow_text) {
  /* A sequence as an explicit key is a valid YAML construct (see sections
   * 7.4.1 and 8.2.2). A dictionary of this DOM always maps a char * to a
   * node, so the library canonicalizes the key into its compact flow YAML
   * text with cyaml_serialize_flow instead of rejecting it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? - a\n  - b\n: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "[a, b]")), "value");
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, tagged_root_object) {
  /* Also checks cyaml_node_tag(doc) itself, not just the mapping's own
   * content: without it, this test's assertions are identical to the
   * untagged "---\n? a\n: b\n" case, and cannot tell whether "!!map" was
   * actually parsed and attached to the right node versus silently
   * discarded. */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- !!map\n? a\n: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  REQUIRE_NE((void *)cyaml_node_tag(doc), NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), CYAML_TAG_MAP);
  cyaml_destroy(doc);
}

/* Flow collection single-pair / bare-key shorthands */

TEST(flow_collections, sequence_bare_pair_shorthand) {
  /* The text "[foo: bar]" is a shorthand for "[{foo: bar}]": one "key: value"
   * pair with no '{' and '}' around it means a mapping element of one
   * entry. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[foo: bar]", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)1);
  cyaml elem = cyaml_list_get(doc, 0);
  REQUIRE_EQ(cyaml_type(elem), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(elem, "foo")), "bar");
  cyaml_destroy(doc);
}

TEST(flow_collections, sequence_bare_pair_empty_key) {
  /* An empty key resolves to CYAML_NULL under the core schema, just as a
   * plain scalar value does, because an empty string is null, exactly as "~"
   * is. node_to_dict_key_string then canonicalizes it to the string key
   * "null", the same treatment that every other null dictionary key in this
   * DOM gets. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[: empty key]", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml elem = cyaml_list_get(doc, 0);
  REQUIRE_EQ(cyaml_type(elem), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(elem, "null")), "empty key");
  cyaml_destroy(doc);
}

TEST(flow_collections, dictionary_bare_key_no_colon_is_null) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{a: 1, b}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_type(b), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(block_mapping, indicator_prefixed_keys_not_immediately_followed_by_space) {
  /* A '?', a ':' and a '-' directly followed by a character that is not
   * whitespace are ordinary plain scalar content, not the indicators for an
   * explicit key, a value or a list. This holds for the first key of a
   * dictionary and for a later key, and it drives the check in
   * parse_one_dict_entry_key that asks whether a character is really an
   * indicator. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "?foo: safe question mark\n:bar: safe colon\n-baz: safe dash\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "?foo")),
                "safe question mark");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, ":bar")), "safe colon");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "-baz")), "safe dash");
  cyaml_destroy(doc);
}

TEST(flow_collections, dictionary_colon_no_value_is_null) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{a: 1, b:}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_type(b), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_first_key_does_not_swallow_sibling_entries) {
  /* The text "&a a: b" as the very first entry of a dictionary anchors only
   * the key scalar "a", not the whole dictionary that this entry starts, so a
   * sibling entry on the next line must be its own, separate entry with no
   * anchor. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&a a: b\nc: &d d\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "c")), "d");
  cyaml_destroy(doc);
}

TEST(block_mapping, alias_used_directly_as_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("&a a: &b b\n*b : *a\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "a");
  cyaml_destroy(doc);
}

/* parse_anchor_name() stops an alias name at a ':' followed by whitespace or
 * the end of the input, which is the same rule that scan_plain_scalar_line
 * holds, where a colon with a space, or a colon at the end of a line, ends the
 * scan.
 *
 * Without that rule, the stop set holds only whitespace, the flow indicators
 * and '#', so the colon becomes part of the parsed alias name, giving "x:"
 * instead of "x". This happens for an alias that is a key directly, with no
 * space before the colon, such as "*x: y", which is the most natural way to
 * write it, because an ordinary key is written "key: value" in the same way.
 * The parser then reports an "unknown alias" error although the anchor is
 * correctly defined.
 *
 * The other tests for an alias as a key all put a space before the colon (see
 * alias_used_directly_as_key above and alias_used_as_key_still_works below),
 * so this test is the only cover for the form with no space. */
TEST(block_mapping, alias_used_as_key_with_no_space_before_colon) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\n*x: y\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "foo");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "y");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchor_for_empty_node_does_not_swallow_sibling_entry) {
  /* The line "a: &anchor" has nothing else on it, so it anchors an empty
   * node, which is null. A sibling key on the next line at the same
   * indentation as "a" must stay a separate entry instead of being taken as
   * nested content of the anchor. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &anchor\nb: *anchor\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_type(a), CYAML_NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_type(b), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(block_list, anchor_for_empty_node_does_not_swallow_sibling_entry) {
  /* The same hazard as the mapping case above, for a sequence: the line
   * "- &anchor" has nothing else on it and must not take in the sibling
   * element that comes after it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- &anchor\n- next\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  cyaml elem0 = cyaml_list_get(doc, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_type(elem0), CYAML_NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "next");
  cyaml_destroy(doc);
}

TEST(block_mapping, sequence_value_same_indent_as_key) {
  /* YAML 1.2 section 8.2.2 lets a block sequence value sit at exactly the
   * same indentation as the mapping key above it, while every other kind of
   * value must be more indented than that key. */
  char *err = NULL;
  cyaml doc = cyaml_parse("one:\n- 2\n- 3\nfour: 5\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml one = cyaml_dictionary_get(doc, "one");
  REQUIRE_NE((void *)one, NULL);
  REQUIRE_EQ(cyaml_type(one), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(one), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(one, 0)), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(one, 1)), 3LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "four")), 5LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, sequence_value_same_indent_mixed_with_more_indented) {
  /* One mapping can hold both forms side by side: one at the same
   * indentation as its key, and the ordinary one, which is more indented. */
  char *err = NULL;
  cyaml doc = cyaml_parse("foo:\n- 42\nbar:\n  - 44\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml foo = cyaml_dictionary_get(doc, "foo");
  REQUIRE_EQ(cyaml_type(foo), CYAML_LIST);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(foo, 0)), 42LL);
  cyaml bar = cyaml_dictionary_get(doc, "bar");
  REQUIRE_EQ(cyaml_type(bar), CYAML_LIST);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(bar, 0)), 44LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, sibling_scalar_key_at_same_indent_is_not_swallowed) {
  /* A sibling at the same indent that is NOT a '-' sequence indicator gives a
   * null value followed by a new sibling entry, because the exception above,
   * for a sequence value at the same indent, covers the '-' alone. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a:\nb: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_type(a), CYAML_NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), 1LL);
  cyaml_destroy(doc);
}

TEST(flow_collections, non_scalar_flow_dictionary_key_canonicalized) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{[a, b]: value}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "[a, b]")), "value");
  cyaml_destroy(doc);
}

TEST(flow_collections, non_scalar_flow_sequence_pair_key_canonicalized) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[ {a: 1}: value ]", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml elem = cyaml_list_get(doc, 0);
  REQUIRE_EQ(cyaml_type(elem), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(elem, "{a: 1}")), "value");
  cyaml_destroy(doc);
}

TEST(flow_collections,
     non_scalar_multi_key_dictionary_key_is_canonically_sorted) {
  /* A dictionary with many keys can itself be a key. The library
   * canonicalizes it with its own entries sorted by key, in alphabetical
   * order, whatever the insertion order of the source; this source
   * deliberately uses the reverse of alphabetical order.
   *
   * The public output of cyaml_serialize_flow() is different: it is not
   * sorted and follows the insertion order (see
   * serialize.multi_key_flow_mapping_preserves_iteration_order below). This
   * canonicalization depends on the content alone. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{z: 1, a: 2}: outer_value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "{a: 2, z: 1}")),
                "outer_value");
  cyaml_destroy(doc);
}

TEST(flow_collections,
     non_scalar_dictionary_keys_collide_regardless_of_insertion_order) {
  /* This is the behaviour that the canonicalization exists for: two
   * dictionary keys that hold the same content but come from a different
   * insertion order must collide onto exactly the same stored key instead of
   * being kept as two separate entries. For a key that appears twice, the
   * last value wins (see block_mapping.duplicate_key_last_wins), so the value
   * of the second entry must win here. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{{a: 1, b: 2}: first, {b: 2, a: 1}: second}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "{a: 1, b: 2}")),
                "second");
  cyaml_destroy(doc);
}

TEST(flow_collections,
     non_scalar_dictionary_key_sorted_recursively_when_nested) {
  /* The sort holds at every level of nesting inside the key that the library
   * canonicalizes, not at the top level alone. The VALUE of a dictionary key
   * can itself be a dictionary whose keys are not sorted, and the library
   * must sort the keys of that nested dictionary too. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{outer: {z: 1, a: 2}}: v\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(
      cyaml_str_val(cyaml_dictionary_get(doc, "{outer: {a: 2, z: 1}}")), "v");
  cyaml_destroy(doc);
}

TEST(block_mapping, flow_collection_compact_implicit_key) {
  /* A flow collection directly followed by a ':' is a valid implicit key of a
   * block mapping, one that is not a scalar; YAML 1.2 section 8.2.2 calls
   * this the compact mapping form. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[a, b]: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml val = cyaml_dictionary_get(doc, "[a, b]");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(cyaml_str_val(val), "value");
  cyaml_destroy(doc);
}

TEST(block_mapping, flow_collection_compact_implicit_key_not_only_first_entry) {
  /* An implicit key can be a bare flow collection with no anchor on it, and
   * the parser must accept such a key at ANY entry position, not only at the
   * first entry of the mapping.
   *
   * parse_one_dict_entry_key handles every entry after the first, and it must
   * not filter out a '[' or a '{' before it calls try_parse_scalar_dict_key,
   * which is the function that knows how to parse a flow collection as a
   * key. With such a filter, the same construct succeeds as the first entry
   * and fails as "trailing content" for any later entry. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n[1, 2]: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_int_val(a), 1LL);
  cyaml list_key_val = cyaml_dictionary_get(doc, "[1, 2]");
  REQUIRE_NE((void *)list_key_val, NULL);
  REQUIRE_STREQ(cyaml_str_val(list_key_val), "value");
  cyaml_destroy(doc);
}

TEST(block_mapping, flow_dictionary_compact_implicit_key_not_only_first_entry) {
  /* The same rule as above, for a key that is a flow dictionary ('{') instead
   * of a flow list ('['). */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n{x: 1}: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  cyaml dict_key_val = cyaml_dictionary_get(doc, "{x: 1}");
  REQUIRE_NE((void *)dict_key_val, NULL);
  REQUIRE_STREQ(cyaml_str_val(dict_key_val), "value");
  cyaml_destroy(doc);
}

TEST(block_mapping, explicit_key_with_compact_flow_list_content) {
  /* The document is "? []: x". The content of the explicit key is itself a
   * compact block mapping on one line, "[]: x", which the "compact" choice of
   * s-l+block-indented in YAML 1.2 section 8.2.2 permits.
   *
   * The whole document is therefore a mapping of one entry, whose key is the
   * mapping of one entry {[]: x}. The library canonicalizes that key into its
   * own flow YAML text, putting quotes around "[]", because an unquoted "[]"
   * parses as a flow list that opens and closes. The value of the entry is
   * null, because nothing follows on the line. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? []: x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml outer = cyaml_dictionary_get(doc, "{\"[]\": x}");
  REQUIRE_NE((void *)outer, NULL);
  REQUIRE_EQ(cyaml_type(outer), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(block_mapping, multiline_flow_collection_not_a_valid_implicit_key) {
  /* A flow collection that covers more than one line can never be an implicit
   * key, while one on a single line can. The ns-s-implicit-yaml-key rule
   * needs one line whether or not the key is a scalar, so the ':' at the end
   * is content that the parser cannot read. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[23\n]: 42\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(directives, verbatim_tag_with_comma_not_swallowed_as_flow_terminator) {
  /* The content of a verbatim tag, written "!<...>", can legitimately hold a
   * literal ',', because its delimiters are the angle brackets and not
   * whitespace. The character set of a shorthand tag is different and holds
   * no ','. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "--- !<tag:clarkevans.com,2002:invoice>\ninvoice: 34843\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "invoice")), 34843LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, tagged_key_does_not_swallow_sibling_entry) {
  /* This test mirrors the one for the '&' branch, where a key must not
   * swallow its sibling: a key with a tag must not let its recursive parse
   * take in an unrelated sibling entry that also carries a tag.
   *
   * The tag "!!null" sits on an empty scalar, because nothing stands between
   * the tag and the ':'. The tag types the key as null, and the canonical
   * text of a null key is "null". The keys of this DOM are plain strings, not
   * nodes, so the tag itself is not kept; see the doc comment of
   * cyaml_node_tag().
   *
   * The VALUE of "b" is "!!str" with nothing after it, so the tag forces an
   * empty string, not null; a tag in a value position stays on the node. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!null : a\nb: !!str\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml empty_key_val = cyaml_dictionary_get(doc, "null");
  REQUIRE_NE((void *)empty_key_val, NULL);
  REQUIRE_STREQ(cyaml_str_val(empty_key_val), "a");
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_type(b), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(b), "");
  cyaml_destroy(doc);
}

TEST(block_mapping, tag_then_anchor_property_order) {
  /* The c-ns-properties rule permits the tag and the anchor in either order.
   * This key puts the tag first, as in "!!str &a1 ...", while another test
   * covers the anchor first and the tag second. The anchor on this key must
   * register, and a later alias must resolve to it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str &a1 \"foo\": bar\nbaz: *a1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml foo = cyaml_dictionary_get(doc, "foo");
  REQUIRE_NE((void *)foo, NULL);
  REQUIRE_STREQ(cyaml_str_val(foo), "bar");
  cyaml baz = cyaml_dictionary_get(doc, "baz");
  REQUIRE_NE((void *)baz, NULL);
  REQUIRE_STREQ(cyaml_str_val(baz), "foo");
  cyaml_destroy(doc);
}

TEST(block_mapping, alias_key_reached_through_anchor_recursion) {
  /* The document is "top: &node\n  *alias : value". The anchor for the value
   * of "top" has nothing else on its own line, so the general recursion of
   * parse_node resolves its content. That recursive entry point is the '*'
   * alias branch, not try_parse_scalar_dict_key, and it must also read an
   * alias followed by a ':' as an implicit key. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&a a: &b b\ntop: &node\n  *a : *b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml top = cyaml_dictionary_get(doc, "top");
  REQUIRE_NE((void *)top, NULL);
  REQUIRE_EQ(cyaml_type(top), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(top, "a")), "b");
  cyaml_destroy(doc);
}

TEST(errors, implicit_value_sequence_cannot_start_inline) {
  /* A block sequence value can never start on the same line as its own "key:",
   * because the grammar of an implicit mapping value,
   * ns-l-block-map-implicit-value, has no compact choice at all, so the
   * sequence must always begin on a later line. A mapping under a '-' sequence
   * entry, by contrast, does have that "compact" right. The reference parser
   * PyYAML confirms this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: - a\n     - b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, sequence_cannot_start_inline_after_anchor) {
  /* The same restriction as above, for content on the same line as an
   * anchor: the text "&anchor - x" has no valid reading, and a plain scalar
   * cannot start with a bare '-' indicator either, so the parser reports a
   * hard error. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&anchor - sequence entry\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, sequence_cannot_start_inline_after_tag) {
  /* The same restriction as in sequence_cannot_start_inline_after_anchor, for
   * content on the same line as a tag. The text "!!seq - x" has no valid
   * reading either, exactly like the anchor case: a tag and an anchor are
   * both c-ns-properties, and neither gets the separate right that the
   * "compact mapping" choice gives to a sequence on the same line. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq - sequence entry\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, explicit_value_sequence_can_start_inline) {
  /* Content in the explicit style, the content of the '?' key and of the ':'
   * value after it, follows the s-l+block-indented grammar, which an
   * implicit value does not. The "compact" choice of that grammar DOES let a
   * bare '-' sequence start on the same line. PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? k\n: - a\n  - b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml val = cyaml_dictionary_get(doc, "k");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_type(val), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(val), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(val, 0)), "a");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(val, 1)), "b");
  cyaml_destroy(doc);
}

TEST(block_mapping, explicit_key_sequence_can_start_inline) {
  /* The same "compact" right holds for the content of the '?' key, not only
   * for the ':' value. This is the construct that
   * explicit_block_mapping.non_scalar_key_canonicalized_to_flow_text drives
   * from end to end; this test looks only at the start on the same line. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? - a\n  - b\n: v\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml_destroy(doc);
}

TEST(block_list, sequence_compact_mapping_under_dash_still_works) {
  /* The difference above goes one way only: a mapping under a '-' sequence
   * entry keeps its own, separate "compact" right. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- k: v\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  cyaml elem = cyaml_list_get(doc, 0);
  REQUIRE_EQ(cyaml_type(elem), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(elem, "k")), "v");
  cyaml_destroy(doc);
}

TEST(errors, block_scalar_comment_needs_preceding_whitespace) {
  /* The text '>#comment' has no whitespace between the indicator of a folded
   * scalar and the '#', so it is not a comment: a '#' directly after the
   * header is a trailing character that is not valid. */
  char *err = NULL;
  cyaml doc = cyaml_parse("block: ># comment\n  scalar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_folded) {
  /* YAML 1.2 section 8.1.1 says: "It is an error for any of the leading
   * empty lines to contain more spaces than the first non-empty line." Here
   * the parser detects the indentation of the block as 1, from " invalid",
   * while an earlier blank line holds 3 spaces. The 5LLU case of the vendored
   * YAML Test Suite confirms this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("scalar: >\n \n  \n   \n invalid\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_literal) {
  /* The same restriction as in
   * block_scalar_leading_blank_line_more_indented_folded, for a literal ('|')
   * block scalar instead of a folded ('>') one. The W9L4 case of the
   * vendored YAML Test Suite confirms this. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "block scalar: |\n     \n  more spaces at the beginning\n"
      "  are invalid\n",
      &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_before_comment) {
  /* The same restriction again, but here the first line that is not blank,
   * and that would set block_indent, is a comment that sits inside the run of
   * blank lines at the start of the scalar. The check on those leading blank
   * lines must fire before the parser reaches the comment. The S98Z case of
   * the vendored YAML Test Suite confirms this. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("empty block scalar: >\n \n  \n   \n # comment\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_double_quoted_implicit_key_rejected) {
  /* An implicit key always sits on one line, as the ns-s-implicit-yaml-key
   * rule says and a plain scalar key obeys too, so a double-quoted scalar
   * that covers more than one line has no valid reading as a key. Two
   * independent reference parsers agree, and the 7LBH case of the vendored
   * YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\nb\": 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_single_quoted_implicit_key_rejected) {
  /* The same restriction as in multiline_double_quoted_implicit_key_rejected,
   * for a single-quoted key. Two independent reference parsers agree, and
   * the D49Q case of the vendored YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("'c\n d': 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_quoted_key_nested_in_sequence_rejected) {
  /* The same restriction, reached here through a nested sequence element
   * instead of a key at the top level. Two independent reference parsers
   * agree, and the JKF3 case of the vendored YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- - \"bar\nbar\": x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_double_quoted_implicit_key_rejected_bare_cr) {
  /* The bare CR form of multiline_double_quoted_implicit_key_rejected. This
   * parser never normalizes CR and LF in its input, so a key can cover two
   * physical lines with a bare '\r' and no '\n' at all, and such a key must
   * fail exactly like the version above that uses LF. A check that looks at
   * '\n' alone accepts this key as if it sat on one line. */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\rb\": 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_single_quoted_implicit_key_rejected_bare_cr) {
  /* The bare CR form of multiline_single_quoted_implicit_key_rejected. See
   * multiline_double_quoted_implicit_key_rejected_bare_cr above for the
   * reasoning. */
  char *err = NULL;
  cyaml doc = cyaml_parse("'c\r d': 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, multiline_quoted_value_still_works) {
  /* A guard for a regression: a quoted scalar over more than one line can be
   * an ordinary VALUE instead of a key, and the restriction on an implicit
   * key over more than one line must not touch it, so such a value stays
   * fully supported. The folding over lines of YAML 1.2 section 7.3.3 covers
   * quoted scalar VALUES; only the position of an implicit KEY is held to
   * one line. */
  char *err = NULL;
  cyaml doc = cyaml_parse("quoted: \"a\nb\nc\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "quoted")), "a b c");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_multiline_quoted_value_still_works) {
  /* A guard for the copy of the same restriction inside
   * try_parse_scalar_dict_key. An anchor can sit on a quoted scalar VALUE
   * that covers more than one line, instead of on a key, and such a value
   * must resolve as an ordinary value with an anchor instead of going
   * through the fast path that detects a key. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x \"multi\nline\"\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "multi line");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "multi line");
  cyaml_destroy(doc);
}

TEST(errors, document_marker_inside_unclosed_flow_collection_rejected) {
  /* The c-forbidden rule of YAML 1.2 section 6.9 says that a '---' or a '...'
   * document marker at the start of a line can never be plain scalar
   * content. A flow collection that is open has no valid way to end at a
   * document boundary, since only its own ']' or '}' can close it, unlike a
   * block context, where the same marker ends the current collection in the
   * ordinary way. Two independent reference parsers agree, and the N782 case
   * of the vendored YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[\n--- ,\n...\n]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_dictionary_continuation_lines_at_column_zero_rejected) {
  /* Every line that a flow collection crosses must be indented more than the
   * indent of the block value around it, as the s-separate(n,c) rule of YAML
   * 1.2 says. Here each continuation line ("k", ":", "v" and "}") sits at
   * column 0, which is never more indented than anything, so the parser
   * rejects this input on that indentation rule alone. The VJP3-0 case of
   * the vendored YAML Test Suite matches this.
   *
   * This is NOT a rule that makes the ':' of an implicit key share the line
   * of the key. The sibling case of VJP3-0 in that same suite, VJP3-1, has
   * the same shape with each continuation line indented by exactly one
   * space, carries fail=0, and this parser accepts it (see
   * implicit_key_colon_may_fold_to_a_later_line_ok below). The implicit key
   * of an ordinary flow dictionary genuinely can have its ':' on a later
   * line, while the "[key: value]" shorthand for a sequence of one pair is
   * stricter, because ns-s-implicit-yaml-key covers it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("k: {\nk\n:\nv\n}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, implicit_key_colon_may_fold_to_a_later_line_ok) {
  /* The sibling of the rejection above, with correct indentation on every
   * continuation line, matched by the VJP3-1 case of the vendored YAML Test
   * Suite, which carries fail=0: the ':' of the implicit key of a flow
   * dictionary can legitimately sit on a line after the key itself. */
  char *err = NULL;
  cyaml doc = cyaml_parse("k: {\n k\n :\n v\n }\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml inner = cyaml_dictionary_get(doc, "k");
  REQUIRE_NE((void *)inner, NULL);
  REQUIRE_EQ(cyaml_type(inner), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(inner, "k")), "v");
  cyaml_destroy(doc);
}

TEST(flow_collections, implicit_key_colon_value_folds_to_next_line_ok) {
  /* A guard for a regression: the VALUE half can fold onto a later line once
   * the parser finds the ':'. A reference parser agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{k: \nv}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "k")), "v");
  cyaml_destroy(doc);
}

TEST(flow_collections, explicit_key_colon_on_later_line_still_works) {
  /* A guard for a regression: the ':' of an EXPLICIT '?' key can also appear
   * on a later line, as it does in the block style "? key\n: value". A
   * reference parser agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{? key\n: value}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(errors, flow_collection_continuation_line_not_indented_enough_rejected) {
  /* The s-separate(n,c) grammar of YAML 1.2 goes through s-separate-lines(n)
   * to s-flow-line-prefix(n), and it needs every line that a flow collection
   * crosses to be indented more than the indent of the block value around
   * it. Here "flow:" sits at column 0, and so do the continuation lines "b,"
   * and "c]". Two independent reference parsers agree, and the 9C9N case of
   * the vendored YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("flow: [a,\nb,\nc]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, continuation_line_indented_enough_still_works) {
  /* Regression guard: the parser accepts the identical content with one more
   * space of indent throughout, because the continuation lines then sit at
   * column 1, which is more than the column 0 of "flow:". Two independent
   * reference parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("flow: [a,\n b,\n c]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml flow = cyaml_dictionary_get(doc, "flow");
  REQUIRE_NE((void *)flow, NULL);
  REQUIRE_EQ(cyaml_type(flow), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(flow), (size_t)3);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(flow, 0)), "a");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(flow, 1)), "b");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(flow, 2)), "c");
  cyaml_destroy(doc);
}

TEST(errors, anchor_flow_continuation_line_not_indented_enough_rejected) {
  /* The value of an anchor can wrap onto a later line inside a flow
   * collection: in "a: {b: &x\nc}\n", the "&x" is the last thing on its line,
   * and its value "c" comes on the next line. That newline must obey, not
   * step around, the same s-separate(n,c) indentation rule as every other
   * continuation line of a flow collection in this file. Here the indent of
   * "a:" is column 0, and the continuation line "c}" also sits at column 0,
   * which is not more indented. Two independent reference parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: {b: &x\nc}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, anchor_continuation_line_indented_enough_still_works) {
  /* A guard for a regression: the same content with one more space of
   * indentation puts the continuation line at column 1, which is more than
   * column 0, where "a:" sits, so the parser accepts it and the value of the
   * anchor makes a correct round trip. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: {b: &x\n c}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_type(a), CYAML_DICTIONARY);
  cyaml b = cyaml_dictionary_get(a, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_type(b), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(b), "c");
  cyaml_destroy(doc);
}

TEST(errors, anchor_flow_continuation_line_tab_indentation_rejected) {
  /* The tab form of
   * anchor_flow_continuation_line_not_indented_enough_rejected. The tab on
   * the continuation line is separation and not indentation, so the line is
   * indented by no space, and the value of "a" needs one. At the root, where
   * the collection needs no indentation, the same line is valid. */
  require_tab_rejected("a: {b: &x\n\tc}\n");
  REQUIRE_TRUE(_tab_accepted_as("{a: &x\n\tc}\n", "{a: c}"));
}

TEST(errors, tag_flow_continuation_line_not_indented_enough_rejected) {
  /* The tag form of
   * anchor_flow_continuation_line_not_indented_enough_rejected: a value with
   * a tag can wrap onto a continuation line inside a flow collection, and
   * when that line is not indented enough, the parser must reject it in the
   * same way. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: {b: !!str\nc}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, tag_continuation_line_indented_enough_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: {b: !!str\n c}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cyaml_type(a), CYAML_DICTIONARY);
  cyaml b = cyaml_dictionary_get(a, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_type(b), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(b), "c");
  cyaml_destroy(doc);
}

TEST(errors, tag_flow_continuation_line_tab_indentation_rejected) {
  require_tab_rejected("a: {b: !!str\n\tc}\n");
  REQUIRE_TRUE(_tab_accepted_as("{a: !!str\n\tc}\n", "{a: c}"));
}

TEST(errors, tab_directly_after_dash_rejected) {
  /* YAML 1.2 section 6.1 says that a tab character is never valid as the
   * indentation or the separation of a block structure, so a bare '-' with a
   * tab directly after it has no valid reading at all, not even as an
   * ordinary plain scalar that starts with '-'. Two independent reference
   * parsers agree and both reject this in every case; the Y79Y-4 case of the
   * vendored YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("-\t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tab_after_dash_and_separator_space_rejected) {
  /* The same restriction as in tab_directly_after_dash_rejected, one step
   * further away: a tab after the one separator space that follows a '-' is
   * just as invalid. Two independent reference parsers agree, and the Y79Y-5
   * case of the vendored YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- \t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tab_directly_after_question_mark_rejected) {
  /* The same restriction as in tab_directly_after_dash_rejected, for the '?'
   * indicator of an explicit key. Two independent reference parsers agree,
   * and the Y79Y-6 case of the vendored YAML Test Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("?\t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tab_after_question_mark_before_key_rejected) {
  /* The same restriction again, for a '?' directly followed by a tab and then
   * a plain scalar key, where the case above has a sequence. Two independent
   * reference parsers agree, and the Y79Y-8 case of the vendored YAML Test
   * Suite matches this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("?\tkey:\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, tab_after_question_mark_before_scalar_key_accepted) {
  /* A tab after a '?', with or without a space before it, is separation in
   * front of a scalar key, for the first entry and for every later one. Only
   * a block collection may not follow the tab (Y79Y-6 and Y79Y-8). */
  REQUIRE_TRUE(_tab_accepted_as("? \ta\n: 1\n", "{a: 1}"));
  REQUIRE_TRUE(_tab_accepted_as("?\ta\n:\t1\n", "{a: 1}"));
}

TEST(block_mapping, tab_after_question_mark_accepted_for_later_entry) {
  /* The loop of the block dictionary reads every later entry without the
   * node dispatch, so it holds the same rule on its own. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\n: 1\n?\tb\n: 2\n? \tc\n: 3\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "c")), 3LL);
  cyaml_destroy(doc);
  require_tab_rejected("? a\n: 1\n?\t- b\n: 2\n");
}

TEST(block_mapping, second_explicit_entry_with_ordinary_space_still_works) {
  /* A guard for a regression: one ordinary space after a '?' on a later
   * entry is not a tab, and the rules above leave it completely alone. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\n: 1\n? b\n: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), (long long)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), (long long)2);
  cyaml_destroy(doc);
}

TEST(block_list, dash_space_content_still_works) {
  /* A guard for a regression: one ordinary space after a '-' is not a tab,
   * and the rules above leave it completely alone. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- a\n- b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "a");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "b");
  cyaml_destroy(doc);
}

TEST(flow_collections, root_level_flow_collection_column_0_continuation_ok) {
  /* A guard for a regression: a flow collection at the root of the document
   * has no block indent around it to obey. Its indent is -1, the sentinel
   * that this whole parser uses, so a continuation line at column 0 is
   * correct there. Two independent reference parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{\"foo\"\n: \"bar\"}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "bar");
  cyaml_destroy(doc);
}

TEST(flow_collections, multiline_flow_list_not_confused_by_unindented_dash) {
  /* A guard for a regression: an ordinary flow list can cover more than one
   * line, and one of its elements can start with a '-' at column 0 on a
   * continuation line without being a "---". The check for a document marker
   * above must leave it alone. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[\n-1,\n-2\n]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc, 0)), -1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc, 1)), -2LL);
  cyaml_destroy(doc);
}

TEST(block_scalars, leading_blank_line_indent_at_or_below_ok) {
  /* A guard for a regression: a blank line at the start can hold FEWER
   * spaces than the block indentation that the parser finds later, or the
   * same number. Both are entirely ordinary, and the parser must not reject
   * them. */
  char *err = NULL;
  cyaml doc = cyaml_parse("scalar: |\n \n  content\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "scalar")),
                "\ncontent\n");
  cyaml_destroy(doc);
}

TEST(errors, hash_immediately_after_comma_is_not_a_valid_element) {
  /* A '#' can never start a plain scalar, in a flow context or anywhere else.
   * A '#' directly after a ',' has no whitespace before it, so it is not a
   * valid comment, because skip_ws_comments needs whitespace before a '#',
   * and it is not valid content either. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[ a, b, c,#invalid\n]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_list_comment_line_does_not_substitute_for_missing_comma) {
  /* The parser steps over a comment line between two elements of a flow
   * sequence, exactly as it steps over any other whitespace or comment, but
   * that comment does not remove the need for a ',' between the two
   * elements. This test also drives a second property: a comment line
   * correctly ends a plain scalar that is running over more than one line,
   * and the scalar does not swallow it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: [ word1\n#  xxx\n  word2 ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, multiline_plain_scalar_stops_before_comment_line) {
  /* A comment line can appear part way through the continuation of a plain
   * scalar over more than one line. The scalar must not take that comment in
   * as content; it ends at the line before the comment. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: word1\n# a comment\nword2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "word1");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "word2")), 2LL);
  cyaml_destroy(doc);
}

TEST(errors, flow_sequence_pair_key_followed_by_newline_then_colon) {
  /* The key of the "[key: value]" shorthand for one pair is an implicit key
   * under ns-s-implicit-yaml-key, so, like any other implicit key, it must
   * fit on one line. A ':' that the parser reaches only after crossing a
   * newline is not this shorthand at all; here the plain scalar of the key
   * continues over lines and folds right up to that ':'.
   *
   * The reference parser PyYAML confirms this on the exact bytes of the
   * vendored fixture. This is a real difference from a KEY over more than
   * one line, which IS valid inside a real "{ }" flow dictionary (see the
   * multiline_flow_dictionary_key test below, beside
   * flow_collections.non_scalar_flow_dictionary_key_canonicalized). The two
   * rules do not conflict. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[ key\n  : value ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_sequence_pair_quoted_key_followed_by_newline_then_colon) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[ \"key\"\n  :value ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_sequence_pair_quoted_key_followed_by_bare_cr_then_colon) {
  /* The bare CR form of
   * flow_sequence_pair_quoted_key_followed_by_newline_then_colon. This parser
   * never normalizes CR and LF in its input, so a "[key ':value]" shorthand
   * can split its key and its ':' across a bare '\r' with no '\n' at all, and
   * the parser must reject it exactly like the version above that uses
   * '\n'. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[ \"key\"\r  :value ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, multiline_flow_dictionary_key) {
  /* The key of a real "{ }" flow dictionary is not held to one line here,
   * while the key of the "[key: value]" sequence shorthand above is. The
   * vendored YAML Test Suite fixture confirms this, and the KNOWN_DEVIATIONS
   * list of this library carries no exception for that fixture, so the
   * parser truly accepts this and it is not a known gap. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{ multi\n  line: value}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml val = cyaml_dictionary_get(doc, "multi line");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(cyaml_str_val(val), "value");
  cyaml_destroy(doc);
}

TEST(flow_collections, explicit_single_pair_entry) {
  /* YAML 1.2 section 7.4.1, Spec Example 7.20, shows "[? key: value]": an
   * entry of one pair in the explicit style that mirrors the bare
   * "[key: value]" shorthand and starts with a '?'. The key can fold across
   * more than one line here, which an implicit key cannot. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[\n? foo\n bar : baz\n]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)1);
  cyaml elem = cyaml_list_get(doc, 0);
  REQUIRE_NE((void *)elem, NULL);
  REQUIRE_EQ(cyaml_type(elem), CYAML_DICTIONARY);
  cyaml foo_bar = cyaml_dictionary_get(elem, "foo bar");
  REQUIRE_NE((void *)foo_bar, NULL);
  REQUIRE_STREQ(cyaml_str_val(foo_bar), "baz");
  cyaml_destroy(doc);
}

TEST(flow_collections, explicit_single_pair_entry_no_value) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[? foo]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml elem = cyaml_list_get(doc, 0);
  REQUIRE_EQ(cyaml_type(elem), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(elem), (size_t)1);
  cyaml foo_val = cyaml_dictionary_get(elem, "foo");
  REQUIRE_NE((void *)foo_val, NULL);
  REQUIRE_EQ(cyaml_type(foo_val), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(flow_collections, explicit_key_entry_in_flow_dictionary) {
  /* YAML 1.2 section 7.4.2 defines ns-flow-map-explicit-entry, in Spec Example
   * 7.16: an explicit '?' can start the key of a flow dictionary entry, just
   * as in the "[? key: value]" shorthand of a flow sequence. The key must be
   * the real content after the '?', not the "? " text itself, and a bare "?"
   * at the end with nothing else gives a null key and a null value. A
   * reference parser agrees. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("{\n? explicit: entry,\nimplicit: entry,\n?\n}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)3);
  cyaml explicit_val = cyaml_dictionary_get(doc, "explicit");
  REQUIRE_NE((void *)explicit_val, NULL);
  REQUIRE_STREQ(cyaml_str_val(explicit_val), "entry");
  cyaml implicit_val = cyaml_dictionary_get(doc, "implicit");
  REQUIRE_NE((void *)implicit_val, NULL);
  REQUIRE_STREQ(cyaml_str_val(implicit_val), "entry");
  cyaml null_val = cyaml_dictionary_get(doc, "null");
  REQUIRE_NE((void *)null_val, NULL);
  REQUIRE_EQ(cyaml_type(null_val), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(errors, chained_implicit_mapping_values_rejected) {
  /* Content on the same line as an ordinary implicit value cannot open a
   * further nested mapping, so the document "a: b: c: d" has no valid
   * reading under YAML 1.2 section 8.2.2: the ns-l-block-map-implicit-value
   * rule has no "compact mapping" choice, and a sequence value has none there
   * either. The reference parser PyYAML agrees and reports "mapping values
   * are not allowed here". */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: b: c: d\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_quoted_key_rejected) {
  /* The same restriction as in chained_implicit_mapping_values_rejected,
   * where a single-quoted scalar is the inner key of the chain. The
   * restriction holds for every form of key: a plain scalar, a quoted scalar
   * and a flow collection. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 'b': c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_flow_collection_key_rejected) {
  /* The same restriction again, where a flow collection is the inner key of
   * the chain; such a collection is a valid key that is not a scalar, under
   * the "compact mapping" form of YAML 1.2 section 8.2.2. The parser rejects
   * "a: [1,2]: c" exactly as it rejects "a: b: c". PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: [1,2]: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_under_sequence_element_rejected) {
  /* The restriction also reaches through the compact mapping of a sequence
   * element: the parser rejects "- a: b: c" exactly as it rejects the bare
   * "a: b: c", because the dash starts a compact mapping, and inside it the
   * value "b: c" of "a" is an ordinary implicit value. PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- a: b: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_after_same_line_anchor_rejected) {
  /* The restriction also reaches through an anchor on the same line that sits
   * on the inner key of the chain: the parser rejects "a: &x b: c" exactly as
   * it rejects the same document with no anchor. PyYAML agrees. The parser
   * passes allow_inline_map along instead of setting that flag back to true
   * when the content of an anchor stays on the same line as the anchor
   * itself. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x b: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_explicit_key_form_rejected) {
  /* The restriction also reaches through the explicit '?' form of a key: the
   * document "a: ? b\n   : c" has no valid reading either, although the
   * explicit form is perfectly legal once the parse is already inside
   * content in the explicit style. PyYAML agrees and reports "mapping keys
   * are not allowed here".
   *
   * Every other dispatch that asks "can a fresh mapping open here" already
   * holds this rule, for a plain scalar key, a quoted scalar key and a flow
   * collection key, and the explicit '?' form must hold it as well. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: ? b\n   : c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, anchor_on_own_line_still_permits_nested_mapping_value) {
  /* A value that starts on its own line, more indented, always keeps the
   * right to open a fresh mapping there, whether or not an anchor sits on
   * that value, so the document "top: &x\n  b: c\n" must nest in the normal
   * way. PyYAML agrees. Only a chain on the SAME line, after an implicit
   * value that is already open, is ever restricted, never the content of an
   * anchor on a later line. */
  char *err = NULL;
  cyaml doc = cyaml_parse("top: &x\n  b: c\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml top = cyaml_dictionary_get(doc, "top");
  REQUIRE_EQ(cyaml_type(top), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(top, "b")), "c");
  cyaml_destroy(doc);
}

TEST(block_mapping, tag_on_own_line_still_permits_nested_mapping_value) {
  /* The same rule as above, with a tag in place of the anchor: a value on its
   * own line always opens a fresh mapping, so the document
   * "top: !!map\n  b: c\n" must nest in the normal way. PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("top: !!map\n  b: c\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml top = cyaml_dictionary_get(doc, "top");
  REQUIRE_EQ(cyaml_type(top), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(top, "b")), "c");
  cyaml_destroy(doc);
}

TEST(block_mapping, explicit_value_still_permits_chained_nested_mapping) {
  /* Value content in the explicit style keeps the same "compact" right for a
   * nested mapping that it already has for a nested sequence, so the document
   * "? k\n: a: b" nests in the normal way, unlike the implicit "a: b: c" case
   * above. PyYAML agrees: it accepts this document and rejects the implicit
   * form. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? k\n: a: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml k = cyaml_dictionary_get(doc, "k");
  REQUIRE_EQ(cyaml_type(k), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(k, "a")), "b");
  cyaml_destroy(doc);
}

TEST(block_mapping, value_on_later_line_not_fooled_by_trailing_comment) {
  /* A value can start on its own line, with more indent, and the parser must
   * read it as such when a comment stands directly after the ':' of the key,
   * because that comment is not a newline. The document
   * "a: # comment\n  b: c\n" is an ordinary nested mapping, not a chain on
   * the same line, so it must open a fresh mapping. PyYAML agrees. This is
   * why rest_of_line_is_blank exists: a simple check with at_eol() decides
   * "same line" against "later line" wrongly when a comment trails the
   * key. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: # comment\n  b: c\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_EQ(cyaml_type(a), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(a, "b")), "c");
  cyaml_destroy(doc);
}

TEST(block_mapping, sibling_entry_after_anchor_trailing_comment_not_swallowed) {
  /* The same hazard with a trailing comment as above, here for an anchor that
   * has nothing else of its own. In "a: &x # comment\nb: 2\n", the parser
   * must see "b: 2" as a SIBLING entry at the same indent as "a", not as the
   * value of the anchor: an anchor must not swallow its sibling, and that
   * guard must hold when a comment, instead of a newline, comes directly
   * after the anchor name. PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x # comment\nb: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  cyaml a_val = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)a_val, NULL);
  REQUIRE_EQ(cyaml_type(a_val), CYAML_NULL);
  cyaml b_val = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b_val, NULL);
  REQUIRE_EQ(cyaml_int_val(b_val), 2LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, tagged_value_on_later_line_after_trailing_comment) {
  /* The same hazard with a trailing comment as in
   * value_on_later_line_not_fooled_by_trailing_comment, here for a tag
   * instead of a bare value: the document "a: !!map # comment\n  b: c\n"
   * nests in the normal way. PyYAML agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!map # comment\n  b: c\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml a = cyaml_dictionary_get(doc, "a");
  REQUIRE_EQ(cyaml_type(a), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(a, "b")), "c");
  cyaml_destroy(doc);
}

TEST(errors, bare_dash_in_flow_sequence_rejected) {
  /* YAML 1.2 section 6.6 defines ns-plain-first(c): a '-' can start a plain
   * scalar only when it is directly followed by an ns-plain-safe(c)
   * character, one that can itself legally appear WITHIN the scalar. A flow
   * indicator, ']' here, is not one, so a lone "-" has no valid reading as an
   * element of a flow sequence. The YJV2 case of the vendored YAML Test Suite
   * confirms this, although some other implementations are less strict about
   * it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[-]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, bare_dashes_separated_by_comma_in_flow_sequence_rejected) {
  /* The same restriction as in bare_dash_in_flow_sequence_rejected, for two
   * lone dashes with a comma between them: each "-" is directly followed by a
   * flow indicator (a ',' and then a ']'), so neither dash has a valid
   * reading. The G5U8 case of the vendored YAML Test Suite confirms this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[-, -]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, dash_followed_by_plain_content_still_valid) {
  /* A '-' directly followed by ordinary content is perfectly valid in a flow
   * context, because that content is safe inside a plain scalar and no
   * whitespace stands before a flow indicator; a bare "-" is the case that
   * fails. This test guards against at_valid_flow_plain_scalar_start being
   * too strict: in a flow context, "-1" and "-foo" are ordinary scalars, not
   * sequence indicators. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[-1, -foo]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 0)), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(doc, 0)), -1LL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "-foo");
  cyaml_destroy(doc);
}

TEST(flow_collections, bare_colon_key_shorthand_still_works) {
  /* A guard for the deliberate exclusion of ':' in
   * at_valid_flow_plain_scalar_start (see the doc comment of that function):
   * an empty implicit key that a bare ':' starts, inside a flow sequence,
   * must keep working. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[: empty key]", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml elem = cyaml_list_get(doc, 0);
  REQUIRE_EQ(cyaml_type(elem), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(elem, "null")), "empty key");
  cyaml_destroy(doc);
}

TEST(errors, bare_question_mark_in_flow_sequence_rejected) {
  /* The same ns-plain-first(c) restriction as for '-', here for '?': a lone
   * "?" directly followed by a flow indicator has no valid reading as a
   * plain scalar. This is not the "? key" syntax for an explicit pair, which
   * needs whitespace after the '?', not a flow indicator. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[?]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, scalar_with_two_stacked_anchors_rejected) {
  /* The c-ns-properties rule permits at most one anchor for each node. The
   * text "&node2\n  &v2 val2" puts a SECOND bare anchor directly around the
   * same scalar value, with nothing else between them and no key, which has
   * no valid reading. Two independent reference parsers agree: both reject a
   * scalar with two anchors around it, split across a newline here. */
  char *err = NULL;
  cyaml doc = cyaml_parse("top2: &node2\n  &v2 val2\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, scalar_with_two_stacked_anchors_same_line_rejected) {
  /* The same restriction as in scalar_with_two_stacked_anchors_rejected, with
   * both anchors on one line instead of split across a newline. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&a &b val\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, two_stacked_tags_rejected) {
  /* The same restriction as in scalar_with_two_stacked_anchors_rejected, for
   * two tags instead of two anchors, since the c-ns-properties rule also
   * permits at most one tag for each node. A reference parser agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str !!int 5\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, anchor_decorating_a_mapping_whose_key_is_also_anchored) {
  /* A guard for a regression that separates two shapes. The first puts two
   * anchors on the SAME node and is invalid (see
   * scalar_with_two_stacked_anchors_rejected); the second puts an anchor on a
   * mapping whose own first key also carries an anchor, and since the key is
   * a completely different node, this shape is valid.
   *
   * The document "top1: &node1\n  &k1 key1: val1\n" anchors the mapping
   * {key1: val1} as node1 and, separately, the key scalar "key1" as k1. Two
   * independent reference parsers agree: both accept this, and both reject
   * the shape with two anchors above. */
  char *err = NULL;
  cyaml doc = cyaml_parse("top1: &node1\n  &k1 key1: val1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml top1 = cyaml_dictionary_get(doc, "top1");
  REQUIRE_EQ(cyaml_type(top1), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(top1, "key1")), "val1");
  cyaml_destroy(doc);
}

/* A dictionary key can carry an anchor, and an alias to that anchor must
 * resolve to the real type and structure of the key, not to a plain
 * CYAML_STRING that the library builds from the canonical text of the key.
 * Anything else would disagree with an anchor in a value position, which
 * always keeps the real type of the anchored node.
 *
 * A plain-scalar key with an implicit type and an anchor, such as "&n 42:",
 * must alias back to the real integer, and a flow-collection key that is not a
 * scalar, such as "&x [1, 2]:", must alias back to the real list; neither may
 * alias to a string of its canonical text. A quoted-scalar key is not touched
 * by this, because its own natural value is already a string. */
TEST(block_mapping, anchored_plain_scalar_key_alias_preserves_implicit_type) {
  char *err = NULL;
  cyaml doc = cyaml_parse("&n 42: v\nlater: *n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml later = cyaml_dictionary_get(doc, "later");
  REQUIRE_NE((void *)later, NULL);
  REQUIRE_EQ(cyaml_type(later), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(later), 42LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_flow_collection_key_alias_preserves_structure) {
  char *err = NULL;
  cyaml doc = cyaml_parse("&x [1, 2]: v\nlater: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml later = cyaml_dictionary_get(doc, "later");
  REQUIRE_NE((void *)later, NULL);
  REQUIRE_EQ(cyaml_type(later), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(later), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(later, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(later, 1)), 2LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_quoted_scalar_key_alias_still_a_string) {
  /* The behaviour above does not touch this case: the natural value of a
   * quoted scalar is always a string, and it matches the key text exactly. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&q \"42\": v\nlater: *q\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml later = cyaml_dictionary_get(doc, "later");
  REQUIRE_NE((void *)later, NULL);
  REQUIRE_EQ(cyaml_type(later), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(later), "42");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_plain_scalar_key_as_later_entry_preserves_type) {
  /* Same rule, exercised through parse_one_dict_entry_key (the key is not
   * the mapping's first entry) rather than parse_node's own '&' fast
   * path. */
  char *err = NULL;
  cyaml doc = cyaml_parse("first: 1\n&b true: v\nlater: *b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml later = cyaml_dictionary_get(doc, "later");
  REQUIRE_NE((void *)later, NULL);
  REQUIRE_EQ(cyaml_type(later), CYAML_BOOL);
  REQUIRE_TRUE(cyaml_bool_val(later));
  cyaml_destroy(doc);
}

TEST(block_mapping,
     tag_and_anchor_decorated_key_alias_resolves_to_the_tagged_node) {
  /* This test drives the fast path of the '!' dispatch, with a key that
   * carries both a tag and an anchor. The anchor names the node of the key as
   * its tag types it, exactly as an anchor in a value position does:
   * "!!str 3.5" is the string "3.5", and an untagged "3.5" is the float 3.5. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("!!str &f 3.5: v\nlater: *f\n&g 3.5: w\nother: *g\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml later = cyaml_dictionary_get(doc, "later");
  REQUIRE_NE((void *)later, NULL);
  REQUIRE_EQ(cyaml_type(later), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(later), "3.5");
  REQUIRE_STREQ(cyaml_node_tag(later), CYAML_TAG_STR);
  cyaml other = cyaml_dictionary_get(doc, "other");
  REQUIRE_NE((void *)other, NULL);
  REQUIRE_EQ(cyaml_type(other), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(other), 3.5);
  cyaml_destroy(doc);
}

TEST(errors, two_stacked_anchors_same_line_decorating_a_key_rejected) {
  /* Neither scalar_with_two_stacked_anchors_same_line_rejected nor
   * anchor_decorating_a_mapping_whose_key_is_also_anchored covers this case
   * on its own: two anchors stand on the SAME line, and what follows looks
   * like a valid compact-mapping key ("&a &b foo: bar").
   * Because the content of the second anchor can look like a valid key, the
   * "is this an anchored key" fast path must not take priority over the
   * ordinary had_anchor rejection there. Without that rule, the parser
   * silently accepts this document, which two independent reference parsers
   * reject in the same way as the same-line case that has no key. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&a &b foo: bar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, two_stacked_tags_same_line_decorating_a_key_rejected) {
  /* Same restriction as two_stacked_anchors_same_line_decorating_a_key_
   * rejected above, for two tags instead of two anchors. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str !!int foo: bar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, anchor_decorating_a_bare_alias_rejected) {
  /* c-ns-alias-node is its own top-level alternative in the grammar of
   * ns-flow-node, separate from the c-ns-properties branch that an anchor or
   * a tag decorates, so an alias can never carry a property at all. Two
   * independent reference parsers agree. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key1: &a value\nkey2: &b *a\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tag_decorating_a_bare_alias_rejected) {
  /* The same restriction as anchor_decorating_a_bare_alias_rejected, for a
   * named tag in place of an anchor. A reference parser agrees. The target of
   * the alias ("&n") is deliberately defined and otherwise valid, so this
   * document fails on the tag-on-alias restriction itself, not on an
   * unrelated "unknown alias" error that a weakened check would still
   * produce. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key1: &n value\nkey2: !!int *n\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, alias_used_as_key_still_works) {
  /* Regression guard: the restriction that a bare alias cannot carry a
   * property must not affect an alias that stands AS A KEY
   * ("*alias: value"). That is a completely different construct, which the
   * library supports through the fast path of try_parse_scalar_dict_key, and
   * no property decorates the alias there. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\n*x : bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "foo");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "bar");
  cyaml_destroy(doc);
}

TEST(errors, comma_directly_after_tag_rejected) {
  /* ns-tag-char excludes every c-flow-indicator character everywhere, not
   * only in a flow context, so a comma that sits directly at the end of a
   * shorthand tag, with no whitespace between them, has no valid meaning as
   * content. A reference parser agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- !!str, xxx\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_indicator_directly_after_anchor_rejected) {
  /* The same restriction as comma_directly_after_tag_rejected, here with an
   * anchor name directly against a flow indicator, with no whitespace between
   * them. A reference parser agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- &a[1,2]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

/* A '[' or a '{' has no valid meaning directly after the name of an anchor
 * or a tag, in ANY context, because nothing in the grammar lets a name carry
 * a brand new nested collection with no separator between them, so the
 * restriction on a glued flow indicator must not be gated on "!in_flow".
 * A gate of that shape silently accepts an anchor or a tag glued directly
 * onto the opener of a NESTED flow collection, one that sits inside an
 * enclosing flow collection, while the parser correctly rejects the identical
 * construct in block context. With the gate, "[&a{x: 1}, *a]" parses: the
 * anchor
 * decorates the nested map, and the alias resolves to a clone of it, which
 * is a real effect on the meaning, not merely trailing text that nothing
 * consumes. A reference parser rejects the document.
 * A ',', a ']' and a '}' are the contrast: each stays legitimate flow
 * collection structure directly after an anchor or a tag INSIDE a flow
 * collection (see the two _still_works guards below), and each is illegal in
 * block context alone, where it has no valid meaning at all. */
TEST(errors, anchor_directly_before_nested_flow_map_rejected_in_flow_context) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[&a{x: 1}, *a]", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, anchor_directly_before_nested_flow_list_rejected_in_flow_context) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[&a[1,2], *a]", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tag_directly_before_nested_flow_list_rejected_in_flow_context) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[!!seq[1,2]]", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_list, anchor_followed_by_comma_still_works_in_flow_context) {
  /* Regression guard: an anchor can stand directly before a flow TERMINATOR,
   * a ',' or a ']', which is not the OPENER of a nested collection but
   * legitimate flow collection structure, and the parser must keep accepting
   * it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[&a, b]", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_NE((void *)cyaml_list_get(doc, 0), NULL);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 0)), CYAML_NULL);
  REQUIRE_NE((void *)cyaml_list_get(doc, 1), NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "b");
  cyaml_destroy(doc);
}

TEST(flow_list, tag_followed_by_comma_still_works_in_flow_context) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[!!str, x]", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "x");
  cyaml_destroy(doc);
}

TEST(block_list, tag_followed_by_space_then_comma_content_still_works) {
  /* Regression guard: the restriction on a glued flow indicator must not
   * affect a tag separated from its content by real whitespace. That content
   * here is a plain scalar that holds a comma, which is valid in block
   * context, where a ',' is not a terminator. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- !!str a, b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "a, b");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchor_and_tag_combo_still_works_either_order) {
  /* Regression guard: the restriction on two stacked properties must not
   * affect the ordinary, valid pair of an anchor and a tag, one of each, in
   * either order. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x !!str foo\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "foo");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "foo");
  cyaml_destroy(doc);

  err = NULL;
  doc = cyaml_parse("a: !!str &x foo\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "foo");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "foo");
  cyaml_destroy(doc);
}

/* ==========================================================================
 * Differential-testing coverage, which you can reproduce with
 * tests/cyaml/differential/compare_pyyaml.py against the vendored YAML Test
 * Suite and the fuzz corpus. A second reference parser, the Psych of Ruby,
 * independently confirms every expectation below.
 * ========================================================================== */

TEST(multi_document, two_consecutive_directive_documents_without_end_marker) {
  /* Two documents back to back, with no "..." end marker between them, each
   * carrying its own %YAML directive. The l-yaml-stream grammar of YAML 1.2
   * section 6.9 lets a document follow directly, with no "...", only through
   * its own l-explicit-document alternative, which excludes a document that
   * carries a directive, because l-directive-document is a separate
   * top-level alternative. This construct is therefore a real parse error,
   * not two accepted empty documents. The "is this document empty" check of
   * the first document must treat a fresh directive line as a document
   * boundary; without that, the parser silently reads the directive text of
   * the second document as the scalar content of the first one and never
   * reaches this error. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n---\n%YAML 1.2\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(multi_document, directive_document_after_end_marker_still_works) {
  /* Regression guard: a document that carries a directive IS permitted
   * directly after an explicit "..." end marker, unlike the case above, which
   * has no marker. This must keep working. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n---\na\n...\n%YAML 1.2\n---\nb\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "a");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "b");
  cyaml_destroy(doc);
}

TEST(multi_document, trailing_comment_after_start_marker_still_works) {
  /* A trailing "# comment" directly after "---" is not "same-line content",
   * so doc_marker_same_line_content must look past it; a check of at_eol
   * alone is not enough, because a comment is not itself a newline. Without
   * that, the parser wrongly reads a later block mapping or block sequence
   * as invalid same-line content and rejects it with "mapping values are not
   * allowed here", or with "a block sequence cannot start on the same
   * line...". */
  char *err = NULL;
  cyaml doc = cyaml_parse("---  # comment\na: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  cyaml_destroy(doc);

  err = NULL;
  doc = cyaml_parse("---  # comment\n- a\n- b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "a");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "b");
  cyaml_destroy(doc);
}

TEST(multi_document, tab_after_start_marker_accepted) {
  /* The separator of a '---' before same-line content may hold tabs
   * (K54U). parse_one_document reads the marker at two call sites, after a
   * directive and without one; both are covered. */
  REQUIRE_TRUE(_tab_accepted_as("---\tfoo\n", "foo"));
  REQUIRE_TRUE(_tab_accepted_as("--- \tfoo\n", "foo"));
  REQUIRE_TRUE(_tab_accepted_as("%YAML 1.2\n---\tfoo\n", "foo"));
  REQUIRE_TRUE(_tab_accepted_as("---\t[a]\n", "[a]"));
  require_tab_rejected("---\t- a\n");
}

TEST(multi_document, tab_after_end_marker) {
  /* Spaces and tabs may separate a '...' from a comment, while content after
   * the marker is an error whatever separates it. */
  REQUIRE_TRUE(_tab_accepted_as("--- x\n...\t# c\n", "x"));
  REQUIRE_TRUE(_tab_accepted_as("--- x\n... \t# c\n", "x"));
  require_tab_rejected("a: 1\n...\tb\n");
  require_tab_rejected("a: 1\n... \tb\n");
}

TEST(multi_document, space_after_start_and_end_marker_still_works) {
  /* An ordinary space can separate after a '---' or a '...'. */
  char *err = NULL;
  cyaml doc = cyaml_parse("---   foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(doc), "foo");
  cyaml_destroy(doc);

  err = NULL;
  doc = cyaml_parse("a: 1\n...   \nb: 2\n...\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(cyaml_list_get(doc, 0), "a")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(cyaml_list_get(doc, 1), "b")), 2LL);
  cyaml_destroy(doc);
}

TEST(multi_document, bare_start_marker_then_directive_document_is_error) {
  /* Here a bare "---", with nothing else on its line and no "..." suffix,
   * stands before a document that carries a directive. This is the same
   * class of construct as
   * two_consecutive_directive_documents_without_end_marker above. The
   * l-yaml-stream grammar of YAML 1.2 section 6.9 says that a document that
   * follows one with no "..." suffix may only be an l-explicit-document,
   * which is a bare "---" with no directives, while l-directive-document is
   * reachable only as l-any-document, behind a "..." in front of it. A
   * directive can never follow the leading "---" that a document has already
   * consumed, because a directive only ever stands before the "---" that it
   * configures. This is therefore a real parse error, not a stream of two
   * documents. The "is this document empty" check of the empty first
   * document must run here too: without it, the loop that parses directives
   * runs directly after the leading "---", silently absorbs the whole
   * "%directive\n---\ncontent" of the second document as trailing content of
   * the first one, and the parser never reaches this error. */
  char *err = NULL;
  cyaml doc = cyaml_parse("---\n%YAML 1.2\n---\nfoo\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

/* A literal or folded block scalar can have an indent of zero, so that its
 * content sits flush at column 0, which is the indent that the parser detects
 * at the document root. Such a scalar cannot tell a real "---" or "..."
 * document marker at column 0 from an ordinary content line at column 0 by
 * the indent alone, because both have spaces == 0 == block_indent.
 * scan_block_scalar_line() therefore checks at_doc_marker() explicitly, like
 * every other parser of block content in this file; without that check it
 * absorbs the marker as scalar content instead of ending the scalar and the
 * document. A block scalar with a positive indent never needs the check,
 * because a marker there always has fewer leading spaces than block_indent,
 * so it already ends the scalar. */
TEST(multi_document, zero_indented_literal_scalar_ends_at_document_marker) {
  char *err = NULL;
  cyaml doc = cyaml_parse("|\nfoo\n---\nbar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "foo\n");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "bar");
  cyaml_destroy(doc);
}

TEST(multi_document, zero_indented_folded_scalar_ends_at_document_marker) {
  char *err = NULL;
  cyaml doc = cyaml_parse(">\nfoo\n---\nbar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "foo\n");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "bar");
  cyaml_destroy(doc);
}

TEST(multi_document, zero_indented_literal_scalar_ends_at_end_marker) {
  char *err = NULL;
  cyaml doc = cyaml_parse("|\nfoo\n...\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "foo\n");
  cyaml_destroy(doc);
}

TEST(multi_document,
     zero_indented_literal_keep_scalar_ends_at_document_marker) {
  /* The same hazard with an explicit KEEP chomp indicator ("|+"), which
   * confirms that the rule holds whatever the chomp mode is. */
  char *err = NULL;
  cyaml doc = cyaml_parse("|+\nfoo\n---\nbar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "foo\n");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "bar");
  cyaml_destroy(doc);
}

TEST(multi_document, indented_literal_scalar_still_ends_at_document_marker) {
  /* A control case: a block scalar with an indent past column 0 needs no
   * explicit marker check of its own, because the 0 leading spaces of a
   * marker are always fewer than a positive block_indent, which already ends
   * the scalar. This test guards that the check at column 0 above leaves
   * that case alone. */
  char *err = NULL;
  cyaml doc = cyaml_parse("|\n  foo\n---\nbar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "foo\n");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "bar");
  cyaml_destroy(doc);
}

TEST(quoted, double_quoted_blank_line_with_only_a_tab_folds_to_newline) {
  /* A line between two content lines can hold inline whitespace alone, a
   * single tab here, and such a line is a real blank line when the parser
   * folds a double-quoted scalar, so it must fold to a newline, not to a
   * space (see YAML 1.2 section 6.5, spec example 6.5). A bare check of
   * cur(ctx) == '\n' or '\r', directly after the parser reads the first
   * newline, is not enough, because it cannot see past the tab to the real
   * newline behind it and folds this line to a space. */
  const char *yaml = "\"Empty line\n \t\nas a line feed\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "Empty line\nas a line feed");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_blank_line_with_only_spaces_folds_to_newline) {
  /* The same rule on the single-quoted side: a blank continuation line that
   * carries its own trailing spaces must fold to a newline. */
  const char *yaml = "'foo\n \nbar'\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "foo\nbar");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escaped_tab_before_fold_is_preserved) {
  /* An escaped tab ("\t") directly before a line break is real content that
   * the author asked for, not incidental whitespace of the source, so the
   * "strip trailing whitespace before a fold" rule of YAML 1.2 section 8.1.2
   * must not trim it; that rule covers only literal, unescaped whitespace
   * that the parser copies straight from the source. A trim loop with no
   * condition silently drops the escaped tab. */
  const char *yaml = "\"a\\t\nb\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "a\t b");
  cyaml_destroy(n);
}

TEST(quoted,
     double_quoted_literal_trailing_spaces_after_escape_still_stripped) {
  /* The fold must strip ordinary literal trailing whitespace AFTER an escape
   * as usual; only the bytes that the escape itself produced are
   * protected. */
  const char *yaml = "\"a\\t  \nb\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "a\t b");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_keep_blank_line_excess_indentation_preserved) {
  /* A blank line can have more indent than the block itself, and it then
   * keeps its own excess spaces as literal content, exactly like a line that
   * is not blank, because the "more indented lines" rule of YAML 1.2 section
   * 8.1.1.2 makes no exception for a blank line. This holds when that line
   * is the very last line of the scalar under CHOMP_KEEP. Code that always
   * discards the indent of a blank line loses every space past the block
   * indent. */
  const char *yaml = "|+\n ab\n \n  \n...\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "ab\n\n \n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_clip_mid_content_blank_excess_indentation) {
  /* The same rule for a blank line in the middle of the content instead of
   * at the end, under CHOMP_CLIP: the excess indent must survive, and the
   * scalar carries more real content after that line. */
  const char *yaml = "text: |\n  a\n    \n  b\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "text")), "a\n  \nb\n");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_keep_blank_line_excess_indentation_preserved) {
  /* The identical rule in the folded (">") style: a blank line in a folded
   * scalar keeps its excess indent, exactly like a blank line in a literal
   * scalar. */
  const char *yaml = "foo: >+\n  x\n   \n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n \n");
  cyaml_destroy(doc);
}

TEST(block_scalars, literal_clip_no_source_trailing_newline_omits_final_lf) {
  /* The b-chomped-last grammar production of YAML 1.2 governs the very last
   * line of a scalar under every chomp mode, CLIP and KEEP included, and it
   * reads "b-as-line-feed | <end of file>". When the input ends right after
   * the last line of the scalar, the source holds no real line break there,
   * so the parser adds no newline on account of that line, even under CLIP,
   * which otherwise always appends exactly one. CLIP must not add a trailing
   * '\n' whenever there is any content: whether the source itself carried a
   * final line break decides that. This test uses cyaml_parse_n instead of
   * cyaml_parse, which needs a NUL-terminated string, so parse_n sees
   * exactly the missing trailing newline of the input rather than an
   * artifact of a C string literal, which always carries an implicit NUL
   * past its last byte. */
  const char *yaml = "foo: |\n  x\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n ");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_keep_no_source_trailing_newline_omits_final_lf) {
  /* The identical b-chomped-last rule, in the folded (">") style under
   * CHOMP_KEEP. */
  const char *yaml = "foo: >+\n  x\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n ");
  cyaml_destroy(doc);
}

TEST(block_scalars, literal_keep_leading_blank_only_no_source_newline) {
  /* The same rule against a synthetic newline holds in one more case, where
   * the ENTIRE content of the scalar is a single leading blank line, the
   * parser finds no real content line at all, and the source has no
   * trailing newline either. The result must be a truly empty string, not a
   * bare "\n". */
  const char *yaml = "- |+\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "");
  cyaml_destroy(doc);
}

TEST(block_list, dash_followed_only_by_comment_is_a_null_entry) {
  /* A block sequence entry can hold a '-' and a trailing comment alone, with
   * no scalar value, and such an entry must be a null entry, exactly like a
   * bare '-' with nothing at all after it. The parser must never hand the
   * text of that comment to parse_node() as real content, because with that
   * content parse_node() walks past the comment and swallows every later
   * sibling entry as nested content instead of separate siblings. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- # Empty\n- two\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  cyaml elem0 = cyaml_list_get(doc, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_type(elem0), CYAML_NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "two");
  cyaml_destroy(doc);
}

TEST(block_list, dash_followed_only_by_tag_is_an_empty_string_entry) {
  /* The same class of hazard, through a tag instead of a comment: the line
   * "- !!str" carries nothing else, and the tag branch of parse_node() must
   * not recurse past the newline and swallow the next sibling entry as the
   * value of this tag. The tag branch carries the same at_block_value_col()
   * guard as the '&' branch for an anchor. The entry itself resolves to an
   * empty string, not to null, because !!str forces CYAML_STRING even on
   * empty text. */
  char *err = NULL;
  cyaml doc = cyaml_parse("- !!str\n- two\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 0)), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "");
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "two");
  cyaml_destroy(doc);
}

TEST(block_mapping, key_with_tag_only_value_is_an_empty_string_entry) {
  /* The same rule for the tag branch, in the position of a dictionary value
   * instead of a sequence entry. The line "a: !!str" carries nothing else,
   * and it must leave "a" mapped to an empty string, because !!str forces
   * CYAML_STRING even on empty text, and leave "b" as a truly separate
   * sibling key: the parser must not swallow "b: two" as the tagged value of
   * "a". */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!str\nb: two\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "a")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "two");
  cyaml_destroy(doc);
}

/* ==========================================================================
 * A full audit of the tab as indent, made with the same differential testing
 * tool as above, which probes every decision about indent in the parser that
 * current_col() drives. The cases below separate two groups: the positions
 * that truly need a rejection, and ordinary, legitimate same-line separator
 * whitespace, or content that is already open.
 * ========================================================================== */

TEST(errors, tab_as_pure_block_mapping_indentation_rejected) {
  /* 4EJS: tabs are the SOLE indent for nested block mapping entries, with no
   * space character at all. */
  require_tab_rejected("---\na:\n\tb:\n\t\tc: value\n");
}

TEST(errors, tab_as_pure_block_sequence_indentation_rejected) {
  /* The sequence form of 4EJS. */
  require_tab_rejected("-\n\t- a\n\t- b\n");
}

TEST(errors, tab_at_start_of_document_rejected) {
  /* A tab as the very first byte of the whole input, before any real document
   * content: the top-level skip of parse_common consumes it before
   * parse_one_document or parse_node ever see it. */
  require_tab_rejected("\ta: 1\n");
}

TEST(errors, tab_before_anchor_after_fresh_line_rejected) {
  /* A tab as the indent of a fresh line, directly before a node property,
   * here an anchor that decorates the value of a sequence. */
  require_tab_rejected("-\n\t&x val\n");
}

TEST(errors, tab_after_anchor_same_line_still_works) {
  /* Regression guard: a tab can be ordinary SAME-LINE separator whitespace
   * between an anchor and its own inline value, which is legitimate because
   * s-separate-in-line permits it, so the tab check above, which covers a
   * fresh line alone, must not reject it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: &x\tvalue\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(errors, tab_on_flow_collection_continuation_line_rejected) {
  /* Y79Y-3: a tab is the indent on the continuation line of a flow list, a
   * line that crosses a newline inside "[...]" content that is open. */
  require_tab_rejected("- [\n\tfoo,\n foo\n ]\n");
}

TEST(errors, tab_on_flow_dictionary_continuation_line_rejected) {
  /* The flow dictionary counterpart to
   * tab_on_flow_collection_continuation_line_rejected above.
   * parse_flow_dictionary is a sibling of parse_flow_list that somebody wrote
   * out by hand instead of sharing one implementation, so the same tab check
   * for a continuation line needs its own regression coverage on the "{...}"
   * side, confirmed on its own; the flow list test alone does not cover
   * it. */
  require_tab_rejected("- {\n\tfoo: 1,\n bar: 2\n }\n");
}

TEST(errors, tab_on_flow_collection_continuation_line_rejected_bare_cr) {
  /* The bare CR counterpart to
   * tab_on_flow_collection_continuation_line_rejected. This parser never
   * normalizes CR and LF in its input, so a document that uses bare '\r'
   * line breaks alone, with no '\n' at all, must trigger the identical tab
   * rejection on a continuation line as the version above that uses LF.
   * flow_skip_ws() decides whether the text really crossed a line by asking
   * span_crosses_newline(), which must treat a bare '\r' exactly as it
   * treats a '\n'. */
  require_tab_rejected("- [\r\tfoo,\r foo\r ]\r");
}

TEST(errors, tab_on_flow_dictionary_continuation_line_rejected_bare_cr) {
  /* The bare CR counterpart to
   * tab_on_flow_dictionary_continuation_line_rejected. See the bare CR test
   * for the flow list above for the reasoning. */
  require_tab_rejected("- {\r\tfoo: 1,\r bar: 2\r }\r");
}

TEST(block_mapping, tab_after_colon_value_indicator) {
  /* A tab after a ':' is separation in front of a scalar or a flow
   * collection ("key:\tvalue", the YAML Test Suite case 6BCT, and the JSON
   * that Go's json.MarshalIndent writes with a tab indent), but a block
   * collection may not follow it: Y79Y-7 and Y79Y-9. This test is
   * non-vacuous: a parser that refuses every tab after ':' fails the
   * accepted cases. */
  REQUIRE_TRUE(_tab_accepted_as("key:\tvalue\n", "{key: value}"));
  REQUIRE_TRUE(_tab_accepted_as("key: \tvalue\n", "{key: value}"));
  REQUIRE_TRUE(_tab_accepted_as("key:\t[1, 2]\n", "{key: [1, 2]}"));
  REQUIRE_TRUE(_tab_accepted_as("key:\t\"q\"\t# c\n", "{key: q}"));
  REQUIRE_TRUE(_tab_accepted_as("block:\t|\n  x\n", "{block: \"x\\n\"}"));
  require_tab_rejected("? key:\n:\tkey:\n");
  require_tab_rejected("? -\n:\t-\n");
  require_tab_rejected("key:\tb: c\n");
  require_tab_rejected("key:\t- a\n");
}

TEST(block_mapping, tab_after_colon_before_newline_value) {
  /* A tab between a ':' and the end of its line is trailing separation, and
   * the value on the next line keeps its ordinary meaning (DC7X). */
  REQUIRE_TRUE(_tab_accepted_as("seq:\t\n - a\n", "{seq: [a]}"));
  REQUIRE_TRUE(_tab_accepted_as("a:\t# c\n  b: 1\n", "{a: {b: 1}}"));
}

TEST(block_mapping,
     explicit_key_value_split_across_tab_indented_lines_rejected) {
  /* An explicit pair of "? key" and ": value" where BOTH the key and the
   * value sit on their own continuation lines, with a tab as the indent. */
  require_tab_rejected("?\n\tkey\n:\n\tvalue\n");
}

TEST(block_mapping, explicit_key_value_indicator_tab_indented_rejected) {
  /* A narrower case than the one above: a tab indents only the line of the
   * ':' value indicator, while a space validly indents the key itself.
   * current_col() counts a tab as a single byte of width, the same as a
   * space, so a lone leading tab can land at exactly map_indent by the byte
   * offset alone. The parser must reject it anyway, like every other
   * comparison of an indent in this file, instead of silently accepting it
   * because the byte count happens to match. */
  require_tab_rejected("a:\n ? b\n\t: c\n");
}

TEST(block_scalars, tab_only_leading_blank_line_rejected) {
  /* Y79Y-0: a tab stands as the very first character of what would be the
   * leading blank line of a block scalar, where block_indent is unknown, so
   * the tab is truly unclear (it can be more indent, or content at indent
   * 0). This is a hard error. */
  require_tab_rejected("foo: |\n\t\nbar: 1\n");
}

TEST(block_scalars, tab_after_one_leading_space_on_blank_line_still_works) {
  /* Y79Y-1: when even one real space stands before the tab on that same
   * kind of leading blank line, the doubt above goes away, because that one
   * space alone fixes the indent of this line. The parser accepts the line,
   * and a reference parser agrees. This is the one-space counterpart to the
   * zero-space rejection directly above, and it must stay accepted if
   * somebody ever widens that check. */
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: |\n \t\nbar: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  /* The one leading space fixes block_indent = 1, so the tab after it becomes
   * the literal content of this one-line scalar. CLIP chomping is the
   * default, and it appends the usual single trailing newline. */
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "\t\n");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "bar")), 1LL);
  cyaml_destroy(doc);
}

TEST(block_scalars, tab_right_after_established_indent_is_content_literal) {
  /* The "1 space" on the very first content line of the scalar fixes
   * block_indent here, so a tab directly after it is ordinary literal
   * content, not indent: the s-indent(n) production covers spaces alone, and
   * whatever comes after it is nb-char* content, a tab included. A reference
   * parser agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: |-\n \tbar", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "\tbar");
  cyaml_destroy(doc);
}

TEST(block_scalars,
     tab_right_after_established_indent_folded_is_content_literal) {
  /* The folded ('>') sibling of the literal scalar case above, where the fold
   * adds one more detail of its own. The s-nb-spaced-text(n) production of
   * YAML 1.2 section 8.1.3 covers a "more indented", or "spaced", line and
   * reads s-indent(n) s-white nb-char*, where s-white is a space OR a tab,
   * so a line whose first byte past block_indent is a tab is a spaced line;
   * a reference parser agrees. The tab adds no extra literal SPACE character
   * of its own, as a real extra space would, but the parser keeps the
   * leading break of a spaced line literally instead of folding it to a
   * space. The plain literal scalar case above differs, because it never
   * folds anything. */
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: >\n  real\n  \tmore\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")),
                "real\n\tmore\n");
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         TAGS                                              */
/* ========================================================================== */

TEST(tags, shorthand_secondary_resolves_to_core_schema_uri) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), CYAML_TAG_STR);
  cyaml_destroy(doc);
}

TEST(tags, shorthand_primary_resolves_to_local_tag) {
  /* No %TAG gives "!" a new meaning, so its default prefix is "!" itself, and
   * a shorthand "!foo" resolves to the literal local tag "!foo". */
  char *err = NULL;
  cyaml doc = cyaml_parse("!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "!foo");
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "bar");
  cyaml_destroy(doc);
}

TEST(tags, verbatim_tag_used_as_is) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<tag:example.com,2000:app/foo> bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app/foo");
  cyaml_destroy(doc);
}

TEST(tags, verbatim_local_tag_used_as_is) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<!local> bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "!local");
  cyaml_destroy(doc);
}

TEST(tags, verbatim_tag_glued_directly_to_content_rejected) {
  /* c-ns-properties needs s-separate(n,c) before any content that follows,
   * while "!<a>b" glues ordinary scalar content directly onto the closing
   * '>' of the tag, with no whitespace between them, and the grammar offers
   * no valid path for that. A reference parser agrees. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a>b\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, verbatim_tag_glued_directly_to_value_content_rejected) {
  /* The same rule at the position of a mapping value, not only at the
   * document root. */
  char *err = NULL;
  cyaml doc = cyaml_parse("k: !<a>b\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, verbatim_tag_glued_directly_to_block_scalar_rejected) {
  /* The same rule with the indicator of a block scalar glued directly onto
   * the tag instead of plain scalar text. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a>|\n  x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, verbatim_tag_with_real_separator_still_works) {
  /* The rejection of glued content above must not reach too far: real
   * whitespace can separate a tag from its content. verbatim_tag_used_as_is
   * already covers that ordinary case with a named tag, and this test
   * repeats it with content that trips that check when the parser handles
   * the separator wrongly. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a> b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "a");
  REQUIRE_STREQ(cyaml_str_val(doc), "b");
  cyaml_destroy(doc);
}

TEST(tags, bare_non_specific_tag_resolves_to_no_forced_type) {
  /* A bare "!" has nothing after it, which makes it different from a
   * shorthand tag: it never forces a type, and the value resolves exactly as
   * it does with no tag at all. */
  char *err = NULL;
  cyaml doc = cyaml_parse("! 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  REQUIRE_EQ((void *)cyaml_node_tag(doc), NULL);
  cyaml_destroy(doc);
}

TEST(tags, tag_then_anchor_combined) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!str &x foo\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "a")), CYAML_TAG_STR);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "b")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "foo");
  cyaml_destroy(doc);
}

TEST(tags, anchor_then_tag_combined) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x !!str foo\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "a")), CYAML_TAG_STR);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "b")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "foo");
  cyaml_destroy(doc);
}

TEST(tags, tag_on_collection_has_no_override_effect_on_parsing) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq\n- a\n- b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_node_tag(doc), CYAML_TAG_SEQ);
  cyaml_destroy(doc);
}

TEST(tags, tag_on_mapping_introduced_by_anchored_key_is_preserved) {
  /* "!!map" decorates the dictionary that "&a key: value" introduces on the
   * next line. The anchor belongs to the key scalar alone, not to the mapping
   * (see the tests for an anchored key in block_mapping), so the outer tag
   * must reach the mapping itself instead of being silently dropped by the
   * "is this a key" fast path of the anchor branch. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!map\n  &a key: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_node_tag(doc), CYAML_TAG_MAP);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(tags, tag_on_mapping_introduced_by_anchored_key_mismatch_rejected) {
  /* The same shape as above, with a "!!seq" that does not match the
   * structure: the parser must catch it as a hard mismatch between the tag
   * and the content instead of letting the fast path that built the mapping
   * of the previous test silently ignore it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq\n  &a key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, tag_on_mapping_introduced_by_aliased_key_is_preserved) {
  /* The same shape, through the fast path that takes a '*' alias as a key
   * instead of '&'. A space before the ':' is needed, just as a plain scalar
   * key needs ": ", because a glued "key:value" is not a map separator. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("top:\n  a: &x k\n  b: !!map\n    *x : value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(cyaml_dictionary_get(doc, "top"), "b");
  REQUIRE_EQ(cyaml_type(b), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_node_tag(b), CYAML_TAG_MAP);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(b, "k")), "value");
  cyaml_destroy(doc);
}

TEST(tags, tag_on_mapping_introduced_by_tagged_key_is_preserved) {
  /* The outer "!!map" sits on its own line and must reach the mapping that
   * the inner "!!str" introduces on the next line. That inner tag belongs to
   * the key, and the parser correctly discards it (see
   * tag_on_dictionary_key_parses_but_is_not_preserved), but that must not
   * also swallow the outer tag, which is a structurally different thing. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!map\n  !!str key: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_node_tag(doc), CYAML_TAG_MAP);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(tags, unknown_custom_tag_preserved_scalar_gets_implicit_type) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!mytag 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "!mytag");
  /* A custom tag never overrides the type of a scalar: the value falls
   * through to implicit resolution, so 42 becomes an integer. */
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tags, binary_tag_stores_literal_text_no_decode) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!binary aGVsbG8=\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "aGVsbG8=");
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:yaml.org,2002:binary");
  cyaml_destroy(doc);
}

TEST(tags, two_tags_on_one_node_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str !!int 42\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, alias_cannot_carry_a_tag) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\nb: !!str *x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, non_specific_tag_then_named_tag_rejected_in_flow_context) {
  /* A bare non-specific "!" resolves to no forced type (see the doc comment
   * of parse_tag_token()), but it is a real tag for the "at most one tag"
   * rule of c-ns-properties, so the parser must reject a second, named tag
   * stacked under it, as it already rejects two named tags stacked on each
   * other, such as the pair "!!str !!int 5" above. The ordinary same-line
   * check of block context ("cur(ctx) == '!'") does not apply inside a flow
   * collection, so this test drives the guard that had_tag carries, not that
   * separate check on a character. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[! !!str x]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, non_specific_tag_then_named_tag_rejected_on_own_line) {
  /* The same rule through the "value starts on its own line" path, which the
   * '&' branch also uses, so the second tag is not glued onto the same line
   * as the first. The parser must carry had_tag into the recursive call to
   * parse_node, since a check against resolved_tag alone is not enough:
   * resolved_tag is NULL for the enclosing non-specific "!", so such a check
   * misses this document. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: !\n  !!int 5\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, alias_cannot_carry_a_non_specific_tag) {
  /* This test mirrors alias_cannot_carry_a_tag above, with a bare
   * non-specific "!" in place of a named tag: an alias can never carry any
   * tag at all, including one whose resolved_tag is NULL. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\nb: ! *x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, tag_on_dictionary_key_parses_but_is_not_preserved) {
  /* A tag that decorates a dictionary key meets a real, permanent structural
   * limitation of this DOM, whose keys are a plain char*, not a
   * cyaml_node_t*. Such a tag must parse with no error, because the grammar
   * accepts it, but after typing the key it has nowhere to live, so the
   * parser does not keep it. The
   * check that cyaml_node_tag(doc) is NULL is what proves "not preserved": a
   * regression can leak the tag of the key onto the enclosing mapping node
   * instead of discarding it, and without that check this test would not
   * catch it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str key: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ((void *)cyaml_node_tag(doc), NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(tags, undefined_tag_handle_on_dictionary_key_rejected) {
  /* The parser parses a tag that decorates a key anyway, to check that it is
   * valid (see the doc comment of the previous test and of
   * cyaml_node_tag()), so it must reject an undefined %TAG handle here,
   * exactly as it already rejects it when the identical tag decorates a
   * value. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!x!foo key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, malformed_verbatim_tag_on_dictionary_key_rejected) {
  /* The same rule about a check for validity as the previous test, but here a
   * verbatim tag rejects a null byte written as a percent escape instead of
   * an undefined handle. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<tag:x,2002:%00> key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, alias_of_tagged_anchor_preserves_tag_null) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!null &x ~\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "b")), CYAML_TAG_NULL);
  cyaml_destroy(doc);
}

TEST(tags, alias_of_tagged_anchor_preserves_tag_bool) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!bool &x true\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "b")), CYAML_TAG_BOOL);
  cyaml_destroy(doc);
}

TEST(tags, alias_of_tagged_anchor_preserves_tag_int) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!int &x 42\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "b")), CYAML_TAG_INT);
  cyaml_destroy(doc);
}

TEST(tags, alias_of_tagged_anchor_preserves_tag_float) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!float &x 1.5\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "b")),
                CYAML_TAG_FLOAT);
  cyaml_destroy(doc);
}

TEST(tags, alias_of_tagged_anchor_preserves_tag_str) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!str &x foo\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "b")), CYAML_TAG_STR);
  cyaml_destroy(doc);
}

TEST(tags, alias_of_tagged_anchor_preserves_tag_list) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!seq &x\n  - 1\n  - 2\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "b")), CYAML_TAG_SEQ);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "b")), CYAML_LIST);
  cyaml_destroy(doc);
}

TEST(tags, alias_of_tagged_anchor_preserves_tag_dictionary) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!map &x\n  k: 1\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_dictionary_get(doc, "b")), CYAML_TAG_MAP);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "b")), CYAML_DICTIONARY);
  cyaml_destroy(doc);
}

TEST(tags, cyaml_set_on_tagged_node_preserves_tag) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!str foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml leaf = cyaml_dictionary_get(doc, "a");
  REQUIRE_NE((void *)leaf, NULL);
  REQUIRE_STREQ(cyaml_node_tag(leaf), CYAML_TAG_STR);
  ccol_retval_t rv = cyaml_set(doc, "a", "bar");
  REQUIRE_EQ((int)rv, (int)ccol_success);
  leaf = cyaml_dictionary_get(doc, "a");
  REQUIRE_STREQ(cyaml_str_val(leaf), "bar");
  REQUIRE_STREQ(cyaml_node_tag(leaf), CYAML_TAG_STR);
  cyaml_destroy(doc);
}

TEST(tags, node_set_tag_refuses_a_core_tag_of_another_type) {
  cyaml n = cyaml_create_int(42);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ((void *)cyaml_node_tag(n), NULL);
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_STR);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 42LL);
  REQUIRE_EQ((void *)cyaml_node_tag(n), NULL);
  ccol_retval_t rv2 = cyaml_node_set_tag(n, CYAML_TAG_INT);
  REQUIRE_EQ((int)rv2, (int)ccol_success);
  REQUIRE_STREQ(cyaml_node_tag(n), CYAML_TAG_INT);
  ccol_retval_t rv3 = cyaml_node_set_tag(n, NULL);
  REQUIRE_EQ((int)rv3, (int)ccol_success);
  REQUIRE_EQ((void *)cyaml_node_tag(n), NULL);
  cyaml_destroy(n);
}

TEST(tags, node_tag_of_null_node_is_null) {
  REQUIRE_EQ((void *)cyaml_node_tag(NULL), NULL);
}

/* ========================================================================== */
/*                         TAG DIRECTIVES (%TAG)                             */
/* ========================================================================== */

TEST(tag_directives, custom_secondary_handle_shorthand_resolves) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG !e! tag:example.com,2000:\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, redefining_primary_handle) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG ! tag:example.com,2000:\n---\n!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, redefining_secondary_handle) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG !! tag:example.com,2000:\n---\n!!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, per_document_scoping) {
  /* A %TAG directive in document 1 must not leak into document 2. The second
   * document gives the same handle "!e!" a DIFFERENT prefix, which proves
   * that the registration of the first document did not survive: a
   * registration that survived would make this a redefinition of one handle,
   * which the parser rejects, whereas the parse succeeds with a different
   * resolved tag. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!foo a\n"
      "...\n%TAG !e! tag:other.example,2000:\n---\n!e!foo b\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_STREQ(cyaml_node_tag(cyaml_list_get(doc, 0)),
                "tag:example.com,2000:foo");
  REQUIRE_STREQ(cyaml_node_tag(cyaml_list_get(doc, 1)),
                "tag:other.example,2000:foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, undefined_handle_in_second_document_fails_whole_parse) {
  /* An undefined tag handle in ANY document of a stream of several documents
   * fails the whole cyaml_parse call, not only that one document, because a
   * parse of several documents has no way to report a partial success. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!foo a\n"
      "...\n---\n!e!foo b\n",
      &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, undefined_named_handle_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!e!foo bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, malformed_tag_directive_rejected) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG notahandle tag:example.com,2000:\n---\na: 1\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, same_document_handle_redefinition_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n"
      "%TAG !e! tag:example.com,2000:\n---\na: 1\n",
      &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, extra_content_after_prefix_rejected) {
  /* Only whitespace, and a comment that whitespace separates, may follow the
   * prefix of a %TAG directive, because the l-tag-directive of YAML 1.2
   * section 6.8.2 ends in the same s-l-comments production as every other
   * directive line. The parser must reject a bare extra word, as it already
   * rejects "%YAML 1.2 garbage". */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app/ garbage-text\n---\na: 1\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, trailing_comment_with_separator_accepted) {
  /* A comment correctly separated from the prefix by whitespace is a valid,
   * ordinary trailing comment; the identical %YAML case is
   * yaml_directive_trailing_comment_with_separator_accepted. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:  # comment\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_glued_to_prefix_is_part_of_the_uri) {
  /* ns-uri-char, the grammar of the tag prefix in YAML 1.2, explicitly
   * permits '#' as an ordinary URL character. A '#' glued directly onto the
   * prefix, with no whitespace before it, cannot be a comment, because a
   * comment needs s-b-comment, which is real whitespace in front of it. The
   * parser must therefore treat that '#' as part of the prefix text itself
   * instead of silently truncating the prefix there and discarding the rest
   * as a comment. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app#\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app#foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_glued_mid_prefix_is_part_of_the_uri) {
  /* The same rule with the '#' in the middle of the prefix instead of at its
   * very end, which confirms that the parser captures the whole rest of the
   * token as prefix text, not a trailing '#' alone. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG !e! tag:example.com/#zzz\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com/#zzzfoo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_after_real_whitespace_is_still_a_comment) {
  /* A '#' separated from the prefix by real whitespace is an ordinary
   * trailing comment. This test guards against the rule for a glued '#'
   * above reaching too far and treating every '#' as prefix content,
   * whatever its position. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app #comment\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:appfoo");
  cyaml_destroy(doc);
}

TEST(tag_directives, verbatim_tag_percent_escaped_slash) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<tag:example.com,2000:app%2Ffoo> bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app/foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, verbatim_tag_percent_escaped_null_byte_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<tag:example.com,2000:app%00foo> bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, verbatim_tag_malformed_percent_escape_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<tag:example.com,2000:app%zzfoo> bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, shorthand_tag_percent_escaped_slash_resolves) {
  /* ns-tag-char, the grammar of the shorthand suffix in YAML 1.2 section 5.5,
   * derives from ns-uri-char, exactly like the content of the verbatim form,
   * so the suffix of a shorthand tag must decode a "%XX" escape too (see
   * verbatim_tag_percent_escaped_slash above). */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!app%2Ffoo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app/foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, shorthand_tag_secondary_handle_percent_escape_resolves) {
  /* The default secondary handle carries the well-known prefix
   * "tag:yaml.org,2002:". That prefix with a percent-escaped suffix must
   * resolve to the same core schema tag as its plain spelling, !!str, not to
   * an unknown custom tag that carries the literal "%74" with no decode. */
  char *err = NULL;
  cyaml doc = cyaml_parse("val: !!s%74r 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml val = cyaml_dictionary_get(doc, "val");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_type(val), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(val), "42");
  REQUIRE_STREQ(cyaml_node_tag(val), CYAML_TAG_STR);
  cyaml_destroy(doc);
}

TEST(tag_directives, shorthand_tag_percent_escaped_null_byte_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!app%00foo bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, shorthand_tag_malformed_percent_escape_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!app%zzfoo bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

/* ========================================================================== */
/*                         TAG-DRIVEN TYPE RESOLUTION                        */
/* ========================================================================== */

TEST(tag_typing, null_forces_on_plain) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!null foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(tag_typing, str_forces_on_plain) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "42");
  cyaml_destroy(doc);
}

TEST(tag_typing, str_forces_on_double_quoted) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str \"42\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "42");
  cyaml_destroy(doc);
}

TEST(tag_typing, str_forces_on_single_quoted) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str '42'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "42");
  cyaml_destroy(doc);
}

TEST(tag_typing, str_forces_on_literal_block) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str |\n  42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "42\n");
  cyaml_destroy(doc);
}

TEST(tag_typing, str_forces_on_folded_block) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str >\n  42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "42\n");
  cyaml_destroy(doc);
}

TEST(tag_typing, int_forces_on_double_quoted) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int \"42\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tag_typing, int_forces_on_single_quoted) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int '42'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tag_typing, int_forces_on_plain) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tag_typing, int_rejects_hex_with_embedded_sign) {
  /* The !!int branch of finalize_scalar_node shares try_parse_int_scalar with
   * implicit typing, so it must reject the same malformed literal in the
   * style of "0x-0" that the regression test for a plain scalar above
   * covers, instead of accepting it as 0. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int 0x-0\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(tag_typing, int_rejects_uppercase_hex_prefix) {
  /* finalize_scalar_node's !!int branch shares try_parse_int_scalar with
   * implicit typing, so an uppercase "0X" prefix must be rejected here too
   * (see implicit_types.uppercase_hex_prefix_is_a_string), not silently
   * accepted as 16. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int 0X10\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(tag_typing, int_rejects_uppercase_octal_prefix) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int 0O17\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(tag_typing, int_forces_on_literal_block_clip) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int |\n  42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tag_typing, int_forces_on_literal_block_strip) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int |-\n  42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tag_typing, int_forces_on_literal_block_keep_multiple_newlines) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int |+\n  42\n\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tag_typing, int_forces_on_folded_block_clip) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int >\n  42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(tag_typing, float_forces_on_double_quoted) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float \"1.5\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(doc), 1.5);
  cyaml_destroy(doc);
}

TEST(tag_typing, float_forces_on_literal_block_keep_multiple_newlines) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float |+\n  1.5\n\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(doc), 1.5);
  cyaml_destroy(doc);
}

TEST(tag_typing, float_rejects_inrange_hex) {
  /* The YAML core schema gives hex and octal integer syntax no float form at
   * all, so a small hex literal that is in range must fail under !!float.
   * With no tag to force the type, try_parse_int_scalar accepts such a
   * literal as a real CYAML_INTEGER, and the parser must not silently turn
   * it into a float of the same magnitude. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0x10\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_rejects_inrange_octal) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0o17\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_rejects_uppercase_hex_prefix) {
  /* The critical regression case. strtod() itself accepts an uppercase "0X"
   * hex prefix, because the C standard writes "0x or 0X", ignoring letter
   * case, so try_parse_float_scalar rejects an uppercase prefix explicitly.
   * Without that, this literal falls through to the generic strtod() call,
   * whose extension silently accepts it as 16.0. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0X10\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_rejects_uppercase_octal_prefix) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0O17\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_accepts_oversized_hex_overflow) {
  /* An oversized hex literal, here 2^64-1, which is too wide for a signed
   * 64-bit integer, has no CYAML_INTEGER form at all, so !!float forces it to
   * a float of the same magnitude. The test
   * implicit_types.integer_hex_overflow_falls_back_to_float covers the
   * identical fallback for the case with no tag. This is the one legitimate
   * reason that this tag ever accepts hex or octal syntax. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0xFFFFFFFFFFFFFFFF\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_FLOAT);
  REQUIRE_GT(cyaml_double_val(doc), 0.0);
  cyaml_destroy(doc);
}

TEST(tag_typing, float_accepts_oversized_octal_overflow) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0o1777777777777777777777\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_FLOAT);
  REQUIRE_GT(cyaml_double_val(doc), 0.0);
  cyaml_destroy(doc);
}

TEST(tag_typing, bool_forces_true_false_on_plain) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool true\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_BOOL);
  REQUIRE_TRUE(cyaml_bool_val(doc));
  cyaml_destroy(doc);
}

TEST(tag_typing, bool_forces_on_double_quoted) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool \"true\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_BOOL);
  REQUIRE_TRUE(cyaml_bool_val(doc));
  cyaml_destroy(doc);
}

TEST(tag_typing, bool_accepts_wider_yes_no_on_off_vocabulary) {
  /* An explicit !!bool tag accepts a wider vocabulary than implicit bool
   * typing does, and it ignores letter case, whereas implicit typing accepts
   * only true, True, TRUE, false, False and FALSE. A measurement against the
   * construct_yaml_bool of PyYAML confirms the wider set: that function
   * matches {yes, no, true, false, on, off}, ignoring letter case, whatever
   * the loader is. See the doc comment of try_parse_bool_scalar_explicit in
   * src/cyaml.c. */
  char *err = NULL;
  cyaml doc_yes = cyaml_parse("!!bool yes\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc_yes, NULL);
  REQUIRE_TRUE(cyaml_bool_val(doc_yes));
  cyaml_destroy(doc_yes);

  cyaml doc_no = cyaml_parse("!!bool no\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc_no, NULL);
  REQUIRE_FALSE(cyaml_bool_val(doc_no));
  cyaml_destroy(doc_no);

  cyaml doc_on = cyaml_parse("!!bool ON\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc_on, NULL);
  REQUIRE_TRUE(cyaml_bool_val(doc_on));
  cyaml_destroy(doc_on);

  cyaml doc_off = cyaml_parse("!!bool Off\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc_off, NULL);
  REQUIRE_FALSE(cyaml_bool_val(doc_off));
  cyaml_destroy(doc_off);
}

TEST(tag_typing, bool_rejects_single_letter_y_n) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool y\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, null_forces_regardless_of_text) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!null anything\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(tag_typing, bool_mismatch_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool maybe\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, bool_forces_on_literal_block_keep_untrimmed_rejected) {
  /* !!int and !!float trim trailing whitespace (see
   * int_forces_on_literal_block_keep_multiple_newlines above), but !!bool
   * deliberately does NOT trim it before it matches against its vocabulary,
   * which matches the stricter lookup of PyYAML, which does not trim either.
   * The parser must NOT silently accept the trailing newlines of a
   * KEEP-chomped block scalar as "true". */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool |+\n  true\n\n\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, int_mismatch_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int abc\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_mismatch_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float abc\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, int_rejects_leading_whitespace) {
  /* Regression guard: the int grammar of the core schema has no production
   * for leading whitespace, but the library parses the trimmed text with
   * strtoll(), which silently skips such whitespace. The parser must reject
   * this document as a mismatch between the tag and the content instead of
   * silently accepting it as untrimmed text. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int \" 42\"\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_rejects_leading_whitespace) {
  /* Regression guard: the same class of defect as
   * int_rejects_leading_whitespace above, for the identical tolerance of
   * leading whitespace in strtod(). */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float \" 3.5\"\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, str_never_mismatches) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str anything at all\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  cyaml_destroy(doc);
}

TEST(tag_typing, seq_stored_without_altering_collection_parsing) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq [1, 2, 3]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)3);
  cyaml_destroy(doc);
}

TEST(tag_typing, map_stored_without_altering_collection_parsing) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!map {a: 1, b: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)2);
  cyaml_destroy(doc);
}

/* Structural-kind-mismatch: the 9 combinations. */

TEST(tag_typing, str_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, int_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float [1, 2]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, bool_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool [1, 2]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, null_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!null {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, map_on_sequence_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!map [1, 2]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, seq_on_mapping_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, seq_on_scalar_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq foo\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, map_on_scalar_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!map foo\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

/* ========================================================================== */
/*                         SERIALIZE TAGS                                    */
/* ========================================================================== */

TEST(serialize_tags, core_schema_tags_omitted_but_queryable) {
  const char *docs[] = {
      "!!null ~\n",  "!!bool true\n",  "!!int 42\n",     "!!float 1.5\n",
      "!!str foo\n", "!!seq [1, 2]\n", "!!map {a: 1}\n",
  };
  const char *expected_tags[] = {
      CYAML_TAG_NULL, CYAML_TAG_BOOL, CYAML_TAG_INT, CYAML_TAG_FLOAT,
      CYAML_TAG_STR,  CYAML_TAG_SEQ,  CYAML_TAG_MAP,
  };
  for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(docs[i], &err);
    REQUIRE_EQ((void *)err, NULL);
    REQUIRE_NE((void *)doc, NULL);
    REQUIRE_STREQ(cyaml_node_tag(doc), expected_tags[i]);

    char *out = cyaml_serialize(doc);
    REQUIRE_NE((void *)out, NULL);
    /* None of the seven core-schema tag URIs ever appear in the output. */
    REQUIRE_EQ((void *)strstr(out, "tag:yaml.org,2002:"), NULL);

    char *err2 = NULL;
    cyaml reparsed = cyaml_parse(out, &err2);
    REQUIRE_EQ((void *)err2, NULL);
    REQUIRE_NE((void *)reparsed, NULL);
    REQUIRE_EQ(cyaml_type(reparsed), cyaml_type(doc));

    cyaml_serialize_free(out);
    cyaml_destroy(doc);
    cyaml_destroy(reparsed);
  }
}

TEST(serialize_tags, custom_tag_always_emitted_verbatim_form) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG !e! tag:example.com,2000:\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:foo");

  char *out = cyaml_serialize(doc);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_NE((void *)strstr(out, "!<tag:example.com,2000:foo>"), NULL);

  char *err2 = NULL;
  cyaml reparsed = cyaml_parse(out, &err2);
  REQUIRE_EQ((void *)err2, NULL);
  REQUIRE_NE((void *)reparsed, NULL);
  REQUIRE_STREQ(cyaml_node_tag(reparsed), "tag:example.com,2000:foo");
  REQUIRE_STREQ(cyaml_str_val(reparsed), "bar");

  cyaml_serialize_free(out);
  cyaml_destroy(doc);
  cyaml_destroy(reparsed);
}

TEST(serialize_tags, custom_tag_on_collection_round_trips) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!mytag\n- a\n- b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  char *out = cyaml_serialize(doc);
  REQUIRE_NE((void *)out, NULL);

  char *err2 = NULL;
  cyaml reparsed = cyaml_parse(out, &err2);
  REQUIRE_EQ((void *)err2, NULL);
  REQUIRE_NE((void *)reparsed, NULL);
  REQUIRE_EQ(cyaml_type(reparsed), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(reparsed), (size_t)2);
  REQUIRE_STREQ(cyaml_node_tag(reparsed), "!mytag");

  cyaml_serialize_free(out);
  cyaml_destroy(doc);
  cyaml_destroy(reparsed);
}

TEST(serialize_tags, custom_tag_round_trips_via_flow_serializer) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!mytag foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  char *out = cyaml_serialize_flow(doc);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_NE((void *)strstr(out, "!<!mytag>"), NULL);

  char *err2 = NULL;
  cyaml reparsed = cyaml_parse(out, &err2);
  REQUIRE_EQ((void *)err2, NULL);
  REQUIRE_NE((void *)reparsed, NULL);
  REQUIRE_STREQ(cyaml_node_tag(reparsed), "!mytag");
  REQUIRE_STREQ(cyaml_str_val(reparsed), "foo");

  cyaml_serialize_free(out);
  cyaml_destroy(doc);
  cyaml_destroy(reparsed);
}

TEST(serialize_tags, matching_core_schema_tag_set_via_api_still_omitted) {
  cyaml n = cyaml_create_int(42);
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_INT);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char *out = cyaml_serialize(n);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_EQ((void *)strstr(out, "tag:yaml.org,2002:"), NULL);

  char *err = NULL;
  cyaml reparsed = cyaml_parse(out, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)reparsed, NULL);
  REQUIRE_EQ((void *)cyaml_node_tag(reparsed), NULL);
  REQUIRE_EQ(cyaml_int_val(reparsed), 42LL);

  cyaml_serialize_free(out);
  cyaml_destroy(n);
  cyaml_destroy(reparsed);
}

/* cyaml_node_set_tag() refuses a core-schema tag that names another type, so
 * no public call puts such a tag in front of the serializer. The node keeps
 * the tag it had, and its serialization round trips to the same type and
 * value. */
TEST(serialize_tags, mismatched_core_schema_scalar_tag_is_refused) {
  cyaml n = cyaml_create_int(42);
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_STR);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  char *out = cyaml_serialize(n);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_EQ((void *)strstr(out, "tag:yaml.org"), NULL);

  char *err = NULL;
  cyaml reparsed = cyaml_parse(out, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)reparsed, NULL);
  REQUIRE_EQ(cyaml_type(reparsed), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(reparsed), 42LL);

  cyaml_serialize_free(out);
  cyaml_destroy(n);
  cyaml_destroy(reparsed);
}

TEST(serialize_tags, mismatched_core_schema_scalar_tag_via_flow_serializer) {
  cyaml n = cyaml_create_bool(true);
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_INT);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  char *out = cyaml_serialize_flow(n);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_STREQ(out, "true");

  cyaml_serialize_free(out);
  cyaml_destroy(n);
}

/* A mismatched CYAML_TAG_SEQ or CYAML_TAG_MAP is a mismatch of structure that
 * a reparse could never accept, and cyaml_node_set_tag() refuses it in the
 * same way. */
TEST(serialize_tags, mismatched_core_schema_collection_tag_is_refused) {
  cyaml n = cyaml_create_list();
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_MAP);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  REQUIRE_EQ((void *)cyaml_node_tag(n), NULL);

  char *out = cyaml_serialize(n);
  REQUIRE_NE((void *)out, NULL);

  char *err = NULL;
  cyaml reparsed = cyaml_parse(out, &err);
  REQUIRE_NE((void *)reparsed, NULL);
  REQUIRE_EQ(cyaml_type(reparsed), CYAML_LIST);

  cyaml_serialize_free(out);
  cyaml_destroy(n);
  cyaml_destroy(reparsed);
}

/* ========================================================================== */
/*                         MERGE KEYS (<<:)                                  */
/* ========================================================================== */

TEST(merge_keys, single_anchor_block) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &base\n  x: 1\n  y: 2\nb:\n  <<: *base\n  z: 3\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "x")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "y")), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "z")), 3LL);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, single_anchor_flow_with_reference) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &a {x: 1, y: 2}\nb: {<<: *a, z: 3}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "x")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "z")), 3LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, anchored_key_flow_still_merges) {
  /* An anchor can decorate the "<<" key itself, which does NOT stop the key
   * from being a merge candidate, just as on the block dictionary side (see the
   * doc comment of try_parse_scalar_dict_key). The key peek of a flow
   * dictionary excludes a '!' and a quote, but it must not exclude a '&' in
   * the same way, because such a rule leaves "<<" as a literal key with no
   * merge whenever an anchor decorates it in flow context. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: {&y <<: *x, k2: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, anchored_and_tagged_key_flow_dict_not_merged) {
  /* A tag stops a key from being a merge candidate also when it follows an
   * anchor on the same "<<" key ("&y !!str <<"), not only when the tag is
   * the very first character, which
   * tagged_double_angle_bracket_key_is_literal_not_merge already covers. The
   * peek at an implicit key of a flow dictionary must not read the first
   * character alone to decide "is this plain and untagged", because such a
   * check misses a tag hidden behind an anchor in front of it and then
   * merges when it must not. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: {&y !!str <<: *x, k2: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(b, "<<")), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, sequence_of_sources_earlier_wins) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &a\n  k: 1\nb: &b\n  k: 2\nc:\n  <<: [*a, *b]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml c = cyaml_dictionary_get(doc, "c");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(c, "k")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_wins_over_merged) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &base\n  x: 1\nb:\n  x: 99\n  <<: *base\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "x")), 99LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, non_mapping_source_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("b:\n  <<: [1, 2]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(merge_keys, non_mapping_scalar_alias_source_rejected) {
  /* non_mapping_source_rejected above covers a literal merge value that is
   * not a mapping, written directly as "<<: [1, 2]". This test covers a
   * separate code path, where the only merge source is an ALIAS that
   * resolves to a scalar: the "else" branch of merge_one_source_into, not
   * the loop over the elements of a sequence that
   * non_mapping_element_in_source_sequence_rejected below drives. The parser
   * must reject this case in the same way instead of silently running zero
   * iterations over it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &a 5\nb:\n  <<: *a\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(merge_keys, non_mapping_element_in_source_sequence_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &a\n  x: 1\nb:\n  <<: [*a, 5]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(merge_keys, transitive_merge_source) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &a\n  k1: 1\nb: &b\n  <<: *a\n  k2: 2\nc:\n  <<: *b\n  k3: 3\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml c = cyaml_dictionary_get(doc, "c");
  REQUIRE_NE((void *)c, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(c), (size_t)3);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(c, "k1")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(c, "k2")), 2LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(c, "k3")), 3LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, same_anchor_merged_into_two_targets) {
  /* The memtest target covers this test, which drives the ownership rule
   * that gives each target its own cyaml_clone: one anchored value merges
   * into two different mappings here. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &base\n  x: 1\nb:\n  <<: *base\n  y: 2\nc:\n  <<: *base\n  z: 3\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  cyaml c = cyaml_dictionary_get(doc, "c");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "x")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(c, "x")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, quoted_double_angle_bracket_key_is_literal_not_merge) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x\n  k: 1\nb:\n  \"<<\": *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(b, "<<")), CYAML_DICTIONARY);
  cyaml_destroy(doc);
}

TEST(merge_keys, tagged_double_angle_bracket_key_is_literal_not_merge) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x\n  k: 1\nb:\n  !!str <<: *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, tag_on_enclosing_mapping_does_not_disable_merge) {
  /* A tag can sit on the ENCLOSING mapping, which a '!' layer passes down to
   * the whole resulting dictionary. That is a completely different thing from
   * a tag directly on the "<<" key itself (see
   * tagged_double_angle_bracket_key_is_literal_not_merge above, where the tag
   * decorates the key). Code that treats the two as one leaves "<<" as a
   * literal key with no merge whenever a tag decorates the enclosing
   * mapping. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("base: &b\n  x: 1\nm: !!map\n  <<: *b\n  y: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml m = cyaml_dictionary_get(doc, "m");
  REQUIRE_NE((void *)m, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(m), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(m, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(m, "x")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(m, "y")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, plain_form_baseline_contrast) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x\n  k: 1\nb:\n  <<: *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_block_expands) {
  /* The explicit form of '? <<' and ': value' is as real a merge trigger as
   * the implicit '<<: value' shorthand, as two independent reference parsers
   * confirm, because the key is the same plain, untagged "<<" scalar either
   * way. This DOM detects a merge where it captures the key, not with a later
   * lookup, so both forms must be recognized there. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x\n  k: 1\nb:\n  ? <<\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_as_first_entry_expands) {
  /* The very first entry of the mapping takes a different dispatch path from
   * every later entry: it goes through the '?' branch of parse_node, with
   * first_key == NULL, while a later entry goes through
   * parse_one_dict_entry_key. The first entry must expand in the same way. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x\n  k: 1\nb:\n  ? <<\n  : *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_block_anchored_still_expands) {
  /* An anchor that decorates the explicit key does NOT disqualify it, just as
   * in the implicit form, where "&y <<: *x" is a real trigger as well. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  ? &z <<\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_block_key_on_own_line_expands) {
  /* YAML 1.2 section 8.2.2 lets the content of an explicit key start on a
   * fresh line with more indent instead of staying on the '?' line itself,
   * unlike an implicit key, which always sits on one line. The check for a
   * merge key candidate must therefore run from wherever the real content of
   * the key starts, not from whatever directly follows the '?' on its own
   * line, which here is nothing but the newline that this construct exists
   * to cross. A reference parser confirms that this expands in the same way
   * as the same-line "? <<" form. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  ?\n    <<\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_anchored_key_on_own_line_expands) {
  /* The same case of a key pushed to a fresh line, with an anchor that also
   * decorates the key: the loop inside explicit_key_peek_is_merge_candidate
   * skips an anchor and a tag, and it must reach the real "<<" text across
   * the same newline. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  ?\n    &z <<\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_block_quoted_not_merged) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  ? \"<<\"\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_block_tagged_not_merged) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  ? !!str <<\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_block_prefix_not_merged) {
  /* "<<x" is a plain scalar that starts with "<<", and the parser must not
   * take it for the merge trigger, which is exactly two characters. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  ? <<x\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<x"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_block_followed_by_word_not_merged) {
  /* "<< foo" holds whitespace directly after "<<" and then more plain scalar
   * content on the same line. The peek for a merge candidate reports it
   * wrongly, because its check on whitespace is deliberately coarse and does
   * not derive the real termination rules of scan_plain_scalar_line again.
   * Under those rules, plain internal whitespace never ends a scalar, so the
   * fully parsed key is the whole two-word scalar "<< foo", never the exact
   * trigger "<<", and the parser must store it as an ordinary literal key,
   * with no merge. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  ? << foo\n  : *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<< foo"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_flow_expands) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: {? <<: *x, k2: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_flow_anchored_still_expands) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: {? &z <<: *x, k2: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_flow_quoted_not_merged) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: {? \"<<\": *x, k2: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, explicit_key_form_flow_colon_glued_not_merged) {
  /* "<<:*x" carries no whitespace to separate it, so it does not end at the
   * ':', because the colon rule of scan_plain_scalar_line in flow context
   * needs the next character to be whitespace or a flow terminator. The
   * whole text is therefore one literal plain scalar key, not a merge
   * trigger with an alias value. A reference parser confirms this. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: {? <<:*x, k2: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_NE((void *)cyaml_dictionary_get(b, "<<:*x"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, flow_sequence_implicit_shorthand_expands) {
  /* "[<<: *x]" is the "[key: value]" compact mapping shorthand of a flow
   * SEQUENCE (see YAML 1.2 section 7.4.1, ns-flow-pair), and it must expand
   * a real merge key exactly like three other forms already do: the flow
   * dictionary "{<<: *x}", the block dictionary, and the compact mapping of
   * a block sequence "- <<: *x". This list holds one element, a mapping with
   * one entry whose only key is "<<". */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: [<<: *x]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_list_len(b), (size_t)1);
  cyaml elem0 = cyaml_list_get(b, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(elem0), (size_t)1);
  cyaml k = cyaml_dictionary_get(elem0, "k");
  REQUIRE_NE((void *)k, NULL);
  REQUIRE_EQ(cyaml_int_val(k), 1LL);
  REQUIRE_EQ((void *)cyaml_dictionary_get(elem0, "<<"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, flow_sequence_bare_key_value_anchored_and_tagged_not_merged) {
  /* This test mirrors merge_keys.anchored_and_tagged_key_flow_dict_not_merged
   * for the bare "[key: value]" shorthand of a flow SEQUENCE: a tag after an
   * anchor on the "<<" key must stop the merge here too, and a peek that
   * reads the very first character alone cannot see that tag. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: [&y !!str <<: *x]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_list_len(b), (size_t)1);
  cyaml elem0 = cyaml_list_get(b, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(elem0), (size_t)1);
  REQUIRE_NE((void *)cyaml_dictionary_get(elem0, "<<"), NULL);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(elem0, "<<")), CYAML_DICTIONARY);
  cyaml_destroy(doc);
}

TEST(merge_keys, flow_sequence_bare_key_value_two_elements_only_first_merges) {
  /* "[<<: *x, k2: 2]" holds two SEPARATE list elements, because the shorthand
   * makes each one a compact mapping of one pair instead of one mapping with
   * two keys. Only the first element has the key "<<", so only it expands. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: [<<: *x, k2: 2]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_list_len(b), (size_t)2);
  cyaml elem0 = cyaml_list_get(b, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(elem0), (size_t)1);
  REQUIRE_NE((void *)cyaml_dictionary_get(elem0, "k"), NULL);
  cyaml elem1 = cyaml_list_get(b, 1);
  REQUIRE_NE((void *)elem1, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(elem1), (size_t)1);
  REQUIRE_NE((void *)cyaml_dictionary_get(elem1, "k2"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys, flow_sequence_explicit_shorthand_expands) {
  /* "[? <<: *x]" is the counterpart with an explicit key of the implicit
   * "[<<: *x]" shorthand directly above (see YAML 1.2 section 7.4.1, spec
   * example 7.20), and it must expand in the same way. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: [? <<: *x]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_list_len(b), (size_t)1);
  cyaml elem0 = cyaml_list_get(b, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(elem0), (size_t)1);
  cyaml k = cyaml_dictionary_get(elem0, "k");
  REQUIRE_NE((void *)k, NULL);
  REQUIRE_EQ(cyaml_int_val(k), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, flow_sequence_explicit_shorthand_followed_by_word_not_merged) {
  /* "[? << foo: 5]" drives the "[? key: value]" shorthand of a flow sequence,
   * where the peek for a merge candidate does not check the fully parsed key
   * against "<<" with strcmp before it calls expand_merge_key, although every
   * other caller of explicit_key_peek_is_merge_candidate does check it. This
   * test therefore drives the safety net of that path, the
   * cyaml_dictionary_get(map, "<<") lookup inside expand_merge_key, which
   * does nothing for a dictionary whose only key is the literal "<< foo". */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: [? << foo: 5]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_list_len(b), (size_t)1);
  cyaml elem0 = cyaml_list_get(b, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(elem0), (size_t)1);
  REQUIRE_EQ((void *)cyaml_dictionary_get(elem0, "<<"), NULL);
  cyaml value = cyaml_dictionary_get(elem0, "<< foo");
  REQUIRE_NE((void *)value, NULL);
  REQUIRE_EQ(cyaml_int_val(value), 5LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, flow_sequence_quoted_double_angle_bracket_key_is_literal) {
  /* Mirrors the existing flow-dictionary/block-dictionary "quoted '<<' is
   * never a merge trigger" coverage, for the flow-sequence shorthand. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x {k: 1}\nb: [\"<<\": *x]\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  cyaml elem0 = cyaml_list_get(b, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(elem0), (size_t)1);
  REQUIRE_NE((void *)cyaml_dictionary_get(elem0, "<<"), NULL);
  cyaml_destroy(doc);
}

/* One mapping can hold more than one entry with the key "<<", and they
 * collapse to whichever one somebody wrote LAST, under the documented "last
 * value wins" policy of this module for a duplicate key. The four tests below
 * confirm two directions: a later "<<" entry that carries a quote or a tag
 * correctly wins as an ordinary literal and never takes the status of a merge
 * trigger from the real "<<" entry that it overwrites, and an earlier literal
 * "<<" must not suppress a later, real merge trigger either. The parser tracks
 * merge candidacy against whichever entry ends up owning the "<<" slot, and
 * never accumulates that flag with OR across every entry of the mapping. */
TEST(merge_keys,
     block_duplicate_double_angle_bracket_later_quoted_string_not_merged) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &x\n  k: 1\nb:\n  <<: *x\n  \"<<\": literal_string\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(lt), "literal_string");
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "k"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys,
     block_duplicate_double_angle_bracket_later_quoted_mapping_not_merged) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &x\n  k: 1\nb:\n  <<: *x\n  \"<<\": {other: 9}\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(lt, "other")), 9LL);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "k"), NULL);
  cyaml_destroy(doc);
}

TEST(merge_keys,
     block_duplicate_double_angle_bracket_earlier_quoted_still_merges) {
  /* The reverse order: an earlier, literal "<<" must not suppress a later,
   * genuine merge trigger for the same key. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &x\n  k: 1\nb:\n  \"<<\": ignored\n  <<: *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "<<"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys,
     flow_duplicate_double_angle_bracket_later_quoted_mapping_not_merged) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &x\n  k: 1\nb: {<<: *x, \"<<\": {other: 9}, k2: 2}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(lt, "other")), 9LL);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "k"), NULL);
  cyaml_destroy(doc);
}

/* merge_one_source_into asks "does the target already have this key", and
 * that check must NOT run before the parser removes the "<<" merge trigger
 * slot of the target. A merge SOURCE can legitimately hold its own literal,
 * quoted "<<" key, which is not itself a merge trigger. Without the right
 * order, the check wrongly reports that key as already present in the target,
 * because it matches the "<<" slot of the target, which nothing has removed
 * yet and which is the very entry that holds the merge source. The parser
 * then silently drops the real entry of the source, destroying it outright
 * once it removes the "<<" slot of the target, and reports no error at all. */
TEST(merge_keys, source_containing_literal_double_angle_bracket_key_preserved) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &x\n  \"<<\": literal_value\nb:\n  <<: *x\n  extra: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)2);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(lt), "literal_value");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "extra")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys,
     sequence_source_containing_literal_double_angle_bracket_key_preserved) {
  /* The same hazard through the "<<: [source1, source2]" form, a sequence of
   * mappings: the first source in the list carries a literal "<<" key of its
   * own, which must survive the merge exactly like any other key. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &x\n  \"<<\": literal_value\ny: &z\n  other: 9\nb:\n  <<: [*x, "
      "*z]\n  extra: 1\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)3);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(lt), "literal_value");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "other")), 9LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "extra")), 1LL);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                    RAW CONTROL CHARACTERS IN SCALARS                      */
/* ========================================================================== */

TEST(errors, raw_control_byte_rejected_in_double_quoted_scalar) {
  /* nb-json governs double-quoted content in YAML 1.2 and reads
   * "#x9 | [#x20-#x10FFFF]", so a raw C0 control byte other than a tab, with
   * no escape, has no valid literal form there. PyYAML rejects the identical
   * byte with "special characters are not allowed". */
  char src[] = "a: \"x\x01y\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, raw_control_byte_rejected_in_single_quoted_scalar) {
  char src[] = "a: 'x\x01y'\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, raw_control_byte_rejected_in_plain_scalar) {
  char src[] = "a: x\x01y\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, raw_control_byte_rejected_in_plain_scalar_continuation_line) {
  /* The same check, through the scan of a continuation line inside
   * parse_plain_scalar_multiline instead of the scan of the first line. */
  char src[] = "a: x\n  y\x01z\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, raw_control_byte_rejected_in_literal_block_scalar) {
  char src[] = "a: |\n  x\x01y\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, raw_control_byte_rejected_in_folded_block_scalar) {
  char src[] = "a: >\n  x\x01y\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, escaped_control_byte_still_accepted_in_double_quoted_scalar) {
  /* The rejection above covers a RAW byte in the source text alone. An
   * explicit escape sequence that produces the identical byte in the decoded
   * string is the deliberate, documented way to embed a C0 control byte that
   * is not null, with one exception: an escape that decodes to codepoint
   * zero (see the null_byte_escape_* tests below for why the parser rejects
   * that one). */
  char *err = NULL;
  cyaml n = cyaml_parse("a: \"x\\x01y\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  cyaml a = cyaml_dictionary_get(n, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_STREQ(cyaml_str_val(a), "x\x01y");
  cyaml_destroy(n);
}

TEST(errors, null_byte_escape_zero_rejected) {
  /* "\0" decodes to codepoint U+0000. Unlike every other double-quoted
   * escape, it cannot live inside the scalar value of a node, because that
   * value is a plain NUL-terminated char* with no separate length field. It
   * must therefore be a hard parse error, and the parser must not silently
   * truncate the string at the null byte inside it. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\0b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_byte_escape_hex_rejected) {
  /* The same restriction as null_byte_escape_zero_rejected, for the "\x00"
   * form with two hex digits. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\x00b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_byte_escape_u4_rejected) {
  /* The same restriction again, for the "\u0000" form with four hex
   * digits. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\u0000b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_byte_escape_u8_rejected) {
  /* The same restriction again, for the "\U00000000" form with eight hex
   * digits. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\U00000000b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(quoted, surrogate_pair_combination_never_produces_null_byte) {
  /* A \u escape can reach codepoint zero only directly: the smallest possible
   * result of a surrogate pair is U+10000, and a lone surrogate with no pair
   * is refused and never decodes to zero. This test confirms that the paths
   * for a surrogate never produce a null byte by accident by asserting that
   * the parser correctly ACCEPTS the pair and decodes it to U+10000, while
   * null_byte_escape_u4_rejected genuinely rejects its own input. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\ud800\\udc00\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xf0\x90\x80\x80");
  cyaml_destroy(n);
}

TEST(serialize_scalars, control_byte_in_string_is_quoted_and_escaped) {
  /* A string value can hold a raw control byte, a state that the API that
   * builds a node reaches, whatever the parser itself rejects on input. The
   * serializer must never write such a value as plain scalar content with no
   * quotes, which puts the raw byte directly into the output, where no YAML
   * 1.2 parser that obeys the spec can read it back, this one included. The
   * serializer must write the value double-quoted, with the byte escaped. */
  cyaml n = cyaml_create_string("x\x01y");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(strstr(s, "\x01") != NULL, false);
  REQUIRE_NE((void *)strstr(s, "\\x01"), NULL);

  /* A round trip: a second parse of the serialized form must give back the
   * same string. */
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_STREQ(cyaml_str_val(back), "x\x01y");

  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

/* Serialize the double v alone with each serializer, and require the text
 * `want` from both and a parse of each output back to exactly v as a
 * CYAML_FLOAT. */
static bool _float_text_round_trips(double v, const char *want) {
  cyaml n = cyaml_create_double(v);
  char *block = n ? cyaml_serialize(n) : NULL;
  char *flow = n ? cyaml_serialize_flow(n) : NULL;
  bool ok = block && flow && strncmp(block, want, strlen(want)) == 0 &&
            block[strlen(want)] == '\n' && strcmp(flow, want) == 0;
  char *err = NULL;
  cyaml back = block ? cyaml_parse(block, &err) : NULL;
  ok = ok && back && cyaml_type(back) == CYAML_FLOAT &&
       memcmp(&(double){cyaml_double_val(back)}, &v, sizeof(v)) == 0;
  cyaml_destroy(back);
  if (block) cyaml_serialize_free(block);
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(n);
  return ok;
}

TEST(serialize_scalars, float_mantissa_always_carries_a_point) {
  /* YAML 1.1 reads a float only when its mantissa has a '.', so "1e+20"
   * loads as a string in PyYAML and Psych. The serializer writes "1.0e+20",
   * which both versions of the spec read as the same double. This test is
   * non-vacuous: without the point in the mantissa, the exponent cases fail
   * on the text. */
  REQUIRE_TRUE(_float_text_round_trips(1e20, "1.0e+20"));
  REQUIRE_TRUE(_float_text_round_trips(1e-5, "1.0e-05"));
  REQUIRE_TRUE(_float_text_round_trips(-1e300, "-1.0e+300"));
  REQUIRE_TRUE(_float_text_round_trips(5e-324, "4.94065645841247e-324"));
  REQUIRE_TRUE(_float_text_round_trips(1.5e20, "1.5e+20"));
  REQUIRE_TRUE(_float_text_round_trips(2.0, "2.0"));
  REQUIRE_TRUE(_float_text_round_trips(-0.0, "-0.0"));
  REQUIRE_TRUE(_float_text_round_trips(0.25, "0.25"));
}

TEST(serialize_scalars, float_text_is_the_shortest_that_round_trips) {
  /* 1/3 needs 16 significant digits, not 17. This test is non-vacuous: a
   * rule that jumps from 15 digits straight to 17 writes
   * "0.33333333333333331". */
  REQUIRE_TRUE(_float_text_round_trips(1.0 / 3.0, "0.3333333333333333"));
  REQUIRE_TRUE(_float_text_round_trips(0.1 + 0.2, "0.30000000000000004"));
  REQUIRE_TRUE(_float_text_round_trips(0.1, "0.1"));

  /* A float key uses the same digits, with no ".0". */
  char *err = NULL;
  cyaml d = cyaml_parse("0.333333333333333314829616256247: a\n1e20: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)d, NULL);
  bool third = cyaml_dictionary_get(d, "0.3333333333333333") != NULL;
  bool big = cyaml_dictionary_get(d, "1e+20") != NULL;
  char *out = cyaml_serialize(d);
  bool out_ok = out && strstr(out, "0.3333333333333333: a\n") &&
                strstr(out, "1e+20: b\n");
  if (out) cyaml_serialize_free(out);
  cyaml_destroy(d);
  REQUIRE_TRUE(third);
  REQUIRE_TRUE(big);
  REQUIRE_TRUE(out_ok);

  /* A sweep: every text parses back to the double that it came from. */
  static const double samples[] = {1e-310,
                                   123456789012345678.0,
                                   9.5e15,
                                   6.02214076e23,
                                   -2.5e-7,
                                   1.7976931348623157e308,
                                   2.2250738585072014e-308,
                                   100.0,
                                   1e15,
                                   1e16};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
    cyaml n = cyaml_create_double(samples[i]);
    char *t = cyaml_serialize_flow(n);
    bool has_point = t && strchr(t, '.') != NULL;
    cyaml back = t ? cyaml_parse(t, &err) : NULL;
    bool same = back && cyaml_type(back) == CYAML_FLOAT &&
                cyaml_double_val(back) == samples[i];
    cyaml_destroy(back);
    if (t) cyaml_serialize_free(t);
    cyaml_destroy(n);
    REQUIRE_TRUE(has_point);
    REQUIRE_TRUE(same);
  }
}

TEST(errors, raw_del_byte_rejected_in_double_quoted_scalar) {
  /* Scalar content with no escape excludes DEL (0x7F) in every style,
   * treating DEL exactly like a C0 control byte; a measurement against
   * PyYAML confirms this. See the doc comment of is_disallowed_control_byte. */
  char src[] = "a: \"x\x7Fy\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, raw_del_byte_rejected_in_plain_scalar) {
  char src[] = "a: x\x7Fy\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, raw_del_byte_rejected_in_literal_block_scalar) {
  char src[] = "a: |\n  x\x7Fy\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, escaped_del_byte_still_accepted_in_double_quoted_scalar) {
  /* The same exception as
   * escaped_control_byte_still_accepted_in_double_quoted_scalar above: the
   * rejection covers a RAW byte alone, and the parser accepts an explicit
   * "\x7f" escape. */
  char *err = NULL;
  cyaml n = cyaml_parse("a: \"x\\x7fy\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  cyaml a = cyaml_dictionary_get(n, "a");
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_STREQ(cyaml_str_val(a), "x\x7Fy");
  cyaml_destroy(n);
}

TEST(serialize_scalars, del_byte_in_string_is_quoted_and_escaped) {
  /* The same guarantee about a round trip as
   * control_byte_in_string_is_quoted_and_escaped above, here for DEL (0x7F).
   * needs_quoting() and yb_append_yaml_dquoted() must agree on this byte;
   * without that, a string that holds it comes out in one of two wrong ways:
   * the serializer writes it as invalid raw plain scalar content, or it
   * quotes the value and escapes nothing. */
  cyaml n = cyaml_create_string("x\x7Fy");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(strstr(s, "\x7F") != NULL, false);
  REQUIRE_NE((void *)strstr(s, "\\x7f"), NULL);

  char *err2 = NULL;
  cyaml back2 = cyaml_parse(s, &err2);
  REQUIRE_EQ((void *)err2, NULL);
  REQUIRE_NE((void *)back2, NULL);
  REQUIRE_STREQ(cyaml_str_val(back2), "x\x7Fy");

  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back2);
}

/* ========================================================================== */
/*                         THREAD-LOCAL NODE POOL                             */
/* ========================================================================== */

/* A white-box accessor that gives the free-list size of the node pool of the
 * CALLING thread (see the THREAD-LOCAL NODE POOL section of cyaml.c). It is
 * not part of the public API; this file declares it in the same way as
 * tests/cvector/tests.c declares cvector_get_capacity(). */
extern size_t cyaml_debug_pool_size(void);

/* This mirrors _CYAML_POOL_CAP in cyaml.c. No public header holds it, because
 * it is an internal constant for tuning, so this file states it again here
 * instead of sharing it, as other white-box tests in this codebase do when
 * they pin a literal internal constant. */
#define CYAML_TEST_POOL_CAP 512

TEST(node_pool, cap_eviction_keeps_pool_bounded) {
  /* The pool of this thread already holds something when this test runs: a
   * count between 0 and CYAML_TEST_POOL_CAP, left by earlier tests in this
   * same binary. This test destroys strictly more than CYAML_TEST_POOL_CAP
   * nodes of the default allocator in one go, after which the pool must hold
   * EXACTLY the cap: the free list accepts every node_free() call below the
   * cap and evicts every call past it, freeing that node directly. This is
   * what stops the pool from growing without bound. */
  cyaml list = cyaml_create_list();
  REQUIRE_NE((void *)list, NULL);
  size_t n = CYAML_TEST_POOL_CAP + 100;
  for (size_t i = 0; i < n; i++)
    REQUIRE_EQ(cyaml_list_push(list, cyaml_create_null()), ccol_success);
  cyaml_destroy(list);

  REQUIRE_EQ(cyaml_debug_pool_size(), (size_t)CYAML_TEST_POOL_CAP);
}

typedef struct {
  size_t start_pool_size;
  size_t end_pool_size;
} pool_populate_result_t;

static void *pool_populate_thread(void *arg) {
  pool_populate_result_t *r = (pool_populate_result_t *)arg;
  r->start_pool_size = cyaml_debug_pool_size();
  /* Allocate all 50 first, and THEN free all 50. An allocate followed at once
   * by a free recycles that same one node fifty times over, so the pool
   * would hold 1, not 50, because node_alloc() always prefers a node that
   * the pool already holds. Here all 50 stay live at once, so every one of
   * them is a fresh calloc, since nothing is in the pool to recycle, and
   * freeing them afterward grows the pool by exactly one entry for each
   * node. cap_eviction_keeps_pool_bounded above has the same shape, where it
   * allocates and then frees in bulk. */
  cyaml nodes[50];
  for (int i = 0; i < 50; i++) nodes[i] = cyaml_create_null();
  for (int i = 0; i < 50; i++) cyaml_destroy(nodes[i]);
  r->end_pool_size = cyaml_debug_pool_size();
  return NULL;
}

TEST(node_pool, fresh_thread_starts_with_an_empty_pool) {
  /* The node pool belongs to each thread (see the THREAD-LOCAL NODE POOL
   * section of cyaml.c), so a brand new thread must never see the nodes that
   * the pool of the main thread holds when this test runs. Every node_free()
   * call that the worker makes below returns a node to ITS OWN pool, never to
   * the pool of this thread. The test joins the worker before it reads
   * either result field, which obeys the rule of this codebase to join
   * always, before any REQUIRE_*. The pool of the worker thread holds 50
   * nodes when that thread exits, so this test also drives the drain of the
   * pool in the pthread destructor, _pool_drain, under `make memtest`, where
   * a broken drain shows up as a leak of 50 allocations. */
  pool_populate_result_t result = {(size_t)-1, (size_t)-1};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, pool_populate_thread, &result), 0);
  pthread_join(tid, NULL);

  REQUIRE_EQ(result.start_pool_size, (size_t)0);
  /* 50 round trips of one allocate and then one free, starting from an empty
   * pool, grow it by exactly one node for each round trip, because each one
   * is a fresh calloc with nothing left to recycle. That stays well under
   * the cap. */
  REQUIRE_EQ(result.end_pool_size, (size_t)50);
}

typedef struct {
  cyaml *nodes;
  size_t count;
  size_t start_pool_size;
  size_t end_pool_size;
} cross_thread_free_arg_t;

static void *cross_thread_free_thread(void *arg) {
  cross_thread_free_arg_t *a = (cross_thread_free_arg_t *)arg;
  a->start_pool_size = cyaml_debug_pool_size();
  for (size_t i = 0; i < a->count; i++) cyaml_destroy(a->nodes[i]);
  a->end_pool_size = cyaml_debug_pool_size();
  return NULL;
}

TEST(node_pool, node_freed_on_a_different_thread_joins_that_threads_own_pool) {
  /* A node belongs to no particular thread: node_free() always returns it to
   * the thread that calls it NOW, never to the thread that allocated it (see
   * the doc comment of node_free() in cyaml.c). This test allocates every
   * node on the main thread, frees all of them from a worker thread, and
   * then confirms that the freed nodes land in the pool of the WORKER, while
   * the pool count of the main thread does not change at all. */
  size_t count = 20;
  cyaml *nodes = malloc(count * sizeof(cyaml));
  REQUIRE_NE((void *)nodes, NULL);
  for (size_t i = 0; i < count; i++) {
    nodes[i] = cyaml_create_int((long long)i);
    REQUIRE_NE((void *)nodes[i], NULL);
  }

  size_t main_pool_before = cyaml_debug_pool_size();

  cross_thread_free_arg_t arg = {nodes, count, (size_t)-1, (size_t)-1};
  pthread_t tid;
  int create_rv = pthread_create(&tid, NULL, cross_thread_free_thread, &arg);
  if (create_rv == 0) {
    pthread_join(tid, NULL);
  } else {
    /* The worker thread never ran to destroy these nodes, so destroy them here
     * instead, because the REQUIRE_EQ below can return early and leak them. */
    for (size_t i = 0; i < count; i++) cyaml_destroy(nodes[i]);
  }
  free(nodes);

  size_t main_pool_after = cyaml_debug_pool_size();

  REQUIRE_EQ(create_rv, 0);
  REQUIRE_EQ(arg.start_pool_size, (size_t)0);
  REQUIRE_EQ(arg.end_pool_size, count);
  REQUIRE_EQ(main_pool_after, main_pool_before);
}

/* ========================================================================== */
/*                         cyaml_type_str */
/* ========================================================================== */

/* Same contract as cjson_type_str: the spelling reaches users through
 * diagnostics, and a mismapped arm is invisible to every other test here. */
TEST(cyaml_type_str, every_node_type_maps_to_its_own_spelling) {
  cyaml nodes[7];
  nodes[0] = cyaml_create_null();
  nodes[1] = cyaml_create_bool(true);
  nodes[2] = cyaml_create_int(42);
  nodes[3] = cyaml_create_double(1.5);
  nodes[4] = cyaml_create_string("s");
  nodes[5] = cyaml_create_list();
  nodes[6] = cyaml_create_dictionary();

  static const char *expected[7] = {
      "CYAML_NULL",   "CYAML_BOOL", "CYAML_INTEGER",   "CYAML_FLOAT",
      "CYAML_STRING", "CYAML_LIST", "CYAML_DICTIONARY"};

  bool all_created = true, all_named = true;
  for (volatile size_t i = 0; i < 7; i++) {
    if (!nodes[i]) {
      all_created = false;
      continue;
    }
    if (strcmp(cyaml_type_str(nodes[i]), expected[i]) != 0) all_named = false;
  }

  for (size_t i = 0; i < 7; i++)
    if (nodes[i]) __cyaml_destroy(nodes[i]);

  REQUIRE_TRUE(all_created);
  REQUIRE_TRUE(all_named);
}

TEST(cyaml_type_str, a_null_handle_reports_the_null_type) {
  REQUIRE_STREQ(cyaml_type_str(NULL), "CYAML_NULL");
}

/* ========================================================================== */
/*                         PARSE MEMORY BUDGET                                */
/* ========================================================================== */

TEST(errors, alias_expansion_bounded_in_bytes_not_only_in_node_count) {
  /* The budget for node allocation counts nodes, and a node is not a fixed
   * amount of memory: a scalar owns its text, and every reference through
   * an alias deep-copies that text. Take an anchored scalar that several
   * hundred aliases reference: the live DOM grows to hundreds of times the
   * input, while the node count stays in the hundreds, far below the node
   * limit. The byte budget of the parse is what rejects such a document,
   * once the copies pass 512 bytes for each input byte, and it names its own
   * limit instead of giving a generic report that an allocation failed.
   *
   * The test lowers the fixed floor of the byte budget to 1 MiB, so that the
   * scaled limit of this 16 KiB document (about 8.4 MB) decides, and 600
   * copies of the scalar (about 9.8 MB) pass it. This test is not vacuous:
   * remove the byte budget and the document parses successfully. */
  size_t saved = cyaml_test_parse_byte_floor;
  cyaml_test_parse_byte_floor = (size_t)1024 * 1024;
  const size_t scalar_len = 16u * 1024u;
  const int refs = 600;
  size_t cap = scalar_len + (size_t)refs * 4u + 64u;
  char *input = malloc(cap);
  cyaml doc = NULL;
  char *err = NULL;
  bool have_input = input != NULL;
  if (input) {
    size_t off = (size_t)snprintf(input, cap, "a: &a \"");
    memset(input + off, 'x', scalar_len);
    off += scalar_len;
    off += (size_t)snprintf(input + off, cap - off, "\"\nb: [");
    for (int i = 0; i < refs; i++)
      off += (size_t)snprintf(input + off, cap - off, "*a,");
    snprintf(input + off, cap - off, "]\n");
    doc = cyaml_parse(input, &err);
    free(input);
  }
  cyaml_test_parse_byte_floor = saved;
  bool rejected = (doc == NULL);
  bool named_the_limit = (err != NULL && strstr(err, "memory limit") != NULL);
  if (doc) cyaml_destroy(doc);
  REQUIRE_TRUE(have_input);
  REQUIRE_TRUE(rejected);
  REQUIRE_TRUE(named_the_limit);
}

TEST(anchors, large_anchored_scalar_aliased_a_few_times_still_parses) {
  /* The counterpart guard to the byte budget above: a big scalar that a few
   * aliases reuse is ordinary, legitimate document content that sits well
   * inside the budget, and the parser must not reject it. */
  const size_t scalar_len = 1024u * 1024u;
  const int refs = 16;
  size_t cap = scalar_len + (size_t)refs * 4u + 64u;
  char *input = malloc(cap);
  REQUIRE_NE((void *)input, NULL);
  size_t off = (size_t)snprintf(input, cap, "a: &a \"");
  memset(input + off, 'x', scalar_len);
  off += scalar_len;
  off += (size_t)snprintf(input + off, cap - off, "\"\nb: [");
  for (int i = 0; i < refs; i++)
    off += (size_t)snprintf(input + off, cap - off, "*a,");
  snprintf(input + off, cap - off, "]\n");

  char *err = NULL;
  cyaml doc = cyaml_parse(input, &err);
  free(input);
  bool parsed = (doc != NULL);
  bool no_err = (err == NULL);
  size_t len = 0;
  if (doc) {
    cyaml b = cyaml_dictionary_get(doc, "b");
    if (b && cyaml_type(b) == CYAML_LIST) len = cyaml_list_len(b);
    cyaml_destroy(doc);
  }
  REQUIRE_TRUE(parsed);
  REQUIRE_TRUE(no_err);
  REQUIRE_EQ(len, (size_t)refs);
}

/* ========================================================================== */
/*              EXPLICIT KEY: ':' GLUED TO A FOLLOWING KEY                    */
/* ========================================================================== */

TEST(explicit_block_mapping,
     colon_glued_to_the_next_key_is_not_a_value_indicator) {
  /* When a character that is not a space follows a ':', ns-plain-first(c)
   * permits that ':' as the first character of a plain scalar, so the key
   * ":adapter" at the indent of the mapping is an ordinary entry, not the
   * value indicator of the explicit key. That key keeps its null value, and
   * the ":adapter" entry stays an entry of its own.
   *
   * This test is not vacuous: without the check on the next character, the
   * ":adapter" entry disappears completely, and the parser gives "a" a
   * submap that it invented. */
  const char *yaml =
      "? a\n"
      ":adapter: pg\n"
      "x: 1\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  bool parsed = (doc != NULL && err == NULL);
  size_t sz = 0;
  bool a_is_null = false, adapter_ok = false, x_ok = false;
  if (doc) {
    sz = cyaml_dictionary_size(doc);
    cyaml a = cyaml_dictionary_get(doc, "a");
    cyaml adapter = cyaml_dictionary_get(doc, ":adapter");
    cyaml x = cyaml_dictionary_get(doc, "x");
    a_is_null = (a != NULL && cyaml_type(a) == CYAML_NULL);
    adapter_ok = (adapter != NULL && cyaml_type(adapter) == CYAML_STRING &&
                  strcmp(cyaml_str_val(adapter), "pg") == 0);
    x_ok = (x != NULL && cyaml_type(x) == CYAML_INTEGER &&
            cyaml_int_val(x) == 1LL);
    cyaml_destroy(doc);
  }
  REQUIRE_TRUE(parsed);
  REQUIRE_EQ(sz, (size_t)3);
  REQUIRE_TRUE(a_is_null);
  REQUIRE_TRUE(adapter_ok);
  REQUIRE_TRUE(x_ok);
}

TEST(explicit_block_mapping, colon_glued_to_the_next_key_nested_in_a_mapping) {
  /* The identical shape one level in, going through the loop of
   * parse_block_dictionary instead of the document root. */
  const char *yaml =
      "outer:\n"
      "  ? a\n"
      "  :b: c\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  bool parsed = (doc != NULL && err == NULL);
  size_t sz = 0;
  bool a_is_null = false, b_ok = false;
  if (doc) {
    cyaml outer = cyaml_dictionary_get(doc, "outer");
    if (outer && cyaml_type(outer) == CYAML_DICTIONARY) {
      sz = cyaml_dictionary_size(outer);
      cyaml a = cyaml_dictionary_get(outer, "a");
      cyaml b = cyaml_dictionary_get(outer, ":b");
      a_is_null = (a != NULL && cyaml_type(a) == CYAML_NULL);
      b_ok = (b != NULL && cyaml_type(b) == CYAML_STRING &&
              strcmp(cyaml_str_val(b), "c") == 0);
    }
    cyaml_destroy(doc);
  }
  REQUIRE_TRUE(parsed);
  REQUIRE_EQ(sz, (size_t)2);
  REQUIRE_TRUE(a_is_null);
  REQUIRE_TRUE(b_ok);
}

TEST(explicit_block_mapping, colon_glued_to_the_next_key_after_an_empty_key) {
  /* An explicit key with no content of its own, followed by a glued ':'
   * entry. The empty key becomes the string "null" in its canonical form,
   * because the keys of this DOM are plain strings, and the ":s" entry must
   * be its own entry, not the value of that key. */
  const char *yaml =
      "?\n"
      ":s: 1\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  bool parsed = (doc != NULL && err == NULL);
  size_t sz = 0;
  bool s_ok = false, null_key_is_null = false;
  if (doc) {
    sz = cyaml_dictionary_size(doc);
    cyaml s = cyaml_dictionary_get(doc, ":s");
    cyaml nk = cyaml_dictionary_get(doc, "null");
    s_ok = (s != NULL && cyaml_type(s) == CYAML_INTEGER &&
            cyaml_int_val(s) == 1LL);
    null_key_is_null = (nk != NULL && cyaml_type(nk) == CYAML_NULL);
    cyaml_destroy(doc);
  }
  REQUIRE_TRUE(parsed);
  REQUIRE_EQ(sz, (size_t)2);
  REQUIRE_TRUE(s_ok);
  REQUIRE_TRUE(null_key_is_null);
}

TEST(explicit_block_mapping,
     bare_colon_on_its_own_line_is_still_the_value_indicator) {
  /* The legitimate spelling that the check above must leave alone: a ':' at
   * the indent of the mapping, with nothing after it on the line, really is
   * the value indicator of the explicit key and introduces an empty value. */
  const char *yaml =
      "? a\n"
      ":\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  bool parsed = (doc != NULL && err == NULL);
  size_t sz = 0;
  bool a_is_null = false;
  if (doc) {
    sz = cyaml_dictionary_size(doc);
    cyaml a = cyaml_dictionary_get(doc, "a");
    a_is_null = (a != NULL && cyaml_type(a) == CYAML_NULL);
    cyaml_destroy(doc);
  }
  REQUIRE_TRUE(parsed);
  REQUIRE_EQ(sz, (size_t)1);
  REQUIRE_TRUE(a_is_null);
}

/* ========================================================================== */
/*              TAGS NEEDING AN ESCAPE IN VERBATIM OUTPUT                     */
/* ========================================================================== */

TEST(serialize_tags, tag_bytes_that_end_the_verbatim_token_round_trip) {
  /* A tag may legitimately hold a '>', a line break or a '%': the first two
   * end a verbatim "!<...>" token, and the third introduces the escape that
   * the parser decodes. The serializer therefore writes all three as a
   * percent escape, so the document that it writes reparses to the identical
   * tag, with its value whole.
   *
   * This test is not vacuous: write the bytes of the tag with no escape, and
   * the first and last documents below fail to reparse at all, while the
   * middle two reparse to a truncated tag whose value is corrupt or wholly
   * lost. */
  const char *docs[] = {
      "key: !a%3Eb value\n",       /* '>' inside the tag */
      "key: !a%3E%20b value\n",    /* '>' then a space */
      "key: !a%3E%20%23b value\n", /* '>' then " #", which would start a comment
                                    */
      "key: !a%0Ab value\n",       /* a line feed inside the tag */
      "key: !a%25b value\n",       /* a literal '%' inside the tag */
  };
  const char *expected_tags[] = {"!a>b", "!a> b", "!a> #b", "!a\nb", "!a%b"};

  for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(docs[i], &err);
    bool parsed = (doc != NULL && err == NULL);
    bool tag_ok = false, block_ok = false, flow_ok = false;
    if (doc) {
      cyaml v = cyaml_dictionary_get(doc, "key");
      const char *t = v ? cyaml_node_tag(v) : NULL;
      tag_ok = (t != NULL && strcmp(t, expected_tags[i]) == 0);

      char *block = cyaml_serialize(doc);
      if (block) {
        char *err2 = NULL;
        cyaml re = cyaml_parse(block, &err2);
        if (re) {
          cyaml rv = cyaml_dictionary_get(re, "key");
          const char *rt = rv ? cyaml_node_tag(rv) : NULL;
          block_ok = (rt != NULL && strcmp(rt, expected_tags[i]) == 0 &&
                      cyaml_type(rv) == CYAML_STRING &&
                      strcmp(cyaml_str_val(rv), "value") == 0);
          cyaml_destroy(re);
        }
        cyaml_serialize_free(block);
      }

      char *flow = cyaml_serialize_flow(doc);
      if (flow) {
        char *err3 = NULL;
        cyaml re = cyaml_parse(flow, &err3);
        if (re) {
          cyaml rv = cyaml_dictionary_get(re, "key");
          const char *rt = rv ? cyaml_node_tag(rv) : NULL;
          flow_ok = (rt != NULL && strcmp(rt, expected_tags[i]) == 0 &&
                     cyaml_type(rv) == CYAML_STRING &&
                     strcmp(cyaml_str_val(rv), "value") == 0);
          cyaml_destroy(re);
        }
        cyaml_serialize_free(flow);
      }
      cyaml_destroy(doc);
    }
    REQUIRE_TRUE(parsed);
    REQUIRE_TRUE(tag_ok);
    REQUIRE_TRUE(block_ok);
    REQUIRE_TRUE(flow_ok);
  }
}

TEST(serialize_tags,
     tag_set_through_the_api_round_trips_whatever_bytes_it_holds) {
  /* cyaml_node_set_tag() accepts any string, so a tag needing an escape is
   * reachable with no parsing involved at all. A collection's tag takes the
   * other emission path (its own line, ahead of the entries), so both are
   * exercised here. */
  const char *tags[] = {"urn:a> b", "urn:a%b", "urn:a\nb"};
  for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
    cyaml doc = cyaml_create_dictionary();
    REQUIRE_NE((void *)doc, NULL);
    cyaml scalar = cyaml_create_string("value");
    cyaml inner = cyaml_create_list();
    bool built = (scalar != NULL && inner != NULL);
    bool scalar_ok = false, coll_ok = false;
    if (built) {
      built = (cyaml_node_set_tag(scalar, tags[i]) == ccol_success &&
               cyaml_node_set_tag(inner, tags[i]) == ccol_success &&
               cyaml_dictionary_set(doc, "s", scalar) == ccol_success &&
               cyaml_dictionary_set(doc, "l", inner) == ccol_success);
    } else {
      if (scalar) cyaml_destroy(scalar);
      if (inner) cyaml_destroy(inner);
    }
    if (built) {
      char *out = cyaml_serialize(doc);
      if (out) {
        char *err = NULL;
        cyaml re = cyaml_parse(out, &err);
        if (re) {
          cyaml rs = cyaml_dictionary_get(re, "s");
          cyaml rl = cyaml_dictionary_get(re, "l");
          const char *st = rs ? cyaml_node_tag(rs) : NULL;
          const char *lt = rl ? cyaml_node_tag(rl) : NULL;
          scalar_ok = (st != NULL && strcmp(st, tags[i]) == 0);
          coll_ok = (lt != NULL && strcmp(lt, tags[i]) == 0);
          cyaml_destroy(re);
        }
        cyaml_serialize_free(out);
      }
    }
    cyaml_destroy(doc);
    REQUIRE_TRUE(built);
    REQUIRE_TRUE(scalar_ok);
    REQUIRE_TRUE(coll_ok);
  }
}

/* ========================================================================== */
/*                          EMPTY CUSTOM TAG REJECTION                        */
/* ========================================================================== */

/*
 * An empty tag serializes as the verbatim form "!<>", which cyaml_parse()
 * refuses as an empty verbatim tag, so accepting an empty tag would let the
 * library write a document that it cannot read back.
 * cyaml_node_set_tag() therefore rejects it and leaves in place whatever tag
 * the node already carries; NULL is the way to clear a tag.
 *
 * This test is not vacuous: remove the guard on an empty string from
 * cyaml_node_set_tag(), and the first assertion reports ccol_success, after
 * which a serialize and a reparse of the result fail with "empty verbatim
 * tag".
 */
TEST(cyaml_tags,
     an_empty_custom_tag_is_rejected_and_leaves_the_node_untouched) {
  cyaml scalar = cyaml_create_string("hello");
  REQUIRE_NE((void *)scalar, NULL);

  REQUIRE_EQ(cyaml_node_set_tag(scalar, "!keepme"), ccol_success);
  REQUIRE_STREQ(cyaml_node_tag(scalar), "!keepme");

  /* Rejected, and the pre-existing tag survives the rejection. */
  REQUIRE_EQ(cyaml_node_set_tag(scalar, ""), ccol_invalid_args);
  REQUIRE_STREQ(cyaml_node_tag(scalar), "!keepme");

  /* NULL clears the tag, which is the documented way to remove one. */
  REQUIRE_EQ(cyaml_node_set_tag(scalar, NULL), ccol_success);
  REQUIRE_EQ((void *)cyaml_node_tag(scalar), NULL);

  cyaml_destroy(scalar);
}

/* ========================================================================== */
/*        OWNERSHIP: ONE RETURN CODE, ONE RULE ABOUT THE CHILD                */
/* ========================================================================== */

/*
 * cyaml_list_push()/cyaml_dictionary_set() answer ccol_invalid_args for every
 * argument rejection, and every one of them leaves the child exactly as it
 * was, owned by the caller. A caller that acts on the return code alone
 * therefore destroys the child on that code and never on any other code,
 * which is the only way to act on one code at all.
 *
 * The free-list length of the node pool is what makes this observable with no
 * read of freed memory: a node that the default allocator creates comes off
 * that list and goes back onto it when something destroys it, so a rejection
 * that wrongly destroyed the child shows up as one extra entry before the
 * cyaml_destroy() of the caller ever runs.
 *
 * These tests are not vacuous: make any of these rejections destroy the
 * child, and the assertion for "unchanged after the rejection" reports one
 * more pooled node than the call started with.
 */
TEST(ownership, every_rejected_list_push_leaves_the_child_with_the_caller) {
  cyaml wrong_kind = cyaml_create_dictionary();
  REQUIRE_NE((void *)wrong_kind, NULL);

  /* A NULL container. Each of these destroys the child only after the pooled
   * count says that the rejection really did leave it alone, so a build that
   * destroys it instead reports a failed assertion rather than taking the
   * whole binary down with a double free. */
  cyaml child = cyaml_create_int(5);
  size_t pooled = cyaml_debug_pool_size();
  ccol_retval_t r_null = cyaml_list_push(NULL, child);
  size_t pooled_after_null = cyaml_debug_pool_size();
  if (pooled_after_null == pooled) cyaml_destroy(child);

  /* A container that is not a list. */
  child = cyaml_create_int(6);
  size_t pooled2 = cyaml_debug_pool_size();
  ccol_retval_t r_kind = cyaml_list_push(wrong_kind, child);
  size_t pooled_after_kind = cyaml_debug_pool_size();
  if (pooled_after_kind == pooled2) cyaml_destroy(child);

  /* One handle stands as both the container and the child here, and that
   * handle is not a list, so both the self-attach rejection and the
   * rejection on the kind of container apply. */
  ccol_retval_t r_self = cyaml_list_push(wrong_kind, wrong_kind);
  bool wrong_kind_alive = (cyaml_dictionary_size(wrong_kind) == 0);
  cyaml_destroy(wrong_kind);

  REQUIRE_EQ(r_null, ccol_invalid_args);
  REQUIRE_EQ(pooled_after_null, pooled);
  REQUIRE_EQ(r_kind, ccol_invalid_args);
  REQUIRE_EQ(pooled_after_kind, pooled2);
  REQUIRE_EQ(r_self, ccol_invalid_args);
  REQUIRE_TRUE(wrong_kind_alive);
}

TEST(ownership,
     every_rejected_dictionary_set_leaves_the_child_with_the_caller) {
  cyaml wrong_kind = cyaml_create_list();
  cyaml map = cyaml_create_dictionary();
  REQUIRE_NE((void *)wrong_kind, NULL);
  REQUIRE_NE((void *)map, NULL);

  /* Each of these destroys the child only after the pooled count says that
   * the rejection really did leave it alone, so a build that destroys it
   * instead reports a failed assertion rather than taking the whole binary
   * down with a double free. */
  cyaml child = cyaml_create_int(5);
  size_t pooled_null = cyaml_debug_pool_size();
  ccol_retval_t r_null = cyaml_dictionary_set(NULL, "k", child);
  size_t pooled_after_null = cyaml_debug_pool_size();
  if (pooled_after_null == pooled_null) cyaml_destroy(child);

  child = cyaml_create_int(6);
  size_t pooled_key = cyaml_debug_pool_size();
  ccol_retval_t r_key = cyaml_dictionary_set(map, NULL, child);
  size_t pooled_after_key = cyaml_debug_pool_size();
  if (pooled_after_key == pooled_key) cyaml_destroy(child);

  child = cyaml_create_int(7);
  size_t pooled_kind = cyaml_debug_pool_size();
  ccol_retval_t r_kind = cyaml_dictionary_set(wrong_kind, "k", child);
  size_t pooled_after_kind = cyaml_debug_pool_size();
  if (pooled_after_kind == pooled_kind) cyaml_destroy(child);

  /* One handle stands as both the container and the child here, and that
   * handle is not a dictionary, so both the self-attach rejection and the
   * rejection on the kind of container apply. */
  ccol_retval_t r_self = cyaml_dictionary_set(wrong_kind, "k", wrong_kind);
  bool wrong_kind_alive = (cyaml_list_len(wrong_kind) == 0);

  cyaml_destroy(wrong_kind);
  cyaml_destroy(map);

  REQUIRE_EQ(r_null, ccol_invalid_args);
  REQUIRE_EQ(pooled_after_null, pooled_null);
  REQUIRE_EQ(r_key, ccol_invalid_args);
  REQUIRE_EQ(pooled_after_key, pooled_key);
  REQUIRE_EQ(r_kind, ccol_invalid_args);
  REQUIRE_EQ(pooled_after_kind, pooled_kind);
  REQUIRE_EQ(r_self, ccol_invalid_args);
  REQUIRE_TRUE(wrong_kind_alive);
}

/*
 * A caller often does not know whether a lookup found a container, and it then
 * writes this shape: push, and destroy the child when the push says no. A
 * rejection that also destroyed the child would put that one node onto the free
 * list of the pool two times, so its "next" pointer would aim at itself, and
 * the next two allocations would hand the same address out two times.
 *
 * This test is not vacuous: make the rejection for a NULL container destroy
 * the child, and the pooled count grows across the rejection, which the
 * assertion below checks. The destroy depends on that count, so the mutated
 * build reports a failure instead of walking a free list that points at
 * itself, which neither fails nor returns.
 */
TEST(ownership, a_rejected_push_never_recycles_one_node_twice) {
  cyaml doc = cyaml_parse("a: 1\n", NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml absent = cyaml_get(doc, "nope");
  REQUIRE_EQ((void *)absent, NULL);

  cyaml child = cyaml_create_int(5);
  size_t pooled = cyaml_debug_pool_size();
  ccol_retval_t r = cyaml_list_push(absent, child);
  size_t pooled_after = cyaml_debug_pool_size();
  if (r != ccol_success && pooled_after == pooled) cyaml_destroy(child);

  /* The pool still hands out one node per request. */
  cyaml first = cyaml_create_int(1);
  cyaml second = cyaml_create_int(2);
  bool distinct = ((void *)first != (void *)second);
  if (distinct) cyaml_destroy(second);
  cyaml_destroy(first);
  cyaml_destroy(doc);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(pooled_after, pooled);
  REQUIRE_TRUE(distinct);
}

/* ========================================================================== */
/*        _cyaml_set_typed: raw_size DESCRIBES THE OBJECT raw NAMES           */
/* ========================================================================== */

/*
 * _cyaml_set_typed() reads *raw as a pointer to character for CYAML_STRING,
 * as an integer for CYAML_INTEGER and as a floating value for CYAML_FLOAT, so
 * it checks raw_size against that width before it reads anything. Without
 * this, a caller can describe a narrower object, and the function then reads
 * sizeof(const char *) bytes out of that object and hands whatever follows
 * on as a string.
 *
 * This test is not vacuous: remove the check on raw_size for CYAML_STRING,
 * and the first assertion reports something other than ccol_invalid_args,
 * while AddressSanitizer reports a stack-buffer-overflow read at the same
 * point.
 */
TEST(cyaml_set_typed, a_string_write_checks_the_width_of_the_object_raw_names) {
  cyaml doc = cyaml_parse("k: 1\n", NULL);
  REQUIRE_NE((void *)doc, NULL);

  char too_narrow = 'q';
  ccol_retval_t r_narrow = _cyaml_set_typed(doc, "k", CYAML_STRING, &too_narrow,
                                            sizeof(too_narrow), false);
  /* The leaf is left exactly as it was. */
  long long still_int = cyaml_int_val(cyaml_get(doc, "k"));

  const char *proper = "hello";
  ccol_retval_t r_ok =
      _cyaml_set_typed(doc, "k", CYAML_STRING, &proper, sizeof(proper), false);
  const char *written = cyaml_str_val(cyaml_get(doc, "k"));
  bool written_ok = (written && strcmp(written, "hello") == 0);

  cyaml_destroy(doc);

  REQUIRE_EQ(r_narrow, ccol_invalid_args);
  REQUIRE_EQ(still_int, 1LL);
  REQUIRE_EQ(r_ok, ccol_success);
  REQUIRE_TRUE(written_ok);
}

/* A bool payload must be exactly sizeof(bool) wide, and only CYAML_NULL may
 * come with no payload at all. Each refusal leaves the leaf untouched, both
 * for a key that exists and for one that does not. */
TEST(cyaml_set_typed, a_bool_write_checks_its_width_and_raw_must_be_present) {
  cyaml doc = cyaml_parse("k: 1\n", NULL);
  REQUIRE_NE((void *)doc, NULL);

  int not_a_bool = 0x0100;
  ccol_retval_t r_wide = _cyaml_set_typed(doc, "k", CYAML_BOOL, &not_a_bool,
                                          sizeof(not_a_bool), false);
  ccol_retval_t r_new = _cyaml_set_typed(doc, "n", CYAML_BOOL, &not_a_bool,
                                         sizeof(not_a_bool), false);
  ccol_retval_t r_null_bool =
      _cyaml_set_typed(doc, "k", CYAML_BOOL, NULL, sizeof(bool), false);
  ccol_retval_t r_null_int =
      _cyaml_set_typed(doc, "k", CYAML_INTEGER, NULL, sizeof(int), true);
  ccol_retval_t r_null_float =
      _cyaml_set_typed(doc, "k", CYAML_FLOAT, NULL, sizeof(double), false);
  ccol_retval_t r_null_str =
      _cyaml_set_typed(doc, "k", CYAML_STRING, NULL, sizeof(char *), false);
  cyaml k = cyaml_get(doc, "k");
  bool untouched = k && cyaml_type(k) == CYAML_INTEGER && cyaml_int_val(k) == 1;
  bool no_new_key = cyaml_get(doc, "n") == NULL;

  bool b = true;
  ccol_retval_t r_ok =
      _cyaml_set_typed(doc, "k", CYAML_BOOL, &b, sizeof(b), false);
  ccol_retval_t r_null_ok =
      _cyaml_set_typed(doc, "z", CYAML_NULL, NULL, 0, false);
  k = cyaml_get(doc, "k");
  bool bool_ok = k && cyaml_type(k) == CYAML_BOOL && cyaml_bool_val(k);
  bool null_ok = cyaml_type(cyaml_get(doc, "z")) == CYAML_NULL &&
                 cyaml_get(doc, "z") != NULL;

  cyaml_destroy(doc);

  REQUIRE_EQ(r_wide, ccol_invalid_args);
  REQUIRE_EQ(r_new, ccol_invalid_args);
  REQUIRE_EQ(r_null_bool, ccol_invalid_args);
  REQUIRE_EQ(r_null_int, ccol_invalid_args);
  REQUIRE_EQ(r_null_float, ccol_invalid_args);
  REQUIRE_EQ(r_null_str, ccol_invalid_args);
  REQUIRE_TRUE(untouched);
  REQUIRE_TRUE(no_new_key);
  REQUIRE_EQ(r_ok, ccol_success);
  REQUIRE_EQ(r_null_ok, ccol_success);
  REQUIRE_TRUE(bool_ok);
  REQUIRE_TRUE(null_ok);
}

/* ========================================================================== */
/*        A SCALAR THAT STARTS WITH A BYTE ORDER MARK                         */
/* ========================================================================== */

/*
 * A U+FEFF in UTF-8 at offset 0 of a document is a byte order mark, not
 * content, and a parse removes it, so the serializer quotes a scalar whose own
 * first character is U+FEFF. Without that, a dictionary whose first key starts
 * with one serializes with the mark at offset 0 and then reparses under a
 * shorter key, losing both the original key and its value.
 *
 * This test is not vacuous: remove the clause for a byte order mark from
 * needs_quoting(), and the dictionary that makes the round trip answers NULL
 * for the original key, while the root scalar comes back as "z".
 */
TEST(serialize, a_scalar_beginning_with_a_byte_order_mark_round_trips) {
#define CYAML_TEST_BOM_KEY \
  "\xEF\xBB\xBF"           \
  "z"
  cyaml map = cyaml_create_dictionary();
  REQUIRE_NE((void *)map, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(map, CYAML_TEST_BOM_KEY, cyaml_create_int(7)),
             ccol_success);

  char *block = cyaml_serialize(map);
  cyaml block_back = block ? cyaml_parse(block, NULL) : NULL;
  cyaml block_val =
      block_back ? cyaml_dictionary_get(block_back, CYAML_TEST_BOM_KEY) : NULL;
  bool block_ok = (block_val && cyaml_int_val(block_val) == 7);

  char *flow = cyaml_serialize_flow(map);
  cyaml flow_back = flow ? cyaml_parse(flow, NULL) : NULL;
  cyaml flow_val =
      flow_back ? cyaml_dictionary_get(flow_back, CYAML_TEST_BOM_KEY) : NULL;
  bool flow_ok = (flow_val && cyaml_int_val(flow_val) == 7);

  cyaml root = cyaml_create_string(CYAML_TEST_BOM_KEY);
  char *root_block = root ? cyaml_serialize(root) : NULL;
  cyaml root_back = root_block ? cyaml_parse(root_block, NULL) : NULL;
  const char *root_str = (root_back && cyaml_type(root_back) == CYAML_STRING)
                             ? cyaml_str_val(root_back)
                             : NULL;
  bool root_ok = (root_str && strcmp(root_str, CYAML_TEST_BOM_KEY) == 0);

  if (block_back) cyaml_destroy(block_back);
  if (flow_back) cyaml_destroy(flow_back);
  if (root_back) cyaml_destroy(root_back);
  if (root) cyaml_destroy(root);
  cyaml_serialize_free(block);
  cyaml_serialize_free(flow);
  cyaml_serialize_free(root_block);
  cyaml_destroy(map);

  REQUIRE_TRUE(block_ok);
  REQUIRE_TRUE(flow_ok);
  REQUIRE_TRUE(root_ok);
#undef CYAML_TEST_BOM_KEY
}

/* ========================================================================== */
/*        DEEP TREES: WALKED WITH A WORKLIST, NOT WITH THE CALL STACK         */
/* ========================================================================== */

/* The deepest tree that a walk of cyaml_clone() or cyaml_serialize*()
 * accepts, counting nested containers and the root. It is one more than the
 * caps that those walks apply to their children, which are a depth below the
 * root. This file states the number here instead of reading it back from the
 * constants of the library, because a test that pins a limit must not move
 * with the knob that it tests. */
#define CYAML_TEST_MAX_WALK_LEVELS 501

/* This builds a chain of `levels` nested containers in which a list and a
 * dictionary alternate, so a walk over the chain drives both kinds of
 * traversal frame at every depth. It gives NULL when any allocation fails,
 * leaving nothing behind. */
static cyaml cyaml_test_build_deep_chain(size_t levels,
                                         ccol_memmgmt_procs_t *mp) {
  if (levels == 0) return NULL;
  cyaml root = cyaml_create_list_mp(mp);
  if (!root) return NULL;
  cyaml cur = root;
  for (size_t i = 1; i < levels; i++) {
    cyaml next =
        (i % 2) ? cyaml_create_dictionary_mp(mp) : cyaml_create_list_mp(mp);
    if (!next) {
      cyaml_destroy(root);
      return NULL;
    }
    ccol_retval_t r = (cyaml_type(cur) == CYAML_LIST)
                          ? cyaml_list_push(cur, next)
                          : cyaml_dictionary_set(cur, "n", next);
    if (r != ccol_success) {
      /* Every failure reachable here takes ownership of next, so root is
       * all that is left to release. */
      cyaml_destroy(root);
      return NULL;
    }
    cur = next;
  }
  return root;
}

/* An allocator that injects a fault and also tracks how many blocks are live
 * now. The single-fault allocator above reaches every failure path on its own,
 * but the live count is what turns "this failure path leaks" into a failed
 * assertion in this suite; without it, only a leak checker over the whole
 * binary would notice. A worklist walk attaches each container that it hands
 * out to its parent before it walks the children of that container, so a
 * failure at any depth leaves the whole partial result reachable from one
 * root. A walk that instead kept a container with no parent in a frame would
 * leak one container for each open level here. */
static int g_walk_fail_at = -1;
static int g_walk_call_idx;
static long g_walk_live;

static void *walk_fault_malloc(size_t sz) {
  int idx = g_walk_call_idx++;
  if (g_walk_fail_at >= 0 && idx == g_walk_fail_at) return NULL;
  void *p = malloc(sz);
  if (p) g_walk_live++;
  return p;
}
static void *walk_fault_calloc(size_t n, size_t sz) {
  int idx = g_walk_call_idx++;
  if (g_walk_fail_at >= 0 && idx == g_walk_fail_at) return NULL;
  void *p = calloc(n, sz);
  if (p) g_walk_live++;
  return p;
}
static void *walk_fault_realloc(void *p, size_t sz) {
  int idx = g_walk_call_idx++;
  if (g_walk_fail_at >= 0 && idx == g_walk_fail_at) return NULL;
  void *q = realloc(p, sz);
  if (q && !p) g_walk_live++;
  return q;
}
static void walk_fault_free(void *p) {
  if (p) g_walk_live--;
  free(p);
}
static ccol_memmgmt_procs_t g_walk_fault_mp = {.malloc = walk_fault_malloc,
                                               .calloc = walk_fault_calloc,
                                               .realloc = walk_fault_realloc,
                                               .free = walk_fault_free};

typedef struct {
  cyaml tree;
  bool clone_ok;
  bool block_ok;
  bool flow_ok;
} cyaml_deep_walk_args_t;

static void *cyaml_deep_walk_thread(void *arg) {
  cyaml_deep_walk_args_t *a = (cyaml_deep_walk_args_t *)arg;
  cyaml copy = cyaml_clone(a->tree);
  a->clone_ok = (copy != NULL);
  if (copy) cyaml_destroy(copy);
  char *block = cyaml_serialize(a->tree);
  a->block_ok = (block != NULL);
  cyaml_serialize_free(block);
  char *flow = cyaml_serialize_flow(a->tree);
  a->flow_ok = (flow != NULL);
  cyaml_serialize_free(flow);
  return NULL;
}

/*
 * cyaml_clone() and both serializers walk their tree with an explicit stack
 * of container frames, so the native call stack that they need is a constant
 * that does not grow with the depth of the nesting, and a tree at the depth
 * that the caps accept runs on a thread with a small stack. That is what makes
 * those caps limits of policy rather than a restatement of whatever stack the
 * caller happens to have.
 *
 * This test is not vacuous: a walk of the tree by recursion needs about 60
 * KiB for the clone at this depth and about 84 KiB for either serializer, so
 * the worker thread dies on its 64 KiB stack and reports nothing.
 */
TEST(deep_trees, a_tree_at_the_depth_cap_is_walked_on_a_small_thread_stack) {
  cyaml_deep_walk_args_t args = {NULL, false, false, false};
  args.tree = cyaml_test_build_deep_chain(CYAML_TEST_MAX_WALK_LEVELS, NULL);
  REQUIRE_NE((void *)args.tree, NULL);

  size_t stack_bytes = 64u * 1024u;
  if (stack_bytes < (size_t)PTHREAD_STACK_MIN)
    stack_bytes = (size_t)PTHREAD_STACK_MIN;

  /* The code captures every outcome into a local and releases every resource
   * before the first assertion, so an assertion that fails and returns early
   * cannot leave the tree or the worker behind. */
  pthread_attr_t attr;
  int attr_ok = pthread_attr_init(&attr);
  int size_ok =
      (attr_ok == 0) ? pthread_attr_setstacksize(&attr, stack_bytes) : -1;
  pthread_t tid;
  int created = (size_ok == 0)
                    ? pthread_create(&tid, &attr, cyaml_deep_walk_thread, &args)
                    : -1;
  if (created == 0) pthread_join(tid, NULL);
  if (attr_ok == 0) pthread_attr_destroy(&attr);
  cyaml_destroy(args.tree);

  REQUIRE_EQ(attr_ok, 0);
  REQUIRE_EQ(size_ok, 0);
  REQUIRE_EQ(created, 0);
  REQUIRE_TRUE(args.clone_ok);
  REQUIRE_TRUE(args.block_ok);
  REQUIRE_TRUE(args.flow_ok);
}

/*
 * The depth caps apply at exactly the nesting that they name: a walk accepts
 * a tree whose deepest node sits CYAML_TEST_MAX_WALK_LEVELS - 1 levels below
 * the root, and refuses one level more by returning NULL.
 */
TEST(deep_trees, the_walk_depth_caps_are_enforced_at_their_own_boundary) {
  cyaml at_cap = cyaml_test_build_deep_chain(CYAML_TEST_MAX_WALK_LEVELS, NULL);
  cyaml past_cap =
      cyaml_test_build_deep_chain(CYAML_TEST_MAX_WALK_LEVELS + 1, NULL);
  REQUIRE_NE((void *)at_cap, NULL);
  REQUIRE_NE((void *)past_cap, NULL);

  cyaml at_clone = cyaml_clone(at_cap);
  char *at_block = cyaml_serialize(at_cap);
  char *at_flow = cyaml_serialize_flow(at_cap);
  cyaml past_clone = cyaml_clone(past_cap);
  char *past_block = cyaml_serialize(past_cap);
  char *past_flow = cyaml_serialize_flow(past_cap);

  bool at_ok = (at_clone && at_block && at_flow);
  bool past_refused = (!past_clone && !past_block && !past_flow);

  if (at_clone) cyaml_destroy(at_clone);
  if (past_clone) cyaml_destroy(past_clone);
  cyaml_serialize_free(at_block);
  cyaml_serialize_free(at_flow);
  cyaml_serialize_free(past_block);
  cyaml_serialize_free(past_flow);
  cyaml_destroy(at_cap);
  cyaml_destroy(past_cap);

  REQUIRE_TRUE(at_ok);
  REQUIRE_TRUE(past_refused);
}

/*
 * A deep clone and a deep serialization each make many allocations, and this
 * test fails each one in turn, including the growth of the traversal stack
 * itself. Each failure must come back as NULL, with everything that the walk
 * took already released. The allocator counts live blocks, so a walk that
 * abandoned a subtree that it had part built would leave the count above what
 * the source tree itself holds. The chain is deeper than the inline capacity
 * of the traversal stack, so the sweep also covers the allocations for
 * growth.
 */
TEST(deep_trees, a_deep_walk_reports_every_allocation_failure_cleanly) {
  g_walk_fail_at = -1;
  g_walk_call_idx = 0;
  g_walk_live = 0;
  cyaml tree = cyaml_test_build_deep_chain(40, &g_walk_fault_mp);
  REQUIRE_NE((void *)tree, NULL);
  long tree_blocks = g_walk_live;

  long clone_failures = 0, block_failures = 0, flow_failures = 0;
  long max_excess = 0;
  int swept = 0;
  for (int i = 0; i < 4000; i++) {
    g_walk_call_idx = 0;
    g_walk_fail_at = i;
    cyaml copy = cyaml_clone(tree);
    int clone_calls = g_walk_call_idx;
    if (copy)
      cyaml_destroy(copy);
    else
      clone_failures++;
    if (g_walk_live - tree_blocks > max_excess)
      max_excess = g_walk_live - tree_blocks;

    g_walk_call_idx = 0;
    g_walk_fail_at = i;
    char *block = cyaml_serialize(tree);
    int block_calls = g_walk_call_idx;
    if (block)
      cyaml_serialize_free_mp(block, &g_walk_fault_mp);
    else
      block_failures++;
    if (g_walk_live - tree_blocks > max_excess)
      max_excess = g_walk_live - tree_blocks;

    g_walk_call_idx = 0;
    g_walk_fail_at = i;
    char *flow = cyaml_serialize_flow(tree);
    int flow_calls = g_walk_call_idx;
    if (flow)
      cyaml_serialize_free_mp(flow, &g_walk_fault_mp);
    else
      flow_failures++;
    if (g_walk_live - tree_blocks > max_excess)
      max_excess = g_walk_live - tree_blocks;

    swept = i + 1;
    if (clone_calls <= i && block_calls <= i && flow_calls <= i) break;
  }
  g_walk_fail_at = -1;
  g_walk_call_idx = 0;

  /* With no injected failure everything succeeds again, so the sweep left no
   * state behind. */
  cyaml copy = cyaml_clone(tree);
  char *block = cyaml_serialize(tree);
  bool recovered = (copy != NULL && block != NULL);
  if (copy) cyaml_destroy(copy);
  cyaml_serialize_free_mp(block, &g_walk_fault_mp);
  cyaml_destroy(tree);
  long final_live = g_walk_live;

  REQUIRE_GT(swept, 40);
  REQUIRE_GT(clone_failures, 0L);
  REQUIRE_GT(block_failures, 0L);
  REQUIRE_GT(flow_failures, 0L);
  REQUIRE_TRUE(recovered);
  /* No failure path abandoned a block, and the source tree's own teardown
   * released everything it held. */
  REQUIRE_EQ(max_excess, 0L);
  REQUIRE_EQ(final_live, 0L);
}

/* ========================================================================== */
/*        PARSING: DRIVEN BY A WORKLIST, NOT BY THE CALL STACK                */
/* ========================================================================== */

/* The deepest nesting that a document may have before cyaml_parse*() refuses
 * it. This file states the number here instead of reading it back from the
 * constant of the library, because a test that pins a limit must not move
 * with the knob that it tests. */
#define CYAML_TEST_MAX_PARSE_LEVELS 500

/* This builds a document that nests `levels` flow sequences: "[[[ ... ]]]". */
static char *cyaml_test_deep_flow_doc(size_t levels) {
  char *s = (char *)malloc(levels * 2 + 2);
  if (!s) return NULL;
  for (size_t i = 0; i < levels; i++) s[i] = '[';
  for (size_t i = 0; i < levels; i++) s[levels + i] = ']';
  s[levels * 2] = '\n';
  s[levels * 2 + 1] = '\0';
  return s;
}

/* This builds a document that nests `levels` block mappings, each one column
 * further in: "k:\n k:\n  k:\n ...". */
static char *cyaml_test_deep_block_doc(size_t levels) {
  size_t cap = levels * (levels + 5) + 8;
  char *s = (char *)malloc(cap);
  if (!s) return NULL;
  size_t p = 0;
  for (size_t i = 0; i < levels; i++) {
    for (size_t j = 0; j < i; j++) s[p++] = ' ';
    s[p++] = 'k';
    s[p++] = ':';
    s[p++] = '\n';
  }
  s[p] = '\0';
  return s;
}

typedef struct {
  const char *doc;
  bool parsed;
  bool saw_depth_error;
} cyaml_deep_parse_args_t;

static void *cyaml_deep_parse_thread(void *arg) {
  cyaml_deep_parse_args_t *a = (cyaml_deep_parse_args_t *)arg;
  char *err = NULL;
  cyaml doc = cyaml_parse(a->doc, &err);
  a->parsed = (doc != NULL);
  a->saw_depth_error =
      (err != NULL &&
       strstr(err, "maximum nesting depth (500) exceeded") != NULL);
  if (doc) cyaml_destroy(doc);
  return NULL;
}

/* This runs one parse on a thread with a 64 KiB stack and reports what that
 * parse did. */
static bool cyaml_run_parse_on_small_stack(const char *doc, bool *parsed_out,
                                           bool *depth_error_out) {
  cyaml_deep_parse_args_t args = {doc, false, false};
  size_t stack_bytes = 64u * 1024u;
  if (stack_bytes < (size_t)PTHREAD_STACK_MIN)
    stack_bytes = (size_t)PTHREAD_STACK_MIN;
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return false;
  bool ok = (pthread_attr_setstacksize(&attr, stack_bytes) == 0);
  pthread_t tid;
  if (ok)
    ok = (pthread_create(&tid, &attr, cyaml_deep_parse_thread, &args) == 0);
  if (ok) pthread_join(tid, NULL);
  pthread_attr_destroy(&attr);
  *parsed_out = args.parsed;
  *depth_error_out = args.saw_depth_error;
  return ok;
}

/*
 * An explicit stack of frames drives the parse, so the native stack that one
 * parse needs is a constant that does not grow with the depth of the nesting
 * of the document, and a document at exactly the depth that cyaml_parse*()
 * accepts parses on a thread with a small stack. That is what makes that cap
 * a limit of policy rather than a restatement of whatever stack the caller
 * happens to have. A document one level past the cap is refused with the
 * documented error instead of overrunning the stack first.
 *
 * This test is not vacuous: a parse by recursion needs about 204 KiB for the
 * flow document at this depth and about 220 KiB for the block one, so the
 * worker thread dies on its 64 KiB stack and reports nothing, and the guard
 * on the depth of the nesting never runs at all.
 */
TEST(deep_documents, a_document_at_the_depth_cap_parses_on_a_small_thread) {
  char *at_cap = cyaml_test_deep_flow_doc(CYAML_TEST_MAX_PARSE_LEVELS);
  char *past_cap = cyaml_test_deep_flow_doc(CYAML_TEST_MAX_PARSE_LEVELS + 1);
  char *block = cyaml_test_deep_block_doc(CYAML_TEST_MAX_PARSE_LEVELS);
  REQUIRE_NE((void *)at_cap, NULL);
  REQUIRE_NE((void *)past_cap, NULL);
  REQUIRE_NE((void *)block, NULL);

  bool at_parsed = false, at_depth_err = false;
  bool past_parsed = false, past_depth_err = false;
  bool block_parsed = false, block_depth_err = false;
  bool ran_at =
      cyaml_run_parse_on_small_stack(at_cap, &at_parsed, &at_depth_err);
  bool ran_past =
      cyaml_run_parse_on_small_stack(past_cap, &past_parsed, &past_depth_err);
  bool ran_block =
      cyaml_run_parse_on_small_stack(block, &block_parsed, &block_depth_err);

  free(at_cap);
  free(past_cap);
  free(block);

  REQUIRE_TRUE(ran_at);
  REQUIRE_TRUE(ran_past);
  REQUIRE_TRUE(ran_block);
  REQUIRE_TRUE(at_parsed);
  REQUIRE_TRUE(block_parsed);
  REQUIRE_FALSE(past_parsed);
  /* One level past the cap, the guard is what refuses it, by name. */
  REQUIRE_TRUE(past_depth_err);
  REQUIRE_FALSE(at_depth_err);
}

/*
 * A document can open many nested collections and then fail part way,
 * leaving one container under construction for each open level, with no root
 * that reaches any of them. The parser must release every one of them when it
 * abandons the parse. The allocator counts live blocks, so a frame that
 * abandoned its container would leave the count above zero.
 *
 * This test is not vacuous: release the container of the innermost frame
 * alone, instead of sweeping the whole stack, and hundreds of blocks stay live
 * at the end of each of these parses.
 */
TEST(deep_documents, an_abandoned_deep_parse_releases_every_open_container) {
  static const char *shapes[] = {"[[[[",
                                 "{{{{",
                                 "[ [ [ [ ",
                                 "[1, [2, [3, [4, ",
                                 "{a: {b: {c: {d: ",
                                 "[[[[@]]]]",
                                 "[{a: [{b: [{c: ",
                                 "&x [[[[",
                                 "!!seq [[[[",
                                 "[[[[\n\n  ]"};
  g_walk_fail_at = -1;
  g_walk_call_idx = 0;
  g_walk_live = 0;

  long max_live = 0;
  bool any_parsed = false;
  for (size_t i = 0; i < sizeof(shapes) / sizeof(*shapes); i++) {
    for (int depth = 1; depth <= 200; depth += 37) {
      /* Repeat the shape `depth` times, so the abandoned stack is deep. */
      size_t unit = strlen(shapes[i]);
      char *doc = (char *)malloc(unit * (size_t)depth + 2);
      if (!doc) continue;
      size_t p = 0;
      for (int k = 0; k < depth; k++) {
        memcpy(doc + p, shapes[i], unit);
        p += unit;
      }
      doc[p++] = '\n';
      doc[p] = '\0';
      char *err = NULL;
      cyaml parsed = cyaml_parse_mp(doc, &err, &g_walk_fault_mp);
      if (parsed) {
        any_parsed = true;
        cyaml_destroy(parsed);
      }
      free(doc);
      if (g_walk_live > max_live) max_live = g_walk_live;
    }
  }
  long final_live = g_walk_live;

  /* Every one of these shapes is malformed, so each parse is abandoned. */
  REQUIRE_FALSE(any_parsed);
  REQUIRE_EQ(max_live, 0L);
  REQUIRE_EQ(final_live, 0L);
}

/*
 * Every allocation that a deep parse makes, the growth of the frame stack
 * itself included, is failed in turn, and each failure must be reported with
 * nothing left allocated.
 */
TEST(deep_documents, a_deep_parse_reports_every_allocation_failure_cleanly) {
  char *doc = cyaml_test_deep_flow_doc(60);
  REQUIRE_NE((void *)doc, NULL);

  g_walk_fail_at = -1;
  g_walk_call_idx = 0;
  g_walk_live = 0;

  long failures = 0, successes = 0, max_live = 0;
  int swept = 0;
  for (int i = 0; i < 6000; i++) {
    g_walk_call_idx = 0;
    g_walk_fail_at = i;
    char *err = NULL;
    cyaml parsed = cyaml_parse_mp(doc, &err, &g_walk_fault_mp);
    int calls = g_walk_call_idx;
    if (parsed) {
      successes++;
      cyaml_destroy(parsed);
    } else {
      failures++;
    }
    if (g_walk_live > max_live) max_live = g_walk_live;
    swept = i + 1;
    if (calls <= i) break;
  }
  g_walk_fail_at = -1;
  g_walk_call_idx = 0;

  char *err = NULL;
  cyaml parsed = cyaml_parse_mp(doc, &err, &g_walk_fault_mp);
  bool recovered = (parsed != NULL);
  if (parsed) cyaml_destroy(parsed);
  long final_live = g_walk_live;
  free(doc);

  REQUIRE_GT(swept, 60);
  REQUIRE_GT(failures, 0L);
  REQUIRE_EQ(max_live, 0L);
  REQUIRE_EQ(final_live, 0L);
  REQUIRE_TRUE(recovered);
  (void)successes;
}

/*
 * When a block mapping entry starts with a '[', a '{', a '*' or a node
 * property, the parser reads it speculatively as a key: it parses the content
 * in full and, when no ':' follows, rewinds the position and reads the same
 * bytes again as something else. For a flow collection that speculative read
 * is a complete nested parse of its own, which shares the nesting budget of
 * this parse and must leave both the position and that budget exactly as it
 * found them. A test build checks this directly; see
 * try_parse_scalar_dict_key.
 *
 * The pairs below differ only in the ':' that decides the question, so each
 * one pins the accepted reading of the same bytes against the reading that
 * the parser declines.
 */
TEST(deep_documents, a_declined_key_speculation_re_reads_the_same_bytes) {
  cyaml as_value = cyaml_parse("&x [1, 2]\n", NULL);
  cyaml as_key = cyaml_parse("&x [1, 2]: v\n", NULL);
  cyaml map_value = cyaml_parse("&x {a: 1}\n", NULL);
  cyaml map_key = cyaml_parse("&x {a: 1}: v\n", NULL);
  cyaml alias_value = cyaml_parse("a: &r 1\nb: *r\n", NULL);
  cyaml alias_key = cyaml_parse("a: &r 1\n*r : v\n", NULL);

  bool as_value_ok = (as_value && cyaml_type(as_value) == CYAML_LIST &&
                      cyaml_list_len(as_value) == 2);
  bool as_key_ok = (as_key && cyaml_type(as_key) == CYAML_DICTIONARY &&
                    cyaml_dictionary_get(as_key, "[1, 2]") != NULL);
  bool map_value_ok = (map_value && cyaml_type(map_value) == CYAML_DICTIONARY &&
                       cyaml_dictionary_get(map_value, "a") != NULL);
  bool map_key_ok = (map_key && cyaml_type(map_key) == CYAML_DICTIONARY &&
                     cyaml_dictionary_get(map_key, "{a: 1}") != NULL);
  bool alias_value_ok =
      (alias_value && cyaml_dictionary_get(alias_value, "b") != NULL);
  bool alias_key_ok =
      (alias_key && cyaml_dictionary_get(alias_key, "1") != NULL);

  /* A speculation deep enough to be a traversal of several levels on its
   * own, which the parser runs and then discards. */
  char *deep = cyaml_test_deep_flow_doc(200);
  char *deep_doc = NULL;
  cyaml deep_value = NULL;
  if (deep) {
    deep_doc = (char *)malloc(strlen(deep) + 8);
    if (deep_doc) {
      strcpy(deep_doc, "&x ");
      strcat(deep_doc, deep);
      deep_value = cyaml_parse(deep_doc, NULL);
    }
  }
  bool deep_ok = (deep_value != NULL && cyaml_type(deep_value) == CYAML_LIST);

  if (as_value) cyaml_destroy(as_value);
  if (as_key) cyaml_destroy(as_key);
  if (map_value) cyaml_destroy(map_value);
  if (map_key) cyaml_destroy(map_key);
  if (alias_value) cyaml_destroy(alias_value);
  if (alias_key) cyaml_destroy(alias_key);
  if (deep_value) cyaml_destroy(deep_value);
  free(deep_doc);
  free(deep);

  REQUIRE_TRUE(as_value_ok);
  REQUIRE_TRUE(as_key_ok);
  REQUIRE_TRUE(map_value_ok);
  REQUIRE_TRUE(map_key_ok);
  REQUIRE_TRUE(alias_value_ok);
  REQUIRE_TRUE(alias_key_ok);
  REQUIRE_TRUE(deep_ok);
}

/* This builds a document of `outer` nested block mappings whose innermost
 * entry is an anchored flow sequence, nested `inner` levels deep, that stands
 * as a mapping key. The parser reads that sequence speculatively, and the ':'
 * after it is what makes the speculation succeed, so the parser never reads
 * the content that it consumed again. Without that ':', the same document
 * reaches the identical depth a second time, through the ordinary value path,
 * which is why the accepted form is the one that tells a shared nesting
 * budget from a separate one. */
static char *cyaml_test_nested_speculation_doc(size_t outer, size_t inner) {
  size_t cap = outer * (outer + 8) + inner * 2 + 64;
  char *s = (char *)malloc(cap);
  if (!s) return NULL;
  size_t p = 0;
  for (size_t i = 0; i < outer; i++) {
    for (size_t j = 0; j < i; j++) s[p++] = ' ';
    s[p++] = 'k';
    s[p++] = ':';
    s[p++] = '\n';
  }
  for (size_t j = 0; j < outer; j++) s[p++] = ' ';
  s[p++] = '&';
  s[p++] = 'x';
  s[p++] = ' ';
  for (size_t i = 0; i < inner; i++) s[p++] = '[';
  for (size_t i = 0; i < inner; i++) s[p++] = ']';
  s[p++] = ':';
  s[p++] = ' ';
  s[p++] = 'v';
  s[p++] = '\n';
  s[p] = '\0';
  return s;
}

/*
 * A key speculation runs a complete nested traversal that shares the nesting
 * budget of the enclosing parse instead of getting a budget of its own. The
 * parser accepts or refuses a document on the total depth that it reaches,
 * not on the depth of whichever traversal runs now, so a speculation `inner`
 * levels deep, reached `outer` levels into a document, is refused exactly
 * when outer + inner passes the cap.
 *
 * This is what pins the design of the speculation. The nested traversal is
 * independent in every other respect: it has its own frame stack, which it
 * drains or sweeps before it returns, and it rewinds ctx->pos when the
 * content turns out not to be a key. The shared budget is the one thing that
 * is deliberately not independent.
 *
 * This test is not vacuous: give the nested traversal a budget of its own,
 * and every refused case here parses instead. The form that the parser
 * accepts as a key is what tells the two apart, for the reason that the
 * comment of cyaml_test_nested_speculation_doc gives.
 */
TEST(deep_documents, a_key_speculation_shares_the_enclosing_nesting_budget) {
  static const struct {
    size_t outer, inner;
    bool accepted;
  } cases[] = {{400, 50, true},   {400, 150, false}, {300, 150, true},
               {300, 250, false}, {100, 300, true},  {450, 80, false}};
  bool outcome_ok = true;
  bool refusal_named_the_cap = true;
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
    char *doc =
        cyaml_test_nested_speculation_doc(cases[i].outer, cases[i].inner);
    if (!doc) {
      outcome_ok = false;
      break;
    }
    char *err = NULL;
    cyaml parsed = cyaml_parse(doc, &err);
    if ((parsed != NULL) != cases[i].accepted) outcome_ok = false;
    if (!cases[i].accepted &&
        (err == NULL ||
         strstr(err, "maximum nesting depth (500) exceeded") == NULL))
      refusal_named_the_cap = false;
    if (parsed) cyaml_destroy(parsed);
    free(doc);
  }
  REQUIRE_TRUE(outcome_ok);
  REQUIRE_TRUE(refusal_named_the_cap);
}

/* ========================================================================== */
/*                       GROWABLE BUFFER READ GUARDS                          */
/* ========================================================================== */

/* See the "convention this suite uses" note above for why this accessor is
   declared here rather than in a header. */
extern unsigned cyaml_debug_growbuf_guard_bits(void);

/* A parse and a serialization both build text in a growable buffer, which
 * latches one flag for out of memory instead of reporting on every single
 * append. Several places read back through the backing store of that buffer:
 * the three trims of trailing whitespace (one in a folded quoted scalar, one
 * in a plain scalar, and one on each continuation line of a plain scalar over
 * several lines), the flush of a deferred newline in the emitter of a block
 * scalar, and, last, the final
 * "does this already end with a newline" test of the serializer.
 * Each of those decides for itself whether the buffer holds anything that it
 * can read, so none of them depends on how a failed append leaves the
 * buffer.
 *
 * This test is not vacuous: it hands each helper a buffer with the flag
 * latched, no backing store, and a length that nothing rolled back. Drop any
 * one guard, and the matching call dereferences a null pointer and takes the
 * process down instead of returning a smaller bitmask. */
TEST(cyaml_growbuf_guards, every_backing_store_read_tests_the_buffer_itself) {
  REQUIRE_EQ(cyaml_debug_growbuf_guard_bits(), 7u);
}

/*
 * Every frame that a traversal opens releases the nesting level that it took,
 * both on the path that finishes and on the path that it abandons, so the
 * budget of a document goes on its deepest point, not on its total size.
 * Three sibling values, each 400 levels deep, therefore all parse, whereas
 * one level left behind by the first would put the second past the cap of
 * 500 levels.
 *
 * This test is not vacuous: drop the release, and the parser refuses the
 * second value with "maximum nesting depth (500) exceeded".
 */
TEST(deep_documents, a_finished_frame_releases_the_nesting_level_it_took) {
  const size_t depth = 400;
  const size_t siblings = 3;
  const size_t cap = siblings * (depth * 2 + 24) + 16;
  char *doc = (char *)malloc(cap);
  REQUIRE_NE((void *)doc, NULL);
  size_t p = 0;
  for (size_t k = 0; k < siblings; k++) {
    int written = snprintf(doc + p, cap - p, "k%zu: ", k);
    REQUIRE_GT(written, 0);
    p += (size_t)written;
    for (size_t i = 0; i < depth; i++) doc[p++] = '[';
    for (size_t i = 0; i < depth; i++) doc[p++] = ']';
    doc[p++] = '\n';
  }
  doc[p] = '\0';

  char *err = NULL;
  cyaml parsed = cyaml_parse(doc, &err);
  bool parsed_ok = parsed != NULL;
  bool every_sibling_is_present = parsed_ok;
  bool every_sibling_is_deep = parsed_ok;
  for (size_t k = 0; parsed_ok && k < siblings; k++) {
    char key[16];
    snprintf(key, sizeof(key), "k%zu", k);
    cyaml node = cyaml_dictionary_get(parsed, key);
    if (!node) {
      every_sibling_is_present = false;
      break;
    }
    size_t reached = 0;
    while (node && cyaml_type(node) == CYAML_LIST &&
           cyaml_list_len(node) == 1) {
      node = cyaml_list_get(node, 0);
      reached++;
    }
    if (reached != depth - 1) every_sibling_is_deep = false;
  }
  bool refusal_named_the_cap =
      parsed_ok ||
      (err != NULL &&
       strstr(err, "maximum nesting depth (500) exceeded") != NULL);

  if (parsed) cyaml_destroy(parsed);
  free(doc);

  REQUIRE_TRUE(refusal_named_the_cap);
  REQUIRE_TRUE(parsed_ok);
  REQUIRE_TRUE(every_sibling_is_present);
  REQUIRE_TRUE(every_sibling_is_deep);
}

/* ========================================================================== */
/*        RAW CONTROL BYTES IN AN ANCHOR NAME OR A TAG TOKEN                  */
/* ========================================================================== */

/* The parser refuses a raw control byte anywhere in the stream, and that
 * covers anchor names and tags for a reason beyond grammar: both are stored
 * as a plain char* that ends at a NUL byte, with no length field beside it. A
 * raw NUL inside the token would end the stored form early and report
 * nothing.
 *
 * Only cyaml_parse_n can carry such a byte into the parser. cyaml_parse stops
 * at the first NUL, so a NUL-terminated document can never reach these paths.
 * cyaml_parse_n is the documented entry point for a bounded buffer that came
 * from outside, which is exactly where untrusted bytes arrive. */

/* Two anchors that differ only AFTER an embedded NUL must not collapse into
 * one name. Without the rejection they both register as "a", the second
 * overwrites the first in the anchor map, and the alias resolves to the node
 * that the OTHER anchor named, while the parse reports success.
 *
 * This test is non-vacuous. Against a build that does not refuse the byte,
 * the parse SUCCEEDS and "third" comes out as 222 instead of 111. */
TEST(nul_in_tokens, anchor_name_with_a_raw_nul_is_rejected) {
  static const char doc[] =
      "first: &a\0X  111\n"
      "second: &a\0Y 222\n"
      "third: *a\0X\n";
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, sizeof(doc) - 1, &err);
  REQUIRE_EQ((void *)root, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "raw control character 0x00"), NULL);
  REQUIRE_NE((void *)strstr(err, "line 1, column 10"), NULL);
}

TEST(nul_in_tokens, alias_name_with_a_raw_nul_is_rejected) {
  static const char doc[] = "a: &anchor 1\nb: *anc\0hor\n";
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, sizeof(doc) - 1, &err);
  REQUIRE_EQ((void *)root, NULL);
  REQUIRE_NE((void *)err, NULL);
}

/* A verbatim tag whose whole content is one NUL byte would store the EMPTY
 * tag if the parser accepted the byte, and cyaml_serialize() would then write
 * "!<>", which cyaml_parse() refuses. cyaml_node_set_tag() refuses an empty
 * tag for exactly this invariant; the parse path holds it too. */
TEST(nul_in_tokens, verbatim_tag_of_one_raw_nul_is_rejected) {
  static const char doc[] = "!<\0> v";
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, sizeof(doc) - 1, &err);
  REQUIRE_EQ((void *)root, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "raw control character 0x00"), NULL);
  REQUIRE_NE((void *)strstr(err, "line 1, column 3"), NULL);
}

TEST(nul_in_tokens, verbatim_tag_with_an_interior_raw_nul_is_rejected) {
  static const char doc[] = "!<tag:a\0b> v";
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, sizeof(doc) - 1, &err);
  REQUIRE_EQ((void *)root, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(nul_in_tokens, shorthand_tag_suffix_with_a_raw_nul_is_rejected) {
  static const char doc[] = "!sh\0ort v";
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, sizeof(doc) - 1, &err);
  REQUIRE_EQ((void *)root, NULL);
  REQUIRE_NE((void *)err, NULL);
}

/* Other C0 bytes and DEL take the same path as the NUL. They cannot truncate
 * a stored string, but they are outside c-printable, so the stream check
 * refuses them in a token as everywhere else. */
TEST(nul_in_tokens, other_control_bytes_are_rejected_in_anchors_and_tags) {
  const char controls[] = {0x01, 0x08, 0x0b, 0x0c, 0x1f, 0x7f};
  for (size_t i = 0; i < sizeof(controls); i++) {
    char anchor_doc[] = "x: &aXb 1\n";
    anchor_doc[5] = controls[i]; /* the byte after '&a' */
    char *err = NULL;
    cyaml root = cyaml_parse_n(anchor_doc, sizeof(anchor_doc) - 1, &err);
    REQUIRE_EQ((void *)root, NULL);

    char verbatim_doc[] = "!<aXb> v";
    verbatim_doc[3] = controls[i];
    err = NULL;
    root = cyaml_parse_n(verbatim_doc, sizeof(verbatim_doc) - 1, &err);
    REQUIRE_EQ((void *)root, NULL);

    char suffix_doc[] = "!aXb v";
    suffix_doc[2] = controls[i];
    err = NULL;
    root = cyaml_parse_n(suffix_doc, sizeof(suffix_doc) - 1, &err);
    REQUIRE_EQ((void *)root, NULL);
  }
}

/* HTAB is the one byte below 0x20 that is not a control byte for this rule.
 * It ends an anchor name and ends a tag token, exactly as a space does, and
 * it must keep doing so rather than becoming a rejection. */
TEST(nul_in_tokens, a_tab_still_terminates_an_anchor_and_a_tag) {
  char *err = NULL;
  cyaml root = cyaml_parse("a: &anc\tvalue\n", &err);
  REQUIRE_NE((void *)root, NULL);
  cyaml_destroy(root);

  err = NULL;
  root = cyaml_parse("!!str\tvalue\n", &err);
  REQUIRE_NE((void *)root, NULL);
  cyaml_destroy(root);
}

/* The rejection must not cost any legitimate anchor, alias or tag. This
 * pins the accepting side, so a future tightening that goes too far fails
 * here instead of in a user's document. */
TEST(nul_in_tokens, ordinary_anchors_aliases_and_tags_still_parse) {
  char *err = NULL;
  cyaml root = cyaml_parse(
      "base: &base\n"
      "  a: 1\n"
      "derived:\n"
      "  <<: *base\n"
      "  b: 2\n"
      "verbatim: !<tag:example.com,2026:thing> content\n"
      "shorthand: !!str 42\n"
      "nonspecific: ! plain\n"
      "url_anchor: &a:b/c 7\n"
      "url_alias: *a:b/c\n",
      &err);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ((int)cyaml_type(root), (int)CYAML_DICTIONARY);
  cyaml verbatim = cyaml_dictionary_get(root, "verbatim");
  REQUIRE_NE((void *)verbatim, NULL);
  REQUIRE_STREQ(cyaml_node_tag(verbatim), "tag:example.com,2026:thing");
  cyaml_destroy(root);
}

/* The library must never emit a document that it cannot read back. This is
 * the property that the empty-tag guard of cyaml_node_set_tag states, applied
 * end to end across the parse and the serialize paths. */
TEST(nul_in_tokens, every_parsed_document_round_trips_through_both_emitters) {
  static const char *const docs[] = {
      "!<!> ~",
      "!<tag:x> v",
      "!!str hello",
      "a: &x 1\nb: *x\n",
      "- 1\n- [2, 3]\n- {k: v}\n",
      "? complex\n: value\n",
      "!<tag:example.com,2026:t> {a: !!int 1}\n",
  };
  for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
    char *err = NULL;
    cyaml root = cyaml_parse(docs[i], &err);
    REQUIRE_NE((void *)root, NULL);

    char *block = cyaml_serialize(root);
    REQUIRE_NE((void *)block, NULL);
    char *block_err = NULL;
    cyaml block_again = cyaml_parse(block, &block_err);
    bool block_ok = (block_again != NULL);
    if (block_again) cyaml_destroy(block_again);

    char *flow = cyaml_serialize_flow(root);
    REQUIRE_NE((void *)flow, NULL);
    char *flow_err = NULL;
    cyaml flow_again = cyaml_parse(flow, &flow_err);
    bool flow_ok = (flow_again != NULL);
    if (flow_again) cyaml_destroy(flow_again);

    /* Free everything BEFORE the assertions. A REQUIRE_* that fails returns
     * from this function at once and skips every line after it. */
    cyaml_serialize_free(block);
    cyaml_serialize_free(flow);
    cyaml_destroy(root);

    REQUIRE_TRUE(block_ok);
    REQUIRE_TRUE(flow_ok);
  }
}

/* ========================================================================== */
/*          cyaml_set: EVERY ACCEPTED C VALUE TYPE, INCLUDING ARRAYS          */
/* ========================================================================== */

/* cyaml_set() copies its argument into a local of the decayed, unqualified
 * type, so a named char array arrives as a pointer to its first character,
 * and a pointer that is itself const arrives as an ordinary pointer; neither
 * can be mistaken for the bytes of the string. */

static const char *const _cyaml_set_types_file_scope_name = "prod";

TEST(set_value_types, string_literal) {
  cyaml root = cyaml_create_dictionary();
  ccol_retval_t r = cyaml_set(root, "k", "champion");
  cyaml leaf = cyaml_get(root, "k");
  bool ok = leaf && cyaml_type(leaf) == CYAML_STRING &&
            strcmp(cyaml_str_val(leaf), "champion") == 0;
  cyaml_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, char_array_filled_by_snprintf) {
  cyaml root = cyaml_create_dictionary();
  char buf[32];
  snprintf(buf, sizeof(buf), "host-%d", 7);
  ccol_retval_t r = cyaml_set(root, "k", buf);
  /* The node holds its own copy, so a later write to buf changes nothing. */
  buf[0] = 'X';
  cyaml leaf = cyaml_get(root, "k");
  bool ok = leaf && cyaml_type(leaf) == CYAML_STRING &&
            strcmp(cyaml_str_val(leaf), "host-7") == 0;
  cyaml_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, const_char_array) {
  cyaml root = cyaml_create_dictionary();
  const char cbuf[] = "fixed";
  ccol_retval_t r = cyaml_set(root, "k", cbuf);
  cyaml leaf = cyaml_get(root, "k");
  bool ok = leaf && cyaml_type(leaf) == CYAML_STRING &&
            strcmp(cyaml_str_val(leaf), "fixed") == 0;
  cyaml_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, every_pointer_qualification) {
  cyaml root = cyaml_create_dictionary();
  char storage[] = "mutable";
  const char *p_const = "a";
  char *const p_top_const = storage;
  const char *const p_both = "b";
  ccol_retval_t r1 = cyaml_set(root, "c", p_const);
  ccol_retval_t r2 = cyaml_set(root, "t", p_top_const);
  ccol_retval_t r3 = cyaml_set(root, "b", p_both);
  ccol_retval_t r4 = cyaml_set(root, "f", _cyaml_set_types_file_scope_name);
  const char *keys[4] = {"c", "t", "b", "f"};
  const char *want[4] = {"a", "mutable", "b", "prod"};
  bool ok = true;
  for (int i = 0; i < 4; i++) {
    cyaml leaf = cyaml_get(root, keys[i]);
    ok = ok && leaf && cyaml_type(leaf) == CYAML_STRING &&
         strcmp(cyaml_str_val(leaf), want[i]) == 0;
  }
  cyaml_destroy(root);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_EQ(r4, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, bool_stays_bool) {
  cyaml root = cyaml_create_dictionary();
  const bool cb = true;
  ccol_retval_t r1 = cyaml_set(root, "a", (bool)false);
  ccol_retval_t r2 = cyaml_set(root, "b", cb);
  cyaml a = cyaml_get(root, "a"), b = cyaml_get(root, "b");
  bool ok = a && b && cyaml_type(a) == CYAML_BOOL &&
            cyaml_type(b) == CYAML_BOOL && !cyaml_bool_val(a) &&
            cyaml_bool_val(b);
  cyaml_destroy(root);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, narrow_integers_keep_their_sign) {
  cyaml root = cyaml_create_dictionary();
  const char c = 'A';
  signed char sc = -5;
  unsigned char uc = 250;
  short sh = -30000;
  unsigned short ush = 65535;
  ccol_retval_t r[5];
  r[0] = cyaml_set(root, "c", c);
  r[1] = cyaml_set(root, "sc", sc);
  r[2] = cyaml_set(root, "uc", uc);
  r[3] = cyaml_set(root, "sh", sh);
  r[4] = cyaml_set(root, "ush", ush);
  cyaml n[5] = {cyaml_get(root, "c"), cyaml_get(root, "sc"),
                cyaml_get(root, "uc"), cyaml_get(root, "sh"),
                cyaml_get(root, "ush")};
  bool all_int = true;
  for (int i = 0; i < 5; i++)
    all_int = all_int && n[i] && cyaml_type(n[i]) == CYAML_INTEGER;
  long long v[5] = {0};
  if (all_int)
    for (int i = 0; i < 5; i++) v[i] = cyaml_int_val(n[i]);
  cyaml_destroy(root);
  for (int i = 0; i < 5; i++) REQUIRE_EQ(r[i], ccol_success);
  REQUIRE_TRUE(all_int);
  REQUIRE_EQ(v[0], (long long)'A');
  REQUIRE_EQ(v[1], -5LL);
  REQUIRE_EQ(v[2], 250LL);
  REQUIRE_EQ(v[3], -30000LL);
  REQUIRE_EQ(v[4], 65535LL);
}

TEST(set_value_types, float_and_double) {
  cyaml root = cyaml_create_dictionary();
  const float f = 0.5f;
  volatile double d = 0.25;
  ccol_retval_t r1 = cyaml_set(root, "f", f);
  ccol_retval_t r2 = cyaml_set(root, "d", d);
  cyaml a = cyaml_get(root, "f"), b = cyaml_get(root, "d");
  bool ok = a && b && cyaml_type(a) == CYAML_FLOAT &&
            cyaml_type(b) == CYAML_FLOAT && cyaml_double_val(a) == 0.5 &&
            cyaml_double_val(b) == 0.25;
  cyaml_destroy(root);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, null_literal) {
  cyaml root = cyaml_create_dictionary();
  cyaml_set(root, "k", "x");
  ccol_retval_t r = cyaml_set(root, "k", NULL);
  cyaml leaf = cyaml_get(root, "k");
  bool ok = leaf && cyaml_type(leaf) == CYAML_NULL;
  cyaml_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, const_pointer_round_trips_through_serialize) {
  /* The string, and not the bytes of the pointer that names it, reaches the
   * serialized text. */
  char *err = NULL;
  cyaml doc = cyaml_parse("name: dev\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  ccol_retval_t r = cyaml_set(doc, "name", _cyaml_set_types_file_scope_name);
  char *out = cyaml_serialize(doc);
  bool ok = out && strcmp(out, "name: prod\n") == 0;
  if (out) cyaml_serialize_free(out);
  cyaml_destroy(doc);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

/* ========================================================================== */
/*        SHARED HELPERS: DEEP COMPARISON AND A SERIALIZE ROUND TRIP          */
/* ========================================================================== */

/* The tag that a round trip can preserve. The serializer omits a core-schema
 * tag that matches the type of its node, because implicit typing restores
 * the type with no help, so only a custom tag takes part in the comparison. */
static const char *_rt_custom_tag(cyaml n) {
  const char *t = cyaml_node_tag(n);
  if (t && strncmp(t, "tag:yaml.org,2002:", 18) == 0 &&
      (strcmp(t + 18, "null") == 0 || strcmp(t + 18, "bool") == 0 ||
       strcmp(t + 18, "int") == 0 || strcmp(t + 18, "float") == 0 ||
       strcmp(t + 18, "str") == 0 || strcmp(t + 18, "seq") == 0 ||
       strcmp(t + 18, "map") == 0))
    return NULL;
  return t;
}

static bool _rt_deep_equal(cyaml a, cyaml b) {
  if (!a || !b) return a == b;
  if (cyaml_type(a) != cyaml_type(b)) return false;
  const char *ta = _rt_custom_tag(a), *tb = _rt_custom_tag(b);
  if ((ta == NULL) != (tb == NULL)) return false;
  if (ta && strcmp(ta, tb) != 0) return false;
  switch (cyaml_type(a)) {
    case CYAML_NULL:
      return true;
    case CYAML_BOOL:
      return cyaml_bool_val(a) == cyaml_bool_val(b);
    case CYAML_INTEGER:
      return cyaml_int_val(a) == cyaml_int_val(b);
    case CYAML_FLOAT: {
      double x = cyaml_double_val(a), y = cyaml_double_val(b);
      return x == y || (x != x && y != y);
    }
    case CYAML_STRING:
      return strcmp(cyaml_str_val(a), cyaml_str_val(b)) == 0;
    case CYAML_LIST: {
      size_t n = cyaml_list_len(a);
      if (n != cyaml_list_len(b)) return false;
      for (size_t i = 0; i < n; i++)
        if (!_rt_deep_equal(cyaml_list_get(a, i), cyaml_list_get(b, i)))
          return false;
      return true;
    }
    case CYAML_DICTIONARY: {
      size_t n = cyaml_dictionary_size(a);
      if (n != cyaml_dictionary_size(b)) return false;
      cyaml_dictionary_iter it;
      for (bool more = cyaml_dictionary_first(a, &it); more;
           more = cyaml_dictionary_next(&it)) {
        cyaml other = cyaml_dictionary_get(b, it.key);
        if (!other) return false;
        if (!_rt_deep_equal(it.value, other)) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

/* Serialize doc in block style and in flow style, parse each result again,
 * and report whether both parses succeed and give a tree deep-equal to
 * doc. */
static bool _rt_round_trips(cyaml doc) {
  bool ok = true;
  for (int flow = 0; flow < 2 && ok; flow++) {
    char *text = flow ? cyaml_serialize_flow(doc) : cyaml_serialize(doc);
    if (!text) return false;
    char *err = NULL;
    cyaml back = cyaml_parse(text, &err);
    ok = back && _rt_deep_equal(doc, back);
    if (back) cyaml_destroy(back);
    cyaml_serialize_free(text);
  }
  return ok;
}

/* ========================================================================== */
/*               FLOAT DICTIONARY KEYS: SHORTEST ROUND-TRIP TEXT              */
/* ========================================================================== */

TEST(float_keys, canonical_text_is_the_shortest_round_trip) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "3.10: a\n3.9: b\n0.1: c\n1.2: d\n1e300: e\n-0.0: f\n.inf: g\n"
      ".nan: h\n-.inf: i\n2.5e-3: j\n",
      &err);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"3.1", "3.9",  "0.1",  "1.2",   "1e+300",
                                     "0",   ".inf", ".nan", "-.inf", "0.0025"};
  static const char *const vals[] = {"a", "b", "c", "d", "e",
                                     "f", "g", "h", "i", "j"};
  size_t found = 0;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    cyaml v = cyaml_dictionary_get(doc, keys[i]);
    if (v && cyaml_type(v) == CYAML_STRING &&
        strcmp(cyaml_str_val(v), vals[i]) == 0)
      found++;
  }
  size_t size = cyaml_dictionary_size(doc);
  cyaml_destroy(doc);
  REQUIRE_EQ(found, sizeof(keys) / sizeof(keys[0]));
  REQUIRE_EQ(size, sizeof(keys) / sizeof(keys[0]));
}

TEST(float_keys, negative_zero_and_integral_floats_share_the_integer_key) {
  /* -0.0 and 0.0 are one number, and 1.0 equals 1, so each pair is one
   * key. A duplicate key is not an error here: the later entry wins. */
  char *err = NULL;
  cyaml doc = cyaml_parse("-0.0: a\n0.0: b\n1.0: c\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  size_t size = cyaml_dictionary_size(doc);
  bool has_zero = cyaml_dictionary_get(doc, "0") != NULL;
  bool has_one = cyaml_dictionary_get(doc, "1") != NULL;
  cyaml_destroy(doc);
  REQUIRE_EQ(size, (size_t)2);
  REQUIRE_TRUE(has_zero);
  REQUIRE_TRUE(has_one);
}

TEST(float_keys, path_lookup_uses_the_canonical_text) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "python:\n  3.10: plain\n  \"3.10\": quoted\n  3.9: nine\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  cyaml plain = cyaml_get(doc, "python.3\\.1");
  cyaml quoted = cyaml_get(doc, "python.3\\.10");
  cyaml nine = cyaml_get(doc, "python.3\\.9");
  bool ok = plain && quoted && nine && cyaml_type(plain) == CYAML_STRING &&
            cyaml_type(quoted) == CYAML_STRING &&
            cyaml_type(nine) == CYAML_STRING &&
            strcmp(cyaml_str_val(plain), "plain") == 0 &&
            strcmp(cyaml_str_val(quoted), "quoted") == 0 &&
            strcmp(cyaml_str_val(nine), "nine") == 0;
  cyaml_destroy(doc);
  REQUIRE_TRUE(ok);
}

TEST(float_keys, serializer_writes_a_canonical_numeric_key_plain) {
  char *err = NULL;
  cyaml doc = cyaml_parse("3.10: a\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  char *block = cyaml_serialize(doc);
  char *flow = cyaml_serialize_flow(doc);
  bool block_ok = block && strcmp(block, "3.1: a\n") == 0;
  bool flow_ok = flow && strcmp(flow, "{3.1: a}") == 0;
  if (block) cyaml_serialize_free(block);
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(doc);
  REQUIRE_TRUE(block_ok);
  REQUIRE_TRUE(flow_ok);
}

TEST(float_keys, serialize_parse_round_trip_keeps_every_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "3.10: a\n3.9: b\n0.1: c\n1.2: d\n1e300: e\n-0.0: f\n.inf: g\n"
      ".nan: h\n-.inf: i\n10: j\n-5: k\n\"3.10\": l\n\"010\": m\n"
      "\"-0\": n\n\"+5\": o\n\"1e300\": p\n\".5\": q\nplain: r\n"
      "nested:\n  3.10: s\n",
      &err);
  REQUIRE_NE((void *)doc, NULL);
  bool ok = _rt_round_trips(doc);
  cyaml_destroy(doc);
  REQUIRE_TRUE(ok);
}

TEST(float_keys, non_canonical_numeric_text_stays_quoted) {
  /* A string key that looks like a number but is not its canonical text
   * must stay quoted, or a parse would store it under another key. */
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"3.10", "010",   "-0", "+5",
                                     "0x10", "1e300", ".5"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    cyaml_dictionary_set(doc, keys[i], cyaml_create_int((long long)i));
  char *out = cyaml_serialize_flow(doc);
  bool all_quoted = out != NULL;
  for (size_t i = 0; all_quoted && i < sizeof(keys) / sizeof(keys[0]); i++) {
    char quoted[16];
    snprintf(quoted, sizeof(quoted), "\"%s\":", keys[i]);
    all_quoted = strstr(out, quoted) != NULL;
  }
  if (out) cyaml_serialize_free(out);
  bool rt = _rt_round_trips(doc);
  cyaml_destroy(doc);
  REQUIRE_TRUE(all_quoted);
  REQUIRE_TRUE(rt);
}

/* ========================================================================== */
/*          CORE-SCHEMA TAGS STAY CONSISTENT WITH THE TYPE OF A NODE          */
/* ========================================================================== */

static const char *const _ct_core_tags[] = {
    CYAML_TAG_NULL, CYAML_TAG_BOOL, CYAML_TAG_INT, CYAML_TAG_FLOAT,
    CYAML_TAG_STR,  CYAML_TAG_SEQ,  CYAML_TAG_MAP};
static const cyaml_node_type_t _ct_core_tag_types[] = {
    CYAML_NULL,   CYAML_BOOL, CYAML_INTEGER,   CYAML_FLOAT,
    CYAML_STRING, CYAML_LIST, CYAML_DICTIONARY};
#define _CT_N 7

static cyaml _ct_make_node(cyaml_node_type_t type) {
  switch (type) {
    case CYAML_NULL:
      return cyaml_create_null();
    case CYAML_BOOL:
      return cyaml_create_bool(true);
    case CYAML_INTEGER:
      return cyaml_create_int(8080);
    case CYAML_FLOAT:
      return cyaml_create_double(1.5);
    case CYAML_STRING:
      return cyaml_create_string("8080");
    case CYAML_LIST: {
      cyaml l = cyaml_create_list();
      if (l) cyaml_list_push(l, cyaml_create_int(1));
      return l;
    }
    case CYAML_DICTIONARY: {
      cyaml d = cyaml_create_dictionary();
      if (d) cyaml_dictionary_set(d, "a", cyaml_create_int(1));
      return d;
    }
    default:
      return NULL;
  }
}

TEST(core_tag_consistency, set_tag_accepts_only_the_matching_core_tag) {
  size_t wrong = 0;
  for (size_t ti = 0; ti < _CT_N; ti++) {
    for (size_t ni = 0; ni < _CT_N; ni++) {
      cyaml doc = cyaml_create_dictionary();
      cyaml node = _ct_make_node(_ct_core_tag_types[ni]);
      if (!doc || !node) {
        wrong++;
        cyaml_destroy(doc);
        cyaml_destroy(node);
        continue;
      }
      cyaml_node_set_tag(node, "!keep");
      ccol_retval_t r = cyaml_node_set_tag(node, _ct_core_tags[ti]);
      bool should_match = ti == ni;
      const char *tag = cyaml_node_tag(node);
      bool tag_ok = should_match ? (r == ccol_success && tag &&
                                    strcmp(tag, _ct_core_tags[ti]) == 0)
                                 : (r == ccol_invalid_args && tag &&
                                    strcmp(tag, "!keep") == 0);
      bool type_ok = cyaml_type(node) == _ct_core_tag_types[ni];
      cyaml_dictionary_set(doc, "v", node);
      bool rt = _rt_round_trips(doc);
      if (!tag_ok || !type_ok || !rt) {
        fprintf(stderr, "tag %s on type %d: tag_ok=%d type_ok=%d rt=%d\n",
                _ct_core_tags[ti], (int)_ct_core_tag_types[ni], tag_ok, type_ok,
                rt);
        wrong++;
      }
      cyaml_destroy(doc);
    }
  }
  REQUIRE_EQ(wrong, (size_t)0);
}

TEST(core_tag_consistency, custom_tags_are_accepted_on_every_type) {
  size_t wrong = 0;
  for (size_t ni = 0; ni < _CT_N; ni++) {
    cyaml doc = cyaml_create_dictionary();
    cyaml node = _ct_make_node(_ct_core_tag_types[ni]);
    if (!doc || !node) {
      wrong++;
      cyaml_destroy(doc);
      cyaml_destroy(node);
      continue;
    }
    ccol_retval_t r1 = cyaml_node_set_tag(node, "!custom");
    ccol_retval_t r2 = cyaml_node_set_tag(node, "tag:yaml.org,2002:binary");
    ccol_retval_t r3 = cyaml_node_set_tag(node, "tag:yaml.org,2002:");
    cyaml_dictionary_set(doc, "v", node);
    bool rt = _rt_round_trips(doc);
    if (r1 != ccol_success || r2 != ccol_success || r3 != ccol_success || !rt)
      wrong++;
    cyaml_destroy(doc);
  }
  REQUIRE_EQ(wrong, (size_t)0);
}

/* The source text of one node of each type, with each core tag that names
 * its type and with a custom tag. */
static const char *const _ct_tagged_sources[] = {
    "v: !!null ~\n",     "v: !!bool true\n",   "v: !!int 8080\n",
    "v: !!float 1.5\n",  "v: !!str x\n",       "v: !!seq [1]\n",
    "v: !!map {a: 1}\n", "v: !custom 8080\n",  "v: !custom x\n",
    "v: !custom [1]\n",  "v: !custom {a: 1}\n"};

/* Apply one cyaml_set() of kind k to the leaf "v". */
static ccol_retval_t _ct_apply_set(cyaml doc, int k) {
  switch (k) {
    case 0:
      return cyaml_set(doc, "v", (bool)false);
    case 1:
      return cyaml_set(doc, "v", 9090);
    case 2:
      return cyaml_set(doc, "v", 2.5);
    case 3:
      return cyaml_set(doc, "v", "auto");
    case 4:
      return cyaml_set(doc, "v", NULL);
    default:
      return ccol_invalid_args;
  }
}
static const cyaml_node_type_t _ct_set_types[] = {
    CYAML_BOOL, CYAML_INTEGER, CYAML_FLOAT, CYAML_STRING, CYAML_NULL};

TEST(core_tag_consistency, set_drops_a_core_tag_that_stops_matching) {
  size_t wrong = 0;
  size_t n_src = sizeof(_ct_tagged_sources) / sizeof(_ct_tagged_sources[0]);
  for (size_t si = 0; si < n_src; si++) {
    for (int k = 0; k < 5; k++) {
      char *err = NULL;
      cyaml doc = cyaml_parse(_ct_tagged_sources[si], &err);
      if (!doc) {
        fprintf(stderr, "source %s did not parse: %s\n", _ct_tagged_sources[si],
                err ? err : "?");
        wrong++;
        continue;
      }
      /* Copy the tag, because the set below can free the string that the node
       * holds. */
      char before[64] = "";
      const char *tag0 = cyaml_node_tag(cyaml_get(doc, "v"));
      if (tag0) snprintf(before, sizeof(before), "%s", tag0);
      bool was_custom = before[0] == '!';
      ccol_retval_t r = _ct_apply_set(doc, k);
      cyaml v = cyaml_get(doc, "v");
      const char *after = v ? cyaml_node_tag(v) : NULL;
      bool ok = r == ccol_success && v && cyaml_type(v) == _ct_set_types[k];
      if (ok && was_custom) {
        /* A custom tag survives every change of value. */
        ok = after && strcmp(after, "!custom") == 0;
      } else if (ok && after) {
        /* A core tag that survives must name the new type. */
        size_t ti = 0;
        while (ti < _CT_N && strcmp(after, _ct_core_tags[ti]) != 0) ti++;
        ok = ti < _CT_N && _ct_core_tag_types[ti] == cyaml_type(v);
      } else if (ok) {
        /* No tag survives only where the core tag stopped matching. */
        size_t ti = 0;
        while (ti < _CT_N && strcmp(before, _ct_core_tags[ti]) != 0) ti++;
        ok = ti < _CT_N && _ct_core_tag_types[ti] != cyaml_type(v);
      }
      bool rt = _rt_round_trips(doc);
      if (!ok || !rt) {
        fprintf(stderr, "source \"%s\" set kind %d: ok=%d rt=%d\n",
                _ct_tagged_sources[si], k, ok, rt);
        wrong++;
      }
      cyaml_destroy(doc);
    }
  }
  REQUIRE_EQ(wrong, (size_t)0);
}

TEST(core_tag_consistency, set_keeps_a_core_tag_that_still_matches) {
  char *err = NULL;
  cyaml doc = cyaml_parse("port: !!int 8080\nname: !!str x\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  ccol_retval_t r1 = cyaml_set(doc, "port", 9090);
  ccol_retval_t r2 = cyaml_set(doc, "name", "y");
  const char *t1 = cyaml_node_tag(cyaml_get(doc, "port"));
  const char *t2 = cyaml_node_tag(cyaml_get(doc, "name"));
  bool ok = t1 && t2 && strcmp(t1, CYAML_TAG_INT) == 0 &&
            strcmp(t2, CYAML_TAG_STR) == 0;
  cyaml_destroy(doc);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(core_tag_consistency, int_node_set_to_string_serializes_without_its_tag) {
  char *err = NULL;
  cyaml doc = cyaml_parse("port: !!int 8080\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  ccol_retval_t r = cyaml_set(doc, "port", "auto");
  char *out = cyaml_serialize(doc);
  bool ok = out && strcmp(out, "port: auto\n") == 0;
  if (out) cyaml_serialize_free(out);
  cyaml_destroy(doc);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

/* ========================================================================== */
/*    FLOW COLLECTION CLOSED BY AN INDICATOR AT THE INDENT OF ITS PARENT      */
/* ========================================================================== */

/* Parse src and deep-compare the tree with the parse of want, which spells
 * the same content on one line. A NULL want means that the parse of src
 * must fail. */
static bool _fc_check(const char *src, const char *want) {
  char *err = NULL;
  cyaml doc = cyaml_parse(src, &err);
  if (!want) {
    if (doc) cyaml_destroy(doc);
    return doc == NULL;
  }
  if (!doc) {
    fprintf(stderr, "unexpected parse failure for %s: %s\n", src,
            err ? err : "?");
    return false;
  }
  cyaml ref = cyaml_parse(want, &err);
  bool ok = ref && _rt_deep_equal(doc, ref);
  if (!ok) fprintf(stderr, "%s does not match %s\n", src, want);
  if (ref) cyaml_destroy(ref);
  cyaml_destroy(doc);
  return ok;
}

TEST(flow_closing_indent, block_mapping_value) {
  REQUIRE_TRUE(_fc_check("key: [\n  a,\n  b\n]\n", "{key: [a, b]}"));
  REQUIRE_TRUE(_fc_check("key: {\n  a: 1\n}\n", "{key: {a: 1}}"));
  REQUIRE_TRUE(
      _fc_check("outer:\n  key: [\n    a\n  ]\n", "{outer: {key: [a]}}"));
}

TEST(flow_closing_indent, block_sequence_item) {
  REQUIRE_TRUE(_fc_check("- [\n  a\n]\n", "[[a]]"));
  REQUIRE_TRUE(_fc_check("- {\n  a: 1\n}\n", "[{a: 1}]"));
  REQUIRE_TRUE(_fc_check("- - [\n    a\n  ]\n", "[[[a]]]"));
}

TEST(flow_closing_indent, nested_flows_and_trailing_comment) {
  REQUIRE_TRUE(_fc_check("key: [[\n  1\n]]\n", "{key: [[1]]}"));
  REQUIRE_TRUE(_fc_check("key: [ [\n  a\n], b ]\n", "{key: [[a], b]}"));
  REQUIRE_TRUE(_fc_check("key: [\n  a\n] # done\n", "{key: [a]}"));
  REQUIRE_TRUE(_fc_check("key: {\n  a: [\n    1\n  ],\n  b: 2\n}\nz: 3\n",
                         "{key: {a: [1], b: 2}, z: 3}"));
}

TEST(flow_closing_indent, other_content_at_that_column_is_still_refused) {
  REQUIRE_TRUE(_fc_check("key: [\na\n]\n", NULL));
  REQUIRE_TRUE(_fc_check("key: [\n  a,\nb]\n", NULL));
  REQUIRE_TRUE(_fc_check("key: [\n  a\n,b]\n", NULL));
  REQUIRE_TRUE(_fc_check("key: {\na: 1\n}\n", NULL));
  REQUIRE_TRUE(_fc_check("key: [\n  a\n]x\n", NULL));
  REQUIRE_TRUE(_fc_check("key: [\n  a\n]: v\n", NULL));
}

TEST(flow_closing_indent, closing_left_of_the_parent_is_still_refused) {
  REQUIRE_TRUE(_fc_check("outer:\n  key: [\n    a\n]\n", NULL));
  REQUIRE_TRUE(_fc_check("- - [\n    a\n]\n", NULL));
}

/* ========================================================================== */
/*          DUPLICATE KEYS: THE LAST VALUE WINS AND NOTHING LEAKS             */
/* ========================================================================== */

/* An allocator that counts the blocks that are live, so that a test can
 * assert that every block it caused is freed again. */
static long _dup_live = 0;
static void *_dup_malloc(size_t sz) {
  void *p = malloc(sz);
  if (p) _dup_live++;
  return p;
}
static void *_dup_calloc(size_t n, size_t sz) {
  void *p = calloc(n, sz);
  if (p) _dup_live++;
  return p;
}
static void *_dup_realloc(void *old, size_t sz) {
  void *p = realloc(old, sz);
  if (p && !old) _dup_live++;
  return p;
}
static void _dup_free(void *p) {
  if (p) _dup_live--;
  free(p);
}
static ccol_memmgmt_procs_t _dup_mp = {.malloc = _dup_malloc,
                                       .calloc = _dup_calloc,
                                       .realloc = _dup_realloc,
                                       .free = _dup_free};

TEST(duplicate_keys, parse_keeps_the_last_value_and_frees_the_others) {
  static const char *const docs[] = {
      ("a: {x: [1, 2, {y: deep}]}\nb: 1\na: [s, {z: 2}]\na: last\n"
       "b: {k: true}\n"),
      "{a: {x: [1]}, b: 1, a: [s], a: last, b: {k: true}}\n",
      "a:\n  x: [1, 2]\nb: 1\na:\n  - s\na: last\nb:\n  k: true\n"};
  for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
    _dup_live = 0;
    char *err = NULL;
    cyaml root = cyaml_parse_mp(docs[i], &err, &_dup_mp);
    bool parsed = root != NULL;
    size_t size = parsed ? cyaml_dictionary_size(root) : 0;
    cyaml a = parsed ? cyaml_get(root, "a") : NULL;
    cyaml bk = parsed ? cyaml_get(root, "b.k") : NULL;
    bool a_ok = a && cyaml_type(a) == CYAML_STRING &&
                strcmp(cyaml_str_val(a), "last") == 0;
    bool b_ok = bk && cyaml_type(bk) == CYAML_BOOL && cyaml_bool_val(bk);
    if (root) cyaml_destroy(root);
    long live = _dup_live;
    REQUIRE_TRUE(parsed);
    REQUIRE_EQ(size, (size_t)2);
    REQUIRE_TRUE(a_ok);
    REQUIRE_TRUE(b_ok);
    REQUIRE_EQ(live, 0L);
  }
}

TEST(duplicate_keys, redefined_anchor_resolves_to_the_later_node) {
  _dup_live = 0;
  char *err = NULL;
  cyaml root = cyaml_parse_mp("a: &x {p: 1}\nb: &x [2, {q: 3}]\nc: *x\n", &err,
                              &_dup_mp);
  bool parsed = root != NULL;
  cyaml c = parsed ? cyaml_get(root, "c") : NULL;
  bool ok = c && cyaml_type(c) == CYAML_LIST && cyaml_list_len(c) == 2;
  if (root) cyaml_destroy(root);
  long live = _dup_live;
  REQUIRE_TRUE(parsed);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(live, 0L);
}

TEST(duplicate_keys, dictionary_set_replaces_and_frees_the_old_subtree) {
  _dup_live = 0;
  cyaml root = cyaml_create_dictionary_mp(&_dup_mp);
  REQUIRE_NE((void *)root, NULL);
  cyaml first = cyaml_create_list_mp(&_dup_mp);
  cyaml_list_push(first, cyaml_create_string_mp("old", &_dup_mp));
  ccol_retval_t r1 = cyaml_dictionary_set(root, "k", first);
  ccol_retval_t r2 =
      cyaml_dictionary_set(root, "k", cyaml_create_int_mp(7, &_dup_mp));
  /* Setting a key to the node that it already holds changes nothing. */
  ccol_retval_t r3 = cyaml_dictionary_set(root, "k", cyaml_get(root, "k"));
  cyaml k = cyaml_get(root, "k");
  bool ok = k && cyaml_type(k) == CYAML_INTEGER && cyaml_int_val(k) == 7 &&
            cyaml_dictionary_size(root) == 1;
  cyaml_destroy(root);
  long live = _dup_live;
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(live, 0L);
}

TEST(duplicate_keys, attached_child_of_another_key_is_refused_untouched) {
  cyaml root = cyaml_create_dictionary();
  REQUIRE_NE((void *)root, NULL);
  cyaml_dictionary_set(root, "a", cyaml_create_int(1));
  cyaml_dictionary_set(root, "b", cyaml_create_int(2));
  ccol_retval_t r = cyaml_dictionary_set(root, "b", cyaml_get(root, "a"));
  cyaml a = cyaml_get(root, "a"), b = cyaml_get(root, "b");
  bool ok = a && b && cyaml_int_val(a) == 1 && cyaml_int_val(b) == 2;
  cyaml_destroy(root);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_TRUE(ok);
}

TEST(duplicate_keys, merge_keeps_the_explicit_value_and_frees_nothing_live) {
  _dup_live = 0;
  char *err = NULL;
  cyaml root = cyaml_parse_mp(
      "base: &b {x: 1, y: [1, 2]}\nm:\n  <<: *b\n  x: 9\n", &err, &_dup_mp);
  bool parsed = root != NULL;
  cyaml x = parsed ? cyaml_get(root, "m.x") : NULL;
  cyaml y = parsed ? cyaml_get(root, "m.y") : NULL;
  bool ok = x && y && cyaml_type(x) == CYAML_INTEGER && cyaml_int_val(x) == 9 &&
            cyaml_type(y) == CYAML_LIST && cyaml_list_len(y) == 2;
  if (root) cyaml_destroy(root);
  long live = _dup_live;
  REQUIRE_TRUE(parsed);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(live, 0L);
}

/* ========================================================================== */
/*     SET OF A NEW KEY: A FAILED BUILD LEAVES NO PLACEHOLDER BEHIND          */
/* ========================================================================== */

/* An allocator that refuses every allocation once _ff_left reaches zero, and
 * that counts the blocks that are live. A _ff_left of -1 never refuses. */
static long _ff_left = -1;
static long _ff_live = 0;
static void *_ff_malloc(size_t sz) {
  if (_ff_left == 0) return NULL;
  if (_ff_left > 0) _ff_left--;
  void *p = malloc(sz);
  if (p) _ff_live++;
  return p;
}
static void *_ff_calloc(size_t n, size_t sz) {
  if (_ff_left == 0) return NULL;
  if (_ff_left > 0) _ff_left--;
  void *p = calloc(n, sz);
  if (p) _ff_live++;
  return p;
}
static void *_ff_realloc(void *old, size_t sz) {
  if (_ff_left == 0) return NULL;
  if (_ff_left > 0) _ff_left--;
  void *p = realloc(old, sz);
  if (p && !old) _ff_live++;
  return p;
}
static void _ff_free(void *p) {
  if (p) _ff_live--;
  free(p);
}
static ccol_memmgmt_procs_t _ff_mp = {.malloc = _ff_malloc,
                                      .calloc = _ff_calloc,
                                      .realloc = _ff_realloc,
                                      .free = _ff_free};

/* Every allocation of a set that creates a new string leaf is refused once,
 * in turn. The last one is the copy of the string inside the build of the
 * new node, which runs after the key is already in the map, so the budget
 * just below the one that succeeds exercises the removal of that key. The
 * size check catches a key that stays behind with a NULL child, which a
 * lookup alone reports as absent. Each refusal
 * must report ccol_not_enough_memory, leave the key absent, and leak
 * nothing. */
TEST(set_new_key, a_failed_build_removes_the_key_and_leaks_nothing) {
  _ff_left = -1;
  _ff_live = 0;
  cyaml root = cyaml_create_dictionary_mp(&_ff_mp);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval_t pre = cyaml_set(root, "keep", 1);
  long live_before = _ff_live;

  size_t failures = 0, wrong = 0;
  ccol_retval_t r = ccol_not_enough_memory;
  for (long budget = 0; budget < 64 && r != ccol_success; budget++) {
    _ff_left = budget;
    r = cyaml_set(root, "fresh", "a string value");
    _ff_left = -1;
    if (r == ccol_success) break;
    failures++;
    bool key_absent = cyaml_get(root, "fresh") == NULL;
    bool size_ok = cyaml_dictionary_size(root) == 1;
    bool no_leak = _ff_live == live_before;
    if (r != ccol_not_enough_memory || !key_absent || !size_ok || !no_leak)
      wrong++;
  }
  cyaml fresh = cyaml_get(root, "fresh");
  bool stored = fresh && cyaml_type(fresh) == CYAML_STRING &&
                strcmp(cyaml_str_val(fresh), "a string value") == 0;
  cyaml_destroy(root);
  long live_after = _ff_live;
  REQUIRE_EQ(pre, ccol_success);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_GT(failures, (size_t)1);
  REQUIRE_EQ(wrong, (size_t)0);
  REQUIRE_TRUE(stored);
  REQUIRE_EQ(live_after, 0L);
}

/* ========================================================================== */
/*                    NUMBERS UNDER A COMMA-DECIMAL LOCALE                    */
/* ========================================================================== */

/*
 * The core schema writes the decimal point of a float as '.', whatever the
 * locale is. These tests move the test thread to a locale whose LC_NUMERIC
 * writes ',' (uselocale() changes this thread only) and check that parsing
 * and serializing still use '.'.
 *
 * Such a locale is often not installed. The Makefile builds a private copy of
 * de_DE.UTF-8 into tests_locale/ with localedef when it can, and
 * _comma_locale_open() falls back to it through LOCPATH. When neither exists,
 * each test prints why and returns without asserting anything.
 */
static locale_t _comma_locale_try(const char *name) {
  locale_t loc = newlocale(LC_NUMERIC_MASK, name, (locale_t)0);
  if (!loc) return (locale_t)0;
  const char *radix = nl_langinfo_l(RADIXCHAR, loc);
  if (radix && strcmp(radix, ",") == 0) return loc;
  freelocale(loc);
  return (locale_t)0;
}

static locale_t _comma_locale_open(void) {
  static const char *const names[] = {
      "de_DE.UTF-8", "de_DE.utf8",  "de_DE",       "fr_FR.UTF-8",
      "fr_FR.utf8",  "nl_NL.UTF-8", "es_ES.UTF-8", "it_IT.UTF-8",
      "ru_RU.UTF-8", "pt_BR.UTF-8"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    locale_t loc = _comma_locale_try(names[i]);
    if (loc) return loc;
  }
  /* The private copy that the Makefile builds, found through LOCPATH, which
   * is set only around this one attempt. glibc's newlocale() never frees
   * the search path that it builds from LOCPATH, which memtest reports as a
   * definite leak, while setlocale() frees its own. The locale is therefore
   * loaded into the global LC_NUMERIC for one moment, copied out with
   * duplocale(), and the global setting is put back at once. No other
   * thread of this binary runs while a test body runs. */
  locale_t loc = (locale_t)0;
  const char *old_path = getenv("LOCPATH");
  char *saved_path = old_path ? strdup(old_path) : NULL;
  const char *old_numeric = setlocale(LC_NUMERIC, NULL);
  char *saved_numeric = old_numeric ? strdup(old_numeric) : NULL;
  setenv("LOCPATH", "tests_locale", 1);
  if (saved_numeric && setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
    locale_t copy = duplocale(LC_GLOBAL_LOCALE);
    setlocale(LC_NUMERIC, saved_numeric);
    if (copy) {
      const char *radix = nl_langinfo_l(RADIXCHAR, copy);
      if (radix && strcmp(radix, ",") == 0)
        loc = copy;
      else
        freelocale(copy);
    }
  }
  free(saved_numeric);
  if (saved_path) {
    setenv("LOCPATH", saved_path, 1);
    free(saved_path);
  } else {
    unsetenv("LOCPATH");
  }
  if (!loc)
    printf(
        "    [skipped: no locale with ',' as its decimal point is "
        "installed, and tests_locale/de_DE.UTF-8 is missing]\n");
  return loc;
}

TEST(number_locale, parse_reads_dot_decimal_floats_under_a_comma_locale) {
  locale_t comma = _comma_locale_open();
  if (!comma) return;
  locale_t prev = uselocale(comma);
  /* The locale really is in effect for this thread. */
  char probe[16];
  snprintf(probe, sizeof(probe), "%.2f", 1.5);

  char *err = NULL;
  cyaml d = cyaml_parse(
      "timeout: 1.5\nlist: [2.25, -5.0e2, .5]\n!!float 7: tagged\n"
      "2.5: float key\n",
      &err);
  cyaml timeout = d ? cyaml_get(d, "timeout") : NULL;
  cyaml list = d ? cyaml_get(d, "list") : NULL;
  bool types_ok = timeout && cyaml_type(timeout) == CYAML_FLOAT && list &&
                  cyaml_list_len(list) == 3 &&
                  cyaml_type(cyaml_list_get(list, 0)) == CYAML_FLOAT &&
                  cyaml_type(cyaml_list_get(list, 1)) == CYAML_FLOAT &&
                  cyaml_type(cyaml_list_get(list, 2)) == CYAML_FLOAT;
  double t = types_ok ? cyaml_double_val(timeout) : 0.0;
  double l0 = types_ok ? cyaml_double_val(cyaml_list_get(list, 0)) : 0.0;
  double l1 = types_ok ? cyaml_double_val(cyaml_list_get(list, 1)) : 0.0;
  double l2 = types_ok ? cyaml_double_val(cyaml_list_get(list, 2)) : 0.0;
  /* A float key is stored as its canonical text, "2.5", and the explicitly
   * tagged 7 as "7". */
  bool float_key = d && cyaml_dictionary_get(d, "2.5") != NULL;
  bool tagged_key = d && cyaml_dictionary_get(d, "7") != NULL;
  /* The parse gives the thread back the locale that it found. */
  bool restored = uselocale((locale_t)0) == comma;
  bool parsed = d != NULL;
  cyaml_destroy(d);

  uselocale(prev);
  freelocale(comma);
  REQUIRE_STREQ(probe, "1,50");
  REQUIRE_TRUE(parsed);
  REQUIRE_TRUE(types_ok);
  REQUIRE_EQ(t, 1.5);
  REQUIRE_EQ(l0, 2.25);
  REQUIRE_EQ(l1, -500.0);
  REQUIRE_EQ(l2, 0.5);
  REQUIRE_TRUE(float_key);
  REQUIRE_TRUE(tagged_key);
  REQUIRE_TRUE(restored);
}

TEST(number_locale, serialize_writes_dot_decimal_floats_under_a_comma_locale) {
  locale_t comma = _comma_locale_open();
  if (!comma) return;
  locale_t prev = uselocale(comma);

  cyaml d = cyaml_create_dictionary();
  cyaml_dictionary_set(d, "ratio", cyaml_create_double(0.25));
  cyaml_dictionary_set(d, "big", cyaml_create_double(1e300));
  cyaml_dictionary_set(d, "text", cyaml_create_string("1.5"));
  char *block = cyaml_serialize(d);
  char *flow = cyaml_serialize_flow(d);
  bool block_ok = block && strstr(block, "ratio: 0.25\n") &&
                  strstr(block, "big: 1.0e+300\n") &&
                  strstr(block, "text: \"1.5\"\n") && !strchr(block, ',');
  bool flow_ok =
      flow && strstr(flow, "ratio: 0.25") && strstr(flow, "text: \"1.5\"");
  char *err = NULL;
  cyaml back = block ? cyaml_parse(block, &err) : NULL;
  cyaml ratio = back ? cyaml_get(back, "ratio") : NULL;
  bool round_trip = ratio && cyaml_type(ratio) == CYAML_FLOAT &&
                    cyaml_double_val(ratio) == 0.25;
  cyaml text = back ? cyaml_get(back, "text") : NULL;
  bool text_ok = text && cyaml_type(text) == CYAML_STRING;
  bool restored = uselocale((locale_t)0) == comma;
  cyaml_destroy(back);
  if (block) cyaml_serialize_free(block);
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(d);

  uselocale(prev);
  freelocale(comma);
  REQUIRE_TRUE(block_ok);
  REQUIRE_TRUE(flow_ok);
  REQUIRE_TRUE(round_trip);
  REQUIRE_TRUE(text_ok);
  REQUIRE_TRUE(restored);
}

/* ========================================================================== */
/*                    TAGS ON IMPLICIT BLOCK MAPPING KEYS                     */
/* ========================================================================== */

/* The tag of an implicit block key types the key, exactly as it types the
 * same key in a flow mapping or after "? ". The canonical text of the typed
 * key is what the dictionary stores. */
TEST(block_mapping, tag_on_implicit_key_types_the_key) {
  char *err = NULL;
  cyaml two = cyaml_parse("perm:\n  !!str 010: a\n  !!str 10: b\n", &err);
  cyaml perm = two ? cyaml_get(two, "perm") : NULL;
  size_t perm_size = perm ? cyaml_dictionary_size(perm) : 0;
  cyaml k010 = perm ? cyaml_dictionary_get(perm, "010") : NULL;
  cyaml k10 = perm ? cyaml_dictionary_get(perm, "10") : NULL;
  bool k010_ok = k010 && strcmp(cyaml_str_val(k010), "a") == 0;
  bool k10_ok = k10 && strcmp(cyaml_str_val(k10), "b") == 0;
  cyaml_destroy(two);

  /* The same keys in flow context give the same result. */
  cyaml flow = cyaml_parse("{!!str 010: a, !!str 10: b}", &err);
  size_t flow_size = flow ? cyaml_dictionary_size(flow) : 0;
  cyaml_destroy(flow);

  cyaml hex = cyaml_parse("!!str 0x10: v\n", &err);
  bool hex_ok = hex && cyaml_dictionary_get(hex, "0x10") != NULL;
  cyaml_destroy(hex);
  cyaml plain_hex = cyaml_parse("0x10: v\n", &err);
  bool plain_hex_ok = plain_hex && cyaml_dictionary_get(plain_hex, "16");
  cyaml_destroy(plain_hex);
  cyaml quoted_int = cyaml_parse("!!int \"0x10\": v\n", &err);
  bool quoted_int_ok =
      quoted_int && cyaml_dictionary_get(quoted_int, "16") != NULL;
  cyaml_destroy(quoted_int);
  cyaml single_quoted = cyaml_parse("!!float '2': v\n", &err);
  bool single_quoted_ok =
      single_quoted && cyaml_dictionary_get(single_quoted, "2") != NULL;
  cyaml_destroy(single_quoted);
  cyaml yes = cyaml_parse("!!bool yes: v\n", &err);
  bool yes_ok = yes && cyaml_dictionary_get(yes, "true") != NULL;
  cyaml_destroy(yes);
  /* A custom tag and the non-specific "!" leave the key as implicit typing
   * gives it. */
  cyaml custom = cyaml_parse("!x 0x10: v\n! 0x11: w\n", &err);
  bool custom_ok = custom && cyaml_dictionary_get(custom, "16") != NULL &&
                   cyaml_dictionary_get(custom, "17") != NULL;
  cyaml_destroy(custom);
  /* A flow collection key checks its tag too. */
  cyaml map_key = cyaml_parse("!!map {a: 1}: v\n", &err);
  bool map_key_ok = map_key && cyaml_dictionary_size(map_key) == 1;
  cyaml_destroy(map_key);

  REQUIRE_EQ(perm_size, (size_t)2);
  REQUIRE_TRUE(k010_ok);
  REQUIRE_TRUE(k10_ok);
  REQUIRE_EQ(flow_size, (size_t)2);
  REQUIRE_TRUE(hex_ok);
  REQUIRE_TRUE(plain_hex_ok);
  REQUIRE_TRUE(quoted_int_ok);
  REQUIRE_TRUE(single_quoted_ok);
  REQUIRE_TRUE(yes_ok);
  REQUIRE_TRUE(custom_ok);
  REQUIRE_TRUE(map_key_ok);
}

/* An anchor on a tagged key names the node as the tag types it. */
TEST(block_mapping, anchor_on_tagged_quoted_key_resolves_to_the_tagged_node) {
  char *err = NULL;
  cyaml d =
      cyaml_parse("&k !!int \"42\": v\nx: *k\n&s !!str 7: w\ny: *s\n", &err);
  cyaml x = d ? cyaml_get(d, "x") : NULL;
  bool x_ok = x && cyaml_type(x) == CYAML_INTEGER && cyaml_int_val(x) == 42;
  cyaml y = d ? cyaml_get(d, "y") : NULL;
  bool y_ok =
      y && cyaml_type(y) == CYAML_STRING && strcmp(cyaml_str_val(y), "7") == 0;
  bool keys_ok = d && cyaml_dictionary_get(d, "42") != NULL &&
                 cyaml_dictionary_get(d, "7") != NULL;
  cyaml_destroy(d);
  REQUIRE_TRUE(x_ok);
  REQUIRE_TRUE(y_ok);
  REQUIRE_TRUE(keys_ok);
}

/* A tag that disagrees with an implicit block key is a parse error, as it is
 * on a value and on a key in flow context. */
TEST(errors, tag_that_disagrees_with_an_implicit_key_rejected) {
  static const char *const docs[] = {
      "!!int abc: v\n",     "{!!int abc: v}\n",    "a: 1\n!!float x: v\n",
      "!!int \"abc\": v\n", "!!seq k: v\n",        "!!str [a]: v\n",
      "!!seq {a: 1}: v\n",  "- !!bool maybe: v\n", "&a !!int 1.5: v\n"};
  size_t accepted = 0;
  for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
    char *err = NULL;
    cyaml d = cyaml_parse(docs[i], &err);
    if (d || !err) {
      printf("    accepted: %s", docs[i]);
      accepted++;
    }
    cyaml_destroy(d);
  }
  REQUIRE_EQ(accepted, (size_t)0);
}

/* ========================================================================== */
/*          SPECULATIVE KEY PARSE: A VALUE IS NEVER TREATED AS A KEY          */
/* ========================================================================== */

/* Builds "<prefix>[item00000, item00001, ...]\n" with n items. */
static char *_big_flow_doc(const char *prefix, const char *suffix, int n) {
  size_t cap = (size_t)n * 12 + strlen(prefix) + strlen(suffix) + 8;
  char *s = malloc(cap);
  if (!s) return NULL;
  size_t len = (size_t)snprintf(s, cap, "%s[", prefix);
  for (int i = 0; i < n; i++)
    len += (size_t)snprintf(s + len, cap - len, "%sitem%05d", i ? ", " : "", i);
  snprintf(s + len, cap - len, "]\n%s", suffix);
  return s;
}

/* A one-line flow collection in a value position may be longer than the
 * limit on the canonical text of a collection KEY. The parser only computes
 * that text for a real key, so every decorated form of the value parses. */
TEST(block_sequence, long_one_line_flow_value_is_not_limited_like_a_key) {
  static const char *const prefixes[] = {"- ",     "- &a ",  "- !foo ", "&a ",
                                         "!!seq ", "k: &a ", "- &a !x "};
  size_t failed = 0;
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    char *doc = _big_flow_doc(prefixes[i], "", 8000);
    char *err = NULL;
    cyaml d = doc ? cyaml_parse(doc, &err) : NULL;
    if (!d) {
      printf("    failed: prefix '%s': %s\n", prefixes[i], err ? err : "");
      failed++;
    }
    cyaml_destroy(d);
    free(doc);
  }
  /* The anchored value stays usable through an alias. */
  char *doc = _big_flow_doc("- &a ", "- *a\n", 8000);
  char *err = NULL;
  cyaml d = doc ? cyaml_parse(doc, &err) : NULL;
  size_t alias_len =
      d && cyaml_list_len(d) == 2 ? cyaml_list_len(cyaml_list_get(d, 1)) : 0;
  cyaml_destroy(d);
  free(doc);
  REQUIRE_EQ(failed, (size_t)0);
  REQUIRE_EQ(alias_len, (size_t)8000);
}

/* The same limit applies to a real key. */
TEST(errors, long_one_line_flow_key_still_rejected) {
  char *doc = _big_flow_doc("", "", 8000);
  REQUIRE_NE((void *)doc, NULL);
  /* Turn the trailing "\n" into ": v\n". */
  size_t len = strlen(doc);
  char *key_doc = malloc(len + 4);
  bool built = key_doc != NULL;
  if (built) {
    memcpy(key_doc, doc, len - 1);
    memcpy(key_doc + len - 1, ": v\n", 5);
  }
  char *err = NULL;
  cyaml d = built ? cyaml_parse(key_doc, &err) : NULL;
  bool rejected = built && !d && err != NULL;
  cyaml_destroy(d);
  free(key_doc);
  free(doc);
  REQUIRE_TRUE(rejected);
}

/* An allocator that counts every allocation call. */
static size_t _alias_allocs = 0;
static void *_alias_malloc(size_t sz) {
  _alias_allocs++;
  return malloc(sz);
}
static void *_alias_calloc(size_t n, size_t sz) {
  _alias_allocs++;
  return calloc(n, sz);
}
static void *_alias_realloc(void *old, size_t sz) {
  _alias_allocs++;
  return realloc(old, sz);
}
static ccol_memmgmt_procs_t _alias_mp = {.malloc = _alias_malloc,
                                         .calloc = _alias_calloc,
                                         .realloc = _alias_realloc,
                                         .free = free};

/* Builds an anchor of 40 strings and then n "- *a" entries. */
static char *_alias_list_doc(int n) {
  size_t cap = 1024 + (size_t)n * 8;
  char *s = malloc(cap);
  if (!s) return NULL;
  size_t len = (size_t)snprintf(s, cap, "anchor: &a [");
  for (int i = 0; i < 40; i++)
    len +=
        (size_t)snprintf(s + len, cap - len, "%sstring_%02d", i ? ", " : "", i);
  len += (size_t)snprintf(s + len, cap - len, "]\nlist:\n");
  for (int i = 0; i < n; i++)
    len += (size_t)snprintf(s + len, cap - len, "- *a\n");
  return s;
}

static size_t _allocs_to_parse(const char *doc, cyaml *out) {
  char *err = NULL;
  _alias_allocs = 0;
  *out = cyaml_parse_mp(doc, &err, &_alias_mp);
  return _alias_allocs;
}

/* "- *a" is a value, so the parser copies the anchored node once for it and
 * never a second time to try the alias as a key: each such copy is also
 * charged to the node and byte budgets of the parse. */
TEST(block_sequence, alias_entry_is_copied_once) {
  char *five = _alias_list_doc(5);
  char *six = _alias_list_doc(6);
  cyaml d5 = NULL, d6 = NULL;
  size_t a5 = five ? _allocs_to_parse(five, &d5) : 0;
  size_t a6 = six ? _allocs_to_parse(six, &d6) : 0;
  /* What one copy of the anchored node costs. */
  cyaml anchor = d5 ? cyaml_get(d5, "anchor") : NULL;
  _alias_allocs = 0;
  cyaml copy = anchor ? cyaml_clone(anchor) : NULL;
  size_t one_copy = _alias_allocs;
  bool shapes_ok =
      d5 && d6 && copy && cyaml_list_len(cyaml_get(d6, "list")) == 6 &&
      cyaml_list_len(cyaml_list_get(cyaml_get(d6, "list"), 5)) == 40;
  cyaml_destroy(copy);
  cyaml_destroy(d5);
  cyaml_destroy(d6);
  free(five);
  free(six);
  REQUIRE_TRUE(shapes_ok);
  REQUIRE_GT(one_copy, (size_t)40);
  /* The sixth entry costs one copy and a little list bookkeeping, never two
   * copies. */
  REQUIRE_GE(a6 - a5, one_copy);
  REQUIRE_LT(a6 - a5, one_copy + one_copy / 2);
}

/* ========================================================================== */
/*                  RAW CONTROL BYTES IN A %TAG PREFIX                        */
/* ========================================================================== */

/* A %TAG prefix is ns-uri-char, which excludes every raw control byte. A raw
 * NUL would also end the stored prefix early, so "!e!int" below would resolve
 * to the core tag CYAML_TAG_INT and type the scalar as an integer. */
TEST(nul_in_tokens, raw_control_byte_in_tag_directive_prefix_rejected) {
  static const char nul_doc[] =
      "%TAG !e! tag:yaml.org,2002:\0evil/\n--- !e!int 42\n";
  static const char soh_doc[] =
      "%TAG !e! tag:ex\x01"
      "ample:\n--- !e!x 1\n";
  static const char del_doc[] =
      "%TAG !e! tag:ex\x7f"
      "ample:\n--- !e!x 1\n";
  static const char ok_doc[] =
      "%TAG !e! tag:example.com,2000:a#b/\n--- !e!x 1\n";
  char *err = NULL;
  cyaml nul = cyaml_parse_n(nul_doc, sizeof(nul_doc) - 1, &err);
  bool nul_rejected = !nul && err && strstr(err, "0x00") != NULL;
  cyaml soh = cyaml_parse_n(soh_doc, sizeof(soh_doc) - 1, &err);
  bool soh_rejected = !soh && err && strstr(err, "0x01") != NULL;
  cyaml del = cyaml_parse_n(del_doc, sizeof(del_doc) - 1, &err);
  bool del_rejected = !del && err && strstr(err, "0x7f") != NULL;
  cyaml ok = cyaml_parse_n(ok_doc, sizeof(ok_doc) - 1, &err);
  bool ok_tag = ok && cyaml_node_tag(ok) &&
                strcmp(cyaml_node_tag(ok), "tag:example.com,2000:a#b/x") == 0;
  cyaml_destroy(nul);
  cyaml_destroy(soh);
  cyaml_destroy(del);
  cyaml_destroy(ok);
  REQUIRE_TRUE(nul_rejected);
  REQUIRE_TRUE(soh_rejected);
  REQUIRE_TRUE(del_rejected);
  REQUIRE_TRUE(ok_tag);
}

/* ========================================================================== */
/*                 KEYS LONGER THAN AN IMPLICIT KEY MAY BE                    */
/* ========================================================================== */

/* Builds a dictionary with one key of the given text, holding a scalar, a
 * dictionary and a list under three suffixes of that text. */
static cyaml _long_key_tree(const char *base) {
  size_t n = strlen(base);
  char *k = malloc(n + 2);
  cyaml d = cyaml_create_dictionary();
  if (!k || !d) {
    free(k);
    cyaml_destroy(d);
    return NULL;
  }
  memcpy(k, base, n);
  k[n + 1] = '\0';
  k[n] = 's';
  cyaml_dictionary_set(d, k, cyaml_create_int(1));
  k[n] = 'd';
  cyaml inner = cyaml_create_dictionary();
  cyaml_dictionary_set(inner, "x", cyaml_create_string("y"));
  cyaml_dictionary_set(d, k, inner);
  k[n] = 'l';
  cyaml list = cyaml_create_list();
  cyaml_list_push(list, cyaml_create_int(2));
  cyaml_dictionary_set(d, k, list);
  free(k);
  return d;
}

/* Both serializers write a key whose rendered text is longer than 1024 bytes
 * in the explicit "? key" form, and the parser reads it back as the same
 * entry. A key of up to 1024 rendered bytes stays implicit. */
static bool _long_key_round_trip(const char *base, bool expect_explicit) {
  cyaml d = _long_key_tree(base);
  if (!d) return false;
  char *block = cyaml_serialize(d);
  char *flow = cyaml_serialize_flow(d);
  bool ok = block && flow;
  if (ok) {
    bool block_explicit =
        strncmp(block, "? ", 2) == 0 || strstr(block, "\n? ") != NULL;
    bool flow_explicit = strstr(flow, "? ") != NULL;
    ok = block_explicit == expect_explicit && flow_explicit == expect_explicit;
  }
  for (int mode = 0; ok && mode < 2; mode++) {
    char *err = NULL;
    cyaml back = cyaml_parse(mode ? flow : block, &err);
    ok = back && cyaml_dictionary_size(back) == 3;
    size_t n = strlen(base);
    char *k = malloc(n + 2);
    if (ok && k) {
      memcpy(k, base, n);
      k[n + 1] = '\0';
      k[n] = 's';
      cyaml s = cyaml_dictionary_get(back, k);
      k[n] = 'd';
      cyaml dd = cyaml_dictionary_get(back, k);
      k[n] = 'l';
      cyaml l = cyaml_dictionary_get(back, k);
      ok = s && cyaml_int_val(s) == 1 && dd && cyaml_dictionary_size(dd) == 1 &&
           l && cyaml_list_len(l) == 1;
    } else {
      ok = false;
    }
    free(k);
    cyaml_destroy(back);
  }
  if (block) cyaml_serialize_free(block);
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(d);
  return ok;
}

TEST(serialize, key_longer_than_an_implicit_key_goes_out_explicit) {
  char plain_1023[1024], plain_1024[1025];
  memset(plain_1023, 'a', 1023);
  plain_1023[1023] = '\0'; /* 1023 + one suffix byte: exactly 1024 */
  memset(plain_1024, 'a', 1024);
  plain_1024[1024] = '\0'; /* 1025 rendered bytes */
  /* 600 raw 0x01 bytes: 601 bytes of text that render as far more than 1024
   * bytes of escapes inside double quotes. */
  char escaped[601];
  memset(escaped, 0x01, 600);
  escaped[600] = '\0';
  /* A long key that needs quotes and holds a line break. */
  char multi[1400];
  memset(multi, 'b', sizeof(multi) - 1);
  multi[sizeof(multi) - 1] = '\0';
  multi[700] = '\n';
  multi[0] = '-';
  bool at_limit = _long_key_round_trip(plain_1023, false);
  bool past_limit = _long_key_round_trip(plain_1024, true);
  bool escapes = _long_key_round_trip(escaped, true);
  bool quoted = _long_key_round_trip(multi, true);
  REQUIRE_TRUE(at_limit);
  REQUIRE_TRUE(past_limit);
  REQUIRE_TRUE(escapes);
  REQUIRE_TRUE(quoted);
}

/* ========================================================================== */
/*              cyaml_set OF AN UNSIGNED VALUE ABOVE LLONG_MAX                */
/* ========================================================================== */

/* No long long holds an unsigned value above LLONG_MAX, so cyaml_set stores
 * the nearest double as a CYAML_FLOAT, which is the node that a parse of the
 * decimal literal gives, and drops a core !!int tag that stops matching. */
TEST(set_value_types, unsigned_above_llong_max_becomes_a_float) {
  char *err = NULL;
  cyaml doc = cyaml_parse("n: !!int 1\nm: 2\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  unsigned long long big = 18446744073709551615ULL;
  unsigned long long edge = (unsigned long long)LLONG_MAX;
  unsigned long long above = (unsigned long long)LLONG_MAX + 1ULL;
  ccol_retval_t r1 = cyaml_set(doc, "n", big);
  ccol_retval_t r2 = cyaml_set(doc, "m", edge);
  ccol_retval_t r3 = cyaml_set(doc, "fresh", above);
  cyaml n = cyaml_get(doc, "n");
  cyaml m = cyaml_get(doc, "m");
  cyaml fresh = cyaml_get(doc, "fresh");
  bool n_ok = n && cyaml_type(n) == CYAML_FLOAT &&
              cyaml_double_val(n) == 18446744073709551615.0 &&
              cyaml_node_tag(n) == NULL;
  bool m_ok =
      m && cyaml_type(m) == CYAML_INTEGER && cyaml_int_val(m) == LLONG_MAX;
  bool fresh_ok = fresh && cyaml_type(fresh) == CYAML_FLOAT &&
                  cyaml_double_val(fresh) == 9223372036854775808.0;
  cyaml parsed = cyaml_parse("n: 18446744073709551615\n", &err);
  cyaml pn = parsed ? cyaml_get(parsed, "n") : NULL;
  bool same_as_parse = pn && n && cyaml_type(pn) == cyaml_type(n) &&
                       cyaml_double_val(pn) == cyaml_double_val(n);
  cyaml_destroy(parsed);
  cyaml_destroy(doc);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_TRUE(n_ok);
  REQUIRE_TRUE(m_ok);
  REQUIRE_TRUE(fresh_ok);
  REQUIRE_TRUE(same_as_parse);
}

extern atomic_ulong _cyaml_pool_key_lock_count_for_tests;

static void *cyaml_pool_arm_worker(void *arg) {
  int *ok = (int *)arg;
  *ok = 1;
  for (int i = 0; i < 5; i++) {
    cyaml d = cyaml_parse("a: 1\nb: [1, 2, 3]\nc: {x: y}\n", NULL);
    if (!d) {
      *ok = 0;
      return NULL;
    }
    cyaml_destroy(d);
  }
  return NULL;
}

/* A thread arms the key of its node pool once, and not every time its pool
 * runs empty. Each parse below drains the pool of the thread and each destroy
 * refills it, so a re-arm on every refill takes the pool key lock once per
 * parse. That lock is written on every acquisition, and on a thread that
 * parses in a loop it becomes a line that every core writes once per parse.
 * This test is non-vacuous: arming on every refill makes the count 5. */
TEST(node_pool, a_thread_takes_the_pool_key_lock_once) {
  unsigned long before = atomic_load(&_cyaml_pool_key_lock_count_for_tests);
  int ok = 0;
  pthread_t th;
  int created = pthread_create(&th, NULL, cyaml_pool_arm_worker, &ok);
  if (created == 0) pthread_join(th, NULL);
  unsigned long after = atomic_load(&_cyaml_pool_key_lock_count_for_tests);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(after - before, 1ul);
}

/* ========================================================================== */
/*                         SERIALIZER INTEROPERABILITY                        */
/* ========================================================================== */

/* Serialize a one-element list and a one-member dictionary holding s, in both
 * styles. Report whether every output holds s double-quoted (so no plain
 * form of it reaches a YAML 1.1 reader), holds none of the raw line-break
 * code points NEL, LS and PS, and parses back to exactly s. */
static bool _ser_quotes_and_round_trips(const char *s) {
  cyaml list = cyaml_create_list();
  cyaml dict = cyaml_create_dictionary();
  if (!list || !dict) {
    cyaml_destroy(list);
    cyaml_destroy(dict);
    return false;
  }
  cyaml_list_push(list, cyaml_create_string(s));
  cyaml_dictionary_set(dict, s, cyaml_create_string(s));
  bool ok = true;
  cyaml roots[2] = {list, dict};
  for (int r = 0; r < 2; r++) {
    for (int flow = 0; flow < 2; flow++) {
      char *out =
          flow ? cyaml_serialize_flow(roots[r]) : cyaml_serialize(roots[r]);
      if (!out) {
        ok = false;
        continue;
      }
      if (!strchr(out, '"')) ok = false;
      if (strstr(out, "\xC2\x85") || strstr(out, "\xE2\x80\xA8") ||
          strstr(out, "\xE2\x80\xA9"))
        ok = false;
      cyaml back = cyaml_parse(out, NULL);
      if (r == 0) {
        cyaml e = back ? cyaml_list_get(back, 0) : NULL;
        if (!e || cyaml_type(e) != CYAML_STRING ||
            strcmp(cyaml_str_val(e), s) != 0)
          ok = false;
      } else {
        cyaml e = back ? cyaml_dictionary_get(back, s) : NULL;
        if (!e || cyaml_type(e) != CYAML_STRING ||
            strcmp(cyaml_str_val(e), s) != 0)
          ok = false;
      }
      cyaml_destroy(back);
      cyaml_serialize_free(out);
    }
  }
  cyaml_destroy(list);
  cyaml_destroy(dict);
  return ok;
}

/* Each of these strings is a valid plain scalar in YAML 1.2, and PyYAML or
 * libyaml reads each one differently, or refuses it, when it goes out plain:
 * NEL, LS and PS are line breaks in YAML 1.1, a tab ends a plain scalar in
 * both, '?' is an indicator inside a flow collection, and a lone "=" is the
 * YAML 1.1 "value" type that PyYAML refuses to construct. This test is
 * non-vacuous: without the matching clauses of the quoting rule, each of
 * these strings goes out plain, and without the \N, \L and \P escapes the
 * raw code points reach the output. */
TEST(serialize_interop, strings_that_yaml11_readers_misread_are_quoted) {
  static const char *const cases[] = {
      "a\xE2\x80\xA8"
      "b",
      "a\xE2\x80\xA9"
      "b",
      "a\xC2\x85"
      "b",
      "\xE2\x80\xA8",
      "a\tb",
      "a?b",
      "ab?",
      "a ? b",
      "=",
  };
  size_t n = sizeof(cases) / sizeof(cases[0]);
  size_t good = 0;
  char failed[128] = "";
  for (size_t i = 0; i < n; i++) {
    if (_ser_quotes_and_round_trips(cases[i]))
      good++;
    else if (strlen(failed) + 4 < sizeof(failed))
      snprintf(failed + strlen(failed), sizeof(failed) - strlen(failed), "%zu,",
               i);
  }
  REQUIRE_STREQ(failed, "");
  REQUIRE_EQ(good, n);
}

TEST(serialize_interop, line_breaks_of_yaml11_use_their_escapes) {
  /* The double-quoted writer writes NEL, LS and PS as \N, \L and \P, which
   * every reader, 1.1 or 1.2, reads back as the same code point. */
  cyaml l = cyaml_create_list();
  cyaml_list_push(l,
                  cyaml_create_string("x\xC2\x85y\xE2\x80\xA8z\xE2\x80\xA9w"));
  char *out = cyaml_serialize_flow(l);
  bool ok = out && strcmp(out, "[\"x\\Ny\\Lz\\Pw\"]") == 0;
  cyaml_serialize_free(out);
  cyaml_destroy(l);
  REQUIRE_TRUE(ok);
}

/* Report whether out holds a raw C1 control other than NEL, or a raw
 * U+FFFE or U+FFFF. */
static bool _ser_has_raw_non_printable(const char *out) {
  for (const unsigned char *p = (const unsigned char *)out; *p; p++) {
    if (p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F && p[1] != 0x85)
      return true;
    if (p[0] == 0xEF && p[1] == 0xBF && (p[2] == 0xBE || p[2] == 0xBF))
      return true;
  }
  return false;
}

/* The C1 controls other than NEL, and U+FFFE and U+FFFF, are outside the
 * c-printable set of YAML 1.2, and libyaml, PyYAML and go-yaml refuse a whole
 * document that holds one raw. Each such string goes out double-quoted with
 * the code point escaped, as a value and as a key, in block and in flow
 * style, and parses back to the same bytes. This test is non-vacuous:
 * without the matching clauses of the quoting rule and of the double-quoted
 * writer, the raw bytes reach the output. */
TEST(serialize_interop, c1_controls_and_noncharacters_are_escaped) {
  static const char *const cases[] = {
      "Don\xC2\x92t",  "\xC2\x80",      "a\xC2\x9F",
      "x\xEF\xBF\xBE", "\xEF\xBF\xBFy", "\xC2\x81:\xC2\x8D",
  };
  size_t n = sizeof(cases) / sizeof(cases[0]);
  size_t good = 0;
  char failed[128] = "";
  for (size_t i = 0; i < n; i++) {
    bool ok = _ser_quotes_and_round_trips(cases[i]);
    cyaml d = cyaml_create_dictionary();
    if (d && cyaml_dictionary_set(d, cases[i], cyaml_create_string(cases[i])) ==
                 ccol_success) {
      char *b = cyaml_serialize(d);
      char *f = cyaml_serialize_flow(d);
      if (!b || !f || _ser_has_raw_non_printable(b) ||
          _ser_has_raw_non_printable(f))
        ok = false;
      cyaml_serialize_free(b);
      cyaml_serialize_free(f);
    } else {
      ok = false;
    }
    cyaml_destroy(d);
    if (ok)
      good++;
    else if (strlen(failed) + 4 < sizeof(failed))
      snprintf(failed + strlen(failed), sizeof(failed) - strlen(failed), "%zu,",
               i);
  }
  REQUIRE_STREQ(failed, "");
  REQUIRE_EQ(good, n);
}

TEST(serialize_interop, c1_controls_and_noncharacters_use_their_escapes) {
  /* A C1 control goes out as \xNN and a noncharacter as \uFFFE or \uFFFF.
   * U+00A0, which is printable, stays raw and plain. */
  cyaml l = cyaml_create_list();
  cyaml_list_push(l, cyaml_create_string("Don\xC2\x92t"));
  cyaml_list_push(l, cyaml_create_string("x\xEF\xBF\xBEy\xEF\xBF\xBF"));
  cyaml_list_push(l, cyaml_create_string("a\xC2\xA0"
                                         "b"));
  char *out = cyaml_serialize_flow(l);
  bool ok = out && strcmp(out,
                          "[\"Don\\x92t\", \"x\\uFFFEy\\uFFFF\", "
                          "a\xC2\xA0"
                          "b]") == 0;
  cyaml_serialize_free(out);
  cyaml_destroy(l);
  REQUIRE_TRUE(ok);
}

/* A '#' is valid inside a verbatim tag in YAML 1.2, but libyaml and PyYAML
 * refuse a verbatim tag that holds one literally. The serializer writes it
 * as %23, which decodes back to the same tag. This test is non-vacuous:
 * with '#' in the literal set the output holds "app#frag". */
TEST(serialize_interop, hash_in_verbatim_tag_is_percent_escaped) {
  cyaml s = cyaml_create_string("v");
  ccol_retval_t rv = s ? cyaml_node_set_tag(s, "tag:example.com,2000:app#frag")
                       : ccol_not_enough_memory;
  char *out = rv == ccol_success ? cyaml_serialize(s) : NULL;
  bool escaped =
      out && strstr(out, "!<tag:example.com,2000:app%23frag>") != NULL;
  cyaml back = out ? cyaml_parse(out, NULL) : NULL;
  const char *tag = back ? cyaml_node_tag(back) : NULL;
  bool same_tag = tag && strcmp(tag, "tag:example.com,2000:app#frag") == 0;
  cyaml_destroy(back);
  cyaml_serialize_free(out);
  cyaml_destroy(s);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(escaped);
  REQUIRE_TRUE(same_tag);
}

TEST(serialize_interop, ordinary_strings_stay_plain) {
  /* The quoting rule does not reach strings that every reader takes as the
   * same plain string. */
  cyaml l = cyaml_create_list();
  cyaml_list_push(l, cyaml_create_string("x=y"));
  cyaml_list_push(l, cyaml_create_string("=="));
  cyaml_list_push(l, cyaml_create_string("caf\xC3\xA9"));
  cyaml_list_push(l, cyaml_create_string("a b"));
  char *out = cyaml_serialize_flow(l);
  bool ok = out && strcmp(out, "[x=y, ==, caf\xC3\xA9, a b]") == 0;
  cyaml_serialize_free(out);
  cyaml_destroy(l);
  REQUIRE_TRUE(ok);
}

/* ========================================================================== */
/*                         STREAM SERIALIZATION                               */
/* ========================================================================== */

static const char _k8s_stream[] =
    "apiVersion: v1\n"
    "kind: Namespace\n"
    "metadata:\n"
    "  name: demo\n"
    "---\n"
    "apiVersion: apps/v1\n"
    "kind: Deployment\n"
    "metadata:\n"
    "  name: web\n"
    "  namespace: demo\n"
    "spec:\n"
    "  replicas: 2\n"
    "  template:\n"
    "    spec:\n"
    "      containers:\n"
    "        - name: web\n"
    "          image: \"nginx:1.27\"\n"
    "          args: [\"--port\", \"8080\"]\n"
    "---\n"
    "apiVersion: v1\n"
    "kind: Service\n"
    "metadata:\n"
    "  name: web\n"
    "spec:\n"
    "  ports:\n"
    "    - port: 80\n"
    "      targetPort: 8080\n";

TEST(serialize_stream, kubernetes_stream_round_trips_as_a_stream) {
  /* Parse a stream of three manifests, edit one, write the list back as a
   * stream and parse it again: the result is a stream of the same three
   * documents, one "---" line before each, with the edit in place. */
  char *err = NULL;
  cyaml docs = cyaml_parse(_k8s_stream, &err);
  ccol_retval_t set_rc =
      docs ? cyaml_set(docs, "#1.spec.replicas", 5) : ccol_invalid_args;
  char *stream = docs ? cyaml_serialize_stream(docs) : NULL;
  cyaml back = stream ? cyaml_parse(stream, NULL) : NULL;
  char *a = docs ? cyaml_serialize_flow(docs) : NULL;
  char *b = back ? cyaml_serialize_flow(back) : NULL;
  bool same = a && b && strcmp(a, b) == 0;
  size_t markers = 0;
  for (const char *p = stream; p && (p = strstr(p, "---\n")); p += 4)
    if (p == stream || p[-1] == '\n') markers++;
  bool starts = stream && strncmp(stream, "---\napiVersion: v1\n", 19) == 0;
  size_t n_back = back ? cyaml_list_len(back) : 0;
  long long replicas =
      back ? cyaml_int_val(cyaml_get(back, "#1.spec.replicas")) : 0;
  cyaml_serialize_free(a);
  cyaml_serialize_free(b);
  cyaml_serialize_free(stream);
  cyaml_destroy(back);
  cyaml_destroy(docs);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(set_rc, ccol_success);
  REQUIRE_EQ(n_back, (size_t)3);
  REQUIRE_EQ(markers, (size_t)3);
  REQUIRE_TRUE(starts);
  REQUIRE_TRUE(same);
  REQUIRE_EQ(replicas, 5LL);
}

TEST(serialize_stream, empty_list_gives_an_empty_stream) {
  cyaml l = cyaml_create_list();
  char *out = cyaml_serialize_stream(l);
  bool empty = out && out[0] == '\0';
  cyaml_serialize_free(out);
  cyaml_destroy(l);
  REQUIRE_TRUE(empty);
}

TEST(serialize_stream, a_node_that_is_not_a_list_is_one_document) {
  cyaml d = cyaml_parse("a: 1\nb: [p, q]\n", NULL);
  char *out = cyaml_serialize_stream(d);
  bool dict_ok = out && strcmp(out, "---\na: 1\nb:\n  - p\n  - q\n") == 0;
  cyaml back = out ? cyaml_parse(out, NULL) : NULL;
  bool back_ok = back && cyaml_type(back) == CYAML_DICTIONARY &&
                 cyaml_dictionary_size(back) == 2;
  cyaml_serialize_free(out);
  cyaml_destroy(back);
  cyaml_destroy(d);
  char *null_out = cyaml_serialize_stream(NULL);
  bool null_ok = null_out && strcmp(null_out, "---\n~\n") == 0;
  cyaml_serialize_free(null_out);
  REQUIRE_TRUE(dict_ok);
  REQUIRE_TRUE(back_ok);
  REQUIRE_TRUE(null_ok);
}

TEST(serialize_stream, scalar_and_tagged_documents_round_trip) {
  /* Scalar roots, an empty root and a tagged root each stay one document;
   * the tag goes out in its verbatim form, which needs no directive. */
  const char *in =
      "--- plain\n"
      "--- 42\n"
      "---\n"
      "--- !app/config\n"
      "k: v\n"
      "--- \"---\"\n";
  cyaml docs = cyaml_parse(in, NULL);
  char *out = docs ? cyaml_serialize_stream(docs) : NULL;
  cyaml back = out ? cyaml_parse(out, NULL) : NULL;
  char *a = docs ? cyaml_serialize_flow(docs) : NULL;
  char *b = back ? cyaml_serialize_flow(back) : NULL;
  bool same = a && b && strcmp(a, b) == 0;
  size_t n = back ? cyaml_list_len(back) : 0;
  const char *tag = back ? cyaml_node_tag(cyaml_list_get(back, 3)) : NULL;
  bool tag_ok = tag && strcmp(tag, "!app/config") == 0;
  cyaml_serialize_free(a);
  cyaml_serialize_free(b);
  cyaml_serialize_free(out);
  cyaml_destroy(back);
  cyaml_destroy(docs);
  REQUIRE_EQ(n, (size_t)5);
  REQUIRE_TRUE(same);
  REQUIRE_TRUE(tag_ok);
}

/* ========================================================================== */
/*                         BYTE ORDER MARK INSIDE A DOCUMENT                  */
/* ========================================================================== */

TEST(bom_inside_document, is_refused) {
  /* Section 5.2 of YAML 1.2: a byte order mark must not appear inside a
   * document. A concatenation of a file without a document start marker and
   * a file that starts with a mark is one mapping to no reader, so the parse
   * refuses it rather than read the mark as the first character of a key.
   * This test is non-vacuous: without the check, the first input parses as
   * one mapping whose second key is "\xEF\xBB\xBF" "b". */
  static const char *const inputs[] = {
      "a: 1\n\xEF\xBB\xBF"
      "b: 2\n",
      "---\n\xEF\xBB\xBF"
      "a: 1\n",
      "- a\n\xEF\xBB\xBF- b\n",
      "a:\n  b: 1\n\xEF\xBB\xBF"
      "c: 2\n",
      "[a,\n\xEF\xBB\xBF"
      "b]\n",
  };
  size_t n = sizeof(inputs) / sizeof(inputs[0]);
  size_t refused = 0;
  for (size_t i = 0; i < n; i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(inputs[i], &err);
    if (!doc && err && strstr(err, "byte order mark")) refused++;
    cyaml_destroy(doc);
  }
  REQUIRE_EQ(refused, n);
}

TEST(bom_inside_document, a_mark_before_a_document_start_is_a_prefix) {
  /* The same concatenation with a "---" after the mark is two documents,
   * and a mark at the very end of the input closes the stream. */
  cyaml two = cyaml_parse("a: 1\n\xEF\xBB\xBF---\nb: 2\n", NULL);
  cyaml cmt = cyaml_parse("a: 1\n\xEF\xBB\xBF# c\n---\nb: 2\n", NULL);
  cyaml tail = cyaml_parse("a: 1\n\xEF\xBB\xBF", NULL);
  bool two_ok = two && cyaml_type(two) == CYAML_LIST &&
                cyaml_list_len(two) == 2 &&
                cyaml_int_val(cyaml_get(two, "#1.b")) == 2;
  bool cmt_ok =
      cmt && cyaml_type(cmt) == CYAML_LIST && cyaml_list_len(cmt) == 2;
  bool tail_ok = tail && cyaml_type(tail) == CYAML_DICTIONARY &&
                 cyaml_dictionary_size(tail) == 1;
  cyaml_destroy(two);
  cyaml_destroy(cmt);
  cyaml_destroy(tail);
  REQUIRE_TRUE(two_ok);
  REQUIRE_TRUE(cmt_ok);
  REQUIRE_TRUE(tail_ok);
}

TEST(bom_inside_document, repeated_marks_at_the_stream_start) {
  /* The first document prefix accepts the same sequence of marks and
   * comment lines as the prefix of a later document. This test is
   * non-vacuous: skipping only one mark at the start reads the second as
   * content. */
  cyaml two = cyaml_parse(
      "\xEF\xBB\xBF\xEF\xBB\xBF"
      "a: 1\n",
      NULL);
  cyaml cmt = cyaml_parse(
      "\xEF\xBB\xBF# c\n\xEF\xBB\xBF"
      "a: 1\n",
      NULL);
  bool two_ok = two && cyaml_type(two) == CYAML_DICTIONARY &&
                cyaml_int_val(cyaml_dictionary_get(two, "a")) == 1;
  bool cmt_ok = cmt && cyaml_type(cmt) == CYAML_DICTIONARY &&
                cyaml_int_val(cyaml_dictionary_get(cmt, "a")) == 1;
  cyaml_destroy(two);
  cyaml_destroy(cmt);
  REQUIRE_TRUE(two_ok);
  REQUIRE_TRUE(cmt_ok);
}

/* ========================================================================== */
/*                  SPECULATIVE FLOW-COLLECTION KEY PARSE                     */
/* ========================================================================== */

/* Parse y, serialize it in flow style, and report whether the text is
 * exactly want. */
static bool _flow_text_is(const char *y, const char *want) {
  char *err = NULL;
  cyaml doc = cyaml_parse(y, &err);
  if (!doc) return false;
  char *out = cyaml_serialize_flow(doc);
  bool ok = out && strcmp(out, want) == 0;
  cyaml_serialize_free(out);
  cyaml_destroy(doc);
  return ok;
}

/* A flow collection with an anchor or a tag in front of it may be an
 * implicit key, so the parser reads it once as a possible key before it
 * reads it as a value. An alias inside it resolves to the anchor that is
 * defined BEFORE the alias, whichever reading wins. This test is
 * non-vacuous: when the possible-key reading leaves its anchors behind, the
 * alias in the value reading resolves to the anchor that the collection
 * defines after it, and each case gives [B, B] in place of [A, B]. */
TEST(anchor_speculation,
     alias_in_a_decorated_flow_value_sees_only_earlier_anchors) {
  /* A sequence entry with an anchor, and one with a tag. */
  REQUIRE_TRUE(_flow_text_is("- &x A\n- &k [*x, &x B]\n", "[A, [A, B]]"));
  REQUIRE_TRUE(_flow_text_is("- &x A\n- !!seq [*x, &x B]\n", "[A, [A, B]]"));
  REQUIRE_TRUE(_flow_text_is("- &x A\n- !!seq &k [*x, &x B]\n", "[A, [A, B]]"));
  /* A value on its own line, as a flow mapping with a tag. */
  REQUIRE_TRUE(_flow_text_is("x: &x A\ny:\n  !!map {a: *x, b: &x B}\n",
                             "{x: A, \"y\": {a: A, b: B}}"));
  REQUIRE_TRUE(
      _flow_text_is("x: &x A\ny:\n  &k [*x, &x B]\n", "{x: A, \"y\": [A, B]}"));
  /* A flow collection over two lines is never a key, and its possible-key
   * reading runs to the end of it before the line check refuses it. */
  REQUIRE_TRUE(_flow_text_is("- &x A\n- &k [*x,\n    &x B]\n", "[A, [A, B]]"));
  /* The anchor on the value itself resolves to the value. */
  REQUIRE_TRUE(
      _flow_text_is("- &x A\n- &k [*x, &x B]\n- *k\n", "[A, [A, B], [A, B]]"));
  /* An anchor that the collection defines again keeps the later definition
   * after the value. */
  REQUIRE_TRUE(
      _flow_text_is("- &x A\n- &k [&x B, C]\n- *x\n", "[A, [B, C], B]"));
}

/* When the flow collection IS a key, the anchors that it defines stay, and
 * the later definition of a name wins after it. This pins the accept half
 * of the speculation: a key reading that dropped its anchors would leave *x
 * at A. */
TEST(anchor_speculation, anchors_of_an_accepted_flow_key_stay) {
  REQUIRE_TRUE(_flow_text_is("- &x A\n- &k [*x, &x B]: v\n- *x\n",
                             "[A, {\"[A, B]\": v}, B]"));
  REQUIRE_TRUE(_flow_text_is("- &x A\n- !!seq [&y C]: v\n- *y\n",
                             "[A, {\"[C]\": v}, C]"));
}

/* An alias in front of the only anchor of its name is unknown, in a
 * decorated flow collection as anywhere else. The possible-key reading
 * reports it, and no reading of the collection lets the alias see the
 * anchor that follows it. */
TEST(anchor_speculation, an_anchor_of_the_refused_reading_is_not_visible) {
  char *err = NULL;
  cyaml doc = cyaml_parse("- &k [*z, &z B]\n", &err);
  bool unknown = err && strstr(err, "unknown alias '*z'") != NULL;
  cyaml_destroy(doc);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_TRUE(unknown);
}

/* The possible-key reading of a decorated flow collection charges nothing
 * that its value reading pays again. A tagged flow list at the root costs
 * exactly the nodes of the same list without the tag, and only the bytes of
 * the tag more. This test is non-vacuous: without the budget rollback the
 * tagged list pays every node and byte twice. */
TEST(anchor_speculation, a_refused_key_reading_is_not_charged) {
  cyaml plain = cyaml_parse("[a, b, [c, d], {e: f}]\n", NULL);
  size_t plain_nodes = cyaml_test_last_parse_nodes_charged;
  size_t plain_bytes = cyaml_test_last_parse_bytes_charged;
  cyaml tagged = cyaml_parse("!!seq [a, b, [c, d], {e: f}]\n", NULL);
  size_t tagged_nodes = cyaml_test_last_parse_nodes_charged;
  size_t tagged_bytes = cyaml_test_last_parse_bytes_charged;
  bool both = plain && tagged;
  cyaml_destroy(plain);
  cyaml_destroy(tagged);
  REQUIRE_TRUE(both);
  REQUIRE_EQ(tagged_nodes, plain_nodes);
  REQUIRE_LT(tagged_bytes, plain_bytes + 64);
}

/* ========================================================================== */
/*                  NODE POOL ACROSS THREAD-EXIT DESTRUCTORS                  */
/* ========================================================================== */

static pthread_key_t _late_doc_key;

static void _late_doc_dtor(void *p) {
  cyaml doc = (cyaml)p;
  cyaml_destroy(doc);
}

static void *_late_doc_worker(void *arg) {
  int *ok = (int *)arg;
  /* Arm the pool key of this thread before the application key gets its
   * value, so the pool drains before the application destructor runs. */
  cyaml w = cyaml_create_null();
  cyaml_destroy(w);
  cyaml d = cyaml_parse("a: [1, 2, 3]\nb: x\n", NULL);
  *ok = d != NULL && pthread_setspecific(_late_doc_key, d) == 0;
  if (!*ok) cyaml_destroy(d);
  return NULL;
}

/* A destructor of another thread-specific key can free nodes after the pool
 * of the thread already drained at thread exit. The first such free arms the
 * pool key again, so the C library runs one more destructor round that
 * drains those nodes too. The test counts the two arms. make memtest also
 * reports the nodes as definitely lost when the second drain does not run.
 * This test is non-vacuous: a drain that leaves the thread marked as armed
 * arms once, and the nodes of the late destroy leak. */
TEST(node_pool, nodes_freed_by_a_later_thread_destructor_are_drained) {
  cyaml w = cyaml_create_null(); /* the pool key exists before ours */
  cyaml_destroy(w);
  int key_rc = pthread_key_create(&_late_doc_key, _late_doc_dtor);
  unsigned long before = atomic_load(&_cyaml_pool_key_lock_count_for_tests);
  int ok = 0;
  int created = -1;
  pthread_t th;
  if (key_rc == 0) {
    created = pthread_create(&th, NULL, _late_doc_worker, &ok);
    if (created == 0) pthread_join(th, NULL);
    pthread_key_delete(_late_doc_key);
  }
  unsigned long after = atomic_load(&_cyaml_pool_key_lock_count_for_tests);
  REQUIRE_EQ(key_rc, 0);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(after - before, 2ul);
}

/* ========================================================================== */
/*                     SIZE AND CHARGE OF SCALAR TEXT                         */
/* ========================================================================== */

/* An allocator that tracks the bytes it holds, through a size header in
 * front of each block. */
static size_t _held_bytes;
typedef union {
  size_t size;
  max_align_t align;
} _held_hdr_t;
static void *_held_malloc(size_t sz) {
  _held_hdr_t *h = malloc(sizeof(*h) + sz);
  if (!h) return NULL;
  h->size = sz;
  _held_bytes += sz;
  return h + 1;
}
static void *_held_calloc(size_t n, size_t sz) {
  if (sz && n > SIZE_MAX / sz) return NULL;
  void *p = _held_malloc(n * sz);
  if (p) memset(p, 0, n * sz);
  return p;
}
static void _held_free(void *p) {
  if (!p) return;
  _held_hdr_t *h = (_held_hdr_t *)p - 1;
  _held_bytes -= h->size;
  free(h);
}
static void *_held_realloc(void *p, size_t sz) {
  if (!p) return _held_malloc(sz);
  _held_hdr_t *h = (_held_hdr_t *)p - 1;
  size_t old = h->size;
  _held_hdr_t *n = realloc(h, sizeof(*n) + sz);
  if (!n) return NULL;
  n->size = sz;
  _held_bytes = _held_bytes - old + sz;
  return n + 1;
}
static ccol_memmgmt_procs_t _held_mp = {.malloc = _held_malloc,
                                        .calloc = _held_calloc,
                                        .realloc = _held_realloc,
                                        .free = _held_free};

/* Build a flow list of n copies of item, parse it through _held_mp, and give
 * the bytes that the document holds and the bytes that the parse charged. */
static bool _scalar_list_cost(const char *item, size_t n, size_t *held,
                              size_t *charged) {
  size_t il = strlen(item);
  char *d = malloc(n * (il + 1) + 2);
  if (!d) return false;
  char *p = d;
  *p++ = '[';
  for (size_t i = 0; i < n; i++) {
    memcpy(p, item, il);
    p += il;
    *p++ = ',';
  }
  p[-1] = ']';
  *p = '\0';
  size_t base = _held_bytes;
  cyaml doc = cyaml_parse_mp(d, NULL, &_held_mp);
  *held = _held_bytes - base;
  *charged = cyaml_test_last_parse_bytes_charged;
  bool ok = doc && cyaml_list_len(doc) == n;
  cyaml_destroy(doc);
  free(d);
  return ok;
}

/* The text of a quoted scalar and of a block scalar is kept at its own
 * length and charged to the byte budget, exactly like the text of a plain
 * scalar. The same list of the same strings therefore holds and charges the
 * same bytes in every style. This test is non-vacuous: a node that keeps the
 * whole scanner buffer of its text holds about 250 bytes more for each
 * quoted item, and charges about 20 bytes less. */
TEST(scalar_text, quoted_text_is_sized_and_charged_like_plain_text) {
  size_t n = 1000;
  size_t plain_held = 0, plain_charged = 0;
  size_t dq_held = 0, dq_charged = 0;
  size_t sq_held = 0, sq_charged = 0;
  size_t tag_held = 0, tag_charged = 0;
  bool ok = _scalar_list_cost("abc", n, &plain_held, &plain_charged) &&
            _scalar_list_cost("\"abc\"", n, &dq_held, &dq_charged) &&
            _scalar_list_cost("'abc'", n, &sq_held, &sq_charged) &&
            _scalar_list_cost("!!str abc", n, &tag_held, &tag_charged);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(_held_bytes, (size_t)0);
  REQUIRE_EQ(dq_held, plain_held);
  REQUIRE_EQ(sq_held, plain_held);
  REQUIRE_EQ(dq_charged, plain_charged);
  REQUIRE_EQ(sq_charged, plain_charged);
  /* A tagged scalar also holds and charges its tag. */
  REQUIRE_LT(tag_held, plain_held + n * 32);
  REQUIRE_GT(tag_charged, plain_charged);
}

/* Build a block list of n copies of item, parse it through _held_mp, and
 * give the bytes that the document holds. */
static bool _block_list_held(const char *item, size_t n, size_t *held) {
  size_t il = strlen(item);
  char *d = malloc(n * il + 1);
  if (!d) return false;
  for (size_t i = 0; i < n; i++) memcpy(d + i * il, item, il);
  d[n * il] = '\0';
  size_t base = _held_bytes;
  cyaml doc = cyaml_parse_mp(d, NULL, &_held_mp);
  *held = _held_bytes - base;
  bool ok = doc && cyaml_list_len(doc) == n &&
            strcmp(cyaml_str_val(cyaml_list_get(doc, 0)), "x") == 0;
  cyaml_destroy(doc);
  free(d);
  return ok;
}

/* A block scalar keeps its text at its own length too. This test is
 * non-vacuous: a literal or folded scalar that keeps its scanner buffer
 * holds about 250 bytes more for each item. */
TEST(scalar_text, block_scalar_text_is_sized_to_its_length) {
  size_t n = 200;
  size_t plain = 0, literal = 0, folded = 0;
  bool ok = _block_list_held("- x\n", n, &plain) &&
            _block_list_held("- |-\n  x\n", n, &literal) &&
            _block_list_held("- >-\n  x\n", n, &folded);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(literal, plain);
  REQUIRE_EQ(folded, plain);
}

/* ========================================================================== */
/*                    MERGE KEYS AND THE DOCUMENT LIMITS                      */
/* ========================================================================== */

/* A merge expansion that the ordinary document memory limit stops reports
 * that limit, as an alias expansion does. The test lowers the byte floor, so
 * that a small nested merge document reaches it. This test is non-vacuous:
 * without the check of the budget flags the error reads "out of memory
 * expanding merge key". */
TEST(merge_keys, a_merge_stopped_by_the_document_limit_names_it) {
  char doc[4096];
  size_t len = (size_t)snprintf(
      doc, sizeof(doc),
      "m0: &m0 {k0: \"%s\", k1: \"%s\", k2: \"%s\", k3: \"%s\"}\n",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy");
  for (int lvl = 1; lvl < 9; lvl++) {
    len +=
        (size_t)snprintf(doc + len, sizeof(doc) - len, "m%d: &m%d {", lvl, lvl);
    for (int q = 0; q < 8; q++)
      len += (size_t)snprintf(doc + len, sizeof(doc) - len, "%sq%d: {<<: *m%d}",
                              q ? ", " : "", q, lvl - 1);
    len += (size_t)snprintf(doc + len, sizeof(doc) - len, "}\n");
  }
  REQUIRE_LT(len, sizeof(doc));
  size_t saved = cyaml_test_parse_byte_floor;
  cyaml_test_parse_byte_floor = (size_t)1024 * 1024;
  char *err = NULL;
  cyaml y = cyaml_parse(doc, &err);
  bool named = err && strstr(err, "merge key expansion exceeded") &&
               strstr(err, "document memory limit");
  cyaml_destroy(y);
  cyaml_test_parse_byte_floor = saved;
  REQUIRE_EQ((void *)y, NULL);
  REQUIRE_TRUE(named);
}

/* A "<<" key whose tag is the core merge tag is a merge key, in each spelling
 * of the tag and of the key, and in each key form. Any other tag keeps it an
 * ordinary key. This test is non-vacuous: treating every tagged "<<" as a
 * literal key leaves a "<<" member in each of the merge cases. */
TEST(merge_keys, a_key_tagged_with_the_merge_tag_merges) {
  const char *src = "b: &b {x: 1}\n";
  static const char *const merges[] = {
      "c:\n  !!merge <<: *b\n  y: 2\n",
      "c:\n  y: 2\n  !!merge <<: *b\n",
      "c:\n  !<tag:yaml.org,2002:merge> <<: *b\n  y: 2\n",
      "c:\n  !!merge \"<<\": *b\n  y: 2\n",
      "c:\n  &a !!merge <<: *b\n  y: 2\n",
      "c:\n  !!merge &a <<: *b\n  y: 2\n",
      "c: {!!merge <<: *b, y: 2}\n",
      "c: {!!merge '<<': *b, y: 2}\n",
      "c: {? !!merge << : *b, y: 2}\n",
      "c:\n  ? !!merge <<\n  : *b\n  y: 2\n",
  };
  for (size_t i = 0; i < sizeof(merges) / sizeof(merges[0]); i++) {
    char doc[256];
    snprintf(doc, sizeof(doc), "%s%s", src, merges[i]);
    char *err = NULL;
    cyaml d = cyaml_parse(doc, &err);
    cyaml c = d ? cyaml_dictionary_get(d, "c") : NULL;
    bool merged = c && cyaml_dictionary_size(c) == 2 &&
                  cyaml_dictionary_get(c, "<<") == NULL &&
                  cyaml_int_val(cyaml_dictionary_get(c, "x")) == 1 &&
                  cyaml_int_val(cyaml_dictionary_get(c, "y")) == 2;
    cyaml_destroy(d);
    if (!merged) printf("not merged: %s\n", merges[i]);
    REQUIRE_TRUE(merged);
  }
  char *err = NULL;
  cyaml d = cyaml_parse(
      "%TAG !y! tag:yaml.org,2002:\n---\nb: &b {x: 1}\nc:\n  !y!merge <<: *b\n",
      &err);
  cyaml c = d ? cyaml_dictionary_get(d, "c") : NULL;
  bool merged = c && cyaml_dictionary_get(c, "<<") == NULL &&
                cyaml_int_val(cyaml_dictionary_get(c, "x")) == 1;
  cyaml_destroy(d);
  REQUIRE_TRUE(merged);
  /* Any other tag, and the merge tag on another key, keep the key. */
  static const char *const literal[] = {
      "c:\n  !!str <<: *b\n",
      "c:\n  !custom <<: *b\n",
      "c: {!!str <<: *b}\n",
      "c:\n  !!merge nope: *b\n",
  };
  for (size_t i = 0; i < sizeof(literal) / sizeof(literal[0]); i++) {
    char doc[256];
    snprintf(doc, sizeof(doc), "%s%s", src, literal[i]);
    cyaml dd = cyaml_parse(doc, NULL);
    cyaml cc = dd ? cyaml_dictionary_get(dd, "c") : NULL;
    bool kept = cc && cyaml_dictionary_size(cc) == 1 &&
                cyaml_dictionary_get(cc, "x") == NULL;
    cyaml_destroy(dd);
    REQUIRE_TRUE(kept);
  }
}

/* ========================================================================== */
/*                           VERBATIM TAG OUTPUT                              */
/* ========================================================================== */

/* A verbatim tag goes out with every byte that ns-uri-char does not allow
 * literally percent-escaped, so every YAML reader accepts it, and it parses
 * back to the same tag. This test is non-vacuous: writing a space, a quote,
 * a brace or a UTF-8 byte literally leaves that byte in the output. */
TEST(serialize_tags, a_verbatim_tag_holds_only_uri_characters) {
  static const char *const tags[] = {
      "tag:x y<z>\"q\"{a}|b\\c^d`e",
      "caf\xc3\xa9:/1",
      "a%b",
      "tag:example.com,2000:a-b_c.d~e!f*g'h(i)j[k]l;m/n?o:p@q&r=s+t$u,v#w",
  };
  for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
    for (int flow = 0; flow < 2; flow++) {
      cyaml l = cyaml_create_list();
      cyaml s = cyaml_create_string("v");
      bool built = l && s && cyaml_node_set_tag(s, tags[i]) == ccol_success &&
                   cyaml_list_push(l, s) == ccol_success;
      if (!built && l && s && cyaml_list_len(l) == 0) cyaml_destroy(s);
      char *out =
          built ? (flow ? cyaml_serialize_flow(l) : cyaml_serialize(l)) : NULL;
      bool uri_only = out != NULL;
      const char *open = out ? strstr(out, "!<") : NULL;
      const char *close = open ? strchr(open, '>') : NULL;
      uri_only = uri_only && open && close;
      for (const char *p = open ? open + 2 : NULL; uri_only && p < close; p++) {
        unsigned char c = (unsigned char)*p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  strchr("-#;/?:@&=+$,_.!~*'()[]%", c);
        if (!ok) uri_only = false;
      }
      cyaml back = out ? cyaml_parse(out, NULL) : NULL;
      const char *t = back && cyaml_list_len(back) == 1
                          ? cyaml_node_tag(cyaml_list_get(back, 0))
                          : NULL;
      bool same = t && strcmp(t, tags[i]) == 0;
      cyaml_destroy(back);
      cyaml_serialize_free(out);
      cyaml_destroy(l);
      REQUIRE_TRUE(built);
      REQUIRE_TRUE(uri_only);
      REQUIRE_TRUE(same);
    }
  }
}

/* ------------------------------------------------------------------ */
/* The allocator procs struct of the caller                           */
/* ------------------------------------------------------------------ */

/* The honest allocator of the tests below. It counts the blocks that it hands
 * out and has not yet taken back, and every call. */
static atomic_long _pl_live;
static atomic_long _pl_calls;
/* The poison allocator, which the tests write over the struct of the caller
 * after the tree exists. It forwards to the C library, so a call through it
 * corrupts nothing; it only counts, and any count above zero means that the
 * library read the struct of the caller after the call that took it. */
static atomic_long _pl_poison_calls;

static void *_pl_malloc(size_t n) {
  _pl_calls++;
  void *p = malloc(n);
  if (p) _pl_live++;
  return p;
}
static void _pl_free(void *p) {
  if (p) _pl_live--;
  free(p);
}
static void *_pl_calloc(size_t a, size_t b) {
  _pl_calls++;
  void *p = calloc(a, b);
  if (p) _pl_live++;
  return p;
}
static void *_pl_realloc(void *p, size_t n) {
  _pl_calls++;
  void *q = realloc(p, n);
  if (!p && q) _pl_live++;
  return q;
}
static void *_pl_poison_malloc(size_t n) {
  _pl_poison_calls++;
  return malloc(n);
}
static void _pl_poison_free(void *p) {
  _pl_poison_calls++;
  free(p);
}
static void *_pl_poison_calloc(size_t a, size_t b) {
  _pl_poison_calls++;
  return calloc(a, b);
}
static void *_pl_poison_realloc(void *p, size_t n) {
  _pl_poison_calls++;
  return realloc(p, n);
}

/* A tree built from a procs struct keeps using the allocator that the struct
 * named when the call ran, after the caller overwrote the struct. The struct
 * lives on the heap here so that the test is deterministic; a struct on the
 * stack of a helper that returned is the same case with undefined behavior
 * in place of the poison count. This test is non-vacuous: a node that keeps
 * the pointer of the caller sends every later allocation of the tree through
 * the poison allocator. */
TEST(procs_lifetime, tree_does_not_read_the_procs_struct_of_the_caller_later) {
  const ccol_memmgmt_procs_t honest = {_pl_malloc, _pl_free, _pl_calloc,
                                       _pl_realloc};
  const ccol_memmgmt_procs_t poison = {_pl_poison_malloc, _pl_poison_free,
                                       _pl_poison_calloc, _pl_poison_realloc};
  ccol_memmgmt_procs_t *mp = malloc(sizeof(*mp));
  REQUIRE_NE((void *)mp, NULL);
  *mp = honest;
  _pl_live = 0;
  _pl_calls = 0;
  _pl_poison_calls = 0;

  cyaml root = cyaml_parse_mp("a: [1, 2, {b: x}]\n", NULL, mp);
  cyaml leaf = cyaml_create_string_mp("a leaf string", mp);
  cyaml list = cyaml_create_list_mp(mp);
  *mp = poison;
  long calls_at_overwrite = _pl_calls;

  bool built = root && leaf && list;
  ccol_retval_t set_rv = ccol_invalid_args, push_rv = ccol_invalid_args,
                dict_rv = ccol_invalid_args;
  char *text = NULL;
  cyaml copy = NULL;
  if (built) {
    set_rv = cyaml_set(root, "a.#2.c", "a string long enough to be copied");
    push_rv = cyaml_list_push(list, leaf);
    if (push_rv == ccol_success) leaf = NULL;
    dict_rv = cyaml_dictionary_set(root, "list", list);
    if (dict_rv == ccol_success) list = NULL;
    text = cyaml_serialize(root);
    copy = cyaml_clone(root);
  }
  bool have_text = text != NULL;
  bool have_copy = copy != NULL;
  if (text) cyaml_serialize_free_mp(text, (ccol_memmgmt_procs_t *)&honest);
  cyaml_destroy(copy);
  cyaml_destroy(leaf);
  cyaml_destroy(list);
  cyaml_destroy(root);
  long poison_calls = _pl_poison_calls;
  long live = _pl_live;
  long later_calls = _pl_calls - calls_at_overwrite;
  free(mp);

  REQUIRE_TRUE(built);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(push_rv, ccol_success);
  REQUIRE_EQ(dict_rv, ccol_success);
  REQUIRE_TRUE(have_text);
  REQUIRE_TRUE(have_copy);
  REQUIRE_EQ(poison_calls, 0L);
  REQUIRE_GT(later_calls, 0L);
  REQUIRE_EQ(live, 0L);
}

/* Two structs with the same four functions are one allocator. A tree built
 * from a struct that the caller already released can therefore take a child
 * built from another struct with the same content, and every block still
 * goes back to the allocator that made it. */
TEST(procs_lifetime, procs_structs_with_the_same_functions_are_one_allocator) {
  ccol_memmgmt_procs_t first = {_pl_malloc, _pl_free, _pl_calloc, _pl_realloc};
  ccol_memmgmt_procs_t second = first;
  ccol_memmgmt_procs_t *a = NULL, *b = NULL;
  ccol_retval_t ra = ccol_procs_intern(&first, &a);
  ccol_retval_t rb = ccol_procs_intern(&second, &b);
  ccol_memmgmt_procs_t *again = NULL;
  ccol_retval_t rc = ccol_procs_intern(a, &again);
  ccol_memmgmt_procs_t *none = (ccol_memmgmt_procs_t *)&first;
  ccol_retval_t rn = ccol_procs_intern(NULL, &none);
  REQUIRE_EQ(ra, ccol_success);
  REQUIRE_EQ(rb, ccol_success);
  REQUIRE_EQ(rc, ccol_success);
  REQUIRE_EQ(rn, ccol_success);
  REQUIRE_NE((void *)a, (void *)&first);
  REQUIRE_EQ((void *)a, (void *)b);
  REQUIRE_EQ((void *)a, (void *)again);
  REQUIRE_EQ((void *)none, NULL);
}

/* The intern table has a fixed number of slots. A factory function or a
 * parse that needs a new set of functions once every slot is taken fails
 * cleanly, and a set that already has a slot keeps working. */
TEST(procs_lifetime, a_full_procs_table_fails_cleanly) {
  ccol_memmgmt_procs_t known = {_pl_malloc, _pl_free, _pl_calloc, _pl_realloc};
  ccol_memmgmt_procs_t *known_i = NULL;
  ccol_retval_t rk = ccol_procs_intern(&known, &known_i);
  size_t keep = _ccol_procs_intern_used_for_tests();
  size_t filled = 0;
  for (size_t i = keep; i < CCOL_PROCS_INTERN_CAPACITY; i++) {
    /* Distinct contents that nothing ever calls. */
    ccol_memmgmt_procs_t fake = {(ccol_malloc_t)(uintptr_t)(0x1000 + i * 16),
                                 _pl_free, _pl_calloc, _pl_realloc};
    ccol_memmgmt_procs_t *out = NULL;
    if (ccol_procs_intern(&fake, &out) == ccol_success) filled++;
  }
  ccol_memmgmt_procs_t fresh = {_pl_poison_malloc, _pl_poison_free,
                                _pl_poison_calloc, _pl_poison_realloc};
  ccol_memmgmt_procs_t *fresh_i = (ccol_memmgmt_procs_t *)&known;
  ccol_retval_t rf = ccol_procs_intern(&fresh, &fresh_i);
  _pl_poison_calls = 0;
  cyaml refused_node = cyaml_create_int_mp(7, &fresh);
  char *err = NULL;
  cyaml refused_tree = cyaml_parse_mp("[1]", &err, &fresh);
  bool err_named = err && strstr(err, "allocator") != NULL;
  long poison_calls = _pl_poison_calls;
  _pl_live = 0;
  cyaml accepted = cyaml_parse_mp("[1, two]", NULL, &known);
  bool accepted_ok = accepted != NULL;
  bool node_refused = refused_node == NULL;
  bool tree_refused = refused_tree == NULL;
  cyaml_destroy(accepted);
  long live = _pl_live;
  cyaml_destroy(refused_node);
  cyaml_destroy(refused_tree);
  _ccol_procs_intern_truncate_for_tests(keep);

  REQUIRE_EQ(rk, ccol_success);
  REQUIRE_EQ(filled, CCOL_PROCS_INTERN_CAPACITY - keep);
  REQUIRE_EQ(rf, ccol_container_full);
  REQUIRE_EQ((void *)fresh_i, NULL);
  REQUIRE_TRUE(node_refused);
  REQUIRE_TRUE(tree_refused);
  REQUIRE_TRUE(err_named);
  REQUIRE_EQ(poison_calls, 0L);
  REQUIRE_TRUE(accepted_ok);
  REQUIRE_EQ(live, 0L);
}

/* This suite compiles as C11, where the macro `true` is the int 1, so the
 * classifier of cyaml_set() stores it as the CYAML_INTEGER 1, as the header
 * documents, while (bool)true stores a CYAML_BOOL. tests_c23.c pins the C23
 * side, where `true` has the type bool. */
TEST(set_value_types, c11_true_is_an_int_and_a_cast_makes_a_bool) {
  cyaml doc = cyaml_parse("{\"i\": 0, \"b\": 0}", NULL);
  ccol_retval_t ri = doc ? cyaml_set(doc, "i", true) : ccol_not_enough_memory;
  ccol_retval_t rb =
      doc ? cyaml_set(doc, "b", (bool)true) : ccol_not_enough_memory;
  cyaml i = doc ? cyaml_get(doc, "i") : NULL;
  cyaml b = doc ? cyaml_get(doc, "b") : NULL;
  bool i_is_int = i && cyaml_type(i) == CYAML_INTEGER && cyaml_int_val(i) == 1;
  bool b_is_bool = b && cyaml_type(b) == CYAML_BOOL && cyaml_bool_val(b);
  cyaml_destroy(doc);
  REQUIRE_EQ(ri, ccol_success);
  REQUIRE_EQ(rb, ccol_success);
  REQUIRE_TRUE(i_is_int);
  REQUIRE_TRUE(b_is_bool);
}

/* ========================================================================== */
/*          STREAM CHARACTER SET: UTF-8 AND C-PRINTABLE (YAML 1.2 5.1)        */
/* ========================================================================== */

/* True when every byte of s is printable ASCII. */
static bool charset_msg_is_printable_ascii(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
  return true;
}

/* Parses len bytes of doc. True when the parse is refused with a printable
 * ASCII message that holds every one of the (up to three) needles that are
 * not NULL; the tree of an unexpected success is destroyed. */
static bool charset_refused(const char *doc, size_t len, const char *n1,
                            const char *n2, const char *n3) {
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, len, &err);
  bool ok = root == NULL && err != NULL &&
            charset_msg_is_printable_ascii(err) && (!n1 || strstr(err, n1)) &&
            (!n2 || strstr(err, n2)) && (!n3 || strstr(err, n3));
  if (!ok)
    fprintf(stderr,
            "charset_refused: unexpected result for a %zu-byte doc: %s\n", len,
            err ? err : "(accepted)");
  cyaml_destroy(root);
  return ok;
}

/* True when the len bytes of doc parse. */
static bool charset_accepted(const char *doc, size_t len) {
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, len, &err);
  bool ok = root != NULL && err == NULL;
  if (!ok) fprintf(stderr, "charset_accepted: refused: %s\n", err);
  cyaml_destroy(root);
  return ok;
}

#define CHARSET_LEN(lit) (sizeof(lit) - 1)

/* Every ill-formed UTF-8 form is refused, and the message names the kind of
 * defect and the offending bytes. */
TEST(stream_charset, ill_formed_utf8_is_refused_with_the_defect_named) {
  static const struct {
    const char *doc;
    size_t len;
    const char *what;
    const char *bytes;
  } cases[] = {
      {"a: \x80x\n", 7, "continuation byte with no lead byte", "0x80"},
      {"a: \xBFx\n", 7, "continuation byte with no lead byte", "0xbf"},
      {"a: \xC0\xAF\n", 7, "overlong encoding", "0xc0"},
      {"a: \xC1\xBF\n", 7, "overlong encoding", "0xc1"},
      {"a: \xE0\x80\xAF\n", 8, "overlong encoding", "0xe0 0x80"},
      {"a: \xF0\x80\x80\xAF\n", 9, "overlong encoding", "0xf0 0x80"},
      {"a: \xED\xA0\x80\n", 8, "encoded surrogate code point", "0xed 0xa0"},
      {"a: \xED\xBF\xBF\n", 8, "encoded surrogate code point", "0xed 0xbf"},
      {"a: \xF4\x90\x80\x80\n", 9, "code point above U+10FFFF", "0xf4 0x90"},
      {"a: \xF5\x80\x80\x80\n", 9, "never appears in UTF-8", "0xf5"},
      {"a: \xFF\n", 5, "never appears in UTF-8", "0xff"},
      {"a: \xFE\n", 5, "never appears in UTF-8", "0xfe"},
      {"a: \xC3x\n", 6, "truncated sequence", "0xc3, then 0x78"},
      {"a: \xE2\x82x\n", 7, "truncated sequence", "0xe2, then 0x78"},
      {"a: \xF0\x9F\x98x\n", 8, "truncated sequence", "0xf0, then 0x78"},
      {"a: \xC3", 4, "truncated by the end of the input", "0xc3"},
      {"a: \xE2\x82", 5, "truncated by the end of the input", "0xe2"},
      {"a: \xF0\x9F\x98", 6, "truncated by the end of the input", "0xf0"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    REQUIRE_TRUE(charset_refused(cases[i].doc, cases[i].len, "invalid UTF-8",
                                 cases[i].what, cases[i].bytes));
}

/* Every character outside c-printable is refused raw: C0 controls other than
 * TAB, LF and CR, DEL, C1 controls other than NEL, and U+FFFE and U+FFFF. */
TEST(stream_charset, characters_outside_c_printable_are_refused) {
  for (unsigned c = 0; c < 0x20; c++) {
    if (c == 0x09 || c == 0x0A || c == 0x0D) continue;
    char doc[] = "a: xZy\n";
    doc[4] = (char)c;
    char needle[40];
    snprintf(needle, sizeof(needle), "raw control character 0x%02x", c);
    REQUIRE_TRUE(charset_refused(doc, CHARSET_LEN(doc), needle,
                                 "line 1, column 5", NULL));
  }
  REQUIRE_TRUE(charset_refused("a: x\x7fy\n", 7, "raw control character 0x7f",
                               "line 1, column 5", NULL));
  for (unsigned c = 0x80; c <= 0x9F; c++) {
    if (c == 0x85) continue;
    char doc[] = "a: xZZy\n";
    doc[4] = (char)0xC2;
    doc[5] = (char)c;
    char needle[40];
    snprintf(needle, sizeof(needle), "C1 control character U+00%02X", c);
    REQUIRE_TRUE(charset_refused(doc, CHARSET_LEN(doc), needle,
                                 "line 1, column 5", NULL));
  }
  REQUIRE_TRUE(charset_refused("a: x\xEF\xBF\xBEy\n", 9, "noncharacter U+FFFE",
                               "line 1, column 5", NULL));
  REQUIRE_TRUE(charset_refused("a: x\xEF\xBF\xBFy\n", 9, "noncharacter U+FFFF",
                               "line 1, column 5", NULL));
}

/* The c-printable characters at the edges of every range are accepted raw,
 * and so is every well-formed sequence length. */
TEST(stream_charset, c_printable_edges_are_accepted) {
  static const char *const values[] = {
      "x\ty",               /* TAB inside a plain scalar */
      "x\xC2\x85y",         /* NEL */
      "x\xC2\xA0y",         /* U+00A0, the first character after C1 */
      "x\xC3\xA9y",         /* U+00E9 */
      "x\xED\x9F\xBFy",     /* U+D7FF, before the surrogates */
      "x\xEE\x80\x80y",     /* U+E000, after the surrogates */
      "x\xEF\xBF\xBDy",     /* U+FFFD */
      "x\xEF\xBB\xBFy",     /* U+FEFF inside a scalar */
      "x\xF0\x90\x80\x80y", /* U+10000 */
      "x\xF4\x8F\xBF\xBFy", /* U+10FFFF */
      "x~y",                /* 0x7E, the last ASCII character before DEL */
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    char doc[64];
    int n = snprintf(doc, sizeof(doc), "a: \"%s\"\nb: '%s'\n", values[i],
                     values[i]);
    REQUIRE_TRUE(charset_accepted(doc, (size_t)n));
    char *err = NULL;
    cyaml root = cyaml_parse_n(doc, (size_t)n, &err);
    bool same = root &&
                strcmp(cyaml_str_val(cyaml_get(root, "a")), values[i]) == 0 &&
                strcmp(cyaml_str_val(cyaml_get(root, "b")), values[i]) == 0;
    cyaml_destroy(root);
    REQUIRE_TRUE(same);
  }
}

/* The rule covers every part of a stream. Each template holds one '@' where
 * the offending character goes, and each one parses when the '@' becomes an
 * ordinary letter, which shows that the refusal comes from the character and
 * not from the shape of the document. */
TEST(stream_charset, every_context_of_the_stream_is_checked) {
  static const char *const templates[] = {
      "key: va@lue\n",                            /* plain value */
      "ke@y: value\n",                            /* plain key */
      "\"ke@y\": value\n",                        /* double-quoted key */
      "key: \"va@lue\"\n",                        /* double-quoted value */
      "key: 'va@lue'\n",                          /* single-quoted value */
      "key: value # comm@ent\n",                  /* comment after content */
      "# comm@ent\nkey: value\n",                 /* comment line */
      "key: !ta@g value\n",                       /* shorthand tag */
      "key: !<tag:a@b> value\n",                  /* verbatim tag */
      "key: &anc@hor value\nother: *anc@hor\n",   /* anchor and alias */
      "key: |\n  li@teral\n",                     /* literal block scalar */
      "key: >\n  fol@ded\n",                      /* folded block scalar */
      "[a, b@c, d]\n",                            /* flow sequence */
      "{a: b@c, d: e}\n",                         /* flow mapping */
      "- a\n- b@c\n",                             /* block sequence */
      "a: 1\n---\nb: c@d\n",                      /* second document */
      "a: 1\n...\n# c@d\n---\nb: 2\n",            /* document prefix */
      "%TAG !e! tag:ex@ample.com:\n--- !e!x 1\n", /* %TAG directive */
      "%YAML 1.2 # c@d\n---\na: 1\n",             /* directive comment */
      "key:  @ \n", /* whitespace between tokens */
  };
  static const struct {
    const char *bytes;
    const char *needle;
  } bad[] = {
      {"\x01", "raw control character 0x01"},
      {"\x7f", "raw control character 0x7f"},
      {"\xC2\x80", "C1 control character U+0080"},
      {"\xEF\xBF\xBE", "noncharacter U+FFFE"},
      {"\xFF", "invalid UTF-8"},
      {"\xC3", "invalid UTF-8"},
      {"\xED\xA0\x80", "encoded surrogate"},
  };
  for (size_t t = 0; t < sizeof(templates) / sizeof(templates[0]); t++) {
    char good[128], doc[128];
    size_t g = 0;
    for (const char *p = templates[t]; *p; p++)
      good[g++] = *p == '@' ? 'x' : *p;
    REQUIRE_TRUE(charset_accepted(good, g));
    for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
      size_t d = 0;
      for (const char *p = templates[t]; *p; p++) {
        if (*p == '@') {
          memcpy(doc + d, bad[b].bytes, strlen(bad[b].bytes));
          d += strlen(bad[b].bytes);
        } else {
          doc[d++] = *p;
        }
      }
      REQUIRE_TRUE(charset_refused(doc, d, bad[b].needle, NULL, NULL));
    }
  }
}

/* The line and the column are counted from 1. LF, CR and CRLF each end one
 * line, the column counts characters and not bytes, and a byte order mark at
 * the start of the stream takes no column. */
TEST(stream_charset, message_gives_line_and_column) {
  REQUIRE_TRUE(charset_refused("a: 1\nb: 2\nccc: x\x01\n", 17,
                               "line 3, column 7", NULL, NULL));
  REQUIRE_TRUE(charset_refused("a: 1\r\nb: 2\r\nccc: x\x01\r\n", 20,
                               "line 3, column 7", NULL, NULL));
  REQUIRE_TRUE(charset_refused("a: 1\rb: 2\rc: \x02\r", 15, "line 3, column 4",
                               NULL, NULL));
  /* Two two-byte and one three-byte character before the bad byte count as
   * three columns. */
  REQUIRE_TRUE(charset_refused("k: \xC3\xA9\xC3\xA9\xE2\x82\xAC\xFF\n", 15,
                               "line 1, column 7", "0xff", NULL));
  REQUIRE_TRUE(
      charset_refused("\xEF\xBB\xBF"
                      "a: \x01\n",
                      9, "line 1, column 4", NULL, NULL));
  REQUIRE_TRUE(charset_refused("\x01", 1, "line 1, column 1", NULL, NULL));
  /* NEL is not a line break in YAML 1.2: it takes one column. */
  REQUIRE_TRUE(charset_refused("a: \"\xC2\x85\x01\"\n", 9, "line 1, column 6",
                               NULL, NULL));
}

/* A long document checks blocks of sixteen bytes at a time. The offending
 * byte is found at every offset inside a block, and on both sides of the
 * last whole block, which the character walk finishes. */
TEST(stream_charset, offending_byte_is_found_at_every_offset) {
  for (size_t len = 1; len <= 70; len++) {
    for (size_t at = 0; at < len; at++) {
      char doc[80];
      memset(doc, 'a', len);
      doc[at] = '\x01';
      char needle[40];
      snprintf(needle, sizeof(needle), "line 1, column %zu", at + 1);
      REQUIRE_TRUE(charset_refused(doc, len, "0x01", needle, NULL));
      memset(doc, 'a', len);
      doc[at] = (char)0x80;
      REQUIRE_TRUE(charset_refused(doc, len, "0x80", needle, NULL));
    }
    char doc[80];
    memset(doc, 'a', len);
    REQUIRE_TRUE(charset_accepted(doc, len));
  }
}

/* cyaml_parse_n checks exactly len bytes: a bad byte or a truncated sequence
 * at the very end is refused, and bytes past len are never read. */
TEST(stream_charset, parse_n_checks_up_to_the_last_byte_and_no_further) {
  static const char tail_nul[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: x\0";
  REQUIRE_TRUE(charset_refused(tail_nul, CHARSET_LEN(tail_nul),
                               "raw control character 0x00", "line 5, column 5",
                               NULL));
  static const char tail_cut[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: \xE2\x82";
  REQUIRE_TRUE(charset_refused(tail_cut, CHARSET_LEN(tail_cut),
                               "truncated by the end of the input",
                               "line 5, column 4", NULL));
  /* The same truncated euro sign is complete one byte later, so a len that
   * cuts it is refused and the full len is accepted. */
  static const char euro[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: \xE2\x82\xAC";
  REQUIRE_TRUE(charset_refused(euro, CHARSET_LEN(euro) - 1,
                               "truncated by the end of the input", NULL,
                               NULL));
  REQUIRE_TRUE(charset_accepted(euro, CHARSET_LEN(euro)));
  /* Bytes past len are not part of the input. */
  static const char past[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: 5\x01\xFF";
  REQUIRE_TRUE(charset_accepted(past, CHARSET_LEN(past) - 2));
  REQUIRE_TRUE(
      charset_refused(past, CHARSET_LEN(past) - 1, "0x01", NULL, NULL));
}

/* An escape in a double-quoted scalar names a character with c-printable
 * text, so it is accepted even where the raw character is refused, and the
 * value holds the character. A tree that holds such characters serializes
 * them as escapes and reads back as the same text. */
TEST(stream_charset, escaped_forms_of_refused_characters_are_accepted) {
  static const struct {
    const char *doc;
    const char *value;
  } cases[] = {
      {"\"\\x01\"", "\x01"},
      {"\"\\x1f\"", "\x1f"},
      {"\"\\x7f\"", "\x7f"},
      {"\"\\x80\"", "\xC2\x80"},
      {"\"\\x9f\"", "\xC2\x9F"},
      {"\"\\u0085\"", "\xC2\x85"},
      {"\"\\uFFFE\"", "\xEF\xBF\xBE"},
      {"\"\\uffff\"", "\xEF\xBF\xBF"},
      {"\"\\U0010FFFF\"", "\xF4\x8F\xBF\xBF"},
      {"\"\\e\\a\\v\"", "\x1B\x07\x0B"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml root = cyaml_parse(cases[i].doc, &err);
    bool ok = root && cyaml_type(root) == CYAML_STRING &&
              strcmp(cyaml_str_val(root), cases[i].value) == 0;
    char *text = ok ? cyaml_serialize(root) : NULL;
    cyaml back = text ? cyaml_parse(text, &err) : NULL;
    bool round_trip = back && cyaml_type(back) == CYAML_STRING &&
                      strcmp(cyaml_str_val(back), cases[i].value) == 0;
    cyaml_destroy(back);
    cyaml_serialize_free(text);
    cyaml_destroy(root);
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(round_trip);
  }
}

/* Percent-escapes in a tag name the bytes of UTF-8 text, so escapes that
 * decode to bytes that are not UTF-8 are refused, and well-formed ones are
 * decoded. */
TEST(stream_charset, tag_percent_escapes_must_decode_to_utf8) {
  REQUIRE_TRUE(charset_refused("!<tag:a%C3> v", 13, "not well-formed UTF-8",
                               NULL, NULL));
  REQUIRE_TRUE(
      charset_refused("!<%FF> v", 8, "not well-formed UTF-8", NULL, NULL));
  REQUIRE_TRUE(charset_refused("!<%ED%A0%80> v", 14, "not well-formed UTF-8",
                               NULL, NULL));
  REQUIRE_TRUE(
      charset_refused("!e%C3 v", 7, "not well-formed UTF-8", NULL, NULL));
  char *err = NULL;
  cyaml root = cyaml_parse("!<tag:%C3%A9> v", &err);
  bool ok = root && cyaml_node_tag(root) &&
            strcmp(cyaml_node_tag(root), "tag:\xC3\xA9") == 0;
  cyaml_destroy(root);
  REQUIRE_TRUE(ok);
}

/* ========================================================================== */
/*            API STRINGS: WELL-FORMED UTF-8 IS REQUIRED                      */
/* ========================================================================== */

/* Byte strings that are not UTF-8, one of each kind of defect. */
static const char *const api_bad_utf8[] = {
    "a\x80",        "\xC3",         "x\xC3(",           "\xC0\xAF",
    "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF",
    "ok\xE2\x82",
};
#define API_BAD_COUNT (sizeof(api_bad_utf8) / sizeof(api_bad_utf8[0]))

TEST(api_utf8, create_string_refuses_ill_formed_utf8) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml a = cyaml_create_string(api_bad_utf8[i]);
    cyaml b = cyaml_create_string_mp(api_bad_utf8[i], NULL);
    bool refused = a == NULL && b == NULL;
    cyaml_destroy(a);
    cyaml_destroy(b);
    REQUIRE_TRUE(refused);
  }
}

/* Every Unicode scalar value is accepted through the API, including the
 * characters that a document may only hold as an escape: the serializer
 * escapes them, and the output reads back as the same text. */
TEST(api_utf8, characters_that_need_an_escape_are_accepted_and_round_trip) {
  static const char *const values[] = {
      "\x01\x02\x1F",         "a\177b",           "\xC2\x80\xC2\x9F",
      "\xEF\xBF\xBE",         "\xEF\xBF\xBF",     "tab\tlf\ncr\r",
      "\xC2\x85\xE2\x80\xA8", "\xF4\x8F\xBF\xBF",
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    cyaml doc = cyaml_create_dictionary();
    ccol_retval_t rk = doc ? cyaml_dictionary_set(
                                 doc, values[i], cyaml_create_string(values[i]))
                           : ccol_not_enough_memory;
    ccol_retval_t rt =
        doc ? cyaml_node_set_tag(cyaml_dictionary_get(doc, values[i]),
                                 values[i])
            : ccol_not_enough_memory;
    char *text = doc ? cyaml_serialize(doc) : NULL;
    char *err = NULL;
    cyaml back = text ? cyaml_parse(text, &err) : NULL;
    cyaml v = back ? cyaml_dictionary_get(back, values[i]) : NULL;
    bool same = v && cyaml_type(v) == CYAML_STRING &&
                strcmp(cyaml_str_val(v), values[i]) == 0 && cyaml_node_tag(v) &&
                strcmp(cyaml_node_tag(v), values[i]) == 0;
    if (!same) fprintf(stderr, "round trip %zu: %s\n", i, err ? err : "");
    cyaml_destroy(back);
    cyaml_serialize_free(text);
    cyaml_destroy(doc);
    REQUIRE_EQ(rk, ccol_success);
    REQUIRE_EQ(rt, ccol_success);
    REQUIRE_TRUE(same);
  }
}

/* cyaml_set refuses a string value that is not UTF-8 and leaves the tree as
 * it was: an existing leaf keeps its value and type, a list element keeps its
 * value, and a missing key is not created. */
TEST(api_utf8, set_refuses_ill_formed_string_value_and_changes_nothing) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml doc = cyaml_parse("{a: old, n: 5, l: [x]}", NULL);
    const char *bad = api_bad_utf8[i];
    ccol_retval_t r1 = doc ? cyaml_set(doc, "a", bad) : ccol_not_enough_memory;
    ccol_retval_t r2 = doc ? cyaml_set(doc, "n", bad) : ccol_not_enough_memory;
    ccol_retval_t r3 =
        doc ? cyaml_set(doc, "new", bad) : ccol_not_enough_memory;
    ccol_retval_t r4 =
        doc ? cyaml_set(doc, "l.#0", bad) : ccol_not_enough_memory;
    bool unchanged =
        doc && strcmp(cyaml_str_val(cyaml_get(doc, "a")), "old") == 0 &&
        cyaml_type(cyaml_get(doc, "n")) == CYAML_INTEGER &&
        cyaml_int_val(cyaml_get(doc, "n")) == 5 &&
        cyaml_get(doc, "new") == NULL && cyaml_dictionary_size(doc) == 3 &&
        strcmp(cyaml_str_val(cyaml_get(doc, "l.#0")), "x") == 0;
    cyaml_destroy(doc);
    REQUIRE_EQ(r1, ccol_invalid_args);
    REQUIRE_EQ(r2, ccol_invalid_args);
    REQUIRE_EQ(r3, ccol_invalid_args);
    REQUIRE_EQ(r4, ccol_invalid_args);
    REQUIRE_TRUE(unchanged);
  }
}

/* cyaml_set refuses a leaf key that is not UTF-8 and creates nothing. */
TEST(api_utf8, set_refuses_ill_formed_leaf_key) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml doc = cyaml_parse("{a: {b: 1}}", NULL);
    char path[32];
    snprintf(path, sizeof(path), "a.%s", api_bad_utf8[i]);
    ccol_retval_t r1 = doc ? cyaml_set(doc, path, 7) : ccol_not_enough_memory;
    ccol_retval_t r2 =
        doc ? cyaml_set(doc, api_bad_utf8[i], "v") : ccol_not_enough_memory;
    bool unchanged = doc && cyaml_dictionary_size(doc) == 1 &&
                     cyaml_dictionary_size(cyaml_get(doc, "a")) == 1;
    cyaml_destroy(doc);
    REQUIRE_EQ(r1, ccol_invalid_args);
    REQUIRE_EQ(r2, ccol_invalid_args);
    REQUIRE_TRUE(unchanged);
  }
}

/* cyaml_dictionary_set refuses a key that is not UTF-8 with
 * ccol_invalid_args, which leaves the child with the caller: it is not
 * attached and the caller still destroys it. */
TEST(api_utf8, dictionary_set_refuses_ill_formed_key_and_leaves_child) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml map = cyaml_create_dictionary();
    cyaml child = cyaml_create_string("v");
    ccol_retval_t r = (map && child)
                          ? cyaml_dictionary_set(map, api_bad_utf8[i], child)
                          : ccol_not_enough_memory;
    /* The child is still unattached, so a valid key can take it. */
    ccol_retval_t again = r == ccol_invalid_args
                              ? cyaml_dictionary_set(map, "ok", child)
                              : ccol_not_enough_memory;
    bool placed = map && cyaml_dictionary_size(map) == 1 &&
                  cyaml_dictionary_get(map, "ok") == child;
    /* A map that took the child owns it; otherwise the caller frees it. */
    if (r != ccol_success && again != ccol_success) cyaml_destroy(child);
    cyaml_destroy(map);
    REQUIRE_EQ(r, ccol_invalid_args);
    REQUIRE_EQ(again, ccol_success);
    REQUIRE_TRUE(placed);
  }
}

/* cyaml_node_set_tag refuses a tag that is not UTF-8 and keeps the tag that
 * the node has. */
TEST(api_utf8, node_set_tag_refuses_ill_formed_tag_and_keeps_the_old_one) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml n = cyaml_create_string("v");
    ccol_retval_t r0 =
        n ? cyaml_node_set_tag(n, "!keep") : ccol_not_enough_memory;
    ccol_retval_t r =
        n ? cyaml_node_set_tag(n, api_bad_utf8[i]) : ccol_not_enough_memory;
    bool kept =
        n && cyaml_node_tag(n) && strcmp(cyaml_node_tag(n), "!keep") == 0;
    cyaml_destroy(n);
    REQUIRE_EQ(r0, ccol_success);
    REQUIRE_EQ(r, ccol_invalid_args);
    REQUIRE_TRUE(kept);
  }
}

/* ------------------------------------------------------------------------ */
/*  The canonical text of a collection key is built under its limit         */
/* ------------------------------------------------------------------------ */

/* The largest buffer that the canonical key builder allocated on this
 * thread, in bytes. */
extern __thread size_t _cyaml_canonical_key_peak_bytes_for_tests;

/* The builder may hold the limit (64 KiB) plus one byte of text and the NUL,
 * and the buffer grows by doubling from 256 bytes, so it never needs more than
 * 128 KiB. The figure is a literal on purpose: it must not move with the
 * constant of the library that it checks. */
#define CANON_PEAK_CEILING ((size_t)128 * 1024)

/* Parses input, which must be refused with the "exceeds" message of the
 * canonical key limit, and reports the largest canonical buffer that the
 * parse built. */
static bool canon_refused(const char *input, size_t *peak) {
  _cyaml_canonical_key_peak_bytes_for_tests = 0;
  char *err = NULL;
  cyaml doc = cyaml_parse(input, &err);
  *peak = _cyaml_canonical_key_peak_bytes_for_tests;
  bool refused = !doc && err &&
                 strstr(err, "canonical form of a non-scalar key") &&
                 strstr(err, "exceeds 65536 bytes");
  if (!refused && err) fprintf(stderr, "unexpected result: %s\n", err);
  cyaml_destroy(doc);
  return refused;
}

/* Builds "a: &a <anchored>\n? [*a, *a, ...]\n: v\n" with `aliases` aliases. */
static char *canon_alias_doc(const char *anchored, size_t aliases) {
  size_t cap = strlen(anchored) + aliases * 4 + 64;
  char *d = malloc(cap);
  if (!d) return NULL;
  size_t n = (size_t)snprintf(d, cap, "a: &a %s\n? [", anchored);
  for (size_t i = 0; i < aliases; i++)
    n += (size_t)snprintf(d + n, cap - n, "%s*a", i ? "," : "");
  snprintf(d + n, cap - n, "]\n: v\n");
  return d;
}

/* Builds a run of `count` copies of `unit`, with prefix and suffix. */
static char *canon_repeat(const char *prefix, const char *unit, size_t count,
                          const char *suffix) {
  size_t ul = strlen(unit);
  size_t cap = strlen(prefix) + ul * count + strlen(suffix) + 1;
  char *s = malloc(cap);
  if (!s) return NULL;
  size_t n = (size_t)snprintf(s, cap, "%s", prefix);
  for (size_t i = 0; i < count; i++, n += ul) memcpy(s + n, unit, ul);
  snprintf(s + n, cap - n, "%s", suffix);
  return s;
}

/* A key made of many aliases of one scalar whose canonical text is four
 * times its stored text. Rendered in full it is about 1 MiB; the builder
 * must stop at the limit. This test is non-vacuous: a builder that renders
 * the whole key and checks the length afterwards allocates 2 MiB here. */
TEST(canonical_key_cap, aliased_escaped_scalar_stops_at_the_limit) {
  char *scalar = canon_repeat("\"", "\\x01", 4096, "\"");
  char *doc = scalar ? canon_alias_doc(scalar, 64) : NULL;
  size_t peak = 0;
  bool refused = doc && canon_refused(doc, &peak);
  free(doc);
  free(scalar);
  REQUIRE_TRUE(refused);
  REQUIRE_LE(peak, CANON_PEAK_CEILING);
}

/* The same through the percent-escaped verbatim tag of each alias. */
TEST(canonical_key_cap, aliased_long_tag_stops_at_the_limit) {
  char *tagged = canon_repeat("!<tag:x,", "%7B", 8192, "> v");
  char *doc = tagged ? canon_alias_doc(tagged, 16) : NULL;
  size_t peak = 0;
  bool refused = doc && canon_refused(doc, &peak);
  free(doc);
  free(tagged);
  REQUIRE_TRUE(refused);
  REQUIRE_LE(peak, CANON_PEAK_CEILING);
}

/* The same through a long dictionary key inside each alias, which the
 * builder writes in the explicit "? key" form. */
TEST(canonical_key_cap, aliased_long_dictionary_key_stops_at_the_limit) {
  char *map = canon_repeat("{\"", "\\x02", 2048, "\": 1}");
  char *doc = map ? canon_alias_doc(map, 64) : NULL;
  size_t peak = 0;
  bool refused = doc && canon_refused(doc, &peak);
  free(doc);
  free(map);
  REQUIRE_TRUE(refused);
  REQUIRE_LE(peak, CANON_PEAK_CEILING);
}

/* A plain scalar of n bytes inside a list key has the canonical text
 * "[" + scalar + "]", n + 2 bytes. 65536 bytes is accepted, with the text
 * intact, and one byte more is refused. */
TEST(canonical_key_cap, limit_is_exact) {
  char *at = canon_repeat("? [", "a", 65534, "]\n: v\n");
  char *over = canon_repeat("? [", "a", 65535, "]\n: v\n");
  char *key = canon_repeat("[", "a", 65534, "]");
  char *err = NULL;
  cyaml doc = at ? cyaml_parse(at, &err) : NULL;
  bool found = doc && key && cyaml_dictionary_size(doc) == 1 &&
               cyaml_dictionary_get(doc, key) != NULL;
  cyaml_destroy(doc);
  size_t peak = 0;
  bool refused = over && canon_refused(over, &peak);
  free(at);
  free(over);
  free(key);
  REQUIRE_TRUE(found);
  REQUIRE_TRUE(refused);
  REQUIRE_LE(peak, CANON_PEAK_CEILING);
}

/* ------------------------------------------------------------------------ */
/*  Every parse error message is printable ASCII                            */
/* ------------------------------------------------------------------------ */

static bool err_text_is_printable(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
  return true;
}

/* Parses len bytes, which must be refused, and checks that the message is
 * printable ASCII and holds `want` when want is not NULL. */
static bool err_text_check(const char *input, size_t len, const char *want) {
  char *err = NULL;
  cyaml doc = cyaml_parse_n(input, len, &err);
  bool ok = !doc && err && err_text_is_printable(err) &&
            (!want || strstr(err, want) != NULL);
  if (!ok)
    fprintf(stderr, "input %zu bytes, want \"%s\", got \"%s\"\n", len,
            want ? want : "",
            doc   ? "(accepted)"
            : err ? err
                  : "(none)");
  cyaml_destroy(doc);
  return ok;
}

#define ERR_TEXT_CHECK(lit, want) err_text_check((lit), sizeof(lit) - 1, (want))

/* Each message that quotes input text spells a byte outside 0x20..0x7E as
 * <0xNN>. Without that, the decoded ESC and LF of the first case reach the
 * message raw and the second case puts half of a UTF-8 sequence in it. */
TEST(error_text, quoted_input_is_printable_ascii) {
  REQUIRE_TRUE(ERR_TEXT_CHECK("x: !!bool \"\\e[31mRED\\nFAKE LOG LINE\"\n",
                              "'<0x1B>[31mRED<0x0A>FAKE LOG LINE' is not a "
                              "valid !!bool value"));
  REQUIRE_TRUE(
      ERR_TEXT_CHECK("\"\\u\xc3\xa9"
                     "000\"\n",
                     "invalid hex digit '<0xC3>' in \\uXXXX"));
  REQUIRE_TRUE(
      ERR_TEXT_CHECK("\"\\U\xc3\xa9"
                     "0000000\"\n",
                     "invalid hex digit '<0xC3>' in \\UXXXXXXXX"));
  REQUIRE_TRUE(ERR_TEXT_CHECK("\"\\\xc3\xa9\"\n", "unknown escape '\\<0xC3>'"));
  REQUIRE_TRUE(
      ERR_TEXT_CHECK("*\xc3\xa9t\n", "unknown alias '*<0xC3><0xA9>t'"));
  REQUIRE_TRUE(
      ERR_TEXT_CHECK("[*\xc3\xa9t]\n", "unknown alias '*<0xC3><0xA9>t'"));
  REQUIRE_TRUE(ERR_TEXT_CHECK("x: !!int \"\\x01\\x7f\"\n",
                              "'<0x01><0x7F>' is not a valid !!int value"));
  REQUIRE_TRUE(ERR_TEXT_CHECK("x: !!float \"\xc3\xa9\"\n",
                              "'<0xC3><0xA9>' is not a valid !!float value"));
  REQUIRE_TRUE(ERR_TEXT_CHECK("%TAG !\xc3\xa9! tag:x,\n--- x\n",
                              "malformed %TAG handle '!<0xC3><0xA9>!'"));
  REQUIRE_TRUE(ERR_TEXT_CHECK("\"\\q\"\n", "unknown escape '\\q'"));
  REQUIRE_TRUE(ERR_TEXT_CHECK("x: &a, y\n",
                              "unexpected ',' directly after anchor name"));
}

/* A long quoted text is cut at a whole byte, never inside a <0xNN>, and
 * ends with "...", so the message stays short. */
TEST(error_text, long_quoted_input_is_cut_at_a_whole_byte) {
  char *esc = canon_repeat("x: !!int \"", "\\e", 300, "\"\n");
  char *plain = canon_repeat("x: !!int \"", "y", 300, "\"\n");
  char *err = NULL;
  cyaml d1 = esc ? cyaml_parse(esc, &err) : NULL;
  char e1[512] = "";
  if (err) snprintf(e1, sizeof(e1), "%s", err);
  err = NULL;
  cyaml d2 = plain ? cyaml_parse(plain, &err) : NULL;
  char e2[512] = "";
  if (err) snprintf(e2, sizeof(e2), "%s", err);
  cyaml_destroy(d1);
  cyaml_destroy(d2);
  free(esc);
  free(plain);
  REQUIRE_EQ((void *)d1, NULL);
  REQUIRE_EQ((void *)d2, NULL);
  REQUIRE_TRUE(err_text_is_printable(e1));
  REQUIRE_TRUE(err_text_is_printable(e2));
  REQUIRE_NE((void *)strstr(e1, "<0x1B>...' is not a valid !!int value"), NULL);
  REQUIRE_NE((void *)strstr(e2, "yyy...' is not a valid !!int value"), NULL);
  REQUIRE_LT(strlen(e1), (size_t)160);
  REQUIRE_LT(strlen(e2), (size_t)160);
}

/* Every byte value, in each place where the parser quotes the byte or the
 * text around it. A refusal before the parse names the byte as 0xNN too. */
TEST(error_text, every_byte_in_every_quoting_context_is_printable) {
  static const char *const ctx[][2] = {{"\"\\u", "000\"\n"},
                                       {"\"\\U", "0000000\"\n"},
                                       {"\"\\", "\"\n"},
                                       {"*", "z\n"},
                                       {"[*", "z]\n"},
                                       {"x: !!bool \"", "\"\n"},
                                       {"x: !!int '", "'\n"},
                                       {"x: !!float ", "\n"},
                                       {"x: &a", ", y\n"},
                                       {"- !<x>", ",y\n"},
                                       {"%TAG !", "! tag:x,\n--- x\n"},
                                       {"!a", "!b x\n"},
                                       {"[", "]\n"},
                                       {"x: ", "\n"}};
  size_t bad = 0;
  for (size_t c = 0; c < sizeof(ctx) / sizeof(ctx[0]); c++) {
    for (unsigned v = 1; v < 256; v++) {
      char in[64];
      int n = snprintf(in, sizeof(in), "%s%c%s", ctx[c][0], (char)v, ctx[c][1]);
      char *err = NULL;
      cyaml doc = cyaml_parse_n(in, (size_t)n, &err);
      if (!doc && err && !err_text_is_printable(err)) bad++;
      cyaml_destroy(doc);
    }
  }
  REQUIRE_EQ(bad, (size_t)0);
}

/* The double-quoted writer escapes exactly the bytes that need it and
 * copies every other byte as it is. */
TEST(serialize, double_quoted_escapes_every_special_byte_exactly) {
  static const char in[] =
      "a\x01\x02\x07\x08\t\n\x0b\x0c\r\x1b\x1f\"\\\x7f"
      "\xc2\x80\xc2\x85\xc2\x9f\xc2\xa0\xe2\x80\xa7\xe2\x80\xa8\xe2\x80\xa9"
      "\xe2\x80\xaa\xef\xbf\xbd\xef\xbf\xbe\xef\xbf\xbf\xc3\xa9z";
  static const char want[] =
      "\"a\\x01\\x02\\x07\\b\\t\\n\\x0b\\f\\r\\x1b\\x1f\\\"\\\\\\x7f"
      "\\x80\\N\\x9f\xc2\xa0\xe2\x80\xa7\\L\\P"
      "\xe2\x80\xaa\xef\xbf\xbd\\uFFFE\\uFFFF\xc3\xa9z\"";
  cyaml s = cyaml_create_string(in);
  char *flow = s ? cyaml_serialize_flow(s) : NULL;
  cyaml back = flow ? cyaml_parse(flow, NULL) : NULL;
  bool same = back && cyaml_type(back) == CYAML_STRING &&
              strcmp(cyaml_str_val(back), in) == 0;
  bool exact = flow && strcmp(flow, want) == 0;
  if (!exact) fprintf(stderr, "got: %s\n", flow ? flow : "(null)");
  cyaml_destroy(back);
  cyaml_serialize_free(flow);
  cyaml_destroy(s);
  REQUIRE_TRUE(exact);
  REQUIRE_TRUE(same);
}
