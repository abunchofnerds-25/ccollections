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

/* Compile-time probe: every public macro of this header nests inside the
 * argument of another public macro, and inside its own argument, under
 * -Wshadow -Werror. The value macros name their temporaries uniquely for each
 * expansion, so no nested expansion declares a local that hides one of the
 * enclosing expansion. Without that, each nesting below is a -Wshadow error
 * in the build of a caller. The Makefile compiles this file with
 * -fsyntax-only; nothing here runs. */

#include <cbstmap.h>
#include <chashmap.h>

void cbmap_nesting_probe(void);
void cbmap_nesting_probe(void) {
  cbmap_construct(b, int, int);
  cbmap_insert(b, 1, 1);
  cbmap_insert(b, 1, cbmap_get(b, 1) + 1);
  cbmap_insert(b, cbmap_get(b, 1), cbmap_get(b, cbmap_get(b, 1)));
  (void)cbmap_get_ptr(b, cbmap_get(b, 1));
  (void)cbmap_get_ptr(b, *cbmap_get_ptr(b, 1));
  (void)cbmap_remove(b, cbmap_get(b, cbmap_remove(b, 9)));
  cbmap_insert(b, 2, ({
                 cbmap_insert(b, 3, 4);
                 5;
               }));
  chmap_construct(m, int, int);
  chmap_insert(m, cbmap_get(b, 1), cbmap_get(b, chmap_get(m, 1)));
  cbmap_insert(b, chmap_get(m, 1), chmap_get(m, cbmap_get(b, 1)));
  chmap_destroy(m);
  cbmap_destroy(b);
}
