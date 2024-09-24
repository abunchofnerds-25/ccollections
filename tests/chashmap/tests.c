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

  chmap_iter_declare(chmap, it);
  for (it = chmap_begin(chmap); it != NULL; it = chmap_iter_next(it)) {
    // *(int*)(it->val_pair->ptr) += 7;
    *chmap_iter_val_ptr(it) += 7;
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
  chmap_iter_declare(chmap, it);
  for (it = chmap_begin(chmap); it != NULL; it = chmap_iter_next(it)) {
    sum += *chmap_iter_val_ptr(it);
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
  chmap_iter_declare(hm, it);
  for (it = chmap_begin(hm); it != NULL; it = chmap_iter_next(it)) {
    key = *chmap_iter_key_ptr(it);
    if (key == 3) {
      REQUIRE_EQ(records[3]++, 0);
      REQUIRE_STREQ(*chmap_iter_val_ptr(it), "hello");
    } else if (key == 4) {
      REQUIRE_EQ(records[4]++, 0);
      REQUIRE_STREQ(*chmap_iter_val_ptr(it), "hi");
    } else if (key == 5) {
      REQUIRE_EQ(records[5]++, 0);
      REQUIRE_STREQ(*chmap_iter_val_ptr(it), "there");
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

  chmap_iter_declare(hm, it);
  for (it = chmap_begin(hm); it != NULL; it = chmap_iter_next(it)) {
    if (strcmp(*chmap_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->a, 3);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->b, 4);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->a, 5);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->b, 6);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->a, 7);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->b, 8);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d4") == 0) {
      REQUIRE_EQ(records[3]++, 0);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->a, 9);
      REQUIRE_EQ((*(some_struct **)chmap_iter_val_ptr(it))->b, 10);
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

  chmap_iter_declare(hm, it);
  for (it = chmap_begin(hm); it != NULL; it = chmap_iter_next(it)) {
    if (strcmp(*chmap_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ((*chmap_iter_val_ptr(it))->a, 3);
      REQUIRE_EQ((*chmap_iter_val_ptr(it))->b, 4);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ((*chmap_iter_val_ptr(it))->a, 5);
      REQUIRE_EQ((*chmap_iter_val_ptr(it))->b, 6);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ((*chmap_iter_val_ptr(it))->a, 7);
      REQUIRE_EQ((*chmap_iter_val_ptr(it))->b, 8);
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

  chmap_iter_declare(hm, it);
  for (it = chmap_begin(hm); it != NULL; it = chmap_iter_next(it)) {
    if (strcmp(*chmap_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 3);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 4);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 5);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 6);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 7);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 8);
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

  for (it = chmap_begin(hm); it != NULL; it = chmap_iter_next(it)) {
    if (strcmp(*chmap_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 4);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 3);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 6);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 5);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 8);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 7);
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

  chmap_iter_declare(chm, it);
  for (it = chmap_begin(chm); it != NULL; it = chmap_iter_next(it)) {
    if (strcmp(*chmap_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 3);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 4);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 5);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 6);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 7);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 8);
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
  chmap_for_each(hm, it, {
    sum += *chmap_iter_val_ptr(it);
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
    chmap_iter_declare(hm, iter);
    for (iter = chmap_begin(hm); iter != NULL; iter = chmap_iter_next(iter)) {
      sum += *chmap_iter_val_ptr(iter);
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
    chmap_iter_declare(hm, iter);
    for (iter = chmap_begin(hm); iter != NULL; iter = chmap_iter_next(iter)) {
      key_sum += *chmap_iter_key_ptr(iter);
      val_sum += *chmap_iter_val_ptr(iter);
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

// Regression: oa_insert returned ccol_container_full when the probe wrapped
// all the way around without finding a truly-empty slot, even though tombstone
// (deleted) slots were available for reuse. The post-loop tombstone reuse path
// is only reachable when all 64 slots are either live or tombstoned, which
// requires the rehash calloc to fail (OOM). We simulate that here.
TEST(chash_maps, oa_tombstone_reuse_after_full_probe_wrap) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = controlled_calloc,
                             .realloc = realloc};

  // Use the minimum capacity (64) so that oa_delete's shrink guard
  // (capacity > minimum_allowed_bucket_array_size) never fires and tombstones
  // accumulate without a rehash clearing them.
  char *err = NULL;
  chashmap *hm = chmap_create_mp(1, ccol_int, ccol_int, &mp, &err);
  REQUIRE_NE((void *)hm, NULL);

  // Make all subsequent calloc calls fail so rehash can never enlarge the map.
  // Inserts k0..k63 still succeed because, even though the load-factor check
  // (count+deleted)/64 > 0.70 fires from key 45 onwards, the failed rehash
  // leaves the original 64-slot array in place and there are still empty slots
  // for the probe to find.
  g_calloc_fail = true;
  for (int i = 0; i < 64; i++) {
    int val = i * 10;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 64);

  // Delete 10 keys — no calloc involved, and at capacity == minimum the shrink
  // check in oa_delete is suppressed, so these become tombstones in place.
  for (int i = 0; i < 10; i++) {
    REQUIRE_EQ(
        chmap_delete_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 54);

  // All 64 slots are now either live (54) or tombstoned (10). Inserting key 64
  // triggers the load check ((54+10)/64 == 1.0 > 0.70), which tries to rehash,
  // which calloc-fails, leaving the map unchanged. The probe then visits every
  // slot without finding an empty one. The post-loop fix reuses the first
  // tombstone slot it recorded instead of returning ccol_container_full.
  int new_key = 64, new_val = 640;
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = &new_key, .size = sizeof(new_key)},
                 &(cmap_pair){.ptr = &new_val, .size = sizeof(new_val)}),
             ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), 55);

  int retrieved = 0;
  REQUIRE_EQ(chmap_get_elem_copy(
                 hm, &(cmap_pair){.ptr = &new_key, .size = sizeof(new_key)},
                 &retrieved, sizeof(retrieved)),
             ccol_success);
  REQUIRE_EQ(retrieved, 640);

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
  chmap_iter_declare(hm, it);
  for (it = chmap_begin(hm); it != NULL; it = chmap_iter_next(it)) {
    ++visited;
  }
  REQUIRE_EQ(visited, (size_t)3);

  chmap_destroy(hm);
}

TEST(chash_maps, re_enabled_local_chm_macros) {
  chmap_construct(hm, char *, helper_struct);

  helper_function(hm);

  int records[3] = {0};
  int counter = 0;

  chmap_iter_declare(hm, it);
  for (it = chmap_begin(hm); it != NULL; it = chmap_iter_next(it)) {
    if (strcmp(*chmap_iter_key_ptr(it), "d1") == 0) {
      REQUIRE_EQ(records[0]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 4);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 3);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d2") == 0) {
      REQUIRE_EQ(records[1]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 6);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 5);
    } else if (strcmp(*chmap_iter_key_ptr(it), "d3") == 0) {
      REQUIRE_EQ(records[2]++, 0);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->a, 8);
      REQUIRE_EQ(chmap_iter_val_ptr(it)->b, 7);
    } else {
      REQUIRE_TRUE(false);
    }
    ++counter;
  }

  REQUIRE_EQ(counter, 3);

  chmap_destroy(hm);
}
