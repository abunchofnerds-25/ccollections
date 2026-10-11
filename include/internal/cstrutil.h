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

/**
 * @file cstrutil.h
 * @brief INTERNAL ONLY. Allocator-aware string helpers that this library's
 *        own modules share.
 *
 * Nothing here is part of the public interface, because no program uses
 * libccollections through these helpers. This is why the library never
 * installs this header, which also carries no visibility block.
 */

#include <common.h>
#include <string.h>

/**
 * @brief A strdup variant that uses the custom memory management procs
 *
 * This function copies the input string with the custom memory allocation
 * function and returns the copy.
 *
 * @param mp Pointer to the custom memory management procs
 * @param input The input string. A NULL input gives a NULL result, and the
 * function does not call strlen(NULL), which is undefined behavior. This lets
 * a caller pass on a string that another allocation already failed to make,
 * so the caller makes one check at the end instead of a check at each step.
 * @return The pointer to the new buffer that holds the copy of input. The
 * result is NULL if input is NULL, or if the memory allocation fails.
 *
 * Example:
 * @code
 * char* new_copy = ccol_strdup(mp, "hello");
 * @endcode
 */
static inline __attribute__((always_inline)) char *ccol_strdup(
    ccol_memmgmt_procs_t *mp, const char *input) {
  if (!input) return NULL;
  size_t len = strlen(input) + 1;  // The '\0' at the end
  char *result = (char *)_ccol_mem_alloc(mp, len * sizeof(char));
  if (result) {
    memcpy(result, input, len);
  }
  return result;
}
