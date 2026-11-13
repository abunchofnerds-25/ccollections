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
TAU_MAIN()

// Resets the signal state that the binary inherited, before any test runs. The
// forked children of this binary report through their wait status, which an
// inherited ignored SIGCHLD or blocked signal would break. Changes the signal
// dispositions and the mask of the process.
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

// A counting allocator: The counts let a test see exactly how many allocations
// and frees went through a procs struct.
static size_t g_malloc_count = 0;
static size_t g_free_count = 0;

// The four functions of the counting allocator, which forward to the C library.
// The scoped_ptr tests use them to count what a scope frees. malloc increments
// g_malloc_count and a non-NULL free increments g_free_count.
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

// Zeroes g_malloc_count and g_free_count, so each test counts only its own
// calls.
static void reset_counters(void) {
  g_malloc_count = 0;
  g_free_count = 0;
}

// The counting procs, handed out through counting_procs(). A literal &variable
// at a macro call site triggers -Waddress ("will always evaluate as true");
// going through a function keeps ccol__mem_alloc, ccol__mem_free and
// ccol_scoped_ptr_mp quiet.
static ccol_memmgmt_procs g_counting_procs_storage = {
    .malloc = _counting_malloc,
    .calloc = _counting_calloc,
    .realloc = _counting_realloc,
    .free = _counting_free};

// Gives a pointer to g_counting_procs_storage, so a macro call site never takes
// the address of a variable directly (see the storage above).
static ccol_memmgmt_procs *counting_procs(void) {
  return &g_counting_procs_storage;
}

// The default allocator has no counting hook, so `make memtest` is what shows
// that there's no leak and no double free. Here we only check that the form
// without _mp compiles and runs.
TEST(scoped_ptr, default_allocator_frees_on_scope_exit) {
  {
    ccol_scoped_ptr(buf, char);
    buf = malloc(16);
    REQUIRE_NE((void *)buf, NULL);
    strcpy(buf, "hello");
  }
}

TEST(scoped_ptr, custom_allocator_frees_via_provided_procs) {
  reset_counters();
  ccol_memmgmt_procs *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = ccol__mem_alloc(mp, sizeof(int) * 4);
    REQUIRE_NE((void *)buf, NULL);
    REQUIRE_EQ(g_malloc_count, (size_t)1);
    REQUIRE_EQ(g_free_count, (size_t)0);
    buf[0] = 42;
  }
  REQUIRE_EQ(g_free_count, (size_t)1);
}

TEST(scoped_ptr, unused_pointer_is_a_no_op) {
  reset_counters();
  ccol_memmgmt_procs *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    (void)buf;
  }
  REQUIRE_EQ(g_free_count, (size_t)0);
}

// Reassigning buf without freeing the old value is the documented trap of the
// macro, so the test frees it by hand before the second allocation.
TEST(scoped_ptr, only_final_value_is_freed_on_reassignment) {
  reset_counters();
  ccol_memmgmt_procs *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = ccol__mem_alloc(mp, sizeof(int));
    REQUIRE_NE((void *)buf, NULL);
    ccol__mem_free(mp, buf);
    buf = ccol__mem_alloc(mp, sizeof(int) * 2);
    REQUIRE_NE((void *)buf, NULL);
  }
  REQUIRE_EQ(g_malloc_count, (size_t)2);
  REQUIRE_EQ(g_free_count, (size_t)2);
}

TEST(scoped_ptr, release_prevents_auto_free_and_transfers_ownership) {
  reset_counters();
  ccol_memmgmt_procs *mp = counting_procs();
  int *released = NULL;
  {
    ccol_scoped_ptr_mp(buf, int, mp);
    buf = ccol__mem_alloc(mp, sizeof(int));
    REQUIRE_NE((void *)buf, NULL);
    *buf = 7;
    released = ccol_scoped_ptr_release(buf);
    REQUIRE_EQ((void *)buf, NULL);
  }
  REQUIRE_EQ(g_free_count, (size_t)0);
  REQUIRE_NE((void *)released, NULL);
  REQUIRE_EQ(*released, 7);
  ccol__mem_free(mp, released);
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
  ccol_memmgmt_procs *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(a, int, mp);
    ccol_scoped_ptr_mp(b, char, mp);
    a = ccol__mem_alloc(mp, sizeof(int));
    b = ccol__mem_alloc(mp, 32);
    REQUIRE_NE((void *)a, NULL);
    REQUIRE_NE((void *)b, NULL);
  }
  REQUIRE_EQ(g_malloc_count, (size_t)2);
  REQUIRE_EQ(g_free_count, (size_t)2);
}

TEST(scoped_ptr, nested_scope_frees_at_inner_block_exit) {
  reset_counters();
  ccol_memmgmt_procs *mp = counting_procs();
  {
    ccol_scoped_ptr_mp(outer, int, mp);
    outer = ccol__mem_alloc(mp, sizeof(int));
    {
      ccol_scoped_ptr_mp(inner, int, mp);
      inner = ccol__mem_alloc(mp, sizeof(int));
      REQUIRE_EQ(g_free_count, (size_t)0);
    }
    REQUIRE_EQ(g_free_count, (size_t)1);
    REQUIRE_NE((void *)outer, NULL);
  }
  REQUIRE_EQ(g_free_count, (size_t)2);
}

// Allocates a buffer through a scoped pointer and returns early when
// should_fail is set. It lets the early_return test count that every path out
// frees the buffer. Gives false on the early return or a failed allocation,
// true otherwise. Changes the counts of the counting allocator.
static bool process_may_fail(bool should_fail) {
  ccol_memmgmt_procs *mp = counting_procs();
  ccol_scoped_ptr_mp(buf, int, mp);
  buf = ccol__mem_alloc(mp, sizeof(int) * 8);
  if (!buf) return false;
  if (should_fail) return false;
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

// chashmap sizes every table it allocates with
// ccol__find_nearest_gte_power_of_two. A value one below and one above each
// power exercise both directions of its search.
TEST(power_of_two, exact_powers_return_themselves) {
  for (volatile unsigned bit = 0; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(v), v);
  }
}

TEST(power_of_two, values_round_up_to_the_next_power) {
  for (volatile unsigned bit = 2; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(v - 1), v);
    REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(v + 1), v << 1);
  }
}

TEST(power_of_two, small_inputs_clamp_to_one) {
  REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(0), (size_t)1);
  REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(1), (size_t)1);
}

// The table ends at the largest power of two a size_t can hold, so a value
// above it has no answer.
TEST(power_of_two, above_the_representable_maximum_is_invalid) {
  size_t top = ((size_t)1) << (sizeof(size_t) * 8 - 1);
  REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(top), top);
  REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(top + 1), ccol_invalid_size);
  REQUIRE_EQ(ccol__find_nearest_gte_power_of_two(SIZE_MAX), ccol_invalid_size);
}

// When set, the growbuf reallocator refuses, so a test can drive the
// out-of-memory latch of the buffer on purpose.
static bool g_growbuf_refuse_realloc = false;
// Counts the calls that reach the growbuf allocator, so a test can assert that
// a refused request never got there. An instrumented allocator stops the
// process on an absurd size that plain malloc only refuses, so the size must be
// rejected before the call.
static size_t g_growbuf_alloc_calls = 0;
// The four functions of the growbuf allocator, which forward to the C library.
// The growbuf tests use them to see whether a request reached the allocator and
// to make a growth fail. Every allocating call increments
// g_growbuf_alloc_calls, and realloc gives NULL while g_growbuf_refuse_realloc
// is set.
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
// The growbuf procs and their accessor, for the same -Waddress reason as
// counting_procs().
static ccol_memmgmt_procs g_growbuf_procs_storage = {
    .malloc = _growbuf_malloc,
    .calloc = _growbuf_calloc,
    .realloc = _growbuf_realloc,
    .free = _growbuf_free};
// Gives a pointer to g_growbuf_procs_storage, for the same -Waddress reason as
// counting_procs().
static ccol_memmgmt_procs *growbuf_procs(void) {
  return &g_growbuf_procs_storage;
}

TEST(growbuf, init_starts_empty_and_nul_terminated) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  bool ok =
      (b.len == 0) && (b.cap >= 256) && !b.oom && b.buf && b.buf[0] == '\0';
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, init_hint_below_the_floor_still_allocates_the_floor) {
  ccol_growbuf b;
  ccol_growbuf_init_hint(&b, growbuf_procs(), 4);
  bool ok = (b.cap >= 64) && !b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, init_hint_above_the_floor_honours_the_hint) {
  ccol_growbuf b;
  ccol_growbuf_init_hint(&b, growbuf_procs(), 4096);
  bool ok = (b.cap >= 4097) && !b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_grows_past_the_initial_capacity) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");

  bool ok = (b.len == 64 * 16) && !b.oom && b.cap > 256 &&
            b.buf[b.len] == '\0' && b.buf[0] == '0';
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

TEST(growbuf, append_c_and_append_cstr_accumulate_in_order) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "ab");
  ccol_growbuf_append_c(&b, 'c');
  ccol_growbuf_append(&b, "de", 2);

  bool ok = (b.len == 5) && (strcmp(b.buf, "abcde") == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

// A NULL string is documented as tolerated, so a caller can pass on the buf
// field of a buffer whose init or append failed.
TEST(growbuf, append_cstr_treats_null_as_an_empty_append) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "x");
  ccol_growbuf_append_cstr(&b, NULL);

  bool ok = (b.len == 1) && (strcmp(b.buf, "x") == 0) && !b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

// Appends that fit the first capacity succeed before the growth fails, so the
// length is compared from the moment the latch trips. Once latched, an append
// must not write again even after the allocator recovers.
TEST(growbuf, a_failed_grow_latches_oom_and_every_later_append_is_a_no_op) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "kept");

  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  bool latched = b.oom;
  size_t len_before = b.len;
  g_growbuf_refuse_realloc = false;
  ccol_growbuf_append_cstr(&b, "ignored");
  bool still_latched = b.oom;
  size_t len_after = b.len;

  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(latched);
  REQUIRE_TRUE(still_latched);
  REQUIRE_EQ(len_after, len_before);
}

// b.buf can legitimately be NULL here, and destroy must tolerate it.
TEST(growbuf, destroy_on_an_oom_buffer_is_safe) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  g_growbuf_refuse_realloc = false;
  bool latched = b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(latched);
}

// An append needs len + n + 1 bytes, and the buffer must check that this size
// is representable before it computes it. Only the boundary shows that. Code
// that forms the sum first wraps it to a value at or below the capacity,
// nothing grows and memcpy runs with the huge n: _FORTIFY_SOURCE stops the test
// and AddressSanitizer reports negative-size-param.
TEST(growbuf, append_of_an_unrepresentable_length_latches_oom_without_copying) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  const char src[1] = {'A'};

  g_growbuf_alloc_calls = 0;
  ccol_growbuf_append(&b, src, SIZE_MAX);

  bool ok = b.oom && (b.len == 0) && (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

// With content in the buffer, len + n + 1 wraps for an n well below SIZE_MAX,
// so the check must use the space left and not n alone.
TEST(growbuf, append_of_an_unrepresentable_length_counts_existing_content) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append_cstr(&b, "ab");
  const char src[1] = {'A'};

  g_growbuf_alloc_calls = 0;
  ccol_growbuf_append(&b, src, SIZE_MAX - 2);

  bool ok = b.oom && (b.len == 2) && (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

// SIZE_MAX - 1 sits one below the boundary: len + n + 1 is exactly SIZE_MAX and
// doesn't wrap, so the doubling guard of the growth refuses it. It covers the
// other side of the boundary and pins that the allocator never sees a request
// of this size.
TEST(growbuf, append_of_the_largest_representable_length_refuses_cleanly) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  const char src[1] = {'A'};

  g_growbuf_alloc_calls = 0;
  ccol_growbuf_append(&b, src, SIZE_MAX - 1);

  bool ok = b.oom && (b.len == 0) && (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

// A hint asks for hint + 1 bytes, which isn't representable at SIZE_MAX. Code
// that forms it wraps to 0, the 64-byte floor wins, and init reports success on
// a buffer unrelated to the request.
TEST(growbuf, init_hint_of_an_unrepresentable_size_latches_oom) {
  ccol_growbuf b;
  g_growbuf_alloc_calls = 0;
  ccol_growbuf_init_hint(&b, growbuf_procs(), SIZE_MAX);

  bool ok = b.oom && (b.cap == 0) && (b.buf == NULL) && (b.len == 0) &&
            (g_growbuf_alloc_calls == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

// A zero-byte append must never reach memcpy, whose pointers must be valid even
// for a length of 0, so a caller can pass NULL. Under
// UndefinedBehaviorSanitizer the defect reports "null pointer passed as
// argument 2"; a plain build passes either way.
TEST(growbuf, append_of_zero_bytes_leaves_the_buffer_terminated) {
  ccol_growbuf b;
  ccol_growbuf_init(&b, growbuf_procs());
  ccol_growbuf_append(&b, NULL, 0);
  ccol_growbuf_append_cstr(&b, "ok");
  ccol_growbuf_append(&b, NULL, 0);

  bool ok = !b.oom && (b.len == 2) && (strcmp(b.buf, "ok") == 0);
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(ok);
}

// Every ccol_retval enumerator with its pinned value and its spelling. Call
// sites compare with ccol_success == 0 directly, and a new enumerator anywhere
// but at the end renumbers every later one; this table makes that fail here
// instead of in some unrelated module.
static const struct {
  ccol_retval value;
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
  volatile ccol_retval bogus = (ccol_retval)12345;
  REQUIRE_STREQ(ccol_retval_to_str(bogus), "unknown");
}

// The type goes through a volatile variable, so the switch really runs instead
// of folding into a constant at each call site.
TEST(data_type, fixed_width_sizes_match_the_underlying_types) {
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
  volatile ccol_data_type t = ccol_string;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
  t = ccol_other_types;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
}

// Runs ccol__dump_key_to_stderr() on size bytes of data with stderr redirected
// to a temporary file, and copies what it wrote into out (out_size bytes, NUL
// terminated). The dump_key tests need the text, because that function runs on
// the fatal-error path of chmap and cbmap, where a fault would turn a
// diagnostic into a second crash. Gives false when the capture itself fails.
// Moves the stderr descriptor for the length of the call and restores it.
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
  ccol__dump_key_to_stderr(data, size);
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
  char src[16] = "hello";
  char *copy = ccol_strdup(NULL, src);
  memset(src, 'x', sizeof(src) - 1);
  src[sizeof(src) - 1] = '\0';

  bool ok = copy && strcmp(copy, "hello") == 0;
  free(copy);
  REQUIRE_TRUE(ok);
}

// A NULL input is documented as tolerated, so a caller can check once at the
// end. Without the guard the call reaches strlen(NULL) and crashes.
TEST(strdup, a_null_input_yields_null_rather_than_reading_through_it) {
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
  REQUIRE_EQ((void *)strstr(buf, "00000010"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|abcdefghijklmnop|"), (void *)NULL);
}

// The short last row pads its hex columns so the sidebar lines up.
TEST(dump_key, a_partial_second_row_is_padded_and_still_labelled) {
  unsigned char key[20];
  for (size_t i = 0; i < sizeof(key); i++) key[i] = (unsigned char)('a' + i);
  char buf[1024];
  REQUIRE_TRUE(dump_key_capture(key, sizeof(key), buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "00000000"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "00000010"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|qrst|"), (void *)NULL);
}

TEST(dump_key, unprintable_bytes_render_as_dots) {
  unsigned char key[4] = {0x00, 0x41, 0x1F, 0x7F};
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(key, sizeof(key), buf, sizeof(buf)));
  REQUIRE_NE((void *)strstr(buf, "|.A..|"), (void *)NULL);
}

// Every module holds its locks from its fork-prepare handler until after the
// fork, so the locks nest in the order the handlers run, and that order must be
// the same across every fork. The recorder aborts on an inversion, so the test
// runs in a forked child: The same pair recorded in one order and then the
// other must end the child with SIGABRT.
TEST(atfork_order, an_inverted_prepare_handler_order_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    ccol__atfork_order_record(ccol__atfork_module_clogger);
    ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
    ccol__atfork_order_reset();
    ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
    ccol__atfork_order_record(ccol__atfork_module_clogger);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The ordinary case, so the check above can't pass by refusing everything. Only
// WIFEXITED is checked: Under valgrind --error-exitcode overrides the exit
// status of a forked child, while a recorder that refused would abort.
TEST(atfork_order, a_repeated_consistent_order_is_accepted) {
  pid_t pid = fork();
  if (pid == 0) {
    for (int fork_round = 0; fork_round < 3; fork_round++) {
      ccol__atfork_order_record(ccol__atfork_module_clogger);
      ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
      ccol__atfork_order_record(ccol__atfork_module_cthreadpool);
      ccol__atfork_order_reset();
    }
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFEXITED(status));
}

// A module used for the first time between two forks runs first in the next
// fork, and its pairs with every later handler must be recorded. A recorder
// that starts a sequence only when a handler runs again (instead of at the
// reset) never records that cthreadpool ran before clogger and lets the third,
// inverted fork through.
TEST(atfork_order, a_handler_that_joins_later_is_ordered_against_the_rest) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    ccol__atfork_order_record(ccol__atfork_module_clogger);
    ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
    ccol__atfork_order_reset();
    ccol__atfork_order_record(ccol__atfork_module_cthreadpool);
    ccol__atfork_order_record(ccol__atfork_module_clogger);
    ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
    ccol__atfork_order_reset();
    ccol__atfork_order_record(ccol__atfork_module_clogger);
    ccol__atfork_order_record(ccol__atfork_module_cthreadpool);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A real fork() ends the sequence through the handlers that the recorder
// registers. Without that reset the opposite order after the inner fork would
// continue the first sequence and the child would exit 0.
TEST(atfork_order, a_real_fork_starts_a_new_sequence) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    ccol__atfork_order_record(ccol__atfork_module_clogger);
    ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
    pid_t inner = fork();
    if (inner == 0) test_child_exit(0);
    if (inner > 0) (void)waitpid(inner, NULL, 0);
    ccol__atfork_order_record(ccol__atfork_module_cthreadcomm);
    ccol__atfork_order_record(ccol__atfork_module_clogger);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  pid_t reaped = waitpid(pid, &status, 0);

  REQUIRE_EQ(reaped, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// An allocator that always hands out this buffer, so a test can assert the
// exact pointer an allocation macro gives instead of only checking for NULL.
static char ccol_hyg_buffer[256];

// The four functions of the fixed-buffer allocator. Every allocation gives
// ccol_hyg_buffer (calloc zeroes it first) and free does nothing, so the
// macro_hygiene test can compare exact pointers.
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

// If an allocation macro body isn't fully parenthesised, a cast or an addition
// outside it binds to one arm of its conditional only, the offset silently
// disappears and the pointer compares equal to the base. The final free goes
// through the procs directly: Through ccol__mem_free, Clang sees the default
// free() arm applied to a static buffer and raises -Wfree-nonheap-object.
TEST(macro_hygiene, an_allocation_macro_composes_with_a_surrounding_operator) {
  ccol_memmgmt_procs procs = {.malloc = ccol_hyg_malloc,
                              .calloc = ccol_hyg_calloc,
                              .realloc = ccol_hyg_realloc,
                              .free = ccol_hyg_free};
  ccol_memmgmt_procs *mp = &procs;

  char *offset_alloc = (char *)ccol__mem_alloc(mp, 64) + 16;
  REQUIRE_EQ((void *)offset_alloc, (void *)(ccol_hyg_buffer + 16));

  char *offset_calloc = (char *)ccol__mem_calloc(mp, 4, 8) + 8;
  REQUIRE_EQ((void *)offset_calloc, (void *)(ccol_hyg_buffer + 8));

  char *offset_realloc = (char *)ccol__mem_realloc(mp, ccol_hyg_buffer, 32) + 4;
  REQUIRE_EQ((void *)offset_realloc, (void *)(ccol_hyg_buffer + 4));

  mp->free(ccol_hyg_buffer);
}

// A macro temporary is in scope from the end of its declarator, so one named
// like a caller variable captures the caller's argument. With temporaries named
// var1 and var2 the first comparison gives 0 instead of -1, silently.
TEST(macro_hygiene, ccol_typed_cmp_does_not_capture_caller_identifiers) {
  int var1 = 5, var2 = 7;
  int a = 5, b = 7;
  REQUIRE_EQ(ccol__typed_cmp(&var1, &var2, int), -1);
  REQUIRE_EQ(ccol__typed_cmp(&a, &b, int), -1);
  REQUIRE_EQ(ccol__typed_cmp(&var2, &var1, int), 1);
  REQUIRE_EQ(ccol__typed_cmp(&var1, &var1, int), 0);
}

// The same capture, through the _Generic helpers of every container creation
// macro: A caller variable named like a temporary picks the default arm and an
// integral value reports as not integral.
TEST(macro_hygiene, type_introspection_does_not_capture_caller_identifiers) {
  int result = 42;
  int plain = 42;
  REQUIRE_TRUE(ccol__is_integral_type(result));
  REQUIRE_TRUE(ccol__is_integral_type(plain));
  REQUIRE_EQ(ccol_determine_ccol_data_type(result),
             ccol_determine_ccol_data_type(plain));
  char *ccol__ptr = "hello";
  cmap_pair pair_from_ptr = {0};
  ccol__populate_cmap_pair(&pair_from_ptr, ccol__ptr);
  REQUIRE_EQ(pair_from_ptr.size, strlen("hello") + 1);
  REQUIRE_STREQ((const char *)pair_from_ptr.ptr, "hello");
}

// Pointers whose own object is const. Taking the address of one gives a type
// like const char *const *, which a classifier built on the type of &(data)
// can't recognize.
static const char *const ccol_cls_const_name = "const-name";
static const signed char *const ccol_cls_const_signed =
    (const signed char *)"signed";
static const unsigned char *const ccol_cls_const_unsigned =
    (const unsigned char *)"unsigned";

TEST(char_classification, a_qualified_char_pointer_is_a_pointer_not_an_array) {
  char *const cp = "char-const";
  unsigned char *volatile vp = (unsigned char *)"volatile";
  signed char *const volatile cvp = (signed char *)"cv";
  REQUIRE_FALSE(ccol__is_char_array(ccol_cls_const_name));
  REQUIRE_FALSE(ccol__is_char_array(ccol_cls_const_signed));
  REQUIRE_FALSE(ccol__is_char_array(ccol_cls_const_unsigned));
  REQUIRE_FALSE(ccol__is_char_array(cp));
  REQUIRE_FALSE(ccol__is_char_array(vp));
  REQUIRE_FALSE(ccol__is_char_array(cvp));
  REQUIRE_TRUE(ccol__is_char_ptr(ccol_cls_const_name));
  REQUIRE_TRUE(ccol__is_char_ptr(cp));
  REQUIRE_EQ((int)ccol_determine_ccol_data_type(ccol_cls_const_name),
             (int)ccol_string);
  REQUIRE_STREQ((const char *)ccol_cls_const_signed, "signed");
}

TEST(char_classification, every_char_array_form_is_still_an_array) {
  char plain[8] = "plain";
  const char c_arr[6] = "const";
  signed char s_arr[4] = {'s', 'g', 'n', 0};
  const unsigned char u_arr[4] = {'u', 'n', 's', 0};
  REQUIRE_TRUE(ccol__is_char_array(plain));
  REQUIRE_TRUE(ccol__is_char_array(c_arr));
  REQUIRE_TRUE(ccol__is_char_array(s_arr));
  REQUIRE_TRUE(ccol__is_char_array(u_arr));
  REQUIRE_TRUE(ccol__is_char_array("literal"));
  char *p = plain;
  const char *cp = c_arr;
  REQUIRE_FALSE(ccol__is_char_array(p));
  REQUIRE_FALSE(ccol__is_char_array(cp));
  int not_a_string = 3;
  REQUIRE_FALSE(ccol__is_char_array(not_a_string));
}

// A pair built from a const pointer object must address the string, not the
// pointer object, whose strlen depends on its address bytes.
TEST(char_classification, populate_pair_reads_the_string_of_a_const_pointer) {
  cmap_pair pair = {0};
  ccol__populate_cmap_pair(&pair, ccol_cls_const_name);
  REQUIRE_EQ(pair.ptr, (void *)ccol_cls_const_name);
  REQUIRE_EQ(pair.size, strlen("const-name") + 1);

  char *const cp = "char-const";
  cmap_pair pair2 = {0};
  ccol__populate_cmap_pair(&pair2, cp);
  REQUIRE_EQ(pair2.ptr, (void *)cp);
  REQUIRE_EQ(pair2.size, strlen("char-const") + 1);

  cmap_pair pair3 = {0};
  ccol__populate_cmap_pair(&pair3, ccol_cls_const_unsigned);
  REQUIRE_EQ(pair3.ptr, (void *)ccol_cls_const_unsigned);
  REQUIRE_EQ(pair3.size, strlen("unsigned") + 1);

  char arr[16] = "array";
  cmap_pair pair4 = {0};
  ccol__populate_cmap_pair(&pair4, arr);
  REQUIRE_EQ(pair4.ptr, (void *)arr);
  REQUIRE_EQ(pair4.size, strlen("array") + 1);

  unsigned char *volatile vp = (unsigned char *)"volatile";
  cmap_pair pair5 = {0};
  ccol__populate_cmap_pair(&pair5, vp);
  REQUIRE_EQ(pair5.ptr, (void *)vp);
  REQUIRE_EQ(pair5.size, strlen("volatile") + 1);

  const char *null_str = NULL;
  cmap_pair pair6 = {(void *)arr, 99};
  ccol__populate_cmap_pair(&pair6, null_str);
  REQUIRE_EQ(pair6.ptr, (void *)NULL);
  REQUIRE_EQ(pair6.size, (size_t)0);
}

// Gives the size that ccol__populate_cmap_pair() computes for x. The nesting
// test uses it as a stand-in with the shape of every public value macro: The
// outer macro passes its argument in parentheses to an implementation macro,
// which copies x into a temporary named k, fills a cmap_pair named p from it
// and evaluates to p.size. k and p are names chosen by the caller, so two
// nested uses don't shadow each other. Reads no state outside its arguments.
#define CCOL_TEST_POP_SIZE(x, k, p) CCOL_TEST_POP_SIZE_IMPL((x), k, p)
#define CCOL_TEST_POP_SIZE_IMPL(x, k, p) \
  ({                                     \
    __typeof__(x) k = (x);               \
    cmap_pair p = {0};                   \
    ccol__populate_cmap_pair(&p, k);     \
    p.size;                              \
  })

// ccol__populate_cmap_pair() must hold no _Pragma, so a macro using it can take
// another one as its argument (the shape of chmap_get_ptr(m, chmap_get(m, k))).
// GCC 10 moves such a pragma in front of the outer macro and fails with
// "expected expression before '#pragma'".
TEST(char_classification, populate_pair_nests_inside_another_macro_argument) {
  const char *const s = "nested";
  size_t size = CCOL_TEST_POP_SIZE(
      CCOL_TEST_POP_SIZE(s, inner_k, inner_p) ? s : s, outer_k, outer_p);
  REQUIRE_EQ(size, strlen("nested") + 1);
}

#undef CCOL_TEST_POP_SIZE_IMPL
#undef CCOL_TEST_POP_SIZE

// A struct with padding between its members.
struct ccol_unqual_probe {
  char c;
  int i;
};

// Each declaration is a compile-time check that a temporary of the unqualified
// type is assignable and that an array type keeps its size, on every supported
// compiler including GCC 10.
TEST(unqual_type, drops_top_level_qualifiers_and_keeps_array_types) {
  const struct ccol_unqual_probe cs = {1, 2};
  ccol__unqual_type(cs) s;
  s = cs;
  _Static_assert(__builtin_types_compatible_p(ccol__unqual_type(cs),
                                              struct ccol_unqual_probe),
                 "a const struct gives the struct");

  const char *const cp = "text";
  ccol__unqual_type(cp) p;
  p = cp;
  _Static_assert(
      __builtin_types_compatible_p(ccol__unqual_type(cp), const char *),
      "a const pointer to const char gives a pointer to const char");

  const volatile _Bool cvb = 1;
  ccol__unqual_type(cvb) b;
  b = cvb;
  _Static_assert(sizeof(b) == sizeof(_Bool), "a bool stays a bool");

  void (*const fp)(void) = NULL;
  ccol__unqual_type(fp) f;
  f = fp;

  const char carr[5] = "abcd";
  char arr[16] = "array";
  _Static_assert(sizeof(ccol__unqual_type(carr)) == 5,
                 "an array type keeps its size");
  _Static_assert(sizeof(ccol__unqual_type(arr)) == 16,
                 "an array type keeps its size");

  REQUIRE_EQ(s.c, 1);
  REQUIRE_EQ(s.i, 2);
  REQUIRE_STREQ(p, "text");
  REQUIRE_TRUE(b);
  REQUIRE_TRUE(f == NULL);
  REQUIRE_STREQ(carr, "abcd");
  REQUIRE_STREQ(arr, "array");
}

#include <signal.h>

// Runs ccol__fatal_err() in a forked child whose stderr is a pipe, so the
// fatal_err test can read what it prints without dying itself. which picks the
// call with no argument (0) or with arguments (1). Copies what the child wrote
// into out (out_size bytes, NUL terminated) and gives true when the child died
// of SIGABRT.
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
      ccol__fatal_err("a message with no argument");
    } else {
      ccol__fatal_err("n=%d s=%s", 42, "text");
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

// The macro appends an empty string for a trailing %s, so it never needs an
// empty variadic list, and stderr gets exactly the caller's format.
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

#if defined(CCOL__CLOEXEC_GATE)
#include <arpa/inet.h>
#include <fcntl.h>
#include <internal/csock.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>

// What gate_forker reports: Whether fork() returned, and the child pid.
typedef struct {
  _Atomic int forked;
  pid_t pid;
} gate_fork_ctx;

// Thread function that forks, so the gate test can see whether a fork() of
// another thread waits at the gate. arg is a gate_fork_ctx; the child exits at
// once, and the parent side stores the pid and then sets forked.
static void *gate_forker(void *arg) {
  gate_fork_ctx *c = (gate_fork_ctx *)arg;
  pid_t pid = fork();
  if (pid == 0) test_child_exit(0);
  c->pid = pid;
  atomic_store(&c->forked, 1);
  return NULL;
}

// The gate's fork-prepare handler waits for the read side, so a fork can't
// return while a thread is inside the gate. The test holds the gate until the
// forking thread has started that handler, then pauses. Without the handler
// reached_prepare stays false; with a handler that doesn't wait the fork
// returns during the pause and forked_inside is 1.
TEST(cloexec_gate, fork_waits_while_a_descriptor_is_inside_the_gate) {
  gate_fork_ctx c;
  atomic_init(&c.forked, 0);
  c.pid = -1;
  ccol__cloexec_gate_enter();
  unsigned long prepares = atomic_load(&ccol__cloexec_gate_prepares_for_tests);
  pthread_t t;
  bool started = pthread_create(&t, NULL, gate_forker, &c) == 0;
  bool reached_prepare = false;
  for (int i = 0; started && i < 20000 && !reached_prepare; i++) {
    reached_prepare =
        atomic_load(&ccol__cloexec_gate_prepares_for_tests) != prepares;
    if (!reached_prepare) {
      struct timespec ms = {.tv_sec = 0, .tv_nsec = 1000 * 1000};
      nanosleep(&ms, NULL);
    }
  }
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
  nanosleep(&pause, NULL);
  int forked_inside = atomic_load(&c.forked);
  ccol__cloexec_gate_leave();
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

// Checks the flags of a descriptor that a gated creation made. Gives true when
// fd is closed on exec and, if nonblock is set, non-blocking.
static bool gate_fd_flags_ok(int fd, bool nonblock) {
  int fd_flags = fcntl(fd, F_GETFD);
  int fl_flags = fcntl(fd, F_GETFL);
  return fd_flags >= 0 && (fd_flags & FD_CLOEXEC) && fl_flags >= 0 &&
         (!nonblock || (fl_flags & O_NONBLOCK));
}

// How many times the gate was entered and left at a given moment.
typedef struct {
  unsigned long enters;
  unsigned long leaves;
} gate_mark;

// Takes a snapshot of the enter and leave counters of the gate, so a test can
// count the gate uses of one call. Reads the test counters of common.c.
static gate_mark gate_mark_now(void) {
  gate_mark m = {atomic_load(&ccol__cloexec_gate_enters_for_tests),
                 atomic_load(&ccol__cloexec_gate_leaves_for_tests)};
  return m;
}

// Compares the gate counters with a snapshot, to check that one creation call
// used the gate exactly once. Gives true when the gate was entered once and
// left once since before.
static bool gate_used_once(gate_mark before) {
  gate_mark after = gate_mark_now();
  return after.enters - before.enters == 1 && after.leaves - before.leaves == 1;
}

// Every call that creates a descriptor in two steps enters and leaves the gate
// once, on success and on failure, and gives a close-on-exec descriptor. A site
// that skips the gate leaves both counts unchanged, and a failure path that
// skips the exit leaves the entries one ahead.
TEST(cloexec_gate, every_two_step_creation_uses_the_gate) {
#if defined(CCOL__EMULATE_DARWIN_SOCK)
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
#if defined(CCOL__EMULATE_DARWIN_SYNC)
  ccol__semaphore sem;
  gate_mark sm = gate_mark_now();
  bool sem_made = ccol__semaphore_init(sem, 1) == 0;
  bool sem_gated = gate_used_once(sm);
  bool sem_flags = sem_made && gate_fd_flags_ok(sem.fd[0], false) &&
                   gate_fd_flags_ok(sem.fd[1], true);
  if (sem_made) ccol__semaphore_destroy(sem);
  REQUIRE_TRUE(sem_made);
  REQUIRE_TRUE(sem_gated);
  REQUIRE_TRUE(sem_flags);
#endif
}
// What the early prepare handler saw: 1 when the gate's prepare had already
// run, 0 when it hadn't, -1 before any fork.
static _Atomic int gate_early_handler_saw_gate = -1;

// A fork-prepare handler registered before the library's constructors, to check
// that the gate's prepare handler still runs after it. Stores 1 in
// gate_early_handler_saw_gate when the gate's prepare had already run, else 0.
static void gate_early_prepare(void) {
  atomic_store(&gate_early_handler_saw_gate,
               ccol__cloexec_gate_prepared_for_tests ? 1 : 0);
}

// Registers gate_early_prepare through ccol__at_fork before the library's own
// constructors run, which is the order the gate test needs. On ELF a
// priority-101 constructor runs before every default-priority one (the gate's
// in common.c included); macOS doesn't order constructors by priority.
#if defined(__APPLE__)
__attribute__((constructor))
#else
__attribute__((constructor(101)))
#endif
static void
gate_register_early_handler(void) {
  (void)ccol__at_fork(gate_early_prepare, NULL, NULL);
}

// A handler that ccol__at_fork registers before the gate's constructor runs
// must still run before the gate's prepare handler; otherwise a fork could wait
// for a lock whose holder waits to enter the gate. If ccol__at_fork didn't
// register the gate first, the gate's prepare would run first and the handler
// would see it.
TEST(cloexec_gate, a_handler_registered_before_the_library_runs_before_gate) {
  atomic_store(&gate_early_handler_saw_gate, -1);
  pid_t pid = fork();
  if (pid == 0) test_child_exit(0);
  int status = 0;
  bool reaped = pid > 0 && waitpid(pid, &status, 0) == pid;
  REQUIRE_TRUE(reaped);
  REQUIRE_EQ(atomic_load(&gate_early_handler_saw_gate), 0);
}
#endif

#if defined(__OpenBSD__)
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>

// Tells fork_libc_spin to stop.
static atomic_bool fork_libc_stop;

// Thread function that calls libc functions taking libc's internal locks, so a
// fork of the main thread can land while one of them is held. Runs until
// fork_libc_stop is set.
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

// OpenBSD's libc takes its internal locks only while the process is threaded
// and marks a fork child as not threaded, and the library's fork handlers must
// leave that mark alone. A thread loops on getpwnam() and localtime_r() while
// we fork children that call both. A handler that marks the child as threaded
// makes a child wait for ever within the first few dozen forks.
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

// State of the iterator probe: The iterator the next callback got, the two
// iterators to hand out, and how many were handed out.
static cmap_iterator *iter_probe_seen;
static cmap_iterator *iter_probe_its[2];
static int iter_probe_picks;

// Hands out the next iterator of iter_probe_its. The iterator test passes this
// call as the argument of ccol_iter_next(), so iter_probe_picks counts how
// often the macro evaluated it.
static cmap_iterator *iter_probe_pick(void) {
  return iter_probe_its[iter_probe_picks++];
}

// The next callback of the probe iterators. It stores its argument in
// iter_probe_seen, so the test sees which iterator the macro handed over, and
// gives NULL to end the iteration.
static cmap_iterator *iter_probe_next(cmap_iterator *it) {
  iter_probe_seen = it;
  return NULL;
}

// The argument is a call with a side effect. An expansion like
// ((it)->ccol__next_fn(it)) makes two picks and hands the callback the second
// iterator.
TEST(iterators, iter_next_evaluates_its_argument_once) {
  static cmap_iterator a = {.key_pair = NULL,
                            .val_pair = NULL,
                            .ccol__next_fn = iter_probe_next,
                            .ccol__free_fn = NULL,
                            .ccol__direct_ptr = false};
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

// Runs the checks of the forked child of the semaphore test: 100000 posts with
// no waiter, then a wait that consumes one. Under the emulated sync it also
// checks that a wait on an empty pipe whose write end is gone fails with errno
// EINVAL. Gives 0, or a distinct status for each failed check.
static int semaphore_post_child(void) {
  ccol__semaphore sem;
  if (ccol__semaphore_init(sem, 0) != 0) return 10;
  for (int i = 0; i < 100000; i++) {
    if (ccol__semaphore_post(sem) != 0) return 11;
  }
  if (ccol__semaphore_wait(sem) != 0) return 12;
#if defined(CCOL__EMULATE_DARWIN_SYNC)
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
  if (ccol__semaphore_wait(sem) != -1 || errno != EINVAL) return 16;
#endif
  ccol__semaphore_destroy(sem);
  return 0;
}

// A post never waits, like sem_post(3): chttpsvr_engine_stop() posts from a
// signal handler. The child posts far more than a pipe holds and must report
// within the bound; it reports through a pipe because valgrind overrides a
// forked child's exit status.
TEST(semaphore, posts_without_a_waiter_never_block) {
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

// The number of threads in each burst of the intern tests; enough for many
// threads to meet one slot at the same moment.
#define INTERN_RACE_THREADS 16

// One worker of an intern burst: Its procs, the shared arrival count and go
// flag, and the pointer and code it got back.
typedef struct {
  ccol_memmgmt_procs *procs;
  atomic_int *arrived;
  atomic_bool *go;
  ccol_memmgmt_procs *got;
  ccol_retval rv;
} intern_race_arg;

// Sleeps 100 us. The intern tests poll their arrival count and go flag with it,
// so a waiting thread doesn't take the CPU from the one it waits for.
static void intern_race_pause(void) {
  struct timespec ts = {0, 100000L};
  nanosleep(&ts, NULL);
}

// Thread function of an intern burst. p is an intern_race_arg. It counts its
// arrival, waits for go, then interns a private copy of *procs, so equal
// pointers come from equal contents and not from one shared address. Stores the
// pointer and the code in the arg; changes the process-wide intern table.
static void *intern_race_worker(void *p) {
  intern_race_arg *a = p;
  atomic_fetch_add(a->arrived, 1);
  while (!atomic_load(a->go)) intern_race_pause();
  ccol_memmgmt_procs mine = *a->procs;
  a->rv = ccol_procs_intern(&mine, &a->got);
  return NULL;
}

// Threads that use one content for the first time at once must all get the same
// pointer and take one slot. The claim hook holds the first claimer between its
// claim and its copy until every other thread has seen the slot being written,
// so the race happens however late they start. A lookup that skips a slot being
// written hands out several pointers. The function pointers are never called.
TEST(procs_intern, a_burst_of_first_uses_of_one_content_takes_one_slot) {
  ccol_memmgmt_procs content = {
      (ccol_malloc)(uintptr_t)0x7a11000, (ccol_free)(uintptr_t)0x7a11010,
      (ccol_calloc)(uintptr_t)0x7a11020, (ccol_realloc)(uintptr_t)0x7a11030};
  size_t before = ccol__procs_intern_used_for_tests();
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
      atomic_load(&ccol__procs_intern_writing_sightings_for_tests);
  if (created >= 2) {
    atomic_store(&ccol__procs_intern_claim_wait_sightings_for_tests,
                 sightings_before + (unsigned)(created - 1));
    atomic_store(&ccol__procs_intern_claim_delay_us_for_tests, 30000000u);
  }
  atomic_store(&go, true);
  for (int i = 0; i < created; i++) pthread_join(tids[i], NULL);
  atomic_store(&ccol__procs_intern_claim_delay_us_for_tests, 0u);
  atomic_store(&ccol__procs_intern_claim_wait_sightings_for_tests, 0u);
  unsigned sightings =
      atomic_load(&ccol__procs_intern_writing_sightings_for_tests) -
      sightings_before;
  size_t after = ccol__procs_intern_used_for_tests();

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
  ccol__procs_intern_truncate_for_tests(before);

  REQUIRE_EQ(created, INTERN_RACE_THREADS);
  REQUIRE_EQ(failed, 0);
  REQUIRE_EQ(distinct, 0);
  REQUIRE_TRUE(same_content);
  REQUIRE_EQ(after, before + 1);
  REQUIRE_GE(sightings, (unsigned)(created - 1));
}

// Contents shared by pairs of threads fill one slot per content, and each pair
// gets one pointer.
TEST(procs_intern,
     a_burst_of_first_uses_of_distinct_contents_fills_one_slot_each) {
  size_t before = ccol__procs_intern_used_for_tests();
  ccol_memmgmt_procs contents[INTERN_RACE_THREADS];
  for (int i = 0; i < INTERN_RACE_THREADS; i++) {
    uintptr_t base = 0x7b22000 + (uintptr_t)(i / 2) * 0x100;
    contents[i] = (ccol_memmgmt_procs){
        (ccol_malloc)base, (ccol_free)(base + 0x10), (ccol_calloc)(base + 0x20),
        (ccol_realloc)(base + 0x30)};
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
  atomic_store(&ccol__procs_intern_claim_delay_us_for_tests, 2000u);
  atomic_store(&go, true);
  for (int i = 0; i < created; i++) pthread_join(tids[i], NULL);
  atomic_store(&ccol__procs_intern_claim_delay_us_for_tests, 0u);
  size_t after = ccol__procs_intern_used_for_tests();

  int wrong = 0;
  for (int i = 0; i < created; i++) {
    const ccol_memmgmt_procs *g = args[i].got;
    if (args[i].rv != ccol_success || g == NULL ||
        g->malloc != contents[i].malloc || g->free != contents[i].free ||
        g->calloc != contents[i].calloc || g->realloc != contents[i].realloc)
      wrong++;
    if ((i & 1) && g != args[i - 1].got) wrong++;
  }
  ccol__procs_intern_truncate_for_tests(before);

  REQUIRE_EQ(created, INTERN_RACE_THREADS);
  REQUIRE_EQ(wrong, 0);
  REQUIRE_EQ(after, before + INTERN_RACE_THREADS / 2);
}

#include <internal/cdeadline.h>

// A condition variable with its mutex and the flag the signaller sets.
typedef struct {
  ccol__mutex m;
  ccol__cond_var c;
  bool done;
} far_wait_ctx;

// Thread function that wakes the far-deadline wait of the cond_var test. After
// 100 ms it sets done and signals c of the far_wait_ctx in arg, under its
// mutex.
static void *far_wait_signaller(void *arg) {
  far_wait_ctx *x = (far_wait_ctx *)arg;
  struct timespec ts = {0, 100000000L};
  nanosleep(&ts, NULL);
  ccol__mutex_lock(x->m);
  x->done = true;
  ccol__cond_var_signal(x->c);
  ccol__mutex_unlock(x->m);
  return NULL;
}

// Every "no limit" timeout of the library gives a deadline at the end of time.
// With the emulated sync and a 32-bit time_t, an unsaturated sum would wrap
// into the past and the loop would wake thousands of times instead of once or
// twice.
TEST(cond_var, a_wait_until_the_end_of_time_blocks) {
  far_wait_ctx x = {.done = false};
  ccol__cond_var_attr a;
  REQUIRE_EQ(ccol__mutex_init(x.m), 0);
  REQUIRE_EQ(ccol__cond_var_attr_init(a), 0);
  REQUIRE_EQ(ccol__cond_var_attr_setclock(a, CLOCK_MONOTONIC), 0);
  REQUIRE_EQ(ccol__cond_var_init_ca(x.c, a), 0);
  (void)ccol__cond_var_attr_destroy(a);
  struct timespec deadline;
  ccol_deadline_after_us(UINT64_MAX, &deadline);
  pthread_t t;
  int created = pthread_create(&t, NULL, far_wait_signaller, &x);
  int wakes = 0;
  ccol__mutex_lock(x.m);
  while (created == 0 && !x.done && wakes < 100000) {
    (void)ccol__cond_var_timedwait(x.c, x.m, deadline);
    wakes++;
  }
  ccol__mutex_unlock(x.m);
  if (created == 0) pthread_join(t, NULL);
  ccol__cond_var_destroy(x.c);
  ccol__mutex_destroy(x.m);
  REQUIRE_EQ(created, 0);
  REQUIRE_LT(wakes, 10);
}

// Makes cnumlocale.h count the users of the C locale object on every system, so
// the c_locale test can drive the destructor path that keeps a used object.
#define CCOL__C_LOCALE_COUNT_USERS_FOR_TESTS 1
#include <internal/cnumlocale.h>
#include <stdio.h>

// The locale user thread: stage is 1 inside the scope, 2 when it may leave,
// 3 once it left; text holds what it formatted.
typedef struct {
  _Atomic int stage;
  char text[32];
} _cnl_user_t;

// Thread function that holds a C locale scope across the locale destructor, for
// the c_locale test. arg is a _cnl_user_t. It enters a scope and sets stage 1,
// waits for stage 2 (the bound is only a hang guard and must outlast the
// destructor), formats 1.5 into text, leaves and sets stage 3. Reads and
// changes the user count of cnumlocale.h.
static void *_cnl_user_thread(void *arg) {
  _cnl_user_t *u = (_cnl_user_t *)arg;
  ccol_c_locale_scope scope = ccol_c_locale_enter();
  atomic_store(&u->stage, 1);
  for (int i = 0; i < 60000 && atomic_load(&u->stage) != 2; i++) usleep(1000);
  snprintf(u->text, sizeof(u->text), "%.1f", 1.5);
  ccol_c_locale_leave(scope);
  atomic_store(&u->stage, 3);
  return NULL;
}

// A destructor that frees the locale object while a thread is inside a scope
// makes that thread format under a freed locale, which valgrind reports. An
// enter after the destructor must switch nothing.
TEST(c_locale, the_destructor_keeps_an_object_that_a_scope_still_uses) {
  _cnl_user_t u = {.stage = 0};
  pthread_t t;
  bool started = pthread_create(&t, NULL, _cnl_user_thread, &u) == 0;
  for (int i = 0; started && i < 60000 && atomic_load(&u.stage) != 1; i++)
    usleep(1000);
  bool in_scope = started && atomic_load(&u.stage) == 1;
  ccol__c_locale_fini();
  bool kept = ccol__c_locale_kept != (locale_t)0;
  ccol_c_locale_scope late = ccol_c_locale_enter();
  bool late_switched = late.switched;
  ccol_c_locale_leave(late);
  atomic_store(&u.stage, 2);
  if (started) pthread_join(t, NULL);
  size_t users = atomic_load(&ccol__c_locale_users);
  if (ccol__c_locale_kept) {
    freelocale(ccol__c_locale_kept);
    ccol__c_locale_kept = (locale_t)0;
  }
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(in_scope);
  REQUIRE_TRUE(kept);
  REQUIRE_FALSE(late_switched);
  REQUIRE_EQ(users, (size_t)0);
  REQUIRE_STREQ(u.text, "1.5");
}

// sscanf shares no code with the shifts that build CCOL_VERSION_NUMBER, so a
// header that changes one spelling and not the other fails here.
TEST(version, the_number_and_the_text_name_the_same_release) {
  unsigned int major = 0, minor = 0, patch = 0;
  char tail = 0;
  int n =
      sscanf(CCOL_VERSION_STRING, "%u.%u.%u%c", &major, &minor, &patch, &tail);
  REQUIRE_EQ(n, 3);
  REQUIRE_EQ(major, (unsigned int)CCOL_VERSION_MAJOR);
  REQUIRE_EQ(minor, (unsigned int)CCOL_VERSION_MINOR);
  REQUIRE_EQ(patch, (unsigned int)CCOL_VERSION_PATCH);
  REQUIRE_LT(minor, 256u);
  REQUIRE_LT(patch, 256u);
  REQUIRE_EQ((unsigned int)CCOL_VERSION_NUMBER,
             major * 65536u + minor * 256u + patch);
}

TEST(version, the_run_time_calls_report_the_built_library) {
  REQUIRE_EQ(ccol_version(), (unsigned int)CCOL_VERSION_NUMBER);
  REQUIRE_STREQ(ccol_version_string(), CCOL_VERSION_STRING);
  REQUIRE_EQ((void *)ccol_version_string(), (void *)ccol_version_string());
}
