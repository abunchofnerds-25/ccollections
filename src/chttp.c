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
#include <internal/test_helpers/chttp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Base64 (RFC 4648, '+' and '/', '=' padding)
//
// The encoding alphabet, indexed by a 6-bit value.
static const char g_chttp_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Maps a byte of the alphabet to its 6-bit value and every other byte, '='
// included, to -1. It's filled once on first use, because an initializer can't
// default 61 unnamed slots to -1 (0 already means 'A').
static signed char g_chttp_base64_decode_table[256];
static ccol__once_flag g_chttp_base64_decode_table_once = CCOL__ONCE_INIT;

// Fills g_chttp_base64_decode_table, which the decoder reads. It runs once,
// through g_chttp_base64_decode_table_once, the first time a string is
// decoded.
static void chttp_base64_decode_table_init(void) {
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

// Encodes len bytes of data as base64, for chttp_basic_auth_mp() and for
// callers that build header values. It gives a new NUL-terminated string from
// mp, with its length in *out_len when out_len isn't NULL, or NULL for a NULL
// data with len > 0, a failed allocation, or a len whose encoded size
// ((len + 2) / 3 * 4 + 1) would overflow size_t.
char *chttp_base64_encode_mp(ccol_memmgmt_procs *mp, const void *data,
                             size_t len, size_t *out_len) {
  if (!data && len > 0) return NULL;
  if (len > (SIZE_MAX / 4) * 3 - 4) return NULL;

  const unsigned char *p = (const unsigned char *)data;
  size_t enc_len = ((len + 2) / 3) * 4;
  char *out = (char *)ccol__mem_alloc(mp, enc_len + 1);
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

// Decodes the NUL-terminated base64 string b64_input, for callers that read
// encoded header values. It gives a new buffer from mp (NUL-terminated too)
// with the decoded length in *out_len, or NULL for malformed input or a
// failed allocation. '=' may only be the last one or two bytes, so a pre-scan
// gives the output an exact size, and the group loop refuses a '=' anywhere
// else before it could write past it. A padded final group must have its
// unused low bits zero (RFC 4648 section 3.5), so "QR==" is refused and each
// byte string has one spelling. Reads the shared decode table and initializes
// it once.
void *chttp_base64_decode_mp(ccol_memmgmt_procs *mp, const char *b64_input,
                             size_t *out_len) {
  if (!b64_input) return NULL;

  size_t in_len = strlen(b64_input);
  if (in_len == 0) {
    char *out = (char *)ccol__mem_alloc(mp, 1);
    if (!out) return NULL;
    out[0] = '\0';
    if (out_len) *out_len = 0;
    return out;
  }
  if (in_len % 4 != 0) return NULL;

  ccol__call_once(g_chttp_base64_decode_table_once,
                  chttp_base64_decode_table_init);

  size_t pad = 0;
  if (b64_input[in_len - 1] == '=') {
    pad = 1;
    if (in_len >= 2 && b64_input[in_len - 2] == '=') pad = 2;
  }

  size_t groups = in_len / 4;
  size_t dec_len = groups * 3 - pad;
  unsigned char *out = (unsigned char *)ccol__mem_alloc(mp, dec_len + 1);
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
          ccol__mem_free(mp, out);
          return NULL;
        }
        seen_pad = true;
        vals[k] = -1;
      } else {
        if (seen_pad) {
          ccol__mem_free(mp, out);
          return NULL;
        }
        int v = g_chttp_base64_decode_table[(unsigned char)c];
        if (v < 0) {
          ccol__mem_free(mp, out);
          return NULL;
        }
        vals[k] = v;
      }
    }

    if ((vals[2] < 0 && (vals[1] & 0x0F) != 0) ||
        (vals[2] >= 0 && vals[3] < 0 && (vals[2] & 0x03) != 0)) {
      ccol__mem_free(mp, out);
      return NULL;
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

// HTTP Basic auth (RFC 7617)
//
// Builds "Basic <base64(username:password)>" in a new string from mp. It's the
// body of chttp_basic_auth_mp(); the lengths are parameters so the white-box
// hook can push the overflow guard with fake lengths. The guard leaves room
// for the ':' and the NUL. It gives NULL when the size would overflow or an
// allocation fails.
static char *chttp_basic_auth_len(ccol_memmgmt_procs *mp, const char *username,
                                  size_t user_len, const char *password,
                                  size_t pass_len) {
  if (pass_len > SIZE_MAX - user_len || user_len + pass_len > SIZE_MAX - 2)
    return NULL;
  size_t combined_len = user_len + 1 + pass_len;
  char *combined = (char *)ccol__mem_alloc(mp, combined_len + 1);
  if (!combined) return NULL;
  snprintf(combined, combined_len + 1, "%s:%s", username, password);

  size_t b64_len = 0;
  char *b64 = chttp_base64_encode_mp(mp, combined, combined_len, &b64_len);
  ccol__mem_free(mp, combined);
  if (!b64) return NULL;

  size_t auth_len = 6 + b64_len;
  char *out = (char *)ccol__mem_alloc(mp, auth_len + 1);
  if (!out) {
    ccol__mem_free(mp, b64);
    return NULL;
  }
  snprintf(out, auth_len + 1, "Basic %s", b64);
  ccol__mem_free(mp, b64);
  return out;
}

// True when s holds a byte below 0x20 or DEL (0x7F). chttp_basic_auth_mp()
// uses it to refuse credentials RFC 7617 forbids.
static bool chttp_has_ctl(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if (*p < 0x20 || *p == 0x7F) return true;
  return false;
}

// Builds the Basic auth header value for username and password, so a caller
// can authenticate a request. It gives a new string from mp, or NULL for a
// NULL string, a failed allocation or a refused input. It refuses a colon in
// the username (the recipient cuts at the first colon, so "a:b" + "c" would
// read as "a" + "b:c") and a control byte in either string, as RFC 7617
// section 2 requires. A colon in the password is fine.
char *chttp_basic_auth_mp(ccol_memmgmt_procs *mp, const char *username,
                          const char *password) {
  if (!username || !password) return NULL;
  if (strchr(username, ':')) return NULL;
  if (chttp_has_ctl(username) || chttp_has_ctl(password)) return NULL;
  return chttp_basic_auth_len(mp, username, strlen(username), password,
                              strlen(password));
}

#ifdef RUNNING_UNIT_TESTS
// Runs chttp_basic_auth_len() with fake lengths and tells whether the guard
// refused, so a test can reach the guard without huge strings. Only lengths
// over the guard's threshold are safe.
bool chttp__basic_auth_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                const char *username,
                                                size_t fake_user_len,
                                                const char *password,
                                                size_t fake_pass_len) {
  char *r = chttp_basic_auth_len(mp, username, fake_user_len, password,
                                 fake_pass_len);
  bool rejected = (r == NULL);
  ccol__mem_free(mp, r);
  return rejected;
}
#endif
