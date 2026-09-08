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

/* See cdebuglog.h for the full rationale, including why both
 * RUNNING_UNIT_TESTS and CDEBUGLOG_ENABLED are required, not just the
 * former. Compiles to an empty translation unit unless both are defined,
 * and this file is deliberately excluded from the root Makefile's own src
 * wildcard sweep (see that Makefile's own SOURCE_FILES comment), so it is
 * never part of the default library build regardless. */
#if defined(RUNNING_UNIT_TESTS) && defined(CDEBUGLOG_ENABLED)

#include <cdebuglog.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* One shared, process-wide buffer rather than one per consumer module: every
 * caller's lines interleave in true chronological order once flushed, which
 * matters when correlating, say, a clogger.c rotation event against a
 * cthreadcomm.c fork() happening around the same time. Sized generously (a
 * full ~200-test suite run has been observed to accumulate several MB of
 * lines) since a caller that lets this fill up between flush points simply
 * starts silently dropping lines; see cdebuglog_write()'s own doc comment. */
#define CDEBUGLOG_BUF_CAP (8U * 1024U * 1024U)
/* Flushing only at fork()/exit() lets an entire crash-free run's worth of
 * lines pile up before any of it reaches stderr, which then dumps as one
 * huge burst interleaved at whatever point the first fork() happens to
 * land, rather than near the tests that actually produced each line. This
 * threshold makes cdebuglog_write() additionally trigger a flush once the
 * buffer crosses it, so a long run's output breaks into several
 * chronologically-meaningful bursts instead of one. Still far fewer write()
 * calls than one per line; only the granularity, not the "batch instead of
 * write per line" design, changes. */
#define CDEBUGLOG_FLUSH_THRESHOLD (CDEBUGLOG_BUF_CAP / 8U)
static char _cdebuglog_buf[CDEBUGLOG_BUF_CAP];
static _Atomic size_t _cdebuglog_pos = 0;
static _Atomic bool _cdebuglog_atexit_registered = false;

void cdebuglog_flush(void) {
  size_t n = atomic_exchange(&_cdebuglog_pos, 0);
  /* A burst of reservations past capacity keeps advancing the counter
   * without rolling back (each individually checks and drops its own write
   * in cdebuglog_write() below), so n can exceed the actual backing buffer
   * size; clamp before reading it, or this write() reads past the end of a
   * fixed-size static array. */
  if (n > CDEBUGLOG_BUF_CAP) n = CDEBUGLOG_BUF_CAP;
  if (n > 0) {
    ssize_t wn = write(STDERR_FILENO, _cdebuglog_buf, n);
    (void)wn; /* best-effort diagnostic flush; nothing to do on failure */
  }
}

static void _cdebuglog_atexit(void) { cdebuglog_flush(); }

void cdebuglog_write(const char *fmt, ...) {
  if (!atomic_exchange(&_cdebuglog_atexit_registered, true)) {
    atexit(_cdebuglog_atexit);
    /* One-shot process-startup context, captured the first time this module
     * is used in a given process: umask (read via the standard "set twice"
     * trick, since umask(2) has no query-only mode) and cwd. Rules an
     * inherited process-wide umask or working-directory oddity in or out as
     * an explanation for a directory permission mismatch observed later in
     * the same process, rather than leaving it a blind spot. */
    mode_t um = umask(0);
    umask(um);
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "(unknown)");
    cdebuglog_write("[DEBUG_PROCSTART] pid=%d umask=%03o cwd=%s\n",
                    (int)getpid(), (unsigned int)um, cwd);
  }

  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (len <= 0) return;
  size_t ulen = (size_t)len < sizeof line ? (size_t)len : sizeof line - 1;

  size_t start = atomic_fetch_add(&_cdebuglog_pos, ulen);
  if (start + ulen > CDEBUGLOG_BUF_CAP) return; /* dropped; see doc comment */
  memcpy(_cdebuglog_buf + start, line, ulen);

  /* Only the single write whose own reservation crosses the threshold
   * triggers a flush, so a burst of concurrent writers doesn't all call
   * flush at once; see CDEBUGLOG_FLUSH_THRESHOLD's own doc comment. */
  if (start < CDEBUGLOG_FLUSH_THRESHOLD &&
      start + ulen >= CDEBUGLOG_FLUSH_THRESHOLD)
    cdebuglog_flush();
}

#endif /* RUNNING_UNIT_TESTS && CDEBUGLOG_ENABLED */
