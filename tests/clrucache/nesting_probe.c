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

// A compile-time probe: every public macro of clrucache.h must nest inside the
// argument of another public macro, and inside its own, under -Wshadow -Werror.
// The Makefile compiles this file with -fsyntax-only; nothing here runs.
#include <chashmap.h>
#include <clrucache.h>

void ccache_nesting_probe(void);
// Nests ccache_get, ccache_set and chmap macros inside each other's arguments.
// Each expansion names its temporaries uniquely, so no nested expansion hides a
// local of the one around it; otherwise each nesting is a -Wshadow error.
void ccache_nesting_probe(void) {
  ccache_construct(c, int, int, 4, NULL, NULL, NULL);
  int got = 0, got2 = 0;
  (void)ccache_set(c, 1, ((void)ccache_get(c, 1, &got), got + 1));
  (void)ccache_get(c, ((void)ccache_get(c, 1, &got), got), &got2);
  (void)ccache_set(c, ((void)ccache_set(c, 2, 2), 3),
                   ((void)ccache_set(c, 4, 4), 5));
  chmap_construct(m, int, int);
  (void)ccache_set(c, chmap_get(m, 1), chmap_get(m, 2));
  chmap_insert(m, 1, ((void)ccache_get(c, chmap_get(m, 1), &got), got));
  chmap_destroy(m);
  ccache_destroy(c);
}

// A small struct, used as a const value type.
struct ccache_probe_pt {
  int x, y;
};
void ccache_const_value_probe(void);
// Uses a const int value type, a const struct value type and a const char * key
// type. ccache_get() copies each value into a temporary of the unqualified
// type, so these must compile.
void ccache_const_value_probe(void) {
  ccache_construct(ci, int, const int, 4, NULL, NULL, NULL);
  int got = 0;
  (void)ccache_set(ci, 1, 7);
  (void)ccache_get(ci, 1, &got);
  ccache_destroy(ci);
  ccache_construct(cs, int, const struct ccache_probe_pt, 4, NULL, NULL, NULL);
  struct ccache_probe_pt pt = {1, 2};
  (void)ccache_set(cs, 1, pt);
  (void)ccache_get(cs, 1, &pt);
  ccache_destroy(cs);
  ccache_construct(cc, const char *, const char *, 4, NULL, NULL, NULL);
  const char *s = NULL;
  (void)ccache_set(cc, "k", "v");
  (void)ccache_get(cc, "k", &s);
  free((void *)(uintptr_t)s);
  ccache_destroy(cc);
}
