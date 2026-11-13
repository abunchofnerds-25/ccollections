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
#include <float.h>
#include <internal/cprocsintern.h>
#include <internal/test_helpers/cjson.h>
#include <internal/test_helpers/cprocsintern.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// Resets the signal state the binary inherited, because the forked children of
// these tests report through their wait status (see
// test_reset_inherited_signals()).
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

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
  cjson a = cjson_create_list();
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_type(a), CJSON_LIST);
  REQUIRE_EQ(cjson_list_size(a), (size_t)0);
  cjson_destroy(a);
}

TEST(construction, array_push) {
  cjson a = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(a, cjson_create_int(1)), ccol_success);
  REQUIRE_EQ(cjson_list_push(a, cjson_create_int(2)), ccol_success);
  REQUIRE_EQ(cjson_list_push(a, cjson_create_int(3)), ccol_success);
  REQUIRE_EQ(cjson_list_size(a), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(a, 0)), 1LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(a, 2)), 3LL);
  REQUIRE_EQ((void *)cjson_list_get(a, 99), NULL);
  cjson_destroy(a);
}

TEST(construction, object_empty) {
  cjson o = cjson_create_dictionary();
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_type(o), CJSON_DICTIONARY);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)0);
  cjson_destroy(o);
}

TEST(construction, object_set_get) {
  cjson o = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(o, "x", cjson_create_int(10)), ccol_success);
  REQUIRE_EQ(cjson_dictionary_set(o, "y", cjson_create_string("hi")),
             ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)2);

  cjson x = cjson_dictionary_get(o, "x");
  REQUIRE_NE((void *)x, NULL);
  REQUIRE_EQ(cjson_int_val(x), 10LL);

  cjson y = cjson_dictionary_get(o, "y");
  REQUIRE_NE((void *)y, NULL);
  REQUIRE_STREQ(cjson_str_val(y), "hi");

  REQUIRE_EQ((void *)cjson_dictionary_get(o, "missing"), NULL);
  cjson_destroy(o);
}

TEST(construction, object_replace_frees_old) {
  cjson o = cjson_create_dictionary();
  cjson_dictionary_set(o, "k", cjson_create_string("original"));
  cjson_dictionary_set(o, "k", cjson_create_int(99));
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(o, "k")), 99LL);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)1);
  cjson_destroy(o);
}

// The ownership tests: A node that already has an owner (a borrowed reference
// from a getter, a parsed child, the container itself) must be refused with
// ccol_invalid_args and the tree must stay intact. Two owners would each
// destroy the node, which corrupts the heap; with the default allocator the
// node pool's free list becomes a cycle and the drain at exit hangs.
TEST(ownership, dictionary_set_key_to_its_own_current_value_is_noop) {
  cjson o = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(o, "k", cjson_create_int(42)), ccol_success);
  cjson v = cjson_dictionary_get(o, "k");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cjson_dictionary_set(o, "k", v), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)1);
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(o, "k")), 42LL);
  cjson_destroy(o);
}

TEST(ownership, list_push_same_owned_node_twice_rejected) {
  cjson arr = cjson_create_list();
  cjson item = cjson_create_int(7);
  REQUIRE_EQ(cjson_list_push(arr, item), ccol_success);
  REQUIRE_EQ(cjson_list_push(arr, item), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(arr), (size_t)1);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(arr, 0)), 7LL);
  cjson_destroy(arr);
}

TEST(ownership, list_push_node_already_in_another_list_rejected) {
  cjson arr1 = cjson_create_list();
  cjson arr2 = cjson_create_list();
  cjson item = cjson_create_int(3);
  REQUIRE_EQ(cjson_list_push(arr1, item), ccol_success);
  REQUIRE_EQ(cjson_list_push(arr2, item), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(arr1), (size_t)1);
  REQUIRE_EQ(cjson_list_size(arr2), (size_t)0);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(arr1, 0)), 3LL);
  cjson_destroy(arr1);
  cjson_destroy(arr2);
}

TEST(ownership, dictionary_set_same_node_under_two_keys_rejected) {
  cjson o = cjson_create_dictionary();
  cjson item = cjson_create_int(9);
  REQUIRE_EQ(cjson_dictionary_set(o, "a", item), ccol_success);
  REQUIRE_EQ(cjson_dictionary_set(o, "b", item), ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)1);
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(o, "a")), 9LL);
  REQUIRE_EQ((void *)cjson_dictionary_get(o, "b"), NULL);
  cjson_destroy(o);
}

TEST(ownership, dictionary_get_borrowed_reference_cannot_be_repushed) {
  cjson o = cjson_create_dictionary();
  cjson arr = cjson_create_list();
  REQUIRE_EQ(cjson_dictionary_set(o, "k", cjson_create_string("owned")),
             ccol_success);
  cjson borrowed = cjson_dictionary_get(o, "k");
  REQUIRE_EQ(cjson_list_push(arr, borrowed), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(arr), (size_t)0);
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(o, "k")), "owned");
  cjson_destroy(o);
  cjson_destroy(arr);
}

TEST(ownership, list_get_borrowed_reference_cannot_be_reset) {
  cjson arr = cjson_create_list();
  cjson o = cjson_create_dictionary();
  REQUIRE_EQ(cjson_list_push(arr, cjson_create_int(5)), ccol_success);
  cjson borrowed = cjson_list_get(arr, 0);
  REQUIRE_EQ(cjson_dictionary_set(o, "x", borrowed), ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)0);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(arr, 0)), 5LL);
  cjson_destroy(arr);
  cjson_destroy(o);
}

TEST(ownership, list_push_self_rejected) {
  cjson arr = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(arr, arr), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(arr), (size_t)0);
  cjson_destroy(arr);
}

TEST(ownership, dictionary_set_self_rejected) {
  cjson o = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(o, "self", o), ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)0);
  cjson_destroy(o);
}

// A self-attach into the wrong kind of container must be answered by the
// self-check, not the type check, which would deep-free a node the caller still
// owns. The node is used again afterwards, so the wrong order shows up under
// memtest as a use-after-free and a double free.
TEST(ownership, list_push_self_on_a_dictionary_leaves_the_node_alive) {
  cjson obj = cjson_create_dictionary();
  REQUIRE_EQ(cjson_list_push(obj, obj), ccol_invalid_args);
  REQUIRE_EQ(cjson_type(obj), CJSON_DICTIONARY);
  REQUIRE_EQ(cjson_dictionary_set(obj, "k", cjson_create_int(1)), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(obj), (size_t)1);
  cjson_destroy(obj);
}

TEST(ownership, dictionary_set_self_on_a_list_leaves_the_node_alive) {
  cjson arr = cjson_create_list();
  REQUIRE_EQ(cjson_dictionary_set(arr, "k", arr), ccol_invalid_args);
  REQUIRE_EQ(cjson_type(arr), CJSON_LIST);
  REQUIRE_EQ(cjson_list_push(arr, cjson_create_int(1)), ccol_success);
  REQUIRE_EQ(cjson_list_size(arr), (size_t)1);
  cjson_destroy(arr);
}

TEST(ownership, dictionary_set_self_with_a_null_key_leaves_the_node_alive) {
  cjson obj = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(obj, NULL, obj), ccol_invalid_args);
  REQUIRE_EQ(cjson_type(obj), CJSON_DICTIONARY);
  REQUIRE_EQ(cjson_dictionary_set(obj, "k", cjson_create_int(1)), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(obj), (size_t)1);
  cjson_destroy(obj);
}

TEST(ownership, list_push_self_on_a_scalar_leaves_the_node_alive) {
  cjson s = cjson_create_string("v");
  REQUIRE_EQ(cjson_list_push(s, s), ccol_invalid_args);
  REQUIRE_EQ(cjson_type(s), CJSON_STRING);
  REQUIRE_STREQ(cjson_str_val(s), "v");
  cjson_destroy(s);
}

TEST(ownership, parsed_child_cannot_be_repushed_elsewhere) {
  cjson root = cjson_parse("{\"items\":[1,2,3]}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson items = cjson_get(root, "items");
  cjson elem = cjson_list_get(items, 0);
  cjson other = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(other, elem), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(other), (size_t)0);
  REQUIRE_EQ(cjson_list_size(items), (size_t)3);
  cjson_destroy(root);
  cjson_destroy(other);
}

TEST(ownership, clone_produces_independently_pushable_node) {
  cjson o = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(o, "k", cjson_create_int(11)), ccol_success);
  cjson borrowed = cjson_dictionary_get(o, "k");
  cjson cloned = cjson_clone(borrowed);
  REQUIRE_NE((void *)cloned, NULL);
  cjson arr = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(arr, cloned), ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(arr, 0)), 11LL);
  cjson_destroy(o);
  cjson_destroy(arr);
}

// The root is never attached, so only the reachability check refuses pushing it
// into its own descendant; the cycle would make the teardown free a node twice.
TEST(ownership, list_push_ancestor_into_own_descendant_rejected) {
  cjson root = cjson_create_dictionary();
  cjson child = cjson_create_list();
  REQUIRE_EQ(cjson_dictionary_set(root, "self", child), ccol_success);
  REQUIRE_EQ(cjson_list_push(child, root), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(child), (size_t)0);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  REQUIRE_EQ((void *)cjson_dictionary_get(root, "self"), (void *)child);
  cjson_destroy(root);
}

TEST(ownership, dictionary_set_ancestor_into_own_descendant_rejected) {
  cjson root = cjson_create_list();
  cjson child = cjson_create_dictionary();
  REQUIRE_EQ(cjson_list_push(root, child), ccol_success);
  REQUIRE_EQ(cjson_dictionary_set(child, "back", root), ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_size(child), (size_t)0);
  REQUIRE_EQ(cjson_list_size(root), (size_t)1);
  REQUIRE_EQ((void *)cjson_list_get(root, 0), (void *)child);
  cjson_destroy(root);
}

TEST(ownership, list_push_multi_level_ancestor_into_descendant_rejected) {
  cjson root = cjson_create_list();
  cjson a = cjson_create_list();
  cjson b = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(root, a), ccol_success);
  REQUIRE_EQ(cjson_list_push(a, b), ccol_success);
  REQUIRE_EQ(cjson_list_push(b, root), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(b), (size_t)0);
  REQUIRE_EQ(cjson_list_size(a), (size_t)1);
  REQUIRE_EQ(cjson_list_size(root), (size_t)1);
  cjson_destroy(root);
}

// With an attached target the cycle check walks the whole child, and for a push
// that is acyclic that walk must still say "no cycle".
TEST(ownership,
     list_push_large_child_into_already_attached_target_still_succeeds) {
  cjson root = cjson_create_list();
  cjson holder = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(root, holder), ccol_success);

  cjson big = cjson_create_list();
  for (int i = 0; i < 500; i++)
    REQUIRE_EQ(cjson_list_push(big, cjson_create_int(i)), ccol_success);

  REQUIRE_EQ(cjson_list_push(holder, big), ccol_success);
  REQUIRE_EQ(cjson_list_size(holder), (size_t)1);
  REQUIRE_EQ(cjson_list_size(big), (size_t)500);
  cjson_destroy(root);
}

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

// Parses len bytes of src and tells whether the parse failed with exactly the
// message want, for the tests that pin an error text.
static bool _cjson_test_parse_fails_with(const char *src, size_t len,
                                         const char *want) {
  char *err = NULL;
  cjson n = cjson_parse_n(src, len, &err);
  if (n) {
    cjson_destroy(n);
    return false;
  }
  return err && strcmp(err, want) == 0;
}
// Calls _cjson_test_parse_fails_with() on a string literal lit (its length
// without the NUL) and evaluates to its result.
#define CJSON_TEST_PARSE_FAILS_WITH(lit, want) \
  _cjson_test_parse_fails_with((lit), sizeof(lit) - 1, (want))

TEST(parse, lone_low_surrogate_is_rejected) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\\uDC00\"",
      "low surrogate escape \\uDC00 at position 1 does not follow a high "
      "surrogate escape"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"ok\\uDE00\"",
      "low surrogate escape \\uDE00 at position 3 does not follow a high "
      "surrogate escape"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\\uD83D\\uDE00\\uDC00\"",
      "low surrogate escape \\uDC00 at position 13 does not follow a high "
      "surrogate escape"));
}

TEST(parse, lone_high_surrogate_is_rejected) {
  static const char *const want =
      "high surrogate escape \\uD800 at position 1 is not followed by a low "
      "surrogate escape";
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH("\"\\uD800\"", want));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH("\"\\uD800\\u0041\"", want));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH("\"\\uD800x\"", want));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH("\"\\uD800\\n\"", want));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH("\"\\uD800\\uD800\"", want));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH("\"\\uD800\\uD83D\\uDE00\"", want));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\\uDBFF\"",
      "high surrogate escape \\uDBFF at position 1 is not followed by a low "
      "surrogate escape"));
}

TEST(parse, surrogate_pair_at_both_ends_of_the_ranges_is_accepted) {
  cjson lo = cjson_parse("\"\\uD800\\uDC00\"", NULL);
  cjson hi = cjson_parse("\"\\uDBFF\\uDFFF\"", NULL);
  bool lo_ok = lo && strcmp(cjson_str_val(lo), "\xF0\x90\x80\x80") == 0;
  bool hi_ok = hi && strcmp(cjson_str_val(hi), "\xF4\x8F\xBF\xBF") == 0;
  cjson_destroy(lo);
  cjson_destroy(hi);
  REQUIRE_TRUE(lo_ok);
  REQUIRE_TRUE(hi_ok);
}

TEST(parse, keys_that_differ_only_in_an_unpaired_surrogate_never_merge) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "{\"role\\uD800\":\"admin\",\"role\\uDC00\":\"user\"}",
      "high surrogate escape \\uD800 at position 6 is not followed by a low "
      "surrogate escape"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "{\"k\\ufffd\":1,\"k\\udfff\":2}",
      "low surrogate escape \\uDFFF at position 15 does not follow a high "
      "surrogate escape"));
}

TEST(parse, null_byte_in_string_rejected) {
  REQUIRE_EQ((void *)cjson_parse("\"\\u0000\"", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("\"hello\\u0000world\"", NULL), NULL);
}

TEST(parse, unknown_escape_rejected) {
  REQUIRE_EQ((void *)cjson_parse("\"\\z\"", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("\"\\q\"", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("\"hello\\z\"", NULL), NULL);
}

TEST(parse, incomplete_escape_at_string_end) {
  REQUIRE_EQ((void *)cjson_parse("\"hello\\", NULL), NULL);
}

TEST(parse, empty_array) {
  cjson a = cjson_parse("[]", NULL);
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_type(a), CJSON_LIST);
  REQUIRE_EQ(cjson_list_size(a), (size_t)0);
  cjson_destroy(a);
}

TEST(parse, array_of_mixed) {
  cjson a = cjson_parse("[null, true, 1, 2.5, \"x\"]", NULL);
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_list_size(a), (size_t)5);
  REQUIRE_EQ(cjson_type(cjson_list_get(a, 0)), CJSON_NULL);
  REQUIRE_TRUE(cjson_bool_val(cjson_list_get(a, 1)));
  REQUIRE_EQ(cjson_int_val(cjson_list_get(a, 2)), 1LL);
  REQUIRE_EQ(cjson_double_val(cjson_list_get(a, 3)), 2.5);
  REQUIRE_STREQ(cjson_str_val(cjson_list_get(a, 4)), "x");
  cjson_destroy(a);
}

TEST(parse, empty_object) {
  cjson o = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_type(o), CJSON_DICTIONARY);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)0);
  cjson_destroy(o);
}

TEST(parse, simple_object) {
  cjson o = cjson_parse("{\"a\":1, \"b\":\"two\", \"c\":true}", NULL);
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(o, "a")), 1LL);
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(o, "b")), "two");
  REQUIRE_TRUE(cjson_bool_val(cjson_dictionary_get(o, "c")));
  cjson_destroy(o);
}

TEST(parse, nested) {
  cjson root = cjson_parse(
      "{\"users\":[{\"name\":\"Alice\",\"age\":30},{\"name\":\"Bob\",\"age\":"
      "25}]}",
      NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson users = cjson_dictionary_get(root, "users");
  REQUIRE_EQ(cjson_list_size(users), (size_t)2);
  cjson alice = cjson_list_get(users, 0);
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(alice, "name")), "Alice");
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(alice, "age")), 30LL);
  cjson bob = cjson_list_get(users, 1);
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(bob, "name")), "Bob");
  cjson_destroy(root);
}

TEST(parse, whitespace_everywhere) {
  cjson o = cjson_parse("  {  \"k\"  :  [  1  ,  2  ]  }  ", NULL);
  REQUIRE_NE((void *)o, NULL);
  REQUIRE_EQ(cjson_list_size(cjson_dictionary_get(o, "k")), (size_t)2);
  cjson_destroy(o);
}

TEST(parse, error_empty) {
  char *err_str = NULL;
  cjson n = cjson_parse_mp("", &err_str, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err_str, NULL);
  REQUIRE_TRUE(strlen(err_str) > 0);
}

TEST(parse, error_trailing_garbage) {
  char *err_str = NULL;
  cjson n = cjson_parse_mp("42 garbage", &err_str, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err_str, NULL);
  REQUIRE_TRUE(strlen(err_str) > 0);
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
}

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

// Serializes the double v alone and gives the text, which the caller frees with
// cjson_serialize_free(); the shortest-digits tests use it.
static char *_ser_double(double v) {
  cjson n = cjson_create_double(v);
  if (!n) return NULL;
  char *s = cjson_serialize(n);
  cjson_destroy(n);
  return s;
}

// Counts the significant digits of a "%g"-style number (before any exponent,
// without leading and trailing zeros, so a trailing ".0" doesn't count), for
// the shortest-digits tests.
static int _significant_digits(const char *s) {
  char digits[64];
  size_t n = 0;
  for (; *s && *s != 'e' && *s != 'E'; s++)
    if (*s >= '0' && *s <= '9' && n < sizeof(digits)) digits[n++] = *s;
  size_t lo = 0;
  while (lo < n && digits[lo] == '0') lo++;
  while (n > lo && digits[n - 1] == '0') n--;
  return (int)(n - lo);
}

TEST(serialize, double_is_written_in_its_shortest_form) {
  struct {
    double v;
    const char *want;
  } cases[] = {
      {1.0 / 3.0, "0.3333333333333333"},
      {2.0 / 3.0, "0.6666666666666666"},
      {0.1, "0.1"},
      {0.1 + 0.2, "0.30000000000000004"},
      {DBL_MAX, "1.7976931348623157e+308"},
      {DBL_MIN, "2.2250738585072014e-308"},
      {5e-324, "5e-324"},
      {-5e-324, "-5e-324"},
      {1.5e-323, "1.5e-323"},
      {1e-310, "1e-310"},
      {-2.5e-320, "-2.5e-320"},
  };
  bool all_ok = true;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *s = _ser_double(cases[i].v);
    bool ok = s && strcmp(s, cases[i].want) == 0;
    if (!ok) {
      printf("value %d: got \"%s\", want \"%s\"\n", (int)i, s ? s : "(null)",
             cases[i].want);
      all_ok = false;
    }
    cjson_serialize_free(s);
  }
  REQUIRE_TRUE(all_ok);
}

// For each random bit pattern (half of them subnormal or zero) the reference is
// the smallest precision whose "%.*g" reads back the same, and the serializer
// must read back the same with exactly that many digits.
TEST(serialize, random_doubles_use_the_fewest_digits_that_read_back) {
  uint64_t state = 0x9E3779B97F4A7C15ull;
  int failures = 0;
  for (int i = 0; i < 20000 && failures < 5; i++) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t bits = state ^ (state >> 29);
    if (i & 1) bits &= 0x800FFFFFFFFFFFFFull;
    double v;
    memcpy(&v, &bits, sizeof(v));
    if (!isfinite(v)) continue;
    char ref[40];
    int p = 1;
    for (; p < 17; p++) {
      snprintf(ref, sizeof(ref), "%.*g", p, v);
      if (strtod(ref, NULL) == v) break;
    }
    char *s = _ser_double(v);
    bool ok = s && strtod(s, NULL) == v && _significant_digits(s) <= p &&
              (v == 0.0 || _significant_digits(s) == p);
    if (!ok) {
      printf("bits 0x%016llx: got \"%s\", shortest has %d digits\n",
             (unsigned long long)bits, s ? s : "(null)", p);
      failures++;
    }
    cjson_serialize_free(s);
  }
  REQUIRE_EQ(failures, 0);
}

TEST(serialize, string_escaping) {
  cjson n = cjson_create_string("tab:\there\nnewline\\back\"quote");
  char *s = cjson_serialize(n);
  REQUIRE_STREQ(s, "\"tab:\\there\\nnewline\\\\back\\\"quote\"");
  cjson_serialize_free(s);
  cjson_destroy(n);
}

TEST(serialize, empty_array) {
  cjson a = cjson_create_list();
  char *s = cjson_serialize(a);
  REQUIRE_STREQ(s, "[]");
  cjson_serialize_free(s);
  cjson_destroy(a);
}

TEST(serialize, simple_array) {
  cjson a = cjson_create_list();
  cjson_list_push(a, cjson_create_int(1));
  cjson_list_push(a, cjson_create_int(2));
  cjson_list_push(a, cjson_create_int(3));
  char *s = cjson_serialize(a);
  REQUIRE_STREQ(s, "[1,2,3]");
  cjson_serialize_free(s);
  cjson_destroy(a);
}

TEST(serialize, empty_object) {
  cjson o = cjson_create_dictionary();
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
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(root2, "name")), "Alice");
  REQUIRE_TRUE(cjson_bool_val(cjson_dictionary_get(root2, "active")));
  cjson arr = cjson_dictionary_get(root2, "scores");
  REQUIRE_EQ(cjson_list_size(arr), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(arr, 1)), 20LL);

  cjson_serialize_free(out);
  cjson_destroy(root);
  cjson_destroy(root2);
}

// A whole-number CJSON_FLOAT must keep a point or an exponent in the output, or
// it parses back as CJSON_INTEGER.
TEST(serialize, float_type_roundtrip) {
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
  const char *original = "caf\xC3\xA9";
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
  cjson o = cjson_create_dictionary();
  cjson_dictionary_set(o, "a", cjson_create_int(1));
  char *pretty = cjson_serialize_pretty(o, 2);
  REQUIRE_NE((void *)pretty, NULL);
  REQUIRE_TRUE(strchr(pretty, '\n') != NULL);
  REQUIRE_TRUE(strchr(pretty, ' ') != NULL);
  cjson_serialize_free(pretty);
  cjson_destroy(o);
}

TEST(serialize, pretty_indent_zero_defaults_to_four) {
  cjson o = cjson_create_dictionary();
  cjson_dictionary_set(o, "a", cjson_create_int(1));
  char *pretty = cjson_serialize_pretty(o, 0);
  REQUIRE_NE((void *)pretty, NULL);
  REQUIRE_TRUE(strchr(pretty, '\n') != NULL);
  REQUIRE_TRUE(strstr(pretty, "    ") != NULL);
  cjson_serialize_free(pretty);
  cjson_destroy(o);
}

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
      "\"Bob\"}}]}",
      NULL);
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

TEST(navigate, set_new_scalar_in_object) {
  cjson root = cjson_parse("{\"name\":\"Alice\"}", NULL);
  ccol_retval r = cjson_set(root, "score", 42);
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

// Gives the value cjson_parse() makes of a decimal literal, or -1. The tests of
// unsigned values above LLONG_MAX hold cjson_set() to it, so the expectation
// comes from the parser.
static double u64_parsed_value(const char *literal) {
  char doc[64];
  snprintf(doc, sizeof(doc), "{\"v\":%s}", literal);
  cjson parsed = cjson_parse(doc, NULL);
  if (!parsed) return -1.0;
  cjson v = cjson_get(parsed, "v");
  double d = cjson_type(v) == CJSON_FLOAT ? cjson_double_val(v) : -1.0;
  cjson_destroy(parsed);
  return d;
}

// Sets value through every path that writes a scalar (a new key, a key that
// holds a subtree, a list element) and requires each to read back, and to round
// trip, as the CJSON_FLOAT the parser makes of literal.
#define U64_CHECK_ABOVE_LLONG_MAX(value, literal)                           \
  do {                                                                      \
    double u64_expected = u64_parsed_value(literal);                        \
    REQUIRE_GT(u64_expected, 9.2e18);                                       \
    cjson u64_root = cjson_parse("{\"old\":{\"x\":1},\"l\":[0]}", NULL);    \
    REQUIRE_NE((void *)u64_root, NULL);                                     \
    ccol_retval u64_r1 = cjson_set(u64_root, "new", (value));               \
    ccol_retval u64_r2 = cjson_set(u64_root, "old", (value));               \
    ccol_retval u64_r3 = cjson_set(u64_root, "l.#0", (value));              \
    int u64_types_ok =                                                      \
        cjson_type(cjson_get(u64_root, "new")) == CJSON_FLOAT &&            \
        cjson_type(cjson_get(u64_root, "old")) == CJSON_FLOAT &&            \
        cjson_type(cjson_get(u64_root, "l.#0")) == CJSON_FLOAT;             \
    double u64_new =                                                        \
        u64_types_ok ? cjson_double_val(cjson_get(u64_root, "new")) : 0.0;  \
    double u64_old =                                                        \
        u64_types_ok ? cjson_double_val(cjson_get(u64_root, "old")) : 0.0;  \
    double u64_elem =                                                       \
        u64_types_ok ? cjson_double_val(cjson_get(u64_root, "l.#0")) : 0.0; \
    char *u64_text = cjson_serialize(u64_root);                             \
    cjson u64_again = u64_text ? cjson_parse(u64_text, NULL) : NULL;        \
    cjson_serialize_free(u64_text);                                         \
    cjson u64_rt = u64_again ? cjson_get(u64_again, "new") : NULL;          \
    int u64_rt_ok = u64_rt && cjson_type(u64_rt) == CJSON_FLOAT &&          \
                    cjson_double_val(u64_rt) == u64_expected;               \
    cjson_destroy(u64_again);                                               \
    cjson_destroy(u64_root);                                                \
    REQUIRE_EQ(u64_r1, ccol_success);                                       \
    REQUIRE_EQ(u64_r2, ccol_success);                                       \
    REQUIRE_EQ(u64_r3, ccol_success);                                       \
    REQUIRE_TRUE(u64_types_ok);                                             \
    REQUIRE_EQ(u64_new, u64_expected);                                      \
    REQUIRE_EQ(u64_old, u64_expected);                                      \
    REQUIRE_EQ(u64_elem, u64_expected);                                     \
    REQUIRE_TRUE(u64_rt_ok);                                                \
  } while (0)

TEST(navigate, set_unsigned_above_llong_max_becomes_float) {
  U64_CHECK_ABOVE_LLONG_MAX(ULLONG_MAX, "18446744073709551615");
  U64_CHECK_ABOVE_LLONG_MAX((unsigned long long)LLONG_MAX + 1ULL,
                            "9223372036854775808");
  U64_CHECK_ABOVE_LLONG_MAX((uint64_t)UINT64_MAX, "18446744073709551615");
  U64_CHECK_ABOVE_LLONG_MAX((uint64_t)12345678901234567890ULL,
                            "12345678901234567890");
#if ULONG_MAX > 0xFFFFFFFFUL
  U64_CHECK_ABOVE_LLONG_MAX((unsigned long)ULONG_MAX, "18446744073709551615");
#endif
#if SIZE_MAX > 0xFFFFFFFFu
  U64_CHECK_ABOVE_LLONG_MAX((size_t)SIZE_MAX, "18446744073709551615");
#endif
}

TEST(navigate, set_unsigned_up_to_llong_max_stays_integer) {
  cjson root = cjson_parse("{\"a\":0,\"b\":0,\"c\":0,\"d\":0}", NULL);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval ra =
      cjson_set(root, "a", (unsigned long long)9223372036854775807ULL);
  ccol_retval rb = cjson_set(root, "b", (uint64_t)0);
  ccol_retval rc = cjson_set(root, "c", (unsigned long)ULONG_MAX);
  ccol_retval rd = cjson_set(root, "d", (uint32_t)UINT32_MAX);
  int types_ok = cjson_type(cjson_get(root, "a")) == CJSON_INTEGER &&
                 cjson_type(cjson_get(root, "b")) == CJSON_INTEGER &&
                 cjson_type(cjson_get(root, "d")) == CJSON_INTEGER;
  long long a = types_ok ? cjson_int_val(cjson_get(root, "a")) : 0;
  long long b = types_ok ? cjson_int_val(cjson_get(root, "b")) : -1;
  long long d = types_ok ? cjson_int_val(cjson_get(root, "d")) : 0;
  cjson_node_type ct = cjson_type(cjson_get(root, "c"));
  long long c = ct == CJSON_INTEGER ? cjson_int_val(cjson_get(root, "c")) : 0;
  cjson_destroy(root);
  REQUIRE_EQ(ra, ccol_success);
  REQUIRE_EQ(rb, ccol_success);
  REQUIRE_EQ(rc, ccol_success);
  REQUIRE_EQ(rd, ccol_success);
  REQUIRE_TRUE(types_ok);
  REQUIRE_EQ(a, 9223372036854775807LL);
  REQUIRE_EQ(b, 0LL);
  REQUIRE_EQ(d, 4294967295LL);
#if ULONG_MAX > 0xFFFFFFFFUL
  REQUIRE_EQ(ct, CJSON_FLOAT);
#else
  REQUIRE_EQ(ct, CJSON_INTEGER);
  REQUIRE_EQ(c, 4294967295LL);
#endif
  (void)c;
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
  cjson null_node = cjson_get(root, "k");
  REQUIRE_NE((void *)null_node, NULL);
  REQUIRE_EQ(cjson_type(null_node), CJSON_NULL);
  cjson_destroy(root);
}

TEST(navigate, set_missing_parent_fails) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  ccol_retval r = cjson_set(root, "nonexistent.child", 5);
  REQUIRE_NE(r, ccol_success);
  cjson_destroy(root);
}

TEST(navigate, set_empty_path_returns_invalid_args) {
  cjson root = cjson_parse("{\"k\":1}", NULL);
  ccol_retval r = cjson_set(root, "", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "k")), 1LL);
  cjson_destroy(root);
}

// A well-formed #N past the end names an absent element, so it's
// ccol_key_not_found, not ccol_invalid_args.
TEST(navigate, set_array_index_out_of_bounds) {
  cjson root = cjson_parse("{\"arr\":[1,2]}", NULL);
  ccol_retval r = cjson_set(root, "arr.#99", 5);
  REQUIRE_EQ(r, ccol_key_not_found);
  cjson_destroy(root);
}

TEST(navigate, array_index_too_large_for_any_integer_is_not_found) {
  static const char *const paths[] = {
      "arr.#4294967296",
      "arr.#18446744073709551616",
      "arr.#99999999999999999999999999999999999999",
  };
  cjson root = cjson_parse("{\"arr\":[1,2]}", NULL);
  REQUIRE_NE((void *)root, NULL);
  bool all_ok = true;
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    if (cjson_get(root, paths[i]) != NULL) all_ok = false;
    if (cjson_set(root, paths[i], 5) != ccol_key_not_found) all_ok = false;
    if (cjson_remove(root, paths[i]) != ccol_key_not_found) all_ok = false;
  }
  size_t len = cjson_list_size(cjson_get(root, "arr"));
  cjson_destroy(root);
  REQUIRE_TRUE(all_ok);
  REQUIRE_EQ(len, (size_t)2);
}

TEST(navigate, set_array_index_malformed_is_invalid_args) {
  cjson root = cjson_parse("{\"arr\":[1,2]}", NULL);
  REQUIRE_EQ(cjson_set(root, "arr.#abc", 5), ccol_invalid_args);
  REQUIRE_EQ(cjson_set(root, "arr.#-1", 5), ccol_invalid_args);
  cjson_destroy(root);
}

// strtol() would accept "# 1" and "#+1"; get, set and delete each parse #N with
// their own code, so all three are checked.
TEST(navigate, array_index_with_leading_whitespace_or_sign_is_invalid_args) {
  cjson root = cjson_parse("{\"arr\":[10,20,30]}", NULL);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ((void *)cjson_get(root, "arr.#  1"), NULL);
  REQUIRE_EQ((void *)cjson_get(root, "arr.# 1"), NULL);
  REQUIRE_EQ((void *)cjson_get(root, "arr.#+1"), NULL);

  REQUIRE_EQ(cjson_set(root, "arr.#  1", 99), ccol_invalid_args);
  REQUIRE_EQ(cjson_set(root, "arr.#+1", 99), ccol_invalid_args);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "arr.#1")), 20LL);

  REQUIRE_EQ(cjson__remove(root, "arr.#  1"), ccol_invalid_args);
  REQUIRE_EQ(cjson__remove(root, "arr.#+1"), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(cjson_get(root, "arr")), (size_t)3);

  cjson_destroy(root);
}

TEST(navigate, consecutive_dots_get_rejected) {
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", NULL);
  REQUIRE_EQ((void *)cjson_get(root, "a..b"), NULL);
  REQUIRE_EQ((void *)cjson_get(root, "a."), NULL);
  cjson_destroy(root);
}

TEST(navigate, consecutive_dots_set_rejected) {
  cjson root = cjson_parse("{\"a\":{\"b\":0}}", NULL);
  ccol_retval r = cjson_set(root, "a..b", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a.b")), 0LL);
  cjson_destroy(root);
}

TEST(navigate, set_trailing_dot_with_nonexistent_parent_still_invalid_args) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  ccol_retval r = cjson_set(root, "missing.", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "missing"), NULL);
  cjson_destroy(root);
}

TEST(navigate,
     consecutive_dots_set_with_nonexistent_first_component_still_invalid_args) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  ccol_retval r = cjson_set(root, "missing..b", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "missing"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_new_key_in_nested_object) {
  cjson root = cjson_parse("{\"user\":{}}", NULL);
  ccol_retval r = cjson_set(root, "user.name", "Alice");
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_STREQ(cjson_str_val(cjson_get(root, "user.name")), "Alice");
  cjson_destroy(root);
}

TEST(navigate, set_on_array_root) {
  cjson root = cjson_parse("[10, 20, 30]", NULL);
  ccol_retval r = cjson_set(root, "#1", 99);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 99LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 10LL);
  cjson_destroy(root);
}

// A 4-byte float takes the sizeof(float) branch, separate from double.
TEST(navigate, set_float_literal) {
  cjson root = cjson_parse("{\"v\":0}", NULL);
  float f = 2.5f;
  cjson_set(root, "v", f);
  REQUIRE_EQ(cjson_type(cjson_get(root, "v")), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(cjson_get(root, "v")), 2.5);
  cjson_destroy(root);
}

TEST(navigate, set_list_slot_composite_to_scalar) {
  cjson root = cjson_parse("{\"items\":[{\"a\":1},\"two\",{\"b\":2}]}", NULL);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval r = cjson_set(root, "items.#0", 99);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cjson_type(cjson_get(root, "items.#0")), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "items.#0")), 99LL);
  REQUIRE_STREQ(cjson_str_val(cjson_get(root, "items.#1")), "two");
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "items.#2.b")), 2LL);
  cjson_destroy(root);
}

TEST(navigate, leading_dot_get_returns_null) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ((void *)cjson_get(root, ".a"), NULL);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(navigate, leading_dot_set_returns_invalid_args) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, ".a", 99), ccol_invalid_args);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a")), 1LL);
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
  ccol_retval r = cjson_set(root, "arr.#", 99);
  REQUIRE_NE(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "arr.#0")), 10LL);
  cjson_destroy(root);
}

TEST(clone, scalar) {
  cjson orig = cjson_create_string("hello");
  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);
  REQUIRE_NE((void *)cjson_str_val(copy), (void *)cjson_str_val(orig));
  REQUIRE_STREQ(cjson_str_val(copy), "hello");
  cjson_destroy(orig);
  REQUIRE_STREQ(cjson_str_val(copy), "hello");
  cjson_destroy(copy);
}

TEST(clone, null_handle) { REQUIRE_EQ((void *)cjson_clone(NULL), NULL); }

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

TEST(type_macro, bool_detection) {
  bool b = true;
  REQUIRE_EQ(cjson__type_of(b), CJSON_BOOL);
}

TEST(type_macro, int_detection) {
  int i = 0;
  REQUIRE_EQ(cjson__type_of(i), CJSON_INTEGER);
}

TEST(type_macro, long_long_detection) {
  long long ll = 0;
  REQUIRE_EQ(cjson__type_of(ll), CJSON_INTEGER);
}

TEST(type_macro, double_detection) {
  double d = 0.0;
  REQUIRE_EQ(cjson__type_of(d), CJSON_FLOAT);
}

TEST(type_macro, float_detection) {
  float f = 0.0f;
  REQUIRE_EQ(cjson__type_of(f), CJSON_FLOAT);
}

TEST(type_macro, string_detection) {
  const char *s = "hi";
  REQUIRE_EQ(cjson__type_of(s), CJSON_STRING);
}

TEST(type_macro, char_ptr_detection) {
  char *s = NULL;
  REQUIRE_EQ(cjson__type_of(s), CJSON_STRING);
}

TEST(type_macro, null_is_null_type) {
  REQUIRE_EQ(cjson__type_of(NULL), CJSON_NULL);
}

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
  REQUIRE_EQ(cjson_type(root), CJSON_LIST);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "#2.k")), 3LL);
  cjson_destroy(root);
}

TEST(edge, set_creates_multiple_new_keys) {
  cjson root = cjson_parse("{}", NULL);
  cjson_set(root, "a", 1);
  cjson_set(root, "b", 2);
  cjson_set(root, "c", 3);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)3);
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

TEST(fuzzy, duplicate_key_no_leak) {
  cjson root = cjson_parse("{\"x\":1,\"x\":2,\"x\":3}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_type(root), CJSON_DICTIONARY);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
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

// Plain char is signed on x86 and unsigned on aarch64, so the expected value is
// the same char widened, never a hardcoded -1.
TEST(fuzzy, plain_char_negative_sign_extension) {
  cjson root = cjson_parse("{\"v\":0}", NULL);
  char c = -1;
  cjson_set(root, "v", c);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "v")), (long long)c);
  cjson_destroy(root);
}

TEST(fuzzy, long_decimal_number_parses) {
  cjson n = cjson_parse(
      "1000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000"
      "000000000000000000000000",
      NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  cjson_destroy(n);
}

TEST(fuzzy, negative_long_decimal_number_parses) {
  cjson n = cjson_parse(
      "-1000000000000000000000000000000000000000"
      "0000000000000000000000000000000000000000"
      "000000000000000000000000",
      NULL);
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
}

TEST(fuzzy, overflow_negative_float_is_parse_error) {
  char *err_str = NULL;
  cjson n = cjson_parse_mp("-1e999", &err_str, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err_str, NULL);
  REQUIRE_TRUE(strlen(err_str) > 0);
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
  ccol_retval r = cjson_set(root, "k", ptr);
  REQUIRE_EQ(r, ccol_success);
  cjson null_node = cjson_get(root, "k");
  REQUIRE_NE((void *)null_node, NULL);
  REQUIRE_EQ(cjson_type(null_node), CJSON_NULL);
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
  ccol_retval r = cjson_set(root, "v", INFINITY);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_type(cjson_get(root, "v")), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(cjson_get(root, "v")), 1.5);
  cjson_destroy(root);
}

TEST(navigate, get_escaped_dot_flat_key) {
  cjson root = cjson_parse("{\"a.b\":42}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "a\\.b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 42LL);
  REQUIRE_EQ((void *)cjson_get(root, "a.b"), NULL);
  cjson_destroy(root);
}

TEST(navigate, get_escaped_backslash_flat_key) {
  cjson root = cjson_parse("{\"a\\\\b\":7}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "a\\\\b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 7LL);
  cjson_destroy(root);
}

TEST(navigate, get_escaped_dot_nested_path) {
  cjson root = cjson_parse("{\"outer\":{\"k.ey\":99}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "outer.k\\.ey");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 99LL);
  cjson_destroy(root);
}

TEST(navigate, get_escaped_dot_both_components) {
  cjson root = cjson_parse("{\"a.b\":{\"c.d\":1}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "a\\.b.c\\.d");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 1LL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_dot_creates_new_key) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "x\\.y", 55), ccol_success);
  cjson node = cjson_get(root, "x\\.y");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 55LL);
  REQUIRE_EQ((void *)cjson_get(root, "x"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_dot_updates_existing_key) {
  cjson root = cjson_parse("{\"p.q\":0}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "p\\.q", 123), ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "p\\.q")), 123LL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_dot_nested_path) {
  cjson root = cjson_parse("{\"outer\":{\"k.ey\":0}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "outer.k\\.ey", 77), ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "outer.k\\.ey")), 77LL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_backslash_creates_key) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "a\\\\b", 9), ccol_success);
  REQUIRE_NE((void *)cjson_get(root, "a\\\\b"), NULL);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a\\\\b")), 9LL);
  cjson_destroy(root);
}

TEST(clone, flat_array) {
  cjson orig = cjson_create_list();
  cjson_list_push(orig, cjson_create_int(10));
  cjson_list_push(orig, cjson_create_int(20));
  cjson_list_push(orig, cjson_create_string("hi"));

  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);
  REQUIRE_EQ(cjson_type(copy), CJSON_LIST);
  REQUIRE_EQ(cjson_list_size(copy), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(copy, 0)), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(copy, 1)), 20LL);
  REQUIRE_STREQ(cjson_str_val(cjson_list_get(copy, 2)), "hi");

  cjson_set(copy, "#0", 99);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(orig, 0)), 10LL);

  cjson_destroy(orig);
  cjson_destroy(copy);
}

TEST(clone, flat_object) {
  cjson orig = cjson_create_dictionary();
  cjson_dictionary_set(orig, "a", cjson_create_int(1));
  cjson_dictionary_set(orig, "b", cjson_create_string("world"));
  cjson_dictionary_set(orig, "c", cjson_create_bool(true));

  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);
  REQUIRE_EQ(cjson_type(copy), CJSON_DICTIONARY);
  REQUIRE_EQ(cjson_dictionary_size(copy), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(copy, "a")), 1LL);
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(copy, "b")), "world");
  REQUIRE_TRUE(cjson_bool_val(cjson_dictionary_get(copy, "c")));

  cjson_dictionary_set(copy, "a", cjson_create_int(999));
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(orig, "a")), 1LL);

  cjson_destroy(orig);
  cjson_destroy(copy);
}

TEST(construction, object_set_replaces_old_child) {
  cjson o = cjson_create_dictionary();
  cjson_dictionary_set(o, "k", cjson_create_string("first"));
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(o, "k")), "first");

  cjson_dictionary_set(o, "k", cjson_create_string("second"));
  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(o, "k")), "second");
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)1);

  cjson_dictionary_set(o, "k", cjson_create_int(42));
  REQUIRE_EQ(cjson_int_val(cjson_dictionary_get(o, "k")), 42LL);

  cjson_destroy(o);
}

TEST(construction, array_push_null_arr_leaves_child_untouched) {
  cjson child = cjson_create_int(42);
  ccol_retval r = cjson_list_push(NULL, child);
  cjson_node_type t = cjson_type(child);
  cjson_destroy(child);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
}

TEST(construction, array_push_null_child_rejected) {
  cjson arr = cjson_create_list();
  ccol_retval r = cjson_list_push(arr, NULL);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(arr), (size_t)0);
  cjson_destroy(arr);
}

TEST(construction, array_push_wrong_type_leaves_child_untouched) {
  cjson not_array = cjson_create_int(7);
  cjson child = cjson_create_string("hi");
  ccol_retval r = cjson_list_push(not_array, child);
  cjson_node_type t = cjson_type(child);
  cjson_destroy(child);
  cjson_destroy(not_array);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_STRING);
}

TEST(construction, object_set_null_obj_leaves_child_untouched) {
  cjson child = cjson_create_int(1);
  ccol_retval r = cjson_dictionary_set(NULL, "k", child);
  cjson_node_type t = cjson_type(child);
  cjson_destroy(child);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
}

TEST(construction, object_set_wrong_type_leaves_child_untouched) {
  cjson not_obj = cjson_create_string("oops");
  cjson child = cjson_create_int(99);
  ccol_retval r = cjson_dictionary_set(not_obj, "k", child);
  cjson_node_type t = cjson_type(child);
  cjson_destroy(child);
  cjson_destroy(not_obj);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
}

TEST(construction, object_set_null_key_leaves_child_untouched) {
  cjson o = cjson_create_dictionary();
  cjson child = cjson_create_int(1);
  ccol_retval r = cjson_dictionary_set(o, NULL, child);
  cjson_node_type t = cjson_type(child);
  size_t n = cjson_dictionary_size(o);
  cjson_destroy(child);
  cjson_destroy(o);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
  REQUIRE_EQ(n, (size_t)0);
}

// An early ccol_invalid_args must also leave an already attached child alone;
// destroying it frees memory its real owner still points at, and the readback
// at the end becomes a use-after-free.
TEST(construction,
     array_push_wrong_type_leaves_already_attached_child_untouched) {
  cjson owner = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(owner, "k", cjson_create_string("owned")),
             ccol_success);
  cjson borrowed = cjson_dictionary_get(owner, "k");

  cjson not_array = cjson_create_int(7);
  REQUIRE_EQ(cjson_list_push(not_array, borrowed), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_push(NULL, borrowed), ccol_invalid_args);

  REQUIRE_STREQ(cjson_str_val(cjson_dictionary_get(owner, "k")), "owned");
  cjson_destroy(owner);
  cjson_destroy(not_array);
}

TEST(construction,
     object_set_wrong_type_leaves_already_attached_child_untouched) {
  cjson owner = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(owner, cjson_create_string("owned")),
             ccol_success);
  cjson borrowed = cjson_list_get(owner, 0);

  cjson not_obj = cjson_create_int(9);
  REQUIRE_EQ(cjson_dictionary_set(not_obj, "k", borrowed), ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_set(NULL, "k", borrowed), ccol_invalid_args);

  cjson valid_obj = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(valid_obj, NULL, borrowed),
             ccol_invalid_args);

  REQUIRE_STREQ(cjson_str_val(cjson_list_get(owner, 0)), "owned");
  cjson_destroy(owner);
  cjson_destroy(not_obj);
  cjson_destroy(valid_obj);
}

TEST(construction, array_get_null_arr_returns_null) {
  REQUIRE_EQ((void *)cjson_list_get(NULL, 0), NULL);
}

TEST(construction, object_get_null_returns_null) {
  REQUIRE_EQ((void *)cjson_dictionary_get(NULL, "k"), NULL);
  cjson o = cjson_create_dictionary();
  REQUIRE_EQ((void *)cjson_dictionary_get(o, NULL), NULL);
  cjson_destroy(o);
}

// A tracking allocator: The _ta_* functions forward to the C library and count
// allocations and frees, so a test can see that a tree uses its custom
// allocator and that the counts balance.
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

static ccol_memmgmt_procs _tracking_alloc = {
    .malloc = _ta_malloc,
    .free = _ta_free,
    .calloc = _ta_calloc,
    .realloc = _ta_realloc,
};

// A budget allocator: It lets g_alloc_remaining calls through and then fails
// every later one (-1 means no limit), for the sweeps of every failure point.
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

// A single-fault allocator: It fails only the call whose 1-based number is
// g_single_fault_at and counts calls in g_single_fault_counter. A test needs it
// when something built after the failure (an error message) must still
// succeed.
static int g_single_fault_at = -1;
static int g_single_fault_counter = 0;

static void *single_fault_malloc(size_t sz) {
  g_single_fault_counter++;
  if (g_single_fault_counter == g_single_fault_at) return NULL;
  return malloc(sz);
}
static void *single_fault_calloc(size_t n, size_t sz) {
  g_single_fault_counter++;
  if (g_single_fault_counter == g_single_fault_at) return NULL;
  return calloc(n, sz);
}
static void *single_fault_realloc(void *p, size_t sz) {
  g_single_fault_counter++;
  if (g_single_fault_counter == g_single_fault_at) return NULL;
  return realloc(p, sz);
}
static ccol_memmgmt_procs g_single_fault_mp = {.malloc = single_fault_malloc,
                                               .calloc = single_fault_calloc,
                                               .realloc = single_fault_realloc,
                                               .free = free};

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

  REQUIRE_GT(_ta_frees, (size_t)0);
}

TEST(custom_alloc, string_node_uses_custom_strdup) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson n = cjson_create_string_mp("hello world", &_tracking_alloc);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_STREQ(cjson_str_val(n), "hello world");

  REQUIRE_GT(_ta_allocs, (size_t)1);

  size_t frees_before = _ta_frees;
  cjson_destroy(n);

  REQUIRE_GT(_ta_frees, frees_before);
}

TEST(custom_alloc, array_and_object_use_custom_alloc) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson arr = cjson_create_list_mp(&_tracking_alloc);
  REQUIRE_NE((void *)arr, NULL);
  REQUIRE_EQ(cjson_list_push(arr, cjson_create_int_mp(1, &_tracking_alloc)),
             ccol_success);
  REQUIRE_EQ(
      cjson_list_push(arr, cjson_create_string_mp("item", &_tracking_alloc)),
      ccol_success);
  REQUIRE_EQ(cjson_list_size(arr), (size_t)2);

  cjson obj = cjson_create_dictionary_mp(&_tracking_alloc);
  REQUIRE_NE((void *)obj, NULL);
  REQUIRE_EQ(cjson_dictionary_set(
                 obj, "key", cjson_create_bool_mp(false, &_tracking_alloc)),
             ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(obj), (size_t)1);

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

  cjson doc = cjson_create_dictionary_mp(&_tracking_alloc);
  cjson_dictionary_set(doc, "x", cjson_create_int_mp(7, &_tracking_alloc));
  cjson_dictionary_set(doc, "s",
                       cjson_create_string_mp("hi", &_tracking_alloc));

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

  cjson leaf = cjson_get(doc, "a.b");
  REQUIRE_NE((void *)leaf, NULL);
  REQUIRE_EQ(cjson_int_val(leaf), 0LL);
  REQUIRE_EQ(_ta_allocs, allocs_before);
  REQUIRE_EQ(_ta_frees, frees_before);

  char long_path[300];
  memset(long_path, 'k', sizeof(long_path) - 1);
  long_path[sizeof(long_path) - 1] = '\0';
  cjson absent = cjson_get(doc, long_path);
  REQUIRE_EQ((void *)absent, NULL);
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
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson n = cjson_create_string("test");
  REQUIRE_NE((void *)n, NULL);

  REQUIRE_EQ(_ta_allocs, (size_t)0);
  REQUIRE_EQ(_ta_frees, (size_t)0);

  cjson_destroy(n);
}

TEST(custom_alloc, custom_alloc_frees_on_destroy) {
  _ta_allocs = 0;
  _ta_frees = 0;

  cjson root = cjson_parse_mp("{\"a\":1,\"b\":\"hello\",\"c\":[1,2,3]}", NULL,
                              &_tracking_alloc);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_GT(_ta_allocs, (size_t)0);

  size_t allocs_at_destroy = _ta_allocs;
  (void)allocs_at_destroy;

  cjson_destroy(root);

  REQUIRE_EQ(_ta_allocs, _ta_frees);
}

// The parse error message lives in library storage, so it costs the caller's
// allocator nothing and the caller never frees it. A message built through mp
// would show up as an allocation with no matching free.
TEST(custom_alloc, parse_failure_error_string_is_not_an_allocation) {
  _ta_allocs = 0;
  _ta_frees = 0;

  char *err = NULL;
  cjson n = cjson_parse_mp("not valid json", &err, &_tracking_alloc);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);

  REQUIRE_EQ(_ta_allocs, (size_t)0);
  REQUIRE_EQ(_ta_frees, (size_t)0);

  _ta_allocs = 0;
  _ta_frees = 0;
  char *err_deep = NULL;
  cjson deep = cjson_parse_mp("[1,2,{\"k\":3},", &err_deep, &_tracking_alloc);
  REQUIRE_EQ((void *)deep, NULL);
  REQUIRE_NE((void *)err_deep, NULL);
  REQUIRE_GT(_ta_allocs, (size_t)0);
  REQUIRE_EQ(_ta_allocs, _ta_frees);

  REQUIRE_TRUE(strlen(err) > 0);

  char *err2 = NULL;
  cjson n2 = cjson_parse_mp("[1,", &err2, NULL);
  REQUIRE_EQ((void *)n2, NULL);
  REQUIRE_NE((void *)err2, NULL);
  REQUIRE_EQ((void *)err2, (void *)err);
}

TEST(clone, dictionary_iterator_oom_never_returns_incomplete_clone) {
  cjson src = cjson_parse_mp("{\"a\":1,\"b\":2,\"c\":3}", NULL, &g_counting_mp);
  REQUIRE_NE((void *)src, NULL);
  size_t src_size = cjson_dictionary_size(src);
  REQUIRE_EQ(src_size, (size_t)3);

  for (int budget = 0; budget < 200; budget++) {
    g_alloc_remaining = budget;
    cjson copy = cjson_clone(src);
    g_alloc_remaining = -1;
    if (copy) {
      REQUIRE_EQ(cjson_dictionary_size(copy), src_size);
      cjson_destroy(copy);
    }
  }

  g_alloc_remaining = -1;
  REQUIRE_EQ(cjson_dictionary_size(src), src_size);
  cjson_destroy(src);
}

TEST(serialize, dictionary_iterator_oom_never_returns_truncated_string) {
  cjson src = cjson_parse_mp("{\"a\":1,\"b\":2,\"c\":3}", NULL, &g_counting_mp);
  REQUIRE_NE((void *)src, NULL);
  size_t src_size = cjson_dictionary_size(src);
  REQUIRE_EQ(src_size, (size_t)3);

  for (int budget = 0; budget < 200; budget++) {
    g_alloc_remaining = budget;
    char *out = cjson_serialize(src);
    g_alloc_remaining = -1;
    if (out) {
      cjson reparsed = cjson_parse(out, NULL);
      REQUIRE_NE((void *)reparsed, NULL);
      REQUIRE_EQ(cjson_dictionary_size(reparsed), src_size);
      cjson_destroy(reparsed);
      cjson_serialize_free_mp(out, &g_counting_mp);
    }
  }

  g_alloc_remaining = -1;
  cjson_destroy(src);
}

// The teardown reaches every child without allocating, so it finishes while
// every allocation fails. Only memtest (valgrind) can see a leak here.
TEST(oom, dictionary_destroy_never_leaks_under_sustained_allocation_failure) {
  const char *docs[] = {
      "{\"a\":1,\"b\":{\"x\":[1,2,3]},\"c\":3}",
      "[{\"a\":1},{\"b\":2},{\"c\":3}]",
      "{\"a\":{\"b\":{\"c\":{\"d\":1}}}}",
  };
  for (size_t d = 0; d < sizeof(docs) / sizeof(docs[0]); d++) {
    for (int budget = 0; budget < 250; budget++) {
      g_alloc_remaining = budget;
      cjson doc = cjson_parse_mp(docs[d], NULL, &g_counting_mp);
      g_alloc_remaining = -1;
      if (doc) cjson_destroy(doc);
    }
  }
}

// Failing only the first allocation (the node of "null") must give an honest
// out-of-memory message. The single-fault allocator is needed because a budget
// allocator would also fail the allocation of the message.
TEST(oom, unreported_allocation_failure_reports_out_of_memory_not_unknown) {
  g_single_fault_counter = 0;
  g_single_fault_at = 1;
  char *err = NULL;
  cjson n = cjson_parse_mp("null", &err, &g_single_fault_mp);
  g_single_fault_at = -1;
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strstr(err, "out of memory") != NULL);
  REQUIRE_TRUE(strstr(err, "unknown parse error") == NULL);
}

TEST(delete, list_remove_middle) {
  char *err = NULL;
  cjson root = cjson_parse("[10, 20, 30, 40]", &err);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_list_size(root), (size_t)4);

  REQUIRE_EQ(cjson_list_remove(root, 1), ccol_success);

  REQUIRE_EQ(cjson_list_size(root), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 30LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 2)), 40LL);
  cjson_destroy(root);
}

TEST(delete, list_remove_first) {
  char *err = NULL;
  cjson root = cjson_parse("[1, 2, 3]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 0), ccol_success);

  REQUIRE_EQ(cjson_list_size(root), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 2LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 3LL);
  cjson_destroy(root);
}

TEST(delete, list_remove_last) {
  char *err = NULL;
  cjson root = cjson_parse("[1, 2, 3]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 2), ccol_success);

  REQUIRE_EQ(cjson_list_size(root), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 1LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 2LL);
  cjson_destroy(root);
}

TEST(delete, list_remove_only_element) {
  char *err = NULL;
  cjson root = cjson_parse("[42]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 0), ccol_success);
  REQUIRE_EQ(cjson_list_size(root), (size_t)0);
  cjson_destroy(root);
}

TEST(delete, list_remove_out_of_bounds) {
  char *err = NULL;
  cjson root = cjson_parse("[1, 2]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 2), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(root), (size_t)2);
  cjson_destroy(root);
}

TEST(delete, list_remove_subtree_freed) {
  char *err = NULL;
  cjson root = cjson_parse("[{\"a\":1,\"b\":[10,20]}, 99]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 0), ccol_success);
  REQUIRE_EQ(cjson_list_size(root), (size_t)1);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 99LL);
  cjson_destroy(root);
}

TEST(delete, dictionary_remove_existing_key) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":1,\"b\":2,\"c\":3}", &err);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)3);

  REQUIRE_EQ(cjson_dictionary_remove(root, "b"), ccol_success);

  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)2);
  REQUIRE_EQ((void *)cjson_dictionary_get(root, "b"), NULL);
  REQUIRE_NE((void *)cjson_dictionary_get(root, "a"), NULL);
  REQUIRE_NE((void *)cjson_dictionary_get(root, "c"), NULL);
  cjson_destroy(root);
}

TEST(delete, dictionary_remove_missing_key) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":1}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_dictionary_remove(root, "z"), ccol_key_not_found);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  cjson_destroy(root);
}

TEST(delete, dictionary_remove_subtree_freed) {
  char *err = NULL;
  cjson root = cjson_parse("{\"keep\":1,\"drop\":{\"x\":[1,2,3]}}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_dictionary_remove(root, "drop"), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  REQUIRE_NE((void *)cjson_dictionary_get(root, "keep"), NULL);
  cjson_destroy(root);
}

TEST(delete, path_delete_dict_key) {
  char *err = NULL;
  cjson root = cjson_parse("{\"x\":1,\"y\":2}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_remove(root, "x"), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  REQUIRE_EQ((void *)cjson_get(root, "x"), NULL);
  REQUIRE_NE((void *)cjson_get(root, "y"), NULL);
  cjson_destroy(root);
}

TEST(delete, path_delete_nested_key) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":{\"b\":1,\"c\":2}}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_remove(root, "a.b"), ccol_success);
  REQUIRE_EQ((void *)cjson_get(root, "a.b"), NULL);
  REQUIRE_NE((void *)cjson_get(root, "a.c"), NULL);
  cjson_destroy(root);
}

TEST(delete, path_delete_list_element) {
  char *err = NULL;
  cjson root = cjson_parse("{\"items\":[10,20,30]}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_remove(root, "items.#1"), ccol_success);

  cjson items = cjson_get(root, "items");
  REQUIRE_EQ(cjson_list_size(items), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(items, 0)), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(items, 1)), 30LL);
  cjson_destroy(root);
}

TEST(delete, path_missing_parent_returns_key_not_found) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":1}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_remove(root, "x.y"), ccol_key_not_found);
  cjson_destroy(root);
}

TEST(delete, path_missing_leaf_returns_key_not_found) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_remove(root, "a.z"), ccol_key_not_found);
  cjson_destroy(root);
}

// Through a path an absent index is ccol_key_not_found, while a direct
// cjson_list_remove() call reports ccol_invalid_args for it on purpose.
TEST(delete, path_delete_list_out_of_bounds_returns_key_not_found) {
  char *err = NULL;
  cjson root = cjson_parse("{\"items\":[1,2,3]}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_remove(root, "items.#3"), ccol_key_not_found);
  REQUIRE_EQ(cjson_remove(root, "items.#99"), ccol_key_not_found);
  REQUIRE_EQ(cjson_list_size(cjson_get(root, "items")), (size_t)3);

  REQUIRE_EQ(cjson_list_remove(cjson_get(root, "items"), 3), ccol_invalid_args);

  cjson_destroy(root);
}

TEST(delete, path_delete_list_root_out_of_bounds_returns_key_not_found) {
  cjson root = cjson_parse("[10, 20, 30]", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_remove(root, "#3"), ccol_key_not_found);
  REQUIRE_EQ(cjson_list_size(root), (size_t)3);
  cjson_destroy(root);
}

TEST(delete, path_delete_with_escaped_dot_in_key) {
  cjson root = cjson_create_dictionary();
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_dictionary_set(root, "a.b", cjson_create_int(7)),
             ccol_success);
  REQUIRE_EQ(cjson_dictionary_set(root, "keep", cjson_create_int(1)),
             ccol_success);

  REQUIRE_EQ(cjson_remove(root, "a\\.b"), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  REQUIRE_NE((void *)cjson_dictionary_get(root, "keep"), NULL);
  cjson_destroy(root);
}

TEST(delete, null_root_returns_invalid_args) {
  REQUIRE_EQ(cjson__remove(NULL, "x"), ccol_invalid_args);
}

TEST(delete, null_path_returns_invalid_args) {
  cjson root = cjson_parse("{\"x\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson__remove(root, NULL), ccol_invalid_args);
  cjson_destroy(root);
}

TEST(delete, empty_path_returns_invalid_args) {
  cjson root = cjson_parse("{\"x\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson__remove(root, ""), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "x"), NULL);
  cjson_destroy(root);
}

TEST(delete, trailing_dot_returns_invalid_args) {
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson__remove(root, "a."), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a.b"), NULL);
  cjson_destroy(root);
}

TEST(delete, consecutive_dots_rejected) {
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson__remove(root, "a..b"), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a.b"), NULL);
  cjson_destroy(root);
}

TEST(delete, trailing_dot_with_nonexistent_parent_still_invalid_args) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson__remove(root, "missing."), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(delete,
     consecutive_dots_with_nonexistent_first_component_still_invalid_args) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson__remove(root, "missing..b"), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(delete, leading_dot_rejected) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson__remove(root, ".a"), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(delete, list_root_delete_element) {
  cjson root = cjson_parse("[10, 20, 30]", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_remove(root, "#1"), ccol_success);
  REQUIRE_EQ(cjson_list_size(root), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 30LL);
  cjson_destroy(root);
}

TEST(delete, list_remove_null_arr) {
  REQUIRE_EQ(cjson_list_remove(NULL, 0), ccol_invalid_args);
}

TEST(delete, list_remove_wrong_type) {
  cjson not_list = cjson_create_int(5);
  REQUIRE_EQ(cjson_list_remove(not_list, 0), ccol_invalid_args);
  cjson_destroy(not_list);
}

TEST(delete, dictionary_remove_null_obj) {
  REQUIRE_EQ(cjson_dictionary_remove(NULL, "k"), ccol_invalid_args);
}

TEST(delete, dictionary_remove_null_key) {
  cjson dict = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_remove(dict, NULL), ccol_invalid_args);
  cjson_destroy(dict);
}

TEST(delete, dictionary_remove_wrong_type) {
  cjson not_dict = cjson_create_int(5);
  REQUIRE_EQ(cjson_dictionary_remove(not_dict, "k"), ccol_invalid_args);
  cjson_destroy(not_dict);
}

TEST(navigate, set_nonfinite_new_key_returns_invalid_args) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "v", INFINITY), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  REQUIRE_EQ(cjson_set(root, "v", NAN), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_typed_invalid_integer_size_new_key) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  long long v = 42;
  ccol_retval r = cjson__set_typed(root, "k", CJSON_INTEGER, &v, 3, true);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "k"), NULL);
  cjson_destroy(root);
}

// Only a direct back-end call can pass a wrong bool size. Reading the bytes at
// the wrong width could store a trap representation in a _Bool.
TEST(navigate, set_typed_invalid_bool_size_new_key) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  int not_a_bool = 4;
  ccol_retval r = cjson__set_typed(root, "k", CJSON_BOOL, &not_a_bool,
                                   sizeof(not_a_bool), false);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "k"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_typed_invalid_bool_size_existing_key_preserves_value) {
  cjson root = cjson_parse("{\"v\":123}", NULL);
  REQUIRE_NE((void *)root, NULL);
  int not_a_bool = 4;
  ccol_retval r = cjson__set_typed(root, "v", CJSON_BOOL, &not_a_bool,
                                   sizeof(not_a_bool), false);
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson v = cjson_get(root, "v");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cjson_type(v), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(v), 123LL);
  cjson_destroy(root);
}

// The list branch reaches the same check through a different call site (#N and
// cvector_at()), so it needs its own case.
TEST(navigate,
     set_typed_invalid_bool_size_existing_list_element_preserves_value) {
  cjson root = cjson_parse("[123]", NULL);
  REQUIRE_NE((void *)root, NULL);
  int not_a_bool = 4;
  ccol_retval r = cjson__set_typed(root, "#0", CJSON_BOOL, &not_a_bool,
                                   sizeof(not_a_bool), false);
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson v = cjson_get(root, "#0");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cjson_type(v), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(v), 123LL);
  cjson_destroy(root);
}

TEST(navigate, set_unsupported_type_new_key_rejected) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  long double ld = 3.14L;
  REQUIRE_EQ(cjson_set(root, "v", ld), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_unsupported_type_existing_key_preserves_value) {
  cjson root = cjson_parse("{\"v\":123.456}", NULL);
  REQUIRE_NE((void *)root, NULL);
  long double ld = 3.14L;
  REQUIRE_EQ(cjson_set(root, "v", ld), ccol_invalid_args);
  cjson v = cjson_get(root, "v");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cjson_type(v), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(v), 123.456);
  cjson_destroy(root);
}

TEST(navigate, set_typed_unsupported_type_direct_call_rejected) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  long double ld = 1.0L;
  ccol_retval r = cjson__set_typed(root, "k", CJSON__TYPE_UNSUPPORTED, &ld,
                                   sizeof(ld), false);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "k"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_nonnull_void_ptr_new_key_rejected) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  int dummy = 5;
  void *p = &dummy;
  REQUIRE_EQ(cjson_set(root, "v", p), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_nonnull_void_ptr_existing_key_preserves_value) {
  cjson root = cjson_parse("{\"v\":123}", NULL);
  REQUIRE_NE((void *)root, NULL);
  int dummy = 5;
  void *p = &dummy;
  REQUIRE_EQ(cjson_set(root, "v", p), ccol_invalid_args);
  cjson v = cjson_get(root, "v");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cjson_type(v), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(v), 123LL);
  cjson_destroy(root);
}

// The guard against a non-NULL void * must not refuse a void * that holds NULL,
// which is documented usage.
TEST(navigate, set_null_void_ptr_still_accepted) {
  cjson root = cjson_parse("{\"v\":123}", NULL);
  REQUIRE_NE((void *)root, NULL);
  void *p = NULL;
  REQUIRE_EQ(cjson_set(root, "v", p), ccol_success);
  REQUIRE_EQ(cjson_type(cjson_get(root, "v")), CJSON_NULL);
  cjson_destroy(root);
}

TEST(parse, null_input_returns_null) {
  REQUIRE_EQ((void *)cjson_parse(NULL, NULL), NULL);
}

TEST(parse, null_input_with_error_str) {
  char *err = NULL;
  cjson n = cjson_parse_mp(NULL, &err, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);
}

TEST(navigate, set_new_null_key) {
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "newkey", NULL), ccol_success);
  cjson node = cjson_get(root, "newkey");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_type(node), CJSON_NULL);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  cjson_destroy(root);
}

TEST(navigate, set_trailing_dot_returns_error) {
  cjson root = cjson_parse("{\"a\":{\"b\":0}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval r = cjson_set(root, "a.", 99);
  REQUIRE_NE(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a.b")), 0LL);
  cjson_destroy(root);
}

TEST(serialize, pretty_print_list) {
  cjson a = cjson_create_list();
  cjson_list_push(a, cjson_create_int(1));
  cjson_list_push(a, cjson_create_int(2));
  char *s = cjson_serialize_pretty(a, 2);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_TRUE(strchr(s, '\n') != NULL);
  REQUIRE_TRUE(strstr(s, "1") != NULL);
  REQUIRE_TRUE(strstr(s, "2") != NULL);
  cjson back = cjson_parse(s, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cjson_type(back), CJSON_LIST);
  REQUIRE_EQ(cjson_list_size(back), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(back, 0)), 1LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(back, 1)), 2LL);
  cjson_serialize_free(s);
  cjson_destroy(a);
  cjson_destroy(back);
}

TEST(clone, null_type_node) {
  cjson orig = cjson_create_null();
  REQUIRE_NE((void *)orig, NULL);
  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, NULL);
  REQUIRE_NE((void *)copy, (void *)orig);
  REQUIRE_EQ(cjson_type(copy), CJSON_NULL);
  cjson_destroy(orig);
  cjson_destroy(copy);
}

TEST(construction, list_get_wrong_type_returns_null) {
  cjson not_list = cjson_create_int(42);
  REQUIRE_EQ((void *)cjson_list_get(not_list, 0), NULL);
  cjson_destroy(not_list);
}

TEST(construction, dict_get_wrong_type_returns_null) {
  cjson not_dict = cjson_create_string("oops");
  REQUIRE_EQ((void *)cjson_dictionary_get(not_dict, "k"), NULL);
  cjson_destroy(not_dict);
}

TEST(navigate, set_on_scalar_root_returns_invalid_args) {
  cjson root = cjson_create_int(42);
  ccol_retval r = cjson_set(root, "key", 5);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_type(root), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(root), 42LL);
  cjson_destroy(root);
}

TEST(edge, llong_max_serialize_roundtrip) {
  cjson n = cjson_create_int(LLONG_MAX);
  REQUIRE_NE((void *)n, NULL);
  char *s = cjson_serialize(n);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(s, "9223372036854775807");
  cjson back = cjson_parse(s, NULL);
  REQUIRE_NE((void *)back, NULL);
  REQUIRE_EQ(cjson_type(back), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(back), LLONG_MAX);
  cjson_serialize_free(s);
  cjson_destroy(n);
  cjson_destroy(back);
}

TEST(delete, path_delete_non_hash_leaf_on_list_returns_invalid_args) {
  cjson root = cjson_parse("[1, 2, 3]", NULL);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval r = cjson_remove(root, "key");
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_list_size(root), (size_t)3);
  cjson_destroy(root);
}

TEST(delete, path_delete_on_scalar_root_returns_invalid_args) {
  cjson root = cjson_create_int(42);
  ccol_retval r = cjson__remove(root, "key");
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_type(root), CJSON_INTEGER);
  cjson_destroy(root);
}

// Builds n '[' around a scalar 1 and n ']' in a malloc'd buffer that the caller
// frees, for the depth tests.
static char *build_nested_array(size_t n) {
  char *buf = malloc(2 * n + 2);
  size_t pos = 0;
  for (size_t i = 0; i < n; i++) buf[pos++] = '[';
  buf[pos++] = '1';
  for (size_t i = 0; i < n; i++) buf[pos++] = ']';
  buf[pos] = '\0';
  return buf;
}

// Builds n levels of {"a": ...} around a scalar 1 in a malloc'd buffer that the
// caller frees, for the depth tests.
static char *build_nested_object(size_t n) {
  char *buf = malloc(6 * n + 2);
  size_t pos = 0;
  for (size_t i = 0; i < n; i++) {
    buf[pos++] = '{';
    buf[pos++] = '"';
    buf[pos++] = 'a';
    buf[pos++] = '"';
    buf[pos++] = ':';
  }
  buf[pos++] = '1';
  for (size_t i = 0; i < n; i++) buf[pos++] = '}';
  buf[pos] = '\0';
  return buf;
}

// A document far past the depth cap must fail quickly with a parse error, not
// crash or hang; the wall-clock bound checks the "quickly".
TEST(parse, deeply_nested_array_rejected_not_crashed) {
  char *json = build_nested_array(5000);
  clock_t t0 = clock();
  char *err = NULL;
  cjson n = cjson_parse_mp(json, &err, NULL);
  double elapsed_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
  free(json);

  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strstr(err, "nesting depth") != NULL);
  REQUIRE_LT(elapsed_ms, 1000.0);
}

TEST(parse, deeply_nested_object_rejected_not_crashed) {
  char *json = build_nested_object(5000);
  clock_t t0 = clock();
  char *err = NULL;
  cjson n = cjson_parse_mp(json, &err, NULL);
  double elapsed_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
  free(json);

  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strstr(err, "nesting depth") != NULL);
  REQUIRE_LT(elapsed_ms, 1000.0);
}

TEST(parse, nesting_at_max_depth_still_parses) {
  char *json = build_nested_array(500);
  cjson n = cjson_parse(json, NULL);
  free(json);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_LIST);
  cjson_destroy(n);
}

TEST(parse, nesting_one_past_max_depth_rejected) {
  char *json = build_nested_array(501);
  char *err = NULL;
  cjson n = cjson_parse_mp(json, &err, NULL);
  free(json);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strstr(err, "nesting depth") != NULL);
}

// Builds a list nested n levels deep through cjson_list_push(), so the parser's
// depth cap plays no part, and gives the outermost node; the clone, serialize
// and destroy depth tests use it.
static cjson build_nested_list_via_api(size_t n) {
  cjson cur = cjson_create_list();
  cjson_list_push(cur, cjson_create_int(1));
  for (size_t i = 0; i < n; i++) {
    cjson outer = cjson_create_list();
    cjson_list_push(outer, cur);
    cur = outer;
  }
  return cur;
}

TEST(clone, deeply_nested_api_built_tree_rejected_not_crashed) {
  cjson deep = build_nested_list_via_api(600);
  REQUIRE_NE((void *)deep, NULL);
  cjson copy = cjson_clone(deep);
  REQUIRE_EQ((void *)copy, NULL);
  cjson_destroy(deep);
}

TEST(serialize, deeply_nested_api_built_tree_rejected_not_crashed) {
  cjson deep = build_nested_list_via_api(600);
  REQUIRE_NE((void *)deep, NULL);
  char *s = cjson_serialize(deep);
  REQUIRE_EQ((void *)s, NULL);
  cjson_destroy(deep);
}

TEST(clone, nesting_at_clone_max_depth_still_clones) {
  cjson deep = build_nested_list_via_api(499);
  REQUIRE_NE((void *)deep, NULL);
  cjson copy = cjson_clone(deep);
  REQUIRE_NE((void *)copy, NULL);
  cjson_destroy(deep);
  cjson_destroy(copy);
}

TEST(destroy, deeply_nested_api_built_tree_destroyed_without_crashing) {
  cjson deep = build_nested_list_via_api(20000);
  REQUIRE_NE((void *)deep, NULL);
  cjson_destroy(deep);
  REQUIRE_EQ((void *)deep, NULL);
}

// Wrapping a built subtree in a new unattached container must not search that
// subtree, or building a tree bottom up costs O(n^2). The test counts the nodes
// the search visits (about n^2/2 without the short-circuit), and first shows
// that a push into an attached container does walk the child.
TEST(ownership,
     list_push_into_unattached_container_stays_cheap_even_for_deep_child) {
  cjson root = cjson_create_list();
  cjson target = cjson_create_list();
  cjson child = cjson_create_list();
  bool created = root && target && child;
  ccol_retval r_target = ccol_unexpected_failure;
  ccol_retval r_child = ccol_unexpected_failure;
  bool members_ok = true;
  unsigned long before = 0, walked = 0;
  if (created) {
    r_target = cjson_list_push(root, target);
    for (int i = 0; i < 3; i++)
      if (cjson_list_push(child, cjson_create_int(i)) != ccol_success)
        members_ok = false;
    before = atomic_load(&cjson__cycle_search_nodes_for_tests);
    if (r_target == ccol_success)
      r_child = cjson_list_push(target, child);
    else
      cjson_destroy(child);
    walked = atomic_load(&cjson__cycle_search_nodes_for_tests) - before;
  } else {
    cjson_destroy(target);
    cjson_destroy(child);
  }
  cjson_destroy(root);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(r_target, ccol_success);
  REQUIRE_TRUE(members_ok);
  REQUIRE_EQ(r_child, ccol_success);
  REQUIRE_EQ(walked, 4UL);

  const size_t n = 20000;
  before = atomic_load(&cjson__cycle_search_nodes_for_tests);
  cjson deep = build_nested_list_via_api(n);
  walked = atomic_load(&cjson__cycle_search_nodes_for_tests) - before;
  REQUIRE_NE((void *)deep, NULL);
  cjson_destroy(deep);
  REQUIRE_EQ(walked, 0UL);
}

// A valid number can be longer than the 360-byte stack token buffer (here
// through redundant zeros); it must parse through the heap fallback.
TEST(fuzzy, number_literal_past_stack_buffer_still_parses) {
  char buf[500];
  size_t pos = 0;
  buf[pos++] = '0';
  buf[pos++] = '.';
  for (int i = 0; i < 400; i++) buf[pos++] = '0';
  buf[pos++] = '1';
  buf[pos] = '\0';
  REQUIRE_GT(strlen(buf), (size_t)360);

  cjson n = cjson_parse(buf, NULL);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(n), 0.0);
  cjson_destroy(n);
}

TEST(fuzzy, long_integer_literal_past_stack_buffer_reports_out_of_range) {
  char buf[500];
  size_t pos = 0;
  buf[pos++] = '1';
  for (int i = 0; i < 400; i++) buf[pos++] = '0';
  buf[pos] = '\0';
  REQUIRE_GT(strlen(buf), (size_t)360);

  char *err = NULL;
  cjson n = cjson_parse_mp(buf, &err, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);
}

TEST(fuzzy, malformed_number_literal_past_stack_buffer_still_rejected) {
  char buf[500];
  size_t pos = 0;
  buf[pos++] = '0';
  buf[pos++] = '.';
  for (int i = 0; i < 200; i++) buf[pos++] = '0';
  buf[pos++] = '.';
  for (int i = 0; i < 200; i++) buf[pos++] = '0';
  buf[pos] = '\0';

  REQUIRE_EQ((void *)cjson_parse(buf, NULL), NULL);
}

TEST(custom_alloc, long_number_literal_uses_custom_alloc_and_frees) {
  _ta_allocs = 0;
  _ta_frees = 0;

  char buf[500];
  size_t pos = 0;
  buf[pos++] = '0';
  buf[pos++] = '.';
  for (int i = 0; i < 400; i++) buf[pos++] = '0';
  buf[pos++] = '1';
  buf[pos] = '\0';

  cjson n = cjson_parse_mp(buf, NULL, &_tracking_alloc);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_FLOAT);
  REQUIRE_GT(_ta_frees, (size_t)0);
  cjson_destroy(n);
}

// NODE_POOL_CAP of cjson.c, stated again here because no header holds it.
#define CJSON_TEST_POOL_CAP 512

// Earlier tests leave anything from 0 to the cap in this thread's pool; after
// destroying more than the cap the pool holds exactly the cap.
TEST(node_pool, cap_eviction_keeps_pool_bounded) {
  cjson list = cjson_create_list();
  REQUIRE_NE((void *)list, NULL);
  size_t n = CJSON_TEST_POOL_CAP + 100;
  for (size_t i = 0; i < n; i++)
    REQUIRE_EQ(cjson_list_push(list, cjson_create_null()), ccol_success);
  cjson_destroy(list);

  REQUIRE_EQ(cjson_debug_pool_size(), (size_t)CJSON_TEST_POOL_CAP);
}

// What pool_populate_thread() saw: Its pool size before and after.
typedef struct {
  size_t start_pool_size;
  size_t end_pool_size;
} pool_populate_result_t;

// Allocates 50 nodes and then frees them all on a new thread, recording the
// thread's pool size before and after in the pool_populate_result_t at arg.
// Allocating all of them first keeps the pool from recycling one node.
static void *pool_populate_thread(void *arg) {
  pool_populate_result_t *r = (pool_populate_result_t *)arg;
  r->start_pool_size = cjson_debug_pool_size();
  cjson nodes[50];
  for (int i = 0; i < 50; i++) nodes[i] = cjson_create_null();
  for (int i = 0; i < 50; i++) cjson_destroy(nodes[i]);
  r->end_pool_size = cjson_debug_pool_size();
  return NULL;
}

// The worker allocates all 50 nodes before it frees any, so each is a fresh
// calloc and the pool grows by 50. It exits with them pooled, which drives the
// thread-exit drain; memtest reports a broken drain as a leak.
TEST(node_pool, fresh_thread_starts_with_an_empty_pool) {
  pool_populate_result_t result = {(size_t)-1, (size_t)-1};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, pool_populate_thread, &result), 0);
  pthread_join(tid, NULL);

  REQUIRE_EQ(result.start_pool_size, (size_t)0);
  REQUIRE_EQ(result.end_pool_size, (size_t)50);
}

// The nodes cross_thread_free_thread() frees and the pool sizes it saw.
typedef struct {
  cjson *nodes;
  size_t count;
  size_t start_pool_size;
  size_t end_pool_size;
} cross_thread_free_arg_t;

// Destroys nodes that the main thread allocated, recording this thread's pool
// size before and after, to show that a node joins the pool of the thread that
// frees it.
static void *cross_thread_free_thread(void *arg) {
  cross_thread_free_arg_t *a = (cross_thread_free_arg_t *)arg;
  a->start_pool_size = cjson_debug_pool_size();
  for (size_t i = 0; i < a->count; i++) cjson_destroy(a->nodes[i]);
  a->end_pool_size = cjson_debug_pool_size();
  return NULL;
}

TEST(node_pool, node_freed_on_a_different_thread_joins_that_threads_own_pool) {
  size_t count = 20;
  cjson *nodes = malloc(count * sizeof(cjson));
  REQUIRE_NE((void *)nodes, NULL);
  for (size_t i = 0; i < count; i++) {
    nodes[i] = cjson_create_int((long long)i);
    REQUIRE_NE((void *)nodes[i], NULL);
  }

  size_t main_pool_before = cjson_debug_pool_size();

  cross_thread_free_arg_t arg = {nodes, count, (size_t)-1, (size_t)-1};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cross_thread_free_thread, &arg), 0);
  pthread_join(tid, NULL);
  free(nodes);

  size_t main_pool_after = cjson_debug_pool_size();

  REQUIRE_EQ(arg.start_pool_size, (size_t)0);
  REQUIRE_EQ(arg.end_pool_size, count);
  REQUIRE_EQ(main_pool_after, main_pool_before);
}

TEST(cjson_type_str, every_node_type_maps_to_its_own_spelling) {
  cjson nodes[7];
  nodes[0] = cjson_create_null();
  nodes[1] = cjson_create_bool(true);
  nodes[2] = cjson_create_int(42);
  nodes[3] = cjson_create_double(1.5);
  nodes[4] = cjson_create_string("s");
  nodes[5] = cjson_create_list();
  nodes[6] = cjson_create_dictionary();

  static const char *expected[7] = {
      "CJSON_NULL",   "CJSON_BOOL", "CJSON_INTEGER",   "CJSON_FLOAT",
      "CJSON_STRING", "CJSON_LIST", "CJSON_DICTIONARY"};

  bool all_created = true, all_named = true;
  for (volatile size_t i = 0; i < 7; i++) {
    if (!nodes[i]) {
      all_created = false;
      continue;
    }
    if (strcmp(cjson_type_str(nodes[i]), expected[i]) != 0) all_named = false;
  }

  for (size_t i = 0; i < 7; i++)
    if (nodes[i]) cjson__destroy(nodes[i]);

  REQUIRE_TRUE(all_created);
  REQUIRE_TRUE(all_named);
}

TEST(cjson_type_str, a_null_handle_reports_the_null_type) {
  REQUIRE_STREQ(cjson_type_str(NULL), "CJSON_NULL");
}

// ccol_invalid_args must always mean the child is untouched. Each test reads
// the child back, attaches it to a real container and lets that destroy it; if
// the rejection had destroyed it, that's a use-after-free and a double free.
TEST(ownership, list_push_into_a_null_container_leaves_the_child_reusable) {
  cjson child = cjson_create_int(42);
  ccol_retval r = cjson_list_push(NULL, child);

  cjson_node_type t = cjson_type(child);
  long long v = (t == CJSON_INTEGER) ? cjson_int_val(child) : -1;
  cjson arr = cjson_create_list();
  ccol_retval r2 = cjson_list_push(arr, child);
  size_t n = cjson_list_size(arr);
  cjson_destroy(arr);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
  REQUIRE_EQ(v, 42LL);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(n, (size_t)1);
}

TEST(ownership, list_push_into_a_non_list_leaves_the_child_reusable) {
  cjson scalar = cjson_create_int(7);
  cjson child = cjson_create_string("kept");
  ccol_retval r = cjson_list_push(scalar, child);

  cjson_node_type t = cjson_type(child);
  bool text_ok =
      (t == CJSON_STRING) && strcmp(cjson_str_val(child), "kept") == 0;
  cjson arr = cjson_create_list();
  ccol_retval r2 = cjson_list_push(arr, child);
  cjson_destroy(arr);
  cjson_destroy(scalar);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_TRUE(text_ok);
  REQUIRE_EQ(r2, ccol_success);
}

TEST(ownership,
     list_push_into_the_result_of_an_absent_get_leaves_the_child_reusable) {
  cjson doc = cjson_parse("{\"present\":1}", NULL);
  cjson child = cjson_create_int(7);
  ccol_retval r = cjson_list_push(cjson_get(doc, "maybe_absent"), child);

  cjson_node_type t = cjson_type(child);
  cjson arr = cjson_create_list();
  ccol_retval r2 = cjson_list_push(arr, child);
  cjson_destroy(arr);
  cjson_destroy(doc);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
  REQUIRE_EQ(r2, ccol_success);
}

TEST(ownership,
     dictionary_set_into_a_null_container_leaves_the_child_reusable) {
  cjson child = cjson_create_int(11);
  ccol_retval r = cjson_dictionary_set(NULL, "k", child);

  cjson_node_type t = cjson_type(child);
  cjson obj = cjson_create_dictionary();
  ccol_retval r2 = cjson_dictionary_set(obj, "k", child);
  size_t n = cjson_dictionary_size(obj);
  cjson_destroy(obj);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(n, (size_t)1);
}

TEST(ownership,
     dictionary_set_into_a_non_dictionary_leaves_the_child_reusable) {
  cjson scalar = cjson_create_bool(true);
  cjson child = cjson_create_int(12);
  ccol_retval r = cjson_dictionary_set(scalar, "k", child);

  cjson_node_type t = cjson_type(child);
  cjson obj = cjson_create_dictionary();
  ccol_retval r2 = cjson_dictionary_set(obj, "k", child);
  cjson_destroy(obj);
  cjson_destroy(scalar);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
  REQUIRE_EQ(r2, ccol_success);
}

TEST(ownership, dictionary_set_with_a_null_key_leaves_the_child_reusable) {
  cjson obj = cjson_create_dictionary();
  cjson child = cjson_create_int(13);
  ccol_retval r = cjson_dictionary_set(obj, NULL, child);

  cjson_node_type t = cjson_type(child);
  ccol_retval r2 = cjson_dictionary_set(obj, "k", child);
  size_t n = cjson_dictionary_size(obj);
  cjson_destroy(obj);

  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(n, (size_t)1);
}

// cjson__set_typed() is a public back-end, so it checks raw_size for every type
// that reads a payload. A narrow string payload would read a pointer out of a
// smaller object and copy through garbage.
TEST(navigate, set_typed_invalid_string_size_new_key) {
  cjson root = cjson_create_dictionary();
  char tiny = 'x';
  ccol_retval r = cjson__set_typed(root, "k", CJSON_STRING, &tiny, 1, false);
  size_t n = cjson_dictionary_size(root);
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(n, (size_t)0);
}

TEST(navigate, set_typed_invalid_string_size_existing_key_preserves_value) {
  cjson root = cjson_parse("{\"k\":\"original\"}", NULL);
  char tiny = 'x';
  ccol_retval r = cjson__set_typed(root, "k", CJSON_STRING, &tiny, 1, false);
  const char *kept = cjson_str_val(cjson_get(root, "k"));
  bool unchanged = strcmp(kept, "original") == 0;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_TRUE(unchanged);
}

TEST(navigate, set_typed_null_payload_rejected_for_every_payload_type) {
  cjson root = cjson_create_dictionary();
  ccol_retval rs =
      cjson__set_typed(root, "k", CJSON_STRING, NULL, sizeof(char *), false);
  ccol_retval rb =
      cjson__set_typed(root, "k", CJSON_BOOL, NULL, sizeof(bool), false);
  ccol_retval ri =
      cjson__set_typed(root, "k", CJSON_INTEGER, NULL, sizeof(int), true);
  ccol_retval rf =
      cjson__set_typed(root, "k", CJSON_FLOAT, NULL, sizeof(double), false);
  ccol_retval rn = cjson__set_typed(root, "n", CJSON_NULL, NULL, 0, false);
  cjson_node_type nt = cjson_type(cjson_get(root, "n"));
  size_t count = cjson_dictionary_size(root);
  cjson_destroy(root);

  REQUIRE_EQ(rs, ccol_invalid_args);
  REQUIRE_EQ(rb, ccol_invalid_args);
  REQUIRE_EQ(ri, ccol_invalid_args);
  REQUIRE_EQ(rf, ccol_invalid_args);
  REQUIRE_EQ(rn, ccol_success);
  REQUIRE_EQ(nt, CJSON_NULL);
  REQUIRE_EQ(count, (size_t)1);
}

// Tells whether s[0..n) is well-formed UTF-8, by the plain Unicode table. The
// UTF-8 tests check the library's output with it; it's written out here because
// asking the library whether the library is right could never fail.
static bool _cjson_test_utf8_ok(const char *s, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < n) {
    unsigned char b = p[i];
    size_t need;
    unsigned char lo = 0x80, hi = 0xBF;
    if (b < 0x80) {
      i++;
      continue;
    } else if (b >= 0xC2 && b <= 0xDF) {
      need = 1;
    } else if (b == 0xE0) {
      need = 2, lo = 0xA0;
    } else if (b == 0xED) {
      need = 2, hi = 0x9F;
    } else if (b >= 0xE1 && b <= 0xEF) {
      need = 2;
    } else if (b == 0xF0) {
      need = 3, lo = 0x90;
    } else if (b == 0xF4) {
      need = 3, hi = 0x8F;
    } else if (b >= 0xF1 && b <= 0xF3) {
      need = 3;
    } else {
      return false;
    }
    if (i + need >= n) return false;
    if (p[i + 1] < lo || p[i + 1] > hi) return false;
    for (size_t k = 2; k <= need; k++)
      if (p[i + k] < 0x80 || p[i + k] > 0xBF) return false;
    i += need + 1;
  }
  return true;
}

// The same check for a NUL-terminated string; false for NULL.
static bool _cjson_test_utf8_cstr_ok(const char *s) {
  return s && _cjson_test_utf8_ok(s, strlen(s));
}

TEST(parse, raw_invalid_utf8_bytes_are_rejected) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "{\"k\":\"\xff\xfe\xc3(\"}",
      "invalid UTF-8 in string at position 6: byte that never appears in "
      "UTF-8 (0xff)"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"a\x80\"",
      "invalid UTF-8 in string at position 2: continuation byte with no lead "
      "byte (0x80)"));
}

TEST(parse, overlong_utf8_sequence_is_rejected) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\xc0\xaf\"",
      "invalid UTF-8 in string at position 1: overlong encoding (lead byte "
      "0xc0)"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\xe0\x80\xaf\"",
      "invalid UTF-8 in string at position 1: overlong encoding (bytes 0xe0 "
      "0x80)"));
}

TEST(parse, utf8_encoded_surrogate_is_rejected) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\xed\xa0\x80\"",
      "invalid UTF-8 in string at position 1: encoded surrogate code point "
      "(bytes 0xed 0xa0)"));
}

TEST(parse, utf8_above_the_unicode_range_is_rejected) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\xf4\x90\x80\x80\"",
      "invalid UTF-8 in string at position 1: code point above U+10FFFF "
      "(bytes 0xf4 0x90)"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\xf5\x80\x80\x80\"",
      "invalid UTF-8 in string at position 1: byte that never appears in "
      "UTF-8 (0xf5)"));
}

TEST(parse, truncated_utf8_sequence_is_rejected) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\xe2\x82\"",
      "invalid UTF-8 in string at position 1: truncated sequence (lead byte "
      "0xe2, then 0x22 where a continuation byte is expected)"));
}

TEST(parse, invalid_utf8_alongside_an_escape_is_rejected_too) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\\t\xff\xc3(\"",
      "invalid UTF-8 in string at position 3: byte that never appears in "
      "UTF-8 (0xff)"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"ab\\n\xe2\x82\\n\"",
      "invalid UTF-8 in string at position 5: truncated sequence (lead byte "
      "0xe2, then 0x5c where a continuation byte is expected)"));
}

TEST(parse, the_first_defect_of_a_string_is_the_one_reported) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\\q\xff\"", "unknown escape '\\q' at position 2"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "\"\xff\\q\"",
      "invalid UTF-8 in string at position 1: byte that never appears in "
      "UTF-8 (0xff)"));
}

TEST(parse, well_formed_multibyte_utf8_is_preserved_byte_for_byte) {
  static const char plain[] = "\"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\"";
  static const char escaped[] = "\"\\t\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\"";
  cjson a = cjson_parse(plain, NULL);
  cjson b = cjson_parse(escaped, NULL);
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_NE((void *)b, NULL);
  bool a_ok =
      strcmp(cjson_str_val(a), "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == 0;
  bool b_ok =
      strcmp(cjson_str_val(b), "\t\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == 0;
  cjson_destroy(a);
  cjson_destroy(b);
  REQUIRE_TRUE(a_ok);
  REQUIRE_TRUE(b_ok);
}

TEST(parse, an_invalid_utf8_dictionary_key_is_rejected_too) {
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "{\"\xffk\":1}",
      "invalid UTF-8 in string at position 2: byte that never appears in "
      "UTF-8 (0xff)"));
  REQUIRE_TRUE(CJSON_TEST_PARSE_FAILS_WITH(
      "{\"is_admin\xff\":true,\"is_admin\xfe\":false}",
      "invalid UTF-8 in string at position 10: byte that never appears in "
      "UTF-8 (0xff)"));
}

TEST(parse, every_invalid_utf8_error_message_is_printable_ascii) {
  bool all_printable = true;
  bool any_refused = false;
  for (int b = 0x80; b < 256; b++) {
    for (int lead = 0; lead < 3; lead++) {
      char doc[32];
      int n;
      if (lead == 0)
        n = snprintf(doc, sizeof(doc), "[\"%c\"]", b);
      else if (lead == 1)
        n = snprintf(doc, sizeof(doc), "[\"\xe2%c\"]", b);
      else
        n = snprintf(doc, sizeof(doc), "{\"\xf0%c\\t\":0}", b);
      char *err = NULL;
      cjson d = cjson_parse_n(doc, (size_t)n, &err);
      if (d) {
        cjson_destroy(d);
        continue;
      }
      any_refused = true;
      for (const char *q = err; q && *q; q++)
        if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7e)
          all_printable = false;
    }
  }
  REQUIRE_TRUE(any_refused);
  REQUIRE_TRUE(all_printable);
}

TEST(serialize, output_is_valid_utf8_for_every_accepted_input) {
  bool all_valid = true;
  bool any_parsed = false;
  for (int b = 1; b < 256; b++) {
    char doc[64];
    int variants[3];
    variants[0] = snprintf(doc, sizeof(doc), "{\"k\":\"%c\"}", b);
    for (int v = 0; v < 3; v++) {
      if (v == 1) snprintf(doc, sizeof(doc), "{\"k\":\"\xe2%c\"}", b);
      if (v == 2) snprintf(doc, sizeof(doc), "{\"\xf0%c\":\"\\tx%c\"}", b, b);
      (void)variants;
      cjson d = cjson_parse(doc, NULL);
      if (!d) continue;
      any_parsed = true;
      char *out = cjson_serialize(d);
      if (!_cjson_test_utf8_cstr_ok(out)) all_valid = false;
      cjson_serialize_free(out);
      cjson_destroy(d);
    }
  }
  REQUIRE_TRUE(any_parsed);
  REQUIRE_TRUE(all_valid);
}

TEST(construction, create_string_rejects_invalid_utf8) {
  cjson bad = cjson_create_string("bad\xffstring");
  REQUIRE_EQ((void *)bad, NULL);
}

TEST(construction, create_string_accepts_well_formed_multibyte_utf8) {
  cjson ok = cjson_create_string("caf\xc3\xa9 \xf0\x9f\x98\x80");
  REQUIRE_NE((void *)ok, NULL);
  bool text_ok = strcmp(cjson_str_val(ok), "caf\xc3\xa9 \xf0\x9f\x98\x80") == 0;
  cjson_destroy(ok);
  REQUIRE_TRUE(text_ok);
}

TEST(navigate,
     set_string_value_with_invalid_utf8_preserves_the_existing_value) {
  cjson root = cjson_parse("{\"k\":\"original\"}", NULL);
  const char *bad = "new\xc3value";
  ccol_retval r = cjson_set(root, "k", bad);
  bool unchanged = strcmp(cjson_str_val(cjson_get(root, "k")), "original") == 0;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_TRUE(unchanged);
}

TEST(construction, dictionary_set_with_an_invalid_utf8_key_leaves_child_alone) {
  cjson obj = cjson_create_dictionary();
  cjson child = cjson_create_int(5);
  ccol_retval r = cjson_dictionary_set(obj,
                                       "k\xff"
                                       "ey",
                                       child);
  cjson_node_type t = cjson_type(child);
  ccol_retval r2 = cjson_dictionary_set(obj, "key", child);
  size_t n = cjson_dictionary_size(obj);
  cjson_destroy(obj);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(t, CJSON_INTEGER);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(n, (size_t)1);
}

TEST(navigate, set_with_an_invalid_utf8_leaf_component_is_rejected) {
  cjson root = cjson_create_dictionary();
  ccol_retval r = cjson_set(root, "ke\xffy", 1);
  size_t n = cjson_dictionary_size(root);
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(n, (size_t)0);
}

// The largest stack the small-stack test asks for; musl's default is 128 KiB.
#define CJSON_TEST_SMALL_STACK (64u * 1024u)

// The depth the small-stack thread works at and whether it succeeded.
typedef struct {
  int depth;
  int ok;
} deep_small_stack_arg_t;

// Parses, serializes and clones a document nested a->depth levels deep and sets
// a->ok when every step succeeds and the clone writes the same text. It's the
// body of the small-stack thread.
static void *_cjson_deep_on_small_stack(void *argp) {
  deep_small_stack_arg_t *a = (deep_small_stack_arg_t *)argp;
  a->ok = 0;
  char *json = build_nested_object((size_t)a->depth);
  if (!json) return NULL;
  cjson root = cjson_parse(json, NULL);
  free(json);
  if (!root) return NULL;
  char *compact = cjson_serialize(root);
  char *pretty = cjson_serialize_pretty(root, 2);
  cjson copy = cjson_clone(root);
  char *copy_text = copy ? cjson_serialize(copy) : NULL;
  a->ok = compact && pretty && copy_text && strcmp(compact, copy_text) == 0;
  cjson_serialize_free(compact);
  cjson_serialize_free(pretty);
  cjson_serialize_free(copy_text);
  cjson_destroy(copy);
  cjson_destroy(root);
  return NULL;
}

// Parse, serialize and clone a document at the depth cap on a thread with a
// 64 KiB stack. It runs in a forked child that reports through a pipe, so a
// stack overflow fails this test only, and valgrind can't hide the result
// behind its exit code.
TEST(parse, a_maximally_nested_document_runs_on_a_small_thread_stack) {
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);

  pid_t pid = fork();
  REQUIRE_TRUE(pid >= 0);

  if (pid == 0) {
    close(fds[0]);
    unsigned char verdict = 0;
    size_t want = CJSON_TEST_SMALL_STACK;
    if (want < (size_t)PTHREAD_STACK_MIN) want = (size_t)PTHREAD_STACK_MIN;
    deep_small_stack_arg_t arg = {.depth = 499, .ok = 0};
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
      if (pthread_attr_setstacksize(&attr, want) == 0) {
        pthread_t tid;
        if (pthread_create(&tid, &attr, _cjson_deep_on_small_stack, &arg) ==
            0) {
          pthread_join(tid, NULL);
          verdict = arg.ok ? 1 : 0;
        }
      }
      pthread_attr_destroy(&attr);
    }
    ssize_t written = write(fds[1], &verdict, 1);
    (void)written;
    close(fds[1]);
    test_child_exit(0);
  }

  close(fds[1]);
  unsigned char verdict = 0;
  ssize_t got = read(fds[0], &verdict, 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);

  REQUIRE_EQ((int)got, 1);
  REQUIRE_EQ((int)verdict, 1);
}

// On a failed parse only the frame stack reaches the open containers, so each
// must be freed. Counting allocator calls makes it fail under plain make test.
TEST(oom, a_failed_parse_of_a_nested_document_releases_every_node) {
  _ta_allocs = 0;
  _ta_frees = 0;
  static const char *broken[] = {
      "[[1,2,",
      "{\"a\":{\"b\":[1,{\"c\":2}",
      "[[[[[[[[[[1]]]]]]]]]",
      "{\"a\":[{\"b\":[{\"c\":[1,2,3]}]}],\"d\":",
      "[1,[2,[3,[4,[5,@]]]]]",
  };
  bool all_rejected = true;
  for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); i++) {
    char *err = NULL;
    cjson d = cjson_parse_mp(broken[i], &err, &_tracking_alloc);
    if (d) {
      all_rejected = false;
      cjson_destroy(d);
    }
  }
  size_t allocs = _ta_allocs, frees = _ta_frees;
  REQUIRE_TRUE(all_rejected);
  REQUIRE_GT(allocs, (size_t)0);
  REQUIRE_EQ(allocs, frees);
}

// A const pointer that is const itself, for the cjson_set() value type tests.
static const char *const _cjson_set_types_file_scope_name = "prod";

TEST(set_value_types, string_literal) {
  cjson root = cjson_create_dictionary();
  ccol_retval r = cjson_set(root, "k", "champion");
  cjson leaf = cjson_get(root, "k");
  bool ok = leaf && cjson_type(leaf) == CJSON_STRING &&
            strcmp(cjson_str_val(leaf), "champion") == 0;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, char_array_filled_by_snprintf) {
  cjson root = cjson_create_dictionary();
  char buf[32];
  snprintf(buf, sizeof(buf), "host-%d", 7);
  ccol_retval r = cjson_set(root, "k", buf);
  buf[0] = 'X';
  cjson leaf = cjson_get(root, "k");
  bool ok = leaf && cjson_type(leaf) == CJSON_STRING &&
            strcmp(cjson_str_val(leaf), "host-7") == 0;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, const_char_array) {
  cjson root = cjson_create_dictionary();
  const char cbuf[] = "fixed";
  ccol_retval r = cjson_set(root, "k", cbuf);
  cjson leaf = cjson_get(root, "k");
  bool ok = leaf && cjson_type(leaf) == CJSON_STRING &&
            strcmp(cjson_str_val(leaf), "fixed") == 0;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, every_pointer_qualification) {
  cjson root = cjson_create_dictionary();
  char storage[] = "mutable";
  const char *p_const = "a";
  char *const p_top_const = storage;
  const char *const p_both = "b";
  ccol_retval r1 = cjson_set(root, "c", p_const);
  ccol_retval r2 = cjson_set(root, "t", p_top_const);
  ccol_retval r3 = cjson_set(root, "b", p_both);
  ccol_retval r4 = cjson_set(root, "f", _cjson_set_types_file_scope_name);
  const char *keys[4] = {"c", "t", "b", "f"};
  const char *want[4] = {"a", "mutable", "b", "prod"};
  bool ok = true;
  for (int i = 0; i < 4; i++) {
    cjson leaf = cjson_get(root, keys[i]);
    ok = ok && leaf && cjson_type(leaf) == CJSON_STRING &&
         strcmp(cjson_str_val(leaf), want[i]) == 0;
  }
  cjson_destroy(root);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_EQ(r4, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, bool_stays_bool) {
  cjson root = cjson_create_dictionary();
  const bool cb = true;
  ccol_retval r1 = cjson_set(root, "a", (bool)false);
  ccol_retval r2 = cjson_set(root, "b", cb);
  cjson a = cjson_get(root, "a"), b = cjson_get(root, "b");
  bool ok = a && b && cjson_type(a) == CJSON_BOOL &&
            cjson_type(b) == CJSON_BOOL && !cjson_bool_val(a) &&
            cjson_bool_val(b);
  cjson_destroy(root);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, narrow_integers_keep_their_sign) {
  cjson root = cjson_create_dictionary();
  const char c = 'A';
  signed char sc = -5;
  unsigned char uc = 250;
  short sh = -30000;
  unsigned short ush = 65535;
  ccol_retval r[5];
  r[0] = cjson_set(root, "c", c);
  r[1] = cjson_set(root, "sc", sc);
  r[2] = cjson_set(root, "uc", uc);
  r[3] = cjson_set(root, "sh", sh);
  r[4] = cjson_set(root, "ush", ush);
  cjson n[5] = {cjson_get(root, "c"), cjson_get(root, "sc"),
                cjson_get(root, "uc"), cjson_get(root, "sh"),
                cjson_get(root, "ush")};
  bool all_int = true;
  for (int i = 0; i < 5; i++)
    all_int = all_int && n[i] && cjson_type(n[i]) == CJSON_INTEGER;
  long long v[5] = {0};
  if (all_int)
    for (int i = 0; i < 5; i++) v[i] = cjson_int_val(n[i]);
  cjson_destroy(root);
  for (int i = 0; i < 5; i++) REQUIRE_EQ(r[i], ccol_success);
  REQUIRE_TRUE(all_int);
  REQUIRE_EQ(v[0], (long long)'A');
  REQUIRE_EQ(v[1], -5LL);
  REQUIRE_EQ(v[2], 250LL);
  REQUIRE_EQ(v[3], -30000LL);
  REQUIRE_EQ(v[4], 65535LL);
}

TEST(set_value_types, unsigned_long_long_above_llong_max) {
  cjson root = cjson_create_dictionary();
  unsigned long long big = (unsigned long long)LLONG_MAX + 2ULL;
  ccol_retval r = cjson_set(root, "k", big);
  cjson leaf = cjson_get(root, "k");
  bool is_float = leaf && cjson_type(leaf) == CJSON_FLOAT;
  double v = is_float ? cjson_double_val(leaf) : 0.0;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(is_float);
  REQUIRE_EQ(v, u64_parsed_value("9223372036854775809"));
  REQUIRE_EQ(v, 9223372036854775808.0);
}

TEST(set_value_types, float_and_double) {
  cjson root = cjson_create_dictionary();
  const float f = 0.5f;
  volatile double d = 0.25;
  ccol_retval r1 = cjson_set(root, "f", f);
  ccol_retval r2 = cjson_set(root, "d", d);
  cjson a = cjson_get(root, "f"), b = cjson_get(root, "d");
  bool ok = a && b && cjson_type(a) == CJSON_FLOAT &&
            cjson_type(b) == CJSON_FLOAT && cjson_double_val(a) == 0.5 &&
            cjson_double_val(b) == 0.25;
  cjson_destroy(root);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_TRUE(ok);
}

TEST(set_value_types, null_literal) {
  cjson root = cjson_create_dictionary();
  cjson_set(root, "k", "x");
  ccol_retval r = cjson_set(root, "k", NULL);
  cjson leaf = cjson_get(root, "k");
  bool ok = leaf && cjson_type(leaf) == CJSON_NULL;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(ok);
}

// An allocator that counts live blocks in _dup_live, so the duplicate key tests
// can see that every replaced value is freed.
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
  _dup_live = 0;
  cjson root = cjson_parse_mp(
      "{\"a\":{\"x\":[1,2,{\"y\":\"deep\"}]},\"b\":1,\"a\":[\"s\",{\"z\":2}],"
      "\"a\":\"last\",\"b\":{\"k\":true}}",
      NULL, &_dup_mp);
  bool parsed = root != NULL;
  size_t size = parsed ? cjson_dictionary_size(root) : 0;
  cjson a = parsed ? cjson_get(root, "a") : NULL;
  cjson bk = parsed ? cjson_get(root, "b.k") : NULL;
  bool a_ok = a && cjson_type(a) == CJSON_STRING &&
              strcmp(cjson_str_val(a), "last") == 0;
  bool b_ok = bk && cjson_type(bk) == CJSON_BOOL && cjson_bool_val(bk);
  if (root) cjson_destroy(root);
  long live = _dup_live;
  REQUIRE_TRUE(parsed);
  REQUIRE_EQ(size, (size_t)2);
  REQUIRE_TRUE(a_ok);
  REQUIRE_TRUE(b_ok);
  REQUIRE_EQ(live, 0L);
}

TEST(duplicate_keys, dictionary_set_replaces_and_frees_the_old_subtree) {
  _dup_live = 0;
  cjson root = cjson_create_dictionary_mp(&_dup_mp);
  REQUIRE_NE((void *)root, NULL);
  cjson first = cjson_create_list_mp(&_dup_mp);
  cjson_list_push(first, cjson_create_string_mp("old", &_dup_mp));
  ccol_retval r1 = cjson_dictionary_set(root, "k", first);
  ccol_retval r2 =
      cjson_dictionary_set(root, "k", cjson_create_int_mp(7, &_dup_mp));
  ccol_retval r3 = cjson_dictionary_set(root, "k", cjson_get(root, "k"));
  cjson k = cjson_get(root, "k");
  bool ok = k && cjson_type(k) == CJSON_INTEGER && cjson_int_val(k) == 7 &&
            cjson_dictionary_size(root) == 1;
  cjson_destroy(root);
  long live = _dup_live;
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(live, 0L);
}

TEST(duplicate_keys, attached_child_of_another_key_is_refused_untouched) {
  cjson root = cjson_create_dictionary();
  REQUIRE_NE((void *)root, NULL);
  cjson_dictionary_set(root, "a", cjson_create_int(1));
  cjson_dictionary_set(root, "b", cjson_create_int(2));
  ccol_retval r = cjson_dictionary_set(root, "b", cjson_get(root, "a"));
  cjson a = cjson_get(root, "a"), b = cjson_get(root, "b");
  bool ok = a && b && cjson_int_val(a) == 1 && cjson_int_val(b) == 2;
  cjson_destroy(root);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_TRUE(ok);
}

// An allocator that refuses every call once _ff_left reaches 0 (-1 never
// refuses) and counts live blocks in _ff_live, for the failed-set test.
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

// Each allocation of a set that creates a string leaf is refused in turn. The
// last is the string copy, which runs after the key is in the map, so its
// failure must remove the placeholder key; the size check catches a key left
// with a NULL child.
TEST(set_new_key, a_failed_build_removes_the_key_and_leaks_nothing) {
  _ff_left = -1;
  _ff_live = 0;
  cjson root = cjson_create_dictionary_mp(&_ff_mp);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval pre = cjson_set(root, "keep", 1);
  long live_before = _ff_live;

  size_t failures = 0, wrong = 0;
  ccol_retval r = ccol_not_enough_memory;
  for (long budget = 0; budget < 64 && r != ccol_success; budget++) {
    _ff_left = budget;
    r = cjson_set(root, "fresh", "a string value");
    _ff_left = -1;
    if (r == ccol_success) break;
    failures++;
    bool key_absent = cjson_get(root, "fresh") == NULL;
    bool size_ok = cjson_dictionary_size(root) == 1;
    bool no_leak = _ff_live == live_before;
    if (r != ccol_not_enough_memory || !key_absent || !size_ok || !no_leak)
      wrong++;
  }
  cjson fresh = cjson_get(root, "fresh");
  bool stored = fresh && cjson_type(fresh) == CJSON_STRING &&
                strcmp(cjson_str_val(fresh), "a string value") == 0;
  cjson_destroy(root);
  long live_after = _ff_live;
  REQUIRE_EQ(pre, ccol_success);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_GT(failures, (size_t)1);
  REQUIRE_EQ(wrong, (size_t)0);
  REQUIRE_TRUE(stored);
  REQUIRE_EQ(live_after, 0L);
}

// Parses and destroys a document five times on a new thread and clears *arg on
// a failed parse; the pool key lock test counts the locks it takes.
static void *cjson_pool_arm_worker(void *arg) {
  int *ok = (int *)arg;
  *ok = 1;
  for (int i = 0; i < 5; i++) {
    cjson d = cjson_parse("{\"a\":1,\"b\":[1,2,3],\"c\":{\"x\":\"y\"}}", NULL);
    if (!d) {
      *ok = 0;
      return NULL;
    }
    cjson_destroy(d);
  }
  return NULL;
}

// Each parse drains the pool and each destroy refills it; arming the key on
// every refill would take the lock 5 times instead of once.
TEST(node_pool, a_thread_takes_the_pool_key_lock_once) {
  unsigned long before = atomic_load(&cjson__pool_key_lock_count_for_tests);
  int ok = 0;
  pthread_t th;
  int created = pthread_create(&th, NULL, cjson_pool_arm_worker, &ok);
  if (created == 0) pthread_join(th, NULL);
  unsigned long after = atomic_load(&cjson__pool_key_lock_count_for_tests);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(after - before, 1ul);
}

// A key whose value owns a parsed tree; its destructor destroys the tree at
// thread exit.
static pthread_key_t cjson_pool_owner_key;

// Destroys the tree that a thread left on cjson_pool_owner_key, so the tree is
// freed from a key destructor at thread exit.
static void cjson_pool_owner_dtor(void *tree) {
  cjson t = (cjson)tree;
  cjson_destroy(t);
}

// Fills and arms this thread's pool first, then parks a parsed tree on
// cjson_pool_owner_key for its destructor and sets *arg on success.
static void *cjson_pool_owner_worker(void *arg) {
  int *ok = (int *)arg;
  cjson warm = cjson_parse("[1,2,3]", NULL);
  if (!warm) return NULL;
  cjson_destroy(warm);
  cjson tree =
      cjson_parse("{\"a\":[1,2,3,4,5,6,7,8],\"b\":{\"c\":\"d\"}}", NULL);
  if (!tree) return NULL;
  if (pthread_setspecific(cjson_pool_owner_key, tree) != 0) {
    cjson_destroy(tree);
    return NULL;
  }
  *ok = 1;
  return NULL;
}

// A key destructor that runs after the pool key's destroys a tree and refills
// the pool. The drain must leave the key disarmed so the refill arms it again;
// otherwise the nodes stay in a pool nothing frees and memtest reports them.
TEST(node_pool, a_tree_freed_by_a_later_key_destructor_is_drained) {
  cjson first = cjson_parse("[0]", NULL);
  REQUIRE_NE((void *)first, NULL);
  cjson_destroy(first);
  REQUIRE_EQ(pthread_key_create(&cjson_pool_owner_key, cjson_pool_owner_dtor),
             0);
  long before = atomic_load(&cjson__pool_population_for_tests);
  int ok = 0;
  pthread_t th;
  int created = pthread_create(&th, NULL, cjson_pool_owner_worker, &ok);
  if (created == 0) pthread_join(th, NULL);
  long after = atomic_load(&cjson__pool_population_for_tests);
  pthread_key_delete(cjson_pool_owner_key);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(after, before);
}

// The tree the late destructor destroys, and the pipe it reports on.
static cjson late_destroy_tree;
static int late_destroy_report_fd = -1;

// Destroys late_destroy_tree after the library's destructor has run and writes
// 'Y' to the pipe when the thread's pool stays empty, 'N' otherwise. A
// destructor with a priority runs after every one without.
__attribute__((destructor(101))) static void late_destroy_from_destructor(
    void) {
  if (!late_destroy_tree || late_destroy_report_fd < 0) return;
  cjson_destroy(late_destroy_tree);
  char verdict = cjson_debug_pool_size() == 0 ? 'Y' : 'N';
  ssize_t w = write(late_destroy_report_fd, &verdict, 1);
  (void)w;
  close(late_destroy_report_fd);
}

// A late destructor in a forked child destroys a tree after the library's
// destructor ran; pooling those nodes would leave them in a pool nothing
// drains, and the child would report 'N' on the pipe. OpenBSD's clang build
// runs no destructor, so it has nothing to report.
TEST(node_pool, a_tree_destroyed_after_the_library_destructor_is_freed) {
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    late_destroy_tree =
        cjson_parse("{\"a\":[1,2,3,{\"b\":\"text\"}],\"c\":[true,null]}", NULL);
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

// Serializes node compactly into buf (cap bytes), for the order tests, and
// frees the library's string.
static void dict_order_serialize(cjson node, char *buf, size_t cap) {
  char *s = cjson_serialize(node);
  snprintf(buf, cap, "%s", s ? s : "(null)");
  cjson_serialize_free(s);
}

TEST(dict_order, a_parsed_object_serializes_in_its_source_order) {
  const char *src =
      "{\"zeta\":1,\"alpha\":2,\"mid\":{\"q\":1,\"b\":2,\"x\":3},"
      "\"b\":[1,{\"k2\":0,\"k1\":0}]}";
  cjson doc = cjson_parse(src, NULL);
  char out[256];
  dict_order_serialize(doc, out, sizeof(out));
  char *pretty = cjson_serialize_pretty(doc, 1);
  cjson back = cjson_parse(pretty ? pretty : "", NULL);
  char out_back[256];
  dict_order_serialize(back, out_back, sizeof(out_back));
  cjson_serialize_free(pretty);
  cjson_destroy(back);
  cjson_destroy(doc);
  REQUIRE_STREQ(out, src);
  REQUIRE_STREQ(out_back, src);
}

TEST(dict_order, set_appends_a_new_key_and_a_replace_keeps_its_place) {
  cjson d = cjson_create_dictionary();
  cjson_dictionary_set(d, "c", cjson_create_int(1));
  cjson_dictionary_set(d, "a", cjson_create_int(2));
  cjson_dictionary_set(d, "b", cjson_create_int(3));
  char o1[128];
  dict_order_serialize(d, o1, sizeof(o1));
  cjson_dictionary_set(d, "a", cjson_create_string("new"));
  cjson_set(d, "c", 10);
  cjson_set(d, "d.e", 5);
  cjson_set(d, "e", (bool)true);
  char o2[128];
  dict_order_serialize(d, o2, sizeof(o2));
  cjson_dictionary_remove(d, "c");
  cjson_dictionary_set(d, "c", cjson_create_null());
  char o3[128];
  dict_order_serialize(d, o3, sizeof(o3));
  cjson_destroy(d);
  REQUIRE_STREQ(o1, "{\"c\":1,\"a\":2,\"b\":3}");
  REQUIRE_STREQ(o2, "{\"c\":10,\"a\":\"new\",\"b\":3,\"e\":true}");
  REQUIRE_STREQ(o3, "{\"a\":\"new\",\"b\":3,\"e\":true,\"c\":null}");
}

TEST(dict_order, a_duplicate_key_keeps_the_first_place_and_the_last_value) {
  cjson doc =
      cjson_parse("{\"x\":1,\"y\":2,\"x\":{\"z\":3},\"w\":4,\"y\":5}", NULL);
  char out[128];
  dict_order_serialize(doc, out, sizeof(out));
  size_t n = doc ? cjson_dictionary_size(doc) : 0;
  cjson_destroy(doc);
  REQUIRE_STREQ(out, "{\"x\":{\"z\":3},\"y\":5,\"w\":4}");
  REQUIRE_EQ(n, (size_t)3);
}

TEST(dict_order, a_clone_keeps_the_order_of_its_source) {
  cjson src = cjson_parse(
      "{\"b\":1,\"a\":{\"d\":1,\"c\":2},\"c\":[{\"z\":1,"
      "\"y\":2}]}",
      NULL);
  cjson copy = cjson_clone(src);
  char o_src[128], o_copy[128];
  dict_order_serialize(src, o_src, sizeof(o_src));
  dict_order_serialize(copy, o_copy, sizeof(o_copy));
  cjson_destroy(copy);
  cjson_destroy(src);
  REQUIRE_STREQ(o_src,
                "{\"b\":1,\"a\":{\"d\":1,\"c\":2},\"c\":[{\"z\":1,"
                "\"y\":2}]}");
  REQUIRE_STREQ(o_copy, o_src);
}

TEST(dict_order, the_order_survives_growth_and_shrinking) {
  enum { N = 3000 };
  size_t cap = (size_t)N * 16 + 16;
  char *expect = malloc(cap);
  size_t len = 0;
  expect[len++] = '{';
  cjson d = cjson_create_dictionary();
  char key[32];
  for (int i = 0; i < N; i++) {
    snprintf(key, sizeof(key), "k%d", (i * 7919) % N);
    cjson_dictionary_set(d, key, cjson_create_int(i));
  }
  for (int i = 0; i < N / 2; i++) {
    snprintf(key, sizeof(key), "k%d", (i * 7919) % N);
    cjson_dictionary_remove(d, key);
  }
  for (int i = N / 2; i < N; i++) {
    if (len > 1) expect[len++] = ',';
    len += (size_t)snprintf(expect + len, cap - len, "\"k%d\":%d",
                            (i * 7919) % N, i);
  }
  expect[len++] = '}';
  expect[len] = '\0';
  char *out = cjson_serialize(d);
  bool same = out && strcmp(out, expect) == 0;
  cjson_serialize_free(out);
  cjson_destroy(d);
  free(expect);
  REQUIRE_TRUE(same);
}

TEST(dict_iter, walks_the_members_in_insertion_order) {
  cjson doc = cjson_parse("{\"c\":1,\"a\":\"two\",\"b\":[3]}", NULL);
  char keys[64] = "";
  long long first_val = -1;
  const char *second_val = NULL;
  size_t third_len = 0;
  size_t visited = 0;
  cjson_dictionary_iter it;
  for (bool ok = cjson_dictionary_first(doc, &it); ok;
       ok = cjson_dictionary_next(&it)) {
    strncat(keys, it.key, sizeof(keys) - strlen(keys) - 1);
    cjson_node_type t = cjson_type(it.value);
    if (visited == 0 && t == CJSON_INTEGER) first_val = cjson_int_val(it.value);
    if (visited == 1 && t == CJSON_STRING) second_val = cjson_str_val(it.value);
    if (visited == 2 && t == CJSON_LIST) third_len = cjson_list_size(it.value);
    visited++;
  }
  bool end_cleared = it.key == NULL && it.value == NULL;
  bool again = cjson_dictionary_next(&it);
  char second[8];
  snprintf(second, sizeof(second), "%s", second_val ? second_val : "");
  cjson_destroy(doc);
  REQUIRE_STREQ(keys, "cab");
  REQUIRE_EQ(visited, (size_t)3);
  REQUIRE_EQ(first_val, 1LL);
  REQUIRE_STREQ(second, "two");
  REQUIRE_EQ(third_len, (size_t)1);
  REQUIRE_TRUE(end_cleared);
  REQUIRE_FALSE(again);
}

TEST(dict_iter, null_empty_and_non_dictionary_give_false_and_clear_fields) {
  cjson empty = cjson_create_dictionary();
  cjson list = cjson_create_list();
  cjson scalar = cjson_create_int(3);
  cjson_dictionary_iter it;
  cjson poison = scalar;
  bool results[4];
  bool cleared[4];
  cjson inputs[4] = {NULL, empty, list, scalar};
  for (int i = 0; i < 4; i++) {
    it.key = "poison";
    it.value = poison;
    results[i] = cjson_dictionary_first(inputs[i], &it);
    cleared[i] = it.key == NULL && it.value == NULL;
  }
  bool null_it = cjson_dictionary_first(empty, NULL);
  bool null_next = cjson_dictionary_next(NULL);
  cjson_destroy(empty);
  cjson_destroy(list);
  cjson_destroy(scalar);
  for (int i = 0; i < 4; i++) {
    REQUIRE_FALSE(results[i]);
    REQUIRE_TRUE(cleared[i]);
  }
  REQUIRE_FALSE(null_it);
  REQUIRE_FALSE(null_next);
}

TEST(dict_iter, removing_the_current_member_keeps_the_walk_going) {
  cjson d = cjson_parse("{\"a\":1,\"b\":{\"x\":[1,2]},\"c\":3,\"d\":4,\"e\":5}",
                        NULL);
  char seen[16] = "";
  cjson_dictionary_iter it;
  for (bool ok = cjson_dictionary_first(d, &it); ok;
       ok = cjson_dictionary_next(&it)) {
    strncat(seen, it.key, sizeof(seen) - strlen(seen) - 1);
    if (strcmp(it.key, "c") != 0 && strcmp(it.key, "e") != 0) {
      cjson_dictionary_remove(d, it.key);
    }
  }
  char left[64];
  dict_order_serialize(d, left, sizeof(left));
  for (bool ok = cjson_dictionary_first(d, &it); ok;
       ok = cjson_dictionary_next(&it)) {
    cjson_dictionary_remove(d, it.key);
  }
  size_t n = cjson_dictionary_size(d);
  cjson_destroy(d);
  REQUIRE_STREQ(seen, "abcde");
  REQUIRE_STREQ(left, "{\"c\":3,\"e\":5}");
  REQUIRE_EQ(n, (size_t)0);
}

TEST(dict_iter, a_walk_after_a_replace_sees_the_new_value_in_the_old_place) {
  cjson d = cjson_parse("{\"a\":1,\"b\":2,\"c\":3}", NULL);
  cjson_dictionary_set(d, "b", cjson_create_string("replaced"));
  char keys[8] = "";
  const char *b_val = NULL;
  cjson_dictionary_iter it;
  for (bool ok = cjson_dictionary_first(d, &it); ok;
       ok = cjson_dictionary_next(&it)) {
    strncat(keys, it.key, sizeof(keys) - strlen(keys) - 1);
    if (strcmp(it.key, "b") == 0) b_val = cjson_str_val(it.value);
  }
  bool b_ok = b_val && strcmp(b_val, "replaced") == 0;
  cjson_destroy(d);
  REQUIRE_STREQ(keys, "abc");
  REQUIRE_TRUE(b_ok);
}

TEST(dict_iter, a_walk_allocates_nothing) {
  cjson d = cjson_parse_mp("{\"a\":1,\"b\":2,\"c\":3}", NULL, &g_counting_mp);
  g_alloc_remaining = 0;
  size_t visited = 0;
  cjson_dictionary_iter it;
  for (bool ok = cjson_dictionary_first(d, &it); ok;
       ok = cjson_dictionary_next(&it)) {
    visited++;
  }
  char *out = cjson_serialize(d);
  cjson copy = cjson_clone(d);
  g_alloc_remaining = -1;
  cjson_destroy(d);
  REQUIRE_EQ(visited, (size_t)3);
  REQUIRE_EQ((void *)out, NULL);
  REQUIRE_EQ((void *)copy, NULL);
}

// The shape of a nesting test document: All arrays, all objects, or both
// alternating by level.
typedef enum { DEPTH_ARRAYS, DEPTH_OBJECTS, DEPTH_MIXED } depth_shape_t;

// Builds n containers nested in the given shape, in exactly the compact text
// cjson_serialize() gives back. With with_scalar the innermost one holds 1;
// otherwise it's empty. The caller frees the result.
static char *depth_build(size_t n, depth_shape_t shape, bool with_scalar) {
  char *buf = malloc(7 * n + 8);
  size_t pos = 0;
  for (size_t i = 0; i < n; i++) {
    bool obj = shape == DEPTH_OBJECTS || (shape == DEPTH_MIXED && (i & 1));
    bool innermost = i + 1 == n;
    buf[pos++] = obj ? '{' : '[';
    if (obj && (!innermost || with_scalar)) {
      memcpy(buf + pos, "\"k\":", 4);
      pos += 4;
    }
  }
  if (with_scalar) buf[pos++] = '1';
  for (size_t i = n; i-- > 0;) {
    bool obj = shape == DEPTH_OBJECTS || (shape == DEPTH_MIXED && (i & 1));
    buf[pos++] = obj ? '}' : ']';
  }
  buf[pos] = '\0';
  return buf;
}

// Every shape, for the boundary tests to loop over.
static const depth_shape_t depth_shapes[] = {DEPTH_ARRAYS, DEPTH_OBJECTS,
                                             DEPTH_MIXED};

// 500 containers parse with or without an innermost scalar, in every shape, and
// 501 are an error naming the limit; a guard that counts the scalar or is one
// level loose fails one of the two edges.
TEST(depth_boundary, parse_accepts_500_and_refuses_501) {
  int accepted_500 = 0, refused_501 = 0, named_limit = 0;
  for (size_t s = 0; s < 3; s++) {
    for (int sc = 0; sc < 2; sc++) {
      char *ok_doc = depth_build(500, depth_shapes[s], sc);
      char *bad_doc = depth_build(501, depth_shapes[s], sc);
      cjson ok = cjson_parse(ok_doc, NULL);
      char *err = NULL;
      cjson bad = cjson_parse(bad_doc, &err);
      if (ok) accepted_500++;
      if (!bad) refused_501++;
      if (!bad && err && strstr(err, "maximum nesting depth (500)"))
        named_limit++;
      cjson_destroy(ok);
      cjson_destroy(bad);
      free(ok_doc);
      free(bad_doc);
    }
  }
  REQUIRE_EQ(accepted_500, 6);
  REQUIRE_EQ(refused_501, 6);
  REQUIRE_EQ(named_limit, 6);
}

// A tree parsed at the limit serializes to the same text and clones; one more
// container added through the API makes all three refuse it.
TEST(depth_boundary, serialize_and_clone_accept_500_and_refuse_501) {
  int parsed = 0, ser_ok = 0, ser_exact = 0, pretty_ok = 0, clone_ok = 0;
  int clone_exact = 0, ser_refused = 0, pretty_refused = 0;
  int clone_refused = 0;
  for (size_t s = 0; s < 3; s++) {
    for (int sc = 0; sc < 2; sc++) {
      char *doc_txt = depth_build(500, depth_shapes[s], sc);
      cjson doc = cjson_parse(doc_txt, NULL);
      if (doc) {
        parsed++;
        char *out = cjson_serialize(doc);
        char *pretty = cjson_serialize_pretty(doc, 1);
        cjson copy = cjson_clone(doc);
        if (out) ser_ok++;
        if (out && strcmp(out, doc_txt) == 0) ser_exact++;
        if (pretty) pretty_ok++;
        if (copy) clone_ok++;
        char *copy_out = copy ? cjson_serialize(copy) : NULL;
        if (copy_out && strcmp(copy_out, doc_txt) == 0) clone_exact++;
        cjson_serialize_free(out);
        cjson_serialize_free(pretty);
        cjson_serialize_free(copy_out);
        cjson_destroy(copy);

        cjson outer =
            (s == 1) ? cjson_create_dictionary() : cjson_create_list();
        ccol_retval r = (s == 1) ? cjson_dictionary_set(outer, "k", doc)
                                 : cjson_list_push(outer, doc);
        if (r == ccol_success) {
          char *deep_out = cjson_serialize(outer);
          char *deep_pretty = cjson_serialize_pretty(outer, 1);
          cjson deep_copy = cjson_clone(outer);
          if (!deep_out) ser_refused++;
          if (!deep_pretty) pretty_refused++;
          if (!deep_copy) clone_refused++;
          cjson_serialize_free(deep_out);
          cjson_serialize_free(deep_pretty);
          cjson_destroy(deep_copy);
        }
        cjson_destroy(outer);
      }
      free(doc_txt);
    }
  }
  REQUIRE_EQ(parsed, 6);
  REQUIRE_EQ(ser_ok, 6);
  REQUIRE_EQ(ser_exact, 6);
  REQUIRE_EQ(pretty_ok, 6);
  REQUIRE_EQ(clone_ok, 6);
  REQUIRE_EQ(clone_exact, 6);
  REQUIRE_EQ(ser_refused, 6);
  REQUIRE_EQ(pretty_refused, 6);
  REQUIRE_EQ(clone_refused, 6);
}

TEST(depth_boundary, a_subtree_within_the_limit_serializes_and_clones) {
  char *txt = depth_build(500, DEPTH_ARRAYS, true);
  cjson doc = cjson_parse(txt, NULL);
  free(txt);
  cjson outer = cjson_create_list();
  ccol_retval r = cjson_list_push(outer, doc);
  char *whole = cjson_serialize(outer);
  char *sub = cjson_serialize(cjson_list_get(outer, 0));
  cjson sub_copy = cjson_clone(cjson_list_get(outer, 0));
  bool sub_ok = sub != NULL, sub_copy_ok = sub_copy != NULL;
  bool whole_refused = whole == NULL;
  cjson_serialize_free(whole);
  cjson_serialize_free(sub);
  cjson_destroy(sub_copy);
  cjson_destroy(outer);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(whole_refused);
  REQUIRE_TRUE(sub_ok);
  REQUIRE_TRUE(sub_copy_ok);
}

// Tells whether every byte of s is printable ASCII, for the error text tests.
static bool err_is_printable_ascii(const char *s) {
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

// Every byte value goes into every template at a place a parse error can quote
// it, through cjson_parse_n() so a NUL gets in too, and every message must be
// printable ASCII.
TEST(parse_err_text, every_message_is_printable_ascii) {
  static const char *const templates[] = {
      "@",           "[@",          "[1@",
      "{\"a\":@",    "\"\\@\"",     "\"\\u@000\"",
      "\"\\u0@00\"", "\"\\u00@0\"", "\"\\u000@\"",
      "{@",          "{\"a\"@",     "\"a@",
      "tru@",        "nul@",        "1.@",
      "1e@",         "-@",          "[1,@",
      "{\"a\":1,@",  "[1]@",        "\"\\ud800\\u@000\""};
  size_t failures = 0, bad_messages = 0, missing = 0;
  char buf[32];
  for (size_t t = 0; t < sizeof(templates) / sizeof(templates[0]); t++) {
    const char *tpl = templates[t];
    size_t len = strlen(tpl);
    const char *at = strchr(tpl, '@');
    size_t off = (size_t)(at - tpl);
    for (int b = 0; b < 256; b++) {
      memcpy(buf, tpl, len);
      buf[off] = (char)b;
      char *err = NULL;
      cjson n = cjson_parse_n(buf, len, &err);
      if (n) {
        cjson_destroy(n);
        continue;
      }
      failures++;
      if (!err)
        missing++;
      else if (!err_is_printable_ascii(err))
        bad_messages++;
    }
  }
  REQUIRE_GT(failures, (size_t)1000);
  REQUIRE_EQ(missing, (size_t)0);
  REQUIRE_EQ(bad_messages, (size_t)0);
}

TEST(parse_err_text, the_offending_byte_is_spelled_readably) {
  char *err = NULL;
  cjson n = cjson_parse("\xEF\xBB\xBF{}", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err, "unexpected byte 0xEF at position 0");

  n = cjson_parse("x", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err, "unexpected character 'x' at position 0");

  n = cjson_parse_n("\x01", 1, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err, "unexpected byte 0x01 at position 0");

  n = cjson_parse_n("[\0]", 3, &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err, "unexpected byte 0x00 at position 1");

  n = cjson_parse("\"\\\xFF\"", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err,
                "unknown escape: '\\' followed by byte 0xFF at position 2");

  n = cjson_parse("\"\\q\"", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err, "unknown escape '\\q' at position 2");

  n = cjson_parse("\"\\u00\xC3\xA9\"", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err, "invalid hex digit byte 0xC3 in \\uXXXX at position 5");

  n = cjson_parse("\"\\u00g0\"", &err);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_STREQ(err, "invalid hex digit 'g' in \\uXXXX at position 5");
}

TEST(dict_iter, survives_replacement_of_current_and_other_values) {
  cjson d = cjson_parse("{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5}", NULL);
  char order[8] = {0};
  size_t visited = 0;
  long long seen_d = 0;
  ccol_retval r1 = ccol_success, r2 = ccol_success, r3 = ccol_success;
  ccol_retval r4 = ccol_success;
  bool value_is_same_node_after_set = false;
  cjson_dictionary_iter it;
  for (bool ok = cjson_dictionary_first(d, &it); ok;
       ok = cjson_dictionary_next(&it)) {
    if (visited < sizeof(order) - 1) order[visited] = it.key[0];
    visited++;
    if (strcmp(it.key, "a") == 0) {
      r1 = cjson_dictionary_set(d, "a", cjson_create_int(10));
    } else if (strcmp(it.key, "b") == 0) {
      cjson before = it.value;
      r2 = cjson_set(d, "b", 20);
      value_is_same_node_after_set = cjson_dictionary_get(d, "b") == before;
      r3 = cjson_dictionary_set(d, "d", cjson_create_int(40));
    } else if (strcmp(it.key, "c") == 0) {
      r4 = cjson_dictionary_remove(d, "a");
    } else if (strcmp(it.key, "d") == 0) {
      seen_d = cjson_int_val(it.value);
    }
  }
  char *out = cjson_serialize(d);
  bool out_ok = out && strcmp(out, "{\"b\":20,\"c\":3,\"d\":40,\"e\":5}") == 0;
  cjson_serialize_free(out);
  cjson_destroy(d);
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

// cjson_destroy(a[--k]) must destroy every tree; evaluating the argument twice
// would skip every second one. The trees sit at the end of a larger array, so a
// wrong index stays inside the test's storage.
TEST(cjson_macros, destroy_evaluates_its_argument_once) {
  cjson store[4] = {NULL, NULL, cjson_parse("[1,2]", NULL),
                    cjson_parse("{\"a\":1}", NULL)};
  cjson *trees = &store[2];
  bool created = trees[0] && trees[1];
  int k = 2;
  while (k > 0) cjson_destroy(trees[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ((void *)trees[0], NULL);
  REQUIRE_EQ((void *)trees[1], NULL);
}

// The honest allocator of the procs tests (_pl_* functions): It counts calls
// and live blocks. The poison allocator (_pl_poison_*) forwards to the C
// library and counts calls in _pl_poison_calls; any call means the library read
// the caller's struct after the call that took it.
static atomic_long _pl_live;
static atomic_long _pl_calls;
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

// After the tree exists the caller overwrites its procs struct with a counting
// "poison" allocator; any call through it means a node kept the caller's
// pointer instead of the interned copy.
TEST(custom_alloc, tree_does_not_read_the_procs_struct_of_the_caller_later) {
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

  cjson root = cjson_parse_mp("{\"a\": [1, 2, {\"b\": \"x\"}]}", NULL, mp);
  cjson leaf = cjson_create_string_mp("a leaf string", mp);
  cjson list = cjson_create_list_mp(mp);
  *mp = poison;
  long calls_at_overwrite = _pl_calls;

  bool built = root && leaf && list;
  ccol_retval set_rv = ccol_invalid_args, push_rv = ccol_invalid_args,
              dict_rv = ccol_invalid_args;
  char *text = NULL;
  cjson copy = NULL;
  if (built) {
    set_rv = cjson_set(root, "a.#2.c", "a string long enough to be copied");
    push_rv = cjson_list_push(list, leaf);
    if (push_rv == ccol_success) leaf = NULL;
    dict_rv = cjson_dictionary_set(root, "list", list);
    if (dict_rv == ccol_success) list = NULL;
    text = cjson_serialize(root);
    copy = cjson_clone(root);
  }
  bool have_text = text != NULL;
  bool have_copy = copy != NULL;
  if (text) cjson_serialize_free_mp(text, (ccol_memmgmt_procs *)&honest);
  cjson_destroy(copy);
  cjson_destroy(leaf);
  cjson_destroy(list);
  cjson_destroy(root);
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

TEST(custom_alloc, procs_structs_with_the_same_functions_are_one_allocator) {
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

TEST(custom_alloc, a_full_procs_table_fails_cleanly) {
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
  cjson refused_node = cjson_create_int_mp(7, &fresh);
  char *err = NULL;
  cjson refused_tree = cjson_parse_mp("[1]", &err, &fresh);
  bool err_named = err && strstr(err, "allocator") != NULL;
  long poison_calls = _pl_poison_calls;
  _pl_live = 0;
  cjson accepted = cjson_parse_mp("[1, \"two\"]", NULL, &known);
  bool accepted_ok = accepted != NULL;
  bool node_refused = refused_node == NULL;
  bool tree_refused = refused_tree == NULL;
  cjson_destroy(accepted);
  long live = _pl_live;
  cjson_destroy(refused_node);
  cjson_destroy(refused_tree);
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

// In C11 true is the int 1, so it's stored as CJSON_INTEGER, and (bool)true as
// CJSON_BOOL. tests_c23.c covers the C23 side.
TEST(set_value_types, c11_true_is_an_int_and_a_cast_makes_a_bool) {
  cjson doc = cjson_parse("{\"i\": 0, \"b\": 0}", NULL);
  ccol_retval ri = doc ? cjson_set(doc, "i", true) : ccol_not_enough_memory;
  ccol_retval rb =
      doc ? cjson_set(doc, "b", (bool)true) : ccol_not_enough_memory;
  cjson i = doc ? cjson_get(doc, "i") : NULL;
  cjson b = doc ? cjson_get(doc, "b") : NULL;
  bool i_is_int = i && cjson_type(i) == CJSON_INTEGER && cjson_int_val(i) == 1;
  bool b_is_bool = b && cjson_type(b) == CJSON_BOOL && cjson_bool_val(b);
  cjson_destroy(doc);
  REQUIRE_EQ(ri, ccol_success);
  REQUIRE_EQ(rb, ccol_success);
  REQUIRE_TRUE(i_is_int);
  REQUIRE_TRUE(b_is_bool);
}

// What the memory probe allocator measures: Live and peak bytes, live blocks,
// calls; fail makes every later call fail.
typedef struct {
  size_t live_bytes;
  size_t peak_bytes;
  size_t live_blocks;
  size_t calls;
  bool fail;
} _cjson_mem_probe_t;
// The state of the memory probe allocator.
static _cjson_mem_probe_t g_mem;
// The size header the probe puts before each block (16 keeps it aligned).
#define _CJSON_MEM_HDR 16

// Adds add and removes sub from the live byte count and raises the peak, for
// the probe allocator functions below.
static void _mem_note(size_t add, size_t sub) {
  g_mem.live_bytes += add;
  g_mem.live_bytes -= sub;
  if (g_mem.live_bytes > g_mem.peak_bytes) g_mem.peak_bytes = g_mem.live_bytes;
}
// The probe allocator: Each block records its size in a header so the
// functions can follow the live and peak bytes in g_mem.
static void *_mem_malloc(size_t n) {
  if (g_mem.fail) return NULL;
  char *p = malloc(n + _CJSON_MEM_HDR);
  if (!p) return NULL;
  memcpy(p, &n, sizeof(n));
  g_mem.calls++;
  g_mem.live_blocks++;
  _mem_note(n, 0);
  return p + _CJSON_MEM_HDR;
}
static void _mem_free(void *q) {
  if (!q) return;
  char *p = (char *)q - _CJSON_MEM_HDR;
  size_t n;
  memcpy(&n, p, sizeof(n));
  g_mem.live_blocks--;
  _mem_note(0, n);
  free(p);
}
static void *_mem_calloc(size_t a, size_t b) {
  void *q = _mem_malloc(a * b);
  if (q) memset(q, 0, a * b);
  return q;
}
static void *_mem_realloc(void *q, size_t n) {
  if (!q) return _mem_malloc(n);
  if (g_mem.fail) return NULL;
  char *p = (char *)q - _CJSON_MEM_HDR;
  size_t old;
  memcpy(&old, p, sizeof(old));
  char *np = realloc(p, n + _CJSON_MEM_HDR);
  if (!np) return NULL;
  memcpy(np, &n, sizeof(n));
  g_mem.calls++;
  _mem_note(n, old);
  return np + _CJSON_MEM_HDR;
}
static ccol_memmgmt_procs g_mem_mp = {.malloc = _mem_malloc,
                                      .free = _mem_free,
                                      .calloc = _mem_calloc,
                                      .realloc = _mem_realloc};

// Clears g_mem before a measurement.
static void _mem_reset(void) { memset(&g_mem, 0, sizeof(g_mem)); }

// Builds "[u,u,...,u]" with count copies of unit and its length in *len_out,
// for the memory tests; the caller frees it.
static char *_mem_repeat_in_list(const char *unit, size_t count,
                                 size_t *len_out) {
  size_t ul = strlen(unit);
  size_t len = 2 + count * (ul + 1);
  char *b = malloc(len + 1);
  if (!b) return NULL;
  char *w = b;
  *w++ = '[';
  for (size_t i = 0; i < count; i++) {
    memcpy(w, unit, ul);
    w += ul;
    *w++ = (i + 1 < count) ? ',' : ']';
  }
  *w = '\0';
  *len_out = (size_t)(w - b);
  return b;
}

// Gives the most bytes the library held at once while parsing src, per byte of
// src (with the tree still alive at the end), or 0 when the parse failed.
static double _mem_parse_ratio(const char *src, size_t len) {
  _mem_reset();
  cjson doc = cjson_parse_n_mp(src, len, NULL, &g_mem_mp);
  double ratio = doc ? (double)g_mem.peak_bytes / (double)len : 0.0;
  cjson_destroy(doc);
  return ratio;
}

// A container builds its store on its first child, so "{}" and "[]" cost one
// node each.
TEST(memory, an_empty_object_or_list_costs_one_node_and_no_store) {
  enum { N = 2000 };
  size_t len_o = 0, len_l = 0;
  char *objs = _mem_repeat_in_list("{}", N, &len_o);
  char *lists = _mem_repeat_in_list("[]", N, &len_l);
  bool built = objs && lists;

  double r_o = built ? _mem_parse_ratio(objs, len_o) : 0.0;
  size_t calls_o = g_mem.calls;
  double r_l = built ? _mem_parse_ratio(lists, len_l) : 0.0;
  size_t calls_l = g_mem.calls;
  size_t leaked = g_mem.live_blocks;
  free(objs);
  free(lists);

  REQUIRE_TRUE(built);
  REQUIRE_GT(r_o, 0.0);
  REQUIRE_GT(r_l, 0.0);
  REQUIRE_LT(calls_o, (size_t)N + 64);
  REQUIRE_LT(calls_l, (size_t)N + 64);
  REQUIRE_LT(r_o, 20.0);
  REQUIRE_LT(r_l, 20.0);
  REQUIRE_EQ(leaked, (size_t)0);
}

// The densest shapes the grammar allows (empty containers, tiny keys and
// numbers, one-member containers, the deepest legal nesting) stay under the 65
// bytes of DOM per input byte that cjson_parse(3) states.
TEST(memory, the_densest_documents_stay_under_the_documented_bound) {
  static const char *const units[] = {
      "{}",        "[]",         "0",           "\"\"",      "{\"\":0}",
      "{\"\":{}}", "{\"\":[]}",  "{\"\":\"\"}", "[0]",       "[[]]",
      "[[0]]",     "[{\"\":0}]", "{\"a\":0}",   "{\"\":[0]}"};
  double worst = 0.0;
  bool all_parsed = true;
  for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
    size_t len = 0;
    char *doc = _mem_repeat_in_list(units[u], 5000, &len);
    double r = doc ? _mem_parse_ratio(doc, len) : 0.0;
    free(doc);
    if (r == 0.0) all_parsed = false;
    if (r > worst) worst = r;
  }
  for (int kind = 0; kind < 2; kind++) {
    char unit[2600];
    char *w = unit;
    for (int i = 0; i < 499; i++) {
      if (kind == 0) {
        memcpy(w, "{\"\":", 4);
        w += 4;
      } else {
        *w++ = '[';
      }
    }
    *w++ = '0';
    for (int i = 0; i < 499; i++) *w++ = kind == 0 ? '}' : ']';
    *w = '\0';
    size_t len = 0;
    char *doc = _mem_repeat_in_list(unit, 40, &len);
    double r = doc ? _mem_parse_ratio(doc, len) : 0.0;
    free(doc);
    if (r == 0.0) all_parsed = false;
    if (r > worst) worst = r;
  }
  REQUIRE_TRUE(all_parsed);
  REQUIRE_LT(worst, 65.0);
  REQUIRE_EQ(g_mem.live_blocks, (size_t)0);
}

TEST(empty_containers, every_call_reads_an_empty_container_as_empty) {
  cjson doc = cjson_parse("{\"d\":{},\"l\":[]}", NULL);
  REQUIRE_NE((void *)doc, NULL);
  cjson d = cjson_get(doc, "d");
  cjson l = cjson_get(doc, "l");
  cjson fd = cjson_create_dictionary();
  cjson fl = cjson_create_list();
  cjson_dictionary_iter it;

  bool reads_ok =
      d && l && fd && fl && cjson_dictionary_size(d) == 0 &&
      cjson_list_size(l) == 0 && cjson_dictionary_size(fd) == 0 &&
      cjson_list_size(fl) == 0 && cjson_dictionary_get(d, "x") == NULL &&
      cjson_list_get(l, 0) == NULL && !cjson_dictionary_first(d, &it) &&
      cjson_list_remove(l, 0) == ccol_invalid_args &&
      cjson_dictionary_remove(d, "x") == ccol_key_not_found &&
      cjson_get(doc, "d.x") == NULL && cjson_get(doc, "l.#0") == NULL &&
      cjson_set(doc, "l.#0", 1) == ccol_key_not_found &&
      cjson_remove(doc, "d.x") == ccol_key_not_found &&
      cjson_remove(doc, "l.#0") == ccol_key_not_found;

  char *compact = cjson_serialize(doc);
  char *pretty = cjson_serialize_pretty(doc, 2);
  cjson copy = cjson_clone(doc);
  char *copied = copy ? cjson_serialize(copy) : NULL;
  bool text_ok = compact && strcmp(compact, "{\"d\":{},\"l\":[]}") == 0 &&
                 pretty &&
                 strcmp(pretty, "{\n  \"d\": {},\n  \"l\": []\n}") == 0 &&
                 copied && strcmp(copied, compact) == 0;
  cjson_serialize_free(compact);
  cjson_serialize_free(pretty);
  cjson_serialize_free(copied);

  bool inserts_ok =
      cjson_set(doc, "d.x", 1) == ccol_success &&
      cjson_list_push(l, cjson_create_int(2)) == ccol_success &&
      cjson_dictionary_set(fd, "k", cjson_create_bool(true)) == ccol_success &&
      cjson_list_push(fl, cjson_create_null()) == ccol_success && copy &&
      cjson_list_push(cjson_get(copy, "l"), cjson_create_int(3)) ==
          ccol_success &&
      cjson_dictionary_set(cjson_get(copy, "d"), "y", cjson_create_int(4)) ==
          ccol_success;
  char *after = cjson_serialize(doc);
  char *after_copy = copy ? cjson_serialize(copy) : NULL;
  char *after_fd = cjson_serialize(fd);
  char *after_fl = cjson_serialize(fl);
  bool after_ok = after && strcmp(after, "{\"d\":{\"x\":1},\"l\":[2]}") == 0 &&
                  after_copy &&
                  strcmp(after_copy, "{\"d\":{\"y\":4},\"l\":[3]}") == 0 &&
                  after_fd && strcmp(after_fd, "{\"k\":true}") == 0 &&
                  after_fl && strcmp(after_fl, "[null]") == 0;
  cjson_serialize_free(after);
  cjson_serialize_free(after_copy);
  cjson_serialize_free(after_fd);
  cjson_serialize_free(after_fl);

  cjson e = cjson_parse("{\"a\":{},\"b\":[]}", NULL);
  bool reinit_ok = e && cjson_set(e, "a", 5) == ccol_success &&
                   cjson_set(e, "b", "s") == ccol_success &&
                   cjson_int_val(cjson_get(e, "a")) == 5 &&
                   strcmp(cjson_str_val(cjson_get(e, "b")), "s") == 0;

  cjson_destroy(e);
  cjson_destroy(copy);
  cjson_destroy(fd);
  cjson_destroy(fl);
  cjson_destroy(doc);
  REQUIRE_TRUE(reads_ok);
  REQUIRE_TRUE(text_ok);
  REQUIRE_TRUE(inserts_ok);
  REQUIRE_TRUE(after_ok);
  REQUIRE_TRUE(reinit_ok);
}

// When the store of an empty container can't be allocated, push and set
// destroy the child as for any memory failure, cjson_set() changes nothing, and
// the container stays empty and usable.
TEST(empty_containers,
     a_first_insert_that_cannot_build_the_store_fails_cleanly) {
  _mem_reset();
  cjson list = cjson_create_list_mp(&g_mem_mp);
  cjson dict = cjson_create_dictionary_mp(&g_mem_mp);
  cjson c1 = cjson_create_int_mp(1, &g_mem_mp);
  cjson c2 = cjson_create_int_mp(2, &g_mem_mp);
  bool built = list && dict && c1 && c2;
  size_t blocks_before = g_mem.live_blocks;

  g_mem.fail = true;
  ccol_retval rl = built ? cjson_list_push(list, c1) : ccol_success;
  ccol_retval rd = built ? cjson_dictionary_set(dict, "k", c2) : ccol_success;
  ccol_retval rs = built ? cjson_set(dict, "k", 3) : ccol_success;
  g_mem.fail = false;
  size_t blocks_after = g_mem.live_blocks;

  bool still_empty =
      built && cjson_list_size(list) == 0 && cjson_dictionary_size(dict) == 0;
  bool usable = built &&
                cjson_list_push(list, cjson_create_int_mp(4, &g_mem_mp)) ==
                    ccol_success &&
                cjson_set(dict, "k", 5) == ccol_success &&
                cjson_list_size(list) == 1 && cjson_dictionary_size(dict) == 1;

  cjson_destroy(list);
  cjson_destroy(dict);
  size_t leaked = g_mem.live_blocks;
  REQUIRE_TRUE(built);
  REQUIRE_EQ(rl, ccol_not_enough_memory);
  REQUIRE_EQ(rd, ccol_not_enough_memory);
  REQUIRE_EQ(rs, ccol_not_enough_memory);
  REQUIRE_EQ(blocks_after, blocks_before - 2);
  REQUIRE_TRUE(still_empty);
  REQUIRE_TRUE(usable);
  REQUIRE_EQ(leaked, (size_t)0);
}

// Indentation is written in 128-byte blocks; widths around a block boundary
// must give exactly depth * indent spaces.
TEST(serialize, pretty_indentation_is_exact_at_every_width) {
  static const unsigned widths[] = {1,   2,   3,   63,  64,  65, 127,
                                    128, 129, 255, 256, 257, 300};
  bool all_ok = true;
  for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
    unsigned indent = widths[w];
    cjson doc = cjson_parse("{\"a\":[1,{\"b\":2}]}", NULL);
    char *out = doc ? cjson_serialize_pretty(doc, indent) : NULL;
    static const struct {
      unsigned depth;
      const char *text;
    } lines[] = {{0, "{"},        {1, "\"a\": ["}, {2, "1,"}, {2, "{"},
                 {3, "\"b\": 2"}, {2, "}"},        {1, "]"},  {0, "}"}};
    size_t cap = 8 * (3 * 300 + 16);
    char *want = malloc(cap);
    size_t n = 0;
    for (size_t i = 0; want && i < sizeof(lines) / sizeof(lines[0]); i++) {
      if (i > 0) want[n++] = '\n';
      for (unsigned s = 0; s < lines[i].depth * indent; s++) want[n++] = ' ';
      size_t tl = strlen(lines[i].text);
      memcpy(want + n, lines[i].text, tl);
      n += tl;
    }
    if (want) want[n] = '\0';
    if (!out || !want || strcmp(out, want) != 0) all_ok = false;
    free(want);
    cjson_serialize_free(out);
    cjson_destroy(doc);
  }
  REQUIRE_TRUE(all_ok);
}

// An allocator that refuses every call while get_refuse is set and counts the
// refused calls, for the test that a lookup allocates nothing.
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

TEST(custom_alloc, a_get_by_path_allocates_nothing) {
  ccol_memmgmt_procs mp = {get_refuse_malloc, get_refuse_free,
                           get_refuse_calloc, get_refuse_realloc};
  atomic_store(&get_refuse, false);
  cjson doc = cjson_parse_mp("{\"a\":{\"b\":1}}", NULL, &mp);
  atomic_store(&get_refused_calls, 0);
  atomic_store(&get_refuse, true);
  cjson found = doc ? cjson_get(doc, "a.b") : NULL;
  long calls = atomic_load(&get_refused_calls);
  atomic_store(&get_refuse, false);
  cjson_destroy(doc);
  REQUIRE_NE((void *)found, NULL);
  REQUIRE_EQ(calls, 0L);
}

// An allocator that counts every allocation in alloc_tally, for the allocation
// count tests.
static atomic_long alloc_tally;

static void *tally_malloc(size_t n) {
  atomic_fetch_add(&alloc_tally, 1);
  return malloc(n);
}
static void *tally_calloc(size_t a, size_t b) {
  atomic_fetch_add(&alloc_tally, 1);
  return calloc(a, b);
}
static void *tally_realloc(void *p, size_t n) {
  atomic_fetch_add(&alloc_tally, 1);
  return realloc(p, n);
}
static ccol_memmgmt_procs tally_mp = {tally_malloc, free, tally_calloc,
                                      tally_realloc};

// Parses text with the tallying allocator and gives the number of allocations
// the parse made, or -1 when it fails.
static long allocations_of_parse(const char *text) {
  atomic_store(&alloc_tally, 0);
  cjson doc = cjson_parse_mp(text, NULL, &tally_mp);
  long n = atomic_load(&alloc_tally);
  if (!doc) return -1;
  cjson_destroy(doc);
  return n;
}

// With a custom allocator (no pool) a node is one allocation and a list with an
// element adds its vector block and buffer. A vector that copied the interned
// procs would add a third, and [[1],[2],[3]] would cost 19 instead of 15.
TEST(alloc_count, a_parsed_list_costs_its_node_and_two_allocations) {
  REQUIRE_EQ(allocations_of_parse("[[1],[2],[3]]"), 7L + 4L * 2L);
  REQUIRE_EQ(allocations_of_parse("[[1],[2],[3],[4]]") -
                 allocations_of_parse("[[1],[2],[3]]"),
             2L + 2L);
}

// Keys are parsed into the parser's own buffer and copied by the map, so a
// member costs its node and its map entry only.
TEST(alloc_count, a_parsed_key_costs_no_allocation) {
  long one = allocations_of_parse("{\"a\":1}");
  long two = allocations_of_parse("{\"a\":1,\"b\":2}");
  REQUIRE_GT(one, 0L);
  REQUIRE_EQ(two - one, 2L);
}

TEST(alloc_count, long_escaped_and_nested_keys_parse_correctly) {
  char big[700];
  memset(big, 'k', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  size_t cap = 40 * 310 + sizeof(big) + 256;
  char *text = malloc(cap);
  REQUIRE_NE((void *)text, NULL);
  size_t pos = 0;
  for (int i = 0; i < 40; i++) {
    text[pos++] = '{';
    text[pos++] = '"';
    memset(text + pos, (char)('a' + i % 26), 300);
    pos += 300;
    text[pos++] = '"';
    text[pos++] = ':';
  }
  const char *tail = "{\"e\\u0041\\n\":7,\"";
  memcpy(text + pos, tail, strlen(tail));
  pos += strlen(tail);
  memcpy(text + pos, big, strlen(big));
  pos += strlen(big);
  memcpy(text + pos, "\":8}", 4);
  pos += 4;
  for (int i = 0; i < 40; i++) text[pos++] = '}';
  text[pos] = '\0';

  cjson doc = cjson_parse_mp(text, NULL, &tally_mp);
  free(text);
  REQUIRE_NE((void *)doc, NULL);
  cjson cur = doc;
  char key[301];
  key[300] = '\0';
  for (int i = 0; i < 40 && cur; i++) {
    memset(key, (char)('a' + i % 26), 300);
    cur = cjson_dictionary_get(cur, key);
  }
  cjson e = cur ? cjson_dictionary_get(cur, "eA\n") : NULL;
  cjson b = cur ? cjson_dictionary_get(cur, big) : NULL;
  long ev = e ? cjson_int_val(e) : -1;
  long bv = b ? cjson_int_val(b) : -1;
  cjson_destroy(doc);
  REQUIRE_EQ(ev, 7L);
  REQUIRE_EQ(bv, 8L);
}

// A clone copies strings that are already valid UTF-8 without checking them
// again; a check would raise the UTF-8 counter by the number of strings.
TEST(alloc_count, a_clone_does_not_check_its_strings_again) {
  cjson doc = cjson_parse(
      "{\"a\":\"x\",\"b\":[\"y\",\"z\"],\"c\":{\"d\":\"w\"}}", NULL);
  REQUIRE_NE((void *)doc, NULL);
  unsigned long before = atomic_load(&cjson__utf8_cstr_checks_for_tests);
  cjson copy = cjson_clone(doc);
  unsigned long after = atomic_load(&cjson__utf8_cstr_checks_for_tests);
  char *a = copy ? cjson_serialize(doc) : NULL;
  char *b = copy ? cjson_serialize(copy) : NULL;
  bool copied = copy != NULL;
  bool same = a && b && strcmp(a, b) == 0;
  cjson_serialize_free(a);
  cjson_serialize_free(b);
  cjson_destroy(copy);
  cjson_destroy(doc);
  REQUIRE_TRUE(copied);
  REQUIRE_TRUE(same);
  REQUIRE_EQ(after - before, 0UL);
}

TEST(navigate, a_malformed_middle_component_is_invalid_args) {
  cjson doc = cjson_parse("{\"l\":[{\"k\":1}],\"d\":{\"k\":2}}", NULL);
  REQUIRE_NE((void *)doc, NULL);
  const char *bad[] = {"l.#x.k", "l.#-1.k",  "l.#.k",         "l.#+0.k",
                       "\xff.k", "d.\xc3.k", "missing.\xff.k"};
  ccol_retval set_rv[7], del_rv[7];
  bool got_null = true;
  for (size_t i = 0; i < 7; i++) {
    set_rv[i] = cjson_set(doc, bad[i], 5);
    del_rv[i] = cjson_remove(doc, bad[i]);
    if (cjson_get(doc, bad[i]) != NULL) got_null = false;
  }
  ccol_retval absent_key = cjson_set(doc, "d.#x.k", 5);
  ccol_retval absent_idx = cjson_remove(doc, "l.#9.k");
  ccol_retval ok = cjson_set(doc, "l.#0.k", 3);
  cjson_destroy(doc);
  for (size_t i = 0; i < 7; i++) {
    REQUIRE_EQ(set_rv[i], ccol_invalid_args);
    REQUIRE_EQ(del_rv[i], ccol_invalid_args);
  }
  REQUIRE_TRUE(got_null);
  REQUIRE_EQ(absent_key, ccol_key_not_found);
  REQUIRE_EQ(absent_idx, ccol_key_not_found);
  REQUIRE_EQ(ok, ccol_success);
}

TEST(navigate, a_path_whose_shape_cannot_apply_is_invalid_args) {
  cjson doc = cjson_parse(
      "{\"s\":\"x\",\"n\":1,\"b\":true,\"z\":null,"
      "\"l\":[{\"k\":1}],\"d\":{\"k\":2}}",
      NULL);
  REQUIRE_NE((void *)doc, NULL);
  const char *at_leaf[] = {"s.k", "n.#0", "b.k", "z.k", "l.k"};
  const char *in_middle[] = {"s.k.x", "n.#0.x", "b.k.x",    "z.k.x",
                             "l.k.x", "d.k.x",  "l.#0.k.x", "s.k.#0.y"};
  enum { N_LEAF = 5, N_MID = 8 };
  ccol_retval leaf_set[N_LEAF], leaf_del[N_LEAF];
  ccol_retval mid_set[N_MID], mid_del[N_MID];
  bool got_null = true;
  for (size_t i = 0; i < N_LEAF; i++) {
    leaf_set[i] = cjson_set(doc, at_leaf[i], 5);
    leaf_del[i] = cjson_remove(doc, at_leaf[i]);
    if (cjson_get(doc, at_leaf[i]) != NULL) got_null = false;
  }
  for (size_t i = 0; i < N_MID; i++) {
    mid_set[i] = cjson_set(doc, in_middle[i], 5);
    mid_del[i] = cjson_remove(doc, in_middle[i]);
    if (cjson_get(doc, in_middle[i]) != NULL) got_null = false;
  }
  ccol_retval absent_mid_key = cjson_set(doc, "d.missing.x", 5);
  ccol_retval absent_mid_idx = cjson_remove(doc, "l.#7.k");
  ccol_retval absent_deep = cjson_remove(doc, "missing.k.x");
  ccol_retval absent_leaf_key = cjson_remove(doc, "d.missing");
  ccol_retval absent_leaf_idx = cjson_set(doc, "l.#7", 5);
  bool absent_get_null =
      cjson_get(doc, "d.missing.x") == NULL && cjson_get(doc, "l.#7.k") == NULL;
  cjson s = cjson_get(doc, "s");
  bool tree_kept = s != NULL && cjson_get(doc, "d.k") != NULL &&
                   cjson_get(doc, "l.#0.k") != NULL &&
                   cjson_get(doc, "n") != NULL;
  cjson_destroy(doc);
  for (size_t i = 0; i < N_LEAF; i++) {
    REQUIRE_EQ(leaf_set[i], ccol_invalid_args);
    REQUIRE_EQ(leaf_del[i], ccol_invalid_args);
  }
  for (size_t i = 0; i < N_MID; i++) {
    REQUIRE_EQ(mid_set[i], ccol_invalid_args);
    REQUIRE_EQ(mid_del[i], ccol_invalid_args);
  }
  REQUIRE_TRUE(got_null);
  REQUIRE_EQ(absent_mid_key, ccol_key_not_found);
  REQUIRE_EQ(absent_mid_idx, ccol_key_not_found);
  REQUIRE_EQ(absent_deep, ccol_key_not_found);
  REQUIRE_EQ(absent_leaf_key, ccol_key_not_found);
  REQUIRE_EQ(absent_leaf_idx, ccol_key_not_found);
  REQUIRE_TRUE(absent_get_null);
  REQUIRE_TRUE(tree_kept);
}
