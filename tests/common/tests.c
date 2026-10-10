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

#include <common.h>
#include <internal/cgrowbuf.h>
#include <internal/cstrutil.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <unistd.h>
TAU_MAIN()  // sets Tau up, and gives the main function

/* Counting allocator */

static size_t g_malloc_count = 0;
static size_t g_free_count = 0;

static void *_counting_malloc(size_t size) {
  g_malloc_count++;
  return malloc(size);
}

static void _counting_free(void *ptr) {
  if (ptr) g_free_count++;
  free(ptr);
}

static void *_counting_calloc(size_t count, size_t size) {
  return calloc(count, size);
}

static void *_counting_realloc(void *ptr, size_t size) {
  return realloc(ptr, size);
}

static void reset_counters(void) {
  g_malloc_count = 0;
  g_free_count = 0;
}

/* One shared instance, which a function returns a pointer to, so that a
 * caller does not take the address of a local or a global variable
 * directly. A literal &some_local_var at the macro call site triggers
 * -Waddress ("the address of X will always evaluate as true"); with this
 * function, a call to _ccol_mem_alloc, _ccol_mem_free or ccol_scoped_ptr_mp
 * does not trigger it. */
static ccol_memmgmt_procs_t g_counting_procs_storage = {
    .malloc = _counting_malloc,
    .calloc = _counting_calloc,
    .realloc = _counting_realloc,
    .free = _counting_free};

static ccol_memmgmt_procs_t *counting_procs(void) {
  return &g_counting_procs_storage;
}

// CCOL_SCOPED_PTR TESTS

TEST(scoped_ptr, default_allocator_frees_on_scope_exit) {
  {
    ccol_scoped_ptr(buf, char);
    buf = malloc(16);
    REQUIRE_NE((void *)buf, NULL);
    strcpy(buf, "hello");
  }
  /* There is no counting hook for the default allocator, so `make memtest`
   * confirms separately that there is no leak and no double free. This test
   * exists to check that the macro form without _mp compiles and runs
   * correctly. */
}

TEST(scoped_ptr, custom_allocator_frees_via_provided_procs) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = _ccol_mem_alloc(mp, sizeof(int) * 4);
    REQUIRE_NE((void *)buf, NULL);
    REQUIRE_EQ(g_malloc_count, (size_t)1);
    REQUIRE_EQ(g_free_count, (size_t)0);
    buf[0] = 42;
  }
  REQUIRE_EQ(g_free_count, (size_t)1);
}

TEST(scoped_ptr, unused_pointer_is_a_no_op) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    (void)buf;
  }
  REQUIRE_EQ(g_free_count, (size_t)0);
}

TEST(scoped_ptr, only_final_value_is_freed_on_reassignment) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = _ccol_mem_alloc(mp, sizeof(int));
    REQUIRE_NE((void *)buf, NULL);
    /* Assigning a new value to buf without first freeing the earlier value is
     * the documented warning of the macro, which the macro does not solve. */
    _ccol_mem_free(mp, buf);
    buf = _ccol_mem_alloc(mp, sizeof(int) * 2);
    REQUIRE_NE((void *)buf, NULL);
  }
  REQUIRE_EQ(g_malloc_count, (size_t)2);
  REQUIRE_EQ(g_free_count, (size_t)2);
}

TEST(scoped_ptr, release_prevents_auto_free_and_transfers_ownership) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  int *released = NULL;
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = _ccol_mem_alloc(mp, sizeof(int));
    REQUIRE_NE((void *)buf, NULL);
    *buf = 7;
    released = ccol_scoped_ptr_release(buf);
    REQUIRE_EQ((void *)buf, NULL);
  }
  REQUIRE_EQ(g_free_count, (size_t)0);
  REQUIRE_NE((void *)released, NULL);
  REQUIRE_EQ(*released, 7);
  _ccol_mem_free(mp, released);
  REQUIRE_EQ(g_free_count, (size_t)1);
}

TEST(scoped_ptr, release_of_never_assigned_pointer_returns_null) {
  ccol_scoped_ptr(buf, int);
  int *out = ccol_scoped_ptr_release(buf);
  REQUIRE_EQ((void *)out, NULL);
  REQUIRE_EQ((void *)buf, NULL);
}

TEST(scoped_ptr, multiple_independent_pointers_in_same_scope) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(a, int, mp);
    ccol_scoped_ptr_mp(b, char, mp);
    a = _ccol_mem_alloc(mp, sizeof(int));
    b = _ccol_mem_alloc(mp, 32);
    REQUIRE_NE((void *)a, NULL);
    REQUIRE_NE((void *)b, NULL);
  }
  REQUIRE_EQ(g_malloc_count, (size_t)2);
  REQUIRE_EQ(g_free_count, (size_t)2);
}

TEST(scoped_ptr, nested_scope_frees_at_inner_block_exit) {
  reset_counters();
  ccol_memmgmt_procs_t *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(outer, int, mp);
    outer = _ccol_mem_alloc(mp, sizeof(int));
    {
      ccol_scoped_ptr_mp(inner, int, mp);
      inner = _ccol_mem_alloc(mp, sizeof(int));
      REQUIRE_EQ(g_free_count, (size_t)0);
    }
    REQUIRE_EQ(g_free_count, (size_t)1);
    REQUIRE_NE((void *)outer, NULL);
  }
  REQUIRE_EQ(g_free_count, (size_t)2);
}

static bool process_may_fail(bool should_fail) {
  ccol_memmgmt_procs_t *mp = counting_procs();
  ccol_scoped_ptr_mp(buf, int, mp);
  buf = _ccol_mem_alloc(mp, sizeof(int) * 8);
  if (!buf) return false;
  if (should_fail) return false; /* an early return, but buf is freed */
  buf[0] = 1;
  return true;
}

TEST(scoped_ptr, early_return_from_multiple_paths_still_frees) {
  reset_counters();

  REQUIRE_FALSE(process_may_fail(true));
  REQUIRE_EQ(g_malloc_count, (size_t)1);
  REQUIRE_EQ(g_free_count, (size_t)1);

  reset_counters();
  REQUIRE_TRUE(process_may_fail(false));
  REQUIRE_EQ(g_malloc_count, (size_t)1);
  REQUIRE_EQ(g_free_count, (size_t)1);
}

/* ========================================================================== */
/*                  _ccol_find_nearest_gte_power_of_two */
/* ========================================================================== */

/* common.c declares this, but common.h does not. chashmap sizes every table
 * that it allocates with it. */
extern size_t _ccol_find_nearest_gte_power_of_two(size_t input);

TEST(power_of_two, exact_powers_return_themselves) {
  for (volatile unsigned bit = 0; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(v), v);
  }
}

TEST(power_of_two, values_round_up_to_the_next_power) {
  /* One value below and one value above each power exercise both directions
   * of the binary search, as well as its early exit for "the entry before
   * this one is smaller, so this is the answer". */
  for (volatile unsigned bit = 2; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(v - 1), v);
    REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(v + 1), v << 1);
  }
}

TEST(power_of_two, small_inputs_clamp_to_one) {
  /* Every value at or below the first table entry returns that entry. */
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(0), (size_t)1);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(1), (size_t)1);
}

TEST(power_of_two, above_the_representable_maximum_is_invalid) {
  /* The table stops at the largest power of two that a size_t can hold, so
   * there is no answer for a value above it. */
  size_t top = ((size_t)1) << (sizeof(size_t) * 8 - 1);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(top), top);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(top + 1), ccol_invalid_size);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(SIZE_MAX), ccol_invalid_size);
}

/* ========================================================================== */
/*                         ccol_growbuf_t */
/* ========================================================================== */

/* A reallocator that refuses once the test arms it, which drives the
 * out-of-memory latch of the buffer on purpose instead of waiting for a
 * real allocation failure. */
static bool g_growbuf_refuse_realloc = false;
/* Counts the round trips to the allocator that one operation makes, so that
 * a test can assert that a refused request never reached the allocator. An
 * assertion on the count rather than on the outcome separates two cases:
 * "the code rejected the size before the call" and "the allocator got an
 * absurd size and said no". Only the first case is safe, because an
 * instrumented allocator stops the process on a request that plain malloc
 * only refuses. */
static size_t g_growbuf_alloc_calls = 0;
static void *_growbuf_malloc(size_t n) {
  g_growbuf_alloc_calls++;
  return malloc(n);
}
static void _growbuf_free(void *p) { free(p); }
static void *_growbuf_calloc(size_t a, size_t b) {
  g_growbuf_alloc_calls++;
  return calloc(a, b);
}
static void *_growbuf_realloc(void *p, size_t n) {
  g_growbuf_alloc_calls++;
  if (g_growbuf_refuse_realloc) return NULL;
  return realloc(p, n);
}
static ccol_memmgmt_procs_t g_growbuf_procs_storage = {
    .malloc = _growbuf_malloc,
    .calloc = _growbuf_calloc,
    .realloc = _growbuf_realloc,
    .free = _growbuf_free};
static ccol_memmgmt_procs_t *growbuf_procs(void) {
  return &g_growbuf_procs_storage;
}

TEST(growbuf, init_starts_empty_and_nul_terminated) {
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  bool ok =
      (b.len == 0) && (b.cap >= 256) && !b.oom && b.buf && b.buf[0] == '\0';
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, init_hint_below_the_floor_still_allocates_the_floor) {
  ccol_growbuf_t b;
  ccol_growbuf_init_hint(&b, growbuf_procs(), 4);
  bool ok = (b.cap >= 64) && !b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, init_hint_above_the_floor_honours_the_hint) {
  ccol_growbuf_t b;
  ccol_growbuf_init_hint(&b, growbuf_procs(), 4096);
  bool ok = (b.cap >= 4097) && !b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_grows_past_the_initial_capacity) {
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");

  bool ok = (b.len == 64 * 16) && !b.oom && b.cap > 256 &&
            b.buf[b.len] == '\0' && b.buf[0] == '0';
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_c_and_append_cstr_accumulate_in_order) {
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "ab");
  ccol_growbuf_append_c(&b, 'c');
  ccol_growbuf_append(&b, "de", 2);

  bool ok = (b.len == 5) && (strcmp(b.buf, "abcde") == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_cstr_treats_null_as_an_empty_append) {
  /* The documentation says that this is tolerated, not undefined, so a caller
   * can pass on the buf field of another buffer, whose init or append can
   * fail and leave the field NULL. */
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "x");
  ccol_growbuf_append_cstr(&b, NULL);

  bool ok = (b.len == 1) && (strcmp(b.buf, "x") == 0) && !b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, a_failed_grow_latches_oom_and_every_later_append_is_a_no_op) {
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "kept");

  /* An append that still fits inside the first capacity succeeds before the
   * buffer tries to grow, so the length to compare against is the length at
   * the moment the latch trips, not the length before the loop. */
  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  bool latched = b.oom;
  size_t len_before = b.len;
  /* Once latched, the buffer stays latched: an append after the failure must
   * not start to write again, even when the allocator recovers. */
  g_growbuf_refuse_realloc = false;
  ccol_growbuf_append_cstr(&b, "ignored");
  bool still_latched = b.oom;
  size_t len_after = b.len;

  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(latched);
  REQUIRE_TRUE(still_latched);
  REQUIRE_EQ(len_after, len_before);
}

TEST(growbuf, destroy_on_an_oom_buffer_is_safe) {
  /* b.buf can correctly be NULL here, and destroy must tolerate that. */
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  g_growbuf_refuse_realloc = false;
  bool latched = b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(latched);
}

/* The size that an append needs is len + n + 1. These tests pin that the
 * buffer decides whether that size is representable before it computes the
 * size. Only the boundary is evidence of that, because a test with an
 * ordinary length passes whether the check is present or absent.
 *
 * These tests are not vacuous. Code that forms the sum first makes it wrap
 * to a value at or below the current capacity, so no growth happens and
 * the memcpy runs with the enormous n of the caller. Such code stops this
 * test under _FORTIFY_SOURCE and segfaults the next test, and
 * AddressSanitizer reports it as negative-size-param. */
TEST(growbuf, append_of_an_unrepresentable_length_latches_oom_without_copying) {
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  const char src[1] = {'A'};

  g_growbuf_alloc_calls = 0;
  ccol_growbuf_append(&b, src, SIZE_MAX);

  bool ok = b.oom && (b.len == 0) && (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_of_an_unrepresentable_length_counts_existing_content) {
  /* Once the buffer holds some content, len + n + 1 wraps for an n that is
   * much smaller than SIZE_MAX, so the check must use the space that is
   * left, not n alone. */
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "ab");
  const char src[1] = {'A'};

  g_growbuf_alloc_calls = 0;
  ccol_growbuf_append(&b, src, SIZE_MAX - 2);

  bool ok = b.oom && (b.len == 2) && (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_of_the_largest_representable_length_refuses_cleanly) {
  /* SIZE_MAX - 1 is one below the boundary: len + n + 1 is then exactly
   * SIZE_MAX and does not wrap. The representability check passes it, and the
   * doubling guard of the growth step refuses it. This test pins that standing
   * property rather than the representability check, and it passes either way;
   * it is here because the boundary has a meaning only when a test covers the
   * value on each side of it. It does pin one thing: the allocator never sees a
   * request of this size, because such a request stops an instrumented
   * allocator instead of giving NULL. */
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  const char src[1] = {'A'};

  g_growbuf_alloc_calls = 0;
  ccol_growbuf_append(&b, src, SIZE_MAX - 1);

  bool ok = b.oom && (b.len == 0) && (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, init_hint_of_an_unrepresentable_size_latches_oom) {
  /* The store that a hint asks for is hint + 1 bytes, which is not
   * representable at SIZE_MAX. Code that forms it wraps to 0, the 64-byte
   * floor then wins, and the code reports success on a buffer whose size is
   * nothing like the one that the caller asked for. */
  ccol_growbuf_t b;
  g_growbuf_alloc_calls = 0;
  ccol_growbuf_init_hint(&b, growbuf_procs(), SIZE_MAX);

  bool ok = b.oom && (b.cap == 0) && (b.buf == NULL) && (b.len == 0) &&
            (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_of_zero_bytes_leaves_the_buffer_terminated) {
  /* An append of zero bytes is complete and never touches the copy path, so
   * a caller can pass the NULL that the wrapper for a NUL-terminated string
   * already tolerates. That NULL never reaches memcpy, whose pointers must be
   * valid even for a length of zero.
   *
   * This test is not vacuous under UndefinedBehaviorSanitizer, which is
   * where it matters: code that lets the call through reports "null pointer
   * passed as argument 2, which is declared to never be null". A build with
   * no instrumentation passes either way. */
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append(&b, NULL, 0);
  ccol_growbuf_append_cstr(&b, "ok");
  ccol_growbuf_append(&b, NULL, 0);

  bool ok = !b.oom && (b.len == 2) && (strcmp(b.buf, "ok") == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

/* ========================================================================== */
/*                  ccol_retval_to_str / data type size / key dump */
/* ========================================================================== */

/* ccol_retval_t pins an explicit numeric value on every enumerator, and
 * call sites that test for ccol_success == 0 depend on that value directly.
 * The table below asserts the value and the spelling together. A new
 * enumerator anywhere except at the end quietly renumbers every later
 * enumerator; with this table, such a change fails here, while without it
 * the change shows up as an unrelated module reporting a retval that it
 * never returns. */
static const struct {
  ccol_retval_t value;
  int expected_number;
  const char *expected_name;
} k_retvals[] = {
    {ccol_success, 0, "ccol_success"},
    {ccol_not_enough_memory, -1, "ccol_not_enough_memory"},
    {ccol_key_already_present, -2, "ccol_key_already_present"},
    {ccol_key_not_found, -3, "ccol_key_not_found"},
    {ccol_invalid_args, -4, "ccol_invalid_args"},
    {ccol_not_permitted, -5, "ccol_not_permitted"},
    {ccol_timed_out, -6, "ccol_timed_out"},
    {ccol_container_full, -7, "ccol_container_full"},
    {ccol_container_empty, -8, "ccol_container_empty"},
    {ccol_msg_too_large, -9, "ccol_msg_too_large"},
    {ccol_http_transfer_aborted, -10, "ccol_http_transfer_aborted"},
    {ccol_http_invalid_url, -11, "ccol_http_invalid_url"},
    {ccol_http_too_many_redirects, -12, "ccol_http_too_many_redirects"},
    {ccol_http_tls_cert_load_failed, -13, "ccol_http_tls_cert_load_failed"},
    {ccol_http_tls_cert_verification_failed, -14,
     "ccol_http_tls_cert_verification_failed"},
    {ccol_http_tls_handshake_failed, -15, "ccol_http_tls_handshake_failed"},
    {ccol_http_host_resolution_failed, -16, "ccol_http_host_resolution_failed"},
    {ccol_http_connection_failed, -17, "ccol_http_connection_failed"},
    {ccol_unexpected_failure, -18, "ccol_unexpected_failure"},
};

TEST(retval, every_enumerator_keeps_its_pinned_numeric_value) {
  size_t count = sizeof(k_retvals) / sizeof(k_retvals[0]);
  bool ok = true;
  for (size_t i = 0; i < count; i++)
    if ((int)k_retvals[i].value != k_retvals[i].expected_number) ok = false;
  REQUIRE_TRUE(ok);
  REQUIRE_EQ((int)ccol_success, 0);
}

TEST(retval, every_enumerator_maps_to_its_own_spelling) {
  size_t count = sizeof(k_retvals) / sizeof(k_retvals[0]);
  bool ok = true;
  for (volatile size_t i = 0; i < count; i++) {
    const char *got = ccol_retval_to_str(k_retvals[i].value);
    if (strcmp(got, k_retvals[i].expected_name) != 0) ok = false;
  }
  REQUIRE_TRUE(ok);
}

TEST(retval, an_out_of_range_value_reports_unknown) {
  volatile ccol_retval_t bogus = (ccol_retval_t)12345;
  REQUIRE_STREQ(ccol_retval_to_str(bogus), "unknown");
}

TEST(data_type, fixed_width_sizes_match_the_underlying_types) {
  /* The test drives this through a volatile variable, so that the switch
   * runs instead of being folded into a constant at each call site. */
  volatile ccol_data_type t;
  t = ccol_char;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(char));
  t = ccol_signed_char;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(char));
  t = ccol_unsigned_char;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(char));
  t = ccol_short;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(short));
  t = ccol_unsigned_short;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(short));
  t = ccol_int;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(int));
  t = ccol_unsigned_int;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(int));
  t = ccol_long;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(long));
  t = ccol_unsigned_long;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(long));
  t = ccol_long_long;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(long long));
  t = ccol_unsigned_long_long;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(long long));
  t = ccol_float;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(float));
  t = ccol_double;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(double));
  t = ccol_long_double;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(long double));
  t = ccol_pointer;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), sizeof(uintptr_t));
}

TEST(data_type, variable_width_types_report_zero) {
  /* A string and an opaque blob have no width that a caller can assume. */
  volatile ccol_data_type t = ccol_string;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
  t = ccol_other_types;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
}

/* _ccol_dump_key_to_stderr runs inside the fatal-error path that chmap and
 * cbmap take before ccol_fatal_err(), so a fault in it turns a diagnostic
 * into a second crash on top of the first one. Capturing stderr is what
 * lets a test assert on it at all. */
static bool dump_key_capture(const void *data, size_t size, char *out,
                             size_t out_size) {
  char path[] = "/tmp/ccol_dumpkeyXXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) return false;

  fflush(stderr);
  int saved = dup(fileno(stderr));
  if (saved < 0) {
    close(fd);
    unlink(path);
    return false;
  }
  dup2(fd, fileno(stderr));
  _ccol_dump_key_to_stderr(data, size);
  fflush(stderr);
  dup2(saved, fileno(stderr));
  close(saved);

  lseek(fd, 0, SEEK_SET);
  ssize_t got = read(fd, out, out_size - 1);
  close(fd);
  unlink(path);
  if (got < 0) return false;
  out[got] = '\0';
  return true;
}

TEST(strdup, a_string_is_copied_into_storage_the_caller_owns) {
  /* The copy is independent of the source: a write over the source after the
   * call must not change the copy. */
  char src[16] = "hello";
  char *copy = ccol_strdup(NULL, src);
  memset(src, 'x', sizeof(src) - 1);
  src[sizeof(src) - 1] = '\0';

  bool ok = copy && strcmp(copy, "hello") == 0;
  free(copy);
  REQUIRE_TRUE(ok);
}

TEST(strdup, a_null_input_yields_null_rather_than_reading_through_it) {
  /* The documentation says that this is tolerated, not undefined, so a
   * caller can pass on a string that another allocation failed to make, and
   * check it once at the end.
   *
   * This test is not vacuous: without the guard, this call reaches
   * strlen(NULL), which AddressSanitizer reports as a SEGV on a null
   * dereference and an ordinary build turns into a crash. */
  REQUIRE_TRUE(ccol_strdup(NULL, NULL) == NULL);
}

TEST(dump_key, a_single_byte_uses_the_singular_noun) {
  unsigned char key = 0x41;
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(&key, 1, buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "Key dump (1 byte):"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "41"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|A|"), (void *)NULL);
}

TEST(dump_key, several_bytes_use_the_plural_noun) {
  unsigned char key[3] = {0x41, 0x42, 0x43};
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(key, sizeof(key), buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "Key dump (3 bytes):"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|ABC|"), (void *)NULL);
}

TEST(dump_key, exactly_one_full_row_emits_a_single_offset_line) {
  unsigned char key[16];
  for (size_t i = 0; i < sizeof(key); i++) key[i] = (unsigned char)('a' + i);
  char buf[1024];
  REQUIRE_TRUE(dump_key_capture(key, sizeof(key), buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "00000000"), (void *)NULL);
  /* A second row would start at offset 16. */
  REQUIRE_EQ((void *)strstr(buf, "00000010"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|abcdefghijklmnop|"), (void *)NULL);
}

TEST(dump_key, a_partial_second_row_is_padded_and_still_labelled) {
  unsigned char key[20];
  for (size_t i = 0; i < sizeof(key); i++) key[i] = (unsigned char)('a' + i);
  char buf[1024];
  REQUIRE_TRUE(dump_key_capture(key, sizeof(key), buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "00000000"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "00000010"), (void *)NULL);
  /* The short last row pads its hex columns to keep the sidebar in line. */
  REQUIRE_NE((void *)strstr(buf, "|qrst|"), (void *)NULL);
}

TEST(dump_key, unprintable_bytes_render_as_dots) {
  unsigned char key[4] = {0x00, 0x41, 0x1F, 0x7F};
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(key, sizeof(key), buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "|.A..|"), (void *)NULL);
}

/* The check on the order of the fork-prepare handlers (see
 * ccol_atfork_module_t). Every module that registers a prepare handler holds
 * its own locks from that handler until after the fork, so the locks of the
 * handlers nest in the order in which the handlers run, and one consistent
 * order across every fork is what keeps that nesting acyclic. The order is
 * a property of the registration order rather than of anything inside one
 * handler, which is why a check is better than a claim in a comment.
 *
 * This test runs in a forked child, because a violation is deliberately
 * fatal: a build that inverts the order must stop instead of continuing
 * into a nesting that no reader has reasoned about.
 *
 * This test is not vacuous: recording the same pair in one order and then
 * in the other order is what makes the child abort, and a build whose
 * recorder accepted both orders would exit 0 here. */
TEST(atfork_order, an_inverted_prepare_handler_order_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    _ccol_atfork_order_record(ccol_atfork_module_clogger);
    _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_reset();
    /* The opposite order, as a second fork makes it. */
    _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_record(ccol_atfork_module_clogger);
    _exit(0); /* reached only if the recorder took the inversion */
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* Recording the same pair in the same order across two forks is the
 * ordinary case, and the recorder must accept it, which is why the check
 * above cannot pass by refusing everything. */
TEST(atfork_order, a_repeated_consistent_order_is_accepted) {
  pid_t pid = fork();
  if (pid == 0) {
    for (int fork_round = 0; fork_round < 3; fork_round++) {
      _ccol_atfork_order_record(ccol_atfork_module_clogger);
      _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
      _ccol_atfork_order_record(ccol_atfork_module_cthreadpool);
      _ccol_atfork_order_reset();
    }
    _exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  /* This checks WIFEXITED only, and deliberately not WEXITSTATUS(status) ==
     0 as well. Under valgrind, --errors-for-leak-kinds=all reports the
     inherited process image of every forked child as still reachable, and
     --error-exitcode then replaces the status of the child, so the exit code
     says nothing about what the child did. WIFEXITED is the part that
     carries the meaning here: a recorder that rejected this consistent
     order would abort, which makes WIFEXITED false. */
  REQUIRE_TRUE(WIFEXITED(status));
}

/* A handler that joins the order between two forks, because the program
 * uses its module for the first time, runs first in the fork after that. Its
 * pairs with every later handler of that fork must be recorded, so that a
 * later fork in which it runs after one of them is refused.
 *
 * This test is non-vacuous: a recorder that starts a new sequence only when
 * a handler runs again, instead of at the explicit reset, records the
 * newcomer after the handlers of the previous fork, never records that it ran
 * before them, and lets the inverted third fork through with exit 0. */
TEST(atfork_order, a_handler_that_joins_later_is_ordered_against_the_rest) {
  pid_t pid = fork();
  if (pid == 0) {
    _ccol_atfork_order_record(ccol_atfork_module_clogger);
    _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_reset();
    /* cthreadpool joins and runs first. */
    _ccol_atfork_order_record(ccol_atfork_module_cthreadpool);
    _ccol_atfork_order_record(ccol_atfork_module_clogger);
    _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_reset();
    /* clogger before cthreadpool contradicts the fork above. */
    _ccol_atfork_order_record(ccol_atfork_module_clogger);
    _ccol_atfork_order_record(ccol_atfork_module_cthreadpool);
    _exit(0); /* reached only if the recorder took the inversion */
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* A real fork() ends the sequence through the handlers that the recorder
 * registers, with no call from the test. The child below records one order,
 * forks, and records the opposite order in the parent side of that fork,
 * which must abort.
 *
 * This test is non-vacuous: without the registered reset, the second
 * sequence continues the first one, both handlers are already in it, and
 * the process exits 0. */
TEST(atfork_order, a_real_fork_starts_a_new_sequence) {
  pid_t pid = fork();
  if (pid == 0) {
    _ccol_atfork_order_record(ccol_atfork_module_clogger);
    _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
    pid_t inner = fork();
    if (inner == 0) _exit(0);
    if (inner > 0) (void)waitpid(inner, NULL, 0);
    _ccol_atfork_order_record(ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_record(ccol_atfork_module_clogger);
    _exit(0); /* reached only if no reset ran at the fork */
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* ========================================================================== */
/*                       PUBLIC MACRO HYGIENE                                 */
/* ========================================================================== */

/* An allocator that always gives the same address, so that a test can
 * assert the exact pointer that an allocation macro makes instead of only
 * checking that the pointer is non-NULL. */
static char ccol_hyg_buffer[256];

static void *ccol_hyg_malloc(size_t size) {
  (void)size;
  return ccol_hyg_buffer;
}

static void *ccol_hyg_calloc(size_t count, size_t size) {
  memset(ccol_hyg_buffer, 0,
         (count * size) < sizeof(ccol_hyg_buffer) ? (count * size)
                                                  : sizeof(ccol_hyg_buffer));
  return ccol_hyg_buffer;
}

static void *ccol_hyg_realloc(void *ptr, size_t size) {
  (void)ptr;
  (void)size;
  return ccol_hyg_buffer;
}

static void ccol_hyg_free(void *ptr) { (void)ptr; }

TEST(macro_hygiene, an_allocation_macro_composes_with_a_surrounding_operator) {
  /* The conditional operator binds more loosely than a cast or an addition,
   * so an allocation macro whose body is not fully parenthesised lets an
   * operator outside it bind to one arm alone: the cast lands on the procs
   * pointer, and the offset attaches to the arm that the code does not take.
   * Both arms have the type void *, and -Wpointer-arith is in neither -Wall
   * nor -Wextra, so nothing reports it. This test is not vacuous: without the
   * parentheses the offset quietly disappears, and the pointer compares
   * equal to the base. */
  ccol_memmgmt_procs_t procs = {.malloc = ccol_hyg_malloc,
                                .calloc = ccol_hyg_calloc,
                                .realloc = ccol_hyg_realloc,
                                .free = ccol_hyg_free};
  ccol_memmgmt_procs_t *mp = &procs;

  char *offset_alloc = (char *)_ccol_mem_alloc(mp, 64) + 16;
  REQUIRE_EQ((void *)offset_alloc, (void *)(ccol_hyg_buffer + 16));

  char *offset_calloc = (char *)_ccol_mem_calloc(mp, 4, 8) + 8;
  REQUIRE_EQ((void *)offset_calloc, (void *)(ccol_hyg_buffer + 8));

  char *offset_realloc = (char *)_ccol_mem_realloc(mp, ccol_hyg_buffer, 32) + 4;
  REQUIRE_EQ((void *)offset_realloc, (void *)(ccol_hyg_buffer + 4));

  /* The free of this allocator does nothing, and it is called through the
   * procs directly: through _ccol_mem_free, Clang sees the default free()
   * arm of the macro applied to a static buffer and rejects the call with
   * -Wfree-nonheap-object, although that arm never runs here. */
  mp->free(ccol_hyg_buffer);
}

TEST(macro_hygiene, ccol_typed_cmp_does_not_capture_caller_identifiers) {
  /* Each temporary that this macro declares is in scope from the end of its
   * own declarator, so a temporary named like a realistic caller variable
   * captures the argument of that caller: the argument resolves to the
   * object that the macro has just declared. This test is not vacuous: with
   * temporaries named var1 and var2, the first comparison answers 0 instead
   * of -1, and no compiler reports it. */
  int var1 = 5, var2 = 7;
  int a = 5, b = 7;
  REQUIRE_EQ(ccol_typed_cmp(&var1, &var2, int), -1);
  REQUIRE_EQ(ccol_typed_cmp(&a, &b, int), -1);
  REQUIRE_EQ(ccol_typed_cmp(&var2, &var1, int), 1);
  REQUIRE_EQ(ccol_typed_cmp(&var1, &var1, int), 0);
}

TEST(macro_hygiene, type_introspection_does_not_capture_caller_identifiers) {
  /* The same mechanism, through the _Generic helpers that every container
   * creation macro uses. Without this check, a caller variable named like
   * one of their temporaries selects the default arm, which reports an
   * integral value as a value that is not integral. */
  int result = 42;
  int plain = 42;
  REQUIRE_TRUE(ccol_is_integral_type(result));
  REQUIRE_TRUE(ccol_is_integral_type(plain));
  REQUIRE_EQ(ccol_determine_ccol_data_type(result),
             ccol_determine_ccol_data_type(plain));
  char *_ptr = "hello";
  cmap_pair pair_from_ptr = {0};
  _populate_cmap_pair(&pair_from_ptr, _ptr);
  REQUIRE_EQ(pair_from_ptr.size, strlen("hello") + 1);
  REQUIRE_STREQ((const char *)pair_from_ptr.ptr, "hello");
}

/* ========================================================================== */
/*          CHARACTER POINTER VERSUS CHARACTER ARRAY CLASSIFICATION           */
/* ========================================================================== */

/* Pointers whose own object is const. The address of such an object has a
 * type like const char *const *, a shape that a classifier built on the
 * type of &(data) cannot recognize. */
static const char *const ccol_cls_const_name = "const-name";
static const signed char *const ccol_cls_const_signed =
    (const signed char *)"signed";
static const unsigned char *const ccol_cls_const_unsigned =
    (const unsigned char *)"unsigned";

TEST(char_classification, a_qualified_char_pointer_is_a_pointer_not_an_array) {
  char *const cp = "char-const";
  unsigned char *volatile vp = (unsigned char *)"volatile";
  signed char *const volatile cvp = (signed char *)"cv";
  REQUIRE_FALSE(ccol_is_char_array(ccol_cls_const_name));
  REQUIRE_FALSE(ccol_is_char_array(ccol_cls_const_signed));
  REQUIRE_FALSE(ccol_is_char_array(ccol_cls_const_unsigned));
  REQUIRE_FALSE(ccol_is_char_array(cp));
  REQUIRE_FALSE(ccol_is_char_array(vp));
  REQUIRE_FALSE(ccol_is_char_array(cvp));
  REQUIRE_TRUE(ccol_is_char_ptr(ccol_cls_const_name));
  REQUIRE_TRUE(ccol_is_char_ptr(cp));
  REQUIRE_EQ((int)ccol_determine_ccol_data_type(ccol_cls_const_name),
             (int)ccol_string);
  REQUIRE_STREQ((const char *)ccol_cls_const_signed, "signed");
}

TEST(char_classification, every_char_array_form_is_still_an_array) {
  char plain[8] = "plain";
  const char c_arr[6] = "const";
  signed char s_arr[4] = {'s', 'g', 'n', 0};
  const unsigned char u_arr[4] = {'u', 'n', 's', 0};
  REQUIRE_TRUE(ccol_is_char_array(plain));
  REQUIRE_TRUE(ccol_is_char_array(c_arr));
  REQUIRE_TRUE(ccol_is_char_array(s_arr));
  REQUIRE_TRUE(ccol_is_char_array(u_arr));
  REQUIRE_TRUE(ccol_is_char_array("literal"));
  char *p = plain;
  const char *cp = c_arr;
  REQUIRE_FALSE(ccol_is_char_array(p));
  REQUIRE_FALSE(ccol_is_char_array(cp));
  int not_a_string = 3;
  REQUIRE_FALSE(ccol_is_char_array(not_a_string));
}

TEST(char_classification, populate_pair_reads_the_string_of_a_const_pointer) {
  /* A pair built from a const-qualified pointer object must address the
   * string, not the pointer object itself; measured with strlen, the pointer
   * object gives a size that depends on its address bytes. */
  cmap_pair pair = {0};
  _populate_cmap_pair(&pair, ccol_cls_const_name);
  REQUIRE_EQ(pair.ptr, (void *)ccol_cls_const_name);
  REQUIRE_EQ(pair.size, strlen("const-name") + 1);

  char *const cp = "char-const";
  cmap_pair pair2 = {0};
  _populate_cmap_pair(&pair2, cp);
  REQUIRE_EQ(pair2.ptr, (void *)cp);
  REQUIRE_EQ(pair2.size, strlen("char-const") + 1);

  cmap_pair pair3 = {0};
  _populate_cmap_pair(&pair3, ccol_cls_const_unsigned);
  REQUIRE_EQ(pair3.ptr, (void *)ccol_cls_const_unsigned);
  REQUIRE_EQ(pair3.size, strlen("unsigned") + 1);

  char arr[16] = "array";
  cmap_pair pair4 = {0};
  _populate_cmap_pair(&pair4, arr);
  REQUIRE_EQ(pair4.ptr, (void *)arr);
  REQUIRE_EQ(pair4.size, strlen("array") + 1);
}

/* ------------------------------------------------------------------------ */
/* ccol_fatal_err                                                             */
/* ------------------------------------------------------------------------ */

#include <signal.h>

/* Runs ccol_fatal_err() in a forked child whose stderr is a pipe and gives
 * back what the child wrote, and whether it died of SIGABRT. */
static bool fatal_err_capture(int which, char *out, size_t out_size) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    if (which == 0) {
      ccol_fatal_err("a message with no argument");
    } else {
      ccol_fatal_err("n=%d s=%s", 42, "text");
    }
    _exit(0);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return false;
  }
  size_t used = 0;
  ssize_t n;
  while (used + 1 < out_size &&
         (n = read(fds[0], out + used, out_size - 1 - used)) > 0) {
    used += (size_t)n;
  }
  out[used] = '\0';
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

/* The macro appends an empty string for a trailing %s so that it never needs
 * an empty variadic list. The text that reaches stderr is exactly the format
 * of the caller, with no argument and with arguments. */
TEST(fatal_err, prints_the_message_of_the_caller_and_aborts) {
  char text0[512], text1[512];
  bool aborted0 = fatal_err_capture(0, text0, sizeof(text0));
  bool aborted1 = fatal_err_capture(1, text1, sizeof(text1));
  REQUIRE_TRUE(aborted0);
  REQUIRE_TRUE(aborted1);
  REQUIRE_TRUE(strstr(text0, ": fatal: a message with no argument\n") != NULL);
  REQUIRE_TRUE(strstr(text1, ": fatal: n=42 s=text\n") != NULL);
  REQUIRE_TRUE(strstr(text0, "tests.c:") == text0);
}

#if defined(_CCOL_CLOEXEC_GATE)
/* ------------------------------------------------------------------------
 * The close-on-exec gate: where a descriptor takes two steps to become
 * closed on exec, a fork() of another thread waits for both steps.
 * ------------------------------------------------------------------------ */
#include <arpa/inet.h>
#include <fcntl.h>
#include <internal/csock.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>

extern atomic_ulong _ccol_cloexec_gate_enters_for_tests;
extern atomic_ulong _ccol_cloexec_gate_leaves_for_tests;

typedef struct {
  _Atomic int forked;
  pid_t pid;
} gate_fork_ctx;

static void *gate_forker(void *arg) {
  gate_fork_ctx *c = (gate_fork_ctx *)arg;
  pid_t pid = fork();
  if (pid == 0) _exit(0);
  c->pid = pid;
  atomic_store(&c->forked, 1);
  return NULL;
}

/* A fork() cannot return while a thread is inside the gate, because the
 * fork-prepare handler of the gate waits for the read side, so the check
 * after the wait can never see a fork that returned while the gate was
 * held, however slow the machine is.
 *
 * This test is non-vacuous: without the fork-prepare handler of the gate,
 * the fork returns at once and forked_inside is 1. */
TEST(cloexec_gate, fork_waits_while_a_descriptor_is_inside_the_gate) {
  gate_fork_ctx c;
  atomic_init(&c.forked, 0);
  c.pid = -1;
  _ccol_cloexec_gate_enter();
  pthread_t t;
  bool started = pthread_create(&t, NULL, gate_forker, &c) == 0;
  struct timespec wait = {.tv_sec = 0, .tv_nsec = 200 * 1000 * 1000};
  nanosleep(&wait, NULL);
  int forked_inside = atomic_load(&c.forked);
  _ccol_cloexec_gate_leave();
  if (started) pthread_join(t, NULL);
  int status = 0;
  bool reaped = c.pid > 0 && waitpid(c.pid, &status, 0) == c.pid;

  REQUIRE_TRUE(started);
  REQUIRE_EQ(forked_inside, 0);
  REQUIRE_EQ(atomic_load(&c.forked), 1);
  REQUIRE_TRUE(reaped);
  REQUIRE_TRUE(WIFEXITED(status));
}

/* Gives true when fd is closed on exec and, if nonblock, non-blocking. */
static bool gate_fd_flags_ok(int fd, bool nonblock) {
  int fd_flags = fcntl(fd, F_GETFD);
  int fl_flags = fcntl(fd, F_GETFL);
  return fd_flags >= 0 && (fd_flags & FD_CLOEXEC) && fl_flags >= 0 &&
         (!nonblock || (fl_flags & O_NONBLOCK));
}

/* The counts of entries and exits of the gate since a mark. */
typedef struct {
  unsigned long enters;
  unsigned long leaves;
} gate_mark;

static gate_mark gate_mark_now(void) {
  gate_mark m = {atomic_load(&_ccol_cloexec_gate_enters_for_tests),
                 atomic_load(&_ccol_cloexec_gate_leaves_for_tests)};
  return m;
}

static bool gate_used_once(gate_mark before) {
  gate_mark after = gate_mark_now();
  return after.enters - before.enters == 1 && after.leaves - before.leaves == 1;
}

/* Every call that creates a descriptor in two steps enters the gate once and
 * leaves it once, on success and on failure, and gives a descriptor that is
 * closed on exec.
 *
 * This test is non-vacuous: a creation site that skips the gate leaves both
 * counts unchanged, and a failure path that skips the exit leaves the
 * entries one ahead. */
TEST(cloexec_gate, every_two_step_creation_uses_the_gate) {
#if defined(_CCOL_EMULATE_DARWIN_SOCK)
  gate_mark m = gate_mark_now();
  int s = ccol_socket_nb(AF_INET, SOCK_STREAM, 0);
  bool socket_gated = gate_used_once(m);
  bool socket_flags = s >= 0 && gate_fd_flags_ok(s, true);

  m = gate_mark_now();
  int bad = ccol_socket_nb(-1, SOCK_STREAM, 0);
  bool bad_socket_gated = gate_used_once(m) && bad < 0;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(addr);
  int cli = -1, acc = -1;
  bool listening = s >= 0 && bind(s, (struct sockaddr *)&addr, alen) == 0 &&
                   listen(s, 1) == 0 &&
                   getsockname(s, (struct sockaddr *)&addr, &alen) == 0;
  if (listening) {
    cli = socket(AF_INET, SOCK_STREAM, 0);
    if (cli >= 0) (void)connect(cli, (struct sockaddr *)&addr, alen);
  }
  struct pollfd pfd = {.fd = s, .events = POLLIN, .revents = 0};
  bool ready = listening && cli >= 0 && poll(&pfd, 1, 5000) == 1;
  m = gate_mark_now();
  if (ready) acc = ccol_accept_nb(s, NULL, NULL);
  bool accept_gated = ready && gate_used_once(m);
  bool accept_flags = acc >= 0 && gate_fd_flags_ok(acc, true);

  m = gate_mark_now();
  int none = ccol_accept_nb(s, NULL, NULL);
  bool empty_accept_gated = s >= 0 && gate_used_once(m) && none < 0;

  int sv[2] = {-1, -1};
  m = gate_mark_now();
  int pr = ccol_socketpair_nb(AF_UNIX, SOCK_STREAM, 0, sv);
  bool pair_gated = gate_used_once(m);
  bool pair_flags =
      pr == 0 && gate_fd_flags_ok(sv[0], true) && gate_fd_flags_ok(sv[1], true);

  m = gate_mark_now();
  int bad_sv[2];
  int bad_pair = ccol_socketpair_nb(-1, SOCK_STREAM, 0, bad_sv);
  bool bad_pair_gated = gate_used_once(m) && bad_pair != 0;

  if (sv[0] >= 0) close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
  if (acc >= 0) close(acc);
  if (cli >= 0) close(cli);
  if (s >= 0) close(s);

  REQUIRE_TRUE(socket_gated);
  REQUIRE_TRUE(socket_flags);
  REQUIRE_TRUE(bad_socket_gated);
  REQUIRE_TRUE(listening);
  REQUIRE_TRUE(accept_gated);
  REQUIRE_TRUE(accept_flags);
  REQUIRE_TRUE(empty_accept_gated);
  REQUIRE_TRUE(pair_gated);
  REQUIRE_TRUE(pair_flags);
  REQUIRE_TRUE(bad_pair_gated);
#endif
#if defined(_CCOL_EMULATE_DARWIN_SYNC)
  ccol_semaphore_t sem;
  gate_mark sm = gate_mark_now();
  bool sem_made = ccol_semaphore_init(sem, 1) == 0;
  bool sem_gated = gate_used_once(sm);
  bool sem_flags = sem_made && gate_fd_flags_ok(sem.fd[0], false) &&
                   gate_fd_flags_ok(sem.fd[1], false);
  if (sem_made) ccol_semaphore_destroy(sem);
  REQUIRE_TRUE(sem_made);
  REQUIRE_TRUE(sem_gated);
  REQUIRE_TRUE(sem_flags);
#endif
}
/* A fork-prepare handler that the library registers before its own
 * constructor runs. The constructor below has priority 101, so on the ELF
 * systems it runs before every constructor of the default priority, the one of
 * the gate in common.c included. macOS does not order constructors by
 * priority, so there it is an ordinary constructor. */
extern bool _ccol_cloexec_gate_prepared_for_tests;
static _Atomic int gate_early_handler_saw_gate = -1;

static void gate_early_prepare(void) {
  atomic_store(&gate_early_handler_saw_gate,
               _ccol_cloexec_gate_prepared_for_tests ? 1 : 0);
}

#if defined(__APPLE__)
__attribute__((constructor))
#else
__attribute__((constructor(101)))
#endif
static void
gate_register_early_handler(void) {
  (void)ccol_at_fork(gate_early_prepare, NULL, NULL);
}

/* A handler that ccol_at_fork registers runs before the prepare handler of
 * the gate, even when it registers before the constructor of the gate runs.
 * In the other order a fork() can wait for a lock whose holder waits to enter
 * the gate.
 *
 * This test is non-vacuous on the ELF systems: when ccol_at_fork does not
 * register the gate first, the gate registers after this handler, its prepare
 * handler runs first, and the handler sees it. */
TEST(cloexec_gate, a_handler_registered_before_the_library_runs_before_gate) {
  atomic_store(&gate_early_handler_saw_gate, -1);
  pid_t pid = fork();
  if (pid == 0) _exit(0);
  int status = 0;
  bool reaped = pid > 0 && waitpid(pid, &status, 0) == pid;
  REQUIRE_TRUE(reaped);
  REQUIRE_EQ(atomic_load(&gate_early_handler_saw_gate), 0);
}
#endif /* _CCOL_CLOEXEC_GATE */
