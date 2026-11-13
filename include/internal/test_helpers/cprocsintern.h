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

// Internal: The test-only hooks of the procs intern table. src/common.c
// defines them and tests/common uses them to count, empty and slow down the
// slots. They exist only under RUNNING_UNIT_TESTS.

#include <internal/cprocsintern.h>

#ifdef RUNNING_UNIT_TESTS
// Gives how many slots of the intern table hold a copy, so a test can check
// that a content takes exactly one slot.
size_t ccol__procs_intern_used_for_tests(void);
// Empties every slot from index keep onward, so tests can fill the table
// without leaving it full for later tests. Call it only while no object holds
// a pointer into those slots.
void ccol__procs_intern_truncate_for_tests(size_t keep);
// While non-zero, a thread that claims a slot sleeps this many microseconds
// before it writes the copy, so other threads run into a slot being written.
extern _Atomic unsigned ccol__procs_intern_claim_delay_us_for_tests;
// How many times a lookup met a slot that another thread was still writing.
extern _Atomic unsigned ccol__procs_intern_writing_sightings_for_tests;
// While this and the delay are both non-zero, a claiming thread waits until
// the sightings reach this count before it writes; the delay bounds the wait.
extern _Atomic unsigned ccol__procs_intern_claim_wait_sightings_for_tests;
#endif /* RUNNING_UNIT_TESTS */
