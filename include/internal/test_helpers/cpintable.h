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
 * @file test_helpers/cpintable.h
 * @brief INTERNAL ONLY. The test-only failure injection of the pin table.
 *
 * src/cpintable.c defines these hooks. The modules that build their handle
 * tables on it use them to reach the rollback paths that only a real allocation
 * failure would otherwise take.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <internal/cpintable.h>

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Force the next ccol_pintable_publish() call to fail, once.
 *
 * Publishing is the last step of every handle acquisition, and its failure
 * path is the rollback the owning module runs to put a half-claimed slot back
 * on its free list. Without this, only a real allocation failure inside this
 * file reaches that path, and no allocator a caller passes can cause one,
 * because the chunk and stripe blocks come from plain calloc by design (a
 * reader indexes them without a lock).
 *
 * The forced failure happens before anything is stored, so the rollback sees
 * exactly the state a real allocation failure leaves. It fires once: the
 * next publish consumes the flag whether it succeeds or fails.
 *
 * It's compiled only under RUNNING_UNIT_TESTS, so it isn't in the shipped
 * library or in its dynamic symbol table.
 */
void _ccol_pintable_force_next_publish_failure_for_tests(void);

/**
 * @brief Force the next stripe-block allocation to fail.
 *
 * This is the one refusal that can happen after the table has published a
 * chunk, and the only way to get a table holding a chunk whose slot never
 * went live. Compiled only under RUNNING_UNIT_TESTS.
 */
void _ccol_pintable_force_next_stripe_alloc_failure_for_tests(void);
#endif /* RUNNING_UNIT_TESTS */
