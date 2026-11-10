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

// Compile-time probe of the logging macros; the Makefile builds it twice with
// -fsyntax-only and nothing runs. Under -Wshadow -Werror the macros nest inside
// each other and inside a value macro of another module, so no expansion may
// hide a local of an enclosing one. Under -std=c11 -pedantic-errors
// (CLOGGER_PROBE_STRICT) a call with only a format string must compile.
#include <clogger.h>
#ifndef CLOGGER_PROBE_STRICT
#include <chashmap.h>
#endif

// Logs and returns 1, so a log call can sit inside another call's arguments.
static int clog_probe_side(clog l) {
  clog_debug(l, "side");
  return 1;
}

// Expands every logging macro, alone and nested, for the probe; it never runs.
void clog_nesting_probe(clog l, int x, const char *s);
void clog_nesting_probe(clog l, int x, const char *s) {
  clog_trace(l, "trace");
  clog_debug(l, "debug");
  clog_info(l, "info");
  clog_warn(l, "warn");
  clog_error(l, "error");
  clog_alert(l, "alert");
  clog_info(l, "%d %s", x, s);
  clog_info(l, "%d", clog_probe_side(l));
  if (x == 42) clog_fatal(l, "fatal");
#ifndef CLOGGER_PROBE_STRICT
  clog_info(l, "%d", ({
              clog_warn(l, "inner %d", x);
              x;
            }));
  chmap_construct(m, int, int);
  chmap_insert(m, 1, ({
                 clog_info(l, "in a value macro %d", x);
                 2;
               }));
  chmap_destroy(m);
#endif
}
