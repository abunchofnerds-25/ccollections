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

/* The value macros of cyaml.h, compiled as C23 (-std=gnu2x). The classifier
 * of cyaml_set() maps the type of the C23 nullptr to CYAML_NULL, and the C23
 * `true` has the type bool, so it stores a CYAML_BOOL with no cast. The main
 * suite compiles as C11, where neither holds, which is why these tests live
 * in a binary of their own. */

#include <cyaml.h>
#include <tau/tau.h>

TAU_MAIN()

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && \
    ((defined(__clang__) && __clang_major__ >= 16) ||          \
     (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 13))
/* This test is non-vacuous: without the nullptr association, cyaml_set()
 * refuses nullptr with ccol_invalid_args and the leaf keeps its value. */
TEST(c23, nullptr_sets_the_leaf_to_null) {
  cyaml doc = cyaml_parse("{\"k\": 1}", NULL);
  ccol_retval_t rv =
      doc ? cyaml_set(doc, "k", nullptr) : ccol_not_enough_memory;
  cyaml leaf = doc ? cyaml_get(doc, "k") : NULL;
  bool is_null = leaf && cyaml_type(leaf) == CYAML_NULL;
  cyaml_destroy(doc);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_null);
}

TEST(c23, true_stores_a_bool) {
  cyaml doc = cyaml_parse("{\"k\": 1}", NULL);
  ccol_retval_t rv = doc ? cyaml_set(doc, "k", true) : ccol_not_enough_memory;
  cyaml leaf = doc ? cyaml_get(doc, "k") : NULL;
  bool is_bool = leaf && cyaml_type(leaf) == CYAML_BOOL && cyaml_bool_val(leaf);
  cyaml_destroy(doc);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_bool);
}
#else
/* A compiler without nullptr and without the C23 `true`: the binary still
 * checks that a NULL sets the leaf to CYAML_NULL. */
TEST(c23, null_sets_the_leaf_to_null) {
  cyaml doc = cyaml_parse("{\"k\": 1}", NULL);
  ccol_retval_t rv = doc ? cyaml_set(doc, "k", NULL) : ccol_not_enough_memory;
  cyaml leaf = doc ? cyaml_get(doc, "k") : NULL;
  bool is_null = leaf && cyaml_type(leaf) == CYAML_NULL;
  cyaml_destroy(doc);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_null);
}
#endif
