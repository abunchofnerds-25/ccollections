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

#include <chashmap.h>
#include <cvector.h>

void cvec_nesting_probe(void);
void cvec_nesting_probe(void) {
  cvec_construct(v, int);
  cvec_push(v, 0);
  cvec_push(v, cvec_at(v, 0));
  cvec_push(v, cvec_at(v, cvec_at(v, 0)));
  cvec_push(v, cvec_pop(v));
  (void)cvec_find(v, cvec_at(v, 0));
  (void)cvec_find(v, (int)cvec_find(v, 0));
  cvec_reserve(v, (size_t)cvec_at(v, 0) + 8);
  int arr[2] = {1, 2};
  cvec_append_array(v, arr, (size_t)cvec_at(v, 0) + 1);
  cvec_push(v, ({
              cvec_push(v, 1);
              2;
            }));
  chmap_construct(m, int, int);
  cvec_push(v, chmap_get(m, cvec_at(v, 0)));
  chmap_insert(m, cvec_at(v, 0), cvec_at(v, chmap_get(m, 0)));
  chmap_destroy(m);
  cvec_destroy(v);
}
