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
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
TAU_MAIN()  // sets up Tau (+ main function)

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

/* A single shared instance, returned by pointer from a function (rather
 * than callers taking the address of a local/global directly), so that
 * passing it to _ccol_mem_alloc/_ccol_mem_free/ccol_scoped_ptr_mp doesn't trip
 * -Waddress ("the address of X will always evaluate as true") the way a
 * literal &some_local_var would at the macro call site. */
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
  /* No counting hook exists for the default allocator; absence of a leak
   * or double free is confirmed separately under `make memtest`. This
   * test exists to exercise the non-_mp macro form itself compiling and
   * running correctly. */
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
    /* Reassigning without freeing the earlier value first is the macro's
     * documented caveat, not something it tries to solve on its own. */
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
  if (should_fail) return false; /* early return, buf still freed */
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
/*                  ccol_mem_zero / ccol_mem_cpy SIZE SWEEP */
/* ========================================================================== */

/* Both helpers dispatch on size: a buffer of SMALL_CHUNKS_SIZE (32) bytes or
 * fewer, with every pointer involved aligned to uint64_t, takes an inlined
 * switch with one packed-struct assignment per exact byte count, and anything
 * else falls through to the C library. Each of those 33 arms is a distinct
 * store sequence, so a single wrong arm writes one byte too few or one too
 * many into whatever sits next in memory and nothing else in the suite would
 * notice.
 *
 * The sweeps below drive the size from a loop variable rather than a literal
 * at each call site. That is load-bearing for what they measure, not a style
 * choice: with a compile-time constant size the optimizer folds the switch
 * away entirely and the individual arms are never executed as such. */

#define GUARD_BYTE 0xC7u
#define SWEEP_PAYLOAD 33 /* 0 through SMALL_CHUNKS_SIZE inclusive */
#define SWEEP_GUARD 8

/* Payload framed by guard bytes on both sides, so an arm that runs past its
 * declared length is caught rather than silently corrupting a neighbour. */
typedef struct sweep_buf {
  unsigned char lead[SWEEP_GUARD];
  _Alignas(16) unsigned char payload[SWEEP_PAYLOAD + SWEEP_GUARD];
} sweep_buf;

static void sweep_buf_fill(sweep_buf *b, unsigned char v) {
  memset(b->lead, GUARD_BYTE, sizeof b->lead);
  memset(b->payload, v, sizeof b->payload);
  memset(b->payload + SWEEP_PAYLOAD, GUARD_BYTE, SWEEP_GUARD);
}

static bool sweep_guards_intact(const sweep_buf *b) {
  for (size_t i = 0; i < SWEEP_GUARD; i++) {
    if (b->lead[i] != GUARD_BYTE) return false;
    if (b->payload[SWEEP_PAYLOAD + i] != GUARD_BYTE) return false;
  }
  return true;
}

TEST(mem_zero, aligned_size_sweep_zeroes_exactly_n_bytes) {
  for (volatile size_t n = 0; n <= 32; n++) {
    sweep_buf b;
    sweep_buf_fill(&b, 0xAA);
    ccol_mem_zero(b.payload, (size_t)n);

    bool head_zeroed = true, tail_untouched = true;
    for (size_t i = 0; i < (size_t)n; i++)
      if (b.payload[i] != 0) head_zeroed = false;
    for (size_t i = (size_t)n; i < SWEEP_PAYLOAD; i++)
      if (b.payload[i] != 0xAA) tail_untouched = false;

    REQUIRE_TRUE(head_zeroed);
    REQUIRE_TRUE(tail_untouched);
    REQUIRE_TRUE(sweep_guards_intact(&b));
  }
}

TEST(mem_zero, misaligned_size_sweep_takes_the_library_path) {
  /* An odd offset fails the uint64_t alignment check, so the same sizes go
   * through memset instead of the inlined switch. */
  for (volatile size_t n = 0; n <= 32; n++) {
    sweep_buf b;
    sweep_buf_fill(&b, 0x5A);
    ccol_mem_zero(b.payload + 1, (size_t)n);

    bool ok = (b.payload[0] == 0x5A);
    for (size_t i = 0; i < (size_t)n; i++)
      if (b.payload[1 + i] != 0) ok = false;
    for (size_t i = 1 + (size_t)n; i < SWEEP_PAYLOAD; i++)
      if (b.payload[i] != 0x5A) ok = false;

    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(sweep_guards_intact(&b));
  }
}

TEST(mem_zero, sizes_above_the_small_chunk_limit_use_memset) {
  enum { BIG = 1024 };
  unsigned char *p = (unsigned char *)malloc(BIG);
  REQUIRE_NE((void *)p, (void *)NULL);
  memset(p, 0xEE, BIG);
  volatile size_t n = BIG;
  ccol_mem_zero(p, (size_t)n);

  bool all_zero = true;
  for (size_t i = 0; i < BIG; i++)
    if (p[i] != 0) all_zero = false;
  free(p);
  REQUIRE_TRUE(all_zero);
}

TEST(mem_cpy, aligned_size_sweep_copies_exactly_n_bytes) {
  for (volatile size_t n = 0; n <= 32; n++) {
    sweep_buf src, dst;
    sweep_buf_fill(&src, 0x00);
    sweep_buf_fill(&dst, 0x5A);
    for (size_t i = 0; i < SWEEP_PAYLOAD; i++)
      src.payload[i] = (unsigned char)(i + 1);

    ccol_mem_cpy(dst.payload, src.payload, (size_t)n);

    bool copied = true, tail_untouched = true;
    for (size_t i = 0; i < (size_t)n; i++)
      if (dst.payload[i] != (unsigned char)(i + 1)) copied = false;
    for (size_t i = (size_t)n; i < SWEEP_PAYLOAD; i++)
      if (dst.payload[i] != 0x5A) tail_untouched = false;

    REQUIRE_TRUE(copied);
    REQUIRE_TRUE(tail_untouched);
    REQUIRE_TRUE(sweep_guards_intact(&dst));
  }
}

TEST(mem_cpy, misaligned_size_sweep_takes_the_library_path) {
  for (volatile size_t n = 0; n <= 32; n++) {
    sweep_buf src, dst;
    sweep_buf_fill(&src, 0x00);
    sweep_buf_fill(&dst, 0x33);
    for (size_t i = 0; i < SWEEP_PAYLOAD; i++)
      src.payload[i] = (unsigned char)(0xF0 ^ i);

    ccol_mem_cpy(dst.payload + 1, src.payload + 1, (size_t)n);

    bool ok = (dst.payload[0] == 0x33);
    for (size_t i = 0; i < (size_t)n; i++)
      if (dst.payload[1 + i] != (unsigned char)(0xF0 ^ (i + 1))) ok = false;
    for (size_t i = 1 + (size_t)n; i < SWEEP_PAYLOAD; i++)
      if (dst.payload[i] != 0x33) ok = false;

    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(sweep_guards_intact(&dst));
  }
}

TEST(mem_cpy, sizes_above_the_small_chunk_limit_use_memcpy) {
  enum { BIG = 4096 };
  unsigned char *src = (unsigned char *)malloc(BIG);
  unsigned char *dst = (unsigned char *)malloc(BIG);
  bool allocated = (src != NULL && dst != NULL);
  bool matches = false;
  if (allocated) {
    for (size_t i = 0; i < BIG; i++) src[i] = (unsigned char)(i * 7);
    memset(dst, 0, BIG);
    volatile size_t n = BIG;
    ccol_mem_cpy(dst, src, (size_t)n);
    matches = (memcmp(dst, src, BIG) == 0);
  }
  free(src);
  free(dst);
  REQUIRE_TRUE(allocated);
  REQUIRE_TRUE(matches);
}

/* ========================================================================== */
/*                  ccol_find_nearest_gte_power_of_two */
/* ========================================================================== */

/* Declared in common.c rather than common.h; chashmap sizes every table it
 * allocates with it. */
extern size_t ccol_find_nearest_gte_power_of_two(size_t input);

TEST(power_of_two, exact_powers_return_themselves) {
  for (volatile unsigned bit = 0; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(v), v);
  }
}

TEST(power_of_two, values_round_up_to_the_next_power) {
  /* One below and one above each power exercises both directions of the
   * binary search, including its "the entry before this one is smaller, so
   * this is the answer" early exit. */
  for (volatile unsigned bit = 2; bit < 20; bit++) {
    size_t v = (size_t)1 << bit;
    REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(v - 1), v);
    REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(v + 1), v << 1);
  }
}

TEST(power_of_two, small_inputs_clamp_to_one) {
  /* Anything at or below the first table entry returns that entry. */
  REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(0), (size_t)1);
  REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(1), (size_t)1);
}

TEST(power_of_two, above_the_representable_maximum_is_invalid) {
  /* The table stops at the largest power of two a size_t can hold, so
   * anything past it has no answer to give. */
  size_t top = ((size_t)1) << (sizeof(size_t) * 8 - 1);
  REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(top), top);
  REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(top + 1), ccol_invalid_size);
  REQUIRE_EQ(ccol_find_nearest_gte_power_of_two(SIZE_MAX), ccol_invalid_size);
}

/* ========================================================================== */
/*                         ccol_growbuf_t */
/* ========================================================================== */

/* A reallocator that refuses once armed, so the buffer's own out-of-memory
 * latch can be driven deliberately instead of waiting for a real allocation
 * failure. */
static bool g_growbuf_refuse_realloc = false;
static void *_growbuf_malloc(size_t n) { return malloc(n); }
static void _growbuf_free(void *p) { free(p); }
static void *_growbuf_calloc(size_t a, size_t b) { return calloc(a, b); }
static void *_growbuf_realloc(void *p, size_t n) {
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
  /* Documented as tolerated rather than undefined, so that a caller can pass
   * another buffer's own buf field straight through after that buffer's init
   * or append failed and left it NULL. */
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

  /* Appends that still fit inside the initial capacity succeed before any
   * grow is attempted, so the length to compare against is the one the buffer
   * holds at the instant the latch trips, not the one before the loop. */
  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  bool latched = b.oom;
  size_t len_before = b.len;
  /* Once latched the buffer stays latched: an append after the failure must
   * not resume writing, even when the allocator recovers. */
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
  /* b.buf may legitimately be NULL here, and destroy has to tolerate it. */
  ccol_growbuf_t b;
  ccol_growbuf_init(&b, growbuf_procs());
  g_growbuf_refuse_realloc = true;
  for (int i = 0; i < 64; i++) ccol_growbuf_append_cstr(&b, "0123456789abcdef");
  g_growbuf_refuse_realloc = false;
  bool latched = b.oom;
  ccol_growbuf_destroy(&b);
  REQUIRE_TRUE(latched);
}

/* ========================================================================== */
/*                  ccol_retval_to_str / data type size / key dump */
/* ========================================================================== */

/* ccol_retval_t pins an explicit numeric value on every enumerator, and
 * ccol_success == 0 is relied on directly by call sites that test for it. The
 * table below asserts the value and the spelling together, so that inserting
 * an enumerator anywhere but the end, which would silently renumber every
 * later one, fails here rather than surfacing as an unrelated module
 * reporting a retval it never returns. */
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
  /* Driven through a volatile so the switch is actually executed rather than
   * folded into a constant at each call site. */
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
  /* A string and an opaque blob have no width the caller can assume. */
  volatile ccol_data_type t = ccol_string;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
  t = ccol_other_types;
  REQUIRE_EQ(ccol_fixed_width_data_type_size(t), (size_t)0);
}

/* _ccol_dump_key_to_stderr runs inside the fatal-error path that chmap and
 * cbmap take before ccol_fatal_err(), so a fault in it turns a diagnostic into
 * a second crash on top of the first one. Capturing stderr is what makes it
 * assertable at all. */
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

TEST(dump_key, a_single_byte_uses_the_singular_noun) {
  unsigned char key = 0x41;
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(&key, 1, buf, sizeof buf));
  REQUIRE_NE((void *)strstr(buf, "Key dump (1 byte):"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "41"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|A|"), (void *)NULL);
}

TEST(dump_key, several_bytes_use_the_plural_noun) {
  unsigned char key[3] = {0x41, 0x42, 0x43};
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(key, sizeof key, buf, sizeof buf));
  REQUIRE_NE((void *)strstr(buf, "Key dump (3 bytes):"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|ABC|"), (void *)NULL);
}

TEST(dump_key, exactly_one_full_row_emits_a_single_offset_line) {
  unsigned char key[16];
  for (size_t i = 0; i < sizeof key; i++) key[i] = (unsigned char)('a' + i);
  char buf[1024];
  REQUIRE_TRUE(dump_key_capture(key, sizeof key, buf, sizeof buf));
  REQUIRE_NE((void *)strstr(buf, "00000000"), (void *)NULL);
  /* A second row would start at offset 16. */
  REQUIRE_EQ((void *)strstr(buf, "00000010"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "|abcdefghijklmnop|"), (void *)NULL);
}

TEST(dump_key, a_partial_second_row_is_padded_and_still_labelled) {
  unsigned char key[20];
  for (size_t i = 0; i < sizeof key; i++) key[i] = (unsigned char)('a' + i);
  char buf[1024];
  REQUIRE_TRUE(dump_key_capture(key, sizeof key, buf, sizeof buf));
  REQUIRE_NE((void *)strstr(buf, "00000000"), (void *)NULL);
  REQUIRE_NE((void *)strstr(buf, "00000010"), (void *)NULL);
  /* The short final row pads its hex columns so the sidebar stays aligned. */
  REQUIRE_NE((void *)strstr(buf, "|qrst|"), (void *)NULL);
}

TEST(dump_key, unprintable_bytes_render_as_dots) {
  unsigned char key[4] = {0x00, 0x41, 0x1F, 0x7F};
  char buf[512];
  REQUIRE_TRUE(dump_key_capture(key, sizeof key, buf, sizeof buf));
  REQUIRE_NE((void *)strstr(buf, "|.A..|"), (void *)NULL);
}
