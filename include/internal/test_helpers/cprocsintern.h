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
 * @file test_helpers/cprocsintern.h
 * @brief INTERNAL ONLY. The test-only hooks of the procs intern table.
 *
 * src/common.c defines them, and the tests of tests/common use them to count,
 * empty and slow down the slots of the table.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <internal/cprocsintern.h>

#ifdef RUNNING_UNIT_TESTS
/* How many slots hold a copy. */
size_t _ccol_procs_intern_used_for_tests(void);
/* Empties every slot from index keep onward. Tests call it only while no
 * object holds a pointer into those slots. */
void _ccol_procs_intern_truncate_for_tests(size_t keep);
/* While this is non-zero, a thread that claims a slot sleeps for this many
 * microseconds before writing the copy, so a test can have other threads run
 * into the slot while it's being written. */
extern _Atomic unsigned _ccol_procs_intern_claim_delay_us_for_tests;
/* How many times a lookup met a slot that another thread was still writing. */
extern _Atomic unsigned _ccol_procs_intern_writing_sightings_for_tests;
/* While this and the delay are both non-zero, a thread that claims a slot
 * waits until the sightings reach this count before it writes the copy, and
 * the delay only bounds that wait. */
extern _Atomic unsigned _ccol_procs_intern_claim_wait_sightings_for_tests;
#endif /* RUNNING_UNIT_TESTS */
