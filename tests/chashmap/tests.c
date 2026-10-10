#include <chashmap.h>
#include <common_invariants.h>
#include <float.h>
#include <internal/chashkey.h>
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

extern size_t _ccol_find_nearest_gte_power_of_two(size_t input);

TEST(chash_maps, _ccol_find_nearest_gte_power_of_two) {
  // The input array and the expected output array below are written by hand,
  // so this test needs no math library.
  //
  // The doc comment of _ccol_find_nearest_gte_power_of_two in src/common.c
  // states that its result depends on the architecture: it is capped at the
  // maximum for the pointer size, and the function gives ccol_invalid_size
  // once the input goes above the largest power of two that a size_t can
  // hold. The 64-bit table below tests all the way up to SIZE_MAX on a 64-bit
  // size_t, but a 32-bit size_t cannot hold several of its literal values (for
  // example 4294967296 and every value above it), so a 32-bit build
  // needs its own shorter table, which stops at the 32-bit SIZE_MAX and
  // moves the boundary-overflow case down to 2^31.
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
    size_t r = _ccol_find_nearest_gte_power_of_two(test_inputs[i]);
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
  const cmap_pair *tmp_val_pair_ptr = NULL;
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
  const cmap_pair *target_pair = NULL;

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

  // The C standard only guarantees that a long has 32 bits or more: a long is
  // 4 bytes on an ILP32 platform such as i386, and 8 bytes on LP64 x86_64, so
  // this value must fit in the smaller of those two widths to be portable.
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

/* This has the same shape as the real use in cyaml.c: the values are pointers
 * to heap memory (here to a plain int) that the destructor must free, exactly
 * as __cyaml_destroy frees a child node. */
static void free_int_ptr_dtor(const cmap_pair *val_pair, void *ctx) {
  destroy_dtor_ctx_t *c = (destroy_dtor_ctx_t *)ctx;
  int *p;
  memcpy(&p, val_pair->ptr, sizeof(p));
  c->call_count++;
  c->sum_of_values += *p;
  free(p);
}

/* An open-addressing map stores a value inline, not a pointer to free, so
 * this destructor only counts and adds, and the test uses it instead of
 * free_int_ptr_dtor. */
static void count_int_value_dtor(const cmap_pair *val_pair, void *ctx) {
  destroy_dtor_ctx_t *c = (destroy_dtor_ctx_t *)ctx;
  int v;
  memcpy(&v, val_pair->ptr, sizeof(v));
  c->call_count++;
  c->sum_of_values += v;
}

TEST(chash_maps, destroy_with_dtor_invokes_once_per_value_sc) {
  /* Separate chaining with string keys, the same shape as the
   * char* -> pointer dictionaries of cyaml.c. */
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
  /* Open-addressing, where the key and the value are both integral: the other
   * backend, which the test covers for completeness, although cyaml.c itself
   * never reaches this path, because its dictionary keys are always char*. */
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

/* destroy_with_dtor_invokes_once_per_value_oa above deletes nothing, so it
 * can never reach the SLOT_DELETED skip of chmap_destroy_with_dtor on the
 * open-addressing backend: every slot that it walks is genuinely empty or
 * live, and never a tombstone. This test deletes half of the keys first, so
 * that the slot array really holds tombstoned slots when
 * chmap_destroy_with_dtor walks it, and confirms that the destructor runs
 * only for the keys that survive, instead of one time for each slot that is
 * occupied or tombstoned. */
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

  /* Delete the even keys, so their slots become tombstones, which carry
   * SLOT_OCCUPIED | SLOT_DELETED, and only the values of the odd keys stay
   * live. */
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
  /* The call must not crash, and it must really free the map; valgrind checks
   * this under make memtest, and no assertion here checks it. */
  chmap_destroy_with_dtor(m, NULL, NULL);
}

TEST(chash_maps, destroy_with_dtor_null_map_is_noop) {
  /* The call must not crash, whatever val_dtor is. */
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
             ccol_key_not_found);  // This must have no side effect
  REQUIRE_EQ(delete_int_from_string(chmap, ""),
             ccol_invalid_args);  // This must have no side effect

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
             ccol_key_not_found);  // This must have no side effect
  REQUIRE_EQ(delete_int_from_string(chmap, ""),
             ccol_invalid_args);  // This must have no side effect

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
  // A change through the pointer changes the map element directly.
  *val_ptr = 1;
  REQUIRE_EQ(get_int_from_string(chmap, "key1", &tmp), ccol_success);
  REQUIRE_EQ(tmp, 1);

  REQUIRE_EQ(get_int_ref_from_string(chmap, "key2", &val_ptr), ccol_success);
  REQUIRE_NE((void *)val_ptr, NULL);
  REQUIRE_EQ(*val_ptr, -5);
  // A change through the pointer changes the map element directly.
  *val_ptr = 7;
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

  // Add 3 and 5 on top of sum. sum is 8 after the following statement.
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

// oa_insert must check whether the key is already present BEFORE it reads the
// load factor of the map and possibly rehashes. Without that order, a pure
// value update for a key that is already present starts a rehash of the whole
// table, only because earlier, unrelated inserts left the map above
// its growth threshold, and that rehash makes the chmap_get_elem_ref pointer
// of every other key invalid, an unwanted side effect. An update of an
// existing key must never grow the table: only a genuinely new element
// justifies growth, and an update does not change elem_count.
TEST(chash_maps, oa_update_of_existing_key_never_triggers_rehash) {
  chmap_construct(hm, int, int);
  REQUIRE_NE((void *)hm, NULL);

  size_t capacity_before_any_insert = chmap_get_bucket_arr_size(hm);
  REQUIRE_EQ(capacity_before_any_insert, (size_t)16);

  // Insert 12 distinct keys. 16 * 0.70 == 11.2, and the map checks the 12th
  // insert against a count of 11 (the count before that insert), so that
  // insert does not cross the growth threshold itself. The table
  // keeps its original capacity and holds a count of 12, a load
  // factor of 0.75, which is already OVER the threshold, and the table never
  // grew.
  for (int i = 0; i < 12; ++i) {
    int val = i * 100;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)12);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  // Hold a reference into a DIFFERENT key, key 0, across the update below.
  int key0 = 0;
  const cmap_pair *key0_val_pair = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(hm, &(cmap_pair){.ptr = &key0, .size = sizeof(key0)},
                         &key0_val_pair),
      ccol_success);
  REQUIRE_NE((void *)key0_val_pair, NULL);
  void *key0_val_addr_before = key0_val_pair->ptr;
  int key0_val_before = *(int *)key0_val_pair->ptr;
  REQUIRE_EQ(key0_val_before, 0);

  // Update the value of an EXISTING key, key 5, while the map sits above
  // its growth threshold. The map must treat this as an update and not as a
  // new insert, so it must never rehash.
  int key5 = 5;
  int new_val5 = 999;
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = &key5, .size = sizeof(key5)},
                 &(cmap_pair){.ptr = &new_val5, .size = sizeof(new_val5)}),
             ccol_key_already_present);

  // The element count does not change, because this is an update and not a
  // new element, and, most important of all, the table must not grow.
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)12);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  // The reference to the value of key 0 must still be valid and point at
  // exactly the same address, because a rehash would move the whole
  // val_accessors array, and with it this pointer, to a new allocation.
  REQUIRE_EQ((void *)key0_val_pair->ptr, key0_val_addr_before);
  REQUIRE_EQ(*(int *)key0_val_pair->ptr, key0_val_before);

  // The update itself must take effect.
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

unsigned long custom_int_key_hasher(const void *ptr, size_t size) {
  (void)size;
  return *(const int *)ptr;
}

TEST(chash_maps, declarative_macros) {
  {
    chmap_declare(hm, int, char *);
    chmap_init(hm);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
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

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
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

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
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
    chmap_init_full(hm, &mp, ch, NULL);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
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

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
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

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
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

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
    strncpy(*chmap_get_ptr(hm, key), "hi!", 5);
    REQUIRE_STREQ(chmap_get(hm, key), "hi!");

    chmap_remove(hm, key);

    chmap_destroy(hm);
  }

  {
    ccol_hashing_proc_t ch = &custom_int_key_hasher;
    ccol_memmgmt_procs_t mp = (ccol_memmgmt_procs_t){
        .malloc = malloc, .free = free, .calloc = calloc, .realloc = realloc};
    chmap_construct_full(hm, int, char *, &mp, ch, NULL);

    int key = 3;

    chmap_insert(hm, key, "hello");
    REQUIRE_STREQ(chmap_get(hm, key), "hello");

    // The new string here is not longer than "hello", which is VERY important;
    // to store a longer string, do a realloc first.
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

  int records[6] = {0};  // Indices 0 to 5, so that 3, 4 and 5 are valid
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
    // The block end destroys hm automatically: no chmap_destroy is necessary.
  }
}

// chmap_get_elem_copy must check its output buffer parameters before it
// dereferences them; without that check, a NULL target_buf or a
// target_buf_size of 0 leaves the caller no way to find the error.
TEST(chash_maps, get_elem_copy_rejects_null_buf_and_zero_size) {
  chashmap *chmap = chmap_create(1, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)chmap, NULL);

  REQUIRE_EQ(insert_string_to_int(chmap, "key1", 42), ccol_success);

  int val = -1;
  cmap_pair key = {.ptr = (void *)"key1", .size = strlen("key1")};

  REQUIRE_EQ(chmap_get_elem_copy(chmap, &key, NULL, sizeof(int)),
             ccol_invalid_args);
  REQUIRE_EQ(chmap_get_elem_copy(chmap, &key, &val, 0), ccol_invalid_args);

  // Confirm that the map still works correctly after the rejected calls
  REQUIRE_EQ(chmap_get_elem_copy(chmap, &key, &val, sizeof(int)), ccol_success);
  REQUIRE_EQ(val, 42);

  chmap_destroy(chmap);
}

// chmap_iter_key_ptr and chmap_iter_val_ptr must expand the iterator name
// that the caller gives as a macro parameter, and never use a
// hardcoded 'it'. Every other test in this file names its iterator 'it', so
// no other test can tell the two apart; this test gives the iterator a
// different name on purpose, so that a macro body that reaches for 'it'
// directly fails to compile here.
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

// A controlled calloc that simulates an out-of-memory condition inside
// oa_rehash, without affecting the creation or the destruction of a map.
static bool g_calloc_fail = false;
static void *controlled_calloc(size_t nmemb, size_t size) {
  if (g_calloc_fail) return NULL;
  return calloc(nmemb, size);
}

// A controlled malloc that simulates an out-of-memory condition inside
// sc_reset_val_of_llist_node, without affecting the creation of a map, the
// first inserts, or the destruction.
static bool g_malloc_fail = false;
static void *controlled_malloc(size_t size) {
  if (g_malloc_fail) return NULL;
  return malloc(size);
}

// A controlled realloc that simulates an out-of-memory condition on the
// heap-to-heap update path of sc_reset_val_of_llist_node, without affecting
// the creation of a map, the first inserts, or the allocation of a node.
static bool g_realloc_fail = false;
static void *controlled_realloc(void *ptr, size_t size) {
  if (g_realloc_fail) return NULL;
  return realloc(ptr, size);
}

// A real realloc can shrink a block or grow it in place, but this one always
// allocates a fresh block and frees the original one, so it moves the
// allocation on every call, deterministically. That makes the
// heap-to-heap update path of sc_reset_val_of_llist_node reliable to test
// when the source pointer aliases the block that the realloc moves, because a
// real realloc can keep the same address for a small change of size, and a
// use-after-free read of the original block then goes unnoticed.
static void *force_moving_realloc(void *ptr, size_t size) {
  void *new_block = malloc(size);
  if (!new_block) return NULL;
  free(ptr);
  return new_block;
}

// A probe can wrap all the way around and find no slot that is truly empty;
// oa_insert must then reuse a tombstone slot (a deleted slot), and
// must not report ccol_container_full. That tombstone reuse path after the
// loop is reachable only when all 16 slots are live or tombstoned, a state
// that in turn needs the calloc of the rehash to fail with an out-of-memory
// condition, which this test simulates.
TEST(chash_maps, oa_tombstone_reuse_after_full_probe_wrap) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = controlled_calloc,
                             .realloc = realloc};

  // Use the minimum capacity of 16, so the shrink guard of oa_delete,
  // capacity > minimum_allowed_bucket_array_size, never fires:
  // tombstones accumulate, and no rehash clears them.
  char *err = NULL;
  chashmap *hm = chmap_create_mp(1, ccol_int, ccol_int, &mp, &err);
  REQUIRE_NE((void *)hm, NULL);

  // Make every later calloc call fail, so a rehash can never enlarge the map.
  // The inserts of k0 to k15 succeed all the same: the load-factor check
  // (count+deleted)/16 > 0.70 fires from key 12 onwards, but the failed
  // rehash leaves the original 16-slot array in place, so the probe
  // finds empty slots.
  g_calloc_fail = true;
  for (int i = 0; i < 16; i++) {
    int val = i * 10;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 16);

  // Delete 10 keys. No calloc runs here, because at the minimum capacity the
  // shrink check in oa_delete does not fire, so these slots become tombstones
  // in place.
  for (int i = 0; i < 10; i++) {
    REQUIRE_EQ(
        chmap_delete_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), 6);

  // All 16 slots are live (6 of them) or tombstoned (10 of them), so an
  // insert of key 16 fires the load check, because (6+10)/16 == 1.0 > 0.70.
  // The map then tries to rehash, the calloc fails, and the map stays
  // unchanged; the probe visits every slot and finds no empty one, so the
  // insert must land in the first tombstone slot that the probe recorded,
  // and must not give ccol_container_full.
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

// An opportunistic grow-on-insert rehash can fail because calloc never
// recovers from an out-of-memory condition, and the probe can then find the
// table genuinely full: every slot is live, and no tombstone is available,
// as the test deletes nothing. oa_insert must not report ccol_container_full
// in that case, a code indistinguishable from the real architectural
// ccol_max_elem_count of the map, when the true cause is a
// failed allocation; oa_insert must report ccol_not_enough_memory instead,
// which is what the documented contract of chmap_insert_elem promises:
// "ccol_not_enough_memory when an allocation fails".
TEST(chash_maps, oa_insert_sustained_oom_reports_not_enough_memory) {
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = free,
                             .calloc = controlled_calloc,
                             .realloc = realloc};

  char *err = NULL;
  chashmap *hm = chmap_create_mp(1, ccol_int, ccol_int, &mp, &err);
  REQUIRE_NE((void *)hm, NULL);

  // Make every later calloc call fail so the table can never grow.
  g_calloc_fail = true;

  // Fill the table completely (16 keys into a 16-slot table), with no
  // deletions at all, so no tombstones ever exist to fall back on. Every one
  // of these succeeds: several of them try a rehash again, and fail,
  // after the load factor crosses 0.70, but the first 16-slot array still has
  // empty slots for the probe to land on.
  for (int i = 0; i < 16; i++) {
    int val = i;
    REQUIRE_EQ(
        chmap_insert_elem(hm, &(cmap_pair){.ptr = &i, .size = sizeof(i)},
                          &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
        ccol_success);
  }
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)16);
  REQUIRE_EQ(chmap_get_bucket_arr_size(hm), (size_t)16);

  // One more, genuinely new key: the load factor check fires, and the rehash
  // fails because calloc is still turned off, so the probe finds the table
  // completely full, with no empty slot and no tombstone. The map must report
  // this as an allocation failure, not as a false "container full".
  int overflow_key = 16, overflow_val = 1600;
  REQUIRE_EQ(
      chmap_insert_elem(
          hm, &(cmap_pair){.ptr = &overflow_key, .size = sizeof(overflow_key)},
          &(cmap_pair){.ptr = &overflow_val, .size = sizeof(overflow_val)}),
      ccol_not_enough_memory);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)16);

  g_calloc_fail = false;

  // With the allocator restored, the very same insert must succeed by
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

// In the OA backend, map->val_size must stay one single value for the whole
// life of the map: oa_create() fixes it when it creates the map, and no later
// insert ever derives it again. Without this, a caller can empty a map with
// repeated deletes, fill it again, and silently corrupt the value size that
// val_accessors reports.
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

// A char*-keyed map always uses separate chaining, and stores any scalar
// value type that fits inline (<= 23 bytes, the SSO threshold) directly
// inside the chmap_entry of the node. chmap_get, chmap_get_ptr and the
// iterator accessors cast a pointer into that storage straight to the type of
// the value and dereference it, so the storage must be naturally
// aligned for that type. This test covers a double (8-byte alignment) and a
// struct whose own alignment requirement is 8 bytes, through chmap_get_ptr,
// because that macro gives back a pointer for a change in place, not only
// a copy of the value.
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

// long double needs 16-byte alignment on this platform, because
// _Alignof(long double) == 16, which is stricter than every other case that
// sc_inline_value_alignment covers above. chashmap.h documents that the
// open-addressing backend excludes long double, because it can be more than 8
// bytes, so any chashmap with a long double key or value always takes
// this separate-chaining path with SSO inline storage. This test covers all
// three direct-cast accessors that read inline storage without a memcpy:
// chmap_get_ptr, chmap_get, and the iterator accessors ccol_iter_key_ptr and
// ccol_iter_val_ptr, each a separate code path that can dereference a
// misaligned pointer. Without _Alignas(max_align_t) on the key_storage and
// val_storage members of chmap_entry, these fail under -fsanitize=undefined
// with "load of misaligned address ... which requires 16 byte alignment".
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

    // Key equality and the hash here are bitwise over the full sizeof(long
    // double), as for any other key type that is not a
    // string and that the map does not canonicalize (see sc_compare_keys and
    // hash_key_data in chashmap.c). On this platform the 16-byte
    // representation of a long double carries real padding bits, because only
    // about 80 bits are significant, and a plain scalar in automatic storage
    // does not reliably leave those padding bytes defined after the compiler
    // optimizes it, even after a memset: the compiler can treat a later
    // whole-object assignment as a reason to drop the earlier memset, since the
    // padding bits are not part of the "value" of the object for the abstract
    // machine. valgrind reports exactly this at the real -O3 flags of the
    // root Makefile: a memset, and then an assignment into a long double in
    // automatic storage, still leaves padding bytes undefined. An object with
    // static storage duration and a constant initializer does not have this
    // problem, because the compiler builds its full, fixed-width byte
    // representation one time, so every byte is deterministic, the padding
    // included. This is a property of how you build a fully defined long double
    // bit pattern in portable C, not of the real target of this test, which is
    // the alignment of the inline storage in chmap_entry.
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

/* The type-inferred macros give their private copy of a caller expression the
   DECLARED key type or value type of the map, so the copy is a plain C
   assignment and converts. A copy of typeof(key) instead would put the RAW
   BYTES of the caller expression into the map whenever the two types happen
   to share a width. Nothing else in the library can see that: the widths
   match, so every size check passes, and no compiler diagnostic fires at any
   optimization level. cvec_push() already works this way, and these tests
   pin the same rule for chmap. */

TEST(chash_maps, value_converts_to_the_declared_type_and_is_not_reinterpreted) {
  chmap_construct(hm, char *, int);
  float f = 1.5f;
  chmap_insert(hm, "k", f);
  /* The converted value is 1. The bit pattern of 1.5f read as an int is
     1069547520, which is what a raw byte copy would store. */
  REQUIRE_EQ(chmap_get(hm, "k"), 1);
  chmap_destroy(hm);
}

TEST(chash_maps, key_converts_to_the_declared_type_and_stays_findable) {
  /* A float key of 2.0f in an int-keyed map is the worse half of the same
     bug. A raw byte copy stores the bit pattern 1073741824, and no int
     lookup can ever reach that entry again. */
  chmap_construct(hm, int, int);
  float fk = 2.0f;
  int v = 7;
  chmap_insert(hm, fk, v);
  int ik = 2;
  int *p = chmap_get_ptr(hm, ik);
  REQUIRE_NE((void *)p, NULL);
  REQUIRE_EQ(*p, 7);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  chmap_destroy(hm);
}

TEST(chash_maps, narrower_value_widens_to_the_declared_type) {
  /* A short that goes into an int-valued map becomes an int, so the stored
     pair is always sizeof(ValT) bytes wide. Without the conversion the
     separate-chaining backend accepts a two-byte value, reports success, and
     the value-size guard of chmap_get() then stops the process on the first
     READ, at a call site that did nothing wrong. */
  chmap_construct(hm, char *, int);
  short sv = -1;
  chmap_insert(hm, "k", sv);
  REQUIRE_EQ(chmap_get(hm, "k"), -1);
  chmap_destroy(hm);

  /* The same, on the open-addressing backend. */
  chmap_construct(oa, int, int);
  short sv2 = -3;
  chmap_insert(oa, 1, sv2);
  REQUIRE_EQ(chmap_get(oa, 1), -3);
  chmap_destroy(oa);
}

TEST(chash_maps, rvalue_key_and_value_accepted) {
  /* The copy is initialized from the expression, so an rvalue needs no
     address and no named variable. */
  chmap_construct(hm, char *, int);
  chmap_insert(hm, "alice", 42);
  chmap_insert(hm, "bob", 6 * 7);
  REQUIRE_EQ(chmap_get(hm, "alice"), 42);
  REQUIRE_EQ(chmap_get(hm, "bob"), 42);
  REQUIRE_EQ(chmap_remove(hm, "bob"), ccol_success);
  chmap_destroy(hm);

  chmap_construct(oa, long, long);
  chmap_insert(oa, 1 + 1, 100L);
  REQUIRE_EQ(chmap_get(oa, 2), 100L);
  chmap_destroy(oa);
}

TEST(chash_maps, array_key_and_string_literal_key_agree) {
  /* The character-pointer arm of the copy decays an array to a pointer. A
     char buf[] key and a string literal with the same content must therefore
     reach the same entry. */
  chmap_construct(hm, char *, int);
  char buf[64];
  snprintf(buf, sizeof(buf), "shared");
  chmap_insert(hm, buf, 5);
  REQUIRE_EQ(chmap_get(hm, "shared"), 5);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  chmap_insert(hm, "shared", 9);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_EQ(chmap_get(hm, buf), 9);
  chmap_destroy(hm);
}

TEST(chash_maps, raw_layer_rejects_a_value_of_the_wrong_declared_width) {
  /* chmap_insert_elem() takes a pair that the caller built by hand. A value
     whose width disagrees with the declared value type must be refused by
     the INSERT, on both backends. Without the check on the separate-chaining
     side, the insert reports success and chmap_get() stops the process later
     at an unrelated call site, with a message that blames the reader. */
  char *err = NULL;
  short bad = -1;
  int good = -1;

  chmap oa = chmap_create_full(16, ccol_int, ccol_int, NULL, NULL, NULL, &err);
  REQUIRE_NE((void *)oa, NULL);
  int ik = 1;
  cmap_pair kp_i = {&ik, sizeof(ik)};
  cmap_pair vp_bad = {&bad, sizeof(bad)};
  cmap_pair vp_good = {&good, sizeof(good)};
  REQUIRE_EQ((int)chmap_insert_elem(oa, &kp_i, &vp_bad),
             (int)ccol_invalid_args);
  REQUIRE_EQ((int)chmap_insert_elem(oa, &kp_i, &vp_good), (int)ccol_success);
  __chmap_destroy(oa);

  chmap sc =
      chmap_create_full(16, ccol_string, ccol_int, NULL, NULL, NULL, &err);
  REQUIRE_NE((void *)sc, NULL);
  cmap_pair kp_s = {(void *)"k", 2};
  REQUIRE_EQ((int)chmap_insert_elem(sc, &kp_s, &vp_bad),
             (int)ccol_invalid_args);
  REQUIRE_EQ((int)chmap_insert_elem(sc, &kp_s, &vp_good), (int)ccol_success);
  __chmap_destroy(sc);

  /* A value type with no fixed width is never checked. Every entry of a
     string-valued map legitimately carries its own size. */
  chmap str =
      chmap_create_full(16, ccol_string, ccol_string, NULL, NULL, NULL, &err);
  REQUIRE_NE((void *)str, NULL);
  cmap_pair vp_short = {(void *)"a", 2};
  cmap_pair vp_long = {(void *)"abcdefgh", 9};
  REQUIRE_EQ((int)chmap_insert_elem(str, &kp_s, &vp_short), (int)ccol_success);
  REQUIRE_EQ((int)chmap_insert_elem(str, &kp_s, &vp_long),
             (int)ccol_key_already_present);
  __chmap_destroy(str);
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

  // char * map, looked up with signed char * and unsigned char *.
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

// sc_reset_val_of_llist_node must not write NULL into the val_storage union
// before it checks the return value of malloc: val_storage.ptr and
// val_storage.inline_data share the same memory, so such a write
// zeroes the first sizeof(void*) bytes of the old inline value, which
// corrupts the value when the allocation fails, while val_is_inline stays
// true. After a failed update from inline to heap, the first inline value
// must still be intact.
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
  const cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, (size_t)4);
  REQUIRE_EQ(memcmp(vp->ptr, small_val, 4), 0);

  // Attempt to update with a heap-requiring value (>23 bytes) while malloc
  // fails; the update must be rejected gracefully.
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
// (string key) and the OA backend (int key). This covers the two early-out
// paths in chashmap_begin_iter.
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

// chashmap_begin_iter(NULL, ...) must behave the same as an empty map: it
// must return NULL, must not touch err, and must not assert. Other code in
// this codebase depends on this contract, so it is more than permissive
// behaviour. For example, request.headers_begin_empty_returns_null in
// tests/chttpclient/tests.c iterates an internal chmap field of a
// chttp_request_t with ccol_begin and chashmap_begin_iter, a field that stays
// NULL until the first header is set. Several call sites in chttpclient.c,
// clogger.c, ctls.c and cthreadcomm.c also hand such an optional chmap
// straight to chashmap_begin_iter.
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

// An SC-backend entry can hold its value on the heap, because the value is
// more than 23 bytes, and an update can then give it a new value that fits
// inline, because the new value is 23 bytes or less. The map must free the old
// heap buffer, the node must change to inline storage, and a reader must then
// get the new value with the correct size.
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

  const cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, (size_t)4);
  REQUIRE_EQ(memcmp(vp->ptr, small_val, 4), 0);

  chmap_destroy(hm);
}

// An SC-backend entry holds its value on the heap, and an update gives it
// another value that also needs the heap, but realloc fails.
// sc_reset_val_of_llist_node must then put the first pointer back and
// leave the old value fully intact.
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

  const cmap_pair *vp = NULL;
  REQUIRE_EQ(
      chmap_get_elem_ref(
          hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)}, &vp),
      ccol_success);
  REQUIRE_EQ(vp->size, (size_t)30);
  REQUIRE_EQ(memcmp(vp->ptr, large_val1, 30), 0);

  chmap_destroy(hm);
}

// A value pointer can alias the current heap storage of the entry itself:
// for example, a caller reads a pointer for one key with chmap_get_elem_ref,
// chmap_get_ptr or chmap_get, and then inserts a value from that pointer, for
// that same key. The map must read that value correctly, although a
// change to an inline-sized value frees that same heap buffer as part of the
// update, so sc_reset_val_of_llist_node must not free the buffer first and
// copy from it after. That order is a use-after-free read, which valgrind and
// AddressSanitizer report, and the value assertion below also fails whenever
// something changes the freed block before the read ends.
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

  const cmap_pair *vp = NULL;
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

// A caller can grow an inline value and use the current inline storage of the
// map as the source, and the map must not corrupt the source bytes before it
// reads them. Because the val_storage union physically overlaps the inline
// byte array, sc_reset_val_of_llist_node must not write the new heap
// pointer into that union before it copies from it: that order corrupts the
// first bytes of the value that the caller gets back, and no sanitizer is
// needed to see it. The new size here is exactly 24 bytes, which is
// INLINE_STORAGE_THRESHOLD + 1, so the read stays fully inside the bounds
// of the inline array, and the expected result is fully deterministic:
// the first 4 bytes, and then the zero bytes that calloc gave the node
// and that nothing ever overwrote.
TEST(chash_maps, sc_value_update_self_referential_inline_to_heap) {
  chashmap *hm = chmap_create(1, ccol_string, ccol_other_types, NULL);
  REQUIRE_NE((void *)hm, NULL);

  const char *key = "k";
  unsigned char small_val[4] = {0x11, 0x22, 0x33, 0x44};
  REQUIRE_EQ(chmap_insert_elem(
                 hm, &(cmap_pair){.ptr = (void *)key, .size = strlen(key)},
                 &(cmap_pair){.ptr = small_val, .size = sizeof(small_val)}),
             ccol_success);

  const cmap_pair *vp = NULL;
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

// A caller can shrink a heap value and use the current heap storage of the
// map as the source; if realloc moves the block, the map must not read from
// the block that realloc frees. force_moving_realloc always frees the first
// allocation and returns a new one, whereas a real realloc can shrink a block
// in place, which hides the stale read, so force_moving_realloc turns
// such a read into a use-after-free that valgrind and AddressSanitizer
// always detect.
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

  const cmap_pair *vp = NULL;
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

// A companion to the three self-referential update tests above: an exact
// self-copy, where the size does not change and the value pointer aliases the
// current storage of the entry byte for byte, must also leave the value
// correct, both for an inline entry and for a heap
// entry.
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

  const cmap_pair *vp = NULL;
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

// A caller can insert a value again for a key that is already present, using
// a pointer from chmap_get_elem_ref or chmap_get_ptr for that exact key
// as the source of the insert, and this must not corrupt the stored value.
// This test is the mirror of the sc_value_update_self_referential_* group
// above, for the existing-key update path of the open-addressing backend,
// which is a plain memcpy in place into the val_data field of the slot and
// needs an alias guard to stay correct. The test reaches it through the raw
// chmap_insert_elem function layer, not through the type-inferred macros,
// because those macros always copy through a local on the stack, so they can
// never alias storage that the map owns in this way.
TEST(chash_maps, oa_value_update_self_referential_same_size) {
  chashmap *hm = chmap_create(1, ccol_int, ccol_long_long, NULL);
  REQUIRE_NE((void *)hm, NULL);

  int key = 7;
  /* long long, and not long: a long is only 4 bytes on ILP32 (for example on
   * i386), too narrow to hold this 8-byte bit pattern at all, while a long
   * long is reliably 8 bytes on every mainstream platform that this library
   * targets. tests/clrucache/tests.c uses a long long for the same
   * "distinctive 8-byte bit pattern" need. */
  long long val = 0x1122334455667788LL;
  REQUIRE_EQ(
      chmap_insert_elem(hm, &(cmap_pair){.ptr = &key, .size = sizeof(key)},
                        &(cmap_pair){.ptr = &val, .size = sizeof(val)}),
      ccol_success);

  const cmap_pair *vp = NULL;
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

// FNV-1a over exactly ptr[0..size), the shape that a hash function for a
// binary key of variable length needs: it must hash two keys that share
// a prefix but have different lengths, such as "ab" and "abc", to different
// values, without depending on a NUL terminator, or on any other
// outside convention, to find where the key ends.
static unsigned long fnv1a_key_hasher(const void *ptr, size_t size) {
  const unsigned char *b = (const unsigned char *)ptr;
  unsigned long h = 2166136261UL;
  for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 16777619UL;
  return h;
}

// Check that the map passes the size parameter to the custom hash callback
// correctly. The map here uses separate chaining with a string key, whose
// length varies, so two keys that share a prefix but differ in length must
// hash independently, and the map must store them and find them
// independently too.
TEST(chash_maps, sc_custom_hashing_with_variable_length_key) {
  ccol_hashing_proc_t ch = &fnv1a_key_hasher;
  chmap_construct_ch(hm, char *, int, ch);

  int v1 = 1, v2 = 2, v3 = 3;
  chmap_insert(hm, "ab", v1);
  chmap_insert(hm, "abc", v2);
  chmap_insert(hm, "abcd", v3);

  REQUIRE_EQ(chmap_elem_count(hm), 3);
  REQUIRE_EQ(chmap_get(hm, "ab"), 1);
  REQUIRE_EQ(chmap_get(hm, "abc"), 2);
  REQUIRE_EQ(chmap_get(hm, "abcd"), 3);

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

  const cmap_pair *out = NULL;
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

// chmap_get_ptr on the OA backend must return a pointer that a caller can
// write through, so that a later chmap_get call shows the new value. This
// test also confirms that the macro returns NULL for a key that is absent.
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

// This test deletes entries from an OA-backend map whose capacity is at the
// minimum of 64 slots, where no rehash runs, so nothing clears the tombstones.
// The iterator must then skip the tombstone slots and visit only
// the live entries that are left.
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

// chmap_reset(hm, 0) must clear all the elements on the SC backend, which a
// char* key selects, and keep the current size of the bucket array: the
// same behaviour that oa_reset_zero_preserves_capacity checks for the
// OA backend.
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
// the integer hash path for ccol_float and ccol_double keys, along with
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

// Negative zero and positive zero are `==` in C, but they have different
// object representations, and key equality in this map is otherwise bitwise
// (see canonicalize_key_pair_if_needed in chashmap.c), so the map
// canonicalizes a signed zero on the way in, and the two zeroes become one
// single key. This holds for the OA backend, which an int value selects, and
// for the SC backend, which a char* value selects.
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

// NaN is never `==` to anything, not even to itself, but a hash map still
// needs a reflexive key-equality relation to be usable at all. Key equality
// for a floating-point key in this map is bitwise, apart from the signed-zero
// canonicalization above, so the map always finds a NaN key again by
// the exact bit pattern of the insert, and two NaN keys with different
// payloads stay two separate keys.
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

// A float is exactly 4 bytes and a double is exactly 8 bytes, with no
// padding, but the in-memory representation of a long double is different,
// because it carries padding bits that the platform defines: on this platform
// it is 80-bit x87 extended precision in a 16-byte slot, which leaves 6
// padding bytes that the C standard says nothing about. Key equality and the
// hash for a long double are therefore value-based (see hash_long_double_value
// and long_double_keys_equal in chashmap.c). Two variables can hold the same
// numeric value and come from completely different code paths, so their
// padding bits can differ and are not deterministic, and the map must still
// find them as the same key. This is the same idea as the -0.0 and 0.0
// unification that float_double_keys_negative_zero_canonicalized covers for
// float and double, but for a long double the mechanism is a native
// floating-point comparison of values, not a canonicalization of bytes,
// because this library cannot zero the padding bits of a long double in a
// portable way. The comment on sc_inline_long_double_alignment gives the
// measured basis for that constraint.
TEST(chash_maps, long_double_keys_padding_insensitive_equality) {
  chmap_construct(hm, long double, int);

  // Two long double variables hold the exact same numeric value, but two
  // different code paths build them (a literal and a computation at run
  // time), which is the case that this test covers: only their significant
  // value bits must be identical, while their padding bits can
  // differ.
  long double a = 3.0L;
  volatile long double one = 1.0L, two = 2.0L;
  long double b = one + two;
  REQUIRE_EQ(a, b);

  int va = 111;
  chmap_insert(hm, a, va);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_NE((void *)chmap_get_ptr(hm, b), (void *)NULL);
  REQUIRE_EQ(chmap_get(hm, b), 111);

  // A padding region that this test pokes on purpose must not change the key
  // that the map finds. The test builds a raw byte buffer that holds the same
  // value, forces every trailing padding byte to a nonzero pattern, and
  // then confirms that the raw chmap_insert_elem and chmap_get_elem_ref layer
  // treats that buffer as the same key that an ordinary, clean variable
  // with the same value gives.
  //
  // The guard is LDBL_MANT_DIG == 64, and not a bare `sizeof(long double) >
  // 10`, because LDBL_MANT_DIG == 64 names the x87 80-bit extended-precision
  // layout: 1 explicit sign bit, 15 exponent bits and 64 explicit mantissa
  // bits, 10 significant bytes padded out to a sizeof(long double) of 12 or 16
  // with bytes that nothing uses. On aarch64 the long double is IEEE
  // binary128 and LDBL_MANT_DIG is 113, as on any other binary128 platform,
  // where the long double has a sizeof(long double) of 16 too, but EVERY
  // one of those 16 bytes is significant, so no padding region is left to
  // poke at all. A poke of bytes 10 to 15 there does more than fail to show
  // the padding insensitivity that this test wants to show: it corrupts the
  // mantissa and exponent bits of the value into a completely different
  // number, and `poked_value` reads back as -nan instead of 3.0. A bare size
  // check lets that through, and the test then fails falsely; the failure
  // is not a defect in the long-double key comparison of this library:
  // chashmap.c compares long double keys by numeric VALUE, with a plain
  // memcpy and then an ==, over the whole object, and never inspects a
  // padding byte, so it does not depend on this assumption.
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
    const cmap_pair *out = NULL;
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

// -0.0L and 0.0L are `==` in C, exactly like -0.0 and 0.0, and the long
// double key equality of this map unifies them, the same behaviour that
// float_double_keys_negative_zero_canonicalized checks for float and double.
// See the sibling comment above, and the dedicated zero branch in
// hash_long_double_value.
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

// The map separates NaN keys of type float and double by their exact bit
// pattern (see float_double_nan_keys_bitwise_identity above), but every NaN
// long double is different: they all collapse into one single key, and the
// map reads no payload byte and no padding byte to decide this. This
// difference from the float and double policy is deliberate, and the padding
// of a long double forces it: in practice the padding bytes of a NaN
// long double are genuine uninitialized memory, as `long double n = NAN;` never
// writes them, so a read of them with memcmp is a real hazard on its own, and
// not only a source of non-determinism, and so is a hash of them;
// valgrind reports such a read as a use of uninitialized memory.
// hash_long_double_value and long_double_keys_equal both test isnan() before
// they touch any raw byte, and the long double comparator of cbstmap collapses
// every NaN into one key for the same reason; see the doc comment on
// cmp_float_val in cbstmap.c.
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

// A caller must be able to use both signs of infinity as keys, so
// hash_long_double_value has a dedicated branch for them. It needs that
// branch, because the exponent output of frexpl is unspecified for an
// infinite input, and a conversion of an infinite value to an integer type is
// undefined behavior. The two infinities must also stay separate from
// each other, and separate from every finite key.
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

// This test is the mirror of sc_float_key_size_mismatch_returns_invalid_args:
// a long double key also gets an exact-size check in the raw
// chmap_insert_elem, chmap_get_elem_ref and chmap_delete_elem layer, the
// check key_size_matches_type_if_fixed_width, which is needed because
// hash_long_double_value and long_double_keys_equal both trust key_size to be
// exactly sizeof(long double) and read that many bytes every time. The map
// must therefore reject a buffer from the caller that is too small, before
// either function reads past the end of that buffer. The size
// of a key type of genuinely variable length is trusted; the size of this one
// is not.
TEST(chash_maps, long_double_key_size_mismatch_returns_invalid_args) {
  chmap_construct(hm, long double, int);
  REQUIRE_NE((void *)hm, NULL);

  // The size below is one byte short of sizeof(long double), not a
  // hardcoded sizeof(double), because the size of a long double depends on
  // the platform and the ABI (see the "long double memory layout" portability
  // note of this codebase). On armhf a long double has no extended precision
  // at all and is bit for bit the same as a double, so sizeof(double) IS
  // the correct, exact size there, which would make this buffer valid by
  // accident instead of too small. One byte short is wrong on every platform
  // by construction.
  size_t tiny_size = sizeof(long double) - 1;
  uint8_t *tiny_key_buf = (uint8_t *)malloc(tiny_size);
  REQUIRE_NE((void *)tiny_key_buf, NULL);
  memset(tiny_key_buf, 0, tiny_size);

  cmap_pair bad_key = {.ptr = tiny_key_buf, .size = tiny_size};
  int val = 5;
  cmap_pair val_pair = {.ptr = &val, .size = sizeof(val)};

  REQUIRE_EQ(chmap_insert_elem(hm, &bad_key, &val_pair), ccol_invalid_args);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);

  const cmap_pair *out = NULL;
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

// The scalar `signed char` maps to ccol_signed_char, which is not the same as
// ccol_char, and its typedef int8_t maps there too. Both must still satisfy
// is_type_integral and get_type_size in the same way as ccol_char, so the map
// selects the OA backend here too, and a negative value must hash
// correctly and come back correctly.
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

// An early break out of an iteration loop must not leak the iterator.
// This test covers two paths: the SC backend depends on the RAII
// _ccol_destructor to free the iterator when its block exits, and the OA
// backend calls ccol_iter_destroy directly before the break. The map must be
// fully intact after each early exit.
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
// so the OA backend is selected.  This test exercises the integer hash path
// for ccol_pointer keys in hash_key_data, along with full CRUD and
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

  // Update in place with chmap_get_ptr.
  int *p = chmap_get_ptr(hm, k2);
  REQUIRE_NE((void *)p, NULL);
  *p = 999;
  REQUIRE_EQ(chmap_get(hm, k2), 999);

  // Overwrite with an upsert.
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

// This hash forces every key into the same bucket with exactly the same
// stored hash_val, so the hash_val pre-check in sc_find_in_llist and
// sc_delete_from_llist can never stop the search early, and every
// lookup must fall through to the full memcmp on every candidate in the
// chain to find the right one.
static unsigned long constant_struct_key_hasher(const void *ptr, size_t size) {
  (void)ptr;
  (void)size;
  return 42;
}

TEST(chash_maps, sc_struct_key_full_hash_collision_still_resolves_correctly) {
  ccol_hashing_proc_t ch = &constant_struct_key_hasher;
  chmap_construct_ch(hm, helper_struct, int, ch);

  // helper_struct has 2 bytes of padding after b, so the code below does a
  // memset first and then sets each field on its own, because a whole-struct
  // assignment from a compound literal copies the uninitialised padding of
  // that literal back in. With a memset, the memcmp in sc_compare_keys never
  // reads an uninitialised padding byte.
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

// This test runs random inserts and removes against an int->int map, which
// uses the open-addressing backend, and the same operations against a
// shadow reference model, a plain array indexed by key. After every operation
// it checks two things: elem_count never goes above the
// capacity or above the scale-up threshold, and chmap_get_ptr gives back
// the exact shadow value for every key that the shadow model holds, and
// NULL for every key that the shadow model does not hold. The map and the
// reference model must never disagree.
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
    // The load-factor check in oa_insert runs BEFORE the insert ends, using
    // the count from before the insert, so one insert can push
    // elem_count one element past the threshold that starts a resize, and the
    // pre-check of the next operation then fires. The real invariant
    // therefore gives a tolerance of one element here, not a strict <=.
    // oa_insert in src/chashmap.c needs that tolerance; it is not a hedge: a
    // stricter form of this assertion, without the +1, fails against the
    // current library, a failure that shows the design of the load-factor
    // check before the insert, not a defect in the library.
    REQUIRE_TRUE(chmap_elem_count(hm) <=
                 chmap_get_elem_count_to_scale_up(hm) + 1);
  }

  chmap_destroy(hm);
}

// This test uses the same reference model as oa_invariants_random_ops, but
// the key is a char*, which selects the separate-chaining backend, and it
// also cross-checks against a manual walk of the public iterator, not only
// against chmap_elem_count.
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

// chmap_reset() must still destroy every existing element when the requested
// new_bucket_array_size is too large to honor, that is, when
// _ccol_find_nearest_gte_power_of_two rounds such a size above
// ccol_max_elem_count. This path must not return ccol_not_enough_memory at
// once without any change to the map, because that behaviour contradicts the
// documented contract of chmap_reset, which says "All elements are
// destroyed regardless of return value" and "ccol_not_enough_memory if resize
// fails (elements still cleared)".
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

// chmap_insert_elem, chmap_get_elem_ref and chmap_delete_elem must reject a
// key or a value whose size is not exactly the fixed key_size or val_size of
// the open-addressing backend, even when the pointer is
// not NULL and the size is not zero. insert_and_ref_invalid_args covers only
// the separate-chaining backend, which a char* key selects, and
// only the NULL and zero-size cases, so this test is the only one that
// covers key_size_matches_type_if_fixed_width and oa_val_size_matches.
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

  const cmap_pair *out = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm, &bad_key, &out), ccol_invalid_args);
  REQUIRE_EQ(chmap_delete_elem(hm, &bad_key), ccol_invalid_args);

  // The valid entry must still be present and intact after every rejection.
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)1);
  REQUIRE_EQ(chmap_get(hm, key), 100);

  chmap_destroy(hm);
}

// The key-size check must apply to every backend, and not only to the
// open-addressing one through oa_key_size_matches, because hash_key_data()
// dispatches on key_type alone: for any numeric key type of fixed width it
// reads a FIXED number of bytes from the key pointer of the caller (the
// sizeof of the matching C type), and ignores key_pair->size
// completely when it decides how many bytes to read. A separate-chaining map
// can also have an integral key type, whenever that key type
// goes with a value type that is not integral; an int->char* map is such a
// case, because a char* value forces separate chaining whatever the key type
// is. Such a map therefore needs its own rejection of a key_pair whose
// declared size is smaller than the true size of its key type; without that
// rejection, chmap_insert_elem, chmap_get_elem_ref and chmap_delete_elem
// silently read past the end of that buffer whenever the buffer
// of the caller is smaller than the true size of that type. This test
// allocates a heap buffer of exactly 2
// bytes for a key that the map declares as an int, which is 4 bytes, so
// valgrind and AddressSanitizer catch a read of even one byte past that
// buffer directly, and not only through a wrong return value.
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

  const cmap_pair *out = NULL;
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

// This is the same hazard, but through the float and double canonicalization
// path of canonicalize_key_pair_if_needed, which must check that
// key_pair->size is exactly the true size (4 bytes for a float and 8 bytes
// for a double) instead of trusting any size of 8 bytes or less and copying
// that many bytes out of the pointer of the caller. Without that check, a
// float-keyed separate-chaining map that a caller gives a key_pair claiming
// a size of 8, as if the key were a double, backed by an allocation of
// exactly 4 bytes, reads 4 bytes past the end of that
// allocation.
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

  const cmap_pair *out = NULL;
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

// The ccol_short, ccol_int, ccol_long and ccol_long_long branches of
// hash_key_data must read the key with a memcpy and never dereference
// a cast pointer, such as *(uint32_t*)key_ptr, as the ccol_float, ccol_double
// and ccol_pointer branches of the same function already do. A
// dereference of a pointer to a multi-byte type that is not naturally aligned
// for that type is undefined behavior, and can also fault on a
// strict-alignment architecture. A caller reaches this through the raw
// chmap_insert_elem, chmap_get_elem_ref and chmap_delete_elem function layer,
// whose cmap_pair.ptr member carries no alignment guarantee of
// its own, unlike the type-inferred macros, which always take the
// address of a local variable, which is naturally aligned. This test drives
// the raw API directly, with key pointers that a plain byte buffer
// makes misaligned on purpose, and never uses the address of a member of a
// packed struct, because that itself fires -Waddress-of-packed-member under
// the -Werror of this project. The relaxed-alignment ISA of this platform
// never gives a wrong VALUE for this; it shows only as undefined behavior
// under -fsanitize=undefined, or on a strict-alignment architecture, so the
// role of this test is to keep the misaligned access covered for that
// sanitizer. It also asserts that the ordinary round trip still succeeds.
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

    const cmap_pair *out = NULL;
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

    const cmap_pair *out = NULL;
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

    const cmap_pair *out = NULL;
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

    const cmap_pair *out = NULL;
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

// sc_set_scaling_limits must compute the exact "(bucket_count + 1) * 1.5" /
// "(bucket_count + 1) / 8" formulas chashmap.h documents for the
// separate-chaining backend. The off-by-a-fraction "bucket_count * 1.5" /
// "bucket_count / 8" pair is not what the header promises, and the exact
// thresholds asserted below are the ones that tell the two apart.
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

// The shrink step of sc_scale must not have a guard of bucket_arr_size >=
// (scale_factor * minimum_allowed_bucket_array_size), which is 64. A bucket
// array can land off the "minimum_allowed_bucket_array_size * 4^k" lineage:
// a size of 32 is such a value, and any initial_bucket_array_size between 16
// and 64 reaches it directly. With that guard, such an array can never shrink
// again, not even after a caller deletes every element, because 32
// / 4 == 8, which is below the documented floor of 16: the guard 32 >= 64 is
// false, so the code skips the shrink completely instead of clamping to 16,
// and the map stays at double the documented minimum bucket count forever.
TEST(chash_maps, sc_bucket_array_shrinks_to_minimum_from_off_lineage_size) {
  // _ccol_find_nearest_gte_power_of_two rounds 20 up to 32. A size of 32 is not
  // on the 16 * 4^k lineage that the automatic 4x and 0.25x scaling produces.
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
  // Without the clamp this stays at 32 forever; the 32 / 4 == 8 undershoot
  // is clamped up to the documented floor instead.
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

// oa_reset() makes two internal allocations. If either one fails, oa_reset()
// must not return ccol_not_enough_memory without any change to the map, and
// must not leave every element that was there before intact, because that
// behaviour contradicts the documented contract of chmap_reset, which says "All
// elements are destroyed regardless of return value" and
// "ccol_not_enough_memory if resize fails (elements still cleared)". The test
// reset_with_oversized_size_still_clears_elements covers only the SIZE_MAX
// pre-check of chmap_reset, which falls back to "keep current
// capacity", goes around the real allocation calls of oa_reset
// completely, and always succeeds. This test instead forces a genuine
// calloc failure at a fully legitimate requested size, with the same
// fault-injecting allocator as
// oa_insert_sustained_oom_reports_not_enough_memory.
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

static int chmap_hash_size_cmp(const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  return (x > y) - (x < y);
}

/* Two distinct long double values must not collapse to one hash.
 *
 * The scale step inside the value hash puts the significant bits at the top of
 * a 64-bit intermediate, so a mantissa with 32 or fewer significant bits has a
 * zero low half; an integer, a half and a third are such values, and so is
 * almost every number that anyone stores. Narrowing that intermediate to
 * size_t before it is hashed drops exactly the bits that tell such keys
 * apart, and the hash then depends on the exponent alone. In a hash map that
 * only makes a chain longer, but in anything that divides a fixed budget by
 * the hash, it throws data away.
 *
 * On a 32-bit build, a narrowing of the intermediate makes 4096 distinct keys
 * produce 13 distinct hashes. */
TEST(chmap_hash, distinct_long_doubles_do_not_share_one_hash) {
  enum { N = 4096 };
  size_t *hashes = (size_t *)malloc(N * sizeof(*hashes));
  REQUIRE_NE((void *)hashes, NULL);

  for (int i = 0; i < N; i++) {
    long double v = (long double)i + 0.5L;
    hashes[i] = ccol_chmap_hash_key(&v, sizeof(v), ccol_long_double);
  }

  /* Counted by sorting, so the check is O(n log n) rather than quadratic. */
  qsort(hashes, N, sizeof(*hashes), chmap_hash_size_cmp);
  size_t distinct = 1;
  for (int i = 1; i < N; i++) {
    if (hashes[i] != hashes[i - 1]) distinct++;
  }
  free(hashes);

  /* A good 64-bit hash over 4096 keys collides only a few times, and the bound
   * below is far under that number while being far above the 13 distinct
   * hashes that a cast which truncates produces. */
  REQUIRE_GT(distinct, (size_t)4000);
}

/* The convention this suite already uses for reaching a RUNNING_UNIT_TESTS-only
 * accessor in the module under test. */
extern unsigned long long chashmap_oa_probe_steps_for_tests(void);
extern void chashmap_reset_oa_probe_steps_for_tests(void);

/* chmap_reset accepts any power of two up to the architectural ceiling on the
 * element count, and the separate-chaining backend turns that count into a
 * byte count by multiplying it by the width of a pointer. At the top of the
 * range that product overflows size_t, and a product that wraps to zero is
 * the dangerous one: realloc can then free the block and return NULL, and
 * the recovery path after that writes through a pointer that the allocator
 * already took back.
 *
 * The count below makes the wrapped product ZERO, not merely too large to
 * satisfy. A count that wraps to a huge number fails cleanly at the
 * allocator, so it passes against the code with no guard and tests
 * nothing.
 *
 * This test is non-vacuous: without the guard, AddressSanitizer reports a
 * heap-use-after-free write inside the reset, and an ordinary build corrupts
 * the heap silently. */
TEST(chashmap_reset, a_bucket_count_whose_byte_size_wraps_is_refused) {
  chmap_construct(m, char *, int);
  int v = 1;
  chmap_insert(m, "alpha", v);

  /* Rounds up to the largest representable power of two, whose product with
     sizeof(void *) is exactly zero. */
  size_t wraps_to_zero = ((size_t)1 << (sizeof(size_t) * 8 - 2)) + 1;
  ccol_retval_t r = chmap_reset(m, wraps_to_zero);

  /* The map must still be usable, which is what proves the bucket array was
     not freed underneath it. Through the raw function layer, not the macro
     one: the macro calls ccol_fatal_err on a hard error, so a regression that
     left the map broken here would abort the whole binary and destroy every
     other suite's result instead of failing this one assertion. */
  int again = 7;
  const char *k = "beta";
  cmap_pair kp = {(void *)k, strlen(k) + 1};
  cmap_pair vp = {&again, sizeof(again)};
  ccol_retval_t ins = chmap_insert_elem(m, &kp, &vp);
  int got = 0;
  ccol_retval_t fetched = chmap_get_elem_copy(m, &kp, &got, sizeof(got));
  bool survived = (ins == ccol_success && fetched == ccol_success && got == 7);
  size_t count = chmap_elem_count(m);
  chmap_destroy(m);

  REQUIRE_EQ(r, ccol_not_enough_memory);
  REQUIRE_TRUE(survived);
  REQUIRE_EQ(count, (size_t)1);
}

/* The keys here have low bits that are all zero, the shape of aligned
 * addresses and of identifiers scaled by a power of two. The hash of this map
 * for an integral key ends in a multiply, whose mixing lands in the high
 * bits, and the index comes from the top of the hash.
 *
 * A hash whose low bits depend only on the low bits of the key, such as a bare
 * multiply, read through a mask over its low bits, makes this insert loop take
 * 6254142 probes, against a bound of 40960. The bound counts probes for each
 * operation and names no constant that the implementation owns, so it cannot
 * follow a change to the hash without notice, unlike an assertion written in
 * terms of the macros of the module itself, which can. */
TEST(chashmap_open_addressing,
     low_bit_constant_keys_do_not_collapse_the_table) {
  enum { N = 4096, STRIDE = 4096 };
  chmap_construct(m, long, int);

  chashmap_reset_oa_probe_steps_for_tests();
  for (long i = 0; i < (long)N; i++) {
    int v = (int)i;
    long k = i * (long)STRIDE;
    chmap_insert(m, k, v);
  }
  unsigned long long insert_probes = chashmap_oa_probe_steps_for_tests();

  chashmap_reset_oa_probe_steps_for_tests();
  long long acc = 0;
  for (long i = 0; i < (long)N; i++) {
    long k = i * (long)STRIDE;
    int *p = chmap_get_ptr(m, k);
    if (p) acc += *p;
  }
  unsigned long long lookup_probes = chashmap_oa_probe_steps_for_tests();

  size_t stored = chmap_elem_count(m);
  chmap_destroy(m);

  /* Every key is distinct, so all of them must be present however they were
     distributed; this separates a distribution failure from a correctness one.
   */
  REQUIRE_EQ(stored, (size_t)N);
  REQUIRE_EQ(acc, (long long)N * (N - 1) / 2);

  /* A table kept under its maximum load factor probes a small constant number
     of slots per operation on average. Ten is far above what any reasonable
     spread produces here and far below the hundreds a collapsed table needs. */
  /* Cast for the assertion macro's own printer, which has no case for
     unsigned long long; the values here are far inside long long's range. */
  REQUIRE_LT((long long)insert_probes, (long long)N * 10);
  REQUIRE_LT((long long)lookup_probes, (long long)N * 10);
}

/* A 64-bit key is often two 32-bit fields packed together, (a << 32) | b,
 * for a pair of coordinates or of identifiers. On a 32-bit target size_t is
 * narrower than such a key, so the hash must still depend on all 64 bits in a
 * way that separates the two halves. The grid below holds every pair with
 * both halves under GRID; with a hash that folds the halves together by an
 * exclusive or, only GRID distinct hashes exist for GRID * GRID keys.
 *
 * The suite runs natively and on the i386 and armhf jobs, and only a 32-bit
 * build can tell the two hashes apart. This test is non-vacuous there:
 * with the halves folded before the multiply, a -m32 build takes about 65
 * probes per insert on both open-addressing grids and builds chains of length
 * GRID (128) in the separate-chaining map, against bounds of 10 and 16. */
extern size_t chashmap_sc_longest_chain_for_tests(chmap chm);

enum { PACKED_GRID = 128 };

TEST(chashmap_hash, packed_pair_64_bit_keys_spread_over_the_table) {
  chmap_construct(m, uint64_t, int);

  chashmap_reset_oa_probe_steps_for_tests();
  for (uint64_t x = 0; x < PACKED_GRID; x++) {
    for (uint64_t y = 0; y < PACKED_GRID; y++) {
      chmap_insert(m, (x << 32) | y, 1);
    }
  }
  unsigned long long insert_probes = chashmap_oa_probe_steps_for_tests();

  chashmap_reset_oa_probe_steps_for_tests();
  long long found = 0;
  for (uint64_t x = 0; x < PACKED_GRID; x++) {
    for (uint64_t y = 0; y < PACKED_GRID; y++) {
      int *p = chmap_get_ptr(m, (x << 32) | y);
      if (p) found += *p;
    }
  }
  unsigned long long lookup_probes = chashmap_oa_probe_steps_for_tests();
  size_t stored = chmap_elem_count(m);
  chmap_destroy(m);

  const long long n = (long long)PACKED_GRID * PACKED_GRID;
  REQUIRE_EQ(stored, (size_t)n);
  REQUIRE_EQ(found, n);
  REQUIRE_LT((long long)insert_probes, n * 10);
  REQUIRE_LT((long long)lookup_probes, n * 10);
}

TEST(chashmap_hash, packed_pair_double_keys_spread_over_the_table) {
  chmap_construct(m, double, int);

  chashmap_reset_oa_probe_steps_for_tests();
  bool all_inserted = true;
  for (uint64_t x = 0; x < PACKED_GRID; x++) {
    for (uint64_t y = 1; y <= PACKED_GRID; y++) {
      /* A high half under 2^11 keeps the exponent field zero, so every key
         is a distinct finite double (a subnormal) and none is a zero. */
      uint64_t bits = (x << 32) | y;
      double k;
      memcpy(&k, &bits, sizeof(k));
      int v = 1;
      cmap_pair kp = {&k, sizeof(k)};
      cmap_pair vp = {&v, sizeof(v)};
      if (chmap_insert_elem(m, &kp, &vp) != ccol_success) all_inserted = false;
    }
  }
  unsigned long long insert_probes = chashmap_oa_probe_steps_for_tests();
  size_t stored = chmap_elem_count(m);
  chmap_destroy(m);

  const long long n = (long long)PACKED_GRID * PACKED_GRID;
  REQUIRE_TRUE(all_inserted);
  REQUIRE_EQ(stored, (size_t)n);
  REQUIRE_LT((long long)insert_probes, n * 10);
}

TEST(chashmap_hash, packed_pair_64_bit_keys_spread_over_the_chains) {
  /* A string value selects separate chaining for an integral key. */
  chmap_construct(m, unsigned long long, char *);
  for (unsigned long long x = 0; x < PACKED_GRID; x++) {
    for (unsigned long long y = 0; y < PACKED_GRID; y++) {
      chmap_insert(m, (x << 32) | y, "v");
    }
  }
  size_t stored = chmap_elem_count(m);
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  chmap_destroy(m);

  REQUIRE_EQ(stored, (size_t)PACKED_GRID * PACKED_GRID);
  /* 16384 keys at the map's load factor: a chain of a well spread hash stays
     in single digits, and a hash that folds the halves gives GRID. */
  REQUIRE_LE(longest, (size_t)16);
}

/* The hash of a caller can put its entropy anywhere (an identity hash over
 * dense integer keys is the ordinary shape of that), so the map must spread
 * such a hash before it derives an index. This test is the custom-hash
 * counterpart of low_bit_constant_keys_do_not_collapse_the_table, and needs
 * its own case because the hash of a caller goes around every hash that the
 * module computes itself.
 *
 * This test is not vacuous: without the finalizer over the result of a custom
 * hash, every key here lands in one slot, the probe count goes quadratic, and
 * this bound catches that by two orders of magnitude. The other custom-hash
 * tests cannot catch it, because they store three keys, and three keys in
 * one slot cost three probes. */
static unsigned long low_bit_entropy_hasher(const void *key_ptr,
                                            size_t key_size) {
  (void)key_size;
  return (unsigned long)*(const int *)key_ptr;
}

TEST(chashmap_hash, custom_hash_with_low_bit_entropy_does_not_collapse) {
  enum { N = 4096 };
  char *err = NULL;
  chmap m = chmap_create_full(16, ccol_int, ccol_int, NULL,
                              low_bit_entropy_hasher, NULL, &err);
  REQUIRE_NE((void *)m, NULL);

  chashmap_reset_oa_probe_steps_for_tests();
  /* The outcome is captured rather than asserted here: an assertion inside this
     loop returns from the test with the map still allocated, which memtest then
     reports as a leak on top of whatever actually failed. Every check runs
     after the destroy below. */
  bool all_inserted = true;
  for (int i = 0; i < N; i++) {
    cmap_pair k = {&i, sizeof(i)};
    cmap_pair v = {&i, sizeof(i)};
    if (chmap_insert_elem(m, &k, &v) != ccol_success) all_inserted = false;
  }
  unsigned long long insert_probes = chashmap_oa_probe_steps_for_tests();

  chashmap_reset_oa_probe_steps_for_tests();
  long long acc = 0;
  for (int i = 0; i < N; i++) {
    cmap_pair k = {&i, sizeof(i)};
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(m, &k, &out) == ccol_success && out)
      acc += *(int *)out->ptr;
  }
  unsigned long long lookup_probes = chashmap_oa_probe_steps_for_tests();

  size_t stored = chmap_elem_count(m);
  __chmap_destroy(m);

  REQUIRE_TRUE(all_inserted);
  /* Every key is distinct, so all of them must be present and readable however
     they were distributed; that separates a distribution failure from a
     correctness one. */
  REQUIRE_EQ(stored, (size_t)N);
  REQUIRE_EQ(acc, (long long)N * (N - 1) / 2);
  REQUIRE_LT((long long)insert_probes, (long long)N * 10);
  REQUIRE_LT((long long)lookup_probes, (long long)N * 10);
}

/* ========================================================================== */
/*                  GROW-PATH PROBE ACCOUNTING (open addressing)              */
/* ========================================================================== */

/* Counts every allocation the map makes, so a test can tell which insert grew
   the table without asking the map anything. An open-addressing map allocates
   on a rehash and on nothing else, which makes an allocation during an insert
   an exact, implementation-independent marker for "the table was rebuilt
   underneath this call". */
static long g_grow_probe_allocs = 0;
static void *_grow_probe_malloc(size_t n) {
  g_grow_probe_allocs++;
  return malloc(n);
}
static void *_grow_probe_calloc(size_t n, size_t sz) {
  g_grow_probe_allocs++;
  return calloc(n, sz);
}
static void *_grow_probe_realloc(void *p, size_t n) {
  g_grow_probe_allocs++;
  return realloc(p, n);
}

/* An insert that grows the table probes twice over: once against the table it
   was handed, and again against the rebuilt one, because the first probe's
   index and mask mean nothing once the capacity has changed. Both probes visit
   at least one slot, so such an insert can never account for fewer than two.
   That floor follows from two tables being probed rather than from any value
   the module owns, which is what keeps it from moving with the code it checks.

   The probe counter exists to catch a hash whose spread collapses (see
   low_bit_constant_keys_do_not_collapse_the_table), and it can only do that
   over the whole of an operation; a grow-path probe left out of it is a
   quadratic scan the guard cannot see.

   This test is non-vacuous: without the grow path's own count, six of the nine
   growing inserts below report a single probe step and it fails on the first
   of them. */
TEST(chashmap_open_addressing, a_growing_insert_counts_both_of_its_probes) {
  enum { N = 4000 };
  g_grow_probe_allocs = 0;
  ccol_memmgmt_procs_t mp = {.malloc = _grow_probe_malloc,
                             .calloc = _grow_probe_calloc,
                             .realloc = _grow_probe_realloc,
                             .free = free};
  chmap m = chmap_create_full(4, ccol_int, ccol_int, &mp, NULL, NULL, NULL);
  bool created = (m != NULL);

  int growing_inserts = 0;
  int undercounted_growing_inserts = 0;
  unsigned long long worst_growing_probe_count = 0;
  bool all_inserted = created;

  for (int i = 0; created && i < N; i++) {
    long allocs_before = g_grow_probe_allocs;
    chashmap_reset_oa_probe_steps_for_tests();

    cmap_pair kp = {&i, sizeof(i)};
    cmap_pair vp = {&i, sizeof(i)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) all_inserted = false;

    unsigned long long probes = chashmap_oa_probe_steps_for_tests();
    if (g_grow_probe_allocs != allocs_before) {
      growing_inserts++;
      if (probes < 2) {
        undercounted_growing_inserts++;
        if (worst_growing_probe_count == 0 ||
            probes < worst_growing_probe_count)
          worst_growing_probe_count = probes;
      }
    }
  }

  size_t stored = created ? chmap_elem_count(m) : 0;
  if (created) __chmap_destroy(m);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(all_inserted);
  REQUIRE_EQ(stored, (size_t)N);

  /* Without at least one grow there is nothing here to check, so the count is
     asserted rather than assumed. */
  REQUIRE_GT(growing_inserts, 0);
  REQUIRE_EQ(undercounted_growing_inserts, 0);
  REQUIRE_EQ((long long)worst_growing_probe_count, 0LL);
}

/* ------------------------------------------------------------------------- */
/* Macro argument lifetime                                                    */
/* ------------------------------------------------------------------------- */

/* A compound literal written as a macro argument has automatic storage
   duration bounded by the block it is instantiated in, so the type-inferred
   macros must bind the caller's expression in their own block before it reaches
   any nested one. Without that, the literal is created inside
   _populate_cmap_pair's own inner block, dies when that block closes, and the
   insert below copies out of a dead stack slot.

   These tests are non-vacuous at the optimization level this suite builds at:
   without the binding, gcc -O3 reuses the slot and the first one reads 64
   instead of 12345 while the rest fail to find the key at all. The defect is
   diagnosed regardless of compiler under AddressSanitizer, which reports it as
   a stack-use-after-scope read inside the insert. */
TEST(chmap_macro_arg_lifetime, a_compound_literal_value_survives_the_insert) {
  chmap_construct_scoped(hm, int, int);

  int key = 7;
  chmap_insert(hm, key, (int){12345});

  int *found = chmap_get_ptr(hm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) REQUIRE_EQ(*found, 12345);
}

TEST(chmap_macro_arg_lifetime, a_compound_literal_key_survives_the_insert) {
  chmap_construct_scoped(hm, int, int);

  int value = 900;
  chmap_insert(hm, (int){7}, value);

  int key = 7;
  int *found = chmap_get_ptr(hm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) REQUIRE_EQ(*found, 900);
}

TEST(chmap_macro_arg_lifetime, a_compound_literal_key_survives_every_lookup) {
  chmap_construct_scoped(hm, int, int);

  int key = 7, value = 900;
  chmap_insert(hm, key, value);

  int *found = chmap_get_ptr(hm, (int){7});
  REQUIRE_NE((void *)found, NULL);
  if (found) REQUIRE_EQ(*found, 900);

  /* chmap_get() runs only once chmap_get_ptr() has shown the key is
     present: an absent key is what a regression here produces, and
     chmap_get answers one with ccol_fatal_err, which would abort the
     whole binary and every other suite's result in the same run. */
  REQUIRE_EQ(chmap_get(hm, (int){7}), 900);

  REQUIRE_EQ(chmap_remove(hm, (int){7}), ccol_success);
  REQUIRE_EQ(chmap_elem_count(hm), (size_t)0);
}

/* The separate-chaining backend, reached by any value type wider than eight
   bytes, copies the value out of the same dead slot. */
typedef struct {
  long a;
  long b;
  long c;
  long d;
} chmap_lifetime_wide_value;

/* The literal is written behind a macro so its commas do not split the
   chmap_insert() argument list; it is substituted, and so instantiated,
   exactly where writing it inline would put it. */
#define CHMAP_LIFETIME_WIDE_LITERAL \
  (chmap_lifetime_wide_value){.a = 11, .b = 22, .c = 33, .d = 44}

TEST(chmap_macro_arg_lifetime, a_compound_literal_struct_value_survives) {
  chmap_construct_scoped(hm, int, chmap_lifetime_wide_value);

  int key = 7;
  chmap_insert(hm, key, CHMAP_LIFETIME_WIDE_LITERAL);

  chmap_lifetime_wide_value *found = chmap_get_ptr(hm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) {
    REQUIRE_EQ(found->a, 11L);
    REQUIRE_EQ(found->b, 22L);
    REQUIRE_EQ(found->c, 33L);
    REQUIRE_EQ(found->d, 44L);
  }
}

/* ------------------------------------------------------------------------- */
/* String value references                                                    */
/* ------------------------------------------------------------------------- */

/* A string-valued map owns the bytes it stores and keeps its own {ptr, size}
   accessor for the entry in step with them. The only char* object in the map is
   that accessor's own ptr field, so chmap_get_ptr() can only hand back its
   address, and storing a different char* through it would leave the size
   describing the previous string. The macro therefore yields a pointer whose
   target is const-qualified for a string-valued map, which makes that store a
   compile error.

   The check is on the result's TYPE rather than on any runtime effect, because
   the guarantee is exactly that the offending store never compiles. _Generic's
   controlling expression is not evaluated, so the map is not touched by it.
   Without the const the first association below does not match and the test
   reports 0. */
TEST(chmap_string_value_ref, get_ptr_on_a_string_map_is_not_writable) {
  chmap_construct_scoped(hm, char *, char *);

  char key[] = "k";
  chmap_insert(hm, key, "a value that is twenty-eight");

  REQUIRE_EQ(_Generic(chmap_get_ptr(hm, key),
                 char *const *: 1,
                 char **: 0,
                 default: -1),
             1);
}

/* The const is confined to the string case: every other value type keeps the
   writable pointer chmap_get_ptr() exists to provide. */
TEST(chmap_string_value_ref, get_ptr_on_a_non_string_map_stays_writable) {
  chmap_construct_scoped(hm, char *, int);

  char key[] = "k";
  int value = 1;
  chmap_insert(hm, key, value);

  REQUIRE_EQ(
      _Generic(chmap_get_ptr(hm, key), int *: 1, const int *: 0, default: -1),
      1);

  int *found = chmap_get_ptr(hm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) {
    *found = 200;
    REQUIRE_EQ(chmap_get(hm, key), 200);
  }
}

/* Reading the stored char*, and editing the bytes it points at within the
   stored length, both stay available; only replacing the pointer does not. */
TEST(chmap_string_value_ref, a_string_reference_reads_and_edits_in_place) {
  chmap_construct_scoped(hm, char *, char *);

  char key[] = "k";
  chmap_insert(hm, key, "hello");

  char *const *found = chmap_get_ptr(hm, key);
  REQUIRE_NE((void *)found, NULL);
  if (found) {
    REQUIRE_STREQ(*found, "hello");
    (*found)[0] = 'j';
    REQUIRE_STREQ(chmap_get(hm, key), "jello");

    /* The accessor's own size never falls out of step with the bytes it
       describes, so a copy-out reads exactly the stored string. */
    char buf[16] = {0};
    cmap_pair key_pair = {.ptr = key, .size = sizeof(key)};
    REQUIRE_EQ(chmap_get_elem_copy(hm, &key_pair, buf, sizeof(buf)),
               ccol_success);
    REQUIRE_STREQ(buf, "jello");
  }
}

/* The raw function layer carries the same rule as the macro layer, and carries
   it for every value type rather than only for strings: chmap_get_elem_ref()
   reports the map's own accessor through a const cmap_pair **, so
   val_pair->ptr and val_pair->size can be read and the bytes ptr describes can
   be edited in place, while assigning to either field is a compile error.
   tests/chashmap/compile_probe.sh asserts the rejected half, which is a
   property of the type that no run-time test can observe; this pins the half
   that must keep working. */
TEST(chmap_elem_ref_accessor, the_const_accessor_still_reads_and_edits_bytes) {
  chmap_construct_scoped(hm, char *, char *);

  char key[] = "k";
  chmap_insert(hm, key, "hello");

  cmap_pair key_pair = {.ptr = key, .size = sizeof(key)};
  const cmap_pair *val_pair = NULL;
  REQUIRE_EQ(chmap_get_elem_ref(hm, &key_pair, &val_pair), ccol_success);
  REQUIRE_NE((void *)val_pair, NULL);
  if (!val_pair) return;

  REQUIRE_EQ(val_pair->size, sizeof("hello"));
  REQUIRE_STREQ((const char *)val_pair->ptr, "hello");

  ((char *)val_pair->ptr)[0] = 'j';
  REQUIRE_STREQ(chmap_get(hm, key), "jello");

  char buf[8] = {0};
  REQUIRE_EQ(chmap_get_elem_copy(hm, &key_pair, buf, sizeof(buf)),
             ccol_success);
  REQUIRE_STREQ(buf, "jello");
}

/* The accessor is per entry, so a reference held for one key is not disturbed
   by looking any other key up, and several may be held at once. */
TEST(chmap_elem_ref_accessor, references_for_distinct_keys_are_independent) {
  chmap_construct_scoped(hm, int, int);

  for (int i = 0; i < 64; i++) {
    int k = i, v = i * 3;
    chmap_insert(hm, k, v);
  }

  int first = 7, second = 41;
  cmap_pair first_key = {.ptr = &first, .size = sizeof(first)};
  cmap_pair second_key = {.ptr = &second, .size = sizeof(second)};
  const cmap_pair *first_ref = NULL;
  const cmap_pair *second_ref = NULL;

  REQUIRE_EQ(chmap_get_elem_ref(hm, &first_key, &first_ref), ccol_success);
  REQUIRE_EQ(chmap_get_elem_ref(hm, &second_key, &second_ref), ccol_success);
  REQUIRE_NE((void *)first_ref, (void *)second_ref);

  for (int i = 0; i < 64; i++) {
    int other = i;
    cmap_pair other_key = {.ptr = &other, .size = sizeof(other)};
    const cmap_pair *ignored = NULL;
    (void)chmap_get_elem_ref(hm, &other_key, &ignored);
  }

  REQUIRE_EQ(*(const int *)first_ref->ptr, 21);
  REQUIRE_EQ(*(const int *)second_ref->ptr, 123);

  *(int *)first_ref->ptr = 999;
  REQUIRE_EQ(*(const int *)first_ref->ptr, 999);
  REQUIRE_EQ(*(const int *)second_ref->ptr, 123);
}

/* ========================================================================== */
/*          AN ITERATOR THAT OUTLIVES ITS CONTAINER UNTIL SCOPE EXIT          */
/* ========================================================================== */

/* An allocator that overwrites every block with a poison pattern before it
 * frees it, so that a read of freed memory gives the poison instead of the
 * stale value, and a use after free through a stale pointer faults in an
 * ordinary run without depending on a sanitizer to show. */
typedef union {
  size_t size;
  max_align_t align;
} chmap_life_poison_hdr;

static void *chmap_life_poison_malloc(size_t size) {
  chmap_life_poison_hdr *h = malloc(sizeof(chmap_life_poison_hdr) + size);
  if (!h) return NULL;
  h->size = size;
  return h + 1;
}

static void *chmap_life_poison_calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return NULL;
  void *p = chmap_life_poison_malloc(count * size);
  if (p) memset(p, 0, count * size);
  return p;
}

static void chmap_life_poison_free(void *ptr) {
  if (!ptr) return;
  chmap_life_poison_hdr *h = (chmap_life_poison_hdr *)ptr - 1;
  memset(ptr, 0xA5, h->size);
  free(h);
}

static void *chmap_life_poison_realloc(void *ptr, size_t size) {
  if (!ptr) return chmap_life_poison_malloc(size);
  chmap_life_poison_hdr *h = (chmap_life_poison_hdr *)ptr - 1;
  void *n = chmap_life_poison_malloc(size);
  if (!n) return NULL;
  memcpy(n, ptr, h->size < size ? h->size : size);
  chmap_life_poison_free(ptr);
  return n;
}

static ccol_memmgmt_procs_t chmap_life_poison_procs_storage = {
    .malloc = chmap_life_poison_malloc,
    .calloc = chmap_life_poison_calloc,
    .realloc = chmap_life_poison_realloc,
    .free = chmap_life_poison_free};

static ccol_memmgmt_procs_t *chmap_life_poison_procs(void) {
  return &chmap_life_poison_procs_storage;
}

/* A loop that leaves early keeps its iterator live until the end of the
 * scope of ccol_iter_declare, so destroying the map inside that scope must
 * leave the iterator freeable: the scope-exit cleanup frees it after the map
 * and its allocator record are gone. Both backends are covered. This test is
 * non-vacuous: an iterator that frees itself through the map reads the
 * poisoned map struct at scope exit and faults. */
TEST(iterator_lifetime, break_then_destroy_then_scope_exit_open_addressing) {
  int seen = 0;
  {
    chmap_construct_mp(m, int, int, chmap_life_poison_procs());
    for (int i = 0; i < 8; i++) chmap_insert(m, i, i * 10);
    ccol_iter_declare(m, it);
    for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
      if (++seen == 2) break;
    }
    chmap_destroy(m);
  }
  REQUIRE_EQ(seen, 2);
}

TEST(iterator_lifetime, break_then_destroy_then_scope_exit_separate_chaining) {
  int seen = 0;
  {
    chmap_construct_mp(m, char *, int, chmap_life_poison_procs());
    chmap_insert(m, "a", 1);
    chmap_insert(m, "b", 2);
    chmap_insert(m, "c", 3);
    ccol_iter_declare(m, it);
    for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
      if (++seen == 2) break;
    }
    chmap_destroy(m);
  }
  REQUIRE_EQ(seen, 2);
}

TEST(iterator_lifetime, break_then_destroy_then_scope_exit_default_allocator) {
  int seen = 0;
  {
    chmap_construct(m, char *, int);
    chmap_insert(m, "x", 1);
    chmap_insert(m, "y", 2);
    ccol_iter_declare(m, it);
    for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
      if (++seen == 1) break;
    }
    chmap_destroy(m);
  }
  REQUIRE_EQ(seen, 1);
}

/* ========================================================================== */
/*          OPEN ADDRESSING: CHURN REBUILDS IN PLACE, AND NEVER THRASHES      */
/* ========================================================================== */

unsigned long long chashmap_oa_rehashes_for_tests(void);

typedef struct {
  unsigned long long rehashes;
  size_t capacity_changes;
  size_t min_capacity;
  size_t max_capacity;
  bool all_found;
} chmap_churn_result;

/* A sliding window of `window` live keys: every step inserts the next key
 * and removes the oldest one, so the live count never changes and the table
 * has no reason to change its capacity at all. */
static chmap_churn_result chmap_churn_window(long window, long steps) {
  chmap_churn_result res = {0, 0, SIZE_MAX, 0, true};
  chmap_construct(m, long, long);
  for (long k = 0; k < window; k++) chmap_insert(m, k, k * 3);
  unsigned long long r0 = chashmap_oa_rehashes_for_tests();
  size_t prev = chmap_get_bucket_arr_size(m);
  for (long s = 0; s < steps; s++) {
    chmap_insert(m, window + s, (window + s) * 3);
    size_t c = chmap_get_bucket_arr_size(m);
    if (c != prev) res.capacity_changes++;
    prev = c;
    if (c < res.min_capacity) res.min_capacity = c;
    if (c > res.max_capacity) res.max_capacity = c;
    chmap_remove(m, s);
    c = chmap_get_bucket_arr_size(m);
    if (c != prev) res.capacity_changes++;
    prev = c;
    if (c < res.min_capacity) res.min_capacity = c;
    if (c > res.max_capacity) res.max_capacity = c;
  }
  res.rehashes = chashmap_oa_rehashes_for_tests() - r0;
  for (long k = steps; k < steps + window; k++) {
    long *v = chmap_get_ptr(m, k);
    if (!v || *v != k * 3) res.all_found = false;
  }
  if (chmap_elem_count(m) != (size_t)window) res.all_found = false;
  chmap_destroy(m);
  return res;
}

/* A delete moves the later entries of its cluster back and leaves no
 * deleted-slot marker, so the live count alone decides every resize. A
 * window whose live count never changes therefore never rebuilds, and keeps
 * one capacity. This test is non-vacuous: with deleted-slot markers that the
 * grow trigger counts, every window rebuilds its table repeatedly. */
TEST(oa_churn, a_sliding_window_never_rebuilds_the_table) {
  static const long windows[] = {1000, 4200, 6000, 11000};
  const long steps = 100000;
  for (size_t i = 0; i < sizeof(windows) / sizeof(*windows); i++) {
    chmap_churn_result r = chmap_churn_window(windows[i], steps);
    REQUIRE_TRUE(r.all_found);
    REQUIRE_EQ(r.capacity_changes, (size_t)0);
    REQUIRE_EQ((size_t)r.rehashes, (size_t)0);
  }
}

TEST(oa_churn, pure_insert_growth_and_pure_delete_shrink_are_unchanged) {
  chmap_construct(m, long, long);
  unsigned long long r0 = chashmap_oa_rehashes_for_tests();
  for (long k = 0; k < 100000; k++) chmap_insert(m, k, k);
  unsigned long long grow = chashmap_oa_rehashes_for_tests() - r0;
  size_t cap_full = chmap_get_bucket_arr_size(m);
  r0 = chashmap_oa_rehashes_for_tests();
  for (long k = 0; k < 100000; k++) chmap_remove(m, k);
  unsigned long long shrink = chashmap_oa_rehashes_for_tests() - r0;
  size_t cap_empty = chmap_get_bucket_arr_size(m);
  chmap_destroy(m);
  /* 16 -> 262144 is 14 doublings; the delete path halves 14 times. */
  REQUIRE_EQ(cap_full, (size_t)262144);
  REQUIRE_EQ((size_t)grow, (size_t)14);
  REQUIRE_EQ(cap_empty, (size_t)16);
  REQUIRE_EQ((size_t)shrink, (size_t)14);
}

TEST(oa_churn, a_steady_live_count_never_resizes) {
  /* 60 live keys in 128 slots, churned through 5000 inserts and deletes.
   * Nothing but the live count may drive a resize, so the table must stay at
   * 128 slots after every single operation. */
  chmap_construct(m, int, int);
  for (int k = 0; k < 60; k++) chmap_insert(m, k, k);
  size_t cap = chmap_get_bucket_arr_size(m);
  bool stayed = true;
  for (int s = 0; s < 5000; s++) {
    chmap_insert(m, 60 + s, s);
    if (chmap_get_bucket_arr_size(m) != cap) stayed = false;
    chmap_remove(m, s);
    if (chmap_get_bucket_arr_size(m) != cap) stayed = false;
  }
  chmap_destroy(m);
  REQUIRE_EQ(cap, (size_t)128);
  REQUIRE_TRUE(stayed);
}

/* ========================================================================== */
/*             CUSTOM KEY EQUALITY FOR A KEY WITH PADDING BYTES               */
/* ========================================================================== */

typedef struct {
  char tag;
  long id; /* padding sits between tag and id on every supported target */
} chmap_padded_key;

static unsigned long g_chmap_padded_eq_calls = 0;

static chmap_padded_key chmap_padded_key_read(const void *p) {
  chmap_padded_key k;
  memcpy(&k, p, sizeof(k));
  return k;
}

static unsigned long chmap_padded_hash(const void *p, size_t size) {
  (void)size;
  chmap_padded_key k = chmap_padded_key_read(p);
  return (unsigned long)k.id * 31ul + (unsigned long)(unsigned char)k.tag;
}

static bool chmap_padded_eq(const void *a, size_t as, const void *b,
                            size_t bs) {
  g_chmap_padded_eq_calls++;
  if (as != sizeof(chmap_padded_key) || bs != sizeof(chmap_padded_key)) {
    return false;
  }
  chmap_padded_key x = chmap_padded_key_read(a);
  chmap_padded_key y = chmap_padded_key_read(b);
  return x.tag == y.tag && x.id == y.id;
}

/* A key built by field assignment over storage that holds a garbage byte
 * pattern, so that two keys with the same fields differ in their padding. */
static chmap_padded_key chmap_padded_make(char tag, long id,
                                          unsigned char garbage) {
  chmap_padded_key k;
  memset(&k, garbage, sizeof(k));
  k.tag = tag;
  k.id = id;
  return k;
}

TEST(key_equality, a_padded_key_is_found_whatever_its_padding_holds) {
  chmap_construct_full(m, chmap_padded_key, int, NULL, chmap_padded_hash,
                       chmap_padded_eq);
  enum { N = 2000 };
  size_t cap_before = chmap_get_bucket_arr_size(m);
  for (int i = 0; i < N; i++) {
    chmap_padded_key k = chmap_padded_make((char)('a' + i % 26), i, 0xAB);
    chmap_insert(m, k, i);
  }
  size_t cap_after = chmap_get_bucket_arr_size(m);
  bool all_found = true;
  for (int i = 0; i < N; i++) {
    chmap_padded_key k = chmap_padded_make((char)('a' + i % 26), i, 0x5C);
    int *v = chmap_get_ptr(m, k);
    if (!v || *v != i) all_found = false;
  }
  /* An insert with different padding updates the one entry. */
  chmap_padded_key upd = chmap_padded_make('a', 0, 0x11);
  chmap_insert(m, upd, -7);
  size_t count_after_update = chmap_elem_count(m);
  chmap_padded_key probe = chmap_padded_make('a', 0, 0xEE);
  int updated = chmap_get(m, probe);
  /* A delete with yet another padding removes it. */
  chmap_padded_key del = chmap_padded_make('a', 0, 0x77);
  cmap_pair kp;
  _populate_cmap_pair(&kp, del);
  ccol_retval_t del_r = chmap_delete_elem(m, &kp);
  const cmap_pair *after_del = NULL;
  ccol_retval_t get_r = chmap_get_elem_ref(m, &kp, &after_del);
  /* A field that differs is a different key. */
  chmap_padded_key other = chmap_padded_make('b', 0, 0xAB);
  int *missing = chmap_get_ptr(m, other);
  unsigned long eq_calls = g_chmap_padded_eq_calls;
  size_t final_count = chmap_elem_count(m);
  chmap_destroy(m);

  REQUIRE_LT(cap_before, cap_after); /* the table grew and rehashed */
  REQUIRE_TRUE(all_found);
  REQUIRE_EQ(count_after_update, (size_t)N);
  REQUIRE_EQ(updated, -7);
  REQUIRE_EQ(del_r, ccol_success);
  REQUIRE_EQ(get_r, ccol_key_not_found);
  REQUIRE_EQ((void *)missing, NULL);
  REQUIRE_EQ(final_count, (size_t)(N - 1));
  REQUIRE_GT(eq_calls, 0UL);
}

static unsigned long chmap_mod100_hash(const void *p, size_t size) {
  (void)size;
  int v;
  memcpy(&v, p, sizeof(v));
  return (unsigned long)(v % 100);
}

static bool chmap_mod100_eq(const void *a, size_t as, const void *b,
                            size_t bs) {
  (void)as;
  (void)bs;
  int x, y;
  memcpy(&x, a, sizeof(x));
  memcpy(&y, b, sizeof(y));
  return x % 100 == y % 100;
}

/* The procedure also governs a map whose types would otherwise select open
 * addressing: here two ids are one key when they agree modulo 100. */
TEST(key_equality, an_integral_key_type_honours_the_procedure) {
  chmap_construct_full(m, int, int, NULL, chmap_mod100_hash, chmap_mod100_eq);
  chmap_insert(m, 5, 1);
  chmap_insert(m, 105, 2); /* the same key as 5 */
  chmap_insert(m, 6, 3);
  size_t count = chmap_elem_count(m);
  int v = chmap_get(m, 205);
  chmap_remove(m, 305);
  int *gone = chmap_get_ptr(m, 5);
  size_t final_count = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_EQ(count, (size_t)2);
  REQUIRE_EQ(v, 2);
  REQUIRE_EQ((void *)gone, NULL);
  REQUIRE_EQ(final_count, (size_t)1);
}

TEST(key_equality, an_equality_proc_without_a_hashing_proc_is_refused) {
  char *err = NULL;
  chmap m = chmap_create_full(16, ccol_other_types, ccol_int, NULL, NULL,
                              chmap_padded_eq, &err);
  bool refused = m == NULL;
  bool has_err = err != NULL;
  if (m) chmap_destroy(m);
  REQUIRE_TRUE(refused);
  REQUIRE_TRUE(has_err);
}

/* With no procedure, a key with padding is compared byte for byte. Through
 * the raw functions, the map reads exactly the bytes of the object that the
 * caller hands it, so a key object that the caller zeroes and then fills in
 * is found again, unlike a struct returned by value, whose padding does not
 * survive the return. */
TEST(key_equality, a_zeroed_padded_key_works_through_the_raw_functions) {
  chmap_construct(m, chmap_padded_key, int);
  bool all_inserted = true;
  for (int i = 0; i < 100; i++) {
    chmap_padded_key k;
    memset(&k, 0, sizeof(k));
    k.tag = 'x';
    k.id = i;
    cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
    cmap_pair vp = {.ptr = &i, .size = sizeof(i)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) all_inserted = false;
  }
  bool all_found = true;
  for (int i = 0; i < 100; i++) {
    chmap_padded_key k;
    memset(&k, 0, sizeof(k));
    k.tag = 'x';
    k.id = i;
    cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
    const cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(m, &kp, &vp) != ccol_success || !vp ||
        *(const int *)vp->ptr != i) {
      all_found = false;
    }
  }
  chmap_destroy(m);
  REQUIRE_TRUE(all_inserted);
  REQUIRE_TRUE(all_found);
}

#if defined(__has_builtin)
#if __has_builtin(__builtin_clear_padding)
#define CHMAP_TEST_COMPILER_CLEARS_PADDING 1
#endif
#endif

#ifdef CHMAP_TEST_COMPILER_CLEARS_PADDING
/* Fills the stack below the caller with a non-zero pattern, so that a copy
 * which leaves its padding untouched shows it as non-zero bytes. */
static __attribute__((noinline)) void chmap_padded_dirty_stack(void) {
  volatile unsigned char buf[4096];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = 0xAB;
}

static __attribute__((noinline)) void chmap_padded_macro_insert(chmap m,
                                                                int i) {
  chmap_redeclare(m, chmap_padded_key, int);
  chmap_insert(m, chmap_padded_make('x', i, 0), i);
}

static __attribute__((noinline)) int *chmap_padded_macro_lookup(chmap m,
                                                                int i) {
  chmap_redeclare(m, chmap_padded_key, int);
  return chmap_get_ptr(m, chmap_padded_make('x', i, 0x5C));
}
#endif

/* Where the compiler offers __builtin_clear_padding, the macros clear the
 * padding of their own copy of the key, so a key needs no procedure and no
 * zeroing, whatever the compiler does with the padding of a struct copy. This
 * test is non-vacuous under GCC: without the clearing, the stored keys carry
 * the stack pattern in their padding and the lookups miss. */
TEST(key_equality, the_macros_clear_the_padding_of_their_key_copy) {
#ifdef CHMAP_TEST_COMPILER_CLEARS_PADDING
  chmap_construct(m, chmap_padded_key, int);
  for (int i = 0; i < 100; i++) {
    chmap_padded_dirty_stack();
    chmap_padded_macro_insert(m, i);
  }
  bool all_found = true;
  for (int i = 0; i < 100; i++) {
    chmap_padded_dirty_stack();
    int *v = chmap_padded_macro_lookup(m, i);
    if (!v || *v != i) all_found = false;
  }
  bool padding_zero = true;
  size_t pad_start = offsetof(chmap_padded_key, tag) + 1;
  size_t pad_end = offsetof(chmap_padded_key, id);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
    const unsigned char *b = (const unsigned char *)it->key_pair->ptr;
    for (size_t j = pad_start; j < pad_end; j++) {
      if (b[j] != 0) padding_zero = false;
    }
  }
  size_t n = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_TRUE(all_found);
  REQUIRE_TRUE(padding_zero);
  REQUIRE_EQ(n, (size_t)100);
#endif
}

/* ========================================================================== */
/*              INTERNAL: INSERT, OR GET THE ENTRY THAT IS PRESENT            */
/* ========================================================================== */

#include <internal/chashinsert.h>

TEST(insert_or_get, an_absent_key_is_inserted_and_a_present_one_is_untouched) {
  chmap_construct(m, char *, long);
  const char *k = "alpha";
  long v1 = 10, v2 = 20;
  cmap_pair kp = {.ptr = (void *)k, .size = strlen(k) + 1};
  cmap_pair vp1 = {.ptr = &v1, .size = sizeof(v1)};
  cmap_pair vp2 = {.ptr = &v2, .size = sizeof(v2)};
  const cmap_pair *existing = (const cmap_pair *)&kp; /* a poison value */
  ccol_retval_t r1 = ccol_chmap_insert_or_get_elem(m, &kp, &vp1, &existing);
  const cmap_pair *after_insert = existing;
  long inserted_seen = after_insert ? *(const long *)after_insert->ptr : -1;
  existing = (const cmap_pair *)&kp;
  ccol_retval_t r2 = ccol_chmap_insert_or_get_elem(m, &kp, &vp2, &existing);
  long seen = existing ? *(const long *)existing->ptr : -1;
  size_t seen_size = existing ? existing->size : 0;
  long stored = chmap_get(m, "alpha");
  size_t count = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(inserted_seen, 10L); /* the slot of the new entry */
  REQUIRE_EQ(r2, ccol_key_already_present);
  REQUIRE_EQ(seen, 10L);
  REQUIRE_EQ(seen_size, sizeof(long));
  REQUIRE_EQ(stored, 10L); /* the second value was not written */
  REQUIRE_EQ(count, (size_t)1);
}

/* The use that the entry point exists for: a value that owns a heap object,
 * where the caller frees the old object and stores the new pointer in place. */
TEST(insert_or_get, an_owning_pointer_value_is_replaced_in_one_pass) {
  enum { N = 300 };
  chmap_construct(m, char *, void *);
  char key[32];
  size_t replaced = 0;
  for (int round = 0; round < 2; round++) {
    for (int i = 0; i < N; i++) {
      snprintf(key, sizeof(key), "key-%d", i);
      int *child = malloc(sizeof(int));
      *child = round * 1000 + i;
      cmap_pair kp = {.ptr = key, .size = strlen(key) + 1};
      cmap_pair vp = {.ptr = &child, .size = sizeof(child)};
      const cmap_pair *existing = NULL;
      ccol_retval_t r = ccol_chmap_insert_or_get_elem(m, &kp, &vp, &existing);
      if (r == ccol_key_already_present) {
        void *old;
        memcpy(&old, existing->ptr, sizeof(old));
        free(old);
        memcpy((void *)existing->ptr, &child, sizeof(child));
        replaced++;
      }
    }
  }
  bool all_new = true;
  for (int i = 0; i < N; i++) {
    snprintf(key, sizeof(key), "key-%d", i);
    void *p = chmap_get(m, key);
    if (*(int *)p != 1000 + i) all_new = false;
    free(p);
  }
  size_t count = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_EQ(replaced, (size_t)N);
  REQUIRE_TRUE(all_new);
  REQUIRE_EQ(count, (size_t)N);
}

TEST(insert_or_get, each_call_costs_one_probe_pass_on_open_addressing) {
  chmap_construct(m, int, int);
  for (int i = 0; i < 1000; i++) chmap_insert(m, i, i);
  int k = 777, v = -1;
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
  const cmap_pair *ref = NULL;
  chashmap_reset_oa_probe_steps_for_tests();
  ccol_retval_t rg = chmap_get_elem_ref(m, &kp, &ref);
  unsigned long long get_probes = chashmap_oa_probe_steps_for_tests();
  const cmap_pair *existing = NULL;
  chashmap_reset_oa_probe_steps_for_tests();
  ccol_retval_t ri = ccol_chmap_insert_or_get_elem(m, &kp, &vp, &existing);
  unsigned long long upsert_probes = chashmap_oa_probe_steps_for_tests();
  bool same_slot = existing == ref;
  int stored = chmap_get(m, 777);
  chmap_destroy(m);

  /* An absent key: the same insert into two maps of identical content costs
   * the same probes through either entry point. */
  chmap_construct(a, int, int);
  chmap_construct(b, int, int);
  for (int i = 0; i < 1000; i++) {
    chmap_insert(a, i * 7, i);
    chmap_insert(b, i * 7, i);
  }
  int nk = 5001, nv = 9;
  cmap_pair nkp = {.ptr = &nk, .size = sizeof(nk)};
  cmap_pair nvp = {.ptr = &nv, .size = sizeof(nv)};
  chashmap_reset_oa_probe_steps_for_tests();
  ccol_retval_t ra = chmap_insert_elem(a, &nkp, &nvp);
  unsigned long long insert_probes = chashmap_oa_probe_steps_for_tests();
  const cmap_pair *none = NULL;
  chashmap_reset_oa_probe_steps_for_tests();
  ccol_retval_t rb = ccol_chmap_insert_or_get_elem(b, &nkp, &nvp, &none);
  unsigned long long upsert_new_probes = chashmap_oa_probe_steps_for_tests();
  int got_b = chmap_get(b, 5001);
  chmap_destroy(a);
  chmap_destroy(b);

  REQUIRE_EQ(rg, ccol_success);
  REQUIRE_EQ(ri, ccol_key_already_present);
  REQUIRE_EQ((size_t)upsert_probes, (size_t)get_probes);
  REQUIRE_TRUE(same_slot);
  REQUIRE_EQ(stored, 777);
  REQUIRE_EQ(ra, ccol_success);
  REQUIRE_EQ(rb, ccol_success);
  REQUIRE_NE((void *)none, NULL);
  REQUIRE_EQ((size_t)upsert_new_probes, (size_t)insert_probes);
  REQUIRE_EQ(got_b, 9);
}

TEST(insert_or_get, a_rejected_call_leaves_the_out_parameter_null) {
  chmap_construct(m, int, int);
  int k = 1, v = 2;
  short wrong = 3;
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair bad_kp = {.ptr = &wrong, .size = sizeof(wrong)};
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
  const cmap_pair *existing = (const cmap_pair *)&kp; /* a poison value */
  ccol_retval_t r_bad =
      ccol_chmap_insert_or_get_elem(m, &bad_kp, &vp, &existing);
  const cmap_pair *after_bad = existing;
  ccol_retval_t r_null = ccol_chmap_insert_or_get_elem(m, &kp, &vp, NULL);
  size_t count = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_EQ(r_bad, ccol_invalid_args);
  REQUIRE_EQ((void *)after_bad, NULL);
  REQUIRE_EQ(r_null, ccol_invalid_args);
  REQUIRE_EQ(count, (size_t)0);
}

/* Inserts every key with a placeholder value, then writes the real value
 * through the slot that the same call returned. Because the loop crosses
 * several grows, including grows that the insert of the key itself triggers,
 * the slot must be the one of the rebuilt table. Returns true when every key
 * reads back its written value and every call reported the slot of its own
 * entry. */
static bool chmap_upsert_write_through(chmap m, int n, bool *saw_grow) {
  *saw_grow = false;
  bool ok = true;
  for (int i = 0; i < n; i++) {
    long key = (long)i * 13 + 1;
    long placeholder = 0;
    long real = key * 7;
    cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
    cmap_pair vp = {.ptr = &placeholder, .size = sizeof(placeholder)};
    size_t cap_before = chmap_get_bucket_arr_size(m);
    const cmap_pair *slot = NULL;
    if (ccol_chmap_insert_or_get_elem(m, &kp, &vp, &slot) != ccol_success ||
        !slot || slot->size != sizeof(long)) {
      ok = false;
      continue;
    }
    if (chmap_get_bucket_arr_size(m) != cap_before) *saw_grow = true;
    memcpy((void *)slot->ptr, &real, sizeof(real));
    const cmap_pair *ref = NULL;
    if (chmap_get_elem_ref(m, &kp, &ref) != ccol_success || ref != slot) {
      ok = false;
    }
  }
  for (int i = 0; i < n; i++) {
    long key = (long)i * 13 + 1;
    cmap_pair kp = {.ptr = &key, .size = sizeof(key)};
    const cmap_pair *ref = NULL;
    long v = 0;
    if (chmap_get_elem_ref(m, &kp, &ref) != ccol_success) {
      ok = false;
      continue;
    }
    memcpy(&v, ref->ptr, sizeof(v));
    if (v != key * 7) ok = false;
  }
  return ok;
}

TEST(insert_or_get, the_slot_of_a_new_key_is_writable_on_open_addressing) {
  chmap_construct(m, long, long);
  bool grew = false;
  bool ok = chmap_upsert_write_through(m, 5000, &grew);
  size_t n = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_TRUE(grew);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(n, (size_t)5000);
}

static unsigned long chmap_upsert_hash_long(const void *p, size_t size) {
  (void)size;
  long v;
  memcpy(&v, p, sizeof(v));
  return (unsigned long)v;
}

static bool chmap_upsert_eq_long(const void *a, size_t as, const void *b,
                                 size_t bs) {
  (void)as;
  (void)bs;
  long x, y;
  memcpy(&x, a, sizeof(x));
  memcpy(&y, b, sizeof(y));
  return x == y;
}

TEST(insert_or_get, the_slot_of_a_new_key_is_writable_on_separate_chaining) {
  /* A key equality procedure makes a long-to-long map use separate
   * chaining, so the same helper exercises the other backend. */
  char *err = NULL;
  chmap m =
      chmap_create_full(16, ccol_long, ccol_long, NULL, chmap_upsert_hash_long,
                        chmap_upsert_eq_long, &err);
  REQUIRE_NE((void *)m, NULL);
  bool grew = false;
  bool ok = chmap_upsert_write_through(m, 5000, &grew);
  size_t n = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_TRUE(grew);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(n, (size_t)5000);
}

/* ========================================================================== */
/*               OPEN ADDRESSING: BACKWARD-SHIFT DELETION                     */
/* ========================================================================== */

size_t chashmap_oa_home_slot_for_tests(chmap chm, const void *key_ptr,
                                       size_t key_size);
bool chashmap_oa_check_invariants_for_tests(chmap chm);
unsigned long long chashmap_oa_shift_moves_for_tests(void);

/* The first n keys, from start upward, whose home slot in m is home. */
static long bs_keys_with_home(chmap m, size_t home, size_t n, long start,
                              long *out) {
  long k = start;
  for (size_t found = 0; found < n; k++) {
    if (chashmap_oa_home_slot_for_tests(m, &k, sizeof(k)) == home) {
      out[found++] = k;
    }
  }
  return k;
}

static bool bs_all_present(chmap m, const long *keys, const bool *present,
                           size_t n) {
  chmap_redeclare(m, long, long);
  for (size_t i = 0; i < n; i++) {
    long *v = chmap_get_ptr(m, keys[i]);
    if (present[i] ? (!v || *v != keys[i] * 3) : v != NULL) return false;
  }
  return chashmap_oa_check_invariants_for_tests(m);
}

/* Builds one cluster from a list of home slots, then deletes one key at a
 * time in the given order, checking every key and the structure after each
 * delete. Gives false at the first failure. */
static bool bs_cluster_case(const size_t *homes, size_t n,
                            const size_t *delete_order) {
  chmap m = chmap_create(64, ccol_long, ccol_long, NULL);
  chmap_redeclare(m, long, long);
  long keys[32];
  bool present[32];
  long next = 1;
  for (size_t i = 0; i < n; i++) {
    next = bs_keys_with_home(m, homes[i], 1, next, &keys[i]);
    present[i] = true;
  }
  /* 20 filler keys far from the cluster keep the load above the shrink
   * threshold, so the capacity stays at 64 throughout. */
  long filler[20];
  bool filler_present[20];
  long fnext = 1000000;
  for (size_t i = 0; i < 20; i++) {
    fnext = bs_keys_with_home(m, 20 + i, 1, fnext, &filler[i]);
    filler_present[i] = true;
    chmap_insert(m, filler[i], filler[i] * 3);
  }
  for (size_t i = 0; i < n; i++) chmap_insert(m, keys[i], keys[i] * 3);
  bool ok =
      chmap_get_bucket_arr_size(m) == 64 && bs_all_present(m, keys, present, n);
  for (size_t d = 0; ok && d < n; d++) {
    size_t victim = delete_order[d];
    ok = chmap_remove(m, keys[victim]) == ccol_success;
    present[victim] = false;
    ok = ok && chmap_get_bucket_arr_size(m) == 64 &&
         bs_all_present(m, keys, present, n) &&
         bs_all_present(m, filler, filler_present, 20);
  }
  chmap_destroy(m);
  return ok;
}

TEST(backward_shift, deletes_at_the_head_middle_and_tail_of_a_cluster) {
  /* One cluster at slots 10 to 17: three keys home at 10, two at 11, one at
   * 13, one at 12 and one at 17, several of them past their home slot. */
  static const size_t homes[] = {10, 10, 10, 11, 11, 13, 12, 17};
  static const size_t head_first[] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const size_t tail_first[] = {7, 6, 5, 4, 3, 2, 1, 0};
  static const size_t middle_first[] = {3, 5, 1, 6, 0, 7, 2, 4};
  REQUIRE_TRUE(bs_cluster_case(homes, 8, head_first));
  REQUIRE_TRUE(bs_cluster_case(homes, 8, tail_first));
  REQUIRE_TRUE(bs_cluster_case(homes, 8, middle_first));
}

TEST(backward_shift, a_cluster_that_wraps_past_the_last_slot) {
  /* Four keys home at 62 and three at 63 fill slots 62 to 63 and 0 to 4, and
   * two keys home at 0 and one at 2 land after them, so every key past slot
   * 63 has a home slot before the start of the array. */
  static const size_t homes[] = {62, 62, 63, 62, 63, 0, 62, 63, 2, 0};
  static const size_t orders[3][10] = {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                                       {9, 8, 7, 6, 5, 4, 3, 2, 1, 0},
                                       {4, 7, 1, 9, 0, 5, 8, 2, 6, 3}};
  for (size_t o = 0; o < 3; o++) {
    REQUIRE_TRUE(bs_cluster_case(homes, 10, orders[o]));
  }
}

TEST(backward_shift, an_entry_never_moves_before_its_home_slot) {
  /* Keys home at 40, 40, 42, 42, 44: deleting the second key of home 40
   * must leave the keys of home 42 at or after slot 42, although the gap at
   * slot 41 is before them. */
  static const size_t homes[] = {40, 40, 42, 42, 44, 41};
  static const size_t order_a[] = {1, 0, 5, 2, 3, 4};
  static const size_t order_b[] = {5, 1, 3, 0, 4, 2};
  REQUIRE_TRUE(bs_cluster_case(homes, 6, order_a));
  REQUIRE_TRUE(bs_cluster_case(homes, 6, order_b));
}

/* A differential test against a plain reference. The key pool is built so
 * that most keys home in a narrow band that wraps past the last slot of a
 * 64-slot table, which makes long clusters. The first phase keeps the live
 * count between the shrink and the grow thresholds, so the capacity stays at
 * 64; the second phase lets the table grow and shrink. After every delete,
 * every key of the pool must be found with its value or be absent, and every
 * probe sequence must be intact. */
TEST(backward_shift, a_randomized_differential_run_against_a_reference) {
  enum { POOL = 160 };
  chmap m = chmap_create(64, ccol_long, ccol_long, NULL);
  chmap_redeclare(m, long, long);
  long keys[POOL];
  bool present[POOL] = {false};
  long next = 1;
  for (size_t i = 0; i < POOL; i++) {
    size_t home = (i % 4 == 3) ? (i * 7) % 64 : (60 + i % 8) % 64;
    next = bs_keys_with_home(m, home, 1, next, &keys[i]);
  }
  unsigned st = 12345u;
  size_t live = 0;
  bool ok = true;
  size_t deletes = 0, iterations = 0;
  for (int phase = 0; phase < 2 && ok; phase++) {
    for (int op = 0; op < 20000 && ok; op++) {
      st = st * 1103515245u + 12345u;
      size_t i = (st >> 8) % POOL;
      unsigned kind = (st >> 20) % 10;
      bool may_insert = phase == 1 || live < 44 || present[i];
      bool may_delete = phase == 1 || live > 17;
      if (kind < 5 && may_insert) {
        if (!present[i]) live++;
        present[i] = true;
        chmap_insert(m, keys[i], keys[i] * 3);
      } else if (kind < 8 && may_delete) {
        ccol_retval_t r = chmap_remove(m, keys[i]);
        if (r != (present[i] ? ccol_success : ccol_key_not_found)) ok = false;
        if (present[i]) live--;
        present[i] = false;
        deletes++;
        ok = ok && bs_all_present(m, keys, present, POOL);
      } else if (kind == 8) {
        long *v = chmap_get_ptr(m, keys[i]);
        if (present[i] ? (!v || *v != keys[i] * 3) : v != NULL) ok = false;
      } else {
        size_t seen = 0;
        long sum = 0, want = 0;
        ccol_iter_declare(m, it);
        for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
          seen++;
          sum += *ccol_iter_val_ptr(it);
        }
        for (size_t k = 0; k < POOL; k++) {
          if (present[k]) want += keys[k] * 3;
        }
        if (seen != live || sum != want) ok = false;
        iterations++;
      }
      if (phase == 0 && chmap_get_bucket_arr_size(m) != 64) ok = false;
      if (chmap_elem_count(m) != live) ok = false;
    }
    if (phase == 1) {
      /* Drain the table, which also shrinks it, checking each delete. */
      for (size_t i = 0; i < POOL && ok; i++) {
        if (!present[i]) continue;
        ok = chmap_remove(m, keys[i]) == ccol_success;
        present[i] = false;
        live--;
        ok = ok && bs_all_present(m, keys, present, POOL);
      }
    }
  }
  size_t final_count = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_GT(deletes, (size_t)1000);
  REQUIRE_GT(iterations, (size_t)100);
  REQUIRE_EQ(final_count, (size_t)0);
}

TEST(backward_shift, a_delete_in_a_cluster_moves_entries_and_one_alone_none) {
  chmap m = chmap_create(64, ccol_long, ccol_long, NULL);
  chmap_redeclare(m, long, long);
  long lone, cl[3];
  bs_keys_with_home(m, 5, 1, 1, &lone);
  bs_keys_with_home(m, 30, 3, 1, cl);
  long filler[16];
  long fnext = 1000000;
  for (size_t i = 0; i < 16; i++) {
    fnext = bs_keys_with_home(m, 40 + i, 1, fnext, &filler[i]);
    chmap_insert(m, filler[i], 0);
  }
  chmap_insert(m, lone, 1);
  for (size_t i = 0; i < 3; i++) chmap_insert(m, cl[i], 2);
  unsigned long long before = chashmap_oa_shift_moves_for_tests();
  chmap_remove(m, lone);
  unsigned long long after_lone = chashmap_oa_shift_moves_for_tests();
  chmap_remove(m, cl[0]);
  unsigned long long after_head = chashmap_oa_shift_moves_for_tests();
  bool ok = chashmap_oa_check_invariants_for_tests(m);
  chmap_destroy(m);
  REQUIRE_EQ((size_t)(after_lone - before), (size_t)0);
  REQUIRE_EQ((size_t)(after_head - after_lone), (size_t)2);
  REQUIRE_TRUE(ok);
}

/* ========================================================================== */
/*     HASH FLOODING: SECRET SEEDS, AND THE KEYED MODE THAT A FLOOD SELECTS   */
/* ========================================================================== */

extern void chashmap_sip_state_for_tests(uint64_t k0, uint64_t k1,
                                         uint64_t sip_v[4]);
extern uint64_t chashmap_siphash13_for_tests(const uint64_t sip_v[4],
                                             const void *key_ptr,
                                             size_t key_size);
extern size_t chashmap_byte_hash_for_tests(const uint64_t sip_v[4],
                                           const void *key_ptr,
                                           size_t key_size);
extern void chashmap_hash_secret_for_tests(chmap chm, uint64_t out[6]);
extern void chashmap_fallback_hash_key_for_tests(uint64_t out[4]);
extern size_t chashmap_sc_longest_chain_for_tests(chmap chm);
extern size_t chashmap_sc_stored_hash_for_tests(chmap chm, const void *key_ptr,
                                                size_t key_size);

enum { FLOOD_KEYS = 600, FLOOD_KEY_LEN = 24, FLOOD_TOP_BITS = 10 };

/* Fills keys with FLOOD_KEYS distinct strings whose byte hash under the
 * SipHash key (0, 0) has its top FLOOD_TOP_BITS bits clear. That is what a
 * party who knows the key computes offline: with at most 2^FLOOD_TOP_BITS
 * buckets, every one of those keys lands in bucket 0 of a map that hashes
 * with that key. */
static void flood_keys_for_key_zero(char (*keys)[FLOOD_KEY_LEN]) {
  const unsigned shift = (unsigned)(sizeof(size_t) * 8 - FLOOD_TOP_BITS);
  uint64_t zero_state[4];
  chashmap_sip_state_for_tests(0, 0, zero_state);
  unsigned long long ctr = 0;
  for (size_t got = 0; got < FLOOD_KEYS;) {
    char k[FLOOD_KEY_LEN];
    int kl = snprintf(k, sizeof(k), "k%llx", ctr++);
    if ((chashmap_byte_hash_for_tests(zero_state, k, (size_t)kl + 1) >>
         shift) != 0) {
      continue;
    }
    memcpy(keys[got++], k, (size_t)kl + 1);
  }
}

static bool hash_secret_is_zero(const uint64_t secret[6]) {
  for (int i = 0; i < 6; i++) {
    if (secret[i] != 0) return false;
  }
  return true;
}

/* Fills keys with FLOOD_KEYS distinct strings whose XXH64 under the seed 0,
 * the byte hash of the fast mode for a party who guesses the seed, has its
 * top FLOOD_TOP_BITS bits clear. */
static void flood_keys_for_xxh_seed_zero(char (*keys)[FLOOD_KEY_LEN]) {
  extern uint64_t chashmap_xxh64_for_tests(const void *key_ptr, size_t key_size,
                                           uint64_t seed);
  unsigned long long ctr = 0;
  for (size_t got = 0; got < FLOOD_KEYS;) {
    char k[FLOOD_KEY_LEN];
    int kl = snprintf(k, sizeof(k), "x%llx", ctr++);
    uint64_t h = chashmap_xxh64_for_tests(k, (size_t)kl + 1, 0);
#if SIZE_MAX != UINT64_MAX
    h = (uint64_t)(uint32_t)(h ^ (h >> 32)) << 32;
#endif
    if ((h >> (64 - FLOOD_TOP_BITS)) != 0) {
      continue;
    }
    memcpy(keys[got++], k, (size_t)kl + 1);
  }
}

/* The fast byte hash is XXH64 with a secret seed, so keys that a party
 * computes for a seed that it guesses spread over the buckets as any other
 * keys do, and the map has no reason to switch. This test is non-vacuous:
 * with a byte_seed of 0, all 600 keys share bucket 0 and the map switches at
 * the twenty-first. */
TEST(hash_seed, keys_that_collide_for_a_guessed_xxh_seed_stay_fast_and_spread) {
  static char keys[FLOOD_KEYS][FLOOD_KEY_LEN];
  extern bool chashmap_is_keyed_for_tests(chmap chm);
  flood_keys_for_xxh_seed_zero(keys);
  chmap m = chmap_create(16, ccol_string, ccol_int, NULL);
  bool all_inserted = true;
  for (size_t i = 0; i < FLOOD_KEYS; i++) {
    int v = (int)i;
    cmap_pair kp = {.ptr = keys[i], .size = strlen(keys[i]) + 1};
    cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) all_inserted = false;
  }
  size_t buckets = chmap_get_bucket_arr_size(m);
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  bool keyed = chashmap_is_keyed_for_tests(m);
  chmap_destroy(m);
  REQUIRE_TRUE(all_inserted);
  REQUIRE_LE(buckets, (size_t)1 << FLOOD_TOP_BITS);
  REQUIRE_LE(longest, (size_t)16);
  REQUIRE_FALSE(keyed);
}

/* The keyed mode hashes a byte key with SipHash-1-3 under a secret 128-bit
 * key, so keys that a party computes for the known key (0, 0) spread over the
 * buckets of a keyed map. This test is non-vacuous: with the keyed byte hash
 * keyed by the constant key (0, 0), all 600 keys share one bucket and the
 * longest chain is 600. */
TEST(hash_seed, keys_that_collide_for_a_known_key_spread_over_the_buckets) {
  static char keys[FLOOD_KEYS][FLOOD_KEY_LEN];
  extern ccol_retval_t chashmap_switch_to_keyed_for_tests(chmap chm);
  flood_keys_for_key_zero(keys);
  chmap m = chmap_create(16, ccol_string, ccol_int, NULL);
  chmap_redeclare(m, char *, int);
  bool all_inserted = chashmap_switch_to_keyed_for_tests(m) == ccol_success;
  for (size_t i = 0; i < FLOOD_KEYS; i++) {
    int v = (int)i;
    cmap_pair kp = {.ptr = keys[i], .size = strlen(keys[i]) + 1};
    cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) all_inserted = false;
  }
  size_t buckets = chmap_get_bucket_arr_size(m);
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  uint64_t secret[6];
  chashmap_hash_secret_for_tests(m, secret);
  chmap_destroy(m);
  REQUIRE_TRUE(all_inserted);
  /* The premise: the table is small enough that the key (0, 0) puts every
   * key in bucket 0. */
  REQUIRE_LE(buckets, (size_t)1 << FLOOD_TOP_BITS);
  /* 600 keys over 1024 buckets: the longest chain of a random hash is about
   * 5. 16 leaves a wide margin and is far below the 600 of a known key. */
  REQUIRE_LE(longest, (size_t)16);
  REQUIRE_FALSE(hash_secret_is_zero(secret));
}

/* Every map of the process, of either backend, reads one secret. A fast map
 * stores XXH64 under byte_seed, and a keyed map stores the hash that
 * ccol_chmap_hash_key gives, SipHash-1-3 under the secret key, whichever map
 * it is. clrucache depends on ccol_chmap_hash_key to route a key to the
 * segment whose map owns it, so that hash must not depend on the mode of
 * any map. */
TEST(hash_seed, every_map_and_ccol_chmap_hash_key_share_one_secret) {
  extern uint64_t chashmap_xxh64_for_tests(const void *key_ptr, size_t key_size,
                                           uint64_t seed);
  extern ccol_retval_t chashmap_switch_to_keyed_for_tests(chmap chm);
  chmap a = chmap_create(16, ccol_string, ccol_int, NULL);
  chmap b = chmap_create(1024, ccol_string, ccol_long, NULL);
  chmap c = chmap_create(16, ccol_long, ccol_long, NULL);
  chmap_redeclare(a, char *, int);
  chmap_redeclare(b, char *, long);
  chmap_insert(a, "shared-key", 1);
  chmap_insert(b, "shared-key", 2L);
  const char *k = "shared-key";
  size_t fa = chashmap_sc_stored_hash_for_tests(a, k, strlen(k) + 1);
  size_t fb = chashmap_sc_stored_hash_for_tests(b, k, strlen(k) + 1);
  bool switched = chashmap_switch_to_keyed_for_tests(a) == ccol_success &&
                  chashmap_switch_to_keyed_for_tests(b) == ccol_success;
  size_t ha = chashmap_sc_stored_hash_for_tests(a, k, strlen(k) + 1);
  size_t hb = chashmap_sc_stored_hash_for_tests(b, k, strlen(k) + 1);
  uint64_t sa[6], sb[6], sc[6];
  chashmap_hash_secret_for_tests(a, sa);
  chashmap_hash_secret_for_tests(b, sb);
  chashmap_hash_secret_for_tests(c, sc);
  chmap_destroy(a);
  chmap_destroy(b);
  chmap_destroy(c);
  size_t h = ccol_chmap_hash_key(k, strlen(k) + 1, ccol_string);
  uint64_t zero_state[4];
  chashmap_sip_state_for_tests(0, 0, zero_state);
  size_t h0 = chashmap_byte_hash_for_tests(zero_state, k, strlen(k) + 1);
  uint64_t x = chashmap_xxh64_for_tests(k, strlen(k) + 1, sa[5]);
#if SIZE_MAX == UINT64_MAX
  size_t fast = (size_t)x;
#else
  size_t fast = (size_t)(x ^ (x >> 32));
#endif
  REQUIRE_TRUE(switched);
  REQUIRE_EQ(memcmp(sa, sb, sizeof(sa)), 0);
  /* The open-addressing map holds only the integer seed. */
  REQUIRE_EQ(sc[4], sa[4]);
  REQUIRE_EQ(fa, fast);
  REQUIRE_EQ(fb, fast);
  REQUIRE_EQ(ha, h);
  REQUIRE_EQ(hb, h);
  REQUIRE_EQ(h, chashmap_byte_hash_for_tests(sa, k, strlen(k) + 1));
  REQUIRE_NE(h, h0);
  REQUIRE_NE(h, fast);
}

/* The key that stands in for getrandom(2) mixes clocks and addresses, so it
 * is neither a constant nor the same on two calls, and its three words
 * differ. */
TEST(hash_seed, the_fallback_key_is_not_a_constant) {
  uint64_t s1[4], s2[4];
  chashmap_fallback_hash_key_for_tests(s1);
  struct timespec ts = {0, 1000};
  nanosleep(&ts, NULL);
  chashmap_fallback_hash_key_for_tests(s2);
  REQUIRE_NE(s1[0], (uint64_t)0);
  REQUIRE_NE(s1[0], s2[0]);
  REQUIRE_NE(s1[0], s1[1]);
  REQUIRE_NE(s1[1], s1[2]);
  REQUIRE_NE(s1[0], s1[2]);
}

/* SipHash-c-d written byte by byte from the specification, with no word
 * loads, as an independent reference for the implementation of the map. */
static uint64_t sip_ref_rotl(uint64_t x, unsigned b) {
  return (x << b) | (x >> (64u - b));
}

static uint64_t sip_ref(const unsigned char *in, size_t len,
                        const unsigned char key[16], int c, int d) {
  uint64_t k0 = 0, k1 = 0;
  for (int i = 0; i < 8; i++) {
    k0 |= (uint64_t)key[i] << (8 * i);
    k1 |= (uint64_t)key[8 + i] << (8 * i);
  }
  uint64_t v[4] = {k0 ^ 0x736f6d6570736575ULL, k1 ^ 0x646f72616e646f6dULL,
                   k0 ^ 0x6c7967656e657261ULL, k1 ^ 0x7465646279746573ULL};
#define SIP_REF_ROUND()            \
  do {                             \
    v[0] += v[1];                  \
    v[1] = sip_ref_rotl(v[1], 13); \
    v[1] ^= v[0];                  \
    v[0] = sip_ref_rotl(v[0], 32); \
    v[2] += v[3];                  \
    v[3] = sip_ref_rotl(v[3], 16); \
    v[3] ^= v[2];                  \
    v[0] += v[3];                  \
    v[3] = sip_ref_rotl(v[3], 21); \
    v[3] ^= v[0];                  \
    v[2] += v[1];                  \
    v[1] = sip_ref_rotl(v[1], 17); \
    v[1] ^= v[2];                  \
    v[2] = sip_ref_rotl(v[2], 32); \
  } while (0)
  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t m = 0;
    for (int j = 0; j < 8; j++) m |= (uint64_t)in[i + j] << (8 * j);
    v[3] ^= m;
    for (int r = 0; r < c; r++) SIP_REF_ROUND();
    v[0] ^= m;
  }
  uint64_t b = (uint64_t)len << 56;
  for (size_t j = 0; i + j < len; j++) b |= (uint64_t)in[i + j] << (8 * j);
  v[3] ^= b;
  for (int r = 0; r < c; r++) SIP_REF_ROUND();
  v[0] ^= b;
  v[2] ^= 0xff;
  for (int r = 0; r < d; r++) SIP_REF_ROUND();
#undef SIP_REF_ROUND
  return v[0] ^ v[1] ^ v[2] ^ v[3];
}

static uint64_t sip_test_rand(uint64_t *s) {
  uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/* The reference is first held to the published SipHash-2-4 vectors of the
 * specification (key 00..0f; the empty input, and the 15 bytes 00..0e), and
 * to the published SipHash-1-3 vector of the empty input under the same key.
 * The byte hash of the map must then equal the reference with one
 * compression round and three finalization rounds for every length from 0 to
 * 130, at four alignments, under random keys. The lengths cover every tail
 * length of the word loop several times over. */
TEST(hash_seed, the_byte_hash_is_siphash_1_3) {
  unsigned char key[16];
  static unsigned char msg[140];
  for (int i = 0; i < 16; i++) key[i] = (unsigned char)i;
  for (int i = 0; i < 140; i++) msg[i] = (unsigned char)i;
  REQUIRE_EQ(sip_ref(msg, 0, key, 2, 4), (uint64_t)0x726fdb47dd0e0e31ULL);
  REQUIRE_EQ(sip_ref(msg, 15, key, 2, 4), (uint64_t)0xa129ca6149be45e5ULL);
  REQUIRE_EQ(sip_ref(msg, 0, key, 1, 3), (uint64_t)0xabac0158050fc4dcULL);

  uint64_t rng = 12345;
  size_t mismatches = 0;
  for (int t = 0; t < 64; t++) {
    if (t > 0) {
      for (int i = 0; i < 16; i++) key[i] = (unsigned char)sip_test_rand(&rng);
      for (int i = 0; i < 140; i++) msg[i] = (unsigned char)sip_test_rand(&rng);
    }
    uint64_t k0 = 0, k1 = 0;
    for (int i = 0; i < 8; i++) {
      k0 |= (uint64_t)key[i] << (8 * i);
      k1 |= (uint64_t)key[8 + i] << (8 * i);
    }
    uint64_t state[4];
    chashmap_sip_state_for_tests(k0, k1, state);
    for (size_t len = 0; len <= 130; len++) {
      for (size_t off = 0; off < 4; off++) {
        if (sip_ref(msg + off, len, key, 1, 3) !=
            chashmap_siphash13_for_tests(state, msg + off, len)) {
          mismatches++;
        }
      }
    }
  }
  REQUIRE_EQ(mismatches, (size_t)0);
}

/* A pair of 24-byte keys that the XXH64 byte hash maps to one full 64-bit
 * hash whatever its seed is. A 24-byte input takes the short path of XXH64:
 * for each 8-byte word w, h = rotl(h ^ round(w), 27) * P1 + P4, where
 * round(w) = rotl(w * P2, 31) * P1 is a bijection of w that anybody can
 * invert. The words of the keys are chosen through that inverse, so that the
 * first word of b contributes the value of a with bit 36 flipped and the
 * second word of b contributes the value of a with bit 63 flipped. After the
 * first word, the rotation by 27 moves the flipped bit to bit 63, and a
 * multiply by an odd number keeps a difference of 2^63 as it is, so h differs
 * in bit 63 alone. The second word flips bit 63 back. From then on the two
 * states are equal, and the seed, which only sets the starting state, plays
 * no part. A keyed pseudorandom function has no such pair. */
static uint64_t xxh_pair_inv(uint64_t a) {
  uint64_t x = a;
  for (int i = 0; i < 6; i++) x *= 2 - a * x;
  return x;
}

static uint64_t xxh_pair_word_of(uint64_t k) {
  const uint64_t p1 = 0x9E3779B185EBCA87ULL, p2 = 0xC2B2AE3D27D4EB4FULL;
  uint64_t r = k * xxh_pair_inv(p1);
  r = (r >> 31) | (r << 33);
  return r * xxh_pair_inv(p2);
}

typedef struct {
  uint64_t w[3];
} xxh_pair_key;

static void xxh_seed_free_pair(xxh_pair_key *a, xxh_pair_key *b) {
  const uint64_t k1 = 0x1234567890abcdefULL, k2 = 0x0fedcba987654321ULL;
  a->w[0] = xxh_pair_word_of(k1);
  a->w[1] = xxh_pair_word_of(k2);
  a->w[2] = 0x4141414141414141ULL;
  b->w[0] = xxh_pair_word_of(k1 ^ (1ULL << 36));
  b->w[1] = xxh_pair_word_of(k2 ^ (1ULL << 63));
  b->w[2] = 0x4141414141414141ULL;
}

/* The pair collides under XXH64 for every seed, so a fast map stores one
 * hash for both, and the keyed mode separates them: SipHash-1-3 under many
 * random keys never maps them to one hash. A set of such keys is what the
 * detection of the fast mode exists for; see adaptive_hash. This test is
 * non-vacuous: with XXH64 as the byte hash of the keyed mode, the switched
 * map stores one hash for both keys. */
TEST(hash_seed, a_seed_independent_xxh64_collision_is_split_by_the_keyed_mode) {
  extern uint64_t chashmap_xxh64_for_tests(const void *key_ptr, size_t key_size,
                                           uint64_t seed);
  extern ccol_retval_t chashmap_switch_to_keyed_for_tests(chmap chm);
  xxh_pair_key a, b;
  xxh_seed_free_pair(&a, &b);
  chmap m = chmap_create(16, ccol_other_types, ccol_int, NULL);
  int one = 1;
  cmap_pair ka = {.ptr = &a, .size = sizeof(a)};
  cmap_pair kb = {.ptr = &b, .size = sizeof(b)};
  cmap_pair v = {.ptr = &one, .size = sizeof(one)};
  ccol_retval_t ra = chmap_insert_elem(m, &ka, &v);
  ccol_retval_t rb = chmap_insert_elem(m, &kb, &v);
  size_t fast_a = chashmap_sc_stored_hash_for_tests(m, &a, sizeof(a));
  size_t fast_b = chashmap_sc_stored_hash_for_tests(m, &b, sizeof(b));
  ccol_retval_t rs = chashmap_switch_to_keyed_for_tests(m);
  size_t ha = chashmap_sc_stored_hash_for_tests(m, &a, sizeof(a));
  size_t hb = chashmap_sc_stored_hash_for_tests(m, &b, sizeof(b));
  chmap_destroy(m);

  /* Under many random seeds, XXH64 maps the pair to one hash every time; under
     many random keys of SipHash, the pair collides no more often than any
     other pair: never, in practice, for a full hash. */
  uint64_t rng = 777;
  size_t collisions = 0, xxh_collisions = 0;
  for (int t = 0; t < 2000; t++) {
    uint64_t state[4];
    chashmap_sip_state_for_tests(sip_test_rand(&rng), sip_test_rand(&rng),
                                 state);
    if (chashmap_byte_hash_for_tests(state, &a, sizeof(a)) ==
        chashmap_byte_hash_for_tests(state, &b, sizeof(b))) {
      collisions++;
    }
    uint64_t seed = sip_test_rand(&rng);
    if (chashmap_xxh64_for_tests(&a, sizeof(a), seed) ==
        chashmap_xxh64_for_tests(&b, sizeof(b), seed)) {
      xxh_collisions++;
    }
  }

  REQUIRE_NE(memcmp(&a, &b, sizeof(a)), 0);
  REQUIRE_EQ(ra, ccol_success);
  REQUIRE_EQ(rb, ccol_success);
  REQUIRE_EQ(rs, ccol_success);
  REQUIRE_EQ(xxh_collisions, (size_t)2000);
  REQUIRE_EQ(fast_a, fast_b);
  REQUIRE_NE(ha, hb);
  REQUIRE_EQ(collisions, (size_t)0);
}

/* The integer flood families below are what a party who knows a public
 * multiplicative hash computes offline. M is the Fibonacci constant, the
 * odd 64-bit multiplier nearest 2^64 / phi; with k * M as the hash, which is
 * the fast hash of a fixed-width key:
 * - i * M^-1 hashes to i, so every key has the top bits of a small number
 *   and all of them share home slot 0;
 * - i * F for a large Fibonacci number F makes F * M nearly a multiple of
 *   2^64, so the keys pile up in a few dense clusters;
 * - i * 32 is a set that varies only in a fixed group of bit positions.
 *   An exclusive or with a seed only translates such a set, so its
 *   clustering under (k ^ seed) * M is the same for every seed.
 * Each set switches a map to the keyed mode, whose seeded mixer has none of
 * these structures. Each test inserts the keys and then looks every one of
 * them up, and bounds the probes for each operation, which a random function
 * of the keys keeps between 1 and 3 at the load of this map. */
enum { INT_FLOOD_KEYS = 20000 };

static uint64_t int_flood_key(int family, uint64_t i) {
  const uint64_t m = 11400714819323198485ULL;
  switch (family) {
    case 0:
      return i * xxh_pair_inv(m);
    case 1:
      return i * 1346269ULL;
    default:
      return i * 32ULL;
  }
}

static void int_flood_probe_counts(int family, unsigned long long *inserts,
                                   unsigned long long *lookups, bool *ok) {
  chmap_construct(m, unsigned long long, unsigned long long);
  *ok = true;
  chashmap_reset_oa_probe_steps_for_tests();
  for (uint64_t i = 0; i < INT_FLOOD_KEYS; i++) {
    unsigned long long k = int_flood_key(family, i);
    unsigned long long v = i;
    cmap_pair kp = {&k, sizeof(k)};
    cmap_pair vp = {&v, sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) *ok = false;
  }
  *inserts = chashmap_oa_probe_steps_for_tests();
  chashmap_reset_oa_probe_steps_for_tests();
  for (uint64_t i = 0; i < INT_FLOOD_KEYS; i++) {
    unsigned long long k = int_flood_key(family, i);
    unsigned long long *p = chmap_get_ptr(m, k);
    if (!p || *p != i) *ok = false;
  }
  *lookups = chashmap_oa_probe_steps_for_tests();
  if (chmap_elem_count(m) != INT_FLOOD_KEYS) *ok = false;
  chmap_destroy(m);
}

/* This test is non-vacuous: without the detection, the keys i * M^-1 take
 * 10001.6 probes for each insert. */
TEST(hash_seed, inverse_multiplier_integer_keys_do_not_share_a_slot) {
  unsigned long long ins = 0, look = 0;
  bool ok = false;
  int_flood_probe_counts(0, &ins, &look, &ok);
  REQUIRE_TRUE(ok);
  REQUIRE_LT((long long)ins, (long long)INT_FLOOD_KEYS * 6);
  REQUIRE_LT((long long)look, (long long)INT_FLOOD_KEYS * 3);
}

/* This test is non-vacuous: without the detection, the multiples of the
 * Fibonacci number 1346269 take 9892 probes for each lookup. */
TEST(hash_seed, fibonacci_stride_integer_keys_do_not_cluster) {
  unsigned long long ins = 0, look = 0;
  bool ok = false;
  int_flood_probe_counts(1, &ins, &look, &ok);
  REQUIRE_TRUE(ok);
  REQUIRE_LT((long long)ins, (long long)INT_FLOOD_KEYS * 6);
  REQUIRE_LT((long long)look, (long long)INT_FLOOD_KEYS * 3);
}

/* This test is non-vacuous: without the window, which is what switches the
 * map for this set, the keys i * 32 take 5.4 probes for each lookup and 5.7
 * for each insert, and (k ^ seed) * M keeps that clustering for every
 * seed. */
TEST(hash_seed, keys_in_a_fixed_group_of_bits_do_not_cluster) {
  unsigned long long ins = 0, look = 0;
  bool ok = false;
  int_flood_probe_counts(2, &ins, &look, &ok);
  REQUIRE_TRUE(ok);
  REQUIRE_LT((long long)ins, (long long)INT_FLOOD_KEYS * 6);
  REQUIRE_LT((long long)look, (long long)INT_FLOOD_KEYS * 3);
}

/* The same keys in a separate-chaining map, which an integral key with a
 * string value selects. This test is non-vacuous: without the detection,
 * every key i * M^-1 goes into bucket 0 and the longest chain holds all
 * 20000 keys. */
TEST(hash_seed, inverse_multiplier_integer_keys_spread_over_the_chains) {
  chmap_construct(m, unsigned long long, char *);
  for (uint64_t i = 0; i < INT_FLOOD_KEYS; i++) {
    chmap_insert(m, (unsigned long long)int_flood_key(0, i), "v");
  }
  size_t stored = chmap_elem_count(m);
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  chmap_destroy(m);
  REQUIRE_EQ(stored, (size_t)INT_FLOOD_KEYS);
  REQUIRE_LE(longest, (size_t)16);
}

/* The inverse of the murmur3 finalizer, which is public and unseeded, and
 * which finalizes a custom hash in the fast mode. Keys mapped through it come
 * out of that finalizer as 0, 1, 2, ..., so with a custom identity hash every
 * one of them gets the top bits of a small number. */
static size_t murmur_fmix_inverse(size_t h) {
#if SIZE_MAX == UINT64_MAX
  h ^= h >> 33;
  h *= xxh_pair_inv(0xc4ceb9fe1a85ec53ULL);
  h ^= h >> 33;
  h *= xxh_pair_inv(0xff51afd7ed558ccdULL);
  h ^= h >> 33;
#else
  uint32_t x = (uint32_t)h;
  x ^= x >> 16;
  x *= (uint32_t)xxh_pair_inv(0xc2b2ae35U);
  x ^= (x >> 13) ^ (x >> 26);
  x *= (uint32_t)xxh_pair_inv(0x85ebca6bU);
  x ^= x >> 16;
  h = x;
#endif
  return h;
}

static unsigned long identity_ulong_hasher(const void *key_ptr,
                                           size_t key_size) {
  (void)key_size;
  unsigned long k;
  memcpy(&k, key_ptr, sizeof(k));
  return k;
}

/* The keys share home slot 0 under the fast finalizer, so the map switches
 * to the keyed mode, which finalizes the custom hash with the secret. This
 * test is non-vacuous: without the detection, every key lands in home slot 0
 * and each insert takes 10001.6 probes. */
TEST(hash_seed, a_custom_hash_is_finalized_with_the_secret) {
  char *err = NULL;
  chmap m = chmap_create_full(16, ccol_unsigned_long, ccol_unsigned_long, NULL,
                              identity_ulong_hasher, NULL, &err);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  chashmap_reset_oa_probe_steps_for_tests();
  for (size_t i = 0; i < INT_FLOOD_KEYS; i++) {
    unsigned long k = (unsigned long)murmur_fmix_inverse(i);
    unsigned long v = (unsigned long)i;
    cmap_pair kp = {&k, sizeof(k)};
    cmap_pair vp = {&v, sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) ok = false;
  }
  unsigned long long ins = chashmap_oa_probe_steps_for_tests();
  size_t stored = chmap_elem_count(m);
  __chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(stored, (size_t)INT_FLOOD_KEYS);
  REQUIRE_LT((long long)ins, (long long)INT_FLOOD_KEYS * 6);
}

/* ========================================================================== */
/*       INTERNAL: THE STORED KEY OF AN ENTRY, AND THE INSERTION ORDER        */
/* ========================================================================== */

TEST(insert_or_get_entry, gives_the_stored_key_and_it_survives_resizes) {
  chmap m = chmap_create(16, ccol_string, ccol_long, NULL);
  chmap_redeclare(m, char *, long);
  /* One short key, stored inline, and one long key, stored on the heap. */
  const char *short_k = "short";
  const char *long_k = "a key that is longer than the inline storage";
  long v = 7;
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
  const cmap_pair *ks = (const cmap_pair *)&vp, *vs = (const cmap_pair *)&vp;
  cmap_pair kp1 = {.ptr = (void *)short_k, .size = strlen(short_k) + 1};
  ccol_retval_t r1 = ccol_chmap_insert_or_get_entry(m, &kp1, &vp, &ks, &vs);
  const void *short_stored = ks ? ks->ptr : NULL;
  cmap_pair kp2 = {.ptr = (void *)long_k, .size = strlen(long_k) + 1};
  ccol_retval_t r2 = ccol_chmap_insert_or_get_entry(m, &kp2, &vp, &ks, &vs);
  const void *long_stored = ks ? ks->ptr : NULL;
  /* Grow the bucket array several times, then replace both values. */
  char key[32];
  for (int i = 0; i < 2000; i++) {
    snprintf(key, sizeof(key), "filler-%d", i);
    chmap_insert(m, key, (long)i);
  }
  chmap_insert(m, (char *)short_k, 8L);
  chmap_insert(m, (char *)long_k, 9L);
  ccol_retval_t r3 = ccol_chmap_insert_or_get_entry(m, &kp1, &vp, &ks, &vs);
  const void *short_again = ks ? ks->ptr : NULL;
  long short_val = vs ? *(const long *)vs->ptr : -1;
  ccol_retval_t r4 = ccol_chmap_insert_or_get_entry(m, &kp2, &vp, &ks, &vs);
  const void *long_again = ks ? ks->ptr : NULL;
  bool short_bytes_ok = short_stored && strcmp(short_stored, short_k) == 0 &&
                        short_stored != (const void *)short_k;
  bool long_bytes_ok = long_stored && strcmp(long_stored, long_k) == 0 &&
                       long_stored != (const void *)long_k;
  chmap_destroy(m);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_key_already_present);
  REQUIRE_EQ(r4, ccol_key_already_present);
  REQUIRE_TRUE(short_bytes_ok);
  REQUIRE_TRUE(long_bytes_ok);
  REQUIRE_TRUE(short_again == short_stored);
  REQUIRE_TRUE(long_again == long_stored);
  REQUIRE_EQ(short_val, 8L);
}

TEST(insert_or_get_entry, refuses_open_addressing_and_null_out_parameters) {
  chmap m = chmap_create(16, ccol_int, ccol_int, NULL);
  int k = 1, v = 2;
  cmap_pair kp = {.ptr = &k, .size = sizeof(k)};
  cmap_pair vp = {.ptr = &v, .size = sizeof(v)};
  const cmap_pair *ks = &kp, *vs = &kp; /* poison values */
  ccol_retval_t r_oa = ccol_chmap_insert_or_get_entry(m, &kp, &vp, &ks, &vs);
  size_t count = chmap_elem_count(m);
  const cmap_pair *ks_after = ks, *vs_after = vs;
  ccol_retval_t r_null_key =
      ccol_chmap_insert_or_get_entry(m, &kp, &vp, NULL, &vs);
  ccol_retval_t r_null_map =
      ccol_chmap_insert_or_get_entry(NULL, &kp, &vp, &ks, &vs);
  chmap_destroy(m);
  REQUIRE_EQ(r_oa, ccol_invalid_args);
  REQUIRE_EQ(count, (size_t)0);
  REQUIRE_TRUE(ks_after == NULL);
  REQUIRE_TRUE(vs_after == NULL);
  REQUIRE_EQ(r_null_key, ccol_invalid_args);
  REQUIRE_EQ(r_null_map, ccol_invalid_args);
}

/* Collects the keys of m from the oldest entry to the newest one into out,
 * joined by ','. */
static void ordered_keys(chmap m, char *out, size_t cap) {
  out[0] = '\0';
  const cmap_pair *k, *v;
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(m); e;) {
    e = ccol_chmap_entry_read(e, &k, &v);
    if (out[0]) strncat(out, ",", cap - strlen(out) - 1);
    strncat(out, (const char *)k->ptr, cap - strlen(out) - 1);
  }
}

TEST(ordered_entries, walk_in_insertion_order_through_replace_delete_resize) {
  chmap m = chmap_create(16, ccol_string, ccol_int, NULL);
  chmap_redeclare(m, char *, int);
  char empty_order[8];
  ordered_keys(m, empty_order, sizeof(empty_order));
  chmap_insert(m, "c", 1);
  chmap_insert(m, "a", 2);
  chmap_insert(m, "b", 3);
  char o1[64];
  ordered_keys(m, o1, sizeof(o1));
  chmap_insert(m, "a", 20); /* a replacement keeps the place */
  char o2[64];
  ordered_keys(m, o2, sizeof(o2));
  chmap_remove(m, "c"); /* the oldest */
  chmap_insert(m, "d", 4);
  chmap_remove(m, "d"); /* the newest */
  chmap_insert(m, "e", 5);
  char o3[64];
  ordered_keys(m, o3, sizeof(o3));
  char key[32];
  for (int i = 0; i < 500; i++) {
    snprintf(key, sizeof(key), "f%d", i);
    chmap_insert(m, key, i);
  }
  for (int i = 0; i < 500; i++) {
    snprintf(key, sizeof(key), "f%d", i);
    chmap_remove(m, key);
  }
  char o4[64];
  ordered_keys(m, o4, sizeof(o4));
  chmap_remove(m, "a");
  chmap_remove(m, "b");
  chmap_remove(m, "e");
  bool empty_after = ccol_chmap_oldest_entry(m) == NULL;
  chmap_insert(m, "z", 26);
  char o5[64];
  ordered_keys(m, o5, sizeof(o5));
  chmap_reset(m, 0);
  bool empty_after_reset = ccol_chmap_oldest_entry(m) == NULL;
  chmap_destroy(m);
  chmap oa = chmap_create(16, ccol_int, ccol_int, NULL);
  chmap_redeclare(oa, int, int);
  chmap_insert(oa, 1, 1);
  bool oa_has_none = ccol_chmap_oldest_entry(oa) == NULL;
  chmap_destroy(oa);
  REQUIRE_STREQ(empty_order, "");
  REQUIRE_STREQ(o1, "c,a,b");
  REQUIRE_STREQ(o2, "c,a,b");
  REQUIRE_STREQ(o3, "a,b,e");
  REQUIRE_STREQ(o4, "a,b,e");
  REQUIRE_TRUE(empty_after);
  REQUIRE_STREQ(o5, "z");
  REQUIRE_TRUE(empty_after_reset);
  REQUIRE_TRUE(oa_has_none);
  REQUIRE_TRUE(ccol_chmap_oldest_entry(NULL) == NULL);
}

/* The successor that entry_read gives survives a delete of the entry just
 * read, which is what lets a caller remove members while it walks. */
TEST(ordered_entries, the_successor_survives_a_delete_of_the_current_entry) {
  chmap m = chmap_create(16, ccol_string, ccol_int, NULL);
  chmap_redeclare(m, char *, int);
  chmap_insert(m, "x", 1);
  chmap_insert(m, "y", 2);
  chmap_insert(m, "z", 3);
  size_t visited = 0;
  const cmap_pair *k, *v;
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(m); e;) {
    e = ccol_chmap_entry_read(e, &k, &v);
    chmap_delete_elem(m, k);
    visited++;
  }
  size_t count = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_EQ(visited, (size_t)3);
  REQUIRE_EQ(count, (size_t)0);
}

/* ------------------------------------------------------------------------ */
/* A NULL character pointer through the typed macros                         */
/* ------------------------------------------------------------------------ */

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* The name of an environment variable that no test environment sets, so that
 * getenv() gives NULL for it, which is the ordinary way a NULL character
 * pointer reaches these macros. */
#define CHMAP_NULL_TEST_UNSET_VAR "CCOL_CHMAP_TEST_SURELY_UNSET_VARIABLE"

/* How a forked child ended, reported through a pipe: the byte that the probe
 * returned, or a code for a signal. The exit status alone is not used,
 * because valgrind replaces the exit status of a child that exits with memory
 * still reachable. A probe that dereferences NULL dies with SIGSEGV and
 * writes nothing, so this reports it as a crash instead of taking the whole
 * test binary down with it. */
enum { chmap_null_probe_crashed = 250, chmap_null_probe_aborted = 251 };

static int chmap_null_run_probe(int (*probe)(void)) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    unsigned char byte = (unsigned char)probe();
    ssize_t w = write(fds[1], &byte, 1);
    (void)w;
    _exit(0);
  }
  close(fds[1]);
  if (pid < 0) {
    close(fds[0]);
    return -1;
  }
  unsigned char byte = 0;
  ssize_t n = read(fds[0], &byte, 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (n == 1) return byte;
  if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
    return chmap_null_probe_aborted;
  }
  return chmap_null_probe_crashed;
}

static int chmap_null_probe_get_ptr_key(void) {
  unsetenv(CHMAP_NULL_TEST_UNSET_VAR);
  chmap_construct(env, char *, char *);
  chmap_insert(env, "HOME", "/home/someone");
  char *absent = getenv(CHMAP_NULL_TEST_UNSET_VAR);
  char *const *p = chmap_get_ptr(env, absent);
  int ok = absent == NULL && p == NULL && chmap_elem_count(env) == 1;
  chmap_destroy(env);
  return ok;
}

static int chmap_null_probe_remove_key(void) {
  unsetenv(CHMAP_NULL_TEST_UNSET_VAR);
  chmap_construct(env, char *, int);
  chmap_insert(env, "HOME", 1);
  ccol_retval_t r = chmap_remove(env, getenv(CHMAP_NULL_TEST_UNSET_VAR));
  int ok = r == ccol_invalid_args && chmap_elem_count(env) == 1;
  chmap_destroy(env);
  return ok;
}

static int chmap_null_probe_insert_value(void) {
  unsetenv(CHMAP_NULL_TEST_UNSET_VAR);
  chmap_construct(env, char *, char *);
  chmap_insert(env, "HOME", getenv(CHMAP_NULL_TEST_UNSET_VAR));
  chmap_destroy(env);
  return 1;
}

static int chmap_null_probe_insert_key(void) {
  unsetenv(CHMAP_NULL_TEST_UNSET_VAR);
  chmap_construct(env, char *, int);
  chmap_insert(env, getenv(CHMAP_NULL_TEST_UNSET_VAR), 1);
  chmap_destroy(env);
  return 1;
}

static int chmap_null_probe_get_key(void) {
  unsetenv(CHMAP_NULL_TEST_UNSET_VAR);
  chmap_construct(env, char *, int);
  chmap_insert(env, "HOME", 1);
  int v = chmap_get(env, getenv(CHMAP_NULL_TEST_UNSET_VAR));
  chmap_destroy(env);
  return v == 1;
}

/* An int value map with a string value type runs the value through the same
 * pair builder as a string key. */
static int chmap_null_probe_insert_value_int_key(void) {
  unsetenv(CHMAP_NULL_TEST_UNSET_VAR);
  chmap_construct(m, int, char *);
  chmap_insert(m, 7, getenv(CHMAP_NULL_TEST_UNSET_VAR));
  chmap_destroy(m);
  return 1;
}

/* A NULL character pointer is not a string: the pair that the macros build
 * for it has no bytes, so the raw layer answers ccol_invalid_args, and each
 * macro reports that in its documented way: chmap_get_ptr() gives NULL,
 * chmap_remove() returns the code, and chmap_insert() and chmap_get() stop
 * the program with it. This test is non-vacuous: without the NULL test in
 * _populate_cmap_pair(), every one of these probes dies with SIGSEGV inside
 * strlen(). */
TEST(chmap_null_char_ptr, get_ptr_of_a_null_key_gives_null) {
  REQUIRE_EQ(chmap_null_run_probe(chmap_null_probe_get_ptr_key), 1);
}

TEST(chmap_null_char_ptr, remove_of_a_null_key_is_invalid_args) {
  REQUIRE_EQ(chmap_null_run_probe(chmap_null_probe_remove_key), 1);
}

TEST(chmap_null_char_ptr, insert_of_a_null_value_is_fatal) {
  REQUIRE_EQ(chmap_null_run_probe(chmap_null_probe_insert_value),
             (int)chmap_null_probe_aborted);
  REQUIRE_EQ(chmap_null_run_probe(chmap_null_probe_insert_value_int_key),
             (int)chmap_null_probe_aborted);
}

TEST(chmap_null_char_ptr, insert_of_a_null_key_is_fatal) {
  REQUIRE_EQ(chmap_null_run_probe(chmap_null_probe_insert_key),
             (int)chmap_null_probe_aborted);
}

TEST(chmap_null_char_ptr, get_of_a_null_key_is_fatal) {
  REQUIRE_EQ(chmap_null_run_probe(chmap_null_probe_get_key),
             (int)chmap_null_probe_aborted);
}

/* ------------------------------------------------------------------------ */
/* Binary keys                                                                */
/* ------------------------------------------------------------------------ */

/* A fixed-size digest wrapped in a struct is a binary key through the typed
 * macros: the map hashes and compares all 32 bytes, embedded zero bytes
 * included, and a character pointer never enters the picture. */
typedef struct {
  unsigned char b[32];
} chmap_test_digest;

TEST(chmap_binary_keys, a_digest_struct_key_compares_every_byte) {
  chmap_construct(seen, chmap_test_digest, int);
  chmap_test_digest d1, d2, d3;
  for (int i = 0; i < 32; i++) {
    d1.b[i] = (unsigned char)(i * 17 + 1);
  }
  d1.b[3] = 0; /* a zero byte early, as a real digest can have */
  d2 = d1;
  d2.b[30] ^= 0xff; /* differs only after the zero byte */
  d3 = d1;
  d3.b[0] = 0; /* starts with a zero byte */
  chmap_insert(seen, d1, 1);
  chmap_insert(seen, d2, 2);
  chmap_insert(seen, d3, 3);
  size_t count = chmap_elem_count(seen);
  int v1 = chmap_get(seen, d1);
  int v2 = chmap_get(seen, d2);
  int v3 = chmap_get(seen, d3);
  chmap_test_digest copy = d2;
  int *pc = chmap_get_ptr(seen, copy);
  int vc = pc ? *pc : -1;
  chmap_destroy(seen);
  REQUIRE_EQ(count, (size_t)3);
  REQUIRE_EQ(v1, 1);
  REQUIRE_EQ(v2, 2);
  REQUIRE_EQ(v3, 3);
  REQUIRE_EQ(vc, 2);
}

/* A binary key of a length that only the caller knows goes through the raw
 * layer, with that length in cmap_pair.size, where every byte counts,
 * embedded zero bytes included, so keys that share a prefix up to a zero byte
 * stay distinct. */
TEST(chmap_binary_keys, a_raw_layer_key_with_embedded_zeros) {
  chmap m = chmap_create(16, ccol_other_types, ccol_int, NULL);
  REQUIRE_NE((void *)m, NULL);
  unsigned char k1[16], k2[16], k3[5];
  for (int i = 0; i < 16; i++) k1[i] = (unsigned char)(i * 13 + 7);
  k1[3] = 0;
  memcpy(k2, k1, sizeof(k1));
  k2[10] ^= 0xff;
  memcpy(k3, k1, sizeof(k3)); /* a prefix of k1, zero byte included */
  int v1 = 1, v2 = 2, v3 = 3;
  cmap_pair kp1 = {k1, sizeof(k1)}, vp1 = {&v1, sizeof(v1)};
  cmap_pair kp2 = {k2, sizeof(k2)}, vp2 = {&v2, sizeof(v2)};
  cmap_pair kp3 = {k3, sizeof(k3)}, vp3 = {&v3, sizeof(v3)};
  ccol_retval_t r1 = chmap_insert_elem(m, &kp1, &vp1);
  ccol_retval_t r2 = chmap_insert_elem(m, &kp2, &vp2);
  ccol_retval_t r3 = chmap_insert_elem(m, &kp3, &vp3);
  size_t count = chmap_elem_count(m);
  unsigned char probe[16];
  memcpy(probe, k2, sizeof(probe));
  cmap_pair pp = {probe, sizeof(probe)};
  const cmap_pair *out = NULL;
  ccol_retval_t rg = chmap_get_elem_ref(m, &pp, &out);
  int got = (rg == ccol_success && out) ? *(const int *)out->ptr : -1;
  cmap_pair pp3 = {k3, sizeof(k3)};
  const cmap_pair *out3 = NULL;
  ccol_retval_t rg3 = chmap_get_elem_ref(m, &pp3, &out3);
  int got3 = (rg3 == ccol_success && out3) ? *(const int *)out3->ptr : -1;
  chmap_destroy(m);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_EQ(count, (size_t)3);
  REQUIRE_EQ(got, 2);
  REQUIRE_EQ(got3, 3);
}

/* chmap_destroy evaluates its argument once, so a walk backwards over an
 * array with `chmap_destroy(a[--k])` destroys and clears every map. This test
 * is non-vacuous: a macro that evaluates its argument twice destroys only
 * every second map and clears the others without destroying them. */
TEST(chmap_macros, destroy_evaluates_its_argument_once) {
  chmap maps[2] = {chmap_create(8, ccol_int, ccol_int, NULL),
                   chmap_create(8, ccol_int, ccol_int, NULL)};
  bool created = maps[0] && maps[1];
  int k = 2;
  while (k > 0) chmap_destroy(maps[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ((void *)maps[0], NULL);
  REQUIRE_EQ((void *)maps[1], NULL);
}

/* ========================================================================== */
/*                     FOOTPRINT OF A MAP AND OF AN ENTRY                     */
/* ========================================================================== */

#include <internal/cprocsintern.h>

/* An allocator that counts the blocks and the bytes that the map holds,
 * keeping the size of each block in a header in front of it so that a free
 * knows what it gives back. g_fp.fail_at, when not 0, makes the allocation
 * with that 1-based index fail, and every one after it succeed. */
typedef struct {
  long blocks;
  long bytes;
  long calls;
  long fail_at;
} fp_counts_t;
static fp_counts_t g_fp;

typedef union {
  size_t size;
  max_align_t align;
} fp_header_t;

static void *fp_malloc(size_t n) {
  g_fp.calls++;
  if (g_fp.fail_at != 0 && g_fp.calls == g_fp.fail_at) return NULL;
  fp_header_t *h = malloc(sizeof(fp_header_t) + n);
  if (!h) return NULL;
  h->size = n;
  g_fp.blocks++;
  g_fp.bytes += (long)n;
  return h + 1;
}
static void fp_free(void *p) {
  if (!p) return;
  fp_header_t *h = (fp_header_t *)p - 1;
  g_fp.blocks--;
  g_fp.bytes -= (long)h->size;
  free(h);
}
static void *fp_calloc(size_t n, size_t sz) {
  if (sz != 0 && n > SIZE_MAX / sz) return NULL;
  void *p = fp_malloc(n * sz);
  if (p) memset(p, 0, n * sz);
  return p;
}
static void *fp_realloc(void *p, size_t n) {
  if (!p) return fp_malloc(n);
  fp_header_t *h = (fp_header_t *)p - 1;
  void *q = fp_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, h->size < n ? h->size : n);
  fp_free(p);
  return q;
}
static void fp_reset(void) { memset(&g_fp, 0, sizeof(g_fp)); }

/* The sizes below hold on a 64-bit target whose max_align_t is 16 bytes,
 * such as x86-64 and aarch64; on any other target the tests check only the
 * block counts and the relations between the byte counts. */
static bool fp_lp64_16(void) {
  return sizeof(void *) == 8 && sizeof(size_t) == 8 &&
         _Alignof(max_align_t) == 16;
}

static ccol_retval_t fp_insert_str(chmap m, const char *k, const void *v,
                                   size_t vsize) {
  cmap_pair kp = {.ptr = (void *)k, .size = strlen(k) + 1};
  cmap_pair vp = {.ptr = (void *)v, .size = vsize};
  return chmap_insert_elem(m, &kp, &vp);
}

/* A separate-chaining map with the default first bucket array is a single
 * block (the map, its 16 buckets and its copy of the procs), and an entry
 * whose key and value fit inline is one block of 112 bytes on a 64-bit target.
 * This test is non-vacuous: a map whose state, bucket array and procs copy
 * are allocations of their own holds four blocks and 304 bytes before its
 * first entry, and an entry of 160 bytes fails the per-entry size. */
TEST(footprint, a_separate_chaining_map_is_one_block_and_an_entry_is_one) {
  fp_reset();
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  chmap m = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                            ccol_pointer, &mp, NULL);
  bool created = m != NULL;
  long empty_blocks = g_fp.blocks, empty_bytes = g_fp.bytes;
  void *v = NULL;
  ccol_retval_t r1 =
      m ? fp_insert_str(m, "k", &v, sizeof(v)) : ccol_unexpected_failure;
  long one_blocks = g_fp.blocks, one_bytes = g_fp.bytes;
  /* A key of 24 bytes or more is kept in a buffer of its own. */
  const char *long_key = "a key that is longer than the inline storage";
  ccol_retval_t r2 =
      m ? fp_insert_str(m, long_key, &v, sizeof(v)) : ccol_unexpected_failure;
  long two_blocks = g_fp.blocks, two_bytes = g_fp.bytes;
  if (m) chmap_destroy(m);
  long left = g_fp.blocks;

  REQUIRE_TRUE(created);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(empty_blocks, 1L);
  REQUIRE_EQ(one_blocks, 2L);
  REQUIRE_EQ(two_blocks, 4L);
  REQUIRE_EQ(two_bytes - one_bytes,
             (one_bytes - empty_bytes) + (long)strlen(long_key) + 1);
  if (fp_lp64_16()) {
    REQUIRE_EQ(empty_bytes, 264L);
    REQUIRE_EQ(one_bytes - empty_bytes, 112L);
  }
  REQUIRE_EQ(left, 0L);
}

/* An open-addressing map is two blocks: the map with its copy of the procs,
 * and its table, which holds the slots, the value accessors and the
 * metadata, so inserts below the growth load allocate nothing. This test is
 * non-vacuous: a map whose state, procs copy, slots and accessors are
 * allocations of their own holds five blocks. */
TEST(footprint, an_open_addressing_map_is_two_blocks) {
  fp_reset();
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  chmap m = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_long,
                            ccol_long, &mp, NULL);
  long empty_blocks = g_fp.blocks, empty_bytes = g_fp.bytes;
  bool inserted = m != NULL;
  for (long i = 0; inserted && i < 8; i++) {
    cmap_pair kp = {&i, sizeof(i)};
    cmap_pair vp = {&i, sizeof(i)};
    inserted = chmap_insert_elem(m, &kp, &vp) == ccol_success;
  }
  long eight_blocks = g_fp.blocks, eight_bytes = g_fp.bytes;
  if (m) chmap_destroy(m);
  long left = g_fp.blocks;

  REQUIRE_TRUE(inserted);
  REQUIRE_EQ(empty_blocks, 2L);
  REQUIRE_EQ(eight_blocks, 2L);
  REQUIRE_EQ(eight_bytes, empty_bytes);
  if (fp_lp64_16()) {
    /* 104 for the map, 32 for the procs, 16 * (16 + 16 + 1) for the table. */
    REQUIRE_EQ(empty_bytes, 664L);
  }
  REQUIRE_EQ(left, 0L);
}

/* A procs pointer that ccol_procs_intern() gave lives for the whole process,
 * so the map keeps it instead of a copy; any other procs struct is copied,
 * so the caller may change or drop its own struct right after the create.
 * This test is non-vacuous: a map that copies every procs struct holds the
 * same bytes for both maps. */
TEST(footprint, interned_procs_are_kept_and_other_procs_are_copied) {
  fp_reset();
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  ccol_memmgmt_procs_t *interned = NULL;
  ccol_retval_t ri = ccol_procs_intern(&mp, &interned);
  chmap copied = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                 ccol_string, ccol_pointer, &mp, NULL);
  long copied_bytes = g_fp.bytes;
  chmap kept = interned
                   ? chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,
                                     ccol_string, ccol_pointer, interned, NULL)
                   : NULL;
  long kept_bytes = g_fp.bytes - copied_bytes;
  /* The copy is the map's own: the caller's struct can change after the
   * create, and the map still allocates and frees through the copy. */
  ccol_memmgmt_procs_t original = mp;
  mp.malloc = NULL;
  mp.free = NULL;
  void *v = NULL;
  ccol_retval_t r1 = copied ? fp_insert_str(copied, "after", &v, sizeof(v))
                            : ccol_unexpected_failure;
  ccol_retval_t r2 = kept ? fp_insert_str(kept, "after", &v, sizeof(v))
                          : ccol_unexpected_failure;
  bool both_created = copied && kept;
  if (copied) chmap_destroy(copied);
  if (kept) chmap_destroy(kept);
  mp = original;
  long left = g_fp.blocks;

  REQUIRE_EQ(ri, ccol_success);
  REQUIRE_TRUE(both_created);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(copied_bytes - kept_bytes, (long)sizeof(ccol_memmgmt_procs_t));
  REQUIRE_EQ(left, 0L);
}

/* Inserts "key_<i>" for i in [from, to) and records whether all succeeded. */
static bool fp_insert_range(chmap m, int from, int to) {
  for (int i = from; i < to; i++) {
    char k[32];
    snprintf(k, sizeof(k), "key_%d", i);
    if (fp_insert_str(m, k, &i, sizeof(i)) != ccol_success) return false;
  }
  return true;
}

static bool fp_lookup_range(chmap m, int from, int to) {
  for (int i = from; i < to; i++) {
    char k[32];
    snprintf(k, sizeof(k), "key_%d", i);
    cmap_pair kp = {.ptr = k, .size = strlen(k) + 1};
    const cmap_pair *vp = NULL;
    if (chmap_get_elem_ref(m, &kp, &vp) != ccol_success || !vp ||
        vp->size != sizeof(int) || memcmp(vp->ptr, &i, sizeof(i)) != 0) {
      return false;
    }
  }
  return true;
}

static bool fp_delete_range(chmap m, int from, int to) {
  for (int i = from; i < to; i++) {
    char k[32];
    snprintf(k, sizeof(k), "key_%d", i);
    cmap_pair kp = {.ptr = k, .size = strlen(k) + 1};
    if (chmap_delete_elem(m, &kp) != ccol_success) return false;
  }
  return true;
}

/* Checks that the oldest-first cursor gives exactly the keys "key_<i>" for i
 * in [from, to), in that order, and that the public iterator gives the same
 * keys newest first. */
static bool fp_orders_match(chmap m, int from, int to) {
  int i = from;
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(m); e;) {
    const cmap_pair *k = NULL, *v = NULL;
    e = ccol_chmap_entry_read(e, &k, &v);
    char want[32];
    snprintf(want, sizeof(want), "key_%d", i);
    if (i >= to || strcmp((const char *)k->ptr, want) != 0 ||
        memcmp(v->ptr, &i, sizeof(i)) != 0) {
      return false;
    }
    i++;
  }
  if (i != to) return false;
  int j = to;
  for (cmap_iterator *it = chashmap_begin_iter(m, NULL); it;
       it = ccol_iter_next(it)) {
    j--;
    char want[32];
    snprintf(want, sizeof(want), "key_%d", j);
    if (j < from || strcmp((const char *)it->key_pair->ptr, want) != 0) {
      __chmap_iterator_destroy(it);
      return false;
    }
  }
  return j == from;
}

/* A compact map starts with 4 buckets inside its block, grows to 16 at its
 * seventh entry and from there follows the sizes and thresholds of a map
 * that started at 16. Every key, the oldest-first cursor and the iterator
 * survive each growth, and a delete never shrinks the map below 16. This test
 * is non-vacuous: a compact map that starts at 16 buckets fails the first
 * size, and one whose first bucket array is an allocation of its own fails
 * the block count. */
TEST(footprint, a_compact_map_grows_from_four_buckets) {
  fp_reset();
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  chmap m = ccol_chmap_create_compact(ccol_string, ccol_int, &mp, NULL);
  chmap ref = chmap_create(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                           ccol_int, NULL);
  bool ok = m && ref;
  long empty_blocks = g_fp.blocks;
  long empty_bytes = g_fp.bytes;
  size_t size0 = ok ? chmap_get_bucket_arr_size(m) : 0;

  ok = ok && fp_insert_range(m, 0, 6);
  size_t size6 = ok ? chmap_get_bucket_arr_size(m) : 0;
  long blocks6 = g_fp.blocks;
  bool order6 = ok && fp_orders_match(m, 0, 6);

  ok = ok && fp_insert_range(m, 6, 7);
  size_t size7 = ok ? chmap_get_bucket_arr_size(m) : 0;
  long blocks7 = g_fp.blocks;
  bool order7 = ok && fp_orders_match(m, 0, 7) && fp_lookup_range(m, 0, 7);
  /* At 16 buckets the thresholds are those of a map that started there. */
  size_t up16 = ok ? chmap_get_elem_count_to_scale_up(m) : 0;
  size_t down16 = ok ? chmap_get_elem_count_to_scale_down(m) : 0;
  size_t ref_up16 = ref ? chmap_get_elem_count_to_scale_up(ref) : 1;
  size_t ref_down16 = ref ? chmap_get_elem_count_to_scale_down(ref) : 1;

  ok = ok && fp_insert_range(m, 7, 25);
  size_t size25 = ok ? chmap_get_bucket_arr_size(m) : 0;
  bool order25 = ok && fp_orders_match(m, 0, 25) && fp_lookup_range(m, 0, 25);

  /* Deleting all but one entry shrinks 64 buckets to 16 and no further. */
  ok = ok && fp_delete_range(m, 0, 24);
  size_t size1 = ok ? chmap_get_bucket_arr_size(m) : 0;
  bool order1 = ok && fp_orders_match(m, 24, 25) && fp_lookup_range(m, 24, 25);
  ok = ok && fp_delete_range(m, 24, 25);
  size_t size_empty = ok ? chmap_get_bucket_arr_size(m) : 0;
  bool none_left = ok && ccol_chmap_oldest_entry(m) == NULL;

  if (m) chmap_destroy(m);
  if (ref) chmap_destroy(ref);
  long left = g_fp.blocks;

  REQUIRE_TRUE(ok);
  REQUIRE_EQ(size0, (size_t)4);
  REQUIRE_EQ(empty_blocks, 1L);
  if (fp_lp64_16()) {
    /* 104 for the map, 4 * 8 for the buckets, 32 for the procs. */
    REQUIRE_EQ(empty_bytes, 168L);
  }
  REQUIRE_EQ(size6, (size_t)4);
  REQUIRE_EQ(blocks6, 1L + 6L);
  REQUIRE_TRUE(order6);
  REQUIRE_EQ(size7, (size_t)16);
  REQUIRE_EQ(blocks7, 1L + 1L + 7L);
  REQUIRE_TRUE(order7);
  REQUIRE_EQ(up16, ref_up16);
  REQUIRE_EQ(down16, ref_down16);
  REQUIRE_EQ(size25, (size_t)64);
  REQUIRE_TRUE(order25);
  REQUIRE_EQ(size1, (size_t)16);
  REQUIRE_TRUE(order1);
  REQUIRE_EQ(size_empty, (size_t)16);
  REQUIRE_TRUE(none_left);
  REQUIRE_EQ(left, 0L);
}

/* A compact map that stays at four buckets never rebuilds them: a long run
 * of inserts and deletes that keeps five or six entries allocates
 * nothing but its entries. This test is non-vacuous: a shrink check that let
 * the map leave four buckets, or a growth check that fired below seven
 * entries, makes the bucket array an allocation and fails the count. */
TEST(footprint, a_steady_window_on_a_compact_map_never_resizes) {
  fp_reset();
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  chmap m = ccol_chmap_create_compact(ccol_string, ccol_int, &mp, NULL);
  bool ok = m && fp_insert_range(m, 0, 5);
  long calls_before = g_fp.calls;
  long steps = 0;
  /* Each step inserts one key and deletes the oldest, so the map holds six
   * entries after the insert and five after the delete. */
  for (int i = 5; ok && i < 2000; i++) {
    ok = fp_insert_range(m, i, i + 1) && fp_delete_range(m, i - 5, i - 4);
    steps++;
  }
  long calls = g_fp.calls - calls_before;
  size_t count = ok ? chmap_elem_count(m) : 0;
  size_t size = ok ? chmap_get_bucket_arr_size(m) : 0;
  long blocks = g_fp.blocks;
  if (m) chmap_destroy(m);
  long left = g_fp.blocks;

  REQUIRE_TRUE(ok);
  REQUIRE_EQ(size, (size_t)4);
  REQUIRE_EQ(count, (size_t)5);
  /* One allocation for each insert, and nothing else. */
  REQUIRE_EQ(calls, steps);
  REQUIRE_EQ(blocks, 1L + (long)count);
  REQUIRE_EQ(left, 0L);
}

/* A map that started with its bucket array in its block takes that array
 * back when it shrinks to its first size, by a delete or by a reset, and a
 * reset that cannot allocate leaves it empty, usable and at its old size.
 * This test is non-vacuous: a map that allocates a new array for the shrink
 * holds one block more than its entries after it, and a reset that
 * reallocates the array in the block corrupts the heap. */
TEST(footprint, the_block_array_is_taken_back_on_a_shrink_and_a_reset) {
  fp_reset();
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  chmap m = chmap_create_mp(CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, ccol_string,
                            ccol_int, &mp, NULL);
  bool ok = m && fp_insert_range(m, 0, 25);
  size_t grown = ok ? chmap_get_bucket_arr_size(m) : 0;
  long grown_blocks = g_fp.blocks;
  /* (64 + 1) / 8 is 8: the delete that leaves 7 entries shrinks to 16. */
  ok = ok && fp_delete_range(m, 0, 18);
  size_t shrunk = ok ? chmap_get_bucket_arr_size(m) : 0;
  long shrunk_blocks = g_fp.blocks;
  bool kept = ok && fp_lookup_range(m, 18, 25) && fp_orders_match(m, 18, 25);

  ccol_retval_t r64 = ok ? chmap_reset(m, 64) : ccol_unexpected_failure;
  size_t size64 = ok ? chmap_get_bucket_arr_size(m) : 0;
  long blocks64 = g_fp.blocks;
  ccol_retval_t r16 = ok ? chmap_reset(m, 16) : ccol_unexpected_failure;
  size_t size16 = ok ? chmap_get_bucket_arr_size(m) : 0;
  long blocks16 = g_fp.blocks;

  /* A reset to a new size whose allocation fails clears the map and keeps
   * the array in the block. */
  ok = ok && fp_insert_range(m, 0, 3);
  g_fp.fail_at = g_fp.calls + 1;
  ccol_retval_t rfail = ok ? chmap_reset(m, 256) : ccol_success;
  g_fp.fail_at = 0;
  size_t size_after_fail = ok ? chmap_get_bucket_arr_size(m) : 0;
  size_t count_after_fail = ok ? chmap_elem_count(m) : 1;
  long blocks_after_fail = g_fp.blocks;
  bool usable = ok && fp_insert_range(m, 0, 30) && fp_lookup_range(m, 0, 30) &&
                fp_orders_match(m, 0, 30);

  if (m) chmap_destroy(m);
  long left = g_fp.blocks;

  REQUIRE_TRUE(ok);
  REQUIRE_EQ(grown, (size_t)64);
  REQUIRE_EQ(grown_blocks, 1L + 1L + 25L);
  REQUIRE_EQ(shrunk, (size_t)16);
  REQUIRE_EQ(shrunk_blocks, 1L + 7L);
  REQUIRE_TRUE(kept);
  REQUIRE_EQ(r64, ccol_success);
  REQUIRE_EQ(size64, (size_t)64);
  REQUIRE_EQ(blocks64, 2L);
  REQUIRE_EQ(r16, ccol_success);
  REQUIRE_EQ(size16, (size_t)16);
  REQUIRE_EQ(blocks16, 1L);
  REQUIRE_EQ(rfail, ccol_not_enough_memory);
  REQUIRE_EQ(size_after_fail, (size_t)16);
  REQUIRE_EQ(count_after_fail, (size_t)0);
  REQUIRE_EQ(blocks_after_fail, 1L);
  REQUIRE_TRUE(usable);
  REQUIRE_EQ(left, 0L);
}

/* Every allocation that creating a map or an entry makes can fail, and each
 * failure leaves nothing allocated behind. An entry whose key and value both
 * need a buffer of their own is three allocations, and a failure of the last
 * one must free the key buffer and the node and nothing else. This test is
 * non-vacuous: a node teardown that read the storage of a value it never
 * allocated frees an invalid pointer, and one that skipped the key buffer
 * leaks it. */
TEST(footprint, every_allocation_failure_leaves_nothing_behind) {
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  bool creates_clean = true;
  int creates_failed = 0;
  for (int kind = 0; kind < 3; kind++) {
    for (long at = 1; at <= 3; at++) {
      fp_reset();
      g_fp.fail_at = at;
      chmap m =
          kind == 0 ? chmap_create_mp(16, ccol_string, ccol_pointer, &mp, NULL)
          : kind == 1
              ? chmap_create_mp(16, ccol_long, ccol_long, &mp, NULL)
              : ccol_chmap_create_compact(ccol_string, ccol_pointer, &mp, NULL);
      long calls = g_fp.calls;
      if (m) {
        chmap_destroy(m);
      } else {
        creates_failed++;
      }
      if (g_fp.blocks != 0) creates_clean = false;
      if (at > calls) break;
    }
  }

  const char *k = "a key that is longer than the inline storage";
  char v[64];
  memset(v, 'v', sizeof(v));
  bool inserts_clean = true;
  int inserts_failed = 0;
  for (long at = 1; at <= 3; at++) {
    fp_reset();
    chmap m = chmap_create_mp(16, ccol_string, ccol_string, &mp, NULL);
    if (!m) {
      inserts_clean = false;
      break;
    }
    long blocks_before = g_fp.blocks;
    g_fp.fail_at = g_fp.calls + at;
    ccol_retval_t r = fp_insert_str(m, k, v, sizeof(v));
    g_fp.fail_at = 0;
    if (r == ccol_not_enough_memory) {
      inserts_failed++;
      if (g_fp.blocks != blocks_before || chmap_elem_count(m) != 0)
        inserts_clean = false;
    } else if (r != ccol_success || g_fp.blocks != blocks_before + 3) {
      inserts_clean = false;
    }
    chmap_destroy(m);
    if (g_fp.blocks != 0) inserts_clean = false;
  }
  fp_reset();

  REQUIRE_TRUE(creates_clean);
  /* A separate-chaining map is one allocation, an open-addressing map two,
   * and a compact map one. */
  REQUIRE_EQ(creates_failed, 4);
  REQUIRE_TRUE(inserts_clean);
  REQUIRE_EQ(inserts_failed, 3);
}

/* A value moves between the inline storage of its entry and a buffer of its
 * own as its size crosses the inline limit of 23 bytes, and the entry frees
 * the buffer exactly when the value leaves it. The sizes walk across the
 * limit from both sides. This test is non-vacuous: an entry that decided
 * where its value lives from anything but the size of the value, or that
 * placed the limit one byte off, frees a buffer it does not own or leaks
 * one. */
TEST(footprint, a_value_moves_between_inline_and_heap_storage) {
  fp_reset();
  ccol_memmgmt_procs_t mp = {.malloc = fp_malloc,
                             .free = fp_free,
                             .calloc = fp_calloc,
                             .realloc = fp_realloc};
  chmap m = chmap_create_mp(16, ccol_string, ccol_string, &mp, NULL);
  /* String lengths, without the terminator, of each value in turn. */
  static const size_t lens[] = {7, 22, 23, 47, 22, 23, 23, 7};
  /* The blocks of the entry after each value: 1 inline, 2 with a buffer. */
  static const long want[] = {1, 1, 2, 2, 1, 2, 2, 1};
  enum { STEPS = sizeof(lens) / sizeof(lens[0]) };
  long base = g_fp.blocks;
  bool ok = m != NULL;
  long blocks[STEPS] = {0};
  bool same[STEPS] = {false};
  for (size_t i = 0; ok && i < STEPS; i++) {
    char val[64];
    memset(val, (int)('a' + i), lens[i]);
    val[lens[i]] = '\0';
    ccol_retval_t r = fp_insert_str(m, "key", val, lens[i] + 1);
    ok = r == ccol_success || r == ccol_key_already_present;
    blocks[i] = g_fp.blocks - base;
    cmap_pair kp = {.ptr = "key", .size = 4};
    const cmap_pair *vp = NULL;
    same[i] = ok && chmap_get_elem_ref(m, &kp, &vp) == ccol_success &&
              vp->size == lens[i] + 1 && memcmp(vp->ptr, val, vp->size) == 0;
  }
  /* A new entry places its value by the same limit: 23 bytes inline, 24
   * bytes in a buffer. */
  char v23[23], v24[24];
  memset(v23, 'x', sizeof(v23));
  memset(v24, 'y', sizeof(v24));
  v23[sizeof(v23) - 1] = '\0';
  v24[sizeof(v24) - 1] = '\0';
  long before23 = g_fp.blocks;
  bool new23 = ok && fp_insert_str(m, "k23", v23, sizeof(v23)) == ccol_success;
  long blocks23 = g_fp.blocks - before23;
  long before24 = g_fp.blocks;
  bool new24 = ok && fp_insert_str(m, "k24", v24, sizeof(v24)) == ccol_success;
  long blocks24 = g_fp.blocks - before24;
  if (m) chmap_destroy(m);
  long left = g_fp.blocks;

  REQUIRE_TRUE(ok);
  for (size_t i = 0; i < STEPS; i++) {
    REQUIRE_EQ(blocks[i], want[i]);
    REQUIRE_TRUE(same[i]);
  }
  REQUIRE_TRUE(new23);
  REQUIRE_TRUE(new24);
  REQUIRE_EQ(blocks23, 1L);
  REQUIRE_EQ(blocks24, 2L);
  REQUIRE_EQ(left, 0L);
}

/* An allocator that hands out adjacent blocks from one arena, with no header
 * between them, and records every live block in a table. Because nothing is
 * ever reused, the blocks of one map sit back to back in the order in which
 * the map asks for them. A block starts at the next multiple of 16 bytes,
 * except an array of pointers from calloc, which needs and gets only the
 * alignment of a pointer, so it can start right where the block before it
 * ends. */
enum { ARENA_BYTES = 1 << 16, ARENA_BLOCKS = 256 };
static _Alignas(16) unsigned char g_arena[ARENA_BYTES];
static size_t g_arena_used;
static struct {
  void *ptr;
  size_t size;
} g_arena_live[ARENA_BLOCKS];
static long g_arena_live_count;
static bool g_arena_bad_free;

static void *arena_alloc_aligned(size_t n, size_t align) {
  size_t start = (g_arena_used + align - 1) & ~(align - 1);
  if (start > ARENA_BYTES || n > ARENA_BYTES - start) return NULL;
  for (int i = 0; i < ARENA_BLOCKS; i++) {
    if (!g_arena_live[i].ptr) {
      void *p = g_arena + start;
      g_arena_used = start + n;
      g_arena_live[i].ptr = p;
      g_arena_live[i].size = n;
      g_arena_live_count++;
      return p;
    }
  }
  return NULL;
}
static void *arena_malloc(size_t n) { return arena_alloc_aligned(n, 16); }
static void arena_free(void *p) {
  if (!p) return;
  for (int i = 0; i < ARENA_BLOCKS; i++) {
    if (g_arena_live[i].ptr == p) {
      g_arena_live[i].ptr = NULL;
      g_arena_live_count--;
      return;
    }
  }
  g_arena_bad_free = true;
}
static void *arena_calloc(size_t n, size_t sz) {
  if (sz != 0 && n > SIZE_MAX / sz) return NULL;
  void *p = arena_alloc_aligned(
      n * sz, sz == sizeof(void *) ? sizeof(void *) : (size_t)16);
  if (p) memset(p, 0, n * sz);
  return p;
}
static void *arena_realloc(void *p, size_t n) {
  if (!p) return arena_malloc(n);
  size_t old = 0;
  bool found = false;
  for (int i = 0; i < ARENA_BLOCKS; i++) {
    if (g_arena_live[i].ptr == p) {
      old = g_arena_live[i].size;
      found = true;
    }
  }
  if (!found) {
    g_arena_bad_free = true;
    return NULL;
  }
  void *q = arena_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, old < n ? old : n);
  arena_free(p);
  return q;
}

/* A map that holds neither a bucket array nor a procs copy in its block
 * ends where its struct ends, and an allocator with no headers can place its
 * separate bucket array exactly there. The map must still free that array
 * on a resize, a reset and a destroy. This test is non-vacuous: a map that
 * compares its bucket array against the address right after its struct
 * whether or not its block holds an array there takes the separate array
 * for part of the block, and leaks it and every array that replaces it. */
TEST(footprint, a_bucket_array_right_after_the_block_is_still_freed) {
  memset(g_arena_live, 0, sizeof(g_arena_live));
  g_arena_used = 0;
  g_arena_live_count = 0;
  g_arena_bad_free = false;
  ccol_memmgmt_procs_t mp = {.malloc = arena_malloc,
                             .free = arena_free,
                             .calloc = arena_calloc,
                             .realloc = arena_realloc};
  ccol_memmgmt_procs_t *interned = NULL;
  ccol_retval_t ri = ccol_procs_intern(&mp, &interned);
  /* 64 buckets is above the size that a block holds, and the interned procs
   * need no copy, so the block is the struct alone. */
  chmap m = interned ? chmap_create_full(64, ccol_string, ccol_int, interned,
                                         NULL, NULL, NULL)
                     : NULL;
  bool created = m != NULL;
  long after_create = g_arena_live_count;
  bool ok = created && fp_insert_range(m, 0, 100);
  long after_grow = g_arena_live_count;
  ok = ok && chmap_reset(m, 1024) == ccol_success;
  long after_reset = g_arena_live_count;
  ok = ok && fp_insert_range(m, 0, 3) && fp_delete_range(m, 0, 3);
  if (m) chmap_destroy(m);
  long left = g_arena_live_count;

  REQUIRE_EQ(ri, ccol_success);
  REQUIRE_TRUE(created);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(after_create, 2L);
  /* The map, one bucket array, and an entry for each key. */
  REQUIRE_EQ(after_grow, 2L + 100L);
  REQUIRE_EQ(after_reset, 2L);
  REQUIRE_FALSE(g_arena_bad_free);
  REQUIRE_EQ(left, 0L);
}

/* ========================================================================== */
/*        ADAPTIVE HASHING: FAST UNTIL A WRITER SEES A FLOOD, THEN KEYED      */
/* ========================================================================== */

extern bool chashmap_is_keyed_for_tests(chmap chm);
extern ccol_retval_t chashmap_switch_to_keyed_for_tests(chmap chm);
extern size_t chashmap_oa_probe_span_for_tests(chmap chm);
extern size_t chashmap_oa_insert_cap_for_tests(size_t capacity);
extern uint32_t chashmap_oa_window_bound_for_tests(size_t start_count,
                                                   size_t capacity);
extern uint32_t chashmap_sc_window_bound_for_tests(size_t start_count,
                                                   size_t bucket_count);
extern uint32_t chashmap_window_bound_of_for_tests(chmap chm);
extern uint32_t chashmap_insert_window_for_tests(chmap chm);
extern unsigned long long chashmap_retries_for_tests(void);
extern unsigned long long chashmap_retry_failures_for_tests(void);
extern unsigned long long chashmap_retry_failed_work_for_tests(void);
extern void chashmap_last_failed_retry_for_tests(size_t *work, size_t *count,
                                                 size_t *capacity);
extern uint64_t chashmap_xxh64_for_tests(const void *key_ptr, size_t key_size,
                                         uint64_t seed);
extern unsigned long long chashmap_sc_node_visits_for_tests(void);
extern void chashmap_reset_sc_node_visits_for_tests(void);
extern unsigned long long chashmap_oa_shift_steps_for_tests(void);

/* The Fibonacci multiplier of the fast hash of a fixed-width key. */
static const uint64_t ah_fib = 0x9E3779B97F4A7C15ULL;

/* The unsigned long long key that the fast hash puts in home slot `home` of a
 * table of 2^log2 slots or buckets, because its product with the multiplier
 * has home in its top log2 bits. t, below 2^(64 - log2), makes keys that share
 * that home slot. The same key works on a 32-bit target, where the fast hash
 * of a 64-bit key is the upper word of the same product. */
static unsigned long long ah_key_for_home(uint64_t home, unsigned log2,
                                          uint64_t t) {
  return (unsigned long long)(((home << (64 - log2)) + t) *
                              xxh_pair_inv(ah_fib));
}

static uint64_t ah_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

static ccol_retval_t ah_put_u64(chmap m, unsigned long long k,
                                unsigned long long v) {
  cmap_pair kp = {&k, sizeof(k)};
  cmap_pair vp = {&v, sizeof(v)};
  return chmap_insert_elem(m, &kp, &vp);
}

static bool ah_get_u64(chmap m, unsigned long long k, unsigned long long *v) {
  cmap_pair kp = {&k, sizeof(k)};
  const cmap_pair *out = NULL;
  if (chmap_get_elem_ref(m, &kp, &out) != ccol_success || !out) return false;
  memcpy(v, out->ptr, sizeof(*v));
  return true;
}

static ccol_retval_t ah_del_u64(chmap m, unsigned long long k) {
  cmap_pair kp = {&k, sizeof(k)};
  return chmap_delete_elem(m, &kp);
}

/* An unsigned long long to char * map, which separate chaining holds. */
static ccol_retval_t ah_put_u64_sc(chmap m, unsigned long long k) {
  static char v[] = "v";
  cmap_pair kp = {&k, sizeof(k)};
  cmap_pair vp = {v, sizeof(v)};
  return chmap_insert_elem(m, &kp, &vp);
}

static bool ah_has_u64(chmap m, unsigned long long k) {
  cmap_pair kp = {&k, sizeof(k)};
  const cmap_pair *out = NULL;
  return chmap_get_elem_ref(m, &kp, &out) == ccol_success && out;
}

/* ------------------------------------------------------------------------ */
/*                       XXH64, THE FAST BYTE HASH                          */
/* ------------------------------------------------------------------------ */

/* XXH64 written byte by byte from its specification, with no word loads, as
 * an independent reference. */
static uint64_t ah_rotl(uint64_t x, unsigned r) {
  return (x << r) | (x >> (64u - r));
}

static uint64_t ah_le(const unsigned char *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
  return v;
}

static uint64_t ah_xxh64_ref(const unsigned char *in, size_t len,
                             uint64_t seed) {
  const uint64_t p1 = 0x9E3779B185EBCA87ULL, p2 = 0xC2B2AE3D27D4EB4FULL,
                 p3 = 0x165667B19E3779F9ULL, p4 = 0x85EBCA77C2B2AE63ULL,
                 p5 = 0x27D4EB2F165667C5ULL;
  size_t i = 0;
  uint64_t h;
  if (len >= 32) {
    uint64_t v[4] = {seed + p1 + p2, seed + p2, seed, seed - p1};
    for (; i + 32 <= len; i += 32) {
      for (int l = 0; l < 4; l++) {
        v[l] += ah_le(in + i + 8 * l, 8) * p2;
        v[l] = ah_rotl(v[l], 31) * p1;
      }
    }
    h = ah_rotl(v[0], 1) + ah_rotl(v[1], 7) + ah_rotl(v[2], 12) +
        ah_rotl(v[3], 18);
    for (int l = 0; l < 4; l++) {
      uint64_t r = ah_rotl(v[l] * p2, 31) * p1;
      h = (h ^ r) * p1 + p4;
    }
  } else {
    h = seed + p5;
  }
  h += (uint64_t)len;
  for (; i + 8 <= len; i += 8) {
    h ^= ah_rotl(ah_le(in + i, 8) * p2, 31) * p1;
    h = ah_rotl(h, 27) * p1 + p4;
  }
  if (i + 4 <= len) {
    h ^= ah_le(in + i, 4) * p1;
    h = ah_rotl(h, 23) * p2 + p3;
    i += 4;
  }
  for (; i < len; i++) {
    h ^= (uint64_t)in[i] * p5;
    h = ah_rotl(h, 11) * p1;
  }
  h ^= h >> 33;
  h *= p2;
  h ^= h >> 29;
  h *= p3;
  h ^= h >> 32;
  return h;
}

/* The xxHash library itself (0.8.3) produced the vectors, from the sanity
 * buffer of its own test suite, where byte i is the top byte of a 32-bit
 * generator that starts at 2654435761 and squares itself. */
static const struct {
  size_t len;
  uint64_t seed;
  uint64_t hash;
} ah_xxh_vectors[] = {
    {0, 0x0000000000000000ULL, 0xef46db3751d8e999ULL},
    {1, 0x0000000000000000ULL, 0x4fce394cc88952d8ULL},
    {3, 0x0000000000000000ULL, 0x63e19de8a52309f9ULL},
    {4, 0x0000000000000000ULL, 0x9256e58aa397aef1ULL},
    {7, 0x0000000000000000ULL, 0xab48f5cd83bcb62cULL},
    {8, 0x0000000000000000ULL, 0xf74cb1451b32b8cfULL},
    {11, 0x0000000000000000ULL, 0x4569d4174c3e056eULL},
    {14, 0x0000000000000000ULL, 0xcffa8db881bc3a3dULL},
    {31, 0x0000000000000000ULL, 0xad09d9a6941dd847ULL},
    {32, 0x0000000000000000ULL, 0xaf5753d39159edeeULL},
    {33, 0x0000000000000000ULL, 0x6711cbdd8543baa8ULL},
    {63, 0x0000000000000000ULL, 0xff4410e17ce11efaULL},
    {64, 0x0000000000000000ULL, 0x18f5388f1d2ba08cULL},
    {100, 0x0000000000000000ULL, 0x7da3f79a7d2667c2ULL},
    {222, 0x0000000000000000ULL, 0x9dd507880debb03dULL},
    {0, 0x000000009e3779b1ULL, 0xac75fda2929b17efULL},
    {1, 0x000000009e3779b1ULL, 0x739840cb819fa723ULL},
    {3, 0x000000009e3779b1ULL, 0x16ffba65774bae68ULL},
    {4, 0x000000009e3779b1ULL, 0x09d5ffdfb928ab4bULL},
    {7, 0x000000009e3779b1ULL, 0x3502e64a543d783bULL},
    {8, 0x000000009e3779b1ULL, 0x9c44b77fbcc302c5ULL},
    {11, 0x000000009e3779b1ULL, 0x0a328cd465b6423cULL},
    {14, 0x000000009e3779b1ULL, 0x5b9611585efcc9cbULL},
    {31, 0x000000009e3779b1ULL, 0x9c90d9d9c2e3d340ULL},
    {32, 0x000000009e3779b1ULL, 0xdcab9233b8ca7b0fULL},
    {33, 0x000000009e3779b1ULL, 0x66e9cecf2f1de71cULL},
    {63, 0x000000009e3779b1ULL, 0x7f715f51d0f26050ULL},
    {64, 0x000000009e3779b1ULL, 0x479e7103cf9aa020ULL},
    {100, 0x000000009e3779b1ULL, 0x69d2695b86943c22ULL},
    {222, 0x000000009e3779b1ULL, 0xdc515172b8ee0600ULL},
    {0, 0x9e3779b97f4a7c15ULL, 0xc4349fc93c010000ULL},
    {1, 0x9e3779b97f4a7c15ULL, 0xac55465bd163ccaaULL},
    {3, 0x9e3779b97f4a7c15ULL, 0x271fa28e31a0d124ULL},
    {4, 0x9e3779b97f4a7c15ULL, 0x16f2a5dfc813c355ULL},
    {7, 0x9e3779b97f4a7c15ULL, 0x3f60b26816127cd1ULL},
    {8, 0x9e3779b97f4a7c15ULL, 0x155d1cebeb371bc7ULL},
    {11, 0x9e3779b97f4a7c15ULL, 0x41e33b6a3d6d721aULL},
    {14, 0x9e3779b97f4a7c15ULL, 0xc86eb7cd4b764625ULL},
    {31, 0x9e3779b97f4a7c15ULL, 0x92ae819d6dd7c871ULL},
    {32, 0x9e3779b97f4a7c15ULL, 0xcf5cc84573f83cdeULL},
    {33, 0x9e3779b97f4a7c15ULL, 0x838f70331c3fd20cULL},
    {63, 0x9e3779b97f4a7c15ULL, 0x49bdf6b3cd3be99cULL},
    {64, 0x9e3779b97f4a7c15ULL, 0x894a7e0d35b55d06ULL},
    {100, 0x9e3779b97f4a7c15ULL, 0xf63fe48f6a9c71edULL},
    {222, 0x9e3779b97f4a7c15ULL, 0xd8d4bbe7f21f52cdULL},
};

/* The fast byte hash of the map must be XXH64: the published vectors, "abc",
 * and the byte-wise reference for every length from 0 to 130 at four
 * alignments under random seeds, which covers every tail of the stripe loop
 * and of the word loops. */
TEST(adaptive_hash, the_fast_byte_hash_is_xxh64) {
  static unsigned char sanity[256];
  uint32_t g = 2654435761U;
  for (int i = 0; i < 256; i++) {
    sanity[i] = (unsigned char)(g >> 24);
    g *= g;
  }
  size_t vector_mismatches = 0, ref_vector_mismatches = 0;
  for (size_t i = 0; i < sizeof(ah_xxh_vectors) / sizeof(ah_xxh_vectors[0]);
       i++) {
    if (chashmap_xxh64_for_tests(sanity, ah_xxh_vectors[i].len,
                                 ah_xxh_vectors[i].seed) !=
        ah_xxh_vectors[i].hash) {
      vector_mismatches++;
    }
    if (ah_xxh64_ref(sanity, ah_xxh_vectors[i].len, ah_xxh_vectors[i].seed) !=
        ah_xxh_vectors[i].hash) {
      ref_vector_mismatches++;
    }
  }
  static unsigned char msg[140];
  size_t mismatches = 0;
  for (int t = 0; t < 32; t++) {
    uint64_t seed = ah_mix((uint64_t)t * 7 + 1);
    for (int i = 0; i < 140; i++) msg[i] = (unsigned char)ah_mix(seed + i);
    for (size_t len = 0; len <= 130; len++) {
      for (size_t off = 0; off < 4; off++) {
        if (ah_xxh64_ref(msg + off, len, seed) !=
            chashmap_xxh64_for_tests(msg + off, len, seed)) {
          mismatches++;
        }
      }
    }
  }
  REQUIRE_EQ(chashmap_xxh64_for_tests("abc", 3, 0),
             (uint64_t)0x44bc2cf5ad770999ULL);
  REQUIRE_EQ(ref_vector_mismatches, (size_t)0);
  REQUIRE_EQ(vector_mismatches, (size_t)0);
  REQUIRE_EQ(mismatches, (size_t)0);
}

/* ------------------------------------------------------------------------ */
/*                          THE THRESHOLDS                                  */
/* ------------------------------------------------------------------------ */

/* The window bounds are computed in integers, and must stay within a small
 * margin of the real-valued bounds that they implement, at every load and
 * for both sizes of open-addressing table: 256 * (5 * E(a) + 0.5) from 4096
 * slots, 256 * (6 * E(a) + 0.5) below, with E(a) = (1 / (1 - a)^2 - 1) / 2,
 * and 256 * (4.5 * lambda + 0.5) for separate chaining. a and lambda are the
 * loads of the count at the start of the window plus 256 inserts, capped at
 * the count at which the table grows. */
static double ah_oa_bound_real(size_t start, size_t cap) {
  size_t limit = (cap / 10) * 7 + ((cap % 10) * 7) / 10 + 1;
  size_t judged = start + 256 < limit ? start + 256 : limit;
  double a = (double)judged / (double)cap;
  double e = (1.0 / ((1.0 - a) * (1.0 - a)) - 1.0) / 2.0;
  return 256.0 * ((cap >= 4096 ? 5.0 : 6.0) * e + 0.5);
}

static double ah_sc_bound_real(size_t start, size_t buckets) {
  size_t up = buckets + buckets / 2 + ((buckets % 2 == 0) ? 1 : 2);
  size_t judged = start + 256 < up ? start + 256 : up;
  return 256.0 * (4.5 * (double)judged / (double)buckets + 0.5);
}

TEST(adaptive_hash, the_integer_window_bounds_match_the_real_valued_ones) {
  double worst_oa = 0, worst_sc = 0;
  size_t checked = 0;
  for (unsigned k = 4; k <= 30; k++) {
    size_t cap = (size_t)1 << k;
    for (size_t step = 0; step <= 1000; step++) {
      size_t start = (size_t)((double)cap * 0.70 * (double)step / 1000.0);
      double real = ah_oa_bound_real(start, cap);
      double got = (double)chashmap_oa_window_bound_for_tests(start, cap);
      double err = fabs(got - real) / real;
      if (err > worst_oa) worst_oa = err;
      size_t sc_start = (size_t)((double)cap * 1.5 * (double)step / 1000.0);
      double sc_real = ah_sc_bound_real(sc_start, cap);
      double sc_got = (double)chashmap_sc_window_bound_for_tests(sc_start, cap);
      double sc_err = fabs(sc_got - sc_real) / sc_real;
      if (sc_err > worst_sc) worst_sc = sc_err;
      checked++;
    }
  }
  /* Spot values of the real-valued bounds, independent of both. */
  /* The judged load is (2^19 + 256) / 2^20 = 0.500244, where
   * 256 * (5 * E(a) + 0.5) = 2050.5. */
  uint32_t at_half =
      chashmap_oa_window_bound_for_tests((size_t)1 << 19, (size_t)1 << 20);
  REQUIRE_GT(checked, (size_t)20000);
  /* The integer forms truncate a 16.16 load and divide once: well under one
   * part in a hundred, and one count of 256 at the smallest bound. */
  REQUIRE_LT(worst_oa, 0.01);
  REQUIRE_LT(worst_sc, 0.01);
  REQUIRE_GE(at_half, (uint32_t)2045);
  REQUIRE_LE(at_half, (uint32_t)2051);
}

/* The cap on one open-addressing insert: 24 slots for each doubling of the
 * table up to 4096 slots, and 288 from there on. The values are literals, so
 * that a change of either constant fails here. */
TEST(adaptive_hash, the_insert_cap_grows_with_the_table_up_to_4096_slots) {
  REQUIRE_EQ(chashmap_oa_insert_cap_for_tests(16), (size_t)96);
  REQUIRE_EQ(chashmap_oa_insert_cap_for_tests(512), (size_t)216);
  REQUIRE_EQ(chashmap_oa_insert_cap_for_tests(1024), (size_t)240);
  REQUIRE_EQ(chashmap_oa_insert_cap_for_tests(4096), (size_t)288);
  REQUIRE_EQ(chashmap_oa_insert_cap_for_tests((size_t)1 << 20), (size_t)288);
  REQUIRE_EQ(chashmap_oa_insert_cap_for_tests((size_t)1 << 30), (size_t)288);
}

/* Every constructor starts a map fast. */
TEST(adaptive_hash, every_map_starts_fast) {
  chmap oa = chmap_create(16, ccol_long, ccol_long, NULL);
  chmap sc = chmap_create(16, ccol_string, ccol_long, NULL);
  chmap compact =
      ccol_chmap_create_compact(ccol_string, ccol_pointer, NULL, NULL);
  bool oa_fast = oa && !chashmap_is_keyed_for_tests(oa);
  bool sc_fast = sc && !chashmap_is_keyed_for_tests(sc);
  bool compact_fast = compact && !chashmap_is_keyed_for_tests(compact);
  size_t span = chashmap_oa_probe_span_for_tests(oa);
  if (oa) chmap_destroy(oa);
  if (sc) chmap_destroy(sc);
  if (compact) chmap_destroy(compact);
  REQUIRE_TRUE(oa_fast);
  REQUIRE_TRUE(sc_fast);
  REQUIRE_TRUE(compact_fast);
  /* A table of 16 slots: the span is the whole table. */
  REQUIRE_EQ(span, (size_t)16);
}

/* ------------------------------------------------------------------------ */
/*                     ORDINARY KEYS NEVER SWITCH                           */
/* ------------------------------------------------------------------------ */

enum {
  AH_FAMILY_SEQUENTIAL,
  AH_FAMILY_RANDOM,
  AH_FAMILY_STRIDE_8,
  AH_FAMILY_STRIDE_10,
  AH_FAMILY_STRIDE_16,
  AH_FAMILY_STRIDE_64,
  AH_FAMILY_STRIDE_100,
  AH_FAMILY_PACKED_PAIRS,
  AH_FAMILY_POINTERS,
  AH_FAMILY_COUNT
};

static unsigned long long ah_ordinary_key(int family, uint64_t i) {
  switch (family) {
    case AH_FAMILY_SEQUENTIAL:
      return i;
    case AH_FAMILY_RANDOM:
      return ah_mix(i);
    case AH_FAMILY_STRIDE_8:
      return i * 8;
    case AH_FAMILY_STRIDE_10:
      return i * 10;
    case AH_FAMILY_STRIDE_16:
      return i * 16;
    case AH_FAMILY_STRIDE_64:
      return i * 64;
    case AH_FAMILY_STRIDE_100:
      return i * 100;
    case AH_FAMILY_PACKED_PAIRS:
      return ((i / 300) << 32) | (i % 300);
    default:
      return 0x7f3a5c100000ULL + i * 16;
  }
}

/* Ordinary integer key sets, which the fast hash spreads well, never switch
 * either backend, and cost few probes. This test is non-vacuous: with the
 * window bound at E(a) for each insert in place of 5 * E(a), an ordinary set
 * switches. */
TEST(adaptive_hash, ordinary_integer_sets_never_switch) {
  enum { OA_KEYS = 100000, SC_KEYS = 30000 };
  int oa_switched = 0, sc_switched = 0, missing = 0;
  unsigned long long worst_lookup = 0;
  for (int f = 0; f < AH_FAMILY_COUNT; f++) {
    chmap m = chmap_create(16, ccol_unsigned_long_long, ccol_unsigned_long_long,
                           NULL);
    for (uint64_t i = 0; i < OA_KEYS; i++) {
      if (ah_put_u64(m, ah_ordinary_key(f, i), i) != ccol_success) missing++;
    }
    chashmap_reset_oa_probe_steps_for_tests();
    for (uint64_t i = 0; i < OA_KEYS; i++) {
      unsigned long long v = 0;
      if (!ah_get_u64(m, ah_ordinary_key(f, i), &v) || v != i) missing++;
    }
    unsigned long long probes = chashmap_oa_probe_steps_for_tests();
    if (probes > worst_lookup) worst_lookup = probes;
    if (chashmap_is_keyed_for_tests(m)) oa_switched++;
    chmap_destroy(m);

    chmap s = chmap_create(16, ccol_unsigned_long_long, ccol_string, NULL);
    for (uint64_t i = 0; i < SC_KEYS; i++) {
      if (ah_put_u64_sc(s, ah_ordinary_key(f, i)) != ccol_success) missing++;
    }
    for (uint64_t i = 0; i < SC_KEYS; i++) {
      if (!ah_has_u64(s, ah_ordinary_key(f, i))) missing++;
    }
    if (chashmap_is_keyed_for_tests(s)) sc_switched++;
    chmap_destroy(s);
  }
  REQUIRE_EQ(missing, 0);
  REQUIRE_EQ(oa_switched, 0);
  REQUIRE_EQ(sc_switched, 0);
  REQUIRE_LT((long long)worst_lookup, (long long)OA_KEYS * 3);
}

/* Fills buf with the ordinary string key i of family f. */
static void ah_string_key(int family, uint64_t i, char *buf, size_t size) {
  static const char *const words[] = {
      "id",     "name",  "type",    "created", "updated", "owner",
      "status", "count", "value",   "parent",  "items",   "labels",
      "url",    "email", "address", "version", "kind",    "metadata"};
  uint64_t r = ah_mix(i ^ ((uint64_t)family << 56));
  switch (family) {
    case 0:
      snprintf(buf, size, "/srv/data/%04llu/%06llu.json",
               (unsigned long long)(i / 1000), (unsigned long long)i);
      break;
    case 1:
      snprintf(buf, size, "%08llx-%04llx-4%03llx-a%03llx-%012llx",
               (unsigned long long)(r >> 32),
               (unsigned long long)((r >> 16) & 0xffff),
               (unsigned long long)(r & 0xfff),
               (unsigned long long)((r >> 40) & 0xfff),
               (unsigned long long)(ah_mix(r) & 0xffffffffffffULL));
      break;
    case 2:
      snprintf(buf, size, "%016llx", (unsigned long long)r);
      break;
    default:
      snprintf(buf, size, "%s_%s%llu", words[i % 18], words[(i / 18) % 18],
               (unsigned long long)(i / 324));
      break;
  }
}

/* Ordinary string key sets (paths, UUIDs, hexadecimal identifiers, field
 * names) never switch a map. This test is non-vacuous: with the chain cap at
 * 2 nodes in place of 20, they switch. */
TEST(adaptive_hash, ordinary_string_sets_never_switch) {
  enum { KEYS = 50000 };
  int switched = 0, missing = 0;
  char key[96];
  for (int f = 0; f < 4; f++) {
    chmap m = chmap_create(16, ccol_string, ccol_int, NULL);
    for (uint64_t i = 0; i < KEYS; i++) {
      ah_string_key(f, i, key, sizeof(key));
      int v = (int)i;
      cmap_pair kp = {key, strlen(key) + 1};
      cmap_pair vp = {&v, sizeof(v)};
      if (chmap_insert_elem(m, &kp, &vp) != ccol_success) missing++;
    }
    for (uint64_t i = 0; i < KEYS; i++) {
      ah_string_key(f, i, key, sizeof(key));
      cmap_pair kp = {key, strlen(key) + 1};
      const cmap_pair *out = NULL;
      if (chmap_get_elem_ref(m, &kp, &out) != ccol_success ||
          *(const int *)out->ptr != (int)i) {
        missing++;
      }
    }
    if (chashmap_is_keyed_for_tests(m)) switched++;
    chmap_destroy(m);
  }
  REQUIRE_EQ(missing, 0);
  REQUIRE_EQ(switched, 0);
}

/* The maps of a parsed JSON document: many small compact maps whose keys are
 * field names, none of which switches. */
TEST(adaptive_hash, compact_json_shaped_maps_never_switch) {
  enum { MAPS = 2000 };
  int switched = 0, failed = 0;
  char key[96];
  for (int n = 0; n < MAPS; n++) {
    chmap m = ccol_chmap_create_compact(ccol_string, ccol_pointer, NULL, NULL);
    if (!m) {
      failed++;
      continue;
    }
    int fields = 3 + n % 40;
    for (int f = 0; f < fields; f++) {
      ah_string_key(3, (uint64_t)(f * 7 + n % 5), key, sizeof(key));
      void *v = NULL;
      cmap_pair kp = {key, strlen(key) + 1};
      cmap_pair vp = {&v, sizeof(v)};
      const cmap_pair *slot = NULL;
      ccol_retval_t r = ccol_chmap_insert_or_get_elem(m, &kp, &vp, &slot);
      if (r != ccol_success && r != ccol_key_already_present) failed++;
    }
    if (chashmap_is_keyed_for_tests(m)) switched++;
    chmap_destroy(m);
  }
  REQUIRE_EQ(failed, 0);
  REQUIRE_EQ(switched, 0);
}

/* A map that churns at its highest load, with random keys and with a sliding
 * window of sequential keys, 150000 deletes and 150000 inserts in each
 * workload, never switches and never resizes. This test is non-vacuous: with a
 * cap of 8 slots for each doubling in place of 24, the random churn of the
 * open-addressing map switches. */
TEST(adaptive_hash, churn_at_the_highest_load_never_switches) {
  enum { ROUNDS = 150000, CAP = 16384, LIVE = 11468 };
  int switched = 0, failed = 0;
  unsigned long long rehashes = 0;
  for (int sliding = 0; sliding < 2; sliding++) {
    chmap m = chmap_create(CAP, ccol_unsigned_long_long,
                           ccol_unsigned_long_long, NULL);
    for (uint64_t i = 0; i < LIVE; i++) {
      unsigned long long k = sliding ? i : ah_mix(i);
      if (ah_put_u64(m, k, i) != ccol_success) failed++;
    }
    unsigned long long r0 = chashmap_oa_rehashes_for_tests();
    for (uint64_t r = 0; r < ROUNDS; r++) {
      /* Each round deletes the oldest live key and inserts a new one. */
      unsigned long long dk = sliding ? r : ah_mix(r);
      if (ah_del_u64(m, dk) != ccol_success) failed++;
      unsigned long long nk = sliding ? r + LIVE : ah_mix(r + LIVE);
      if (ah_put_u64(m, nk, r) != ccol_success) failed++;
    }
    rehashes += chashmap_oa_rehashes_for_tests() - r0;
    if (chmap_elem_count(m) != LIVE) failed++;
    if (!chashmap_oa_check_invariants_for_tests(m)) failed++;
    if (chashmap_is_keyed_for_tests(m)) switched++;
    chmap_destroy(m);
  }
  /* Separate chaining at its highest load: 4096 buckets hold 6144 entries,
   * one below the count at which they grow. */
  char key[96];
  for (int sliding = 0; sliding < 2; sliding++) {
    enum { SC_BUCKETS = 4096, SC_LIVE = 6144 };
    chmap m = chmap_create(SC_BUCKETS, ccol_string, ccol_int, NULL);
    for (uint64_t i = 0; i < SC_LIVE; i++) {
      ah_string_key(sliding ? 0 : 2, i, key, sizeof(key));
      int v = (int)i;
      cmap_pair kp = {key, strlen(key) + 1};
      cmap_pair vp = {&v, sizeof(v)};
      if (chmap_insert_elem(m, &kp, &vp) != ccol_success) failed++;
    }
    for (uint64_t r = 0; r < ROUNDS; r++) {
      ah_string_key(sliding ? 0 : 2, r, key, sizeof(key));
      cmap_pair dp = {key, strlen(key) + 1};
      if (chmap_delete_elem(m, &dp) != ccol_success) failed++;
      ah_string_key(sliding ? 0 : 2, r + SC_LIVE, key, sizeof(key));
      int v = (int)r;
      cmap_pair kp = {key, strlen(key) + 1};
      cmap_pair vp = {&v, sizeof(v)};
      if (chmap_insert_elem(m, &kp, &vp) != ccol_success) failed++;
    }
    if (chmap_get_bucket_arr_size(m) != SC_BUCKETS) failed++;
    if (chashmap_is_keyed_for_tests(m)) switched++;
    chmap_destroy(m);
  }
  REQUIRE_EQ(failed, 0);
  REQUIRE_EQ(rehashes, 0ULL);
  REQUIRE_EQ(switched, 0);
}

/* The number of inserts of a new key in the current window of a fast map. */
static unsigned ah_window_inserts(chmap m) {
  return (unsigned)((chashmap_insert_window_for_tests(m) - 0x80000000u) >> 23);
}

/* A window can hold a bulk delete, after which the load is far below the
 * load at which most of its inserts ran. The window is judged at the highest
 * load that it can have reached, so a map of random keys at its highest load
 * that loses three fifths of its entries just before a window ends does not
 * switch. This test is non-vacuous: judged at the load at the end of the
 * window, these windows switch the map. */
TEST(adaptive_hash, a_bulk_delete_inside_a_window_does_not_switch) {
  enum { CAP = 16384, LIVE = 11468, CYCLES = 12 };
  chmap m =
      chmap_create(CAP, ccol_unsigned_long_long, ccol_unsigned_long_long, NULL);
  uint64_t next = 0, oldest = 0;
  int failed = 0, windows_judged = 0;
  for (; next < LIVE; next++) {
    if (ah_put_u64(m, ah_mix(next), next) != ccol_success) failed++;
  }
  for (int c = 0; c < CYCLES; c++) {
    /* Churn at the highest load until the window holds 250 inserts. */
    while (ah_window_inserts(m) != 250) {
      if (ah_del_u64(m, ah_mix(oldest++)) != ccol_success) failed++;
      if (ah_put_u64(m, ah_mix(next), next) != ccol_success) failed++;
      next++;
    }
    /* Delete three fifths of the entries; the load stays above one quarter,
     * so the table does not shrink. */
    uint64_t drop = (next - oldest) * 3 / 5;
    for (uint64_t d = 0; d < drop; d++) {
      if (ah_del_u64(m, ah_mix(oldest++)) != ccol_success) failed++;
    }
    /* Six more inserts end the window. */
    for (int i = 0; i < 6; i++) {
      if (ah_put_u64(m, ah_mix(next), next) != ccol_success) failed++;
      next++;
    }
    windows_judged++;
    /* Refill to the highest load. */
    while (next - oldest < LIVE) {
      if (ah_put_u64(m, ah_mix(next), next) != ccol_success) failed++;
      next++;
    }
  }
  bool keyed = chashmap_is_keyed_for_tests(m);
  size_t cap = chmap_get_bucket_arr_size(m);
  chmap_destroy(m);
  REQUIRE_EQ(failed, 0);
  REQUIRE_EQ(windows_judged, CYCLES);
  REQUIRE_EQ(cap, (size_t)CAP);
  REQUIRE_FALSE(keyed);
}

/* ------------------------------------------------------------------------ */
/*                  FLOODS SWITCH, THEN SPREAD AS RANDOM                    */
/* ------------------------------------------------------------------------ */

enum { AH_FLOOD_KEYS = 20000 };

/* Inserts AH_FLOOD_KEYS keys of the generator into a fresh open-addressing
 * map, then looks every one up, and reports the insert at which the map
 * switched (0 when it did not) and the probes for each lookup after the
 * fill. */
typedef unsigned long long (*ah_gen_t)(uint64_t i);

typedef struct {
  size_t switched_at;
  double lookup_probes;
  bool ok;
} ah_flood_result;

static ah_flood_result ah_flood_oa(ah_gen_t gen, size_t keys) {
  ah_flood_result res = {0, 0, true};
  chmap m =
      chmap_create(16, ccol_unsigned_long_long, ccol_unsigned_long_long, NULL);
  for (uint64_t i = 0; i < keys; i++) {
    if (ah_put_u64(m, gen(i), i) != ccol_success) res.ok = false;
    if (!res.switched_at && chashmap_is_keyed_for_tests(m)) {
      res.switched_at = (size_t)i + 1;
    }
  }
  chashmap_reset_oa_probe_steps_for_tests();
  for (uint64_t i = 0; i < keys; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, gen(i), &v) || v != i) res.ok = false;
  }
  res.lookup_probes =
      (double)chashmap_oa_probe_steps_for_tests() / (double)keys;
  if (chmap_elem_count(m) != keys) res.ok = false;
  if (!chashmap_oa_check_invariants_for_tests(m)) res.ok = false;
  chmap_destroy(m);
  return res;
}

/* The same in an unsigned long long to char * map, which separate chaining
 * holds; the cost is the nodes that a lookup visits. */
static ah_flood_result ah_flood_sc(ah_gen_t gen, size_t keys) {
  ah_flood_result res = {0, 0, true};
  chmap m = chmap_create(16, ccol_unsigned_long_long, ccol_string, NULL);
  for (uint64_t i = 0; i < keys; i++) {
    if (ah_put_u64_sc(m, gen(i)) != ccol_success) res.ok = false;
    if (!res.switched_at && chashmap_is_keyed_for_tests(m)) {
      res.switched_at = (size_t)i + 1;
    }
  }
  chashmap_reset_sc_node_visits_for_tests();
  for (uint64_t i = 0; i < keys; i++) {
    if (!ah_has_u64(m, gen(i))) res.ok = false;
  }
  res.lookup_probes =
      (double)chashmap_sc_node_visits_for_tests() / (double)keys;
  if (chmap_elem_count(m) != keys) res.ok = false;
  chmap_destroy(m);
  return res;
}

static unsigned long long ah_inverse_fibonacci(uint64_t i) {
  return (unsigned long long)(i * xxh_pair_inv(ah_fib));
}

static unsigned long long ah_fibonacci_stride(uint64_t i) {
  return (unsigned long long)(i * 1346269ULL);
}

/* Keys i * M^-1 for the multiplier M of the fast hash all hash to the top
 * bits of a small number and share home slot 0, so both backends switch within
 * the first few hundred inserts, and afterwards a lookup costs what a random
 * function costs. This test is non-vacuous: without the detection, an
 * open-addressing lookup costs 10000.5 probes and the longest chain holds all
 * 20000 keys. */
TEST(adaptive_hash, inverse_fibonacci_integers_switch_then_spread) {
  ah_flood_result oa = ah_flood_oa(ah_inverse_fibonacci, AH_FLOOD_KEYS);
  ah_flood_result sc = ah_flood_sc(ah_inverse_fibonacci, AH_FLOOD_KEYS);
  REQUIRE_TRUE(oa.ok);
  REQUIRE_TRUE(sc.ok);
  REQUIRE_GT(oa.switched_at, (size_t)0);
  REQUIRE_LE(oa.switched_at, (size_t)512);
  REQUIRE_GT(sc.switched_at, (size_t)0);
  REQUIRE_LE(sc.switched_at, (size_t)32);
  REQUIRE_LT(oa.lookup_probes, 2.5);
  REQUIRE_LT(sc.lookup_probes, 2.0);
}

/* The multiples of the Fibonacci number 1346269 pile up in a few dense
 * clusters under the fast hash. This test is non-vacuous: without the
 * detection, an open-addressing lookup costs 9892.2 probes. */
TEST(adaptive_hash, fibonacci_stride_integers_switch_then_spread) {
  ah_flood_result oa = ah_flood_oa(ah_fibonacci_stride, AH_FLOOD_KEYS);
  ah_flood_result sc = ah_flood_sc(ah_fibonacci_stride, AH_FLOOD_KEYS);
  REQUIRE_TRUE(oa.ok);
  REQUIRE_TRUE(sc.ok);
  REQUIRE_GT(oa.switched_at, (size_t)0);
  REQUIRE_LE(oa.switched_at, (size_t)512);
  REQUIRE_GT(sc.switched_at, (size_t)0);
  REQUIRE_LE(sc.switched_at, (size_t)512);
  REQUIRE_LT(oa.lookup_probes, 2.5);
  REQUIRE_LT(sc.lookup_probes, 2.0);
}

static unsigned long long ah_stride_24(uint64_t i) { return i * 24; }
static unsigned long long ah_stride_32(uint64_t i) { return i * 32; }
static unsigned long long ah_stride_48(uint64_t i) { return i * 48; }
static unsigned long long ah_stride_96(uint64_t i) { return i * 96; }
static unsigned long long ah_shift_16(uint64_t i) { return i << 16; }

/* Some strides cluster under a Fibonacci multiply: their products with the
 * multiplier fall into few runs of home slots. Such a set switches once the
 * table is large enough for the clusters to cost more than the bound, and
 * then spreads at least as well as a random function, keyed or, after a
 * growth past the clustering size, fast again. This test is non-vacuous:
 * without the
 * detection, every one of these sets stays fast, and the open-addressing
 * inserts cost from 4.7 to 17.4 probes each. */
TEST(adaptive_hash, clustered_strides_switch_early) {
  static const ah_gen_t oa_sets[] = {ah_stride_24, ah_stride_32, ah_stride_48,
                                     ah_stride_96, ah_shift_16};
  static const ah_gen_t sc_sets[] = {ah_stride_48, ah_stride_96};
  enum { KEYS = 60000 };
  int not_switched = 0, failed = 0;
  double worst = 0;
  for (size_t s = 0; s < sizeof(oa_sets) / sizeof(oa_sets[0]); s++) {
    ah_flood_result r = ah_flood_oa(oa_sets[s], KEYS);
    if (!r.ok) failed++;
    if (r.switched_at == 0 || r.switched_at > 50000) not_switched++;
    if (r.lookup_probes > worst) worst = r.lookup_probes;
  }
  for (size_t s = 0; s < sizeof(sc_sets) / sizeof(sc_sets[0]); s++) {
    ah_flood_result r = ah_flood_sc(sc_sets[s], KEYS);
    if (!r.ok) failed++;
    if (r.switched_at == 0 || r.switched_at > 50000) not_switched++;
  }
  REQUIRE_EQ(failed, 0);
  REQUIRE_EQ(not_switched, 0);
  REQUIRE_LT(worst, 2.5);
}

/* Multiples of 65536 in a table of 2^20 slots cost 44.7 probes for each
 * lookup under the fast hash, so the map switches, and a lookup then costs
 * what a random function costs at the same load. */
TEST(adaptive_hash, stride_65536_in_a_million_slot_table_switches) {
  enum { KEYS = 734000 };
  chmap m = chmap_create((size_t)1 << 20, ccol_unsigned_long_long,
                         ccol_unsigned_long_long, NULL);
  bool ok = true;
  size_t switched_at = 0;
  for (uint64_t i = 0; i < KEYS; i++) {
    if (ah_put_u64(m, i << 16, i) != ccol_success) ok = false;
    if (!switched_at && chashmap_is_keyed_for_tests(m)) switched_at = i + 1;
  }
  size_t cap = chmap_get_bucket_arr_size(m);
  chashmap_reset_oa_probe_steps_for_tests();
  for (uint64_t i = 0; i < KEYS; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, i << 16, &v) || v != i) ok = false;
  }
  double probes = (double)chashmap_oa_probe_steps_for_tests() / KEYS;
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(cap, (size_t)1 << 20);
  REQUIRE_GT(switched_at, (size_t)0);
  REQUIRE_LT(probes, 2.5);
}

/* The 128-byte keys of a multicollision of XXH64 that holds for every seed.
 * Each of the eight lanes of the two pairs of 32-byte stripes takes one of
 * three choices, where a round of a lane is acc = rotl(acc + w * P2, 31) * P1.
 * The second and third choices add 2^63 to acc + w * P2 through the first
 * word, which after the rotation is plus or minus 2^30 depending on one bit
 * of acc, and cancel one of those two signs through the second word. For any
 * seed, the first choice and exactly one of the other two therefore leave the
 * lane in the same state. The 6561 keys hash to 256 values, and 256 of them
 * share one hash, whatever the seed is. */
typedef struct {
  uint64_t w[16];
} ah_xxh_key;

static void ah_xxh_multicollision_key(unsigned index, ah_xxh_key *key) {
  const uint64_t p1 = 0x9E3779B185EBCA87ULL, p2 = 0xC2B2AE3D27D4EB4FULL;
  const uint64_t inv_p2 = xxh_pair_inv(p2);
  for (int i = 0; i < 16; i++) {
    key->w[i] =
        0x0123456789abcdefULL * (uint64_t)(i + 1) ^ 0x5555aaaa3333ccccULL;
  }
  for (int pos = 0; pos < 8; pos++) {
    unsigned choice = index % 3;
    index /= 3;
    int lane = pos % 4, pair = pos / 4;
    int first = (2 * pair) * 4 + lane, second = (2 * pair + 1) * 4 + lane;
    if (choice) {
      key->w[first] += (1ULL << 63) * inv_p2;
      key->w[second] +=
          (choice == 1 ? (uint64_t)0 - (1ULL << 30) : (1ULL << 30)) * p1 *
          inv_p2;
    }
  }
}

static int ah_u64_cmp(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/* The multicollision switches a map within the first two hundred keys, and the
 * keyed map then holds the 6561 keys in short chains. The first 81 keys vary
 * only the first four lanes and fall into classes of at most 16 keys; the fifth
 * lane doubles the classes, at key 82 or at key 163 depending on the seed, and
 * a class then reaches the chain cap of 20. The premise is checked first: under
 * three seeds, the keys hash to 256 values with 256 keys in the largest class.
 * This test is non-vacuous: without the detection, the longest chain holds 256
 * keys. */
TEST(adaptive_hash, a_seed_independent_xxh64_multicollision_switches) {
  enum { KEYS = 6561 };
  uint64_t *hashes = (uint64_t *)malloc(KEYS * sizeof(uint64_t));
  REQUIRE_NE((void *)hashes, NULL);
  size_t worst_distinct = 0, smallest_largest_class = KEYS;
  ah_xxh_key key;
  for (int s = 0; s < 3; s++) {
    uint64_t seed = ah_mix((uint64_t)s + 99);
    for (unsigned i = 0; i < KEYS; i++) {
      ah_xxh_multicollision_key(i, &key);
      hashes[i] = chashmap_xxh64_for_tests(&key, sizeof(key), seed);
    }
    qsort(hashes, KEYS, sizeof(uint64_t), ah_u64_cmp);
    size_t distinct = 1, run = 1, largest = 1;
    for (unsigned i = 1; i < KEYS; i++) {
      if (hashes[i] == hashes[i - 1]) {
        if (++run > largest) largest = run;
      } else {
        distinct++;
        run = 1;
      }
    }
    if (distinct > worst_distinct) worst_distinct = distinct;
    if (largest < smallest_largest_class) smallest_largest_class = largest;
  }
  free(hashes);

  chmap m = chmap_create(16, ccol_other_types, ccol_int, NULL);
  bool ok = m != NULL;
  size_t switched_at = 0;
  for (unsigned i = 0; ok && i < KEYS; i++) {
    ah_xxh_multicollision_key(i, &key);
    int v = (int)i;
    cmap_pair kp = {&key, sizeof(key)};
    cmap_pair vp = {&v, sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) ok = false;
    if (!switched_at && chashmap_is_keyed_for_tests(m)) switched_at = i + 1;
  }
  chashmap_reset_sc_node_visits_for_tests();
  for (unsigned i = 0; ok && i < KEYS; i++) {
    ah_xxh_multicollision_key(i, &key);
    cmap_pair kp = {&key, sizeof(key)};
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(m, &kp, &out) != ccol_success ||
        *(const int *)out->ptr != (int)i) {
      ok = false;
    }
  }
  double visits = (double)chashmap_sc_node_visits_for_tests() / KEYS;
  size_t longest = m ? chashmap_sc_longest_chain_for_tests(m) : 0;
  if (m) chmap_destroy(m);

  REQUIRE_EQ(worst_distinct, (size_t)256);
  REQUIRE_EQ(smallest_largest_class, (size_t)256);
  REQUIRE_TRUE(ok);
  REQUIRE_GT(switched_at, (size_t)0);
  REQUIRE_LE(switched_at, (size_t)200);
  REQUIRE_LE(longest, (size_t)12);
  REQUIRE_LT(visits, 2.0);
}

/* A custom hash is finalized with the murmur3 finalizer in the fast mode, a
 * public bijection, so keys that come out of the identity custom hash as the
 * inverse images of 0, 1, 2, ... under it share home slot 0. Both backends
 * switch, and the keyed mode then finalizes with the seeded mixer. This test
 * is non-vacuous: without the detection, an open-addressing insert costs
 * 10001.6 probes. */
static unsigned long ah_identity_hash(const void *key_ptr, size_t key_size) {
  (void)key_size;
  unsigned long k;
  memcpy(&k, key_ptr, sizeof(k));
  return k;
}

TEST(adaptive_hash, a_custom_hash_flood_switches) {
  enum { KEYS = 20000 };
  chmap oa = chmap_create_full(16, ccol_unsigned_long, ccol_unsigned_long, NULL,
                               ah_identity_hash, NULL, NULL);
  chmap sc = chmap_create_full(16, ccol_unsigned_long, ccol_string, NULL,
                               ah_identity_hash, NULL, NULL);
  REQUIRE_NE((void *)oa, NULL);
  REQUIRE_NE((void *)sc, NULL);
  bool ok = true;
  size_t oa_at = 0, sc_at = 0;
  static char v[] = "v";
  for (size_t i = 0; i < KEYS; i++) {
    unsigned long k = (unsigned long)murmur_fmix_inverse(i);
    unsigned long val = (unsigned long)i;
    cmap_pair kp = {&k, sizeof(k)};
    cmap_pair vp = {&val, sizeof(val)};
    cmap_pair sp = {v, sizeof(v)};
    if (chmap_insert_elem(oa, &kp, &vp) != ccol_success) ok = false;
    if (chmap_insert_elem(sc, &kp, &sp) != ccol_success) ok = false;
    if (!oa_at && chashmap_is_keyed_for_tests(oa)) oa_at = i + 1;
    if (!sc_at && chashmap_is_keyed_for_tests(sc)) sc_at = i + 1;
  }
  chashmap_reset_oa_probe_steps_for_tests();
  chashmap_reset_sc_node_visits_for_tests();
  for (size_t i = 0; i < KEYS; i++) {
    unsigned long k = (unsigned long)murmur_fmix_inverse(i);
    cmap_pair kp = {&k, sizeof(k)};
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(oa, &kp, &out) != ccol_success ||
        *(const unsigned long *)out->ptr != (unsigned long)i) {
      ok = false;
    }
    if (chmap_get_elem_ref(sc, &kp, &out) != ccol_success) ok = false;
  }
  double oa_probes = (double)chashmap_oa_probe_steps_for_tests() / KEYS;
  double sc_visits = (double)chashmap_sc_node_visits_for_tests() / KEYS;
  __chmap_destroy(oa);
  __chmap_destroy(sc);
  REQUIRE_TRUE(ok);
  REQUIRE_GT(oa_at, (size_t)0);
  REQUIRE_LE(oa_at, (size_t)512);
  REQUIRE_GT(sc_at, (size_t)0);
  REQUIRE_LE(sc_at, (size_t)32);
  REQUIRE_LT(oa_probes, 2.5);
  REQUIRE_LT(sc_visits, 2.0);
}

/* Strings whose XXH64 under the seed of the process shares its top bits: the
 * flood that a party who learned the seed computes. The map switches at the
 * twenty-first of them, and the keyed map spreads them. This test is
 * non-vacuous: without the chain cap the map switches only when the first
 * window ends, and without the detection the longest chain holds all 600
 * keys. */
TEST(adaptive_hash, a_string_chain_flood_switches) {
  enum { KEYS = 600, KEY_LEN = 24, TOP_BITS = 10 };
  static char keys[KEYS][KEY_LEN];
  chmap m = chmap_create(16, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)m, NULL);
  uint64_t secret[6];
  chashmap_hash_secret_for_tests(m, secret);
  const uint64_t byte_seed = secret[5];
  unsigned long long ctr = 0;
  for (size_t got = 0; got < KEYS;) {
    char k[KEY_LEN];
    int kl = snprintf(k, sizeof(k), "f%llx", ctr++);
    uint64_t h = chashmap_xxh64_for_tests(k, (size_t)kl + 1, byte_seed);
#if SIZE_MAX != UINT64_MAX
    h = (uint32_t)(h ^ (h >> 32));
    h <<= 32;
#endif
    if ((h >> (64 - TOP_BITS)) != 0) continue;
    memcpy(keys[got++], k, (size_t)kl + 1);
  }
  bool ok = true;
  size_t switched_at = 0;
  for (size_t i = 0; i < KEYS; i++) {
    int v = (int)i;
    cmap_pair kp = {keys[i], strlen(keys[i]) + 1};
    cmap_pair vp = {&v, sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) ok = false;
    if (!switched_at && chashmap_is_keyed_for_tests(m)) switched_at = i + 1;
  }
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(switched_at, (size_t)21);
  REQUIRE_LE(longest, (size_t)16);
}

/* ------------------------------------------------------------------------ */
/*                        CHURN AT A CHOSEN COST                            */
/* ------------------------------------------------------------------------ */

/* Builds a fast open-addressing map of 2^13 slots at load 0.49 whose keys sit
 * in their own even home slots, outside [hole_lo, hole_hi), and then a run of
 * `run` keys with the consecutive home slots run_home, run_home + 1, ..., all
 * of whose inserts have displacement 0. Gives NULL when anything fails. */
static chmap ah_oa_with_run(uint64_t hole_lo, uint64_t hole_hi,
                            uint64_t run_home, uint64_t run) {
  enum { LOG2 = 13 };
  chmap m = chmap_create((size_t)1 << LOG2, ccol_unsigned_long_long,
                         ccol_unsigned_long_long, NULL);
  bool ok = m != NULL;
  for (uint64_t s = 0; ok && s < ((uint64_t)1 << LOG2) - 128; s += 2) {
    if (s >= hole_lo && s < hole_hi) continue;
    ok = ah_put_u64(m, ah_key_for_home(s, LOG2, 0), s) == ccol_success;
  }
  for (uint64_t i = 0; ok && i < run; i++) {
    ok = ah_put_u64(m, ah_key_for_home(run_home + i, LOG2, 0), i) ==
         ccol_success;
  }
  if (ok && chashmap_is_keyed_for_tests(m)) ok = false;
  if (!ok && m) {
    chmap_destroy(m);
    m = NULL;
  }
  return m;
}

/* An insert and a delete that repeat with a displacement of 24 every time:
 * below the deep-insert path at 32 and far below the cap, so only the window
 * sees it. At a load of about one half the bound is about 9.5 for each
 * insert, and the map switches within two windows. This test is non-vacuous:
 * without the window judgement the map stays fast through every round. */
TEST(adaptive_hash, churn_at_displacement_24_switches_within_two_windows) {
  enum { ROUNDS = 2000 };
  chmap m = ah_oa_with_run(1000, 1200, 1024, 24);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  size_t switched_at = 0;
  for (uint64_t r = 0; r < ROUNDS && !switched_at; r++) {
    unsigned long long k = ah_key_for_home(1024, 13, 1000 + r);
    if (ah_put_u64(m, k, r) != ccol_success) ok = false;
    if (chashmap_is_keyed_for_tests(m)) switched_at = (size_t)r + 1;
    if (ah_del_u64(m, k) != ccol_success) ok = false;
  }
  for (uint64_t i = 0; i < 24; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, ah_key_for_home(1024 + i, 13, 0), &v) || v != i) {
      ok = false;
    }
  }
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_GT(switched_at, (size_t)0);
  REQUIRE_LE(switched_at, (size_t)512);
}

/* The same churn with a displacement of 340, above the cap of 288 of a table
 * of 8192 slots: the first such insert switches the map. This test is
 * non-vacuous: without the cap check it takes the window to switch. */
TEST(adaptive_hash, churn_at_displacement_340_switches_at_once) {
  chmap m = ah_oa_with_run(1000, 1500, 1000, 340);
  REQUIRE_NE((void *)m, NULL);
  unsigned long long k = ah_key_for_home(1000, 13, 77);
  ccol_retval_t r = ah_put_u64(m, k, 1);
  bool keyed = chashmap_is_keyed_for_tests(m);
  unsigned long long v = 0;
  bool found = ah_get_u64(m, k, &v) && v == 1;
  bool ok = chashmap_oa_check_invariants_for_tests(m);
  chmap_destroy(m);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_TRUE(keyed);
  REQUIRE_TRUE(found);
  REQUIRE_TRUE(ok);
}

/* Builds a fast separate-chaining map of 1024 buckets that holds `fillers`
 * keys, at most two in each bucket outside bucket 500, and a chain of
 * `chain` keys in bucket 500. */
static chmap ah_sc_with_chain(uint64_t fillers, uint64_t chain) {
  enum { LOG2 = 10 };
  chmap m = chmap_create((size_t)1 << LOG2, ccol_unsigned_long_long,
                         ccol_string, NULL);
  bool ok = m != NULL;
  for (uint64_t i = 0, placed = 0; ok && placed < fillers; i++) {
    uint64_t bucket = i % 1024;
    if (bucket == 500) continue;
    ok = ah_put_u64_sc(m, ah_key_for_home(bucket, LOG2, 1 + i / 1024)) ==
         ccol_success;
    placed++;
  }
  for (uint64_t i = 0; ok && i < chain; i++) {
    ok = ah_put_u64_sc(m, ah_key_for_home(500, LOG2, 100 + i)) == ccol_success;
  }
  if (ok && (chashmap_is_keyed_for_tests(m) ||
             chmap_get_bucket_arr_size(m) != 1024)) {
    ok = false;
  }
  if (!ok && m) {
    chmap_destroy(m);
    m = NULL;
  }
  return m;
}

/* Churn of one key into a chain of `chain` nodes; gives the round at which
 * the map switched, or 0. */
static size_t ah_sc_chain_churn(chmap m, uint64_t rounds, bool *ok) {
  for (uint64_t r = 0; r < rounds; r++) {
    unsigned long long k = ah_key_for_home(500, 10, 5000 + r);
    if (ah_put_u64_sc(m, k) != ccol_success) *ok = false;
    bool keyed = chashmap_is_keyed_for_tests(m);
    if (ah_del_u64(m, k) != ccol_success) *ok = false;
    if (keyed) return (size_t)r + 1;
  }
  return 0;
}

/* A new key that meets 18 nodes is below the chain cap of 20, so only the
 * window sees this churn; at a load of about one entry for each bucket it
 * switches within two windows. This test is non-vacuous: without the window
 * judgement the map stays fast. */
TEST(adaptive_hash, churn_at_chain_18_switches_within_two_windows) {
  chmap m = ah_sc_with_chain(1000, 18);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  size_t at = ah_sc_chain_churn(m, 2000, &ok);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_GT(at, (size_t)0);
  REQUIRE_LE(at, (size_t)512);
}

/* A chain of 8 at the highest load, 1.5 entries for each bucket, where the
 * bound is about 7.25 nodes for each insert: the map switches within two
 * windows. This test is non-vacuous: without the window judgement the map
 * stays fast. */
TEST(adaptive_hash, churn_at_chain_8_switches_within_two_windows) {
  chmap m = ah_sc_with_chain(1527, 8);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  size_t at = ah_sc_chain_churn(m, 2000, &ok);
  size_t buckets = chmap_get_bucket_arr_size(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(buckets, (size_t)1024);
  REQUIRE_GT(at, (size_t)0);
  REQUIRE_LE(at, (size_t)512);
}

/* One insert whose displacement is above the cap switches the map at once;
 * one whose displacement equals the cap does not, and widens probe_span to
 * cover it. Checked at 1024 slots, where the cap is 240, and at 65536 slots,
 * where it is 288. This test is non-vacuous: without the cap check no single
 * insert switches a map. */
TEST(adaptive_hash, a_deep_insert_past_the_cap_switches_small_and_large) {
  static const unsigned log2s[] = {10, 16};
  bool ok = true;
  int fast_at_cap = 0, keyed_past_cap = 0, span_covers = 0;
  for (size_t t = 0; t < 2; t++) {
    unsigned lg = log2s[t];
    size_t capacity = (size_t)1 << lg;
    size_t cap = chashmap_oa_insert_cap_for_tests(capacity);
    for (int past = 0; past < 2; past++) {
      chmap m = chmap_create(capacity, ccol_unsigned_long_long,
                             ccol_unsigned_long_long, NULL);
      /* A run of cap + past keys with consecutive home slots from 100. */
      for (uint64_t i = 0; ok && i < cap + (size_t)past; i++) {
        ok = ah_put_u64(m, ah_key_for_home(100 + i, lg, 0), i) == ccol_success;
      }
      bool fast_before = !chashmap_is_keyed_for_tests(m);
      ok = ok && ah_put_u64(m, ah_key_for_home(100, lg, 9), 9) == ccol_success;
      bool keyed = chashmap_is_keyed_for_tests(m);
      size_t span = chashmap_oa_probe_span_for_tests(m);
      if (!fast_before || chmap_get_bucket_arr_size(m) != capacity) ok = false;
      if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
      if (past) {
        if (keyed) keyed_past_cap++;
      } else {
        if (!keyed) fast_at_cap++;
        if (span == cap + 1) span_covers++;
      }
      chmap_destroy(m);
    }
  }
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(fast_at_cap, 2);
  REQUIRE_EQ(span_covers, 2);
  REQUIRE_EQ(keyed_past_cap, 2);
}

/* ------------------------------------------------------------------------ */
/*                     PROBE_SPAN AND THE SHRINK CHECKS                     */
/* ------------------------------------------------------------------------ */

/* A run of 5000 keys with consecutive home slots: every insert lands in its
 * home slot, so nothing switches the map, and the run is as long as an
 * attacker wants. A lookup or a delete of an absent key whose home slot is in
 * the run, and the backward-shift walk of a delete inside it, stop after
 * probe_span slots. This test is non-vacuous: without the bound on the
 * lookup, the absent lookups below take 7.8 million probes, and so do the
 * absent deletes without the bound on the delete search; without the bound
 * on the walk, the 100 deletes inside the run take 390000 steps. */
TEST(adaptive_hash, lookups_and_deletes_stay_within_probe_span) {
  enum { LOG2 = 14, RUN = 5000, ABSENT = 2000 };
  chmap m = chmap_create((size_t)1 << LOG2, ccol_unsigned_long_long,
                         ccol_unsigned_long_long, NULL);
  bool ok = m != NULL;
  for (uint64_t i = 0; ok && i < RUN; i++) {
    ok = ah_put_u64(m, ah_key_for_home(i, LOG2, 0), i) == ccol_success;
  }
  bool fast = !chashmap_is_keyed_for_tests(m);
  size_t span = chashmap_oa_probe_span_for_tests(m);
  chashmap_reset_oa_probe_steps_for_tests();
  int found_absent = 0;
  for (uint64_t i = 0; i < ABSENT; i++) {
    unsigned long long v = 0;
    if (ah_get_u64(m, ah_key_for_home(100 + i, LOG2, 777), &v)) found_absent++;
  }
  unsigned long long lookup_probes = chashmap_oa_probe_steps_for_tests();
  chashmap_reset_oa_probe_steps_for_tests();
  for (uint64_t i = 0; i < ABSENT; i++) {
    if (ah_del_u64(m, ah_key_for_home(100 + i, LOG2, 777)) !=
        ccol_key_not_found) {
      found_absent++;
    }
  }
  unsigned long long delete_probes = chashmap_oa_probe_steps_for_tests();
  unsigned long long s0 = chashmap_oa_shift_steps_for_tests();
  for (uint64_t i = 0; i < 100; i++) {
    if (ah_del_u64(m, ah_key_for_home(1000 + 2 * i, LOG2, 0)) != ccol_success) {
      ok = false;
    }
  }
  unsigned long long walk_steps = chashmap_oa_shift_steps_for_tests() - s0;
  for (uint64_t i = 0; i < RUN; i++) {
    bool deleted = i >= 1000 && i < 1200 && (i % 2) == 0;
    unsigned long long v = 0;
    if (ah_get_u64(m, ah_key_for_home(i, LOG2, 0), &v) != !deleted) ok = false;
  }
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  if (m) chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(fast);
  REQUIRE_EQ(span, (size_t)32);
  REQUIRE_EQ(found_absent, 0);
  /* Each absent key walks the whole span, inside the run. */
  REQUIRE_EQ(lookup_probes, (unsigned long long)ABSENT * 32);
  REQUIRE_EQ(delete_probes, (unsigned long long)ABSENT * 32);
  /* Each walk examines at most span - 1 slots past the hole. */
  REQUIRE_LE(walk_steps, 100ULL * 31);
}

/* A shrink of an open-addressing table merges clusters: keys in consecutive
 * home slots of the large table share home slots in pairs in the small one.
 * When that makes a displacement above the cap of the new table, the shrink
 * builds it with the keyed hash. This test is non-vacuous: without the
 * check of the rebuilt table, the map stays fast with a displacement of
 * 2000 and a probe_span to match. */
TEST(adaptive_hash, an_open_addressing_shrink_that_merges_clusters_switches) {
  enum { LOG2 = 14, RUN = 4000, FILLERS = 200 };
  chmap m = chmap_create((size_t)1 << LOG2, ccol_unsigned_long_long,
                         ccol_unsigned_long_long, NULL);
  bool ok = m != NULL;
  for (uint64_t i = 0; ok && i < RUN; i++) {
    ok = ah_put_u64(m, ah_key_for_home(i, LOG2, 0), i) == ccol_success;
  }
  for (uint64_t i = 0; ok && i < FILLERS; i++) {
    ok = ah_put_u64(m, ah_key_for_home(8000 + 2 * i, LOG2, 0), i) ==
         ccol_success;
  }
  bool fast_before = !chashmap_is_keyed_for_tests(m);
  for (uint64_t i = 0; ok && i < FILLERS; i++) {
    ok = ah_del_u64(m, ah_key_for_home(8000 + 2 * i, LOG2, 0)) == ccol_success;
  }
  size_t cap_after = chmap_get_bucket_arr_size(m);
  bool keyed_after = chashmap_is_keyed_for_tests(m);
  size_t span = chashmap_oa_probe_span_for_tests(m);
  for (uint64_t i = 0; ok && i < RUN; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, ah_key_for_home(i, LOG2, 0), &v) || v != i) ok = false;
  }
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  if (m) chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(fast_before);
  REQUIRE_EQ(cap_after, (size_t)1 << (LOG2 - 1));
  REQUIRE_TRUE(keyed_after);
  REQUIRE_LE(span, (size_t)288);
}

/* A shrink of a separate-chaining map merges the chains of four neighbouring
 * buckets. Five keys in each of buckets 400 to 403 of 1024 meet at most four
 * nodes on insert, and merge into one chain of 20 when the array shrinks to
 * 256 buckets: the shrink switches the map. This test is non-vacuous:
 * without the measure of the merged chains, the map stays fast with a chain
 * of 20. */
TEST(adaptive_hash, a_separate_chaining_shrink_that_merges_chains_switches) {
  enum { LOG2 = 10, FILLERS = 600 };
  chmap m = chmap_create(16, ccol_unsigned_long_long, ccol_string, NULL);
  bool ok = m != NULL;
  /* Grow to 1024 buckets with keys spread over buckets other than 400 to
   * 403 of that size. */
  for (uint64_t i = 0; ok && i < FILLERS; i++) {
    uint64_t bucket = (i * 7) % 1024;
    if (bucket >= 396 && bucket < 408) bucket += 100;
    ok = ah_put_u64_sc(m, ah_key_for_home(bucket, LOG2, 1 + i)) == ccol_success;
  }
  size_t grown = chmap_get_bucket_arr_size(m);
  for (uint64_t b = 400; ok && b < 404; b++) {
    for (uint64_t t = 0; ok && t < 5; t++) {
      ok =
          ah_put_u64_sc(m, ah_key_for_home(b, LOG2, 50000 + t)) == ccol_success;
    }
  }
  bool fast_before = !chashmap_is_keyed_for_tests(m);
  for (uint64_t i = 0; ok && i < FILLERS; i++) {
    uint64_t bucket = (i * 7) % 1024;
    if (bucket >= 396 && bucket < 408) bucket += 100;
    ok = ah_del_u64(m, ah_key_for_home(bucket, LOG2, 1 + i)) == ccol_success;
  }
  size_t shrunk = chmap_get_bucket_arr_size(m);
  bool keyed = chashmap_is_keyed_for_tests(m);
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  for (uint64_t b = 400; ok && b < 404; b++) {
    for (uint64_t t = 0; t < 5; t++) {
      if (!ah_has_u64(m, ah_key_for_home(b, LOG2, 50000 + t))) ok = false;
    }
  }
  if (m) chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(grown, (size_t)1024);
  REQUIRE_TRUE(fast_before);
  REQUIRE_LE(shrunk, (size_t)256);
  REQUIRE_TRUE(keyed);
  REQUIRE_LT(longest, (size_t)20);
}

/* ------------------------------------------------------------------------ */
/*                       THE SWITCH KEEPS EVERYTHING                        */
/* ------------------------------------------------------------------------ */

/* A separate-chaining map switches in place: every entry, its value, the
 * address of its value and of its key accessor, the insertion order that the
 * oldest-first cursor walks, the newest-first order of the iterator, and an
 * iterator that is part way through, all survive. Keys are 128-byte structs:
 * 3000 random ones, then the multicollision family switches the map. This
 * test is non-vacuous: a switch that leaves a node out of the buckets fails
 * the lookups. */
TEST(adaptive_hash, the_switch_keeps_every_separate_chaining_entry) {
  enum { HONEST = 3000, FLOOD = 300 };
  chmap m = chmap_create(16, ccol_other_types, ccol_int, NULL);
  REQUIRE_NE((void *)m, NULL);
  static ah_xxh_key keys[HONEST + FLOOD];
  static const cmap_pair *val_refs[HONEST];
  static const cmap_pair *key_refs[HONEST];
  bool ok = true;
  for (unsigned i = 0; i < HONEST; i++) {
    for (int w = 0; w < 16; w++) keys[i].w[w] = ah_mix(i * 16u + (unsigned)w);
    int v = (int)i;
    cmap_pair kp = {&keys[i], sizeof(keys[i])};
    cmap_pair vp = {&v, sizeof(v)};
    if (ccol_chmap_insert_or_get_entry(m, &kp, &vp, &key_refs[i],
                                       &val_refs[i]) != ccol_success) {
      ok = false;
    }
  }
  bool fast_before = !chashmap_is_keyed_for_tests(m);
  /* An iterator that has visited the newest half. */
  chmap_redeclare(m, ah_xxh_key, int);
  ccol_iter_declare(m, it);
  it = ccol_begin(m);
  for (unsigned n = 0; it && n < HONEST / 2; n++) it = ccol_iter_next(it);
  for (unsigned i = 0; i < FLOOD; i++) {
    ah_xxh_multicollision_key(i, &keys[HONEST + i]);
    int v = (int)(HONEST + i);
    cmap_pair kp = {&keys[HONEST + i], sizeof(keys[0])};
    cmap_pair vp = {&v, sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) ok = false;
  }
  bool keyed = chashmap_is_keyed_for_tests(m);
  /* The iterator goes on with the older half, newest first. */
  unsigned expect = HONEST / 2;
  while (it) {
    expect--;
    if (memcmp(it->key_pair->ptr, &keys[expect], sizeof(keys[0])) != 0) {
      ok = false;
    }
    it = ccol_iter_next(it);
  }
  if (expect != 0) ok = false;
  /* Every honest entry keeps its accessors and its value. */
  for (unsigned i = 0; i < HONEST; i++) {
    cmap_pair kp = {&keys[i], sizeof(keys[i])};
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(m, &kp, &out) != ccol_success ||
        out != val_refs[i] || *(const int *)out->ptr != (int)i) {
      ok = false;
    }
    const cmap_pair *ks = NULL, *vs = NULL;
    int unused = -1;
    cmap_pair up = {&unused, sizeof(unused)};
    if (ccol_chmap_insert_or_get_entry(m, &kp, &up, &ks, &vs) !=
            ccol_key_already_present ||
        ks != key_refs[i] || vs != val_refs[i]) {
      ok = false;
    }
  }
  /* Insertion order, oldest first, over all entries. */
  unsigned pos = 0;
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(m); e;) {
    const cmap_pair *k = NULL, *v = NULL;
    e = ccol_chmap_entry_read(e, &k, &v);
    if (pos >= HONEST + FLOOD ||
        memcmp(k->ptr, &keys[pos], sizeof(keys[0])) != 0 ||
        *(const int *)v->ptr != (int)pos) {
      ok = false;
    }
    pos++;
  }
  /* Deletes after the switch: every other key. */
  for (unsigned i = 0; i < HONEST + FLOOD; i += 2) {
    cmap_pair kp = {&keys[i], sizeof(keys[0])};
    if (chmap_delete_elem(m, &kp) != ccol_success) ok = false;
  }
  size_t left = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(fast_before);
  REQUIRE_TRUE(keyed);
  REQUIRE_EQ(pos, (unsigned)(HONEST + FLOOD));
  REQUIRE_EQ(left, (size_t)(HONEST + FLOOD) / 2);
}

/* An open-addressing map switches by building a new table: every entry and
 * value survives, an iteration visits each key once, and deletes work. */
TEST(adaptive_hash, the_switch_keeps_every_open_addressing_entry) {
  enum { HONEST = 5000, FLOOD = 600 };
  chmap m =
      chmap_create(16, ccol_unsigned_long_long, ccol_unsigned_long_long, NULL);
  bool ok = true;
  for (uint64_t i = 0; i < HONEST; i++) {
    if (ah_put_u64(m, ah_mix(i), i) != ccol_success) ok = false;
  }
  bool fast_before = !chashmap_is_keyed_for_tests(m);
  for (uint64_t i = 1; i <= FLOOD; i++) {
    if (ah_put_u64(m, ah_inverse_fibonacci(i), HONEST + i) != ccol_success) {
      ok = false;
    }
  }
  bool keyed = chashmap_is_keyed_for_tests(m);
  unsigned long long sum = 0;
  size_t visited = 0;
  chmap_redeclare(m, unsigned long long, unsigned long long);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
    sum += *(const unsigned long long *)it->val_pair->ptr;
    visited++;
  }
  for (uint64_t i = 0; i < HONEST; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, ah_mix(i), &v) || v != i) ok = false;
    if (ah_del_u64(m, ah_mix(i)) != ccol_success) ok = false;
  }
  for (uint64_t i = 1; i <= FLOOD; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, ah_inverse_fibonacci(i), &v) || v != HONEST + i) {
      ok = false;
    }
  }
  size_t left = chmap_elem_count(m);
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  chmap_destroy(m);
  unsigned long long expect = 0;
  for (uint64_t i = 0; i < HONEST + FLOOD + 1; i++) expect += i;
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(fast_before);
  REQUIRE_TRUE(keyed);
  REQUIRE_EQ(visited, (size_t)(HONEST + FLOOD));
  REQUIRE_EQ(sum, expect - HONEST);
  REQUIRE_EQ(left, (size_t)FLOOD);
}

/* A compact map, the kind that a JSON or YAML parser makes for every
 * object, switches on a flood and keeps its insertion order. */
TEST(adaptive_hash, a_compact_map_switches_and_keeps_its_order) {
  enum { KEYS = 400 };
  chmap m = ccol_chmap_create_compact(ccol_other_types, ccol_int, NULL, NULL);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  ah_xxh_key key;
  for (unsigned i = 0; i < KEYS; i++) {
    ah_xxh_multicollision_key(i, &key);
    int v = (int)i;
    cmap_pair kp = {&key, sizeof(key)};
    cmap_pair vp = {&v, sizeof(v)};
    const cmap_pair *slot = NULL;
    if (ccol_chmap_insert_or_get_elem(m, &kp, &vp, &slot) != ccol_success ||
        *(const int *)slot->ptr != (int)i) {
      ok = false;
    }
  }
  bool keyed = chashmap_is_keyed_for_tests(m);
  unsigned pos = 0;
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(m); e; pos++) {
    const cmap_pair *k = NULL, *v = NULL;
    e = ccol_chmap_entry_read(e, &k, &v);
    ah_xxh_multicollision_key(pos, &key);
    if (memcmp(k->ptr, &key, sizeof(key)) != 0 ||
        *(const int *)v->ptr != (int)pos) {
      ok = false;
    }
  }
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(keyed);
  REQUIRE_EQ(pos, (unsigned)KEYS);
  REQUIRE_LE(longest, (size_t)12);
}

/* A keyed map stays keyed across a reset and a shrink, which keep the mode,
 * and across growths whose fast attempts all fail, because the flood that
 * switched it collides under the fast hash at every size; it keeps spreading
 * that flood. A growth of a keyed map whose keys spread under the fast hash
 * returns to it; see honest_strides_return_to_the_fast_hash_after_growing. */
TEST(adaptive_hash, a_keyed_map_stays_keyed_across_reset_grow_and_shrink) {
  enum { KEYS = 6000 };
  chmap oa =
      chmap_create(16, ccol_unsigned_long_long, ccol_unsigned_long_long, NULL);
  chmap sc = chmap_create(16, ccol_unsigned_long_long, ccol_string, NULL);
  bool ok = true;
  for (uint64_t i = 0; i < 1000; i++) {
    if (ah_put_u64(oa, ah_inverse_fibonacci(i), i) != ccol_success) ok = false;
    if (ah_put_u64_sc(sc, ah_inverse_fibonacci(i)) != ccol_success) ok = false;
  }
  bool switched =
      chashmap_is_keyed_for_tests(oa) && chashmap_is_keyed_for_tests(sc);
  if (chmap_reset(oa, 0) != ccol_success) ok = false;
  if (chmap_reset(sc, 64) != ccol_success) ok = false;
  bool keyed_after_reset =
      chashmap_is_keyed_for_tests(oa) && chashmap_is_keyed_for_tests(sc);
  /* Grow both well past their sizes, then shrink them back down. */
  chashmap_reset_oa_probe_steps_for_tests();
  unsigned long long attempts0 = chashmap_retries_for_tests();
  unsigned long long failures0 = chashmap_retry_failures_for_tests();
  for (uint64_t i = 0; i < KEYS; i++) {
    if (ah_put_u64(oa, ah_inverse_fibonacci(i), i) != ccol_success) ok = false;
    if (ah_put_u64_sc(sc, ah_inverse_fibonacci(i)) != ccol_success) ok = false;
  }
  double grow_probes = (double)chashmap_oa_probe_steps_for_tests() / KEYS;
  unsigned long long attempts = chashmap_retries_for_tests() - attempts0;
  unsigned long long failures = chashmap_retry_failures_for_tests() - failures0;
  bool keyed_after_grow =
      chashmap_is_keyed_for_tests(oa) && chashmap_is_keyed_for_tests(sc);
  size_t oa_grown = chmap_get_bucket_arr_size(oa);
  size_t sc_grown = chmap_get_bucket_arr_size(sc);
  for (uint64_t i = 0; i < KEYS - 50; i++) {
    if (ah_del_u64(oa, ah_inverse_fibonacci(i)) != ccol_success) ok = false;
    if (ah_del_u64(sc, ah_inverse_fibonacci(i)) != ccol_success) ok = false;
  }
  bool keyed_after_shrink =
      chashmap_is_keyed_for_tests(oa) && chashmap_is_keyed_for_tests(sc);
  size_t oa_shrunk = chmap_get_bucket_arr_size(oa);
  size_t sc_shrunk = chmap_get_bucket_arr_size(sc);
  for (uint64_t i = KEYS - 50; i < KEYS; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(oa, ah_inverse_fibonacci(i), &v) || v != i) ok = false;
    if (!ah_has_u64(sc, ah_inverse_fibonacci(i))) ok = false;
  }
  if (!chashmap_oa_check_invariants_for_tests(oa)) ok = false;
  chmap_destroy(oa);
  chmap_destroy(sc);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(switched);
  REQUIRE_TRUE(keyed_after_reset);
  REQUIRE_TRUE(keyed_after_grow);
  REQUIRE_GE(attempts, 2ull);
  REQUIRE_EQ(failures, attempts);
  REQUIRE_TRUE(keyed_after_shrink);
  REQUIRE_GT(oa_grown, oa_shrunk);
  REQUIRE_GT(sc_grown, sc_shrunk);
  /* A random function costs 2.6 to 3.1 probes for each insert here, the
   * growth included; the flood under the fast hash costs thousands. */
  REQUIRE_LT(grow_probes, 4.0);
}

/* A reset restarts the insert window: the walks that the window held before
 * the reset do not count toward the window after it. This test is
 * non-vacuous: without the restart, the first insert after the reset ends a
 * window of 255 inserts with displacements 0 to 254 against the bound of an
 * empty table, and switches the map. */
TEST(adaptive_hash, a_reset_restarts_the_window) {
  chmap m = chmap_create(8192, ccol_unsigned_long_long, ccol_unsigned_long_long,
                         NULL);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  /* 255 keys that share home slot 100, below the cap of 288. */
  for (uint64_t i = 0; ok && i < 255; i++) {
    ok = ah_put_u64(m, ah_key_for_home(100, 13, i), i) == ccol_success;
  }
  bool fast_before =
      !chashmap_is_keyed_for_tests(m) && ah_window_inserts(m) == 255;
  ok = ok && chmap_reset(m, 0) == ccol_success;
  ok = ok && ah_put_u64(m, 12345, 1) == ccol_success;
  bool fast_after = !chashmap_is_keyed_for_tests(m);
  unsigned in_window = ah_window_inserts(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(fast_before);
  REQUIRE_TRUE(fast_after);
  REQUIRE_EQ(in_window, 1u);
}

/* Key identity does not depend on the mode. -0.0 and 0.0 are one key and a
 * long double is one key whatever its padding, before and after a switch,
 * and ccol_chmap_hash_key gives the hash that a keyed map stores, whatever
 * the mode of any map is. */
TEST(adaptive_hash, key_identity_does_not_depend_on_the_mode) {
  chmap d = chmap_create(16, ccol_double, ccol_int, NULL);
  chmap l = chmap_create(16, ccol_long_double, ccol_int, NULL);
  chmap s = chmap_create(16, ccol_string, ccol_int, NULL);
  REQUIRE_NE((void *)d, NULL);
  REQUIRE_NE((void *)l, NULL);
  REQUIRE_NE((void *)s, NULL);
  int one = 1, out = 0;
  bool ok = true;
  for (int pass = 0; pass < 2; pass++) {
    double nz = -0.0, pz = 0.0;
    cmap_pair kn = {&nz, sizeof(nz)}, kz = {&pz, sizeof(pz)};
    cmap_pair vp = {&one, sizeof(one)};
    if (chmap_insert_elem(d, &kn, &vp) == ccol_invalid_args) ok = false;
    if (chmap_get_elem_copy(d, &kz, &out, sizeof(out)) != ccol_success) {
      ok = false;
    }
    long double a, b;
    memset(&a, 0xA5, sizeof(a));
    memset(&b, 0x5A, sizeof(b));
    a = 3.25L;
    b = 3.25L;
    cmap_pair la = {&a, sizeof(a)}, lb = {&b, sizeof(b)};
    if (chmap_insert_elem(l, &la, &vp) == ccol_invalid_args) ok = false;
    if (chmap_get_elem_copy(l, &lb, &out, sizeof(out)) != ccol_success) {
      ok = false;
    }
    if (chmap_elem_count(d) != 1 || chmap_elem_count(l) != 1) ok = false;
    if (pass == 0) {
      if (chashmap_switch_to_keyed_for_tests(d) != ccol_success) ok = false;
      if (chashmap_switch_to_keyed_for_tests(l) != ccol_success) ok = false;
    }
  }
  const char *k = "shared-key";
  cmap_pair kp = {(void *)k, strlen(k) + 1};
  cmap_pair vp = {&one, sizeof(one)};
  ok = ok && chmap_insert_elem(s, &kp, &vp) == ccol_success;
  size_t fast_stored = chashmap_sc_stored_hash_for_tests(s, k, strlen(k) + 1);
  ok = ok && chashmap_switch_to_keyed_for_tests(s) == ccol_success;
  size_t keyed_stored = chashmap_sc_stored_hash_for_tests(s, k, strlen(k) + 1);
  size_t identity = ccol_chmap_hash_key(k, strlen(k) + 1, ccol_string);
  bool keyed = chashmap_is_keyed_for_tests(d) &&
               chashmap_is_keyed_for_tests(l) && chashmap_is_keyed_for_tests(s);
  chmap_destroy(d);
  chmap_destroy(l);
  chmap_destroy(s);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(keyed);
  REQUIRE_EQ(keyed_stored, identity);
  REQUIRE_NE(fast_stored, identity);
}

/* ------------------------------------------------------------------------ */
/*                     A SWITCH THAT CANNOT ALLOCATE                        */
/* ------------------------------------------------------------------------ */

static long g_ah_alloc_index = 0;
static long g_ah_fail_at = -1;

static bool ah_alloc_fails(void) { return g_ah_alloc_index++ == g_ah_fail_at; }
static void *ah_malloc(size_t n) { return ah_alloc_fails() ? NULL : malloc(n); }
static void *ah_calloc(size_t n, size_t s) {
  return ah_alloc_fails() ? NULL : calloc(n, s);
}
static void *ah_realloc(void *p, size_t n) {
  return ah_alloc_fails() ? NULL : realloc(p, n);
}

/* The scenario of the next test: a table of 1024 slots with a run of 241
 * keys in consecutive home slots, then two inserts at the start of the run,
 * each past the cap of 240. Reports whether the map is keyed, and whether
 * every key that the calls reported as inserted is there. */
static void ah_deep_scenario(long fail_at, bool *created, bool *keyed,
                             bool *intact, long *allocations) {
  ccol_memmgmt_procs_t mp = {.malloc = ah_malloc,
                             .calloc = ah_calloc,
                             .realloc = ah_realloc,
                             .free = free};
  g_ah_alloc_index = 0;
  g_ah_fail_at = fail_at;
  chmap m = chmap_create_full(1024, ccol_unsigned_long_long,
                              ccol_unsigned_long_long, &mp, NULL, NULL, NULL);
  *created = m != NULL;
  *keyed = false;
  *intact = true;
  if (m) {
    bool inserted[243] = {false};
    for (uint64_t i = 0; i < 241; i++) {
      inserted[i] =
          ah_put_u64(m, ah_key_for_home(100 + i, 10, 0), i) == ccol_success;
    }
    inserted[241] =
        ah_put_u64(m, ah_key_for_home(100, 10, 1), 241) == ccol_success;
    inserted[242] =
        ah_put_u64(m, ah_key_for_home(100, 10, 2), 242) == ccol_success;
    for (uint64_t i = 0; i < 243; i++) {
      unsigned long long k = i < 241 ? ah_key_for_home(100 + i, 10, 0)
                                     : ah_key_for_home(100, 10, i - 240);
      unsigned long long v = 0;
      bool present = ah_get_u64(m, k, &v) && v == i;
      if (present != inserted[i] || !inserted[i]) *intact = false;
    }
    if (!chashmap_oa_check_invariants_for_tests(m)) *intact = false;
    *keyed = chashmap_is_keyed_for_tests(m);
    chmap_destroy(m);
  }
  *allocations = g_ah_alloc_index;
  g_ah_fail_at = -1;
}

/* Each allocation of the scenario fails in turn. A failed creation fails
 * cleanly, and a failed switch leaves the map fast and correct: the insert
 * that asked for it succeeds, and the next insert past the cap switches. This
 * test is non-vacuous: a failed switch that marks the map keyed while its
 * table still holds the fast hash fails the intact check. */
TEST(adaptive_hash, a_switch_that_cannot_allocate_keeps_the_map_correct) {
  bool created = false, keyed = false, intact = false;
  long allocations = 0;
  ah_deep_scenario(-1, &created, &keyed, &intact, &allocations);
  REQUIRE_TRUE(created);
  REQUIRE_TRUE(keyed);
  REQUIRE_TRUE(intact);
  /* The map block, the table, and the one block of the switch. */
  REQUIRE_EQ(allocations, 3L);
  int failed_creations = 0, recovered = 0, broken = 0;
  for (long f = 0; f < allocations; f++) {
    long used = 0;
    ah_deep_scenario(f, &created, &keyed, &intact, &used);
    if (!created) {
      failed_creations++;
      continue;
    }
    if (!intact) broken++;
    if (keyed) recovered++;
  }
  REQUIRE_EQ(broken, 0);
  REQUIRE_EQ(failed_creations, 2);
  REQUIRE_EQ(recovered, 1);
}

/* A rebuild restarts the insert window, so a window never mixes the bound of
 * one table with the inserts of another. A table of 8192 slots ends a window at
 * a load of one quarter, where the bound of the next window is about 2.8 for
 * each insert, and then shrinks to 4096 slots at a load of one half, where it
 * is about 11.1. A churn at displacement 4 that follows is ordinary there and
 * does not switch the map. This test is non-vacuous: without the restart, the
 * window that the shrink interrupts keeps the bound of the large table and the
 * churn switches the map. */
TEST(adaptive_hash, a_shrink_restarts_the_open_addressing_window) {
  enum { LOG2 = 13 };
  chmap m = chmap_create((size_t)1 << LOG2, ccol_unsigned_long_long,
                         ccol_unsigned_long_long, NULL);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  /* Fillers in the slots 4k of the large table, which are the even home
   * slots 2k of the small one, leaving homes 200 to 299 of the small table
   * free. */
  for (uint64_t k = 0; ok && k < 2048; k++) {
    if (k >= 100 && k < 150) continue;
    ok = ah_put_u64(m, ah_key_for_home(4 * k, LOG2, 0), k) == ccol_success;
  }
  /* Extra keys in slots 4k + 2 until the count is at least 2048 and a
   * window has just ended. */
  uint64_t extra = 0;
  while (ok && (chmap_elem_count(m) < 2048 || ah_window_inserts(m) != 0)) {
    ok = ah_put_u64(m, ah_key_for_home(4 * (1000 + extra) + 2, LOG2, 0),
                    extra) == ccol_success;
    extra++;
  }
  /* Delete the extra keys and then fillers until the table shrinks. */
  for (uint64_t e = 0; ok && e < extra; e++) {
    ok = ah_del_u64(m, ah_key_for_home(4 * (1000 + e) + 2, LOG2, 0)) ==
         ccol_success;
  }
  for (uint64_t k = 0; ok && chmap_get_bucket_arr_size(m) == 8192; k++) {
    ok = ah_del_u64(m, ah_key_for_home(4 * k, LOG2, 0)) == ccol_success;
  }
  bool shrunk = chmap_get_bucket_arr_size(m) == 4096;
  /* Four keys in the consecutive free homes 200 to 203 of the small table,
   * then 600 rounds that insert a key with home 200 and delete it. */
  for (uint64_t i = 0; ok && i < 4; i++) {
    ok =
        ah_put_u64(m, ah_key_for_home(200 + i, LOG2 - 1, 0), i) == ccol_success;
  }
  for (uint64_t r = 0; ok && r < 600; r++) {
    unsigned long long k = ah_key_for_home(200, LOG2 - 1, 100 + r);
    ok =
        ah_put_u64(m, k, r) == ccol_success && ah_del_u64(m, k) == ccol_success;
  }
  bool fast = !chashmap_is_keyed_for_tests(m);
  bool still_small = chmap_get_bucket_arr_size(m) == 4096;
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(shrunk);
  REQUIRE_TRUE(still_small);
  REQUIRE_TRUE(fast);
}

/* The same for separate chaining: a map of 1024 buckets ends a window at a
 * quarter of an entry for each bucket, where the bound of the next window is
 * about 2.75 nodes for each insert, and then shrinks to 256 buckets, where the
 * window that starts is judged at the highest load, 1.5, and its bound is about
 * 7.2. A churn into a chain of 4 nodes is ordinary there. This test is
 * non-vacuous: without the restart, the window that the shrink interrupts keeps
 * the bound of the large array and the churn switches the map. */
TEST(adaptive_hash, a_shrink_restarts_the_separate_chaining_window) {
  enum { LOG2 = 10 };
  chmap m = chmap_create((size_t)1 << LOG2, ccol_unsigned_long_long,
                         ccol_string, NULL);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  /* One key in each bucket of 1024 outside 800 to 803, which merge into
   * bucket 200 of 256, until the count is at least 130 and a window has just
   * ended. */
  uint64_t placed = 0;
  for (uint64_t b = 0; ok && (placed < 130 || ah_window_inserts(m) != 0); b++) {
    if (b % 1024 >= 800 && b % 1024 < 804) continue;
    ok = ah_put_u64_sc(m, ah_key_for_home(b % 1024, LOG2, 1 + b / 1024)) ==
         ccol_success;
    placed++;
  }
  bool big = chmap_get_bucket_arr_size(m) == 1024;
  for (uint64_t b = 0; ok && chmap_get_bucket_arr_size(m) == 1024; b++) {
    if (b % 1024 >= 800 && b % 1024 < 804) continue;
    ok = ah_del_u64(m, ah_key_for_home(b % 1024, LOG2, 1 + b / 1024)) ==
         ccol_success;
  }
  bool shrunk = chmap_get_bucket_arr_size(m) == 256;
  for (uint64_t i = 0; ok && i < 4; i++) {
    ok = ah_put_u64_sc(m, ah_key_for_home(200, LOG2 - 2, 10 + i)) ==
         ccol_success;
  }
  for (uint64_t r = 0; ok && r < 600; r++) {
    unsigned long long k = ah_key_for_home(200, LOG2 - 2, 1000 + r);
    ok =
        ah_put_u64_sc(m, k) == ccol_success && ah_del_u64(m, k) == ccol_success;
  }
  bool fast = !chashmap_is_keyed_for_tests(m);
  bool still_small = chmap_get_bucket_arr_size(m) == 256;
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(big);
  REQUIRE_TRUE(shrunk);
  REQUIRE_TRUE(still_small);
  REQUIRE_TRUE(fast);
}

/* ------------------------------------------------------------------------ */
/*                   THE FAST ATTEMPT OF A KEYED GROWTH                     */
/* ------------------------------------------------------------------------ */

static unsigned ah_log2_of(size_t capacity) {
  unsigned lg = 0;
  while (((size_t)1 << lg) < capacity) lg++;
  return lg;
}

/* Strides that cluster under the Fibonacci multiply at some table sizes on
 * the way up switch a map at a small size, and spread again in a larger
 * table. The growth that reaches that size makes its fast attempt, the
 * attempt fills the table, and the map ends fast, with a lookup that costs
 * one probe, against 1.31 probes for a random function at the same load (a
 * keyed table costs that). The premise is checked: every set switched on
 * the way. This test is non-vacuous: without the fast attempt every set
 * stays keyed. */
TEST(adaptive_hash, honest_strides_return_to_the_fast_hash_after_growing) {
  static const ah_gen_t oa_sets[] = {ah_stride_24, ah_stride_32, ah_stride_48,
                                     ah_stride_96};
  static const ah_gen_t sc_sets[] = {ah_stride_48, ah_stride_96};
  enum { KEYS = 100000 };
  int not_switched = 0, ended_keyed = 0, failed = 0;
  double worst = 0;
  for (size_t s = 0; s < sizeof(oa_sets) / sizeof(oa_sets[0]); s++) {
    chmap m = chmap_create(16, ccol_unsigned_long_long, ccol_unsigned_long_long,
                           NULL);
    bool switched = false;
    for (uint64_t i = 0; i < KEYS; i++) {
      if (ah_put_u64(m, oa_sets[s](i), i) != ccol_success) failed++;
      if (chashmap_is_keyed_for_tests(m)) switched = true;
    }
    if (!switched) not_switched++;
    if (chashmap_is_keyed_for_tests(m)) ended_keyed++;
    chashmap_reset_oa_probe_steps_for_tests();
    for (uint64_t i = 0; i < KEYS; i++) {
      unsigned long long v = 0;
      if (!ah_get_u64(m, oa_sets[s](i), &v) || v != i) failed++;
    }
    double probes = (double)chashmap_oa_probe_steps_for_tests() / KEYS;
    if (probes > worst) worst = probes;
    if (!chashmap_oa_check_invariants_for_tests(m)) failed++;
    chmap_destroy(m);
  }
  for (size_t s = 0; s < sizeof(sc_sets) / sizeof(sc_sets[0]); s++) {
    chmap m = chmap_create(16, ccol_unsigned_long_long, ccol_string, NULL);
    bool switched = false;
    for (uint64_t i = 0; i < KEYS; i++) {
      if (ah_put_u64_sc(m, sc_sets[s](i)) != ccol_success) failed++;
      if (chashmap_is_keyed_for_tests(m)) switched = true;
    }
    if (!switched) not_switched++;
    if (chashmap_is_keyed_for_tests(m)) ended_keyed++;
    chashmap_reset_sc_node_visits_for_tests();
    for (uint64_t i = 0; i < KEYS; i++) {
      if (!ah_has_u64(m, sc_sets[s](i))) failed++;
    }
    double visits = (double)chashmap_sc_node_visits_for_tests() / KEYS;
    if (visits > worst) worst = visits;
    chmap_destroy(m);
  }
  REQUIRE_EQ(failed, 0);
  REQUIRE_EQ(not_switched, 0);
  REQUIRE_EQ(ended_keyed, 0);
  REQUIRE_LT(worst, 1.05);
}

/* The work of every failed fast attempt, against the bound that the
 * attempt documents: an open-addressing attempt inspects at most
 * 2 * count + 353 slots (cap + 65, with a cap of at most 288), and a
 * separate-chaining attempt hashes and walks at most 2 * count + 83 nodes. */
typedef struct {
  unsigned long long attempts, failures, failed_work, seen_failures;
  size_t growths, last_capacity;
  bool bound_ok, ok;
} ah_attempt_watch;

static void ah_watch_start(ah_attempt_watch *w, chmap m) {
  memset(w, 0, sizeof(*w));
  w->attempts = chashmap_retries_for_tests();
  w->failures = chashmap_retry_failures_for_tests();
  w->failed_work = chashmap_retry_failed_work_for_tests();
  w->seen_failures = w->failures;
  w->last_capacity = chmap_get_bucket_arr_size(m);
  w->bound_ok = true;
  w->ok = true;
}

static void ah_watch_step(ah_attempt_watch *w, chmap m, bool sc) {
  unsigned long long f = chashmap_retry_failures_for_tests();
  if (f != w->seen_failures) {
    size_t work = 0, count = 0, capacity = 0;
    chashmap_last_failed_retry_for_tests(&work, &count, &capacity);
    size_t bound = sc ? 2 * count + 83 : 2 * count + 353;
    if (f - w->seen_failures != 1 || work > bound) w->bound_ok = false;
    w->seen_failures = f;
  }
  size_t cap = chmap_get_bucket_arr_size(m);
  if (cap != w->last_capacity) {
    w->growths++;
    w->last_capacity = cap;
  }
}

static void ah_watch_end(ah_attempt_watch *w) {
  w->attempts = chashmap_retries_for_tests() - w->attempts;
  w->failures = chashmap_retry_failures_for_tests() - w->failures;
  w->failed_work = chashmap_retry_failed_work_for_tests() - w->failed_work;
}

/* A flood that collides under the fast hash at every size switches the map,
 * and every growth after the switch makes a fast attempt that fails within
 * its bound: the map ends keyed, the failed attempts cost at most a few
 * probes or node visits for each insert in all, and a lookup costs what a
 * random function costs. This test is non-vacuous: without the abort of a
 * failed attempt, an attempt over the inverse-Fibonacci flood fills the
 * whole table with the fast hash, a number of probes quadratic in the count
 * that breaks the bound at the first growth. */
static void ah_flood_across_growths(ah_gen_t gen, bool sc, size_t keys,
                                    ah_attempt_watch *w, bool *keyed,
                                    double *lookup) {
  chmap m = sc ? chmap_create(16, ccol_unsigned_long_long, ccol_string, NULL)
               : chmap_create(16, ccol_unsigned_long_long,
                              ccol_unsigned_long_long, NULL);
  ah_watch_start(w, m);
  size_t growths_while_keyed = 0;
  for (uint64_t i = 0; i < keys; i++) {
    bool was_keyed = chashmap_is_keyed_for_tests(m);
    size_t before = w->growths;
    ccol_retval_t r = sc ? ah_put_u64_sc(m, gen(i)) : ah_put_u64(m, gen(i), i);
    if (r != ccol_success) w->ok = false;
    ah_watch_step(w, m, sc);
    if (was_keyed && w->growths != before) growths_while_keyed++;
  }
  ah_watch_end(w);
  if (w->attempts != growths_while_keyed) w->ok = false;
  *keyed = chashmap_is_keyed_for_tests(m);
  chashmap_reset_oa_probe_steps_for_tests();
  chashmap_reset_sc_node_visits_for_tests();
  for (uint64_t i = 0; i < keys; i++) {
    unsigned long long v = 0;
    if (sc ? !ah_has_u64(m, gen(i)) : (!ah_get_u64(m, gen(i), &v) || v != i)) {
      w->ok = false;
    }
  }
  *lookup = (double)(sc ? chashmap_sc_node_visits_for_tests()
                        : chashmap_oa_probe_steps_for_tests()) /
            (double)keys;
  if (!sc && !chashmap_oa_check_invariants_for_tests(m)) w->ok = false;
  chmap_destroy(m);
}

TEST(adaptive_hash, integer_floods_stay_keyed_across_growths_within_bounds) {
  static const ah_gen_t gens[] = {ah_inverse_fibonacci, ah_fibonacci_stride};
  enum { KEYS = 40000 };
  for (size_t g = 0; g < 2; g++) {
    for (int sc = 0; sc < 2; sc++) {
      ah_attempt_watch w;
      bool keyed = false;
      double lookup = 0;
      ah_flood_across_growths(gens[g], sc != 0, KEYS, &w, &keyed, &lookup);
      REQUIRE_TRUE(w.ok);
      REQUIRE_TRUE(w.bound_ok);
      REQUIRE_TRUE(keyed);
      REQUIRE_GE(w.attempts, 3ull);
      REQUIRE_EQ(w.failures, w.attempts);
      REQUIRE_LT((double)w.failed_work / KEYS, 5.0);
      REQUIRE_LT(lookup, sc ? 2.0 : 2.5);
    }
  }
}

/* The seed-independent multicollision of XXH64 keeps 256 keys on one hash
 * whatever the size, so every growth of the keyed map makes a fast attempt
 * that a chain of 20 ends within its bound, and the map ends keyed. This test
 * is non-vacuous: without the chain measure of the attempt the map ends fast
 * with a chain of 256. */
TEST(adaptive_hash, an_xxh64_multicollision_stays_keyed_across_growths) {
  enum { KEYS = 6561 };
  chmap m = chmap_create(16, ccol_other_types, ccol_int, NULL);
  REQUIRE_NE((void *)m, NULL);
  ah_attempt_watch w;
  ah_watch_start(&w, m);
  ah_xxh_key key;
  for (unsigned i = 0; i < KEYS; i++) {
    ah_xxh_multicollision_key(i, &key);
    int v = (int)i;
    cmap_pair kp = {&key, sizeof(key)};
    cmap_pair vp = {&v, sizeof(v)};
    if (chmap_insert_elem(m, &kp, &vp) != ccol_success) w.ok = false;
    ah_watch_step(&w, m, true);
  }
  ah_watch_end(&w);
  for (unsigned i = 0; i < KEYS; i++) {
    ah_xxh_multicollision_key(i, &key);
    cmap_pair kp = {&key, sizeof(key)};
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(m, &kp, &out) != ccol_success ||
        *(const int *)out->ptr != (int)i) {
      w.ok = false;
    }
  }
  bool keyed = chashmap_is_keyed_for_tests(m);
  size_t longest = chashmap_sc_longest_chain_for_tests(m);
  chmap_destroy(m);
  REQUIRE_TRUE(w.ok);
  REQUIRE_TRUE(w.bound_ok);
  REQUIRE_TRUE(keyed);
  REQUIRE_GE(w.attempts, 2ull);
  REQUIRE_EQ(w.failures, w.attempts);
  REQUIRE_LE(longest, (size_t)12);
}

/* Inserts random keys until the capacity of m doubles (open addressing) or
 * quadruples (separate chaining). */
static void ah_grow_with_random(chmap m, bool sc, uint64_t *next, bool *ok) {
  size_t cap = chmap_get_bucket_arr_size(m);
  while (*ok && chmap_get_bucket_arr_size(m) == cap) {
    unsigned long long k = ah_mix(0x5eed0000ULL + (*next)++);
    *ok = (sc ? ah_put_u64_sc(m, k) : ah_put_u64(m, k, 7)) == ccol_success;
  }
}

/* The open-addressing churn of a chosen displacement, across growths: at
 * each size the attack places a run of `run` keys with consecutive home
 * slots under the fast hash and then inserts and deletes one more key with
 * the home slot of the start of the run, until the map is keyed. Random keys
 * then grow the table, and the growth makes its fast attempt. Gives the
 * largest number of attack inserts that any round needed, the rounds that
 * started fast, and whether the map ended keyed. */
static void ah_oa_churn_across_growths(uint64_t run, unsigned rounds,
                                       size_t *worst, unsigned *fast_rounds,
                                       bool *keyed_end, ah_attempt_watch *w) {
  chmap m = chmap_create(8192, ccol_unsigned_long_long, ccol_unsigned_long_long,
                         NULL);
  bool ok = m != NULL;
  uint64_t next = 0;
  for (uint64_t i = 0; ok && i < 3000; i++) {
    ok = ah_put_u64(m, ah_mix(0x5eed0000ULL + next++), 7) == ccol_success;
  }
  ah_watch_start(w, m);
  *worst = 0;
  *fast_rounds = 0;
  for (unsigned round = 0; ok && round < rounds; round++) {
    if (round > 0) ah_grow_with_random(m, false, &next, &ok);
    ah_watch_step(w, m, false);
    if (chashmap_is_keyed_for_tests(m)) continue;
    (*fast_rounds)++;
    unsigned lg = ah_log2_of(chmap_get_bucket_arr_size(m));
    uint64_t home = 1000 + 3000 * (uint64_t)round;
    uint64_t tag = 1 + 100000 * (uint64_t)round;
    size_t inserts = 0;
    for (uint64_t i = 0; ok && i < run && !chashmap_is_keyed_for_tests(m);
         i++) {
      ok = ah_put_u64(m, ah_key_for_home(home + i, lg, tag + i), i) ==
           ccol_success;
      inserts++;
    }
    for (uint64_t r = 0; ok && r < 4000 && !chashmap_is_keyed_for_tests(m);
         r++) {
      unsigned long long k = ah_key_for_home(home, lg, tag + 50000 + r);
      ok = ah_put_u64(m, k, r) == ccol_success;
      inserts++;
      ok = ok && ah_del_u64(m, k) == ccol_success;
    }
    if (!chashmap_is_keyed_for_tests(m)) inserts = SIZE_MAX;
    if (inserts > *worst) *worst = inserts;
  }
  ah_watch_end(w);
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  *keyed_end = chashmap_is_keyed_for_tests(m);
  w->ok = w->ok && ok;
  chmap_destroy(m);
}

/* Churn at displacement 24, which only the window sees, and at displacement
 * 340, above the cap of 288: after every growth that brings the map back to
 * the fast hash, the resumed attack switches it again within its run plus
 * two windows, every failed attempt stays within its bound, and the map ends
 * keyed. This test is non-vacuous: without the fast attempt no round after
 * the first starts fast. */
TEST(adaptive_hash, churn_attacks_switch_again_after_every_fast_growth) {
  static const uint64_t runs[] = {24, 340};
  for (size_t t = 0; t < 2; t++) {
    ah_attempt_watch w;
    size_t worst = 0;
    unsigned fast_rounds = 0;
    bool keyed = false;
    ah_oa_churn_across_growths(runs[t], 5, &worst, &fast_rounds, &keyed, &w);
    REQUIRE_TRUE(w.ok);
    REQUIRE_TRUE(w.bound_ok);
    REQUIRE_EQ(w.growths, (size_t)4);
    REQUIRE_EQ(w.attempts, 4ull);
    REQUIRE_GE(fast_rounds, 4u);
    REQUIRE_LE(worst, (size_t)runs[t] + 512);
    REQUIRE_TRUE(keyed);
  }
}

/* The separate-chaining churn into a chain of 18, below the chain cap, across
 * growths: at each size a chain of 18 keys under the fast hash, then rounds
 * that insert and delete one more key of that bucket until the map is keyed;
 * random keys then grow the bucket array. After every growth that brings the
 * map back to the fast hash, the resumed attack switches it again within two
 * windows, and the map ends keyed. This test is non-vacuous: without the fast
 * attempt no round after the first starts fast. */
TEST(adaptive_hash, chain_churn_switches_again_after_every_fast_growth) {
  chmap m = ah_sc_with_chain(1000, 0);
  REQUIRE_NE((void *)m, NULL);
  ah_attempt_watch w;
  ah_watch_start(&w, m);
  bool ok = true;
  uint64_t next = 0;
  size_t worst = 0;
  unsigned fast_rounds = 0;
  for (unsigned round = 0; ok && round < 4; round++) {
    if (round > 0) ah_grow_with_random(m, true, &next, &ok);
    ah_watch_step(&w, m, true);
    if (chashmap_is_keyed_for_tests(m)) continue;
    fast_rounds++;
    unsigned lg = ah_log2_of(chmap_get_bucket_arr_size(m));
    uint64_t bucket = 500 + 1000 * (uint64_t)round;
    uint64_t tag = 100 + 100000 * (uint64_t)round;
    for (uint64_t i = 0; ok && i < 18; i++) {
      ok = ah_put_u64_sc(m, ah_key_for_home(bucket, lg, tag + i)) ==
           ccol_success;
    }
    size_t rounds = 0;
    for (uint64_t r = 0; ok && r < 4000 && !chashmap_is_keyed_for_tests(m);
         r++) {
      unsigned long long k = ah_key_for_home(bucket, lg, tag + 50000 + r);
      ok = ah_put_u64_sc(m, k) == ccol_success &&
           ah_del_u64(m, k) == ccol_success;
      rounds++;
    }
    if (!chashmap_is_keyed_for_tests(m)) rounds = SIZE_MAX;
    if (rounds > worst) worst = rounds;
  }
  ah_watch_end(&w);
  bool keyed = chashmap_is_keyed_for_tests(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(w.bound_ok);
  REQUIRE_EQ(w.growths, (size_t)3);
  REQUIRE_EQ(w.attempts, 3ull);
  REQUIRE_EQ(fast_rounds, 4u);
  REQUIRE_LE(worst, (size_t)512);
  REQUIRE_TRUE(keyed);
}

/* The fast attempt of a separate-chaining growth relinks in place, whether
 * it fills the bucket array or fails: every entry, its value, the address of
 * its value and of its key accessor, the oldest-first order, the
 * newest-first order of an iterator that is part way through, and deletes
 * all survive. Keys are 128-byte structs: random ones grow a map that the
 * test switched, and the attempt succeeds; then the multicollision family
 * grows it again, switches it, and the next attempt fails. This test is
 * non-vacuous: an attempt that leaves a node out of the buckets, or that
 * keeps the fast hash of a node after it fails, fails the lookups. */
TEST(adaptive_hash, a_fast_attempt_keeps_every_separate_chaining_entry) {
  enum { HONEST = 3000, FLOOD = 3300 };
  chmap m = chmap_create(16, ccol_other_types, ccol_int, NULL);
  REQUIRE_NE((void *)m, NULL);
  static ah_xxh_key keys[HONEST + FLOOD];
  static const cmap_pair *val_refs[HONEST + FLOOD];
  static const cmap_pair *key_refs[HONEST + FLOOD];
  bool ok = true;
  for (unsigned i = 0; i < HONEST + FLOOD; i++) {
    if (i < HONEST) {
      for (int w = 0; w < 16; w++) {
        keys[i].w[w] = ah_mix(i * 16u + (unsigned)w);
      }
    } else {
      ah_xxh_multicollision_key(i - HONEST, &keys[i]);
    }
  }
  /* 600 honest keys, a switch, then the rest of the honest keys grow it. */
  unsigned long long attempts0 = chashmap_retries_for_tests();
  unsigned long long failures0 = chashmap_retry_failures_for_tests();
  bool fast_after_grow = false, keyed_after_flood = false;
  chmap_redeclare(m, ah_xxh_key, int);
  ccol_iter_declare(m, it);
  it = NULL;
  for (unsigned i = 0; i < HONEST + FLOOD; i++) {
    if (i == 600) {
      if (chashmap_switch_to_keyed_for_tests(m) != ccol_success) ok = false;
      /* An iterator that has visited the newest half. */
      it = ccol_begin(m);
      for (unsigned n = 0; it && n < 300; n++) it = ccol_iter_next(it);
    }
    int v = (int)i;
    cmap_pair kp = {&keys[i], sizeof(keys[i])};
    cmap_pair vp = {&v, sizeof(v)};
    if (ccol_chmap_insert_or_get_entry(m, &kp, &vp, &key_refs[i],
                                       &val_refs[i]) != ccol_success) {
      ok = false;
    }
    if (i == HONEST - 1) fast_after_grow = !chashmap_is_keyed_for_tests(m);
  }
  keyed_after_flood = chashmap_is_keyed_for_tests(m);
  unsigned long long attempts = chashmap_retries_for_tests() - attempts0;
  unsigned long long failures = chashmap_retry_failures_for_tests() - failures0;
  /* The iterator goes on with the older 300, newest first. */
  unsigned expect = 300;
  while (it) {
    expect--;
    if (memcmp(it->key_pair->ptr, &keys[expect], sizeof(keys[0])) != 0) {
      ok = false;
    }
    it = ccol_iter_next(it);
  }
  if (expect != 0) ok = false;
  for (unsigned i = 0; i < HONEST + FLOOD; i++) {
    cmap_pair kp = {&keys[i], sizeof(keys[i])};
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(m, &kp, &out) != ccol_success ||
        out != val_refs[i] || *(const int *)out->ptr != (int)i) {
      ok = false;
    }
    const cmap_pair *ks = NULL, *vs = NULL;
    int unused = -1;
    cmap_pair up = {&unused, sizeof(unused)};
    if (ccol_chmap_insert_or_get_entry(m, &kp, &up, &ks, &vs) !=
            ccol_key_already_present ||
        ks != key_refs[i] || vs != val_refs[i]) {
      ok = false;
    }
  }
  unsigned pos = 0;
  for (const ccol_chmap_entry_ref *e = ccol_chmap_oldest_entry(m); e;) {
    const cmap_pair *k = NULL, *v = NULL;
    e = ccol_chmap_entry_read(e, &k, &v);
    if (pos >= HONEST + FLOOD ||
        memcmp(k->ptr, &keys[pos], sizeof(keys[0])) != 0 ||
        *(const int *)v->ptr != (int)pos) {
      ok = false;
    }
    pos++;
  }
  for (unsigned i = 0; i < HONEST + FLOOD; i += 2) {
    cmap_pair kp = {&keys[i], sizeof(keys[0])};
    if (chmap_delete_elem(m, &kp) != ccol_success) ok = false;
  }
  for (unsigned i = 1; i < HONEST + FLOOD; i += 2) {
    cmap_pair kp = {&keys[i], sizeof(keys[0])};
    const cmap_pair *out = NULL;
    if (chmap_get_elem_ref(m, &kp, &out) != ccol_success) ok = false;
  }
  size_t left = chmap_elem_count(m);
  chmap_destroy(m);
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(fast_after_grow);
  REQUIRE_TRUE(keyed_after_flood);
  REQUIRE_GE(attempts, 2ull);
  REQUIRE_GE(failures, 1ull);
  REQUIRE_LT(failures, attempts);
  REQUIRE_EQ(pos, (unsigned)(HONEST + FLOOD));
  REQUIRE_EQ(left, (size_t)(HONEST + FLOOD) / 2);
}

/* The open-addressing counterpart: a switched table grows with random keys
 * and its fast attempt fills the new table; the inverse-Fibonacci flood then
 * switches it again and the next attempt fails. After each growth every
 * entry and value is there, an iteration visits each key once, the
 * invariants hold (every key within probe_span of its home slot), and
 * deletes work. This test is non-vacuous: an attempt that fails and leaves
 * the fast hash in a table marked keyed fails the lookups. */
TEST(adaptive_hash, a_fast_attempt_keeps_every_open_addressing_entry) {
  enum { HONEST = 5000, FLOOD = 12000 };
  chmap m =
      chmap_create(16, ccol_unsigned_long_long, ccol_unsigned_long_long, NULL);
  REQUIRE_NE((void *)m, NULL);
  bool ok = true;
  unsigned long long attempts0 = chashmap_retries_for_tests();
  unsigned long long failures0 = chashmap_retry_failures_for_tests();
  for (uint64_t i = 0; i < 1000; i++) {
    if (ah_put_u64(m, ah_mix(i), i) != ccol_success) ok = false;
  }
  if (chashmap_switch_to_keyed_for_tests(m) != ccol_success) ok = false;
  for (uint64_t i = 1000; i < HONEST; i++) {
    if (ah_put_u64(m, ah_mix(i), i) != ccol_success) ok = false;
  }
  bool fast_after_grow = !chashmap_is_keyed_for_tests(m);
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  for (uint64_t i = 1; i <= FLOOD; i++) {
    if (ah_put_u64(m, ah_inverse_fibonacci(i), HONEST + i) != ccol_success) {
      ok = false;
    }
  }
  bool keyed_after_flood = chashmap_is_keyed_for_tests(m);
  unsigned long long attempts = chashmap_retries_for_tests() - attempts0;
  unsigned long long failures = chashmap_retry_failures_for_tests() - failures0;
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  unsigned long long sum = 0;
  size_t visited = 0;
  chmap_redeclare(m, unsigned long long, unsigned long long);
  ccol_iter_declare(m, it);
  for (it = ccol_begin(m); it; it = ccol_iter_next(it)) {
    sum += *(const unsigned long long *)it->val_pair->ptr;
    visited++;
  }
  for (uint64_t i = 0; i < HONEST; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, ah_mix(i), &v) || v != i) ok = false;
    if (ah_del_u64(m, ah_mix(i)) != ccol_success) ok = false;
  }
  for (uint64_t i = 1; i <= FLOOD; i++) {
    unsigned long long v = 0;
    if (!ah_get_u64(m, ah_inverse_fibonacci(i), &v) || v != HONEST + i) {
      ok = false;
    }
  }
  size_t left = chmap_elem_count(m);
  if (!chashmap_oa_check_invariants_for_tests(m)) ok = false;
  chmap_destroy(m);
  unsigned long long expect = 0;
  for (uint64_t i = 0; i < HONEST + FLOOD + 1; i++) expect += i;
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(fast_after_grow);
  REQUIRE_TRUE(keyed_after_flood);
  REQUIRE_GE(failures, 1ull);
  REQUIRE_LT(failures, attempts);
  REQUIRE_EQ(visited, (size_t)(HONEST + FLOOD));
  REQUIRE_EQ(sum, expect - HONEST);
  REQUIRE_EQ(left, (size_t)FLOOD);
}

/* A keyed map grows while each of its allocations fails in turn: the map
 * switches through the test hook at its eighth key, and inserts then grow it
 * several times. A growth that cannot allocate leaves the map keyed at its
 * size, and the insert that asked for it still succeeds or reports the
 * failure; the next insert asks again. After every insert, every key that an
 * insert reported as inserted is there with its value, and the map ends grown
 * and fast. This test is non-vacuous: an attempt that marks the map fast
 * before its block exists loses the keys until the next growth, which the
 * check after every insert sees. */
enum { AH_RETRY_ALLOC_KEYS = 120 };

static bool ah_retry_alloc_keys_intact(chmap m, bool sc, const bool *inserted,
                                       uint64_t upto) {
  for (uint64_t i = 0; i < upto; i++) {
    unsigned long long k = ah_mix(i);
    cmap_pair kp = {&k, sizeof(k)};
    const cmap_pair *out = NULL;
    bool present = chmap_get_elem_ref(m, &kp, &out) == ccol_success;
    if (present != inserted[i]) return false;
    if (!present) continue;
    char v[24];
    snprintf(v, sizeof(v), "v%llu", (unsigned long long)i);
    if (sc ? strcmp((const char *)out->ptr, v) != 0
           : *(const unsigned long long *)out->ptr != i) {
      return false;
    }
  }
  return true;
}

static void ah_retry_alloc_scenario(bool sc, long fail_at, bool *created,
                                    bool *fast, bool *intact,
                                    long *allocations) {
  ccol_memmgmt_procs_t mp = {.malloc = ah_malloc,
                             .calloc = ah_calloc,
                             .realloc = ah_realloc,
                             .free = free};
  g_ah_alloc_index = 0;
  g_ah_fail_at = fail_at;
  chmap m = chmap_create_full(16, ccol_unsigned_long_long,
                              sc ? ccol_string : ccol_unsigned_long_long, &mp,
                              NULL, NULL, NULL);
  *created = m != NULL;
  *fast = false;
  *intact = true;
  if (m) {
    bool inserted[AH_RETRY_ALLOC_KEYS] = {false};
    for (uint64_t i = 0; i < AH_RETRY_ALLOC_KEYS; i++) {
      if (i == 8) {
        /* A switch that cannot allocate leaves the map fast; ask again. */
        while (chashmap_switch_to_keyed_for_tests(m) != ccol_success) {
        }
      }
      unsigned long long k = ah_mix(i);
      if (sc) {
        char v[24];
        int len = snprintf(v, sizeof(v), "v%llu", (unsigned long long)i);
        cmap_pair kp = {&k, sizeof(k)};
        cmap_pair vp = {v, (size_t)len + 1};
        inserted[i] = chmap_insert_elem(m, &kp, &vp) == ccol_success;
      } else {
        inserted[i] = ah_put_u64(m, k, i) == ccol_success;
      }
      if (!ah_retry_alloc_keys_intact(m, sc, inserted, i + 1)) {
        *intact = false;
      }
    }
    if (!sc && !chashmap_oa_check_invariants_for_tests(m)) *intact = false;
    if (chmap_get_bucket_arr_size(m) < 256u) *intact = false;
    *fast = !chashmap_is_keyed_for_tests(m);
    chmap_destroy(m);
  }
  *allocations = g_ah_alloc_index;
  g_ah_fail_at = -1;
}

TEST(adaptive_hash, a_keyed_growth_that_cannot_allocate_keeps_the_map_correct) {
  for (int sc = 0; sc < 2; sc++) {
    bool created = false, fast = false, intact = false;
    long allocations = 0;
    ah_retry_alloc_scenario(sc != 0, -1, &created, &fast, &intact,
                            &allocations);
    REQUIRE_TRUE(created);
    REQUIRE_TRUE(fast);
    REQUIRE_TRUE(intact);
    int broken = 0, keyed_end = 0, runs = 0;
    for (long f = 0; f < allocations; f++) {
      long used = 0;
      ah_retry_alloc_scenario(sc != 0, f, &created, &fast, &intact, &used);
      if (!created) continue;
      runs++;
      if (!intact) broken++;
      if (!fast) keyed_end++;
    }
    REQUIRE_GT(runs, 0);
    REQUIRE_EQ(broken, 0);
    REQUIRE_EQ(keyed_end, 0);
  }
}
