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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <time.h>

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

/* Single-fault-injection allocator: unlike g_counting_mp above (which fails
 * every call once its budget hits zero, so a message allocated AFTER the
 * failure that triggered the whole parse to fail can itself never succeed),
 * this fails exactly the g_single_fail_at'th allocation call and lets every
 * other call, before or after it, succeed normally. Needed to actually
 * exercise a specific allocation's own failure while still leaving enough
 * "budget" for whatever error message gets built afterward to be
 * allocatable, mirroring the identical pattern already used in
 * tests/chttp/tests.c and tests/chttpclient/tests.c. -1 means never fail. */
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
  /* cyaml_dictionary_get returns a borrowed reference; handing that exact
   * pointer back to cyaml_dictionary_set for the SAME key (old_child ==
   * child) previously destroyed the node the dictionary slot still
   * pointed to right after re-storing that same pointer, corrupting (or,
   * for a custom allocator without the thread-local pool, freeing) the
   * live node still reachable from the dictionary. */
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
  /* An empty document (zero bytes) is a null value; an empty plain scalar
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

TEST(implicit_types, integer_decimal_leading_zero) {
  /* YAML 1.2 core schema's decimal int grammar is [-+]?[0-9]+, with no
   * leading-zero restriction (unlike the stricter JSON schema's own
   * (0|[1-9][0-9]*)); "007" is a plain, unambiguous integer 7, not a
   * string. Locks in try_parse_int_scalar's actual, correct behavior,
   * which a stale internal comment had previously mis-described. */
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

TEST(implicit_types, hex_with_embedded_sign_is_not_a_valid_integer) {
  /* The core schema's own grammar for this form is exactly
   * "0x" [0-9a-fA-F]+, with no room for a sign between the prefix and the
   * digits. strtoull() itself is more permissive than that (it accepts an
   * optional leading '+'/'-' before the digit sequence it consumes), which
   * previously let a malformed literal like "0x-0" be silently accepted as
   * a valid CYAML_INTEGER (magnitude 0) instead of falling back to
   * CYAML_STRING like any other non-numeric plain scalar. */
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
  /* A sign directly after the overall literal (before "0x") is legitimate
   * and unrelated to the embedded-sign bug above; "-0x1" is a genuinely
   * valid negative hex integer and must still parse as one. */
  char *err = NULL;
  cyaml n = cyaml_parse("-0x1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), -1LL);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_hex_overflow_falls_back_to_float) {
  /* 0xFFFFFFFFFFFFFFFF (2^64-1) does not fit in a signed 64-bit long long;
   * it must fall back to CYAML_FLOAT, mirroring how a too-wide decimal
   * literal already falls back, rather than silently reinterpreting the
   * bit pattern as a negative long long. */
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
  /* -0x8000000000000000 (-2^63) is exactly LLONG_MIN and DOES fit; must be
   * accepted as CYAML_INTEGER without invoking signed-overflow UB while
   * computing the negation. */
  char *err = NULL;
  cyaml n = cyaml_parse("-0x8000000000000000\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), LLONG_MIN);
  cyaml_destroy(n);
}

TEST(implicit_types, integer_octal_overflow_falls_back_to_float) {
  /* 0o1777777777777777777777 = 2^64-1 in octal; same overflow class as the
   * hex case above, and must fall back to CYAML_FLOAT the same way,
   * rather than silently reinterpreting the bit pattern as a negative
   * long long or dropping to CYAML_STRING for lack of any other
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
  /* The core schema's own grammar for the int/float hex fallback forms is
   * exactly lowercase "0x" [0-9a-fA-F]+; an uppercase "0X" prefix has no
   * core-schema numeric representation at all and must resolve to an
   * ordinary CYAML_STRING, the same as any other non-numeric plain scalar
   * (verified against a reference parser). */
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
  /* An in-range lowercase "0x..." literal that overflows int64 falls back
   * to CYAML_FLOAT (see integer_hex_overflow_falls_back_to_float above);
   * the identical magnitude with an uppercase "0X" prefix has no valid
   * numeric interpretation of ANY kind (not even the float fallback) and
   * must stay a CYAML_STRING. This also guards against strtod()'s own
   * native hex-float extension (which recognizes "0X" case-insensitively
   * per the C standard) silently reinterpreting this as a hex float were
   * the uppercase rejection ever removed from try_parse_float_scalar. */
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
  /* 5e-324 is a real, valid IEEE-754 denormal double; strtod() sets
   * errno=ERANGE on this legitimate underflow just as it does on true
   * overflow, so a bare errno==ERANGE check must not be used to reject
   * it; only a result that actually clamped to +-infinity is a real
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
  /* 1e-400 legitimately underflows all the way to 0.0; still a
   * correctly-computed CYAML_FLOAT, not CYAML_STRING. */
  char *err = NULL;
  cyaml n = cyaml_parse("1e-400\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(n), 0.0);
  cyaml_destroy(n);
}

TEST(implicit_types, hex_float_syntax_is_not_a_yaml_float) {
  /* strtod() accepts C99 hex-float syntax (0x1p3) as a GNU/C99 extension,
   * but it has no place in YAML 1.2's core schema float grammar (decimal
   * only); it must fall through to CYAML_STRING like any other
   * non-numeric-looking scalar, not be silently misinterpreted as the
   * float value 8.0. */
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
  /* A pure hex-digit run with no p/P exponent is never real hex-float
   * syntax; the hex_float_syntax_is_not_a_yaml_float guard above must not
   * regress the pre-existing, still-desired
   * integer_hex_overflow_falls_back_to_float behavior for it. Uses
   * 0x8000000000000001 (2^63+1) rather than that test's own
   * "0xFFFFFFFFFFFFFFFF" (2^64-1) so this exercises a distinct overflow
   * magnitude, still safely between 2^63 (exclusive) and 2^64-1
   * (inclusive) so it genuinely takes the float-fallback path rather than
   * the wider "does not even fit in uint64, falls back to CYAML_STRING
   * instead" case one more hex digit would trigger. */
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

TEST(implicit_types, string_nan_with_parenthesized_payload) {
  /* glibc's strtod() also accepts C99's "nan(n-char-sequence)" syntax
   * (e.g. "nan(123)"), which is not part of the YAML 1.2 core schema
   * float grammar (only the dot-prefixed/bare forms above are); without
   * an explicit pre-filter, the generic strtod() fallback would silently
   * accept this as a NaN float, discarding the original text. */
  char *err = NULL;
  cyaml n = cyaml_parse("nan(123)\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "nan(123)");
  cyaml_destroy(n);
}

TEST(implicit_types, string_nan_with_empty_parens_and_sign) {
  /* Same class, covering the sign-prefixed and empty-payload variants
   * strtod() also accepts on its own. */
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
  /* An explicit !!float tag reaches try_parse_float_scalar() directly
   * (never via make_typed_scalar()'s int-first cascade), so this
   * exercises the same guard from a second call site. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float nan(3)\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(implicit_types, flow_dict_nan_parenthesized_keys_do_not_collide) {
  /* Regression guard for the key-collision consequence: if "nan(1)" and
   * "nan(2)" both silently canonicalized to the float NaN's own "nan"
   * dictionary key text, the first entry would be destroyed by the
   * second. */
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

TEST(quoted, double_quoted_full_escape_table) {
  /* double_quoted_escapes above only covers \n and \t; this drives every
   * other single-character escape in YAML 1.2 sec. 5.7 not already covered
   * by a dedicated test elsewhere in this suite (\x/\u/\U have their own
   * tests): \a \b \v \f \r \e, an escaped space, \" , \/, and the three
   * Unicode line/space separator escapes \N \_ \L \P. */
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
  /* \U takes 8 hex digits directly (no surrogate pairing needed) for a
   * codepoint above the Basic Multilingual Plane. U+1F600 in UTF-8 is
   * F0 9F 98 80, matching the \u surrogate-pair test above. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\U0001F600\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xF0\x9F\x98\x80");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_big_unicode_escape_out_of_range_substitutes_fffd) {
  /* \U names an arbitrary 32-bit hex value; anything above the maximum
   * valid Unicode scalar value (0x10FFFF) has no valid encoding.
   * encode_utf8() previously truncated the out-of-range value into a
   * structurally invalid UTF-8 byte sequence instead of substituting
   * U+FFFD the way the \u escape's own invalid-surrogate handling
   * already does. U+FFFD in UTF-8 is EF BF BD. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\UFFFFFFFF\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xEF\xBF\xBD");
  cyaml_destroy(n);

  err = NULL;
  n = cyaml_parse("\"\\U00110000\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "\xEF\xBF\xBD");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_big_unicode_escape_lone_surrogate_substitutes_fffd) {
  /* A \U escape naming a UTF-16 surrogate codepoint (0xD800-0xDFFF)
   * directly has no valid Unicode scalar value either, the same way a
   * lone \u surrogate does not; substitute U+FFFD rather than embedding
   * the raw surrogate value as invalid UTF-8. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\U0000D800\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xEF\xBF\xBD");
  cyaml_destroy(n);
}

TEST(quoted,
     double_quoted_unpaired_high_surrogate_followed_by_ordinary_escape) {
  /* An unpaired high surrogate (\uD800) immediately followed by a \u
   * escape that is NOT a matching low surrogate must not silently
   * discard the second escape's own character; the first substitutes
   * U+FFFD (EF BF BD) on its own, and the second is processed
   * independently, yielding "A" (0x41), not just a single U+FFFD. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\uD800\\u0041\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n),
                "\xEF\xBF\xBD"
                "A");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_two_consecutive_unpaired_high_surrogates) {
  /* Neither high surrogate pairs with what follows it (the second is
   * itself another high surrogate, which cannot be a valid low
   * surrogate); each substitutes its own, independent U+FFFD. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\uD800\\uD800\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xEF\xBF\xBD\xEF\xBF\xBD");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_unpaired_high_surrogate_then_valid_pair) {
  /* A chain: an unpaired high surrogate (its own lone U+FFFD) followed by
   * a genuinely valid surrogate pair (U+1F600), confirming the unpaired
   * one doesn't swallow or corrupt a real pair that happens to follow
   * it. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\uD800\\uD83D\\uDE00\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xEF\xBF\xBD\xF0\x9F\x98\x80");
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

TEST(quoted, double_quoted_escaped_newline_then_blank_line_joins_with_one_lf) {
  /* Per YAML 1.2 sec. 8.1.2's s-double-escaped production ("\" b-non-content
   * l-empty(n,flow-in)* s-flow-line-prefix(n)), only the escaped break
   * ITSELF is non-content; each l-empty blank line that follows it still
   * ends in a real b-as-line-feed, exactly like an ordinary unescaped
   * break's own blank-line folding does. "a", then an escaped newline, then
   * one genuinely blank line, then "b" must join with exactly one literal
   * newline (contributed by the blank line), never a space and never
   * nothing at all. Verified against two independent reference parsers
   * (PyYAML, Ruby's Psych/libyaml), both of which agree on "a\nb". */
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
  /* Same as above with two consecutive blank lines between the escaped
   * break and the next real content: each blank line contributes its own
   * literal newline, so two blank lines join with two, confirming the
   * blank-line loop's per-line accounting rather than a single fixed
   * newline regardless of count. Verified against PyYAML and Psych, both
   * of which agree on "a\n\nb". */
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
  /* A blank line inside a literal block scalar terminated by a standalone
   * '\r' (no following '\n') must be recognized as its own, one-line-long
   * blank line; skip_to_eol previously only stopped at '\n', so it would
   * scan straight through the following content line looking for the
   * next real '\n', silently discarding that whole line from the
   * scalar's value. */
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
  /* Same over-consumption bug as literal_blank_line_bare_cr_does_not_
   * swallow_next_line above, but in the folded scalar path, whose own
   * per-line skip_to_eol call is shared by both blank AND content lines
   * (parse_block_scalar_content's own equivalent call is reached only
   * from its blank-line branch). */
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

TEST(errors, block_scalar_duplicate_indentation_indicator_rejected) {
  /* c-b-block-header permits at most one indentation indicator; a second
   * digit previously silently overwrote the first ("last one wins")
   * instead of being rejected. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: |24\n    x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, block_scalar_duplicate_chomping_indicator_rejected) {
  /* c-b-block-header permits at most one chomping indicator; two of the
   * same kind, or two different kinds stacked, previously both silently
   * accepted "last one wins" semantics instead of being rejected. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: |--\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);

  err = NULL;
  doc = cyaml_parse("a: |++\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);

  err = NULL;
  doc = cyaml_parse("a: |+-\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);

  err = NULL;
  doc = cyaml_parse("a: |-+\n  x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
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

TEST(block_scalars, folded_blank_line_adjacent_to_more_indented_line) {
  /* YAML 1.2 sec. 8.1.3: a transition into or out of a more-indented run of
   * lines costs one literal newline ON TOP OF however many blank lines
   * separate the two chunks; the two are additive, not alternatives.
   * Confirmed against a reference parser. */
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
  /* Same additive rule, transitioning out of a more-indented run back to
   * normal content via an intervening blank line. */
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
  /* The have_content guard on the new additive newline must not misfire for
   * leading blank lines before the scalar's own very first content line
   * (there is no preceding content line for such a break to be "adjacent
   * to"). An explicit indentation indicator is used so the first content
   * line ("more") is genuinely more-indented relative to the DECLARED
   * indent (2) rather than relative to auto-detection (which would instead
   * just adopt "more"'s own column as the base, making it trivially
   * non-more-indented relative to itself). Confirmed against a reference
   * parser: only the leading blank line's own newline is emitted, with no
   * additional transition newline on top of it. */
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
  /* An implicit key's own core-schema type must be resolved before it is
   * stored, exactly like a flow dictionary key, a flow sequence's
   * "key: value" shorthand, and an explicit "? key" block key already
   * do: "~", "null", and an explicit "? ~" key are all equivalent
   * spellings of the same value under YAML 1.2 and must collide on the
   * identical "null" dictionary key. Without this, "? ~\n: 1\n~: 2\n"
   * kept two separate entries ("~" and "null") instead of the one a
   * consistent implementation produces. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? ~\n: 1\n~: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(doc), (size_t)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "null")), 2LL);
  cyaml_destroy(doc);
}

TEST(block_mapping, implicit_bool_and_int_keys_canonicalize) {
  /* Further spellings of the same principle: an implicit "TRUE" key
   * canonicalizes to "true" (matching an explicit "? TRUE" key), and an
   * implicit "0x10" key canonicalizes to "16" (matching "? 0x10"). */
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
  /* The first entry of a mapping is discovered by a separate code path
   * (parse_node's own top-level plain-scalar dispatch) from every later
   * entry (parse_one_dict_entry_key); this exercises the fix at both
   * positions in one document, not just the first. */
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
   * exactly like an un-anchored one, while the anchor itself still
   * resolves to the key's real, typed value (an integer here, not the
   * string "16") for a later alias - the two are handled by separate
   * code paths (a cloned typed node for the alias, a canonicalized
   * string for dictionary storage) that must stay consistent. */
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

TEST(block_mapping, utf8_bom_skipped_multiline) {
  /* Regression guard: the BOM must not shift where line_start_pos()
   * thinks the first physical line begins.  A single-key document (see
   * utf8_bom_skipped above) cannot catch this, since it never compares a
   * SECOND line's own column against anything - a second key at column 0
   * needs the first line to genuinely start at byte 3 (right after the
   * BOM), not byte 0, or every column on that line comes out 3 bytes too
   * high and this document is wrongly rejected as "trailing content". */
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
  /* Same underlying bug, reached via at_doc_marker()'s own column-0
   * requirement instead of an ordinary indentation comparison: a "---"
   * immediately after the BOM must be recognized as a real document-start
   * marker, not silently absorbed as three bytes of plain-scalar text. */
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
  /* The most severe form of the same bug: with the BOM's own 3 bytes
   * wrongly counted as part of line 1's indentation, the first "---"
   * failed at_doc_marker()'s column-0 check and was silently absorbed as
   * plain-scalar text ("--- a"), merging what should be two separate
   * documents into one and never reporting any error at all. */
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
  /* production: *def must actually resolve the alias to the anchored
   * mapping's own content; a version of this test that defines &def but
   * never references it via *anywhere could not detect any alias-
   * resolution regression at all. */
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

TEST(anchors, many_linear_aliases_to_small_anchor_still_works) {
  /* Regression guard for the node-allocation budget (CYAML_MAX_PARSE_NODES,
   * see exponential_alias_expansion_rejected_not_exhausted): repeated
   * aliasing of the SAME small anchor is ordinary, legitimate linear reuse
   * (each *a clone is independent but small), nowhere near the budget, and
   * must not be false-positively rejected. */
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

TEST(path, set_integer_various_widths_and_signs_round_trip) {
  /* Every existing cyaml_set() integer test passes a bare int literal,
   * which only ever exercises node_reinit_scalar's raw_size == 4, is_signed
   * == true case. This drives every other combination in that function's
   * own raw_size (1/2/4/8) x is_signed (true/false) switch, matching every
   * width _cyaml_type_of()/_cyaml_is_signed() dispatch on. */
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
  /* cyaml_set on a sequence index beyond the last element must fail without
   * modifying the existing elements, with ccol_key_not_found specifically
   * (a syntactically valid but out-of-range "#N" component is an absent
   * path component, exactly like a missing dictionary key would be;
   * mirrors cyaml_delete's identical, explicitly documented distinction -
   * see delete.path_out_of_range_list_index_returns_key_not_found). */
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
  REQUIRE_EQ(cyaml_set(doc, "key.sub", 1), ccol_invalid_args);

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

  REQUIRE_EQ(cyaml_set(doc, "missing.sub", 1), ccol_key_not_found);

  /* Original key must be untouched. */
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  cyaml_destroy(doc);
}

TEST(path,
     set_malformed_index_syntax_in_non_leaf_component_returns_invalid_args) {
  /* A malformed "#N" index is a caller-side path-construction error, not a
   * data-availability question; and that must hold at ANY position in
   * the path, not just the leaf. Before this distinction was implemented,
   * navigate_y() collapsed a malformed mid-path index and a genuinely
   * absent mid-path component into the same NULL result, so this returned
   * ccol_key_not_found instead. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "items.#xyz.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(doc, "items")), (size_t)2);

  cyaml_destroy(doc);
}

TEST(path, get_index_with_leading_whitespace_is_malformed) {
  /* Regression test: a bare strtol() call silently accepts leading
   * whitespace before the digits (and an explicit leading '+'), so
   * "items.#  0" and "items.#+0" used to be silently accepted as index 0
   * instead of being rejected as the documented "#N" grammar (digits
   * only, immediately after '#') requires. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ((void *)cyaml_get(doc, "items.#  0.a"), NULL);
  REQUIRE_EQ((void *)cyaml_get(doc, "items.#+0.a"), NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#0.a")), 1LL);

  cyaml_destroy(doc);
}

TEST(path, set_index_with_leading_whitespace_returns_invalid_args) {
  /* Mirrors path.get_index_with_leading_whitespace_is_malformed's own two
   * forms (leading whitespace and an explicit leading '+') so a regression
   * reinstating strtol()-style sign acceptance on the set path specifically
   * (distinct from the get/delete paths, which have their own dedicated
   * coverage for the '+' form below) cannot slip through undetected. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "items.# 0.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_set(doc, "items.#+0.a", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "items.#0.a")), 1LL);

  cyaml_destroy(doc);
}

TEST(delete, path_index_with_leading_sign_returns_invalid_args) {
  /* Mirrors path.get_index_with_leading_whitespace_is_malformed's own two
   * forms (an explicit leading '+' and leading whitespace) so a regression
   * reinstating strtol()-style whitespace acceptance on the delete path
   * specifically cannot slip through undetected. */
  char *err = NULL;
  cyaml doc = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_delete(doc, "items.#+0"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_delete(doc, "items.# 0"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(doc, "items")), (size_t)2);

  cyaml_destroy(doc);
}

TEST(path, set_wrong_type_in_non_leaf_component_returns_invalid_args) {
  /* A scalar encountered mid-path (with further components still
   * remaining beyond it) has no navigable children; that is a wrong-type
   * path-construction error (ccol_invalid_args), not an absent-component
   * error (ccol_key_not_found). "a" is itself the scalar 1, so "a.b.c"
   * must fail while still trying to resolve the "a.b" parent segment,
   * inside navigate_y() itself. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ(cyaml_set(doc, "a.b.c", 9), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "a")), 1LL);

  cyaml_destroy(doc);
}

TEST(path, set_null_value) {
  /* cyaml_set with a NULL literal must change the leaf's type to CYAML_NULL.
   * typeof(NULL) == void * which _cyaml_type_of maps to CYAML_NULL via its
   * own explicit void * association (distinct from the default branch,
   * which instead produces _CYAML_TYPE_UNSUPPORTED for a genuinely
   * unsupported type; see set_unsupported_type_is_rejected below). */
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
  /* A C value whose type _cyaml_type_of() does not recognise (e.g. long
   * double) must be rejected with ccol_invalid_args, leaving the existing
   * leaf completely untouched, rather than silently succeeding and
   * overwriting it with CYAML_NULL. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: 42\n", &err);
  REQUIRE_EQ((void *)err, NULL);

  long double ld = 3.14L;
  REQUIRE_EQ(cyaml_set(doc, "key", ld), ccol_invalid_args);
  REQUIRE_EQ(cyaml_type(cyaml_get(doc, "key")), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(doc, "key")), 42LL);

  /* Also rejected when the leaf doesn't exist yet (the node_make_scalar
   * path, distinct from node_reinit_scalar's existing-leaf path above). */
  REQUIRE_EQ(cyaml_set(doc, "brand_new_key", ld), ccol_invalid_args);
  REQUIRE_EQ((void *)cyaml_get(doc, "brand_new_key"), NULL);

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

  REQUIRE_EQ(cyaml_set(doc, "key.", 1), ccol_invalid_args);
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
  /* Reset before checking the result: a REQUIRE_* failure returns from this
   * function immediately, and leaving the reset until after it would pin
   * g_alloc_remaining at its exhausted budget for every later test in this
   * binary that uses &g_counting_mp, turning one clear failure here into a
   * cascade of unrelated-looking ones. */
  g_alloc_remaining = -1;
  REQUIRE_EQ(r, ccol_not_enough_memory);

  /* The document must be unmodified. */
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
  /* See set_single_component_oom's identical comment above for why the
   * reset happens before this check rather than after it. */
  g_alloc_remaining = -1;
  REQUIRE_EQ(r, ccol_not_enough_memory);

  /* The document must be unmodified. */
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

  /* Mutate original; copy should not change. Both the mutation itself and
   * its effect on original are asserted, not just copy's own unchanged
   * value: without them, a regressed cyaml_set() that silently no-ops (or
   * a fully shallow/aliasing cyaml_clone()) would pass this test just as
   * well as the correct, independent-deep-copy behavior it exists to
   * verify. */
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
  /* When the first node allocation inside cyaml_clone fails the function
   * must return NULL without leaking anything. */
  g_alloc_remaining = -1;
  cyaml src = cyaml_create_string_mp("hello", &g_counting_mp);
  REQUIRE_NE((void *)src, NULL);

  g_alloc_remaining = 0;
  cyaml copy = cyaml_clone(src);
  /* See path.set_single_component_oom's identical comment for why the
   * reset happens before this check rather than after it. */
  g_alloc_remaining = -1;
  REQUIRE_EQ((void *)copy, NULL);

  /* src must still be intact and destroyable. */
  REQUIRE_STREQ(cyaml_str_val(src), "hello");
  cyaml_destroy(src);
}

TEST(clone, dictionary_iterator_oom_never_returns_incomplete_clone) {
  /* chashmap_begin_iter() returns NULL both when a map is genuinely empty
   * and when its own small per-iteration allocation fails on a non-empty
   * map (a real OOM); naively treating both cases as "nothing to clone"
   * would let this function return a non-NULL "successful" clone silently
   * missing every key, violating its own documented "NULL on allocation
   * failure" contract. Exhaustively fails at every allocation budget from
   * 0 up through comfortably past this whole clone's real allocation
   * count and checks the invariant holds at each one: cyaml_clone must
   * never return a non-NULL dictionary clone with fewer entries than the
   * source. */
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
      /* Cleanup happens before the REQUIRE_* check below, not after: an
       * OOM-sweep test leaking its own copy on an already-failing
       * assertion would be indistinguishable, under valgrind, from a real
       * leak in the code under test, defeating the point of running this
       * test under valgrind at all. */
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
  /* A tree handed to cyaml_clone() need not come from cyaml_parse() at all
   * (which is separately, and much more shallowly, bounded by
   * CYAML_MAX_PARSE_DEPTH): building one directly via cyaml_create_list() +
   * cyaml_list_push() has no depth restriction at construction time at all,
   * so cyaml_clone() must independently bound its own recursion (mirroring
   * cyaml_serialize()'s identical CYAML_MAX_SERIALIZE_DEPTH guard; see
   * serialize.excessive_depth_rejected_not_crashed above), reporting NULL
   * (its own documented "NULL on allocation failure" contract) rather than
   * crashing via stack overflow on an otherwise perfectly acyclic, merely
   * deep tree. depth (1000) matches serialize.excessive_depth_rejected_
   * not_crashed's own choice: comfortably past the 500-level cap while
   * staying shallow enough that this test's own cleanup (cyaml_destroy's
   * tree walk, which is not depth-guarded) has no risk of exhausting the
   * stack itself. */
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
  /* Regression guard for the depth cap above: a tree comfortably within
   * CYAML_CLONE_MAX_DEPTH (500) must still clone correctly and completely,
   * not be spuriously rejected by an off-by-one in the new depth check. */
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
  /* A dictionary built incrementally while parsing (cyaml_dictionary_set
   * called once per entry across many iterations, in parse_flow_dictionary/
   * parse_block_dictionary/parse_flow_list/merge_one_source_into) and then
   * destroyed wholesale on some LATER, unrelated parse failure must never
   * leak an already-inserted entry: __cyaml_destroy's own dictionary
   * cleanup (node_clear) reaches every child via chmap_destroy_with_dtor
   * (chashmap.h), which walks the map's own internal storage directly and
   * therefore never needs to allocate to do so, unlike enumerating the map
   * via chashmap_begin_iter() first (whose own small internal allocation
   * can itself fail under sustained, not merely transient, OOM).
   *
   * This exhaustively fails at every allocation budget across six
   * documents chosen to exercise every incremental dictionary-building
   * loop in the parser, including a dictionary nested inside a flow
   * dictionary's own value (a dictionary that finishes building
   * successfully and is only later torn down as part of an enclosing
   * failure) and a merge-key ("<<") expansion. This test cannot itself
   * detect a leak (tau has no built-in leak checker); its purpose is to
   * exercise these exact code paths under `make memtest` (valgrind),
   * which already runs this whole suite; a regression here is expected
   * to be caught there, not by any assertion in this function. */
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
      if (doc)
        cyaml_destroy(doc);
      else
        free(err);
    }
  }
}

TEST(
    oom,
    dictionary_entry_key_resolution_never_returns_a_truncated_document_under_sustained_allocation_failure) {
  /* try_parse_scalar_dict_key()'s alias-as-key branch (cyaml_clone() on
   * the aliased node) and its anchored-flow-collection-as-key branch
   * (parse_flow_list()/parse_flow_dictionary() failing at their very
   * first allocation) can both fail purely on allocator OOM without ever
   * calling parse_err(); every caller of this function distinguishes "not
   * a dictionary entry after all" from "a genuine error" purely by
   * whether ctx->error was set (see this function's own doc comment), so
   * an unreported OOM there must never be silently treated as "stop this
   * dictionary here, return what's been parsed so far as a complete,
   * successful document." Exhaustively fails at every allocation budget
   * across one document per affected branch and checks the invariant
   * holds: a non-NULL result must always be the complete, correct
   * document, never a truncated one missing later entries. */
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
        /* Cleanup happens before the REQUIRE_* check below, not after: an
         * OOM-sweep test leaking its own doc on an already-failing
         * assertion would be indistinguishable, under valgrind, from a
         * real leak in the code under test, defeating the point of
         * running this test under valgrind at all. Mirrors this file's
         * own established discipline for every other allocator-fault
         * test's baseline/reset step (see e.g. the anchored-key OOM test
         * above). */
        size_t actual_size = cyaml_dictionary_size(doc);
        cyaml_destroy(doc);
        REQUIRE_EQ(actual_size, expected_sizes[d]);
      } else {
        /* err itself may legitimately be NULL too: allocating the error
         * message string can itself fail under a sufficiently tight
         * budget (see the identical, unchecked else-branch in the
         * pre-existing parsing_a_dictionary_never_leaks... test above). */
        free(err);
      }
    }
  }
}

TEST(oom, scalar_dictionary_key_oom_reports_specific_message) {
  /* node_to_dict_key_string()'s five scalar branches (STRING/INTEGER/FLOAT/
   * NULL/BOOL) previously reported no diagnostic of their own on a bare
   * ccol_strdup OOM, unlike the LIST/DICTIONARY branch right next to them.
   * parse_flow_list's "[1: a]" shorthand-entry call site (unlike
   * try_parse_scalar_dict_key, which has its own fallback) has no
   * fallback message of its own either, so an OOM converting the integer
   * key "1" to a string fell all the way through to parse_common's own
   * generic "unknown parse error" fallback instead of a real diagnostic.
   *
   * Uses the single-fault allocator (rather than g_counting_mp's own
   * budget style) specifically because a budget-style failure leaves zero
   * allocation headroom for whatever error message gets built afterward
   * (every call after the one that hits zero also fails), which would
   * make err always NULL and defeat the point of this test; failing
   * exactly one call lets the rest of the parse, including building the
   * error string, proceed normally. Sweeps every call index and checks
   * that the specific new message is reached at some index; deliberately
   * does not assert anything about every OTHER failing index (several
   * other, unrelated allocation sites elsewhere in the parser have their
   * own, pre-existing, out-of-scope gaps of the same "falls through to
   * the generic fallback" kind, which this test is not responsible for
   * and must not treat as a regression here). */
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
      free(err);
    }
  }
  REQUIRE_TRUE(saw_specific_oom_message);
}

TEST(oom, anchor_or_tag_decorated_key_oom_never_silently_succeeds) {
  /* try_parse_scalar_dict_key()'s own doc comment promises that a "not a
   * key after all" return always leaves ctx->pos restored to exactly where
   * it was on entry, with ctx->error[0] the only way a caller distinguishes
   * that from a genuine error. parse_anchor_name() and parse_tag_token()
   * (called speculatively from inside try_parse_scalar_dict_key while
   * scanning a possible "&anchor key:"/"!!tag key:" prefix) used to return
   * false on their own _mem_alloc OOM without calling parse_err() at all,
   * silently violating that contract: the caller saw ctx->error[0] == 0
   * and treated it as "not a key, position already restored" even though
   * the position was NOT restored (it had already been advanced past the
   * scanned anchor/tag text), corrupting the parse instead of failing it.
   * anchors_store() (called once the key IS confirmed, to register the
   * key's own anchor for later '*name' resolution) had an analogous gap:
   * it returned void and silently discarded its clone on OOM, and its two
   * callers (register_key_anchor(), and the ordinary whole-node anchor
   * branch) both then reported success regardless.
   *
   * This document exercises the anchor-decorated-key path specifically
   * (try_parse_scalar_dict_key's '&' property-scanning branch, followed by
   * register_key_anchor()/anchors_store()); every one of its allocations
   * is a single, non-retried _mem_alloc/node_alloc/chmap_insert_elem call
   * (confirmed by direct inspection: this input's dictionary has a single
   * entry, so no chmap iterator retry path is ever exercised), so a real
   * allocation failure at any point during this parse must always cause
   * the whole document to fail; there is no legitimate "should still
   * succeed" outcome for a real fault anywhere in this specific call
   * count, unlike some of this parser's other allocation sites, which can
   * legitimately recover from a single transient failure via
   * chmap_begin_iter_safe's own built-in retry. */
  const char *doc_src = "&a key: value\n";

  g_single_call_idx = 0;
  g_single_fail_at = -1;
  char *baseline_err = NULL;
  cyaml baseline = cyaml_parse_mp(doc_src, &baseline_err, &g_single_fault_mp);
  int total_calls = g_single_call_idx;
  /* Cleanup happens before the REQUIRE_* checks below, not after: unlike an
   * ordinary test (where a leak on an already-failing assertion is this
   * file's accepted, uniform convention), an OOM-sweep test leaking its own
   * baseline would be indistinguishable, under valgrind, from a real leak
   * in the code under test, defeating the point of running this test under
   * valgrind at all. Mirrors this file's own established discipline for
   * every other allocator-fault test's baseline/reset step. */
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
    if (err) free(err);
    /* The real bug symptom: doc non-NULL despite an allocation that was
     * made to fail. A correct parser must report failure here, every
     * time, for this specific document. Freed before the REQUIRE_EQ (which
     * would otherwise return past this cleanup on failure), for the same
     * reason given above. */
    bool doc_was_null = (doc == NULL);
    if (doc) cyaml_destroy(doc);
    REQUIRE_TRUE(doc_was_null);
  }
}

TEST(oom, unreported_allocation_failure_reports_honest_fallback_not_vague_one) {
  /* parse_common's own top-level fallback message, reached whenever a
   * document fails to parse with ctx->error still empty, used to say the
   * flatly unhelpful "unknown parse error" - a string that gives a caller
   * debugging a real OOM no indication of what actually happened. Since
   * every genuine syntax rejection in this parser reports a specific
   * diagnostic via parse_err() before returning failure (this file's own
   * established, audited convention throughout), the only way to reach
   * this fallback with ctx->error empty is an allocation that failed
   * somewhere deep in the call chain with no diagnostic of its own to
   * report (many low-level DOM construction helpers are shared with the
   * public, non-parsing API and have no parse_ctx_t to report through at
   * all) - confirmed empirically by sweeping the single-fault allocator
   * across a wide variety of document shapes (block/flow mappings and
   * sequences, scalars of every type, anchors, aliases, tags, merge keys,
   * directives, block scalars, deeply nested structures) and finding the
   * literal string "unknown parse error" unreachable in every one of them
   * once this fix landed. This test covers just two representative shapes
   * (a plain block mapping and a flow sequence) directly; the fix's own
   * comment at its call site names the broader reasoning. */
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
        /* Cleanup happens before the REQUIRE_* check below, not after: an
         * OOM-sweep test leaking its own err string on an already-failing
         * assertion would be indistinguishable, under valgrind, from a
         * real leak in the code under test, defeating the point of
         * running this test under valgrind at all. */
        bool is_specific = strcmp(err, "unknown parse error") != 0;
        if (strstr(err, "out of memory") != NULL)
          saw_specific_oom_message = true;
        free(err);
        REQUIRE_TRUE(is_specific);
      }
    }
  }
  REQUIRE_TRUE(saw_specific_oom_message);
}

TEST(
    oom,
    plain_scalar_multiline_continuation_never_crashes_under_allocation_failure) {
  /* parse_plain_scalar_multiline's continuation-line loop scans each
   * continuation line's own content directly into the shared accumulator
   * buffer b (see that function's own doc comment on line_floor for why:
   * appending straight into b, trimming trailing whitespace back only to
   * a recorded floor offset, avoids a separate per-line buffer entirely);
   * a transient allocator failure while growing b mid-scan must be
   * reported as a graceful parse failure, not crash. This is also a
   * regression test for a real, previously-fixed strlen(NULL) undefined-
   * behavior bug reachable when this function used a separate, short-
   * lived per-line buffer whose own allocation could fail independently
   * of b's; that separate buffer no longer exists, but the underlying
   * class of bug (an allocation failure during continuation-line scanning
   * corrupting or crashing the parse instead of failing it cleanly) is
   * still worth guarding against on its own. Exhaustively fails at every
   * allocation budget across several plain scalars requiring multi-line
   * continuation, both at the document root and inside a flow collection.
   * This test cannot itself detect a crash via any assertion; its purpose
   * is to exercise this exact path under `make memtest` (valgrind) and
   * plain execution alike, either of which would abort the whole suite on
   * the bug this guards against. */
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
      if (doc)
        cyaml_destroy(doc);
      else
        free(err);
    }
  }
}

/* ========================================================================== */
/*                         SERIALIZATION                                      */
/* ========================================================================== */

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

TEST(serialize, float_whole_number_gets_dot_zero_suffix) {
  /* yb_append_double's "%.15g" formatting of a whole-number double (e.g.
   * 5.0) produces "5", with no '.'/'e'/'E'; left as-is, re-parsing the
   * serialized output would silently produce a CYAML_INTEGER instead of
   * the original CYAML_FLOAT. A dedicated branch appends ".0" to force a
   * float re-parse; this is a direct round-trip-fidelity regression test
   * for that branch. */
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
  /* The public cyaml_serialize_flow() output order is deliberately left
   * completely unchanged by the internal, canonical-mode-only sorting
   * added for non-scalar dictionary key canonicalization (see
   * flow_collections.non_scalar_multi_key_dictionary_key_is_canonically_
   * sorted): it must still emit entries in the dictionary's own chmap
   * iteration order (reverse insertion order for separate chaining), not
   * lexicographically sorted by key. */
  cyaml m = cyaml_create_dictionary();
  cyaml_dictionary_set(m, "a", cyaml_create_int(1));
  cyaml_dictionary_set(m, "b", cyaml_create_int(2));
  cyaml_dictionary_set(m, "c", cyaml_create_int(3));
  char *s = cyaml_serialize_flow(m);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "{c: 3, b: 2, a: 1}");
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

TEST(
    serialize,
    dictionary_never_serializes_incomplete_under_sustained_allocation_failure) {
  /* serialize_block()'s and serialize_flow()'s CYAML_DICTIONARY cases walk
   * the dictionary via chmap_begin_iter_safe(), which can legitimately
   * return NULL for a NON-empty map under sustained allocation failure
   * (see that helper's own doc comment); silently treating that the same
   * as "nothing left to iterate" would let cyaml_serialize()/
   * cyaml_serialize_flow() return a "successful" (non-NULL) string
   * missing one or more of the dictionary's own entries, in violation of
   * their documented "NULL on OOM" contract. Exhaustively fails at every
   * allocation budget and checks the invariant holds: a non-NULL result
   * must always contain every original key. */
  char *err = NULL;
  cyaml src =
      cyaml_parse_mp("a: 1\nb: 2\nc: 3\nd: 4\ne: 5\n", &err, &g_counting_mp);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)src, NULL);
  REQUIRE_EQ(cyaml_dictionary_size(src), (size_t)5);

  /* Every REQUIRE_* below lives inside an "if (block)"/"if (flow)" guard,
   * since most budgets in this sweep are deliberately too small to
   * succeed at all; without also confirming that at least one budget DID
   * succeed, a regression that made cyaml_serialize()/cyaml_serialize_
   * flow() return NULL unconditionally for this tree would silently
   * degrade this test to "does not crash", passing just as well as the
   * correct behavior it exists to verify. */
  bool saw_block_success = false;
  bool saw_flow_success = false;

  for (int budget = 0; budget < 300; budget++) {
    g_alloc_remaining = budget;
    char *block = cyaml_serialize(src);
    g_alloc_remaining = -1;
    if (block) {
      saw_block_success = true;
      /* Cleanup happens before the REQUIRE_* checks below, not after: an
       * OOM-sweep test leaking its own buffer on an already-failing
       * assertion would be indistinguishable, under valgrind, from a real
       * leak in the code under test, defeating the point of running this
       * test under valgrind at all. */
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
      /* See the identical comment on the block-serialization branch above. */
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
  /* Reset before checking the result: see path.set_single_component_oom's
   * identical comment above for why the reset must not happen after a
   * REQUIRE_* check that could itself return from this function early. */
  g_alloc_remaining = -1;
  /* victim is owned (and destroyed) by list_push regardless of outcome. */
  REQUIRE_NE(r, ccol_success);

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
  /* Reset before checking the result: see path.set_single_component_oom's
   * identical comment above for why the reset must not happen after a
   * REQUIRE_* check that could itself return from this function early. */
  g_alloc_remaining = -1;
  /* val is owned (and destroyed) by dictionary_set regardless of outcome. */
  REQUIRE_NE(r, ccol_success);

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
  /* Unlike an earlier version of this test, both tasks actually reference
   * &common via a "<<: *common" merge key rather than spelling out its
   * fields literally, so a regression in anchor registration, alias
   * resolution, or merge-key expansion in a realistic multi-level document
   * is actually caught. This also exercises the anchor-decorated mapping
   * being immediately followed, at the SAME indentation, by a sibling key
   * ("tasks"): historically the exact shape that could get swallowed into
   * the anchor's own value instead of being treated as a separate entry. */
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

  /* First task: both fields come purely from the merge. */
  cyaml task0 = cyaml_list_get(tasks, 0);
  REQUIRE_NE((void *)task0, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(task0, "name")),
                "fetch data");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task0, "timeout")), 30LL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(task0, "max_retries")), 3LL);

  /* Second task: explicit keys override the merged-in values. */
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
  /* A standalone '\r' (no following '\n') must terminate a "# comment" the
   * same way a '\n' or "\r\n" would; skip_to_eol (used by skip_ws_comments)
   * previously only recognized '\n', so a bare-CR-terminated comment would
   * silently consume the entire following line as if it were still part
   * of the comment. */
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
  free(err);
}

TEST(errors, unterminated_single_quote) {
  char *err = NULL;
  cyaml doc = cyaml_parse("'unterminated\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, indented_doc_end_marker_is_plain_scalar_continuation) {
  /* An indented '...' sequence is not a document-end marker; the YAML spec
   * requires document markers to be at column 0.  Since it isn't one, and
   * it is indented more than the enclosing mapping, it is ordinary
   * multi-line plain scalar continuation content (verified against
   * PyYAML: {key: "value ..."}), not an error. */
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

TEST(errors, flow_list_element_error_cleanup_no_leak) {
  /* A flow-list element that parses successfully but is immediately
   * followed by an invalid continuation (a tab used as indentation on the
   * crossed-newline continuation line) must free that already-built
   * element on its way out through the error path, not just the flow list
   * being built so far; found by fuzzing (a mutation of the tags seed
   * corpus reproduced a real leak here before this was fixed). */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: [1\n\t]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, deeply_nested_explicit_keys_rejected_not_hung) {
  /* A chain of "? " explicit-key indicators chained on a single line
   * (each one's own key being the entire remaining chain that follows
   * it) is a real denial-of-service vector found by fuzzing:
   * canonicalizing a non-scalar key to text (node_to_dict_key_string)
   * re-double-quotes the text the level below it already produced, so
   * the canonical string's length (and the time to produce it) grows as
   * O(2^depth), not O(depth); a ~200-byte input with 100 levels
   * previously took an unbounded amount of time/memory. Both new guards
   * are exercised here: the max-canonical-key-length check
   * (CYAML_MAX_CANONICAL_KEY_LEN) rejects this specific pattern at a
   * shallow depth, well before CYAML_MAX_PARSE_DEPTH would ever be
   * reached for it. Must return (rejected) promptly, not hang or crash;
   * asserted directly via clock(), matching this file's own
   * serialize.oom_short_circuits_remaining_siblings_after_depth_exceeded
   * precedent, since tau itself has no per-test timeout and a regression
   * here would otherwise hang the whole binary rather than fail visibly. */
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
  free(err);
}

TEST(errors,
     repeated_explicit_key_lines_separated_by_bare_cr_flatten_not_nest) {
  /* Regression guard for current_col()/line_start_pos()'s own bare-CR fix:
   * a chain of separate "?" lines at the SAME column (as opposed to
   * deeply_nested_explicit_keys_rejected_not_hung's genuinely nested,
   * single-line "? ? ? ..." chain above) are sibling entries, not nested
   * ones, regardless of whether they are separated by '\n' or a bare
   * '\r' (confirmed against PyYAML, which likewise collapses 100
   * repeated "?\n" lines at column 0 into one {null: null} entry; later
   * bare keys simply redefine the same null key). Before
   * current_col()/line_indent_has_tab()'s own bare-CR line-start fix,
   * their backward line-start scan recognized only '\n', so a '\r'-only
   * line ending made every subsequent "?" appear at an ever-increasing
   * (wrong) column, misinterpreting this exact shape as genuine nesting;
   * it must now parse instantly as the same flat, harmless single-entry
   * dictionary a real '\n'-separated version already does. Checking the
   * single entry's key and value type, not just the top-level size, is
   * what actually distinguishes the flat parse from the very nested
   * misparse this test guards against: a nested {null: {null: {...}}}
   * misparse also has exactly one top-level entry, but that entry's own
   * value would be a CYAML_DICTIONARY rather than CYAML_NULL. */
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
  /* CYAML_MAX_CANONICAL_KEY_LEN (64 KiB) guards every non-scalar
   * dictionary key's canonical text, not merely the O(2^depth)
   * deeply-nested-explicit-key shape covered by
   * deeply_nested_explicit_keys_rejected_not_hung above: a single, flat,
   * non-recursive key whose own canonical text alone exceeds the limit
   * must be rejected too. */
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
  free(err);
}

TEST(errors, excessive_parse_nesting_depth_rejected_not_crashed) {
  /* A generic, ordinary (non-exponential) deep-nesting chain (plain
   * block sequences, not the non-scalar-explicit-key pattern above)
   * must still be bounded by CYAML_MAX_PARSE_DEPTH alone, protecting
   * against a plain stack overflow from unbounded recursion depth,
   * independent of the canonical-key-length guard above (which this
   * input never reaches, since none of its keys are non-scalar). Must
   * return (rejected) promptly, not hang or crash; asserted directly via
   * clock(), for the same reason given in
   * deeply_nested_explicit_keys_rejected_not_hung above. */
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
  free(err);
  free(input);
}

TEST(errors, exponential_alias_expansion_rejected_not_exhausted) {
  /* Each alias reference materializes an independent deep clone of its
   * anchor's subtree (cyaml_clone), and anchor registration itself clones
   * the node being anchored; a document that nests aliases of aliases can
   * therefore make the final live node count grow exponentially in the
   * number of anchor levels even though the source text and the parser's
   * own recursion depth both stay small (the classic "billion laughs"
   * entity-expansion shape), a real DoS vector distinct from both guards
   * exercised above (this input never nests explicit keys, and its own
   * recursion depth never remotely approaches CYAML_MAX_PARSE_DEPTH). The
   * node-allocation budget (CYAML_MAX_PARSE_NODES) must reject this
   * promptly rather than exhausting memory or hanging; asserted directly
   * via clock(), for the same reason given in
   * deeply_nested_explicit_keys_rejected_not_hung above. Unlike that test
   * (rejected within microseconds, since the canonical-key-length guard
   * catches it at a shallow depth), this guard only trips once
   * CYAML_MAX_PARSE_NODES worth of clones have actually been materialized
   * and freed, so the bound here is far more generous (measured: ~0.5s
   * plain, ~13s under valgrind's own per-allocation instrumentation
   * overhead on the machine this was verified on; 60s leaves wide margin
   * for a slower or more loaded environment while still being nowhere
   * close to "hangs indefinitely"). */
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
  free(err);
  free(input);
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
  cyaml elem0 = cyaml_list_get(doc, 0);
  REQUIRE_NE((void *)elem0, NULL);
  REQUIRE_EQ(cyaml_type(elem0), CYAML_NULL);
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

TEST(serialize, plus_dot_underflowing_float_strings_are_quoted) {
  /* "+.1e-400" and "+.999e-320" both parse via strtod() with errno==ERANGE:
   * the first underflows all the way to 0.0, the second underflows to a
   * legitimate subnormal. Both are still genuine, successfully-parsed
   * CYAML_FLOAT values per try_parse_float_scalar()'s own ERANGE-vs-actual-
   * overflow distinction (see implicit_types.float_subnormal_underflow_is_
   * still_a_float / float_underflow_to_zero_is_still_a_float), so
   * needs_quoting() must still quote them, exactly like the non-underflowing
   * "+.N" cases above, or a CYAML_STRING holding this text silently
   * reparses as CYAML_FLOAT instead of round-tripping as a string. */
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
  /* A tree handed to cyaml_serialize()/cyaml_serialize_flow() need not
   * come from cyaml_parse() at all (which is separately, and much more
   * shallowly, bounded by CYAML_MAX_PARSE_DEPTH): building one directly
   * via cyaml_create_list() + cyaml_list_push() has no depth restriction
   * at construction time at all, so the serializers must independently
   * bound their own recursion via CYAML_MAX_SERIALIZE_DEPTH, reporting
   * NULL (their own documented OOM-failure contract) rather than crashing
   * via stack overflow on an otherwise perfectly acyclic, merely deep
   * tree. depth (1000) is comfortably past CYAML_MAX_SERIALIZE_DEPTH
   * (500) while staying shallow enough that this test's own cleanup
   * (cyaml_destroy's tree walk, which is not depth-guarded) has no risk
   * of exhausting the stack itself. */
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
  /* serialize_block()/serialize_flow() must abort the WHOLE traversal the
   * instant b->oom is set (whether by CYAML_MAX_SERIALIZE_DEPTH tripping
   * on one branch, or a genuine allocator failure), not merely stop
   * appending to the buffer while still fully re-walking every remaining
   * sibling branch to independently rediscover the identical, already-known
   * failure. Without an "if (b->oom) return;" guard at the top of both
   * functions, a wide top-level list of many independently-deep branches
   * costs O(branches * CYAML_MAX_SERIALIZE_DEPTH) instead of O(1) once the
   * very first branch has already tripped the depth guard: each visited
   * dictionary along every one of those redundant walks still pays for a
   * real chmap_begin_iter_safe() allocation even though its own output is
   * always immediately discarded (b->oom already true). branch_count many
   * independent, deep single-key-dictionary chains (each safely past
   * CYAML_MAX_SERIALIZE_DEPTH) exercise exactly this: correct behavior
   * short-circuits after fully walking only the FIRST branch, so wall-clock
   * time stays roughly independent of branch_count; the pre-fix behavior
   * instead scales linearly with it. Measured directly (a scratch build
   * with the guard removed): correct behavior completes this call in
   * under 1ms, versus roughly 1.25s for branch_count re-walks of a
   * ~520-level chain; the 0.5s bound below sits comfortably between the
   * two, with wide margin on the fast side for a slower or instrumented
   * (e.g. valgrind) run. */
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
  REQUIRE_NE((void *)items, NULL);
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

TEST(delete, path_out_of_range_list_index_returns_key_not_found) {
  /* A syntactically valid but out-of-range "#N" leaf component is an
   * absent path component, exactly like a missing dictionary key, per
   * cyaml_delete's own documented contract ("ccol_key_not_found if any
   * path component is absent"); this must not be conflated with
   * ccol_invalid_args, which cyaml_delete reserves for a malformed path
   * (empty, or a non-"#N" leaf on a list parent). cyaml_list_remove()
   * itself is unaffected and still correctly returns ccol_invalid_args
   * for the same out-of-range index when called directly (see
   * list_remove_out_of_bounds above); the distinction only applies to
   * cyaml_delete's own path-based, higher-level contract. */
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - 1\n  - 2\n  - 3\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "items.#10"), ccol_key_not_found);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(root, "items")), (size_t)3);
  cyaml_destroy(root);
}

TEST(delete,
     path_malformed_index_syntax_in_non_leaf_component_returns_invalid_args) {
  /* A malformed "#N" index (not matching "#" + digits) is a caller-side
   * path-construction error, not a data-availability question; and that
   * must hold at ANY position in the path, not just the leaf. Before this
   * distinction was implemented, navigate_y() collapsed a malformed
   * mid-path index and a genuinely absent mid-path component into the same
   * NULL result, so this returned ccol_key_not_found instead. */
  char *err = NULL;
  cyaml root = cyaml_parse("items:\n  - a: 1\n  - a: 2\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "items.#xyz.a"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_list_len(cyaml_dictionary_get(root, "items")), (size_t)2);
  cyaml_destroy(root);
}

TEST(delete, path_wrong_type_in_non_leaf_component_returns_invalid_args) {
  /* A scalar encountered mid-path (with further components still remaining
   * beyond it, unlike a scalar sitting at the immediate parent of the
   * leaf) has no navigable children; that is a wrong-type
   * path-construction error (ccol_invalid_args), not an absent-component
   * error (ccol_key_not_found). "a" is itself the scalar 1, so "a.b.c"
   * must fail while still trying to resolve the "a.b" parent segment,
   * inside navigate_y() itself, not via the separate wrong-type check
   * _cyaml_delete performs on the immediate parent afterward. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\n", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cyaml_delete(root, "a.b.c"), ccol_invalid_args);
  REQUIRE_EQ(cyaml_int_val(cyaml_get(root, "a")), 1LL);
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
   * ccol_invalid_args, exactly like an empty LEAF component (e.g. "key.",
   * see delete.degenerate_path_trailing_dot above) already does: an
   * empty path component has no valid
   * interpretation as either a literal empty-string key or a "#N" index
   * anywhere in the path, so it is a caller-side path-construction error
   * (malformed syntax), not a well-formed-but-absent one, regardless of
   * whether it happens to be the leaf or an intermediate component. */
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

TEST(multi_document, bare_document_after_end_marker_is_a_new_document) {
  /* YAML 1.2 sec. 6.9 (l-yaml-stream): a document may omit its own '---'
   * only when directly preceded by a '...' end marker; that '...' is
   * enough on its own to start the next document, which may then be bare
   * (no '---'); this is not "trailing garbage" but a second, valid
   * document. */
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
  /* YAML 1.2 sec. 6.9's l-yaml-stream groups one-or-more consecutive
   * '...' markers (l-document-suffix+) as a single unit of separator
   * material, not one document boundary per marker; a second, immediately
   * redundant '...' must not be mistaken for "the next document is
   * empty" and must not fabricate a spurious extra CYAML_NULL document.
   * Cross-checked directly against PyYAML, which parses this as a single
   * (unwrapped) document. */
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
  /* Same redundant-'...' shape as above, but followed by a genuine second
   * document; must yield exactly two documents with no spurious NULL
   * document sitting between them. Cross-checked directly against
   * PyYAML, which parses this as exactly two documents. */
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
  /* Three redundant '...' in a row, still followed by a valid bare
   * (no '---') document per the "may omit '---' after '...'" rule;
   * confirms the redundant-marker loop doesn't over-consume into the
   * following document's own content. */
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
  /* Without an intervening '...', a second document MUST start with
   * '---'; bare content directly following an unterminated first
   * document is genuinely ambiguous trailing material, not a new
   * document. */
  char *err = NULL;
  cyaml root = cyaml_parse("a: 1\nsome garbage\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)root, NULL);
  free(err);
}

TEST(errors, duplicate_yaml_directive_rejected) {
  /* YAML 1.2 sec. 6.8.1: "it is an error to define more than one YAML
   * directive for the same document, even if both occurrences give the
   * same version" (verified against the vendored YAML Test Suite's own
   * SF5V case). Two directives for two SEPARATE documents (each with its
   * own '---') remains a distinct, unaffected scenario. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n%YAML 1.2\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, content_after_end_marker_on_same_line_rejected) {
  /* Nothing but whitespace and, optionally, a comment may share the
   * '...' marker's own line; real trailing content there ("... invalid")
   * has no valid interpretation (verified against the vendored YAML Test
   * Suite's own 3HFZ case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("---\nkey: value\n... invalid\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, hash_glued_directly_onto_start_marker_is_plain_scalar) {
  /* A '#' glued directly onto '---' with no separating whitespace is not a
   * comment: YAML 1.2's s-l-comments grammar requires s-separate-in-line
   * (real whitespace) before a comment's '#', matching this parser's own
   * skip_ws_comments()/rest_of_line_is_blank() rule elsewhere. "---#x" is
   * therefore ordinary plain-scalar content, not a document-start marker
   * followed by a comment (verified against both PyYAML and Psych, which
   * both parse this as the plain scalar "---#x"). */
  char *err = NULL;
  cyaml doc = cyaml_parse("---#x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "---#x");
  cyaml_destroy(doc);
}

TEST(errors, hash_glued_directly_onto_end_marker_is_plain_scalar) {
  /* Same rule as above, applied to '...': a glued '#' does not terminate
   * the plain scalar content, so "...#x" folds together with the following
   * line into one multi-line plain scalar (verified against PyYAML, which
   * parses this document as the string "...#x foo"). */
  char *err = NULL;
  cyaml doc = cyaml_parse("...#x\nfoo\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(doc), "...#x foo");
  cyaml_destroy(doc);
}

TEST(errors, hash_after_real_whitespace_on_start_marker_still_a_comment) {
  /* A '#' preceded by real whitespace after '---' is an ordinary comment;
   * the marker itself is unaffected. Guards against the fix above being
   * too broad. */
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

TEST(directives, custom_node_tag_preserved_and_has_no_typing_effect) {
  /* A custom local tag (!foo) on a scalar is preserved as queryable
   * metadata (see cyaml_node_tag()) but never forces a type of its own;
   * implicit typing rules still resolve the bare value normally. */
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
  /* A secondary tag (!!str) on a scalar forces CYAML_STRING regardless of
   * what the bare value would otherwise implicitly resolve to. */
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
  /* YAML 1.2 sec. 6.8.1: l-yaml-directive is "YAML" s-separate-in-line
   * ns-yaml-version; exactly one version token, nothing else.  A
   * trailing word after the version has no valid interpretation
   * (verified against PyYAML, which rejects this identically). */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2 foo\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, yaml_directive_comment_without_separating_space_rejected) {
  /* A '#' glued directly onto the version with no separating whitespace
   * is not a valid comment at all (this codebase's own general "a
   * comment must be preceded by whitespace" rule), so it is just
   * malformed trailing content on the directive line (verified against
   * PyYAML, which rejects this identically). */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.1#comment\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(directives, yaml_directive_extra_spaces_accepted) {
  /* Any number of spaces between "YAML" and the version token is
   * accepted, not just exactly one (verified against PyYAML). */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML  1.1\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, yaml_directive_tab_separator_rejected) {
  /* A tab is never valid block-structural indentation or separation
   * (YAML 1.2 sec. 6.1), and a %YAML directive's own separator before the
   * version token is no exception, whether glued directly on or
   * following a real space first (verified against PyYAML, which rejects
   * both; a prior version of this test incorrectly asserted both were
   * accepted, unverified against any reference parser at the time). */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML\t1.1\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);

  err = NULL;
  doc = cyaml_parse("%YAML \t1.1\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(directives, tag_directive_tab_separator_rejected) {
  /* A tab is never valid block-structural indentation or separation
   * (YAML 1.2 sec. 6.1); a %TAG directive's own two separators (between
   * "%TAG" and its handle, and between the handle and its prefix) are no
   * exception, whether glued directly on or following a real space first,
   * mirroring yaml_directive_tab_separator_rejected above exactly. */
  const char *cases[] = {
      "%TAG\t! foo:\n---\nkey: value\n",
      "%TAG \t! foo:\n---\nkey: value\n",
      "%TAG !\tfoo:\n---\nkey: value\n",
      "%TAG ! \tfoo:\n---\nkey: value\n",
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(cases[i], &err);
    REQUIRE_EQ((void *)doc, NULL);
    REQUIRE_NE((void *)err, NULL);
    free(err);
  }
}

TEST(directives, yaml_directive_tab_after_version_rejected) {
  /* A tab is never valid block-structural separation (YAML 1.2 sec.
   * 6.1); the separator between a %YAML directive's version token and a
   * trailing comment is no exception, mirroring the identical checks
   * already enforced before the version (yaml_directive_tab_separator_
   * rejected above). */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\t# comment\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(directives, yaml_directive_trailing_comment_with_separator_accepted) {
  /* A comment properly separated from the version by whitespace is a
   * valid, ordinary trailing comment (verified against PyYAML). */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.1  # comment\n---\nkey: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(directives, unrecognized_directive_name_with_extra_words_accepted) {
  /* Only "YAML" gets the strict, fixed-arity grammar above; any other
   * directive name (even one that merely resembles "YAML", like "YAM" or
   * "YAMLL") falls under YAML 1.2 sec. 6.8.2's ns-reserved-directive,
   * which permits any number of trailing parameters (verified against
   * PyYAML, which accepts both). */
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

TEST(errors, block_mapping_cannot_start_on_document_marker_line) {
  /* YAML 1.2's s-l+block-node grammar gives content directly on the same
   * physical line as an explicit '---' only the "flow-in-block"
   * alternative (a plain/quoted scalar or a flow collection); never
   * "block-in-block", which a block mapping needs and which requires
   * s-l-comments (only whitespace/a comment/a newline) directly after
   * '---'. A block mapping must therefore start on its OWN line
   * (verified against two independent reference parsers, both of which
   * reject this while accepting the identical content with no '---' at
   * all, or with '---' and a newline before it). */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- a: b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, block_sequence_cannot_start_on_document_marker_line) {
  /* Same restriction as block_mapping_cannot_start_on_document_marker_
   * line, for a block sequence: it has no "flow-in-block" alternative
   * either (verified against two independent reference parsers). */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- - a\n    - b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, anchored_mapping_cannot_start_on_document_marker_line) {
  /* Same restriction again, reached through an anchor decorating the
   * would-be key: "--- &anchor a: b" has no valid interpretation either,
   * even though the identical "&anchor a: b" with no marker at all (or
   * with the marker on its own line) is an ordinary, valid anchored key
   * (verified against two independent reference parsers). */
  char *err = NULL;
  cyaml doc = cyaml_parse("--- &anchor a: b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, mapping_on_own_line_after_document_marker_still_works) {
  /* Unlike same-line content, a block mapping starting on the line AFTER
   * '---' is entirely ordinary (verified against two independent
   * reference parsers). */
  char *err = NULL;
  cyaml doc = cyaml_parse("---\na: b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "b");
  cyaml_destroy(doc);
}

TEST(block_mapping, scalar_on_document_marker_line_still_works) {
  /* A plain scalar (not a mapping/sequence) directly on the same line as
   * '---', with or without a decorating anchor, keeps working: only
   * block-in-block content (a mapping or sequence) is restricted to a
   * later line, since a scalar has the "flow-in-block" alternative
   * available to it (verified against two independent reference
   * parsers). */
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

/* ----- Explicit "? key" / ": value" block mapping entries ----------------- */

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
  /* Per YAML 1.2 sec. 8.2.2, an explicit key with no following ':' at the
   * same indent has a null value. */
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
  /* An anchor on an explicit key ("&a a") can be referenced by a later
   * alias.  The third line, a bare ": *a" with nothing before the ':', is
   * its own separate entry whose implicit key is an empty plain scalar;
   * it does not attach to the "&b b" entry above it. An empty scalar
   * resolves to null under YAML 1.2's core schema exactly like "~" or
   * "null" would, so this key canonicalizes to the same "null" string
   * key those spellings do (see node_to_dict_key_string()'s own doc
   * comment: every implicit/explicit key-capture site canonicalizes
   * through the identical core-schema typing, not just non-empty ones). */
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
  /* An anchor may decorate an ordinary (implicit-style) key too, not just
   * an explicit '?' one, including for an entry after the first. */
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
  /* A sequence used as an explicit key is a valid YAML construct
   * (sec. 7.4.1/8.2.2); since this DOM's dictionaries are always
   * char* -> node, the key is canonicalized to its compact flow-YAML
   * text via cyaml_serialize_flow rather than rejected. */
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

/* ----- Flow collection single-pair / bare-key shorthands ------------------ */

TEST(flow_collections, sequence_bare_pair_shorthand) {
  /* "[foo: bar]" is shorthand for "[{foo: bar}]": a single "key: value"
   * pair with no surrounding '{'/'}' denotes a one-entry mapping element. */
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
  /* An empty key resolves to CYAML_NULL under the core schema (matching
   * plain scalar values; an empty string is null, same as "~"), which
   * node_to_dict_key_string canonicalizes to the string key "null",
   * consistent with every other null dictionary key in this DOM. */
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
  /* '?', ':', and '-' immediately followed by non-whitespace are ordinary
   * plain scalar content, not the explicit-key/value/list indicators, both
   * as a dictionary's first key and as a later one (exercising
   * parse_one_dict_entry_key's own "is this really an indicator" check). */
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
  /* "&a a: b" as a dictionary's very first entry anchors just the key
   * scalar "a", not the whole dictionary that entry turns out to
   * introduce; a sibling entry on the next line must still be its own,
   * separate, unanchored entry. */
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

/* Regression test for a real bug: parse_anchor_name()'s stop-character set
 * (whitespace, flow indicators, '#') did not include ':', so an alias used
 * directly as a key with no space before the colon (e.g. "*x: y", by far
 * the most natural way to write it, since an ordinary key is written
 * "key: value" the same way) had the colon silently absorbed into the
 * parsed alias name ("x:" instead of "x"), reporting a spurious "unknown
 * alias" error even though the anchor was validly defined. Every existing
 * alias-as-key test happened to use a space before the colon
 * (alias_used_directly_as_key above, alias_used_as_key_still_works below),
 * so this gap had no coverage. Fixed by stopping the name at a ':'
 * followed by whitespace/EOF, mirroring scan_plain_scalar_line's own
 * identical "colon+space or colon+EOL terminates" rule. */
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
  /* "a: &anchor" with nothing else on the line anchors an empty (null)
   * node; a sibling key at the same indentation as "a" on the next line
   * must remain a separate entry, not be absorbed as nested content of
   * the anchor. */
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
  /* Same hazard as the mapping case above, for a sequence: "- &anchor"
   * with nothing else on the line must not absorb the following sibling
   * element. */
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
  /* YAML 1.2 sec. 8.2.2: a block sequence value may sit at exactly the
   * same indentation as its parent mapping key, unlike every other value
   * kind (which must be strictly more indented). */
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
  /* Both the same-indentation-as-key form and the ordinary more-indented
   * form may appear side by side in one mapping. */
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
  /* A same-indent sibling that is NOT a '-' sequence indicator must still
   * be treated as a null value plus a fresh sibling entry, exactly as
   * before this exception existed. */
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
  /* A multi-key dictionary used as a key is canonicalized with its own
   * entries sorted lexicographically by key, regardless of the source's
   * own (here deliberately reverse-alphabetical) insertion order; unlike
   * cyaml_serialize_flow()'s own public, unsorted, iteration-order output
   * (see serialize.multi_key_flow_mapping_preserves_iteration_order
   * below), this canonicalization is a pure function of content. */
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
  /* The actual, end-to-end fix this canonicalization exists for: two
   * dictionary keys with identical content but different construction
   * (insertion) order must collide onto the exact same stored key, not be
   * treated as two distinct entries. Duplicate-key semantics are
   * last-wins (see block_mapping.duplicate_key_last_wins), so the second
   * entry's value must win. */
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
  /* The sort applies at every nesting level within the key being
   * canonicalized, not just the top level: a dictionary key whose own
   * VALUE is itself a dictionary with unsorted keys must have that nested
   * dictionary's keys sorted too. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{outer: {z: 1, a: 2}}: v\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(
      cyaml_str_val(cyaml_dictionary_get(doc, "{outer: {a: 2, z: 1}}")), "v");
  cyaml_destroy(doc);
}

TEST(block_mapping, flow_collection_compact_implicit_key) {
  /* A flow collection immediately followed by ':' is itself a valid
   * (non-scalar) implicit block mapping key; YAML 1.2 sec. 8.2.2's
   * compact mapping form. */
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
  /* Regression guard: a bare (unanchored) flow-collection implicit key must
   * be accepted at ANY entry position, not only the mapping's first one.
   * parse_one_dict_entry_key (used for every entry after the first) once
   * filtered out '['/'{' before ever trying try_parse_scalar_dict_key,
   * which is the function that actually knows how to parse a flow
   * collection as a key; this made the exact same construct succeed as the
   * first entry but fail as "trailing content" for any later entry. */
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
  /* Same regression as above, for a flow dictionary ('{') key rather than a
   * flow list ('[') one. */
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
  /* "? []: x"; the explicit key's own content is itself a compact
   * one-line block mapping ("[]: x"), per YAML 1.2 sec. 8.2.2's
   * s-l+block-indented "compact" alternative; i.e. the whole document
   * is a one-entry mapping whose key is itself the one-entry mapping
   * {[]: x} (canonicalized to its own flow-YAML text, quoting "[]" since
   * an unquoted "[]" would misparse as a flow-list open/close pair) and
   * whose value is null (nothing follows on the line). */
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
  /* Unlike a single-line flow collection, one that itself spans multiple
   * lines can never be an implicit key (ns-s-implicit-yaml-key requires a
   * single line, regardless of whether the key is a scalar or not); the
   * trailing ':' is therefore genuinely unparseable content. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[23\n]: 42\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(directives, verbatim_tag_with_comma_not_swallowed_as_flow_terminator) {
  /* A verbatim tag's own content ("!<...>") may legitimately contain a
   * literal ',' (its delimiters are the angle brackets, not whitespace),
   * unlike a shorthand tag's character set, which excludes it. */
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
  /* Mirrors the '&' branch's own sibling-swallowing regression test: a
   * tagged key must not let its recursive parse absorb an unrelated
   * sibling entry that happens to also be tagged.  "!!null" tags an empty
   * scalar (nothing between the tag and ':'); a tag on a key is parsed
   * for validity and then discarded (this DOM's keys are plain strings,
   * not nodes - see cyaml_node_tag()'s own doc comment), so the key's own
   * text is just the empty string, exactly as if no tag had been there at
   * all. That empty string still canonicalizes through the same
   * core-schema typing every other key does (node_to_dict_key_string()'s
   * own doc comment): an empty scalar resolves to null, so the key stored
   * is "null", the same string "~: a"/"? ~\n: a" would also produce.
   * "b"'s own VALUE, "!!str" with nothing following, forces an empty
   * string (not null), per this module's own tag-driven type resolution -
   * a value position's tag is not discarded the way a key position's is. */
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
  /* c-ns-properties permits the tag and anchor in either order; this key
   * has the tag first ("!!str &a1 ..."), unlike the anchor-then-tag order
   * covered elsewhere; and the anchor it carries must still be
   * registered and resolvable via a later alias. */
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
  /* "top: &node\n  *alias : value"; the anchor for "top"'s value has
   * nothing on its own line, so its content is resolved via parse_node's
   * generic recursion; that recursive entry point (the '*' alias branch,
   * not try_parse_scalar_dict_key) must also recognize an alias followed
   * by ':' as an implicit key. */
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
  /* A block sequence value can never start inline on the same line as its
   * own "key:"; unlike a mapping under a '-' sequence entry, which does
   * get that "compact" privilege, an implicit mapping value's own grammar
   * (ns-l-block-map-implicit-value) has no compact alternative at all;
   * the sequence must always begin on a later line. Verified against a
   * reference parser (PyYAML), not assumed. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: - a\n     - b\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, sequence_cannot_start_inline_after_anchor) {
  /* Same restriction as above, for an anchor's own inline content:
   * "&anchor - x" has no valid interpretation (a plain scalar can't
   * start with a bare '-' indicator either), so it is a hard error. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&anchor - sequence entry\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, sequence_cannot_start_inline_after_tag) {
  /* Same restriction as sequence_cannot_start_inline_after_anchor, for a
   * tag's own inline content: "!!seq - x" has no valid interpretation
   * either, matching the anchor case exactly (both are c-ns-properties,
   * and neither gets the "compact mapping" alternative's own separate
   * sequence-inline privilege). */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq - sequence entry\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, explicit_value_sequence_can_start_inline) {
  /* Unlike an implicit value, explicit-style content (both the '?' key's
   * own content and the ':' value that follows it) is grammatically
   * s-l+block-indented, whose "compact" alternative DOES permit a bare
   * '-' sequence to start inline on the same line (verified against
   * PyYAML). */
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
  /* Same "compact" privilege applies to the '?' key's own content, not
   * just the ':' value; this is the exact construct
   * explicit_block_mapping.non_scalar_key_canonicalized_to_flow_text
   * exercises end-to-end; this test isolates the inline-start aspect. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? - a\n  - b\n: v\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_type(doc), CYAML_DICTIONARY);
  cyaml_destroy(doc);
}

TEST(block_list, sequence_compact_mapping_under_dash_still_works) {
  /* Confirms the asymmetric fix above didn't disturb the other direction:
   * a mapping under a '-' sequence entry still gets its own, separate,
   * pre-existing "compact" privilege. */
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
  /* '>#comment' (no whitespace between the folded-scalar indicator and
   * '#') is not a comment; '#' right after the header is simply an
   * invalid trailing character. */
  char *err = NULL;
  cyaml doc = cyaml_parse("block: ># comment\n  scalar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_folded) {
  /* YAML 1.2 sec. 8.1.1: "It is an error for any of the leading empty
   * lines to contain more spaces than the first non-empty line." Here
   * the block's own indentation auto-detects to 1 (from " invalid"), but
   * an earlier blank line had 3 spaces (verified against the vendored
   * YAML Test Suite's own 5LLU case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("scalar: >\n \n  \n   \n invalid\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_literal) {
  /* Same restriction as block_scalar_leading_blank_line_more_indented_
   * folded, for a literal ('|') block scalar instead of folded ('>')
   * (verified against the vendored YAML Test Suite's own W9L4 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "block scalar: |\n     \n  more spaces at the beginning\n"
      "  are invalid\n",
      &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, block_scalar_leading_blank_line_more_indented_before_comment) {
  /* Same restriction again, where the first non-blank "line" that would
   * establish block_indent is itself a comment inside the scalar's own
   * leading blank-line run; the leading-blank-line check must still
   * fire before ever reaching that comment (verified against the
   * vendored YAML Test Suite's own S98Z case). */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("empty block scalar: >\n \n  \n   \n # comment\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, multiline_double_quoted_implicit_key_rejected) {
  /* An implicit key is always single-line (ns-s-implicit-yaml-key),
   * exactly like a plain scalar key; a double-quoted scalar spanning
   * multiple lines has no valid interpretation as a key (verified
   * against two independent reference parsers; matches the vendored
   * YAML Test Suite's own 7LBH case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\nb\": 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, multiline_single_quoted_implicit_key_rejected) {
  /* Same restriction as multiline_double_quoted_implicit_key_rejected,
   * for a single-quoted key (verified against two independent reference
   * parsers; matches the vendored YAML Test Suite's own D49Q case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("'c\n d': 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, multiline_quoted_key_nested_in_sequence_rejected) {
  /* Same restriction reached through a nested sequence element instead
   * of a top-level key (verified against two independent reference
   * parsers; matches the vendored YAML Test Suite's own JKF3 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("- - \"bar\nbar\": x\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, multiline_double_quoted_implicit_key_rejected_bare_cr) {
  /* Bare-CR counterpart to multiline_double_quoted_implicit_key_rejected:
   * this parser never CR/LF-normalizes its input, so a key that spans two
   * physical lines using only a bare '\r' (no '\n' at all) must be
   * rejected exactly like the LF-delimited version above; a "did this key
   * span more than one line" check based on '\n' alone would silently
   * accept this as if it were single-line. */
  char *err = NULL;
  cyaml doc = cyaml_parse("\"a\rb\": 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, multiline_single_quoted_implicit_key_rejected_bare_cr) {
  /* Bare-CR counterpart to multiline_single_quoted_implicit_key_rejected;
   * see multiline_double_quoted_implicit_key_rejected_bare_cr above for
   * the reasoning. */
  char *err = NULL;
  cyaml doc = cyaml_parse("'c\r d': 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, multiline_quoted_value_still_works) {
  /* Regression guard: the multiline-implicit-key restriction must not
   * affect a multi-line quoted scalar used as an ordinary VALUE (not a
   * key), which remains fully supported (YAML 1.2 sec. 7.3.3's
   * multi-line folding applies to quoted scalar VALUES; only the
   * implicit-KEY position is single-line-restricted). */
  char *err = NULL;
  cyaml doc = cyaml_parse("quoted: \"a\nb\nc\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "quoted")), "a b c");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchored_multiline_quoted_value_still_works) {
  /* Regression guard for try_parse_scalar_dict_key's own copy of the
   * same restriction: an anchor decorating a multi-line quoted scalar
   * VALUE (not a key) must still resolve as an ordinary anchored value,
   * not be misrouted through the key-detection fast path at all. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x \"multi\nline\"\nb: *x\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "multi line");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "b")), "multi line");
  cyaml_destroy(doc);
}

TEST(errors, document_marker_inside_unclosed_flow_collection_rejected) {
  /* YAML 1.2 sec. 6.9's c-forbidden: a '---'/'...' document marker at
   * the start of a line can never be plain scalar content; and a
   * still-open flow collection has no valid way to end at a document
   * boundary (only its own closing ']'/'}' can close it), unlike block
   * context, where the same marker simply, ordinarily ends the current
   * collection (verified against two independent reference parsers;
   * matches the vendored YAML Test Suite's own N782 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("[\n--- ,\n...\n]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, flow_dictionary_continuation_lines_at_column_zero_rejected) {
  /* Every line crossed inside a flow collection must be indented
   * strictly more than the enclosing block value's own indent
   * (YAML 1.2's s-separate(n,c)); here each continuation line ("k", ":",
   * "v", "}") sits at column 0, which is never more indented than
   * anything, so this is rejected purely on that indentation rule -
   * matching the vendored YAML Test Suite's own VJP3-0 case. This is NOT
   * a restriction on an implicit key's ':' having to share the key's own
   * line: VJP3-0's sibling case, VJP3-1 (identical shape, each
   * continuation line indented by exactly one space), is fail=0 in that
   * same suite and is accepted by this parser (see
   * implicit_key_colon_may_fold_to_a_later_line_ok below) - an ordinary
   * flow dictionary's implicit key genuinely may have its ':' on a later
   * line, unlike the "[key: value]" single-pair sequence shorthand's own
   * stricter ns-s-implicit-yaml-key restriction. */
  char *err = NULL;
  cyaml doc = cyaml_parse("k: {\nk\n:\nv\n}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(flow_collections, implicit_key_colon_may_fold_to_a_later_line_ok) {
  /* Sibling of the rejection above with proper indentation on every
   * continuation line (matching the vendored YAML Test Suite's own
   * VJP3-1 case, fail=0): a flow dictionary's implicit key's own ':' may
   * legitimately sit on a later line than the key itself. */
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
  /* Regression guard: the VALUE half is free to fold onto a later line
   * once the ':' itself has already been found (verified against a
   * reference parser). */
  char *err = NULL;
  cyaml doc = cyaml_parse("{k: \nv}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "k")), "v");
  cyaml_destroy(doc);
}

TEST(flow_collections, explicit_key_colon_on_later_line_still_works) {
  /* Regression guard: an EXPLICIT '?' key's own ':' may also appear on a
   * later line, exactly like block-style "? key\n: value" (verified
   * against a reference parser). */
  char *err = NULL;
  cyaml doc = cyaml_parse("{? key\n: value}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(errors, flow_collection_continuation_line_not_indented_enough_rejected) {
  /* YAML 1.2's s-separate(n,c) grammar (s-separate-lines(n) ->
   * s-flow-line-prefix(n)) requires every line crossed inside a flow
   * collection to be indented strictly more than the enclosing block
   * value's own indent; here "flow:"'s own column 0, with continuation
   * lines "b," and "c]" also at column 0 (verified against two
   * independent reference parsers; matches the vendored YAML Test
   * Suite's own 9C9N case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("flow: [a,\nb,\nc]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(flow_collections, continuation_line_indented_enough_still_works) {
  /* Regression guard: the identical content, indented one more space
   * throughout so continuation lines sit at column 1 (more than "flow:"'s
   * own column 0), is accepted (verified against two independent
   * reference parsers). */
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
  /* An anchor whose own value wraps onto a later line inside a flow
   * collection ("a: {b: &x\nc}\n": "&x" is the last thing on its line, its
   * value "c" follows on the next) must cross that newline via the same
   * s-separate(n,c) indentation rule every other flow-collection
   * continuation line in this file already enforces, not bypass it. Here
   * "a:"'s own indent is column 0, and the continuation line "c}" also
   * sits at column 0, not more indented (verified against two independent
   * reference parsers). */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: {b: &x\nc}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(flow_collections, anchor_continuation_line_indented_enough_still_works) {
  /* Regression guard: the identical content, indented one more space so
   * the continuation line sits at column 1 (more than "a:"'s own column
   * 0), is accepted, and the anchor's value round-trips correctly. */
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
  /* Tab-indentation counterpart to
   * anchor_flow_continuation_line_not_indented_enough_rejected: a tab used
   * as the continuation line's indentation has no valid interpretation
   * either, mirroring every other flow-collection continuation line's own
   * tab rejection in this file. */
  char *err = NULL;
  cyaml doc = cyaml_parse("{a: &x\n\tc}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tag_flow_continuation_line_not_indented_enough_rejected) {
  /* Tag counterpart to anchor_flow_continuation_line_not_indented_enough_
   * rejected: a tagged value wrapping onto an under-indented continuation
   * line inside a flow collection must be rejected the same way. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: {b: !!str\nc}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
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
  char *err = NULL;
  cyaml doc = cyaml_parse("{a: !!str\n\tc}\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_directly_after_dash_rejected) {
  /* YAML 1.2 sec. 6.1: tab characters are never valid as block-structural
   * indentation or separation. A bare '-' immediately followed by one
   * has no valid interpretation at all; not even as an ordinary plain
   * scalar starting with '-' (verified against two independent
   * reference parsers, both of which reject this unconditionally;
   * matches the vendored YAML Test Suite's own Y79Y-4 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("-\t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_after_dash_and_separator_space_rejected) {
  /* Same restriction as tab_directly_after_dash_rejected, one level
   * removed: a tab following the single separator space after '-' is
   * just as invalid (verified against two independent reference
   * parsers; matches the vendored YAML Test Suite's own Y79Y-5 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("- \t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_directly_after_question_mark_rejected) {
  /* Same restriction as tab_directly_after_dash_rejected, for the
   * explicit key indicator '?' (verified against two independent
   * reference parsers; matches the vendored YAML Test Suite's own
   * Y79Y-6 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("?\t-\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_after_question_mark_before_key_rejected) {
  /* Same restriction again, for '?' immediately followed by tab then a
   * plain scalar key rather than a sequence (verified against two
   * independent reference parsers; matches the vendored YAML Test
   * Suite's own Y79Y-8 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("?\tkey:\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_after_separator_space_before_first_question_mark_key) {
  /* Same restriction as tab_after_dash_and_separator_space_rejected, for
   * '?' rather than '-': a tab following the single separator space after
   * '?' is just as invalid as one directly after it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? \ta\n: 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_directly_after_question_mark_rejected_for_later_entry) {
  /* tab_directly_after_question_mark_rejected only ever exercises the
   * FIRST entry of a mapping, reached via parse_node's own '?' dispatch;
   * every later entry is read directly by parse_one_dict_entry_key from
   * parse_block_dictionary's own loop instead, which must enforce the
   * identical restriction itself rather than relying on parse_node's
   * dispatch (which it never revisits). */
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\n: 1\n?\tb\n: 2\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_after_separator_space_before_later_question_mark_key) {
  /* Same as tab_directly_after_question_mark_rejected_for_later_entry, but
   * for the tab-after-separator-space variant. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\n: 1\n? \tb\n: 2\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, second_explicit_entry_with_ordinary_space_still_works) {
  /* Regression guard: an ordinary single space (not a tab) after '?' on a
   * later entry remains completely unaffected. */
  char *err = NULL;
  cyaml doc = cyaml_parse("? a\n: 1\n? b\n: 2\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "a")), (long long)1);
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "b")), (long long)2);
  cyaml_destroy(doc);
}

TEST(block_list, dash_space_content_still_works) {
  /* Regression guard: an ordinary single space (not a tab) after '-'
   * remains completely unaffected. */
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
  /* Regression guard: a flow collection reached directly at the document
   * root has no enclosing block indent to satisfy (indent -1, the same
   * sentinel used throughout this parser), so column-0 continuation
   * lines are fine there (verified against two independent reference
   * parsers). */
  char *err = NULL;
  cyaml doc = cyaml_parse("{\"foo\"\n: \"bar\"}\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "bar");
  cyaml_destroy(doc);
}

TEST(flow_collections, multiline_flow_list_not_confused_by_unindented_dash) {
  /* Regression guard: an ordinary multi-line flow list, with an element
   * starting with '-' at column 0 on a continuation line (not "---"
   * itself), must not be affected by the document-marker check above. */
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
  /* Regression guard: a leading blank line with FEWER (or equal) spaces
   * than the eventually-detected block indentation is entirely ordinary
   * and must not be rejected. */
  char *err = NULL;
  cyaml doc = cyaml_parse("scalar: |\n \n  content\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "scalar")),
                "\ncontent\n");
  cyaml_destroy(doc);
}

TEST(errors, hash_immediately_after_comma_is_not_a_valid_element) {
  /* '#' can never start a plain scalar, in flow context or otherwise; a
   * '#' directly after ',' (no preceding whitespace) is neither a valid
   * comment (skip_ws_comments already requires whitespace before '#')
   * nor valid content. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[ a, b, c,#invalid\n]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, flow_list_comment_line_does_not_substitute_for_missing_comma) {
  /* A comment line between two flow-sequence elements is skipped like any
   * other whitespace/comment, but does not relax the requirement that a
   * ',' separate them; this also exercises that a comment line correctly
   * terminates (rather than gets swallowed into) an in-progress
   * multi-line plain scalar continuation. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: [ word1\n#  xxx\n  word2 ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, multiline_plain_scalar_stops_before_comment_line) {
  /* A comment line partway through what would otherwise be a multi-line
   * plain scalar's continuation must not be absorbed as content; the
   * scalar ends at the line before it. */
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
  /* The "[key: value]" single-pair shorthand's key is an implicit key
   * (ns-s-implicit-yaml-key) and so, like any other implicit key, must
   * fit on a single line; a ':' reached only by crossing a newline (here,
   * via the key's own multi-line plain scalar continuation folding
   * straight up to it) is not this shorthand at all. Verified against a
   * reference parser (PyYAML) using the exact vendored fixture bytes,
   * confirming this is a real difference from the multi-line KEY that
   * IS accepted inside a real "{ }" flow dictionary (see the
   * flow_collections.non_scalar_flow_dictionary_key_canonicalized-
   * adjacent multiline_flow_dictionary_key test below), not a
   * contradiction. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[ key\n  : value ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, flow_sequence_pair_quoted_key_followed_by_newline_then_colon) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[ \"key\"\n  :value ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, flow_sequence_pair_quoted_key_followed_by_bare_cr_then_colon) {
  /* Bare-CR counterpart to
   * flow_sequence_pair_quoted_key_followed_by_newline_then_colon: this
   * parser never CR/LF-normalizes its input, so a "[key ':value]" shorthand
   * whose key and ':' are split across only a bare '\r' (no '\n' at all)
   * must be rejected identically to the '\n'-delimited version above. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[ \"key\"\r  :value ]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(flow_collections, multiline_flow_dictionary_key) {
  /* Unlike the "[key: value]" sequence shorthand's key above, a real "{ }"
   * flow dictionary's own key is not restricted to a single line here
   * (verified against the vendored YAML Test Suite fixture, which this
   * library's own KNOWN_DEVIATIONS list does not carry an exception for,
   * i.e. this is confirmed accepted, not a known gap). */
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
  /* YAML 1.2 sec. 7.4.1, Spec Example 7.20: "[? key: value]" is an
   * explicit-style single-pair entry, mirroring the bare "[key: value]"
   * shorthand but introduced by '?'; permitting the key to fold across
   * multiple lines (an implicit key could not). */
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
  /* YAML 1.2 sec. 7.4.2, ns-flow-map-explicit-entry (Spec Example 7.16):
   * a flow dictionary entry's key may be introduced by an explicit '?',
   * exactly like a flow sequence's own "[? key: value]" shorthand; the
   * key must be the real content after the '?', not literally include
   * the "? " prefix text itself. A trailing bare "?" with nothing else
   * is a null key with a null value (verified against a reference
   * parser). */
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
  /* An ordinary implicit value's own same-line content has no license to
   * open a further nested mapping: "a: b: c: d" has no valid
   * interpretation under YAML 1.2 sec. 8.2.2 (ns-l-block-map-implicit-
   * value has no "compact mapping" alternative the way a sequence value
   * does not either); verified against a reference parser (PyYAML),
   * which reports this identically as "mapping values are not allowed
   * here". */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: b: c: d\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, chained_implicit_mapping_value_quoted_key_rejected) {
  /* Same restriction as chained_implicit_mapping_values_rejected, for a
   * single-quoted scalar acting as the chained inner key; the
   * restriction applies to every key form (plain scalar, quoted scalar,
   * flow collection), not just plain scalars. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: 'b': c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, chained_implicit_mapping_value_flow_collection_key_rejected) {
  /* Same restriction again, this time for a flow collection (itself a
   * valid non-scalar key per YAML 1.2 sec. 8.2.2's "compact mapping"
   * form) acting as the chained inner key: "a: [1,2]: c" is rejected
   * identically to "a: b: c" (verified against PyYAML). */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: [1,2]: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, chained_implicit_mapping_value_under_sequence_element_rejected) {
  /* The restriction reaches through a sequence element's own compact
   * mapping too: "- a: b: c" is rejected identically to the bare "a: b:
   * c" case, since "a"'s value "b: c" is still an ordinary implicit
   * value once inside the compact mapping the dash introduces (verified
   * against PyYAML). */
  char *err = NULL;
  cyaml doc = cyaml_parse("- a: b: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, chained_implicit_mapping_value_after_same_line_anchor_rejected) {
  /* The restriction also reaches through a same-line anchor decorating
   * the chained inner key: "a: &x b: c" is rejected identically to the
   * undecorated case (verified against PyYAML); allow_inline_map is
   * forwarded, not silently reset to true, when an anchor's content
   * stays on the same line as the anchor itself. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x b: c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, chained_implicit_mapping_value_explicit_key_form_rejected) {
  /* The restriction also reaches through the explicit '?' key form: "a: ?
   * b\n   : c" has no valid interpretation either, even though the
   * explicit form is otherwise perfectly legal once already inside
   * explicit-style content (verified against PyYAML, which reports "
   * mapping keys are not allowed here"). Every other "this could open a
   * fresh mapping here" dispatch (plain/quoted-scalar key, flow-
   * collection key) already enforces this; the explicit '?' form must
   * too. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: ? b\n   : c\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, anchor_on_own_line_still_permits_nested_mapping_value) {
  /* A value that starts on its own, more-indented line always keeps the
   * "may open a fresh mapping here" privilege, regardless of whether an
   * anchor decorates it; "top: &x\n  b: c\n" must still nest normally
   * (verified against PyYAML): only SAME-LINE chaining after an
   * already-open implicit value is ever restricted, not an anchor's own
   * subsequent-line content. */
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
  /* Same "own line always permits a fresh mapping" fix as above, for a
   * tag rather than an anchor: "top: !!map\n  b: c\n" must still nest
   * normally (verified against PyYAML). */
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
  /* Explicit-style value content keeps the same "compact" privilege for
   * a nested mapping that it already has for a nested sequence: "? k\n:
   * a: b" nests normally, unlike the implicit "a: b: c" case above
   * (verified against PyYAML, which accepts this and rejects the
   * implicit form identically). */
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
  /* A value that starts on its own, more-indented line must still be
   * recognized as such even when a comment (not a newline) is the very
   * next character after the key's ':'; "a: # comment\n  b: c\n" is an
   * ordinary nested mapping, not a same-line chain, so it must still be
   * permitted to open a fresh mapping (verified against PyYAML). Guards
   * against rest_of_line_is_blank's own purpose: naive at_eol()-based
   * same-line/later-line detection is fooled by the trailing comment. */
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
  /* Same trailing-comment hazard as above, this time for an anchor with
   * nothing else of its own: "a: &x # comment\nb: 2\n" must still see
   * "b: 2" as a SIBLING entry (same indent as "a"), not as the anchor's
   * own value; the sibling-swallow guard must not be bypassed just
   * because a comment, not a newline, immediately follows the anchor
   * name (verified against PyYAML). */
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
  /* Same trailing-comment hazard as value_on_later_line_not_fooled_by_
   * trailing_comment, for a tag rather than a bare value: "a: !!map #
   * comment\n  b: c\n" nests normally (verified against PyYAML). */
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
  /* YAML 1.2 sec. 6.6, ns-plain-first(c): '-' may start a plain scalar
   * only when immediately followed by an ns-plain-safe(c) character,
   * one that could itself legally appear WITHIN the scalar. A flow
   * indicator (here ']') fails that requirement, so a lone "-" has no
   * valid interpretation as a flow sequence element (verified against
   * the vendored YAML Test Suite's own YJV2 case; this specific
   * restriction is stricter than some other implementations enforce). */
  char *err = NULL;
  cyaml doc = cyaml_parse("[-]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, bare_dashes_separated_by_comma_in_flow_sequence_rejected) {
  /* Same restriction as bare_dash_in_flow_sequence_rejected, for two lone
   * dashes separated by a comma; each "-" is immediately followed by a
   * flow indicator (',' then ']'), so neither has a valid interpretation
   * (verified against the vendored YAML Test Suite's own G5U8 case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("[-, -]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(flow_collections, dash_followed_by_plain_content_still_valid) {
  /* Unlike a bare "-", a '-' immediately followed by ordinary plain-
   * scalar-safe content (no separating whitespace before a flow
   * indicator) is exactly as valid in flow context as it always was;
   * this guards against at_valid_flow_plain_scalar_start over-
   * restricting: "-1" and "-foo" are ordinary scalars, not sequence
   * indicators, in flow context. */
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
  /* Regression guard for at_valid_flow_plain_scalar_start's own
   * deliberate exclusion of ':' (see its doc comment): an empty implicit
   * key introduced by a bare ':' inside a flow sequence must keep
   * working exactly as before. */
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
  /* Same ns-plain-first(c) restriction as '-', for '?': a lone "?"
   * immediately followed by a flow indicator has no valid interpretation
   * as a plain scalar (this is distinct from "? key" style explicit-pair
   * syntax, which requires whitespace after the '?', not a flow
   * indicator). */
  char *err = NULL;
  cyaml doc = cyaml_parse("[?]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, scalar_with_two_stacked_anchors_rejected) {
  /* c-ns-properties permits at most one anchor per node; "&node2\n  &v2
   * val2" stacks a SECOND bare anchor directly around the same scalar
   * value, with nothing else (no key) in between, which has no valid
   * interpretation (verified against two independent reference parsers,
   * both of which reject a scalar wrapped in two anchors, split across a
   * newline here). */
  char *err = NULL;
  cyaml doc = cyaml_parse("top2: &node2\n  &v2 val2\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, scalar_with_two_stacked_anchors_same_line_rejected) {
  /* Same restriction as scalar_with_two_stacked_anchors_rejected, with
   * both anchors on one line instead of split across a newline. */
  char *err = NULL;
  cyaml doc = cyaml_parse("&a &b val\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, two_stacked_tags_rejected) {
  /* Same restriction as scalar_with_two_stacked_anchors_rejected, for two
   * tags instead of two anchors: c-ns-properties permits at most one tag
   * per node too (verified against a reference parser). */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str !!int 5\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, anchor_decorating_a_mapping_whose_key_is_also_anchored) {
  /* Regression guard distinguishing "two anchors stacked on the SAME
   * node" (invalid, see scalar_with_two_stacked_anchors_rejected) from
   * "an anchor decorates a mapping whose OWN first key happens to carry
   * a separate anchor" (an entirely different node; valid): "top1:
   * &node1\n  &k1 key1: val1\n" anchors the mapping {key1: val1} as
   * node1, and separately anchors the key scalar "key1" as k1 (verified
   * against two independent reference parsers, both of which accept
   * this while rejecting the shape-of-two-stacked-anchors case above). */
  char *err = NULL;
  cyaml doc = cyaml_parse("top1: &node1\n  &k1 key1: val1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml top1 = cyaml_dictionary_get(doc, "top1");
  REQUIRE_EQ(cyaml_type(top1), CYAML_DICTIONARY);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(top1, "key1")), "val1");
  cyaml_destroy(doc);
}

/* Regression tests for a real bug: an alias to an anchored dictionary
 * key's own anchor used to always resolve to a plain CYAML_STRING built
 * from the key's already-canonicalized text, discarding the key's real
 * type/structure - inconsistent with ordinary value-position anchoring,
 * which always preserves the anchored node's real type. An anchored
 * implicitly-typed plain-scalar key (e.g. "&n 42:") or non-scalar
 * flow-collection key (e.g. "&x [1, 2]:") must alias back to the real
 * integer/list, not a string of its canonical text; a quoted-scalar key
 * (whose own natural value is already a string) is unaffected. */
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
  /* Unaffected by the fix above: a quoted scalar's own natural value is
   * always a string, matching the key text exactly. */
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
  /* Same fix, exercised through parse_one_dict_entry_key (the key is not
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
     tag_and_anchor_decorated_key_alias_preserves_implicit_type) {
  /* Exercises the '!' dispatch's own fast path (a key carrying both a tag,
   * which this DOM always discards for a key, and an anchor, which must
   * still be registered against the key's real value). */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str &f 3.5: v\nlater: *f\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  cyaml later = cyaml_dictionary_get(doc, "later");
  REQUIRE_NE((void *)later, NULL);
  REQUIRE_EQ(cyaml_type(later), CYAML_FLOAT);
  REQUIRE_EQ(cyaml_double_val(later), 3.5);
  cyaml_destroy(doc);
}

TEST(errors, two_stacked_anchors_same_line_decorating_a_key_rejected) {
  /* The intersection scalar_with_two_stacked_anchors_same_line_rejected
   * and anchor_decorating_a_mapping_whose_key_is_also_anchored don't
   * individually cover: two anchors stacked on the SAME line, where what
   * follows looks like a valid compact-mapping key ("&a &b foo: bar").
   * The "is this an anchored key" fast path used to take priority over
   * the ordinary had_anchor rejection whenever the second anchor's own
   * content happened to look like a valid key, silently accepting this
   * (verified against two independent reference parsers, both of which
   * reject it identically to the non-key same-line case). */
  char *err = NULL;
  cyaml doc = cyaml_parse("&a &b foo: bar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, two_stacked_tags_same_line_decorating_a_key_rejected) {
  /* Same restriction as two_stacked_anchors_same_line_decorating_a_key_
   * rejected above, for two tags instead of two anchors. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str !!int foo: bar\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, anchor_decorating_a_bare_alias_rejected) {
  /* c-ns-alias-node is its own top-level alternative in ns-flow-node's
   * grammar, separate from the c-ns-properties branch an anchor/tag
   * decorates; an alias can never carry a property at all (verified
   * against two independent reference parsers). */
  char *err = NULL;
  cyaml doc = cyaml_parse("key1: &a value\nkey2: &b *a\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tag_decorating_a_bare_alias_rejected) {
  /* Same restriction as anchor_decorating_a_bare_alias_rejected, for a
   * named tag instead of an anchor (verified against a reference
   * parser). The alias target ("&n") is deliberately defined and
   * otherwise valid, so this fails specifically on the tag-on-alias
   * restriction, not merely on an unrelated "unknown alias" error a
   * weakened check would still happen to produce. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key1: &n value\nkey2: !!int *n\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(block_mapping, alias_used_as_key_still_works) {
  /* Regression guard: the bare-alias-cannot-carry-a-property restriction
   * must not affect an alias used AS A KEY ("*alias: value"), which is a
   * completely different, already-supported construct handled by
   * try_parse_scalar_dict_key's own fast path, not by decorating the
   * alias with a property at all. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\n*x : bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "a")), "foo");
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "bar");
  cyaml_destroy(doc);
}

TEST(errors, comma_directly_after_tag_rejected) {
  /* ns-tag-char excludes every c-flow-indicator character
   * unconditionally (not just in flow context); a comma glued directly
   * onto the end of a shorthand tag, with no separating whitespace, has
   * no valid interpretation as content (verified against a reference
   * parser). */
  char *err = NULL;
  cyaml doc = cyaml_parse("- !!str, xxx\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, flow_indicator_directly_after_anchor_rejected) {
  /* Same restriction as comma_directly_after_tag_rejected, for an anchor
   * name directly abutting a flow indicator with no separating
   * whitespace (verified against a reference parser). */
  char *err = NULL;
  cyaml doc = cyaml_parse("- &a[1,2]\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

/* Regression test for a real bug: the glued-flow-indicator restriction
 * above was gated by "!in_flow", so an anchor or tag glued directly onto a
 * NESTED flow collection opener ('['/'{') with no separating whitespace
 * was silently accepted inside an enclosing flow collection, even though
 * the identical construct was already correctly rejected in block
 * context. "[&a{x: 1}, *a]" parsed successfully, with the anchor
 * decorating the nested map and the alias resolving to a clone of it
 * (proving real semantic effect, not merely unconsumed trailing text);
 * verified against a reference parser, which rejects this. '['/'{' have no
 * valid interpretation directly after an anchor/tag name in ANY context
 * (nothing in the grammar lets a name be immediately followed by a
 * brand-new nested collection with no separator), unlike ','/']'/'}',
 * which remain legitimate flow-collection structure immediately after an
 * anchor/tag INSIDE a flow collection (see the two _still_works guards
 * below) and are only illegal in block context, where they have no valid
 * meaning at all. */
TEST(errors, anchor_directly_before_nested_flow_map_rejected_in_flow_context) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[&a{x: 1}, *a]", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, anchor_directly_before_nested_flow_list_rejected_in_flow_context) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[&a[1,2], *a]", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tag_directly_before_nested_flow_list_rejected_in_flow_context) {
  char *err = NULL;
  cyaml doc = cyaml_parse("[!!seq[1,2]]", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(flow_list, anchor_followed_by_comma_still_works_in_flow_context) {
  /* Regression guard: an anchor immediately followed by a flow
   * TERMINATOR (','/']'), as opposed to a nested collection OPENER, is
   * legitimate flow-collection structure and must remain accepted. */
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
  /* Regression guard: the glued-flow-indicator restriction must not
   * affect a tag whose content is genuinely separated by whitespace and
   * itself happens to be a plain scalar containing a comma (valid in
   * block context, where ',' is not a terminator). */
  char *err = NULL;
  cyaml doc = cyaml_parse("- !!str a, b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "a, b");
  cyaml_destroy(doc);
}

TEST(block_mapping, anchor_and_tag_combo_still_works_either_order) {
  /* Regression guard: the two-stacked-properties restriction must not
   * affect the ordinary, valid anchor+tag combo (one of each, either
   * order). */
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
 * Differential-testing regressions (found via tests/cyaml/differential/
 * compare_pyyaml.py against the vendored YAML Test Suite and fuzz corpus,
 * each independently cross-checked against a second reference parser,
 * Ruby's Psych, before being treated as a real bug).
 * ========================================================================== */

TEST(multi_document, two_consecutive_directive_documents_without_end_marker) {
  /* Two documents, each carrying its own %YAML directive, immediately
   * back to back with no "..." end marker between them: YAML 1.2 sec. 6.9's
   * l-yaml-stream grammar only permits a document to follow directly
   * (without "...") via its own l-explicit-document alternative, which
   * excludes a directive-carrying document (l-directive-document is a
   * separate top-level alternative); this construct is therefore a genuine
   * parse error, not two accepted empty documents. Previously, the first
   * document's own "is this document empty" check had no way to recognize
   * a fresh directive line as a document boundary, so it silently
   * mis-consumed the second document's own directive text as the first
   * document's scalar content instead of ever reaching this error. */
  char *err = NULL;
  cyaml doc = cyaml_parse("%YAML 1.2\n---\n%YAML 1.2\n---\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(multi_document, directive_document_after_end_marker_still_works) {
  /* Regression guard: a directive-carrying document IS permitted to
   * follow directly after an explicit "..." end marker (unlike the
   * no-marker case above); this must keep working. */
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
  /* A trailing "# comment" directly after "---" is not "same-line content"
   * (doc_marker_same_line_content only checked at_eol, which a comment
   * defeats since it is not itself a newline); this previously caused a
   * subsequent block mapping/sequence to be misdiagnosed as invalid
   * same-line content and rejected with "mapping values are not allowed
   * here" / "a block sequence cannot start on the same line...". */
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

TEST(multi_document, tab_after_start_marker_rejected) {
  /* A tab is never valid block-structural indentation or separation
   * (YAML 1.2 sec. 6.1), and '---'s own separator before same-line content
   * is no exception, whether glued directly on or following a real space
   * first (verified against PyYAML, which rejects both; Psych disagrees
   * and accepts them, matching this file's existing tab-vs-reference-parser
   * precedent elsewhere - see tests_spec_suite.c's KNOWN_DEVIATIONS entry
   * for case K54U). Covers both the plain leading-'---' path and the
   * '---' following a directive, since parse_one_document consumes the
   * marker at two separate call sites. */
  const char *cases[] = {
      "---\tfoo\n",
      "--- \tfoo\n",
      "%YAML 1.2\n---\tfoo\n",
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(cases[i], &err);
    REQUIRE_EQ((void *)doc, NULL);
    REQUIRE_NE((void *)err, NULL);
    free(err);
  }
}

TEST(multi_document, tab_after_end_marker_rejected) {
  /* Mirrors tab_after_start_marker_rejected above for the '...' document
   * end marker's own separator, whether glued directly on or following a
   * real space first. */
  const char *cases[] = {
      "a: 1\n...\tb\n",
      "a: 1\n... \tb\n",
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *err = NULL;
    cyaml doc = cyaml_parse(cases[i], &err);
    REQUIRE_EQ((void *)doc, NULL);
    REQUIRE_NE((void *)err, NULL);
    free(err);
  }
}

TEST(multi_document, space_after_start_and_end_marker_still_works) {
  /* Regression guard: ordinary space separation after '---'/'...' (as
   * opposed to a tab) must remain completely unaffected by the tab
   * rejection above. */
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
  /* A bare "---" (nothing else on its line, no "..." suffix) followed by a
   * directive-carrying document is the same class of construct as
   * two_consecutive_directive_documents_without_end_marker above: per
   * YAML 1.2 sec. 6.9's l-yaml-stream grammar, a document following one
   * with no "..." suffix may only be an l-explicit-document (a bare
   * "---", no directives); l-directive-document is only reachable as
   * l-any-document, gated behind a preceding "...". A directive can never
   * follow a document's own already-consumed leading "---" at all (a
   * directive only ever precedes the "---" it configures), so this is a
   * genuine parse error, not a two-document stream. Previously, the empty
   * first document's own "is this document empty" check never ran here:
   * the directive-parsing loop unconditionally ran right after consuming
   * the leading "---", silently absorbing the second document's entire
   * "%directive\n---\ncontent" as if it were the first document's own
   * trailing content instead of ever reaching this error. */
  char *err = NULL;
  cyaml doc = cyaml_parse("---\n%YAML 1.2\n---\nfoo\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

/* Regression test for a real bug: a zero-indented literal/folded block
 * scalar (content flush at column 0, the auto-detected indent this feature
 * has always supported at the document root) had no way to distinguish a
 * genuine "---"/"..." document marker at column 0 from an ordinary content
 * line also at column 0, since both have spaces == 0 == block_indent.
 * scan_block_scalar_line() silently absorbed the marker as scalar content
 * instead of ending the scalar (and the document), unlike every other
 * block-content parser in this file, which already guards this via
 * at_doc_marker(). A non-zero-indented block scalar was never affected: a
 * marker there always has fewer leading spaces than block_indent and was
 * already correctly treated as ending the scalar. */
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
  /* Same hazard, exercised with an explicit KEEP chomp indicator ("|+"),
   * confirming the fix applies regardless of chomp mode. */
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
  /* Control case: a block scalar indented beyond column 0 was never
   * affected by the bug above (a marker's own 0 leading spaces are always
   * fewer than a positive block_indent, already correctly ending the
   * scalar); kept as a guard that the fix didn't change this pre-existing,
   * already-correct case. */
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
  /* A line consisting solely of inline whitespace (here, a single tab)
   * between two content lines is still a genuinely blank line for
   * double-quoted scalar folding purposes and must fold to a newline, not
   * a space; YAML 1.2 sec. 6.5 spec example 6.5. Previously, the fold
   * logic only recognized a blank line via a bare cur(ctx) == '\n'/'\r'
   * check taken immediately after consuming the first newline, which
   * cannot see past the tab to the real newline behind it, so this
   * folded to a space instead. */
  const char *yaml = "\"Empty line\n \t\nas a line feed\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "Empty line\nas a line feed");
  cyaml_destroy(n);
}

TEST(quoted, single_quoted_blank_line_with_only_spaces_folds_to_newline) {
  /* Same fix, single-quoted side: a blank continuation line carrying its
   * own trailing spaces must still fold to a newline. */
  const char *yaml = "'foo\n \nbar'\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "foo\nbar");
  cyaml_destroy(n);
}

TEST(quoted, double_quoted_escaped_tab_before_fold_is_preserved) {
  /* An escaped tab ("\t") sitting immediately before a line break is real
   * content the author explicitly asked for, not incidental source
   * whitespace to be trimmed by the "strip trailing whitespace before a
   * fold" rule (YAML 1.2 sec. 8.1.2); that rule applies only to
   * literal, unescaped whitespace copied straight from the source.
   * Previously the trim loop stripped it unconditionally, silently
   * dropping the escaped tab. */
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
  /* Regression guard: ordinary literal trailing whitespace AFTER an
   * escape must still be stripped by the fold, exactly as before; only
   * the bytes the escape itself produced are protected. */
  const char *yaml = "\"a\\t  \nb\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "a\t b");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_keep_blank_line_excess_indentation_preserved) {
  /* A blank line more indented than the block's own indentation keeps its
   * own excess spaces as literal content, exactly like a non-blank line
   * (YAML 1.2 sec. 8.1.1.2's "more indented lines" rule makes no
   * exception for blank lines); including when that line is the
   * scalar's very last line under CHOMP_KEEP. Previously, a blank line's
   * own indentation was discarded unconditionally, regardless of how much
   * it exceeded the block indent. */
  const char *yaml = "|+\n ab\n \n  \n...\n";
  char *err = NULL;
  cyaml n = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cyaml_str_val(n), "ab\n\n \n");
  cyaml_destroy(n);
}

TEST(block_scalars, literal_clip_mid_content_blank_excess_indentation) {
  /* Same rule, mid-content (not trailing) blank line, CHOMP_CLIP: the
   * excess indentation must survive even though the scalar continues
   * with more real content afterward. */
  const char *yaml = "text: |\n  a\n    \n  b\n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "text")), "a\n  \nb\n");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_keep_blank_line_excess_indentation_preserved) {
  /* Identical fix, folded (">") style: a folded scalar's blank lines get
   * the same excess-indentation treatment as a literal scalar's. */
  const char *yaml = "foo: >+\n  x\n   \n";
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n \n");
  cyaml_destroy(doc);
}

TEST(block_scalars, literal_clip_no_source_trailing_newline_omits_final_lf) {
  /* YAML 1.2's own b-chomped-last grammar production (governing the
   * scalar's very last line under every chomp mode, CLIP and KEEP
   * included) is "b-as-line-feed | <end of file>": when the source has no
   * real line break at all after the scalar's last line (the input
   * simply ends there), no newline (synthetic or otherwise) is added
   * on that line's account, even under CLIP, which would otherwise always
   * append exactly one. Previously CLIP always added a trailing '\n'
   * whenever there was any content, regardless of whether the source
   * itself had a final line break. Uses cyaml_parse_n (not cyaml_parse,
   * which requires a NUL-terminated string) so the input's own missing
   * trailing newline is exactly what parse_n sees, not an artifact of a
   * C string literal always having an implicit NUL past its last byte. */
  const char *yaml = "foo: |\n  x\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n ");
  cyaml_destroy(doc);
}

TEST(block_scalars, folded_keep_no_source_trailing_newline_omits_final_lf) {
  /* Identical b-chomped-last fix, folded (">") style under CHOMP_KEEP. */
  const char *yaml = "foo: >+\n  x\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "x\n ");
  cyaml_destroy(doc);
}

TEST(block_scalars, literal_keep_leading_blank_only_no_source_newline) {
  /* The same no-synthetic-newline rule applies even when the scalar's
   * ENTIRE content is a single leading blank line with no real content
   * line ever found, and the source has no trailing newline at all: the
   * result must be a genuinely empty string, not a bare "\n". */
  const char *yaml = "- |+\n   ";
  char *err = NULL;
  cyaml doc = cyaml_parse_n(yaml, strlen(yaml), &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_list_get(doc, 0)), "");
  cyaml_destroy(doc);
}

TEST(block_list, dash_followed_only_by_comment_is_a_null_entry) {
  /* A block sequence entry consisting of just '-' followed only by a
   * trailing comment (no scalar value) must be a null entry, exactly
   * like a bare '-' with nothing at all after it; not have its comment
   * text handed to parse_node() as if it were real content, which
   * previously let parse_node() walk past the comment and swallow every
   * following sibling entry as nested content instead of separate
   * siblings. */
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
  /* Same class of bug, reached via a tag instead of a comment: "- !!str"
   * with nothing else on the line must not have parse_node()'s tag branch
   * recurse unconditionally past the newline and swallow the following
   * sibling entry as this tag's own value. Mirrors the '&' (anchor)
   * branch's own at_block_value_col() guard, which the tag branch was
   * missing. The entry itself resolves to an empty string (not null),
   * since !!str forces CYAML_STRING even on empty text. */
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
  /* Same tag-branch fix, dictionary-value position rather than a sequence
   * entry: "a: !!str" with nothing else on the line must leave "a" mapped
   * to an empty string (!!str forces CYAML_STRING even on empty text) and
   * "b" as a genuinely separate sibling key, not have "b: two" swallowed
   * as if it were "a"'s own tagged value. */
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
 * Tab-as-indentation full audit (found via the same differential-testing
 * tool as above, extended to probe every current_col()-based indentation
 * decision in the parser; see this project's own "Tab-as-indentation full
 * audit" internal history notes for the full investigation, including which
 * positions genuinely require rejection versus which are ordinary,
 * legitimate same-line separator whitespace or already-open content).
 * ========================================================================== */

static void require_tab_rejected(const char *yaml) {
  char *err = NULL;
  cyaml doc = cyaml_parse(yaml, &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, tab_as_pure_block_mapping_indentation_rejected) {
  /* 4EJS: tabs used as the SOLE indentation for nested block mapping
   * entries (no space characters at all). */
  require_tab_rejected("---\na:\n\tb:\n\t\tc: value\n");
}

TEST(errors, tab_as_pure_block_sequence_indentation_rejected) {
  /* Sequence analog of 4EJS, found during the same audit. */
  require_tab_rejected("-\n\t- a\n\t- b\n");
}

TEST(errors, tab_at_start_of_document_rejected) {
  /* A tab as the very first byte of the whole input, before any real
   * document content, consumed by parse_common's own top-level skip
   * before parse_one_document/parse_node ever get a chance to see it. */
  require_tab_rejected("\ta: 1\n");
}

TEST(errors, tab_before_anchor_after_fresh_line_rejected) {
  /* A tab used as fresh-line indentation immediately before a node
   * property (here, an anchor) that then decorates a sequence's own
   * value. */
  require_tab_rejected("-\n\t&x val\n");
}

TEST(errors, tab_after_anchor_same_line_still_works) {
  /* Regression guard: a tab used as ordinary SAME-LINE separator
   * whitespace between an anchor and its own inline value is legitimate
   * (s-separate-in-line permits it) and must not be rejected by the
   * fresh-line-only tab check above. */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: &x\tvalue\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(errors, tab_on_flow_collection_continuation_line_rejected) {
  /* Y79Y-3: a tab used as indentation on a flow list's own continuation
   * line, crossing a newline inside still-open "[...]" content. */
  require_tab_rejected("- [\n\tfoo,\n foo\n ]\n");
}

TEST(errors, tab_on_flow_dictionary_continuation_line_rejected) {
  /* Flow-dictionary counterpart to tab_on_flow_collection_continuation_
   * line_rejected above: parse_flow_dictionary is a hand-duplicated
   * sibling of parse_flow_list, not a shared implementation, so the same
   * continuation-line tab check needs its own, independently verified
   * regression coverage on the "{...}" side rather than being assumed
   * covered by the flow-list test alone. */
  require_tab_rejected("- {\n\tfoo: 1,\n bar: 2\n }\n");
}

TEST(errors, tab_on_flow_collection_continuation_line_rejected_bare_cr) {
  /* Bare-CR counterpart to tab_on_flow_collection_continuation_line_
   * rejected: this parser never CR/LF-normalizes its input, and a document
   * using nothing but bare '\r' line breaks (no '\n' at all) must trigger
   * the identical continuation-line tab rejection as the LF-delimited
   * version above; span_crosses_newline() (used by flow_skip_ws() to
   * decide whether a line was actually crossed) must recognize a bare '\r'
   * exactly like it recognizes '\n', not just the latter. */
  require_tab_rejected("- [\r\tfoo,\r foo\r ]\r");
}

TEST(errors, tab_on_flow_dictionary_continuation_line_rejected_bare_cr) {
  /* Bare-CR counterpart to tab_on_flow_dictionary_continuation_line_
   * rejected; see the flow-list bare-CR test above for the reasoning. */
  require_tab_rejected("- {\r\tfoo: 1,\r bar: 2\r }\r");
}

TEST(errors, tab_after_colon_value_indicator_rejected) {
  /* Y79Y-9 (explicit form) and the equally-invalid implicit form
   * ("key:\tvalue"), both verified against a reference parser: neither
   * has a valid interpretation, mirroring the pre-existing tab-after-'-'
   * and tab-after-'?' indicator checks. */
  require_tab_rejected("? key:\n:\tkey:\n");
  require_tab_rejected("key:\tvalue\n");
  require_tab_rejected("key: \tvalue\n");
}

TEST(errors, tab_after_colon_before_newline_value_still_rejected) {
  /* A tab directly after ':' is rejected even when the real value is on
   * a LATER line (not glued-on same-line content): the tab here is still
   * genuinely ambiguous separator whitespace, not merely harmless
   * trailing whitespace before a newline, per a reference parser. */
  require_tab_rejected("seq:\t\n - a\n");
}

TEST(block_mapping,
     explicit_key_value_split_across_tab_indented_lines_rejected) {
  /* An explicit "? key" / ": value" pair where BOTH the key and the value
   * are pushed onto their own tab-indented continuation lines. */
  require_tab_rejected("?\n\tkey\n:\n\tvalue\n");
}

TEST(block_mapping, explicit_key_value_indicator_tab_indented_rejected) {
  /* A narrower case than the one above: only the ':' value indicator's own
   * line is tab-indented (the key itself is validly space-indented).
   * current_col() counts a tab as a single byte of width, the same as a
   * space, so a lone leading tab can land at exactly map_indent by byte
   * offset alone; this must still be rejected like every other
   * indentation-measuring comparison in this file, not silently accepted
   * because the byte count happens to match. */
  require_tab_rejected("a:\n ? b\n\t: c\n");
}

TEST(directives, yaml_directive_tab_after_name_rejected) {
  /* A tab immediately after "%YAML" (before any real separator space)
   * has no valid interpretation, matching the space-then-tab case
   * covered by yaml_directive_tab_separator_rejected above. */
  require_tab_rejected("%YAML\t1.1\n---\nkey: value\n");
}

TEST(block_scalars, tab_only_leading_blank_line_rejected) {
  /* Y79Y-0: a tab as the very first character of a block scalar's own
   * (would-be) leading blank line, with block_indent still undetermined,
   * is genuinely ambiguous (more indentation, or content at indent 0?)
   * and a hard error. */
  require_tab_rejected("foo: |\n\t\nbar: 1\n");
}

TEST(block_scalars, tab_after_one_leading_space_on_blank_line_still_works) {
  /* Y79Y-1: the moment even one real space precedes the tab on that same
   * kind of leading blank line, the ambiguity above is gone (that one
   * space alone already establishes this line's own indent unambiguously)
   * and the line is accepted (verified against a reference parser;
   * this is the one-space counterpart to the zero-space rejection
   * immediately above, and must not regress if that fix is ever
   * broadened carelessly). */
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: |\n \t\nbar: 1\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  /* The one leading space establishes block_indent = 1; the tab that
   * follows it becomes this (single-line) scalar's own literal content,
   * and CLIP chomping (the default) appends the usual one trailing
   * newline. */
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "\t\n");
  REQUIRE_EQ(cyaml_int_val(cyaml_dictionary_get(doc, "bar")), 1LL);
  cyaml_destroy(doc);
}

TEST(block_scalars, tab_right_after_established_indent_is_content_literal) {
  /* Once block_indent is established (here, by the "1 space" on the
   * scalar's very first content line), a tab immediately following it is
   * ordinary literal content, not indentation; s-indent(n)'s own
   * production is spaces only, so whatever comes after it (tab included)
   * is nb-char* content (verified against a reference parser). */
  char *err = NULL;
  cyaml doc = cyaml_parse("foo: |-\n \tbar", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "foo")), "\tbar");
  cyaml_destroy(doc);
}

TEST(block_scalars,
     tab_right_after_established_indent_folded_is_content_literal) {
  /* Folded ('>') sibling of the literal-scalar case above, with one
   * further wrinkle specific to folding: YAML 1.2 sec. 8.1.3's
   * s-nb-spaced-text(n) ("more-indented"/"spaced" line) production is
   * s-indent(n) s-white nb-char*, and s-white is space OR tab - so a line
   * whose first byte past block_indent is a tab is itself a spaced line
   * (verified against a reference parser), even though the tab adds no
   * extra literal SPACE character of its own the way a real extra space
   * would. A spaced line's own leading break is preserved literally
   * rather than folded to a space, unlike the plain literal-scalar case
   * above (which never folds anything). */
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
  /* No %TAG redefines "!", so its default prefix is "!" itself; a
   * shorthand "!foo" resolves to the literal local tag "!foo". */
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
  /* c-ns-properties requires s-separate(n,c) before any following
   * content; "!<a>b" glues ordinary scalar content directly onto the
   * tag's own closing '>' with no separating whitespace at all, which
   * has no valid grammar path (verified against a reference parser). */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a>b\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, verbatim_tag_glued_directly_to_value_content_rejected) {
  /* Same rule at a mapping value position, not just the document root. */
  char *err = NULL;
  cyaml doc = cyaml_parse("k: !<a>b\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, verbatim_tag_glued_directly_to_block_scalar_rejected) {
  /* Same rule with a block scalar indicator glued directly onto the
   * tag, rather than plain scalar text. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a>|\n  x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, verbatim_tag_with_real_separator_still_works) {
  /* Regression guard: the glued-content rejection above must not
   * over-reach into rejecting a tag genuinely separated from its
   * content by whitespace, the ordinary case verbatim_tag_used_as_is
   * already covers with a named tag; this repeats it with content that
   * would trip the new check if the separator were mishandled. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<a> b\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "a");
  REQUIRE_STREQ(cyaml_str_val(doc), "b");
  cyaml_destroy(doc);
}

TEST(tags, bare_non_specific_tag_resolves_to_no_forced_type) {
  /* A bare "!" (nothing after it, distinct from a shorthand tag) never
   * forces a type; the value resolves exactly as if untagged. */
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
  /* "!!map" decorates the dictionary that "&a key: value" turns out to
   * introduce on the following line; the anchor belongs to just the key
   * scalar (see block_mapping's own anchored-key tests), not to the
   * mapping, so the outer tag must still reach the mapping itself rather
   * than being silently dropped by the anchor-branch's own "is this a
   * key" fast path. */
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
  /* Same shape as above but with a structurally mismatched "!!seq": must
   * still be caught as a hard tag/content mismatch, not silently ignored
   * by the fast path that produced the previous test's mapping. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq\n  &a key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, tag_on_mapping_introduced_by_aliased_key_is_preserved) {
  /* Same shape, via the '*' alias-as-key fast path instead of '&'. A space
   * before the ':' is required, exactly like a plain scalar key needs
   * ": " (not glued "key:value") to be recognized as a map separator. */
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
  /* The outer "!!map" (on its own line) must reach the mapping that the
   * inner, key-scoped "!!str" introduces on the next line; the inner tag
   * itself is correctly discarded (see
   * tag_on_dictionary_key_parses_but_is_not_preserved), but that must not
   * also swallow the outer, structurally distinct tag. */
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
  /* A custom tag never overrides scalar typing; falls through to implicit
   * resolution (42 -> integer). */
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
  free(err);
}

TEST(tags, alias_cannot_carry_a_tag) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\nb: !!str *x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, non_specific_tag_then_named_tag_rejected_in_flow_context) {
  /* A bare non-specific "!" resolves to no forced type (see
   * parse_tag_token()'s own doc comment), but it is still a real tag for
   * c-ns-properties' own "at most one tag" rule; a second, named tag
   * stacked underneath it must be rejected exactly like two named tags
   * stacked on each other already are ("!!str !!int 5" above). The
   * ordinary block-context same-line check ("cur(ctx) == '!'") does not
   * apply inside a flow collection, so this specifically exercises the
   * had_tag-based guard rather than that separate character check. */
  char *err = NULL;
  cyaml doc = cyaml_parse("[! !!str x]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, non_specific_tag_then_named_tag_rejected_on_own_line) {
  /* Same rule via the '&'-branch-style "value starts on its own line"
   * path, so the second tag is not glued onto the same line as the
   * first: had_tag must still have been carried into the recursive
   * parse_node call, not merely checked against resolved_tag (which
   * would be NULL for the enclosing non-specific "!" and would
   * therefore miss this). */
  char *err = NULL;
  cyaml doc = cyaml_parse("key: !\n  !!int 5\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, alias_cannot_carry_a_non_specific_tag) {
  /* Mirrors alias_cannot_carry_a_tag above, but with a bare non-specific
   * "!" instead of a named tag: an alias can never carry any tag at all,
   * including one whose own resolved_tag is NULL. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &x foo\nb: ! *x\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, tag_on_dictionary_key_parses_but_is_not_preserved) {
  /* A tag decorating a dictionary key is a real, permanent structural
   * limitation of this DOM (keys are plain char*, not cyaml_node_t*): it
   * must parse without error (grammar validity) but has nowhere to be
   * stored, so it is discarded. Checking cyaml_node_tag(doc) == NULL is
   * what actually verifies "not preserved" (a regression that leaked the
   * key's tag onto the enclosing mapping node instead of discarding it
   * would otherwise pass this test undetected). */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str key: value\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_EQ((void *)cyaml_node_tag(doc), NULL);
  REQUIRE_STREQ(cyaml_str_val(cyaml_dictionary_get(doc, "key")), "value");
  cyaml_destroy(doc);
}

TEST(tags, undefined_tag_handle_on_dictionary_key_rejected) {
  /* A tag decorating a key is still "parsed for validity" (see the
   * previous test's own doc comment and cyaml_node_tag()'s own doc
   * comment): an undefined %TAG handle must be rejected here exactly like
   * it already is when the identical tag decorates a value. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!x!foo key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tags, malformed_verbatim_tag_on_dictionary_key_rejected) {
  /* Same "parsed for validity" rule as the previous test, exercised via a
   * verbatim tag's own percent-escaped-null-byte rejection instead of an
   * undefined handle. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!<tag:x,2002:%00> key: value\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
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

TEST(tags, node_set_tag_does_not_coerce_value) {
  cyaml n = cyaml_create_int(42);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ((void *)cyaml_node_tag(n), NULL);
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_STR);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_EQ(cyaml_type(n), CYAML_INTEGER);
  REQUIRE_EQ(cyaml_int_val(n), 42LL);
  REQUIRE_STREQ(cyaml_node_tag(n), CYAML_TAG_STR);
  ccol_retval_t rv2 = cyaml_node_set_tag(n, NULL);
  REQUIRE_EQ((int)rv2, (int)ccol_success);
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
  /* A %TAG directive in document 1 must not leak into document 2: the
   * second document redefines the same handle "!e!" to a DIFFERENT
   * prefix, proving the first document's own registration didn't survive
   * (if it had, this would be rejected as a same-handle redefinition
   * instead of succeeding with a different resolved tag). */
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
  /* An undefined tag handle in ANY document of a multi-document stream
   * fails the whole cyaml_parse call, not just that one document; there
   * is no partial-success representation for a multi-document parse. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!foo a\n"
      "...\n---\n!e!foo b\n",
      &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_directives, undefined_named_handle_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!e!foo bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_directives, malformed_tag_directive_rejected) {
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG notahandle tag:example.com,2000:\n---\na: 1\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_directives, same_document_handle_redefinition_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n"
      "%TAG !e! tag:example.com,2000:\n---\na: 1\n",
      &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_directives, extra_content_after_prefix_rejected) {
  /* Nothing may follow a %TAG directive's own prefix except whitespace
   * and, once separated by that whitespace, a comment (YAML 1.2 sec.
   * 6.8.2's l-tag-directive ends in the same s-l-comments production
   * every other directive line does); a bare extra word must be
   * rejected, mirroring how "%YAML 1.2 garbage" is already rejected. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app/ garbage-text\n---\na: 1\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_directives, trailing_comment_with_separator_accepted) {
  /* A comment properly separated from the prefix by whitespace is a
   * valid, ordinary trailing comment, exactly like the identical %YAML
   * case (yaml_directive_trailing_comment_with_separator_accepted). */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:  # comment\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_glued_to_prefix_is_part_of_the_uri) {
  /* YAML 1.2's ns-uri-char (the tag prefix's own grammar) explicitly
   * permits '#' as an ordinary URI character; a '#' glued directly onto
   * the prefix with no separating whitespace cannot be a comment (a
   * comment requires s-b-comment, real preceding whitespace), so it must
   * be treated as part of the prefix text itself, not silently truncate
   * the prefix there and discard the rest as if it were a comment. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app#\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app#foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_glued_mid_prefix_is_part_of_the_uri) {
  /* Same rule with the '#' in the middle of the prefix rather than at
   * its very end, confirming the whole rest of the token (not just a
   * trailing '#') is captured as prefix text. */
  char *err = NULL;
  cyaml doc =
      cyaml_parse("%TAG !e! tag:example.com/#zzz\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com/#zzzfoo");
  cyaml_destroy(doc);
}

TEST(tag_directives, hash_after_real_whitespace_is_still_a_comment) {
  /* Once genuinely separated from the prefix by real whitespace, a '#'
   * is an ordinary trailing comment exactly as before; this guards
   * against the hash-glued fix above over-reaching into treating every
   * '#' as prefix content regardless of position. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:app #comment\n---\n!e!foo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:appfoo");
  cyaml_destroy(doc);
}

TEST(tag_directives, tab_after_prefix_rejected) {
  /* A tab is never valid block-structural separation (YAML 1.2 sec.
   * 6.1); the separator between a %TAG prefix and a trailing comment is
   * no exception, mirroring the identical %YAML-version check. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\t# comment\n---\na: 1\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
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
  free(err);
}

TEST(tag_directives, verbatim_tag_malformed_percent_escape_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!<tag:example.com,2000:app%zzfoo> bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_directives, shorthand_tag_percent_escaped_slash_resolves) {
  /* ns-tag-char (the shorthand suffix's own grammar, YAML 1.2 sec. 5.5)
   * derives from ns-uri-char exactly like the verbatim form's content
   * does, so a shorthand tag's suffix must decode a "%XX" escape too,
   * mirroring verbatim_tag_percent_escaped_slash above. */
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!app%2Ffoo bar\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)doc, NULL);
  REQUIRE_STREQ(cyaml_node_tag(doc), "tag:example.com,2000:app/foo");
  cyaml_destroy(doc);
}

TEST(tag_directives, shorthand_tag_secondary_handle_percent_escape_resolves) {
  /* The default secondary handle's own well-known "tag:yaml.org,2002:"
   * prefix plus a percent-escaped suffix must resolve to the identical
   * core-schema tag its unescaped spelling already does (!!str), not to an
   * unrecognized custom tag carrying the literal, un-decoded "%74". */
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
  free(err);
}

TEST(tag_directives, shorthand_tag_malformed_percent_escape_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse(
      "%TAG !e! tag:example.com,2000:\n---\n!e!app%zzfoo bar\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
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
  /* finalize_scalar_node's !!int branch shares try_parse_int_scalar with
   * implicit typing, so it must reject the same malformed "0x-0"-style
   * literal the plain-scalar regression test above covers, rather than
   * accepting it as 0. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int 0x-0\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
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
  free(err);
}

TEST(tag_typing, int_rejects_uppercase_octal_prefix) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int 0O17\n", &err);
  REQUIRE_EQ((void *)doc, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
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
  /* Hex/octal integer syntax has no float representation of its own in
   * the YAML core schema at all; a small, in-range hex literal (one
   * try_parse_int_scalar would have accepted as a genuine CYAML_INTEGER
   * on its own, were it not for the forcing tag) must be rejected under
   * !!float, not silently coerced to a float of the same magnitude. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0x10\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, float_rejects_inrange_octal) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0o17\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, float_rejects_uppercase_hex_prefix) {
  /* The critical regression case: strtod() itself still recognizes an
   * uppercase "0X" hex prefix per the C standard's own case-insensitive
   * "0x or 0X" wording, so without try_parse_float_scalar's explicit
   * uppercase-prefix rejection, this would fall through to the generic
   * strtod() call and be silently accepted as 16.0 via that extension. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0X10\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, float_rejects_uppercase_octal_prefix) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float 0O17\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, float_accepts_oversized_hex_overflow) {
  /* An oversized hex literal (2^64-1, too wide for a signed 64-bit
   * integer) has no CYAML_INTEGER representation at all; !!float forcing
   * it to the same magnitude as a float, mirroring implicit_types.
   * integer_hex_overflow_falls_back_to_float's identical fallback for
   * the untagged case, is the one legitimate reason this tag ever
   * accepts hex/octal syntax in the first place. */
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
  /* An explicit !!bool tag accepts a wider, case-insensitive vocabulary
   * than implicit bool typing does (which only recognizes true/True/TRUE/
   * false/False/FALSE); confirmed empirically against PyYAML's own
   * construct_yaml_bool, which matches case-insensitively against
   * {yes, no, true, false, on, off} regardless of loader (see
   * try_parse_bool_scalar_explicit's own doc comment in src/cyaml.c). */
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
  free(err);
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
  free(err);
}

TEST(tag_typing, bool_forces_on_literal_block_keep_untrimmed_rejected) {
  /* Unlike !!int/!!float (see int_forces_on_literal_block_keep_multiple_
   * newlines above), !!bool deliberately does NOT trim trailing whitespace
   * before matching against its vocabulary (matching PyYAML's own
   * stricter, untrimmed lookup); a KEEP-chomped block scalar's trailing
   * newlines must NOT be silently accepted as "true". */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool |+\n  true\n\n\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, int_mismatch_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int abc\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, float_mismatch_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float abc\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, int_rejects_leading_whitespace) {
  /* Regression guard: the core schema's own int grammar has no
   * leading-whitespace production, but strtoll() (used internally to parse
   * the trimmed text) silently skips it; this must still be rejected as a
   * tag/content mismatch, not silently accepted as if untrimmed. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int \" 42\"\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, float_rejects_leading_whitespace) {
  /* Regression guard: same class of bug as int_rejects_leading_whitespace
   * above, but for strtod()'s identical leading-whitespace tolerance. */
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float \" 3.5\"\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
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

/* ---- Structural-kind-mismatch: the 9 combinations. ---- */

TEST(tag_typing, str_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!str {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, int_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!int {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, float_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!float [1, 2]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, bool_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!bool [1, 2]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, null_on_collection_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!null {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, map_on_sequence_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!map [1, 2]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, seq_on_mapping_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq {a: b}\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, seq_on_scalar_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!seq foo\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(tag_typing, map_on_scalar_syntax_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("!!map foo\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
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

/* Regression test for a real bug: cyaml_node_set_tag() is documented to
 * allow attaching a core-schema tag that does not match the node's actual
 * type without coercing its value (see tags.node_set_tag_does_not_coerce_
 * value). serialize_block/serialize_flow's own tag-omission check used to
 * treat every one of the seven core-schema URIs as unconditionally
 * redundant, regardless of whether it actually matched the node it
 * decorated; a mismatched tag attached this way was therefore silently
 * dropped on every cyaml_serialize()/cyaml_serialize_flow() call, with no
 * error and nothing in the output to reconstruct it from. Fixed by only
 * omitting a core-schema tag when it actually matches the node's type
 * (tag_matches_node_type() in cyaml.c); a mismatched one is now emitted
 * verbatim, exactly like a custom tag. */
TEST(serialize_tags, mismatched_core_schema_scalar_tag_not_silently_dropped) {
  cyaml n = cyaml_create_int(42);
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_STR);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char *out = cyaml_serialize(n);
  REQUIRE_NE((void *)out, NULL);
  /* The mismatched tag must survive serialization, unlike before the fix. */
  REQUIRE_NE((void *)strstr(out, "!<tag:yaml.org,2002:str>"), NULL);

  char *err = NULL;
  cyaml reparsed = cyaml_parse(out, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)reparsed, NULL);
  REQUIRE_STREQ(cyaml_node_tag(reparsed), CYAML_TAG_STR);
  /* An explicit !!str tag forces string typing on reparse (the documented,
   * pre-existing tag-driven type resolution rule); this is the honest
   * consequence of the tag now actually reaching the wire, not a further
   * bug: a hand-written "!!str 42" has always parsed as a string. */
  REQUIRE_EQ(cyaml_type(reparsed), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(reparsed), "42");

  cyaml_serialize_free(out);
  cyaml_destroy(n);
  cyaml_destroy(reparsed);
}

TEST(serialize_tags, mismatched_core_schema_scalar_tag_via_flow_serializer) {
  cyaml n = cyaml_create_bool(true);
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_INT);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char *out = cyaml_serialize_flow(n);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_NE((void *)strstr(out, "!<tag:yaml.org,2002:int>"), NULL);

  cyaml_serialize_free(out);
  cyaml_destroy(n);
}

/* A mismatched CYAML_TAG_SEQ/_MAP is a structural, not merely a scalar-
 * typing, mismatch; it is still preserved on serialize (never silently
 * dropped) rather than special-cased, but reparsing the result correctly
 * fails, since a real YAML document can never legitimately carry "!!map"
 * on a sequence either. */
TEST(serialize_tags,
     mismatched_core_schema_collection_tag_rejected_on_reparse) {
  cyaml n = cyaml_create_list();
  ccol_retval_t rv = cyaml_node_set_tag(n, CYAML_TAG_MAP);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char *out = cyaml_serialize(n);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_NE((void *)strstr(out, "!<tag:yaml.org,2002:map>"), NULL);

  char *err = NULL;
  cyaml reparsed = cyaml_parse(out, &err);
  REQUIRE_EQ((void *)reparsed, NULL);
  REQUIRE_NE((void *)err, NULL);

  cyaml_serialize_free(out);
  free(err);
  cyaml_destroy(n);
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
  /* An anchor decorating the "<<" key itself does NOT disqualify merge
   * candidacy, mirroring the block-dictionary side (see
   * try_parse_scalar_dict_key's own doc comment); previously the flow-
   * dictionary key peek excluded '&' the same way it excludes '!'/quotes,
   * leaving "<<" as a literal, unmerged key whenever it happened to be
   * anchored in flow context. */
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
  /* A tag still disqualifies merge candidacy even when it follows an
   * anchor on the same "<<" key ("&y !!str <<"), not just when the tag is
   * the very first character (already covered by
   * tagged_double_angle_bracket_key_is_literal_not_merge). The flow-
   * dictionary implicit-key peek previously only inspected the first
   * character to decide "is this plain/untagged", which missed a tag
   * hidden behind a preceding anchor and incorrectly merged. */
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
  free(err);
}

TEST(merge_keys, non_mapping_scalar_alias_source_rejected) {
  /* non_mapping_source_rejected above covers a literal non-mapping merge
   * value directly ("<<: [1, 2]"); this covers the separate code path
   * reached when the sole merge source is an ALIAS resolving to a scalar
   * (merge_one_source_into's own "else" branch, not the sequence-element
   * loop non_mapping_element_in_source_sequence_rejected below exercises),
   * which must be rejected identically rather than, say, silently
   * iterating zero times over it. */
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &a 5\nb:\n  <<: *a\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
}

TEST(merge_keys, non_mapping_element_in_source_sequence_rejected) {
  char *err = NULL;
  cyaml doc = cyaml_parse("a: &a\n  x: 1\nb:\n  <<: [*a, 5]\n", &err);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_EQ((void *)doc, NULL);
  free(err);
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
  /* Memtest-covered: exercises cyaml_clone-per-target ownership discipline
   * for the exact same anchored value merged into two different mappings. */
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
  /* A tag on the ENCLOSING mapping (delegated down from a '!' layer to
   * the whole resulting dictionary) is a different thing entirely from a
   * tag directly on the "<<" key itself (see
   * tagged_double_angle_bracket_key_is_literal_not_merge above, where the
   * tag decorates the key). Conflating the two previously left "<<" as a
   * literal, unmerged key whenever the enclosing mapping merely happened
   * to be tagged. */
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
  /* The explicit '? <<' / ': value' form is just as genuine a merge
   * trigger as the implicit '<<: value' shorthand (confirmed against two
   * independent reference parsers): the key is the same plain, untagged
   * "<<" scalar either way, and this DOM's merge detection happens at the
   * key-capture site, not via a post-hoc lookup, so both forms must be
   * recognized there. */
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
  /* The mapping's very first entry, parsed through a different dispatch
   * path (parse_node's own '?' branch, with first_key == NULL) than every
   * later entry (parse_one_dict_entry_key); must expand identically. */
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
  /* An anchor decorating the explicit key does NOT disqualify it, mirroring
   * the implicit form's own "&y <<: *x is still a genuine trigger" rule. */
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
  /* YAML 1.2 sec. 8.2.2 permits an explicit key's own content to start on a
   * fresh, more-indented line rather than staying on the '?' line itself
   * (unlike an implicit key, which is always single-line); the merge-key
   * candidacy check must be computed from wherever the key's real content
   * actually starts, not from whatever immediately follows '?' on its own
   * line (which, here, is nothing but the newline this construct exists to
   * cross). Confirmed against a reference parser to expand identically to
   * the same-line "? <<" form. */
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
  /* Same "key pushed to a fresh line" case, but with an anchor decorating
   * the key too: explicit_key_peek_is_merge_candidate's own anchor/tag
   * skip-loop must reach the real "<<" text through the same newline
   * crossing. */
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
  /* "<<x" (a plain scalar that merely starts with "<<") must not be
   * mistaken for the exact two-character merge trigger. */
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
  /* "<< foo" (whitespace immediately after "<<", followed by further
   * plain-scalar content on the same line) is a false positive for the
   * merge-candidate peek's own deliberately coarse whitespace check (it
   * does not re-derive scan_plain_scalar_line's real termination rules,
   * under which plain internal whitespace never ends a scalar), but the
   * fully-parsed key is the whole two-word scalar "<< foo", never the
   * exact trigger "<<"; must be stored as an ordinary literal key with no
   * merge expansion. */
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
  /* "<<:*x" with no separating whitespace does not terminate at the ':'
   * (scan_plain_scalar_line's own flow-context colon rule requires the
   * following character to be whitespace or a flow terminator), so the
   * whole thing is one literal plain-scalar key, not a merge trigger with
   * an alias value; confirmed against a reference parser. */
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
  /* "[<<: *x]" (a flow SEQUENCE's own "[key: value]" compact-mapping
   * shorthand, YAML 1.2 sec. 7.4.1, ns-flow-pair) must expand a genuine
   * merge key exactly like the equivalent flow-dictionary ("{<<: *x}"),
   * block-dictionary, and block-sequence-compact-mapping ("- <<: *x")
   * forms already do; this one-element list's own element is itself a
   * one-entry mapping whose sole key is "<<". */
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
  /* Mirrors merge_keys.anchored_and_tagged_key_flow_dict_not_merged for
   * the flow-SEQUENCE "[key: value]" bare shorthand: a tag following an
   * anchor on the "<<" key must still disqualify the merge, which a peek
   * that only inspects the very first character cannot detect. */
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
  /* "[<<: *x, k2: 2]" is two SEPARATE list elements (each its own one-pair
   * compact mapping via the shorthand), not one two-key mapping; only the
   * first element's own key is "<<", so only it expands. */
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
  /* "[? <<: *x]": the explicit-key-introduced counterpart of the implicit
   * "[<<: *x]" shorthand just above (YAML 1.2 sec. 7.4.1, Spec Example
   * 7.20); must expand identically. */
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
  /* "[? << foo: 5]": the flow-sequence "[? key: value]" shorthand's own
   * merge-candidate peek does not re-check the fully-parsed key against
   * "<<" via strcmp before calling expand_merge_key (unlike every other
   * caller of explicit_key_peek_is_merge_candidate); this exercises that
   * path's own safety net instead, expand_merge_key's internal
   * cyaml_dictionary_get(map, "<<") lookup, which is a no-op for a
   * dictionary whose only key is the literal "<< foo". */
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

/* A duplicate "<<"-keyed entry within the same mapping collapses to
 * whichever one was written LAST (this module's own documented "last value
 * wins" duplicate-key policy); the four tests below confirm that a later,
 * explicitly quoted/tagged "<<" entry correctly wins as an ordinary
 * literal, and never inherits merge-trigger status from an earlier,
 * genuine "<<" entry it overwrites (nor the reverse: an earlier literal
 * "<<" must not suppress a later, genuine merge trigger). Regression
 * coverage for a real bug where merge-candidacy was OR-accumulated across
 * every entry seen while parsing the mapping, rather than tracked against
 * whichever entry actually ends up owning the "<<" slot. */
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

/* Regression test for a real bug: merge_one_source_into's own dedup check
 * ("does target already have this key") used to run BEFORE target's own
 * "<<" merge-trigger slot was removed, so a merge SOURCE that legitimately
 * contains its own literal, quoted "<<" key (not itself a merge trigger)
 * was spuriously seen as "already present in target" (matching target's
 * own not-yet-removed "<<" slot, the very entry holding the merge source),
 * silently dropping the source's real entry - which was then destroyed
 * outright once target's own "<<" slot was removed, with no error at all. */
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
  /* Same defect, via the "<<: [source1, source2]" sequence-of-mappings
   * form: the first source in the list carries a literal "<<" key of its
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
  /* YAML 1.2's nb-json (governing double-quoted content) is
   * "#x9 | [#x20-#x10FFFF]": a raw, unescaped C0 control byte other than
   * tab has no valid literal representation there; confirmed against
   * PyYAML, which rejects the identical byte with "special characters are
   * not allowed". */
  char src[] = "a: \"x\x01y\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, raw_control_byte_rejected_in_single_quoted_scalar) {
  char src[] = "a: 'x\x01y'\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, raw_control_byte_rejected_in_plain_scalar) {
  char src[] = "a: x\x01y\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, raw_control_byte_rejected_in_plain_scalar_continuation_line) {
  /* Same check, reached through parse_plain_scalar_multiline's own
   * continuation-line scan rather than the first line's. */
  char src[] = "a: x\n  y\x01z\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, raw_control_byte_rejected_in_literal_block_scalar) {
  char src[] = "a: |\n  x\x01y\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, raw_control_byte_rejected_in_folded_block_scalar) {
  char src[] = "a: >\n  x\x01y\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, escaped_control_byte_still_accepted_in_double_quoted_scalar) {
  /* The rejection above must be scoped to a RAW byte in the source text;
   * an explicit escape sequence producing the identical byte in the
   * decoded string remains the deliberate, documented way to embed a
   * non-null C0 control byte. An escape decoding to codepoint zero
   * specifically is the sole exception: see the null_byte_escape_*
   * tests below for why that one is rejected instead. */
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
  /* "\0" decodes to codepoint U+0000; unlike every other double-quoted
   * escape, this can never be embedded in a node's scalar value (a plain
   * NUL-terminated char* with no separate length field), so it must be a
   * hard parse error rather than silently truncating the string at the
   * embedded null byte. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\0b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, null_byte_escape_hex_rejected) {
  /* Same restriction as null_byte_escape_zero_rejected, for the
   * equivalent "\x00" two-digit-hex form. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\x00b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, null_byte_escape_u4_rejected) {
  /* Same restriction again, for the "\u0000" four-digit-hex form. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\u0000b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, null_byte_escape_u8_rejected) {
  /* Same restriction again, for the "\U00000000" eight-digit-hex form. */
  char *err = NULL;
  cyaml n = cyaml_parse("\"a\\U00000000b\"\n", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(quoted, surrogate_pair_combination_never_produces_null_byte) {
  /* A \u escape can only ever decode to codepoint zero directly (a
   * surrogate-pair combination's minimum possible result is U+10000, and
   * an unpaired/lone surrogate substitutes U+FFFD, never zero); this
   * confirms the surrogate-handling paths never accidentally produce a
   * null byte of their own by asserting the combination is correctly
   * ACCEPTED and decodes to U+10000, not a duplicate of the genuinely
   * rejecting null_byte_escape_u4_rejected (this test's own former
   * "errors"/"..._rejected" name was itself misleading: this input is
   * valid and must be accepted, not rejected). */
  char *err = NULL;
  cyaml n = cyaml_parse("\"\\ud800\\udc00\"\n", &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cyaml_type(n), CYAML_STRING);
  REQUIRE_STREQ(cyaml_str_val(n), "\xf0\x90\x80\x80");
  cyaml_destroy(n);
}

TEST(serialize_scalars, control_byte_in_string_is_quoted_and_escaped) {
  /* A string value containing a raw control byte (reachable via the
   * constructive API regardless of what the parser itself now rejects on
   * input) must never be emitted as unquoted plain-scalar content: that
   * would put the raw byte directly on the wire, producing output no
   * conformant YAML 1.2 parser (including this one, post-fix) can read
   * back. It must instead be double-quoted with the byte escaped. */
  cyaml n = cyaml_create_string("x\x01y");
  REQUIRE_NE((void *)n, NULL);
  char *s = cyaml_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(strstr(s, "\x01") != NULL, false);
  REQUIRE_NE((void *)strstr(s, "\\x01"), NULL);

  /* Round-trip: re-parsing the serialized form must recover the same
   * string. */
  char *err = NULL;
  cyaml back = cyaml_parse(s, &err);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_STREQ(cyaml_str_val(back), "x\x01y");

  cyaml_serialize_free(s);
  cyaml_destroy(n);
  cyaml_destroy(back);
}

TEST(errors, raw_del_byte_rejected_in_double_quoted_scalar) {
  /* DEL (0x7F) is excluded from unescaped scalar content in every style,
   * exactly like a C0 control byte, confirmed empirically against PyYAML
   * (see is_disallowed_control_byte's own doc comment). */
  char src[] = "a: \"x\x7Fy\"\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, raw_del_byte_rejected_in_plain_scalar) {
  char src[] = "a: x\x7Fy\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, raw_del_byte_rejected_in_literal_block_scalar) {
  char src[] = "a: |\n  x\x7Fy\n";
  char *err = NULL;
  cyaml n = cyaml_parse_n(src, sizeof(src) - 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

TEST(errors, escaped_del_byte_still_accepted_in_double_quoted_scalar) {
  /* Same carve-out as escaped_control_byte_still_accepted_in_double_
   * quoted_scalar above: the rejection is scoped to a RAW byte; an
   * explicit "\x7f" escape remains accepted. */
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
  /* Same round-trip guarantee as control_byte_in_string_is_quoted_and_
   * escaped above, for DEL (0x7F) specifically: needs_quoting() and
   * yb_append_yaml_dquoted() must agree on this byte, or a string
   * containing it would either be emitted as invalid raw plain-scalar
   * content, or quoted without actually being escaped. */
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

/* White-box accessor exposing the CALLING thread's own node-pool free-list
 * size (see cyaml.c's own THREAD-LOCAL NODE POOL section); not part of the
 * public API, declared here the same way tests/cvector/tests.c declares
 * cvector_get_capacity(). */
extern size_t cyaml_debug_pool_size(void);

/* Mirrors _CYAML_POOL_CAP in cyaml.c. Not part of any public header (it is
 * an internal tuning constant), so it is deliberately re-stated here rather
 * than shared, matching how other white-box tests in this codebase pin a
 * literal internal constant directly. */
#define CYAML_TEST_POOL_CAP 512

TEST(node_pool, cap_eviction_keeps_pool_bounded) {
  /* Whatever this thread's own pool already holds when this test runs (0..
   * CYAML_TEST_POOL_CAP, carried over from earlier tests in this same
   * binary), destroying strictly more than CYAML_TEST_POOL_CAP
   * default-allocator nodes in one go must leave the pool at EXACTLY the
   * cap afterward: every node_free() call below the cap is accepted into
   * the free-list, and every one past it is evicted (freed directly)
   * instead of letting the pool grow without bound. */
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
  /* Allocate all 50 first, THEN free all 50: interleaving a single alloc
   * with an immediate free would just recycle that same one node fifty
   * times over (net pool size 1, not 50), since node_alloc() always prefers
   * a pool-resident node when one is available. Holding all 50 live at
   * once forces every one of them to be a fresh calloc (nothing is yet
   * resident to recycle), so freeing them afterward grows the pool by
   * exactly one entry per node, mirroring cap_eviction_keeps_pool_bounded's
   * own allocate-then-free-in-bulk shape above. */
  cyaml nodes[50];
  for (int i = 0; i < 50; i++) nodes[i] = cyaml_create_null();
  for (int i = 0; i < 50; i++) cyaml_destroy(nodes[i]);
  r->end_pool_size = cyaml_debug_pool_size();
  return NULL;
}

TEST(node_pool, fresh_thread_starts_with_an_empty_pool) {
  /* The node pool is thread-local (see cyaml.c's own THREAD-LOCAL NODE POOL
   * section): a brand-new thread must never see whatever nodes the calling
   * (main) thread's own pool already holds by the time this test runs.
   * Every node_free() call the worker makes below only ever returns a node
   * to ITS OWN pool, never to this thread's; joining before reading either
   * result field satisfies this codebase's own "join unconditionally,
   * before any REQUIRE_*" test-hygiene rule. Leaves the worker thread's own
   * pool holding 50 nodes at thread-exit time, so this also exercises the
   * pthread-destructor pool-drain path (_pool_drain) under `make memtest`:
   * a broken drain would show up there as a 50-allocation leak. */
  pool_populate_result_t result = {(size_t)-1, (size_t)-1};
  pthread_t tid;
  pthread_create(&tid, NULL, pool_populate_thread, &result);
  pthread_join(tid, NULL);

  REQUIRE_EQ(result.start_pool_size, (size_t)0);
  /* 50 single alloc-then-free round trips starting from an empty pool grow
   * it by exactly one node per round trip (each is a fresh calloc, since
   * nothing was left to recycle from yet), well under the cap. */
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
  /* A node carries no thread affinity: node_free() always returns it to
   * whichever thread is CURRENTLY calling it, never the one that originally
   * allocated it (see node_free()'s own doc comment in cyaml.c). Allocate
   * every node on the main thread, but free all of them from a worker
   * thread instead, and confirm the freed nodes land in the WORKER's own
   * pool, leaving the main thread's own pool count completely unaffected. */
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
  pthread_create(&tid, NULL, cross_thread_free_thread, &arg);
  pthread_join(tid, NULL);
  free(nodes);

  size_t main_pool_after = cyaml_debug_pool_size();

  REQUIRE_EQ(arg.start_pool_size, (size_t)0);
  REQUIRE_EQ(arg.end_pool_size, count);
  REQUIRE_EQ(main_pool_after, main_pool_before);
}
