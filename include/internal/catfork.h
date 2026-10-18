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
 * @file catfork.h
 * @brief INTERNAL ONLY. Forced fork-handler registration of the modules that
 * other modules depend on.
 *
 * Fork-prepare handlers run in the reverse of their registration order, and
 * every module registers its handlers lazily, on its first use. A module that
 * holds another module's locks inside its own (chttpserver and chttpclient both
 * do this with cthreadcomm, cthreadpool and clogger) has to make those
 * dependencies register before it does, or the two sets of handler locks can
 * nest in opposite orders in two forks and deadlock.
 *
 * Each function below makes its module register its _ccol_at_fork() handlers
 * now, if it hasn't yet, and returns at once when it already has. Call all
 * three before your own _ccol_at_fork(). The doc comment of each definition
 * explains the ordering it protects, and the call site in
 * _chttpsvr_slot_table_init_globals in chttpserver.c describes the deadlock.
 *
 * None of this is public API and the library never installs or exports it. The
 * defining files include this header too, so a changed signature fails the
 * build instead of leaving a stale copy in a caller.
 */

/* cthreadcomm: ccol_event_loop and the queues. */
void _cthreadcomm_ensure_atfork_registered_before_caller(void);

/* cthreadpool: ctpool. */
void _ctpool_ensure_atfork_registered_before_caller(void);

/* clogger: clog. */
void _clog_ensure_atfork_registered_before_caller(void);
