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
#include <common_invariants.h>
#include <internal/test_helpers/cbstmap.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <test_coverage.h>
#include <test_signals.h>

TAU_MAIN()

// Resets the signal state this binary inherited, before any test runs. The
// forked children report through their wait status, which an inherited signal
// state can break; test_reset_inherited_signals() explains how.
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

TEST(cbst_maps, create_fails) {
  char *err = NULL;
  cbstmap *cbmap = cbstmap_create(ccol_int, &err);
  REQUIRE_NE((void *)cbmap, NULL);
  cbmap_destroy(cbmap);

  cbmap = cbstmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs){
          .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  cbmap = cbstmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  cbmap = cbstmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  cbmap = cbstmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cbst_maps, create_succeeds) {
  char *err = "";
  cbstmap *cbmap = cbstmap_create(ccol_int, &err);
  REQUIRE_NE((void *)cbmap, NULL);
  REQUIRE_EQ((void *)err, NULL);

  cbmap_destroy(cbmap);
  REQUIRE_EQ((void *)cbmap, NULL);

  cbmap = cbstmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err);

  cbmap_destroy(cbmap);
  REQUIRE_EQ((void *)cbmap, NULL);
}

// Raw-layer wrappers for an int -> int map, so the raw tests don't build pairs
// by hand: Insert, copy out, get a pointer to the value (0 on a hit, -1 on a
// miss) and delete. Each gives the result of the raw call.
int insert_int_to_int(cbstmap *cbmap, int key, int val) {
  return cbstmap_insert_elem(
      cbmap, &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)},
      &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)});
}

int get_int_from_int(cbstmap *cbmap, int key, int *val_ptr) {
  return cbstmap_get_elem_copy(
      cbmap, &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)}, val_ptr,
      sizeof(int));
}

int get_int_ref_from_int(cbstmap *cbmap, int key, int **val_ptr) {
  cmap_pair tmp_val_pair_ptr = {NULL, 0};
  if (cbstmap_get_elem_ref(
          cbmap, &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)},
          &tmp_val_pair_ptr) == 0) {
    *val_ptr = (int *)tmp_val_pair_ptr.ptr;
    return 0;
  }
  return -1;
}

ccol_retval delete_int_from_int(cbstmap *cbmap, int key) {
  return cbstmap_remove_elem(
      cbmap, &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)});
}

// A copy into typeof(val) would store the raw bytes of a same-width expression,
// so 1.5f would read back as 1069547520.
TEST(cbst_maps, value_converts_to_the_declared_type_and_is_not_reinterpreted) {
  cbmap_construct(b, char *, int);
  float f = 1.5f;
  cbmap_insert(b, "k", f);
  REQUIRE_EQ(cbmap_get(b, "k"), 1);
  cbmap_destroy(b);
}

TEST(cbst_maps, key_converts_to_the_declared_type_and_stays_findable) {
  cbmap_construct(b, int, int);
  float fk = 2.0f;
  int v = 7;
  cbmap_insert(b, fk, v);
  int ik = 2;
  int *p = cbmap_get_ptr(b, ik);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(*p, 7);
  REQUIRE_EQ(cbstmap_size(b), (size_t)1);
  cbmap_destroy(b);
}

TEST(cbst_maps, narrower_value_widens_to_the_declared_type) {
  cbmap_construct(b, char *, int);
  short sv = -1;
  cbmap_insert(b, "k", sv);
  REQUIRE_EQ(cbmap_get(b, "k"), -1);
  cbmap_destroy(b);
}

TEST(cbst_maps, rvalue_key_and_value_accepted) {
  cbmap_construct(b, char *, int);
  cbmap_insert(b, "alice", 42);
  cbmap_insert(b, "bob", 6 * 7);
  REQUIRE_EQ(cbmap_get(b, "alice"), 42);
  REQUIRE_EQ(cbmap_get(b, "bob"), 42);
  REQUIRE_EQ(cbmap_remove(b, "bob"), ccol_success);
  cbmap_destroy(b);

  cbmap_construct(n, long, long);
  cbmap_insert(n, 1 + 1, 100L);
  REQUIRE_EQ(cbmap_get(n, 2), 100L);
  cbmap_destroy(n);
}

TEST(cbst_maps, array_key_and_string_literal_key_agree) {
  cbmap_construct(b, char *, int);
  char buf[64];
  snprintf(buf, sizeof(buf), "shared");
  cbmap_insert(b, buf, 5);
  REQUIRE_EQ(cbmap_get(b, "shared"), 5);
  REQUIRE_EQ(cbstmap_size(b), (size_t)1);
  cbmap_insert(b, "shared", 9);
  REQUIRE_EQ(cbstmap_size(b), (size_t)1);
  REQUIRE_EQ(cbmap_get(b, buf), 9);
  cbmap_destroy(b);
}

TEST(cbst_maps, basic_insertions_and_lookups) {
  cbstmap *cbmap = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  int val = -1;

  REQUIRE_EQ(get_int_from_int(cbmap, 100, &val), ccol_key_not_found);

  REQUIRE_EQ(cbstmap_size(cbmap), 0);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 20), ccol_success);
  REQUIRE_EQ(cbstmap_size(cbmap), 1);
  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 30), ccol_key_already_present);
  REQUIRE_EQ(cbstmap_size(cbmap), 1);

  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 40), ccol_success);
  REQUIRE_EQ(cbstmap_size(cbmap), 2);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 50), ccol_key_already_present);
  REQUIRE_EQ(cbstmap_size(cbmap), 2);

  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 30);
  REQUIRE_EQ(get_int_from_int(cbmap, 20, &val), ccol_success);
  REQUIRE_EQ(val, 50);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, basic_insertions_and_lookups_with_memmgmt_procs) {
  cbstmap *cbmap = cbstmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  int val = -1;

  REQUIRE_EQ(cbstmap_size(cbmap), 0);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 20), ccol_success);
  REQUIRE_EQ(cbstmap_size(cbmap), 1);
  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 30), ccol_key_already_present);
  REQUIRE_EQ(cbstmap_size(cbmap), 1);

  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 40), ccol_success);
  REQUIRE_EQ(cbstmap_size(cbmap), 2);

  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 30);
  REQUIRE_EQ(get_int_from_int(cbmap, 20, &val), ccol_success);
  REQUIRE_EQ(val, 40);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, insert_values_with_different_sizes) {
  cbstmap *cbmap = cbstmap_create(ccol_int, NULL);

  int key1 = 3;
  long val1 = 43;
  REQUIRE_EQ(
      cbstmap_insert_elem(
          cbmap, &(cmap_pair){.ptr = (void *)&key1, .size = sizeof(key1)},
          &(cmap_pair){.ptr = (void *)&val1, .size = sizeof(val1)}),
      ccol_success);

  int key2 = 4;
  char val2 = 44;
  REQUIRE_EQ(
      cbstmap_insert_elem(
          cbmap, &(cmap_pair){.ptr = (void *)&key2, .size = sizeof(key2)},
          &(cmap_pair){.ptr = (void *)&val2, .size = sizeof(val2)}),
      ccol_success);

  long val1_from_map = -1;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(
          cbmap, &(cmap_pair){.ptr = (void *)&key1, .size = sizeof(key1)},
          &val1_from_map, sizeof(val1_from_map)),
      ccol_success);
  REQUIRE_EQ(val1_from_map, val1);

  char val2_from_map = -1;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(
          cbmap, &(cmap_pair){.ptr = (void *)&key2, .size = sizeof(key2)},
          &val2_from_map, sizeof(val2_from_map)),
      ccol_success);
  REQUIRE_EQ(val2_from_map, val2);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, basic_deletions) {
  cbstmap *cbmap = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 100), ccol_success);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 200), ccol_success);
  REQUIRE_EQ(insert_int_to_int(cbmap, 30, 300), ccol_success);
  REQUIRE_EQ(cbstmap_size(cbmap), 3);

  REQUIRE_EQ(delete_int_from_int(cbmap, 20), ccol_success);
  REQUIRE_EQ(cbstmap_size(cbmap), 2);

  int val = -1;
  REQUIRE_EQ(get_int_from_int(cbmap, 20, &val), ccol_key_not_found);
  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 100);
  REQUIRE_EQ(get_int_from_int(cbmap, 30, &val), ccol_success);
  REQUIRE_EQ(val, 300);

  REQUIRE_EQ(delete_int_from_int(cbmap, 20), ccol_key_not_found);
  REQUIRE_EQ(delete_int_from_int(cbmap, 10), ccol_success);
  REQUIRE_EQ(delete_int_from_int(cbmap, 30), ccol_success);
  REQUIRE_EQ(cbstmap_size(cbmap), 0);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, accessing_references) {
  cbstmap *cbmap = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 100), ccol_success);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 200), ccol_success);

  int *val_ptr = NULL;
  REQUIRE_EQ(get_int_ref_from_int(cbmap, 10, &val_ptr), 0);
  REQUIRE_EQ(*val_ptr, 100);

  *val_ptr = 150;

  int val = -1;
  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 150);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, reset) {
  cbstmap *cbmap = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  for (int i = 0; i < 100; ++i) {
    REQUIRE_EQ(insert_int_to_int(cbmap, i, i * 10), ccol_success);
  }

  REQUIRE_EQ(cbstmap_size(cbmap), 100);

  cbstmap_reset(cbmap);

  REQUIRE_EQ(cbstmap_size(cbmap), 0);

  int val = -1;
  for (int i = 0; i < 100; ++i) {
    REQUIRE_EQ(get_int_from_int(cbmap, i, &val), ccol_key_not_found);
  }

  REQUIRE_EQ(insert_int_to_int(cbmap, 42, 420), ccol_success);
  REQUIRE_EQ(get_int_from_int(cbmap, 42, &val), ccol_success);
  REQUIRE_EQ(val, 420);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, in_order_iteration) {
  cbmap_construct(bmap, int, int);

  int keys[] = {50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45};
  int num_keys = sizeof(keys) / sizeof(keys[0]);

  for (int i = 0; i < num_keys; ++i) {
    int val = keys[i] * 10;
    cbmap_insert(bmap, keys[i], val);
  }

  REQUIRE_EQ(cbstmap_size(bmap), (size_t)num_keys);

  int prev_key = -1;
  int count = 0;
  ccol_iter_declare(bmap, it);
  for (it = ccol_begin(bmap); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    int val = *ccol_iter_val_ptr(it);

    REQUIRE_TRUE(key > prev_key);
    REQUIRE_EQ(val, key * 10);

    prev_key = key;
    ++count;
  }

  REQUIRE_EQ(count, num_keys);

  cbmap_destroy(bmap);
}

TEST(cbst_maps, avl_balancing_worst_case_insertions) {
  cbmap_construct(bmap, int, int);

  for (int i = 0; i < 100; ++i) {
    int val = i * 10;
    cbmap_insert(bmap, i, val);
  }

  REQUIRE_EQ(cbstmap_size(bmap), 100);

  for (int i = 0; i < 100; ++i) {
    int val = cbmap_get(bmap, i);
    REQUIRE_EQ(val, i * 10);
  }

  int expected_key = 0;
  ccol_iter_declare(bmap, it);
  for (it = ccol_begin(bmap); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    REQUIRE_EQ(key, expected_key);
    ++expected_key;
  }

  cbmap_destroy(bmap);
}

TEST(cbst_maps, avl_balancing_reverse_order) {
  cbmap_construct(bmap, int, int);

  for (int i = 99; i >= 0; --i) {
    int val = i * 10;
    cbmap_insert(bmap, i, val);
  }

  REQUIRE_EQ(cbstmap_size(bmap), 100);

  for (int i = 0; i < 100; ++i) {
    int val = cbmap_get(bmap, i);
    REQUIRE_EQ(val, i * 10);
  }

  int expected_key = 0;
  ccol_iter_declare(bmap, it);
  for (it = ccol_begin(bmap); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    REQUIRE_EQ(key, expected_key);
    ++expected_key;
  }

  cbmap_destroy(bmap);
}

TEST(cbst_maps, deletions_maintain_balance) {
  cbmap_construct(bmap, int, int);

  for (int i = 0; i < 50; ++i) {
    int val = i * 10;
    cbmap_insert(bmap, i, val);
  }

  for (int i = 0; i < 50; i += 2) {
    REQUIRE_EQ(cbmap_remove(bmap, i), ccol_success);
  }

  REQUIRE_EQ(cbstmap_size(bmap), 25);

  for (int i = 1; i < 50; i += 2) {
    int val = cbmap_get(bmap, i);
    REQUIRE_EQ(val, i * 10);
  }

  for (int i = 0; i < 50; i += 2) {
    REQUIRE_EQ(cbmap_remove(bmap, i), ccol_key_not_found);
  }

  int count = 0;
  ccol_iter_declare(bmap, it);
  for (it = ccol_begin(bmap); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    REQUIRE_TRUE(key % 2 == 1);
    ++count;
  }
  REQUIRE_EQ(count, 25);

  cbmap_destroy(bmap);
}

// Orders int keys ascending, for the tests of a custom comparator.
int custom_int_comparator(const void *a, const void *b) {
  int val_a = *(const int *)a;
  int val_b = *(const int *)b;
  return (val_a > val_b) - (val_a < val_b);
}

TEST(cbst_maps, declarative_macros) {
  {
    cbmap_declare(bm, int, int);
    cbmap_init(bm);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    *cbmap_get_ptr(bm, key) = 35;
    REQUIRE_EQ(cbmap_get(bm, key), 35);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }

  {
    ccol_memmgmt_procs mp = (ccol_memmgmt_procs){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    cbmap_declare(bm, int, int);
    cbmap_init_mp(bm, &mp);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }

  {
    ccol_comparison_proc comp = &custom_int_comparator;
    cbmap_declare(bm, int, int);
    cbmap_init_cc(bm, comp);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }

  {
    ccol_comparison_proc comp = &custom_int_comparator;
    ccol_memmgmt_procs mp = (ccol_memmgmt_procs){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    cbmap_declare(bm, int, int);
    cbmap_init_full(bm, &mp, comp);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }
}

TEST(cbst_maps, constructive_macros) {
  {
    cbmap_construct(bm, int, int);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }

  {
    ccol_memmgmt_procs mp = (ccol_memmgmt_procs){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    cbmap_construct_mp(bm, int, int, &mp);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }

  {
    ccol_comparison_proc comp = &custom_int_comparator;
    cbmap_construct_cc(bm, int, int, comp);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }

  {
    ccol_comparison_proc comp = &custom_int_comparator;
    ccol_memmgmt_procs mp = (ccol_memmgmt_procs){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    cbmap_construct_full(bm, int, int, &mp, comp);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }
}

TEST(cbst_maps, iteration_with_macros) {
  cbmap_construct(bm, int, char *);

  int key = 3;
  cbmap_insert(bm, key, "hello");

  key = 4;
  cbmap_insert(bm, key, "world");

  key = 5;
  cbmap_insert(bm, key, "test");

  int records[6] = {0};
  int counter = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    key = *ccol_iter_key_ptr(it);
    if (key == 3) {
      REQUIRE_EQ(records[3]++, 0);
      REQUIRE_STREQ(*ccol_iter_val_ptr(it), "hello");
    } else if (key == 4) {
      REQUIRE_EQ(records[4]++, 0);
      REQUIRE_STREQ(*ccol_iter_val_ptr(it), "world");
    } else if (key == 5) {
      REQUIRE_EQ(records[5]++, 0);
      REQUIRE_STREQ(*ccol_iter_val_ptr(it), "test");
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  cbmap_destroy(bm);
}

TEST(cbst_maps, map_int_to_struct) {
  typedef struct some_struct {
    int a;
    short b;
    char c;
  } some_struct;

  cbmap_construct(bm, int, some_struct);

  some_struct s1 = {.a = 10, .b = 20, .c = 'x'};
  some_struct s2 = {.a = 30, .b = 40, .c = 'y'};
  some_struct s3 = {.a = 50, .b = 60, .c = 'z'};

  int key = 1;
  cbmap_insert(bm, key, s1);
  key = 2;
  cbmap_insert(bm, key, s2);
  key = 3;
  cbmap_insert(bm, key, s3);

  key = 1;
  some_struct result = cbmap_get(bm, key);
  REQUIRE_EQ(result.a, 10);
  REQUIRE_EQ(result.b, 20);
  REQUIRE_EQ(result.c, 'x');

  key = 2;
  result = cbmap_get(bm, key);
  REQUIRE_EQ(result.a, 30);
  REQUIRE_EQ(result.b, 40);
  REQUIRE_EQ(result.c, 'y');

  key = 3;
  result = cbmap_get(bm, key);
  REQUIRE_EQ(result.a, 50);
  REQUIRE_EQ(result.b, 60);
  REQUIRE_EQ(result.c, 'z');

  cbmap_destroy(bm);
}

TEST(cbst_maps, map_int_to_struct_ptr) {
  typedef struct some_struct {
    int a;
    short b;
  } some_struct;

  cbmap_construct(bm, int, some_struct *);

  some_struct d1 = {.a = 3, .b = 4};
  some_struct d2 = {.a = 5, .b = 6};
  some_struct d3 = {.a = 7, .b = 8};

  int key = 1;
  some_struct *ptr = &d1;
  cbmap_insert(bm, key, ptr);
  key = 2;
  ptr = &d2;
  cbmap_insert(bm, key, ptr);
  key = 3;
  ptr = &d3;
  cbmap_insert(bm, key, ptr);

  int counter = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    if (key == 1) {
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->a, 3);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->b, 4);
    } else if (key == 2) {
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->a, 5);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->b, 6);
    } else if (key == 3) {
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->a, 7);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->b, 8);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  cbmap_destroy(bm);
}

TEST(cbst_maps, unsigned_keys) {
  cbmap_construct(bm, unsigned int, int);

  for (unsigned int i = 0; i < 100; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbstmap_size(bm), 100);

  for (unsigned int i = 0; i < 100; ++i) {
    int val = cbmap_get(bm, i);
    REQUIRE_EQ(val, (int)(i * 10));
  }

  cbmap_destroy(bm);
}

TEST(cbst_maps, negative_keys) {
  cbmap_construct(bm, int, int);

  for (int i = -50; i < 50; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbstmap_size(bm), 100);

  int expected = -50;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    REQUIRE_EQ(key, expected);
    ++expected;
  }
  REQUIRE_EQ(expected, 50);

  cbmap_destroy(bm);
}

TEST(cbst_maps, long_keys) {
  cbmap_construct(bm, long, long);

  long keys[] = {LONG_MAX / 8, LONG_MAX / 6, LONG_MAX / 4, LONG_MAX / 2};
  int num_keys = sizeof(keys) / sizeof(keys[0]);

  for (int i = 0; i < num_keys; ++i) {
    long val = keys[i] * 2;
    cbmap_insert(bm, keys[i], val);
  }

  for (int i = 0; i < num_keys; ++i) {
    long val = cbmap_get(bm, keys[i]);
    REQUIRE_EQ(val, keys[i] * 2);
  }

  cbmap_destroy(bm);
}

TEST(cbst_maps, duplicate_insert_updates_value) {
  cbmap_construct(bm, int, int);

  int key = 42;
  int val = 100;
  cbmap_insert(bm, key, val);
  REQUIRE_EQ(cbstmap_size(bm), 1);
  REQUIRE_EQ(cbmap_get(bm, key), 100);

  val = 200;
  cbmap_insert(bm, key, val);
  REQUIRE_EQ(cbstmap_size(bm), 1);
  REQUIRE_EQ(cbmap_get(bm, key), 200);

  cbmap_destroy(bm);
}

TEST(cbst_maps, empty_tree_iteration) {
  cbmap_construct(bm, int, int);

  int count = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    ++count;
  }

  REQUIRE_EQ(count, 0);

  cbmap_destroy(bm);
}

TEST(cbst_maps, single_element_tree) {
  cbmap_construct(bm, int, int);

  int key = 42;
  int val = 420;
  cbmap_insert(bm, key, val);

  int count = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    REQUIRE_EQ(*ccol_iter_key_ptr(it), 42);
    REQUIRE_EQ(*ccol_iter_val_ptr(it), 420);
    ++count;
  }

  REQUIRE_EQ(count, 1);

  cbmap_destroy(bm);
}

TEST(cbst_maps, stress_test_many_insertions) {
  cbmap_construct(bm, int, int);

  const int size = 1000000;

  for (int i = 0; i < size; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbstmap_size(bm), (size_t)size);

  for (int i = 0; i < size; ++i) {
    int val = cbmap_get(bm, i);
    REQUIRE_EQ(val, i * 10);
  }

  int expected = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    REQUIRE_EQ(key, expected);
    ++expected;
  }
  REQUIRE_EQ(expected, size);

  cbmap_destroy(bm);
}

TEST(cbst_maps, stress_test_many_deletions) {
  cbmap_construct(bm, int, int);

  const int size = 1000000;

  for (int i = 0; i < size; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbstmap_size(bm), (size_t)size);

  for (int i = 0; i < size; ++i) {
    REQUIRE_EQ(cbmap_remove(bm, i), ccol_success);
  }

  REQUIRE_EQ(cbstmap_size(bm), 0);

  for (int i = 0; i < size; ++i) {
    REQUIRE_EQ(cbmap_remove(bm, i), ccol_key_not_found);
  }

  cbmap_destroy(bm);
}

TEST(cbst_maps, alternating_insert_delete) {
  cbmap_construct(bm, int, int);

  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 100; ++i) {
      int val = i * round;
      cbmap_insert(bm, i, val);
    }

    REQUIRE_EQ(cbstmap_size(bm), 100);

    for (int i = 0; i < 50; ++i) {
      cbmap_remove(bm, i);
    }

    REQUIRE_EQ(cbstmap_size(bm), 50);

    for (int i = 50; i < 100; ++i) {
      cbmap_remove(bm, i);
    }

    REQUIRE_EQ(cbstmap_size(bm), 0);
  }

  cbmap_destroy(bm);
}

TEST(cbst_maps, re_enabled_local_cbm_macros) {
  cbmap_construct(bm, int, int);

  for (int i = 0; i < 10; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  {
    cbmap_redeclare(bm, int, int);

    for (int i = 0; i < 10; ++i) {
      int val = cbmap_get(bm, i);
      REQUIRE_EQ(val, i * 10);
    }
  }

  cbmap_destroy(bm);
}

TEST(cbst_maps, string_keys) {
  cbmap_construct(bm, char *, int);

  char *k_alpha = "alpha";
  char *k_beta = "beta";
  char *k_gamma = "gamma";
  char *k_missing = "zeta";

  int v1 = 100, v2 = 200, v3 = 300;
  cbmap_insert(bm, k_alpha, v1);
  cbmap_insert(bm, k_beta, v2);
  cbmap_insert(bm, k_gamma, v3);

  REQUIRE_EQ(cbmap_get(bm, k_alpha), v1);
  REQUIRE_EQ(cbmap_get(bm, k_beta), v2);
  REQUIRE_EQ(cbmap_get(bm, k_gamma), v3);

  int v_updated = 999;
  cbmap_insert(bm, k_alpha, v_updated);
  REQUIRE_EQ(cbmap_get(bm, k_alpha), v_updated);

  REQUIRE_EQ(cbmap_remove(bm, k_beta), ccol_success);
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, k_beta), NULL);
  REQUIRE_EQ(cbmap_remove(bm, k_beta), ccol_key_not_found);

  REQUIRE_EQ((void *)cbmap_get_ptr(bm, k_missing), NULL);

  cbmap_destroy(bm);
}

// Walks a char * -> int map in order and stores up to max_out keys in out, so
// the iteration-order tests can compare the walk with an expected array. Gives
// the number of keys walked, which can exceed max_out.
static int collect_string_keys(cbmap bm, const char *out[], int max_out) {
  cbmap_redeclare(bm, char *, int);
  int n = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    if (n < max_out) {
      out[n] = *ccol_iter_key_ptr(it);
    }
    ++n;
  }
  return n;
}

TEST(cbst_maps, string_keys_3_char_iteration_order) {
  cbmap_construct(bm, char *, int);

  char *keys[] = {"bca", "abc", "cab", "acb", "bac"};
  for (int i = 0; i < 5; ++i) {
    int val = i + 1;
    cbmap_insert(bm, keys[i], val);
  }

  REQUIRE_EQ(cbstmap_size(bm), 5);

  const char *expected[] = {"abc", "acb", "bac", "bca", "cab"};
  const char *got[5];
  int n = collect_string_keys(bm, got, 5);

  REQUIRE_EQ(n, 5);
  for (int i = 0; i < 5; ++i) {
    REQUIRE_STREQ(got[i], expected[i]);
  }

  REQUIRE_NE((void *)cbmap_get_ptr(bm, "abc"), NULL);
  REQUIRE_NE((void *)cbmap_get_ptr(bm, "bca"), NULL);
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, "xyz"), NULL);

  cbmap_destroy(bm);
}

TEST(cbst_maps, string_keys_7_char_iteration_order) {
  cbmap_construct(bm, char *, int);

  char *keys[] = {"bacdefg", "abcdefg", "gfedcba", "abcdegh", "abcdefh"};
  for (int i = 0; i < 5; ++i) {
    int val = i + 1;
    cbmap_insert(bm, keys[i], val);
  }

  REQUIRE_EQ(cbstmap_size(bm), 5);

  const char *expected[] = {"abcdefg", "abcdefh", "abcdegh", "bacdefg",
                            "gfedcba"};
  const char *got[5];
  int n = collect_string_keys(bm, got, 5);

  REQUIRE_EQ(n, 5);
  for (int i = 0; i < 5; ++i) {
    REQUIRE_STREQ(got[i], expected[i]);
  }

  REQUIRE_NE((void *)cbmap_get_ptr(bm, "abcdefg"), NULL);
  REQUIRE_NE((void *)cbmap_get_ptr(bm, "gfedcba"), NULL);
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, "zzzzzzz"), NULL);

  cbmap_destroy(bm);
}

TEST(cbst_maps, char_type_variants_as_string_keys) {
  {
    cbmap_construct(bm, signed char *, int);

    signed char *k_banana = (signed char *)"banana";
    signed char *k_apple = (signed char *)"apple";
    signed char *k_cherry = (signed char *)"cherry";
    int v1 = 1, v2 = 2, v3 = 3;

    cbmap_insert(bm, k_banana, v1);
    cbmap_insert(bm, k_apple, v2);
    cbmap_insert(bm, k_cherry, v3);

    REQUIRE_EQ(cbstmap_size(bm), 3);
    REQUIRE_EQ(cbmap_get(bm, k_banana), 1);
    REQUIRE_EQ(cbmap_get(bm, k_apple), 2);
    REQUIRE_EQ(cbmap_get(bm, k_cherry), 3);

    const char *expected_sc[] = {"apple", "banana", "cherry"};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_STREQ((const char *)*ccol_iter_key_ptr(it), expected_sc[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 3);

    char *ck = "banana";
    REQUIRE_EQ(cbmap_get(bm, ck), 1);

    cbmap_destroy(bm);
  }

  {
    cbmap_construct(bm, unsigned char *, int);

    unsigned char *k_mango = (unsigned char *)"mango";
    unsigned char *k_kiwi = (unsigned char *)"kiwi";
    unsigned char *k_lime = (unsigned char *)"lime";
    int v10 = 10, v20 = 20, v30 = 30;

    cbmap_insert(bm, k_mango, v10);
    cbmap_insert(bm, k_kiwi, v20);
    cbmap_insert(bm, k_lime, v30);

    REQUIRE_EQ(cbstmap_size(bm), 3);
    REQUIRE_EQ(cbmap_get(bm, k_mango), 10);
    REQUIRE_EQ(cbmap_get(bm, k_kiwi), 20);
    REQUIRE_EQ(cbmap_get(bm, k_lime), 30);

    const char *expected_uc[] = {"kiwi", "lime", "mango"};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_STREQ((const char *)*ccol_iter_key_ptr(it), expected_uc[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 3);

    signed char *sk = (signed char *)"kiwi";
    REQUIRE_EQ(cbmap_get(bm, sk), 20);

    cbmap_destroy(bm);
  }

  {
    cbmap_construct(bm, char *, int);

    char *k1 = "one";
    char *k2 = "two";
    char *k3 = "three";
    int v100 = 100, v200 = 200, v300 = 300;
    cbmap_insert(bm, k1, v100);
    cbmap_insert(bm, k2, v200);
    cbmap_insert(bm, k3, v300);

    signed char *sk2 = (signed char *)"two";
    unsigned char *uk1 = (unsigned char *)"one";
    REQUIRE_EQ(cbmap_get(bm, sk2), 200);
    REQUIRE_EQ(cbmap_get(bm, uk1), 100);

    cbmap_destroy(bm);
  }
}

TEST(cbst_maps, float_double_long_double_keys_sort_numerically) {
  {
    cbmap_construct(bm, float, int);

    float keys[] = {3.0f, -5.0f, 100.0f, -100.0f, 0.0f};
    for (int i = 0; i < 5; ++i) {
      int val = i + 1;
      cbmap_insert(bm, keys[i], val);
    }

    float expected[] = {-100.0f, -5.0f, 0.0f, 3.0f, 100.0f};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_EQ(*ccol_iter_key_ptr(it), expected[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 5);

    cbmap_destroy(bm);
  }

  {
    cbmap_construct(bm, double, int);

    double keys[] = {3.0, -5.0, 100.0, -100.0, 0.0};
    for (int i = 0; i < 5; ++i) {
      int val = i + 1;
      cbmap_insert(bm, keys[i], val);
    }

    double expected[] = {-100.0, -5.0, 0.0, 3.0, 100.0};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_EQ(*ccol_iter_key_ptr(it), expected[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 5);

    double neg_zero = -0.0;
    double pos_zero = 0.0;
    REQUIRE_EQ(cbmap_get(bm, neg_zero), cbmap_get(bm, pos_zero));

    cbmap_destroy(bm);
  }

  {
    cbmap_construct(bm, long double, int);

    long double keys[] = {3.0L, -5.0L, 100.0L, -100.0L, 0.0L};
    for (int i = 0; i < 5; ++i) {
      int val = i + 1;
      cbmap_insert(bm, keys[i], val);
    }

    long double expected[] = {-100.0L, -5.0L, 0.0L, 3.0L, 100.0L};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_EQ(*ccol_iter_key_ptr(it), expected[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 5);

    cbmap_destroy(bm);
  }
}

TEST(cbst_maps, nan_key_does_not_corrupt_other_entries) {
  cbmap_construct(bm, double, int);

  double k1 = 5.0, k2 = 9.0;
  int v1 = 111, v2 = 222;
  cbmap_insert(bm, k1, v1);
  cbmap_insert(bm, k2, v2);
  REQUIRE_EQ(cbstmap_size(bm), (size_t)2);

  double nan_key = NAN;
  int nan_val = 999;
  ccol_retval r = cbstmap_insert_elem(
      bm, &(cmap_pair){.ptr = &nan_key, .size = sizeof(nan_key)},
      &(cmap_pair){.ptr = &nan_val, .size = sizeof(nan_val)});
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cbstmap_size(bm), (size_t)3);

  REQUIRE_EQ(cbmap_get(bm, k1), 111);
  REQUIRE_EQ(cbmap_get(bm, k2), 222);

  int readback = 0;
  REQUIRE_EQ(cbstmap_get_elem_copy(
                 bm, &(cmap_pair){.ptr = &nan_key, .size = sizeof(nan_key)},
                 &readback, sizeof(readback)),
             ccol_success);
  REQUIRE_EQ(readback, 999);

  double other_nan = -NAN;
  int other_nan_val = 1000;
  r = cbstmap_insert_elem(
      bm, &(cmap_pair){.ptr = &other_nan, .size = sizeof(other_nan)},
      &(cmap_pair){.ptr = &other_nan_val, .size = sizeof(other_nan_val)});
  REQUIRE_EQ(r, ccol_key_already_present);
  REQUIRE_EQ(cbstmap_size(bm), (size_t)3);

  REQUIRE_EQ(cbstmap_remove_elem(
                 bm, &(cmap_pair){.ptr = &nan_key, .size = sizeof(nan_key)}),
             ccol_success);
  REQUIRE_EQ(cbstmap_size(bm), (size_t)2);
  REQUIRE_EQ(cbmap_get(bm, k1), 111);
  REQUIRE_EQ(cbmap_get(bm, k2), 222);

  cbmap_destroy(bm);
}

TEST(cbst_maps, char_keys_use_native_char_comparison) {
  cbmap_construct(bm, char, int);

  unsigned char raw[] = {200, 5, 128, 1, 255, 0, 127, 100};
  const int n = (int)(sizeof(raw) / sizeof(raw[0]));
  char keys[sizeof(raw) / sizeof(raw[0])];
  for (int i = 0; i < n; ++i) {
    keys[i] = (char)raw[i];
  }

  for (int i = 0; i < n; ++i) {
    int val = i;
    cbmap_insert(bm, keys[i], val);
  }
  REQUIRE_EQ(cbstmap_size(bm), (size_t)n);

  char expected[sizeof(raw) / sizeof(raw[0])];
  memcpy(expected, keys, sizeof(keys));
  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (expected[j] < expected[i]) {
        char tmp = expected[i];
        expected[i] = expected[j];
        expected[j] = tmp;
      }
    }
  }

  int idx = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    REQUIRE_EQ(*ccol_iter_key_ptr(it), expected[idx]);
    ++idx;
  }
  REQUIRE_EQ(idx, n);

  cbmap_destroy(bm);
}

TEST(cbst_maps, scalar_signed_char_keys_sort_by_genuine_signed_value) {
  cbmap_construct(bm, signed char, int);

  signed char keys[] = {(signed char)-128, (signed char)-5,  0,
                        (signed char)3,    (signed char)100, (signed char)127};
  const int n = (int)(sizeof(keys) / sizeof(keys[0]));

  int insert_order[] = {3, 0, 5, 1, 4, 2};
  for (int i = 0; i < n; ++i) {
    int idx = insert_order[i];
    int val = idx;
    cbmap_insert(bm, keys[idx], val);
  }
  REQUIRE_EQ(cbstmap_size(bm), (size_t)n);

  int idx = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    REQUIRE_EQ((int)*ccol_iter_key_ptr(it), (int)keys[idx]);
    REQUIRE_EQ(*ccol_iter_val_ptr(it), idx);
    ++idx;
  }
  REQUIRE_EQ(idx, n);

  REQUIRE_EQ(cbmap_get(bm, keys[0]), 0);
  REQUIRE_EQ(cbmap_get(bm, keys[1]), 1);
  REQUIRE_EQ(cbmap_get(bm, keys[5]), 5);

  cbmap_destroy(bm);
}

TEST(cbst_maps, int8_t_keys_sort_by_genuine_signed_value) {
  cbmap_construct(bm, int8_t, int);

  int8_t keys[] = {-100, -1, 0, 1, 100};
  const int n = (int)(sizeof(keys) / sizeof(keys[0]));

  for (int i = n - 1; i >= 0; --i) {
    int val = i;
    cbmap_insert(bm, keys[i], val);
  }
  REQUIRE_EQ(cbstmap_size(bm), (size_t)n);

  int idx = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    REQUIRE_EQ((int)*ccol_iter_key_ptr(it), (int)keys[idx]);
    ++idx;
  }
  REQUIRE_EQ(idx, n);

  cbmap_destroy(bm);
}

TEST(cbst_maps, signed_char_keys_4_and_8_byte_strings_iteration_order) {
  {
    cbmap_construct(bm, signed char *, int);

    signed char *keys[] = {(signed char *)"bca", (signed char *)"abc",
                           (signed char *)"cab", (signed char *)"acb",
                           (signed char *)"bac"};
    for (int i = 0; i < 5; ++i) {
      int val = i + 1;
      cbmap_insert(bm, keys[i], val);
    }

    const char *expected[] = {"abc", "acb", "bac", "bca", "cab"};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_STREQ((const char *)*ccol_iter_key_ptr(it), expected[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 5);

    cbmap_destroy(bm);
  }

  {
    cbmap_construct(bm, signed char *, int);

    signed char *keys[] = {(signed char *)"bacdefg", (signed char *)"abcdefg",
                           (signed char *)"gfedcba", (signed char *)"abcdegh",
                           (signed char *)"abcdefh"};
    for (int i = 0; i < 5; ++i) {
      int val = i + 1;
      cbmap_insert(bm, keys[i], val);
    }

    const char *expected[] = {"abcdefg", "abcdefh", "abcdegh", "bacdefg",
                              "gfedcba"};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_STREQ((const char *)*ccol_iter_key_ptr(it), expected[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 5);

    cbmap_destroy(bm);
  }
}

TEST(cbst_maps, struct_key_default_comparator_uses_memcmp_regardless_of_size) {
  typedef struct {
    short a;
    short b;
  } pair_t;

  cbmap_construct(bm, pair_t, int);

  pair_t keys[] = {{1, 0}, {0, 1}, {2, 0}, {0, 2}};
  for (int i = 0; i < 4; ++i) {
    int val = i + 1;
    cbmap_insert(bm, keys[i], val);
  }

  REQUIRE_EQ(cbstmap_size(bm), 4);

  pair_t expected[4];
  memcpy(expected, keys, sizeof(keys));
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) {
      if (memcmp(&expected[j], &expected[i], sizeof(pair_t)) < 0) {
        pair_t tmp = expected[i];
        expected[i] = expected[j];
        expected[j] = tmp;
      }
    }
  }

  int idx = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    pair_t k = *ccol_iter_key_ptr(it);
    REQUIRE_EQ(k.a, expected[idx].a);
    REQUIRE_EQ(k.b, expected[idx].b);
    ++idx;
  }
  REQUIRE_EQ(idx, 4);

  cbmap_destroy(bm);
}

TEST(cbst_maps, construct_scoped_lifecycle) {
  {
    cbmap_construct_scoped(bm, int, int);
    REQUIRE_NE((void *)bm, NULL);

    for (int i = 0; i < 5; ++i) {
      int val = i * 10;
      cbmap_insert(bm, i, val);
    }
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cbmap_get(bm, i), i * 10);
    }
  }
}

TEST(cbst_maps, declare_scoped_lifecycle) {
  {
    cbmap_declare_scoped(bm, int, int);
    cbmap_init(bm);
    REQUIRE_NE((void *)bm, NULL);

    int k1 = 7, v1 = 70;
    int k2 = 3, v2 = 30;
    int k3 = 9, v3 = 90;
    cbmap_insert(bm, k1, v1);
    cbmap_insert(bm, k2, v2);
    cbmap_insert(bm, k3, v3);

    REQUIRE_EQ(cbmap_get(bm, k1), 70);
    REQUIRE_EQ(cbmap_get(bm, k2), 30);
    REQUIRE_EQ(cbmap_get(bm, k3), 90);
  }
}

TEST(cbst_maps, get_elem_copy_size_mismatch) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 42;
  int val = 100;
  REQUIRE_EQ(
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  double wrong_buf = 0;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                            &wrong_buf, sizeof(wrong_buf)),
      ccol_invalid_args);

  int right_buf = 0;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                            &right_buf, sizeof(right_buf)),
      ccol_success);
  REQUIRE_EQ(right_buf, 100);

  cbmap_destroy(cbm);
}

// A NULL buffer of size 0 is fine for a stored value of size 0. Without the
// check, the copy writes through NULL and the process dies with SIGSEGV.
TEST(cbst_maps, get_elem_copy_refuses_a_null_buffer) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);
  int key = 7, val = 70;
  ccol_retval ins =
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)});
  ccol_retval got = cbstmap_get_elem_copy(
      cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, NULL, sizeof(int));
  cbmap_destroy(cbm);
  REQUIRE_EQ(ins, ccol_success);
  REQUIRE_EQ(got, ccol_invalid_args);
}

TEST(cbst_maps, get_elem_ref_missing_key) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 1;
  int val = 10;
  REQUIRE_EQ(
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  int missing = 999;
  cmap_pair out = {NULL, 0};
  REQUIRE_EQ(
      cbstmap_get_elem_ref(
          cbm, &(cmap_pair){.ptr = &missing, .size = sizeof(missing)}, &out),
      ccol_key_not_found);
  REQUIRE_EQ(out.ptr, NULL);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, get_ptr_missing_int_key) {
  cbmap_construct(bm, int, int);

  int k1 = 1, v1 = 10;
  int k2 = 2, v2 = 20;
  int k_missing = 99;
  cbmap_insert(bm, k1, v1);
  cbmap_insert(bm, k2, v2);

  REQUIRE_NE((void *)cbmap_get_ptr(bm, k1), NULL);
  REQUIRE_EQ(*cbmap_get_ptr(bm, k1), 10);

  REQUIRE_EQ((void *)cbmap_get_ptr(bm, k_missing), NULL);

  cbmap_destroy(bm);
}

TEST(cbst_maps, early_iterator_destroy) {
  cbmap_construct(bm, int, int);

  for (int i = 0; i < 20; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  int count = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    ++count;
    if (count == 5) {
      ccol_iter_destroy(it);
      break;
    }
  }
  REQUIRE_EQ(count, 5);

  REQUIRE_EQ(cbstmap_size(bm), 20);
  for (int i = 0; i < 20; ++i) {
    int val = cbmap_get(bm, i);
    REQUIRE_EQ(val, i * 10);
  }

  count = 0;
  ccol_iter_declare(bm, it2);
  for (it2 = ccol_begin(bm); it2 != NULL; it2 = ccol_iter_next(it2)) {
    ++count;
  }
  REQUIRE_EQ(count, 20);

  cbmap_destroy(bm);
}

TEST(cbst_maps, update_value_with_different_size) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 7;
  int small_val = 42;
  REQUIRE_EQ(cbstmap_insert_elem(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                 &(cmap_pair){.ptr = &small_val, .size = sizeof(small_val)}),
             ccol_success);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  int readback = 0;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                            &readback, sizeof(readback)),
      ccol_success);
  REQUIRE_EQ(readback, 42);

  int64_t big_val = INT64_C(1234567890123);
  REQUIRE_EQ(cbstmap_insert_elem(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                 &(cmap_pair){.ptr = &big_val, .size = sizeof(big_val)}),
             ccol_key_already_present);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  int64_t big_readback = 0;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                            &big_readback, sizeof(big_readback)),
      ccol_success);
  const int64_t expected_big_val = INT64_C(1234567890123);
  REQUIRE_EQ(big_readback, expected_big_val);

  small_val = 99;
  REQUIRE_EQ(cbstmap_insert_elem(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                 &(cmap_pair){.ptr = &small_val, .size = sizeof(small_val)}),
             ccol_key_already_present);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  readback = 0;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                            &readback, sizeof(readback)),
      ccol_success);
  REQUIRE_EQ(readback, 99);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, update_value_to_zero_size_does_not_corrupt) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 7;
  int val = 42;
  REQUIRE_EQ(
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  char dummy = 0;
  ccol_retval r =
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &(cmap_pair){.ptr = &dummy, .size = 0});
  REQUIRE_EQ(r, ccol_key_already_present);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  cmap_pair vp = {(void *)0x1, 12345};
  REQUIRE_EQ(cbstmap_get_elem_ref(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, &vp),
             ccol_success);
  REQUIRE_EQ(vp.size, (size_t)0);
  REQUIRE_EQ((void *)vp.ptr, NULL);

  REQUIRE_EQ(cbstmap_get_elem_copy(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, NULL, 0),
             ccol_success);

  int restored = 777;
  r = cbstmap_insert_elem(
      cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
      &(cmap_pair){.ptr = &restored, .size = sizeof(restored)});
  REQUIRE_EQ(r, ccol_key_already_present);

  int readback = 0;
  REQUIRE_EQ(
      cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                            &readback, sizeof(readback)),
      ccol_success);
  REQUIRE_EQ(readback, 777);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, insert_and_retrieve_zero_size_value_for_new_key) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key1 = 1;
  int key2 = 2;
  char dummy = 0;

  REQUIRE_EQ(
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &key1, .size = sizeof(key1)},
                          &(cmap_pair){.ptr = &dummy, .size = 0}),
      ccol_success);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  int val2 = 55;
  REQUIRE_EQ(
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &key2, .size = sizeof(key2)},
                          &(cmap_pair){.ptr = &val2, .size = sizeof(val2)}),
      ccol_success);
  REQUIRE_EQ(cbstmap_size(cbm), 2);

  cmap_pair vp1 = {(void *)0x1, 12345};
  REQUIRE_EQ(cbstmap_get_elem_ref(
                 cbm, &(cmap_pair){.ptr = &key1, .size = sizeof(key1)}, &vp1),
             ccol_success);
  REQUIRE_EQ(vp1.size, (size_t)0);
  REQUIRE_EQ((void *)vp1.ptr, NULL);

  int readback2 = 0;
  REQUIRE_EQ(cbstmap_get_elem_copy(
                 cbm, &(cmap_pair){.ptr = &key2, .size = sizeof(key2)},
                 &readback2, sizeof(readback2)),
             ccol_success);
  REQUIRE_EQ(readback2, 55);

  REQUIRE_EQ(cbstmap_remove_elem(
                 cbm, &(cmap_pair){.ptr = &key1, .size = sizeof(key1)}),
             ccol_success);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, wrong_size_key_rejected_for_fixed_width_key_type) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  unsigned char too_wide[sizeof(int) + 1] = {0};
  int val = 42;

  const ccol_retval insert_r = cbstmap_insert_elem(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)},
      &(cmap_pair){.ptr = &val, .size = sizeof(val)});
  const size_t count_after_reject = cbstmap_size(cbm);

  int readback = -1;
  const ccol_retval copy_r = cbstmap_get_elem_copy(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)}, &readback,
      sizeof(readback));

  cmap_pair val_pair = {NULL, 0};
  const ccol_retval ref_r = cbstmap_get_elem_ref(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)}, &val_pair);

  const ccol_retval delete_r = cbstmap_remove_elem(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)});

  int good_key = 5;
  const ccol_retval good_insert_r = cbstmap_insert_elem(
      cbm, &(cmap_pair){.ptr = &good_key, .size = sizeof(good_key)},
      &(cmap_pair){.ptr = &val, .size = sizeof(val)});
  const size_t count_after_good = cbstmap_size(cbm);
  const ccol_retval good_copy_r = cbstmap_get_elem_copy(
      cbm, &(cmap_pair){.ptr = &good_key, .size = sizeof(good_key)}, &readback,
      sizeof(readback));

  cbmap_destroy(cbm);

  REQUIRE_EQ(insert_r, ccol_invalid_args);
  REQUIRE_EQ(count_after_reject, (size_t)0);
  REQUIRE_EQ(copy_r, ccol_invalid_args);
  REQUIRE_EQ(ref_r, ccol_invalid_args);
  REQUIRE_EQ(delete_r, ccol_invalid_args);
  REQUIRE_EQ(good_insert_r, ccol_success);
  REQUIRE_EQ(count_after_good, (size_t)1);
  REQUIRE_EQ(good_copy_r, ccol_success);
  REQUIRE_EQ(readback, 42);
}

TEST(cbst_maps,
     wrong_size_key_rejected_matches_chashmap_for_every_fixed_width_type) {
  const ccol_data_type fixed_width_types[] = {ccol_char,
                                              ccol_signed_char,
                                              ccol_unsigned_char,
                                              ccol_short,
                                              ccol_unsigned_short,
                                              ccol_int,
                                              ccol_unsigned_int,
                                              ccol_long,
                                              ccol_unsigned_long,
                                              ccol_long_long,
                                              ccol_unsigned_long_long,
                                              ccol_float,
                                              ccol_double,
                                              ccol_long_double,
                                              ccol_pointer};

  for (size_t i = 0;
       i < sizeof(fixed_width_types) / sizeof(fixed_width_types[0]); i++) {
    const ccol_data_type type = fixed_width_types[i];
    const size_t width = ccol_fixed_width_data_type_size(type);
    REQUIRE_NE(width, (size_t)0);

    cbmap cbm = cbstmap_create(type, NULL);
    REQUIRE_NE((void *)cbm, NULL);

    unsigned char key_buf[sizeof(long double) + sizeof(uintptr_t)] = {0};
    int val = 7;

    const ccol_retval short_r = cbstmap_insert_elem(
        cbm, &(cmap_pair){.ptr = key_buf, .size = width - 1},
        &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const size_t count_after_short = cbstmap_size(cbm);

    const ccol_retval exact_r =
        cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = key_buf, .size = width},
                            &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const size_t count_after_exact = cbstmap_size(cbm);

    cbmap_destroy(cbm);

    REQUIRE_EQ(short_r, ccol_invalid_args);
    REQUIRE_EQ(count_after_short, (size_t)0);
    REQUIRE_EQ(exact_r, ccol_success);
    REQUIRE_EQ(count_after_exact, (size_t)1);
  }
}

TEST(cbst_maps, variable_width_key_types_still_accept_any_key_size) {
  const ccol_data_type variable_width_types[] = {ccol_string, ccol_other_types};

  for (size_t i = 0;
       i < sizeof(variable_width_types) / sizeof(variable_width_types[0]);
       i++) {
    REQUIRE_EQ(ccol_fixed_width_data_type_size(variable_width_types[i]),
               (size_t)0);

    cbmap cbm = cbstmap_create(variable_width_types[i], NULL);
    REQUIRE_NE((void *)cbm, NULL);

    int val = 1;
    const ccol_retval short_r =
        cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = (void *)"ab", .size = 3},
                            &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const ccol_retval long_r = cbstmap_insert_elem(
        cbm, &(cmap_pair){.ptr = (void *)"abcdefghij", .size = 11},
        &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const size_t count = cbstmap_size(cbm);

    cbmap_destroy(cbm);

    REQUIRE_EQ(short_r, ccol_success);
    REQUIRE_EQ(long_r, ccol_success);
    REQUIRE_EQ(count, (size_t)2);
  }
}

TEST(cbst_maps, zero_size_key_roundtrips) {
  cbmap cbm = cbstmap_create(ccol_other_types, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  char dummy = 0;
  int val1 = 111;
  REQUIRE_EQ(
      cbstmap_insert_elem(cbm, &(cmap_pair){.ptr = &dummy, .size = 0},
                          &(cmap_pair){.ptr = &val1, .size = sizeof(val1)}),
      ccol_success);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  int readback = 0;
  REQUIRE_EQ(cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &dummy, .size = 0},
                                   &readback, sizeof(readback)),
             ccol_success);
  REQUIRE_EQ(readback, 111);

  int other_key = 5;
  int val2 = 222;
  REQUIRE_EQ(
      cbstmap_insert_elem(
          cbm, &(cmap_pair){.ptr = &other_key, .size = sizeof(other_key)},
          &(cmap_pair){.ptr = &val2, .size = sizeof(val2)}),
      ccol_success);
  REQUIRE_EQ(cbstmap_size(cbm), 2);

  REQUIRE_EQ(cbstmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &dummy, .size = 0},
                                   &readback, sizeof(readback)),
             ccol_success);
  REQUIRE_EQ(readback, 111);

  REQUIRE_EQ(cbstmap_remove_elem(cbm, &(cmap_pair){.ptr = &dummy, .size = 0}),
             ccol_success);
  REQUIRE_EQ(cbstmap_size(cbm), 1);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, zero_size_string_key_is_refused_by_every_raw_entry_point) {
  cbmap cbm = cbstmap_create(ccol_string, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  char dummy = 0;
  int val = 7;
  const cmap_pair val_pair = {.ptr = &val, .size = sizeof(val)};
  const cmap_pair null_key = {.ptr = NULL, .size = 0};
  const cmap_pair empty_key = {.ptr = &dummy, .size = 0};

  const ccol_retval insert_null =
      cbstmap_insert_elem(cbm, &null_key, &val_pair);
  const ccol_retval insert_empty =
      cbstmap_insert_elem(cbm, &empty_key, &val_pair);
  const size_t count_after_inserts = cbstmap_size(cbm);

  const ccol_retval insert_real = cbstmap_insert_elem(
      cbm, &(cmap_pair){.ptr = (void *)"", .size = 1}, &val_pair);

  int readback = 0;
  const ccol_retval copy_r =
      cbstmap_get_elem_copy(cbm, &null_key, &readback, sizeof(readback));
  cmap_pair ref = {NULL, 0};
  const ccol_retval ref_r = cbstmap_get_elem_ref(cbm, &empty_key, &ref);
  const ccol_retval delete_r = cbstmap_remove_elem(cbm, &null_key);

  bool saw_null_key = false;
  size_t iterated = 0;
  cbmap_redeclare(cbm, char *, int);
  ccol_iter_declare(cbm, it);
  for (it = cbstmap_begin_iter(cbm, NULL); it; it = ccol_iter_next(it)) {
    iterated++;
    if (!it->key_pair->ptr) saw_null_key = true;
  }
  const size_t final_count = cbstmap_size(cbm);
  cbmap_destroy(cbm);

  REQUIRE_EQ(insert_null, ccol_invalid_args);
  REQUIRE_EQ(insert_empty, ccol_invalid_args);
  REQUIRE_EQ(count_after_inserts, (size_t)0);
  REQUIRE_EQ(insert_real, ccol_success);
  REQUIRE_EQ(copy_r, ccol_invalid_args);
  REQUIRE_EQ(ref_r, ccol_invalid_args);
  REQUIRE_EQ(ref.ptr, NULL);
  REQUIRE_EQ(delete_r, ccol_invalid_args);
  REQUIRE_EQ(iterated, (size_t)1);
  REQUIRE_FALSE(saw_null_key);
  REQUIRE_EQ(final_count, (size_t)1);
}

// begin_iter is the one accessor that accepts a NULL map, as
// chashmap_begin_iter() does, so a lazily created map field can be iterated.
TEST(cbst_maps, begin_iter_null_map_returns_null_like_empty) {
  cbmap null_map = NULL;
  char *err = (char *)0x1;
  REQUIRE_EQ((void *)cbstmap_begin_iter(null_map, &err), NULL);
  REQUIRE_EQ((void *)err, NULL);

  REQUIRE_EQ((void *)cbstmap_begin_iter(null_map, NULL), NULL);
}

// The key range of the randomized invariant test, small enough that inserts and
// removes keep hitting existing keys.
#define INVARIANTS_KEY_RANGE 200

// Random inserts and removes, checking after every operation the AVL heights
// and balance, a strictly increasing in-order walk, and a shadow array that
// every lookup must agree with.
TEST(cbst_maps, invariants_random_ops) {
  ccol_invariants_rng_t rng;
  uint64_t seed = CCOL_INVARIANTS_DEFAULT_SEED;
  ccol_invariants_seed(&rng, seed);
  ccol_invariants_print_seed("cbst_maps.invariants_random_ops", seed);

  cbmap_construct(hm, int, int);

  int shadow_value[INVARIANTS_KEY_RANGE];
  bool shadow_present[INVARIANTS_KEY_RANGE] = {0};

  const int num_ops = 2000;
  for (int i = 0; i < num_ops; ++i) {
    int key = (int)ccol_invariants_next_bounded(&rng, INVARIANTS_KEY_RANGE);
    bool do_insert = ccol_invariants_next_bounded(&rng, 2) == 0;

    if (do_insert) {
      int val = (int)ccol_invariants_next_bounded(&rng, 1000000);
      cbmap_insert(hm, key, val);
      shadow_value[key] = val;
      shadow_present[key] = true;
    } else {
      cbmap_remove(hm, key);
      shadow_present[key] = false;
    }

    REQUIRE_TRUE(cbmap_debug_validate_avl(hm));

    size_t shadow_count = 0;
    int prev_key = -1;
    bool have_prev = false;
    ccol_iter_declare(hm, it);
    for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
      int k = *ccol_iter_key_ptr(it);
      if (have_prev) {
        REQUIRE_TRUE(k > prev_key);
      }
      prev_key = k;
      have_prev = true;
      REQUIRE_TRUE(shadow_present[k]);
      REQUIRE_EQ(*ccol_iter_val_ptr(it), shadow_value[k]);
      ++shadow_count;
    }
    REQUIRE_EQ(cbstmap_size(hm), shadow_count);

    for (int k = 0; k < INVARIANTS_KEY_RANGE; ++k) {
      if (!shadow_present[k]) {
        REQUIRE_EQ((void *)cbmap_get_ptr(hm, k), (void *)NULL);
      }
    }
  }

  cbmap_destroy(hm);
}

// The value passed back is the pointer cbstmap_get_elem_ref() gave out. If the
// resize copied it after freeing the old storage, AddressSanitizer would report
// a heap-use-after-free.
TEST(cbstmap_value_storage, resizing_a_value_from_its_own_buffer_is_safe) {
  cbmap_construct_scoped(bm, int, char *);
  int key = 3;

  cbmap_insert(bm, key, "first value, short enough to sit inside the node");
  cbmap_insert(bm, key, "second value, a different length, now external");

  cmap_pair kp = {&key, sizeof(key)};
  cmap_pair held = {NULL, 0};
  ccol_retval got = cbstmap_get_elem_ref(bm, &kp, &held);

  bool shrank = false;
  if (got == ccol_success && held.ptr) {
    cmap_pair selfref = {held.ptr, 14};
    shrank =
        (cbstmap_insert_elem(bm, &kp, &selfref) == ccol_key_already_present);
  }

  cmap_pair after = {NULL, 0};
  ccol_retval reread = cbstmap_get_elem_ref(bm, &kp, &after);
  bool intact = (reread == ccol_success && after.ptr && after.size == 14 &&
                 memcmp(after.ptr, "second value,", 13) == 0);

  REQUIRE_EQ(got, ccol_success);
  REQUIRE_TRUE(shrank);
  REQUIRE_TRUE(intact);
}

// Checks the string stored under key_ the way the value-storage tests need it.
// It reads with cbmap_get_ptr(), so a lost key fails only this test;
// cbmap_get() would end the whole binary.
#define REQUIRE_STRING_VALUE_IS(map_, key_, expected_) \
  do {                                                 \
    char *const *found_ = cbmap_get_ptr(map_, key_);   \
    REQUIRE_NE((void *)found_, NULL);                  \
    if (found_) REQUIRE_STREQ(*found_, (expected_));   \
  } while (0)

// A value starts inside the node's allocation and moves to a buffer of its own
// when an update changes its size. This drives every crossing; a wrong one is a
// free of an interior pointer or a leak, which the memtest run catches.
TEST(cbstmap_value_storage, resizing_a_value_across_the_inline_boundary) {
  cbmap_construct_scoped(bm, int, char *);

  int key = 7;
  cbmap_insert(bm, key, "short");
  REQUIRE_STRING_VALUE_IS(bm, key, "short");

  cbmap_insert(bm, key, "a considerably longer value than the first one");
  REQUIRE_STRING_VALUE_IS(bm, key,
                          "a considerably longer value than the first one");

  cbmap_insert(bm, key, "tiny");
  REQUIRE_STRING_VALUE_IS(bm, key, "tiny");

  cbmap_insert(bm, key, "tin2");
  REQUIRE_STRING_VALUE_IS(bm, key, "tin2");

  cbmap_insert(bm, key, "grown back out to something long again");
  int other = 8;
  cbmap_insert(bm, other, "other");
  REQUIRE_STRING_VALUE_IS(bm, key, "grown back out to something long again");
  REQUIRE_STRING_VALUE_IS(bm, other, "other");

  cbmap_remove(bm, key);
  REQUIRE_STRING_VALUE_IS(bm, other, "other");
  REQUIRE_EQ(cbstmap_size(bm), (size_t)1);
}

#undef REQUIRE_STRING_VALUE_IS

// The key offset in a node is rounded to the key type's own alignment. On
// x86-64 a misaligned read works anyway, so this test pays off under
// -fsanitize=alignment and on strict-alignment targets; long double needs 16.
TEST(cbstmap_key_storage, every_key_type_round_trips_through_inline_storage) {
#define ROUND_TRIP(ctype, keyval, valval)      \
  do {                                         \
    cbmap_construct_scoped(m_, ctype, int);    \
    ctype k_ = (keyval);                       \
    int v_ = (valval);                         \
    cbmap_insert(m_, k_, v_);                  \
    int *found_ = cbmap_get_ptr(m_, k_);       \
    REQUIRE_NE((void *)found_, NULL);          \
    if (found_) REQUIRE_EQ(*found_, (valval)); \
    REQUIRE_EQ(cbstmap_size(m_), (size_t)1);   \
  } while (0)

  ROUND_TRIP(char, 'q', 1);
  ROUND_TRIP(signed char, (signed char)-7, 2);
  ROUND_TRIP(unsigned char, (unsigned char)250, 3);
  ROUND_TRIP(short, (short)-300, 4);
  ROUND_TRIP(unsigned short, (unsigned short)60000, 5);
  ROUND_TRIP(int, -123456, 6);
  ROUND_TRIP(unsigned int, 4000000000u, 7);
  ROUND_TRIP(long, -1234567L, 8);
  ROUND_TRIP(unsigned long, 1234567UL, 9);
  ROUND_TRIP(long long, -123456789LL, 10);
  ROUND_TRIP(unsigned long long, 123456789ULL, 11);
  ROUND_TRIP(float, 1.5f, 12);
  ROUND_TRIP(double, 2.25, 13);
#undef ROUND_TRIP

  {
    cbmap_construct_scoped(mld, long double, int);
    long double k = 3.0625L;
    int v = 14;
    cbmap_insert(mld, k, v);
    int *found = cbmap_get_ptr(mld, k);
    REQUIRE_NE((void *)found, NULL);
    if (found) REQUIRE_EQ(*found, 14);
  }

  {
    int target = 0;
    cbmap_construct_scoped(mp, int *, int);
    int *pk = &target;
    int pv = 15;
    cbmap_insert(mp, pk, pv);
    int *found = cbmap_get_ptr(mp, pk);
    REQUIRE_NE((void *)found, NULL);
    if (found) REQUIRE_EQ(*found, 15);
  }
  {
    cbmap_construct_scoped(ms, char *, int);
    int sv = 16;
    cbmap_insert(ms, "a string key", sv);
    int *found = cbmap_get_ptr(ms, "a string key");
    REQUIRE_NE((void *)found, NULL);
    if (found) REQUIRE_EQ(*found, 16);
  }
}

TEST(cbst_maps, null_pair_arguments_are_rejected_like_chashmap_rejects_them) {
  cbmap cbm = cbstmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 3, val = 42;
  const cmap_pair valid_key = {.ptr = &key, .size = sizeof(key)};
  const cmap_pair valid_val = {.ptr = &val, .size = sizeof(val)};

  const ccol_retval insert_null_key =
      cbstmap_insert_elem(cbm, NULL, &valid_val);
  const ccol_retval insert_null_val =
      cbstmap_insert_elem(cbm, &valid_key, NULL);
  const ccol_retval insert_both_null = cbstmap_insert_elem(cbm, NULL, NULL);

  int readback = -1;
  const ccol_retval copy_null_key =
      cbstmap_get_elem_copy(cbm, NULL, &readback, sizeof(readback));

  cmap_pair out = {NULL, 0};
  const ccol_retval ref_null_key = cbstmap_get_elem_ref(cbm, NULL, &out);
  const ccol_retval ref_null_out = cbstmap_get_elem_ref(cbm, &valid_key, NULL);

  const ccol_retval delete_null_key = cbstmap_remove_elem(cbm, NULL);

  const size_t count_after_rejects = cbstmap_size(cbm);
  const ccol_retval good_insert =
      cbstmap_insert_elem(cbm, &valid_key, &valid_val);
  const size_t count_after_good = cbstmap_size(cbm);
  const ccol_retval good_copy =
      cbstmap_get_elem_copy(cbm, &valid_key, &readback, sizeof(readback));

  cbmap_destroy(cbm);

  REQUIRE_EQ(insert_null_key, ccol_invalid_args);
  REQUIRE_EQ(insert_null_val, ccol_invalid_args);
  REQUIRE_EQ(insert_both_null, ccol_invalid_args);
  REQUIRE_EQ(copy_null_key, ccol_invalid_args);
  REQUIRE_EQ(ref_null_key, ccol_invalid_args);
  REQUIRE_EQ(ref_null_out, ccol_invalid_args);
  REQUIRE_EQ(delete_null_key, ccol_invalid_args);
  REQUIRE_EQ(count_after_rejects, (size_t)0);
  REQUIRE_EQ(good_insert, ccol_success);
  REQUIRE_EQ(count_after_good, (size_t)1);
  REQUIRE_EQ(good_copy, ccol_success);
  REQUIRE_EQ(readback, 42);
  REQUIRE_EQ(out.ptr, NULL);
}

// The tree is filled first so the lookup and delete paths reach compare_keys();
// on an empty tree they'd return ccol_key_not_found without reading the pair.
TEST(raw_entry_points, a_null_pointer_pair_with_a_nonzero_size_is_rejected) {
  cbmap_construct(m, int, int);

  cmap_pair unreadable = {.ptr = NULL, .size = sizeof(int)};
  int real_key = 7, real_val = 42;
  cmap_pair key_pair = {.ptr = &real_key, .size = sizeof(int)};
  cmap_pair val_pair = {.ptr = &real_val, .size = sizeof(int)};

  REQUIRE_EQ(cbstmap_insert_elem(m, &unreadable, &val_pair), ccol_invalid_args);
  REQUIRE_EQ(cbstmap_insert_elem(m, &key_pair, &unreadable), ccol_invalid_args);

  REQUIRE_EQ(cbstmap_insert_elem(m, &key_pair, &val_pair), ccol_success);

  int out = 0;
  REQUIRE_EQ(cbstmap_get_elem_copy(m, &unreadable, &out, sizeof(out)),
             ccol_invalid_args);
  cmap_pair ref = {NULL, 0};
  REQUIRE_EQ(cbstmap_get_elem_ref(m, &unreadable, &ref), ccol_invalid_args);
  REQUIRE_EQ(cbstmap_remove_elem(m, &unreadable), ccol_invalid_args);

  out = 0;
  REQUIRE_EQ(cbstmap_get_elem_copy(m, &key_pair, &out, sizeof(out)),
             ccol_success);
  REQUIRE_EQ(out, 42);

  cbmap_destroy(m);
}

// A compound literal argument lives only in the block it's created in. Without
// the macros' own copy it would die inside ccol__populate_cmap_pair(), which
// gcc -O3 and AddressSanitizer both show.
TEST(cbmap_macro_arg_lifetime, a_compound_literal_value_survives_the_insert) {
  cbmap_construct_scoped(bm, int, int);

  int key = 7;
  cbmap_insert(bm, key, (int){12345});

  int *found = cbmap_get_ptr(bm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) REQUIRE_EQ(*found, 12345);
}

TEST(cbmap_macro_arg_lifetime, a_compound_literal_key_survives_the_insert) {
  cbmap_construct_scoped(bm, int, int);

  int value = 900;
  cbmap_insert(bm, (int){7}, value);

  int key = 7;
  int *found = cbmap_get_ptr(bm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) REQUIRE_EQ(*found, 900);
}

TEST(cbmap_macro_arg_lifetime, a_compound_literal_key_survives_every_lookup) {
  cbmap_construct_scoped(bm, int, int);

  int key = 7, value = 900;
  cbmap_insert(bm, key, value);

  int *found = cbmap_get_ptr(bm, (int){7});
  REQUIRE_NE((void *)found, NULL);
  if (found) REQUIRE_EQ(*found, 900);

  REQUIRE_EQ(cbmap_get(bm, (int){7}), 900);

  REQUIRE_EQ(cbmap_remove(bm, (int){7}), ccol_success);
  REQUIRE_EQ(cbstmap_size(bm), (size_t)0);
}

// A struct value wider than any register, for the lifetime tests.
typedef struct {
  long a;
  long b;
  long c;
  long d;
} cbmap_lifetime_wide_value;

// A struct literal behind a macro, so its commas don't split the cbmap_insert()
// arguments; it's created where it's written.
#define CBMAP_LIFETIME_WIDE_LITERAL \
  (cbmap_lifetime_wide_value){.a = 11, .b = 22, .c = 33, .d = 44}

TEST(cbmap_macro_arg_lifetime, a_compound_literal_struct_value_survives) {
  cbmap_construct_scoped(bm, int, cbmap_lifetime_wide_value);

  int key = 7;
  cbmap_insert(bm, key, CBMAP_LIFETIME_WIDE_LITERAL);

  cbmap_lifetime_wide_value *found = cbmap_get_ptr(bm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) {
    REQUIRE_EQ(found->a, 11L);
    REQUIRE_EQ(found->b, 22L);
    REQUIRE_EQ(found->c, 33L);
    REQUIRE_EQ(found->d, 44L);
  }
}

// The guarantee is that a store through the result doesn't compile, so the test
// checks the result's type with _Generic, which doesn't evaluate the map.
TEST(cbmap_string_value_ref, get_ptr_on_a_string_map_is_not_writable) {
  cbmap_construct_scoped(bm, char *, char *);

  char key[] = "k";
  cbmap_insert(bm, key, "a value that is twenty-eight");

  REQUIRE_EQ(_Generic(cbmap_get_ptr(bm, key),
                 char *const *: 1,
                 char **: 0,
                 default: -1),
             1);
}

TEST(cbmap_string_value_ref, get_ptr_on_a_non_string_map_stays_writable) {
  cbmap_construct_scoped(bm, char *, int);

  char key[] = "k";
  int value = 1;
  cbmap_insert(bm, key, value);

  REQUIRE_EQ(
      _Generic(cbmap_get_ptr(bm, key), int *: 1, const int *: 0, default: -1),
      1);

  int *found = cbmap_get_ptr(bm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) {
    *found = 200;
    REQUIRE_EQ(cbmap_get(bm, key), 200);
  }
}

TEST(cbmap_string_value_ref, a_string_reference_reads_and_edits_in_place) {
  cbmap_construct_scoped(bm, char *, char *);

  char key[] = "k";
  cbmap_insert(bm, key, "hello");

  char *const *found = cbmap_get_ptr(bm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) {
    REQUIRE_STREQ(*found, "hello");
    (*found)[0] = 'j';
    REQUIRE_STREQ(cbmap_get(bm, key), "jello");

    char buf[6] = {0};
    cmap_pair key_pair = {.ptr = key, .size = sizeof(key)};
    REQUIRE_EQ(cbstmap_get_elem_copy(bm, &key_pair, buf, sizeof(buf)),
               ccol_success);
    REQUIRE_STREQ(buf, "jello");
  }
}

TEST(cbmap_elem_ref_accessor, the_filled_pair_reads_and_edits_bytes) {
  cbmap_construct_scoped(bm, char *, char *);

  char key[] = "k";
  cbmap_insert(bm, key, "hello");

  cmap_pair key_pair = {.ptr = key, .size = sizeof(key)};
  cmap_pair val_pair = {NULL, 0};
  REQUIRE_EQ(cbstmap_get_elem_ref(bm, &key_pair, &val_pair), ccol_success);
  REQUIRE_NE(val_pair.ptr, NULL);
  if (!val_pair.ptr) return;

  REQUIRE_EQ(val_pair.size, sizeof("hello"));
  REQUIRE_STREQ((const char *)val_pair.ptr, "hello");

  ((char *)val_pair.ptr)[0] = 'j';
  REQUIRE_STREQ(cbmap_get(bm, key), "jello");

  char buf[6] = {0};
  REQUIRE_EQ(cbstmap_get_elem_copy(bm, &key_pair, buf, sizeof(buf)),
             ccol_success);
  REQUIRE_STREQ(buf, "jello");
}

TEST(cbmap_elem_ref_accessor, references_for_distinct_keys_are_independent) {
  cbmap_construct_scoped(bm, int, int);

  for (int i = 0; i < 64; i++) {
    int k = i, v = i * 3;
    cbmap_insert(bm, k, v);
  }

  int first = 7, second = 41;
  cmap_pair first_key = {.ptr = &first, .size = sizeof(first)};
  cmap_pair second_key = {.ptr = &second, .size = sizeof(second)};
  cmap_pair first_ref = {NULL, 0};
  cmap_pair second_ref = {NULL, 0};

  REQUIRE_EQ(cbstmap_get_elem_ref(bm, &first_key, &first_ref), ccol_success);
  REQUIRE_EQ(cbstmap_get_elem_ref(bm, &second_key, &second_ref), ccol_success);
  REQUIRE_NE(first_ref.ptr, second_ref.ptr);

  for (int i = 0; i < 64; i++) {
    int other = i;
    cmap_pair other_key = {.ptr = &other, .size = sizeof(other)};
    cmap_pair ignored = {NULL, 0};
    (void)cbstmap_get_elem_ref(bm, &other_key, &ignored);
  }

  REQUIRE_EQ(*(const int *)first_ref.ptr, 21);
  REQUIRE_EQ(*(const int *)second_ref.ptr, 123);

  *(int *)first_ref.ptr = 999;
  REQUIRE_EQ(*(const int *)first_ref.ptr, 999);
  REQUIRE_EQ(*(const int *)second_ref.ptr, 123);
}

TEST(cbmap_elem_ref_accessor, the_pair_contract_holds) {
  cbmap_construct(bm, int, long);
  for (int i = 0; i < 100; i++) {
    long v = (long)i * 3;
    cbmap_insert(bm, i, v);
  }
  int k = 17, absent = 1000;
  short wrong = 17;
  cmap_pair kp = {&k, sizeof(k)}, kpa = {&absent, sizeof(absent)};
  cmap_pair bad = {&wrong, sizeof(wrong)};
  cmap_pair hit = {(void *)0x1, 12345};
  ccol_retval hit_r = cbstmap_get_elem_ref(bm, &kp, &hit);
  bool ptr_in_map = false, write_seen = false;
  if (hit_r == ccol_success) {
    long *direct = cbmap_get_ptr(bm, k);
    ptr_in_map = direct == hit.ptr;
    *(long *)hit.ptr = 4242;
    write_seen = cbmap_get(bm, k) == 4242;
  }
  cmap_pair miss = {(void *)0x1, 12345};
  ccol_retval miss_r = cbstmap_get_elem_ref(bm, &kpa, &miss);
  cmap_pair refused = {(void *)0x1, 12345};
  ccol_retval refused_r = cbstmap_get_elem_ref(bm, &bad, &refused);
  cmap_pair null_key = {(void *)0x1, 12345};
  ccol_retval null_key_r = cbstmap_get_elem_ref(bm, NULL, &null_key);
  cbmap_destroy(bm);

  REQUIRE_EQ(hit_r, ccol_success);
  REQUIRE_EQ(hit.size, sizeof(long));
  REQUIRE_TRUE(ptr_in_map);
  REQUIRE_TRUE(write_seen);
  REQUIRE_EQ(miss_r, ccol_key_not_found);
  REQUIRE_EQ(miss.ptr, NULL);
  REQUIRE_EQ(miss.size, (size_t)0);
  REQUIRE_EQ(refused_r, ccol_invalid_args);
  REQUIRE_EQ(refused.ptr, NULL);
  REQUIRE_EQ(refused.size, (size_t)0);
  REQUIRE_EQ(null_key_r, ccol_invalid_args);
  REQUIRE_EQ(null_key.ptr, NULL);
  REQUIRE_EQ(null_key.size, (size_t)0);
}

TEST(cbmap_elem_ref_accessor, get_ptr_of_a_string_value_points_into_the_node) {
  cbmap_construct(bm, int, char *);
  cbmap_insert(bm, 1, "hello");
  char *const *first = cbmap_get_ptr(bm, 1);
  char *const *second = cbmap_get_ptr(bm, 1);
  char *const *absent = cbmap_get_ptr(bm, 2);
  bool same = first && first == second;
  if (first) (*first)[0] = 'j';
  char *now = cbmap_get(bm, 1);
  bool edited = strcmp(now, "jello") == 0;
  cbmap_destroy(bm);
  REQUIRE_TRUE(same);
  REQUIRE_EQ((void *)absent, NULL);
  REQUIRE_TRUE(edited);
}

// A raw pair can point at any offset, for example inside a packed frame. If the
// comparison read through a cast pointer, UndefinedBehaviorSanitizer would
// report a misaligned load at the first probe.
TEST(cbmap_raw_layer_alignment, misaligned_key_pair_resolves_for_every_type) {
  _Alignas(64) static unsigned char frame[256];

#define CBMAP_MISALIGNED_PROBE(TYPE, DATA_TYPE, VALUE, OFFSET)                 \
  do {                                                                         \
    char *probe_err = NULL;                                                    \
    cbmap probe_map = cbstmap_create(DATA_TYPE, &probe_err);                   \
    REQUIRE_NE((void *)probe_map, NULL);                                       \
    TYPE probe_key = (VALUE);                                                  \
    int probe_val = 4242;                                                      \
    cmap_pair aligned_pair = {.ptr = &probe_key, .size = sizeof(probe_key)};   \
    cmap_pair val_pair = {.ptr = &probe_val, .size = sizeof(probe_val)};       \
    REQUIRE_EQ(cbstmap_insert_elem(probe_map, &aligned_pair, &val_pair),       \
               ccol_success);                                                  \
    memset(frame, 0, sizeof(frame));                                           \
    memcpy(frame + (OFFSET), &probe_key, sizeof(probe_key));                   \
    REQUIRE_NE((size_t)((uintptr_t)(frame + (OFFSET)) % _Alignof(TYPE) == 0 && \
                        _Alignof(TYPE) > 1),                                   \
               (size_t)1);                                                     \
    cmap_pair misaligned_pair = {.ptr = frame + (OFFSET),                      \
                                 .size = sizeof(probe_key)};                   \
    cmap_pair found = {NULL, 0};                                               \
    REQUIRE_EQ(cbstmap_get_elem_ref(probe_map, &misaligned_pair, &found),      \
               ccol_success);                                                  \
    REQUIRE_EQ(*(const int *)found.ptr, 4242);                                 \
    REQUIRE_EQ(cbstmap_remove_elem(probe_map, &misaligned_pair),               \
               ccol_success);                                                  \
    REQUIRE_EQ(cbstmap_size(probe_map), (size_t)0);                            \
    cbmap_destroy(probe_map);                                                  \
  } while (0)

  CBMAP_MISALIGNED_PROBE(short, ccol_short, (short)-1234, 1);
  CBMAP_MISALIGNED_PROBE(int, ccol_int, -123456, 1);
  CBMAP_MISALIGNED_PROBE(long, ccol_long, -1234567L, 3);
  CBMAP_MISALIGNED_PROBE(long long, ccol_long_long, -123456789LL, 5);
  CBMAP_MISALIGNED_PROBE(unsigned short, ccol_unsigned_short,
                         (unsigned short)4321, 1);
  CBMAP_MISALIGNED_PROBE(unsigned int, ccol_unsigned_int, 654321u, 3);
  CBMAP_MISALIGNED_PROBE(unsigned long, ccol_unsigned_long, 7654321ul, 1);
  CBMAP_MISALIGNED_PROBE(unsigned long long, ccol_unsigned_long_long,
                         87654321ull, 7);
  CBMAP_MISALIGNED_PROBE(float, ccol_float, 3.5f, 1);
  CBMAP_MISALIGNED_PROBE(double, ccol_double, 6.25, 3);
  CBMAP_MISALIGNED_PROBE(long double, ccol_long_double, 7.125L, 1);

#undef CBMAP_MISALIGNED_PROBE
}

TEST(cbmap_raw_layer_alignment, misaligned_inserts_keep_the_sorted_order) {
  _Alignas(64) static unsigned char frame[64];
  char *err = NULL;
  cbmap bm = cbstmap_create(ccol_int, &err);
  REQUIRE_NE((void *)bm, NULL);

  for (int step = 0; step < 64; step++) {
    int k = (step * 37) % 64;
    int v = k * 5;
    memcpy(frame + 1, &k, sizeof(k));
    cmap_pair kp = {.ptr = frame + 1, .size = sizeof(k)};
    cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
    ccol_retval r = cbstmap_insert_elem(bm, &kp, &vp);
    REQUIRE_EQ(r, ccol_success);
  }
  REQUIRE_EQ(cbstmap_size(bm), (size_t)64);

  int expected = 0;
  for (cmap_iterator *it = cbstmap_begin_iter(bm, NULL); it;
       it = it->ccol__next_fn(it)) {
    int k;
    memcpy(&k, it->key_pair->ptr, sizeof(k));
    REQUIRE_EQ(k, expected);
    REQUIRE_EQ(*(const int *)it->val_pair->ptr, expected * 5);
    expected++;
  }
  REQUIRE_EQ(expected, 64);
  cbmap_destroy(bm);
}

// The size header in front of each block of the poisoning allocator, padded to
// max_align_t so the block after it stays aligned.
typedef union {
  size_t size;
  max_align_t align;
} cbmap_life_poison_hdr;

// An allocator that fills every block with 0xA5 before it frees it, so the
// iterator-lifetime tests see a use after free as a fault in an ordinary run.
static void *cbmap_life_poison_malloc(size_t size) {
  cbmap_life_poison_hdr *h = malloc(sizeof(cbmap_life_poison_hdr) + size);
  if (!h) return NULL;
  h->size = size;
  return h + 1;
}

static void *cbmap_life_poison_calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return NULL;
  void *p = cbmap_life_poison_malloc(count * size);
  if (p) memset(p, 0, count * size);
  return p;
}

static void cbmap_life_poison_free(void *ptr) {
  if (!ptr) return;
  cbmap_life_poison_hdr *h = (cbmap_life_poison_hdr *)ptr - 1;
  memset(ptr, 0xA5, h->size);
  free(h);
}

static void *cbmap_life_poison_realloc(void *ptr, size_t size) {
  if (!ptr) return cbmap_life_poison_malloc(size);
  cbmap_life_poison_hdr *h = (cbmap_life_poison_hdr *)ptr - 1;
  void *n = cbmap_life_poison_malloc(size);
  if (!n) return NULL;
  memcpy(n, ptr, h->size < size ? h->size : size);
  cbmap_life_poison_free(ptr);
  return n;
}

// The poisoning allocator as a procs table, and a getter for the macros.
static ccol_memmgmt_procs cbmap_life_poison_procs_storage = {
    .malloc = cbmap_life_poison_malloc,
    .calloc = cbmap_life_poison_calloc,
    .realloc = cbmap_life_poison_realloc,
    .free = cbmap_life_poison_free};

static ccol_memmgmt_procs *cbmap_life_poison_procs(void) {
  return &cbmap_life_poison_procs_storage;
}

// The scope-exit cleanup frees an iterator after its map and allocator copy are
// gone. An iterator that freed itself through the map would read the poisoned
// map and fault.
TEST(iterator_lifetime, break_then_destroy_then_scope_exit_custom_allocator) {
  int seen = 0;
  {
    cbmap_construct_mp(m, int, int, cbmap_life_poison_procs());
    for (int i = 0; i < 8; i++) cbmap_insert(m, i, i * 10);
    ccol_iter_declare(m, it);
    for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
      if (++seen == 3) break;
    }
    cbmap_destroy(m);
  }
  REQUIRE_EQ(seen, 3);
}

TEST(iterator_lifetime, break_then_destroy_then_scope_exit_default_allocator) {
  int seen = 0;
  {
    cbmap_construct(m, int, int);
    for (int i = 0; i < 8; i++) cbmap_insert(m, i, i);
    ccol_iter_declare(m, it);
    for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
      if (++seen == 2) break;
    }
    cbmap_destroy(m);
  }
  REQUIRE_EQ(seen, 2);
}

// A key with padding between tag and id.
typedef struct {
  char tag;
  long id;
} cbmap_padded_key;

// Set when the compiler has __builtin_clear_padding, which the macros need to
// clear the padding of their key copy.
#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define CBMAP_TEST_COMPILER_CLEARS_PADDING 1
#endif
#endif

#ifdef CBMAP_TEST_COMPILER_CLEARS_PADDING
// Gives a padded key with zeroed padding. The macros copy it member by member,
// so the zeros don't survive into their copy by themselves.
static cbmap_padded_key cbmap_padded_make(char tag, long id) {
  cbmap_padded_key k;
  memset(&k, 0, sizeof(k));
  k.tag = tag;
  k.id = id;
  return k;
}

// Fills the stack below the caller with 0xAB, so a key copy that leaves its
// padding alone shows non-zero bytes.
static __attribute__((noinline)) void cbmap_padded_dirty_stack(void) {
  volatile unsigned char buf[4096];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0xAB;
}

// Inserts or looks up a padded key from its own stack frame, on top of the
// dirtied stack, as the padding test needs.
static __attribute__((noinline)) void cbmap_padded_insert(cbmap m, int i) {
  cbmap_redeclare(m, cbmap_padded_key, int);
  cbmap_insert(m, cbmap_padded_make('x', i), i);
}

static __attribute__((noinline)) int *cbmap_padded_lookup(cbmap m, int i) {
  cbmap_redeclare(m, cbmap_padded_key, int);
  return cbmap_get_ptr(m, cbmap_padded_make('x', i));
}

#endif

// A key of an unknown type compares byte for byte, padding included. Under GCC
// the test fails without the clearing: The padding carries the stack pattern
// and the lookups miss.
TEST(padded_key, the_macros_clear_the_padding_of_their_key_copy) {
#ifdef CBMAP_TEST_COMPILER_CLEARS_PADDING
  cbmap_construct(m, cbmap_padded_key, int);
  for (int i = 0; i < 100; i++) {
    cbmap_padded_dirty_stack();
    cbmap_padded_insert(m, i);
  }
  bool all_found = true;
  for (int i = 0; i < 100; i++) {
    cbmap_padded_dirty_stack();
    int *v = cbmap_padded_lookup(m, i);
    if (!v || *v != i) all_found = false;
  }
  bool padding_zero = true;
  size_t pad_start = offsetof(cbmap_padded_key, tag) + 1;
  size_t pad_end = offsetof(cbmap_padded_key, id);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
    const unsigned char *b = (const unsigned char *)it->key_pair->ptr;
    for (size_t j = pad_start; j < pad_end; j++) {
      if (b[j] != 0) padding_zero = false;
    }
  }
  size_t n = cbstmap_size(m);
  cbmap_destroy(m);
  REQUIRE_TRUE(all_found);
  REQUIRE_TRUE(padding_zero);
  REQUIRE_EQ(n, (size_t)100);
#endif
}

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

// A variable that is never set, so getenv() gives a real NULL char * the
// compiler can't see through.
#define CBMAP_NULL_TEST_UNSET_VAR "CCOL_CBMAP_TEST_SURELY_UNSET_VARIABLE"

// What cbmap_null_run_probe() reports when the child dies instead of writing a
// result.
enum { cbmap_null_probe_crashed = 250, cbmap_null_probe_aborted = 251 };

// Runs probe in a forked child and gives its result byte, or
// cbmap_null_probe_aborted for SIGABRT and cbmap_null_probe_crashed for any
// other death. The NULL key tests need it because the typed macros end the
// process on purpose. The result goes through a pipe, since valgrind overrides
// the exit status of a child with reachable memory.
static int cbmap_null_run_probe(int (*probe)(void)) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    close(fds[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    unsigned char byte = (unsigned char)probe();
    ssize_t w = write(fds[1], &byte, 1);
    (void)w;
    test_child_exit(0);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return -1;
  }
  unsigned char byte = 0;
  ssize_t n = read(fds[0], &byte, 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (n == 1) return byte;
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
    return cbmap_null_probe_aborted;
  }
  return cbmap_null_probe_crashed;
}

// Probes for the NULL char pointer tests. Each passes a NULL key or value to
// one typed macro and gives 1 when the map behaved as documented; the fatal
// ones never reach their return.
static int cbmap_null_probe_get_ptr_key(void) {
  unsetenv(CBMAP_NULL_TEST_UNSET_VAR);
  cbmap_construct(b, char *, int);
  cbmap_insert(b, "HOME", 1);
  int *p = cbmap_get_ptr(b, getenv(CBMAP_NULL_TEST_UNSET_VAR));
  int ok = p == NULL && cbstmap_size(b) == 1;
  cbmap_destroy(b);
  return ok;
}

static int cbmap_null_probe_remove_key(void) {
  unsetenv(CBMAP_NULL_TEST_UNSET_VAR);
  cbmap_construct(b, char *, int);
  cbmap_insert(b, "HOME", 1);
  ccol_retval r = cbmap_remove(b, getenv(CBMAP_NULL_TEST_UNSET_VAR));
  int ok = r == ccol_invalid_args && cbstmap_size(b) == 1;
  cbmap_destroy(b);
  return ok;
}

static int cbmap_null_probe_insert_key(void) {
  unsetenv(CBMAP_NULL_TEST_UNSET_VAR);
  cbmap_construct(b, char *, int);
  cbmap_insert(b, getenv(CBMAP_NULL_TEST_UNSET_VAR), 1);
  cbmap_destroy(b);
  return 1;
}

static int cbmap_null_probe_insert_value(void) {
  unsetenv(CBMAP_NULL_TEST_UNSET_VAR);
  cbmap_construct(b, int, char *);
  cbmap_insert(b, 1, getenv(CBMAP_NULL_TEST_UNSET_VAR));
  cbmap_destroy(b);
  return 1;
}

static int cbmap_null_probe_get_key(void) {
  unsetenv(CBMAP_NULL_TEST_UNSET_VAR);
  cbmap_construct(b, char *, int);
  cbmap_insert(b, "HOME", 1);
  int v = cbmap_get(b, getenv(CBMAP_NULL_TEST_UNSET_VAR));
  cbmap_destroy(b);
  return v == 1;
}

// A NULL char pointer isn't a string. Without the NULL test in
// ccol__populate_cmap_pair() the probes die in strlen(); without the macros'
// refusal the inserts store an empty key or value and succeed.
TEST(cbmap_null_char_ptr, get_ptr_of_a_null_key_gives_null) {
  REQUIRE_EQ(cbmap_null_run_probe(cbmap_null_probe_get_ptr_key), 1);
}

TEST(cbmap_null_char_ptr, remove_of_a_null_key_is_invalid_args) {
  REQUIRE_EQ(cbmap_null_run_probe(cbmap_null_probe_remove_key), 1);
}

TEST(cbmap_null_char_ptr, insert_of_a_null_key_is_fatal) {
  REQUIRE_EQ(cbmap_null_run_probe(cbmap_null_probe_insert_key),
             (int)cbmap_null_probe_aborted);
}

TEST(cbmap_null_char_ptr, insert_of_a_null_value_is_fatal) {
  REQUIRE_EQ(cbmap_null_run_probe(cbmap_null_probe_insert_value),
             (int)cbmap_null_probe_aborted);
}

TEST(cbmap_null_char_ptr, get_of_a_null_key_is_fatal) {
  REQUIRE_EQ(cbmap_null_run_probe(cbmap_null_probe_get_key),
             (int)cbmap_null_probe_aborted);
}

// A 32-byte digest wrapped in a struct, used as a binary key.
typedef struct {
  unsigned char b[32];
} cbmap_test_digest;

TEST(cbmap_binary_keys, a_digest_struct_key_compares_every_byte) {
  cbmap_construct(seen, cbmap_test_digest, int);
  cbmap_test_digest d1, d2, d3;
  for (int i = 0; i < 32; i++) {
    d1.b[i] = (unsigned char)(i * 17 + 1);
  }
  d1.b[3] = 0;
  d2 = d1;
  d2.b[30] ^= 0xff;
  d3 = d1;
  d3.b[0] = 0;
  cbmap_insert(seen, d1, 1);
  cbmap_insert(seen, d2, 2);
  cbmap_insert(seen, d3, 3);
  size_t count = cbstmap_size(seen);
  int v1 = cbmap_get(seen, d1);
  int v2 = cbmap_get(seen, d2);
  int v3 = cbmap_get(seen, d3);
  cbmap_destroy(seen);
  REQUIRE_EQ(count, (size_t)3);
  REQUIRE_EQ(v1, 1);
  REQUIRE_EQ(v2, 2);
  REQUIRE_EQ(v3, 3);
}

TEST(cbmap_binary_keys, a_raw_layer_key_with_embedded_zeros) {
  cbmap m = cbstmap_create(ccol_other_types, NULL);
  REQUIRE_NE((void *)m, NULL);
  unsigned char k1[16], k2[16], k3[5];
  for (int i = 0; i < 16; i++) k1[i] = (unsigned char)(i * 13 + 7);
  k1[3] = 0;
  memcpy(k2, k1, sizeof(k1));
  k2[10] ^= 0xff;
  memcpy(k3, k1, sizeof(k3));
  int v1 = 1, v2 = 2, v3 = 3;
  cmap_pair kp1 = {k1, sizeof(k1)}, vp1 = {&v1, sizeof(v1)};
  cmap_pair kp2 = {k2, sizeof(k2)}, vp2 = {&v2, sizeof(v2)};
  cmap_pair kp3 = {k3, sizeof(k3)}, vp3 = {&v3, sizeof(v3)};
  ccol_retval r1 = cbstmap_insert_elem(m, &kp1, &vp1);
  ccol_retval r2 = cbstmap_insert_elem(m, &kp2, &vp2);
  ccol_retval r3 = cbstmap_insert_elem(m, &kp3, &vp3);
  size_t count = cbstmap_size(m);
  unsigned char probe[16];
  memcpy(probe, k2, sizeof(probe));
  cmap_pair pp = {probe, sizeof(probe)};
  cmap_pair out = {NULL, 0};
  ccol_retval rg = cbstmap_get_elem_ref(m, &pp, &out);
  int got = -1;
  if (rg == ccol_success && out.ptr) memcpy(&got, out.ptr, sizeof(got));
  cmap_pair pp3 = {k3, sizeof(k3)};
  cmap_pair out3 = {NULL, 0};
  ccol_retval rg3 = cbstmap_get_elem_ref(m, &pp3, &out3);
  int got3 = -1;
  if (rg3 == ccol_success && out3.ptr) memcpy(&got3, out3.ptr, sizeof(got3));
  cbmap_destroy(m);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_EQ(count, (size_t)3);
  REQUIRE_EQ(got, 2);
  REQUIRE_EQ(got3, 3);
}

// Inserts the values in a scrambled order and checks that the walk increases
// under T's own <, that every key is found with its value and that absent is
// missing. The value sets mix signs and top-bit values, so a comparison of the
// wrong signedness or width gives another order.
#define CBMAP_TYPED_ORDER_CASE(T, absent, ...)                           \
  do {                                                                   \
    T vals_[] = {__VA_ARGS__};                                           \
    const size_t n_ = sizeof(vals_) / sizeof(vals_[0]);                  \
    _Static_assert(sizeof(vals_) / sizeof(vals_[0]) < 11, "scramble");   \
    cbmap_construct(om, T, int);                                         \
    for (size_t i_ = 0; i_ < n_; ++i_) {                                 \
      size_t j_ = (i_ * 11 + 3) % n_;                                    \
      T k_ = vals_[j_];                                                  \
      int v_ = (int)j_;                                                  \
      cbmap_insert(om, k_, v_);                                          \
    }                                                                    \
    bool ok_ = cbstmap_size(om) == n_ && cbmap_debug_validate_avl(om);   \
    size_t seen_ = 0;                                                    \
    T prev_ = vals_[0];                                                  \
    ccol_iter_declare(om, it_);                                          \
    for (it_ = ccol_begin(om); it_ != NULL; it_ = ccol_iter_next(it_)) { \
      T k_ = *ccol_iter_key_ptr(it_);                                    \
      if (seen_ > 0 && !(prev_ < k_)) ok_ = false;                       \
      prev_ = k_;                                                        \
      ++seen_;                                                           \
    }                                                                    \
    ok_ = ok_ && seen_ == n_;                                            \
    for (size_t i_ = 0; i_ < n_; ++i_) {                                 \
      T k_ = vals_[i_];                                                  \
      int *p_ = cbmap_get_ptr(om, k_);                                   \
      if (!p_ || *p_ != (int)i_) ok_ = false;                            \
    }                                                                    \
    T a_ = (absent);                                                     \
    if (cbmap_get_ptr(om, a_) != NULL) ok_ = false;                      \
    cbmap_destroy(om);                                                   \
    REQUIRE_TRUE(ok_);                                                   \
  } while (0)

TEST(cbst_maps, typed_key_kinds_order_and_find_like_native_less_than) {
  CBMAP_TYPED_ORDER_CASE(char, 'q', 'a', 'z', '0', 'M', ' ', '~', '\x7f',
                         (char)0x80, (char)0xff, (char)0xa0);
  CBMAP_TYPED_ORDER_CASE(signed char, 5, -128, 127, -1, 0, 1, -64, 64, 100);
  CBMAP_TYPED_ORDER_CASE(unsigned char, 7, 0, 255, 128, 127, 1, 200, 64);
  CBMAP_TYPED_ORDER_CASE(short, 3, SHRT_MIN, SHRT_MAX, -1, 0, 1, -300, 300);
  CBMAP_TYPED_ORDER_CASE(unsigned short, 3, 0, USHRT_MAX, 0x8000, 0x7fff, 1,
                         40000);
  CBMAP_TYPED_ORDER_CASE(int, 3, INT_MIN, INT_MAX, -1, 0, 1, -70000, 70000);
  CBMAP_TYPED_ORDER_CASE(unsigned int, 3, 0u, UINT_MAX, 0x80000000u,
                         0x7fffffffu, 1u, 3000000000u);
  CBMAP_TYPED_ORDER_CASE(long, 3L, LONG_MIN, LONG_MAX, -1L, 0L, 1L, -70000L,
                         70000L);
  CBMAP_TYPED_ORDER_CASE(unsigned long, 3UL, 0UL, ULONG_MAX, 1UL, ULONG_MAX / 2,
                         ULONG_MAX / 2 + 1, 99UL);
  CBMAP_TYPED_ORDER_CASE(long long, 3LL, LLONG_MIN, LLONG_MAX, -1LL, 0LL, 1LL,
                         -5000000000LL, 5000000000LL);
  CBMAP_TYPED_ORDER_CASE(unsigned long long, 3ULL, 0ULL, ULLONG_MAX,
                         0x8000000000000000ULL, 0x7fffffffffffffffULL, 1ULL,
                         0xfffffffffffffff0ULL);
  CBMAP_TYPED_ORDER_CASE(
      void *, (void *)(uintptr_t)0x30, (void *)(uintptr_t)0x10,
      (void *)(uintptr_t)UINTPTR_MAX, (void *)(uintptr_t)(UINTPTR_MAX / 2 + 1),
      (void *)(uintptr_t)(UINTPTR_MAX / 2), (void *)(uintptr_t)0x20,
      (void *)(uintptr_t)(UINTPTR_MAX - 15));
  CBMAP_TYPED_ORDER_CASE(double, 0.5, -1e300, 1e300, -0.25, 0.0, 2.5, -7.0);
}

// Orders int keys descending, to show that a custom comparator overrides the
// typed descent of an int key.
static int cbmap_tests_reverse_int_cmp(const void *a, const void *b) {
  int x, y;
  memcpy(&x, a, sizeof(x));
  memcpy(&y, b, sizeof(y));
  return (y > x) - (y < x);
}

TEST(cbst_maps, custom_proc_on_an_integer_key_still_decides_the_order) {
  cbmap_construct_cc(rm, int, int, cbmap_tests_reverse_int_cmp);
  for (int i = 0; i < 64; ++i) {
    int k = (i * 37) % 64, v = k * 2;
    cbmap_insert(rm, k, v);
  }
  bool ok = cbstmap_size(rm) == 64 && cbmap_debug_validate_avl(rm);
  int expected = 63;
  ccol_iter_declare(rm, it);
  for (it = ccol_begin(rm); it != NULL; it = ccol_iter_next(it)) {
    if (*ccol_iter_key_ptr(it) != expected) ok = false;
    --expected;
  }
  ok = ok && expected == -1;
  for (int k = 0; k < 64; ++k) {
    int *p = cbmap_get_ptr(rm, k);
    if (!p || *p != k * 2) ok = false;
  }
  for (int k = 0; k < 64; k += 2) cbmap_remove(rm, k);
  ok = ok && cbstmap_size(rm) == 32 && cbmap_debug_validate_avl(rm);
  for (int k = 0; k < 64; ++k) {
    if ((cbmap_get_ptr(rm, k) != NULL) != (k % 2 == 1)) ok = false;
  }
  cbmap_destroy(rm);
  REQUIRE_TRUE(ok);
}

// The key range of the deep churn test, large enough for rotations far from the
// leaves.
#define DEEP_CHURN_KEY_RANGE 4096

// Insert and delete stop rebalancing at the first unchanged height. A stop one
// level too early leaves a stale height that the validator reports.
TEST(cbst_maps, deep_churn_keeps_every_height_and_balance_exact) {
  ccol_invariants_rng_t rng;
  uint64_t seed = CCOL_INVARIANTS_DEFAULT_SEED ^ 0x5bd1e995u;
  ccol_invariants_seed(&rng, seed);
  ccol_invariants_print_seed("cbst_maps.deep_churn", seed);

  cbmap_construct(dm, int, int);
  static bool present[DEEP_CHURN_KEY_RANGE];
  memset(present, 0, sizeof(present));
  size_t count = 0;
  bool ok = true;

  for (int phase = 0; phase < 12 && ok; ++phase) {
    unsigned insert_share = (phase % 2 == 0) ? 85 : 15;
    for (int i = 0; i < 3000 && ok; ++i) {
      int key = (int)ccol_invariants_next_bounded(&rng, DEEP_CHURN_KEY_RANGE);
      if (ccol_invariants_next_bounded(&rng, 100) < insert_share) {
        int val = key ^ 0x55;
        cbmap_insert(dm, key, val);
        if (!present[key]) ++count;
        present[key] = true;
      } else {
        cbmap_remove(dm, key);
        if (present[key]) --count;
        present[key] = false;
      }
      if (!cbmap_debug_validate_avl(dm) || cbstmap_size(dm) != count) {
        ok = false;
      }
    }
  }
  for (int k = 0; k < DEEP_CHURN_KEY_RANGE && ok; ++k) {
    int *p = cbmap_get_ptr(dm, k);
    if (present[k] ? (!p || *p != (k ^ 0x55)) : p != NULL) ok = false;
  }
  for (int lo = 0, hi = DEEP_CHURN_KEY_RANGE - 1; lo <= hi && ok; ++lo, --hi) {
    cbmap_remove(dm, lo);
    if (lo != hi) cbmap_remove(dm, hi);
    if (!cbmap_debug_validate_avl(dm)) ok = false;
  }
  ok = ok && cbstmap_size(dm) == 0;
  cbmap_destroy(dm);
  REQUIRE_TRUE(ok);
}

TEST(cbmap_aliases, raw_destroy_evaluates_its_argument_once) {
  cbmap maps[2] = {cbstmap_create(ccol_int, NULL),
                   cbstmap_create(ccol_int, NULL)};
  bool created = maps[0] && maps[1];
  int k = 2;
  while (k > 0) cbstmap_destroy(maps[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ((void *)maps[0], NULL);
  REQUIRE_EQ((void *)maps[1], NULL);
}

TEST(cbmap_aliases, size_and_reset_follow_the_raw_layer) {
  cbmap_construct(m, int, int);
  for (int i = 0; i < 40; i++) cbmap_insert(m, i, i * 3);
  size_t typed = cbmap_size(m);
  size_t raw = cbstmap_size(m);
  ccol_retval rv = cbmap_reset(m);
  size_t after = cbmap_size(m);
  cbmap_insert(m, 7, 8);
  int back = cbmap_get(m, 7);
  cbmap_destroy(m);
  REQUIRE_EQ(typed, (size_t)40);
  REQUIRE_EQ(raw, (size_t)40);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(after, (size_t)0);
  REQUIRE_EQ(back, 8);
  REQUIRE_EQ((void *)m, NULL);
}
