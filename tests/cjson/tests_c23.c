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

// The value macros of cjson.h compiled as C23 (-std=gnu2x). cjson_set() maps
// the C23 nullptr to CJSON_NULL, and the C23 true has the type bool, so it
// stores a CJSON_BOOL with no cast. Neither holds in C11, which the main suite
// uses, so these tests get their own binary.
#include <cjson.h>
#include <tau/tau.h>

TAU_MAIN()

#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && \
    ((defined(__clang__) && __clang_major__ >= 16) ||          \
     (!defined(__clang__) && defined(__GNUC__) && __GNUC__ >= 13))
// Without the nullptr association cjson_set() refuses nullptr with
// ccol_invalid_args and the leaf keeps its old value.
TEST(c23, nullptr_sets_the_leaf_to_null) {
  cjson doc = cjson_parse("{\"k\": 1}", NULL);
  ccol_retval rv = doc ? cjson_set(doc, "k", nullptr) : ccol_not_enough_memory;
  cjson leaf = doc ? cjson_get(doc, "k") : NULL;
  bool is_null = leaf && cjson_type(leaf) == CJSON_NULL;
  cjson_destroy(doc);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_null);
}

TEST(c23, true_stores_a_bool) {
  cjson doc = cjson_parse("{\"k\": 1}", NULL);
  ccol_retval rv = doc ? cjson_set(doc, "k", true) : ccol_not_enough_memory;
  cjson leaf = doc ? cjson_get(doc, "k") : NULL;
  bool is_bool = leaf && cjson_type(leaf) == CJSON_BOOL && cjson_bool_val(leaf);
  cjson_destroy(doc);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_bool);
}
#else
// A compiler with neither nullptr nor the C23 true checks at least that NULL
// sets the leaf to CJSON_NULL.
TEST(c23, null_sets_the_leaf_to_null) {
  cjson doc = cjson_parse("{\"k\": 1}", NULL);
  ccol_retval rv = doc ? cjson_set(doc, "k", NULL) : ccol_not_enough_memory;
  cjson leaf = doc ? cjson_get(doc, "k") : NULL;
  bool is_null = leaf && cjson_type(leaf) == CJSON_NULL;
  cjson_destroy(doc);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_null);
}
#endif
