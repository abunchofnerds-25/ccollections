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

/**
 * @file cgrowbuf.h
 * @brief INTERNAL ONLY. A growable byte buffer with a latching OOM flag.
 *
 * Code that builds text (a serializer, or text that a parse accumulates)
 * appends an unknown quantity of data one piece at a time, and checking the
 * allocation after each append makes that code difficult to read. This buffer
 * doubles its capacity when it grows and latches one out-of-memory flag; once
 * the flag is set, every further operation on the buffer is a safe no-op, so
 * the caller checks a whole build pass once, at the end.
 *
 * This header is internal: it carries no visibility block, make install does
 * not install it, and its symbols are absent from the dynamic symbol table of
 * the shared library.
 */

#ifndef CCOL_CGROWBUF_H
#define CCOL_CGROWBUF_H

#include <common.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <string.h>

/**
 * @brief A byte buffer that grows dynamically, for code that builds text (a
 * serializer, or text that a parse accumulates) and must append an unknown
 * quantity of data one piece at a time.
 *
 * The capacity doubles when the buffer grows. When an allocation fails, or
 * when a requested size is too large to represent, oom latches to true and
 * every further ccol_growbuf_* operation on that buffer is a safe no-op. A
 * caller can therefore check for an error at the end of a whole build pass
 * instead of after each append.
 *
 * A caller reads buf, len and oom directly after the build is complete. buf
 * is NUL-terminated, but it can be NULL when oom is set. cap and m_procs are
 * internal bookkeeping for growth, and a caller must not read them.
 */
typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool oom;
  ccol_memmgmt_procs_t *m_procs;
} ccol_growbuf_t;

/**
 * @brief Initialize a growable byte buffer with a 256-byte backing store.
 * @param b  The buffer to initialize.
 * @param mp The allocator for every growth and every append on this buffer.
 * @note If the allocation fails, this function sets the internal OOM flag of
 * the buffer, and every further ccol_growbuf_* call on b is then a safe no-op.
 */
void ccol_growbuf_init(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp);

/**
 * @brief The same as ccol_growbuf_init(), but it sizes the backing store in
 * advance, for a caller that already knows about how much content it will
 * append. The store holds at least `hint + 1` bytes, and never less than 64
 * bytes.
 * @param b    The buffer to initialize.
 * @param mp   The allocator for every growth and every append on this buffer.
 * @param hint The approximate number of content bytes that the caller
 *             expects. A hint of SIZE_MAX asks for a store of SIZE_MAX + 1
 *             bytes, which is not representable, so the function sets the
 *             internal OOM flag of the buffer instead of quietly allocating
 *             a different quantity.
 * @note If the allocation fails, this function sets the internal OOM flag of
 * the buffer, and every further ccol_growbuf_* call on b is then a safe no-op.
 */
void ccol_growbuf_init_hint(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp,
                            size_t hint);

/**
 * @brief Append n raw bytes, growing the buffer if necessary.
 * @param b    The buffer that receives the bytes.
 * @param data The bytes to append. data must be non-NULL and readable for n
 *             bytes, unless n is 0: the function then ignores data, which
 *             can be NULL.
 * @param n    The number of bytes to append.
 * @note This function is a no-op once the internal OOM flag of the buffer is
 * set.
 * @note An append needs n + 1 bytes more than the content that the buffer
 * already holds. When that total is too large to represent, the append makes
 * no copy and sets the internal OOM flag of the buffer, which is also what a
 * failed allocation does.
 */
void ccol_growbuf_append(ccol_growbuf_t *b, const char *data, size_t n);

/** @brief Append a single byte. */
static inline void ccol_growbuf_append_c(ccol_growbuf_t *b, char c) {
  ccol_growbuf_append(b, &c, 1);
}

/**
 * @brief Append a NUL-terminated string.
 * @note The function treats a NULL s as an empty append instead of calling
 * strlen(NULL), which is undefined behavior. It accepts that NULL on purpose:
 * a caller can pass on the buf field of another buffer, and a failed init or
 * append of that other buffer leaves the field NULL.
 */
static inline void ccol_growbuf_append_cstr(ccol_growbuf_t *b, const char *s) {
  if (!s) return;
  ccol_growbuf_append(b, s, strlen(s));
}

/**
 * @brief Free the backing store of the buffer (b->buf).
 * @note This function does not free b, which is usually on the stack or
 * inside another struct. A call on a buffer that is already in the OOM state
 * is safe, because b->buf can be NULL.
 */
static inline void ccol_growbuf_destroy(ccol_growbuf_t *b) {
  _ccol_mem_free(b->m_procs, b->buf);
}

#endif /* CCOL_CGROWBUF_H */
