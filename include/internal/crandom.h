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

/* Random bytes from the kernel, without blocking: getrandom(2) where the
 * system has it (Linux, FreeBSD), getentropy(3) on macOS, which serves at
 * most 256 bytes a call. It gives false when the bytes are not available,
 * and the caller then falls back to a mix of its own. */

#ifndef CCOL_INTERNAL_CRANDOM_H
#define CCOL_INTERNAL_CRANDOM_H

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <sys/random.h>
#include <sys/types.h>
#if defined(__APPLE__)
#include <unistd.h>
#endif

static inline bool ccol_random_bytes(void *buf, size_t len) {
#if defined(__APPLE__)
  return len <= 256 && getentropy(buf, len) == 0;
#else
  return getrandom(buf, len, GRND_NONBLOCK) == (ssize_t)len;
#endif
}

#endif /* CCOL_INTERNAL_CRANDOM_H */
