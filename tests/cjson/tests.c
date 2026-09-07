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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <time.h>

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
  cjson a = cjson_create_list();
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_type(a), CJSON_LIST);
  REQUIRE_EQ(cjson_list_len(a), (size_t)0);
  cjson_destroy(a);
}

TEST(construction, array_push) {
  cjson a = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(a, cjson_create_int(1)), ccol_success);
  REQUIRE_EQ(cjson_list_push(a, cjson_create_int(2)), ccol_success);
  REQUIRE_EQ(cjson_list_push(a, cjson_create_int(3)), ccol_success);
  REQUIRE_EQ(cjson_list_len(a), (size_t)3);
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

/* ========================================================================== */
/*    OWNERSHIP; cjson_list_push / cjson_dictionary_set alias rejection     */
/* ========================================================================== */

/*
 * cjson_list_push()/cjson_dictionary_set() take unconditional ownership of
 * their child argument.  Before this guard, handing the same already-owned
 * node to a second container slot (including via cjson_get()/cjson_list_get()
 * /cjson_dictionary_get()'s borrowed references, or a bare self-reference)
 * gave that node two owners; each owner's own teardown independently
 * destroyed it, corrupting the heap; confirmed to corrupt the default
 * allocator's thread-local node-pool free-list into a self-referencing
 * cycle (hanging the pool's own process-exit drain) and to segfault directly
 * under a custom allocator.  These tests cover every reachable variant of
 * that hazard and confirm the tree is left completely valid, and the
 * offending call reports ccol_invalid_args, rather than corrupting anything.
 */

TEST(ownership, dictionary_set_key_to_its_own_current_value_is_noop) {
  /* Setting a key to its own current value (e.g. a "no-op refresh" pattern)
   * must succeed rather than being treated as an illegal re-parent. */
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
  /* Rejected, not corrupted: exactly one slot, and item is still readable
   * through it. */
  REQUIRE_EQ(cjson_list_len(arr), (size_t)1);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(arr, 0)), 7LL);
  cjson_destroy(arr);
}

TEST(ownership, list_push_node_already_in_another_list_rejected) {
  cjson arr1 = cjson_create_list();
  cjson arr2 = cjson_create_list();
  cjson item = cjson_create_int(3);
  REQUIRE_EQ(cjson_list_push(arr1, item), ccol_success);
  REQUIRE_EQ(cjson_list_push(arr2, item), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(arr1), (size_t)1);
  REQUIRE_EQ(cjson_list_len(arr2), (size_t)0);
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
  /* The most natural way to trigger the hazard: taking a borrowed reference
   * from cjson_dictionary_get and handing it to a different container. */
  cjson o = cjson_create_dictionary();
  cjson arr = cjson_create_list();
  REQUIRE_EQ(cjson_dictionary_set(o, "k", cjson_create_string("owned")),
             ccol_success);
  cjson borrowed = cjson_dictionary_get(o, "k");
  REQUIRE_EQ(cjson_list_push(arr, borrowed), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(arr), (size_t)0);
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
  REQUIRE_EQ(cjson_list_len(arr), (size_t)0);
  cjson_destroy(arr);
}

TEST(ownership, dictionary_set_self_rejected) {
  cjson o = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(o, "self", o), ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)0);
  cjson_destroy(o);
}

TEST(ownership, parsed_child_cannot_be_repushed_elsewhere) {
  /* A node produced by cjson_parse() is just as "attached" as one built
   * directly through the public API; the guard must not be bypassable by
   * routing the child through the parser first. */
  cjson root = cjson_parse("{\"items\":[1,2,3]}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson items = cjson_get(root, "items");
  cjson elem = cjson_list_get(items, 0);
  cjson other = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(other, elem), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(other), (size_t)0);
  REQUIRE_EQ(cjson_list_len(items), (size_t)3);
  cjson_destroy(root);
  cjson_destroy(other);
}

TEST(ownership, clone_produces_independently_pushable_node) {
  /* cjson_clone() must produce a genuinely fresh, unattached node; a
   * cloned child is a completely legitimate cjson_list_push()/
   * cjson_dictionary_set() argument, unlike the borrowed original. */
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

TEST(ownership, list_push_ancestor_into_own_descendant_rejected) {
  /* `root` is never itself attached to anything (it IS the root), so the
   * pre-existing `attached` guard alone does not reject pushing it into
   * `child`, one of its own already-attached descendants; doing so would
   * create a graph cycle (root -> child -> root) that corrupts
   * __cjson_destroy()'s own worklist-driven teardown into a double free.
   * Directly reproduces the double-free confirmed via glibc's own
   * "double free detected in tcache" abort before this check was added. */
  cjson root = cjson_create_dictionary();
  cjson child = cjson_create_list();
  REQUIRE_EQ(cjson_dictionary_set(root, "self", child), ccol_success);
  REQUIRE_EQ(cjson_list_push(child, root), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(child), (size_t)0);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  REQUIRE_EQ((void *)cjson_dictionary_get(root, "self"), (void *)child);
  cjson_destroy(root);
}

TEST(ownership, dictionary_set_ancestor_into_own_descendant_rejected) {
  /* Same hazard as list_push_ancestor_into_own_descendant_rejected, with the
   * roles of list/dictionary swapped for both the ancestor and the
   * descendant, confirming the guard is not accidentally type-specific. */
  cjson root = cjson_create_list();
  cjson child = cjson_create_dictionary();
  REQUIRE_EQ(cjson_list_push(root, child), ccol_success);
  REQUIRE_EQ(cjson_dictionary_set(child, "back", root), ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_size(child), (size_t)0);
  REQUIRE_EQ(cjson_list_len(root), (size_t)1);
  REQUIRE_EQ((void *)cjson_list_get(root, 0), (void *)child);
  cjson_destroy(root);
}

TEST(ownership, list_push_multi_level_ancestor_into_descendant_rejected) {
  /* The cycle need not be a direct 2-node loop: an ancestor several levels
   * up the tree, pushed into a deeply-nested descendant, must be caught the
   * same way; node_reaches() walks the whole subtree, not just the
   * immediate children. */
  cjson root = cjson_create_list();
  cjson a = cjson_create_list();
  cjson b = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(root, a), ccol_success);
  REQUIRE_EQ(cjson_list_push(a, b), ccol_success);
  REQUIRE_EQ(cjson_list_push(b, root), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(b), (size_t)0);
  REQUIRE_EQ(cjson_list_len(a), (size_t)1);
  REQUIRE_EQ(cjson_list_len(root), (size_t)1);
  cjson_destroy(root);
}

TEST(ownership,
     list_push_large_child_into_already_attached_target_still_succeeds) {
  /* node_reaches()'s O(1) needle->attached short-circuit only applies when
   * the target is NOT yet attached; once it is (as here), the full subtree
   * search runs and must still correctly conclude "no cycle" for a
   * genuinely acyclic push, rather than false-positive-rejecting it. */
  cjson root = cjson_create_list();
  cjson holder = cjson_create_list();
  REQUIRE_EQ(cjson_list_push(root, holder), ccol_success); /* holder attached */

  cjson big = cjson_create_list();
  for (int i = 0; i < 500; i++)
    REQUIRE_EQ(cjson_list_push(big, cjson_create_int(i)), ccol_success);

  REQUIRE_EQ(cjson_list_push(holder, big), ccol_success);
  REQUIRE_EQ(cjson_list_len(holder), (size_t)1);
  REQUIRE_EQ(cjson_list_len(big), (size_t)500);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                         PARSING - SCALARS                                  */
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
  /* RFC 8259 section 6: a leading zero may not be followed by more digits. */
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

TEST(parse, lone_high_surrogate_at_end_of_string) {
  /* A high surrogate with nothing following it at all has no valid low
   * surrogate to pair with; substitute U+FFFD, same as a lone low
   * surrogate. */
  cjson n = cjson_parse("\"\\uD800\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  const char *s = cjson_str_val(n);
  REQUIRE_EQ((unsigned char)s[0], 0xEFu);
  REQUIRE_EQ((unsigned char)s[1], 0xBFu);
  REQUIRE_EQ((unsigned char)s[2], 0xBDu);
  REQUIRE_EQ(s[3], '\0');
  cjson_destroy(n);
}

TEST(parse, high_surrogate_followed_by_ordinary_escape_not_swallowed) {
  /* A lone high surrogate immediately followed by a \uXXXX escape that is
   * NOT itself a low surrogate must not consume that following escape:
   * both characters must survive independently (U+FFFD for the lone high
   * surrogate, then the ordinary escaped character on its own), never
   * collapsing into a single replacement character that silently drops
   * the second one. */
  cjson n = cjson_parse("\"\\uD800\\u0041\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  const char *s = cjson_str_val(n);
  REQUIRE_EQ(strlen(s), (size_t)4);
  REQUIRE_EQ((unsigned char)s[0], 0xEFu);
  REQUIRE_EQ((unsigned char)s[1], 0xBFu);
  REQUIRE_EQ((unsigned char)s[2], 0xBDu);
  REQUIRE_EQ(s[3], 'A');
  REQUIRE_EQ(s[4], '\0');
  cjson_destroy(n);
}

TEST(parse, high_surrogate_followed_by_another_high_surrogate) {
  /* Two consecutive lone high surrogates: neither one is a valid low
   * surrogate for the other, so both must independently become their own
   * U+FFFD rather than the pair collapsing into a single replacement
   * character. */
  cjson n = cjson_parse("\"\\uD800\\uD800\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  const char *s = cjson_str_val(n);
  REQUIRE_EQ(strlen(s), (size_t)6);
  REQUIRE_EQ((unsigned char)s[0], 0xEFu);
  REQUIRE_EQ((unsigned char)s[1], 0xBFu);
  REQUIRE_EQ((unsigned char)s[2], 0xBDu);
  REQUIRE_EQ((unsigned char)s[3], 0xEFu);
  REQUIRE_EQ((unsigned char)s[4], 0xBFu);
  REQUIRE_EQ((unsigned char)s[5], 0xBDu);
  REQUIRE_EQ(s[6], '\0');
  cjson_destroy(n);
}

TEST(parse, high_surrogate_then_valid_pair_afterward_both_preserved) {
  /* A lone high surrogate followed by a genuine, independent surrogate
   * pair: the first must become U+FFFD without disturbing the pair that
   * follows it. */
  cjson n = cjson_parse("\"\\uD800\\uD83D\\uDE00\"", NULL);
  REQUIRE_NE((void *)n, NULL);
  const char *s = cjson_str_val(n);
  REQUIRE_EQ(strlen(s), (size_t)7);
  REQUIRE_EQ((unsigned char)s[0], 0xEFu);
  REQUIRE_EQ((unsigned char)s[1], 0xBFu);
  REQUIRE_EQ((unsigned char)s[2], 0xBDu);
  REQUIRE_EQ((unsigned char)s[3], 0xF0u);
  REQUIRE_EQ((unsigned char)s[4], 0x9Fu);
  REQUIRE_EQ((unsigned char)s[5], 0x98u);
  REQUIRE_EQ((unsigned char)s[6], 0x80u);
  REQUIRE_EQ(s[7], '\0');
  cjson_destroy(n);
}

TEST(parse, null_byte_in_string_rejected) {
  /* \u0000 encodes a null byte which cannot be stored in a null-terminated C
   * string; the parser must reject it rather than silently truncate. */
  REQUIRE_EQ((void *)cjson_parse("\"\\u0000\"", NULL), NULL);
  REQUIRE_EQ((void *)cjson_parse("\"hello\\u0000world\"", NULL), NULL);
}

TEST(parse, unknown_escape_rejected) {
  /* RFC 8259 section 7 only allows \", \\, \/, \b, \f, \n, \r, \t, \uXXXX. */
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
/*                         PARSING - COMPOSITE                                */
/* ========================================================================== */

TEST(parse, empty_array) {
  cjson a = cjson_parse("[]", NULL);
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_type(a), CJSON_LIST);
  REQUIRE_EQ(cjson_list_len(a), (size_t)0);
  cjson_destroy(a);
}

TEST(parse, array_of_mixed) {
  cjson a = cjson_parse("[null, true, 1, 2.5, \"x\"]", NULL);
  REQUIRE_NE((void *)a, NULL);
  REQUIRE_EQ(cjson_list_len(a), (size_t)5);
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
  REQUIRE_EQ(cjson_list_len(users), (size_t)2);
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
  REQUIRE_EQ(cjson_list_len(cjson_dictionary_get(o, "k")), (size_t)2);
  cjson_destroy(o);
}

/* ========================================================================== */
/*                         PARSING - ERROR CASES                              */
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
  /* RFC 8259 section 6 requires at least one digit after '-', after '.', and
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
  REQUIRE_EQ(cjson_list_len(arr), (size_t)3);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(arr, 1)), 20LL);

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
  /* Non-ASCII UTF-8 bytes must survive serialize -> parse unchanged. */
  const char *original =
      "caf\xC3\xA9"; /* "cafe" with an accented e, UTF-8 encoded */
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
  /* Key must still be present - cjson_type(NULL) == CJSON_NULL would make a
   * bare type check pass even if the key were accidentally removed. */
  cjson null_node = cjson_get(root, "k");
  REQUIRE_NE((void *)null_node, NULL);
  REQUIRE_EQ(cjson_type(null_node), CJSON_NULL);
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
  /* A syntactically valid but out-of-range "#N" index addresses an absent
   * element, not a malformed path: this must report ccol_key_not_found
   * (matching _cjson_delete's own identical classification of the same
   * situation), not ccol_invalid_args. */
  cjson root = cjson_parse("{\"arr\":[1,2]}", NULL);
  ccol_retval_t r = cjson_set(root, "arr.#99", 5);
  REQUIRE_EQ(r, ccol_key_not_found);
  cjson_destroy(root);
}

TEST(navigate, set_array_index_malformed_is_invalid_args) {
  /* Unlike a valid-but-out-of-range index, a malformed one (non-numeric,
   * negative, or bare '#') is a genuine syntax error. */
  cjson root = cjson_parse("{\"arr\":[1,2]}", NULL);
  REQUIRE_EQ(cjson_set(root, "arr.#abc", 5), ccol_invalid_args);
  REQUIRE_EQ(cjson_set(root, "arr.#-1", 5), ccol_invalid_args);
  cjson_destroy(root);
}

TEST(navigate, array_index_with_leading_whitespace_or_sign_is_invalid_args) {
  /* Regression test: a bare strtol() call tolerates leading whitespace and
   * an explicit '+' sign before the digits of a "#N" index, which would
   * otherwise silently accept "#  1"/"#+1" as well-formed indices instead
   * of rejecting them the way "#abc"/"#-1" are already rejected above.
   * Covers cjson_get (via navigate()), cjson_set, and cjson_delete, since
   * all three parse a "#N" component independently. */
  cjson root = cjson_parse("{\"arr\":[10,20,30]}", NULL);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ((void *)cjson_get(root, "arr.#  1"), NULL);
  REQUIRE_EQ((void *)cjson_get(root, "arr.# 1"), NULL);
  REQUIRE_EQ((void *)cjson_get(root, "arr.#+1"), NULL);

  REQUIRE_EQ(cjson_set(root, "arr.#  1", 99), ccol_invalid_args);
  REQUIRE_EQ(cjson_set(root, "arr.#+1", 99), ccol_invalid_args);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "arr.#1")), 20LL);

  REQUIRE_EQ(_cjson_delete(root, "arr.#  1"), ccol_invalid_args);
  REQUIRE_EQ(_cjson_delete(root, "arr.#+1"), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(cjson_get(root, "arr")), (size_t)3);

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
  /* An empty INTERMEDIATE path component ("a..b") is a syntax error, just
   * like an empty LEAF component ("a.", covered by
   * set_empty_path_returns_invalid_args-style tests elsewhere): both must
   * report ccol_invalid_args, not be conflated with an ordinary absent
   * (but syntactically valid) component. */
  cjson root = cjson_parse("{\"a\":{\"b\":0}}", NULL);
  ccol_retval_t r = cjson_set(root, "a..b", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a.b")), 0LL);
  cjson_destroy(root);
}

TEST(navigate, set_trailing_dot_with_nonexistent_parent_still_invalid_args) {
  /* Regression test: a trailing dot (empty leaf component) must be reported
   * as ccol_invalid_args unconditionally, even when the parent path itself
   * also does not exist; the leaf's own syntax must be validated before
   * the parent is ever navigated to, so a genuinely absent parent never
   * masks a leaf syntax error as ccol_key_not_found. */
  cjson root = cjson_parse("{\"a\":1}", NULL);
  ccol_retval_t r = cjson_set(root, "missing.", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "missing"), NULL);
  cjson_destroy(root);
}

TEST(navigate,
     consecutive_dots_set_with_nonexistent_first_component_still_invalid_args) {
  /* Regression test: an empty INTERMEDIATE component ("missing..b") must
   * still be reported as ccol_invalid_args even when the component before
   * it ("missing") does not exist either. */
  cjson root = cjson_parse("{\"a\":1}", NULL);
  ccol_retval_t r = cjson_set(root, "missing..b", 99);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "missing"), NULL);
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
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 99LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 10LL);
  cjson_destroy(root);
}

TEST(navigate, set_float_literal) {
  /* cjson_set with a 4-byte float exercises the sizeof(float) branch in
   * node_reinit_scalar - distinct from the double path. */
  cjson root = cjson_parse("{\"v\":0}", NULL);
  float f = 2.5f;
  cjson_set(root, "v", f);
  REQUIRE_EQ(cjson_type(cjson_get(root, "v")), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(cjson_get(root, "v")), 2.5);
  cjson_destroy(root);
}

TEST(navigate, set_list_slot_composite_to_scalar) {
  /* Replacing a list element that is itself a composite node (dictionary)
   * with a scalar must deep-free the composite via node_clear and leave
   * adjacent elements intact. */
  cjson root = cjson_parse("{\"items\":[{\"a\":1},\"two\",{\"b\":2}]}", NULL);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval_t r = cjson_set(root, "items.#0", 99);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cjson_type(cjson_get(root, "items.#0")), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "items.#0")), 99LL);
  REQUIRE_STREQ(cjson_str_val(cjson_get(root, "items.#1")), "two");
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "items.#2.b")), 2LL);
  cjson_destroy(root);
}

TEST(navigate, leading_dot_get_returns_null) {
  /* A path starting with '.' has an empty leading component; cjson_get
   * must return NULL cleanly. */
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ((void *)cjson_get(root, ".a"), NULL);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(navigate, leading_dot_set_returns_invalid_args) {
  /* A leading dot is a path syntax error; cjson_set must return
   * ccol_invalid_args and leave the tree unmodified. */
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

/* ========================================================================== */
/*                         TYPE SAFETY - _cjson_type_of                      */
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

/* ========================================================================== */
/*                              FUZZY TESTS                                   */
/* ========================================================================== */

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

TEST(fuzzy, plain_char_negative_sign_extension) {
  /* "Plain" char's own signedness is implementation-defined by the C
   * standard, not guaranteed signed: x86/x86_64's ABI makes it signed,
   * but the standard ARM AAPCS64 ABI (aarch64) makes it UNSIGNED, so
   * `char c = -1;` holds a genuinely different value (255, not -1) there;
   * not a bug, just a different platform convention. The comparison
   * must therefore widen the SAME `c` the library was given, following
   * whatever this platform's own char signedness naturally produces,
   * rather than hardcoding the x86-specific assumption that plain char
   * is always signed. */
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
  /* Key must still be present - same masking risk as set_null above. */
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
  ccol_retval_t r = cjson_set(root, "v", INFINITY);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_type(cjson_get(root, "v")), CJSON_FLOAT);
  REQUIRE_EQ(cjson_double_val(cjson_get(root, "v")), 1.5);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                       PATH ESCAPE SEQUENCE TESTS                           */
/* ========================================================================== */

TEST(navigate, get_escaped_dot_flat_key) {
  /* Key is "a.b" (contains a literal dot).  Access it with "a\\.b". */
  cjson root = cjson_parse("{\"a.b\":42}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "a\\.b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 42LL);
  /* Unescaped dot must NOT find the key. */
  REQUIRE_EQ((void *)cjson_get(root, "a.b"), NULL);
  cjson_destroy(root);
}

TEST(navigate, get_escaped_backslash_flat_key) {
  /* Key is "a\\b" (contains a literal backslash).  Access it with "a\\\\b". */
  cjson root = cjson_parse("{\"a\\\\b\":7}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "a\\\\b");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 7LL);
  cjson_destroy(root);
}

TEST(navigate, get_escaped_dot_nested_path) {
  /* Outer key is plain "outer"; inner key is "k.ey" (literal dot). */
  cjson root = cjson_parse("{\"outer\":{\"k.ey\":99}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "outer.k\\.ey");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 99LL);
  cjson_destroy(root);
}

TEST(navigate, get_escaped_dot_both_components) {
  /* Both levels have dot-containing keys: "a.b" -> "c.d". */
  cjson root = cjson_parse("{\"a.b\":{\"c.d\":1}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  cjson node = cjson_get(root, "a\\.b.c\\.d");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 1LL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_dot_creates_new_key) {
  /* Create a new key "x.y" (literal dot) at the root. */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "x\\.y", 55), ccol_success);
  cjson node = cjson_get(root, "x\\.y");
  REQUIRE_NE((void *)node, NULL);
  REQUIRE_EQ(cjson_int_val(node), 55LL);
  /* Must not have created a spurious nested "x" key. */
  REQUIRE_EQ((void *)cjson_get(root, "x"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_dot_updates_existing_key) {
  /* Update an existing key "p.q" (literal dot). */
  cjson root = cjson_parse("{\"p.q\":0}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "p\\.q", 123), ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "p\\.q")), 123LL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_dot_nested_path) {
  /* Set "outer"."k.ey" via escaped path "outer.k\\.ey". */
  cjson root = cjson_parse("{\"outer\":{\"k.ey\":0}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "outer.k\\.ey", 77), ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "outer.k\\.ey")), 77LL);
  cjson_destroy(root);
}

TEST(navigate, set_escaped_backslash_creates_key) {
  /* Create a key "a\\b" (literal backslash) via "a\\\\b". */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "a\\\\b", 9), ccol_success);
  REQUIRE_NE((void *)cjson_get(root, "a\\\\b"), NULL);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a\\\\b")), 9LL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                         CLONE REGRESSION TESTS                             */
/* ========================================================================== */

TEST(clone, flat_array) {
  cjson orig = cjson_create_list();
  cjson_list_push(orig, cjson_create_int(10));
  cjson_list_push(orig, cjson_create_int(20));
  cjson_list_push(orig, cjson_create_string("hi"));

  cjson copy = cjson_clone(orig);
  REQUIRE_NE((void *)copy, (void *)orig);
  REQUIRE_EQ(cjson_type(copy), CJSON_LIST);
  REQUIRE_EQ(cjson_list_len(copy), (size_t)3);
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

/* ========================================================================== */
/*                 OWNERSHIP CONTRACT - EARLY INVALID_ARGS PATHS              */
/* ========================================================================== */

TEST(construction, array_push_null_arr_frees_child) {
  ccol_retval_t r = cjson_list_push(NULL, cjson_create_int(42));
  REQUIRE_EQ(r, ccol_invalid_args);
}

TEST(construction, array_push_null_child_rejected) {
  cjson arr = cjson_create_list();
  ccol_retval_t r = cjson_list_push(arr, NULL);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(arr), (size_t)0);
  cjson_destroy(arr);
}

TEST(construction, array_push_wrong_type_frees_child) {
  cjson not_array = cjson_create_int(7);
  ccol_retval_t r = cjson_list_push(not_array, cjson_create_string("hi"));
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson_destroy(not_array);
}

TEST(construction, object_set_null_obj_frees_child) {
  ccol_retval_t r = cjson_dictionary_set(NULL, "k", cjson_create_int(1));
  REQUIRE_EQ(r, ccol_invalid_args);
}

TEST(construction, object_set_wrong_type_frees_child) {
  cjson not_obj = cjson_create_string("oops");
  ccol_retval_t r = cjson_dictionary_set(not_obj, "k", cjson_create_int(99));
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson_destroy(not_obj);
}

TEST(construction, object_set_null_key_frees_child) {
  cjson o = cjson_create_dictionary();
  ccol_retval_t r = cjson_dictionary_set(o, NULL, cjson_create_int(1));
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_dictionary_size(o), (size_t)0);
  cjson_destroy(o);
}

/*
 * Regression tests: an early ccol_invalid_args reject caused by an invalid
 * arr/obj/key must never destroy an already-attached child, even though a
 * freshly unattached child IS destroyed on that same path (see the
 * "_frees_child" tests above). Before this fix, cjson_list_push()'s and
 * cjson_dictionary_set()'s own arr/obj-validity checks ran, and destroyed
 * child, BEFORE the child->attached guard ever had a chance to run;
 * silently freeing memory a real owner elsewhere in the tree still held a
 * pointer to, corrupting that tree the moment it was next touched or
 * destroyed. Confirmed via a direct revert of the fix: each REQUIRE_EQ
 * below failed with a heap-use-after-free/corruption crash on the final
 * REQUIRE_STREQ/REQUIRE_EQ readback rather than merely returning the wrong
 * code.
 */

TEST(construction,
     array_push_wrong_type_leaves_already_attached_child_untouched) {
  cjson owner = cjson_create_dictionary();
  REQUIRE_EQ(cjson_dictionary_set(owner, "k", cjson_create_string("owned")),
             ccol_success);
  cjson borrowed = cjson_dictionary_get(owner, "k");

  cjson not_array = cjson_create_int(7);
  REQUIRE_EQ(cjson_list_push(not_array, borrowed), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_push(NULL, borrowed), ccol_invalid_args);

  /* borrowed must still be alive and still owned by owner. */
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

  /* borrowed must still be alive and still owned by owner. */
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

/* ========================================================================== */
/*                         CUSTOM ALLOCATOR                                   */
/* ========================================================================== */

/*
 * Tracking allocator - wraps the standard allocator and counts alloc/free
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

/* Single-fault allocator: fails exactly the g_single_fault_at'th call
 * (1-indexed) and lets every other call (before or after) succeed
 * normally.  Needed where the budget-style counting allocator above cannot
 * observe the result: once its budget hits zero it fails every subsequent
 * call too, leaving no headroom for something built AFTER the triggering
 * failure (e.g. a parse error message allocated once parsing itself has
 * already failed) to ever succeed. */
static int g_single_fault_at = -1; /* -1 = disabled */
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
static ccol_memmgmt_procs_t g_single_fault_mp = {
    .malloc = single_fault_malloc,
    .calloc = single_fault_calloc,
    .realloc = single_fault_realloc,
    .free = free};

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

  cjson arr = cjson_create_list_mp(&_tracking_alloc);
  REQUIRE_NE((void *)arr, NULL);
  REQUIRE_EQ(cjson_list_push(arr, cjson_create_int_mp(1, &_tracking_alloc)),
             ccol_success);
  REQUIRE_EQ(
      cjson_list_push(arr, cjson_create_string_mp("item", &_tracking_alloc)),
      ccol_success);
  REQUIRE_EQ(cjson_list_len(arr), (size_t)2);

  cjson obj = cjson_create_dictionary_mp(&_tracking_alloc);
  REQUIRE_NE((void *)obj, NULL);
  REQUIRE_EQ(cjson_dictionary_set(
                 obj, "key", cjson_create_bool_mp(false, &_tracking_alloc)),
             ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(obj), (size_t)1);

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

  cjson root = cjson_parse_mp("{\"a\":1,\"b\":\"hello\",\"c\":[1,2,3]}", NULL,
                              &_tracking_alloc);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_GT(_ta_allocs, (size_t)0);

  size_t allocs_at_destroy = _ta_allocs;
  (void)allocs_at_destroy;

  cjson_destroy(root);

  /* Every allocation must be matched by a free. */
  REQUIRE_EQ(_ta_allocs, _ta_frees);
}

TEST(custom_alloc, parse_failure_error_string_uses_custom_alloc_and_frees) {
  /* cjson_parse_mp()'s own doc comment: on failure, *err_str is allocated
   * through mp, and the caller must free it with cjson_serialize_free_mp(),
   * passing the same mp. Every other custom-allocator test in this file
   * either ignores err_str entirely or (see the single-fault OOM test
   * further down) frees it with plain free(), which cannot distinguish
   * "freed through mp" from "freed through libc directly" because that
   * particular mock's own .free happens to just be libc free(). _tracking_alloc
   * is used here specifically because its .malloc/.free are distinct
   * functions from libc's own that only call through to malloc()/free()
   * after bumping a counter, so a future regression that allocates or frees
   * the error string through the wrong allocator actually fails this test
   * instead of passing vacuously. */
  _ta_allocs = 0;
  _ta_frees = 0;

  char *err = NULL;
  cjson n = cjson_parse_mp("not valid json", &err, &_tracking_alloc);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);
  REQUIRE_GT(_ta_allocs, (size_t)0);

  size_t frees_before = _ta_frees;
  cjson_serialize_free_mp(err, &_tracking_alloc);
  REQUIRE_GT(_ta_frees, frees_before);
}

/* ========================================================================== */
/*      DICTIONARY-ITERATION OOM - chashmap_begin_iter's empty/OOM gap       */
/* ========================================================================== */

TEST(clone, dictionary_iterator_oom_never_returns_incomplete_clone) {
  /* chashmap_begin_iter() returns NULL both when a map is genuinely empty
   * and when its own small per-call allocation fails on a non-empty map (a
   * real OOM); naively treating both cases as "nothing to clone" would let
   * cjson_clone return a non-NULL "successful" clone silently missing every
   * key, violating its own documented "NULL on allocation failure"
   * contract. Exhaustively fails at every allocation budget from 0 up
   * through comfortably past this whole clone's real allocation count and
   * checks the invariant holds at each one: cjson_clone must never return a
   * non-NULL dictionary clone with fewer entries than the source. */
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
  /* Mirrors the clone test above for cjson_serialize(): a dictionary-
   * iterator allocation failure mid-serialization must flag the buffer as
   * OOM (cjson_serialize returns NULL) rather than silently emitting a
   * "successful" JSON object missing one or more of its entries. */
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

TEST(oom, dictionary_destroy_never_leaks_under_sustained_allocation_failure) {
  /* __cjson_destroy's own dictionary cleanup (node_clear) reaches every
   * child via chmap_destroy_with_dtor (chashmap.h), which walks the map's
   * own internal storage directly and therefore never needs to allocate to
   * do so, unlike enumerating the map via chashmap_begin_iter() first
   * (whose own small internal allocation can itself fail under sustained,
   * not merely transient, OOM). This test cannot itself detect a leak (tau
   * has no built-in leak checker); its purpose is to exercise this exact
   * teardown path under `make memtest` (valgrind), which already runs this
   * whole suite; a regression here is expected to be caught there, not by
   * any assertion in this function. */
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

TEST(oom, unreported_allocation_failure_reports_out_of_memory_not_unknown) {
  /* Every genuine syntax rejection in this parser reports a specific
   * diagnostic via parse_err() before returning failure; the only way to
   * reach parse_common's own fallback with ctx.error still empty is an
   * allocation failure with nowhere of its own to report through; here,
   * node_alloc()'s single _mem_calloc() call for the "null" literal's own
   * node, the very first (and, for this input, only) allocation the parse
   * would otherwise make. A single-fault allocator is required rather than
   * the budget-style counting one above: failing every call from a budget
   * onward would also fail the error message's own subsequent allocation,
   * making a non-NULL message structurally impossible to observe. Failing
   * only that first allocation must report an honest out-of-memory message,
   * not the misleading "unknown parse error" (which would suggest a
   * malformed document rather than memory pressure). */
  g_single_fault_counter = 0;
  g_single_fault_at = 1;
  char *err = NULL;
  cjson n = cjson_parse_mp("null", &err, &g_single_fault_mp);
  g_single_fault_at = -1;
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strstr(err, "out of memory") != NULL);
  REQUIRE_TRUE(strstr(err, "unknown parse error") == NULL);
  free(err);
}

/* ========================================================================== */
/*                         DELETE                                             */
/* ========================================================================== */

TEST(delete, list_remove_middle) {
  char *err = NULL;
  cjson root = cjson_parse("[10, 20, 30, 40]", &err);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_list_len(root), (size_t)4);

  REQUIRE_EQ(cjson_list_remove(root, 1), ccol_success);

  REQUIRE_EQ(cjson_list_len(root), (size_t)3);
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

  REQUIRE_EQ(cjson_list_len(root), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 2LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 3LL);
  cjson_destroy(root);
}

TEST(delete, list_remove_last) {
  char *err = NULL;
  cjson root = cjson_parse("[1, 2, 3]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 2), ccol_success);

  REQUIRE_EQ(cjson_list_len(root), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 1LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 2LL);
  cjson_destroy(root);
}

TEST(delete, list_remove_only_element) {
  char *err = NULL;
  cjson root = cjson_parse("[42]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 0), ccol_success);
  REQUIRE_EQ(cjson_list_len(root), (size_t)0);
  cjson_destroy(root);
}

TEST(delete, list_remove_out_of_bounds) {
  char *err = NULL;
  cjson root = cjson_parse("[1, 2]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 2), ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(root), (size_t)2);
  cjson_destroy(root);
}

TEST(delete, list_remove_subtree_freed) {
  /* Removing a list element that is itself a nested object must not leak. */
  char *err = NULL;
  cjson root = cjson_parse("[{\"a\":1,\"b\":[10,20]}, 99]", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_list_remove(root, 0), ccol_success);
  REQUIRE_EQ(cjson_list_len(root), (size_t)1);
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
  /* Removing a key whose value is a nested container must not leak. */
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

  REQUIRE_EQ(cjson_delete(root, "x"), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  REQUIRE_EQ((void *)cjson_get(root, "x"), NULL);
  REQUIRE_NE((void *)cjson_get(root, "y"), NULL);
  cjson_destroy(root);
}

TEST(delete, path_delete_nested_key) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":{\"b\":1,\"c\":2}}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_delete(root, "a.b"), ccol_success);
  REQUIRE_EQ((void *)cjson_get(root, "a.b"), NULL);
  REQUIRE_NE((void *)cjson_get(root, "a.c"), NULL);
  cjson_destroy(root);
}

TEST(delete, path_delete_list_element) {
  char *err = NULL;
  cjson root = cjson_parse("{\"items\":[10,20,30]}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_delete(root, "items.#1"), ccol_success);

  cjson items = cjson_get(root, "items");
  REQUIRE_EQ(cjson_list_len(items), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(items, 0)), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(items, 1)), 30LL);
  cjson_destroy(root);
}

TEST(delete, path_missing_parent_returns_key_not_found) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":1}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_delete(root, "x.y"), ccol_key_not_found);
  cjson_destroy(root);
}

TEST(delete, path_missing_leaf_returns_key_not_found) {
  char *err = NULL;
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_delete(root, "a.z"), ccol_key_not_found);
  cjson_destroy(root);
}

TEST(delete, path_delete_list_out_of_bounds_returns_key_not_found) {
  /* A syntactically valid "#N" index past the end of the list addresses an
   * absent element, not a malformed path: this must report
   * ccol_key_not_found (matching _cjson_delete's own documented contract,
   * "ccol_key_not_found if any path component is absent"), not
   * ccol_invalid_args. This is deliberately distinct from
   * cjson_list_remove()'s own direct-call contract (ccol_invalid_args for
   * an out-of-bounds index), which is unaffected. */
  char *err = NULL;
  cjson root = cjson_parse("{\"items\":[1,2,3]}", &err);
  REQUIRE_NE((void *)root, NULL);

  REQUIRE_EQ(cjson_delete(root, "items.#3"), ccol_key_not_found);
  REQUIRE_EQ(cjson_delete(root, "items.#99"), ccol_key_not_found);
  REQUIRE_EQ(cjson_list_len(cjson_get(root, "items")), (size_t)3);

  /* cjson_list_remove() itself, called directly, still reports
   * ccol_invalid_args for the identical out-of-bounds index. */
  REQUIRE_EQ(cjson_list_remove(cjson_get(root, "items"), 3), ccol_invalid_args);

  cjson_destroy(root);
}

TEST(delete, path_delete_list_root_out_of_bounds_returns_key_not_found) {
  /* Same as above, but with a CJSON_LIST as the path's own root rather than
   * nested under a dictionary key. */
  cjson root = cjson_parse("[10, 20, 30]", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_delete(root, "#3"), ccol_key_not_found);
  REQUIRE_EQ(cjson_list_len(root), (size_t)3);
  cjson_destroy(root);
}

TEST(delete, path_delete_with_escaped_dot_in_key) {
  /* Key literally named "a.b" must be addressable via "a\\.b". */
  cjson root = cjson_create_dictionary();
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_dictionary_set(root, "a.b", cjson_create_int(7)),
             ccol_success);
  REQUIRE_EQ(cjson_dictionary_set(root, "keep", cjson_create_int(1)),
             ccol_success);

  REQUIRE_EQ(cjson_delete(root, "a\\.b"), ccol_success);
  REQUIRE_EQ(cjson_dictionary_size(root), (size_t)1);
  REQUIRE_NE((void *)cjson_dictionary_get(root, "keep"), NULL);
  cjson_destroy(root);
}

TEST(delete, null_root_returns_invalid_args) {
  REQUIRE_EQ(_cjson_delete(NULL, "x"), ccol_invalid_args);
}

TEST(delete, null_path_returns_invalid_args) {
  cjson root = cjson_parse("{\"x\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(_cjson_delete(root, NULL), ccol_invalid_args);
  cjson_destroy(root);
}

TEST(delete, empty_path_returns_invalid_args) {
  cjson root = cjson_parse("{\"x\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(_cjson_delete(root, ""), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "x"), NULL);
  cjson_destroy(root);
}

TEST(delete, trailing_dot_returns_invalid_args) {
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(_cjson_delete(root, "a."), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a.b"), NULL);
  cjson_destroy(root);
}

TEST(delete, consecutive_dots_rejected) {
  /* An empty INTERMEDIATE path component ("a..b") must be classified the
   * same way an empty LEAF component already is (see
   * trailing_dot_returns_invalid_args above): ccol_invalid_args, a syntax
   * error, not ccol_key_not_found. */
  cjson root = cjson_parse("{\"a\":{\"b\":1}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(_cjson_delete(root, "a..b"), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a.b"), NULL);
  cjson_destroy(root);
}

TEST(delete, trailing_dot_with_nonexistent_parent_still_invalid_args) {
  /* Regression test: a trailing dot (empty leaf component) must be reported
   * as ccol_invalid_args unconditionally, even when the parent path itself
   * also does not exist. The leaf's own syntax must be validated before the
   * parent is ever looked up, so a genuinely absent parent never masks a
   * leaf syntax error as ccol_key_not_found. */
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(_cjson_delete(root, "missing."), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(delete,
     consecutive_dots_with_nonexistent_first_component_still_invalid_args) {
  /* Regression test: an empty INTERMEDIATE component ("missing..b") must
   * still be reported as ccol_invalid_args even when the component before
   * it ("missing") does not exist either; navigate() must keep scanning
   * the rest of the path for a syntax error rather than giving up the
   * moment an earlier, well-formed component fails to resolve. */
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(_cjson_delete(root, "missing..b"), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(delete, leading_dot_rejected) {
  cjson root = cjson_parse("{\"a\":1}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(_cjson_delete(root, ".a"), ccol_invalid_args);
  REQUIRE_NE((void *)cjson_get(root, "a"), NULL);
  cjson_destroy(root);
}

TEST(delete, list_root_delete_element) {
  /* cjson_delete must work when root itself is a CJSON_LIST and the path
   * addresses an element directly (no intermediate dictionary lookup). */
  cjson root = cjson_parse("[10, 20, 30]", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_delete(root, "#1"), ccol_success);
  REQUIRE_EQ(cjson_list_len(root), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 0)), 10LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(root, 1)), 30LL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*          REMOVE FUNCTION NULL / WRONG-TYPE ARGUMENT EDGE CASES             */
/* ========================================================================== */

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

/* ========================================================================== */
/*         node_make_scalar ERROR PROPAGATION (new-key path)                  */
/* ========================================================================== */

TEST(navigate, set_nonfinite_new_key_returns_invalid_args) {
  /* Inf/NaN on a key that does NOT yet exist must return ccol_invalid_args,
   * not ccol_not_enough_memory. The new-key path in _cjson_set_typed must
   * propagate the exact error from node_reinit_scalar via node_make_scalar. */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  REQUIRE_EQ(cjson_set(root, "v", INFINITY), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  REQUIRE_EQ(cjson_set(root, "v", NAN), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_typed_invalid_integer_size_new_key) {
  /* A direct _cjson_set_typed call with an invalid raw_size for CJSON_INTEGER
   * and a key that does not yet exist must return ccol_invalid_args, not
   * ccol_not_enough_memory.  This is only reachable via the back-end function
   * (cjson_set always passes a valid sizeof). */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  long long v = 42;
  ccol_retval_t r =
      _cjson_set_typed(root, "k", CJSON_INTEGER, &v, 3, true, false);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "k"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_typed_invalid_bool_size_new_key) {
  /* Mirrors set_typed_invalid_integer_size_new_key above, for CJSON_BOOL:
   * a mismatched raw_size (anything other than sizeof(bool)) on a key that
   * does not yet exist must be rejected outright, not read as a bool via a
   * mismatched-width reinterpretation of the raw bytes (which could produce
   * a _Bool trap representation). Only reachable via the back-end function;
   * cjson_set always passes sizeof(bool) for a bool-typed C expression. */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  int not_a_bool = 4;
  ccol_retval_t r = _cjson_set_typed(root, "k", CJSON_BOOL, &not_a_bool,
                                     sizeof(not_a_bool), false, false);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "k"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_typed_invalid_bool_size_existing_key_preserves_value) {
  /* Mirrors set_unsupported_type_existing_key_preserves_value above, for a
   * mismatched-size CJSON_BOOL: rejecting the write must leave the
   * existing value completely untouched, not partially overwritten. */
  cjson root = cjson_parse("{\"v\":123}", NULL);
  REQUIRE_NE((void *)root, NULL);
  int not_a_bool = 4;
  ccol_retval_t r = _cjson_set_typed(root, "v", CJSON_BOOL, &not_a_bool,
                                     sizeof(not_a_bool), false, false);
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson v = cjson_get(root, "v");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cjson_type(v), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(v), 123LL);
  cjson_destroy(root);
}

TEST(navigate,
     set_typed_invalid_bool_size_existing_list_element_preserves_value) {
  /* Mirrors set_typed_invalid_bool_size_existing_key_preserves_value above,
   * but for a CJSON_LIST parent instead of a CJSON_DICTIONARY one:
   * _cjson_set_typed's list branch reaches the exact same
   * node_reinit_scalar() validate-before-mutate call as the dictionary
   * branch, but via a structurally different call site (cvector_at() plus a
   * '#N' component instead of a chmap lookup), so it needs its own
   * regression coverage rather than relying on the dictionary case to stand
   * in for it. */
  cjson root = cjson_parse("[123]", NULL);
  REQUIRE_NE((void *)root, NULL);
  int not_a_bool = 4;
  ccol_retval_t r = _cjson_set_typed(root, "#0", CJSON_BOOL, &not_a_bool,
                                     sizeof(not_a_bool), false, false);
  REQUIRE_EQ(r, ccol_invalid_args);
  cjson v = cjson_get(root, "#0");
  REQUIRE_NE((void *)v, NULL);
  REQUIRE_EQ(cjson_type(v), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(v), 123LL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*        cjson_set - UNSUPPORTED C TYPE MUST BE REJECTED, NOT SILENCED       */
/* ========================================================================== */

TEST(navigate, set_unsupported_type_new_key_rejected) {
  /* long double is not on cjson_set's documented accepted-type list (bool,
   * any integer type, float, double, char *, const char *). A key that does
   * not yet exist must not be silently created as CJSON_NULL for it. */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  long double ld = 3.14L;
  REQUIRE_EQ(cjson_set(root, "v", ld), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_unsupported_type_existing_key_preserves_value) {
  /* Setting an unsupported type on a key that already holds real data must
   * leave that data completely untouched, not silently overwrite it with
   * CJSON_NULL. */
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
  /* Exercise the same guard directly through the back-end function, mirroring
   * how _cjson_type_of's sentinel reaches node_reinit_scalar's validation. */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  long double ld = 1.0L;
  ccol_retval_t r = _cjson_set_typed(root, "k", _CJSON_TYPE_UNSUPPORTED, &ld,
                                     sizeof(ld), false, false);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "k"), NULL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*     cjson_set - NON-NULL void* MUST BE REJECTED, NOT WRITTEN AS NULL       */
/* ========================================================================== */

TEST(navigate, set_nonnull_void_ptr_new_key_rejected) {
  /* cjson_set's _Generic dispatch (_cjson_type_of in cjson.h) maps ANY
   * void*-typed C expression to CJSON_NULL, not just a literal NULL.
   * node_reinit_scalar's own guard must reject a genuinely non-NULL void*
   * value with ccol_invalid_args instead of silently creating the key as
   * CJSON_NULL. */
  cjson root = cjson_parse("{}", NULL);
  REQUIRE_NE((void *)root, NULL);
  int dummy = 5;
  void *p = &dummy;
  REQUIRE_EQ(cjson_set(root, "v", p), ccol_invalid_args);
  REQUIRE_EQ((void *)cjson_get(root, "v"), NULL);
  cjson_destroy(root);
}

TEST(navigate, set_nonnull_void_ptr_existing_key_preserves_value) {
  /* Same guard as set_nonnull_void_ptr_new_key_rejected, but on a key that
   * already holds real data: rejecting the write must leave that data
   * completely untouched rather than silently overwriting it with
   * CJSON_NULL. */
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

TEST(navigate, set_null_void_ptr_still_accepted) {
  /* Regression guard for the guard above: it must reject only a genuinely
   * non-NULL void*, not void* as a C type in general. A void* variable that
   * actually holds NULL is still documented, intentional usage and must
   * keep setting the leaf to CJSON_NULL. */
  cjson root = cjson_parse("{\"v\":123}", NULL);
  REQUIRE_NE((void *)root, NULL);
  void *p = NULL;
  REQUIRE_EQ(cjson_set(root, "v", p), ccol_success);
  REQUIRE_EQ(cjson_type(cjson_get(root, "v")), CJSON_NULL);
  cjson_destroy(root);
}

/* ========================================================================== */
/*                         ADDITIONAL COVERAGE                                */
/* ========================================================================== */

TEST(parse, null_input_returns_null) {
  /* cjson_parse(NULL) must return NULL without crashing. */
  REQUIRE_EQ((void *)cjson_parse(NULL, NULL), NULL);
}

TEST(parse, null_input_with_error_str) {
  /* The error string must be populated when input is NULL. */
  char *err = NULL;
  cjson n = cjson_parse_mp(NULL, &err, NULL);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strlen(err) > 0);
  free(err);
}

TEST(navigate, set_new_null_key) {
  /* cjson_set with NULL must CREATE a findable CJSON_NULL node when the key
   * does not yet exist (not merely return success without inserting). */
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
  /* A path with a trailing dot has an empty leaf component; cjson_set must
   * return a non-success error code and leave the tree unmodified. */
  cjson root = cjson_parse("{\"a\":{\"b\":0}}", NULL);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval_t r = cjson_set(root, "a.", 99);
  REQUIRE_NE(r, ccol_success);
  REQUIRE_EQ(cjson_int_val(cjson_get(root, "a.b")), 0LL);
  cjson_destroy(root);
}

TEST(serialize, pretty_print_list) {
  /* cjson_serialize_pretty must emit a newline-indented form for list roots. */
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
  REQUIRE_EQ(cjson_list_len(back), (size_t)2);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(back, 0)), 1LL);
  REQUIRE_EQ(cjson_int_val(cjson_list_get(back, 1)), 2LL);
  cjson_serialize_free(s);
  cjson_destroy(a);
  cjson_destroy(back);
}

TEST(clone, null_type_node) {
  /* cjson_clone on a CJSON_NULL-TYPE node (distinct from a NULL handle) must
   * return an independent non-NULL handle of type CJSON_NULL. */
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
  /* cjson_list_get on a non-CJSON_LIST node must return NULL, not crash. */
  cjson not_list = cjson_create_int(42);
  REQUIRE_EQ((void *)cjson_list_get(not_list, 0), NULL);
  cjson_destroy(not_list);
}

TEST(construction, dict_get_wrong_type_returns_null) {
  /* cjson_dictionary_get on a non-CJSON_DICTIONARY node must return NULL. */
  cjson not_dict = cjson_create_string("oops");
  REQUIRE_EQ((void *)cjson_dictionary_get(not_dict, "k"), NULL);
  cjson_destroy(not_dict);
}

TEST(navigate, set_on_scalar_root_returns_invalid_args) {
  /* cjson_set where the eventual parent is a scalar (not dict or list) must
   * return ccol_invalid_args and leave the node untouched. */
  cjson root = cjson_create_int(42);
  ccol_retval_t r = cjson_set(root, "key", 5);
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_type(root), CJSON_INTEGER);
  REQUIRE_EQ(cjson_int_val(root), 42LL);
  cjson_destroy(root);
}

TEST(edge, llong_max_serialize_roundtrip) {
  /* LLONG_MAX must serialize to the correct decimal string and parse back. */
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
  /* A path whose leaf component does not start with '#' while the parent is a
   * CJSON_LIST is invalid; cjson_delete must return ccol_invalid_args and
   * leave the list unmodified. */
  cjson root = cjson_parse("[1, 2, 3]", NULL);
  REQUIRE_NE((void *)root, NULL);
  ccol_retval_t r = cjson_delete(root, "key");
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_list_len(root), (size_t)3);
  cjson_destroy(root);
}

TEST(delete, path_delete_on_scalar_root_returns_invalid_args) {
  /* cjson_delete where root is a scalar (not dict or list) must return
   * ccol_invalid_args and leave the node untouched. */
  cjson root = cjson_create_int(42);
  ccol_retval_t r = _cjson_delete(root, "key");
  REQUIRE_EQ(r, ccol_invalid_args);
  REQUIRE_EQ(cjson_type(root), CJSON_INTEGER);
  cjson_destroy(root);
}

/* ========================================================================== */
/*        PARSE NESTING DEPTH GUARD (CJSON_MAX_PARSE_DEPTH = 500)             */
/* ========================================================================== */

/* Builds a string of n '[' characters, a scalar '1', then n ']' characters
 * into a heap-allocated buffer the caller must free(). */
static char *build_nested_array(size_t n) {
  char *buf = malloc(2 * n + 2);
  size_t pos = 0;
  for (size_t i = 0; i < n; i++) buf[pos++] = '[';
  buf[pos++] = '1';
  for (size_t i = 0; i < n; i++) buf[pos++] = ']';
  buf[pos] = '\0';
  return buf;
}

/* Builds n levels of {"a": ... } nesting around a scalar '1' into a
 * heap-allocated buffer the caller must free(). */
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

TEST(parse, deeply_nested_array_rejected_not_crashed) {
  /* A document with far more nesting than CJSON_MAX_PARSE_DEPTH must be
   * rejected promptly with a parse error, not crash the process via
   * unbounded recursive-descent stack growth (confirmed, prior to this
   * guard, to segfault well under this depth) and not hang. Asserts an
   * explicit wall-clock bound, matching this codebase's own established
   * convention for DoS-guard regression tests (see cyaml's own
   * deeply_nested_explicit_keys_rejected_not_hung / cthreadcomm's
   * event_loop DoS-guard tests). */
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
  free(err);
}

TEST(parse, deeply_nested_object_rejected_not_crashed) {
  /* Same guard, exercised via object nesting instead of array nesting;
   * both parse_list() and parse_dictionary() recurse back into
   * parse_value(), so both must be independently covered. */
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
  free(err);
}

TEST(parse, nesting_at_max_depth_still_parses) {
  /* A document sitting exactly at the accepted boundary (499 nested '['
   * wrapping one scalar; 500 total parse_value() levels, matching
   * CJSON_MAX_PARSE_DEPTH) must still parse successfully; the guard must
   * not be off-by-one against ordinary, legitimate deep-but-bounded
   * documents. */
  char *json = build_nested_array(499);
  cjson n = cjson_parse(json, NULL);
  free(json);
  REQUIRE_NE((void *)n, NULL);
  REQUIRE_EQ(cjson_type(n), CJSON_LIST);
  cjson_destroy(n);
}

TEST(parse, nesting_one_past_max_depth_rejected) {
  /* One level past the accepted boundary (500 nested '[', 501 total
   * parse_value() levels) must be rejected, confirming the guard's exact
   * threshold rather than merely "eventually rejects something". */
  char *json = build_nested_array(500);
  char *err = NULL;
  cjson n = cjson_parse_mp(json, &err, NULL);
  free(json);
  REQUIRE_EQ((void *)n, NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_TRUE(strstr(err, "nesting depth") != NULL);
  free(err);
}

/* ========================================================================== */
/*     API-CONSTRUCTED (NOT PARSED) DEEP TREES; CLONE/SERIALIZE/DESTROY      */
/* ========================================================================== */

/* Builds a list nested n levels deep directly through the public
 * construction API (cjson_list_push), bypassing the parser (and its
 * CJSON_MAX_PARSE_DEPTH guard) entirely.  Returns the outermost node. */
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
  /* cjson_clone() has its own independent depth cap (CJSON_CLONE_MAX_DEPTH)
   * precisely because a tree reaching it need not have come from
   * cjson_parse() at all; it can be built arbitrarily deep directly via
   * cjson_list_push(), which CJSON_MAX_PARSE_DEPTH cannot see or bound. */
  cjson deep = build_nested_list_via_api(600);
  REQUIRE_NE((void *)deep, NULL);
  cjson copy = cjson_clone(deep);
  REQUIRE_EQ((void *)copy, NULL);
  cjson_destroy(deep);
}

TEST(serialize, deeply_nested_api_built_tree_rejected_not_crashed) {
  /* Same guard, for cjson_serialize()'s own independent depth cap
   * (CJSON_MAX_SERIALIZE_DEPTH). */
  cjson deep = build_nested_list_via_api(600);
  REQUIRE_NE((void *)deep, NULL);
  char *s = cjson_serialize(deep);
  REQUIRE_EQ((void *)s, NULL);
  cjson_destroy(deep);
}

TEST(clone, nesting_at_clone_max_depth_still_clones) {
  /* A tree sitting exactly at the parser's own CJSON_MAX_PARSE_DEPTH (499
   * nested lists wrapping one scalar, matching the boundary test above)
   * must still be clonable; CJSON_CLONE_MAX_DEPTH is deliberately set to
   * the same value so a maximally-nested, successfully-parsed document
   * never spuriously fails to clone. */
  cjson deep = build_nested_list_via_api(499);
  REQUIRE_NE((void *)deep, NULL);
  cjson copy = cjson_clone(deep);
  REQUIRE_NE((void *)copy, NULL);
  cjson_destroy(deep);
  cjson_destroy(copy);
}

TEST(destroy, deeply_nested_api_built_tree_destroyed_without_crashing) {
  /* Unlike clone()/serialize(), __cjson_destroy() has no "fail cleanly"
   * contract to fall back on (it is void), so it must remain safe against
   * an arbitrarily deep tree via an iterative worklist rather than a depth
   * cap; confirmed here well past every other guard's own cap. This test
   * cannot itself detect a leak (tau has no built-in leak checker); its
   * purpose is to exercise this exact teardown path under `make memtest`
   * (valgrind), which already runs this whole suite. */
  cjson deep = build_nested_list_via_api(20000);
  REQUIRE_NE((void *)deep, NULL);
  cjson_destroy(deep);
  REQUIRE_EQ((void *)deep, NULL);
}

TEST(ownership,
     list_push_into_unattached_container_stays_cheap_even_for_deep_child) {
  /* Regression guard for the cycle-detection check added to
   * cjson_list_push()/cjson_dictionary_set(): the search for a would-be
   * cycle must not turn the ordinary "wrap an already-built subtree in a
   * brand-new, still-unattached outer container" pattern into an O(n^2)
   * cost; see node_reaches()'s own doc comment in cjson.c for why
   * checking needle->attached first keeps this O(1) per call regardless of
   * how large the already-built child is. build_nested_list_via_api()
   * performs exactly this pattern once per level; without the
   * short-circuit, n=20000 measured well over a second here, versus a few
   * milliseconds with it. Asserts an explicit wall-clock bound, matching
   * this codebase's own established DoS-guard test convention (see
   * parse.deeply_nested_array_rejected_not_crashed above). */
  clock_t t0 = clock();
  cjson deep = build_nested_list_via_api(20000);
  double elapsed_ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
  REQUIRE_NE((void *)deep, NULL);
  REQUIRE_LT(elapsed_ms, 500.0);
  cjson_destroy(deep);
}

/* ========================================================================== */
/*      NUMBER LITERALS LONGER THAN THE PARSER'S STACK TOKEN BUFFER           */
/* ========================================================================== */

TEST(fuzzy, number_literal_past_stack_buffer_still_parses) {
  /* RFC 8259 sec. 6 places no length limit on a number literal. A
   * syntactically valid, finite number whose raw source text exceeds the
   * parser's internal 360-byte stack token buffer (e.g. many redundant
   * leading zeros in the fractional part) must still parse successfully
   * via the heap-allocated fallback, not be rejected as "too long". */
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
  /* Same stack-buffer-overflow class, exercised via the integer path (many
   * digits, no decimal point): a 401-digit literal both exceeds the
   * parser's 360-byte stack token buffer (forcing the heap-allocated
   * fallback) AND exceeds any finite double's range (~309 decimal digits
   * at most), so strtoll() reports ERANGE, the strtod() fallback reports
   * infinity, and the whole parse must still fail cleanly with a
   * heap-allocated token cleaned up on the error path; not crash, leak,
   * or silently wrap/truncate. */
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
  free(err);
}

TEST(fuzzy, malformed_number_literal_past_stack_buffer_still_rejected) {
  /* The heap fallback must not bypass ordinary number-grammar validation:
   * a >360-byte token with a syntax error (two decimal points) is still a
   * parse error, not silently accepted. */
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
  /* The heap-fallback token buffer for an over-long number literal must go
   * through the same custom allocator as everything else in the parse, and
   * must be freed regardless of which branch (integer overflow vs. float)
   * is taken. */
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
  /* The token allocation itself must already have been freed (it is
   * scratch state, not part of the returned tree), independently of
   * destroying n. */
  REQUIRE_GT(_ta_frees, (size_t)0);
  cjson_destroy(n);
}

/* ========================================================================== */
/*                         THREAD-LOCAL NODE POOL                             */
/* ========================================================================== */

/* White-box accessor exposing the CALLING thread's own node-pool free-list
 * size (see cjson.c's own thread-local free-list pool); not part of the
 * public API, declared here the same way tests/cvector/tests.c declares
 * cvector_get_capacity(). */
extern size_t cjson_debug_pool_size(void);

/* Mirrors _NODE_POOL_CAP in cjson.c. Not part of any public header (it is
 * an internal tuning constant), so it is deliberately re-stated here rather
 * than shared, matching how other white-box tests in this codebase pin a
 * literal internal constant directly. */
#define CJSON_TEST_POOL_CAP 512

TEST(node_pool, cap_eviction_keeps_pool_bounded) {
  /* Whatever this thread's own pool already holds when this test runs (0..
   * CJSON_TEST_POOL_CAP, carried over from earlier tests in this same
   * binary), destroying strictly more than CJSON_TEST_POOL_CAP
   * default-allocator nodes in one go must leave the pool at EXACTLY the
   * cap afterward: every node_free() call below the cap is accepted into
   * the free-list, and every one past it is evicted (freed directly)
   * instead of letting the pool grow without bound. */
  cjson list = cjson_create_list();
  REQUIRE_NE((void *)list, NULL);
  size_t n = CJSON_TEST_POOL_CAP + 100;
  for (size_t i = 0; i < n; i++)
    REQUIRE_EQ(cjson_list_push(list, cjson_create_null()), ccol_success);
  cjson_destroy(list);

  REQUIRE_EQ(cjson_debug_pool_size(), (size_t)CJSON_TEST_POOL_CAP);
}

typedef struct {
  size_t start_pool_size;
  size_t end_pool_size;
} pool_populate_result_t;

static void *pool_populate_thread(void *arg) {
  pool_populate_result_t *r = (pool_populate_result_t *)arg;
  r->start_pool_size = cjson_debug_pool_size();
  /* Allocate all 50 first, THEN free all 50: interleaving a single alloc
   * with an immediate free would just recycle that same one node fifty
   * times over (net pool size 1, not 50), since node_alloc() always prefers
   * a pool-resident node when one is available. Holding all 50 live at
   * once forces every one of them to be a fresh calloc (nothing is yet
   * resident to recycle), so freeing them afterward grows the pool by
   * exactly one entry per node, mirroring cap_eviction_keeps_pool_bounded's
   * own allocate-then-free-in-bulk shape above. */
  cjson nodes[50];
  for (int i = 0; i < 50; i++) nodes[i] = cjson_create_null();
  for (int i = 0; i < 50; i++) cjson_destroy(nodes[i]);
  r->end_pool_size = cjson_debug_pool_size();
  return NULL;
}

TEST(node_pool, fresh_thread_starts_with_an_empty_pool) {
  /* The node pool is thread-local (see cjson.c's own thread-local free-list
   * pool): a brand-new thread must never see whatever nodes the calling
   * (main) thread's own pool already holds by the time this test runs.
   * Every node_free() call the worker makes below only ever returns a node
   * to ITS OWN pool, never to this thread's; joining before reading either
   * result field satisfies this codebase's own "join unconditionally,
   * before any REQUIRE_*" test-hygiene rule. Leaves the worker thread's own
   * pool holding 50 nodes at thread-exit time, so this also exercises the
   * pthread-destructor pool-drain path (_node_pool_drain) under `make
   * memtest`: a broken drain would show up there as a 50-allocation leak. */
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
  cjson *nodes;
  size_t count;
  size_t start_pool_size;
  size_t end_pool_size;
} cross_thread_free_arg_t;

static void *cross_thread_free_thread(void *arg) {
  cross_thread_free_arg_t *a = (cross_thread_free_arg_t *)arg;
  a->start_pool_size = cjson_debug_pool_size();
  for (size_t i = 0; i < a->count; i++) cjson_destroy(a->nodes[i]);
  a->end_pool_size = cjson_debug_pool_size();
  return NULL;
}

TEST(node_pool, node_freed_on_a_different_thread_joins_that_threads_own_pool) {
  /* A node carries no thread affinity: node_free() always returns it to
   * whichever thread is CURRENTLY calling it, never the one that originally
   * allocated it (see node_free()'s own doc comment in cjson.c). Allocate
   * every node on the main thread, but free all of them from a worker
   * thread instead, and confirm the freed nodes land in the WORKER's own
   * pool, leaving the main thread's own pool count completely unaffected. */
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
  pthread_create(&tid, NULL, cross_thread_free_thread, &arg);
  pthread_join(tid, NULL);
  free(nodes);

  size_t main_pool_after = cjson_debug_pool_size();

  REQUIRE_EQ(arg.start_pool_size, (size_t)0);
  REQUIRE_EQ(arg.end_pool_size, count);
  REQUIRE_EQ(main_pool_after, main_pool_before);
}
