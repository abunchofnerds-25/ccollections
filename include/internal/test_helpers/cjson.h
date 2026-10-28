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
 * @file test_helpers/cjson.h
 * @brief INTERNAL ONLY. The test-only hooks of cjson.
 *
 * src/cjson.c defines these hooks and the cjson tests use them to read the
 * node pool and to count work that the public API doesn't show.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <cjson.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
/* How many strings the UTF-8 check of a C string has checked, over all
 * threads. */
extern atomic_ulong _cjson_utf8_cstr_checks_for_tests;

/* How many nodes the cycle check of cjson_list_push() and
 * cjson_dictionary_set() has visited, over all threads. A test reads it
 * before and after a series of inserts to count the work of the search. */
extern atomic_ulong _cjson_cycle_search_nodes_for_tests;

/* How many times any thread has taken the pool key lock to arm the key. */
extern atomic_ulong _cjson_pool_key_lock_count_for_tests;

/* How many nodes the pools of all threads hold together. A pool that a thread
 * leaves behind at exit stays in this count. */
extern atomic_long _cjson_pool_population_for_tests;

/* The size of the node-pool free list of the calling thread, so a test can
 * check how the pool evicts a node at its cap and that one thread doesn't
 * see the pool of another. */
size_t cjson_debug_pool_size(void);
#endif /* RUNNING_UNIT_TESTS */
