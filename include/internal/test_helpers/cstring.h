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

// INTERNAL ONLY. The test-only hooks of cstring, defined in src/cstring.c. They
// let a test reach overflow guards that would otherwise need a string of
// several gigabytes, and see which search path cstring_rfind() took. They exist
// only when RUNNING_UNIT_TESTS is defined.

#include <cstring.h>

#ifdef RUNNING_UNIT_TESTS
// Gives the number of bytes the data buffer of s holds right now.
size_t cstring__get_capacity(cstr s);

// The guard that every changing function goes through: false when length is
// exactly SIZE_MAX, since adding 1 for the '\0' would wrap to 0.
bool cstring__length_fits_with_terminator_for_tests(size_t length);

// The overflow-checked length arithmetic of cstring_replace(). count must be
// more than 0. It gives ccol_success with the new length in *new_len_out, or
// ccol_container_full when the computation overflows a size_t.
ccol_retval cstring__replace_compute_new_length_for_tests(size_t orig_length,
                                                          size_t nlen,
                                                          size_t rlen,
                                                          size_t count,
                                                          size_t *new_len_out);

// Counts the cstring_rfind() calls that hand a long string over to the backward
// scan, so a test can prove that its inputs reach that path.
extern unsigned long cstring_rfind_linear_count_for_tests;
#endif  // RUNNING_UNIT_TESTS
