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

// INTERNAL ONLY: The test hooks of clrucache. src/clrucache.c defines them so a
// test can force a failure in the handle table, read the segments and waiters
// of a cache and hold a publishing thread at a chosen point. They exist only
// when RUNNING_UNIT_TESTS is defined, which the shipped library never does.
#include <clrucache.h>

#ifdef RUNNING_UNIT_TESTS
// The opaque cache, which the hooks below take by pointer.
struct clrucache;

// Gives the cache behind h with no pin, so a test has no unpin to forget; NULL
// where the real resolve would fail.
struct clrucache *clrucache__resolve_for_tests(ccache h);

// Makes the next push onto the handle table's free list fail like an
// allocation, then disarms itself, so a test reaches the recovery of a lost
// index without a real out-of-memory condition.
void clrucache__force_next_free_index_push_failure_for_tests(void);

// Gives the sum of the segment capacities and puts the number of segments in
// *out_segments, so a test can pin how a capacity is split.
size_t clrucache__segment_capacity_sum_for_tests(ccache cache,
                                                 size_t *out_segments);

// Gives the number of segments whose map hashes in chashmap's keyed mode and
// puts the number of segments in *out_segments, so a test can see the hash
// flooding defence switch on.
size_t clrucache__keyed_segment_maps_for_tests(ccache cache,
                                               size_t *out_segments);

// Give the number of indices on the handle table's free list and the number of
// slots the table holds, so a test can see that slots are reused and none is
// lost, whatever handles earlier tests held.
size_t clrucache__free_index_count_for_tests(void);
size_t clrucache__slot_table_capacity_for_tests(void);

// Gives the waiter references the entry of key_pair holds, or -1 when the cache
// doesn't hold the key, so a test knows that a getter really waits on an
// operation in flight.
int clrucache__key_waiters_for_tests(ccache cache, const cmap_pair *key_pair);

// Make a thread that publishes an entry hold its segment's mutex delay_us
// longer before it wakes the waiters, so a test can queue another thread for
// that mutex first; the delay stays until the test sets 0. The second call
// tells whether a publisher has entered the delay.
void ccache_test_set_post_publish_delay_us(unsigned int delay_us);
bool ccache_test_post_publish_delay_entered(void);
#endif
