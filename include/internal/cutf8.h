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

// INTERNAL ONLY. The UTF-8 well-formedness check that cjson and cyaml share,
// so the two modules can never disagree about which bytes count as UTF-8.
// Extend this file instead of writing a check of your own. Every function is
// static inline and nothing here is installed or exported.
#ifndef CCOL_CUTF8_H
#define CCOL_CUTF8_H

// C23 has bool, true and false as keywords.
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdio.h>

// Steps over one UTF-8 sequence at s, where n > 0 bytes are readable; every
// other function here is built on it. For a well-formed sequence it returns its
// length (1 to 4) and sets *ok. For an ill-formed one it clears *ok and returns
// the length of the maximal subpart (at least 1), the unit that Unicode
// replaces with one U+FFFD. The lead byte table (trail count plus the range of
// the first continuation byte) refuses stray continuation bytes, overlong
// forms, surrogates and anything above U+10FFFF without decoding the value.
static inline size_t ccol_utf8_step(const unsigned char *s, size_t n,
                                    bool *ok) {
  unsigned char b0 = s[0];
  *ok = true;
  if (b0 < 0x80) return 1;

  size_t trail;
  unsigned char lo, hi;
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

// Tells whether the n-byte buffer s is well-formed UTF-8, for the API calls of
// cjson and cyaml that refuse a C string that isn't.
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

// Gives the offset of the first byte of s that starts an ill-formed sequence,
// or n when the whole buffer is well-formed, so a parser can report where its
// input goes wrong. ASCII costs one comparison per byte.
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

// Names, for a parse error, what is wrong with the ill-formed sequence at p[at]
// (at < len). It returns a static phrase and writes the bytes that show the
// defect into detail (detail_size bytes), each as 0xNN, so the message is
// printable ASCII whatever the input holds. A lead byte whose maximal subpart
// is 1 while the next byte is a continuation byte has a first continuation byte
// outside its narrow range: A surrogate, a value above U+10FFFF, or an overlong
// form.
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

#endif
