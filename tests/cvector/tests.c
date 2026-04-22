#include <cvector.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
TAU_MAIN()  // sets up Tau (+ main function)

// C_VECTOR TESTS

TEST(cvectors, create_fails) {
  char *err_str = NULL;
  cvector *cvec = cvector_create(0, &err_str);
  REQUIRE_EQ((void *)cvec, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cvec = cvector_create_full(
      0,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)cvec, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cvec = cvector_create_full(
      16,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)cvec, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cvec = cvector_create_full(
      16,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)cvec, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cvec = cvector_create_full(
      16,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)cvec, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

TEST(cvectors, create_succeeds) {
  char *err_str = NULL;
  cvector *cvec = cvector_create(sizeof(int), &err_str);
  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);
  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);

  cvec = cvector_create_full(
      sizeof(int),
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);
  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(cvectors, simple_push_backs) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_push_back(cvec, &(int){1}), ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 1);

  REQUIRE_EQ(cvector_push_back(cvec, &(int){1}), ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 2);

  cvector_destroy(cvec);
}

TEST(cvectors, simple_push_backs_with_memmgmt_procs) {
  cvector *cvec = cvector_create_full(
      sizeof(int),
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      NULL);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_push_back(cvec, &(int){1}), ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 1);

  REQUIRE_EQ(cvector_push_back(cvec, &(int){1}), ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 2);

  cvector_destroy(cvec);
}

TEST(cvectors, push_back_null_element) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_push_back(cvec, NULL), ccol_invalid_args);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

TEST(cvectors, access_an_index) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_at(cvec, 0), NULL);
  REQUIRE_EQ(cvector_at(cvec, 1), NULL);
  REQUIRE_EQ(cvector_at(cvec, (size_t)-1), NULL);

  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &(int){i + 1});
  }

  for (int i = 0; i < 4; ++i) {
    int *var_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void *)var_ptr, NULL);
    REQUIRE_EQ(*var_ptr, i + 1);
  }

  REQUIRE_EQ(cvector_at(cvec, 4), NULL);
  REQUIRE_EQ(cvector_at(cvec, (size_t)-1), NULL);

  cvector_destroy(cvec);
}

TEST(cvectors, simple_pop_backs) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  int target = -2;
  REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_container_empty);
  REQUIRE_EQ(target, -2);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  cvector_push_back(cvec, &(int){1});
  cvector_push_back(cvec, &(int){2});
  REQUIRE_EQ(cvector_elem_count(cvec), 2);

  REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
  REQUIRE_EQ(target, 2);
  REQUIRE_EQ(cvector_elem_count(cvec), 1);
  REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
  REQUIRE_EQ(target, 1);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

TEST(cvectors, pop_back_null_target) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  cvector_push_back(cvec, &(int){42});
  REQUIRE_EQ(cvector_pop_back(cvec, NULL), ccol_invalid_args);
  REQUIRE_EQ(cvector_elem_count(cvec), 1);

  cvector_destroy(cvec);
}

TEST(cvectors, different_sizes) {
  {
    cvector *cvec = cvector_create(sizeof(long), NULL);

    REQUIRE_EQ(cvector_push_back(cvec, &(long){1}), ccol_success);
    REQUIRE_EQ(cvector_push_back(cvec, &(long){2}), ccol_success);

    REQUIRE_EQ(cvector_elem_count(cvec), 2);

    long target;
    REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
    REQUIRE_EQ(target, 2);
    REQUIRE_EQ(cvector_elem_count(cvec), 1);
    REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
    REQUIRE_EQ(target, 1);
    REQUIRE_EQ(cvector_elem_count(cvec), 0);

    cvector_destroy(cvec);
  }

  {
    cvector *cvec = cvector_create(sizeof(char), NULL);

    REQUIRE_EQ(cvector_push_back(cvec, &(char){1}), ccol_success);
    REQUIRE_EQ(cvector_push_back(cvec, &(char){2}), ccol_success);

    REQUIRE_EQ(cvector_elem_count(cvec), 2);

    char target;
    REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
    REQUIRE_EQ(target, 2);
    REQUIRE_EQ(cvector_elem_count(cvec), 1);
    REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
    REQUIRE_EQ(target, 1);
    REQUIRE_EQ(cvector_elem_count(cvec), 0);

    cvector_destroy(cvec);
  }

  {
    cvector *cvec = cvector_create(sizeof(int16_t), NULL);

    REQUIRE_EQ(cvector_push_back(cvec, &(int16_t){1}), ccol_success);
    REQUIRE_EQ(cvector_push_back(cvec, &(int16_t){2}), ccol_success);

    REQUIRE_EQ(cvector_elem_count(cvec), 2);

    int16_t target;
    REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
    REQUIRE_EQ(target, 2);
    REQUIRE_EQ(cvector_elem_count(cvec), 1);
    REQUIRE_EQ(cvector_pop_back(cvec, &target), ccol_success);
    REQUIRE_EQ(target, 1);
    REQUIRE_EQ(cvector_elem_count(cvec), 0);

    cvector_destroy(cvec);
  }
}

TEST(cvectors, large_struct_elements) {
  typedef struct {
    int arr[64];  // 256 bytes
    double val;
  } large_struct;

  cvector *cvec = cvector_create(sizeof(large_struct), NULL);

  large_struct ls = {{0}, 3.14};
  ls.arr[0] = 100;
  ls.arr[63] = 200;

  REQUIRE_EQ(cvector_push_back(cvec, &ls), ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 1);

  large_struct *retrieved = cvector_at(cvec, 0);
  REQUIRE_NE((void *)retrieved, NULL);
  REQUIRE_EQ(retrieved->arr[0], 100);
  REQUIRE_EQ(retrieved->arr[63], 200);

  cvector_destroy(cvec);
}

TEST(cvectors, reset) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &(int){i + 1});
  }
  REQUIRE_EQ(cvector_elem_count(cvec), 4);

  cvector_reset(cvec);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

TEST(cvectors, for_each_wr) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &i);
  }

  int size = cvector_elem_count(cvec);

  for (int i = 0; i < size; ++i) {
    int *val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void *)val_ptr, NULL);
    *val_ptr += 3;
  }

  for (int i = 0; i < size; ++i) {
    int *val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void *)val_ptr, NULL);
    REQUIRE_EQ(*val_ptr, i + 3);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, for_each_rd) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &i);
  }

  int size = cvector_elem_count(cvec);

  int sum = 0;
  for (int i = 0; i < size; ++i) {
    int *val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void *)val_ptr, NULL);
    sum += *val_ptr;
  }
  REQUIRE_EQ(sum, 6);  // 0 + 1 + 2 + 3

  for (int i = 0; i < 4; ++i) {
    int *val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void *)val_ptr, NULL);
    REQUIRE_EQ(*val_ptr, i);
  }

  cvector_destroy(cvec);
}

extern size_t cvector_get_capacity(cvector *v);
extern const size_t minimum_capacity;
extern const size_t scaling_factor;

TEST(cvectors, scaling) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  for (size_t i = 0; i < minimum_capacity; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), minimum_capacity);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  cvector_push_back(cvec, &(int){minimum_capacity});
  REQUIRE_EQ(cvector_elem_count(cvec), minimum_capacity + 1);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity * scaling_factor);

  int tmp;
  for (size_t i = 0; i <= minimum_capacity; ++i) {
    cvector_pop_back(cvec, &tmp);
  }
  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  cvector_destroy(cvec);
}

TEST(cvectors, scaling_multiple_doublings) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Push 64 elements to trigger multiple capacity doublings:
  // 4->8->16->32->64
  for (int i = 0; i < 64; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), 64);
  REQUIRE_EQ(cvector_get_capacity(cvec), 64);

  // Verify all elements are correct
  for (int i = 0; i < 64; ++i) {
    int *val = cvector_at(cvec, i);
    REQUIRE_NE((void *)val, NULL);
    REQUIRE_EQ(*val, i);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, scaling_shrink_threshold) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Fill to capacity 16
  for (int i = 0; i < 16; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_get_capacity(cvec), 16);

  // Push one more element
  cvector_push_back(cvec, &(int){16});

  // Capacity should be doubled
  REQUIRE_EQ(cvector_get_capacity(cvec), 32);

  int tmp;
  cvector_pop_back(cvec, &tmp);

  // Capacity should remain unchanged
  REQUIRE_EQ(cvector_get_capacity(cvec), 32);

  // Pop 13 more times to reach 3 elements (shrinks 32->16 at count<8, 16->8 at
  // count<4)
  for (int i = 0; i < 13; ++i) {
    cvector_pop_back(cvec, &tmp);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), 3);
  REQUIRE_EQ(cvector_get_capacity(cvec), 8);  // Should halve twice

  cvector_destroy(cvec);
}

TEST(cvectors, data_ptr_access) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 10; ++i) {
    cvector_push_back(cvec, &i);
  }

  int *data = cvector_data_ptr(cvec);
  REQUIRE_NE((void *)data, NULL);

  // Verify all elements through data pointer
  for (int i = 0; i < 10; ++i) {
    REQUIRE_EQ(data[i], i);
  }

  // Modify through data pointer
  data[5] = 999;

  // Verify modification through cvector_at
  int *elem = cvector_at(cvec, 5);
  REQUIRE_EQ(*elem, 999);

  cvector_destroy(cvec);
}

// ========================================================================
// RESERVE TESTS
// ========================================================================

TEST(cvectors, reserve_basic) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  int result = (int)cvector_reserve(cvec, 100);
  REQUIRE_EQ(result, 1);
  REQUIRE_EQ(cvector_get_capacity(cvec), 128);  // Rounded to next power of 2
  REQUIRE_EQ(cvector_elem_count(cvec), 0);      // Count unchanged

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_power_of_two_rounding) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  cvector_reserve(cvec, 7);
  REQUIRE_EQ(cvector_get_capacity(cvec), 8);

  cvector_reserve(cvec, 16);
  REQUIRE_EQ(cvector_get_capacity(cvec), 16);

  cvector_reserve(cvec, 30);
  REQUIRE_EQ(cvector_get_capacity(cvec), 32);

  cvector_reserve(cvec, 65);
  REQUIRE_EQ(cvector_get_capacity(cvec), 128);

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_below_minimum) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  cvector_reserve(cvec, 1);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  cvector_reserve(cvec, 2);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_does_not_shrink) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  cvector_reserve(cvec, 64);
  REQUIRE_EQ(cvector_get_capacity(cvec), 64);

  // Try to reserve smaller capacity
  cvector_reserve(cvec, 16);
  REQUIRE_EQ(cvector_get_capacity(cvec), 64);  // Should not shrink

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_with_existing_elements) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 10; ++i) {
    cvector_push_back(cvec, &i);
  }

  cvector_reserve(cvec, 100);
  REQUIRE_EQ(cvector_elem_count(cvec), 10);

  // Verify elements are preserved
  for (int i = 0; i < 10; ++i) {
    int *val = cvector_at(cvec, i);
    REQUIRE_EQ(*val, i);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_large_capacity) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Reserve 100k elements (400KB) - realistic for testing
  int result = (int)cvector_reserve(cvec, 100000);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_get_capacity(cvec), 131072);  // 2^17

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_exceeds_max) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Try to reserve more than max_elem_count
  int result = (int)cvector_reserve(cvec, max_elem_count + 1);
  REQUIRE_EQ(result, false);

  cvector_destroy(cvec);
}

// ========================================================================
// APPEND ARRAY TESTS
// ========================================================================

TEST(cvectors, append_array_basic) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  int arr[] = {10, 20, 30, 40, 50};
  int result = (int)cvector_append_array(cvec, arr, 5);

  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 5);

  for (int i = 0; i < 5; ++i) {
    int *val = cvector_at(cvec, i);
    REQUIRE_EQ(*val, arr[i]);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_empty) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  int arr[] = {1, 2, 3};
  int result = (int)cvector_append_array(cvec, arr, 0);

  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_to_nonempty) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Add initial elements
  for (int i = 0; i < 3; ++i) {
    cvector_push_back(cvec, &i);
  }

  // Append array
  int arr[] = {10, 11, 12};
  cvector_append_array(cvec, arr, 3);

  REQUIRE_EQ(cvector_elem_count(cvec), 6);

  // Verify original elements
  for (int i = 0; i < 3; ++i) {
    int *val = cvector_at(cvec, i);
    REQUIRE_EQ(*val, i);
  }

  // Verify appended elements
  for (int i = 0; i < 3; ++i) {
    int *val = cvector_at(cvec, i + 3);
    REQUIRE_EQ(*val, arr[i]);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_triggers_expansion) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  size_t initial_capacity = cvector_get_capacity(cvec);

  int arr[20];
  for (int i = 0; i < 20; ++i) {
    arr[i] = i;
  }

  int result = (int)cvector_append_array(cvec, arr, 20);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 20);

  size_t final_capacity = cvector_get_capacity(cvec);
  REQUIRE_NE(final_capacity, initial_capacity);
  REQUIRE_EQ(final_capacity, 32);  // Should expand to accommodate

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_large) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Append 5000 elements (20KB) - realistic size
  int *large_arr = malloc(5000 * sizeof(int));
  for (int i = 0; i < 5000; ++i) {
    large_arr[i] = i;
  }

  int result = (int)cvector_append_array(cvec, large_arr, 5000);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 5000);

  // Spot check some elements
  int *val0 = cvector_at(cvec, 0);
  REQUIRE_EQ(*val0, 0);

  int *val2500 = cvector_at(cvec, 2500);
  REQUIRE_EQ(*val2500, 2500);

  int *val4999 = cvector_at(cvec, 4999);
  REQUIRE_EQ(*val4999, 4999);

  free(large_arr);
  cvector_destroy(cvec);
}

TEST(cvectors, append_array_null_ptr) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  int result = (int)cvector_append_array(cvec, NULL, 3);
  REQUIRE_EQ(result, false);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_overflow_detection) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Try to append SIZE_MAX elements with a valid pointer (should fail due to
  // overflow)
  int dummy[1] = {0};
  int result = (int)cvector_append_array(cvec, dummy, SIZE_MAX);
  REQUIRE_EQ(result, false);

  cvector_destroy(cvec);
}

// ========================================================================
// APPEND CVECTOR TESTS
// ========================================================================

TEST(cvectors, append_cvector_basic) {
  cvector *v1 = cvector_create(sizeof(int), NULL);
  cvector *v2 = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 3; ++i) {
    cvector_push_back(v1, &i);
  }

  for (int i = 10; i < 13; ++i) {
    cvector_push_back(v2, &i);
  }

  int result = (int)cvector_append_cvector(v1, v2);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(v1), 6);
  REQUIRE_EQ(cvector_elem_count(v2), 3);  // v2 unchanged

  // Verify v1 has correct elements
  int *data = cvector_data_ptr(v1);
  REQUIRE_EQ(data[0], 0);
  REQUIRE_EQ(data[2], 2);
  REQUIRE_EQ(data[3], 10);
  REQUIRE_EQ(data[5], 12);

  cvector_destroy(v1);
  cvector_destroy(v2);
}

TEST(cvectors, append_cvector_empty_source) {
  cvector *v1 = cvector_create(sizeof(int), NULL);
  cvector *v2 = cvector_create(sizeof(int), NULL);

  cvector_push_back(v1, &(int){42});

  int result = (int)cvector_append_cvector(v1, v2);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(v1), 1);

  cvector_destroy(v1);
  cvector_destroy(v2);
}

TEST(cvectors, append_cvector_empty_destination) {
  cvector *v1 = cvector_create(sizeof(int), NULL);
  cvector *v2 = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 5; ++i) {
    cvector_push_back(v2, &i);
  }

  int result = (int)cvector_append_cvector(v1, v2);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(v1), 5);

  // Verify elements
  for (int i = 0; i < 5; ++i) {
    int *val = cvector_at(v1, i);
    REQUIRE_EQ(*val, i);
  }

  cvector_destroy(v1);
  cvector_destroy(v2);
}

TEST(cvectors, append_cvector_triggers_expansion) {
  cvector *v1 = cvector_create(sizeof(int), NULL);
  cvector *v2 = cvector_create(sizeof(int), NULL);

  // v1 has 5 elements
  for (int i = 0; i < 5; ++i) {
    cvector_push_back(v1, &i);
  }

  // v2 has 20 elements
  for (int i = 100; i < 120; ++i) {
    cvector_push_back(v2, &i);
  }

  size_t cap_before = cvector_get_capacity(v1);
  int result = (int)cvector_append_cvector(v1, v2);
  size_t cap_after = cvector_get_capacity(v1);

  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(v1), 25);
  REQUIRE_NE(cap_after, cap_before);

  cvector_destroy(v1);
  cvector_destroy(v2);
}

TEST(cvectors, append_cvector_self_append) {
  // No-realloc path: 2 elements, capacity=4, self-append fits without growing.
  {
    cvector *v = cvector_create(sizeof(int), NULL);
    cvector_push_back(v, &(int){1});
    cvector_push_back(v, &(int){2});

    REQUIRE_EQ(cvector_get_capacity(v), minimum_capacity);
    REQUIRE_EQ((int)cvector_append_cvector(v, v), true);
    REQUIRE_EQ(cvector_elem_count(v), 4);
    REQUIRE_EQ(cvector_get_capacity(v), minimum_capacity);  // no realloc

    REQUIRE_EQ(*(int *)cvector_at(v, 0), 1);
    REQUIRE_EQ(*(int *)cvector_at(v, 1), 2);
    REQUIRE_EQ(*(int *)cvector_at(v, 2), 1);
    REQUIRE_EQ(*(int *)cvector_at(v, 3), 2);

    cvector_destroy(v);
  }

  // Realloc path: 3 elements, capacity=4, self-append needs 6 slots -> realloc.
  {
    cvector *v = cvector_create(sizeof(int), NULL);
    cvector_push_back(v, &(int){10});
    cvector_push_back(v, &(int){20});
    cvector_push_back(v, &(int){30});

    REQUIRE_EQ(cvector_get_capacity(v), minimum_capacity);
    REQUIRE_EQ((int)cvector_append_cvector(v, v), true);
    REQUIRE_EQ(cvector_elem_count(v), 6);
    REQUIRE_NE(cvector_get_capacity(v), minimum_capacity);  // grew

    REQUIRE_EQ(*(int *)cvector_at(v, 0), 10);
    REQUIRE_EQ(*(int *)cvector_at(v, 1), 20);
    REQUIRE_EQ(*(int *)cvector_at(v, 2), 30);
    REQUIRE_EQ(*(int *)cvector_at(v, 3), 10);
    REQUIRE_EQ(*(int *)cvector_at(v, 4), 20);
    REQUIRE_EQ(*(int *)cvector_at(v, 5), 30);

    cvector_destroy(v);
  }
}

// ========================================================================
// COMPLEX SCENARIOS
// ========================================================================

TEST(cvectors, push_pop_interleaved_sequence) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Push 5, pop 3, push 4, pop 2
  for (int i = 0; i < 5; ++i) {
    cvector_push_back(cvec, &i);
  }

  int tmp;
  for (int i = 0; i < 3; ++i) {
    cvector_pop_back(cvec, &tmp);
  }

  for (int i = 10; i < 14; ++i) {
    cvector_push_back(cvec, &i);
  }

  for (int i = 0; i < 2; ++i) {
    cvector_pop_back(cvec, &tmp);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), 4);

  int *data = cvector_data_ptr(cvec);
  REQUIRE_EQ(data[0], 0);
  REQUIRE_EQ(data[1], 1);
  REQUIRE_EQ(data[2], 10);
  REQUIRE_EQ(data[3], 11);

  cvector_destroy(cvec);
}

TEST(cvectors, realistic_usage_scenario) {
  // Simulate collecting 3D coordinates
  typedef struct {
    double x, y, z;
  } Point3D;

  cvector *points = cvector_create(sizeof(Point3D), NULL);

  // Reserve space
  cvector_reserve(points, 1000);

  // Add points
  for (int i = 0; i < 500; ++i) {
    Point3D p = {i * 1.0, i * 2.0, i * 3.0};
    cvector_push_back(points, &p);
  }

  REQUIRE_EQ(cvector_elem_count(points), 500);

  // Access and verify
  Point3D *p100 = cvector_at(points, 100);
  REQUIRE_EQ(p100->x, 100.0);
  REQUIRE_EQ(p100->y, 200.0);
  REQUIRE_EQ(p100->z, 300.0);

  // Modify
  Point3D *p200 = cvector_at(points, 200);
  p200->x = 999.0;

  Point3D *p200_check = cvector_at(points, 200);
  REQUIRE_EQ(p200_check->x, 999.0);

  cvector_destroy(points);
}

TEST(cvectors, string_pointer_storage) {
  cvector *cvec = cvector_create(sizeof(char *), NULL);

  char *s1 = strdup("Hello");
  char *s2 = strdup("World");
  char *s3 = strdup("Test");

  cvector_push_back(cvec, &s1);
  cvector_push_back(cvec, &s2);
  cvector_push_back(cvec, &s3);

  REQUIRE_EQ(cvector_elem_count(cvec), 3);

  char **strings = cvector_data_ptr(cvec);
  REQUIRE_EQ(strcmp(strings[0], "Hello"), 0);
  REQUIRE_EQ(strcmp(strings[1], "World"), 0);
  REQUIRE_EQ(strcmp(strings[2], "Test"), 0);

  // Clean up strings
  for (size_t i = 0; i < cvector_elem_count(cvec); ++i) {
    free(strings[i]);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, reset_after_growth) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Grow to large capacity
  for (int i = 0; i < 100; ++i) {
    cvector_push_back(cvec, &i);
  }

  size_t large_capacity = cvector_get_capacity(cvec);
  REQUIRE_NE(large_capacity, minimum_capacity);

  cvector_reset(cvec);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  // Verify vector still works
  cvector_push_back(cvec, &(int){999});
  REQUIRE_EQ(cvector_elem_count(cvec), 1);

  cvector_destroy(cvec);
}

TEST(cvectors, boundary_fill_to_capacity) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Fill to exact minimum capacity
  for (int i = 0; i < (int)minimum_capacity; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), minimum_capacity);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  cvector_destroy(cvec);
}

TEST(cvectors, many_small_operations) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // 500 alternating push/pop operations
  for (int i = 0; i < 500; ++i) {
    cvector_push_back(cvec, &i);

    if (i % 2 == 0 && cvector_elem_count(cvec) > 0) {
      int tmp;
      cvector_pop_back(cvec, &tmp);
    }
  }

  // Should have some elements remaining
  REQUIRE_NE(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

// ========================================================================
// TYPE-SAFE MACRO TESTS
// ========================================================================

TEST(cvectors, constructive_macros) {
  cvec_construct(vec, int);

  int tmp = 2;
  cvec_push(vec, tmp);
  cvec_push_rvalue(vec, 3);

  int val = cvec_pop(vec);
  REQUIRE_EQ(val, 3);  // LIFO
  val = cvec_pop(vec);
  REQUIRE_EQ(val, 2);

  cvec_reset(vec);

  for (int i = 0; i < 10; ++i) {
    cvec_push(vec, i);
    val = cvec_at(vec, i);
    REQUIRE_EQ(val, i);

    cvec_at(vec, i) = i + 1;
    REQUIRE_EQ(cvec_at(vec, i), i + 1);

    cvec_at(vec, i) = i;
  }

  int sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
    cvec_at(vec, i) += 1;
  }
  REQUIRE_EQ(sum, 45);  // 9*(9+1)/2

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 55);  // 10*(10+1)/2

  for (int i = 0; i < 10; ++i) {
    cvec_at(vec, i) = 0;
  }

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 0);  // All elements were zeroed

  cvec_destroy(vec);
}

TEST(cvectors, type_safe_reserve) {
  cvec_construct(vec, int);

  cvec_reserve(vec, 100);
  REQUIRE_EQ(cvector_get_capacity(vec), 128);

  cvec_destroy(vec);
}

TEST(cvectors, type_safe_append_array) {
  cvec_construct(vec, int);

  int arr[] = {1, 2, 3, 4, 5};
  cvec_append_array(vec, arr, 5);

  REQUIRE_EQ(cvec_size(vec), 5);
  REQUIRE_EQ(cvec_at(vec, 0), 1);
  REQUIRE_EQ(cvec_at(vec, 4), 5);

  cvec_destroy(vec);
}

TEST(cvectors, type_safe_append_cvec) {
  cvec_construct(v1, int);
  cvec_construct(v2, int);

  cvec_push_rvalue(v1, 10);
  cvec_push_rvalue(v1, 20);

  cvec_push_rvalue(v2, 30);
  cvec_push_rvalue(v2, 40);

  cvec_append_cvec(v1, v2);

  REQUIRE_EQ(cvec_size(v1), 4);
  REQUIRE_EQ(cvec_at(v1, 0), 10);
  REQUIRE_EQ(cvec_at(v1, 1), 20);
  REQUIRE_EQ(cvec_at(v1, 2), 30);
  REQUIRE_EQ(cvec_at(v1, 3), 40);

  cvec_destroy(v1);
  cvec_destroy(v2);
}

TEST(cvectors, type_safe_data_ptr) {
  cvec_construct(vec, int);

  for (int i = 0; i < 5; ++i) {
    cvec_push_rvalue(vec, i * 10);
  }

  int *data = cvec_data_ptr(vec);
  REQUIRE_NE((void *)data, NULL);

  REQUIRE_EQ(data[0], 0);
  REQUIRE_EQ(data[2], 20);
  REQUIRE_EQ(data[4], 40);

  cvec_destroy(vec);
}

TEST(cvectors, declarative_macros) {
  cvec_declare(vec, int);
  cvec_init(vec);

  int tmp = 2;
  cvec_push(vec, tmp);
  int val = cvec_pop(vec);
  REQUIRE_EQ(val, 2);

  cvec_reset(vec);

  for (int i = 0; i < 10; ++i) {
    cvec_push(vec, i);
    val = cvec_at(vec, i);
    REQUIRE_EQ(val, i);

    cvec_at(vec, i) = i + 1;
    REQUIRE_EQ(cvec_at(vec, i), i + 1);

    cvec_at(vec, i) = i;
  }

  int sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
    cvec_at(vec, i) += 1;
  }
  REQUIRE_EQ(sum, 45);  // 9*(9+1)/2

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 55);  // 10*(10+1)/2

  for (int i = 0; i < 10; ++i) {
    cvec_at(vec, i) = 0;
  }

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 0);  // All elements were zeroed

  cvec_destroy(vec);
}

TEST(cvectors, constructive_macros_mp) {
  cvec_construct_mp(vec, int,
                    (&(ccol_memmgmt_procs_t){.malloc = malloc,
                                             .free = free,
                                             .calloc = calloc,
                                             .realloc = realloc}));

  int tmp = 2;
  cvec_push(vec, tmp);
  int val = cvec_pop(vec);
  REQUIRE_EQ(val, 2);

  cvec_reset(vec);

  for (int i = 0; i < 10; ++i) {
    cvec_push(vec, i);
    val = cvec_at(vec, i);
    REQUIRE_EQ(val, i);

    cvec_at(vec, i) = i + 1;
    REQUIRE_EQ(cvec_at(vec, i), i + 1);

    cvec_at(vec, i) = i;
  }

  int sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
    cvec_at(vec, i) += 1;
  }
  REQUIRE_EQ(sum, 45);  // 9*(9+1)/2

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 55);  // 10*(10+1)/2

  for (int i = 0; i < 10; ++i) {
    cvec_at(vec, i) = 0;
  }

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 0);  // All elements were zeroed

  cvec_destroy(vec);
}

TEST(cvectors, declarative_macros_mp) {
  cvec_declare(vec, int);
  cvec_init_mp(vec, (&(ccol_memmgmt_procs_t){.malloc = malloc,
                                             .free = free,
                                             .calloc = calloc,
                                             .realloc = realloc}));

  int tmp = 2;
  cvec_push(vec, tmp);
  int val = cvec_pop(vec);
  REQUIRE_EQ(val, 2);

  cvec_reset(vec);

  for (int i = 0; i < 10; ++i) {
    cvec_push(vec, i);
    val = cvec_at(vec, i);
    REQUIRE_EQ(val, i);

    cvec_at(vec, i) = i + 1;
    REQUIRE_EQ(cvec_at(vec, i), i + 1);

    cvec_at(vec, i) = i;
  }

  int sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
    cvec_at(vec, i) += 1;
  }
  REQUIRE_EQ(sum, 45);  // 9*(9+1)/2

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 55);  // 10*(10+1)/2

  for (int i = 0; i < 10; ++i) {
    cvec_at(vec, i) = 0;
  }

  sum = 0;
  for (int i = 0; i < 10; ++i) {
    sum += cvec_at(vec, i);
  }
  REQUIRE_EQ(sum, 0);  // All elements were zeroed

  cvec_destroy(vec);
}

TEST(cvectors, construct_scoped_lifecycle) {
  {
    cvec_construct_scoped(v, int);
    REQUIRE_NE((void *)v, NULL);

    for (int i = 1; i <= 5; ++i) {
      cvec_push_rvalue(v, i);
    }
    REQUIRE_EQ(cvector_elem_count(v), 5);
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cvec_at(v, i), i + 1);
    }
    // v is automatically destroyed at end of block (no cvec_destroy needed)
  }
}

// ========================================================================
// ITERATOR TESTS
// ========================================================================

TEST(cvectors, iterator_empty_vector) {
  cvec_construct(vec, int);

  ccol_iter_declare(vec, it);
  it = ccol_begin(vec);
  REQUIRE_EQ((void *)it, NULL);

  cvec_destroy(vec);
}

TEST(cvectors, iterator_single_element) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 99);

  ccol_iter_declare(vec, it);
  it = ccol_begin(vec);
  REQUIRE_NE((void *)it, NULL);

  const size_t *key = ccol_iter_key_ptr(it);
  int *val = ccol_iter_val_ptr(it);
  REQUIRE_EQ(*key, (size_t)0);
  REQUIRE_EQ(*val, 99);

  it = ccol_iter_next(it);
  REQUIRE_EQ((void *)it, NULL);

  cvec_destroy(vec);
}

TEST(cvectors, iterator_full_traversal) {
  cvec_construct(vec, int);
  for (int i = 0; i < 8; ++i) {
    cvec_push_rvalue(vec, i * 5);
  }

  ccol_iter_declare(vec, it);
  int count = 0;
  for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) {
    const size_t *key = ccol_iter_key_ptr(it);
    int *val = ccol_iter_val_ptr(it);
    REQUIRE_EQ(*key, (size_t)count);
    REQUIRE_EQ(*val, count * 5);
    ++count;
  }
  REQUIRE_EQ(count, 8);

  cvec_destroy(vec);
}

TEST(cvectors, iterator_val_mutation) {
  cvec_construct(vec, int);
  for (int i = 0; i < 5; ++i) {
    cvec_push_rvalue(vec, i);
  }

  ccol_iter_declare(vec, it);
  for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) {
    int *val = ccol_iter_val_ptr(it);
    *val *= 3;
  }

  for (int i = 0; i < 5; ++i) {
    REQUIRE_EQ(cvec_at(vec, i), i * 3);
  }

  cvec_destroy(vec);
}

TEST(cvectors, iterator_early_exit) {
  cvec_construct(vec, int);
  for (int i = 0; i < 10; ++i) {
    cvec_push_rvalue(vec, i);
  }

  int count = 0;
  ccol_iter_declare(vec, it);
  for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) {
    ++count;
    if (count == 4) {
      ccol_iter_destroy(it);
      break;
    }
  }
  REQUIRE_EQ(count, 4);
  REQUIRE_EQ((void *)it, NULL);
  REQUIRE_EQ(cvector_elem_count(vec), 10);  // vector unchanged

  cvec_destroy(vec);
}

TEST(cvectors, iterator_for_each_macro) {
  cvec_construct(vec, int);
  for (int i = 1; i <= 6; ++i) {
    cvec_push_rvalue(vec, i);
  }

  int sum = 0;
  ccol_for_each(vec, it, {
    int *val = ccol_iter_val_ptr(it);
    sum += *val;
  });
  REQUIRE_EQ(sum, 21);  // 1+2+3+4+5+6

  cvec_destroy(vec);
}

// ========================================================================
// SORT TESTS
// ========================================================================

static int cmp_int_descending(const void *a, const void *b) {
  int va = *(const int *)a;
  int vb = *(const int *)b;
  return (va < vb) - (va > vb);
}

typedef struct {
  int key;
  int seq;
} cvec_stable_item;

static int cmp_stable_item(const void *a, const void *b) {
  int ka = ((const cvec_stable_item *)a)->key;
  int kb = ((const cvec_stable_item *)b)->key;
  return (ka > kb) - (ka < kb);
}

TEST(cvectors, sort_empty_vector) {
  cvec_construct(vec, int);
  cvec_sort(vec);  // must not crash on empty input
  REQUIRE_EQ(cvec_size(vec), (size_t)0);
  cvec_destroy(vec);
}

TEST(cvectors, sort_single_element) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 42);
  cvec_sort(vec);
  REQUIRE_EQ(cvec_size(vec), (size_t)1);
  REQUIRE_EQ(cvec_at(vec, 0), 42);
  cvec_destroy(vec);
}

TEST(cvectors, sort_ascending) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 5);
  cvec_push_rvalue(vec, 3);
  cvec_push_rvalue(vec, 1);
  cvec_push_rvalue(vec, 4);
  cvec_push_rvalue(vec, 2);

  cvec_sort(vec);

  for (int i = 0; i < 5; ++i) {
    REQUIRE_EQ(cvec_at(vec, i), i + 1);
  }

  cvec_destroy(vec);
}

TEST(cvectors, sort_already_sorted) {
  cvec_construct(vec, int);
  for (int i = 0; i < 8; ++i) {
    cvec_push_rvalue(vec, i);
  }

  cvec_sort(vec);

  for (int i = 0; i < 8; ++i) {
    REQUIRE_EQ(cvec_at(vec, i), i);
  }

  cvec_destroy(vec);
}

TEST(cvectors, sort_custom_comparator_descending) {
  cvec_construct(vec, int);
  for (int i = 1; i <= 6; ++i) {
    cvec_push_rvalue(vec, i);
  }

  cvector_sort_with_comparison_proc(vec, cmp_int_descending);

  for (int i = 0; i < 6; ++i) {
    REQUIRE_EQ(cvec_at(vec, i), 6 - i);
  }

  cvec_destroy(vec);
}

TEST(cvectors, sort_doubles) {
  cvec_construct(vec, double);
  cvec_push_rvalue(vec, 3.0);
  cvec_push_rvalue(vec, 1.0);
  cvec_push_rvalue(vec, 4.0);
  cvec_push_rvalue(vec, 2.0);

  cvec_sort(vec);

  REQUIRE_EQ(cvec_at(vec, 0), 1.0);
  REQUIRE_EQ(cvec_at(vec, 1), 2.0);
  REQUIRE_EQ(cvec_at(vec, 2), 3.0);
  REQUIRE_EQ(cvec_at(vec, 3), 4.0);

  cvec_destroy(vec);
}

TEST(cvectors, sort_stable) {
  cvec_declare(vec, cvec_stable_item);
  cvec_init(vec);

  // Interleaved insertion: key=2/seq=0, key=1/seq=0, key=2/seq=1, ...
  cvec_push(vec, ((cvec_stable_item){2, 0}));
  cvec_push(vec, ((cvec_stable_item){1, 0}));
  cvec_push(vec, ((cvec_stable_item){2, 1}));
  cvec_push(vec, ((cvec_stable_item){1, 1}));
  cvec_push(vec, ((cvec_stable_item){2, 2}));
  cvec_push(vec, ((cvec_stable_item){1, 2}));

  cvector_sort_with_comparison_proc(vec, cmp_stable_item);

  // key=1 group must appear first, in original insertion order (seq 0,1,2)
  REQUIRE_EQ(cvec_at(vec, 0).key, 1);
  REQUIRE_EQ(cvec_at(vec, 0).seq, 0);
  REQUIRE_EQ(cvec_at(vec, 1).key, 1);
  REQUIRE_EQ(cvec_at(vec, 1).seq, 1);
  REQUIRE_EQ(cvec_at(vec, 2).key, 1);
  REQUIRE_EQ(cvec_at(vec, 2).seq, 2);
  // key=2 group follows, in original insertion order
  REQUIRE_EQ(cvec_at(vec, 3).key, 2);
  REQUIRE_EQ(cvec_at(vec, 3).seq, 0);
  REQUIRE_EQ(cvec_at(vec, 4).key, 2);
  REQUIRE_EQ(cvec_at(vec, 4).seq, 1);
  REQUIRE_EQ(cvec_at(vec, 5).key, 2);
  REQUIRE_EQ(cvec_at(vec, 5).seq, 2);

  cvec_destroy(vec);
}

// C_VECTOR FIND TESTS

TEST(cvectors, find_empty_vector) {
  cvec_construct(vec, int);
  REQUIRE_EQ(cvec_find(vec, 42), ccol_invalid_size);
  REQUIRE_EQ(cvector_find(vec, &(int){42}, NULL), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_null_elem) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 1);
  REQUIRE_EQ(cvector_find(vec, NULL, NULL), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_single_element_match) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 7);
  REQUIRE_EQ(cvec_find(vec, 7), (size_t)0);
  cvec_destroy(vec);
}

TEST(cvectors, find_single_element_no_match) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 7);
  REQUIRE_EQ(cvec_find(vec, 99), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_first_element) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 10);
  cvec_push_rvalue(vec, 20);
  cvec_push_rvalue(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 10), (size_t)0);
  cvec_destroy(vec);
}

TEST(cvectors, find_middle_element) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 10);
  cvec_push_rvalue(vec, 20);
  cvec_push_rvalue(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 20), (size_t)1);
  cvec_destroy(vec);
}

TEST(cvectors, find_last_element) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 10);
  cvec_push_rvalue(vec, 20);
  cvec_push_rvalue(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 30), (size_t)2);
  cvec_destroy(vec);
}

TEST(cvectors, find_not_found) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 10);
  cvec_push_rvalue(vec, 20);
  cvec_push_rvalue(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 99), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_returns_first_occurrence) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 5);
  cvec_push_rvalue(vec, 10);
  cvec_push_rvalue(vec, 5);
  cvec_push_rvalue(vec, 10);
  // Both 5 and 10 appear twice; must return the first index
  REQUIRE_EQ(cvec_find(vec, 5), (size_t)0);
  REQUIRE_EQ(cvec_find(vec, 10), (size_t)1);
  cvec_destroy(vec);
}

int helper_cmp_int(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

TEST(cvectors, find_with_custom_comparator) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 100);
  cvec_push_rvalue(vec, 200);
  cvec_push_rvalue(vec, 300);
  REQUIRE_EQ(cvector_find(vec, &(int){200}, helper_cmp_int), (size_t)1);
  REQUIRE_EQ(cvector_find(vec, &(int){999}, helper_cmp_int), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_with_double_type) {
  cvec_construct(vec, double);
  cvec_push_rvalue(vec, 1.5);
  cvec_push_rvalue(vec, 2.5);
  cvec_push_rvalue(vec, 3.5);
  REQUIRE_EQ(cvec_find(vec, 2.5), (size_t)1);
  REQUIRE_EQ(cvec_find(vec, 9.9), ccol_invalid_size);
  cvec_destroy(vec);
}

typedef struct {
  int x;
  int y;
} helper_point_t;

int helper_cmp_point(const void *a, const void *b) {
  const helper_point_t *pa = (const helper_point_t *)a;
  const helper_point_t *pb = (const helper_point_t *)b;
  if (pa->x != pb->x) return pa->x - pb->x;
  return pa->y - pb->y;
}

TEST(cvectors, find_with_struct_type) {
  cvec_construct(vec, helper_point_t);
  cvec_push(vec, ((helper_point_t){1, 2}));
  cvec_push(vec, ((helper_point_t){3, 4}));
  cvec_push(vec, ((helper_point_t){5, 6}));
  REQUIRE_EQ(cvector_find(vec, &(helper_point_t){3, 4}, helper_cmp_point),
             (size_t)1);
  REQUIRE_EQ(cvector_find(vec, &(helper_point_t){9, 9}, helper_cmp_point),
             ccol_invalid_size);
  cvec_destroy(vec);
}

// ========================================================================
// MACRO VARIANT TESTS
// ========================================================================

TEST(cvectors, redeclare_macro) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 10);
  cvec_push_rvalue(vec, 20);
  cvec_push_rvalue(vec, 30);

  {
    cvec_redeclare(vec, int);
    REQUIRE_EQ(cvec_size(vec), (size_t)3);
    REQUIRE_EQ(cvec_at(vec, 0), 10);
    REQUIRE_EQ(cvec_at(vec, 1), 20);
    REQUIRE_EQ(cvec_at(vec, 2), 30);

    cvec_push_rvalue(vec, 40);
    REQUIRE_EQ(cvec_size(vec), (size_t)4);
    REQUIRE_EQ(cvec_at(vec, 3), 40);
  }

  cvec_destroy(vec);
}

TEST(cvectors, declare_scoped_lifecycle) {
  {
    cvec_declare_scoped(v, int);
    cvec_init(v);
    REQUIRE_NE((void *)v, NULL);

    for (int i = 1; i <= 5; ++i) {
      cvec_push_rvalue(v, i);
    }
    REQUIRE_EQ(cvector_elem_count(v), 5);
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cvec_at(v, i), i + 1);
    }
    // v is automatically destroyed at end of block (no cvec_destroy needed)
  }
}

TEST(cvectors, construct_mp_scoped_lifecycle) {
  {
    cvec_construct_mp_scoped(v, int,
                             (&(ccol_memmgmt_procs_t){.malloc = malloc,
                                                      .free = free,
                                                      .calloc = calloc,
                                                      .realloc = realloc}));
    REQUIRE_NE((void *)v, NULL);

    for (int i = 1; i <= 5; ++i) {
      cvec_push_rvalue(v, i);
    }
    REQUIRE_EQ(cvector_elem_count(v), 5);
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cvec_at(v, i), i + 1);
    }
    // v is automatically destroyed at end of block (no cvec_destroy needed)
  }
}

TEST(cvectors, data_ptr_empty_vector) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  void *ptr = cvector_data_ptr(cvec);
  REQUIRE_NE(ptr, NULL);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

// ========================================================================
// ITERATOR RAII TESTS
// ========================================================================

TEST(cvectors, iterator_raii_scope_exit) {
  cvec_construct(vec, int);
  for (int i = 0; i < 5; ++i) {
    cvec_push_rvalue(vec, i);
  }

  int count = 0;
  {
    ccol_iter_declare(vec, it);
    for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) {
      ++count;
      if (count == 2) {
        break;  // exit without explicit ccol_iter_destroy — RAII must clean up
      }
    }
    // 'it' is still live here; the RAII destructor on 'it' fires at '}'
  }
  REQUIRE_EQ(count, 2);
  REQUIRE_EQ(cvector_elem_count(vec), 5);  // vector is unchanged

  cvec_destroy(vec);
}
