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

// INTERNAL ONLY. Case folding and case-insensitive comparison of ASCII text,
// independent of the locale. HTTP field names, tokens such as "chunked", URI
// schemes and host names follow the case rules of the protocol. strcasecmp()
// and tolower() follow LC_CTYPE, and under a Turkish locale tolower('I') isn't
// 'i', so "TRANSFER-ENCODING" stops matching. Every comparison of protocol text
// goes through these helpers, which fold only 'A' to 'Z'. No test in the C
// locale shows the mistake, so please use them for any new comparison.

#ifndef CCOL_INTERNAL_CASCII_H
#define CCOL_INTERNAL_CASCII_H

#include <stddef.h>

// Gives c in lower case when it's an ASCII upper-case letter, else c. It's the
// locale-free tolower() that every fold of protocol text uses.
static inline int ccol_ascii_tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

// Gives c in upper case when it's an ASCII lower-case letter, else c; the
// locale-free toupper() that cstring's case conversion uses.
static inline int ccol_ascii_toupper(int c) {
  return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

// Says whether c is ASCII white space; cstring_trim() uses it so the locale
// can't change what counts as space. True for the six bytes that isspace()
// reports in the C locale: Space, '\t', '\n', '\v', '\f' and '\r'. No other
// byte counts as white space, whatever the locale says.
static inline int ccol_ascii_isspace(int c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

// Compares two strings ignoring ASCII case, for protocol names and tokens;
// strcasecmp() with ASCII folding only. Gives < 0, 0 or > 0 and compares the
// folded bytes as unsigned char.
static inline int ccol_ascii_casecmp(const char *a, const char *b) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;
  for (;; p++, q++) {
    int x = ccol_ascii_tolower(*p);
    int y = ccol_ascii_tolower(*q);
    if (x != y || x == 0) return x - y;
  }
}

// Compares at most n bytes ignoring ASCII case, for protocol text that isn't
// NUL-terminated where it ends; strncasecmp() with ASCII folding only.
static inline int ccol_ascii_ncasecmp(const char *a, const char *b, size_t n) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;
  for (; n > 0; n--, p++, q++) {
    int x = ccol_ascii_tolower(*p);
    int y = ccol_ascii_tolower(*q);
    if (x != y || x == 0) return x - y;
  }
  return 0;
}

#endif  // CCOL_INTERNAL_CASCII_H
