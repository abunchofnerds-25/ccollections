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
 * @file test_helpers/cmempool.h
 * @brief INTERNAL ONLY. The test-only hooks of cmempool.
 *
 * src/cmempool.c defines these hooks and the cmempool tests call them. They
 * read the state of the thread caches and put an entry into a state that no
 * correct sequence of calls can produce, so the tests can assert on
 * properties whose failure is otherwise silent.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <cmempool.h>

#ifdef RUNNING_UNIT_TESTS
/* Overwrites what a pool records about one of its entries, so a corruption
 * test can prove that the pool catches that state. The ranged form finds the
 * sub-pool that owns entry from its address, as the free path does. */
void _ccol_mempool_corrupt_entry_status_for_tests(ccol_mempool *mp, void *entry,
                                                  unsigned char value);
void _ccol_r_mempool_corrupt_entry_status_for_tests(ccol_r_mempool *rmp,
                                                    void *entry,
                                                    unsigned char value);

/* The magazines that mp has handed out right now, so a test can check that
 * the exit of a thread gives its slot back. */
size_t _ccol_mempool_live_magazines_for_tests(ccol_mempool *mp);

/* The magazines that the calling thread holds, across every pool, so a test
 * can check that a magazine orphaned by a destroyed pool gets reaped. */
size_t _ccol_mempool_thread_magazines_for_tests(void);

/* Runs what the module runs at unload while the code stays mapped, so a test
 * can check what a pool and a thread do after the unload. */
void _ccol_mempool_unload_cache_for_tests(void);

/* The slots that mp allocated above the advertised count; zero for a pool
 * with no cache. */
size_t _ccol_mempool_reserve_for_tests(ccol_mempool *mp);
#endif /* RUNNING_UNIT_TESTS */
