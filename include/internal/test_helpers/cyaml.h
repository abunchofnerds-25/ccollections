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
 * @file test_helpers/cyaml.h
 * @brief INTERNAL ONLY. The test-only hooks of cyaml.
 *
 * src/cyaml.c defines these hooks and the cyaml tests use them. They read
 * the node pool, the parse budgets and the peak sizes of internal buffers,
 * and they lower the budget floors or force the degraded line-start path.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <cyaml.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
/* Runs each guarded helper of the growable buffer on a buffer in the state a
 * failed append leaves behind, and returns one bit per helper that answered
 * without reading anything: 1 for the trim, 2 for the append of one buffer
 * to another and 4 for the last-byte test, so 7 means all three. */
unsigned cyaml_debug_growbuf_guard_bits(void);

/* How many times any thread has taken the pool key lock to arm the key. */
extern atomic_ulong _cyaml_pool_key_lock_count_for_tests;

/* The size of the node-pool free list of the calling thread, so a test can
 * check how the pool evicts a node above its cap and that each thread keeps
 * its own pool. */
size_t cyaml_debug_pool_size(void);

/* The floors of the two parse limits. A test lowers them so a document of a
 * few kilobytes shows how the limits scale with the input, and puts them back
 * before it returns. */
extern size_t cyaml_test_parse_node_floor;
extern size_t cyaml_test_parse_byte_floor;

/* The budget that a parse of input_len bytes gets from a floor and a
 * per-byte factor. */
size_t cyaml_debug_parse_budget_for_input(size_t floor, size_t per_byte,
                                          size_t input_len);

/* What the last parse on this thread charged against its two budgets. A test
 * divides these by the input length to check the per-byte factors against
 * the worst honest shapes. */
extern __thread size_t cyaml_test_last_parse_nodes_charged;
extern __thread size_t cyaml_test_last_parse_bytes_charged;

/* The largest capacity that the frame stack of a parse reached on this
 * thread. A test resets it and checks it against _CYAML_PARSE_MAX_FRAMES. */
extern __thread size_t cyaml_test_peak_parse_frame_cap;

/* Forces the degraded line-start path without a cache, the same path that a
 * failed growth of the line-start cache latches. */
extern bool cyaml_test_force_line_cache_disabled;

/* The largest buffer, in bytes, that the canonical key builder allocated on
 * this thread. A test resets it to 0 before the parse it checks. */
extern __thread size_t _cyaml_canonical_key_peak_bytes_for_tests;

/* How many nodes the serializers of this thread have refused for nesting too
 * deep, or met after an earlier refusal or a failed allocation had already
 * spoiled the output. A walk that stops at its first refusal adds exactly 1
 * per refused tree. */
extern __thread size_t _cyaml_serialize_oom_visits_for_tests;
#endif /* RUNNING_UNIT_TESTS */
