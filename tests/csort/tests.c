#include <csort.h>
#include <cvector.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()  // sets up Tau (+ main function)

#define define_integer_type_test(name, type, modulus_value)     \
  TEST(csort, cvector_##name##_sort) {                          \
    const int num_sample = 10;                                  \
    const unsigned int seed = time(NULL);                       \
                                                                \
    int i;                                                      \
                                                                \
    srand(seed);                                                \
    cvec_declare(cvec, type);                                   \
    cvec_init(cvec);                                            \
                                                                \
    REQUIRE_NE((void *)cvec, NULL);                             \
                                                                \
    for (i = 0; i < num_sample; i++) {                          \
      cvector_push_back(cvec, &(type){rand() % modulus_value}); \
    }                                                           \
                                                                \
    REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);   \
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

  int i;

  srand(seed);
  cvec_declare(cvec, int);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(int){rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

  cvec_sort(cvec);

  for (i = 1; i < num_sample; i++) {
    REQUIRE_LE(*(int *)cvector_at(cvec, i - 1), *(int *)cvector_at(cvec, i));
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

define_integer_type_test(char, char, 100)
    // define_integer_type_test(signed_char, signed char, 100) // TAU
    // REQUIRE_LE does not support signed char; see the dedicated
    // signed_char_default_comparator_is_not_null_and_sorts_signed test below
    // instead, which casts to int for the comparison.
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

  int i;

  srand(seed);
  cvec_declare(cvec, double);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(double){((double)rand() / RAND_MAX * 10)});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

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

  int i;

  srand(seed);
  cvec_declare(cvec, float);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(float){((float)rand() / (float)RAND_MAX * 10)});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

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

  int i;

  srand(seed);
  cvec_declare(cvec, long double);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec,
                      &(long double){((long double)rand() / RAND_MAX * 10)});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

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

  int i;

  cvec_declare(cvec, char *);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(char *){test_strings[i]});
  }
  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

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
  static const char charset[] =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  /* Use modulo so the index stays in [0, sizeof(charset) - 2], never reaching
   * the null terminator at charset[sizeof(charset) - 1]. */
  while (length-- > 0) {
    *dest++ = charset[rand() % (sizeof(charset) - 1)];
  }
  *dest = '\0';
}

TEST(csort, cvector_string_sort_with_random_values) {
  const int num_sample = 10;
  const int max_str_lenght = 100;
  char rand_strings[10][100];

  int i;

  cvec_declare(cvec, char *);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (i = 0; i < num_sample; i++) {
    /* rand() % max_str_lenght gives lengths in [0, max_str_lenght - 1], so the
     * generated string (plus its null terminator) always fits in the 100-byte
     * buffer. A floating-point scaling of rand() would instead yield
     * length == 100 when rand() == RAND_MAX, overflowing that buffer by one
     * byte. */
    create_random_str(rand_strings[i], (size_t)(rand() % max_str_lenght));
    cvector_push_back(cvec, &(char *){rand_strings[i]});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

  cvec_sort(cvec);

  {
    const char *first, *second;
    for (i = 1; i < num_sample; i++) {
      first = *(const char **)cvector_at(cvec, i - 1);
      second = *(const char **)cvector_at(cvec, i);
      /* Use <= 0: two random strings of equal content are a valid sort result
       * and would incorrectly fail a strict < 0 check. */
      REQUIRE_TRUE(strcmp(first, second) <= 0);
    }
  }

  cvector_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

typedef struct custom_test_struct {
  int data;
} custom_test_struct;

int custom_test_struct_comparison_proc(const void *first, const void *second) {
  const custom_test_struct *a = (const custom_test_struct *)first;
  const custom_test_struct *b = (const custom_test_struct *)second;
  return (a->data > b->data) - (a->data < b->data);
}

TEST(csort, cvector_custom_test_struct_sort) {
  const int num_sample = 10;
  const unsigned int seed = time(NULL);

  int i;

  srand(seed);
  cvec_declare(cvec, custom_test_struct);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (i = 0; i < num_sample; i++) {
    cvector_push_back(cvec, &(custom_test_struct){.data = rand()});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

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
  cvec_push(v, 42);
  cvec_sort(v);
  REQUIRE_EQ(cvector_elem_count(v), 1);
  REQUIRE_EQ(cvec_at(v, 0), 42);
  cvec_destroy(v);
}

TEST(csort, already_sorted_stays_sorted) {
  cvec_construct(v, int);
  for (int i = 0; i < 10; ++i) {
    cvec_push(v, i);
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
    cvec_push(v, i);
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

// csort_get_default_comparison_proc(x) must return a real comparator that is
// not NULL for a `signed char` x. Every other integral type above already has
// one. That includes the plain `char`, whose signedness depends on the
// platform. The comparator must also order genuinely signed values. It must
// not order raw byte patterns. A missing default, which is a NULL comparator
// or an unsigned one, gives that raw-byte order.
// csort_item_getter_proc_t needs exactly void *(*)(void *, size_t). The first
// parameter of cvector_at() is a cvec and not a void *. A cast of cvector_at
// straight to csort_item_getter_proc_t, and a call through that cast pointer,
// is therefore undefined behavior. See C11 6.3.2.3p8. The small adapter below
// has the exact signature that is needed, which closes that gap.
static void *csort_test_cvec_getter(void *collection, size_t index) {
  return cvector_at((cvec)collection, index);
}

TEST(csort, signed_char_default_comparator_is_not_null_and_sorts_signed) {
  ccol_comparison_proc_t cmp =
      csort_get_default_comparison_proc((signed char)0);
  REQUIRE_NE((void *)cmp, NULL);

  cvec_declare(vec, signed char);
  cvec_init(vec);
  cvec_push(vec, (signed char)3);
  cvec_push(vec, (signed char)-5);
  cvec_push(vec, (signed char)100);
  cvec_push(vec, (signed char)-100);
  cvec_push(vec, (signed char)0);

  const signed char expected[] = {-100, -5, 0, 3, 100};
  const int n = 5;

  bool result = csort_sort(vec, (size_t)n, sizeof(signed char),
                           csort_test_cvec_getter, cmp, NULL);
  REQUIRE_TRUE(result);
  for (int i = 0; i < n; i++) {
    // The REQUIRE_EQ printer of tau has no `signed char` case. It has only a
    // plain `char` case. The comparison therefore casts to int. This suite
    // uses that pattern for every type that the vendored printer does not
    // support directly.
    REQUIRE_EQ((int)*(signed char *)cvector_at(vec, i), (int)expected[i]);
  }

  cvec_destroy(vec);
}

static void *_csort_oom_malloc(size_t size) {
  (void)size;
  return NULL;
}
static void *_csort_oom_calloc(size_t count, size_t size) {
  (void)count;
  (void)size;
  return NULL;
}
static void *_csort_oom_realloc(void *ptr, size_t size) {
  (void)ptr;
  (void)size;
  return NULL;
}
static void _csort_oom_free(void *ptr) { (void)ptr; }

// The allocation of the temporary merge buffer can fail. csort_sort must
// report that failure through its return value. It must not silently do
// nothing, which leaves the caller with no way to know that the collection is
// still unsorted.
TEST(csort, csort_sort_reports_oom_and_leaves_collection_untouched) {
  ccol_memmgmt_procs_t always_fails = {.malloc = _csort_oom_malloc,
                                       .calloc = _csort_oom_calloc,
                                       .realloc = _csort_oom_realloc,
                                       .free = _csort_oom_free};

  int arr[] = {5, 3, 4, 1, 2};
  const int expected[] = {5, 3, 4, 1, 2};
  const int n = 5;

  bool result =
      csort_sort(arr, (size_t)n, sizeof(int), c_int_array_getter_proc_t,
                 csort_get_default_comparison_proc(arr[0]), &always_fails);

  REQUIRE_FALSE(result);
  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(arr[i], expected[i]);
  }
}

// The trivial length-0/1 cases have nothing to allocate for and must report
// success even under an allocator that fails every call.
TEST(csort, csort_sort_trivial_length_reports_success_even_under_oom) {
  ccol_memmgmt_procs_t always_fails = {.malloc = _csort_oom_malloc,
                                       .calloc = _csort_oom_calloc,
                                       .realloc = _csort_oom_realloc,
                                       .free = _csort_oom_free};

  int one[] = {42};
  REQUIRE_TRUE(
      csort_sort(one, (size_t)1, sizeof(int), c_int_array_getter_proc_t,
                 csort_get_default_comparison_proc(one[0]), &always_fails));
  REQUIRE_EQ(one[0], 42);

  REQUIRE_TRUE(
      csort_sort(one, (size_t)0, sizeof(int), c_int_array_getter_proc_t,
                 csort_get_default_comparison_proc(one[0]), &always_fails));
}

// This is the documented contract of ___csort_merge_sort. For a collection
// that is not NULL and whose length is 0 or 1, the trivial-success path
// returns true. It never looks at getter_proc or comparison_proc. The check
// on a length of 0 or 1 runs strictly before the check for a NULL proc. This
// combination must therefore succeed silently, and must never assert, even
// when both procs are genuinely NULL. It is the one documented
// trivial-success combination that no other test in this suite covers.
// csort_sort_trivial_length_reports_success_even_under_oom above covers a
// trivial length with procs that are valid.
// csort_sort_null_collection_reports_success_without_touching_anything below
// covers a NULL collection with procs that are valid.
TEST(csort, csort_sort_trivial_length_with_null_procs_reports_success) {
  int one[] = {42};
  REQUIRE_TRUE(csort_sort(one, (size_t)1, sizeof(int), NULL, NULL, NULL));
  REQUIRE_EQ(one[0], 42);

  REQUIRE_TRUE(csort_sort(one, (size_t)0, sizeof(int), NULL, NULL, NULL));
  REQUIRE_EQ(one[0], 42);
}

static bool g_null_col_getter_called = false;
static void *_csort_null_col_getter(void *collection, size_t index) {
  (void)collection;
  (void)index;
  g_null_col_getter_called = true;
  return NULL;
}
static bool g_null_col_cmp_called = false;
static int _csort_null_col_cmp(const void *a, const void *b) {
  (void)a;
  (void)b;
  g_null_col_cmp_called = true;
  return 0;
}

// The documented trivial-success contract of ___csort_merge_sort covers a
// NULL collection directly. It says "true ... including the trivial col ==
// NULL / length 0 or 1 cases, which have nothing to do". It does not cover
// only a length of 0 or 1 against a real collection that is not NULL. The
// trivial-length test just above covers that case. This contract must hold
// even for a length well above 1. The function must never try any allocation.
// It must never call getter_proc or comparison_proc. A NULL collection has no
// real memory behind it for either of them to work on safely.
TEST(csort,
     csort_sort_null_collection_reports_success_without_touching_anything) {
  ccol_memmgmt_procs_t always_fails = {.malloc = _csort_oom_malloc,
                                       .calloc = _csort_oom_calloc,
                                       .realloc = _csort_oom_realloc,
                                       .free = _csort_oom_free};

  g_null_col_getter_called = false;
  g_null_col_cmp_called = false;

  REQUIRE_TRUE(csort_sort(NULL, (size_t)5, sizeof(int), _csort_null_col_getter,
                          _csort_null_col_cmp, &always_fails));
  REQUIRE_FALSE(g_null_col_getter_called);
  REQUIRE_FALSE(g_null_col_cmp_called);
}

static bool g_overflow_test_getter_called = false;
static void *_csort_overflow_test_getter(void *collection, size_t index) {
  (void)collection;
  (void)index;
  g_overflow_test_getter_called = true;
  return NULL;
}
static bool g_overflow_test_cmp_called = false;
static int _csort_overflow_test_cmp(const void *a, const void *b) {
  (void)a;
  (void)b;
  g_overflow_test_cmp_called = true;
  return 0;
}

static bool g_overflow_test_alloc_called = false;
static void *_csort_overflow_test_malloc(size_t size) {
  g_overflow_test_alloc_called = true;
  return malloc(size);
}
static void *_csort_overflow_test_calloc(size_t count, size_t size) {
  g_overflow_test_alloc_called = true;
  return calloc(count, size);
}
static void *_csort_overflow_test_realloc(void *ptr, size_t size) {
  g_overflow_test_alloc_called = true;
  return realloc(ptr, size);
}
static void _csort_overflow_test_free(void *ptr) { free(ptr); }

// The product length * elem_size can overflow size_t. csort_sort must reject
// such a product before it tries the allocation of the temp buffer. That
// allocation would otherwise get a byte count that wrapped silently and is too
// small. Every merge pass still writes and reads elements of the full
// elem_size, at indices that come from the real length, which did not wrap.
// That is a heap buffer overflow. csort_sort must also reject the product
// before it touches the collection through getter_proc or comparison_proc.
// Neither one is safe to call for a length this large, because no real memory
// is behind it.
TEST(csort, csort_sort_rejects_length_elem_size_overflow_without_allocating) {
  ccol_memmgmt_procs_t counting = {.malloc = _csort_overflow_test_malloc,
                                   .calloc = _csort_overflow_test_calloc,
                                   .realloc = _csort_overflow_test_realloc,
                                   .free = _csort_overflow_test_free};

  g_overflow_test_getter_called = false;
  g_overflow_test_cmp_called = false;
  g_overflow_test_alloc_called = false;

  // Here elem_size is 2 and length is SIZE_MAX/2 + 1. The product length *
  // elem_size wraps around size_t. The length comes from SIZE_MAX/elem_size
  // directly, which is the same comparison that the guard itself uses. It is
  // not a hardcoded literal. It therefore stays a genuine overflow at every
  // pointer width, and not only at 64 bits. The +1 is deliberate, and a +2
  // would be wrong. For elem_size == 2, SIZE_MAX/elem_size + 1 is exactly
  // ccol_max_elem_count. Such a length passes the separate, earlier guard on
  // length > ccol_max_elem_count, which runs before this one. It still trips
  // this overflow guard. A length of +2 is *also* rejected, but the
  // ccol_max_elem_count guard rejects it, because it is past that cap too.
  // This test could then not tell the two guards apart. It would give no
  // coverage for this overflow guard if somebody removed or broke it.
  size_t elem_size = 2;
  size_t length = SIZE_MAX / elem_size + 1;

  int dummy_collection;  // address only; never dereferenced by this test
  bool result = csort_sort(&dummy_collection, length, elem_size,
                           _csort_overflow_test_getter,
                           _csort_overflow_test_cmp, &counting);

  REQUIRE_FALSE(result);
  REQUIRE_FALSE(g_overflow_test_getter_called);
  REQUIRE_FALSE(g_overflow_test_cmp_called);
  REQUIRE_FALSE(g_overflow_test_alloc_called);
}

static bool g_zero_elem_alloc_called = false;
static void *_csort_zero_elem_malloc(size_t size) {
  g_zero_elem_alloc_called = true;
  // This mimics an allocator that conforms and that returns NULL for a
  // request of zero bytes. The C standard leaves malloc(0) to the
  // implementation. glibc returns a pointer that is not NULL, but nothing
  // needs that. If csort_sort() ever made this allocation for elem_size == 0,
  // this allocator would always make it report a false out-of-memory error.
  // That would happen whatever libc the suite runs against.
  if (size == 0) {
    return NULL;
  }
  return malloc(size);
}
static void *_csort_zero_elem_calloc(size_t count, size_t size) {
  g_zero_elem_alloc_called = true;
  return calloc(count, size);
}
static void *_csort_zero_elem_realloc(void *ptr, size_t size) {
  g_zero_elem_alloc_called = true;
  return realloc(ptr, size);
}
static void _csort_zero_elem_free(void *ptr) { free(ptr); }

static bool g_zero_elem_getter_called = false;
static void *_csort_zero_elem_getter(void *collection, size_t index) {
  (void)collection;
  (void)index;
  g_zero_elem_getter_called = true;
  return NULL;
}
static bool g_zero_elem_cmp_called = false;
static int _csort_zero_elem_cmp(const void *a, const void *b) {
  (void)a;
  (void)b;
  g_zero_elem_cmp_called = true;
  return 0;
}

// An elem_size of 0 must report success. csort_sort must not try the
// allocation of the temp buffer for it. That allocation would be 0 bytes
// long, and the result of such a request is left to the implementation and
// can be NULL. csort_sort must also not call getter_proc or comparison_proc.
// An element of size zero has no bytes for either one to read, and none for a
// merge pass to move. The length here is well above the trivial case of 0 or
// 1. That shows that this is the dedicated short circuit of elem_size, and
// not a side effect of the unrelated trivial-length path.
TEST(csort, csort_sort_zero_elem_size_reports_success_without_allocating) {
  ccol_memmgmt_procs_t zero_size_returns_null = {
      .malloc = _csort_zero_elem_malloc,
      .calloc = _csort_zero_elem_calloc,
      .realloc = _csort_zero_elem_realloc,
      .free = _csort_zero_elem_free};

  g_zero_elem_alloc_called = false;
  g_zero_elem_getter_called = false;
  g_zero_elem_cmp_called = false;

  int dummy_collection;  // address only; never dereferenced by this test
  bool result = csort_sort(&dummy_collection, (size_t)10, (size_t)0,
                           _csort_zero_elem_getter, _csort_zero_elem_cmp,
                           &zero_size_returns_null);

  REQUIRE_TRUE(result);
  REQUIRE_FALSE(g_zero_elem_alloc_called);
  REQUIRE_FALSE(g_zero_elem_getter_called);
  REQUIRE_FALSE(g_zero_elem_cmp_called);
}

// csort_get_default_comparison_proc(x) must return NULL for a type that this
// module documents as unsupported. Such a type is not numeric, not integral,
// and not a genuine char* or const char* pointer. A return of NULL there is
// the contract that a caller with its own fallback depends on. cvec_find in
// cvector compares such an element byte by byte instead. This test therefore
// checks the return value of this macro directly.
TEST(csort, get_default_comparison_proc_returns_null_for_unsupported_types) {
  bool dummy_bool = true;
  ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(dummy_bool);
  REQUIRE_EQ((void *)cmp, NULL);

  custom_test_struct dummy_struct = {.data = 42};
  cmp = csort_get_default_comparison_proc(dummy_struct);
  REQUIRE_EQ((void *)cmp, NULL);
}

// ___csort_has_default_comparison_proc(x) answers at compile time the exact
// question that csort_get_default_comparison_proc(x) answers at run time.
// That is what lets cvec_sort reject an element type with no default
// comparison procedure through a _Static_assert, and not through an abort at
// run time. Each of the two carries its own _Generic association list.
// Nothing but a check like this one holds them together. A type added to the
// selector but not to the predicate turns a vector that sorts perfectly well
// into a build failure. A type added to the predicate but not to the selector
// puts the abort at run time straight back.
//
// This test also uses the predicate in a _Static_assert, and not only in a
// comparison at run time. Half of the purpose of the predicate is that it is
// a constant expression. A form written as a statement expression compiles as
// a plain value and fails only there.
typedef enum { csort_test_enum_a = 0, csort_test_enum_b = 11 } csort_test_enum;
typedef enum {
  csort_test_senum_a = -4,
  csort_test_senum_b = 6
} csort_test_senum;
typedef union {
  int as_int;
  double as_double;
} csort_test_union;
typedef char csort_test_char_array[64];
typedef int csort_test_int_array[4];
typedef void (*csort_test_fn_ptr)(void);

#define CSORT_TEST_PREDICATE_AGREES(T, expected)                              \
  do {                                                                        \
    T csort_pred_probe = {0};                                                 \
    _Static_assert(                                                           \
        ___csort_has_default_comparison_proc(csort_pred_probe) == (expected), \
        "___csort_has_default_comparison_proc(" #T                            \
        ") disagrees with the expected answer");                              \
    const int predicted =                                                     \
        ___csort_has_default_comparison_proc(csort_pred_probe) ? 1 : 0;       \
    const int actual =                                                        \
        csort_get_default_comparison_proc(csort_pred_probe) ? 1 : 0;          \
    REQUIRE_EQ(predicted, (expected));                                        \
    REQUIRE_EQ(actual, (expected));                                           \
  } while (0)

TEST(csort, has_default_comparison_proc_predicate_matches_the_selector) {
  CSORT_TEST_PREDICATE_AGREES(char, 1);
  CSORT_TEST_PREDICATE_AGREES(signed char, 1);
  CSORT_TEST_PREDICATE_AGREES(unsigned char, 1);
  CSORT_TEST_PREDICATE_AGREES(short, 1);
  CSORT_TEST_PREDICATE_AGREES(unsigned short, 1);
  CSORT_TEST_PREDICATE_AGREES(int, 1);
  CSORT_TEST_PREDICATE_AGREES(unsigned int, 1);
  CSORT_TEST_PREDICATE_AGREES(long, 1);
  CSORT_TEST_PREDICATE_AGREES(unsigned long, 1);
  CSORT_TEST_PREDICATE_AGREES(long long, 1);
  CSORT_TEST_PREDICATE_AGREES(unsigned long long, 1);
  CSORT_TEST_PREDICATE_AGREES(float, 1);
  CSORT_TEST_PREDICATE_AGREES(double, 1);
  CSORT_TEST_PREDICATE_AGREES(long double, 1);

  // A qualified type selects its unqualified association, because the
  // controlling expression of a _Generic selection undergoes lvalue
  // conversion.
  CSORT_TEST_PREDICATE_AGREES(const int, 1);
  CSORT_TEST_PREDICATE_AGREES(const double, 1);
  CSORT_TEST_PREDICATE_AGREES(const char, 1);

  // An enumerated type is compatible with one of the standard integer types,
  // so it sorts with that type's comparator. Both signedness outcomes are
  // covered, since they resolve to different comparators.
  CSORT_TEST_PREDICATE_AGREES(csort_test_enum, 1);
  CSORT_TEST_PREDICATE_AGREES(csort_test_senum, 1);

  // Genuine char pointer variables, every spelling the selector accepts.
  CSORT_TEST_PREDICATE_AGREES(char *, 1);
  CSORT_TEST_PREDICATE_AGREES(const char *, 1);
  CSORT_TEST_PREDICATE_AGREES(signed char *, 1);
  CSORT_TEST_PREDICATE_AGREES(const signed char *, 1);
  CSORT_TEST_PREDICATE_AGREES(unsigned char *, 1);
  CSORT_TEST_PREDICATE_AGREES(const unsigned char *, 1);

  // Everything with no default comparison procedure.
  CSORT_TEST_PREDICATE_AGREES(bool, 0);
  CSORT_TEST_PREDICATE_AGREES(custom_test_struct, 0);
  CSORT_TEST_PREDICATE_AGREES(csort_test_union, 0);
  CSORT_TEST_PREDICATE_AGREES(int *, 0);
  CSORT_TEST_PREDICATE_AGREES(double *, 0);
  CSORT_TEST_PREDICATE_AGREES(void *, 0);
  CSORT_TEST_PREDICATE_AGREES(csort_test_fn_ptr, 0);
  CSORT_TEST_PREDICATE_AGREES(csort_test_int_array, 0);

  // A char array of a fixed size decays to a char pointer when it is the
  // controlling expression of a _Generic. Without an explicit exclusion, the
  // macro therefore classifies such an array as a string. The default string
  // comparison procedure reads a stored char * value. It does not read the
  // bytes of the array itself.
  CSORT_TEST_PREDICATE_AGREES(csort_test_char_array, 0);
}

// A width mistake in the index arithmetic only misbehaves for a collection
// with more than INT_MAX elements. An int where a size_t is needed is such a
// mistake. No test can drive that directly. The tests below narrow that blind
// spot in two ways. They run the algorithm over a much wider range of sizes
// than the rest of the suite, which stops at 10 elements. They also check the
// stability property that a mergesort promises.

TEST(csort, large_collection_sort) {
  // 1000 elements drives ~10 iterations of the outer doubling loop.
  const int n = 1000;
  cvec_construct(v, int);

  srand(1234);
  for (int i = 0; i < n; i++) {
    cvec_push(v, rand());
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
    cvec_push(v, i);
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

TEST(csort, sort_negative_integers) {
  int values[] = {0, -5, 3, -100, 42, -1, 7, -3};
  const int n = 8;

  cvec_construct(v, int);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), -100);
  REQUIRE_EQ(cvec_at(v, n - 1), 42);

  cvec_destroy(v);
}

// sort_negative_integers above drives the default comparator of int against
// negative values. It is the only test that does so. Every OTHER signed
// integral type in this suite gets its coverage from the char, short, long
// and long_long instantiations of define_integer_type_test. Those only push
// rand() % modulus_value, which is never negative for any signed type they
// use. One shared macro generates all of these comparators, which is
// ___csort__define_default_integral_comparison_proc. A defect in only one of
// those types is therefore unlikely. But this project holds its tests to a
// high standard. The dedicated coverage of plain char signedness in cbstmap
// and cjson is an example. Without a negative-value test for each type, a
// typo specific to one type goes undetected. C makes short, long and long
// long clearly signed, and a plain char is different. See sort_full_range_
// chars below. A set of literal negative values, like the one in
// sort_negative_integers, is therefore portable to every platform that this
// library targets.

TEST(csort, sort_negative_shorts) {
  short values[] = {0, -5, 3, -100, 42, -1, 7, -3};
  const int n = 8;

  cvec_construct(v, short);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), (short)-100);
  REQUIRE_EQ(cvec_at(v, n - 1), (short)42);

  cvec_destroy(v);
}

TEST(csort, sort_negative_longs) {
  long values[] = {0, -5, 3, -100, 42, -1, 7, -3};
  const int n = 8;

  cvec_construct(v, long);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), -100L);
  REQUIRE_EQ(cvec_at(v, n - 1), 42L);

  cvec_destroy(v);
}

TEST(csort, sort_negative_long_longs) {
  long long values[] = {0, -5, 3, -100, 42, -1, 7, -3};
  const int n = 8;

  cvec_construct(v, long long);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), -100LL);
  REQUIRE_EQ(cvec_at(v, n - 1), 42LL);

  cvec_destroy(v);
}

// The platform defines the signedness of a plain char. It is signed on x86
// and x86_64. It is unsigned under the standard AAPCS64 ABI of aarch64. The
// test plain_char_negative_sign_extension in the cjson suite of this project
// covers the same point. A literal negative value, like the ones above for
// short, long and long long, is therefore not portable here. On a platform
// with an unsigned char, an assignment of -100 to a char is well defined and
// wraps to 156. Such a test no longer checks what it looks like it checks.
// The other char coverage of this suite is the define_integer_type_test(char,
// char, 100) instantiation. That one only pushes rand() % 100, which is a
// value from 0 to 99. A signed char and an unsigned char represent those
// values identically, so it cannot detect a signedness-ordering regression at
// all. This test is meaningful on both kinds of platform instead. It spans
// the full range that a char can represent, from CHAR_MIN to CHAR_MAX,
// whatever those two are here. It uses symbolic expressions and not literals
// that assume a signed char. It checks the ascending order with a plain <= on
// char values directly, which follows the comparison rules of the platform
// automatically.
TEST(csort, sort_full_range_chars) {
  char values[] = {(char)0,
                   CHAR_MIN,
                   (char)5,
                   CHAR_MAX,
                   (char)(CHAR_MIN + 1),
                   (char)(CHAR_MAX - 1),
                   (char)2,
                   (char)(CHAR_MIN + 2)};
  const int n = 8;

  cvec_construct(v, char);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), CHAR_MIN);
  REQUIRE_EQ(cvec_at(v, n - 1), CHAR_MAX);

  cvec_destroy(v);
}

// The three floating-point sort tests above are cvector_float_sort,
// cvector_double_sort and cvector_long_double_sort. Each one builds its values
// from rand()/RAND_MAX, so none of them is ever negative.
// sort_negative_integers just above is different, because it drives the int
// comparator against negative values directly. The default comparators use
// the same (a > b) - (a < b) idiom for every type. The three tests below are
// what check that negative floating-point values sort correctly, and not only
// that the sort does not crash. Their values are exact binary fractions, each
// one a sum of negative powers of two. REQUIRE_EQ can therefore check them
// with no rounding trouble.

TEST(csort, sort_negative_floats) {
  float values[] = {0.0f, -5.5f, 3.25f, -100.75f, 42.5f, -1.0f, 7.0f, -3.125f};
  const int n = 8;

  cvec_construct(v, float);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), -100.75f);
  REQUIRE_EQ(cvec_at(v, n - 1), 42.5f);

  cvec_destroy(v);
}

TEST(csort, sort_negative_doubles) {
  double values[] = {0.0, -5.5, 3.25, -100.75, 42.5, -1.0, 7.0, -3.125};
  const int n = 8;

  cvec_construct(v, double);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), -100.75);
  REQUIRE_EQ(cvec_at(v, n - 1), 42.5);

  cvec_destroy(v);
}

TEST(csort, sort_negative_long_doubles) {
  long double values[] = {0.0L,  -5.5L, 3.25L, -100.75L,
                          42.5L, -1.0L, 7.0L,  -3.125L};
  const int n = 8;

  cvec_construct(v, long double);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  for (int i = 1; i < n; i++) {
    REQUIRE_LE(cvec_at(v, i - 1), cvec_at(v, i));
  }
  REQUIRE_EQ(cvec_at(v, 0), -100.75L);
  REQUIRE_EQ(cvec_at(v, n - 1), 42.5L);

  cvec_destroy(v);
}

TEST(csort, sort_all_duplicates) {
  const int n = 8;

  cvec_construct(v, int);
  for (int i = 0; i < n; i++) {
    cvec_push(v, 7);
  }

  cvec_sort(v);

  REQUIRE_EQ(cvector_elem_count(v), (size_t)n);
  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(cvec_at(v, i), 7);
  }

  cvec_destroy(v);
}

TEST(csort, sort_two_elements_ascending) {
  cvec_construct(v, int);
  cvec_push(v, 1);
  cvec_push(v, 2);
  cvec_sort(v);
  REQUIRE_EQ(cvec_at(v, 0), 1);
  REQUIRE_EQ(cvec_at(v, 1), 2);
  cvec_destroy(v);
}

TEST(csort, sort_two_elements_descending) {
  cvec_construct(v, int);
  cvec_push(v, 2);
  cvec_push(v, 1);
  cvec_sort(v);
  REQUIRE_EQ(cvec_at(v, 0), 1);
  REQUIRE_EQ(cvec_at(v, 1), 2);
  cvec_destroy(v);
}

TEST(csort, cvector_unsigned_long_long_sort) {
  /* REQUIRE_LE does not support unsigned long long in tau, so the ordering
   * check is written as REQUIRE_TRUE with an explicit <= comparison. */
  const int num_sample = 10;

  srand((unsigned int)time(NULL));
  cvec_declare(cvec, unsigned long long);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

  for (int i = 0; i < num_sample; i++) {
    unsigned long long val =
        ((unsigned long long)rand() << 32) | (unsigned long long)rand();
    cvector_push_back(cvec, &val);
  }

  REQUIRE_EQ(cvector_elem_count(cvec), (size_t)num_sample);

  cvec_sort(cvec);

  for (int i = 1; i < num_sample; i++) {
    unsigned long long prev = *(unsigned long long *)cvector_at(cvec, i - 1);
    unsigned long long curr = *(unsigned long long *)cvector_at(cvec, i);
    REQUIRE_TRUE(prev <= curr);
  }

  cvec_destroy(cvec);
  REQUIRE_EQ((void *)cvec, NULL);
}

void *c_str_array_getter(void *collection, size_t index) {
  return ((char **)collection) + index;
}

TEST(csort, c_str_array_sort) {
  /* Verifies csort_sort on a plain C array of char* pointers. */
  char *arr[] = {"banana", "apple", "cherry", "date", "apricot"};
  const int n = 5;

  csort_sort(arr, (size_t)n, sizeof(char *), c_str_array_getter,
             csort_get_default_comparison_proc(arr[0]), NULL);

  for (int i = 1; i < n; i++) {
    REQUIRE_TRUE(strcmp(arr[i - 1], arr[i]) <= 0);
  }
  REQUIRE_STREQ(arr[0], "apple");
  REQUIRE_STREQ(arr[4], "date");
}

// csort_sort must reject a length that is above ccol_max_elem_count. It must
// do so before it tries any allocation, and before it calls getter_proc or
// comparison_proc. The overflow guard on length * elem_size just above works
// in the same way. But this rejection must hold even when elem_size == 0. The
// overflow guard never rejects that case on its own, on purpose. An elem_size
// of zero can never make length * elem_size overflow, so that guard alone
// leaves the length with no bound at all. Without a dedicated cap here, this
// exact combination lets the internal bottom-up doubling counter of the sort
// wrap around the range of size_t. The sort then spins forever. It never
// finishes and it never reports an error.
TEST(csort, csort_sort_rejects_length_exceeding_max_elem_count) {
  ccol_memmgmt_procs_t counting = {.malloc = _csort_overflow_test_malloc,
                                   .calloc = _csort_overflow_test_calloc,
                                   .realloc = _csort_overflow_test_realloc,
                                   .free = _csort_overflow_test_free};

  g_overflow_test_getter_called = false;
  g_overflow_test_cmp_called = false;
  g_overflow_test_alloc_called = false;

  int dummy_collection;  // address only; never dereferenced by this test
  bool result = csort_sort(&dummy_collection, ccol_max_elem_count + 1, 0,
                           _csort_overflow_test_getter,
                           _csort_overflow_test_cmp, &counting);

  REQUIRE_FALSE(result);
  REQUIRE_FALSE(g_overflow_test_getter_called);
  REQUIRE_FALSE(g_overflow_test_cmp_called);
  REQUIRE_FALSE(g_overflow_test_alloc_called);
}

// The test above drives the length > ccol_max_elem_count guard only with
// elem_size == 0. In that case the guard is not the thing that does the work.
// An elem_size of 0 has its own, completely separate short circuit. See
// csort_sort_zero_elem_size_reports_success_without_allocating. That short
// circuit returns success and never reaches the doubling loop that this guard
// protects. It does so whether this guard is present or not. For an elem_size
// of 2 or more, the overflow guard on length * elem_size below this one
// already rejects any length near ccol_max_elem_count on its own. SIZE_MAX /
// elem_size is already below ccol_max_elem_count for every elem_size of 2 or
// more. The rejection range of this guard for those elem_size values is
// therefore empty too. An elem_size of 1 is the ONE value for which neither
// of those holds. SIZE_MAX / 1 == SIZE_MAX, so for elem_size == 1 the
// overflow guard rejects nothing on the basis of size at all. This
// length > ccol_max_elem_count check is then the only thing between a length
// above ccol_max_elem_count and the doubling loop. In that loop curr_size *=
// 2 overflows to zero and the loop never ends. Without a test that drives
// exactly this combination, which is a nonzero elem_size with a length above
// ccol_max_elem_count, a regression hides. A change that removed this guard
// still reports false for the elem_size == 0 case above. A change that put
// the guard in the wrong order does the same. That case never depended on
// this guard. Such a change also still reports false for a case with a
// nonzero elem_size. The real allocator, or the operating system, fails an
// allocation this large anyway.
// That failure comes from exhausted virtual address space, and not from the
// guard. It hides the missing check instead of catching it. This test closes
// that gap in the same way as the elem_size == 0 test. It asserts that
// nothing ever called the allocator, and not only that the overall result was
// false.
TEST(csort,
     csort_sort_rejects_length_exceeding_max_elem_count_for_nonzero_elem_size) {
  ccol_memmgmt_procs_t counting = {.malloc = _csort_overflow_test_malloc,
                                   .calloc = _csort_overflow_test_calloc,
                                   .realloc = _csort_overflow_test_realloc,
                                   .free = _csort_overflow_test_free};

  g_overflow_test_getter_called = false;
  g_overflow_test_cmp_called = false;
  g_overflow_test_alloc_called = false;

  int dummy_collection;  // address only; never dereferenced by this test
  bool result = csort_sort(&dummy_collection, ccol_max_elem_count + 1, 1,
                           _csort_overflow_test_getter,
                           _csort_overflow_test_cmp, &counting);

  REQUIRE_FALSE(result);
  REQUIRE_FALSE(g_overflow_test_getter_called);
  REQUIRE_FALSE(g_overflow_test_cmp_called);
  REQUIRE_FALSE(g_overflow_test_alloc_called);
}

// The documentation of ___csort_merge_sort says that it asserts, and so stops
// the process, when getter_proc is NULL. That holds for a real collection
// that is not NULL and whose length is past the trivial cases of 0 and 1.
// This test runs in a forked child, because ccol_assert() and abort() stop
// the whole process. This codebase uses that pattern for every fatal path.
// The tests sort_out_of_memory_is_fatal and
// sort_unsupported_type_is_fatal_with_clear_diagnostic in
// tests/cvector/tests.c are examples. Those two reach this exact assert only
// indirectly, through the NULL-default-comparator path of cvec_sort. They
// never pass a getter_proc that is NULL directly, which csort_sort itself
// allows.
TEST(csort, csort_sort_null_getter_proc_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int arr[] = {2, 1};
    csort_sort(arr, (size_t)2, sizeof(int), NULL,
               csort_get_default_comparison_proc(arr[0]), NULL);
    _exit(0); /* unreachable if ccol_assert(false) aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Same as above, but for a NULL comparison_proc instead of a NULL
// getter_proc, exercising the other half of the same guard's ||.
TEST(csort, csort_sort_null_comparison_proc_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int arr[] = {2, 1};
    csort_sort(arr, (size_t)2, sizeof(int), c_int_array_getter_proc_t, NULL,
               NULL);
    _exit(0); /* unreachable if ccol_assert(false) aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The short circuit for elem_size == 0 lives inside csort_mergesort_iterative,
// which is a helper of ___csort_merge_sort. See
// csort_sort_zero_elem_size_reports_success_without_allocating above. A call
// reaches that helper only AFTER the assert of the outer function, which
// tests getter_proc and comparison_proc for NULL. An elem_size of 0 therefore
// does NOT exempt a call with a length of 2 or more from that assert. Such a
// call would never need to call either proc, and the assert still fires. This
// behaviour is documented and deliberate. The note on ___csort_merge_sort
// says "Will assert ... unless col is NULL or length is 0 or 1". That wording
// already keeps elem_size == 0 out of the exemption for a NULL proc. This
// test pins that behaviour as a fatal-path test in a forked child.
// csort_sort_null_getter_proc_is_fatal and
// csort_sort_null_comparison_proc_is_fatal above have the same shape. A
// future change to the order of these two checks therefore cannot change this
// behaviour in either direction without a test that notices.
TEST(
    csort,
    csort_sort_null_procs_with_zero_elem_size_is_still_fatal_for_non_trivial_length) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int dummy_collection;  // address only; never dereferenced by this test
    csort_sort(&dummy_collection, (size_t)5, (size_t)0, NULL, NULL, NULL);
    _exit(0); /* unreachable if ccol_assert(false) aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// csort_get_default_comparison_proc holds a result variable inside its own
// statement expression. That variable must never shadow an argument from the
// caller. A caller can pass an identifier whose name is literally the same as
// that internal variable. The declarator-scope rules of C say that the scope
// of the internal variable starts directly after its own declarator. That is
// before the macro expands its parameter into the body. The internal
// variable and the argument of the caller can share a name. Every use of the
// parameter inside the expansion then resolves to the fresh local of
// the macro. That local has no value yet. The result is a silently wrong NULL,
// and the compiler reports nothing. The internal locals of cvec_push in the
// cvector module carry defensive names against this same hazard.
TEST(csort,
     get_default_comparison_proc_not_shadowed_by_arg_named_comparison_proc) {
  int comparison_proc = 42;
  ccol_comparison_proc_t cmp =
      csort_get_default_comparison_proc(comparison_proc);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)_csort_default_int_comparison_proc);

  char *comparison_proc_str = "hello";
  cmp = csort_get_default_comparison_proc(comparison_proc_str);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)_csort_default_string_comparison_proc);
}

// csort_get_default_comparison_proc(x) must return NULL for a genuine char
// array of a fixed size, which is a char[N]. A `char name[64]` struct field
// is such an array. It must not return the string comparator. The controlling
// expression of a _Generic goes through the ordinary decay from an array to a
// pointer. See C11 6.5.1.1p2 and 6.3.2.1p3. A bare ccol_is_char_ptr(x) check
// therefore cannot separate two cases. In the first, x really is a char*
// variable. In the second, x is an array that only decayed and now looks like
// one for this comparison. ccol_is_char_ptr(some_char_array) is true, and a
// small standalone _Generic snippet confirms that. Without an explicit
// !ccol_is_char_array(x) exclusion, this macro therefore hands back
// _csort_default_string_comparison_proc for an array argument. The contract of
// that comparator needs its arguments to point at a stored char* VALUE,
// because it does one more pointer indirection, *(const char **)first. It
// does not accept a pointer to the inline bytes of the array. A call against
// real array data is therefore undefined behavior, and not only a wrong sort
// order. It builds an arbitrary pointer from the first sizeof(char*) content
// bytes of the array and dereferences it. NULL is the correct result for any
// type that this macro does not recognize, and the documentation already says
// so.
TEST(csort, get_default_comparison_proc_returns_null_for_char_array) {
  char name[64] = "hello";
  ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(name);
  REQUIRE_EQ((void *)cmp, NULL);

  // A differently-sized array must be rejected the same way, not just the
  // one size used above.
  char short_buf[8] = "hi";
  cmp = csort_get_default_comparison_proc(short_buf);
  REQUIRE_EQ((void *)cmp, NULL);

  // A genuine char* pointer variable (as opposed to an array) must still be
  // correctly recognized as a string type and remain unaffected by that rule.
  char *p = name;
  cmp = csort_get_default_comparison_proc(p);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)_csort_default_string_comparison_proc);
}

// The difference between an array and a pointer above is not a special case
// for a plain `char[N]`. ccol_is_char_ptr() and ccol_is_char_array() in
// common.h treat `signed char *` and `unsigned char *` as string-like
// pointers, exactly like a `char *`. A `signed char[N]` array and an
// `unsigned char[N]` array must therefore stay out of
// _csort_default_string_comparison_proc in the same way. The identical
// !ccol_is_char_array(__csort_gdcp_arr_probe) check does that. This test and
// the one below drive those two spellings directly. They do not leave them to
// a reading of the plain `char[N]` case above.
TEST(csort, get_default_comparison_proc_returns_null_for_signed_char_array) {
  signed char name[64] = {1, 2, 3, 0};
  ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(name);
  REQUIRE_EQ((void *)cmp, NULL);

  signed char *p = name;
  cmp = csort_get_default_comparison_proc(p);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)_csort_default_string_comparison_proc);
}

TEST(csort, get_default_comparison_proc_returns_null_for_unsigned_char_array) {
  unsigned char name[64] = {1, 2, 3, 0};
  ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(name);
  REQUIRE_EQ((void *)cmp, NULL);

  unsigned char *p = name;
  cmp = csort_get_default_comparison_proc(p);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)_csort_default_string_comparison_proc);
}

// _csort_default_float_comparison_proc, _csort_default_double_comparison_proc
// and _csort_default_long_double_comparison_proc must give a genuine total
// order, even when one or both operands are NaN. A NaN is greater than every
// value that is not a NaN. It is equal only to another NaN. The native `<`
// and `>` of IEEE 754 are both false whenever either operand is a NaN.
// Without this rule, the plain (a>b)-(a<b) idiom therefore reports a NaN as
// "equal" to everything. That includes two unrelated non-NaN values that the
// NaN sits between. Without that order, csort_sort() over the doubles
// {9, NaN, 1, 4, NaN, 2, 7} gives {1, 4, 9, NaN, NaN, 2, 7}. The non-NaN part
// of that result, 1, 4, 9, 2, 7, is not sorted. This test pins the
// return-value contract of the comparators directly, for all three
// floating-point types. sort_with_nan_does_not_corrupt_non_nan_order below
// drives the same rule indirectly, through a full cvec_sort().
TEST(csort, float_comparators_order_nan_as_greatest_and_equal_only_to_nan) {
  float f_nan = NAN, f_five = 5.0f, f_nan2 = NAN;
  REQUIRE_GT(_csort_default_float_comparison_proc(&f_nan, &f_five), 0);
  REQUIRE_LT(_csort_default_float_comparison_proc(&f_five, &f_nan), 0);
  REQUIRE_EQ(_csort_default_float_comparison_proc(&f_nan, &f_nan2), 0);

  double d_nan = NAN, d_five = 5.0, d_nan2 = NAN;
  REQUIRE_GT(_csort_default_double_comparison_proc(&d_nan, &d_five), 0);
  REQUIRE_LT(_csort_default_double_comparison_proc(&d_five, &d_nan), 0);
  REQUIRE_EQ(_csort_default_double_comparison_proc(&d_nan, &d_nan2), 0);

  long double ld_nan = NAN, ld_five = 5.0L, ld_nan2 = NAN;
  REQUIRE_GT(_csort_default_long_double_comparison_proc(&ld_nan, &ld_five), 0);
  REQUIRE_LT(_csort_default_long_double_comparison_proc(&ld_five, &ld_nan), 0);
  REQUIRE_EQ(_csort_default_long_double_comparison_proc(&ld_nan, &ld_nan2), 0);

  // Finite values (no NaN involved) must still compare by plain numeric
  // ordering.
  float f_three = 3.0f;
  REQUIRE_LT(_csort_default_float_comparison_proc(&f_three, &f_five), 0);
  REQUIRE_GT(_csort_default_float_comparison_proc(&f_five, &f_three), 0);
}

// The (v1 > v2) - (v1 < v2) path runs whenever neither operand is a NaN. That
// path must report -0.0 and 0.0 as equal. This matches the numeric equality
// rule of IEEE 754, where -0.0 == 0.0. It does not match a comparison of raw
// bit patterns, which separates them. cbstmap has the same -0.0 and 0.0 check
// for its key comparator for a float, a double and a long double.
TEST(csort, float_comparators_treat_negative_zero_as_equal_to_positive_zero) {
  float f_neg_zero = -0.0f, f_pos_zero = 0.0f;
  REQUIRE_EQ(_csort_default_float_comparison_proc(&f_neg_zero, &f_pos_zero), 0);
  REQUIRE_EQ(_csort_default_float_comparison_proc(&f_pos_zero, &f_neg_zero), 0);

  double d_neg_zero = -0.0, d_pos_zero = 0.0;
  REQUIRE_EQ(_csort_default_double_comparison_proc(&d_neg_zero, &d_pos_zero),
             0);
  REQUIRE_EQ(_csort_default_double_comparison_proc(&d_pos_zero, &d_neg_zero),
             0);

  long double ld_neg_zero = -0.0L, ld_pos_zero = 0.0L;
  REQUIRE_EQ(
      _csort_default_long_double_comparison_proc(&ld_neg_zero, &ld_pos_zero),
      0);
  REQUIRE_EQ(
      _csort_default_long_double_comparison_proc(&ld_pos_zero, &ld_neg_zero),
      0);
}

// This test covers the same NaN total-order rule from end to end. It drives
// cvec_sort(), which is the main documented entry point that most callers
// use. It does not call the raw comparator functions directly. A NaN anywhere
// in the vector must not corrupt the relative order of the other elements
// that are not NaN. The NaN values themselves must land at the end, because
// they are the greatest. Their order among themselves does not matter.
TEST(csort, sort_with_nan_does_not_corrupt_non_nan_order) {
  double values[] = {9.0, NAN, 1.0, 4.0, NAN, 2.0, 7.0};
  const int n = 7;

  cvec_construct(v, double);
  for (int i = 0; i < n; i++) {
    cvector_push_back(v, &values[i]);
  }

  cvec_sort(v);

  double last = -1e300;
  bool have_last = false;
  int nan_count = 0;
  for (int i = 0; i < n; i++) {
    double val = cvec_at(v, i);
    if (isnan(val)) {
      nan_count++;
      // Every NaN must sort after every non-NaN value: once we've seen a
      // NaN, every remaining element (NaN or not) must also be NaN.
      continue;
    }
    REQUIRE_EQ(nan_count, 0);
    if (have_last) {
      REQUIRE_LE(last, val);
    }
    last = val;
    have_last = true;
  }
  REQUIRE_EQ(nan_count, 2);
  REQUIRE_TRUE(have_last);
  REQUIRE_EQ(last, 9.0);

  cvec_destroy(v);
}

static int g_csort_custom_alloc_malloc_calls = 0;
static int g_csort_custom_alloc_free_calls = 0;
static void *g_csort_custom_alloc_last_malloc_ptr = NULL;
static size_t g_csort_custom_alloc_last_malloc_size = 0;

static void *_csort_custom_malloc(size_t size) {
  g_csort_custom_alloc_malloc_calls++;
  g_csort_custom_alloc_last_malloc_size = size;
  void *p = malloc(size);
  g_csort_custom_alloc_last_malloc_ptr = p;
  return p;
}
static void *_csort_custom_calloc(size_t count, size_t size) {
  return calloc(count, size);
}
static void *_csort_custom_realloc(void *ptr, size_t size) {
  return realloc(ptr, size);
}
static void _csort_custom_free(void *ptr) {
  g_csort_custom_alloc_free_calls++;
  if (ptr != NULL && ptr == g_csort_custom_alloc_last_malloc_ptr) {
    g_csort_custom_alloc_last_malloc_ptr = NULL;
  }
  free(ptr);
}

// csort_sort() must send the allocation and the free of its temp buffer
// through a custom allocator from the caller that genuinely works. It must do
// so for an ordinary sort that succeeds and that is not trivial. Two other
// tests cover less than that. csort_sort_reports_oom_and_leaves_collection_
// untouched only checks that the sort fails cleanly when the allocator fails.
// csort_sort_rejects_length_elem_size_overflow_without_allocating and
// csort_sort_rejects_length_exceeding_max_elem_count only check a rejected
// input, and both assert that nothing ever calls the allocator. None of those
// drives the real round trip of an allocation and then a free. A real
// integration with a custom allocator depends on that round trip. Without
// this test, a regression that silently used the default heap for the temp
// buffer goes undetected. This test checks the call counts. It also checks
// that the freed pointer is the one that the allocation gave back.
TEST(csort, csort_sort_uses_the_provided_custom_allocator_for_a_real_sort) {
  ccol_memmgmt_procs_t custom = {.malloc = _csort_custom_malloc,
                                 .calloc = _csort_custom_calloc,
                                 .realloc = _csort_custom_realloc,
                                 .free = _csort_custom_free};

  g_csort_custom_alloc_malloc_calls = 0;
  g_csort_custom_alloc_free_calls = 0;
  g_csort_custom_alloc_last_malloc_ptr = NULL;
  g_csort_custom_alloc_last_malloc_size = 0;

  const int n = 20;
  int arr[20];
  srand(99);
  for (int i = 0; i < n; i++) {
    arr[i] = rand() % 1000;
  }

  bool result =
      csort_sort(arr, (size_t)n, sizeof(int), c_int_array_getter_proc_t,
                 csort_get_default_comparison_proc(arr[0]), &custom);

  REQUIRE_TRUE(result);
  for (int i = 1; i < n; i++) {
    REQUIRE_LE(arr[i - 1], arr[i]);
  }

  // There must be exactly one malloc for the temp buffer, of exactly the
  // expected size. There must also be exactly one matching free. That means
  // that the free of the custom allocator got back the same pointer that its
  // own malloc gave. It did not get a pointer from the default heap, a
  // leaked pointer, or a duplicate one.
  REQUIRE_EQ(g_csort_custom_alloc_malloc_calls, 1);
  REQUIRE_EQ(g_csort_custom_alloc_free_calls, 1);
  REQUIRE_EQ(g_csort_custom_alloc_last_malloc_size, (size_t)n * sizeof(int));
  REQUIRE_EQ((void *)g_csort_custom_alloc_last_malloc_ptr, NULL);
}

// csort_sort() must reject an mprocs that is not NULL and that does not have
// all four of malloc, free, calloc and realloc. ccol_memmgmt_procs_t
// documents that "all-or-nothing" contract. Every other entry point in this
// library that accepts an allocator already enforces it. cvector_create_full
// and chmap_create_full are two of them. They all call
// ccol_verify_memmgmt_procs() before they touch such a struct. Without that
// check, a struct with .malloc, .calloc and .realloc set and .free left NULL
// lets the allocation of the temp buffer succeed. The sort then finishes and
// crashes on a call through the NULL .free function pointer. It does not fail
// cleanly, which is what every sibling module already does for the identical
// mistake. The test create_with_invalid_mem_mgmt_procs in
// tests/cvector/tests.c has the same shape, one field at a time. This test
// uses a genuine length that is not trivial, well above the trivial-success
// path of 0 or 1, and an elem_size that is not zero. This guard is therefore
// the ONE thing under test. No unrelated trivial-success path or overflow
// guard can short-circuit it.
TEST(csort, csort_sort_rejects_incomplete_mprocs_without_allocating) {
  int arr[] = {5, 3, 4, 1, 2};
  const int expected[] = {5, 3, 4, 1, 2};
  const int n = 5;

  ccol_memmgmt_procs_t missing_free = {
      .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc};
  ccol_memmgmt_procs_t missing_malloc = {
      .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc};
  ccol_memmgmt_procs_t missing_calloc = {
      .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc};
  ccol_memmgmt_procs_t missing_realloc = {
      .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL};
  ccol_memmgmt_procs_t *incomplete_variants[] = {
      &missing_free, &missing_malloc, &missing_calloc, &missing_realloc};

  for (size_t v = 0;
       v < sizeof(incomplete_variants) / sizeof(*incomplete_variants); v++) {
    g_overflow_test_getter_called = false;
    g_overflow_test_cmp_called = false;

    bool result =
        csort_sort(arr, (size_t)n, sizeof(int), _csort_overflow_test_getter,
                   _csort_overflow_test_cmp, incomplete_variants[v]);

    REQUIRE_FALSE(result);
    REQUIRE_FALSE(g_overflow_test_getter_called);
    REQUIRE_FALSE(g_overflow_test_cmp_called);
    for (int i = 0; i < n; i++) {
      REQUIRE_EQ(arr[i], expected[i]);
    }
  }
}

// The trivial-success paths never touch mprocs at all. Those paths are a col
// that is NULL, a length of 0 or 1, and an elem_size of 0. See the doc
// comment on ___csort_merge_sort. An mprocs that is incomplete must therefore
// not turn any of them into a failure. The check on mprocs fires only
// directly before the allocation of the temp buffer that it guards.
TEST(csort, csort_sort_trivial_paths_ignore_incomplete_mprocs) {
  ccol_memmgmt_procs_t missing_free = {
      .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc};

  int one[] = {42};
  REQUIRE_TRUE(
      csort_sort(one, (size_t)1, sizeof(int), c_int_array_getter_proc_t,
                 csort_get_default_comparison_proc(one[0]), &missing_free));
  REQUIRE_EQ(one[0], 42);

  REQUIRE_TRUE(
      csort_sort(one, (size_t)0, sizeof(int), c_int_array_getter_proc_t,
                 csort_get_default_comparison_proc(one[0]), &missing_free));

  REQUIRE_TRUE(
      csort_sort(NULL, (size_t)5, sizeof(int), c_int_array_getter_proc_t,
                 csort_get_default_comparison_proc(one[0]), &missing_free));

  int dummy_collection;  // address only; never dereferenced by this test
  REQUIRE_TRUE(csort_sort(
      &dummy_collection, (size_t)10, (size_t)0, c_int_array_getter_proc_t,
      csort_get_default_comparison_proc(one[0]), &missing_free));
}
// NULL is an ordinary member of the char * element type. An argv-style list
// holds NULL entries, and so does a sparse table of optional strings. The
// default string comparator must therefore give NULL a defined position of
// its own. It must not hand NULL to strcmp(). NULL sorts before every string
// that is not NULL. It is equal only to another NULL. That keeps the
// comparator a genuine total order. The take-left or take-right decision of
// csort_merge() depends on that order. A comparator that answered
// inconsistently for NULL would therefore also misorder the strings around
// it. This test is not vacuous. Without the NULL rule, the first assertion
// below dereferences a null pointer inside strcmp(). The whole binary then
// dies with SIGSEGV.
TEST(csort, string_comparator_orders_null_before_every_non_null_string) {
  const char *null_str = NULL;
  const char *alpha = "alpha";
  const char *empty = "";
  const char *null_str2 = NULL;

  REQUIRE_LT(_csort_default_string_comparison_proc(&null_str, &alpha), 0);
  REQUIRE_GT(_csort_default_string_comparison_proc(&alpha, &null_str), 0);
  REQUIRE_EQ(_csort_default_string_comparison_proc(&null_str, &null_str2), 0);

  // Even the empty string, which compares less than every other non-empty
  // string, still sorts after NULL.
  REQUIRE_LT(_csort_default_string_comparison_proc(&null_str, &empty), 0);
  REQUIRE_GT(_csort_default_string_comparison_proc(&empty, &null_str), 0);

  // A pair with no NULL in it still compares by plain strcmp() ordering.
  REQUIRE_LT(_csort_default_string_comparison_proc(&empty, &alpha), 0);
  REQUIRE_GT(_csort_default_string_comparison_proc(&alpha, &empty), 0);
}

// This test covers the rule above from end to end. It runs a real
// csort_sort() over a C array of char * that holds NULL entries. Every NULL
// must land at the front. The part that is not NULL must stay fully sorted.
// Without the NULL rule this dies with SIGSEGV. It dies on the first
// comparison that reaches a NULL entry.
TEST(csort, sort_string_array_with_null_entries_orders_nulls_first) {
  char *arr[] = {"pear", NULL, "apple", "fig", NULL, "cherry"};
  const size_t n = sizeof(arr) / sizeof(arr[0]);

  REQUIRE_TRUE(csort_sort(arr, n, sizeof(char *), c_str_array_getter,
                          csort_get_default_comparison_proc(arr[0]), NULL));

  REQUIRE_EQ((void *)arr[0], NULL);
  REQUIRE_EQ((void *)arr[1], NULL);
  REQUIRE_STREQ(arr[2], "apple");
  REQUIRE_STREQ(arr[3], "cherry");
  REQUIRE_STREQ(arr[4], "fig");
  REQUIRE_STREQ(arr[5], "pear");

  for (size_t i = 3; i < n; i++) {
    REQUIRE_TRUE(strcmp(arr[i - 1], arr[i]) <= 0);
  }
}

/* A char pointer object that is itself const still names a string, so it
 * has the string comparator. A char array of any qualification has none. */
static const char *const csort_qual_const_name = "q";

TEST(csort_qualified,
     a_const_char_pointer_object_selects_the_string_comparator) {
  char *const cp = "cp";
  const signed char *const scp = (const signed char *)"s";
  unsigned char *volatile vup = (unsigned char *)"u";
  const char *plain = "p";
  ccol_comparison_proc_t expected = csort_get_default_comparison_proc(plain);
  REQUIRE_NE((void *)expected, NULL);
  REQUIRE_EQ((void *)csort_get_default_comparison_proc(csort_qual_const_name),
             (void *)expected);
  REQUIRE_EQ((void *)csort_get_default_comparison_proc(cp), (void *)expected);
  REQUIRE_EQ((void *)csort_get_default_comparison_proc(scp), (void *)expected);
  REQUIRE_EQ((void *)csort_get_default_comparison_proc(vup), (void *)expected);
  REQUIRE_TRUE(___csort_has_default_comparison_proc(csort_qual_const_name));
  REQUIRE_STREQ(csort_qual_const_name, "q");
  REQUIRE_TRUE(___csort_has_default_comparison_proc(cp));
  REQUIRE_TRUE(___csort_has_default_comparison_proc(vup));

  char arr[8] = "arr";
  const char carr[4] = "ca";
  REQUIRE_EQ((void *)csort_get_default_comparison_proc(arr), NULL);
  REQUIRE_EQ((void *)csort_get_default_comparison_proc(carr), NULL);
  REQUIRE_FALSE(___csort_has_default_comparison_proc(arr));
  REQUIRE_FALSE(___csort_has_default_comparison_proc(carr));
}

/* A default comparator may receive an address with neither the alignment
 * nor the effective type of its element, for example from a custom getter
 * over a packed byte buffer. Each one reads its operands with memcpy, so
 * every such call is defined; the UndefinedBehaviorSanitizer build of this
 * suite reports a misaligned typed read here otherwise. The checks test the
 * sign only, because the string comparator returns what strcmp returns. */
TEST(csort_unaligned, default_comparators_accept_unaligned_operands) {
  unsigned char buf[2 * sizeof(long double) + 2];
  unsigned char *a = buf + 1;
  unsigned char *b = a + sizeof(long double);

#define CSORT_UNALIGNED_CHECK(type, lo, hi)                               \
  do {                                                                    \
    type _lo = (lo);                                                      \
    type _hi = (hi);                                                      \
    ccol_comparison_proc_t _cmp = csort_get_default_comparison_proc(_lo); \
    REQUIRE_NE((void *)_cmp, NULL);                                       \
    memcpy(a, &_lo, sizeof(_lo));                                         \
    memcpy(b, &_hi, sizeof(_hi));                                         \
    REQUIRE_LT(_cmp(a, b), 0);                                            \
    REQUIRE_GT(_cmp(b, a), 0);                                            \
    REQUIRE_EQ(_cmp(a, a), 0);                                            \
  } while (0)

  CSORT_UNALIGNED_CHECK(short, -2, 7);
  CSORT_UNALIGNED_CHECK(int, INT_MIN, INT_MAX);
  CSORT_UNALIGNED_CHECK(long, LONG_MIN, 3L);
  CSORT_UNALIGNED_CHECK(long long, -1LL, LLONG_MAX);
  CSORT_UNALIGNED_CHECK(unsigned int, 0u, UINT_MAX);
  CSORT_UNALIGNED_CHECK(unsigned long long, 1ULL, ULLONG_MAX);
  CSORT_UNALIGNED_CHECK(float, -1.5f, 2.25f);
  CSORT_UNALIGNED_CHECK(double, -1e300, 1e300);
  CSORT_UNALIGNED_CHECK(long double, -1.0L, 1.0L);
  CSORT_UNALIGNED_CHECK(const char *, "alpha", "beta");
#undef CSORT_UNALIGNED_CHECK
}
