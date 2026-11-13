/*
 * MIT License
 *
 * Copyright (c) 2026 - A bunch of nerds
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// INTERNAL ONLY. A growable byte buffer with a latching out-of-memory flag,
// for code that builds text piece by piece (the serializers, the record
// builder of the logger, text that a parse collects). Once an allocation fails
// every later operation on the buffer does nothing, so the caller checks for
// an error once, at the end of the whole build. The header isn't installed and
// its symbols aren't exported.
#ifndef CCOL_CGROWBUF_H
#define CCOL_CGROWBUF_H

#include <common.h>
// C23 has bool, true and false as keywords.
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The buffer. When an allocation fails, or a size is too large to represent,
// oom latches to true. After the build the caller reads buf, len and oom
// directly; buf is NUL-terminated but can be NULL when oom is set. cap and
// m_procs are internal bookkeeping.
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
  ccol_memmgmt_procs *m_procs;
} ccol_growbuf;

// Sets b up with a 256-byte store, so a builder can start appending. Every
// later growth allocates through mp. A failed allocation sets oom.
void ccol_growbuf_init(ccol_growbuf *b, ccol_memmgmt_procs *mp);

// Like ccol_growbuf_init(), for a caller that knows roughly how much it will
// append, so it doesn't pay for growth steps: The store holds at least hint + 1
// bytes (never less than 64). A hint of SIZE_MAX can't be represented, so it
// sets oom.
void ccol_growbuf_init_hint(ccol_growbuf *b, ccol_memmgmt_procs *mp,
                            size_t hint);

// Appends n bytes of data to b, growing the buffer when needed. It's the out of
// line half of ccol_growbuf_append(), which calls it for every case that the
// inline path doesn't handle. data may be NULL when n is 0. Does nothing once
// oom is set, and sets oom when the new size can't be represented or the
// allocation fails.
void ccol_growbuf_append_slow(ccol_growbuf *b, const char *data, size_t n);

// Appends n bytes of data to b; it's the append that every builder calls. The
// common case (n bytes that fit in the room already allocated) runs inline and
// costs no call. Growth, an empty append and the oom state go to
// ccol_growbuf_append_slow(). The n <= PTRDIFF_MAX bound changes nothing at run
// time; it tells the fortify checks that the memcpy never sees a larger
// length.
static inline void ccol_growbuf_append(ccol_growbuf *b, const char *data,
                                       size_t n) {
  if (__builtin_expect(
          !b->oom && n != 0 && n <= (size_t)PTRDIFF_MAX && n < b->cap - b->len,
          1)) {
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return;
  }
  ccol_growbuf_append_slow(b, data, n);
}

// Appends the byte c to b, for builders that emit punctuation one byte at a
// time.
static inline void ccol_growbuf_append_c(ccol_growbuf *b, char c) {
  ccol_growbuf_append(b, &c, 1);
}

// Appends the NUL-terminated string s to b, for builders that emit literals. A
// NULL s is an empty append, so a caller can pass along the buf field of
// another buffer that ran out of memory.
static inline void ccol_growbuf_append_cstr(ccol_growbuf *b, const char *s) {
  if (!s) return;
  ccol_growbuf_append(b, s, strlen(s));
}

// Frees the store of b (not b itself), for a builder that drops its result or
// fails. Safe on a buffer in the oom state.
static inline void ccol_growbuf_destroy(ccol_growbuf *b) {
  ccol__mem_free(b->m_procs, b->buf);
}

#endif
