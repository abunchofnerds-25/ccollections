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

#include <cjson.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

/* ========================================================================== */
/*                         CONSTRUCTION                                       */
/* ========================================================================== */

TEST(construction, null) {
  cjson n = cjson_create_null();
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_NULL);
  cjson_destroy(n);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(construction, bool_true) {
  cjson n = cjson_create_bool(true);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_BOOL);
  REQUIRE_TRUE(cjson_bool_val(n));
  cjson_destroy(n);
}

TEST(construction, bool_false) {
  cjson n = cjson_create_bool(false);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_FALSE(cjson_bool_val(n));
  cjson_destroy(n);
}

TEST(construction, integer) {
  cjson n = cjson_create_int(-123456789LL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(n), -123456789LL);
  cjson_destroy(n);
}

TEST(construction, number) {
  cjson n = cjson_create_double(3.14);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(n), 3.14);
  cjson_destroy(n);
}

TEST(construction, string) {
  cjson n = cjson_create_string("hello");
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_STRING);
  REQUIRE_STREQ(cjson_str_val(n), "hello");
  cjson_destroy(n);
}

TEST(construction, empty_string) {
  cjson n = cjson_create_string("");
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cjson_str_val(n), "");
  cjson_destroy(n);
}

TEST(construction, array_empty) {
  cjson a = cjson_create_array();
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_type(a), CJSON_ARRAY);
  REQUIRE_EQ(cjson_array_len(a), (size_t)0);
  cjson_destroy(a);
}

TEST(construction, array_push) {
  cjson a = cjson_create_array();
  REQUIRE_EQ(cjson_array_push(a, cjson_create_int(1)), ccol_success);
  REQUIRE_EQ(cjson_array_push(a, cjson_create_int(2)), ccol_success);
  REQUIRE_EQ(cjson_array_push(a, cjson_create_int(3)), ccol_success);
  REQUIRE_EQ(cjson_array_len(a), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(a, 0)), 1LL);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(a, 2)), 3LL);
  REQUIRE_EQ((void *)cjson_array_get(a, 99), NULL);
  cjson_destroy(a);
}

TEST(construction, object_empty) {
  cjson o = cjson_create_object();
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_type(o), CJSON_OBJECT);
  REQUIRE_EQ(cjson_object_size(o), (size_t)0);
  cjson_destroy(o);
}

TEST(construction, object_set_get) {
  cjson o = cjson_create_object();
  REQUIRE_EQ(cjson_object_set(o, "x", cjson_create_int(10)), ccol_success);
  REQUIRE_EQ(cjson_object_set(o, "y", cjson_create_string("hi")), ccol_success);
  REQUIRE_EQ(cjson_object_size(o), (size_t)2);

  cjson x = cjson_object_get(o, "x");
  REQUIRE_NE((void *)x, NULL);
  REQUIRE_EQ(cjson_int_val(x), 10LL);

  cjson y = cjson_object_get(o, "y");
  REQUIRE_NE((void *)y, NULL);
  REQUIRE_STREQ(cjson_str_val(y), "hi");

  REQUIRE_EQ((void *)cjson_object_get(o, "missing"), NULL);
  cjson_destroy(o);
}

TEST(construction, object_replace_frees_old) {
  cjson o = cjson_create_object();
  cjson_object_set(o, "k", cjson_create_string("original"));
  cjson_object_set(o, "k", cjson_create_int(99));
  REQUIRE_EQ(cjson_int_val(cjson_object_get(o, "k")), 99LL);
  REQUIRE_EQ(cjson_object_size(o), (size_t)1);
  cjson_destroy(o);
}

/* ========================================================================== */
/*                         PARSING — SCALARS                                  */
/* ========================================================================== */

TEST(parse, null_literal) {
  cjson n = cjson_parse("null", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_NULL);
  cjson_destroy(n);
}

TEST(parse, bool_true) {
  cjson n = cjson_parse("true", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_TRUE(cjson_bool_val(n));
  cjson_destroy(n);
}

TEST(parse, bool_false) {
  cjson n = cjson_parse("false", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_FALSE(cjson_bool_val(n));
  cjson_destroy(n);
}

TEST(parse, positive_integer) {
  cjson n = cjson_parse("42", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(n), 42LL);
  cjson_destroy(n);
}

TEST(parse, negative_integer) {
  cjson n = cjson_parse("-7", NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(n), -7LL);
  cjson_destroy(n);
}

TEST(parse, zero) {
  cjson n = cjson_parse("0", NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(n), 0LL);
  cjson_destroy(n);
}

TEST(parse, leading_zero_rejected) {
  /* RFC 8259 §6: a leading zero may not be followed by more digits. */
  REQUIRE_EQ((void *)cjson_parse("01", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("001", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("-01", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("00.5", NULL), NULL);
}

TEST(parse, float_decimal) {
  cjson n = cjson_parse("3.14", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(n), 3.14);
  cjson_destroy(n);
}

TEST(parse, float_exponent) {
  cjson n = cjson_parse("1e3", NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(n), 1000.0);
  cjson_destroy(n);
}

TEST(parse, float_negative_exp) {
  cjson n = cjson_parse("-2.5e-1", NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(n), -0.25);
  cjson_destroy(n);
}

TEST(parse, simple_string) {
  cjson n = cjson_parse("\"hello world\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_STRING);
  REQUIRE_STREQ(cjson_str_val(n), "hello world");
  cjson_destroy(n);
}

TEST(parse, empty_string) {
  cjson n = cjson_parse("\"\"", NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_STRING);
  REQUIRE_STREQ(cjson_str_val(n), "");
  cjson_destroy(n);
}

TEST(parse, string_escapes) {
  cjson n = cjson_parse("\"tab:\\there\\nnewline\\\\back\\\"quote\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_STRING);
  REQUIRE_STREQ(cjson_str_val(n), "tab:\there\nnewline\\back\"quote");
  cjson_destroy(n);
}

TEST(parse, unicode_escape_ascii) {
  cjson n = cjson_parse("\"\\u0041\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cjson_str_val(n), "A");
  cjson_destroy(n);
}

TEST(parse, unicode_escape_2byte_utf8) {
  cjson n = cjson_parse("\"\\u00e9\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  const char *s = cjson_str_val(n);
  REQUIRE_EQ((unsigned char)s[0], 0xC3u);
  REQUIRE_EQ((unsigned char)s[1], 0xA9u);
  REQUIRE_EQ(s[2], '\0');
  cjson_destroy(n);
}

TEST(parse, surrogate_pair) {
  cjson n = cjson_parse("\"\\uD83D\\uDE00\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  const char *s = cjson_str_val(n);
  REQUIRE_EQ((unsigned char)s[0], 0xF0u);
  REQUIRE_EQ((unsigned char)s[1], 0x9Fu);
  REQUIRE_EQ((unsigned char)s[2], 0x98u);
  REQUIRE_EQ((unsigned char)s[3], 0x80u);
  REQUIRE_EQ(s[4], '\0');
  cjson_destroy(n);
}

TEST(parse, lone_low_surrogate) {
  /* A lone low surrogate has no valid meaning; substitute U+FFFD (0xEF 0xBF
   * 0xBD in UTF-8). */
  cjson n = cjson_parse("\"\\uDC00\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  const char *s = cjson_str_val(n);
  REQUIRE_EQ((unsigned char)s[0], 0xEFu);
  REQUIRE_EQ((unsigned char)s[1], 0xBFu);
  REQUIRE_EQ((unsigned char)s[2], 0xBDu);
  REQUIRE_EQ(s[3], '\0');
  cjson_destroy(n);
}

TEST(parse, null_byte_in_string_rejected) {
  /*   encodes a null byte which cannot be stored in a null-terminated C
   * string; the parser must reject it rather than silently truncate. */
  REQUIRE_EQ((void *)cjson_parse("\"\\u0000\"", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("\"hello\\u0000world\"", NULL), NULL);
}

TEST(parse, unknown_escape_rejected) {
  /* RFC 8259 §7 only allows \", \\, \/, \b, \f, \n, \r, \t, \uXXXX. */
  REQUIRE_EQ((void *)cjson_parse("\"\\z\"", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("\"\\q\"", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("\"hello\\z\"", NULL), NULL);
}

TEST(parse, incomplete_escape_at_string_end) {
  /* A lone backslash at end-of-input begins an escape that cannot be
   * completed; the string is unterminated. */
  REQUIRE_EQ((void *)cjson_parse("\"hello\\", NULL), NULL);
}

/* ========================================================================== */
/*                         PARSING — COMPOSITE                                */
/* ========================================================================== */

TEST(parse, empty_array) {
  cjson a = cjson_parse("[]", NULL);
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_type(a), CJSON_ARRAY);
  REQUIRE_EQ(cjson_array_len(a), (size_t)0);
  cjson_destroy(a);
}

TEST(parse, array_of_mixed) {
  cjson a = cjson_parse("[null, true, 1, 2.5, \"x\"]", NULL);
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_array_len(a), (size_t)5);
  REQUIRE_EQ(cjson_type(cjson_array_get(a, 0)), CJSON_NULL);
  REQUIRE_TRUE(cjson_bool_val(cjson_array_get(a, 1)));
  REQUIRE_EQ(cjson_int_val(cjson_array_get(a, 2)), 1LL);
  REQUIRE_EQ(cjson_double_val(cjson_array_get(a, 3)), 2.5);
  REQUIRE_STREQ(cjson_str_val(cjson_array_get(a, 4)), "x");
  cjson_destroy(a);
}

TEST(parse, empty_object) {
  cjson o = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_type(o), CJSON_OBJECT);
  REQUIRE_EQ(cjson_object_size(o), (size_t)0);
  cjson_destroy(o);
}

TEST(parse, simple_object) {
  cjson o = cjson_parse("{\"a\":1, \"b\":\"two\", \"c\":true}", NULL);
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_object_size(o), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_object_get(o, "a")), 1LL);
  REQUIRE_STREQ(cjson_str_val(cjson_object_get(o, "b")), "two");
  REQUIRE_TRUE(cjson_bool_val(cjson_object_get(o, "c")));
  cjson_destroy(o);
}

TEST(parse, nested) {
  cjson root = cjson_parse(
      "{\"users\":[{\"name\":\"Alice\",\"age\":30},{\"name\":\"Bob\",\"age\":"
      "25}]}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson users = cjson_object_get(root, "users");
  REQUIRE_EQ(cjson_array_len(users), (size_t)2);
  cjson alice = cjson_array_get(users, 0);
  REQUIRE_STREQ(cjson_str_val(cjson_object_get(alice, "name")), "Alice");
  REQUIRE_EQ(cjson_int_val(cjson_object_get(alice, "age")), 30LL);
  cjson bob = cjson_array_get(users, 1);
  REQUIRE_STREQ(cjson_str_val(cjson_object_get(bob, "name")), "Bob");
  cjson_destroy(root);
}

TEST(parse, whitespace_everywhere) {
  cjson o = cjson_parse("  {  \"k\"  :  [  1  ,  2  ]  }  ", NULL);
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_array_len(cjson_object_get(o, "k")), (size_t)2);
  cjson_destroy(o);
}

/* ========================================================================== */
/*                         PARSING — ERROR CASES                              */
/* ========================================================================== */

TEST(parse, error_empty) {
  char *err_str = NULL;
  cjson n = cjson_parse_mp("", &err_str, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err_str, NULL);
  REQUIRE_TRUE(strlen(err_str) > 0);
  free(err_str);
}

TEST(parse, error_trailing_garbage) {
  char *err_str = NULL;
  cjson n = cjson_parse_mp("42 garbage", &err_str, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err_str, NULL);
  REQUIRE_TRUE(strlen(err_str) > 0);
  free(err_str);
}

TEST(parse, error_unclosed_string) {
  cjson n = cjson_parse("\"unterminated", NULL);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(parse, error_unclosed_array) {
  cjson n = cjson_parse("[1,2,3", NULL);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(parse, error_unclosed_object) {
  cjson n = cjson_parse("{\"k\":1", NULL);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(parse, error_bad_value) {
  cjson n = cjson_parse("undefined", NULL);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(parse, error_control_char_in_string) {
  cjson n = cjson_parse("\"\x01\"", NULL);
  REQUIRE_EQ((void *)n, NULL);
}

TEST(parse, invalid_number_syntax) {
  /* RFC 8259 §6 requires at least one digit after '-', after '.', and
   * after 'e'/'E' (optionally preceded by '+'/'-'). */
  REQUIRE_EQ((void *)cjson_parse("-", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("1.", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("1e", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("1e+", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("1e-", NULL), NULL);
}

TEST(parse, bounded_input) {
  cjson n = cjson_parse_n("null garbage", 4, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_NULL);
  cjson_destroy(n);
}

TEST(parse, bounded_input_zero_len) {
  char *err = NULL;
  cjson n = cjson_parse_n_mp("anything", 0, &err, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  free(err);
}

/* ========================================================================== */
/*                         SERIALIZATION                                      */
/* ========================================================================== */

TEST(serialize, null_val) {
  cjson n = cjson_create_null();
  char *s = cjson_serialize(n);
  REQUIRE_STREQ(s, "null");
  cjson_serialize_free(s);
  cjson_destroy(n);
}

TEST(serialize, bool_vals) {
  cjson t = cjson_create_bool(true);
  cjson f = cjson_create_bool(false);
  char *st = cjson_serialize(t);
  char *sf = cjson_serialize(f);
  REQUIRE_STREQ(st, "true");
  REQUIRE_STREQ(sf, "false");
  cjson_serialize_free(st);
  cjson_serialize_free(sf);
  cjson_destroy(t);
  cjson_destroy(f);
}

TEST(serialize, integer_val) {
  cjson n = cjson_create_int(-42LL);
  char *s = cjson_serialize(n);
  REQUIRE_STREQ(s, "-42");
  cjson_serialize_free(s);
  cjson_destroy(n);
}

TEST(serialize, double_val_roundtrip) {
  double original = 1.23456789012345;
  cjson n = cjson_create_double(original);
  char *s = cjson_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  double parsed = strtod(s, NULL);
  REQUIRE_EQ(parsed, original);
  cjson_serialize_free(s);
  cjson_destroy(n);
}

TEST(serialize, string_escaping) {
  cjson n = cjson_create_string("tab:\there\nnewline\\back\"quote");
  char *s = cjson_serialize(n);
  REQUIRE_STREQ(s, "\"tab:\\there\\nnewline\\\\back\\\"quote\"");
  cjson_serialize_free(s);
  cjson_destroy(n);
}

TEST(serialize, empty_array) {
  cjson a = cjson_create_array();
  char *s = cjson_serialize(a);
  REQUIRE_STREQ(s, "[]");
  cjson_serialize_free(s);
  cjson_destroy(a);
}

TEST(serialize, simple_array) {
  cjson a = cjson_create_array();
  cjson_array_push(a, cjson_create_int(1));
  cjson_array_push(a, cjson_create_int(2));
  cjson_array_push(a, cjson_create_int(3));
  char *s = cjson_serialize(a);
  REQUIRE_STREQ(s, "[1,2,3]");
  cjson_serialize_free(s);
  cjson_destroy(a);
}

TEST(serialize, empty_object) {
  cjson o = cjson_create_object();
  char *s = cjson_serialize(o);
  REQUIRE_STREQ(s, "{}");
  cjson_serialize_free(s);
  cjson_destroy(o);
}

TEST(serialize, roundtrip_nested) {
  const char *input =
      "{\"name\":\"Alice\",\"scores\":[10,20,30],\"active\":true}";
  cjson root = cjson_parse(input, NULL);
  REQUIRE_NE((void *)root, NULL);

  char *out = cjson_serialize(root);
  REQUIRE_NE((void *)out, NULL);
  cjson root2 = cjson_parse(out, NULL);
  REQUIRE_NE((void *)root2, NULL);
  REQUIRE_STREQ(cjson_str_val(cjson_object_get(root2, "name")), "Alice");
  REQUIRE_TRUE(cjson_bool_val(cjson_object_get(root2, "active")));
  cjson arr = cjson_object_get(root2, "scores");
  REQUIRE_EQ(cjson_array_len(arr), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(arr, 1)), 20LL);

  cjson_serialize_free(out);
  cjson_destroy(root);
  cjson_destroy(root2);
}

TEST(serialize, float_type_roundtrip) {
  /* Whole-number CJSON_FLOAT nodes must serialize with a decimal point or
   * exponent so that parsing the output back yields CJSON_FLOAT, not
   * CJSON_INTEGER. */
  double cases[] = {1.0, 0.0, -5.0, 100.0, 1e14};
  for (size_t i = 0; i < 5; i++) {
    cjson n = cjson_create_double(cases[i]);
    char *s = cjson_serialize(n);
    REQUIRE_NE((void *)s, NULL);
    REQUIRE_TRUE(strchr(s, '.') != NULL || strchr(s, 'e') != NULL ||
                 strchr(s, 'E') != NULL);
    cjson back = cjson_parse(s, NULL);
    REQUIRE_NE((void *)back, NULL);
    REQUIRE_EQ(cjson_type(back), CJSON_FLOAT);
    REQUIRE_EQ(cjson_double_val(back), cases[i]);
    cjson_serialize_free(s);
    cjson_destroy(n);
    cjson_destroy(back);
  }
}

TEST(serialize, utf8_roundtrip) {
  /* Non-ASCII UTF-8 bytes must survive serialize → parse unchanged. */
  const char *original = "caf\xC3\xA9";  /* "café" in UTF-8 */
  cjson n = cjson_create_string(original);
  char *s = cjson_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  cjson back = cjson_parse(s, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cjson_type(back), CJSON_STRING);
  REQUIRE_STREQ(cjson_str_val(back), original);
  cjson_serialize_free(s);
  cjson_destroy(n);
  cjson_destroy(back);
}

TEST(serialize, pretty_null_handle) {
  char *s = cjson_serialize_pretty(NULL, 4);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "null");
  cjson_serialize_free(s);
}

TEST(serialize, pretty_print_indented) {
  cjson o = cjson_create_object();
  cjson_object_set(o, "a", cjson_create_int(1));
  char *pretty = cjson_serialize_pretty(o, 2);
  REQUIRE_NE((void *)pretty, NULL);
  REQUIRE_TRUE(strchr(pretty, '\n') != NULL);
  REQUIRE_TRUE(strchr(pretty, ' ') != NULL);
  cjson_serialize_free(pretty);
  cjson_destroy(o);
}

TEST(serialize, pretty_indent_zero_defaults_to_four) {
  cjson o = cjson_create_object();
  cjson_object_set(o, "a", cjson_create_int(1));
  char *pretty = cjson_serialize_pretty(o, 0);
  REQUIRE_NE((void *)pretty, NULL);
  REQUIRE_TRUE(strchr(pretty, '\n') != NULL);
  REQUIRE_TRUE(strstr(pretty, "    ") != NULL);
  cjson_serialize_free(pretty);
  cjson_destroy(o);
}

/* ========================================================================== */
/*                         cjson_get                                          */
/* ========================================================================== */

TEST(navigate, get_top_level_key) {
  cjson root = cjson_parse("{\"city\":\"Paris\"}", NULL);
  cjson city = cjson_get(root, "city");
  REQUIRE_NE((void *)city, NULL);
  REQUIRE_STREQ(cjson_str_val(city), "Paris");
  cjson_destroy(root);
}

TEST(navigate, get_nested_key) {
  cjson root = cjson_parse("{\"a\":{\"b\":{\"c\":99}}}", NULL);
  cjson c = cjson_get(root, "a.b.c");
  REQUIRE_NE((void *)c, NULL);
  REQUIRE_EQ(cjson_int_val(c), 99LL);
  cjson_destroy(root);
}

TEST(navigate, get_array_element) {
  cjson root = cjson_parse("{\"arr\":[10,20,30]}", NULL);
  cjson elem = cjson_get(root, "arr.#1");
  REQUIRE_NE((void *)elem, NULL);
  REQUIRE_EQ(cjson_int_val(elem), 20LL);
  cjson_destroy(root);
}

TEST(navigate, get_mixed_path) {
  cjson root = cjson_parse(
      "{\"users\":[{\"profile\":{\"name\":\"Alice\"}},{\"profile\":{\"name\":"
      "\"Bob\"}}]}", NULL);
  cjson name = cjson_get(root, "users.#0.profile.name");
  REQUIRE_NE((void *)name, NULL);
  REQUIRE_STREQ(cjson_str_val(name), "Alice");
  cjson_destroy(root);
}

TEST(navigate, get_hash_key_in_object) {
  cjson root = cjson_parse("{\"#0\":\"literal\"}", NULL);
  cjson v = cjson_get(root, "#0");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_STREQ(cjson_str_val(v), "literal");
  cjson_destroy(root);
}

TEST(navigate, get_not_found) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_EQ((void *)cjson_get(root, "b"), NULL);
  REQUIRE_EQ((void *)cjson_get(root, "a.nonexistent"), NULL);
  cjson_destroy(root);
}

TEST(navigate, get_empty_path_returns_root) {
  cjson root = cjson_parse("{\"x\":1}", NULL);
  REQUIRE_EQ((void *)cjson_get(root, ""), (void *)root);
  cjson_destroy(root);
}

TEST(navigate, get_array_out_of_bounds) {
  cjson root = cjson_parse("[1,2,3]", NULL);
  REQUIRE_EQ((void *)cjson_get(root, "#10"), NULL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                         cjson_set                                          */
/* ========================================================================== */

TEST(navigate, set_new_scalar_in_object) {
  cjson root = cjson_parse("{\"name\":\"Alice\"}", NULL);
  ccol_retval_t r = cjson_set(root, "score", 42);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "score")), 42LL);
  cjson_destroy(root);
}

TEST(navigate, set_update_existing_scalar) {
  cjson root = cjson_parse("{\"count\":0}", NULL);
  cjson_set(root, "count", 99);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "count")), 99LL);
  cjson_destroy(root);
}

TEST(navigate, set_change_type_int_to_string) {
  cjson root = cjson_parse("{\"val\":42}", NULL);
  cjson_set(root, "val", "now a string");
  REQUIRE_EQ(cjson_type(cjson_get(root, "val")), CJSON_STRING);
  REQUIRE_STREQ(cjson_str_val(cjson_get(root, "val")), "now a string");
  cjson_destroy(root);
}

TEST(navigate, set_change_type_object_to_scalar) {
  cjson root = cjson_parse("{\"nested\":{\"a\":1,\"b\":2}}", NULL);
  cjson_set(root, "nested", 7);
  REQUIRE_EQ(cjson_type(cjson_get(root, "nested")), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "nested")), 7LL);
  cjson_destroy(root);
}

TEST(navigate, set_change_type_array_to_scalar) {
  cjson root = cjson_parse("{\"list\":[1,2,3]}", NULL);
  cjson_set(root, "list", 0.5);
  REQUIRE_EQ(cjson_type(cjson_get(root, "list")), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(cjson_get(root, "list")), 0.5);
  cjson_destroy(root);
}

TEST(navigate, set_nested_path) {
  cjson root = cjson_parse("{\"user\":{\"active\":false}}", NULL);
  cjson_set(root, "user.active", (bool)true);
  REQUIRE_TRUE(cjson_bool_val(cjson_get(root, "user.active")));
  cjson_destroy(root);
}

TEST(navigate, set_array_element) {
  cjson root = cjson_parse("{\"scores\":[10,20,30]}", NULL);
  cjson_set(root, "scores.#1", 99);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "scores.#1")), 99LL);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "scores.#0")), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "scores.#2")), 30LL);
  cjson_destroy(root);
}

TEST(navigate, set_unsigned_int) {
  cjson root = cjson_parse("{\"v\":0}", NULL);
  unsigned int big = 3000000000U;
  cjson_set(root, "v", big);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "v")), (long long)3000000000ULL);
  cjson_destroy(root);
}

TEST(navigate, set_double) {
  cjson root = cjson_parse("{\"pi\":0}", NULL);
  cjson_set(root, "pi", 3.141592653589793);
  REQUIRE_EQ(cjson_double_val(cjson_get(root, "pi")), 3.141592653589793);
  cjson_destroy(root);
}

TEST(navigate, set_null) {
  cjson root = cjson_parse("{\"k\":\"hello\"}", NULL);
  cjson_set(root, "k", NULL);
  REQUIRE_EQ(cjson_type(cjson_get(root, "k")), CJSON_NULL);
  cjson_destroy(root);
}

TEST(navigate, set_missing_parent_fails) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  ccol_retval_t r = cjson_set(root, "nonexistent.child", 5);
  REQUIRE_NE(r, ccol_success);
  cjson_destroy(root);
}

TEST(navigate, set_empty_path_returns_invalid_args) {
  cjson root = cjson_parse("{\"k\":1}", NULL);
  ccol_retval_t r = cjson_set(root, "", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  /* Existing key must be unmodified. */
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "k")), 1LL);
  cjson_destroy(root);
}

TEST(navigate, set_array_index_out_of_bounds) {
  cjson root = cjson_parse("{\"arr\":[1,2]}", NULL);
  ccol_retval_t r = cjson_set(root, "arr.#99", 5);
  REQUIRE_NE(r, ccol_success);
  cjson_destroy(root);
}

TEST(navigate, consecutive_dots_get_rejected) {
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", NULL);
  /* "a..b" has an empty component and must return NULL, not silently map to
   * "a.b". */
  REQUIRE_EQ((void *)cjson_get(root, "a..b"), NULL);
  REQUIRE_EQ((void *)cjson_get(root, "a."), NULL);
  cjson_destroy(root);
}

TEST(navigate, consecutive_dots_set_rejected) {
  cjson root = cjson_parse("{\"a\":{\"b\":0}}", NULL);
  ccol_retval_t r = cjson_set(root, "a..b", 99);
  REQUIRE_NE(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a.b")), 0LL);
  cjson_destroy(root);
}

TEST(navigate, set_new_key_in_nested_object) {
  /* cjson_set must create a new leaf key when the parent path exists but the
   * leaf key is absent. */
  cjson root = cjson_parse("{\"user\":{}}", NULL);
  ccol_retval_t r = cjson_set(root, "user.name", "Alice");
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_STREQ(cjson_str_val(cjson_get(root, "user.name")), "Alice");
  cjson_destroy(root);
}

TEST(navigate, set_on_array_root) {
  /* cjson_set works when root itself is an array. */
  cjson root = cjson_parse("[10, 20, 30]", NULL);
  ccol_retval_t r = cjson_set(root, "#1", 99);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(root, 1)), 99LL);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(root, 0)), 10LL);
  cjson_destroy(root);
}

TEST(navigate, bare_hash_get_is_error) {
  cjson root = cjson_parse("{\"arr\":[10,20,30]}", NULL);
  cjson v = cjson_get(root, "arr.#");
  REQUIRE_EQ((void *)v, NULL);
  cjson_destroy(root);
}

TEST(navigate, bare_hash_set_is_error) {
  cjson root = cjson_parse("{\"arr\":[10,20,30]}", NULL);
  ccol_retval_t r = cjson_set(root, "arr.#", 99);
  REQUIRE_NE(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "arr.#0")), 10LL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

TEST(clone, scalar) {
  cjson orig = cjson_create_string("hello");
  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);
  /* String buffers must be independently allocated. */
  REQUIRE_NE((void *)cjson_str_val(copy), (void *)cjson_str_val(orig));
  REQUIRE_STREQ(cjson_str_val(copy), "hello");
  /* Destroying orig must not corrupt copy. */
  cjson_destroy(orig);
  REQUIRE_STREQ(cjson_str_val(copy), "hello");
  cjson_destroy(copy);
}

TEST(clone, null_handle) {
  REQUIRE_EQ((void *)cjson_clone(NULL), NULL);
}

TEST(clone, nested_object) {
  cjson orig = cjson_parse("{\"a\":{\"b\":[1,2,3]}}", NULL);
  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);

  REQUIRE_EQ(cjson_int_val(cjson_get(copy, "a.b.#0")), 1LL);
  REQUIRE_EQ(cjson_int_val(cjson_get(copy, "a.b.#2")), 3LL);

  cjson_set(copy, "a.b.#0", 999);
  REQUIRE_EQ(cjson_int_val(cjson_get(orig, "a.b.#0")), 1LL);

  cjson_destroy(orig);
  cjson_destroy(copy);
}

/* ========================================================================== */
/*                         TYPE SAFETY — _cjson_type_of                      */
/* ========================================================================== */

TEST(type_macro, bool_detection) {
  bool b = true;
  REQUIRE_EQ(_cjson_type_of(b), CJSON_BOOL);
}

TEST(type_macro, int_detection) {
  int i = 0;
  REQUIRE_EQ(_cjson_type_of(i), CJSON_INTEGER);
}

TEST(type_macro, long_long_detection) {
  long long ll = 0;
  REQUIRE_EQ(_cjson_type_of(ll), CJSON_INTEGER);
}

TEST(type_macro, double_detection) {
  double d = 0.0;
  REQUIRE_EQ(_cjson_type_of(d), CJSON_FLOAT);
}

TEST(type_macro, float_detection) {
  float f = 0.0f;
  REQUIRE_EQ(_cjson_type_of(f), CJSON_FLOAT);
}

TEST(type_macro, string_detection) {
  const char *s = "hi";
  REQUIRE_EQ(_cjson_type_of(s), CJSON_STRING);
}

TEST(type_macro, char_ptr_detection) {
  char *s = NULL;
  REQUIRE_EQ(_cjson_type_of(s), CJSON_STRING);
}

TEST(type_macro, null_is_null_type) {
  REQUIRE_EQ(_cjson_type_of(NULL), CJSON_NULL);
}

/* ========================================================================== */
/*                         SIGN EXTENSION                                     */
/* ========================================================================== */

TEST(sign_extension, signed_char_negative) {
  cjson root = cjson_parse("{\"v\":0}", NULL);
  signed char sc = -10;
  cjson_set(root, "v", sc);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "v")), -10LL);
  cjson_destroy(root);
}

TEST(sign_extension, unsigned_char_large) {
  cjson root = cjson_parse("{\"v\":0}", NULL);
  unsigned char uc = 200;
  cjson_set(root, "v", uc);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "v")), 200LL);
  cjson_destroy(root);
}

TEST(sign_extension, negative_short) {
  cjson root = cjson_parse("{\"v\":0}", NULL);
  short s = -500;
  cjson_set(root, "v", s);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "v")), -500LL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                         EDGE CASES                                         */
/* ========================================================================== */

TEST(edge, cjson_type_null_handle) { REQUIRE_EQ(cjson_type(NULL), CJSON_NULL); }

TEST(edge, deeply_nested_parse_and_get) {
  cjson root = cjson_parse("{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":42}}}}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a.b.c.d.e")), 42LL);
  cjson_destroy(root);
}

TEST(edge, array_containing_objects) {
  cjson root = cjson_parse("[{\"k\":1},{\"k\":2},{\"k\":3}]", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_type(root), CJSON_ARRAY);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "#2.k")), 3LL);
  cjson_destroy(root);
}

TEST(edge, set_creates_multiple_new_keys) {
  cjson root = cjson_parse("{}", NULL);
  cjson_set(root, "a", 1);
  cjson_set(root, "b", 2);
  cjson_set(root, "c", 3);
  REQUIRE_EQ(cjson_object_size(root), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "b")), 2LL);
  cjson_destroy(root);
}

TEST(edge, serialize_null_handle) {
  char *s = cjson_serialize(NULL);
  REQUIRE_STREQ(s, "null");
  cjson_serialize_free(s);
}

TEST(edge, float_serialized_with_decimal) {
  cjson n = cjson_create_double(1.0);
  char *s = cjson_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_TRUE(strchr(s, '.') != NULL || strchr(s, 'e') != NULL ||
               strchr(s, 'E') != NULL);
  REQUIRE_EQ(strtod(s, NULL), 1.0);
  cjson_serialize_free(s);
  cjson_destroy(n);
}

TEST(edge, large_integer_parse) {
  cjson n = cjson_parse("9223372036854775807", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_int_val(n), 9223372036854775807LL);
  cjson_destroy(n);
}

TEST(edge, llong_min_parse) {
  cjson n = cjson_parse("-9223372036854775808", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(n), LLONG_MIN);
  cjson_destroy(n);
}

TEST(edge, llong_min_serialize_roundtrip) {
  cjson n = cjson_create_int(LLONG_MIN);
  REQUIRE_NE((void *)n, NULL);
  char *s = cjson_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "-9223372036854775808");
  cjson back = cjson_parse(s, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cjson_type(back), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(back), LLONG_MIN);
  cjson_serialize_free(s);
  cjson_destroy(n);
  cjson_destroy(back);
}

/* ========================================================================== */
/*                              FUZZY TESTS                                   */
/* ========================================================================== */

TEST(fuzzy, duplicate_key_no_leak) {
  cjson root = cjson_parse("{\"x\":1,\"x\":2,\"x\":3}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_type(root), CJSON_OBJECT);
  REQUIRE_EQ(cjson_object_size(root), (size_t)1);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "x")), 3LL);
  cjson_destroy(root);
}

TEST(fuzzy, integer_overflow_becomes_double) {
  cjson pos = cjson_parse("9999999999999999999", NULL);
  REQUIRE_NE((void *)pos, NULL);
  REQUIRE_EQ(cjson_type(pos), CJSON_FLOAT);
  cjson_destroy(pos);

  cjson neg = cjson_parse("-9999999999999999999", NULL);
  REQUIRE_NE((void *)neg, NULL);
  REQUIRE_EQ(cjson_type(neg), CJSON_FLOAT);
  cjson_destroy(neg);
}

TEST(fuzzy, plain_char_negative_sign_extension) {
  cjson root = cjson_parse("{\"v\":0}", NULL);
  char c = -1;
  cjson_set(root, "v", c);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "v")), (long long)(signed char)-1);
  cjson_destroy(root);
}

TEST(fuzzy, long_decimal_number_parses) {
  cjson n = cjson_parse(
      "1000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000"
      "000000000000000000000000", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  cjson_destroy(n);
}

TEST(fuzzy, negative_long_decimal_number_parses) {
  cjson n = cjson_parse(
      "-1000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000"
      "000000000000000000000000", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  cjson_destroy(n);
}

TEST(fuzzy, overflow_float_is_parse_error) {
  char *err_str = NULL;
  cjson n = cjson_parse_mp("1e999", &err_str, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err_str, NULL);
  REQUIRE_TRUE(strlen(err_str) > 0);
  free(err_str);
}

TEST(fuzzy, overflow_negative_float_is_parse_error) {
  char *err_str = NULL;
  cjson n = cjson_parse_mp("-1e999", &err_str, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err_str, NULL);
  REQUIRE_TRUE(strlen(err_str) > 0);
  free(err_str);
}

TEST(fuzzy, max_representable_double_parses) {
  cjson n = cjson_parse("1.7976931348623157e+308", NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  cjson_destroy(n);
}

TEST(construction, null_string_becomes_json_null) {
  cjson n = cjson_create_string(NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_NULL);
  char *s = cjson_serialize(n);
  REQUIRE_STREQ(s, "null");
  cjson_serialize_free(s);
  cjson_destroy(n);
}

TEST(navigate, set_null_string_ptr_becomes_json_null) {
  cjson root = cjson_parse("{\"k\":\"hello\"}", NULL);
  const char *ptr = NULL;
  ccol_retval_t r = cjson_set(root, "k", ptr);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cjson_type(cjson_get(root, "k")), CJSON_NULL);
  char *s = cjson_serialize(root);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_TRUE(strstr(s, "null") != NULL);
  cjson_serialize_free(s);
  cjson_destroy(root);
}

TEST(navigate, set_nonfinite_returns_invalid_args) {
  cjson root = cjson_parse("{\"v\":1.0}", NULL);
  REQUIRE_EQ(cjson_set(root, "v", INFINITY), ccol_invalid_args);
  REQUIRE_EQ(cjson_set(root, "v", -INFINITY), ccol_invalid_args);
  REQUIRE_EQ(cjson_set(root, "v", NAN), ccol_invalid_args);
  cjson_destroy(root);
}

TEST(construction, double_infinity_rejected) {
  cjson n = cjson_create_double(INFINITY);
  REQUIRE_EQ((void *)n, NULL);

  cjson m = cjson_create_double(-INFINITY);
  REQUIRE_EQ((void *)m, NULL);

  cjson k = cjson_create_double(NAN);
  REQUIRE_EQ((void *)k, NULL);
}

TEST(construction, double_finite_still_works) {
  cjson n = cjson_create_double(1.5);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(n), 1.5);
  cjson_destroy(n);
}

TEST(navigate, set_nonfinite_double_rejected) {
  cjson root = cjson_parse("{\"v\":1.5}", NULL);
  ccol_retval_t r = cjson_set(root, "v", INFINITY);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_type(cjson_get(root, "v")), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(cjson_get(root, "v")), 1.5);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                         CLONE REGRESSION TESTS                             */
/* ========================================================================== */

TEST(clone, flat_array) {
  cjson orig = cjson_create_array();
  cjson_array_push(orig, cjson_create_int(10));
  cjson_array_push(orig, cjson_create_int(20));
  cjson_array_push(orig, cjson_create_string("hi"));

  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);
  REQUIRE_EQ(cjson_type(copy), CJSON_ARRAY);
  REQUIRE_EQ(cjson_array_len(copy), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(copy, 0)), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(copy, 1)), 20LL);
  REQUIRE_STREQ(cjson_str_val(cjson_array_get(copy, 2)), "hi");

  cjson_set(copy, "#0", 99);
  REQUIRE_EQ(cjson_int_val(cjson_array_get(orig, 0)), 10LL);

  cjson_destroy(orig);
  cjson_destroy(copy);
}

TEST(clone, flat_object) {
  cjson orig = cjson_create_object();
  cjson_object_set(orig, "a", cjson_create_int(1));
  cjson_object_set(orig, "b", cjson_create_string("world"));
  cjson_object_set(orig, "c", cjson_create_bool(true));

  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);
  REQUIRE_EQ(cjson_type(copy), CJSON_OBJECT);
  REQUIRE_EQ(cjson_object_size(copy), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_object_get(copy, "a")), 1LL);
  REQUIRE_STREQ(cjson_str_val(cjson_object_get(copy, "b")), "world");
  REQUIRE_TRUE(cjson_bool_val(cjson_object_get(copy, "c")));

  cjson_object_set(copy, "a", cjson_create_int(999));
  REQUIRE_EQ(cjson_int_val(cjson_object_get(orig, "a")), 1LL);

  cjson_destroy(orig);
  cjson_destroy(copy);
}

TEST(construction, object_set_replaces_old_child) {
  cjson o = cjson_create_object();
  cjson_object_set(o, "k", cjson_create_string("first"));
  REQUIRE_STREQ(cjson_str_val(cjson_object_get(o, "k")), "first");

  cjson_object_set(o, "k", cjson_create_string("second"));
  REQUIRE_STREQ(cjson_str_val(cjson_object_get(o, "k")), "second");
  REQUIRE_EQ(cjson_object_size(o), (size_t)1);

  cjson_object_set(o, "k", cjson_create_int(42));
  REQUIRE_EQ(cjson_int_val(cjson_object_get(o, "k")), 42LL);

  cjson_destroy(o);
}

/* ========================================================================== */
/*                 OWNERSHIP CONTRACT — EARLY INVALID_ARGS PATHS              */
/* ========================================================================== */

TEST(construction, array_push_null_arr_frees_child) {
  ccol_retval_t r = cjson_array_push(NULL, cjson_create_int(42));
  REQUIRE_EQ(r, ccol_invalid_args);
}

TEST(construction, array_push_null_child_rejected) {
  cjson arr = cjson_create_array();
  ccol_retval_t r = cjson_array_push(arr, NULL);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_array_len(arr), (size_t)0);
  cjson_destroy(arr);
}

TEST(construction, array_push_wrong_type_frees_child) {
  cjson not_array = cjson_create_int(7);
  ccol_retval_t r = cjson_array_push(not_array, cjson_create_string("hi"));
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson_destroy(not_array);
}

TEST(construction, object_set_null_obj_frees_child) {
  ccol_retval_t r = cjson_object_set(NULL, "k", cjson_create_int(1));
  REQUIRE_EQ(r, ccol_invalid_args);
}

TEST(construction, object_set_wrong_type_frees_child) {
  cjson not_obj = cjson_create_string("oops");
  ccol_retval_t r = cjson_object_set(not_obj, "k", cjson_create_int(99));
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson_destroy(not_obj);
}

TEST(construction, object_set_null_key_frees_child) {
  cjson o = cjson_create_object();
  ccol_retval_t r = cjson_object_set(o, NULL, cjson_create_int(1));
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_object_size(o), (size_t)0);
  cjson_destroy(o);
}

TEST(construction, array_get_null_arr_returns_null) {
  REQUIRE_EQ((void *)cjson_array_get(NULL, 0), NULL);
}

TEST(construction, object_get_null_returns_null) {
  REQUIRE_EQ((void *)cjson_object_get(NULL, "k"), NULL);
  cjson o = cjson_create_object();
  REQUIRE_EQ((void *)cjson_object_get(o, NULL), NULL);
  cjson_destroy(o);
}

/* ========================================================================== */
/*                         CUSTOM ALLOCATOR                                   */
/* ========================================================================== */

/*
 * Tracking allocator — wraps the standard allocator and counts alloc/free
 * calls so tests can verify the custom allocator is actually being used.
 */
static size_t _ta_allocs = 0;
static size_t _ta_frees = 0;

static void *_ta_malloc(size_t sz) {
  _ta_allocs++;
  return malloc(sz);
}
static void _ta_free(void *p) {
  if (p) _ta_frees++;
  free(p);
}
static void *_ta_calloc(size_t n, size_t sz) {
  _ta_allocs++;
  return calloc(n, sz);
}
static void *_ta_realloc(void *p, size_t sz) { return realloc(p, sz); }

static ccol_memmgmt_procs_t _tracking_alloc = {
    .malloc = _ta_malloc,
    .free = _ta_free,
    .calloc = _ta_calloc,
    .realloc = _ta_realloc,
};

/* ------------------------------------------------------------------ */

TEST(custom_alloc, scalar_nodes_use_custom_alloc) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson n1 = cjson_create_null_mp(&_tracking_alloc);
  cjson n2 = cjson_create_bool_mp(true, &_tracking_alloc);
  cjson n3 = cjson_create_int_mp(42LL, &_tracking_alloc);
  cjson n4 = cjson_create_double_mp(3.14, &_tracking_alloc);

  REQUIRE_NE((void *)n1, NULL);
  REQUIRE_NE((void *)n2, NULL);
  REQUIRE_NE((void *)n3, NULL);
  REQUIRE_NE((void *)n4, NULL);
  REQUIRE_GT(_ta_allocs, (size_t)0);

  size_t allocs_before_destroy = _ta_allocs;
  (void)allocs_before_destroy;

  cjson_destroy(n1);
  cjson_destroy(n2);
  cjson_destroy(n3);
  cjson_destroy(n4);

  /* Custom-allocator nodes bypass the pool and are freed directly. */
  REQUIRE_GT(_ta_frees, (size_t)0);
}

TEST(custom_alloc, string_node_uses_custom_strdup) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson n = cjson_create_string_mp("hello world", &_tracking_alloc);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cjson_str_val(n), "hello world");

  /* At least 2 allocations: node calloc + string malloc. */
  REQUIRE_GT(_ta_allocs, (size_t)1);

  size_t frees_before = _ta_frees;
  cjson_destroy(n);

  /* The string value is freed directly (custom alloc bypasses pool). */
  REQUIRE_GT(_ta_frees, frees_before);
}

TEST(custom_alloc, array_and_object_use_custom_alloc) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson arr = cjson_create_array_mp(&_tracking_alloc);
  REQUIRE_NE((void *)arr, NULL);
  REQUIRE_EQ(cjson_array_push(arr, cjson_create_int_mp(1, &_tracking_alloc)),
             ccol_success);
  REQUIRE_EQ(
      cjson_array_push(arr, cjson_create_string_mp("item", &_tracking_alloc)),
      ccol_success);
  REQUIRE_EQ(cjson_array_len(arr), (size_t)2);

  cjson obj = cjson_create_object_mp(&_tracking_alloc);
  REQUIRE_NE((void *)obj, NULL);
  REQUIRE_EQ(cjson_object_set(obj, "key",
                              cjson_create_bool_mp(false, &_tracking_alloc)),
             ccol_success);
  REQUIRE_EQ(cjson_object_size(obj), (size_t)1);

  /* Backing cvec and chmap each require at least one allocation. */
  REQUIRE_GT(_ta_allocs, (size_t)3);

  cjson_destroy(arr);
  cjson_destroy(obj);

  REQUIRE_GT(_ta_frees, (size_t)0);
}

TEST(custom_alloc, parse_uses_custom_alloc) {
  const char *json =
      "{\"name\":\"Alice\",\"age\":30,\"scores\":[10,20,30],"
      "\"active\":true,\"ratio\":1.5}";

  _ta_allocs = 0;
  _ta_frees = 0;

  cjson doc = cjson_parse_mp(json, NULL, &_tracking_alloc);
  REQUIRE_NE((void *)doc, NULL);

  cjson name = cjson_get(doc, "name");
  REQUIRE_NE((void *)name, NULL);
  REQUIRE_STREQ(cjson_str_val(name), "Alice");

  cjson age = cjson_get(doc, "age");
  REQUIRE_EQ(cjson_int_val(age), 30LL);

  cjson s1 = cjson_get(doc, "scores.#1");
  REQUIRE_EQ(cjson_int_val(s1), 20LL);

  REQUIRE_GT(_ta_allocs, (size_t)0);

  cjson_destroy(doc);
  REQUIRE_GT(_ta_frees, (size_t)0);
}

TEST(custom_alloc, serialize_uses_custom_alloc) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson doc = cjson_create_object_mp(&_tracking_alloc);
  cjson_object_set(doc, "x", cjson_create_int_mp(7, &_tracking_alloc));
  cjson_object_set(doc, "s", cjson_create_string_mp("hi", &_tracking_alloc));

  size_t allocs_before_ser = _ta_allocs;

  char *out = cjson_serialize(doc);
  REQUIRE_NE((void *)out, NULL);
  REQUIRE_GT(_ta_allocs, allocs_before_ser);

  size_t frees_before_free = _ta_frees;
  cjson_serialize_free_mp(out, &_tracking_alloc);
  REQUIRE_GT(_ta_frees, frees_before_free);

  cjson_destroy(doc);
}

TEST(custom_alloc, path_navigation_uses_custom_alloc) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson doc = cjson_parse_mp("{\"a\":{\"b\":0}}", NULL, &_tracking_alloc);
  REQUIRE_NE((void *)doc, NULL);

  size_t allocs_before = _ta_allocs;
  size_t frees_before = _ta_frees;

  /* cjson_get dups the path internally then frees it. */
  cjson leaf = cjson_get(doc, "a.b");
  REQUIRE_NE((void *)leaf, NULL);
  REQUIRE_EQ(cjson_int_val(leaf), 0LL);

  REQUIRE_GT(_ta_allocs, allocs_before);
  REQUIRE_GT(_ta_frees, frees_before);

  size_t allocs_before_set = _ta_allocs;
  size_t frees_before_set = _ta_frees;
  REQUIRE_EQ(cjson_set(doc, "a.b", 99), ccol_success);
  REQUIRE_GT(_ta_allocs, allocs_before_set);
  REQUIRE_GT(_ta_frees, frees_before_set);

  cjson_destroy(doc);
}

TEST(custom_alloc, clone_uses_custom_alloc) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson src = cjson_parse_mp("{\"k\":\"val\",\"n\":1}", NULL, &_tracking_alloc);
  REQUIRE_NE((void *)src, NULL);

  size_t allocs_before = _ta_allocs;
  cjson copy = cjson_clone(src);
  REQUIRE_NE((void *)copy, NULL);
  REQUIRE_GT(_ta_allocs, allocs_before);

  cjson_destroy(src);
  cjson_destroy(copy);

  REQUIRE_GT(_ta_frees, (size_t)0);
}

TEST(custom_alloc, default_alloc_bypasses_tracking) {
  /* When NULL is passed (default allocator), the tracking allocator must not
   * be called at all. */
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson n = cjson_create_string("test");
  REQUIRE_NE((void *)n, NULL);

  REQUIRE_EQ(_ta_allocs, (size_t)0);
  REQUIRE_EQ(_ta_frees, (size_t)0);

  cjson_destroy(n);
}

TEST(custom_alloc, custom_alloc_frees_on_destroy) {
  /* Verify alloc and free counts balance for a non-trivial tree. */
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson root = cjson_parse_mp("{\"a\":1,\"b\":\"hello\",\"c\":[1,2,3]}",
                              NULL, &_tracking_alloc);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_GT(_ta_allocs, (size_t)0);

  size_t allocs_at_destroy = _ta_allocs;
  (void)allocs_at_destroy;

  cjson_destroy(root);

  /* Every allocation must be matched by a free. */
  REQUIRE_EQ(_ta_allocs, _ta_frees);
}
