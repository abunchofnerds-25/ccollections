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
 * @file csortimpl.h
 * @brief INTERNAL ONLY. The contiguous-array mergesort that cvector uses for
 *        its own sort.
 *
 * No program uses this library through this function. A user reaches it
 * through cvec_sort and cvector_sort_with_comparison_proc, and never through
 * this name.
 *
 * This header is internal. It carries no visibility block. make install does
 * not install it. Its symbol is absent from the dynamic symbol table of the
 * shared library.
 */

#include <common.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>

/**
 * @brief Stable sort of an array whose element i sits at
 *        base + i * elem_size
 *
 * The limits, the return value and the order of the result are those of
 * ___csort_merge_sort(), for the same length, element size, comparator and
 * allocator. The function reads and writes each element directly, so it
 * makes no call for each element access.
 *
 * @param base First byte of the array. NULL, and a length of 0 or 1, give
 *             true and do nothing.
 * @param length Number of elements
 * @param elem_size Size of each element in bytes
 * @param comparison_proc Comparator. The function asserts when it is NULL
 *                        and there is something to sort.
 * @param mprocs Allocator of the temporary buffer, or NULL for the default
 *
 * @return true when the array is sorted, false when the length, the size of
 *         the buffer or mprocs is refused, or when the allocation fails. The
 *         array is unchanged whenever the function gives false.
 */
bool _csort_merge_sort_contiguous(void *base, size_t length, size_t elem_size,
                                  ccol_comparison_proc_t comparison_proc,
                                  ccol_memmgmt_procs_t *mprocs);

#ifdef RUNNING_UNIT_TESTS
/* The number of sorts that took the contiguous path and allocated a buffer,
 * across the process. A test uses it to prove that cvec_sort reaches this
 * path. */
unsigned long long _csort_contiguous_sorts_for_tests(void);
#endif
