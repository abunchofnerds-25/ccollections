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
#include <common.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ========================================================================== */
/*                         BASE64 (RFC 4648, '+'/'/' with '=' padding) */
/* ========================================================================== */

static const char g_chttp_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Reverse lookup table: maps a base64 alphabet byte to its 6-bit value, or
 * -1 for any byte that is not part of the alphabet (including '=', which is
 * handled separately as a padding marker, not a data character). Built once,
 * lazily, via pthread_once rather than a designated-initializer literal;
 * a literal would need every one of the 61 non-specified alphabet slots to
 * default to something other than 0, since 0 already means 'A'. */
static signed char g_chttp_base64_decode_table[256];
static once_flag_t g_chttp_base64_decode_table_once = ONCE_INIT;

static void _chttp_base64_decode_table_init(void) {
  memset(g_chttp_base64_decode_table, -1, sizeof(g_chttp_base64_decode_table));
  for (int i = 0; i < 26; i++) {
    g_chttp_base64_decode_table[(unsigned char)('A' + i)] = (signed char)i;
    g_chttp_base64_decode_table[(unsigned char)('a' + i)] =
        (signed char)(26 + i);
  }
  for (int i = 0; i < 10; i++)
    g_chttp_base64_decode_table[(unsigned char)('0' + i)] =
        (signed char)(52 + i);
  g_chttp_base64_decode_table[(unsigned char)'+'] = 62;
  g_chttp_base64_decode_table[(unsigned char)'/'] = 63;
}

/**
 * @brief Base64-encode a buffer (custom allocator).
 */
char *chttp_base64_encode_mp(ccol_memmgmt_procs_t *mp, const void *data,
                             size_t len, size_t *out_len) {
  if (!data && len > 0) return NULL;
  /* enc_len below is ((len+2)/3)*4; len is a caller-supplied length with no
   * upper bound of its own, and neither "len+2" nor the final "*4" was
   * previously checked for overflow. Practically unreachable (it would take
   * a multi-exabyte input to actually wrap size_t), but an unchecked size
   * computation feeding an allocation is worth rejecting outright rather
   * than trusting the input never gets that large. */
  if (len > (SIZE_MAX / 4) * 3 - 4) return NULL;

  const unsigned char *p = (const unsigned char *)data;
  size_t enc_len = ((len + 2) / 3) * 4;
  char *out = (char *)_mem_alloc(mp, enc_len + 1);
  if (!out) return NULL;

  size_t oi = 0, i = 0;
  for (; i + 3 <= len; i += 3) {
    uint32_t n =
        ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | (uint32_t)p[i + 2];
    out[oi++] = g_chttp_base64_alphabet[(n >> 18) & 0x3F];
    out[oi++] = g_chttp_base64_alphabet[(n >> 12) & 0x3F];
    out[oi++] = g_chttp_base64_alphabet[(n >> 6) & 0x3F];
    out[oi++] = g_chttp_base64_alphabet[n & 0x3F];
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t n = (uint32_t)p[i] << 16;
    out[oi++] = g_chttp_base64_alphabet[(n >> 18) & 0x3F];
    out[oi++] = g_chttp_base64_alphabet[(n >> 12) & 0x3F];
    out[oi++] = '=';
    out[oi++] = '=';
  } else if (rem == 2) {
    uint32_t n = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
    out[oi++] = g_chttp_base64_alphabet[(n >> 18) & 0x3F];
    out[oi++] = g_chttp_base64_alphabet[(n >> 12) & 0x3F];
    out[oi++] = g_chttp_base64_alphabet[(n >> 6) & 0x3F];
    out[oi++] = '=';
  }
  out[oi] = '\0';

  if (out_len) *out_len = enc_len;
  return out;
}

/**
 * @brief Base64-decode a NUL-terminated base64 string (custom allocator).
 */
void *chttp_base64_decode_mp(ccol_memmgmt_procs_t *mp, const char *b64_input,
                             size_t *out_len) {
  if (!b64_input) return NULL;

  size_t in_len = strlen(b64_input);
  if (in_len == 0) {
    char *out = (char *)_mem_alloc(mp, 1);
    if (!out) return NULL;
    out[0] = '\0';
    if (out_len) *out_len = 0;
    return out;
  }
  if (in_len % 4 != 0) return NULL;

  call_once(g_chttp_base64_decode_table_once, _chttp_base64_decode_table_init);

  /* '=' padding may only appear as the final one or two bytes of the whole
   * string; this pre-scan bounds the output allocation exactly, and the
   * per-group validation loop below independently rejects any other
   * placement of '=' (elsewhere in the string, or data following a padding
   * byte within the last group) before ever writing past that bound. */
  size_t pad = 0;
  if (b64_input[in_len - 1] == '=') {
    pad = 1;
    if (in_len >= 2 && b64_input[in_len - 2] == '=') pad = 2;
  }

  size_t groups = in_len / 4;
  size_t dec_len = groups * 3 - pad;
  unsigned char *out = (unsigned char *)_mem_alloc(mp, dec_len + 1);
  if (!out) return NULL;

  size_t oi = 0;
  for (size_t g = 0; g < groups; g++) {
    const char *chunk = b64_input + g * 4;
    bool last_group = (g == groups - 1);
    int vals[4];
    bool seen_pad = false;
    for (int k = 0; k < 4; k++) {
      char c = chunk[k];
      if (c == '=') {
        if (!last_group || k < 2) {
          _mem_free(mp, out);
          return NULL;
        }
        seen_pad = true;
        vals[k] = -1;
      } else {
        if (seen_pad) {
          _mem_free(mp, out);
          return NULL;
        }
        int v = g_chttp_base64_decode_table[(unsigned char)c];
        if (v < 0) {
          _mem_free(mp, out);
          return NULL;
        }
        vals[k] = v;
      }
    }

    uint32_t n = ((uint32_t)vals[0] << 18) | ((uint32_t)vals[1] << 12) |
                 ((uint32_t)(vals[2] < 0 ? 0 : vals[2]) << 6) |
                 (uint32_t)(vals[3] < 0 ? 0 : vals[3]);
    out[oi++] = (unsigned char)((n >> 16) & 0xFF);
    if (vals[2] >= 0) out[oi++] = (unsigned char)((n >> 8) & 0xFF);
    if (vals[3] >= 0) out[oi++] = (unsigned char)(n & 0xFF);
  }
  out[oi] = '\0';

  if (out_len) *out_len = oi;
  return out;
}

/* ========================================================================== */
/*                         HTTP BASIC AUTH (RFC 7617)                         */
/* ========================================================================== */

/**
 * @brief Build a "Basic <base64(username:password)>" header value (custom
 * allocator).
 */
/* Does the actual work for chttp_basic_auth_mp, taking user_len/pass_len as
 * explicit parameters (rather than computing them via strlen() internally)
 * so the overflow guard below can be exercised directly from a white-box
 * test with a fake length pair, without needing to actually construct a
 * multi-exabyte input string; see
 * _chttp_basic_auth_overflow_guard_for_tests below. */
static char *_chttp_basic_auth_len(ccol_memmgmt_procs_t *mp,
                                   const char *username, size_t user_len,
                                   const char *password, size_t pass_len) {
  /* Overflow guard on the "needed size" computation below, mirroring
   * chttp_base64_encode_mp's own identical guard just above: user_len and
   * pass_len are two independently caller-supplied string lengths, so their
   * sum reaching close to SIZE_MAX does not require either string alone to
   * be implausibly large. Leaves room for both the ':' separator below and
   * the final allocation's own NUL terminator. Unreachable in practice
   * (would need multi-exabyte input strings), but this project treats a
   * reducible/unguarded overflow in a size computation as a real bug
   * regardless of how large an input is needed to trigger it. */
  if (pass_len > SIZE_MAX - user_len || user_len + pass_len > SIZE_MAX - 2)
    return NULL;
  size_t combined_len = user_len + 1 + pass_len;
  char *combined = (char *)_mem_alloc(mp, combined_len + 1);
  if (!combined) return NULL;
  snprintf(combined, combined_len + 1, "%s:%s", username, password);

  size_t b64_len = 0;
  char *b64 = chttp_base64_encode_mp(mp, combined, combined_len, &b64_len);
  _mem_free(mp, combined);
  if (!b64) return NULL;

  size_t auth_len = 6 + b64_len; /* strlen("Basic ") == 6 */
  char *out = (char *)_mem_alloc(mp, auth_len + 1);
  if (!out) {
    _mem_free(mp, b64);
    return NULL;
  }
  snprintf(out, auth_len + 1, "Basic %s", b64);
  _mem_free(mp, b64);
  return out;
}

char *chttp_basic_auth_mp(ccol_memmgmt_procs_t *mp, const char *username,
                          const char *password) {
  if (!username || !password) return NULL;
  return _chttp_basic_auth_len(mp, username, strlen(username), password,
                               strlen(password));
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper exposing _chttp_basic_auth_len's overflow guard
 * directly. ONLY safe to call with a fake_user_len/fake_pass_len pair that
 * actually exceeds the guard's own threshold, so the guard rejects before
 * snprintf ever reads past username/password's own real, short contents.
 * Not part of the public API; gated so this symbol does not leak into a
 * production build of libccollections.so, matching every white-box helper
 * already established elsewhere in this project (e.g. chttpclient.c's
 * _chttp_ob_append_overflow_guard_for_tests).
 */
bool _chttp_basic_auth_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                const char *username,
                                                size_t fake_user_len,
                                                const char *password,
                                                size_t fake_pass_len) {
  char *r = _chttp_basic_auth_len(mp, username, fake_user_len, password,
                                  fake_pass_len);
  bool rejected = (r == NULL);
  _mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */
