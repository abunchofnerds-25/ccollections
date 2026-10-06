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

#pragma once

#include <string.h>

#include "citerators.h"
#include "common.h"

/* Every declaration from here to the end of this header is part of the public
 * ABI of libccollections. The shared library exports all of them. The build
 * of the library uses -fvisibility=hidden. A function or object that no such
 * block covers therefore stays internal to the library. It is absent from the
 * dynamic symbol table of the library. A symbol of the same name in the
 * application that links against the library cannot interpose it and cannot
 * collide with it. */
#pragma GCC visibility push(default)

/**
 * @file chashmap.h
 * @brief Hash map (dictionary). It resizes itself and has two implementation
 * strategies.
 *
 * This module gives you a hash map. The map selects one of two strategies by
 * itself:
 *
 * **Open-addressing** (the map uses this when the key and the value are both
 * integral types of 8 bytes or less):
 * - Compact 16-byte slots. A slot holds an 8-byte key beside an 8-byte value
 * and nothing else. The occupied bit of each slot lives in a parallel byte
 * array. The map allocates that array as the tail of the same block. The
 * storage for the key and the value therefore keeps its natural alignment.
 * This lets you read it and write it in place, for example with chmap_get_ptr
 * - Linear probing, with a multiplicative hash of an integer that the map
 *   reads from its high bits; see the two hash modes below
 * - Backward-shift deletion: a delete moves the later entries of its cluster
 *   back, so the table never holds a deleted-slot marker
 * - Load factor thresholds: an insert doubles the table past 0.70; a delete
 *   halves it under 0.25
 * - Zero allocations for each entry (one contiguous array)
 * - Good cache locality and good memory efficiency
 * - Excludes long double, which can be more than 8 bytes on some
 * architectures
 *
 * **Separate chaining** (the map uses this for a type that is not integral,
 * or for a type of more than 8 bytes):
 * - Linked lists that resolve a collision
 * - Small String Optimization (SSO): 23 bytes of inline storage for a key and
 * for a value
 * - Doubly-linked list that keeps the reverse insertion order and drives
 *   iteration
 * - Minimum bucket array size: 16 (always a power of 2)
 * - Scale factor: 4x (grows to 4x the size, shrinks to 0.25x the size)
 * - Scale up threshold: (bucket_count + 1) * 1.5 elements
 * - Scale down threshold: (bucket_count + 1) / 8 elements
 *
 * Common features:
 * - The map selects its implementation from the key type and the value type
 * - Support for a custom hash function. The map passes its result through a
 * finalizer before it derives an index from it
 * - Two hash modes. Every map starts in the fast mode: a Fibonacci multiply
 * for an integral, float, double or pointer key, XXH64 with a secret seed
 * for a string or any other buffer, and the murmur3 finalizer for the result
 * of a custom hash. These spread ordinary keys well and cost little, but a
 * party that controls the keys can choose a set that collides under them.
 * Every insert of a new key therefore measures how far it had to go, and a
 * map whose keys collide more than a random function lets them switches to
 * the keyed mode: a mixer of two multiplies keyed with a secret seed for a
 * fixed-width key and for the result of a custom hash, and SipHash-1-3 with
 * a secret key for a buffer. Each growth of a keyed map tries the fast mode
 * again on the new table and keeps it when the keys spread under it. A
 * lookup never writes to the map. A long double key is hashed by value with
 * SipHash-1-3 in both modes. The secrets are drawn once for each process. See
 * chmap_create_full() for the thresholds and for what a party that controls
 * the keys can still force
 * - The order of an iteration of a separate-chaining map follows insertion.
 * The order of an open-addressing map follows its slots, so it depends on
 * the mode, and in the keyed mode it differs from one process to the next
 * - Type-inferred macros for the common operations
 * - Average complexity: O(1) for insert, for get and for delete
 *
 * Examples of the implementation selection:
 * - int->int, long->double, float->uint32_t: Open-addressing
 * - string->int, int->string, string->string, int->long double: Separate
 * chaining
 */

/** @brief The default first size of the bucket array */
#define CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE 16

/** @brief Opaque hash map structure */
typedef struct chashmap chashmap;

/** @brief Pointer to hash map (handle type) */
typedef chashmap *chmap;

/* ========================================================================== */
/*                         HASH MAP CREATION                                  */
/* ========================================================================== */

/**
 * @brief Create a hash map and set every option
 *
 * This function makes a new hash map. The caller gives the first bucket
 * count, the key type, the value type, custom memory management, a custom
 * hash function and a custom key equality function. The map selects its
 * implementation strategy by itself. It selects the strategy from the key
 * type and the value type. The strategy is open-addressing or separate
 * chaining.
 *
 * @param initial_bucket_array_size First number of buckets (minimum 16)
 * @param key_type Type of the keys. This selects the implementation strategy
 * @param val_type Type of the values. This selects the implementation
 * strategy
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc/free
 * @param custom_hashing_proc Custom hash function, or NULL for the default
 * @param custom_key_equality_proc Custom key equality function, or NULL for
 * the built-in comparison. A map that has one must also have a
 * custom_hashing_proc, and the two must agree: keys that the equality
 * function reports as equal must hash equally.
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails. The call
 * fails when custom_key_equality_proc is not NULL and custom_hashing_proc is
 * NULL.
 *
 * @note How the map selects the implementation:
 *       - Open-addressing: the key and the value are both integral types of
 *         8 bytes or less, and custom_key_equality_proc is NULL
 *       - Separate chaining: the key or the value is not integral, or is
 *         more than 8 bytes, or custom_key_equality_proc is not NULL
 * @note With no custom_key_equality_proc, a key of a type that the map does
 * not know, such as a struct, is equal to another key only when every byte of
 * the two is equal, and the default hash reads every byte too. That includes
 * the padding bytes of a struct. Such a key must therefore be built from a
 * fully zeroed object, for example with memset() or `= {0}` before the fields
 * are assigned. A key whose padding holds whatever was on the stack misses on
 * lookup. A custom_hashing_proc and a custom_key_equality_proc that read only
 * the fields remove that requirement.
 * @note The integral types are: char, short, int, long, long long (signed and
 * unsigned), float and double
 * @note long double is not available for open-addressing, because it can be
 * 10 to 16 bytes
 * @note The key type and the hash mode select the default hash. The backend
 * does not select it. In the fast mode, which every map starts in, an
 * integral key, a float key, a double key and a pointer key get a Fibonacci
 * multiply that the map reads from its high bits, a string key and any other
 * key that is like a buffer get XXH64 with a secret 64-bit seed, and the
 * result of a custom_hashing_proc goes through the murmur3 finalizer. In the
 * keyed mode they get a mixer of two multiplies that starts from an exclusive
 * or with a secret 64-bit seed, SipHash-1-3 with a secret 128-bit key, and
 * that same mixer. A long double key gets SipHash-1-3 by value in both
 * modes. A separate-chaining map with an integral key type gets the integer
 * hash for that key, exactly as an open-addressing map does; one example is a
 * double key together with a value that is not integral.
 * @note The map watches every insert of a new key and switches itself to the
 * keyed mode when its keys collide more than a random function lets them. An
 * open-addressing map switches when one insert lands more than its cap from its
 * home slot: 24 slots for each doubling of the table up to 4096 slots, and 288
 * slots from there on. A separate-chaining map switches when a new key meets 20
 * nodes in its chain. Both switch when 256 consecutive inserts of new keys cost
 * more in total than 256 times 5 * E(a) + 0.5 probes for an open-addressing
 * table of 4096 slots or more, 6 * E(a) + 0.5 for a smaller one, and 4.5 *
 * lambda + 0.5 nodes for separate chaining. E(a) = (1 / (1 - a)^2 - 1) / 2 is
 * the expected displacement of an insert at load a under a random function, and
 * lambda is the number of entries for each bucket; a and lambda are the highest
 * load that those 256 inserts can have reached. A shrink that merges clusters
 * or chains is held to the same caps. A lookup never writes to the map and
 * never switches it.
 * @note Until it switches, a party that controls the keys can make an
 * open-addressing insert cost about 26 probes on average at a load of 0.70
 * (about 31 in a table below 4096 slots), and one insert no more than the
 * cap; a separate-chaining insert meets about 7.3 nodes on average at 1.5
 * entries for each bucket, and never 20. A lookup or a delete of an
 * open-addressing map examines at most one slot more than the largest
 * displacement of any key in the table, which the cap bounds in the fast
 * mode, so a long run of keys whose home slots are consecutive, which no
 * insert pays for, costs a lookup no more than that either.
 * @note The switch rebuilds an open-addressing table at the same size; when
 * that allocation fails, the map stays fast and correct, the insert that
 * asked for the switch succeeds, and the next insert that asks for it tries
 * again. It rehashes the nodes of a separate-chaining map in place, with no
 * allocation, so every reference to a key or a value, the insertion order
 * and every iterator stay valid. A reset and a shrink keep the mode.
 * @note A growth of a keyed map tries the fast mode on the new table: the
 * doubled table of an open-addressing map, or the bucket array of a
 * separate-chaining map that grows by four. The map keeps the fast mode
 * unless that fill shows that the keys collide under it: an
 * open-addressing entry that would land more than the cap of the new table
 * from its home slot, a separate-chaining chain that would reach 20 nodes,
 * or displacements or chain walks that add up to more than the count plus
 * 64. The attempt stops at the first of these and the growth fills the same
 * new table with the keyed hash, so it allocates nothing more. For a map of
 * n entries a failed attempt inspects at most 2 * n + cap + 65 slots, or
 * hashes n nodes and walks at most n + 83 chain nodes, besides the keyed
 * fill; growth is geometric, so a map whose every growth fails pays a
 * constant amount of extra work for each insert. A map that returns to the
 * fast mode is judged by its inserts again, so an attack that resumes
 * switches it within two windows of 256 inserts. Integer sets that cluster
 * under the multiply only at some sizes, such as multiples of 24, 32, 48 or
 * 96, switch at a small size and come back to the fast mode at a later
 * growth whose table spreads them well; at other sizes they cluster again
 * and the window switches the map back. The attempt keeps every entry, and
 * under separate chaining every reference to a key or a value, the insertion
 * order and every iterator, whether it succeeds or not. The keys that the map
 * treats as the same key are the same in both modes.
 * @note The secrets are random. The library draws them from getrandom(2)
 * once for each process, the first time that the process creates a map, and
 * mixes the clocks, the process id and randomized addresses when the kernel
 * cannot answer without blocking. A custom_hashing_proc that gives two keys
 * the same value makes them collide in both modes, because no finalizer can
 * tell them apart.
 * @note For a float key_type or a double key_type, the map always changes a
 * -0.0 key to 0.0. It does this before it hashes the key, stores it or
 * compares it. The two values therefore collide into one single key, exactly
 * as `-0.0 == 0.0` does in C. This is always true. It is also true when
 * custom_hashing_proc is not NULL. The custom function always gets the bit
 * pattern that the map already changed, never a raw -0.0. The map stores that
 * changed value, and an iteration later reads that same value. The map does
 * not store the exact bit pattern that the caller gave to chmap_insert.
 * @note A long double key_type always uses separate chaining. See the note
 * above about long double and open-addressing. For this key type, key
 * equality and the hash come from the numeric VALUE of the key. They do not
 * come from the raw bytes of the key. Every other key type behaves
 * differently. `-0.0L` and `0.0L` collide into one single key, exactly as
 * `-0.0` and `0.0` do for a float and for a double. Two keys that hold the
 * identical value stay the same key even when their padding bits differ. The
 * platform defines those padding bits. They really can differ for a long
 * double, because the representation of a long double in memory is not fully
 * significant on most platforms. Every NaN long double collapses into one
 * single key. This is a deliberate difference from the policy for a float and
 * for a double, which keep a distinct NaN payload. The same padding forces
 * this difference. The map never reads a padding byte of a NaN long double,
 * because that byte is often genuinely uninitialized memory and not merely
 * unspecified content. The map does not use this value-based handling when
 * custom_hashing_proc is not NULL. The custom function then gets the raw
 * bytes of a long double key, padding included, in the same way as for any
 * other type. A custom hash function for a long double key type must
 * therefore be value-based itself. It must ignore the padding. For example,
 * it can hash the result of frexpl(). Without this, the bucket that the hash
 * selects does not agree with the always-value-based key equality of this
 * map. Two representations of the same numeric value can then select
 * different buckets. A lookup can then report the key as absent while the
 * key is present.
 * @note Destroy the map with chmap_destroy() after you finish with it
 *
 * @see chmap_create
 * @see chmap_create_mp
 * @see chmap_create_ch
 * @see chmap_destroy
 */
chmap chmap_create_full(size_t initial_bucket_array_size,
                        ccol_data_type key_type, ccol_data_type val_type,
                        ccol_memmgmt_procs_t *mmgmt_procs,
                        ccol_hashing_proc_t custom_hashing_proc,
                        ccol_key_equality_proc_t custom_key_equality_proc,
                        char **err);

/**
 * @brief Create a hash map with the default settings
 *
 * This is a convenient wrapper for chmap_create_full(). It uses the default
 * memory management and the default hash, which starts in the fast mode and
 * switches to the keyed mode on a flood; see chmap_create_full().
 *
 * @param initial_bucket_array_size First number of buckets
 * @param key_type Type of the keys. This selects the implementation strategy
 * @param val_type Type of the values. This selects the implementation
 * strategy
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails
 *
 * @note See chmap_create_full() for the details of the implementation
 * selection
 */
static inline __attribute__((always_inline)) chmap
chmap_create(size_t initial_bucket_array_size, ccol_data_type key_type,
             ccol_data_type val_type, char **err) {
  return chmap_create_full(initial_bucket_array_size, key_type, val_type, NULL,
                           NULL, NULL, err);
}

/**
 * @brief Create a hash map with custom memory management
 *
 * This is a convenient wrapper for chmap_create_full(). It uses custom memory
 * management but the default hash.
 *
 * @param initial_bucket_array_size First number of buckets
 * @param key_type Type of the keys. This selects the implementation strategy
 * @param val_type Type of the values. This selects the implementation
 * strategy
 * @param mmgmt_procs Custom memory management procedures
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails
 *
 * @note See chmap_create_full() for the details of the implementation
 * selection
 */
static inline __attribute__((always_inline)) chmap chmap_create_mp(
    size_t initial_bucket_array_size, ccol_data_type key_type,
    ccol_data_type val_type, ccol_memmgmt_procs_t *mmgmt_procs, char **err) {
  return chmap_create_full(initial_bucket_array_size, key_type, val_type,
                           mmgmt_procs, NULL, NULL, err);
}

/**
 * @brief Create a hash map with a custom hash function
 *
 * This is a convenient wrapper for chmap_create_full(). It uses a custom hash
 * function but the default memory management.
 *
 * @param initial_bucket_array_size First number of buckets
 * @param key_type Type of the keys. This selects the implementation strategy
 * @param val_type Type of the values. This selects the implementation
 * strategy
 * @param custom_hashing_proc Custom hash function
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails
 *
 * @note See chmap_create_full() for the details of the implementation
 * selection
 */
static inline __attribute__((always_inline)) chmap
chmap_create_ch(size_t initial_bucket_array_size, ccol_data_type key_type,
                ccol_data_type val_type,
                ccol_hashing_proc_t custom_hashing_proc, char **err) {
  return chmap_create_full(initial_bucket_array_size, key_type, val_type, NULL,
                           custom_hashing_proc, NULL, err);
}

/* ========================================================================== */
/*                         HASH MAP OPERATIONS                                */
/* ========================================================================== */

/**
 * @brief Get the number of elements in the map
 *
 * This function gives the number of key-value pairs that the hash map holds
 * now.
 *
 * @param chm The hash map to read
 *
 * @return Number of elements in the map
 *
 * @note The function asserts when chm is NULL
 * @note O(1) complexity
 */
size_t chmap_elem_count(chmap chm);

/**
 * @brief Remove every element, and resize the map if the caller asks for it
 *
 * This function removes every key-value pair from the map. It also destroys
 * every internal data structure. It can resize the array of buckets or slots
 * to a new size.
 *
 * @param chm The hash map to reset
 * @param new_bucket_array_size New capacity. Give 0 to keep the current size
 *
 * @return ccol_success when the call succeeds
 * @return ccol_not_enough_memory when the resize fails. The function still
 * removes every element
 *
 * @note The function destroys every element whatever the return value is
 * @note The array size stays the same when new_bucket_array_size is 0
 * @note The function sets the size to 16 when new_bucket_array_size is less
 * than 16
 * @note In every other case the function rounds the size UP. It rounds to the
 * nearest power of two that is >= new_bucket_array_size. This is a ceiling.
 * The function never rounds down.
 * @note The function asserts when chm is NULL
 *
 * @see chmap_destroy
 */
ccol_retval_t chmap_reset(chmap chm, size_t new_bucket_array_size);

/**
 * @brief Insert a key-value pair, or update it (upsert)
 *
 * This function inserts a new key-value pair into the map. If the key is
 * already present, the function updates the value instead. The map resizes
 * itself when the load factor goes above the threshold. The map copies the
 * key and the value into its own storage.
 *
 * @param chm The hash map to insert into
 * @param key_pair The key to insert. ptr and size must both be valid
 * @param val_pair The value to insert. ptr and size must both be valid
 *
 * @return ccol_success when the function inserts a genuinely new key
 * @return ccol_key_already_present when the key is already present and the
 * function updates its value in place. This is a successful upsert, not an
 * error. The caller must treat this value and ccol_success as success
 * @return ccol_invalid_args when a pointer is NULL or a size is 0. The
 * function also gives this value when key_pair->size does not match the byte
 * size of the key type exactly. That check applies when the key type is a
 * fixed-width numeric type, and it applies to every backend. The fixed-width
 * numeric types are char, short, int, long, long long, their unsigned
 * equivalents, float, double, long double and a pointer type. The function
 * also gives this value when the value type that the caller made the map
 * with is a fixed-width type and val_pair->size does not match its byte size
 * exactly. That check, too, applies to every backend. A value type with no
 * fixed width, which is ccol_string and ccol_other_types, is never checked,
 * because every entry of such a map carries its own size
 * @return ccol_container_full when the map reaches ccol_max_elem_count
 * @return ccol_not_enough_memory when an allocation fails
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe
 * @note Separate chaining: O(n) worst case for each bucket, where n is the
 * chain length
 * @note The map copies the key data and the value data. It does not point at
 * them
 * @note When the key is present, the function updates only the value. The key
 * stays unchanged. This path reports ccol_key_already_present and not
 * ccol_success. See above
 * @note Open-addressing: starts a resize at a load factor of 0.70
 * @note Separate chaining: starts a resize when elem_count >=
 * (bucket_count + 1) * 1.5
 * @note Scale factor is 2x for open-addressing, 4x for separate chaining
 *
 * @see chmap_get_elem_copy
 * @see chmap_get_elem_ref
 * @see chmap_delete_elem
 */
ccol_retval_t chmap_insert_elem(chmap chm, const cmap_pair *key_pair,
                                const cmap_pair *val_pair);

/**
 * @brief Get a copy of the value that belongs to a key
 *
 * This function copies the value for the given key into the buffer that the
 * caller gives. It copies min(value_size, target_buf_size) bytes. This lets
 * the two sizes differ.
 *
 * @param chm The hash map to search
 * @param key_pair The key to look up
 * @param target_buf The buffer that gets the copy of the value
 * @param target_buf_size Size of the target buffer
 *
 * @return ccol_success when the function finds the key and copies the value
 * @return ccol_invalid_args when a pointer is NULL or a size is 0. The
 * function also gives this value when key_pair->size does not match the byte
 * size of the key type exactly. That check applies when the key type is a
 * fixed-width numeric type, and it applies to every backend. The fixed-width
 * numeric types are char, short, int, long, long long, their unsigned
 * equivalents, float, double, long double and a pointer type
 * @return ccol_key_not_found when the key is not present
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe
 * @note Separate chaining: O(n) worst case for each bucket, where n is the
 * chain length
 * @note The function copies min(actual_value_size, target_buf_size) bytes
 * @note A buffer that is too small is safe. The function then makes a partial
 * copy
 * @note When target_buf_size is larger than the stored value, the function
 * sets the remaining bytes of target_buf to zero. It does not leave them
 * unchanged
 *
 * @see chmap_get_elem_ref
 * @see chmap_insert_elem
 */
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size);

/**
 * @brief Get a pointer to the value that belongs to a key
 *
 * This function gives a pointer to the value pair struct for the given key.
 * The pointer stays valid until something changes the map. An insert, a
 * delete or a resize changes the map.
 *
 * @param chm The hash map to search
 * @param key_pair The key to look up
 * @param val_pair Output parameter that gets the pointer to the value pair
 *
 * @return ccol_success when the function finds the key
 * @return ccol_invalid_args when a pointer is NULL or the key size is 0. The
 * function also gives this value when key_pair->size does not match the byte
 * size of the key type exactly. That check applies when the key type is a
 * fixed-width numeric type, and it applies to every backend. The fixed-width
 * numeric types are char, short, int, long, long long, their unsigned
 * equivalents, float, double, long double and a pointer type
 * @return ccol_key_not_found when the key is not present
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe
 * @note Separate chaining: O(n) worst case for each bucket, where n is the
 * chain length
 * @note An insert, a delete or a resize makes the returned pointer invalid. A
 * call to chmap_get_elem_ref, chmap_get or chmap_get_ptr for a different key
 * never does. The caller can hold two or more pointers for different keys at
 * the same time. Each one stays valid on its own
 * @note Do not free the returned pointer. The map owns it
 * @note The cmap_pair that the function gives back is the accessor of the map
 * for that entry. It describes the value. It is not part of the value. Its
 * target is const-qualified. The caller can read through val_pair->ptr. The
 * caller can also write to the bytes that it points at, inside
 * val_pair->size. But an assignment to val_pair->ptr or to val_pair->size is
 * a compile error. The two fields describe one another. The map cannot own a
 * pointer that it did not allocate. Use chmap_insert_elem() to replace a
 * value.
 * @note The out parameter is a const cmap_pair **. The caller must therefore
 * declare its own variable as const cmap_pair *. The address of a plain
 * cmap_pair * does not compile.
 *
 * @see chmap_get_elem_copy
 * @see chmap_insert_elem
 */
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair *key_pair,
                                 const cmap_pair **val_pair);

/**
 * @brief Delete a key-value pair from the map
 *
 * This function removes the given key and its value from the map. The map
 * resizes itself down when the load factor falls below the threshold.
 *
 * @param chm The hash map to delete from
 * @param key_pair The key to delete
 *
 * @return ccol_success when the function finds the key and deletes it
 * @return ccol_invalid_args when a pointer is NULL or the key size is 0. The
 * function also gives this value when key_pair->size does not match the byte
 * size of the key type exactly. That check applies when the key type is a
 * fixed-width numeric type, and it applies to every backend. The fixed-width
 * numeric types are char, short, int, long, long long, their unsigned
 * equivalents, float, double, long double and a pointer type
 * @return ccol_key_not_found when the key is not present
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe. A delete
 * moves later entries of the same cluster back into the gap, so it leaves no
 * deleted-slot marker, and every element reference is invalid after it
 * @note Separate chaining: O(n) worst case for each bucket, where n is the
 * chain length
 * @note The function frees the memory of the key and of the value
 * @note Open-addressing: starts a resize at a load factor of 0.25
 * @note Separate chaining: starts a resize when elem_count <
 * (bucket_count + 1) / 8
 * @note Scale factor is 0.5x for open-addressing, 0.25x for separate chaining
 * @note The function does not resize below the minimum threshold
 *
 * @see chmap_insert_elem
 * @see chmap_reset
 */
ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair *key_pair);

/* ========================================================================== */
/*                         HASH MAP ITERATION                                 */
/* ========================================================================== */

/**
 * @brief Start an iteration over the hash map
 *
 * This function makes an iterator that points at the first element. A
 * separate-chaining map iterates in the reverse insertion order, through the
 * doubly-linked list. An open-addressing map iterates in the slot order.
 *
 * @param chm The hash map to iterate over
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the iterator. The function returns NULL when the map is
 * empty or when an allocation fails
 *
 * @note Separate chaining: the iteration goes in the reverse insertion order.
 *       The element that the caller inserted last comes first. The iteration
 *       uses the doubly-linked list
 * @note Open-addressing: the iteration goes in the slot order, not the
 * insertion order
 * @note Destroy the iterator with chmap_iter_destroy(). The iterator also
 * destroys itself at the end of the iteration
 * @note A change to the map during the iteration makes the iterator invalid
 * @note The function treats a NULL chm in the same way as an empty map. It
 * returns NULL, and this is not an error. This behaviour is deliberate. A map
 * field that the program makes only when it needs it can stay uninitialized
 * while nothing goes into it. The caller can iterate such a field directly,
 * and no caller needs its own NULL guard first
 * @note The function returns NULL when the map is empty. This is not an error
 *
 * @see chmap_begin (macro wrapper)
 * @see chmap_iter_next
 * @see chmap_iter_destroy
 */
cmap_iterator *chashmap_begin_iter(chmap chm, char **err);

void __chmap_iterator_destroy(cmap_iterator *iter);

/* ========================================================================== */
/*                         HASH MAP DESTRUCTION                               */
/* ========================================================================== */

/**
 * @brief Destroy a hash map (internal function)
 *
 * @param chm The hash map to destroy
 *
 * @warning Do not call this function directly. Use the chmap_destroy() macro
 */
void __chmap_destroy(chmap chm);

/**
 * @brief Destroy a hash map. Call a destructor on every value first, and
 * allocate no memory to do it
 *
 * This function behaves like __chmap_destroy(), with one difference. When
 * val_dtor is not NULL, the function calls it one time for each live entry.
 * It gives the value of that entry to the destructor as a cmap_pair. It makes
 * this call immediately before it frees the entry. This function allocates
 * nothing at all. It walks the internal bucket storage and slot storage of
 * the map directly, in the same way as __chmap_destroy() does. The call to
 * the destructor is part of that same walk.
 *
 * This function is for a map whose values are owned pointers. Each such
 * pointer names a larger structure that needs its own recursive teardown. One
 * example is a dictionary of dictionaries. There is another way to destroy
 * such a map. The caller can walk it with chashmap_begin_iter(), run a value
 * destructor for each entry, and destroy the empty map at the end. That way
 * has a real gap under memory pressure. The small internal allocation of
 * chashmap_begin_iter() can fail. That way then has no route left to reach
 * and free the values that it still owns. This function has no such gap. It
 * always reaches every value, because it allocates nothing of its own.
 *
 * @param chm The hash map to destroy
 * @param val_dtor Destructor that the function calls one time for each live
 *   value, before it frees that entry. Give NULL for the same behaviour as
 *   __chmap_destroy()
 * @param dtor_ctx Opaque pointer that the function passes unchanged to every
 *   val_dtor call
 *
 * @note A call with a NULL chm is safe. It does nothing
 * @note The function does not set the chm handle to NULL. This matches the
 *   raw internal convention of __chmap_destroy()
 * @note val_dtor must not change chm. The map is in the middle of its
 *   teardown for the whole duration of this call
 * @note val_pair is the accessor of the map for that entry. Its target is
 *   const, in the same way as for chmap_get_elem_ref(). A destructor reads
 *   val_pair->ptr and val_pair->size. It then frees whatever the value itself
 *   owns. That is usually a pointer that the destructor copies out of those
 *   bytes. An assignment to val_pair->ptr or to val_pair->size is a compile
 *   error. The accessor describes storage that the map is about to free. The
 *   map also cannot own a pointer that it did not allocate
 *
 * @see __chmap_destroy
 * @see chashmap_begin_iter
 */
void chmap_destroy_with_dtor(chmap chm,
                             void (*val_dtor)(const cmap_pair *val_pair,
                                              void *dtor_ctx),
                             void *dtor_ctx);

/**
 * @brief Internal cleanup function that destroys a map automatically
 *
 * @param chm Pointer to a hash map pointer
 *
 * @note The _ccol_destructor attribute uses this function
 * @warning Do not call this function directly
 */
static inline void ___chmap_destroy(chmap *chm) {
  if (chm && *chm) {
    __chmap_destroy(*chm);
    *chm = NULL;
  }
}

/**
 * @brief Destroy a hash map and set the pointer to NULL
 *
 * This macro frees every resource of the hash map. This includes every key,
 * every value, every bucket and every internal structure.
 *
 * @param chm The hash map to destroy. The macro sets it to NULL
 *
 * @note A call with NULL is safe
 * @note The macro frees every key and every value
 * @note The macro destroys every collision chain of every bucket
 * @note The macro evaluates chm exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define chmap_destroy(chm)      \
  _ccol_chmap_destroy_impl(chm, \
                           _ccol_uniq(__ccol_chmap_destroy_slot, __COUNTER__))

/* Internal. The body of chmap_destroy. slot is a name from _ccol_uniq(), so
 * the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_chmap_destroy_impl(chm, slot) \
  do {                                      \
    __typeof__(chm) *slot = &(chm);         \
    __chmap_destroy(*slot);                 \
    *slot = NULL;                           \
  } while (0)

/* ========================================================================== */
/*                    TYPE-INFERRED CONVENIENCE MACROS */
/* ========================================================================== */

/**
 * @brief Turn on the type-inferred macros for a hash map that already exists
 *
 * This macro declares the type variables that the type-inferred macros need.
 * Use it for a hash map that another scope made.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 *
 * Example:
 * @code
 * void process(chmap map) {
 *   chmap_redeclare(map, int, char*);
 *   int key = 42;
 *   chmap_insert(map, key, "hello");
 * }
 * @endcode
 */
#define chmap_redeclare(hm_name, key_t, val_t)                              \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

/**
 * @brief Declare a hash map variable that is not initialized
 *
 * This macro declares a hash map variable. It also declares the type
 * variables that the type-inferred macros need. Initialize the map before you
 * use it.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note Initialize the map with chmap_init*() before you use it
 *
 * @see chmap_init
 * @see chmap_construct
 */
#define chmap_declare(hm_name, key_t, val_t)                              \
  __typeof__(key_t) *hm_name##__ccol_key_type_var                         \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  __typeof__(val_t) *hm_name##__ccol_val_type_var                         \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  chmap hm_name                /* deliberately not initialized to NULL */

#define chmap_declare_scoped(hm_name, key_t, val_t)                         \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL

/**
 * @brief Initialize a hash map and set every option
 *
 * This macro initializes a hash map that the caller declared before. It uses
 * custom memory management, a custom hash and a custom key equality. It calls
 * ccol_fatal_err() when the initialization fails. It reads the key type and
 * the value type from the type variables that chmap_declare makes.
 *
 * @param hm_name Hash map variable to initialize. It must be declared
 * @param mmgmt_procs Custom memory management procedures, or NULL
 * @param custom_hashing_proc Custom hash function, or NULL
 * @param custom_key_equality_proc Custom key equality function, or NULL. See
 * chmap_create_full() for the rule that ties it to custom_hashing_proc.
 *
 * @note The macro stops the program when the initialization fails
 * @note The macro uses CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (16)
 * @note The macro reads the key type and the value type by itself, and
 * selects the implementation from them
 *
 * @see chmap_declare
 * @see chmap_construct_full
 */
#define chmap_init_full(hm_name, mmgmt_procs, custom_hashing_proc,           \
                        custom_key_equality_proc)                            \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_full(                                             \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),    \
        &__ccol_chmap_err);                                                  \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare a hash map, initialize it, and set every option
 *
 * This macro joins the declaration and the initialization. It uses custom
 * memory management, a custom hash and a custom key equality. It calls
 * ccol_fatal_err() when the initialization fails. The caller names the key
 * type and the value type, and those two types select the implementation
 * strategy.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures, or NULL
 * @param custom_hashing_proc Custom hash function, or NULL
 * @param custom_key_equality_proc Custom key equality function, or NULL. See
 * chmap_create_full() for the rule that ties it to custom_hashing_proc.
 *
 * @note The macro stops the program when the initialization fails
 * @note The macro uses CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (16)
 * @note The macro selects the implementation by itself, from key_t and val_t
 *
 * @see chmap_init_full
 */
#define chmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,             \
                             custom_hashing_proc, custom_key_equality_proc)  \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_full(                                             \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),    \
        &__ccol_chmap_err);                                                  \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define chmap_construct_full_scoped(hm_name, key_t, val_t, mmgmt_procs,      \
                                    custom_hashing_proc,                     \
                                    custom_key_equality_proc)                \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                   \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_full(                                             \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),    \
        &__ccol_chmap_err);                                                  \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a hash map with the default settings
 *
 * This macro initializes a hash map that the caller declared before. It uses
 * the default settings. It reads the key type and the value type from the
 * type variables.
 *
 * @param hm_name Hash map variable to initialize
 *
 * @note The macro stops the program when the initialization fails
 * @note The macro uses the default memory management. It also selects the
 * hash by itself
 * @note The macro selects the implementation by itself, from the types that
 * it reads
 */
#define chmap_init(hm_name)                                                  \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create(                                                  \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        &__ccol_chmap_err);                                                  \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare a hash map and initialize it with the default settings
 *
 * This macro joins the declaration and the initialization. It uses the
 * default settings. The macro selects the implementation strategy by itself,
 * from the key type and the value type.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note The macro stops the program when the initialization fails
 * @note Open-addressing is for int->int, long->double, float->uint32_t and
 * other similar pairs of types
 * @note Separate chaining is for string->int, int->string, string->string and
 * other similar pairs of types
 *
 * Example:
 * @code
 * chmap_construct(ages, char*, int);      // string->int: separate chaining
 * int years = 30;
 * chmap_insert(ages, "Alice", years);
 * years = 25;
 * chmap_insert(ages, "Bob", years);
 * int age = chmap_get(ages, "Alice");     // age == 30
 * chmap_destroy(ages);
 *
 * chmap_construct(counters, int, int);    // int->int: open-addressing
 * int id = 1, hits = 100;
 * chmap_insert(counters, id, hits);
 * int count = chmap_get(counters, id);    // count == 100
 * chmap_destroy(counters);
 * @endcode
 */
#define chmap_construct(hm_name, key_t, val_t)                               \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create(                                                  \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        &__ccol_chmap_err);                                                  \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define chmap_construct_scoped(hm_name, key_t, val_t)                        \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                   \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create(                                                  \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        &__ccol_chmap_err);                                                  \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a hash map with custom memory management
 *
 * This macro initializes a hash map that the caller declared before. It uses
 * custom memory management. It reads the key type and the value type from the
 * type variables.
 *
 * @param hm_name Hash map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note The macro stops the program when the initialization fails
 * @note The macro uses the default hash. It selects that hash by itself, from
 * the types
 */
#define chmap_init_mp(hm_name, mmgmt_procs)                                  \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_mp(                                               \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (mmgmt_procs), &__ccol_chmap_err);                                   \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare a hash map and initialize it with custom memory management
 *
 * This macro joins the declaration and the initialization. It uses custom
 * memory management.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note The macro stops the program when the initialization fails
 */
#define chmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)               \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_mp(                                               \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (mmgmt_procs), &__ccol_chmap_err);                                   \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define chmap_construct_mp_scoped(hm_name, key_t, val_t, mmgmt_procs)        \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                   \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_mp(                                               \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (mmgmt_procs), &__ccol_chmap_err);                                   \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Initialize a hash map with a custom hash
 *
 * This macro initializes a hash map that the caller declared before. It uses
 * a custom hash function.
 *
 * @param hm_name Hash map variable to initialize
 * @param custom_hashing_proc Custom hash function
 *
 * @note The macro stops the program when the initialization fails
 * @note The macro uses the default memory management
 */
#define chmap_init_ch(hm_name, custom_hashing_proc)                          \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_ch(                                               \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (custom_hashing_proc), &__ccol_chmap_err);                           \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

/**
 * @brief Declare a hash map and initialize it with a custom hash
 *
 * This macro joins the declaration and the initialization. It uses a custom
 * hash function.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 * @param custom_hashing_proc Custom hash function
 *
 * @note The macro stops the program when the initialization fails
 */
#define chmap_construct_ch(hm_name, key_t, val_t, custom_hashing_proc)       \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                  \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;             \
  do {                                                                       \
    char *__ccol_chmap_err = NULL;                                           \
    hm_name = chmap_create_ch(                                               \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                              \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),        \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),        \
        (custom_hashing_proc), &__ccol_chmap_err);                           \
    if (!hm_name) {                                                          \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                        \
  } while (0)

#define chmap_construct_ch_scoped(hm_name, key_t, val_t, custom_hashing_proc) \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_ch(                                                \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (custom_hashing_proc), &__ccol_chmap_err);                            \
    if (!hm_name) {                                                           \
      ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,          \
                     __ccol_chmap_err ? __ccol_chmap_err : "unknown error");  \
    }                                                                         \
  } while (0)

/* ========================================================================== */
/*                    TYPE-INFERRED OPERATION MACROS */
/* ========================================================================== */

/**
 * @brief Insert a key-value pair (type-inferred)
 *
 * This is a type-inferred wrapper for chmap_insert_elem(). It builds the
 * cmap_pair for the key and the cmap_pair for the value from the expressions
 * that the caller gives. It calls ccol_fatal_err() when the insert fails.
 *
 * The macro converts key and val to the declared key type and the declared
 * value type of the map, in the same way as a plain C assignment, and it
 * stores the converted copies. It never reinterprets the bytes of an
 * expression of another type. An expression that has no implicit conversion
 * to the declared type is a compile error. A character-pointer key type or
 * value type keeps the pointer type of the expression instead, so a const
 * char * needs no cast.
 *
 * @param hm_name The hash map to insert into
 * @param key The key to insert
 * @param val The value for that key
 *
 * @note The macro stops the program when the insert fails
 * @note The macro takes the address of the key and of the value by itself
 * @note The macro is correct for a value type and for a string type
 *
 * @note A NULL key or value of a character-pointer type is not a string.
 * The macro stops the program with ccol_invalid_args for it
 *
 * @see chmap_insert_elem
 * @see chmap_get
 * @see chmap_remove
 *
 * Example:
 * @code
 * chmap_construct(map, int, char*);
 * int key = 42;
 * chmap_insert(map, key, "hello");
 * key = 100;
 * chmap_insert(map, key, "world");
 * @endcode
 */
#define chmap_insert(hm_name, key, __ccol_chmap_val)        \
  _ccol_chmap_insert_impl(                                  \
      hm_name, (key), (__ccol_chmap_val),                   \
      _ccol_uniq(__ccol_chmap_insert_key_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_chmap_insert_val_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_chmap_insert_kp, __COUNTER__),      \
      _ccol_uniq(__ccol_chmap_insert_vp, __COUNTER__),      \
      _ccol_uniq(__ccol_chmap_insert_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_chmap_insert_impl(                                              \
    hm_name, key, __ccol_chmap_val, __ccol_chmap_key_tmp,                     \
    __ccol_chmap_val_tmp, __ccol_chmap_kp, __ccol_chmap_vp, __ccol_chmap_r)   \
  do {                                                                        \
    /* Each temporary below holds a private copy of the expression that the   \
     * caller wrote. _ccol_declared_or_own_type() gives that copy the         \
     * DECLARED key type or value type of the map, so the initialization is   \
     * a plain C assignment and it converts the value through the conversion  \
     * rules of the compiler. cvec_push() uses the same construct for the     \
     * same reason.                                                           \
     *                                                                        \
     * A temporary of __typeof__(key) instead would copy the RAW BYTES of the \
     * caller expression into the map whenever the two types happen to have   \
     * the same width. A float that goes into a chmap_construct(m, char *,    \
     * int) would store the bit pattern of that float, and a later            \
     * chmap_get() would report 1069547520 for 1.5f. A key is worse: a float  \
     * key of 2.0f in an int-keyed map is then unreachable by any int lookup  \
     * for the life of the map. The widths match, so no size check anywhere   \
     * can see it, and no compiler diagnostic fires at any optimization       \
     * level. An expression whose type has no implicit conversion to the      \
     * declared type is a compile error here instead, which is the contract   \
     * that cvec_push() already documents.                                    \
     *                                                                        \
     * A narrower expression is the other half of the same rule. A short      \
     * that goes into an int-valued map becomes an int here, so the pair is   \
     * always exactly sizeof(ValT) bytes wide.                                \
     *                                                                        \
     * A declared type that is a character pointer keeps the type of the      \
     * caller expression instead; read _ccol_declared_or_own_type() in        \
     * common.h for why that case must not convert.                           \
     *                                                                        \
     * The copy also settles the lifetime question that                       \
     * _populate_cmap_pair() raises. That helper takes the address of its     \
     * argument from inside one of its own nested blocks, so a compound       \
     * literal that the caller writes would be created there and would die    \
     * with that block. These temporaries hold their own copy for the whole   \
     * call instead. The expression of the caller is still evaluated exactly  \
     * one time, and an rvalue is as acceptable as an lvalue.                 \
     */                                                                       \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))           \
        __ccol_chmap_key_tmp = (key);                                         \
    _ccol_clear_padding(&__ccol_chmap_key_tmp);                               \
    _ccol_declared_or_own_type(hm_name##__ccol_val_type_var,                  \
                               (__ccol_chmap_val)) __ccol_chmap_val_tmp =     \
        (__ccol_chmap_val);                                                   \
    cmap_pair *__ccol_chmap_kp = &(cmap_pair){};                              \
    cmap_pair *__ccol_chmap_vp = &(cmap_pair){};                              \
    _populate_cmap_pair(__ccol_chmap_kp, __ccol_chmap_key_tmp);               \
    _populate_cmap_pair(__ccol_chmap_vp, __ccol_chmap_val_tmp);               \
    ccol_retval_t __ccol_chmap_r =                                            \
        chmap_insert_elem(hm_name, __ccol_chmap_kp, __ccol_chmap_vp);         \
    if (__ccol_chmap_r != ccol_success &&                                     \
        __ccol_chmap_r != ccol_key_already_present) {                         \
      _ccol_dump_key_to_stderr(__ccol_chmap_kp->ptr, __ccol_chmap_kp->size);  \
      ccol_fatal_err("chmap_insert('%s'): r: %d (%s)", #hm_name,              \
                     __ccol_chmap_r, ccol_retval_to_str(__ccol_chmap_r));     \
    }                                                                         \
  } while (0)

/**
 * @brief Remove a key-value pair (type-inferred)
 *
 * This is a type-inferred wrapper for chmap_delete_elem(). It gives the result
 * code back to the caller.
 *
 * @param hm_name The hash map to remove from
 * @param key The key to remove
 *
 * @return ccol_success when the macro removes the pair. ccol_key_not_found
 * when the key is not present
 *
 * @note The macro does not stop the program for ccol_key_not_found
 * @note The macro frees the memory of the key and of the value
 *
 * @note A NULL key of a character-pointer type is not a string. The macro
 * returns ccol_invalid_args for it
 *
 * @see chmap_delete_elem
 * @see chmap_insert
 *
 * Example:
 * @code
 * int key = 42;
 * ccol_retval_t r = chmap_remove(map, key);
 * if (r == ccol_key_not_found) {
 *   printf("Key not found\n");
 * }
 * @endcode
 */
#define chmap_remove(hm_name, key)                                          \
  _ccol_chmap_remove_impl(                                                  \
      hm_name, (key), _ccol_uniq(__ccol_chmap_remove_key_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_chmap_remove_kp, __COUNTER__),                      \
      _ccol_uniq(__ccol_chmap_remove_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_chmap_remove_impl(hm_name, key, __ccol_chmap_key_tmp, \
                                __ccol_chmap_kp, __ccol_chmap_r)    \
  ({                                                                \
    /* The temporary carries the DECLARED key type of the map.      \
     * See chmap_insert() for why the lookup key goes through a     \
     * conversion and not through a raw byte copy. */               \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key)) \
        __ccol_chmap_key_tmp = (key);                               \
    _ccol_clear_padding(&__ccol_chmap_key_tmp);                     \
    cmap_pair *__ccol_chmap_kp = &(cmap_pair){};                    \
    _populate_cmap_pair(__ccol_chmap_kp, __ccol_chmap_key_tmp);     \
    ccol_retval_t __ccol_chmap_r =                                  \
        chmap_delete_elem(hm_name, __ccol_chmap_kp);                \
    __ccol_chmap_r;                                                 \
  })

/**
 * @brief Get a value by its key (type-inferred, gives the value)
 *
 * This is a type-inferred wrapper for chmap_get_elem_ref(). It gives the value
 * itself. It calls ccol_fatal_err() when the key is not present. It also
 * calls ccol_fatal_err() when the two sizes do not match.
 *
 * @param hm_name The hash map to search
 * @param key The key to look up
 *
 * @return The value for that key
 *
 * @note The macro stops the program when the key is not present
 * @note The macro stops the program when the size of the stored value does
 * not match the size of the declared value type
 * @note The macro gives the value, not a pointer
 * @note For a string, the macro gives the char* itself
 *
 * @note A NULL key of a character-pointer type is not a string. The macro
 * stops the program with ccol_invalid_args for it
 *
 * @see chmap_get_ptr
 * @see chmap_insert
 *
 * Example:
 * @code
 * chmap_construct(map, int, char*);
 * int key = 42;
 * chmap_insert(map, key, "hello");
 * char* val = chmap_get(map, key); // val == "hello"
 * @endcode
 */
#define chmap_get(hm_name, key)                                           \
  _ccol_chmap_get_impl(hm_name, (key),                                    \
                       _ccol_uniq(__ccol_chmap_get_key_tmp, __COUNTER__), \
                       _ccol_uniq(__ccol_chmap_get_val, __COUNTER__),     \
                       _ccol_uniq(__ccol_chmap_get_kp, __COUNTER__),      \
                       _ccol_uniq(__ccol_chmap_get_vp, __COUNTER__),      \
                       _ccol_uniq(__ccol_chmap_get_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_chmap_get_impl(hm_name, key, __ccol_chmap_key_tmp,               \
                             __ccol_chmap_val, __ccol_chmap_kp,                \
                             __ccol_chmap_vp, __ccol_chmap_r)                  \
  ({                                                                           \
    /* The temporary carries the DECLARED key type of the map. See             \
     * chmap_insert() for why the lookup key goes through a conversion and     \
     * not through a raw byte copy. A key that the map stored through that     \
     * conversion is only ever found again by a lookup that makes the same     \
     * one. */                                                                 \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        __ccol_chmap_key_tmp = (key);                                          \
    _ccol_clear_padding(&__ccol_chmap_key_tmp);                                \
    __typeof__(*hm_name##__ccol_val_type_var) *__ccol_chmap_val = NULL;        \
    cmap_pair *__ccol_chmap_kp = &(cmap_pair){};                               \
    const cmap_pair *__ccol_chmap_vp = NULL;                                   \
    _populate_cmap_pair(__ccol_chmap_kp, __ccol_chmap_key_tmp);                \
    ccol_retval_t __ccol_chmap_r =                                             \
        chmap_get_elem_ref(hm_name, __ccol_chmap_kp, &__ccol_chmap_vp);        \
    if (__ccol_chmap_r != ccol_success) {                                      \
      _ccol_dump_key_to_stderr(__ccol_chmap_kp->ptr, __ccol_chmap_kp->size);   \
      ccol_fatal_err("chmap_get('%s'): r: %d (%s)", #hm_name, __ccol_chmap_r,  \
                     ccol_retval_to_str(__ccol_chmap_r));                      \
    }                                                                          \
    if (ccol_is_char_ptr(*hm_name##__ccol_val_type_var)) {                     \
      __ccol_chmap_val = (__typeof__(*hm_name##__ccol_val_type_var) *)&(       \
          __ccol_chmap_vp->ptr);                                               \
    } else if (__ccol_chmap_vp->size != sizeof(*__ccol_chmap_val)) {           \
      ccol_fatal_err(                                                          \
          "chmap_get('%s'): value size mismatch - stored: %lu bytes, "         \
          "requested: %lu bytes; wrong type or missing chmap_redeclare()?",    \
          #hm_name, (unsigned long)__ccol_chmap_vp->size,                      \
          (unsigned long)sizeof(*__ccol_chmap_val));                           \
    } else {                                                                   \
      __ccol_chmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)(__ccol_chmap_vp->ptr); \
    }                                                                          \
    *__ccol_chmap_val;                                                         \
  })

/* This macro gives p unchanged for every value type except a string. For a
 * map whose value is a string, it gives a pointer whose target is
 * const-qualified.
 *
 * A map that stores strings owns the bytes. It also keeps its own
 * {ptr, size} accessor for the entry in step with those bytes. The only
 * char* object anywhere in the map is the ptr field of that accessor. The
 * chmap_get_ptr() of a map whose value is a string can therefore only give
 * back the address of that field. A store of a different char* through that
 * address replaces the pointer. The size then still describes the previous
 * string. Such a store also hands the map a pointer that the map does not own
 * and whose lifetime it cannot control. Without the const,
 * chmap_get_elem_copy() and every read of an iterator value then walk the new
 * buffer for the length of the previous string. That new buffer can be much
 * shorter. AddressSanitizer reports a global-buffer-overflow read of the old
 * length on the first such read.
 *
 * The const makes that store a compile error instead. The caller can still
 * read the stored char* through the returned pointer. The caller can still
 * write to the string bytes that it points at, inside the stored length. Use
 * chmap_insert() to replace a string value. It frees the old bytes and copies
 * the new bytes into storage that the map owns. */
#define _ccol_chmap_value_ptr_result(hm_name, p)                               \
  _Generic(*hm_name##__ccol_val_type_var,                                      \
      char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),          \
      const char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),    \
      signed char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p),   \
      const signed char *: (__typeof__(*hm_name##__ccol_val_type_var)          \
                                const *)(p),                                   \
      unsigned char *: (__typeof__(*hm_name##__ccol_val_type_var) const *)(p), \
      const unsigned char *: (__typeof__(*hm_name##__ccol_val_type_var)        \
                                  const *)(p),                                 \
      default: (p))

/**
 * @brief Get a pointer to a value by its key (type-inferred, gives a pointer or
 * NULL)
 *
 * This is a type-inferred wrapper for chmap_get_elem_ref(). It gives a pointer
 * to the value. It gives NULL when the key is not present. chmap_get() behaves
 * differently: this macro does not stop the program for a key that is not
 * present.
 *
 * @param hm_name The hash map to search
 * @param key The key to look up
 *
 * @return Pointer to the value, or NULL when the key is not present
 *
 * @note The macro gives NULL when the key is not present. It does not stop
 * the program
 * @note The macro stops the program when the size of the value does not match
 * the size of the type
 * @note The macro gives a pointer to the value, so the caller can change the
 * value in place
 * @note A later insert, delete or resize makes the pointer invalid
 * @note For a char* value type, the target of the pointer is const-qualified
 * (char *const *). The caller can read the stored string. The caller can also
 * edit its bytes in place, inside the stored length. But a replacement of the
 * pointer itself is a compile error. The map owns the string bytes and keeps
 * its own accessor in step with them. A replacement of the pointer would
 * leave the length of the accessor describing the previous string. Use
 * chmap_insert() to replace a string value. It frees the old bytes and copies
 * the new bytes into storage that the map owns.
 *
 * @note A NULL key of a character-pointer type is not a string. The macro
 * gives NULL for it
 *
 * @see chmap_get
 * @see chmap_get_elem_ref
 * @see chmap_insert
 *
 * Example:
 * @code
 * chmap_construct(map, int, int);
 * int key = 42, value = 100;
 * chmap_insert(map, key, value);
 * int* ptr = chmap_get_ptr(map, key);
 * if (ptr) {
 *   *ptr = 200; // Change it in place
 * }
 *
 * chmap_construct(names, int, char *);
 * chmap_insert(names, key, "hello");
 * char *const *sptr = chmap_get_ptr(names, key);
 * if (sptr) {
 *   printf("%s\n", *sptr);   // Read the stored string
 *   (*sptr)[0] = 'j';        // Edit its bytes in place: "jello"
 *   chmap_insert(names, key, "a longer replacement");  // Replace it
 * }
 * @endcode
 */
#define chmap_get_ptr(hm_name, key)                                          \
  _ccol_chmap_get_ptr_impl(                                                  \
      hm_name, (key), _ccol_uniq(__ccol_chmap_get_ptr_key_tmp, __COUNTER__), \
      _ccol_uniq(__ccol_chmap_get_ptr_val, __COUNTER__),                     \
      _ccol_uniq(__ccol_chmap_get_ptr_kp, __COUNTER__),                      \
      _ccol_uniq(__ccol_chmap_get_ptr_vp, __COUNTER__),                      \
      _ccol_uniq(__ccol_chmap_get_ptr_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_chmap_get_ptr_impl(hm_name, key, __ccol_chmap_key_tmp,          \
                                 __ccol_chmap_val, __ccol_chmap_kp,           \
                                 __ccol_chmap_vp, __ccol_chmap_r)             \
  ({                                                                          \
    /* The temporary carries the DECLARED key type of the map. See            \
     * chmap_insert() for why the lookup key goes through a conversion and    \
     * not through a raw byte copy. A key that the map stored through that    \
     * conversion is only ever found again by a lookup that makes the same    \
     * one. */                                                                \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))           \
        __ccol_chmap_key_tmp = (key);                                         \
    _ccol_clear_padding(&__ccol_chmap_key_tmp);                               \
    __typeof__(*hm_name##__ccol_val_type_var) *__ccol_chmap_val = NULL;       \
    cmap_pair *__ccol_chmap_kp = &(cmap_pair){};                              \
    const cmap_pair *__ccol_chmap_vp = NULL;                                  \
    _populate_cmap_pair(__ccol_chmap_kp, __ccol_chmap_key_tmp);               \
    ccol_retval_t __ccol_chmap_r =                                            \
        chmap_get_elem_ref(hm_name, __ccol_chmap_kp, &__ccol_chmap_vp);       \
    if (__ccol_chmap_r == ccol_success) {                                     \
      if (ccol_is_char_ptr(*hm_name##__ccol_val_type_var)) {                  \
        __ccol_chmap_val = (__typeof__(*hm_name##__ccol_val_type_var) *)&(    \
            __ccol_chmap_vp->ptr);                                            \
      } else if (__ccol_chmap_vp->size != sizeof(*__ccol_chmap_val)) {        \
        ccol_fatal_err(                                                       \
            "chmap_get_ptr('%s'): value size mismatch - stored: %lu bytes, "  \
            "requested: %lu bytes; wrong type or missing chmap_redeclare()?", \
            #hm_name, (unsigned long)__ccol_chmap_vp->size,                   \
            (unsigned long)sizeof(*__ccol_chmap_val));                        \
      } else {                                                                \
        __ccol_chmap_val =                                                    \
            (__typeof__(*hm_name##__ccol_val_type_var) *)(__ccol_chmap_vp     \
                                                              ->ptr);         \
      }                                                                       \
    }                                                                         \
    _ccol_chmap_value_ptr_result(hm_name, __ccol_chmap_val);                  \
  })

#pragma GCC visibility pop
