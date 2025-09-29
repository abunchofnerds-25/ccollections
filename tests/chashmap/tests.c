#include <chashmap.h>
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

  long l = 0xffeeddbbccaa1100;
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
    snprintf(key_buf, sizeof(key_buf), "key%lu", i);
    REQUIRE_EQ(insert_string_to_int(chmap, key_buf, i + 1), ccol_success);
  }

  REQUIRE_EQ(chmap_elem_count(chmap), first_up_threshold + 1);

  REQUIRE_TRUE(first_up_threshold < chmap_get_elem_count_to_scale_up(chmap));
  REQUIRE_TRUE(first_down_threshold <
               chmap_get_elem_count_to_scale_down(chmap));
  REQUIRE_TRUE(first_capacity < chmap_get_bucket_arr_size(chmap));

  for (size_t i = 0; i <= first_up_threshold; ++i) {
    snprintf(key_buf, sizeof(key_buf), "key%lu", i);
    delete_int_from_string(chmap, key_buf);
  }

  REQUIRE_EQ(chmap_elem_count(chmap), 0);

  REQUIRE_EQ(first_up_threshold, chmap_get_elem_count_to_scale_up(chmap));
  REQUIRE_EQ(first_down_threshold, chmap_get_elem_count_to_scale_down(chmap));
  REQUIRE_EQ(first_capacity, chmap_get_bucket_arr_size(chmap));

  chmap_destroy(chmap);
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
    snprintf(key_buf, sizeof(key_buf), "k%lu", i);
    REQUIRE_EQ(insert_string_to_int(chmap, key_buf, i + 1), ccol_success);
  }

  REQUIRE_EQ(chmap_elem_count(chmap), max_elems + 1);

  for (size_t i = 0; i <= max_elems; ++i) {
    snprintf(key_buf, sizeof(key_buf), "k%lu", i);
    ccol_retval_t r = delete_int_from_string(chmap, key_buf);
    if (r != ccol_success) {
      printf("Failed to delete %s - i:%lu - r: %d\n", key_buf, i, r);
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

  // Delete 10 keys — no calloc involved, and at capacity == minimum the shrink
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

// Regression: in the OA backend, val_size was re-written on every insert when
// count == 0. This meant deleting all elements and reinserting could silently
// mutate val_size. The fix gates the update on a val_size_initialized flag so
// it only fires once per map lifetime (or once after an explicit reset).
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

  // unsigned char * (= uint8_t *) — identical check.
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
  // but integer literals are not -- use variables.
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
