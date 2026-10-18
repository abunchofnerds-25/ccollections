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

/* See cdebuglog.h for the full reason, and for why the build needs both
 * RUNNING_UNIT_TESTS and CDEBUGLOG_ENABLED instead of only the first one.
 * This file compiles to an empty translation unit unless both are defined,
 * and the src wildcard sweep of the root Makefile also excludes it on purpose
 * (see the SOURCE_FILES comment of that Makefile), so it is never part of the
 * default library build. */
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)

#include <internal/cdebuglog.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

/* We keep one buffer for the whole process rather than one per consumer
 * module, so after a flush the lines of all callers appear in their real
 * chronological order. That matters when you compare a rotation event in
 * clogger.c with a fork() in cthreadcomm.c at about the same time. The
 * buffer is large because a full run of about 200 tests collects several MB
 * of lines, and once it gets full between two flush points it starts
 * dropping lines silently (see the doc comment of cdebuglog_write()). */
#define CDEBUGLOG_BUF_CAP (8U * 1024U * 1024U)
/* If we flushed only at fork() or exit(), the lines of a whole run without a
 * crash would pile up before any of them reached stderr, and the output
 * would come as one huge burst at the first fork() instead of near the tests
 * that wrote each line.
 *
 * With this threshold cdebuglog_write() also starts a flush once the buffer
 * passes it, so a long run comes out in several bursts, each with a clear
 * place in time, and still with far fewer write() calls than one per line.
 * The threshold only sets the size of each batch; batching itself happens
 * either way. */
#define CDEBUGLOG_FLUSH_THRESHOLD (CDEBUGLOG_BUF_CAP / 8U)
static char _cdebuglog_buf[CDEBUGLOG_BUF_CAP];
static size_t _cdebuglog_pos = 0;
static _Atomic bool _cdebuglog_atexit_registered = false;
/* One lock makes copying a line into the buffer and flushing the buffer
 * exclusive. A flush then never writes a line that another thread is still
 * copying, and a line never lands in a range that a flush has already given
 * back. */
static _ccol_mutex_t _cdebuglog_lock;
static _ccol_once_flag_t _cdebuglog_lock_once = _CCOL_ONCE_INIT;

static void _cdebuglog_lock_init(void) {
  (void)_ccol_mutex_init(_cdebuglog_lock);
}

/* Writes the buffer to stderr and empties it. The caller holds the lock. */
static void _cdebuglog_flush_locked(void) {
  if (_cdebuglog_pos > 0) {
    ssize_t wn = write(STDERR_FILENO, _cdebuglog_buf, _cdebuglog_pos);
    (void)wn; /* best-effort diagnostic flush, nothing to do on a failure */
  }
  _cdebuglog_pos = 0;
}

void cdebuglog_flush(void) {
  _ccol_call_once(_cdebuglog_lock_once, _cdebuglog_lock_init);
  _ccol_mutex_lock(_cdebuglog_lock);
  _cdebuglog_flush_locked();
  _ccol_mutex_unlock(_cdebuglog_lock);
}

static void _cdebuglog_atexit(void) { cdebuglog_flush(); }

void cdebuglog_write(const char *fmt, ...) {
  if (!atomic_exchange(&_cdebuglog_atexit_registered, true)) {
    atexit(_cdebuglog_atexit);
    /* The context at process start (the umask and the current working
     * directory), captured once, the first time a process uses this module.
     * If a directory permission mismatch shows up later in the same process,
     * it tells you whether an inherited process-wide umask or an odd working
     * directory explains it. We read the umask from the "Umask:" line of
     * /proc/self/status, because umask(2) can only be read by setting it,
     * and that would change the mode of every file another thread creates in
     * the meantime. Without /proc it isn't shown. */
    char um[16] = "unknown";
    FILE *st = fopen("/proc/self/status", "r");
    if (st) {
      char l[256];
      while (fgets(l, sizeof(l), st))
        if (sscanf(l, "Umask: %15s", um) == 1) break;
      fclose(st);
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "(unknown)");
    cdebuglog_write("[DEBUG_PROCSTART] pid=%d umask=%s cwd=%s\n", (int)getpid(),
                    um, cwd);
  }

  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (len <= 0) return;
  size_t ulen = (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1;

  _ccol_call_once(_cdebuglog_lock_once, _cdebuglog_lock_init);
  _ccol_mutex_lock(_cdebuglog_lock);
  if (_cdebuglog_pos + ulen <= CDEBUGLOG_BUF_CAP) {
    memcpy(_cdebuglog_buf + _cdebuglog_pos, line, ulen);
    _cdebuglog_pos += ulen;
  } /* else dropped, see the doc comment of cdebuglog_write() */
  /* Once the buffer is past the threshold it goes out in one burst; see the
   * doc comment of CDEBUGLOG_FLUSH_THRESHOLD. */
  if (_cdebuglog_pos >= CDEBUGLOG_FLUSH_THRESHOLD) _cdebuglog_flush_locked();
  _ccol_mutex_unlock(_cdebuglog_lock);
}

#endif /* RUNNING_UNIT_TESTS && CDEBUGLOG_ENABLED */
