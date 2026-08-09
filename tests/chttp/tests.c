#include <chttp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                     BASE64 ENCODE; RFC 4648 TEST VECTORS                 */
/* ========================================================================== */

TEST(base64_encode, rfc4648_vectors) {
  struct {
    const char *input;
    const char *expected;
  } cases[] = {
      {"", ""},
      {"f", "Zg=="},
      {"fo", "Zm8="},
      {"foo", "Zm9v"},
      {"foob", "Zm9vYg=="},
      {"fooba", "Zm9vYmE="},
      {"foobar", "Zm9vYmFy"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    size_t out_len = (size_t)-1;
    char *enc =
        chttp_base64_encode(cases[i].input, strlen(cases[i].input), &out_len);
    REQUIRE_NE((void *)enc, NULL);
    REQUIRE_STREQ(enc, cases[i].expected);
    REQUIRE_EQ(out_len, strlen(cases[i].expected));
    free(enc);
  }
}

TEST(base64_encode, out_len_is_optional) {
  char *enc = chttp_base64_encode("foobar", 6, NULL);
  REQUIRE_NE((void *)enc, NULL);
  REQUIRE_STREQ(enc, "Zm9vYmFy");
  free(enc);
}

TEST(base64_encode, null_data_with_zero_len_succeeds) {
  size_t out_len = (size_t)-1;
  char *enc = chttp_base64_encode(NULL, 0, &out_len);
  REQUIRE_NE((void *)enc, NULL);
  REQUIRE_STREQ(enc, "");
  REQUIRE_EQ(out_len, (size_t)0);
  free(enc);
}

TEST(base64_encode, null_data_with_nonzero_len_fails) {
  REQUIRE_EQ((void *)chttp_base64_encode(NULL, 4, NULL), NULL);
}

TEST(base64_encode, length_large_enough_to_overflow_is_rejected) {
  /* Regression test: enc_len = ((len+2)/3)*4 had no overflow check before
   * allocating enc_len+1 bytes; a caller-supplied len large enough to wrap
   * size_t would previously proceed with a tiny wrapped allocation and then
   * write far past it. len is deliberately close to SIZE_MAX here, well
   * past what this function could ever legitimately be asked to encode; a
   * real (non-NULL, non-dereferenced) pointer is passed since a correctly
   * fixed implementation must reject this before ever touching *data*, not
   * merely before allocating. */
  char dummy = 0;
  REQUIRE_EQ((void *)chttp_base64_encode(&dummy, SIZE_MAX - 1, NULL), NULL);
}

TEST(base64_encode, embedded_nul_bytes_round_trip_via_out_len) {
  const unsigned char raw[] = {0x00, 0x01, 0x00, 0xFF, 0x02};
  size_t out_len = 0;
  char *enc = chttp_base64_encode(raw, sizeof(raw), &out_len);
  REQUIRE_NE((void *)enc, NULL);
  REQUIRE_EQ(out_len, strlen(enc)); /* encoded text itself has no NULs */

  size_t dec_len = 0;
  unsigned char *dec = (unsigned char *)chttp_base64_decode(enc, &dec_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(dec_len, sizeof(raw));
  REQUIRE_EQ(memcmp(dec, raw, sizeof(raw)), 0);
  free(enc);
  free(dec);
}

/* ========================================================================== */
/*                     BASE64 DECODE; RFC 4648 TEST VECTORS                 */
/* ========================================================================== */

TEST(base64_decode, rfc4648_vectors) {
  struct {
    const char *input;
    const char *expected;
  } cases[] = {
      {"", ""},
      {"Zg==", "f"},
      {"Zm8=", "fo"},
      {"Zm9v", "foo"},
      {"Zm9vYg==", "foob"},
      {"Zm9vYmE=", "fooba"},
      {"Zm9vYmFy", "foobar"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    size_t out_len = (size_t)-1;
    char *dec = (char *)chttp_base64_decode(cases[i].input, &out_len);
    REQUIRE_NE((void *)dec, NULL);
    REQUIRE_EQ(out_len, strlen(cases[i].expected));
    REQUIRE_EQ(memcmp(dec, cases[i].expected, out_len), 0);
    free(dec);
  }
}

TEST(base64_decode, null_input_fails) {
  REQUIRE_EQ((void *)chttp_base64_decode(NULL, NULL), NULL);
}

TEST(base64_decode, length_not_multiple_of_four_fails) {
  REQUIRE_EQ((void *)chttp_base64_decode("Zg=", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("A", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("AAAAA", NULL), NULL);
}

TEST(base64_decode, invalid_character_fails) {
  REQUIRE_EQ((void *)chttp_base64_decode("AB!D", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("AB D", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("AB\nD", NULL), NULL);
}

TEST(base64_decode, misplaced_padding_fails) {
  REQUIRE_EQ((void *)chttp_base64_decode("=AAA", NULL), NULL);  // pad at k=0
  REQUIRE_EQ((void *)chttp_base64_decode("A=AA", NULL), NULL);  // pad at k=1
  REQUIRE_EQ((void *)chttp_base64_decode("AB=A", NULL),
             NULL);  // data after pad
  REQUIRE_EQ((void *)chttp_base64_decode("A===", NULL),
             NULL);  // pad at k=1 in last group
  REQUIRE_EQ((void *)chttp_base64_decode("AAA=BBBB", NULL),
             NULL);  // pad in non-last group
}

TEST(base64_decode, valid_padding_variants_succeed) {
  size_t out_len = (size_t)-1;
  char *dec = (char *)chttp_base64_decode("AB==", &out_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(out_len, (size_t)1);
  free(dec);

  dec = (char *)chttp_base64_decode("ABC=", &out_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(out_len, (size_t)2);
  free(dec);
}

TEST(base64_decode, out_len_is_optional) {
  char *dec = (char *)chttp_base64_decode("Zm9vYmFy", NULL);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(memcmp(dec, "foobar", 6), 0);
  free(dec);
}

/* ========================================================================== */
/*                     BASIC AUTH; RFC 7617                                 */
/* ========================================================================== */

TEST(basic_auth, rfc7617_example) {
  /* The canonical RFC 7617 example. */
  char *auth = chttp_basic_auth("Aladdin", "open sesame");
  REQUIRE_NE((void *)auth, NULL);
  REQUIRE_STREQ(auth, "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
  free(auth);
}

TEST(basic_auth, round_trips_through_decode) {
  char *auth = chttp_basic_auth("user", "pass");
  REQUIRE_NE((void *)auth, NULL);
  REQUIRE_TRUE(strncmp(auth, "Basic ", 6) == 0);

  size_t dec_len = 0;
  char *dec = (char *)chttp_base64_decode(auth + 6, &dec_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(dec_len, strlen("user:pass"));
  REQUIRE_EQ(memcmp(dec, "user:pass", dec_len), 0);
  free(auth);
  free(dec);
}

TEST(basic_auth, empty_username_and_password_succeeds) {
  char *auth = chttp_basic_auth("", "");
  REQUIRE_NE((void *)auth, NULL);

  size_t dec_len = 0;
  char *dec = (char *)chttp_base64_decode(auth + 6, &dec_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(dec_len, (size_t)1);
  REQUIRE_EQ(dec[0], ':');
  free(auth);
  free(dec);
}

TEST(basic_auth, null_username_or_password_fails) {
  REQUIRE_EQ((void *)chttp_basic_auth(NULL, "pass"), NULL);
  REQUIRE_EQ((void *)chttp_basic_auth("user", NULL), NULL);
  REQUIRE_EQ((void *)chttp_basic_auth(NULL, NULL), NULL);
}

/* ========================================================================== */
/*                     CUSTOM ALLOCATOR PLUMBING                              */
/* ========================================================================== */

static atomic_int g_alloc_count = 0;
static atomic_int g_free_count = 0;

static void *tracked_malloc(size_t sz) {
  atomic_fetch_add(&g_alloc_count, 1);
  return malloc(sz);
}
static void tracked_free(void *p) {
  if (p) atomic_fetch_add(&g_free_count, 1);
  free(p);
}
static void *tracked_calloc(size_t n, size_t sz) {
  atomic_fetch_add(&g_alloc_count, 1);
  return calloc(n, sz);
}
static void *tracked_realloc(void *p, size_t sz) {
  if (p) atomic_fetch_add(&g_free_count, 1);
  atomic_fetch_add(&g_alloc_count, 1);
  return realloc(p, sz);
}

TEST(custom_allocator, encode_decode_and_basic_auth_go_through_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};

  char *enc = chttp_base64_encode_mp(&mp, "foobar", 6, NULL);
  REQUIRE_NE((void *)enc, NULL);
  char *dec = (char *)chttp_base64_decode_mp(&mp, enc, NULL);
  REQUIRE_NE((void *)dec, NULL);
  char *auth = chttp_basic_auth_mp(&mp, "user", "pass");
  REQUIRE_NE((void *)auth, NULL);

  tracked_free(enc);
  tracked_free(dec);
  tracked_free(auth);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* ========================================================================== */
/*                     OOM INJECTION                                          */
/* ========================================================================== */

static int g_fail_at_call = -1; /* -1 = never fail */
static atomic_int g_call_index;

static void *counting_malloc(size_t sz) {
  int idx = atomic_fetch_add(&g_call_index, 1);
  if (g_fail_at_call >= 0 && idx == g_fail_at_call) return NULL;
  return malloc(sz);
}

static ccol_memmgmt_procs_t g_counting_mp = {.malloc = counting_malloc,
                                             .free = free,
                                             .calloc = calloc,
                                             .realloc = realloc};

TEST(oom, base64_encode_alloc_failure) {
  atomic_store(&g_call_index, 0);
  g_fail_at_call = 0;
  REQUIRE_EQ((void *)chttp_base64_encode_mp(&g_counting_mp, "foobar", 6, NULL),
             NULL);
  g_fail_at_call = -1;
}

TEST(oom, base64_decode_alloc_failure) {
  atomic_store(&g_call_index, 0);
  g_fail_at_call = 0;
  REQUIRE_EQ((void *)chttp_base64_decode_mp(&g_counting_mp, "Zm9vYmFy", NULL),
             NULL);
  g_fail_at_call = -1;
}

TEST(oom, base64_decode_empty_string_alloc_failure) {
  atomic_store(&g_call_index, 0);
  g_fail_at_call = 0;
  REQUIRE_EQ((void *)chttp_base64_decode_mp(&g_counting_mp, "", NULL), NULL);
  g_fail_at_call = -1;
}

TEST(oom, basic_auth_fails_at_every_allocation_site) {
  /* chttp_basic_auth_mp performs exactly 3 allocations: the combined
   * "user:pass" buffer, chttp_base64_encode_mp's own output buffer, and the
   * final "Basic <b64>" buffer. Fail each one in turn and confirm a clean
   * NULL return (and, under the memtest target, no leak). */
  for (int fail_idx = 0; fail_idx < 3; fail_idx++) {
    atomic_store(&g_call_index, 0);
    g_fail_at_call = fail_idx;
    REQUIRE_EQ((void *)chttp_basic_auth_mp(&g_counting_mp, "user", "pass"),
               NULL);
  }
  g_fail_at_call = -1;
}
