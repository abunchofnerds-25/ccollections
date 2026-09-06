#include <csort.h>
#include <cvector.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
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

  int i;

  srand(seed);
  cvec_declare(cvec, int);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

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

  int i;

  srand(seed);
  cvec_declare(cvec, float);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

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

  int i;

  srand(seed);
  cvec_declare(cvec, long double);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

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

  int i;

  cvec_declare(cvec, char *);
  cvec_init(cvec);

  REQUIRE_NE((void *)cvec, NULL);

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
  static const char charset[] =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  /* Use modulo so the index stays in [0, sizeof charset - 2], never reaching
   * the null terminator at charset[sizeof charset - 1]. */
  while (length-- > 0) {
    *dest++ = charset[rand() % (sizeof charset - 1)];
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
     * buffer. The previous floating-point formula could yield length == 100
     * when rand() == RAND_MAX, causing a one-byte stack overflow. */
    create_random_str(rand_strings[i], (size_t)(rand() % max_str_lenght));
    cvector_push_back(cvec, &(char *){rand_strings[i]});
  }

  REQUIRE_EQ(cvector_elem_count(cvec), num_sample);

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

// csort_get_default_comparison_proc(x) must return a real, non-NULL
// comparator for a `signed char` x, on par with every other integral type
// already covered above (including the plain, platform-signedness-dependent
// `char`), and that comparator must produce a genuinely signed ordering, not
// the raw-byte-pattern ordering a missing default (falling back to a NULL
// comparator, or (if such a gap were ever reintroduced) an unsigned
// comparator) would produce.
// csort_item_getter_proc_t requires exactly void *(*)(void *, size_t);
// cvector_at() itself takes a cvec (not a void *) as its first parameter, so
// casting cvector_at directly to csort_item_getter_proc_t and calling it
// through that cast pointer is undefined behavior (C11 6.3.2.3p8); this
// small adapter has the exact required signature instead, closing that gap
// the same way cvector.h's own internal _cvec_sort_getter does for
// cvec_sort()/cvector_sort_with_comparison_proc().
static void *csort_test_cvec_getter(void *collection, size_t index) {
  return cvector_at((cvec)collection, index);
}

TEST(csort, signed_char_default_comparator_is_not_null_and_sorts_signed) {
  ccol_comparison_proc_t cmp =
      csort_get_default_comparison_proc((signed char)0);
  REQUIRE_NE((void *)cmp, NULL);

  cvec_declare(vec, signed char);
  cvec_init(vec);
  cvec_push_rvalue(vec, (signed char)3);
  cvec_push_rvalue(vec, (signed char)-5);
  cvec_push_rvalue(vec, (signed char)100);
  cvec_push_rvalue(vec, (signed char)-100);
  cvec_push_rvalue(vec, (signed char)0);

  const signed char expected[] = {-100, -5, 0, 3, 100};
  const int n = 5;

  bool result = csort_sort(vec, (size_t)n, sizeof(signed char),
                           csort_test_cvec_getter, cmp, NULL);
  REQUIRE_TRUE(result);
  for (int i = 0; i < n; i++) {
    // Tau's REQUIRE_EQ printer has no `signed char` case (only plain `char`);
    // cast to int for the comparison, matching this suite's own established
    // pattern for types the vendored printer doesn't directly support.
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

// csort_sort must report a failed temporary-merge-buffer allocation via its
// return value, rather than silently no-op'ing with no way for the caller
// to tell the collection was left unsorted.
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

// ___csort_merge_sort's own documented contract: for a non-NULL collection
// whose length is 0 or 1, the trivial-success path returns true without ever
// inspecting getter_proc/comparison_proc; the length-0-or-1 check runs
// strictly before the NULL-proc check, so this combination must succeed
// silently (never assert) even when both procs are genuinely NULL. This is
// the one documented trivial-success combination the rest of this suite does
// not otherwise exercise: csort_sort_trivial_length_reports_success_even_
// under_oom above covers a trivial length with valid (non-NULL) procs, and
// csort_sort_null_collection_reports_success_without_touching_anything below
// covers a NULL collection with valid procs.
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

// ___csort_merge_sort's documented trivial-success contract explicitly
// covers a NULL collection ("true ... including the trivial col == NULL /
// length 0 or 1 cases, which have nothing to do"), not merely a length of
// 0 or 1 against a real, non-NULL collection (already covered by the
// trivial-length test just above). This must hold even for a length well
// above 1, and must never attempt any allocation or call getter_proc /
// comparison_proc, since there is no real backing memory behind a NULL
// collection for either of them to safely operate on.
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

// length * elem_size overflowing size_t must be rejected before ever
// attempting the temp-buffer allocation (which would otherwise receive a
// silently wrapped, undersized byte count while every merge pass still
// writes/reads full elem_size-sized elements at indices derived from the
// real, un-wrapped length; a heap buffer overflow) and before ever
// touching the collection through getter_proc/comparison_proc, neither of
// which is safe to call for a length this large with no genuine backing
// memory behind it.
TEST(csort, csort_sort_rejects_length_elem_size_overflow_without_allocating) {
  ccol_memmgmt_procs_t counting = {.malloc = _csort_overflow_test_malloc,
                                   .calloc = _csort_overflow_test_calloc,
                                   .realloc = _csort_overflow_test_realloc,
                                   .free = _csort_overflow_test_free};

  g_overflow_test_getter_called = false;
  g_overflow_test_cmp_called = false;
  g_overflow_test_alloc_called = false;

  // elem_size = 2, length = SIZE_MAX/2 + 1: length * elem_size would wrap
  // around size_t. Derived from SIZE_MAX/elem_size directly (the same
  // comparison the guard itself uses) rather than a hardcoded literal, so
  // this stays a genuine overflow at every pointer width, not just 64-bit.
  // Deliberately +1, not +2: for elem_size == 2, SIZE_MAX/elem_size + 1 is
  // exactly max_elem_count, which passes the separate, earlier
  // length > max_elem_count guard (checked before this one) while still
  // genuinely tripping this overflow guard. A +2 length is *also* rejected,
  // but by the max_elem_count guard instead, since it exceeds max_elem_count
  // too; which would leave this test unable to tell the two guards apart,
  // giving no regression coverage for this overflow guard specifically were
  // it ever removed or broken.
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
  // Mimics a conforming allocator that returns NULL for a zero-byte
  // request (malloc(0) is explicitly implementation-defined by the C
  // standard; glibc happens to return a non-NULL pointer, but nothing
  // requires that). If csort_sort() ever went back to actually issuing
  // this allocation for elem_size == 0, this would deterministically make
  // it misreport OOM regardless of which libc the suite happens to run
  // against.
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

// elem_size == 0 must report success without ever attempting the
// temp-buffer allocation (whose size would itself be 0 bytes, and whose
// result is implementation-defined, possibly NULL, for exactly that
// request) and without ever calling getter_proc/comparison_proc, since a
// zero-sized element has no bytes for either to read or for a merge pass to
// move. length is chosen well above the trivial 0/1 case specifically to
// prove this is elem_size's own dedicated short-circuit, not a side effect
// of the unrelated trivial-length path.
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

// csort_get_default_comparison_proc(x) must return NULL for a type this
// module documents as unsupported: neither a numeric/integral type nor a
// genuine char*/const char* pointer. bool and a plain struct value (as
// opposed to a struct field used through an explicit comparator, the
// documented, supported pattern) are both covered indirectly elsewhere in
// this codebase (cvector's own sort_unsupported_type_is_fatal_with_clear_
// diagnostic test, via cvec_sort's fatal_err path), but never directly
// against this macro's own return value within this module's own suite.
TEST(csort, get_default_comparison_proc_returns_null_for_unsupported_types) {
  bool dummy_bool = true;
  ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(dummy_bool);
  REQUIRE_EQ((void *)cmp, NULL);

  custom_test_struct dummy_struct = {.data = 42};
  cmp = csort_get_default_comparison_proc(dummy_struct);
  REQUIRE_EQ((void *)cmp, NULL);
}

// The index-arithmetic bugs (int -> size_t) only manifest for collections
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

// sort_negative_integers above only ever exercises int's default comparator
// against negative values; every OTHER signed integral type's coverage in
// this suite (define_integer_type_test's char/short/long/long_long
// instantiations) only ever pushes rand() % modulus_value, which is
// non-negative for every signed type it is instantiated with. All of these
// comparators are generated from the same shared macro
// (___csort__define_default_integral_comparison_proc), so a regression
// specific to one of the untested types is unlikely, but per this project's
// own standard for test rigor (see e.g. cbstmap/cjson's own dedicated
// plain-char-signedness coverage), an actual type-specific typo would still
// go undetected here without dedicated negative-value tests for each type
// short/long/long_long are unambiguously signed in C (unlike plain char,
// see sort_full_range_chars below), so a literal negative value set mirroring
// sort_negative_integers is portable across every platform this library
// targets.

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

// Plain char's signedness is platform-defined (signed on x86/x86_64,
// unsigned on aarch64's standard AAPCS64 ABI; see this project's own
// plain_char_negative_sign_extension precedent in cjson's test suite), so a
// literal negative value like the ones used above for short/long/long_long
// is not portable here: on an unsigned-char platform, assigning e.g. -100 to
// a char is well-defined (it wraps to 156) but is no longer testing what it
// looks like it is testing. Every char coverage this suite had before this
// test (the define_integer_type_test(char, char, 100) instantiation) only
// ever pushes rand() % 100, i.e. values in [0, 99]; representable
// identically as signed or unsigned char, so it cannot distinguish a
// signedness-ordering regression at all. This test is written to be
// meaningful on both kinds of platform instead: it spans char's own full
// representable range (CHAR_MIN..CHAR_MAX, whatever those numerically are
// here) via symbolic expressions rather than assumed-signed literals, and
// checks ascending order via plain <= on char values directly, which
// follows the platform's own comparison semantics automatically.
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

// The three floating-point sort tests above (cvector_float_sort/_double_sort/
// _long_double_sort) only ever generate non-negative values via
// rand()/RAND_MAX, unlike sort_negative_integers just above, which exercises
// the int comparator against negative values explicitly. The default
// comparators use the same (a > b) - (a < b) idiom regardless of type, but
// nothing in this suite previously verified that negative floating-point
// values sort correctly (as opposed to merely not crashing). Values below
// are exact binary fractions (sums of negative powers of two) so REQUIRE_EQ
// can check them without floating-point rounding flakiness.

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
    cvec_push_rvalue(v, 7);
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
  cvec_push_rvalue(v, 1);
  cvec_push_rvalue(v, 2);
  cvec_sort(v);
  REQUIRE_EQ(cvec_at(v, 0), 1);
  REQUIRE_EQ(cvec_at(v, 1), 2);
  cvec_destroy(v);
}

TEST(csort, sort_two_elements_descending) {
  cvec_construct(v, int);
  cvec_push_rvalue(v, 2);
  cvec_push_rvalue(v, 1);
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

// A length exceeding max_elem_count must be rejected before any allocation
// is attempted and before getter_proc/comparison_proc are ever called
// (exactly like the length * elem_size overflow guard just above), but this
// rejection must hold even when elem_size == 0, a case the overflow guard
// deliberately never rejects on its own (a zero elem_size can never
// overflow length * elem_size, so that guard alone leaves length completely
// unbounded). Without a dedicated cap here, this exact combination let the
// sort's own internal bottom-up doubling counter wrap around size_t's range
// and spin forever instead of ever finishing or reporting an error.
TEST(csort, csort_sort_rejects_length_exceeding_max_elem_count) {
  ccol_memmgmt_procs_t counting = {.malloc = _csort_overflow_test_malloc,
                                   .calloc = _csort_overflow_test_calloc,
                                   .realloc = _csort_overflow_test_realloc,
                                   .free = _csort_overflow_test_free};

  g_overflow_test_getter_called = false;
  g_overflow_test_cmp_called = false;
  g_overflow_test_alloc_called = false;

  int dummy_collection;  // address only; never dereferenced by this test
  bool result = csort_sort(&dummy_collection, max_elem_count + 1, 0,
                           _csort_overflow_test_getter,
                           _csort_overflow_test_cmp, &counting);

  REQUIRE_FALSE(result);
  REQUIRE_FALSE(g_overflow_test_getter_called);
  REQUIRE_FALSE(g_overflow_test_cmp_called);
  REQUIRE_FALSE(g_overflow_test_alloc_called);
}

// The test above only ever exercises the length > max_elem_count guard with
// elem_size == 0, a case where the guard is not actually the thing doing the
// work: elem_size == 0 has its own, entirely separate short-circuit (see
// csort_sort_zero_elem_size_reports_success_without_allocating) that returns
// success without ever reaching the doubling loop this guard exists to
// protect, regardless of whether this guard is present. For elem_size >= 2,
// the length * elem_size overflow guard immediately below this one already
// independently rejects any length anywhere near max_elem_count on its own
// (SIZE_MAX / elem_size is already below max_elem_count for every
// elem_size >= 2), so this guard's own rejection range for those elem_size
// values is empty too. elem_size == 1 is the ONE value for which neither of
// those is true: SIZE_MAX / 1 == SIZE_MAX, so the overflow guard rejects
// nothing on the basis of size at all for elem_size == 1, leaving this
// length > max_elem_count check as the sole thing standing between a length
// in (max_elem_count, SIZE_MAX] and the doubling loop's own curr_size *= 2
// overflow-to-zero infinite loop. Without a test exercising exactly this
// combination (nonzero elem_size, length > max_elem_count), a regression
// that removed or misordered this guard would very likely still report
// false for the elem_size == 0 case above (since that case never depended
// on this guard to begin with) and would very likely still report false for
// a nonzero-elem_size case too, purely because the real allocator/OS would
// fail an allocation this large anyway (virtual address space exhaustion,
// not the guard); masking the missing check rather than catching it. This
// test closes that gap the same way the elem_size == 0 test already proves
// its own guard is doing the rejecting: by asserting the allocator was never
// even invoked, not merely that the overall result was false.
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
  bool result = csort_sort(&dummy_collection, max_elem_count + 1, 1,
                           _csort_overflow_test_getter,
                           _csort_overflow_test_cmp, &counting);

  REQUIRE_FALSE(result);
  REQUIRE_FALSE(g_overflow_test_getter_called);
  REQUIRE_FALSE(g_overflow_test_cmp_called);
  REQUIRE_FALSE(g_overflow_test_alloc_called);
}

// ___csort_merge_sort is documented to assert (abort the process) if
// getter_proc is NULL, for a real, non-NULL collection with a length past
// the trivial 0/1 cases; run in a forked child since ccol_assert()/abort()
// terminates the whole process, mirroring this codebase's own established
// fatal-path test pattern (see e.g. tests/cvector/tests.c's
// sort_out_of_memory_is_fatal / sort_unsupported_type_is_fatal_with_clear_
// diagnostic tests, which exercise this exact assert only indirectly,
// through cvec_sort's own NULL-default-comparator path, never with a
// directly-NULL getter_proc as csort_sort itself allows).
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
  waitpid(pid, &status, 0);
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
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The elem_size == 0 short-circuit (csort_sort_zero_elem_size_reports_
// success_without_allocating above) lives inside ___csort_merge_sort's own
// helper, csort_mergesort_iterative; reached only AFTER the outer
// function's NULL-getter_proc/comparison_proc assert has already run. So an
// elem_size of 0 does NOT exempt a length-2-or-greater call from that
// assert, even though such a call would never actually need to invoke
// either proc. This is the documented, intentional behavior
// (___csort_merge_sort's own "Will assert ... unless col is NULL or length
// is 0 or 1" note already scopes the NULL-proc exemption to exclude
// elem_size == 0), but it was previously untested; pinned here as a
// forked-child fatal-path test, mirroring csort_sort_null_getter_proc_is_
// fatal/csort_sort_null_comparison_proc_is_fatal above, so a future
// reordering of these two checks cannot silently change this behavior in
// either direction without a test noticing.
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
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// csort_get_default_comparison_proc's own internal statement-expression
// result variable must never shadow a caller-supplied argument that happens
// to be an identifier literally named the same as that internal variable.
// Per C's declarator-scope rules, the internal variable's scope begins
// immediately after its own declarator, before the macro's parameter is
// ever expanded into the body; so if the internal variable and the
// caller's argument shared a name, every use of the parameter inside the
// expansion would silently resolve to the macro's own fresh, not-yet
// -assigned local instead of the caller's real variable, producing a
// silently wrong (NULL) result with no compiler diagnostic. This mirrors
// the exact class of bug already found and fixed for cvec_push's own
// internal locals in the cvector module.
TEST(csort,
     get_default_comparison_proc_not_shadowed_by_arg_named_comparison_proc) {
  int comparison_proc = 42;
  ccol_comparison_proc_t cmp =
      csort_get_default_comparison_proc(comparison_proc);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)csort_default_int_comparison_proc);

  char *comparison_proc_str = "hello";
  cmp = csort_get_default_comparison_proc(comparison_proc_str);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)csort_default_string_comparison_proc);
}

// csort_get_default_comparison_proc(x) must return NULL, not the string
// comparator, for a genuine fixed-size char array (char[N]) such as a
// `char name[64]` struct field. _Generic's controlling expression undergoes
// ordinary array-to-pointer decay (C11 6.5.1.1p2/6.3.2.1p3), so a bare
// is_char_ptr(x) check cannot tell "x really is a char* variable" apart
// from "x is an array that merely decayed to look like one for this one
// comparison"; reproduced directly against a minimal standalone _Generic
// snippet before this test was written: is_char_ptr(some_char_array)
// evaluates to true. Without an explicit !is_char_array(x) exclusion, this
// macro used to silently hand back csort_default_string_comparison_proc for
// an array argument; that comparator's contract requires its arguments to
// point at a stored char* VALUE (it performs one extra pointer indirection,
// *(const char **)first), not at the array's own inline byte content, so
// invoking it against real array data is undefined behavior (an arbitrary
// pointer built from the array's first sizeof(char*) content bytes,
// dereferenced), not merely a wrong sort order. NULL is the correct,
// already-documented result for any type this macro does not recognize.
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
  // correctly recognized as a string type and remain unaffected by the fix.
  char *p = name;
  cmp = csort_get_default_comparison_proc(p);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)csort_default_string_comparison_proc);
}

// The array-vs-pointer distinction above is not special-cased to plain
// `char[N]`: is_char_ptr()/is_char_array() (common.h) treat `signed char *`/
// `unsigned char *` as string-like pointers exactly like `char *`, so a
// `signed char[N]`/`unsigned char[N]` array must be excluded from
// csort_default_string_comparison_proc the same way, via the identical
// !is_char_array(__csort_gdcp_arr_probe) check. Verified correct by
// inspection when get_default_comparison_proc_returns_null_for_char_array
// above was added, but never directly exercised until now.
TEST(csort, get_default_comparison_proc_returns_null_for_signed_char_array) {
  signed char name[64] = {1, 2, 3, 0};
  ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(name);
  REQUIRE_EQ((void *)cmp, NULL);

  signed char *p = name;
  cmp = csort_get_default_comparison_proc(p);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)csort_default_string_comparison_proc);
}

TEST(csort, get_default_comparison_proc_returns_null_for_unsigned_char_array) {
  unsigned char name[64] = {1, 2, 3, 0};
  ccol_comparison_proc_t cmp = csort_get_default_comparison_proc(name);
  REQUIRE_EQ((void *)cmp, NULL);

  unsigned char *p = name;
  cmp = csort_get_default_comparison_proc(p);
  REQUIRE_NE((void *)cmp, NULL);
  REQUIRE_EQ((void *)cmp, (void *)csort_default_string_comparison_proc);
}

// csort_default_float_comparison_proc/_double_/_long_double_ must apply a
// genuine total order even when one or both operands are NaN: NaN compares
// greater than every non-NaN value, and equal only to another NaN. Without
// this, IEEE 754's native `<`/`>` (both false whenever either operand is
// NaN) would make the naive (a>b)-(a<b) idiom report a NaN as "equal" to
// everything, including two unrelated non-NaN values it happens to sit
// between; reproduced directly against the pre-fix library: csort_sort()
// on {9, NaN, 1, 4, NaN, 2, 7} (double) produced {1, 4, 9, NaN, NaN, 2, 7},
// whose non-NaN subsequence 1, 4, 9, 2, 7 is not sorted. This test pins the
// comparators' own return-value contract directly (all three floating-point
// types); sort_with_nan_does_not_corrupt_non_nan_order below exercises the
// same rule indirectly through a full cvec_sort().
TEST(csort, float_comparators_order_nan_as_greatest_and_equal_only_to_nan) {
  float f_nan = NAN, f_five = 5.0f, f_nan2 = NAN;
  REQUIRE_GT(csort_default_float_comparison_proc(&f_nan, &f_five), 0);
  REQUIRE_LT(csort_default_float_comparison_proc(&f_five, &f_nan), 0);
  REQUIRE_EQ(csort_default_float_comparison_proc(&f_nan, &f_nan2), 0);

  double d_nan = NAN, d_five = 5.0, d_nan2 = NAN;
  REQUIRE_GT(csort_default_double_comparison_proc(&d_nan, &d_five), 0);
  REQUIRE_LT(csort_default_double_comparison_proc(&d_five, &d_nan), 0);
  REQUIRE_EQ(csort_default_double_comparison_proc(&d_nan, &d_nan2), 0);

  long double ld_nan = NAN, ld_five = 5.0L, ld_nan2 = NAN;
  REQUIRE_GT(csort_default_long_double_comparison_proc(&ld_nan, &ld_five), 0);
  REQUIRE_LT(csort_default_long_double_comparison_proc(&ld_five, &ld_nan), 0);
  REQUIRE_EQ(csort_default_long_double_comparison_proc(&ld_nan, &ld_nan2), 0);

  // Finite values (no NaN involved) must still compare exactly as before.
  float f_three = 3.0f;
  REQUIRE_LT(csort_default_float_comparison_proc(&f_three, &f_five), 0);
  REQUIRE_GT(csort_default_float_comparison_proc(&f_five, &f_three), 0);
}

// The (v1 > v2) - (v1 < v2) finite-value path (taken whenever neither
// operand is NaN) must report -0.0 and 0.0 as equal, matching IEEE 754's own
// numeric equality rule (-0.0 == 0.0), not a raw bit-pattern comparison that
// would tell them apart. Mirrors cbstmap's own identical -0.0/0.0 check for
// its float/double/long double key comparator.
TEST(csort, float_comparators_treat_negative_zero_as_equal_to_positive_zero) {
  float f_neg_zero = -0.0f, f_pos_zero = 0.0f;
  REQUIRE_EQ(csort_default_float_comparison_proc(&f_neg_zero, &f_pos_zero), 0);
  REQUIRE_EQ(csort_default_float_comparison_proc(&f_pos_zero, &f_neg_zero), 0);

  double d_neg_zero = -0.0, d_pos_zero = 0.0;
  REQUIRE_EQ(csort_default_double_comparison_proc(&d_neg_zero, &d_pos_zero), 0);
  REQUIRE_EQ(csort_default_double_comparison_proc(&d_pos_zero, &d_neg_zero), 0);

  long double ld_neg_zero = -0.0L, ld_pos_zero = 0.0L;
  REQUIRE_EQ(
      csort_default_long_double_comparison_proc(&ld_neg_zero, &ld_pos_zero), 0);
  REQUIRE_EQ(
      csort_default_long_double_comparison_proc(&ld_pos_zero, &ld_neg_zero), 0);
}

// End-to-end regression for the same NaN-total-order rule, exercised through
// cvec_sort() (the primary, documented entry point most callers actually
// use) rather than the raw comparator functions directly. A NaN present
// anywhere in the vector must not corrupt the relative order of the other,
// non-NaN elements; the NaNs themselves are expected to land at the end
// (greatest), in no particular relative order among themselves.
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

// csort_sort() must route its temp-buffer allocation and free through a
// genuinely working, caller-supplied custom allocator for an ordinary,
// successful, non-trivial sort; not merely fail gracefully when the
// allocator fails (csort_sort_reports_oom_and_leaves_collection_untouched)
// or skip the allocator entirely for a rejected input
// (csort_sort_rejects_length_elem_size_overflow_without_allocating /
// csort_sort_rejects_length_exceeding_max_elem_count, both of which assert
// the allocator is NEVER called). None of those exercise the actual
// allocate-then-free round trip a real custom-allocator integration depends
// on, so a regression that silently fell back to the default heap for the
// temp buffer would have gone undetected. This checks both the call counts
// and that the freed pointer matches the one that was allocated.
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

  // Exactly one temp-buffer malloc, of exactly the expected size, and
  // exactly one matching free (i.e. the custom allocator's free was handed
  // back the same pointer its own malloc produced, not a default-heap
  // pointer or a leaked/duplicate one).
  REQUIRE_EQ(g_csort_custom_alloc_malloc_calls, 1);
  REQUIRE_EQ(g_csort_custom_alloc_free_calls, 1);
  REQUIRE_EQ(g_csort_custom_alloc_last_malloc_size, (size_t)n * sizeof(int));
  REQUIRE_EQ((void *)g_csort_custom_alloc_last_malloc_ptr, NULL);
}

// csort_sort() must reject a non-NULL mprocs that does not have all four of
// malloc/free/calloc/realloc populated; the same "all-or-nothing" contract
// ccol_memmgmt_procs_t documents and every other allocator-accepting entry
// point in this library (cvector_create_full, chmap_create_full, ...)
// already enforces via ccol_verify_memmgmt_procs() before ever touching such
// a struct. Reproduced directly against the pre-fix library before this test
// was written: a struct with .malloc/.calloc/.realloc populated but .free
// left NULL let the temp-buffer allocation succeed and then crashed calling
// through the NULL .free function pointer once the sort finished, instead of
// failing gracefully like every sibling module already does for the
// identical mistake (mirrors tests/cvector/tests.c's own
// create_with_invalid_mem_mgmt_procs test, one field at a time). A genuine,
// non-trivial length (well above the 0/1 trivial-success path, and non-zero
// elem_size) is used so this is the ONE guard actually being exercised, not
// an unrelated trivial-success or overflow-guard short-circuit.
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

// The trivial-success paths (col == NULL / length 0 or 1 / elem_size == 0)
// never touch mprocs at all (see ___csort_merge_sort's own doc comment),
// so an incomplete mprocs must not turn any of them into a failure; the
// mprocs validation guard only fires immediately before the temp buffer it
// guards would actually be allocated.
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