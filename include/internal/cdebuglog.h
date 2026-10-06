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
 * cdebuglog: an opt-in in-RAM diagnostic log buffer that only a
 * RUNNING_UNIT_TESTS build can use. It helps you find a CI failure that is
 * difficult to reproduce and sensitive to timing (an intermittent hang, a
 * race that only shows itself under qemu-user emulation, and similar cases).
 *
 * A plain fprintf(stderr, ...) debug print costs a real write(2) syscall at
 * that moment, because stderr is unbuffered by default. Under an emulator
 * such as qemu-user, each such syscall pays a real, measurable translation
 * cost. That cost can itself disturb the exact timing that you investigate.
 * This module appends formatted lines to a fixed-size, process-wide,
 * lock-free in-RAM buffer instead. It writes the whole buffer to stderr in
 * one write() call, or in a small number of them, at explicit points that the
 * caller chooses.
 *
 * This is not part of the default build of the library, on purpose. The src
 * wildcard sweep of the root Makefile excludes it. Every declaration below
 * compiles to nothing unless BOTH RUNNING_UNIT_TESTS and CDEBUGLOG_ENABLED
 * are defined. It is scaffolding for work on one specific issue. It is not a
 * feature of the shipped library, and not a feature of an ordinary test
 * build.
 *
 * RUNNING_UNIT_TESTS alone does not turn it on, because every test suite
 * Makefile in this repository defines that macro unconditionally. The second,
 * narrower macro protects a call site in a widely-linked production file
 * (src/clogger.c, src/cthreadcomm.c). Without that macro, every OTHER test
 * suite that links that file must also link src/cdebuglog.c, and most of
 * those suites have no interest in this tool. A module or a test suite that
 * wants to use this tool does two things:
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
 * @brief Appends one formatted line to the shared in-RAM diagnostic buffer.
 *        It does not write the line out immediately.
 *
 * The fmt of the caller must contain its own "\n" at the end. This module
 * does one write() for each flush, and not one write() for each line. This
 * function is lock-free. One atomic fetch-add reserves a region of the
 * backing buffer for each writer, and those regions never overlap. This is
 * why a call from a signal handler is safe, and why a call from ordinary code
 * is safe. Concurrent calls from many threads are also safe.
 *
 * The function quietly drops a line that overflows the fixed backing buffer.
 * This prevents a torn write and an overlapping write. A caller that flushes
 * often (see cdebuglog_flush() below) makes such a drop very improbable.
 *
 * The first call in a process registers a flush at process exit with
 * atexit(). This covers an ordinary end of the process that does not bypass
 * atexit(). A process can also stop with _exit(), with abort(), or from an
 * uncaught signal. atexit() handlers do not run on any of those paths. A
 * caller in that situation must call cdebuglog_flush() explicitly at the
 * correct point. Without that call, the content is lost quietly.
 */
__attribute__((format(printf, 1, 2))) void cdebuglog_write(const char *fmt,
                                                           ...);

/**
 * @brief Flushes every byte in the buffer to stderr with one write() call.
 *        It then resets the buffer.
 *
 * This function uses only a raw write(2), and never stdio. This is why a call
 * from a signal handler is also safe. There is a race with a writer that
 * reserves a region while this function runs. A line that arrives in the
 * middle of a flush can appear in two parts, one in this output and one in
 * the next output. Such a line can also be corrupt. This is an accepted,
 * bounded risk: the instrumentation is only a best-effort diagnostic, and no
 * test assertion depends on it.
 */
void cdebuglog_flush(void);

#endif /* RUNNING_UNIT_TESTS && CDEBUGLOG_ENABLED */

#endif /* CDEBUGLOG_H */
