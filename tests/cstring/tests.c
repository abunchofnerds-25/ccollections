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
#include <unistd.h>

TAU_MAIN()

// Same value as the static constant in cstring.c
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
  size_t initial_cap = _cstring_get_capacity(s);
  REQUIRE_EQ(initial_cap, CSTRING_MIN_CAPACITY);

  // Append enough bytes to go past the first buffer
  const char *chunk = "0123456789abcdef";  // 16 chars
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);  // now 32 chars

  REQUIRE_EQ(cstring_length(s), 32);
  REQUIRE_TRUE(_cstring_get_capacity(s) > initial_cap);
  // The capacity should be a power of two >= 33
  size_t cap = _cstring_get_capacity(s);
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
  // The original "456" stays intact at the right offset
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
  // "hello" fits in the smallest capacity, which is 16, so
  // set(s, c_str(s)) must not realloc and the content must stay the same.
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), (size_t)5);
  cstring_destroy(s);
}

TEST(cstrings, set_self_alias_full_length_no_realloc) {
  // A 16-char string gets init_cap = next_pow2(17) = 32. Setting that
  // string to itself asks for a capacity of 17, which the string has
  // already, so there's no realloc. This drives the self-alias path without
  // a realloc, with a longer string than the one in
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
  // A suffix of s->data always has str_len < s->length < s->capacity, so no
  // realloc can happen here. This drives the path that tracks the alias
  // offset, where the code must recompute the internal pointer after
  // grow_to, which does nothing in this case.
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
  // Grow the string past the smallest capacity
  const char *chunk = "0123456789abcdef0123456789abcdef";  // 32 chars
  cstring_append(s, chunk);
  REQUIRE_TRUE(_cstring_get_capacity(s) > CSTRING_MIN_CAPACITY);

  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), 0);
  REQUIRE_EQ(_cstring_get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, reset_then_reuse) {
  cstr s = cstring_create("initial", NULL);
  cstring_reset(s);
  REQUIRE_EQ(cstring_append(s, "new content"), ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "new content");
  cstring_destroy(s);
}

// An allocator that counts the realloc calls. Unlike the budget-style
// out-of-memory allocator further below, this one always passes the call on
// to the real allocator, and it's only here so a test can assert on the
// number of realloc() calls.
static int g_cstr_realloc_call_count = 0;
static void *_cstr_counting_realloc(void *ptr, size_t size) {
  g_cstr_realloc_call_count++;
  return realloc(ptr, size);
}
static ccol_memmgmt_procs_t g_cstr_counting_procs = {
    .malloc = malloc,
    .calloc = calloc,
    .realloc = _cstr_counting_realloc,
    .free = free};

// cstring_reset() must not call realloc() when the string is already at the
// smallest capacity, since there's nothing to shrink then. That's the
// common case, because a new string and a string after a reset are both at
// that capacity. Checking the resulting capacity alone can't tell "the code
// skipped the call" from "the code made the call and it did nothing", and
// reset_clears_content and reset_shrinks_capacity above only make that
// check, so this test asserts on the real call count.
TEST(cstrings, reset_at_minimum_capacity_skips_realloc) {
  cstr s = cstring_create_full(NULL, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_realloc_call_count = 0;
  cstring_reset(s);
  REQUIRE_EQ(g_cstr_realloc_call_count, 0);
  REQUIRE_EQ(cstring_length(s), (size_t)0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  cstring_destroy(s);
}

// The converse of the test above: a string that grew past the smallest
// capacity shrinks back with exactly one realloc() call.
TEST(cstrings, reset_above_minimum_capacity_still_reallocs_once) {
  cstr s = cstring_create_full(NULL, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);

  const char *chunk = "0123456789abcdef0123456789abcdef";  // 32 chars
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  REQUIRE_TRUE(_cstring_get_capacity(s) > (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_realloc_call_count = 0;
  cstring_reset(s);
  REQUIRE_EQ(g_cstr_realloc_call_count, 1);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  cstring_destroy(s);
}

// ========================================================================
// RESERVE
// ========================================================================

TEST(cstrings, reserve_basic) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_EQ(_cstring_get_capacity(s), CSTRING_MIN_CAPACITY);

  REQUIRE_TRUE(cstring_reserve(s, 100));
  REQUIRE_EQ(_cstring_get_capacity(s), 128);  // next power of two >= 100
  REQUIRE_EQ(cstring_length(s), 0);           // content unchanged
  cstring_destroy(s);
}

TEST(cstrings, reserve_power_of_two_rounding) {
  cstr s = cstring_create(NULL, NULL);

  REQUIRE_TRUE(cstring_reserve(s, 7));
  REQUIRE_EQ(_cstring_get_capacity(s), 16);  // clamped to minimum

  REQUIRE_TRUE(cstring_reserve(s, 17));
  REQUIRE_EQ(_cstring_get_capacity(s), 32);

  REQUIRE_TRUE(cstring_reserve(s, 32));
  REQUIRE_EQ(_cstring_get_capacity(s), 32);

  REQUIRE_TRUE(cstring_reserve(s, 33));
  REQUIRE_EQ(_cstring_get_capacity(s), 64);

  cstring_destroy(s);
}

TEST(cstrings, reserve_does_not_shrink) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_reserve(s, 128));
  REQUIRE_EQ(_cstring_get_capacity(s), 128);

  REQUIRE_TRUE(cstring_reserve(s, 16));
  REQUIRE_EQ(_cstring_get_capacity(s), 128);  // unchanged
  cstring_destroy(s);
}

TEST(cstrings, reserve_below_minimum) {
  cstr s = cstring_create(NULL, NULL);
  REQUIRE_TRUE(cstring_reserve(s, 1));
  REQUIRE_EQ(_cstring_get_capacity(s), CSTRING_MIN_CAPACITY);
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

TEST(cstrings, starts_with_null_prefix) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_FALSE(cstring_starts_with(s, NULL));
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

TEST(cstrings, ends_with_null_suffix) {
  cstr s = cstring_create("hello", NULL);
  REQUIRE_FALSE(cstring_ends_with(s, NULL));
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
  // strstr(data, "") returns data, so find("") always gives offset 0.
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_find(s, ""), (size_t)0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_empty_needle) {
  // An empty needle has no meaningful last occurrence, so the library
  // returns s->length, the past-the-end position, which is what C++
  // does too.
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
  // Replacing with an empty string deletes the needle
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

// ------------------------------------------------------------------------
// The tests below drive the ccol_container_full overflow guards of
// cstring_replace() through _cstring_replace_compute_new_length_for_tests()
// and not through cstring_replace() itself, because hitting either overflow
// through the real API needs real strings of several gigabytes: a source
// string of several gigabytes that holds one character many times, plus a
// replacement string of several gigabytes, which isn't practical in a normal
// test run. These tests feed the same arithmetic cstring_replace() runs,
// compute_replace_new_length() in cstring.c, with made-up lengths and counts
// that have no real memory behind
// them.
// ------------------------------------------------------------------------

TEST(cstrings, replace_new_length_normal_growth_case) {
  size_t new_len = 0;
  // A 10-byte string with 3 occurrences of a 2-byte needle, each replaced
  // by 5 bytes: 10 + (5-2)*3 = 19.
  ccol_retval_t rv =
      _cstring_replace_compute_new_length_for_tests(10, 2, 5, 3, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)19);
}

TEST(cstrings, replace_new_length_normal_shrink_case) {
  size_t new_len = 0;
  // A 20-byte string with 2 occurrences of a 5-byte needle, each replaced
  // by 2 bytes: 20 - (5-2)*2 = 14.
  ccol_retval_t rv =
      _cstring_replace_compute_new_length_for_tests(20, 5, 2, 2, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)14);
}

TEST(cstrings, replace_new_length_multiplication_overflow_detected) {
  // (rlen - nlen) * count has to overflow a size_t: (SIZE_MAX/2 + 1) * 2 wraps.
  size_t new_len = 12345;  // sentinel, the call must not touch it on failure
  size_t nlen = 1;
  size_t rlen = (SIZE_MAX / 2) + 2;  // rlen - nlen == SIZE_MAX/2 + 1
  ccol_retval_t rv =
      _cstring_replace_compute_new_length_for_tests(0, nlen, rlen, 2, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

TEST(cstrings, replace_new_length_addition_overflow_detected) {
  // The added value is 1 so it doesn't overflow, but the sum
  // orig_length + added does.
  size_t new_len = 12345;  // sentinel, the call must not touch it on failure
  ccol_retval_t rv = _cstring_replace_compute_new_length_for_tests(
      SIZE_MAX, 1, 2, 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

// Unlike replace_new_length_addition_overflow_detected above, here
// orig_length + added lands exactly on SIZE_MAX and never goes below
// orig_length, so the wraparound check "new_len < orig_length" alone
// does NOT catch it. That's the case cstring_length_fits_with_terminator()
// is there to reject, because the caller of cstring_replace() adds 1 to
// new_len for the null terminator before using it as a capacity.
TEST(cstrings, replace_new_length_result_exactly_size_max_detected) {
  size_t new_len = 12345;  // sentinel, the call must not touch it on failure
  ccol_retval_t rv = _cstring_replace_compute_new_length_for_tests(
      SIZE_MAX - 1, 1, 2, 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

// The guard below protects against a size_t wraparound. Every function that
// changes a string (append, prepend, insert, set and create_full) goes
// through it before asking for a buffer of length + 1 bytes, and so does the
// length arithmetic of cstring_replace(). We drive the guard directly,
// because a test through the public API would need a string that fills
// the whole address space.
TEST(cstrings, length_fits_with_terminator_guard) {
  REQUIRE_TRUE(_cstring_length_fits_with_terminator_for_tests(0));
  REQUIRE_TRUE(_cstring_length_fits_with_terminator_for_tests(1));
  REQUIRE_TRUE(_cstring_length_fits_with_terminator_for_tests(SIZE_MAX - 1));
  REQUIRE_FALSE(_cstring_length_fits_with_terminator_for_tests(SIZE_MAX));
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
  // Ask for more characters than start=2 can give -> only 3 chars ("llo")
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
  // start == length -> returns an empty string
  cstr sub = cstring_substring(s, 5, 1, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "");
  cstring_destroy(sub);
  // start past length -> returns an empty string
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
  // Asking for zero characters from a non-empty string gives an
  // empty cstring and not NULL.
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
  // Changing the original doesn't affect the substring
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
  REQUIRE_EQ(_cstring_get_capacity(s), CSTRING_MIN_CAPACITY);
  cstring_destroy(s);
}

TEST(cstrings, capacity_large_initial_rounds_up) {
  // A first string of 30 chars needs at least 31 bytes -> rounds up to 32
  cstr s = cstring_create("012345678901234567890123456789", NULL);
  REQUIRE_EQ(cstring_length(s), 30);
  size_t cap = _cstring_get_capacity(s);
  REQUIRE_TRUE(cap >= 31);
  REQUIRE_EQ(cap & (cap - 1), 0);  // power of two
  cstring_destroy(s);
}

TEST(cstrings, capacity_always_power_of_two) {
  cstr s = cstring_create(NULL, NULL);
  const char *chunk = "0123456789";

  for (int i = 0; i < 10; i++) {
    cstring_append(s, chunk);
    size_t cap = _cstring_get_capacity(s);
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
  // s gets destroyed automatically when the scope above ends
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
  REQUIRE_EQ(_cstring_get_capacity(s), 256);
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
// SELF-ALIAS TESTS (use-after-realloc and overlapping-copy hazards)
// A string of 9 characters or more makes the doubled length go past the
// smallest capacity of 16 bytes and forces a realloc, so these tests drive
// those hazards.
// ========================================================================

TEST(cstrings, append_self_alias_triggers_realloc) {
  // "123456789" has len=9. Appending it to itself gives "123456789123456789"
  // with len=18. Since 18+1 > 16, cstring_grow_to reallocs and the
  // code then needs to recompute str against the new buffer, or str
  // dangles.
  cstr_construct(s, "123456789");
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, append_suffix_alias_no_realloc) {
  // The 9-char source has a capacity of 16. Appending the 5-char suffix
  // "56789" gives 14 chars, which fits without a realloc since
  // grow_to(15) <= 16 does nothing. This drives the path that tracks the
  // alias offset, without a reallocation.
  cstr_construct(s, "123456789");
  // cstring_c_str(s) + 4 points to "56789"
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12345678956789");
  cstr_destroy(s);
}

TEST(cstrings, append_self_alias_no_realloc) {
  // "hello" has 5 chars and a capacity of 16. Appending it to itself gives
  // "hellohello" with 10 chars, and grow_to(11) <= 16 does nothing. This
  // drives the path with alias_off==0 and no realloc. The memcpy is safe here
  // because dst starts at s->length and src ends at s->length-1, so the two
  // regions don't overlap.
  cstr_construct(s, "hello");
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_triggers_realloc) {
  // Prepending "123456789" to itself gives "123456789123456789"
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, prepend_suffix_alias_no_realloc) {
  // The 9-char source has a capacity of 16. Prepending the 5-char suffix
  // "56789" gives 14 chars, which fits without a realloc. This drives the
  // path with alias_off > 0 and no reallocation, which corrects the pointer
  // a second time after the memmove.
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "56789123456789");
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_no_realloc) {
  // "hello" has 5 chars and a capacity of 16. Prepending it to itself gives
  // "hellohello" with 10 chars, and grow_to(11) <= 16 does nothing. This
  // drives the path with alias_off==0 and no realloc. The final
  // memmove(s->data, s->data, 5) uses one pointer for both sides, which
  // memmove handles safely. The memmove before it has already moved the
  // content to the right, so positions 0..4 hold the right prefix at that
  // point.
  cstr_construct(s, "hello");
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_triggers_realloc) {
  // Inserting "123456789" into itself at pos 0 gives
  // "123456789123456789"
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_insert(s, 0, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_after_pos_no_realloc) {
  // Inserting "56789", which sits at offset 4, into "123456789" at pos 2
  // gives "12567893456789". The 9-char source has a capacity of 16, inserting
  // 5 chars gives 14, and grow_to(15) <= 16 does nothing. Here alias_off is
  // 4 and pos is 2, so alias_off > pos, and the first memmove moves the
  // source right by str_len (5), so the code must recompute the pointer.
  // This drives that second correction without a
  // reallocation.
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_insert(s, 2, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12567893456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_overlapping_dst_gt_src) {
  // The string is "abcde" and we insert s->data+1, which is "bcde", at pos 3.
  // With alias_off=1 <= pos=3 and str_len=4, after the memmove the copy's
  // source overlaps its destination with dst > src. A memcpy copies forward
  // and would corrupt the result, so the code needs a memmove, which handles
  // the overlap. The expected result is
  // "abc" + "bcde" + "de" = "abcbcdede".
  cstr_construct(s, "abcde");
  ccol_retval_t rv = cstring_insert(s, 3, cstring_c_str(s) + 1);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "abcbcdede");
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_at_nonzero_pos_no_realloc) {
  // "hello" has 5 chars and a capacity of 16. Inserting it into itself at
  // pos 2 gives "he" + "hello" + "llo" = "hehellollo", 10 chars, and
  // grow_to(11) <= 16. This drives alias_off==0 with pos>0 and no
  // reallocation. The final memmove(s->data+2, s->data, 5) has dst > src with
  // an overlap, which memmove handles correctly.
  cstr_construct(s, "hello");
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_insert(s, 2, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hehellollo");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

// Every other test above that mixes an alias with a realloc uses
// alias_off == 0, where the alias covers the whole string, and every other
// test with a non-zero alias offset stays inside the current capacity, so
// grow_to() doesn't reallocate there. This one combines the two: a non-zero
// alias offset AND a real move of the buffer. That's exactly the case the
// second computation of str after grow_to() is there for, since grow_to()
// can move the buffer.
TEST(cstrings, prepend_alias_offset_nonzero_triggers_realloc) {
  // A 20-char base gives init_cap = next_pow2(21) = 32. Prepending its own
  // 15-char suffix at offset 5 grows the string to 35 bytes, which forces a
  // real realloc to a new capacity of 64. alias_off is 5 here, so not
  // zero.
  const char *base = "12345678901234567890";
  char expected[64];
  snprintf(expected, sizeof(expected), "%s%s", base + 5, base);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), (size_t)35);

  cstring_destroy(s);
}

// Same combination as prepend_alias_offset_nonzero_triggers_realloc above,
// but with cstring_insert() and a non-zero pos. Here alias_off is 5 and pos
// is 3, so alias_off > pos, and together with a real reallocation the test
// also drives the branch that corrects the pointer a second time after the
// first memmove moves the source to the
// right.
TEST(cstrings, insert_alias_offset_nonzero_triggers_realloc) {
  const char *base = "12345678901234567890";  // 20 chars, capacity 32
  const char *inserted = base + 5;            // "678901234567890", 15 chars
  size_t pos = 3;

  char expected[64];
  size_t blen = strlen(base), ilen = strlen(inserted);
  memcpy(expected, base, pos);
  memcpy(expected + pos, inserted, ilen);
  memcpy(expected + pos + ilen, base + pos, blen - pos + 1);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_insert(s, pos, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// The converse of insert_alias_offset_nonzero_triggers_realloc above: here
// alias_off is 2 and pos is 10, so alias_off < pos, with a real
// reallocation. cstring_insert() only corrects the pointer a second time
// when `alias_off > pos`, because when alias_off <= pos the first memmove
// provably never touches the region [alias_off, alias_off+str_len), and the
// comment on that memmove in cstring.c gives the proof. This test pins that
// the path without the correction is right when grow_to() moves the buffer
// too, not only when the buffer is big enough
// already.
TEST(cstrings, insert_alias_offset_less_than_pos_triggers_realloc) {
  const char *base = "12345678901234567890";  // 20 chars, capacity 32
  const char *inserted = base + 2;            // "345678901234567890", 18 chars
  size_t pos = 10;

  char expected[64];
  size_t blen = strlen(base), ilen = strlen(inserted);
  memcpy(expected, base, pos);
  memcpy(expected + pos, inserted, ilen);
  memcpy(expected + pos + ilen, base + pos, blen - pos + 1);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_insert(s, pos, cstring_c_str(s) + 2);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// The exact boundary between the two combinations above: alias_off == pos,
// with a real reallocation. The second correction in cstring_insert() uses
// `> pos` and not `>= pos`, so it leaves this case out on purpose. The
// region [alias_off, alias_off+str_len) == [pos, pos+str_len) sits exactly
// on the boundary of the first memmove's shift, so that shift never touches
// the region whether grow_to() moved the buffer or not.
// insert_self_alias_at_nonzero_pos_no_realloc in the INSERT section above
// covers this alias_off == pos boundary for an alias_off of 0, and this one
// covers it for a non-zero alias_off together with a real reallocation,
// which that test doesn't drive.
TEST(cstrings, insert_alias_offset_equal_to_pos_triggers_realloc) {
  const char *base = "12345678901234567890";  // 20 chars, capacity 32
  const char *inserted = base + 5;            // "678901234567890", 15 chars
  size_t pos = 5;

  char expected[64];
  size_t blen = strlen(base), ilen = strlen(inserted);
  memcpy(expected, base, pos);
  memcpy(expected + pos, inserted, ilen);
  memcpy(expected + pos + ilen, base + pos, blen - pos + 1);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_insert(s, pos, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// set_self_alias_full_length_no_realloc in the SET section above checks that
// the resulting content is right. This test checks the "no realloc" half of
// that test's comment with an assertion on the real call count, the way
// reset_at_minimum_capacity_skips_realloc does, with the same counting
// allocator for the same kind of
// claim.
TEST(cstrings, set_self_alias_no_realloc_verified_by_call_count) {
  cstr s =
      cstring_create_full("1234567890123456", &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(g_cstr_realloc_call_count, 0);
  REQUIRE_STREQ(cstring_c_str(s), "1234567890123456");
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)32);

  cstring_destroy(s);
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
    cstr_construct_mp_scoped(s, "scoped",
                             (&(ccol_memmgmt_procs_t){.malloc = malloc,
                                                      .free = free,
                                                      .calloc = calloc,
                                                      .realloc = realloc}));
    REQUIRE_STREQ(cstring_c_str(s), "scoped");
  }
  // s gets destroyed automatically when the scope above ends
}

TEST(cstrings, trim_single_whitespace_char) {
  cstr s = cstring_create(" ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_overlapping_needles) {
  // "ababa" holds "aba" at offsets 0 and 2, and the two overlap, so rfind
  // returns 2 and not 0.
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
  // cstring_c_str(s) is a safe replacement argument here, because the loop
  // that builds the result reads the replacement bytes before the old
  // buffer is freed.
  cstr s = cstring_create("axb", NULL);
  REQUIRE_EQ(cstring_replace(s, "x", cstring_c_str(s)), ccol_success);
  // "axb" becomes "a" + "axb" + "b" = "aaxbb"
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

// ========================================================================
// MACRO HYGIENE REGRESSION (cstr_insert's internal retval local)
// ========================================================================

// cstr_insert must not call its internal retval plain '_r'. A caller can
// call its own pos argument '_r', and because of C's declarator-scope rule
// that argument then binds to the macro's not-yet-initialized local instead
// of the caller's real value, with nothing reporting it. A ccol_retval_t
// also converts to a size_t implicitly, and no compiler should warn about
// that. Give the internal local the plain name '_r' and this test catches
// it, failing either with a wrong insert position or with a false
// ccol_invalid_args, depending on the garbage value in the uninitialized
// enum.
TEST(cstrings, insert_macro_pos_argument_named__r_is_not_shadowed) {
  cstr_construct(s, "hello");
  size_t _r = 2;
  cstr_insert(s, _r, "X");
  REQUIRE_STREQ(cstring_c_str(s), "heXllo");
  cstr_destroy(s);
}

// ========================================================================
// FATAL / NULL-ARGUMENT TESTS
//
// This module documents that a NULL cstr makes a function assert, so the
// function aborts with _ccol_assert. That holds for every function of the
// module that queries, changes or searches a string, and for the substring
// function. Each test below runs in its own forked child so the assert's
// abort doesn't take the whole test binary down, and the parent then checks
// that the child died with SIGABRT and not with a plain exit or some other
// crash; without a NULL guard the child dies of a SIGSEGV from a NULL
// dereference instead. This fork+SIGABRT pattern shows up all over this
// codebase, type_safe_at_out_of_bounds_is_fatal in tests/cvector is
// another user of it.
// ========================================================================

/* Returns false without touching *out_status when fork() or waitpid()
 * fails, so the caller can tell that apart from a real test failure, where
 * the child is WIFSIGNALED but the signal isn't SIGABRT. We can't use
 * REQUIRE_* in here, because those macros end in a bare `return;`, which
 * only fits a void TEST body, and this helper returns an
 * int. */
static bool run_forked(void (*fn)(void), int *out_status) {
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    fn();
    _exit(0); /* not reachable when the assert aborts, as it should */
  }
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return false;
  *out_status = status;
  return true;
}

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
// cstring_get_mprocs needs a NULL guard of its own. Without it the function
// segfaults (WIFSIGNALED + SIGSEGV) instead of asserting (WIFSIGNALED +
// SIGABRT) like the rest
// of this module.
DEFINE_NULL_ARG_FATAL_TEST(null_get_mprocs_is_fatal,
                           (void)cstring_get_mprocs(NULL))

// ========================================================================
// OUT OF MEMORY / ALLOCATOR FAILURE TESTS
//
// A counting allocator with a budget. The first g_cstr_oom_budget calls to
// malloc, calloc and realloc succeed and go to the real allocator, and
// every call after the budget runs out returns NULL. free() always goes to
// the real free() and the budget never covers it, so every cleanup path
// under test frees correctly whatever a call did allocate.
//
// The budgets below are exact allocation counts and not estimates.
// cstring_create_full() always costs exactly 2 calls: 1 calloc for the
// block that holds the container and the copy of the mprocs, then 1 malloc
// for the data buffer, which is allocated once at its final size. A
// grow_to() that really reallocates costs exactly 1 more realloc call.
// cvector_create_full(sizeof(cstr)), which cstring_split uses internally,
// costs 2 calls too.
// ========================================================================

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

static ccol_memmgmt_procs_t g_cstr_oom_procs = {.malloc = _cstr_oom_malloc,
                                                .calloc = _cstr_oom_calloc,
                                                .realloc = _cstr_oom_realloc,
                                                .free = _cstr_oom_free};

TEST(cstrings, create_full_oom_container_alloc_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 0;  // the first call fails, the calloc for the struct
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_full_oom_data_buffer_alloc_fails) {
  char *err = NULL;
  // The budget covers the calloc for the container's block, which also
  // holds the copy of the mprocs, so the malloc for the data buffer (the
  // 2nd call) is the one that fails.
  g_cstr_oom_budget = 1;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

// A cstring with a custom allocator costs 2 allocator calls, because the
// copy of the mprocs lives in the container's block and needs no call of its
// own. With a separate allocation for the copy, the create would fail
// under this budget.
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

// The cstring keeps its own copy of a procs struct that isn't interned, so
// the caller's struct may change or go away after the create, and every
// later allocation and the destroy go through the copy. Strings derived
// from it (a substring, a copy, the tokens of a split) each keep their own
// copy as well, so each one outlives the string it came from.
TEST(cstrings, the_procs_copy_outlives_the_callers_struct) {
  ccol_memmgmt_procs_t *mp = malloc(sizeof(*mp));
  REQUIRE_NE((void *)mp, NULL);
  *mp = g_cstr_oom_procs;
  g_cstr_oom_budget = 1000000;
  cstr s = cstring_create_full("hello", mp, NULL);
  cstr sub = s ? cstring_substring(s, 0, 4, NULL) : NULL;
  ccol_memmgmt_procs_t *kept = s ? cstring_get_mprocs(s) : NULL;
  bool copied = kept != NULL && kept != mp &&
                memcmp(kept, &g_cstr_oom_procs, sizeof(*kept)) == 0;
  free(mp);  // the caller's struct is gone from here on
  int before = g_cstr_oom_budget;
  bool grown = s != NULL &&
               cstring_append(s, "0123456789abcdef0123456789") == ccol_success;
  int used = before - g_cstr_oom_budget;
  cstring_destroy(s);  // sub keeps its own copy and stays usable
  bool sub_ok = sub != NULL && cstring_append(sub, "!") == ccol_success &&
                strcmp(cstring_c_str(sub), "hell!") == 0;
  cstring_destroy(sub);
  REQUIRE_TRUE(copied);
  REQUIRE_TRUE(grown);
  REQUIRE_EQ(used, 1);  // the grow went through the copy of the procs
  REQUIRE_TRUE(sub_ok);
}

// An interned procs pointer lives as long as the process, so the cstring
// keeps it as it is and copies nothing.
TEST(cstrings, an_interned_procs_pointer_is_kept_as_it_is) {
  ccol_memmgmt_procs_t *interned = NULL;
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

  g_cstr_oom_budget = 0;  // the grow_to() realloc must fail
  ccol_retval_t rv = cstring_append(s, "0123456789abcdef0123456789");
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_STREQ(cstring_c_str(s), "hello");  // unchanged
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
  ccol_retval_t rv = cstring_prepend(s, "0123456789abcdef0123456789");
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
  ccol_retval_t rv = cstring_insert(s, 2, "0123456789abcdef0123456789");
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
  ccol_retval_t rv = cstring_set(s, "0123456789abcdef0123456789");
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
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 0;
  REQUIRE_FALSE(cstring_reserve(s, 1024));
  REQUIRE_EQ(_cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, replace_oom_new_buffer_alloc_fails_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello world hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;  // the single new-buffer malloc must fail
  ccol_retval_t rv = cstring_replace(s, "hello", "X");
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_STREQ(cstring_c_str(s), "hello world hello");  // unchanged

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, substring_oom_result_creation_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello world", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 0;  // the result's own container calloc must fail
  char *sub_err = NULL;
  cstr sub = cstring_substring(s, 0, 5, &sub_err);
  REQUIRE_EQ((void *)sub, NULL);
  REQUIRE_NE((void *)sub_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

// A substring longer than the smallest capacity costs the same 2 allocator
// calls as a short one, since the buffer is allocated once at its final size
// and never grows. The 20 characters below need a capacity of 32.
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
  size_t cap = ok ? _cstring_get_capacity(sub) : 0;

  g_cstr_oom_budget = 1;  // one call short: the data buffer must fail
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

static void free_cstr_vector_oom(cvec parts) {
  if (!parts) return;
  g_cstr_oom_budget = 1000000;  // the budget must never limit the teardown
  for (size_t i = 0; i < cvector_elem_count(parts); i++) {
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

  g_cstr_oom_budget = 0;  // the first call of cvector_create_full() must fail
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

  // The budget covers the vector (2 calls) and the first token "a" (2 more
  // calls), so the container calloc of the second token "b", the 5th call,
  // fails. The split must return NULL and destroy_cstr_vector() must
  // have destroyed the first token already, because it must not leak, and
  // `make memtest` catches a regression there.
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

  // The budget covers the vector (2 calls) and the two loop tokens "a" and
  // "b" (2 calls each), 6 calls in total, so the container calloc of the
  // final tail token "c", the 7th call, fails.
  g_cstr_oom_budget = 6;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

// A token longer than the smallest capacity costs the same 2 allocator calls
// as a short one, because its buffer is allocated once at its final size.
// The vector takes 2 calls and each of the two tokens 2 more, so 6 calls are
// exactly enough and 5 are one short.
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
  size_t count = parts ? cvector_elem_count(parts) : 0;
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

  g_cstr_oom_budget = 8;  // the vector (2 calls) and 3 tokens (2 calls each)
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ((void *)split_err, NULL);
  REQUIRE_EQ(cvector_elem_count(parts), (size_t)3);

  g_cstr_oom_budget = 1000000;
  free_cstr_vector_oom(parts);
  cstring_destroy(s);
}

// cstring_reset() documents what happens when the internal realloc that
// shrinks the buffer fails: the buffer keeps its capacity and the function
// sets the length to 0 anyway, without losing the content or
// crashing.
TEST(cstrings, reset_realloc_failure_leaves_capacity_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 1000000;
  cstr s = cstring_create_full(NULL, &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  const char *chunk =
      "0123456789abcdef0123456789abcdef";  // 33 chars, which forces a grow
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  size_t grown_cap = _cstring_get_capacity(s);
  REQUIRE_TRUE(grown_cap > CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 0;  // the realloc back to the smallest size must fail
  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), (size_t)0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(_cstring_get_capacity(s), grown_cap);  // no change, no shrink

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

// ========================================================================
// RFIND AGAINST A REFERENCE SEARCH
// ========================================================================

/* The reference answer: every start position from the right, compared in
 * full. It shares no code with cstring_rfind(). */
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

/* A small xorshift generator, so every run checks the same inputs. */
static uint64_t g_rfind_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rfind_rng_next(void) {
  g_rfind_rng ^= g_rfind_rng << 13;
  g_rfind_rng ^= g_rfind_rng >> 7;
  g_rfind_rng ^= g_rfind_rng << 17;
  return g_rfind_rng;
}

static void rfind_fill(char *buf, size_t len, unsigned alphabet) {
  for (size_t i = 0; i < len; i++) {
    buf[i] = (char)('a' + rfind_rng_next() % alphabet);
  }
  buf[len] = '\0';
}

/* Random texts over alphabets of one to four letters, where matches and near
 * misses are dense, with needles of every length up to the text and past it.
 * Needles cut from the text itself make sure there are many hits. */
TEST(rfind, agrees_with_the_reference_on_random_inputs) {
  char hay[1100];
  char needle[1120];
  size_t mismatches = 0;
  size_t checked = 0;
  for (int round = 0; round < 20000; round++) {
    unsigned alphabet = 1 + (unsigned)(rfind_rng_next() % 4);
    /* Half the rounds use a string below the length where cstring_rfind
     * switches from its forward search to its backward scan and half use one
     * above it, so both paths get compared with the reference. */
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

/* Long texts of 'a' with sparse 'b' and 'c' bytes, and needles that agree
 * with the text over long runs. These inputs get cstring_rfind() into its
 * linear fallback with the table both on the stack and on the heap, at
 * every kind of position, and every answer must equal the reference. */
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

/* Builds a text of hlen bytes of 'a' with a "b" at every position in
 * b_positions, and a needle of nlen bytes of 'a' with a "b" at offset b_at.
 * So every candidate of the backward scan agrees with the needle on its
 * first and last byte and only differs from it in the middle. */
static char *rfind_adversarial(size_t len, size_t b_at) {
  /* No allocation reaches PTRDIFF_MAX bytes. Telling GCC 12 so keeps it,
   * under _FORTIFY_SOURCE=3, from checking the memset below against a
   * wrapped buffer size of len + 1. */
  if (len >= PTRDIFF_MAX) return NULL;
  char *buf = malloc(len + 1);
  if (!buf) return NULL;
  memset(buf, 'a', len);
  if (b_at < len) buf[b_at] = 'b';
  buf[len] = '\0';
  return buf;
}

/* A needle that agrees with the text over long runs makes the backward scan
 * hand the search over to its linear fallback. That fallback keeps a short
 * needle's table on the stack and takes a long needle's table from the
 * string's allocator, and when that allocator refuses, the scan finishes the
 * search without it. Every answer still has to equal the reference, and the
 * counter shows that the inputs reach the fallback. */
TEST(rfind, the_linear_fallback_agrees_with_the_reference) {
  struct {
    size_t hlen, text_b, nlen, needle_b;
  } cases[] = {
      {4096, 4096, 16, 8},      /* no match, short needle: stack table */
      {4096, 100, 16, 8},       /* one match far to the left */
      {4096, 4000, 16, 8},      /* one match near the right end */
      {20000, 20000, 600, 300}, /* no match, long needle: heap table */
      {20000, 1500, 600, 300},  /* one match, long needle */
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

    /* The same search, on a string whose allocator refuses everything. */
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
  /* Each case makes two searches that reach the fallback. */
  REQUIRE_EQ(handovers, 10UL);
}

/* Periodic text with a needle that matches everywhere. The backward scan
 * hits the answer at its very first candidate and never needs the
 * fallback. */
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

// ========================================================================
// SINGLE EVALUATION OF MACRO ARGUMENTS
// ========================================================================

/* Runs fn in a forked child with stderr on a pipe and returns what the child
 * wrote. We expect the child to stop in _ccol_fatal_err(). */
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
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    fn();
    _exit(0);
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

static void substring_failure_message_thunk(void) {
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello world", &g_cstr_oom_procs, NULL);
  if (!s) _exit(3);
  g_cstr_oom_budget = 0;
  size_t start = 2, len = 3;
  cstr sub = cstr_substring(s, start++, len++);
  (void)sub;
}

/* cstr_substring's failure message reports the start and the length the call
 * used, because each argument is evaluated once. */
TEST(macro_arguments, substring_failure_reports_the_arguments_it_used) {
  char msg[512];
  int status = 0;
  REQUIRE_TRUE(run_forked_capture_stderr(substring_failure_message_thunk, msg,
                                         sizeof(msg), &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
  REQUIRE_NE((void *)strstr(msg, "start=2, len=3"), NULL);
}

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

/* cstring_destroy and cstr_destroy evaluate their lvalue once, so the element
 * they free is the element they set to NULL. */
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
    /* A macro that evaluates its argument more than once frees a neighbour
     * instead of ss[0]. We free the survivors so the failure below is a
     * clean one. */
    __cstring_destroy(first);
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

/* cstr_init and cstr_init_mp assign their lvalue once, after the string is
 * created, and read no other element. The slots after the two targets hold
 * a sentinel string, so a macro that reads its target again finds a string
 * there and fails this test instead of stopping the process. */
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

/* Sets LC_CTYPE to a Turkish Latin-5 locale, where the ctype functions fold
 * 'i' to 0xDD and 'I' to 0xFD, and returns true when the process is running
 * under one afterwards. It tries the locale names that systems install, and
 * if none works it builds the locale with localedef(1) into dir and loads it
 * from there through LOCPATH. */
static bool cstr_set_turkish_ctype(const char *dir) {
  static const char *const names[] = {"tr_TR.ISO-8859-9", "tr_TR.ISO8859-9",
                                      "tr_TR.iso88599"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (setlocale(LC_CTYPE, names[i]) && toupper('i') == 0xDD) return true;
  }
  char cmd[512];
  snprintf(cmd, sizeof(cmd),
           "localedef -i tr_TR -f ISO-8859-9 %s/tr_TR.ISO-8859-9 "
           ">/dev/null 2>&1",
           dir);
  if (system(cmd) == -1) return false;
  if (setenv("LOCPATH", dir, 1) != 0) return false;
  return setlocale(LC_CTYPE, "tr_TR.ISO-8859-9") && toupper('i') == 0xDD &&
         tolower('I') == 0xFD;
}

/* The case functions fold ASCII letters only, whatever the locale. Under a
 * Turkish locale toupper('i') is the dotted capital I (0xDD in Latin-5) and
 * tolower('I') the dotless small i (0xFD), and the Latin-5 letters above
 * 0x7F fold among themselves. It catches a regression because with the ctype
 * functions instead of the ASCII folding, "iI" converts to "\xDD" "I" and to
 * "i\xFD", and the bytes above 0x7F change. */
TEST(cstring_case, folds_ascii_letters_only_under_a_turkish_locale) {
  char dir[64];
  snprintf(dir, sizeof(dir), "cstr_tr_locale_%ld", (long)getpid());
  if (mkdir(dir, 0700) != 0) {
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
  unsetenv("LOCPATH");
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

/* In the C locale the six ASCII white-space bytes get trimmed from both ends
 * and nothing else does. */
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

/* Sets LC_CTYPE to a Latin-1 locale whose white-space class also holds the
 * no-break space (0xA0) and NEL (0x85), and returns true when the process is
 * running under one afterwards. It writes a small locale definition into
 * dir, builds it with localedef(1) and loads it from there through LOCPATH. */
static bool cstr_set_wide_space_ctype(const char *dir) {
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
}

/* cstring_trim only removes ASCII white space, whatever the locale. Under a
 * locale whose ctype functions count the no-break space (0xA0) and NEL
 * (0x85) as white space, both stay, and so does the last byte of the UTF-8
 * sequence C3 A0. Swap in isspace() for the ASCII test and the 0xA0 at
 * each end goes away and "x\xC3\xA0" loses its last
 * byte. */
TEST(cstring_trim,
     keeps_bytes_above_0x7f_under_a_locale_that_calls_them_space) {
  char dir[64];
  snprintf(dir, sizeof(dir), "cstr_sp_locale_%ld", (long)getpid());
  if (mkdir(dir, 0700) != 0) {
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
  unsetenv("LOCPATH");
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
