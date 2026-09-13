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
  // itself requests capacity 17 which is already satisfied; no realloc.
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

// A realloc-counting allocator, distinct from the budget-style OOM allocator
// further below: it always delegates to the real allocator, purely to let a
// test assert on how many times realloc() was actually invoked.
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

// cstring_reset() must not issue a realloc() call at all when the string is
// already at the minimum capacity (the common case: a freshly created
// string, or one that has already been reset); there would be nothing to
// shrink. Checking only the resulting capacity (as reset_clears_content and
// reset_shrinks_capacity above already do) cannot distinguish "the call was
// skipped" from "the call was made and happened to be a no-op", so this
// asserts the actual call count instead.
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

// The converse of the above: a string that has actually grown past the
// minimum capacity must still shrink back via exactly one realloc() call.
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

// ------------------------------------------------------------------------
// cstring_replace()'s ccol_container_full overflow guards, exercised via
// cstring_replace_compute_new_length_for_tests() rather than through
// cstring_replace() itself: reaching either overflow condition through the
// real API would require constructing actual multi-gigabyte strings (a
// several-GB source string built from a single repeated character, together
// with a several-GB replacement string), impractical for a routine test run.
// These tests exercise the exact same arithmetic cstring_replace() itself
// runs (see compute_replace_new_length() in cstring.c), just fed with
// fabricated length/count values instead of real backing memory.
// ------------------------------------------------------------------------

TEST(cstrings, replace_new_length_normal_growth_case) {
  size_t new_len = 0;
  // 10-byte string, 3 occurrences of a 2-byte needle each replaced by a
  // 5-byte replacement: 10 + (5-2)*3 = 19.
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(10, 2, 5, 3, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)19);
}

TEST(cstrings, replace_new_length_normal_shrink_case) {
  size_t new_len = 0;
  // 20-byte string, 2 occurrences of a 5-byte needle each replaced by a
  // 2-byte replacement: 20 - (5-2)*2 = 14.
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(20, 5, 2, 2, &new_len);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(new_len, (size_t)14);
}

TEST(cstrings, replace_new_length_multiplication_overflow_detected) {
  // (rlen - nlen) * count must overflow size_t: (SIZE_MAX/2 + 1) * 2 wraps.
  size_t new_len = 12345;  // sentinel; must be left untouched on failure
  size_t nlen = 1;
  size_t rlen = (SIZE_MAX / 2) + 2;  // rlen - nlen == SIZE_MAX/2 + 1
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(0, nlen, rlen, 2, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

TEST(cstrings, replace_new_length_addition_overflow_detected) {
  // added itself doesn't overflow (it's 1), but orig_length + added does.
  size_t new_len = 12345;  // sentinel; must be left untouched on failure
  ccol_retval_t rv =
      cstring_replace_compute_new_length_for_tests(SIZE_MAX, 1, 2, 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

// Distinct from replace_new_length_addition_overflow_detected above: here
// orig_length + added lands on exactly SIZE_MAX without ever dipping below
// orig_length, so the "new_len < orig_length" wraparound check alone would
// NOT catch it. This is the case cstring_length_fits_with_terminator()
// exists to reject, since cstring_replace()'s caller still needs to add 1
// to new_len (for the null terminator) before it can be used as a capacity.
TEST(cstrings, replace_new_length_result_exactly_size_max_detected) {
  size_t new_len = 12345;  // sentinel; must be left untouched on failure
  ccol_retval_t rv = cstring_replace_compute_new_length_for_tests(
      SIZE_MAX - 1, 1, 2, 1, &new_len);
  REQUIRE_EQ(rv, ccol_container_full);
  REQUIRE_EQ(new_len, (size_t)12345);
}

// The shared size_t-wraparound guard every mutating function (append,
// prepend, insert, set, create_full) and cstring_replace()'s own length
// arithmetic route through before requesting a length + 1 sized buffer.
// Exercised directly, since reaching it through any public API would
// require a string spanning the entire address space.
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
  // Request more than available from start=2 -> only 3 chars ("llo")
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
  // start == length -> returns empty string
  cstr sub = cstring_substring(s, 5, 1, NULL);
  REQUIRE_NE((void *)sub, NULL);
  REQUIRE_STREQ(cstring_c_str(sub), "");
  cstring_destroy(sub);
  // start beyond length -> returns empty string
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
  // A 30-char initial string needs at least 31 bytes -> rounds up to 32
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
  // "123456789" len=9; append self -> "123456789123456789" len=18
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
  // src ends at s->length-1; no overlap.
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_append(s, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hellohello");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

TEST(cstrings, prepend_self_alias_triggers_realloc) {
  // "123456789" prepend self -> "123456789123456789"
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
  // "123456789" insert self at pos 0 -> "123456789123456789"
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
  // destination with dst > src; previously a forward memcpy would corrupt.
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
  // memmove(s->data+2, s->data, 5) has dst > src with overlap; memmove
  // handles it correctly.
  cstr_construct(s, "hello");
  REQUIRE_EQ(cstring_get_capacity(s), (size_t)CSTRING_MIN_CAPACITY);
  ccol_retval_t rv = cstring_insert(s, 2, cstring_c_str(s));
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(cstring_c_str(s), "hehellollo");
  REQUIRE_EQ(cstring_length(s), (size_t)10);
  cstr_destroy(s);
}

// Every other alias-with-realloc test above uses alias_off == 0 (the whole
// string aliased); every other nonzero-alias-offset test stays within the
// current capacity, so grow_to() never actually reallocates. This test
// combines both: a nonzero alias offset AND an actual buffer relocation,
// which is exactly the scenario the "re-derive str after grow_to() may have
// moved the buffer" fix exists for.
TEST(cstrings, prepend_alias_offset_nonzero_triggers_realloc) {
  // 20-char base -> init_cap = next_pow2(21) = 32. Prepending its own
  // offset-5 suffix (15 chars) grows the string to 35 bytes, forcing an
  // actual realloc (new capacity 64) while alias_off (5) is nonzero.
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

// Same combination as prepend_alias_offset_nonzero_triggers_realloc above,
// but for cstring_insert() with a nonzero pos as well: alias_off (5) > pos
// (3), so this also exercises the "second pointer correction after the
// first memmove shifts the source right" branch together with a genuine
// reallocation.
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

// The converse combination of insert_alias_offset_nonzero_triggers_realloc
// above: alias_off (2) < pos (10), together with a genuine reallocation.
// cstring_insert()'s second pointer correction is gated on `alias_off > pos`
// specifically because the region [alias_off, alias_off+str_len) is provably
// never touched by the first memmove's shift whenever alias_off <= pos (see
// cstring.c's own comment on that memmove); this pins that no-correction
// path is still correct once grow_to() actually relocates the buffer, not
// just when the buffer happens to already be large enough.
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

// The exact boundary between the two combinations above: alias_off == pos,
// together with a genuine reallocation. This is the case the `> pos` (not
// `>= pos`) condition in cstring_insert()'s second correction deliberately
// excludes: the region [alias_off, alias_off+str_len) == [pos, pos+str_len)
// sits precisely at the first memmove's own shift boundary, so it is left
// untouched by that shift regardless of whether grow_to() relocated the
// buffer. insert_self_alias_at_nonzero_pos_no_realloc (INSERT section,
// above) already covers this same alias_off == pos boundary for alias_off
// == 0; this covers it for a nonzero alias_off, combined with an actual
// reallocation, which that test does not exercise.
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

// set_self_alias_full_length_no_realloc (SET section, above) already checks
// the resulting content is correct; this checks the "no realloc" half of its
// own comment directly, via an actual call-count assertion, mirroring
// reset_at_minimum_capacity_skips_realloc's use of the same counting
// allocator for the identical kind of claim.
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

// ========================================================================
// MACRO HYGIENE REGRESSION (cstr_insert's internal retval local)
// ========================================================================

// cstr_insert used to declare its internal retval as plain '_r'; a caller
// whose own pos argument was literally named '_r' would silently bind to
// that not-yet-initialized local instead (C's declarator-scope rule) rather
// than the caller's real value, since ccol_retval_t implicitly converts to
// size_t with no diagnostic guaranteed. Confirmed to actually reproduce
// (wrong insertion position, or a spurious ccol_invalid_args depending on
// what garbage the uninitialized enum held) against the pre-fix macro before
// being fixed by renaming the internal local to __cstr_insert_r.
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
// Every query/modification/search/substring function in this module is
// documented to assert (abort via ccol_assert) when handed a NULL cstr.
// None of these paths were previously exercised by this suite. Each one is
// run in its own forked child (mirroring the fork+SIGABRT pattern used
// throughout this codebase, e.g. tests/cvector's
// type_safe_at_out_of_bounds_is_fatal) so the process-aborting assert
// doesn't take down the whole test binary, and the parent confirms the
// child actually died via SIGABRT rather than merely exiting or crashing
// some other way (e.g. a plain NULL-deref SIGSEGV, which is exactly the
// failure mode cstring_get_mprocs had before it gained its own NULL guard).
// ========================================================================

/* Returns false (leaving *out_status untouched) if fork() or waitpid()
 * itself failed, so the caller can tell that apart from a genuine
 * WIFSIGNALED-but-not-SIGABRT test failure; REQUIRE_* cannot be used
 * directly in this function since its bare `return;` only fits a void
 * TEST body, not this int-returning helper. */
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
    _exit(0); /* unreachable if the assert aborted as expected */
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
// cstring_get_mprocs previously had no NULL guard at all and would segfault
// (WIFSIGNALED + SIGSEGV) rather than assert (WIFSIGNALED + SIGABRT); this
// pins the fixed, now-consistent-with-the-rest-of-the-module behavior.
DEFINE_NULL_ARG_FATAL_TEST(null_get_mprocs_is_fatal,
                           (void)cstring_get_mprocs(NULL))

// ========================================================================
// OUT OF MEMORY / ALLOCATOR FAILURE TESTS
//
// A budget-style counting allocator: the first g_cstr_oom_budget calls to
// malloc/calloc/realloc succeed (delegating to the real allocator); every
// call after the budget is exhausted returns NULL. free() always delegates
// to the real free() unconditionally (never budget-tracked), so whatever DID
// succeed is still released correctly by every cleanup path under test.
//
// Calibrated against the real, built library (not guessed) via a standalone
// counting harness: cstring_create_full() always costs exactly 3 calls
// (1 calloc for the container + 1 malloc for the copied mprocs + 1 malloc
// for the data buffer, in that order) for any initial content that fits
// within the minimum 16-byte capacity; a grow_to() that actually reallocates
// costs exactly 1 further realloc call; cvector_create_full(sizeof(cstr))
// (used internally by cstring_split) costs the identical 3 calls.
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
  g_cstr_oom_budget = 0;  // fails the very first call (calloc for the struct)
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_full_oom_mprocs_copy_alloc_fails) {
  char *err = NULL;
  // Budget covers exactly the container calloc; the mprocs-copy malloc
  // (the 2nd call) must fail.
  g_cstr_oom_budget = 1;
  cstr s = cstring_create_full("hello", &g_cstr_oom_procs, &err);
  REQUIRE_EQ((void *)s, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cstrings, create_full_oom_data_buffer_alloc_fails) {
  char *err = NULL;
  // Budget covers the container calloc and the mprocs-copy malloc; the data
  // buffer malloc (the 3rd call) must fail.
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

TEST(cstrings, substring_oom_grow_to_fails_after_result_created) {
  char *err = NULL;
  // Source must hold >= 16 characters so the requested substring's own
  // length forces cstring_grow_to() to actually reallocate past the
  // result's initial minimum capacity.
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("0123456789abcdefghij", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  // Budget covers exactly the result's own cstring_create_full() (3 calls);
  // the substring-specific grow_to() realloc (the 4th call) must fail.
  g_cstr_oom_budget = 3;
  char *sub_err = NULL;
  cstr sub = cstring_substring(s, 0, 20, &sub_err);
  REQUIRE_EQ((void *)sub, NULL);
  REQUIRE_NE((void *)sub_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
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
  g_cstr_oom_budget = 1000000;  // teardown itself must never be budget-limited
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

  g_cstr_oom_budget = 0;  // cvector_create_full()'s own first call must fail
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

  // Budget covers the vector (3 calls) and the first token "a" (3 calls);
  // the second token "b"'s own container calloc (the 7th call) must fail.
  // Must return NULL with the first token already destroyed by
  // destroy_cstr_vector(), not leaked (verified separately under
  // `make memtest`).
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

  // Budget covers the vector and both loop tokens "a"/"b" (9 calls); the
  // final tail token "c"'s own container calloc (the 10th call) must fail.
  g_cstr_oom_budget = 9;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, split_oom_token_grow_to_fails) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
  // First token is 16 characters, forcing its own post-creation grow_to()
  // to actually reallocate past the default minimum capacity.
  cstr s =
      cstring_create_full("0123456789abcdef,short", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  // Budget covers the vector (3) and the first token's own
  // cstring_create_full() (3); its grow_to() realloc (the 7th call) must
  // fail.
  g_cstr_oom_budget = 6;
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_EQ((void *)parts, NULL);
  REQUIRE_NE((void *)split_err, NULL);

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}

TEST(cstrings, split_oom_budget_exactly_sufficient_succeeds) {
  char *err = NULL;
  g_cstr_oom_budget = 3;
  cstr s = cstring_create_full("a,b,c", &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  g_cstr_oom_budget = 12;  // vector + 3 tokens, 3 calls each
  char *split_err = NULL;
  cvec parts = cstring_split(s, ",", &split_err);
  REQUIRE_NE((void *)parts, NULL);
  REQUIRE_EQ((void *)split_err, NULL);
  REQUIRE_EQ(cvector_elem_count(parts), (size_t)3);

  g_cstr_oom_budget = 1000000;
  free_cstr_vector_oom(parts);
  cstring_destroy(s);
}

// cstring_reset() is documented to leave the buffer's capacity unchanged
// (while still unconditionally resetting length to 0) when its internal
// shrink-realloc fails, rather than losing the existing content or crashing.
TEST(cstrings, reset_realloc_failure_leaves_capacity_unchanged) {
  char *err = NULL;
  g_cstr_oom_budget = 1000000;
  cstr s = cstring_create_full(NULL, &g_cstr_oom_procs, &err);
  REQUIRE_NE((void *)s, NULL);

  const char *chunk =
      "0123456789abcdef0123456789abcdef";  // 33 chars, forces grow
  REQUIRE_EQ(cstring_append(s, chunk), ccol_success);
  size_t grown_cap = cstring_get_capacity(s);
  REQUIRE_TRUE(grown_cap > CSTRING_MIN_CAPACITY);

  g_cstr_oom_budget = 0;  // the shrink-back-to-minimum realloc must fail
  cstring_reset(s);
  REQUIRE_EQ(cstring_length(s), (size_t)0);
  REQUIRE_STREQ(cstring_c_str(s), "");
  REQUIRE_EQ(cstring_get_capacity(s), grown_cap);  // unchanged, not shrunk

  g_cstr_oom_budget = 1000000;
  cstring_destroy(s);
}
