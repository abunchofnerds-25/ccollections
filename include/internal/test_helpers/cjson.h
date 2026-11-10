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

// INTERNAL ONLY. The test-only hooks of cjson, defined in src/cjson.c. They
// exist only in a build with RUNNING_UNIT_TESTS, which the shipped library
// never defines.
#include <cjson.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
// How many C strings the UTF-8 check has checked, over all threads.
extern atomic_ulong cjson__utf8_cstr_checks_for_tests;

// How many nodes the cycle check of cjson_list_push() and
// cjson_dictionary_set() has visited, over all threads.
extern atomic_ulong cjson__cycle_search_nodes_for_tests;

// How many times any thread took the pool key lock to arm the key.
extern atomic_ulong cjson__pool_key_lock_count_for_tests;

// How many nodes the pools of all threads hold together. A pool that a thread
// leaves behind at exit stays in the count.
extern atomic_long cjson__pool_population_for_tests;

// Gives the size of the calling thread's node pool, so the white-box tests can
// check the cap and that threads don't share a pool.
size_t cjson_debug_pool_size(void);
#endif
