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
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const size_t minimum_allowed_bucket_array_size = 16;
// The first bucket array of a map from ccol_chmap_create_compact(). It is a
// power of two of at least 2; see hash_index_for.
static const size_t compact_bucket_array_size = 4;
static const size_t scale_factor = 4;

#define OPEN_ADDR_MAX_LOAD_FACTOR 0.70
#define OPEN_ADDR_MIN_LOAD_FACTOR 0.25
#define INLINE_STORAGE_THRESHOLD 23  // SSO: Small String Optimization threshold

// The integer mixer and the reduction of a hash to an index each have one
// form for a 64-bit size_t and one for a 32-bit size_t.
#if SIZE_MAX != UINT64_MAX && SIZE_MAX != UINT32_MAX
#error "Unsupported architecture: SIZE_MAX is neither UINT32_MAX nor UINT64_MAX"
#endif

/* The secret of the built-in hashes. There is one for the process, chosen
 * before the first map exists and never changed afterwards; see
 * chmap_process_hash_secret. Every map and ccol_chmap_hash_key read that one
 * copy, so a map stores none. It has three independent parts.
 *
 * sip_v is the initial state of SipHash-1-3 for the 128-bit key (k0, k1):
 * k0 ^ "somepseu", k1 ^ "dorandom", k0 ^ "lygenera", k1 ^ "tedbytes". The map
 * keeps that state instead of the key, so a hash does not redo those four
 * exclusive ors. The keyed byte hash of a key with no fixed width uses it,
 * and so does the hash of a long double in either mode.
 *
 * int_seed keys the mixer of every other fixed-width key and of the result of
 * a custom hashing proc in the keyed mode; see hash_word_seeded.
 *
 * byte_seed is the seed of XXH64, the byte hash of the fast mode; see
 * hash_bytes_fast. */
typedef struct chmap_hash_secret {
  uint64_t sip_v[4];
  uint64_t int_seed;
  uint64_t byte_seed;
} chmap_hash_secret;

/* ========================================================================== */
/*                    OPEN ADDRESSING STRUCTURES                              */
/* ========================================================================== */

// A 16-byte slot: an 8-byte key and an 8-byte value. A parallel byte array
// holds the status bits. They are not in this struct. See the metadata field
// of open_addr_map, and the note that comes next, for the reason. The natural
// alignment keeps key_data and val_data on 8-byte boundaries for every array
// index. A strict-alignment architecture needs this for correctness. It also
// lets chmap_get_ptr give back an aligned pointer straight into the slot
// array, so the caller can change the value in place.
/* Exactly two words. There is no padding, and there is nothing that a probe
 * does not read. The occupied bit lives in a separate byte array. See the
 * metadata field of open_addr_map. It is not in this struct for a reason. A
 * third member of one byte pads this struct out to 24 bytes. A third of every
 * cache line that a probe pulls in would then carry nothing. The bits that a
 * probe tests first would also be one for each 24 bytes instead of packed
 * together. */
typedef struct {
  uint64_t key_data;
  uint64_t val_data;
} oa_slot;

#define SLOT_OCCUPIED 0x01

typedef struct {
  oa_slot* slots;
  // One byte for each slot, with the same indices as slots[]. Each byte holds
  // the occupied bit of that slot. This array is the tail of the slots
  // allocation. It is not an allocation of its own; see
  // oa_alloc_block. The arrays of the block can therefore never disagree
  // about their length, and a resize frees one block instead of several.
  //
  // It is separate from oa_slot so that a probe reads its bits densely. One
  // 64-byte line covers the bits of 64 slots. A byte inside each slot would
  // need sixteen lines to test the same number of slots. All but one byte of
  // each of those lines would be key bytes and value bytes. The probe needs
  // those bytes only for the one slot where it stops.
  uint8_t* metadata;
  // A parallel array with one entry for each slot. Its indices always match
  // slots[]. Each entry holds a stable {ptr, size} accessor for the value of
  // that slot. This array is separate from oa_slot and not inside it. The hot
  // insert loop, probe loop and delete loop touch only key_data, val_data and
  // metadata. They therefore keep the scan over the compact 16-byte oa_slot
  // array. They do not drag 16 more bytes for each slot through the cache on
  // every probe. Only the final lookup of chmap_get_elem_ref reads this array,
  // and that path is comparatively cold. The ptr of val_accessors[index] is
  // always &slots[index].val_data. Its size is always the val_size of the map.
  // The array therefore holds nothing that the slot it describes cannot give
  // again.
  //
  // The library writes each entry wherever it writes its slot, which is
  // oa_insert and oa_rehash. The backward shift of oa_delete moves an entry
  // only into a slot that held one a moment ago, whose accessor is already
  // correct; see oa_backward_shift_walk. oa_get only takes the address of one
  // entry. The write belongs on the paths that write a slot, not on the path
  // that hands an entry out. A write where the entry is read spares the insert
  // path one write into a second array the size of the table. It charges every
  // lookup that write instead. This costs more than it saves wherever lookups
  // are more frequent than inserts. Exactly one path must write the entry,
  // whichever path that is. An entry that the map hands out before anything
  // fills it in is {NULL, 0}. A caller that asks for an element reference then
  // gets a null pointer for every key.
  //
  // The array sits between slots[] and metadata[] in the block of the slots;
  // see oa_alloc_block.
  //
  // The array gives one entry for each slot. Two chmap_get_elem_ref results
  // that the caller holds at the same time for different keys therefore never
  // alias one another. A single shared accessor field would make them alias.
  // Each result stays valid until the library rewrites the slot itself, or
  // until the table rehashes.
  cmap_pair* val_accessors;
  size_t capacity;
  // One more than the largest displacement of any key that the table holds,
  // and never less than 32 or the capacity, whichever is smaller. Every key
  // therefore sits within probe_span slots of its home slot, and a lookup, a
  // delete and the backward-shift walk stop after that many slots. That
  // bounds a probe that would otherwise walk to the end of a long run of
  // keys whose home slots are consecutive: each such key sits in its own
  // home slot, so no insert sees a long displacement, while the run itself
  // can be as long as the table. An insert widens it (see
  // oa_note_deep_insert) and a rebuild sets it from the table it builds; a
  // delete only moves keys toward their home slots, so it never needs to
  // widen it.
  size_t probe_span;
  size_t count;
  // The fields above, the hash mode and the insert window in the header of
  // the map, and the two below, are what an insert, a lookup and a delete
  // read. The fields after them are read on colder paths only.
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

// This struct is not packed. A packed struct gives no size benefit here,
// because two pointer fields of the same size need no internal padding in
// either case. A packed struct could also sit at an offset inside llist_node
// that is not aligned to 8 bytes, and that brings no gain.
typedef struct dllist_ref_node {
  struct dllist_ref_node* prev;
  struct dllist_ref_node* next;
} dllist_ref_node;

// The two ends of the list of every live entry. head is the newest entry and
// tail the oldest one. The public iterator walks from head through next, so
// it visits the newest entry first. The internal ordered cursor of
// chashinsert.h walks from tail through prev, so it visits the entries in
// insertion order.
typedef struct dllist_root {
  dllist_ref_node* head;
  dllist_ref_node* tail;
} dllist_root;

// One entry of a separate-chaining map.
//
// key_storage and val_storage hold a key or a value of up to
// INLINE_STORAGE_THRESHOLD bytes inline (SSO), and a pointer to a heap buffer
// for anything larger. Which one a union holds follows from the size in its
// accessor: a key or a value is inline exactly when its size is at most
// INLINE_STORAGE_THRESHOLD. Every path that stores a value keeps that true;
// see sc_create_llist_node and sc_reset_val_of_llist_node.
//
// Each union carries an explicit _Alignas(max_align_t), and the struct is
// deliberately NOT packed. inline_data must land at an offset that is
// correctly aligned for ANY type that a caller can store there. chmap_get,
// chmap_get_ptr and the iterator accessor macros cast a pointer into this
// storage directly to the value type of the caller, and then dereference it.
// They do this for a change in place too. A read through memcpy is not a safe
// substitute for that.
//
// max_align_t is the correct bound here, and 8 alone is not enough. A plain
// malloc() gives the same guarantee to the heap buffer that holds anything
// larger than INLINE_STORAGE_THRESHOLD, and the inline storage is a drop-in
// substitute for that buffer, so it must meet the same alignment contract for
// whatever type-erased bytes a caller stores there. `long double` needs 16
// bytes on x86-64, and it is one of the built-in value types that always goes
// inline through this path. UndefinedBehaviorSanitizer reports a storage that
// is less aligned directly on chmap_get_ptr of a `char* -> long double` map
// ("load of misaligned address ... which requires 16 byte alignment"). It is
// the same root cause as the packed-struct hazard that cjson.c, cyaml.c,
// clrucache.c, cthreadcomm.c and chttpclient.c document, arriving through a
// union that under-declares its own alignment requirement.
//
// The order of the members fills every byte on LP64. Each union is 24 bytes
// at a multiple of 16, and the 8-byte member after each one fills the rest of
// its 32 bytes, so the struct is 112 bytes with no padding. It also puts the
// two fields that a chain walk reads at every node, hash_val and next, in the
// first 64 bytes, beside the bytes of an inline key. The accessors follow,
// and the links of the insertion-order list come last, because only an
// insert, a delete and an iteration read them.
//
// The node keeps no allocator of its own. Every path that frees or resizes a
// buffer of a node gets the procs of its map as an argument.
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
  // The stable {ptr, size} of the stored key and of the stored value, which
  // the map hands out. ptr addresses the inline_data of the union when the
  // size is at most INLINE_STORAGE_THRESHOLD, and the heap buffer otherwise.
  cmap_pair key_pair_accessor;
  cmap_pair val_pair_accessor;
  dllist_ref_node dllist_refs;
} llist_node;

// The layout above on LP64 with a 16-byte max_align_t, which x86-64 and
// aarch64 have. A member that is added or moved makes this fail to compile
// instead of quietly adding padding to every entry.
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
  // A ccol_data_type each. They are one byte wide so that the three bytes
  // share the last word of the struct.
  uint8_t key_type;
  uint8_t val_type;
  // The number of bucket pointers that the block of the map holds right
  // after struct chashmap, or 0 when it holds none; see
  // sc_inline_bucket_arr. It is never more than
  // minimum_allowed_bucket_array_size.
  uint8_t inline_buckets;
} sep_chain_map;

/* ========================================================================== */
/*                      UNIFIED STRUCTURE                                     */
/* ========================================================================== */

typedef enum { IMPL_OPEN_ADDRESSING, IMPL_SEPARATE_CHAINING } hashmap_impl_type;

/* A map is one allocation. The block holds this struct, with the state of
 * its backend in state, then the bucket array that a separate-chaining map
 * starts with when that array is small (see sc_inline_bucket_arr), then the
 * map's own copy of the allocator procs when it needs one (see
 * chmap_create_impl).
 *
 * impl points at state. An operation reaches the backend through that
 * pointer, exactly as it would reach a backend in an allocation of its own.
 * Addressing state at its fixed offset instead saves the load and 8 bytes,
 * and it costs the insert and the delete of an open-addressing map two
 * instructions each, because the compiler then allocates the registers of
 * those paths differently.
 *
 * The first eight bytes hold the backend, the hash mode and the insert
 * window. Both backends reach the mode and the window at a constant offset
 * back from their state; see chmap_of_oa and chmap_of_sc. */
struct chashmap {
  // A hashmap_impl_type.
  uint8_t impl_type;
  // CHMAP_HASH_FAST or CHMAP_HASH_KEYED; see the section on adaptive
  // hashing.
  uint8_t hash_mode;
  // The bound of the current insert window, which the start of the window
  // computes; see oa_window_bound and sc_window_bound. No bound exceeds
  // 16 bits.
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

/* Every map starts in the fast mode and can switch to the keyed mode; each
 * growth of a keyed map tries the fast mode again (see below).
 * The keyed mode is the hash that ccol_chmap_hash_key gives: SipHash-1-3 for
 * a byte key and the seeded mixer for a fixed-width key and for the result of
 * a custom hashing proc. The fast mode is a Fibonacci multiply for a
 * fixed-width key, XXH64 with a secret seed for a byte key, and the murmur3
 * finalizer for the result of a custom hashing proc. A long double key is
 * hashed by value with SipHash-1-3 in both modes. Key identity does not
 * depend on the mode: -0.0 and 0.0 are one key, and so are two long doubles
 * of one value, in either.
 *
 * The fast hashes spread every ordinary key set as well as the keyed ones do,
 * or better, and cost less, but a party that controls the keys can choose a
 * set that collides under them: the Fibonacci multiply and the finalizer are
 * public bijections, and XXH64 has collisions that hold for every seed. Only
 * the writers of a map watch for that, so a lookup never writes:
 *
 * - The insert window. Every insert of a new key adds 2^23 plus its walk (the
 *   displacement of an open-addressing slot, or the nodes of the chain that
 *   the new key met) to the 32-bit insert_window, which starts at 2^31. The
 *   carry out of that add marks the end of a window of 256 inserts, and the
 *   window then holds the sum of the walks; see chmap_window_add. A fast map
 *   switches when that sum is above the bound of oa_window_bound or
 *   sc_window_bound, which is several times what a random function costs at
 *   the highest load that the window can have reached. A rebuild, a reset and
 *   the switch restart the window. A keyed map keeps adding to the window
 *   and is never judged, which spares the insert path a test of the mode.
 * - A cap on one insert. An open-addressing insert whose displacement is
 *   above oa_insert_cap, and a separate-chaining insert that meets
 *   SC_CHAIN_CAP nodes, switch at once. A rebuild of an open-addressing
 *   table, and a shrink of a separate-chaining one, measure the same things
 *   on the table that they build.
 *
 * The switch rebuilds an open-addressing table into a new block with the
 * keyed hash, and rehashes the nodes of a separate-chaining map in place.
 *
 * A growth of a keyed map makes a fast attempt: it fills the new table with
 * the fast hash and keeps the fast mode unless the fill shows that the keys
 * collide under it; see oa_place_all_fast_attempt and
 * sc_relink_fast_attempt. A failed attempt stops early and the growth fills
 * the same new table with the keyed hash, so it allocates nothing more. Its
 * work is linear in the count: for a map of n entries at most
 * 2 * n + cap + 65 slots, where cap is oa_insert_cap of the new table, or
 * n hashes and n + SC_ATTEMPT_SLACK + SC_CHAIN_CAP - 1 chain nodes. Growth is
 * geometric, so even a map whose every growth fails pays O(1) amortized for
 * each insert: in an open-addressing table of 4096 slots or more, a growth
 * at count 0.7 * c follows 0.35 * c inserts, so at most about four slots and
 * two keyed placements for each insert; a separate-chaining growth at
 * count 1.5 * b follows 1.125 * b inserts, so at most about three node
 * operations and 1.3 keyed hashes for each insert. A map that the attempt
 * returns to the fast mode restarts its window like any rebuild, so an
 * attack that resumes switches it again within two windows. A reset and a
 * shrink keep the mode: a shrink follows deletes, which say nothing new about
 * how the keys spread, and the next growth of the map makes the attempt. */
enum { CHMAP_HASH_FAST = 0, CHMAP_HASH_KEYED = 1 };

/* The insert window starts at 2^31 and each insert of a new key adds 2^23
 * plus its walk, so 256 inserts carry out of 32 bits exactly when their walks
 * sum to less than 2^23, and the 32 bits then hold that sum. A fast map never
 * holds a walk above its cap, so its windows always end after 256 inserts. */
#define CHMAP_WINDOW_START 0x80000000u
#define CHMAP_WINDOW_STEP 0x00800000u
#define CHMAP_WINDOW_INSERTS 256u

/* A new separate-chaining key that meets this many nodes switches the map. */
#define SC_CHAIN_CAP 20u

/* An open-addressing insert with a displacement of at least this many slots
 * takes oa_note_deep_insert, which widens probe_span and checks the cap. It
 * is also the smallest probe_span of a table of at least this many slots. */
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
 * true when that ends the window. It is a lea, an add to memory and a jump on
 * the carry. */
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

/* Gives true for every ccol_data_type value that fits in 8 bytes or less and
 * that the integer mixer hashes. These are the integers, the floats and the
 * pointers. The library uses this function at creation time to select the map
 * backend. */
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

/* Gives the byte size of the C type that belongs to a ccol_data_type enum
 * value. For a type that this function does not know, it gives 8. An
 * open-addressing slot is then always large enough for a value of the size of
 * a pointer. */
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

/* Gives true when the key type and the value type are both integral and both
 * 8 bytes or less. The library selects the compact open-addressing backend on
 * this condition. In every other case it selects the separate-chaining
 * backend. That backend holds a key and a value of any size, which includes a
 * string and a struct that the caller defines. */
static inline bool should_use_open_addressing(ccol_data_type key_type,
                                              ccol_data_type val_type) {
  return is_type_integral(key_type) && is_type_integral(val_type) &&
         get_type_size(key_type) <= 8 && get_type_size(val_type) <= 8;
}

/* ========================================================================== */
/*                         HASH FUNCTIONS                                     */
/* ========================================================================== */

/* The mixer of every fixed-width key and of the result of a custom hashing
 * proc: an exclusive or with the secret int_seed, a multiply, a xorshift and
 * a second multiply. hash_index_for() reads the index from the top bits of
 * the result, which are the bits that the last multiply mixes best, so the
 * final xorshift of a general-purpose finalizer such as murmur3 fmix64 would
 * add work that no reduction reads.
 *
 * Each step is a bijection, so two keys of one width never share the whole
 * hash. Every key bit reaches the top bits through both multiplies: the first
 * multiply carries each bit upward, the xorshift brings the upper half of the
 * product down, and the second multiply carries the whole word upward again.
 *
 * The seed is what makes the reduction impossible to steer. Without it, a
 * party that controls the keys, such as an identifier that a peer sends, can
 * invert any public mixer and compute offline a set of keys whose hashes share
 * their top bits: they all land in one home slot or one bucket, and n inserts
 * cost O(n^2) probes. A single multiply by a public constant keeps a
 * structure that no seed removes. With k * C, the keys i * C^-1 all hash to i
 * and share slot 0, and for the Fibonacci constant the multiples of a large
 * Fibonacci number pile up in a few clusters. With (k ^ s) * C, a set of keys
 * that varies only inside a fixed group of bit positions, all combinations
 * included, is mapped by every seed onto a translate of one and the same set,
 * so the clustering that the party measures offline for one seed holds for
 * every seed. A secret multiplier has no such set, but the spread of a run of
 * sequential keys then depends on the multiplier drawn: over 300 random
 * multipliers such a run costs between 1.0 and 27.7 probes for each lookup
 * at a load of 0.38. In this mixer, the first product of such a set is an
 * additive translate by a secret amount, and the xorshift that follows turns
 * the carries of that addition into a nonlinear function of the seed before
 * the second multiply spreads it, so the top bits that a set of keys shares
 * depend on the seed.
 *
 * The price is regularity. A bare Fibonacci multiply spreads a run of
 * sequential keys perfectly, one probe for each lookup. This mixer spreads
 * every key set, sequential runs included, as a random function would, which
 * at a load of 0.38 is about 1.3 probes for each lookup, and about 2 at the
 * maximum load of 0.70.
 *
 * A 32-bit size_t uses a 32-bit form of the same steps, with the constants of
 * the lowbias32 finalizer, for every key of 32 bits or less. */
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
 * double. On a 64-bit target that is hash_word_seeded itself. On a 32-bit
 * target a size_t cannot hold the key, so the 64-bit steps run at the width
 * of the key and the upper word of the result is the hash, because
 * hash_index_for reads the index from the top. Folding the two halves of the
 * key together first (low ^ high) would give every key with the same
 * exclusive or of its halves the same hash, and a key packed from two 32-bit
 * fields, such as (a << 32) | b, is the ordinary shape of a 64-bit key. */
static inline size_t hash_u64_seeded(uint64_t x, uint64_t seed) {
#if SIZE_MAX == UINT64_MAX
  return hash_word_seeded((size_t)x, seed);
#else
  uint64_t r = (x ^ seed) * 0xBF58476D1CE4E5B9ULL;
  r ^= r >> 32;
  return (size_t)((r * 0x94D049BB133111EBULL) >> 32);
#endif
}

/* The fast hash of a fixed-width key of at most the width of size_t: a
 * Fibonacci multiply, read from its top bits by hash_index_for. The multiply
 * is a bijection, so the top bits of the product are a permutation of a run
 * of sequential keys, and such a run costs one probe for each lookup. Keys
 * scaled by a power of two and aligned addresses spread the same way. The
 * multiplier is public and nothing keys it, so a party that controls the keys
 * can make any number of them share a home slot; the insert window and the
 * insert cap see that and switch the map to hash_word_seeded. */
static inline size_t hash_word_fast(size_t x) {
#if SIZE_MAX == UINT64_MAX
  return x * (size_t)0x9E3779B97F4A7C15ULL;
#else
  return x * (size_t)0x9E3779B9U;
#endif
}

/* The fast hash of a key of exactly 64 bits. On a 32-bit target the multiply
 * runs at the width of the key and the upper word of the product is the
 * hash, because hash_index_for reads the index from the top. */
static inline size_t hash_u64_fast(uint64_t x) {
#if SIZE_MAX == UINT64_MAX
  return hash_word_fast((size_t)x);
#else
  return (size_t)((x * 0x9E3779B97F4A7C15ULL) >> 32);
#endif
}

/* The fast finalizer of the result of a custom hashing proc: the murmur3
 * finalizer, which makes every output bit depend on every input bit. A custom
 * hash may put its entropy anywhere, for example in the low bits of an
 * identity or a counter, and hash_index_for reads the top bits. The finalizer
 * is public and unseeded; see hash_word_fast for what that leaves to the
 * detection. */
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
 * of its 128-bit key. A party that does not know the key cannot compute two
 * inputs whose hashes collide, in full or in their top bits, any better than
 * by chance, and no pair of inputs collides for every key. A byte hash that is
 * only seeded, and not keyed in this sense, can have pairs of inputs that
 * collide whatever the seed is, and those pairs are enough to put any number
 * of keys into one bucket. The variant with one compression round and three
 * finalization rounds is the one that the hash tables of Rust and Python use
 * for the same purpose. The input is read as little-endian 64-bit words,
 * which is the definition of the algorithm, through memcpy, so the address of
 * a key needs no alignment. */
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

/* The last 0 to 7 bytes as a little-endian word. Four to seven bytes take two
 * 4-byte reads that overlap; one to three bytes take the first, the middle
 * and the last byte. The bytes that two reads share are equal, so the or of
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

/* SipHash-1-3 of a buffer, as a size_t. A 32-bit size_t keeps both halves of
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

/* XXH64, the byte hash of the fast mode, exactly as its specification
 * defines it, with the input read as little-endian words through memcpy. It
 * is seeded with a secret, so a party that does not know the seed cannot
 * compute keys that collide under it by trying keys offline. XXH64 is not a
 * pseudorandom function of its seed, though: some sets of keys collide under
 * every seed. A round over one lane, acc = rotl(acc + w * P2, 31) * P1,
 * turns a change of 2^63 in acc + w * P2 into a change of plus or minus 2^30
 * after the rotation, and the next word of the same lane can cancel either
 * sign. The insert window and the chain cap see such a set and switch the map
 * to SipHash-1-3. */
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

/* Reduces a hash to an index into a table whose size is a power of two. It
 * takes the HIGH bits of the hash. Every reduction in this file goes through
 * here, so the code makes this choice one time only. Every hash of this file
 * mixes its top bits best: the multiplies of hash_word_fast and of
 * hash_word_seeded carry each bit upward, and SipHash and XXH64 mix every
 * bit. A custom hash reaches this function only through hash_custom_fast or
 * hash_word_seeded.
 *
 * capacity is always a power of two. It is also never below
 * compact_bucket_array_size, which is at least 2. The shift is therefore at
 * most one less than the width of the type. It can never be a shift of the
 * full width, which would be undefined.
 *
 * Read that precondition before you add a new reduction site. A capacity of 1
 * makes the shift as wide as the type. A capacity of 0 reaches __builtin_clz
 * with zero. Both of those are undefined. */
static inline size_t hash_index_for(size_t hash_val, size_t capacity) {
  /* The shift is the leading-zero count plus one. A log2(capacity) kept as an
     intermediate value would mean that the code writes the width of the type
     two times: one time to derive the width and one time to subtract it. Those
     two must then agree with each other AND with the operand width of the
     builtin. The count itself removes both of those restatements, so only the
     builtin has to match size_t. An error here makes an unsigned intermediate
     value underflow into a shift that is wider than the type. Such a shift is
     undefined, and compilers fold it to a constant zero. Every key then goes
     into slot 0, while every answer stays correct. */
#if SIZE_MAX == UINT64_MAX
  return hash_val >>
         ((unsigned)__builtin_clzll((unsigned long long)capacity) + 1u);
#else
  return hash_val >> ((unsigned)__builtin_clz((unsigned int)capacity) + 1u);
#endif
}

/* Hashes a long double key by its numeric VALUE and not by its raw bytes. A
 * float is exactly 4 bytes and a double is exactly 8 bytes, and neither has
 * padding. The representation of a long double in memory is different on most
 * platforms. One example is 80-bit x87 extended precision stored in a 16-byte
 * slot. That representation contains padding bits, and the C standard leaves
 * them completely unspecified. Two variables that hold exactly the same
 * mathematical value can differ in those padding bits. The difference depends
 * on how the program computed and stored each one. The padding of a long
 * double with automatic storage duration is not reliably the same across two
 * separate constructions of the same value. This stays true after an explicit
 * memset, once the compiler treats the whole-object assignment as something
 * that makes that memset dead. A raw byte hash would put two long double keys
 * with identical numbers into different buckets. It would then silently
 * defeat every lookup that does not reuse the exact original bytes. frexpl
 * takes the VALUE apart and avoids this. frexpl works on the loaded
 * floating-point value and never on the bytes below it. The mantissa and the
 * exponent that it gives are a deterministic function of the number itself.
 * They do not depend on the padding bits that came with it.
 *
 * The function hashes the pair {scaled mantissa, exponent} as 16 bytes with
 * SipHash-1-3. Both parts reach the hash through the keyed function, so no
 * pair of long double values collides for every key. The scaled mantissa of a
 * finite nonzero value is never 0, and each of the three special classes
 * below is the pair {0, its own class number}, so a special class never
 * shares its pair with a number.
 *
 * Three values get their own handling and never reach frexpl:
 * - NaN: frexpl and a relational comparison have no useful meaning for NaN.
 *   The full representation of a float and of a double is always meaningful,
 *   so a raw-byte comparison of a NaN payload is well defined for them. The
 *   padding bytes beside a NaN long double are different. In practice they
 *   are often genuinely uninitialized memory, and not merely content that is
 *   unspecified but stable. A plain `long double n = NAN;` writes only the
 *   significant NaN bits and never the padding. A read of those padding bytes
 *   is therefore a real use of uninitialized memory. This is true even for a
 *   read that only hashes them or gives them to memcmp. valgrind and
 *   MemorySanitizer both report it. Every NaN long double therefore hashes as
 *   one fixed class, and this touches no padding byte at all. This
 *   necessarily means that every NaN long double collapses into a single
 *   key. See the matching NaN branch of long_double_keys_equal below. A float
 *   and a double keep their own policy of a distinct NaN payload. This is a
 *   deliberate and narrower difference that the padding of a long double
 *   forces, and it is not an oversight. This codebase has precedent for it.
 *   The long double comparator of cbstmap collapses every NaN into one
 *   equivalence class for the identical reason. The total-order requirement
 *   of a BST leaves it no other sound choice, and it also never reads a
 *   padding byte to do this.
 * - +-0.0: the function unifies these into one class up front. It does not
 *   trust this to come out of frexpl. The C standard only promises that frexp
 *   returns "zero" for a zero input. It does not guarantee that every libm
 *   implementation keeps the sign of that zero in the same way.
 * - +-Infinity: the exponent output of frexpl is unspecified for an infinite
 *   input. A conversion of an infinite floating value to an integer type is
 *   undefined behavior. Each sign of infinity is therefore a class of its
 *   own, with no call to frexpl and no cast of the value to an integer. */
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
    // |mantissa| is in [0.5, 1). The scaled value below therefore always fits
    // safely in an int64_t. Its magnitude is in [2^61, 2^62), so it is never
    // 0. The scale puts the significant bits at the TOP of the 64-bit value,
    // and all 64 bits are hashed.
    int64_t scaled_mantissa = (int64_t)(mantissa * 4611686018427387904.0L);
    parts[0] = (uint64_t)scaled_mantissa;
    parts[1] = (uint64_t)(int64_t)exp;
  }
  return hash_bytes_keyed(sip_v, parts, sizeof(parts));
}

/* The secret of the process; see chmap_hash_secret. It is chosen once, on
 * the first call of chmap_process_hash_secret, which every map creation and
 * every ccol_chmap_hash_key call makes before it hashes anything. It is never
 * chosen in a constructor. A map that the constructor of another object
 * creates before any constructor of this library runs therefore still hashes
 * with the final secret, in a shared build and in a static one. The value
 * never changes once chosen, so every map and ccol_chmap_hash_key agree on
 * it.
 *
 * The operations of a map read this object directly and keep no copy. Its
 * address is a constant of the library, so a read costs what a read from the
 * map struct costs. A thread that uses a map without having created it reads
 * the chosen value too: the creation of the map called
 * chmap_process_hash_secret first, and whatever handed the map to that thread
 * orders the creation before the use. */
static chmap_hash_secret g_chmap_hash_secret;
static ccol_once_flag_t g_chmap_hash_secret_once = CCOL_ONCE_INIT;

/* One step of the splitmix64 finalizer. It spreads every input bit over
 * every output bit, so it can mix entropy sources of uneven quality. */
static uint64_t chmap_seed_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

/* The key words when the kernel cannot give random bytes without blocking: a
 * kernel with no getrandom(2), a seccomp policy that refuses it, or an
 * entropy pool that is not initialized yet early in boot. It mixes both
 * clocks, the process id and three addresses that address space layout
 * randomization places, and derives the four words from that mix. It is
 * weaker than the kernel generator and far stronger than a constant, and it
 * never fails. */
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

/* Gives the secret of the process, choosing it first when no call has chosen
 * it yet. It is on the creation path of a map and on ccol_chmap_hash_key,
 * never on the path of an insert or a lookup of a map. */
static const chmap_hash_secret* chmap_process_hash_secret(void) {
  ccol_call_once(g_chmap_hash_secret_once, chmap_init_process_hash_secret);
  return &g_chmap_hash_secret;
}

/* The byte hash of the fast mode, XXH64 seeded with byte_seed, as a size_t.
 * A 32-bit size_t keeps both halves of the output. */
static inline size_t hash_bytes_fast(const void* input, size_t len) {
  uint64_t h = xxh64(input, len, g_chmap_hash_secret.byte_seed);
#if SIZE_MAX == UINT64_MAX
  return (size_t)h;
#else
  return (size_t)(h ^ (h >> 32));
#endif
}

/* The byte hash of the keyed mode. It is out of line, so that the fast byte
 * hash stays inline in the functions that hash both ways. */
static __attribute__((noinline)) size_t hash_bytes_keyed_ool(const void* input,
                                                             size_t len) {
  return hash_bytes_keyed(g_chmap_hash_secret.sip_v, input, len);
}

/* The hash of a fixed-width key of at most the width of size_t, in the mode
 * that keyed names. */
static inline __attribute__((always_inline)) size_t
hash_word_mode(size_t x, bool keyed, uint64_t int_seed) {
  return keyed ? hash_word_seeded(x, int_seed) : hash_word_fast(x);
}

/* The hash of a key of exactly 64 bits, in the mode that keyed names. */
static inline __attribute__((always_inline)) size_t
hash_u64_mode(uint64_t x, bool keyed, uint64_t int_seed) {
  return keyed ? hash_u64_seeded(x, int_seed) : hash_u64_fast(x);
}

/* Sends the key to the correct hash function for its key type and for the
 * mode that keyed names; see the section on adaptive hashing. Every integral
 * type, float, double and a pointer go through hash_word_mode or
 * hash_u64_mode. A long double goes through hash_long_double_value in both
 * modes, and every other type through hash_bytes_fast or
 * hash_bytes_keyed_ool. That covers a string and a struct. The result of a
 * custom hashing proc goes through hash_custom_fast or hash_word_seeded.
 *
 * Only the separate-chaining backend reaches the long double and byte-hash
 * cases. The open-addressing backend holds only the fixed-width types of the
 * first group.
 *
 * A custom proc hashes with its own function, which the map finalizes. That
 * keeps a custom hash whose good bits are at the low end, such as an
 * identity or a counter, from putting every key in one slot. Keys whose
 * custom hashes are equal still collide in both modes, because no finalizer
 * can tell them apart.
 *
 * int_seed is the int_seed of the secret. The callers pass it, so that a
 * loop that writes through a byte pointer, which may alias the secret, keeps
 * it in a register. */
static inline __attribute__((always_inline)) size_t
hash_key_data(const void* key_ptr, size_t key_size, ccol_data_type key_type,
              ccol_hashing_proc_t custom_proc, bool keyed, uint64_t int_seed) {
  if (custom_proc) {
    size_t h = (size_t)custom_proc(key_ptr, key_size);
    return keyed ? hash_word_seeded(h, int_seed) : hash_custom_fast(h);
  }

  // The sizes here are compile-time constants, and the compiler turns each
  // one into a single unaligned load. Every multi-byte read below goes
  // through memcpy. None of them dereferences a cast pointer directly.
  // key_ptr can come straight from a cmap_pair that the caller built. That is
  // the raw function layer of chmap_insert_elem, chmap_get_elem_ref and
  // chmap_delete_elem. Such a pointer carries no alignment guarantee. The
  // address of the local variable of a type-inferred macro does carry one. A
  // direct read in the style of `*(uint32_t*)key_ptr` from a misaligned
  // pointer is undefined behavior. It can also fault on a strict-alignment
  // architecture. See the _Alignas(max_align_t) comment on llist_node in
  // this file for the same class of hazard elsewhere in this module.
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

/* Gives true when the byte range [a, a+a_size) and the byte range
 * [b, b+b_size) overlap. Both backends use this function. It finds the case
 * where a value pointer from the caller aliases the exact storage that an
 * insert or an update is about to change. The two users are the existing-key
 * update path of oa_insert, below in this section, and
 * sc_reset_val_of_llist_node, in the separate-chaining section further
 * down. */
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
/* A white-box regression guard for the quality of the hash. It counts the
 * slots that open-addressing probes examine. That count is the real cost of a
 * hash that does not spread the keys. The map stays correct even with very
 * bad clusters, so a correctness test cannot see the difference. A timing test
 * would measure the machine instead. Take a key set whose low bits are
 * constant, such as aligned addresses or identifiers scaled by a power of
 * two. That set drives this count quadratic when the index reads bits that
 * the hash did not mix. It leaves the count near one probe for each operation
 * when the index reads mixed bits. */
/* The counter is atomic, with relaxed ordering. It sits in the probe loop of
 * a map that many threads can read at the same time. A plain increment here
 * is therefore a data race. ThreadSanitizer reports such a race against every
 * caller that shares a map. This instrumentation would then be a source of
 * findings instead of a guard against them. A count needs no more than
 * relaxed ordering. The accessor also reads the counter only after the test
 * joins the threads under test. */
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
 * cannot show. The counter is relaxed and atomic for the reason that the
 * probe counter above gives. */
static _Atomic unsigned long long g_oa_rehashes_for_tests = 0;
unsigned long long chashmap_oa_rehashes_for_tests(void) {
  return atomic_load_explicit(&g_oa_rehashes_for_tests, memory_order_relaxed);
}
#define OA_COUNT_REHASH()                                    \
  (atomic_fetch_add_explicit(&g_oa_rehashes_for_tests, 1ull, \
                             memory_order_relaxed))
/* The number of entries that backward-shift deletion moved, across the
 * process. */
static _Atomic unsigned long long g_oa_shift_moves_for_tests = 0;
unsigned long long chashmap_oa_shift_moves_for_tests(void) {
  return atomic_load_explicit(&g_oa_shift_moves_for_tests,
                              memory_order_relaxed);
}
#define OA_COUNT_SHIFT_MOVE()                                   \
  (atomic_fetch_add_explicit(&g_oa_shift_moves_for_tests, 1ull, \
                             memory_order_relaxed))
/* The number of occupied slots that the backward-shift walk examined,
 * across the process. */
static _Atomic unsigned long long g_oa_shift_steps_for_tests = 0;
unsigned long long chashmap_oa_shift_steps_for_tests(void) {
  return atomic_load_explicit(&g_oa_shift_steps_for_tests,
                              memory_order_relaxed);
}
#define OA_COUNT_SHIFT_STEP()                                   \
  (atomic_fetch_add_explicit(&g_oa_shift_steps_for_tests, 1ull, \
                             memory_order_relaxed))
/* The number of chain nodes that the searches of the separate-chaining
 * backend visited, across the process. It is the counterpart of the probe
 * counter above for that backend. */
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
/* The fast attempts that the growth of a keyed map makes, across the
 * process: how many began, how many failed, and the work, the count and the
 * capacity of the last one that failed. The work of an open-addressing
 * attempt is the slots that its fill inspected; the work of a
 * separate-chaining attempt is the nodes that it hashed plus the chain nodes
 * that it walked. The counters are relaxed and atomic for the reason that
 * the probe counter above gives. */
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

/* Widens the key of a caller to the same 64-bit form that a slot stores. A
 * probe can then compare two integers. It does not call memcmp for each step.
 * Every write of key_data sets all 8 bytes to zero before it copies key_size
 * bytes in; see oa_insert. A key that this function widens in the same way
 * therefore compares equal exactly when the bytes are equal. This is true for
 * any byte order, because both sides put the bytes of the key at the start of
 * the object and set the rest to zero.
 *
 * The switch gives each copy a size that is a compile-time constant. The
 * compiler turns such a copy into a single unaligned load. A memcpy whose
 * size is known only at run time is a call.
 *
 * The listed cases are every width that an integral key can have. Every entry
 * point also checks a key from the caller against the width of its declared
 * type before the key reaches this function. No current path therefore
 * reaches the default case. The default case still clamps the size instead of
 * a copy of key_size bytes. The destination is one 8-byte object. A size that
 * did arrive unchecked would be a stack buffer overflow. A comparison that
 * only read the bytes would be no more than an over-READ. A key that is wider
 * than the slot cannot compare equal to a stored key anyway. The clamp
 * therefore loses no answer that the wider copy would have given. */
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

/* Compares the key that a slot stores against a key that oa_widen_key already
 * widened. Move that call to oa_widen_key out of the probe loop. The key of
 * the caller does not change while the probe walks. The repeat of that work
 * for each step is the whole cost that this arrangement avoids. */
static inline bool oa_keys_equal(const oa_slot* slot, uint64_t probe_key) {
  return slot->key_data == probe_key;
}

/* The bytes of the block of a table of capacity slots: the slots, then one
 * value accessor for each slot, then one metadata byte for each slot. */
#define OA_BLOCK_BYTES_PER_SLOT (sizeof(oa_slot) + sizeof(cmap_pair) + 1)

/* Allocates one block that holds capacity slots, then capacity value
 * accessors, then capacity metadata bytes, and reports where the accessors
 * and the metadata start. The accessors start right after the slots, which
 * leaves them aligned for a pointer, because a slot is two 8-byte words. The
 * block is zeroed, so every slot starts without SLOT_OCCUPIED, which is the
 * empty sentinel, and every accessor starts as {NULL, 0}.
 *
 * This function forms the byte count itself. It does not hand calloc a count
 * and a size. It must therefore make the overflow check that calloc would
 * have made. Without that check, a product that wraps becomes a small
 * allocation that succeeds. A probe loop then walks capacity entries through
 * that small allocation. */
static oa_slot* oa_alloc_block(ccol_memmgmt_procs_t* m_procs, size_t capacity,
                               cmap_pair** val_accessors_out,
                               uint8_t** metadata_out) {
  if (capacity == 0 || capacity > SIZE_MAX / OA_BLOCK_BYTES_PER_SLOT) {
    return NULL;
  }
  oa_slot* slots = (oa_slot*)_ccol_mem_calloc(
      m_procs, 1, capacity * OA_BLOCK_BYTES_PER_SLOT);
  if (!slots) return NULL;
  *val_accessors_out = (cmap_pair*)(slots + capacity);
  *metadata_out = (uint8_t*)(*val_accessors_out + capacity);
  return slots;
}

/* The smallest probe_span of a table of capacity slots: OA_DEEP_INSERT, or
 * the whole table when it is smaller than that. */
static inline size_t oa_min_probe_span(size_t capacity) {
  return capacity < OA_DEEP_INSERT ? capacity : OA_DEEP_INSERT;
}

/* Initializes the open-addressing state that the block of a map holds, and
 * allocates its table. calloc sets every slot to zero, so the metadata byte
 * of each slot starts as 0, without SLOT_OCCUPIED, which is the empty
 * sentinel. Gives false, with nothing allocated, when the table cannot be
 * allocated. */
static bool oa_init(open_addr_map* map, size_t capacity,
                    ccol_data_type key_type, ccol_data_type val_type,
                    size_t key_size, size_t val_size,
                    ccol_memmgmt_procs_t* m_procs,
                    ccol_hashing_proc_t custom_hashing_proc) {
  map->slots =
      oa_alloc_block(m_procs, capacity, &map->val_accessors, &map->metadata);
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

/* The cap on the displacement of one insert into a fast table of capacity
 * slots: 24 slots for each doubling of the table up to 4096 slots, and 288
 * from there on. An insert whose displacement is above it switches the map.
 *
 * The cap must sit above the displacements that a random function produces,
 * or ordinary keys switch the map. The tail of the displacement of an insert
 * at a given load does not depend on the size of a large table: at the
 * highest load, 0.70, a random function displaces about one insert in 5e5 by
 * more than 160 slots, one in 2e8 by more than 256 and one in 1.4e9 by more
 * than 288. A small table cannot hold a long cluster, which lowers that tail
 * well below these figures, and the cap there is lower in proportion. A
 * smaller cap, such as 8 slots for each doubling, which is about the largest
 * displacement that a random function produces in a fill, switches an
 * ordinary map that churns at its highest load once in every few thousand
 * inserts. */
#define OA_CAP_PER_DOUBLING 24u
#define OA_CAP_LOG2_CEILING 12u
static inline size_t oa_insert_cap(size_t capacity) {
  unsigned k = chmap_log2_pow2(capacity);
  return (size_t)OA_CAP_PER_DOUBLING *
         (k < OA_CAP_LOG2_CEILING ? k : OA_CAP_LOG2_CEILING);
}

/* The largest count that a table of capacity slots holds after an insert
 * that did not grow it: an insert grows the table when the count before it is
 * above 0.70 of the capacity; see oa_above_grow_load. */
static inline size_t oa_max_count(size_t capacity) {
  return (capacity / 10) * 7 + ((capacity % 10) * 7) / 10 + 1;
}

/* The bound on the sum of the displacements of the 256 inserts of a window,
 * above which a fast table switches.
 *
 * E(a) = (1 / (1 - a)^2 - 1) / 2 is the mean displacement of an insert into a
 * table of load a under a random function. The bound is
 * 256 * (5 * E(a) + 0.5) for a table of at least 4096 slots, and
 * 256 * (6 * E(a) + 0.5) for a smaller one, whose clusters vary more from one
 * window to the next. Simulated with a random function, the largest window
 * sum stays under 0.72 of this bound in tables of 16 to 2^20 slots, in fills
 * and in churn at the highest load.
 *
 * a is the highest load that the window can have reached: the count when the
 * window started, plus its 256 inserts, and never more than the table holds
 * before it grows. A window can hold deletes, so the load at its end can be
 * far below the load at which most of its inserts ran; judged at the load at
 * its end, a window that ends just after a bulk delete would switch a map
 * that holds ordinary keys.
 *
 * The arithmetic is in integers: with d = 1 - a in 16.16 fixed point,
 * 256 * 5 * E(a) = 640 * (2^32 - d^2) / d^2, and 768 in place of 640 for the
 * smaller tables. The truncation of a to 16 fractional bits and the integer
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

/* Starts a new insert window, and computes its bound from the count now. A
 * keyed table is never judged, so it skips the division. */
static inline void oa_window_restart(open_addr_map* map) {
  struct chashmap* chm = chmap_of_oa(map);
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound = chm->hash_mode == CHMAP_HASH_KEYED
                          ? UINT16_MAX
                          : oa_window_bound(map->count, map->capacity);
}

/* Places every live entry of the old table into the zeroed table that
 * new_slots heads, with the hash of the mode that keyed names, and gives the
 * largest displacement that it made. The table does not store a hash, so
 * this computes each one again. new_capacity is always a power of two, so a
 * probe advances with & (new_capacity - 1) instead of the far more costly
 * % new_capacity. */
static size_t oa_place_all(const open_addr_map* map, oa_slot* new_slots,
                           cmap_pair* new_val_accessors, uint8_t* new_metadata,
                           size_t new_capacity, const oa_slot* old_slots,
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

    // Prefetch likely next location
    __builtin_prefetch(&new_slots[(index + 1) & mask], 1, 1);

    while (new_metadata[index] & SLOT_OCCUPIED) {
      index = (index + 1) & mask;
      __builtin_prefetch(&new_slots[(index + 1) & mask], 1, 1);
    }

    new_slots[index] = old_slots[i];
    new_metadata[index] = SLOT_OCCUPIED;
    // The .ptr of the accessor refers back into its own slot: it must point
    // at the val_data of this slot. A copy from the old array, like the copy
    // of the other bytes of the slot, is therefore never correct. The code
    // must compute it again against the address of the new array for this
    // index.
    new_val_accessors[index].ptr = &new_slots[index].val_data;
    new_val_accessors[index].size = map->val_size;
    size_t displacement = (index - home) & mask;
    if (displacement > largest) largest = displacement;
  }
  return largest;
}

/* The slack of the displacement budget of a fast attempt; see
 * oa_place_all_fast_attempt. */
#define OA_ATTEMPT_SLACK 64u

/* The fast attempt of the growth of a keyed table: places every live entry
 * of the old table into the zeroed new table with the fast hash, as
 * oa_place_all does, and gives the largest displacement that it made, or
 * SIZE_MAX as soon as the fill shows that the fast hash is bad for these
 * keys. It is bad when an entry would sit more than cap = oa_insert_cap
 * slots from its home slot, or when the displacements placed so far sum to
 * more than the budget map->count + OA_ATTEMPT_SLACK. A random function fills
 * a table to the load of a growth, at most one half, with a sum of about a
 * third of the count, so neither limit refuses an ordinary key set.
 *
 * The abort is what bounds a failed attempt. Each placed entry inspects its
 * displacement plus one slot, the sum before the last placement is at most
 * the budget, and the walk of one entry stops after cap + 1 slots, so a
 * failed attempt inspects at most count + budget + cap + 1, which is
 * 2 * count + cap + 65 slots, before the caller fills the same block again
 * with the keyed hash. Without the abort, keys that an attacker made collide
 * under the fast hash would cost the fill a number of probes quadratic in the
 * count. */
static __attribute__((noinline)) size_t oa_place_all_fast_attempt(
    const open_addr_map* map, oa_slot* new_slots, cmap_pair* new_val_accessors,
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
    new_val_accessors[index].ptr = &new_slots[index].val_data;
    new_val_accessors[index].size = map->val_size;
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

/* Rebuilds the table at new_capacity with the hash of the mode that keyed
 * names, and restarts the insert window. A fast rebuild whose largest
 * displacement is above the cap of the new table, which a shrink that merges
 * clusters can produce, rebuilds the same new block again with the keyed
 * hash, so the table that a fast map ends up with never holds a displacement
 * above its cap; see oa_insert_cap.
 *
 * A request for the fast hash on a keyed table is the fast attempt of a
 * growth: the fill stops as soon as it shows that the fast hash is bad for
 * these keys (see oa_place_all_fast_attempt), and the same new block is then
 * filled with the keyed hash, so the attempt allocates nothing more. A table
 * that the attempt fills keeps the fast hash, and the insert window that
 * restarts below judges it as it judges any fast table.
 *
 * The function gives ccol_not_enough_memory when the allocation fails, and
 * it leaves the map completely unchanged. A caller whose own operation then
 * fails only because this opportunistic rehash could not happen can report
 * that honestly. It does not have to name some other condition instead. See
 * the use of this return value in oa_insert_impl. */
static ccol_retval_t oa_rebuild(open_addr_map* map, size_t new_capacity,
                                bool keyed) {
  cmap_pair* new_val_accessors = NULL;
  uint8_t* new_metadata = NULL;
  oa_slot* new_slots = oa_alloc_block(map->m_procs, new_capacity,
                                      &new_val_accessors, &new_metadata);
  if (!new_slots) {
    return ccol_not_enough_memory;
  }

  size_t largest;
  if (!keyed && oa_keyed(map)) {
    largest = oa_place_all_fast_attempt(map, new_slots, new_val_accessors,
                                        new_metadata, new_capacity);
  } else {
    largest = oa_place_all(map, new_slots, new_val_accessors, new_metadata,
                           new_capacity, map->slots, map->metadata,
                           map->capacity, keyed);
  }
  if (!keyed && largest > oa_insert_cap(new_capacity)) {
    memset(new_slots, 0, new_capacity * OA_BLOCK_BYTES_PER_SLOT);
    keyed = true;
    largest = oa_place_all(map, new_slots, new_val_accessors, new_metadata,
                           new_capacity, map->slots, map->metadata,
                           map->capacity, keyed);
  }

  _ccol_mem_free(map->m_procs, map->slots);
  map->slots = new_slots;
  map->val_accessors = new_val_accessors;
  map->metadata = new_metadata;
  map->capacity = new_capacity;
  size_t span = oa_min_probe_span(new_capacity);
  map->probe_span = largest + 1 > span ? largest + 1 : span;
  chmap_of_oa(map)->hash_mode = keyed ? CHMAP_HASH_KEYED : CHMAP_HASH_FAST;
  oa_window_restart(map);
  OA_COUNT_REHASH();
  return ccol_success;
}

/* The capacity that the insert path rehashes to, for a table that is about
 * to hold live_after_insert entries. It is the smallest power of two at or
 * above twice that count, so the live load after the rehash is at most one
 * half. It is never less than minimum_allowed_bucket_array_size.
 *
 * The insert path rehashes only when the live entries fill more than
 * OPEN_ADDR_MAX_LOAD_FACTOR of the table, so this is always twice the
 * capacity. Sizing from the live count keeps that true by construction, and
 * keeps the result above OPEN_ADDR_MIN_LOAD_FACTOR, which is one quarter:
 * half of the SMALLEST power of two at or above twice the live count is below
 * twice that count. A delete therefore cannot shrink it straight back, and a
 * shrink leaves a live load below one half, which needs a fifth of the
 * capacity in inserts before the next grow. Growth and shrink cannot
 * alternate. */
static size_t oa_rehash_capacity_for(size_t live_after_insert) {
  if (live_after_insert > ccol_max_power_of_two_size_t / 2) {
    return ccol_max_power_of_two_size_t;
  }
  size_t wanted = _ccol_find_nearest_gte_power_of_two(2 * live_after_insert);
  return wanted < minimum_allowed_bucket_array_size
             ? minimum_allowed_bucket_array_size
             : wanted;
}

/* Answers count / capacity > OPEN_ADDR_MAX_LOAD_FACTOR (0.70) in integers,
 * as count * 10 > capacity * 7. Neither product overflows while capacity is
 * at most SIZE_MAX / 10, because count never exceeds capacity; a larger
 * capacity takes the division form, which is exact for any capacity. The
 * common form is two multiplications by constants and a compare, with no
 * conversion to floating point and no division. */
static inline bool oa_above_grow_load(size_t count, size_t capacity) {
  if (__builtin_expect(capacity <= SIZE_MAX / 10, 1)) {
    return count * 10 > capacity * 7;
  }
  return count > (capacity / 10) * 7 + ((capacity % 10) * 7) / 10;
}

/* Probes for the first empty slot. It probes in probe order, and it starts at
 * the home slot of hash_val. It gives map->capacity when the table has no
 * empty slot. This function locates an insertion point against the *current*
 * capacity and mask of the map. The caller must already know that the key is
 * not present anywhere in the table. This is exactly what oa_insert needs
 * after a rehash. The rehash makes the index and the capacity of an earlier
 * probe stale. */
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
 * be present. It lies within probe_span slots of its home slot. The key is
 * hashed from a buffer as wide as any key type, because the compiler cannot
 * see that an open-addressing map never has a long double key. */
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
  ccol_assert(false);
  return index;
}

/* Switches a fast table to the keyed hash, at the same capacity, and gives
 * the new slot of the key that slot index held. The rebuild allocates a new
 * block. When that fails the table stays fast and correct, the key stays
 * where it is, and the next window end or deep insert tries again. */
static size_t oa_switch_keeping(open_addr_map* map, size_t index) {
  uint64_t key = map->slots[index].key_data;
  if (oa_rebuild(map, map->capacity, true) != ccol_success) {
    return index;
  }
  return oa_find_stored_key(map, key);
}

/* The cold path of an insert whose displacement is at least OA_DEEP_INSERT.
 * It widens probe_span to cover the new key, and a fast table whose new key
 * sits further from its home slot than the cap allows switches. Gives the
 * slot of the new key, which the switch moves. */
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

/* The cold path of the insert that ends a window. A fast table whose window
 * sum is above its bound switches. Gives the slot of the new key, which the
 * switch moves. */
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

/* The cold path of an insert of a new key into a table above its growth
 * load. It grows the table and gives the empty slot for the key in the grown
 * table, with its home slot in *home. A growth asks for the fast hash in
 * either mode: a fast table can still switch, and a keyed table makes its
 * fast attempt; see oa_rebuild. Gives SIZE_MAX, with the map unchanged, when
 * the allocation fails. */
static __attribute__((noinline)) size_t oa_grow_for_insert(
    open_addr_map* map, const cmap_pair* key_pair, size_t* home) {
  if (oa_rebuild(map, oa_rehash_capacity_for(map->count + 1), false) !=
      ccol_success) {
    return SIZE_MAX;
  }
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  *home = hash_index_for(hash_val, map->capacity);
  return oa_find_insertion_slot(map, hash_val);
}

/* Inserts a key-value pair, or updates it, with a linear probe. The table
 * holds no tombstone (see oa_delete), so the first empty slot of the probe
 * both proves that the key is absent and is where it goes.
 *
 * The function probes for an existing entry FIRST. That probe does not
 * consider growth at all. A pure value update for a key that is already
 * present never changes elem_count. Such an update therefore can never start
 * a rehash. It also can never make the chmap_get_elem_ref pointer of ANY
 * OTHER key invalid. Without this order, it could do so only because earlier,
 * unrelated inserts left the map above its growth threshold. The function
 * considers the load factor count / capacity only after it confirms that the
 * key is genuinely absent. The table holds no tombstone, so count is the
 * whole load. A load above OPEN_ADDR_MAX_LOAD_FACTOR then starts a rehash to
 * the capacity that oa_rehash_capacity_for() picks from the live count. A
 * fresh probe with oa_find_insertion_slot then locates the insertion point
 * against the rebuilt table. The rehash makes the index and the capacity of the
 * first probe invalid. The absence of the key needs no second check: a rehash
 * only moves entries that are already live, and it cannot introduce this key.
 * The common case is a plain update, or a plain insert of a new key that does
 * not cross the growth threshold. That case costs exactly one probe pass. A
 * design that checks growth up front for every call costs the same.
 *
 * An insert of a new key then reports its displacement to the detection of
 * the adaptive hash; see the section on adaptive hashing. A displacement of
 * OA_DEEP_INSERT or more takes oa_note_deep_insert, and every insert adds to
 * the window, which costs a lea, an add and a jump on the carry. Either can
 * switch the table, which moves the new key, so both give back its slot.
 *
 * The function can need a growth that it cannot get. oa_rebuild() can fail on
 * an allocation, or the function can skip it because the table is already at
 * its architectural maximum capacity. The insert then still runs against the
 * current size of the table. If that finds no room at all, the two cases get
 * distinct, honest return codes. See the final lines of the function. They do
 * not both collapse into ccol_container_full.
 *
 * The caller is chmap_insert_elem. It already rejects any key_pair or
 * val_pair whose size does not match map->key_size or map->val_size exactly.
 * Both memcpy calls below are therefore always inside the 8-byte key_data
 * field and val_data field that they target. Nothing here can spill into a
 * neighbouring slot. */
/* slot_out selects what a key that is already present does. NULL overwrites
 * its value, which is chmap_insert_elem(). Not NULL leaves such an entry
 * untouched, and reports through *slot_out the value accessor of the entry
 * that the key names once the call succeeds, whether it was present or has
 * just been inserted. That is ccol_chmap_insert_or_get_elem(). Every caller
 * passes a constant, so each instantiation keeps only its own arm. */
static inline __attribute__((always_inline)) ccol_retval_t
oa_insert_impl(open_addr_map* map, const cmap_pair* key_pair,
               const cmap_pair* val_pair, const cmap_pair** slot_out) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t start_index = index;
  bool found_empty = false;
  const uint64_t probe_key = oa_widen_key(key_pair->ptr, key_pair->size);

  // Prefetch first location
  __builtin_prefetch(&map->slots[index], 1, 1);

  do {
    OA_COUNT_PROBE();
    // Prefetch next likely location
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 1, 1);

    if (!(map->metadata[index] & SLOT_OCCUPIED)) {
      found_empty = true;
      break;
    }

    if (oa_keys_equal(&map->slots[index], probe_key)) {
      if (slot_out) {
        *slot_out = &map->val_accessors[index];
        return ccol_key_already_present;
      }
      // val_pair->ptr can alias the val_data of this slot. One example is a
      // caller that inserts a value again through the raw chmap_insert_elem
      // function layer. That value comes from a pointer that
      // chmap_get_elem_ref or chmap_get_ptr gave for this exact key. memcpy
      // needs its source and its destination to never overlap. This code
      // therefore copies the value into a small stack buffer first when they
      // do overlap. val_pair->size is always 8 bytes or less for this
      // backend. sc_reset_val_of_llist_node has the identical protection
      // against aliasing for the separate-chaining backend.
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

  // The key genuinely does not exist. The ccol_max_elem_count check of
  // sc_insert sits at the same point; see its own comment for the reason why
  // the lookup for an existing key must run first. The check is explicit here
  // for the same reason. It does not merely fall out of the physical cap on
  // capacity at ccol_max_power_of_two_size_t (== ccol_max_elem_count).
  // Without the explicit check, a table that is already at that architectural
  // limit and full reaches the same
  // ccol_container_full outcome some lines further down. But it reaches that
  // outcome only after it computes a load factor and confirms that growth is
  // impossible, and both of those steps are needless.
  if (map->count == ccol_max_elem_count) {
    return ccol_container_full;
  }

  // Only now is it safe to consider an opportunistic rehash. This call
  // really is going to add a new entry.
  bool rehash_oom_failed = false;
  if (oa_above_grow_load(map->count, map->capacity) &&
      map->capacity < ccol_max_power_of_two_size_t) {
    // The capacity changes under the probe above, and the index mask changes
    // with it, so the grow probes the rebuilt table again.
    size_t grown = oa_grow_for_insert(map, key_pair, &start_index);
    if (grown != SIZE_MAX) {
      index = grown;
      found_empty = index < map->capacity;
    } else {
      rehash_oom_failed = true;
    }
  }

  if (found_empty) {
    memset(&map->slots[index].key_data, 0, sizeof(uint64_t));
    memset(&map->slots[index].val_data, 0, sizeof(uint64_t));
    memcpy(&map->slots[index].key_data, key_pair->ptr, key_pair->size);
    memcpy(&map->slots[index].val_data, val_pair->ptr, val_pair->size);
    map->metadata[index] = SLOT_OCCUPIED;
    map->val_accessors[index].ptr = &map->slots[index].val_data;
    map->val_accessors[index].size = map->val_size;
    map->count++;
    size_t displacement = (index - start_index) & (map->capacity - 1);
    if (__builtin_expect(displacement >= OA_DEEP_INSERT, 0)) {
      index = oa_note_deep_insert(map, index, displacement);
    }
    if (__builtin_expect(chmap_window_add(chmap_of_oa(map), displacement), 0)) {
      index = oa_window_end(map, index);
    }
    if (slot_out) {
      *slot_out = &map->val_accessors[index];
    }
    return ccol_success;
  }

  // There is no room anywhere in the table. The code separates two cases. The
  // first is a genuine architectural capacity limit: the table was already at
  // ccol_max_power_of_two_size_t, so the code never even tried to grow it. The
  // second is a growth that the code needed but that an allocation failure
  // stopped. That second case is a lack of resources. It does not mean that
  // this map reached its real element cap.
  return rehash_oom_failed ? ccol_not_enough_memory : ccol_container_full;
}

static ccol_retval_t oa_insert(open_addr_map* map, const cmap_pair* key_pair,
                               const cmap_pair* val_pair) {
  return oa_insert_impl(map, key_pair, val_pair, NULL);
}

static ccol_retval_t oa_insert_or_get(open_addr_map* map,
                                      const cmap_pair* key_pair,
                                      const cmap_pair* val_pair,
                                      const cmap_pair** slot_out) {
  return oa_insert_impl(map, key_pair, val_pair, slot_out);
}

/* Looks up key_pair with a linear probe. An empty slot stops the search at
 * once. This is safe, because neither an insert nor a delete ever leaves a
 * gap between a key and its home slot; see oa_delete. The search also stops
 * after probe_span slots, because no key sits further from its home slot.
 * The function gives a pointer to the entry of this slot in
 * map->val_accessors. That cmap_pair is distinct from the one of every other
 * slot. The code fills it wherever it writes the slot itself, so this path
 * only takes its address. A single shared scratch field would behave
 * differently. Two or more chmap_get_elem_ref results that the caller holds
 * at the same time for different keys therefore never alias one another. Each
 * one stays valid until something really changes the map, which is an insert,
 * a delete or a resize. It does not become invalid at the next get. This is
 * the documented contract of this function. */
static ccol_retval_t oa_get(open_addr_map* map, const cmap_pair* key_pair,
                            const cmap_pair** val_pair) {
  size_t hash_val = hash_key_data(key_pair->ptr, key_pair->size, map->key_type,
                                  map->custom_hashing_proc, oa_keyed(map),
                                  g_chmap_hash_secret.int_seed);
  size_t index = hash_index_for(hash_val, map->capacity);
  size_t left = map->probe_span;
  const uint64_t probe_key = oa_widen_key(key_pair->ptr, key_pair->size);

  // Prefetch first location
  __builtin_prefetch(&map->slots[index], 0, 1);

  do {
    OA_COUNT_PROBE();
    // Prefetch next likely location
    __builtin_prefetch(&map->slots[(index + 1) & (map->capacity - 1)], 0, 1);

    if (!(map->metadata[index] & SLOT_OCCUPIED)) {
      return ccol_key_not_found;
    }

    if (oa_keys_equal(&map->slots[index], probe_key)) {
      *val_pair = &map->val_accessors[index];
      return ccol_success;
    }

    index = (index + 1) & (map->capacity - 1);
  } while (--left != 0);

  return ccol_key_not_found;
}

/* Closes the gap that an emptied slot hole leaves in its cluster, by
 * backward-shift deletion. The table therefore never holds a tombstone.
 *
 * Linear probing places every entry at or after its home slot, with no empty
 * slot in between. Emptying a slot breaks that for every later entry of the
 * cluster whose home slot lies at or before the hole. The walk visits the
 * rest of the cluster in probe order. An entry at slot j with home slot h may
 * move into the hole exactly when h is not in the cyclic range (hole, j],
 * that is when the distance from h to j is at least the distance from the
 * hole to j. Such an entry moves, and its old slot becomes the hole. Every
 * other entry stays, because moving it would put it before its home slot. The
 * masked differences make the test correct for a cluster that wraps past the
 * last slot.
 *
 * The walk stops at the first empty slot, or once the slot it visits is
 * span slots past the hole: no entry sits span or more slots from its home
 * slot (see probe_span), so no entry from there on may move into the hole.
 * That bounds a delete inside a long run of keys whose home slots are
 * consecutive, which no entry of the run may leave.
 *
 * The hole itself is always empty, so the walk meets an empty slot within
 * one pass even in a table with no other free slot.
 *
 * A move writes no accessor. The accessor of a slot describes that slot and
 * nothing else, and it is written when an entry is first stored there. The
 * destination of a move always held an entry a moment ago (the deleted one,
 * or the source of the previous move), so its accessor is already correct.
 *
 * oa_delete empties the slot and calls oa_backward_shift_close_gap only
 * when the next slot is occupied. Each walk is an out-of-line instance of
 * this body, so the delete whose next slot is empty costs one store and one
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

/* One walk for each key type that open addressing accepts with the built-in
 * hash, in each mode. Each takes only the values that it reads, so it needs
 * no stack frame of its own. The size argument is the fixed width of the
 * type, which hash_key_data() does not read for these types. */
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

/* The walk for a custom hash, or for a key type that the list above does not
 * name. */
static __attribute__((noinline)) void oa_shift_walk_generic(open_addr_map* map,
                                                            size_t hole) {
  oa_backward_shift_walk(map->slots, map->metadata, map->capacity,
                         map->key_size, map->key_type, map->custom_hashing_proc,
                         oa_keyed(map), g_chmap_hash_secret.int_seed,
                         map->probe_span, hole);
}

/* Selects the walk for the key type and the mode of map. The walk writes
 * through a byte pointer, which may alias anything, so every walk receives
 * the fields that it reads as arguments. Without that, every step reloads
 * them from the map. Each call below is in tail position, so this function
 * adds no frame. */
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
 * oa_backward_shift_close_gap. The search stops after probe_span slots, as
 * the one of oa_get does. An entry of the same cluster can move one or more
 * slots toward its home slot, so every reference that the map handed out
 * becomes invalid, which the contract of chmap_get_elem_ref already states.
 * The function halves the table when the load factor after the delete falls
 * below OPEN_ADDR_MIN_LOAD_FACTOR. */
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
      // least 16. The integer form spares a division on every delete.
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

/* Frees the table. The state itself lives in the block of the map, which
 * __chmap_destroy frees. */
static void oa_destroy(open_addr_map* map) {
  _ccol_mem_free(map->m_procs, map->slots);
}

/* Sets the metadata byte of every slot, and every accessor entry, to zero. It
 * does this in place, at the current capacity of the map, and it allocates
 * nothing. The open-addressing backend never allocates heap memory for a key
 * or a value. Every key and every value lives inline in the slot array
 * itself. A clear of the metadata, which drops SLOT_OCCUPIED on every slot,
 * is therefore a complete and correct "destroy
 * every element" for this backend. There is nothing else to free. oa_reset()
 * uses this function. A failure to allocate the new capacity that the caller
 * asked for then still destroys every element that exists. It does not leave
 * the old table, and all of its data, completely unchanged. */
static void oa_clear_in_place(open_addr_map* map) {
  memset(map->slots, 0, map->capacity * OA_BLOCK_BYTES_PER_SLOT);
  map->count = 0;
  map->probe_span = oa_min_probe_span(map->capacity);
  oa_window_restart(map);
}

/* Clears every entry. When new_capacity differs from the current capacity, it
 * also replaces the slot array with a new zeroed array of that size. When
 * new_capacity is 0, or equal to the current capacity of the map, it clears
 * the existing slots array and val_accessors array in place. It then
 * allocates nothing at all. sc_reset() has the same behaviour: it skips the
 * realloc when the size does not change. The hash mode stays as it is, and
 * the insert window restarts.
 *
 * The function destroys every element whatever the return value is. This
 * matches the documented contract of chmap_reset. The allocation for
 * new_capacity can fail. The function then falls back to oa_clear_in_place()
 * against the current, unchanged capacity of the map. It does not return with
 * the old table, and every element still inside it, completely intact. */
static ccol_retval_t oa_reset(open_addr_map* map, size_t new_capacity) {
  if (new_capacity == 0 || new_capacity == map->capacity) {
    oa_clear_in_place(map);
    return ccol_success;
  }

  cmap_pair* new_val_accessors = NULL;
  uint8_t* new_metadata = NULL;
  oa_slot* new_slots = oa_alloc_block(map->m_procs, new_capacity,
                                      &new_val_accessors, &new_metadata);
  if (!new_slots) {
    oa_clear_in_place(map);
    return ccol_not_enough_memory;
  }

  _ccol_mem_free(map->m_procs, map->slots);
  map->slots = new_slots;
  map->val_accessors = new_val_accessors;
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

/* Puts node at the front of the doubly-linked list that root describes. The
 * list therefore keeps the element that the caller inserted last at the head,
 * and the element that the caller inserted first at the tail. An iteration
 * through the next pointers visits the nodes in the reverse insertion order,
 * with the newest node first. An iteration from the tail through the prev
 * pointers visits them in insertion order. */
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

/* Removes node from the doubly-linked list. It does not free the node. The
 * update of the neighbours and of both ends covers every case: the head node,
 * the tail node, a node in the middle and the only node. */
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

/* Frees the key buffer and the value buffer of an entry, when those buffers
 * are on the heap and not inline, with m_procs, the procs of the map. It then
 * removes the node from the insertion-order dllist, and frees the node struct
 * itself. A NULL all_elems skips the removal from the dllist. Only the
 * failure paths of sc_create_llist_node() pass NULL, because the node they
 * free is not on the list yet. Every other caller, a delete, a reset and the
 * teardown of a whole map, passes the list of the map, so the list never
 * names a freed node.
 *
 * A buffer is on the heap exactly when the size in its accessor exceeds
 * INLINE_STORAGE_THRESHOLD. A node that sc_create_llist_node() abandons part
 * way has the size of a buffer that it did not allocate still at 0 from
 * calloc, so the function frees only what was allocated. */
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

/* Allocates a new llist_node and copies the key data and the value data into
 * it. The function stores a key and a value inline when it is 23 bytes or
 * less, which is SSO. It stores anything larger in a separate heap buffer.
 * The accessor cmap_pair structs then point into the storage that the
 * function chose, so a caller always goes through a stable pointer. On
 * success, the function puts the node at the front of the insertion-order
 * dllist. */
/* Forced inline: it has two callers, the insert and the insert-or-get
 * instantiations of sc_insert_impl, and as a real call it costs the insert
 * path about 57 more instructions for each new key than inlined, because the
 * sizes and the storage choice no longer fold into the caller. */
static inline __attribute__((always_inline)) llist_node* sc_create_llist_node(
    dllist_root* all_elems, ccol_memmgmt_procs_t* m_procs, size_t hash_val,
    const void* key_ptr, size_t key_size, const void* val_ptr,
    size_t val_size) {
  llist_node* new_elem =
      (llist_node*)_ccol_mem_calloc(m_procs, 1, sizeof(llist_node));
  if (!new_elem) return NULL;

  new_elem->hash_val = hash_val;

  // Each accessor is written only once its storage holds the bytes, so a
  // failure below leaves the size of a buffer that was never allocated at 0;
  // see sc_destroy_llist_node.
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

/* Compares two long double keys by VALUE. It matches the special cases of
 * hash_long_double_value exactly. The hash and the equality therefore never
 * disagree with each other for the same pair of keys.
 * - When one operand is NaN, the pair is equal only when BOTH are NaN. Every
 *   NaN long double collapses into one key. The function reads no padding
 *   byte to decide this. See the comment on hash_long_double_value for the
 *   reason. The padding of a NaN long double is often genuinely uninitialized
 *   memory. It is not merely content that is unspecified but stable. Any
 *   comparison of it, even one through memcmp, is therefore a real hazard. It
 *   is not only a source of results that differ from run to run.
 * - In every other case the function uses the native `==`. That operator
 *   works on the value, so unspecified padding bits never affect the result.
 *   A raw memcmp over the full representation behaves differently. -0.0L and
 *   0.0L also compare equal here, exactly as -0.0 and 0.0 already do for the
 *   float key type and the double key type. */
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

/* Compares the key of a node against key_ptr. The size check is a fast
 * reject. For a key of 4 bytes and for a key of 8 bytes, the function
 * compares integers instead of a call to memcmp. The compiler can then emit a
 * single load and compare instruction. The function reads key_type for one
 * purpose only: to send a long double key to long_double_keys_equal instead
 * of the generic byte-exact paths below. That test must run before the
 * dispatch on the size. sizeof(long double) is the same as sizeof(double) on
 * some platforms and ABIs. A long double key must never fall into the plain
 * 8-byte integer-compare branch on those platforms. */
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

/* sc_find_in_llist for a map with a custom key equality procedure. The stored
 * hash still rejects most nodes with one comparison, because the procedure
 * must agree with the hash. The procedure gets both sizes and decides on its
 * own whether keys of different sizes can be equal. It is a separate
 * function so that a map without the procedure keeps its search loop
 * unchanged. */
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

/* The number of nodes in the chain that head starts. */
static __attribute__((noinline)) size_t
sc_chain_length(const llist_node* head) {
  size_t length = 0;
  for (; head; head = head->next) length++;
  return length;
}

/* A linear search through the singly-linked chain of a bucket. hash_val is
 * the full hash of key_ptr from the caller, before any reduction. The
 * function compares it against the stored hash_val of each node first. Most
 * rejections are therefore one size_t comparison. The function reaches the
 * memcmp/strcmp comparison inside sc_compare_keys only when two hashes really
 * collide. It gives the matching node, or NULL. The bucket scaling strategy
 * keeps the chains short, which is O(1) on average.
 *
 * walk is NULL, or receives the number of nodes that the search met when it
 * finds no match, which is the length of the chain. The insert of a new key
 * reports that number to the detection of the adaptive hash. Every caller
 * passes a constant NULL or a pointer, so a lookup keeps no count. */
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

/* Updates the value that an existing node stores. It covers three cases. The
 * cases come from the new value size against the stored size. The first case
 * is the same size, where the function overwrites the value in place. The
 * second case is a smaller value that fits inline, where the function moves
 * to inline storage and frees the old heap buffer. The third case is a larger
 * value, where the function reallocs the heap buffer or allocates a new one.
 *
 * val_ptr can alias the current value storage of this entry. A caller is free
 * to insert a value again from a pointer that chmap_get_elem_ref,
 * chmap_get_ptr or chmap_get gave it for this exact key. For a char* value
 * type, chmap_get and chmap_get_ptr give a pointer straight into the stored
 * bytes of a separate-chaining entry. That is their own documented contract:
 * "for a string, the macro gives the char* itself" and "pointer to the char*
 * itself". Every branch below changes the existing storage of the entry
 * before it would otherwise read the bytes of val_ptr. It frees the heap
 * buffer, or it overwrites the val_storage union in place, or it reallocs the
 * heap buffer. An aliased val_ptr would then see freed, corrupted or moved
 * memory instead of the value that the caller wanted. The overlap check below
 * runs one time for each call and is cheap. The snapshot copy itself runs
 * only on the rare path where the two alias. The common case, where they do
 * not alias, pays only the comparison. */
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
  // function read the storage of a value from.
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

/* Removes the node that matches key_ptr from the chain of a bucket. It sets
 * *found, and it gives the new head of the chain. hash_val lets the search
 * reject a node that does not match with one size_t comparison, before it
 * falls back to sc_compare_keys. sc_find_in_llist does the same. The pointer
 * to the previous node gives an O(n) delete with no doubly-linked bucket
 * list. */
/* Unlinks node from the chain whose head is head, destroys it, and gives
 * the new head of the chain. previous is the node before it, or NULL. */
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

/* sc_delete_from_llist for a map with a custom key equality procedure. It is
 * a separate function for the reason that sc_find_in_llist_custom_eq gives. */
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

/* Destroys every node in the chain of a bucket and gives NULL. A reset of the
 * map, and a destroy of the map, use it to clear every bucket in turn. */
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

/* Computes the scale-up threshold and the scale-down threshold again, from
 * the current bucket array size. Call this function after every resize of the
 * bucket array. It keeps the thresholds in step with the new capacity.
 *
 * The result matches the formulas that chashmap.h documents exactly, which
 * are "(bucket_count + 1) * 1.5" and "(bucket_count + 1) / 8". It never
 * overflows size_t. The scale-up threshold is floor(3 * bucket_arr_size / 2),
 * which is the bucket_arr_size + bucket_arr_size / 2 expression below. That
 * expression cannot overflow, because bucket_arr_size has a cap at
 * ccol_max_power_of_two_size_t. The code then adds a correction that depends
 * on parity: +1 for an even bucket_arr_size and +2 for an odd one. That
 * correction comes from floor((n+1)*3/2) written in terms of floor(3n/2). A
 * direct (bucket_arr_size + 1) * 3 would overflow size_t at that same upper
 * bound. The scale-down threshold has no such risk, because
 * bucket_arr_size + 1 on its own never overflows. */
static void sc_set_scaling_limits(sep_chain_map* map) {
  size_t base_up = map->bucket_arr_size + map->bucket_arr_size / 2;
  map->elem_count_to_scale_up =
      base_up + ((map->bucket_arr_size % 2 == 0) ? 1 : 2);
  map->elem_count_to_scale_down = (map->bucket_arr_size + 1) / 8;
}

/* The bucket array that the block of the map holds right after struct
 * chashmap, and its length in *count. A map whose first bucket array is
 * small starts with this one, so creating it takes a single allocation. The
 * map uses it while its size is that length, and never frees it, because it
 * is part of the block.
 *
 * A block that holds no such array gives NULL and a count of 0. The address
 * right after the struct is then the end of the block, or its copy of the
 * procs, and an allocator that hands out adjacent blocks with no header can
 * place the separate bucket array of the map exactly there. A comparison of
 * that array with the address would then take it for part of the block,
 * and the map would never free it. */
static inline llist_node** sc_inline_bucket_arr(sep_chain_map* map,
                                                size_t* count) {
  *count = map->inline_buckets;
  return *count ? (llist_node**)(chmap_of_sc(map) + 1) : NULL;
}

/* The bound on the number of nodes that the 256 inserts of a window met,
 * above which a fast map switches: 256 * (4.5 * lambda + 0.5), where lambda
 * is the number of entries for each bucket. A new key under a random function
 * meets lambda nodes on average. lambda is the highest load that the window
 * can have reached: the count when the window started plus its 256 inserts,
 * and never more than the count at which the bucket array grows; see
 * oa_window_bound for why the load at the end of the window does not do.
 *
 * The arithmetic is in integers: lambda in 16.16 fixed point, so the bound
 * is 1152 * lambda / 65536 + 128, within two counts, and within one percent,
 * of the real-valued bound. start_count is the count when the window starts
 * and scale_up the count at which the bucket array grows. */
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

/* Starts a new insert window, and computes its bound from the count now. A
 * keyed map is never judged, so it skips the arithmetic. */
static inline void sc_window_restart(sep_chain_map* map) {
  struct chashmap* chm = chmap_of_sc(map);
  chm->insert_window = CHMAP_WINDOW_START;
  chm->window_bound =
      chm->hash_mode == CHMAP_HASH_KEYED
          ? UINT16_MAX
          : sc_window_bound(map->elem_count, map->elem_count_to_scale_up,
                            map->bucket_arr_size);
}

/* The hash that the node of a separate-chaining map stores for its key, in
 * the mode that keyed names. */
static inline size_t sc_hash_of_node(const sep_chain_map* map,
                                     const llist_node* node, bool keyed) {
  return hash_key_data(
      node->key_pair_accessor.ptr, node->key_pair_accessor.size, map->key_type,
      map->custom_hashing_proc, keyed, g_chmap_hash_secret.int_seed);
}

/* Switches a fast map to the keyed hash. Every node gets its keyed hash, and
 * the nodes are linked into the same bucket array again. Nothing moves and
 * nothing is allocated, so the switch cannot fail, and every accessor, every
 * entry reference, the insertion order and every iterator stay valid. */
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

/* The cold path of an insert of a new key that met SC_CHAIN_CAP nodes or
 * more. A fast map switches. */
static __attribute__((noinline)) void sc_note_long_chain(sep_chain_map* map) {
  if (!sc_keyed(map)) {
    sc_switch_to_keyed(map);
  }
}

/* The cold path of the insert that ends a window. A fast map whose window
 * sum is above its bound switches. */
static __attribute__((noinline)) void sc_window_end(sep_chain_map* map) {
  struct chashmap* chm = chmap_of_sc(map);
  bool switch_now = chm->hash_mode == CHMAP_HASH_FAST &&
                    chm->insert_window > chm->window_bound;
  sc_window_restart(map);
  if (switch_now) {
    sc_switch_to_keyed(map);
  }
}

/* The slack of the walk budget of a fast attempt; see sc_relink_fast_attempt.
 */
#define SC_ATTEMPT_SLACK 64u

/* The fast attempt of the growth of a keyed map: links every node into the
 * zeroed bucket array new_arr of new_size buckets with the fast hash, which
 * it stores in the node, and gives true. A node goes to the front of its
 * chain after a walk of the nodes already there, so the attempt knows the
 * length of every chain. It stops as soon as it shows that the fast hash is
 * bad for these keys: when a chain would reach SC_CHAIN_CAP nodes, or when
 * the walks so far sum to more than the budget elem_count +
 * SC_ATTEMPT_SLACK. A random function at the load after a growth, at most
 * three eighths of an entry for each bucket, walks about a fifth of the
 * count in all, so neither limit refuses an ordinary key set.
 *
 * A failed attempt then links every node again with the keyed hash into the
 * same array, and gives false. The nodes that the attempt reached hold the
 * fast hash and get the keyed one again; the rest still hold the keyed one.
 * Nothing moves and nothing is allocated, so every accessor, every entry
 * reference, the insertion order and every iterator stay valid either way.
 * A failed attempt hashes at most elem_count nodes with the fast hash and
 * walks at most elem_count + SC_ATTEMPT_SLACK + SC_CHAIN_CAP - 1 chain nodes,
 * which is linear in the count whatever the keys are. */
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
 * into its new bucket, which hash_index_for picks from the stored hash. A
 * size that is a power of two is what makes that shift well defined. A
 * resize restarts the insert window. A shrink merges
 * the chains of neighbouring buckets, since the new index is the old one
 * shifted right, and the relink below visits every node anyway, so it
 * measures the merged chains: a fast map with a merged chain of SC_CHAIN_CAP
 * nodes or more switches. A growth of a keyed map makes its fast attempt
 * instead of the relink; see sc_relink_fast_attempt. A shrink keeps the
 * mode. */
static void sc_scale(sep_chain_map* map, bool up) {
  if (up &&
      (map->bucket_arr_size > ccol_max_power_of_two_size_t / scale_factor)) {
    // This is beyond the scale-up limit
    return;
  }

  size_t new_size = up ? map->bucket_arr_size * scale_factor
                       : map->bucket_arr_size / scale_factor;

  // A division by scale_factor (4x) can go below the minimum bucket array
  // size. This happens for any bucket_arr_size that is not on the
  // "minimum_allowed_bucket_array_size * 4^k" line. One example is 32.
  // chmap_create_full and chmap_reset reach 32 directly for any requested
  // size between minimum_allowed_bucket_array_size and
  // minimum_allowed_bucket_array_size * scale_factor. 32 / 4 == 8, which is
  // below the floor. The caller of sc_delete only guarantees that
  // bucket_arr_size > minimum_allowed_bucket_array_size before this call. It
  // does not guarantee that the size divides down cleanly. The code therefore
  // clamps here. Without the clamp, a table that is off that line goes below
  // the floor.
  if (!up && new_size < minimum_allowed_bucket_array_size) {
    new_size = minimum_allowed_bucket_array_size;
  }

  // A shrink to the length of the bucket array in the block takes that array
  // again, which allocates nothing and cannot fail. The map is not using it
  // at this point, because the current size differs from the new one.
  size_t inline_count = 0;
  llist_node** new_arr = sc_inline_bucket_arr(map, &inline_count);
  if (new_size == inline_count) {
    memset(new_arr, 0, new_size * sizeof(llist_node*));
  } else {
    new_arr = (llist_node**)_ccol_mem_calloc(map->m_procs, new_size,
                                             sizeof(llist_node*));
    if (!new_arr) return;
  }

  // Old buckets i * ratio to i * ratio + ratio - 1 merge into new bucket i
  // on a shrink. merged counts the nodes of the current group.
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

  // The address of the array in the block is computed again here, so that
  // the relink loop above keeps no register for it.
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

/* Initializes the separate-chaining state that the block of a map holds.
 * The first bucket array is the one in the block when the block holds one,
 * which is already zeroed; otherwise the function allocates it, and calloc
 * sets every bucket pointer to zero. Gives false, with nothing allocated,
 * when that allocation fails. */
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

/* Inserts a key-value pair into the separate-chaining map, or updates it. For
 * a key that is already present, sc_reset_val_of_llist_node updates the value
 * in place. For a new key, the function makes a node and puts it at the front
 * of the chain of the bucket. After the insert, the function scales the
 * bucket array up when elem_count reaches elem_count_to_scale_up.
 *
 * The lookup for an existing key runs before the ccol_max_elem_count check,
 * and not after it. The check on fullness only makes sense for an insert of a
 * genuinely new key. An update of a key that is already present never changes
 * elem_count at all. With the fullness check first, a map that reaches
 * ccol_max_elem_count rejects a plain value update for a key that it already
 * holds. It answers ccol_container_full instead of an update.
 */
/* slot_out has the meaning that it has for oa_insert_impl(). key_slot_out,
 * when not NULL, receives the accessor of the stored key of the same entry
 * wherever slot_out receives its value accessor. Every caller passes a
 * constant for both, so each instantiation keeps only its own arms. */
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

  // The detection of the adaptive hash; see the section on adaptive hashing.
  // A switch rehashes the nodes in place, so new_node stays valid.
  if (__builtin_expect(walk >= SC_CHAIN_CAP, 0)) {
    sc_note_long_chain(map);
  }
  if (__builtin_expect(chmap_window_add(chmap_of_sc(map), walk), 0)) {
    sc_window_end(map);
  }

  if (map->elem_count >= map->elem_count_to_scale_up) {
    sc_scale(map, true);
  }

  // A scale relinks the nodes and never moves one, so the accessor of the
  // new node stays valid across it.
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

static ccol_retval_t sc_insert_or_get(sep_chain_map* map,
                                      const cmap_pair* key_pair,
                                      const cmap_pair* val_pair,
                                      const cmap_pair** slot_out) {
  return sc_insert_impl(map, key_pair, val_pair, slot_out, NULL);
}

static ccol_retval_t sc_insert_or_get_entry(sep_chain_map* map,
                                            const cmap_pair* key_pair,
                                            const cmap_pair* val_pair,
                                            const cmap_pair** slot_out,
                                            const cmap_pair** key_slot_out) {
  return sc_insert_impl(map, key_pair, val_pair, slot_out, key_slot_out);
}

/* Looks up key_pair and sets *val_pair to point at the value accessor of the
 * node. The pointer stays valid until a delete of that key, or until an
 * update of its value to a different size. */
static ccol_retval_t sc_get(sep_chain_map* map, const cmap_pair* key_pair,
                            const cmap_pair** val_pair) {
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

/* Deletes the entry that matches key_pair from the chain of its bucket. It
 * scales the bucket array down when elem_count falls below
 * elem_count_to_scale_down and the array is large enough to shrink. */
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

/* Destroys every bucket chain and frees the bucket array unless it is the
 * one in the block of the map. The state itself lives in that block, which
 * __chmap_destroy frees. The function passes the root of the insertion-order
 * dllist (&map->all_elems) to the teardown of each chain, which detaches each
 * node from that list before it frees the node. The root therefore never
 * names a freed node. */
static void sc_destroy(sep_chain_map* map) {
  // Locals, because each free is an opaque call after which the compiler
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

/* Clears every entry. It can also resize the bucket array to
 * new_bucket_array_size. Pass 0 to keep the current size. */
static ccol_retval_t sc_reset(sep_chain_map* map,
                              size_t new_bucket_array_size) {
  {
    // Locals, for the reason that sc_destroy gives.
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
    /* The code forms the byte count here. It does not leave that to the
       allocator, which cannot check a product that arrives already
       multiplied. chmap_reset accepts any power of two up to
       ccol_max_elem_count. That many pointers overflow size_t on every
       supported target. A product that wraps to zero is the dangerous one.
       realloc is then free to release the block and to return NULL. The
       recovery path below would then write through a pointer that the
       allocator already reclaimed. The code reports a failed allocation,
       which is what this is. Every element above is still destroyed, which
       matches what chmap_reset documents. */
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
      // The array in the block has this length, and the map is not using it,
      // because the current length differs. Taking it back cannot fail.
      _ccol_mem_free(map->m_procs, orig);
      map->bucket_arr = inline_arr;
    } else if (orig == inline_arr) {
      // The array in the block is part of the block, so it cannot be
      // reallocated. A new array replaces it, and it stays in the block.
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
   * iterator that a loop left early after the caller has already destroyed
   * the map, and with it the procs struct that the map owns. Freeing the
   * iterator must therefore never read the map. */
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

/* Creates a new hash map. The function chooses the implementation at creation
 * time, from the key type and the value type. The implementation is
 * open-addressing or separate chaining.
 *
 * The map is one block; see struct chashmap. A separate-chaining map whose
 * first bucket array has at most minimum_allowed_bucket_array_size buckets
 * keeps that array in the block too, so creating it takes one allocation. An
 * open-addressing map allocates its table on its own, because every resize
 * replaces it.
 *
 * When the caller gives a custom allocator, the map keeps a pointer to a
 * procs struct that lives as long as the map. A pointer that
 * ccol_procs_intern() gave lives for the whole process, so the map keeps that
 * pointer itself. Any other procs struct is copied into the block, so the
 * caller can free its own copy at once.
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
    if (err) *err = CCOL_ERR_STR("initial_bucket_array_size is zero");
    return NULL;
  }

  if (custom_key_equality_proc && !custom_hashing_proc) {
    // The built-in hash reads the bytes of a key, or its value for a
    // floating type. A custom equality that treats two different byte images
    // as one key therefore disagrees with it, and a lookup misses a key that
    // is present. Such a map is refused here rather than at the first miss.
    if (err) {
      *err = CCOL_ERR_STR(
          "custom_key_equality_proc needs a custom_hashing_proc that agrees "
          "with it");
    }
    return NULL;
  }

  if (!ccol_verify_memmgmt_procs(mmgmt_procs, err)) {
    return NULL;
  }

  // A custom key equality selects separate chaining. The open-addressing
  // probe compares a key as one 64-bit integer, and it keeps that loop free
  // of an indirect call that a map without the procedure would still pay a
  // branch for.
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
        *err = CCOL_ERR_STR("Initial bucket array size is too big");
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
    if (err) *err = CCOL_ERR_STR("Failed to allocate chashmap");
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

  // The secret is chosen before the first hash of any map; see
  // chmap_process_hash_secret.
  (void)chmap_process_hash_secret();

  if (open_addressing) {
    chm->impl_type = IMPL_OPEN_ADDRESSING;
    chm->impl.oa = &chm->state.oa;
    if (!oa_init(chm->impl.oa, initial_bucket_array_size, key_type, val_type,
                 get_type_size(key_type), get_type_size(val_type), m_procs,
                 custom_hashing_proc)) {
      if (err) *err = CCOL_ERR_STR("Failed to create open addressing map");
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
      if (err) *err = CCOL_ERR_STR("Failed to create separate chaining map");
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

/* Gives the key type that the caller made this map with. The backend does not
 * matter. */
static inline ccol_data_type chmap_key_type(chmap chm) {
  return chm->impl_type == IMPL_OPEN_ADDRESSING ? chm->impl.oa->key_type
                                                : chm->impl.sc->key_type;
}

/* Gives the value type that the caller made this map with. The backend does
 * not matter. */
static inline ccol_data_type chmap_val_type(chmap chm) {
  return chm->impl_type == IMPL_OPEN_ADDRESSING ? chm->impl.oa->val_type
                                                : chm->impl.sc->val_type;
}

/* Without this function, the key equality of the map is bitwise; see
 * oa_keys_equal and sc_compare_keys. +0.0 and -0.0 would then hash to
 * different buckets and compare unequal, although `0.0 == -0.0` in C. That is
 * surprising for a float key type and a double key type. For every other
 * numeric key type, the bitwise equality already agrees with the C equality.
 * When key_type is ccol_float or ccol_double, this function copies key_pair
 * into out_canon_pair and out_canon_buf. It changes a negative-zero bit
 * pattern to a positive zero, and it gives out_canon_pair. In every other
 * case it gives key_pair unchanged. It leaves every other bit pattern
 * unchanged. That includes each of the NaN payloads, which are never equal to
 * anything under `==`, not even to themselves. The key equality of this map
 * for a floating-point key is therefore bitwise equality with the signed zero
 * collapsed. It is not IEEE-754 equality. out_canon_buf must hold at least
 * key_pair->size bytes. ccol_float and ccol_double are always 8 bytes or
 * less, so a uint64_t is enough for every key that this function changes. */
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

/* The idea of key identity of the map. It is available to a module that
 * must agree with the map on which keys are the same key. It is the hash of
 * the keyed mode, whatever the mode of any map is, because key identity does
 * not depend on the mode. The change to a
 * canonical form runs first. That is what makes the answer an identity and
 * not a representation. -0.0 and 0.0 are one key for a float and for a
 * double. The function also takes a long double apart by value, so it never
 * reads the padding bytes of that value. See chashkey.h. */
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

/* The open-addressing backend stores a key-value pair inline, in the fixed
 * 8-byte key_data field and val_data field of a slot; see oa_slot. The memcpy
 * calls of oa_insert trust key_pair->size and val_pair->size completely. They
 * have no bounds check of their own. map->key_size and map->val_size are
 * fixed from key_type and val_type at creation. They are 8 bytes or less only
 * for the open-addressing backend. A size from the caller that does not match
 * them would silently overwrite the key bytes and value bytes of the
 * neighbouring slot. It would corrupt or lose an unrelated entry that is
 * already stored. It would also overflow the slot array outright when the
 * target slot sits near the end of that array. The rejection of a size that
 * does not match, up front, is what makes all of that structurally
 * unreachable and not merely unlikely. */
static inline bool oa_val_size_matches(const open_addr_map* map,
                                       size_t val_size) {
  return val_size == map->val_size;
}

/* Answers whether key_size is safe to give to hash_key_data(). For a float
 * key_type, a double key_type or a long double key_type, it also answers
 * whether key_size is safe for canonicalize_key_pair_if_needed(),
 * hash_long_double_value() and long_double_keys_equal(). All of those
 * functions dispatch on key_type. Each one then reads a FIXED number of bytes
 * straight out of the pointer of the caller. That number is the sizeof() of
 * the C type. This holds for every type that is_type_integral() knows: char,
 * short, int, long, long long, their unsigned equivalents, float, double and
 * a pointer. It also holds for ccol_long_double, which this function checks
 * deliberately. is_type_integral() itself excludes long double, but only for
 * the choice of the backend; see should_use_open_addressing. A long double
 * key always goes to separate chaining, and the code still hashes it and
 * compares it with a fixed-size read of sizeof(long double). That is the same
 * class of hazard as every other fixed-width type here. The key_size
 * parameter itself matters only for a key type that genuinely has no fixed
 * width, such as ccol_string and ccol_other_types. SipHash-1-3 always
 * hashes such a key with exactly the key_size of the caller.
 *
 * This check must therefore run for BOTH backends whenever key_type has a
 * fixed width. It is not only for open-addressing. A caller can give a
 * cmap_pair through the raw chmap_insert_elem, chmap_get_elem_ref or
 * chmap_delete_elem layer with a key_size smaller than the true size of the
 * type. Without this check, hash_key_data(),
 * canonicalize_key_pair_if_needed(), hash_long_double_value() and
 * long_double_keys_equal() then read past the end of the buffer of that
 * caller. This is reachable on the separate-chaining backend whenever a
 * fixed-width key type goes together with a value type that is not integral
 * or that is larger than 8 bytes. Such a value type forces separate chaining
 * whatever the key type is; two examples are int->char* and double->char*.
 * For long double it is reachable always, because a long double forces
 * separate chaining on its own. It is not a concern only for
 * open-addressing, and it is not theoretical. The open-addressing backend
 * always has a key_type that is_type_integral() accepts, by construction; see
 * should_use_open_addressing. This one check therefore also covers the own
 * hazard of that backend, which is a size that does not match and that
 * corrupts a neighbouring slot. No separate check for open-addressing alone
 * is necessary beside it.
 *
 * The set of fixed-width key types, and the expected size of each one, comes
 * from ccol_fixed_width_data_type_size() in common.h. It does not come from
 * is_type_integral() or get_type_size() above. cbstmap enforces the identical
 * rule on its own key_pairs, and the two modules must agree on exactly which
 * types the rule covers. The local get_type_size() is not a substitute. It
 * answers a different question, which is how wide an open-addressing slot
 * must be. It also reports 8 deliberately for a type with no fixed width at
 * all. That would turn every ccol_string key that is not 8 bytes long into a
 * false rejection. */
static inline bool key_size_matches_type_if_fixed_width(ccol_data_type key_type,
                                                        size_t key_size) {
  size_t fixed_width = ccol_fixed_width_data_type_size(key_type);
  return fixed_width == 0 || key_size == fixed_width;
}

/* Answers whether val_size agrees with the declared value type of the map,
 * for a value type that has one fixed width.
 *
 * oa_val_size_matches() above answers the same question for the
 * open-addressing backend, against the exact field that its memcpy targets.
 * That check is a memory-safety requirement of that backend, and it stays.
 * This one covers the SEPARATE-CHAINING backend, which has no such
 * requirement: it allocates exactly val_pair->size bytes for whatever it
 * gets, so a value of the wrong width can never run past its own storage.
 *
 * It is still wrong, and the failure lands on the wrong call site without
 * this check. A node that holds two bytes for a map whose declared value
 * type is four bytes accepts the insert and reports success. The value-size
 * guard of chmap_get() then stops the process on the first READ of that key,
 * at a call site that did nothing wrong, with a message that names
 * chmap_get() and suggests a missing chmap_redeclare(). This check moves the
 * rejection to the call that actually built the bad pair.
 *
 * Only the raw chmap_insert_elem() layer can reach it. The type-inferred
 * chmap_insert() macro converts the value through the declared value type of
 * the map first, so every pair that it builds is exactly sizeof(ValT) bytes
 * wide by construction.
 *
 * A value type with no fixed width returns 0 from
 * ccol_fixed_width_data_type_size() and is never checked. ccol_string is such
 * a type, and so is ccol_other_types, which covers a struct of the caller.
 * Both legitimately carry a different size for every entry. */
static inline bool val_size_matches_type_if_fixed_width(ccol_data_type val_type,
                                                        size_t val_size) {
  size_t fixed_width = ccol_fixed_width_data_type_size(val_type);
  return fixed_width == 0 || val_size == fixed_width;
}

/* The checks and the dispatch that an insert and an update share.
 * slot_out has the meaning that it has for oa_insert_impl(), and
 * key_slot_out the meaning that it has for sc_insert_impl(). A caller passes
 * a key_slot_out only together with a slot_out, and only for a
 * separate-chaining map. */
static inline __attribute__((always_inline)) ccol_retval_t
chmap_insert_elem_impl(chmap chm, const cmap_pair* key_pair,
                       const cmap_pair* val_pair, const cmap_pair** slot_out,
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
    return sc_insert_or_get_entry(chm->impl.sc, key_pair, val_pair, slot_out,
                                  key_slot_out);
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

/* The public dispatch for an insert and for an update. It checks the inputs,
 * then it calls the insert function of the backend. */
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair* key_pair,
                                const cmap_pair* val_pair) {
  return chmap_insert_elem_impl(chm, key_pair, val_pair, NULL, NULL);
}

/* One hash and one probe that insert a new key or find the key that is
 * already present, and hand back the value accessor of its entry either way.
 * See chashinsert.h. */
ccol_retval_t ccol_chmap_insert_or_get_elem(chmap chm,
                                            const cmap_pair* key_pair,
                                            const cmap_pair* val_pair,
                                            const cmap_pair** val_slot) {
  if (!val_slot) {
    return ccol_invalid_args;
  }
  *val_slot = NULL;
  return chmap_insert_elem_impl(chm, key_pair, val_pair, val_slot, NULL);
}

/* The ordered cursor of chashinsert.h. An entry reference is the address of
 * the dllist_refs member of a separate-chaining node, so each step is one
 * load of prev and no allocation. */
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

/* The insert-or-get above that also hands back the accessor of the stored
 * key. Only a separate-chaining entry has one; see chashinsert.h. */
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
  return chmap_insert_elem_impl(chm, key_pair, val_pair, val_slot, key_slot);
}

/* Gives the {ptr, size} accessor of the map for the entry of key_pair.
 * The accessor stays valid until the next operation that changes this key.
 * Its target is const. A caller can therefore read the accessor and edit the
 * bytes that it describes. An assignment to either field is a compile error.
 * The two fields describe one another, and the map cannot own a pointer that
 * it did not allocate. */
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair* key_pair,
                                 const cmap_pair** val_pair) {
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
    return oa_get(chm->impl.oa, key_pair, val_pair);
  } else {
    return sc_get(chm->impl.sc, key_pair, val_pair);
  }
}

/* Looks up key_pair and copies as many as target_buf_size bytes of the value
 * into target_buf. When the stored value is smaller than target_buf_size, the
 * function sets the remaining bytes to zero. */
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair* key_pair,
                                  void* target_buf, size_t target_buf_size) {
  if (!target_buf || target_buf_size == 0) {
    return ccol_invalid_args;
  }
  const cmap_pair* val_pair = NULL;
  ccol_retval_t ret = chmap_get_elem_ref(chm, key_pair, &val_pair);
  if (ret == ccol_success && val_pair) {
    size_t copy_size =
        val_pair->size < target_buf_size ? val_pair->size : target_buf_size;
    memcpy(target_buf, val_pair->ptr, copy_size);
    if (val_pair->size < target_buf_size) {
      memset((uint8_t*)target_buf + val_pair->size, 0,
             target_buf_size - val_pair->size);
    }
  }
  return ret;
}

/* The public dispatch for a delete. It checks the inputs, then it calls the
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
    ccol_assert(false);
  }

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->count;
  } else {
    return chm->impl.sc->elem_count;
  }
}

/* Clears every entry. It can also resize the internal bucket array. The
 * function rounds the size up to the nearest power of two, and it clamps the
 * size to minimum_allowed_bucket_array_size. Pass 0 to keep the current
 * size. */
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
        _ccol_find_nearest_gte_power_of_two(new_bucket_array_size);
    if (new_bucket_array_size > ccol_max_elem_count) {
      // The size that the caller asked for is too big. The code falls back
      // to 0, which keeps the current capacity. It does not skip the reset.
      // This failure path therefore still obeys the documented contract:
      // the reset destroys every element whatever the return value is. The
      // code does not silently leave every element in place.
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

/* Makes an iterator that points at the first element. A separate-chaining map
 * iterates through the insertion-order dllist. An open-addressing map finds
 * the first occupied slot that is not deleted with a linear scan. The
 * function gives NULL for an empty map. The iterator allocates
 * chmap_cmap_iterator on the heap. */
static cmap_iterator* chmap_iter_next(cmap_iterator* iter);

#ifdef RUNNING_UNIT_TESTS
/* A white-box regression guard. It tracks the NET count of
 * chmap_cmap_iterator allocations that chashmap_begin_iter() makes and that
 * __chmap_iterator_destroy() does not free yet. The count covers every chmap
 * in the process.
 *
 * valgrind on its own is not a dependable guard for this balance.
 * chashmap_begin_iter() gives back a pointer to a field inside the iterator
 * struct. It does not give the base address of the struct itself. valgrind
 * therefore reports a live and perfectly valid iterator as "possibly lost",
 * which is its classification for a live reference through an interior
 * pointer. That report is also noisy under concurrent load, and it is absent
 * on most runs.
 *
 * This counter is the always-on and deterministic alternative. Any regression
 * in the allocate and free balance of ANY chashmap iterator, anywhere in the
 * process, gives a loud and immediate abort() the moment it happens. See
 * _check_chmap_iter_balance_at_exit in tests/ctls/tests.c. A valgrind report
 * for the same problem is rare and hard to reproduce. */
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
        *err = CCOL_ERR_STR("Failed to allocate iterator");
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

    // Find first occupied slot
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
        *err = CCOL_ERR_STR("Failed to allocate iterator");
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

/* Frees the iterator struct that chashmap_begin_iter allocated, with the
 * free function that the iterator copied at creation. It reads nothing of
 * the map, so it stays valid after the map is destroyed. */
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
      ccol_mem_free(real_iter);
    }
  }
}

/* Moves the iterator to the next element. For separate chaining, the function
 * follows the next pointer of the dllist. For open-addressing, it scans the
 * slot array linearly for the next occupied slot that is not deleted. At the
 * end, it destroys the iterator and gives NULL. */
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

    // Find next occupied slot
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

/* Destroys the backend map below and the copy of m_procs that the map owns.
 * It then frees the top-level chashmap struct. When the caller gave a custom
 * allocator, that allocator made chm, so the function frees chm with the same
 * free function. The code copies that function into the free_func local
 * before it frees procs. The function pointer therefore stays valid after the
 * procs struct itself is gone. */
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
      // The procs struct can be part of the block, so the free function is
      // read before the block goes.
      ccol_free_t free_func = procs->free;
      free_func(chm);
      return;
    }
    ccol_mem_free(chm);
  }
}

/* See the doc comment of this function in chashmap.h. The function is
 * identical to __chmap_destroy() above, with one difference. It threads a
 * destructor callback into the same walk below. oa_destroy() and sc_destroy()
 * already make that walk silently for their own backend, and that walk
 * allocates nothing. The two walks below are deliberately NOT moved into
 * oa_destroy() and sc_destroy() themselves. The loop here is a duplicate on
 * purpose. This function exists only when val_dtor is not NULL. The
 * duplicate keeps its cost away from the hot path with no destructor, which
 * every other user of the map takes.
 *
 * The destructor gets the accessor of the entry through a const cmap_pair*.
 * chmap_get_elem_ref() reports one in the same shape. The accessor describes
 * storage that the map is about to free. A destructor therefore reads it and
 * frees what the value itself owns. An assignment to either field is a
 * compile error. On the separate-chaining side, the accessor is the live one
 * of the node. The rule is therefore not only about the stack of this
 * function. */
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
/* Gives the current size of the bucket array or the slot array. White-box
 * unit tests use it to check the resize thresholds of both backends. It is
 * not part of the public API. */
size_t chmap_get_bucket_arr_size(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->capacity;
  } else {
    return chm->impl.sc->bucket_arr_size;
  }
}

/* Gives the element count that starts the next scale-up. */
size_t chmap_get_elem_count_to_scale_up(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    size_t cap = chm->impl.oa->capacity;
    return (cap / 10) * 7 + ((cap % 10) * 7) / 10;
  } else {
    return chm->impl.sc->elem_count_to_scale_up;
  }
}

/* Gives the element count that starts the next scale-down. */
size_t chmap_get_elem_count_to_scale_down(chmap chm) {
  if (!chm) return 0;

  if (chm->impl_type == IMPL_OPEN_ADDRESSING) {
    return chm->impl.oa->capacity / 4;
  } else {
    return chm->impl.sc->elem_count_to_scale_down;
  }
}

/* The SipHash state of the 128-bit key (k0, k1), as a map keeps it. */
void chashmap_sip_state_for_tests(uint64_t k0, uint64_t k1, uint64_t sip_v[4]) {
  chmap_sip_state_from_key(k0, k1, sip_v);
}

/* SipHash-1-3 of a buffer from an explicit state. A test compares it with an
 * independent byte-wise implementation of the algorithm. */
uint64_t chashmap_siphash13_for_tests(const uint64_t sip_v[4],
                                      const void* key_ptr, size_t key_size) {
  return siphash13(sip_v, key_ptr, key_size);
}

/* The byte hash of the map from an explicit state. A test uses the state of
 * a key that anybody can know to build keys that collide for that key. */
size_t chashmap_byte_hash_for_tests(const uint64_t sip_v[4],
                                    const void* key_ptr, size_t key_size) {
  return hash_bytes_keyed(sip_v, key_ptr, key_size);
}

/* The secret that a map hashes with: the four words of the SipHash state,
 * then int_seed, then byte_seed. A map of the open-addressing backend reads
 * neither byte hash, so it gives 0 for the SipHash state and for byte_seed. */
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

/* The four key words that the fallback path derives when getrandom(2)
 * cannot answer. */
void chashmap_fallback_hash_key_for_tests(uint64_t out[4]) {
  chmap_fallback_hash_key(out);
}

/* XXH64 of a buffer with an explicit seed. A test compares it with the
 * published vectors and with an independent byte-wise implementation. */
uint64_t chashmap_xxh64_for_tests(const void* key_ptr, size_t key_size,
                                  uint64_t seed) {
  return xxh64(key_ptr, key_size, seed);
}

/* Whether the map hashes with the keyed mode. */
bool chashmap_is_keyed_for_tests(chmap chm) {
  return chm && chm->hash_mode == CHMAP_HASH_KEYED;
}

/* Switches the map to the keyed mode through the same function that the
 * detection calls. Gives ccol_not_enough_memory when the rebuild of an
 * open-addressing table cannot allocate, with the map still fast. */
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

/* The cap on one insert into a fast open-addressing table of capacity
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
 * the scale-up threshold of that size, whose window started at start_count
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

/* The bound of the current insert window of a map. */
uint32_t chashmap_window_bound_of_for_tests(chmap chm) {
  return chm ? chm->window_bound : 0;
}

/* The running sum of the insert window of a map. */
uint32_t chashmap_insert_window_for_tests(chmap chm) {
  return chm ? chm->insert_window : 0;
}

/* The length of the longest bucket chain of a separate-chaining map, or 0
 * for a map of the other backend. It is the cost of the worst lookup. */
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

/* The hash that a separate-chaining map stored for the entry whose key bytes
 * equal key_ptr, or 0 when there is none. */
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

/* The home slot of a key of an open-addressing map, or SIZE_MAX for a map of
 * the other backend. A test uses it to pick keys that form a cluster at a
 * chosen place, such as one that wraps past the last slot. */
size_t chashmap_oa_home_slot_for_tests(chmap chm, const void* key_ptr,
                                       size_t key_size) {
  if (!chm || chm->impl_type != IMPL_OPEN_ADDRESSING) return SIZE_MAX;
  open_addr_map* map = chm->impl.oa;
  return hash_index_for(
      hash_key_data(key_ptr, key_size, map->key_type, map->custom_hashing_proc,
                    oa_keyed(map), g_chmap_hash_secret.int_seed),
      map->capacity);
}

/* Checks the structure of an open-addressing map. Every occupied slot must be
 * reachable from its home slot through occupied slots only, its accessor
 * must describe that slot, and the occupied slots must number count. Gives
 * true for a map of the other backend. */
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
    if (map->val_accessors[j].ptr != &map->slots[j].val_data ||
        map->val_accessors[j].size != map->val_size) {
      return false;
    }
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
