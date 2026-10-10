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

#pragma once

/**
 * @file cutf8.h
 * @brief INTERNAL ONLY. The UTF-8 well-formedness check that cjson and cyaml
 *        share.
 *
 * Both modules keep the invariant that every string a DOM node holds is a
 * well-formed UTF-8 encoding of Unicode scalar values. They decide that with
 * this one definition, so that the two can never disagree about which byte
 * sequence is UTF-8.
 *
 * This header is internal, so it carries no visibility block and make
 * install does not install it. Every function is static inline, so nothing
 * here reaches the dynamic symbol table of the shared library.
 */

#ifndef CCOL_CUTF8_H
#define CCOL_CUTF8_H

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <stdio.h>

/*
 * Step over one UTF-8 sequence at s. The caller can read n > 0 bytes there.
 *
 * For a well-formed sequence, the function returns its length (1 to 4) and
 * sets *ok. For an ill-formed one, it clears *ok and returns the length of
 * the maximal subpart, which is at least 1: the longest prefix of s that
 * could still start a well-formed sequence. That is the unit that "U+FFFD
 * Substitution of Maximal Subparts" in Unicode 15.0 sec. 3.9 replaces with
 * one U+FFFD. Any other count of the bytes is wrong, because it merges
 * several independent errors into one replacement character, or splits one
 * error into several.
 *
 * The table of lead bytes rejects the following by its own shape, with no
 * arithmetic on the decoded value: a continuation byte with no lead (0x80 to
 * 0xBF); the two overlong two-byte leads (0xC0 and 0xC1); a lead outside the
 * range of Unicode (0xF5 to 0xFF); the overlong three-byte and four-byte
 * forms, through the narrow range for the first continuation byte under 0xE0
 * and under 0xF0; the surrogate range of UTF-16, through the narrow range
 * under 0xED; and anything past U+10FFFF, through the narrow range under
 * 0xF4.
 */
static inline size_t ccol_utf8_step(const unsigned char *s, size_t n,
                                    bool *ok) {
  unsigned char b0 = s[0];
  *ok = true;
  if (b0 < 0x80) return 1;

  size_t trail;         /* continuation bytes that the lead needs after it */
  unsigned char lo, hi; /* the range for the FIRST continuation byte */
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    trail = 1, lo = 0x80, hi = 0xBF;
  } else if (b0 == 0xE0) {
    trail = 2, lo = 0xA0, hi = 0xBF;
  } else if (b0 == 0xED) {
    trail = 2, lo = 0x80, hi = 0x9F;
  } else if (b0 >= 0xE1 && b0 <= 0xEF) {
    trail = 2, lo = 0x80, hi = 0xBF;
  } else if (b0 == 0xF0) {
    trail = 3, lo = 0x90, hi = 0xBF;
  } else if (b0 == 0xF4) {
    trail = 3, lo = 0x80, hi = 0x8F;
  } else if (b0 >= 0xF1 && b0 <= 0xF3) {
    trail = 3, lo = 0x80, hi = 0xBF;
  } else {
    *ok = false;
    return 1;
  }

  if (n < 2 || s[1] < lo || s[1] > hi) {
    *ok = false;
    return 1;
  }
  for (size_t i = 2; i <= trail; i++) {
    if (n <= i || s[i] < 0x80 || s[i] > 0xBF) {
      *ok = false;
      return i;
    }
  }
  return trail + 1;
}

/* True when the whole n-byte buffer is well-formed UTF-8. */
static inline bool ccol_utf8_is_valid(const char *s, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < n) {
    if (p[i] < 0x80) {
      i++;
      continue;
    }
    bool ok;
    size_t step = ccol_utf8_step(p + i, n - i, &ok);
    if (!ok) return false;
    i += step;
  }
  return true;
}

/* Gives the offset of the first byte of s[0..n) that starts an ill-formed
 * UTF-8 sequence, or n when the whole buffer is well-formed. A run of ASCII
 * costs one comparison for each byte. */
static inline size_t ccol_utf8_first_ill_formed(const char *s, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  size_t i = 0;
  while (i < n) {
    if (p[i] < 0x80) {
      i++;
      continue;
    }
    bool ok;
    size_t step = ccol_utf8_step(p + i, n - i, &ok);
    if (!ok) return i;
    i += step;
  }
  return n;
}

/*
 * Names, for a diagnostic, the defect of the ill-formed sequence that starts
 * at p[at], where ccol_utf8_step() reports one. The caller can read len bytes
 * at p, and at < len. The function returns a static phrase for what is wrong
 * and writes into detail the bytes that show it, each as 0xNN. Every byte of
 * both is printable ASCII whatever the input holds, so a message built from
 * them stays valid UTF-8 and cannot be truncated by a NUL.
 */
static inline const char *ccol_utf8_describe_ill_formed(const unsigned char *p,
                                                        size_t len, size_t at,
                                                        char *detail,
                                                        size_t detail_size) {
  unsigned char c = p[at];
  unsigned char c1 = at + 1 < len ? p[at + 1] : 0;
  bool ok;
  size_t step = ccol_utf8_step(p + at, len - at, &ok);
  if (c <= 0xBF) {
    snprintf(detail, detail_size, "0x%02x", c);
    return "continuation byte with no lead byte";
  }
  if (c <= 0xC1) {
    snprintf(detail, detail_size, "lead byte 0x%02x", c);
    return "overlong encoding";
  }
  if (c >= 0xF5) {
    snprintf(detail, detail_size, "0x%02x", c);
    return "byte that never appears in UTF-8";
  }
  if (at + step >= len) {
    snprintf(detail, detail_size, "lead byte 0x%02x", c);
    return "sequence truncated by the end of the input";
  }
  if (step == 1 && c1 >= 0x80 && c1 <= 0xBF) {
    /* The lead accepts only a narrower range for its first continuation
     * byte, and c1 is a continuation byte outside that range. */
    snprintf(detail, detail_size, "bytes 0x%02x 0x%02x", c, c1);
    return (c == 0xED)   ? "encoded surrogate code point"
           : (c == 0xF4) ? "code point above U+10FFFF"
                         : "overlong encoding";
  }
  snprintf(detail, detail_size,
           "lead byte 0x%02x, then 0x%02x where a continuation byte is "
           "expected",
           c, p[at + step]);
  return "truncated sequence";
}

#endif /* CCOL_CUTF8_H */
