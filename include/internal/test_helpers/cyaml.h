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

// Internal only: The test hooks of cyaml. src/cyaml.c defines them and the
// cyaml tests use them to read the node pool, the parse budgets and the peak
// sizes of internal buffers, and to lower the budget floors or force the
// degraded line-start path. They exist only when RUNNING_UNIT_TESTS is defined,
// which every test suite does and the shipped library never does.
#include <cyaml.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
// Runs each guarded growbuf helper on a buffer in the state a failed append
// leaves, so a test can prove none of them reads the missing store. Gives one
// bit per helper that read nothing (1 trim, 2 buffer append, 4 last-byte
// test); 7 means all three.
unsigned cyaml_debug_growbuf_guard_bits(void);

// How many times any thread took the pool key lock to arm the key, so a test
// can check the lock is taken once per thread.
extern atomic_ulong cyaml__pool_key_lock_count_for_tests;

// Gives the size of the calling thread's node pool, so a test can check the cap
// and that each thread keeps its own pool.
size_t cyaml_debug_pool_size(void);

// The floors of the two parse limits. A test lowers them so a small document
// shows how the limits scale with the input, and restores them afterwards.
extern size_t cyaml_test_parse_node_floor;
extern size_t cyaml_test_parse_byte_floor;

// Gives the budget a parse of input_len bytes gets from a floor and a per-byte
// factor, so a test can check the scaling and the saturation.
size_t cyaml_debug_parse_budget_for_input(size_t floor, size_t per_byte,
                                          size_t input_len);

// What the last parse on this thread charged to its two budgets, so a test can
// check the per-byte factors against the worst honest shapes.
extern __thread size_t cyaml_test_last_parse_nodes_charged;
extern __thread size_t cyaml_test_last_parse_bytes_charged;

// The largest frame stack capacity a parse reached on this thread; a test
// resets it and checks it against CYAML_PARSE_MAX_FRAMES.
extern __thread size_t cyaml_test_peak_parse_frame_cap;

// Forces the cache-less line-start path, the one a failed cache growth
// switches to, so a test can compare both paths.
extern bool cyaml_test_force_line_cache_disabled;

// The largest buffer the canonical key builder allocated on this thread; a test
// resets it and checks it stays near the key limit.
extern __thread size_t cyaml__canonical_key_peak_bytes_for_tests;

// How many nodes this thread's serializers refused for nesting too deep, or met
// after the output was already spoiled. A walk that stops at its first refusal
// adds exactly 1 per refused tree.
extern __thread size_t cyaml__serialize_oom_visits_for_tests;
#endif
