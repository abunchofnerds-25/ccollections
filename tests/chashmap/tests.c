#include <chashmap.h>
#include <common_invariants.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()  // sets up Tau (+ main function)

// HASH_MAP TESTS

extern size_t find_nearest_gte_power_of_two(size_t input);

TEST(chash_maps, find_nearest_gte_power_of_two) {
  // Hand-crafted the following input and expected output arrays, as we don't
  // want to depend on a math lib to do the testing.
  //
  // find_nearest_gte_power_of_two's own doc comment (src/common.c) states
  // its result is architecture-dependent, capped at the pointer-size
  // maximum, returning ccol_invalid_size once the input exceeds the largest
  // power of two representable in size_t. The 64-bit table below tests all
  // the way up to SIZE_MAX on a 64-bit size_t; several of its literal
  // values (e.g. 4294967296 and beyond) are not representable in a 32-bit
  // size_t at all, so a 32-bit build needs its own, shorter table that stops
  // at the 32-bit SIZE_MAX instead, with the boundary-overflow case moved
  // down to 2^31 accordingly.
#if SIZE_MAX == UINT64_MAX
  size_t test_inputs[] = {0,
                          1,
                          2,
                          3,
                          4,
                          5,
                          7,
                          8,
                          9,
                          15,
                          16,
                          17,
                          31,
                          32,
                          33,
                          63,
                          64,
                          65,
                          127,
                          128,
                          129,
                          255,
                          256,
                          257,
                          511,
                          512,
                          513,
                          1023,
                          1024,
                          1025,
                          2047,
                          2048,
                          2049,
                          4095,
                          4096,
                          4097,
                          8191,
                          8192,
                          8193,
                          16383,
                          16384,
                          16385,
                          32767,
                          32768,
                          32769,
                          65535,
                          65536,
                          65537,
                          131071,
                          131072,
                          131073,
                          262143,
                          262144,
                          262145,
                          524287,
                          524288,
                          524289,
                          1048575,
                          1048576,
                          1048577,
                          2097151,
                          2097152,
                          2097153,
                          4194303,
                          4194304,
                          4194305,
                          8388607,
                          8388608,
                          8388609,
                          16777215,
                          16777216,
                          16777217,
                          33554431,
                          33554432,
                          33554433,
                          67108863,
                          67108864,
                          67108865,
                          134217727,
                          134217728,
                          134217729,
                          268435455,
                          268435456,
                          268435457,
                          536870911,
                          536870912,
                          536870913,
                          1073741823,
                          1073741824,
                          1073741825,
                          2147483647,
                          2147483648,
                          2147483649,
                          4294967295,
                          4294967296,
                          4294967297,
                          4611686018427387903,
                          4611686018427387904,
                          4611686018427387905,
                          9223372036854775807UL,
                          9223372036854775808UL,
                          9223372036854775809UL,
                          18446744073709551615UL};

  size_t expected_outputs[] = {1,
                               1,
                               2,
                               4,
                               4,
                               8,
                               8,
                               8,
                               16,
                               16,
                               16,
                               32,
                               32,
                               32,
                               64,
                               64,
                               64,
                               128,
                               128,
                               128,
                               256,
                               256,
                               256,
                               512,
                               512,
                               512,
                               1024,
                               1024,
                               1024,
                               2048,
                               2048,
                               2048,
                               4096,
                               4096,
                               4096,
                               8192,
                               8192,
                               8192,
                               16384,
                               16384,
                               16384,
                               32768,
                               32768,
                               32768,
                               65536,
                               65536,
                               65536,
                               131072,
                               131072,
                               131072,
                               262144,
                               262144,
                               262144,
                               524288,
                               524288,
                               524288,
                               1048576,
                               1048576,
                               1048576,
                               2097152,
                               2097152,
                               2097152,
                               4194304,
                               4194304,
                               4194304,
                               8388608,
                               8388608,
                               8388608,
                               16777216,
                               16777216,
                               16777216,
                               33554432,
                               33554432,
                               33554432,
                               67108864,
                               67108864,
                               67108864,
                               134217728,
                               134217728,
                               134217728,
                               268435456,
                               268435456,
                               268435456,
                               536870912,
                               536870912,
                               536870912,
                               1073741824,
                               1073741824,
                               1073741824,
                               2147483648,
                               2147483648,
                               2147483648,
                               4294967296,
                               4294967296,
                               4294967296,
                               8589934592,
                               4611686018427387904,
                               4611686018427387904,
                               9223372036854775808UL,
                               9223372036854775808UL,
                               9223372036854775808UL,
                               ccol_invalid_size,
                               ccol_invalid_size};
#else
  size_t test_inputs[] = {
      0,          1,           2,           3,          4,          5,
      7,          8,           9,           15,         16,         17,
      31,         32,          33,          63,         64,         65,
      127,        128,         129,         255,        256,        257,
      511,        512,         513,         1023,       1024,       1025,
      2047,       2048,        2049,        4095,       4096,       4097,
      8191,       8192,        8193,        16383,      16384,      16385,
      32767,      32768,       32769,       65535,      65536,      65537,
      131071,     131072,      131073,      262143,     262144,     262145,
      524287,     524288,      524289,      1048575,    1048576,    1048577,
      2097151,    2097152,     2097153,     4194303,    4194304,    4194305,
      8388607,    8388608,     8388609,     16777215,   16777216,   16777217,
      33554431,   33554432,    33554433,    67108863,   67108864,   67108865,
      134217727,  134217728,   134217729,   268435455,  268435456,  268435457,
      536870911,  536870912,   536870913,   1073741823, 1073741824, 1073741825,
      2147483647, 2147483648U, 2147483649U, 4294967295U};

  size_t expected_outputs[] = {1,
                               1,
                               2,
                               4,
                               4,
                               8,
                               8,
                               8,
                               16,
                               16,
                               16,
                               32,
                               32,
                               32,
                               64,
                               64,
                               64,
                               128,
                               128,
                               128,
                               256,
                               256,
                               256,
                               512,
                               512,
                               512,
                               1024,
                               1024,
                               1024,
                               2048,
                               2048,
                               2048,
                               4096,
                               4096,
                               4096,
                               8192,
                               8192,
                               8192,
                               16384,
                               16384,
                               16384,
                               32768,
                               32768,
                               32768,
                               65536,
                               65536,
                               65536,
                               131072,
                               131072,
                               131072,
                               262144,
                               262144,
                               262144,
                               524288,
                               524288,
                               524288,
                               1048576,
                               1048576,
                               1048576,
                               2097152,
                               2097152,
                               2097152,
                               4194304,
                               4194304,
                               4194304,
                               8388608,
                               8388608,
                               8388608,
                               16777216,
                               16777216,
                               16777216,
                               33554432,
                               33554432,
                               33554432,
                               67108864,
                               67108864,
                               67108864,
                               134217728,
                               134217728,
                               134217728,
                               268435456,
                               268435456,
                               268435456,
                               536870912,
                               536870912,
                               536870912,
                               1073741824,
                               1073741824,
                               1073741824,
                               2147483648U,
                               2147483648U,
                               2147483648U,
                               ccol_invalid_size,
                               ccol_invalid_size};

#endif

  int len = sizeof(test_inputs) / sizeof(size_t);
  REQUIRE_EQ(len, sizeof(expected_outputs) / sizeof(size_t));

  for (int i = 0; i < len; ++i) {
    size_t r = find_nearest_gte_power_of_two(test_inputs[i]);
    REQUIRE_EQ(r, expected_outputs[i]);
  }
}

TEST(chash_maps, create_fails) {
  char *err = NULL;
  chashmap *chmap = chmap_create(0, ccol_char, ccol_char, &err);
  REQUIRE_EQ((void *)chmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  chmap = chmap_create_mp(
      4096, ccol_char, ccol_char,
      &(ccol_memmgmt_procs_t){
          .malloc = NULL, .free = free, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)chmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  chmap = chmap_create_mp(
      4096, ccol_char, ccol_char,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = NULL, .calloc = calloc, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)chmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  chmap = chmap_create_mp(
      4096, ccol_char, ccol_char,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = NULL, .realloc = realloc},
      &err);
  REQUIRE_EQ((void *)chmap, NULL);
  REQUIRE_NE((void *)err, NULL);

  chmap = chmap_create_mp(
      4096, ccol_char, ccol_char,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = NULL},
      &err);
  REQUIRE_EQ((void *)chmap, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(chash_maps, create_succeeds) {
  char *err = "";
  chashmap *chmap = chmap_create(4096, ccol_char, ccol_char, &err);
  REQUIRE_NE((void *)chmap, NULL);
  REQUIRE_EQ((void *)err, NULL);

  chmap_destroy(chmap);
  REQUIRE_EQ((void *)chmap, NULL);

  chmap = chmap_create_mp(
      4096, ccol_char, ccol_char,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      &err);

  chmap_destroy(chmap);
  REQUIRE_EQ((void *)chmap, NULL);
}

// Helper functions start.
int insert_string_to_int(chashmap *chmap, const char *key, int val) {
  return chmap_insert_elem(
      chmap, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
      &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)});
}

int get_int_from_string(chashmap *chmap, const char *key, int *val_ptr) {
  return chmap_get_elem_copy(
      chmap, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, val_ptr,
      sizeof(int));
}

int get_int_ref_from_string(chashmap *chmap, const char *key, int **val_ptr) {
  cmap_pair *tmp_val_pair_ptr = NULL;
  if (chmap_get_elem_ref(chmap,
                         &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                         &tmp_val_pair_ptr) == 0) {
    *val_ptr = (int *)tmp_val_pair_ptr->ptr;
    return 0;
  }
  return -1;
}

ccol_retval_t delete_int_from_string(chashmap *chmap, const char *key) {
  return chmap_delete_elem(
      chmap, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)});
}
// Helper functions end.

TEST(chash_maps, basic_insertions_and_lookups) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  int val = -1;

  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_key_not_found);
  REQUIRE_EQ(get_int_from_string(chmap, "", &val), ccol_invalid_args);

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 2), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);
  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 3), ccol_key_already_present);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);

  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 4), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);
  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 5), ccol_key_already_present);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);

  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_success);
  REQUIRE_EQ(val, 3);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &val), ccol_success);
  REQUIRE_EQ(val, 5);

  chmap_destroy(chmap);
}

TEST(chash_maps, basic_insertions_and_lookups_with_memmgmt_procs) {
  chashmap *chmap = chmap_create_mp(
      1, ccol_string, ccol_int,
      &(ccol_memmgmt_procs_t){
          .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc},
      NULL);
  REQUIRE_NE((void *)chmap, NULL);

  int val = -1;

  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_key_not_found);
  REQUIRE_EQ(get_int_from_string(chmap, "", &val), ccol_invalid_args);

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 2), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);
  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 3), ccol_key_already_present);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);

  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 4), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);
  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 5), ccol_key_already_present);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);

  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_success);
  REQUIRE_EQ(val, 3);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &val), ccol_success);
  REQUIRE_EQ(val, 5);

  chmap_destroy(chmap);
}

TEST(chash_maps, insert_values_with_different_sizes) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_other_types, NULL);

  const char *key1 = "key";
  cmap_pair *target_pair = NULL;

  {
    struct s1 {
      unsigned int m1;
      unsigned int m2;
    } val, test_val;
    val.m1 = 3;
    val.m2 = 5;

    REQUIRE_EQ(
        chmap_insert_elem(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)}),
        ccol_success);

    REQUIRE_EQ(
        chmap_get_elem_ref(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &target_pair),
        ccol_success);

    REQUIRE_EQ(memcmp(target_pair->ptr, &val, sizeof(val)), 0);

    REQUIRE_EQ(
        chmap_get_elem_copy(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &test_val, sizeof(test_val)),
        ccol_success);

    REQUIRE_EQ(memcmp(&test_val, &val, sizeof(val)), 0);
  }

  {
    struct s2 {
      unsigned long m1;
      unsigned long m2;
      unsigned long m3;
      unsigned long m4;
    } val, test_val;
    val.m1 = 7;
    val.m2 = 9;
    val.m3 = 11;
    val.m4 = 13;

    REQUIRE_EQ(
        chmap_insert_elem(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)}),
        ccol_key_already_present);

    REQUIRE_EQ(
        chmap_get_elem_ref(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &target_pair),
        ccol_success);

    REQUIRE_EQ(memcmp(target_pair->ptr, &val, sizeof(val)), 0);

    REQUIRE_EQ(
        chmap_get_elem_copy(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &test_val, sizeof(test_val)),
        ccol_success);

    REQUIRE_EQ(memcmp(&test_val, &val, sizeof(val)), 0);
  }

  {
    unsigned long val = 0xabcdef01, test_val = 0xffffffff;

    REQUIRE_EQ(
        chmap_insert_elem(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)}),
        ccol_key_already_present);

    REQUIRE_EQ(
        chmap_get_elem_ref(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &target_pair),
        ccol_success);

    REQUIRE_EQ(memcmp(target_pair->ptr, &val, sizeof(val)), 0);

    REQUIRE_EQ(
        chmap_get_elem_copy(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &test_val, sizeof(test_val)),
        ccol_success);

    REQUIRE_EQ(memcmp(&test_val, &val, sizeof(val)), 0);
  }

  {
    unsigned char val = 0xab, test_val = 0xcd;

    REQUIRE_EQ(
        chmap_insert_elem(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)}),
        ccol_key_already_present);

    REQUIRE_EQ(
        chmap_get_elem_ref(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &target_pair),
        ccol_success);

    REQUIRE_EQ(memcmp(target_pair->ptr, &val, sizeof(val)), 0);

    REQUIRE_EQ(
        chmap_get_elem_copy(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &test_val, sizeof(test_val)),
        ccol_success);

    REQUIRE_EQ(memcmp(&test_val, &val, sizeof(val)), 0);
  }

  {
    unsigned short val = 0xabcd, test_val = 0xef01;

    REQUIRE_EQ(
        chmap_insert_elem(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &(cmap_pair){.ptr = (void *)&val, .size = sizeof(val)}),
        ccol_key_already_present);

    REQUIRE_EQ(
        chmap_get_elem_ref(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &target_pair),
        ccol_success);

    REQUIRE_EQ(memcmp(target_pair->ptr, &val, sizeof(val)), 0);

    REQUIRE_EQ(
        chmap_get_elem_copy(
            chmap, &(cmap_pair){.ptr = (void *)key1, .size = strlen(key1)},
            &test_val, sizeof(test_val)),
        ccol_success);

    REQUIRE_EQ(memcmp(&test_val, &val, sizeof(val)), 0);
  }

  chmap_destroy(chmap);
}

TEST(chash_maps, insertions_with_a_variety_of_key_sizes) {
  chashmap *chmap = chmap_create(1, ccol_other_types, ccol_other_types, NULL);

  int val;
  int target = 0;
  char ch = 'A';

  val = 1;
  REQUIRE_EQ(
      chmap_insert_elem(chmap, &(cmap_pair){.ptr = &ch, .size = sizeof(ch)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);
  REQUIRE_EQ(
      chmap_get_elem_copy(chmap, &(cmap_pair){.ptr = &ch, .size = sizeof(ch)},
                          &target, sizeof(int)),
      ccol_success);
  REQUIRE_EQ(val, target);

  short sh = 32767;
  val = 2;
  REQUIRE_EQ(
      chmap_insert_elem(chmap, &(cmap_pair){.ptr = &sh, .size = sizeof(sh)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);
  REQUIRE_EQ(
      chmap_get_elem_copy(chmap, &(cmap_pair){.ptr = &sh, .size = sizeof(sh)},
                          &target, sizeof(int)),
      ccol_success);
  REQUIRE_EQ(val, target);

  int id = 0xabcdef01;
  val = 3;
  REQUIRE_EQ(
      chmap_insert_elem(chmap, &(cmap_pair){.ptr = &id, .size = sizeof(id)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);
  REQUIRE_EQ(
      chmap_get_elem_copy(chmap, &(cmap_pair){.ptr = &id, .size = sizeof(id)},
                          &target, sizeof(int)),
      ccol_success);
  REQUIRE_EQ(val, target);

  // long is only guaranteed >= 32 bits by the C standard (4 bytes on
  // ILP32 platforms such as i386, vs. 8 on LP64 x86_64); this value must
  // fit in the smaller of those two widths to be portable.
  long l = 0x6EEDDBB0L;
  val = 4;
  REQUIRE_EQ(
      chmap_insert_elem(chmap, &(cmap_pair){.ptr = &l, .size = sizeof(l)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);
  REQUIRE_EQ(
      chmap_get_elem_copy(chmap, &(cmap_pair){.ptr = &l, .size = sizeof(l)},
                          &target, sizeof(int)),
      ccol_success);
  REQUIRE_EQ(val, target);

  char *a_string_key = "a string key for testing";
  for (uint32_t i = 1; i <= 24; ++i) {
    char key[32] = {0};
    snprintf(key, sizeof(key), "%.*s", i, a_string_key);
    val = i + 5;

    REQUIRE_EQ(
        chmap_insert_elem(chmap, &(cmap_pair){.ptr = key, .size = strlen(key)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
    REQUIRE_EQ(chmap_get_elem_copy(
                   chmap, &(cmap_pair){.ptr = key, .size = strlen(key)},
                   &target, sizeof(int)),
               ccol_success);
    REQUIRE_EQ(val, target);
  }

  chmap_destroy(chmap);
}

/* ========================================================================== */
/*                    chmap_destroy_with_dtor                                 */
/* ========================================================================== */

typedef struct {
  int call_count;
  int sum_of_values;
} destroy_dtor_ctx_t;

/* Mirrors cyaml.c's own actual usage shape: values are heap-allocated
 * pointers (here to a plain int) that the destructor must free, exactly
 * like __cyaml_destroy would free a child node. */
static void free_int_ptr_dtor(cmap_pair *val_pair, void *ctx) {
  destroy_dtor_ctx_t *c = (destroy_dtor_ctx_t *)ctx;
  int *p;
  memcpy(&p, val_pair->ptr, sizeof(p));
  c->call_count++;
  c->sum_of_values += *p;
  free(p);
}

/* For open-addressing maps, values are stored inline (not as a pointer to
 * free), so a plain counter/summer is used instead of free_int_ptr_dtor. */
static void count_int_value_dtor(cmap_pair *val_pair, void *ctx) {
  destroy_dtor_ctx_t *c = (destroy_dtor_ctx_t *)ctx;
  int v;
  memcpy(&v, val_pair->ptr, sizeof(v));
  c->call_count++;
  c->sum_of_values += v;
}

TEST(chash_maps, destroy_with_dtor_invokes_once_per_value_sc) {
  /* Separate chaining (string keys): mirrors cyaml.c's own char* ->
   * pointer dictionaries exactly. */
  char *err = NULL;
  chmap m = chmap_create(1, ccol_string, ccol_pointer, &err);
  REQUIRE_NE((void *)m, NULL);

  const char *keys[] = {"a", "b", "c", "d", "e"};
  int expected_sum = 0;
  for (int i = 0; i < 5; i++) {
    int *val = malloc(sizeof(int));
    REQUIRE_NE((void *)val, NULL);
    *val = i + 1;
    expected_sum += *val;
    REQUIRE_EQ(
        chmap_insert_elem(
            m, &(cmap_pair){.ptr = (void *)keys[i], .size = strlen(keys[i])},
            &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }

  destroy_dtor_ctx_t ctx = {0};
  chmap_destroy_with_dtor(m, free_int_ptr_dtor, &ctx);
  REQUIRE_EQ(ctx.call_count, 5);
  REQUIRE_EQ(ctx.sum_of_values, expected_sum);
}

TEST(chash_maps, destroy_with_dtor_invokes_once_per_value_oa) {
  /* Open-addressing (both key and value integral): the other backend,
   * exercised for completeness even though cyaml.c itself never reaches
   * this path (its own dictionary keys are always char*). */
  char *err = NULL;
  chmap m = chmap_create(1, ccol_int, ccol_int, &err);
  REQUIRE_NE((void *)m, NULL);

  int expected_sum = 0;
  for (int i = 0; i < 10; i++) {
    int key = i;
    int val = i * 10;
    expected_sum += val;
    REQUIRE_EQ(
        chmap_insert_elem(m, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }

  destroy_dtor_ctx_t ctx = {0};
  chmap_destroy_with_dtor(m, count_int_value_dtor, &ctx);
  REQUIRE_EQ(ctx.call_count, 10);
  REQUIRE_EQ(ctx.sum_of_values, expected_sum);
}

/* destroy_with_dtor_invokes_once_per_value_oa above never deletes anything,
 * so it can never exercise chmap_destroy_with_dtor's SLOT_DELETED skip on
 * the open-addressing backend: every slot it walks is either genuinely
 * empty or live, never a tombstone. This test deletes half the keys first
 * so tombstoned slots are actually present in the slot array by the time
 * chmap_destroy_with_dtor walks it, and confirms the destructor fires only
 * for the survivors, not once per every occupied-or-tombstoned slot. */
TEST(chash_maps, destroy_with_dtor_skips_oa_tombstones) {
  char *err = NULL;
  chmap m = chmap_create(1, ccol_int, ccol_int, &err);
  REQUIRE_NE((void *)m, NULL);

  for (int i = 0; i < 10; i++) {
    int key = i;
    int val = i * 10;
    REQUIRE_EQ(
        chmap_insert_elem(m, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }

  /* Delete the even keys: their slots become tombstones (SLOT_OCCUPIED |
   * SLOT_DELETED), leaving only the odd keys' values live. */
  for (int i = 0; i < 10; i += 2) {
    int key = i;
    REQUIRE_EQ(
        chmap_delete_elem(m, &(cmap_pair){.ptr = &key, .size = sizeof(key)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(m), (size_t)5);

  int expected_sum = 0;
  for (int i = 1; i < 10; i += 2) {
    expected_sum += i * 10;
  }

  destroy_dtor_ctx_t ctx = {0};
  chmap_destroy_with_dtor(m, count_int_value_dtor, &ctx);
  REQUIRE_EQ(ctx.call_count, 5);
  REQUIRE_EQ(ctx.sum_of_values, expected_sum);
}

TEST(chash_maps, destroy_with_dtor_empty_map_no_calls) {
  char *err = NULL;
  chmap m = chmap_create(1, ccol_string, ccol_int, &err);
  REQUIRE_NE((void *)m, NULL);

  destroy_dtor_ctx_t ctx = {0};
  chmap_destroy_with_dtor(m, count_int_value_dtor, &ctx);
  REQUIRE_EQ(ctx.call_count, 0);
}

TEST(chash_maps, destroy_with_dtor_null_val_dtor_behaves_like_plain_destroy) {
  char *err = NULL;
  chmap m = chmap_create(1, ccol_string, ccol_int, &err);
  REQUIRE_NE((void *)m, NULL);
  int val = 42;
  REQUIRE_EQ(chmap_insert_elem(m, &(cmap_pair){.ptr = "k", .size = 1},
                               &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
             ccol_success);
  /* Must not crash and must actually free the map; verified by valgrind
   * under make memtest, not by any direct assertion here. */
  chmap_destroy_with_dtor(m, NULL, NULL);
}

TEST(chash_maps, destroy_with_dtor_null_map_is_noop) {
  /* Must not crash regardless of val_dtor. */
  destroy_dtor_ctx_t ctx = {0};
  chmap_destroy_with_dtor(NULL, count_int_value_dtor, &ctx);
  REQUIRE_EQ(ctx.call_count, 0);
}

TEST(chash_maps, basic_deletions) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  int val = -1;

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(delete_int_from_string(chmap, "key1"),
             ccol_key_not_found);  // Should not have any side effects
  REQUIRE_EQ(delete_int_from_string(chmap, ""),
             ccol_invalid_args);  // Should not have any side effects

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 3), ccol_success);
  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 5), ccol_success);

  REQUIRE_EQ(chmap_elem_count(chmap), 2);

  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_success);
  REQUIRE_EQ(val, 3);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &val), ccol_success);
  REQUIRE_EQ(val, 5);

  REQUIRE_EQ(delete_int_from_string(chmap, "key1"), ccol_success);
  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_key_not_found);

  REQUIRE_EQ(chmap_elem_count(chmap), 1);

  REQUIRE_EQ(delete_int_from_string(chmap, "key2"), ccol_success);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &val), ccol_key_not_found);

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(delete_int_from_string(chmap, "key1"),
             ccol_key_not_found);  // Should not have any side effects
  REQUIRE_EQ(delete_int_from_string(chmap, ""),
             ccol_invalid_args);  // Should not have any side effects

  chmap_destroy(chmap);
}

TEST(chash_maps, accessing_references) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  int *val_ptr = NULL;

  REQUIRE_EQ(get_int_ref_from_string(chmap, "key1", &val_ptr), -1);
  REQUIRE_EQ((void *)val_ptr, NULL);
  REQUIRE_EQ(get_int_ref_from_string(chmap, "", &val_ptr), -1);
  REQUIRE_EQ((void *)val_ptr, NULL);

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", -3), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);
  REQUIRE_EQ(insert_string_to_int(chmap, "key2", -5), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);

  int tmp;

  REQUIRE_EQ(get_int_ref_from_string(chmap, "key1", &val_ptr), ccol_success);
  REQUIRE_NE((void *)val_ptr, NULL);
  REQUIRE_EQ(*val_ptr, -3);
  *val_ptr = 1;  // The changes made via pointer alters the map element directly
  REQUIRE_EQ(get_int_from_string(chmap, "key1", &tmp), ccol_success);
  REQUIRE_EQ(tmp, 1);

  REQUIRE_EQ(get_int_ref_from_string(chmap, "key2", &val_ptr), ccol_success);
  REQUIRE_NE((void *)val_ptr, NULL);
  REQUIRE_EQ(*val_ptr, -5);
  *val_ptr = 7;  // The changes made via pointer alters the map element directly
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &tmp), ccol_success);
  REQUIRE_EQ(tmp, 7);

  chmap_destroy(chmap);
}

TEST(chash_maps, reset) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 3), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);

  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 5), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);

  REQUIRE_EQ(chmap_reset(chmap, 2), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 4), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);

  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 6), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);

  int tmp;
  REQUIRE_EQ(get_int_from_string(chmap, "key1", &tmp), ccol_success);
  REQUIRE_EQ(tmp, 4);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &tmp), ccol_success);
  REQUIRE_EQ(tmp, 6);

  // Reset with a different bucket size
  REQUIRE_EQ(chmap_reset(chmap, 8192), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 4), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 1);

  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 6), ccol_success);
  REQUIRE_EQ(chmap_elem_count(chmap), 2);

  REQUIRE_EQ(get_int_from_string(chmap, "key1", &tmp), ccol_success);
  REQUIRE_EQ(tmp, 4);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &tmp), ccol_success);
  REQUIRE_EQ(tmp, 6);

  chmap_destroy(chmap);
}

TEST(chash_maps, for_each_elem_wr) {
  // chashmap* chmap = chmap_create(1, NULL);
  chmap_construct(chmap, char *, int);
  REQUIRE_NE((void *)chmap, NULL);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 3), ccol_success);
  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 5), ccol_success);

  ccol_iter_declare(chmap, it);
  for (it = ccol_begin(chmap); it != NULL; it = ccol_iter_next(it)) {
    // *(int*)(it->val_pair->ptr) += 7;
    *ccol_iter_val_ptr(it) += 7;
  }

  int val = -1;
  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_success);
  REQUIRE_EQ(val, 10);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &val), ccol_success);
  REQUIRE_EQ(val, 12);

  chmap_destroy(chmap);
}

TEST(chash_maps, for_each_elem_rd) {
  chmap_construct(chmap, char *, int);
  REQUIRE_NE((void *)chmap, NULL);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 3), ccol_success);
  REQUIRE_EQ(insert_string_to_int(chmap, "key2", 5), ccol_success);

  int sum = 0;

  // Add 3 and 5 on top of sum. It should be 8
  // when the following statement is completed.
  ccol_iter_declare(chmap, it);
  for (it = ccol_begin(chmap); it != NULL; it = ccol_iter_next(it)) {
    sum += *ccol_iter_val_ptr(it);
  }

  REQUIRE_EQ(sum, 8);

  int val = -1;
  REQUIRE_EQ(get_int_from_string(chmap, "key1", &val), ccol_success);
  REQUIRE_EQ(val, 3);
  REQUIRE_EQ(get_int_from_string(chmap, "key2", &val), ccol_success);
  REQUIRE_EQ(val, 5);

  chmap_destroy(chmap);
}

extern size_t chmap_get_bucket_arr_size(chashmap *chmap);
extern size_t chmap_get_elem_count_to_scale_up(chashmap *chmap);
extern size_t chmap_get_elem_count_to_scale_down(chashmap *chmap);

TEST(chash_maps, scaling) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  size_t first_up_threshold = chmap_get_elem_count_to_scale_up(chmap);
  size_t first_down_threshold = chmap_get_elem_count_to_scale_down(chmap);
  size_t first_capacity = chmap_get_bucket_arr_size(chmap);

  char key_buf[16] = {0};
  for (size_t i = 0; i <= first_up_threshold; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%zu", i);
    REQUIRE_EQ(insert_string_to_int(chmap, key_buf, i + 1), ccol_success);
  }

  REQUIRE_EQ(chmap_elem_count(chmap), first_up_threshold + 1);

  REQUIRE_TRUE(first_up_threshold < chmap_get_elem_count_to_scale_up(chmap));
  REQUIRE_TRUE(first_down_threshold <
               chmap_get_elem_count_to_scale_down(chmap));
  REQUIRE_TRUE(first_capacity < chmap_get_bucket_arr_size(chmap));

  for (size_t i = 0; i <= first_up_threshold; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%zu", i);
    delete_int_from_string(chmap, key_buf);
  }

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(first_up_threshold, chmap_get_elem_count_to_scale_up(chmap));
  REQUIRE_EQ(first_down_threshold, chmap_get_elem_count_to_scale_down(chmap));
  REQUIRE_EQ(first_capacity, chmap_get_bucket_arr_size(chmap));

  chmap_destroy(chmap);
}

// Regression: oa_insert used to check the map's load factor (and possibly
// rehash) BEFORE ever checking whether the key already existed, so a pure
// value update for an already-present key could trigger a full-table
// rehash purely because the map happened to already sit above its growth
// threshold from unrelated prior inserts, invalidating every other key's
// already-held chmap_get_elem_ref pointer as an unwanted side effect. An
// update of an existing key must never grow the table: growth is only
// ever warranted by genuinely adding a new element, and elem_count does
// not change on an update.
TEST(chash_maps, oa_update_of_existing_key_never_triggers_rehash) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  size_t capacity_before_any_insert = chmap_get_bucket_arr_size(hm);
  REQUIRE_EQ(capacity_before_any_insert, (size_t)16);

  // Insert 12 distinct keys. 16 * 0.70 == 11.2, so the 12th distinct insert
  // (evaluated against a pre-insert count of 11) does not itself cross the
  // growth threshold; the table is left at its original capacity with
  // count == 12, i.e. a load factor of 0.75, already OVER the threshold,
  // without ever having grown.
  for (int i = 0; i < 12; ++i) {
    int val = i * 100;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)12);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  // Hold a reference into a DIFFERENT key (key 0) across the update below.
  int key0 = 0;
  cmap_pair *key0_val_pair = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(hm, &(cmap_pair){.ptr = &key0, .size = sizeof(key0)},
                         &key0_val_pair),
      ccol_success);
  REQUIRE_NE((void *)key0_val_pair, NULL);
  void *key0_val_addr_before = key0_val_pair->ptr;
  int key0_val_before = *(int *)key0_val_pair->ptr;
  REQUIRE_EQ(key0_val_before, 0);

  // Now update an EXISTING key's value (key 5) while the map sits above its
  // growth threshold. This must be recognized as an update, not a new
  // insertion, and must therefore never rehash.
  int key5 = 5;
  int new_val5 = 999;
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = &key5, .size = sizeof(key5)},
                 &(cmap_pair){.ptr = &new_val5, .size = sizeof(new_val5)}),
             ccol_key_already_present);

  // Element count is unchanged (an update, not a new element) and, crucially,
  // the table must not have grown.
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)12);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  // The previously-held reference to key 0's value must still be valid AND
  // must still point at the exact same address: had a rehash occurred, the
  // whole val_accessors array (and therefore this pointer) would have been
  // reallocated elsewhere.
  REQUIRE_EQ((void *)key0_val_pair->ptr, key0_val_addr_before);
  REQUIRE_EQ(*(int *)key0_val_pair->ptr, key0_val_before);

  // The update itself must have actually taken effect.
  int retrieved5 = -1;
  REQUIRE_EQ(
      chmap_get_elem_copy(hm, &(cmap_pair){.ptr = &key5, .size = sizeof(key5)},
                          &retrieved5, sizeof(retrieved5)),
      ccol_success);
  REQUIRE_EQ(retrieved5, 999);

  chmap_destroy(hm);
}

TEST(chash_maps, stress_scaling_ints) {
  chmap_construct(chm, size_t, int);

  size_t max_elems = 1000000;

  // Insert elements
  for (size_t i = 0; i <= max_elems; ++i) {
    int val = i * 10;
    chmap_insert(chm, i, val);
  }

  REQUIRE_EQ(chmap_elem_count(chm), max_elems + 1);

  // Check elements
  for (size_t i = 0; i <= max_elems; ++i) {
    REQUIRE_EQ(chmap_get(chm, i), (int)i * 10);
  }

  // Delete elements
  for (size_t i = 0; i <= max_elems; ++i) {
    chmap_remove(chm, i);
  }

  REQUIRE_EQ(chmap_elem_count(chm), 0);

  chmap_destroy(chm);
}

TEST(chash_maps, stress_scaling_strings) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  size_t max_elems = 1000000;

  char key_buf[16] = {0};
  for (size_t i = 0; i <= max_elems; ++i) {
    snprintf(key_buf, sizeof(key_buf), "k%zu", i);
    REQUIRE_EQ(insert_string_to_int(chmap, key_buf, i + 1), ccol_success);
  }

  REQUIRE_EQ(chmap_elem_count(chmap), max_elems + 1);

  for (size_t i = 0; i <= max_elems; ++i) {
    snprintf(key_buf, sizeof(key_buf), "k%zu", i);
    ccol_retval_t r = delete_int_from_string(chmap, key_buf);
    if (r != ccol_success) {
      printf("Failed to delete %s - i:%zu - r: %d\n", key_buf, i, r);
    }
  }

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  chmap_destroy(chmap);
}

TEST(chash_maps, stress_scaling_ptrs) {
  chmap_construct(chm, size_t, void *);

  size_t max_elems = 1000000;

  // Insert elements
  for (size_t i = 0; i <= max_elems; ++i) {
    void *val = (void *)((uintptr_t)i * 10);
    chmap_insert(chm, i, val);
  }

  REQUIRE_EQ(chmap_elem_count(chm), max_elems + 1);

  // Check elements
  for (size_t i = 0; i <= max_elems; ++i) {
    REQUIRE_EQ((uintptr_t)chmap_get(chm, i), (uintptr_t)i * 10);
  }

  // Delete elements
  for (size_t i = 0; i <= max_elems; ++i) {
    chmap_remove(chm, i);
  }

  REQUIRE_EQ(chmap_elem_count(chm), 0);

  chmap_destroy(chm);
}

unsigned long custom_int_key_hasher(const void *ptr) {
  return *(const int *)ptr;
}

TEST(chash_maps, declarative_macros) {
  {
    chmap_declare(hm, int, char *);
    chmap_init(hm);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }

  {
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    chmap_declare(hm, int, char *);
    chmap_init_mp(hm, &mp);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }

  {
    ccol_hashing_proc_t ch = &custom_int_key_hasher;
    chmap_declare(hm, int, char *);
    chmap_init_ch(hm, ch);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }

  {
    ccol_hashing_proc_t ch = &custom_int_key_hasher;
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    chmap_declare(hm, int, char *);
    chmap_init_full(hm, &mp, ch);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }
}

TEST(chash_maps, constructive_macros) {
  {
    chmap_construct(hm, int, char *);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }

  {
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    chmap_construct_mp(hm, int, char *, &mp);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }

  {
    ccol_hashing_proc_t ch = &custom_int_key_hasher;
    chmap_construct_ch(hm, int, char *, ch);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }

  {
    ccol_hashing_proc_t ch = &custom_int_key_hasher;
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    chmap_construct_full(hm, int, char *, &mp, ch);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // Here, we are not exceeding the size of hello, that's VERY important
    // If we want to store a longer string, we'll need to do a realloc.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }
}

TEST(chash_maps, iteration) {
  chmap_construct(hm, int, char *);

  int key = 3;
  chmap_insert(hm, key, "hello");

  key = 4;
  chmap_insert(hm, key, "hi");

  key = 5;
  chmap_insert(hm, key, "there");

  int records[6] = {0};  // from 0 to 5, so that 3, 4 and 5 are valid indices
  int counter = 0;
  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    key = *ccol_iter_key_ptr(it);
    if (key == 3) {
      REQUIRE_EQ(records[3]++, 0);
      REQUIRE_STREQ(*ccol_iter_val_ptr(it), "hello");
    } else if (key == 4) {
      REQUIRE_EQ(records[4]++, 0);
      REQUIRE_STREQ(*ccol_iter_val_ptr(it), "hi");
    } else if (key == 5) {
      REQUIRE_EQ(records[5]++, 0);
      REQUIRE_STREQ(*ccol_iter_val_ptr(it), "there");
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  chmap_destroy(hm);
}

TEST(chash_maps, map_string_to_void_ptrs) {
  chmap_construct(hm, char *, void *);

  typedef struct some_struct {
    int a;
    short b;
  } some_struct;

  some_struct d1 = (some_struct){.a = 3, .b = 4};
  some_struct d2 = (some_struct){.a = 5, .b = 6};
  some_struct d3 = (some_struct){.a = 7, .b = 8};
  some_struct d4 = (some_struct){.a = 9, .b = 10};

  void *val_ptr = &d1;
  chmap_insert(hm, "d1", val_ptr);

  val_ptr = &d2;
  chmap_insert(hm, "d2", val_ptr);

  val_ptr = &d3;
  char k_d3[16];
  snprintf(k_d3, sizeof(k_d3), "d3");
  chmap_insert(hm, k_d3, val_ptr);

  val_ptr = &d4;
  const char *k_d4 = "d4";
  chmap_insert(hm, k_d4, val_ptr);

  int records[4] = {0};
  int counter = 0;

  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    if (strcmp(*ccol_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->a, 3);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->b, 4);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->a, 5);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->b, 6);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->a, 7);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->b, 8);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d4") == 0) {
      REQUIRE_EQ(records[3]++, 0);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->a, 9);
      REQUIRE_EQ((*(some_struct **)ccol_iter_val_ptr(it))->b, 10);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 4);

  chmap_destroy(hm);
}

TEST(chash_maps, map_string_to_struct_ptrs) {
  typedef struct some_struct {
    int a;
    short b;
  } some_struct;

  chmap_construct(hm, char *, some_struct *);

  some_struct d1 = (some_struct){.a = 3, .b = 4};
  some_struct d2 = (some_struct){.a = 5, .b = 6};
  some_struct d3 = (some_struct){.a = 7, .b = 8};

  some_struct *val_ptr = &d1;
  chmap_insert(hm, "d1", val_ptr);
  val_ptr = &d2;
  chmap_insert(hm, "d2", val_ptr);
  val_ptr = &d3;
  chmap_insert(hm, "d3", val_ptr);

  int records[3] = {0};
  int counter = 0;

  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    if (strcmp(*ccol_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->a, 3);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->b, 4);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->a, 5);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->b, 6);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->a, 7);
      REQUIRE_EQ((*ccol_iter_val_ptr(it))->b, 8);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  chmap_destroy(hm);
}

TEST(chash_maps, map_string_to_a_struct) {
  typedef struct some_struct {
    int a;
    short b;
  } some_struct;

  chmap_construct(hm, char *, some_struct);

  some_struct d1 = (some_struct){.a = 3, .b = 4};
  some_struct d2 = (some_struct){.a = 5, .b = 6};
  some_struct d3 = (some_struct){.a = 7, .b = 8};

  chmap_insert(hm, "d1", d1);
  chmap_insert(hm, "d2", d2);
  chmap_insert(hm, "d3", d3);

  int records[3] = {0};
  int counter = 0;

  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    if (strcmp(*ccol_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 3);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 4);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 5);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 6);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 7);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 8);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  chmap_get_ptr(hm, "d1")->a++;
  chmap_get_ptr(hm, "d1")->b--;
  chmap_get_ptr(hm, "d2")->a++;
  chmap_get_ptr(hm, "d2")->b--;
  chmap_get_ptr(hm, "d3")->a++;
  chmap_get_ptr(hm, "d3")->b--;

  memset(records, 0, sizeof(records));
  counter = 0;

  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    if (strcmp(*ccol_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 4);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 3);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 6);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 5);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 8);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 7);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  chmap_destroy(hm);
}

typedef struct helper_struct {
  int a;
  short b;
} helper_struct;

void helper_function(chmap chm) {
  chmap_redeclare(chm, char *, helper_struct);

  helper_struct d1 = (helper_struct){.a = 3, .b = 4};
  helper_struct d2 = (helper_struct){.a = 5, .b = 6};
  helper_struct d3 = (helper_struct){.a = 7, .b = 8};

  chmap_insert(chm, "d1", d1);
  chmap_insert(chm, "d2", d2);
  chmap_insert(chm, "d3", d3);

  int records[3] = {0};
  int counter = 0;

  ccol_iter_declare(chm, it);
  for (it = ccol_begin(chm); it != NULL; it = ccol_iter_next(it)) {
    if (strcmp(*ccol_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 3);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 4);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 5);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 6);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 7);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 8);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  chmap_get_ptr(chm, "d1")->a++;
  chmap_get_ptr(chm, "d1")->b--;
  chmap_get_ptr(chm, "d2")->a++;
  chmap_get_ptr(chm, "d2")->b--;
  chmap_get_ptr(chm, "d3")->a++;
  chmap_get_ptr(chm, "d3")->b--;
}

TEST(chash_maps, for_each_macro) {
  chmap_construct(hm, int, int);

  for (int i = 1; i <= 5; ++i) {
    int val = i * 10;
    chmap_insert(hm, i, val);
  }

  int sum = 0;
  int count = 0;
  ccol_for_each(hm, it, {
    sum += *ccol_iter_val_ptr(it);
    ++count;
  });

  REQUIRE_EQ(count, 5);
  REQUIRE_EQ(sum, 150);  // 10+20+30+40+50

  chmap_destroy(hm);
}

TEST(chash_maps, construct_scoped_lifecycle) {
  {
    chmap_construct_scoped(hm, int, int);
    REQUIRE_NE((void *)hm, NULL);

    for (int i = 1; i <= 5; ++i) {
      int val = i * 100;
      chmap_insert(hm, i, val);
    }
    for (int i = 1; i <= 5; ++i) {
      REQUIRE_EQ(chmap_get(hm, i), i * 100);
    }
    REQUIRE_EQ(chmap_elem_count(hm), 5);
    // hm is automatically destroyed at end of block (no chmap_destroy needed)
  }
}

// Regression: chmap_get_elem_copy must validate its output buffer parameters
// before dereferencing them. Before the fix, NULL target_buf and zero
// target_buf_size were not checked, so callers had no way to detect the error.
TEST(chash_maps, get_elem_copy_rejects_null_buf_and_zero_size) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 42), ccol_success);

  int val = -1;
  cmap_pair key = {.ptr = (void *)"key1", .size = strlen("key1")};

  REQUIRE_EQ(chmap_get_elem_copy(chmap, &key, NULL, sizeof(int)),
             ccol_invalid_args);
  REQUIRE_EQ(chmap_get_elem_copy(chmap, &key, &val, 0), ccol_invalid_args);

  // Confirm the map still works correctly after the rejected calls
  REQUIRE_EQ(chmap_get_elem_copy(chmap, &key, &val, sizeof(int)), ccol_success);
  REQUIRE_EQ(val, 42);

  chmap_destroy(chmap);
}

// Regression: chmap_iter_key_ptr and chmap_iter_val_ptr used a hardcoded
// variable name 'it' inside the macro body instead of the macro parameter.
// All existing tests happened to name their iterator 'it', masking the bug.
// This test uses a different name to exercise the corrected macro expansion.
TEST(chash_maps, iterator_with_non_default_variable_name) {
  // SC backend (char* -> int)
  {
    chmap_construct(hm, char *, int);
    REQUIRE_NE((void *)hm, NULL);

    REQUIRE_EQ(insert_string_to_int(hm, "alpha", 1), ccol_success);
    REQUIRE_EQ(insert_string_to_int(hm, "beta", 2), ccol_success);
    REQUIRE_EQ(insert_string_to_int(hm, "gamma", 3), ccol_success);

    int sum = 0;
    int count = 0;
    ccol_iter_declare(hm, iter);
    for (iter = ccol_begin(hm); iter != NULL; iter = ccol_iter_next(iter)) {
      sum += *ccol_iter_val_ptr(iter);
      ++count;
    }
    REQUIRE_EQ(count, 3);
    REQUIRE_EQ(sum, 6);

    chmap_destroy(hm);
  }

  // OA backend (int -> int)
  {
    chmap_construct(hm, int, int);
    REQUIRE_NE((void *)hm, NULL);

    int k1 = 10, v1 = 100;
    chmap_insert(hm, k1, v1);
    int k2 = 20, v2 = 200;
    chmap_insert(hm, k2, v2);
    int k3 = 30, v3 = 300;
    chmap_insert(hm, k3, v3);

    int key_sum = 0;
    int val_sum = 0;
    int count = 0;
    ccol_iter_declare(hm, iter);
    for (iter = ccol_begin(hm); iter != NULL; iter = ccol_iter_next(iter)) {
      key_sum += *ccol_iter_key_ptr(iter);
      val_sum += *ccol_iter_val_ptr(iter);
      ++count;
    }
    REQUIRE_EQ(count, 3);
    REQUIRE_EQ(key_sum, 60);   // 10+20+30
    REQUIRE_EQ(val_sum, 600);  // 100+200+300

    chmap_destroy(hm);
  }
}

// Controlled calloc used to simulate OOM during oa_rehash without affecting
// map creation or destruction.
static bool g_calloc_fail = false;
static void *controlled_calloc(size_t nmemb, size_t size) {
  if (g_calloc_fail) return NULL;
  return calloc(nmemb, size);
}

// Controlled malloc used to simulate OOM in sc_reset_val_of_llist_node without
// affecting map creation, initial inserts, or destruction.
static bool g_malloc_fail = false;
static void *controlled_malloc(size_t size) {
  if (g_malloc_fail) return NULL;
  return malloc(size);
}

// Controlled realloc used to simulate OOM in sc_reset_val_of_llist_node's
// heap-to-heap update path without affecting creation, initial inserts, or
// node allocation.
static bool g_realloc_fail = false;
static void *controlled_realloc(void *ptr, size_t size) {
  if (g_realloc_fail) return NULL;
  return realloc(ptr, size);
}

// Unlike a real realloc (which may shrink/grow a block in place), this
// always allocates a fresh block and frees the original, so it
// deterministically moves the allocation on every call. Used to reliably
// exercise sc_reset_val_of_llist_node's heap-to-heap update path when the
// source pointer aliases the block being reallocated: a real realloc might
// happen to keep the same address for a small size change, which would let
// a use-after-free read of the original block go unnoticed.
static void *force_moving_realloc(void *ptr, size_t size) {
  void *new_block = malloc(size);
  if (!new_block) return NULL;
  free(ptr);
  return new_block;
}

// Regression: oa_insert returned ccol_container_full when the probe wrapped
// all the way around without finding a truly-empty slot, even though tombstone
// (deleted) slots were available for reuse. The post-loop tombstone reuse path
// is only reachable when all 16 slots are either live or tombstoned, which
// requires the rehash calloc to fail (OOM). We simulate that here.
TEST(chash_maps, oa_tombstone_reuse_after_full_probe_wrap) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = controlled_calloc,
                             .realloc = realloc};

  // Use the minimum capacity (16) so that oa_delete's shrink guard
  // (capacity > minimum_allowed_bucket_array_size) never fires and tombstones
  // accumulate without a rehash clearing them.
  char *err = NULL;
  chashmap *hm = chmap_create_mp(1, ccol_int, ccol_int, &mp, &err);
  REQUIRE_NE((void *)hm, NULL);

  // Make all subsequent calloc calls fail so rehash can never enlarge the map.
  // Inserts k0..k15 still succeed because, even though the load-factor check
  // (count+deleted)/16 > 0.70 fires from key 12 onwards, the failed rehash
  // leaves the original 16-slot array in place and there are still empty slots
  // for the probe to find.
  g_calloc_fail = true;
  for (int i = 0; i < 16; i++) {
    int val = i * 10;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 16);

  // Delete 10 keys; no calloc involved, and at capacity == minimum the shrink
  // check in oa_delete is suppressed, so these become tombstones in place.
  for (int i = 0; i < 10; i++) {
    REQUIRE_EQ(
        chmap_delete_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 6);

  // All 16 slots are now either live (6) or tombstoned (10). Inserting key 16
  // triggers the load check ((6+10)/16 == 1.0 > 0.70), which tries to rehash,
  // which calloc-fails, leaving the map unchanged. The probe then visits every
  // slot without finding an empty one. The post-loop fix reuses the first
  // tombstone slot it recorded instead of returning ccol_container_full.
  int new_key = 16, new_val = 160;
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = &new_key, .size = sizeof(new_key)},
                 &(cmap_pair){.ptr = &new_val, .size = sizeof(new_val)}),
             ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), 7);

  int retrieved = 0;
  REQUIRE_EQ(chmap_get_elem_copy(
                 hm, &(cmap_pair){.ptr = &new_key, .size = sizeof(new_key)},
                 &retrieved, sizeof(retrieved)),
             ccol_success);
  REQUIRE_EQ(retrieved, 160);

  g_calloc_fail = false;
  chmap_destroy(hm);
}

// Regression: when an opportunistic grow-on-insert rehash fails (sustained
// OOM: calloc never recovers) AND the probe subsequently finds the table
// genuinely full (every slot live, no tombstones to reuse since nothing was
// ever deleted), oa_insert used to unconditionally report
// ccol_container_full - indistinguishable from having reached the map's
// real, architectural max_elem_count - even though the true cause is a
// failed allocation. It must report ccol_not_enough_memory instead, exactly
// as chmap_insert_elem's own documented contract promises ("ccol_not_enough
// _memory if allocation fails").
TEST(chash_maps, oa_insert_sustained_oom_reports_not_enough_memory) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = controlled_calloc,
                             .realloc = realloc};

  char *err = NULL;
  chashmap *hm = chmap_create_mp(1, ccol_int, ccol_int, &mp, &err);
  REQUIRE_NE((void *)hm, NULL);

  // Make every subsequent calloc call fail so the table can never grow.
  g_calloc_fail = true;

  // Fill the table completely (16 keys into a 16-slot table), with no
  // deletions at all, so no tombstones ever exist to fall back on. Every one
  // of these still succeeds: even though several of them re-attempt (and
  // fail) a rehash once the load factor crosses 0.70, there are still empty
  // slots left in the original 16-slot array for the probe to land on.
  for (int i = 0; i < 16; i++) {
    int val = i;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)16);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  // One more, genuinely new key: the load factor check fires, the rehash
  // fails (calloc still disabled), and the probe now finds the table
  // completely full - no empty slot, no tombstone. This must be reported as
  // an allocation failure, not a false "container full".
  int overflow_key = 16, overflow_val = 1600;
  REQUIRE_EQ(
      chmap_insert_elem(
          hm, &(cmap_pair){.ptr = &overflow_key, .size = sizeof(overflow_key)},
          &(cmap_pair){.ptr = &overflow_val, .size = sizeof(overflow_val)}),
      ccol_not_enough_memory);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)16);

  g_calloc_fail = false;

  // With the allocator restored, the very same insert must now succeed by
  // actually growing the table.
  REQUIRE_EQ(
      chmap_insert_elem(
          hm, &(cmap_pair){.ptr = &overflow_key, .size = sizeof(overflow_key)},
          &(cmap_pair){.ptr = &overflow_val, .size = sizeof(overflow_val)}),
      ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)17);
  REQUIRE_TRUE(chmap_get_bucket_arr_size(hm) > 16);

  chmap_destroy(hm);
}

// Regression guard: in the OA backend, map->val_size must stay the single,
// immutable value fixed by oa_create() at map-creation time for the entire
// lifetime of the map (never re-derived from a later insert), so that
// emptying a map via repeated deletes and then repopulating it cannot
// silently corrupt val_accessors' reported value size.
TEST(chash_maps, oa_repopulate_after_full_delete) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  for (int i = 0; i < 20; ++i) {
    int val = i * 10;
    chmap_insert(hm, i, val);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 20);

  for (int i = 0; i < 20; ++i) {
    chmap_remove(hm, i);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 0);

  // Repopulate and verify all values survive correctly
  for (int i = 0; i < 20; ++i) {
    int val = i * 100;
    chmap_insert(hm, i, val);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 20);

  for (int i = 0; i < 20; ++i) {
    REQUIRE_EQ(chmap_get(hm, i), i * 100);
  }

  chmap_destroy(hm);
}

TEST(chash_maps, sc_large_key_and_large_value) {
  // Keys and values larger than INLINE_STORAGE_THRESHOLD (23 bytes) take the
  // heap-allocation path in sc_create_llist_node.  This test verifies that
  // the happy path works correctly and that the node is properly attached to
  // the insertion-order list after both allocations succeed.
  chmap_construct(hm, char *, char *);

  // 24-byte key (just over the 23-byte SSO limit), 24-byte value.
  char key1[25], val1[25], key2[25], val2[25], key3[25], val3[25];
  memset(key1, 'a', 24);
  key1[24] = '\0';
  memset(val1, '1', 24);
  val1[24] = '\0';
  memset(key2, 'b', 24);
  key2[24] = '\0';
  memset(val2, '2', 24);
  val2[24] = '\0';
  memset(key3, 'c', 24);
  key3[24] = '\0';
  memset(val3, '3', 24);
  val3[24] = '\0';

  char *k1 = key1, *k2 = key2, *k3 = key3;
  char *v1 = val1, *v2 = val2, *v3 = val3;

  chmap_insert(hm, k1, v1);
  chmap_insert(hm, k2, v2);
  chmap_insert(hm, k3, v3);

  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);

  REQUIRE_STREQ(chmap_get(hm, k1), val1);
  REQUIRE_STREQ(chmap_get(hm, k2), val2);
  REQUIRE_STREQ(chmap_get(hm, k3), val3);

  // Verify iteration visits all three entries exactly once.
  size_t visited = 0;
  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    ++visited;
  }
  REQUIRE_EQ(visited, (size_t)3);

  chmap_destroy(hm);
}

// A char*-keyed map always uses separate chaining, and any scalar value
// type that fits inline (<= 23 bytes, the SSO threshold) is stored directly
// inside the chmap_entry backing the node. chmap_get/chmap_get_ptr and the
// iterator accessors cast a pointer into that storage straight to the
// value's own type and dereference it, so the storage must be naturally
// aligned for that type; this test exercises that for double (8-byte
// alignment) and for a struct whose own alignment requirement is 8 bytes,
// via chmap_get_ptr specifically since it hands back a pointer for
// in-place modification, not just a copied-out value.
TEST(chash_maps, sc_inline_value_alignment) {
  {
    chmap_construct(hm, char *, double);

    double v1 = 3.14159, v2 = -2.71828;
    chmap_insert(hm, "pi", v1);
    chmap_insert(hm, "e", v2);

    double *p1 = chmap_get_ptr(hm, "pi");
    double *p2 = chmap_get_ptr(hm, "e");
    REQUIRE_NE((void *)p1, (void *)NULL);
    REQUIRE_NE((void *)p2, (void *)NULL);
    REQUIRE_EQ((uintptr_t)p1 % _Alignof(double), (uintptr_t)0);
    REQUIRE_EQ((uintptr_t)p2 % _Alignof(double), (uintptr_t)0);
    REQUIRE_EQ(*p1, v1);
    REQUIRE_EQ(*p2, v2);

    // chmap_get_ptr's documented in-place-modification contract.
    *p1 = 999.5;
    REQUIRE_EQ(chmap_get(hm, "pi"), 999.5);

    chmap_destroy(hm);
  }

  {
    typedef struct {
      double d;
      long l;
    } aligned_struct;

    chmap_construct(hm, char *, aligned_struct);

    aligned_struct v = {.d = 1.5, .l = 42};
    chmap_insert(hm, "s", v);

    aligned_struct *p = chmap_get_ptr(hm, "s");
    REQUIRE_NE((void *)p, (void *)NULL);
    REQUIRE_EQ((uintptr_t)p % _Alignof(aligned_struct), (uintptr_t)0);
    REQUIRE_EQ(p->d, 1.5);
    REQUIRE_EQ(p->l, 42);

    chmap_destroy(hm);
  }
}

// long double requires 16-byte alignment on this platform (_Alignof(long
// double) == 16), stricter than every other case sc_inline_value_alignment
// covers above; chashmap.h documents that long double is
// excluded from the open-addressing backend (it can be > 8 bytes), so any
// chashmap with a long double key or value always routes through this
// separate-chaining, SSO-inline-storage path. Covers all three direct-cast
// accessors that read inline storage without a memcpy (chmap_get_ptr,
// chmap_get, and the iterator's ccol_iter_key_ptr/ccol_iter_val_ptr), since
// each is a distinct code path capable of dereferencing a misaligned
// pointer. Verified (before the fix that added _Alignas(max_align_t) to
// chmap_entry's key_storage/val_storage) to fail under
// -fsanitize=undefined with "load of misaligned address ... which requires
// 16 byte alignment".
TEST(chash_maps, sc_inline_long_double_alignment) {
  // long double as a value (paired with a char* key -> separate chaining).
  {
    chmap_construct(hm, char *, long double);

    long double v = 3.5L;
    chmap_insert(hm, "pi", v);

    long double *p = chmap_get_ptr(hm, "pi");
    REQUIRE_NE((void *)p, (void *)NULL);
    REQUIRE_EQ((uintptr_t)p % _Alignof(long double), (uintptr_t)0);
    REQUIRE_EQ(*p, v);
    REQUIRE_EQ(chmap_get(hm, "pi"), v);

    *p = 999.5L;
    REQUIRE_EQ(chmap_get(hm, "pi"), 999.5L);

    ccol_iter_declare(hm, it);
    it = ccol_begin(hm);
    REQUIRE_NE((void *)it, NULL);
    const long double *val_via_iter = ccol_iter_val_ptr(it);
    REQUIRE_EQ((uintptr_t)val_via_iter % _Alignof(long double), (uintptr_t)0);
    REQUIRE_EQ(*val_via_iter, 999.5L);
    ccol_iter_destroy(it);

    chmap_destroy(hm);
  }

  // long double as a key (paired with an int value -> separate chaining,
  // since long double is not integral-for-open-addressing purposes either).
  {
    chmap_construct(hm, long double, int);

    // Key equality/hashing here is bitwise over the full sizeof(long
    // double), same as any other non-string, non-canonicalized key type
    // (see chashmap.c's own sc_compare_keys/hash_key_data); on this
    // platform long double's 16-byte representation carries real padding
    // bits (only ~80 bits are significant). A plain automatic-storage
    // scalar (even memset first) does not reliably leave those padding
    // bytes defined once optimized: the compiler is free to treat a later
    // whole-object assignment as making the prior memset dead, since the
    // padding bits are not part of the object's "value" from the abstract
    // machine's point of view (confirmed empirically under valgrind at
    // this Makefile's actual -O3 flags: memset-then-assign into an
    // automatic-storage-duration long double still left padding bytes
    // undefined). A static-storage-duration object with a compile-time-
    // constant initializer does not have this problem: the compiler
    // materializes its full, fixed-width byte representation once, so
    // every byte (including padding) is deterministic. This is a
    // property of constructing a fully-defined long double bit pattern in
    // portable C, unrelated to this test's actual target (chmap_entry's
    // inline-storage alignment).
    static long double k = 2.5L;
    int val = 7;
    chmap_insert(hm, k, val);

    REQUIRE_EQ(chmap_get(hm, k), 7);

    ccol_iter_declare(hm, it);
    it = ccol_begin(hm);
    REQUIRE_NE((void *)it, NULL);
    const long double *key_via_iter = ccol_iter_key_ptr(it);
    REQUIRE_EQ((uintptr_t)key_via_iter % _Alignof(long double), (uintptr_t)0);
    REQUIRE_EQ(*key_via_iter, k);
    ccol_iter_destroy(it);

    chmap_destroy(hm);
  }
}

TEST(chash_maps, char_type_variants_as_string_keys) {
  // signed char * (= int8_t *) must route through the string path and behave
  // identically to char * for all map operations.
  {
    chmap_construct(hm, signed char *, int);

    signed char *k1 = (signed char *)"hello";
    signed char *k2 = (signed char *)"world";
    int v1 = 10, v2 = 20;

    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    REQUIRE_EQ(chmap_elem_count(hm), 2);
    REQUIRE_EQ(chmap_get(hm, k1), 10);
    REQUIRE_EQ(chmap_get(hm, k2), 20);

    // char * with identical string content must find the same entries.
    char *ck1 = "hello";
    char *ck2 = "world";
    REQUIRE_EQ(chmap_get(hm, ck1), 10);
    REQUIRE_EQ(chmap_get(hm, ck2), 20);

    chmap_remove(hm, k2);
    REQUIRE_EQ(chmap_elem_count(hm), 1);

    chmap_destroy(hm);
  }

  // unsigned char * (= uint8_t *); identical check.
  {
    chmap_construct(hm, unsigned char *, int);

    unsigned char *k1 = (unsigned char *)"alpha";
    unsigned char *k2 = (unsigned char *)"beta";
    int v1 = 30, v2 = 40;

    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    REQUIRE_EQ(chmap_elem_count(hm), 2);
    REQUIRE_EQ(chmap_get(hm, k1), 30);
    REQUIRE_EQ(chmap_get(hm, k2), 40);

    char *ck1 = "alpha";
    char *ck2 = "beta";
    REQUIRE_EQ(chmap_get(hm, ck1), 30);
    REQUIRE_EQ(chmap_get(hm, ck2), 40);

    chmap_destroy(hm);
  }

  // char * map looked up via signed char * and unsigned char *.
  {
    chmap_construct(hm, char *, int);

    char *k1 = "foo";
    char *k2 = "bar";
    int v1 = 50, v2 = 60;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);

    signed char *sk1 = (signed char *)"foo";
    unsigned char *uk2 = (unsigned char *)"bar";
    REQUIRE_EQ(chmap_get(hm, sk1), 50);
    REQUIRE_EQ(chmap_get(hm, uk2), 60);

    chmap_destroy(hm);
  }
}

TEST(chash_maps, re_enabled_local_chm_macros) {
  chmap_construct(hm, char *, helper_struct);

  helper_function(hm);

  int records[3] = {0};
  int counter = 0;

  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    if (strcmp(*ccol_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 4);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 3);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 6);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 5);
    } else if (strcmp(*ccol_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->a, 8);
      REQUIRE_EQ(ccol_iter_val_ptr(it)->b, 7);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  chmap_destroy(hm);
}

// SC backend iteration is reverse-insertion-order (most-recently inserted
// element first) because attach_node_to_dllist prepends new nodes.
// Verify that the documented behaviour holds: inserting alpha, beta, gamma
// produces the traversal order gamma -> beta -> alpha.
TEST(chash_maps, sc_reverse_insertion_order_iteration) {
  chmap_construct(hm, char *, int);
  REQUIRE_NE((void *)hm, NULL);

  // chmap_insert requires lvalue arguments; string literals are lvalues in C
  // but integer literals are not; use variables.
  int v1 = 1, v2 = 2, v3 = 3, v100 = 100;
  chmap_insert(hm, "alpha", v1);
  chmap_insert(hm, "beta", v2);
  chmap_insert(hm, "gamma", v3);

  const char *expected_keys[] = {"gamma", "beta", "alpha"};
  int expected_vals[] = {3, 2, 1};
  int idx = 0;

  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    REQUIRE_TRUE(idx < 3);
    REQUIRE_STREQ(*ccol_iter_key_ptr(it), expected_keys[idx]);
    REQUIRE_EQ(*ccol_iter_val_ptr(it), expected_vals[idx]);
    ++idx;
  }
  REQUIRE_EQ(idx, 3);

  // Updating a value must not change the element's position in the traversal.
  chmap_insert(hm, "alpha", v100);

  idx = 0;
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    REQUIRE_TRUE(idx < 3);
    REQUIRE_STREQ(*ccol_iter_key_ptr(it), expected_keys[idx]);
    if (idx == 2) {
      REQUIRE_EQ(*ccol_iter_val_ptr(it), 100);  // alpha updated
    } else {
      REQUIRE_EQ(*ccol_iter_val_ptr(it), expected_vals[idx]);
    }
    ++idx;
  }
  REQUIRE_EQ(idx, 3);

  chmap_destroy(hm);
}

// chmap_reset(map, 0) must clear all elements but retain the current bucket
// array size.  This exercises the zero-argument path in both oa_reset and the
// public chmap_reset dispatcher.
TEST(chash_maps, oa_reset_zero_preserves_capacity) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  for (int i = 0; i < 10; ++i) {
    int val = i * 7;
    chmap_insert(hm, i, val);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 10);

  size_t capacity_before = chmap_get_bucket_arr_size(hm);

  // Pass 0: keep current size.
  REQUIRE_EQ(chmap_reset(hm, 0), ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), 0);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), capacity_before);

  // Elements inserted after reset must work correctly.
  for (int i = 0; i < 10; ++i) {
    int val = i * 13;
    chmap_insert(hm, i, val);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 10);
  for (int i = 0; i < 10; ++i) {
    REQUIRE_EQ(chmap_get(hm, i), i * 13);
  }

  chmap_destroy(hm);
}

// chmap_get_elem_copy must copy only min(val_size, target_buf_size) bytes
// when the caller provides a buffer smaller than the stored value.  No crash,
// no out-of-bounds write, and the partial data must match the leading bytes of
// the original value.
TEST(chash_maps, get_elem_copy_partial_buffer) {
  chashmap *hm = chmap_create(1, ccol_string, ccol_other_types, NULL);
  REQUIRE_NE((void *)hm, NULL);

  typedef struct {
    int a;
    int b;
    int c;
    int d;
  } big_val;
  big_val v = {10, 20, 30, 40};
  const char *key = "bigkey";

  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = &v, .size = sizeof(v)}),
             ccol_success);

  // Buffer for only the first two fields.
  struct {
    int a;
    int b;
  } partial = {0xFF, 0xFF};
  REQUIRE_EQ(chmap_get_elem_copy(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &partial, sizeof(partial)),
             ccol_success);

  REQUIRE_EQ(partial.a, 10);
  REQUIRE_EQ(partial.b, 20);

  chmap_destroy(hm);
}

// Regression: sc_reset_val_of_llist_node wrote NULL directly into the
// val_storage union before checking the malloc return value.  Because
// val_storage.ptr and val_storage.inline_data share the same memory, this
// zeroed the first sizeof(void*) bytes of the old inline value, corrupting it
// on OOM while leaving val_is_inline still true.  Verify that after a failed
// inline-to-heap update the original inline value is intact.
TEST(chash_maps, sc_value_update_inline_to_heap_oom_resilience) {
  ccol_memmgmt_procs_t mp = {.malloc = controlled_malloc,
                             .free = free,
                             .calloc = calloc,
                             .realloc = realloc};
  chashmap *hm = chmap_create_mp(1, ccol_string, ccol_other_types, &mp, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key = "k";

  // Insert with a 4-byte inline value.
  unsigned char small_val[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = small_val, .size = sizeof(small_val)}),
             ccol_success);

  // Confirm the inline value is readable.
  cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, (size_t)4);
  REQUIRE_EQ(memcmp(vp->ptr, small_val, 4), 0);

  // Attempt to update with a heap-requiring value (>23 bytes) while malloc
  // fails.  The update must be rejected gracefully.
  unsigned char large_val[30];
  memset(large_val, 0xAB, sizeof(large_val));
  g_malloc_fail = true;
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = large_val, .size = sizeof(large_val)}),
             ccol_not_enough_memory);
  g_malloc_fail = false;

  // The original 4-byte inline value must be intact.
  vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, (size_t)4);
  REQUIRE_EQ(memcmp(vp->ptr, small_val, 4), 0);

  chmap_destroy(hm);
}

// ccol_begin on an empty map must return NULL for both the SC backend
// (string key) and the OA backend (int key), exercising the early-out paths
// in chashmap_begin_iter.
TEST(chash_maps, empty_map_iteration) {
  {
    chmap_construct(hm, char *, int);
    REQUIRE_NE((void *)hm, NULL);
    REQUIRE_EQ((void *)ccol_begin(hm), NULL);
    chmap_destroy(hm);
  }

  {
    chmap_construct(hm, int, int);
    REQUIRE_NE((void *)hm, NULL);
    REQUIRE_EQ((void *)ccol_begin(hm), NULL);
    chmap_destroy(hm);
  }
}

// chashmap_begin_iter(NULL, ...) must be treated the same as an empty map:
// return NULL without touching err, rather than asserting. This is a real,
// deliberately relied-upon contract elsewhere in this codebase, not merely
// permissive behavior: e.g. tests/chttpclient/tests.c's own
// request.headers_begin_empty_returns_null iterates a chttp_request_t's
// lazily-created (still-NULL until the first header is set) internal chmap
// field directly via ccol_begin/chashmap_begin_iter, and several call sites
// across chttpclient.c/clogger.c/ctls.c/cthreadcomm.c hand an
// optional/lazily-created chmap straight to chashmap_begin_iter as well.
TEST(chash_maps, begin_iter_null_map_returns_null_like_empty) {
  chmap null_map = NULL;
  char *err = (char *)0x1; /* poison value: must be reset to NULL, not left */
  REQUIRE_EQ((void *)chashmap_begin_iter(null_map, &err), NULL);
  REQUIRE_EQ((void *)err, NULL);

  // NULL for err itself must also be tolerated (it's documented as optional).
  REQUIRE_EQ((void *)chashmap_begin_iter(null_map, NULL), NULL);
}

// chmap_reset with a non-zero new_bucket_array_size must resize the internal
// slot array on the OA backend, clear all elements, and allow correct
// insertions and lookups afterwards.
TEST(chash_maps, oa_reset_with_new_size) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  for (int i = 0; i < 10; ++i) {
    int val = i * 7;
    chmap_insert(hm, i, val);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 10);

  REQUIRE_EQ(chmap_reset(hm, 256), ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), 0);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)256);

  for (int i = 0; i < 10; ++i) {
    int val = i * 13;
    chmap_insert(hm, i, val);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 10);
  for (int i = 0; i < 10; ++i) {
    REQUIRE_EQ(chmap_get(hm, i), i * 13);
  }

  chmap_destroy(hm);
}

// When an existing SC-backend entry whose value is heap-allocated (>23 bytes)
// is updated with a new value that fits inline (<=23 bytes), the old heap
// buffer must be freed, the node must switch to inline storage, and the new
// value must be readable with the correct size.
TEST(chash_maps, sc_value_update_heap_to_inline) {
  chashmap *hm = chmap_create(1, ccol_string, ccol_other_types, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key = "k";

  unsigned char large_val[30];
  memset(large_val, 0xAA, sizeof(large_val));
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = large_val, .size = sizeof(large_val)}),
             ccol_success);

  unsigned char small_val[4] = {0x11, 0x22, 0x33, 0x44};
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = small_val, .size = sizeof(small_val)}),
             ccol_key_already_present);

  cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, (size_t)4);
  REQUIRE_EQ(memcmp(vp->ptr, small_val, 4), 0);

  chmap_destroy(hm);
}

// When an SC-backend entry with a heap-allocated value is updated with another
// heap-requiring value but realloc fails, sc_reset_val_of_llist_node must
// restore the original pointer and leave the old value fully intact.
TEST(chash_maps, sc_value_update_heap_to_heap_oom) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = calloc,
                             .realloc = controlled_realloc};
  chashmap *hm = chmap_create_mp(1, ccol_string, ccol_other_types, &mp, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key = "k";

  unsigned char large_val1[30];
  memset(large_val1, 0xAA, sizeof(large_val1));
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = large_val1, .size = sizeof(large_val1)}),
             ccol_success);

  unsigned char large_val2[40];
  memset(large_val2, 0xBB, sizeof(large_val2));
  g_realloc_fail = true;
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = large_val2, .size = sizeof(large_val2)}),
             ccol_not_enough_memory);
  g_realloc_fail = false;

  cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, (size_t)30);
  REQUIRE_EQ(memcmp(vp->ptr, large_val1, 30), 0);

  chmap_destroy(hm);
}

// Regression: a value pointer that aliases the entry's own current
// (heap-backed) storage - e.g. a caller re-inserting a value derived from a
// pointer obtained via chmap_get_elem_ref/chmap_get_ptr/chmap_get for the
// same key - must still be read correctly even though shrinking to an
// inline-sized value frees that same heap buffer as part of the update.
// Before the fix, sc_reset_val_of_llist_node freed the buffer and only then
// copied from it, a use-after-free read (caught under valgrind/ASan; the
// value assertion below also fails whenever the freed block happens to be
// altered before the read completes).
TEST(chash_maps, sc_value_update_self_referential_heap_to_inline) {
  chashmap *hm = chmap_create(1, ccol_string, ccol_other_types, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key = "k";
  unsigned char large_val[30];
  for (size_t i = 0; i < sizeof(large_val); ++i) {
    large_val[i] = (unsigned char)(i + 1);
  }
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = large_val, .size = sizeof(large_val)}),
             ccol_success);

  cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  void *heap_storage = vp->ptr;

  unsigned char expected[4];
  memcpy(expected, heap_storage, sizeof(expected));

  // Re-insert using the map's own current heap storage as the source,
  // shrinking to a size that fits inline.
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = heap_storage, .size = sizeof(expected)}),
             ccol_key_already_present);

  vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, sizeof(expected));
  REQUIRE_EQ(memcmp(vp->ptr, expected, sizeof(expected)), 0);

  chmap_destroy(hm);
}

// Regression: growing an inline-backed value using the map's own current
// inline storage as the source must not corrupt the source bytes before
// they are read. Before the fix, sc_reset_val_of_llist_node wrote the new
// heap pointer into the val_storage union - which physically overlaps the
// inline byte array - before copying from it, corrupting the copied-out
// value's leading bytes regardless of any sanitizer. Growing to exactly 24
// bytes (INLINE_STORAGE_THRESHOLD + 1) keeps the read fully within the
// inline array's own bounds, so the expected result is fully deterministic:
// the original 4 bytes followed by the zero bytes the node was calloc'd
// with and never overwrote.
TEST(chash_maps, sc_value_update_self_referential_inline_to_heap) {
  chashmap *hm = chmap_create(1, ccol_string, ccol_other_types, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key = "k";
  unsigned char small_val[4] = {0x11, 0x22, 0x33, 0x44};
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = small_val, .size = sizeof(small_val)}),
             ccol_success);

  cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  void *inline_storage = vp->ptr;

  unsigned char expected[24] = {0};
  memcpy(expected, small_val, sizeof(small_val));

  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = inline_storage, .size = sizeof(expected)}),
             ccol_key_already_present);

  vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, sizeof(expected));
  REQUIRE_EQ(memcmp(vp->ptr, expected, sizeof(expected)), 0);

  chmap_destroy(hm);
}

// Regression: shrinking a heap-backed value using the map's own current
// heap storage as the source, when realloc moves the block, must not read
// from the just-freed original block. force_moving_realloc deterministically
// frees the original allocation and returns a fresh one (unlike a real
// realloc, which may shrink a block in place and mask the bug), turning a
// stale read into a reliably-detectable use-after-free under valgrind/ASan.
TEST(chash_maps, sc_value_update_self_referential_heap_to_heap) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = calloc,
                             .realloc = force_moving_realloc};
  chashmap *hm = chmap_create_mp(1, ccol_string, ccol_other_types, &mp, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key = "k";
  unsigned char large_val[40];
  for (size_t i = 0; i < sizeof(large_val); ++i) {
    large_val[i] = (unsigned char)(i + 1);
  }
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = large_val, .size = sizeof(large_val)}),
             ccol_success);

  cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  void *heap_storage = vp->ptr;

  unsigned char expected[30];
  memcpy(expected, heap_storage, sizeof(expected));

  // Still > INLINE_STORAGE_THRESHOLD, so this stays on the heap-to-heap
  // (realloc) path rather than switching to inline storage.
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = heap_storage, .size = sizeof(expected)}),
             ccol_key_already_present);

  vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, sizeof(expected));
  REQUIRE_EQ(memcmp(vp->ptr, expected, sizeof(expected)), 0);

  chmap_destroy(hm);
}

// Regression companion: an exact self-copy (same size, value pointer
// aliases the entry's own current storage byte-for-byte) must also leave
// the value correct, whether the entry is currently inline or heap-backed.
TEST(chash_maps, sc_value_update_self_referential_same_size) {
  chashmap *hm = chmap_create(1, ccol_string, ccol_other_types, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key_inline = "i";
  unsigned char small_val[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  REQUIRE_EQ(
      chmap_insert_elem(
          hm,
          &(cmap_pair){.ptr = (void *)key_inline, .size = strlen(key_inline)},
          &(cmap_pair){.ptr = small_val, .size = sizeof(small_val)}),
      ccol_success);

  cmap_pair *vp = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm,
                                &(cmap_pair){.ptr = (void *)key_inline,
                                             .size = strlen(key_inline)},
                                &vp),
             ccol_success);
  REQUIRE_EQ(chmap_insert_elem(hm,
                               &(cmap_pair){.ptr = (void *)key_inline,
                                            .size = strlen(key_inline)},
                               &(cmap_pair){.ptr = vp->ptr, .size = vp->size}),
             ccol_key_already_present);

  vp = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm,
                                &(cmap_pair){.ptr = (void *)key_inline,
                                             .size = strlen(key_inline)},
                                &vp),
             ccol_success);
  REQUIRE_EQ(memcmp(vp->ptr, small_val, sizeof(small_val)), 0);

  const char *key_heap = "h";
  unsigned char large_val[30];
  for (size_t i = 0; i < sizeof(large_val); ++i) {
    large_val[i] = (unsigned char)(i + 1);
  }
  REQUIRE_EQ(
      chmap_insert_elem(
          hm, &(cmap_pair){.ptr = (void *)key_heap, .size = strlen(key_heap)},
          &(cmap_pair){.ptr = large_val, .size = sizeof(large_val)}),
      ccol_success);

  vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key_heap, .size = strlen(key_heap)},
          &vp),
      ccol_success);
  REQUIRE_EQ(
      chmap_insert_elem(
          hm, &(cmap_pair){.ptr = (void *)key_heap, .size = strlen(key_heap)},
          &(cmap_pair){.ptr = vp->ptr, .size = vp->size}),
      ccol_key_already_present);

  vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key_heap, .size = strlen(key_heap)},
          &vp),
      ccol_success);
  REQUIRE_EQ(memcmp(vp->ptr, large_val, sizeof(large_val)), 0);

  chmap_destroy(hm);
}

// Regression: re-inserting a value for an already-present key, using a
// pointer obtained via chmap_get_elem_ref/chmap_get_ptr for that exact key
// as the insert source, must not corrupt the stored value. This mirrors the
// sc_value_update_self_referential_* group above, but for the
// open-addressing backend's own existing-key update path (a plain in-place
// mem_cpy into the slot's val_data field, with no aliasing guard before the
// fix), reached through the raw chmap_insert_elem function layer rather
// than the type-safe macros (which always copy through an on-stack local
// and can never alias map-owned storage this way).
TEST(chash_maps, oa_value_update_self_referential_same_size) {
  chashmap *hm = chmap_create(1, ccol_int, ccol_long_long, NULL);
  REQUIRE_NE((void *)hm, NULL);

  int key = 7;
  /* long long, not long: long is only 4 bytes on ILP32 (e.g. i386), too
   * narrow to hold this 8-byte bit pattern at all; long long is reliably
   * 8 bytes on every mainstream platform this library targets, matching the
   * precedent tests/clrucache/tests.c already established for the identical
   * "distinctive 8-byte bit pattern" need. */
  long long val = 0x1122334455667788LL;
  REQUIRE_EQ(
      chmap_insert_elem(hm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  cmap_pair *vp = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(
                 hm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, &vp),
             ccol_success);
  REQUIRE_EQ(
      chmap_insert_elem(hm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = vp->ptr, .size = vp->size}),
      ccol_key_already_present);

  vp = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(
                 hm, &(cmap_pair){.ptr = &key, .size = sizeof(key)}, &vp),
             ccol_success);
  long long got;
  memcpy(&got, vp->ptr, sizeof(got));
  REQUIRE_EQ(got, val);

  chmap_destroy(hm);
}

// Verify that a custom hashing function is correctly wired through the OA
// backend (int->int, both key and value are integral, so OA is selected).
// All CRUD operations and iteration must work under the custom hasher.
TEST(chash_maps, oa_custom_hashing) {
  ccol_hashing_proc_t ch = &custom_int_key_hasher;
  chmap_construct_ch(hm, int, int, ch);

  int k10 = 10, v100 = 100;
  int k20 = 20, v200 = 200;
  int k30 = 30, v300 = 300;
  chmap_insert(hm, k10, v100);
  chmap_insert(hm, k20, v200);
  chmap_insert(hm, k30, v300);

  REQUIRE_EQ(chmap_elem_count(hm), 3);
  REQUIRE_EQ(chmap_get(hm, k10), 100);
  REQUIRE_EQ(chmap_get(hm, k20), 200);
  REQUIRE_EQ(chmap_get(hm, k30), 300);

  int v999 = 999;
  chmap_insert(hm, k20, v999);
  REQUIRE_EQ(chmap_get(hm, k20), 999);

  ccol_retval_t r = chmap_remove(hm, k10);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), 2);
  REQUIRE_EQ((void *)chmap_get_ptr(hm, k10), (void *)NULL);

  int key_sum = 0;
  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    key_sum += *ccol_iter_key_ptr(it);
  }
  REQUIRE_EQ(key_sum, 50);

  chmap_destroy(hm);
}

// chmap_insert_elem, chmap_get_elem_ref, and chmap_delete_elem must return
// ccol_invalid_args when passed NULL pointers or a zero-size key/value.
TEST(chash_maps, insert_and_ref_invalid_args) {
  chmap_construct(hm, char *, int);
  REQUIRE_NE((void *)hm, NULL);

  const char *s = "hello";
  int v = 42;
  cmap_pair key_null_ptr = {.ptr = NULL, .size = 5};
  cmap_pair key_zero_size = {.ptr = (void *)s, .size = 0};
  cmap_pair valid_key = {.ptr = (void *)s, .size = 5};
  cmap_pair val_null_ptr = {.ptr = NULL, .size = 4};
  cmap_pair val_zero_size = {.ptr = &v, .size = 0};
  cmap_pair valid_val = {.ptr = &v, .size = sizeof(v)};

  REQUIRE_EQ(chmap_insert_elem(hm, &key_null_ptr, &valid_val),
             ccol_invalid_args);
  REQUIRE_EQ(chmap_insert_elem(hm, &key_zero_size, &valid_val),
             ccol_invalid_args);
  REQUIRE_EQ(chmap_insert_elem(hm, NULL, &valid_val), ccol_invalid_args);
  REQUIRE_EQ(chmap_insert_elem(hm, &valid_key, &val_null_ptr),
             ccol_invalid_args);
  REQUIRE_EQ(chmap_insert_elem(hm, &valid_key, &val_zero_size),
             ccol_invalid_args);
  REQUIRE_EQ(chmap_insert_elem(hm, &valid_key, NULL), ccol_invalid_args);

  cmap_pair *out = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm, &key_null_ptr, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_get_elem_ref(hm, &key_zero_size, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_get_elem_ref(hm, NULL, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_get_elem_ref(hm, &valid_key, NULL), ccol_invalid_args);

  REQUIRE_EQ(chmap_delete_elem(hm, &key_null_ptr), ccol_invalid_args);
  REQUIRE_EQ(chmap_delete_elem(hm, &key_zero_size), ccol_invalid_args);
  REQUIRE_EQ(chmap_delete_elem(hm, NULL), ccol_invalid_args);

  REQUIRE_EQ(chmap_elem_count(hm), 0);
  chmap_destroy(hm);
}

// chmap_get_ptr on the OA backend must return a pointer that, when written
// through, is reflected in subsequent chmap_get calls.  Also confirms NULL is
// returned for an absent key.
TEST(chash_maps, oa_get_ptr_in_place_modification) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  int k1 = 10, v1 = 100;
  int k2 = 20, v2 = 200;
  int k3 = 30, v3 = 300;
  chmap_insert(hm, k1, v1);
  chmap_insert(hm, k2, v2);
  chmap_insert(hm, k3, v3);

  int *p = chmap_get_ptr(hm, k1);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(*p, 100);
  *p = 999;
  REQUIRE_EQ(chmap_get(hm, k1), 999);

  // Other entries must be unaffected.
  REQUIRE_EQ(chmap_get(hm, k2), 200);
  REQUIRE_EQ(chmap_get(hm, k3), 300);

  // Absent key returns NULL.
  int absent = 99;
  REQUIRE_EQ((void *)chmap_get_ptr(hm, absent), (void *)NULL);

  chmap_destroy(hm);
}

// chmap_construct_scoped must automatically destroy an SC-backend map
// (char* -> int) when the enclosing block exits, without a manual
// chmap_destroy.
TEST(chash_maps, sc_construct_scoped_lifecycle) {
  {
    chmap_construct_scoped(hm, char *, int);
    REQUIRE_NE((void *)hm, NULL);

    int v1 = 10, v2 = 20, v3 = 30;
    chmap_insert(hm, "alpha", v1);
    chmap_insert(hm, "beta", v2);
    chmap_insert(hm, "gamma", v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, "alpha"), 10);
    REQUIRE_EQ(chmap_get(hm, "beta"), 20);
    REQUIRE_EQ(chmap_get(hm, "gamma"), 30);
    // hm is automatically destroyed at end of block (no chmap_destroy needed)
  }
}

// ccol_for_each must visit every element exactly once on the SC backend
// (char* -> int).  for_each_macro already covers the OA backend (int -> int).
TEST(chash_maps, sc_for_each_macro) {
  chmap_construct(hm, char *, int);
  REQUIRE_NE((void *)hm, NULL);

  int v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5;
  chmap_insert(hm, "a", v1);
  chmap_insert(hm, "b", v2);
  chmap_insert(hm, "c", v3);
  chmap_insert(hm, "d", v4);
  chmap_insert(hm, "e", v5);

  int sum = 0, count = 0;
  ccol_for_each(hm, it, {
    sum += *ccol_iter_val_ptr(it);
    ++count;
  });

  REQUIRE_EQ(count, 5);
  REQUIRE_EQ(sum, 15);  // 1+2+3+4+5

  chmap_destroy(hm);
}

// chmap_get_elem_copy must work correctly on the OA backend (int -> int).
// get_elem_copy_partial_buffer only covers the SC backend (ccol_other_types).
TEST(chash_maps, oa_get_elem_copy) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  int k1 = 10, v1 = 100;
  int k2 = 20, v2 = 200;
  chmap_insert(hm, k1, v1);
  chmap_insert(hm, k2, v2);

  int result = 0;
  REQUIRE_EQ(
      chmap_get_elem_copy(hm, &(cmap_pair){.ptr = &k1, .size = sizeof(k1)},
                          &result, sizeof(result)),
      ccol_success);
  REQUIRE_EQ(result, 100);

  result = 0;
  REQUIRE_EQ(
      chmap_get_elem_copy(hm, &(cmap_pair){.ptr = &k2, .size = sizeof(k2)},
                          &result, sizeof(result)),
      ccol_success);
  REQUIRE_EQ(result, 200);

  int k3 = 30;
  REQUIRE_EQ(
      chmap_get_elem_copy(hm, &(cmap_pair){.ptr = &k3, .size = sizeof(k3)},
                          &result, sizeof(result)),
      ccol_key_not_found);

  chmap_destroy(hm);
}

// After deleting elements from an OA-backend map whose capacity sits at the
// minimum (64 slots, so no rehash/tombstone-clear is triggered), the iterator
// must skip tombstone slots and visit only the remaining live entries.
TEST(chash_maps, oa_iterator_skips_tombstones) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  for (int i = 1; i <= 5; ++i) {
    int val = i * 10;
    chmap_insert(hm, i, val);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)5);

  // Delete keys 2 and 4.  Capacity stays at the 64-slot minimum so no rehash
  // fires and the deleted slots remain as tombstones in the slot array.
  int del1 = 2, del2 = 4, absent = 99;
  ccol_retval_t r = chmap_remove(hm, del1);
  REQUIRE_EQ(r, ccol_success);
  r = chmap_remove(hm, del2);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);

  // chmap_remove on a key that was never inserted must return
  // ccol_key_not_found.
  r = chmap_remove(hm, absent);
  REQUIRE_EQ(r, ccol_key_not_found);

  int key_sum = 0, val_sum = 0, count = 0;
  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    key_sum += *ccol_iter_key_ptr(it);
    val_sum += *ccol_iter_val_ptr(it);
    ++count;
  }
  REQUIRE_EQ(count, 3);
  REQUIRE_EQ(key_sum, 9);   // 1+3+5
  REQUIRE_EQ(val_sum, 90);  // 10+30+50

  chmap_destroy(hm);
}

// chmap_reset(hm, 0) must clear all elements but retain the current bucket
// array size on the SC backend (char* key), mirroring the behaviour already
// verified for the OA backend in oa_reset_zero_preserves_capacity.
TEST(chash_maps, sc_reset_zero_preserves_capacity) {
  chashmap *hm = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)hm, NULL);

  char key_buf[16];
  for (int i = 0; i < 10; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%d", i);
    REQUIRE_EQ(insert_string_to_int(hm, key_buf, i * 7), ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 10);

  size_t capacity_before = chmap_get_bucket_arr_size(hm);

  REQUIRE_EQ(chmap_reset(hm, 0), ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), 0);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), capacity_before);

  for (int i = 0; i < 5; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%d", i);
    REQUIRE_EQ(insert_string_to_int(hm, key_buf, i * 13), ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 5);
  for (int i = 0; i < 5; ++i) {
    int val;
    snprintf(key_buf, sizeof(key_buf), "key%d", i);
    REQUIRE_EQ(get_int_from_string(hm, key_buf, &val), ccol_success);
    REQUIRE_EQ(val, i * 13);
  }

  chmap_destroy(hm);
}

// float and double satisfy is_type_integral (size <= 8 bytes), so the OA
// backend is selected for float->int and double->int maps.  This exercises
// the Fibonacci hash path for ccol_float and ccol_double keys, along with
// full CRUD and iteration.
TEST(chash_maps, oa_float_and_double_keys) {
  {
    chmap_construct(hm, float, int);
    REQUIRE_NE((void *)hm, NULL);

    float k1 = 1.0f, k2 = 2.5f, k3 = -3.14f;
    int v1 = 10, v2 = 20, v3 = 30;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    chmap_insert(hm, k3, v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, k1), 10);
    REQUIRE_EQ(chmap_get(hm, k2), 20);
    REQUIRE_EQ(chmap_get(hm, k3), 30);

    int v_new = 999;
    chmap_insert(hm, k2, v_new);
    REQUIRE_EQ(chmap_get(hm, k2), 999);

    ccol_retval_t r = chmap_remove(hm, k1);
    REQUIRE_EQ(r, ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)2);
    REQUIRE_EQ((void *)chmap_get_ptr(hm, k1), (void *)NULL);

    int sum = 0, count = 0;
    ccol_iter_declare(hm, it);
    for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
      sum += *ccol_iter_val_ptr(it);
      ++count;
    }
    REQUIRE_EQ(count, 2);
    REQUIRE_EQ(sum, 999 + 30);

    chmap_destroy(hm);
  }

  {
    chmap_construct(hm, double, int);
    REQUIRE_NE((void *)hm, NULL);

    double k1 = 1.0, k2 = 3.14159, k3 = -2.71828;
    int v1 = 100, v2 = 200, v3 = 300;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    chmap_insert(hm, k3, v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, k1), 100);
    REQUIRE_EQ(chmap_get(hm, k2), 200);
    REQUIRE_EQ(chmap_get(hm, k3), 300);

    chmap_destroy(hm);
  }
}

// Negative zero and positive zero are `==` in C but have different object
// representations; this map's key equality is otherwise bitwise (see
// chashmap.c's canonicalize_key_pair_if_needed), so signed zero is
// canonicalized on the way in, making the two collide to a single key for
// both the OA (int value) and SC (char* value forces separate chaining)
// backends.
TEST(chash_maps, float_double_keys_negative_zero_canonicalized) {
  {
    // OA backend: float -> int
    chmap_construct(hm, float, int);

    float neg_zero = -0.0f;
    float pos_zero = 0.0f;
    int v1 = 111;
    chmap_insert(hm, neg_zero, v1);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
    REQUIRE_NE((void *)chmap_get_ptr(hm, pos_zero), (void *)NULL);
    REQUIRE_EQ(chmap_get(hm, pos_zero), 111);

    // Inserting through the other sign of zero updates the same key.
    int v2 = 222;
    chmap_insert(hm, pos_zero, v2);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
    REQUIRE_EQ(chmap_get(hm, neg_zero), 222);

    REQUIRE_EQ(chmap_remove(hm, pos_zero), ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

    chmap_destroy(hm);
  }

  {
    // OA backend: double -> int
    chmap_construct(hm, double, int);

    double neg_zero = -0.0;
    double pos_zero = 0.0;
    int v1 = 42;
    chmap_insert(hm, neg_zero, v1);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
    REQUIRE_EQ(chmap_get(hm, pos_zero), 42);

    chmap_destroy(hm);
  }

  {
    // SC backend: double key, char* value (char* value forces separate
    // chaining regardless of the key type).
    chmap_construct(hm, double, char *);

    double neg_zero = -0.0;
    double pos_zero = 0.0;
    chmap_insert(hm, neg_zero, "hello");

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
    REQUIRE_STREQ(chmap_get(hm, pos_zero), "hello");

    chmap_destroy(hm);
  }
}

// NaN is never `==` to anything, including itself, but a hash map still
// needs a reflexive key-equality relation to be usable at all: this map's
// floating-point key equality is bitwise (aside from the signed-zero
// canonicalization above), so a NaN key is always found again by the exact
// bit pattern it was inserted with, and two different NaN payloads remain
// distinct keys.
TEST(chash_maps, float_double_nan_keys_bitwise_identity) {
  {
    chmap_construct(hm, double, int);

    double nan_key = NAN;
    int v1 = 999;
    chmap_insert(hm, nan_key, v1);

    // The same bit pattern is always found.
    REQUIRE_NE((void *)chmap_get_ptr(hm, nan_key), (void *)NULL);
    REQUIRE_EQ(chmap_get(hm, nan_key), 999);

    // A different NaN payload is a different key.
    uint64_t other_nan_bits = 0x7FF8000000000001ULL;
    double other_nan;
    memcpy(&other_nan, &other_nan_bits, sizeof(other_nan));
    REQUIRE_TRUE(isnan(other_nan));

    int v2 = 1000;
    chmap_insert(hm, other_nan, v2);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)2);
    REQUIRE_EQ(chmap_get(hm, nan_key), 999);
    REQUIRE_EQ(chmap_get(hm, other_nan), 1000);

    chmap_destroy(hm);
  }
}

// Unlike float/double (exactly 4/8 bytes, no padding), long double's own
// in-memory representation carries platform-defined padding bits (e.g.
// 80-bit x87 extended precision stored in a 16-byte slot on this platform,
// leaving 6 padding bytes the C standard says nothing about). Key equality
// and hashing for long double is value-based (see hash_long_double_value/
// long_double_keys_equal in chashmap.c), specifically so two variables that
// hold the identical numeric value - constructed via completely different
// code paths, and therefore free to carry different, non-deterministic
// padding bits - are still found as the same key. This mirrors the -0.0/0.0
// unification float_double_keys_negative_zero_canonicalized already covers
// for float/double, extended to long double via a different mechanism (a
// native floating-point value comparison, not a byte-level canonicalization,
// since long double's padding bits cannot be reliably zeroed by this
// library in a portable way - see sc_inline_long_double_alignment's own
// comment for the empirical finding behind that constraint).
TEST(chash_maps, long_double_keys_padding_insensitive_equality) {
  chmap_construct(hm, long double, int);

  // Two long double variables holding the exact same numeric value via two
  // different construction paths (a literal vs. a runtime computation), the
  // scenario this test exists to cover: their padding bits are not
  // guaranteed identical, only their significant (value) bits are.
  long double a = 3.0L;
  volatile long double one = 1.0L, two = 2.0L;
  long double b = one + two;
  REQUIRE_EQ(a, b);

  int va = 111;
  chmap_insert(hm, a, va);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_NE((void *)chmap_get_ptr(hm, b), (void *)NULL);
  REQUIRE_EQ(chmap_get(hm, b), 111);

  // A deliberately byte-poked padding region must not change the key found:
  // construct a raw byte buffer holding the same value with every trailing
  // (padding) byte forced to a nonzero pattern, and confirm the raw
  // chmap_insert_elem/_get_elem_ref layer still treats it as the same key
  // as an ordinary, cleanly-constructed variable holding that value.
  //
  // Gated on LDBL_MANT_DIG == 64 (the x87 80-bit extended-precision layout:
  // 1 explicit sign bit + 15 exponent bits + 64 explicit mantissa bits = 10
  // significant bytes, padded out to sizeof(long double) == 12 or 16 with
  // genuinely unused bytes), not on a bare `sizeof(long double) > 10`: on
  // aarch64 (and every other platform whose long double is IEEE binary128,
  // LDBL_MANT_DIG == 113), sizeof(long double) is also 16, but EVERY one of
  // those 16 bytes is significant; there is no padding region left to poke
  // at all. Poking bytes 10-15 there does not merely fail to prove the
  // padding-insensitivity this test wants to demonstrate; it corrupts the
  // value's own mantissa/exponent bits into an entirely different number (a
  // NaN, confirmed directly: `poked_value` read back as -nan instead of
  // 3.0), which is what a bare size check let through as a false failure
  // before this fix, not a real defect in this library's own long-double key
  // comparison (chashmap.c compares long double keys by numeric VALUE via a
  // plain memcpy-then-== on the whole object, with no padding-byte
  // inspection anywhere, so it never depended on this assumption itself).
  if (LDBL_MANT_DIG == 64) {
    unsigned char poked[sizeof(long double)];
    memcpy(poked, &a, sizeof(a));
    for (size_t i = 10; i < sizeof(poked); i++) {
      poked[i] = 0xFF;
    }
    long double poked_value;
    memcpy(&poked_value, poked, sizeof(poked_value));
    REQUIRE_EQ(a, poked_value);

    cmap_pair poked_key = {.ptr = poked, .size = sizeof(long double)};
    cmap_pair *out = NULL;
    REQUIRE_EQ(chmap_get_elem_ref(hm, &poked_key, &out), ccol_success);
    int found;
    memcpy(&found, out->ptr, sizeof(found));
    REQUIRE_EQ(found, 111);
  }

  // A genuinely distinct value must not be conflated with it.
  long double c = 3.0001L;
  REQUIRE_EQ((void *)chmap_get_ptr(hm, c), (void *)NULL);

  chmap_destroy(hm);
}

// -0.0L and 0.0L are `==` in C exactly like -0.0/0.0; this map's long double
// key equality unifies them the same way float_double_keys_negative_zero_
// canonicalized already verifies for float/double (see this test's own
// sibling comment above and hash_long_double_value's dedicated zero branch).
TEST(chash_maps, long_double_keys_negative_zero_unified) {
  chmap_construct(hm, long double, int);

  long double neg_zero = -0.0L;
  long double pos_zero = 0.0L;
  int v1 = 42;
  chmap_insert(hm, neg_zero, v1);

  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_NE((void *)chmap_get_ptr(hm, pos_zero), (void *)NULL);
  REQUIRE_EQ(chmap_get(hm, pos_zero), 42);

  int v2 = 84;
  chmap_insert(hm, pos_zero, v2);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_EQ(chmap_get(hm, neg_zero), 84);

  chmap_destroy(hm);
}

// Unlike float/double (whose NaN keys are distinguished by exact bit
// pattern - see float_double_nan_keys_bitwise_identity above), every NaN
// long double collapses into a single key, and no payload/padding byte is
// ever read to decide this. This is a deliberate divergence from the
// float/double policy, forced by long double's own padding: a NaN long
// double's padding bytes are routinely genuine uninitialized memory in
// practice (a plain `long double n = NAN;` never writes them), so even
// reading them via memcmp - not just hashing them - is a real hazard, not
// merely a source of non-determinism (confirmed via valgrind while
// developing this behavior). hash_long_double_value/long_double_keys_equal
// both special-case isnan() before ever touching a raw byte, matching the
// same collapse-every-NaN-into-one-key precedent this codebase already
// established for the identical reason in cbstmap's own long double
// comparator (see cmp_float_val's doc comment in cbstmap.c).
TEST(chash_maps, long_double_nan_keys_collapse_to_one_key) {
  chmap_construct(hm, long double, int);

  long double nan_key = NAN;
  int v1 = 999;
  chmap_insert(hm, nan_key, v1);

  REQUIRE_NE((void *)chmap_get_ptr(hm, nan_key), (void *)NULL);
  REQUIRE_EQ(chmap_get(hm, nan_key), 999);

  unsigned char other_nan_bytes[sizeof(long double)];
  memcpy(other_nan_bytes, &nan_key, sizeof(nan_key));
  other_nan_bytes[0] ^= 0x01;  // flip a significant (non-padding) bit
  long double other_nan;
  memcpy(&other_nan, other_nan_bytes, sizeof(other_nan));
  REQUIRE_TRUE(isnan(other_nan));

  // A different NaN payload is still found as (and overwrites) the same
  // single NaN key, unlike float/double's distinct-payload policy.
  int v2 = 1000;
  chmap_insert(hm, other_nan, v2);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_EQ(chmap_get(hm, nan_key), 1000);
  REQUIRE_EQ(chmap_get(hm, other_nan), 1000);

  chmap_destroy(hm);
}

// Both signs of infinity must be usable as keys (hash_long_double_value has
// a dedicated branch for them specifically because frexpl's exponent output
// is unspecified for an infinite input, and converting an infinite value to
// an integer type is undefined behavior); they must also remain distinct
// from each other and from every finite key.
TEST(chash_maps, long_double_infinity_keys) {
  chmap_construct(hm, long double, int);

  long double pos_inf = INFINITY;
  long double neg_inf = -INFINITY;
  long double finite = 1.0L;
  int v1 = 1, v2 = 2, v3 = 3;
  chmap_insert(hm, pos_inf, v1);
  chmap_insert(hm, neg_inf, v2);
  chmap_insert(hm, finite, v3);

  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
  REQUIRE_EQ(chmap_get(hm, pos_inf), 1);
  REQUIRE_EQ(chmap_get(hm, neg_inf), 2);
  REQUIRE_EQ(chmap_get(hm, finite), 3);

  chmap_destroy(hm);
}

// Mirrors sc_float_key_size_mismatch_returns_invalid_args: long double keys
// now also get exact-size validation through the raw chmap_insert_elem/
// _get_elem_ref/_delete_elem layer (key_size_matches_type_if_fixed_width),
// since hash_long_double_value/long_double_keys_equal both trust key_size
// to be exactly sizeof(long double) and read that many bytes unconditionally
// - a caller-supplied undersized buffer must be rejected before either
// function ever reads past its end, rather than silently trusted the way a
// genuinely variable-length key type's size is.
TEST(chash_maps, long_double_key_size_mismatch_returns_invalid_args) {
  chmap_construct(hm, long double, int);
  REQUIRE_NE((void *)hm, NULL);

  uint8_t *tiny_key_buf = (uint8_t *)malloc(sizeof(double));
  REQUIRE_NE((void *)tiny_key_buf, NULL);
  double d = 1.0;
  memcpy(tiny_key_buf, &d, sizeof(d));

  cmap_pair bad_key = {.ptr = tiny_key_buf, .size = sizeof(double)};
  int val = 5;
  cmap_pair val_pair = {.ptr = &val, .size = sizeof(val)};

  REQUIRE_EQ(chmap_insert_elem(hm, &bad_key, &val_pair), ccol_invalid_args);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

  cmap_pair *out = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm, &bad_key, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_delete_elem(hm, &bad_key), ccol_invalid_args);

  // A correctly-sized key must still work normally afterwards.
  long double good_key = 1.0L;
  chmap_insert(hm, good_key, val);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_EQ(chmap_get(hm, good_key), 5);

  free(tiny_key_buf);
  chmap_destroy(hm);
}

// char, short, and long long all satisfy is_type_integral (size <= 8 bytes),
// so the OA backend is selected.  These types exercise hash paths not covered
// by the existing int / size_t / void* tests.
TEST(chash_maps, oa_char_and_short_and_long_long_keys) {
  {
    chmap_construct(hm, char, int);
    REQUIRE_NE((void *)hm, NULL);

    char k1 = 'A', k2 = 'B', k3 = 'Z';
    int v1 = 10, v2 = 20, v3 = 30;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    chmap_insert(hm, k3, v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, k1), 10);
    REQUIRE_EQ(chmap_get(hm, k2), 20);
    REQUIRE_EQ(chmap_get(hm, k3), 30);

    int v_updated = 999;
    chmap_insert(hm, k2, v_updated);
    REQUIRE_EQ(chmap_get(hm, k2), 999);

    ccol_retval_t r = chmap_remove(hm, k1);
    REQUIRE_EQ(r, ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)2);
    REQUIRE_EQ((void *)chmap_get_ptr(hm, k1), (void *)NULL);

    chmap_destroy(hm);
  }

  {
    chmap_construct(hm, short, int);
    REQUIRE_NE((void *)hm, NULL);

    short k1 = 100, k2 = -500, k3 = 32767;
    int v1 = 10, v2 = 20, v3 = 30;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    chmap_insert(hm, k3, v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, k1), 10);
    REQUIRE_EQ(chmap_get(hm, k2), 20);
    REQUIRE_EQ(chmap_get(hm, k3), 30);

    int count = 0;
    ccol_iter_declare(hm, it);
    for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
      ++count;
    }
    REQUIRE_EQ(count, 3);

    chmap_destroy(hm);
  }

  {
    chmap_construct(hm, long long, int);
    REQUIRE_NE((void *)hm, NULL);

    long long k1 = 0x1234567890ABCDEFLL, k2 = -1LL, k3 = (long long)9e18;
    int v1 = 10, v2 = 20, v3 = 30;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    chmap_insert(hm, k3, v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, k1), 10);
    REQUIRE_EQ(chmap_get(hm, k2), 20);
    REQUIRE_EQ(chmap_get(hm, k3), 30);

    chmap_destroy(hm);
  }
}

// Scalar `signed char` (ccol_signed_char, distinct from ccol_char) and its
// typedef int8_t must still satisfy is_type_integral/get_type_size the same
// way ccol_char already does, so the OA backend is selected here too and
// negative values hash/round-trip correctly.
TEST(chash_maps, oa_signed_char_and_int8_t_keys) {
  {
    chmap_construct(hm, signed char, int);
    REQUIRE_NE((void *)hm, NULL);

    signed char k1 = -128, k2 = -1, k3 = 127;
    int v1 = 10, v2 = 20, v3 = 30;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    chmap_insert(hm, k3, v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, k1), 10);
    REQUIRE_EQ(chmap_get(hm, k2), 20);
    REQUIRE_EQ(chmap_get(hm, k3), 30);

    int v_updated = 999;
    chmap_insert(hm, k2, v_updated);
    REQUIRE_EQ(chmap_get(hm, k2), 999);

    ccol_retval_t r = chmap_remove(hm, k1);
    REQUIRE_EQ(r, ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)2);
    REQUIRE_EQ((void *)chmap_get_ptr(hm, k1), (void *)NULL);

    chmap_destroy(hm);
  }

  {
    chmap_construct(hm, int8_t, int);
    REQUIRE_NE((void *)hm, NULL);

    int8_t k1 = -100, k2 = 0, k3 = 100;
    int v1 = 1, v2 = 2, v3 = 3;
    chmap_insert(hm, k1, v1);
    chmap_insert(hm, k2, v2);
    chmap_insert(hm, k3, v3);

    REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
    REQUIRE_EQ(chmap_get(hm, k1), 1);
    REQUIRE_EQ(chmap_get(hm, k2), 2);
    REQUIRE_EQ(chmap_get(hm, k3), 3);

    chmap_destroy(hm);
  }
}

// SC backend with both key and value as strings.  Verifies insert, update,
// get, iteration, and delete for char*->char* maps.
TEST(chash_maps, sc_string_to_string) {
  chmap_construct(hm, char *, char *);
  REQUIRE_NE((void *)hm, NULL);

  char *k1 = "apple", *k2 = "banana", *k3 = "cherry";
  char *v1 = "red", *v2 = "yellow", *v3 = "dark red";

  chmap_insert(hm, k1, v1);
  chmap_insert(hm, k2, v2);
  chmap_insert(hm, k3, v3);

  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
  REQUIRE_STREQ(chmap_get(hm, k1), "red");
  REQUIRE_STREQ(chmap_get(hm, k2), "yellow");
  REQUIRE_STREQ(chmap_get(hm, k3), "dark red");

  // Update an existing key's value.
  char *v2_new = "green";
  chmap_insert(hm, k2, v2_new);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
  REQUIRE_STREQ(chmap_get(hm, k2), "green");

  // All three entries must be visited exactly once.
  int count = 0;
  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    ++count;
  }
  REQUIRE_EQ(count, 3);

  // Delete one entry and confirm absence.
  REQUIRE_EQ(chmap_remove(hm, k1), ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)2);
  REQUIRE_EQ((void *)chmap_get_ptr(hm, k1), (void *)NULL);

  chmap_destroy(hm);
}

// Breaking out of an iteration loop early must not leak the iterator.
// Two paths are exercised: relying on the RAII _ccol_destructor to free the
// iterator when its block exits (SC backend), and calling ccol_iter_destroy
// explicitly before break (OA backend).  The map must be fully intact after
// each early exit.
TEST(chash_maps, ccol_iter_early_exit) {
  // SC backend: RAII cleanup on scope exit
  {
    chmap_construct(hm, char *, int);

    int v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5;
    chmap_insert(hm, "a", v1);
    chmap_insert(hm, "b", v2);
    chmap_insert(hm, "c", v3);
    chmap_insert(hm, "d", v4);
    chmap_insert(hm, "e", v5);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)5);

    int visited = 0;
    {
      ccol_iter_declare(hm, it);
      for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
        if (++visited == 2) break;
        // it is freed automatically by _ccol_destructor when this block exits
      }
    }
    REQUIRE_EQ(visited, 2);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)5);

    chmap_destroy(hm);
  }

  // OA backend: explicit ccol_iter_destroy before break
  {
    chmap_construct(hm, int, int);

    for (int i = 1; i <= 5; ++i) {
      int val = i * 10;
      chmap_insert(hm, i, val);
    }
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)5);

    int visited = 0;
    ccol_iter_declare(hm, it);
    for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
      if (++visited == 2) {
        ccol_iter_destroy(it);  // explicit destroy; RAII then no-ops on NULL
        break;
      }
    }
    REQUIRE_EQ(visited, 2);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)5);

    chmap_destroy(hm);
  }
}

// void* keys are typed as ccol_pointer and satisfy is_type_integral + size<=8,
// so the OA backend is selected.  This test exercises the Fibonacci hash path
// added for ccol_pointer keys in hash_key_data, along with full CRUD and
// iteration.
TEST(chash_maps, void_ptr_keys_oa_backend) {
  chmap_construct(hm, void *, int);
  REQUIRE_NE((void *)hm, NULL);

  void *k1 = (void *)0x1000;
  void *k2 = (void *)0x2000;
  void *k3 = (void *)0x3000;
  int v1 = 10, v2 = 20, v3 = 30;
  chmap_insert(hm, k1, v1);
  chmap_insert(hm, k2, v2);
  chmap_insert(hm, k3, v3);

  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
  REQUIRE_EQ(chmap_get(hm, k1), 10);
  REQUIRE_EQ(chmap_get(hm, k2), 20);
  REQUIRE_EQ(chmap_get(hm, k3), 30);

  // In-place update via chmap_get_ptr.
  int *p = chmap_get_ptr(hm, k2);
  REQUIRE_NE((void *)p, NULL);
  *p = 999;
  REQUIRE_EQ(chmap_get(hm, k2), 999);

  // Overwrite via upsert.
  int v3_new = 777;
  chmap_insert(hm, k3, v3_new);
  REQUIRE_EQ(chmap_get(hm, k3), 777);

  // Delete k1 and confirm absence.
  ccol_retval_t r = chmap_remove(hm, k1);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)2);
  REQUIRE_EQ((void *)chmap_get_ptr(hm, k1), (void *)NULL);

  // Removing an absent key must return ccol_key_not_found.
  void *absent = (void *)0xDEAD;
  r = chmap_remove(hm, absent);
  REQUIRE_EQ(r, ccol_key_not_found);

  // Iterate remaining two entries and verify aggregate values.
  int sum = 0, count = 0;
  ccol_iter_declare(hm, it);
  for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
    sum += *ccol_iter_val_ptr(it);
    ++count;
  }
  REQUIRE_EQ(count, 2);
  REQUIRE_EQ(sum, 999 + 777);

  chmap_destroy(hm);
}

TEST(chash_maps, chmap_as_a_struct_field) {
  // Use chmap_declare to declare hm as a member of a struct
  typedef struct {
    int a;
    long double b;
    char c[8];
    chmap_declare(hm, int, int);
  } tmp_struct;

  tmp_struct s;
  chmap_init(s.hm);

  for (int key = 0; key < 10; ++key) {
    int val = key + 1;
    chmap_insert(s.hm, key, val);
  }

  for (int key = 0; key < 10; ++key) {
    int val = chmap_get(s.hm, key);
    REQUIRE_EQ(val, key + 1);
  }

  chmap_destroy(s.hm);
}

TEST(chash_maps, chmap_as_a_struct_field_two_levels) {
  // Use chmap_declare to declare hm as a member of a struct
  typedef struct {
    int a;
    long double b;
    char c[8];
    chmap_declare(hm, int, int);
  } inner_struct;

  typedef struct {
    int d;
    inner_struct s;
  } tmp_struct;

  tmp_struct tmp;
  chmap_init(tmp.s.hm);

  for (int key = 0; key < 10; ++key) {
    int val = key + 1;
    chmap_insert(tmp.s.hm, key, val);
  }

  for (int key = 0; key < 10; ++key) {
    int val = chmap_get(tmp.s.hm, key);
    REQUIRE_EQ(val, key + 1);
  }

  chmap_destroy(tmp.s.hm);
}

TEST(chash_maps, chmap_as_a_struct_field_access_via_ptr) {
  // Use chmap_declare to declare hm as a member of a struct
  typedef struct {
    int a;
    long double b;
    char c[8];
    chmap_declare(hm, int, int);
  } tmp_struct;

  tmp_struct s_obj;
  tmp_struct *s = &s_obj;
  chmap_init(s->hm);

  for (int key = 0; key < 10; ++key) {
    int val = key + 1;
    chmap_insert(s->hm, key, val);
  }

  for (int key = 0; key < 10; ++key) {
    int val = chmap_get(s->hm, key);
    REQUIRE_EQ(val, key + 1);
  }

  chmap_destroy(s->hm);
}

TEST(chash_maps, chmap_as_a_struct_field_two_levels_access_via_ptr) {
  // Use chmap_declare to declare hm as a member of a struct
  typedef struct {
    int a;
    long double b;
    char c[8];
    chmap_declare(hm, int, int);
  } inner_struct;

  typedef struct {
    int d;
    inner_struct *s;
  } tmp_struct;

  inner_struct s_obj;
  tmp_struct tmp_obj;
  tmp_obj.s = &s_obj;
  tmp_struct *tmp = &tmp_obj;

  chmap_init(tmp->s->hm);

  for (int key = 0; key < 10; ++key) {
    int val = key + 1;
    chmap_insert(tmp->s->hm, key, val);
  }

  for (int key = 0; key < 10; ++key) {
    int val = chmap_get(tmp->s->hm, key);
    REQUIRE_EQ(val, key + 1);
  }

  chmap_destroy(tmp->s->hm);
}

// Forces every key into the same bucket with the exact same stored hash_val,
// so the sc_find_in_llist/sc_delete_from_llist hash_val pre-check can never
// short-circuit: every lookup must fall through to the full memcmp on every
// candidate in the chain to land on the right one.
static unsigned long constant_struct_key_hasher(const void *ptr) {
  (void)ptr;
  return 42;
}

TEST(chash_maps, sc_struct_key_full_hash_collision_still_resolves_correctly) {
  ccol_hashing_proc_t ch = &constant_struct_key_hasher;
  chmap_construct_ch(hm, helper_struct, int, ch);

  // helper_struct has 2 bytes of padding after b. memset first, then set
  // fields individually (a whole-struct assignment from a compound literal
  // would just copy its own uninitialised padding back in) so
  // sc_compare_keys' memcmp never reads uninitialised padding bytes.
  helper_struct k1, k2, k3, k4;
  memset(&k1, 0, sizeof(k1));
  memset(&k2, 0, sizeof(k2));
  memset(&k3, 0, sizeof(k3));
  memset(&k4, 0, sizeof(k4));
  k1.a = 1;
  k1.b = 10;
  k2.a = 2;
  k2.b = 20;
  k3.a = 3;
  k3.b = 30;
  k4.a = 4;
  k4.b = 40;

  int v1 = 100, v2 = 200, v3 = 300, v4 = 400;
  chmap_insert(hm, k1, v1);
  chmap_insert(hm, k2, v2);
  chmap_insert(hm, k3, v3);
  chmap_insert(hm, k4, v4);

  REQUIRE_EQ(chmap_elem_count(hm), (size_t)4);

  REQUIRE_EQ(chmap_get(hm, k1), 100);
  REQUIRE_EQ(chmap_get(hm, k2), 200);
  REQUIRE_EQ(chmap_get(hm, k3), 300);
  REQUIRE_EQ(chmap_get(hm, k4), 400);

  int v999 = 999;
  chmap_insert(hm, k2, v999);
  REQUIRE_EQ(chmap_get(hm, k2), 999);

  ccol_retval_t r = chmap_remove(hm, k3);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
  REQUIRE_EQ((void *)chmap_get_ptr(hm, k3), (void *)NULL);

  // The rest of the (still fully-colliding) chain must resolve correctly
  // after a deletion from the middle of it.
  REQUIRE_EQ(chmap_get(hm, k1), 100);
  REQUIRE_EQ(chmap_get(hm, k2), 999);
  REQUIRE_EQ(chmap_get(hm, k4), 400);

  chmap_destroy(hm);
}

// ========================================================================
// INVARIANT TESTS (randomized operation sequences)
// ========================================================================

extern size_t chmap_get_bucket_arr_size(chashmap *chmap);
extern size_t chmap_get_elem_count_to_scale_up(chashmap *chmap);

#define INVARIANTS_KEY_RANGE 200

// Random insert/remove against an int->int map (open-addressing backend)
// against a shadow reference model (a plain array indexed by key), checking
// after every op: elem_count never exceeds capacity or the scale-up
// threshold, and every key the shadow model says is present round-trips
// its exact shadow value via chmap_get_ptr (and every absent key returns
// NULL), i.e. the map and the reference model never disagree.
TEST(chash_maps, oa_invariants_random_ops) {
  ccol_invariants_rng_t rng;
  uint64_t seed = CCOL_INVARIANTS_DEFAULT_SEED;
  ccol_invariants_seed(&rng, seed);
  ccol_invariants_print_seed("chash_maps.oa_invariants_random_ops", seed);

  chmap_construct(hm, int, int);

  int shadow_value[INVARIANTS_KEY_RANGE];
  bool shadow_present[INVARIANTS_KEY_RANGE] = {0};

  const int num_ops = 3000;
  for (int i = 0; i < num_ops; ++i) {
    int key = (int)ccol_invariants_next_bounded(&rng, INVARIANTS_KEY_RANGE);
    bool do_insert = ccol_invariants_next_bounded(&rng, 2) == 0;

    if (do_insert) {
      int val = (int)ccol_invariants_next_bounded(&rng, 1000000);
      chmap_insert(hm, key, val);
      shadow_value[key] = val;
      shadow_present[key] = true;
    } else {
      chmap_remove(hm, key);
      shadow_present[key] = false;
    }

    size_t shadow_count = 0;
    for (int k = 0; k < INVARIANTS_KEY_RANGE; ++k) {
      if (shadow_present[k]) {
        int *ptr = chmap_get_ptr(hm, k);
        REQUIRE_NE((void *)ptr, NULL);
        REQUIRE_EQ(*ptr, shadow_value[k]);
        ++shadow_count;
      } else {
        REQUIRE_EQ((void *)chmap_get_ptr(hm, k), (void *)NULL);
      }
    }
    REQUIRE_EQ(chmap_elem_count(hm), shadow_count);
    REQUIRE_TRUE(chmap_elem_count(hm) <= chmap_get_bucket_arr_size(hm));
    // oa_insert's load-factor check runs BEFORE the insert completes, using
    // the pre-insert count; a single insert can therefore legitimately push
    // elem_count one past the "would trigger a resize" threshold until the
    // next op's own pre-check fires, so the real invariant allows a
    // one-element tolerance here, not a strict <=. Confirmed against
    // src/chashmap.c's oa_insert directly, not assumed: an earlier, stricter
    // version of this assertion (without the +1) failed, which is what
    // surfaced this design detail rather than a real library bug.
    REQUIRE_TRUE(chmap_elem_count(hm) <=
                 chmap_get_elem_count_to_scale_up(hm) + 1);
  }

  chmap_destroy(hm);
}

// Same reference-model approach as oa_invariants_random_ops, but with a
// char* key (forcing the separate-chaining backend) and cross-checked
// against a manual walk of the public iterator, not just chmap_elem_count.
TEST(chash_maps, sc_invariants_random_ops) {
  ccol_invariants_rng_t rng;
  uint64_t seed = CCOL_INVARIANTS_DEFAULT_SEED;
  ccol_invariants_seed(&rng, seed);
  ccol_invariants_print_seed("chash_maps.sc_invariants_random_ops", seed);

  chmap_construct(hm, char *, int);

  int shadow_value[INVARIANTS_KEY_RANGE];
  bool shadow_present[INVARIANTS_KEY_RANGE] = {0};
  char key_buf[16];

  const int num_ops = 1500;
  for (int i = 0; i < num_ops; ++i) {
    int key_id = (int)ccol_invariants_next_bounded(&rng, INVARIANTS_KEY_RANGE);
    snprintf(key_buf, sizeof(key_buf), "k%d", key_id);
    bool do_insert = ccol_invariants_next_bounded(&rng, 2) == 0;

    if (do_insert) {
      int val = (int)ccol_invariants_next_bounded(&rng, 1000000);
      chmap_insert(hm, key_buf, val);
      shadow_value[key_id] = val;
      shadow_present[key_id] = true;
    } else {
      chmap_remove(hm, key_buf);
      shadow_present[key_id] = false;
    }

    size_t shadow_count = 0;
    for (int k = 0; k < INVARIANTS_KEY_RANGE; ++k) {
      snprintf(key_buf, sizeof(key_buf), "k%d", k);
      if (shadow_present[k]) {
        int *ptr = chmap_get_ptr(hm, key_buf);
        REQUIRE_NE((void *)ptr, NULL);
        REQUIRE_EQ(*ptr, shadow_value[k]);
        ++shadow_count;
      } else {
        REQUIRE_EQ((void *)chmap_get_ptr(hm, key_buf), (void *)NULL);
      }
    }
    REQUIRE_EQ(chmap_elem_count(hm), shadow_count);

    // Cross-check against a manual walk of the public iterator too.
    size_t iter_count = 0;
    ccol_iter_declare(hm, it);
    for (it = ccol_begin(hm); it != NULL; it = ccol_iter_next(it)) {
      ++iter_count;
    }
    REQUIRE_EQ(iter_count, shadow_count);
  }

  chmap_destroy(hm);
}

// ========================================================================
// REGRESSION TESTS (chmap_reset oversized size / hash_key_data alignment /
// open-addressing size-mismatch validation)
// ========================================================================

// Regression: chmap_reset() must still destroy every existing element even
// when the requested new_bucket_array_size is too large to honor (rounds,
// via find_nearest_gte_power_of_two, above max_elem_count). Before the fix,
// this path returned ccol_not_enough_memory immediately without ever
// touching the map, contradicting chmap_reset's own documented contract
// ("All elements are destroyed regardless of return value" /
// "ccol_not_enough_memory if resize fails (elements still cleared)").
TEST(chash_maps, reset_with_oversized_size_still_clears_elements) {
  // OA backend
  {
    chmap_construct(hm, int, int);
    REQUIRE_NE((void *)hm, NULL);

    for (int i = 0; i < 10; ++i) {
      int val = i * 7;
      chmap_insert(hm, i, val);
    }
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)10);

    size_t capacity_before = chmap_get_bucket_arr_size(hm);

    REQUIRE_EQ(chmap_reset(hm, SIZE_MAX), ccol_not_enough_memory);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);
    // The requested size could not be honored, so the capacity is left
    // unchanged (equivalent to passing 0), but every element is still gone.
    REQUIRE_EQ(chmap_get_bucket_arr_size(hm), capacity_before);

    // The map remains fully usable afterwards.
    for (int i = 0; i < 10; ++i) {
      int val = i * 13;
      chmap_insert(hm, i, val);
    }
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)10);
    for (int i = 0; i < 10; ++i) {
      REQUIRE_EQ(chmap_get(hm, i), i * 13);
    }

    chmap_destroy(hm);
  }

  // SC backend
  {
    chashmap *hm = chmap_create(1, ccol_string, ccol_int, NULL);
    REQUIRE_NE((void *)hm, NULL);

    char key_buf[16];
    for (int i = 0; i < 10; ++i) {
      snprintf(key_buf, sizeof(key_buf), "key%d", i);
      REQUIRE_EQ(insert_string_to_int(hm, key_buf, i * 7), ccol_success);
    }
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)10);

    size_t capacity_before = chmap_get_bucket_arr_size(hm);

    REQUIRE_EQ(chmap_reset(hm, SIZE_MAX), ccol_not_enough_memory);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);
    REQUIRE_EQ(chmap_get_bucket_arr_size(hm), capacity_before);

    for (int i = 0; i < 5; ++i) {
      snprintf(key_buf, sizeof(key_buf), "key%d", i);
      REQUIRE_EQ(insert_string_to_int(hm, key_buf, i * 13), ccol_success);
    }
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)5);
    for (int i = 0; i < 5; ++i) {
      int val;
      snprintf(key_buf, sizeof(key_buf), "key%d", i);
      REQUIRE_EQ(get_int_from_string(hm, key_buf, &val), ccol_success);
      REQUIRE_EQ(val, i * 13);
    }

    chmap_destroy(hm);
  }
}

// Regression/coverage: chmap_insert_elem, chmap_get_elem_ref, and
// chmap_delete_elem must reject a key/value whose size does not exactly
// match the open-addressing backend's fixed key_size/val_size, even when
// the pointer is non-NULL and the size is non-zero. The pre-existing
// insert_and_ref_invalid_args test only covers the separate-chaining
// backend (a char* key) and only NULL/zero-size cases, leaving
// key_size_matches_type_if_fixed_width/oa_val_size_matches entirely
// untested.
TEST(chash_maps, oa_size_mismatch_returns_invalid_args) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  int key = 1, val = 100;
  short wrong_size_key = 1;
  short wrong_size_val = 100;

  cmap_pair good_key = {.ptr = &key, .size = sizeof(key)};
  cmap_pair good_val = {.ptr = &val, .size = sizeof(val)};
  cmap_pair bad_key = {.ptr = &wrong_size_key, .size = sizeof(wrong_size_key)};
  cmap_pair bad_val = {.ptr = &wrong_size_val, .size = sizeof(wrong_size_val)};

  REQUIRE_EQ(chmap_insert_elem(hm, &bad_key, &good_val), ccol_invalid_args);
  REQUIRE_EQ(chmap_insert_elem(hm, &good_key, &bad_val), ccol_invalid_args);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

  REQUIRE_EQ(chmap_insert_elem(hm, &good_key, &good_val), ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);

  cmap_pair *out = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm, &bad_key, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_delete_elem(hm, &bad_key), ccol_invalid_args);

  // The valid entry must still be present and intact after every rejection.
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_EQ(chmap_get(hm, key), 100);

  chmap_destroy(hm);
}

// Regression: key-size validation used to be applied only for the
// open-addressing backend (oa_key_size_matches). hash_key_data() dispatches
// on key_type alone and reads a FIXED number of bytes (sizeof of the
// corresponding C type) from the caller-supplied key pointer for any
// fixed-width numeric key type, completely ignoring key_pair->size when
// deciding how many bytes to read. A separate-chaining map with an
// integral key type - reachable whenever that key type is paired with a
// non-integral value type, e.g. int->char*, which forces separate chaining
// regardless of the key type itself - had no way to reject a key_pair
// whose declared size understates the true size of its own key type, so
// chmap_insert_elem/_get_elem_ref/_delete_elem would silently read past
// the end of a caller-supplied buffer smaller than that type's true size.
// This test allocates an exactly-2-byte heap buffer for what the map
// declares as an int (4-byte) key, so a regression reading even one byte
// past it is caught directly by valgrind/ASan, not merely by an incorrect
// return value.
TEST(chash_maps, sc_integral_key_size_mismatch_returns_invalid_args) {
  // int key + char* value forces the separate-chaining backend even though
  // the key type itself is integral.
  chmap_construct(hm, int, char *);
  REQUIRE_NE((void *)hm, NULL);

  uint8_t *tiny_key_buf = (uint8_t *)malloc(2);
  REQUIRE_NE((void *)tiny_key_buf, NULL);
  tiny_key_buf[0] = 5;
  tiny_key_buf[1] = 0;

  cmap_pair bad_key = {.ptr = tiny_key_buf, .size = 2};
  cmap_pair val = {.ptr = (void *)"hello", .size = strlen("hello") + 1};

  REQUIRE_EQ(chmap_insert_elem(hm, &bad_key, &val), ccol_invalid_args);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

  cmap_pair *out = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm, &bad_key, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_delete_elem(hm, &bad_key), ccol_invalid_args);

  // A correctly-sized key must still work normally afterwards.
  int good_key = 5;
  chmap_insert(hm, good_key, "hello");
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_STREQ(chmap_get(hm, good_key), "hello");

  free(tiny_key_buf);
  chmap_destroy(hm);
}

// Same hazard, but through canonicalize_key_pair_if_needed's float/double
// canonicalization path, which trusted any key_pair->size <= 8 without
// verifying it was exactly the true 4-byte float / 8-byte double size, and
// then mem_cpy'd that many (caller-claimed) bytes out of the caller's
// pointer. A float-keyed separate-chaining map, given a key_pair claiming
// size 8 (as if it were a double) backed by an exactly-4-byte allocation,
// used to read 4 bytes past the end of that allocation.
TEST(chash_maps, sc_float_key_size_mismatch_returns_invalid_args) {
  // float key + char* value forces the separate-chaining backend even
  // though the key type itself is integral (fixed-width numeric).
  chmap_construct(hm, float, char *);
  REQUIRE_NE((void *)hm, NULL);

  uint8_t *tiny_key_buf = (uint8_t *)malloc(sizeof(float));
  REQUIRE_NE((void *)tiny_key_buf, NULL);
  float f = 1.0f;
  memcpy(tiny_key_buf, &f, sizeof(f));

  cmap_pair bad_key = {.ptr = tiny_key_buf, .size = sizeof(double)};
  cmap_pair val = {.ptr = (void *)"world", .size = strlen("world") + 1};

  REQUIRE_EQ(chmap_insert_elem(hm, &bad_key, &val), ccol_invalid_args);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

  cmap_pair *out = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm, &bad_key, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_delete_elem(hm, &bad_key), ccol_invalid_args);

  // A correctly-sized key must still work normally afterwards.
  float good_key = 1.0f;
  chmap_insert(hm, good_key, "world");
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_STREQ(chmap_get(hm, good_key), "world");

  free(tiny_key_buf);
  chmap_destroy(hm);
}

// Regression: hash_key_data's ccol_short/ccol_int/ccol_long/ccol_long_long
// branches used to read the key via a direct pointer-cast dereference (e.g.
// *(uint32_t*)key_ptr) instead of a memcpy-based read, unlike the
// ccol_float/ccol_double/ccol_pointer branches in the very same function.
// Dereferencing a multi-byte-typed pointer that is not naturally aligned
// for that type is UB and can fault on strict-alignment architectures.
// This is reachable through the raw chmap_insert_elem/_get_elem_ref/
// _delete_elem function layer, whose cmap_pair.ptr carries no alignment
// guarantee of its own; unlike the type-safe macros, which always take
// the address of a naturally-aligned local variable. This test drives the
// raw API directly with key pointers deliberately misaligned via a plain
// byte buffer (never a packed-struct member address, which would itself
// trigger -Waddress-of-packed-member under this project's -Werror). Not
// reproducible as a wrong VALUE on this platform's relaxed-alignment ISA
// (only as UB under -fsanitize=undefined or on a strict-alignment
// architecture), so this test's role is to keep the misaligned-access
// pattern exercised for that sanitizer coverage, alongside asserting the
// ordinary round-trip still succeeds.
TEST(chash_maps, oa_misaligned_key_pointers_hash_correctly) {
  unsigned char raw[64];
  memset(raw, 0, sizeof(raw));

  short s_val = (short)12345;
  memcpy(raw + 1, &s_val,
         sizeof(s_val));  // offset 1: misaligned for short (align 2)

  int i_val = (int)0x1234ABCD;
  memcpy(raw + 3, &i_val,
         sizeof(i_val));  // offset 3: misaligned for int (align 4)

  long l_val = (long)0x6EEDDBB0L;
  memcpy(raw + 5, &l_val, sizeof(l_val));  // offset 5: misaligned for long

  long long ll_val = (long long)0x1234567890ABCDEFLL;
  memcpy(raw + 9, &ll_val,
         sizeof(ll_val));  // offset 9: misaligned for long long (align 8)

  // short
  {
    chmap_construct(hm, short, int);
    int val = 111;
    REQUIRE_EQ(chmap_insert_elem(
                   hm, &(cmap_pair){.ptr = raw + 1, .size = sizeof(s_val)},
                   &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
               ccol_success);

    cmap_pair *out = NULL;
    REQUIRE_EQ(
        chmap_get_elem_ref(
            hm, &(cmap_pair){.ptr = raw + 1, .size = sizeof(s_val)}, &out),
        ccol_success);
    int got;
    memcpy(&got, out->ptr, sizeof(got));
    REQUIRE_EQ(got, 111);

    REQUIRE_EQ(chmap_delete_elem(
                   hm, &(cmap_pair){.ptr = raw + 1, .size = sizeof(s_val)}),
               ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

    chmap_destroy(hm);
  }

  // int
  {
    chmap_construct(hm, int, int);
    int val = 222;
    REQUIRE_EQ(chmap_insert_elem(
                   hm, &(cmap_pair){.ptr = raw + 3, .size = sizeof(i_val)},
                   &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
               ccol_success);

    cmap_pair *out = NULL;
    REQUIRE_EQ(
        chmap_get_elem_ref(
            hm, &(cmap_pair){.ptr = raw + 3, .size = sizeof(i_val)}, &out),
        ccol_success);
    int got;
    memcpy(&got, out->ptr, sizeof(got));
    REQUIRE_EQ(got, 222);

    REQUIRE_EQ(chmap_delete_elem(
                   hm, &(cmap_pair){.ptr = raw + 3, .size = sizeof(i_val)}),
               ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

    chmap_destroy(hm);
  }

  // long
  {
    chmap_construct(hm, long, int);
    int val = 333;
    REQUIRE_EQ(chmap_insert_elem(
                   hm, &(cmap_pair){.ptr = raw + 5, .size = sizeof(l_val)},
                   &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
               ccol_success);

    cmap_pair *out = NULL;
    REQUIRE_EQ(
        chmap_get_elem_ref(
            hm, &(cmap_pair){.ptr = raw + 5, .size = sizeof(l_val)}, &out),
        ccol_success);
    int got;
    memcpy(&got, out->ptr, sizeof(got));
    REQUIRE_EQ(got, 333);

    REQUIRE_EQ(chmap_delete_elem(
                   hm, &(cmap_pair){.ptr = raw + 5, .size = sizeof(l_val)}),
               ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

    chmap_destroy(hm);
  }

  // long long
  {
    chmap_construct(hm, long long, int);
    int val = 444;
    REQUIRE_EQ(chmap_insert_elem(
                   hm, &(cmap_pair){.ptr = raw + 9, .size = sizeof(ll_val)},
                   &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
               ccol_success);

    cmap_pair *out = NULL;
    REQUIRE_EQ(
        chmap_get_elem_ref(
            hm, &(cmap_pair){.ptr = raw + 9, .size = sizeof(ll_val)}, &out),
        ccol_success);
    int got;
    memcpy(&got, out->ptr, sizeof(got));
    REQUIRE_EQ(got, 444);

    REQUIRE_EQ(chmap_delete_elem(
                   hm, &(cmap_pair){.ptr = raw + 9, .size = sizeof(ll_val)}),
               ccol_success);
    REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

    chmap_destroy(hm);
  }
}

// Regression: sc_set_scaling_limits must compute the exact "(bucket_count +
// 1) * 1.5" / "(bucket_count + 1) / 8" formulas chashmap.h documents for
// the separate-chaining backend, not the (undocumented, off-by-a-fraction)
// "bucket_count * 1.5" / "bucket_count / 8" the implementation previously
// used.
TEST(chash_maps, sc_scaling_thresholds_match_documented_formula) {
  chashmap *hm = chmap_create(16, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)hm, NULL);

  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);
  // (16 + 1) * 1.5 = 25.5 -> floor -> 25
  REQUIRE_EQ(chmap_get_elem_count_to_scale_up(hm), (size_t)25);
  // (16 + 1) / 8 = 2 (integer division)
  REQUIRE_EQ(chmap_get_elem_count_to_scale_down(hm), (size_t)2);

  chmap_destroy(hm);
}

// Regression: sc_scale's down-shrink used to be gated on bucket_arr_size >=
// (scale_factor * minimum_allowed_bucket_array_size) (64), so a bucket array
// that ever landed off the "minimum_allowed_bucket_array_size * 4^k"
// lineage - e.g. 32, directly reachable from any initial_bucket_array_size
// in (16, 64) - could never shrink again, even after every element was
// deleted: 32 / 4 == 8 is below the documented floor of 16, so the old
// guard (32 >= 64) skipped the shrink entirely instead of clamping to 16,
// permanently leaving the map at double the documented minimum bucket
// count.
TEST(chash_maps, sc_bucket_array_shrinks_to_minimum_from_off_lineage_size) {
  // 20 rounds up to 32 via find_nearest_gte_power_of_two, which is not on
  // the 16 * 4^k lineage the automatic 4x/0.25x scaling alone ever produces.
  chashmap *hm = chmap_create(20, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)hm, NULL);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)32);

  char key_buf[16];
  for (int i = 0; i < 5; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%d", i);
    REQUIRE_EQ(insert_string_to_int(hm, key_buf, i), ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)5);

  // elem_count_to_scale_down(32) == (32 + 1) / 8 == 4, so deleting down to
  // 3 elements crosses the shrink threshold.
  for (int i = 0; i < 2; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%d", i);
    REQUIRE_EQ(delete_int_from_string(hm, key_buf), ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)3);
  // Before the fix this stayed at 32 forever; the fix clamps the 32 / 4 ==
  // 8 undershoot up to the documented floor instead.
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  // Deleting the rest must not misbehave at the floor, and the map must
  // remain fully usable afterwards.
  for (int i = 2; i < 5; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%d", i);
    REQUIRE_EQ(delete_int_from_string(hm, key_buf), ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  REQUIRE_EQ(insert_string_to_int(hm, "after", 42), ccol_success);
  int val = -1;
  REQUIRE_EQ(get_int_from_string(hm, "after", &val), ccol_success);
  REQUIRE_EQ(val, 42);

  chmap_destroy(hm);
}

// Regression: oa_reset() used to return ccol_not_enough_memory without
// touching the map at all when either of its two internal allocations
// failed, leaving every pre-existing element completely intact - directly
// contradicting chmap_reset's own documented contract ("All elements are
// destroyed regardless of return value" / "ccol_not_enough_memory if resize
// fails (elements still cleared)"). reset_with_oversized_size_still_clears_
// elements only exercises chmap_reset's own SIZE_MAX pre-check, which
// degrades to "keep current capacity" and routes around oa_reset's real
// allocation calls entirely, always succeeding; this test drives a genuine
// calloc failure at a perfectly legitimate requested size via the same
// fault-injecting allocator oa_insert_sustained_oom_reports_not_enough_
// memory already uses.
TEST(chash_maps, oa_reset_allocation_failure_still_clears_elements) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = controlled_calloc,
                             .realloc = realloc};

  char *err = NULL;
  chashmap *hm = chmap_create_mp(1, ccol_int, ccol_int, &mp, &err);
  REQUIRE_NE((void *)hm, NULL);

  for (int i = 0; i < 10; ++i) {
    int val = i * 3;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)10);
  size_t capacity_before = chmap_get_bucket_arr_size(hm);

  // A perfectly legitimate resize request (not chmap_reset's own "too big"
  // pre-filter case), but every calloc call now fails.
  g_calloc_fail = true;
  REQUIRE_EQ(chmap_reset(hm, 64), ccol_not_enough_memory);
  g_calloc_fail = false;

  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);
  // The requested capacity could not be honored, so the map keeps its old
  // capacity, but every element must still be gone.
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), capacity_before);

  // The map remains fully usable afterwards.
  for (int i = 0; i < 10; ++i) {
    int k = 100 + i;
    int val = i * 7;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &k, .size = sizeof(k)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)10);
  for (int i = 0; i < 10; ++i) {
    int k = 100 + i;
    int retrieved = -1;
    REQUIRE_EQ(
        chmap_get_elem_copy(hm, &(cmap_pair){.ptr = &k, .size = sizeof(k)},
                            &retrieved, sizeof(retrieved)),
        ccol_success);
    REQUIRE_EQ(retrieved, i * 7);
  }

  chmap_destroy(hm);
}
