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
 * @file cascii.h
 * @brief INTERNAL ONLY. Case folding and case-insensitive comparison of ASCII
 *        text, independent of the locale.
 *
 * HTTP field names, tokens such as "chunked" and "close", URI schemes and host
 * names are ASCII and their case rules come from the protocol, never from the
 * locale. strcasecmp(), strncasecmp() and tolower() follow LC_CTYPE instead,
 * and under a Turkish locale tolower('I') is not 'i'. "TRANSFER-ENCODING"
 * then stops matching "transfer-encoding", and a request that a front end
 * frames with chunked reaches this library framed by Content-Length. So every
 * comparison of protocol text goes through these helpers, which fold only 'A'
 * to 'Z' and leave every other byte alone. When you add a new comparison of
 * protocol text, use them too; strcasecmp() is the easy mistake here, and no
 * test in the C locale shows it.
 */

#ifndef CCOL_INTERNAL_CASCII_H
#define CCOL_INTERNAL_CASCII_H

#include <stddef.h>

/* Gives c in lower case when it's an ASCII upper-case letter, else c. */
static inline int ccol_ascii_tolower(int c) {
  return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* Gives c in upper case when it's an ASCII lower-case letter, else c. */
static inline int ccol_ascii_toupper(int c) {
  return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

/* True for the six ASCII white-space bytes that isspace() reports in the C
 * locale: space, '\t', '\n', '\v', '\f' and '\r'. We treat no other byte as
 * white space, whatever the locale says. */
static inline int ccol_ascii_isspace(int c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

/* Same as strcasecmp() but with ASCII folding only. Gives < 0, 0 or > 0 when
 * a sorts before, equal to or after b, and compares the folded bytes as
 * unsigned char. */
static inline int ccol_ascii_casecmp(const char *a, const char *b) {
  const unsigned char *p = (const unsigned char *)a;
  const unsigned char *q = (const unsigned char *)b;
  for (;; p++, q++) {
    int x = ccol_ascii_tolower(*p);
    int y = ccol_ascii_tolower(*q);
    if (x != y || x == 0) return x - y;
  }
}

/* Same as strncasecmp() with ASCII folding only, looking at n bytes at most. */
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

#endif /* CCOL_INTERNAL_CASCII_H */
