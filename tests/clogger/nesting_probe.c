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

/* Compile-time probe for the logging macros, in two builds that the Makefile
 * runs with -fsyntax-only; nothing here runs.
 *
 * Under -Wshadow -Werror, a logging macro nests inside the argument of
 * another logging macro and inside the argument of a value macro of another
 * module, so no expansion declares a local that hides one of an enclosing
 * expansion.
 *
 * Under -std=c11 -pedantic-errors, a call with a format string and no
 * argument after it compiles. The format is the first argument of the
 * variadic part of each macro, so no call needs the GNU comma swallow. */

#include <clogger.h>
#ifndef CLOGGER_PROBE_STRICT
#include <chashmap.h>
#endif

static int clog_probe_side(clog l) {
  ccol_log_debug(l, "side");
  return 1;
}

void clog_nesting_probe(clog l, int x, const char *s);
void clog_nesting_probe(clog l, int x, const char *s) {
  ccol_log_trace(l, "trace");
  ccol_log_debug(l, "debug");
  ccol_log_info(l, "info");
  ccol_log_warn(l, "warn");
  ccol_log_error(l, "error");
  ccol_log_alert(l, "alert");
  ccol_log_info(l, "%d %s", x, s);
  ccol_log_info(l, "%d", clog_probe_side(l));
  if (x == 42) ccol_log_fatal(l, "fatal");
#ifndef CLOGGER_PROBE_STRICT
  ccol_log_info(l, "%d", ({
                  ccol_log_warn(l, "inner %d", x);
                  x;
                }));
  chmap_construct(m, int, int);
  chmap_insert(m, 1, ({
                 ccol_log_info(l, "in a value macro %d", x);
                 2;
               }));
  chmap_destroy(m);
#endif
}
