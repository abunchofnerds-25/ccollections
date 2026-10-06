#include <cstring.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <unistd.h>

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

  // Append enough bytes to go past the first buffer
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
  // "hello" fits in the smallest capacity, which is 16. This is why
  // set(s, c_str(s)) must not realloc. The content must not change.
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  REQUIRE_EQ(cstring_length(s), (size_t)5);
  cstring_destroy(s);
}

TEST(cstrings, set_self_alias_full_length_no_realloc) {
  // A 16-char string gets init_cap = next_pow2(17) = 32. A set of that
  // string to itself asks for a capacity of 17, which the string already
  // has, so there is no realloc. This test drives the self-alias path
  // without a realloc. Its string is longer than the one in
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
  // A suffix of s->data always has str_len < s->length < s->capacity. No
  // realloc can happen here. This test drives the path that tracks the
  // alias offset. On that path the code must compute the internal pointer
  // again after grow_to, and grow_to does nothing in this case.
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

// An allocator that counts the realloc calls. It is not the same as the
// budget-style out-of-memory allocator further below. This one always gives
// the work to the real allocator. Its only purpose is to let a test assert
// on the number of calls to realloc().
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

// cstring_reset() must make no realloc() call when the string is already at
// the smallest capacity. There is nothing to shrink in that case. This is
// the common case, because a new string and a string after a reset are both
// at that capacity. A check of the resulting capacity alone cannot tell
// "the code skipped the call" from "the code made the call and the call did
// nothing". The tests reset_clears_content and reset_shrinks_capacity above
// make only that check. This test asserts on the real call count instead.
TEST(cstrings, reset_at_minimum_capacity_skips_realloc) {
  cstr s = cstring_create_full(NULL, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_realloc_call_count = 0;
  cstring_reset(s);
  REQUIRE_EQ(g_cstr_realloc_call_count, 0);
  REQUIRE_EQ(cstring_length(s), (size_t)0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  cstring_destroy(s);
}

// This test is the converse of the one above. A string that grew past the
// smallest capacity must shrink back with exactly one realloc() call.
TEST(cstrings, reset_above_minimum_capacity_still_reallocs_once) {
  cstr s = cstring_create_full(NULL, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);

  const char *chunk = "0123456789abcdef0123456789abcdef";  // 32 chars
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  REQUIRE_TRUE(cstring_get_capacity(s) > (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_realloc_call_count = 0;
  cstring_reset(s);
  REQUIRE_EQ(g_cstr_realloc_call_count, 1);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

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
  // strstr(data, "") returns data, so find("") always gives the offset 0.
  cstr s = cstring_create("hello", NULL);
  REQUIRE_EQ(cstring_find(s, ""), (size_t)0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_empty_needle) {
  // An empty needle has no last occurrence with a clear meaning. The
  // library returns s->length, which is the past-the-end position. This is
  // the same behaviour as the one in C++.
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
  // A replacement with an empty string deletes the needle
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
// cstring_replace(). They call cstring_replace_compute_new_length_for_tests()
// and not cstring_replace() itself. To reach either overflow condition
// through the real API, a test needs real strings of several gigabytes. One
// such string is a source string of several gigabytes that holds one
// character many times. The test also needs a replacement string of several
// gigabytes. This is not practical for a normal test run. These tests drive
// the same arithmetic that cstring_replace() runs. That arithmetic is in
// compute_replace_new_length() in cstring.c. They give it invented values
// for the length and the count, and no real memory behind them.
// ------------------------------------------------------------------------

TEST(cstrings, replace_new_length_normal_growth_case) {
  size_t new_len = 0;
  // A 10-byte string with 3 occurrences of a 2-byte needle. A 5-byte
  // replacement takes the place of each one: 10 + (5-2)*3 = 19.
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(10, 2, 5, 3, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)19);
}

TEST(cstrings, replace_new_length_normal_shrink_case) {
  size_t new_len = 0;
  // A 20-byte string with 2 occurrences of a 5-byte needle. A 2-byte
  // replacement takes the place of each one: 20 - (5-2)*2 = 14.
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(20, 5, 2, 2, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)14);
}

TEST(cstrings, replace_new_length_multiplication_overflow_detected) {
  // (rlen - nlen) * count must overflow a size_t: (SIZE_MAX/2 + 1) * 2 wraps.
  size_t new_len = 12345;  // sentinel. The call must not touch it on failure
  size_t nlen = 1;
  size_t rlen = (SIZE_MAX / 2) + 2;  // rlen - nlen == SIZE_MAX/2 + 1
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(0, nlen, rlen, 2, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

TEST(cstrings, replace_new_length_addition_overflow_detected) {
  // The value added does not overflow, because it is 1. The sum
  // orig_length + added does overflow.
  size_t new_len = 12345;  // sentinel. The call must not touch it on failure
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(SIZE_MAX, 1, 2, 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

// This test is not the same as replace_new_length_addition_overflow_detected
// above. Here orig_length + added lands on exactly SIZE_MAX, and it never
// goes below orig_length. The wraparound check "new_len < orig_length" alone
// therefore does NOT catch it. This is the case that
// cstring_length_fits_with_terminator() exists to reject. The caller of
// cstring_replace() still adds 1 to new_len for the null terminator before
// it uses that value as a capacity.
TEST(cstrings, replace_new_length_result_exactly_size_max_detected) {
  size_t new_len = 12345;  // sentinel. The call must not touch it on failure
  ccol_retval_t rv = cstring_replace_compute_new_length_for_tests(
      SIZE_MAX - 1, 1, 2, 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

// The guard below protects against a wraparound of a size_t. Every function
// that changes a string goes through it before it asks for a buffer of
// length + 1 bytes. Those functions are append, prepend, insert, set and
// create_full. The length arithmetic of cstring_replace() goes through the
// same guard. This test drives the guard directly, because a test that goes
// through the public API needs a string that fills the full address space.
TEST(cstrings, length_fits_with_terminator_guard) {
  REQUIRE_TRUE(cstring_length_fits_with_terminator_for_tests(0));
  REQUIRE_TRUE(cstring_length_fits_with_terminator_for_tests(1));
  REQUIRE_TRUE(cstring_length_fits_with_terminator_for_tests(SIZE_MAX - 1));
  REQUIRE_FALSE(cstring_length_fits_with_terminator_for_tests(SIZE_MAX));
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
  // A request for zero characters from a string that is not empty must
  // return an empty cstring and not NULL.
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
  // A change to the original does not affect the substring
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
  // A first string of 30 chars needs at least 31 bytes -> rounds up to 32
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
  // The program destroys s automatically when the scope above ends
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
// SELF-ALIAS TESTS (use-after-realloc and overlapping-copy hazards)
// A string of 9 characters or more makes sure that the doubled length goes
// past the smallest capacity of 16 bytes. This forces a realloc, so these
// tests drive those hazards.
// ========================================================================

TEST(cstrings, append_self_alias_triggers_realloc) {
  // "123456789" has len=9. An append of the string to itself gives
  // "123456789123456789" with len=18. Because 18+1 > 16, cstring_grow_to
  // must realloc. The code must then compute str again against the new
  // buffer. Without that step, str dangles.
  cstr_construct(s, "123456789");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, append_suffix_alias_no_realloc) {
  // The 9-char source has a capacity of 16. An append of the 5-char suffix
  // "56789" gives 14 chars, which fits without a realloc, because
  // grow_to(15) <= 16 does nothing. This test drives the path that tracks
  // the alias offset, and it does so without a reallocation.
  cstr_construct(s, "123456789");
  // cstring_c_str(s) + 4 points to "56789"
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12345678956789");
  cstr_destroy(s);
}

TEST(cstrings, append_self_alias_no_realloc) {
  // "hello" has 5 chars and a capacity of 16. An append of the string to
  // itself gives "hellohello", which has 10 chars, and grow_to(11) <= 16
  // does nothing. This test drives the path with alias_off==0 and no
  // realloc. The memcpy is safe here, because dst starts at s->length and
  // src ends at s->length-1. The two regions do not overlap.
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_triggers_realloc) {
  // A prepend of "123456789" to itself gives "123456789123456789"
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  REQUIRE_EQ(cstring_length(s), (size_t)18);
  cstr_destroy(s);
}

TEST(cstrings, prepend_suffix_alias_no_realloc) {
  // The 9-char source has a capacity of 16. A prepend of the 5-char suffix
  // "56789" gives 14 chars, which fits without a realloc. This test drives
  // the path with alias_off > 0 and no reallocation. That path makes a
  // second correction of the pointer after the memmove.
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "56789123456789");
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_no_realloc) {
  // "hello" has 5 chars and a capacity of 16. A prepend of the string to
  // itself gives "hellohello", which has 10 chars, and grow_to(11) <= 16
  // does nothing. This test drives the path with alias_off==0 and no
  // realloc. The final memmove(s->data, s->data, 5) copies with one pointer
  // for both sides, and memmove handles that safely. The memmove before it
  // already moved the content to the right, so the positions 0..4 still
  // hold the correct prefix.
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_triggers_realloc) {
  // An insert of "123456789" into itself at pos 0 gives
  // "123456789123456789"
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_insert(s, 0, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "123456789123456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_after_pos_no_realloc) {
  // An insert of "56789", which sits at offset 4, into "123456789" at pos 2
  // gives "12567893456789". The 9-char source has a capacity of 16. An
  // insert of 5 chars gives 14 chars, and grow_to(15) <= 16 does nothing.
  // Here alias_off is 4 and pos is 2, so alias_off > pos. The first memmove
  // moves the source to the right by str_len, which is 5. The code must
  // then compute the pointer again. This test drives that second correction
  // without a reallocation.
  cstr_construct(s, "123456789");
  ccol_retval_t rv = cstring_insert(s, 2, cstring_c_str(s) + 4);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "12567893456789");
  cstr_destroy(s);
}

TEST(cstrings, insert_alias_overlapping_dst_gt_src) {
  // The string is "abcde". The test inserts s->data+1, which is "bcde", at
  // pos 3. Here alias_off=1 <= pos=3 and str_len=4. After the memmove, the
  // source of the copy overlaps the destination with dst > src. A memcpy
  // copies forward, so it corrupts the result. The code needs a memmove,
  // which is safe with an overlap. The expected result is
  // "abc" + "bcde" + "de" = "abcbcdede".
  cstr_construct(s, "abcde");
  ccol_retval_t rv = cstring_insert(s, 3, cstring_c_str(s) + 1);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "abcbcdede");
  cstr_destroy(s);
}

TEST(cstrings, insert_self_alias_at_nonzero_pos_no_realloc) {
  // "hello" has 5 chars and a capacity of 16. An insert of the string into
  // itself at pos 2 gives "he" + "hello" + "llo" = "hehellollo", which has
  // 10 chars, and grow_to(11) <= 16. This test drives alias_off==0 with
  // pos>0 and no reallocation. The final memmove(s->data+2, s->data, 5) has
  // dst > src with an overlap, and memmove handles it correctly.
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_insert(s, 2, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hehellollo");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

// Every other test above that combines an alias with a realloc uses
// alias_off == 0, where the alias covers the whole string. Every other test
// with an alias offset that is not zero stays inside the current capacity,
// so grow_to() makes no reallocation there. This test combines the two: an
// alias offset that is not zero AND a real move of the buffer. That is the
// exact case that the second computation of str after grow_to() exists for,
// because grow_to() can move the buffer.
TEST(cstrings, prepend_alias_offset_nonzero_triggers_realloc) {
  // A 20-char base gives init_cap = next_pow2(21) = 32. A prepend of its
  // own suffix at offset 5, which has 15 chars, grows the string to 35
  // bytes. That forces a real realloc, and the new capacity is 64. The
  // value of alias_off is 5 here, which is not zero.
  const char *base = "12345678901234567890";
  char expected[64];
  snprintf(expected, sizeof(expected), "%s%s", base + 5, base);

  cstr s = cstring_create_full(base, &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_prepend(s, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), (size_t)35);

  cstring_destroy(s);
}

// This test uses the same combination as
// prepend_alias_offset_nonzero_triggers_realloc above. It uses
// cstring_insert() and a pos that is not zero. Here alias_off is 5 and pos
// is 3, so alias_off > pos. This test therefore also drives the branch that
// makes the second correction of the pointer after the first memmove moves
// the source to the right. It does so together with a real reallocation.
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
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_insert(s, pos, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// This test is the converse of insert_alias_offset_nonzero_triggers_realloc
// above. Here alias_off is 2 and pos is 10, so alias_off < pos, and there is
// a real reallocation. The second correction of the pointer in
// cstring_insert() runs only when `alias_off > pos`. The reason is that the
// first memmove provably never touches the region
// [alias_off, alias_off+str_len) when alias_off <= pos. The comment on that
// memmove in cstring.c gives the proof. This test pins that the path
// without a correction is still correct when grow_to() moves the buffer. It
// is not enough to test it when the buffer is already large enough.
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
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_insert(s, pos, cstring_c_str(s) + 2);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// This test covers the exact boundary between the two combinations above.
// Here alias_off == pos, and there is a real reallocation. The second
// correction in cstring_insert() uses the condition `> pos` and not
// `>= pos`, so it excludes this case on purpose. The region
// [alias_off, alias_off+str_len) == [pos, pos+str_len) sits exactly at the
// boundary of the shift of the first memmove. That shift therefore never
// touches the region, and this is true whether or not grow_to() moved the
// buffer. The test insert_self_alias_at_nonzero_pos_no_realloc, in the
// INSERT section above, already covers this alias_off == pos boundary for
// an alias_off of 0. This test covers it for an alias_off that is not zero,
// together with a real reallocation, which that test does not drive.
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
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_insert(s, pos, cstring_c_str(s) + 5);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(g_cstr_realloc_call_count >= 1);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)64);
  REQUIRE_STREQ(cstring_c_str(s), expected);
  REQUIRE_EQ(cstring_length(s), blen + ilen);

  cstring_destroy(s);
}

// The test set_self_alias_full_length_no_realloc, in the SET section above,
// already checks that the resulting content is correct. This test checks the
// "no realloc" half of the comment on that test. It does so with an
// assertion on the real call count. The test
// reset_at_minimum_capacity_skips_realloc uses the same counting allocator
// for the same kind of claim.
TEST(cstrings, set_self_alias_no_realloc_verified_by_call_count) {
  cstr s =
      cstring_create_full("1234567890123456", &g_cstr_counting_procs, NULL);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)32);

  g_cstr_realloc_call_count = 0;
  ccol_retval_t rv = cstring_set(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(g_cstr_realloc_call_count, 0);
  REQUIRE_STREQ(cstring_c_str(s), "1234567890123456");
  REQUIRE_EQ(cstring_length(s), (size_t)16);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)32);

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
  // The program destroys s automatically when the scope above ends
}

TEST(cstrings, trim_single_whitespace_char) {
  cstr s = cstring_create(" ", NULL);
  cstring_trim(s);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_length(s), 0);
  cstring_destroy(s);
}

TEST(cstrings, rfind_overlapping_needles) {
  // "ababa" holds "aba" at the offsets 0 and 2, and the two overlap.
  // rfind must return 2 and not 0.
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
  // cstring_c_str(s) is a safe replacement argument here. The loop that
  // builds the result reads the replacement bytes before the code frees the
  // old buffer.
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

// cstr_insert must not name its internal retval plain '_r'. A caller can
// name its own pos argument '_r'. That argument then binds to the
// not-yet-initialized local of the macro and not to the real value of the
// caller, and nothing reports it. The declarator-scope rule of C is the
// reason. A ccol_retval_t also converts to a size_t on its own, and no
// compiler must report that. This test is non-vacuous: give the internal
// local the plain name '_r' and the test fails. It fails either with a
// wrong position for the insert or with a false ccol_invalid_args. Which
// one it is depends on the garbage value in the uninitialized enum.
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
// The documentation of this module says that a NULL cstr makes a function
// assert. The function then aborts with ccol_assert. This holds for every
// function in the module that queries, changes or searches a string, and
// for the substring function. Each test below runs in its own forked child.
// The abort of the assert therefore does not take down the whole test
// binary. The parent then confirms that the child died with SIGABRT, and
// not with a plain exit or some other crash. Without a NULL guard, the
// child dies with a SIGSEGV from a dereference of NULL instead. This
// fork+SIGABRT pattern is common in this codebase. One other user of it is
// type_safe_at_out_of_bounds_is_fatal in tests/cvector.
// ========================================================================

/* Returns false and does not touch *out_status when fork() or waitpid()
 * fails. The caller can then tell that case apart from a real test failure,
 * where the child is WIFSIGNALED but the signal is not SIGABRT. This
 * function cannot use REQUIRE_*, because those macros end in a bare
 * `return;`. That fits a void TEST body only, and this helper returns an
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
    _exit(0); /* not reachable when the assert aborts, as it must */
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
// cstring_get_mprocs needs a NULL guard of its own. Without that guard it
// segfaults, which gives WIFSIGNALED + SIGSEGV, and it does not assert,
// which gives WIFSIGNALED + SIGABRT. That behaviour differs from the rest
// of this module.
DEFINE_NULL_ARG_FATAL_TEST(null_get_mprocs_is_fatal,
                           (void)cstring_get_mprocs(NULL))

// ========================================================================
// OUT OF MEMORY / ALLOCATOR FAILURE TESTS
//
// A counting allocator with a budget. The first g_cstr_oom_budget calls to
// malloc, calloc and realloc succeed, and they give the work to the real
// allocator. Every call after the budget runs out returns NULL. free()
// always gives the work to the real free(), and the budget never covers it.
// Every cleanup path under test therefore still frees correctly whatever a
// call did allocate.
//
// The budgets below are exact allocation counts and not estimates.
// cstring_create_full() always costs exactly 3 calls: 1 calloc for the
// container, then 1 malloc for the copy of the mprocs, then 1 malloc for
// the data buffer. This holds for any first content that fits in the
// smallest capacity of 16 bytes. A grow_to() that really reallocates costs
// exactly 1 more realloc call. cvector_create_full(sizeof(cstr)) costs the
// same 3 calls, and cstring_split uses it internally.
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
  g_cstr_oom_budget = 0;  // the first call fails: the calloc for the struct
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_full_oom_mprocs_copy_alloc_fails) {
  char *err = NULL;
  // The budget covers the calloc for the container only. The malloc for the
  // copy of the mprocs, which is the 2nd call, must fail.
  g_cstr_oom_budget = 1;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_full_oom_data_buffer_alloc_fails) {
  char *err = NULL;
  // The budget covers the calloc for the container and the malloc for the
  // copy of the mprocs. The malloc for the data buffer, which is the 3rd
  // call, must fail.
  g_cstr_oom_budget = 2;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_full_oom_budget_exactly_sufficient_succeeds) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ((void *)err, NULL);
  REQUIRE_STREQ(cstring_c_str(s), "hello");
  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, append_oom_grow_failure_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
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
  g_cstr_oom_budget = 3;
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
  g_cstr_oom_budget = 3;
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
  g_cstr_oom_budget = 3;
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
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full(NULL, &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 0;
  REQUIRE_FALSE(cstring_reserve(s, 1024));
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, replace_oom_new_buffer_alloc_fails_leaves_string_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
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
  g_cstr_oom_budget = 3;
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

// A substring longer than the smallest capacity costs the same 3 allocator
// calls as a short one: the buffer is allocated once, at its final size, and
// never grown. The 20 characters below need a capacity of 32.
TEST(cstrings, substring_of_a_long_range_allocates_its_buffer_once) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("0123456789abcdefghij", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 3;
  char *sub_err = (char *)"poison";
  cstr sub = cstring_substring(s, 0, 20, &sub_err);
  int left = g_cstr_oom_budget;
  bool ok = sub != NULL;
  bool content_ok =
      ok && strcmp(cstring_c_str(sub), "0123456789abcdefghij") == 0;
  size_t cap = ok ? cstring_get_capacity(sub) : 0;

  g_cstr_oom_budget = 2;  // one call short: the data buffer must fail
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
  g_cstr_oom_budget = 3;
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
  g_cstr_oom_budget = 3;
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
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  // The budget covers the vector, which is 3 calls, and the first token "a",
  // which is 3 more calls. The container calloc of the second token "b",
  // which is the 7th call, must fail. The split must return NULL, and
  // destroy_cstr_vector() must already have destroyed the first token. That
  // token must not leak. `make memtest` catches a regression there.
  g_cstr_oom_budget = 6;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, split_oom_last_token_creation_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  // The budget covers the vector and the two loop tokens "a" and "b", which
  // is 9 calls in total. The container calloc of the final tail token "c",
  // which is the 10th call, must fail.
  g_cstr_oom_budget = 9;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

// A token longer than the smallest capacity costs the same 3 allocator calls
// as a short one, because its buffer is allocated once at its final size. The
// vector takes 3 calls and each of the two tokens 3 more, so 9 calls are
// exactly enough and 8 are one short.
TEST(cstrings, split_allocates_a_long_token_once) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
  cstr s =
      cstring_create_full("0123456789abcdef,short", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 9;
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

  g_cstr_oom_budget = 8;
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
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 12;  // the vector and 3 tokens, with 3 calls for each
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ((void *)split_err, NULL);
  REQUIRE_EQ(cvector_elem_count(parts), (size_t)3);

  g_cstr_oom_budget = 1000000;
  free_cstr_vector_oom(parts);
  cstring_destroy(s);
}

// The documentation of cstring_reset() says what happens when the internal
// realloc that shrinks the buffer fails. The capacity of the buffer does not
// change, and the function still sets the length to 0. The function does not
// lose the content and it does not crash.
TEST(cstrings, reset_realloc_failure_leaves_capacity_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 1000000;
  cstr s = cstring_create_full(NULL, &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  const char *chunk =
      "0123456789abcdef0123456789abcdef";  // 33 chars, which forces a grow
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  size_t grown_cap = cstring_get_capacity(s);
  REQUIRE_TRUE(grown_cap > CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 0;  // the realloc back to the smallest size must fail
  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), (size_t)0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_get_capacity(s), grown_cap);  // no change, no shrink

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

// ========================================================================
// RFIND AGAINST A REFERENCE SEARCH
// ========================================================================

extern unsigned long cstring_rfind_linear_count_for_tests;

/* The reference answer: every start position, from the right, compared in
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

/* A small xorshift generator, so that every run checks the same inputs. */
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

/* Random texts over alphabets of one to four letters, where matches and
 * near misses are dense, with needles of every length up to the text and
 * past it. Needles cut from the text itself guarantee many hits. */
TEST(rfind, agrees_with_the_reference_on_random_inputs) {
  char hay[1100];
  char needle[1120];
  size_t mismatches = 0;
  size_t checked = 0;
  for (int round = 0; round < 20000; round++) {
    unsigned alphabet = 1 + (unsigned)(rfind_rng_next() % 4);
    /* Half the rounds use a string below the length at which cstring_rfind
     * switches from its forward search to its backward scan, and half use a
     * string above it, so both paths meet the reference. */
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
 * with the text on long runs. These inputs reach the linear fallback of
 * cstring_rfind() with the table on the stack and on the heap, at every
 * kind of position, and each answer must equal the reference. */
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
 * Every candidate of the backward scan then agrees with the needle on its
 * first and last byte and differs from it only in the middle. */
static char *rfind_adversarial(size_t len, size_t b_at) {
  char *buf = malloc(len + 1);
  if (!buf) return NULL;
  memset(buf, 'a', len);
  if (b_at < len) buf[b_at] = 'b';
  buf[len] = '\0';
  return buf;
}

/* A needle that agrees with the text on long runs makes the backward scan
 * hand the search over to its linear fallback. That fallback keeps a short
 * needle's table on the stack and takes a long needle's table from the
 * allocator of the string; when that allocator refuses, the scan finishes
 * the search itself. Every answer must still equal the reference, and the
 * counter proves that the inputs reach the fallback. */
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

    /* The same search on a string whose allocator refuses everything. */
    g_cstr_oom_budget = 3;
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
  /* Two searches for each case reach the fallback. */
  REQUIRE_EQ(handovers, 10UL);
}

/* Periodic text with a needle that matches everywhere. The backward scan
 * meets the answer at its very first candidate and never needs the
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

/* Runs fn in a forked child with stderr on a pipe, and returns what the
 * child wrote. The child is expected to stop in ccol_fatal_err(). */
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
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("hello world", &g_cstr_oom_procs, NULL);
  if (!s) _exit(3);
  g_cstr_oom_budget = 0;
  size_t start = 2, len = 3;
  cstr sub = cstr_substring(s, start++, len++);
  (void)sub;
}

/* The failure message of cstr_substring reports the start and the length
 * that the call used, because each argument is evaluated once. */
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

/* cstring_destroy and cstr_destroy evaluate their lvalue once: the element
 * that they free is the element that they set to NULL. */
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
     * in place of ss[0]. Free the survivors, so that the failure below is a
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
 * a sentinel string, so that a macro which reads its target again sees a
 * string there and fails this test instead of stopping the process. */
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
