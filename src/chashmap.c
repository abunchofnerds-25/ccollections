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
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const size_t minimum_allowed_bucket_array_size = 16;
static const size_t scale_factor = 4;

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
  // Parallel array, one entry per slot (indices always match slots[]), each
  // holding a stable {ptr, size} accessor for that slot's value. Kept
  // separate from oa_slot itself (rather than embedded in it) so the hot
  // insert/probe/delete loops, which only ever touch key_data/val_data/
  // metadata, keep scanning the compact 24-byte oa_slot array without also
  // dragging an extra 16 bytes per slot through cache on every probe; only
  // chmap_get_elem_ref's own, comparatively cold, final lookup ever reads
  // this array. A slot's own val_accessors[index].ptr is always
  // &slots[index].val_data and .size is always the map's own val_size, both
  // populated once whenever that slot is (re)written (see oa_insert /
  // oa_rehash), never on the read side. This is what gives every distinct
  // occupied slot its own permanently stable cmap_pair, so that two
  // concurrently-held chmap_get_elem_ref results (for two different keys)
  // never alias one another the way a single shared accessor field would.
  cmap_pair* val_accessors;
  size_t capacity;
  size_t count;
  size_t key_size;
  size_t val_size;
  size_t deleted_count;
  ccol_memmgmt_procs_t* m_procs;
  ccol_hashing_proc_t custom_hashing_proc;
  ccol_data_type key_type;
  ccol_data_type val_type;
} open_addr_map;

/* ========================================================================== */
/*                 SEPARATE CHAINING STRUCTURES                               */
/* ========================================================================== */

// Deliberately NOT packed, and each union explicitly _Alignas(max_align_t):
// key_storage/val_storage's inline_data (used to store an SSO-eligible
// key/value, up to INLINE_STORAGE_THRESHOLD bytes) must land at an offset
// suitably aligned for ANY type a caller may store there, since
// chmap_get/chmap_get_ptr and the iterator accessor macros cast a pointer
// into this storage directly to the caller's value type and dereference it,
// including for in-place modification, which a memcpy-based read cannot
// safely substitute for. max_align_t (not merely 8) is the correct bound
// here, not an arbitrary strengthening: it is the same guarantee a plain
// malloc() already gives the heap-allocated (> INLINE_STORAGE_THRESHOLD)
// storage path right below, and this struct exists specifically to be a
// drop-in inline substitute for that heap allocation, so it must satisfy
// the same alignment contract for whatever type-erased bytes a caller
// stores here, not just the common <=8-byte-aligned case (int/long/double/
// pointer). A plain 8-byte bound (this struct's own alignment before this
// fix, since no member other than key_storage/val_storage required more)
// left val_storage at absolute offset 72 within llist_node, 8 bytes short
// of the 16-byte alignment `long double` requires on this platform, one
// of the two built-in types (see chashmap.h's own "excludes long double
// from open-addressing" note) that is guaranteed to be stored inline via
// this exact path for any chashmap value; reproduced directly with
// -fsanitize=undefined ("load of misaligned address ... which requires 16
// byte alignment") on chmap_get_ptr of a plain `char* -> long double` map,
// not merely inferred from the struct layout. The same root cause (a
// direct-cast read of a pointer to insufficiently-aligned storage, as
// opposed to a memcpy-based read) as the packed-struct hazard documented
// in cjson.c/cyaml.c/clrucache.c/cthreadcomm.c/chttpclient.c, just reached
// via a union that under-declares its own alignment requirement rather
// than via an explicit __attribute__((packed)).
typedef struct chmap_entry {
  size_t hash_val;
  _Alignas(max_align_t) union {
    void* ptr;
    // SSO: 23 bytes + null terminator
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } key_storage;
  size_t key_size;
  bool key_is_inline;
  _Alignas(max_align_t) union {
    void* ptr;
    // SSO: 23 bytes + null terminator
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } val_storage;
  size_t val_size;
  bool val_is_inline;
  ccol_memmgmt_procs_t* m_procs;
} chmap_entry;

// Not packed: provides no size benefit (two same-size pointer fields need no
// internal padding either way), and packing it would leave it embeddable at
// a non-8-byte-aligned offset inside llist_node with no upside.
typedef struct dllist_ref_node {
  struct dllist_ref_node* prev;
  struct dllist_ref_node* next;
} dllist_ref_node;

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

/* Returns true for all ccol_data_type values that fit in <= 8 bytes and can be
 * hashed with Fibonacci hashing (integers, floats, and pointers). Used to
 * decide which map backend to instantiate at creation time. */
static inline bool is_type_integral(ccol_data_type type) {
  switch (type) {
    case ccol_char:
    case ccol_signed_char:
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
    case ccol_signed_char:
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

/* Returns true when both key and value types are integral and <= 8 bytes, which
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

/* Hashes a long double key by its numeric VALUE rather than its raw byte
 * representation. Unlike float/double (exactly 4/8 bytes, no padding),
 * long double's in-memory representation on most platforms (e.g. 80-bit
 * x87 extended precision stored in a 16-byte slot) contains padding bits
 * the C standard leaves completely unspecified; two variables holding the
 * exact same mathematical value can differ in those padding bits depending
 * on how each was computed and stored (empirically confirmed elsewhere in
 * this codebase: an automatic-storage-duration long double's padding is
 * not reliably reproducible across separate constructions of the same
 * value, even after an explicit memset, once the compiler treats the
 * whole-object assignment as making that memset dead). A raw byte hash
 * (the strategy used for every other >8-byte / non-integral key type)
 * would therefore make two numerically-identical long double keys land in
 * different buckets, silently defeating any lookup that doesn't reuse the
 * exact original bytes. Decomposing the VALUE via frexpl (which operates
 * on the loaded floating-point value, never on its underlying byte
 * representation) sidesteps this: the resulting mantissa/exponent pair is
 * a deterministic function of the number itself, independent of whatever
 * padding bits happened to accompany it.
 *
 * Three values get dedicated handling instead of reaching frexpl:
 * - NaN: frexpl and relational comparison have no useful meaning for NaN,
 *   and unlike float/double (whose full representation is always
 *   meaningful, so a raw-byte NaN-payload comparison is well-defined),
 *   long double's padding bytes accompanying a NaN are routinely genuine
 *   uninitialized memory in practice, not merely unspecified-but-stable
 *   content: a plain `long double n = NAN;` writes only the significant
 *   NaN bits, never the padding, so reading those padding bytes at all
 *   (even just to hash or memcmp them) is a real, valgrind/MSan-flagged
 *   use of uninitialized memory, confirmed empirically while developing
 *   this function. Every NaN long double therefore hashes to one fixed,
 *   dedicated constant, touching no padding byte at all; this necessarily
 *   means every NaN long double collapses into a single key (see
 *   long_double_keys_equal's matching NaN branch below), unlike float/
 *   double's own distinct-NaN-payload policy - a deliberate, narrower
 *   divergence forced by long double's padding, not an oversight, and one
 *   this codebase already has precedent for (cbstmap's own long double
 *   comparator collapses every NaN into one equivalence class for the
 *   identical reason: a BST's total-order requirement leaves it no other
 *   sound choice, and it never reads padding bytes to do so either).
 * - +-0.0: unified into one canonical hash up front rather than trusted to
 *   fall out of frexpl, since the C standard only promises frexp returns
 *   "zero" for a zero input without guaranteeing the sign of that zero is
 *   (or isn't) preserved identically across every libm implementation.
 * - +-Infinity: frexpl's exponent output is unspecified for an infinite
 *   input, and converting an infinite floating value to an integer type is
 *   undefined behavior, so both signs of infinity get their own fixed
 *   hash constant computed without ever calling frexpl or casting the
 *   value to an integer. */
static inline size_t hash_long_double_value(long double v) {
  if (isnan(v)) {
    return hash_int_fast(3);
  }

  if (v == 0.0L) {
    return hash_int_fast(0);
  }

  if (isinf(v)) {
    return hash_int_fast(v > 0.0L ? 1 : 2);
  }

  int exp = 0;
  long double mantissa = frexpl(v, &exp);
  // |mantissa| is in [0.5, 1), so the scaled value below always fits
  // safely within int64_t (magnitude strictly less than 2^62).
  int64_t scaled_mantissa = (int64_t)(mantissa * 4611686018427387904.0L);
  size_t h1 = hash_int_fast((size_t)(uint64_t)scaled_mantissa);
  size_t h2 = hash_int_fast((size_t)(int64_t)exp);
  return h1 ^ (h2 * XXH_PRIME_2 + XXH_PRIME_1);
}

/* Dispatches to the appropriate hash function based on key type. Integral and
 * float types use Fibonacci hashing on their bit pattern; everything else
 * (strings, structs, pointer-to-data) uses xxhash64_buffer. A custom hashing
 * proc overrides all built-in strategies when provided. */
static inline size_t hash_key_data(const void* key_ptr, size_t key_size,
                                   ccol_data_type key_type,
                                   ccol_hashing_proc_t custom_proc) {
  if (custom_proc) {
    return custom_proc(key_ptr, key_size);
  }

  // Use fast Fibonacci hashing for all integral types. Every multi-byte
  // read below goes through mem_cpy rather than a direct pointer-cast
  // dereference: key_ptr may come straight from a caller-supplied cmap_pair
  // (the raw chmap_insert_elem/_get_elem_ref/_delete_elem function layer),
  // which carries no alignment guarantee the way a type-safe macro's own
  // local variable address does. A direct `*(uint32_t*)key_ptr`-style read
  // of a misaligned pointer is UB and can fault on strict-alignment
  // architectures; see this file's own chmap_entry _Alignas(max_align_t)
  // comment for the identical hazard class already fixed elsewhere in this
  // module.
  switch (key_type) {
    case ccol_char:
    case ccol_signed_char:
    case ccol_unsigned_char:
      return hash_int_fast((size_t)*(uint8_t*)key_ptr);
    case ccol_short:
    case ccol_unsigned_short: {
      uint16_t bits;
      mem_cpy(&bits, key_ptr, sizeof(bits));
      return hash_int_fast((size_t)bits);
    }
    case ccol_int:
    case ccol_unsigned_int: {
      uint32_t bits;
      mem_cpy(&bits, key_ptr, sizeof(bits));
      return hash_int_fast((size_t)bits);
    }
    case ccol_long:
    case ccol_unsigned_long: {
#if SIZE_MAX == UINT64_MAX
      uint64_t bits;
      mem_cpy(&bits, key_ptr, sizeof(bits));
      return hash_int_fast((size_t)bits);
#else
      uint32_t bits;
      mem_cpy(&bits, key_ptr, sizeof(bits));
      return hash_int_fast((size_t)bits);
#endif
    }
    case ccol_long_long:
    case ccol_unsigned_long_long: {
#if SIZE_MAX == UINT64_MAX
      uint64_t bits;
      mem_cpy(&bits, key_ptr, sizeof(bits));
      return hash_int_fast((size_t)bits);
#else
      // On 32-bit, hash the 64-bit value by combining high and low parts
      uint64_t val;
      mem_cpy(&val, key_ptr, sizeof(val));
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
    case ccol_long_double: {
      long double v;
      mem_cpy(&v, key_ptr, sizeof(v));
      return hash_long_double_value(v);
    }
    default:
      return xxhash64_buffer(key_ptr, key_size, 0);
  }
}

/* Returns true if the byte ranges [a, a+a_size) and [b, b+b_size) overlap.
 * Shared by both backends to detect when a caller-supplied value pointer
 * aliases the very storage an insert/update is about to mutate: oa_insert's
 * existing-key update path (this section, below) and
 * sc_reset_val_of_llist_node (separate-chaining section, further down). */
static inline bool ranges_overlap(const void* a, size_t a_size, const void* b,
                                  size_t b_size) {
  if (a_size == 0 || b_size == 0) return false;
  uintptr_t a_start = (uintptr_t)a;
  uintptr_t a_end = a_start + a_size;
  uintptr_t b_start = (uintptr_t)b;
  uintptr_t b_end = b_start + b_size;
  return a_start < b_end && b_start < a_end;
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

  map->val_accessors =
      (cmap_pair*)_mem_calloc(m_procs, capacity, sizeof(cmap_pair));
  if (!map->val_accessors) {
    _mem_free(m_procs, map->slots);
    _mem_free(m_procs, map);
    return NULL;
  }

  map->capacity = capacity;
  map->count = 0;
  map->deleted_count = 0;
  map->key_size = key_size;
  map->val_size = val_size;
  map->m_procs = m_procs;
  map->custom_hashing_proc = custom_hashing_proc;
  map->key_type = key_type;
  map->val_type = val_type;

  return map;
}

/* Resizes the slot array to new_capacity and reinserts all live entries.
 * Deleted slots are not carried over so the deleted_count resets to zero,
 * which reduces probing length after many deletions. The hash is recomputed
 * for each entry because the slot array does not store hash values.
 * new_capacity is always a power of two (see should_use_open_addressing's
 * callers), so index arithmetic uses & (new_capacity - 1) instead of the far
 * costlier % new_capacity.
 *
 * Returns ccol_not_enough_memory (leaving the map completely untouched) if
 * either allocation fails, so a caller whose own operation subsequently
 * fails purely because an opportunistic rehash couldn't happen can report
 * that honestly instead of misattributing it to some other condition (see
 * oa_insert's own use of this return value). */
static ccol_retval_t oa_rehash(open_addr_map* map, size_t new_capacity) {
  oa_slot* old_slots = map->slots;
  cmap_pair* old_val_accessors = map->val_accessors;
  size_t old_capacity = map->capacity;

  map->slots =
      (oa_slot*)_mem_calloc(map->m_procs, new_capacity, sizeof(oa_slot));
  if (!map->slots) {
    map->slots = old_slots;
    return ccol_not_enough_memory;
  }

  map->val_accessors =
      (cmap_pair*)_mem_calloc(map->m_procs, new_capacity, sizeof(cmap_pair));
  if (!map->val_accessors) {
    _mem_free(map->m_procs, map->slots);
    map->slots = old_slots;
    map->val_accessors = old_val_accessors;
    return ccol_not_enough_memory;
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
      size_t index = hash_val & (new_capacity - 1);

      // Prefetch likely next location
      __builtin_prefetch(&map->slots[(index + 1) & (new_capacity - 1)], 1, 1);

      while (map->slots[index].metadata & SLOT_OCCUPIED) {
        index = (index + 1) & (new_capacity - 1);
        __builtin_prefetch(&map->slots[(index + 1) & (new_capacity - 1)], 1, 1);
      }

      map->slots[index] = old_slots[i];
      map->slots[index].metadata = SLOT_OCCUPIED;  // Clear deleted flag
      // The accessor's .ptr is self-referential (it must point at this
      // slot's own val_data), so it can never simply be copied over from
      // the old array like the rest of the slot's bytes; it has to be
      // recomputed against the new array's own address for this index.
      map->val_accessors[index].ptr = &map->slots[index].val_data;
      map->val_accessors[index].size = map->val_size;
      map->count++;
    }
  }

  _mem_free(map->m_procs, old_slots);
  _mem_free(map->m_procs, old_val_accessors);
  return ccol_success;
}

/* Probes for the first genuinely empty slot (in probe order from hash_val's
 * own home slot), or, failing that, the first tombstone encountered along
 * the way, at which a definitely-new key may be placed. Used to (re-)locate
 * an insertion point against the map's *current* capacity/mask; the caller
 * must already have established that the key is not present anywhere in the
 * table (a probe can only stop early at a genuinely empty slot, so a fresh
 * probe like this one cannot itself re-verify absence without scanning the
 * whole table again). This is exactly what oa_insert needs after growing
 * the table out from under an earlier probe's now-stale index/capacity. */
static void oa_find_insertion_slot(const open_addr_map* map, size_t hash_val,
                                   size_t* out_index, bool* out_found_empty,
                                   size_t* out_first_deleted) {
  size_t index = hash_val & (map->capacity - 1);
  size_t start_index = index;
  size_t first_deleted = map->capacity;

  __builtin_prefetch(&map->slots[index], 0, 1);

  do {
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 0, 1);

    if (!(map->slots[index].metadata & SLOT_OCCUPIED)) {
      *out_index = index;
      *out_found_empty = true;
      *out_first_deleted = first_deleted;
      return;
    }

    if ((map->slots[index].metadata & SLOT_DELETED) &&
        first_deleted == map->capacity) {
      first_deleted = index;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (index != start_index);

  *out_index = map->capacity;
  *out_found_empty = false;
  *out_first_deleted = first_deleted;
}

/* Inserts or updates a key-value pair using linear probing. Deleted slots
 * encountered during probing are reused so they don't accumulate without
 * bound.
 *
 * The table is probed for an existing entry FIRST, entirely without
 * considering growth: this way, a pure value update for an already-present
 * key (which never changes elem_count) can never trigger a rehash, and
 * therefore can never invalidate any OTHER key's already-held
 * chmap_get_elem_ref pointer, purely as a side effect of the map happening
 * to sit above its growth threshold from unrelated prior insertions. Only
 * once the key is confirmed genuinely absent does this function consider
 * the load factor (count + deleted) / capacity; exceeding
 * OPEN_ADDR_MAX_LOAD_FACTOR then triggers a 2x rehash, after which a fresh
 * probe (oa_find_insertion_slot) locates the insertion point against the
 * grown table, since growing invalidates the index/capacity the first probe
 * computed against (the key's absence does not need re-verifying: a rehash
 * only ever relocates already-live entries, it cannot introduce this key).
 * For the common case of a plain update, or a plain new-key insert that
 * does not happen to cross the growth threshold, this costs exactly one
 * probe pass, identical to a design that checks growth unconditionally up
 * front.
 *
 * If growth is needed but oa_rehash() fails (allocation failure) or is
 * skipped because the table is already at its architectural maximum
 * capacity, insertion still proceeds against the table's current size; if
 * that ultimately finds no room at all, the two cases are reported with
 * distinct, honest return codes (see the function's own final lines)
 * instead of both collapsing into ccol_container_full.
 *
 * Callers (chmap_insert_elem) have already rejected any key_pair/val_pair
 * whose size does not exactly match map->key_size/map->val_size, so both
 * mem_cpy calls below are always bounded by the 8-byte key_data/val_data
 * fields they target; nothing here can spill into a neighbouring slot. */
static ccol_retval_t oa_insert(open_addr_map* map, const cmap_pair* key_pair,
                               const cmap_pair* val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val & (map->capacity - 1);
  size_t start_index = index;
  size_t first_deleted = map->capacity;
  bool found_empty = false;

  // Prefetch first location
  __builtin_prefetch(&map->slots[index], 1, 1);

  do {
    // Prefetch next likely location
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 1, 1);

    if (!(map->slots[index].metadata & SLOT_OCCUPIED)) {
      found_empty = true;
      break;
    }

    if ((map->slots[index].metadata & SLOT_DELETED) &&
        first_deleted == map->capacity) {
      first_deleted = index;
    }

    if (!(map->slots[index].metadata & SLOT_DELETED) &&
        oa_keys_equal(&map->slots[index], key_pair->ptr, key_pair->size)) {
      // val_pair->ptr may alias this slot's own val_data (e.g. a caller
      // re-inserting a value derived from a pointer previously obtained via
      // chmap_get_elem_ref/chmap_get_ptr for this exact key, through the raw
      // chmap_insert_elem function layer). mem_cpy's underlying memcpy
      // requires src/dst to never overlap; snapshot into a small stack
      // buffer first when they do (val_pair->size is always <= 8 bytes for
      // this backend), mirroring sc_reset_val_of_llist_node's identical
      // aliasing protection for the separate-chaining backend.
      const void* src = val_pair->ptr;
      uint64_t snapshot;
      if (ranges_overlap(src, val_pair->size, &map->slots[index].val_data,
                         map->val_size)) {
        mem_cpy(&snapshot, src, val_pair->size);
        src = &snapshot;
      }
      mem_cpy(&map->slots[index].val_data, src, val_pair->size);
      return ccol_key_already_present;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (index != start_index);

  // The key genuinely does not exist: this is where sc_insert's own
  // max_elem_count check lives too (see its own comment for why the
  // existing-key lookup must run first), stated explicitly here for the
  // same reason rather than left to fall out only implicitly from capacity
  // being physically capped at max_power_of_two_size_t (== max_elem_count):
  // a table already at that architectural limit, fully occupied with no
  // reusable tombstone, would otherwise reach the exact same
  // ccol_container_full outcome several lines further down, but only after
  // needlessly computing a load factor and confirming growth is impossible
  // first.
  if (map->count == max_elem_count) {
    return ccol_container_full;
  }

  // Only now is it safe to consider an opportunistic grow-on-insert
  // rehash, since this call really is going to add a new entry.
  bool rehash_oom_failed = false;
  double load_factor =
      (double)(map->count + map->deleted_count) / map->capacity;
  if (load_factor > OPEN_ADDR_MAX_LOAD_FACTOR &&
      map->capacity < max_power_of_two_size_t) {
    if (oa_rehash(map, map->capacity * 2) == ccol_success) {
      // Capacity (and therefore the index mask) changed underneath the
      // probe above; the slot(s) it located are meaningless now, so a fresh
      // probe against the grown table is required.
      oa_find_insertion_slot(map, hash_val, &index, &found_empty,
                             &first_deleted);
    } else {
      rehash_oom_failed = true;
    }
  }

  if (found_empty) {
    if (first_deleted < map->capacity) {
      index = first_deleted;
      map->deleted_count--;
    }

    mem_zero(&map->slots[index].key_data, sizeof(uint64_t));
    mem_zero(&map->slots[index].val_data, sizeof(uint64_t));
    mem_cpy(&map->slots[index].key_data, key_pair->ptr, key_pair->size);
    mem_cpy(&map->slots[index].val_data, val_pair->ptr, val_pair->size);
    map->slots[index].metadata = SLOT_OCCUPIED;
    map->val_accessors[index].ptr = &map->slots[index].val_data;
    map->val_accessors[index].size = map->val_size;
    map->count++;
    return ccol_success;
  }

  if (first_deleted < map->capacity) {
    map->deleted_count--;
    mem_zero(&map->slots[first_deleted].key_data, sizeof(uint64_t));
    mem_zero(&map->slots[first_deleted].val_data, sizeof(uint64_t));
    mem_cpy(&map->slots[first_deleted].key_data, key_pair->ptr, key_pair->size);
    mem_cpy(&map->slots[first_deleted].val_data, val_pair->ptr, val_pair->size);
    map->slots[first_deleted].metadata = SLOT_OCCUPIED;
    map->val_accessors[first_deleted].ptr = &map->slots[first_deleted].val_data;
    map->val_accessors[first_deleted].size = map->val_size;
    map->count++;
    return ccol_success;
  }

  // No room anywhere in the table: distinguish a genuine, architectural
  // capacity limit (growth was never even attempted, since the table was
  // already at max_power_of_two_size_t) from growth having been needed but
  // failing due to an allocation failure, which is a resource-exhaustion
  // condition, not "this map has reached its real element cap".
  return rehash_oom_failed ? ccol_not_enough_memory : ccol_container_full;
}

/* Looks up key_pair using linear probing. An empty slot (no OCCUPIED or DELETED
 * bit) terminates the search immediately; this is safe because insertions
 * never leave a gap between a key and its probe chain. Returns a pointer to
 * this slot's own entry in map->val_accessors: a stable, permanently-owned
 * cmap_pair distinct from every other slot's, populated once by oa_insert/
 * oa_rehash and never mutated by oa_get itself. Unlike a single shared
 * scratch field, this means two (or more) chmap_get_elem_ref results held
 * concurrently for different keys never alias one another; each remains
 * valid, per this function's own documented contract, until the map is
 * actually modified (insert/delete/resize), not merely until the next get. */
static ccol_retval_t oa_get(open_addr_map* map, const cmap_pair* key_pair,
                            cmap_pair** val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val & (map->capacity - 1);
  size_t start_index = index;

  // Prefetch first location
  __builtin_prefetch(&map->slots[index], 0, 1);

  do {
    // Prefetch next likely location
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 0, 1);

    if (!(map->slots[index].metadata & SLOT_OCCUPIED) &&
        !(map->slots[index].metadata & SLOT_DELETED)) {
      return ccol_key_not_found;
    }

    if ((map->slots[index].metadata & SLOT_OCCUPIED) &&
        !(map->slots[index].metadata & SLOT_DELETED) &&
        oa_keys_equal(&map->slots[index], key_pair->ptr, key_pair->size)) {
      *val_pair = &map->val_accessors[index];
      return ccol_success;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (index != start_index);

  return ccol_key_not_found;
}

/* Marks the matching slot DELETED (tombstone) rather than clearing it, so that
 * probe chains through the slot remain intact. If the load factor after
 * deletion falls below OPEN_ADDR_MIN_LOAD_FACTOR the table is halved. */
static ccol_retval_t oa_delete(open_addr_map* map, const cmap_pair* key_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc);
  size_t index = hash_val & (map->capacity - 1);
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

    index = (index + 1) & (map->capacity - 1);
  } while (index != start_index);

  return ccol_key_not_found;
}

/* Frees the slot array and the map struct. Does not free the m_procs pointer
 * itself; that is done by the unified __chmap_destroy since the same procs
 * copy is shared between the map struct and its fields. */
static void oa_destroy(open_addr_map* map) {
  if (map) {
    _mem_free(map->m_procs, map->slots);
    _mem_free(map->m_procs, map->val_accessors);
    _mem_free(map->m_procs, map);
  }
}

/* Zeroes every slot's metadata byte and every accessor entry in place, at
 * the map's current capacity, without allocating anything. The
 * open-addressing backend never heap-allocates per key/value (every key and
 * value lives inline in the slot array itself), so clearing the metadata
 * (dropping both SLOT_OCCUPIED and SLOT_DELETED on every slot) is a
 * complete, correct "destroy every element" for this backend on its own -
 * there is nothing else to free. Used by oa_reset() so that a failure to
 * allocate the requested new capacity still destroys every existing element
 * rather than leaving the old table (and all its data) completely
 * untouched. */
static void oa_clear_in_place(open_addr_map* map) {
  mem_zero(map->slots, map->capacity * sizeof(oa_slot));
  mem_zero(map->val_accessors, map->capacity * sizeof(cmap_pair));
  map->count = 0;
  map->deleted_count = 0;
}

/* Clears all entries and, if new_capacity differs from the current capacity,
 * replaces the slot array with a freshly zeroed one of that size. If
 * new_capacity is 0, or equals the map's current capacity, the existing
 * slot/val_accessors arrays are cleared in place with no allocation at all,
 * mirroring sc_reset()'s own "skip the realloc when the size doesn't
 * change" behavior.
 *
 * Every element is destroyed regardless of the return value, matching
 * chmap_reset's documented contract: if either allocation needed to honor
 * new_capacity fails, this falls back to oa_clear_in_place() against the
 * map's current (unchanged) capacity instead of returning with the old
 * table, and every element still inside it, left completely intact. */
static ccol_retval_t oa_reset(open_addr_map* map, size_t new_capacity) {
  if (new_capacity == 0 || new_capacity == map->capacity) {
    oa_clear_in_place(map);
    return ccol_success;
  }

  oa_slot* new_slots =
      (oa_slot*)_mem_calloc(map->m_procs, new_capacity, sizeof(oa_slot));
  if (!new_slots) {
    oa_clear_in_place(map);
    return ccol_not_enough_memory;
  }

  cmap_pair* new_val_accessors =
      (cmap_pair*)_mem_calloc(map->m_procs, new_capacity, sizeof(cmap_pair));
  if (!new_val_accessors) {
    _mem_free(map->m_procs, new_slots);
    oa_clear_in_place(map);
    return ccol_not_enough_memory;
  }

  _mem_free(map->m_procs, map->slots);
  map->slots = new_slots;
  _mem_free(map->m_procs, map->val_accessors);
  map->val_accessors = new_val_accessors;

  map->capacity = new_capacity;
  map->count = 0;
  map->deleted_count = 0;

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
 * and value are stored inline (SSO: <= 23 bytes) or in a separate heap buffer
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

/* Compares two long double keys by VALUE, matching hash_long_double_value's
 * own special-casing exactly so hash and equality never disagree with each
 * other for the same pair of keys:
 * - If either operand is NaN, the pair is equal only when BOTH are NaN;
 *   every NaN long double collapses into one key, and no padding byte is
 *   ever read to decide this (see hash_long_double_value's own comment for
 *   why: a NaN long double's padding is routinely genuine uninitialized
 *   memory, not merely unspecified-but-stable content, so comparing it at
 *   all - even via memcmp - is itself a real hazard, not just a source of
 *   non-determinism).
 * - Otherwise, native `==` is used: value-based, so unspecified padding
 *   bits never affect the result (unlike a raw memcmp of the full
 *   representation), and -0.0L/0.0L compare equal exactly like -0.0/0.0
 *   already do for the float/double key types. */
static inline bool long_double_keys_equal(const void* a_ptr,
                                          const void* b_ptr) {
  long double a, b;
  mem_cpy(&a, a_ptr, sizeof(a));
  mem_cpy(&b, b_ptr, sizeof(b));

  bool a_nan = isnan(a);
  bool b_nan = isnan(b);
  if (a_nan || b_nan) {
    return a_nan && b_nan;
  }

  return a == b;
}

/* Compares a node's key against key_ptr. The size check is a fast-reject;
 * for 4- and 8-byte keys integer comparison is used instead of memcmp to
 * allow the compiler to emit a single load+compare instruction. key_type
 * is consulted only to route a long double key through
 * long_double_keys_equal instead of the generic byte-exact paths below -
 * this must happen before the size-based dispatch, since sizeof(long
 * double) coincides with sizeof(double) on some platforms/ABIs, and a
 * long double key must never fall into the plain 8-byte integer-compare
 * branch on those platforms. */
static inline bool sc_compare_keys(const llist_node* node, const void* key_ptr,
                                   size_t key_size, ccol_data_type key_type) {
  if (node->data.key_size != key_size) return false;

  const void* node_key_ptr =
      node->data.key_is_inline
          ? (const void*)&node->data.key_storage.inline_data
          : (const void*)node->data.key_storage.ptr;

  if (key_type == ccol_long_double) {
    return long_double_keys_equal(node_key_ptr, key_ptr);
  }

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

/* Linear search through a bucket's singly-linked chain. hash_val is the
 * caller's full (pre-modulo) hash of key_ptr; comparing it against each
 * node's stored hash_val first turns most rejections into a single size_t
 * comparison, only falling through to sc_compare_keys' memcmp/strcmp when
 * the hashes actually collide. Returns the matching node or NULL. Chains are
 * expected to be short (O(1) average) due to the bucket scaling strategy. */
static llist_node* sc_find_in_llist(llist_node* head, size_t hash_val,
                                    const void* key_ptr, size_t key_size,
                                    ccol_data_type key_type) {
  llist_node* tracker = head;
  while (tracker) {
    if (tracker->data.hash_val == hash_val &&
        sc_compare_keys(tracker, key_ptr, key_size, key_type)) {
      return tracker;
    }
    tracker = tracker->next;
  }
  return NULL;
}

/* Updates the value stored in an existing node, handling three cases based on
 * the new value size vs. the stored size: same size (overwrite in place),
 * smaller and fits inline (switch to inline storage and free old heap buffer),
 * or larger (realloc or allocate new heap buffer).
 *
 * val_ptr may alias this entry's own current value storage: a caller is free
 * to re-insert a value derived from a pointer it obtained via
 * chmap_get_elem_ref/chmap_get_ptr/chmap_get for this exact key (chmap_get/
 * chmap_get_ptr return a pointer straight into a separate-chaining entry's
 * own stored bytes for a char* value type, per their own documented "for
 * strings, returns the char* itself"/"pointer to the char* itself"
 * contracts). Every branch below mutates the entry's existing storage -
 * frees the heap buffer, overwrites the val_storage union in place, or
 * reallocs the heap buffer - before it would otherwise read val_ptr's bytes;
 * an aliased val_ptr would then observe freed, corrupted, or moved memory
 * instead of the caller's intended value. The overlap check below runs once
 * per call and is cheap; the snapshot copy itself only runs on the rare
 * aliasing path, so the common (non-aliasing) case pays only the
 * comparison. */
static bool sc_reset_val_of_llist_node(llist_node* elem, const void* val_ptr,
                                       size_t val_size) {
  const void* old_ptr = elem->data.val_is_inline
                            ? (const void*)&elem->data.val_storage.inline_data
                            : (const void*)elem->data.val_storage.ptr;

  unsigned char snapshot_buf[INLINE_STORAGE_THRESHOLD + 1];
  void* heap_snapshot = NULL;
  if (ranges_overlap(val_ptr, val_size, old_ptr, elem->data.val_size)) {
    if (val_size <= sizeof(snapshot_buf)) {
      memcpy(snapshot_buf, val_ptr, val_size);
      val_ptr = snapshot_buf;
    } else {
      heap_snapshot = _mem_alloc(elem->m_procs, val_size);
      if (!heap_snapshot) return false;
      memcpy(heap_snapshot, val_ptr, val_size);
      val_ptr = heap_snapshot;
    }
  }

  bool ok = true;
  if (val_size == elem->data.val_size) {
    if (elem->data.val_is_inline) {
      mem_cpy(&elem->data.val_storage.inline_data, val_ptr, val_size);
    } else {
      mem_cpy(elem->data.val_storage.ptr, val_ptr, val_size);
    }
  } else if (val_size <= INLINE_STORAGE_THRESHOLD) {
    if (!elem->data.val_is_inline) {
      _mem_free(elem->m_procs, elem->data.val_storage.ptr);
    }
    elem->data.val_is_inline = true;
    elem->data.val_size = val_size;
    mem_cpy(&elem->data.val_storage.inline_data, val_ptr, val_size);
    elem->val_pair_accessor.ptr = &elem->data.val_storage.inline_data;
    elem->val_pair_accessor.size = val_size;
  } else {
    if (elem->data.val_is_inline) {
      void* new_ptr = _mem_alloc(elem->m_procs, val_size);
      if (!new_ptr) {
        ok = false;
      } else {
        elem->data.val_storage.ptr = new_ptr;
        elem->data.val_is_inline = false;
      }
    } else {
      void* orig = elem->data.val_storage.ptr;
      elem->data.val_storage.ptr =
          _mem_realloc(elem->m_procs, elem->data.val_storage.ptr, val_size);
      if (!elem->data.val_storage.ptr) {
        elem->data.val_storage.ptr = orig;
        ok = false;
      }
    }

    if (ok) {
      mem_cpy(elem->data.val_storage.ptr, val_ptr, val_size);
      elem->data.val_size = val_size;
      elem->val_pair_accessor.ptr = elem->data.val_storage.ptr;
      elem->val_pair_accessor.size = val_size;
    }
  }

  if (heap_snapshot) {
    _mem_free(elem->m_procs, heap_snapshot);
  }
  return ok;
}

/* Removes the node matching key_ptr from a bucket's chain, sets *found, and
 * returns the updated chain head. hash_val lets the search reject
 * non-matching nodes via a size_t comparison before falling back to
 * sc_compare_keys, same as sc_find_in_llist. The previous-pointer tracking
 * enables O(n) deletion without a doubly-linked bucket list. */
static llist_node* sc_delete_from_llist(llist_node* head,
                                        dllist_ref_node** head_of_all_elems,
                                        size_t hash_val, const void* key_ptr,
                                        size_t key_size,
                                        ccol_data_type key_type, bool* found) {
  *found = false;
  llist_node* tracker = head;
  llist_node* previous = NULL;

  while (tracker) {
    if (tracker->data.hash_val == hash_val &&
        sc_compare_keys(tracker, key_ptr, key_size, key_type)) {
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
 * to keep the thresholds consistent with the new capacity.
 *
 * Matches chashmap.h's documented "(bucket_count + 1) * 1.5" / "(bucket_count
 * + 1) / 8" formulas exactly, computed without ever overflowing size_t: the
 * scale-up threshold is floor(3 * bucket_arr_size / 2) (the same
 * bucket_arr_size + bucket_arr_size / 2 expression used below, which cannot
 * overflow since bucket_arr_size is capped at max_power_of_two_size_t) plus a
 * parity-dependent +1 (even bucket_arr_size) or +2 (odd) correction, derived
 * algebraically from floor((n+1)*3/2) in terms of floor(3n/2) rather than by
 * computing (bucket_arr_size + 1) * 3 directly, which would overflow size_t
 * at that same upper bound. The scale-down threshold has no such risk, since
 * bucket_arr_size + 1 alone never overflows. */
static void sc_set_scaling_limits(sep_chain_map* map) {
  size_t base_up = map->bucket_arr_size + map->bucket_arr_size / 2;
  map->elem_count_to_scale_up =
      base_up + ((map->bucket_arr_size % 2 == 0) ? 1 : 2);
  map->elem_count_to_scale_down = (map->bucket_arr_size + 1) / 8;
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

  // Dividing by scale_factor (4x) can undershoot the minimum bucket array
  // size for any bucket_arr_size that isn't on the "minimum_allowed_
  // bucket_array_size * 4^k" lineage - e.g. 32, directly reachable from
  // chmap_create_full/chmap_reset with any requested size in
  // (minimum_allowed_bucket_array_size, minimum_allowed_bucket_array_size *
  // scale_factor): 32 / 4 == 8, below the floor. sc_delete's caller only
  // guarantees bucket_arr_size > minimum_allowed_bucket_array_size before
  // calling this, not that it divides down cleanly, so clamp here rather
  // than letting an off-lineage table under-shoot the floor.
  if (!up && new_size < minimum_allowed_bucket_array_size) {
    new_size = minimum_allowed_bucket_array_size;
  }

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
 *
 * The existing-key lookup runs before the max_elem_count check, not after
 * it: the fullness check only makes sense for "insert a genuinely new
 * key", since an update of an already-present key never changes
 * elem_count at all. Checking fullness first would mean a map that had,
 * at some point, actually reached max_elem_count incorrectly rejects a
 * plain value update for a key it already holds with ccol_container_full
 * instead of updating it.
 */
static ccol_retval_t sc_insert(sep_chain_map* map, const cmap_pair* key_pair,
                               const cmap_pair* val_pair) {
  chmap_entry data = {
      .hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                map->custom_hashing_proc),
      .key_size = key_pair->size,
      .val_size = val_pair->size,
      .m_procs = map->m_procs};

  size_t index = data.hash_val & (map->bucket_arr_size - 1);

  llist_node* existing =
      sc_find_in_llist(map->bucket_arr[index], data.hash_val, key_pair->ptr,
                       key_pair->size, map->key_type);
  if (existing) {
    return sc_reset_val_of_llist_node(existing, val_pair->ptr, val_pair->size)
               ? ccol_key_already_present
               : ccol_not_enough_memory;
  }

  if (map->elem_count == max_elem_count) {
    return ccol_container_full;
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
      sc_find_in_llist(map->bucket_arr[index], hash_val, key_pair->ptr,
                       key_pair->size, map->key_type);
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
  map->bucket_arr[index] = sc_delete_from_llist(
      map->bucket_arr[index], &map->head_of_all_elems, hash_val, key_pair->ptr,
      key_pair->size, map->key_type, &found);

  if (found) {
    if (--map->elem_count < map->elem_count_to_scale_down &&
        map->bucket_arr_size > minimum_allowed_bucket_array_size) {
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
    map->bucket_arr[i] =
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

/* Returns the key type this map was created with, regardless of backend. */
static inline ccol_data_type chmap_key_type(chmap chm) {
  return chm->impl_type == IMPL_OPEN_ADDRESSING ? chm->impl.oa_map->key_type
                                                : chm->impl.sc_map->key_type;
}

/* The map's key equality is otherwise bitwise (see oa_keys_equal /
 * sc_compare_keys), which would let +0.0 and -0.0 hash to different
 * buckets and compare unequal despite `0.0 == -0.0` in C; that is
 * surprising for a float/double key type where every other numeric key
 * type's bitwise equality already coincides with its C equality. If
 * key_type is ccol_float/ccol_double, copies key_pair into
 * out_canon_pair/out_canon_buf with a negative-zero bit pattern normalized
 * to positive zero, and returns out_canon_pair; otherwise returns key_pair
 * unchanged. Every other bit pattern (including the various NaN payloads,
 * which are never equal to anything under `==`, not even themselves) is
 * left untouched: this map's key equality for floating-point keys is
 * bitwise equality with signed zero collapsed, not IEEE-754 equality.
 * out_canon_buf must be at least key_pair->size bytes; since
 * ccol_float/ccol_double are always <= 8 bytes, a uint64_t satisfies this
 * for every key this function ever canonicalizes. */
static inline const cmap_pair* canonicalize_key_pair_if_needed(
    const cmap_pair* key_pair, ccol_data_type key_type, uint64_t* out_canon_buf,
    cmap_pair* out_canon_pair) {
  if ((key_type != ccol_float && key_type != ccol_double) ||
      key_pair->size > sizeof(*out_canon_buf)) {
    return key_pair;
  }

  *out_canon_buf = 0;
  mem_cpy(out_canon_buf, key_pair->ptr, key_pair->size);

  if (key_type == ccol_float) {
    uint32_t bits;
    mem_cpy(&bits, out_canon_buf, sizeof(bits));
    if (bits == 0x80000000u) {
      mem_zero(out_canon_buf, sizeof(bits));
    }
  } else if (*out_canon_buf == 0x8000000000000000ULL) {
    *out_canon_buf = 0;
  }

  out_canon_pair->ptr = out_canon_buf;
  out_canon_pair->size = key_pair->size;
  return out_canon_pair;
}

/* The open-addressing backend stores a key/value pair inline in a slot's
 * fixed 8-byte key_data/val_data fields (see oa_slot); oa_insert's mem_cpy
 * calls trust key_pair->size/val_pair->size completely and have no bounds
 * check of their own. A caller-supplied size that does not match what this
 * particular map was created for (map->key_size/map->val_size, both fixed
 * from key_type/val_type at creation and never <= 8 bytes for anything else
 * than the open-addressing backend to begin with) would silently overwrite
 * the neighbouring slot's own key/value bytes, corrupting or losing an
 * unrelated, already-stored entry, or overflow the slot array outright when
 * the target slot sits near its end. Rejecting a mismatched size up front
 * is what makes that structurally unreachable rather than merely unlikely. */
static inline bool oa_val_size_matches(const open_addr_map* map,
                                       size_t val_size) {
  return val_size == map->val_size;
}

/* Whether key_size is safe to hand to hash_key_data() and, for a float/
 * double/long double key_type, to canonicalize_key_pair_if_needed() /
 * hash_long_double_value() / long_double_keys_equal(). All of those
 * functions dispatch on key_type and read a FIXED number of bytes -
 * sizeof() of the corresponding C type - straight out of the caller-
 * supplied pointer for every type is_type_integral() recognises (char/
 * short/int/long/long long, their unsigned counterparts, float, double,
 * pointer) plus ccol_long_double (deliberately checked here even though
 * is_type_integral() itself excludes it for backend-selection purposes -
 * see should_use_open_addressing - since long double keys, forced onto
 * separate chaining, are still hashed/compared by a fixed-size read of
 * sizeof(long double), the same hazard class as every other fixed-width
 * type below); the key_size parameter is only ever actually consulted for
 * a genuinely non-fixed-width key type (ccol_string, ccol_other_types,
 * ...), which is always hashed via xxhash64_buffer using exactly the
 * caller's own key_size.
 *
 * This check therefore has to run for BOTH backends whenever key_type is
 * fixed-width, not just open-addressing: a caller supplying a cmap_pair
 * through the raw chmap_insert_elem/_get_elem_ref/_delete_elem layer with
 * a key_size smaller than the type's true size would otherwise make
 * hash_key_data()/canonicalize_key_pair_if_needed()/
 * hash_long_double_value()/long_double_keys_equal() read past the end of
 * that caller's own buffer. This is reachable on the separate-chaining
 * backend any time a fixed-width key type is paired with a non-integral or
 * >8-byte value type (which forces separate chaining regardless of the key
 * type itself, e.g. int->char* or double->char*), or, for long double
 * specifically, unconditionally (long double always forces separate
 * chaining on its own), not merely a theoretical open-addressing-only
 * concern. The open-addressing backend always has an is_type_integral()
 * key_type by construction (see should_use_open_addressing), so this one
 * check also covers the pre-existing "don't let a mismatched size corrupt
 * a neighbouring slot" hazard a narrower, open-addressing-only version of
 * this check used to guard on its own. */
static inline bool key_size_matches_type_if_fixed_width(ccol_data_type key_type,
                                                        size_t key_size) {
  bool is_fixed_width =
      is_type_integral(key_type) || key_type == ccol_long_double;
  return !is_fixed_width || key_size == get_type_size(key_type);
}

/* Public insert/update dispatch: validates inputs then delegates to the
 * backend-specific insert function. */
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair) {
  if (!chm || !key_pair || !val_pair || !key_pair->ptr || !val_pair->ptr ||
      key_pair->size == 0 || val_pair->size == 0) {
    return ccol_invalid_args;
  }

  ccol_data_type key_type = chmap_key_type(chm);
  if (!key_size_matches_type_if_fixed_width(key_type, key_pair->size)) {
    return ccol_invalid_args;
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING &&
      !oa_val_size_matches(chm->impl.oa_map, val_pair->size)) {
    return ccol_invalid_args;
  }

  uint64_t canon_buf;
  cmap_pair canon_pair;
  key_pair = canonicalize_key_pair_if_needed(key_pair, key_type, &canon_buf,
                                             &canon_pair);

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

  ccol_data_type key_type = chmap_key_type(chm);
  if (!key_size_matches_type_if_fixed_width(key_type, key_pair->size)) {
    return ccol_invalid_args;
  }

  uint64_t canon_buf;
  cmap_pair canon_pair;
  key_pair = canonicalize_key_pair_if_needed(key_pair, key_type, &canon_buf,
                                             &canon_pair);

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

  ccol_data_type key_type = chmap_key_type(chm);
  if (!key_size_matches_type_if_fixed_width(key_type, key_pair->size)) {
    return ccol_invalid_args;
  }

  uint64_t canon_buf;
  cmap_pair canon_pair;
  key_pair = canonicalize_key_pair_if_needed(key_pair, key_type, &canon_buf,
                                             &canon_pair);

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

  bool requested_size_too_big = false;
  if (new_bucket_array_size > 0 &&
      new_bucket_array_size < minimum_allowed_bucket_array_size) {
    new_bucket_array_size = minimum_allowed_bucket_array_size;
  } else if (new_bucket_array_size > 0) {
    new_bucket_array_size =
        find_nearest_gte_power_of_two(new_bucket_array_size);
    if (new_bucket_array_size > max_elem_count) {
      // The requested size is too big: fall back to "keep the current
      // capacity" (0) rather than skipping the reset entirely, so this
      // failure path still honors the documented "all elements are
      // destroyed regardless of return value" contract instead of
      // silently leaving every existing element in place.
      new_bucket_array_size = 0;
      requested_size_too_big = true;
    }
  }

  ccol_retval_t r;
  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    r = oa_reset(chm->impl.oa_map, new_bucket_array_size);
  } else {
    r = sc_reset(chm->impl.sc_map, new_bucket_array_size);
  }

  if (requested_size_too_big) {
    return ccol_not_enough_memory;
  }
  return r;
}

/* Creates an iterator positioned at the first element. For separate-chaining
 * maps, iteration follows the insertion-order dllist. For open-addressing maps,
 * the first occupied non-deleted slot is found by linear scan. Returns NULL
 * for an empty map. The iterator heap-allocates chmap_cmap_iterator. */
static cmap_iterator* chmap_iter_next(cmap_iterator* iter);

#ifdef RUNNING_UNIT_TESTS
/* White-box regression guard: tracks the NET count of chashmap_cmap_iterator
 * allocations made by chashmap_begin_iter() that have not yet been released
 * via __chmap_iterator_destroy(), across every chmap in the process. Added
 * (2026-09-03) after a `make memtest` run of tests/ctls under heavy
 * concurrent system load reported one such iterator as "possibly lost"
 * (valgrind's own classification for an interior-pointer live reference,
 * not a genuine leak in itself; see chashmap_begin_iter()'s own
 * return-a-pointer-to-an-embedded-field shape) and the finding could not be
 * reproduced afterward across 35 further runs (10 native, 25 under
 * valgrind, several deliberately concurrent) once this exact counter was
 * checked directly: never once found nonzero at the one call site under
 * suspicion (tests/ctls/tests.c's own
 * ctls_sni.cert_add_race_during_live_handshake_does_not_crash, via
 * ctls_ctx_cert_add -> _ctls_ctx_rebuild_locked, which reads/frees this
 * exact iterator type while ctx->lock is held for the call's entire
 * duration, confirmed by direct code tracing). Kept as a permanent,
 * always-available accessor (not deleted once that investigation
 * concluded) so any FUTURE genuine regression in ANY chashmap iterator's
 * own alloc/free balance, anywhere in the process, is caught by a loud,
 * immediate abort() the moment it happens (see
 * tests/ctls/tests.c's own _check_chmap_iter_balance_at_exit) rather than
 * relying on a rare, hard-to-reproduce valgrind report alone. */
static long g_chmap_iter_outstanding_for_tests = 0;
long chashmap_iter_outstanding_count_for_tests(void) {
  return __atomic_load_n(&g_chmap_iter_outstanding_for_tests, __ATOMIC_SEQ_CST);
}
#endif /* RUNNING_UNIT_TESTS */

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

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif /* RUNNING_UNIT_TESTS */
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

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif /* RUNNING_UNIT_TESTS */
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
#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_sub(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif /* RUNNING_UNIT_TESTS */
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

/* See this function's own doc comment in chashmap.h: identical to
 * __chmap_destroy() above, except a destructor callback is threaded into
 * the same underlying, already-allocation-free walk each backend's own
 * oa_destroy()/sc_destroy() would otherwise perform silently. The two
 * walks below are deliberately NOT extracted into oa_destroy()/
 * sc_destroy() themselves (which stay exactly as they were): duplicating
 * the loop here keeps this function's own only-when-val_dtor-is-non-NULL
 * cost isolated from the hot, no-destructor path every other map user
 * still takes. */
void chmap_destroy_with_dtor(chmap chm,
                             void (*val_dtor)(cmap_pair* val_pair,
                                              void* dtor_ctx),
                             void* dtor_ctx) {
  if (!chm) return;
  if (!val_dtor) {
    __chmap_destroy(chm);
    return;
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    open_addr_map* map = chm->impl.oa_map;
    if (map) {
      for (size_t i = 0; i < map->capacity; i++) {
        if ((map->slots[i].metadata & SLOT_OCCUPIED) &&
            !(map->slots[i].metadata & SLOT_DELETED)) {
          cmap_pair vp = {.ptr = &map->slots[i].val_data,
                          .size = map->val_size};
          val_dtor(&vp, dtor_ctx);
        }
      }
    }
  } else {
    sep_chain_map* map = chm->impl.sc_map;
    if (map) {
      for (size_t i = 0; i < map->bucket_arr_size; i++) {
        for (llist_node* tracker = map->bucket_arr[i]; tracker;
             tracker = tracker->next) {
          val_dtor(&tracker->val_pair_accessor, dtor_ctx);
        }
      }
    }
  }

  __chmap_destroy(chm);
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
