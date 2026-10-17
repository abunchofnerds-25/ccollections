#include <common_invariants.h>
#include <cvector.h>
#include <fcntl.h>
#include <internal/test_helpers/cvector.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <unistd.h>
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

// The first allocation of the backing buffer holds 4 elements, so its byte
// count is _ccol_cvector_minimum_capacity * elem_size. An elem_size big
// enough to overflow size_t there must be rejected at once, or the product
// silently wraps to a small allocation while v->elem_size records the real,
// huge value.
TEST(cvectors, create_fails_on_elem_size_overflow) {
  char *err_str = NULL;
  cvector *cvec = cvector_create(SIZE_MAX, &err_str);
  REQUIRE_EQ((void *)cvec, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  err_str = NULL;
  cvec = cvector_create(SIZE_MAX / 4 + 1, &err_str);  // one past the boundary
  REQUIRE_EQ((void *)cvec, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

// A large elem_size that does NOT overflow the initial capacity allocation
// is accepted, so the overflow guard isn't stricter than it should be.
TEST(cvectors, create_succeeds_with_large_elem_size) {
  char *err_str = NULL;
  size_t large_elem_size = 1024 * 1024;  // 1 MiB per element
  cvector *cvec = cvector_create(large_elem_size, &err_str);
  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);
  cvector_destroy(cvec);
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

TEST(cvectors, get_mprocs) {
  cvector *default_cvec = cvector_create(sizeof(int), NULL);
  REQUIRE_EQ((void *)cvector_get_mprocs(default_cvec), NULL);
  cvector_destroy(default_cvec);

  ccol_memmgmt_procs_t custom = {
      .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
  cvector *custom_cvec = cvector_create_full(sizeof(int), &custom, NULL);
  ccol_memmgmt_procs_t *stored = cvector_get_mprocs(custom_cvec);
  REQUIRE_NE((void *)stored, NULL);
  // The vector keeps its own copy of the procs struct in its own block
  // instead of the caller's pointer, but the function pointers in that copy
  // match the ones from the creation call.
  REQUIRE_NE((void *)stored, (void *)&custom);
  REQUIRE_TRUE(stored->malloc == custom.malloc);
  REQUIRE_TRUE(stored->free == custom.free);
  REQUIRE_TRUE(stored->calloc == custom.calloc);
  REQUIRE_TRUE(stored->realloc == custom.realloc);
  cvector_destroy(custom_cvec);
}

// cvector_get_mprocs(NULL) must call _ccol_fatal_err(), which asserts,
// instead of dereferencing a NULL vector. Every other accessor of this
// module does the same: cvector_at, cvector_elem_count, cvector_reset,
// cvector_data_ptr, cvector_find, cvector_push_back, cvector_pop_back and
// cvector_reserve. The test runs in a forked child because the assertion
// stops the whole process, the same shape as
// type_safe_at_out_of_bounds_is_fatal.
TEST(cvectors, get_mprocs_null_vec_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    (void)cvector_get_mprocs(NULL); /* NULL vector; must _ccol_fatal_err() */
    _exit(0); /* unreachable if _ccol_fatal_err() aborted */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
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

// ========================================================================
// PUSH BACK SELF-ALIAS TESTS
// ========================================================================

// When new_elem aliases v's own backing buffer it must stay safe, even when
// scale_the_cvector_size_up moves the buffer to a new address mid-call.
TEST(cvectors, push_back_self_alias_no_realloc) {
  // 3 elements, capacity=4: pushing an existing element back onto itself
  // fits without growing, so this exercises the no-realloc path, where
  // new_elem needs no fix-up at all.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){10});
  cvector_push_back(cvec, &(int){20});
  cvector_push_back(cvec, &(int){30});

  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);
  int *elem_ptr = cvector_at(cvec, 1);  // points at element {20}
  ccol_retval_t result = cvector_push_back(cvec, elem_ptr);
  REQUIRE_EQ(result, ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 4);
  REQUIRE_EQ(cvector_get_capacity(cvec),
             _ccol_cvector_minimum_capacity);  // no realloc

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 10);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 20);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 30);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 20);

  cvector_destroy(cvec);
}

TEST(cvectors, push_back_self_alias_triggers_expansion) {
  // The vector holds 4 elements with a capacity of 4, so it's exactly full,
  // and pushing one of its own elements back onto it forces
  // scale_the_cvector_size_up to call realloc. Without a guard, new_elem
  // points into the vector's own buffer, so it dangles as soon as realloc
  // moves that buffer and the copy after that reads freed
  // memory.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &(int){(i + 1) * 10});
  }

  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);
  int *elem_ptr = cvector_at(cvec, 3);  // points at element {40}
  ccol_retval_t result = cvector_push_back(cvec, elem_ptr);
  REQUIRE_EQ(result, ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 5);
  REQUIRE_NE(cvector_get_capacity(cvec),
             _ccol_cvector_minimum_capacity);  // grew

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 10);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 20);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 30);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 40);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 4), 40);

  cvector_destroy(cvec);
}

TEST(cvectors, push_back_self_alias_via_type_safe_macro) {
  // cvec_push can hit the same hazard, since it just takes the address of
  // its (possibly vector-derived) lvalue argument.
  cvec_construct(vec, int);
  for (int i = 0; i < 4; ++i) {
    cvec_push(vec, (i + 1) * 100);
  }
  REQUIRE_EQ(cvector_get_capacity(vec), _ccol_cvector_minimum_capacity);

  cvec_push(vec, cvec_at(vec, 0));  // aliases the vector's own storage
  REQUIRE_EQ(cvec_size(vec), (size_t)5);
  REQUIRE_NE(cvector_get_capacity(vec),
             _ccol_cvector_minimum_capacity);  // grew
  REQUIRE_EQ(cvec_at(vec, 4), 100);

  cvec_destroy(vec);
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

TEST(cvectors, scaling) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  for (size_t i = 0; i < _ccol_cvector_minimum_capacity; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), _ccol_cvector_minimum_capacity);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  cvector_push_back(cvec, &(int){_ccol_cvector_minimum_capacity});
  REQUIRE_EQ(cvector_elem_count(cvec), _ccol_cvector_minimum_capacity + 1);
  REQUIRE_EQ(cvector_get_capacity(cvec),
             _ccol_cvector_minimum_capacity * _ccol_cvector_scaling_factor);

  int tmp;
  for (size_t i = 0; i <= _ccol_cvector_minimum_capacity; ++i) {
    cvector_pop_back(cvec, &tmp);
  }
  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  cvector_destroy(cvec);
}

TEST(cvectors, scaling_multiple_doublings) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Push 64 elements, which doubles the capacity several times:
  // 4->8->16->32->64
  for (int i = 0; i < 64; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), 64);
  REQUIRE_EQ(cvector_get_capacity(cvec), 64);

  // Check that every element is right
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

  // The capacity doubles
  REQUIRE_EQ(cvector_get_capacity(cvec), 32);

  int tmp;
  cvector_pop_back(cvec, &tmp);

  // The capacity should stay the same
  REQUIRE_EQ(cvector_get_capacity(cvec), 32);

  // Pop 13 more times to get to 3 elements (it shrinks 32->16 at count<8 and
  // 16->8 at count<4)
  for (int i = 0; i < 13; ++i) {
    cvector_pop_back(cvec, &tmp);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), 3);
  REQUIRE_EQ(cvector_get_capacity(cvec), 8);  // halved twice

  cvector_destroy(cvec);
}

TEST(cvectors, data_ptr_access) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 10; ++i) {
    cvector_push_back(cvec, &i);
  }

  int *data = cvector_data_ptr(cvec);
  REQUIRE_NE((void *)data, NULL);

  // Check every element through the data pointer
  for (int i = 0; i < 10; ++i) {
    REQUIRE_EQ(data[i], i);
  }

  // Modify through data pointer
  data[5] = 999;

  // and check the change through cvector_at
  int *elem = cvector_at(cvec, 5);
  REQUIRE_EQ(*elem, 999);

  cvector_destroy(cvec);
}

// ========================================================================
// RESERVE TESTS
// ========================================================================

TEST(cvectors, reserve_basic) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  int result = (int)cvector_reserve(cvec, 100);
  REQUIRE_EQ(result, 1);
  REQUIRE_EQ(cvector_get_capacity(cvec), 128);  // rounded up to a power of 2
  REQUIRE_EQ(cvector_elem_count(cvec), 0);      // the count is unchanged

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
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  cvector_reserve(cvec, 2);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_does_not_shrink) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  cvector_reserve(cvec, 64);
  REQUIRE_EQ(cvector_get_capacity(cvec), 64);

  // Try to reserve a smaller capacity
  cvector_reserve(cvec, 16);
  REQUIRE_EQ(cvector_get_capacity(cvec), 64);  // it must not shrink

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_with_existing_elements) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  for (int i = 0; i < 10; ++i) {
    cvector_push_back(cvec, &i);
  }

  cvector_reserve(cvec, 100);
  REQUIRE_EQ(cvector_elem_count(cvec), 10);

  // The elements are preserved
  for (int i = 0; i < 10; ++i) {
    int *val = cvector_at(cvec, i);
    REQUIRE_EQ(*val, i);
  }

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_large_capacity) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Reserve 100k elements (400KB), a realistic size for a test
  int result = (int)cvector_reserve(cvec, 100000);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_get_capacity(cvec), 131072);  // 2^17

  cvector_destroy(cvec);
}

TEST(cvectors, reserve_exceeds_max) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Try to reserve more than ccol_max_elem_count
  int result = (int)cvector_reserve(cvec, ccol_max_elem_count + 1);
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
  REQUIRE_EQ(final_capacity, 32);  // it grows to make room

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_large) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Append 5000 elements (20KB), a realistic size
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

  // Try to append SIZE_MAX elements with a valid pointer. Expect a failure
  // because the byte count overflows.
  int dummy[1] = {0};
  int result = (int)cvector_append_array(cvec, dummy, SIZE_MAX);
  REQUIRE_EQ(result, false);

  cvector_destroy(cvec);
}

// When arr_ptr aliases v's own buffer it needs to stay safe, even when
// cvector_reserve moves the buffer to a new address mid-call.
TEST(cvectors, append_array_self_alias_no_realloc) {
  // 2 elements, capacity=4: appending the whole buffer back onto itself
  // fits without growing, so this exercises the no-realloc path, where
  // arr_ptr needs no fix-up at all.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){1});
  cvector_push_back(cvec, &(int){2});

  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);
  void *arr_ptr = cvector_data_ptr(cvec);
  size_t overlap_count_before = cvector_overlap_copy_count_for_tests;
  int result = (int)cvector_append_array(cvec, arr_ptr, 2);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 4);
  REQUIRE_EQ(cvector_get_capacity(cvec),
             _ccol_cvector_minimum_capacity);  // no realloc
  // The source range ends exactly where the destination begins (touching,
  // not overlapping), so the cheaper memcpy path should be used here and not
  // memmove.
  REQUIRE_EQ(cvector_overlap_copy_count_for_tests, overlap_count_before);

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 2);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 2);

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_self_alias_triggers_expansion) {
  // 3 elements, capacity=4: appending the whole buffer back onto itself
  // needs 6 slots and forces cvector_reserve to realloc. Without a guard
  // arr_ptr (== cvector_data_ptr(cvec)) dangles as soon as that realloc
  // moves the buffer, and the second copy reads freed memory.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){10});
  cvector_push_back(cvec, &(int){20});
  cvector_push_back(cvec, &(int){30});

  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);
  void *arr_ptr = cvector_data_ptr(cvec);
  int result = (int)cvector_append_array(cvec, arr_ptr, 3);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 6);
  REQUIRE_NE(cvector_get_capacity(cvec),
             _ccol_cvector_minimum_capacity);  // grew

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 10);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 20);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 30);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 10);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 4), 20);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 5), 30);

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_partial_self_alias_triggers_expansion) {
  // 4 elements, capacity=4: arr_ptr points partway into the buffer (not at
  // its start), so this covers the logic that keeps the byte offset, apart
  // from the whole-buffer aliasing case above.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);
  int *arr_ptr =
      (int *)cvector_data_ptr(cvec) + 2;  // points at elements {2, 3}
  int result = (int)cvector_append_array(cvec, arr_ptr, 2);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 6);
  REQUIRE_NE(cvector_get_capacity(cvec),
             _ccol_cvector_minimum_capacity);  // grew

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 0);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 2);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 3);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 4), 2);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 5), 3);

  cvector_destroy(cvec);
}

// arr_ptr can alias v's buffer at an offset where the requested elem_count
// reads past the vector's *live* elem_count, into capacity that is reserved
// but not live yet, so the source byte range and the destination byte range
// really overlap. That's different from every self-alias test above, where
// the source range always ends exactly where the destination begins. Neither
// a plain memcpy nor a simple forward byte copy is safe for such an overlap,
// because writing the first appended element can destroy source bytes that
// a later appended element needs. cvector_copy_into_tail must detect it
// and use a memmove, which handles the overlap.
//
// The two tests below use a large element count with a shift of only ONE
// ELEMENT between the source and destination ranges, so nearly the whole
// copy overlaps itself. The size and the shift matter, they aren't
// incidental: a smaller shift and size drive exactly the same code path but
// can pass with the overlap check turned off. An overlap is undefined
// behavior, not behavior that is guaranteed to go wrong, so at that size
// and shift a given memcpy may just happen not to corrupt a few small
// elements. A huge total size with a one-element shift is the classic
// "shift an array by one in place with a memcpy instead of a memmove"
// pattern: an implementation that copies in pieces smaller than the whole
// buffer can't get it right without memmove's overlap handling, and every
// real memcpy copies in such pieces, by design, for anything longer than a
// few bytes. So the test reproduces reliably whatever memcpy and whatever
// compiler the build uses.
TEST(cvectors, append_array_self_alias_overlap_shift_by_one_no_realloc) {
  const int n = 10000;
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){0});
  cvector_push_back(cvec, &(int){1});
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)2);

  // Grow the capacity, leave elem_count alone, and fill the spare region with
  // deterministic values directly through the raw data pointer. That region
  // is allocated but not live yet, and this bypasses push_back completely,
  // so no shrink logic ever runs.
  REQUIRE_TRUE(cvector_reserve(cvec, (size_t)n));
  int *base = cvector_data_ptr(cvec);
  for (int i = 2; i < n; ++i) {
    base[i] = i;
  }

  // Reads indices [1, n-1) (values 1..n-2) and writes indices [2, n). Source
  // and destination overlap everywhere except the very first source element
  // and the very last destination slot.
  size_t overlap_count_before = cvector_overlap_copy_count_for_tests;
  int result = (int)cvector_append_array(cvec, base + 1, (size_t)(n - 2));
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)n);
  // This is the whole point of the test: it checks that the code took the
  // memmove branch, which handles the overlap, instead of trusting only the
  // output of the copy, because nothing forces a real memcpy to corrupt an
  // overlapping copy, a memcpy is only allowed to corrupt one.
  REQUIRE_GT(cvector_overlap_copy_count_for_tests, overlap_count_before);

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 0);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 1);
  for (int k = 2; k < n; ++k) {
    REQUIRE_EQ(*(int *)cvector_at(cvec, k), k - 1);
  }

  cvector_destroy(cvec);
}

TEST(cvectors,
     append_array_self_alias_overlap_shift_by_one_triggers_expansion) {
  const int cap = 8192;
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){0});
  cvector_push_back(cvec, &(int){1});
  REQUIRE_TRUE(cvector_reserve(cvec, (size_t)cap));
  REQUIRE_EQ(cvector_get_capacity(cvec), (size_t)cap);

  int *base = cvector_data_ptr(cvec);
  for (int i = 2; i < cap; ++i) {
    base[i] = i;
  }

  // count = cap - 1 elements starting at index 1: new_elem_count = 2 +
  // (cap - 1) = cap + 1 > capacity, which forces cvector_reserve to realloc.
  // The source range [1, cap) and the destination range [2, cap+1) overlap
  // almost completely, with a one-element shift. The copy has to get both
  // right: the realloc, after which the code recomputes arr_ptr against the
  // new buffer, and the overlap.
  size_t overlap_count_before = cvector_overlap_copy_count_for_tests;
  int result = (int)cvector_append_array(cvec, base + 1, (size_t)(cap - 1));
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)(cap + 1));
  REQUIRE_NE(cvector_get_capacity(cvec), (size_t)cap);  // grew
  REQUIRE_GT(cvector_overlap_copy_count_for_tests, overlap_count_before);

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 0);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 1);
  for (int k = 2; k <= cap; ++k) {
    REQUIRE_EQ(*(int *)cvector_at(cvec, k), k - 1);
  }

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

  // Check that v1 has the right elements
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

  // Check the elements
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
  // No-realloc path: 2 elements, capacity=4, the self-append fits as it is.
  {
    cvector *v = cvector_create(sizeof(int), NULL);
    cvector_push_back(v, &(int){1});
    cvector_push_back(v, &(int){2});

    REQUIRE_EQ(cvector_get_capacity(v), _ccol_cvector_minimum_capacity);
    REQUIRE_EQ((int)cvector_append_cvector(v, v), true);
    REQUIRE_EQ(cvector_elem_count(v), 4);
    REQUIRE_EQ(cvector_get_capacity(v),
               _ccol_cvector_minimum_capacity);  // no realloc

    REQUIRE_EQ(*(int *)cvector_at(v, 0), 1);
    REQUIRE_EQ(*(int *)cvector_at(v, 1), 2);
    REQUIRE_EQ(*(int *)cvector_at(v, 2), 1);
    REQUIRE_EQ(*(int *)cvector_at(v, 3), 2);

    cvector_destroy(v);
  }

  // Realloc path: 3 elements, capacity=4, the self-append needs 6 slots.
  {
    cvector *v = cvector_create(sizeof(int), NULL);
    cvector_push_back(v, &(int){10});
    cvector_push_back(v, &(int){20});
    cvector_push_back(v, &(int){30});

    REQUIRE_EQ(cvector_get_capacity(v), _ccol_cvector_minimum_capacity);
    REQUIRE_EQ((int)cvector_append_cvector(v, v), true);
    REQUIRE_EQ(cvector_elem_count(v), 6);
    REQUIRE_NE(cvector_get_capacity(v),
               _ccol_cvector_minimum_capacity);  // grew

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
  // Collect some 3D coordinates
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

  // Access and check them
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

  // Grow to a large capacity
  for (int i = 0; i < 100; ++i) {
    cvector_push_back(cvec, &i);
  }

  size_t large_capacity = cvector_get_capacity(cvec);
  REQUIRE_NE(large_capacity, _ccol_cvector_minimum_capacity);

  cvector_reset(cvec);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  // The vector keeps working
  cvector_push_back(cvec, &(int){999});
  REQUIRE_EQ(cvector_elem_count(cvec), 1);

  cvector_destroy(cvec);
}

static size_t g_reset_test_realloc_count = 0;

static void *_reset_test_malloc(size_t size) { return malloc(size); }
static void *_reset_test_calloc(size_t count, size_t size) {
  return calloc(count, size);
}
static void *_reset_test_realloc(void *ptr, size_t size) {
  g_reset_test_realloc_count++;
  return realloc(ptr, size);
}
static void _reset_test_free(void *ptr) { free(ptr); }

// cvector_reset must not touch the allocator at all when the vector is
// already at _ccol_cvector_minimum_capacity, where the shrink does nothing
// (scale_the_cvector_size_down has an early return for the same case), and
// it should shrink, and so call realloc, when a shrink really is
// needed.
TEST(cvectors, reset_skips_realloc_when_already_at_minimum_capacity) {
  ccol_memmgmt_procs_t procs = {.malloc = _reset_test_malloc,
                                .calloc = _reset_test_calloc,
                                .realloc = _reset_test_realloc,
                                .free = _reset_test_free};
  g_reset_test_realloc_count = 0;

  cvector *cvec = cvector_create_full(sizeof(int), &procs, NULL);
  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  cvector_reset(cvec);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);
  REQUIRE_EQ(g_reset_test_realloc_count, (size_t)0);

  // Grow past the minimum, then reset, and now expect a shrinking realloc.
  for (int i = 0; i < 20; ++i) {
    cvector_push_back(cvec, &i);
  }
  REQUIRE_NE(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  size_t realloc_count_before_shrink = g_reset_test_realloc_count;
  cvector_reset(cvec);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);
  REQUIRE_GT(g_reset_test_realloc_count, realloc_count_before_shrink);

  cvector_destroy(cvec);
}

TEST(cvectors, boundary_fill_to_capacity) {
  cvector *cvec = cvector_create(sizeof(int), NULL);

  // Fill up to exactly the minimum capacity
  for (int i = 0; i < (int)_ccol_cvector_minimum_capacity; ++i) {
    cvector_push_back(cvec, &i);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), _ccol_cvector_minimum_capacity);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

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

  // Some elements are left
  REQUIRE_NE(cvector_elem_count(cvec), 0);

  cvector_destroy(cvec);
}

// ========================================================================
// TYPE-INFERRED MACRO TESTS
// ========================================================================

TEST(cvectors, constructive_macros) {
  cvec_construct(vec, int);

  int tmp = 2;
  cvec_push(vec, tmp);
  cvec_push(vec, 3);

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
  REQUIRE_EQ(sum, 0);  // all the elements were zeroed

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

  cvec_push(v1, 10);
  cvec_push(v1, 20);

  cvec_push(v2, 30);
  cvec_push(v2, 40);

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
    cvec_push(vec, i * 10);
  }

  int *data = cvec_data_ptr(vec);
  REQUIRE_NE((void *)data, NULL);

  REQUIRE_EQ(data[0], 0);
  REQUIRE_EQ(data[2], 20);
  REQUIRE_EQ(data[4], 40);

  cvec_destroy(vec);
}

TEST(cvectors, type_safe_at_ptr) {
  cvec_construct(vec, int);

  for (int i = 0; i < 5; ++i) {
    cvec_push(vec, i * 10);
  }

  int *p0 = cvec_at_ptr(vec, 0);
  int *p4 = cvec_at_ptr(vec, 4);
  REQUIRE_NE((void *)p0, NULL);
  REQUIRE_NE((void *)p4, NULL);
  REQUIRE_EQ(*p0, 0);
  REQUIRE_EQ(*p4, 40);

  *p0 = 999;
  REQUIRE_EQ(cvec_at(vec, 0), 999);

  // Out of bounds: NULL, no termination.
  REQUIRE_EQ((void *)cvec_at_ptr(vec, 5), NULL);
  REQUIRE_EQ((void *)cvec_at_ptr(vec, (size_t)-1), NULL);

  cvec_destroy(vec);
}

// cvec_at stops the process with _ccol_fatal_err() for an out-of-bounds
// index, the same "aborting convenience API" contract every other
// type-inferred macro has. cvector_at() returns NULL for such an index, and
// dereferencing that NULL directly gives a SIGSEGV instead. The test runs in
// a forked child because _ccol_fatal_err stops the whole process, like the
// tests in tests/cthreadpool/tests.c do for misuse that stops the process
// the same way.
TEST(cvectors, type_safe_at_out_of_bounds_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cvec_construct(vec, int);
    cvec_push(vec, 1);
    (void)cvec_at(vec,
                  1); /* out of bounds; must _ccol_fatal_err(), not SIGSEGV */
    _exit(0);         /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
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
  REQUIRE_EQ(sum, 0);  // all the elements were zeroed

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
  REQUIRE_EQ(sum, 0);  // all the elements were zeroed

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
  REQUIRE_EQ(sum, 0);  // all the elements were zeroed

  cvec_destroy(vec);
}

TEST(cvectors, construct_scoped_lifecycle) {
  {
    cvec_construct_scoped(v, int);
    REQUIRE_NE((void *)v, NULL);

    for (int i = 1; i <= 5; ++i) {
      cvec_push(v, i);
    }
    REQUIRE_EQ(cvector_elem_count(v), 5);
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cvec_at(v, i), i + 1);
    }
    // v gets destroyed automatically at the end of the block (no cvec_destroy)
  }
}

// ========================================================================
// MACRO HYGIENE TESTS
// ========================================================================
//
// cvec_push and cvec_pop each declare an internal local that holds a
// result, and neither may call that local plain 'r'. cvec_pop also keeps an
// element in a local, which may not be called plain '_tmp'. The trouble is
// in the SAME statement whose initializer holds the macro's parameters,
// which the preprocessor substitutes as text: in C a declared identifier's
// scope starts right after its own declarator, so before the initializer is
// evaluated (the usual `int x = x;` problem comes from the same rule).
//
// A caller can call its own vector variable, or the expression it pushes or
// pops, plain 'r', and this project uses 'r' for a retval local all the
// time. If a macro local had that name, the caller's identifier would
// silently resolve to the macro's local instead of the caller's real
// variable, while that local has no value yet, and the result would be wrong
// data with no compiler warning at any optimization level. So each of these
// internal locals has a name specific to its own macro, which no real caller
// identifier can collide with.

TEST(cvectors, push_value_named_r_is_not_shadowed_by_internal_retval) {
  cvec_construct(vec, int);
  int r = 42;
  cvec_push(vec, r);
  REQUIRE_EQ(cvec_size(vec), (size_t)1);
  REQUIRE_EQ(cvec_at(vec, 0), 42);
  cvec_destroy(vec);
}

TEST(cvectors, push_expression_named_r_is_not_shadowed) {
  cvec_construct(vec, int);
  int r = 77;
  cvec_push(vec, r);
  REQUIRE_EQ(cvec_size(vec), (size_t)1);
  REQUIRE_EQ(cvec_at(vec, 0), 77);
  cvec_destroy(vec);
}

TEST(cvectors, pop_from_vector_variable_named_r_is_not_shadowed) {
  cvec_construct(r, int);
  cvec_push(r, 5);
  cvec_push(r, 9);
  int popped = cvec_pop(r);
  REQUIRE_EQ(popped, 9);
  REQUIRE_EQ(cvec_size(r), (size_t)1);
  REQUIRE_EQ(cvec_at(r, 0), 5);
  cvec_destroy(r);
}

TEST(cvectors, pop_from_vector_variable_named__tmp_is_not_shadowed) {
  // The internal local of cvec_pop that holds an element can't be a plain
  // '_tmp', because a vector variable the caller calls '_tmp' drives the
  // cross-statement form of the same hazard: such a declaration in the body
  // of the macro's statement expression declares '_tmp' again with the
  // vector's element type, and so shadows the caller's cvec for the rest of
  // that statement expression.
  cvec_construct(_tmp, int);
  cvec_push(_tmp, 3);
  cvec_push(_tmp, 6);
  int popped = cvec_pop(_tmp);
  REQUIRE_EQ(popped, 6);
  REQUIRE_EQ(cvec_size(_tmp), (size_t)1);
  REQUIRE_EQ(cvec_at(_tmp, 0), 3);
  cvec_destroy(_tmp);
}

// cvec_at must not expand its index argument twice, once for the
// bounds-checked access of cvector_at() and once for the re-read that
// _cvec_at_checked() makes just for its message. Both are arguments of the
// same function call, so nothing sequences one against the other. With an
// ordinary variable or a literal index a double expansion can't be seen, but
// with an index expression that has a side effect, like cvec_at(vec, i++),
// it's real undefined behavior: in practice the index variable then goes up
// by 2 per call instead of 1, so the loop silently skips every other element
// and walks off the end of the vector. Copying the index into a local
// exactly once, before either use, prevents it. The tests below read and
// write through a loop with a bare j++ index expression and assert that the
// loop variable goes up by exactly 1 per iteration and that the loop visits
// every element exactly once, in
// order.
TEST(cvectors, at_index_expression_with_side_effect_evaluated_once) {
  cvec_construct(vec, int);
  for (int i = 0; i < 5; ++i) {
    cvec_push(vec, i * 10);
  }

  int j = 0;
  int sum = 0;
  for (int n = 0; n < 5; ++n) {
    sum += cvec_at(vec, j++);
  }
  REQUIRE_EQ(j, 5);      // j goes up by exactly 1 per call
  REQUIRE_EQ(sum, 100);  // 0+10+20+30+40, a double eval would break this

  cvec_destroy(vec);
}

TEST(cvectors, at_index_expression_with_side_effect_evaluated_once_as_lvalue) {
  cvec_construct(vec, int);
  for (int i = 0; i < 4; ++i) {
    cvec_push(vec, 0);
  }

  int j = 0;
  for (int n = 0; n < 4; ++n) {
    cvec_at(vec, j++) = n + 1;  // used as an assignment target, also one eval
  }
  REQUIRE_EQ(j, 4);

  for (int i = 0; i < 4; ++i) {
    REQUIRE_EQ(cvec_at(vec, i), i + 1);
  }

  cvec_destroy(vec);
}

// ========================================================================
// FAILURE-PATH SINGLE-EVALUATION TESTS
// ========================================================================
//
// cvec_reserve puts its new_capacity_count argument in two places of its
// expansion, and cvec_append_array does the same with its elem_count
// argument: one is the real call to cvector_reserve() or
// cvector_append_array(), the other is the _ccol_fatal_err() message, which
// the macro only builds on the failure path. So both macros need a guard
// that evaluates the argument once, like the one for cvec_at's index (see
// the "at_index_expression_with_side_effect_evaluated_once*" tests above)
// and the internal locals of cvec_push and cvec_pop.
//
// Without that guard, an argument expression with a side effect runs a
// second time when the real call fails, just to build the message. That
// silently repeats the real side effect, and it can show a value different
// from the one the call used, right before _ccol_fatal_err() stops the
// process. A macro that copies the argument into a local exactly once
// prevents it, the way cvec_at does with its index.
//
// The two tests below use a deterministic failure: one goes above
// ccol_max_elem_count and the other overflows the append's total. Neither
// uses a failing allocator, so the call count below comes only from the
// macro's expansion and not from a retry in the allocator. The tests keep
// that count in shared memory from mmap: the forked child is the process
// that does the double evaluation and then aborts, so the only state the
// parent can see is what the abort doesn't remove, which is memory the two
// processes share across the fork.

static int *g_reserve_side_effect_call_count;
static size_t reserve_side_effecting_target(void) {
  (*g_reserve_side_effect_call_count)++;
  return ccol_max_elem_count + 1;  // makes sure cvector_reserve() fails
}

TEST(cvectors, reserve_fatal_err_evaluates_count_argument_exactly_once) {
  int *call_count = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  REQUIRE_NE((void *)call_count, MAP_FAILED);
  *call_count = 0;
  g_reserve_side_effect_call_count = call_count;

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cvec_construct(vec, int);
    cvec_reserve(vec, reserve_side_effecting_target());
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
  REQUIRE_EQ(*call_count, 1);

  munmap(call_count, sizeof(int));
}

static int *g_append_array_side_effect_call_count;
static size_t append_array_side_effecting_count(void) {
  (*g_append_array_side_effect_call_count)++;
  return SIZE_MAX;  // makes sure cvector_append_array() fails on overflow
}

TEST(cvectors, append_array_fatal_err_evaluates_count_argument_exactly_once) {
  int *call_count = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  REQUIRE_NE((void *)call_count, MAP_FAILED);
  *call_count = 0;
  g_append_array_side_effect_call_count = call_count;

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cvec_construct(vec, int);
    int dummy[1] = {0};
    cvec_append_array(vec, dummy, append_array_side_effecting_count());
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
  REQUIRE_EQ(*call_count, 1);

  munmap(call_count, sizeof(int));
}

// ========================================================================
// PUSH TYPE-CONVERSION TESTS
// ========================================================================
//
// cvec_push must not take the address of new_elem directly, must not build
// a compound literal of new_elem's OWN type, and must not rely on a
// _Static_assert over sizeof() as its only guard:
// - Such an assert catches a new_elem of a different SIZE, so it prevents
//   an out-of-bounds read.
// - It does nothing for a new_elem of a different TYPE and the *same* size.
// - Pushing a `float` into a vector from cvec_construct(v, int) passes the
//   size check, since sizeof(float) == sizeof(int) on every mainstream
//   platform.
// - The raw 4-byte IEEE 754 bit pattern then goes byte for byte into the
//   vector's int storage, while a plain C assignment like
//   `int x = some_float;` converts the value.
// - Reading that element gives the float's bits read as an int instead of
//   the expected truncated integer, and no compiler warns at any
//   optimization level.
//
// So the internal temporary has v's declared element type and new_elem
// initializes it, which puts the conversion through the compiler's real
// assignment rules, the same pattern cvec_find uses. These tests push a
// value of another type with the same size and check the actual VALUE that
// comes out, not only that the call compiles and succeeds, because a push
// through an untyped temporary also compiles and gives ccol_success while
// silently storing a wrong value.

TEST(cvectors,
     push_expression_float_into_int_vector_converts_not_reinterprets) {
  cvec_construct(vec, int);
  cvec_push(vec, 3.0f);
  // A raw byte copy of 3.0f's IEEE-754 bit pattern read as an int would be
  // 1077936128 and not 3, so a regression brings back exactly that wrong
  // value.
  REQUIRE_EQ(cvec_at(vec, 0), 3);
  cvec_destroy(vec);
}

TEST(cvectors, push_float_lvalue_into_int_vector_converts_not_reinterprets) {
  cvec_construct(vec, int);
  float f = 3.0f;
  cvec_push(vec, f);
  REQUIRE_EQ(cvec_at(vec, 0), 3);
  cvec_destroy(vec);
}

TEST(cvectors, push_expression_double_into_long_long_vector_converts) {
  cvec_construct(vec, long long);
  cvec_push(vec, 7.0);
  REQUIRE_EQ(cvec_at(vec, 0), (long long)7);
  cvec_destroy(vec);
}

TEST(cvectors,
     push_negative_float_into_int_vector_truncates_like_plain_assignment) {
  cvec_construct(vec, int);
  // The value goes through a variable and not a literal, because Clang's
  // -Wliteral-conversion warns about a narrowing conversion of a
  // compile-time constant literal but not of a general expression. If we
  // gave -9.75f directly to cvec_push, the literal would initialize the
  // macro's typed temporary and the warning would fire, although the
  // truncation below is on purpose and the warning would be wrong here. A
  // variable has the same value at run time and gives the warning no constant
  // literal to find.
  float negative_value = -9.75f;
  cvec_push(vec, negative_value);
  int expected =
      (int)negative_value;  // plain C conversion, truncates toward 0 (-9)
  REQUIRE_EQ(cvec_at(vec, 0), expected);
  cvec_destroy(vec);
}

// An int literal with no suffix can go into a vector of a wider integer
// type, and so can any smaller integer type. Such a push should really
// convert to the wider type instead of being rejected at compile time
// because of its size, which is what a _Static_assert over sizeof() would
// do, even though in C that conversion is ordinary and completely safe.
TEST(cvectors, push_expression_int_literal_into_long_vector_converts) {
  cvec_construct(vec, long);
  cvec_push(vec, 5);
  REQUIRE_EQ(cvec_at(vec, 0), (long)5);
  cvec_destroy(vec);
}

TEST(cvectors, push_int_lvalue_into_long_vector_converts) {
  cvec_construct(vec, long);
  int x = 5;
  cvec_push(vec, x);
  REQUIRE_EQ(cvec_at(vec, 0), (long)5);
  cvec_destroy(vec);
}

// The conversion must happen before anything touches the vector, so an
// lvalue that aliases the vector, like cvec_at itself, is read once and
// correctly, and no capacity growth the push starts can affect it. This is
// the same self-alias-during-growth case as
// push_back_self_alias_via_type_safe_macro above, but through the code path
// that carries the conversion.
TEST(cvectors, push_self_alias_is_safe_with_conversion) {
  cvec_construct(vec, int);
  for (int i = 0; i < 4; ++i) {
    cvec_push(vec, (i + 1) * 100);
  }
  REQUIRE_EQ(cvector_get_capacity(vec), _ccol_cvector_minimum_capacity);

  cvec_push(vec, cvec_at(vec, 0));  // aliases the vector's own storage
  REQUIRE_EQ(cvec_size(vec), (size_t)5);
  REQUIRE_NE(cvector_get_capacity(vec),
             _ccol_cvector_minimum_capacity);  // grew
  REQUIRE_EQ(cvec_at(vec, 4), 100);

  cvec_destroy(vec);
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

// cvector_begin_iter(NULL, ...) must behave like an empty vector: return
// NULL, set err to NULL and not assert. chashmap_begin_iter() and
// cbmap_begin_iter() tolerate NULL in the same deliberate way, so the three
// container modules stay consistent here, and a container field can stay a
// null pointer while there's nothing in it yet: the caller can iterate such
// a field directly with no NULL guard of its
// own.
TEST(cvectors, begin_iter_null_vec_returns_null_like_empty) {
  cvec null_vec = NULL;
  char *err = (char *)0x1; /* poison value, must be reset to NULL */
  REQUIRE_EQ((void *)cvector_begin_iter(null_vec, &err), NULL);
  REQUIRE_EQ((void *)err, NULL);

  // A NULL err has to be tolerated too (it's documented as optional).
  REQUIRE_EQ((void *)cvector_begin_iter(null_vec, NULL), NULL);
}

TEST(cvectors, iterator_single_element) {
  cvec_construct(vec, int);
  cvec_push(vec, 99);

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
    cvec_push(vec, i * 5);
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
    cvec_push(vec, i);
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
    cvec_push(vec, i);
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
    cvec_push(vec, i);
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
  cvec_sort(vec);  // must not crash on an empty input
  REQUIRE_EQ(cvec_size(vec), (size_t)0);
  cvec_destroy(vec);
}

TEST(cvectors, sort_single_element) {
  cvec_construct(vec, int);
  cvec_push(vec, 42);
  cvec_sort(vec);
  REQUIRE_EQ(cvec_size(vec), (size_t)1);
  REQUIRE_EQ(cvec_at(vec, 0), 42);
  cvec_destroy(vec);
}

TEST(cvectors, sort_ascending) {
  cvec_construct(vec, int);
  cvec_push(vec, 5);
  cvec_push(vec, 3);
  cvec_push(vec, 1);
  cvec_push(vec, 4);
  cvec_push(vec, 2);

  cvec_sort(vec);

  for (int i = 0; i < 5; ++i) {
    REQUIRE_EQ(cvec_at(vec, i), i + 1);
  }

  cvec_destroy(vec);
}

TEST(cvectors, sort_already_sorted) {
  cvec_construct(vec, int);
  for (int i = 0; i < 8; ++i) {
    cvec_push(vec, i);
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
    cvec_push(vec, i);
  }

  cvector_sort_with_comparison_proc(vec, cmp_int_descending);

  for (int i = 0; i < 6; ++i) {
    REQUIRE_EQ(cvec_at(vec, i), 6 - i);
  }

  cvec_destroy(vec);
}

TEST(cvectors, sort_doubles) {
  cvec_construct(vec, double);
  cvec_push(vec, 3.0);
  cvec_push(vec, 1.0);
  cvec_push(vec, 4.0);
  cvec_push(vec, 2.0);

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

  // The key=1 group comes first, in insertion order (seq 0,1,2)
  REQUIRE_EQ(cvec_at(vec, 0).key, 1);
  REQUIRE_EQ(cvec_at(vec, 0).seq, 0);
  REQUIRE_EQ(cvec_at(vec, 1).key, 1);
  REQUIRE_EQ(cvec_at(vec, 1).seq, 1);
  REQUIRE_EQ(cvec_at(vec, 2).key, 1);
  REQUIRE_EQ(cvec_at(vec, 2).seq, 2);
  // then the key=2 group, also in insertion order
  REQUIRE_EQ(cvec_at(vec, 3).key, 2);
  REQUIRE_EQ(cvec_at(vec, 3).seq, 0);
  REQUIRE_EQ(cvec_at(vec, 4).key, 2);
  REQUIRE_EQ(cvec_at(vec, 4).seq, 1);
  REQUIRE_EQ(cvec_at(vec, 5).key, 2);
  REQUIRE_EQ(cvec_at(vec, 5).seq, 2);

  cvec_destroy(vec);
}

// A budget-style counting allocator. The first g_sort_oom_budget calls to
// malloc/calloc/realloc succeed (passed on to the real allocator) and every
// call after the budget is used up returns NULL. free() always goes to the
// real free(), so whatever DID succeed gets released correctly during
// cvector_destroy.
static int g_sort_oom_budget = 0;
static void *_sort_oom_malloc(size_t size) {
  if (g_sort_oom_budget <= 0) return NULL;
  g_sort_oom_budget--;
  return malloc(size);
}
static void *_sort_oom_calloc(size_t count, size_t size) {
  if (g_sort_oom_budget <= 0) return NULL;
  g_sort_oom_budget--;
  return calloc(count, size);
}
static void *_sort_oom_realloc(void *ptr, size_t size) {
  if (g_sort_oom_budget <= 0) return NULL;
  g_sort_oom_budget--;
  return realloc(ptr, size);
}
static void _sort_oom_free(void *ptr) { free(ptr); }

// Allocating the sort's internal temp buffer can fail, and then cvec_sort
// and cvector_sort_with_comparison_proc must call _ccol_fatal_err() and
// not silently leave the vector unsorted, which would tell the caller
// nothing; every other type-inferred macro in cvector.h that changes a
// vector does the same. The test runs in a forked child because
// _ccol_fatal_err() stops the whole process, the same shape as
// type_safe_at_out_of_bounds_is_fatal.
TEST(cvectors, sort_out_of_memory_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    ccol_memmgmt_procs_t procs = {.malloc = _sort_oom_malloc,
                                  .calloc = _sort_oom_calloc,
                                  .realloc = _sort_oom_realloc,
                                  .free = _sort_oom_free};
    // The budget covers exactly the vector's construction (1 calloc for the
    // struct with its procs copy and 1 malloc for the first data buffer)
    // plus three push_backs, which stay inside the first capacity of 4 and
    // need no allocation. So the malloc for the sort's temp buffer is the 3rd
    // allocator call, and that one is the
    // one to fail.
    g_sort_oom_budget = 2;
    cvec_declare(vec, int);
    cvec_init_mp(vec, &procs);
    cvec_push(vec, 3);
    cvec_push(vec, 1);
    cvec_push(vec, 2);
    cvec_sort(vec); /* must _ccol_fatal_err(), not return unsorted */
    _exit(0);       /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// cvec_sort on a vector of `signed char` needs a real default signed
// comparator instead of silently falling back to a NULL one, so the _Generic
// dispatch of csort_get_default_comparison_proc needs a case for the
// separate `signed char` type and not only for plain `char`.
// _ccol_is_integral_type() in common.h classifies `signed char` as integral
// and the project documents it elsewhere as a proper signed type. We use
// negative values on purpose, because they show the comparator really
// compares as signed and rule out an unsigned or raw-byte order that just
// happens to pass.
TEST(cvectors, sort_signed_char_uses_genuine_signed_default_comparator) {
  cvec_construct(vec, signed char);
  cvec_push(vec, (signed char)3);
  cvec_push(vec, (signed char)-5);
  cvec_push(vec, (signed char)100);
  cvec_push(vec, (signed char)-100);
  cvec_push(vec, (signed char)0);

  cvec_sort(vec);

  const int expected[] = {-100, -5, 0, 3, 100};
  for (int i = 0; i < 5; ++i) {
    REQUIRE_EQ((int)cvec_at(vec, i), expected[i]);
  }

  cvec_destroy(vec);
}

// Some element types have no default comparison procedure: a struct, a
// union, a pointer to anything but a char, and a fixed-size char array.
// cvec_sort's _Static_assert rejects them, so such a call never builds and
// never reaches a running program. A test binary has to compile, so it can't
// drive a diagnostic the compiler issues, and compile_probes.sh pins that
// rejection instead; the `test` target of this directory runs it next to this
// binary. Under every compiler it finds, it asserts that each of those
// element types fails to compile with a message naming
// cvector_sort_with_comparison_proc, and that every supported element type
// compiles.
//
// An enumerated type IS supported and has to keep sorting: C makes every
// enumerated type compatible with one of the standard integer types, so
// _Generic picks that type's comparator. The test covers two enumerations,
// the first of which the compiler can represent as unsigned and the second
// it can't, because it has a negative enumerator, so the two resolve to
// different comparators.
typedef enum {
  cvec_test_enum_low = 0,
  cvec_test_enum_mid = 7,
  cvec_test_enum_high = 42
} cvec_test_unsigned_enum;

typedef enum {
  cvec_test_senum_neg = -9,
  cvec_test_senum_zero = 0,
  cvec_test_senum_pos = 5
} cvec_test_signed_enum;

TEST(cvectors, sort_of_an_enum_vector_uses_the_underlying_integer_comparator) {
  cvec_construct(vec, cvec_test_unsigned_enum);
  cvec_push(vec, cvec_test_enum_high);
  cvec_push(vec, cvec_test_enum_low);
  cvec_push(vec, cvec_test_enum_mid);

  cvec_sort(vec);

  REQUIRE_EQ((int)cvec_at(vec, 0), (int)cvec_test_enum_low);
  REQUIRE_EQ((int)cvec_at(vec, 1), (int)cvec_test_enum_mid);
  REQUIRE_EQ((int)cvec_at(vec, 2), (int)cvec_test_enum_high);

  cvec_destroy(vec);
}

TEST(cvectors, sort_of_a_signed_enum_vector_orders_negative_enumerators_first) {
  cvec_construct(vec, cvec_test_signed_enum);
  cvec_push(vec, cvec_test_senum_pos);
  cvec_push(vec, cvec_test_senum_neg);
  cvec_push(vec, cvec_test_senum_zero);

  cvec_sort(vec);

  REQUIRE_EQ((int)cvec_at(vec, 0), (int)cvec_test_senum_neg);
  REQUIRE_EQ((int)cvec_at(vec, 1), (int)cvec_test_senum_zero);
  REQUIRE_EQ((int)cvec_at(vec, 2), (int)cvec_test_senum_pos);

  cvec_destroy(vec);
}

// Every element type cvec_sort accepts should really sort, compiling isn't
// enough. The sweep below is driven by a macro and covers each integer
// width, each signedness and each floating type in one place, so if csort's
// _Generic dispatch loses a comparator for one width, it shows up as a
// wrong order and not as a test that is silently
// missing.
#define CVEC_TEST_SORTS_INTEGRAL(T, lo, mid, hi)                       \
  do {                                                                 \
    cvec_construct(sweep_vec, T);                                      \
    cvec_push(sweep_vec, (T)(hi));                                     \
    cvec_push(sweep_vec, (T)(lo));                                     \
    cvec_push(sweep_vec, (T)(mid));                                    \
    cvec_sort(sweep_vec);                                              \
    REQUIRE_EQ((long long)cvec_at(sweep_vec, 0), (long long)(T)(lo));  \
    REQUIRE_EQ((long long)cvec_at(sweep_vec, 1), (long long)(T)(mid)); \
    REQUIRE_EQ((long long)cvec_at(sweep_vec, 2), (long long)(T)(hi));  \
    cvec_destroy(sweep_vec);                                           \
  } while (0)

#define CVEC_TEST_SORTS_FLOATING(T, lo, mid, hi)                 \
  do {                                                           \
    cvec_construct(sweep_vec, T);                                \
    cvec_push(sweep_vec, (T)(hi));                               \
    cvec_push(sweep_vec, (T)(lo));                               \
    cvec_push(sweep_vec, (T)(mid));                              \
    cvec_sort(sweep_vec);                                        \
    REQUIRE_EQ((double)cvec_at(sweep_vec, 0), (double)(T)(lo));  \
    REQUIRE_EQ((double)cvec_at(sweep_vec, 1), (double)(T)(mid)); \
    REQUIRE_EQ((double)cvec_at(sweep_vec, 2), (double)(T)(hi));  \
    cvec_destroy(sweep_vec);                                     \
  } while (0)

TEST(cvectors, sort_sweeps_every_supported_numeric_element_type) {
  CVEC_TEST_SORTS_INTEGRAL(char, 'a', 'm', 'z');
  CVEC_TEST_SORTS_INTEGRAL(signed char, -128, 0, 127);
  CVEC_TEST_SORTS_INTEGRAL(unsigned char, 0, 128, 255);
  CVEC_TEST_SORTS_INTEGRAL(short, -32768, 0, 32767);
  CVEC_TEST_SORTS_INTEGRAL(unsigned short, 0, 30000, 65535);
  CVEC_TEST_SORTS_INTEGRAL(int, -2000000000, 0, 2000000000);
  CVEC_TEST_SORTS_INTEGRAL(unsigned int, 0u, 2000000000u, 4000000000u);
  CVEC_TEST_SORTS_INTEGRAL(long, -1000000L, 0L, 1000000L);
  CVEC_TEST_SORTS_INTEGRAL(unsigned long, 0UL, 1000000UL, 4000000000UL);
  CVEC_TEST_SORTS_INTEGRAL(long long, -9000000000000LL, 0LL, 9000000000000LL);
  CVEC_TEST_SORTS_INTEGRAL(unsigned long long, 0ULL, 9000000000000ULL,
                           18000000000000ULL);

  CVEC_TEST_SORTS_FLOATING(float, -1.5f, 0.0f, 2.25f);
  CVEC_TEST_SORTS_FLOATING(double, -1.5, 0.0, 2.25);
  CVEC_TEST_SORTS_FLOATING(long double, -1.5L, 0.0L, 2.25L);
}

// Every char pointer spelling that csort's default dispatch accepts sorts by
// string content. signed char * and unsigned char * are included because the
// dispatch accepts them exactly like char *.
TEST(cvectors, sort_sweeps_every_supported_char_pointer_element_type) {
  {
    cvec_construct(vec, const char *);
    cvec_push(vec, "pear");
    cvec_push(vec, "apple");
    cvec_push(vec, "fig");
    cvec_sort(vec);
    REQUIRE_STREQ(cvec_at(vec, 0), "apple");
    REQUIRE_STREQ(cvec_at(vec, 1), "fig");
    REQUIRE_STREQ(cvec_at(vec, 2), "pear");
    cvec_destroy(vec);
  }
  {
    signed char s_a[] = "apple";
    signed char s_f[] = "fig";
    signed char s_p[] = "pear";
    cvec_construct(vec, signed char *);
    cvec_push(vec, s_p);
    cvec_push(vec, s_a);
    cvec_push(vec, s_f);
    cvec_sort(vec);
    REQUIRE_STREQ((const char *)cvec_at(vec, 0), "apple");
    REQUIRE_STREQ((const char *)cvec_at(vec, 1), "fig");
    REQUIRE_STREQ((const char *)cvec_at(vec, 2), "pear");
    cvec_destroy(vec);
  }
  {
    unsigned char u_a[] = "apple";
    unsigned char u_f[] = "fig";
    unsigned char u_p[] = "pear";
    cvec_construct(vec, unsigned char *);
    cvec_push(vec, u_p);
    cvec_push(vec, u_a);
    cvec_push(vec, u_f);
    cvec_sort(vec);
    REQUIRE_STREQ((const char *)cvec_at(vec, 0), "apple");
    REQUIRE_STREQ((const char *)cvec_at(vec, 1), "fig");
    REQUIRE_STREQ((const char *)cvec_at(vec, 2), "pear");
    cvec_destroy(vec);
  }
}

// cvec_find accepts every element type on purpose, the ones cvec_sort
// rejects too, because byte-by-byte equality is a real answer for a type
// with no default comparison procedure, while an order isn't.
TEST(cvectors, find_on_an_element_type_without_a_default_comparator_compiles) {
  cvec_construct(vec, bool);
  bool t = true, f = false;
  cvec_push(vec, f);
  cvec_push(vec, t);

  REQUIRE_EQ(cvec_find(vec, true), (size_t)1);
  REQUIRE_EQ(cvec_find(vec, false), (size_t)0);

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
  cvec_push(vec, 1);
  REQUIRE_EQ(cvector_find(vec, NULL, NULL), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_single_element_match) {
  cvec_construct(vec, int);
  cvec_push(vec, 7);
  REQUIRE_EQ(cvec_find(vec, 7), (size_t)0);
  cvec_destroy(vec);
}

TEST(cvectors, find_single_element_no_match) {
  cvec_construct(vec, int);
  cvec_push(vec, 7);
  REQUIRE_EQ(cvec_find(vec, 99), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_first_element) {
  cvec_construct(vec, int);
  cvec_push(vec, 10);
  cvec_push(vec, 20);
  cvec_push(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 10), (size_t)0);
  cvec_destroy(vec);
}

TEST(cvectors, find_middle_element) {
  cvec_construct(vec, int);
  cvec_push(vec, 10);
  cvec_push(vec, 20);
  cvec_push(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 20), (size_t)1);
  cvec_destroy(vec);
}

TEST(cvectors, find_last_element) {
  cvec_construct(vec, int);
  cvec_push(vec, 10);
  cvec_push(vec, 20);
  cvec_push(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 30), (size_t)2);
  cvec_destroy(vec);
}

TEST(cvectors, find_not_found) {
  cvec_construct(vec, int);
  cvec_push(vec, 10);
  cvec_push(vec, 20);
  cvec_push(vec, 30);
  REQUIRE_EQ(cvec_find(vec, 99), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_returns_first_occurrence) {
  cvec_construct(vec, int);
  cvec_push(vec, 5);
  cvec_push(vec, 10);
  cvec_push(vec, 5);
  cvec_push(vec, 10);
  // Both 5 and 10 appear twice, and it returns the first index
  REQUIRE_EQ(cvec_find(vec, 5), (size_t)0);
  REQUIRE_EQ(cvec_find(vec, 10), (size_t)1);
  cvec_destroy(vec);
}

int helper_cmp_int(const void *a, const void *b) {
  return *(const int *)a - *(const int *)b;
}

TEST(cvectors, find_with_custom_comparator) {
  cvec_construct(vec, int);
  cvec_push(vec, 100);
  cvec_push(vec, 200);
  cvec_push(vec, 300);
  REQUIRE_EQ(cvector_find(vec, &(int){200}, helper_cmp_int), (size_t)1);
  REQUIRE_EQ(cvector_find(vec, &(int){999}, helper_cmp_int), ccol_invalid_size);
  cvec_destroy(vec);
}

TEST(cvectors, find_with_double_type) {
  cvec_construct(vec, double);
  cvec_push(vec, 1.5);
  cvec_push(vec, 2.5);
  cvec_push(vec, 3.5);
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
  cvec_push(vec, 10);
  cvec_push(vec, 20);
  cvec_push(vec, 30);

  {
    cvec_redeclare(vec, int);
    REQUIRE_EQ(cvec_size(vec), (size_t)3);
    REQUIRE_EQ(cvec_at(vec, 0), 10);
    REQUIRE_EQ(cvec_at(vec, 1), 20);
    REQUIRE_EQ(cvec_at(vec, 2), 30);

    cvec_push(vec, 40);
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
      cvec_push(v, i);
    }
    REQUIRE_EQ(cvector_elem_count(v), 5);
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cvec_at(v, i), i + 1);
    }
    // v gets destroyed automatically at the end of the block (no cvec_destroy)
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
      cvec_push(v, i);
    }
    REQUIRE_EQ(cvector_elem_count(v), 5);
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cvec_at(v, i), i + 1);
    }
    // v gets destroyed automatically at the end of the block (no cvec_destroy)
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
    cvec_push(vec, i);
  }

  int count = 0;
  {
    ccol_iter_declare(vec, it);
    for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) {
      ++count;
      if (count == 2) {
        break;  // leave with no explicit ccol_iter_destroy, RAII cleans up
      }
    }
    // 'it' is still live here, and its RAII destructor fires at '}'
  }
  REQUIRE_EQ(count, 2);
  REQUIRE_EQ(cvector_elem_count(vec), 5);  // the vector is unchanged

  cvec_destroy(vec);
}

// ========================================================================
// INVARIANT TESTS (randomized operation sequences)
// ========================================================================

static bool cvector_is_power_of_two(size_t x) {
  return x != 0 && (x & (x - 1)) == 0;
}

// A random sequence of push_back, pop_back, reserve and reset operations,
// with the structural invariants checked after every single one: elem_count
// never goes above the capacity, the capacity never drops below
// _ccol_cvector_minimum_capacity and is always a power of two, and data_ptr
// is never NULL while the capacity is above 0.
// The seed is fixed (see common_invariants.h), so you can always reproduce a
// failure from the printed seed alone.
TEST(cvectors, invariants_random_ops) {
  ccol_invariants_rng_t rng;
  uint64_t seed = CCOL_INVARIANTS_DEFAULT_SEED;
  ccol_invariants_seed(&rng, seed);
  ccol_invariants_print_seed("cvectors.invariants_random_ops", seed);

  cvector *vec = cvector_create(sizeof(int), NULL);
  REQUIRE_NE((void *)vec, NULL);

  const int num_ops = 2000;
  for (int i = 0; i < num_ops; ++i) {
    int op = (int)ccol_invariants_next_bounded(&rng, 4);
    switch (op) {
      case 0: {
        int val = (int)ccol_invariants_next_bounded(&rng, 1000000);
        cvector_push_back(vec, &val);
        break;
      }
      case 1: {
        int out;
        cvector_pop_back(
            vec, &out);  // no-op (returns ccol_container_empty) if empty
        break;
      }
      case 2: {
        size_t target = ccol_invariants_next_bounded(&rng, 200);
        cvector_reserve(vec, target);
        break;
      }
      case 3: {
        if (ccol_invariants_next_bounded(&rng, 20) == 0) {
          cvector_reset(vec);
        }
        break;
      }
    }

    size_t elem_count = cvector_elem_count(vec);
    size_t capacity = cvector_get_capacity(vec);
    REQUIRE_TRUE(elem_count <= capacity);
    REQUIRE_TRUE(capacity >= _ccol_cvector_minimum_capacity);
    REQUIRE_TRUE(cvector_is_power_of_two(capacity));
    if (capacity > 0) {
      REQUIRE_NE(cvector_data_ptr(vec), NULL);
    }
  }

  cvector_destroy(vec);
}

/* ========================================================================== */
/*         ELEMENT EQUALITY, MACRO HYGIENE AND ITERATOR RAII                  */
/* ========================================================================== */

/* cvec_find compares with the element type's default comparison proc, so
 * equality means equality of the type and not of its object representation.
 * That matters for any type whose representation has bytes the value doesn't
 * fix, like x86-64's 80-bit long double, which uses 10 of its 16 bytes while
 * nothing initialises the other 6. A byte-by-byte comparison of two objects
 * holding the same value reports them as different, and it reads
 * indeterminate bytes on top of that. These tests do catch it: comparing
 * raw bytes here makes the first test report every element that is present
 * as missing at -O0, and makes valgrind report an uninitialised read at
 * every optimisation level. */
static long double helper_parse_long_double(const char *s) {
  char *end;
  return strtold(s, &end);
}

TEST(find, long_double_vector_finds_every_present_value) {
  cvec_construct(vec, long double);
  static const char *inputs[] = {"1.25", "2.50", "3.75", "4.00"};
  for (size_t i = 0; i < 4; i++)
    cvec_push(vec, helper_parse_long_double(inputs[i]));

  bool all_found = true;
  for (size_t i = 0; i < 4; i++) {
    if (cvec_find(vec, helper_parse_long_double(inputs[i])) != i)
      all_found = false;
  }
  bool absent_reported_absent =
      (cvec_find(vec, helper_parse_long_double("99.5")) == ccol_invalid_size);

  /* This is the case that tells the two rules apart: -0.0L and 0.0L are equal
   * values but they differ in the byte that holds the sign bit, so only a
   * comparison of values finds this one. The four lookups above agree with a
   * byte comparison whenever the padding bytes happen to match, so they check
   * correctness but aren't what pins the contract. */
  cvec_construct(zvec, long double);
  cvec_push(zvec, -0.0L);
  size_t neg_zero_idx = cvec_find(zvec, 0.0L);
  cvec_destroy(zvec);

  cvec_destroy(vec);
  REQUIRE_TRUE(all_found);
  REQUIRE_TRUE(absent_reported_absent);
  REQUIRE_EQ(neg_zero_idx, (size_t)0);
}

TEST(find, double_vector_treats_negative_zero_and_zero_as_one_value) {
  /* The default comparison proc orders by value, the same notion of equality
   * that chashmap and cbstmap use for a double key. */
  cvec_construct(vec, double);
  cvec_push(vec, -0.0);
  size_t idx = cvec_find(vec, 0.0);
  cvec_destroy(vec);
  REQUIRE_EQ(idx, (size_t)0);
}

typedef struct {
  int x;
  int y;
} helper_find_point_t;

TEST(find, an_aggregate_element_type_is_accepted_by_the_macro) {
  /* cvec_find builds its needle by assignment through the vector's own
   * declared element type, so an aggregate element type is just a copy. A
   * braced initialiser here would only initialise the first member, which is
   * a hard compile error for an argument of the same type. */
  cvec_construct(vec, helper_find_point_t);
  helper_find_point_t a = {1, 2};
  helper_find_point_t b = {3, 4};
  cvec_push(vec, a);
  cvec_push(vec, b);
  size_t found = cvec_find(vec, b);
  helper_find_point_t absent = {9, 9};
  size_t missing = cvec_find(vec, absent);
  cvec_destroy(vec);
  REQUIRE_EQ(found, (size_t)1);
  REQUIRE_EQ(missing, ccol_invalid_size);
}

/* For cvec_push and for cvec_find a named array is a value of an array
 * element type. The macro fills the value with zeros up to the element size,
 * so a shorter array or string literal finds the element that went in with
 * the same text, and a longer array gets cut to the element size. If the
 * macro stored the argument by initialization (`T tmp = (elem);`), this
 * code wouldn't compile for a named array. */
TEST(find, a_named_array_is_a_value_of_an_array_element_type) {
  typedef char name8[8];
  cvec_construct(vec, name8);
  char alpha[8] = "alpha";
  const char beta[4] = "bet";
  char gamma_long[16] = "gamma_is_long";
  cvec_push(vec, alpha);
  cvec_push(vec, beta);
  cvec_push(vec, gamma_long);
  cvec_push(vec, "delta");

  char alpha_needle[8] = "alpha";
  size_t by_array = cvec_find(vec, alpha_needle);
  size_t by_short_array = cvec_find(vec, beta);
  size_t by_literal = cvec_find(vec, "delta");
  char gamma_needle[32] = "gamma_is_even_longer";
  size_t by_long_array = cvec_find(vec, gamma_needle);
  size_t missing = cvec_find(vec, "epsilon");
  char stored_beta[8];
  memcpy(stored_beta, cvec_at(vec, 1), sizeof(stored_beta));
  char stored_gamma[8];
  memcpy(stored_gamma, cvec_at(vec, 2), sizeof(stored_gamma));
  cvec_destroy(vec);

  REQUIRE_EQ(by_array, (size_t)0);
  REQUIRE_EQ(by_short_array, (size_t)1);
  REQUIRE_EQ(by_literal, (size_t)3);
  /* Both get cut to the eight bytes "gamma_is", which match. */
  REQUIRE_EQ(by_long_array, (size_t)2);
  REQUIRE_EQ(missing, ccol_invalid_size);
  REQUIRE_EQ(memcmp(stored_beta, "bet\0\0\0\0\0", 8), 0);
  REQUIRE_EQ(memcmp(stored_gamma, "gamma_is", 8), 0);
}

TEST(find, an_int_array_element_type_pushes_and_finds_by_value) {
  typedef int triple[3];
  cvec_construct(vec, triple);
  int a[3] = {1, 2, 3};
  int b[3] = {4, 5, 6};
  cvec_push(vec, a);
  cvec_push(vec, b);
  int needle[3] = {4, 5, 6};
  size_t found = cvec_find(vec, needle);
  int other[3] = {4, 5, 7};
  size_t missing = cvec_find(vec, other);
  cvec_destroy(vec);
  REQUIRE_EQ(found, (size_t)1);
  REQUIRE_EQ(missing, ccol_invalid_size);
}

TEST(macro_hygiene, caller_identifiers_matching_macro_temporaries_still_work) {
  /* Every temporary a public macro declares has a name that no realistic
   * caller identifier collides with. When a macro expands a caller
   * expression into the scope of its own temporary, a shared name makes the
   * caller's argument resolve to that temporary, because an identifier is in
   * scope from the end of its own declarator. None of the cvector macros
   * here expands a caller expression into such a scope, so this test guards
   * against that changing rather than catching a current defect. The form
   * of the hazard a caller can actually reach is pinned in tests/chashmap,
   * where the key argument does get into a temporary's scope. */
  /* The two variables below come BEFORE the construct, so the temporary of
   * the construct and init macro is the one that would capture them. */
  int err = 42;
  int index = 7;
  cvec_construct(vec, int);
  cvec_push(vec, err);
  cvec_push(vec, index);
  int first = cvec_at(vec, 0);
  int second = cvec_at(vec, 1);
  size_t found = cvec_find(vec, index);
  cvec_destroy(vec);
  REQUIRE_EQ(first, 42);
  REQUIRE_EQ(second, 7);
  REQUIRE_EQ(found, (size_t)1);
}

/* This function returns -1 by leaving the scope before anything assigns the
 * iterator variable. ccol_iter_declare attaches a cleanup handler that runs
 * on every exit from the enclosing block, this one included, so the variable
 * it declares must start as NULL: the handler dereferences it and calls
 * through a function pointer inside it, and for an indeterminate value that's
 * an arbitrary indirect call, not a NULL dereference. A missing initialiser
 * is caught at compile time every time, because reading the variable below
 * is then a -Wmaybe-uninitialized error under this project's -Werror. The
 * run-time assertion records the value the cleanup handler would have seen,
 * which an uninitialised stack slot only makes non-NULL some of the
 * time. */
static void *helper_iter_value_before_assignment;

static int helper_iter_early_return(cvec vec, bool take_early_exit) {
  cvec_redeclare(vec, int);
  ccol_iter_declare(vec, it);
  int seen = 0;
  if (take_early_exit) {
    /* The recorded value is the assertion that matters: the cleanup handler
     * that runs on this return dereferences this variable and calls through
     * a function pointer inside it, so the declaration has to leave it NULL.
     * Whether an uninitialised slot holds a trapping value depends on the
     * stack and isn't part of the contract. */
    helper_iter_value_before_assignment = (void *)it;
    return -1;
  }
  for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) seen++;
  return seen;
}

/* Dirties the stack region the iterator variable is going to use, so an
 * uninitialised declaration holds a non-NULL value and not an accidental
 * zero. Without the initialisation this test faults instead of failing. */
static void __attribute__((noinline)) helper_dirty_stack(void) {
  volatile uintptr_t pad[64];
  for (size_t i = 0; i < 64; i++) pad[i] = (uintptr_t)0x4141414141414141ULL;
  (void)pad;
}

TEST(iterator_raii, a_scope_exit_before_the_iterator_is_assigned_is_safe) {
  cvec_construct(vec, int);
  cvec_push(vec, 1);
  cvec_push(vec, 2);
  helper_dirty_stack();
  helper_iter_value_before_assignment =
      (void *)(uintptr_t)0x4141414141414141ULL;
  int early = helper_iter_early_return(vec, true);
  void *seen_before_assignment = helper_iter_value_before_assignment;
  helper_dirty_stack();
  int full = helper_iter_early_return(vec, false);
  cvec_destroy(vec);
  REQUIRE_EQ(early, -1);
  REQUIRE_EQ(full, 2);
  REQUIRE_EQ(seen_before_assignment, NULL);
}

// ========================================================================
// char * vectors holding NULL elements, and a NULL needle
// ========================================================================

// A char * vector with NULL holes is an ordinary shape, an argv-style list is
// one and so is a sparse table of optional strings. cvec_find() and
// cvec_sort() both reach _csort_default_string_comparison_proc with those
// NULL elements, and that comparator orders NULL before every non-NULL string
// and makes NULL equal only to another NULL, so both macros stay defined for
// such a vector. Without that ordering rule these tests fail hard: the
// comparator hands a null pointer to strcmp() and the whole binary dies with
// SIGSEGV instead of a single assertion failing.
TEST(null_strings, find_in_a_char_ptr_vector_holding_a_null_element) {
  cvec_construct(vec, char *);
  cvec_push(vec, "alpha");
  cvec_push(vec, (char *)NULL);
  cvec_push(vec, "beta");

  REQUIRE_EQ(cvec_find(vec, "beta"), (size_t)2);
  REQUIRE_EQ(cvec_find(vec, "alpha"), (size_t)0);
  REQUIRE_EQ(cvec_find(vec, "gamma"), ccol_invalid_size);

  cvec_destroy(vec);
}

// A caller searches for NULL to find the first free slot of such a vector,
// so cvec_find needs to answer a NULL needle without dereferencing
// it.
TEST(null_strings, find_a_null_needle_reports_the_first_null_element) {
  cvec_construct(vec, char *);
  cvec_push(vec, "alpha");
  cvec_push(vec, (char *)NULL);
  cvec_push(vec, "beta");
  cvec_push(vec, (char *)NULL);

  REQUIRE_EQ(cvec_find(vec, (char *)NULL), (size_t)1);

  cvec_destroy(vec);
}

// Here a vector with no NULL element at all gets a NULL needle, so every
// comparison has a NULL on one side only, and none of them may reach
// strcmp().
TEST(null_strings, find_a_null_needle_in_a_vector_without_null_elements) {
  cvec_construct(vec, char *);
  cvec_push(vec, "alpha");
  cvec_push(vec, "beta");

  REQUIRE_EQ(cvec_find(vec, (char *)NULL), ccol_invalid_size);

  cvec_destroy(vec);
}

// Sorting the same shape: every NULL lands at the front and the non-NULL
// elements stay in strcmp() order after them.
TEST(null_strings, sort_a_char_ptr_vector_holding_null_elements) {
  cvec_construct(vec, char *);
  cvec_push(vec, "pear");
  cvec_push(vec, (char *)NULL);
  cvec_push(vec, "apple");
  cvec_push(vec, "fig");
  cvec_push(vec, (char *)NULL);

  cvec_sort(vec);

  REQUIRE_EQ((void *)cvec_at(vec, 0), NULL);
  REQUIRE_EQ((void *)cvec_at(vec, 1), NULL);
  REQUIRE_STREQ(cvec_at(vec, 2), "apple");
  REQUIRE_STREQ(cvec_at(vec, 3), "fig");
  REQUIRE_STREQ(cvec_at(vec, 4), "pear");

  cvec_destroy(vec);
}

// ========================================================================
// cvector_append_array source-range bound
// ========================================================================

// A source range that aliases the vector may read past v's live element
// count, into capacity that is reserved but not live yet, but it may never
// read past the reservation itself, because the bytes after that belong to
// no allocation of the vector and reading them is a heap over-read. The call
// has to reject such a request with false and leave the vector completely
// untouched. Remove the bound and the test notices: the call reports true
// and AddressSanitizer reports a heap-buffer-overflow READ inside the
// copy.
TEST(cvectors, append_array_rejects_a_source_range_past_the_reservation) {
  cvector *cvec = cvector_create(sizeof(int), NULL);
  REQUIRE_TRUE(cvector_reserve(cvec, (size_t)16));
  REQUIRE_EQ(cvector_get_capacity(cvec), (size_t)16);
  cvector_push_back(cvec, &(int){7});

  int *base = (int *)cvector_data_ptr(cvec);
  // The source starts at the last reserved slot, so only one element can be
  // read from there while 15 are requested.
  REQUIRE_FALSE(cvector_append_array(cvec, base + 15, (size_t)15));
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 7);

  cvector_destroy(cvec);
}

// This sweeps the bound's own boundary, from inside it to one element past
// it: a source range that ends exactly at the end of the reservation is
// accepted, and the same range with one more element is rejected. Asserting
// only the accepted side would leave the rejection untested, and asserting
// only the rejected side wouldn't show that the bound is exactly
// right.
TEST(cvectors, append_array_accepts_a_source_range_ending_at_the_reservation) {
  cvector *cvec = cvector_create(sizeof(int), NULL);
  REQUIRE_TRUE(cvector_reserve(cvec, (size_t)16));
  REQUIRE_EQ(cvector_get_capacity(cvec), (size_t)16);

  int *base = (int *)cvector_data_ptr(cvec);
  for (int i = 0; i < 16; ++i) {
    base[i] = i;
  }

  // One live element, and the source is [12, 16), which ends exactly at the
  // end of the reserved region.
  cvector_push_back(cvec, &(int){100});
  REQUIRE_TRUE(cvector_append_array(cvec, base + 12, (size_t)4));
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)5);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 12);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 4), 15);

  // The same start offset with one more element runs one slot past the end
  // of the grown reservation, so it's refused.
  size_t capacity = cvector_get_capacity(cvec);
  base = (int *)cvector_data_ptr(cvec);
  REQUIRE_FALSE(cvector_append_array(cvec, base + (capacity - 4), (size_t)5));
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)5);

  cvector_destroy(cvec);
}

/* A vector whose element type is a char pointer that is const itself. Its
 * default comparator is the string comparator, so cvec_find matches on the
 * string's content and not on the address in the pointer, and cvec_sort
 * orders by that content. */
TEST(qualified_char_ptr, find_matches_content_for_a_const_pointer_element) {
  static const char *const needle = "beta";
  char needle_copy[8] = "beta";
  cvec_construct(v, const char *const);
  cvec_push(v, "alpha");
  cvec_push(v, "beta");
  cvec_push(v, "gamma");
  const char *const copy_ptr = needle_copy;
  size_t by_static = cvec_find(v, needle);
  size_t by_copy = cvec_find(v, copy_ptr);
  size_t absent = cvec_find(v, "delta");
  cvec_destroy(v);
  REQUIRE_EQ(by_static, (size_t)1);
  REQUIRE_EQ(by_copy, (size_t)1);
  REQUIRE_EQ(absent, ccol_invalid_size);
}

TEST(qualified_char_ptr, sort_orders_const_pointer_elements_by_content) {
  char buf_c[4] = "c", buf_a[4] = "a", buf_b[4] = "b";
  cvec_construct(v, const char *const);
  cvec_push(v, buf_c);
  cvec_push(v, buf_a);
  cvec_push(v, buf_b);
  cvec_sort(v);
  char got[4] = {0};
  for (size_t i = 0; i < 3; i++) {
    got[i] = (*(const char *const *)cvector_at(v, i))[0];
  }
  cvec_destroy(v);
  REQUIRE_STREQ(got, "abc");
}

TEST(qualified_char_ptr, sort_and_find_for_signed_and_unsigned_const_pointers) {
  unsigned char ub[3][2] = {{'z', 0}, {'m', 0}, {'a', 0}};
  cvec_construct(uv, const unsigned char *const);
  for (size_t i = 0; i < 3; i++) cvec_push(uv, ub[i]);
  cvec_sort(uv);
  char ugot[4] = {0};
  for (size_t i = 0; i < 3; i++) {
    ugot[i] = (char)(*(const unsigned char *const *)cvector_at(uv, i))[0];
  }
  unsigned char uneedle_buf[2] = {'m', 0};
  const unsigned char *const uneedle = uneedle_buf;
  size_t uidx = cvec_find(uv, uneedle);
  cvec_destroy(uv);

  signed char sb[2][2] = {{'y', 0}, {'b', 0}};
  cvec_construct(sv, signed char *const);
  for (size_t i = 0; i < 2; i++) cvec_push(sv, sb[i]);
  cvec_sort(sv);
  char sgot[3] = {0};
  for (size_t i = 0; i < 2; i++) {
    sgot[i] = (char)(*(signed char *const *)cvector_at(sv, i))[0];
  }
  signed char sneedle_buf[2] = {'y', 0};
  signed char *const sneedle = sneedle_buf;
  size_t sidx = cvec_find(sv, sneedle);
  cvec_destroy(sv);

  REQUIRE_STREQ(ugot, "amz");
  REQUIRE_EQ(uidx, (size_t)1);
  REQUIRE_STREQ(sgot, "by");
  REQUIRE_EQ(sidx, (size_t)1);
}

/* ========================================================================== */
/*          AN ITERATOR THAT OUTLIVES ITS CONTAINER UNTIL SCOPE EXIT          */
/* ========================================================================== */

/* An allocator that overwrites every block with a poison pattern before it
 * frees it, so reading freed memory gives the poison and not the stale
 * value, and a use after free through a stale pointer faults in an ordinary
 * run without needing a sanitizer to show up. */
typedef union {
  size_t size;
  max_align_t align;
} cvec_life_poison_hdr;

static void *cvec_life_poison_malloc(size_t size) {
  cvec_life_poison_hdr *h = malloc(sizeof(cvec_life_poison_hdr) + size);
  if (!h) return NULL;
  h->size = size;
  return h + 1;
}

static void *cvec_life_poison_calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return NULL;
  void *p = cvec_life_poison_malloc(count * size);
  if (p) memset(p, 0, count * size);
  return p;
}

static void cvec_life_poison_free(void *ptr) {
  if (!ptr) return;
  cvec_life_poison_hdr *h = (cvec_life_poison_hdr *)ptr - 1;
  memset(ptr, 0xA5, h->size);
  free(h);
}

static void *cvec_life_poison_realloc(void *ptr, size_t size) {
  if (!ptr) return cvec_life_poison_malloc(size);
  cvec_life_poison_hdr *h = (cvec_life_poison_hdr *)ptr - 1;
  void *n = cvec_life_poison_malloc(size);
  if (!n) return NULL;
  memcpy(n, ptr, h->size < size ? h->size : size);
  cvec_life_poison_free(ptr);
  return n;
}

static ccol_memmgmt_procs_t cvec_life_poison_procs_storage = {
    .malloc = cvec_life_poison_malloc,
    .calloc = cvec_life_poison_calloc,
    .realloc = cvec_life_poison_realloc,
    .free = cvec_life_poison_free};

static ccol_memmgmt_procs_t *cvec_life_poison_procs(void) {
  return &cvec_life_poison_procs_storage;
}

/* A loop that leaves early keeps its iterator live until the end of the
 * ccol_iter_declare scope. Destroying the vector inside that scope has to
 * leave the iterator freeable, because the scope-exit cleanup frees it after
 * the vector and its allocator record are gone. An iterator that freed
 * itself through the vector would read the poisoned vector struct at scope
 * exit and fault, so this test would catch it. */
TEST(iterator_lifetime, break_then_destroy_then_scope_exit_custom_allocator) {
  int seen = 0;
  {
    cvec_construct_mp(v, int, cvec_life_poison_procs());
    for (int i = 0; i < 6; i++) cvec_push(v, i);
    ccol_iter_declare(v, it);
    for (it = ccol_begin(v); it; it = ccol_iter_next(it)) {
      if (++seen == 2) break;
    }
    cvec_destroy(v);
  }
  REQUIRE_EQ(seen, 2);
}

TEST(iterator_lifetime, break_then_destroy_then_scope_exit_default_allocator) {
  int seen = 0;
  {
    cvec_construct(v, int);
    for (int i = 0; i < 6; i++) cvec_push(v, i);
    ccol_iter_declare(v, it);
    for (it = ccol_begin(v); it; it = ccol_iter_next(it)) {
      if (++seen == 3) break;
    }
    cvec_destroy(v);
  }
  REQUIRE_EQ(seen, 3);
}

/* ========================================================================== */
/*           cvec_sort USES THE CONTIGUOUS PATH AND MATCHES csort_sort */
/* ========================================================================== */

#include <internal/csortimpl.h>
#include <internal/test_helpers/csortimpl.h>
#include <math.h>

static unsigned long long g_cvec_sort_getter_calls = 0;

static void *cvec_sort_counting_getter(void *collection, size_t index) {
  g_cvec_sort_getter_calls++;
  return cvector_at((cvec)collection, index);
}

typedef struct {
  int key;
  int order;
  int pad;
} cvec_sort_stable_rec;

static int cvec_sort_stable_rec_cmp(const void *a, const void *b) {
  int ka = ((const cvec_sort_stable_rec *)a)->key;
  int kb = ((const cvec_sort_stable_rec *)b)->key;
  return (ka > kb) - (ka < kb);
}

static unsigned cvec_sort_lcg(unsigned *state) {
  *state = *state * 1103515245u + 12345u;
  return (*state >> 16) & 0x7fff;
}

/* Sorts one copy through cvec_sort and another through csort_sort's getter
 * path, and requires the same elements in the same order. The getter path is
 * the reference, since it's the general algorithm with the same pass
 * structure. eq compares two elements, and for a long double it compares
 * values, because the padding bytes of two equal values can differ. */
#define CVEC_SORT_MATCH_CASE(type, fill_expr, cmp, eq)                     \
  do {                                                                     \
    static const size_t lens[] = {0, 1, 2, 3, 5, 16, 17, 100, 1000, 4099}; \
    for (size_t li = 0; li < sizeof(lens) / sizeof(*lens); li++) {         \
      size_t n = lens[li];                                                 \
      unsigned st = 7u + (unsigned)n;                                      \
      cvec_construct(a, type);                                             \
      cvec_construct(b, type);                                             \
      for (size_t i = 0; i < n; i++) {                                     \
        type val = (fill_expr);                                            \
        cvec_push(a, val);                                                 \
        cvec_push(b, val);                                                 \
      }                                                                    \
      unsigned long long before = _csort_contiguous_sorts_for_tests();     \
      cvector_sort_with_comparison_proc(a, (cmp));                         \
      unsigned long long after = _csort_contiguous_sorts_for_tests();      \
      bool took_path = n < 2 ? after == before : after == before + 1;      \
      bool ok = csort_sort(b, n, sizeof(type), cvec_sort_counting_getter,  \
                           (cmp), NULL);                                   \
      bool same = true;                                                    \
      for (size_t i = 0; i < n; i++) {                                     \
        type x = cvec_at(a, i);                                            \
        type y = cvec_at(b, i);                                            \
        if (!(eq)) same = false;                                           \
      }                                                                    \
      cvec_destroy(a);                                                     \
      cvec_destroy(b);                                                     \
      REQUIRE_TRUE(took_path);                                             \
      REQUIRE_TRUE(ok);                                                    \
      REQUIRE_TRUE(same);                                                  \
    }                                                                      \
  } while (0)

#define CVEC_SORT_BYTES_EQ (memcmp(&x, &y, sizeof(x)) == 0)
#define CVEC_SORT_FLOAT_EQ ((isnan(x) && isnan(y)) || x == y)

TEST(contiguous_sort, cvec_sort_matches_the_getter_path_for_every_width) {
  CVEC_SORT_MATCH_CASE(char, (char)(cvec_sort_lcg(&st) % 90),
                       csort_get_default_comparison_proc((char)0),
                       CVEC_SORT_BYTES_EQ);
  CVEC_SORT_MATCH_CASE(short, (short)((int)(cvec_sort_lcg(&st) % 50) - 25),
                       csort_get_default_comparison_proc((short)0),
                       CVEC_SORT_BYTES_EQ);
  CVEC_SORT_MATCH_CASE(int, (int)(cvec_sort_lcg(&st) % 300) - 150,
                       csort_get_default_comparison_proc((int)0),
                       CVEC_SORT_BYTES_EQ);
  CVEC_SORT_MATCH_CASE(long long, (long long)cvec_sort_lcg(&st) * 7919 - 9000,
                       csort_get_default_comparison_proc((long long)0),
                       CVEC_SORT_BYTES_EQ);
  CVEC_SORT_MATCH_CASE(
      double,
      (cvec_sort_lcg(&st) % 11 == 0) ? NAN : (double)cvec_sort_lcg(&st) / 7.0,
      csort_get_default_comparison_proc((double)0), CVEC_SORT_FLOAT_EQ);
  CVEC_SORT_MATCH_CASE(
      long double,
      (cvec_sort_lcg(&st) % 9 == 0) ? (long double)NAN
                                    : (long double)cvec_sort_lcg(&st) / 3.0L,
      csort_get_default_comparison_proc((long double)0), CVEC_SORT_FLOAT_EQ);
  /* 12 bytes, the width no constant specialization covers. The key range is
   * small, so ties are common and you can see the stability. */
  CVEC_SORT_MATCH_CASE(
      cvec_sort_stable_rec,
      ((cvec_sort_stable_rec){
          .key = (int)(cvec_sort_lcg(&st) % 8), .order = (int)i, .pad = 0}),
      cvec_sort_stable_rec_cmp, CVEC_SORT_BYTES_EQ);
}

TEST(contiguous_sort, cvec_sort_is_stable_and_makes_no_getter_call) {
  cvec_construct(v, cvec_sort_stable_rec);
  unsigned st = 99u;
  for (int i = 0; i < 2000; i++) {
    cvec_sort_stable_rec r = {
        .key = (int)(cvec_sort_lcg(&st) % 5), .order = i, .pad = 0};
    cvec_push(v, r);
  }
  g_cvec_sort_getter_calls = 0;
  unsigned long long before = _csort_contiguous_sorts_for_tests();
  cvector_sort_with_comparison_proc(v, cvec_sort_stable_rec_cmp);
  bool took_path = _csort_contiguous_sorts_for_tests() == before + 1;
  bool stable = true;
  for (size_t i = 1; i < cvec_size(v); i++) {
    cvec_sort_stable_rec p = cvec_at(v, i - 1);
    cvec_sort_stable_rec c = cvec_at(v, i);
    if (p.key > c.key || (p.key == c.key && p.order > c.order)) {
      stable = false;
    }
  }
  cvec_destroy(v);
  REQUIRE_TRUE(took_path);
  REQUIRE_TRUE(stable);
  REQUIRE_EQ((size_t)g_cvec_sort_getter_calls, (size_t)0);
}

TEST(contiguous_sort, cvec_sort_orders_nan_after_every_number) {
  cvec_construct(v, double);
  double in[] = {9, NAN, 1, 4, NAN, 2, 7};
  for (size_t i = 0; i < sizeof(in) / sizeof(*in); i++) cvec_push(v, in[i]);
  cvec_sort(v);
  double out[7];
  for (size_t i = 0; i < 7; i++) out[i] = cvec_at(v, i);
  cvec_destroy(v);
  REQUIRE_EQ(out[0], 1.0);
  REQUIRE_EQ(out[1], 2.0);
  REQUIRE_EQ(out[2], 4.0);
  REQUIRE_EQ(out[3], 7.0);
  REQUIRE_EQ(out[4], 9.0);
  REQUIRE_TRUE(isnan(out[5]));
  REQUIRE_TRUE(isnan(out[6]));
}

/* ========================================================================== */
/*            cvec_at READS THE ELEMENT COUNT ON ITS FAILURE PATH ONLY        */
/* ========================================================================== */

/* The Makefile links this suite with --wrap=cvector_elem_count, so every
 * call this file makes to cvector_elem_count, including the ones a macro
 * expands to, comes here first. A linker without --wrap (Apple's) can't
 * route the calls to a function the same binary defines, so there the test
 * below is skipped; what it checks is a property of the macro expansion,
 * which is the same on every system. */
#if !defined(TEST_NO_LD_WRAP)
size_t __real_cvector_elem_count(cvec v);
static unsigned long g_cvec_elem_count_calls = 0;
size_t __wrap_cvector_elem_count(cvec v);
size_t __wrap_cvector_elem_count(cvec v) {
  g_cvec_elem_count_calls++;
  return __real_cvector_elem_count(v);
}
#endif

TEST(cvec_at_cost, a_successful_access_does_not_read_the_element_count) {
#if defined(TEST_NO_LD_WRAP)
  fprintf(stderr, "SKIP: the linker cannot wrap cvector_elem_count\n");
  return;
#else
  cvec_construct(v, int);
  for (int i = 0; i < 64; i++) cvec_push(v, i);
  g_cvec_elem_count_calls = 0;
  long sum = 0;
  for (size_t i = 0; i < 64; i++) sum += cvec_at(v, i);
  unsigned long calls = g_cvec_elem_count_calls;
  cvec_destroy(v);
  REQUIRE_EQ(sum, 64L * 63L / 2L);
  REQUIRE_EQ(calls, 0UL);
#endif
}

TEST(cvec_at_cost, the_out_of_bounds_diagnostic_still_reports_the_size) {
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    dup2(fds[1], STDOUT_FILENO);
    close(fds[1]);
    cvec_construct(vec, int);
    cvec_push(vec, 1);
    cvec_push(vec, 2);
    cvec_push(vec, 3);
    (void)cvec_at(vec, 7);
    _exit(0);
  }
  close(fds[1]);
  char buf[4096] = {0};
  size_t got = 0;
  ssize_t r;
  while (got < sizeof(buf) - 1 &&
         (r = read(fds[0], buf + got, sizeof(buf) - 1 - got)) > 0) {
    got += (size_t)r;
  }
  close(fds[0]);
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  REQUIRE_NE(pid, -1);
  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_NE((void *)strstr(buf, "index 7 out of bounds (size: 3)"), NULL);
}

/* ========================================================================== */
/*                  cvec_push ACCEPTS A STRUCT RVALUE                         */
/* ========================================================================== */

typedef struct {
  int x;
  int y;
} cvec_rv_point;

static cvec_rv_point cvec_rv_make_point(int x, int y) {
  cvec_rv_point p = {.x = x, .y = y};
  return p;
}

/* A brace initializer would take a struct rvalue as the initializer of the
 * first member, which doesn't compile, so against a macro that pushes
 * through one this test fails to build. */
TEST(push_expressions, a_struct_rvalue_from_a_function_is_pushed_whole) {
  cvec_construct(v, cvec_rv_point);
  cvec_push(v, cvec_rv_make_point(3, 4));
  cvec_push(v, ((cvec_rv_point){.x = 5, .y = 6}));
  cvec_rv_point a = cvec_at(v, 0);
  cvec_rv_point b = cvec_at(v, 1);
  size_t n = cvec_size(v);
  cvec_destroy(v);
  REQUIRE_EQ(n, (size_t)2);
  REQUIRE_EQ(a.x, 3);
  REQUIRE_EQ(a.y, 4);
  REQUIRE_EQ(b.x, 5);
  REQUIRE_EQ(b.y, 6);
}

TEST(push_expressions, a_same_size_value_of_another_type_is_converted) {
  cvec_construct(v, int);
  float f = 3.0f;
  double d = 7.9;
  cvec_push(v, f);
  cvec_push(v, d);
  int a = cvec_at(v, 0);
  int b = cvec_at(v, 1);
  cvec_destroy(v);
  REQUIRE_EQ(a, 3);
  REQUIRE_EQ(b, 7);
}

// ========================================================================
// PADDING OF THE VALUE TEMPORARIES
// ========================================================================

#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define CVEC_TEST_COMPILER_CLEARS_PADDING 1
#endif
#endif

/* padding sits between tag and id on every supported target. The third
 * member makes the struct too wide to return in registers, so a function
 * that returns it writes straight into its caller's object, padding
 * included. */
typedef struct {
  char tag;
  long id;
  long spare;
} cvec_padded_elem;

/* A value built by member assignment over storage that holds a non-zero
 * byte pattern, so two values with the same members differ in their
 * padding. */
static __attribute__((noinline)) cvec_padded_elem
cvec_padded_make(char tag, long id, unsigned char garbage) {
  cvec_padded_elem e;
  memset(&e, garbage, sizeof(e));
  e.tag = tag;
  e.id = id;
  e.spare = 0;
  return e;
}

#ifdef CVEC_TEST_COMPILER_CLEARS_PADDING
/* Fills the stack below the caller with a non-zero pattern, so a temporary
 * whose padding nothing writes shows it as non-zero bytes. */
static __attribute__((noinline)) void cvec_padded_dirty_stack(void) {
  volatile unsigned char buf[4096];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0xAB;
}

static __attribute__((noinline)) void cvec_padded_push(cvec v, int i) {
  cvec_redeclare(v, cvec_padded_elem);
  if (i % 2) {
    cvec_push(v, cvec_padded_make((char)('a' + i % 26), i, 0xC3));
  } else {
    cvec_padded_elem e = cvec_padded_make((char)('a' + i % 26), i, 0x3C);
    cvec_push(v, e);
  }
}

static __attribute__((noinline)) size_t cvec_padded_find(cvec v, int i) {
  cvec_redeclare(v, cvec_padded_elem);
  return cvec_find(v, cvec_padded_make((char)('a' + i % 26), i, 0x5C));
}
#endif

/* When the compiler has __builtin_clear_padding, cvec_push and cvec_find
 * clear the padding of their own copy of the value, so the stored elements
 * have zero padding, and cvec_find's byte-wise fallback matches a needle
 * with equal members whatever its padding bytes hold. Under GCC the test
 * catches a missing clear: without it the stored elements carry the value's
 * pattern in their padding and every lookup fails. */
TEST(padding, the_value_macros_clear_the_padding_of_their_copy) {
#ifdef CVEC_TEST_COMPILER_CLEARS_PADDING
  cvec_construct(v, cvec_padded_elem);
  enum { N = 100 };
  for (int i = 0; i < N; i++) {
    cvec_padded_dirty_stack();
    cvec_padded_push(v, i);
  }
  bool padding_zero = true;
  size_t pad_start = offsetof(cvec_padded_elem, tag) + 1;
  size_t pad_end = offsetof(cvec_padded_elem, id);
  for (size_t i = 0; i < cvec_size(v); i++) {
    const unsigned char *b = (const unsigned char *)cvector_at(v, i);
    for (size_t j = pad_start; j < pad_end; j++) {
      if (b[j] != 0) padding_zero = false;
    }
  }
  size_t found = 0;
  for (int i = 0; i < N; i++) {
    cvec_padded_dirty_stack();
    if (cvec_padded_find(v, i) == (size_t)i) found++;
  }
  size_t n = cvec_size(v);
  cvec_destroy(v);
  REQUIRE_EQ(n, (size_t)N);
  REQUIRE_TRUE(padding_zero);
  REQUIRE_EQ(found, (size_t)N);
#endif
}

/* A value macro's temporary drops the top-level qualifiers of the element
 * type and keeps an array element type whole, so both shapes push and find,
 * with the padding clear in place. */
typedef char cvec_padding_char_array[16];

TEST(padding, qualified_and_array_element_types_push_and_find) {
  cvec_construct(cv, const cvec_padded_elem);
  cvec_padded_elem a = cvec_padded_make('q', 7, 0);
  cvec_push(cv, a);
  cvec_push(cv, cvec_padded_make('r', 8, 0));
  size_t found_const = cvec_find(cv, cvec_padded_make('r', 8, 0));
  size_t n_const = cvec_size(cv);
  cvec_destroy(cv);

  cvec_construct(av, cvec_padding_char_array);
  cvec_push(av, "first");
  cvec_push(av, "second");
  size_t found_array = cvec_find(av, "second");
  char second[16] = {0};
  memcpy(second, cvector_at(av, 1), sizeof(second));
  size_t elem_size = sizeof(*av__ccol_val_type_var);
  cvec_destroy(av);

  REQUIRE_EQ(n_const, (size_t)2);
  REQUIRE_EQ(found_const, (size_t)1);
  REQUIRE_EQ(found_array, (size_t)1);
  REQUIRE_STREQ(second, "second");
  REQUIRE_EQ(elem_size, (size_t)16);
}

// ========================================================================
// APPEND FROM A CONST ARRAY
// ========================================================================

static const int cvec_const_source_table[] = {3, 1, 4, 1, 5};

/* cvector_append_array() takes a pointer to const, so appending from a const
 * array compiles under -Werror, through the function and through the macro.
 * If the parameter lost its const, this test wouldn't build. */
TEST(cvectors, append_array_accepts_a_const_source) {
  cvec_construct(v, int);
  bool raw = cvector_append_array(v, cvec_const_source_table, 5);
  cvec_append_array(v, cvec_const_source_table, 2);
  size_t n = cvec_size(v);
  int got[7] = {0};
  for (size_t i = 0; i < n && i < 7; i++) got[i] = cvec_at(v, i);
  cvec_destroy(v);
  REQUIRE_TRUE(raw);
  REQUIRE_EQ(n, (size_t)7);
  for (size_t i = 0; i < 7; i++) {
    REQUIRE_EQ(got[i], cvec_const_source_table[i % 5]);
  }
}

// ========================================================================
// SINGLE EVALUATION OF THE VECTOR ARGUMENT
// ========================================================================

static int cvec_single_eval_cmp_int(const void *a, const void *b) {
  int x, y;
  memcpy(&x, a, sizeof(x));
  memcpy(&y, b, sizeof(y));
  return (x > y) - (x < y);
}

/* cvector_sort_with_comparison_proc takes a plain vector expression and not
 * a name it pastes, so the expression can have a side effect. The macro
 * evaluates it once: the vector it checks is the vector it sorts, and the
 * side effect happens once. */
TEST(cvectors, sort_with_comparison_proc_evaluates_the_vector_once) {
  cvec vs[2];
  vs[0] = cvector_create(sizeof(int), NULL);
  vs[1] = cvector_create(sizeof(int), NULL);
  int vals[] = {3, 1, 2};
  cvector_append_array(vs[0], vals, 3);
  cvector_append_array(vs[1], vals, 3);
  int k = 0;
  cvector_sort_with_comparison_proc(vs[k++], cvec_single_eval_cmp_int);
  int first0 = *(int *)cvector_at(vs[0], 0);
  int first1 = *(int *)cvector_at(vs[1], 0);
  cvector_destroy(vs[0]);
  cvector_destroy(vs[1]);
  REQUIRE_EQ(k, 1);
  REQUIRE_EQ(first0, 1);
  REQUIRE_EQ(first1, 3);
}

/* cvector_destroy evaluates its lvalue once, so the element it frees is the
 * element it sets to NULL, and no neighbour is read or written. */
TEST(cvectors, destroy_evaluates_the_vector_once) {
  cvec vs[4] = {NULL, NULL, NULL, NULL};
  vs[0] = cvector_create(sizeof(int), NULL);
  vs[1] = cvector_create(sizeof(int), NULL);
  cvec first = vs[0];
  int k = 0;
  cvector_destroy(vs[k++]);
  bool first_cleared = vs[0] == NULL;
  bool second_kept = vs[1] != NULL;
  if (k == 1) {
    cvector_destroy(vs[1]);
  } else {
    /* A macro that evaluates its argument more than once frees vs[1]
     * instead of vs[0]. We free the survivor so the failure below is a
     * clean one. */
    __cvector_destroy(first);
  }
  REQUIRE_EQ(k, 1);
  REQUIRE_TRUE(first_cleared);
  REQUIRE_TRUE(second_kept);
}

/* ------------------------------------------------------------------------ */
/* Element types whose declarator is not "type *name"                        */
/* ------------------------------------------------------------------------ */

static int cvec_fp_elem_calls;
static void cvec_fp_elem_add(int x) { cvec_fp_elem_calls += x; }
static void cvec_fp_elem_twice(int x) { cvec_fp_elem_calls += 2 * x; }

static int cvec_fp_elem_sum_via_redeclare(cvec fns) {
  cvec_redeclare(fns, void (*)(int));
  cvec_fp_elem_calls = 0;
  for (size_t i = 0; i < cvec_size(fns); i++) cvec_at(fns, i)(10);
  return cvec_fp_elem_calls;
}

/* A function-pointer element type and an array element type can't be written
 * as "type *name", so the companion variable of cvec_declare(),
 * cvec_declare_scoped() and cvec_redeclare() is spelled through __typeof__.
 * With the plain spelling this file doesn't compile. */
TEST(cvec_elem_type_spelling, a_function_pointer_element_type) {
  cvec_construct(fns, void (*)(int));
  cvec_push(fns, cvec_fp_elem_add);
  cvec_push(fns, cvec_fp_elem_twice);
  int sum = cvec_fp_elem_sum_via_redeclare(fns);
  size_t n = cvec_size(fns);
  cvec_destroy(fns);
  REQUIRE_EQ(n, (size_t)2);
  REQUIRE_EQ(sum, 30);
}

TEST(cvec_elem_type_spelling, a_scoped_array_element_type) {
  int total = 0;
  size_t n = 0;
  {
    cvec_declare_scoped(rows, int[4]);
    cvec_init(rows);
    int r0[4] = {1, 2, 3, 4};
    int r1[4] = {10, 20, 30, 40};
    bool ok0 = cvector_push_back(rows, r0) == ccol_success;
    bool ok1 = cvector_push_back(rows, r1) == ccol_success;
    n = cvec_size(rows);
    if (ok0 && ok1) total = cvec_at(rows, 0)[3] + cvec_at(rows, 1)[1];
    REQUIRE_EQ(sizeof(*rows__ccol_val_type_var), sizeof(int[4]));
  }
  REQUIRE_EQ(n, (size_t)2);
  REQUIRE_EQ(total, 24);
}

/* A vector of char * stores the pointer itself, so a NULL element, like
 * what getenv() gives for a variable that isn't set, is an ordinary element
 * that cvec_find() matches. */
TEST(cvec_elem_type_spelling, a_null_char_pointer_element) {
  unsetenv("CCOL_CVEC_TEST_SURELY_UNSET_VARIABLE");
  cvec_construct(names, char *);
  cvec_push(names, "HOME");
  cvec_push(names, getenv("CCOL_CVEC_TEST_SURELY_UNSET_VARIABLE"));
  size_t idx = cvec_find(names, getenv("CCOL_CVEC_TEST_SURELY_UNSET_VARIABLE"));
  size_t idx_home = cvec_find(names, "HOME");
  char *stored = cvec_at(names, 1);
  cvec_destroy(names);
  REQUIRE_EQ(idx, (size_t)1);
  REQUIRE_EQ(idx_home, (size_t)0);
  REQUIRE_EQ((void *)stored, NULL);
}

/* cvec_pop compiles and works for a vector with a const-qualified element
 * type, because its temporary, which cvector_pop_back() writes, has the
 * element type without the qualifiers. Get that wrong and the build breaks:
 * a temporary declared with the qualified element type makes this file fail
 * to compile with -Werror=discarded-qualifiers. */
TEST(cvec_macros, pop_of_a_const_element_type) {
  cvec_construct(cv, const int);
  cvec_push(cv, 3);
  cvec_push(cv, 4);
  int a = cvec_pop(cv);
  int b = cvec_pop(cv);
  size_t left = cvector_elem_count(cv);
  cvec_destroy(cv);
  REQUIRE_EQ(a, 4);
  REQUIRE_EQ(b, 3);
  REQUIRE_EQ(left, (size_t)0);
}

#include <internal/cprocsintern.h>

static long cvec_tally;
static void *cvec_tally_malloc(size_t n) {
  cvec_tally++;
  return malloc(n);
}
static void *cvec_tally_calloc(size_t a, size_t b) {
  cvec_tally++;
  return calloc(a, b);
}
static void *cvec_tally_realloc(void *p, size_t n) {
  cvec_tally++;
  return realloc(p, n);
}

/* A vector costs two allocations, its own block and its element buffer,
 * whether the procs it gets are interned or the caller's own: an interned
 * procs pointer lives as long as the process, and any other procs struct is
 * copied into the tail of the vector's block. Both kinds work for every later
 * allocation and for the destroy.
 *
 * A vector that copied the procs into an allocation of their own would
 * cost three, and the count here would show it. */
TEST(cvector_procs, a_vector_costs_two_allocations_with_any_procs) {
  ccol_memmgmt_procs_t own = {cvec_tally_malloc, free, cvec_tally_calloc,
                              cvec_tally_realloc};
  ccol_memmgmt_procs_t *interned = NULL;
  REQUIRE_EQ(ccol_procs_intern(&own, &interned), ccol_success);
  REQUIRE_TRUE(ccol_procs_is_interned(interned));

  ccol_memmgmt_procs_t *kinds[2] = {&own, interned};
  long counts[2];
  bool pushed[2];
  for (int k = 0; k < 2; k++) {
    cvec_tally = 0;
    cvec v = cvector_create_full(sizeof(int), kinds[k], NULL);
    counts[k] = cvec_tally;
    pushed[k] = v != NULL;
    for (int i = 0; v && i < 100; i++) {
      if (cvector_push_back(v, &i) != ccol_success) pushed[k] = false;
    }
    if (v && cvector_get_mprocs(v)->malloc != cvec_tally_malloc)
      pushed[k] = false;
    cvector_destroy(v);
  }
  REQUIRE_EQ(counts[0], 2L);
  REQUIRE_EQ(counts[1], 2L);
  REQUIRE_TRUE(pushed[0]);
  REQUIRE_TRUE(pushed[1]);
}
