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

/* Compile-time probe: every public macro of this header must nest inside the
 * argument of another public macro, and inside its own argument, under -Wshadow
 * -Werror. The value macros give their temporaries a unique name for each
 * expansion, so a nested expansion never declares a local that hides one of the
 * enclosing expansion. Without that, every nesting below is a -Wshadow error in
 * a caller's build. The Makefile compiles this file with -fsyntax-only and
 * nothing here runs. */

#include <chashmap.h>
#include <clrucache.h>

void clru_nesting_probe(void);
void clru_nesting_probe(void) {
  clru_construct(c, int, int, 4, NULL, NULL, NULL);
  int got = 0, got2 = 0;
  (void)clru_set(c, 1, ((void)clru_get(c, 1, &got), got + 1));
  (void)clru_get(c, ((void)clru_get(c, 1, &got), got), &got2);
  (void)clru_set(c, ((void)clru_set(c, 2, 2), 3), ((void)clru_set(c, 4, 4), 5));
  chmap_construct(m, int, int);
  (void)clru_set(c, chmap_get(m, 1), chmap_get(m, 2));
  chmap_insert(m, 1, ((void)clru_get(c, chmap_get(m, 1), &got), got));
  chmap_destroy(m);
  clru_destroy(c);
}

/* A const value type, a const struct value type and a const char * key type.
 * clru_get() copies each value into a temporary of the unqualified value type,
 * so the copy compiles, and so does assigning it to a plain variable of that
 * type. */
struct clru_probe_pt {
  int x, y;
};
void clru_const_value_probe(void);
void clru_const_value_probe(void) {
  clru_construct(ci, int, const int, 4, NULL, NULL, NULL);
  int got = 0;
  (void)clru_set(ci, 1, 7);
  (void)clru_get(ci, 1, &got);
  clru_destroy(ci);
  clru_construct(cs, int, const struct clru_probe_pt, 4, NULL, NULL, NULL);
  struct clru_probe_pt pt = {1, 2};
  (void)clru_set(cs, 1, pt);
  (void)clru_get(cs, 1, &pt);
  clru_destroy(cs);
  clru_construct(cc, const char *, const char *, 4, NULL, NULL, NULL);
  const char *s = NULL;
  (void)clru_set(cc, "k", "v");
  (void)clru_get(cc, "k", &s);
  free((void *)s);
  clru_destroy(cc);
}
