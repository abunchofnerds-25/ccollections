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

#include <chttp.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/chttp1_parser.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#if defined(TEST_NO_LD_WRAP)
#include <dlfcn.h>
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                     BASE64 ENCODE: RFC 4648 TEST VECTORS                  */
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
  /* enc_len = ((len+2)/3)*4 needs an overflow check before the code
   * allocates enc_len+1 bytes. Without that check, a len from the caller
   * that is large enough to wrap size_t goes through. The allocation is then
   * tiny, because it wrapped, and the code writes far past it.
   *
   * len is close to SIZE_MAX here on purpose. That is well above anything
   * that a real caller asks this function to encode. This test passes a real
   * pointer, which is not NULL and which nothing dereferences. The rejection
   * must happen before anything touches *data*, and not only before the
   * allocation. */
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
/*                     BASE64 DECODE: RFC 4648 TEST VECTORS                  */
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
  char *dec = (char *)chttp_base64_decode("AQ==", &out_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(out_len, (size_t)1);
  REQUIRE_EQ((unsigned char)dec[0], 0x01);
  free(dec);

  dec = (char *)chttp_base64_decode("ABA=", &out_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(out_len, (size_t)2);
  REQUIRE_EQ((unsigned char)dec[0], 0x00);
  REQUIRE_EQ((unsigned char)dec[1], 0x10);
  free(dec);
}

/* RFC 4648 SS3.5: the bits of a padded final group that no output byte
 * holds must be zero. "QQ==" is the one spelling of "A"; "QR==" sets a bit
 * that the decoded byte does not carry, and so do "AB==" and "ABC=". */
TEST(base64_decode, nonzero_padding_bits_fail) {
  size_t out_len = (size_t)-1;
  char *dec = (char *)chttp_base64_decode("QQ==", &out_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(out_len, (size_t)1);
  REQUIRE_EQ(dec[0], 'A');
  free(dec);
  REQUIRE_EQ((void *)chttp_base64_decode("QR==", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("AB==", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("ABC=", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("QUJ=", NULL), NULL);
  REQUIRE_EQ((void *)chttp_base64_decode("Zm9vYh==", NULL), NULL);
}

/* Every final group with padding decodes exactly when the bits that no
 * output byte holds are zero, and what it decodes to encodes back to the
 * same text. The sweep covers every "XY==" and every "XYZ=". */
TEST(base64_decode, only_the_canonical_final_group_decodes) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t accepted_one = 0, accepted_two = 0;
  bool mismatch = false;
  for (int a = 0; a < 64; a++) {
    for (int b = 0; b < 64; b++) {
      for (int c = -1; c < 64; c++) {
        char probe[5] = {alphabet[a], alphabet[b], c < 0 ? '=' : alphabet[c],
                         '=', '\0'};
        bool canonical = c < 0 ? (b & 0x0F) == 0 : (c & 0x03) == 0;
        size_t n = 0;
        unsigned char *dec = (unsigned char *)chttp_base64_decode(probe, &n);
        if ((dec != NULL) != canonical) mismatch = true;
        if (dec) {
          if (n != (c < 0 ? 1u : 2u)) mismatch = true;
          char *enc = chttp_base64_encode(dec, n, NULL);
          if (!enc || strcmp(enc, probe) != 0) mismatch = true;
          free(enc);
          if (c < 0)
            accepted_one++;
          else
            accepted_two++;
        }
        free(dec);
      }
    }
  }
  REQUIRE_FALSE(mismatch);
  REQUIRE_EQ(accepted_one, (size_t)256);
  REQUIRE_EQ(accepted_two, (size_t)65536);
}

TEST(base64_decode, out_len_is_optional) {
  char *dec = (char *)chttp_base64_decode("Zm9vYmFy", NULL);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(memcmp(dec, "foobar", 6), 0);
  free(dec);
}

/* ========================================================================== */
/*                     BASIC AUTH: RFC 7617                                  */
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

TEST(basic_auth, username_containing_a_colon_is_rejected) {
  /* RFC 7617 SS2 makes the colon the only delimiter of the encoded
   * "user-id:password" string. A recipient splits at the FIRST colon. A
   * user-id that holds one therefore cannot travel as itself. Without this
   * rejection, the pair "user:with:colons" and "pw" encodes a credential
   * that decodes to the user-id "user" and the password "with:colons:pw".
   * That is a different identity from the one that the caller asked
   * for. */
  REQUIRE_EQ((void *)chttp_basic_auth("user:with:colons", "pw"), NULL);
  REQUIRE_EQ((void *)chttp_basic_auth(":", ""), NULL);
  REQUIRE_EQ((void *)chttp_basic_auth("trailing:", "pw"), NULL);
  REQUIRE_EQ((void *)chttp_basic_auth_mp(NULL, "user:name", "pw"), NULL);
}

TEST(basic_auth, two_distinct_credentials_cannot_encode_identically) {
  /* A colon joins the two parts. The pair "admin:x" and "" then gives the
   * same byte string as the pair "admin" and "x:". To accept the first pair
   * would make one header value stand for two different credentials that
   * callers meant. The code therefore refuses the username that holds a
   * colon. It builds the password that holds a colon as the caller asked. */
  REQUIRE_EQ((void *)chttp_basic_auth("admin:x", ""), NULL);

  char *auth = chttp_basic_auth("admin", "x:");
  REQUIRE_NE((void *)auth, NULL);
  size_t dec_len = 0;
  char *dec = (char *)chttp_base64_decode(auth + 6, &dec_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(dec_len, strlen("admin:x:"));
  REQUIRE_EQ(memcmp(dec, "admin:x:", dec_len), 0);
  free(auth);
  free(dec);
}

TEST(basic_auth, password_containing_colons_round_trips) {
  /* Everything after the first colon is the password. A password full of
   * colons therefore needs no escaping, and it must keep working. */
  char *auth = chttp_basic_auth("user", "pa:ss:word");
  REQUIRE_NE((void *)auth, NULL);

  size_t dec_len = 0;
  char *dec = (char *)chttp_base64_decode(auth + 6, &dec_len);
  REQUIRE_NE((void *)dec, NULL);
  REQUIRE_EQ(dec_len, strlen("user:pa:ss:word"));
  REQUIRE_EQ(memcmp(dec, "user:pa:ss:word", dec_len), 0);
  free(auth);
  free(dec);
}

extern bool _chttp_basic_auth_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                       const char *username,
                                                       size_t fake_user_len,
                                                       const char *password,
                                                       size_t fake_pass_len);

TEST(basic_auth, length_sum_large_enough_to_overflow_is_rejected) {
  /* combined_len = user_len + 1 + pass_len needs an overflow check before
   * the code allocates combined_len+1 bytes. Without that check, two
   * separate lengths from the caller that sum to near SIZE_MAX go through.
   * The allocation then wraps, and the code writes far past it.
   *
   * Both fake lengths are huge on purpose, and not only one of them. That
   * follows the pattern of
   * base64_encode.length_large_enough_to_overflow_is_rejected. This test
   * passes real, short strings that are not NULL. The rejection must happen
   * before any call to snprintf, and not only before the allocation. */
  bool rejected = _chttp_basic_auth_overflow_guard_for_tests(
      NULL, "user", SIZE_MAX / 2, "pass", SIZE_MAX / 2);
  REQUIRE_TRUE(rejected);
}

TEST(basic_auth, ordinary_small_lengths_still_work) {
  /* The same helper, with real strings that are nowhere near an overflow,
   * and with their own real lengths. This confirms that the guard above
   * reports no problem for a username and a password of a real length. */
  bool rejected = _chttp_basic_auth_overflow_guard_for_tests(
      NULL, "user", strlen("user"), "pass", strlen("pass"));
  REQUIRE_FALSE(rejected);
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
  /* chttp_basic_auth_mp makes exactly 3 allocations. The first is the
   * combined "user:pass" buffer. The second is the output buffer of
   * chttp_base64_encode_mp. The third is the final "Basic <b64>" buffer.
   * This test fails each one in turn. It confirms a clean NULL return, and,
   * under the memtest target, no leak. */
  for (int fail_idx = 0; fail_idx < 3; fail_idx++) {
    atomic_store(&g_call_index, 0);
    g_fail_at_call = fail_idx;
    REQUIRE_EQ((void *)chttp_basic_auth_mp(&g_counting_mp, "user", "pass"),
               NULL);
  }
  g_fail_at_call = -1;
}

/* ========================================================================== */
/*                         chttp_method_str */
/* ========================================================================== */

/* Every method name that the library puts on the wire, or into a log line,
 * comes from here. An index at run time drives this test, so the switch
 * really runs. */
TEST(chttp_method_str, every_method_maps_to_its_own_spelling) {
  static const struct {
    chttp_method_t m;
    const char *name;
  } cases[] = {
      {CHTTP_GET, "GET"},         {CHTTP_POST, "POST"},   {CHTTP_PUT, "PUT"},
      {CHTTP_DELETE, "DELETE"},   {CHTTP_PATCH, "PATCH"}, {CHTTP_HEAD, "HEAD"},
      {CHTTP_OPTIONS, "OPTIONS"}, {CHTTP_ANY, "ANY"},
  };

  bool all_named = true;
  for (volatile size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    if (strcmp(chttp_method_str(cases[i].m), cases[i].name) != 0)
      all_named = false;

  REQUIRE_TRUE(all_named);
}

TEST(chttp_method_str, an_out_of_range_method_reports_unknown) {
  /* chttp_method_t is a plain enum, so a caller can hand it any integer and
   * the fallback has to hold. */
  volatile chttp_method_t bogus = (chttp_method_t)9999;
  REQUIRE_STREQ(chttp_method_str(bogus), "UNKNOWN");
}

/* ========================================================================== */
/*               chttp1_parser: FRAMING AND CONNECTION RULES                  */
/* ========================================================================== */

/* These cases drive the internal HTTP/1.1 parser that chttpserver, in
 * request mode, and chttpclient, in response mode, both sit on. Every case
 * runs twice: once with the whole message in one call, and once with one
 * byte per call. A rule that holds only when a boundary falls in a
 * particular place is not a rule. */

typedef struct {
  char body[256];
  size_t body_len;
} _p1_ctx_t;

static int _p1_on_body(chttp1_parser_t *p, const char *at, size_t len) {
  _p1_ctx_t *c = (_p1_ctx_t *)p->data;
  if (c->body_len + len > sizeof(c->body)) return 1;
  memcpy(c->body + c->body_len, at, len);
  c->body_len += len;
  return 0;
}

/* Runs msg through a fresh parser of the given mode. Reports the first
 * result that is not CHTTP1_OK, or CHTTP1_OK when the input ran out first.
 * *consumed_out receives how many bytes of msg the parser used up to that
 * result. */
static chttp1_errno_t _p1_run(chttp1_parser_t *p, chttp1_settings_t *st,
                              _p1_ctx_t *c, bool request, const char *msg,
                              size_t len, bool bytewise, size_t *consumed_out) {
  memset(c, 0, sizeof(*c));
  chttp1_settings_init(st);
  st->on_body = _p1_on_body;
  if (request)
    chttp1_parser_init_request(p, st);
  else
    chttp1_parser_init(p, st);
  p->data = c;
  size_t used = 0;
  chttp1_errno_t r = CHTTP1_OK;
  if (bytewise) {
    for (size_t i = 0; i < len; i++) {
      r = chttp1_parser_execute(p, msg + i, 1);
      if (r != CHTTP1_OK) {
        used = i + (r == CHTTP1_PAUSED ? chttp1_parser_consumed(p) : 1);
        break;
      }
      used = i + 1;
    }
  } else {
    r = chttp1_parser_execute(p, msg, len);
    used = (r == CHTTP1_PAUSED) ? chttp1_parser_consumed(p) : len;
  }
  if (consumed_out) *consumed_out = used;
  return r;
}

#define P1_LEN(s) (sizeof(s) - 1)

TEST(chttp1_framing, http_1_0_request_with_chunked_te_is_refused) {
  /* The body below is chunked and a second request follows it. A recipient
   * that de-chunks and keeps the connection reads that second request as a
   * request of its own. */
  static const char msg[] =
      "POST /a HTTP/1.0\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
      "Connection: keep-alive\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n"
      "GET /admin HTTP/1.1\r\nHost: h\r\n\r\n";
  for (int bw = 0; bw < 2; bw++) {
    chttp1_parser_t p;
    chttp1_settings_t st;
    _p1_ctx_t c;
    chttp1_errno_t r = _p1_run(&p, &st, &c, true, msg, P1_LEN(msg), bw, NULL);
    REQUIRE_EQ((int)r, (int)CHTTP1_ERROR);
    REQUIRE_STREQ(p.reason, "Transfer-Encoding in an HTTP/1.0 request");
    REQUIRE_EQ(c.body_len, (size_t)0);
  }
}

TEST(chttp1_framing, http_1_1_request_with_chunked_te_is_accepted) {
  static const char msg[] =
      "POST /a HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  for (int bw = 0; bw < 2; bw++) {
    chttp1_parser_t p;
    chttp1_settings_t st;
    _p1_ctx_t c;
    chttp1_errno_t r = _p1_run(&p, &st, &c, true, msg, P1_LEN(msg), bw, NULL);
    REQUIRE_EQ((int)r, (int)CHTTP1_PAUSED);
    REQUIRE_EQ(c.body_len, (size_t)5);
    REQUIRE_TRUE(chttp1_should_keep_alive(&p));
  }
}

TEST(chttp1_framing,
     http_1_0_response_with_chunked_te_is_processed_then_closed) {
  static const char msg[] =
      "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "Connection: keep-alive\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n"
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nbad";
  for (int bw = 0; bw < 2; bw++) {
    chttp1_parser_t p;
    chttp1_settings_t st;
    _p1_ctx_t c;
    size_t used = 0;
    chttp1_errno_t r = _p1_run(&p, &st, &c, false, msg, P1_LEN(msg), bw, &used);
    REQUIRE_EQ((int)r, (int)CHTTP1_PAUSED);
    REQUIRE_EQ(c.body_len, (size_t)5);
    REQUIRE_EQ(memcmp(c.body, "hello", 5), 0);
    REQUIRE_LT(used, P1_LEN(msg));
    REQUIRE_FALSE(chttp1_should_keep_alive(&p));
  }
}

TEST(chttp1_framing, http_1_1_response_with_chunked_te_keeps_alive) {
  static const char msg[] =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  for (int bw = 0; bw < 2; bw++) {
    chttp1_parser_t p;
    chttp1_settings_t st;
    _p1_ctx_t c;
    chttp1_errno_t r = _p1_run(&p, &st, &c, false, msg, P1_LEN(msg), bw, NULL);
    REQUIRE_EQ((int)r, (int)CHTTP1_PAUSED);
    REQUIRE_TRUE(chttp1_should_keep_alive(&p));
  }
}

/* One row of the Connection table below: the header lines, and whether the
 * message keeps the connection alive. */
typedef struct {
  const char *version;
  const char *conn_lines;
  bool keep_alive;
} _p1_conn_case_t;

static const _p1_conn_case_t _p1_conn_cases[] = {
    {"1.0", "Connection: keep-alive\r\n", true},
    {"1.0", "", false},
    {"1.0", "Connection: keep-alive, close\r\n", false},
    {"1.0", "Connection: close, keep-alive\r\n", false},
    {"1.0", "Connection: keep-alive\r\nConnection: close\r\n", false},
    {"1.0", "Connection: close\r\nConnection: keep-alive\r\n", false},
    {"1.1", "", true},
    {"1.1", "Connection: keep-alive\r\n", true},
    {"1.1", "Connection: keep-alive, close\r\n", false},
    {"1.1", "Connection: keep-alive\r\nConnection: close\r\n", false},
};

TEST(chttp1_framing, close_wins_over_keep_alive_for_every_version) {
  for (size_t i = 0; i < sizeof(_p1_conn_cases) / sizeof(_p1_conn_cases[0]);
       i++) {
    for (int request = 0; request < 2; request++) {
      char msg[256];
      int n = request
                  ? snprintf(
                        msg, sizeof(msg), "GET / HTTP/%s\r\nHost: h\r\n%s\r\n",
                        _p1_conn_cases[i].version, _p1_conn_cases[i].conn_lines)
                  : snprintf(msg, sizeof(msg),
                             "HTTP/%s 200 OK\r\nContent-Length: 0\r\n%s\r\n",
                             _p1_conn_cases[i].version,
                             _p1_conn_cases[i].conn_lines);
      REQUIRE_GT(n, 0);
      for (int bw = 0; bw < 2; bw++) {
        chttp1_parser_t p;
        chttp1_settings_t st;
        _p1_ctx_t c;
        chttp1_errno_t r =
            _p1_run(&p, &st, &c, request, msg, (size_t)n, bw, NULL);
        REQUIRE_EQ((int)r, (int)CHTTP1_PAUSED);
        REQUIRE_EQ((int)chttp1_should_keep_alive(&p),
                   (int)_p1_conn_cases[i].keep_alive);
      }
    }
  }
}

/* One chunk-size line, with its CRLF, that must be refused or accepted.
 * ok is the answer of request mode and resp_ok the answer of response mode;
 * the two differ only for whitespace between the size and the CRLF. */
typedef struct {
  const char *line;
  size_t len; /* explicit, so that a line can hold a NUL */
  bool ok;
  bool resp_ok;
} _p1_chunk_case_t;

#define P1_CHUNK(s, ok) {s, sizeof(s) - 1, ok, ok}
#define P1_CHUNK_RESP_ONLY(s) {s, sizeof(s) - 1, false, true}

static const _p1_chunk_case_t _p1_chunk_cases[] = {
    P1_CHUNK("5\r\n", true),
    P1_CHUNK("5;a\r\n", true),
    P1_CHUNK("5;a=b\r\n", true),
    P1_CHUNK("5;a=b;c\r\n", true),
    P1_CHUNK("5 ; a = b\r\n", true),
    P1_CHUNK("5;a=\"q u\\\"o\"\r\n", true),
    P1_CHUNK("5;a\rb\r\n", false),
    P1_CHUNK("5;a\0b\r\n", false),
    P1_CHUNK("5;a=b\rc\r\n", false),
    P1_CHUNK("5;a=\"x\ry\"\r\n", false),
    P1_CHUNK("5;a=\"x\\\ry\"\r\n", false),
    P1_CHUNK("5;a=\"x\0y\"\r\n", false),
    P1_CHUNK("5;\r\n", false),
    P1_CHUNK("5;=b\r\n", false),
    P1_CHUNK("5;a=\r\n", false),
    P1_CHUNK("5;a=\"open\r\n", false),
    P1_CHUNK("5;a b\r\n", false),
    P1_CHUNK("5;a\x7f\r\n", false),
    P1_CHUNK_RESP_ONLY("5 \r\n"),
    P1_CHUNK_RESP_ONLY("5\t\r\n"),
    P1_CHUNK_RESP_ONLY("5 \t \r\n"),
    P1_CHUNK("5 x\r\n", false),
    P1_CHUNK("5 ;\r\n", false),
    P1_CHUNK("5;a=b \r\n", false),
};

TEST(chttp1_framing, chunk_extension_follows_the_rfc_9112_grammar) {
  static const char req_head[] =
      "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  static const char resp_head[] =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  static const char tail[] = "hello\r\n0\r\n\r\n";
  for (size_t i = 0; i < sizeof(_p1_chunk_cases) / sizeof(_p1_chunk_cases[0]);
       i++) {
    for (int request = 0; request < 2; request++) {
      const char *head = request ? req_head : resp_head;
      size_t head_len = request ? P1_LEN(req_head) : P1_LEN(resp_head);
      char msg[512];
      size_t len = 0;
      memcpy(msg + len, head, head_len);
      len += head_len;
      memcpy(msg + len, _p1_chunk_cases[i].line, _p1_chunk_cases[i].len);
      len += _p1_chunk_cases[i].len;
      memcpy(msg + len, tail, P1_LEN(tail));
      len += P1_LEN(tail);
      for (int bw = 0; bw < 2; bw++) {
        chttp1_parser_t p;
        chttp1_settings_t st;
        _p1_ctx_t c;
        chttp1_errno_t r = _p1_run(&p, &st, &c, request, msg, len, bw, NULL);
        if (request ? _p1_chunk_cases[i].ok : _p1_chunk_cases[i].resp_ok) {
          REQUIRE_EQ((int)r, (int)CHTTP1_PAUSED);
          REQUIRE_EQ(c.body_len, (size_t)5);
        } else {
          REQUIRE_EQ((int)r, (int)CHTTP1_ERROR);
        }
      }
    }
  }
}

/* ========================================================================== */
/*               chttp1_parser: WHAT A BODY STILL OWES                        */
/* ========================================================================== */

TEST(chttp1_framing, body_bytes_still_expected_follows_the_framing) {
  /* A server that waits for the rest of a body asks the kernel to wake it
   * only once this many bytes are queued, so the answer must never exceed
   * what the framing guarantees. */
  chttp1_parser_t p;
  chttp1_settings_t st;
  _p1_ctx_t c;
  static const char cl_head[] =
      "POST /a HTTP/1.1\r\nHost: h\r\nContent-Length: 10\r\n\r\n";
  REQUIRE_EQ(
      (int)_p1_run(&p, &st, &c, true, cl_head, P1_LEN(cl_head), false, NULL),
      (int)CHTTP1_OK);
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)10);
  REQUIRE_EQ((int)chttp1_parser_execute(&p, "abc", 3), (int)CHTTP1_OK);
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)7);
  REQUIRE_EQ((int)chttp1_parser_execute(&p, "defghij", 7), (int)CHTTP1_PAUSED);
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)0);

  static const char te_head[] =
      "POST /a HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(
      (int)_p1_run(&p, &st, &c, true, te_head, P1_LEN(te_head), false, NULL),
      (int)CHTTP1_OK);
  /* On a chunk-size line the next byte can be the last one of the body. */
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)0);
  REQUIRE_EQ((int)chttp1_parser_execute(&p, "5\r\nab", 5), (int)CHTTP1_OK);
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)3);
  REQUIRE_EQ((int)chttp1_parser_execute(&p, "cde", 3), (int)CHTTP1_OK);
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)0);
  REQUIRE_EQ((int)chttp1_parser_execute(&p, "\r\n0\r\n", 5), (int)CHTTP1_OK);
  /* In the trailers too. */
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)0);
  REQUIRE_EQ((int)chttp1_parser_execute(&p, "\r\n", 2), (int)CHTTP1_PAUSED);
  REQUIRE_EQ(chttp1_body_bytes_still_expected(&p), (uint64_t)0);
  REQUIRE_EQ(chttp1_body_bytes_still_expected(NULL), (uint64_t)0);
}

static int _p1_refuse_headers(chttp1_parser_t *p) {
  (void)p;
  return -1;
}

TEST(chttp1_framing, request_bytes_may_remain_follows_the_framing) {
  /* A server closes with a lingering close exactly when this answers true,
   * so it must be false only where nothing more of the message can come. */
  chttp1_parser_t p;
  chttp1_settings_t st;
  chttp1_settings_init(&st);
  /* In the head: the framing is unknown. */
  chttp1_parser_init_request(&p, &st);
  static const char part[] = "POST /a HTTP/1.1\r\nHost: h\r\n";
  chttp1_errno_t r0 = chttp1_parser_execute(&p, part, P1_LEN(part));
  bool in_head = chttp1_request_bytes_may_remain(&p);
  /* A request refused at the end of its head: true only when its framing
   * announces a body. */
  static const char *heads[4] = {
      "GET /a HTTP/1.1\r\nHost: h\r\n\r\n",
      "POST /a HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n",
      "POST /a HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\n",
      "POST /a HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"};
  bool refused[4];
  chttp1_errno_t rr[4];
  st.on_headers_complete = _p1_refuse_headers;
  for (int i = 0; i < 4; i++) {
    chttp1_parser_init_request(&p, &st);
    rr[i] = chttp1_parser_execute(&p, heads[i], strlen(heads[i]));
    refused[i] = chttp1_request_bytes_may_remain(&p);
  }
  /* A complete message: false. */
  chttp1_settings_init(&st);
  chttp1_parser_init_request(&p, &st);
  static const char whole[] =
      "POST /a HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\nab";
  chttp1_errno_t r1 = chttp1_parser_execute(&p, whole, P1_LEN(whole));
  bool done = chttp1_request_bytes_may_remain(&p);
  /* A body cut short: true. */
  chttp1_parser_init_request(&p, &st);
  chttp1_errno_t r2 = chttp1_parser_execute(&p, whole, P1_LEN(whole) - 1);
  bool mid_body = chttp1_request_bytes_may_remain(&p);
  REQUIRE_EQ((int)r0, (int)CHTTP1_OK);
  REQUIRE_TRUE(in_head);
  for (int i = 0; i < 4; i++) REQUIRE_EQ((int)rr[i], (int)CHTTP1_USER);
  REQUIRE_FALSE(refused[0]);
  REQUIRE_FALSE(refused[1]);
  REQUIRE_TRUE(refused[2]);
  REQUIRE_TRUE(refused[3]);
  REQUIRE_EQ((int)r1, (int)CHTTP1_PAUSED);
  REQUIRE_FALSE(done);
  REQUIRE_EQ((int)r2, (int)CHTTP1_OK);
  REQUIRE_TRUE(mid_body);
  REQUIRE_FALSE(chttp1_request_bytes_may_remain(NULL));
}

TEST(chttp1_stream, a_nonblocking_write_reports_a_full_socket_as_a_timeout) {
  /* write_nonblocking turns every write into "now or never": once the
   * socket is full, the call returns -1 with chttp1_stream_timed_out() true
   * at once, whatever timeout the caller passed. The fd itself is blocking
   * and each chunk is larger than the socket buffer, so the attempt after a
   * successful poll must not block either: POLLOUT means some room, not room
   * for the whole chunk. Non-vacuous: without the flag, the last call waits
   * out its whole 3 s timeout; without MSG_DONTWAIT on the send, the first
   * call blocks until SO_SNDTIMEO (5 s, the safety net that keeps a
   * regression from hanging the suite) gives up. */
  int fds[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  struct timeval sndtimeo = {.tv_sec = 5};
  setsockopt(fds[0], SOL_SOCKET, SO_SNDTIMEO, &sndtimeo, sizeof(sndtimeo));
  chttp1_stream_t s;
  bool prepared = chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL);
  s.write_nonblocking = true;
  static char chunk[1 << 20];
  memset(chunk, 'n', sizeof(chunk));
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ssize_t w = 0;
  for (int i = 0; prepared && i < 10000; i++) {
    w = chttp1_stream_write(&s, chunk, sizeof(chunk), 3000);
    if (w < 0) break;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  bool timed_out = chttp1_stream_timed_out(&s);
  long ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 +
            (t1.tv_nsec - t0.tv_nsec) / 1000000;
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_TRUE(prepared);
  REQUIRE_EQ(w, (ssize_t)-1);
  REQUIRE_TRUE(timed_out);
  REQUIRE_LT(ms, 2000L);
}

TEST(chttp1_stream, writev2_keeps_the_byte_order_across_short_writes) {
  /* The two parts go out as one stream of bytes. A short write can end
   * inside either part, and a caller that resumes from the count it got
   * back must reproduce the original bytes exactly. */
  int fds[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  fcntl(fds[1], F_SETFL, O_NONBLOCK);
  chttp1_stream_t s;
  bool prepared = chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL);
  s.fd_nonblocking = true;
  s.write_nonblocking = true;
  enum { A = 3000, B = 300000 };
  static char a[A], b[B], got[A + B];
  for (int i = 0; i < A; i++) a[i] = (char)('A' + i % 26);
  for (int i = 0; i < B; i++) b[i] = (char)('a' + i % 23);
  size_t sent = 0, recvd = 0;
  int blocked = 0;
  for (int guard = 0; prepared && recvd < A + B && guard < 100000; guard++) {
    if (sent < A + B) {
      size_t alen = sent < A ? A - sent : 0;
      size_t boff = sent > A ? sent - A : 0;
      ssize_t n = chttp1_stream_writev2(&s, a + (A - alen), alen, b + boff,
                                        B - boff, 1000);
      if (n > 0) {
        sent += (size_t)n;
        continue; /* read only once the socket is full */
      }
      if (chttp1_stream_timed_out(&s)) blocked++;
    }
    ssize_t r = read(fds[1], got + recvd, A + B - recvd);
    if (r > 0) recvd += (size_t)r;
  }
  bool same =
      recvd == A + B && memcmp(got, a, A) == 0 && memcmp(got + A, b, B) == 0;
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_TRUE(prepared);
  REQUIRE_GT(blocked, 0);
  REQUIRE_TRUE(same);
}

/* The link routes send(2) and read(2) through these two wrappers (see the
 * Makefile). Each passes straight through unless a test armed it, in which
 * case exactly one call reports EINTR without touching the socket, which is
 * what a signal that arrives before the call moves a byte produces. */
#if defined(TEST_NO_LD_WRAP)
/* The linker has no --wrap. send and read below take every call that this
 * binary makes, and the original functions come from the C library through
 * dlsym(RTLD_NEXT). */
static ssize_t __real_send(int fd, const void *buf, size_t len, int flags) {
  static ssize_t (*_Atomic fn)(int, const void *, size_t, int);
  if (!fn)
    fn = (ssize_t(*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
  return fn(fd, buf, len, flags);
}
static ssize_t __real_read(int fd, void *buf, size_t len) {
  static ssize_t (*_Atomic fn)(int, void *, size_t);
  if (!fn) fn = (ssize_t(*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
  return fn(fd, buf, len);
}
#else
ssize_t __real_send(int fd, const void *buf, size_t len, int flags);
ssize_t __real_read(int fd, void *buf, size_t len);
#endif
static atomic_int _p1_send_eintr_armed;
static atomic_int _p1_read_eintr_armed;

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags);
ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags) {
  if (atomic_exchange(&_p1_send_eintr_armed, 0)) {
    errno = EINTR;
    return -1;
  }
  return __real_send(fd, buf, len, flags);
}

ssize_t __wrap_read(int fd, void *buf, size_t len);
ssize_t __wrap_read(int fd, void *buf, size_t len) {
  if (atomic_exchange(&_p1_read_eintr_armed, 0)) {
    errno = EINTR;
    return -1;
  }
  return __real_read(fd, buf, len);
}

#if defined(TEST_NO_LD_WRAP)
ssize_t send(int fd, const void *buf, size_t len, int flags) {
  return __wrap_send(fd, buf, len, flags);
}
ssize_t read(int fd, void *buf, size_t len) {
  return __wrap_read(fd, buf, len);
}
#endif

TEST(chttp1_stream, a_write_interrupted_by_a_signal_is_retried) {
  /* Non-vacuous: without the EINTR retry, the first write returns -1 with
   * last_errno EINTR and the peer receives nothing. A write can give a short
   * count (the macOS path of csock.h gives at most SO_SNDLOWAT bytes to a
   * blocking socket, and Linux reports that value as 1), so the test writes
   * until the 5 bytes are out. */
  int fds[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  chttp1_stream_t s;
  bool prepared = chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL);
  atomic_store(&_p1_send_eintr_armed, 1);
  static const char hello[] = "hello";
  ssize_t w = prepared ? 0 : -2;
  while (prepared && w >= 0 && w < 5) {
    ssize_t n = chttp1_stream_write(&s, hello + w, 5 - (size_t)w, 1000);
    w = n < 0 ? -1 : w + n;
  }
  int still_armed = atomic_exchange(&_p1_send_eintr_armed, 0);
  char got[8] = {0};
  ssize_t r = w == 5 ? __real_read(fds[1], got, sizeof(got)) : -1;
  if (prepared) chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_TRUE(prepared);
  REQUIRE_EQ(still_armed, 0);
  REQUIRE_EQ(w, (ssize_t)5);
  REQUIRE_EQ(r, (ssize_t)5);
  REQUIRE_EQ(memcmp(got, "hello", 5), 0);
}

TEST(chttp1_stream, a_blocking_read_interrupted_by_a_signal_is_retried) {
  /* The stream is not marked fd_nonblocking, so the read waits in poll(2)
   * and then reads once. Non-vacuous: without the EINTR retry, the read
   * returns -1 with last_errno EINTR although the bytes are queued. */
  int fds[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  REQUIRE_EQ(__real_send(fds[1], "world", 5, MSG_NOSIGNAL), (ssize_t)5);
  chttp1_stream_t s;
  bool prepared = chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL);
  atomic_store(&_p1_read_eintr_armed, 1);
  char got[8] = {0};
  ssize_t r = prepared ? chttp1_stream_read(&s, got, sizeof(got), 1000) : -2;
  int still_armed = atomic_exchange(&_p1_read_eintr_armed, 0);
  if (prepared) chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_TRUE(prepared);
  REQUIRE_EQ(still_armed, 0);
  REQUIRE_EQ(r, (ssize_t)5);
  REQUIRE_EQ(memcmp(got, "world", 5), 0);
}

/* CHTTP_TLS_DEFAULT and a zero-initialised struct are the two documented ways
 * to spell "the defaults", and zero is the strict value of every field. The
 * comparison is field by field and not a memcmp, because a compound literal
 * leaves the padding between its members unspecified. A field added to
 * chttp_tls_config_t must be added here too. */
TEST(tls_config, the_default_macro_is_the_zeroed_struct_field_by_field) {
  chttp_tls_config_t from_macro = CHTTP_TLS_DEFAULT;
  REQUIRE_EQ((void *)from_macro.cert_path, (void *)NULL);
  REQUIRE_EQ((void *)from_macro.key_path, (void *)NULL);
  REQUIRE_EQ((void *)from_macro.ca_bundle_path, (void *)NULL);
  /* The casts to int are what the _Generic printer of tau accepts. */
  REQUIRE_EQ((int)from_macro.insecure_skip_verify, 0);
  REQUIRE_EQ((int)from_macro.insecure_skip_hostname_check, 0);
  REQUIRE_EQ((int)from_macro.client_cert_optional, 0);

  /* A designated initializer that names only a CA bundle, the idiomatic way
   * to configure mutual TLS on a server, leaves the requirement of a client
   * certificate on. */
  chttp_tls_config_t mtls = {.ca_bundle_path = "ca.pem"};
  REQUIRE_EQ((int)mtls.client_cert_optional, 0);
}
