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

// cdebuglog: An opt-in diagnostic log buffer in RAM for chasing CI failures
// that are hard to reproduce and sensitive to timing, like a hang that shows up
// only under qemu-user. A fprintf(stderr, ...) costs one write(2) per line, and
// under an emulator that cost can disturb the timing you are looking at. This
// module appends lines to one shared buffer and writes it out in a few write()
// calls.
//
// It's never part of the library build: The src wildcard of the root Makefile
// leaves it out, and everything below compiles to nothing unless both
// RUNNING_UNIT_TESTS and CDEBUGLOG_ENABLED are defined. Every test Makefile
// defines the first one, so the second keeps the call sites in clogger.c and
// cthreadcomm.c from forcing every suite to link this file. A suite that wants
// the tool adds src/cdebuglog.c to its SRC_FILES and defines CDEBUGLOG_ENABLED.
// Take both out again once the issue is understood.
#ifndef CDEBUGLOG_H
#define CDEBUGLOG_H

#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)

#include <stddef.h>

// Appends one formatted line to the shared buffer without writing it out; fmt
// carries its own "\n". It's the print call for chasing a timing-sensitive
// failure, where a write(2) per line would disturb the timing. Many threads can
// call it at once, but not a signal handler (it takes a lock). A line that
// doesn't fit is dropped silently, which needs a burst of several MB between
// two flushes. The first call registers a flush with atexit(); a process that
// ends with _exit(), abort() or a signal must call cdebuglog_flush() itself, or
// the lines are lost.
__attribute__((format(printf, 1, 2))) void cdebuglog_write(const char *fmt,
                                                           ...);

// Writes the whole buffer to stderr with a raw write(2) and resets it. Call it
// before a path that skips atexit(), or where you want the lines out now. It
// takes the same lock as cdebuglog_write(), so each line goes out whole. Not
// for signal handlers.
void cdebuglog_flush(void);

#endif

#endif
