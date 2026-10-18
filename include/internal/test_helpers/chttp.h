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
 * @file test_helpers/chttp.h
 * @brief INTERNAL ONLY. The test-only hooks of chttp.
 *
 * src/chttp.c defines this hook and the chttp tests call it, so a test can
 * reach an overflow guard that would otherwise need inputs of several
 * gigabytes.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <chttp.h>

#ifdef RUNNING_UNIT_TESTS
/* Runs the Basic auth encoder with the given fake lengths and tells whether
 * its overflow guard rejected the call. It's safe only with lengths that go
 * over the threshold of the guard, because the encoder would otherwise read
 * past the real, short strings. */
bool _chttp_basic_auth_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                const char *username,
                                                size_t fake_user_len,
                                                const char *password,
                                                size_t fake_pass_len);
#endif /* RUNNING_UNIT_TESTS */
