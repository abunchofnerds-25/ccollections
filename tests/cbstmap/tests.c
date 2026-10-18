#include <cbstmap.h>
#include <common_invariants.h>
#include <internal/test_helpers/cbstmap.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>

TAU_MAIN()  // Sets up Tau and gives the main function

// BST_MAP TESTS (AVL Tree)

TEST(cbst_maps, create_fails) {
  char *err = NULL;
  cbinarymap *cbmap = cbmap_create(ccol_int, &err);
  REQUIRE_NE((void *)cbmap, NULL);
  cbmap_destroy(cbmap);

  cbmap = cbmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs_t){
          .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  cbmap = cbmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  cbmap = cbmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  cbmap = cbmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL},
      &err);
  REQUIRE_EQ((void *)cbmap, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(cbst_maps, create_succeeds) {
  char *err = "";
  cbinarymap *cbmap = cbmap_create(ccol_int, &err);
  REQUIRE_NE((void *)cbmap, NULL);
  REQUIRE_EQ((void *)err, NULL);

  cbmap_destroy(cbmap);
  REQUIRE_EQ((void *)cbmap, NULL);

  cbmap = cbmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err);

  cbmap_destroy(cbmap);
  REQUIRE_EQ((void *)cbmap, NULL);
}

// The helper functions start here.
int insert_int_to_int(cbinarymap *cbmap, int key, int val) {
  return cbmap_insert_elem(
      cbmap, &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)},
      &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)});
}

int get_int_from_int(cbinarymap *cbmap, int key, int *val_ptr) {
  return cbmap_get_elem_copy(
      cbmap, &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)}, val_ptr,
      sizeof(int));
}

int get_int_ref_from_int(cbinarymap *cbmap, int key, int **val_ptr) {
  cmap_pair tmp_val_pair_ptr = {NULL, 0};
  if (cbmap_get_elem_ref(cbmap,
                         &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)},
                         &tmp_val_pair_ptr) == 0) {
    *val_ptr = (int *)tmp_val_pair_ptr.ptr;
    return 0;
  }
  return -1;
}

ccol_retval_t delete_int_from_int(cbinarymap *cbmap, int key) {
  return cbmap_delete_elem(
      cbmap, &(cmap_pair){.ptr = (void *)&key, .size = sizeof(key)});
}
// The helper functions end here.

/* The type-inferred macros give their private copy of a caller expression
   the tree's DECLARED key or value type, so the copy is a plain C assignment
   and converts. Copying into typeof(key) instead would put the RAW BYTES of
   the caller expression into the tree whenever the two types happen to have
   the same width, with no size check and no compiler diagnostic able to
   notice. cvec_push() and chmap_insert() follow the same rule. */

TEST(cbst_maps, value_converts_to_the_declared_type_and_is_not_reinterpreted) {
  cbmap_construct(b, char *, int);
  float f = 1.5f;
  cbmap_insert(b, "k", f);
  /* The converted value is 1. The bits of 1.5f read as an int give
     1069547520, which is what a raw byte copy would store. */
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
  REQUIRE_EQ(cbmap_elem_count(b), (size_t)1);
  cbmap_destroy(b);
}

TEST(cbst_maps, narrower_value_widens_to_the_declared_type) {
  /* Without the conversion the node holds two bytes, the insert reports
     success, and cbmap_get()'s value-size guard stops the process on the
     first READ, at a call site that did nothing wrong. */
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
  REQUIRE_EQ(cbmap_elem_count(b), (size_t)1);
  cbmap_insert(b, "shared", 9);
  REQUIRE_EQ(cbmap_elem_count(b), (size_t)1);
  REQUIRE_EQ(cbmap_get(b, buf), 9);
  cbmap_destroy(b);
}

TEST(cbst_maps, basic_insertions_and_lookups) {
  cbinarymap *cbmap = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  int val = -1;

  REQUIRE_EQ(get_int_from_int(cbmap, 100, &val), ccol_key_not_found);

  REQUIRE_EQ(cbmap_elem_count(cbmap), 0);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 20), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 1);
  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 30), ccol_key_already_present);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 1);

  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 40), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 2);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 50), ccol_key_already_present);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 2);

  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 30);
  REQUIRE_EQ(get_int_from_int(cbmap, 20, &val), ccol_success);
  REQUIRE_EQ(val, 50);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, basic_insertions_and_lookups_with_memmgmt_procs) {
  cbinarymap *cbmap = cbmap_create_mp(
      ccol_int,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  int val = -1;

  REQUIRE_EQ(cbmap_elem_count(cbmap), 0);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 20), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 1);
  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 30), ccol_key_already_present);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 1);

  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 40), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 2);

  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 30);
  REQUIRE_EQ(get_int_from_int(cbmap, 20, &val), ccol_success);
  REQUIRE_EQ(val, 40);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, insert_values_with_different_sizes) {
  cbinarymap *cbmap = cbmap_create(ccol_int, NULL);

  // Both keys are int, which matches the map's declared ccol_int key type.
  // Values can have any size, but keys of a fixed-width key type can't
  // (see wrong_size_key_rejected_for_fixed_width_key_type below).
  int key1 = 3;
  long val1 = 43;
  REQUIRE_EQ(
      cbmap_insert_elem(
          cbmap, &(cmap_pair){.ptr = (void *)&key1, .size = sizeof(key1)},
          &(cmap_pair){.ptr = (void *)&val1, .size = sizeof(val1)}),
      ccol_success);

  int key2 = 4;
  char val2 = 44;
  REQUIRE_EQ(
      cbmap_insert_elem(
          cbmap, &(cmap_pair){.ptr = (void *)&key2, .size = sizeof(key2)},
          &(cmap_pair){.ptr = (void *)&val2, .size = sizeof(val2)}),
      ccol_success);

  long val1_from_map = -1;
  REQUIRE_EQ(
      cbmap_get_elem_copy(
          cbmap, &(cmap_pair){.ptr = (void *)&key1, .size = sizeof(key1)},
          &val1_from_map, sizeof(val1_from_map)),
      ccol_success);
  REQUIRE_EQ(val1_from_map, val1);

  char val2_from_map = -1;
  REQUIRE_EQ(
      cbmap_get_elem_copy(
          cbmap, &(cmap_pair){.ptr = (void *)&key2, .size = sizeof(key2)},
          &val2_from_map, sizeof(val2_from_map)),
      ccol_success);
  REQUIRE_EQ(val2_from_map, val2);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, basic_deletions) {
  cbinarymap *cbmap = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 100), ccol_success);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 200), ccol_success);
  REQUIRE_EQ(insert_int_to_int(cbmap, 30, 300), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 3);

  REQUIRE_EQ(delete_int_from_int(cbmap, 20), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 2);

  int val = -1;
  REQUIRE_EQ(get_int_from_int(cbmap, 20, &val), ccol_key_not_found);
  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 100);
  REQUIRE_EQ(get_int_from_int(cbmap, 30, &val), ccol_success);
  REQUIRE_EQ(val, 300);

  REQUIRE_EQ(delete_int_from_int(cbmap, 20), ccol_key_not_found);
  REQUIRE_EQ(delete_int_from_int(cbmap, 10), ccol_success);
  REQUIRE_EQ(delete_int_from_int(cbmap, 30), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 0);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, accessing_references) {
  cbinarymap *cbmap = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 100), ccol_success);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 200), ccol_success);

  int *val_ptr = NULL;
  REQUIRE_EQ(get_int_ref_from_int(cbmap, 10, &val_ptr), 0);
  REQUIRE_EQ(*val_ptr, 100);

  // Modify through the reference
  *val_ptr = 150;

  int val = -1;
  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 150);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, reset) {
  cbinarymap *cbmap = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbmap, NULL);

  for (int i = 0; i < 100; ++i) {
    REQUIRE_EQ(insert_int_to_int(cbmap, i, i * 10), ccol_success);
  }

  REQUIRE_EQ(cbmap_elem_count(cbmap), 100);

  cbmap_reset(cbmap);

  REQUIRE_EQ(cbmap_elem_count(cbmap), 0);

  int val = -1;
  for (int i = 0; i < 100; ++i) {
    REQUIRE_EQ(get_int_from_int(cbmap, i, &val), ccol_key_not_found);
  }

  // The map is usable after a reset
  REQUIRE_EQ(insert_int_to_int(cbmap, 42, 420), ccol_success);
  REQUIRE_EQ(get_int_from_int(cbmap, 42, &val), ccol_success);
  REQUIRE_EQ(val, 420);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, in_order_iteration) {
  // The BST iterates in sorted order
  cbmap_construct(bmap, int, int);

  // Insert in random order
  int keys[] = {50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45};
  int num_keys = sizeof(keys) / sizeof(keys[0]);

  for (int i = 0; i < num_keys; ++i) {
    int val = keys[i] * 10;
    cbmap_insert(bmap, keys[i], val);
  }

  REQUIRE_EQ(cbmap_elem_count(bmap), (size_t)num_keys);

  // Iterate and verify in-order traversal
  int prev_key = -1;
  int count = 0;
  ccol_iter_declare(bmap, it);
  for (it = ccol_begin(bmap); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    int val = *ccol_iter_val_ptr(it);

    // Verify sorted order
    REQUIRE_TRUE(key > prev_key);
    REQUIRE_EQ(val, key * 10);

    prev_key = key;
    ++count;
  }

  REQUIRE_EQ(count, num_keys);

  cbmap_destroy(bmap);
}

TEST(cbst_maps, avl_balancing_worst_case_insertions) {
  // Insert in sorted order, the worst case for an unbalanced BST, which the
  // AVL tree should handle without trouble
  cbmap_construct(bmap, int, int);

  // Insert 100 elements in ascending order
  for (int i = 0; i < 100; ++i) {
    int val = i * 10;
    cbmap_insert(bmap, i, val);
  }

  REQUIRE_EQ(cbmap_elem_count(bmap), 100);

  // Verify all elements are present and correct
  for (int i = 0; i < 100; ++i) {
    int val = cbmap_get(bmap, i);
    REQUIRE_EQ(val, i * 10);
  }

  // Verify in-order iteration
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
  // Insert in reverse sorted order (the other worst case)
  cbmap_construct(bmap, int, int);

  // Insert 100 elements in descending order
  for (int i = 99; i >= 0; --i) {
    int val = i * 10;
    cbmap_insert(bmap, i, val);
  }

  REQUIRE_EQ(cbmap_elem_count(bmap), 100);

  // Verify all elements are present and correct
  for (int i = 0; i < 100; ++i) {
    int val = cbmap_get(bmap, i);
    REQUIRE_EQ(val, i * 10);
  }

  // Check the in-order iteration (expect it ascending)
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

  // Insert many elements
  for (int i = 0; i < 50; ++i) {
    int val = i * 10;
    cbmap_insert(bmap, i, val);
  }

  // Delete every other element
  for (int i = 0; i < 50; i += 2) {
    REQUIRE_EQ(cbmap_remove(bmap, i), ccol_success);
  }

  REQUIRE_EQ(cbmap_elem_count(bmap), 25);

  // Verify remaining elements
  for (int i = 1; i < 50; i += 2) {
    int val = cbmap_get(bmap, i);
    REQUIRE_EQ(val, i * 10);
  }

  // Verify deleted elements are gone
  for (int i = 0; i < 50; i += 2) {
    REQUIRE_EQ(cbmap_remove(bmap, i), ccol_key_not_found);
  }

  // The in-order iteration should keep working
  int count = 0;
  ccol_iter_declare(bmap, it);
  for (it = ccol_begin(bmap); it != NULL; it = ccol_iter_next(it)) {
    int key = *ccol_iter_key_ptr(it);
    REQUIRE_TRUE(key % 2 == 1);  // only odd numbers are left
    ++count;
  }
  REQUIRE_EQ(count, 25);

  cbmap_destroy(bmap);
}

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
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
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
    ccol_comparison_proc_t comp = &custom_int_comparator;
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
    ccol_comparison_proc_t comp = &custom_int_comparator;
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
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
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
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
    ccol_comparison_proc_t comp = &custom_int_comparator;
    cbmap_construct_cc(bm, int, int, comp);

    int key = 3;
    int val = 30;

    cbmap_insert(bm, key, val);
    REQUIRE_EQ(cbmap_get(bm, key), 30);

    cbmap_remove(bm, key);

    cbmap_destroy(bm);
  }

  {
    ccol_comparison_proc_t comp = &custom_int_comparator;
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
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

  int records[6] = {0};  // 0 to 5, so 3, 4 and 5 are valid indices
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

  REQUIRE_EQ(cbmap_elem_count(bm), 100);

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

  REQUIRE_EQ(cbmap_elem_count(bm), 100);

  // The in-order iteration includes the negative numbers, in order
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

  // Derived from LONG_MAX instead of fixed 10-digit literals, because `long`
  // is only 4 bytes on an ILP32 target (i386, armhf) and a literal like
  // 4000000000L overflows it there. Capping the largest key at LONG_MAX / 2
  // also keeps this test's own `keys[i] * 2` below from overflowing on any
  // platform.
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
  REQUIRE_EQ(cbmap_elem_count(bm), 1);
  REQUIRE_EQ(cbmap_get(bm, key), 100);

  // Insert the same key with a different value
  val = 200;
  cbmap_insert(bm, key, val);
  REQUIRE_EQ(cbmap_elem_count(bm), 1);  // the count must not grow
  REQUIRE_EQ(cbmap_get(bm, key), 200);  // and the value must be updated

  cbmap_destroy(bm);
}

TEST(cbst_maps, empty_tree_iteration) {
  cbmap_construct(bm, int, int);

  // Iterate over empty tree
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

  // Insert elements
  for (int i = 0; i < size; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)size);

  // Verify all elements
  for (int i = 0; i < size; ++i) {
    int val = cbmap_get(bm, i);
    REQUIRE_EQ(val, i * 10);
  }

  // Verify in-order iteration
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

  // Insert elements
  for (int i = 0; i < size; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)size);

  // Delete all elements
  for (int i = 0; i < size; ++i) {
    REQUIRE_EQ(cbmap_remove(bm, i), ccol_success);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), 0);

  // Verify all are gone
  for (int i = 0; i < size; ++i) {
    REQUIRE_EQ(cbmap_remove(bm, i), ccol_key_not_found);
  }

  cbmap_destroy(bm);
}

TEST(cbst_maps, alternating_insert_delete) {
  cbmap_construct(bm, int, int);

  for (int round = 0; round < 10; ++round) {
    // Insert 100 elements
    for (int i = 0; i < 100; ++i) {
      int val = i * round;
      cbmap_insert(bm, i, val);
    }

    REQUIRE_EQ(cbmap_elem_count(bm), 100);

    // Delete 50 elements
    for (int i = 0; i < 50; ++i) {
      cbmap_remove(bm, i);
    }

    REQUIRE_EQ(cbmap_elem_count(bm), 50);

    // Delete remaining 50
    for (int i = 50; i < 100; ++i) {
      cbmap_remove(bm, i);
    }

    REQUIRE_EQ(cbmap_elem_count(bm), 0);
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
    // Check that enabling the local macros again works
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

  // Strings of varied lengths, so size != 4 or 8 (that avoids the
  // endian-sensitive path in compare_keys for equal-length keys whose first
  // characters differ)
  char *k_alpha = "alpha";   // 6 bytes with null
  char *k_beta = "beta";     // 5 bytes with null
  char *k_gamma = "gamma";   // 6 bytes with null
  char *k_missing = "zeta";  // 5 bytes with null, never inserted

  int v1 = 100, v2 = 200, v3 = 300;
  cbmap_insert(bm, k_alpha, v1);
  cbmap_insert(bm, k_beta, v2);
  cbmap_insert(bm, k_gamma, v3);

  REQUIRE_EQ(cbmap_get(bm, k_alpha), v1);
  REQUIRE_EQ(cbmap_get(bm, k_beta), v2);
  REQUIRE_EQ(cbmap_get(bm, k_gamma), v3);

  // Update existing key
  int v_updated = 999;
  cbmap_insert(bm, k_alpha, v_updated);
  REQUIRE_EQ(cbmap_get(bm, k_alpha), v_updated);

  // Remove a key and check that it's gone
  REQUIRE_EQ(cbmap_remove(bm, k_beta), ccol_success);
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, k_beta), NULL);
  REQUIRE_EQ(cbmap_remove(bm, k_beta), ccol_key_not_found);

  // A key we never inserted must not be found
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, k_missing), NULL);

  cbmap_destroy(bm);
}

// Helper: collects the iterated string keys into an array, returns the count.
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
  // A string of 3 characters takes exactly 4 bytes with the null terminator.
  // compare_keys must not fall through to cmp_unsigned_small, which compares
  // a key of size 4 as a uint32_t, because on a little-endian machine that
  // compares the bytes from the highest address down to the lowest, the
  // reverse of the character order, and the lexicographic order comes out
  // wrong. We insert strings in an order whose sorted sequence under an
  // integer comparison differs from the right lexicographic one, then assert
  // that an in-order iteration gives the right order.
  //
  // Correct lexicographic order: "abc" < "acb" < "bac" < "bca" < "cab"
  //
  // uint32_t order (little-endian): "bca" < "cab" < "acb" < "bac" <
  // "abc" (sorted by 0x00616362, 0x00626163, 0x00626361, 0x00636162,
  // 0x00636261)
  cbmap_construct(bm, char *, int);

  char *keys[] = {"bca", "abc", "cab", "acb", "bac"};
  for (int i = 0; i < 5; ++i) {
    int val = i + 1;
    cbmap_insert(bm, keys[i], val);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), 5);

  const char *expected[] = {"abc", "acb", "bac", "bca", "cab"};
  const char *got[5];
  int n = collect_string_keys(bm, got, 5);

  REQUIRE_EQ(n, 5);
  for (int i = 0; i < 5; ++i) {
    REQUIRE_STREQ(got[i], expected[i]);
  }

  // Lookups work whatever the ordering.
  REQUIRE_NE((void *)cbmap_get_ptr(bm, "abc"), NULL);
  REQUIRE_NE((void *)cbmap_get_ptr(bm, "bca"), NULL);
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, "xyz"), NULL);

  cbmap_destroy(bm);
}

TEST(cbst_maps, string_keys_7_char_iteration_order) {
  // A string of 7 characters takes exactly 8 bytes with the null terminator.
  // compare_keys must not compare a key of size 8 as a uint64_t, because on a
  // little-endian machine that reverses the character order and char[6]
  // becomes the most significant byte. Two strings that differ only at
  // char[0] then get ordered by their LAST character instead of their FIRST,
  // which can turn the right order around.
  //
  // Correct lexicographic order:
  //   "abcdefg" < "abcdefh" < "abcdegh" < "bacdefg" < "gfedcba"
  //
  // uint64_t order (little-endian):
  //   "gfedcba" < "bacdefg" < "abcdefg" < "abcdefh" < "abcdegh"
  // (sorted by 0x0061..., 0x0067...62, 0x0067...61, 0x0068...61, 0x0068...61)
  cbmap_construct(bm, char *, int);

  char *keys[] = {"bacdefg", "abcdefg", "gfedcba", "abcdegh", "abcdefh"};
  for (int i = 0; i < 5; ++i) {
    int val = i + 1;
    cbmap_insert(bm, keys[i], val);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), 5);

  const char *expected[] = {"abcdefg", "abcdefh", "abcdegh", "bacdefg",
                            "gfedcba"};
  const char *got[5];
  int n = collect_string_keys(bm, got, 5);

  REQUIRE_EQ(n, 5);
  for (int i = 0; i < 5; ++i) {
    REQUIRE_STREQ(got[i], expected[i]);
  }

  // Lookups have to work too.
  REQUIRE_NE((void *)cbmap_get_ptr(bm, "abcdefg"), NULL);
  REQUIRE_NE((void *)cbmap_get_ptr(bm, "gfedcba"), NULL);
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, "zzzzzzz"), NULL);

  cbmap_destroy(bm);
}

TEST(cbst_maps, char_type_variants_as_string_keys) {
  // signed char * (= int8_t *) must be treated as a string key, with the
  // right lexicographic order in an in-order iteration.
  {
    cbmap_construct(bm, signed char *, int);

    signed char *k_banana = (signed char *)"banana";
    signed char *k_apple = (signed char *)"apple";
    signed char *k_cherry = (signed char *)"cherry";
    int v1 = 1, v2 = 2, v3 = 3;

    cbmap_insert(bm, k_banana, v1);
    cbmap_insert(bm, k_apple, v2);
    cbmap_insert(bm, k_cherry, v3);

    REQUIRE_EQ(cbmap_elem_count(bm), 3);
    REQUIRE_EQ(cbmap_get(bm, k_banana), 1);
    REQUIRE_EQ(cbmap_get(bm, k_apple), 2);
    REQUIRE_EQ(cbmap_get(bm, k_cherry), 3);

    // The in-order iteration gives lexicographic order.
    const char *expected_sc[] = {"apple", "banana", "cherry"};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_STREQ((const char *)*ccol_iter_key_ptr(it), expected_sc[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 3);

    // A char * with the same string content finds the same entries.
    char *ck = "banana";
    REQUIRE_EQ(cbmap_get(bm, ck), 1);

    cbmap_destroy(bm);
  }

  // unsigned char * (= uint8_t *), the same check.
  {
    cbmap_construct(bm, unsigned char *, int);

    unsigned char *k_mango = (unsigned char *)"mango";
    unsigned char *k_kiwi = (unsigned char *)"kiwi";
    unsigned char *k_lime = (unsigned char *)"lime";
    int v10 = 10, v20 = 20, v30 = 30;

    cbmap_insert(bm, k_mango, v10);
    cbmap_insert(bm, k_kiwi, v20);
    cbmap_insert(bm, k_lime, v30);

    REQUIRE_EQ(cbmap_elem_count(bm), 3);
    REQUIRE_EQ(cbmap_get(bm, k_mango), 10);
    REQUIRE_EQ(cbmap_get(bm, k_kiwi), 20);
    REQUIRE_EQ(cbmap_get(bm, k_lime), 30);

    // In-order: kiwi < lime < mango.
    const char *expected_uc[] = {"kiwi", "lime", "mango"};
    int idx = 0;
    ccol_iter_declare(bm, it);
    for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
      REQUIRE_STREQ((const char *)*ccol_iter_key_ptr(it), expected_uc[idx]);
      ++idx;
    }
    REQUIRE_EQ(idx, 3);

    // A signed char * with the same content should find the entries too.
    signed char *sk = (signed char *)"kiwi";
    REQUIRE_EQ(cbmap_get(bm, sk), 20);

    cbmap_destroy(bm);
  }

  // A char * map, looked up with signed char * and unsigned char *.
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
  // A default comparator (one the caller didn't supply) must order a
  // floating-point key by its real numeric value and not by reading the
  // key's raw bit pattern as an unsigned integer. A negative float, double
  // or long double has its sign bit set, which is a large value as an
  // unsigned integer, but such a key still sorts before every non-negative
  // key of the same map.
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

    // -0.0 and 0.0 compare equal, not as two distinct keys.
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
  // The default comparator for float, double and long double can't rely on
  // the native `<` and `>` alone, because those return false for ANY
  // comparison with a NaN operand, against ANY other key and not only another
  // NaN. Every descent for an insert, a get or a delete starts by comparing
  // against the root of the tree, so a comparator with no guard makes a NaN
  // key silently "equal" to whatever key sits at the root: inserting a NaN
  // key never creates a new node but overwrites the ROOT's value and reports
  // ccol_key_already_present for a key that isn't there. cmp_float_small
  // gives a NaN a well-defined place instead. A NaN is greater than every key
  // that isn't a NaN and equal only to another NaN, so a NaN key is a real
  // entry of its
  // own.
  cbmap_construct(bm, double, int);

  double k1 = 5.0, k2 = 9.0;
  int v1 = 111, v2 = 222;
  cbmap_insert(bm, k1, v1);
  cbmap_insert(bm, k2, v2);
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)2);

  double nan_key = NAN;
  int nan_val = 999;
  ccol_retval_t r = cbmap_insert_elem(
      bm, &(cmap_pair){.ptr = &nan_key, .size = sizeof(nan_key)},
      &(cmap_pair){.ptr = &nan_val, .size = sizeof(nan_val)});
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)3);

  // The keys that were there before must not be affected by the NaN insert.
  REQUIRE_EQ(cbmap_get(bm, k1), 111);
  REQUIRE_EQ(cbmap_get(bm, k2), 222);

  // The NaN key itself can be read back on its own.
  int readback = 0;
  REQUIRE_EQ(cbmap_get_elem_copy(
                 bm, &(cmap_pair){.ptr = &nan_key, .size = sizeof(nan_key)},
                 &readback, sizeof(readback)),
             ccol_success);
  REQUIRE_EQ(readback, 999);

  // A second, different NaN bit pattern compares equal to the first (an
  // explicit, documented choice), so it updates that entry in place instead
  // of adding a fourth one.
  double other_nan = -NAN;
  int other_nan_val = 1000;
  r = cbmap_insert_elem(
      bm, &(cmap_pair){.ptr = &other_nan, .size = sizeof(other_nan)},
      &(cmap_pair){.ptr = &other_nan_val, .size = sizeof(other_nan_val)});
  REQUIRE_EQ(r, ccol_key_already_present);
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)3);

  // The NaN entry can be deleted like any other key, without touching the
  // rest of the map.
  REQUIRE_EQ(cbmap_delete_elem(
                 bm, &(cmap_pair){.ptr = &nan_key, .size = sizeof(nan_key)}),
             ccol_success);
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)2);
  REQUIRE_EQ(cbmap_get(bm, k1), 111);
  REQUIRE_EQ(cbmap_get(bm, k2), 222);

  cbmap_destroy(bm);
}

TEST(cbst_maps, char_keys_use_native_char_comparison) {
  // The default comparator for a `char` key should match this platform's
  // native `char` order, whatever the signedness of `char` is here: it's
  // signed on x86 and x86_64 but unsigned under the standard aarch64 AAPCS64
  // ABI, so the comparator must not force a signed int8_t reading everywhere.
  // We check an in-order iteration against a reference order computed with
  // the native `char` `<`, so the expectation itself is right on whatever
  // platform runs
  // it.
  cbmap_construct(bm, char, int);

  // The values below come in two groups. The first one has the high bit set,
  // so a forced signed reading makes each of them negative, and the second
  // one holds small positive values, so a signed/unsigned mismatch reorders
  // them visibly.
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
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)n);

  // The reference order comes from a plain insertion sort with the native
  // `char` `<`.
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
  // A scalar `signed char` key, or one of its typedef `int8_t`, is a C type
  // of its own, separate from plain `char`, and it needs a real signed
  // comparison that doesn't depend on the platform, like short, int, long
  // and long long do, NOT the native `char` comparison ccol_char uses. So
  // ccol_determine_ccol_data_type() must keep `signed char` in its own
  // ccol_signed_char bucket instead of folding it into ccol_char together
  // with plain `char`. On some platforms a plain `char` is unsigned by default
  // (the standard aarch64 AAPCS64 ABI is one), and there a folded negative
  // signed char or int8_t key would sort as a large positive value although
  // it belongs before every non-negative key. The expected order below is
  // fixed, the true two's-complement signed order, while the expectation of
  // char_keys_use_native_char_comparison depends on the platform. The
  // contract of ccol_signed_char is NOT to depend on the signedness of a
  // native `char`.
  cbmap_construct(bm, signed char, int);

  signed char keys[] = {(signed char)-128, (signed char)-5,  0,
                        (signed char)3,    (signed char)100, (signed char)127};
  const int n = (int)(sizeof(keys) / sizeof(keys[0]));

  // Insert in shuffled order so the tree structure doesn't just happen to
  // match the expected in-order sequence already.
  int insert_order[] = {3, 0, 5, 1, 4, 2};
  for (int i = 0; i < n; ++i) {
    int idx = insert_order[i];
    int val = idx;
    cbmap_insert(bm, keys[idx], val);
  }
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)n);

  // We cast to `int` for REQUIRE_EQ because tau's printer in tests/tau/tau.h
  // has a `_Generic` case for `char` but none for `signed char`. This key
  // type is a different C type from `char` on purpose, and that difference
  // is exactly what this test pins.
  int idx = 0;
  ccol_iter_declare(bm, it);
  for (it = ccol_begin(bm); it != NULL; it = ccol_iter_next(it)) {
    REQUIRE_EQ((int)*ccol_iter_key_ptr(it), (int)keys[idx]);
    REQUIRE_EQ(*ccol_iter_val_ptr(it), idx);
    ++idx;
  }
  REQUIRE_EQ(idx, n);

  // Lookups for negative and non-negative keys both work.
  REQUIRE_EQ(cbmap_get(bm, keys[0]), 0);  // -128
  REQUIRE_EQ(cbmap_get(bm, keys[1]), 1);  // -5
  REQUIRE_EQ(cbmap_get(bm, keys[5]), 5);  // 127

  cbmap_destroy(bm);
}

TEST(cbst_maps, int8_t_keys_sort_by_genuine_signed_value) {
  // int8_t is a typedef for signed char on every mainstream platform the
  // library targets, and the same contract holds through that spelling.
  cbmap_construct(bm, int8_t, int);

  int8_t keys[] = {-100, -1, 0, 1, 100};
  const int n = (int)(sizeof(keys) / sizeof(keys[0]));

  for (int i = n - 1; i >= 0; --i) {
    int val = i;
    cbmap_insert(bm, keys[i], val);
  }
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)n);

  // Cast to `int`, for the same tau printer reason as above.
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
  // A signed char* key must be recognized as a string (ccol_string), just
  // like a char* key and an unsigned char* key, and that is expected for
  // string sizes of 4 and 8 bytes (null terminator included) as well. Those
  // two sizes would otherwise land on the typed uint32_t and uint64_t
  // reading of cmp_unsigned_small, which silently orders such keys by their
  // raw byte pattern and not lexicographically.
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
  // A struct key type is ccol_other_types, and it is always ordered by
  // a raw memcmp of its representation, even when the struct happens to be
  // the size of a real integer, 1, 2, 4 or 8 bytes. Otherwise such a size
  // sends the key through a typed integer reading, which depends on the
  // machine's endianness.
  typedef struct {
    short a;
    short b;
  } pair_t;  // 4 bytes, the size of a uint32_t

  cbmap_construct(bm, pair_t, int);

  pair_t keys[] = {{1, 0}, {0, 1}, {2, 0}, {0, 2}};
  for (int i = 0; i < 4; ++i) {
    int val = i + 1;
    cbmap_insert(bm, keys[i], val);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), 4);

  // The expected order is whatever a plain memcmp of the 4-byte
  // representation gives, not a numeric reading of the two short fields.
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
    // bm gets destroyed automatically at the end of the block
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
    // bm gets destroyed automatically at the end of the block
  }
}

TEST(cbst_maps, get_elem_copy_size_mismatch) {
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 42;
  int val = 100;
  REQUIRE_EQ(
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  // The buffer size is wrong, since the map stores a sizeof(int) and the
  // call asks for a sizeof(double). We avoid `long` on purpose, because on an
  // ILP32 platform such as i386 sizeof(long) == sizeof(int) (both 4 bytes),
  // so that pair isn't a real size mismatch there. A `double` is 8 bytes on
  // every mainstream platform this library targets, LP64 and ILP32
  // alike.
  double wrong_buf = 0;
  REQUIRE_EQ(
      cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &wrong_buf, sizeof(wrong_buf)),
      ccol_invalid_args);

  // The right buffer size works
  int right_buf = 0;
  REQUIRE_EQ(
      cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &right_buf, sizeof(right_buf)),
      ccol_success);
  REQUIRE_EQ(right_buf, 100);

  cbmap_destroy(cbm);
}

/* A NULL target buffer whose non-zero size matches the stored value is
 * refused, the same way chmap_get_elem_copy refuses it. A NULL buffer of
 * size 0 can read a stored value of size 0, which
 * update_value_to_zero_size_does_not_corrupt pins. Drop the check and the
 * copy writes through NULL, so the process dies with SIGSEGV. */
TEST(cbst_maps, get_elem_copy_refuses_a_null_buffer) {
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);
  int key = 7, val = 70;
  ccol_retval_t ins =
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)});
  ccol_retval_t got = cbmap_get_elem_copy(
      cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, NULL, sizeof(int));
  cbmap_destroy(cbm);
  REQUIRE_EQ(ins, ccol_success);
  REQUIRE_EQ(got, ccol_invalid_args);
}

TEST(cbst_maps, get_elem_ref_missing_key) {
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 1;
  int val = 10;
  REQUIRE_EQ(
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  int missing = 999;
  cmap_pair out = {NULL, 0};
  REQUIRE_EQ(
      cbmap_get_elem_ref(
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

  // Present keys
  REQUIRE_NE((void *)cbmap_get_ptr(bm, k1), NULL);
  REQUIRE_EQ(*cbmap_get_ptr(bm, k1), 10);

  // A missing key gives NULL
  REQUIRE_EQ((void *)cbmap_get_ptr(bm, k_missing), NULL);

  cbmap_destroy(bm);
}

TEST(cbst_maps, early_iterator_destroy) {
  cbmap_construct(bm, int, int);

  for (int i = 0; i < 20; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  // Stop the iteration after the first 5 elements and check nothing leaks
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

  // The map has to be fully usable after the aborted iteration
  REQUIRE_EQ(cbmap_elem_count(bm), 20);
  for (int i = 0; i < 20; ++i) {
    int val = cbmap_get(bm, i);
    REQUIRE_EQ(val, i * 10);
  }

  // A fresh full iteration visits every node
  count = 0;
  ccol_iter_declare(bm, it2);
  for (it2 = ccol_begin(bm); it2 != NULL; it2 = ccol_iter_next(it2)) {
    ++count;
  }
  REQUIRE_EQ(count, 20);

  cbmap_destroy(bm);
}

TEST(cbst_maps, update_value_with_different_size) {
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  // Insert with int value (4 bytes)
  int key = 7;
  int small_val = 42;
  REQUIRE_EQ(cbmap_insert_elem(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                 &(cmap_pair){.ptr = &small_val, .size = sizeof(small_val)}),
             ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  int readback = 0;
  REQUIRE_EQ(
      cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &readback, sizeof(readback)),
      ccol_success);
  REQUIRE_EQ(readback, 42);

  // Insert the same key again with an 8-byte value, which takes the realloc
  // path. It's an int64_t on purpose and a long would be wrong: a long is
  // only 4 bytes on an ILP32 platform such as i386, and that would silently
  // kill the whole point of this test, which is to resize a value from 4
  // bytes to 8.
  int64_t big_val = INT64_C(1234567890123);
  REQUIRE_EQ(
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &big_val, .size = sizeof(big_val)}),
      ccol_key_already_present);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  int64_t big_readback = 0;
  REQUIRE_EQ(
      cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &big_readback, sizeof(big_readback)),
      ccol_success);
  const int64_t expected_big_val = INT64_C(1234567890123);
  REQUIRE_EQ(big_readback, expected_big_val);

  // Insert it again back at int size, which reallocs in the other direction
  small_val = 99;
  REQUIRE_EQ(cbmap_insert_elem(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                 &(cmap_pair){.ptr = &small_val, .size = sizeof(small_val)}),
             ccol_key_already_present);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  readback = 0;
  REQUIRE_EQ(
      cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &readback, sizeof(readback)),
      ccol_success);
  REQUIRE_EQ(readback, 99);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, update_value_to_zero_size_does_not_corrupt) {
  // Shrinking an existing key's value to zero bytes must not call
  // realloc(ptr, 0) directly. glibc defines that call as freeing ptr and
  // returning NULL, and the return value alone can't tell that from a real
  // allocation failure. What follows is that the node's value pointer
  // dangles while the call reports ccol_not_enough_memory, later reads go to
  // the old value which is already freed, and destroying the map frees it a
  // second time.
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 7;
  int val = 42;
  REQUIRE_EQ(
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  char dummy = 0;
  ccol_retval_t r =
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &dummy, .size = 0});
  REQUIRE_EQ(r, ccol_key_already_present);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  cmap_pair vp = {(void *)0x1, 12345}; /* a poison value */
  REQUIRE_EQ(cbmap_get_elem_ref(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, &vp),
             ccol_success);
  REQUIRE_EQ(vp.size, (size_t)0);
  REQUIRE_EQ((void *)vp.ptr, NULL);

  // Reading a zero-size buffer with cbmap_get_elem_copy succeeds
  // too.
  REQUIRE_EQ(cbmap_get_elem_copy(
                 cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, NULL, 0),
             ccol_success);

  // Grow the same key's value back to a real size, which exercises
  // realloc(NULL, n), defined to behave like malloc(n).
  int restored = 777;
  r = cbmap_insert_elem(
      cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
      &(cmap_pair){.ptr = &restored, .size = sizeof(restored)});
  REQUIRE_EQ(r, ccol_key_already_present);

  int readback = 0;
  REQUIRE_EQ(
      cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &readback, sizeof(readback)),
      ccol_success);
  REQUIRE_EQ(readback, 777);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, insert_and_retrieve_zero_size_value_for_new_key) {
  // Inserting a new key with a zero-size value must succeed without ever
  // calling the allocator with a size of zero, because the C standard lets
  // malloc(0) return either NULL or a unique pointer even when it
  // succeeds.
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key1 = 1;
  int key2 = 2;
  char dummy = 0;

  REQUIRE_EQ(
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key1, .size = sizeof(key1)},
                        &(cmap_pair){.ptr = &dummy, .size = 0}),
      ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  int val2 = 55;
  REQUIRE_EQ(
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &key2, .size = sizeof(key2)},
                        &(cmap_pair){.ptr = &val2, .size = sizeof(val2)}),
      ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbm), 2);

  cmap_pair vp1 = {(void *)0x1, 12345}; /* a poison value */
  REQUIRE_EQ(cbmap_get_elem_ref(
                 cbm, &(cmap_pair){.ptr = &key1, .size = sizeof(key1)}, &vp1),
             ccol_success);
  REQUIRE_EQ(vp1.size, (size_t)0);
  REQUIRE_EQ((void *)vp1.ptr, NULL);

  int readback2 = 0;
  REQUIRE_EQ(
      cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &key2, .size = sizeof(key2)},
                          &readback2, sizeof(readback2)),
      ccol_success);
  REQUIRE_EQ(readback2, 55);

  // Deleting the key with the zero-size value works like for any other key.
  REQUIRE_EQ(
      cbmap_delete_elem(cbm, &(cmap_pair){.ptr = &key1, .size = sizeof(key1)}),
      ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  cbmap_destroy(cbm);
}

TEST(cbst_maps, wrong_size_key_rejected_for_fixed_width_key_type) {
  // A fixed-width declared key type has one exact size, and every raw-layer
  // entry point that takes a key_pair rejects a key of any other size
  // with ccol_invalid_args, instead of storing it as a separate key that no
  // correctly typed lookup could ever reach.
  //
  // The oversized key's size comes from sizeof(int) itself, never from some
  // other type that just happens to be wider on this machine: on an ILP32
  // ABI such as armhf and i386 a long is as wide as an int, so a long key
  // there is a perfectly valid int-sized key and proves nothing.
  //
  // Each call stores its result in a local and the assertions only run after
  // the map is destroyed, for two reasons. Tau evaluates the expression it
  // gets in REQUIRE_EQ a second time to print it when the assertion fails,
  // so an inline call that changes the map runs twice and the report shows
  // the second run's value. And a failing REQUIRE_* returns at once, which
  // skips any cleanup below it.
  //
  // Without the size check this fails: insert_r is
  // ccol_success, count_after_reject is 1, and the three lookups report
  // ccol_key_not_found instead of ccol_invalid_args.
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  unsigned char too_wide[sizeof(int) + 1] = {0};
  int val = 42;

  const ccol_retval_t insert_r = cbmap_insert_elem(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)},
      &(cmap_pair){.ptr = &val, .size = sizeof(val)});
  const size_t count_after_reject = cbmap_elem_count(cbm);

  int readback = -1;
  const ccol_retval_t copy_r = cbmap_get_elem_copy(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)}, &readback,
      sizeof(readback));

  cmap_pair val_pair = {NULL, 0};
  const ccol_retval_t ref_r = cbmap_get_elem_ref(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)}, &val_pair);

  const ccol_retval_t delete_r = cbmap_delete_elem(
      cbm, &(cmap_pair){.ptr = too_wide, .size = sizeof(too_wide)});

  // A correctly sized key of the same declared type keeps working
  // everywhere, so the check only rejects the mismatch.
  int good_key = 5;
  const ccol_retval_t good_insert_r = cbmap_insert_elem(
      cbm, &(cmap_pair){.ptr = &good_key, .size = sizeof(good_key)},
      &(cmap_pair){.ptr = &val, .size = sizeof(val)});
  const size_t count_after_good = cbmap_elem_count(cbm);
  const ccol_retval_t good_copy_r = cbmap_get_elem_copy(
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
  // Every key type that ccol_fixed_width_data_type_size() gives a width for
  // enforces it, and a key one byte short is rejected. We cover the float,
  // long double and pointer types as well as the integers, since they reach
  // different branches of compare_keys().
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

    cbmap cbm = cbmap_create(type, NULL);
    REQUIRE_NE((void *)cbm, NULL);

    // A buffer big enough for the widest key type, so the deliberately
    // undersized key_pair below never points at less memory than it claims.
    unsigned char key_buf[sizeof(long double) + sizeof(uintptr_t)] = {0};
    int val = 7;

    const ccol_retval_t short_r =
        cbmap_insert_elem(cbm, &(cmap_pair){.ptr = key_buf, .size = width - 1},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const size_t count_after_short = cbmap_elem_count(cbm);

    const ccol_retval_t exact_r =
        cbmap_insert_elem(cbm, &(cmap_pair){.ptr = key_buf, .size = width},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const size_t count_after_exact = cbmap_elem_count(cbm);

    cbmap_destroy(cbm);

    REQUIRE_EQ(short_r, ccol_invalid_args);
    REQUIRE_EQ(count_after_short, (size_t)0);
    REQUIRE_EQ(exact_r, ccol_success);
    REQUIRE_EQ(count_after_exact, (size_t)1);
  }
}

TEST(cbst_maps, variable_width_key_types_still_accept_any_key_size) {
  // ccol_string and ccol_other_types have no fixed width, so keys of
  // different sizes are an ordinary, supported case for them: the map orders
  // them by their common prefix and then by size, and never rejects them.
  // Otherwise every string key that isn't exactly as long as some other type
  // would start failing.
  const ccol_data_type variable_width_types[] = {ccol_string, ccol_other_types};

  for (size_t i = 0;
       i < sizeof(variable_width_types) / sizeof(variable_width_types[0]);
       i++) {
    REQUIRE_EQ(ccol_fixed_width_data_type_size(variable_width_types[i]),
               (size_t)0);

    cbmap cbm = cbmap_create(variable_width_types[i], NULL);
    REQUIRE_NE((void *)cbm, NULL);

    int val = 1;
    const ccol_retval_t short_r =
        cbmap_insert_elem(cbm, &(cmap_pair){.ptr = (void *)"ab", .size = 3},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const ccol_retval_t long_r = cbmap_insert_elem(
        cbm, &(cmap_pair){.ptr = (void *)"abcdefghij", .size = 11},
        &(cmap_pair){.ptr = &val, .size = sizeof(val)});
    const size_t count = cbmap_elem_count(cbm);

    cbmap_destroy(cbm);

    REQUIRE_EQ(short_r, ccol_success);
    REQUIRE_EQ(long_r, ccol_success);
    REQUIRE_EQ(count, (size_t)2);
  }
}

TEST(cbst_maps, zero_size_key_roundtrips) {
  // The same coverage for a zero-size KEY, because create_new_node's
  // key-side allocation has exactly the same malloc(0) hazard as the value
  // side.
  cbmap cbm = cbmap_create(ccol_other_types, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  char dummy = 0;
  int val1 = 111;
  REQUIRE_EQ(
      cbmap_insert_elem(cbm, &(cmap_pair){.ptr = &dummy, .size = 0},
                        &(cmap_pair){.ptr = &val1, .size = sizeof(val1)}),
      ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  int readback = 0;
  REQUIRE_EQ(cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &dummy, .size = 0},
                                 &readback, sizeof(readback)),
             ccol_success);
  REQUIRE_EQ(readback, 111);

  // A different, non-zero-size key must not collide with the zero-size key.
  int other_key = 5;
  int val2 = 222;
  REQUIRE_EQ(
      cbmap_insert_elem(
          cbm, &(cmap_pair){.ptr = &other_key, .size = sizeof(other_key)},
          &(cmap_pair){.ptr = &val2, .size = sizeof(val2)}),
      ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbm), 2);

  REQUIRE_EQ(cbmap_get_elem_copy(cbm, &(cmap_pair){.ptr = &dummy, .size = 0},
                                 &readback, sizeof(readback)),
             ccol_success);
  REQUIRE_EQ(readback, 111);

  REQUIRE_EQ(cbmap_delete_elem(cbm, &(cmap_pair){.ptr = &dummy, .size = 0}),
             ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbm), 1);

  cbmap_destroy(cbm);
}

// A string key always has a size of at least 1, because its size counts the
// terminating NUL. Every raw entry point refuses a size-0 key on a map with
// ccol_string keys, like chmap does, with or without a pointer, so no
// iterator over a string map can yield a NULL char * key. Take the size
// check away and the test catches it: the insert succeeds and the iterator
// hands back a key whose ptr is NULL.
TEST(cbst_maps, zero_size_string_key_is_refused_by_every_raw_entry_point) {
  cbmap cbm = cbmap_create(ccol_string, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  char dummy = 0;
  int val = 7;
  const cmap_pair val_pair = {.ptr = &val, .size = sizeof(val)};
  const cmap_pair null_key = {.ptr = NULL, .size = 0};
  const cmap_pair empty_key = {.ptr = &dummy, .size = 0};

  const ccol_retval_t insert_null =
      cbmap_insert_elem(cbm, &null_key, &val_pair);
  const ccol_retval_t insert_empty =
      cbmap_insert_elem(cbm, &empty_key, &val_pair);
  const size_t count_after_inserts = cbmap_elem_count(cbm);

  /* A real string key, so looking up the empty key has a tree to descend. */
  const ccol_retval_t insert_real = cbmap_insert_elem(
      cbm, &(cmap_pair){.ptr = (void *)"", .size = 1}, &val_pair);

  int readback = 0;
  const ccol_retval_t copy_r =
      cbmap_get_elem_copy(cbm, &null_key, &readback, sizeof(readback));
  cmap_pair ref = {NULL, 0};
  const ccol_retval_t ref_r = cbmap_get_elem_ref(cbm, &empty_key, &ref);
  const ccol_retval_t delete_r = cbmap_delete_elem(cbm, &null_key);

  bool saw_null_key = false;
  size_t iterated = 0;
  cbmap_redeclare(cbm, char *, int);
  ccol_iter_declare(cbm, it);
  for (it = cbmap_begin_iter(cbm, NULL); it; it = ccol_iter_next(it)) {
    iterated++;
    if (!it->key_pair->ptr) saw_null_key = true;
  }
  const size_t final_count = cbmap_elem_count(cbm);
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

// cbmap_begin_iter(NULL, ...) must behave like an empty map: return NULL,
// leave err alone and not assert. That's a deliberate exception to how every
// OTHER cbstmap accessor treats a NULL map, since cbmap_elem_count,
// cbmap_reset, cbmap_insert_elem, cbmap_get_elem_copy, cbmap_get_elem_ref
// and cbmap_delete_elem all call _ccol_assert(false) for one. Only
// begin_iter is exempt. It matches the same deliberate NULL tolerance of
// chashmap_begin_iter and keeps the two sibling map modules consistent for
// a map field that the caller creates lazily and that can be NULL for a
// while.
TEST(cbst_maps, begin_iter_null_map_returns_null_like_empty) {
  cbmap null_map = NULL;
  char *err = (char *)0x1; /* poison value, must be reset to NULL */
  REQUIRE_EQ((void *)cbmap_begin_iter(null_map, &err), NULL);
  REQUIRE_EQ((void *)err, NULL);

  // A NULL err has to be tolerated too (it's documented as optional).
  REQUIRE_EQ((void *)cbmap_begin_iter(null_map, NULL), NULL);
}

// ========================================================================
// INVARIANT TESTS (randomized operation sequences)
// ========================================================================

#define INVARIANTS_KEY_RANGE 200

// Random inserts and removes against an int->int map, with three
// independent invariants checked after every operation: the AVL balance and
// height invariant, which cbmap_debug_validate_avl checks inside cbstmap.c;
// an in-order iteration that strictly increases; and a shadow reference
// model, a plain array indexed by key, where every key the model holds has
// to give back its exact shadow value and every key it lacks should be
// really absent.
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
        REQUIRE_TRUE(k > prev_key);  // strictly increasing in-order traversal
      }
      prev_key = k;
      have_prev = true;
      REQUIRE_TRUE(shadow_present[k]);
      REQUIRE_EQ(*ccol_iter_val_ptr(it), shadow_value[k]);
      ++shadow_count;
    }
    REQUIRE_EQ(cbmap_elem_count(hm), shadow_count);

    for (int k = 0; k < INVARIANTS_KEY_RANGE; ++k) {
      if (!shadow_present[k]) {
        REQUIRE_EQ((void *)cbmap_get_ptr(hm, k), (void *)NULL);
      }
    }
  }

  cbmap_destroy(hm);
}

/* A value's bytes start inside the node's own allocation and move out to a
 * buffer of their own the first time an update changes their size. This
 * test drives every crossing of that boundary, in both directions. Each
 * crossing decides where a later free points, at the node block or at a
 * buffer of its own, and a mistake either way is a free of an interior
 * pointer or a leak, which this suite's memtest run catches on top of the
 * values the test compares. The zero-size representation is one more
 * crossing, and update_value_to_zero_size_does_not_corrupt owns that one. A
 * char * value is always at least one byte long, so nothing here reaches
 * it. */
/* cbmap_get_elem_ref fills a pair whose ptr points into the node's value,
 * and callers often hand that pointer straight back to resize the value.
 * The resize allocates new storage, frees the old one and copies, so a copy
 * that reads its source after that free reads the buffer it has just
 * freed.
 *
 * Move the copy after the free and AddressSanitizer reports a
 * heap-use-after-free inside the update, and the node ends up
 * with whatever the allocator left behind. */
TEST(cbstmap_value_storage, resizing_a_value_from_its_own_buffer_is_safe) {
  cbmap_construct_scoped(bm, int, char *);
  int key = 3;

  cbmap_insert(bm, key, "first value, short enough to sit inside the node");
  /* A different size moves it out to its own buffer, which is the state
     where the release below has something to release. */
  cbmap_insert(bm, key, "second value, a different length, now external");

  cmap_pair kp = {&key, sizeof(key)};
  cmap_pair held = {NULL, 0};
  ccol_retval_t got = cbmap_get_elem_ref(bm, &kp, &held);

  /* Resize with the map's own buffer as the source. */
  bool shrank = false;
  if (got == ccol_success && held.ptr) {
    cmap_pair selfref = {held.ptr, 14};
    shrank = (cbmap_insert_elem(bm, &kp, &selfref) == ccol_key_already_present);
  }

  cmap_pair after = {NULL, 0};
  ccol_retval_t reread = cbmap_get_elem_ref(bm, &kp, &after);
  bool intact = (reread == ccol_success && after.ptr && after.size == 14 &&
                 memcmp(after.ptr, "second value,", 13) == 0);

  REQUIRE_EQ(got, ccol_success);
  REQUIRE_TRUE(shrank);
  REQUIRE_TRUE(intact);
}

/* We read through cbmap_get_ptr and not cbmap_get everywhere. A regression in
   the resize path loses the key outright, and cbmap_get answers a missing key
   with _ccol_fatal_err, which aborts the whole binary along with every other
   suite's result in the same run; cbmap_get_ptr answers NULL, so the same
   regression fails only this test and reports what it found. */
#define REQUIRE_STRING_VALUE_IS(map_, key_, expected_) \
  do {                                                 \
    char *const *found_ = cbmap_get_ptr(map_, key_);   \
    REQUIRE_NE((void *)found_, NULL);                  \
    if (found_) REQUIRE_STREQ(*found_, (expected_));   \
  } while (0)

TEST(cbstmap_value_storage, resizing_a_value_across_the_inline_boundary) {
  cbmap_construct_scoped(bm, int, char *);

  int key = 7;
  cbmap_insert(bm, key, "short");
  REQUIRE_STRING_VALUE_IS(bm, key, "short");

  /* Longer than the room reserved when the node was built, so it moves out. */
  cbmap_insert(bm, key, "a considerably longer value than the first one");
  REQUIRE_STRING_VALUE_IS(bm, key,
                          "a considerably longer value than the first one");

  /* Shorter again: it stays in its own buffer and doesn't go back inline. */
  cbmap_insert(bm, key, "tiny");
  REQUIRE_STRING_VALUE_IS(bm, key, "tiny");

  /* The same size twice in a row is written in place, wherever it lives. */
  cbmap_insert(bm, key, "tin2");
  REQUIRE_STRING_VALUE_IS(bm, key, "tin2");

  /* Back up to a longer one, then a second node to show that none of this
     disturbed the first node's own storage. */
  cbmap_insert(bm, key, "grown back out to something long again");
  int other = 8;
  cbmap_insert(bm, other, "other");
  REQUIRE_STRING_VALUE_IS(bm, key, "grown back out to something long again");
  REQUIRE_STRING_VALUE_IS(bm, other, "other");

  /* Removing the resized node frees exactly one buffer of its own plus
     the node block, and leave the untouched node readable. */
  cbmap_remove(bm, key);
  REQUIRE_STRING_VALUE_IS(bm, other, "other");
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)1);
}

#undef REQUIRE_STRING_VALUE_IS

/* A node's key bytes sit inside the node's allocation, at an offset rounded
 * to what that key type really needs and not to the strongest alignment any
 * type could need. This test stores and reads back every declared key type
 * through the macro layer, which casts the stored pointer to the caller's
 * type and dereferences it, so an offset that under-aligns any of those
 * types is a real misaligned access on this path, not just a latent one.
 *
 * On x86-64 a misaligned scalar read gives the right value anyway, so the
 * value comparisons alone can't prove the offsets are right. The test pays
 * off under -fsanitize=alignment and on a target with strict alignment,
 * where a wrong offset stops being invisible. A long double is the case
 * that really needs 16, and a narrower rounding breaks it
 * first. */
TEST(cbstmap_key_storage, every_key_type_round_trips_through_inline_storage) {
  /* We read back through cbmap_get_ptr, because a key offset that is
     under-aligned or of the wrong size makes the key impossible to find, and
     cbmap_get answers that with _ccol_fatal_err, which takes the whole binary
     down with it; a NULL keeps the failure local. */
#define ROUND_TRIP(ctype, keyval, valval)        \
  do {                                           \
    cbmap_construct_scoped(m_, ctype, int);      \
    ctype k_ = (keyval);                         \
    int v_ = (valval);                           \
    cbmap_insert(m_, k_, v_);                    \
    int *found_ = cbmap_get_ptr(m_, k_);         \
    REQUIRE_NE((void *)found_, NULL);            \
    if (found_) REQUIRE_EQ(*found_, (valval));   \
    REQUIRE_EQ(cbmap_elem_count(m_), (size_t)1); \
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

  /* long double on its own, because it has the strongest alignment
     requirement of any key type here, so it fails first if the offset is
     rounded to anything narrower than the type really needs. */
  {
    cbmap_construct_scoped(mld, long double, int);
    long double k = 3.0625L;
    int v = 14;
    cbmap_insert(mld, k, v);
    int *found = cbmap_get_ptr(mld, k);
    REQUIRE_NE((void *)found, NULL);
    if (found) REQUIRE_EQ(*found, 14);
  }

  /* A pointer key, and a string key whose bytes are stored instead of a
     pointer to them. */
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
  // A NULL cmap_pair is a caller mistake in either map module, but the two
  // modules are close enough to interchangeable that two different answers
  // would be a trap: a caller moving from one to the other would get a
  // return code from chmap_insert_elem and a crash from cbmap_insert_elem
  // for the very same line of code. So both report ccol_invalid_args and
  // neither dereferences it.
  //
  // val_pair is the out-parameter of cbmap_get_elem_ref(), which has nowhere
  // to report a hit without it, and an input pair for cbmap_insert_elem(),
  // and we cover both.
  //
  // Every result goes into a local and is only asserted after the map is
  // destroyed, for the reasons given in
  // wrong_size_key_rejected_for_fixed_width_key_type above.
  //
  // It matters because, without the guards, each call dereferences the
  // NULL pair and the whole binary dies with SIGSEGV on the first one.
  cbmap cbm = cbmap_create(ccol_int, NULL);
  REQUIRE_NE((void *)cbm, NULL);

  int key = 3, val = 42;
  const cmap_pair valid_key = {.ptr = &key, .size = sizeof(key)};
  const cmap_pair valid_val = {.ptr = &val, .size = sizeof(val)};

  const ccol_retval_t insert_null_key =
      cbmap_insert_elem(cbm, NULL, &valid_val);
  const ccol_retval_t insert_null_val =
      cbmap_insert_elem(cbm, &valid_key, NULL);
  const ccol_retval_t insert_both_null = cbmap_insert_elem(cbm, NULL, NULL);

  int readback = -1;
  const ccol_retval_t copy_null_key =
      cbmap_get_elem_copy(cbm, NULL, &readback, sizeof(readback));

  cmap_pair out = {NULL, 0};
  const ccol_retval_t ref_null_key = cbmap_get_elem_ref(cbm, NULL, &out);
  const ccol_retval_t ref_null_out = cbmap_get_elem_ref(cbm, &valid_key, NULL);

  const ccol_retval_t delete_null_key = cbmap_delete_elem(cbm, NULL);

  // None of the rejected calls stored anything, and a correctly formed call
  // works afterwards, so the guards only reject the mistake.
  const size_t count_after_rejects = cbmap_elem_count(cbm);
  const ccol_retval_t good_insert =
      cbmap_insert_elem(cbm, &valid_key, &valid_val);
  const size_t count_after_good = cbmap_elem_count(cbm);
  const ccol_retval_t good_copy =
      cbmap_get_elem_copy(cbm, &valid_key, &readback, sizeof(readback));

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

/*
 * The map reads from the cmap_pair the caller supplies instead of just
 * passing it along, so a pair that describes bytes it doesn't have needs
 * to be reported instead of dereferenced. The combination that can't be read
 * is a NULL ptr with a non-zero size. A size of zero is fine on either side,
 * because this module stores and looks up empty keys and empty values, and
 * that's where it differs from chashmap.
 *
 * If the guards go, the insert dies in the memcpy
 * of create_new_node, and the three lookup and delete paths die in the
 * memcmp of compare_keys. The test fills the tree before those three just so
 * they reach compare_keys, because on an empty tree they return
 * ccol_key_not_found without ever reading the pair.
 */
TEST(raw_entry_points, a_null_pointer_pair_with_a_nonzero_size_is_rejected) {
  cbmap_construct(m, int, int);

  cmap_pair unreadable = {.ptr = NULL, .size = sizeof(int)};
  int real_key = 7, real_val = 42;
  cmap_pair key_pair = {.ptr = &real_key, .size = sizeof(int)};
  cmap_pair val_pair = {.ptr = &real_val, .size = sizeof(int)};

  /* An empty tree, where the insert path reads both pairs directly. */
  REQUIRE_EQ(cbmap_insert_elem(m, &unreadable, &val_pair), ccol_invalid_args);
  REQUIRE_EQ(cbmap_insert_elem(m, &key_pair, &unreadable), ccol_invalid_args);

  /* Populated, so the three below really reach compare_keys. */
  REQUIRE_EQ(cbmap_insert_elem(m, &key_pair, &val_pair), ccol_success);

  int out = 0;
  REQUIRE_EQ(cbmap_get_elem_copy(m, &unreadable, &out, sizeof(out)),
             ccol_invalid_args);
  cmap_pair ref = {NULL, 0};
  REQUIRE_EQ(cbmap_get_elem_ref(m, &unreadable, &ref), ccol_invalid_args);
  REQUIRE_EQ(cbmap_delete_elem(m, &unreadable), ccol_invalid_args);

  /* None of the rejections touched the legitimate entry. */
  out = 0;
  REQUIRE_EQ(cbmap_get_elem_copy(m, &key_pair, &out, sizeof(out)),
             ccol_success);
  REQUIRE_EQ(out, 42);

  cbmap_destroy(m);
}

/* ------------------------------------------------------------------------- */
/* Macro argument lifetime                                                    */
/* ------------------------------------------------------------------------- */

/* A compound literal written as a macro argument has automatic storage
   duration limited to the block it's instantiated in, so the type-inferred
   macros have to bind the caller's expression in their own block before it
   reaches any nested one. Otherwise the literal gets created inside
   _populate_cmap_pair's own inner block, dies when that block closes, and the
   insert below copies out of a dead stack slot.

   At the optimization level this suite builds at, these tests do catch it:
   without the binding gcc -O3 reuses the slot, the first one reads 9 instead
   of 12345 and the rest don't find the key at all. Under AddressSanitizer the
   defect is diagnosed whatever the compiler, as a stack-use-after-scope read
   inside create_new_node(). */
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

  /* cbmap_get() only runs once cbmap_get_ptr() has shown the key is there,
     because an absent key is what a regression here produces, and cbmap_get
     answers one with _ccol_fatal_err, which would abort the whole binary and
     every other suite's result in the same run. */
  REQUIRE_EQ(cbmap_get(bm, (int){7}), 900);

  REQUIRE_EQ(cbmap_remove(bm, (int){7}), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)0);
}

typedef struct {
  long a;
  long b;
  long c;
  long d;
} cbmap_lifetime_wide_value;

/* The literal is written behind a macro so its commas don't split the
   cbmap_insert() argument list. It's substituted, and so instantiated,
   exactly where writing it inline would put it. */
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

/* ------------------------------------------------------------------------- */
/* String value references                                                    */
/* ------------------------------------------------------------------------- */

/* A map with string values owns the bytes it stores and keeps its own
   {ptr, size} accessor for the node in step with them. The only char* object
   in the map is that accessor's own ptr field, so cbmap_get_ptr() can only
   hand back its address, and storing a different char* through it would
   leave the size describing the old string. So for a string-valued map the
   macro gives a pointer to a const-qualified target, which turns that store
   into a compile error.

   We check the result's TYPE and not some run-time effect, because the
   guarantee is exactly that the bad store never compiles. The controlling
   expression of _Generic isn't evaluated, so it doesn't touch the map.
   Without the const, the first association below doesn't match and the test
   reports 0. */
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

/* The const only applies to the string case, and every other value type
   keeps the writable pointer that cbmap_get_ptr() is there to provide. */
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

/* Reading the stored char*, and editing the bytes it points at within the
   stored length, are both available; only replacing the pointer isn't. */
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

    /* The accessor's own size never gets out of step with the bytes it
       describes, so a copy-out reads exactly the stored string. */
    char buf[6] = {0};
    cmap_pair key_pair = {.ptr = key, .size = sizeof(key)};
    REQUIRE_EQ(cbmap_get_elem_copy(bm, &key_pair, buf, sizeof(buf)),
               ccol_success);
    REQUIRE_STREQ(buf, "jello");
  }
}

/* cbmap_get_elem_ref() fills a cmap_pair that belongs to the caller, so
   val_pair.ptr and val_pair.size can be read and the bytes ptr describes can
   be edited in place, for a string value like for any other.
   tests/cbstmap/compile_probe.sh pins the signature, a property of the type
   that no run-time test can see, and this test pins the reads and the
   edits. */
TEST(cbmap_elem_ref_accessor, the_filled_pair_reads_and_edits_bytes) {
  cbmap_construct_scoped(bm, char *, char *);

  char key[] = "k";
  cbmap_insert(bm, key, "hello");

  cmap_pair key_pair = {.ptr = key, .size = sizeof(key)};
  cmap_pair val_pair = {NULL, 0};
  REQUIRE_EQ(cbmap_get_elem_ref(bm, &key_pair, &val_pair), ccol_success);
  REQUIRE_NE(val_pair.ptr, NULL);
  if (!val_pair.ptr) return;

  REQUIRE_EQ(val_pair.size, sizeof("hello"));
  REQUIRE_STREQ((const char *)val_pair.ptr, "hello");

  ((char *)val_pair.ptr)[0] = 'j';
  REQUIRE_STREQ(cbmap_get(bm, key), "jello");

  char buf[6] = {0};
  REQUIRE_EQ(cbmap_get_elem_copy(bm, &key_pair, buf, sizeof(buf)),
             ccol_success);
  REQUIRE_STREQ(buf, "jello");
}

/* Each filled pair points at its own node's value, so a result held for one
   key isn't disturbed by looking up any other key, and you can hold several
   at once. */
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

  REQUIRE_EQ(cbmap_get_elem_ref(bm, &first_key, &first_ref), ccol_success);
  REQUIRE_EQ(cbmap_get_elem_ref(bm, &second_key, &second_ref), ccol_success);
  REQUIRE_NE(first_ref.ptr, second_ref.ptr);

  for (int i = 0; i < 64; i++) {
    int other = i;
    cmap_pair other_key = {.ptr = &other, .size = sizeof(other)};
    cmap_pair ignored = {NULL, 0};
    (void)cbmap_get_elem_ref(bm, &other_key, &ignored);
  }

  REQUIRE_EQ(*(const int *)first_ref.ptr, 21);
  REQUIRE_EQ(*(const int *)second_ref.ptr, 123);

  *(int *)first_ref.ptr = 999;
  REQUIRE_EQ(*(const int *)first_ref.ptr, 999);
  REQUIRE_EQ(*(const int *)second_ref.ptr, 123);
}

/* A hit fills the caller's pair with the address and size of the stored
   value, and writing through ptr changes what cbmap_get() reads. A miss, and
   every refused call with a val_pair, set the pair to {NULL, 0}, which the
   poison values below prove. */
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
  ccol_retval_t hit_r = cbmap_get_elem_ref(bm, &kp, &hit);
  bool ptr_in_map = false, write_seen = false;
  if (hit_r == ccol_success) {
    long *direct = cbmap_get_ptr(bm, k);
    ptr_in_map = direct == hit.ptr;
    *(long *)hit.ptr = 4242;
    write_seen = cbmap_get(bm, k) == 4242;
  }
  cmap_pair miss = {(void *)0x1, 12345};
  ccol_retval_t miss_r = cbmap_get_elem_ref(bm, &kpa, &miss);
  cmap_pair refused = {(void *)0x1, 12345};
  ccol_retval_t refused_r = cbmap_get_elem_ref(bm, &bad, &refused);
  cmap_pair null_key = {(void *)0x1, 12345};
  ccol_retval_t null_key_r = cbmap_get_elem_ref(bm, NULL, &null_key);
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

/* For a map with string values cbmap_get_ptr() points at the char* that the
   node keeps, so two calls for one key give the same address and the string
   bytes can be edited through it. */
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

/* ========================================================================== */
/*          RAW-LAYER KEY PAIRS CARRY NO ALIGNMENT GUARANTEE                  */
/* ========================================================================== */

/* cbmap_insert_elem, cbmap_get_elem_ref and cbmap_delete_elem accept a
 * cmap_pair the caller built by hand, pointing at whatever address the
 * caller has: key bytes sliced out of a packed frame, a mmap()ed record or a
 * serialized buffer sit at an arbitrary offset.
 *
 * So the comparison path must read both operands with memcpy and never
 * dereference a pointer cast to the key type. The note on _ccol_typed_cmp in
 * common.h explains the two separate requirements the cast breaks: the
 * address's alignment and the object's effective type.
 *
 * This test would notice a build whose _ccol_typed_cmp and
 * cmp_float_val dereference a cast pointer: an UndefinedBehaviorSanitizer
 * run reports "load of misaligned address ... which requires N byte
 * alignment" at the first probe below, for every one of these key types. */
TEST(cbmap_raw_layer_alignment, misaligned_key_pair_resolves_for_every_type) {
  /* One over-aligned block. Every probe puts its key bytes at an ODD offset
   * inside it, so the address is misaligned for every type wider than a
   * byte. */
  _Alignas(64) static unsigned char frame[256];

#define CBMAP_MISALIGNED_PROBE(TYPE, DATA_TYPE, VALUE, OFFSET)                 \
  do {                                                                         \
    char *probe_err = NULL;                                                    \
    cbmap probe_map = cbmap_create(DATA_TYPE, &probe_err);                     \
    REQUIRE_NE((void *)probe_map, NULL);                                       \
    TYPE probe_key = (VALUE);                                                  \
    int probe_val = 4242;                                                      \
    cmap_pair aligned_pair = {.ptr = &probe_key, .size = sizeof(probe_key)};   \
    cmap_pair val_pair = {.ptr = &probe_val, .size = sizeof(probe_val)};       \
    REQUIRE_EQ(cbmap_insert_elem(probe_map, &aligned_pair, &val_pair),         \
               ccol_success);                                                  \
    memset(frame, 0, sizeof(frame));                                           \
    memcpy(frame + (OFFSET), &probe_key, sizeof(probe_key));                   \
    REQUIRE_NE((size_t)((uintptr_t)(frame + (OFFSET)) % _Alignof(TYPE) == 0 && \
                        _Alignof(TYPE) > 1),                                   \
               (size_t)1); /* the probe address really is misaligned */        \
    cmap_pair misaligned_pair = {.ptr = frame + (OFFSET),                      \
                                 .size = sizeof(probe_key)};                   \
    cmap_pair found = {NULL, 0};                                               \
    REQUIRE_EQ(cbmap_get_elem_ref(probe_map, &misaligned_pair, &found),        \
               ccol_success);                                                  \
    REQUIRE_EQ(*(const int *)found.ptr, 4242);                                 \
    REQUIRE_EQ(cbmap_delete_elem(probe_map, &misaligned_pair), ccol_success);  \
    REQUIRE_EQ(cbmap_elem_count(probe_map), (size_t)0);                        \
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

/* A misaligned key pair should give the SAME ordering as an aligned one. A
 * read that silently picked up the wrong bytes would still "work" for a
 * single lookup while corrupting the tree order, so this walks the map in
 * order and checks the sequence itself. */
TEST(cbmap_raw_layer_alignment, misaligned_inserts_keep_the_sorted_order) {
  _Alignas(64) static unsigned char frame[64];
  char *err = NULL;
  cbmap bm = cbmap_create(ccol_int, &err);
  REQUIRE_NE((void *)bm, NULL);

  /* Insert 0..63 out of order, every key through a misaligned pair. */
  for (int step = 0; step < 64; step++) {
    int k = (step * 37) % 64; /* 37 is coprime with 64: a permutation */
    int v = k * 5;
    memcpy(frame + 1, &k, sizeof(k));
    cmap_pair kp = {.ptr = frame + 1, .size = sizeof(k)};
    cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
    ccol_retval_t r = cbmap_insert_elem(bm, &kp, &vp);
    REQUIRE_EQ(r, ccol_success);
  }
  REQUIRE_EQ(cbmap_elem_count(bm), (size_t)64);

  int expected = 0;
  for (cmap_iterator *it = cbmap_begin_iter(bm, NULL); it;
       it = it->_next_fn(it)) {
    int k;
    memcpy(&k, it->key_pair->ptr, sizeof(k));
    REQUIRE_EQ(k, expected);
    REQUIRE_EQ(*(const int *)it->val_pair->ptr, expected * 5);
    expected++;
  }
  REQUIRE_EQ(expected, 64);
  cbmap_destroy(bm);
}

/* ========================================================================== */
/*          AN ITERATOR THAT OUTLIVES ITS CONTAINER UNTIL SCOPE EXIT          */
/* ========================================================================== */

/* An allocator that overwrites every block with a poison pattern before it
 * frees it. Reading freed memory then gives the poison and not the stale
 * value, so a use after free through a stale pointer faults in an ordinary
 * run and doesn't need a sanitizer to show up. */
typedef union {
  size_t size;
  max_align_t align;
} cbmap_life_poison_hdr;

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

static ccol_memmgmt_procs_t cbmap_life_poison_procs_storage = {
    .malloc = cbmap_life_poison_malloc,
    .calloc = cbmap_life_poison_calloc,
    .realloc = cbmap_life_poison_realloc,
    .free = cbmap_life_poison_free};

static ccol_memmgmt_procs_t *cbmap_life_poison_procs(void) {
  return &cbmap_life_poison_procs_storage;
}

/* A loop that leaves early keeps its iterator live until the end of the
 * ccol_iter_declare scope. Destroying the map inside that scope has to leave
 * the iterator freeable, because the scope-exit cleanup frees it after the
 * map and its allocator record are gone. An iterator that frees itself
 * through the map would read the poisoned map struct at scope exit and
 * fault, which this test catches. */
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

/* ========================================================================== */
/*              A STRUCT KEY WITH PADDING, THROUGH THE MACRO LAYER            */
/* ========================================================================== */

typedef struct {
  char tag;
  long id; /* padding sits between tag and id on every supported target */
} cbmap_padded_key;

#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define CBMAP_TEST_COMPILER_CLEARS_PADDING 1
#endif
#endif

#ifdef CBMAP_TEST_COMPILER_CLEARS_PADDING
static cbmap_padded_key cbmap_padded_make(char tag, long id) {
  cbmap_padded_key k;
  memset(&k, 0, sizeof(k));
  k.tag = tag;
  k.id = id;
  return k;
}

/* Fills the stack below the caller with a non-zero pattern, so a copy that
 * leaves its padding untouched shows it as non-zero bytes. */
static __attribute__((noinline)) void cbmap_padded_dirty_stack(void) {
  volatile unsigned char buf[4096];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0xAB;
}

static __attribute__((noinline)) void cbmap_padded_insert(cbmap m, int i) {
  cbmap_redeclare(m, cbmap_padded_key, int);
  cbmap_insert(m, cbmap_padded_make('x', i), i);
}

static __attribute__((noinline)) int *cbmap_padded_lookup(cbmap m, int i) {
  cbmap_redeclare(m, cbmap_padded_key, int);
  return cbmap_get_ptr(m, cbmap_padded_make('x', i));
}

#endif

/* With no comparator, a key of a type the map doesn't know compares byte for
 * byte, padding included. Where the compiler has __builtin_clear_padding the
 * macros clear the padding of their own copy of the key, so a padded key
 * needs no comparator there. Under GCC this test catches it: without the
 * clearing the stored keys carry the stack pattern in their padding and the
 * lookups miss. */
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
  /* Every stored key carries zero padding. */
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
  size_t n = cbmap_elem_count(m);
  cbmap_destroy(m);
  REQUIRE_TRUE(all_found);
  REQUIRE_TRUE(padding_zero);
  REQUIRE_EQ(n, (size_t)100);
#endif
}

/* ------------------------------------------------------------------------ */
/* A NULL character pointer through the typed macros                         */
/* ------------------------------------------------------------------------ */

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define CBMAP_NULL_TEST_UNSET_VAR "CCOL_CBMAP_TEST_SURELY_UNSET_VARIABLE"

/* See the matching helper in tests/chashmap/tests.c. The result goes through
 * a pipe, because valgrind overrides the exit status of a child that exits
 * with memory still reachable, and a child that dies on SIGSEGV writes
 * nothing. */
enum { cbmap_null_probe_crashed = 250, cbmap_null_probe_aborted = 251 };

static int cbmap_null_run_probe(int (*probe)(void)) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    unsigned char byte = (unsigned char)probe();
    ssize_t w = write(fds[1], &byte, 1);
    (void)w;
    _exit(0);
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

static int cbmap_null_probe_get_ptr_key(void) {
  unsetenv(CBMAP_NULL_TEST_UNSET_VAR);
  cbmap_construct(b, char *, int);
  cbmap_insert(b, "HOME", 1);
  int *p = cbmap_get_ptr(b, getenv(CBMAP_NULL_TEST_UNSET_VAR));
  int ok = p == NULL && cbmap_elem_count(b) == 1;
  cbmap_destroy(b);
  return ok;
}

static int cbmap_null_probe_remove_key(void) {
  unsetenv(CBMAP_NULL_TEST_UNSET_VAR);
  cbmap_construct(b, char *, int);
  cbmap_insert(b, "HOME", 1);
  ccol_retval_t r = cbmap_remove(b, getenv(CBMAP_NULL_TEST_UNSET_VAR));
  int ok = r == ccol_invalid_args && cbmap_elem_count(b) == 1;
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

/* A NULL character pointer isn't a string. The raw layer of this module
 * accepts a size-0 pair as an empty key or value, so the macros refuse a
 * pair with no pointer themselves, with the same ccol_invalid_args chmap
 * reports: cbmap_get_ptr() gives NULL, cbmap_remove() returns the code, and
 * cbmap_insert() and cbmap_get() stop the program. Without the NULL test
 * in _populate_cmap_pair() every probe dies with
 * SIGSEGV inside strlen(), and without the refusal in the macros the insert
 * probes store an empty key or value and report success. */
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

/* ------------------------------------------------------------------------ */
/* Binary keys                                                                */
/* ------------------------------------------------------------------------ */

/* Through the typed macros a fixed-size digest wrapped in a struct is a
 * binary key, and the tree orders keys by all 32 bytes, embedded zero bytes
 * included. */
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
  size_t count = cbmap_elem_count(seen);
  int v1 = cbmap_get(seen, d1);
  int v2 = cbmap_get(seen, d2);
  int v3 = cbmap_get(seen, d3);
  cbmap_destroy(seen);
  REQUIRE_EQ(count, (size_t)3);
  REQUIRE_EQ(v1, 1);
  REQUIRE_EQ(v2, 2);
  REQUIRE_EQ(v3, 3);
}

/* A binary key whose length only the caller knows goes through the raw
 * layer, with that length in cmap_pair.size. Every byte counts, embedded
 * zero bytes included, and a key that's a prefix of another up to and past a
 * zero byte stays distinct from it. */
TEST(cbmap_binary_keys, a_raw_layer_key_with_embedded_zeros) {
  cbmap m = cbmap_create(ccol_other_types, NULL);
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
  ccol_retval_t r1 = cbmap_insert_elem(m, &kp1, &vp1);
  ccol_retval_t r2 = cbmap_insert_elem(m, &kp2, &vp2);
  ccol_retval_t r3 = cbmap_insert_elem(m, &kp3, &vp3);
  size_t count = cbmap_elem_count(m);
  unsigned char probe[16];
  memcpy(probe, k2, sizeof(probe));
  cmap_pair pp = {probe, sizeof(probe)};
  cmap_pair out = {NULL, 0};
  ccol_retval_t rg = cbmap_get_elem_ref(m, &pp, &out);
  int got = -1;
  if (rg == ccol_success && out.ptr) memcpy(&got, out.ptr, sizeof(got));
  cmap_pair pp3 = {k3, sizeof(k3)};
  cmap_pair out3 = {NULL, 0};
  ccol_retval_t rg3 = cbmap_get_elem_ref(m, &pp3, &out3);
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

// ========================================================================
// TYPED DESCENT AND REBALANCE EARLY-STOP TESTS
// ========================================================================

// Inserts the values in a scrambled order, then checks three things: an
// in-order walk strictly increases under the key type's native <, every
// inserted key is found with its own value, and a key that was never
// inserted is absent. The value sets put negative numbers next to positive
// ones for the signed types, and values with the top bit set next to small
// ones for the unsigned and pointer types, so a comparison of the wrong
// signedness or width orders them differently from <.
#define CBMAP_TYPED_ORDER_CASE(T, absent, ...)                             \
  do {                                                                     \
    T vals_[] = {__VA_ARGS__};                                             \
    const size_t n_ = sizeof(vals_) / sizeof(vals_[0]);                    \
    _Static_assert(sizeof(vals_) / sizeof(vals_[0]) < 11, "scramble");     \
    cbmap_construct(om, T, int);                                           \
    for (size_t i_ = 0; i_ < n_; ++i_) {                                   \
      size_t j_ = (i_ * 11 + 3) % n_; /* a permutation for n_ < 11 */      \
      T k_ = vals_[j_];                                                    \
      int v_ = (int)j_;                                                    \
      cbmap_insert(om, k_, v_);                                            \
    }                                                                      \
    bool ok_ = cbmap_elem_count(om) == n_ && cbmap_debug_validate_avl(om); \
    size_t seen_ = 0;                                                      \
    T prev_ = vals_[0];                                                    \
    ccol_iter_declare(om, it_);                                            \
    for (it_ = ccol_begin(om); it_ != NULL; it_ = ccol_iter_next(it_)) {   \
      T k_ = *ccol_iter_key_ptr(it_);                                      \
      if (seen_ > 0 && !(prev_ < k_)) ok_ = false;                         \
      prev_ = k_;                                                          \
      ++seen_;                                                             \
    }                                                                      \
    ok_ = ok_ && seen_ == n_;                                              \
    for (size_t i_ = 0; i_ < n_; ++i_) {                                   \
      T k_ = vals_[i_];                                                    \
      int *p_ = cbmap_get_ptr(om, k_);                                     \
      if (!p_ || *p_ != (int)i_) ok_ = false;                              \
    }                                                                      \
    T a_ = (absent);                                                       \
    if (cbmap_get_ptr(om, a_) != NULL) ok_ = false;                        \
    cbmap_destroy(om);                                                     \
    REQUIRE_TRUE(ok_);                                                     \
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
  // Pointers order as unsigned addresses. These values are never
  // dereferenced, they're only keys.
  CBMAP_TYPED_ORDER_CASE(
      void *, (void *)(uintptr_t)0x30, (void *)(uintptr_t)0x10,
      (void *)(uintptr_t)UINTPTR_MAX, (void *)(uintptr_t)(UINTPTR_MAX / 2 + 1),
      (void *)(uintptr_t)(UINTPTR_MAX / 2), (void *)(uintptr_t)0x20,
      (void *)(uintptr_t)(UINTPTR_MAX - 15));
  // The generic comparison path, for contrast.
  CBMAP_TYPED_ORDER_CASE(double, 0.5, -1e300, 1e300, -0.25, 0.0, 2.5, -7.0);
}

static int cbmap_tests_reverse_int_cmp(const void *a, const void *b) {
  int x, y;
  memcpy(&x, a, sizeof(x));
  memcpy(&y, b, sizeof(y));
  return (y > x) - (y < x);
}

// A custom comparison proc has to decide the order even for an integer key
// type, which otherwise has a typed descent of its own.
TEST(cbst_maps, custom_proc_on_an_integer_key_still_decides_the_order) {
  cbmap_construct_cc(rm, int, int, cbmap_tests_reverse_int_cmp);
  for (int i = 0; i < 64; ++i) {
    int k = (i * 37) % 64, v = k * 2;
    cbmap_insert(rm, k, v);
  }
  bool ok = cbmap_elem_count(rm) == 64 && cbmap_debug_validate_avl(rm);
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
  ok = ok && cbmap_elem_count(rm) == 32 && cbmap_debug_validate_avl(rm);
  for (int k = 0; k < 64; ++k) {
    if ((cbmap_get_ptr(rm, k) != NULL) != (k % 2 == 1)) ok = false;
  }
  cbmap_destroy(rm);
  REQUIRE_TRUE(ok);
}

#define DEEP_CHURN_KEY_RANGE 4096

// Insert and delete both stop rebalancing at the first ancestor whose height
// didn't change. This test builds trees deep enough for a rotation and a
// height change to happen far from the leaf, with long runs of deletes that
// shrink the tree and runs of inserts that grow it back, and validates every
// height and balance factor after every operation. A rebalance that stops
// one level too early leaves a stale height above it, and the validator
// reports that.
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
    // Even phases mostly insert, odd phases mostly delete.
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
      if (!cbmap_debug_validate_avl(dm) || cbmap_elem_count(dm) != count) {
        ok = false;
      }
    }
  }
  for (int k = 0; k < DEEP_CHURN_KEY_RANGE && ok; ++k) {
    int *p = cbmap_get_ptr(dm, k);
    if (present[k] ? (!p || *p != (k ^ 0x55)) : p != NULL) ok = false;
  }
  // Drain in an order that deletes from both ends towards the middle.
  for (int lo = 0, hi = DEEP_CHURN_KEY_RANGE - 1; lo <= hi && ok; ++lo, --hi) {
    cbmap_remove(dm, lo);
    if (lo != hi) cbmap_remove(dm, hi);
    if (!cbmap_debug_validate_avl(dm)) ok = false;
  }
  ok = ok && cbmap_elem_count(dm) == 0;
  cbmap_destroy(dm);
  REQUIRE_TRUE(ok);
}
