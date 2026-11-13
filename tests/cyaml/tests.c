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
#include <internal/test_helpers/cprocsintern.h>
#include <internal/test_helpers/cyaml.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
// macOS declares nl_langinfo_l() in <xlocale.h>.
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// The forked children report through their wait status, so the binary starts
// with a clean signal state (see test_reset_inherited_signals()).
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

// A budget allocator for the tests that fail every allocation after a given
// point. These let g_alloc_remaining calls through to malloc, calloc and
// realloc, decrementing it on each call, and return NULL once it is 0. -1
// means no limit.
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
static ccol_memmgmt_procs g_counting_mp = {.malloc = counting_malloc,
                                           .calloc = counting_calloc,
                                           .realloc = counting_realloc,
                                           .free = free};

// A tally allocator for the tests that pin how much work a parse does. These
// forward to the C library and count each call in g_tally; they never
// refuse.
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
static ccol_memmgmt_procs g_tally_mp = {.malloc = tally_malloc,
                                        .calloc = tally_calloc,
                                        .realloc = tally_realloc,
                                        .free = free};

// A single-fault allocator for the tests that fail one particular allocation
// and still need later ones (such as the error message) to succeed. These
// forward to the C library, count each call in g_single_call_idx and fail
// only the call whose index is g_single_fail_at. -1 means none fails.
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
static ccol_memmgmt_procs g_single_fault_mp = {.malloc = single_fault_malloc,
                                               .calloc = single_fault_calloc,
                                               .realloc = single_fault_realloc,
                                               .free = free};

// Construction

// The parser moves a string key's buffer into the dictionary instead of
// copying it, which saves one allocation per entry. The bound sits between the
// two counts (457 with the move, 521 with a copy on this document).
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
  if (parsed) cyaml_destroy(parsed);

  REQUIRE_TRUE(parsed_ok);
  REQUIRE_EQ(entries, (size_t)N);
  REQUIRE_LT(allocations, (long)(8 * N - N / 2));
}

// Returns parent's child under key, or NULL when parent isn't a dictionary.
// The BOM tests use it to read nested nodes without aborting the binary in
// ccol__fatal_err when a nesting regression turns a node into a scalar.
static cyaml _cyaml_dict_child(cyaml parent, const char *key) {
  if (!parent || cyaml_type(parent) != CYAML_DICTIONARY) return NULL;
  return cyaml_dictionary_get(parent, key);
}

// Reads the shape of the BOM test document: The size of "root" and the two
// nested integers "leaf" and "deep". The BOM tests compare these between the
// documents with and without a mark. Each out-parameter stays as the caller
// set it when the shape doesn't match.
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

// The same BOM document with the line cache off, the fallback that a failed
// growth of line_starts latches. The backward scan then needs the end of the
// mark as its floor, or every column on the first line is three too large and
// the parse fails with "trailing content".
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

// A byte order mark shifts every offset by three but no column. Columns decide
// block nesting, so the same document with and without a BOM must agree on
// its whole structure.
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

// Copies body with every '\n' replaced by eol and a byte order mark in front
// when bom is true, so the span tests can build one document in every line
// ending. The caller frees the result; NULL when memory runs out.
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

// Parses a matrix of implicit-key documents for each line ending (LF, CRLF,
// bare CR), with and without a BOM, and reports whether every `multi` key
// (spanning two lines) was refused and every `single` one accepted. The span
// tests run it with the line cache on and off, since the parser decides
// "crosses a line" from the line starts it tracks. The bare-CR column catches
// a check that only looks for '\n'.
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

// The same matrix with the line cache off, so the span decisions come from the
// backward scan and must agree with the cached ones.
TEST(cyaml_line_cache, an_implicit_key_span_agrees_when_the_cache_is_off) {
  bool multi_rejected = false, single_accepted = false;
  cyaml_test_force_line_cache_disabled = true;
  _cyaml_check_implicit_key_span_matrix(&multi_rejected, &single_accepted);
  cyaml_test_force_line_cache_disabled = false;
  REQUIRE_TRUE(multi_rejected);
  REQUIRE_TRUE(single_accepted);
}

// A speculative key parse rewinds, and the parser then asks about offsets the
// line cache has already passed, so the cache has to search instead of
// answering from its last entry. Span decisions after such a rewind must come
// out the same as on the way forward.
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

// cyaml_dictionary_get() hands out a borrowed pointer. Setting that same
// pointer again under the same key must not free the node.
TEST(construction, mapping_set_self_assignment_does_not_corrupt_value) {
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
  cyaml m = cyaml_create_dictionary();
  REQUIRE_NE((void *)m, NULL);

  cyaml lst = cyaml_create_list();
  REQUIRE_NE((void *)lst, NULL);
  REQUIRE_EQ(cyaml_list_push(lst, cyaml_create_int(1)), ccol_success);
  REQUIRE_EQ(cyaml_list_push(lst, cyaml_create_int(2)), ccol_success);
  REQUIRE_EQ(cyaml_dictionary_set(m, "k", lst), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(m, "k")), CYAML_LIST);

  REQUIRE_EQ(cyaml_dictionary_set(m, "k", cyaml_create_string("replaced")),
             ccol_success);
  REQUIRE_EQ(cyaml_dictionary_size(m), (size_t)1);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(m, "k")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(m, "k")), "replaced");

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

// Implicit type resolution

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

// The parser hands strtoll() the signed text. Without the sign 2^63 overflows,
// and the value would turn into a lossy float.
TEST(implicit_types, integer_llong_min) {
  char *err = NULL;
  cyaml n = cyaml_parse("-9223372036854775808\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), LLONG_MIN);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_llong_min_round_trip) {
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

TEST(implicit_types, hex_with_a_sign_before_the_prefix_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("-0x1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "-0x1");
  cyaml_destroy(n);
}

TEST(implicit_types, hex_above_the_integer_range_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("0xFFFFFFFFFFFFFFFF\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0xFFFFFFFFFFFFFFFF");
  cyaml_destroy(n);
}

TEST(implicit_types, hex_exactly_two_pow_63_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("0x8000000000000000\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0x8000000000000000");
  cyaml_destroy(n);
}

TEST(implicit_types, negative_hex_two_pow_63_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("-0x8000000000000000\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "-0x8000000000000000");
  cyaml_destroy(n);
}

TEST(implicit_types, octal_above_the_integer_range_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("0o1777777777777777777777\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0o1777777777777777777777");
  cyaml_destroy(n);
}

TEST(implicit_types, negative_octal_above_the_integer_range_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("-0o1777777777777777777777\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "-0o1777777777777777777777");
  cyaml_destroy(n);
}

TEST(implicit_types, hex_with_a_plus_sign_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("+0x10\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "+0x10");
  cyaml_destroy(n);
}

TEST(implicit_types, octal_with_a_sign_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("-0o7\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "-0o7");
  cyaml_destroy(n);
}

TEST(implicit_types, hex_and_octal_at_llong_max_are_integers) {
  char *err = NULL;
  cyaml h = cyaml_parse("0x7FFFFFFFFFFFFFFF\n", &err);
  cyaml o = cyaml_parse("0o777777777777777777777\n", &err);
  int h_ok =
      h && cyaml_type(h) == CYAML_INTEGER && cyaml_int_val(h) == LLONG_MAX;
  int o_ok =
      o && cyaml_type(o) == CYAML_INTEGER && cyaml_int_val(o) == LLONG_MAX;
  cyaml_destroy(h);
  cyaml_destroy(o);
  REQUIRE_TRUE(h_ok);
  REQUIRE_TRUE(o_ok);
}

TEST(tag_typing, int_refuses_a_signed_or_oversized_hex) {
  char *err = NULL;
  cyaml a = cyaml_parse("!!int -0x1\n", &err);
  int a_refused = a == NULL && err != NULL;
  cyaml_destroy(a);
  err = NULL;
  cyaml b = cyaml_parse("!!int 0x8000000000000000\n", &err);
  int b_refused = b == NULL && err != NULL;
  cyaml_destroy(b);
  REQUIRE_TRUE(a_refused);
  REQUIRE_TRUE(b_refused);
}

// A string that looks like a signed or oversized hex or octal literal goes out
// quoted, so YAML 1.1 readers see a string as well, and it reads back as the
// same string, also as a key.
TEST(implicit_types, signed_and_oversized_hex_strings_round_trip_quoted) {
  static const char *const texts[] = {"-0x1", "+0x10", "-0o7",
                                      "0x8000000000000000",
                                      "0o1777777777777777777777"};
  int bad = 0;
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
    cyaml d = cyaml_create_dictionary();
    cyaml_dictionary_set(d, texts[i], cyaml_create_string(texts[i]));
    char *out = cyaml_serialize(d);
    cyaml back = out ? cyaml_parse(out, NULL) : NULL;
    cyaml v = back ? cyaml_dictionary_get(back, texts[i]) : NULL;
    if (!out || (out[0] != '\'' && out[0] != '"')) bad++;
    if (!v || cyaml_type(v) != CYAML_STRING ||
        strcmp(cyaml_str_val(v), texts[i]) != 0)
      bad++;
    cyaml_destroy(back);
    cyaml_serialize_free(out);
    cyaml_destroy(d);
  }
  REQUIRE_EQ(bad, 0);
}

TEST(implicit_types, uppercase_hex_prefix_is_a_string) {
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

// strtod() reads "0X..." as a hex float, so the float path must refuse the
// uppercase prefix too.
TEST(implicit_types, uppercase_hex_prefix_overflow_stays_a_string_not_float) {
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

// strtod() sets ERANGE on this valid subnormal, so only a result clamped to an
// infinity may count as a failure.
TEST(implicit_types, float_subnormal_underflow_is_still_a_float) {
  char *err = NULL;
  cyaml n = cyaml_parse("5e-324\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_GT(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, float_underflow_to_zero_is_still_a_float) {
  char *err = NULL;
  cyaml n = cyaml_parse("1e-400\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, float_overflow_is_an_infinity) {
  static const struct {
    const char *text;
    double want;
  } cases[] = {{"1e400", __builtin_inf()},
               {"1.0e400", __builtin_inf()},
               {"-1e400", -__builtin_inf()},
               {"+1.5E+999", __builtin_inf()},
               {"-.5e400", -__builtin_inf()}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml n = cyaml_parse(cases[i].text, &err);
    bool ok = n && cyaml_type(n) == CYAML_FLOAT &&
              cyaml_double_val(n) == cases[i].want;
    cyaml_destroy(n);
    REQUIRE_TRUE(ok);
  }
}

TEST(implicit_types, float_overflow_with_the_float_tag_is_an_infinity) {
  char *err = NULL;
  cyaml n = cyaml_parse("[!!float 1e400, !!float -1.0e400]", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  bool ok = cyaml_list_len(n) == 2 &&
            cyaml_type(cyaml_list_get(n, 0)) == CYAML_FLOAT &&
            cyaml_double_val(cyaml_list_get(n, 0)) == __builtin_inf() &&
            cyaml_double_val(cyaml_list_get(n, 1)) == -__builtin_inf();
  cyaml_destroy(n);
  REQUIRE_TRUE(ok);
}

TEST(implicit_types, an_overflowing_float_key_is_the_infinity_key) {
  char *err = NULL;
  cyaml n = cyaml_parse("{1e400: a, .inf: b}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  cyaml v = cyaml_dictionary_get(n, ".inf");
  bool ok =
      cyaml_dictionary_size(n) == 1 && v && strcmp(cyaml_str_val(v), "b") == 0;
  cyaml_destroy(n);
  REQUIRE_TRUE(ok);
}

TEST(implicit_types, hex_float_syntax_is_not_a_yaml_float) {
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

TEST(implicit_types, hex_just_above_the_integer_range_is_a_string) {
  char *err = NULL;
  cyaml n = cyaml_parse("0x8000000000000001\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "0x8000000000000001");
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
  char *err = NULL;
  cyaml n = cyaml_parse("trueish\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "trueish");
  cyaml_destroy(n);
}

// C99 nan and inf bare forms are strings (YAML 1.2)

TEST(implicit_types, string_nan_lowercase) {
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

// strtod() accepts "nan(123)", which is no YAML 1.2 float, so the parser
// filters it out before the strtod() fallback.
TEST(implicit_types, string_nan_with_parenthesized_payload) {
  char *err = NULL;
  cyaml n = cyaml_parse("nan(123)\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "nan(123)");
  cyaml_destroy(n);
}

TEST(implicit_types, string_nan_with_empty_parens_and_sign) {
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

// An explicit !!float reaches try_parse_float_scalar() directly, a second way
// into the same guard.
TEST(implicit_types, tagged_float_nan_with_parenthesized_payload_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float nan(3)\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(implicit_types, flow_dict_nan_parenthesized_keys_do_not_collide) {
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

// Quoted scalars

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
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\a\\b\\v\\f\\r\\e\\ \\\"\\/\\N\\_\\L\\P\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n),
                "\a\b\v\f\r\x1B \"/\xC2\x85\xC2\xA0\xE2\x80\xA8\xE2\x80\xA9");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_preserves_null_like) {
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
  char *err = NULL;
  cyaml n = cyaml_parse("'\\n'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\\n");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_hex_escape) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"H\\x65llo\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "Hello");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_surrogate_pair) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\uD83D\\uDE00\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xF0\x9F\x98\x80");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_big_unicode_escape) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\U0001F600\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xF0\x9F\x98\x80");
  cyaml_destroy(n);
}

// A \U escape above U+10FFFF or of a surrogate names no character, so the
// document is refused and no stand-in character is stored.
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
  char *err = NULL;
  cyaml n = cyaml_parse("[\"\\U0010FFFF\", \"\\U0000E000\"]\n", &err);
  bool ok =
      n != NULL && err == NULL &&
      strcmp(cyaml_str_val(cyaml_list_get(n, 0)), "\xF4\x8F\xBF\xBF") == 0 &&
      strcmp(cyaml_str_val(cyaml_list_get(n, 1)), "\xEE\x80\x80") == 0;
  cyaml_destroy(n);
  REQUIRE_TRUE(ok);
}

// A surrogate escape is valid only as the high half of a pair that the next
// \u escape completes. Every other one is a lone surrogate and is refused.
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
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\nline2\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 line2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_multiline_blank_line_preserved) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\n\nline2\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1\nline2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_multiline_leading_whitespace_stripped) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\n   continuation\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 continuation");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_multiline_fold_to_space) {
  char *err = NULL;
  cyaml n = cyaml_parse("'line1\nline2'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 line2");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_multiline_blank_line_preserved) {
  char *err = NULL;
  cyaml n = cyaml_parse("'line1\n\nline2'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1\nline2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_trailing_whitespace_stripped_before_fold) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"hello   \nworld\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello world");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_trailing_whitespace_stripped_before_blank_fold) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"hello   \n\nworld\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello\nworld");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_trailing_whitespace_stripped_before_fold) {
  char *err = NULL;
  cyaml n = cyaml_parse("'hello   \nworld'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "hello world");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escaped_newline_joins_lines) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\\\n   cont\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1cont");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escaped_newline_then_blank_line_joins_with_one_lf) {
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
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\\n\n\nb\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "a\n\nb");
  cyaml_destroy(n);
}

// Block scalars

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
  REQUIRE_STREQ(cyaml_str_val(n), "line one\nline two\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_blank_line_bare_cr_does_not_swallow_next_line) {
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
  const char *yaml = ">\n  x\r  y\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
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
  REQUIRE_STREQ(cyaml_str_val(n), "line one line two\n\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_keep_content_no_trailing_blank) {
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
  char *err = NULL;
  cyaml n = cyaml_parse("|\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_leading_blank) {
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
  char *err = NULL;
  cyaml n = cyaml_parse("|+\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_keep_no_content_trailing_blank) {
  char *err = NULL;
  cyaml n = cyaml_parse(">+\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_chomp_indent_both_orders) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("a: |24\n    x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_duplicate_chomping_indicator_rejected) {
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

// The extra newline for a change into a more-indented run must not fire for
// blank lines before the first content line. The explicit indentation
// indicator makes the first line really more indented than the declared
// indent.
TEST(block_scalars,
     folded_leading_blank_lines_before_more_indented_unaffected) {
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

// Block mappings

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
  char *err = NULL;
  cyaml doc = cyaml_parse("? ~\n: 1\n~: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "null")), 2LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, implicit_bool_and_int_keys_canonicalize) {
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

// The first entry of a mapping and every later one are found by different
// code paths, so both positions are checked.
TEST(block_mapping, implicit_key_canonicalization_applies_to_every_entry) {
  char *err = NULL;
  cyaml doc = cyaml_parse("~: first\nNull: second\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "null")), "second");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_implicit_key_canonicalizes_and_alias_resolves) {
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
  static const char yaml[] = "\xEF\xBB\xBFkey: value\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

// A second key at column 0 shows that the BOM doesn't move the start of the
// first line; otherwise every column there is three too high.
TEST(block_mapping, utf8_bom_skipped_multiline) {
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

// Requires that yaml fails to parse with an error message; the shorthand of
// the tab-indentation tests for a refused document.
static void require_tab_rejected(const char *yaml) {
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Tells whether yaml parses and its flow serialization is exactly want, the
// accepting half of the tab tests. Prints the case when it isn't.
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
  static const char yaml[] = "\xEF\xBB\xBF# c\na: 1\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), 1LL);
  cyaml_destroy(doc);

  static const char only[] = "\xEF\xBB\xBF#";
  doc = cyaml_parse(only, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_NULL);
  cyaml_destroy(doc);
}

TEST(bom_prefix, comment_after_bom_on_value_line) {
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

// Block sequences

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

// Flow collections

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
  char *err = NULL;
  cyaml doc = cyaml_parse("{42: the_answer}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "42")), "the_answer");
  cyaml_destroy(doc);
}

TEST(flow, mapping_non_string_keys) {
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

// Anchors and aliases

TEST(anchors, basic_alias) {
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

// Path navigation (cyaml_get)

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
  REQUIRE_EQ((void *)cyaml_get(doc, ""), (void *)doc);
  cyaml_destroy(doc);
}

// Path mutation (cyaml_set)

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
  char *err = NULL;
  cyaml doc = cyaml_parse("scale: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_set(doc, "scale", 1.5f), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "scale")), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(cyaml_get(doc, "scale")), 1.5);
  cyaml_destroy(doc);
}

// Goes through every width (1, 2, 4, 8) and signedness that cyaml__type_of()
// and cyaml__is_signed() dispatch on.
TEST(path, set_integer_various_widths_and_signs_round_trip) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("nums:\n  - 1\n  - 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "nums.#99", 42), ccol_key_not_found);

  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "nums.#0")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "nums.#1")), 2LL);

  cyaml_destroy(doc);
}

TEST(path, set_on_list_root) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "key.sub", 1), ccol_invalid_args);

  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, set_missing_parent) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "missing.sub", 1), ccol_key_not_found);

  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

// A malformed "#N" index is a caller error at any position in the path, so a
// malformed middle component must not look like an absent one.
TEST(path,
     set_malformed_index_syntax_in_non_leaf_component_returns_invalid_args) {
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "items.#xyz.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(doc, "items")), (size_t)2);

  cyaml_destroy(doc);
}

// strtol() accepts leading blanks and a '+', but the "#N" grammar wants digits
// right after the '#'.
TEST(path, get_index_with_leading_whitespace_is_malformed) {
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ((void *)cyaml_get(doc, "items.#  0.a"), NULL);
  REQUIRE_EQ((void *)cyaml_get(doc, "items.#+0.a"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#0.a")), 1LL);

  cyaml_destroy(doc);
}

TEST(path, set_index_with_leading_whitespace_returns_invalid_args) {
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "items.# 0.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_set(doc, "items.#+0.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#0.a")), 1LL);

  cyaml_destroy(doc);
}

TEST(delete, path_index_with_leading_sign_returns_invalid_args) {
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_delete(doc, "items.#+0"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_delete(doc, "items.# 0"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(doc, "items")), (size_t)2);

  cyaml_destroy(doc);
}

TEST(path, set_wrong_type_in_non_leaf_component_returns_invalid_args) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "a.b.c", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "a")), 1LL);

  cyaml_destroy(doc);
}

TEST(path, set_null_value) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  long double ld = 3.14L;
  REQUIRE_EQ(cyaml_set(doc, "key", ld), ccol_invalid_args);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "key")), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  REQUIRE_EQ(cyaml_set(doc, "brand_new_key", ld), ccol_invalid_args);
  REQUIRE_EQ((void *)cyaml_get(doc, "brand_new_key"), NULL);

  cyaml_destroy(doc);
}

TEST(path, set_string_variable) {
  char *err = NULL;
  cyaml doc = cyaml_parse("env: dev\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  char *val = "prod";
  REQUIRE_EQ(cyaml_set(doc, "env", val), ccol_success);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "env")), "prod");

  cyaml_destroy(doc);
}

TEST(path, set_replaces_container_leaf) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "key.", 1), ccol_invalid_args);
  REQUIRE_EQ((void *)cyaml_get(doc, "key."), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, degenerate_path_consecutive_dots) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a:\n  b: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ((void *)cyaml_get(doc, "a..b"), NULL);

  cyaml_destroy(doc);
}

// Path escape sequences

TEST(path, get_escaped_dot_flat_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a.b\": 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\.b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 42LL);
  REQUIRE_EQ((void *)cyaml_get(doc, "a.b"), NULL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_backslash_flat_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\\\\b\": 7\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\\\b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 7LL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_dot_nested_path) {
  char *err = NULL;
  cyaml doc = cyaml_parse("outer:\n  \"k.ey\": 99\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "outer.k\\.ey");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 99LL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_dot_both_components) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a.b\":\n  \"c.d\": 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\.b.c\\.d");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 1LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_creates_new_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "x\\.y", 55), ccol_success);
  cyaml node = cyaml_get(doc, "x\\.y");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 55LL);
  REQUIRE_EQ((void *)cyaml_get(doc, "x"), NULL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_updates_existing_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"p.q\": 0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "p\\.q", 123), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "p\\.q")), 123LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_nested_path) {
  char *err = NULL;
  cyaml doc = cyaml_parse("outer:\n  \"k.ey\": 0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "outer.k\\.ey", 77), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "outer.k\\.ey")), 77LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_backslash_creates_key) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "a\\\\b", 9), ccol_success);
  REQUIRE_NE((void *)cyaml_get(doc, "a\\\\b"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "a\\\\b")), 9LL);
  cyaml_destroy(doc);
}

TEST(path, set_replaces_container_list_element) {
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
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#1")), 99LL);

  cyaml_destroy(doc);
}

// A one-component path makes two strdup calls. The budget lets the first
// succeed and fails the second, and the document must stay whole. The budget
// is reset before any assertion, so a failure here can't break later tests
// that share g_counting_mp.
TEST(path, set_single_component_oom) {
  g_alloc_remaining = -1;
  char *err = NULL;
  cyaml doc = cyaml_parse_mp("port: 8080\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  g_alloc_remaining = 1;
  long long new_val = 9090LL;
  ccol_retval r = cyaml__set_typed(doc, "port", CYAML_INTEGER, &new_val,
                                   sizeof(new_val), true);
  g_alloc_remaining = -1;
  REQUIRE_EQ(r, ccol_not_enough_memory);

  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

// The delete counterpart of set_single_component_oom.
TEST(path, delete_single_component_oom) {
  g_alloc_remaining = -1;
  char *err = NULL;
  cyaml doc = cyaml_parse_mp("port: 8080\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  g_alloc_remaining = 1;
  ccol_retval r = cyaml__delete(doc, "port");
  g_alloc_remaining = -1;
  REQUIRE_EQ(r, ccol_not_enough_memory);

  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

// Deep copy

// Writes the keys of dict in iteration order, joined by ',', into out (cap
// bytes), so the order tests can compare a whole order with one string.
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
  cyaml d = cyaml_parse("{a: 1, b: 2, c: 3, d: 4, e: 5}", NULL);
  char order[8] = {0};
  size_t visited = 0;
  long long seen_d = 0;
  ccol_retval r1 = ccol_success, r2 = ccol_success, r3 = ccol_success;
  ccol_retval r4 = ccol_success;
  bool value_is_same_node_after_set = false;
  cyaml_dictionary_iter it;
  for (bool ok = cyaml_dictionary_first(d, &it); ok;
       ok = cyaml_dictionary_next(&it)) {
    if (visited < sizeof(order) - 1) order[visited] = it.key[0];
    visited++;
    if (strcmp(it.key, "a") == 0) {
      r1 = cyaml_dictionary_set(d, "a", cyaml_create_int(10));
    } else if (strcmp(it.key, "b") == 0) {
      cyaml before = it.value;
      r2 = cyaml_set(d, "b", 20);
      value_is_same_node_after_set = cyaml_dictionary_get(d, "b") == before;
      r3 = cyaml_dictionary_set(d, "d", cyaml_create_int(40));
    } else if (strcmp(it.key, "c") == 0) {
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

// It checks the change on the original as well, so a set that does nothing and
// a shallow clone both fail.
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
  g_alloc_remaining = -1;
  cyaml src = cyaml_create_string_mp("hello", &g_counting_mp);
  REQUIRE_NE((void *)src, NULL);

  g_alloc_remaining = 0;
  cyaml copy = cyaml_clone(src);
  g_alloc_remaining = -1;
  REQUIRE_EQ((void *)copy, NULL);

  REQUIRE_STREQ(cyaml_str_val(src), "hello");
  cyaml_destroy(src);
}

// Fails an allocation at every budget and requires that a clone, when one
// comes back, never holds fewer entries than the source. Each clone is freed
// before its assertion, so valgrind sees no leak of the test's own.
TEST(clone, dictionary_iterator_oom_never_returns_incomplete_clone) {
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
      size_t copy_size = cyaml_dictionary_size(copy);
      cyaml_destroy(copy);
      REQUIRE_EQ(copy_size, src_size);
    }
  }

  g_alloc_remaining = -1;
  REQUIRE_EQ(cyaml_dictionary_size(src), src_size);
  cyaml_destroy(src);
}

// A tree built through the API has no depth bound, so cyaml_clone() applies
// the cap itself and returns NULL for a chain of 1000 levels.
TEST(clone, exceeding_max_depth_returns_null_not_crashed) {
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

// Fails an allocation at every budget across documents that build
// dictionaries entry by entry, so a later failure tears down a dictionary
// that already holds entries. There is no leak checker in tau; make memtest
// catches a leak here.
TEST(oom, parsing_a_dictionary_never_leaks_under_sustained_allocation_failure) {
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
      if (doc) cyaml_destroy(doc);
    }
  }
}

// An alias key and an anchored flow key can each fail only for lack of memory.
// Such a failure must never look like "not a key" and end the dictionary
// early, so a result that comes back must be the complete document.
TEST(
    oom,
    dictionary_entry_key_resolution_never_returns_a_truncated_document_under_sustained_allocation_failure) {
  const char *docs[] = {
      "x: &k v\na: 1\n*k: 2\n",
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
        size_t actual_size = cyaml_dictionary_size(doc);
        cyaml_destroy(doc);
        REQUIRE_EQ(actual_size, expected_sizes[d]);
      } else {
      }
    }
  }
}

// Every scalar branch of node_to_dict_key_string() reports its own message on
// a failed strdup. The single-fault allocator leaves room for that message.
// Only the existence of the specific message is checked, not what every other
// index reports.
TEST(oom, scalar_dictionary_key_oom_reports_specific_message) {
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

// A failed allocation in the speculative anchor or tag scan of a key, or in
// anchors_store(), must be reported. Returning "not a key" there would leave
// the position moved, so every failed allocation must fail the whole parse.
TEST(oom, anchor_or_tag_decorated_key_oom_never_silently_succeeds) {
  const char *doc_src = "&a key: value\n";

  g_single_call_idx = 0;
  g_single_fail_at = -1;
  char *baseline_err = NULL;
  cyaml baseline = cyaml_parse_mp(doc_src, &baseline_err, &g_single_fault_mp);
  int total_calls = g_single_call_idx;
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
    bool doc_was_null = (doc == NULL);
    if (doc) cyaml_destroy(doc);
    REQUIRE_TRUE(doc_was_null);
  }
}

// Each failure gives either a failed parse with nothing leaked (memtest checks
// that) or exactly the tree of an undisturbed parse.
TEST(oom, every_allocation_failure_of_several_merge_keys_fails_cleanly) {
  static const char *const docs[] = {
      ("b: &b {x: 1, y: 2}\nc: &c {y: 3, z: 4}\n"
       "m:\n  p: 0\n  <<: *b\n  q: 1\n  <<: [*c, {w: 5}]\n  r: 2\n  x: 9\n"),
      "{a: 0, <<: {k: 1}, \"<<\": 5, <<: {k: 2, j: 3}, z: 9}\n",
      "- [<<: {a: 1}]\n- {<<: {a: 1}, <<: {b: 2}, <<: {c: 3}, <<: {d: 4}}\n",
  };
  for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); d++) {
    g_single_call_idx = 0;
    g_single_fail_at = -1;
    char *err = NULL;
    cyaml baseline = cyaml_parse_mp(docs[d], &err, &g_single_fault_mp);
    int total_calls = g_single_call_idx;
    char *want = baseline ? cyaml_serialize_flow(baseline) : NULL;
    if (baseline) cyaml_destroy(baseline);
    bool all_ok = want != NULL && total_calls > 0;
    for (int fail_at = 0; all_ok && fail_at < total_calls; fail_at++) {
      g_single_call_idx = 0;
      g_single_fail_at = fail_at;
      err = NULL;
      cyaml doc = cyaml_parse_mp(docs[d], &err, &g_single_fault_mp);
      g_single_fail_at = -1;
      if (doc) {
        char *got = cyaml_serialize_flow(doc);
        all_ok = got && strcmp(got, want) == 0;
        if (got) cyaml_serialize_free_mp(got, &g_single_fault_mp);
        cyaml_destroy(doc);
      } else {
        all_ok = err != NULL;
      }
    }
    if (want) cyaml_serialize_free_mp(want, &g_single_fault_mp);
    REQUIRE_TRUE(all_ok);
  }
}

// A failed allocation deep in a shared DOM helper reaches the parser's
// fallback with no message of its own, and that fallback must name the memory
// failure instead of "unknown parse error".
TEST(oom, unreported_allocation_failure_reports_honest_fallback_not_vague_one) {
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
        bool is_specific = strcmp(err, "unknown parse error") != 0;
        if (strstr(err, "out of memory") != NULL)
          saw_specific_oom_message = true;
        REQUIRE_TRUE(is_specific);
      }
    }
  }
  REQUIRE_TRUE(saw_specific_oom_message);
}

// A continuation line is scanned straight into the shared buffer, and growing
// it can fail. That must end in a clean parse failure, never a crash;
// valgrind and a plain run both catch the crash.
TEST(
    oom,
    plain_scalar_multiline_continuation_never_crashes_under_allocation_failure) {
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
      if (doc) cyaml_destroy(doc);
    }
  }
}

// Serialization

// The parse error message lives in library storage, the same rule as for every
// err and err_str out-parameter of the library (see common.h): The caller never
// frees it, and a failed allocation can't make it disappear.
TEST(parse_error_string, is_library_storage_and_survives_without_a_free) {
  char *err = NULL;
  cyaml bad = cyaml_parse("a: [1, 2", &err);
  REQUIRE_EQ((void *)bad, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);
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

// The same failing parse runs with and without asking for the message, and
// both runs must make the same number of allocations.
TEST(parse_error_string, a_custom_allocator_never_sees_it) {
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

// PyYAML, Psych and yaml.v2 read a plain "yes" or "NO" as a boolean, so the
// serializer quotes every YAML 1.1 boolean spelling even though this module
// reads them as strings.
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

// Tells whether w is one of the spellings that a parse would read as another
// type. The exact-list test uses it to check that a word one byte longer or
// shorter than a listed one is quoted only when it is on the list itself.
static bool cyaml_test_is_ambiguous_word(const char *w) {
  static const char *const list[] = {
      "~",     "null",  "Null",  "NULL", "true", "True",  "TRUE",  "false",
      "False", "FALSE", ".inf",  ".Inf", ".INF", "+.inf", "+.Inf", "+.INF",
      "-.inf", "-.Inf", "-.INF", ".nan", ".NaN", ".NAN",  "<<",    "y",
      "Y",     "yes",   "Yes",   "YES",  "n",    "N",     "no",    "No",
      "NO",    "on",    "On",    "ON",   "off",  "Off",   "OFF",   "="};
  for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++)
    if (strcmp(w, list[i]) == 0) return true;
  return false;
}

// Tells whether a string node holding w serializes in quotes, which the
// quoting tests ask for every word of their lists.
static bool cyaml_test_serializes_quoted(const char *w) {
  cyaml n = cyaml_create_string(w);
  if (!n) return false;
  char *s = cyaml_serialize(n);
  bool quoted = s && (s[0] == '"' || s[0] == '\'');
  cyaml_serialize_free(s);
  cyaml_destroy(n);
  return quoted;
}

TEST(serialize, the_ambiguous_word_list_is_exact) {
  static const char *const words[] = {
      "~",     "null",  "Null",  "NULL", "true", "True",  "TRUE",  "false",
      "False", "FALSE", ".inf",  ".Inf", ".INF", "+.inf", "+.Inf", "+.INF",
      "-.inf", "-.Inf", "-.INF", ".nan", ".NaN", ".NAN",  "<<",    "y",
      "Y",     "yes",   "Yes",   "YES",  "n",    "N",     "no",    "No",
      "NO",    "on",    "On",    "ON",   "off",  "Off",   "OFF",   "="};
  for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
    REQUIRE_TRUE(cyaml_test_serializes_quoted(words[i]));
    char longer[16];
    snprintf(longer, sizeof(longer), "%sz", words[i]);
    char c0 = words[i][0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z'))) continue;
    REQUIRE_FALSE(cyaml_test_serializes_quoted(longer));
    size_t len = strlen(words[i]);
    if (len > 1) {
      char shorter[16];
      memcpy(shorter, words[i], len - 1);
      shorter[len - 1] = '\0';
      REQUIRE_EQ((int)cyaml_test_serializes_quoted(shorter),
                 (int)cyaml_test_is_ambiguous_word(shorter));
    }
  }
}

// Every byte that makes a plain scalar unsafe in the middle of a string forces
// quotes, while the same byte in a harmless position leaves it plain. Each
// string reads back as the same string.
TEST(serialize, a_byte_in_the_middle_that_needs_quotes_is_found) {
  static const char *const quoted[] = {"a #b",
                                       "a\t#b",
                                       "a: b",
                                       "a:",
                                       "a:,b",
                                       "a:]b",
                                       "a:}b",
                                       "a?b",
                                       "a,b",
                                       "a[b",
                                       "a]b",
                                       "a{b",
                                       "a}b",
                                       "a\nb",
                                       "a\rb",
                                       "a\x7f"
                                       "b",
                                       "a\x01"
                                       "b",
                                       "a\xc2\x85"
                                       "b",
                                       "a\xc2\x9f"
                                       "b",
                                       "a\xe2\x80\xa8"
                                       "b",
                                       "a\xe2\x80\xa9"
                                       "b",
                                       "a\xef\xbf\xbe"
                                       "b",
                                       "a\xef\xbf\xbf"
                                       "b",
                                       "ab "};
  static const char *const plain[] = {"a#b",
                                      "a:b",
                                      "a:b:c",
                                      "a\xc2\xa0"
                                      "b",
                                      "a\xe2\x80\xa0"
                                      "b",
                                      "a\xef\xbb\xbf"
                                      "b",
                                      "a b"};
  for (size_t i = 0; i < sizeof(quoted) / sizeof(quoted[0]); i++) {
    cyaml n = cyaml_create_string(quoted[i]);
    REQUIRE_NE((void *)n, NULL);
    char *out = cyaml_serialize(n);
    REQUIRE_NE((void *)out, NULL);
    bool is_quoted = out[0] == '"' || out[0] == '\'';
    char *err = NULL;
    cyaml back = cyaml_parse(out, &err);
    bool same = back && cyaml_type(back) == CYAML_STRING &&
                strcmp(cyaml_str_val(back), quoted[i]) == 0;
    cyaml_destroy(back);
    cyaml_serialize_free(out);
    cyaml_destroy(n);
    REQUIRE_TRUE(is_quoted);
    REQUIRE_TRUE(same);
  }
  for (int c = 0x01; c <= 0x7F; c = (c == 0x1F) ? 0x7F : c + 1) {
    char w[4] = {'a', (char)c, 'b', '\0'};
    cyaml n = cyaml_create_string(w);
    REQUIRE_NE((void *)n, NULL);
    char *out = cyaml_serialize(n);
    REQUIRE_NE((void *)out, NULL);
    bool is_quoted = out[0] == '"' || out[0] == '\'';
    cyaml_serialize_free(out);
    cyaml_destroy(n);
    REQUIRE_TRUE(is_quoted);
  }
  for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
    cyaml n = cyaml_create_string(plain[i]);
    REQUIRE_NE((void *)n, NULL);
    char *out = cyaml_serialize(n);
    REQUIRE_NE((void *)out, NULL);
    bool is_quoted = out[0] == '"' || out[0] == '\'';
    char *err = NULL;
    cyaml back = cyaml_parse(out, &err);
    bool same = back && cyaml_type(back) == CYAML_STRING &&
                strcmp(cyaml_str_val(back), plain[i]) == 0;
    cyaml_destroy(back);
    cyaml_serialize_free(out);
    cyaml_destroy(n);
    REQUIRE_FALSE(is_quoted);
    REQUIRE_TRUE(same);
  }
}

TEST(serialize, yaml_1_1_boolean_spellings_round_trip_as_strings) {
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
  char *s = cyaml_serialize(NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "~\n");
  cyaml_serialize_free(s);
}

TEST(serialize, null_handle_flow) {
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

// "%.15g" writes 5.0 as "5", which would parse as an integer, so the
// serializer appends ".0".
TEST(serialize, float_whole_number_gets_dot_zero_suffix) {
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
  REQUIRE_NE(strstr(s, "null"), NULL);
  REQUIRE_NE(s[0], 'n');
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, quoted_string_null_variants_round_trip) {
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

// The canonical text of a dictionary key sorts its keys, and that sort must
// not leak into the public serializers, which keep insertion order.
TEST(serialize, multi_key_flow_mapping_preserves_iteration_order) {
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

// Fails an allocation at every budget and requires that a serialization that
// comes back holds every key, and that at least one budget succeeds, so a
// serializer that always returns NULL doesn't pass.
TEST(
    serialize,
    dictionary_never_serializes_incomplete_under_sustained_allocation_failure) {
  char *err = NULL;
  cyaml src =
      cyaml_parse_mp("a: 1\nb: 2\nc: 3\nd: 4\ne: 5\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)src, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(src), (size_t)5);

  bool saw_block_success = false;
  bool saw_flow_success = false;

  for (int budget = 0; budget < 300; budget++) {
    g_alloc_remaining = budget;
    char *block = cyaml_serialize(src);
    g_alloc_remaining = -1;
    if (block) {
      saw_block_success = true;
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

// Scoped lifecycle

TEST(lifecycle, scoped_destroy) {
  cyaml out = NULL;
  {
    cyaml_declare_scoped(doc);
    doc = cyaml_parse("key: 42\n", NULL);
    REQUIRE_NE((void *)doc, NULL);
    out = cyaml_clone(doc);
  }
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(out, "key")), 42LL);
  cyaml_destroy(out);
}

TEST(lifecycle, declare_macro) {
  cyaml_declare(n);
  n = cyaml_create_int(42);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 42LL);
  cyaml_destroy(n);
  REQUIRE_EQ((void *)n, NULL);
}

// The fifth push needs a realloc, which the empty budget refuses, and the
// push then destroys the child and reports the failure.
TEST(lifecycle, list_push_oom) {
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
  ccol_retval r = cyaml_list_push(list, victim);
  g_alloc_remaining = -1;
  REQUIRE_NE(r, ccol_success);

  REQUIRE_EQ(cyaml_list_len(list), (size_t)4);
  cyaml_destroy(list);
}

TEST(lifecycle, dictionary_set_oom) {
  g_alloc_remaining = -1;
  cyaml doc = cyaml_create_dictionary_mp(&g_counting_mp);
  REQUIRE_NE((void *)doc, NULL);

  g_alloc_remaining = 0;
  cyaml val = cyaml_create_int(42);
  REQUIRE_NE((void *)val, NULL);
  ccol_retval r = cyaml_dictionary_set(doc, "key", val);
  g_alloc_remaining = -1;
  REQUIRE_NE(r, ccol_success);

  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)0);
  cyaml_destroy(doc);
}

// A node that already has a parent is refused and left alone. Accepting it
// would give it two parents and a double free when both are destroyed.
TEST(lifecycle, list_push_refuses_an_already_attached_child) {
  cyaml first = cyaml_create_list();
  cyaml second = cyaml_create_list();
  cyaml child = cyaml_create_string("v");
  REQUIRE_NE((void *)first, NULL);
  REQUIRE_NE((void *)second, NULL);
  REQUIRE_NE((void *)child, NULL);
  REQUIRE_EQ(cyaml_list_push(first, child), ccol_success);

  ccol_retval r = cyaml_list_push(second, child);
  size_t first_size = cyaml_list_len(first);
  size_t second_size = cyaml_list_len(second);

  cyaml_destroy(second);
  cyaml_destroy(first);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(first_size, (size_t)1);
  REQUIRE_EQ(second_size, (size_t)0);
}

TEST(lifecycle, dictionary_set_refuses_an_already_attached_child) {
  cyaml owner = cyaml_create_dictionary();
  cyaml other = cyaml_create_dictionary();
  cyaml val = cyaml_create_int(42);
  REQUIRE_NE((void *)owner, NULL);
  REQUIRE_NE((void *)other, NULL);
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(owner, "k", val), ccol_success);

  cyaml borrowed = cyaml_dictionary_get(owner, "k");
  ccol_retval r = cyaml_dictionary_set(other, "copy", borrowed);
  size_t other_size = cyaml_dictionary_size(other);
  cyaml still_there = cyaml_dictionary_get(owner, "k");
  bool original_intact = (still_there == borrowed);

  cyaml_destroy(other);
  cyaml_destroy(owner);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(other_size, (size_t)0);
  REQUIRE_TRUE(original_intact);
}

TEST(lifecycle, dictionary_set_accepts_a_key_set_to_its_own_current_value) {
  cyaml doc = cyaml_create_dictionary();
  cyaml val = cyaml_create_int(7);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_EQ(cyaml_dictionary_set(doc, "k", val), ccol_success);

  cyaml borrowed = cyaml_dictionary_get(doc, "k");
  ccol_retval r = cyaml_dictionary_set(doc, "k", borrowed);
  cyaml after = cyaml_dictionary_get(doc, "k");
  bool same_node = (after == borrowed);
  long long v = after ? cyaml_int_val(after) : -1;

  cyaml_destroy(doc);

  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(same_node);
  REQUIRE_EQ(v, 7LL);
}

TEST(lifecycle, a_container_refuses_to_be_attached_to_itself) {
  cyaml seq = cyaml_create_list();
  cyaml map = cyaml_create_dictionary();
  REQUIRE_NE((void *)seq, NULL);
  REQUIRE_NE((void *)map, NULL);

  ccol_retval rs = cyaml_list_push(seq, seq);
  ccol_retval rm = cyaml_dictionary_set(map, "self", map);
  size_t seq_size = cyaml_list_len(seq);
  size_t map_size = cyaml_dictionary_size(map);

  cyaml_destroy(seq);
  cyaml_destroy(map);

  REQUIRE_EQ(rs, ccol_invalid_args);
  REQUIRE_EQ(rm, ccol_invalid_args);
  REQUIRE_EQ(seq_size, (size_t)0);
  REQUIRE_EQ(map_size, (size_t)0);
}

// A bad container destroys a child the caller owns, but never a child that
// belongs to another tree.
TEST(lifecycle, a_rejected_attach_into_a_bad_container_spares_an_owned_child) {
  cyaml owner = cyaml_create_list();
  cyaml scalar = cyaml_create_string("not a container");
  cyaml child = cyaml_create_string("v");
  REQUIRE_NE((void *)owner, NULL);
  REQUIRE_NE((void *)scalar, NULL);
  REQUIRE_NE((void *)child, NULL);
  REQUIRE_EQ(cyaml_list_push(owner, child), ccol_success);

  ccol_retval r = cyaml_list_push(scalar, child);
  ccol_retval r2 = cyaml_list_push(NULL, child);
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
  cyaml doc = cyaml_create_dictionary();
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_remove(NULL, "key"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_dictionary_remove(doc, NULL), ccol_invalid_args);
  cyaml_destroy(doc);

  REQUIRE_EQ(cyaml_list_remove(NULL, 0), ccol_invalid_args);
}

// Complex and real-world documents

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

// Merge keys inside a realistic document, with an anchored mapping followed by
// a sibling key at the same indentation, which the anchor must not swallow.
TEST(real_world, ansible_like_with_anchors) {
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

  cyaml task0 = cyaml_list_get(tasks, 0);
  REQUIRE_NE((void *)task0, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(task0, "name")),
                "fetch data");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task0, "timeout")), 30LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task0, "max_retries")), 3LL);

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

// Error handling

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
  char *err = NULL;
  cyaml doc = cyaml_parse_n(NULL, 0, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// A finished flow list element followed by an invalid continuation line must
// be freed on the error path together with the list.
TEST(errors, flow_list_element_error_cleanup_no_leak) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: [1\n\tb]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);

  err = NULL;
  doc = cyaml_parse("a: [1\n\t]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// A chain of "? " keys on one line makes the canonical key text grow as
// 2^depth. The canonical key length limit refuses it at a shallow depth. The
// clock bound catches a slow parse; a real hang is left to SUITE_TIMEOUT.
TEST(errors, deeply_nested_explicit_keys_rejected_not_hung) {
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

// "?" lines at the same column are siblings whether a '\n' or a bare '\r'
// ends them. A column scan that only knows '\n' would make them nest, and
// the value type of the one entry tells the two parses apart.
TEST(errors,
     repeated_explicit_key_lines_separated_by_bare_cr_flatten_not_nest) {
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

// A plain deep chain of block sequences, which only the nesting depth limit
// bounds.
TEST(errors, excessive_parse_nesting_depth_rejected_not_crashed) {
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

// A "billion laughs" document of nested aliases must hit the node budget
// (CYAML_MAX_PARSE_NODES) and fail quickly. The CPU bound is generous because
// the guard trips only after building that many clones (seconds under
// valgrind).
TEST(errors, exponential_alias_expansion_rejected_not_exhausted) {
  const int levels = 30;
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
  REQUIRE_LT(elapsed_s, 300.0);
  free(input);
}

// Builds "- []\n" repeated n times, about 48 DOM bytes per input byte, for
// the tests of the scaled byte limit. The caller frees it.
static char *_empty_lists_doc(size_t n) {
  char *d = malloc(n * 5 + 1);
  if (!d) return NULL;
  for (size_t i = 0; i < n; i++) memcpy(d + i * 5, "- []\n", 5);
  d[n * 5] = '\0';
  return d;
}

TEST(parse_budget, byte_limit_scales_with_the_input) {
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

// "[:,:,:]" charges the most per input byte of any document without aliases
// or collection keys. With both floors far below its cost, only the scaled
// limits decide, and the charges must stay at least 1.5 times below each
// factor.
TEST(parse_budget, worst_ordinary_shape_parses_above_the_floors) {
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
  REQUIRE_LE(nodes * 3, len * 4 * 2);
  REQUIRE_LE(bytes * 3, len * 512 * 2);
  REQUIRE_GT(nodes, (size_t)1000);
  REQUIRE_GT(bytes, (size_t)1024 * 1024);
}

TEST(parse_budget, node_limit_scales_with_the_input) {
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
  REQUIRE_LT(elapsed_s, 300.0);
}

// Builds a nested-alias document (each level holds eight aliases of the one
// below) with a comment of pad bytes in front, for the tests that padding
// buys no alias expansion. Stores the length in *len_out; the caller frees
// the text.
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

// What aliases copy is charged to a separate budget with the fixed floors, so
// padding a nested-alias document with a long comment buys no more
// expansion.
TEST(parse_budget, alias_expansion_limit_does_not_grow_with_padding) {
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
    if (err &&
        strstr(err, pads[i] ? "alias expansion memory limit" : "memory limit"))
      named++;
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
  char *err = NULL;
  cyaml doc = cyaml_parse("key: value\nextra junk !!!\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// Block sequence value on the next line

TEST(block_sequence, value_on_next_line_mapping) {
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

// Mapping key starting with '-'

TEST(block_mapping, second_key_starting_with_dash) {
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

// A repeated key keeps the last value and frees the earlier one; this is
// implementation-defined behaviour (see cyaml.h).
TEST(block_mapping, duplicate_key_last_wins) {
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

// Float-special string round trips

TEST(serialize, float_special_strings_are_quoted) {
  const char *specials[] = {"+.inf", "+.Inf", "+.INF", "-.inf", "-.Inf",
                            "-.INF", ".inf",  ".Inf",  ".INF",  ".nan",
                            ".NaN",  ".NAN",  NULL};
  for (int i = 0; specials[i]; i++) {
    cyaml n = cyaml_create_string(specials[i]);
    REQUIRE_NE((void *)n, NULL);
    char *s = cyaml_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    REQUIRE_NE(s[0], specials[i][0]);
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

// Quoting of tabs and white space

TEST(serialize, string_with_colon_tab_round_trip) {
  cyaml n = cyaml_create_string("proto:\thttp");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
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
  cyaml n = cyaml_create_string("+extra");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
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

// The parser reads "+.1e-400" and its overflowing and subnormal relatives as
// floats, so the serializer quotes them like every other "+.N" text.
TEST(serialize, plus_dot_underflowing_float_strings_are_quoted) {
  const char *cases[] = {"+.1e-400", "+.999e-320", "+.1e400", NULL};
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

// A tree built through the API has no depth bound, so each serializer applies
// the cap itself and returns NULL for a chain of 1000 levels.
TEST(serialize, excessive_depth_rejected_not_crashed) {
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

// Once the depth guard or a failed allocation sets the out-of-memory flag, the
// whole walk must stop. The white-box counter of refused visits is exactly 1
// for any number of deep branches; a walk that kept going would count one per
// branch.
TEST(serialize, oom_short_circuits_remaining_siblings_after_depth_exceeded) {
  size_t branch_count = 3000;
  size_t chain_depth = 520;

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

  cyaml__serialize_oom_visits_for_tests = 0;
  char *block = cyaml_serialize(top);
  size_t block_visits = cyaml__serialize_oom_visits_for_tests;
  cyaml__serialize_oom_visits_for_tests = 0;
  char *flow = cyaml_serialize_flow(top);
  size_t flow_visits = cyaml__serialize_oom_visits_for_tests;
  cyaml_serialize_free(block);
  cyaml_serialize_free(flow);
  cyaml_destroy(top);

  REQUIRE_EQ((void *)block, NULL);
  REQUIRE_EQ((void *)flow, NULL);
  REQUIRE_EQ(block_visits, (size_t)1);
  REQUIRE_EQ(flow_visits, (size_t)1);
}

TEST(block_mapping, document_start_marker_no_separator) {
  char *err = NULL;
  cyaml doc = cyaml_parse("---42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---42");
  cyaml_destroy(doc);
}

// Bounded parse (cyaml_parse_n)

TEST(parse_n, basic_bounded_parse) {
  const char *buf = "key: value\ntrailing garbage";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(buf, 11, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(parse_n, not_null_terminated) {
  char buf[16];
  memcpy(buf, "42", 2);
  char *err = NULL;
  cyaml doc = cyaml_parse_n(buf, 2, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(parse_n, sequence_bounded) {
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

// Delete

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

TEST(path, a_list_index_too_large_for_any_integer_is_absent) {
  static const char *const paths[] = {
      "items.#4294967296", "items.#9223372036854775808",
      "items.#18446744073709551615", "items.#18446744073709551616",
      "items.#99999999999999999999999999999999999999"};
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - 1\n  - 2\n", &err);
  REQUIRE_NE((void *)root, NULL);
  bool ok = true;
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    ok = ok && cyaml_get(root, paths[i]) == NULL;
    ok = ok && cyaml_set(root, paths[i], 5) == ccol_key_not_found;
    ok = ok && cyaml_delete(root, paths[i]) == ccol_key_not_found;
  }
  ok = ok && cyaml_set(root, "items.#18446744073709551616.x", 5) ==
                 ccol_key_not_found;
  ok = ok && cyaml_list_len(cyaml_dictionary_get(root, "items")) == 2;
  cyaml_destroy(root);
  REQUIRE_TRUE(ok);
}

// For cyaml_delete() an out-of-range "#N" is an absent component
// (ccol_key_not_found), while cyaml_list_remove() called directly refuses the
// same index with ccol_invalid_args.
TEST(delete, path_out_of_range_list_index_returns_key_not_found) {
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - 1\n  - 2\n  - 3\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "items.#10"), ccol_key_not_found);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(root, "items")), (size_t)3);
  cyaml_destroy(root);
}

// A malformed "#N" in the middle of a path is a caller error, not an absent
// component.
TEST(delete,
     path_malformed_index_syntax_in_non_leaf_component_returns_invalid_args) {
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "items.#xyz.a"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(root, "items")), (size_t)2);
  cyaml_destroy(root);
}

TEST(delete, path_wrong_type_in_non_leaf_component_returns_invalid_args) {
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a.b.c"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(root, "a")), 1LL);
  cyaml_destroy(root);
}

TEST(delete, path_delete_with_escaped_dot_in_key) {
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
  char *err = NULL;
  cyaml root = cyaml_parse("key: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, ""), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "key"), NULL);
  cyaml_destroy(root);
}

TEST(delete, degenerate_path_trailing_dot) {
  char *err = NULL;
  cyaml root = cyaml_parse("key: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "key."), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "key"), NULL);
  cyaml_destroy(root);
}

TEST(delete, degenerate_path_consecutive_dots) {
  char *err = NULL;
  cyaml root = cyaml_parse("a:\n  b: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a..b"), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "a.b"), NULL);
  cyaml_destroy(root);
}

TEST(delete, an_empty_component_after_an_absent_key_is_invalid) {
  char *err = NULL;
  cyaml root = cyaml_parse("a:\n  b: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);
  static const char *const paths[] = {"x.", "x..y", "x.y.", "a.x..y"};
  ccol_retval set_rv[4], del_rv[4];
  for (int i = 0; i < 4; i++) {
    set_rv[i] = cyaml_set(root, paths[i], 1);
    del_rv[i] = cyaml_delete(root, paths[i]);
  }
  bool intact = cyaml_get(root, "a.b") != NULL && cyaml_get(root, "x") == NULL;
  cyaml_destroy(root);
  for (int i = 0; i < 4; i++) {
    REQUIRE_EQ(set_rv[i], ccol_invalid_args);
    REQUIRE_EQ(del_rv[i], ccol_invalid_args);
  }
  REQUIRE_TRUE(intact);
}

// Multi-document parsing

TEST(multi_document, a_marker_after_a_lone_property_ends_the_document) {
  char *err = NULL;
  cyaml a = cyaml_parse("--- !!str\n...\n", &err);
  bool a_ok =
      a && cyaml_type(a) == CYAML_STRING && strcmp(cyaml_str_val(a), "") == 0;
  cyaml_destroy(a);
  cyaml b = cyaml_parse("--- &x\n...\n", &err);
  bool b_ok = b && cyaml_type(b) == CYAML_NULL;
  cyaml_destroy(b);
  cyaml c = cyaml_parse("!!str\n---\nb\n", &err);
  bool c_ok = c && cyaml_type(c) == CYAML_LIST && cyaml_list_len(c) == 2 &&
              cyaml_type(cyaml_list_get(c, 0)) == CYAML_STRING &&
              strcmp(cyaml_str_val(cyaml_list_get(c, 0)), "") == 0 &&
              strcmp(cyaml_str_val(cyaml_list_get(c, 1)), "b") == 0;
  cyaml_destroy(c);
  cyaml d = cyaml_parse("a: 1\n--- !!str\n---\nz\n", &err);
  bool d_ok = d && cyaml_type(d) == CYAML_LIST && cyaml_list_len(d) == 3 &&
              strcmp(cyaml_str_val(cyaml_list_get(d, 1)), "") == 0 &&
              strcmp(cyaml_str_val(cyaml_list_get(d, 2)), "z") == 0;
  cyaml_destroy(d);
  REQUIRE_TRUE(a_ok);
  REQUIRE_TRUE(b_ok);
  REQUIRE_TRUE(c_ok);
  REQUIRE_TRUE(d_ok);
}

TEST(multi_document, two_scalars_with_markers) {
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
  char *err = NULL;
  cyaml root = cyaml_parse("---\na: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(root, "a")), 1LL);
  cyaml_destroy(root);
}

TEST(multi_document, anchors_do_not_leak_across_documents) {
  char *err = NULL;
  cyaml root = cyaml_parse("---\nval: &anchor 42\n---\nref: *anchor\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)root, NULL);
}

TEST(multi_document, doc_start_marker_in_mapping_value_is_data) {
  char *err = NULL;
  cyaml root = cyaml_parse("key: ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(root, "key")), "---");
  cyaml_destroy(root);
}

TEST(multi_document, triple_doc_start_in_sequence_value_is_data) {
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

// A run of "..." markers is one separator, so the extra ones add no empty
// document (PyYAML agrees).
TEST(multi_document,
     redundant_consecutive_end_markers_do_not_fabricate_a_document) {
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
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\nsome garbage\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)root, NULL);
}

TEST(errors, duplicate_yaml_directive_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n%YAML 1.2\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, content_after_end_marker_on_same_line_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("---\nkey: value\n... invalid\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, hash_glued_directly_onto_start_marker_is_plain_scalar) {
  char *err = NULL;
  cyaml doc = cyaml_parse("---#x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---#x");
  cyaml_destroy(doc);
}

TEST(errors, hash_glued_directly_onto_end_marker_is_plain_scalar) {
  char *err = NULL;
  cyaml doc = cyaml_parse("...#x\nfoo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "...#x foo");
  cyaml_destroy(doc);
}

TEST(errors, hash_after_real_whitespace_on_start_marker_still_a_comment) {
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
  char *err = NULL;
  cyaml root = cyaml_parse_n("ignored", 0, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_NULL);
  cyaml_destroy(root);
}

// Flow serialization round trips

TEST(flow_serialize, comma_in_string_round_trip) {
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

// YAML directives and tags

TEST(directives, yaml_directive_ignored) {
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, tag_directive_ignored) {
  char *err = NULL;
  cyaml doc = cyaml_parse("%TAG ! foo:\n---\nval: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "val")), 1LL);
  cyaml_destroy(doc);
}

TEST(directives, multiple_directives_ignored) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2 foo\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, yaml_directive_comment_without_separating_space_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.1#comment\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(directives, yaml_directive_extra_spaces_accepted) {
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML  1.1\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, yaml_directive_tab_separator_accepted) {
  REQUIRE_TRUE(
      _tab_accepted_as("%YAML\t1.1\n---\nkey: value\n", "{key: value}"));
  REQUIRE_TRUE(
      _tab_accepted_as("%YAML \t1.1\n---\nkey: value\n", "{key: value}"));
  REQUIRE_TRUE(_tab_accepted_as("%YAML 1.2\t# comment\n---\nk: v\n", "{k: v}"));
  require_tab_rejected("%YAML\t\n---\nk: v\n");
  require_tab_rejected("%YAML 1.2\tx\n---\nk: v\n");
}

TEST(directives, tag_directive_tab_separator_accepted) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.1  # comment\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, unrecognized_directive_name_with_extra_words_accepted) {
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

// Document-start marker edge cases

TEST(block_mapping, indented_triple_dash_is_plain_scalar) {
  char *err = NULL;
  cyaml doc = cyaml_parse("   ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---");
  cyaml_destroy(doc);
}

TEST(errors, block_mapping_cannot_start_on_document_marker_line) {
  char *err = NULL;
  cyaml doc = cyaml_parse("--- a: b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_sequence_cannot_start_on_document_marker_line) {
  char *err = NULL;
  cyaml doc = cyaml_parse("--- - a\n    - b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, anchored_mapping_cannot_start_on_document_marker_line) {
  char *err = NULL;
  cyaml doc = cyaml_parse("--- &anchor a: b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, mapping_on_own_line_after_document_marker_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("---\na: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  cyaml_destroy(doc);
}

TEST(block_mapping, scalar_on_document_marker_line_still_works) {
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

// Explicit "? key" / ": value" block mapping entries

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
  char *err = NULL;
  cyaml doc = cyaml_parse("? - a\n  - b\n: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "[a, b]")), "value");
  cyaml_destroy(doc);
}

TEST(explicit_block_mapping, tagged_root_object) {
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

// Flow collection single-pair and bare-key shorthands

TEST(flow_collections, sequence_bare_pair_shorthand) {
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

// An alias name ends at a ':' followed by white space or the end of input, as
// a plain scalar does, so "*x: y" names the alias "x". Every other alias-key
// test puts a space before the colon.
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

// A dictionary used as a key is canonicalized with its keys sorted, whatever
// their order in the source, so two keys with the same content collide. The
// public flow serializer keeps insertion order instead.
TEST(flow_collections,
     non_scalar_multi_key_dictionary_key_is_canonically_sorted) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("{outer: {z: 1, a: 2}}: v\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(
      cyaml_str_val(cyaml_dictionary_get(doc, "{outer: {a: 2, z: 1}}")), "v");
  cyaml_destroy(doc);
}

TEST(block_mapping, flow_collection_compact_implicit_key) {
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

// Later entries go through parse_one_dict_entry_key(), which must not filter
// out '[' and '{' before try_parse_scalar_dict_key().
TEST(block_mapping, flow_collection_compact_implicit_key_not_only_first_entry) {
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

// "? []: x" is a mapping whose key is the mapping {[]: x}. Its canonical text
// quotes "[]" so the key doesn't read back as an empty list.
TEST(block_mapping, explicit_key_with_compact_flow_list_content) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("[23\n]: 42\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(directives, verbatim_tag_with_comma_not_swallowed_as_flow_terminator) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "--- !<tag:clarkevans.com,2002:invoice>\ninvoice: 34843\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "invoice")), 34843LL);
  cyaml_destroy(doc);
}

// A tagged key must not take in a tagged sibling entry. The key's tag isn't
// kept, since keys are plain strings, while "!!str" on a value stays on the
// node.
TEST(block_mapping, tagged_key_does_not_swallow_sibling_entry) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("key: - a\n     - b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, sequence_cannot_start_inline_after_anchor) {
  char *err = NULL;
  cyaml doc = cyaml_parse("&anchor - sequence entry\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, sequence_cannot_start_inline_after_tag) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq - sequence entry\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, explicit_value_sequence_can_start_inline) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("? - a\n  - b\n: v\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml_destroy(doc);
}

TEST(block_list, sequence_compact_mapping_under_dash_still_works) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("block: ># comment\n  scalar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_folded) {
  char *err = NULL;
  cyaml doc = cyaml_parse("scalar: >\n \n  \n   \n invalid\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_literal) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "block scalar: |\n     \n  more spaces at the beginning\n"
      "  are invalid\n",
      &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_before_comment) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("empty block scalar: >\n \n  \n   \n # comment\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_double_quoted_implicit_key_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\nb\": 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_single_quoted_implicit_key_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("'c\n d': 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_quoted_key_nested_in_sequence_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("- - \"bar\nbar\": x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_double_quoted_implicit_key_rejected_bare_cr) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\rb\": 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, multiline_single_quoted_implicit_key_rejected_bare_cr) {
  char *err = NULL;
  cyaml doc = cyaml_parse("'c\r d': 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, multiline_quoted_value_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("quoted: \"a\nb\nc\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "quoted")), "a b c");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_multiline_quoted_value_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x \"multi\nline\"\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "multi line");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "multi line");
  cyaml_destroy(doc);
}

TEST(errors, document_marker_inside_unclosed_flow_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[\n--- ,\n...\n]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_dictionary_continuation_lines_at_column_zero_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("k: {\nk\n:\nv\n}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, implicit_key_colon_may_fold_to_a_later_line_ok) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("{k: \nv}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "k")), "v");
  cyaml_destroy(doc);
}

TEST(flow_collections, explicit_key_colon_on_later_line_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{? key\n: value}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(errors, flow_collection_continuation_line_not_indented_enough_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("flow: [a,\nb,\nc]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, continuation_line_indented_enough_still_works) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("a: {b: &x\nc}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, anchor_continuation_line_indented_enough_still_works) {
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
  require_tab_rejected("a: {b: &x\n\tc}\n");
  REQUIRE_TRUE(_tab_accepted_as("{a: &x\n\tc}\n", "{a: c}"));
}

TEST(errors, tag_flow_continuation_line_not_indented_enough_rejected) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("-\t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tab_after_dash_and_separator_space_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("- \t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tab_directly_after_question_mark_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("?\t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tab_after_question_mark_before_key_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("?\tkey:\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, tab_after_question_mark_before_scalar_key_accepted) {
  REQUIRE_TRUE(_tab_accepted_as("? \ta\n: 1\n", "{a: 1}"));
  REQUIRE_TRUE(_tab_accepted_as("?\ta\n:\t1\n", "{a: 1}"));
}

TEST(block_mapping, tab_after_question_mark_accepted_for_later_entry) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\n: 1\n? b\n: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), (long long)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), (long long)2);
  cyaml_destroy(doc);
}

TEST(block_list, dash_space_content_still_works) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("{\"foo\"\n: \"bar\"}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "bar");
  cyaml_destroy(doc);
}

TEST(flow_collections, multiline_flow_list_not_confused_by_unindented_dash) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("scalar: |\n \n  content\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "scalar")),
                "\ncontent\n");
  cyaml_destroy(doc);
}

TEST(errors, hash_immediately_after_comma_is_not_a_valid_element) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[ a, b, c,#invalid\n]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_list_comment_line_does_not_substitute_for_missing_comma) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key: [ word1\n#  xxx\n  word2 ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, multiline_plain_scalar_stops_before_comment_line) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("[ \"key\"\r  :value ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, multiline_flow_dictionary_key) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("a: b: c: d\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_quoted_key_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 'b': c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_flow_collection_key_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: [1,2]: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_under_sequence_element_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("- a: b: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_after_same_line_anchor_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x b: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, chained_implicit_mapping_value_explicit_key_form_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: ? b\n   : c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, anchor_on_own_line_still_permits_nested_mapping_value) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("[-]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, bare_dashes_separated_by_comma_in_flow_sequence_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[-, -]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(flow_collections, dash_followed_by_plain_content_still_valid) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("[?]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, scalar_with_two_stacked_anchors_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("top2: &node2\n  &v2 val2\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, scalar_with_two_stacked_anchors_same_line_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("&a &b val\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, two_stacked_tags_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str !!int 5\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, anchor_decorating_a_mapping_whose_key_is_also_anchored) {
  char *err = NULL;
  cyaml doc = cyaml_parse("top1: &node1\n  &k1 key1: val1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml top1 = cyaml_dictionary_get(doc, "top1");
  REQUIRE_EQ(cyaml_type(top1), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(top1, "key1")), "val1");
  cyaml_destroy(doc);
}

// An alias to an anchored key resolves to the key's real type and structure
// ("&n 42:" to an integer, "&x [1, 2]:" to a list), never to a string of its
// canonical text, just as an anchor in a value position does.
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
  char *err = NULL;
  cyaml doc = cyaml_parse("&a &b foo: bar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, two_stacked_tags_same_line_decorating_a_key_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str !!int foo: bar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, anchor_decorating_a_bare_alias_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key1: &a value\nkey2: &b *a\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, tag_decorating_a_bare_alias_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key1: &n value\nkey2: !!int *n\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(block_mapping, alias_used_as_key_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\n*x : bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "foo");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "bar");
  cyaml_destroy(doc);
}

TEST(errors, comma_directly_after_tag_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("- !!str, xxx\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, flow_indicator_directly_after_anchor_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("- &a[1,2]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// A '[' or '{' glued to an anchor or tag name has no valid meaning in any
// context, including a flow collection nested in another one. A ',', ']' or
// '}' there is ordinary flow structure inside a flow collection.
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
  char *err = NULL;
  cyaml doc = cyaml_parse("- !!str a, b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "a, b");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchor_and_tag_combo_still_works_either_order) {
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

// This test and the ones after it come from differential testing against the
// YAML Test Suite and the fuzz corpus (tests/cyaml/differential/
// compare_pyyaml.py), and Ruby's Psych agrees with every expectation.
TEST(multi_document, two_consecutive_directive_documents_without_end_marker) {
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n---\n%YAML 1.2\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(multi_document, directive_document_after_end_marker_still_works) {
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
  REQUIRE_TRUE(_tab_accepted_as("---\tfoo\n", "foo"));
  REQUIRE_TRUE(_tab_accepted_as("--- \tfoo\n", "foo"));
  REQUIRE_TRUE(_tab_accepted_as("%YAML 1.2\n---\tfoo\n", "foo"));
  REQUIRE_TRUE(_tab_accepted_as("---\t[a]\n", "[a]"));
  require_tab_rejected("---\t- a\n");
}

TEST(multi_document, tab_after_end_marker) {
  REQUIRE_TRUE(_tab_accepted_as("--- x\n...\t# c\n", "x"));
  REQUIRE_TRUE(_tab_accepted_as("--- x\n... \t# c\n", "x"));
  require_tab_rejected("a: 1\n...\tb\n");
  require_tab_rejected("a: 1\n... \tb\n");
}

TEST(multi_document, space_after_start_and_end_marker_still_works) {
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

// A directive can't follow the "---" that a document already consumed, and
// without a "..." in between this is a parse error, not two documents.
TEST(multi_document, bare_start_marker_then_directive_document_is_error) {
  char *err = NULL;
  cyaml doc = cyaml_parse("---\n%YAML 1.2\n---\nfoo\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// A block scalar of indent 0 can't tell a "---" or "..." at column 0 from
// content by the indent alone, so scan_block_scalar_line() checks for a
// document marker explicitly.
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
  const char *yaml = "\"Empty line\n \t\nas a line feed\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "Empty line\nas a line feed");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_blank_line_with_only_spaces_folds_to_newline) {
  const char *yaml = "'foo\n \nbar'\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "foo\nbar");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escaped_tab_before_fold_is_preserved) {
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
  const char *yaml = "\"a\\t  \nb\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "a\t b");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_keep_blank_line_excess_indentation_preserved) {
  const char *yaml = "|+\n ab\n \n  \n...\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "ab\n\n \n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_clip_mid_content_blank_excess_indentation) {
  const char *yaml = "text: |\n  a\n    \n  b\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "text")), "a\n  \nb\n");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_keep_blank_line_excess_indentation_preserved) {
  const char *yaml = "foo: >+\n  x\n   \n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n \n");
  cyaml_destroy(doc);
}

TEST(block_scalars, literal_clip_no_source_trailing_newline_omits_final_lf) {
  const char *yaml = "foo: |\n  x\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n ");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_keep_no_source_trailing_newline_omits_final_lf) {
  const char *yaml = "foo: >+\n  x\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n ");
  cyaml_destroy(doc);
}

TEST(block_scalars, literal_keep_leading_blank_only_no_source_newline) {
  const char *yaml = "- |+\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "");
  cyaml_destroy(doc);
}

TEST(block_list, dash_followed_only_by_comment_is_a_null_entry) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("a: !!str\nb: two\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "a")), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "two");
  cyaml_destroy(doc);
}

// This test and the ones after it cover every place where a tab can stand as
// indentation. They separate the positions that must be refused from same-line
// separator white space and content that is already open.
TEST(errors, tab_as_pure_block_mapping_indentation_rejected) {
  require_tab_rejected("---\na:\n\tb:\n\t\tc: value\n");
}

TEST(errors, tab_as_pure_block_sequence_indentation_rejected) {
  require_tab_rejected("-\n\t- a\n\t- b\n");
}

TEST(errors, tab_at_start_of_document_rejected) {
  require_tab_rejected("\ta: 1\n");
}

TEST(errors, tab_before_anchor_after_fresh_line_rejected) {
  require_tab_rejected("-\n\t&x val\n");
}

TEST(errors, tab_after_anchor_same_line_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key: &x\tvalue\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(errors, tab_on_flow_collection_continuation_line_rejected) {
  require_tab_rejected("- [\n\tfoo,\n foo\n ]\n");
}

TEST(errors, tab_on_flow_dictionary_continuation_line_rejected) {
  require_tab_rejected("- {\n\tfoo: 1,\n bar: 2\n }\n");
}

TEST(errors, tab_on_flow_collection_continuation_line_rejected_bare_cr) {
  require_tab_rejected("- [\r\tfoo,\r foo\r ]\r");
}

TEST(errors, tab_on_flow_dictionary_continuation_line_rejected_bare_cr) {
  require_tab_rejected("- {\r\tfoo: 1,\r bar: 2\r }\r");
}

TEST(block_mapping, tab_after_colon_value_indicator) {
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
  REQUIRE_TRUE(_tab_accepted_as("seq:\t\n - a\n", "{seq: [a]}"));
  REQUIRE_TRUE(_tab_accepted_as("a:\t# c\n  b: 1\n", "{a: {b: 1}}"));
}

TEST(block_mapping,
     explicit_key_value_split_across_tab_indented_lines_rejected) {
  require_tab_rejected("?\n\tkey\n:\n\tvalue\n");
}

// A lone leading tab can land exactly on map_indent by byte count, and it is
// still refused like every other tab indent.
TEST(block_mapping, explicit_key_value_indicator_tab_indented_rejected) {
  require_tab_rejected("a:\n ? b\n\t: c\n");
}

TEST(block_scalars, tab_only_leading_blank_line_rejected) {
  require_tab_rejected("foo: |\n\t\nbar: 1\n");
}

TEST(block_scalars, tab_after_one_leading_space_on_blank_line_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: |\n \t\nbar: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "\t\n");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "bar")), 1LL);
  cyaml_destroy(doc);
}

TEST(block_scalars, tab_right_after_established_indent_is_content_literal) {
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: |-\n \tbar", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "\tbar");
  cyaml_destroy(doc);
}

TEST(block_scalars,
     tab_right_after_established_indent_folded_is_content_literal) {
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: >\n  real\n  \tmore\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")),
                "real\n\tmore\n");
  cyaml_destroy(doc);
}

// Tags

TEST(tags, shorthand_secondary_resolves_to_core_schema_uri) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str foo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), CYAML_TAG_STR);
  cyaml_destroy(doc);
}

TEST(tags, shorthand_primary_resolves_to_local_tag) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a>b\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, verbatim_tag_glued_directly_to_value_content_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("k: !<a>b\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, verbatim_tag_glued_directly_to_block_scalar_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a>|\n  x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, verbatim_tag_with_real_separator_still_works) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a> b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "a");
  REQUIRE_STREQ(cyaml_str_val(doc), "b");
  cyaml_destroy(doc);
}

// The non-specific tag "!" makes a plain scalar a string whatever its text
// (YAML 1.2 section 6.9.1), leaves a collection as it is and isn't stored.
// Reading it as no tag would give 42 for "! 42".
TEST(tags, bare_non_specific_tag_makes_a_plain_scalar_a_string) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "int: ! 42\n"
      "null: ! ~\n"
      "bool: ! true\n"
      "float: ! 1.5\n"
      "empty: !\n"
      "list: ! [1, 2]\n"
      "map: ! {a: 1}\n"
      "! 0x11: key\n"
      "anchored: &x ! 12\n"
      "alias: *x\n"
      "! <<: not a merge\n"
      "quoted: ! \"7\"\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const names[] = {"int",      "null",  "bool",  "float",
                                      "anchored", "alias", "quoted"};
  static const char *const texts[] = {"42", "~",  "true", "1.5",
                                      "12", "12", "7"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    cyaml v = cyaml_dictionary_get(doc, names[i]);
    REQUIRE_NE((void *)v, NULL);
    REQUIRE_EQ((int)cyaml_type(v), (int)CYAML_STRING);
    REQUIRE_STREQ(cyaml_str_val(v), texts[i]);
    REQUIRE_EQ((void *)cyaml_node_tag(v), NULL);
  }
  cyaml empty = cyaml_dictionary_get(doc, "empty");
  REQUIRE_EQ((int)cyaml_type(empty), (int)CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(empty), "");
  cyaml list = cyaml_dictionary_get(doc, "list");
  REQUIRE_EQ((int)cyaml_type(list), (int)CYAML_LIST);
  REQUIRE_EQ((void *)cyaml_node_tag(list), NULL);
  REQUIRE_EQ((int)cyaml_type(cyaml_list_get(list, 0)), (int)CYAML_INTEGER);
  cyaml map = cyaml_dictionary_get(doc, "map");
  REQUIRE_EQ((int)cyaml_type(map), (int)CYAML_DICTIONARY);
  REQUIRE_EQ((void *)cyaml_node_tag(map), NULL);
  REQUIRE_NE((void *)cyaml_dictionary_get(doc, "0x11"), NULL);
  REQUIRE_EQ((void *)cyaml_dictionary_get(doc, "17"), NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "<<")), "not a merge");
  cyaml_destroy(doc);
}

// A written tag that resolves to "!" is refused, like the verbatim "!<!>" of
// YAML 1.2 example 6.25; the setter refuses "!" because no written tag could
// carry it through a round trip.
TEST(tags, a_written_tag_that_names_the_non_specific_tag_is_refused) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<!> x\n", &err);
  bool refused = doc == NULL && err != NULL;
  cyaml_destroy(doc);
  cyaml n = cyaml_create_string("x");
  ccol_retval rv = n ? cyaml_node_set_tag(n, "!") : ccol_unexpected_failure;
  bool untagged = n && cyaml_node_tag(n) == NULL;
  cyaml_destroy(n);
  REQUIRE_TRUE(refused);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  REQUIRE_TRUE(untagged);
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
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq\n  &a key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, tag_on_mapping_introduced_by_aliased_key_is_preserved) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("[! !!str x]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

// The second tag sits on the next line, so had_tag has to travel into the
// recursive parse; the resolved tag of "!" is NULL and can't tell.
TEST(tags, non_specific_tag_then_named_tag_rejected_on_own_line) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key: !\n  !!int 5\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, alias_cannot_carry_a_non_specific_tag) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\nb: ! *x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, tag_on_dictionary_key_parses_but_is_not_preserved) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str key: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ((void *)cyaml_node_tag(doc), NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(tags, undefined_tag_handle_on_dictionary_key_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!x!foo key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tags, malformed_verbatim_tag_on_dictionary_key_rejected) {
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
  ccol_retval rv = cyaml_set(doc, "a", "bar");
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
  ccol_retval rv = cyaml_node_set_tag(n, CYAML_TAG_STR);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 42LL);
  REQUIRE_EQ((void *)cyaml_node_tag(n), NULL);
  ccol_retval rv2 = cyaml_node_set_tag(n, CYAML_TAG_INT);
  REQUIRE_EQ((int)rv2, (int)ccol_success);
  REQUIRE_STREQ(cyaml_node_tag(n), CYAML_TAG_INT);
  ccol_retval rv3 = cyaml_node_set_tag(n, NULL);
  REQUIRE_EQ((int)rv3, (int)ccol_success);
  REQUIRE_EQ((void *)cyaml_node_tag(n), NULL);
  cyaml_destroy(n);
}

TEST(tags, node_tag_of_null_node_is_null) {
  REQUIRE_EQ((void *)cyaml_node_tag(NULL), NULL);
}

// Tag directives (%TAG)

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
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app/ garbage-text\n---\na: 1\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_directives, trailing_comment_with_separator_accepted) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:  # comment\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_glued_to_prefix_is_part_of_the_uri) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app#\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app#foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_glued_mid_prefix_is_part_of_the_uri) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG !e! tag:example.com/#zzz\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com/#zzzfoo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_after_real_whitespace_is_still_a_comment) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!app%2Ffoo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app/foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, shorthand_tag_secondary_handle_percent_escape_resolves) {
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

// Tag-driven type resolution

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
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int 0x-0\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(tag_typing, int_rejects_uppercase_hex_prefix) {
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

TEST(tag_typing, float_refuses_oversized_hex) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0xFFFFFFFFFFFFFFFF\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_refuses_oversized_octal) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0o1777777777777777777777\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
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

// !!bool doesn't trim before matching (as in PyYAML), so a KEEP-chomped "true"
// with trailing newlines is refused.
TEST(tag_typing, bool_forces_on_literal_block_keep_untrimmed_rejected) {
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
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int \" 42\"\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(tag_typing, float_rejects_leading_whitespace) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float \" 3.5\"\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

// strtoll() and strtod() skip VT and FF too, and a double-quoted scalar can
// carry both through escapes, so a guard that only knows space, tab, LF and CR
// would accept "\v42".
TEST(tag_typing, numeric_tags_reject_every_leading_isspace_byte) {
  static const char *const docs[] = {
      "!!int \"\\v42\"\n",    "!!int \"\\f42\"\n",      "!!int \"\\v-42\"\n",
      "!!int \"\\f0x10\"\n",  "!!int \"\\v0o7\"\n",     "!!float \"\\v3.5\"\n",
      "!!float \"\\f3.5\"\n", "!!float \"\\v-1e5\"\n",  "!!float \"\\f.5\"\n",
      "!!int \"\\x0b42\"\n",  "!!float \"\\x0c3.5\"\n",
  };
  size_t accepted = 0;
  for (size_t i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(docs[i], &err);
    if (doc != NULL || err == NULL) accepted++;
    if (doc != NULL) cyaml_destroy(doc);
  }
  REQUIRE_EQ(accepted, (size_t)0);

  char *err = NULL;
  cyaml doc = cyaml_parse("!!int \"42\"\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  bool int_ok = cyaml_type(doc) == CYAML_INTEGER && cyaml_int_val(doc) == 42;
  cyaml_destroy(doc);
  REQUIRE_TRUE(int_ok);
  doc = cyaml_parse("!!float \".5\"\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  bool float_ok =
      cyaml_type(doc) == CYAML_FLOAT && cyaml_double_val(doc) == 0.5;
  cyaml_destroy(doc);
  REQUIRE_TRUE(float_ok);
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

// This test and the ones after it go through the nine structural mismatches
// between a tag and a collection or scalar.
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

// Serialize tags

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
  ccol_retval rv = cyaml_node_set_tag(n, CYAML_TAG_INT);
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

// cyaml_node_set_tag() refuses a core-schema tag of another type, so no such
// tag reaches the serializer. The node keeps its tag and round-trips.
TEST(serialize_tags, mismatched_core_schema_scalar_tag_is_refused) {
  cyaml n = cyaml_create_int(42);
  ccol_retval rv = cyaml_node_set_tag(n, CYAML_TAG_STR);
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
  ccol_retval rv = cyaml_node_set_tag(n, CYAML_TAG_INT);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  char *out = cyaml_serialize_flow(n);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_STREQ(out, "true");

  cyaml_serialize_free(out);
  cyaml_destroy(n);
}

// The same for a mismatched sequence or mapping tag.
TEST(serialize_tags, mismatched_core_schema_collection_tag_is_refused) {
  cyaml n = cyaml_create_list();
  ccol_retval rv = cyaml_node_set_tag(n, CYAML_TAG_MAP);
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

// Merge keys (<<:)

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

// An explicit key's content can start on the next line, so the merge check
// looks where the key's content starts, not right after the '?'.
TEST(merge_keys, explicit_key_form_block_key_on_own_line_expands) {
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

// The merge peek flags "<< foo", and only the fully parsed key text decides
// that it is an ordinary key.
TEST(merge_keys, explicit_key_form_block_followed_by_word_not_merged) {
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

// Tells whether map's members, in member order, are exactly keys[i] with the
// integer values vals[i], so the merge tests can pin both the members and
// their order.
static bool merge_test_members_are(cyaml map, const char *const *keys,
                                   const long long *vals, size_t n) {
  if (cyaml_type(map) != CYAML_DICTIONARY || cyaml_dictionary_size(map) != n)
    return false;
  cyaml_dictionary_iter it;
  size_t i = 0;
  for (bool more = cyaml_dictionary_first(map, &it); more;
       more = cyaml_dictionary_next(&it), i++) {
    if (i >= n || strcmp(it.key, keys[i]) != 0) return false;
    if (cyaml_type(it.value) != CYAML_INTEGER ||
        cyaml_int_val(it.value) != vals[i])
      return false;
  }
  return i == n;
}

// This test and the ones after it hold several "<<" entries in one mapping.
// Every merge entry expands, and a quoted or tagged "<<" is an ordinary member
// that never collides with one. They fail when merge entries are stored under
// the key "<<", because the later one then replaces the earlier one.
TEST(merge_keys,
     block_duplicate_double_angle_bracket_later_quoted_string_not_merged) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: &x\n  k: 1\nb:\n  <<: *x\n  \"<<\": literal_string\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml b = cyaml_dictionary_get(doc, "b");
  REQUIRE_NE((void *)b, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)3);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(lt), "literal_string");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
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
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)3);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(lt, "other")), 9LL);
  REQUIRE_EQ((void *)cyaml_dictionary_get(b, "other"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  cyaml_destroy(doc);
}

TEST(merge_keys,
     block_duplicate_double_angle_bracket_earlier_quoted_still_merges) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("a: &x\n  k: 1\nb:\n  \"<<\": 7\n  <<: *x\n  k2: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"<<", "k", "k2"};
  static const long long vals[] = {7, 1, 2};
  bool members_ok =
      merge_test_members_are(cyaml_dictionary_get(doc, "b"), keys, vals, 3);
  cyaml_destroy(doc);
  REQUIRE_TRUE(members_ok);
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
  REQUIRE_EQ(cyaml_dictionary_size(b), (size_t)3);
  cyaml lt = cyaml_dictionary_get(b, "<<");
  REQUIRE_NE((void *)lt, NULL);
  REQUIRE_EQ(cyaml_type(lt), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(lt, "other")), 9LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(b, "k2")), 2LL);
  cyaml_destroy(doc);
}

TEST(merge_keys, flow_two_merge_entries_both_expand) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{<<: {k: 1}, <<: {z: 2}}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"k", "z"};
  static const long long vals[] = {1, 2};
  bool members_ok = merge_test_members_are(doc, keys, vals, 2);
  cyaml_destroy(doc);
  REQUIRE_TRUE(members_ok);
}

TEST(merge_keys, flow_quoted_double_angle_bracket_after_a_merge_is_a_member) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{<<: {k: 1}, \"<<\": 5}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"k", "<<"};
  static const long long vals[] = {1, 5};
  bool members_ok = merge_test_members_are(doc, keys, vals, 2);
  cyaml_destroy(doc);
  REQUIRE_TRUE(members_ok);
}

TEST(merge_keys, flow_tagged_double_angle_bracket_before_a_merge_is_a_member) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{!!str <<: 5, <<: {k: 1}}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"<<", "k"};
  static const long long vals[] = {5, 1};
  bool members_ok = merge_test_members_are(doc, keys, vals, 2);
  cyaml_destroy(doc);
  REQUIRE_TRUE(members_ok);
}

TEST(merge_keys, a_later_merge_entry_wins_over_an_earlier_one) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "a: {<<: {k: 1}, <<: {k: 2}}\nb: {<<: {k: 1}, k: 0, <<: {k: 2}}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"k"};
  static const long long a_vals[] = {2};
  static const long long b_vals[] = {0};
  bool a_ok =
      merge_test_members_are(cyaml_dictionary_get(doc, "a"), keys, a_vals, 1);
  bool b_ok =
      merge_test_members_are(cyaml_dictionary_get(doc, "b"), keys, b_vals, 1);
  cyaml_destroy(doc);
  REQUIRE_TRUE(a_ok);
  REQUIRE_TRUE(b_ok);
}

TEST(merge_keys, block_merge_entries_with_lists_and_aliases_keep_their_place) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "b: &b {x: 1, y: 2}\n"
      "c: &c {y: 3, z: 4}\n"
      "d: &d {w: 5, z: 6}\n"
      "m:\n"
      "  p: 0\n"
      "  <<: *b\n"
      "  q: 1\n"
      "  <<: [*c, *d]\n"
      "  r: 2\n"
      "  x: 9\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"p", "y", "q", "z", "w", "r", "x"};
  static const long long vals[] = {0, 3, 1, 4, 5, 2, 9};
  bool members_ok =
      merge_test_members_are(cyaml_dictionary_get(doc, "m"), keys, vals, 7);
  cyaml_destroy(doc);
  REQUIRE_TRUE(members_ok);
}

TEST(merge_keys, explicit_and_tagged_merge_entries_with_quoted_members) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "? <<\n"
      ": {a: 1}\n"
      "'<<': 3\n"
      "!!merge \"<<\": {b: 2}\n"
      "<<: {a: 9, c: 4}\n",
      &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const keys[] = {"a", "<<", "b", "c"};
  static const long long vals[] = {9, 3, 2, 4};
  bool members_ok = merge_test_members_are(doc, keys, vals, 4);
  cyaml_destroy(doc);
  REQUIRE_TRUE(members_ok);
}

TEST(merge_keys, a_second_merge_source_error_is_reported) {
  char *err = NULL;
  cyaml doc = cyaml_parse("{<<: {a: 1}, <<: [{b: 2}, 3]}", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "merge key source is not a mapping"), NULL);
}

// A merge source can hold a quoted "<<" member of its own, which the merge
// copies like any other member.
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

// Raw control characters in scalars

TEST(errors, raw_control_byte_rejected_in_double_quoted_scalar) {
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
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\0b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_byte_escape_hex_rejected) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\x00b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_byte_escape_u4_rejected) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\u0000b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(errors, null_byte_escape_u8_rejected) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\U00000000b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(quoted, surrogate_pair_combination_never_produces_null_byte) {
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\ud800\\udc00\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xf0\x90\x80\x80");
  cyaml_destroy(n);
}

TEST(serialize_scalars, control_byte_in_string_is_quoted_and_escaped) {
  cyaml n = cyaml_create_string("x\x01y");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(strstr(s, "\x01") != NULL, false);
  REQUIRE_NE((void *)strstr(s, "\\x01"), NULL);

  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_STREQ(cyaml_str_val(back), "x\x01y");

  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

// Serializes v alone with both serializers and tells whether both give the
// text want and parse back to exactly v as a float. The float text tests use
// it to pin the shortest round-trip form.
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
  REQUIRE_TRUE(_float_text_round_trips(1e20, "1.0e+20"));
  REQUIRE_TRUE(_float_text_round_trips(1e-5, "1.0e-05"));
  REQUIRE_TRUE(_float_text_round_trips(-1e300, "-1.0e+300"));
  REQUIRE_TRUE(_float_text_round_trips(5e-324, "5.0e-324"));
  REQUIRE_TRUE(_float_text_round_trips(1.5e20, "1.5e+20"));
  REQUIRE_TRUE(_float_text_round_trips(2.0, "2.0"));
  REQUIRE_TRUE(_float_text_round_trips(-0.0, "-0.0"));
  REQUIRE_TRUE(_float_text_round_trips(0.25, "0.25"));
}

TEST(serialize_scalars, subnormal_float_text_is_the_shortest_that_round_trips) {
  REQUIRE_TRUE(_float_text_round_trips(5e-324, "5.0e-324"));
  REQUIRE_TRUE(_float_text_round_trips(-5e-324, "-5.0e-324"));
  REQUIRE_TRUE(_float_text_round_trips(1e-310, "1.0e-310"));
  REQUIRE_TRUE(_float_text_round_trips(2.5e-320, "2.5e-320"));
  bool all_ok = true;
  for (double v = 5e-324; v < 2.3e-308 && all_ok; v = v * 3.7 + 5e-324) {
    char *text = NULL;
    cyaml n = cyaml_create_double(v);
    text = n ? cyaml_serialize(n) : NULL;
    char *err = NULL;
    cyaml back = text ? cyaml_parse(text, &err) : NULL;
    all_ok =
        back && cyaml_type(back) == CYAML_FLOAT && cyaml_double_val(back) == v;
    cyaml_destroy(back);
    if (text) cyaml_serialize_free(text);
    cyaml_destroy(n);
  }
  REQUIRE_TRUE(all_ok);
  REQUIRE_TRUE(_float_text_round_trips(2.2250738585072014e-308,
                                       "2.2250738585072014e-308"));
}

TEST(serialize_scalars, a_subnormal_float_key_keeps_one_canonical_text) {
  char *err = NULL;
  cyaml n = cyaml_parse("{4.9406564584124654e-324: a, 5e-324: b}", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  cyaml v = cyaml_dictionary_get(n, "5e-324");
  bool ok =
      cyaml_dictionary_size(n) == 1 && v && strcmp(cyaml_str_val(v), "b") == 0;
  char *flow = cyaml_serialize_flow(n);
  ok = ok && flow && strcmp(flow, "{5e-324: b}") == 0;
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(n);
  REQUIRE_TRUE(ok);
}

TEST(serialize_scalars, float_text_is_the_shortest_that_round_trips) {
  REQUIRE_TRUE(_float_text_round_trips(1.0 / 3.0, "0.3333333333333333"));
  REQUIRE_TRUE(_float_text_round_trips(0.1 + 0.2, "0.30000000000000004"));
  REQUIRE_TRUE(_float_text_round_trips(0.1, "0.1"));

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

// Thread-local node pool

// The pool cap, the same value as CYAML_POOL_CAP in cyaml.c, which no header
// exports. The pool tests need it to check that the pool stops at the cap.
#define CYAML_TEST_POOL_CAP 512

// Destroying more than the cap of nodes in one go leaves the pool holding
// exactly the cap, whatever earlier tests left in it.
TEST(node_pool, cap_eviction_keeps_pool_bounded) {
  cyaml list = cyaml_create_list();
  REQUIRE_NE((void *)list, NULL);
  size_t n = CYAML_TEST_POOL_CAP + 100;
  for (size_t i = 0; i < n; i++)
    REQUIRE_EQ(cyaml_list_push(list, cyaml_create_null()), ccol_success);
  cyaml_destroy(list);

  REQUIRE_EQ(cyaml_debug_pool_size(), (size_t)CYAML_TEST_POOL_CAP);
}

// The pool sizes a worker thread saw when it started and when it finished.
typedef struct {
  size_t start_pool_size;
  size_t end_pool_size;
} pool_populate_result_t;

// The worker of the fresh-thread pool test: Records its pool size, allocates
// 50 nodes and only then frees them all, and records the size again in the
// pool_populate_result_t at arg. All 50 are alive at once, so each is a fresh
// allocation and the pool grows by one entry per node.
static void *pool_populate_thread(void *arg) {
  pool_populate_result_t *r = (pool_populate_result_t *)arg;
  r->start_pool_size = cyaml_debug_pool_size();
  cyaml nodes[50];
  for (int i = 0; i < 50; i++) nodes[i] = cyaml_create_null();
  for (int i = 0; i < 50; i++) cyaml_destroy(nodes[i]);
  r->end_pool_size = cyaml_debug_pool_size();
  return NULL;
}

// The pool is per thread, so a new thread starts empty and its frees go to its
// own pool. The worker exits holding 50 nodes, so memtest also checks that the
// thread-exit drain frees them.
TEST(node_pool, fresh_thread_starts_with_an_empty_pool) {
  pool_populate_result_t result = {(size_t)-1, (size_t)-1};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, pool_populate_thread, &result), 0);
  pthread_join(tid, NULL);

  REQUIRE_EQ(result.start_pool_size, (size_t)0);
  REQUIRE_EQ(result.end_pool_size, (size_t)50);
}

// The nodes a worker frees, and the pool sizes it saw before and after.
typedef struct {
  cyaml *nodes;
  size_t count;
  size_t start_pool_size;
  size_t end_pool_size;
} cross_thread_free_arg_t;

// The worker of the cross-thread free test: Destroys the nodes in the
// cross_thread_free_arg_t at arg and records its own pool size before and
// after.
static void *cross_thread_free_thread(void *arg) {
  cross_thread_free_arg_t *a = (cross_thread_free_arg_t *)arg;
  a->start_pool_size = cyaml_debug_pool_size();
  for (size_t i = 0; i < a->count; i++) cyaml_destroy(a->nodes[i]);
  a->end_pool_size = cyaml_debug_pool_size();
  return NULL;
}

// node_free() gives a node to the pool of the thread that frees it, never to
// the thread that allocated it.
TEST(node_pool, node_freed_on_a_different_thread_joins_that_threads_own_pool) {
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
    for (size_t i = 0; i < count; i++) cyaml_destroy(nodes[i]);
  }
  free(nodes);

  size_t main_pool_after = cyaml_debug_pool_size();

  REQUIRE_EQ(create_rv, 0);
  REQUIRE_EQ(arg.start_pool_size, (size_t)0);
  REQUIRE_EQ(arg.end_pool_size, count);
  REQUIRE_EQ(main_pool_after, main_pool_before);
}

// The tree that late_destroy_from_destructor() destroys, and the pipe it
// reports on.
static cyaml late_destroy_tree;
static int late_destroy_report_fd = -1;

// Destroys late_destroy_tree after the library's destructor has run (a
// destructor with a priority runs after every one without), for the test that
// a late destroy frees its nodes. Writes 'Y' to late_destroy_report_fd when
// the thread's pool stayed empty, else 'N', and closes the fd.
__attribute__((destructor(101))) static void late_destroy_from_destructor(
    void) {
  if (!late_destroy_tree || late_destroy_report_fd < 0) return;
  cyaml_destroy(late_destroy_tree);
  char verdict = cyaml_debug_pool_size() == 0 ? 'Y' : 'N';
  ssize_t w = write(late_destroy_report_fd, &verdict, 1);
  (void)w;
  close(late_destroy_report_fd);
}

// A tree destroyed after the library's destructor drained the pool and deleted
// its key must be freed, not pooled where nothing drains it. The child ends
// with exit() so both destructors run.
TEST(node_pool, a_tree_destroyed_after_the_library_destructor_is_freed) {
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    late_destroy_tree =
        cyaml_parse("a: [1, 2, 3, {b: text}]\nc: [true, null]\n", NULL);
    late_destroy_report_fd = fds[1];
    exit(0);
  }
  close(fds[1]);
  char verdict = 0;
  ssize_t got = pid > 0 ? read(fds[0], &verdict, 1) : -1;
  close(fds[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  REQUIRE_GT(pid, 0);
#if defined(__OpenBSD__)
  if (got == 0) return;
#endif
  REQUIRE_EQ(got, (ssize_t)1);
  REQUIRE_EQ(verdict, 'Y');
}

// cyaml_type_str

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
    if (nodes[i]) cyaml__destroy(nodes[i]);

  REQUIRE_TRUE(all_created);
  REQUIRE_TRUE(all_named);
}

TEST(cyaml_type_str, a_null_handle_reports_the_null_type) {
  REQUIRE_STREQ(cyaml_type_str(NULL), "CYAML_NULL");
}

// Parse memory budget

TEST(errors, alias_expansion_bounded_in_bytes_not_only_in_node_count) {
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

// Explicit key: ':' glued to a following key

TEST(explicit_block_mapping,
     colon_glued_to_the_next_key_is_not_a_value_indicator) {
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

// Tags that need an escape in verbatim output

TEST(serialize_tags, tag_bytes_that_end_the_verbatim_token_round_trip) {
  const char *docs[] = {
      "key: !a%3Eb value\n",       "key: !a%3E%20b value\n",
      "key: !a%3E%20%23b value\n", "key: !a%0Ab value\n",
      "key: !a%25b value\n",
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

// Empty custom tag rejection

// An empty tag serializes as "!<>", which the parser refuses, so
// cyaml_node_set_tag() refuses it and keeps the node's tag. NULL clears a
// tag.
TEST(cyaml_tags,
     an_empty_custom_tag_is_rejected_and_leaves_the_node_untouched) {
  cyaml scalar = cyaml_create_string("hello");
  REQUIRE_NE((void *)scalar, NULL);

  REQUIRE_EQ(cyaml_node_set_tag(scalar, "!keepme"), ccol_success);
  REQUIRE_STREQ(cyaml_node_tag(scalar), "!keepme");

  REQUIRE_EQ(cyaml_node_set_tag(scalar, ""), ccol_invalid_args);
  REQUIRE_STREQ(cyaml_node_tag(scalar), "!keepme");

  REQUIRE_EQ(cyaml_node_set_tag(scalar, NULL), ccol_success);
  REQUIRE_EQ((void *)cyaml_node_tag(scalar), NULL);

  cyaml_destroy(scalar);
}

// Ownership: One return code, one rule about the child

// Every argument rejection returns ccol_invalid_args and leaves the child with
// the caller. The pool's free-list length shows a wrongly destroyed child as
// one extra pooled node, without reading freed memory.
TEST(ownership, every_rejected_list_push_leaves_the_child_with_the_caller) {
  cyaml wrong_kind = cyaml_create_dictionary();
  REQUIRE_NE((void *)wrong_kind, NULL);

  cyaml child = cyaml_create_int(5);
  size_t pooled = cyaml_debug_pool_size();
  ccol_retval r_null = cyaml_list_push(NULL, child);
  size_t pooled_after_null = cyaml_debug_pool_size();
  if (pooled_after_null == pooled) cyaml_destroy(child);

  child = cyaml_create_int(6);
  size_t pooled2 = cyaml_debug_pool_size();
  ccol_retval r_kind = cyaml_list_push(wrong_kind, child);
  size_t pooled_after_kind = cyaml_debug_pool_size();
  if (pooled_after_kind == pooled2) cyaml_destroy(child);

  ccol_retval r_self = cyaml_list_push(wrong_kind, wrong_kind);
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

  cyaml child = cyaml_create_int(5);
  size_t pooled_null = cyaml_debug_pool_size();
  ccol_retval r_null = cyaml_dictionary_set(NULL, "k", child);
  size_t pooled_after_null = cyaml_debug_pool_size();
  if (pooled_after_null == pooled_null) cyaml_destroy(child);

  child = cyaml_create_int(6);
  size_t pooled_key = cyaml_debug_pool_size();
  ccol_retval r_key = cyaml_dictionary_set(map, NULL, child);
  size_t pooled_after_key = cyaml_debug_pool_size();
  if (pooled_after_key == pooled_key) cyaml_destroy(child);

  child = cyaml_create_int(7);
  size_t pooled_kind = cyaml_debug_pool_size();
  ccol_retval r_kind = cyaml_dictionary_set(wrong_kind, "k", child);
  size_t pooled_after_kind = cyaml_debug_pool_size();
  if (pooled_after_kind == pooled_kind) cyaml_destroy(child);

  ccol_retval r_self = cyaml_dictionary_set(wrong_kind, "k", wrong_kind);
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

// A caller that destroys the child when the push says no must not free it
// twice. A rejection that also destroyed the child would put one node on the
// free list twice, and two later allocations would get the same address.
TEST(ownership, a_rejected_push_never_recycles_one_node_twice) {
  cyaml doc = cyaml_parse("a: 1\n", NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml absent = cyaml_get(doc, "nope");
  REQUIRE_EQ((void *)absent, NULL);

  cyaml child = cyaml_create_int(5);
  size_t pooled = cyaml_debug_pool_size();
  ccol_retval r = cyaml_list_push(absent, child);
  size_t pooled_after = cyaml_debug_pool_size();
  if (r != ccol_success && pooled_after == pooled) cyaml_destroy(child);

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

// cyaml__set_typed: raw_size describes the object raw names

// cyaml__set_typed() checks raw_size against the width it is about to read.
// Without the check a narrower object would be read as a pointer, which
// AddressSanitizer reports as a stack-buffer-overflow.
TEST(cyaml_set_typed, a_string_write_checks_the_width_of_the_object_raw_names) {
  cyaml doc = cyaml_parse("k: 1\n", NULL);
  REQUIRE_NE((void *)doc, NULL);

  char too_narrow = 'q';
  ccol_retval r_narrow = cyaml__set_typed(doc, "k", CYAML_STRING, &too_narrow,
                                          sizeof(too_narrow), false);
  long long still_int = cyaml_int_val(cyaml_get(doc, "k"));

  const char *proper = "hello";
  ccol_retval r_ok =
      cyaml__set_typed(doc, "k", CYAML_STRING, &proper, sizeof(proper), false);
  const char *written = cyaml_str_val(cyaml_get(doc, "k"));
  bool written_ok = (written && strcmp(written, "hello") == 0);

  cyaml_destroy(doc);

  REQUIRE_EQ(r_narrow, ccol_invalid_args);
  REQUIRE_EQ(still_int, 1LL);
  REQUIRE_EQ(r_ok, ccol_success);
  REQUIRE_TRUE(written_ok);
}

// A bool payload must be exactly sizeof(bool) wide, and only CYAML_NULL may
// come without a payload. Each refusal leaves the leaf untouched, for an
// existing key and for a new one.
TEST(cyaml_set_typed, a_bool_write_checks_its_width_and_raw_must_be_present) {
  cyaml doc = cyaml_parse("k: 1\n", NULL);
  REQUIRE_NE((void *)doc, NULL);

  int not_a_bool = 0x0100;
  ccol_retval r_wide = cyaml__set_typed(doc, "k", CYAML_BOOL, &not_a_bool,
                                        sizeof(not_a_bool), false);
  ccol_retval r_new = cyaml__set_typed(doc, "n", CYAML_BOOL, &not_a_bool,
                                       sizeof(not_a_bool), false);
  ccol_retval r_null_bool =
      cyaml__set_typed(doc, "k", CYAML_BOOL, NULL, sizeof(bool), false);
  ccol_retval r_null_int =
      cyaml__set_typed(doc, "k", CYAML_INTEGER, NULL, sizeof(int), true);
  ccol_retval r_null_float =
      cyaml__set_typed(doc, "k", CYAML_FLOAT, NULL, sizeof(double), false);
  ccol_retval r_null_str =
      cyaml__set_typed(doc, "k", CYAML_STRING, NULL, sizeof(char *), false);
  cyaml k = cyaml_get(doc, "k");
  bool untouched = k && cyaml_type(k) == CYAML_INTEGER && cyaml_int_val(k) == 1;
  bool no_new_key = cyaml_get(doc, "n") == NULL;

  bool b = true;
  ccol_retval r_ok =
      cyaml__set_typed(doc, "k", CYAML_BOOL, &b, sizeof(b), false);
  ccol_retval r_null_ok =
      cyaml__set_typed(doc, "z", CYAML_NULL, NULL, 0, false);
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

// A scalar that starts with a byte order mark

// A parse drops a U+FEFF at offset 0, so the serializer quotes a scalar that
// starts with one. Otherwise the first key of a dictionary would come back
// shorter and lose its value.
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

// Deep trees are walked with a worklist, not the call stack

// The deepest tree, in nested containers counting the root, that cyaml_clone()
// and the serializers accept; the same as the parser's limit. The deep-walk
// tests build trees at and past it. It is a literal so it doesn't move with the
// constant it tests.
#define CYAML_TEST_MAX_WALK_LEVELS 500

// Builds a chain of levels nested containers through mp, alternating list and
// dictionary, so the deep-walk tests drive both kinds of traversal frame.
// Gives NULL and leaves nothing behind when an allocation fails; each failed
// attach already took ownership of the child, so only root is released.
static cyaml cyaml_test_build_deep_chain(size_t levels,
                                         ccol_memmgmt_procs *mp) {
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
    ccol_retval r = (cyaml_type(cur) == CYAML_LIST)
                        ? cyaml_list_push(cur, next)
                        : cyaml_dictionary_set(cur, "n", next);
    if (r != ccol_success) {
      cyaml_destroy(root);
      return NULL;
    }
    cur = next;
  }
  return root;
}

// A single-fault allocator for the deep-walk sweeps. These fail only the call
// whose index (counted in g_walk_call_idx) is g_walk_fail_at, and keep the
// number of live blocks in g_walk_live, so a walk that leaks on a failure
// path fails an assertion.
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
static ccol_memmgmt_procs g_walk_fault_mp = {.malloc = walk_fault_malloc,
                                             .calloc = walk_fault_calloc,
                                             .realloc = walk_fault_realloc,
                                             .free = walk_fault_free};

// The tree a worker walks and whether each walk succeeded.
typedef struct {
  cyaml tree;
  bool clone_ok;
  bool block_ok;
  bool flow_ok;
} cyaml_deep_walk_args_t;

// The worker of the small-stack walk test: Clones and serializes a->tree in
// both styles and records in *a which walks succeeded.
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

// The clone and the serializers walk with an explicit stack, so a tree at the
// cap runs on a 64 KiB thread stack. A recursive walk needs more and dies
// there.
TEST(deep_trees, a_tree_at_the_depth_cap_is_walked_on_a_small_thread_stack) {
  cyaml_deep_walk_args_t args = {NULL, false, false, false};
  args.tree = cyaml_test_build_deep_chain(CYAML_TEST_MAX_WALK_LEVELS, NULL);
  REQUIRE_NE((void *)args.tree, NULL);

  size_t stack_bytes = 64u * 1024u;
  if (stack_bytes < (size_t)PTHREAD_STACK_MIN)
    stack_bytes = (size_t)PTHREAD_STACK_MIN;

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

// Fails every allocation of a deep clone and serialization in turn, the stack
// growth included, and requires NULL with nothing leaked.
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
  REQUIRE_EQ(max_excess, 0L);
  REQUIRE_EQ(final_live, 0L);
}

// Parsing is driven by a worklist, not the call stack

// The deepest nesting a document may have before cyaml_parse*() refuses it,
// for the deep-parse tests that build documents at and past it. It is a
// literal so it doesn't move with the library constant.
#define CYAML_TEST_MAX_PARSE_LEVELS 500

// Builds levels nested flow sequences, "[[[ ... ]]]", for the deep-parse
// tests. The caller frees it.
static char *cyaml_test_deep_flow_doc(size_t levels) {
  char *s = (char *)malloc(levels * 2 + 2);
  if (!s) return NULL;
  for (size_t i = 0; i < levels; i++) s[i] = '[';
  for (size_t i = 0; i < levels; i++) s[levels + i] = ']';
  s[levels * 2] = '\n';
  s[levels * 2 + 1] = '\0';
  return s;
}

// Builds levels nested block mappings, each one column further in, for the
// deep-parse tests. The caller frees it.
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

// The document a worker parses and what the parse did.
typedef struct {
  const char *doc;
  bool parsed;
  bool saw_depth_error;
} cyaml_deep_parse_args_t;

// The worker of cyaml_run_parse_on_small_stack(): Parses a->doc and records in
// *a whether it parsed and whether it failed on the nesting depth.
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

// Runs one parse of doc on a thread with a 64 KiB stack, for the tests that a
// deep parse needs only a constant native stack. Stores whether it parsed and
// whether it failed on the depth in the out-parameters. Returns false when
// the thread couldn't run.
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

// The parse is driven by an explicit stack of frames, so a document at the cap
// parses on a 64 KiB thread stack and one level more is refused with the
// documented error. A recursive parse would overrun the stack first.
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
  REQUIRE_TRUE(past_depth_err);
  REQUIRE_FALSE(at_depth_err);
}

// A parse that fails deep down leaves one open container per level with no
// root, and the parser must release all of them.
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

  REQUIRE_FALSE(any_parsed);
  REQUIRE_EQ(max_live, 0L);
  REQUIRE_EQ(final_live, 0L);
}

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

// An entry that starts with '[', '{', '*' or a property is first read as a
// possible key, then re-read as something else when no ':' follows. Each pair
// differs only in that ':', so it pins both readings of the same bytes.
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

// Builds outer nested block mappings whose innermost key is an anchored flow
// sequence inner levels deep, for the test that a key speculation shares the
// nesting budget. The ':' after it makes the speculation succeed, so the
// content is read only once. The caller frees it.
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

// The nested traversal of a key speculation has its own frame stack but
// shares the parse's nesting budget, so a document is refused exactly when
// outer + inner passes the cap.
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

// Growable buffer read guards

// Every helper that reads back through the growable buffer (the three
// trailing-white-space trims, the deferred newline of the block scalar
// emitter, the serializer's final newline test) checks for a backing store
// itself. Each gets a buffer with the out-of-memory flag set and no store, and
// must not dereference it.
TEST(cyaml_growbuf_guards, every_backing_store_read_tests_the_buffer_itself) {
  REQUIRE_EQ(cyaml_debug_growbuf_guard_bits(), 7u);
}

// Every frame gives back its nesting level when it finishes or is abandoned,
// so three sibling values of 400 levels each all parse.
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

// Raw control bytes in an anchor name or a tag token

// Anchor names and tags are stored as NUL-terminated strings, so a raw NUL
// would cut them short. Two anchors that differ only after a NUL would become
// one, and the alias would resolve to the wrong node. Only cyaml_parse_n() can
// bring such a byte in.
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

// Accepting the byte would store the empty tag, which serializes as "!<>" and
// doesn't parse back.
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

// Other C0 bytes and DEL can't cut a string short, but they are outside
// c-printable and refused like everywhere else.
TEST(nul_in_tokens, other_control_bytes_are_rejected_in_anchors_and_tags) {
  const char controls[] = {0x01, 0x08, 0x0b, 0x0c, 0x1f, 0x7f};
  for (size_t i = 0; i < sizeof(controls); i++) {
    char anchor_doc[] = "x: &aXb 1\n";
    anchor_doc[5] = controls[i];
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

TEST(nul_in_tokens, every_parsed_document_round_trips_through_both_emitters) {
  static const char *const docs[] = {
      "! ~",
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

    cyaml_serialize_free(block);
    cyaml_serialize_free(flow);
    cyaml_destroy(root);

    REQUIRE_TRUE(block_ok);
    REQUIRE_TRUE(flow_ok);
  }
}

// cyaml_set: Every accepted C value type, arrays included

// A pointer that is const itself, at file scope. cyaml_set() copies its value
// into a decayed, unqualified local, so such a pointer and a named char array
// both arrive as a plain pointer to the string.
static const char *const _cyaml_set_types_file_scope_name = "prod";

TEST(set_value_types, string_literal) {
  cyaml root = cyaml_create_dictionary();
  ccol_retval r = cyaml_set(root, "k", "champion");
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
  ccol_retval r = cyaml_set(root, "k", buf);
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
  ccol_retval r = cyaml_set(root, "k", cbuf);
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
  ccol_retval r1 = cyaml_set(root, "c", p_const);
  ccol_retval r2 = cyaml_set(root, "t", p_top_const);
  ccol_retval r3 = cyaml_set(root, "b", p_both);
  ccol_retval r4 = cyaml_set(root, "f", _cyaml_set_types_file_scope_name);
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
  ccol_retval r1 = cyaml_set(root, "a", (bool)false);
  ccol_retval r2 = cyaml_set(root, "b", cb);
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
  ccol_retval r[5];
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
  ccol_retval r1 = cyaml_set(root, "f", f);
  ccol_retval r2 = cyaml_set(root, "d", d);
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
  ccol_retval r = cyaml_set(root, "k", NULL);
  cyaml leaf = cyaml_get(root, "k");
  bool ok = leaf && cyaml_type(leaf) == CYAML_NULL;
  cyaml_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, const_pointer_round_trips_through_serialize) {
  char *err = NULL;
  cyaml doc = cyaml_parse("name: dev\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  ccol_retval r = cyaml_set(doc, "name", _cyaml_set_types_file_scope_name);
  char *out = cyaml_serialize(doc);
  bool ok = out && strcmp(out, "name: prod\n") == 0;
  if (out) cyaml_serialize_free(out);
  cyaml_destroy(doc);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

// Shared helpers: Deep comparison and a serialize round trip

// Returns the tag of n that a round trip keeps: A custom tag, or NULL for a
// core tag, which the serializer leaves out because implicit typing restores
// it. _rt_deep_equal() compares tags through it.
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

// Tells whether two trees are equal in types, custom tags, values (NaN equals
// NaN), list order and dictionary members. The round-trip and walk tests
// compare trees with it.
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

// Tells whether doc survives a block and a flow serialization, each parsed
// back into a tree deep-equal to doc; the core-tag tests check every edited
// tree with it.
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

// Float dictionary keys: Shortest round-trip text

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

// Core-schema tags stay consistent with the type of a node

// The core tags and, at the same index, the node type each one names, so the
// core-tag tests can try every tag on every type.
static const char *const _ct_core_tags[] = {
    CYAML_TAG_NULL, CYAML_TAG_BOOL, CYAML_TAG_INT, CYAML_TAG_FLOAT,
    CYAML_TAG_STR,  CYAML_TAG_SEQ,  CYAML_TAG_MAP};
static const cyaml_node_type _ct_core_tag_types[] = {
    CYAML_NULL,   CYAML_BOOL, CYAML_INTEGER,   CYAML_FLOAT,
    CYAML_STRING, CYAML_LIST, CYAML_DICTIONARY};
// The number of core tags, the bound of the loops over the two arrays above.
#define _CT_N 7

// Builds a small node of the given type, so the core-tag tests can try every
// tag on every type. NULL when memory runs out.
static cyaml _ct_make_node(cyaml_node_type type) {
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
      ccol_retval r = cyaml_node_set_tag(node, _ct_core_tags[ti]);
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
    ccol_retval r1 = cyaml_node_set_tag(node, "!custom");
    ccol_retval r2 = cyaml_node_set_tag(node, "tag:yaml.org,2002:binary");
    ccol_retval r3 = cyaml_node_set_tag(node, "tag:yaml.org,2002:");
    cyaml_dictionary_set(doc, "v", node);
    bool rt = _rt_round_trips(doc);
    if (r1 != ccol_success || r2 != ccol_success || r3 != ccol_success || !rt)
      wrong++;
    cyaml_destroy(doc);
  }
  REQUIRE_EQ(wrong, (size_t)0);
}

// Source text of one node of each type, each core tag naming its own type,
// and the same with a custom tag. The set test edits each of them.
static const char *const _ct_tagged_sources[] = {
    "v: !!null ~\n",     "v: !!bool true\n",   "v: !!int 8080\n",
    "v: !!float 1.5\n",  "v: !!str x\n",       "v: !!seq [1]\n",
    "v: !!map {a: 1}\n", "v: !custom 8080\n",  "v: !custom x\n",
    "v: !custom [1]\n",  "v: !custom {a: 1}\n"};

// Applies the cyaml_set() of kind k (bool, int, float, string, null) to the
// leaf "v" of doc, so the core-tag test can change each tagged node in every
// way. Returns what cyaml_set() returns.
static ccol_retval _ct_apply_set(cyaml doc, int k) {
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
// The type that each kind of _ct_apply_set() gives the leaf, which the set
// test compares with the tag that survives.
static const cyaml_node_type _ct_set_types[] = {
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
      char before[64] = "";
      const char *tag0 = cyaml_node_tag(cyaml_get(doc, "v"));
      if (tag0) snprintf(before, sizeof(before), "%s", tag0);
      bool was_custom = before[0] == '!';
      ccol_retval r = _ct_apply_set(doc, k);
      cyaml v = cyaml_get(doc, "v");
      const char *after = v ? cyaml_node_tag(v) : NULL;
      bool ok = r == ccol_success && v && cyaml_type(v) == _ct_set_types[k];
      if (ok && was_custom) {
        ok = after && strcmp(after, "!custom") == 0;
      } else if (ok && after) {
        size_t ti = 0;
        while (ti < _CT_N && strcmp(after, _ct_core_tags[ti]) != 0) ti++;
        ok = ti < _CT_N && _ct_core_tag_types[ti] == cyaml_type(v);
      } else if (ok) {
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
  ccol_retval r1 = cyaml_set(doc, "port", 9090);
  ccol_retval r2 = cyaml_set(doc, "name", "y");
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
  ccol_retval r = cyaml_set(doc, "port", "auto");
  char *out = cyaml_serialize(doc);
  bool ok = out && strcmp(out, "port: auto\n") == 0;
  if (out) cyaml_serialize_free(out);
  cyaml_destroy(doc);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

// Flow collection closed by an indicator at the indent of its parent

// Parses src and tells whether its tree deep-equals the parse of want, which
// spells the same content on one line; the flow-closing-indent tests compare
// each layout with it. With want NULL the parse of src must fail.
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

// Duplicate keys: The last value wins and nothing leaks

// A counting allocator for the duplicate-key tests, which check that every
// block they caused is freed again. These forward to the C library and keep
// the number of live blocks in _dup_live.
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
static ccol_memmgmt_procs _dup_mp = {.malloc = _dup_malloc,
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
  ccol_retval r1 = cyaml_dictionary_set(root, "k", first);
  ccol_retval r2 =
      cyaml_dictionary_set(root, "k", cyaml_create_int_mp(7, &_dup_mp));
  ccol_retval r3 = cyaml_dictionary_set(root, "k", cyaml_get(root, "k"));
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
  ccol_retval r = cyaml_dictionary_set(root, "b", cyaml_get(root, "a"));
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

// Set of a new key: A failed build leaves no placeholder behind

// A budget allocator for the failed-set tests. These refuse every allocation
// once _ff_left (decremented on each call) reaches zero, and keep the number
// of live blocks in _ff_live. With _ff_left at -1 they never refuse.
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
static ccol_memmgmt_procs _ff_mp = {.malloc = _ff_malloc,
                                    .calloc = _ff_calloc,
                                    .realloc = _ff_realloc,
                                    .free = _ff_free};

// Refuses each allocation of a set that creates a new string leaf in turn.
// The last one runs after the key is already in the map, so the set must take
// the key out again; the size check catches a key left with a NULL child.
TEST(set_new_key, a_failed_build_removes_the_key_and_leaks_nothing) {
  _ff_left = -1;
  _ff_live = 0;
  cyaml root = cyaml_create_dictionary_mp(&_ff_mp);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval pre = cyaml_set(root, "keep", 1);
  long live_before = _ff_live;

  size_t failures = 0, wrong = 0;
  ccol_retval r = ccol_not_enough_memory;
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

// Numbers under a comma-decimal locale

// Opens the LC_NUMERIC locale name for _comma_locale_open(). Returns it when
// its decimal point is ',', else frees it and returns (locale_t)0.
static locale_t _comma_locale_try(const char *name) {
  locale_t loc = newlocale(LC_NUMERIC_MASK, name, (locale_t)0);
  if (!loc) return (locale_t)0;
  const char *radix = nl_langinfo_l(RADIXCHAR, loc);
  if (radix && strcmp(radix, ",") == 0) return loc;
  freelocale(loc);
  return (locale_t)0;
}

// Finds a locale whose decimal point is ',' for the comma-locale tests, which
// switch the thread to it with uselocale(). Returns it, or (locale_t)0 after
// printing why the test is skipped. It tries the usual installed names, then
// the private de_DE.UTF-8 that the Makefile builds into tests_locale/. That
// one is loaded through setlocale() and copied with duplocale(), because
// glibc's newlocale() leaks its LOCPATH search path. It sets LOCPATH (glibc
// only, the one C library that reads it) and the global LC_NUMERIC only for
// that attempt and puts both back.
static locale_t _comma_locale_open(void) {
  static const char *const names[] = {
      "de_DE.UTF-8", "de_DE.utf8",  "de_DE",       "fr_FR.UTF-8",
      "fr_FR.utf8",  "nl_NL.UTF-8", "es_ES.UTF-8", "it_IT.UTF-8",
      "ru_RU.UTF-8", "pt_BR.UTF-8"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    locale_t loc = _comma_locale_try(names[i]);
    if (loc) return loc;
  }
  locale_t loc = (locale_t)0;
#if defined(__GLIBC__)
  const char *old_path = getenv("LOCPATH");
  char *saved_path = old_path ? strdup(old_path) : NULL;
  setenv("LOCPATH", "tests_locale", 1);
#endif
  const char *old_numeric = setlocale(LC_NUMERIC, NULL);
  char *saved_numeric = old_numeric ? strdup(old_numeric) : NULL;
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
#if defined(__GLIBC__)
  if (saved_path) {
    setenv("LOCPATH", saved_path, 1);
    free(saved_path);
  } else {
    unsetenv("LOCPATH");
  }
#endif
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
  bool float_key = d && cyaml_dictionary_get(d, "2.5") != NULL;
  bool tagged_key = d && cyaml_dictionary_get(d, "7") != NULL;
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

// Tags on implicit block mapping keys

// A tag types an implicit block key exactly as it types the same key in a
// flow mapping or after "? ", and the key is stored as its canonical text.
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
  cyaml custom = cyaml_parse("!x 0x10: v\n! 0x11: w\n", &err);
  bool custom_ok = custom && cyaml_dictionary_get(custom, "16") != NULL &&
                   cyaml_dictionary_get(custom, "0x11") != NULL;
  cyaml_destroy(custom);
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

// Speculative key parse: A value is never treated as a key

// Builds "<prefix>[item00000, item00001, ...]<suffix>" with n items, a flow
// collection longer than the canonical key limit for the speculative key
// tests. The caller frees it.
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

// A one-line flow value may be longer than the canonical text limit of a key,
// because the parser computes that text only for a real key.
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

TEST(errors, long_one_line_flow_key_still_rejected) {
  char *doc = _big_flow_doc("", "", 8000);
  REQUIRE_NE((void *)doc, NULL);
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

// A counting allocator for the alias copy tests. These forward to the C
// library and count every call in _alias_allocs.
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
static ccol_memmgmt_procs _alias_mp = {.malloc = _alias_malloc,
                                       .calloc = _alias_calloc,
                                       .realloc = _alias_realloc,
                                       .free = free};

// Builds an anchor of 40 strings followed by n "- *a" entries, so the alias
// copy test can compare the cost of two list lengths. The caller frees it.
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

// Parses doc through _alias_mp into *out and returns how many allocations the
// parse made. Resets _alias_allocs first.
static size_t _allocs_to_parse(const char *doc, cyaml *out) {
  char *err = NULL;
  _alias_allocs = 0;
  *out = cyaml_parse_mp(doc, &err, &_alias_mp);
  return _alias_allocs;
}

// "- *a" is a value, so the parser copies the anchored node once for it and
// never again to try it as a key, and charges each copy to the budgets.
TEST(block_sequence, alias_entry_is_copied_once) {
  char *five = _alias_list_doc(5);
  char *six = _alias_list_doc(6);
  cyaml d5 = NULL, d6 = NULL;
  size_t a5 = five ? _allocs_to_parse(five, &d5) : 0;
  size_t a6 = six ? _allocs_to_parse(six, &d6) : 0;
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
  REQUIRE_GE(a6 - a5, one_copy);
  REQUIRE_LT(a6 - a5, one_copy + one_copy / 2);
}

// Raw control bytes in a %TAG prefix

// A raw NUL in a %TAG prefix would also cut the stored prefix, so "!e!int"
// would resolve to the core int tag.
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

// Keys longer than an implicit key may be

// Builds a dictionary with three keys made from base plus a suffix, holding a
// scalar, a dictionary and a list, for the long-key round-trip tests. NULL
// when memory runs out.
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

// Tells whether both serializers write the keys of _long_key_tree(base) in the
// explicit "? key" form exactly when expect_explicit is true (a rendered key
// longer than 1024 bytes), and whether the text parses back to the same tree.
// The long-key tests call it with keys just below and above the limit.
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
  plain_1023[1023] = '\0';
  memset(plain_1024, 'a', 1024);
  plain_1024[1024] = '\0';
  char escaped[601];
  memset(escaped, 0x01, 600);
  escaped[600] = '\0';
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

// cyaml_set of an unsigned value above LLONG_MAX

// No long long holds the value, so cyaml_set() stores the nearest double, as a
// parse of the literal does, and drops a core !!int tag that stops
// matching.
TEST(set_value_types, unsigned_above_llong_max_becomes_a_float) {
  char *err = NULL;
  cyaml doc = cyaml_parse("n: !!int 1\nm: 2\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  unsigned long long big = 18446744073709551615ULL;
  unsigned long long edge = (unsigned long long)LLONG_MAX;
  unsigned long long above = (unsigned long long)LLONG_MAX + 1ULL;
  ccol_retval r1 = cyaml_set(doc, "n", big);
  ccol_retval r2 = cyaml_set(doc, "m", edge);
  ccol_retval r3 = cyaml_set(doc, "fresh", above);
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

// The worker of the pool-key lock test: Parses and destroys a small document
// five times on a new thread. Sets *arg to 0 when a parse fails, else 1.
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

// A thread arms its pool key once, not on every refill of its pool. Re-arming
// on every refill would take the shared lock once per parse.
TEST(node_pool, a_thread_takes_the_pool_key_lock_once) {
  unsigned long before = atomic_load(&cyaml__pool_key_lock_count_for_tests);
  int ok = 0;
  pthread_t th;
  int created = pthread_create(&th, NULL, cyaml_pool_arm_worker, &ok);
  if (created == 0) pthread_join(th, NULL);
  unsigned long after = atomic_load(&cyaml__pool_key_lock_count_for_tests);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(after - before, 1ul);
}

// Serializer interoperability

// Serializes s as a list element and as a dictionary value in both styles and
// tells whether every output holds s double-quoted, holds no raw NEL, LS or
// PS, and parses back to exactly s. The interop tests run it on every string
// that YAML 1.1 readers would misread.
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

// Each string is a valid plain scalar in YAML 1.2 that PyYAML or libyaml reads
// differently or refuses: NEL, LS and PS are YAML 1.1 line breaks, a tab ends
// a plain scalar, '?' is a flow indicator and a lone "=" is the YAML 1.1
// "value" type.
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
  cyaml l = cyaml_create_list();
  cyaml_list_push(l,
                  cyaml_create_string("x\xC2\x85y\xE2\x80\xA8z\xE2\x80\xA9w"));
  char *out = cyaml_serialize_flow(l);
  bool ok = out && strcmp(out, "[\"x\\Ny\\Lz\\Pw\"]") == 0;
  cyaml_serialize_free(out);
  cyaml_destroy(l);
  REQUIRE_TRUE(ok);
}

// Tells whether out holds a raw C1 control other than NEL, or a raw U+FFFE or
// U+FFFF, which the interop tests forbid in serializer output.
static bool _ser_has_raw_non_printable(const char *out) {
  for (const unsigned char *p = (const unsigned char *)out; *p; p++) {
    if (p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F && p[1] != 0x85)
      return true;
    if (p[0] == 0xEF && p[1] == 0xBF && (p[2] == 0xBE || p[2] == 0xBF))
      return true;
  }
  return false;
}

// libyaml, PyYAML and go-yaml refuse a document holding one of these raw, so
// each goes out double-quoted with the code point escaped, as a value and as a
// key, in both styles.
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

// libyaml and PyYAML refuse a literal '#' in a verbatim tag, so the
// serializer writes %23.
TEST(serialize_interop, hash_in_verbatim_tag_is_percent_escaped) {
  cyaml s = cyaml_create_string("v");
  ccol_retval rv = s ? cyaml_node_set_tag(s, "tag:example.com,2000:app#frag")
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

// Stream serialization

// A stream of three Kubernetes manifests, which the stream serialization
// tests parse, edit and write back.
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
  char *err = NULL;
  cyaml docs = cyaml_parse(_k8s_stream, &err);
  ccol_retval set_rc =
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

// Byte order mark inside a document

TEST(bom_inside_document, is_refused) {
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

// Speculative flow-collection key parse

// Tells whether the flow serialization of the parse of y is exactly want, a
// compact way for the speculation and BOM tests to pin a whole tree.
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

// A decorated flow collection is read once as a possible key before it is
// read as a value. An alias inside it resolves to the anchor defined before
// it either way; a key reading that left its anchors behind would give
// [B, B] instead of [A, B].
TEST(anchor_speculation,
     alias_in_a_decorated_flow_value_sees_only_earlier_anchors) {
  REQUIRE_TRUE(_flow_text_is("- &x A\n- &k [*x, &x B]\n", "[A, [A, B]]"));
  REQUIRE_TRUE(_flow_text_is("- &x A\n- !!seq [*x, &x B]\n", "[A, [A, B]]"));
  REQUIRE_TRUE(_flow_text_is("- &x A\n- !!seq &k [*x, &x B]\n", "[A, [A, B]]"));
  REQUIRE_TRUE(_flow_text_is("x: &x A\ny:\n  !!map {a: *x, b: &x B}\n",
                             "{x: A, \"y\": {a: A, b: B}}"));
  REQUIRE_TRUE(
      _flow_text_is("x: &x A\ny:\n  &k [*x, &x B]\n", "{x: A, \"y\": [A, B]}"));
  REQUIRE_TRUE(_flow_text_is("- &x A\n- &k [*x,\n    &x B]\n", "[A, [A, B]]"));
  REQUIRE_TRUE(
      _flow_text_is("- &x A\n- &k [*x, &x B]\n- *k\n", "[A, [A, B], [A, B]]"));
  REQUIRE_TRUE(
      _flow_text_is("- &x A\n- &k [&x B, C]\n- *x\n", "[A, [B, C], B]"));
}

// When the collection is a key, its anchors stay and a later definition of
// the name wins after it.
TEST(anchor_speculation, anchors_of_an_accepted_flow_key_stay) {
  REQUIRE_TRUE(_flow_text_is("- &x A\n- &k [*x, &x B]: v\n- *x\n",
                             "[A, {\"[A, B]\": v}, B]"));
  REQUIRE_TRUE(_flow_text_is("- &x A\n- !!seq [&y C]: v\n- *y\n",
                             "[A, {\"[C]\": v}, C]"));
}

TEST(anchor_speculation, an_anchor_of_the_refused_reading_is_not_visible) {
  char *err = NULL;
  cyaml doc = cyaml_parse("- &k [*z, &z B]\n", &err);
  bool unknown = err && strstr(err, "unknown alias '*z'") != NULL;
  cyaml_destroy(doc);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_TRUE(unknown);
}

// The error gives the offset of the alias's '*', not the byte after its
// name.
TEST(anchor_speculation, an_unknown_alias_is_reported_where_it_starts) {
  static const struct {
    const char *doc;
    const char *want;
  } cases[] = {
      {"a: *undefined\n", "unknown alias '*undefined' at position 3"},
      {"a: 1\nb: *x\n", "unknown alias '*x' at position 8"},
      {"*k : v\n", "unknown alias '*k' at position 0"},
      {"[1, *zz]\n", "unknown alias '*zz' at position 4"},
  };
  size_t wrong = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(cases[i].doc, &err);
    if (doc != NULL || err == NULL || strcmp(err, cases[i].want) != 0) {
      fprintf(stderr, "case %zu: got \"%s\"\n", i, err ? err : "(null)");
      wrong++;
    }
    cyaml_destroy(doc);
  }
  REQUIRE_EQ(wrong, (size_t)0);
}

// A tagged flow list costs the nodes of the untagged list plus the tag's
// bytes, so the refused key reading charges nothing twice.
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

// Node pool across thread-exit destructors

// An application key whose destructor destroys a document after the pool of
// the exiting thread has already drained.
static pthread_key_t _late_doc_key;

// The destructor of _late_doc_key: Destroys the document stored there, after
// the exiting thread's pool has already drained.
static void _late_doc_dtor(void *p) {
  cyaml doc = (cyaml)p;
  cyaml_destroy(doc);
}

// The worker of the late-destructor test: Stores a parsed document under
// _late_doc_key so its destructor frees nodes after the pool drained. *arg is
// set to 1 when that worked. The key gets a placeholder before the pool key is
// armed, so the pool drains first on glibc (key order) and on OpenBSD (reverse
// order of first value).
static void *_late_doc_worker(void *arg) {
  int *ok = (int *)arg;
  static char placeholder;
  if (pthread_setspecific(_late_doc_key, &placeholder) != 0) return NULL;
  cyaml w = cyaml_create_null();
  cyaml_destroy(w);
  cyaml d = cyaml_parse("a: [1, 2, 3]\nb: x\n", NULL);
  *ok = d != NULL && pthread_setspecific(_late_doc_key, d) == 0;
  if (!*ok) {
    cyaml_destroy(d);
    (void)pthread_setspecific(_late_doc_key, NULL);
  }
  return NULL;
}

// The first free after the pool drained at thread exit arms the pool key
// again, so one more destructor round drains those nodes too. The test counts
// the two arms; memtest reports the nodes as lost if the second drain doesn't
// run.
TEST(node_pool, nodes_freed_by_a_later_thread_destructor_are_drained) {
  cyaml w = cyaml_create_null();
  cyaml_destroy(w);
  int key_rc = pthread_key_create(&_late_doc_key, _late_doc_dtor);
  unsigned long before = atomic_load(&cyaml__pool_key_lock_count_for_tests);
  int ok = 0;
  int created = -1;
  pthread_t th;
  if (key_rc == 0) {
    created = pthread_create(&th, NULL, _late_doc_worker, &ok);
    if (created == 0) pthread_join(th, NULL);
    pthread_key_delete(_late_doc_key);
  }
  unsigned long after = atomic_load(&cyaml__pool_key_lock_count_for_tests);
  REQUIRE_EQ(key_rc, 0);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(after - before, 2ul);
}

// Size and charge of scalar text

// A tracking allocator for the scalar text tests, which compare the bytes a
// document holds. These keep the number of bytes they hold in _held_bytes,
// through a size header in front of each block.
static size_t _held_bytes;
// The size header in front of each block, aligned for any object.
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
static ccol_memmgmt_procs _held_mp = {.malloc = _held_malloc,
                                      .calloc = _held_calloc,
                                      .realloc = _held_realloc,
                                      .free = _held_free};

// Parses a flow list of n copies of item through _held_mp and gives the bytes
// the document holds and the bytes the parse charged, so the scalar text test
// can compare styles. False when the parse fails or gives a list of another
// length.
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

// Quoted and block scalar text is kept at its own length and charged like
// plain text, so the same strings cost the same in every style.
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
  REQUIRE_LT(tag_held, plain_held + n * 32);
  REQUIRE_GT(tag_charged, plain_charged);
}

// Parses a block list of n copies of item through _held_mp and gives the bytes
// the document holds, for the block scalar size test. False when the parse
// fails.
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

// Merge keys and the document limits

// A merge stopped by the document memory limit names that limit, like an
// alias does. The byte floor sits three quarters of the way between the cost
// without and with the last line, so the alias copy fits and the merge copy
// is what the limit stops.
TEST(merge_keys, a_merge_stopped_by_the_document_limit_names_it) {
  char doc[4096];
  size_t len = (size_t)snprintf(
      doc, sizeof(doc),
      "m0: &m0 {k0: \"%s\", k1: \"%s\", k2: \"%s\", k3: \"%s\"}\n",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy",
      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy");
  for (int lvl = 1; lvl < 4; lvl++) {
    len +=
        (size_t)snprintf(doc + len, sizeof(doc) - len, "m%d: &m%d {", lvl, lvl);
    for (int q = 0; q < 8; q++)
      len += (size_t)snprintf(doc + len, sizeof(doc) - len, "%sq%d: {<<: *m%d}",
                              q ? ", " : "", q, lvl - 1);
    len += (size_t)snprintf(doc + len, sizeof(doc) - len, "}\n");
  }
  size_t prefix_len = len;
  len += (size_t)snprintf(doc + len, sizeof(doc) - len, "z: {<<: *m3}\n");
  REQUIRE_LT(len, sizeof(doc));

  char *err = NULL;
  doc[prefix_len] = '\0';
  cyaml y = cyaml_parse(doc, &err);
  size_t prefix_cost = cyaml_test_last_parse_bytes_charged;
  cyaml_destroy(y);
  doc[prefix_len] = 'z';
  y = cyaml_parse(doc, &err);
  size_t full_cost = cyaml_test_last_parse_bytes_charged;
  bool both_parsed = y != NULL && prefix_cost < full_cost;
  cyaml_destroy(y);
  REQUIRE_TRUE(both_parsed);

  size_t saved = cyaml_test_parse_byte_floor;
  cyaml_test_parse_byte_floor = prefix_cost + (full_cost - prefix_cost) / 4 * 3;
  y = cyaml_parse(doc, &err);
  bool named = err && strstr(err, "merge key expansion exceeded") &&
               strstr(err, "document memory limit");
  cyaml_destroy(y);
  cyaml_test_parse_byte_floor = saved;
  REQUIRE_EQ((void *)y, NULL);
  REQUIRE_TRUE(named);
}

// A "<<" key whose tag is the core merge tag is a merge key in every spelling
// and key form, while any other tag keeps it an ordinary key.
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

// Verbatim tag output

// Every byte that ns-uri-char doesn't allow is percent-escaped, so every YAML
// reader accepts the tag and it parses back the same.
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

// The allocator procs struct of the caller

// The honest allocator of the procs-lifetime tests. These forward to the C
// library, count every call in _pl_calls and keep the number of live blocks in
// _pl_live.
static atomic_long _pl_live;
static atomic_long _pl_calls;
// The poison allocator, which a procs-lifetime test writes over the caller's
// struct once the tree exists. These forward to the C library and count every
// call in _pl_poison_calls; any call means the library read the caller's
// struct after the call that took it.
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

// A tree keeps using the allocator the struct named when the call ran, even
// after the caller overwrote the struct (a struct on the stack of a returned
// helper is the same case).
TEST(procs_lifetime, tree_does_not_read_the_procs_struct_of_the_caller_later) {
  const ccol_memmgmt_procs honest = {_pl_malloc, _pl_free, _pl_calloc,
                                     _pl_realloc};
  const ccol_memmgmt_procs poison = {_pl_poison_malloc, _pl_poison_free,
                                     _pl_poison_calloc, _pl_poison_realloc};
  ccol_memmgmt_procs *mp = malloc(sizeof(*mp));
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
  ccol_retval set_rv = ccol_invalid_args, push_rv = ccol_invalid_args,
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
  if (text) cyaml_serialize_free_mp(text, (ccol_memmgmt_procs *)&honest);
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

// Two structs with the same four functions are one allocator, so a child built
// from one can join a tree built from the other after the first is
// released.
TEST(procs_lifetime, procs_structs_with_the_same_functions_are_one_allocator) {
  ccol_memmgmt_procs first = {_pl_malloc, _pl_free, _pl_calloc, _pl_realloc};
  ccol_memmgmt_procs second = first;
  ccol_memmgmt_procs *a = NULL, *b = NULL;
  ccol_retval ra = ccol_procs_intern(&first, &a);
  ccol_retval rb = ccol_procs_intern(&second, &b);
  ccol_memmgmt_procs *again = NULL;
  ccol_retval rc = ccol_procs_intern(a, &again);
  ccol_memmgmt_procs *none = (ccol_memmgmt_procs *)&first;
  ccol_retval rn = ccol_procs_intern(NULL, &none);
  REQUIRE_EQ(ra, ccol_success);
  REQUIRE_EQ(rb, ccol_success);
  REQUIRE_EQ(rc, ccol_success);
  REQUIRE_EQ(rn, ccol_success);
  REQUIRE_NE((void *)a, (void *)&first);
  REQUIRE_EQ((void *)a, (void *)b);
  REQUIRE_EQ((void *)a, (void *)again);
  REQUIRE_EQ((void *)none, NULL);
}

// Once every slot of the fixed intern table is taken, a call that needs a new
// set of functions fails cleanly while a set with a slot keeps working.
TEST(procs_lifetime, a_full_procs_table_fails_cleanly) {
  ccol_memmgmt_procs known = {_pl_malloc, _pl_free, _pl_calloc, _pl_realloc};
  ccol_memmgmt_procs *known_i = NULL;
  ccol_retval rk = ccol_procs_intern(&known, &known_i);
  size_t keep = ccol__procs_intern_used_for_tests();
  size_t filled = 0;
  for (size_t i = keep; i < CCOL_PROCS_INTERN_CAPACITY; i++) {
    ccol_memmgmt_procs fake = {(ccol_malloc)(uintptr_t)(0x1000 + i * 16),
                               _pl_free, _pl_calloc, _pl_realloc};
    ccol_memmgmt_procs *out = NULL;
    if (ccol_procs_intern(&fake, &out) == ccol_success) filled++;
  }
  ccol_memmgmt_procs fresh = {_pl_poison_malloc, _pl_poison_free,
                              _pl_poison_calloc, _pl_poison_realloc};
  ccol_memmgmt_procs *fresh_i = (ccol_memmgmt_procs *)&known;
  ccol_retval rf = ccol_procs_intern(&fresh, &fresh_i);
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
  ccol__procs_intern_truncate_for_tests(keep);

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

// Under C11 `true` is the int 1, so cyaml_set() stores an integer, while
// (bool)true stores a bool. tests_c23.c pins the C23 side.
TEST(set_value_types, c11_true_is_an_int_and_a_cast_makes_a_bool) {
  cyaml doc = cyaml_parse("{\"i\": 0, \"b\": 0}", NULL);
  ccol_retval ri = doc ? cyaml_set(doc, "i", true) : ccol_not_enough_memory;
  ccol_retval rb =
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

// Stream character set: UTF-8 and c-printable (YAML 1.2 5.1)

// Tells whether every byte of s is printable ASCII, which every refusal of
// the character set tests must keep its message to.
static bool charset_msg_is_printable_ascii(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
  return true;
}

// Parses len bytes of doc and tells whether it is refused with a printable
// ASCII message holding every needle that isn't NULL. The character set tests
// use it for each bad byte sequence.
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

// Tells whether the len bytes of doc parse, the accepting half of the
// character set tests.
static bool charset_accepted(const char *doc, size_t len) {
  char *err = NULL;
  cyaml root = cyaml_parse_n(doc, len, &err);
  bool ok = root != NULL && err == NULL;
  if (!ok) fprintf(stderr, "charset_accepted: refused: %s\n", err);
  cyaml_destroy(root);
  return ok;
}

// Evaluates to the length of the string literal lit without its terminating
// NUL. The character set tests pass documents that hold NUL bytes, so they
// can't take the length with strlen().
#define CHARSET_LEN(lit) (sizeof(lit) - 1)

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

TEST(stream_charset, c_printable_edges_are_accepted) {
  static const char *const values[] = {
      "x\ty",
      "x\xC2\x85y",
      "x\xC2\xA0y",
      "x\xC3\xA9y",
      "x\xED\x9F\xBFy",
      "x\xEE\x80\x80y",
      "x\xEF\xBF\xBDy",
      "x\xEF\xBB\xBFy",
      "x\xF0\x90\x80\x80y",
      "x\xF4\x8F\xBF\xBFy",
      "x~y",
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

// Each template holds one '@' where the bad character goes, and parses when
// the '@' is a letter, so a refusal comes from the character alone.
TEST(stream_charset, every_context_of_the_stream_is_checked) {
  static const char *const templates[] = {
      "key: va@lue\n",
      "ke@y: value\n",
      "\"ke@y\": value\n",
      "key: \"va@lue\"\n",
      "key: 'va@lue'\n",
      "key: value # comm@ent\n",
      "# comm@ent\nkey: value\n",
      "key: !ta@g value\n",
      "key: !<tag:a@b> value\n",
      "key: &anc@hor value\nother: *anc@hor\n",
      "key: |\n  li@teral\n",
      "key: >\n  fol@ded\n",
      "[a, b@c, d]\n",
      "{a: b@c, d: e}\n",
      "- a\n- b@c\n",
      "a: 1\n---\nb: c@d\n",
      "a: 1\n...\n# c@d\n---\nb: 2\n",
      "%TAG !e! tag:ex@ample.com:\n--- !e!x 1\n",
      "%YAML 1.2 # c@d\n---\na: 1\n",
      "key:  @ \n",
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

TEST(stream_charset, message_gives_line_and_column) {
  REQUIRE_TRUE(charset_refused("a: 1\nb: 2\nccc: x\x01\n", 17,
                               "line 3, column 7", NULL, NULL));
  REQUIRE_TRUE(charset_refused("a: 1\r\nb: 2\r\nccc: x\x01\r\n", 20,
                               "line 3, column 7", NULL, NULL));
  REQUIRE_TRUE(charset_refused("a: 1\rb: 2\rc: \x02\r", 15, "line 3, column 4",
                               NULL, NULL));
  REQUIRE_TRUE(charset_refused("k: \xC3\xA9\xC3\xA9\xE2\x82\xAC\xFF\n", 15,
                               "line 1, column 7", "0xff", NULL));
  REQUIRE_TRUE(
      charset_refused("\xEF\xBB\xBF"
                      "a: \x01\n",
                      9, "line 1, column 4", NULL, NULL));
  REQUIRE_TRUE(charset_refused("\x01", 1, "line 1, column 1", NULL, NULL));
  REQUIRE_TRUE(charset_refused("a: \"\xC2\x85\x01\"\n", 9, "line 1, column 6",
                               NULL, NULL));
}

// The check reads sixteen-byte blocks, so the bad byte is put at every offset
// of a block and on both sides of the last whole block.
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

TEST(stream_charset, parse_n_checks_up_to_the_last_byte_and_no_further) {
  static const char tail_nul[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: x\0";
  REQUIRE_TRUE(charset_refused(tail_nul, CHARSET_LEN(tail_nul),
                               "raw control character 0x00", "line 5, column 5",
                               NULL));
  static const char tail_cut[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: \xE2\x82";
  REQUIRE_TRUE(charset_refused(tail_cut, CHARSET_LEN(tail_cut),
                               "truncated by the end of the input",
                               "line 5, column 4", NULL));
  static const char euro[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: \xE2\x82\xAC";
  REQUIRE_TRUE(charset_refused(euro, CHARSET_LEN(euro) - 1,
                               "truncated by the end of the input", NULL,
                               NULL));
  REQUIRE_TRUE(charset_accepted(euro, CHARSET_LEN(euro)));
  static const char past[] = "a: 1\nb: 2\nc: 3\nd: 4\ne: 5\x01\xFF";
  REQUIRE_TRUE(charset_accepted(past, CHARSET_LEN(past) - 2));
  REQUIRE_TRUE(
      charset_refused(past, CHARSET_LEN(past) - 1, "0x01", NULL, NULL));
}

// An escape names a character with c-printable text, so it is accepted where
// the raw character is refused, and the tree serializes it as an escape
// again.
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

// API strings: Well-formed UTF-8 is required

// Byte strings that aren't UTF-8, one of each kind of defect, which the API
// UTF-8 tests pass to every call that takes a string.
static const char *const api_bad_utf8[] = {
    "a\x80",        "\xC3",         "x\xC3(",           "\xC0\xAF",
    "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF",
    "ok\xE2\x82",
};
// The number of entries of api_bad_utf8, the bound of the API UTF-8 tests'
// loops.
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

// Every Unicode scalar value is accepted through the API, including the ones a
// document may only hold as an escape.
TEST(api_utf8, characters_that_need_an_escape_are_accepted_and_round_trip) {
  static const char *const values[] = {
      "\x01\x02\x1F",         "a\177b",           "\xC2\x80\xC2\x9F",
      "\xEF\xBF\xBE",         "\xEF\xBF\xBF",     "tab\tlf\ncr\r",
      "\xC2\x85\xE2\x80\xA8", "\xF4\x8F\xBF\xBF",
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    cyaml doc = cyaml_create_dictionary();
    ccol_retval rk = doc ? cyaml_dictionary_set(doc, values[i],
                                                cyaml_create_string(values[i]))
                         : ccol_not_enough_memory;
    ccol_retval rt = doc ? cyaml_node_set_tag(
                               cyaml_dictionary_get(doc, values[i]), values[i])
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

TEST(api_utf8, set_refuses_ill_formed_string_value_and_changes_nothing) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml doc = cyaml_parse("{a: old, n: 5, l: [x]}", NULL);
    const char *bad = api_bad_utf8[i];
    ccol_retval r1 = doc ? cyaml_set(doc, "a", bad) : ccol_not_enough_memory;
    ccol_retval r2 = doc ? cyaml_set(doc, "n", bad) : ccol_not_enough_memory;
    ccol_retval r3 = doc ? cyaml_set(doc, "new", bad) : ccol_not_enough_memory;
    ccol_retval r4 = doc ? cyaml_set(doc, "l.#0", bad) : ccol_not_enough_memory;
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

TEST(api_utf8, set_refuses_ill_formed_leaf_key) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml doc = cyaml_parse("{a: {b: 1}}", NULL);
    char path[32];
    snprintf(path, sizeof(path), "a.%s", api_bad_utf8[i]);
    ccol_retval r1 = doc ? cyaml_set(doc, path, 7) : ccol_not_enough_memory;
    ccol_retval r2 =
        doc ? cyaml_set(doc, api_bad_utf8[i], "v") : ccol_not_enough_memory;
    bool unchanged = doc && cyaml_dictionary_size(doc) == 1 &&
                     cyaml_dictionary_size(cyaml_get(doc, "a")) == 1;
    cyaml_destroy(doc);
    REQUIRE_EQ(r1, ccol_invalid_args);
    REQUIRE_EQ(r2, ccol_invalid_args);
    REQUIRE_TRUE(unchanged);
  }
}

TEST(api_utf8, dictionary_set_refuses_ill_formed_key_and_leaves_child) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml map = cyaml_create_dictionary();
    cyaml child = cyaml_create_string("v");
    ccol_retval r = (map && child)
                        ? cyaml_dictionary_set(map, api_bad_utf8[i], child)
                        : ccol_not_enough_memory;
    ccol_retval again = r == ccol_invalid_args
                            ? cyaml_dictionary_set(map, "ok", child)
                            : ccol_not_enough_memory;
    bool placed = map && cyaml_dictionary_size(map) == 1 &&
                  cyaml_dictionary_get(map, "ok") == child;
    if (r != ccol_success && again != ccol_success) cyaml_destroy(child);
    cyaml_destroy(map);
    REQUIRE_EQ(r, ccol_invalid_args);
    REQUIRE_EQ(again, ccol_success);
    REQUIRE_TRUE(placed);
  }
}

TEST(api_utf8, node_set_tag_refuses_ill_formed_tag_and_keeps_the_old_one) {
  for (size_t i = 0; i < API_BAD_COUNT; i++) {
    cyaml n = cyaml_create_string("v");
    ccol_retval r0 =
        n ? cyaml_node_set_tag(n, "!keep") : ccol_not_enough_memory;
    ccol_retval r =
        n ? cyaml_node_set_tag(n, api_bad_utf8[i]) : ccol_not_enough_memory;
    bool kept =
        n && cyaml_node_tag(n) && strcmp(cyaml_node_tag(n), "!keep") == 0;
    cyaml_destroy(n);
    REQUIRE_EQ(r0, ccol_success);
    REQUIRE_EQ(r, ccol_invalid_args);
    REQUIRE_TRUE(kept);
  }
}

// The canonical text of a collection key is built under its limit

// The largest canonical buffer the builder may need: The 64 KiB limit plus a
// byte and the NUL, grown by doubling from 256 bytes. A literal, so it doesn't
// move with the library constant. The canonical key tests require the peak
// buffer to stay under it.
#define CANON_PEAK_CEILING ((size_t)128 * 1024)

// Parses input and tells whether it is refused with the canonical key limit's
// message. Stores the largest canonical buffer the parse built in *peak, so
// the tests can check that the builder stopped at the limit.
static bool canon_refused(const char *input, size_t *peak) {
  cyaml__canonical_key_peak_bytes_for_tests = 0;
  char *err = NULL;
  cyaml doc = cyaml_parse(input, &err);
  *peak = cyaml__canonical_key_peak_bytes_for_tests;
  bool refused = !doc && err &&
                 strstr(err, "canonical form of a non-scalar key") &&
                 strstr(err, "exceeds 65536 bytes");
  if (!refused && err) fprintf(stderr, "unexpected result: %s\n", err);
  cyaml_destroy(doc);
  return refused;
}

// Builds "a: &a <anchored>\n? [*a, *a, ...]\n: v\n" with aliases aliases, a
// key whose canonical text grows with every alias. The caller frees it.
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

// Builds prefix, count copies of unit, and suffix on the heap, for the long
// inputs of the canonical key tests. The caller frees it.
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

// The key renders to about 1 MiB in full (each alias's canonical text is four
// times its stored text), so the builder must stop at the limit instead of
// rendering everything and checking afterwards.
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

// Every parse error message is printable ASCII

// Tells whether every byte of s is printable ASCII, which every parse error
// message must be.
static bool err_text_is_printable(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
  return true;
}

// Parses len bytes of input, which must be refused, and tells whether the
// message is printable ASCII and holds want when want isn't NULL. The error
// text tests run it for each quoting context.
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

// Runs err_text_check() on the string literal lit, with the length taken from
// the literal so the input may hold NUL bytes, and evaluates to its verdict.
// want is the text the error message must hold, or NULL.
#define ERR_TEXT_CHECK(lit, want) err_text_check((lit), sizeof(lit) - 1, (want))

// A message that quotes input spells every byte outside 0x20..0x7E as <0xNN>,
// so neither a decoded ESC nor half a UTF-8 sequence reaches it.
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

// One nesting limit for parse, clone and every serializer

// The limit counts nested containers only: 500 are accepted with anything
// inside the innermost, a 501st is refused, and scalars, tags and anchors take
// no level. The nesting tests build documents at and past the limit and look
// for CYAML_TEST_DEPTH_ERR in the refusal. Both are literals so they don't
// move with the library.
#define CYAML_TEST_NESTING_LIMIT 500
#define CYAML_TEST_DEPTH_ERR "maximum nesting depth (500) exceeded"

// Builds head + open * n + inner + close * n + tail on the heap, the nested
// documents of the nesting limit tests. The caller frees it.
static char *cyaml_test_nest(const char *head, const char *open, size_t n,
                             const char *inner, const char *close,
                             const char *tail) {
  size_t lo = strlen(open), lc = strlen(close);
  size_t len = strlen(head) + n * (lo + lc) + strlen(inner) + strlen(tail);
  char *s = (char *)malloc(len + 1);
  if (!s) return NULL;
  size_t off = 0;
  size_t lh = strlen(head), li = strlen(inner), lt = strlen(tail);
  memcpy(s + off, head, lh);
  off += lh;
  for (size_t i = 0; i < n; i++, off += lo) memcpy(s + off, open, lo);
  memcpy(s + off, inner, li);
  off += li;
  for (size_t i = 0; i < n; i++, off += lc) memcpy(s + off, close, lc);
  memcpy(s + off, tail, lt);
  off += lt;
  s[off] = '\0';
  return s;
}

// What a parse of one document did, with every resource released: Whether the
// document could be built at all, whether it parsed, whether a refusal named
// the nesting limit or an alias expansion, and whether every walk of an
// accepted tree succeeded and reparsed.
typedef struct {
  bool built;
  bool parsed;
  bool depth;
  bool alias;
  bool walks_ok;
} cyaml_nest_outcome_t;

// Tells whether doc clones and serializes in all three forms, and whether the
// clone and the block and flow text give a tree equal to doc. The nesting
// tests require it for every tree the parser accepts at the limit.
static bool cyaml_test_walks_agree(cyaml doc) {
  cyaml copy = cyaml_clone(doc);
  char *block = cyaml_serialize(doc);
  char *flow = cyaml_serialize_flow(doc);
  char *stream = cyaml_serialize_stream(doc);
  cyaml from_block = block ? cyaml_parse(block, NULL) : NULL;
  cyaml from_flow = flow ? cyaml_parse(flow, NULL) : NULL;
  cyaml from_stream = stream ? cyaml_parse(stream, NULL) : NULL;
  bool ok = copy && block && flow && stream && from_block && from_flow &&
            from_stream && _rt_deep_equal(doc, copy) &&
            _rt_deep_equal(doc, from_block) && _rt_deep_equal(doc, from_flow);
  cyaml_destroy(from_stream);
  cyaml_destroy(from_flow);
  cyaml_destroy(from_block);
  cyaml_serialize_free(stream);
  cyaml_serialize_free(flow);
  cyaml_serialize_free(block);
  cyaml_destroy(copy);
  return ok;
}

// Parses doc_text, frees it, and gives what the parse did, so the nesting
// tests can judge a document in one call. A NULL doc_text (a build that ran
// out of memory) gives an outcome with built false.
static cyaml_nest_outcome_t cyaml_test_parse_outcome(char *doc_text) {
  cyaml_nest_outcome_t o = {doc_text != NULL, false, false, false, false};
  if (!doc_text) return o;
  char *err = NULL;
  cyaml doc = cyaml_parse(doc_text, &err);
  o.parsed = (doc != NULL);
  o.depth = err && strstr(err, CYAML_TEST_DEPTH_ERR) != NULL;
  o.alias =
      err && strstr(err, "expansion exceeds the maximum nesting depth (500)");
  if (doc) o.walks_ok = cyaml_test_walks_agree(doc);
  cyaml_destroy(doc);
  free(doc_text);
  return o;
}

// Tells whether o was accepted with every walk agreeing on the tree, the
// verdict for a document at the limit.
static bool cyaml_test_accepted(cyaml_nest_outcome_t o) {
  return o.built && o.parsed && o.walks_ok;
}

// Tells whether o was refused by the nesting guard, the verdict for a document
// one container past the limit.
static bool cyaml_test_refused(cyaml_nest_outcome_t o) {
  return o.built && !o.parsed && o.depth;
}

// If a scalar took a level, the parse would refuse 500 lists around a scalar
// that the clone and the serializers accept when the tree comes from the
// API.
TEST(nesting_limit, flow_collections_count_and_a_scalar_inside_does_not) {
  const size_t n = CYAML_TEST_NESTING_LIMIT;
  bool empty_at = cyaml_test_accepted(
      cyaml_test_parse_outcome(cyaml_test_nest("", "[", n, "", "]", "\n")));
  bool scalar_at = cyaml_test_accepted(
      cyaml_test_parse_outcome(cyaml_test_nest("", "[", n, "1", "]", "\n")));
  bool dict_at = cyaml_test_accepted(
      cyaml_test_parse_outcome(cyaml_test_nest("", "{k: ", n, "1", "}", "\n")));
  bool empty_past = cyaml_test_refused(
      cyaml_test_parse_outcome(cyaml_test_nest("", "[", n + 1, "", "]", "\n")));
  bool scalar_past = cyaml_test_refused(cyaml_test_parse_outcome(
      cyaml_test_nest("", "[", n + 1, "1", "]", "\n")));
  bool dict_past = cyaml_test_refused(cyaml_test_parse_outcome(
      cyaml_test_nest("", "{k: ", n + 1, "1", "}", "\n")));
  REQUIRE_TRUE(empty_at);
  REQUIRE_TRUE(scalar_at);
  REQUIRE_TRUE(dict_at);
  REQUIRE_TRUE(empty_past);
  REQUIRE_TRUE(scalar_past);
  REQUIRE_TRUE(dict_past);
}

// Builds levels nested block mappings, each one column further in, whose
// innermost key holds the scalar 1, for the block nesting test. The caller
// frees it.
static char *cyaml_test_block_leaf_doc(size_t levels) {
  char *s = (char *)malloc(levels * (levels + 6) + 8);
  if (!s) return NULL;
  size_t p = 0;
  for (size_t i = 0; i < levels; i++) {
    for (size_t j = 0; j < i; j++) s[p++] = ' ';
    memcpy(s + p, (i + 1 == levels) ? "k: 1\n" : "k:\n",
           (i + 1 == levels) ? 5 : 3);
    p += (i + 1 == levels) ? 5 : 3;
  }
  s[p] = '\0';
  return s;
}

TEST(nesting_limit, block_mappings_with_a_scalar_leaf) {
  const size_t n = CYAML_TEST_NESTING_LIMIT;
  bool at = cyaml_test_accepted(
      cyaml_test_parse_outcome(cyaml_test_block_leaf_doc(n)));
  bool past = cyaml_test_refused(
      cyaml_test_parse_outcome(cyaml_test_block_leaf_doc(n + 1)));
  REQUIRE_TRUE(at);
  REQUIRE_TRUE(past);
}

TEST(nesting_limit, tags_and_anchors_take_no_level) {
  const size_t n = CYAML_TEST_NESTING_LIMIT;
  static const char *opens[] = {"!t [", "&a [", "&a !t [", "!t &a {k: "};
  static const char *closes[] = {"]", "]", "]", "}"};
  bool at_ok = true, past_ok = true;
  for (size_t i = 0; i < sizeof(opens) / sizeof(opens[0]); i++) {
    if (!cyaml_test_accepted(cyaml_test_parse_outcome(
            cyaml_test_nest("", opens[i], n, "&b !u 1", closes[i], "\n"))))
      at_ok = false;
    if (!cyaml_test_refused(cyaml_test_parse_outcome(
            cyaml_test_nest("", opens[i], n + 1, "&b !u 1", closes[i], "\n"))))
      past_ok = false;
  }
  REQUIRE_TRUE(at_ok);
  REQUIRE_TRUE(past_ok);
}

// A "key: value" pair in a flow sequence is a one-entry mapping one level
// below the sequence. Without that level the parse would accept a tree of 501
// containers that every walk then refuses.
TEST(nesting_limit, a_pair_inside_a_flow_sequence_takes_a_level) {
  const size_t n = CYAML_TEST_NESTING_LIMIT;
  bool shorthand_at = cyaml_test_accepted(cyaml_test_parse_outcome(
      cyaml_test_nest("", "[", n - 1, "a: 1", "]", "\n")));
  bool shorthand_past = cyaml_test_refused(
      cyaml_test_parse_outcome(cyaml_test_nest("", "[", n, "a: 1", "]", "\n")));
  bool explicit_at = cyaml_test_accepted(cyaml_test_parse_outcome(
      cyaml_test_nest("", "[", n - 1, "? a : 1", "]", "\n")));
  bool explicit_past = cyaml_test_refused(cyaml_test_parse_outcome(
      cyaml_test_nest("", "[", n, "? a : 1", "]", "\n")));
  bool novalue_at = cyaml_test_accepted(cyaml_test_parse_outcome(
      cyaml_test_nest("", "[", n - 1, "? a", "]", "\n")));
  bool novalue_past = cyaml_test_refused(
      cyaml_test_parse_outcome(cyaml_test_nest("", "[", n, "? a", "]", "\n")));
  bool container_value_at = cyaml_test_accepted(cyaml_test_parse_outcome(
      cyaml_test_nest("", "[", n - 2, "a: []", "]", "\n")));
  bool container_value_past = cyaml_test_refused(cyaml_test_parse_outcome(
      cyaml_test_nest("", "[", n - 1, "a: []", "]", "\n")));
  REQUIRE_TRUE(shorthand_at);
  REQUIRE_TRUE(shorthand_past);
  REQUIRE_TRUE(explicit_at);
  REQUIRE_TRUE(explicit_past);
  REQUIRE_TRUE(novalue_at);
  REQUIRE_TRUE(novalue_past);
  REQUIRE_TRUE(container_value_at);
  REQUIRE_TRUE(container_value_past);
}

// Builds "a: &x" + anchored + "\nb: " + "[" * m + use + "]" * m, where the
// anchored node nests n flow lists, or n flow mappings with a scalar inside
// when as_map is true, and use is "*x" or "{<<: *x}". The alias and merge
// nesting tests vary n and m around the limit. The caller frees it.
static char *cyaml_test_alias_doc(size_t n, bool as_map, size_t m,
                                  const char *use) {
  char *anchored = as_map ? cyaml_test_nest("&x ", "{k: ", n, "1", "}", "")
                          : cyaml_test_nest("&x ", "[", n, "", "]", "");
  if (!anchored) return NULL;
  size_t head_size = strlen(anchored) + 16;
  char *head = (char *)malloc(head_size);
  if (!head) {
    free(anchored);
    return NULL;
  }
  snprintf(head, head_size, "a: %s\nb: ", anchored);
  free(anchored);
  char *doc = cyaml_test_nest(head, "[", m, use, "]", "\n");
  free(head);
  return doc;
}

// An alias copies its subtree below every container around the alias, so the
// tree is 1 + m + n deep and the expansion must fit in the levels left.
TEST(nesting_limit, an_alias_expansion_must_fit_below_the_alias) {
  const size_t n = CYAML_TEST_NESTING_LIMIT;
  cyaml_nest_outcome_t at = cyaml_test_parse_outcome(
      cyaml_test_alias_doc(n / 2 - 1, false, n / 2, "*x"));
  cyaml_nest_outcome_t past_anchor =
      cyaml_test_parse_outcome(cyaml_test_alias_doc(n / 2, false, n / 2, "*x"));
  cyaml_nest_outcome_t past_alias = cyaml_test_parse_outcome(
      cyaml_test_alias_doc(n / 2 - 1, false, n / 2 + 1, "*x"));
  REQUIRE_TRUE(cyaml_test_accepted(at));
  REQUIRE_TRUE(past_anchor.built && !past_anchor.parsed && past_anchor.alias);
  REQUIRE_TRUE(past_alias.built && !past_alias.parsed && past_alias.alias);
}

// A merge value is an alias that counts where it stands: m + 2 levels around
// it and n inside, accepted at 500 and refused one deeper.
TEST(nesting_limit, a_merge_expansion_must_fit_below_the_merge_key) {
  cyaml_nest_outcome_t at = cyaml_test_parse_outcome(
      cyaml_test_alias_doc(200, true, 298, "{<<: *x}"));
  cyaml_nest_outcome_t past = cyaml_test_parse_outcome(
      cyaml_test_alias_doc(200, true, 299, "{<<: *x}"));
  cyaml_nest_outcome_t far_past = cyaml_test_parse_outcome(
      cyaml_test_alias_doc(400, true, 300, "{<<: *x}"));
  REQUIRE_TRUE(cyaml_test_accepted(at));
  REQUIRE_TRUE(past.built && !past.parsed && past.alias);
  REQUIRE_TRUE(far_past.built && !far_past.parsed && far_past.alias);
}

// Builds levels containers through the API, alternating list and dictionary,
// each tagged "!t" when tagged is true, with the integer 1 in the innermost
// one. The nesting tests compare such trees with what the parser accepts.
// NULL when memory runs out.
static cyaml cyaml_test_api_chain_with_leaf(size_t levels, bool tagged) {
  cyaml root = cyaml_test_build_deep_chain(levels, NULL);
  if (!root) return NULL;
  cyaml cur = root;
  bool ok = true;
  while (ok) {
    if (tagged && cyaml_node_set_tag(cur, "!t") != ccol_success) ok = false;
    cyaml next = (cyaml_type(cur) == CYAML_LIST)
                     ? cyaml_list_get(cur, 0)
                     : cyaml_dictionary_get(cur, "n");
    if (!next) break;
    cur = next;
  }
  cyaml leaf = cyaml_create_int(1);
  if (!ok || !leaf) {
    cyaml_destroy(leaf);
    cyaml_destroy(root);
    return NULL;
  }
  ccol_retval r = (cyaml_type(cur) == CYAML_LIST)
                      ? cyaml_list_push(cur, leaf)
                      : cyaml_dictionary_set(cur, "n", leaf);
  if (r != ccol_success) {
    cyaml_destroy(root);
    return NULL;
  }
  return root;
}

// A 500-level API tree, tagged or not, serializes into text that parses back
// and clones; one more container makes every walk refuse it.
TEST(nesting_limit, an_api_tree_at_the_limit_serializes_into_text_that_parses) {
  const size_t n = CYAML_TEST_NESTING_LIMIT;
  bool at_ok = true, past_refused = true;
  for (int tagged = 0; tagged <= 1; tagged++) {
    cyaml at = cyaml_test_api_chain_with_leaf(n, tagged != 0);
    cyaml past = cyaml_test_api_chain_with_leaf(n + 1, tagged != 0);
    if (!at || !cyaml_test_walks_agree(at)) at_ok = false;
    if (!past) {
      past_refused = false;
    } else {
      cyaml copy = cyaml_clone(past);
      char *block = cyaml_serialize(past);
      char *flow = cyaml_serialize_flow(past);
      if (copy || block || flow) past_refused = false;
      cyaml_destroy(copy);
      cyaml_serialize_free(block);
      cyaml_serialize_free(flow);
    }
    cyaml_destroy(at);
    cyaml_destroy(past);
  }
  REQUIRE_TRUE(at_ok);
  REQUIRE_TRUE(past_refused);
}

// A stream parses into a list, so each document of a stream has 499 levels
// left while a single document keeps 500. cyaml_serialize_stream() counts the
// list the same way.
TEST(nesting_limit, a_stream_of_several_documents_counts_its_list) {
  const size_t n = CYAML_TEST_NESTING_LIMIT;
  bool single_at = cyaml_test_accepted(
      cyaml_test_parse_outcome(cyaml_test_nest("--- ", "[", n, "", "]", "\n")));
  bool both_fit = cyaml_test_accepted(cyaml_test_parse_outcome(
      cyaml_test_nest("--- ", "[", n - 1, "1", "]", "\n--- 2\n")));
  bool second_fits = cyaml_test_accepted(cyaml_test_parse_outcome(
      cyaml_test_nest("--- 2\n--- ", "[", n - 1, "1", "]", "\n")));
  bool first_full = cyaml_test_refused(cyaml_test_parse_outcome(
      cyaml_test_nest("--- ", "[", n, "1", "]", "\n--- 2\n")));
  bool second_full = cyaml_test_refused(cyaml_test_parse_outcome(
      cyaml_test_nest("--- 2\n--- ", "[", n, "1", "]", "\n")));

  cyaml list_at = cyaml_create_list();
  cyaml list_past = cyaml_create_list();
  bool built =
      list_at && list_past &&
      cyaml_list_push(list_at, cyaml_test_api_chain_with_leaf(n - 1, false)) ==
          ccol_success &&
      cyaml_list_push(list_at, cyaml_create_int(2)) == ccol_success &&
      cyaml_list_push(list_past, cyaml_test_api_chain_with_leaf(n, false)) ==
          ccol_success &&
      cyaml_list_push(list_past, cyaml_create_int(2)) == ccol_success;
  char *stream_at = built ? cyaml_serialize_stream(list_at) : NULL;
  char *stream_past = built ? cyaml_serialize_stream(list_past) : NULL;
  cyaml back = stream_at ? cyaml_parse(stream_at, NULL) : NULL;
  bool stream_round_trip = back && _rt_deep_equal(list_at, back);
  bool stream_past_refused = built && stream_past == NULL;
  cyaml_destroy(back);
  cyaml_serialize_free(stream_at);
  cyaml_serialize_free(stream_past);
  cyaml_destroy(list_at);
  cyaml_destroy(list_past);

  REQUIRE_TRUE(single_at);
  REQUIRE_TRUE(both_fit);
  REQUIRE_TRUE(second_fits);
  REQUIRE_TRUE(first_full);
  REQUIRE_TRUE(second_full);
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(stream_round_trip);
  REQUIRE_TRUE(stream_past_refused);
}

// A refusing allocator for the test that a get by path allocates nothing.
// These refuse every request while get_refuse is set and count the refused
// requests in get_refused_calls.
static atomic_bool get_refuse;
static atomic_long get_refused_calls;
static void *get_refuse_malloc(size_t n) {
  if (atomic_load(&get_refuse)) {
    get_refused_calls++;
    return NULL;
  }
  return malloc(n);
}
static void get_refuse_free(void *p) { free(p); }
static void *get_refuse_calloc(size_t a, size_t b) {
  if (atomic_load(&get_refuse)) {
    get_refused_calls++;
    return NULL;
  }
  return calloc(a, b);
}
static void *get_refuse_realloc(void *p, size_t n) {
  if (atomic_load(&get_refuse)) {
    get_refused_calls++;
    return NULL;
  }
  return realloc(p, n);
}

// A lookup by path allocates nothing for an ordinary path, so it finds a node
// even while the allocator refuses everything, and NULL always means the path
// names nothing.
TEST(custom_alloc, a_get_by_path_allocates_nothing) {
  ccol_memmgmt_procs mp = {get_refuse_malloc, get_refuse_free,
                           get_refuse_calloc, get_refuse_realloc};
  atomic_store(&get_refuse, false);
  cyaml doc = cyaml_parse_mp("a:\n  b: 1\n", NULL, &mp);
  atomic_store(&get_refused_calls, 0);
  atomic_store(&get_refuse, true);
  cyaml found = doc ? cyaml_get(doc, "a.b") : NULL;
  long calls = atomic_load(&get_refused_calls);
  atomic_store(&get_refuse, false);
  cyaml_destroy(doc);
  REQUIRE_NE((void *)found, NULL);
  REQUIRE_EQ(calls, 0L);
}

// A non-NULL void * is a caller mistake that cyaml_set() refuses and changes
// nothing for, as cjson_set() does; a NULL writes a null.
TEST(set_types, a_void_pointer_that_is_not_null_is_refused) {
  cyaml doc = cyaml_parse("k: 1\n", NULL);
  int target = 7;
  void *vp = &target;
  void *np = NULL;
  ccol_retval refused = doc ? cyaml_set(doc, "k", vp) : ccol_unexpected_failure;
  cyaml k = doc ? cyaml_get(doc, "k") : NULL;
  long long kept =
      (k && cyaml_type(k) == CYAML_INTEGER) ? cyaml_int_val(k) : -1;
  ccol_retval nulled = doc ? cyaml_set(doc, "n", np) : ccol_unexpected_failure;
  cyaml_node_type ntype = doc ? cyaml_type(cyaml_get(doc, "n")) : CYAML_INTEGER;
  cyaml_destroy(doc);
  REQUIRE_EQ(refused, ccol_invalid_args);
  REQUIRE_EQ(kept, 1LL);
  REQUIRE_EQ(nulled, ccol_success);
  REQUIRE_EQ((int)ntype, (int)CYAML_NULL);
}

TEST(flow_list, an_explicit_pair_may_leave_out_its_key) {
  char *err = NULL;
  cyaml a = cyaml_parse("[? ]", &err);
  cyaml b = cyaml_parse("[? : v, 1]", &err);
  cyaml m = cyaml_parse("{? : v}", &err);
  cyaml a0 = a ? cyaml_list_get(a, 0) : NULL;
  cyaml b0 = b ? cyaml_list_get(b, 0) : NULL;
  bool a_ok = a0 && cyaml_type(a0) == CYAML_DICTIONARY &&
              cyaml_dictionary_size(a0) == 1 &&
              cyaml_type(cyaml_dictionary_get(a0, "null")) == CYAML_NULL;
  cyaml bv = b0 ? cyaml_dictionary_get(b0, "null") : NULL;
  bool b_ok = bv && cyaml_type(bv) == CYAML_STRING &&
              strcmp(cyaml_str_val(bv), "v") == 0 && cyaml_list_len(b) == 2;
  cyaml mv = m ? cyaml_dictionary_get(m, "null") : NULL;
  bool same_as_map = mv && strcmp(cyaml_str_val(mv), "v") == 0;
  cyaml_destroy(a);
  cyaml_destroy(b);
  cyaml_destroy(m);
  REQUIRE_TRUE(a_ok);
  REQUIRE_TRUE(b_ok);
  REQUIRE_TRUE(same_as_map);
}

// "! *x : v" and "&y *x : v" at the first entry of a nested block mapping look
// like an alias key, and they are refused like everywhere else. An anchor on
// its own line before an alias key stays valid.
TEST(aliases, an_alias_key_never_carries_a_property) {
  static const char *const refused[] = {
      "a: &x 1\nb:\n  ! *x : v\n",
      "a: &x 1\nb:\n  &y *x : v\n",
      "a: &x 1\nb:\n  !!str *x : v\n",
  };
  int accepted = 0;
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
    char *err = NULL;
    cyaml d = cyaml_parse(refused[i], &err);
    if (d || !err || !strstr(err, "an alias cannot carry")) accepted++;
    cyaml_destroy(d);
  }
  char *err = NULL;
  cyaml ok = cyaml_parse("a: &x k\ntop: &node\n  *x : value\n", &err);
  cyaml top = ok ? cyaml_get(ok, "top") : NULL;
  cyaml val = top ? cyaml_dictionary_get(top, "k") : NULL;
  bool valid_ok = val && strcmp(cyaml_str_val(val), "value") == 0;
  cyaml_destroy(ok);
  REQUIRE_EQ(accepted, 0);
  REQUIRE_TRUE(valid_ok);
}

// Grammar, tags, directives, anchors and limits

// Tells whether cyaml_parse() refuses doc with an error message that holds
// needle, the refusing half of the grammar tests. Releases everything before
// it returns.
static bool cyaml_test_refused_with(const char *doc, const char *needle) {
  char *err = NULL;
  cyaml d = cyaml_parse(doc, &err);
  bool refused = d == NULL && err != NULL && strstr(err, needle) != NULL;
  cyaml_destroy(d);
  return refused;
}

// Tells whether the flow serialization of the parse of doc equals want, the
// accepting half of the grammar tests. Releases everything before it
// returns.
static bool cyaml_test_parses_to(const char *doc, const char *want) {
  char *err = NULL;
  cyaml d = cyaml_parse(doc, &err);
  char *got = d ? cyaml_serialize_flow(d) : NULL;
  bool same = got != NULL && strcmp(got, want) == 0;
  if (got) cyaml_serialize_free(got);
  cyaml_destroy(d);
  return same;
}

// ns-plain-first (YAML 1.2 section 7.3.3) excludes every c-indicator, so a
// block plain scalar never starts with ']', '}' or ',', while a flow context
// reads them as structure.
TEST(plain_scalars, a_block_plain_scalar_never_starts_with_a_flow_indicator) {
  static const char *const refused[] = {
      "]: x\n",       "a: ]\n",    "- ]x\n",       ",a: x\n", "a: }\n",
      "a: 1\n}: x\n", "&a ]: x\n", "!!str ,: x\n", "- - ,\n", "a:\n  - }b\n",
  };
  int accepted = 0;
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    if (!cyaml_test_refused_with(refused[i], "") ||
        (i != 5 && !cyaml_test_refused_with(refused[i], "flow indicator")))
      accepted++;
  bool flow_ok = cyaml_test_parses_to("[&a, b]", "[~, b]") &&
                 cyaml_test_parses_to("[!!str]", "[\"\"]") &&
                 cyaml_test_parses_to("a: b]c", "{a: \"b]c\"}") &&
                 cyaml_test_parses_to("a: x,y", "{a: \"x,y\"}");
  REQUIRE_EQ(accepted, 0);
  REQUIRE_TRUE(flow_ok);
}

// In a flow context a plain scalar ends at '[' and '{' as well as at ',', ']'
// and '}', so "[a{b]" is refused as a missing ','. In a block context they
// stay content.
TEST(plain_scalars, a_flow_plain_scalar_ends_at_every_flow_indicator) {
  static const char *const refused[] = {
      "[a{b]", "{a: b{c}", "k: [x[1, y]", "[a\n[b]]", "{a: b\n{c}}", "[a{]",
  };
  int accepted = 0;
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    if (!cyaml_test_refused_with(refused[i], "expected ','")) accepted++;
  bool ok = cyaml_test_parses_to("[a:[b]]", "[{a: [b]}]") &&
            cyaml_test_parses_to("a: b{c}", "{a: \"b{c}\"}") &&
            cyaml_test_parses_to("a: b[c]", "{a: \"b[c]\"}");
  REQUIRE_EQ(accepted, 0);
  REQUIRE_TRUE(ok);
}

// A %TAG prefix is percent-decoded like a verbatim tag and a shorthand suffix,
// so both spellings give one tag string; a malformed escape is refused.
TEST(tags, a_tag_prefix_is_percent_decoded_like_the_rest_of_the_tag) {
  char *err = NULL;
  cyaml a = cyaml_parse("%TAG !e! tag:x%21,\n---\n- !e!a b\n", &err);
  cyaml b = cyaml_parse("- !<tag:x%21,a> b\n", &err);
  const char *ta = a ? cyaml_node_tag(cyaml_list_get(a, 0)) : NULL;
  const char *tb = b ? cyaml_node_tag(cyaml_list_get(b, 0)) : NULL;
  bool same = ta && tb && strcmp(ta, "tag:x!,a") == 0 && strcmp(tb, ta) == 0;
  cyaml_destroy(a);
  cyaml_destroy(b);
  bool bad_escape =
      cyaml_test_refused_with("%TAG !e! tag:%ZZ\n---\n!e!a b\n",
                              "malformed percent-escape") &&
      cyaml_test_refused_with("%TAG !e! tag:%00\n---\n!e!a b\n", "null byte") &&
      cyaml_test_refused_with("%TAG !e! tag:%C3\n---\n!e!x b\n", "UTF-8") &&
      cyaml_test_refused_with("%TAG !e! tag:a{b\n---\n!e!x b\n",
                              "not allowed in a tag") &&
      cyaml_test_refused_with("%TAG !e! [x\n---\n!e!a b\n",
                              "cannot start a %TAG prefix");
  REQUIRE_TRUE(same);
  REQUIRE_TRUE(bad_escape);
}

// A verbatim tag is ns-uri-char and a shorthand suffix ns-tag-char, so any
// other byte is refused while its percent escape is accepted, and every tag
// the setter accepts round-trips.
TEST(tags, a_tag_holds_only_uri_characters) {
  static const char *const refused[] = {
      "!<a b> x\n",   "!<a{b}> x\n",  "!<a\"b> x\n", "!<\xc3\xa9> x\n",
      "!a.b!c x\n",   "!!a!b x\n",    "!a\"b x\n",   "!a<b x\n",
      "- !<a|b> x\n", "[!<a^b> x]\n",
  };
  int accepted = 0;
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++)
    if (!cyaml_test_refused_with(refused[i], "not allowed in a tag"))
      accepted++;
  char *err = NULL;
  cyaml esc = cyaml_parse("!<a%7Bb%7D%20c> x\n", &err);
  bool escaped_ok = esc && strcmp(cyaml_node_tag(esc), "a{b} c") == 0;
  cyaml_destroy(esc);

  static const char *const set_tags[] = {"a b",   "x{y}", "\xc3\xa9!", "a#b",
                                         "p<q>r", "%41",  "u\\v^w`"};
  int round_trip_bad = 0;
  for (size_t i = 0; i < sizeof(set_tags) / sizeof(set_tags[0]); i++) {
    cyaml n = cyaml_create_string("v");
    bool set = n && cyaml_node_set_tag(n, set_tags[i]) == ccol_success;
    char *text = set ? cyaml_serialize(n) : NULL;
    cyaml back = text ? cyaml_parse(text, &err) : NULL;
    if (!back || !cyaml_node_tag(back) ||
        strcmp(cyaml_node_tag(back), set_tags[i]) != 0)
      round_trip_bad++;
    cyaml_destroy(back);
    if (text) cyaml_serialize_free(text);
    cyaml_destroy(n);
  }
  REQUIRE_EQ(accepted, 0);
  REQUIRE_TRUE(escaped_ok);
  REQUIRE_EQ(round_trip_bad, 0);
}

TEST(directives, another_major_version_and_a_nameless_directive_are_refused) {
  bool refused =
      cyaml_test_refused_with("%YAML 2.0\n--- x\n",
                              "unsupported %YAML version") &&
      cyaml_test_refused_with("%YAML 0.9\n--- x\n",
                              "unsupported %YAML version") &&
      cyaml_test_refused_with("%YAML 10.1\n--- x\n",
                              "unsupported %YAML version") &&
      cyaml_test_refused_with("%\n--- x\n", "directive with no name") &&
      cyaml_test_refused_with("% YAML 1.2\n--- x\n", "directive with no name");
  bool accepted = cyaml_test_parses_to("%YAML 1.3\n--- x\n", "x") &&
                  cyaml_test_parses_to("%YAML 01.2\n--- x\n", "x") &&
                  cyaml_test_parses_to("%FOO bar\n--- x\n", "x");
  REQUIRE_TRUE(refused);
  REQUIRE_TRUE(accepted);
}

TEST(flow_list, a_comment_may_follow_an_anchor_or_a_tag) {
  bool ok = cyaml_test_parses_to("[&a # c\n x, *a]", "[x, x]") &&
            cyaml_test_parses_to("[!!str # c\n 1]", "[\"1\"]") &&
            cyaml_test_parses_to("{a: &x # c\n  1, b: *x}", "{a: 1, b: 1}") &&
            cyaml_test_parses_to("{? &a # c\n  k: v}", "{k: v}") &&
            cyaml_test_parses_to("[&a # c\n# d\n x, *a]", "[x, x]") &&
            cyaml_test_parses_to("[&a #c\n]", "[~]");
  REQUIRE_TRUE(ok);
}

// A collection that becomes a key counts its levels below the mapping that
// holds it in every key form, as an explicit block key and a flow mapping key
// do.
TEST(deep_documents, a_key_collection_counts_below_its_mapping_in_every_form) {
  struct {
    const char *head;
    size_t lists;
    const char *tail;
  } refused[] = {
      {"- ", 498, "[[x]]: v\n"},       {"- ", 498, "&a [[x]]: v\n"},
      {"- ", 498, "!!seq [[x]]: v\n"}, {"- ", 498, "{[[x]]: v}\n"},
      {"- ", 498, "? [[x]]\n"},        {"- ", 497, "[[[x]]: v]\n"},
      {"- ", 497, "[? [[x]] : v]\n"},
  };
  int wrong = 0;
  for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
    for (size_t one_less = 0; one_less < 2; one_less++) {
      size_t n = refused[i].lists - one_less;
      char *doc =
          cyaml_test_nest("", refused[i].head, n, refused[i].tail, "", "");
      if (!doc) {
        wrong++;
        continue;
      }
      char *err = NULL;
      cyaml d = cyaml_parse(doc, &err);
      bool want_refused = one_less == 0;
      if ((d == NULL) != want_refused ||
          (want_refused && (!err || !strstr(err, "maximum nesting depth"))))
        wrong++;
      cyaml_destroy(d);
      free(doc);
    }
  }
  int alias_wrong = 0;
  for (size_t inner = 498; inner <= 499; inner++) {
    char *anchored =
        cyaml_test_nest("- &k ", "[", inner, "", "]", "\n- *k : v\n");
    char *err = NULL;
    cyaml d = anchored ? cyaml_parse(anchored, &err) : NULL;
    if ((d != NULL) != (inner == 498)) alias_wrong++;
    cyaml_destroy(d);
    free(anchored);
  }
  REQUIRE_EQ(wrong, 0);
  REQUIRE_EQ(alias_wrong, 0);
}

// The frame stack stays within 4 * (CYAML_MAX_NESTING_DEPTH + 1) frames even
// for 500 containers each behind an anchor and a tag.
TEST(deep_documents, the_frame_stack_never_grows_past_its_bound) {
  char *doc = cyaml_test_nest("", "&a !!seq [", 500, "x", "]", "\n");
  REQUIRE_NE((void *)doc, NULL);
  cyaml_test_peak_parse_frame_cap = 0;
  char *err = NULL;
  cyaml d = cyaml_parse(doc, &err);
  size_t peak = cyaml_test_peak_parse_frame_cap;
  bool parsed = d != NULL;
  cyaml_destroy(d);
  free(doc);
  REQUIRE_TRUE(parsed);
  REQUIRE_GT(peak, (size_t)1536);
  REQUIRE_LE(peak, (size_t)(4 * (500 + 1)));
}

// An anchored node that no alias names costs the same as the node without the
// anchor, while each alias pays for its copy under the alias expansion limit,
// lowered here to 1000 nodes.
TEST(anchors, an_anchor_that_no_alias_names_copies_nothing) {
  size_t n = 3000;
  size_t cap = 64 + n * 3;
  char *doc = malloc(cap);
  char *nested = malloc(cap);
  char *aliased = malloc(cap + 16);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_NE((void *)nested, NULL);
  REQUIRE_NE((void *)aliased, NULL);
  size_t off = (size_t)snprintf(doc, cap, "a: &x [");
  for (size_t i = 0; i < n; i++) {
    memcpy(doc + off, i ? ",1" : "1", i ? 2 : 1);
    off += i ? 2 : 1;
  }
  memcpy(doc + off, "]\n", 3);
  snprintf(nested, cap, "a: &p [&q [&r [&s %s]]]\n", doc + 6);
  nested[strlen(nested) - 1] = '\0';
  snprintf(aliased, cap + 16, "%sb: *x\n", doc);

  size_t saved_nodes = cyaml_test_parse_node_floor;
  cyaml_test_parse_node_floor = 1000;
  char *err = NULL;
  cyaml d1 = cyaml_parse(doc, &err);
  size_t len1 = d1 ? cyaml_list_len(cyaml_get(d1, "a")) : 0;
  cyaml d2 = cyaml_parse(nested, &err);
  cyaml d3 = cyaml_parse(aliased, &err);
  bool alias_refused = d3 == NULL && err && strstr(err, "alias expansion");
  cyaml_test_parse_node_floor = saved_nodes;
  bool nested_ok = d2 != NULL;
  cyaml_destroy(d1);
  cyaml_destroy(d2);
  cyaml_destroy(d3);
  free(doc);
  free(nested);
  free(aliased);
  REQUIRE_EQ(len1, n);
  REQUIRE_TRUE(nested_ok);
  REQUIRE_TRUE(alias_refused);
}

TEST(anchors, an_alias_and_its_anchor_change_independently) {
  char *err = NULL;
  cyaml d = cyaml_parse("src: &a {val: [1]}\ndst: *a\n", &err);
  REQUIRE_NE((void *)d, NULL);
  cyaml dst_list = cyaml_get(d, "dst.val");
  cyaml src_list = cyaml_get(d, "src.val");
  bool pushed = dst_list &&
                cyaml_list_push(dst_list, cyaml_create_int(2)) == ccol_success;
  bool set = cyaml_set(d, "src.val.#0", 7) == ccol_success;
  size_t src_len = src_list ? cyaml_list_len(src_list) : 0;
  long long dst_first =
      dst_list ? cyaml_int_val(cyaml_list_get(dst_list, 0)) : 0;
  cyaml_destroy(d);
  REQUIRE_TRUE(pushed);
  REQUIRE_TRUE(set);
  REQUIRE_EQ(src_len, (size_t)1);
  REQUIRE_EQ(dst_first, 1LL);
}

// The anchor table points at the node itself, so a node the parse drops (the
// old value of a repeated key, a merge value, a key node, an anchor inside a
// key or a re-read speculation) must stay alive for later aliases. memtest
// reports a read of freed memory otherwise.
TEST(anchors, an_anchored_node_that_the_tree_drops_still_resolves) {
  static const struct {
    const char *doc, *want;
  } cases[] = {
      {"a: &x [1, 2]\na: 3\nb: *x\n", "{a: 3, b: [1, 2]}"},
      {"{a: &x [1], a: *x}", "{a: [1]}"},
      {"m: {<<: &y {p: 1}}\nn: *y\n", "{m: {p: 1}, \"n\": {p: 1}}"},
      {"- {<<: [&y {p: 1}, {q: 2}]}\n- *y\n", "[{p: 1, q: 2}, {p: 1}]"},
      {"[<<: &y {p: 1}]\n", "[{p: 1}]"},
      {"&k [a, b]: v\nc: *k\n", "{\"[a, b]\": v, c: [a, b]}"},
      {"&s \"k\": v\nt: *s\n", "{k: v, t: k}"},
      {"&n 12: v\nt: *n\n", "{12: v, t: 12}"},
      {"{[&i x]: 1, j: *i}", "{\"[x]\": 1, j: x}"},
      {"? &e [k]\n: v\nf: *e\n", "{\"[k]\": v, f: [k]}"},
      {"- &a [&b x]\n- *b\n- *a\n", "[[x], x, [x]]"},
      {"[{a: &x 1, a: 2}]: v\nq: *x\n", "{\"[{a: 2}]\": v, q: 1}"},
      {"- &a [{a: &x 1, a: 2}]\n- *x\n", "[[{a: 2}], 1]"},
      {"a: &x 1\na: &x 2\nb: *x\n", "{a: 2, b: 2}"},
  };
  int wrong = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    if (!cyaml_test_parses_to(cases[i].doc, cases[i].want)) wrong++;
  REQUIRE_EQ(wrong, 0);
}

TEST(serialize, indentation_wider_than_one_block_round_trips) {
  char *doc = cyaml_test_nest("", "[", 100, "x", "]", "\n");
  REQUIRE_NE((void *)doc, NULL);
  char *err = NULL;
  cyaml d = cyaml_parse(doc, &err);
  char *text = d ? cyaml_serialize(d) : NULL;
  char want[256];
  want[0] = '\n';
  memset(want + 1, ' ', 198);
  memcpy(want + 199, "- x\n", sizeof("- x\n"));
  bool deep_line = text && strstr(text, want) != NULL;
  cyaml back = text ? cyaml_parse(text, &err) : NULL;
  char *flow_a = d ? cyaml_serialize_flow(d) : NULL;
  char *flow_b = back ? cyaml_serialize_flow(back) : NULL;
  bool same = flow_a && flow_b && strcmp(flow_a, flow_b) == 0;
  if (flow_a) cyaml_serialize_free(flow_a);
  if (flow_b) cyaml_serialize_free(flow_b);
  if (text) cyaml_serialize_free(text);
  cyaml_destroy(back);
  cyaml_destroy(d);
  free(doc);
  REQUIRE_TRUE(deep_line);
  REQUIRE_TRUE(same);
}

// A component that isn't UTF-8 makes a path malformed at any position, like an
// empty component: Set and delete answer ccol_invalid_args and get gives NULL,
// while a well-formed absent component stays ccol_key_not_found.
TEST(paths, a_component_that_is_not_utf8_is_invalid_at_any_position) {
  char *err = NULL;
  cyaml doc = cyaml_parse("d: {k: 1}\nl: [1]\n", &err);
  REQUIRE_NE((void *)doc, NULL);
  static const char *const bad[] = {
      "\xff.k", "d.\xc3.k", "missing.\xff.k", "missing.k.\xff",
      "\xff",   "d.\xff",   "missing.\xc3",   "missing.x.\xc3.y",
  };
  int set_wrong = 0, delete_wrong = 0, get_wrong = 0;
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    if (cyaml_set(doc, bad[i], 5) != ccol_invalid_args) set_wrong++;
    if (cyaml_delete(doc, bad[i]) != ccol_invalid_args) delete_wrong++;
    if (cyaml_get(doc, bad[i]) != NULL) get_wrong++;
  }
  static const char *const absent[] = {"missing.k", "d.x.k", "missing.x.y",
                                       "l.#5.k"};
  int absent_wrong = 0;
  for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
    if (cyaml_set(doc, absent[i], 5) != ccol_key_not_found) absent_wrong++;
    if (cyaml_delete(doc, absent[i]) != ccol_key_not_found) absent_wrong++;
  }
  bool shape_invalid = cyaml_set(doc, "d.k.x", 5) == ccol_invalid_args &&
                       cyaml_set(doc, "l.x", 5) == ccol_invalid_args &&
                       cyaml_set(doc, "l.#x.y", 5) == ccol_invalid_args &&
                       cyaml_delete(doc, "missing..k") == ccol_invalid_args;
  char *flow = cyaml_serialize_flow(doc);
  bool unchanged = flow && strcmp(flow, "{d: {k: 1}, l: [1]}") == 0;
  if (flow) cyaml_serialize_free(flow);
  cyaml_destroy(doc);
  REQUIRE_EQ(set_wrong, 0);
  REQUIRE_EQ(delete_wrong, 0);
  REQUIRE_EQ(get_wrong, 0);
  REQUIRE_EQ(absent_wrong, 0);
  REQUIRE_TRUE(shape_invalid);
  REQUIRE_TRUE(unchanged);
}
