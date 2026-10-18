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

/* Every declaration from here to the end of this header belongs to the
 * public ABI of libccollections, and the shared library exports all of
 * them. The library is built with -fvisibility=hidden, so a function or
 * object outside such a block stays internal: it isn't in the library's
 * dynamic symbol table, and a symbol of the same name in an application
 * linking against the library can neither interpose it nor collide with
 * it. */
#pragma GCC visibility push(default)

/**
 * @file chashmap.h
 * @brief Hash map (dictionary) that resizes itself and has two
 * implementation strategies.
 *
 * A hash map that picks one of two strategies for you.
 *
 * Open addressing is used when the key and the value are both integral
 * types of 8 bytes or less:
 * - Compact 16-byte slots: a slot holds an 8-byte key next to an 8-byte
 * value and nothing else. The occupied bit of each slot lives in a parallel
 * byte array, allocated as the tail of the same block, so the key and value
 * storage keeps its natural alignment and you can read and write it in
 * place, for example with chmap_get_ptr
 * - Linear probing, with a multiplicative hash of an integer that is read
 *   from its high bits; see the two hash modes below
 * - Backward-shift deletion: a delete moves the later entries of its cluster
 *   back, so the table never holds a deleted-slot marker
 * - Load factor thresholds: an insert doubles the table past 0.70, and a
 *   delete halves it under 0.25
 * - No allocation per entry (one contiguous array)
 * - Good cache locality and good memory efficiency
 * - long double is left out, since it can be more than 8 bytes on some
 * architectures
 *
 * Separate chaining is used for a type that isn't integral, or a type of
 * more than 8 bytes:
 * - Linked lists that resolve a collision
 * - Small String Optimization (SSO): 23 bytes of inline storage for a key and
 * for a value
 * - A doubly-linked list that keeps the reverse insertion order and drives
 *   iteration
 * - Minimum bucket array size: 16 (always a power of 2)
 * - Scale factor: 4x (grows to 4x the size, shrinks to 0.25x the size)
 * - Scale up threshold: (bucket_count + 1) * 1.5 elements
 * - Scale down threshold: (bucket_count + 1) / 8 elements
 *
 * Both strategies share these:
 * - The implementation is picked from the key type and the value type
 * - A custom hash function is supported, and its result goes through a
 * finalizer before an index is derived from it
 * - Two hash modes. Every map starts in the fast mode: a Fibonacci multiply
 * for an integral, float, double or pointer key, XXH64 with a secret seed
 * for a string or any other buffer, and the murmur3 finalizer for the result
 * of a custom hash. These spread ordinary keys well and are cheap, but
 * whoever controls the keys can pick a set that collides under them. So
 * every insert of a new key measures how far it had to go, and a map whose
 * keys collide more than a random function would let them switches to the
 * keyed mode: a mixer of two multiplies keyed with a secret seed for a
 * fixed-width key and for the result of a custom hash, and SipHash-1-3 with
 * a secret key for a buffer. Each growth of a keyed map tries the fast mode
 * again on the new table and keeps it when the keys spread under it. A
 * lookup never writes to the map. A long double key is hashed by value with
 * SipHash-1-3 in both modes. The secrets are drawn once per process. See
 * chmap_create_full() for the thresholds and for what someone who controls
 * the keys can still force
 * - A separate-chaining map iterates in insertion order, while an
 * open-addressing map iterates in slot order, which depends on the mode and
 * in the keyed mode is different from one process to the next
 * - Type-inferred macros for the common operations
 * - Average complexity: O(1) for insert, for get and for delete
 *
 * Some examples of which implementation gets picked:
 * - int->int, long->double, float->uint32_t: Open-addressing
 * - string->int, int->string, string->string, int->long double: Separate
 * chaining
 */

/** @brief The default first size of the bucket array */
#define CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE 16

/** @brief Opaque hash map structure */
typedef struct chashmap chashmap;

/** @brief Pointer to a hash map (the handle type) */
typedef chashmap *chmap;

/* ========================================================================== */
/*                         HASH MAP CREATION                                  */
/* ========================================================================== */

/**
 * @brief Create a hash map and set every option
 *
 * Makes a new hash map. You give the first bucket count, the key type, the
 * value type, custom memory management, a custom hash function and a custom
 * key equality function, and the map picks its implementation strategy
 * (open-addressing or separate chaining) from the key type and
 * the value type.
 *
 * @param initial_bucket_array_size First number of buckets. 0 is refused
 * and the call fails; a value from 1 to 16 gives 16 buckets, and a larger
 * value is rounded up to the next power of two
 * @param key_type Type of the keys, which selects the implementation strategy
 * @param val_type Type of the values, which selects the implementation
 * strategy
 * @param mmgmt_procs Custom memory management procedures, or NULL for the
 * default malloc/free
 * @param custom_hashing_proc Custom hash function, or NULL for the default
 * @param custom_key_equality_proc Custom key equality function, or NULL for
 * the built-in comparison. A map that has one must also have a
 * custom_hashing_proc, and the two have to agree: keys that the equality
 * function reports as equal must hash the same.
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails. It fails
 * when custom_key_equality_proc is not NULL and custom_hashing_proc is NULL.
 *
 * @note How the implementation is picked:
 *       - Open-addressing: the key and the value are both integral types of
 *         8 bytes or less, and custom_key_equality_proc is NULL
 *       - Separate chaining: the key or the value is not integral, or is
 *         more than 8 bytes, or custom_key_equality_proc is not NULL
 * @note Without a custom_key_equality_proc, a key of a type the map doesn't
 * know (a struct, say) equals another key only when every byte of the two
 * is equal, and the default hash reads every byte as well, padding bytes of
 * a struct included. Two keys with equal members but different padding are
 * then two different keys and a lookup misses. Give such a key type a
 * custom_hashing_proc and a custom_key_equality_proc that read only the
 * members. Without them the bytes of a padded key must be fully determined.
 * Through the raw functions such as chmap_insert_elem() the map reads
 * exactly the object that key_pair->ptr points at, so zero the whole key
 * object with memset() before you set its members. Through the
 * type-inferred macros the map sees the macro's own copy of the key, and C
 * leaves the padding of a struct copy unspecified: with GCC 11 or later the
 * macros zero that padding, and with any other compiler a padded key that
 * goes through the macros needs the two procedures.
 * @note The integral types are char, short, int, long, long long (signed and
 * unsigned), float and double
 * @note long double can't use open-addressing, because it can be 10 to 16
 * bytes
 * @note The default hash is picked by the key type and the hash mode, not by
 * the backend. In the fast mode, where every map starts, an integral, float,
 * double or pointer key gets a Fibonacci multiply read from its high bits,
 * a string key and any other buffer-like key gets XXH64 with a secret
 * 64-bit seed, and the result of a custom_hashing_proc goes through the
 * murmur3 finalizer. In the keyed mode they get, in the same order, a mixer
 * of two multiplies that starts from an exclusive or with a secret 64-bit
 * seed, SipHash-1-3 with a secret 128-bit key, and that same mixer. A long
 * double key gets SipHash-1-3 by value in both modes. A separate-chaining
 * map with an integral key type gets the integer hash for that key exactly
 * like an open-addressing map does, for example with a double key and a
 * value that isn't integral.
 * @note The map watches every insert of a new key and switches to the keyed
 * mode on its own when its keys collide more than a random function would
 * let them. An open-addressing map switches when one insert lands more than
 * its cap away from its home slot: 24 slots per doubling of the table up to
 * 4096 slots, and 288 slots from there on. A separate-chaining map switches
 * when a new key meets 20 nodes in its chain. Both switch when 256
 * consecutive inserts of new keys cost more in total than 256 times
 * 5 * E(a) + 0.5 probes for an open-addressing table of 4096 slots or more,
 * 6 * E(a) + 0.5 for a smaller one, and 4.5 * lambda + 0.5 nodes for
 * separate chaining. E(a) = (1 / (1 - a)^2 - 1) / 2 is the expected
 * displacement of an insert at load a under a random function, lambda is the
 * number of entries per bucket, and a and lambda are the highest load that
 * those 256 inserts can have reached. A shrink that merges clusters or
 * chains is held to the same caps. A lookup never writes to the map and
 * never switches it.
 * @note Until the map switches, someone who controls the keys can make an
 * open-addressing insert cost about 26 probes on average at a load of 0.70
 * (about 31 in a table below 4096 slots) and a single insert no more than
 * the cap; a separate-chaining insert meets about 7.3 nodes on average at
 * 1.5 entries per bucket, and never 20. A lookup or a delete in an
 * open-addressing map looks at no more than one slot past the largest
 * displacement of any key in the table, which the cap bounds in the fast
 * mode. So a long run of keys with consecutive home slots, which no insert
 * pays for, can't cost a lookup more than that either.
 * @note The switch rebuilds an open-addressing table at the same size. When
 * that allocation fails the map stays fast and correct, the insert that
 * asked for the switch succeeds, and the next insert that asks for it tries
 * again. The nodes of a separate-chaining map are rehashed in place with no
 * allocation, so every reference to a key or a value, the insertion order
 * and every iterator stay valid. A reset and a shrink keep the mode.
 * @note A growth of a keyed map tries the fast mode on the new table, which
 * is the doubled table of an open-addressing map or the bucket array of a
 * separate-chaining map that grows by four. The fast mode is kept unless
 * that fill shows the keys collide under it: an open-addressing entry that
 * would land more than the new table's cap away from its home slot, a
 * separate-chaining chain that would reach 20 nodes, or displacements or
 * chain walks that add up to more than the count plus 64. The attempt stops
 * at the first of these, and the growth fills the same new table with the
 * keyed hash, so nothing more is allocated. For a map of n entries a failed
 * attempt looks at no more than 2 * n + cap + 65 slots, or hashes n nodes
 * and walks no more than n + 83 chain nodes, on top of the keyed fill.
 * Growth is geometric, so a map whose every growth fails pays a constant
 * amount of extra work per insert. A map that goes back to the fast mode is
 * judged by its inserts again, so a resumed attack switches it within two
 * windows of 256 inserts. Integer sets that cluster under the multiply only
 * at some sizes, such as multiples of 24, 32, 48 or 96, switch at a small
 * size and come back to the fast mode at a later growth whose table spreads
 * them well; at other sizes they cluster again and the window switches the
 * map back. Whether the attempt succeeds or not it keeps every entry, and
 * under separate chaining every reference to a key or a value, the
 * insertion order and every iterator. Which keys the map treats as the same
 * key is the same in both modes.
 * @note The secrets are random. They are drawn from getrandom(2) once per
 * process, the first time the process creates a map, and when the kernel
 * can't answer without blocking we mix the clocks, the process id and
 * randomized addresses instead. A custom_hashing_proc that gives two keys
 * the same value makes them collide in both modes, because no finalizer can
 * tell them apart.
 * @note For a float or double key_type a -0.0 key is always changed to 0.0
 * before it is hashed, stored or compared, so the two values collide into
 * one single key, exactly as `-0.0 == 0.0` in C. That holds with a
 * custom_hashing_proc too: the custom function always gets the bit pattern
 * that the map already changed, never a raw -0.0. The map stores the changed
 * value and a later iteration reads that same value, not the exact bit
 * pattern the caller gave to chmap_insert.
 * @note A long double key_type always uses separate chaining (see the note
 * above about long double and open-addressing). For this key type, unlike
 * every other one, key equality and the hash come from the numeric VALUE of
 * the key and not from its raw bytes. `-0.0L` and `0.0L` collide into one
 * single key, exactly like `-0.0` and `0.0` for a float or a double. Two keys
 * holding the identical value stay the same key even when their padding
 * bits differ; the platform defines those padding bits, and they really can
 * differ for a long double, since its in-memory representation isn't fully
 * significant on most platforms. Every NaN long double collapses into one
 * single key. That is a deliberate difference from the float and double
 * policy, which keeps distinct NaN payloads, and the same padding forces it:
 * the map never reads a padding byte of a NaN long double, because that byte
 * is often genuinely uninitialized memory and not just unspecified content.
 * None of this value-based handling applies when custom_hashing_proc is not
 * NULL; the custom function then gets the raw bytes of a long double key,
 * padding included, as for any other type. So a custom hash function for a
 * long double key type has to be value-based itself and ignore the padding,
 * for example by hashing the result of frexpl(). Otherwise the bucket the
 * hash picks doesn't agree with this map's key equality, which is always
 * value-based: two representations of the same numeric value can pick
 * different buckets, and a lookup can report a present key as absent.
 * @note Destroy the map with chmap_destroy() once you're done with it
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
 * A convenience wrapper for chmap_create_full() with the default memory
 * management and the default hash, which starts in the fast mode and
 * switches to the keyed mode on a flood; see chmap_create_full().
 *
 * @param initial_bucket_array_size First number of buckets; see
 * chmap_create_full(). 0 is refused and the call fails
 * @param key_type Type of the keys, which selects the implementation strategy
 * @param val_type Type of the values, which selects the implementation
 * strategy
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails
 *
 * @note See chmap_create_full() for how the implementation is picked
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
 * A convenience wrapper for chmap_create_full() with custom memory
 * management and the default hash.
 *
 * @param initial_bucket_array_size First number of buckets; see
 * chmap_create_full(). 0 is refused and the call fails
 * @param key_type Type of the keys, which selects the implementation strategy
 * @param val_type Type of the values, which selects the implementation
 * strategy
 * @param mmgmt_procs Custom memory management procedures
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails
 *
 * @note See chmap_create_full() for how the implementation is picked
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
 * A convenience wrapper for chmap_create_full() with a custom hash function
 * and the default memory management.
 *
 * @param initial_bucket_array_size First number of buckets; see
 * chmap_create_full(). 0 is refused and the call fails
 * @param key_type Type of the keys, which selects the implementation strategy
 * @param val_type Type of the values, which selects the implementation
 * strategy
 * @param custom_hashing_proc Custom hash function
 * @param err Optional pointer that gets an error string when the call fails
 *
 * @return Pointer to the new hash map, or NULL when the call fails
 *
 * @note See chmap_create_full() for how the implementation is picked
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
 * Gives how many key-value pairs the hash map holds right now.
 *
 * @param chm The hash map to read
 *
 * @return Number of elements in the map
 *
 * @note It asserts when chm is NULL
 * @note O(1) complexity
 */
size_t chmap_elem_count(chmap chm);

/**
 * @brief Remove every element, and resize the map if you ask for it
 *
 * Removes every key-value pair from the map and destroys every internal data
 * structure. It can also resize the array of buckets or slots to a new size.
 *
 * @param chm The hash map to reset
 * @param new_bucket_array_size New capacity. Give 0 to keep the current size
 *
 * @return ccol_success when the call succeeds
 * @return ccol_not_enough_memory when the resize fails. Every element is
 * removed anyway
 *
 * @note Every element is destroyed whatever the return value is
 * @note The array size stays as it is when new_bucket_array_size is 0
 * @note The size is set to 16 when new_bucket_array_size is less than 16
 * @note Otherwise the size is rounded UP to the nearest power of two that is
 * >= new_bucket_array_size. It's a ceiling, never a round down.
 * @note It asserts when chm is NULL
 *
 * @see chmap_destroy
 */
ccol_retval_t chmap_reset(chmap chm, size_t new_bucket_array_size);

/**
 * @brief Insert a key-value pair, or update it (upsert)
 *
 * Inserts a new key-value pair, or updates the value when the key is there
 * already. The map resizes itself when the load factor goes over the
 * threshold, and it copies the key and the value into its own storage.
 *
 * @param chm The hash map to insert into
 * @param key_pair The key to insert. ptr and size must both be valid
 * @param val_pair The value to insert. ptr and size must both be valid
 *
 * @return ccol_success when a genuinely new key was inserted
 * @return ccol_key_already_present when the key was there and its value was
 * updated in place. That's a successful upsert and not an error, so treat
 * this value as success just like ccol_success
 * @return ccol_invalid_args when a pointer is NULL or a size is 0. Also when
 * the key type is a fixed-width numeric type and key_pair->size doesn't
 * match its byte size exactly, on every backend. The fixed-width numeric
 * types are char, short, int, long, long long, their unsigned equivalents,
 * float, double, long double and a pointer type. Also when the value type
 * the map was made with is a fixed-width type and val_pair->size doesn't
 * match its byte size exactly, again on every backend. A value type with no
 * fixed width (ccol_string and ccol_other_types) is never checked, since
 * every entry of such a map carries its own size
 * @return ccol_container_full when the map reaches ccol_max_elem_count
 * @return ccol_not_enough_memory when an allocation fails
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe
 * @note Separate chaining: O(n) worst case per bucket, where n is the chain
 * length
 * @note The key data and the value data are copied, the map doesn't point at
 * them
 * @note When the key is present only the value is updated and the key stays
 * as it is. This path reports ccol_key_already_present and not
 * ccol_success; see above
 * @note key_pair and val_pair may point into the map itself: each can be a
 * pair that chmap_get_elem_ref() filled for any key of the same map, or any
 * other pair that points at the bytes of a stored key or value. Both pairs
 * are read before anything changes, so this holds even for the insert that
 * resizes the map and frees the storage they point at
 * @note Open-addressing: a resize starts at a load factor of 0.70
 * @note Separate chaining: a resize starts when elem_count >=
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
 * Copies the value of the given key into the buffer you give. It copies
 * min(value_size, target_buf_size) bytes, so the two sizes may differ.
 *
 * @param chm The hash map to search
 * @param key_pair The key to look up
 * @param target_buf The buffer that gets the copy of the value
 * @param target_buf_size Size of the target buffer
 *
 * @return ccol_success when the key was found and the value copied
 * @return ccol_invalid_args when a pointer is NULL or a size is 0. Also when
 * the key type is a fixed-width numeric type and key_pair->size doesn't
 * match its byte size exactly, on every backend. The fixed-width numeric
 * types are char, short, int, long, long long, their unsigned equivalents,
 * float, double, long double and a pointer type
 * @return ccol_key_not_found when the key is not present
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe
 * @note Separate chaining: O(n) worst case per bucket, where n is the chain
 * length
 * @note min(actual_value_size, target_buf_size) bytes are copied
 * @note A buffer that is too small is safe, you just get a partial copy
 * @note When target_buf_size is larger than the stored value, the remaining
 * bytes of target_buf are set to zero rather than left as they were
 *
 * @see chmap_get_elem_ref
 * @see chmap_insert_elem
 */
ccol_retval_t chmap_get_elem_copy(chmap chm, const cmap_pair *key_pair,
                                  void *target_buf, size_t target_buf_size);

/**
 * @brief Get the location and the size of the value that belongs to a key
 *
 * Fills the caller's cmap_pair with the location and the size of the stored
 * value of the given key: val_pair->ptr points at the value bytes inside the
 * map and val_pair->size is the stored value's size. You can read those
 * bytes and change them in place within val_pair->size. The pointer is valid
 * until something (an insert, a delete or a resize) changes the map.
 *
 * @param chm The hash map to search
 * @param key_pair The key to look up
 * @param val_pair A cmap_pair of the caller that the function fills
 *
 * @return ccol_success when the key was found
 * @return ccol_invalid_args when a pointer is NULL or the key size is 0.
 * Also when the key type is a fixed-width numeric type and key_pair->size
 * doesn't match its byte size exactly, on every backend. The fixed-width
 * numeric types are char, short, int, long, long long, their unsigned
 * equivalents, float, double, long double and a pointer type
 * @return ccol_key_not_found when the key is not present
 *
 * @note On every failure with a non-NULL val_pair, val_pair->ptr is set to
 * NULL and val_pair->size to 0
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe
 * @note Separate chaining: O(n) worst case per bucket, where n is the chain
 * length
 * @note An insert, a delete or a resize makes val_pair->ptr invalid, but a
 * call to chmap_get_elem_ref, chmap_get or chmap_get_ptr for a different key
 * never does. So you can hold two or more results for different keys at the
 * same time and each one stays valid on its own
 * @note The cmap_pair belongs to the caller and the map keeps no reference
 * to it. Don't free val_pair->ptr, the map owns the value bytes. Changing
 * val_pair->ptr or val_pair->size changes only the caller's copy and never
 * the stored value; use chmap_insert_elem() to replace a value.
 *
 * @see chmap_get_elem_copy
 * @see chmap_insert_elem
 */
ccol_retval_t chmap_get_elem_ref(chmap chm, const cmap_pair *key_pair,
                                 cmap_pair *val_pair);

/**
 * @brief Get the accessor that a separate-chaining map keeps for the value
 * of a key (internal function)
 *
 * The string arm of chmap_get_ptr() needs the address of the char* object
 * that the map stores for the value, and that object is this accessor's ptr
 * field. An open-addressing map never holds a string value, so it gives
 * ccol_invalid_args for one, and on every failure with a non-NULL
 * val_accessor *val_accessor is set to NULL.
 *
 * @warning Do not call this function directly. Use the chmap_get_ptr() macro
 */
ccol_retval_t _chmap_get_val_accessor(chmap chm, const cmap_pair *key_pair,
                                      const cmap_pair **val_accessor);

/**
 * @brief Delete a key-value pair from the map
 *
 * Removes the given key and its value from the map. The map resizes itself
 * down when the load factor falls below the threshold.
 *
 * @param chm The hash map to delete from
 * @param key_pair The key to delete
 *
 * @return ccol_success when the key was found and deleted
 * @return ccol_invalid_args when a pointer is NULL or the key size is 0.
 * Also when the key type is a fixed-width numeric type and key_pair->size
 * doesn't match its byte size exactly, on every backend. The fixed-width
 * numeric types are char, short, int, long, long long, their unsigned
 * equivalents, float, double, long double and a pointer type
 * @return ccol_key_not_found when the key is not present
 *
 * @note O(1) average complexity
 * @note Open-addressing: O(n) worst case for the linear probe. A delete
 * moves later entries of the same cluster back into the gap, so it leaves
 * no deleted-slot marker, and every element reference is invalid after it
 * @note Separate chaining: O(n) worst case per bucket, where n is the chain
 * length
 * @note The memory of the key and of the value is freed
 * @note Open-addressing: a resize starts at a load factor of 0.25
 * @note Separate chaining: a resize starts when elem_count <
 * (bucket_count + 1) / 8
 * @note Scale factor is 0.5x for open-addressing, 0.25x for separate chaining
 * @note It never resizes below the minimum threshold
 *
 * @see chmap_insert_elem
 * @see chmap_reset
 */
ccol_retval_t chmap_delete_elem(chmap chm, const cmap_pair *key_pair);

/* ========================================================================== */
/*                         HASH MAP ITERATION                                 */
/* ========================================================================== */

/* chashmap_begin_iter() is declared in citerators.h (included above), since
 * the ccol_begin() macro there needs all three iterator functions in every
 * translation unit that uses it. */

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
 * @brief Destroy a hash map, call a destructor on every value first, and
 * allocate no memory to do it
 *
 * Behaves like __chmap_destroy() with one difference: when val_dtor is not
 * NULL it is called once for each live entry, with that entry's value as a
 * cmap_pair, right before the entry is freed. Nothing at all is allocated.
 * The walk goes over the map's internal bucket and slot storage directly,
 * the same way __chmap_destroy() does it, and the destructor call is part of
 * that same walk.
 *
 * It's meant for a map whose values are owned pointers, each naming a
 * bigger structure that needs its own recursive teardown, a dictionary of
 * dictionaries for example. You could walk such a map with
 * chashmap_begin_iter() instead, run a value destructor for each entry and
 * destroy the empty map at the end, but that has a real gap under memory
 * pressure: the small internal allocation of chashmap_begin_iter() can fail,
 * and then there's no way left to reach and free the values the map still
 * owns. This function doesn't have that gap, because it allocates nothing of
 * its own and so always reaches every value.
 *
 * @param chm The hash map to destroy
 * @param val_dtor Destructor called once for each live value, before that
 *   entry is freed. Give NULL for the same behaviour as __chmap_destroy()
 * @param dtor_ctx Opaque pointer passed unchanged to every val_dtor call
 *
 * @note Calling it with a NULL chm is safe and does nothing
 * @note The chm handle is not set to NULL, which matches the raw internal
 *   convention of __chmap_destroy()
 * @note val_dtor must not change chm, since the map is in the middle of its
 *   teardown for the whole call
 * @note val_pair describes the stored value of the entry: val_pair->ptr
 *   points into storage the map is about to free, and its target is const.
 *   A destructor reads val_pair->ptr and val_pair->size and then frees
 *   whatever the value owns, usually a pointer it copies out of those
 *   bytes. Assigning to val_pair->ptr or val_pair->size is a compile error:
 *   the pair describes storage that the map is about to free, and the map
 *   can't own a pointer it didn't allocate
 *
 * @see __chmap_destroy
 * @see chashmap_begin_iter
 */
void chmap_destroy_with_dtor(chmap chm,
                             void (*val_dtor)(const cmap_pair *val_pair,
                                              void *dtor_ctx),
                             void *dtor_ctx);

/**
 * @brief Internal cleanup function that destroys a map at the end of its scope
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
 * Frees every resource of the hash map: every key, every value, every
 * bucket and every internal structure.
 *
 * @param chm The hash map to destroy. It is set to NULL
 *
 * @note Calling it with NULL is safe
 * @note Every key and every value is freed
 * @note Every collision chain of every bucket is destroyed
 * @note chm is evaluated exactly once, and it must be a modifiable lvalue
 * such as a variable or an element of an array
 */
#define chmap_destroy(chm)      \
  _ccol_chmap_destroy_impl(chm, \
                           _ccol_uniq(__ccol_chmap_destroy_slot, __COUNTER__))

/* Internal: the body of chmap_destroy. slot is a name from _ccol_uniq(), so
 * the macro can nest inside the argument of another destroy macro and still
 * be -Wshadow clean. The argument is evaluated exactly once. */
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
 * Declares the type variables that the type-inferred macros need, for a
 * hash map that was made in another scope.
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
  _ccol_refuse_array_types("chmap", key_t, val_t);                          \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

/**
 * @brief Declare a hash map variable that is not initialized
 *
 * Declares a hash map variable together with the type variables that the
 * type-inferred macros need. Initialize the map before you use it.
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
/* The unused attribute goes on the pointer type of each companion variable
 * and not on the declaration, because this macro also declares struct
 * members and GCC before version 11 refuses the attribute on a member. Put
 * on a type, the attribute still marks every variable of that type as
 * possibly unused. */
#define chmap_declare(hm_name, key_t, val_t)                             \
  _ccol_refuse_array_types("chmap", key_t, val_t);                       \
  __typeof__(key_t) *__attribute__((unused))                             \
  hm_name##__ccol_key_type_var; /* not initialized to NULL on purpose */ \
  __typeof__(val_t) *__attribute__((unused))                             \
  hm_name##__ccol_val_type_var; /* not initialized to NULL on purpose */ \
  chmap hm_name                 /* not initialized to NULL on purpose */

#define chmap_declare_scoped(hm_name, key_t, val_t)                         \
  _ccol_refuse_array_types("chmap", key_t, val_t);                          \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) = \
      NULL;                                                                 \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL

/**
 * @brief Initialize a hash map and set every option
 *
 * Initializes a hash map that you declared before, with custom memory
 * management, a custom hash and a custom key equality, and calls
 * _ccol_fatal_err() when the initialization fails. The key type and the
 * value type come from the type variables that chmap_declare makes.
 *
 * @param hm_name Hash map variable to initialize. It must be declared
 * @param mmgmt_procs Custom memory management procedures, or NULL
 * @param custom_hashing_proc Custom hash function, or NULL
 * @param custom_key_equality_proc Custom key equality function, or NULL. See
 * chmap_create_full() for the rule that ties it to custom_hashing_proc.
 *
 * @note It stops the program when the initialization fails
 * @note It uses CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (16)
 * @note The key type and the value type are read by the macro itself, and
 * the implementation is picked from them
 *
 * @see chmap_declare
 * @see chmap_construct_full
 */
#define chmap_init_full(hm_name, mmgmt_procs, custom_hashing_proc,            \
                        custom_key_equality_proc)                             \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_full(                                              \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),     \
        &__ccol_chmap_err);                                                   \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/**
 * @brief Declare a hash map, initialize it, and set every option
 *
 * Declaration and initialization in one, with custom memory management, a
 * custom hash and a custom key equality, and a call to _ccol_fatal_err()
 * when the initialization fails. You name the key type and the value type,
 * and those two pick the implementation strategy.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures, or NULL
 * @param custom_hashing_proc Custom hash function, or NULL
 * @param custom_key_equality_proc Custom key equality function, or NULL. See
 * chmap_create_full() for the rule that ties it to custom_hashing_proc.
 *
 * @note It stops the program when the initialization fails
 * @note It uses CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE (16)
 * @note The implementation is picked from key_t and val_t
 *
 * @see chmap_init_full
 */
#define chmap_construct_full(hm_name, key_t, val_t, mmgmt_procs,              \
                             custom_hashing_proc, custom_key_equality_proc)   \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_full(                                              \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),     \
        &__ccol_chmap_err);                                                   \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

#define chmap_construct_full_scoped(hm_name, key_t, val_t, mmgmt_procs,       \
                                    custom_hashing_proc,                      \
                                    custom_key_equality_proc)                 \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_full(                                              \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),     \
        &__ccol_chmap_err);                                                   \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/**
 * @brief Initialize a hash map with the default settings
 *
 * Initializes a hash map that you declared before, with the default
 * settings. The key type and the value type come from the type variables.
 *
 * @param hm_name Hash map variable to initialize
 *
 * @note It stops the program when the initialization fails
 * @note It uses the default memory management and picks the hash for you
 * @note The implementation follows from the types it reads
 */
#define chmap_init(hm_name)                                                   \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create(                                                   \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        &__ccol_chmap_err);                                                   \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/**
 * @brief Declare a hash map and initialize it with the default settings
 *
 * Declaration and initialization in one, with the default settings. The
 * implementation strategy is chosen from the key type and the
 * value type.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 *
 * @note It stops the program when the initialization fails
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
#define chmap_construct(hm_name, key_t, val_t)                                \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create(                                                   \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        &__ccol_chmap_err);                                                   \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

#define chmap_construct_scoped(hm_name, key_t, val_t)                         \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create(                                                   \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        &__ccol_chmap_err);                                                   \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/**
 * @brief Initialize a hash map with custom memory management
 *
 * Initializes a hash map that you declared before, with custom memory
 * management. The key type and the value type come from the type variables.
 *
 * @param hm_name Hash map variable to initialize
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note It stops the program when the initialization fails
 * @note It uses the default hash, chosen to fit the types
 */
#define chmap_init_mp(hm_name, mmgmt_procs)                                   \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_mp(                                                \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (mmgmt_procs), &__ccol_chmap_err);                                    \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/**
 * @brief Declare a hash map and initialize it with custom memory management
 *
 * Declaration and initialization in one, with custom memory management.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 * @param mmgmt_procs Custom memory management procedures
 *
 * @note It stops the program when the initialization fails
 */
#define chmap_construct_mp(hm_name, key_t, val_t, mmgmt_procs)                \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_mp(                                                \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (mmgmt_procs), &__ccol_chmap_err);                                    \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

#define chmap_construct_mp_scoped(hm_name, key_t, val_t, mmgmt_procs)         \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name _ccol_destructor(___chmap_destroy) = NULL;                    \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_mp(                                                \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (mmgmt_procs), &__ccol_chmap_err);                                    \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/**
 * @brief Initialize a hash map with a custom hash
 *
 * Initializes a hash map that you declared before, with a custom hash
 * function.
 *
 * @param hm_name Hash map variable to initialize
 * @param custom_hashing_proc Custom hash function
 *
 * @note It stops the program when the initialization fails
 * @note It uses the default memory management
 */
#define chmap_init_ch(hm_name, custom_hashing_proc)                           \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_ch(                                                \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (custom_hashing_proc), &__ccol_chmap_err);                            \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/**
 * @brief Declare a hash map and initialize it with a custom hash
 *
 * Declaration and initialization in one, with a custom hash function.
 *
 * @param hm_name Name of the hash map variable
 * @param key_t Key type
 * @param val_t Value type
 * @param custom_hashing_proc Custom hash function
 *
 * @note It stops the program when the initialization fails
 */
#define chmap_construct_ch(hm_name, key_t, val_t, custom_hashing_proc)        \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
  __typeof__(key_t) *hm_name##__ccol_key_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  __typeof__(val_t) *hm_name##__ccol_val_type_var __attribute__((unused)) =   \
      NULL;                                                                   \
  chmap hm_name /* _ccol_destructor(___chmap_destroy) */ = NULL;              \
  do {                                                                        \
    char *__ccol_chmap_err = NULL;                                            \
    hm_name = chmap_create_ch(                                                \
        CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                               \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),         \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),         \
        (custom_hashing_proc), &__ccol_chmap_err);                            \
    if (!hm_name) {                                                           \
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

#define chmap_construct_ch_scoped(hm_name, key_t, val_t, custom_hashing_proc) \
  _ccol_refuse_array_types("chmap", key_t, val_t);                            \
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
      _ccol_fatal_err("Failed to create hash map '%s': %s", #hm_name,         \
                      __ccol_chmap_err ? __ccol_chmap_err : "unknown error"); \
    }                                                                         \
  } while (0)

/* ========================================================================== */
/*                    TYPE-INFERRED OPERATION MACROS */
/* ========================================================================== */

/**
 * @brief Insert a key-value pair (type-inferred)
 *
 * The type-inferred wrapper for chmap_insert_elem(). It builds the key and
 * value cmap_pairs from the expressions you give and calls _ccol_fatal_err()
 * when the insert fails.
 *
 * key and val are converted to the map's declared key and value types the
 * same way a plain C assignment converts them, and the converted copies are
 * stored; the bytes of an expression of another type are never
 * reinterpreted. An expression with no implicit conversion to the declared
 * type is a compile error. A character-pointer key or value type keeps the
 * pointer type of the expression instead, so a const char * needs no cast.
 *
 * @param hm_name The hash map to insert into
 * @param key The key to insert
 * @param val The value for that key
 *
 * @note It stops the program when the insert fails
 * @note It takes the address of the key and of the value for you
 * @note It works for a value type and for a string type
 *
 * @note A NULL key or value of a character-pointer type is not a string,
 * and the program stops with ccol_invalid_args for it
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
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_chmap_insert_impl(                                              \
    hm_name, key, __ccol_chmap_val, __ccol_chmap_key_tmp,                     \
    __ccol_chmap_val_tmp, __ccol_chmap_kp, __ccol_chmap_vp, __ccol_chmap_r)   \
  do {                                                                        \
    /* Each temporary below holds a private copy of the expression the caller \
     * wrote. _ccol_declared_or_own_type() gives that copy the map's DECLARED \
     * key or value type, so the initialization is a plain C assignment and   \
     * the value is converted by the compiler's conversion rules. cvec_push() \
     * uses the same construct for the same reason.                           \
     *                                                                        \
     * With a temporary of __typeof__(key) instead, the RAW BYTES of the      \
     * caller's expression would go into the map whenever the two types       \
     * happen to have the same width. A float going into a                    \
     * chmap_construct(m, char *, int) would store the float's bit pattern,   \
     * and a later chmap_get() would report 1069547520 for 1.5f. A key is     \
     * worse: a float key of 2.0f in an int-keyed map could then never be     \
     * reached by any int lookup for the life of the map. The widths match,   \
     * so no size check anywhere can see it, and no compiler diagnostic fires \
     * at any optimization level. Here an expression whose type has no        \
     * implicit conversion to the declared type is a compile error instead,   \
     * which is the contract cvec_push() documents too.                       \
     *                                                                        \
     * A narrower expression is the other half of the same rule. A short      \
     * going into an int-valued map becomes an int here, so the pair is       \
     * always exactly sizeof(ValT) bytes wide.                                \
     *                                                                        \
     * A declared type that is a character pointer keeps the type of the      \
     * caller's expression instead; _ccol_declared_or_own_type() in common.h  \
     * explains why that case must not convert.                               \
     *                                                                        \
     * The copy also settles the lifetime question that                       \
     * _populate_cmap_pair() raises. That helper takes its argument's address \
     * inside one of its own nested blocks, so a compound literal written by  \
     * the caller would be created there and die with that block, while these \
     * temporaries keep their own copy for the whole call. The caller's       \
     * expression is still evaluated exactly once, and an rvalue is as fine   \
     * as an lvalue.                                                          \
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
      _ccol_fatal_err("chmap_insert('%s'): r: %d (%s)", #hm_name,             \
                      __ccol_chmap_r, ccol_retval_to_str(__ccol_chmap_r));    \
    }                                                                         \
  } while (0)

/**
 * @brief Remove a key-value pair (type-inferred)
 *
 * The type-inferred wrapper for chmap_delete_elem(), which hands the result
 * code back to you.
 *
 * @param hm_name The hash map to remove from
 * @param key The key to remove
 *
 * @return ccol_success when the pair was removed, or ccol_key_not_found
 * when the key is not present
 *
 * @note It doesn't stop the program for ccol_key_not_found
 * @note The memory of the key and of the value is freed
 *
 * @note A NULL key of a character-pointer type is not a string, and you get
 * ccol_invalid_args for it
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
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_chmap_remove_impl(hm_name, key, __ccol_chmap_key_tmp,    \
                                __ccol_chmap_kp, __ccol_chmap_r)       \
  ({                                                                   \
    /* The temporary has the map's DECLARED key type. chmap_insert()   \
     * explains why the lookup key goes through a conversion and not a \
     * raw byte copy. */                                               \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))    \
        __ccol_chmap_key_tmp = (key);                                  \
    _ccol_clear_padding(&__ccol_chmap_key_tmp);                        \
    cmap_pair *__ccol_chmap_kp = &(cmap_pair){};                       \
    _populate_cmap_pair(__ccol_chmap_kp, __ccol_chmap_key_tmp);        \
    ccol_retval_t __ccol_chmap_r =                                     \
        chmap_delete_elem(hm_name, __ccol_chmap_kp);                   \
    __ccol_chmap_r;                                                    \
  })

/**
 * @brief Get a value by its key (type-inferred, gives the value)
 *
 * The type-inferred wrapper for chmap_get_elem_ref() that gives the value
 * itself. It calls _ccol_fatal_err() when the key is not present, and also
 * when the two sizes don't match.
 *
 * @param hm_name The hash map to search
 * @param key The key to look up
 *
 * @return The value for that key
 *
 * @note It stops the program when the key is not present
 * @note It stops the program when the size of the stored value doesn't
 * match the size of the declared value type
 * @note It gives the value, not a pointer
 * @note For a string it gives the char* itself
 *
 * @note A NULL key of a character-pointer type is not a string, and the
 * program stops with ccol_invalid_args for it
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
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_chmap_get_impl(hm_name, key, __ccol_chmap_key_tmp,               \
                             __ccol_chmap_val, __ccol_chmap_kp,                \
                             __ccol_chmap_vp, __ccol_chmap_r)                  \
  ({                                                                           \
    /* The temporary has the map's DECLARED key type. chmap_insert()           \
     * explains why the lookup key goes through a conversion and not a raw     \
     * byte copy. A key that the map stored through that conversion can only   \
     * be found again by a lookup that does the same conversion. */            \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        __ccol_chmap_key_tmp = (key);                                          \
    _ccol_clear_padding(&__ccol_chmap_key_tmp);                                \
    __typeof__(*hm_name##__ccol_val_type_var) *__ccol_chmap_val = NULL;        \
    cmap_pair *__ccol_chmap_kp = &(cmap_pair){};                               \
    cmap_pair __ccol_chmap_vp = {NULL, 0};                                     \
    _populate_cmap_pair(__ccol_chmap_kp, __ccol_chmap_key_tmp);                \
    ccol_retval_t __ccol_chmap_r =                                             \
        chmap_get_elem_ref(hm_name, __ccol_chmap_kp, &__ccol_chmap_vp);        \
    if (__ccol_chmap_r != ccol_success) {                                      \
      _ccol_dump_key_to_stderr(__ccol_chmap_kp->ptr, __ccol_chmap_kp->size);   \
      _ccol_fatal_err("chmap_get('%s'): r: %d (%s)", #hm_name, __ccol_chmap_r, \
                      ccol_retval_to_str(__ccol_chmap_r));                     \
    }                                                                          \
    if (_ccol_is_char_ptr(*hm_name##__ccol_val_type_var)) {                    \
      __ccol_chmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)&(__ccol_chmap_vp.ptr); \
    } else if (__ccol_chmap_vp.size != sizeof(*__ccol_chmap_val)) {            \
      _ccol_fatal_err(                                                         \
          "chmap_get('%s'): value size mismatch - stored: %lu bytes, "         \
          "requested: %lu bytes; wrong type or missing chmap_redeclare()?",    \
          #hm_name, (unsigned long)__ccol_chmap_vp.size,                       \
          (unsigned long)sizeof(*__ccol_chmap_val));                           \
    } else {                                                                   \
      __ccol_chmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)(__ccol_chmap_vp.ptr);  \
    }                                                                          \
    *__ccol_chmap_val;                                                         \
  })

/* Gives p unchanged for every value type except a string. For a map whose
 * value is a string it gives a pointer to a const-qualified target.
 *
 * A map that stores strings owns the bytes and keeps its own {ptr, size}
 * accessor for the entry in step with them. The only char* object anywhere
 * in the map is that accessor's ptr field, so chmap_get_ptr() on a
 * string-valued map can only hand back the address of that field. Storing a
 * different char* through that address would replace the pointer while the
 * size still describes the previous string, and it would give the map a
 * pointer it doesn't own and whose lifetime it can't control. Without the
 * const, chmap_get_elem_copy() and every read of an iterator value would
 * then walk the new buffer, which can be much shorter, for the length of
 * the previous string: AddressSanitizer reports a global-buffer-overflow
 * read of the old length on the first such read.
 *
 * With the const that store is a compile error. The caller can still read
 * the stored char* through the returned pointer and write to the string
 * bytes it points at, within the stored length. To replace a string value
 * use chmap_insert(), which frees the old bytes and copies the new ones
 * into storage that the map owns. */
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
 * The type-inferred wrapper for chmap_get_elem_ref() that gives a pointer to
 * the value, or NULL when the key is not present. Unlike chmap_get() it
 * doesn't stop the program for a missing key.
 *
 * @param hm_name The hash map to search
 * @param key The key to look up
 *
 * @return Pointer to the value, or NULL when the key is not present
 *
 * @note It gives NULL for a missing key and doesn't stop the program
 * @note It stops the program when the size of the value doesn't match the
 * size of the type
 * @note You get a pointer to the value, so you can change the value in place
 * @note A later insert, delete or resize makes the pointer invalid
 * @note For a char* value type the target of the pointer is const-qualified
 * (char *const *). You can read the stored string and edit its bytes in
 * place within the stored length, but replacing the pointer itself is a
 * compile error. The map owns the string bytes and keeps its own accessor in
 * step with them, so replacing the pointer would leave the accessor's length
 * describing the previous string. To replace a string value use
 * chmap_insert(), which frees the old bytes and copies the new ones into
 * storage that the map owns.
 *
 * @note A NULL key of a character-pointer type is not a string, and you get
 * NULL for it
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
      _ccol_uniq(__ccol_chmap_get_ptr_acc, __COUNTER__),                     \
      _ccol_uniq(__ccol_chmap_get_ptr_r, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro can nest inside the argument of any public
 * macro, itself included. */
#define _ccol_chmap_get_ptr_impl(                                            \
    hm_name, key, __ccol_chmap_key_tmp, __ccol_chmap_val, __ccol_chmap_kp,   \
    __ccol_chmap_vp, __ccol_chmap_acc, __ccol_chmap_r)                       \
  ({                                                                         \
    /* The temporary has the map's DECLARED key type. chmap_insert()         \
     * explains why the lookup key goes through a conversion and not a raw   \
     * byte copy. A key that the map stored through that conversion can only \
     * be found again by a lookup that does the same conversion. */          \
    _ccol_declared_or_own_type(hm_name##__ccol_key_type_var, (key))          \
        __ccol_chmap_key_tmp = (key);                                        \
    _ccol_clear_padding(&__ccol_chmap_key_tmp);                              \
    __typeof__(*hm_name##__ccol_val_type_var) *__ccol_chmap_val = NULL;      \
    cmap_pair *__ccol_chmap_kp = &(cmap_pair){};                             \
    /* The lookup fills this pair on every path and we read it only after a  \
     * success, so it doesn't need an initializer of its own. */             \
    cmap_pair __ccol_chmap_vp;                                               \
    const cmap_pair *__ccol_chmap_acc = NULL;                                \
    ccol_retval_t __ccol_chmap_r;                                            \
    _populate_cmap_pair(__ccol_chmap_kp, __ccol_chmap_key_tmp);              \
    if (_ccol_is_char_ptr(*hm_name##__ccol_val_type_var)) {                  \
      /* The entry's only char* object is the ptr field of the accessor the  \
       * map keeps for it, so the result points there. */                    \
      __ccol_chmap_r = _chmap_get_val_accessor(hm_name, __ccol_chmap_kp,     \
                                               &__ccol_chmap_acc);           \
      if (__ccol_chmap_r == ccol_success) {                                  \
        __ccol_chmap_val = (__typeof__(*hm_name##__ccol_val_type_var) *)&(   \
            __ccol_chmap_acc->ptr);                                          \
      }                                                                      \
    } else {                                                                 \
      __ccol_chmap_r =                                                       \
          chmap_get_elem_ref(hm_name, __ccol_chmap_kp, &__ccol_chmap_vp);    \
      if (__ccol_chmap_r == ccol_success) {                                  \
        if (__ccol_chmap_vp.size != sizeof(*__ccol_chmap_val)) {             \
          _ccol_fatal_err(                                                   \
              "chmap_get_ptr('%s'): value size mismatch - stored: %lu "      \
              "bytes, requested: %lu bytes; wrong type or missing "          \
              "chmap_redeclare()?",                                          \
              #hm_name, (unsigned long)__ccol_chmap_vp.size,                 \
              (unsigned long)sizeof(*__ccol_chmap_val));                     \
        }                                                                    \
        __ccol_chmap_val =                                                   \
            (__typeof__(*hm_name##__ccol_val_type_var) *)(__ccol_chmap_vp    \
                                                              .ptr);         \
      }                                                                      \
    }                                                                        \
    _ccol_chmap_value_ptr_result(hm_name, __ccol_chmap_val);                 \
  })

#pragma GCC visibility pop
