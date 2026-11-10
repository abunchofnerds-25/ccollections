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

#pragma once

// INTERNAL ONLY. Allocator-aware string helpers that the modules of this
// library share. No program uses the library through them, so the header is
// never installed and has no visibility block.

#include <common.h>
#include <string.h>

// Copies a string through the allocator mp, so a module's copies come from the
// caller's allocator; strdup() through mp. It gives the copy, or NULL when
// input is NULL or the allocation fails. A NULL input is allowed so a caller
// can pass on the result of an earlier failed allocation and check once at the
// end.
static inline __attribute__((always_inline)) char *ccol_strdup(
    ccol_memmgmt_procs *mp, const char *input) {
  if (!input) return NULL;
  size_t len = strlen(input) + 1;
  char *result = (char *)ccol__mem_alloc(mp, len * sizeof(char));
  if (result) {
    memcpy(result, input, len);
  }
  return result;
}
