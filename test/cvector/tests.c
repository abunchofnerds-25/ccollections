#include <cvector.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
TAU_MAIN()  // sets up Tau (+ main function)

// C_VECTOR TESTS

TEST(cvectors, create_fails) {
  char* err_str = NULL;
  cvector* cvec = cvector_create(0, &err_str);
  REQUIRE_EQ((void*)cvec, NULL);
  REQUIRE_NE((void*)err_str, NULL);

  cvec = cvector_create_with_mprocs(
      0,
      &(ccol_memmgmt_procs_t){
          .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void*)cvec, NULL);
  REQUIRE_NE((void*)err_str, NULL);

  cvec = cvector_create_with_mprocs(
      0,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void*)cvec, NULL);
  REQUIRE_NE((void*)err_str, NULL);

  cvec = cvector_create_with_mprocs(
      0,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void*)cvec, NULL);
  REQUIRE_NE((void*)err_str, NULL);

  cvec = cvector_create_with_mprocs(
      0,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void*)cvec, NULL);
  REQUIRE_NE((void*)err_str, NULL);
}

TEST(cvectors, create_succeeds) {
  char* err_str = NULL;
  cvector* cvec = cvector_create(sizeof(int), &err_str);
  REQUIRE_NE((void*)cvec, NULL);
  REQUIRE_EQ((void*)err_str, NULL);
  cvector_destroy(cvec);
  REQUIRE_EQ((void*)cvec, NULL);

  cvec = cvector_create_with_mprocs(
      sizeof(int),
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void*)cvec, NULL);
  REQUIRE_EQ((void*)err_str, NULL);
  cvector_destroy(cvec);
  REQUIRE_EQ((void*)cvec, NULL);
}

TEST(cvectors, simple_push_backs) {
  cvector* cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_push_back(cvec, &(int){1}), ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 1);

  REQUIRE_EQ(cvector_push_back(cvec, &(int){1}), ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 2);

  cvector_destroy(cvec);
}

TEST(cvectors, simple_push_backs_with_memmgmt_procs) {
  cvector* cvec = cvector_create_with_mprocs(
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

TEST(cvectors, access_an_index) {
  cvector* cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_at(cvec, 0), NULL);
  REQUIRE_EQ(cvector_at(cvec, 1), NULL);
  REQUIRE_EQ(cvector_at(cvec, (uint32_t)-1), NULL);

  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &(int){i + 1});
  }

  for (int i = 0; i < 4; ++i) {
    int* var_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void*)var_ptr, NULL);
    REQUIRE_EQ(*var_ptr, i + 1);
  }

  REQUIRE_EQ(cvector_at(cvec, 4), NULL);
  REQUIRE_EQ(cvector_at(cvec, (uint32_t)-1), NULL);

  cvector_destroy(cvec);
}

TEST(cvectors, simple_pop_backs) {
  cvector* cvec = cvector_create(sizeof(int), NULL);

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

TEST(cvectors, different_sizes) {
  {
    cvector* cvec = cvector_create(sizeof(long), NULL);

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
    cvector* cvec = cvector_create(sizeof(char), NULL);

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
    cvector* cvec = cvector_create(sizeof(int16_t), NULL);

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

TEST(cvectors, reset) {
  cvector* cvec = cvector_create(sizeof(int), NULL);

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
  cvector* cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &i);
  }

  int size = cvector_elem_count(cvec);

  for (int i = 0; i < size; ++i) {
    int* val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void*)val_ptr, NULL);
    *val_ptr += 3;
  }

  for (int i = 0; i < size; ++i) {
    int* val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void*)val_ptr, NULL);
    REQUIRE_EQ(*val_ptr, i + 3);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, for_each_rd) {
  cvector* cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &i);
  }

  int size = cvector_elem_count(cvec);

  int sum = 0;
  for (int i = 0; i < size; ++i) {
    int* val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void*)val_ptr, NULL);
    sum += *val_ptr;
  }
  REQUIRE_EQ(sum, 6);  // 0 + 1 + 2 + 3

  for (int i = 0; i < 4; ++i) {
    int* val_ptr = cvector_at(cvec, i);
    REQUIRE_NE((void*)val_ptr, NULL);
    REQUIRE_EQ(*val_ptr, i);
  }

  cvector_destroy(cvec);
}

extern uint32_t cvector_get_capacity(cvector* v);
extern const uint32_t minimum_capacity;
extern const uint32_t scaling_factor;

TEST(cvectors, scaling) {
  cvector* cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  for (uint32_t i = 0; i < minimum_capacity; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), minimum_capacity);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity * scaling_factor);

  int tmp;
  for (uint32_t i = 0; i < minimum_capacity; ++i) {
    cvector_pop_back(cvec, &tmp);
  }
  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  cvector_destroy(cvec);
}

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

  cvec_destruct(vec);
}

TEST(cvectors, declarative_macros) {
  cvec_declare(vec, int);
  cvec_init(vec);

  int tmp = 2;
  cvec_push(vec, tmp);
  int val = cvec_pop(vec);

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

  cvec_destruct(vec);
}

TEST(cvectors, constructive_macros_with_mprocs) {
  cvec_construct_with_mprocs(vec, int,
                             (&(ccol_memmgmt_procs_t){.malloc = malloc,
                                                      .free = free,
                                                      .calloc = calloc,
                                                      .realloc = realloc}));

  int tmp = 2;
  cvec_push(vec, tmp);
  int val = cvec_pop(vec);

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

  cvec_destruct(vec);
}

TEST(cvectors, declarative_macros_with_mprocs) {
  cvec_declare(vec, int);
  cvec_init_with_mprocs(vec, (&(ccol_memmgmt_procs_t){.malloc = malloc,
                                                      .free = free,
                                                      .calloc = calloc,
                                                      .realloc = realloc}));

  int tmp = 2;
  cvec_push(vec, tmp);
  int val = cvec_pop(vec);

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

  cvec_destruct(vec);
}
