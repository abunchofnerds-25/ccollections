/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <chashmap.h>
#include <stdlib.h>
#include <string.h>

const size_t minimum_allowed_bucket_array_size = 16;
const size_t scale_factor = 4;
const size_t minimum_scale_down_threshold =
    scale_factor * (minimum_allowed_bucket_array_size);

#define OPEN_ADDR_MAX_LOAD_FACTOR 0.70
#define OPEN_ADDR_MIN_LOAD_FACTOR 0.25
#define INLINE_STORAGE_THRESHOLD 23  // SSO: Small String Optimization threshold

// Fibonacci hashing for integers - architecture dependent
#if SIZE_MAX == UINT64_MAX  // 64-bit architecture
#define FIBONACCI_HASH_MULTIPLIER 11400714819323198485ULL
#elif SIZE_MAX == UINT32_MAX  // 32-bit architecture
#define FIBONACCI_HASH_MULTIPLIER 2654435769U
#else
#error "Unsupported architecture: SIZE_MAX is neither UINT32_MAX nor UINT64_MAX"
#endif

/* ========================================================================== */
/*                    OPEN ADDRESSING STRUCTURES                              */
/* ========================================================================== */

// 24-byte slot (8-byte key + 8-byte value + 1-byte metadata + 7 bytes natural
// padding). Natural alignment keeps key_data and val_data at 8-byte boundaries
// across all array indices, which is required for correctness on strict-
// alignment architectures and allows chmap_get_ptr to return an aligned pointer
// directly into the slot array for in-place modification.
typedef struct {
  uint64_t key_data;
  uint64_t val_data;
  uint8_t metadata;  // bit 0: occupied, bit 1: deleted
} oa_slot;

#define SLOT_OCCUPIED 0x01
#define SLOT_DELETED 0x02

typedef struct {
  oa_slot* slots;
  size_t capacity;
  size_t count;
  size_t key_size;
  size_t val_size;
  size_t deleted_count;
  ccol_memmgmt_procs_t* m_procs;
  ccol_hashing_proc_t custom_hashing_proc;
  ccol_data_type key_type;
  ccol_data_type val_type;
  cmap_pair get_accessor;
  bool val_size_initialized;
} open_addr_map;

/* ========================================================================== */
/*                 SEPARATE CHAINING STRUCTURES                               */
/* ========================================================================== */

typedef struct chmap_entry {
  size_t hash_val;
  union {
    void* ptr;
    // SSO: 23 bytes + null terminator
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } key_storage;
  size_t key_size;
  bool key_is_inline;
  union {
    void* ptr;
    // SSO: 23 bytes + null terminator
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } val_storage;
  size_t val_size;
  bool val_is_inline;
  ccol_memmgmt_procs_t* m_procs;
} __attribute__((packed)) chmap_entry;

typedef struct dllist_ref_node {
  struct dllist_ref_node* prev;
  struct dllist_ref_node* next;
} __attribute__((packed)) dllist_ref_node;

typedef struct llist_node {
  struct llist_node* next;
  dllist_ref_node dllist_refs;
  chmap_entry data;
  cmap_pair key_pair_accessor;
  cmap_pair val_pair_accessor;
  ccol_memmgmt_procs_t* m_procs;
} llist_node;

typedef struct sep_chain_map {
  size_t elem_count;
  size_t bucket_arr_size;
  size_t elem_count_to_scale_up;
  size_t elem_count_to_scale_down;
  llist_node** bucket_arr;
  dllist_ref_node* head_of_all_elems;
  ccol_memmgmt_procs_t* m_procs;
  ccol_hashing_proc_t custom_hashing_proc;
  ccol_data_type key_type;
  ccol_data_type val_type;
} sep_chain_map;

/* ========================================================================== */
/*                      UNIFIED STRUCTURE                                     */
/* ========================================================================== */

typedef enum { IMPL_OPEN_ADDRESSING, IMPL_SEPARATE_CHAINING } hashmap_impl_type;

struct chashmap {
  hashmap_impl_type impl_type;
  union {
    open_addr_map* oa_map;
    sep_chain_map* sc_map;
  } impl;
};

/* ========================================================================== */
/*                      TYPE DETECTION                                        */
/* ========================================================================== */

/* Returns true for all ccol_data_type values that fit in ≤ 8 bytes and can be
 * hashed with Fibonacci hashing (integers, floats, and pointers). Used to
 * decide which map backend to instantiate at creation time. */
static inline bool is_type_integral(ccol_data_type type) {
  switch (type) {
    case ccol_char:
    case ccol_short:
    case ccol_int:
    case ccol_long:
    case ccol_long_long:
    case ccol_unsigned_char:
    case ccol_unsigned_short:
    case ccol_unsigned_int:
    case ccol_unsigned_long:
    case ccol_unsigned_long_long:
    case ccol_float:
    case ccol_double:
    case ccol_pointer:
      return true;
    default:
      return false;
  }
}

/* Returns the byte size of the corresponding C type for a ccol_data_type enum
 * value. Defaults to 8 for unknown types so open-addressing slots are always
 * large enough to store a pointer-sized value. */
static inline size_t get_type_size(ccol_data_type type) {
  switch (type) {
    case ccol_char:
    case ccol_unsigned_char:
      return sizeof(char);
    case ccol_short:
    case ccol_unsigned_short:
      return sizeof(short);
    case ccol_int:
    case ccol_unsigned_int:
      return sizeof(int);
    case ccol_long:
    case ccol_unsigned_long:
      return sizeof(long);
    case ccol_long_long:
    case ccol_unsigned_long_long:
      return sizeof(long long);
    case ccol_pointer:
      return sizeof(uintptr_t);
    case ccol_float:
      return sizeof(float);
    case ccol_double:
      return sizeof(double);
    case ccol_long_double:
      return sizeof(long double);
    default:
      return 8;
  }
}

/* Returns true when both key and value types are integral and ≤ 8 bytes, which
 * is the condition under which the compact open-addressing backend is selected.
 * Otherwise the separate-chaining backend is used to handle arbitrary-size keys
 * and values including strings and user-defined structs. */
static inline bool should_use_open_addressing(ccol_data_type key_type,
                                              ccol_data_type val_type) {
  return is_type_integral(key_type) && is_type_integral(val_type) &&
         get_type_size(key_type) <= 8 && get_type_size(val_type) <= 8;
}

/* ========================================================================== */
/*                         HASH FUNCTIONS                                     */
/* ========================================================================== */

// XXHash constants adapted for both 32-bit and 64-bit
#if SIZE_MAX == UINT64_MAX  // 64-bit architecture

#define XXH_PRIME_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME_3 0x165667B19E3779F9ULL
#define XXH_PRIME_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME_5 0x27D4EB2F165667C5ULL

/* Rotate-left helper for the XXHash mixing step. */
static inline size_t xxh_rotl(size_t x, int r) {
  return (x << r) | (x >> (64 - r));
}

/* Accumulates one 8-byte block into the XXHash running state acc. */
static inline size_t xxh_round(size_t acc, size_t input) {
  acc += input * XXH_PRIME_2;
  acc = xxh_rotl(acc, 31);
  acc *= XXH_PRIME_1;
  return acc;
}

/* Finalisation mix that ensures every bit of input affects every bit of the
 * output hash (avalanche effect). Applied once at the end of xxhash64_buffer.
 */
static inline size_t xxh_avalanche(size_t hash) {
  hash ^= hash >> 33;
  hash *= XXH_PRIME_2;
  hash ^= hash >> 29;
  hash *= XXH_PRIME_3;
  hash ^= hash >> 32;
  return hash;
}

/* Hashes an arbitrary-length byte buffer using the XXHash64 algorithm (adapted
 * for both 32-bit and 64-bit architectures). The seed parameter allows
 * different hash domains. Used for non-integral key types in the
 * separate-chaining backend. */
static inline size_t xxhash64_buffer(const void* input, size_t len,
                                     size_t seed) {
  const uint8_t* p = (const uint8_t*)input;
  const uint8_t* const end = p + len;
  size_t hash;

  if (len >= 32) {
    const uint8_t* const limit = end - 32;
    size_t v1 = seed + XXH_PRIME_1 + XXH_PRIME_2;
    size_t v2 = seed + XXH_PRIME_2;
    size_t v3 = seed + 0;
    size_t v4 = seed - XXH_PRIME_1;

    do {
      uint64_t w;
      memcpy(&w, p, sizeof(w));
      v1 = xxh_round(v1, w);
      p += 8;
      memcpy(&w, p, sizeof(w));
      v2 = xxh_round(v2, w);
      p += 8;
      memcpy(&w, p, sizeof(w));
      v3 = xxh_round(v3, w);
      p += 8;
      memcpy(&w, p, sizeof(w));
      v4 = xxh_round(v4, w);
      p += 8;
    } while (p <= limit);

    hash =
        xxh_rotl(v1, 1) + xxh_rotl(v2, 7) + xxh_rotl(v3, 12) + xxh_rotl(v4, 18);
    hash ^= xxh_round(0, v1);
    hash = hash * XXH_PRIME_1 + XXH_PRIME_4;
    hash ^= xxh_round(0, v2);
    hash = hash * XXH_PRIME_1 + XXH_PRIME_4;
    hash ^= xxh_round(0, v3);
    hash = hash * XXH_PRIME_1 + XXH_PRIME_4;
    hash ^= xxh_round(0, v4);
    hash = hash * XXH_PRIME_1 + XXH_PRIME_4;
  } else {
    hash = seed + XXH_PRIME_5;
  }

  hash += len;

  while (p + 8 <= end) {
    uint64_t w;
    memcpy(&w, p, sizeof(w));
    size_t k1 = xxh_round(0, w);
    hash ^= k1;
    hash = xxh_rotl(hash, 27) * XXH_PRIME_1 + XXH_PRIME_4;
    p += 8;
  }

  if (p + 4 <= end) {
    uint32_t w;
    memcpy(&w, p, sizeof(w));
    hash ^= (size_t)w * XXH_PRIME_1;
    hash = xxh_rotl(hash, 23) * XXH_PRIME_2 + XXH_PRIME_3;
    p += 4;
  }

  while (p < end) {
    hash ^= (*p) * XXH_PRIME_5;
    hash = xxh_rotl(hash, 11) * XXH_PRIME_1;
    p++;
  }

  return xxh_avalanche(hash);
}

#else  // 32-bit architecture

#define XXH_PRIME_1 0x9E3779B1U
#define XXH_PRIME_2 0x85EBCA77U
#define XXH_PRIME_3 0xC2B2AE3DU
#define XXH_PRIME_4 0x27D4EB2FU
#define XXH_PRIME_5 0x165667B1U

/* 32-bit rotate-left helper for the XXHash mixing step. */
static inline size_t xxh_rotl(size_t x, int r) {
  return (x << r) | (x >> (32 - r));
}

/* 32-bit accumulation round for XXHash state. */
static inline size_t xxh_round(size_t acc, size_t input) {
  acc += input * XXH_PRIME_2;
  acc = xxh_rotl(acc, 13);
  acc *= XXH_PRIME_1;
  return acc;
}

/* 32-bit avalanche finaliser for XXHash. */
static inline size_t xxh_avalanche(size_t hash) {
  hash ^= hash >> 15;
  hash *= XXH_PRIME_2;
  hash ^= hash >> 13;
  hash *= XXH_PRIME_3;
  hash ^= hash >> 16;
  return hash;
}

/* 32-bit variant of xxhash64_buffer. Processes data in 4-byte chunks instead
 * of 8-byte chunks to match the narrower register width. */
static inline size_t xxhash64_buffer(const void* input, size_t len,
                                     size_t seed) {
  const uint8_t* p = (const uint8_t*)input;
  const uint8_t* const end = p + len;
  size_t hash;

  if (len >= 16) {
    const uint8_t* const limit = end - 16;
    size_t v1 = seed + XXH_PRIME_1 + XXH_PRIME_2;
    size_t v2 = seed + XXH_PRIME_2;
    size_t v3 = seed + 0;
    size_t v4 = seed - XXH_PRIME_1;

    do {
      uint32_t w;
      memcpy(&w, p, sizeof(w));
      v1 = xxh_round(v1, w);
      p += 4;
      memcpy(&w, p, sizeof(w));
      v2 = xxh_round(v2, w);
      p += 4;
      memcpy(&w, p, sizeof(w));
      v3 = xxh_round(v3, w);
      p += 4;
      memcpy(&w, p, sizeof(w));
      v4 = xxh_round(v4, w);
      p += 4;
    } while (p <= limit);

    hash =
        xxh_rotl(v1, 1) + xxh_rotl(v2, 7) + xxh_rotl(v3, 12) + xxh_rotl(v4, 18);
  } else {
    hash = seed + XXH_PRIME_5;
  }

  hash += len;

  while (p + 4 <= end) {
    uint32_t w;
    memcpy(&w, p, sizeof(w));
    hash += w * XXH_PRIME_3;
    hash = xxh_rotl(hash, 17) * XXH_PRIME_4;
    p += 4;
  }

  while (p < end) {
    hash += (*p) * XXH_PRIME_5;
    hash = xxh_rotl(hash, 11) * XXH_PRIME_1;
    p++;
  }

  return xxh_avalanche(hash);
}

#endif

/* Fibonacci hashing: multiplies key by the golden-ratio-derived constant to
 * achieve excellent bit distribution with a single multiply. The constant is
 * architecture-specific (64-bit or 32-bit) to match the native word size. */
static inline size_t hash_int_fast(size_t key) {
  return key * FIBONACCI_HASH_MULTIPLIER;
}

/* Dispatches to the appropriate hash function based on key type. Integral and
 * float types use Fibonacci hashing on their bit pattern; everything else
 * (strings, structs, pointer-to-data) uses xxhash64_buffer. A custom hashing
 * proc overrides all built-in strategies when provided. */
static inline size_t hash_key_data(const void* key_ptr, size_t key_size,
                                   ccol_data_type key_type,
                                   ccol_hashing_proc_t custom_proc) {
  if (custom_proc) {
    return custom_proc(key_ptr);
  }

  // Use fast Fibonacci hashing for all integral types
  switch (key_type) {
    case ccol_char:
    case ccol_unsigned_char:
      return hash_int_fast((size_t)*(uint8_t*)key_ptr);
    case ccol_short:
    case ccol_unsigned_short:
      return hash_int_fast((size_t)*(uint16_t*)key_ptr);
    case ccol_int:
    case ccol_unsigned_int:
      return hash_int_fast((size_t)*(uint32_t*)key_ptr);
    case ccol_long:
    case ccol_unsigned_long:
#if SIZE_MAX == UINT64_MAX
      return hash_int_fast((size_t)*(uint64_t*)key_ptr);
#else
      return hash_int_fast((size_t)*(uint32_t*)key_ptr);
#endif
    case ccol_long_long:
    case ccol_unsigned_long_long: {
#if SIZE_MAX == UINT64_MAX
      return hash_int_fast((size_t)*(uint64_t*)key_ptr);
#else
      // On 32-bit, hash the 64-bit value by combining high and low parts
      uint64_t val = *(uint64_t*)key_ptr;
      uint32_t low = (uint32_t)val;
      uint32_t high = (uint32_t)(val >> 32);
      return hash_int_fast((size_t)(low ^ high));
#endif
    }
    case ccol_float: {
      uint32_t bits;
      mem_cpy(&bits, key_ptr, 4);
      return hash_int_fast((size_t)bits);
    }
    case ccol_double: {
#if SIZE_MAX == UINT64_MAX
      uint64_t bits;
      mem_cpy(&bits, key_ptr, 8);
      return hash_int_fast((size_t)bits);
#else
      // On 32-bit, hash the 64-bit double by combining parts
      uint64_t bits;
      mem_cpy(&bits, key_ptr, 8);
      uint32_t low = (uint32_t)bits;
      uint32_t high = (uint32_t)(bits >> 32);
      return hash_int_fast((size_t)(low ^ high));
#endif
    }
    case ccol_pointer: {
      uintptr_t bits;
      mem_cpy(&bits, key_ptr, sizeof(uintptr_t));
      return hash_int_fast((size_t)bits);
    }
    default:
      return xxhash64_buffer(key_ptr, key_size, 0);
  }
}

/* ========================================================================== */
/*                   OPEN ADDRESSING IMPLEMENTATION                           */
/* ========================================================================== */

/* Compares the key stored in a slot against key_ptr byte-for-byte. The key is
 * stored inline in slot->key_data (up to 8 bytes), so memcmp directly against
 * that field works for all supported integral key sizes. */
static inline bool oa_keys_equal(const oa_slot* slot, const void* key_ptr,
                                 size_t key_size) {
  return memcmp(&slot->key_data, key_ptr, key_size) == 0;
}

/* Allocates and initialises the open-addressing map struct and its slot array.
 * All slots are zeroed via calloc so their metadata bytes start as 0
 * (neither SLOT_OCCUPIED nor SLOT_DELETED), which is the empty sentinel. */
static open_addr_map* oa_create(size_t capacity, ccol_data_type key_type,
                                ccol_data_type val_type, size_t key_size,
                                size_t val_size, ccol_memmgmt_procs_t* m_procs,
                                ccol_hashing_proc_t custom_hashing_proc) {
  open_addr_map* map =
      (open_addr_map*)_mem_alloc(m_procs, sizeof(open_addr_map));
  if (!map) return NULL;

  map->slots = (oa_slot*)_mem_calloc(m_procs, capacity, sizeof(oa_slot));
  if (!map->slots) {
    _mem_free(m_procs, map);
    return NULL;
  }

  map->capacity = capacity;
  map->count = 0;
  map->deleted_count = 0;
  map->key_size = key_size;
  map->val_size = val_size;
  map->val_size_initialized = false;
  map->m_procs = m_procs;
  map->custom_hashing_proc = custom_hashing_proc;
  map->key_type = key_type;
  map->val_type = val_type;

  return map;
}

/* Resizes the slot array to new_capacity and reinserts all live entries.
 * Deleted slots are not carried over so the deleted_count resets to zero,
 * which reduces probing length after many deletions. The hash is recomputed
 * for each entry because the slot array does not store hash values. */
static void oa_rehash(open_addr_map* map, size_t new_capacity) {
  oa_slot* old_slots = map->slots;
  size_t old_capacity = map->capacity;

  map->slots =
      (oa_slot*)_mem_calloc(map->m_procs, new_capacity, sizeof(oa_slot));
  if (!map->slots) {
    map->slots = old_slots;
    return;
  }

  map->capacity = new_capacity;
  map->count = 0;
  map->deleted_count = 0;

  // Rehash all existing entries - recalculate hash since we don't store it
  for (size_t i = 0; i < old_capacity; i++) {
    if ((old_slots[i].metadata & SLOT_OCCUPIED) &&
        !(old_slots[i].metadata & SLOT_DELETED)) {
      size_t hash_val = hash_key_data(&old_slots[i].key_data, map->key_size,
                                      map->key_type, map->custom_hashing_proc);
      size_t index = hash_val % new_capacity;

      // Prefetch likely next location
      __builtin_prefetch(&map->slots[(index + 1) % new_capacity], 1, 1);

      while (map->slots[index].metadata & SLOT_OCCUPIED) {
        index = (index + 1) % new_capacity;
        __builtin_prefetch(&map->slots[(index + 1) % new_capacity], 1, 1);
      }

      map->slots[index] = old_slots[i];
      map->slots[index].metadata = SLOT_OCCUPIED;  // Clear deleted flag
      map->count++;
    }
  }

  _mem_free(map->m_procs, old_slots);
}

/* Inserts or updates a key-value pair using linear probing. The load factor
 * (count + deleted) / capacity is checked before insertion; exceeding
 * OPEN_ADDR_MAX_LOAD_FACTOR triggers a 2× rehash. Deleted slots encountered
 * during probing are reused so they don't accumulate without bound. */
static ccol_retval_t oa_insert(open_addr_map* map, const cmap_pair* key_pair,
                               const cmap_pair* val_pair) {
  double load_factor =
      (double)(map->count + map->deleted_count) / map->capacity;
  if (load_factor > OPEN_ADDR_MAX_LOAD_FACTOR) {
    if (map->capacity < max_power_of_two_size_t) {
      oa_rehash(map, map->capacity * 2);
    }
  }

  // Set val_size on first insert to match actual value size
  if (!map->val_size_initialized && val_pair->size <= 8) {
    map->val_size = val_pair->size;
    map->val_size_initialized = true;
  }

  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val % map->capacity;
  size_t start_index = index;
  size_t first_deleted = map->capacity;

  // Prefetch first location
  __builtin_prefetch(&map->slots[index], 1, 1);

  do {
    // Prefetch next likely location
    __builtin_prefetch(&map->slots[(index + 1) % map->capacity], 1, 1);

    if (!(map->slots[index].metadata & SLOT_OCCUPIED)) {
      if (first_deleted < map->capacity) {
        index = first_deleted;
        map->deleted_count--;
      }

      mem_zero(&map->slots[index].key_data, sizeof(uint64_t));
      mem_zero(&map->slots[index].val_data, sizeof(uint64_t));
      mem_cpy(&map->slots[index].key_data, key_pair->ptr, key_pair->size);
      mem_cpy(&map->slots[index].val_data, val_pair->ptr, val_pair->size);
      map->slots[index].metadata = SLOT_OCCUPIED;
      map->count++;
      return ccol_success;
    }

    if ((map->slots[index].metadata & SLOT_DELETED) &&
        first_deleted == map->capacity) {
      first_deleted = index;
    }

    if ((map->slots[index].metadata & SLOT_OCCUPIED) &&
        !(map->slots[index].metadata & SLOT_DELETED) &&
        oa_keys_equal(&map->slots[index], key_pair->ptr, key_pair->size)) {
      mem_cpy(&map->slots[index].val_data, val_pair->ptr, val_pair->size);
      return ccol_key_already_present;
    }

    index = (index + 1) % map->capacity;
  } while (index != start_index);

  if (first_deleted < map->capacity) {
    map->deleted_count--;
    mem_zero(&map->slots[first_deleted].key_data, sizeof(uint64_t));
    mem_zero(&map->slots[first_deleted].val_data, sizeof(uint64_t));
    mem_cpy(&map->slots[first_deleted].key_data, key_pair->ptr, key_pair->size);
    mem_cpy(&map->slots[first_deleted].val_data, val_pair->ptr, val_pair->size);
    map->slots[first_deleted].metadata = SLOT_OCCUPIED;
    map->count++;
    return ccol_success;
  }
  return ccol_container_full;
}

/* Looks up key_pair using linear probing. An empty slot (no OCCUPIED or DELETED
 * bit) terminates the search immediately – this is safe because insertions
 * never leave a gap between a key and its probe chain. Returns a pointer to a
 * file-static cmap_pair holding the slot's value address; callers must not
 * store this pointer across any mutating operation. */
static ccol_retval_t oa_get(open_addr_map* map, const cmap_pair* key_pair,
                            cmap_pair** val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val % map->capacity;
  size_t start_index = index;

  // Prefetch first location
  __builtin_prefetch(&map->slots[index], 0, 1);

  do {
    // Prefetch next likely location
    __builtin_prefetch(&map->slots[(index + 1) % map->capacity], 0, 1);

    if (!(map->slots[index].metadata & SLOT_OCCUPIED) &&
        !(map->slots[index].metadata & SLOT_DELETED)) {
      return ccol_key_not_found;
    }

    if ((map->slots[index].metadata & SLOT_OCCUPIED) &&
        !(map->slots[index].metadata & SLOT_DELETED) &&
        oa_keys_equal(&map->slots[index], key_pair->ptr, key_pair->size)) {
      map->get_accessor.ptr = &map->slots[index].val_data;
      map->get_accessor.size = map->val_size;
      *val_pair = &map->get_accessor;
      return ccol_success;
    }

    index = (index + 1) % map->capacity;
  } while (index != start_index);

  return ccol_key_not_found;
}

/* Marks the matching slot DELETED (tombstone) rather than clearing it, so that
 * probe chains through the slot remain intact. If the load factor after
 * deletion falls below OPEN_ADDR_MIN_LOAD_FACTOR the table is halved. */
static ccol_retval_t oa_delete(open_addr_map* map, const cmap_pair* key_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val % map->capacity;
  size_t start_index = index;

  do {
    if (!(map->slots[index].metadata & SLOT_OCCUPIED) &&
        !(map->slots[index].metadata & SLOT_DELETED)) {
      return ccol_key_not_found;
    }

    if ((map->slots[index].metadata & SLOT_OCCUPIED) &&
        !(map->slots[index].metadata & SLOT_DELETED) &&
        oa_keys_equal(&map->slots[index], key_pair->ptr, key_pair->size)) {
      map->slots[index].metadata |= SLOT_DELETED;
      map->count--;
      map->deleted_count++;

      double load_factor = (double)map->count / map->capacity;
      if (load_factor < OPEN_ADDR_MIN_LOAD_FACTOR &&
          map->capacity > minimum_allowed_bucket_array_size) {
        oa_rehash(map, map->capacity / 2);
      }

      return ccol_success;
    }

    index = (index + 1) % map->capacity;
  } while (index != start_index);

  return ccol_key_not_found;
}

/* Frees the slot array and the map struct. Does not free the m_procs pointer
 * itself; that is done by the unified __chmap_destroy since the same procs
 * copy is shared between the map struct and its fields. */
static void oa_destroy(open_addr_map* map) {
  if (map) {
    _mem_free(map->m_procs, map->slots);
    _mem_free(map->m_procs, map);
  }
}

/* Clears all entries by replacing the slot array with a freshly zeroed one.
 * If new_capacity is 0, the existing capacity is reused. */
static ccol_retval_t oa_reset(open_addr_map* map, size_t new_capacity) {
  if (new_capacity == 0) {
    new_capacity = map->capacity;
  }

  oa_slot* new_slots =
      (oa_slot*)_mem_calloc(map->m_procs, new_capacity, sizeof(oa_slot));
  if (!new_slots) {
    return ccol_not_enough_memory;
  }
  _mem_free(map->m_procs, map->slots);
  map->slots = new_slots;

  map->capacity = new_capacity;
  map->count = 0;
  map->deleted_count = 0;
  map->val_size_initialized = false;

  return ccol_success;
}

/* ========================================================================== */
/*              SEPARATE CHAINING IMPLEMENTATION                              */
/* ========================================================================== */

#define dllistRefNodePtr2LlistNodePtr(tracker) \
  (llist_node*)((uint8_t*)tracker - offsetof(llist_node, dllist_refs))

/* Prepends node to the doubly-linked list rooted at *head. The list therefore
 * keeps the most-recently inserted element at the head; iteration via next
 * pointers visits nodes in reverse insertion order (newest first). */
static void attach_node_to_dllist(dllist_ref_node** head,
                                  dllist_ref_node* node) {
  node->prev = NULL;
  if (!(*head)) {
    node->next = NULL;
    *head = node;
  } else {
    node->next = *head;
    (*head)->prev = node;
    *head = node;
  }
}

/* Removes node from the doubly-linked list without freeing it. The three-way
 * pointer update handles all cases: head node, tail node, and middle node. */
static void detach_node_from_dllist(dllist_ref_node** head,
                                    dllist_ref_node* node) {
  if (node->next) {
    node->next->prev = node->prev;
  }
  if (node->prev) {
    node->prev->next = node->next;
  }
  if (*head == node) {
    *head = node->next;
  }
}

/* Frees the key and value buffers for an entry (if they are heap-allocated
 * rather than inline), detaches the node from the insertion-order dllist, then
 * frees the node struct itself. Passing NULL for head_of_all_elems skips the
 * dllist detach (used during full-map teardown where the list is abandoned). */
static void sc_destroy_llist_node(dllist_ref_node** head_of_all_elems,
                                  llist_node* elem) {
  if (elem) {
    if (!elem->data.key_is_inline && elem->data.key_storage.ptr) {
      _mem_free(elem->m_procs, elem->data.key_storage.ptr);
    }
    if (!elem->data.val_is_inline && elem->data.val_storage.ptr) {
      _mem_free(elem->m_procs, elem->data.val_storage.ptr);
    }
    if (head_of_all_elems) {
      detach_node_from_dllist(head_of_all_elems, &elem->dllist_refs);
    }
    if (elem->m_procs) {
      ccol_free_t free_func = elem->m_procs->free;
      free_func(elem);
    } else {
      mem_free(elem);
    }
  }
}

/* Allocates a new llist_node and copies key and value data into it. Both key
 * and value are stored inline (SSO: ≤ 23 bytes) or in a separate heap buffer
 * (> 23 bytes). The accessor cmap_pair structs are set to point into whichever
 * storage was chosen so callers always go through a stable pointer. The node is
 * prepended to the insertion-order dllist on success. */
static llist_node* sc_create_llist_node(dllist_ref_node** head_of_all_elems,
                                        chmap_entry* data, const void* key_ptr,
                                        const void* val_ptr) {
  llist_node* new_elem =
      (llist_node*)_mem_calloc(data->m_procs, 1, sizeof(llist_node));
  if (!new_elem) return NULL;

  new_elem->m_procs = data->m_procs;
  new_elem->data.m_procs = data->m_procs;
  new_elem->data.hash_val = data->hash_val;
  new_elem->data.key_size = data->key_size;

  if (data->key_size <= INLINE_STORAGE_THRESHOLD) {
    new_elem->data.key_is_inline = true;
    mem_cpy(&new_elem->data.key_storage.inline_data, key_ptr, data->key_size);
    new_elem->key_pair_accessor.ptr = &new_elem->data.key_storage.inline_data;
    new_elem->key_pair_accessor.size = data->key_size;
  } else {
    new_elem->data.key_is_inline = false;
    new_elem->data.key_storage.ptr = _mem_alloc(data->m_procs, data->key_size);
    if (!new_elem->data.key_storage.ptr) {
      sc_destroy_llist_node(NULL, new_elem);
      return NULL;
    }
    mem_cpy(new_elem->data.key_storage.ptr, key_ptr, data->key_size);
    new_elem->key_pair_accessor.ptr = new_elem->data.key_storage.ptr;
    new_elem->key_pair_accessor.size = data->key_size;
  }

  new_elem->data.val_size = data->val_size;
  if (data->val_size <= INLINE_STORAGE_THRESHOLD) {
    new_elem->data.val_is_inline = true;
    mem_cpy(&new_elem->data.val_storage.inline_data, val_ptr, data->val_size);
    new_elem->val_pair_accessor.ptr = &new_elem->data.val_storage.inline_data;
    new_elem->val_pair_accessor.size = data->val_size;
  } else {
    new_elem->data.val_is_inline = false;
    new_elem->data.val_storage.ptr = _mem_alloc(data->m_procs, data->val_size);
    if (!new_elem->data.val_storage.ptr) {
      sc_destroy_llist_node(NULL, new_elem);
      return NULL;
    }
    mem_cpy(new_elem->data.val_storage.ptr, val_ptr, data->val_size);
    new_elem->val_pair_accessor.ptr = new_elem->data.val_storage.ptr;
    new_elem->val_pair_accessor.size = data->val_size;
  }

  attach_node_to_dllist(head_of_all_elems, &new_elem->dllist_refs);
  new_elem->next = NULL;
  return new_elem;
}

/* Compares a node's key against key_ptr. The size check is a fast-reject;
 * for 4- and 8-byte keys integer comparison is used instead of memcmp to
 * allow the compiler to emit a single load+compare instruction. */
static inline bool sc_compare_keys(const llist_node* node, const void* key_ptr,
                                   size_t key_size) {
  if (node->data.key_size != key_size) return false;

  const void* node_key_ptr =
      node->data.key_is_inline
          ? (const void*)&node->data.key_storage.inline_data
          : (const void*)node->data.key_storage.ptr;

  if (key_size == sizeof(unsigned int)) {
    unsigned int a, b;
    memcpy(&a, node_key_ptr, sizeof(a));
    memcpy(&b, key_ptr, sizeof(b));
    return a == b;
  } else if (key_size == sizeof(unsigned long)) {
    unsigned long a, b;
    memcpy(&a, node_key_ptr, sizeof(a));
    memcpy(&b, key_ptr, sizeof(b));
    return a == b;
  } else {
    return memcmp(node_key_ptr, key_ptr, key_size) == 0;
  }
}

/* Linear search through a bucket's singly-linked chain. Returns the matching
 * node or NULL. Chains are expected to be short (O(1) average) due to the
 * bucket scaling strategy. */
static llist_node* sc_find_in_llist(llist_node* head, const void* key_ptr,
                                    size_t key_size) {
  llist_node* tracker = head;
  while (tracker) {
    if (sc_compare_keys(tracker, key_ptr, key_size)) {
      return tracker;
    }
    tracker = tracker->next;
  }
  return NULL;
}

/* Updates the value stored in an existing node, handling three cases based on
 * the new value size vs. the stored size: same size (overwrite in place),
 * smaller and fits inline (switch to inline storage and free old heap buffer),
 * or larger (realloc or allocate new heap buffer). */
static bool sc_reset_val_of_llist_node(llist_node* elem, const void* val_ptr,
                                       size_t val_size) {
  if (val_size == elem->data.val_size) {
    if (elem->data.val_is_inline) {
      mem_cpy(&elem->data.val_storage.inline_data, val_ptr, val_size);
    } else {
      mem_cpy(elem->data.val_storage.ptr, val_ptr, val_size);
    }
    return true;
  } else if (val_size <= INLINE_STORAGE_THRESHOLD) {
    if (!elem->data.val_is_inline) {
      _mem_free(elem->m_procs, elem->data.val_storage.ptr);
    }
    elem->data.val_is_inline = true;
    elem->data.val_size = val_size;
    mem_cpy(&elem->data.val_storage.inline_data, val_ptr, val_size);
    elem->val_pair_accessor.ptr = &elem->data.val_storage.inline_data;
    elem->val_pair_accessor.size = val_size;
    return true;
  } else {
    if (elem->data.val_is_inline) {
      void* new_ptr = _mem_alloc(elem->m_procs, val_size);
      if (!new_ptr) return false;
      elem->data.val_storage.ptr = new_ptr;
      elem->data.val_is_inline = false;
    } else {
      void* orig = elem->data.val_storage.ptr;
      elem->data.val_storage.ptr =
          _mem_realloc(elem->m_procs, elem->data.val_storage.ptr, val_size);
      if (!elem->data.val_storage.ptr) {
        elem->data.val_storage.ptr = orig;
        return false;
      }
    }
    mem_cpy(elem->data.val_storage.ptr, val_ptr, val_size);
    elem->data.val_size = val_size;
    elem->val_pair_accessor.ptr = elem->data.val_storage.ptr;
    elem->val_pair_accessor.size = val_size;
    return true;
  }
}

/* Removes the node matching key_ptr from a bucket's chain, sets *found, and
 * returns the updated chain head. The previous-pointer tracking enables O(n)
 * deletion without a doubly-linked bucket list. */
static llist_node* sc_delete_from_llist(llist_node* head,
                                        dllist_ref_node** head_of_all_elems,
                                        const void* key_ptr, size_t key_size,
                                        bool* found) {
  *found = false;
  llist_node* tracker = head;
  llist_node* previous = NULL;

  while (tracker) {
    if (sc_compare_keys(tracker, key_ptr, key_size)) {
      *found = true;
      if (!previous) {
        head = tracker->next;
      } else {
        previous->next = tracker->next;
      }
      sc_destroy_llist_node(head_of_all_elems, tracker);
      return head;
    }
    previous = tracker;
    tracker = tracker->next;
  }
  return head;
}

/* Destroys every node in a bucket chain and returns NULL. Used during
 * map reset/destroy to clear all buckets in sequence. */
static llist_node* sc_destroy_the_whole_llist(
    llist_node* head, dllist_ref_node** head_of_all_elems) {
  llist_node* tracker = head;
  while (tracker) {
    llist_node* node_to_be_deleted = tracker;
    tracker = tracker->next;
    sc_destroy_llist_node(head_of_all_elems, node_to_be_deleted);
  }
  return NULL;
}

/* Recalculates the scale-up and scale-down element count thresholds based on
 * the current bucket array size. Must be called after every bucket array resize
 * to keep the thresholds consistent with the new capacity. */
static void sc_set_scaling_limits(sep_chain_map* map) {
  map->elem_count_to_scale_up = map->bucket_arr_size + map->bucket_arr_size / 2;
  map->elem_count_to_scale_down = (map->bucket_arr_size) / 8;
}

/* Resizes the bucket array by scale_factor (up or down) and rehashes all
 * existing nodes into their new bucket positions. Scaling uses & (new_size-1)
 * rather than modulo, which is why all sizes are kept as powers of two. */
static void sc_scale(sep_chain_map* map, bool up) {
  if (up && (map->bucket_arr_size > max_power_of_two_size_t / scale_factor)) {
    // That's beyond the scale-up limit
    return;
  }

  size_t new_size = up ? map->bucket_arr_size * scale_factor
                       : map->bucket_arr_size / scale_factor;

  llist_node** new_arr =
      (llist_node**)_mem_calloc(map->m_procs, new_size, sizeof(llist_node*));
  if (!new_arr) return;

  for (size_t i = 0; i < map->bucket_arr_size; i++) {
    llist_node* tracker = map->bucket_arr[i];
    while (tracker) {
      llist_node* next = tracker->next;
      size_t new_index = tracker->data.hash_val & (new_size - 1);
      tracker->next = new_arr[new_index];
      new_arr[new_index] = tracker;
      tracker = next;
    }
  }

  _mem_free(map->m_procs, map->bucket_arr);
  map->bucket_arr = new_arr;
  map->bucket_arr_size = new_size;
  sc_set_scaling_limits(map);
}

/* Allocates and initialises the separate-chaining map struct and its bucket
 * array. All bucket pointers are zeroed via calloc. */
static sep_chain_map* sc_create(size_t bucket_arr_size, ccol_data_type key_type,
                                ccol_data_type val_type,
                                ccol_memmgmt_procs_t* m_procs,
                                ccol_hashing_proc_t custom_hashing_proc) {
  sep_chain_map* map =
      (sep_chain_map*)_mem_alloc(m_procs, sizeof(sep_chain_map));
  if (!map) return NULL;

  map->bucket_arr =
      (llist_node**)_mem_calloc(m_procs, bucket_arr_size, sizeof(llist_node*));
  if (!map->bucket_arr) {
    _mem_free(m_procs, map);
    return NULL;
  }

  map->bucket_arr_size = bucket_arr_size;
  map->elem_count = 0;
  map->key_type = key_type;
  map->val_type = val_type;
  map->head_of_all_elems = NULL;
  map->m_procs = m_procs;
  map->custom_hashing_proc = custom_hashing_proc;
  sc_set_scaling_limits(map);

  return map;
}

/* Inserts or updates a key-value pair in the separate-chaining map. An existing
 * key results in an in-place value update via sc_reset_val_of_llist_node. A new
 * key triggers node creation; the node is prepended to the bucket's chain.
 * Scales up the bucket array after insertion if elem_count_to_scale_up is hit.
 */
static ccol_retval_t sc_insert(sep_chain_map* map, const cmap_pair* key_pair,
                               const cmap_pair* val_pair) {
  if (map->elem_count == max_elem_count) {
    return ccol_container_full;
  }

  chmap_entry data = {
      .hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                map->custom_hashing_proc),
      .key_size = key_pair->size,
      .val_size = val_pair->size,
      .m_procs = map->m_procs};

  size_t index = data.hash_val & (map->bucket_arr_size - 1);

  llist_node* existing =
      sc_find_in_llist(map->bucket_arr[index], key_pair->ptr, key_pair->size);
  if (existing) {
    return sc_reset_val_of_llist_node(existing, val_pair->ptr, val_pair->size)
               ? ccol_key_already_present
               : ccol_not_enough_memory;
  }

  llist_node* new_node = sc_create_llist_node(&map->head_of_all_elems, &data,
                                              key_pair->ptr, val_pair->ptr);
  if (!new_node) {
    return ccol_not_enough_memory;
  }

  new_node->next = map->bucket_arr[index];
  map->bucket_arr[index] = new_node;

  if (++map->elem_count >= map->elem_count_to_scale_up) {
    sc_scale(map, true);
  }

  return ccol_success;
}

/* Looks up key_pair and sets *val_pair to point at the node's value accessor.
 * The returned pointer is valid until the key is deleted or its value is
 * updated to a different size. */
static ccol_retval_t sc_get(sep_chain_map* map, const cmap_pair* key_pair,
                            cmap_pair** val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val & (map->bucket_arr_size - 1);

  llist_node* node =
      sc_find_in_llist(map->bucket_arr[index], key_pair->ptr, key_pair->size);
  if (node) {
    *val_pair = &node->val_pair_accessor;
    return ccol_success;
  }
  return ccol_key_not_found;
}

/* Deletes the entry matching key_pair from its bucket chain. Scales down the
 * bucket array when elem_count drops below elem_count_to_scale_down and the
 * array is large enough to shrink. */
static ccol_retval_t sc_delete(sep_chain_map* map, const cmap_pair* key_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val & (map->bucket_arr_size - 1);

  bool found = false;
  map->bucket_arr[index] =
      sc_delete_from_llist(map->bucket_arr[index], &map->head_of_all_elems,
                           key_pair->ptr, key_pair->size, &found);

  if (found) {
    if (--map->elem_count < map->elem_count_to_scale_down &&
        map->bucket_arr_size >= minimum_scale_down_threshold) {
      sc_scale(map, false);
    }
    return ccol_success;
  }
  return ccol_key_not_found;
}

/* Destroys all bucket chains, frees the bucket array, and frees the map struct.
 * The insertion-order dllist head pointer is passed through to each chain
 * teardown so it is updated correctly during destruction (even though the full
 * list is being discarded anyway, this keeps the bookkeeping consistent). */
static void sc_destroy(sep_chain_map* map) {
  if (map) {
    for (size_t i = 0; i < map->bucket_arr_size; i++) {
      sc_destroy_the_whole_llist(map->bucket_arr[i], &map->head_of_all_elems);
    }
    _mem_free(map->m_procs, map->bucket_arr);
    _mem_free(map->m_procs, map);
  }
}

/* Clears all entries and optionally resizes the bucket array to
 * new_bucket_array_size. Pass 0 to retain the current size. */
static ccol_retval_t sc_reset(sep_chain_map* map,
                              size_t new_bucket_array_size) {
  for (size_t i = 0; i < map->bucket_arr_size; i++) {
    sc_destroy_the_whole_llist(map->bucket_arr[i], &map->head_of_all_elems);
  }

  if (new_bucket_array_size > 0 &&
      new_bucket_array_size != map->bucket_arr_size) {
    llist_node** orig = map->bucket_arr;
    map->bucket_arr = _mem_realloc(map->m_procs, map->bucket_arr,
                                   new_bucket_array_size * sizeof(llist_node*));
    if (!map->bucket_arr) {
      map->bucket_arr = orig;
      mem_zero(map->bucket_arr, map->bucket_arr_size * sizeof(llist_node*));
      map->elem_count = 0;
      sc_set_scaling_limits(map);
      return ccol_not_enough_memory;
    }
    map->bucket_arr_size = new_bucket_array_size;
  }

  mem_zero(map->bucket_arr, map->bucket_arr_size * sizeof(llist_node*));
  map->elem_count = 0;
  sc_set_scaling_limits(map);

  return ccol_success;
}

/* ========================================================================== */
/*                    ITERATOR STRUCTURES                                     */
/* ========================================================================== */

typedef struct chmap_cmap_iterator {
  chmap parent_map;
  union {
    dllist_ref_node* sc_tracker;
    size_t oa_index;
  } iter;
  cmap_pair oa_key_pair;
  cmap_pair oa_val_pair;
  cmap_iterator user_iter;
} chmap_cmap_iterator;

#define cmapIter2ChmapIter(u_iter)          \
  (chmap_cmap_iterator*)((uint8_t*)u_iter - \
                         offsetof(chmap_cmap_iterator, user_iter))

/* ========================================================================== */
/*                         UNIFIED API IMPLEMENTATION                         */
/* ========================================================================== */

/* Creates a new hash map. The implementation (open-addressing or separate-
 * chaining) is chosen at creation time based on key and value types. The
 * custom allocator, if provided, is copied into a privately-owned struct so
 * the caller's copy can be freed independently. */
chmap chmap_create_full(size_t initial_bucket_array_size,
                        ccol_data_type key_type, ccol_data_type val_type,
                        ccol_memmgmt_procs_t* mmgmt_procs,
                        ccol_hashing_proc_t custom_hashing_proc, char** err) {
  if (initial_bucket_array_size == 0) {
    if (err) *err = CCOL_ERR_STR("initial_bucket_array_size is zero");
    return NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  chmap chm = (chmap)_mem_alloc(mmgmt_procs, sizeof(struct chashmap));
  if (!chm) {
    if (err) *err = CCOL_ERR_STR("Failed to allocate chashmap");
    return NULL;
  }

  if (initial_bucket_array_size <= minimum_allowed_bucket_array_size) {
    initial_bucket_array_size = minimum_allowed_bucket_array_size;
  } else {
    initial_bucket_array_size =
        find_nearest_gte_power_of_two(initial_bucket_array_size);
    if (initial_bucket_array_size > max_elem_count) {
      _mem_free(mmgmt_procs, chm);
      if (err) {
        *err = CCOL_ERR_STR("Initial bucket array size is too big");
      }
      return NULL;
    }
  }

  if (should_use_open_addressing(key_type, val_type)) {
    chm->impl_type = IMPL_OPEN_ADDRESSING;

    size_t key_size = get_type_size(key_type);
    size_t val_size = get_type_size(val_type);

    ccol_memmgmt_procs_t* procs_copy = NULL;
    if (mmgmt_procs) {
      procs_copy = (ccol_memmgmt_procs_t*)_mem_alloc(
          mmgmt_procs, sizeof(ccol_memmgmt_procs_t));
      if (!procs_copy) {
        if (err) *err = CCOL_ERR_STR("Failed to allocate m_procs");
        _mem_free(mmgmt_procs, chm);
        return NULL;
      }
      mem_cpy(procs_copy, mmgmt_procs, sizeof(ccol_memmgmt_procs_t));
    }

    chm->impl.oa_map =
        oa_create(initial_bucket_array_size, key_type, val_type, key_size,
                  val_size, procs_copy, custom_hashing_proc);
    if (!chm->impl.oa_map) {
      if (err) *err = CCOL_ERR_STR("Failed to create open addressing map");
      if (procs_copy) _mem_free(mmgmt_procs, procs_copy);
      _mem_free(mmgmt_procs, chm);
      return NULL;
    }
  } else {
    chm->impl_type = IMPL_SEPARATE_CHAINING;

    ccol_memmgmt_procs_t* procs_copy = NULL;
    if (mmgmt_procs) {
      procs_copy = (ccol_memmgmt_procs_t*)_mem_alloc(
          mmgmt_procs, sizeof(ccol_memmgmt_procs_t));
      if (!procs_copy) {
        if (err) *err = CCOL_ERR_STR("Failed to allocate m_procs");
        _mem_free(mmgmt_procs, chm);
        return NULL;
      }
      mem_cpy(procs_copy, mmgmt_procs, sizeof(ccol_memmgmt_procs_t));
    }

    chm->impl.sc_map = sc_create(initial_bucket_array_size, key_type, val_type,
                                 procs_copy, custom_hashing_proc);
    if (!chm->impl.sc_map) {
      if (err) *err = CCOL_ERR_STR("Failed to create separate chaining map");
      if (procs_copy) _mem_free(mmgmt_procs, procs_copy);
      _mem_free(mmgmt_procs, chm);
      return NULL;
    }
  }

  if (err) *err = NULL;
  return chm;
}

/* Public insert/update dispatch: validates inputs then delegates to the
 * backend-specific insert function. */
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair) {
  if (!chm || !key_pair || !val_pair || !key_pair->ptr || !val_pair->ptr ||
      key_pair->size == 0 || val_pair->size == 0) {
    return ccol_invalid_args;
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return oa_insert(chm->impl.oa_map, key_pair, val_pair);
  } else {
    return sc_insert(chm->impl.sc_map, key_pair, val_pair);
  }
}

/* Returns a pointer-to-pointer to the value storage for key_pair. The inner
 * pointer is valid until the next mutating operation on this key. */
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair* key_pair,
                                 cmap_pair** val_pair) {
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0 || !val_pair) {
    return ccol_invalid_args;
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return oa_get(chm->impl.oa_map, key_pair, val_pair);
  } else {
    return sc_get(chm->impl.sc_map, key_pair, val_pair);
  }
}

/* Looks up key_pair and copies up to target_buf_size bytes of the value into
 * target_buf. If the stored value is smaller than target_buf_size, the
 * remaining bytes are zeroed. */
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair* key_pair,
                                  void* target_buf, size_t target_buf_size) {
  if (!target_buf || target_buf_size == 0) {
    return ccol_invalid_args;
  }
  cmap_pair* val_pair = NULL;
  ccol_retval_t ret = chmap_get_elem_ref(chm, key_pair, &val_pair);
  if (ret == ccol_success && val_pair) {
    size_t copy_size =
        val_pair->size < target_buf_size ? val_pair->size : target_buf_size;
    mem_cpy(target_buf, val_pair->ptr, copy_size);
    if (val_pair->size < target_buf_size) {
      mem_zero((uint8_t*)target_buf + val_pair->size,
               target_buf_size - val_pair->size);
    }
  }
  return ret;
}

/* Public delete dispatch: validates inputs then delegates to the backend. */
ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair* key_pair) {
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0) {
    return ccol_invalid_args;
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return oa_delete(chm->impl.oa_map, key_pair);
  } else {
    return sc_delete(chm->impl.sc_map, key_pair);
  }
}

/* Returns the number of live key-value pairs in the map. */
size_t chmap_elem_count(chmap chm) {
  if (!chm) {
    ccol_assert(false);
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa_map->count;
  } else {
    return chm->impl.sc_map->elem_count;
  }
}

/* Clears all entries and optionally resizes the internal bucket array. The
 * size is rounded up to the nearest power of two and clamped to
 * minimum_allowed_bucket_array_size. Pass 0 to retain the current size. */
ccol_retval_t chmap_reset(chmap chm, size_t new_bucket_array_size) {
  if (!chm) {
    ccol_assert(false);
  }

  if (new_bucket_array_size > 0 &&
      new_bucket_array_size < minimum_allowed_bucket_array_size) {
    new_bucket_array_size = minimum_allowed_bucket_array_size;
  } else if (new_bucket_array_size > 0) {
    new_bucket_array_size =
        find_nearest_gte_power_of_two(new_bucket_array_size);
    if (new_bucket_array_size > max_elem_count) {
      // The requsted size is too big
      return ccol_not_enough_memory;
    }
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return oa_reset(chm->impl.oa_map, new_bucket_array_size);
  } else {
    return sc_reset(chm->impl.sc_map, new_bucket_array_size);
  }
}

/* Creates an iterator positioned at the first element. For separate-chaining
 * maps, iteration follows the insertion-order dllist. For open-addressing maps,
 * the first occupied non-deleted slot is found by linear scan. Returns NULL
 * for an empty map. The iterator heap-allocates chmap_cmap_iterator. */
static cmap_iterator* chmap_iter_next(cmap_iterator* iter);

cmap_iterator* chashmap_begin_iter(chmap chm, char** err) {
  if (err) {
    *err = NULL;
  }

  if (!chm) {
    return NULL;
  }

  if (chm->impl_type == IMPL_SEPARATE_CHAINING) {
    if (!chm->impl.sc_map->head_of_all_elems) {
      return NULL;
    }

    ccol_memmgmt_procs_t* m_procs = chm->impl.sc_map->m_procs;
    chmap_cmap_iterator* real_iter =
        _mem_calloc(m_procs, 1, sizeof(chmap_cmap_iterator));
    if (!real_iter) {
      if (err) {
        *err = CCOL_ERR_STR("Failed to allocate iterator");
      }
      return NULL;
    }

    dllist_ref_node* tracker = chm->impl.sc_map->head_of_all_elems;
    real_iter->parent_map = chm;
    real_iter->iter.sc_tracker = tracker;
    llist_node* host = dllistRefNodePtr2LlistNodePtr(tracker);
    real_iter->user_iter.key_pair = &(host->key_pair_accessor);
    real_iter->user_iter.val_pair = &(host->val_pair_accessor);
    real_iter->user_iter._next_fn = chmap_iter_next;
    real_iter->user_iter._free_fn = __chmap_iterator_destroy;
    real_iter->user_iter._direct_ptr = false;

    return &(real_iter->user_iter);
  } else {
    // Open addressing iteration
    open_addr_map* map = chm->impl.oa_map;

    // Find first occupied slot
    size_t i = 0;
    while (i < map->capacity && (!(map->slots[i].metadata & SLOT_OCCUPIED) ||
                                 (map->slots[i].metadata & SLOT_DELETED))) {
      i++;
    }

    if (i >= map->capacity) {
      return NULL;  // Empty map
    }

    chmap_cmap_iterator* real_iter =
        _mem_calloc(map->m_procs, 1, sizeof(chmap_cmap_iterator));
    if (!real_iter) {
      if (err) {
        *err = CCOL_ERR_STR("Failed to allocate iterator");
      }
      return NULL;
    }

    real_iter->parent_map = chm;
    real_iter->iter.oa_index = i;

    real_iter->oa_key_pair.ptr = &map->slots[i].key_data;
    real_iter->oa_key_pair.size = map->key_size;
    real_iter->oa_val_pair.ptr = &map->slots[i].val_data;
    real_iter->oa_val_pair.size = map->val_size;

    real_iter->user_iter.key_pair = &real_iter->oa_key_pair;
    real_iter->user_iter.val_pair = &real_iter->oa_val_pair;
    real_iter->user_iter._next_fn = chmap_iter_next;
    real_iter->user_iter._free_fn = __chmap_iterator_destroy;
    real_iter->user_iter._direct_ptr = false;

    return &(real_iter->user_iter);
  }
}

/* Frees the iterator struct allocated by chashmap_begin_iter. Uses the m_procs
 * stored in the underlying backend map since the iterator itself does not hold
 * an allocator pointer. */
void __chmap_iterator_destroy(cmap_iterator* iter) {
  if (iter) {
    chmap_cmap_iterator* real_iter = cmapIter2ChmapIter(iter);
    if (real_iter->parent_map->impl_type == IMPL_OPEN_ADDRESSING) {
      _mem_free(real_iter->parent_map->impl.oa_map->m_procs, real_iter);
    } else {
      _mem_free(real_iter->parent_map->impl.sc_map->m_procs, real_iter);
    }
  }
}

/* Advances the iterator to the next element. For separate-chaining the dllist
 * next pointer is followed. For open-addressing the slot array is scanned
 * linearly for the next occupied non-deleted slot. Returns NULL (and destroys
 * the iterator) when the end is reached. */
static cmap_iterator* chmap_iter_next(cmap_iterator* iter) {
  chmap_cmap_iterator* real_iter = cmapIter2ChmapIter(iter);

  if (real_iter->parent_map->impl_type == IMPL_SEPARATE_CHAINING) {
    real_iter->iter.sc_tracker = real_iter->iter.sc_tracker->next;

    if (real_iter->iter.sc_tracker) {
      dllist_ref_node* tracker = real_iter->iter.sc_tracker;
      llist_node* host = dllistRefNodePtr2LlistNodePtr(tracker);
      iter->key_pair = &(host->key_pair_accessor);
      iter->val_pair = &(host->val_pair_accessor);
    } else {
      __chmap_iterator_destroy(iter);
      iter = NULL;
    }
  } else {
    // Open addressing iteration
    open_addr_map* map = real_iter->parent_map->impl.oa_map;
    size_t i = real_iter->iter.oa_index + 1;

    // Find next occupied slot
    while (i < map->capacity && (!(map->slots[i].metadata & SLOT_OCCUPIED) ||
                                 (map->slots[i].metadata & SLOT_DELETED))) {
      i++;
    }

    if (i >= map->capacity) {
      __chmap_iterator_destroy(iter);
      iter = NULL;
    } else {
      real_iter->iter.oa_index = i;

      real_iter->oa_key_pair.ptr = &map->slots[i].key_data;
      real_iter->oa_key_pair.size = map->key_size;
      real_iter->oa_val_pair.ptr = &map->slots[i].val_data;
      real_iter->oa_val_pair.size = map->val_size;

      iter->key_pair = &real_iter->oa_key_pair;
      iter->val_pair = &real_iter->oa_val_pair;
    }
  }

  return iter;
}

/* Destroys the underlying backend map and its privately-owned m_procs copy,
 * then frees the top-level chashmap struct. When a custom allocator was
 * provided, chm was allocated through it, so the same free function is used
 * for chm. The free_func local is captured before freeing procs so the
 * function pointer remains valid after the procs struct itself is freed. */
void __chmap_destroy(chmap chm) {
  if (chm) {
    if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
      if (chm->impl.oa_map) {
        ccol_memmgmt_procs_t* procs = chm->impl.oa_map->m_procs;
        oa_destroy(chm->impl.oa_map);
        if (procs) {
          ccol_free_t free_func = procs->free;
          free_func(procs);
          free_func(chm);
          return;
        }
      }
    } else {
      if (chm->impl.sc_map) {
        ccol_memmgmt_procs_t* procs = chm->impl.sc_map->m_procs;
        sc_destroy(chm->impl.sc_map);
        if (procs) {
          ccol_free_t free_func = procs->free;
          free_func(procs);
          free_func(chm);
          return;
        }
      }
    }
    mem_free(chm);
  }
}

#ifdef RUNNING_UNIT_TESTS
/* Returns the current bucket array / slot array size for white-box unit tests
 * that verify the resize thresholds of both backends. Not part of the public
 * API. */
size_t chmap_get_bucket_arr_size(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa_map->capacity;
  } else {
    return chm->impl.sc_map->bucket_arr_size;
  }
}

/* Returns the element count at which the next scale-up will be triggered. */
size_t chmap_get_elem_count_to_scale_up(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return (size_t)(chm->impl.oa_map->capacity * OPEN_ADDR_MAX_LOAD_FACTOR);
  } else {
    return chm->impl.sc_map->elem_count_to_scale_up;
  }
}

/* Returns the element count at which the next scale-down will be triggered. */
size_t chmap_get_elem_count_to_scale_down(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return (size_t)(chm->impl.oa_map->capacity * OPEN_ADDR_MIN_LOAD_FACTOR);
  } else {
    return chm->impl.sc_map->elem_count_to_scale_down;
  }
}
#endif
