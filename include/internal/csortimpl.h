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

// INTERNAL ONLY. The contiguous-array mergesort that cvector uses for its own
// sort. A user reaches it through cvec_sort and
// cvector_sort_with_comparison_proc. The header is never installed and its
// symbol isn't exported.

#include <common.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <stddef.h>

// Sorts an array whose element i sits at base + i * elem_size, stably. cvector
// uses it for its own sort. For the same input it gives the same limits, return
// value and order as csort__merge_sort(), but it reads and writes the elements
// directly, with no call per element access. A NULL base or a length of 0 or 1
// gives true and does nothing; a NULL comparator asserts when there's something
// to sort. mprocs allocates the temporary buffer (NULL for the default). It
// gives false, with the array unchanged, when the length, the buffer size or
// mprocs is refused, or when the allocation fails.
bool csort__merge_sort_contiguous(void *base, size_t length, size_t elem_size,
                                  ccol_comparison_proc comparison_proc,
                                  ccol_memmgmt_procs *mprocs);
