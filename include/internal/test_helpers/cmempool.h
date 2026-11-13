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

// Internal: The test-only hooks of cmempool. src/cmempool.c defines them, and
// the tests use them to read the thread caches and to put an entry into a
// state no correct sequence of calls produces. Only under RUNNING_UNIT_TESTS.

#include <cmempool.h>

#ifdef RUNNING_UNIT_TESTS
// Overwrites the recorded status of entry with value, so a corruption test can
// prove the pool catches a state that no correct call sequence produces. The
// ranged form finds the owning tier from the address, as the free path does.
void cmempool__corrupt_entry_status_for_tests(cmempool *mp, void *entry,
                                              unsigned char value);
void crmempool__corrupt_entry_status_for_tests(crmempool *rmp, void *entry,
                                               unsigned char value);

// Gives the magazines mp has handed out right now, so a test can check that a
// thread's exit gives its slot back.
size_t cmempool__live_magazines_for_tests(cmempool *mp);

// Gives the magazines the calling thread holds across every pool, so a test
// can check that orphans of destroyed pools get reaped.
size_t cmempool__thread_magazines_for_tests(void);

// Runs what the module runs at unload while the code stays mapped, so a test
// can check what pools and threads do afterwards.
void cmempool__unload_cache_for_tests(void);

// Gives the slots mp allocated above its advertised count (0 without a
// cache), so a test can check the reserve.
size_t cmempool__reserve_for_tests(cmempool *mp);
#endif
