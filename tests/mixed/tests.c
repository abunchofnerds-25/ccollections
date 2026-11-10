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

#include <cbstmap.h>
#include <chashmap.h>
#include <cstring.h>
#include <cvector.h>
#include <stdio.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()

// Containers nested in other containers. The outer container stores only the
// handle (a pointer) of each inner one, so every test destroys the inner
// containers before the outer one.
//
// Vector of hash maps
TEST(vec_of_hmaps, int_int_maps_stored_and_retrieved) {
  cvec_construct(maps, chmap);

  for (int i = 0; i < 3; i++) {
    chmap_construct(m, int, int);
    for (int j = 0; j < 5; j++) {
      int key = i * 10 + j;
      int val = j * j;
      chmap_insert(m, key, val);
    }
    cvec_push(maps, m);
  }

  REQUIRE_EQ(cvec_size(maps), 3);

  for (int i = 0; i < 3; i++) {
    chmap inner = cvec_at(maps, i);
    chmap_redeclare(inner, int, int);
    REQUIRE_EQ(chashmap_elem_count(inner), 5);
    for (int j = 0; j < 5; j++) {
      int key = i * 10 + j;
      int val = chmap_get(inner, key);
      REQUIRE_EQ(val, j * j);
    }
  }

  for (size_t i = 0; i < cvec_size(maps); i++) {
    chmap_destroy(cvec_at(maps, i));
  }
  cvec_destroy(maps);
}

TEST(vec_of_hmaps, update_map_retrieved_from_vector) {
  cvec_construct(maps, chmap);

  for (int i = 0; i < 2; i++) {
    chmap_construct(m, char *, int);
    int v_a = i * 100;
    int v_b = i * 200;
    chmap_insert(m, "alpha", v_a);
    chmap_insert(m, "beta", v_b);
    cvec_push(maps, m);
  }

  chmap first = cvec_at(maps, 0);
  chmap_redeclare(first, char *, int);
  int new_val = 999;
  chmap_insert(first, "alpha", new_val);
  REQUIRE_EQ(chmap_get(first, "alpha"), 999);

  chmap second = cvec_at(maps, 1);
  chmap_redeclare(second, char *, int);
  REQUIRE_EQ(chmap_get(second, "alpha"), 100);
  REQUIRE_EQ(chmap_get(second, "beta"), 200);

  for (size_t i = 0; i < cvec_size(maps); i++) {
    chmap_destroy(cvec_at(maps, i));
  }
  cvec_destroy(maps);
}

TEST(vec_of_hmaps, delete_entry_in_inner_map) {
  cvec_construct(maps, chmap);

  for (int i = 0; i < 2; i++) {
    chmap_construct(m, int, int);
    for (int j = 1; j <= 4; j++) {
      int val = j * 10;
      chmap_insert(m, j, val);
    }
    cvec_push(maps, m);
  }

  chmap target = cvec_at(maps, 1);
  chmap_redeclare(target, int, int);
  int k2 = 2;

  REQUIRE_EQ(chmap_remove(target, k2), ccol_success);
  REQUIRE_EQ(chashmap_elem_count(target), 3);
  REQUIRE_EQ((void *)chmap_get_ptr(target, k2), NULL);

  chmap other = cvec_at(maps, 0);
  chmap_redeclare(other, int, int);
  REQUIRE_EQ(chashmap_elem_count(other), 4);

  for (size_t i = 0; i < cvec_size(maps); i++) {
    chmap_destroy(cvec_at(maps, i));
  }
  cvec_destroy(maps);
}

// Vector of BST maps; each inner map must traverse in sorted order
TEST(vec_of_bmaps, sorted_traversal_per_map) {
  cvec_construct(maps, cbmap);

  for (int i = 0; i < 3; i++) {
    cbmap_construct(bm, int, int);
    for (int j = 4; j >= 0; j--) {
      int key = i * 10 + j;
      int val = j * j;
      cbmap_insert(bm, key, val);
    }
    cvec_push(maps, bm);
  }

  REQUIRE_EQ(cvec_size(maps), 3);

  for (int i = 0; i < 3; i++) {
    cbmap bm = cvec_at(maps, i);
    cbmap_redeclare(bm, int, int);
    REQUIRE_EQ(cbstmap_elem_count(bm), 5);

    int prev_key = -1;
    ccol_for_each(bm, it, {
      const int *key = ccol_iter_key_ptr(it);
      REQUIRE_TRUE(*key > prev_key);
      prev_key = *key;
    });
    REQUIRE_EQ(prev_key, i * 10 + 4);
  }

  for (size_t i = 0; i < cvec_size(maps); i++) {
    cbmap_destroy(cvec_at(maps, i));
  }
  cvec_destroy(maps);
}

TEST(vec_of_bmaps, independent_maps_do_not_interfere) {
  cvec_construct(maps, cbmap);

  for (int i = 0; i < 2; i++) {
    cbmap_construct(bm, int, int);
    for (int k = 1; k <= 5; k++) {
      int val = k * (i + 1);
      cbmap_insert(bm, k, val);
    }
    cvec_push(maps, bm);
  }

  cbmap bm0 = cvec_at(maps, 0);
  cbmap_redeclare(bm0, int, int);
  cbmap bm1 = cvec_at(maps, 1);
  cbmap_redeclare(bm1, int, int);

  for (int k = 1; k <= 5; k++) {
    REQUIRE_EQ(cbmap_get(bm0, k), k);
    REQUIRE_EQ(cbmap_get(bm1, k), k * 2);
  }

  int k3 = 3;
  cbmap_remove(bm0, k3);
  REQUIRE_EQ(cbstmap_elem_count(bm0), 4);
  REQUIRE_EQ(cbstmap_elem_count(bm1), 5);

  for (size_t i = 0; i < cvec_size(maps); i++) {
    cbmap_destroy(cvec_at(maps, i));
  }
  cvec_destroy(maps);
}

// Hash map of vectors
TEST(hmap_of_vecs, category_to_int_list) {
  chmap_construct(hm, char *, cvec);

  cvec_construct(evens, int);
  for (int i = 0; i < 5; i++) cvec_push(evens, i * 2);
  chmap_insert(hm, "evens", evens);

  cvec_construct(odds, int);
  for (int i = 0; i < 5; i++) cvec_push(odds, i * 2 + 1);
  chmap_insert(hm, "odds", odds);

  REQUIRE_EQ(chashmap_elem_count(hm), 2);

  cvec ev = chmap_get(hm, "evens");
  cvec_redeclare(ev, int);
  REQUIRE_EQ(cvec_size(ev), 5);
  for (int i = 0; i < 5; i++) {
    REQUIRE_EQ(cvec_at(ev, i), i * 2);
  }

  cvec od = chmap_get(hm, "odds");
  cvec_redeclare(od, int);
  REQUIRE_EQ(cvec_size(od), 5);
  for (int i = 0; i < 5; i++) {
    REQUIRE_EQ(cvec_at(od, i), i * 2 + 1);
  }

  ccol_for_each(hm, it, {
    cvec *vp = ccol_iter_val_ptr(it);
    cvector_destroy(*vp);
  });
  chmap_destroy(hm);
}

TEST(hmap_of_vecs, append_to_inner_vector) {
  chmap_construct(hm, char *, cvec);

  cvec_construct(nums, int);
  cvec_push(nums, 10);
  cvec_push(nums, 20);
  chmap_insert(hm, "nums", nums);

  cvec stored = chmap_get(hm, "nums");
  cvec_redeclare(stored, int);
  cvec_push(stored, 30);
  cvec_push(stored, 40);

  cvec reread = chmap_get(hm, "nums");
  cvec_redeclare(reread, int);
  REQUIRE_EQ(cvec_size(reread), 4);
  REQUIRE_EQ(cvec_at(reread, 2), 30);
  REQUIRE_EQ(cvec_at(reread, 3), 40);

  ccol_for_each(hm, it, {
    cvec *vp = ccol_iter_val_ptr(it);
    cvector_destroy(*vp);
  });
  chmap_destroy(hm);
}

// BST map of vectors, in sorted key order
TEST(bmap_of_vecs, ordered_groups) {
  cbmap_construct(bm, int, cvec);

  int group_sizes[] = {3, 5, 2, 4};
  for (int g = 0; g < 4; g++) {
    cvec_construct(v, int);
    for (int k = 0; k < group_sizes[g]; k++) {
      cvec_push(v, g * 100 + k);
    }
    cbmap_insert(bm, g, v);
  }

  REQUIRE_EQ(cbstmap_elem_count(bm), 4);

  int expected_group = 0;
  ccol_for_each(bm, it, {
    const int *key = ccol_iter_key_ptr(it);
    REQUIRE_EQ(*key, expected_group);

    cvec *vp = ccol_iter_val_ptr(it);
    cvec_redeclare(*vp, int);
    REQUIRE_EQ(cvec_size(*vp), (size_t)group_sizes[expected_group]);
    REQUIRE_EQ(cvec_at(*vp, 0), expected_group * 100);

    expected_group++;
  });
  REQUIRE_EQ(expected_group, 4);

  ccol_for_each(bm, it, {
    cvec *vp = ccol_iter_val_ptr(it);
    cvector_destroy(*vp);
  });
  cbmap_destroy(bm);
}

// Hash map of hash maps
TEST(hmap_of_hmaps, nested_string_key_lookup) {
  chmap_construct(outer, char *, chmap);

  chmap_construct(fruits, char *, int);
  int v1 = 1, v2 = 2, v3 = 3;
  chmap_insert(fruits, "apple", v1);
  chmap_insert(fruits, "banana", v2);
  chmap_insert(fruits, "cherry", v3);
  chmap_insert(outer, "fruits", fruits);

  chmap_construct(vegs, char *, int);
  int v10 = 10, v20 = 20;
  chmap_insert(vegs, "carrot", v10);
  chmap_insert(vegs, "broccoli", v20);
  chmap_insert(outer, "vegs", vegs);

  REQUIRE_EQ(chashmap_elem_count(outer), 2);

  chmap inner_f = chmap_get(outer, "fruits");
  chmap_redeclare(inner_f, char *, int);
  REQUIRE_EQ(chashmap_elem_count(inner_f), 3);
  REQUIRE_EQ(chmap_get(inner_f, "apple"), 1);
  REQUIRE_EQ(chmap_get(inner_f, "banana"), 2);
  REQUIRE_EQ(chmap_get(inner_f, "cherry"), 3);

  chmap inner_v = chmap_get(outer, "vegs");
  chmap_redeclare(inner_v, char *, int);
  REQUIRE_EQ(chashmap_elem_count(inner_v), 2);
  REQUIRE_EQ(chmap_get(inner_v, "carrot"), 10);
  REQUIRE_EQ(chmap_get(inner_v, "broccoli"), 20);

  ccol_for_each(outer, it, {
    chmap *mp = ccol_iter_val_ptr(it);
    chmap_destroy(*mp);
  });
  chmap_destroy(outer);
}

TEST(hmap_of_hmaps, add_and_remove_inner_entries) {
  chmap_construct(outer, char *, chmap);

  chmap_construct(inner, int, int);
  for (int i = 1; i <= 5; i++) {
    int val = i * i;
    chmap_insert(inner, i, val);
  }
  chmap_insert(outer, "squares", inner);

  chmap sq = chmap_get(outer, "squares");
  chmap_redeclare(sq, int, int);
  int k6 = 6, v36 = 36;
  chmap_insert(sq, k6, v36);
  int k1 = 1;
  REQUIRE_EQ(chmap_remove(sq, k1), ccol_success);
  REQUIRE_EQ(chashmap_elem_count(sq), 5);

  chmap sq2 = chmap_get(outer, "squares");
  chmap_redeclare(sq2, int, int);
  REQUIRE_EQ((void *)chmap_get_ptr(sq2, k1), NULL);
  REQUIRE_EQ(chmap_get(sq2, k6), 36);

  ccol_for_each(outer, it, {
    chmap *mp = ccol_iter_val_ptr(it);
    chmap_destroy(*mp);
  });
  chmap_destroy(outer);
}

// BST map of BST maps; outer and inner traversals are both sorted
TEST(bmap_of_bmaps, nested_sorted_maps) {
  cbmap_construct(outer, int, cbmap);

  for (int i = 0; i < 3; i++) {
    cbmap_construct(inner, int, int);
    for (int j = 4; j >= 0; j--) {
      int key = j;
      int val = j + i * 10;
      cbmap_insert(inner, key, val);
    }
    cbmap_insert(outer, i, inner);
  }

  REQUIRE_EQ(cbstmap_elem_count(outer), 3);

  int expected_outer_key = 0;
  ccol_for_each(outer, it, {
    const int *outer_key = ccol_iter_key_ptr(it);
    REQUIRE_EQ(*outer_key, expected_outer_key);

    cbmap *inner_p = ccol_iter_val_ptr(it);
    cbmap_redeclare(*inner_p, int, int);
    REQUIRE_EQ(cbstmap_elem_count(*inner_p), 5);

    int expected_inner_key = 0;
    ccol_for_each(*inner_p, inner_it, {
      const int *k = ccol_iter_key_ptr(inner_it);
      REQUIRE_EQ(*k, expected_inner_key);
      int key_copy = *k;
      int v = cbmap_get(*inner_p, key_copy);
      REQUIRE_EQ(v, key_copy + expected_outer_key * 10);
      expected_inner_key++;
    });
    REQUIRE_EQ(expected_inner_key, 5);

    expected_outer_key++;
  });
  REQUIRE_EQ(expected_outer_key, 3);

  ccol_for_each(outer, it, {
    cbmap *inner_p = ccol_iter_val_ptr(it);
    cbmap_destroy(*inner_p);
  });
  cbmap_destroy(outer);
}

// Vector of vectors: a jagged 2D array whose rows differ in length
TEST(vec_of_vecs, jagged_2d_array) {
  cvec_construct(rows, cvec);

  for (int i = 0; i < 4; i++) {
    cvec_construct(row, int);
    for (int j = 0; j <= i; j++) {
      cvec_push(row, j);
    }
    cvec_push(rows, row);
  }

  REQUIRE_EQ(cvec_size(rows), 4);

  for (int i = 0; i < 4; i++) {
    cvec row = cvec_at(rows, i);
    cvec_redeclare(row, int);
    REQUIRE_EQ(cvec_size(row), (size_t)(i + 1));
    for (int j = 0; j <= i; j++) {
      REQUIRE_EQ(cvec_at(row, j), j);
    }
  }

  for (size_t i = 0; i < cvec_size(rows); i++) {
    cvec_destroy(cvec_at(rows, i));
  }
  cvec_destroy(rows);
}

TEST(vec_of_vecs, sort_rows_independently) {
  cvec_construct(matrix, cvec);

  int data0[] = {5, 3, 1, 4, 2};
  int data1[] = {9, 7, 8, 6};
  int data2[] = {2, 2, 2, 2, 2, 2};

  {
    cvec_construct(row, int);
    cvec_append_array(row, data0, 5);
    cvec_push(matrix, row);
  }
  {
    cvec_construct(row, int);
    cvec_append_array(row, data1, 4);
    cvec_push(matrix, row);
  }
  {
    cvec_construct(row, int);
    cvec_append_array(row, data2, 6);
    cvec_push(matrix, row);
  }

  for (size_t i = 0; i < cvec_size(matrix); i++) {
    cvec row = cvec_at(matrix, i);
    cvec_redeclare(row, int);
    cvec_sort(row);
  }

  cvec r0 = cvec_at(matrix, 0);
  cvec_redeclare(r0, int);
  REQUIRE_EQ(cvec_at(r0, 0), 1);
  REQUIRE_EQ(cvec_at(r0, 4), 5);
  for (size_t j = 1; j < cvec_size(r0); j++) {
    REQUIRE_TRUE(cvec_at(r0, j - 1) <= cvec_at(r0, j));
  }

  cvec r1 = cvec_at(matrix, 1);
  cvec_redeclare(r1, int);
  REQUIRE_EQ(cvec_at(r1, 0), 6);
  REQUIRE_EQ(cvec_at(r1, 3), 9);

  cvec r2 = cvec_at(matrix, 2);
  cvec_redeclare(r2, int);
  for (size_t j = 0; j < cvec_size(r2); j++) {
    REQUIRE_EQ(cvec_at(r2, j), 2);
  }

  for (size_t i = 0; i < cvec_size(matrix); i++) {
    cvec_destroy(cvec_at(matrix, i));
  }
  cvec_destroy(matrix);
}

TEST(vec_of_vecs, append_outer_vector_of_double_rows) {
  cvec_construct(outer, cvec);

  cvec_construct(r0, double);
  cvec_push(r0, 1.5);
  cvec_push(r0, 2.5);
  cvec_push(outer, r0);

  cvec_construct(r1, double);
  cvec_push(r1, 3.5);
  cvec_push(r1, 4.5);
  cvec_push(r1, 5.5);
  cvec_push(outer, r1);

  REQUIRE_EQ(cvec_size(outer), 2);

  cvec row0 = cvec_at(outer, 0);
  cvec_redeclare(row0, double);
  REQUIRE_EQ(cvec_size(row0), 2);
  REQUIRE_EQ(cvec_at(row0, 0), 1.5);

  cvec row1 = cvec_at(outer, 1);
  cvec_redeclare(row1, double);
  REQUIRE_EQ(cvec_size(row1), 3);
  REQUIRE_EQ(cvec_at(row1, 2), 5.5);

  for (size_t i = 0; i < cvec_size(outer); i++) {
    cvec_destroy(cvec_at(outer, i));
  }
  cvec_destroy(outer);
}

// Vector of cstrings
TEST(vec_of_cstrings, build_and_retrieve) {
  cvec_construct(strings, cstr);

  const char *words[] = {"hello", "world", "foo", "bar", "baz"};
  for (int i = 0; i < 5; i++) {
    cstr_construct(s, words[i]);
    cvec_push(strings, s);
  }

  REQUIRE_EQ(cvec_size(strings), 5);

  for (int i = 0; i < 5; i++) {
    cstr s = cvec_at(strings, i);
    REQUIRE_EQ(strcmp(cstring_c_str(s), words[i]), 0);
  }

  for (size_t i = 0; i < cvec_size(strings); i++) {
    cstr_destroy(cvec_at(strings, i));
  }
  cvec_destroy(strings);
}

TEST(vec_of_cstrings, modify_strings_through_vector) {
  cvec_construct(lines, cstr);

  const char *initials[] = {"foo", "bar", "baz"};
  for (int i = 0; i < 3; i++) {
    cstr_construct(s, initials[i]);
    cvec_push(lines, s);
  }

  for (size_t i = 0; i < cvec_size(lines); i++) {
    cstr s = cvec_at(lines, i);
    cstring_append(s, "_suffix");
  }

  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(lines, 0)), "foo_suffix"), 0);
  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(lines, 1)), "bar_suffix"), 0);
  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(lines, 2)), "baz_suffix"), 0);

  for (size_t i = 0; i < cvec_size(lines); i++) {
    cstr_destroy(cvec_at(lines, i));
  }
  cvec_destroy(lines);
}

TEST(vec_of_cstrings, dynamic_string_building) {
  cvec_construct(tokens, cstr);

  for (int i = 0; i < 4; i++) {
    cstr_construct(s, "item_");
    char num[8];
    snprintf(num, sizeof(num), "%d", i);
    cstring_append(s, num);
    cvec_push(tokens, s);
  }

  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(tokens, 0)), "item_0"), 0);
  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(tokens, 3)), "item_3"), 0);

  size_t total_len = 0;
  for (size_t i = 0; i < cvec_size(tokens); i++) {
    total_len += cstring_length(cvec_at(tokens, i));
  }
  REQUIRE_EQ(total_len, 24);

  for (size_t i = 0; i < cvec_size(tokens); i++) {
    cstr_destroy(cvec_at(tokens, i));
  }
  cvec_destroy(tokens);
}

// Hash map of vectors of cstrings
TEST(hmap_of_vec_of_cstrings, three_level_nesting) {
  chmap_construct(catalog, char *, cvec);

  cvec_construct(greetings, cstr);
  const char *gr_words[] = {"hi", "hello", "hey"};
  for (int i = 0; i < 3; i++) {
    cstr_construct(s, gr_words[i]);
    cvec_push(greetings, s);
  }
  chmap_insert(catalog, "greetings", greetings);

  cvec_construct(farewells, cstr);
  const char *fw_words[] = {"bye", "goodbye"};
  for (int i = 0; i < 2; i++) {
    cstr_construct(s, fw_words[i]);
    cvec_push(farewells, s);
  }
  chmap_insert(catalog, "farewells", farewells);

  REQUIRE_EQ(chashmap_elem_count(catalog), 2);

  cvec gv = chmap_get(catalog, "greetings");
  cvec_redeclare(gv, cstr);
  REQUIRE_EQ(cvec_size(gv), 3);
  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(gv, 0)), "hi"), 0);
  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(gv, 2)), "hey"), 0);

  cvec fv = chmap_get(catalog, "farewells");
  cvec_redeclare(fv, cstr);
  REQUIRE_EQ(cvec_size(fv), 2);
  REQUIRE_EQ(strcmp(cstring_c_str(cvec_at(fv, 1)), "goodbye"), 0);

  ccol_for_each(catalog, it, {
    cvec *vp = ccol_iter_val_ptr(it);
    cvec_redeclare(*vp, cstr);
    for (size_t i = 0; i < cvec_size(*vp); i++) {
      cstr_destroy(cvec_at(*vp, i));
    }
    cvector_destroy(*vp);
  });
  chmap_destroy(catalog);
}

// BST map (int key) of hash maps (char * key) of vectors
TEST(bmap_of_hmap_of_vecs, three_level_nesting) {
  cbmap_construct(outer, int, chmap);

  for (int g = 0; g < 2; g++) {
    chmap_construct(hm, char *, cvec);
    cvec_construct(va, int);
    cvec_construct(vb, int);

    if (g == 0) {
      for (int k = 0; k < 3; k++) cvec_push(va, k);
      for (int k = 3; k < 5; k++) cvec_push(vb, k);
      chmap_insert(hm, "a", va);
      chmap_insert(hm, "b", vb);
    } else {
      cvec_push(va, 10);
      cvec_push(va, 11);
      for (int k = 20; k < 24; k++) cvec_push(vb, k);
      chmap_insert(hm, "x", va);
      chmap_insert(hm, "y", vb);
    }

    cbmap_insert(outer, g, hm);
  }

  REQUIRE_EQ(cbstmap_elem_count(outer), 2);

  int k0 = 0;
  chmap hm0 = cbmap_get(outer, k0);
  chmap_redeclare(hm0, char *, cvec);
  cvec va0 = chmap_get(hm0, "a");
  cvec_redeclare(va0, int);
  REQUIRE_EQ(cvec_size(va0), 3);
  REQUIRE_EQ(cvec_at(va0, 1), 1);

  int k1 = 1;
  chmap hm1 = cbmap_get(outer, k1);
  chmap_redeclare(hm1, char *, cvec);
  cvec vy1 = chmap_get(hm1, "y");
  cvec_redeclare(vy1, int);
  REQUIRE_EQ(cvec_size(vy1), 4);
  REQUIRE_EQ(cvec_at(vy1, 3), 23);

  ccol_for_each(outer, it, {
    chmap *hmp = ccol_iter_val_ptr(it);
    chmap_redeclare(*hmp, char *, cvec);
    ccol_for_each(*hmp, it, {
      cvec *vp = ccol_iter_val_ptr(it);
      cvector_destroy(*vp);
    });
    chmap_destroy(*hmp);
  });
  cbmap_destroy(outer);
}
