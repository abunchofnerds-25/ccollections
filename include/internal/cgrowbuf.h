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
 * Code that builds text (a serializer, or text that a parse collects) appends
 * an unknown amount of data piece by piece, and checking the allocation
 * after every append makes that code hard to read. This buffer doubles its
 * capacity when it grows and latches a single out-of-memory flag. Once the
 * flag is set every further operation on the buffer is a safe no-op, so the
 * caller only checks once, at the end of the whole build pass.
 *
 * The header is internal: it has no visibility block, make install doesn't
 * install it, and its symbols aren't in the dynamic symbol table of the
 * shared library.
 */

#ifndef CCOL_CGROWBUF_H
#define CCOL_CGROWBUF_H

#include <common.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief A byte buffer that grows dynamically, for code that builds text (a
 * serializer, or text that a parse collects) and has to append an unknown
 * amount of data piece by piece.
 *
 * The capacity doubles when the buffer grows. When an allocation fails, or a
 * requested size is too large to represent, oom latches to true and every
 * further ccol_growbuf_* operation on that buffer is a safe no-op, so a
 * caller can check for an error at the end of a whole build pass instead of
 * after each append.
 *
 * Once the build is done the caller reads buf, len and oom directly. buf is
 * NUL-terminated but can be NULL when oom is set. cap and m_procs are
 * internal bookkeeping for growth and a caller must not read them.
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
 * @note When the allocation fails the buffer's internal OOM flag gets set,
 * and every further ccol_growbuf_* call on b is then a safe no-op.
 */
void ccol_growbuf_init(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp);

/**
 * @brief Like ccol_growbuf_init(), but the backing store is sized up front,
 * for a caller that already knows roughly how much content it will append.
 * The store holds at least `hint + 1` bytes, and never less than 64 bytes.
 * @param b    The buffer to initialize.
 * @param mp   The allocator for every growth and every append on this buffer.
 * @param hint The approximate number of content bytes that the caller
 *             expects. A hint of SIZE_MAX asks for a store of SIZE_MAX + 1
 *             bytes, which can't be represented, so the buffer's internal
 *             OOM flag is set instead of quietly allocating some other
 *             amount.
 * @note When the allocation fails the buffer's internal OOM flag gets set,
 * and every further ccol_growbuf_* call on b is then a safe no-op.
 */
void ccol_growbuf_init_hint(ccol_growbuf_t *b, ccol_memmgmt_procs_t *mp,
                            size_t hint);

/**
 * @brief Append n raw bytes, growing the buffer if needed.
 * @param b    The buffer that receives the bytes.
 * @param data The bytes to append. data must be non-NULL and readable for n
 *             bytes, unless n is 0: data is ignored then and can be NULL.
 * @param n    The number of bytes to append.
 * @note This is a no-op once the buffer's internal OOM flag is set.
 * @note An append needs n + 1 bytes more than the content the buffer already
 * holds. When that total is too large to represent, nothing is copied and the
 * buffer's internal OOM flag is set, the same as for a failed allocation.
 */
void ccol_growbuf_append_slow(ccol_growbuf_t *b, const char *data, size_t n);

/* The common case (n bytes that fit in the room already allocated) runs
 * inline, so appending a literal or one byte costs a few moves and no call.
 * Growth, an empty append and the OOM state go to ccol_growbuf_append_slow,
 * which applies every rule documented above. */
static inline void ccol_growbuf_append(ccol_growbuf_t *b, const char *data,
                                       size_t n) {
  /* No object is larger than PTRDIFF_MAX, so this bound changes nothing at
   * run time. It's there to tell the compiler that the memcpy below never
   * sees a larger length; without it a constant length near SIZE_MAX (an
   * append that the slow path refuses anyway) makes it report one. */
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

/** @brief Append one byte. */
static inline void ccol_growbuf_append_c(ccol_growbuf_t *b, char c) {
  ccol_growbuf_append(b, &c, 1);
}

/**
 * @brief Append a NUL-terminated string.
 * @note A NULL s is treated as an empty append rather than calling
 * strlen(NULL), which is undefined behavior. We accept that NULL on purpose:
 * a caller can pass the buf field of another buffer along, and a failed init
 * or append of that other buffer leaves the field NULL.
 */
static inline void ccol_growbuf_append_cstr(ccol_growbuf_t *b, const char *s) {
  if (!s) return;
  ccol_growbuf_append(b, s, strlen(s));
}

/**
 * @brief Free the buffer's backing store (b->buf).
 * @note b itself isn't freed, it usually lives on the stack or inside
 * another struct. Calling this on a buffer that is already in the OOM state
 * is safe, since b->buf can be NULL there.
 */
static inline void ccol_growbuf_destroy(ccol_growbuf_t *b) {
  _ccol_mem_free(b->m_procs, b->buf);
}

#endif /* CCOL_CGROWBUF_H */
