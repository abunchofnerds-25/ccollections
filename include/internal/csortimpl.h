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
 * No program calls this function by this name; a user gets to it through
 * cvec_sort and cvector_sort_with_comparison_proc.
 *
 * Since the header is internal it carries no visibility block, make install
 * leaves it out, and its symbol isn't in the dynamic symbol table of the
 * shared library.
 */

#include <common.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* bool, true and false are keywords in C23 */
#endif
#include <stddef.h>

/**
 * @brief Stable sort of an array whose element i sits at
 *        base + i * elem_size
 *
 * For the same length, element size, comparator and allocator, the limits,
 * the return value and the order of the result are the same as those of
 * ___csort_merge_sort(). The difference is that this function reads and
 * writes the elements directly, so there's no call per element access.
 *
 * @param base First byte of the array. NULL, and a length of 0 or 1, give
 *             true and do nothing.
 * @param length Number of elements
 * @param elem_size Size of each element in bytes
 * @param comparison_proc Comparator; the function asserts when it is NULL
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
