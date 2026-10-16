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
 * @file test_helpers/cvector.h
 * @brief INTERNAL ONLY. The test-only hooks of cvector.
 *
 * src/cvector.c defines these variables and the cvector tests read them, so
 * a test can compute the capacities it expects and see which copy branch
 * ran.
 *
 * The two constants exist in every build, as hidden symbols that the library
 * doesn't export. The counter exists only in a build that defines
 * RUNNING_UNIT_TESTS, which every test suite defines and the shipped library
 * never does. This header is never installed.
 */

#include <cvector.h>

#ifdef RUNNING_UNIT_TESTS
/* The smallest capacity of a vector and the factor by which it grows. */
extern const size_t _ccol_cvector_minimum_capacity;
extern const size_t _ccol_cvector_scaling_factor;

/* Counts the copies into the tail of a vector that take the branch that is
 * safe for an overlap, because the output of a copy alone doesn't show which
 * branch ran. */
extern size_t cvector_overlap_copy_count_for_tests;

/* The capacity of the backing buffer of v, so a test can check the growth
 * and shrink thresholds. */
size_t cvector_get_capacity(cvec v);
#endif /* RUNNING_UNIT_TESTS */
