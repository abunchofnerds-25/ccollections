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

// Test-only hooks of chttp, declared for src/chttp.c and the chttp tests. They
// exist only when RUNNING_UNIT_TESTS is defined, which the shipped library
// never does.

#include <chttp.h>

#ifdef RUNNING_UNIT_TESTS
// Runs the Basic auth encoder with fake string lengths and tells whether its
// overflow guard refused the call. It exists so a test reaches the guard
// without gigabytes of input. mp is the allocator, and the fake lengths
// replace the strlen() of username and password. Only lengths above the
// guard's threshold are safe; below it the encoder would read past the real,
// short strings.
bool chttp__basic_auth_overflow_guard_for_tests(ccol_memmgmt_procs *mp,
                                                const char *username,
                                                size_t fake_user_len,
                                                const char *password,
                                                size_t fake_pass_len);
#endif /* RUNNING_UNIT_TESTS */
