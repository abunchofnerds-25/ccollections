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
#include <internal/chashinsert.h>
#include <internal/chashkey.h>
#include <internal/cpow2.h>
#include <internal/cprocsintern.h>
#include <internal/crandom.h>
#include <internal/test_helpers/chashmap.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// The smallest bucket or slot array of a map made by the public constructors,
// and the factor separate chaining grows and shrinks by. A map made by
// ccol_chmap_create_compact() starts at compact_bucket_array_size, which must
// be a power of two of at least 2 (see hash_index_for()).
static const size_t minimum_allowed_bucket_array_size = 16;
static const size_t compact_bucket_array_size = 4;
static const size_t scale_factor = 4;

// The loads at which an open-addressing table grows and shrinks, and the
// largest key or value that a separate-chaining node keeps inline.
#define OPEN_ADDR_MAX_LOAD_FACTOR 0.70
#define OPEN_ADDR_MIN_LOAD_FACTOR 0.25
#define INLINE_STORAGE_THRESHOLD 23

// The mixers and the reduction to an index come in a 64-bit and a 32-bit form.
#if SIZE_MAX != UINT64_MAX && SIZE_MAX != UINT32_MAX
#error "Unsupported architecture: SIZE_MAX is neither UINT32_MAX nor UINT64_MAX"
#endif

// The secret of the built-in hashes. There's one per process (see
// g_chmap_hash_secret), so no map stores any of it. sip_v is the SipHash-1-3
// state for the key (k0, k1), kept instead of the key so a hash skips the four
// xors; the keyed byte hash and the long double hash use it. int_seed keys the
// mixer of fixed-width keys and custom results in the keyed mode, and
// byte_seed seeds XXH64 in the fast mode.
typedef struct chmap_hash_secret {
  uint64_t sip_v[4];
  uint64_t int_seed;
  uint64_t byte_seed;
} chmap_hash_secret;

// An open-addressing slot: an 8-byte key and an 8-byte value, 16 bytes with no
// padding. The occupied bit lives in the metadata array instead, because a
// third member would pad the slot to 24 bytes and spread the bits a probe
// tests. The natural alignment lets chmap_get_ptr() hand out an aligned
// pointer into the slot.
typedef struct {
  uint64_t key_data;
  uint64_t val_data;
} oa_slot;

// The metadata bit of an occupied slot; 0 means empty.
#define SLOT_OCCUPIED 0x01

// The state of an open-addressing map. The fields up to val_type, with the hash
// mode and the window in the header, are what insert, lookup and delete read;
// the rest is colder.
typedef struct {
  oa_slot* slots;
  // One occupied byte per slot, at the tail of the slots block (see
  // oa_alloc_block()), so one cache line holds the bits of 64 slots.
  uint8_t* metadata;
  size_t capacity;
  // One more than the largest displacement in the table, at least
  // min(OA_DEEP_INSERT, capacity). Lookups, deletes and the backward-shift walk
  // stop after this many slots, which bounds a probe through a long run of keys
  // with consecutive home slots that no insert sees as displaced. Inserts widen
  // it and rebuilds set it; deletes never need to.
  size_t probe_span;
  size_t count;
  ccol_hashing_proc custom_hashing_proc;
  ccol_data_type key_type;
  ccol_data_type val_type;
  size_t key_size;
  size_t val_size;
  ccol_memmgmt_procs* m_procs;
} open_addr_map;

// The links of a node on the insertion-order list. Not packed, so it stays
// 8-byte aligned inside llist_node.
typedef struct dllist_ref_node {
  struct dllist_ref_node* prev;
  struct dllist_ref_node* next;
} dllist_ref_node;

// The ends of the list of all live entries: head is the newest and tail the
// oldest. The public iterator walks from head, the ordered cursor of
// chashinsert.h from tail.
typedef struct dllist_root {
  dllist_ref_node* head;
  dllist_ref_node* tail;
} dllist_root;

// One entry of a separate-chaining map. A key or value of at most
// INLINE_STORAGE_THRESHOLD bytes lives inline in its union, anything bigger in
// a heap buffer, and the size in the accessor alone says which; every store
// keeps that true. Each union is _Alignas(max_align_t) and the struct isn't
// packed, because the typed macros cast a pointer into inline_data to the
// caller's type (long double needs 16 bytes), just as they would for a malloc
// buffer. On LP64 the order leaves no padding, and hash_val and next, which a
// chain walk reads, share the first line with an inline key. The node keeps no
// allocator; its map's procs are passed in.
typedef struct llist_node {
  _Alignas(max_align_t) union {
    void* ptr;
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } key_storage;
  size_t hash_val;
  _Alignas(max_align_t) union {
    void* ptr;
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } val_storage;
  struct llist_node* next;
  // The stable {ptr, size} of the stored key and value that the map hands out.
  cmap_pair key_pair_accessor;
  cmap_pair val_pair_accessor;
  dllist_ref_node dllist_refs;
} llist_node;

// Pins the layout above on LP64 with a 16-byte max_align_t (x86-64, aarch64).
_Static_assert(SIZE_MAX != UINT64_MAX || _Alignof(max_align_t) != 16 ||
                   (sizeof(llist_node) == 112 &&
                    offsetof(llist_node, hash_val) == 24 &&
                    offsetof(llist_node, val_storage) == 32 &&
                    offsetof(llist_node, next) == 56),
               "llist_node must stay 112 bytes with no padding");

// The state of a separate-chaining map.
typedef struct sep_chain_map {
  size_t elem_count;
  size_t bucket_arr_size;
  size_t elem_count_to_scale_up;
  size_t elem_count_to_scale_down;
  llist_node** bucket_arr;
  dllist_root all_elems;
  ccol_memmgmt_procs* m_procs;
  ccol_hashing_proc custom_hashing_proc;
  // NULL selects the built-in comparison of sc_compare_keys().
  ccol_key_equality_proc custom_key_equality_proc;
  // ccol_data_type values, one byte each so they share the last word.
  uint8_t key_type;
  uint8_t val_type;
  // How many bucket pointers the map's block holds after struct chashmap, or 0;
  // see sc_inline_bucket_arr().
  uint8_t inline_buckets;
} sep_chain_map;

// The two backends.
typedef enum { IMPL_OPEN_ADDRESSING, IMPL_SEPARATE_CHAINING } hashmap_impl_type;

// A map is one allocation: this struct with its backend in state, then the
// first bucket array of a small separate-chaining map (see
// sc_inline_bucket_arr()), then the map's own procs copy when it needs one.
// Operations reach the backend through impl, which measured cheaper than
// fixed-offset addressing of state. The first eight bytes hold the backend,
// the hash mode and the insert window, which both backends reach at a constant
// offset back from their state (chmap_of_oa(), chmap_of_sc()).
struct chashmap {
  uint8_t impl_type;
  // CHMAP_HASH_FAST or CHMAP_HASH_KEYED.
  uint8_t hash_mode;
  // The bound and the running sum of the insert window; see chmap_window_add().
  uint16_t window_bound;
  uint32_t insert_window;
  union {
    open_addr_map* oa;
    sep_chain_map* sc;
  } impl;
  union {
    open_addr_map oa;
    sep_chain_map sc;
  } state;
};

// The hash modes. Every map starts fast and may switch to keyed, and each
// growth of a keyed map tries fast again. Fast is a Fibonacci multiply for a
// fixed-width key, XXH64 with a secret seed for bytes and the murmur3
// finalizer for a custom result; keyed is what ccol_chmap_hash_key() gives:
// hash_word_seeded() and SipHash-1-3. A long double is hashed by value with
// SipHash in both modes, and key identity never depends on the mode.
//
// The fast hashes are cheap and spread ordinary keys well, but they're public
// bijections (or, for XXH64, have collisions under every seed), so only the
// writers watch: every insert of a new key adds to the insert window, and a
// window of 256 whose walks pass oa_window_bound() or sc_window_bound(), or one
// insert past its cap, switches the map. A lookup never writes. The switch
// rebuilds an open-addressing table into a new block and rehashes chaining
// nodes in place. A failed fast attempt at growth stops early and fills the
// same new table keyed, so its work is linear in the count and geometric
// growth keeps it O(1) amortized per insert. A reset and a shrink keep the
// mode.
enum { CHMAP_HASH_FAST = 0, CHMAP_HASH_KEYED = 1 };

// The window starts at 2^31 and each insert of a new key adds 2^23 plus its
// walk, so 256 inserts carry out of 32 bits exactly when their walks sum to
// less than 2^23, and the 32 bits then hold that sum. A fast map never holds a
// walk above its cap, so its windows always end after 256 inserts.
#define CHMAP_WINDOW_START 0x80000000u
#define CHMAP_WINDOW_STEP 0x00800000u
#define CHMAP_WINDOW_INSERTS 256u

// A new separate-chaining key that meets this many nodes switches the map.
#define SC_CHAIN_CAP 20u

// An open-addressing insert displaced this far or more goes to
// oa_note_deep_insert(). It's also the smallest probe_span of a table this
// big or bigger.
#define OA_DEEP_INSERT 32u

// Give the map header that sits at a constant offset before a backend's state,
// so a backend function can reach the hash mode and the insert window without
// an extra pointer.
static inline struct chashmap* chmap_of_oa(open_addr_map* map) {
  return (struct chashmap*)((uint8_t*)map -
                            offsetof(struct chashmap, state.oa));
}

static inline struct chashmap* chmap_of_sc(sep_chain_map* map) {
  return (struct chashmap*)((uint8_t*)map -
                            offsetof(struct chashmap, state.sc));
}

// Tell whether a backend hashes in the keyed mode; every hash and every
// detection step branches on it.
static inline bool oa_keyed(open_addr_map* map) {
  return chmap_of_oa(map)->hash_mode == CHMAP_HASH_KEYED;
}

static inline bool sc_keyed(sep_chain_map* map) {
  return chmap_of_sc(map)->hash_mode == CHMAP_HASH_KEYED;
}

// Adds one insert of a new key with the given walk to the window, and gives
// true when that ends the window (a carry out of 32 bits).
static inline bool chmap_window_add(struct chashmap* chm, size_t walk) {
  uint32_t sum;
  bool ended = __builtin_add_overflow(chm->insert_window,
                                      CHMAP_WINDOW_STEP + (uint32_t)walk, &sum);
  chm->insert_window = sum;
  return ended;
}

// Gives the base-2 logarithm of a power of two; oa_insert_cap() and the window
// bounds need it.
static inline unsigned chmap_log2_pow2(size_t capacity) {
#if SIZE_MAX == UINT64_MAX
  return (unsigned)__builtin_ctzll((unsigned long long)capacity);
#else
  return (unsigned)__builtin_ctz((unsigned int)capacity);
#endif
}

// Gives count / capacity in 16.16 fixed point for a power-of-two capacity, so
// the window bounds need no floating point.
static inline uint64_t chmap_load_q16(size_t count, size_t capacity) {
  unsigned k = chmap_log2_pow2(capacity);
  return k >= 16 ? (uint64_t)count >> (k - 16) : (uint64_t)count << (16 - k);
}

// Tells whether a type is one of the types of at most 8 bytes that the integer
// mixer hashes (the integers, float, double, pointers; not long double). The
// backend choice at creation uses it.
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

// Gives the size of the C type behind a ccol_data_type, or 8 for an unknown
// type. It sizes the key and value of an open-addressing map, so a slot always
// fits a pointer.
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

// Tells whether a map with these key and value types uses open addressing
// (both integral) instead of separate chaining; creation calls it once.
static inline bool should_use_open_addressing(ccol_data_type key_type,
                                              ccol_data_type val_type) {
  return is_type_integral(key_type) && is_type_integral(val_type) &&
         get_type_size(key_type) <= 8 && get_type_size(val_type) <= 8;
}

// Hashes a fixed-width key or a custom hash result in the keyed mode: an xor
// with the secret seed, a multiply, a xorshift and a second multiply. It's
// what a map switches to when its keys collide under the fast hash.
// hash_index_for() reads the top bits, which the last multiply mixes best.
// Every step is a bijection. The xorshift between the multiplies turns the
// carries of the seeded first product into a nonlinear function of the seed,
// so no key set (sequential, or varying only in some fixed bits) clusters the
// same way for every seed, as it would under (k ^ s) * C. A 32-bit size_t uses
// the same steps with lowbias32 constants.
static inline size_t hash_word_seeded(size_t x, uint64_t seed) {
#if SIZE_MAX == UINT64_MAX
  uint64_t r = ((uint64_t)x ^ seed) * 0xBF58476D1CE4E5B9ULL;
  r ^= r >> 32;
  return (size_t)(r * 0x94D049BB133111EBULL);
#else
  uint32_t r = ((uint32_t)x ^ (uint32_t)seed) * 0x7FEB352DU;
  r ^= r >> 16;
  return (size_t)(r * 0x846CA68BU);
#endif
}

// Hashes a 64-bit key (long long, double) in the keyed mode. On a 32-bit
// target the 64-bit steps run at the key's width and the upper word is the
// hash; folding the halves together first would make every (a << 32) | b with
// the same a ^ b collide.
static inline size_t hash_u64_seeded(uint64_t x, uint64_t seed) {
#if SIZE_MAX == UINT64_MAX
  return hash_word_seeded((size_t)x, seed);
#else
  uint64_t r = (x ^ seed) * 0xBF58476D1CE4E5B9ULL;
  r ^= r >> 32;
  return (size_t)((r * 0x94D049BB133111EBULL) >> 32);
#endif
}

// Hashes a fixed-width key no wider than size_t in the fast mode, where every
// map starts: a Fibonacci multiply, read from the top bits. Sequential keys,
// scaled keys and aligned addresses spread perfectly. Nothing keys it, so
// whoever controls the keys can make them collide; the detection notices and
// switches the map.
static inline size_t hash_word_fast(size_t x) {
#if SIZE_MAX == UINT64_MAX
  return x * (size_t)0x9E3779B97F4A7C15ULL;
#else
  return x * (size_t)0x9E3779B9U;
#endif
}

// Hashes a 64-bit key in the fast mode; on a 32-bit target the hash is the
// upper word of the 64-bit product.
static inline size_t hash_u64_fast(uint64_t x) {
#if SIZE_MAX == UINT64_MAX
  return hash_word_fast((size_t)x);
#else
  return (size_t)((x * 0x9E3779B97F4A7C15ULL) >> 32);
#endif
}

// Finalizes a custom hash result in the fast mode with murmur3 fmix. A custom
// hash may keep its entropy in the low bits (an identity, a counter), and this
// carries it to the top bits that the index reads.
static inline size_t hash_custom_fast(size_t h) {
#if SIZE_MAX == UINT64_MAX
  uint64_t x = (uint64_t)h;
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDULL;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ULL;
  x ^= x >> 33;
  return (size_t)x;
#else
  uint32_t x = (uint32_t)h;
  x ^= x >> 16;
  x *= 0x85EBCA6BU;
  x ^= x >> 13;
  x *= 0xC2B2AE35U;
  x ^= x >> 16;
  return (size_t)x;
#endif
}

// Rotates a word left by b bits, for SipHash-1-3. SipHash-1-3 is the keyed
// byte hash of every key with no fixed width: a pseudorandom function of its
// 128-bit key, so without the key nobody finds colliding inputs better than by
// chance. Rust and Python use the same variant for their hash tables.
static inline uint64_t sip_rotl(uint64_t x, unsigned b) {
  return (x << b) | (x >> (64u - b));
}

// Runs one SipHash round on the four state words in place; siphash13() runs
// it once per input word and three times to finalize.
#define CHMAP_SIPROUND(v0, v1, v2, v3) \
  do {                                 \
    (v0) += (v1);                      \
    (v1) = sip_rotl((v1), 13);         \
    (v1) ^= (v0);                      \
    (v0) = sip_rotl((v0), 32);         \
    (v2) += (v3);                      \
    (v3) = sip_rotl((v3), 16);         \
    (v3) ^= (v2);                      \
    (v0) += (v3);                      \
    (v3) = sip_rotl((v3), 21);         \
    (v3) ^= (v0);                      \
    (v2) += (v1);                      \
    (v1) = sip_rotl((v1), 17);         \
    (v1) ^= (v2);                      \
    (v2) = sip_rotl((v2), 32);         \
  } while (0)

// Read 8 or 4 bytes as a little-endian word, the way SipHash defines its
// input, whatever the host's byte order and the pointer's alignment.
static inline uint64_t sip_load64(const unsigned char* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = __builtin_bswap64(v);
#endif
  return v;
}

static inline uint64_t sip_load32(const unsigned char* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  v = __builtin_bswap32(v);
#endif
  return v;
}

// Reads the last 0 to 7 bytes of a SipHash input as a little-endian word: two
// overlapping 4-byte reads for 4 to 7 bytes, the first, middle and last byte
// for 1 to 3. Bytes that two reads share are equal, so or-ing them is safe.
static inline uint64_t sip_load_tail(const unsigned char* p, size_t left) {
  if (left >= 4) {
    return sip_load32(p) | (sip_load32(p + left - 4) << (8u * (left - 4)));
  }
  if (left == 0) {
    return 0;
  }
  return (uint64_t)p[0] | ((uint64_t)p[left >> 1] << (8u * (left >> 1))) |
         ((uint64_t)p[left - 1] << (8u * (left - 1)));
}

// Computes SipHash-1-3 of len bytes from the initial state sip_v, for the
// keyed byte hash and the long double hash. Words are read through memcpy, so
// the input needs no alignment.
static inline __attribute__((always_inline)) uint64_t
siphash13(const uint64_t sip_v[4], const void* input, size_t len) {
  uint64_t v0 = sip_v[0];
  uint64_t v1 = sip_v[1];
  uint64_t v2 = sip_v[2];
  uint64_t v3 = sip_v[3];
  const unsigned char* p = (const unsigned char*)input;
  for (size_t blocks = len >> 3; blocks != 0; blocks--) {
    uint64_t m = sip_load64(p);
    v3 ^= m;
    CHMAP_SIPROUND(v0, v1, v2, v3);
    v0 ^= m;
    p += 8;
  }
  uint64_t b = ((uint64_t)len << 56) | sip_load_tail(p, len & 7u);
  v3 ^= b;
  CHMAP_SIPROUND(v0, v1, v2, v3);
  v0 ^= b;
  v2 ^= 0xff;
  CHMAP_SIPROUND(v0, v1, v2, v3);
  CHMAP_SIPROUND(v0, v1, v2, v3);
  CHMAP_SIPROUND(v0, v1, v2, v3);
  return v0 ^ v1 ^ v2 ^ v3;
}

// Hashes a buffer with SipHash-1-3 and folds the result into a size_t; a
// 32-bit size_t keeps both halves.
static inline size_t hash_bytes_keyed(const uint64_t sip_v[4],
                                      const void* input, size_t len) {
  uint64_t h = siphash13(sip_v, input, len);
#if SIZE_MAX == UINT64_MAX
  return (size_t)h;
#else
  return (size_t)(h ^ (h >> 32));
#endif
}

// XXH64, the fast byte hash, as its specification defines it, with a secret
// seed so colliding keys can't be worked out offline. Some key sets collide
// under every seed (a lane round maps a change of 2^63 to plus or minus 2^30,
// which the next word can cancel); the detection switches such a map to
// SipHash.
#define XXH_P1 0x9E3779B185EBCA87ULL
#define XXH_P2 0xC2B2AE3D27D4EB4FULL
#define XXH_P3 0x165667B19E3779F9ULL
#define XXH_P4 0x85EBCA77C2B2AE63ULL
#define XXH_P5 0x27D4EB2F165667C5ULL

// One XXH64 lane round: mixes an 8-byte input word into a lane accumulator.
static inline uint64_t xxh_round(uint64_t acc, uint64_t input) {
  acc += input * XXH_P2;
  acc = sip_rotl(acc, 31);
  return acc * XXH_P1;
}

// Folds a lane accumulator into the XXH64 result after the stripes.
static inline uint64_t xxh_merge_round(uint64_t h, uint64_t v) {
  h ^= xxh_round(0, v);
  return h * XXH_P1 + XXH_P4;
}

// Computes XXH64 of len bytes with the given seed, exactly as the
// specification defines it; hash_bytes_fast() uses it as the fast byte hash.
static inline uint64_t xxh64(const void* input, size_t len, uint64_t seed) {
  const unsigned char* p = (const unsigned char*)input;
  const unsigned char* const end = p + len;
  uint64_t h;
  if (len >= 32) {
    const unsigned char* const limit = end - 32;
    uint64_t v1 = seed + XXH_P1 + XXH_P2;
    uint64_t v2 = seed + XXH_P2;
    uint64_t v3 = seed;
    uint64_t v4 = seed - XXH_P1;
    do {
      v1 = xxh_round(v1, sip_load64(p));
      v2 = xxh_round(v2, sip_load64(p + 8));
      v3 = xxh_round(v3, sip_load64(p + 16));
      v4 = xxh_round(v4, sip_load64(p + 24));
      p += 32;
    } while (p <= limit);
    h = sip_rotl(v1, 1) + sip_rotl(v2, 7) + sip_rotl(v3, 12) + sip_rotl(v4, 18);
    h = xxh_merge_round(h, v1);
    h = xxh_merge_round(h, v2);
    h = xxh_merge_round(h, v3);
    h = xxh_merge_round(h, v4);
  } else {
    h = seed + XXH_P5;
  }
  h += (uint64_t)len;
  size_t left = (size_t)(end - p);
  for (; left >= 8; left -= 8, p += 8) {
    h ^= xxh_round(0, sip_load64(p));
    h = sip_rotl(h, 27) * XXH_P1 + XXH_P4;
  }
  if (left >= 4) {
    h ^= sip_load32(p) * XXH_P1;
    h = sip_rotl(h, 23) * XXH_P2 + XXH_P3;
    p += 4;
    left -= 4;
  }
  for (; left != 0; left--, p++) {
    h ^= (uint64_t)*p * XXH_P5;
    h = sip_rotl(h, 11) * XXH_P1;
  }
  h ^= h >> 33;
  h *= XXH_P2;
  h ^= h >> 29;
  h *= XXH_P3;
  h ^= h >> 32;
  return h;
}

// Reduces a hash to an index into a power-of-two table by taking its HIGH
// bits, the bits every hash here mixes best; every reduction goes through
// here. The shift is the leading-zero count of capacity plus one, so only the
// builtin names the width. capacity must be a power of two of at least 2: 1
// makes the shift as wide as the type and 0 reaches __builtin_clz, both
// undefined, and such a shift folds to 0 and puts every key in slot 0.
static inline size_t hash_index_for(size_t hash_val, size_t capacity) {
#if SIZE_MAX == UINT64_MAX
  return hash_val >>
         ((unsigned)__builtin_clzll((unsigned long long)capacity) + 1u);
#else
  return hash_val >> ((unsigned)__builtin_clz((unsigned int)capacity) + 1u);
#endif
}

// Hashes a long double key by its VALUE, in both modes. A long double's
// padding bits are unspecified (often never written) and differ between two
// copies of one number, so a byte hash would miss equal keys. frexpl() takes
// the value apart, and {scaled mantissa, exponent} goes through SipHash-1-3.
// NaN, +-0.0 and +-infinity never reach frexpl(): each class is the pair
// {0, class}, which no number produces. Every NaN is one key, because reading
// a NaN's padding is a read of uninitialized memory (long_double_keys_equal()
// matches); +-0.0 is merged up front, since frexpl() needn't keep the sign of
// zero; and infinities can't be converted to an integer.
static __attribute__((noinline)) size_t
hash_long_double_value(long double v, const uint64_t sip_v[4]) {
  uint64_t parts[2] = {0, 0};
  if (isnan(v)) {
    parts[1] = 3;
  } else if (v == 0.0L) {
    parts[1] = 0;
  } else if (isinf(v)) {
    parts[1] = v > 0.0L ? 1 : 2;
  } else {
    int exp = 0;
    long double mantissa = frexpl(v, &exp);
    int64_t scaled_mantissa = (int64_t)(mantissa * 4611686018427387904.0L);
    parts[0] = (uint64_t)scaled_mantissa;
    parts[1] = (uint64_t)(int64_t)exp;
  }
  return hash_bytes_keyed(sip_v, parts, sizeof(parts));
}

// The process secret and its once flag. It's picked on the first map creation
// or ccol_chmap_hash_key() call, never in a constructor, so a map that another
// object's constructor creates still gets the final secret, and it never
// changes. Map operations read it directly; whatever hands a map to another
// thread orders the creation, and so the pick, before the use.
static chmap_hash_secret g_chmap_hash_secret;
static ccol__once_flag g_chmap_hash_secret_once = CCOL__ONCE_INIT;

// Mixes a word with one splitmix64 step, which spreads every input bit over
// every output bit, so chmap_fallback_hash_key() can combine sources of uneven
// quality.
static uint64_t chmap_seed_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Derives the four key words of the secret when getrandom(2) can't answer
// without blocking (no syscall, a seccomp policy, early boot): a mix of both
// clocks, the pid and three addresses that ASLR places. Weaker than the
// kernel, much stronger than a constant, and it never fails.
static void chmap_fallback_hash_key(uint64_t out[4]) {
  struct timespec rt = {0, 0};
  struct timespec mt = {0, 0};
  (void)clock_gettime(CLOCK_REALTIME, &rt);
  (void)clock_gettime(CLOCK_MONOTONIC, &mt);
  uint64_t x = chmap_seed_mix((uint64_t)rt.tv_sec * 1000000000ULL +
                              (uint64_t)rt.tv_nsec);
  x = chmap_seed_mix(
      x ^ ((uint64_t)mt.tv_sec * 1000000000ULL + (uint64_t)mt.tv_nsec));
  x = chmap_seed_mix(x ^ (uint64_t)getpid());
  x = chmap_seed_mix(x ^ (uint64_t)(uintptr_t)&x);
  x = chmap_seed_mix(x ^ (uint64_t)(uintptr_t)&g_chmap_hash_secret);
  x = chmap_seed_mix(x ^ (uint64_t)(uintptr_t)&clock_gettime);
  out[0] = x;
  out[1] = chmap_seed_mix(out[0]);
  out[2] = chmap_seed_mix(out[1]);
  out[3] = chmap_seed_mix(out[2]);
}

// Fills the SipHash state sip_v from the key (k0, k1), so a hash doesn't redo
// the four xors.
static void chmap_sip_state_from_key(uint64_t k0, uint64_t k1,
                                     uint64_t sip_v[4]) {
  sip_v[0] = k0 ^ 0x736F6D6570736575ULL;
  sip_v[1] = k1 ^ 0x646F72616E646F6DULL;
  sip_v[2] = k0 ^ 0x6C7967656E657261ULL;
  sip_v[3] = k1 ^ 0x7465646279746573ULL;
}

// Picks the process secret and writes it to g_chmap_hash_secret: 32 random
// bytes (or the fallback) give k0, k1, int_seed and byte_seed. It runs once,
// through g_chmap_hash_secret_once.
static void chmap_init_process_hash_secret(void) {
  uint64_t key[4] = {0, 0, 0, 0};
  if (!ccol_random_bytes(key, sizeof(key))) {
    chmap_fallback_hash_key(key);
  }
  chmap_sip_state_from_key(key[0], key[1], g_chmap_hash_secret.sip_v);
  g_chmap_hash_secret.int_seed = key[2];
  g_chmap_hash_secret.byte_seed = key[3];
}

// Gives the process secret, picking it on the first call through the once
// flag. Only the creation path and ccol_chmap_hash_key() call it, never an
// insert or a lookup, which read g_chmap_hash_secret directly.
static const chmap_hash_secret* chmap_process_hash_secret(void) {
  ccol__call_once(g_chmap_hash_secret_once, chmap_init_process_hash_secret);
  return &g_chmap_hash_secret;
}

// Hashes a buffer in the fast mode with XXH64 seeded with byte_seed and gives
// a size_t; a 32-bit size_t keeps both halves.
static inline size_t hash_bytes_fast(const void* input, size_t len) {
  uint64_t h = xxh64(input, len, g_chmap_hash_secret.byte_seed);
#if SIZE_MAX == UINT64_MAX
  return (size_t)h;
#else
  return (size_t)(h ^ (h >> 32));
#endif
}

// Hashes a buffer with the keyed SipHash, out of line, so the fast byte hash
// stays inline in the functions that hash both ways.
static __attribute__((noinline)) size_t hash_bytes_keyed_ool(const void* input,
                                                             size_t len) {
  return hash_bytes_keyed(g_chmap_hash_secret.sip_v, input, len);
}

// Hash a fixed-width key no wider than size_t, or a 64-bit key, in the mode
// that keyed names; hash_key_data() picks between them.
static inline __attribute__((always_inline)) size_t
hash_word_mode(size_t x, bool keyed, uint64_t int_seed) {
  return keyed ? hash_word_seeded(x, int_seed) : hash_word_fast(x);
}

static inline __attribute__((always_inline)) size_t
hash_u64_mode(uint64_t x, bool keyed, uint64_t int_seed) {
  return keyed ? hash_u64_seeded(x, int_seed) : hash_u64_fast(x);
}

// Hashes a key with the function for its type and the mode: integers, float,
// double and pointers through hash_word_mode() or hash_u64_mode(), long double
// through hash_long_double_value() in both modes, anything else through
// hash_bytes_fast() or hash_bytes_keyed_ool(). A custom proc's result goes
// through hash_custom_fast() or hash_word_seeded(). Only separate chaining
// reaches the long double and byte cases. Multi-byte reads go through memcpy
// of a constant size, because key_ptr can come from a caller's cmap_pair with
// no alignment. The callers pass int_seed so loops that write through a byte
// pointer keep it in a register.
static inline __attribute__((always_inline)) size_t
hash_key_data(const void* key_ptr, size_t key_size, ccol_data_type key_type,
              ccol_hashing_proc custom_proc, bool keyed, uint64_t int_seed) {
  if (custom_proc) {
    size_t h = (size_t)custom_proc(key_ptr, key_size);
    return keyed ? hash_word_seeded(h, int_seed) : hash_custom_fast(h);
  }

  switch (key_type) {
    case ccol_char:
    case ccol_signed_char:
    case ccol_unsigned_char:
      return hash_word_mode((size_t)*(const uint8_t*)key_ptr, keyed, int_seed);
    case ccol_short:
    case ccol_unsigned_short: {
      uint16_t bits;
      memcpy(&bits, key_ptr, sizeof(bits));
      return hash_word_mode((size_t)bits, keyed, int_seed);
    }
    case ccol_int:
    case ccol_unsigned_int: {
      uint32_t bits;
      memcpy(&bits, key_ptr, sizeof(bits));
      return hash_word_mode((size_t)bits, keyed, int_seed);
    }
    case ccol_long:
    case ccol_unsigned_long: {
#if SIZE_MAX == UINT64_MAX
      uint64_t bits;
      memcpy(&bits, key_ptr, sizeof(bits));
      return hash_word_mode((size_t)bits, keyed, int_seed);
#else
      uint32_t bits;
      memcpy(&bits, key_ptr, sizeof(bits));
      return hash_word_mode((size_t)bits, keyed, int_seed);
#endif
    }
    case ccol_long_long:
    case ccol_unsigned_long_long: {
      uint64_t bits;
      memcpy(&bits, key_ptr, sizeof(bits));
      return hash_u64_mode(bits, keyed, int_seed);
    }
    case ccol_float: {
      uint32_t bits;
      memcpy(&bits, key_ptr, 4);
      return hash_word_mode((size_t)bits, keyed, int_seed);
    }
    case ccol_double: {
      uint64_t bits;
      memcpy(&bits, key_ptr, 8);
      return hash_u64_mode(bits, keyed, int_seed);
    }
    case ccol_pointer: {
      uintptr_t bits;
      memcpy(&bits, key_ptr, sizeof(uintptr_t));
      return hash_word_mode((size_t)bits, keyed, int_seed);
    }
    case ccol_long_double: {
      long double v;
      memcpy(&v, key_ptr, sizeof(v));
      return hash_long_double_value(v, g_chmap_hash_secret.sip_v);
    }
    default:
      return keyed ? hash_bytes_keyed_ool(key_ptr, key_size)
                   : hash_bytes_fast(key_ptr, key_size);
  }
}

// True when [a, a + a_size) and [b, b + b_size) overlap. An update uses it to
// catch a value pointer that aliases the storage it's about to change.
static inline bool ranges_overlap(const void* a, size_t a_size, const void* b,
                                  size_t b_size) {
  if (a_size == 0 || b_size == 0) return false;
  uintptr_t a_start = (uintptr_t)a;
  uintptr_t a_end = a_start + a_size;
  uintptr_t b_start = (uintptr_t)b;
  uintptr_t b_end = b_start + b_size;
  return a_start < b_end && b_start < a_end;
}

// White-box work counters and their accessors for the tests: probe steps,
// rehashes, shift moves and steps, chain node visits, and the fast attempts of
// keyed growths. They let a test assert on work instead of time. Each is
// atomic with relaxed ordering, because the probe loops run on many threads
// that share a map, and the tests read them only after joining those threads.
// The COUNT macros and CHMAP_NOTE_RETRY() add to them (the latter calls
// chmap_note_retry()), and CHMAP_RETRY_WORK() adds n to an attempt's local
// work count; in a normal build they all expand to nothing.
#ifdef RUNNING_UNIT_TESTS
static _Atomic unsigned long long g_oa_probe_steps_for_tests = 0;
// Gives the probe step count.
unsigned long long chashmap_oa_probe_steps_for_tests(void) {
  return atomic_load_explicit(&g_oa_probe_steps_for_tests,
                              memory_order_relaxed);
}
// Sets the probe step count to 0.
void chashmap_reset_oa_probe_steps_for_tests(void) {
  atomic_store_explicit(&g_oa_probe_steps_for_tests, 0, memory_order_relaxed);
}
#define OA_COUNT_PROBE()                                        \
  (atomic_fetch_add_explicit(&g_oa_probe_steps_for_tests, 1ull, \
                             memory_order_relaxed))
static _Atomic unsigned long long g_oa_rehashes_for_tests = 0;
// Gives the count of successful open-addressing rehashes.
unsigned long long chashmap_oa_rehashes_for_tests(void) {
  return atomic_load_explicit(&g_oa_rehashes_for_tests, memory_order_relaxed);
}
#define OA_COUNT_REHASH()                                    \
  (atomic_fetch_add_explicit(&g_oa_rehashes_for_tests, 1ull, \
                             memory_order_relaxed))
static _Atomic unsigned long long g_oa_shift_moves_for_tests = 0;
// Gives how many entries backward-shift deletion moved.
unsigned long long chashmap_oa_shift_moves_for_tests(void) {
  return atomic_load_explicit(&g_oa_shift_moves_for_tests,
                              memory_order_relaxed);
}
#define OA_COUNT_SHIFT_MOVE()                                   \
  (atomic_fetch_add_explicit(&g_oa_shift_moves_for_tests, 1ull, \
                             memory_order_relaxed))
static _Atomic unsigned long long g_oa_shift_steps_for_tests = 0;
// Gives how many slots the backward-shift walks looked at.
unsigned long long chashmap_oa_shift_steps_for_tests(void) {
  return atomic_load_explicit(&g_oa_shift_steps_for_tests,
                              memory_order_relaxed);
}
#define OA_COUNT_SHIFT_STEP()                                   \
  (atomic_fetch_add_explicit(&g_oa_shift_steps_for_tests, 1ull, \
                             memory_order_relaxed))
static _Atomic unsigned long long g_sc_node_visits_for_tests = 0;
// Gives how many chain nodes the chaining searches visited.
unsigned long long chashmap_sc_node_visits_for_tests(void) {
  return atomic_load_explicit(&g_sc_node_visits_for_tests,
                              memory_order_relaxed);
}
// Sets the chain node count to 0.
void chashmap_reset_sc_node_visits_for_tests(void) {
  atomic_store_explicit(&g_sc_node_visits_for_tests, 0, memory_order_relaxed);
}
#define SC_COUNT_NODE_VISIT()                                   \
  (atomic_fetch_add_explicit(&g_sc_node_visits_for_tests, 1ull, \
                             memory_order_relaxed))
static _Atomic unsigned long long g_retries_for_tests = 0;
static _Atomic unsigned long long g_retry_failures_for_tests = 0;
static _Atomic unsigned long long g_retry_failed_work_for_tests = 0;
static _Atomic size_t g_retry_last_work_for_tests = 0;
static _Atomic size_t g_retry_last_count_for_tests = 0;
static _Atomic size_t g_retry_last_capacity_for_tests = 0;
// Gives how many fast attempts keyed growths made.
unsigned long long chashmap_retries_for_tests(void) {
  return atomic_load_explicit(&g_retries_for_tests, memory_order_relaxed);
}
// Gives how many of those attempts failed.
unsigned long long chashmap_retry_failures_for_tests(void) {
  return atomic_load_explicit(&g_retry_failures_for_tests,
                              memory_order_relaxed);
}
// Gives the total work of the failed attempts.
unsigned long long chashmap_retry_failed_work_for_tests(void) {
  return atomic_load_explicit(&g_retry_failed_work_for_tests,
                              memory_order_relaxed);
}
// Gives the work, count and capacity of the last failed attempt.
void chashmap_last_failed_retry_for_tests(size_t* work, size_t* count,
                                          size_t* capacity) {
  *work =
      atomic_load_explicit(&g_retry_last_work_for_tests, memory_order_relaxed);
  *count =
      atomic_load_explicit(&g_retry_last_count_for_tests, memory_order_relaxed);
  *capacity = atomic_load_explicit(&g_retry_last_capacity_for_tests,
                                   memory_order_relaxed);
}
// Records one fast attempt of a keyed growth for the test counters, and its
// work, count and capacity when it failed.
static void chmap_note_retry(bool failed, size_t work, size_t count,
                             size_t capacity) {
  atomic_fetch_add_explicit(&g_retries_for_tests, 1ull, memory_order_relaxed);
  if (!failed) return;
  atomic_fetch_add_explicit(&g_retry_failures_for_tests, 1ull,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&g_retry_failed_work_for_tests,
                            (unsigned long long)work, memory_order_relaxed);
  atomic_store_explicit(&g_retry_last_work_for_tests, work,
                        memory_order_relaxed);
  atomic_store_explicit(&g_retry_last_count_for_tests, count,
                        memory_order_relaxed);
  atomic_store_explicit(&g_retry_last_capacity_for_tests, capacity,
                        memory_order_relaxed);
}
#define CHMAP_NOTE_RETRY(failed, work, count, capacity) \
  chmap_note_retry((failed), (work), (count), (capacity))
#define CHMAP_RETRY_WORK(var, n) ((var) += (n))
#else
#define CHMAP_NOTE_RETRY(failed, work, count, capacity) ((void)0)
#define CHMAP_RETRY_WORK(var, n) ((void)0)
#define OA_COUNT_SHIFT_MOVE() ((void)0)
#define OA_COUNT_SHIFT_STEP() ((void)0)
#define OA_COUNT_PROBE() ((void)0)
#define OA_COUNT_REHASH() ((void)0)
#define SC_COUNT_NODE_VISIT() ((void)0)
#endif

// Widens a key to the 64-bit form a slot stores (zeroed, then key_size bytes
// copied), so a probe compares integers instead of calling memcmp. The switch
// gives each copy a constant size. Every entry point checks the size against
// the type first; the default case clamps anyway, since the target is one
// 8-byte object.
static inline uint64_t oa_widen_key(const void* key_ptr, size_t key_size) {
  uint64_t k = 0;
  switch (key_size) {
    case 1:
      memcpy(&k, key_ptr, 1);
      break;
    case 2:
      memcpy(&k, key_ptr, 2);
      break;
    case 4:
      memcpy(&k, key_ptr, 4);
      break;
    case 8:
      memcpy(&k, key_ptr, 8);
      break;
    default:
      memcpy(&k, key_ptr, key_size > sizeof(k) ? sizeof(k) : key_size);
      break;
  }
  return k;
}

// Compares a slot's key with a key that oa_widen_key() widened outside the
// probe loop.
static inline bool oa_keys_equal(const oa_slot* slot, uint64_t probe_key) {
  return slot->key_data == probe_key;
}

// Fills out with {&slots[index].val_data, val_size}, the value pair that every
// open-addressing lookup reports.
static inline void oa_fill_val_pair(const open_addr_map* map, size_t index,
                                    cmap_pair* out) {
  out->ptr = &map->slots[index].val_data;
  out->size = map->val_size;
}

// A table block holds the slots and then one metadata byte per slot, and
// nothing else, so a growth faults in only 17 bytes per slot.
#define OA_BLOCK_BYTES_PER_SLOT (sizeof(oa_slot) + 1)

// Allocates a zeroed block of capacity slots plus their metadata bytes and
// reports where the metadata starts. It checks the multiplication itself,
// because a product that wraps would give a small block that the probes then
// walk past.
static oa_slot* oa_alloc_block(ccol_memmgmt_procs* m_procs, size_t capacity,
                               uint8_t** metadata_out) {
  if (capacity == 0 || capacity > SIZE_MAX / OA_BLOCK_BYTES_PER_SLOT) {
    return NULL;
  }
  oa_slot* slots = (oa_slot*)ccol__mem_calloc(
      m_procs, 1, capacity * OA_BLOCK_BYTES_PER_SLOT);
  if (!slots) return NULL;
  *metadata_out = (uint8_t*)(slots + capacity);
  return slots;
}

// Gives the smallest probe_span of a table: OA_DEEP_INSERT, or the whole
// table when smaller.
static inline size_t oa_min_probe_span(size_t capacity) {
  return capacity < OA_DEEP_INSERT ? capacity : OA_DEEP_INSERT;
}

// Sets up the open-addressing state in the map's block and allocates a zeroed
// table. Gives false with nothing allocated on failure.
static bool oa_init(open_addr_map* map, size_t capacity,
                    ccol_data_type key_type, ccol_data_type val_type,
                    size_t key_size, size_t val_size,
                    ccol_memmgmt_procs* m_procs,
                    ccol_hashing_proc custom_hashing_proc) {
  map->slots = oa_alloc_block(m_procs, capacity, &map->metadata);
  if (!map->slots) return false;

  map->capacity = capacity;
  map->probe_span = oa_min_probe_span(capacity);
  map->count = 0;
  map->custom_hashing_proc = custom_hashing_proc;
  map->key_type = key_type;
  map->val_type = val_type;
  map->key_size = key_size;
  map->val_size = val_size;
  map->m_procs = m_procs;
  return true;
}

// The parameters of oa_insert_cap(): slots per doubling, and the log2 size
// from which the cap stays fixed.
#define OA_CAP_PER_DOUBLING 24u
#define OA_CAP_LOG2_CEILING 12u
// Gives the cap on the displacement of one insert into a fast table of
// capacity slots: 24 slots per doubling up to 4096 slots, then 288. An insert
// past it switches the map at once. The cap sits above what a random function
// produces (at load 0.70 one insert in 1.4e9 passes 288), so ordinary keys
// never trigger it.
static inline size_t oa_insert_cap(size_t capacity) {
  unsigned k = chmap_log2_pow2(capacity);
  return (size_t)OA_CAP_PER_DOUBLING *
         (k < OA_CAP_LOG2_CEILING ? k : OA_CAP_LOG2_CEILING);
}

// Gives the largest count a table holds after an insert that didn't grow it,
// which caps the load a window bound assumes.
static inline size_t oa_max_count(size_t capacity) {
  return (capacity / 10) * 7 + ((capacity % 10) * 7) / 10 + 1;
}

// The bound on the displacement sum of a window's 256 inserts: 256 *
// (5 * E(a) + 0.5) for 4096 slots or more, 6 * E(a) for smaller tables, with
// E(a) = (1 / (1 - a)^2 - 1) / 2 the mean displacement under a random function.
// a is the highest load the window can reach (start count plus 256, capped
// where the table grows), never the load at its end, which a bulk delete can
// pull down. Computed in integers in 16.16 fixed point, within 1 percent.
static uint16_t oa_window_bound(size_t start_count, size_t capacity) {
  size_t limit = oa_max_count(capacity);
  size_t judged =
      start_count < limit && limit - start_count > CHMAP_WINDOW_INSERTS
          ? start_count + CHMAP_WINDOW_INSERTS
          : limit;
  uint64_t a = chmap_load_q16(judged, capacity);
  if (a > 65535u) a = 65535u;
  uint64_t d = 65536u - a;
  uint64_t d2 = d * d;
  uint64_t mul = capacity >= 4096 ? 640u : 768u;
  uint64_t bound = mul * ((1ULL << 32) - d2) / d2 + 128u;
  return bound > UINT16_MAX ? UINT16_MAX : (uint16_t)bound;
}

// Starts a new insert window of an open-addressing map and computes its
// bound; a keyed table skips the division, since it's never judged.
static inline void oa_window_restart(open_addr_map* map) {
  struct chashmap* chm = chmap_of_oa(map);
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound = chm->hash_mode == CHMAP_HASH_KEYED
                          ? UINT16_MAX
                          : oa_window_bound(map->count, map->capacity);
}

// Places every live entry of the old table into the zeroed new_slots with the
// hash of the mode keyed names, for oa_rebuild(), and gives the largest
// displacement, which sets probe_span. The table stores no hash, so each one
// is computed again.
static size_t oa_place_all(const open_addr_map* map, oa_slot* new_slots,
                           uint8_t* new_metadata, size_t new_capacity,
                           const oa_slot* old_slots,
                           const uint8_t* old_metadata, size_t old_capacity,
                           bool keyed) {
  const size_t mask = new_capacity - 1;
  const uint64_t int_seed = g_chmap_hash_secret.int_seed;
  size_t largest = 0;
  for (size_t i = 0; i < old_capacity; i++) {
    if (!(old_metadata[i] & SLOT_OCCUPIED)) continue;
    size_t hash_val =
        hash_key_data(&old_slots[i].key_data, map->key_size, map->key_type,
                      map->custom_hashing_proc, keyed, int_seed);
    size_t home = hash_index_for(hash_val, new_capacity);
    size_t index = home;

    __builtin_prefetch(&new_slots[(index + 1) & mask], 1, 1);

    while (new_metadata[index] & SLOT_OCCUPIED) {
      index = (index + 1) & mask;
      __builtin_prefetch(&new_slots[(index + 1) & mask], 1, 1);
    }

    new_slots[index] = old_slots[i];
    new_metadata[index] = SLOT_OCCUPIED;
    size_t displacement = (index - home) & mask;
    if (displacement > largest) largest = displacement;
  }
  return largest;
}

// The slack in a fast attempt's displacement budget.
#define OA_ATTEMPT_SLACK 64u

// Fills the new table with the fast hash when a keyed table grows, so a map
// whose keys spread again under the cheap hash goes back to it. It works like
// oa_place_all(), but gives SIZE_MAX as soon as an entry lands more than
// oa_insert_cap() from home or the displacements pass count +
// OA_ATTEMPT_SLACK; random keys never hit either limit. The abort keeps a
// failed attempt under 2 * count + cap + 65 slots, where keys made to collide
// would otherwise cost quadratic time.
static __attribute__((noinline)) size_t
oa_place_all_fast_attempt(const open_addr_map* map, oa_slot* new_slots,
                          uint8_t* new_metadata, size_t new_capacity) {
  const size_t mask = new_capacity - 1;
  const size_t cap = oa_insert_cap(new_capacity);
  const size_t budget = map->count + OA_ATTEMPT_SLACK;
  const oa_slot* old_slots = map->slots;
  const uint8_t* old_metadata = map->metadata;
  size_t largest = 0, sum = 0, work = 0;
  size_t result = SIZE_MAX;
  for (size_t i = 0; i < map->capacity; i++) {
    if (!(old_metadata[i] & SLOT_OCCUPIED)) continue;
    size_t hash_val =
        hash_key_data(&old_slots[i].key_data, map->key_size, map->key_type,
                      map->custom_hashing_proc, false, 0);
    size_t index = hash_index_for(hash_val, new_capacity);
    size_t displacement = 0;
    CHMAP_RETRY_WORK(work, 1);
    while (new_metadata[index] & SLOT_OCCUPIED) {
      if (displacement == cap) goto out;
      index = (index + 1) & mask;
      displacement++;
      CHMAP_RETRY_WORK(work, 1);
    }
    new_slots[index] = old_slots[i];
    new_metadata[index] = SLOT_OCCUPIED;
    if (displacement > largest) largest = displacement;
    sum += displacement;
    if (sum > budget) goto out;
  }
  result = largest;
out:
  CHMAP_NOTE_RETRY(result == SIZE_MAX, work, map->count, new_capacity);
  (void)work;
  return result;
}

// Rebuilds the table at new_capacity with the hash of the mode keyed names,
// for growth, shrink and the switch, and restarts the window. A fast rebuild
// whose largest displacement passes the new cap (a shrink can merge clusters)
// refills the same block keyed. Asking for fast on a keyed table is a growth's
// fast attempt, and a failure refills the same block keyed. Gives
// ccol_not_enough_memory with the map unchanged when the block can't be
// allocated.
static ccol_retval oa_rebuild(open_addr_map* map, size_t new_capacity,
                              bool keyed) {
  uint8_t* new_metadata = NULL;
  oa_slot* new_slots =
      oa_alloc_block(map->m_procs, new_capacity, &new_metadata);
  if (!new_slots) {
    return ccol_not_enough_memory;
  }

  size_t largest;
  if (!keyed && oa_keyed(map)) {
    largest =
        oa_place_all_fast_attempt(map, new_slots, new_metadata, new_capacity);
  } else {
    largest = oa_place_all(map, new_slots, new_metadata, new_capacity,
                           map->slots, map->metadata, map->capacity, keyed);
  }
  if (!keyed && largest > oa_insert_cap(new_capacity)) {
    memset(new_slots, 0, new_capacity * OA_BLOCK_BYTES_PER_SLOT);
    keyed = true;
    largest = oa_place_all(map, new_slots, new_metadata, new_capacity,
                           map->slots, map->metadata, map->capacity, keyed);
  }

  ccol__mem_free(map->m_procs, map->slots);
  map->slots = new_slots;
  map->metadata = new_metadata;
  map->capacity = new_capacity;
  size_t span = oa_min_probe_span(new_capacity);
  map->probe_span = largest + 1 > span ? largest + 1 : span;
  chmap_of_oa(map)->hash_mode = keyed ? CHMAP_HASH_KEYED : CHMAP_HASH_FAST;
  oa_window_restart(map);
  OA_COUNT_REHASH();
  return ccol_success;
}

// Gives the capacity a growth rehashes to: the smallest power of two at or
// above twice the live count, at least the minimum. The load after it is
// between a quarter and a half, so a delete can't shrink it straight back and
// growth and shrink can't alternate.
static size_t oa_rehash_capacity_for(size_t live_after_insert) {
  if (live_after_insert > ccol__max_power_of_two_size / 2) {
    return ccol__max_power_of_two_size;
  }
  size_t wanted = ccol__find_nearest_gte_power_of_two(2 * live_after_insert);
  return wanted < minimum_allowed_bucket_array_size
             ? minimum_allowed_bucket_array_size
             : wanted;
}

// Tells whether count / capacity > 0.70, the growth trigger, in integers
// (count * 10 > capacity * 7), with a division form for capacities where the
// products could overflow.
static inline bool oa_above_grow_load(size_t count, size_t capacity) {
  if (__builtin_expect(capacity <= SIZE_MAX / 10, 1)) {
    return count * 10 > capacity * 7;
  }
  return count > (capacity / 10) * 7 + ((capacity % 10) * 7) / 10;
}

// Finds the first empty slot in probe order from hash_val's home, or gives
// capacity when there's none. The key must be known to be absent; the insert
// uses it after a rehash makes its earlier probe stale.
static size_t oa_find_insertion_slot(const open_addr_map* map,
                                     size_t hash_val) {
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t start_index = index;

  __builtin_prefetch(&map->slots[index], 0, 1);

  do {
    OA_COUNT_PROBE();
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 0, 1);

    if (!(map->metadata[index] & SLOT_OCCUPIED)) {
      return index;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (index != start_index);

  return map->capacity;
}

// Finds the slot of a key that's present, so a switch can tell the insert
// where its new key moved. The key buffer is as wide as any key type, because
// the compiler can't see that this backend never has a long double.
static size_t oa_find_stored_key(open_addr_map* map, uint64_t key) {
  union {
    uint64_t word;
    long double widest;
  } buf;
  memset(&buf, 0, sizeof(buf));
  buf.word = key;
  size_t hash_val = hash_key_data(&buf, map->key_size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  for (size_t left = map->probe_span; left != 0; left--) {
    OA_COUNT_PROBE();
    if ((map->metadata[index] & SLOT_OCCUPIED) &&
        oa_keys_equal(&map->slots[index], key)) {
      return index;
    }
    index = (index + 1) & (map->capacity - 1);
  }
  ccol__assert(false);
  return index;
}

// Switches a fast table to keyed at the same capacity and gives the new slot
// of the key at index. When the rebuild can't allocate, nothing changes and a
// later trigger tries again.
static size_t oa_switch_keeping(open_addr_map* map, size_t index) {
  uint64_t key = map->slots[index].key_data;
  if (oa_rebuild(map, map->capacity, true) != ccol_success) {
    return index;
  }
  return oa_find_stored_key(map, key);
}

// Widens probe_span to cover a new key displaced OA_DEEP_INSERT or more, and
// switches a fast table when the key is past its cap. It's the cold path of
// the insert. Gives the key's slot, which the switch can move.
static __attribute__((noinline)) size_t
oa_note_deep_insert(open_addr_map* map, size_t index, size_t displacement) {
  if (displacement >= map->probe_span) {
    map->probe_span = displacement + 1;
  }
  if (!oa_keyed(map) && displacement > oa_insert_cap(map->capacity)) {
    index = oa_switch_keeping(map, index);
  }
  return index;
}

// Judges a window that just ended and switches a fast table whose sum passes
// the bound; a cold path of the insert. Gives the key's slot, which the switch
// can move.
static __attribute__((noinline)) size_t oa_window_end(open_addr_map* map,
                                                      size_t index) {
  struct chashmap* chm = chmap_of_oa(map);
  bool switch_now = chm->hash_mode == CHMAP_HASH_FAST &&
                    chm->insert_window > chm->window_bound;
  oa_window_restart(map);
  if (switch_now) {
    index = oa_switch_keeping(map, index);
  }
  return index;
}

// Grows the table and inserts a new key into it, for an insert into a table
// above its growth load (the cold path of oa_insert_impl()). Growth asks for
// the fast hash in either mode, and the displacement is reported like any
// insert's. key_pair and val_pair can point into the block the rebuild frees,
// so both sizes and both sets of bytes are copied first. When the growth
// can't allocate, the entry goes into the old table's empty slot (index) if
// found_empty says there is one, else it gives ccol_not_enough_memory; a grown
// table with no room gives ccol_container_full.
static __attribute__((noinline)) ccol_retval oa_insert_with_growth(
    open_addr_map* map, const cmap_pair* key_pair, const cmap_pair* val_pair,
    cmap_pair* slot_out, size_t index, size_t home, bool found_empty) {
  union {
    uint64_t word;
    long double widest;
  } key_copy;
  uint64_t val_copy = 0;
  memset(&key_copy, 0, sizeof(key_copy));
  const size_t key_size = key_pair->size;
  const size_t val_size = val_pair->size;
  memcpy(&key_copy.word, key_pair->ptr, key_size);
  memcpy(&val_copy, val_pair->ptr, val_size);

  if (oa_rebuild(map, oa_rehash_capacity_for(map->count + 1), false) ==
      ccol_success) {
    size_t hash_val = hash_key_data(&key_copy, key_size, map->key_type,
                                    map->custom_hashing_proc, oa_keyed(map),
                                    g_chmap_hash_secret.int_seed);
    home = hash_index_for(hash_val, map->capacity);
    index = oa_find_insertion_slot(map, hash_val);
    if (index >= map->capacity) {
      return ccol_container_full;
    }
  } else if (!found_empty) {
    return ccol_not_enough_memory;
  }

  memcpy(&map->slots[index].key_data, &key_copy.word, sizeof(uint64_t));
  memcpy(&map->slots[index].val_data, &val_copy, sizeof(uint64_t));
  map->metadata[index] = SLOT_OCCUPIED;
  map->count++;
  size_t displacement = (index - home) & (map->capacity - 1);
  if (displacement >= OA_DEEP_INSERT) {
    index = oa_note_deep_insert(map, index, displacement);
  }
  if (chmap_window_add(chmap_of_oa(map), displacement)) {
    index = oa_window_end(map, index);
  }
  if (slot_out) {
    oa_fill_val_pair(map, index, slot_out);
  }
  return ccol_success;
}

// Inserts or updates a pair in an open-addressing map with a linear probe;
// the body of every open-addressing insert. The first empty slot both proves
// the key absent and is where it goes. The probe for the key comes before any
// thought of growth, so an update never rehashes and never invalidates
// another key's reference. A value that aliases its own slot is copied to a
// stack buffer first. A new key past the growth load goes to
// oa_insert_with_growth(); otherwise its displacement feeds the detection,
// which can move it. A full table at the largest capacity gives
// ccol_container_full. slot_out NULL overwrites an existing value; non-NULL
// leaves it and fills *slot_out with the entry's value pair. Callers pass a
// constant, so each instance keeps one arm.
static inline __attribute__((always_inline)) ccol_retval
oa_insert_impl(open_addr_map* map, const cmap_pair* key_pair,
               const cmap_pair* val_pair, cmap_pair* slot_out) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t start_index = index;
  bool found_empty = false;
  const uint64_t probe_key = oa_widen_key(key_pair->ptr, key_pair->size);

  __builtin_prefetch(&map->slots[index], 1, 1);

  do {
    OA_COUNT_PROBE();
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 1, 1);

    if (!(map->metadata[index] & SLOT_OCCUPIED)) {
      found_empty = true;
      break;
    }

    if (oa_keys_equal(&map->slots[index], probe_key)) {
      if (slot_out) {
        oa_fill_val_pair(map, index, slot_out);
        return ccol_key_already_present;
      }
      const void* src = val_pair->ptr;
      uint64_t snapshot;
      if (ranges_overlap(src, val_pair->size, &map->slots[index].val_data,
                         map->val_size)) {
        memcpy(&snapshot, src, val_pair->size);
        src = &snapshot;
      }
      memcpy(&map->slots[index].val_data, src, val_pair->size);
      return ccol_key_already_present;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (index != start_index);

  if (map->count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  if (oa_above_grow_load(map->count, map->capacity) &&
      map->capacity < ccol__max_power_of_two_size) {
    return oa_insert_with_growth(map, key_pair, val_pair, slot_out, index,
                                 start_index, found_empty);
  }

  if (found_empty) {
    memset(&map->slots[index].key_data, 0, sizeof(uint64_t));
    memset(&map->slots[index].val_data, 0, sizeof(uint64_t));
    memcpy(&map->slots[index].key_data, key_pair->ptr, key_pair->size);
    memcpy(&map->slots[index].val_data, val_pair->ptr, val_pair->size);
    map->metadata[index] = SLOT_OCCUPIED;
    map->count++;
    size_t displacement = (index - start_index) & (map->capacity - 1);
    if (__builtin_expect(displacement >= OA_DEEP_INSERT, 0)) {
      index = oa_note_deep_insert(map, index, displacement);
    }
    if (__builtin_expect(chmap_window_add(chmap_of_oa(map), displacement), 0)) {
      index = oa_window_end(map, index);
    }
    if (slot_out) {
      oa_fill_val_pair(map, index, slot_out);
    }
    return ccol_success;
  }

  return ccol_container_full;
}

// Inserts or updates a pair in an open-addressing map; the backend of
// chashmap_insert_elem().
static ccol_retval oa_insert(open_addr_map* map, const cmap_pair* key_pair,
                             const cmap_pair* val_pair) {
  return oa_insert_impl(map, key_pair, val_pair, NULL);
}

// Inserts a key that's absent and fills *slot_out with its value pair either
// way; the open-addressing backend of ccol_chmap_insert_or_get_elem().
static ccol_retval oa_insert_or_get(open_addr_map* map,
                                    const cmap_pair* key_pair,
                                    const cmap_pair* val_pair,
                                    cmap_pair* slot_out) {
  return oa_insert_impl(map, key_pair, val_pair, slot_out);
}

// Looks up a key in an open-addressing map, for every lookup entry point. An
// empty slot or probe_span slots end the search. A hit fills the caller's pair
// with the slot's value bytes, valid until the next change of the map; a miss
// gives {NULL, 0}.
static ccol_retval oa_get(open_addr_map* map, const cmap_pair* key_pair,
                          cmap_pair* val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t left = map->probe_span;
  const uint64_t probe_key = oa_widen_key(key_pair->ptr, key_pair->size);

  __builtin_prefetch(&map->slots[index], 0, 1);

  do {
    OA_COUNT_PROBE();
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 0, 1);

    if (!(map->metadata[index] & SLOT_OCCUPIED)) {
      break;
    }

    if (oa_keys_equal(&map->slots[index], probe_key)) {
      oa_fill_val_pair(map, index, val_pair);
      return ccol_success;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (--left != 0);

  val_pair->ptr = NULL;
  val_pair->size = 0;
  return ccol_key_not_found;
}

// Closes the hole a delete leaves, moving later entries of the cluster back
// (backward-shift deletion), so the table never holds a tombstone and lookups
// stop at the first empty slot. An entry at j with home h moves into the hole
// when h isn't in the cyclic range (hole, j], and its slot becomes the hole;
// masked distances handle a cluster that wraps. The walk stops at the first
// empty slot or span slots past the hole, since nothing further can move in.
static inline __attribute__((always_inline)) void oa_backward_shift_walk(
    oa_slot* const slots, uint8_t* const metadata, const size_t capacity,
    const size_t key_size, const ccol_data_type key_type,
    const ccol_hashing_proc custom, const bool keyed, const uint64_t int_seed,
    const size_t span, size_t hole) {
  const size_t mask = capacity - 1;
  size_t j = hole;
  for (;;) {
    j = (j + 1) & mask;
    if (!(metadata[j] & SLOT_OCCUPIED)) {
      return;
    }
    const size_t gap = (j - hole) & mask;
    if (gap >= span) {
      return;
    }
    OA_COUNT_SHIFT_STEP();
    size_t home =
        hash_index_for(hash_key_data(&slots[j].key_data, key_size, key_type,
                                     custom, keyed, int_seed),
                       capacity);
    if (((j - home) & mask) >= gap) {
      slots[hole] = slots[j];
      metadata[hole] = SLOT_OCCUPIED;
      metadata[j] = 0;
      OA_COUNT_SHIFT_MOVE();
      hole = j;
    }
  }
}

// Defines the fast and the keyed backward-shift walk of one built-in key
// type, so oa_backward_shift_close_gap() can tail-call a walk whose hash is
// fixed at compile time. Each takes only what it reads, so it needs no
// frame.
#define OA_DEFINE_SHIFT_WALKS(suffix, type_enum)                             \
  static __attribute__((noinline)) void oa_shift_walk_fast_##suffix(         \
      oa_slot* slots, uint8_t* metadata, size_t capacity, size_t span,       \
      size_t hole) {                                                         \
    oa_backward_shift_walk(slots, metadata, capacity, 8, type_enum, NULL,    \
                           false, 0, span, hole);                            \
  }                                                                          \
  static __attribute__((noinline)) void oa_shift_walk_keyed_##suffix(        \
      oa_slot* slots, uint8_t* metadata, size_t capacity, uint64_t int_seed, \
      size_t span, size_t hole) {                                            \
    oa_backward_shift_walk(slots, metadata, capacity, 8, type_enum, NULL,    \
                           true, int_seed, span, hole);                      \
  }
OA_DEFINE_SHIFT_WALKS(char, ccol_char)
OA_DEFINE_SHIFT_WALKS(short, ccol_short)
OA_DEFINE_SHIFT_WALKS(int, ccol_int)
OA_DEFINE_SHIFT_WALKS(long, ccol_long)
OA_DEFINE_SHIFT_WALKS(long_long, ccol_long_long)
OA_DEFINE_SHIFT_WALKS(float, ccol_float)
OA_DEFINE_SHIFT_WALKS(double, ccol_double)
OA_DEFINE_SHIFT_WALKS(pointer, ccol_pointer)
#undef OA_DEFINE_SHIFT_WALKS

// Runs the backward-shift walk for a custom hash or a key type that the list
// doesn't name.
static __attribute__((noinline)) void oa_shift_walk_generic(open_addr_map* map,
                                                            size_t hole) {
  oa_backward_shift_walk(map->slots, map->metadata, map->capacity,
                         map->key_size, map->key_type, map->custom_hashing_proc,
                         oa_keyed(map), g_chmap_hash_secret.int_seed,
                         map->probe_span, hole);
}

// Closes the gap after a delete by calling the walk for the map's key type and
// mode; oa_delete() calls it only when the next slot is occupied. The fields
// go in as arguments, because the walk writes through a byte pointer that
// could alias the map. Every call is a tail call.
static __attribute__((noinline)) void oa_backward_shift_close_gap(
    open_addr_map* map, size_t hole) {
  oa_slot* const slots = map->slots;
  uint8_t* const metadata = map->metadata;
  const size_t cap = map->capacity;
  const size_t span = map->probe_span;
  if (map->custom_hashing_proc) {
    oa_shift_walk_generic(map, hole);
    return;
  }
  if (oa_keyed(map)) {
    const uint64_t seed = g_chmap_hash_secret.int_seed;
    switch (map->key_type) {
      case ccol_char:
      case ccol_signed_char:
      case ccol_unsigned_char:
        oa_shift_walk_keyed_char(slots, metadata, cap, seed, span, hole);
        return;
      case ccol_short:
      case ccol_unsigned_short:
        oa_shift_walk_keyed_short(slots, metadata, cap, seed, span, hole);
        return;
      case ccol_int:
      case ccol_unsigned_int:
        oa_shift_walk_keyed_int(slots, metadata, cap, seed, span, hole);
        return;
      case ccol_long:
      case ccol_unsigned_long:
        oa_shift_walk_keyed_long(slots, metadata, cap, seed, span, hole);
        return;
      case ccol_long_long:
      case ccol_unsigned_long_long:
        oa_shift_walk_keyed_long_long(slots, metadata, cap, seed, span, hole);
        return;
      case ccol_float:
        oa_shift_walk_keyed_float(slots, metadata, cap, seed, span, hole);
        return;
      case ccol_double:
        oa_shift_walk_keyed_double(slots, metadata, cap, seed, span, hole);
        return;
      case ccol_pointer:
        oa_shift_walk_keyed_pointer(slots, metadata, cap, seed, span, hole);
        return;
      default:
        oa_shift_walk_generic(map, hole);
        return;
    }
  }
  switch (map->key_type) {
    case ccol_char:
    case ccol_signed_char:
    case ccol_unsigned_char:
      oa_shift_walk_fast_char(slots, metadata, cap, span, hole);
      return;
    case ccol_short:
    case ccol_unsigned_short:
      oa_shift_walk_fast_short(slots, metadata, cap, span, hole);
      return;
    case ccol_int:
    case ccol_unsigned_int:
      oa_shift_walk_fast_int(slots, metadata, cap, span, hole);
      return;
    case ccol_long:
    case ccol_unsigned_long:
      oa_shift_walk_fast_long(slots, metadata, cap, span, hole);
      return;
    case ccol_long_long:
    case ccol_unsigned_long_long:
      oa_shift_walk_fast_long_long(slots, metadata, cap, span, hole);
      return;
    case ccol_float:
      oa_shift_walk_fast_float(slots, metadata, cap, span, hole);
      return;
    case ccol_double:
      oa_shift_walk_fast_double(slots, metadata, cap, span, hole);
      return;
    case ccol_pointer:
      oa_shift_walk_fast_pointer(slots, metadata, cap, span, hole);
      return;
    default:
      oa_shift_walk_generic(map, hole);
      return;
  }
}

// Deletes a key from an open-addressing map with backward-shift deletion,
// searching at most probe_span slots. Later entries can move, so every
// reference into the map becomes invalid. The table halves when the count
// falls below a quarter.
static ccol_retval oa_delete(open_addr_map* map, const cmap_pair* key_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t left = map->probe_span;
  const uint64_t probe_key = oa_widen_key(key_pair->ptr, key_pair->size);

  do {
    OA_COUNT_PROBE();
    if (!(map->metadata[index] & SLOT_OCCUPIED)) {
      return ccol_key_not_found;
    }

    if (oa_keys_equal(&map->slots[index], probe_key)) {
      map->metadata[index] = 0;
      if (map->metadata[(index + 1) & (map->capacity - 1)] & SLOT_OCCUPIED) {
        oa_backward_shift_close_gap(map, index);
      }
      map->count--;

      if (map->count < (map->capacity >> 2) &&
          map->capacity > minimum_allowed_bucket_array_size) {
        oa_rebuild(map, map->capacity / 2, oa_keyed(map));
      }

      return ccol_success;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (--left != 0);

  return ccol_key_not_found;
}

// Frees the slot table at teardown; the state itself lives in the map's block.
static void oa_destroy(open_addr_map* map) {
  ccol__mem_free(map->m_procs, map->slots);
}

// Empties every slot without allocating, for a reset whose new table can't be
// allocated. Keys and values live in the slots, so this destroys every
// element.
static void oa_clear_in_place(open_addr_map* map) {
  memset(map->slots, 0, map->capacity * OA_BLOCK_BYTES_PER_SLOT);
  map->count = 0;
  map->probe_span = oa_min_probe_span(map->capacity);
  oa_window_restart(map);
}

// Clears every entry of an open-addressing map, for chashmap_reset(), and
// replaces the table when new_capacity differs from the current one. When
// that allocation fails it clears in place, so every element is gone whatever
// it returns. The mode stays, the window restarts.
static ccol_retval oa_reset(open_addr_map* map, size_t new_capacity) {
  if (new_capacity == 0 || new_capacity == map->capacity) {
    oa_clear_in_place(map);
    return ccol_success;
  }

  uint8_t* new_metadata = NULL;
  oa_slot* new_slots =
      oa_alloc_block(map->m_procs, new_capacity, &new_metadata);
  if (!new_slots) {
    oa_clear_in_place(map);
    return ccol_not_enough_memory;
  }

  ccol__mem_free(map->m_procs, map->slots);
  map->slots = new_slots;
  map->metadata = new_metadata;

  map->capacity = new_capacity;
  map->count = 0;
  map->probe_span = oa_min_probe_span(new_capacity);
  oa_window_restart(map);

  return ccol_success;
}

// Gives the llist_node that holds the dllist_refs member tracker points at,
// so a walk of the insertion-order list can reach its entries.
#define dllistRefNodePtr2LlistNodePtr(tracker) \
  (llist_node*)((uint8_t*)tracker - offsetof(llist_node, dllist_refs))

// Puts node at the head (newest end) of the insertion-order list, when an
// insert makes it.
static void attach_node_to_dllist(dllist_root* root, dllist_ref_node* node) {
  node->prev = NULL;
  if (!root->head) {
    node->next = NULL;
    root->head = node;
    root->tail = node;
  } else {
    node->next = root->head;
    root->head->prev = node;
    root->head = node;
  }
}

// Takes node off the insertion-order list without freeing it, when its entry
// is deleted.
static void detach_node_from_dllist(dllist_root* root, dllist_ref_node* node) {
  if (node->next) {
    node->next->prev = node->prev;
  } else {
    root->tail = node->prev;
  }
  if (node->prev) {
    node->prev->next = node->next;
  } else {
    root->head = node->next;
  }
}

// Frees a chaining node: its heap key and value buffers (heap exactly when the
// accessor size passes INLINE_STORAGE_THRESHOLD), its place on all_elems, and
// the node. A NULL all_elems skips the list; only the failure paths of
// sc_create_llist_node() pass it.
static void sc_destroy_llist_node(dllist_root* all_elems, llist_node* elem,
                                  ccol_memmgmt_procs* m_procs) {
  if (elem) {
    if (elem->key_pair_accessor.size > INLINE_STORAGE_THRESHOLD &&
        elem->key_storage.ptr) {
      ccol__mem_free(m_procs, elem->key_storage.ptr);
    }
    if (elem->val_pair_accessor.size > INLINE_STORAGE_THRESHOLD &&
        elem->val_storage.ptr) {
      ccol__mem_free(m_procs, elem->val_storage.ptr);
    }
    if (all_elems) {
      detach_node_from_dllist(all_elems, &elem->dllist_refs);
    }
    ccol__mem_free(m_procs, elem);
  }
}

// Makes a chaining node holding copies of the key and value, inline or on the
// heap, for an insert of a new key, and puts it at the head of the
// insertion-order list. Each accessor is set only once its storage holds the
// bytes, so a failure frees only what exists. Forced inline: as a call it
// costs each new key about 57 instructions.
static inline __attribute__((always_inline)) llist_node* sc_create_llist_node(
    dllist_root* all_elems, ccol_memmgmt_procs* m_procs, size_t hash_val,
    const void* key_ptr, size_t key_size, const void* val_ptr,
    size_t val_size) {
  llist_node* new_elem =
      (llist_node*)ccol__mem_calloc(m_procs, 1, sizeof(llist_node));
  if (!new_elem) return NULL;

  new_elem->hash_val = hash_val;

  if (key_size <= INLINE_STORAGE_THRESHOLD) {
    memcpy(&new_elem->key_storage.inline_data, key_ptr, key_size);
    new_elem->key_pair_accessor.ptr = &new_elem->key_storage.inline_data;
  } else {
    new_elem->key_storage.ptr = ccol__mem_alloc(m_procs, key_size);
    if (!new_elem->key_storage.ptr) {
      sc_destroy_llist_node(NULL, new_elem, m_procs);
      return NULL;
    }
    memcpy(new_elem->key_storage.ptr, key_ptr, key_size);
    new_elem->key_pair_accessor.ptr = new_elem->key_storage.ptr;
  }
  new_elem->key_pair_accessor.size = key_size;

  if (val_size <= INLINE_STORAGE_THRESHOLD) {
    memcpy(&new_elem->val_storage.inline_data, val_ptr, val_size);
    new_elem->val_pair_accessor.ptr = &new_elem->val_storage.inline_data;
  } else {
    new_elem->val_storage.ptr = ccol__mem_alloc(m_procs, val_size);
    if (!new_elem->val_storage.ptr) {
      sc_destroy_llist_node(NULL, new_elem, m_procs);
      return NULL;
    }
    memcpy(new_elem->val_storage.ptr, val_ptr, val_size);
    new_elem->val_pair_accessor.ptr = new_elem->val_storage.ptr;
  }
  new_elem->val_pair_accessor.size = val_size;

  attach_node_to_dllist(all_elems, &new_elem->dllist_refs);
  new_elem->next = NULL;
  return new_elem;
}

// Compares two long double keys by value, matching hash_long_double_value():
// two NaNs are equal (no padding byte is read), otherwise ==, so -0.0L equals
// 0.0L.
static inline bool long_double_keys_equal(const void* a_ptr,
                                          const void* b_ptr) {
  long double a, b;
  memcpy(&a, a_ptr, sizeof(a));
  memcpy(&b, b_ptr, sizeof(b));

  bool a_nan = isnan(a);
  bool b_nan = isnan(b);
  if (a_nan || b_nan) {
    return a_nan && b_nan;
  }

  return a == b;
}

// Compares a node's key with key_ptr, for every chain search: sizes first,
// integer compares for 4 and 8 bytes, memcmp otherwise. A long double goes to
// long_double_keys_equal() before the size dispatch, because it can be 8
// bytes wide.
static inline bool sc_compare_keys(const llist_node* node, const void* key_ptr,
                                   size_t key_size, ccol_data_type key_type) {
  if (node->key_pair_accessor.size != key_size) return false;

  const void* node_key_ptr = node->key_pair_accessor.ptr;

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

// Searches a chain of a map with a custom key equality. It's out of line so
// the ordinary loop of sc_find_in_llist() stays as it is. The stored hash
// still rejects most nodes.
static __attribute__((noinline)) llist_node* sc_find_in_llist_custom_eq(
    llist_node* head, size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_key_equality_proc custom_eq) {
  for (llist_node* tracker = head; tracker; tracker = tracker->next) {
    SC_COUNT_NODE_VISIT();
    if (tracker->hash_val == hash_val &&
        custom_eq(tracker->key_pair_accessor.ptr,
                  tracker->key_pair_accessor.size, key_ptr, key_size)) {
      return tracker;
    }
  }
  return NULL;
}

// Counts the nodes of a chain, for the detection of an insert into a map with
// a custom equality.
static __attribute__((noinline)) size_t
sc_chain_length(const llist_node* head) {
  size_t length = 0;
  for (; head; head = head->next) length++;
  return length;
}

// Searches a chain for key_ptr and gives its node or NULL, for every chaining
// lookup and insert. It compares the full stored hash before the key. When
// walk isn't NULL and nothing matches, it gets the chain length for the
// detection. Callers pass walk as a constant.
static inline __attribute__((always_inline)) llist_node* sc_find_in_llist(
    llist_node* head, size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_data_type key_type, ccol_key_equality_proc custom_eq, size_t* walk) {
  if (custom_eq) {
    llist_node* found = sc_find_in_llist_custom_eq(head, hash_val, key_ptr,
                                                   key_size, custom_eq);
    if (!found && walk) *walk = sc_chain_length(head);
    return found;
  }
  size_t met = 0;
  llist_node* tracker = head;
  while (tracker) {
    SC_COUNT_NODE_VISIT();
    if (tracker->hash_val == hash_val &&
        sc_compare_keys(tracker, key_ptr, key_size, key_type)) {
      return tracker;
    }
    if (walk) met++;
    tracker = tracker->next;
  }
  if (walk) *walk = met;
  return NULL;
}

// Replaces a node's value, for an update of a present key: in place for the
// same size, inline for a small value, a realloc or a new buffer for a bigger
// one. Gives false when memory runs out. val_ptr may alias the stored value
// (a chmap_get_ptr() result handed back), and every branch changes the
// storage before reading val_ptr, so an aliasing value is copied first.
static bool sc_reset_val_of_llist_node(llist_node* elem, const void* val_ptr,
                                       size_t val_size,
                                       ccol_memmgmt_procs* m_procs) {
  void* old_ptr = elem->val_pair_accessor.ptr;
  size_t old_size = elem->val_pair_accessor.size;
  bool old_inline = old_size <= INLINE_STORAGE_THRESHOLD;

  unsigned char snapshot_buf[INLINE_STORAGE_THRESHOLD + 1];
  void* heap_snapshot = NULL;
  if (ranges_overlap(val_ptr, val_size, old_ptr, old_size)) {
    if (val_size <= sizeof(snapshot_buf)) {
      memcpy(snapshot_buf, val_ptr, val_size);
      val_ptr = snapshot_buf;
    } else {
      heap_snapshot = ccol__mem_alloc(m_procs, val_size);
      if (!heap_snapshot) return false;
      memcpy(heap_snapshot, val_ptr, val_size);
      val_ptr = heap_snapshot;
    }
  }

  bool ok = true;
  if (val_size == old_size) {
    memcpy(old_ptr, val_ptr, val_size);
  } else if (val_size <= INLINE_STORAGE_THRESHOLD) {
    if (!old_inline) {
      ccol__mem_free(m_procs, elem->val_storage.ptr);
    }
    memcpy(&elem->val_storage.inline_data, val_ptr, val_size);
    elem->val_pair_accessor.ptr = &elem->val_storage.inline_data;
    elem->val_pair_accessor.size = val_size;
  } else {
    if (old_inline) {
      void* new_ptr = ccol__mem_alloc(m_procs, val_size);
      if (!new_ptr) {
        ok = false;
      } else {
        elem->val_storage.ptr = new_ptr;
      }
    } else {
      void* orig = elem->val_storage.ptr;
      elem->val_storage.ptr =
          ccol__mem_realloc(m_procs, elem->val_storage.ptr, val_size);
      if (!elem->val_storage.ptr) {
        elem->val_storage.ptr = orig;
        ok = false;
      }
    }

    if (ok) {
      memcpy(elem->val_storage.ptr, val_ptr, val_size);
      elem->val_pair_accessor.ptr = elem->val_storage.ptr;
      elem->val_pair_accessor.size = val_size;
    }
  }

  if (heap_snapshot) {
    ccol__mem_free(m_procs, heap_snapshot);
  }
  return ok;
}

// Unlinks node from its chain (previous is the node before it, or NULL),
// destroys it and gives the chain's new head; both delete searches end here.
static llist_node* sc_unlink_and_destroy(llist_node* head, llist_node* previous,
                                         llist_node* node,
                                         dllist_root* all_elems,
                                         ccol_memmgmt_procs* m_procs) {
  if (!previous) {
    head = node->next;
  } else {
    previous->next = node->next;
  }
  sc_destroy_llist_node(all_elems, node, m_procs);
  return head;
}

// Removes the node of key_ptr from a chain of a map with a custom key
// equality, out of line like the find. Gives the new head and sets *found.
static __attribute__((noinline)) llist_node* sc_delete_from_llist_custom_eq(
    llist_node* head, dllist_root* all_elems, ccol_memmgmt_procs* m_procs,
    size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_key_equality_proc custom_eq, bool* found) {
  llist_node* previous = NULL;
  for (llist_node* tracker = head; tracker; tracker = tracker->next) {
    SC_COUNT_NODE_VISIT();
    if (tracker->hash_val == hash_val &&
        custom_eq(tracker->key_pair_accessor.ptr,
                  tracker->key_pair_accessor.size, key_ptr, key_size)) {
      *found = true;
      return sc_unlink_and_destroy(head, previous, tracker, all_elems, m_procs);
    }
    previous = tracker;
  }
  return head;
}

// Removes the node of key_ptr from a chain and gives the chain's new head, for
// sc_delete(). *found says whether there was one. The stored hash rejects most
// nodes with one compare.
static llist_node* sc_delete_from_llist(
    llist_node* head, dllist_root* all_elems, ccol_memmgmt_procs* m_procs,
    size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_data_type key_type, ccol_key_equality_proc custom_eq, bool* found) {
  *found = false;
  if (custom_eq) {
    return sc_delete_from_llist_custom_eq(head, all_elems, m_procs, hash_val,
                                          key_ptr, key_size, custom_eq, found);
  }
  llist_node* tracker = head;
  llist_node* previous = NULL;

  while (tracker) {
    SC_COUNT_NODE_VISIT();
    if (tracker->hash_val == hash_val &&
        sc_compare_keys(tracker, key_ptr, key_size, key_type)) {
      *found = true;
      if (!previous) {
        head = tracker->next;
      } else {
        previous->next = tracker->next;
      }
      sc_destroy_llist_node(all_elems, tracker, m_procs);
      return head;
    }
    previous = tracker;
    tracker = tracker->next;
  }
  return head;
}

// Destroys every node of a chain and gives NULL; reset and destroy clear each
// bucket with it.
static llist_node* sc_destroy_the_whole_llist(llist_node* head,
                                              dllist_root* all_elems,
                                              ccol_memmgmt_procs* m_procs) {
  llist_node* tracker = head;
  while (tracker) {
    llist_node* node_to_be_deleted = tracker;
    tracker = tracker->next;
    sc_destroy_llist_node(all_elems, node_to_be_deleted, m_procs);
  }
  return NULL;
}

// Sets the growth and shrink thresholds of a chaining map from its bucket
// count, exactly (n + 1) * 3 / 2 and (n + 1) / 8, written so neither
// overflows. Every resize calls it.
static void sc_set_scaling_limits(sep_chain_map* map) {
  size_t base_up = map->bucket_arr_size + map->bucket_arr_size / 2;
  map->elem_count_to_scale_up =
      base_up + ((map->bucket_arr_size % 2 == 0) ? 1 : 2);
  map->elem_count_to_scale_down = (map->bucket_arr_size + 1) / 8;
}

// Gives the bucket array inside the map's block and its length, or NULL and
// 0, so creation, shrink and reset can use it instead of an allocation. The
// map never frees it. Comparing addresses instead would mistake a separate
// array that a headerless allocator puts right after the block for this one.
static inline llist_node** sc_inline_bucket_arr(sep_chain_map* map,
                                                size_t* count) {
  *count = map->inline_buckets;
  return *count ? (llist_node**)(chmap_of_sc(map) + 1) : NULL;
}

// Gives the bound on the nodes a window's 256 inserts meet, above which a fast
// chaining map switches: 256 * (4.5 * lambda + 0.5), lambda being entries per
// bucket at the highest load the window can reach (see oa_window_bound()). In
// integers, within 1 percent.
static uint16_t sc_window_bound(size_t start_count, size_t scale_up,
                                size_t bucket_count) {
  size_t judged =
      start_count < scale_up && scale_up - start_count > CHMAP_WINDOW_INSERTS
          ? start_count + CHMAP_WINDOW_INSERTS
          : scale_up;
  uint64_t lambda = chmap_load_q16(judged, bucket_count);
  uint64_t bound = ((1152u * lambda) >> 16) + 128u;
  return bound > UINT16_MAX ? UINT16_MAX : (uint16_t)bound;
}

// Starts a new insert window of a chaining map and computes its bound; a
// keyed map skips the arithmetic.
static inline void sc_window_restart(sep_chain_map* map) {
  struct chashmap* chm = chmap_of_sc(map);
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound =
      chm->hash_mode == CHMAP_HASH_KEYED
          ? UINT16_MAX
          : sc_window_bound(map->elem_count, map->elem_count_to_scale_up,
                            map->bucket_arr_size);
}

// Computes the hash a node stores for its key, in the mode keyed names.
static inline size_t sc_hash_of_node(const sep_chain_map* map,
                                     const llist_node* node, bool keyed) {
  return hash_key_data(
      node->key_pair_accessor.ptr, node->key_pair_accessor.size, map->key_type,
      map->custom_hashing_proc, keyed, g_chmap_hash_secret.int_seed);
}

// Switches a fast chaining map to keyed when its keys collide: every node gets
// its keyed hash and is linked into the same array again. Nothing moves or
// allocates, so it can't fail and every reference, the order and every
// iterator stay valid.
static __attribute__((noinline)) void sc_switch_to_keyed(sep_chain_map* map) {
  chmap_of_sc(map)->hash_mode = CHMAP_HASH_KEYED;
  llist_node** const buckets = map->bucket_arr;
  const size_t bucket_count = map->bucket_arr_size;
  memset(buckets, 0, bucket_count * sizeof(llist_node*));
  for (dllist_ref_node* t = map->all_elems.tail; t; t = t->prev) {
    llist_node* node = dllistRefNodePtr2LlistNodePtr(t);
    node->hash_val = sc_hash_of_node(map, node, true);
    size_t index = hash_index_for(node->hash_val, bucket_count);
    node->next = buckets[index];
    buckets[index] = node;
  }
  sc_window_restart(map);
}

// Switches a fast map whose new key met SC_CHAIN_CAP nodes; a cold path of
// the insert, kept out of line so the insert stays short.
static __attribute__((noinline)) void sc_note_long_chain(sep_chain_map* map) {
  if (!sc_keyed(map)) {
    sc_switch_to_keyed(map);
  }
}

// Judges a window that just ended and switches a fast map whose sum passes its
// bound; a cold path of the insert.
static __attribute__((noinline)) void sc_window_end(sep_chain_map* map) {
  struct chashmap* chm = chmap_of_sc(map);
  bool switch_now = chm->hash_mode == CHMAP_HASH_FAST &&
                    chm->insert_window > chm->window_bound;
  sc_window_restart(map);
  if (switch_now) {
    sc_switch_to_keyed(map);
  }
}

// The slack in a fast attempt's walk budget.
#define SC_ATTEMPT_SLACK 64u

// Relinks every node into new_arr with the fast hash when a keyed chaining map
// grows, so a map whose keys spread again goes back to the cheap hash. It
// stores the fast hash in each node, walks each chain first, and stops when a
// chain would reach SC_CHAIN_CAP or the walks pass elem_count +
// SC_ATTEMPT_SLACK. On failure it relinks everything keyed into the same
// array (only the nodes it reached need the keyed hash again) and gives false.
// Nothing moves or allocates, and the work stays linear in the count.
static __attribute__((noinline)) bool sc_relink_fast_attempt(
    sep_chain_map* map, llist_node** new_arr, size_t new_size) {
  const size_t budget = map->elem_count + SC_ATTEMPT_SLACK;
  size_t walked = 0, work = 0;
  dllist_ref_node* stop = NULL;
  for (dllist_ref_node* t = map->all_elems.tail; t; t = t->prev) {
    llist_node* node = dllistRefNodePtr2LlistNodePtr(t);
    node->hash_val = sc_hash_of_node(map, node, false);
    size_t index = hash_index_for(node->hash_val, new_size);
    size_t chain = 0;
    for (const llist_node* c = new_arr[index]; c; c = c->next) {
      if (++chain == SC_CHAIN_CAP - 1) break;
    }
    CHMAP_RETRY_WORK(work, 1 + chain);
    walked += chain;
    if (chain == SC_CHAIN_CAP - 1 || walked > budget) {
      stop = t;
      break;
    }
    node->next = new_arr[index];
    new_arr[index] = node;
  }
  CHMAP_NOTE_RETRY(stop != NULL, work, map->elem_count, new_size);
  (void)work;
  if (!stop) return true;

  memset(new_arr, 0, new_size * sizeof(llist_node*));
  bool holds_fast = true;
  for (dllist_ref_node* t = map->all_elems.tail; t; t = t->prev) {
    llist_node* node = dllistRefNodePtr2LlistNodePtr(t);
    if (holds_fast) node->hash_val = sc_hash_of_node(map, node, true);
    if (t == stop) holds_fast = false;
    size_t index = hash_index_for(node->hash_val, new_size);
    node->next = new_arr[index];
    new_arr[index] = node;
  }
  return false;
}

// Grows or shrinks the bucket array of a chaining map by scale_factor when
// the count crosses a threshold, and relinks every node by its stored hash;
// the window restarts. A shrink is clamped to the minimum, takes the in-block
// array back when its length fits, and measures the merged chains: one of
// SC_CHAIN_CAP nodes switches a fast map. Growing a keyed map makes the fast
// attempt. On a failed allocation nothing changes.
static void sc_scale(sep_chain_map* map, bool up) {
  if (up &&
      (map->bucket_arr_size > ccol__max_power_of_two_size / scale_factor)) {
    return;
  }

  size_t new_size = up ? map->bucket_arr_size * scale_factor
                       : map->bucket_arr_size / scale_factor;

  if (!up && new_size < minimum_allowed_bucket_array_size) {
    new_size = minimum_allowed_bucket_array_size;
  }

  size_t inline_count = 0;
  llist_node** new_arr = sc_inline_bucket_arr(map, &inline_count);
  if (new_size == inline_count) {
    memset(new_arr, 0, new_size * sizeof(llist_node*));
  } else {
    new_arr = (llist_node**)ccol__mem_calloc(map->m_procs, new_size,
                                             sizeof(llist_node*));
    if (!new_arr) return;
  }

  const size_t ratio_mask = up ? 0 : map->bucket_arr_size / new_size - 1;
  size_t merged = 0;
  size_t longest_merged = 0;
  if (up && sc_keyed(map)) {
    if (sc_relink_fast_attempt(map, new_arr, new_size)) {
      chmap_of_sc(map)->hash_mode = CHMAP_HASH_FAST;
    }
  } else {
    for (size_t i = 0; i < map->bucket_arr_size; i++) {
      if ((i & ratio_mask) == 0) merged = 0;
      llist_node* tracker = map->bucket_arr[i];
      while (tracker) {
        llist_node* next = tracker->next;
        size_t new_index = hash_index_for(tracker->hash_val, new_size);
        tracker->next = new_arr[new_index];
        new_arr[new_index] = tracker;
        tracker = next;
        merged++;
      }
      if (merged > longest_merged) longest_merged = merged;
    }
  }

  if (map->bucket_arr != sc_inline_bucket_arr(map, &inline_count)) {
    ccol__mem_free(map->m_procs, map->bucket_arr);
  }
  map->bucket_arr = new_arr;
  map->bucket_arr_size = new_size;
  sc_set_scaling_limits(map);
  sc_window_restart(map);
  if (!up && longest_merged >= SC_CHAIN_CAP && !sc_keyed(map)) {
    sc_switch_to_keyed(map);
  }
}

// Sets up the chaining state in the map's block at creation; the first bucket
// array is the zeroed in-block one when the block has it, else a calloc.
// Gives false with nothing allocated on failure.
static bool sc_init(sep_chain_map* map, size_t bucket_arr_size,
                    ccol_data_type key_type, ccol_data_type val_type,
                    ccol_memmgmt_procs* m_procs,
                    ccol_hashing_proc custom_hashing_proc,
                    ccol_key_equality_proc custom_key_equality_proc) {
  size_t inline_count = 0;
  llist_node** inline_arr = sc_inline_bucket_arr(map, &inline_count);
  if (bucket_arr_size == inline_count) {
    map->bucket_arr = inline_arr;
  } else {
    map->bucket_arr = (llist_node**)ccol__mem_calloc(m_procs, bucket_arr_size,
                                                     sizeof(llist_node*));
    if (!map->bucket_arr) return false;
  }

  map->bucket_arr_size = bucket_arr_size;
  map->elem_count = 0;
  map->key_type = key_type;
  map->val_type = val_type;
  map->all_elems.head = NULL;
  map->all_elems.tail = NULL;
  map->m_procs = m_procs;
  map->custom_hashing_proc = custom_hashing_proc;
  map->custom_key_equality_proc = custom_key_equality_proc;
  sc_set_scaling_limits(map);
  sc_window_restart(map);
  return true;
}

// Inserts or updates a pair in a chaining map; the body of every chaining
// insert. The lookup comes before the ccol_max_elem_count check, so a full map
// still accepts an update. A new node goes to the front of its chain, its
// chain length feeds the detection (a switch rehashes in place), and the
// array grows at elem_count_to_scale_up; neither moves the node. slot_out
// means what it means for oa_insert_impl(), and key_slot_out gets the key
// accessor where slot_out gets the value.
static inline __attribute__((always_inline)) ccol_retval sc_insert_impl(
    sep_chain_map* map, const cmap_pair* key_pair, const cmap_pair* val_pair,
    const cmap_pair** slot_out, const cmap_pair** key_slot_out) {
  const size_t hash_val = hash_key_data(
      key_pair->ptr, key_pair->size, map->key_type, map->custom_hashing_proc,
      sc_keyed(map), g_chmap_hash_secret.int_seed);

  size_t index = hash_index_for(hash_val, map->bucket_arr_size);

  size_t walk = 0;
  llist_node* existing = sc_find_in_llist(
      map->bucket_arr[index], hash_val, key_pair->ptr, key_pair->size,
      map->key_type, map->custom_key_equality_proc, &walk);
  if (existing) {
    if (slot_out) {
      *slot_out = &existing->val_pair_accessor;
      if (key_slot_out) {
        *key_slot_out = &existing->key_pair_accessor;
      }
      return ccol_key_already_present;
    }
    return sc_reset_val_of_llist_node(existing, val_pair->ptr, val_pair->size,
                                      map->m_procs)
               ? ccol_key_already_present
               : ccol_not_enough_memory;
  }

  if (map->elem_count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  llist_node* new_node = sc_create_llist_node(
      &map->all_elems, map->m_procs, hash_val, key_pair->ptr, key_pair->size,
      val_pair->ptr, val_pair->size);
  if (!new_node) {
    return ccol_not_enough_memory;
  }

  new_node->next = map->bucket_arr[index];
  map->bucket_arr[index] = new_node;
  ++map->elem_count;

  if (__builtin_expect(walk >= SC_CHAIN_CAP, 0)) {
    sc_note_long_chain(map);
  }
  if (__builtin_expect(chmap_window_add(chmap_of_sc(map), walk), 0)) {
    sc_window_end(map);
  }

  if (map->elem_count >= map->elem_count_to_scale_up) {
    sc_scale(map, true);
  }

  if (slot_out) {
    *slot_out = &new_node->val_pair_accessor;
    if (key_slot_out) {
      *key_slot_out = &new_node->key_pair_accessor;
    }
  }
  return ccol_success;
}

// Inserts or updates a pair in a chaining map; the backend of
// chashmap_insert_elem().
static ccol_retval sc_insert(sep_chain_map* map, const cmap_pair* key_pair,
                             const cmap_pair* val_pair) {
  return sc_insert_impl(map, key_pair, val_pair, NULL, NULL);
}

// Inserts a key that's absent and copies its value accessor to *slot_out
// either way; the chaining backend of ccol_chmap_insert_or_get_elem().
static ccol_retval sc_insert_or_get(sep_chain_map* map,
                                    const cmap_pair* key_pair,
                                    const cmap_pair* val_pair,
                                    cmap_pair* slot_out) {
  const cmap_pair* accessor = NULL;
  ccol_retval ret = sc_insert_impl(map, key_pair, val_pair, &accessor, NULL);
  if (accessor) {
    *slot_out = *accessor;
  }
  return ret;
}

// Inserts a key that's absent and hands out the key and value accessors of its
// node either way; the backend of ccol_chmap_insert_or_get_entry().
static ccol_retval sc_insert_or_get_entry(sep_chain_map* map,
                                          const cmap_pair* key_pair,
                                          const cmap_pair* val_pair,
                                          const cmap_pair** slot_out,
                                          const cmap_pair** key_slot_out) {
  return sc_insert_impl(map, key_pair, val_pair, slot_out, key_slot_out);
}

// Finds a key and points *val_pair at its node's value accessor, which stays
// put until the key is deleted; sc_get() and chashmap__get_val_accessor() use
// it. Forced inline into both; a call costs a string lookup about 15
// percent.
static inline __attribute__((always_inline)) ccol_retval sc_find_accessor(
    sep_chain_map* map, const cmap_pair* key_pair, const cmap_pair** val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, sc_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->bucket_arr_size);

  llist_node* node = sc_find_in_llist(
      map->bucket_arr[index], hash_val, key_pair->ptr, key_pair->size,
      map->key_type, map->custom_key_equality_proc, NULL);
  if (node) {
    *val_pair = &node->val_pair_accessor;
    return ccol_success;
  }
  return ccol_key_not_found;
}

// Looks up a key in a chaining map and copies its value accessor into the
// caller's pair, or {NULL, 0}.
static ccol_retval sc_get(sep_chain_map* map, const cmap_pair* key_pair,
                          cmap_pair* val_pair) {
  const cmap_pair* accessor;
  if (sc_find_accessor(map, key_pair, &accessor) == ccol_success) {
    *val_pair = *accessor;
    return ccol_success;
  }
  val_pair->ptr = NULL;
  val_pair->size = 0;
  return ccol_key_not_found;
}

// Deletes a key from a chaining map; the array shrinks once the count drops
// below elem_count_to_scale_down.
static ccol_retval sc_delete(sep_chain_map* map, const cmap_pair* key_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, sc_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->bucket_arr_size);

  bool found = false;
  map->bucket_arr[index] = sc_delete_from_llist(
      map->bucket_arr[index], &map->all_elems, map->m_procs, hash_val,
      key_pair->ptr, key_pair->size, map->key_type,
      map->custom_key_equality_proc, &found);

  if (found) {
    if (--map->elem_count < map->elem_count_to_scale_down &&
        map->bucket_arr_size > minimum_allowed_bucket_array_size) {
      sc_scale(map, false);
    }
    return ccol_success;
  }
  return ccol_key_not_found;
}

// Frees every node of a chaining map at teardown, detaching each from
// all_elems first, and frees the bucket array unless it's the in-block one.
static void sc_destroy(sep_chain_map* map) {
  llist_node** const buckets = map->bucket_arr;
  const size_t bucket_count = map->bucket_arr_size;
  ccol_memmgmt_procs* const m_procs = map->m_procs;
  for (size_t i = 0; i < bucket_count; i++) {
    sc_destroy_the_whole_llist(buckets[i], &map->all_elems, m_procs);
  }
  size_t inline_count = 0;
  if (buckets != sc_inline_bucket_arr(map, &inline_count)) {
    ccol__mem_free(m_procs, buckets);
  }
}

// Clears every entry of a chaining map, for chashmap_reset(), and resizes the
// array unless new_bucket_array_size is 0. It checks the byte count itself,
// because a product that wraps to 0 could make realloc free the array. Every
// element is gone whatever it returns, and the in-block array is taken back
// or replaced, never reallocated.
static ccol_retval sc_reset(sep_chain_map* map, size_t new_bucket_array_size) {
  {
    llist_node** const buckets = map->bucket_arr;
    const size_t bucket_count = map->bucket_arr_size;
    ccol_memmgmt_procs* const m_procs = map->m_procs;
    for (size_t i = 0; i < bucket_count; i++) {
      buckets[i] =
          sc_destroy_the_whole_llist(buckets[i], &map->all_elems, m_procs);
    }
  }

  if (new_bucket_array_size > 0 &&
      new_bucket_array_size != map->bucket_arr_size) {
    if (new_bucket_array_size > SIZE_MAX / sizeof(llist_node*)) {
      memset(map->bucket_arr, 0, map->bucket_arr_size * sizeof(llist_node*));
      map->elem_count = 0;
      sc_set_scaling_limits(map);
      sc_window_restart(map);
      return ccol_not_enough_memory;
    }
    size_t inline_count = 0;
    llist_node** inline_arr = sc_inline_bucket_arr(map, &inline_count);
    llist_node** orig = map->bucket_arr;
    if (new_bucket_array_size == inline_count) {
      ccol__mem_free(map->m_procs, orig);
      map->bucket_arr = inline_arr;
    } else if (orig == inline_arr) {
      map->bucket_arr = ccol__mem_alloc(
          map->m_procs, new_bucket_array_size * sizeof(llist_node*));
    } else {
      map->bucket_arr =
          ccol__mem_realloc(map->m_procs, map->bucket_arr,
                            new_bucket_array_size * sizeof(llist_node*));
    }
    if (!map->bucket_arr) {
      map->bucket_arr = orig;
      memset(map->bucket_arr, 0, map->bucket_arr_size * sizeof(llist_node*));
      map->elem_count = 0;
      sc_set_scaling_limits(map);
      sc_window_restart(map);
      return ccol_not_enough_memory;
    }
    map->bucket_arr_size = new_bucket_array_size;
  }

  memset(map->bucket_arr, 0, map->bucket_arr_size * sizeof(llist_node*));
  map->elem_count = 0;
  sc_set_scaling_limits(map);
  sc_window_restart(map);

  return ccol_success;
}

// The iterator behind a cmap_iterator of a map. free_fn is copied from the
// map, because a scoped iterator can be freed after the map and its procs are
// gone.
typedef struct chmap_cmap_iterator {
  chmap parent_map;
  ccol_free free_fn;
  union {
    dllist_ref_node* sc_tracker;
    size_t oa_index;
  } iter;
  cmap_pair oa_key_pair;
  cmap_pair oa_val_pair;
  cmap_iterator user_iter;
} chmap_cmap_iterator;

// Gives the chmap_cmap_iterator that holds the cmap_iterator u_iter points
// at, so the next and free functions reach their state from the caller's
// pointer.
#define cmapIter2ChmapIter(u_iter)          \
  (chmap_cmap_iterator*)((uint8_t*)u_iter - \
                         offsetof(chmap_cmap_iterator, user_iter))

// Creates a map in one block (see struct chashmap) and picks the backend; the
// body of every constructor. A custom equality without a custom hash is
// refused, and it forces separate chaining so the open-addressing probe stays
// free of an indirect call. An interned procs pointer is kept, any other is
// copied into the block. The process secret is picked here, before any hash.
// sc_min_buckets is the smallest first chaining array: the public minimum, or
// compact_bucket_array_size. Gives NULL with *err set on failure.
static chmap chmap_create_impl(size_t initial_bucket_array_size,
                               ccol_data_type key_type, ccol_data_type val_type,
                               ccol_memmgmt_procs* mmgmt_procs,
                               ccol_hashing_proc custom_hashing_proc,
                               ccol_key_equality_proc custom_key_equality_proc,
                               size_t sc_min_buckets, char** err) {
  if (initial_bucket_array_size == 0) {
    if (err) *err = CCOL__ERR_STR("initial_bucket_array_size is zero");
    return NULL;
  }

  if (custom_key_equality_proc && !custom_hashing_proc) {
    if (err) {
      *err = CCOL__ERR_STR(
          "custom_key_equality_proc needs a custom_hashing_proc that agrees "
          "with it");
    }
    return NULL;
  }

  if (!ccol__verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  bool open_addressing = !custom_key_equality_proc &&
                         should_use_open_addressing(key_type, val_type);
  size_t floor =
      open_addressing ? minimum_allowed_bucket_array_size : sc_min_buckets;

  if (initial_bucket_array_size <= floor) {
    initial_bucket_array_size = floor;
  } else {
    initial_bucket_array_size =
        ccol__find_nearest_gte_power_of_two(initial_bucket_array_size);
    if (initial_bucket_array_size > ccol_max_elem_count) {
      if (err) {
        *err = CCOL__ERR_STR("Initial bucket array size is too big");
      }
      return NULL;
    }
  }

  size_t inline_buckets =
      (!open_addressing &&
       initial_bucket_array_size <= minimum_allowed_bucket_array_size)
          ? initial_bucket_array_size
          : 0;
  bool copy_procs = mmgmt_procs && !ccol_procs_is_interned(mmgmt_procs);
  size_t procs_offset =
      sizeof(struct chashmap) + inline_buckets * sizeof(llist_node*);
  size_t block_bytes =
      procs_offset + (copy_procs ? sizeof(ccol_memmgmt_procs) : 0);

  chmap chm = (chmap)ccol__mem_alloc(mmgmt_procs, block_bytes);
  if (!chm) {
    if (err) *err = CCOL__ERR_STR("Failed to allocate chashmap");
    return NULL;
  }
  chm->hash_mode = CHMAP_HASH_FAST;
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound = UINT16_MAX;
  memset(chm + 1, 0, inline_buckets * sizeof(llist_node*));

  ccol_memmgmt_procs* m_procs = mmgmt_procs;
  if (copy_procs) {
    m_procs = (ccol_memmgmt_procs*)((uint8_t*)chm + procs_offset);
    memcpy(m_procs, mmgmt_procs, sizeof(ccol_memmgmt_procs));
  }

  (void)chmap_process_hash_secret();

  if (open_addressing) {
    chm->impl_type = IMPL_OPEN_ADDRESSING;
    chm->impl.oa = &chm->state.oa;
    if (!oa_init(chm->impl.oa, initial_bucket_array_size, key_type, val_type,
                 get_type_size(key_type), get_type_size(val_type), m_procs,
                 custom_hashing_proc)) {
      if (err) *err = CCOL__ERR_STR("Failed to create open addressing map");
      ccol__mem_free(mmgmt_procs, chm);
      return NULL;
    }
    oa_window_restart(chm->impl.oa);
  } else {
    chm->impl_type = IMPL_SEPARATE_CHAINING;
    chm->impl.sc = &chm->state.sc;
    chm->state.sc.inline_buckets = (uint8_t)inline_buckets;
    if (!sc_init(chm->impl.sc, initial_bucket_array_size, key_type, val_type,
                 m_procs, custom_hashing_proc, custom_key_equality_proc)) {
      if (err) *err = CCOL__ERR_STR("Failed to create separate chaining map");
      ccol__mem_free(mmgmt_procs, chm);
      return NULL;
    }
  }

  if (err) *err = NULL;
  return chm;
}

// Creates a map with every option; the constructor behind chashmap_create()
// and the chmap_construct macros. It applies the public minimum bucket count;
// the rest is chmap_create_impl().
chmap chashmap_create_full(size_t initial_bucket_array_size,
                           ccol_data_type key_type, ccol_data_type val_type,
                           ccol_memmgmt_procs* mmgmt_procs,
                           ccol_hashing_proc custom_hashing_proc,
                           ccol_key_equality_proc custom_key_equality_proc,
                           char** err) {
  return chmap_create_impl(initial_bucket_array_size, key_type, val_type,
                           mmgmt_procs, custom_hashing_proc,
                           custom_key_equality_proc,
                           minimum_allowed_bucket_array_size, err);
}

// Creates a map whose chaining bucket array starts at
// compact_bucket_array_size inside its block, for the JSON and YAML parsers,
// which make many small dictionaries. Gives NULL with *err set on failure.
chmap ccol_chmap_create_compact(ccol_data_type key_type,
                                ccol_data_type val_type,
                                ccol_memmgmt_procs* mmgmt_procs, char** err) {
  return chmap_create_impl(compact_bucket_array_size, key_type, val_type,
                           mmgmt_procs, NULL, NULL, compact_bucket_array_size,
                           err);
}

// Give the key type and the value type a map was made with, whatever the
// backend, for the size checks of the public entry points.
static inline ccol_data_type chmap_key_type(chmap chm) {
  return chm->impl_type == IMPL_OPEN_ADDRESSING ? chm->impl.oa->key_type
                                                : chm->impl.sc->key_type;
}

static inline ccol_data_type chmap_val_type(chmap chm) {
  return chm->impl_type == IMPL_OPEN_ADDRESSING ? chm->impl.oa->val_type
                                                : chm->impl.sc->val_type;
}

// Gives the canonical form of a key for hashing and comparing: for a float or
// double key a copy in out_canon_* with -0.0 turned into 0.0, so the bitwise
// key equality agrees with ==. Every other key, and every NaN payload, comes
// back as it is.
static inline const cmap_pair* canonicalize_key_pair_if_needed(
    const cmap_pair* key_pair, ccol_data_type key_type, uint64_t* out_canon_buf,
    cmap_pair* out_canon_pair) {
  if ((key_type != ccol_float && key_type != ccol_double) ||
      key_pair->size > sizeof(*out_canon_buf)) {
    return key_pair;
  }

  *out_canon_buf = 0;
  memcpy(out_canon_buf, key_pair->ptr, key_pair->size);

  if (key_type == ccol_float) {
    uint32_t bits;
    memcpy(&bits, out_canon_buf, sizeof(bits));
    if (bits == 0x80000000u) {
      memset(out_canon_buf, 0, sizeof(bits));
    }
  } else if (*out_canon_buf == 0x8000000000000000ULL) {
    *out_canon_buf = 0;
  }

  out_canon_pair->ptr = out_canon_buf;
  out_canon_pair->size = key_pair->size;
  return out_canon_pair;
}

// Gives the keyed hash of the canonical form of a key, so clrucache and other
// modules agree with the map about key identity; see chashkey.h.
size_t ccol_chmap_hash_key(const void* key_ptr, size_t key_size,
                           ccol_data_type key_type) {
  cmap_pair given = {.ptr = (void*)key_ptr, .size = key_size};
  uint64_t canon_buf = 0;
  cmap_pair canon_pair = {.ptr = NULL, .size = 0};
  const cmap_pair* key = canonicalize_key_pair_if_needed(
      &given, key_type, &canon_buf, &canon_pair);
  const chmap_hash_secret* secret = chmap_process_hash_secret();
  return hash_key_data(key->ptr, key->size, key_type, NULL, true,
                       secret->int_seed);
}

// Tells whether a value fits an open-addressing slot exactly. The insert
// copies without a bound, so a different size would overwrite the next
// slot.
static inline bool oa_val_size_matches(const open_addr_map* map,
                                       size_t val_size) {
  return val_size == map->val_size;
}

// Tells whether key_size is right for the key type, checked by every entry
// point on both backends: for a fixed-width type
// (ccol_fixed_width_data_type_size(), shared with cbstmap) it must be that
// width, because the hash, the canonical form and the long double compare read
// that many bytes. Keys with no fixed width always pass.
static inline bool key_size_matches_type_if_fixed_width(ccol_data_type key_type,
                                                        size_t key_size) {
  size_t fixed_width = ccol_fixed_width_data_type_size(key_type);
  return fixed_width == 0 || key_size == fixed_width;
}

// Tells whether val_size is right for a fixed-width value type. Separate
// chaining would store a wrong size safely, but the first chmap_get() would
// then stop the program at a call site that did nothing wrong. Only the raw
// layer can get here with a wrong size.
static inline bool val_size_matches_type_if_fixed_width(ccol_data_type val_type,
                                                        size_t val_size) {
  size_t fixed_width = ccol_fixed_width_data_type_size(val_type);
  return fixed_width == 0 || val_size == fixed_width;
}

// Checks the inputs of an insert and calls the backend; the body of the three
// public inserts. slot_out is as in oa_insert_impl(); entry_val_out and
// key_slot_out are sc_insert_impl()'s and come together, only for a chaining
// map.
static inline __attribute__((always_inline)) ccol_retval chmap_insert_elem_impl(
    chmap chm, const cmap_pair* key_pair, const cmap_pair* val_pair,
    cmap_pair* slot_out, const cmap_pair** entry_val_out,
    const cmap_pair** key_slot_out) {
  if (!chm || !key_pair || !val_pair || !key_pair->ptr || !val_pair->ptr ||
      key_pair->size == 0 || val_pair->size == 0) {
    return ccol_invalid_args;
  }

  ccol_data_type key_type = chmap_key_type(chm);
  if (!key_size_matches_type_if_fixed_width(key_type, key_pair->size)) {
    return ccol_invalid_args;
  }

  if (!val_size_matches_type_if_fixed_width(chmap_val_type(chm),
                                            val_pair->size)) {
    return ccol_invalid_args;
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING &&
      !oa_val_size_matches(chm->impl.oa, val_pair->size)) {
    return ccol_invalid_args;
  }

  uint64_t canon_buf;
  cmap_pair canon_pair;
  key_pair = canonicalize_key_pair_if_needed(key_pair, key_type, &canon_buf,
                                             &canon_pair);

  if (key_slot_out) {
    return sc_insert_or_get_entry(chm->impl.sc, key_pair, val_pair,
                                  entry_val_out, key_slot_out);
  }
  if (slot_out) {
    if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
      return oa_insert_or_get(chm->impl.oa, key_pair, val_pair, slot_out);
    }
    return sc_insert_or_get(chm->impl.sc, key_pair, val_pair, slot_out);
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return oa_insert(chm->impl.oa, key_pair, val_pair);
  } else {
    return sc_insert(chm->impl.sc, key_pair, val_pair);
  }
}

// Inserts a pair or replaces the value of a present key; the raw entry point
// behind chmap_insert(). Gives ccol_success, ccol_key_already_present or the
// error of the checks and the backend.
ccol_retval chashmap_insert_elem(chmap chm, const cmap_pair* key_pair,
                                 const cmap_pair* val_pair) {
  return chmap_insert_elem_impl(chm, key_pair, val_pair, NULL, NULL, NULL);
}

// Inserts a key that's absent and fills *val_slot with its value pair either
// way, in one hash and one probe, so a parser can replace an owning value
// without a second lookup. *val_slot is {NULL, 0} on failure.
ccol_retval ccol_chmap_insert_or_get_elem(chmap chm, const cmap_pair* key_pair,
                                          const cmap_pair* val_pair,
                                          cmap_pair* val_slot) {
  if (!val_slot) {
    return ccol_invalid_args;
  }
  val_slot->ptr = NULL;
  val_slot->size = 0;
  return chmap_insert_elem_impl(chm, key_pair, val_pair, val_slot, NULL, NULL);
}

// Gives the oldest entry of a chaining map (the list's tail), the start of the
// ordered walk of chashinsert.h, or NULL. An entry reference is the address of
// a node's dllist_refs, so a step allocates nothing.
const ccol_chmap_entry_ref* ccol_chmap_oldest_entry(chmap chm) {
  if (!chm || chm->impl_type != IMPL_SEPARATE_CHAINING) {
    return NULL;
  }
  return (const ccol_chmap_entry_ref*)chm->impl.sc->all_elems.tail;
}

// Gives the key and value accessors of an entry and the next newer entry (its
// prev link), or NULL at the newest.
const ccol_chmap_entry_ref* ccol_chmap_entry_read(
    const ccol_chmap_entry_ref* entry, const cmap_pair** key,
    const cmap_pair** val) {
  dllist_ref_node* tracker = (dllist_ref_node*)entry;
  llist_node* host = dllistRefNodePtr2LlistNodePtr(tracker);
  *key = &host->key_pair_accessor;
  *val = &host->val_pair_accessor;
  return (const ccol_chmap_entry_ref*)tracker->prev;
}

// Gives the next live entry of a walk over either backend, so a teardown can
// reach every value without allocating: chaining maps follow the ordered
// cursor, open-addressing maps the slots. Gives false at the end or for a NULL
// chm.
bool ccol_chmap_walk_next(chmap chm, ccol_chmap_walk* walk, const void** key,
                          const void** val) {
  if (!chm) {
    return false;
  }
  if (chm->impl_type == IMPL_SEPARATE_CHAINING) {
    if (!walk->started) {
      walk->started = true;
      walk->next_entry = ccol_chmap_oldest_entry(chm);
    }
    if (!walk->next_entry) {
      return false;
    }
    const cmap_pair* k;
    const cmap_pair* v;
    walk->next_entry = ccol_chmap_entry_read(walk->next_entry, &k, &v);
    *key = k->ptr;
    *val = v->ptr;
    return true;
  }
  open_addr_map* map = chm->impl.oa;
  size_t i = walk->next_slot;
  while (i < map->capacity && !(map->metadata[i] & SLOT_OCCUPIED)) {
    i++;
  }
  if (i >= map->capacity) {
    walk->next_slot = i;
    return false;
  }
  walk->next_slot = i + 1;
  *key = &map->slots[i].key_data;
  *val = &map->slots[i].val_data;
  return true;
}

// ccol_chmap_insert_or_get_elem() that also hands out the stored key's
// accessor. Only a chaining node has one, so an open-addressing map gives
// ccol_invalid_args; both out parameters are NULL on failure.
ccol_retval ccol_chmap_insert_or_get_entry(chmap chm, const cmap_pair* key_pair,
                                           const cmap_pair* val_pair,
                                           const cmap_pair** key_slot,
                                           const cmap_pair** val_slot) {
  if (!key_slot || !val_slot) {
    return ccol_invalid_args;
  }
  *key_slot = NULL;
  *val_slot = NULL;
  if (!chm || chm->impl_type != IMPL_SEPARATE_CHAINING) {
    return ccol_invalid_args;
  }
  return chmap_insert_elem_impl(chm, key_pair, val_pair, NULL, val_slot,
                                key_slot);
}

// Fills *val_pair with the location and size of the stored value of a key;
// the lookup behind chmap_get() and chmap_get_ptr(). Every failure sets it to
// {NULL, 0}.
ccol_retval chashmap_get_elem_ref(chmap chm, const cmap_pair* key_pair,
                                  cmap_pair* val_pair) {
  if (!val_pair) {
    return ccol_invalid_args;
  }
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0) {
    val_pair->ptr = NULL;
    val_pair->size = 0;
    return ccol_invalid_args;
  }

  ccol_data_type key_type = chmap_key_type(chm);
  if (!key_size_matches_type_if_fixed_width(key_type, key_pair->size)) {
    val_pair->ptr = NULL;
    val_pair->size = 0;
    return ccol_invalid_args;
  }

  uint64_t canon_buf;
  cmap_pair canon_pair;
  key_pair = canonicalize_key_pair_if_needed(key_pair, key_type, &canon_buf,
                                             &canon_pair);

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return oa_get(chm->impl.oa, key_pair, val_pair);
  } else {
    return sc_get(chm->impl.sc, key_pair, val_pair);
  }
}

// Gives the node's value accessor of a key, whose ptr field is the stored
// char * that the string arm of chmap_get_ptr() hands out. An
// open-addressing map never holds a string and gives ccol_invalid_args; every
// failure sets *val_accessor to NULL.
ccol_retval chashmap__get_val_accessor(chmap chm, const cmap_pair* key_pair,
                                       const cmap_pair** val_accessor) {
  if (!val_accessor) {
    return ccol_invalid_args;
  }
  *val_accessor = NULL;
  if (!chm || !key_pair || !key_pair->ptr || key_pair->size == 0 ||
      chm->impl_type != IMPL_SEPARATE_CHAINING) {
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
  ccol_retval ret = sc_find_accessor(chm->impl.sc, key_pair, val_accessor);
  if (ret != ccol_success) {
    *val_accessor = NULL;
  }
  return ret;
}

// Copies up to target_buf_size bytes of the value of a key into target_buf and
// zeroes the rest, for a caller that wants its own copy.
ccol_retval chashmap_get_elem_copy(chmap chm, const cmap_pair* key_pair,
                                   void* target_buf, size_t target_buf_size) {
  if (!target_buf || target_buf_size == 0) {
    return ccol_invalid_args;
  }
  cmap_pair val_pair;
  ccol_retval ret = chashmap_get_elem_ref(chm, key_pair, &val_pair);
  if (ret == ccol_success) {
    size_t copy_size =
        val_pair.size < target_buf_size ? val_pair.size : target_buf_size;
    memcpy(target_buf, val_pair.ptr, copy_size);
    if (val_pair.size < target_buf_size) {
      memset((uint8_t*)target_buf + val_pair.size, 0,
             target_buf_size - val_pair.size);
    }
  }
  return ret;
}

// Deletes a key and its value; the raw entry point behind chmap_remove(). It
// checks the inputs and calls the backend's delete.
ccol_retval chashmap_delete_elem(chmap chm, const cmap_pair* key_pair) {
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
    return oa_delete(chm->impl.oa, key_pair);
  } else {
    return sc_delete(chm->impl.sc, key_pair);
  }
}

// Gives the number of live entries of either backend. Asserts on a NULL chm.
size_t chashmap_elem_count(chmap chm) {
  if (!chm) {
    ccol__assert(false);
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->count;
  } else {
    return chm->impl.sc->elem_count;
  }
}

// Removes every entry and resizes the array when new_bucket_array_size isn't
// 0, rounded up to a power of two (at least the minimum). A size too big to
// round falls back to 0, so the reset still destroys every element.
ccol_retval chashmap_reset(chmap chm, size_t new_bucket_array_size) {
  if (!chm) {
    ccol__assert(false);
  }

  bool requested_size_too_big = false;
  if (new_bucket_array_size > 0 &&
      new_bucket_array_size < minimum_allowed_bucket_array_size) {
    new_bucket_array_size = minimum_allowed_bucket_array_size;
  } else if (new_bucket_array_size > 0) {
    new_bucket_array_size =
        ccol__find_nearest_gte_power_of_two(new_bucket_array_size);
    if (new_bucket_array_size > ccol_max_elem_count) {
      new_bucket_array_size = 0;
      requested_size_too_big = true;
    }
  }

  ccol_retval r;
  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    r = oa_reset(chm->impl.oa, new_bucket_array_size);
  } else {
    r = sc_reset(chm->impl.sc, new_bucket_array_size);
  }

  if (requested_size_too_big) {
    return ccol_not_enough_memory;
  }
  return r;
}

static cmap_iterator* chmap_iter_next(cmap_iterator* iter);

// The net count of iterators that chashmap_begin_iter() allocated and nothing
// freed, which a test destructor checks. valgrind reports a live iterator as
// possibly lost (an interior pointer), so it can't guard this.
#ifdef RUNNING_UNIT_TESTS
static long g_chmap_iter_outstanding_for_tests = 0;
// Gives the net count of live chashmap iterators.
long chashmap_iter_outstanding_count_for_tests(void) {
  return __atomic_load_n(&g_chmap_iter_outstanding_for_tests, __ATOMIC_SEQ_CST);
}
#endif

// Creates an iterator at the newest entry of a chaining map or the first
// occupied slot; the begin function behind ccol_begin(). Gives NULL with no
// error for an empty or NULL map, and NULL with *err set when the allocation
// fails.
cmap_iterator* chashmap_begin_iter(chmap chm, char** err) {
  if (err) {
    *err = NULL;
  }

  if (!chm) {
    return NULL;
  }

  if (chm->impl_type == IMPL_SEPARATE_CHAINING) {
    if (!chm->impl.sc->all_elems.head) {
      return NULL;
    }

    ccol_memmgmt_procs* m_procs = chm->impl.sc->m_procs;
    chmap_cmap_iterator* real_iter =
        ccol__mem_calloc(m_procs, 1, sizeof(chmap_cmap_iterator));
    if (!real_iter) {
      if (err) {
        *err = CCOL__ERR_STR("Failed to allocate iterator");
      }
      return NULL;
    }

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif
    dllist_ref_node* tracker = chm->impl.sc->all_elems.head;
    real_iter->parent_map = chm;
    real_iter->free_fn = m_procs ? m_procs->free : NULL;
    real_iter->iter.sc_tracker = tracker;
    llist_node* host = dllistRefNodePtr2LlistNodePtr(tracker);
    real_iter->user_iter.key_pair = &(host->key_pair_accessor);
    real_iter->user_iter.val_pair = &(host->val_pair_accessor);
    real_iter->user_iter.ccol__next_fn = chmap_iter_next;
    real_iter->user_iter.ccol__free_fn = chashmap__iterator_destroy;
    real_iter->user_iter.ccol__direct_ptr = false;

    return &(real_iter->user_iter);
  } else {
    open_addr_map* map = chm->impl.oa;

    size_t i = 0;
    while (i < map->capacity && !(map->metadata[i] & SLOT_OCCUPIED)) {
      i++;
    }

    if (i >= map->capacity) {
      return NULL;
    }

    chmap_cmap_iterator* real_iter =
        ccol__mem_calloc(map->m_procs, 1, sizeof(chmap_cmap_iterator));
    if (!real_iter) {
      if (err) {
        *err = CCOL__ERR_STR("Failed to allocate iterator");
      }
      return NULL;
    }

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif
    real_iter->parent_map = chm;
    real_iter->free_fn = map->m_procs ? map->m_procs->free : NULL;
    real_iter->iter.oa_index = i;

    real_iter->oa_key_pair.ptr = &map->slots[i].key_data;
    real_iter->oa_key_pair.size = map->key_size;
    real_iter->oa_val_pair.ptr = &map->slots[i].val_data;
    real_iter->oa_val_pair.size = map->val_size;

    real_iter->user_iter.key_pair = &real_iter->oa_key_pair;
    real_iter->user_iter.val_pair = &real_iter->oa_val_pair;
    real_iter->user_iter.ccol__next_fn = chmap_iter_next;
    real_iter->user_iter.ccol__free_fn = chashmap__iterator_destroy;
    real_iter->user_iter.ccol__direct_ptr = false;

    return &(real_iter->user_iter);
  }
}

// Frees an iterator with its own free_fn, the free function every map
// iterator carries. It reads nothing of the map, which may be gone already.
void chashmap__iterator_destroy(cmap_iterator* iter) {
  if (iter) {
    chmap_cmap_iterator* real_iter = cmapIter2ChmapIter(iter);
#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_sub(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif
    ccol_free free_fn = real_iter->free_fn;
    if (free_fn) {
      free_fn(real_iter);
    } else {
      ccol__default_mem_free(real_iter);
    }
  }
}

// Moves an iterator to the next entry (next link, or next occupied slot); the
// next function of every map iterator. At the end it frees the iterator and
// gives NULL.
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
      chashmap__iterator_destroy(iter);
      iter = NULL;
    }
  } else {
    open_addr_map* map = real_iter->parent_map->impl.oa;
    size_t i = real_iter->iter.oa_index + 1;

    while (i < map->capacity && !(map->metadata[i] & SLOT_OCCUPIED)) {
      i++;
    }

    if (i >= map->capacity) {
      chashmap__iterator_destroy(iter);
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

// Frees every entry, the backend's arrays and the map's block; the body of
// chashmap_destroy(). The free function is read first, because the procs can
// be part of the block.
void chashmap__destroy(chmap chm) {
  if (chm) {
    ccol_memmgmt_procs* procs;
    if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
      procs = chm->impl.oa->m_procs;
      oa_destroy(chm->impl.oa);
    } else {
      procs = chm->impl.sc->m_procs;
      sc_destroy(chm->impl.sc);
    }
    if (procs) {
      ccol_free free_func = procs->free;
      free_func(chm);
      return;
    }
    ccol__default_mem_free(chm);
  }
}

// Destroys the map and calls val_dtor on every value right before its entry
// is freed, allocating nothing, for maps of owned pointers. The walks are
// duplicates of the backend teardowns on purpose, to keep the destructor's
// cost off the plain destroy.
void chashmap_destroy_with_dtor(chmap chm,
                                void (*val_dtor)(const cmap_pair* val_pair,
                                                 void* dtor_ctx),
                                void* dtor_ctx) {
  if (!chm) return;
  if (!val_dtor) {
    chashmap__destroy(chm);
    return;
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    open_addr_map* map = chm->impl.oa;
    for (size_t i = 0; i < map->capacity; i++) {
      if (map->metadata[i] & SLOT_OCCUPIED) {
        cmap_pair vp = {.ptr = &map->slots[i].val_data, .size = map->val_size};
        val_dtor(&vp, dtor_ctx);
      }
    }
  } else {
    sep_chain_map* map = chm->impl.sc;
    for (size_t i = 0; i < map->bucket_arr_size; i++) {
      for (llist_node* tracker = map->bucket_arr[i]; tracker;
           tracker = tracker->next) {
        val_dtor(&tracker->val_pair_accessor, dtor_ctx);
      }
    }
  }

  chashmap__destroy(chm);
}

// The test hooks that read internal state, so the tests can assert on work
// and structure instead of time. test_helpers/chashmap.h declares them.
#ifdef RUNNING_UNIT_TESTS
// Gives the size of the bucket array or the slot array.
size_t chmap_get_bucket_arr_size(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->capacity;
  } else {
    return chm->impl.sc->bucket_arr_size;
  }
}

// Gives the count at which the next growth starts.
size_t chmap_get_elem_count_to_scale_up(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    size_t cap = chm->impl.oa->capacity;
    return (cap / 10) * 7 + ((cap % 10) * 7) / 10;
  } else {
    return chm->impl.sc->elem_count_to_scale_up;
  }
}

// Gives the count at which the next shrink starts.
size_t chmap_get_elem_count_to_scale_down(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->capacity / 4;
  } else {
    return chm->impl.sc->elem_count_to_scale_down;
  }
}

// Computes the SipHash state of the key (k0, k1), as a map keeps it.
void chashmap_sip_state_for_tests(uint64_t k0, uint64_t k1, uint64_t sip_v[4]) {
  chmap_sip_state_from_key(k0, k1, sip_v);
}

// Runs SipHash-1-3 from an explicit state, to compare with a reference.
uint64_t chashmap_siphash13_for_tests(const uint64_t sip_v[4],
                                      const void* key_ptr, size_t key_size) {
  return siphash13(sip_v, key_ptr, key_size);
}

// Runs the keyed byte hash from an explicit state, to build colliding keys.
size_t chashmap_byte_hash_for_tests(const uint64_t sip_v[4],
                                    const void* key_ptr, size_t key_size) {
  return hash_bytes_keyed(sip_v, key_ptr, key_size);
}

// Gives the secret a map hashes with: sip_v, int_seed, byte_seed. An
// open-addressing map reads no byte hash and gives 0 for those parts.
void chashmap_hash_secret_for_tests(chmap chm, uint64_t out[6]) {
  memset(out, 0, 6 * sizeof(uint64_t));
  if (!chm) return;
  out[4] = g_chmap_hash_secret.int_seed;
  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return;
  }
  memcpy(out, g_chmap_hash_secret.sip_v, 4 * sizeof(uint64_t));
  out[5] = g_chmap_hash_secret.byte_seed;
}

// Gives the key words that the fallback derives when getrandom(2) can't
// answer.
void chashmap_fallback_hash_key_for_tests(uint64_t out[4]) {
  chmap_fallback_hash_key(out);
}

// Runs XXH64 with an explicit seed, to compare with the published vectors.
uint64_t chashmap_xxh64_for_tests(const void* key_ptr, size_t key_size,
                                  uint64_t seed) {
  return xxh64(key_ptr, key_size, seed);
}

// Tells whether a map hashes in the keyed mode.
bool chashmap_is_keyed_for_tests(chmap chm) {
  return chm && chm->hash_mode == CHMAP_HASH_KEYED;
}

// Switches a map to keyed through the detection's own function; gives
// ccol_not_enough_memory, the map staying fast, when the rebuild fails.
ccol_retval chashmap_switch_to_keyed_for_tests(chmap chm) {
  if (!chm) return ccol_invalid_args;
  if (chm->hash_mode == CHMAP_HASH_KEYED) return ccol_success;
  if (chm->impl_type == IMPL_SEPARATE_CHAINING) {
    sc_switch_to_keyed(chm->impl.sc);
    return ccol_success;
  }
  open_addr_map* map = chm->impl.oa;
  return oa_rebuild(map, map->capacity, true);
}

// Gives the probe_span of an open-addressing map, or 0.
size_t chashmap_oa_probe_span_for_tests(chmap chm) {
  if (!chm || chm->impl_type != IMPL_OPEN_ADDRESSING) return 0;
  return chm->impl.oa->probe_span;
}

// Gives oa_insert_cap() of a capacity.
size_t chashmap_oa_insert_cap_for_tests(size_t capacity) {
  return oa_insert_cap(capacity);
}

// Gives oa_window_bound() of a window start and a capacity.
uint32_t chashmap_oa_window_bound_for_tests(size_t start_count,
                                            size_t capacity) {
  return oa_window_bound(start_count, capacity);
}

// Gives sc_window_bound() of a window start and a bucket count.
uint32_t chashmap_sc_window_bound_for_tests(size_t start_count,
                                            size_t bucket_count) {
  sep_chain_map probe;
  memset(&probe, 0, sizeof(probe));
  probe.bucket_arr_size = bucket_count;
  sc_set_scaling_limits(&probe);
  return sc_window_bound(start_count, probe.elem_count_to_scale_up,
                         bucket_count);
}

// Gives the bound of a map's current window.
uint32_t chashmap_window_bound_of_for_tests(chmap chm) {
  return chm ? chm->window_bound : 0;
}

// Gives the running sum of a map's current window.
uint32_t chashmap_insert_window_for_tests(chmap chm) {
  return chm ? chm->insert_window : 0;
}

// Gives the longest chain of a chaining map, or 0.
size_t chashmap_sc_longest_chain_for_tests(chmap chm) {
  if (!chm || chm->impl_type != IMPL_SEPARATE_CHAINING) return 0;
  sep_chain_map* map = chm->impl.sc;
  size_t longest = 0;
  for (size_t i = 0; i < map->bucket_arr_size; i++) {
    size_t len = 0;
    for (llist_node* n = map->bucket_arr[i]; n; n = n->next) len++;
    if (len > longest) longest = len;
  }
  return longest;
}

// Gives the hash a chaining map stored for a key, or 0.
size_t chashmap_sc_stored_hash_for_tests(chmap chm, const void* key_ptr,
                                         size_t key_size) {
  if (!chm || chm->impl_type != IMPL_SEPARATE_CHAINING) return 0;
  for (dllist_ref_node* t = chm->impl.sc->all_elems.head; t; t = t->next) {
    llist_node* n = dllistRefNodePtr2LlistNodePtr(t);
    if (n->key_pair_accessor.size == key_size &&
        memcmp(n->key_pair_accessor.ptr, key_ptr, key_size) == 0) {
      return n->hash_val;
    }
  }
  return 0;
}

// Gives a key's home slot in an open-addressing map, or SIZE_MAX.
size_t chashmap_oa_home_slot_for_tests(chmap chm, const void* key_ptr,
                                       size_t key_size) {
  if (!chm || chm->impl_type != IMPL_OPEN_ADDRESSING) return SIZE_MAX;
  open_addr_map* map = chm->impl.oa;
  return hash_index_for(
      hash_key_data(key_ptr, key_size, map->key_type, map->custom_hashing_proc,
                    oa_keyed(map), g_chmap_hash_secret.int_seed),
      map->capacity);
}

// Checks that every occupied slot is reachable from its home through occupied
// slots within probe_span, and that exactly count slots are occupied. Gives
// true for a chaining map.
bool chashmap_oa_check_invariants_for_tests(chmap chm) {
  if (!chm || chm->impl_type != IMPL_OPEN_ADDRESSING) return true;
  open_addr_map* map = chm->impl.oa;
  size_t mask = map->capacity - 1;
  size_t occupied = 0;
  for (size_t j = 0; j < map->capacity; j++) {
    if (!(map->metadata[j] & SLOT_OCCUPIED)) {
      if (map->metadata[j] != 0) return false;
      continue;
    }
    occupied++;
    size_t home = hash_index_for(
        hash_key_data(&map->slots[j].key_data, map->key_size, map->key_type,
                      map->custom_hashing_proc, oa_keyed(map),
                      g_chmap_hash_secret.int_seed),
        map->capacity);
    for (size_t k = home; k != j; k = (k + 1) & mask) {
      if (!(map->metadata[k] & SLOT_OCCUPIED)) return false;
    }
    if (((j - home) & mask) >= map->probe_span) return false;
  }
  return occupied == map->count && map->probe_span <= map->capacity &&
         map->probe_span >= oa_min_probe_span(map->capacity);
}
#endif
