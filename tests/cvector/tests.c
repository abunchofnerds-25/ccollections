#include <common_invariants.h>
#include <cvector.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <unistd.h>
TAU_MAIN()  // sets up Tau (+ main function)

extern size_t cvector_get_capacity(cvector *v);
extern const size_t _ccol_cvector_minimum_capacity;
extern const size_t _ccol_cvector_scaling_factor;
extern size_t cvector_overlap_copy_count_for_tests;

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

// The first allocation of the backing buffer holds 4 elements. Its byte count
// is _ccol_cvector_minimum_capacity * elem_size. An elem_size large enough to
// overflow size_t there must be rejected at once. Without that, the product
// wraps silently to a small allocation, and v->elem_size still records the
// real, huge value.
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
// must still be accepted; the overflow guard must not be over-strict.
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
  // The vector keeps its own copy of the procs struct on the heap. It does
  // not keep the pointer of the caller. But the function pointers inside that
  // copy must match the ones from the creation call.
  REQUIRE_NE((void *)stored, (void *)&custom);
  REQUIRE_TRUE(stored->malloc == custom.malloc);
  REQUIRE_TRUE(stored->free == custom.free);
  REQUIRE_TRUE(stored->calloc == custom.calloc);
  REQUIRE_TRUE(stored->realloc == custom.realloc);
  cvector_destroy(custom_cvec);
}

// cvector_get_mprocs(NULL) must call ccol_fatal_err(), which asserts. It must
// not dereference a NULL vector directly. Every other accessor in this module
// behaves in the same way: cvector_at, cvector_elem_count, cvector_reset,
// cvector_data_ptr, cvector_find, cvector_push_back, cvector_pop_back and
// cvector_reserve. This test runs in a forked child, because the assertion
// stops the whole process. type_safe_at_out_of_bounds_is_fatal has the same
// shape.
TEST(cvectors, get_mprocs_null_vec_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    (void)cvector_get_mprocs(NULL); /* NULL vector; must ccol_fatal_err() */
    _exit(0); /* unreachable if ccol_fatal_err() aborted */
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

// new_elem aliasing into v's own backing buffer must remain safe even when
// scale_the_cvector_size_up reallocates the buffer to a new address mid-call.
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
  // The vector holds 4 elements and its capacity is 4, so it is exactly
  // full. A push of an element that is already in the vector back onto
  // itself therefore forces scale_the_cvector_size_up to call realloc.
  // Without a guard, new_elem points into the buffer of the vector itself. It
  // dangles the moment realloc moves that buffer, and the copy after that
  // reads freed memory.
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
  // The same hazard is reachable through cvec_push, since it just takes the
  // address of its (possibly vector-derived) lvalue argument.
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

  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

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
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  cvector_reserve(cvec, 2);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

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

  // Try to append SIZE_MAX elements with a valid pointer. This must fail,
  // because the byte count overflows.
  int dummy[1] = {0};
  int result = (int)cvector_append_array(cvec, dummy, SIZE_MAX);
  REQUIRE_EQ(result, false);

  cvector_destroy(cvec);
}

// arr_ptr aliasing into v's own backing buffer must remain safe even when
// cvector_reserve reallocates the buffer to a new address mid-call.
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
  // Source range ends exactly where the destination begins (touching, not
  // overlapping): the cheaper memcpy path must still be used, not
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
  // needs 6 slots, forcing cvector_reserve to realloc. Unguarded, arr_ptr
  // (== cvector_data_ptr(cvec)) dangles the moment that realloc moves the
  // buffer, and the second copy reads freed memory.
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
  // its start), covering the byte-offset-preservation logic distinctly from
  // the whole-buffer-aliasing case above.
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

// arr_ptr can alias the buffer of v at an offset where the requested
// elem_count reads past the *live* elem_count of the vector. It then reads
// into capacity that is reserved but not yet live. The source byte range and
// the destination byte range then genuinely overlap. Every self-alias test
// above is different, because its source range always ends exactly where the
// destination begins. A plain memcpy is not safe for such an overlap, and a
// simple forward copy of bytes is not either. The write of the first appended
// element can destroy source bytes that a later appended element still needs.
// cvector_copy_into_tail must detect this and use a memmove, which is safe
// for an overlap.
//
// The two tests below use a large element count with a shift of only ONE
// ELEMENT between the source range and the destination range. Nearly the
// whole copy therefore overlaps itself. The size and the shift are
// load-bearing, and not incidental. A smaller shift and size drive the exact
// same code path, and they can still pass with the overlap check turned off.
// An overlap is undefined behavior, and not behavior that is guaranteed to be
// wrong. A given memcpy can therefore happen not to corrupt a few small
// elements at that size and shift. A huge total size with a shift of one
// element is the classic pattern. It is a "shift an array by one in place
// with a memcpy instead of a memmove". Any implementation that copies in
// pieces
// smaller than the whole buffer cannot get this right without the overlap
// handling of memmove. Every real memcpy copies in such pieces, by design,
// for anything longer than a few bytes. This test therefore reproduces
// reliably, whatever memcpy and whatever compiler the build uses.
TEST(cvectors, append_array_self_alias_overlap_shift_by_one_no_realloc) {
  const int n = 10000;
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){0});
  cvector_push_back(cvec, &(int){1});
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)2);

  // Grow the capacity and leave elem_count alone. Then fill the spare region
  // with deterministic values, directly through the raw data pointer. That
  // region is allocated but not yet live. This goes around push_back
  // completely, so no shrink logic ever runs.
  REQUIRE_TRUE(cvector_reserve(cvec, (size_t)n));
  int *base = cvector_data_ptr(cvec);
  for (int i = 2; i < n; ++i) {
    base[i] = i;
  }

  // Reads indices [1, n-1) (values 1..n-2); writes indices [2, n). Source
  // and destination overlap everywhere except the very first source
  // element and the very last destination slot.
  size_t overlap_count_before = cvector_overlap_copy_count_for_tests;
  int result = (int)cvector_append_array(cvec, base + 1, (size_t)(n - 2));
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)n);
  // This is the whole point of this test. It confirms that the code took the
  // memmove branch, which is safe for an overlap. It does not trust the
  // output of the copy alone. No contract makes a real memcpy corrupt an
  // overlapping copy. A memcpy is only permitted to corrupt one.
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
  // (cap - 1) = cap + 1 > capacity, forcing cvector_reserve to realloc.
  // The source range [1, cap) and the destination range [2, cap+1) overlap
  // almost completely, with a shift of one element. The copy must survive
  // both things correctly. The realloc is the first, and the code recomputes
  // arr_ptr against the new buffer. The overlap is the second.
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

  // Realloc path: 3 elements, capacity=4, self-append needs 6 slots -> realloc.
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
  REQUIRE_NE(large_capacity, _ccol_cvector_minimum_capacity);

  cvector_reset(cvec);

  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), _ccol_cvector_minimum_capacity);

  // Verify vector still works
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

// cvector_reset must not touch the allocator at all when the vector already
// sits at _ccol_cvector_minimum_capacity. That shrink does nothing.
// scale_the_cvector_size_down has an early-return guard for the same case.
// cvector_reset must still shrink, and therefore call realloc, when a shrink
// is genuinely needed.
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

  // Grow past minimum, then reset: this time a shrinking realloc is expected.
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

  // Fill to exact minimum capacity
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

  // Should have some elements remaining
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

// cvec_at stops the process with ccol_fatal_err() for an index that is out of
// bounds. Every other type-inferred macro has the same "aborting convenience
// API" contract. cvector_at() returns NULL for such an index, and a direct
// dereference of that NULL gives a SIGSEGV instead. This test runs in a
// forked child, because ccol_fatal_err stops the whole process.
// tests/cthreadpool/tests.c uses a forked child for misuse that stops the
// process in the same way.
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
                  1); /* out of bounds; must ccol_fatal_err(), not SIGSEGV */
    _exit(0);         /* unreachable if ccol_fatal_err() aborted as expected */
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
      cvec_push(v, i);
    }
    REQUIRE_EQ(cvector_elem_count(v), 5);
    for (int i = 0; i < 5; ++i) {
      REQUIRE_EQ(cvec_at(v, i), i + 1);
    }
    // v is automatically destroyed at end of block (no cvec_destroy needed)
  }
}

// ========================================================================
// MACRO HYGIENE TESTS
// ========================================================================
//
// cvec_push and cvec_pop each declare an internal local that holds a result.
// Neither of them can give that local the plain name 'r'. cvec_pop also holds
// an element in a local, and that local cannot have the plain name '_tmp'. The
// problem is in the SAME statement whose initializer holds the parameters of
// the macro. The preprocessor substitutes these parameters as text. In C, the
// scope of a declared identifier starts directly after its own declarator.
// That is before the initializer is evaluated. The usual `int x = x;` problem
// is the same rule.
//
// A caller can give the bare name 'r' to its own vector variable, or to the
// expression that it pushes or pops. This project frequently uses 'r' for a
// retval local. If a macro local had that name, the identifier of the
// caller would silently resolve to the local of the macro, and not to the
// real variable of the caller. That local has no value yet. The result would
// be incorrect data, with no compiler warning at any optimization level.
// Therefore, each such internal local has a name that is specific to its own
// macro. No real identifier of a caller can collide with it.

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
  // The internal local of cvec_pop that holds an element must not be a plain
  // '_tmp'. A vector variable that the caller names '_tmp' drives the
  // cross-statement form of the same hazard. Such a declaration in the body
  // of the statement expression of the macro declares '_tmp' again, with the
  // element type of the vector. It then shadows the cvec of the caller for
  // the rest of the scope of that statement expression.
  cvec_construct(_tmp, int);
  cvec_push(_tmp, 3);
  cvec_push(_tmp, 6);
  int popped = cvec_pop(_tmp);
  REQUIRE_EQ(popped, 6);
  REQUIRE_EQ(cvec_size(_tmp), (size_t)1);
  REQUIRE_EQ(cvec_at(_tmp, 0), 3);
  cvec_destroy(_tmp);
}

// cvec_at must not expand its index argument two times. One expansion serves
// the bounds-checked access of cvector_at(). The other serves the re-read
// that _cvec_at_checked() makes for its message alone. The two are arguments
// of the very same function call, so nothing sequences one against the other.
// For an ordinary variable or a literal index, a double expansion is
// invisible. For an index expression with a side effect, such as cvec_at(vec,
// i++), it is genuine undefined behavior. In practice the index variable then
// advances by 2 for each call, and not by 1. The loop silently skips every
// other element, and it walks off the end of the vector. A capture of the
// index into a local exactly one time, before either use, prevents that. The
// tests below read and write through a loop with a bare j++ index expression.
// They assert that the loop variable advances by exactly 1 for each
// iteration. They also assert that the loop visits every element exactly one
// time, in order.
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
  REQUIRE_EQ(j, 5);      // j must advance by exactly 1 per call
  REQUIRE_EQ(sum, 100);  // 0+10+20+30+40; double-eval would corrupt this

  cvec_destroy(vec);
}

TEST(cvectors, at_index_expression_with_side_effect_evaluated_once_as_lvalue) {
  cvec_construct(vec, int);
  for (int i = 0; i < 4; ++i) {
    cvec_push(vec, 0);
  }

  int j = 0;
  for (int n = 0; n < 4; ++n) {
    cvec_at(vec, j++) = n + 1;  // assignment-target usage must also single-eval
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
// expansion. cvec_append_array does the same with its elem_count argument. One
// place is the real call to cvector_reserve() or cvector_append_array(). The
// other place is the ccol_fatal_err() message, which the macro makes only on
// the failure path. Neither macro can do that without a guard that evaluates
// the argument one time. The index of cvec_at has such a guard (see the
// "at_index_expression_with_side_effect_evaluated_once*" tests above). The
// internal locals of cvec_push and cvec_pop also have one.
//
// Without that guard, an argument expression with a side effect runs a second
// time when the real call fails. That second run occurs only to make the
// message. It silently does the real side effect again, and it can show a
// value that is different from the value that the call used. All of this
// occurs immediately before ccol_fatal_err() stops the process. A macro that
// copies the argument into a local exactly one time prevents this. cvec_at
// does the same with its index.
//
// The two tests below use a deterministic failure. One test goes above
// ccol_max_elem_count, and the other test causes an overflow of the total of
// the append. Neither test uses an allocator that fails. Therefore, the call
// count below comes only from the expansion of the macro, and not from a retry
// in the allocator. The tests record that count in shared memory from mmap. The
// forked child is the process that does the double evaluation, and that child
// then aborts. Therefore, the parent can see only the state that the abort does
// not remove: memory that the two processes share across the fork.

static int *g_reserve_side_effect_call_count;
static size_t reserve_side_effecting_target(void) {
  (*g_reserve_side_effect_call_count)++;
  return ccol_max_elem_count + 1;  // guarantees cvector_reserve() fails
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
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
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
  return SIZE_MAX;  // guarantees cvector_append_array() fails via overflow
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
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
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
// cvec_push must not take the address of new_elem directly. It must not make
// a compound literal whose type is the OWN type of new_elem. It also must not
// use a _Static_assert over sizeof() as its only guard:
// - Such an assert finds a new_elem of a different SIZE. It therefore
//   prevents a read out of bounds.
// - It does nothing for a new_elem of a different TYPE and the *same* size.
// - A push of a `float` into a vector from cvec_construct(v, int) passes the
//   size check, because sizeof(float) == sizeof(int) on every mainstream
//   platform.
// - The raw 4-byte IEEE 754 bit pattern then goes byte for byte into the int
//   storage of the vector. A plain C assignment, such as
//   `int x = some_float;`, converts the value.
// - A read of that element gives the bits of the float read as an int, and not
//   the expected truncated integer value. No compiler shows a warning, at any
//   optimization level.
//
// Therefore, the internal temporary has the declared element type of v, and
// new_elem initializes it. The conversion then goes through the real assignment
// rules of the C compiler. cvec_find uses the same pattern. These tests push a
// value of a different type with the same size. They check the real VALUE that
// results. They do not only check that the call compiles and gives success. A
// push through a temporary with no type also compiles and gives ccol_success,
// but it silently stores an incorrect value.

TEST(cvectors,
     push_expression_float_into_int_vector_converts_not_reinterprets) {
  cvec_construct(vec, int);
  cvec_push(vec, 3.0f);
  // A raw byte copy of 3.0f's IEEE-754 bit pattern reinterpreted as int
  // would be 1077936128, not 3. If this ever regresses, that's exactly the
  // wrong value that would come back.
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
  // The value goes through a variable, and not as a literal. The Clang warning
  // -Wliteral-conversion shows a narrowing conversion of a literal that is a
  // compile-time constant. It does not show it for a general expression. If
  // the test gives -9.75f directly to cvec_push, the literal initializes the
  // typed temporary of the macro, and the warning occurs. This test does the
  // truncation below on purpose. Therefore, the warning is not correct here. A
  // variable has the same value at run time, and the warning has no constant
  // literal to find.
  float negative_value = -9.75f;
  cvec_push(vec, negative_value);
  int expected =
      (int)negative_value;  // plain C conversion: truncates toward 0 (-9)
  REQUIRE_EQ(cvec_at(vec, 0), expected);
  cvec_destroy(vec);
}

// An int literal with no suffix can go into a vector of a wider integer type.
// Any smaller integer type can too. Such a push must genuinely convert to
// that wider type. The macro must not reject it at compile time on its size.
// A _Static_assert over sizeof() refuses to compile such a push at all, even
// though the conversion is an ordinary and completely safe one in C.
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

// The conversion must happen before anything touches the vector. An lvalue
// that aliases the vector, such as cvec_at itself, is then read one time and
// correctly. Any growth of the capacity that the push starts cannot affect
// it. This test drives the same self-alias-during-growth case as
// push_back_self_alias_via_type_safe_macro above. It drives it through the
// code path that carries the conversion.
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

// cvector_begin_iter(NULL, ...) must behave the same as an empty vector. It
// must return NULL and must not touch err. It must not assert.
// chashmap_begin_iter() and cbmap_begin_iter() are tolerant of NULL in the
// same deliberate way, and all three container modules stay consistent here.
// A container field can therefore stay uninitialized while nothing is in it
// yet. A caller can iterate such a field directly, with no NULL guard of its
// own.
TEST(cvectors, begin_iter_null_vec_returns_null_like_empty) {
  cvec null_vec = NULL;
  char *err = (char *)0x1; /* poison value: must be reset to NULL, not left */
  REQUIRE_EQ((void *)cvector_begin_iter(null_vec, &err), NULL);
  REQUIRE_EQ((void *)err, NULL);

  // NULL for err itself must also be tolerated (it's documented as optional).
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
  cvec_sort(vec);  // must not crash on empty input
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

// A budget-style counting allocator: the first g_sort_oom_budget calls to
// malloc/calloc/realloc succeed (delegating to the real allocator); every
// call after the budget is exhausted returns NULL. free() always delegates
// to the real free() unconditionally, so whatever DID succeed is still
// released correctly during cvector_destroy.
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

// The internal allocation of the temp buffer of the sort can fail. cvec_sort
// and cvector_sort_with_comparison_proc must then call ccol_fatal_err(). They
// must not silently leave the vector unsorted, which tells the caller
// nothing. Every other type-inferred macro in cvector.h that changes a vector
// behaves in the same way. This test runs in a forked child, because
// ccol_fatal_err() stops the whole process.
// type_safe_at_out_of_bounds_is_fatal has the same shape.
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
    // The budget covers exactly the construction of the vector. That is 1
    // calloc for the struct, 1 malloc for the copy of the procs, and 1 malloc
    // for the first data buffer. It also covers three push_backs, which stay
    // inside the first capacity of 4 and therefore need no allocation. The
    // malloc for the temp buffer of the sort is then the 4th call to the
    // allocator, and it must fail.
    g_sort_oom_budget = 3;
    cvec_declare(vec, int);
    cvec_init_mp(vec, &procs);
    cvec_push(vec, 3);
    cvec_push(vec, 1);
    cvec_push(vec, 2);
    cvec_sort(vec); /* must ccol_fatal_err(), not silently return unsorted */
    _exit(0);       /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// cvec_sort on a vector of `signed char` must use a genuine default signed
// comparator. It must not silently fall back to a NULL one. The _Generic
// dispatch of csort_get_default_comparison_proc must therefore carry a case
// for the separate `signed char` type, and not only one for a plain `char`.
// ccol_is_integral_type() in common.h classifies `signed char` as integral.
// This project also documents it elsewhere as a first-class signed type. This
// test uses negative values on purpose. They show that the comparator is a
// real signed comparison. They rule out an unsigned order or a raw-byte order
// by accident.
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

// Some element types have no default comparison procedure. A bool, a struct,
// a union, a pointer to anything other than a char, and a char array of a
// fixed size are such types. The _Static_assert of cvec_sort rejects them, so
// such a call never builds and never reaches a running program. A test binary
// has to compile, so it cannot drive a diagnostic that the compiler issues.
// compile_probes.sh pins that rejection instead. The `test` target of this
// directory runs it beside this binary. It asserts, under every compiler that
// is available, that each of those element types fails to compile with a
// message naming cvector_sort_with_comparison_proc. It also asserts that
// every supported element type still compiles.
//
// An enumerated type IS supported and must keep sorting. C makes every
// enumerated type compatible with one of the standard integer types.
// _Generic therefore selects the comparator of that type. This test covers
// two enumerations. The compiler can represent the first as unsigned. It
// cannot represent the second that way, because that one has a negative
// enumerator. The two resolve to different comparators.
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

// Every element type that cvec_sort accepts must really sort. It is not
// enough that it compiles. The sweep below is macro-driven. It covers each
// integer width, each signedness, and each floating type in one place. A
// comparator that the _Generic dispatch of csort loses for one width
// therefore shows up as a wrong order. It does not show up as a test that is
// silently absent.
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

// Every char pointer spelling csort's default dispatch accepts sorts by
// string content. signed char * and unsigned char * are included because the
// dispatch accepts them exactly as it accepts char *.
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

// cvec_find keeps accepting every element type on purpose. That includes the
// types that cvec_sort rejects. Equality byte by byte is a real answer for a
// type with no default comparison procedure. An order is not.
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
      cvec_push(v, i);
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
    cvec_push(vec, i);
  }

  int count = 0;
  {
    ccol_iter_declare(vec, it);
    for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) {
      ++count;
      if (count == 2) {
        break;  // exit without explicit ccol_iter_destroy - RAII must clean up
      }
    }
    // 'it' is still live here; the RAII destructor on 'it' fires at '}'
  }
  REQUIRE_EQ(count, 2);
  REQUIRE_EQ(cvector_elem_count(vec), 5);  // vector is unchanged

  cvec_destroy(vec);
}

// ========================================================================
// INVARIANT TESTS (randomized operation sequences)
// ========================================================================

static bool cvector_is_power_of_two(size_t x) {
  return x != 0 && (x & (x - 1)) == 0;
}

// This test runs a random sequence of push_back, pop_back, reserve and reset
// operations. It checks the structural invariants after every single one.
// elem_count never goes above the capacity. The capacity never drops below
// _ccol_cvector_minimum_capacity, and it is always a power of two. data_ptr is
// never NULL while the capacity is above 0.
// A fixed seed is used (see common_invariants.h) so a failure is always
// reproducible from the printed seed alone.
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

/* cvec_find compares with the default comparison proc of the element type.
 * Equality is therefore the equality of the type, and not the equality of its
 * object representation. That difference is load-bearing for any type whose
 * representation carries bytes that the value does not fix. The 80-bit long
 * double of x86-64 is such a type. It uses 10 of its 16 bytes, and nothing
 * initialises the other 6. A comparison byte by byte of two objects that hold
 * the identical value therefore reports them as different. It also reads
 * indeterminate bytes. These tests are not vacuous. A comparison of raw bytes
 * here makes the first test report every element that is present as missing at
 * -O0. It also makes valgrind report an uninitialised read at every
 * optimisation level. */
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

  /* This is the case that separates the two rules. The values -0.0L and 0.0L
   * are equal. They differ in the byte that holds their sign bit. Only a
   * comparison of values finds this one. The four lookups above agree with a
   * comparison of bytes whenever the padding bytes happen to match. They are
   * therefore a check of correctness, and not the thing that pins the
   * contract. */
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
  /* The default comparison proc orders by value, which is the same notion of
   * equality chashmap and cbstmap use for a double key. */
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
   * declared element type, so an aggregate element type is a plain copy. A
   * braced initialiser here would instead initialise the first member only,
   * which is a hard compile error for a same-type argument. */
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

/* For cvec_push and for cvec_find, a named array is a value of an array
 * element type. The macro fills the value with zeros to the element size.
 * Therefore, a shorter array or string literal finds the element that went
 * in with the same text. The macro cuts a longer array to the element size.
 * This test is non-vacuous: if the macro stores the argument by
 * initialization (`T tmp = (elem);`), the code does not compile for a named
 * array. */
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
  /* Both are cut to the eight bytes "gamma_is", which match. */
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
  /* Every temporary that a public macro declares carries a name that no
   * realistic identifier of a caller collides with. A macro can expand an
   * expression from the caller into the scope of its own temporary. A shared
   * name then makes the argument of the caller resolve to that temporary. The
   * reason is that an identifier is in scope from the end of its own
   * declarator. None of the cvector macros here expands an expression of the
   * caller into such a scope. This test is therefore a standing guard against
   * that changing. It does not separate a current defect. The form of this
   * hazard that a caller can reach is pinned in tests/chashmap, where the key
   * argument does reach the scope of a temporary. */
  /* The two variables below come BEFORE the construct. The temporary of the
   * construct and init macro is therefore the one that would capture
   * them. */
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

/* This function returns -1 through an exit from the scope. That exit happens
 * before anything assigns the iterator variable. ccol_iter_declare attaches a
 * cleanup handler that runs on every exit from the enclosing block, this one
 * included. The variable that it declares must therefore start as NULL. The
 * handler dereferences that variable and calls through a function pointer
 * inside it. For an indeterminate value that is an arbitrary indirect call,
 * and not a dereference of NULL. A missing initialiser is caught at compile
 * time, every time. A read of the variable below is then a
 * -Wmaybe-uninitialized error under the -Werror of this project. The
 * assertion at run time records the value that the cleanup handler would have
 * seen. An uninitialised stack slot makes that value non-NULL only some of
 * the time. */
static void *helper_iter_value_before_assignment;

static int helper_iter_early_return(cvec vec, bool take_early_exit) {
  cvec_redeclare(vec, int);
  ccol_iter_declare(vec, it);
  int seen = 0;
  if (take_early_exit) {
    /* The record of the value is the assertion that matters. The cleanup
     * handler that runs on this return dereferences this variable. It then
     * calls through a function pointer inside it. The declaration must
     * therefore leave the variable NULL. Whether an uninitialised slot holds
     * a trapping value is a property of the stack. It is not part of the
     * contract. */
    helper_iter_value_before_assignment = (void *)it;
    return -1;
  }
  for (it = ccol_begin(vec); it != NULL; it = ccol_iter_next(it)) seen++;
  return seen;
}

/* Dirties the stack region the iterator variable will occupy, so that an
 * uninitialised declaration holds a non-NULL value rather than an incidental
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

// A char * vector with NULL holes is an ordinary shape. An argv-style list is
// one, and so is a sparse table of optional strings. Both cvec_find() and
// cvec_sort() reach _csort_default_string_comparison_proc with those NULL
// elements. That comparator orders NULL before every string that is not NULL.
// It makes NULL equal only to another NULL. Both macros therefore stay
// defined for such a vector. These tests are not vacuous. Without that
// ordering rule, the comparator hands a null pointer to strcmp(). The whole
// binary then dies with SIGSEGV, and one assertion does not merely fail.
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

// A caller searches for NULL to ask for the first free slot of such a vector.
// cvec_find must therefore answer a NULL needle. It must not dereference
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

// Here a vector with no NULL element at all gets a NULL needle. Every
// comparison then has a NULL on one side only. None of them may go to
// strcmp().
TEST(null_strings, find_a_null_needle_in_a_vector_without_null_elements) {
  cvec_construct(vec, char *);
  cvec_push(vec, "alpha");
  cvec_push(vec, "beta");

  REQUIRE_EQ(cvec_find(vec, (char *)NULL), ccol_invalid_size);

  cvec_destroy(vec);
}

// Sorting the same shape: every NULL lands at the front and the non-NULL
// elements stay in strcmp() order behind them.
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

// A source range that aliases the vector may read past the live element count
// of v, into capacity that is reserved but not yet live. It may never read
// past the reservation itself. The bytes after that belong to no allocation
// of the vector, so a read of them is an over-read of the heap. The call must
// reject such a request with false. It must leave the vector completely
// untouched. This test is not vacuous. Without the bound, the call reports
// true, and AddressSanitizer reports a heap-buffer-overflow READ inside the
// copy.
TEST(cvectors, append_array_rejects_a_source_range_past_the_reservation) {
  cvector *cvec = cvector_create(sizeof(int), NULL);
  REQUIRE_TRUE(cvector_reserve(cvec, (size_t)16));
  REQUIRE_EQ(cvector_get_capacity(cvec), (size_t)16);
  cvector_push_back(cvec, &(int){7});

  int *base = (int *)cvector_data_ptr(cvec);
  // Source starts at the last reserved slot, so only one element is
  // readable from there while 15 are requested.
  REQUIRE_FALSE(cvector_append_array(cvec, base + 15, (size_t)15));
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 7);

  cvector_destroy(cvec);
}

// This test sweeps the boundary of the bound itself, from inside it to one
// element past it. A source range that ends exactly at the end of the
// reservation is accepted. The same range with one more element is rejected.
// An assertion on the accepted side alone leaves the rejection untested. An
// assertion on the rejected side alone does not show that the bound is off by
// nothing.
TEST(cvectors, append_array_accepts_a_source_range_ending_at_the_reservation) {
  cvector *cvec = cvector_create(sizeof(int), NULL);
  REQUIRE_TRUE(cvector_reserve(cvec, (size_t)16));
  REQUIRE_EQ(cvector_get_capacity(cvec), (size_t)16);

  int *base = (int *)cvector_data_ptr(cvec);
  for (int i = 0; i < 16; ++i) {
    base[i] = i;
  }

  // One live element; source is [12, 16), which ends exactly at the end of
  // the reserved region.
  cvector_push_back(cvec, &(int){100});
  REQUIRE_TRUE(cvector_append_array(cvec, base + 12, (size_t)4));
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)5);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 12);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 4), 15);

  // The same starting offset with one more element runs one slot past the
  // (now grown) reservation's end and is refused.
  size_t capacity = cvector_get_capacity(cvec);
  base = (int *)cvector_data_ptr(cvec);
  REQUIRE_FALSE(cvector_append_array(cvec, base + (capacity - 4), (size_t)5));
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)5);

  cvector_destroy(cvec);
}

/* A vector whose element type is a char pointer that is itself const. The
 * default comparator of such a vector is the string comparator, so cvec_find
 * matches on the content of the string and not on the address that the
 * pointer holds, and cvec_sort orders by that content. */
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
 * frees it. A read of freed memory then gives the poison and not the stale
 * value, so a use after free through a stale pointer faults in an ordinary
 * run and does not depend on a sanitizer to show. */
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
 * scope of ccol_iter_declare. Destroying the vector inside that scope must
 * leave the iterator freeable, because the scope-exit cleanup frees it after
 * the vector and its allocator record are gone. This test is non-vacuous:
 * an iterator that frees itself through the vector reads the poisoned vector
 * struct at scope exit and faults. */
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

/* Sorts one copy through cvec_sort, and a second copy through the getter
 * path of csort_sort, and requires the same elements in the same order. The
 * getter path is the reference: it is the general algorithm, with the same
 * pass structure. eq compares two elements; for a long double it compares
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
  /* 12 bytes: the width that no constant specialization covers. The key
   * range is small, so ties are common and stability is visible. */
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
 * call that this file makes to cvector_elem_count, including the calls that
 * a macro expands to, comes here first. A linker with no --wrap (Apple's)
 * cannot route the calls to a function that the same binary defines, so
 * there the test below is skipped; what it checks is a property of the macro
 * expansion, the same on every system. */
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

/* A brace initializer takes a struct rvalue as the initializer of the first
 * member, and that does not compile. This test therefore fails to build
 * against a macro that pushes through one. */
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
 * member makes the struct too wide for a return in registers, so a function
 * that returns it writes straight into the object of its caller, padding
 * included. */
typedef struct {
  char tag;
  long id;
  long spare;
} cvec_padded_elem;

/* A value built by member assignment over storage that holds a non-zero byte
 * pattern. Two values with the same members then differ in their padding. */
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
/* Fills the stack below the caller with a non-zero pattern, so that a
 * temporary whose padding nothing writes shows it as non-zero bytes. */
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
 * clear the padding of their own copy of the value. The stored elements
 * then have zero padding. The byte-wise fallback of cvec_find then matches
 * a needle whose members are equal, whatever its padding bytes hold. This
 * test is non-vacuous under GCC: without the clear operation, the stored
 * elements have the pattern of the value in their padding, and each lookup
 * fails. */
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

/* The temporary of a value macro drops the top-level qualifiers of the
 * element type and keeps an array element type whole. Both shapes therefore
 * still push and find, with the padding clear in place. */
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

/* cvector_append_array() takes a pointer to const. An append from a const
 * array therefore compiles under -Werror, through the function and through
 * the macro. This test does not build when the parameter drops the const. */
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

/* cvector_sort_with_comparison_proc takes a plain vector expression, not a
 * name that it pastes, so the expression can carry a side effect. The macro
 * evaluates it once: the vector that it checks is the vector that it sorts,
 * and the side effect happens once. */
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

/* cvector_destroy evaluates its lvalue once. The element that it frees is
 * the element that it sets to NULL, and no neighbour is read or written. */
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
    /* A macro that evaluates its argument more than once frees vs[1] in
     * place of vs[0]. Free the survivor, so that the failure below is a
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

/* A function-pointer element type and an array element type cannot be spelled
 * as "type *name", so the companion variable of cvec_declare(),
 * cvec_declare_scoped() and cvec_redeclare() is spelled through __typeof__.
 * This test is non-vacuous: with the plain spelling it does not compile. */
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

/* A vector of char * stores the pointer itself, so a NULL element, such as
 * what getenv() gives for a variable that is not set, is an ordinary element
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
 * type. Its temporary is written through cvector_pop_back(), so it has the
 * element type without its qualifiers. This test is non-vacuous: a temporary
 * declared with the qualified element type makes this file fail to compile
 * with -Werror=discarded-qualifiers. */
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
