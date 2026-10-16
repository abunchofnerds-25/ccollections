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
 * @file test_helpers/cstring.h
 * @brief INTERNAL ONLY. The test-only hooks of cstring.
 *
 * src/cstring.c defines these hooks and the cstring tests call them, so a test
 * can reach overflow guards that would otherwise need a string of several
 * gigabytes, and can see which search path cstring_rfind() took.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <cstring.h>

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Expose the internal capacity in bytes, for the tests
 *
 * @param s  String to query
 * @return   Number of bytes the data buffer holds right now
 */
size_t _cstring_get_capacity(cstr s);

/**
 * @brief Expose the internal guard against a size_t wraparound, for the
 * tests
 *
 * Every function that changes a string goes through this guard, which
 * rejects a length of exactly SIZE_MAX; without it, that length would wrap
 * silently to 0 when we add 1 for the null terminator. With this declaration
 * a test can drive the guard directly instead of needing a string that
 * fills the whole address space to reach it through the public API.
 *
 * @param length  Candidate content length, in bytes, without the '\0'
 * @return        true if length + 1 fits in a size_t, false if length is
 * exactly SIZE_MAX
 */
bool _cstring_length_fits_with_terminator_for_tests(size_t length);

/**
 * @brief Expose the overflow-checked length arithmetic of cstring_replace(),
 * for the tests
 *
 * With this declaration a test can drive the ccol_container_full overflow
 * guards of cstring_replace() directly, without a real string of many
 * gigabytes to reach them through the public API.
 *
 * @param orig_length  Length of the string before the replace
 * @param nlen         Needle length
 * @param rlen         Replacement length
 * @param count        Number of non-overlapping occurrences (must be > 0)
 * @param new_len_out  Receives the new length on ccol_success
 *
 * @return ccol_success on success
 * @return ccol_container_full if the computation overflows a size_t
 */
ccol_retval_t _cstring_replace_compute_new_length_for_tests(
    size_t orig_length, size_t nlen, size_t rlen, size_t count,
    size_t *new_len_out);

/* Counts the cstring_rfind() calls that hand the search over to the backward
 * scan for long strings, so a test can prove that its inputs reach that
 * path. */
extern unsigned long cstring_rfind_linear_count_for_tests;
#endif /* RUNNING_UNIT_TESTS */
