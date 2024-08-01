#include <csort.h>
#include <cvector.h>
#include <stdio.h>
#include <tau/tau.h>
#include <time.h>

TAU_MAIN()  // sets up Tau (+ main function)

#define define_integer_type_test(name, type, modulus_value)     \
  TEST(csort, cvector_##name##_sort) {                          \
    const int num_sample = 10;                                  \
    const unsigned int seed = time(NULL);                       \
                                                                \
    char *err_str = NULL;                                       \
    int i;                                                      \
                                                                \
    srand(seed);                                                \
    cvec_declare(cvec, type);                                   \
    cvec_init(cvec);                                            \
                                                                \
    REQUIRE_NE((void *)cvec, NULL);                             \
    REQUIRE_EQ((void *)err_str, NULL);                          \
                                                                \
    for (i = 0; i < num_sample; i++) {                          \
      cvector_push_back(cvec, &(type){rand() % modulus_value}); \
    }                                                           \
                                                                \
    REQUIRE_EQ(cvector_elem_count(cvec), num_sample);           \
                                                                \
    cvec_sort(cvec);                                            \
                                                                \
    for (i = 1; i < num_sample; i++) {                          \
      REQUIRE_LE(*(type *)cvector_at(cvec, i - 1),              \
                 *(type *)cvector_at(cvec, i));                 \
    }                                                           \
                                                                \
    cvector_destroy(cvec);                                      \
    REQUIRE_EQ((void *)cvec, NULL);                             \
  }

// The test for the int type has been written intentionally explicitly for
// inspection purposes. The rest of the integer type test will be defined with
// the macro above.
TEST(csort, cvector_integer_sort) {
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, int);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(int){rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++) {
    REQUIRE_LE(*(int *)cvector_at(cvec, i - 1), *(int *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

define_integer_type_test(char, char, 100)
    define_integer_type_test(short, short, 1000)
    // define_integer_type_test(int, int, 1000) // Expilictly defined above
    define_integer_type_test(long, long, 1000)
        define_integer_type_test(long_long, long long, 1000)
            define_integer_type_test(unsigned_char, unsigned char, 100)
                define_integer_type_test(unsigned_short, unsigned short, 1000)
                    define_integer_type_test(unsigned_int, unsigned int, 1000)
                        define_integer_type_test(unsigned_long, unsigned long,
                                                 1000)
    // define_integer_type_test(unsigned_long_long, unsigned long long, 1000) //
    // TAU REQUIRE_LE not supports unsigned long long

    TEST(csort, cvector_double_sort) {
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, double);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(double){((double)rand() / RAND_MAX * 10)});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++) {
    REQUIRE_LE(*(double *)cvector_at(cvec, i - 1),
               *(double *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, cvector_float_sort) {
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, float);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(float){((float)rand() / (float)RAND_MAX * 10)});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++) {
    REQUIRE_LE(*(float *)cvector_at(cvec, i - 1),
               *(float *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, cvector_long_double_sort) {
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, long double);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec,
                      &(long double){((long double)rand() / RAND_MAX * 10)});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++) {
    REQUIRE_LE(*(long double *)cvector_at(cvec, i - 1),
               *(long double *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, cvector_string_sort_with_known_values) {
  const int num_sample = 10;
  char test_strings[10][100] = {"hVqL8eY9hAfJ2ZyGvKpT7u5wN1bXk06RzDlI",
                                "wz2A4TgB1fV0o9c8Qe5jZyH7NkWv6pUdxXlJl0L",
                                "D3kRzVbYF4wJ5qW7uN0sTp1L3H2m8oXG6CZa5Bp",
                                "kQb9ZfL6Dp0sW8XnV3mJ1rYw7E5qH2P9LgFZ5sT0",
                                "Af56V7dL9Rg0pXJz3WmYq1hTkB2c8Z9yN0sD4FvK1",
                                "N2Q4Fz1Jk8Wv0RmH5gXpYdT3L7V9nJwU6qBc9y2V",
                                "p8Lz7yF5w2m9rG0YqD6XnQW1b5Vj4kT3oH2KsZy7A",
                                "jL0V1BzG8oK7q6mF9X4H3tW2N5rYwP9Vb0uY2L8N",
                                "k3G8pV0YwT9L7mJ1qZ5v0W2bN1cH9Xg3R6K4zQ2Y",
                                "p6A3t9V1fWqL0rH8Z7X2j9yF5b0G5cL2m3WqY0k9Z"};

  char sorted_compare_values[10][100] = {
      "Af56V7dL9Rg0pXJz3WmYq1hTkB2c8Z9yN0sD4FvK1",
      "D3kRzVbYF4wJ5qW7uN0sTp1L3H2m8oXG6CZa5Bp",
      "N2Q4Fz1Jk8Wv0RmH5gXpYdT3L7V9nJwU6qBc9y2V",
      "hVqL8eY9hAfJ2ZyGvKpT7u5wN1bXk06RzDlI",
      "jL0V1BzG8oK7q6mF9X4H3tW2N5rYwP9Vb0uY2L8N",
      "k3G8pV0YwT9L7mJ1qZ5v0W2bN1cH9Xg3R6K4zQ2Y",
      "kQb9ZfL6Dp0sW8XnV3mJ1rYw7E5qH2P9LgFZ5sT0",
      "p6A3t9V1fWqL0rH8Z7X2j9yF5b0G5cL2m3WqY0k9Z",
      "p8Lz7yF5w2m9rG0YqD6XnQW1b5Vj4kT3oH2KsZy7A",
      "wz2A4TgB1fV0o9c8Qe5jZyH7NkWv6pUdxXlJl0L"};

  char *err_str = NULL;
  int i;

  cvec_declare(cvec, char *);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(char *){test_strings[i]});
  }
  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  for (i = 0; i < num_sample; i++) {
    REQUIRE_TRUE(
        strcmp(*(char **)cvector_at(cvec, i), sorted_compare_values[i]) == 0);
  }

  {
    const char *first, *second;
    for (i = 1; i < num_sample; i++) {
      first = *(const char **)cvector_at(cvec, i - 1);
      second = *(const char **)cvector_at(cvec, i);
      REQUIRE_TRUE(strcmp(first, second) < 0);
    }
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

void create_random_str(char *dest, size_t length) {
  char charset[] =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  while (length-- > 0) {
    size_t index = (double)rand() / RAND_MAX * (sizeof charset - 1);
    *dest++ = charset[index];
  }
  *dest = '\0';
}

TEST(csort, cvector_string_sort_with_random_values) {
  const int num_sample = 10;
  const int max_str_lenght = 100;
  char rand_strings[10][100];

  char *err_str = NULL;
  int i;

  cvec_declare(cvec, char *);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++) {
    create_random_str(rand_strings[i],
                      (int)((double)rand() / RAND_MAX * max_str_lenght));
    cvector_push_back(cvec, &(char *){rand_strings[i]});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvec_sort(cvec);

  {
    const char *first, *second;
    for (i = 1; i < num_sample; i++) {
      first = *(const char **)cvector_at(cvec, i - 1);
      second = *(const char **)cvector_at(cvec, i);
      REQUIRE_TRUE(strcmp(first, second) < 0);
    }
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

typedef struct custom_test_struct {
  int data;
} custom_test_struct;

int custom_test_struct_comparison_proc(const void *first, const void *second) {
  custom_test_struct *p_struct_first = (custom_test_struct *)first;
  custom_test_struct *p_struct_second = (custom_test_struct *)second;

  return p_struct_first->data - p_struct_second->data;
}

TEST(csort, cvector_custom_test_struct_sort) {
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  char *err_str = NULL;
  int i;

  srand(seed);
  cvec_declare(cvec, custom_test_struct);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(custom_test_struct){.data = rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

  cvector_sort_with_comparison_proc(cvec, custom_test_struct_comparison_proc);

  custom_test_struct *p_struct_first;
  custom_test_struct *p_struct_second;
  for (i = 1; i < num_sample; i++) {
    p_struct_first = (custom_test_struct *)cvector_at(cvec, i - 1);
    p_struct_second = (custom_test_struct *)cvector_at(cvec, i);
    REQUIRE_LE(p_struct_first->data, p_struct_second->data);
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

TEST(csort, empty_vector_sort) {
  cvec_construct(v, int);
  REQUIRE_EQ(cvector_elem_count(v), 0);
  cvec_sort(v);
  REQUIRE_EQ(cvector_elem_count(v), 0);
  cvec_destroy(v);
}

TEST(csort, single_element_sort) {
  cvec_construct(v, int);
  cvec_push_rvalue(v, 42);
  cvec_sort(v);
  REQUIRE_EQ(cvector_elem_count(v), 1);
  REQUIRE_EQ(cvec_at(v, 0), 42);
  cvec_destroy(v);
}

TEST(csort, already_sorted_stays_sorted) {
  cvec_construct(v, int);
  for (int i = 0; i < 10; ++i) {
    cvec_push_rvalue(v, i);
  }
  cvec_sort(v);
  for (int i = 1; i < 10; ++i) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  cvec_destroy(v);
}

TEST(csort, reverse_sorted_input) {
  cvec_construct(v, int);
  for (int i = 9; i >= 0; --i) {
    cvec_push_rvalue(v, i);
  }
  cvec_sort(v);
  for (int i = 1; i < 10; ++i) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  cvec_destroy(v);
}

void *c_int_array_getter_proc_t(void *collection, size_t index) {
  return ((int *)collection) + index;
}

TEST(csort, c_int_array_sort) {
  const int num_sample = 10;
  const unsigned int seed = time(NULL);
  int test_array[10];
  int i;

  srand(seed);

  for (i = 0; i < num_sample; i++) {
    test_array[i] = rand();
  }

  csort_sort(test_array, sizeof(test_array) / sizeof(*test_array),
             sizeof(*test_array), c_int_array_getter_proc_t,
             csort_get_default_comparison_proc(*test_array), NULL);
  for (i = 1; i < num_sample; i++) {
    REQUIRE_LE(test_array[i - 1], test_array[i]);
  }
}

// The index-arithmetic bugs (int → size_t) only manifest for collections
// larger than INT_MAX elements and cannot be exercised directly. The tests
// below guard against regressions by running the algorithm over a much
// larger range of sizes than the rest of the suite (which tops out at 10
// elements) and verifying the stability property that mergesort claims.

TEST(csort, large_collection_sort) {
  // 1000 elements drives ~10 iterations of the outer doubling loop.
  const int n = 1000;
  cvec_construct(v, int);

  srand(1234);
  for (int i = 0; i < n; i++) {
    cvec_push_rvalue(v, rand());
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }

  cvec_destroy(v);
}

TEST(csort, power_of_two_collection_sort) {
  // 256 = 2^8 hits the exact loop-exit boundary of the outer doubling loop.
  const int n = 256;
  cvec_construct(v, int);

  for (int i = n - 1; i >= 0; i--) {
    cvec_push_rvalue(v, i);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }

  cvec_destroy(v);
}

typedef struct {
  int key;
  int original_index;
} stability_elem_t;

int stability_elem_comparison_proc(const void *first, const void *second) {
  const stability_elem_t *a = (const stability_elem_t *)first;
  const stability_elem_t *b = (const stability_elem_t *)second;
  return (a->key > b->key) - (a->key < b->key);
}

TEST(csort, sort_is_stable) {
  // Mergesort claims to be stable: equal-key elements must retain their
  // original relative order. Verify with 8 elements that have duplicate keys.
  int keys[] = {3, 1, 2, 1, 3, 2, 1, 3};
  const int n = 8;

  cvec_construct(v, stability_elem_t);

  for (int i = 0; i < n; i++) {
    cvector_push_back(v,
                      &(stability_elem_t){.key = keys[i], .original_index = i});
  }

  cvector_sort_with_comparison_proc(v, stability_elem_comparison_proc);

  for (int i = 1; i < n; i++) {
    stability_elem_t *prev = (stability_elem_t *)cvector_at(v, i - 1);
    stability_elem_t *curr = (stability_elem_t *)cvector_at(v, i);
    REQUIRE_LE(prev->key, curr->key);
    if (prev->key == curr->key) {
      REQUIRE_TRUE(prev->original_index < curr->original_index);
    }
  }

  cvec_destroy(v);
}