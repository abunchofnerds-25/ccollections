#include <common_invariants.h>
#include <cvector.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <unistd.h>
TAU_MAIN()  // sets up Tau (+ main function)

extern size_t cvector_get_capacity(cvector *v);
extern const size_t minimum_capacity;
extern const size_t scaling_factor;
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

// An elem_size large enough that the initial 4-element backing buffer
// allocation (minimum_capacity * elem_size) would overflow size_t must be
// rejected upfront, rather than silently wrapping to an undersized
// allocation while v->elem_size still records the real, huge value.
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
  // The vector stores its own heap-allocated copy of the procs struct, not
  // the caller's original pointer, but the function pointers inside it must
  // match what was supplied at creation.
  REQUIRE_NE((void *)stored, (void *)&custom);
  REQUIRE_TRUE(stored->malloc == custom.malloc);
  REQUIRE_TRUE(stored->free == custom.free);
  REQUIRE_TRUE(stored->calloc == custom.calloc);
  REQUIRE_TRUE(stored->realloc == custom.realloc);
  cvector_destroy(custom_cvec);
}

// cvector_get_mprocs(NULL) must fatal_err() (assert), matching every other
// accessor in this module (cvector_at, cvector_elem_count, cvector_reset,
// cvector_data_ptr, cvector_find, cvector_push_back, cvector_pop_back,
// cvector_reserve), rather than dereferencing a NULL vector directly. Run in
// a forked child (mirroring type_safe_at_out_of_bounds_is_fatal) since the
// assertion aborts the whole process.
TEST(cvectors, get_mprocs_null_vec_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    (void)cvector_get_mprocs(NULL); /* NULL vector; must fatal_err() */
    _exit(0);                       /* unreachable if fatal_err() aborted */
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
  // fits without growing, so this exercises the already-safe no-realloc path.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){10});
  cvector_push_back(cvec, &(int){20});
  cvector_push_back(cvec, &(int){30});

  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);
  int *elem_ptr = cvector_at(cvec, 1);  // points at element {20}
  ccol_retval_t result = cvector_push_back(cvec, elem_ptr);
  REQUIRE_EQ(result, ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 4);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);  // no realloc

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 10);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 20);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 30);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 20);

  cvector_destroy(cvec);
}

TEST(cvectors, push_back_self_alias_triggers_expansion) {
  // 4 elements, capacity=4 (exactly full): pushing an existing element back
  // onto itself forces scale_the_cvector_size_up to realloc. Before the fix,
  // new_elem (a pointer into the vector's own buffer) would dangle once the
  // realloc moved the buffer, and the subsequent mem_cpy would read freed
  // memory.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  for (int i = 0; i < 4; ++i) {
    cvector_push_back(cvec, &(int){(i + 1) * 10});
  }

  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);
  int *elem_ptr = cvector_at(cvec, 3);  // points at element {40}
  ccol_retval_t result = cvector_push_back(cvec, elem_ptr);
  REQUIRE_EQ(result, ccol_success);
  REQUIRE_EQ(cvector_elem_count(cvec), 5);
  REQUIRE_NE(cvector_get_capacity(cvec), minimum_capacity);  // grew

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
    cvec_push_rvalue(vec, (i + 1) * 100);
  }
  REQUIRE_EQ(cvector_get_capacity(vec), minimum_capacity);

  cvec_push(vec, cvec_at(vec, 0));  // aliases the vector's own storage
  REQUIRE_EQ(cvec_size(vec), (size_t)5);
  REQUIRE_NE(cvector_get_capacity(vec), minimum_capacity);  // grew
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

// arr_ptr aliasing into v's own backing buffer must remain safe even when
// cvector_reserve reallocates the buffer to a new address mid-call.
TEST(cvectors, append_array_self_alias_no_realloc) {
  // 2 elements, capacity=4: appending the whole buffer back onto itself
  // fits without growing, so this exercises the already-safe no-realloc path.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){1});
  cvector_push_back(cvec, &(int){2});

  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);
  void *arr_ptr = cvector_data_ptr(cvec);
  size_t overlap_count_before = cvector_overlap_copy_count_for_tests;
  int result = (int)cvector_append_array(cvec, arr_ptr, 2);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 4);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);  // no realloc
  // Source range ends exactly where the destination begins (touching, not
  // overlapping): the cheaper mem_cpy path must still be used, not memmove.
  REQUIRE_EQ(cvector_overlap_copy_count_for_tests, overlap_count_before);

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 2);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 2);

  cvector_destroy(cvec);
}

TEST(cvectors, append_array_self_alias_triggers_expansion) {
  // 3 elements, capacity=4: appending the whole buffer back onto itself
  // needs 6 slots, forcing cvector_reserve to realloc. Before the fix,
  // arr_ptr (== cvector_data_ptr(cvec)) would dangle once the realloc moved
  // the buffer, and the second mem_cpy would read freed memory.
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){10});
  cvector_push_back(cvec, &(int){20});
  cvector_push_back(cvec, &(int){30});

  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);
  void *arr_ptr = cvector_data_ptr(cvec);
  int result = (int)cvector_append_array(cvec, arr_ptr, 3);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 6);
  REQUIRE_NE(cvector_get_capacity(cvec), minimum_capacity);  // grew

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

  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);
  int *arr_ptr =
      (int *)cvector_data_ptr(cvec) + 2;  // points at elements {2, 3}
  int result = (int)cvector_append_array(cvec, arr_ptr, 2);
  REQUIRE_EQ(result, true);
  REQUIRE_EQ(cvector_elem_count(cvec), 6);
  REQUIRE_NE(cvector_get_capacity(cvec), minimum_capacity);  // grew

  REQUIRE_EQ(*(int *)cvector_at(cvec, 0), 0);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 1), 1);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 2), 2);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 3), 3);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 4), 2);
  REQUIRE_EQ(*(int *)cvector_at(cvec, 5), 3);

  cvector_destroy(cvec);
}

// arr_ptr aliasing v's own buffer at an offset such that the requested
// elem_count reads past the vector's *live* elem_count into
// reserved-but-not-yet-live capacity can make the source and destination
// byte ranges genuinely overlap (unlike every self-alias test above, whose
// source range always ends exactly where the destination begins). A plain
// memcpy (or a naive forward byte-copy) is not safe for that: writing the
// first appended element can clobber source bytes that are still needed for
// a later appended element. cvector_copy_into_tail must detect this and use
// an overlap-safe memmove.
//
// The two tests below deliberately use a large element count with only a
// ONE-ELEMENT shift between the source and destination ranges (so nearly
// the entire copy overlaps itself). This is not incidental: a smaller
// shift/size combination was tried first and, despite exercising the exact
// same code path, passed even with the overlap-safety check disabled;
// this glibc's memcpy happens not to visibly corrupt data for a handful of
// small structs at that particular size/shift, since overlap is undefined
// behavior, not guaranteed-wrong behavior. A huge total size with a
// one-element shift is the classic "shift an array by one in place via
// memcpy instead of memmove" pattern: any implementation that copies in
// pieces smaller than the whole buffer (which every real memcpy does, by
// design, for anything beyond a handful of bytes) is architecturally
// incapable of getting this right without memmove's overlap handling, so
// this reproduces reliably regardless of the specific memcpy/compiler used.
TEST(cvectors, append_array_self_alias_overlap_shift_by_one_no_realloc) {
  const int n = 10000;
  cvector *cvec = cvector_create(sizeof(int), NULL);
  cvector_push_back(cvec, &(int){0});
  cvector_push_back(cvec, &(int){1});
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)2);

  // Grow capacity without touching elem_count, then hand-populate the spare
  // (not-yet-live, but now allocated) region directly through the raw data
  // pointer with deterministic values, bypassing push_back entirely so no
  // shrink logic is ever triggered.
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
  // The whole point of this test: confirm the overlap-safe memmove branch
  // was actually taken, rather than trusting the copy engine's output alone;
  // a real memcpy implementation is not contractually required to
  // corrupt an overlapping copy, only permitted to.
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
  // Source range [1, cap) and destination range [2, cap+1) overlap almost
  // entirely, shifted by one element; this must survive both the realloc
  // (arr_ptr recomputed against the new buffer) and the overlap correctly.
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
// already at minimum_capacity (a true no-op shrink), mirroring
// scale_the_cvector_size_down's own early-return guard for the identical
// case; it must still shrink (and therefore realloc) when genuinely needed.
TEST(cvectors, reset_skips_realloc_when_already_at_minimum_capacity) {
  ccol_memmgmt_procs_t procs = {.malloc = _reset_test_malloc,
                                .calloc = _reset_test_calloc,
                                .realloc = _reset_test_realloc,
                                .free = _reset_test_free};
  g_reset_test_realloc_count = 0;

  cvector *cvec = cvector_create_full(sizeof(int), &procs, NULL);
  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);

  cvector_reset(cvec);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);
  REQUIRE_EQ(g_reset_test_realloc_count, (size_t)0);

  // Grow past minimum, then reset: this time a shrinking realloc is expected.
  for (int i = 0; i < 20; ++i) {
    cvector_push_back(cvec, &i);
  }
  REQUIRE_NE(cvector_get_capacity(cvec), minimum_capacity);

  size_t realloc_count_before_shrink = g_reset_test_realloc_count;
  cvector_reset(cvec);
  REQUIRE_EQ(cvector_elem_count(cvec), 0);
  REQUIRE_EQ(cvector_get_capacity(cvec), minimum_capacity);
  REQUIRE_GT(g_reset_test_realloc_count, realloc_count_before_shrink);

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

TEST(cvectors, type_safe_at_ptr) {
  cvec_construct(vec, int);

  for (int i = 0; i < 5; ++i) {
    cvec_push_rvalue(vec, i * 10);
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

// cvec_at terminates the process via fatal_err() on an out-of-bounds index
// (matching every other type-safe macro's "aborting convenience API"
// contract), rather than the raw NULL-dereference SIGSEGV a direct
// dereference of cvector_at()'s NULL return would otherwise produce. Run in
// a forked child (mirroring tests/cthreadpool/tests.c's own fork-test
// precedent for process-terminating misuse) since fatal_err aborts the
// whole process.
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
    cvec_push_rvalue(vec, 1);
    (void)cvec_at(vec, 1); /* out of bounds; must fatal_err(), not SIGSEGV */
    _exit(0);              /* unreachable if fatal_err() aborted as expected */
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
// MACRO HYGIENE TESTS
// ========================================================================
//
// cvec_push, cvec_push_rvalue, and cvec_pop each used to declare their own
// internal result-holding local as plain 'r' (and cvec_pop also declared a
// plain '_tmp'), in the SAME statement whose initializer embeds the macro's
// own parameter(s) via textual substitution. Per C's declarator-scope rule
// (identical to the classic `int x = x;` footgun: the scope of a declared
// identifier begins right after its own declarator, before its initializer
// is even evaluated), a caller whose own vector variable or pushed/popped
// expression was the bare identifier 'r' (this project's own extremely
// common convention for a retval local) had that identifier silently
// resolve to the macro's own not-yet-initialized local instead of the
// caller's real variable; reproduced as genuine, silently wrong data with
// zero compiler warnings at every optimization level. Fixed by renaming
// every such internal local to a name specific enough to this one macro
// that no real caller identifier could plausibly collide with it.

TEST(cvectors, push_value_named_r_is_not_shadowed_by_internal_retval) {
  cvec_construct(vec, int);
  int r = 42;
  cvec_push(vec, r);
  REQUIRE_EQ(cvec_size(vec), (size_t)1);
  REQUIRE_EQ(cvec_at(vec, 0), 42);
  cvec_destroy(vec);
}

TEST(cvectors, push_rvalue_expression_named_r_is_not_shadowed) {
  cvec_construct(vec, int);
  int r = 77;
  cvec_push_rvalue(vec, r);
  REQUIRE_EQ(cvec_size(vec), (size_t)1);
  REQUIRE_EQ(cvec_at(vec, 0), 77);
  cvec_destroy(vec);
}

TEST(cvectors, pop_from_vector_variable_named_r_is_not_shadowed) {
  cvec_construct(r, int);
  cvec_push_rvalue(r, 5);
  cvec_push_rvalue(r, 9);
  int popped = cvec_pop(r);
  REQUIRE_EQ(popped, 9);
  REQUIRE_EQ(cvec_size(r), (size_t)1);
  REQUIRE_EQ(cvec_at(r, 0), 5);
  cvec_destroy(r);
}

TEST(cvectors, pop_from_vector_variable_named__tmp_is_not_shadowed) {
  // cvec_pop's own internal element-holding local used to be plain '_tmp';
  // a vector variable itself named '_tmp' exercises the cross-statement
  // shadowing variant of the same hazard (the first declaration in the
  // macro's statement-expression body would have redeclared '_tmp' as the
  // vector's element type, shadowing the caller's own cvec for the rest of
  // that statement-expression's scope).
  cvec_construct(_tmp, int);
  cvec_push_rvalue(_tmp, 3);
  cvec_push_rvalue(_tmp, 6);
  int popped = cvec_pop(_tmp);
  REQUIRE_EQ(popped, 6);
  REQUIRE_EQ(cvec_size(_tmp), (size_t)1);
  REQUIRE_EQ(cvec_at(_tmp, 0), 3);
  cvec_destroy(_tmp);
}

// cvec_at used to expand its index argument twice (once for cvector_at()'s
// own bounds-checked access, once more for _cvec_at_checked()'s
// diagnostic-only re-read), as two arguments of the very same function
// call; unsequenced relative to each other. For an ordinary variable or
// literal index this was invisible, but for a side-effecting index
// expression (e.g. cvec_at(vec, i++)) it was genuine undefined behavior:
// in practice the index variable advanced by 2 per call instead of 1,
// silently skipping every other element and eventually walking off the end
// of the vector. Fixed by capturing index into a local exactly once before
// either use. These tests read/write through a loop with a bare j++ index
// expression and assert the loop variable advances by exactly 1 per
// iteration and every element is visited exactly once, in order.
TEST(cvectors, at_index_expression_with_side_effect_evaluated_once) {
  cvec_construct(vec, int);
  for (int i = 0; i < 5; ++i) {
    cvec_push_rvalue(vec, i * 10);
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
    cvec_push_rvalue(vec, 0);
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
// cvec_reserve and cvec_append_array used to embed their own
// new_capacity_count / elem_count argument into two places in their
// expansion (the real cvector_reserve()/cvector_append_array() call, and
// the fatal_err() diagnostic built only on the failure path) with no
// single-evaluation guard, unlike cvec_at's index (see the
// "at_index_expression_with_side_effect_evaluated_once*" tests above) and
// cvec_push/cvec_push_rvalue/cvec_pop's own internal locals. A
// side-effecting new_capacity_count/elem_count expression was therefore
// evaluated a second time purely to build the diagnostic message whenever
// the underlying call failed, silently duplicating any real side effect
// (and potentially reporting a different value than what was actually
// attempted) right before fatal_err() aborts the process. Fixed by
// capturing the argument into a local exactly once, mirroring cvec_at's own
// fix.
//
// Both tests below use a deterministic failure mode (exceeding
// max_elem_count / overflowing the append total) rather than a failing
// allocator, so the call count below can be attributed unambiguously to the
// macro's own expansion rather than to any allocator retry behavior. The
// call count is recorded in shared (mmap'd) memory: the process containing
// the double evaluation is the forked child, which then aborts, so the
// parent can only observe state that survives the abort via memory shared
// across the fork.

static int *g_reserve_side_effect_call_count;
static size_t reserve_side_effecting_target(void) {
  (*g_reserve_side_effect_call_count)++;
  return max_elem_count + 1;  // guarantees cvector_reserve() fails
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
    _exit(0); /* unreachable if fatal_err() aborted as expected */
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
    _exit(0); /* unreachable if fatal_err() aborted as expected */
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
// cvec_push and cvec_push_rvalue used to take the address of new_elem
// directly (cvec_push) or build a compound literal typed as new_elem's OWN
// type (cvec_push_rvalue), guarded only by a sizeof()-based _Static_assert.
// That assert caught a differently-SIZED new_elem (preventing an
// out-of-bounds read) but did nothing for a differently-TYPED new_elem of
// the *same* size: e.g. pushing a `float` into a vector declared with
// cvec_construct(v, int) passed the size check (sizeof(float) ==
// sizeof(int) on every mainstream platform) and then had its raw 4-byte
// IEEE-754 bit pattern copied verbatim into the vector's int-typed storage,
// rather than being converted to an int the way a plain C assignment
// (`int x = some_float;`) would; reading it back produced the float's
// bits reinterpreted as an int, not the expected truncated integer value,
// with zero compiler diagnostics at any optimization level. Fixed by typing
// the internal temporary as v's own declared element type and initializing
// it from new_elem (routing the conversion through the C compiler's real
// assignment rules), mirroring the pattern cvec_find already used
// correctly. These tests push a same-size-but-different-type value and
// check the actual resulting VALUE, not just that the call compiles and
// "succeeds" (the pre-fix code compiled and returned ccol_success too;
// it just silently stored the wrong value).

TEST(cvectors, push_rvalue_float_into_int_vector_converts_not_reinterprets) {
  cvec_construct(vec, int);
  cvec_push_rvalue(vec, 3.0f);
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

TEST(cvectors, push_rvalue_double_into_long_long_vector_converts) {
  cvec_construct(vec, long long);
  cvec_push_rvalue(vec, 7.0);
  REQUIRE_EQ(cvec_at(vec, 0), (long long)7);
  cvec_destroy(vec);
}

TEST(cvectors,
     push_negative_float_into_int_vector_truncates_like_plain_assignment) {
  cvec_construct(vec, int);
  // Routed through a variable rather than passed as a literal: Clang's
  // -Wliteral-conversion specifically flags a narrowing conversion applied
  // to a compile-time-constant literal (not a general expression), so
  // passing -9.75f directly to cvec_push_rvalue's internal typed compound
  // literal trips it even though the truncation below is exactly what this
  // test means to exercise, not a mistake to be warned about. A variable
  // carries the identical runtime value with no compile-time-constant
  // literal for the warning to key on.
  float negative_value = -9.75f;
  cvec_push_rvalue(vec, negative_value);
  int expected =
      (int)negative_value;  // plain C conversion: truncates toward 0 (-9)
  REQUIRE_EQ(cvec_at(vec, 0), expected);
  cvec_destroy(vec);
}

// An unsuffixed int literal (or any smaller integer type) pushed into a
// vector of a wider integer type must genuinely convert to that wider type,
// not merely be size-rejected at compile time (the old sizeof()-based
// _Static_assert would have refused to compile this entirely, even though
// it is a completely safe, ordinary C conversion).
TEST(cvectors, push_rvalue_int_literal_into_long_vector_converts) {
  cvec_construct(vec, long);
  cvec_push_rvalue(vec, 5);
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

// The conversion must happen before the vector is touched at all, so an
// aliasing lvalue (cvec_at itself) is read once, correctly, and is
// unaffected by any capacity expansion the push triggers; exercises the
// same self-alias-during-growth scenario as
// push_back_self_alias_via_type_safe_macro above, but through the
// conversion-bearing code path.
TEST(cvectors, push_self_alias_still_safe_after_conversion_fix) {
  cvec_construct(vec, int);
  for (int i = 0; i < 4; ++i) {
    cvec_push_rvalue(vec, (i + 1) * 100);
  }
  REQUIRE_EQ(cvector_get_capacity(vec), minimum_capacity);

  cvec_push(vec, cvec_at(vec, 0));  // aliases the vector's own storage
  REQUIRE_EQ(cvec_size(vec), (size_t)5);
  REQUIRE_NE(cvector_get_capacity(vec), minimum_capacity);  // grew
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

// cvector_begin_iter(NULL, ...) must be treated the same as an empty vector:
// return NULL without touching err, rather than asserting. This mirrors
// chashmap_begin_iter()/cbmap_begin_iter()'s own identical, deliberate
// NULL-tolerance (kept consistent across all three container modules), so a
// lazily-created container field left uninitialized because nothing has
// been inserted into it yet can be iterated directly without every caller
// needing its own NULL guard first.
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

// cvec_sort/cvector_sort_with_comparison_proc must call fatal_err() (not
// silently leave the vector unsorted with no indication anything went
// wrong) when the sort's own internal temp-buffer allocation fails, matching
// every other mutating type-safe macro in cvector.h. Run in a forked child
// (mirroring type_safe_at_out_of_bounds_is_fatal) since fatal_err() aborts
// the whole process.
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
    // Budget covers exactly vector construction (1 calloc for the struct + 1
    // malloc for the procs copy + 1 malloc for the initial data buffer) plus
    // three push_backs that stay within the initial capacity of 4 (no
    // further allocation needed); the sort's own temp-buffer malloc is then
    // the 4th allocation call and must fail.
    g_sort_oom_budget = 3;
    cvec_declare(vec, int);
    cvec_init_mp(vec, &procs);
    cvec_push_rvalue(vec, 3);
    cvec_push_rvalue(vec, 1);
    cvec_push_rvalue(vec, 2);
    cvec_sort(vec); /* must fatal_err(), not silently return unsorted */
    _exit(0);       /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// cvec_sort on a vector of `signed char` must use a genuine default signed
// comparator, not silently fall back to a NULL one (csort_get_default_
// comparison_proc's own _Generic dispatch previously had a case for plain
// `char` but not the distinct `signed char` type, even though `signed char`
// is classified as integral by common.h's is_integral_type() and is
// documented elsewhere in this project as a first-class signed type). This
// includes negative values specifically, to prove the comparator is a real
// signed comparison rather than, say, an accidental unsigned or raw-byte
// ordering.
TEST(cvectors, sort_signed_char_uses_genuine_signed_default_comparator) {
  cvec_construct(vec, signed char);
  cvec_push_rvalue(vec, (signed char)3);
  cvec_push_rvalue(vec, (signed char)-5);
  cvec_push_rvalue(vec, (signed char)100);
  cvec_push_rvalue(vec, (signed char)-100);
  cvec_push_rvalue(vec, (signed char)0);

  cvec_sort(vec);

  const int expected[] = {-100, -5, 0, 3, 100};
  for (int i = 0; i < 5; ++i) {
    REQUIRE_EQ((int)cvec_at(vec, i), expected[i]);
  }

  cvec_destroy(vec);
}

// A vector element type with no default comparator at all (bool was never
// classified as a numeric type by common.h's is_integral_type(), unlike
// every char/int/float family type) must still fail via a clear,
// cvector.h-originated fatal_err() naming the actual problem, rather than
// silently propagating a NULL comparator into csort_sort and aborting on an
// opaque internal assertion deep inside csort.c with no indication of what
// went wrong. Run in a forked child since fatal_err() aborts the whole
// process.
TEST(cvectors, sort_unsupported_type_is_fatal_with_clear_diagnostic) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    cvec_construct(vec, bool);
    bool a = true, b = false;
    cvec_push(vec, a);
    cvec_push(vec, b);
    cvec_sort(vec); /* must fatal_err() with a clear message, not an assert */
    _exit(0);       /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
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

// Runs a random sequence of push_back/pop_back/reserve/reset operations and
// checks structural invariants after every single one: elem_count never
// exceeds capacity, capacity never drops below minimum_capacity and is
// always a power of two, and data_ptr is never NULL while capacity > 0.
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
    REQUIRE_TRUE(capacity >= minimum_capacity);
    REQUIRE_TRUE(cvector_is_power_of_two(capacity));
    if (capacity > 0) {
      REQUIRE_NE(cvector_data_ptr(vec), NULL);
    }
  }

  cvector_destroy(vec);
}
