#include <cstring.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

// Matches the static constant in cstring.c
#define CSTRING_MIN_CAPACITY 16

// ========================================================================
// CREATION AND DESTRUCTION
// ========================================================================

TEST(cstrings, create_fails_bad_mprocs) {
  char *err = NULL;

  // NULL malloc
  cstr s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs_t){
          .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);

  // NULL free
  s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);

  // NULL calloc
  s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);

  // NULL realloc
  s = cstring_create_full(
      NULL,
      &(ccol_memmgmt_procs_t){
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
      &(ccol_memmgmt_procs_t){
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

// ========================================================================
// QUERY FUNCTIONS
// ========================================================================

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
  REQUIRE_EQ(cstring_at(s, 2), '\0');    // == length
  REQUIRE_EQ(cstring_at(s, 100), '\0');  // way out of bounds
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

// ========================================================================
// APPEND
// ========================================================================

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
  size_t initial_cap = cstring_get_capacity(s);
  REQUIRE_EQ(initial_cap, CSTRING_MIN_CAPACITY);

  // Append enough to exceed the initial buffer
  const char *chunk = "0123456789abcdef";  // 16 chars
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);  // now 32 chars

  REQUIRE_EQ(cstring_length(s), 32);
  REQUIRE_TRUE(cstring_get_capacity(s) > initial_cap);
  // Capacity must still be a power of two >= 33
  size_t cap = cstring_get_capacity(s);
  REQUIRE_TRUE(cap >= 33);
  REQUIRE_EQ(cap & (cap - 1), 0);  // power of two check
  cstring_destroy(s);
}

// ========================================================================
// PREPEND
// ========================================================================

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
  // The original "456" must still be intact at the correct offset
  REQUIRE_EQ(cstring_at(s, 3), '4');
  REQUIRE_EQ(cstring_at(s, 4), '5');
  REQUIRE_EQ(cstring_at(s, 5), '6');
  cstring_destroy(s);
}

// ========================================================================
// INSERT
// ========================================================================

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
  REQUIRE_EQ(cstring_insert(s, 6, "x"), ccol_invalid_args);  // pos > length
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

TEST(cstrings, insert_null) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_insert(s, 0, NULL), ccol_invalid_args);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  cstring_destroy(s);
}

// ========================================================================
// SET
// ========================================================================

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
  // "hello" fits in minimum capacity (16), so set(s, c_str(s)) must not
  // realloc and must leave the content unchanged.
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), (size_t)5);
  cstring_destroy(s);
}

TEST(cstrings, set_self_alias_full_length_no_realloc) {
  // A 16-char string gets init_cap = next_pow2(17) = 32, so setting it to
  // itself requests capacity 17 which is already satisfied -- no realloc.
  // Exercises the no-realloc self-alias path with a longer string than
  // set_self_alias_no_realloc.
  cstr s = cstring_create("1234567890123456", NULL);
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  ccol_retval_t rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "1234567890123456");
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  cstring_destroy(s);
}

TEST(cstrings, set_suffix_alias_no_realloc) {
  // A suffix of s->data always has str_len < s->length < s->capacity, so
  // no realloc can occur.  Exercises the alias-offset tracking path where
  // the internal pointer must be re-derived after (a no-op) grow_to.
  cstr s = cstring_create("1234567890123456", NULL);
  // cstring_c_str(s) + 6 points to "7890123456"
  ccol_retval_t rv = cstring_set(s, cstring_c_str(s) + 6);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "7890123456");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstring_destroy(s);
}

// ========================================================================
// RESET
// ========================================================================

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
  // Grow the string past minimum capacity
  const char *chunk = "0123456789abcdef0123456789abcdef";  // 32 chars
  cstring_append(s, chunk);
  REQUIRE_TRUE(cstring_get_capacity(s) > CSTRING_MIN_CAPACITY);

  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), 0);
  REQUIRE_EQ(cstring_get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, reset_then_reuse) {
  cstr s = cstring_create("initial", NULL);
  cstring_reset(s);
  REQUIRE_EQ(cstring_append(s, "new content"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "new content");
  cstring_destroy(s);
}

// ========================================================================
// RESERVE
// ========================================================================

TEST(cstrings, reserve_basic) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring_get_capacity(s), CSTRING_MIN_CAPACITY);

  REQUIRE_TRUE(cstring_reserve(s, 100));
  REQUIRE_EQ(cstring_get_capacity(s), 128);  // next power of two >= 100
  REQUIRE_EQ(cstring_length(s), 0);          // content unchanged
  cstring_destroy(s);
}

TEST(cstrings, reserve_power_of_two_rounding) {
  cstr s = cstring_create(NULL, NULL);

  REQUIRE_TRUE(cstring_reserve(s, 7));
  REQUIRE_EQ(cstring_get_capacity(s), 16);  // clamped to minimum

  REQUIRE_TRUE(cstring_reserve(s, 17));
  REQUIRE_EQ(cstring_get_capacity(s), 32);

  REQUIRE_TRUE(cstring_reserve(s, 32));
  REQUIRE_EQ(cstring_get_capacity(s), 32);

  REQUIRE_TRUE(cstring_reserve(s, 33));
  REQUIRE_EQ(cstring_get_capacity(s), 64);

  cstring_destroy(s);
}

TEST(cstrings, reserve_does_not_shrink) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_reserve(s, 128));
  REQUIRE_EQ(cstring_get_capacity(s), 128);

  REQUIRE_TRUE(cstring_reserve(s, 16));
  REQUIRE_EQ(cstring_get_capacity(s), 128);  // unchanged
  cstring_destroy(s);
}

TEST(cstrings, reserve_below_minimum) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_reserve(s, 1));
  REQUIRE_EQ(cstring_get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, reserve_preserves_content) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_TRUE(cstring_reserve(s, 256));
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), 5);
  cstring_destroy(s);
}

// ========================================================================
// CASE CONVERSION
// ========================================================================

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

// ========================================================================
// TRIM
// ========================================================================

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

// ========================================================================
// COMPARE AND EQUALS
// ========================================================================

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

// ========================================================================
// STARTS_WITH / ENDS_WITH
// ========================================================================

TEST(cstrings, starts_with_true) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_TRUE(cstring_starts_with(s, "hello"));
  REQUIRE_TRUE(cstring_starts_with(s, "hello world"));  // full match
  cstring_destroy(s);
}

TEST(cstrings, starts_with_false) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_FALSE(cstring_starts_with(s, "world"));
  REQUIRE_FALSE(cstring_starts_with(s, "Hello"));  // case-sensitive
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

TEST(cstrings, ends_with_true) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_TRUE(cstring_ends_with(s, "world"));
  REQUIRE_TRUE(cstring_ends_with(s, "hello world"));  // full match
  cstring_destroy(s);
}

TEST(cstrings, ends_with_false) {
  cstr s = cstring_create("hello world", NULL);
  REQUIRE_FALSE(cstring_ends_with(s, "hello"));
  REQUIRE_FALSE(cstring_ends_with(s, "World"));  // case-sensitive
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

// ========================================================================
// FIND / RFIND
// ========================================================================

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
  // strstr(data, "") returns data, so find("") always yields offset 0.
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_find(s, ""), (size_t)0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_empty_needle) {
  // An empty needle has no meaningful last occurrence; the implementation
  // returns s->length (the past-the-end position), matching C++ semantics.
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_rfind(s, ""), cstring_length(s));
  cstring_destroy(s);
}

// ========================================================================
// REPLACE
// ========================================================================

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
  REQUIRE_STREQ(cstring_c_str(s), "hello world");  // unchanged
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
  // Replacing with empty string effectively deletes the needle
  cstr s = cstring_create("he_llo_wor_ld", NULL);
  REQUIRE_EQ(cstring_replace(s, "_", ""), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "helloworld");
  cstring_destroy(s);
}

TEST(cstrings, replace_invalid_args) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_replace(s, NULL, "x"), ccol_invalid_args);  // NULL needle
  REQUIRE_EQ(cstring_replace(s, "", "x"), ccol_invalid_args);    // empty needle
  REQUIRE_EQ(cstring_replace(s, "x", NULL),
             ccol_invalid_args);             // NULL replacement
  REQUIRE_STREQ(cstring_c_str(s), "hello");  // unchanged
  cstring_destroy(s);
}

TEST(cstrings, replace_entire_string) {
  cstr s = cstring_create("old", NULL);
  REQUIRE_EQ(cstring_replace(s, "old", "brand new content"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "brand new content");
  cstring_destroy(s);
}

// ========================================================================
// SUBSTRING
// ========================================================================

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
  // Request more than available from start=2 → only 3 chars ("llo")
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
  // start == length → returns empty string
  cstr sub = cstring_substring(s, 5, 1, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "");
  cstring_destroy(sub);
  // start beyond length → returns empty string
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
  // Requesting zero characters from a non-empty string must return an empty
  // cstring, not NULL.
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
  // Modifying original does not affect substring
  cstring_set(s, "aaaaa world");
  REQUIRE_STREQ(cstring_c_str(sub), "hello");
  cstring_destroy(sub);
  cstring_destroy(s);
}

// ========================================================================
// COPY
// ========================================================================

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
  REQUIRE_STREQ(cstring_c_str(c), "hello");  // copy unaffected
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

// ========================================================================
// SPLIT
// ========================================================================

static void free_cstr_vector(cvec parts) {
  for (size_t i = 0; i < cvector_elem_count(parts); i++) {
    cstr *p = (cstr *)cvector_at(parts, i);
    cstring_destroy(*p);
  }
  cvector_destroy(parts);
}

TEST(cstrings, split_basic) {
  cstr s = cstring_create("a,b,c", NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_elem_count(parts), 3);

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
  REQUIRE_EQ(cvector_elem_count(parts), 3);

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
  REQUIRE_EQ(cvector_elem_count(parts), 3);  // "a", "b", ""

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
  REQUIRE_EQ(cvector_elem_count(parts), 3);  // "", "a", "b"

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
  REQUIRE_EQ(cvector_elem_count(parts), 1);
  REQUIRE_STREQ(cstring_c_str(*(cstr *)cvector_at(parts, 0)), "hello");

  free_cstr_vector(parts);
  cstring_destroy(s);
}

TEST(cstrings, split_consecutive_delimiters) {
  cstr s = cstring_create("a,,b", NULL);
  cvec parts = cstring_split(s, ",", NULL);

  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ(cvector_elem_count(parts), 3);  // "a", "", "b"

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
  REQUIRE_EQ(cvector_elem_count(parts), 1);
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

// ========================================================================
// INTERNAL CAPACITY SCALING
// ========================================================================

TEST(cstrings, capacity_initial_is_minimum) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(cstring_get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, capacity_large_initial_rounds_up) {
  // A 30-char initial string needs at least 31 bytes → rounds up to 32
  cstr s = cstring_create("012345678901234567890123456789", NULL);
  REQUIRE_EQ(cstring_length(s), 30);
  size_t cap = cstring_get_capacity(s);
  REQUIRE_TRUE(cap >= 31);
  REQUIRE_EQ(cap & (cap - 1), 0);  // power of two
  cstring_destroy(s);
}

TEST(cstrings, capacity_always_power_of_two) {
  cstr s = cstring_create(NULL, NULL);
  const char *chunk = "0123456789";

  for (int i = 0; i < 10; i++) {
    cstring_append(s, chunk);
    size_t cap = cstring_get_capacity(s);
    REQUIRE_TRUE(cap > 0);
    REQUIRE_EQ(cap & (cap - 1), 0);  // power of two
  }
  cstring_destroy(s);
}

// ========================================================================
// LIFECYCLE MACROS
// ========================================================================

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
                    (&(ccol_memmgmt_procs_t){.malloc = malloc,
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
  // s is destroyed automatically when the scope above exits
}

// ========================================================================
// OPERATION MACROS
// ========================================================================

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
  REQUIRE_EQ(cstring_get_capacity(s), 256);
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

// ========================================================================
// COMPLEX / REALISTIC SCENARIOS
// ========================================================================

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
  REQUIRE_EQ(cvector_elem_count(parts), 3);

  cstr_construct(joined, NULL);
  for (size_t i = 0; i < cvector_elem_count(parts); i++) {
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

// ========================================================================
// SELF-ALIAS TESTS (use-after-realloc and overlapping-copy bug coverage)
// Strings of length >= 9 ensure the doubled length exceeds the 16-byte
// minimum capacity, triggering a realloc so the alias bugs are exercised.
// ========================================================================

TEST(cstrings, append_self_alias_triggers_realloc) {
  // "123456789" len=9; append self → "123456789123456789" len=18
  // 18+1 > 16 so cstring_grow_to must realloc; the old str pointer would
  // be dangling without the alias fix.
  cstr_construct(s, "123456789");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, append_suffix_alias_no_realloc) {
  // The 9-char source has capacity 16; appending the 5-char suffix "56789"
  // gives 14 chars which fits without realloc (grow_to(15) <= 16, no-op).
  // Exercises the alias-offset tracking path without reallocation.
  cstr_construct(s, "123456789");
  // cstring_c_str(s) + 4 points to "56789"
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12345678956789");
  cstr_destroy(s);
}

TEST(cstrings, append_self_alias_no_realloc) {
  // "hello" (5 chars, capacity 16): appending itself gives "hellohello"
  // (10 chars), grow_to(11) <= 16 is a no-op.  Exercises the alias_off==0
  // no-realloc path; memcpy is safe because dst starts at s->length while
  // src ends at s->length-1 -- no overlap.
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_triggers_realloc) {
  // "123456789" prepend self → "123456789123456789"
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, prepend_suffix_alias_no_realloc) {
  // The 9-char source has capacity 16; prepending the 5-char suffix "56789"
  // gives 14 chars which fits without realloc.  Exercises the alias_off > 0
  // path (second pointer correction after memmove) without reallocation.
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "56789123456789");
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_no_realloc) {
  // "hello" (5 chars, capacity 16): prepending itself gives "hellohello"
  // (10 chars), grow_to(11) <= 16 is a no-op.  Exercises the alias_off==0
  // no-realloc path; the final memmove(s->data, s->data, 5) is a same-pointer
  // copy that memmove handles safely (the content was already shifted right
  // by the preceding memmove, so positions 0..4 still hold the correct prefix).
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_triggers_realloc) {
  // "123456789" insert self at pos 0 → "123456789123456789"
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_insert(s, 0, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_after_pos_no_realloc) {
  // "123456789" insert "56789" (offset 4) at pos 2 -> "12567893456789"
  // The 9-char source has capacity 16; inserting 5 chars gives 14 chars
  // (grow_to(15) <= 16, no-op).  alias_off (4) > pos (2): after the first
  // memmove the source shifts right by str_len (5) and the pointer must be
  // re-derived; exercises that second correction without reallocation.
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_insert(s, 2, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12567893456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_overlapping_dst_gt_src) {
  // "abcde", insert s->data+1 ("bcde") at pos 3.
  // alias_off=1 <= pos=3, str_len=4: after memmove the copy source overlaps
  // destination with dst > src — previously a forward memcpy would corrupt.
  // Expected: "abc" + "bcde" + "de" = "abcbcdede"
  cstr_construct(s, "abcde");
  ccol_retval_t rv = cstring_insert(s, 3, cstring_c_str(s) + 1);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "abcbcdede");
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_at_nonzero_pos_no_realloc) {
  // "hello" (5 chars, capacity 16): inserting itself at pos 2 gives
  // "he" + "hello" + "llo" = "hehellollo" (10 chars), grow_to(11) <= 16.
  // Exercises alias_off==0 with pos>0 without reallocation; the final
  // memmove(s->data+2, s->data, 5) has dst > src with overlap -- memmove
  // handles it correctly.
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_insert(s, 2, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hehellollo");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

// ========================================================================
// ADDITIONAL EDGE CASES AND UNTESTED PATHS
// ========================================================================

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
  ccol_memmgmt_procs_t mp = {
      .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
  char *err = NULL;
  cstr s = cstring_create_full("hello", &mp, &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_NE((void *)cstring_get_mprocs(s), NULL);
  cstring_destroy(s);
}

TEST(cstrings, construct_mp_scoped_macro) {
  {
    cstr_construct_mp_scoped(
        s, "scoped",
        (&(ccol_memmgmt_procs_t){.malloc = malloc,
                                  .free = free,
                                  .calloc = calloc,
                                  .realloc = realloc}));
    REQUIRE_STREQ(cstring_c_str(s), "scoped");
  }
  // s is destroyed automatically when the scope above exits
}

TEST(cstrings, trim_single_whitespace_char) {
  cstr s = cstring_create(" ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_overlapping_needles) {
  // "ababa" contains "aba" at offsets 0 and 2 (overlapping).
  // rfind must return 2, not 0.  The +1 advance (not +nlen) in the
  // implementation is specifically there to handle this case.
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
  // Passing cstring_c_str(s) as the replacement is safe: the build loop
  // reads the replacement bytes before freeing the old buffer.
  cstr s = cstring_create("axb", NULL);
  REQUIRE_EQ(cstring_replace(s, "x", cstring_c_str(s)), ccol_success);
  // "axb" -> "a" + "axb" + "b" = "aaxbb"
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
