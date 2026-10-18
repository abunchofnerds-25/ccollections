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

/* Compile-time probe: cyaml_set nests inside its own argument, and inside the
 * argument of another public macro, under -Wshadow -Werror. Every expansion
 * gives its temporaries unique names, so a nested expansion never declares
 * a local that hides one of the enclosing expansion's. Without that, each
 * nesting below would be a -Wshadow error in a caller's build. The Makefile
 * compiles this file with -fsyntax-only and nothing here runs. */

#include <cyaml.h>

void cyaml_nesting_probe(cyaml doc);
void cyaml_nesting_probe(cyaml doc) {
  (void)cyaml_set(doc, "a", (int)cyaml_set(doc, "b", 1));
  (void)cyaml_set(doc, "c",
                  (int)cyaml_set(doc, "d", (int)cyaml_set(doc, "e", 2.5)));
  (void)cyaml_set(doc, "f", ({
                    (void)cyaml_set(doc, "g", "x");
                    (bool)true;
                  }));
}
