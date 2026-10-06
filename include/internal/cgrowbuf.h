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
 * appends an unknown quantity of data one piece at a time. A check of the
 * allocation after each append makes that code difficult to read. This buffer
 * doubles its capacity when it grows, and it latches one out-of-memory flag.
 * After the flag is set, every further operation on the buffer is a safe
 * no-op. The caller then checks a whole build pass one time, at the end.
 *
 * This header is internal. It carries no visibility block. make install does
 * not install it. Its symbols are absent from the dynamic symbol table of the
 * shared library.
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
 * @brief A byte buffer that grows dynamically. It is for code that builds
 * text (a serializer, or text that a parse accumulates) and that must append
 * an unknown quantity of data one piece at a time.
 *
 * The capacity doubles when the buffer grows. An allocation can fail, or a
 * requested size can be too large to represent. In each of these two cases,
 * oom latches to true. Every further ccol_growbuf_* operation on that buffer
 * is then a safe no-op. This lets a caller check for an error at the end of a
 * whole build pass, and not after each append.
 *
 * A caller reads buf, len and oom directly after the build is complete. buf
 * is NUL-terminated, but buf can be NULL when oom is set. cap and m_procs are
 * internal bookkeeping for growth. A caller must not read them.
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
 * @note This function sets the internal OOM flag of the buffer if the
 * allocation fails. Every further ccol_growbuf_* call on b is then a safe
 * no-op.
 */
void ccol_growbuf_init(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp);

/**
 * @brief The same as ccol_growbuf_init(), but it sizes the backing store in
 * advance. The store holds a minimum of `hint + 1` bytes, and never less than
 * 64 bytes. This is for a caller that already knows about how much content it
 * will append.
 * @param b    The buffer to initialize.
 * @param mp   The allocator for every growth and every append on this buffer.
 * @param hint The approximate number of content bytes that the caller
 *             expects. A hint of SIZE_MAX asks for a store of SIZE_MAX + 1
 *             bytes. That size is not representable. The function then sets
 *             the internal OOM flag of the buffer. It does not quietly
 *             allocate a different quantity.
 * @note This function sets the internal OOM flag of the buffer if the
 * allocation fails. Every further ccol_growbuf_* call on b is then a safe
 * no-op.
 */
void ccol_growbuf_init_hint(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp,
                            size_t hint);

/**
 * @brief Append n raw bytes. The buffer grows if this is necessary.
 * @param b    The buffer that receives the bytes.
 * @param data The bytes to append. data must be non-NULL and readable for n
 *             bytes. If n is 0, the function ignores data, and data can be
 *             NULL.
 * @param n    The number of bytes to append.
 * @note This function is a no-op after the internal OOM flag of the buffer is
 * set.
 * @note An append needs n + 1 bytes more than the content that the buffer
 * already holds. That total can be too large to represent. The append then
 * makes no copy and sets the internal OOM flag of the buffer. A failed
 * allocation has the same result.
 */
void ccol_growbuf_append(ccol_growbuf_t *b, const char *data, size_t n);

/** @brief Append a single byte. */
static inline void ccol_growbuf_append_c(ccol_growbuf_t *b, char c) {
  ccol_growbuf_append(b, &c, 1);
}

/**
 * @brief Append a NUL-terminated string.
 * @note The function treats a NULL s as an empty append. It does not call
 * strlen(NULL), which is undefined behavior. A caller can pass on the buf
 * field of another buffer. The init or the append of that other buffer can
 * fail and leave the field NULL. This function accepts that NULL on purpose.
 */
static inline void ccol_growbuf_append_cstr(ccol_growbuf_t *b, const char *s) {
  if (!s) return;
  ccol_growbuf_append(b, s, strlen(s));
}

/**
 * @brief Free the backing store of the buffer (b->buf).
 * @note This function does not free b. b is usually on the stack or inside
 * another struct. A call on a buffer that is already in the OOM state is
 * safe, because b->buf can be NULL.
 */
static inline void ccol_growbuf_destroy(ccol_growbuf_t *b) {
  _ccol_mem_free(b->m_procs, b->buf);
}

#endif /* CCOL_CGROWBUF_H */
