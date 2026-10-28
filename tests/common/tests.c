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

#include <citerators.h>
#include <common.h>
#include <internal/cgrowbuf.h>
#include <internal/cpow2.h>
#include <internal/cprocsintern.h>
#include <internal/cstrutil.h>
#include <internal/test_helpers/common.h>
#include <internal/test_helpers/cprocsintern.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>
TAU_MAIN()  // sets Tau up, and gives the main function

/* The forked children of this binary report through their wait status, and
 * test_reset_inherited_signals() explains what an inherited signal state does
 * to that. */
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

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

/* One shared instance that a function hands out a pointer to, so the caller
 * never takes the address of a local or a global directly. A literal
 * &some_local_var at the macro call site triggers -Waddress ("the address of
 * X will always evaluate as true"), and going through this function keeps
 * _ccol_mem_alloc, _ccol_mem_free and ccol_scoped_ptr_mp calls quiet. */
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
  /* The default allocator has no counting hook, so `make memtest` is what
   * confirms there is no leak and no double free. This test is only here to
   * check that the macro form without _mp compiles and runs
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
    /* Assigning a new value to buf without freeing the old one first is the
     * macro's documented warning, and the macro doesn't solve it for you. */
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

/* chashmap sizes every table it allocates with this function. */
TEST(power_of_two, exact_powers_return_themselves) {
  for (volatile unsigned bit = 0; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(v), v);
  }
}

TEST(power_of_two, values_round_up_to_the_next_power) {
  /* One value below and one above each power exercise both directions of the
   * binary search, plus its early exit for "the entry before this one is
   * smaller, so this is the answer". */
  for (volatile unsigned bit = 2; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(v - 1), v);
    REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(v + 1), v << 1);
  }
}

TEST(power_of_two, small_inputs_clamp_to_one) {
  /* Any value at or below the first table entry gives that entry. */
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(0), (size_t)1);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(1), (size_t)1);
}

TEST(power_of_two, above_the_representable_maximum_is_invalid) {
  /* The table ends at the largest power of two a size_t can hold, so a value
   * above it has no answer. */
  size_t top = ((size_t)1) << (sizeof(size_t) * 8 - 1);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(top), top);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(top + 1), ccol_invalid_size);
  REQUIRE_EQ(_ccol_find_nearest_gte_power_of_two(SIZE_MAX), ccol_invalid_size);
}

/* ========================================================================== */
/*                         ccol_growbuf_t */
/* ========================================================================== */

/* A reallocator that refuses once the test arms it, so we can drive the
 * buffer's out-of-memory latch on purpose instead of waiting for a real
 * allocation failure. */
static bool g_growbuf_refuse_realloc = false;
/* Counts how many times one operation goes to the allocator, so a test can
 * assert that a refused request never reached it. Asserting on the count
 * rather than on the outcome tells apart "the code rejected the size before
 * the call" and "the allocator got an absurd size and said no". Only the
 * first one is safe, because an instrumented allocator stops the process on
 * a request that plain malloc just refuses. */
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
  /* The documentation says this is tolerated and not undefined, so a caller
   * can pass on the buf field of another buffer whose init or append failed
   * and left it NULL. */
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

  /* An append that still fits in the first capacity succeeds before the
   * buffer tries to grow, so the length to compare against is the one at the
   * moment the latch trips and not the one before the loop. */
  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  bool latched = b.oom;
  size_t len_before = b.len;
  /* Once latched the buffer stays latched, so an append after the failure
   * must not start writing again even if the allocator recovers. */
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
  /* b.buf can legitimately be NULL here and destroy has to tolerate it. */
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  g_growbuf_refuse_realloc = false;
  bool latched = b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(latched);
}

/* The size an append needs is len + n + 1, and these tests pin that the
 * buffer checks whether that size is representable before computing it.
 * Only the boundary can show that, since a test with an ordinary length
 * passes whether the check is there or not.
 *
 * Both tests catch the bug. Code that forms the sum first makes it wrap to
 * a value at or below the current capacity, so nothing grows and memcpy
 * runs with the caller's enormous n. Under _FORTIFY_SOURCE that stops this
 * test and segfaults the next one, and AddressSanitizer reports it as
 * negative-size-param. */
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
  /* Once the buffer holds some content, len + n + 1 wraps for an n much
   * smaller than SIZE_MAX, so the check must use the space left and not n
   * alone. */
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
  /* SIZE_MAX - 1 sits one below the boundary, where len + n + 1 is exactly
   * SIZE_MAX and doesn't wrap. The representability check lets it through
   * and the growth step's doubling guard refuses it. So this test pins that
   * guard and not the representability check, and it passes either way; it's
   * here because a boundary only means something when a test covers the
   * value on each side of it. It does pin one thing: the allocator never gets
   * a request of this size, since such a request stops an instrumented
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
  /* A hint asks for a store of hint + 1 bytes, which isn't representable at
   * SIZE_MAX. Code that forms it wraps to 0, the 64-byte floor wins, and the
   * code reports success on a buffer whose size has nothing to do with what
   * the caller asked for. */
  ccol_growbuf_t b;
  g_growbuf_alloc_calls = 0;
  ccol_growbuf_init_hint(&b, growbuf_procs(), SIZE_MAX);

  bool ok = b.oom && (b.cap == 0) && (b.buf == NULL) && (b.len == 0) &&
            (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_of_zero_bytes_leaves_the_buffer_terminated) {
  /* An append of zero bytes is already complete and never goes near the copy
   * path, so the caller can pass the NULL that the wrapper for a
   * NUL-terminated string tolerates. That NULL never reaches memcpy, whose
   * pointers must be valid even for a length of zero.
   *
   * Under UndefinedBehaviorSanitizer, which is where it matters, the test
   * catches the defect: code that lets the call through reports "null pointer
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

/* ccol_retval_t gives every enumerator an explicit numeric value, and call
 * sites that test for ccol_success == 0 depend on that value directly. The
 * table below asserts the value and the spelling together. A new enumerator
 * anywhere but at the end silently renumbers every enumerator after it, and
 * with this table such a change fails here, while without it the change
 * would show up as some unrelated module reporting a retval it never
 * returns. */
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
  /* We go through a volatile variable so the switch really runs instead of
   * being folded into a constant at each call site. */
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
  /* A string and an opaque blob have no width a caller can assume. */
  volatile ccol_data_type t = ccol_string;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
  t = ccol_other_types;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
}

/* _ccol_dump_key_to_stderr runs on the fatal-error path that chmap and cbmap
 * take before _ccol_fatal_err(), so a fault in it would turn a diagnostic
 * into a second crash on top of the first one. We capture stderr so a test
 * can assert on it at all. */
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
  /* The copy doesn't depend on the source, so writing over the source after
   * the call must leave the copy as it was. */
  char src[16] = "hello";
  char *copy = ccol_strdup(NULL, src);
  memset(src, 'x', sizeof(src) - 1);
  src[sizeof(src) - 1] = '\0';

  bool ok = copy && strcmp(copy, "hello") == 0;
  free(copy);
  REQUIRE_TRUE(ok);
}

TEST(strdup, a_null_input_yields_null_rather_than_reading_through_it) {
  /* The documentation says this is tolerated and not undefined, so a caller
   * can pass on a string that another allocation failed to make and check
   * it once at the end.
   *
   * Without the guard the call reaches strlen(NULL), which AddressSanitizer
   * reports as a SEGV on a null dereference and an ordinary build turns into
   * a crash. */
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
  /* The short last row pads its hex columns so the sidebar lines up. */
  REQUIRE_NE((void *)strstr(buf, "|qrst|"), (void *)NULL);
}

TEST(dump_key, unprintable_bytes_render_as_dots) {
  unsigned char key[4] = {0x00, 0x41, 0x1F, 0x7F};
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(key, sizeof(key), buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "|.A..|"), (void *)NULL);
}

/* The check on the order of the fork-prepare handlers (see
 * _ccol_atfork_module_t). Every module that registers a prepare handler holds
 * its own locks from that handler until after the fork, so the handlers'
 * locks nest in the order the handlers run, and keeping that order the same
 * across every fork is what keeps the nesting acyclic. The order comes from
 * the registration order and not from anything inside one handler, so a
 * check is worth more than a claim in a comment.
 *
 * The test runs in a forked child because a violation is fatal on purpose:
 * a build that inverts the order must stop rather than go on into a
 * nesting nobody has reasoned about.
 *
 * Recording the same pair in one order and then in the other is what makes
 * the child abort, and a build whose recorder accepted both orders would
 * exit 0 here. */
TEST(atfork_order, an_inverted_prepare_handler_order_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    _ccol_atfork_order_record(_ccol_atfork_module_clogger);
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_reset();
    /* The opposite order, the way a second fork would make it. */
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_record(_ccol_atfork_module_clogger);
    test_child_exit(0); /* reached only if the recorder took the inversion */
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* Recording the same pair in the same order across two forks is the
 * ordinary case and the recorder accepts it, which is why the check
 * above can't pass by just refusing everything. */
TEST(atfork_order, a_repeated_consistent_order_is_accepted) {
  pid_t pid = fork();
  if (pid == 0) {
    for (int fork_round = 0; fork_round < 3; fork_round++) {
      _ccol_atfork_order_record(_ccol_atfork_module_clogger);
      _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
      _ccol_atfork_order_record(_ccol_atfork_module_cthreadpool);
      _ccol_atfork_order_reset();
    }
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  /* We only check WIFEXITED here and not WEXITSTATUS(status) == 0 too.
     Under valgrind, --errors-for-leak-kinds=all reports the inherited process
     image of every forked child as still reachable and --error-exitcode then
     overrides the child's status, so the exit code tells us nothing about
     what the child did. WIFEXITED is the part that carries meaning: a
     recorder that rejected this consistent order would abort, and WIFEXITED
     would be false. */
  REQUIRE_TRUE(WIFEXITED(status));
}

/* When the program uses a module for the first time between two forks, its
 * handler joins the order and runs first in the next fork. Its pairs with
 * every later handler of that fork need to be recorded, so that a later fork
 * where it runs after one of them gets refused.
 *
 * The test isn't vacuous: a recorder that starts a new sequence only when a
 * handler runs again, instead of at the explicit reset, records the newcomer
 * after the previous fork's handlers, never records that it ran before them,
 * and lets the inverted third fork through with exit 0. */
TEST(atfork_order, a_handler_that_joins_later_is_ordered_against_the_rest) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    _ccol_atfork_order_record(_ccol_atfork_module_clogger);
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_reset();
    /* cthreadpool joins and runs first. */
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadpool);
    _ccol_atfork_order_record(_ccol_atfork_module_clogger);
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_reset();
    /* clogger before cthreadpool contradicts the fork above. */
    _ccol_atfork_order_record(_ccol_atfork_module_clogger);
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadpool);
    test_child_exit(0); /* reached only if the recorder took the inversion */
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* A real fork() ends the sequence through the handlers the recorder
 * registers, without any call from the test. The child below records one
 * order, forks, and records the opposite order on the parent side of that
 * fork, which must abort.
 *
 * Without the registered reset the second sequence would continue the
 * first one, both handlers would already be in it, and the process would
 * exit 0. */
TEST(atfork_order, a_real_fork_starts_a_new_sequence) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    _ccol_atfork_order_record(_ccol_atfork_module_clogger);
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
    pid_t inner = fork();
    if (inner == 0) test_child_exit(0);
    if (inner > 0) (void)waitpid(inner, NULL, 0);
    _ccol_atfork_order_record(_ccol_atfork_module_cthreadcomm);
    _ccol_atfork_order_record(_ccol_atfork_module_clogger);
    test_child_exit(0); /* reached only if no reset ran at the fork */
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

/* An allocator that always hands out the same address, so a test can assert
 * the exact pointer an allocation macro produces instead of only checking
 * that it isn't NULL. */
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
   * so if an allocation macro's body isn't fully parenthesised, an operator
   * outside it binds to one arm only: the cast lands on the procs pointer and
   * the offset sticks to the arm that isn't taken. Both arms are void * and
   * -Wpointer-arith is in neither -Wall nor -Wextra, so nothing reports it.
   * Drop the parentheses and the offset silently disappears, so the pointer
   * compares equal to the base. */
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

  /* This allocator's free does nothing, and we call it through the procs
   * directly, because through _ccol_mem_free Clang sees the macro's default
   * free() arm applied to a static buffer and rejects the call with
   * -Wfree-nonheap-object, even though that arm never runs here. */
  mp->free(ccol_hyg_buffer);
}

TEST(macro_hygiene, ccol_typed_cmp_does_not_capture_caller_identifiers) {
  /* Every temporary this macro declares is in scope from the end of its own
   * declarator, so a temporary named like a realistic caller variable
   * captures that caller's argument: the argument resolves to the object the
   * macro just declared. This test notices: with temporaries named var1
   * and var2 the first comparison gives 0 instead of -1, and no compiler
   * says a word. */
  int var1 = 5, var2 = 7;
  int a = 5, b = 7;
  REQUIRE_EQ(_ccol_typed_cmp(&var1, &var2, int), -1);
  REQUIRE_EQ(_ccol_typed_cmp(&a, &b, int), -1);
  REQUIRE_EQ(_ccol_typed_cmp(&var2, &var1, int), 1);
  REQUIRE_EQ(_ccol_typed_cmp(&var1, &var1, int), 0);
}

TEST(macro_hygiene, type_introspection_does_not_capture_caller_identifiers) {
  /* Same mechanism, through the _Generic helpers that every container
   * creation macro uses. Without this check a caller variable named like one
   * of their temporaries picks the default arm, which reports an integral
   * value as not integral. */
  int result = 42;
  int plain = 42;
  REQUIRE_TRUE(_ccol_is_integral_type(result));
  REQUIRE_TRUE(_ccol_is_integral_type(plain));
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

/* Pointers whose own object is const. Taking the address of such an object
 * gives a type like const char *const *, which a classifier built on the
 * type of &(data) can't recognize. */
static const char *const ccol_cls_const_name = "const-name";
static const signed char *const ccol_cls_const_signed =
    (const signed char *)"signed";
static const unsigned char *const ccol_cls_const_unsigned =
    (const unsigned char *)"unsigned";

TEST(char_classification, a_qualified_char_pointer_is_a_pointer_not_an_array) {
  char *const cp = "char-const";
  unsigned char *volatile vp = (unsigned char *)"volatile";
  signed char *const volatile cvp = (signed char *)"cv";
  REQUIRE_FALSE(_ccol_is_char_array(ccol_cls_const_name));
  REQUIRE_FALSE(_ccol_is_char_array(ccol_cls_const_signed));
  REQUIRE_FALSE(_ccol_is_char_array(ccol_cls_const_unsigned));
  REQUIRE_FALSE(_ccol_is_char_array(cp));
  REQUIRE_FALSE(_ccol_is_char_array(vp));
  REQUIRE_FALSE(_ccol_is_char_array(cvp));
  REQUIRE_TRUE(_ccol_is_char_ptr(ccol_cls_const_name));
  REQUIRE_TRUE(_ccol_is_char_ptr(cp));
  REQUIRE_EQ((int)ccol_determine_ccol_data_type(ccol_cls_const_name),
             (int)ccol_string);
  REQUIRE_STREQ((const char *)ccol_cls_const_signed, "signed");
}

TEST(char_classification, every_char_array_form_is_still_an_array) {
  char plain[8] = "plain";
  const char c_arr[6] = "const";
  signed char s_arr[4] = {'s', 'g', 'n', 0};
  const unsigned char u_arr[4] = {'u', 'n', 's', 0};
  REQUIRE_TRUE(_ccol_is_char_array(plain));
  REQUIRE_TRUE(_ccol_is_char_array(c_arr));
  REQUIRE_TRUE(_ccol_is_char_array(s_arr));
  REQUIRE_TRUE(_ccol_is_char_array(u_arr));
  REQUIRE_TRUE(_ccol_is_char_array("literal"));
  char *p = plain;
  const char *cp = c_arr;
  REQUIRE_FALSE(_ccol_is_char_array(p));
  REQUIRE_FALSE(_ccol_is_char_array(cp));
  int not_a_string = 3;
  REQUIRE_FALSE(_ccol_is_char_array(not_a_string));
}

TEST(char_classification, populate_pair_reads_the_string_of_a_const_pointer) {
  /* A pair built from a const-qualified pointer object should address the
   * string and not the pointer object, because measured with strlen the
   * pointer object gives a size that depends on its address bytes. */
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

  unsigned char *volatile vp = (unsigned char *)"volatile";
  cmap_pair pair5 = {0};
  _populate_cmap_pair(&pair5, vp);
  REQUIRE_EQ(pair5.ptr, (void *)vp);
  REQUIRE_EQ(pair5.size, strlen("volatile") + 1);

  const char *null_str = NULL;
  cmap_pair pair6 = {(void *)arr, 99};
  _populate_cmap_pair(&pair6, null_str);
  REQUIRE_EQ(pair6.ptr, (void *)NULL);
  REQUIRE_EQ(pair6.size, (size_t)0);
}

/* Two levels, like every public value macro has, where the outer one passes
 * its argument on to an implementation macro. */
#define CCOL_TEST_POP_SIZE(x, k, p) CCOL_TEST_POP_SIZE_IMPL((x), k, p)
#define CCOL_TEST_POP_SIZE_IMPL(x, k, p) \
  ({                                     \
    __typeof__(x) k = (x);               \
    cmap_pair p = {0};                   \
    _populate_cmap_pair(&p, k);          \
    p.size;                              \
  })

TEST(char_classification, populate_pair_nests_inside_another_macro_argument) {
  /* _populate_cmap_pair() has no diagnostic pragma in it, so a macro that
   * uses it can take another macro that uses it as its argument, which is
   * the shape of chmap_get_ptr(m, chmap_get(m, k)). It also guards this:
   * with a _Pragma in _populate_cmap_pair(), GCC 10 moves the pragma in front
   * of the outer macro, into the middle of this declaration, and fails with
   * "expected expression before '#pragma'". */
  const char *const s = "nested";
  size_t size = CCOL_TEST_POP_SIZE(
      CCOL_TEST_POP_SIZE(s, inner_k, inner_p) ? s : s, outer_k, outer_p);
  REQUIRE_EQ(size, strlen("nested") + 1);
}

#undef CCOL_TEST_POP_SIZE_IMPL
#undef CCOL_TEST_POP_SIZE

/* A struct with padding between its members. */
struct ccol_unqual_probe {
  char c;
  int i;
};

TEST(unqual_type, drops_top_level_qualifiers_and_keeps_array_types) {
  /* Each declaration below is a compile-time check that a temporary of the
   * unqualified type is assignable and that an array type keeps its size, on
   * every supported compiler including GCC 10. */
  const struct ccol_unqual_probe cs = {1, 2};
  _ccol_unqual_type(cs) s;
  s = cs;
  _Static_assert(__builtin_types_compatible_p(_ccol_unqual_type(cs),
                                              struct ccol_unqual_probe),
                 "a const struct gives the struct");

  const char *const cp = "text";
  _ccol_unqual_type(cp) p;
  p = cp;
  _Static_assert(
      __builtin_types_compatible_p(_ccol_unqual_type(cp), const char *),
      "a const pointer to const char gives a pointer to const char");

  const volatile _Bool cvb = 1;
  _ccol_unqual_type(cvb) b;
  b = cvb;
  _Static_assert(sizeof(b) == sizeof(_Bool), "a bool stays a bool");

  void (*const fp)(void) = NULL;
  _ccol_unqual_type(fp) f;
  f = fp;

  const char carr[5] = "abcd";
  char arr[16] = "array";
  _Static_assert(sizeof(_ccol_unqual_type(carr)) == 5,
                 "an array type keeps its size");
  _Static_assert(sizeof(_ccol_unqual_type(arr)) == 16,
                 "an array type keeps its size");

  REQUIRE_EQ(s.c, 1);
  REQUIRE_EQ(s.i, 2);
  REQUIRE_STREQ(p, "text");
  REQUIRE_TRUE(b);
  REQUIRE_TRUE(f == NULL);
  REQUIRE_STREQ(carr, "abcd");
  REQUIRE_STREQ(arr, "array");
}

/* ------------------------------------------------------------------------ */
/* _ccol_fatal_err */
/* ------------------------------------------------------------------------ */

#include <signal.h>

/* Runs _ccol_fatal_err() in a forked child whose stderr is a pipe, and gives
 * back what the child wrote and whether it died of SIGABRT. */
static bool fatal_err_capture(int which, char *out, size_t out_size) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    if (which == 0) {
      _ccol_fatal_err("a message with no argument");
    } else {
      _ccol_fatal_err("n=%d s=%s", 42, "text");
    }
    test_child_exit(0);
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

/* The macro appends an empty string for a trailing %s, so it never needs an
 * empty variadic list. What reaches stderr is exactly the caller's format,
 * with and without arguments. */
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

typedef struct {
  _Atomic int forked;
  pid_t pid;
} gate_fork_ctx;

static void *gate_forker(void *arg) {
  gate_fork_ctx *c = (gate_fork_ctx *)arg;
  pid_t pid = fork();
  if (pid == 0) test_child_exit(0);
  c->pid = pid;
  atomic_store(&c->forked, 1);
  return NULL;
}

/* A fork() can't return while a thread is inside the gate, because the
 * gate's fork-prepare handler waits for the read side. The test holds the
 * gate until the forking thread has started that handler, so the check
 * looks at a fork that really waits at the gate, however slowly the forking
 * thread gets there.
 *
 * Without the gate's fork-prepare handler the counter never moves and
 * reached_prepare stays false. With a handler that doesn't wait, the fork
 * returns during the pause and forked_inside is 1. */
TEST(cloexec_gate, fork_waits_while_a_descriptor_is_inside_the_gate) {
  gate_fork_ctx c;
  atomic_init(&c.forked, 0);
  c.pid = -1;
  _ccol_cloexec_gate_enter();
  unsigned long prepares = atomic_load(&_ccol_cloexec_gate_prepares_for_tests);
  pthread_t t;
  bool started = pthread_create(&t, NULL, gate_forker, &c) == 0;
  /* The bound is only a hang guard; the forking thread gets to the handler
   * in microseconds. */
  bool reached_prepare = false;
  for (int i = 0; started && i < 20000 && !reached_prepare; i++) {
    reached_prepare =
        atomic_load(&_ccol_cloexec_gate_prepares_for_tests) != prepares;
    if (!reached_prepare) {
      struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
      nanosleep(&ms, NULL);
    }
  }
  /* A correct gate keeps the fork where it is however long this lasts. The
   * pause only gives a handler that doesn't wait the time to let its fork
   * return, so the check below can see it. */
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
  nanosleep(&pause, NULL);
  int forked_inside = atomic_load(&c.forked);
  _ccol_cloexec_gate_leave();
  if (started) pthread_join(t, NULL);
  int status = 0;
  bool reaped = c.pid > 0 && waitpid(c.pid, &status, 0) == c.pid;

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(reached_prepare);
  REQUIRE_EQ(forked_inside, 0);
  REQUIRE_EQ(atomic_load(&c.forked), 1);
  REQUIRE_TRUE(reaped);
  REQUIRE_TRUE(WIFEXITED(status));
}

/* True when fd is closed on exec and, if nonblock is set, non-blocking. */
static bool gate_fd_flags_ok(int fd, bool nonblock) {
  int fd_flags = fcntl(fd, F_GETFD);
  int fl_flags = fcntl(fd, F_GETFL);
  return fd_flags >= 0 && (fd_flags & FD_CLOEXEC) && fl_flags >= 0 &&
         (!nonblock || (fl_flags & O_NONBLOCK));
}

/* How many times the gate was entered and left since a mark. */
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
 * leaves it once, on success and on failure alike, and gives a descriptor
 * that is closed on exec.
 *
 * A creation site that skips the gate leaves both counts unchanged, and a
 * failure path that skips the exit leaves the entries one ahead. */
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
    if (cli >= 0) (void)test_connect(cli, (struct sockaddr *)&addr, alen);
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
  _ccol_semaphore_t sem;
  gate_mark sm = gate_mark_now();
  bool sem_made = _ccol_semaphore_init(sem, 1) == 0;
  bool sem_gated = gate_used_once(sm);
  bool sem_flags = sem_made && gate_fd_flags_ok(sem.fd[0], false) &&
                   gate_fd_flags_ok(sem.fd[1], true);
  if (sem_made) _ccol_semaphore_destroy(sem);
  REQUIRE_TRUE(sem_made);
  REQUIRE_TRUE(sem_gated);
  REQUIRE_TRUE(sem_flags);
#endif
}
/* A fork-prepare handler that the library registers before its own
 * constructor runs. The constructor below has priority 101, so on the ELF
 * systems it runs before every default-priority constructor, including the
 * gate's one in common.c. macOS doesn't order constructors by priority, so
 * there it's just an ordinary constructor. */
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
  (void)_ccol_at_fork(gate_early_prepare, NULL, NULL);
}

/* A handler that _ccol_at_fork registers runs before the gate's prepare
 * handler even when it registers before the gate's constructor runs. In the
 * other order a fork() could wait for a lock whose holder is waiting to enter
 * the gate.
 *
 * On the ELF systems this catches it: if _ccol_at_fork didn't
 * register the gate first, the gate would register after this handler, its
 * prepare handler would run first, and the handler would see it. */
TEST(cloexec_gate, a_handler_registered_before_the_library_runs_before_gate) {
  atomic_store(&gate_early_handler_saw_gate, -1);
  pid_t pid = fork();
  if (pid == 0) test_child_exit(0);
  int status = 0;
  bool reaped = pid > 0 && waitpid(pid, &status, 0) == pid;
  REQUIRE_TRUE(reaped);
  REQUIRE_EQ(atomic_load(&gate_early_handler_saw_gate), 0);
}
#endif /* _CCOL_CLOEXEC_GATE */

/* ------------------------------------------------------------------------ */
/* fork() in the child of a threaded process                                */
/* ------------------------------------------------------------------------ */

#if defined(__OpenBSD__)
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>

static atomic_bool fork_libc_stop;

/* Calls libc functions that take libc's internal locks until told to stop. */
static void *fork_libc_spin(void *arg) {
  (void)arg;
  time_t now = time(NULL);
  struct tm tm;
  while (!atomic_load(&fork_libc_stop)) {
    localtime_r(&now, &tm);
    (void)getpwnam("root");
  }
  return NULL;
}

/* OpenBSD's libc only takes its internal locks while the process is threaded
 * and it marks a fork child as not threaded, so a child can use a function
 * whose lock another parent thread held at the fork. The library loads with
 * fork handlers registered and must leave that mark alone. Here a thread
 * loops on getpwnam() and localtime_r() while the main thread forks children
 * that call both and exit, and every child is expected to exit.
 *
 * A fork handler that marks the child as threaded makes children wait for
 * ever on a lock in getpwnam() within the first few dozen forks. */
TEST(fork, a_child_of_a_threaded_process_can_call_libc_functions_that_lock) {
  atomic_store(&fork_libc_stop, false);
  pthread_t t;
  bool started = pthread_create(&t, NULL, fork_libc_spin, NULL) == 0;
  int stuck = 0, forked = 0;
  for (int i = 0; started && i < 200; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      time_t now = time(NULL);
      struct tm tm;
      localtime_r(&now, &tm);
      (void)getpwnam("root");
      test_child_exit(0);
    }
    if (pid < 0) break;
    forked++;
    int status = 0;
    pid_t w = 0;
    for (int k = 0; k < 500 && (w = waitpid(pid, &status, WNOHANG)) == 0; k++)
      usleep(10000);
    if (w == 0) {
      stuck++;
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
  }
  atomic_store(&fork_libc_stop, true);
  if (started) pthread_join(t, NULL);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(forked, 200);
  REQUIRE_EQ(stuck, 0);
}
#endif

/* ========================================================================== */
/*                              ITERATORS                                     */
/* ========================================================================== */

static cmap_iterator *iter_probe_seen;
static cmap_iterator *iter_probe_its[2];
static int iter_probe_picks;

/* Hands out the next iterator of iter_probe_its and counts the call. */
static cmap_iterator *iter_probe_pick(void) {
  return iter_probe_its[iter_probe_picks++];
}

static cmap_iterator *iter_probe_next(cmap_iterator *it) {
  iter_probe_seen = it;
  return NULL;
}

/* ccol_iter_next() evaluates its argument exactly once, like every public
 * macro. The argument here is a call with a side effect, so a macro that
 * names it twice makes two calls and hands the callback the wrong iterator.
 * With the expansion ((it)->_next_fn(it)) it fails, because that makes two
 * picks and hands the callback the second iterator. */
TEST(iterators, iter_next_evaluates_its_argument_once) {
  static cmap_iterator a = {.key_pair = NULL,
                            .val_pair = NULL,
                            ._next_fn = iter_probe_next,
                            ._free_fn = NULL,
                            ._direct_ptr = false};
  static cmap_iterator b;
  b = a;
  iter_probe_its[0] = &a;
  iter_probe_its[1] = &b;
  iter_probe_picks = 0;
  iter_probe_seen = NULL;
  cmap_iterator *next = ccol_iter_next(iter_probe_pick());
  REQUIRE_EQ(iter_probe_picks, 1);
  REQUIRE_EQ((void *)iter_probe_seen, (void *)&a);
  REQUIRE_EQ((void *)next, NULL);
}

/* ========================================================================== */
/*                              SEMAPHORE                                     */
/* ========================================================================== */

/* The checks that the child of semaphore.posts_without_a_waiter_never_block
 * runs, each failure with its own exit status. */
static int semaphore_post_child(void) {
  _ccol_semaphore_t sem;
  if (_ccol_semaphore_init(sem, 0) != 0) return 10;
  for (int i = 0; i < 100000; i++) {
    if (_ccol_semaphore_post(sem) != 0) return 11;
  }
  /* A count the posts left behind still wakes a waiter. */
  if (_ccol_semaphore_wait(sem) != 0) return 12;
#if defined(_CCOL_EMULATE_DARWIN_SYNC)
  /* Every failed wait sets errno. Once the write end is gone the pipe ends
   * after the bytes it holds, and a wait on an empty, ended pipe
   * fails. */
  int flags = fcntl(sem.fd[0], F_GETFL);
  if (flags < 0 || fcntl(sem.fd[0], F_SETFL, flags | O_NONBLOCK) != 0)
    return 13;
  char drain[4096];
  while (read(sem.fd[0], drain, sizeof(drain)) > 0) {
  }
  if (fcntl(sem.fd[0], F_SETFL, flags) != 0) return 14;
  close(sem.fd[1]);
  sem.fd[1] = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (sem.fd[1] < 0) return 15;
  errno = 0;
  if (_ccol_semaphore_wait(sem) != -1 || errno != EINVAL) return 16;
#endif
  _ccol_semaphore_destroy(sem);
  return 0;
}

/* A post never waits, just like sem_post(3), however many posts no waiter
 * has consumed. chttpsvr_engine_stop() posts from a signal handler, where a
 * blocked post would hang the process. The child makes 100000 posts with no
 * waiter, far more than a pipe holds, and must report within the bound.
 * Under _CCOL_EMULATE_DARWIN_SYNC it catches both mistakes: with a write end
 * that blocks, the child stops in its first post past the pipe's capacity
 * and the bound runs out, and with a wait that fails without setting errno
 * the child reports 16. */
TEST(semaphore, posts_without_a_waiter_never_block) {
  /* The child reports through a pipe because valgrind overrides a forked
   * child's exit status. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    unsigned char code = (unsigned char)semaphore_post_child();
    ssize_t wr;
    do {
      wr = write(pfd[1], &code, 1);
    } while (wr < 0 && errno == EINTR);
    test_child_exit(0);
  }
  close(pfd[1]);
  int code = -1;
  if (pid > 0) {
    struct pollfd p = {.fd = pfd[0], .events = POLLIN};
    int pr;
    do {
      pr = poll(&p, 1, 30000);
    } while (pr < 0 && errno == EINTR);
    unsigned char got = 0;
    ssize_t r = -1;
    if (pr > 0) {
      do {
        r = read(pfd[0], &got, 1);
      } while (r < 0 && errno == EINTR);
    }
    if (r == 1)
      code = got;
    else
      kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
  }
  close(pfd[0]);
  REQUIRE_GT(pid, 0);
  REQUIRE_EQ(code, 0);
}

/* ========================================================================== */
/*                         ALLOCATOR PROCS INTERN TABLE                       */
/* ========================================================================== */

#define INTERN_RACE_THREADS 16

typedef struct {
  ccol_memmgmt_procs_t *procs;
  atomic_int *arrived;
  atomic_bool *go;
  ccol_memmgmt_procs_t *got;
  ccol_retval_t rv;
} intern_race_arg;

static void intern_race_pause(void) {
  struct timespec ts = {0, 100000L};
  nanosleep(&ts, NULL);
}

static void *intern_race_worker(void *p) {
  intern_race_arg *a = p;
  atomic_fetch_add(a->arrived, 1);
  while (!atomic_load(a->go)) intern_race_pause();
  /* Every thread interns its own struct, so equal pointers come from equal
   * contents and not from one shared address. */
  ccol_memmgmt_procs_t mine = *a->procs;
  a->rv = ccol_procs_intern(&mine, &a->got);
  return NULL;
}

/* Many threads that use one content for the first time at the same moment
 * all get the same pointer, and the content takes exactly one slot. The
 * claim hook holds the first claimer between its claim and its copy until
 * every other thread has met that slot while it is being written, so the
 * race happens however late the other threads start. A lookup that skips a
 * slot being written fails it, since it hands the threads several pointers
 * and takes a slot for each of them. */
TEST(procs_intern, a_burst_of_first_uses_of_one_content_takes_one_slot) {
  /* Function pointers nothing ever calls, different from every content that
   * another test interns. */
  ccol_memmgmt_procs_t content = {(ccol_malloc_t)(uintptr_t)0x7a11000,
                                  (ccol_free_t)(uintptr_t)0x7a11010,
                                  (ccol_calloc_t)(uintptr_t)0x7a11020,
                                  (ccol_realloc_t)(uintptr_t)0x7a11030};
  size_t before = _ccol_procs_intern_used_for_tests();
  atomic_int arrived = 0;
  atomic_bool go = false;
  intern_race_arg args[INTERN_RACE_THREADS];
  pthread_t tids[INTERN_RACE_THREADS];
  int created = 0;
  for (int i = 0; i < INTERN_RACE_THREADS; i++) {
    args[i] = (intern_race_arg){&content, &arrived, &go, NULL, ccol_success};
    if (pthread_create(&tids[i], NULL, intern_race_worker, &args[i]) != 0)
      break;
    created++;
  }
  while (atomic_load(&arrived) < created) intern_race_pause();
  unsigned sightings_before =
      atomic_load(&_ccol_procs_intern_writing_sightings_for_tests);
  /* The 30 s is only the bound of the claimer's wait. With fewer than two
   * threads nobody can see the slot being written, the wait could only run
   * out, and the test fails on the thread count below anyway. */
  if (created >= 2) {
    atomic_store(&_ccol_procs_intern_claim_wait_sightings_for_tests,
                 sightings_before + (unsigned)(created - 1));
    atomic_store(&_ccol_procs_intern_claim_delay_us_for_tests, 30000000u);
  }
  atomic_store(&go, true);
  for (int i = 0; i < created; i++) pthread_join(tids[i], NULL);
  atomic_store(&_ccol_procs_intern_claim_delay_us_for_tests, 0u);
  atomic_store(&_ccol_procs_intern_claim_wait_sightings_for_tests, 0u);
  unsigned sightings =
      atomic_load(&_ccol_procs_intern_writing_sightings_for_tests) -
      sightings_before;
  size_t after = _ccol_procs_intern_used_for_tests();

  int failed = 0, distinct = 0;
  for (int i = 0; i < created; i++) {
    if (args[i].rv != ccol_success || args[i].got == NULL) failed++;
    if (args[i].got != args[0].got) distinct++;
  }
  bool same_content = args[0].got != NULL &&
                      args[0].got->malloc == content.malloc &&
                      args[0].got->free == content.free &&
                      args[0].got->calloc == content.calloc &&
                      args[0].got->realloc == content.realloc;
  _ccol_procs_intern_truncate_for_tests(before);

  REQUIRE_EQ(created, INTERN_RACE_THREADS);
  REQUIRE_EQ(failed, 0);
  REQUIRE_EQ(distinct, 0);
  REQUIRE_TRUE(same_content);
  REQUIRE_EQ(after, before + 1);
  /* Every other thread met the slot while it was being written, so the race
   * the test is about took place. */
  REQUIRE_GE(sightings, (unsigned)(created - 1));
}

/* A burst of first uses of many different contents fills one slot per
 * content, so the table holds CCOL_PROCS_INTERN_CAPACITY of them. */
TEST(procs_intern,
     a_burst_of_first_uses_of_distinct_contents_fills_one_slot_each) {
  size_t before = _ccol_procs_intern_used_for_tests();
  ccol_memmgmt_procs_t contents[INTERN_RACE_THREADS];
  for (int i = 0; i < INTERN_RACE_THREADS; i++) {
    /* Two threads share each content, so a content races with itself and
     * with the others. */
    uintptr_t base = 0x7b22000 + (uintptr_t)(i / 2) * 0x100;
    contents[i] = (ccol_memmgmt_procs_t){
        (ccol_malloc_t)base, (ccol_free_t)(base + 0x10),
        (ccol_calloc_t)(base + 0x20), (ccol_realloc_t)(base + 0x30)};
  }
  atomic_int arrived = 0;
  atomic_bool go = false;
  intern_race_arg args[INTERN_RACE_THREADS];
  pthread_t tids[INTERN_RACE_THREADS];
  int created = 0;
  for (int i = 0; i < INTERN_RACE_THREADS; i++) {
    args[i] =
        (intern_race_arg){&contents[i], &arrived, &go, NULL, ccol_success};
    if (pthread_create(&tids[i], NULL, intern_race_worker, &args[i]) != 0)
      break;
    created++;
  }
  while (atomic_load(&arrived) < created) intern_race_pause();
  atomic_store(&_ccol_procs_intern_claim_delay_us_for_tests, 2000u);
  atomic_store(&go, true);
  for (int i = 0; i < created; i++) pthread_join(tids[i], NULL);
  atomic_store(&_ccol_procs_intern_claim_delay_us_for_tests, 0u);
  size_t after = _ccol_procs_intern_used_for_tests();

  int wrong = 0;
  for (int i = 0; i < created; i++) {
    const ccol_memmgmt_procs_t *g = args[i].got;
    if (args[i].rv != ccol_success || g == NULL ||
        g->malloc != contents[i].malloc || g->free != contents[i].free ||
        g->calloc != contents[i].calloc || g->realloc != contents[i].realloc)
      wrong++;
    /* The partner thread of the same content got the same pointer. */
    if ((i & 1) && g != args[i - 1].got) wrong++;
  }
  _ccol_procs_intern_truncate_for_tests(before);

  REQUIRE_EQ(created, INTERN_RACE_THREADS);
  REQUIRE_EQ(wrong, 0);
  REQUIRE_EQ(after, before + INTERN_RACE_THREADS / 2);
}

/* ---- A timed wait with a deadline at the end of time ------------------- */

#include <internal/cdeadline.h>

typedef struct {
  _ccol_mutex_t m;
  _ccol_cond_var_t c;
  bool done;
} far_wait_ctx;

static void *far_wait_signaller(void *arg) {
  far_wait_ctx *x = (far_wait_ctx *)arg;
  struct timespec ts = {0, 100000000L};
  nanosleep(&ts, NULL);
  _ccol_mutex_lock(x->m);
  x->done = true;
  _ccol_cond_var_signal(x->c);
  _ccol_mutex_unlock(x->m);
  return NULL;
}

/* A timed wait on a CLOCK_MONOTONIC condition variable whose deadline is at
 * the end of time (which is what every "no limit" timeout of the library
 * gives) blocks until it's signalled, at every width of time_t. For a build
 * with _CCOL_EMULATE_DARWIN_SYNC and a 32-bit time_t this test matters:
 * if the emulated wait added its remaining time to the realtime clock
 * without saturating, the sum would wrap into the past, every wait would
 * return ETIMEDOUT at once, and the loop below would wake thousands of times
 * instead of once or twice. */
TEST(cond_var, a_wait_until_the_end_of_time_blocks) {
  far_wait_ctx x = {.done = false};
  _ccol_cond_var_attr_t a;
  REQUIRE_EQ(_ccol_mutex_init(x.m), 0);
  REQUIRE_EQ(_ccol_cond_var_attr_init(a), 0);
  REQUIRE_EQ(_ccol_cond_var_attr_setclock(a, CLOCK_MONOTONIC), 0);
  REQUIRE_EQ(_ccol_cond_var_init_ca(x.c, a), 0);
  (void)_ccol_cond_var_attr_destroy(a);
  struct timespec deadline;
  ccol_deadline_after_us(UINT64_MAX, &deadline);
  pthread_t t;
  int created = pthread_create(&t, NULL, far_wait_signaller, &x);
  int wakes = 0;
  _ccol_mutex_lock(x.m);
  while (created == 0 && !x.done && wakes < 100000) {
    (void)_ccol_cond_var_timedwait(x.c, x.m, deadline);
    wakes++;
  }
  _ccol_mutex_unlock(x.m);
  if (created == 0) pthread_join(t, NULL);
  _ccol_cond_var_destroy(x.c);
  _ccol_mutex_destroy(x.m);
  REQUIRE_EQ(created, 0);
  REQUIRE_LT(wakes, 10);
}

#define _CCOL_C_LOCALE_COUNT_USERS_FOR_TESTS 1
#include <internal/cnumlocale.h>
#include <stdio.h>

typedef struct {
  _Atomic int stage; /* 1 in the scope, 2 may leave, 3 left */
  char text[32];
} _cnl_user_t;

static void *_cnl_user_thread(void *arg) {
  _cnl_user_t *u = (_cnl_user_t *)arg;
  ccol_c_locale_scope_t scope = ccol_c_locale_enter();
  atomic_store(&u->stage, 1);
  /* The main thread sets stage 2 on every path, so the bound is only a hang
   * guard, and a generous one, because the thread must stay in its scope
   * until the destructor has run. */
  for (int i = 0; i < 60000 && atomic_load(&u->stage) != 2; i++) usleep(1000);
  snprintf(u->text, sizeof(u->text), "%.1f", 1.5);
  ccol_c_locale_leave(scope);
  atomic_store(&u->stage, 3);
  return NULL;
}

/*
 * The destructor of the "C" locale object keeps the object while another
 * thread is inside a scope, so that thread carries on with a live locale,
 * and an enter after the destructor switches nothing. A destructor that
 * frees the object whatever the users count says would fail here: it
 * keeps nothing, so the thread formats under a freed locale, and valgrind
 * reports it where the C library allocates the object.
 */
TEST(c_locale, the_destructor_keeps_an_object_that_a_scope_still_uses) {
  _cnl_user_t u = {.stage = 0};
  pthread_t t;
  bool started = pthread_create(&t, NULL, _cnl_user_thread, &u) == 0;
  for (int i = 0; started && i < 60000 && atomic_load(&u.stage) != 1; i++)
    usleep(1000);
  bool in_scope = started && atomic_load(&u.stage) == 1;
  _ccol_c_locale_fini();
  bool kept = _ccol_c_locale_kept != (locale_t)0;
  ccol_c_locale_scope_t late = ccol_c_locale_enter();
  bool late_switched = late.switched;
  ccol_c_locale_leave(late);
  atomic_store(&u.stage, 2);
  if (started) pthread_join(t, NULL);
  size_t users = atomic_load(&_ccol_c_locale_users);
  if (_ccol_c_locale_kept) {
    freelocale(_ccol_c_locale_kept);
    _ccol_c_locale_kept = (locale_t)0;
  }
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(in_scope);
  REQUIRE_TRUE(kept);
  REQUIRE_FALSE(late_switched);
  REQUIRE_EQ(users, (size_t)0);
  REQUIRE_STREQ(u.text, "1.5");
}
