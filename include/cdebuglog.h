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

#ifndef CDEBUGLOG_H
#define CDEBUGLOG_H

/*
 * cdebuglog: an opt-in, RUNNING_UNIT_TESTS-only in-RAM diagnostic log buffer
 * for chasing hard-to-reproduce, timing-sensitive CI failures (an
 * intermittent hang, a race that only manifests under qemu-user emulation,
 * and similar).
 *
 * A plain fprintf(stderr, ...) debug print costs a real write(2) syscall on
 * the spot, since stderr is unbuffered by default; under an emulator like
 * qemu-user, every such syscall pays real, measurable translation overhead
 * that can itself perturb the exact timing being investigated. This module
 * instead appends formatted lines to a fixed-size, process-wide, lock-free
 * in-RAM buffer, and flushes the whole accumulated buffer to stderr in one
 * (or a small handful of) write() calls at explicit, caller-chosen points.
 *
 * This is deliberately NOT part of the library's default build (it is
 * excluded from the root Makefile's own src wildcard sweep) and every
 * declaration below compiles to nothing unless BOTH RUNNING_UNIT_TESTS and
 * CDEBUGLOG_ENABLED are defined: it is scaffolding for actively chasing a
 * specific issue, not a feature of the shipped library or of an ordinary
 * test build. RUNNING_UNIT_TESTS alone is not enough to activate it,
 * because it is unconditionally defined by every test suite's own Makefile
 * in this repository; a second, narrower macro is what keeps a call site in
 * a widely-linked production file (src/clogger.c, src/cthreadcomm.c) from
 * forcing every OTHER test suite that happens to link that file (most of
 * which have no interest in this tool at all) to also link
 * src/cdebuglog.c. A module or test suite that wants to use this tool:
 *  1. adds src/cdebuglog.c to its own build explicitly (see, for example,
 *     tests/clogger/Makefile's or tests/cthreadcomm/Makefile's own
 *     SRC_FILES), and
 *  2. defines CDEBUGLOG_ENABLED (alongside RUNNING_UNIT_TESTS, already
 *     defined by every test Makefile) for that build, e.g. via that same
 *     Makefile's own DEFINITIONS variable.
 */
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)

#include <stddef.h>

/**
 * @brief Appends one formatted line to the shared in-RAM diagnostic buffer
 *        instead of writing it out immediately.
 *
 * The caller's fmt must include its own trailing "\n" (matching this
 * module's underlying single write()-per-flush design, not one write() per
 * line). Lock-free (a single atomic fetch-add reserves each writer's own
 * non-overlapping region of the backing buffer), so this is safe to call
 * from a signal handler as well as from ordinary code, and safe to call
 * concurrently from multiple threads. A line that would overflow the fixed
 * backing buffer is silently dropped rather than risking a torn or
 * overlapping write; callers that flush regularly (see cdebuglog_flush()
 * below) make this exceedingly unlikely in practice.
 *
 * Registers a process-exit flush (atexit()) the first time this is called
 * in a given process, covering ordinary, non-_exit()-bypassing completion;
 * a caller whose process may terminate via _exit(), abort(), or an uncaught
 * signal instead must call cdebuglog_flush() explicitly at the relevant
 * point, or that content is silently lost (atexit() handlers do not run on
 * any of those paths).
 */
__attribute__((format(printf, 1, 2))) void cdebuglog_write(const char *fmt,
                                                           ...);

/**
 * @brief Flushes every byte accumulated so far to stderr in a single
 *        write() call, then resets the buffer.
 *
 * Uses only a raw write(2), never stdio, so this is also safe to call from
 * a signal handler. Racy against a writer concurrently reserving a region
 * while this runs (a line being appended mid-flush can end up split across
 * the flushed output and the next one, or occasionally garbled); this is an
 * accepted, bounded risk for best-effort diagnostic-only instrumentation
 * that is never load-bearing for any test's own assertions.
 */
void cdebuglog_flush(void);

#endif /* RUNNING_UNIT_TESTS && CDEBUGLOG_ENABLED */

#endif /* CDEBUGLOG_H */
