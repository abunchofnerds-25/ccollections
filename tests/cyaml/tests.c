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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

/* Counting allocator: allows exactly g_alloc_remaining malloc/calloc/realloc
 * calls before returning NULL.  -1 means unlimited (normal behaviour). */
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

/* ========================================================================== */
/*                         CONSTRUCTION                                       */
/* ========================================================================== */

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

TEST(construction, mapping_set_replaces_container) {
  /* Replacing a container value (CYAML_LIST) with a scalar must recursively
   * free the old subtree via node_clear and store the new value without
   * leaks or dangling pointers. */
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
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
}

TEST(implicit_types, null_NULL) {
  char *err = NULL;
  cyaml n = cyaml_parse("NULL\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_NULL);
  cyaml_destroy(n);
}

TEST(implicit_types, null_empty_input) {
  /* An empty document (zero bytes) is a null value -- an empty plain scalar
   * resolves to null per the YAML 1.2 core schema. */
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

TEST(implicit_types, integer_negative) {
  char *err = NULL;
  cyaml n = cyaml_parse("-100\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), -100LL);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_llong_min) {
  /* LLONG_MIN = -9223372036854775808.  strtoll on the sign-stripped substring
   * "9223372036854775808" (= 2^63) overflows long long, so this value must be
   * parsed by passing the full signed string to strtoll, not p after the '-'.
   * Without the fix this silently becomes a CYAML_FLOAT with precision loss. */
  char *err = NULL;
  cyaml n = cyaml_parse("-9223372036854775808\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), LLONG_MIN);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_llong_min_round_trip) {
  /* Serialize LLONG_MIN and re-parse; type and value must survive intact. */
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
  /* "trueish" should be a string, not a bool. */
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
  /* YAML 1.2 core schema: only ".nan"/".NaN"/".NAN" are float NaN.
   * Bare "nan" is a plain string; strtod() on C99 platforms accepts it as
   * NaN, so make_typed_scalar must pre-filter these. */
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

TEST(implicit_types, string_inf_lowercase) {
  /* Bare "inf" is a string under YAML 1.2; only ".inf"/".Inf"/".INF" are
   * float infinity. */
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
  /* Verify full round-trip for the bare C99 forms: create string node,
   * serialize, re-parse, value and type must be preserved. */
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
  /* U+1F600 encoded as a UTF-16 surrogate pair 😀. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\uD83D\\uDE00\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  /* U+1F600 in UTF-8 is F0 9F 98 80. */
  REQUIRE_STREQ(cyaml_str_val(n), "\xF0\x9F\x98\x80");
  cyaml_destroy(n);
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
  /* Plain first key followed by a double-quoted subsequent key. */
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
  /* Single-quoted key containing spaces (impossible as a plain scalar). */
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
  /* A double-quoted scalar spanning two lines: the single line break is
   * folded to a space per YAML 1.2 section 6.5. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\nline2\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 line2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_multiline_blank_line_preserved) {
  /* One blank line (two consecutive newlines) between content lines must be
   * retained as a single newline in the value per YAML 1.2 section 6.5. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\n\nline2\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1\nline2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_multiline_leading_whitespace_stripped) {
  /* Leading whitespace on a continuation line is stripped after folding. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\n   continuation\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 continuation");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_multiline_fold_to_space) {
  /* Single-quoted multiline: same line-folding rules as double-quoted. */
  char *err = NULL;
  cyaml n = cyaml_parse("'line1\nline2'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1 line2");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_multiline_blank_line_preserved) {
  /* One blank line in a single-quoted scalar must be retained as one newline.
   */
  char *err = NULL;
  cyaml n = cyaml_parse("'line1\n\nline2'\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1\nline2");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_trailing_whitespace_stripped_before_fold) {
  /* YAML 1.2 sec. 8.1.2: trailing white space is excluded from content on
   * the line where the fold occurs.  "hello   \nworld" must produce
   * "hello world" (three spaces before the newline are stripped, replaced
   * by a single fold space). */
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
  /* A backslash immediately before a literal newline discards the newline
   * and all leading whitespace on the continuation line (YAML 1.2 sec.
   * 8.1.1.2).  The result must not contain either the newline or any
   * surrounding spaces. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"line1\\\n   cont\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "line1cont");
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
  /* Folded: single newline between lines folds to space; trailing blank line
   * is preserved by CHOMP_KEEP, giving two trailing newlines total. */
  REQUIRE_STREQ(cyaml_str_val(n), "line one line two\n\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_keep_content_no_trailing_blank) {
  /* >+ with content but NO trailing blank lines: CHOMP_KEEP must still emit
   * exactly one terminating newline after the last content line.  The
   * "trailing_blanks = 0" branch of the CHOMP_KEEP logic previously went
   * untested. */
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
  /* A blank line before the first content line of a literal block scalar must
   * produce a leading newline in the value per YAML 1.2 spec. */
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
  /* Same contract for folded block scalars: a leading blank line produces a
   * literal newline at the start of the value. */
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
  /* |2 sets block_indent = parent_indent(0) + 2 = 2.  Lines indented beyond
   * the block indent carry their extra spaces into the value. */
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
  /* >2 with two normal content lines: the single newline between them folds
   * to a space, and CHOMP_CLIP appends one trailing newline. */
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
  /* |+ with no content lines but one trailing blank line must produce "\n".
   * YAML 1.2 sec. 8.1.1.2: CHOMP_KEEP preserves trailing empty lines
   * regardless of whether any non-empty content lines are present. */
  char *err = NULL;
  cyaml n = cyaml_parse("|+\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\n");
  cyaml_destroy(n);
}

TEST(block_scalars, folded_keep_no_content_trailing_blank) {
  /* >+ with no content lines but one trailing blank line must produce "\n".
   * Same CHOMP_KEEP rule as the literal case. */
  char *err = NULL;
  cyaml n = cyaml_parse(">+\n\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_chomp_indent_both_orders) {
  /* |2- and |-2 must produce the same result: YAML 1.2 allows the chomping
   * and indentation indicators in either order. */
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

TEST(block_scalars, folded_chomp_indent_both_orders) {
  /* >2- and >-2 must produce the same result. */
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
  /* YAML 1.2 spec s8.1.1.2: a line break adjacent to a more-indented line
   * (on either side) must be preserved as a newline, not folded to a space.
   * Tests the transition: normal -> more-indented -> normal. */
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
  /* Multiple consecutive more-indented lines: each line break among them is
   * preserved (each line's break is adjacent to a more-indented neighbour). */
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
  /* A leading UTF-8 BOM (EF BB BF) must be silently consumed; the rest of
   * the document is parsed normally. */
  static const char yaml[] = "\xEF\xBB\xBFkey: value\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
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
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 3)), CYAML_NULL);
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
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 3)), CYAML_NULL);
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
  REQUIRE_EQ(cyaml_type(ports), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(ports), (size_t)2);

  cyaml p0 = cyaml_list_get(ports, 0);
  REQUIRE_EQ(cyaml_type(p0), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(p0, "host")), 80LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(p0, "container")), 8080LL);

  cyaml_destroy(doc);
}

TEST(flow, mapping_integer_key) {
  /* Integer keys in flow mappings are stringified for storage. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{42: the_answer}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "42")), "the_answer");
  cyaml_destroy(doc);
}

TEST(flow, mapping_non_string_keys) {
  /* null and bool flow-dictionary keys are stringified. */
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
  /* A plain scalar value in a flow dictionary may contain ':' that is not
   * followed by whitespace or a flow terminator.  The canonical case is a
   * URL: "http://example.com" must not be truncated at the first ':'. */
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
  /* Same rule applies inside a flow list. */
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
  /* cyaml_serialize_flow of a dictionary whose value contains a bare ':' must
   * produce output that re-parses back to the original string. */
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
  const char *yaml =
      "default: &def\n"
      "  timeout: 30\n"
      "  retries: 3\n"
      "production:\n"
      "  timeout: 60\n"
      "  retries: 3\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);

  cyaml def = cyaml_dictionary_get(doc, "default");
  REQUIRE_EQ(cyaml_type(def), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(def, "timeout")), 30LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(def, "retries")), 3LL);

  cyaml_destroy(doc);
}

TEST(anchors, alias_resolves_to_anchor_value) {
  /* *alias must produce a node whose content matches the anchored value. */
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
  /* An alias should produce a deep clone, not a shared reference. */
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
  /* Both are separate nodes. */
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
  cyaml alt = cyaml_dictionary_get(doc, "alt");
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
  cyaml copy = cyaml_dictionary_get(doc, "copy");
  REQUIRE_EQ(cyaml_type(shared), CYAML_LIST);
  REQUIRE_EQ(cyaml_type(copy), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(shared), (size_t)2);
  REQUIRE_EQ(cyaml_list_len(copy), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(copy, 0)), 80LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(copy, 1)), 443LL);

  cyaml_destroy(doc);
}

TEST(anchors, anchor_name_reuse) {
  /* The second definition of &a must override the first; *a resolves to the
   * most recent binding. */
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
  /* Empty path returns root. */
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
  /* cyaml_set with a bool literal exercises the CYAML_BOOL branch of
   * _cyaml_type_of and node_reinit_scalar. */
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
  /* cyaml_set with a float literal exercises the float branch of
   * _cyaml_type_of (raw_size == sizeof(float)) in node_reinit_scalar. */
  char *err = NULL;
  cyaml doc = cyaml_parse("scale: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_set(doc, "scale", 1.5f), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "scale")), CYAML_FLOAT);
  /* 1.5 is exactly representable; the float->double promotion is lossless. */
  REQUIRE_EQ(cyaml_double_val(cyaml_get(doc, "scale")), 1.5);
  cyaml_destroy(doc);
}

TEST(path, set_sequence_element_out_of_bounds) {
  /* cyaml_set on a sequence index beyond the last element must fail without
   * modifying the existing elements. */
  char *err = NULL;
  cyaml doc = cyaml_parse("nums:\n  - 1\n  - 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_NE(cyaml_set(doc, "nums.#99", 42), ccol_success);

  /* Existing elements must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "nums.#0")), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "nums.#1")), 2LL);

  cyaml_destroy(doc);
}

TEST(path, set_on_list_root) {
  /* cyaml_set(list_root, "#N", val) must work when the root node is itself a
   * CYAML_LIST rather than a dictionary. */
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
  /* cyaml_set must fail when the intermediate node is a scalar. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  /* "key" is an integer node; "key.sub" has no valid parent container. */
  REQUIRE_NE(cyaml_set(doc, "key.sub", 1), ccol_success);

  /* The original value must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, set_missing_parent) {
  /* cyaml_set must fail when an intermediate node in the path does not exist.
   * The original tree must not be modified. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_NE(cyaml_set(doc, "missing.sub", 1), ccol_success);

  /* Original key must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, set_null_value) {
  /* cyaml_set with a NULL literal must change the leaf's type to CYAML_NULL.
   * typeof(NULL) == void * which _cyaml_type_of maps to CYAML_NULL via the
   * default branch of _Generic. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "key", NULL), ccol_success);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "key")), CYAML_NULL);

  cyaml_destroy(doc);
}

TEST(path, set_string_variable) {
  /* cyaml_set with a char * variable (not a string literal).  The macro
   * path for char * has raw_is_char_array=false, so raw is already a
   * const char ** that node_reinit_scalar dereferences directly. */
  char *err = NULL;
  cyaml doc = cyaml_parse("env: dev\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  char *val = "prod";
  REQUIRE_EQ(cyaml_set(doc, "env", val), ccol_success);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "env")), "prod");

  cyaml_destroy(doc);
}

TEST(path, set_replaces_container_leaf) {
  /* cyaml_set on a path whose leaf is currently a container must replace it
   * with the new scalar in-place via node_clear + reinit. */
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
  /* A path ending with a dot produces an empty leaf component; cyaml_set
   * must reject it and cyaml_get must return NULL. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_NE(cyaml_set(doc, "key.", 1), ccol_success);
  REQUIRE_EQ((void *)cyaml_get(doc, "key."), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path, degenerate_path_consecutive_dots) {
  /* A path with consecutive dots produces an empty intermediate component;
   * navigation must fail and return NULL. */
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
  /* Key is "a.b" (contains a literal dot).  Access it with "a\\.b". */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a.b\": 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\.b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 42LL);
  /* Unescaped dot must NOT find the key. */
  REQUIRE_EQ((void *)cyaml_get(doc, "a.b"), NULL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_backslash_flat_key) {
  /* Key is "a\b" (literal backslash).  Access it with "a\\\\b". */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\\\\b\": 7\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\\\b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 7LL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_dot_nested_path) {
  /* Outer key is plain "outer"; inner key is "k.ey" (literal dot). */
  char *err = NULL;
  cyaml doc = cyaml_parse("outer:\n  \"k.ey\": 99\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "outer.k\\.ey");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 99LL);
  cyaml_destroy(doc);
}

TEST(path, get_escaped_dot_both_components) {
  /* Both levels have dot-containing keys: "a.b" -> "c.d". */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a.b\":\n  \"c.d\": 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  cyaml node = cyaml_get(doc, "a\\.b.c\\.d");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 1LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_creates_new_key) {
  /* Create a new key "x.y" (literal dot) at the root. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "x\\.y", 55), ccol_success);
  cyaml node = cyaml_get(doc, "x\\.y");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cyaml_int_val(node), 55LL);
  /* Must not have created a spurious nested "x" key. */
  REQUIRE_EQ((void *)cyaml_get(doc, "x"), NULL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_updates_existing_key) {
  /* Update an existing key "p.q" (literal dot). */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"p.q\": 0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "p\\.q", 123), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "p\\.q")), 123LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_dot_nested_path) {
  /* Set "outer"."k.ey" via escaped path "outer.k\\.ey". */
  char *err = NULL;
  cyaml doc = cyaml_parse("outer:\n  \"k.ey\": 0\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "outer.k\\.ey", 77), ccol_success);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "outer.k\\.ey")), 77LL);
  cyaml_destroy(doc);
}

TEST(path, set_escaped_backslash_creates_key) {
  /* Create a key "a\\b" (literal backslash) via "a\\\\b". */
  char *err = NULL;
  cyaml doc = cyaml_parse("{}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_set(doc, "a\\\\b", 9), ccol_success);
  REQUIRE_NE((void *)cyaml_get(doc, "a\\\\b"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "a\\\\b")), 9LL);
  cyaml_destroy(doc);
}

TEST(path, set_replaces_container_list_element) {
  /* cyaml_set on a sequence index whose current value is a container must
   * replace it with the new scalar in-place (node_clear + reinit).
   * Verifies the same node_reinit_scalar path as set_replaces_container_leaf
   * but exercised through the list branch of _cyaml_set_typed. */
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
  /* Sibling element must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#1")), 99LL);

  cyaml_destroy(doc);
}

TEST(path, set_single_component_oom) {
  /* _cyaml_set_typed performs two ccol_strdup calls for a single-component
   * path (no dots): one for the scratch copy used to locate dots, and one
   * for leaf_copy.  When the second strdup fails (OOM) the function must
   * return ccol_not_enough_memory and leave the document intact rather than
   * passing NULL into path_unescape_component. */
  g_alloc_remaining = -1;
  char *err = NULL;
  cyaml doc = cyaml_parse_mp("port: 8080\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  /* Allow exactly one malloc so the first strdup (scratch copy) succeeds and
   * the second (leaf_copy) fails. */
  g_alloc_remaining = 1;
  long long new_val = 9090LL;
  ccol_retval_t r = _cyaml_set_typed(doc, "port", CYAML_INTEGER, &new_val,
                                     sizeof(new_val), true, false);
  REQUIRE_EQ(r, ccol_not_enough_memory);

  /* The document must be unmodified. */
  g_alloc_remaining = -1;
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

TEST(path, delete_single_component_oom) {
  /* _cyaml_delete performs two ccol_strdup calls for a single-component path
   * (no dots): one for the scratch copy used to locate dots, and one for
   * leaf_copy.  When the second strdup fails (OOM) the function must return
   * ccol_not_enough_memory and leave the document intact rather than passing
   * NULL into path_unescape_component. */
  g_alloc_remaining = -1;
  char *err = NULL;
  cyaml doc = cyaml_parse_mp("port: 8080\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  /* Allow exactly one malloc so the first strdup (scratch copy) succeeds and
   * the second (leaf_copy) fails. */
  g_alloc_remaining = 1;
  ccol_retval_t r = _cyaml_delete(doc, "port");
  REQUIRE_EQ(r, ccol_not_enough_memory);

  /* The document must be unmodified. */
  g_alloc_remaining = -1;
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "port")), 8080LL);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

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

  /* Mutate original; copy should not change. */
  cyaml_set(original, "server.host", "changed");
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
  /* When the first node allocation inside cyaml_clone fails the function
   * must return NULL without leaking anything. */
  g_alloc_remaining = -1;
  cyaml src = cyaml_create_string_mp("hello", &g_counting_mp);
  REQUIRE_NE((void *)src, NULL);

  g_alloc_remaining = 0;
  cyaml copy = cyaml_clone(src);
  REQUIRE_EQ((void *)copy, NULL);

  g_alloc_remaining = -1;
  /* src must still be intact and destroyable. */
  REQUIRE_STREQ(cyaml_str_val(src), "hello");
  cyaml_destroy(src);
}

/* ========================================================================== */
/*                         SERIALIZATION                                      */
/* ========================================================================== */

TEST(serialize, null) {
  cyaml n = cyaml_create_null();
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "~\n");
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, null_handle_block) {
  /* cyaml_serialize(NULL) must produce "~\n", not an empty string. */
  char *s = cyaml_serialize(NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "~\n");
  cyaml_serialize_free(s);
}

TEST(serialize, null_handle_flow) {
  /* cyaml_serialize_flow(NULL) must produce "~". */
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
  /* "null" needs quoting so it doesn't parse back as CYAML_NULL. */
  REQUIRE_NE(strstr(s, "null"), NULL);
  REQUIRE_NE(s[0], 'n'); /* must be quoted, not plain 'null' */
  cyaml_serialize_free(s);
  cyaml_destroy(n);
}

TEST(serialize, quoted_string_null_variants_round_trip) {
  /* "Null" and "NULL" must serialize as quoted strings so they parse back as
   * CYAML_STRING rather than CYAML_NULL. */
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
  /* "True", "TRUE", "False", "FALSE" must serialize as quoted strings so they
   * parse back as CYAML_STRING rather than CYAML_BOOL. */
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
  /* An empty CYAML_STRING must serialize as "" (quoted) and parse back as
   * CYAML_STRING with value "". */
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
  /* Re-parse the serialized form and verify values survive. */
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

TEST(serialize, empty_sequence_nested_in_mapping_round_trip) {
  /* A dictionary value that is an empty list must be serialized with the
   * correct indentation so that the output re-parses correctly. */
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
  /* A dictionary value that is an empty dictionary must be serialized with the
   * correct indentation so that the output re-parses correctly. */
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
  /* An empty list nested as an element of an outer list must
   * serialize with the correct indentation so the output re-parses. */
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

/* ========================================================================== */
/*                         SCOPED LIFECYCLE                                   */
/* ========================================================================== */

TEST(lifecycle, scoped_destroy) {
  cyaml out = NULL;
  {
    cyaml_declare_scoped(doc) = cyaml_parse("key: 42\n", NULL);
    REQUIRE_NE((void *)doc, NULL);
    out = cyaml_clone(doc);
    /* doc destroyed automatically here */
  }
  /* out must still be valid since it is a clone. */
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(out, "key")), 42LL);
  cyaml_destroy(out);
}

TEST(lifecycle, declare_macro) {
  /* cyaml_declare is a plain typed variable declaration; it must act as a
   * normal lvalue suitable for assignment, cyaml_get, and cyaml_destroy. */
  cyaml_declare(n);
  n = cyaml_create_int(42);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 42LL);
  cyaml_destroy(n);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(lifecycle, list_push_oom) {
  /* Fill a counting_mp-backed list to its initial cvector capacity (4) so
   * the next push triggers a realloc.  Block all counting_mp allocations and
   * verify that cyaml_list_push destroys the victim child and returns a
   * failure code rather than leaking it. */
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
  /* victim is owned (and destroyed) by list_push regardless of outcome. */
  REQUIRE_NE(r, ccol_success);

  g_alloc_remaining = -1;
  REQUIRE_EQ(cyaml_list_len(list), (size_t)4);
  cyaml_destroy(list);
}

TEST(lifecycle, dictionary_set_oom) {
  /* When the backing chmap insert fails (counting_mp exhausted),
   * cyaml_dictionary_set must destroy the child and return a failure code. */
  g_alloc_remaining = -1;
  cyaml doc = cyaml_create_dictionary_mp(&g_counting_mp);
  REQUIRE_NE((void *)doc, NULL);

  g_alloc_remaining = 0;
  cyaml val = cyaml_create_int(42);
  REQUIRE_NE((void *)val, NULL);
  ccol_retval_t r = cyaml_dictionary_set(doc, "key", val);
  /* val is owned (and destroyed) by dictionary_set regardless of outcome. */
  REQUIRE_NE(r, ccol_success);

  g_alloc_remaining = -1;
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)0);
  cyaml_destroy(doc);
}

TEST(lifecycle, remove_null_args) {
  /* Passing NULL map/seq or NULL key to remove functions must return
   * ccol_invalid_args without crashing. */
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

  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "version")), "3.8");

  cyaml web = cyaml_get(doc, "services.web");
  REQUIRE_NE((void *)web, NULL);
  REQUIRE_EQ(cyaml_type(web), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "services.web.image")),
                "nginx:latest");

  cyaml ports = cyaml_get(doc, "services.web.ports");
  REQUIRE_EQ(cyaml_type(ports), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(ports), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(ports, 0)), 80LL);

  cyaml debug = cyaml_get(doc, "services.web.environment.DEBUG");
  REQUIRE_EQ(cyaml_type(debug), CYAML_BOOL);
  REQUIRE_FALSE(cyaml_bool_val(debug));

  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "services.db.ports.#0")), 5432LL);

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
  REQUIRE_EQ(cyaml_type(steps), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(steps), (size_t)2);

  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "jobs.build.steps.#0.name")),
                "Checkout");
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(doc, "jobs.build.steps.#1.run")),
                "make");

  cyaml branches = cyaml_get(doc, "on.push.branches");
  REQUIRE_EQ(cyaml_type(branches), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(branches), (size_t)2);

  cyaml_destroy(doc);
}

TEST(real_world, ansible_like_with_anchors) {
  const char *yaml =
      "defaults: &common\n"
      "  timeout: 30\n"
      "  max_retries: 3\n"
      "tasks:\n"
      "  - name: fetch data\n"
      "    timeout: 30\n"
      "    max_retries: 3\n"
      "  - name: store result\n"
      "    timeout: 60\n"
      "    max_retries: 5\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);

  cyaml defaults = cyaml_dictionary_get(doc, "defaults");
  REQUIRE_EQ(cyaml_type(defaults), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(defaults, "timeout")), 30LL);

  cyaml tasks = cyaml_dictionary_get(doc, "tasks");
  REQUIRE_EQ(cyaml_type(tasks), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(tasks), (size_t)2);

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

/* ========================================================================== */
/*                         ERROR HANDLING                                     */
/* ========================================================================== */

TEST(errors, unterminated_double_quote) {
  char *err = NULL;
  cyaml doc = cyaml_parse("\"unterminated\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, unterminated_single_quote) {
  char *err = NULL;
  cyaml doc = cyaml_parse("'unterminated\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, indented_doc_end_marker_is_trailing_garbage) {
  /* An indented '...' sequence is not a document-end marker; the YAML spec
   * requires document markers to be at column 0.  The parser must reject
   * this as trailing content rather than silently consuming it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: value\n  ...\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, unknown_alias) {
  char *err = NULL;
  cyaml doc = cyaml_parse("key: *nonexistent\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, null_input) {
  char *err = NULL;
  cyaml doc = cyaml_parse(NULL, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, null_input_ignored_error) {
  cyaml doc = cyaml_parse(NULL, NULL);
  REQUIRE_EQ((void *)doc, NULL);
}

TEST(errors, null_input_parse_n) {
  /* cyaml_parse_n with NULL input must return NULL and set the error string,
   * matching the behaviour of cyaml_parse with a non-NULL error pointer. */
  char *err = NULL;
  cyaml doc = cyaml_parse_n(NULL, 0, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, trailing_garbage) {
  /* "extra junk !!!" on the second line has no ':' separator, so the block
   * dictionary stops after "key: value" and the parser reports trailing
   * content.
   */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: value\nextra junk !!!\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

/* ========================================================================== */
/*                         BLOCK SEQUENCE NEXT-LINE VALUE                     */
/* ========================================================================== */

TEST(block_sequence, value_on_next_line_mapping) {
  /* Block-in form: value of each list entry on the next, more-indented
   * line rather than on the same line as '-'. */
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
  /* Scalar value on the next line after '-'. */
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
  /* '-' followed by newline and then another '-' at the same indent: the
   * first element is null, not the second element. */
  const char *yaml =
      "-\n"
      "- value\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_list_len(doc), (size_t)2);
  REQUIRE_EQ(cyaml_type(cyaml_list_get(doc, 0)), CYAML_NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 1)), "value");
  cyaml_destroy(doc);
}

TEST(block_sequence, nested_sequence_on_next_line) {
  /* Inner list whose '-' entries are on the next line after the outer '-'.
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
  /* A plain scalar key starting with '-' (no following space) as the second
   * key in a block dictionary.  Previously the dictionary loop broke on '-'. */
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
  /* Multiple consecutive keys starting with '-'. */
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
  /* Duplicate mapping keys: the last value wins and the earlier value is freed
   * without leaking (implementation-defined behavior, see cyaml.h). */
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
  /* The first value is a list; the second replaces it with a scalar.
   * The replaced list must be recursively freed without leaks or corruption. */
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
  /* String nodes whose values match YAML float-special tokens must be quoted
   * on serialization so they re-parse as strings, not as floats. */
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
    /* Re-parse must yield a STRING, not a FLOAT. */
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
  /* A string whose value contains ':' followed by a tab must be quoted on
   * serialization.  Without quoting the plain scalar would terminate at the
   * colon, causing re-parse to produce a dictionary instead of a string. */
  cyaml n = cyaml_create_string("proto:\thttp");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  /* The value must NOT start with the plain token 'p' immediately followed by
   * an unquoted colon; the first byte must be a quote character. */
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
  /* A string whose value contains a tab followed by '#' must be quoted.
   * Without quoting, the tab+'#' list would be treated as an inline
   * comment marker by the plain scalar parser, truncating the value. */
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
  /* Same tab-hash rule when the string is a dictionary value. */
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
  /* A string whose value starts with a tab must be quoted on serialization.
   * Without quoting the leading tab is consumed as whitespace by the parser,
   * silently changing the value. */
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
  /* A string whose value ends with a space must be quoted on serialization.
   * Without quoting the plain scalar parser strips the trailing space. */
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
  /* A string whose value ends with a tab must be quoted on serialization.
   * Without quoting the plain scalar parser strips the trailing tab. */
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
  /* A string like "+extra" does not start a numeric literal, so
   * needs_quoting() must return false and the plain scalar round-trips. */
  cyaml n = cyaml_create_string("+extra");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  /* The serialized form must NOT contain quotes. */
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
  /* "+42" would be parsed as integer 42 by make_typed_scalar, so
   * needs_quoting() must quote it. */
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
  /* "+.3" is parsed as float 0.3 by strtod if emitted unquoted; it must be
   * quoted so it round-trips as CYAML_STRING. */
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
  /* All +.N patterns that strtod parses as floats must be quoted on
   * serialization so they round-trip as CYAML_STRING. */
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

TEST(block_mapping, document_start_marker_no_separator) {
  /* Per YAML 1.2 sec. 9.1.4, the '---' token is only a document-start
   * marker when the three dashes are followed by whitespace, a comment '#',
   * or EOF.  '---42' has a non-separator fourth character ('4'), so it is
   * NOT a document-start marker and must be parsed as the plain scalar
   * string "---42". */
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
  /* cyaml_parse_n must parse only the first len bytes; trailing bytes beyond
   * the length are invisible to the parser. */
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
  /* cyaml_parse_n must not read past len even when there is no null byte. */
  char buf[16];
  memcpy(buf, "42", 2);
  /* Leave the rest of buf uninitialised (intentional). */
  char *err = NULL;
  cyaml doc = cyaml_parse_n(buf, 2, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(doc), 42LL);
  cyaml_destroy(doc);
}

TEST(parse_n, sequence_bounded) {
  /* Verify a list round-trip through the bounded parser. */
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
  /* cyaml_parse_n must skip a leading UTF-8 BOM (EF BB BF) and parse the
   * remainder normally. */
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
  /* Removing a list element that is itself a nested mapping must not leak. */
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
  /* Removing a key whose value is a nested container must not leak. */
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
  REQUIRE_EQ(cyaml_list_len(items), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(items, 0)), 10LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(items, 1)), 30LL);
  cyaml_destroy(root);
}

TEST(delete, path_delete_on_list_root) {
  /* cyaml_delete(list_root, "#N") must work when the root itself is a
   * CYAML_LIST rather than a dictionary. */
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

TEST(delete, path_delete_with_escaped_dot_in_key) {
  /* Key literally named "a.b" must be addressable via "a\\.b". */
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
  /* An empty path must be rejected with ccol_invalid_args. */
  char *err = NULL;
  cyaml root = cyaml_parse("key: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, ""), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "key"), NULL);
  cyaml_destroy(root);
}

TEST(delete, degenerate_path_trailing_dot) {
  /* A path ending with a dot produces an empty leaf component; cyaml_delete
   * must reject it with ccol_invalid_args and leave the document intact. */
  char *err = NULL;
  cyaml root = cyaml_parse("key: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "key."), ccol_invalid_args);
  REQUIRE_NE((void *)cyaml_get(root, "key"), NULL);
  cyaml_destroy(root);
}

TEST(delete, degenerate_path_consecutive_dots) {
  /* A path with consecutive dots produces an empty intermediate component in
   * the parent path; navigate_y fails and the call must return
   * ccol_key_not_found. */
  char *err = NULL;
  cyaml root = cyaml_parse("a:\n  b: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a..b"), ccol_key_not_found);
  REQUIRE_NE((void *)cyaml_get(root, "a.b"), NULL);
  cyaml_destroy(root);
}

/* ========================================================================== */
/*                         MULTI-DOCUMENT PARSING                             */
/* ========================================================================== */

TEST(multi_document, two_scalars_with_markers) {
  /* Two plain scalar documents separated by '---'. */
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
  /* First document has no leading '---'; second document requires it. */
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
  REQUIRE_EQ(cyaml_type(seq0), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(seq0), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq0, 0)), 1LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq0, 1)), 2LL);

  cyaml seq1 = cyaml_list_get(root, 1);
  REQUIRE_EQ(cyaml_type(seq1), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(seq1), (size_t)2);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq1, 0)), 3LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_list_get(seq1, 1)), 4LL);
  cyaml_destroy(root);
}

TEST(multi_document, with_document_end_markers) {
  /* Explicit '...' end markers between documents. */
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
  /* '---' immediately followed by '---' produces a null document. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\nhello\n---\n---\nworld\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)3);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(root, 0)), "hello");
  REQUIRE_EQ(cyaml_type(cyaml_list_get(root, 1)), CYAML_NULL);
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
  /* A single document is returned directly, not wrapped in a list. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\na: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_DICTIONARY);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(root, "a")), 1LL);
  cyaml_destroy(root);
}

TEST(multi_document, anchors_do_not_leak_across_documents) {
  /* An anchor defined in doc1 must not be resolvable in doc2. */
  char *err = NULL;
  cyaml root = cyaml_parse("---\nval: &anchor 42\n---\nref: *anchor\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)root, NULL);
  free(err);
}

TEST(multi_document, doc_start_marker_in_mapping_value_is_data) {
  /* '---' that appears as the value of a key (same line) is plain string data,
   * NOT a document boundary. */
  char *err = NULL;
  cyaml root = cyaml_parse("key: ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_get(root, "key")), "---");
  cyaml_destroy(root);
}

TEST(multi_document, triple_doc_start_in_sequence_value_is_data) {
  /* '---' after '- ' (sequence item indicator) is plain string data. */
  char *err = NULL;
  cyaml root = cyaml_parse("- ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cyaml_type(root), CYAML_LIST);
  REQUIRE_EQ(cyaml_list_len(root), (size_t)1);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(root, 0)), "---");
  cyaml_destroy(root);
}

TEST(multi_document, trailing_content_after_end_marker_is_error) {
  /* Bare content after '...' that is not a new '---' is a parse error. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n...\nsome garbage\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)root, NULL);
  free(err);
}

TEST(multi_document, parse_n_multi_document) {
  /* cyaml_parse_n also supports multi-document streams. */
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
  /* cyaml_parse_n with len=0 must behave like an empty document: return a
   * CYAML_NULL node with no error. */
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
  /* A string containing a bare ',' must be double-quoted in flow output.
   * Without quoting, {key: a,b} would be mis-parsed as two separate entries. */
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
  /* A string containing ']' mid-string must be quoted when inside a flow
   * sequence; otherwise the plain scalar terminates early at ']'. */
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
  /* A string containing '}' mid-string must be quoted when inside a flow
   * mapping; otherwise the plain scalar terminates early at '}'. */
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
  /* A colon immediately followed by ',' must be quoted; the plain scalar
   * parser treats ':,' as the end of the value indicator. */
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
  /* A colon immediately followed by ']' must be quoted in flow sequences. */
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
  /* A colon immediately followed by '}' must be quoted in flow mappings. */
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
  /* A %YAML directive line before '---' must be silently accepted. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, tag_directive_ignored) {
  /* A %TAG directive line before '---' must be silently accepted. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%TAG ! foo:\n---\nval: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "val")), 1LL);
  cyaml_destroy(doc);
}

TEST(directives, multiple_directives_ignored) {
  /* Multiple directive lines before '---' must all be silently accepted. */
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

TEST(directives, node_tag_ignored) {
  /* A local node tag (!foo) on a scalar must be stripped; implicit typing
   * rules then resolve the bare value normally. */
  char *err = NULL;
  cyaml doc = cyaml_parse("value: !foo 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "value")), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "value")), 42LL);
  cyaml_destroy(doc);
}

TEST(directives, double_exclamation_tag_ignored) {
  /* A secondary tag (!!str) on a scalar must be stripped; the bare value is
   * resolved by the implicit-typing rules regardless of the tag. */
  char *err = NULL;
  cyaml doc = cyaml_parse("val: !!str 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  /* !!str tag stripped; 42 resolves as an integer. */
  REQUIRE_EQ(cyaml_type(cyaml_dictionary_get(doc, "val")), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "val")), 42LL);
  cyaml_destroy(doc);
}

/* ========================================================================== */
/*                    DOCUMENT-START MARKER EDGE CASES                        */
/* ========================================================================== */

TEST(block_mapping, indented_triple_dash_is_plain_scalar) {
  /* '---' appearing after leading whitespace (column > 0) is NOT a document-
   * start marker; it must be parsed as the plain string "---". */
  char *err = NULL;
  cyaml doc = cyaml_parse("   ---\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---");
  cyaml_destroy(doc);
}
