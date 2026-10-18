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
 * @file test_helpers/common.h
 * @brief INTERNAL ONLY. The test-only check of the fork-handler order, and
 * the counters of the close-on-exec gate.
 *
 * src/common.c defines the recorder, and every module that registers a
 * fork-prepare handler (clogger, cthreadcomm, cthreadpool, chttpserver and
 * chttpclient) reports to it. It also defines the counters that the tests of
 * the close-on-exec gate read.
 *
 * All of it exists only in a build that defines RUNNING_UNIT_TESTS. Every test
 * suite defines it and the shipped library never does, so none of this is
 * installed or exported.
 */

#include <common.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
/*
 * The order of the fork-prepare handlers, checked rather than assumed.
 *
 * Every module here that registers a pthread_atfork() prepare handler takes
 * its own locks in that handler and holds them until after the fork, so the
 * handlers' locks nest in the order the handlers run, which is the reverse
 * of registration order. The module the embedding program happens to use
 * first decides the registration order, so each module forces its
 * dependencies to register ahead of it.
 *
 * Two handlers must never run in opposite relative orders in two different
 * forks. If they do, their locks nest both ways and the cycle is real, not
 * something a detector made up.
 *
 * Each handler reports itself here when it starts, and we record the order
 * of each pair and refuse a later contradiction, so the test suites check
 * that property on every fork they make instead of a comment just claiming
 * it. Only the test binaries get this code; the shipped library has none of
 * it.
 */
typedef enum {
  _ccol_atfork_module_clogger = 0,
  _ccol_atfork_module_cthreadcomm,
  _ccol_atfork_module_cthreadpool,
  _ccol_atfork_module_chttpserver,
  _ccol_atfork_module_chttpclient,
  _ccol_atfork_module_count
} _ccol_atfork_module_t;

void _ccol_atfork_order_record(_ccol_atfork_module_t ccol_module);

/* Starts a new sequence, so the next record is the first handler of a
 * fork. The parent and child handlers that the recorder registers call it
 * after every fork, and a test that records sequences with no fork in
 * between calls it between two sequences. */
void _ccol_atfork_order_reset(void);

#if defined(_CCOL_CLOEXEC_GATE)
/* How many times a thread entered and left the close-on-exec gate, so a test
 * can check that every creation site uses it and leaves it on every path. */
extern atomic_ulong _ccol_cloexec_gate_enters_for_tests;
extern atomic_ulong _ccol_cloexec_gate_leaves_for_tests;

/* True from the prepare handler of the gate until the parent or the child
 * handler, so a test can see whether the gate already ran in the current
 * fork. */
extern bool _ccol_cloexec_gate_prepared_for_tests;
#endif /* _CCOL_CLOEXEC_GATE */
#endif /* RUNNING_UNIT_TESTS */
