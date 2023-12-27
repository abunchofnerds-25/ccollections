#include <cbstmap.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()  // sets up Tau (+ main function)

// BST_MAP TESTS (AVL Tree)

TEST(cbst_maps, create_fails) {
  char* err = NULL;
  cbinarymap* cbmap = cbmap_create(true, &err);
  REQUIRE_NE((void*)cbmap, NULL);
  cbmap_destroy(cbmap);

  cbmap = cbmap_create_mp(
      true,
      &(ccol_memmgmt_procs_t){
          .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void*)cbmap, NULL);
  REQUIRE_NE((void*)err, NULL);

  cbmap = cbmap_create_mp(
      true,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void*)cbmap, NULL);
  REQUIRE_NE((void*)err, NULL);

  cbmap = cbmap_create_mp(
      true,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err);
  REQUIRE_EQ((void*)cbmap, NULL);
  REQUIRE_NE((void*)err, NULL);

  cbmap = cbmap_create_mp(
      true,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL},
      &err);
  REQUIRE_EQ((void*)cbmap, NULL);
  REQUIRE_NE((void*)err, NULL);
}

TEST(cbst_maps, create_succeeds) {
  char* err = "";
  cbinarymap* cbmap = cbmap_create(true, &err);
  REQUIRE_NE((void*)cbmap, NULL);
  REQUIRE_EQ((void*)err, NULL);

  cbmap_destroy(cbmap);
  REQUIRE_EQ((void*)cbmap, NULL);

  cbmap = cbmap_create_mp(
      true,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err);

  cbmap_destroy(cbmap);
  REQUIRE_EQ((void*)cbmap, NULL);
}

// Helper functions start.
int insert_int_to_int(cbinarymap* cbmap, int key, int val) {
  return cbmap_insert_elem(
      cbmap, &(cmap_pair){.ptr = (void*)&key, .size = sizeof(key)},
      &(cmap_pair){.ptr = (void*)&val, .size = sizeof(val)});
}

int get_int_from_int(cbinarymap* cbmap, int key, int* val_ptr) {
  return cbmap_get_elem_copy(
      cbmap, &(cmap_pair){.ptr = (void*)&key, .size = sizeof(key)}, val_ptr,
      sizeof(int));
}

int get_int_ref_from_int(cbinarymap* cbmap, int key, int** val_ptr) {
  cmap_pair* tmp_val_pair_ptr = NULL;
  if (cbmap_get_elem_ref(cbmap,
                         &(cmap_pair){.ptr = (void*)&key, .size = sizeof(key)},
                         &tmp_val_pair_ptr) == 0) {
    *val_ptr = (int*)tmp_val_pair_ptr->ptr;
    return 0;
  }
  return -1;
}

ccol_retval_t delete_int_from_int(cbinarymap* cbmap, int key) {
  return cbmap_delete_elem(
      cbmap, &(cmap_pair){.ptr = (void*)&key, .size = sizeof(key)});
}
// Helper functions end.

TEST(cbst_maps, basic_insertions_and_lookups) {
  cbinarymap* cbmap = cbmap_create(true, NULL);
  REQUIRE_NE((void*)cbmap, NULL);

  int val = -1;

  REQUIRE_EQ(get_int_from_int(cbmap, 100, &val), ccol_key_not_found);

  REQUIRE_EQ(cbmap_elem_count(cbmap), 0);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 20), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 1);
  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 30), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 1);

  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 40), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 2);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 50), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 2);

  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 30);
  REQUIRE_EQ(get_int_from_int(cbmap, 20, &val), ccol_success);
  REQUIRE_EQ(val, 50);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, basic_insertions_and_lookups_with_memmgmt_procs) {
  cbinarymap* cbmap = cbmap_create_mp(
      true,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      NULL);
  REQUIRE_NE((void*)cbmap, NULL);

  int val = -1;

  REQUIRE_EQ(cbmap_elem_count(cbmap), 0);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 20), ccol_success);
  REQUIRE_EQ(cbmap_elem_count(cbmap), 1);
  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 30), ccol_success);
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
  cbinarymap* cbmap = cbmap_create(true, NULL);

  short key1 = 3;
  long val1 = 43;
  REQUIRE_EQ(cbmap_insert_elem(
                 cbmap, &(cmap_pair){.ptr = (void*)&key1, .size = sizeof(key1)},
                 &(cmap_pair){.ptr = (void*)&val1, .size = sizeof(val1)}),
             ccol_success);

  int key2 = 4;
  char val2 = 44;
  REQUIRE_EQ(cbmap_insert_elem(
                 cbmap, &(cmap_pair){.ptr = (void*)&key2, .size = sizeof(key2)},
                 &(cmap_pair){.ptr = (void*)&val2, .size = sizeof(val2)}),
             ccol_success);

  long val1_from_map = -1;
  REQUIRE_EQ(cbmap_get_elem_copy(
                 cbmap, &(cmap_pair){.ptr = (void*)&key1, .size = sizeof(key1)},
                 &val1_from_map, sizeof(val1_from_map)),
             ccol_success);
  REQUIRE_EQ(val1_from_map, val1);

  char val2_from_map = -1;
  REQUIRE_EQ(cbmap_get_elem_copy(
                 cbmap, &(cmap_pair){.ptr = (void*)&key2, .size = sizeof(key2)},
                 &val2_from_map, sizeof(val2_from_map)),
             ccol_success);
  REQUIRE_EQ(val2_from_map, val2);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, basic_deletions) {
  cbinarymap* cbmap = cbmap_create(true, NULL);
  REQUIRE_NE((void*)cbmap, NULL);

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
  cbinarymap* cbmap = cbmap_create(true, NULL);
  REQUIRE_NE((void*)cbmap, NULL);

  REQUIRE_EQ(insert_int_to_int(cbmap, 10, 100), ccol_success);
  REQUIRE_EQ(insert_int_to_int(cbmap, 20, 200), ccol_success);

  int* val_ptr = NULL;
  REQUIRE_EQ(get_int_ref_from_int(cbmap, 10, &val_ptr), 0);
  REQUIRE_EQ(*val_ptr, 100);

  // Modify through reference
  *val_ptr = 150;

  int val = -1;
  REQUIRE_EQ(get_int_from_int(cbmap, 10, &val), ccol_success);
  REQUIRE_EQ(val, 150);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, reset) {
  cbinarymap* cbmap = cbmap_create(true, NULL);
  REQUIRE_NE((void*)cbmap, NULL);

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

  // Can still use after reset
  REQUIRE_EQ(insert_int_to_int(cbmap, 42, 420), ccol_success);
  REQUIRE_EQ(get_int_from_int(cbmap, 42, &val), ccol_success);
  REQUIRE_EQ(val, 420);

  cbmap_destroy(cbmap);
}

TEST(cbst_maps, in_order_iteration) {
  // BST should iterate in sorted order
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
  cbmap_iter_declare(bmap, it);
  for (it = cbmap_begin(bmap); it != NULL; it = cbmap_iter_next(it)) {
    int key = *cbmap_iter_key_ptr(it);
    int val = *cbmap_iter_val_ptr(it);

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
  // Insert in sorted order (worst case for unbalanced BST)
  // AVL tree should handle this efficiently
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
  cbmap_iter_declare(bmap, it);
  for (it = cbmap_begin(bmap); it != NULL; it = cbmap_iter_next(it)) {
    int key = *cbmap_iter_key_ptr(it);
    REQUIRE_EQ(key, expected_key);
    ++expected_key;
  }

  cbmap_destroy(bmap);
}

TEST(cbst_maps, avl_balancing_reverse_order) {
  // Insert in reverse sorted order (another worst case)
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

  // Verify in-order iteration (should be ascending)
  int expected_key = 0;
  cbmap_iter_declare(bmap, it);
  for (it = cbmap_begin(bmap); it != NULL; it = cbmap_iter_next(it)) {
    int key = *cbmap_iter_key_ptr(it);
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

  // Verify in-order iteration still works
  int count = 0;
  cbmap_iter_declare(bmap, it);
  for (it = cbmap_begin(bmap); it != NULL; it = cbmap_iter_next(it)) {
    int key = *cbmap_iter_key_ptr(it);
    REQUIRE_TRUE(key % 2 == 1);  // Should be odd numbers only
    ++count;
  }
  REQUIRE_EQ(count, 25);

  cbmap_destroy(bmap);
}

int custom_int_comparator(const void* a, const void* b) {
  int val_a = *(const int*)a;
  int val_b = *(const int*)b;
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
  cbmap_construct(bm, int, char*);

  int key = 3;
  cbmap_insert(bm, key, "hello");

  key = 4;
  cbmap_insert(bm, key, "world");

  key = 5;
  cbmap_insert(bm, key, "test");

  int records[6] = {0};  // from 0 to 5, so that 3, 4 and 5 are valid indices
  int counter = 0;
  cbmap_iter_declare(bm, it);
  for (it = cbmap_begin(bm); it != NULL; it = cbmap_iter_next(it)) {
    key = *cbmap_iter_key_ptr(it);
    if (key == 3) {
      REQUIRE_EQ(records[3]++, 0);
      REQUIRE_STREQ(*cbmap_iter_val_ptr(it), "hello");
    } else if (key == 4) {
      REQUIRE_EQ(records[4]++, 0);
      REQUIRE_STREQ(*cbmap_iter_val_ptr(it), "world");
    } else if (key == 5) {
      REQUIRE_EQ(records[5]++, 0);
      REQUIRE_STREQ(*cbmap_iter_val_ptr(it), "test");
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

  cbmap_construct(bm, int, some_struct*);

  some_struct d1 = {.a = 3, .b = 4};
  some_struct d2 = {.a = 5, .b = 6};
  some_struct d3 = {.a = 7, .b = 8};

  int key = 1;
  some_struct* ptr = &d1;
  cbmap_insert(bm, key, ptr);
  key = 2;
  ptr = &d2;
  cbmap_insert(bm, key, ptr);
  key = 3;
  ptr = &d3;
  cbmap_insert(bm, key, ptr);

  int counter = 0;
  cbmap_iter_declare(bm, it);
  for (it = cbmap_begin(bm); it != NULL; it = cbmap_iter_next(it)) {
    int key = *cbmap_iter_key_ptr(it);
    if (key == 1) {
      REQUIRE_EQ((*cbmap_iter_val_ptr(it))->a, 3);
      REQUIRE_EQ((*cbmap_iter_val_ptr(it))->b, 4);
    } else if (key == 2) {
      REQUIRE_EQ((*cbmap_iter_val_ptr(it))->a, 5);
      REQUIRE_EQ((*cbmap_iter_val_ptr(it))->b, 6);
    } else if (key == 3) {
      REQUIRE_EQ((*cbmap_iter_val_ptr(it))->a, 7);
      REQUIRE_EQ((*cbmap_iter_val_ptr(it))->b, 8);
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

  // Verify in-order iteration includes negative numbers in order
  int expected = -50;
  cbmap_iter_declare(bm, it);
  for (it = cbmap_begin(bm); it != NULL; it = cbmap_iter_next(it)) {
    int key = *cbmap_iter_key_ptr(it);
    REQUIRE_EQ(key, expected);
    ++expected;
  }
  REQUIRE_EQ(expected, 50);

  cbmap_destroy(bm);
}

TEST(cbst_maps, long_keys) {
  cbmap_construct(bm, long, long);

  long keys[] = {1000000000L, 2000000000L, 3000000000L, 4000000000L};
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

  // Insert same key with different value
  val = 200;
  cbmap_insert(bm, key, val);
  REQUIRE_EQ(cbmap_elem_count(bm), 1);  // Count should not increase
  REQUIRE_EQ(cbmap_get(bm, key), 200);  // Value should be updated

  cbmap_destroy(bm);
}

TEST(cbst_maps, empty_tree_iteration) {
  cbmap_construct(bm, int, int);

  // Iterate over empty tree
  int count = 0;
  cbmap_iter_declare(bm, it);
  for (it = cbmap_begin(bm); it != NULL; it = cbmap_iter_next(it)) {
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
  cbmap_iter_declare(bm, it);
  for (it = cbmap_begin(bm); it != NULL; it = cbmap_iter_next(it)) {
    REQUIRE_EQ(*cbmap_iter_key_ptr(it), 42);
    REQUIRE_EQ(*cbmap_iter_val_ptr(it), 420);
    ++count;
  }

  REQUIRE_EQ(count, 1);

  cbmap_destroy(bm);
}

TEST(cbst_maps, stress_test_many_insertions) {
  cbmap_construct(bm, int, int);

  const int size = 1000;

  // Insert elements
  for (int i = 0; i < size; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), size);

  // Verify all elements
  for (int i = 0; i < size; ++i) {
    int val = cbmap_get(bm, i);
    REQUIRE_EQ(val, i * 10);
  }

  // Verify in-order iteration
  int expected = 0;
  cbmap_iter_declare(bm, it);
  for (it = cbmap_begin(bm); it != NULL; it = cbmap_iter_next(it)) {
    int key = *cbmap_iter_key_ptr(it);
    REQUIRE_EQ(key, expected);
    ++expected;
  }
  REQUIRE_EQ(expected, size);

  cbmap_destroy(bm);
}

TEST(cbst_maps, stress_test_many_deletions) {
  cbmap_construct(bm, int, int);

  const int size = 129;

  // Insert elements
  for (int i = 0; i < size; ++i) {
    int val = i * 10;
    cbmap_insert(bm, i, val);
  }

  REQUIRE_EQ(cbmap_elem_count(bm), size);

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
    // Test that re-enabling local macros works
    cbmap_enable_local_macros(bm, int, int);

    for (int i = 0; i < 10; ++i) {
      int val = cbmap_get(bm, i);
      REQUIRE_EQ(val, i * 10);
    }
  }

  cbmap_destroy(bm);
}