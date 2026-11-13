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

// Internal only: The test-only check of the fork-handler order, and the
// counters of the close-on-exec gate. src/common.c defines them, and only a
// build with RUNNING_UNIT_TESTS has them, so none of this ships.
#include <common.h>
#include <stdatomic.h>

#ifdef RUNNING_UNIT_TESTS
// The modules that register a fork-prepare handler. Each handler takes its
// module's locks and holds them across the fork, so the locks nest in the order
// the handlers run (the reverse of registration). Two handlers that run in
// opposite relative orders in two forks make a real lock cycle, so each handler
// reports itself to the recorder below and a contradiction ends the process.
typedef enum {
  ccol__atfork_module_clogger = 0,
  ccol__atfork_module_cthreadcomm,
  ccol__atfork_module_cthreadpool,
  ccol__atfork_module_chttpserver,
  ccol__atfork_module_chttpclient,
  ccol__atfork_module_count
} ccol__atfork_module;

// Records that the prepare handler of ccol_module started. Every module's
// prepare handler calls it in a test build, so a lock cycle between two
// handlers fails the run instead of hanging a fork one day. It aborts when the
// order of any pair contradicts an earlier fork, or when the handler runs after
// the close-on-exec gate.
void ccol__atfork_order_record(ccol__atfork_module ccol_module);

// Starts a new sequence, so the next record counts as the first handler of a
// fork. The recorder's own parent and child handlers call it after every fork;
// a test that records sequences with no fork between calls it itself.
void ccol__atfork_order_reset(void);

#if defined(CCOL__CLOEXEC_GATE)
// How many times a thread entered and left the close-on-exec gate, so a test
// can check that every creation site uses it and leaves it on every path.
extern atomic_ulong ccol__cloexec_gate_enters_for_tests;
extern atomic_ulong ccol__cloexec_gate_leaves_for_tests;

// True from the gate's prepare handler until its parent or child handler, so a
// test can see whether the gate already ran in the current fork.
extern bool ccol__cloexec_gate_prepared_for_tests;

// How many times the gate's prepare handler started. It counts before the wait,
// so a test sees a fork() that waits at the gate.
extern atomic_ulong ccol__cloexec_gate_prepares_for_tests;
#endif
#endif
