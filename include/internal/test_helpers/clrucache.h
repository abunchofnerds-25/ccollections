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
 * @file test_helpers/clrucache.h
 * @brief INTERNAL ONLY. The test-only hooks of clrucache.
 *
 * src/clrucache.c defines these hooks and the clrucache tests call them.
 * They force a failure in the handle table, read the segments and the
 * waiters of a cache, and hold a publishing thread at a chosen point.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <clrucache.h>

#ifdef RUNNING_UNIT_TESTS
struct clrucache;

/* Resolves h to the cache behind it without a pin, so there is no unpin to
 * forget; NULL under the same conditions as the real resolve. */
struct clrucache *_clrucache_resolve_for_tests(clru_cache h);

/* Makes the very next push onto the free list of the handle table behave
 * exactly like an allocation failure, then disarms itself, so a test reaches
 * the recovery of a lost index without a real out-of-memory condition. */
void _clrucache_force_next_free_index_push_failure_for_tests(void);

/* The sum of the segment capacities, and the number of segments in
 * *out_segments, so a test can pin how a capacity is split. */
size_t _clrucache_segment_capacity_sum_for_tests(clru_cache cache,
                                                 size_t *out_segments);

/* The number of segments whose map hashes in the keyed mode of chashmap,
 * and the number of segments in *out_segments. */
size_t _clrucache_keyed_segment_maps_for_tests(clru_cache cache,
                                               size_t *out_segments);

/* The indices on the free list of the handle table, and the number of slots
 * the table holds, counting freed slots not reused yet. Read together, they
 * describe the table whatever handles earlier tests held. */
size_t _clrucache_free_index_count_for_tests(void);
size_t _clrucache_slot_table_capacity_for_tests(void);

/* How many waiter references the entry of key_pair holds right now, or -1
 * when the cache doesn't hold the key, so a test knows that a getter really
 * waits on an operation in flight. */
int _clrucache_key_waiters_for_tests(clru_cache cache,
                                     const cmap_pair *key_pair);

/* Makes a thread that publishes an entry hold the mutex of its segment for
 * delay_us more before it wakes the waiters, so another thread can queue for
 * that mutex first. It doesn't disarm itself: a test sets it back to 0. The
 * second call tells whether the publishing thread has entered that delay. */
void clru_test_set_post_publish_delay_us(unsigned int delay_us);
bool clru_test_post_publish_delay_entered(void);
#endif /* RUNNING_UNIT_TESTS */
