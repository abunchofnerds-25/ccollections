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
#include <sys/stat.h>
#include <unistd.h>

/* There is one shared, process-wide buffer instead of one buffer for each
 * consumer module, so the lines of every caller mix in true chronological
 * order after a flush. That order is important when you compare a rotation
 * event in clogger.c against a fork() in cthreadcomm.c at about the same
 * time. The buffer is large because a full run of about 200 tests
 * accumulates several MB of lines; a buffer that becomes full between two
 * flush points starts to drop lines quietly (see the doc comment of
 * cdebuglog_write()). */
#define CDEBUGLOG_BUF_CAP (8U * 1024U * 1024U)
/* A flush only at fork() or at exit() lets the lines of a whole crash-free
 * run collect before any of them reach stderr, so the output arrives as one
 * very large burst at the point where the first fork() happens, instead of
 * near the tests that made each line.
 *
 * With this threshold, cdebuglog_write() also starts a flush when the buffer
 * passes it, so the output of a long run breaks into several bursts, each
 * with a clear position in time. This costs many fewer write() calls than
 * one call for each line. The threshold sets the size of each batch; it does
 * not decide whether the module batches lines at all. */
#define CDEBUGLOG_FLUSH_THRESHOLD (CDEBUGLOG_BUF_CAP / 8U)
static char _cdebuglog_buf[CDEBUGLOG_BUF_CAP];
static _Atomic size_t _cdebuglog_pos = 0;
static _Atomic bool _cdebuglog_atexit_registered = false;

void cdebuglog_flush(void) {
  size_t n = atomic_exchange(&_cdebuglog_pos, 0);
  /* A burst of reservations past the capacity continues to advance the
   * counter, and no reservation rolls it back: each one checks and drops its
   * own write in cdebuglog_write() below. This is why n can be larger than
   * the real backing buffer, and why n must be clamped before the read;
   * without the clamp, this write() reads past the end of a fixed-size static
   * array. */
  if (n > CDEBUGLOG_BUF_CAP) n = CDEBUGLOG_BUF_CAP;
  if (n > 0) {
    ssize_t wn = write(STDERR_FILENO, _cdebuglog_buf, n);
    (void)wn; /* a best-effort diagnostic flush; no action on a failure */
  }
}

static void _cdebuglog_atexit(void) { cdebuglog_flush(); }

void cdebuglog_write(const char *fmt, ...) {
  if (!atomic_exchange(&_cdebuglog_atexit_registered, true)) {
    atexit(_cdebuglog_atexit);
    /* The context of the process start (the umask and the current working
     * directory), captured one time, at the first use of this module in a
     * process. The code reads the umask with the standard "set it twice"
     * method, because umask(2) has no query-only mode. When a directory
     * permission mismatch appears later in the same process, this context
     * shows whether an inherited process-wide umask, or a strange working
     * directory, explains it. */
    mode_t um = umask(0);
    umask(um);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "(unknown)");
    cdebuglog_write("[DEBUG_PROCSTART] pid=%d umask=%03o cwd=%s\n",
                    (int)getpid(), (unsigned int)um, cwd);
  }

  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (len <= 0) return;
  size_t ulen = (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1;

  size_t start = atomic_fetch_add(&_cdebuglog_pos, ulen);
  if (start + ulen > CDEBUGLOG_BUF_CAP) return; /* dropped, see doc comment */
  memcpy(_cdebuglog_buf + start, line, ulen);

  /* Only the one write whose own reservation crosses the threshold starts a
   * flush, so a burst of concurrent writers does not call flush many times at
   * once. See the doc comment of CDEBUGLOG_FLUSH_THRESHOLD. */
  if (start < CDEBUGLOG_FLUSH_THRESHOLD &&
      start + ulen >= CDEBUGLOG_FLUSH_THRESHOLD)
    cdebuglog_flush();
}

#endif /* RUNNING_UNIT_TESTS && CDEBUGLOG_ENABLED */
