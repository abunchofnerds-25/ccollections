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

/* ========================================================================== */
/*                         BASE64 (RFC 4648, '+'/'/' with '=' padding) */
/* ========================================================================== */

static const char g_chttp_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Reverse lookup table: maps a byte of the base64 alphabet to its 6-bit
 * value and holds -1 for every byte outside the alphabet. That includes '=',
 * since we treat '=' as a padding marker and not as data. The table is built
 * once, the first time we need it, with pthread_once. A designated
 * initializer literal can't do this job because it would need a default
 * other than 0 for each of the 61 slots it doesn't name, and 0 already means
 * 'A'. */
static signed char g_chttp_base64_decode_table[256];
static _ccol_once_flag_t g_chttp_base64_decode_table_once = _CCOL_ONCE_INIT;

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
 * @brief Encode a buffer as base64 (custom allocator).
 */
char *chttp_base64_encode_mp(ccol_memmgmt_procs_t *mp, const void *data,
                             size_t len, size_t *out_len) {
  if (!data && len > 0) return NULL;
  /* enc_len below is ((len+2)/3)*4. len comes from the caller and has no
   * upper bound of its own, so both "len+2" and the final "*4" need an
   * overflow check. No real input gets here (wrapping size_t takes an input
   * of many exabytes), but a size computation that feeds an allocation must
   * not go unchecked, so we reject the input rather than trust it never
   * gets that large. */
  if (len > (SIZE_MAX / 4) * 3 - 4) return NULL;

  const unsigned char *p = (const unsigned char *)data;
  size_t enc_len = ((len + 2) / 3) * 4;
  char *out = (char *)_ccol_mem_alloc(mp, enc_len + 1);
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
 * @brief Decode a base64 string that ends with a NUL (custom allocator).
 */
void *chttp_base64_decode_mp(ccol_memmgmt_procs_t *mp, const char *b64_input,
                             size_t *out_len) {
  if (!b64_input) return NULL;

  size_t in_len = strlen(b64_input);
  if (in_len == 0) {
    char *out = (char *)_ccol_mem_alloc(mp, 1);
    if (!out) return NULL;
    out[0] = '\0';
    if (out_len) *out_len = 0;
    return out;
  }
  if (in_len % 4 != 0) return NULL;

  _ccol_call_once(g_chttp_base64_decode_table_once,
                  _chttp_base64_decode_table_init);

  /* '=' padding can only be the last byte or the last two bytes of the whole
   * string, so this pre-scan gives the output allocation an exact bound. The
   * loop below validates each group and rejects a '=' anywhere
   * else (earlier in the string, or after a padding byte inside the last
   * group) before it could write past that bound. */
  size_t pad = 0;
  if (b64_input[in_len - 1] == '=') {
    pad = 1;
    if (in_len >= 2 && b64_input[in_len - 2] == '=') pad = 2;
  }

  size_t groups = in_len / 4;
  size_t dec_len = groups * 3 - pad;
  unsigned char *out = (unsigned char *)_ccol_mem_alloc(mp, dec_len + 1);
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
          _ccol_mem_free(mp, out);
          return NULL;
        }
        seen_pad = true;
        vals[k] = -1;
      } else {
        if (seen_pad) {
          _ccol_mem_free(mp, out);
          return NULL;
        }
        int v = g_chttp_base64_decode_table[(unsigned char)c];
        if (v < 0) {
          _ccol_mem_free(mp, out);
          return NULL;
        }
        vals[k] = v;
      }
    }

    /* A padded final group carries bits that no output byte holds: the low 4
     * bits of the second character before "==", and the low 2 bits of the
     * third character before "=". RFC 4648 SS3.5 has the encoder set them to
     * zero, and a decoder may refuse data where they aren't. Without this
     * check "QQ==" and "QR==" both decode to "A", so two different strings
     * name the same bytes, and comparing encoded values (a credential, a
     * digest) gives a different answer from comparing the bytes. So we only
     * accept the canonical spelling. */
    if ((vals[2] < 0 && (vals[1] & 0x0F) != 0) ||
        (vals[2] >= 0 && vals[3] < 0 && (vals[2] & 0x03) != 0)) {
      _ccol_mem_free(mp, out);
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

/* ========================================================================== */
/*                         HTTP BASIC AUTH (RFC 7617)                         */
/* ========================================================================== */

/**
 * @brief Build a "Basic <base64(username:password)>" header value (custom
 * allocator).
 */
/* Does the work for chttp_basic_auth_mp. user_len and pass_len are explicit
 * parameters instead of being computed with strlen() in here, so that a
 * white-box test can drive the overflow guard below with a pair of fake
 * lengths and doesn't need an input string of many exabytes. See
 * _chttp_basic_auth_overflow_guard_for_tests below. */
static char *_chttp_basic_auth_len(ccol_memmgmt_procs_t *mp,
                                   const char *username, size_t user_len,
                                   const char *password, size_t pass_len) {
  /* Overflow guard for the size computation below, same as the one in
   * chttp_base64_encode_mp above. user_len and pass_len are two separate
   * string lengths from the caller, so their sum can get close to SIZE_MAX
   * even when neither string is very large on its own. The guard leaves room
   * for the ':' separator below and for the NUL terminator of the final
   * allocation. No real input gets here, it would take strings of many
   * exabytes, but in this project an overflow in a size computation that we
   * can remove is a real bug, however large the input it needs. */
  if (pass_len > SIZE_MAX - user_len || user_len + pass_len > SIZE_MAX - 2)
    return NULL;
  size_t combined_len = user_len + 1 + pass_len;
  char *combined = (char *)_ccol_mem_alloc(mp, combined_len + 1);
  if (!combined) return NULL;
  snprintf(combined, combined_len + 1, "%s:%s", username, password);

  size_t b64_len = 0;
  char *b64 = chttp_base64_encode_mp(mp, combined, combined_len, &b64_len);
  _ccol_mem_free(mp, combined);
  if (!b64) return NULL;

  size_t auth_len = 6 + b64_len; /* strlen("Basic ") == 6 */
  char *out = (char *)_ccol_mem_alloc(mp, auth_len + 1);
  if (!out) {
    _ccol_mem_free(mp, b64);
    return NULL;
  }
  snprintf(out, auth_len + 1, "Basic %s", b64);
  _ccol_mem_free(mp, b64);
  return out;
}

/* True when s holds a control character, which is a byte below 0x20 or DEL
 * (0x7F). */
static bool _chttp_has_ctl(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if (*p < 0x20 || *p == 0x7F) return true;
  return false;
}

char *chttp_basic_auth_mp(ccol_memmgmt_procs_t *mp, const char *username,
                          const char *password) {
  if (!username || !password) return NULL;
  /* RFC 7617 SS2: the user-id must not contain a colon. The colon is the ONE
   * delimiter in the "user-id ':' password" string we build here, and a
   * recipient cuts that string at its FIRST colon. If we let a colon into the
   * username, the cut lands in the wrong place: username "a:b" with password
   * "c" encodes to the same bytes as username "a" with password "b:c". A
   * caller asking for one identity would send another one, and two different
   * sets of credentials could encode to the same bytes. A colon in the
   * PASSWORD is legal and we build the string as asked, because the password
   * is everything after the first colon. */
  if (strchr(username, ':')) return NULL;
  /* RFC 7617 SS2: neither the user-id nor the password may contain a control
   * character (CTL of RFC 5234: 0x00 to 0x1F and 0x7F). A recipient that
   * follows the RFC refuses such credentials anyway, so we refuse them here
   * where the caller can see why, instead of encoding a value that no
   * conforming server will accept. */
  if (_chttp_has_ctl(username) || _chttp_has_ctl(password)) return NULL;
  return _chttp_basic_auth_len(mp, username, strlen(username), password,
                               strlen(password));
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper that exposes the overflow guard of
 * _chttp_basic_auth_len directly. It's safe ONLY with a pair of
 * fake_user_len and fake_pass_len values that goes over the guard's
 * threshold, so that the guard rejects the call before snprintf reads past
 * the real, short contents of username and password. It isn't part of the
 * public API: a gate keeps this symbol out of a production build of
 * libccollections.so, as for every other white-box helper in this project
 * (for example _chttp_ob_append_overflow_guard_for_tests in chttpclient.c).
 */
bool _chttp_basic_auth_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                const char *username,
                                                size_t fake_user_len,
                                                const char *password,
                                                size_t fake_pass_len) {
  char *r = _chttp_basic_auth_len(mp, username, fake_user_len, password,
                                  fake_pass_len);
  bool rejected = (r == NULL);
  _ccol_mem_free(mp, r);
  return rejected;
}
#endif /* RUNNING_UNIT_TESTS */
