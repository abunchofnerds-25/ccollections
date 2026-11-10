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

// Internal: test-only failure injection of the pin table, so the tests of the
// modules built on it reach their rollback paths. Only under
// RUNNING_UNIT_TESTS.

#include <internal/cpintable.h>

#ifdef RUNNING_UNIT_TESTS
// Makes the next ccol_pintable_publish() fail once, before it stores
// anything, so the tests of the owning modules reach the rollback that a real
// allocation failure would take. The chunk and stripe blocks come from plain
// calloc, so no caller's allocator can make it fail. The next publish consumes
// the flag either way.
void ccol__pintable_force_next_publish_failure_for_tests(void);

// Makes the next stripe-block allocation fail. Tests use it to get a published
// chunk whose slot never went live, which nothing else produces.
void ccol__pintable_force_next_stripe_alloc_failure_for_tests(void);
#endif
