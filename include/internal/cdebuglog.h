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
 * cdebuglog: an opt-in diagnostic log buffer in RAM that only a
 * RUNNING_UNIT_TESTS build can use. It's meant for the CI failures that are
 * hard to reproduce and sensitive to timing, like an intermittent hang or a
 * race that only shows up under qemu-user emulation.
 *
 * A plain fprintf(stderr, ...) debug print costs a real write(2) syscall on
 * the spot, because stderr is unbuffered by default. Under an emulator such
 * as qemu-user every one of those syscalls pays a measurable translation
 * cost, and that cost can disturb the very timing you are looking at. So
 * this module appends the formatted lines to a fixed-size buffer in RAM that
 * the whole process shares, and writes the buffer to stderr in one write()
 * call (or a few of them) at points that the caller picks.
 *
 * It isn't part of the default build of the library on purpose. The src
 * wildcard sweep of the root Makefile excludes it, and every declaration
 * below compiles to nothing unless BOTH RUNNING_UNIT_TESTS and
 * CDEBUGLOG_ENABLED are defined. Think of it as scaffolding for chasing one
 * specific issue, not as a feature of the shipped library or of an ordinary
 * test build, and take it out of the suite again once the issue is
 * understood.
 *
 * Defining RUNNING_UNIT_TESTS doesn't turn it on, because every test suite
 * Makefile in this repository defines that macro unconditionally. The second,
 * narrower macro protects the call sites in widely linked production files
 * (src/clogger.c, src/cthreadcomm.c). Without it every OTHER test suite that
 * links one of those files would also have to link src/cdebuglog.c, and
 * most of them have no use for this tool. A module or a test suite that
 * wants the tool does two things:
 *  1. it adds src/cdebuglog.c to its own build explicitly (see, for example,
 *     the SRC_FILES of tests/clogger/Makefile or of
 *     tests/cthreadcomm/Makefile), and
 *  2. it defines CDEBUGLOG_ENABLED for that build, for example in the
 *     DEFINITIONS variable of that same Makefile. Every test Makefile
 *     already defines RUNNING_UNIT_TESTS.
 */
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)

#include <stddef.h>

/**
 * @brief Appends one formatted line to the shared diagnostic buffer in RAM.
 *        The line isn't written out immediately.
 *
 * The caller's fmt must carry its own "\n" at the end. We do one write()
 * per flush instead of one per line. A short lock makes copying a line and
 * flushing exclusive, so many threads can call this at once and a flush
 * never writes a half-copied line. Calling it from a signal handler isn't
 * safe though, because the handler can interrupt a thread that holds that
 * lock.
 *
 * A line that doesn't fit in the fixed backing buffer is dropped silently.
 * Since the buffer is flushed whenever it passes an eighth of its size, a
 * drop needs a burst of several MB between two calls.
 *
 * The first call in a process registers a flush at exit with atexit(), which
 * covers any ordinary end of the process that doesn't bypass atexit(). A
 * process can also end with _exit(), with abort() or from an uncaught
 * signal, and no atexit() handler runs on those paths. A caller in that
 * situation must call cdebuglog_flush() itself at the right point, or the
 * content is lost without a word.
 */
__attribute__((format(printf, 1, 2))) void cdebuglog_write(const char *fmt,
                                                           ...);

/**
 * @brief Flushes every byte in the buffer to stderr with one write() call,
 *        and then resets the buffer.
 *
 * It only uses a raw write(2), never stdio, and it takes the same lock as
 * cdebuglog_write(), so each line goes out whole within one flush. Same as
 * cdebuglog_write(), it must not be called from a signal handler.
 */
void cdebuglog_flush(void);

#endif /* RUNNING_UNIT_TESTS && CDEBUGLOG_ENABLED */

#endif /* CDEBUGLOG_H */
