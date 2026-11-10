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

// See cdebuglog.h for why this file needs both RUNNING_UNIT_TESTS and
// CDEBUGLOG_ENABLED. Without both it's an empty translation unit.
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

// One buffer for the whole process, so the lines of all callers come out in
// their real order. It's large because a full run collects several MB.
#define CDEBUGLOG_BUF_CAP (8U * 1024U * 1024U)
// cdebuglog_write() flushes once the buffer passes this, so a long run comes
// out in several bursts near the tests that wrote them, instead of one burst at
// the first fork() or at exit.
#define CDEBUGLOG_FLUSH_THRESHOLD (CDEBUGLOG_BUF_CAP / 8U)
// The shared buffer, its fill level and whether the atexit flush is registered.
static char cdebuglog_buf[CDEBUGLOG_BUF_CAP];
static size_t cdebuglog_pos = 0;
static _Atomic bool cdebuglog_atexit_registered = false;
// Makes copying a line and flushing exclusive, so a flush never writes a
// half-copied line. It's initialized once through cdebuglog_lock_once.
static ccol__mutex cdebuglog_lock;
static ccol__once_flag cdebuglog_lock_once = CCOL__ONCE_INIT;

// Initializes cdebuglog_lock, once through cdebuglog_lock_once, because the
// library initializes no lock statically.
static void cdebuglog_lock_init(void) {
  (void)ccol__mutex_init(cdebuglog_lock);
}

// Writes cdebuglog_buf to stderr and empties it, for the flush entry points and
// the threshold flush of cdebuglog_write. A failed write is ignored, since this
// is a best-effort diagnostic. The caller holds cdebuglog_lock.
static void cdebuglog_flush_locked(void) {
  if (cdebuglog_pos > 0) {
    ssize_t wn = write(STDERR_FILENO, cdebuglog_buf, cdebuglog_pos);
    (void)wn;
  }
  cdebuglog_pos = 0;
}

// Writes the buffer to stderr and empties it, for a caller that is about to end
// the process without atexit() or wants its lines out now. It takes
// cdebuglog_lock, so no half-copied line goes out.
void cdebuglog_flush(void) {
  ccol__call_once(cdebuglog_lock_once, cdebuglog_lock_init);
  ccol__mutex_lock(cdebuglog_lock);
  cdebuglog_flush_locked();
  ccol__mutex_unlock(cdebuglog_lock);
}

// Flushes at exit, so the lines of an ordinary run aren't lost. cdebuglog_write
// registers it.
static void cdebuglog_atexit(void) { cdebuglog_flush(); }

// Formats one line and appends it to cdebuglog_buf under cdebuglog_lock. It's
// the logging call of a suite that chases a timing-sensitive failure, where a
// write per line would disturb the timing. It flushes once the buffer passes
// CDEBUGLOG_FLUSH_THRESHOLD, and drops a line that doesn't fit. The first call
// of a process sets cdebuglog_atexit_registered, registers the atexit flush and
// logs the umask and the working directory once, which helps explain a later
// permission mismatch. The umask comes from /proc/self/status, because umask(2)
// can only be read by setting it, and that would race with files other threads
// create.
void cdebuglog_write(const char *fmt, ...) {
  if (!atomic_exchange(&cdebuglog_atexit_registered, true)) {
    atexit(cdebuglog_atexit);
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

  ccol__call_once(cdebuglog_lock_once, cdebuglog_lock_init);
  ccol__mutex_lock(cdebuglog_lock);
  if (cdebuglog_pos + ulen <= CDEBUGLOG_BUF_CAP) {
    memcpy(cdebuglog_buf + cdebuglog_pos, line, ulen);
    cdebuglog_pos += ulen;
  }
  if (cdebuglog_pos >= CDEBUGLOG_FLUSH_THRESHOLD) cdebuglog_flush_locked();
  ccol__mutex_unlock(cdebuglog_lock);
}

#endif
