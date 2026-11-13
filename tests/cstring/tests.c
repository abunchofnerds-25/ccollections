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

#include <cstring.h>
#include <ctype.h>
#include <fcntl.h>
#include <internal/cprocsintern.h>
#include <internal/test_helpers/cstring.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <unistd.h>

TAU_MAIN()

// Resets the signal state this binary inherited, before any test runs. The
// forked children report through their wait status, which an inherited signal
// state can break; test_reset_inherited_signals() explains how.
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

// The smallest capacity of a string, the same value as the constant in
// cstring.c, for the capacity tests.
#define CSTRING_MIN_CAPACITY 16

TEST(cstrings, create_fails_bad_mprocs) {
  char *err = NULL;

  cstr s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs){
          .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);

  s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);

  s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);

  s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL},
      &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_succeeds_null_initial) {
  char *err = NULL;
  cstr s = cstring_create(NULL, &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cstring_length(s), 0);
  REQUIRE_NE((void *)cstring_c_str(s), NULL);
  REQUIRE_EQ(cstring_c_str(s)[0], '\0');
  cstring_destroy(s);
  REQUIRE_EQ((void *)s, NULL);
}

TEST(cstrings, create_succeeds_empty_initial) {
  char *err = NULL;
  cstr s = cstring_create("", &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cstring_length(s), 0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  cstring_destroy(s);
}

TEST(cstrings, create_succeeds_with_initial) {
  char *err = NULL;
  cstr s = cstring_create("hello", &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cstring_length(s), 5);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, create_succeeds_with_mprocs) {
  char *err = NULL;
  cstr s = cstring_create_full(
      "world",
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_EQ(cstring_length(s), 5);
  REQUIRE_STREQ(cstring_c_str(s), "world");
  cstring_destroy(s);
}

TEST(cstrings, create_without_error_ptr) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

TEST(cstrings, length_empty) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, length_nonempty) {
  cstr s = cstring_create("abcdef", NULL);
  REQUIRE_EQ(cstring_length(s), 6);
  cstring_destroy(s);
}

TEST(cstrings, c_str_null_terminated) {
  cstr s = cstring_create("test", NULL);
  const char *p = cstring_c_str(s);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(p[4], '\0');
  REQUIRE_STREQ(p, "test");
  cstring_destroy(s);
}

TEST(cstrings, at_valid_indices) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_at(s, 0), 'h');
  REQUIRE_EQ(cstring_at(s, 1), 'e');
  REQUIRE_EQ(cstring_at(s, 4), 'o');
  cstring_destroy(s);
}

TEST(cstrings, at_out_of_bounds) {
  cstr s = cstring_create("hi", NULL);
  REQUIRE_EQ(cstring_at(s, 2), '\0');
  REQUIRE_EQ(cstring_at(s, 100), '\0');
  cstring_destroy(s);
}

TEST(cstrings, at_on_empty_string) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring_at(s, 0), '\0');
  cstring_destroy(s);
}

TEST(cstrings, is_empty_true) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_is_empty(s));
  cstring_destroy(s);
}

TEST(cstrings, is_empty_false) {
  cstr s = cstring_create("x", NULL);
  REQUIRE_FALSE(cstring_is_empty(s));
  cstring_destroy(s);
}

TEST(cstrings, is_empty_after_reset) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_FALSE(cstring_is_empty(s));
  cstring_reset(s);
  REQUIRE_TRUE(cstring_is_empty(s));
  cstring_destroy(s);
}

TEST(cstrings, append_to_empty) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring_append(s, "hello"), ccol_success);
  REQUIRE_EQ(cstring_length(s), 5);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, append_to_nonempty) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_append(s, ", world"), ccol_success);
  REQUIRE_EQ(cstring_length(s), 12);
  REQUIRE_STREQ(cstring_c_str(s), "hello, world");
  cstring_destroy(s);
}

TEST(cstrings, append_empty_string) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_append(s, ""), ccol_success);
  REQUIRE_EQ(cstring_length(s), 5);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, append_null) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_append(s, NULL), ccol_invalid_args);
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

TEST(cstrings, append_multiple_times) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring_append(s, "a"), ccol_success);
  REQUIRE_EQ(cstring_append(s, "b"), ccol_success);
  REQUIRE_EQ(cstring_append(s, "c"), ccol_success);
  REQUIRE_EQ(cstring_length(s), 3);
  REQUIRE_STREQ(cstring_c_str(s), "abc");
  cstring_destroy(s);
}

TEST(cstrings, append_triggers_growth) {
  cstr s = cstring_create(NULL, NULL);
  size_t initial_cap = cstring__get_capacity(s);
  REQUIRE_EQ(initial_cap, CSTRING_MIN_CAPACITY);

  const char *chunk = "0123456789abcdef";
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);

  REQUIRE_EQ(cstring_length(s), 32);
  REQUIRE_TRUE(cstring__get_capacity(s) > initial_cap);
  size_t cap = cstring__get_capacity(s);
  REQUIRE_TRUE(cap >= 33);
  REQUIRE_EQ(cap & (cap - 1), 0);
  cstring_destroy(s);
}

TEST(cstrings, prepend_to_empty) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring_prepend(s, "hello"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, prepend_to_nonempty) {
  cstr s = cstring_create("world", NULL);
  REQUIRE_EQ(cstring_prepend(s, "hello "), ccol_success);
  REQUIRE_EQ(cstring_length(s), 11);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, prepend_empty_string) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_prepend(s, ""), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, prepend_null) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_prepend(s, NULL), ccol_invalid_args);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, prepend_preserves_original_content) {
  cstr s = cstring_create("456", NULL);
  REQUIRE_EQ(cstring_prepend(s, "123"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456");
  REQUIRE_EQ(cstring_at(s, 3), '4');
  REQUIRE_EQ(cstring_at(s, 4), '5');
  REQUIRE_EQ(cstring_at(s, 5), '6');
  cstring_destroy(s);
}

TEST(cstrings, insert_at_beginning) {
  cstr s = cstring_create("world", NULL);
  REQUIRE_EQ(cstring_insert(s, 0, "hello "), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, insert_at_middle) {
  cstr s = cstring_create("helloworld", NULL);
  REQUIRE_EQ(cstring_insert(s, 5, " "), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, insert_at_end) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_insert(s, 5, " world"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, insert_empty_string) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_insert(s, 2, ""), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, insert_invalid_position) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_insert(s, 6, "x"), ccol_invalid_args);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, insert_null) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_insert(s, 0, NULL), ccol_invalid_args);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, set_basic) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_set(s, "world"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "world");
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

TEST(cstrings, set_with_longer_string) {
  cstr s = cstring_create("hi", NULL);
  REQUIRE_EQ(cstring_set(s, "a much longer string"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "a much longer string");
  REQUIRE_EQ(cstring_length(s), 20);
  cstring_destroy(s);
}

TEST(cstrings, set_with_shorter_string) {
  cstr s = cstring_create("a much longer string", NULL);
  REQUIRE_EQ(cstring_set(s, "hi"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hi");
  REQUIRE_EQ(cstring_length(s), 2);
  cstring_destroy(s);
}

TEST(cstrings, set_with_empty_string) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_set(s, ""), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, set_null) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_set(s, NULL), ccol_invalid_args);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, set_self_alias_no_realloc) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), (size_t)5);
  cstring_destroy(s);
}

TEST(cstrings, set_self_alias_full_length_no_realloc) {
  cstr s = cstring_create("1234567890123456", NULL);
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  ccol_retval rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "1234567890123456");
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  cstring_destroy(s);
}

TEST(cstrings, set_suffix_alias_no_realloc) {
  cstr s = cstring_create("1234567890123456", NULL);
  ccol_retval rv = cstring_set(s, cstring_c_str(s) + 6);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "7890123456");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstring_destroy(s);
}

TEST(cstrings, reset_clears_content) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_length(s), 11);
  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), 0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  cstring_destroy(s);
}

TEST(cstrings, reset_shrinks_capacity) {
  cstr s = cstring_create(NULL, NULL);
  const char *chunk = "0123456789abcdef0123456789abcdef";
  cstring_append(s, chunk);
  REQUIRE_TRUE(cstring__get_capacity(s) > CSTRING_MIN_CAPACITY);

  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), 0);
  REQUIRE_EQ(cstring__get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, reset_then_reuse) {
  cstr s = cstring_create("initial", NULL);
  cstring_reset(s);
  REQUIRE_EQ(cstring_append(s, "new content"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "new content");
  cstring_destroy(s);
}

// An allocator that counts realloc calls and forwards everything to the system
// heap, so a test can tell "skipped the realloc" from "made a realloc that
// changed nothing".
static int g_cstr_realloc_call_count = 0;
static void *_cstr_counting_realloc(void *ptr, size_t size) {
  g_cstr_realloc_call_count++;
  return realloc(ptr, size);
}
static ccol_memmgmt_procs g_cstr_counting_procs = {
    .malloc = malloc,
    .calloc = calloc,
    .realloc = _cstr_counting_realloc,
    .free = free};

// The capacity alone can't show that no realloc was made, so this counts the
// calls.
TEST(cstrings, reset_at_minimum_capacity_skips_realloc) {
  cstr s = cstring_create_full(NULL, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_realloc_call_count = 0;
  cstring_reset(s);
  REQUIRE_EQ(g_cstr_realloc_call_count, 0);
  REQUIRE_EQ(cstring_length(s), (size_t)0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  cstring_destroy(s);
}

TEST(cstrings, reset_above_minimum_capacity_still_reallocs_once) {
  cstr s = cstring_create_full(NULL, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);

  const char *chunk = "0123456789abcdef0123456789abcdef";
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  REQUIRE_TRUE(cstring__get_capacity(s) > (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_realloc_call_count = 0;
  cstring_reset(s);
  REQUIRE_EQ(g_cstr_realloc_call_count, 1);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  cstring_destroy(s);
}

TEST(cstrings, reserve_basic) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), CSTRING_MIN_CAPACITY);

  REQUIRE_TRUE(cstring_reserve(s, 100));
  REQUIRE_EQ(cstring__get_capacity(s), 128);
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, reserve_power_of_two_rounding) {
  cstr s = cstring_create(NULL, NULL);

  REQUIRE_TRUE(cstring_reserve(s, 7));
  REQUIRE_EQ(cstring__get_capacity(s), 16);

  REQUIRE_TRUE(cstring_reserve(s, 17));
  REQUIRE_EQ(cstring__get_capacity(s), 32);

  REQUIRE_TRUE(cstring_reserve(s, 32));
  REQUIRE_EQ(cstring__get_capacity(s), 32);

  REQUIRE_TRUE(cstring_reserve(s, 33));
  REQUIRE_EQ(cstring__get_capacity(s), 64);

  cstring_destroy(s);
}

TEST(cstrings, reserve_does_not_shrink) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_reserve(s, 128));
  REQUIRE_EQ(cstring__get_capacity(s), 128);

  REQUIRE_TRUE(cstring_reserve(s, 16));
  REQUIRE_EQ(cstring__get_capacity(s), 128);
  cstring_destroy(s);
}

TEST(cstrings, reserve_below_minimum) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_reserve(s, 1));
  REQUIRE_EQ(cstring__get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, reserve_preserves_content) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_TRUE(cstring_reserve(s, 256));
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

TEST(cstrings, to_upper_basic) {
  cstr s = cstring_create("hello world", NULL);
  cstring_to_upper(s);
  REQUIRE_STREQ(cstring_c_str(s), "HELLO WORLD");
  cstring_destroy(s);
}

TEST(cstrings, to_lower_basic) {
  cstr s = cstring_create("HELLO WORLD", NULL);
  cstring_to_lower(s);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, case_conversion_round_trip) {
  cstr s = cstring_create("Hello World", NULL);
  cstring_to_upper(s);
  REQUIRE_STREQ(cstring_c_str(s), "HELLO WORLD");
  cstring_to_lower(s);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, case_conversion_leaves_non_alpha) {
  cstr s = cstring_create("abc123!@#", NULL);
  cstring_to_upper(s);
  REQUIRE_STREQ(cstring_c_str(s), "ABC123!@#");
  cstring_destroy(s);
}

TEST(cstrings, case_conversion_empty_string) {
  cstr s = cstring_create(NULL, NULL);
  cstring_to_upper(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  cstring_to_lower(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  cstring_destroy(s);
}

TEST(cstrings, trim_leading_whitespace) {
  cstr s = cstring_create("   hello", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

TEST(cstrings, trim_trailing_whitespace) {
  cstr s = cstring_create("hello   ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

TEST(cstrings, trim_both_sides) {
  cstr s = cstring_create("  \t hello world \n  ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, trim_no_whitespace) {
  cstr s = cstring_create("hello", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, trim_all_whitespace) {
  cstr s = cstring_create("   \t\n  ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, trim_empty_string) {
  cstr s = cstring_create(NULL, NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  cstring_destroy(s);
}

TEST(cstrings, trim_preserves_internal_whitespace) {
  cstr s = cstring_create("  hello   world  ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "hello   world");
  cstring_destroy(s);
}

TEST(cstrings, compare_less_than) {
  cstr s = cstring_create("apple", NULL);
  REQUIRE_TRUE(cstring_compare(s, "banana") < 0);
  cstring_destroy(s);
}

TEST(cstrings, compare_equal) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_compare(s, "hello"), 0);
  cstring_destroy(s);
}

TEST(cstrings, compare_greater_than) {
  cstr s = cstring_create("zebra", NULL);
  REQUIRE_TRUE(cstring_compare(s, "apple") > 0);
  cstring_destroy(s);
}

TEST(cstrings, compare_empty_with_nonempty) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_compare(s, "a") < 0);
  REQUIRE_EQ(cstring_compare(s, ""), 0);
  cstring_destroy(s);
}

TEST(cstrings, compare_null_str) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_compare(s, NULL), 1);
  cstring_destroy(s);
}

TEST(cstrings, equals_match) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_TRUE(cstring_equals(s, "hello"));
  cstring_destroy(s);
}

TEST(cstrings, equals_no_match) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_FALSE(cstring_equals(s, "world"));
  REQUIRE_FALSE(cstring_equals(s, "hell"));
  REQUIRE_FALSE(cstring_equals(s, "helloo"));
  cstring_destroy(s);
}

TEST(cstrings, equals_null) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_FALSE(cstring_equals(s, NULL));
  cstring_destroy(s);
}

TEST(cstrings, equals_empty_strings) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_equals(s, ""));
  REQUIRE_FALSE(cstring_equals(s, "x"));
  cstring_destroy(s);
}

TEST(cstrings, starts_with_true) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_TRUE(cstring_starts_with(s, "hello"));
  REQUIRE_TRUE(cstring_starts_with(s, "hello world"));
  cstring_destroy(s);
}

TEST(cstrings, starts_with_false) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_FALSE(cstring_starts_with(s, "world"));
  REQUIRE_FALSE(cstring_starts_with(s, "Hello"));
  cstring_destroy(s);
}

TEST(cstrings, starts_with_empty_prefix) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_TRUE(cstring_starts_with(s, ""));
  cstring_destroy(s);
}

TEST(cstrings, starts_with_prefix_longer_than_string) {
  cstr s = cstring_create("hi", NULL);
  REQUIRE_FALSE(cstring_starts_with(s, "hello"));
  cstring_destroy(s);
}

TEST(cstrings, starts_with_null_prefix) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_FALSE(cstring_starts_with(s, NULL));
  cstring_destroy(s);
}

TEST(cstrings, ends_with_true) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_TRUE(cstring_ends_with(s, "world"));
  REQUIRE_TRUE(cstring_ends_with(s, "hello world"));
  cstring_destroy(s);
}

TEST(cstrings, ends_with_false) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_FALSE(cstring_ends_with(s, "hello"));
  REQUIRE_FALSE(cstring_ends_with(s, "World"));
  cstring_destroy(s);
}

TEST(cstrings, ends_with_empty_suffix) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_TRUE(cstring_ends_with(s, ""));
  cstring_destroy(s);
}

TEST(cstrings, ends_with_suffix_longer_than_string) {
  cstr s = cstring_create("hi", NULL);
  REQUIRE_FALSE(cstring_ends_with(s, "hello"));
  cstring_destroy(s);
}

TEST(cstrings, ends_with_null_suffix) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_FALSE(cstring_ends_with(s, NULL));
  cstring_destroy(s);
}

TEST(cstrings, find_found) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_find(s, "world"), 6);
  cstring_destroy(s);
}

TEST(cstrings, find_at_start) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_find(s, "hello"), 0);
  cstring_destroy(s);
}

TEST(cstrings, find_single_char) {
  cstr s = cstring_create("abcabc", NULL);
  REQUIRE_EQ(cstring_find(s, "a"), 0);
  REQUIRE_EQ(cstring_find(s, "c"), 2);
  cstring_destroy(s);
}

TEST(cstrings, find_not_found) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_find(s, "xyz"), ccol_invalid_size);
  cstring_destroy(s);
}

TEST(cstrings, find_needle_longer_than_string) {
  cstr s = cstring_create("hi", NULL);
  REQUIRE_EQ(cstring_find(s, "hello"), ccol_invalid_size);
  cstring_destroy(s);
}

TEST(cstrings, find_null_needle) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_find(s, NULL), ccol_invalid_size);
  cstring_destroy(s);
}

TEST(cstrings, rfind_last_occurrence) {
  cstr s = cstring_create("abcabc", NULL);
  REQUIRE_EQ(cstring_rfind(s, "a"), 3);
  REQUIRE_EQ(cstring_rfind(s, "bc"), 4);
  cstring_destroy(s);
}

TEST(cstrings, rfind_single_occurrence) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_rfind(s, "world"), 6);
  cstring_destroy(s);
}

TEST(cstrings, rfind_not_found) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_rfind(s, "xyz"), ccol_invalid_size);
  cstring_destroy(s);
}

TEST(cstrings, rfind_null_needle) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_rfind(s, NULL), ccol_invalid_size);
  cstring_destroy(s);
}

TEST(cstrings, find_rfind_first_vs_last) {
  cstr s = cstring_create("one two one two one", NULL);
  REQUIRE_EQ(cstring_find(s, "one"), 0);
  REQUIRE_EQ(cstring_rfind(s, "one"), 16);
  cstring_destroy(s);
}

TEST(cstrings, find_empty_needle) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_find(s, ""), (size_t)0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_empty_needle) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_rfind(s, ""), cstring_length(s));
  cstring_destroy(s);
}

TEST(cstrings, replace_basic) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_replace(s, "world", "there"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello there");
  REQUIRE_EQ(cstring_length(s), 11);
  cstring_destroy(s);
}

TEST(cstrings, replace_no_occurrences) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_replace(s, "xyz", "abc"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, replace_multiple_occurrences) {
  cstr s = cstring_create("aababaa", NULL);
  REQUIRE_EQ(cstring_replace(s, "a", "x"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "xxbxbxx");
  cstring_destroy(s);
}

TEST(cstrings, replace_with_shorter) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_replace(s, "world", "C"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello C");
  REQUIRE_EQ(cstring_length(s), 7);
  cstring_destroy(s);
}

TEST(cstrings, replace_with_longer) {
  cstr s = cstring_create("hi", NULL);
  REQUIRE_EQ(cstring_replace(s, "hi", "hello world"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstring_destroy(s);
}

TEST(cstrings, replace_deletes_occurrences) {
  cstr s = cstring_create("he_llo_wor_ld", NULL);
  REQUIRE_EQ(cstring_replace(s, "_", ""), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "helloworld");
  cstring_destroy(s);
}

TEST(cstrings, replace_invalid_args) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_replace(s, NULL, "x"), ccol_invalid_args);
  REQUIRE_EQ(cstring_replace(s, "", "x"), ccol_invalid_args);
  REQUIRE_EQ(cstring_replace(s, "x", NULL), ccol_invalid_args);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, replace_entire_string) {
  cstr s = cstring_create("old", NULL);
  REQUIRE_EQ(cstring_replace(s, "old", "brand new content"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "brand new content");
  cstring_destroy(s);
}

// The replace length tests drive compute_replace_new_length() through its test
// hook with made-up sizes; reaching its overflow guards through
// cstring_replace() would take strings of several gigabytes.
TEST(cstrings, replace_new_length_normal_growth_case) {
  size_t new_len = 0;
  ccol_retval rv =
      cstring__replace_compute_new_length_for_tests(10, 2, 5, 3, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)19);
}

TEST(cstrings, replace_new_length_normal_shrink_case) {
  size_t new_len = 0;
  ccol_retval rv =
      cstring__replace_compute_new_length_for_tests(20, 5, 2, 2, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)14);
}

TEST(cstrings, replace_new_length_multiplication_overflow_detected) {
  size_t new_len = 12345;
  size_t nlen = 1;
  size_t rlen = (SIZE_MAX / 2) + 2;
  ccol_retval rv =
      cstring__replace_compute_new_length_for_tests(0, nlen, rlen, 2, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

TEST(cstrings, replace_new_length_addition_overflow_detected) {
  size_t new_len = 12345;
  ccol_retval rv = cstring__replace_compute_new_length_for_tests(SIZE_MAX, 1, 2,
                                                                 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

// Here orig_length + added is exactly SIZE_MAX, which the wraparound check
// alone misses; the result must leave room for the '\0'.
TEST(cstrings, replace_new_length_result_exactly_size_max_detected) {
  size_t new_len = 12345;
  ccol_retval rv = cstring__replace_compute_new_length_for_tests(
      SIZE_MAX - 1, 1, 2, 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

TEST(cstrings, length_fits_with_terminator_guard) {
  REQUIRE_TRUE(cstring__length_fits_with_terminator_for_tests(0));
  REQUIRE_TRUE(cstring__length_fits_with_terminator_for_tests(1));
  REQUIRE_TRUE(cstring__length_fits_with_terminator_for_tests(SIZE_MAX - 1));
  REQUIRE_FALSE(cstring__length_fits_with_terminator_for_tests(SIZE_MAX));
}

TEST(cstrings, substring_basic) {
  cstr s = cstring_create("hello world", NULL);
  cstr sub = cstring_substring(s, 6, 5, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "world");
  REQUIRE_EQ(cstring_length(sub), 5);
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, substring_from_start) {
  cstr s = cstring_create("hello world", NULL);
  cstr sub = cstring_substring(s, 0, 5, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "hello");
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, substring_clamped_length) {
  cstr s = cstring_create("hello", NULL);
  cstr sub = cstring_substring(s, 2, 100, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "llo");
  REQUIRE_EQ(cstring_length(sub), 3);
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, substring_full_string) {
  cstr s = cstring_create("hello", NULL);
  cstr sub = cstring_substring(s, 0, 5, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "hello");
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, substring_start_at_or_beyond_length) {
  cstr s = cstring_create("hello", NULL);
  cstr sub = cstring_substring(s, 5, 1, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "");
  cstring_destroy(sub);
  sub = cstring_substring(s, 100, 1, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "");
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, substring_of_empty_string) {
  cstr s = cstring_create(NULL, NULL);
  cstr sub = cstring_substring(s, 0, 3, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "");
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, substring_zero_length) {
  cstr s = cstring_create("hello", NULL);
  cstr sub = cstring_substring(s, 2, 0, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "");
  REQUIRE_EQ(cstring_length(sub), (size_t)0);
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, substring_is_independent) {
  cstr s = cstring_create("hello world", NULL);
  cstr sub = cstring_substring(s, 0, 5, NULL);
  cstring_set(s, "aaaaa world");
  REQUIRE_STREQ(cstring_c_str(sub), "hello");
  cstring_destroy(sub);
  cstring_destroy(s);
}

TEST(cstrings, copy_basic) {
  cstr s = cstring_create("hello", NULL);
  cstr c = cstring_copy(s, NULL);
  REQUIRE_NE((void *)c, NULL);
  REQUIRE_STREQ(cstring_c_str(c), "hello");
  REQUIRE_EQ(cstring_length(c), 5);
  cstring_destroy(c);
  cstring_destroy(s);
}

TEST(cstrings, copy_is_independent) {
  cstr s = cstring_create("hello", NULL);
  cstr c = cstring_copy(s, NULL);
  cstring_set(s, "world");
  REQUIRE_STREQ(cstring_c_str(c), "hello");
  REQUIRE_STREQ(cstring_c_str(s), "world");
  cstring_destroy(c);
  cstring_destroy(s);
}

TEST(cstrings, copy_of_empty_string) {
  cstr s = cstring_create(NULL, NULL);
  cstr c = cstring_copy(s, NULL);
  REQUIRE_NE((void *)c, NULL);
  REQUIRE_STREQ(cstring_c_str(c), "");
  cstring_destroy(c);
  cstring_destroy(s);
}

// Destroys every token of a split result and the vector, for the split tests.
static void free_cstr_vector(cvec parts) {
  for (size_t i = 0; i < cvector_size(parts); i++) {
    cstr *p = (cstr *)cvector_at(parts, i);
    cstring_destroy(*p);
  }
  cvector_destroy(parts);
}

TEST(cstrings, split_basic) {
  cstr s = cstring_create("a,b,c", NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_size(parts), 3);

  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "a");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 1)), "b");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 2)), "c");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_multi_char_delimiter) {
  cstr s = cstring_create("one::two::three", NULL);
  cvec parts = cstring_split(s, "::", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_size(parts), 3);

  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "one");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 1)), "two");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 2)), "three");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_trailing_delimiter) {
  cstr s = cstring_create("a,b,", NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_size(parts), 3);

  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "a");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 1)), "b");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 2)), "");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_leading_delimiter) {
  cstr s = cstring_create(",a,b", NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_size(parts), 3);

  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 1)), "a");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 2)), "b");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_no_delimiter_found) {
  cstr s = cstring_create("hello", NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_size(parts), 1);
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "hello");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_consecutive_delimiters) {
  cstr s = cstring_create("a,,b", NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_size(parts), 3);

  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "a");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 1)), "");
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 2)), "b");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_empty_source_string) {
  cstr s = cstring_create(NULL, NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_size(parts), 1);
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_null_delimiter_fails) {
  char *err = NULL;
  cstr s = cstring_create("hello", NULL);
  cvec parts = cstring_split(s, NULL, &err);

  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)err, NULL);

  cstring_destroy(s);
}

TEST(cstrings, split_empty_delimiter_fails) {
  char *err = NULL;
  cstr s = cstring_create("hello", NULL);
  cvec parts = cstring_split(s, "", &err);

  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)err, NULL);

  cstring_destroy(s);
}

TEST(cstrings, capacity_initial_is_minimum) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, capacity_large_initial_rounds_up) {
  cstr s = cstring_create("012345678901234567890123456789", NULL);
  REQUIRE_EQ(cstring_length(s), 30);
  size_t cap = cstring__get_capacity(s);
  REQUIRE_TRUE(cap >= 31);
  REQUIRE_EQ(cap & (cap - 1), 0);
  cstring_destroy(s);
}

TEST(cstrings, capacity_always_power_of_two) {
  cstr s = cstring_create(NULL, NULL);
  const char *chunk = "0123456789";

  for (int i = 0; i < 10; i++) {
    cstring_append(s, chunk);
    size_t cap = cstring__get_capacity(s);
    REQUIRE_TRUE(cap > 0);
    REQUIRE_EQ(cap & (cap - 1), 0);
  }
  cstring_destroy(s);
}

TEST(cstrings, construct_macro) {
  cstr_construct(s, "hello");
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstr_destroy(s);
  REQUIRE_EQ((void *)s, NULL);
}

TEST(cstrings, construct_null_initial_macro) {
  cstr_construct(s, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_TRUE(cstring_is_empty(s));
  cstr_destroy(s);
}

TEST(cstrings, declare_and_init_macro) {
  cstr_declare(s);
  cstr_init(s, "world");
  REQUIRE_STREQ(cstring_c_str(s), "world");
  cstr_destroy(s);
}

TEST(cstrings, construct_mp_macro) {
  cstr_construct_mp(s, "test",
                    (&(ccol_memmgmt_procs){.malloc = malloc,
                                           .free = free,
                                           .calloc = calloc,
                                           .realloc = realloc}));
  REQUIRE_STREQ(cstring_c_str(s), "test");
  cstr_destroy(s);
}

TEST(cstrings, construct_scoped_macro) {
  {
    cstr_construct_scoped(s, "scoped");
    REQUIRE_STREQ(cstring_c_str(s), "scoped");
  }
}

TEST(cstrings, append_macro) {
  cstr_construct(s, "hello");
  cstr_append(s, " world");
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstr_destroy(s);
}

TEST(cstrings, prepend_macro) {
  cstr_construct(s, "world");
  cstr_prepend(s, "hello ");
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstr_destroy(s);
}

TEST(cstrings, insert_macro) {
  cstr_construct(s, "helloworld");
  cstr_insert(s, 5, " ");
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  cstr_destroy(s);
}

TEST(cstrings, set_macro) {
  cstr_construct(s, "old");
  cstr_set(s, "new");
  REQUIRE_STREQ(cstring_c_str(s), "new");
  cstr_destroy(s);
}

TEST(cstrings, replace_macro) {
  cstr_construct(s, "foo foo foo");
  cstr_replace(s, "foo", "bar");
  REQUIRE_STREQ(cstring_c_str(s), "bar bar bar");
  cstr_destroy(s);
}

TEST(cstrings, reserve_macro) {
  cstr_construct(s, NULL);
  cstr_reserve(s, 256);
  REQUIRE_EQ(cstring__get_capacity(s), 256);
  cstr_destroy(s);
}

TEST(cstrings, query_macros) {
  cstr_construct(s, "hello");

  REQUIRE_EQ(cstr_length(s), 5);
  REQUIRE_STREQ(cstr_c_str(s), "hello");
  REQUIRE_EQ(cstr_at(s, 0), 'h');
  REQUIRE_EQ(cstr_at(s, 4), 'o');
  REQUIRE_FALSE(cstr_is_empty(s));

  cstr_reset(s);
  REQUIRE_TRUE(cstr_is_empty(s));

  cstr_destroy(s);
}

TEST(cstrings, case_macros) {
  cstr_construct(s, "Hello World");
  cstr_to_upper(s);
  REQUIRE_STREQ(cstr_c_str(s), "HELLO WORLD");
  cstr_to_lower(s);
  REQUIRE_STREQ(cstr_c_str(s), "hello world");
  cstr_destroy(s);
}

TEST(cstrings, trim_macro) {
  cstr_construct(s, "  hello  ");
  cstr_trim(s);
  REQUIRE_STREQ(cstr_c_str(s), "hello");
  cstr_destroy(s);
}

TEST(cstrings, search_macros) {
  cstr_construct(s, "hello world");

  REQUIRE_TRUE(cstr_equals(s, "hello world"));
  REQUIRE_EQ(cstr_compare(s, "hello world"), 0);
  REQUIRE_TRUE(cstr_starts_with(s, "hello"));
  REQUIRE_TRUE(cstr_ends_with(s, "world"));
  REQUIRE_EQ(cstr_find(s, "world"), 6);
  REQUIRE_EQ(cstr_rfind(s, "l"), 9);

  cstr_destroy(s);
}

TEST(cstrings, build_csv_line) {
  cstr_construct(line, NULL);
  const char *fields[] = {"Alice", "30", "Engineer"};
  for (int i = 0; i < 3; i++) {
    if (i > 0) cstr_append(line, ",");
    cstr_append(line, fields[i]);
  }
  REQUIRE_STREQ(cstring_c_str(line), "Alice,30,Engineer");
  cstr_destroy(line);
}

TEST(cstrings, round_trip_split_and_join) {
  cstr s = cstring_create("one:two:three", NULL);
  cvec parts = cstring_split(s, ":", NULL);
  REQUIRE_EQ(cvector_size(parts), 3);

  cstr_construct(joined, NULL);
  for (size_t i = 0; i < cvector_size(parts); i++) {
    if (i > 0) cstr_append(joined, ":");
    cstr *p = (cstr *)cvector_at(parts, i);
    cstr_append(joined, cstring_c_str(*p));
  }
  REQUIRE_STREQ(cstring_c_str(joined), "one:two:three");

  cstr_destroy(joined);
  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, transform_and_trim_pipeline) {
  cstr_construct(s, "  Hello World  ");
  cstr_trim(s);
  cstr_to_lower(s);
  cstr_replace(s, " ", "_");
  REQUIRE_STREQ(cstring_c_str(s), "hello_world");
  cstr_destroy(s);
}

TEST(cstrings, many_appends_correctness) {
  cstr_construct(s, NULL);
  char expected[1001];
  expected[0] = '\0';

  for (int i = 0; i < 100; i++) {
    cstr_append(s, "ab");
    strcat(expected, "ab");
  }

  REQUIRE_EQ(cstring_length(s), 200);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  cstr_destroy(s);
}

// In the self-alias tests the argument points into the string itself. A string
// of 9 bytes or more doubled passes the 16-byte minimum, so the buffer moves
// while the source points into it.
TEST(cstrings, append_self_alias_triggers_realloc) {
  cstr_construct(s, "123456789");
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, append_suffix_alias_no_realloc) {
  cstr_construct(s, "123456789");
  ccol_retval rv = cstring_append(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12345678956789");
  cstr_destroy(s);
}

TEST(cstrings, append_self_alias_no_realloc) {
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_triggers_realloc) {
  cstr_construct(s, "123456789");
  ccol_retval rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, prepend_suffix_alias_no_realloc) {
  cstr_construct(s, "123456789");
  ccol_retval rv = cstring_prepend(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "56789123456789");
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_no_realloc) {
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_triggers_realloc) {
  cstr_construct(s, "123456789");
  ccol_retval rv = cstring_insert(s, 0, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_after_pos_no_realloc) {
  cstr_construct(s, "123456789");
  ccol_retval rv = cstring_insert(s, 2, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12567893456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_overlapping_dst_gt_src) {
  cstr_construct(s, "abcde");
  ccol_retval rv = cstring_insert(s, 3, cstring_c_str(s) + 1);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "abcbcdede");
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_at_nonzero_pos_no_realloc) {
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval rv = cstring_insert(s, 2, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hehellollo");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

// A non-zero alias offset together with a real move of the buffer, which is the
// case the second pointer computation after the growth exists for.
TEST(cstrings, prepend_alias_offset_nonzero_triggers_realloc) {
  const char *base = "12345678901234567890";
  char expected[64];
  snprintf(expected, sizeof(expected), "%s%s", base + 5, base);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval rv = cstring_prepend(s, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), (size_t)35);

  cstring_destroy(s);
}

// alias_off 5 is past pos 3, so the source shifts with the memmove and needs
// its second correction, on top of a real reallocation.
TEST(cstrings, insert_alias_offset_nonzero_triggers_realloc) {
  const char *base = "12345678901234567890";
  const char *inserted = base + 5;
  size_t pos = 3;

  char expected[64];
  size_t blen = strlen(base), ilen = strlen(inserted);
  memcpy(expected, base, pos);
  memcpy(expected + pos, inserted, ilen);
  memcpy(expected + pos + ilen, base + pos, blen - pos + 1);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval rv = cstring_insert(s, pos, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// alias_off 2 is before pos 10, so the memmove never touches the source and no
// second correction is made, even when the buffer moves.
TEST(cstrings, insert_alias_offset_less_than_pos_triggers_realloc) {
  const char *base = "12345678901234567890";
  const char *inserted = base + 2;
  size_t pos = 10;

  char expected[64];
  size_t blen = strlen(base), ilen = strlen(inserted);
  memcpy(expected, base, pos);
  memcpy(expected + pos, inserted, ilen);
  memcpy(expected + pos + ilen, base + pos, blen - pos + 1);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval rv = cstring_insert(s, pos, cstring_c_str(s) + 2);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// alias_off equal to pos sits on the boundary of the shift, which never touches
// the source; the correction uses > pos on purpose.
TEST(cstrings, insert_alias_offset_equal_to_pos_triggers_realloc) {
  const char *base = "12345678901234567890";
  const char *inserted = base + 5;
  size_t pos = 5;

  char expected[64];
  size_t blen = strlen(base), ilen = strlen(inserted);
  memcpy(expected, base, pos);
  memcpy(expected + pos, inserted, ilen);
  memcpy(expected + pos + ilen, base + pos, blen - pos + 1);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval rv = cstring_insert(s, pos, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

TEST(cstrings, set_self_alias_no_realloc_verified_by_call_count) {
  cstr s =
      cstring_create_full("1234567890123456", &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(g_cstr_realloc_call_count, 0);
  REQUIRE_STREQ(cstring_c_str(s), "1234567890123456");
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)32);

  cstring_destroy(s);
}

TEST(cstrings, cstring_new_function) {
  cstr s = cstring_new("hello");
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_length(s), 5);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);

  cstr empty = cstring_new(NULL);
  REQUIRE_NE((void *)empty, NULL);
  REQUIRE_EQ(cstring_length(empty), 0);
  REQUIRE_STREQ(cstring_c_str(empty), "");
  cstring_destroy(empty);
}

TEST(cstrings, get_mprocs_null_for_default) {
  cstr s = cstring_new("hello");
  REQUIRE_EQ((void *)cstring_get_mprocs(s), NULL);
  cstring_destroy(s);
}

TEST(cstrings, get_mprocs_returns_custom) {
  ccol_memmgmt_procs mp = {
      .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
  char *err = NULL;
  cstr s = cstring_create_full("hello", &mp, &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_NE((void *)cstring_get_mprocs(s), NULL);
  cstring_destroy(s);
}

TEST(cstrings, construct_mp_scoped_macro) {
  {
    cstr_construct_mp_scoped(s, "scoped",
                             (&(ccol_memmgmt_procs){.malloc = malloc,
                                                    .free = free,
                                                    .calloc = calloc,
                                                    .realloc = realloc}));
    REQUIRE_STREQ(cstring_c_str(s), "scoped");
  }
}

TEST(cstrings, trim_single_whitespace_char) {
  cstr s = cstring_create(" ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_overlapping_needles) {
  cstr s = cstring_create("ababa", NULL);
  REQUIRE_EQ(cstring_rfind(s, "aba"), (size_t)2);
  cstring_destroy(s);
}

TEST(cstrings, replace_needle_equals_replacement) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_EQ(cstring_replace(s, "hello", "hello"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello world");
  REQUIRE_EQ(cstring_length(s), 11);
  cstring_destroy(s);
}

TEST(cstrings, replace_with_self_as_replacement) {
  cstr s = cstring_create("axb", NULL);
  REQUIRE_EQ(cstring_replace(s, "x", cstring_c_str(s)), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "aaxbb");
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

TEST(cstrings, substring_macro) {
  cstr_construct(s, "hello world");
  cstr sub = cstr_substring(s, 6, 5);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "world");
  REQUIRE_EQ(cstring_length(sub), 5);
  cstr_destroy(sub);
  cstr_destroy(s);
}

// A pos argument named like the macro's internal local would bind to that
// uninitialized local, and a ccol_retval converts to size_t without a warning,
// so the insert would land at a garbage position.
TEST(cstrings, insert_macro_pos_argument_named__r_is_not_shadowed) {
  cstr_construct(s, "hello");
  size_t _r = 2;
  cstr_insert(s, _r, "X");
  REQUIRE_STREQ(cstring_c_str(s), "heXllo");
  cstr_destroy(s);
}

// Runs fn in a forked child with stdout and stderr on /dev/null, for the NULL
// argument tests whose assert ends the child. Stores the wait status in
// *out_status and gives true, or gives false without touching it when fork() or
// waitpid() fails, so that isn't mistaken for a test failure.
static bool run_forked(void (*fn)(void), int *out_status) {
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    fn();
    test_child_exit(0);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return false;
  *out_status = status;
  return true;
}

// Defines a test that runs callexpr in a forked child and expects it to die
// with SIGABRT. Every query, change and search function asserts on a NULL cstr;
// without the guard the child dies of SIGSEGV instead.
#define DEFINE_NULL_ARG_FATAL_TEST(test_name, callexpr)             \
  static void _null_arg_thunk_##test_name(void) { callexpr; }       \
  TEST(cstrings, test_name) {                                       \
    int status = 0;                                                 \
    REQUIRE_TRUE(run_forked(_null_arg_thunk_##test_name, &status)); \
    REQUIRE_TRUE(WIFSIGNALED(status));                              \
    REQUIRE_EQ(WTERMSIG(status), SIGABRT);                          \
  }

DEFINE_NULL_ARG_FATAL_TEST(null_length_is_fatal, (void)cstring_length(NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_c_str_is_fatal, (void)cstring_c_str(NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_at_is_fatal, (void)cstring_at(NULL, 0))
DEFINE_NULL_ARG_FATAL_TEST(null_is_empty_is_fatal, (void)cstring_is_empty(NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_append_is_fatal,
                           (void)cstring_append(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_prepend_is_fatal,
                           (void)cstring_prepend(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_insert_is_fatal,
                           (void)cstring_insert(NULL, 0, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_set_is_fatal, (void)cstring_set(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_reset_is_fatal, cstring_reset(NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_reserve_is_fatal,
                           (void)cstring_reserve(NULL, 16))
DEFINE_NULL_ARG_FATAL_TEST(null_to_upper_is_fatal, cstring_to_upper(NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_to_lower_is_fatal, cstring_to_lower(NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_trim_is_fatal, cstring_trim(NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_replace_is_fatal,
                           (void)cstring_replace(NULL, "a", "b"))
DEFINE_NULL_ARG_FATAL_TEST(null_compare_is_fatal,
                           (void)cstring_compare(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_equals_is_fatal,
                           (void)cstring_equals(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_starts_with_is_fatal,
                           (void)cstring_starts_with(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_ends_with_is_fatal,
                           (void)cstring_ends_with(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_find_is_fatal, (void)cstring_find(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_rfind_is_fatal, (void)cstring_rfind(NULL, "x"))
DEFINE_NULL_ARG_FATAL_TEST(null_substring_is_fatal,
                           (void)cstring_substring(NULL, 0, 1, NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_copy_is_fatal, (void)cstring_copy(NULL, NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_split_is_fatal,
                           (void)cstring_split(NULL, ",", NULL))
DEFINE_NULL_ARG_FATAL_TEST(null_get_mprocs_is_fatal,
                           (void)cstring_get_mprocs(NULL))

// An allocator with a budget: The first g_cstr_oom_budget calls of malloc,
// calloc and realloc go to the system heap and later ones give NULL, while free
// always works. The out-of-memory tests use it with exact budgets: A create
// costs 2 calls (block and buffer), a real growth 1 more, and the vector of a
// split 2.
static int g_cstr_oom_budget = 0;
static void *_cstr_oom_malloc(size_t size) {
  if (g_cstr_oom_budget <= 0) return NULL;
  g_cstr_oom_budget--;
  return malloc(size);
}
static void *_cstr_oom_calloc(size_t count, size_t size) {
  if (g_cstr_oom_budget <= 0) return NULL;
  g_cstr_oom_budget--;
  return calloc(count, size);
}
static void *_cstr_oom_realloc(void *ptr, size_t size) {
  if (g_cstr_oom_budget <= 0) return NULL;
  g_cstr_oom_budget--;
  return realloc(ptr, size);
}
static void _cstr_oom_free(void *ptr) { free(ptr); }

static ccol_memmgmt_procs g_cstr_oom_procs = {.malloc = _cstr_oom_malloc,
                                              .calloc = _cstr_oom_calloc,
                                              .realloc = _cstr_oom_realloc,
                                              .free = _cstr_oom_free};

TEST(cstrings, create_full_oom_container_alloc_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 0;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_full_oom_data_buffer_alloc_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 1;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// The procs copy lives in the string's block, so 2 calls are enough; a separate
// allocation for it would fail here.
TEST(cstrings, create_full_oom_budget_exactly_sufficient_succeeds) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  int left = g_cstr_oom_budget;
  bool ok = s != NULL && err == NULL && strcmp(cstring_c_str(s), "hello") == 0;
  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(left, 0);
}

TEST(cstrings, the_procs_copy_outlives_the_callers_struct) {
  ccol_memmgmt_procs *mp = malloc(sizeof(*mp));
  REQUIRE_NE((void *)mp, NULL);
  *mp = g_cstr_oom_procs;
  g_cstr_oom_budget = 1000000;
  cstr s = cstring_create_full("hello", mp, NULL);
  cstr sub = s ? cstring_substring(s, 0, 4, NULL) : NULL;
  ccol_memmgmt_procs *kept = s ? cstring_get_mprocs(s) : NULL;
  bool copied = kept != NULL && kept != mp &&
                memcmp(kept, &g_cstr_oom_procs, sizeof(*kept)) == 0;
  free(mp);
  int before = g_cstr_oom_budget;
  bool grown = s != NULL &&
               cstring_append(s, "0123456789abcdef0123456789") == ccol_success;
  int used = before - g_cstr_oom_budget;
  cstring_destroy(s);
  bool sub_ok = sub != NULL && cstring_append(sub, "!") == ccol_success &&
                strcmp(cstring_c_str(sub), "hell!") == 0;
  cstring_destroy(sub);
  REQUIRE_TRUE(copied);
  REQUIRE_TRUE(grown);
  REQUIRE_EQ(used, 1);
  REQUIRE_TRUE(sub_ok);
}

TEST(cstrings, an_interned_procs_pointer_is_kept_as_it_is) {
  ccol_memmgmt_procs *interned = NULL;
  REQUIRE_EQ(ccol_procs_intern(&g_cstr_oom_procs, &interned), ccol_success);
  REQUIRE_NE((void *)interned, NULL);
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", interned, NULL);
  int left = g_cstr_oom_budget;
  bool same = s != NULL && cstring_get_mprocs(s) == interned;
  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
  REQUIRE_TRUE(same);
  REQUIRE_EQ(left, 0);
}

TEST(cstrings, append_oom_grow_failure_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  ccol_retval rv = cstring_append(s, "0123456789abcdef0123456789");
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), (size_t)5);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, prepend_oom_grow_failure_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  ccol_retval rv = cstring_prepend(s, "0123456789abcdef0123456789");
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_STREQ(cstring_c_str(s), "hello");

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, insert_oom_grow_failure_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  ccol_retval rv = cstring_insert(s, 2, "0123456789abcdef0123456789");
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_STREQ(cstring_c_str(s), "hello");

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, set_oom_grow_failure_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  ccol_retval rv = cstring_set(s, "0123456789abcdef0123456789");
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_STREQ(cstring_c_str(s), "hello");

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, reserve_oom_returns_false_capacity_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full(NULL, &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 0;
  REQUIRE_FALSE(cstring_reserve(s, 1024));
  REQUIRE_EQ(cstring__get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, replace_oom_new_buffer_alloc_fails_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello world hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  ccol_retval rv = cstring_replace(s, "hello", "X");
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_STREQ(cstring_c_str(s), "hello world hello");

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, substring_oom_result_creation_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello world", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  char *sub_err = NULL;
  cstr sub = cstring_substring(s, 0, 5, &sub_err);
  REQUIRE_EQ((void *)sub, NULL);
  REQUIRE_NE((void *)sub_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, substring_of_a_long_range_allocates_its_buffer_once) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("0123456789abcdefghij", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 2;
  char *sub_err = (char *)"poison";
  cstr sub = cstring_substring(s, 0, 20, &sub_err);
  int left = g_cstr_oom_budget;
  bool ok = sub != NULL;
  bool content_ok =
      ok && strcmp(cstring_c_str(sub), "0123456789abcdefghij") == 0;
  size_t cap = ok ? cstring__get_capacity(sub) : 0;

  g_cstr_oom_budget = 1;
  char *short_err = NULL;
  cstr none = cstring_substring(s, 0, 20, &short_err);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(sub);
  cstring_destroy(none);
  cstring_destroy(s);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(content_ok);
  REQUIRE_EQ(left, 0);
  REQUIRE_EQ((void *)sub_err, NULL);
  REQUIRE_EQ(cap, (size_t)32);
  REQUIRE_EQ((void *)none, NULL);
  REQUIRE_NE((void *)short_err, NULL);
}

TEST(cstrings, copy_oom_propagates_create_full_failure) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  char *copy_err = NULL;
  cstr c = cstring_copy(s, &copy_err);
  REQUIRE_EQ((void *)c, NULL);
  REQUIRE_NE((void *)copy_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

// Lifts the budget and destroys every token of a split result and the vector,
// so the out-of-memory tests can clean up whatever the call built.
static void free_cstr_vector_oom(cvec parts) {
  if (!parts) return;
  g_cstr_oom_budget = 1000000;
  for (size_t i = 0; i < cvector_size(parts); i++) {
    cstr *p = (cstr *)cvector_at(parts, i);
    cstring_destroy(*p);
  }
  cvector_destroy(parts);
}

TEST(cstrings, split_oom_vector_creation_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, split_oom_mid_loop_token_creation_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 4;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, split_oom_last_token_creation_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 6;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, split_allocates_a_long_token_once) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s =
      cstring_create_full("0123456789abcdef,short", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 6;
  char *split_err = (char *)"poison";
  cvec parts = cstring_split(s, ",", &split_err);
  int left = g_cstr_oom_budget;
  size_t count = parts ? cvector_size(parts) : 0;
  bool first_ok =
      count == 2 &&
      strcmp(cstring_c_str(*(cstr *)cvector_at(parts, 0)),
             "0123456789abcdef") == 0 &&
      strcmp(cstring_c_str(*(cstr *)cvector_at(parts, 1)), "short") == 0;
  free_cstr_vector_oom(parts);

  g_cstr_oom_budget = 5;
  char *short_err = NULL;
  cvec none = cstring_split(s, ",", &short_err);
  free_cstr_vector_oom(none);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
  REQUIRE_EQ(count, (size_t)2);
  REQUIRE_TRUE(first_ok);
  REQUIRE_EQ(left, 0);
  REQUIRE_EQ((void *)split_err, NULL);
  REQUIRE_EQ((void *)none, NULL);
  REQUIRE_NE((void *)short_err, NULL);
}

TEST(cstrings, split_oom_budget_exactly_sufficient_succeeds) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 8;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ((void *)split_err, NULL);
  REQUIRE_EQ(cvector_size(parts), (size_t)3);

  g_cstr_oom_budget = 1000000;
  free_cstr_vector_oom(parts);
  cstring_destroy(s);
}

TEST(cstrings, reset_realloc_failure_leaves_capacity_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 1000000;
  cstr s = cstring_create_full(NULL, &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  const char *chunk = "0123456789abcdef0123456789abcdef";
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  size_t grown_cap = cstring__get_capacity(s);
  REQUIRE_TRUE(grown_cap > CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 0;
  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), (size_t)0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring__get_capacity(s), grown_cap);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

// Gives the last needle in hay[0, hlen) by comparing every start from the right
// in full, as the reference that cstring_rfind() must agree with. It shares no
// code with the library.
static size_t rfind_reference(const char *hay, size_t hlen,
                              const char *needle) {
  size_t nlen = strlen(needle);
  if (nlen == 0) return hlen;
  if (nlen > hlen) return ccol_invalid_size;
  for (size_t i = hlen - nlen + 1; i-- > 0;) {
    if (memcmp(hay + i, needle, nlen) == 0) return i;
  }
  return ccol_invalid_size;
}

// The state of a small xorshift generator, so every run checks the same random
// inputs. rfind_rng_next() gives its next value.
static uint64_t g_rfind_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rfind_rng_next(void) {
  g_rfind_rng ^= g_rfind_rng << 13;
  g_rfind_rng ^= g_rfind_rng >> 7;
  g_rfind_rng ^= g_rfind_rng << 17;
  return g_rfind_rng;
}

// Fills buf with len random letters from the first alphabet letters and a '\0',
// for the random rfind inputs.
static void rfind_fill(char *buf, size_t len, unsigned alphabet) {
  for (size_t i = 0; i < len; i++) {
    buf[i] = (char)('a' + rfind_rng_next() % alphabet);
  }
  buf[len] = '\0';
}

TEST(rfind, agrees_with_the_reference_on_random_inputs) {
  char hay[1100];
  char needle[1120];
  size_t mismatches = 0;
  size_t checked = 0;
  for (int round = 0; round < 20000; round++) {
    unsigned alphabet = 1 + (unsigned)(rfind_rng_next() % 4);
    size_t hlen = (round & 1) ? 256 + (size_t)(rfind_rng_next() % 768)
                              : (size_t)(rfind_rng_next() % 64);
    rfind_fill(hay, hlen, alphabet);
    size_t nlen = (size_t)(rfind_rng_next() % 12);
    if (round % 3 == 0 && hlen > 0) {
      size_t from = (size_t)(rfind_rng_next() % hlen);
      nlen = 1 + (size_t)(rfind_rng_next() % (hlen - from));
      memcpy(needle, hay + from, nlen);
      needle[nlen] = '\0';
    } else {
      rfind_fill(needle, nlen, alphabet);
    }
    cstr s = cstring_create(hay, NULL);
    size_t got = cstring_rfind(s, needle);
    cstring_destroy(s);
    if (got != rfind_reference(hay, hlen, needle)) mismatches++;
    checked++;
  }
  REQUIRE_EQ(checked, (size_t)20000);
  REQUIRE_EQ(mismatches, (size_t)0);
}

// Long runs of 'a' with sparse other bytes push cstring_rfind() into its linear
// fallback, with the table on the stack and on the heap.
TEST(rfind, agrees_with_the_reference_on_long_periodic_inputs) {
  enum { HMAX = 3000, NMAX = 400 };
  char *hay = malloc(HMAX + 1);
  char *needle = malloc(NMAX + 1);
  REQUIRE_NE((void *)hay, NULL);
  REQUIRE_NE((void *)needle, NULL);
  size_t mismatches = 0;
  unsigned long before = cstring_rfind_linear_count_for_tests;
  for (int round = 0; round < 400; round++) {
    size_t hlen = 1 + (size_t)(rfind_rng_next() % HMAX);
    unsigned sparsity = 20 + (unsigned)(rfind_rng_next() % 400);
    for (size_t i = 0; i < hlen; i++) {
      uint64_t r = rfind_rng_next() % sparsity;
      hay[i] = r == 0 ? 'b' : r == 1 ? 'c' : 'a';
    }
    hay[hlen] = '\0';
    size_t nlen = 1 + (size_t)(rfind_rng_next() % NMAX);
    if (round % 2 == 0 && nlen <= hlen) {
      memcpy(needle, hay + rfind_rng_next() % (hlen - nlen + 1), nlen);
    } else {
      memset(needle, 'a', nlen);
      needle[rfind_rng_next() % nlen] = 'b';
    }
    needle[nlen] = '\0';
    cstr s = cstring_create(hay, NULL);
    if (cstring_rfind(s, needle) != rfind_reference(hay, hlen, needle)) {
      mismatches++;
    }
    cstring_destroy(s);
  }
  unsigned long handovers = cstring_rfind_linear_count_for_tests - before;
  free(hay);
  free(needle);
  REQUIRE_EQ(mismatches, (size_t)0);
  REQUIRE_GT(handovers, 50UL);
}

// Gives a malloc'd text of len 'a' bytes with a 'b' at b_at (none when b_at is
// past the end), or NULL. Every candidate then matches the needle's first and
// last byte and differs only in the middle, which drives the fallback.
static char *rfind_adversarial(size_t len, size_t b_at) {
  if (len >= PTRDIFF_MAX) return NULL;
  char *buf = malloc(len + 1);
  if (!buf) return NULL;
  memset(buf, 'a', len);
  if (b_at < len) buf[b_at] = 'b';
  buf[len] = '\0';
  return buf;
}

// The fallback keeps a short needle's table on the stack and takes a long one's
// from the allocator; when that's refused the scan finishes alone. Every answer
// must equal the reference, and the counter proves the fallback ran.
TEST(rfind, the_linear_fallback_agrees_with_the_reference) {
  struct {
    size_t hlen, text_b, nlen, needle_b;
  } cases[] = {
      {4096, 4096, 16, 8},      {4096, 100, 16, 8},      {4096, 4000, 16, 8},
      {20000, 20000, 600, 300}, {20000, 1500, 600, 300},
  };
  size_t mismatches = 0;
  unsigned long before = cstring_rfind_linear_count_for_tests;
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    char *hay = rfind_adversarial(cases[c].hlen, cases[c].text_b);
    char *needle = rfind_adversarial(cases[c].nlen, cases[c].needle_b);
    if (!hay || !needle) {
      free(hay);
      free(needle);
      mismatches++;
      continue;
    }
    cstr s = cstring_create(hay, NULL);
    if (cstring_rfind(s, needle) !=
        rfind_reference(hay, cases[c].hlen, needle)) {
      mismatches++;
    }
    cstring_destroy(s);

    g_cstr_oom_budget = 2;
    cstr t = cstring_create_full(NULL, &g_cstr_oom_procs, NULL);
    g_cstr_oom_budget = 1000000;
    if (!t || cstring_append(t, hay) != ccol_success) {
      mismatches++;
    } else {
      g_cstr_oom_budget = 0;
      if (cstring_rfind(t, needle) !=
          rfind_reference(hay, cases[c].hlen, needle)) {
        mismatches++;
      }
      g_cstr_oom_budget = 1000000;
    }
    cstring_destroy(t);
    free(hay);
    free(needle);
  }
  unsigned long handovers = cstring_rfind_linear_count_for_tests - before;
  REQUIRE_EQ(mismatches, (size_t)0);
  REQUIRE_EQ(handovers, 10UL);
}

TEST(rfind, a_match_at_the_right_end_is_found_by_the_first_candidate) {
  char *hay = rfind_adversarial(8192, 8192);
  char *needle = rfind_adversarial(512, 512);
  REQUIRE_NE((void *)hay, NULL);
  REQUIRE_NE((void *)needle, NULL);
  cstr s = cstring_create(hay, NULL);
  unsigned long before = cstring_rfind_linear_count_for_tests;
  size_t got = cstring_rfind(s, needle);
  unsigned long handovers = cstring_rfind_linear_count_for_tests - before;
  cstring_destroy(s);
  free(hay);
  free(needle);
  REQUIRE_EQ(got, (size_t)(8192 - 512));
  REQUIRE_EQ(handovers, 0UL);
}

// Runs fn in a forked child with stderr on a pipe and stores what it wrote in
// out, for the tests of a fatal error's message. Stores the wait status in
// *out_status and gives true, or false when fork(), pipe() or waitpid() fails.
static bool run_forked_capture_stderr(void (*fn)(void), char *out,
                                      size_t out_size, int *out_status) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    test_child_no_core();
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    fn();
    test_child_exit(0);
  }
  close(fds[1]);
  size_t used = 0;
  ssize_t n;
  while (used + 1 < out_size &&
         (n = read(fds[0], out + used, out_size - 1 - used)) > 0) {
    used += (size_t)n;
  }
  out[used] = '\0';
  close(fds[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return false;
  *out_status = status;
  return true;
}

// Makes cstr_substring() fail on an allocation with arguments that have side
// effects; the parent checks that the message shows the values used.
static void substring_failure_message_thunk(void) {
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello world", &g_cstr_oom_procs, NULL);
  if (!s) test_child_exit(3);
  g_cstr_oom_budget = 0;
  size_t start = 2, len = 3;
  cstr sub = cstr_substring(s, start++, len++);
  (void)sub;
}

TEST(macro_arguments, substring_failure_reports_the_arguments_it_used) {
  char msg[512];
  int status = 0;
  REQUIRE_TRUE(run_forked_capture_stderr(substring_failure_message_thunk, msg,
                                         sizeof(msg), &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
  REQUIRE_NE((void *)strstr(msg, "start=2, len=3"), NULL);
}

// Makes cstr_reserve() fail with a capacity argument that has a side effect;
// the parent checks that the message shows the value used.
static void reserve_failure_message_thunk(void) {
  cstr s = cstring_create("x", NULL);
  size_t cap = SIZE_MAX - 1;
  cstr_reserve(s, cap++);
}

TEST(macro_arguments, reserve_failure_reports_the_capacity_it_used) {
  char msg[512];
  char expected[64];
  snprintf(expected, sizeof(expected), "reserve %zu bytes",
           (size_t)(SIZE_MAX - 1));
  int status = 0;
  REQUIRE_TRUE(run_forked_capture_stderr(reserve_failure_message_thunk, msg,
                                         sizeof(msg), &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
  REQUIRE_NE((void *)strstr(msg, expected), NULL);
}

TEST(macro_arguments, destroy_evaluates_the_string_once) {
  cstr ss[4] = {NULL, NULL, NULL, NULL};
  ss[0] = cstring_create("a", NULL);
  ss[1] = cstring_create("b", NULL);
  ss[2] = cstring_create("c", NULL);
  cstr first = ss[0], second = ss[1];
  int k = 0;
  cstring_destroy(ss[k++]);
  int k_after_first = k;
  bool first_cleared = ss[0] == NULL;
  if (k_after_first == 1) {
    cstr_destroy(ss[k++]);
  }
  int k_after_second = k;
  bool second_cleared = ss[1] == NULL;
  bool third_kept = ss[2] != NULL;
  if (k_after_first != 1) {
    cstring__destroy(first);
    (void)second;
  } else {
    cstring_destroy(ss[2]);
  }
  REQUIRE_EQ(k_after_first, 1);
  REQUIRE_EQ(k_after_second, 2);
  REQUIRE_TRUE(first_cleared);
  REQUIRE_TRUE(second_cleared);
  REQUIRE_TRUE(third_kept);
}

// The slots after the two targets hold a sentinel string, so a macro that read
// its target again would find a string there and fail the test.
TEST(macro_arguments, init_evaluates_the_target_once) {
  cstr sentinel = cstring_create("sentinel", NULL);
  cstr ss[5] = {NULL, sentinel, sentinel, sentinel, sentinel};
  int k = 0;
  cstr_init(ss[k++], "one");
  int k_after_init = k;
  cstr_init_mp(ss[k++], "two", NULL);
  int k_after_init_mp = k;
  bool ok = ss[0] && ss[1] && ss[1] != sentinel && ss[2] == sentinel &&
            strcmp(cstring_c_str(ss[0]), "one") == 0 &&
            strcmp(cstring_c_str(ss[1]), "two") == 0;
  for (int i = 0; i < 5; i++) {
    if (ss[i] != sentinel) cstring_destroy(ss[i]);
  }
  cstring_destroy(sentinel);
  REQUIRE_EQ(k_after_init, 1);
  REQUIRE_EQ(k_after_init_mp, 2);
  REQUIRE_TRUE(ok);
}

// Sets LC_CTYPE to a Turkish Latin-5 locale, where toupper('i') is 0xDD and
// tolower('I') is 0xFD, for the case-folding test. It tries the installed
// names, and on glibc builds the locale with localedef(1) into dir and loads it
// through LOCPATH. Gives true when the process runs under such a locale.
static bool cstr_set_turkish_ctype(const char *dir) {
  static const char *const names[] = {"tr_TR.ISO-8859-9", "tr_TR.ISO8859-9",
                                      "tr_TR.iso88599"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (setlocale(LC_CTYPE, names[i]) && toupper('i') == 0xDD) return true;
  }
#if defined(__GLIBC__)
  char cmd[512];
  snprintf(cmd, sizeof(cmd),
           "localedef -i tr_TR -f ISO-8859-9 %s/tr_TR.ISO-8859-9 "
           ">/dev/null 2>&1",
           dir);
  if (system(cmd) == -1) return false;
  if (setenv("LOCPATH", dir, 1) != 0) return false;
  return setlocale(LC_CTYPE, "tr_TR.ISO-8859-9") && toupper('i') == 0xDD &&
         tolower('I') == 0xFD;
#else
  (void)dir;
  return false;
#endif
}

// With the ctype functions instead of the ASCII fold, "iI" would become "\xDD"
// "I" and "i\xFD", and the bytes above 0x7F would change.
TEST(cstring_case, folds_ascii_letters_only_under_a_turkish_locale) {
  char dir[64] = "cstr_tr_locale_XXXXXX";
  if (mkdtemp(dir) == NULL) {
    fprintf(stderr, "SKIP: cannot create the locale directory %s\n", dir);
    return;
  }
  bool turkish = cstr_set_turkish_ctype(dir);
  char upper_out[16] = {0};
  char lower_out[16] = {0};
  if (turkish) {
    cstr s = cstring_new("iI\xDD\xFD\xE0\xC0z");
    cstr t = cstring_new("iI\xDD\xFD\xE0\xC0Z");
    if (s && t) {
      cstring_to_upper(s);
      cstring_to_lower(t);
      snprintf(upper_out, sizeof(upper_out), "%s", cstring_c_str(s));
      snprintf(lower_out, sizeof(lower_out), "%s", cstring_c_str(t));
    }
    cstring_destroy(s);
    cstring_destroy(t);
  }
  setlocale(LC_CTYPE, "C");
#if defined(__GLIBC__)
  unsetenv("LOCPATH");
#endif
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  int rm_status = system(cmd);
  if (!turkish) {
    fprintf(stderr,
            "SKIP: no Turkish Latin-5 locale is installed and localedef "
            "cannot build one\n");
    return;
  }
  REQUIRE_EQ(rm_status, 0);
  REQUIRE_STREQ(upper_out, "II\xDD\xFD\xE0\xC0Z");
  REQUIRE_STREQ(lower_out, "ii\xDD\xFD\xE0\xC0z");
}

TEST(cstring_trim, trims_the_six_ascii_white_space_bytes_only) {
  cstr s = cstring_new(" \t\n\v\f\rx y\r\f\v\n\t ");
  cstr t = cstring_new("\xA0\x85 x \x85\xA0");
  cstr u = cstring_new("\x1c\x1dx\x1e\x1f");
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_NE((void *)t, NULL);
  REQUIRE_NE((void *)u, NULL);
  cstring_trim(s);
  cstring_trim(t);
  cstring_trim(u);
  char s_out[16], t_out[16], u_out[16];
  snprintf(s_out, sizeof(s_out), "%s", cstring_c_str(s));
  snprintf(t_out, sizeof(t_out), "%s", cstring_c_str(t));
  snprintf(u_out, sizeof(u_out), "%s", cstring_c_str(u));
  cstring_destroy(s);
  cstring_destroy(t);
  cstring_destroy(u);
  REQUIRE_STREQ(s_out, "x y");
  REQUIRE_STREQ(t_out, "\xA0\x85 x \x85\xA0");
  REQUIRE_STREQ(u_out, "\x1c\x1dx\x1e\x1f");
}

// Sets LC_CTYPE to a private Latin-1 locale whose white-space class also holds
// 0xA0 and 0x85, for the trim test. On glibc it writes a locale definition into
// dir, builds it with localedef(1) and loads it through LOCPATH. Gives true
// when the process runs under that locale.
static bool cstr_set_wide_space_ctype(const char *dir) {
#if !defined(__GLIBC__)
  (void)dir;
  return false;
#else
  char path[256];
  snprintf(path, sizeof(path), "%s/def", dir);
  FILE *f = fopen(path, "w");
  if (!f) return false;
  fputs(
      "comment_char %\n"
      "escape_char /\n"
      "LC_CTYPE\n"
      "upper <U0041>..<U005A>\n"
      "lower <U0061>..<U007A>\n"
      "digit <U0030>..<U0039>\n"
      "space <U0020>;<U000C>;<U000A>;<U000D>;<U0009>;<U000B>;<U00A0>;"
      "<U0085>\n"
      "blank <U0020>;<U0009>\n"
      "cntrl <U0000>..<U001F>;<U007F>\n"
      "toupper (<U0061>,<U0041>);(<U007A>,<U005A>)\n"
      "tolower (<U0041>,<U0061>);(<U005A>,<U007A>)\n"
      "END LC_CTYPE\n",
      f);
  if (fclose(f) != 0) return false;
  char cmd[640];
  snprintf(cmd, sizeof(cmd),
           "localedef -c -i %s/def -f ISO-8859-1 %s/xx_XX.ISO-8859-1 "
           ">/dev/null 2>&1",
           dir, dir);
  if (system(cmd) == -1) return false;
  if (setenv("LOCPATH", dir, 1) != 0) return false;
  return setlocale(LC_CTYPE, "xx_XX.ISO-8859-1") && isspace(0xA0) &&
         isspace(0x85);
#endif
}

// Under a locale that counts 0xA0 and 0x85 as space, both stay, and so does the
// last byte of the UTF-8 sequence C3 A0; isspace() would remove them.
TEST(cstring_trim,
     keeps_bytes_above_0x7f_under_a_locale_that_calls_them_space) {
  char dir[64] = "cstr_sp_locale_XXXXXX";
  if (mkdtemp(dir) == NULL) {
    fprintf(stderr, "SKIP: cannot create the locale directory %s\n", dir);
    return;
  }
  bool wide = cstr_set_wide_space_ctype(dir);
  char a_out[16] = {0};
  char b_out[16] = {0};
  if (wide) {
    cstr a = cstring_new("\xA0\x85 x \xA0");
    cstr b = cstring_new("x\xC3\xA0");
    if (a && b) {
      cstring_trim(a);
      cstring_trim(b);
      snprintf(a_out, sizeof(a_out), "%s", cstring_c_str(a));
      snprintf(b_out, sizeof(b_out), "%s", cstring_c_str(b));
    }
    cstring_destroy(a);
    cstring_destroy(b);
  }
  setlocale(LC_CTYPE, "C");
#if defined(__GLIBC__)
  unsetenv("LOCPATH");
#endif
  char cmd[128];
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  int rm_status = system(cmd);
  if (!wide) {
    fprintf(stderr,
            "SKIP: localedef cannot build a locale whose white space holds "
            "0xA0\n");
    return;
  }
  REQUIRE_EQ(rm_status, 0);
  REQUIRE_STREQ(a_out, "\xA0\x85 x \xA0");
  REQUIRE_STREQ(b_out, "x\xC3\xA0");
}
