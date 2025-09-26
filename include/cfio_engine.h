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

#include <common.h>
#include <stdbool.h>

/**
 * @file cfio_engine.h
 * @brief INTERNAL ONLY. The single, process-wide, lazily-started facil.io
 *        ("facio") reactor lifecycle shared by chttpserver.c and
 *        chttpclient.c's async engine (Tier 2/3).
 *
 * This is not a public collections module: it has no macros, no opaque
 * handle type, and is never included by chttp.h/chttpclient.h/chttpserver.h
 * or any other public header. src/chttpserver.c and src/chttpclient.c are
 * the only two files that #include this header.
 *
 * facio's fio_data (third_party/facio/fio.c) is a process-wide singleton --
 * there can only be one fio_start()/fio_stop() pair alive per process. Prior
 * to this module, chttpserver.c and chttpclient.c each owned an independent
 * copy of the lazy-start/ref-counted-stop lifecycle pattern around that one
 * reactor, and a single process could only ever run one of the two engines
 * at a time (starting the second while the first was live would corrupt
 * shared facio state). This module merges that lifecycle into one place so
 * both can run simultaneously: chttpsvr_start()/__chttpsvr_destroy() and
 * chttpclient's _client_engine_acquire()/_client_engine_release() both
 * acquire/release references to the exact same underlying reactor here,
 * while each module keeps its own, unrelated bookkeeping layered on top
 * (chttpserver's per-instance g_server_count; chttpclient's per-request
 * chain refcounting, DNS/connect pool, and deadline sweep).
 *
 * Always calls http_lib_constructor() (declared in the vendored
 * third_party/facio/http_internal.h) as part of its one-time global init,
 * regardless of which module triggers the very first acquire -- required so
 * facio's HTTP-layer FIO_CALL_ON_INITIALIZE callback is queued before
 * fio_lib_init() fires initialisation callbacks, a hard ordering constraint
 * chttpserver's listeners need even if chttpclient happens to start the
 * shared engine first. This means chttpclient.c now transitively depends on
 * the same vendored HTTP-layer object files chttpserver.c already needed
 * (http.c, http1.c, http_internal.c, fiobj_*.c) -- see
 * tests/chttpclient/Makefile and tests/chttpclient_tls/Makefile, both
 * updated accordingly. The root Makefile already links every vendor file
 * unconditionally into the one shared library, so production builds are
 * unaffected.
 */

/**
 * @brief Acquires a reference to the shared reactor, starting it (and
 *        running the one-time http_lib_constructor()+fio_lib_init() global
 *        init) on the very first call from EITHER module, anywhere in the
 *        process. Blocks until the reactor's event loop is confirmed
 *        running (FIO_CALL_ON_START has fired) before returning -- callers
 *        never need their own separate readiness callback or wait loop.
 *
 * Must be paired with exactly one _cfio_engine_release() call.
 *
 * @return ccol_success, or ccol_not_enough_memory / ccol_unexpected_failure
 *         if the reactor could not be started on the first call.
 */
ccol_retval_t _cfio_engine_acquire(void);

/**
 * @brief Releases a reference acquired via _cfio_engine_acquire(). Once the
 *        reference count returns to zero, hands the actual teardown off to
 *        a freshly spawned reaper thread rather than performing it inline --
 *        essential, not a style choice: this can be called from inside a
 *        facio-owned callback thread (chttpclient's _async_on_close, in
 *        particular), and joining the reactor's own thread from inside one
 *        of its own callback threads deadlocks permanently (fio_start's
 *        internal fio_defer_thread_pool_join() waits for that very thread to
 *        finish, which it never will while blocked on the join). The reaper
 *        thread itself is joinable (not detached) so that
 *        _cfio_engine_wait_for_quiescence() / _cfio_engine_wait_until_
 *        stopped() can later wait for its own OS-level teardown to fully
 *        complete rather than just its work being logically done -- see
 *        those functions' own comments.
 *
 * Does not block. Callers that need a guaranteed-quiescent engine before
 * proceeding (tests between requests, or a real application at shutdown)
 * must call _cfio_engine_wait_for_quiescence() explicitly afterward.
 */
void _cfio_engine_release(void);

/**
 * @brief Blocks until any in-flight reaper-thread teardown (triggered by a
 *        prior _cfio_engine_release() call that dropped the reference count
 *        to zero, or by _cfio_engine_force_stop()) has fully finished. A
 *        no-op if the engine is not currently stopping (including if it
 *        isn't running at all, or is running and staying up because other
 *        references remain).
 */
void _cfio_engine_wait_for_quiescence(void);

/**
 * @brief Blocks the calling thread until the shared reactor is fully
 *        stopped -- whether a stop was already triggered before this call,
 *        is triggered concurrently, or is not triggered until sometime
 *        after this call returns control to no one, i.e. this blocks
 *        indefinitely if nothing ever stops the engine, exactly like
 *        joining the reactor's own thread would.
 *
 * Distinct from _cfio_engine_wait_for_quiescence(), which only blocks on an
 * *already in-flight* teardown and is a no-op otherwise (including while the
 * engine is running normally with references held) -- that is the right
 * primitive for "did the stop I just triggered finish yet", but wrong for
 * chttpsvr_engine_wait()'s documented contract of "block until the engine
 * exits", which must also cover the common pattern of calling it right
 * after chttpsvr_start() to block the main thread until an external event
 * (e.g. a SIGTERM handler calling chttpsvr_engine_stop()) stops the engine
 * at some later, unknown time.
 *
 * Safe to call even if the reactor was never started (returns immediately).
 */
void _cfio_engine_wait_until_stopped(void);

/**
 * @brief Returns true if the shared reactor is currently running (at least
 *        one reference held and fully started).
 *
 * Used by chttpsvr_set_engine_mem_mgmt_procs() to preserve its existing
 * "must be called before the engine ever starts" contract against the
 * shared reactor, regardless of which module started it.
 */
bool _cfio_engine_running(void);

/**
 * @brief Unconditionally forces the reference count to zero and triggers
 *        the same reaper-thread teardown _cfio_engine_release() uses, even
 *        if references are still outstanding.
 *
 * Non-blocking and safe to call from a signal handler (does not touch heap
 * memory, does not block) -- used by chttpsvr_engine_stop(), which
 * documents this exact contract for its own callers. Once the shared
 * reactor is used by both modules, this also tears down any in-flight
 * chttpclient async work in the same process; that is an inherent, correct
 * consequence of forcing a shared, process-wide reactor down for what is
 * meant to be process-shutdown-driven use, not a defect.
 */
void _cfio_engine_force_stop(void);
