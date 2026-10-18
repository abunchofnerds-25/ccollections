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

static const size_t minimum_allowed_bucket_array_size = 16;
// The first bucket array of a map made by ccol_chmap_create_compact(), a
// power of two of at least 2; see hash_index_for.
static const size_t compact_bucket_array_size = 4;
static const size_t scale_factor = 4;

#define OPEN_ADDR_MAX_LOAD_FACTOR 0.70
#define OPEN_ADDR_MIN_LOAD_FACTOR 0.25
#define INLINE_STORAGE_THRESHOLD 23  // SSO: Small String Optimization threshold

// The integer mixer and the reduction of a hash to an index each come in
// one form for a 64-bit size_t and another for a 32-bit size_t.
#if SIZE_MAX != UINT64_MAX && SIZE_MAX != UINT32_MAX
#error "Unsupported architecture: SIZE_MAX is neither UINT32_MAX nor UINT64_MAX"
#endif

/* The secret of the built-in hashes. There's one per process, picked before
 * the first map exists and never changed afterwards (see
 * chmap_process_hash_secret), and every map and ccol_chmap_hash_key read that
 * one copy, so no map stores any of it. It has three independent parts.
 *
 * sip_v is SipHash-1-3's initial state for the 128-bit key (k0, k1):
 * k0 ^ "somepseu", k1 ^ "dorandom", k0 ^ "lygenera", k1 ^ "tedbytes". We keep
 * that state rather than the key, so a hash doesn't redo those four
 * exclusive ors. The keyed byte hash of a key with no fixed width uses it,
 * and so does the hash of a long double in either mode.
 *
 * int_seed keys the mixer of every other fixed-width key, and of a custom
 * hashing proc's result, in the keyed mode; see hash_word_seeded.
 *
 * byte_seed is the seed of XXH64, the fast mode's byte hash; see
 * hash_bytes_fast. */
typedef struct chmap_hash_secret {
  uint64_t sip_v[4];
  uint64_t int_seed;
  uint64_t byte_seed;
} chmap_hash_secret;

/* ========================================================================== */
/*                    OPEN ADDRESSING STRUCTURES                              */
/* ========================================================================== */

// A 16-byte slot, an 8-byte key and an 8-byte value. The status bits live
// in a parallel byte array rather than in this struct; the metadata field of
// open_addr_map and the next note say why. The natural alignment keeps
// key_data and val_data on 8-byte boundaries at every array index, which a
// strict-alignment architecture needs to work at all, and it lets
// chmap_get_ptr hand back an aligned pointer straight into the slot array
// so the caller can change the value in place.
/* Exactly two words, with no padding and nothing a probe doesn't read. The
 * occupied bit lives in a separate byte array (see the metadata field of
 * open_addr_map) because a third one-byte member would pad this struct out
 * to 24 bytes, so a third of every cache line a probe pulls in would carry
 * nothing, and the bits a probe tests first would sit one per 24 bytes
 * instead of packed together. */
typedef struct {
  uint64_t key_data;
  uint64_t val_data;
} oa_slot;

#define SLOT_OCCUPIED 0x01

typedef struct {
  oa_slot* slots;
  // One byte per slot, with the same indices as slots[], holding that slot's
  // occupied bit. This array is the tail of the slots allocation, not an
  // allocation of its own (see oa_alloc_block), so the block's arrays can
  // never disagree about their length and a resize frees one block instead
  // of several.
  //
  // It's kept apart from oa_slot so a probe reads its bits densely: one
  // 64-byte line covers the bits of 64 slots, while a byte inside each slot
  // would need sixteen lines to test as many slots, and all but one byte of
  // each of those lines would be key and value bytes, which the probe only
  // needs for the one slot where it stops.
  uint8_t* metadata;
  size_t capacity;
  // One more than the largest displacement of any key in the table, and
  // never less than 32 or the capacity, whichever is smaller. So every key
  // sits within probe_span slots of its home slot, and a lookup, a delete and
  // the backward-shift walk stop after that many slots. That bounds a probe
  // that would otherwise walk to the end of a long run of keys with
  // consecutive home slots: each such key sits in its own home slot, so no
  // insert sees a long displacement, while the run itself can be as long as
  // the table. An insert widens it (see oa_note_deep_insert) and a rebuild
  // sets it from the table it builds; a delete only moves keys toward their
  // home slots, so it never needs to widen it.
  size_t probe_span;
  size_t count;
  // The fields above, the hash mode and the insert window in the map's
  // header, and the two below are what an insert, a lookup and a delete
  // read; the fields after them are only read on colder paths.
  ccol_hashing_proc_t custom_hashing_proc;
  ccol_data_type key_type;
  ccol_data_type val_type;
  size_t key_size;
  size_t val_size;
  ccol_memmgmt_procs_t* m_procs;
} open_addr_map;

/* ========================================================================== */
/*                 SEPARATE CHAINING STRUCTURES                               */
/* ========================================================================== */

// Not packed. Packing would save nothing here, since two pointer fields of
// the same size need no internal padding either way, and a packed struct
// could end up at an offset inside llist_node that isn't 8-byte aligned,
// for no gain.
typedef struct dllist_ref_node {
  struct dllist_ref_node* prev;
  struct dllist_ref_node* next;
} dllist_ref_node;

// The two ends of the list of all live entries: head is the newest entry
// and tail the oldest. The public iterator walks from head through next, so
// it sees the newest entry first, while chashinsert.h's internal ordered
// cursor walks from tail through prev and so sees the entries in insertion
// order.
typedef struct dllist_root {
  dllist_ref_node* head;
  dllist_ref_node* tail;
} dllist_root;

// One entry of a separate-chaining map.
//
// key_storage and val_storage keep a key or a value of up to
// INLINE_STORAGE_THRESHOLD bytes inline (SSO), and a pointer to a heap
// buffer for anything bigger. Which of the two a union holds follows from
// the size in its accessor: a key or a value is inline exactly when its size
// is at most INLINE_STORAGE_THRESHOLD, and every path that stores a value
// keeps it that way; see sc_create_llist_node and
// sc_reset_val_of_llist_node.
//
// Each union carries an explicit _Alignas(max_align_t), and the struct is
// deliberately NOT packed, because inline_data must land at an offset
// that is properly aligned for ANY type a caller can store there:
// chmap_get, chmap_get_ptr and the iterator accessor macros cast a pointer
// into this storage straight to the caller's value type and dereference it,
// also to change it in place, and a memcpy read is no safe substitute for
// that.
//
// max_align_t is the right bound here, 8 alone isn't enough. A plain
// malloc() gives the same guarantee to the heap buffer that holds anything
// bigger than INLINE_STORAGE_THRESHOLD, and since the inline storage is a
// drop-in substitute for that buffer, it must meet the same alignment
// contract for whatever type-erased bytes a caller puts there. `long double`
// needs 16 bytes on x86-64, and it's one of the built-in value types that
// always goes inline through this path. UndefinedBehaviorSanitizer reports
// less aligned storage directly on chmap_get_ptr of a `char* -> long double`
// map ("load of misaligned address ... which requires 16 byte alignment").
// It's the same root cause as the packed-struct hazard that cjson.c,
// cyaml.c, clrucache.c, cthreadcomm.c and chttpclient.c document, coming in
// through a union that under-declares its own alignment requirement.
//
// The member order fills every byte on LP64: each union is 24 bytes at a
// multiple of 16 and the 8-byte member after it fills the rest of its 32
// bytes, so the struct is 112 bytes with no padding. It also puts the two
// fields a chain walk reads at every node, hash_val and next, in the first
// 64 bytes next to an inline key's bytes. The accessors come next, and the
// insertion-order list links come last, since only an insert, a delete and
// an iteration read them.
//
// The node keeps no allocator of its own: every path that frees or resizes
// a node's buffer gets its map's procs as an argument.
typedef struct llist_node {
  _Alignas(max_align_t) union {
    void* ptr;
    // SSO: 23 bytes + null terminator
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } key_storage;
  size_t hash_val;
  _Alignas(max_align_t) union {
    void* ptr;
    // SSO: 23 bytes + null terminator
    char inline_data[INLINE_STORAGE_THRESHOLD + 1];
  } val_storage;
  struct llist_node* next;
  // The stable {ptr, size} of the stored key and the stored value that the
  // map hands out. ptr addresses the union's inline_data when the size is
  // at most INLINE_STORAGE_THRESHOLD, and the heap buffer otherwise.
  cmap_pair key_pair_accessor;
  cmap_pair val_pair_accessor;
  dllist_ref_node dllist_refs;
} llist_node;

// The layout above on LP64 with a 16-byte max_align_t, which is what x86-64
// and aarch64 have. Adding or moving a member makes this fail to compile
// rather than quietly padding every entry.
_Static_assert(SIZE_MAX != UINT64_MAX || _Alignof(max_align_t) != 16 ||
                   (sizeof(llist_node) == 112 &&
                    offsetof(llist_node, hash_val) == 24 &&
                    offsetof(llist_node, val_storage) == 32 &&
                    offsetof(llist_node, next) == 56),
               "llist_node must stay 112 bytes with no padding");

typedef struct sep_chain_map {
  size_t elem_count;
  size_t bucket_arr_size;
  size_t elem_count_to_scale_up;
  size_t elem_count_to_scale_down;
  llist_node** bucket_arr;
  dllist_root all_elems;
  ccol_memmgmt_procs_t* m_procs;
  ccol_hashing_proc_t custom_hashing_proc;
  // NULL selects the built-in key comparison of sc_compare_keys.
  ccol_key_equality_proc_t custom_key_equality_proc;
  // One ccol_data_type each, one byte wide so the three bytes share the
  // last word of the struct.
  uint8_t key_type;
  uint8_t val_type;
  // How many bucket pointers the map's block holds right after struct
  // chashmap, or 0 when it holds none (see sc_inline_bucket_arr); never more
  // than minimum_allowed_bucket_array_size.
  uint8_t inline_buckets;
} sep_chain_map;

/* ========================================================================== */
/*                      UNIFIED STRUCTURE                                     */
/* ========================================================================== */

typedef enum { IMPL_OPEN_ADDRESSING, IMPL_SEPARATE_CHAINING } hashmap_impl_type;

/* A map is a single allocation. The block holds this struct, with its
 * backend's state in state, then the bucket array a separate-chaining map
 * starts with when that array is small (see sc_inline_bucket_arr), and then
 * the map's own copy of the allocator procs when it needs one (see
 * chmap_create_impl).
 *
 * impl points at state, and an operation reaches the backend through that
 * pointer just as it would reach a backend living in its own allocation.
 * Addressing state at its fixed offset instead would save the load and 8
 * bytes, but it costs the open-addressing insert and delete two instructions
 * each, because the compiler then allocates registers differently on those
 * paths.
 *
 * The first eight bytes hold the backend, the hash mode and the insert
 * window, and both backends get to the mode and the window at a constant
 * offset back from their state; see chmap_of_oa and chmap_of_sc. */
struct chashmap {
  // A hashmap_impl_type.
  uint8_t impl_type;
  // CHMAP_HASH_FAST or CHMAP_HASH_KEYED; see the adaptive hashing
  // section.
  uint8_t hash_mode;
  // The bound of the current insert window, computed when the window
  // starts; see oa_window_bound and sc_window_bound. No bound needs more
  // than 16 bits.
  uint16_t window_bound;
  // The running sum of the insert window; see chmap_window_add.
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

/* ========================================================================== */
/*                         ADAPTIVE HASHING                                   */
/* ========================================================================== */

/* Every map starts in the fast mode and may switch to the keyed mode, and
 * each growth of a keyed map tries the fast mode again (see below). The
 * keyed mode is the hash ccol_chmap_hash_key gives: SipHash-1-3 for a byte
 * key, and the seeded mixer for a fixed-width key and for a custom hashing
 * proc's result. The fast mode is a Fibonacci multiply for a fixed-width
 * key, XXH64 with a secret seed for a byte key, and the murmur3 finalizer
 * for a custom hashing proc's result. A long double key is hashed by value
 * with SipHash-1-3 in both modes. Key identity doesn't depend on the mode: in
 * either one -0.0 and 0.0 are one key, and so are two long doubles of the
 * same value.
 *
 * The fast hashes spread every ordinary key set as well as the keyed ones
 * or better, and they're cheaper, but whoever controls the keys can pick a
 * set that collides under them, because the Fibonacci multiply and the
 * finalizer are public bijections and XXH64 has collisions that hold for
 * every seed. Only a map's writers watch for that, so a lookup never writes:
 *
 * - The insert window. Every insert of a new key adds 2^23 plus its walk
 *   (the displacement of an open-addressing slot, or the nodes of the chain
 *   the new key met) to the 32-bit insert_window, which starts at 2^31. The
 *   carry out of that add marks the end of a window of 256 inserts, and the
 *   window then holds the sum of the walks; see chmap_window_add. A fast map
 *   switches when that sum is above the bound of oa_window_bound or
 *   sc_window_bound, which is several times what a random function costs at
 *   the highest load the window can have reached. A rebuild, a reset and the
 *   switch restart the window. A keyed map keeps adding to the window but is
 *   never judged, which saves the insert path a test of the mode.
 * - A cap on a single insert. An open-addressing insert whose displacement
 *   is above oa_insert_cap, and a separate-chaining insert that meets
 *   SC_CHAIN_CAP nodes, switch at once, and rebuilding an open-addressing
 *   table, or shrinking a separate-chaining one, measures the same things on
 *   the table it builds.
 *
 * The switch rebuilds an open-addressing table into a new block with the
 * keyed hash, and rehashes a separate-chaining map's nodes in place.
 *
 * A growth of a keyed map makes a fast attempt: it fills the new table with
 * the fast hash and keeps the fast mode unless the fill shows the keys
 * collide under it; see oa_place_all_fast_attempt and
 * sc_relink_fast_attempt. A failed attempt stops early and the growth then
 * fills the same new table with the keyed hash, so nothing more is
 * allocated. Its work is linear in the count: for a map of n entries, at
 * most 2 * n + cap + 65 slots, cap being oa_insert_cap of the new table, or
 * n hashes and n + SC_ATTEMPT_SLACK + SC_CHAIN_CAP - 1 chain nodes. Growth is
 * geometric, so even a map whose every growth fails pays O(1) amortized per
 * insert. In an open-addressing table of 4096 slots or more, a growth at
 * count 0.7 * c comes after 0.35 * c inserts, which is at most about four
 * slots and two keyed placements per insert; a separate-chaining growth at
 * count 1.5 * b comes after 1.125 * b inserts, at most about three node
 * operations and 1.3 keyed hashes per insert. A map the attempt puts back in
 * the fast mode restarts its window like any rebuild, so a resumed attack
 * switches it again within two windows. A reset and a shrink keep the mode:
 * a shrink follows deletes, which say nothing new about how the keys spread,
 * and the map's next growth makes the attempt anyway. */
enum { CHMAP_HASH_FAST = 0, CHMAP_HASH_KEYED = 1 };

/* The insert window starts at 2^31 and each insert of a new key adds 2^23
 * plus its walk, so 256 inserts carry out of 32 bits exactly when their
 * walks add up to less than 2^23, and the 32 bits then hold that sum. A fast
 * map never holds a walk above its cap, so its windows always end after 256
 * inserts. */
#define CHMAP_WINDOW_START 0x80000000u
#define CHMAP_WINDOW_STEP 0x00800000u
#define CHMAP_WINDOW_INSERTS 256u

/* A new separate-chaining key that meets this many nodes switches the map. */
#define SC_CHAIN_CAP 20u

/* An open-addressing insert with a displacement of at least this many slots
 * goes to oa_note_deep_insert, which widens probe_span and checks the cap.
 * It's also the smallest probe_span of a table with at least this many
 * slots. */
#define OA_DEEP_INSERT 32u

static inline struct chashmap* chmap_of_oa(open_addr_map* map) {
  return (struct chashmap*)((uint8_t*)map -
                            offsetof(struct chashmap, state.oa));
}

static inline struct chashmap* chmap_of_sc(sep_chain_map* map) {
  return (struct chashmap*)((uint8_t*)map -
                            offsetof(struct chashmap, state.sc));
}

static inline bool oa_keyed(open_addr_map* map) {
  return chmap_of_oa(map)->hash_mode == CHMAP_HASH_KEYED;
}

static inline bool sc_keyed(sep_chain_map* map) {
  return chmap_of_sc(map)->hash_mode == CHMAP_HASH_KEYED;
}

/* Adds one insert of a new key with the given walk to the window, and gives
 * true when that ends the window. Compiles to a lea, an add to memory and a
 * jump on the carry. */
static inline bool chmap_window_add(struct chashmap* chm, size_t walk) {
  uint32_t sum;
  bool ended = __builtin_add_overflow(chm->insert_window,
                                      CHMAP_WINDOW_STEP + (uint32_t)walk, &sum);
  chm->insert_window = sum;
  return ended;
}

/* The base-2 logarithm of a power of two. */
static inline unsigned chmap_log2_pow2(size_t capacity) {
#if SIZE_MAX == UINT64_MAX
  return (unsigned)__builtin_ctzll((unsigned long long)capacity);
#else
  return (unsigned)__builtin_ctz((unsigned int)capacity);
#endif
}

/* count / capacity in 16.16 fixed point, for a power-of-two capacity. */
static inline uint64_t chmap_load_q16(size_t count, size_t capacity) {
  unsigned k = chmap_log2_pow2(capacity);
  return k >= 16 ? (uint64_t)count >> (k - 16) : (uint64_t)count << (16 - k);
}

/* ========================================================================== */
/*                      TYPE DETECTION                                        */
/* ========================================================================== */

/* True for every ccol_data_type value that fits in 8 bytes or less and that
 * the integer mixer hashes: the integers, the floats and the pointers. Used
 * at creation time to pick the map backend. */
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

/* Gives the byte size of the C type behind a ccol_data_type value, or 8 for
 * a type it doesn't know, so an open-addressing slot is always big enough
 * for a pointer-sized value. */
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

/* True when the key type and the value type are both integral and both 8
 * bytes or less, which is when the compact open-addressing backend gets
 * picked. In every other case it's the separate-chaining backend, which
 * holds a key and a value of any size, a string or a caller-defined struct
 * included. */
static inline bool should_use_open_addressing(ccol_data_type key_type,
                                              ccol_data_type val_type) {
  return is_type_integral(key_type) && is_type_integral(val_type) &&
         get_type_size(key_type) <= 8 && get_type_size(val_type) <= 8;
}

/* ========================================================================== */
/*                         HASH FUNCTIONS                                     */
/* ========================================================================== */

/* The mixer of every fixed-width key and of a custom hashing proc's result:
 * an exclusive or with the secret int_seed, a multiply, a xorshift and a
 * second multiply. hash_index_for() reads the index from the top bits of
 * the result, which are the bits the last multiply mixes best, so the final
 * xorshift of a general-purpose finalizer like murmur3 fmix64 would add
 * work that no reduction reads.
 *
 * Each step is a bijection, so two keys of one width never share the whole
 * hash. Every key bit reaches the top bits through both multiplies: the
 * first multiply carries each bit upward, the xorshift brings the upper half
 * of the product down, and the second multiply carries the whole word upward
 * again.
 *
 * The seed is what makes the reduction impossible to steer. Without it,
 * someone who controls the keys (an identifier a peer sends, say) can invert
 * any public mixer and work out offline a set of keys whose hashes share
 * their top bits, so they all land in one home slot or one bucket and n
 * inserts cost O(n^2) probes. A single multiply by a public constant keeps a
 * structure no seed gets rid of. With k * C the keys i * C^-1 all hash to i
 * and share slot 0, and with the Fibonacci constant the multiples of a large
 * Fibonacci number pile up in a few clusters. With (k ^ s) * C, a key set
 * that only varies inside a fixed group of bit positions, with all the
 * combinations, is mapped by every seed onto a translate of one and the same
 * set, so the clustering measured offline for one seed holds for every
 * seed. A secret multiplier has no such set, but then how well a run of
 * sequential keys spreads depends on which multiplier was drawn: over 300
 * random multipliers such a run costs between 1.0 and 27.7 probes per lookup
 * at a load of 0.38. In this mixer the first product of such a set is an
 * additive translate by a secret amount, and the xorshift after it turns the
 * carries of that addition into a nonlinear function of the seed before the
 * second multiply spreads it, so the top bits a set of keys shares depend on
 * the seed.
 *
 * What we pay is regularity. A bare Fibonacci multiply spreads a run of
 * sequential keys perfectly, one probe per lookup, while this mixer spreads
 * every key set, sequential runs included, the way a random function would,
 * which at a load of 0.38 is about 1.3 probes per lookup and about 2 at the
 * maximum load of 0.70. Only a map in the keyed mode pays that, and only
 * after its keys looked hostile, so it's a price well worth paying.
 *
 * A 32-bit size_t uses a 32-bit form of the same steps, with the lowbias32
 * finalizer's constants, for every key of 32 bits or less. */
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

/* The mixer for a key of exactly 64 bits: long long, unsigned long long and
 * double. On a 64-bit target that's just hash_word_seeded. On a 32-bit
 * target a size_t can't hold the key, so the 64-bit steps run at the key's
 * width and the upper word of the result is the hash, since hash_index_for
 * reads the index from the top. Folding the key's two halves together first
 * (low ^ high) would give the same hash to every key with the same exclusive
 * or of its halves, and a key packed from two 32-bit fields, like
 * (a << 32) | b, is the ordinary shape of a 64-bit key. */
static inline size_t hash_u64_seeded(uint64_t x, uint64_t seed) {
#if SIZE_MAX == UINT64_MAX
  return hash_word_seeded((size_t)x, seed);
#else
  uint64_t r = (x ^ seed) * 0xBF58476D1CE4E5B9ULL;
  r ^= r >> 32;
  return (size_t)((r * 0x94D049BB133111EBULL) >> 32);
#endif
}

/* The fast hash of a fixed-width key no wider than size_t: a Fibonacci
 * multiply, which hash_index_for reads from its top bits. The multiply is a
 * bijection, so the top bits of the product are a permutation of a run of
 * sequential keys, and such a run costs one probe per lookup; keys scaled by
 * a power of two and aligned addresses spread the same way. The multiplier
 * is public and nothing keys it, so whoever controls the keys can make any
 * number of them share a home slot; the insert window and the insert cap
 * notice that and switch the map to hash_word_seeded. */
static inline size_t hash_word_fast(size_t x) {
#if SIZE_MAX == UINT64_MAX
  return x * (size_t)0x9E3779B97F4A7C15ULL;
#else
  return x * (size_t)0x9E3779B9U;
#endif
}

/* The fast hash of a key of exactly 64 bits. On a 32-bit target the
 * multiply runs at the key's width and the upper word of the product is the
 * hash, since hash_index_for reads the index from the top. */
static inline size_t hash_u64_fast(uint64_t x) {
#if SIZE_MAX == UINT64_MAX
  return hash_word_fast((size_t)x);
#else
  return (size_t)((x * 0x9E3779B97F4A7C15ULL) >> 32);
#endif
}

/* The fast finalizer for a custom hashing proc's result: the murmur3
 * finalizer, which makes every output bit depend on every input bit. It's
 * needed because a custom hash may put its entropy anywhere, in the low bits
 * of an identity or a counter for example, while hash_index_for reads the
 * top bits. The finalizer is public and unseeded; hash_word_fast says what
 * that leaves to the detection. */
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

/* SipHash-1-3, the keyed byte hash of every key with no fixed width: a
 * string, a struct and any other buffer. SipHash is a pseudorandom function
 * of its 128-bit key, so someone who doesn't know the key can't find two
 * inputs whose hashes collide, fully or in their top bits, any better than
 * by chance, and no pair of inputs collides for every key. A byte hash that
 * is only seeded, not keyed in this sense, can have pairs of inputs that
 * collide whatever the seed is, and such pairs are enough to put any number
 * of keys into one bucket. The variant with one compression round and three
 * finalization rounds is what the hash tables of Rust and Python use for the
 * same purpose. The input is read as little-endian 64-bit words, as the
 * algorithm defines it, through memcpy, so a key's address needs no
 * alignment. */
static inline uint64_t sip_rotl(uint64_t x, unsigned b) {
  return (x << b) | (x >> (64u - b));
}

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

/* The last 0 to 7 bytes as a little-endian word. Four to seven bytes take
 * two overlapping 4-byte reads, and one to three bytes take the first, the
 * middle and the last byte. The bytes two reads share are equal, so or-ing
 * the two never disturbs them. */
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

/* SipHash-1-3 of a buffer as a size_t; a 32-bit size_t keeps both halves of
 * the output. */
static inline size_t hash_bytes_keyed(const uint64_t sip_v[4],
                                      const void* input, size_t len) {
  uint64_t h = siphash13(sip_v, input, len);
#if SIZE_MAX == UINT64_MAX
  return (size_t)h;
#else
  return (size_t)(h ^ (h >> 32));
#endif
}

/* XXH64, the fast mode's byte hash, exactly as its specification defines
 * it, with the input read as little-endian words through memcpy. It's seeded
 * with a secret, so someone who doesn't know the seed can't work out
 * colliding keys by trying keys offline. XXH64 isn't a pseudorandom function
 * of its seed though, and some key sets collide under every seed: a round
 * over one lane, acc = rotl(acc + w * P2, 31) * P1, turns a change of 2^63 in
 * acc + w * P2 into a change of plus or minus 2^30 after the rotation, and
 * the next word of the same lane can cancel either sign. The insert window
 * and the chain cap notice such a set and switch the map to SipHash-1-3. */
#define XXH_P1 0x9E3779B185EBCA87ULL
#define XXH_P2 0xC2B2AE3D27D4EB4FULL
#define XXH_P3 0x165667B19E3779F9ULL
#define XXH_P4 0x85EBCA77C2B2AE63ULL
#define XXH_P5 0x27D4EB2F165667C5ULL

static inline uint64_t xxh_round(uint64_t acc, uint64_t input) {
  acc += input * XXH_P2;
  acc = sip_rotl(acc, 31);
  return acc * XXH_P1;
}

static inline uint64_t xxh_merge_round(uint64_t h, uint64_t v) {
  h ^= xxh_round(0, v);
  return h * XXH_P1 + XXH_P4;
}

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

/* Reduces a hash to an index into a power-of-two table by taking the HIGH
 * bits of the hash. Every reduction in this file goes through here, so the
 * choice is made in one place only. Every hash in this file mixes its top
 * bits best: the multiplies of hash_word_fast and hash_word_seeded carry each
 * bit upward, and SipHash and XXH64 mix every bit. A custom hash only gets
 * here through hash_custom_fast or hash_word_seeded.
 *
 * capacity is always a power of two and never below
 * compact_bucket_array_size, which is at least 2, so the shift is at most
 * one less than the type's width and can never be a full-width shift, which
 * would be undefined.
 *
 * Read that precondition before adding a new reduction site: a capacity of 1
 * makes the shift as wide as the type, and a capacity of 0 hands zero to
 * __builtin_clz, and both are undefined. */
static inline size_t hash_index_for(size_t hash_val, size_t capacity) {
  /* The shift is the leading-zero count plus one. Keeping a log2(capacity)
     as an intermediate would mean writing the type's width twice, once to
     derive the width and once to subtract it, and both would then need to
     agree with each other AND with the builtin's operand width. Using the
     count itself drops both restatements, so only the builtin has to match
     size_t. A mistake here makes an unsigned intermediate underflow into a
     shift wider than the type; such a shift is undefined and compilers fold
     it to a constant zero, so every key goes into slot 0 while every answer
     stays correct. */
#if SIZE_MAX == UINT64_MAX
  return hash_val >>
         ((unsigned)__builtin_clzll((unsigned long long)capacity) + 1u);
#else
  return hash_val >> ((unsigned)__builtin_clz((unsigned int)capacity) + 1u);
#endif
}

/* Hashes a long double key by its numeric VALUE, not by its raw bytes. A
 * float is exactly 4 bytes and a double exactly 8, and neither has padding,
 * but a long double's in-memory representation is different on most
 * platforms: 80-bit x87 extended precision kept in a 16-byte slot, for
 * example, has padding bits the C standard leaves completely unspecified.
 * Two variables holding exactly the same mathematical value can differ in
 * those bits depending on how the program computed and stored each one, and
 * the padding of an automatic long double isn't reliably the same across
 * two separate constructions of the same value. That stays true even after
 * an explicit memset, once the compiler decides the whole-object assignment
 * makes that memset dead. So a raw byte hash would put two long double keys
 * with identical numbers into different buckets, and quietly defeat every
 * lookup that doesn't reuse the exact original bytes. frexpl gets around
 * this by taking the VALUE apart: it works on the loaded floating-point
 * value and never on the bytes underneath, so the mantissa and exponent it
 * gives are a deterministic function of the number itself, whatever padding
 * bits came with it.
 *
 * The pair {scaled mantissa, exponent} is hashed as 16 bytes with
 * SipHash-1-3. Both parts go through the keyed function, so no pair of long
 * double values collides for every key. The scaled mantissa of a finite
 * nonzero value is never 0, and each of the three special classes below is
 * the pair {0, its own class number}, so a special class never shares its
 * pair with a number.
 *
 * Three values get their own handling and never reach frexpl:
 * - NaN: frexpl and relational comparisons mean nothing useful for NaN.
 *   A float's and a double's full representation is always meaningful, so a
 *   raw-byte comparison of a NaN payload is well defined for them, but the
 *   padding bytes next to a NaN long double are another story: in practice
 *   they are often really uninitialized memory, not just unspecified but
 *   stable content. A plain `long double n = NAN;` writes only the
 *   significant NaN bits and never the padding, so reading those padding
 *   bytes is a real use of uninitialized memory, even if the read only
 *   hashes them or hands them to memcmp, and valgrind and MemorySanitizer
 *   both report it. So every NaN long double hashes as one fixed class,
 *   which touches no padding byte at all and necessarily collapses every NaN
 *   long double into a single key; see the matching NaN branch of
 *   long_double_keys_equal below. A float and a double keep their own
 *   distinct-NaN-payload policy. We accept that narrower difference on
 *   purpose, because the long double's padding leaves no safe alternative,
 *   and cbstmap does the same: its long double comparator collapses every
 *   NaN into one equivalence class for the same reason, since a BST's
 *   total-order requirement leaves it no other sound choice, and it never
 *   reads a padding byte to do it either.
 * - +-0.0: these are merged into one class up front instead of trusting
 *   frexpl to do it, because the C standard only promises that frexp returns
 *   "zero" for a zero input and doesn't guarantee that every libm keeps the
 *   sign of that zero the same way.
 * - +-Infinity: frexpl's exponent output is unspecified for an infinite
 *   input, and converting an infinite floating value to an integer type is
 *   undefined behavior, so each sign of infinity is its own class, with no
 *   call to frexpl and no cast of the value to an integer. */
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
    // |mantissa| is in [0.5, 1), so the scaled value below always fits in an
    // int64_t, and its magnitude is in [2^61, 2^62), so it's never 0. The
    // scale puts the significant bits at the TOP of the 64-bit value, and all
    // 64 bits get hashed.
    int64_t scaled_mantissa = (int64_t)(mantissa * 4611686018427387904.0L);
    parts[0] = (uint64_t)scaled_mantissa;
    parts[1] = (uint64_t)(int64_t)exp;
  }
  return hash_bytes_keyed(sip_v, parts, sizeof(parts));
}

/* The process secret; see chmap_hash_secret. It gets picked once, on the
 * first call of chmap_process_hash_secret, which every map creation and
 * every ccol_chmap_hash_key call makes before hashing anything, and never in
 * a constructor, so a map that another object's constructor creates before
 * any of this library's constructors runs still hashes with the final
 * secret, in a shared build and in a static one. Once picked the value never
 * changes, so every map and ccol_chmap_hash_key agree on it.
 *
 * Map operations read this object directly and keep no copy. Its address is
 * a constant of the library, so a read costs what a read from the map struct
 * costs. A thread using a map it didn't create reads the picked value too,
 * because creating the map called chmap_process_hash_secret first, and
 * whatever handed the map to that thread orders the creation before the
 * use. */
static chmap_hash_secret g_chmap_hash_secret;
static _ccol_once_flag_t g_chmap_hash_secret_once = _CCOL_ONCE_INIT;

/* One step of the splitmix64 finalizer, which spreads every input bit over
 * every output bit and so can mix entropy sources of uneven quality. */
static uint64_t chmap_seed_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

/* The key words for when the kernel can't give random bytes without
 * blocking: a kernel without getrandom(2), a seccomp policy that refuses it,
 * or an entropy pool that isn't initialized yet early in boot. It mixes both
 * clocks, the process id and three addresses that address space layout
 * randomization places, and derives the four words from that mix. The
 * result is weaker than the kernel generator but far stronger than a
 * constant, and this function never fails. */
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

/* Fills the SipHash state of secret from the 128-bit key (k0, k1). */
static void chmap_sip_state_from_key(uint64_t k0, uint64_t k1,
                                     uint64_t sip_v[4]) {
  sip_v[0] = k0 ^ 0x736F6D6570736575ULL;
  sip_v[1] = k1 ^ 0x646F72616E646F6DULL;
  sip_v[2] = k0 ^ 0x6C7967656E657261ULL;
  sip_v[3] = k1 ^ 0x7465646279746573ULL;
}

static void chmap_init_process_hash_secret(void) {
  // k0 and k1 of SipHash, then int_seed, then byte_seed.
  uint64_t key[4] = {0, 0, 0, 0};
  if (!ccol_random_bytes(key, sizeof(key))) {
    chmap_fallback_hash_key(key);
  }
  chmap_sip_state_from_key(key[0], key[1], g_chmap_hash_secret.sip_v);
  g_chmap_hash_secret.int_seed = key[2];
  g_chmap_hash_secret.byte_seed = key[3];
}

/* Gives the process secret, picking it first if no call has picked it yet.
 * It's called on a map's creation path and from ccol_chmap_hash_key, never
 * on the path of a map's insert or lookup. */
static const chmap_hash_secret* chmap_process_hash_secret(void) {
  _ccol_call_once(g_chmap_hash_secret_once, chmap_init_process_hash_secret);
  return &g_chmap_hash_secret;
}

/* The fast mode's byte hash, XXH64 seeded with byte_seed, as a size_t; a
 * 32-bit size_t keeps both halves of the output. */
static inline size_t hash_bytes_fast(const void* input, size_t len) {
  uint64_t h = xxh64(input, len, g_chmap_hash_secret.byte_seed);
#if SIZE_MAX == UINT64_MAX
  return (size_t)h;
#else
  return (size_t)(h ^ (h >> 32));
#endif
}

/* The keyed mode's byte hash. It's out of line so the fast byte hash stays
 * inline in the functions that hash both ways. */
static __attribute__((noinline)) size_t hash_bytes_keyed_ool(const void* input,
                                                             size_t len) {
  return hash_bytes_keyed(g_chmap_hash_secret.sip_v, input, len);
}

/* The hash of a fixed-width key no wider than size_t, in the mode keyed
 * names. */
static inline __attribute__((always_inline)) size_t
hash_word_mode(size_t x, bool keyed, uint64_t int_seed) {
  return keyed ? hash_word_seeded(x, int_seed) : hash_word_fast(x);
}

/* The hash of a key of exactly 64 bits, in the mode keyed names. */
static inline __attribute__((always_inline)) size_t
hash_u64_mode(uint64_t x, bool keyed, uint64_t int_seed) {
  return keyed ? hash_u64_seeded(x, int_seed) : hash_u64_fast(x);
}

/* Sends the key to the right hash function for its key type and for the
 * mode keyed names; see the adaptive hashing section. Every integral type,
 * float, double and a pointer go through hash_word_mode or hash_u64_mode. A
 * long double goes through hash_long_double_value in both modes, and every
 * other type, a string or a struct included, through hash_bytes_fast or
 * hash_bytes_keyed_ool. A custom hashing proc's result goes through
 * hash_custom_fast or hash_word_seeded.
 *
 * Only the separate-chaining backend gets to the long double and byte-hash
 * cases, because the open-addressing backend only holds the fixed-width
 * types of the first group.
 *
 * A custom proc hashes with its own function, which the map then finalizes,
 * so a custom hash whose good bits are at the low end, like an identity or a
 * counter, doesn't put every key in one slot. Keys whose custom hashes are
 * equal collide in both modes regardless, since no finalizer can tell them
 * apart.
 *
 * int_seed is the secret's int_seed. The callers pass it so that a loop
 * writing through a byte pointer, which may alias the secret, keeps it in a
 * register. */
static inline __attribute__((always_inline)) size_t
hash_key_data(const void* key_ptr, size_t key_size, ccol_data_type key_type,
              ccol_hashing_proc_t custom_proc, bool keyed, uint64_t int_seed) {
  if (custom_proc) {
    size_t h = (size_t)custom_proc(key_ptr, key_size);
    return keyed ? hash_word_seeded(h, int_seed) : hash_custom_fast(h);
  }

  // The sizes here are compile-time constants, so the compiler turns each
  // copy into a single unaligned load. Every multi-byte read below goes
  // through memcpy rather than dereferencing a cast pointer, because key_ptr
  // can come straight from a cmap_pair the caller built (through the raw
  // function layer of chmap_insert_elem, chmap_get_elem_ref and
  // chmap_delete_elem), and such a pointer carries no alignment guarantee,
  // while the address of a type-inferred macro's local variable does. A
  // direct `*(uint32_t*)key_ptr` style read from a misaligned pointer is
  // undefined behavior and can fault on a strict-alignment architecture. The
  // _Alignas(max_align_t) comment on llist_node in this file covers the same
  // class of hazard elsewhere in this module.
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

/* True when the byte ranges [a, a+a_size) and [b, b+b_size) overlap. Both
 * backends use it to catch a caller's value pointer aliasing the very
 * storage an insert or update is about to change: oa_insert's existing-key
 * update path, further down in this section, and sc_reset_val_of_llist_node
 * in the separate-chaining section. */
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

#ifdef RUNNING_UNIT_TESTS
/* A white-box regression guard for hash quality. It counts the slots
 * open-addressing probes look at, which is the real cost of a hash that
 * doesn't spread the keys. The map stays correct even with very bad
 * clusters, so a correctness test can't see the difference, and a timing
 * test would measure the machine instead. A key set whose low bits are
 * constant, like aligned addresses or identifiers scaled by a power of two,
 * drives this count quadratic when the index reads bits the hash didn't mix,
 * and leaves it near one probe per operation when the index reads mixed
 * bits. */
/* Atomic, with relaxed ordering, because it sits in the probe loop of a map
 * that many threads can read at once, where a plain increment would be a
 * data race. ThreadSanitizer would report that race against every caller
 * that shares a map, and the instrumentation would then produce findings
 * instead of guarding against them. A count needs nothing stronger than
 * relaxed ordering, and the accessor only reads the counter after the test
 * has joined the threads under test. */
static _Atomic unsigned long long g_oa_probe_steps_for_tests = 0;
unsigned long long chashmap_oa_probe_steps_for_tests(void) {
  return atomic_load_explicit(&g_oa_probe_steps_for_tests,
                              memory_order_relaxed);
}
void chashmap_reset_oa_probe_steps_for_tests(void) {
  atomic_store_explicit(&g_oa_probe_steps_for_tests, 0, memory_order_relaxed);
}
#define OA_COUNT_PROBE()                                        \
  (atomic_fetch_add_explicit(&g_oa_probe_steps_for_tests, 1ull, \
                             memory_order_relaxed))
/* The number of successful open-addressing rehashes, across the process. A
 * rehash at an unchanged capacity counts too, which the capacity alone
 * can't show. Relaxed and atomic for the same reason as the probe counter
 * above. */
static _Atomic unsigned long long g_oa_rehashes_for_tests = 0;
unsigned long long chashmap_oa_rehashes_for_tests(void) {
  return atomic_load_explicit(&g_oa_rehashes_for_tests, memory_order_relaxed);
}
#define OA_COUNT_REHASH()                                    \
  (atomic_fetch_add_explicit(&g_oa_rehashes_for_tests, 1ull, \
                             memory_order_relaxed))
/* How many entries backward-shift deletion moved, across the process. */
static _Atomic unsigned long long g_oa_shift_moves_for_tests = 0;
unsigned long long chashmap_oa_shift_moves_for_tests(void) {
  return atomic_load_explicit(&g_oa_shift_moves_for_tests,
                              memory_order_relaxed);
}
#define OA_COUNT_SHIFT_MOVE()                                   \
  (atomic_fetch_add_explicit(&g_oa_shift_moves_for_tests, 1ull, \
                             memory_order_relaxed))
/* How many occupied slots the backward-shift walk looked at, across the
 * process. */
static _Atomic unsigned long long g_oa_shift_steps_for_tests = 0;
unsigned long long chashmap_oa_shift_steps_for_tests(void) {
  return atomic_load_explicit(&g_oa_shift_steps_for_tests,
                              memory_order_relaxed);
}
#define OA_COUNT_SHIFT_STEP()                                   \
  (atomic_fetch_add_explicit(&g_oa_shift_steps_for_tests, 1ull, \
                             memory_order_relaxed))
/* How many chain nodes the separate-chaining backend's searches visited,
 * across the process: the probe counter's counterpart for that backend. */
static _Atomic unsigned long long g_sc_node_visits_for_tests = 0;
unsigned long long chashmap_sc_node_visits_for_tests(void) {
  return atomic_load_explicit(&g_sc_node_visits_for_tests,
                              memory_order_relaxed);
}
void chashmap_reset_sc_node_visits_for_tests(void) {
  atomic_store_explicit(&g_sc_node_visits_for_tests, 0, memory_order_relaxed);
}
#define SC_COUNT_NODE_VISIT()                                   \
  (atomic_fetch_add_explicit(&g_sc_node_visits_for_tests, 1ull, \
                             memory_order_relaxed))
/* The fast attempts that growing a keyed map makes, across the process: how
 * many started, how many failed, and the work, count and capacity of the
 * last one that failed. An open-addressing attempt's work is the slots its
 * fill looked at, and a separate-chaining attempt's work is the nodes it
 * hashed plus the chain nodes it walked. Relaxed and atomic for the same
 * reason as the probe counter above. */
static _Atomic unsigned long long g_retries_for_tests = 0;
static _Atomic unsigned long long g_retry_failures_for_tests = 0;
static _Atomic unsigned long long g_retry_failed_work_for_tests = 0;
static _Atomic size_t g_retry_last_work_for_tests = 0;
static _Atomic size_t g_retry_last_count_for_tests = 0;
static _Atomic size_t g_retry_last_capacity_for_tests = 0;
unsigned long long chashmap_retries_for_tests(void) {
  return atomic_load_explicit(&g_retries_for_tests, memory_order_relaxed);
}
unsigned long long chashmap_retry_failures_for_tests(void) {
  return atomic_load_explicit(&g_retry_failures_for_tests,
                              memory_order_relaxed);
}
unsigned long long chashmap_retry_failed_work_for_tests(void) {
  return atomic_load_explicit(&g_retry_failed_work_for_tests,
                              memory_order_relaxed);
}
void chashmap_last_failed_retry_for_tests(size_t* work, size_t* count,
                                          size_t* capacity) {
  *work =
      atomic_load_explicit(&g_retry_last_work_for_tests, memory_order_relaxed);
  *count =
      atomic_load_explicit(&g_retry_last_count_for_tests, memory_order_relaxed);
  *capacity = atomic_load_explicit(&g_retry_last_capacity_for_tests,
                                   memory_order_relaxed);
}
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
#endif /* RUNNING_UNIT_TESTS */

/* Widens the caller's key to the same 64-bit form a slot stores, so a probe
 * can compare two integers instead of calling memcmp at each step. Every
 * write of key_data zeroes all 8 bytes before copying key_size bytes in (see
 * oa_insert), so a key widened the same way here compares equal exactly when
 * the bytes are equal. That holds for any byte order, because both sides put
 * the key's bytes at the start of the object and zero the rest.
 *
 * The switch gives each copy a compile-time constant size, which the
 * compiler turns into a single unaligned load, whereas a memcpy whose size
 * is only known at run time is a call.
 *
 * The listed cases cover every width an integral key can have, and every
 * entry point checks the caller's key against its declared type's width
 * before the key gets here, so no current path reaches the default case.
 * Still, the default case clamps the size rather than copying key_size
 * bytes. The destination is one 8-byte object, so a size that did arrive
 * unchecked would be a stack buffer overflow, whereas a comparison that only
 * read the bytes would be no worse than an over-READ. A key wider than the
 * slot can't compare equal to a stored key anyway, so the clamp loses no
 * answer the wider copy would have given. */
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

/* Compares a slot's stored key with a key oa_widen_key has widened already.
 * Keep that oa_widen_key call out of the probe loop: the caller's key
 * doesn't change while the probe walks, and repeating that work at every
 * step is exactly the cost this arrangement saves. */
static inline bool oa_keys_equal(const oa_slot* slot, uint64_t probe_key) {
  return slot->key_data == probe_key;
}

/* Fills out with the {ptr, size} pair of the value in the slot at index: the
 * address of its value bytes and the map's value size. */
static inline void oa_fill_val_pair(const open_addr_map* map, size_t index,
                                    cmap_pair* out) {
  out->ptr = &map->slots[index].val_data;
  out->size = map->val_size;
}

/* The bytes of a table block of capacity slots: the slots, then one
 * metadata byte per slot. The table keeps nothing else per slot. The
 * {ptr, size} pair a lookup reports for a value is always
 * {&slots[index].val_data, val_size}, so the lookup computes it into the
 * caller's storage instead of reading it from the table, and every growth
 * faults in and zeroes only these 17 bytes per slot. */
#define OA_BLOCK_BYTES_PER_SLOT (sizeof(oa_slot) + 1)

/* Allocates one block holding capacity slots and then capacity metadata
 * bytes, and reports where the metadata starts. A metadata byte needs no
 * alignment beyond one byte. The block is zeroed, so every slot starts
 * without SLOT_OCCUPIED, which is the empty sentinel.
 *
 * We form the byte count ourselves instead of handing calloc a count and a
 * size, so we make the overflow check that calloc would have made.
 * Without it a product that wraps would turn into a small allocation that
 * succeeds, and a probe loop would then walk capacity entries through it. */
static oa_slot* oa_alloc_block(ccol_memmgmt_procs_t* m_procs, size_t capacity,
                               uint8_t** metadata_out) {
  if (capacity == 0 || capacity > SIZE_MAX / OA_BLOCK_BYTES_PER_SLOT) {
    return NULL;
  }
  oa_slot* slots = (oa_slot*)_ccol_mem_calloc(
      m_procs, 1, capacity * OA_BLOCK_BYTES_PER_SLOT);
  if (!slots) return NULL;
  *metadata_out = (uint8_t*)(slots + capacity);
  return slots;
}

/* The smallest probe_span of a table of capacity slots: OA_DEEP_INSERT, or
 * the whole table when it's smaller than that. */
static inline size_t oa_min_probe_span(size_t capacity) {
  return capacity < OA_DEEP_INSERT ? capacity : OA_DEEP_INSERT;
}

/* Initializes the open-addressing state in a map's block and allocates its
 * table. calloc zeroes every slot, so each slot's metadata byte starts as 0,
 * without SLOT_OCCUPIED, which is the empty sentinel. Gives false, with
 * nothing allocated, when the table can't be allocated. */
static bool oa_init(open_addr_map* map, size_t capacity,
                    ccol_data_type key_type, ccol_data_type val_type,
                    size_t key_size, size_t val_size,
                    ccol_memmgmt_procs_t* m_procs,
                    ccol_hashing_proc_t custom_hashing_proc) {
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

/* The cap on the displacement of a single insert into a fast table of
 * capacity slots: 24 slots per doubling of the table up to 4096 slots, and
 * 288 from there on. An insert whose displacement is above it switches the
 * map.
 *
 * The cap must sit above the displacements a random function produces,
 * otherwise ordinary keys would switch the map. For a large table the tail
 * of an insert's displacement at a given load is independent of the table's
 * size: at the highest load, 0.70, a random function displaces about one
 * insert in 5e5 by more than 160 slots, one in 2e8 by more than 256 and one
 * in 1.4e9 by more than 288. A small table can't hold a long cluster, which
 * pulls that tail well below these figures, so the cap there is lower in
 * proportion. A smaller cap, say 8 slots per doubling (about the largest
 * displacement a random function produces in a fill), would switch an
 * ordinary map churning at its highest load once every few thousand
 * inserts, which is far too often for a map that nobody attacks. */
#define OA_CAP_PER_DOUBLING 24u
#define OA_CAP_LOG2_CEILING 12u
static inline size_t oa_insert_cap(size_t capacity) {
  unsigned k = chmap_log2_pow2(capacity);
  return (size_t)OA_CAP_PER_DOUBLING *
         (k < OA_CAP_LOG2_CEILING ? k : OA_CAP_LOG2_CEILING);
}

/* The largest count a table of capacity slots holds after an insert that
 * didn't grow it: an insert grows the table when the count before it is
 * above 0.70 of the capacity; see oa_above_grow_load. */
static inline size_t oa_max_count(size_t capacity) {
  return (capacity / 10) * 7 + ((capacity % 10) * 7) / 10 + 1;
}

/* The bound on the sum of a window's 256 insert displacements, above which a
 * fast table switches.
 *
 * E(a) = (1 / (1 - a)^2 - 1) / 2 is the mean displacement of an insert into
 * a table of load a under a random function. The bound is
 * 256 * (5 * E(a) + 0.5) for a table of 4096 slots or more, and
 * 256 * (6 * E(a) + 0.5) for a smaller one, whose clusters vary more from
 * one window to the next. Simulated with a random function, the largest
 * window sum stays under 0.72 of this bound in tables of 16 to 2^20 slots,
 * both in fills and in churn at the highest load.
 *
 * a is the highest load the window can have reached: the count when the
 * window started plus its 256 inserts, and never more than the table holds
 * before it grows. A window can contain deletes, so the load at its end can
 * be far below the load most of its inserts ran at; judged at its end load,
 * a window ending just after a bulk delete would switch a map holding
 * ordinary keys.
 *
 * The arithmetic is in integers: with d = 1 - a in 16.16 fixed point,
 * 256 * 5 * E(a) = 640 * (2^32 - d^2) / d^2, and 768 instead of 640 for the
 * smaller tables. Truncating a to 16 fractional bits and the integer
 * division keep the bound within two counts, and within one percent, of the
 * real-valued bound. */
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

/* Starts a new insert window and computes its bound from the current count.
 * A keyed table is never judged, so it skips the division. */
static inline void oa_window_restart(open_addr_map* map) {
  struct chashmap* chm = chmap_of_oa(map);
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound = chm->hash_mode == CHMAP_HASH_KEYED
                          ? UINT16_MAX
                          : oa_window_bound(map->count, map->capacity);
}

/* Places every live entry of the old table into the zeroed table new_slots
 * heads, with the hash of the mode keyed names, and gives the largest
 * displacement it made. The table stores no hash, so each one is computed
 * again. new_capacity is always a power of two, so a probe advances with
 * & (new_capacity - 1) rather than the far more expensive % new_capacity. */
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

    // Prefetch the likely next location
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

/* The slack in a fast attempt's displacement budget; see
 * oa_place_all_fast_attempt. */
#define OA_ATTEMPT_SLACK 64u

/* The fast attempt when a keyed table grows: places every live entry of the
 * old table into the zeroed new table with the fast hash, like oa_place_all,
 * and gives the largest displacement it made, or SIZE_MAX as soon as the
 * fill shows the fast hash is bad for these keys. It's bad when an entry
 * would sit more than cap = oa_insert_cap slots from its home slot, or when
 * the displacements placed so far add up to more than the budget
 * map->count + OA_ATTEMPT_SLACK. A random function fills a table to a
 * growth's load, at most one half, with a sum of about a third of the count,
 * so neither limit refuses an ordinary key set.
 *
 * The abort is what bounds a failed attempt. Each placed entry looks at its
 * displacement plus one slot, the sum before the last placement is at most
 * the budget, and one entry's walk stops after cap + 1 slots, so a failed
 * attempt looks at no more than count + budget + cap + 1, i.e.
 * 2 * count + cap + 65 slots, before the caller fills the same block again
 * with the keyed hash. Without the abort, keys an attacker made collide under
 * the fast hash would cost the fill a number of probes quadratic in the
 * count. */
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

/* Rebuilds the table at new_capacity with the hash of the mode keyed names,
 * and restarts the insert window. A fast rebuild whose largest displacement
 * is above the new table's cap, which a shrink that merges clusters can
 * produce, rebuilds the same new block again with the keyed hash, so the
 * table a fast map ends up with never holds a displacement above its cap;
 * see oa_insert_cap.
 *
 * Asking for the fast hash on a keyed table is a growth's fast attempt: the
 * fill stops as soon as it shows the fast hash is bad for these keys (see
 * oa_place_all_fast_attempt), and the same new block then gets filled with
 * the keyed hash, so the attempt allocates nothing more. A table the attempt
 * fills keeps the fast hash, and the insert window restarted below judges it
 * like any fast table.
 *
 * Gives ccol_not_enough_memory when the allocation fails and leaves the map
 * completely unchanged, so a caller whose own operation then fails only
 * because this opportunistic rehash couldn't happen can say so honestly
 * instead of naming some other condition; see how oa_insert_impl uses this
 * return value. */
static ccol_retval_t oa_rebuild(open_addr_map* map, size_t new_capacity,
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

  _ccol_mem_free(map->m_procs, map->slots);
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

/* The capacity the insert path rehashes to for a table about to hold
 * live_after_insert entries: the smallest power of two at or above twice that
 * count, so the live load after the rehash is at most one half, and never
 * less than minimum_allowed_bucket_array_size.
 *
 * The insert path only rehashes when the live entries fill more than
 * OPEN_ADDR_MAX_LOAD_FACTOR of the table, so this is always twice the
 * capacity. Sizing from the live count keeps that true by construction, and
 * keeps the result above OPEN_ADDR_MIN_LOAD_FACTOR, one quarter, because half
 * of the SMALLEST power of two at or above twice the live count is below
 * twice that count. So a delete can't shrink it straight back, and a shrink
 * leaves a live load below one half, which takes a fifth of the capacity in
 * inserts before the next grow, so growth and shrink can't alternate. */
static size_t oa_rehash_capacity_for(size_t live_after_insert) {
  if (live_after_insert > _ccol_max_power_of_two_size_t / 2) {
    return _ccol_max_power_of_two_size_t;
  }
  size_t wanted = _ccol_find_nearest_gte_power_of_two(2 * live_after_insert);
  return wanted < minimum_allowed_bucket_array_size
             ? minimum_allowed_bucket_array_size
             : wanted;
}

/* Answers count / capacity > OPEN_ADDR_MAX_LOAD_FACTOR (0.70) in integers,
 * as count * 10 > capacity * 7. Neither product overflows while capacity is
 * at most SIZE_MAX / 10, since count never exceeds capacity; a larger
 * capacity takes the division form, which is exact for any capacity. The
 * common form is two multiplications by constants and a compare, with no
 * floating point and no division. */
static inline bool oa_above_grow_load(size_t count, size_t capacity) {
  if (__builtin_expect(capacity <= SIZE_MAX / 10, 1)) {
    return count * 10 > capacity * 7;
  }
  return count > (capacity / 10) * 7 + ((capacity % 10) * 7) / 10;
}

/* Probes, in probe order from hash_val's home slot, for the first empty slot,
 * and gives map->capacity when the table has none. It finds an insertion
 * point against the map's *current* capacity and mask, and the caller must
 * already know the key isn't anywhere in the table. That's exactly what
 * oa_insert needs after a rehash, which leaves an earlier probe's index and
 * capacity stale. */
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

/* Gives the slot of the stored key whose widened bytes are key. The key must
 * be present, within probe_span slots of its home slot. It's hashed from
 * a buffer as wide as any key type, because the compiler can't see that an
 * open-addressing map never has a long double key. */
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
  _ccol_assert(false);
  return index;
}

/* Switches a fast table to the keyed hash at the same capacity, and gives
 * the new slot of the key slot index held. The rebuild allocates a new
 * block; when that fails the table stays fast and correct, the key stays
 * where it is, and the next window end or deep insert tries again. */
static size_t oa_switch_keeping(open_addr_map* map, size_t index) {
  uint64_t key = map->slots[index].key_data;
  if (oa_rebuild(map, map->capacity, true) != ccol_success) {
    return index;
  }
  return oa_find_stored_key(map, key);
}

/* The cold path of an insert whose displacement is at least
 * OA_DEEP_INSERT. It widens probe_span to cover the new key, and switches a
 * fast table whose new key sits further from its home slot than the cap
 * allows. Gives the new key's slot, which the switch moves. */
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

/* The cold path of the insert that ends a window, which switches a fast
 * table whose window sum is above its bound. Gives the new key's slot,
 * which the switch moves. */
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

/* The cold path of inserting a new key into a table above its growth load,
 * which finishes the insert. index is the empty slot the probe of the
 * current table found for the key, and found_empty says whether it found
 * one. It grows the table, writes the new entry into its empty slot in the
 * grown table, and reports the entry's displacement to the adaptive hash's
 * detection, like oa_insert_impl does. A growth asks for the fast hash in
 * either mode, because a fast table may switch and a keyed table makes its
 * fast attempt; see oa_rebuild. When the allocation fails the insert goes
 * into the current table's slot, and without such a slot the result is
 * ccol_not_enough_memory; a grown table with no empty slot gives
 * ccol_container_full.
 *
 * The bytes key_pair and val_pair point at can be inside the block the
 * rebuild frees: the ptr chmap_get_elem_ref reports for another key points
 * at the value bytes of a slot in that block. So both sizes and both sets of
 * bytes (at most 8 each in this backend) are copied before the rebuild, and
 * only those copies are read after it. */
static __attribute__((noinline)) ccol_retval_t oa_insert_with_growth(
    open_addr_map* map, const cmap_pair* key_pair, const cmap_pair* val_pair,
    cmap_pair* slot_out, size_t index, size_t home, bool found_empty) {
  /* The key copy is as wide as any key type, because the compiler can't
   * see that an open-addressing map never has a long double key. */
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

/* Inserts or updates a key-value pair with a linear probe. The table holds
 * no tombstone (see oa_delete), so the probe's first empty slot both proves
 * the key is absent and is where it goes.
 *
 * The probe for an existing entry comes FIRST, with no thought of growth at
 * all. A pure value update for a key already present never changes
 * elem_count, so it can never start a rehash, and it can never invalidate
 * the chmap_get_elem_ref pointer of ANY OTHER key; in the other order it
 * could, just because earlier unrelated inserts left the map above its
 * growth threshold. The load factor count / capacity is only looked at once
 * the key is confirmed absent. With no tombstones count is the whole load,
 * and a load above OPEN_ADDR_MAX_LOAD_FACTOR then starts a rehash to the
 * capacity oa_rehash_capacity_for() picks from the live count. The rehash
 * makes the first probe's index and capacity invalid, so a fresh probe with
 * oa_find_insertion_slot then finds the insertion point in the rebuilt
 * table. The key's absence needs no second check, since a rehash only moves
 * entries that are live already and can't bring this key in. The common
 * case, a plain update or a plain insert of a new key that doesn't cross the
 * growth threshold, costs exactly one probe pass, the same as a design that
 * checks growth up front on every call. So the order is free, and it's worth
 * keeping if you change this function.
 *
 * An insert of a new key then reports its displacement to the adaptive
 * hash's detection (see the adaptive hashing section): a displacement of
 * OA_DEEP_INSERT or more goes to oa_note_deep_insert, and every insert adds
 * to the window, which costs a lea, an add and a jump on the carry. Either
 * one can switch the table, which moves the new key, so both give back its
 * slot.
 *
 * The function can need a growth it can't get: oa_rebuild() can fail on an
 * allocation, or the growth is skipped because the table is at its
 * architectural maximum capacity already. The insert then runs against the
 * table's current size, and if that finds no room at all the two cases get
 * distinct, honest return codes instead of both collapsing into
 * ccol_container_full: ccol_not_enough_memory from oa_insert_with_growth,
 * and ccol_container_full at the end of this function.
 *
 * The caller, chmap_insert_elem, already rejects any key_pair or val_pair
 * whose size isn't exactly map->key_size or map->val_size, so both memcpy
 * calls below stay inside the 8-byte key_data and val_data fields they
 * target, and nothing here can spill into a neighbouring slot. */
/* slot_out decides what happens for a key that is already present. NULL
 * overwrites its value, which is chmap_insert_elem(). Non-NULL leaves such an
 * entry alone and, once the call succeeds, fills *slot_out with the
 * {ptr, size} pair of the value of the entry the key names, whether it was
 * present or just inserted, which is ccol_chmap_insert_or_get_elem(). Every
 * caller passes a constant, so each instantiation keeps only its own arm. */
static inline __attribute__((always_inline)) ccol_retval_t
oa_insert_impl(open_addr_map* map, const cmap_pair* key_pair,
               const cmap_pair* val_pair, cmap_pair* slot_out) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t start_index = index;
  bool found_empty = false;
  const uint64_t probe_key = oa_widen_key(key_pair->ptr, key_pair->size);

  // Prefetch the first location
  __builtin_prefetch(&map->slots[index], 1, 1);

  do {
    OA_COUNT_PROBE();
    // Prefetch the likely next location
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
      // val_pair->ptr can alias this slot's val_data, for example when a
      // caller inserts a value again through the raw chmap_insert_elem
      // layer, from a pointer chmap_get_elem_ref or chmap_get_ptr gave for
      // this exact key. memcpy needs its source and destination to never
      // overlap, so when they do overlap the value is copied into a small
      // stack buffer first; val_pair->size is always 8 bytes or less for
      // this backend. sc_reset_val_of_llist_node has the same aliasing
      // protection for the separate-chaining backend.
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

  // The key really doesn't exist. sc_insert has its ccol_max_elem_count
  // check at the same point (its own comment says why the lookup for an
  // existing key must come first), and the check is explicit here for the
  // same reason, instead of just falling out of the physical capacity cap at
  // _ccol_max_power_of_two_size_t (== ccol_max_elem_count). Without the
  // explicit check, a full table already at that architectural limit gets
  // to the same ccol_container_full outcome some lines further down, but
  // only after computing a load factor and confirming that growth is
  // impossible, both of which are pointless steps.
  if (map->count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  // Only now, when this call really is about to add a new entry, is an
  // opportunistic rehash safe to consider. The capacity changes under the
  // probe above, and the index mask with it, so the growth probes the
  // rebuilt table again, and it writes the new entry itself because key_pair
  // and val_pair may point into the block it frees.
  if (oa_above_grow_load(map->count, map->capacity) &&
      map->capacity < _ccol_max_power_of_two_size_t) {
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

  // No room anywhere in the table, which is a real architectural capacity
  // limit: the table is at _ccol_max_power_of_two_size_t, so growing it was
  // never tried. A growth stopped by an allocation failure reports
  // ccol_not_enough_memory instead; see oa_insert_with_growth.
  return ccol_container_full;
}

static ccol_retval_t oa_insert(open_addr_map* map, const cmap_pair* key_pair,
                               const cmap_pair* val_pair) {
  return oa_insert_impl(map, key_pair, val_pair, NULL);
}

static ccol_retval_t oa_insert_or_get(open_addr_map* map,
                                      const cmap_pair* key_pair,
                                      const cmap_pair* val_pair,
                                      cmap_pair* slot_out) {
  return oa_insert_impl(map, key_pair, val_pair, slot_out);
}

/* Looks up key_pair with a linear probe. An empty slot ends the search
 * immediately, which is safe because neither an insert nor a delete ever
 * leaves a gap between a key and its home slot (see oa_delete), and the
 * search also ends after probe_span slots, since no key sits further from
 * its home slot. On a hit the caller's pair gets the address of the slot's
 * value bytes and the map's value size, so the lookup touches no line other
 * than the slot and its metadata byte. Each result points at its own slot,
 * so results the caller holds at the same time for different keys never
 * alias each other, and each stays valid until something changes the map
 * (an insert, a delete or a resize). On a miss the pair becomes {NULL, 0}. */
static ccol_retval_t oa_get(open_addr_map* map, const cmap_pair* key_pair,
                            cmap_pair* val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t left = map->probe_span;
  const uint64_t probe_key = oa_widen_key(key_pair->ptr, key_pair->size);

  // Prefetch the first location
  __builtin_prefetch(&map->slots[index], 0, 1);

  do {
    OA_COUNT_PROBE();
    // Prefetch the likely next location
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

/* Closes the gap an emptied slot (the hole) leaves in its cluster with
 * backward-shift deletion, so the table never holds a tombstone.
 *
 * Linear probing puts every entry at or after its home slot with no empty
 * slot in between, and emptying a slot breaks that for every later entry of
 * the cluster whose home slot is at or before the hole. The walk visits the
 * rest of the cluster in probe order. An entry at slot j with home slot h may
 * move into the hole exactly when h isn't in the cyclic range (hole, j],
 * i.e. when the distance from h to j is at least the distance from the hole
 * to j; such an entry moves and its old slot becomes the hole. Every other
 * entry stays, since moving it would put it before its home slot. The masked
 * differences make the test work for a cluster that wraps past the last
 * slot.
 *
 * The walk stops at the first empty slot, or once the slot it visits is
 * span slots past the hole: no entry sits span or more slots from its home
 * slot (see probe_span), so no entry from there on may move into the hole.
 * That bounds a delete inside a long run of keys with consecutive home
 * slots, which no entry of the run may leave.
 *
 * The hole itself is always empty, so the walk meets an empty slot within
 * one pass even in a table with no other free slot.
 *
 * A move copies the slot's 16 bytes and sets its metadata byte, and nothing
 * more, since the table keeps no other per-slot state.
 *
 * oa_delete empties the slot and only calls oa_backward_shift_close_gap when
 * the next slot is occupied, and each walk is an out-of-line instance of
 * this body, so a delete whose next slot is empty costs one store and one
 * test. */
static inline __attribute__((always_inline)) void oa_backward_shift_walk(
    oa_slot* const slots, uint8_t* const metadata, const size_t capacity,
    const size_t key_size, const ccol_data_type key_type,
    const ccol_hashing_proc_t custom, const bool keyed, const uint64_t int_seed,
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

/* One walk per key type that open addressing accepts with the built-in
 * hash, in each mode. Each takes only the values it reads, so it needs no
 * stack frame of its own. The size argument is the type's fixed width, which
 * hash_key_data() doesn't read for these types. */
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

/* The walk for a custom hash, or for a key type the list above doesn't
 * name. */
static __attribute__((noinline)) void oa_shift_walk_generic(open_addr_map* map,
                                                            size_t hole) {
  oa_backward_shift_walk(map->slots, map->metadata, map->capacity,
                         map->key_size, map->key_type, map->custom_hashing_proc,
                         oa_keyed(map), g_chmap_hash_secret.int_seed,
                         map->probe_span, hole);
}

/* Picks the walk for map's key type and mode. The walk writes through a
 * byte pointer, which may alias anything, so every walk gets the fields it
 * reads as arguments; otherwise every step would reload them from the map.
 * Each call below is in tail position, so this function adds no frame. */
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

/* Deletes the matching entry with backward-shift deletion; see
 * oa_backward_shift_close_gap. Like oa_get's search, the search stops after
 * probe_span slots. An entry of the same cluster can move one or more slots
 * toward its home slot, so every reference the map handed out becomes
 * invalid, as chmap_get_elem_ref's contract says. The table is halved when
 * the load factor after the delete drops below OPEN_ADDR_MIN_LOAD_FACTOR. */
static ccol_retval_t oa_delete(open_addr_map* map, const cmap_pair* key_pair) {
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

      // count < capacity / 4 is exactly count / capacity <
      // OPEN_ADDR_MIN_LOAD_FACTOR, because capacity is a power of two of at
      // least 16, and the integer form saves a division on every delete.
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

/* Frees the table. The state itself lives in the map's block, which
 * __chmap_destroy frees. */
static void oa_destroy(open_addr_map* map) {
  _ccol_mem_free(map->m_procs, map->slots);
}

/* Zeroes every slot and every slot's metadata byte in place at the map's
 * current capacity, without allocating anything. The open-addressing
 * backend never allocates heap memory for a key or a value, since every key
 * and value lives inline in the slot array, so clearing the metadata, which
 * drops SLOT_OCCUPIED on every slot, is a complete and correct "destroy every
 * element" for this backend, with nothing else to free. oa_reset() uses it
 * so that failing to allocate the new capacity the caller asked for still
 * destroys every existing element, instead of leaving the old table and all
 * its data completely unchanged. */
static void oa_clear_in_place(open_addr_map* map) {
  memset(map->slots, 0, map->capacity * OA_BLOCK_BYTES_PER_SLOT);
  map->count = 0;
  map->probe_span = oa_min_probe_span(map->capacity);
  oa_window_restart(map);
}

/* Clears every entry. When new_capacity differs from the current capacity
 * it also replaces the slot array with a new zeroed array of that size; when
 * new_capacity is 0 or equal to the map's current capacity, the existing
 * slots and metadata arrays are cleared in place and nothing at all is
 * allocated, just as sc_reset() skips the realloc when the size stays the
 * same. The hash mode stays as it is, and the insert window restarts.
 *
 * Every element is destroyed whatever the return value is, which matches
 * chmap_reset's documented contract. When the allocation for new_capacity
 * fails we fall back to oa_clear_in_place() at the map's current, unchanged
 * capacity, rather than returning with the old table and every element in
 * it fully intact. */
static ccol_retval_t oa_reset(open_addr_map* map, size_t new_capacity) {
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

  _ccol_mem_free(map->m_procs, map->slots);
  map->slots = new_slots;
  map->metadata = new_metadata;

  map->capacity = new_capacity;
  map->count = 0;
  map->probe_span = oa_min_probe_span(new_capacity);
  oa_window_restart(map);

  return ccol_success;
}

/* ========================================================================== */
/*              SEPARATE CHAINING IMPLEMENTATION                              */
/* ========================================================================== */

#define dllistRefNodePtr2LlistNodePtr(tracker) \
  (llist_node*)((uint8_t*)tracker - offsetof(llist_node, dllist_refs))

/* Puts node at the front of the doubly-linked list root describes, so the
 * list keeps the element inserted last at the head and the element inserted
 * first at the tail. Walking the next pointers visits the nodes newest
 * first, in reverse insertion order, and walking from the tail through the
 * prev pointers visits them in insertion order. */
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

/* Takes node off the doubly-linked list without freeing it. Updating the
 * neighbours and both ends covers every case: the head node, the tail node,
 * a node in the middle and the only node. */
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

/* Frees an entry's key and value buffers when they are on the heap and not
 * inline, with m_procs (the map's procs), then takes the node off the
 * insertion-order dllist and frees the node struct itself. A NULL all_elems
 * skips the dllist removal; only sc_create_llist_node()'s failure paths pass
 * NULL, because the node they free isn't on the list yet. Every other caller
 * (a delete, a reset and the teardown of a whole map) passes the map's list,
 * so the list never names a freed node.
 *
 * A buffer is on the heap exactly when the size in its accessor exceeds
 * INLINE_STORAGE_THRESHOLD. A node that sc_create_llist_node() abandons
 * halfway keeps the size of any buffer it didn't allocate at 0 from calloc,
 * so only what was allocated gets freed. */
static void sc_destroy_llist_node(dllist_root* all_elems, llist_node* elem,
                                  ccol_memmgmt_procs_t* m_procs) {
  if (elem) {
    if (elem->key_pair_accessor.size > INLINE_STORAGE_THRESHOLD &&
        elem->key_storage.ptr) {
      _ccol_mem_free(m_procs, elem->key_storage.ptr);
    }
    if (elem->val_pair_accessor.size > INLINE_STORAGE_THRESHOLD &&
        elem->val_storage.ptr) {
      _ccol_mem_free(m_procs, elem->val_storage.ptr);
    }
    if (all_elems) {
      detach_node_from_dllist(all_elems, &elem->dllist_refs);
    }
    _ccol_mem_free(m_procs, elem);
  }
}

/* Allocates a new llist_node and copies the key and value data into it. A
 * key or a value of 23 bytes or less is stored inline (SSO), anything bigger
 * in a separate heap buffer, and the accessor cmap_pair structs then point
 * into whichever storage was chosen, so a caller always goes through a
 * stable pointer. On success the node goes to the front of the
 * insertion-order dllist. */
/* Forced inline: it has two callers, the insert and the insert-or-get
 * instantiations of sc_insert_impl, and as a real call it costs the insert
 * path about 57 more instructions per new key than inlined, because the
 * sizes and the storage choice then don't fold into the caller. */
static inline __attribute__((always_inline)) llist_node* sc_create_llist_node(
    dllist_root* all_elems, ccol_memmgmt_procs_t* m_procs, size_t hash_val,
    const void* key_ptr, size_t key_size, const void* val_ptr,
    size_t val_size) {
  llist_node* new_elem =
      (llist_node*)_ccol_mem_calloc(m_procs, 1, sizeof(llist_node));
  if (!new_elem) return NULL;

  new_elem->hash_val = hash_val;

  // Each accessor is written only once its storage holds the bytes, so a
  // failure below leaves the size of a never-allocated buffer at 0; see
  // sc_destroy_llist_node.
  if (key_size <= INLINE_STORAGE_THRESHOLD) {
    memcpy(&new_elem->key_storage.inline_data, key_ptr, key_size);
    new_elem->key_pair_accessor.ptr = &new_elem->key_storage.inline_data;
  } else {
    new_elem->key_storage.ptr = _ccol_mem_alloc(m_procs, key_size);
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
    new_elem->val_storage.ptr = _ccol_mem_alloc(m_procs, val_size);
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

/* Compares two long double keys by VALUE, matching hash_long_double_value's
 * special cases exactly so the hash and the equality never disagree for the
 * same pair of keys.
 * - When one operand is NaN, the pair is equal only when BOTH are NaN, so
 *   every NaN long double collapses into one key, and no padding byte is
 *   read to decide it. The comment on hash_long_double_value says why: a NaN
 *   long double's padding is often really uninitialized memory, not just
 *   unspecified but stable content, so any comparison of it, even through
 *   memcmp, is a real hazard and not only a source of results that vary from
 *   run to run.
 * - In every other case the native `==` is used, which works on the value,
 *   so unspecified padding bits never affect the result, unlike a raw memcmp
 *   over the full representation. -0.0L and 0.0L compare equal here too,
 *   exactly like -0.0 and 0.0 do for the float and double key types. */
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

/* Compares a node's key with key_ptr. The size check is a fast reject, and
 * for a 4-byte or 8-byte key integers are compared instead of calling
 * memcmp, so the compiler can emit a single load and compare. key_type is
 * read for one purpose only: to send a long double key to
 * long_double_keys_equal instead of the generic byte-exact paths below. That
 * test has to come before the size dispatch, because sizeof(long double)
 * equals sizeof(double) on some platforms and ABIs, and a long double key
 * must never fall into the plain 8-byte integer compare there. */
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

/* sc_find_in_llist for a map with a custom key equality procedure. The
 * procedure is expected to agree with the hash, so the stored hash rejects most
 * nodes with one comparison here as well. The procedure gets both sizes and
 * decides on its own whether keys of different sizes can be equal. It's a
 * separate function so a map without the procedure keeps its search loop
 * as it is. */
static __attribute__((noinline)) llist_node* sc_find_in_llist_custom_eq(
    llist_node* head, size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_key_equality_proc_t custom_eq) {
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

/* How many nodes are in the chain that starts at head. */
static __attribute__((noinline)) size_t
sc_chain_length(const llist_node* head) {
  size_t length = 0;
  for (; head; head = head->next) length++;
  return length;
}

/* A linear search through a bucket's singly-linked chain. hash_val is the
 * caller's full hash of key_ptr, before any reduction, and it's compared
 * with each node's stored hash_val first, so most rejections cost one
 * size_t comparison and the memcmp/strcmp comparison inside sc_compare_keys
 * only runs when two hashes really collide. Gives the matching node, or
 * NULL. The bucket scaling strategy keeps the chains short, which is O(1) on
 * average.
 *
 * walk is NULL, or gets the number of nodes the search met when it finds no
 * match, which is the chain's length; inserting a new key reports that
 * number to the adaptive hash's detection. Every caller passes a constant
 * NULL or a pointer, so a lookup keeps no count. */
static inline __attribute__((always_inline)) llist_node* sc_find_in_llist(
    llist_node* head, size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_data_type key_type, ccol_key_equality_proc_t custom_eq, size_t* walk) {
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

/* Updates the value stored in an existing node. There are three cases,
 * depending on the new value size against the stored size: the same size,
 * where the value is overwritten in place; a smaller value that fits
 * inline, which moves to inline storage and frees the old heap buffer; and a
 * larger value, which reallocs the heap buffer or allocates a new one.
 *
 * val_ptr may alias this entry's current value storage, because a caller is
 * free to insert a value again from a pointer that chmap_get_elem_ref,
 * chmap_get_ptr or chmap_get gave it for this exact key. For a char* value
 * type chmap_get and chmap_get_ptr hand out a pointer straight into a
 * separate-chaining entry's stored bytes, which is their own documented
 * contract: "for a string, the macro gives the char* itself" and "pointer to
 * the char* itself". Every branch below changes the entry's existing storage
 * (it frees the heap buffer, overwrites the val_storage union in place, or
 * reallocs the heap buffer) before it would otherwise read val_ptr's bytes,
 * so an aliased val_ptr would see freed, corrupted or moved memory instead
 * of the value the caller wanted. The overlap check below is cheap and runs
 * once per call, while the snapshot copy only runs on the rare path where
 * the two alias, so the common case pays just the comparison. */
static bool sc_reset_val_of_llist_node(llist_node* elem, const void* val_ptr,
                                       size_t val_size,
                                       ccol_memmgmt_procs_t* m_procs) {
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
      heap_snapshot = _ccol_mem_alloc(m_procs, val_size);
      if (!heap_snapshot) return false;
      memcpy(heap_snapshot, val_ptr, val_size);
      val_ptr = heap_snapshot;
    }
  }

  // Each branch keeps a value inline exactly when its size is at most
  // INLINE_STORAGE_THRESHOLD, which is what sc_destroy_llist_node and this
  // function go by to know where a value is stored.
  bool ok = true;
  if (val_size == old_size) {
    memcpy(old_ptr, val_ptr, val_size);
  } else if (val_size <= INLINE_STORAGE_THRESHOLD) {
    if (!old_inline) {
      _ccol_mem_free(m_procs, elem->val_storage.ptr);
    }
    memcpy(&elem->val_storage.inline_data, val_ptr, val_size);
    elem->val_pair_accessor.ptr = &elem->val_storage.inline_data;
    elem->val_pair_accessor.size = val_size;
  } else {
    if (old_inline) {
      void* new_ptr = _ccol_mem_alloc(m_procs, val_size);
      if (!new_ptr) {
        ok = false;
      } else {
        elem->val_storage.ptr = new_ptr;
      }
    } else {
      void* orig = elem->val_storage.ptr;
      elem->val_storage.ptr =
          _ccol_mem_realloc(m_procs, elem->val_storage.ptr, val_size);
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
    _ccol_mem_free(m_procs, heap_snapshot);
  }
  return ok;
}

/* Removes the node matching key_ptr from a bucket's chain, sets *found, and
 * gives the chain's new head. As in sc_find_in_llist, hash_val lets the
 * search reject a non-matching node with one size_t comparison before it
 * falls back to sc_compare_keys. The previous-node pointer gives an O(n)
 * delete without a doubly-linked bucket list. */
/* Unlinks node from the chain headed by head, destroys it, and gives the
 * chain's new head. previous is the node before it, or NULL. */
static llist_node* sc_unlink_and_destroy(llist_node* head, llist_node* previous,
                                         llist_node* node,
                                         dllist_root* all_elems,
                                         ccol_memmgmt_procs_t* m_procs) {
  if (!previous) {
    head = node->next;
  } else {
    previous->next = node->next;
  }
  sc_destroy_llist_node(all_elems, node, m_procs);
  return head;
}

/* sc_delete_from_llist for a map with a custom key equality procedure. It's
 * a separate function for the reason sc_find_in_llist_custom_eq gives. */
static __attribute__((noinline)) llist_node* sc_delete_from_llist_custom_eq(
    llist_node* head, dllist_root* all_elems, ccol_memmgmt_procs_t* m_procs,
    size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_key_equality_proc_t custom_eq, bool* found) {
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

static llist_node* sc_delete_from_llist(
    llist_node* head, dllist_root* all_elems, ccol_memmgmt_procs_t* m_procs,
    size_t hash_val, const void* key_ptr, size_t key_size,
    ccol_data_type key_type, ccol_key_equality_proc_t custom_eq, bool* found) {
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

/* Destroys every node in a bucket's chain and gives NULL. Resetting and
 * destroying the map use it to clear every bucket in turn. */
static llist_node* sc_destroy_the_whole_llist(llist_node* head,
                                              dllist_root* all_elems,
                                              ccol_memmgmt_procs_t* m_procs) {
  llist_node* tracker = head;
  while (tracker) {
    llist_node* node_to_be_deleted = tracker;
    tracker = tracker->next;
    sc_destroy_llist_node(all_elems, node_to_be_deleted, m_procs);
  }
  return NULL;
}

/* Works out the scale-up and scale-down thresholds again from the current
 * bucket array size. Call it after every resize of the bucket array, so the
 * thresholds keep up with the new capacity.
 *
 * The result matches exactly the formulas chashmap.h documents,
 * "(bucket_count + 1) * 1.5" and "(bucket_count + 1) / 8", and it never
 * overflows size_t. The scale-up threshold is floor(3 * bucket_arr_size / 2),
 * the bucket_arr_size + bucket_arr_size / 2 expression below, which can't
 * overflow because bucket_arr_size is capped at
 * _ccol_max_power_of_two_size_t. Then a parity-dependent correction is
 * added, +1 for an even bucket_arr_size and +2 for an odd one, which comes
 * from writing floor((n+1)*3/2) in terms of floor(3n/2); a direct
 * (bucket_arr_size + 1) * 3 would overflow size_t at that same upper bound.
 * The scale-down threshold has no such risk, since bucket_arr_size + 1 by
 * itself never overflows. */
static void sc_set_scaling_limits(sep_chain_map* map) {
  size_t base_up = map->bucket_arr_size + map->bucket_arr_size / 2;
  map->elem_count_to_scale_up =
      base_up + ((map->bucket_arr_size % 2 == 0) ? 1 : 2);
  map->elem_count_to_scale_down = (map->bucket_arr_size + 1) / 8;
}

/* The bucket array the map's block holds right after struct chashmap, with
 * its length in *count. A map whose first bucket array is small starts with
 * this one, so creating it is a single allocation; the map uses it while its
 * size is that length and never frees it, since it's part of the block.
 *
 * A block without such an array gives NULL and a count of 0. The address
 * right after the struct is then the end of the block, or its procs copy,
 * and an allocator that hands out adjacent blocks with no header can put the
 * map's separate bucket array exactly there, so comparing that array with
 * the address would take it for part of the block and the map would never
 * free it. */
static inline llist_node** sc_inline_bucket_arr(sep_chain_map* map,
                                                size_t* count) {
  *count = map->inline_buckets;
  return *count ? (llist_node**)(chmap_of_sc(map) + 1) : NULL;
}

/* The bound on the nodes a window's 256 inserts met, above which a fast map
 * switches: 256 * (4.5 * lambda + 0.5), lambda being the number of entries
 * per bucket, which is how many nodes a new key meets on average under a
 * random function. lambda is the highest load the window can have reached:
 * the count when the window started plus its 256 inserts, and never more
 * than the count at which the bucket array grows; oa_window_bound says why
 * the load at the window's end won't do.
 *
 * The arithmetic is in integers with lambda in 16.16 fixed point, so the
 * bound is 1152 * lambda / 65536 + 128, within two counts and within one
 * percent of the real-valued bound. start_count is the count when the
 * window starts and scale_up the count at which the bucket array grows. */
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

/* Starts a new insert window and computes its bound from the current count.
 * A keyed map is never judged, so it skips the arithmetic. */
static inline void sc_window_restart(sep_chain_map* map) {
  struct chashmap* chm = chmap_of_sc(map);
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound =
      chm->hash_mode == CHMAP_HASH_KEYED
          ? UINT16_MAX
          : sc_window_bound(map->elem_count, map->elem_count_to_scale_up,
                            map->bucket_arr_size);
}

/* The hash a separate-chaining map's node stores for its key, in the mode
 * keyed names. */
static inline size_t sc_hash_of_node(const sep_chain_map* map,
                                     const llist_node* node, bool keyed) {
  return hash_key_data(
      node->key_pair_accessor.ptr, node->key_pair_accessor.size, map->key_type,
      map->custom_hashing_proc, keyed, g_chmap_hash_secret.int_seed);
}

/* Switches a fast map to the keyed hash: every node gets its keyed hash and
 * is linked into the same bucket array again. Nothing moves and nothing is
 * allocated, so the switch can't fail, and every accessor, every entry
 * reference, the insertion order and every iterator stay valid. */
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

/* The cold path of inserting a new key that met SC_CHAIN_CAP nodes or more,
 * which switches a fast map. */
static __attribute__((noinline)) void sc_note_long_chain(sep_chain_map* map) {
  if (!sc_keyed(map)) {
    sc_switch_to_keyed(map);
  }
}

/* The cold path of the insert that ends a window, which switches a fast map
 * whose window sum is above its bound. */
static __attribute__((noinline)) void sc_window_end(sep_chain_map* map) {
  struct chashmap* chm = chmap_of_sc(map);
  bool switch_now = chm->hash_mode == CHMAP_HASH_FAST &&
                    chm->insert_window > chm->window_bound;
  sc_window_restart(map);
  if (switch_now) {
    sc_switch_to_keyed(map);
  }
}

/* The slack in a fast attempt's walk budget; see sc_relink_fast_attempt.
 */
#define SC_ATTEMPT_SLACK 64u

/* The fast attempt when a keyed map grows: links every node into the
 * zeroed bucket array new_arr of new_size buckets with the fast hash, which
 * it stores in the node, and gives true. A node goes to the front of its
 * chain after walking the nodes already there, so the attempt knows every
 * chain's length, and it stops as soon as it shows the fast hash is bad for
 * these keys: when a chain would reach SC_CHAIN_CAP nodes, or when the walks
 * so far add up to more than the budget elem_count + SC_ATTEMPT_SLACK. A
 * random function at the load after a growth, at most three eighths of an
 * entry per bucket, walks about a fifth of the count in total, so neither
 * limit refuses an ordinary key set.
 *
 * A failed attempt then links every node again with the keyed hash into the
 * same array and gives false. The nodes the attempt reached hold the fast
 * hash and get the keyed one back, and the rest hold the keyed one already.
 * Nothing moves and nothing is allocated, so every accessor, every entry
 * reference, the insertion order and every iterator stay valid either way. A
 * failed attempt hashes at most elem_count nodes with the fast hash and walks
 * at most elem_count + SC_ATTEMPT_SLACK + SC_CHAIN_CAP - 1 chain nodes, which
 * is linear in the count whatever the keys are. */
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

/* Resizes the bucket array by scale_factor, up or down, and links every node
 * into its new bucket, which hash_index_for picks from the stored hash; a
 * power-of-two size is what makes that shift well defined. A resize restarts
 * the insert window. A shrink merges neighbouring buckets' chains, since the
 * new index is the old one shifted right, and because the relink below
 * visits every node anyway it measures the merged chains: a fast map with a
 * merged chain of SC_CHAIN_CAP nodes or more switches. Growing a keyed map
 * makes its fast attempt instead of the relink (see sc_relink_fast_attempt),
 * and a shrink keeps the mode. */
static void sc_scale(sep_chain_map* map, bool up) {
  if (up &&
      (map->bucket_arr_size > _ccol_max_power_of_two_size_t / scale_factor)) {
    // Past the scale-up limit
    return;
  }

  size_t new_size = up ? map->bucket_arr_size * scale_factor
                       : map->bucket_arr_size / scale_factor;

  // Dividing by scale_factor (4x) can go below the minimum bucket array
  // size for any bucket_arr_size that isn't on the
  // "minimum_allowed_bucket_array_size * 4^k" line, 32 for example:
  // chmap_create_full and chmap_reset get to 32 directly for any requested
  // size between minimum_allowed_bucket_array_size and
  // minimum_allowed_bucket_array_size * scale_factor, and 32 / 4 == 8 is
  // below the floor. sc_delete's caller only guarantees
  // bucket_arr_size > minimum_allowed_bucket_array_size before this call,
  // not that the size divides down cleanly, so we clamp here; without the
  // clamp a table off that line would drop below the floor.
  if (!up && new_size < minimum_allowed_bucket_array_size) {
    new_size = minimum_allowed_bucket_array_size;
  }

  // A shrink to the length of the in-block bucket array takes that array
  // back, which allocates nothing and can't fail. The map isn't using it
  // right now, because the current size differs from the new one.
  size_t inline_count = 0;
  llist_node** new_arr = sc_inline_bucket_arr(map, &inline_count);
  if (new_size == inline_count) {
    memset(new_arr, 0, new_size * sizeof(llist_node*));
  } else {
    new_arr = (llist_node**)_ccol_mem_calloc(map->m_procs, new_size,
                                             sizeof(llist_node*));
    if (!new_arr) return;
  }

  // On a shrink old buckets i * ratio to i * ratio + ratio - 1 merge into
  // new bucket i. merged counts the nodes of the current group.
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

  // The in-block array's address is worked out again here, so the relink
  // loop above keeps no register for it.
  if (map->bucket_arr != sc_inline_bucket_arr(map, &inline_count)) {
    _ccol_mem_free(map->m_procs, map->bucket_arr);
  }
  map->bucket_arr = new_arr;
  map->bucket_arr_size = new_size;
  sc_set_scaling_limits(map);
  sc_window_restart(map);
  if (!up && longest_merged >= SC_CHAIN_CAP && !sc_keyed(map)) {
    sc_switch_to_keyed(map);
  }
}

/* Initializes the separate-chaining state in a map's block. The first
 * bucket array is the in-block one when the block has one, which is zeroed
 * already; otherwise it gets allocated, and calloc zeroes every bucket
 * pointer. Gives false, with nothing allocated, when that allocation fails. */
static bool sc_init(sep_chain_map* map, size_t bucket_arr_size,
                    ccol_data_type key_type, ccol_data_type val_type,
                    ccol_memmgmt_procs_t* m_procs,
                    ccol_hashing_proc_t custom_hashing_proc,
                    ccol_key_equality_proc_t custom_key_equality_proc) {
  size_t inline_count = 0;
  llist_node** inline_arr = sc_inline_bucket_arr(map, &inline_count);
  if (bucket_arr_size == inline_count) {
    map->bucket_arr = inline_arr;
  } else {
    map->bucket_arr = (llist_node**)_ccol_mem_calloc(m_procs, bucket_arr_size,
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

/* Inserts or updates a key-value pair in the separate-chaining map. For a
 * key already present sc_reset_val_of_llist_node updates the value in place;
 * for a new key a node is made and put at the front of the bucket's chain,
 * and after the insert the bucket array scales up once elem_count reaches
 * elem_count_to_scale_up.
 *
 * The lookup for an existing key runs before the ccol_max_elem_count check,
 * not after, because the fullness check only makes sense for inserting a
 * really new key, and updating a key that's already present never changes
 * elem_count at all. With the fullness check first, a map at
 * ccol_max_elem_count would reject a plain value update for a key it already
 * holds and answer ccol_container_full instead of updating.
 */
/* slot_out means the same as for oa_insert_impl(). key_slot_out, when not
 * NULL, gets the accessor of the same entry's stored key wherever slot_out
 * gets its value accessor. Every caller passes a constant for both, so each
 * instantiation keeps only its own arms. */
static inline __attribute__((always_inline)) ccol_retval_t sc_insert_impl(
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

  // The adaptive hash's detection; see the adaptive hashing section. A
  // switch rehashes the nodes in place, so new_node stays valid.
  if (__builtin_expect(walk >= SC_CHAIN_CAP, 0)) {
    sc_note_long_chain(map);
  }
  if (__builtin_expect(chmap_window_add(chmap_of_sc(map), walk), 0)) {
    sc_window_end(map);
  }

  if (map->elem_count >= map->elem_count_to_scale_up) {
    sc_scale(map, true);
  }

  // A scale relinks the nodes and never moves one, so the new node's
  // accessor stays valid across it.
  if (slot_out) {
    *slot_out = &new_node->val_pair_accessor;
    if (key_slot_out) {
      *key_slot_out = &new_node->key_pair_accessor;
    }
  }
  return ccol_success;
}

static ccol_retval_t sc_insert(sep_chain_map* map, const cmap_pair* key_pair,
                               const cmap_pair* val_pair) {
  return sc_insert_impl(map, key_pair, val_pair, NULL, NULL);
}

/* The separate-chaining arm of ccol_chmap_insert_or_get_elem(): copies the
 * node's value accessor into the caller's pair. */
static ccol_retval_t sc_insert_or_get(sep_chain_map* map,
                                      const cmap_pair* key_pair,
                                      const cmap_pair* val_pair,
                                      cmap_pair* slot_out) {
  const cmap_pair* accessor = NULL;
  ccol_retval_t ret = sc_insert_impl(map, key_pair, val_pair, &accessor, NULL);
  if (accessor) {
    *slot_out = *accessor;
  }
  return ret;
}

static ccol_retval_t sc_insert_or_get_entry(sep_chain_map* map,
                                            const cmap_pair* key_pair,
                                            const cmap_pair* val_pair,
                                            const cmap_pair** slot_out,
                                            const cmap_pair** key_slot_out) {
  return sc_insert_impl(map, key_pair, val_pair, slot_out, key_slot_out);
}

/* Looks up key_pair and points *val_pair at the node's value accessor. The
 * accessor stays at its address until that key is deleted, and its fields
 * only change when an update gives the key a value of a different size. It
 * has two callers, sc_get() and _chmap_get_val_accessor(), and is forced
 * inline into both, because calling an out-of-line copy costs a string
 * lookup about 15 percent more instructions. */
static inline __attribute__((always_inline)) ccol_retval_t sc_find_accessor(
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

/* Looks up key_pair and fills the caller's pair with a copy of the node's
 * value accessor, or with {NULL, 0} when the key is absent. */
static ccol_retval_t sc_get(sep_chain_map* map, const cmap_pair* key_pair,
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

/* Deletes the entry matching key_pair from its bucket's chain. The bucket
 * array scales down when elem_count drops below elem_count_to_scale_down and
 * the array is large enough to shrink. */
static ccol_retval_t sc_delete(sep_chain_map* map, const cmap_pair* key_pair) {
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

/* Destroys every bucket chain and frees the bucket array unless it's the
 * in-block one; the state itself lives in that block, which __chmap_destroy
 * frees. The insertion-order dllist's root (&map->all_elems) is passed to
 * each chain's teardown, which detaches every node from that list before
 * freeing it, so the root never names a freed node. */
static void sc_destroy(sep_chain_map* map) {
  // Locals, since each free is an opaque call after which the compiler
  // would otherwise load these from the map again for every bucket.
  llist_node** const buckets = map->bucket_arr;
  const size_t bucket_count = map->bucket_arr_size;
  ccol_memmgmt_procs_t* const m_procs = map->m_procs;
  for (size_t i = 0; i < bucket_count; i++) {
    sc_destroy_the_whole_llist(buckets[i], &map->all_elems, m_procs);
  }
  size_t inline_count = 0;
  if (buckets != sc_inline_bucket_arr(map, &inline_count)) {
    _ccol_mem_free(m_procs, buckets);
  }
}

/* Clears every entry, and can also resize the bucket array to
 * new_bucket_array_size. Pass 0 to keep the current size. */
static ccol_retval_t sc_reset(sep_chain_map* map,
                              size_t new_bucket_array_size) {
  {
    // Locals, for the reason sc_destroy gives.
    llist_node** const buckets = map->bucket_arr;
    const size_t bucket_count = map->bucket_arr_size;
    ccol_memmgmt_procs_t* const m_procs = map->m_procs;
    for (size_t i = 0; i < bucket_count; i++) {
      buckets[i] =
          sc_destroy_the_whole_llist(buckets[i], &map->all_elems, m_procs);
    }
  }

  if (new_bucket_array_size > 0 &&
      new_bucket_array_size != map->bucket_arr_size) {
    /* We form the byte count here instead of leaving it to the allocator,
       which can't check a product that arrives already multiplied.
       chmap_reset accepts any power of two up to ccol_max_elem_count, and
       that many pointers overflow size_t on every supported target. A
       product that wraps to zero is the dangerous one: realloc may then
       release the block and return NULL, and the recovery path below would
       write through a pointer the allocator already took back. We report a
       failed allocation, which is what it is, and every element above is
       destroyed anyway, as chmap_reset documents. */
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
      // The in-block array has this length and the map isn't using it, since
      // the current length differs. Taking it back can't fail.
      _ccol_mem_free(map->m_procs, orig);
      map->bucket_arr = inline_arr;
    } else if (orig == inline_arr) {
      // The in-block array is part of the block, so it can't be reallocated.
      // A new array replaces it, and it stays in the block.
      map->bucket_arr = _ccol_mem_alloc(
          map->m_procs, new_bucket_array_size * sizeof(llist_node*));
    } else {
      map->bucket_arr =
          _ccol_mem_realloc(map->m_procs, map->bucket_arr,
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

/* ========================================================================== */
/*                    ITERATOR STRUCTURES                                     */
/* ========================================================================== */

typedef struct chmap_cmap_iterator {
  chmap parent_map;
  /* The free function of the allocator that made this iterator, or NULL for
   * the default one. The scope-exit cleanup of ccol_iter_declare can free an
   * iterator that a loop left early after the caller already destroyed the
   * map, and the procs struct the map owns along with it, so freeing the
   * iterator must never read the map. */
  ccol_free_t free_fn;
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

/* Creates a new hash map, picking the implementation (open addressing or
 * separate chaining) at creation time from the key type and the value
 * type.
 *
 * The map is one block; see struct chashmap. A separate-chaining map whose
 * first bucket array has at most minimum_allowed_bucket_array_size buckets
 * keeps that array in the block too, so creating it is one allocation,
 * while an open-addressing map allocates its table separately, because
 * every resize replaces it.
 *
 * When the caller gives a custom allocator, the map keeps a pointer to a
 * procs struct that lives as long as the map. A pointer ccol_procs_intern()
 * gave lives for the whole process, so the map keeps that pointer itself;
 * any other procs struct is copied into the block, so the caller can free
 * its own copy as soon as the call returns.
 *
 * sc_min_buckets is the smallest first bucket array of a separate-chaining
 * map: minimum_allowed_bucket_array_size for the public constructors, and
 * compact_bucket_array_size for ccol_chmap_create_compact(). */
static chmap chmap_create_impl(
    size_t initial_bucket_array_size, ccol_data_type key_type,
    ccol_data_type val_type, ccol_memmgmt_procs_t* mmgmt_procs,
    ccol_hashing_proc_t custom_hashing_proc,
    ccol_key_equality_proc_t custom_key_equality_proc, size_t sc_min_buckets,
    char** err) {
  if (initial_bucket_array_size == 0) {
    if (err) *err = _CCOL_ERR_STR("initial_bucket_array_size is zero");
    return NULL;
  }

  if (custom_key_equality_proc && !custom_hashing_proc) {
    // The built-in hash reads a key's bytes, or its value for a floating
    // type, so a custom equality that treats two different byte images as
    // one key disagrees with it, and a lookup misses a key that is present.
    // We refuse such a map here rather than at the first miss.
    if (err) {
      *err = _CCOL_ERR_STR(
          "custom_key_equality_proc needs a custom_hashing_proc that agrees "
          "with it");
    }
    return NULL;
  }

  if (!_ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  // A custom key equality picks separate chaining. The open-addressing
  // probe compares a key as one 64-bit integer, which keeps that loop free
  // of an indirect call that a map without the procedure would otherwise
  // pay a branch for.
  bool open_addressing = !custom_key_equality_proc &&
                         should_use_open_addressing(key_type, val_type);
  size_t floor =
      open_addressing ? minimum_allowed_bucket_array_size : sc_min_buckets;

  if (initial_bucket_array_size <= floor) {
    initial_bucket_array_size = floor;
  } else {
    initial_bucket_array_size =
        _ccol_find_nearest_gte_power_of_two(initial_bucket_array_size);
    if (initial_bucket_array_size > ccol_max_elem_count) {
      if (err) {
        *err = _CCOL_ERR_STR("Initial bucket array size is too big");
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
      procs_offset + (copy_procs ? sizeof(ccol_memmgmt_procs_t) : 0);

  chmap chm = (chmap)_ccol_mem_alloc(mmgmt_procs, block_bytes);
  if (!chm) {
    if (err) *err = _CCOL_ERR_STR("Failed to allocate chashmap");
    return NULL;
  }
  chm->hash_mode = CHMAP_HASH_FAST;
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound = UINT16_MAX;
  memset(chm + 1, 0, inline_buckets * sizeof(llist_node*));

  ccol_memmgmt_procs_t* m_procs = mmgmt_procs;
  if (copy_procs) {
    m_procs = (ccol_memmgmt_procs_t*)((uint8_t*)chm + procs_offset);
    memcpy(m_procs, mmgmt_procs, sizeof(ccol_memmgmt_procs_t));
  }

  // The secret is picked before any map's first hash; see
  // chmap_process_hash_secret.
  (void)chmap_process_hash_secret();

  if (open_addressing) {
    chm->impl_type = IMPL_OPEN_ADDRESSING;
    chm->impl.oa = &chm->state.oa;
    if (!oa_init(chm->impl.oa, initial_bucket_array_size, key_type, val_type,
                 get_type_size(key_type), get_type_size(val_type), m_procs,
                 custom_hashing_proc)) {
      if (err) *err = _CCOL_ERR_STR("Failed to create open addressing map");
      _ccol_mem_free(mmgmt_procs, chm);
      return NULL;
    }
    oa_window_restart(chm->impl.oa);
  } else {
    chm->impl_type = IMPL_SEPARATE_CHAINING;
    chm->impl.sc = &chm->state.sc;
    chm->state.sc.inline_buckets = (uint8_t)inline_buckets;
    if (!sc_init(chm->impl.sc, initial_bucket_array_size, key_type, val_type,
                 m_procs, custom_hashing_proc, custom_key_equality_proc)) {
      if (err) *err = _CCOL_ERR_STR("Failed to create separate chaining map");
      _ccol_mem_free(mmgmt_procs, chm);
      return NULL;
    }
  }

  if (err) *err = NULL;
  return chm;
}

chmap chmap_create_full(size_t initial_bucket_array_size,
                        ccol_data_type key_type, ccol_data_type val_type,
                        ccol_memmgmt_procs_t* mmgmt_procs,
                        ccol_hashing_proc_t custom_hashing_proc,
                        ccol_key_equality_proc_t custom_key_equality_proc,
                        char** err) {
  return chmap_create_impl(initial_bucket_array_size, key_type, val_type,
                           mmgmt_procs, custom_hashing_proc,
                           custom_key_equality_proc,
                           minimum_allowed_bucket_array_size, err);
}

/* See chashinsert.h. */
chmap ccol_chmap_create_compact(ccol_data_type key_type,
                                ccol_data_type val_type,
                                ccol_memmgmt_procs_t* mmgmt_procs, char** err) {
  return chmap_create_impl(compact_bucket_array_size, key_type, val_type,
                           mmgmt_procs, NULL, NULL, compact_bucket_array_size,
                           err);
}

/* Gives the key type the map was made with, whatever the backend. */
static inline ccol_data_type chmap_key_type(chmap chm) {
  return chm->impl_type == IMPL_OPEN_ADDRESSING ? chm->impl.oa->key_type
                                                : chm->impl.sc->key_type;
}

/* Gives the value type the map was made with, whatever the backend. */
static inline ccol_data_type chmap_val_type(chmap chm) {
  return chm->impl_type == IMPL_OPEN_ADDRESSING ? chm->impl.oa->val_type
                                                : chm->impl.sc->val_type;
}

/* Without this the map's key equality is bitwise (see oa_keys_equal and
 * sc_compare_keys), so +0.0 and -0.0 would hash to different buckets and
 * compare unequal, although `0.0 == -0.0` in C, which would surprise anyone
 * with a float or double key type. For every other numeric key type the
 * bitwise equality already agrees with C's equality. When key_type is
 * ccol_float or ccol_double, key_pair is copied into out_canon_pair and
 * out_canon_buf, a negative-zero bit pattern becomes a positive zero, and
 * out_canon_pair comes back; otherwise key_pair comes back unchanged. Every
 * other bit pattern is left alone, each NaN payload included, and those are
 * never equal to anything under `==`, not even to themselves. So this map's
 * key equality for a floating-point key is bitwise equality with the signed
 * zero collapsed, not IEEE-754 equality. out_canon_buf needs to hold at least
 * key_pair->size bytes; ccol_float and ccol_double are always 8 bytes or
 * less, so a uint64_t is enough for every key this function changes. */
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

/* The map's idea of key identity, for a module that has to agree with the
 * map about which keys are the same key. It's the keyed mode's hash,
 * whatever mode any map is in, because key identity doesn't depend on the
 * mode. The canonical form is applied first, and that's what makes the
 * answer an identity rather than a representation: -0.0 and 0.0 are one key
 * for a float and a double, and a long double is taken apart by value, so
 * its padding bytes are never read. See chashkey.h. */
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

/* The open-addressing backend stores a key-value pair inline, in a slot's
 * fixed 8-byte key_data and val_data fields (see oa_slot), and oa_insert's
 * memcpy calls trust key_pair->size and val_pair->size completely, with no
 * bounds check of their own. map->key_size and map->val_size are fixed from
 * key_type and val_type at creation, and are 8 bytes or less only for the
 * open-addressing backend. A caller's size that doesn't match them would
 * quietly overwrite the neighbouring slot's key and value bytes, corrupting
 * or losing an unrelated entry already stored there, and would overflow the
 * slot array outright when the target slot is near its end. Rejecting a
 * mismatched size up front is what makes all of that structurally
 * unreachable, not just unlikely. */
static inline bool oa_val_size_matches(const open_addr_map* map,
                                       size_t val_size) {
  return val_size == map->val_size;
}

/* Answers whether key_size is safe to hand to hash_key_data(), and for a
 * float, double or long double key_type also whether it's safe for
 * canonicalize_key_pair_if_needed(), hash_long_double_value() and
 * long_double_keys_equal(). All of those dispatch on key_type and then read
 * a FIXED number of bytes, the C type's sizeof(), straight out of the
 * caller's pointer. That holds for every type is_type_integral() knows
 * (char, short, int, long, long long, their unsigned equivalents, float,
 * double and a pointer). It also holds for ccol_long_double, which this
 * function checks on purpose: is_type_integral() leaves long double out, but
 * only for the backend choice (see should_use_open_addressing). A long double
 * key always goes to separate chaining, yet it's hashed and compared with a
 * fixed-size read of sizeof(long double), the same class of hazard as every
 * other fixed-width type here. The key_size parameter itself only matters
 * for a key type with really no fixed width, such as ccol_string and
 * ccol_other_types, which SipHash-1-3 always hashes with exactly the
 * caller's key_size.
 *
 * So this check must run for BOTH backends whenever key_type has a fixed
 * width, not just for open-addressing. A caller can pass a cmap_pair through
 * the raw chmap_insert_elem, chmap_get_elem_ref or chmap_delete_elem layer
 * with a key_size smaller than the type's real size, and without this check
 * hash_key_data(), canonicalize_key_pair_if_needed(),
 * hash_long_double_value() and long_double_keys_equal() then read past the
 * end of that caller's buffer. It's reachable on the separate-chaining
 * backend whenever a fixed-width key type comes with a value type that isn't
 * integral or is bigger than 8 bytes, since such a value type forces
 * separate chaining whatever the key type (int->char* and double->char* for
 * example), and for long double it's always reachable, because a long double
 * always forces separate chaining. It's neither an open-addressing-only
 * concern nor theoretical. The open-addressing backend always has a key_type
 * is_type_integral() accepts, by construction (see
 * should_use_open_addressing), so this one check also covers that backend's
 * own hazard, a mismatched size corrupting a neighbouring slot, and no
 * separate open-addressing-only check is needed next to it.
 *
 * The set of fixed-width key types, and each one's expected size, comes from
 * ccol_fixed_width_data_type_size() in common.h, not from is_type_integral()
 * or get_type_size() above, because cbstmap enforces the identical rule on
 * its own key_pairs and the two modules must agree on exactly which types
 * it covers. The local get_type_size() is no substitute: it answers another
 * question, how wide an open-addressing slot needs to be, and it deliberately
 * reports 8 for a type with no fixed width at all, which would turn every
 * ccol_string key that isn't 8 bytes long into a false rejection. */
static inline bool key_size_matches_type_if_fixed_width(ccol_data_type key_type,
                                                        size_t key_size) {
  size_t fixed_width = ccol_fixed_width_data_type_size(key_type);
  return fixed_width == 0 || key_size == fixed_width;
}

/* Answers whether val_size agrees with the map's declared value type, for a
 * value type that has one fixed width.
 *
 * oa_val_size_matches() above answers the same question for the
 * open-addressing backend against the exact field its memcpy targets; that
 * check is that backend's memory-safety requirement, and it stays. This one
 * covers the SEPARATE-CHAINING backend, which has no such requirement: it
 * allocates exactly val_pair->size bytes for whatever it gets, so a value of
 * the wrong width can never run past its own storage.
 *
 * Such a value is still wrong, though, and without this check the failure
 * shows up at the wrong call site. A node holding two bytes in a map whose
 * declared value type is four bytes accepts the insert and reports success,
 * and chmap_get()'s value-size guard then stops the process on the first
 * READ of that key, at a call site that did nothing wrong, with a message
 * naming chmap_get() and hinting at a missing chmap_redeclare(). This check
 * moves the rejection to the call that really built the bad pair.
 *
 * Only the raw chmap_insert_elem() layer can get here, because the
 * type-inferred chmap_insert() macro converts the value through the map's
 * declared value type first, so every pair it builds is exactly
 * sizeof(ValT) bytes wide by construction.
 *
 * A value type with no fixed width gives 0 from
 * ccol_fixed_width_data_type_size() and is never checked: ccol_string is one,
 * and so is ccol_other_types, which covers a caller's struct, and both
 * rightly carry a different size for every entry. */
static inline bool val_size_matches_type_if_fixed_width(ccol_data_type val_type,
                                                        size_t val_size) {
  size_t fixed_width = ccol_fixed_width_data_type_size(val_type);
  return fixed_width == 0 || val_size == fixed_width;
}

/* The checks and the dispatch an insert and an update share. slot_out means
 * the same as for oa_insert_impl(). entry_val_out and key_slot_out are
 * sc_insert_impl()'s accessor out parameters; a caller passes them together,
 * never together with slot_out, and only for a separate-chaining map. */
static inline __attribute__((always_inline)) ccol_retval_t
chmap_insert_elem_impl(chmap chm, const cmap_pair* key_pair,
                       const cmap_pair* val_pair, cmap_pair* slot_out,
                       const cmap_pair** entry_val_out,
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

/* The public dispatch for an insert and for an update: checks the inputs,
 * then calls the backend's insert function. */
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair) {
  return chmap_insert_elem_impl(chm, key_pair, val_pair, NULL, NULL, NULL);
}

/* One hash and one probe that insert a new key or find the key already
 * present, and fill the caller's pair with the stored value's {ptr, size}
 * either way. See chashinsert.h. */
ccol_retval_t ccol_chmap_insert_or_get_elem(chmap chm,
                                            const cmap_pair* key_pair,
                                            const cmap_pair* val_pair,
                                            cmap_pair* val_slot) {
  if (!val_slot) {
    return ccol_invalid_args;
  }
  val_slot->ptr = NULL;
  val_slot->size = 0;
  return chmap_insert_elem_impl(chm, key_pair, val_pair, val_slot, NULL, NULL);
}

/* chashinsert.h's ordered cursor. An entry reference is the address of a
 * separate-chaining node's dllist_refs member, so each step is one load of
 * prev and no allocation. */
const ccol_chmap_entry_ref* ccol_chmap_oldest_entry(chmap chm) {
  if (!chm || chm->impl_type != IMPL_SEPARATE_CHAINING) {
    return NULL;
  }
  return (const ccol_chmap_entry_ref*)chm->impl.sc->all_elems.tail;
}

const ccol_chmap_entry_ref* ccol_chmap_entry_read(
    const ccol_chmap_entry_ref* entry, const cmap_pair** key,
    const cmap_pair** val) {
  dllist_ref_node* tracker = (dllist_ref_node*)entry;
  llist_node* host = dllistRefNodePtr2LlistNodePtr(tracker);
  *key = &host->key_pair_accessor;
  *val = &host->val_pair_accessor;
  return (const ccol_chmap_entry_ref*)tracker->prev;
}

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

/* The insert-or-get above, also handing back the stored key's accessor.
 * Only a separate-chaining entry has one; see chashinsert.h. */
ccol_retval_t ccol_chmap_insert_or_get_entry(chmap chm,
                                             const cmap_pair* key_pair,
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

/* Fills the caller's pair with the {ptr, size} of key_pair's stored value:
 * ptr points at the value bytes inside the slot or the node, and size is the
 * stored size. Every failure with a non-NULL val_pair sets it to
 * {NULL, 0}. */
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair* key_pair,
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

/* The lookup behind chmap_get_ptr()'s string arm; see chashmap.h. A
 * string-valued map is always a separate-chaining map, and the only char*
 * object of such an entry is the ptr field of its node's value accessor, so
 * this hands out the address of that accessor. An open-addressing map has
 * no accessor to hand out and is refused with ccol_invalid_args. Every
 * failure with a non-NULL val_accessor sets it to NULL. */
ccol_retval_t _chmap_get_val_accessor(chmap chm, const cmap_pair* key_pair,
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
  ccol_retval_t ret = sc_find_accessor(chm->impl.sc, key_pair, val_accessor);
  if (ret != ccol_success) {
    *val_accessor = NULL;
  }
  return ret;
}

/* Looks up key_pair and copies up to target_buf_size bytes of the value into
 * target_buf. When the stored value is smaller than target_buf_size, the
 * remaining bytes are zeroed. */
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair* key_pair,
                                  void* target_buf, size_t target_buf_size) {
  if (!target_buf || target_buf_size == 0) {
    return ccol_invalid_args;
  }
  cmap_pair val_pair;
  ccol_retval_t ret = chmap_get_elem_ref(chm, key_pair, &val_pair);
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

/* The public dispatch for a delete: checks the inputs, then calls the
 * backend. */
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
    return oa_delete(chm->impl.oa, key_pair);
  } else {
    return sc_delete(chm->impl.sc, key_pair);
  }
}

/* Gives the number of live key-value pairs in the map. */
size_t chmap_elem_count(chmap chm) {
  if (!chm) {
    _ccol_assert(false);
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->count;
  } else {
    return chm->impl.sc->elem_count;
  }
}

/* Clears every entry, and can also resize the internal bucket array,
 * rounding the size up to the nearest power of two and clamping it to
 * minimum_allowed_bucket_array_size. Pass 0 to keep the current
 * size. */
ccol_retval_t chmap_reset(chmap chm, size_t new_bucket_array_size) {
  if (!chm) {
    _ccol_assert(false);
  }

  bool requested_size_too_big = false;
  if (new_bucket_array_size > 0 &&
      new_bucket_array_size < minimum_allowed_bucket_array_size) {
    new_bucket_array_size = minimum_allowed_bucket_array_size;
  } else if (new_bucket_array_size > 0) {
    new_bucket_array_size =
        _ccol_find_nearest_gte_power_of_two(new_bucket_array_size);
    if (new_bucket_array_size > ccol_max_elem_count) {
      // The requested size is too big, so we fall back to 0, which keeps the
      // current capacity, rather than skipping the reset. That way this
      // failure path follows the documented contract too: the reset destroys
      // every element whatever the return value is, and never quietly leaves
      // every element in place.
      new_bucket_array_size = 0;
      requested_size_too_big = true;
    }
  }

  ccol_retval_t r;
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

/* Makes an iterator pointing at the first element, or gives NULL for an
 * empty map. A separate-chaining map iterates through the insertion-order
 * dllist, while an open-addressing map finds the first occupied slot that
 * isn't deleted with a linear scan. The iterator's chmap_cmap_iterator is
 * allocated on the heap. */
static cmap_iterator* chmap_iter_next(cmap_iterator* iter);

#ifdef RUNNING_UNIT_TESTS
/* A white-box regression guard tracking the NET count of
 * chmap_cmap_iterator allocations that chashmap_begin_iter() made and
 * __chmap_iterator_destroy() hasn't freed yet, across every chmap in the
 * process.
 *
 * valgrind alone isn't a dependable guard for this balance, because
 * chashmap_begin_iter() hands back a pointer to a field inside the iterator
 * struct rather than the struct's base address, so valgrind reports a live,
 * perfectly valid iterator as "possibly lost", its classification for a live
 * reference through an interior pointer. That report is also noisy under
 * concurrent load, and missing on most runs.
 *
 * This counter is the always-on, deterministic alternative: any regression
 * in the allocate and free balance of ANY chashmap iterator anywhere in the
 * process gives a loud, immediate abort() the moment it happens (see
 * _check_chmap_iter_balance_at_exit in tests/ctls/tests.c), whereas a
 * valgrind report for the same problem is rare and hard to reproduce. */
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
    if (!chm->impl.sc->all_elems.head) {
      return NULL;
    }

    ccol_memmgmt_procs_t* m_procs = chm->impl.sc->m_procs;
    chmap_cmap_iterator* real_iter =
        _ccol_mem_calloc(m_procs, 1, sizeof(chmap_cmap_iterator));
    if (!real_iter) {
      if (err) {
        *err = _CCOL_ERR_STR("Failed to allocate iterator");
      }
      return NULL;
    }

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif /* RUNNING_UNIT_TESTS */
    dllist_ref_node* tracker = chm->impl.sc->all_elems.head;
    real_iter->parent_map = chm;
    real_iter->free_fn = m_procs ? m_procs->free : NULL;
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
    open_addr_map* map = chm->impl.oa;

    // Find the first occupied slot
    size_t i = 0;
    while (i < map->capacity && !(map->metadata[i] & SLOT_OCCUPIED)) {
      i++;
    }

    if (i >= map->capacity) {
      return NULL;  // Empty map
    }

    chmap_cmap_iterator* real_iter =
        _ccol_mem_calloc(map->m_procs, 1, sizeof(chmap_cmap_iterator));
    if (!real_iter) {
      if (err) {
        *err = _CCOL_ERR_STR("Failed to allocate iterator");
      }
      return NULL;
    }

#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_add(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif /* RUNNING_UNIT_TESTS */
    real_iter->parent_map = chm;
    real_iter->free_fn = map->m_procs ? map->m_procs->free : NULL;
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

/* Frees the iterator struct chashmap_begin_iter allocated, with the free
 * function the iterator copied at creation. It reads nothing of the map, so
 * it stays valid after the map is destroyed. */
void __chmap_iterator_destroy(cmap_iterator* iter) {
  if (iter) {
    chmap_cmap_iterator* real_iter = cmapIter2ChmapIter(iter);
#ifdef RUNNING_UNIT_TESTS
    __atomic_fetch_sub(&g_chmap_iter_outstanding_for_tests, 1,
                       __ATOMIC_SEQ_CST);
#endif /* RUNNING_UNIT_TESTS */
    ccol_free_t free_fn = real_iter->free_fn;
    if (free_fn) {
      free_fn(real_iter);
    } else {
      _ccol_default_mem_free(real_iter);
    }
  }
}

/* Moves the iterator to the next element: for separate chaining it follows
 * the dllist's next pointer, and for open-addressing it scans the slot array
 * linearly for the next occupied slot that isn't deleted. At the end it
 * destroys the iterator and gives NULL. */
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
    open_addr_map* map = real_iter->parent_map->impl.oa;
    size_t i = real_iter->iter.oa_index + 1;

    // Find the next occupied slot
    while (i < map->capacity && !(map->metadata[i] & SLOT_OCCUPIED)) {
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

/* Destroys the backend map underneath and the map's own m_procs copy, then
 * frees the top-level chashmap struct. When the caller gave a custom
 * allocator, that allocator made chm, so chm is freed with the same free
 * function, which is copied into the free_func local before procs is freed,
 * so the function pointer is still valid once the procs struct itself is
 * gone. */
void __chmap_destroy(chmap chm) {
  if (chm) {
    ccol_memmgmt_procs_t* procs;
    if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
      procs = chm->impl.oa->m_procs;
      oa_destroy(chm->impl.oa);
    } else {
      procs = chm->impl.sc->m_procs;
      sc_destroy(chm->impl.sc);
    }
    if (procs) {
      // The procs struct can be part of the block, so the free function must
      // be read before the block goes.
      ccol_free_t free_func = procs->free;
      free_func(chm);
      return;
    }
    _ccol_default_mem_free(chm);
  }
}

/* See this function's doc comment in chashmap.h. It's the same as
 * __chmap_destroy() above, except that it threads a destructor callback into
 * the same walk below, the walk oa_destroy() and sc_destroy() make quietly
 * for their own backend, and it allocates nothing. The two walks below are
 * deliberately NOT moved into oa_destroy() and sc_destroy(), and the loop
 * here is a duplicate on purpose: this function only exists for a non-NULL
 * val_dtor, and the duplicate keeps its cost off the no-destructor hot path
 * that every other user of the map takes.
 *
 * The destructor gets the entry's accessor through a const cmap_pair*. The
 * accessor describes storage the map is about to free, so a destructor reads
 * it and frees what the value itself owns, while assigning to either field
 * is a compile error. On the separate-chaining side the accessor is the
 * node's live one, so the rule isn't only about this function's stack. */
void chmap_destroy_with_dtor(chmap chm,
                             void (*val_dtor)(const cmap_pair* val_pair,
                                              void* dtor_ctx),
                             void* dtor_ctx) {
  if (!chm) return;
  if (!val_dtor) {
    __chmap_destroy(chm);
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

  __chmap_destroy(chm);
}

#ifdef RUNNING_UNIT_TESTS
/* Gives the current size of the bucket array or the slot array, which
 * white-box unit tests use to check both backends' resize thresholds. It
 * isn't part of the public API. */
size_t chmap_get_bucket_arr_size(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->capacity;
  } else {
    return chm->impl.sc->bucket_arr_size;
  }
}

/* Gives the element count at which the next scale-up starts. */
size_t chmap_get_elem_count_to_scale_up(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    size_t cap = chm->impl.oa->capacity;
    return (cap / 10) * 7 + ((cap % 10) * 7) / 10;
  } else {
    return chm->impl.sc->elem_count_to_scale_up;
  }
}

/* Gives the element count at which the next scale-down starts. */
size_t chmap_get_elem_count_to_scale_down(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->capacity / 4;
  } else {
    return chm->impl.sc->elem_count_to_scale_down;
  }
}

/* The SipHash state of the 128-bit key (k0, k1), the way a map keeps it. */
void chashmap_sip_state_for_tests(uint64_t k0, uint64_t k1, uint64_t sip_v[4]) {
  chmap_sip_state_from_key(k0, k1, sip_v);
}

/* SipHash-1-3 of a buffer from an explicit state, which a test compares with
 * an independent byte-wise implementation of the algorithm. */
uint64_t chashmap_siphash13_for_tests(const uint64_t sip_v[4],
                                      const void* key_ptr, size_t key_size) {
  return siphash13(sip_v, key_ptr, key_size);
}

/* The map's byte hash from an explicit state, which a test uses with the
 * state of a key anybody can know, to build keys that collide for it. */
size_t chashmap_byte_hash_for_tests(const uint64_t sip_v[4],
                                    const void* key_ptr, size_t key_size) {
  return hash_bytes_keyed(sip_v, key_ptr, key_size);
}

/* The secret a map hashes with: the four words of the SipHash state, then
 * int_seed, then byte_seed. An open-addressing map reads neither byte hash,
 * so it gives 0 for the SipHash state and for byte_seed. */
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

/* The four key words the fallback path derives when getrandom(2) can't
 * answer. */
void chashmap_fallback_hash_key_for_tests(uint64_t out[4]) {
  chmap_fallback_hash_key(out);
}

/* XXH64 of a buffer with an explicit seed, which a test compares with the
 * published vectors and with an independent byte-wise implementation. */
uint64_t chashmap_xxh64_for_tests(const void* key_ptr, size_t key_size,
                                  uint64_t seed) {
  return xxh64(key_ptr, key_size, seed);
}

/* Whether the map hashes in the keyed mode. */
bool chashmap_is_keyed_for_tests(chmap chm) {
  return chm && chm->hash_mode == CHMAP_HASH_KEYED;
}

/* Switches the map to the keyed mode through the same function the
 * detection calls. Gives ccol_not_enough_memory, leaving the map fast, when
 * rebuilding an open-addressing table can't allocate. */
ccol_retval_t chashmap_switch_to_keyed_for_tests(chmap chm) {
  if (!chm) return ccol_invalid_args;
  if (chm->hash_mode == CHMAP_HASH_KEYED) return ccol_success;
  if (chm->impl_type == IMPL_SEPARATE_CHAINING) {
    sc_switch_to_keyed(chm->impl.sc);
    return ccol_success;
  }
  open_addr_map* map = chm->impl.oa;
  return oa_rebuild(map, map->capacity, true);
}

/* The probe_span of an open-addressing map, or 0 for the other backend. */
size_t chashmap_oa_probe_span_for_tests(chmap chm) {
  if (!chm || chm->impl_type != IMPL_OPEN_ADDRESSING) return 0;
  return chm->impl.oa->probe_span;
}

/* The cap on a single insert into a fast open-addressing table of capacity
 * slots. */
size_t chashmap_oa_insert_cap_for_tests(size_t capacity) {
  return oa_insert_cap(capacity);
}

/* The window bound of an open-addressing table of capacity slots whose
 * window started at start_count entries. */
uint32_t chashmap_oa_window_bound_for_tests(size_t start_count,
                                            size_t capacity) {
  return oa_window_bound(start_count, capacity);
}

/* The window bound of a separate-chaining map with bucket_count buckets and
 * that size's scale-up threshold, whose window started at start_count
 * entries. */
uint32_t chashmap_sc_window_bound_for_tests(size_t start_count,
                                            size_t bucket_count) {
  sep_chain_map probe;
  memset(&probe, 0, sizeof(probe));
  probe.bucket_arr_size = bucket_count;
  sc_set_scaling_limits(&probe);
  return sc_window_bound(start_count, probe.elem_count_to_scale_up,
                         bucket_count);
}

/* The bound of a map's current insert window. */
uint32_t chashmap_window_bound_of_for_tests(chmap chm) {
  return chm ? chm->window_bound : 0;
}

/* The running sum of a map's insert window. */
uint32_t chashmap_insert_window_for_tests(chmap chm) {
  return chm ? chm->insert_window : 0;
}

/* The length of a separate-chaining map's longest bucket chain, which is
 * what the worst lookup costs, or 0 for a map of the other backend. */
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

/* The hash a separate-chaining map stored for the entry whose key bytes
 * equal key_ptr, or 0 when there's none. */
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

/* A key's home slot in an open-addressing map, or SIZE_MAX for a map of the
 * other backend. A test uses it to pick keys that form a cluster at a chosen
 * place, such as one that wraps past the last slot. */
size_t chashmap_oa_home_slot_for_tests(chmap chm, const void* key_ptr,
                                       size_t key_size) {
  if (!chm || chm->impl_type != IMPL_OPEN_ADDRESSING) return SIZE_MAX;
  open_addr_map* map = chm->impl.oa;
  return hash_index_for(
      hash_key_data(key_ptr, key_size, map->key_type, map->custom_hashing_proc,
                    oa_keyed(map), g_chmap_hash_secret.int_seed),
      map->capacity);
}

/* Checks an open-addressing map's structure. Every occupied slot must be
 * reachable from its home slot through occupied slots only, and there have
 * to be exactly count occupied slots. Gives true for a map of the other
 * backend. */
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
    // Every key lies within probe_span slots of its home slot.
    if (((j - home) & mask) >= map->probe_span) return false;
  }
  return occupied == map->count && map->probe_span <= map->capacity &&
         map->probe_span >= oa_min_probe_span(map->capacity);
}
#endif
