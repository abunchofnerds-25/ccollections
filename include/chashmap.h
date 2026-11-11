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

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// A hash map that picks its backend from the key and value types.
//
// Open addressing is for keys and values that are both integral and at most
// 8 bytes: 16-byte slots of key and value with the occupied bits in a byte
// array at the tail of the same block, linear probing, backward-shift
// deletion (no deleted-slot markers), doubling past a load of 0.70 and
// halving under 0.25. It allocates nothing per entry and iterates in slot
// order.
//
// Separate chaining is for everything else (strings, long double, structs):
// chained nodes with 23 bytes of inline storage for a short key or value, at
// least 16 buckets (a power of two), growing by 4 past (buckets + 1) * 1.5
// entries and shrinking by 4 under (buckets + 1) / 8. Its entries sit on one
// list, so iteration goes newest first.
//
// int->int, long->double and float->uint32_t use open addressing;
// string->int, int->string and int->long double use separate chaining. Both
// accept a custom hash, which goes through a finalizer, and both start in a
// cheap fast hash mode and switch to a keyed one when the keys collide too
// much (see chashmap_create_full()).

// The default first size of the bucket array.
#define CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE 16

// The hash map. The struct is opaque and chmap is the handle.
typedef struct chashmap chashmap;

typedef chashmap *chmap;

// Creation

// Creates a map with every option. initial_bucket_array_size can't be 0; 1 to
// 16 gives 16, and a larger value is rounded up to a power of two. key_type and
// val_type pick the backend: open addressing when both are integral and at
// most 8 bytes (char to long long, float, double; never long double) and
// custom_key_equality_proc is NULL, separate chaining otherwise. mmgmt_procs
// is your allocator or NULL. A custom_key_equality_proc needs a
// custom_hashing_proc that agrees with it (equal keys hash the same), or the
// call fails. On failure it gives NULL and an error string in *err. Destroy the
// map with chmap_destroy().
//
// Without custom procs a key of an unknown type (a struct) is compared and
// hashed byte by byte, padding included. Zero such a key with memset() before
// you set its members when you use the raw functions. Through the typed macros
// the map sees the macro's copy of the key; GCC 11 and later zero its padding,
// and with any other compiler a padded key needs both procs.
//
// A float or double -0.0 key becomes 0.0 before it's hashed, stored or
// compared, custom hash included. A long double key always uses separate
// chaining and is hashed and compared by value: -0.0L equals 0.0L, padding
// bits never matter, and every NaN is one key. A custom hash for long double
// gets the raw bytes, so it must be value-based itself (hash frexpl(), say), or
// equal values can land in different buckets.
//
// Every map starts in the fast mode: a Fibonacci multiply for integral, float,
// double and pointer keys, XXH64 with a secret seed for strings and other
// buffers, and the murmur3 finalizer on a custom hash. Inserts of new keys are
// measured, and a map whose keys collide more than a random function would let
// them (an insert far from its home slot, a chain of 20 nodes, or 256 inserts
// that cost too much in total) switches to the keyed mode: a seeded
// two-multiply mixer for fixed-width keys and custom results, SipHash-1-3 for
// buffers. A lookup never writes to the map. The switch keeps every entry, and
// under separate chaining every reference, the insertion order and every
// iterator; when an open-addressing rebuild can't allocate, the map stays fast
// and tries again later. Each growth of a keyed map tries the fast mode again.
// The secrets come from getrandom(2) once per process, and which keys are the
// same key never depends on the mode. chashmap_create_full(3) gives the exact
// thresholds and what an attacker can still force.
chmap chashmap_create_full(size_t initial_bucket_array_size,
                           ccol_data_type key_type, ccol_data_type val_type,
                           ccol_memmgmt_procs *mmgmt_procs,
                           ccol_hashing_proc custom_hashing_proc,
                           ccol_key_equality_proc custom_key_equality_proc,
                           char **err);

// Creates a map with the default allocator and the default hash, for the
// common case; see chashmap_create_full() for the arguments and the result.
static inline __attribute__((always_inline)) chmap
chashmap_create(size_t initial_bucket_array_size, ccol_data_type key_type,
                ccol_data_type val_type, char **err) {
  return chashmap_create_full(initial_bucket_array_size, key_type, val_type,
                              NULL, NULL, NULL, err);
}

// Creates a map that allocates through mmgmt_procs, with the default hash; see
// chashmap_create_full().
static inline __attribute__((always_inline)) chmap chashmap_create_mp(
    size_t initial_bucket_array_size, ccol_data_type key_type,
    ccol_data_type val_type, ccol_memmgmt_procs *mmgmt_procs, char **err) {
  return chashmap_create_full(initial_bucket_array_size, key_type, val_type,
                              mmgmt_procs, NULL, NULL, err);
}

// Creates a map that hashes keys with custom_hashing_proc, with the default
// allocator; see chashmap_create_full().
static inline __attribute__((always_inline)) chmap
chashmap_create_ch(size_t initial_bucket_array_size, ccol_data_type key_type,
                   ccol_data_type val_type,
                   ccol_hashing_proc custom_hashing_proc, char **err) {
  return chashmap_create_full(initial_bucket_array_size, key_type, val_type,
                              NULL, custom_hashing_proc, NULL, err);
}

// Operations

// Gives the number of key-value pairs the map holds. Asserts on a NULL chm.
size_t chashmap_elem_count(chmap chm);

// Removes every element, so a map can be reused without a new allocation, and
// resizes the array to new_bucket_array_size rounded up to a power of two (16
// at least) unless it's 0. It gives ccol_not_enough_memory when the resize
// fails, but every element is gone anyway. Asserts on a NULL chm.
ccol_retval chashmap_reset(chmap chm, size_t new_bucket_array_size);

// Inserts the pair, or replaces the value when the key is present (the key
// stays); the raw insert behind chmap_insert(), for code that must not stop
// the program. The map copies both. It gives ccol_success for a new key and
// ccol_key_already_present for an update, which is also a success. It gives
// ccol_invalid_args for a NULL pointer, a size of 0, or a key or value size
// that doesn't match a fixed-width declared type (char to long long, float,
// double, long double, pointers; ccol_string and ccol_other_types aren't
// checked), ccol_container_full at ccol_max_elem_count and
// ccol_not_enough_memory. key_pair and val_pair may point into this map (a
// chashmap_get_elem_ref() result, say), even when the insert grows it. Open
// addressing doubles past a load of 0.70; chaining grows by 4 once
// elem_count >= (bucket_count + 1) * 1.5.
ccol_retval chashmap_insert_elem(chmap chm, const cmap_pair *key_pair,
                                 const cmap_pair *val_pair);

// Copies the value of the key into your buffer, so you keep a copy that no
// later change of the map affects: min(stored size, target_buf_size) bytes,
// with the rest of target_buf zeroed. It gives ccol_success,
// ccol_key_not_found, or ccol_invalid_args for a NULL pointer, a size of 0 or
// a key size that doesn't match a fixed-width key type.
ccol_retval chashmap_get_elem_copy(chmap chm, const cmap_pair *key_pair,
                                   void *target_buf, size_t target_buf_size);

// Finds the value of the key without copying it: fills your *val_pair with the
// location and size of the stored value. You may change those bytes in place,
// but not ptr or size themselves (that changes only your copy). The pointer is
// valid until an insert, delete or resize; lookups of other keys never
// invalidate it. It gives ccol_success, ccol_key_not_found or
// ccol_invalid_args (as for chashmap_get_elem_copy()), and every failure with
// a non-NULL val_pair sets it to {NULL, 0}.
ccol_retval chashmap_get_elem_ref(chmap chm, const cmap_pair *key_pair,
                                  cmap_pair *val_pair);

// Internal: gives the accessor a separate-chaining map keeps for the value of a
// key, whose ptr field is the stored char *; the string arm of chmap_get_ptr()
// needs its address. An open-addressing map gives ccol_invalid_args, and every
// failure sets *val_accessor to NULL. Use chmap_get_ptr() instead.
ccol_retval chashmap__get_val_accessor(chmap chm, const cmap_pair *key_pair,
                                       const cmap_pair **val_accessor);

// Deletes the key and frees its key and value; the raw delete behind
// chmap_remove(). It gives ccol_success, ccol_key_not_found or
// ccol_invalid_args (as for chashmap_get_elem_copy()). An open-addressing
// delete moves later entries of the cluster back, so every reference into the
// map is invalid after it. Open addressing halves below a load of 0.25 and
// chaining shrinks by 4 once elem_count < (bucket_count + 1) / 8, never below
// the minimum.
ccol_retval chashmap_delete_elem(chmap chm, const cmap_pair *key_pair);

// Iteration

// Internal: frees an iterator that chashmap_begin_iter() (declared in
// citerators.h) made; every map iterator calls it through its free function.
void chashmap__iterator_destroy(cmap_iterator *iter);

// Destruction

// Internal: frees every key, value and bucket of the map; the body of
// chashmap_destroy(). Use the macro instead.
void chashmap__destroy(chmap chm);

// Destroys the map and calls val_dtor (when not NULL) with dtor_ctx once for
// each live value, right before its entry is freed. It's meant for maps of
// owned pointers, and it allocates nothing, so it reaches every value even
// when memory runs out, which a walk with chashmap_begin_iter() can't promise.
// val_pair is const and points into storage that is about to go away; read it
// and free what it owns. val_dtor must not change the map. A NULL chm does
// nothing, and chm isn't set to NULL.
void chashmap_destroy_with_dtor(chmap chm,
                                void (*val_dtor)(const cmap_pair *val_pair,
                                                 void *dtor_ctx),
                                void *dtor_ctx);

// Internal: destroys *chm and sets it to NULL; the cleanup handler that the
// _scoped macros attach with ccol__destructor.
static inline void chashmap__destroy_cleanup(chmap *chm) {
  if (chm && *chm) {
    chashmap__destroy(*chm);
    *chm = NULL;
  }
}

// Frees every key, value and bucket of the map and sets chm to NULL, so the
// handle can't be used after the free. chm must be a modifiable lvalue and is
// evaluated once; NULL is fine.
#define chashmap_destroy(chm)  \
  ccol__chashmap_destroy_impl( \
      chm, ccol__uniq(ccol__chashmap_destroy_slot, __COUNTER__))

// Internal: the body of chashmap_destroy(). It takes the address of chm once
// into slot, a name from ccol__uniq(), so destroy macros can nest without
// -Wshadow warnings.
#define ccol__chashmap_destroy_impl(chm, slot) \
  do {                                         \
    __typeof__(chm) *slot = &(chm);            \
    chashmap__destroy(*slot);                  \
    *slot = NULL;                              \
  } while (0)

// Type-inferred declaration and creation macros

// Type-inferred macros. chmap_redeclare declares the companion type variables
// for a map made in another scope, because the typed macros read the key and
// value types from them.
//
//   void process(chmap map) {
//     chmap_redeclare(map, int, char *);
//     chmap_insert(map, 42, "hello");
//   }
#define chmap_redeclare(hm_name, KeyT, ValT)                               \
  ccol__refuse_array_types("chmap", KeyT, ValT);                           \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) = NULL

// Declare a map hm_name with its companion type variables, which the typed
// macros read. chmap_declare leaves it uninitialized (use a chmap_init macro
// next); chmap_declare_scoped sets it to NULL and destroys it at the end of
// the scope. Array key or value types are refused at compile time. The unused
// attribute sits on the pointer type because this macro also declares struct
// members, and GCC before 11 refuses it on a member.
#define chmap_declare(hm_name, KeyT, ValT)                                \
  ccol__refuse_array_types("chmap", KeyT, ValT);                          \
  __typeof__(KeyT) *__attribute__((unused)) hm_name##__ccol_key_type_var; \
  __typeof__(ValT) *__attribute__((unused)) hm_name##__ccol_val_type_var; \
  chmap hm_name

#define chmap_declare_scoped(hm_name, KeyT, ValT)                          \
  ccol__refuse_array_types("chmap", KeyT, ValT);                           \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) = \
      NULL;                                                                \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) = \
      NULL;                                                                \
  chmap hm_name ccol__destructor(chashmap__destroy_cleanup) = NULL

// Creates the map for a declared hm_name with your allocator, hash and key
// equality (any of them NULL), with CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE.
// The key and value types come from the companions, so you don't repeat them.
// It stops the program with ccol__fatal_err() on failure. See
// chashmap_create_full() for the rule that ties the two procs together.
#define chmap_init_full(hm_name, mmgmt_procs, custom_hashing_proc,          \
                        custom_key_equality_proc)                           \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_full(                                         \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),   \
        &ccol__chmap_err);                                                  \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Declare and create a map with every option in one step: chmap_declare (or
// chmap_declare_scoped) plus chmap_init_full. They stop the program on
// failure.
#define chmap_construct_full(hm_name, KeyT, ValT, mmgmt_procs,              \
                             custom_hashing_proc, custom_key_equality_proc) \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_full(                                         \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),   \
        &ccol__chmap_err);                                                  \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define chmap_construct_full_scoped(hm_name, KeyT, ValT, mmgmt_procs,       \
                                    custom_hashing_proc,                    \
                                    custom_key_equality_proc)               \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name ccol__destructor(chashmap__destroy_cleanup) = NULL;         \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_full(                                         \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (mmgmt_procs), (custom_hashing_proc), (custom_key_equality_proc),   \
        &ccol__chmap_err);                                                  \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Creates the map for a declared hm_name with the default allocator and hash,
// reading the types from the companions. It stops the program on failure.
#define chmap_init(hm_name)                                                 \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create(                                              \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        &ccol__chmap_err);                                                  \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Declare and create a map with the default settings in one step, the usual
// way to start; the scoped form destroys it at the end of the scope. They stop
// the program on failure.
//
//   chmap_construct(ages, char *, int);    // separate chaining
//   chmap_insert(ages, "Alice", 30);
//   int age = chmap_get(ages, "Alice");    // 30
//   chmap_destroy(ages);
#define chmap_construct(hm_name, KeyT, ValT)                                \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create(                                              \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        &ccol__chmap_err);                                                  \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define chmap_construct_scoped(hm_name, KeyT, ValT)                         \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name ccol__destructor(chashmap__destroy_cleanup) = NULL;         \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create(                                              \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        &ccol__chmap_err);                                                  \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Creates the map for a declared hm_name that allocates through mmgmt_procs.
// It stops the program on failure.
#define chmap_init_mp(hm_name, mmgmt_procs)                                 \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_mp(                                           \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (mmgmt_procs), &ccol__chmap_err);                                   \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Declare and create a map that allocates through mmgmt_procs; the scoped
// form destroys it at the end of the scope. They stop the program on
// failure.
#define chmap_construct_mp(hm_name, KeyT, ValT, mmgmt_procs)                \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_mp(                                           \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (mmgmt_procs), &ccol__chmap_err);                                   \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define chmap_construct_mp_scoped(hm_name, KeyT, ValT, mmgmt_procs)         \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name ccol__destructor(chashmap__destroy_cleanup) = NULL;         \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_mp(                                           \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (mmgmt_procs), &ccol__chmap_err);                                   \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Creates the map for a declared hm_name that hashes with
// custom_hashing_proc. It stops the program on failure.
#define chmap_init_ch(hm_name, custom_hashing_proc)                         \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_ch(                                           \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (custom_hashing_proc), &ccol__chmap_err);                           \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Declare and create a map that hashes with custom_hashing_proc; the scoped
// form destroys it at the end of the scope. They stop the program on
// failure.
#define chmap_construct_ch(hm_name, KeyT, ValT, custom_hashing_proc)        \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name = NULL;                                                     \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_ch(                                           \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (custom_hashing_proc), &ccol__chmap_err);                           \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

#define chmap_construct_ch_scoped(hm_name, KeyT, ValT, custom_hashing_proc) \
  ccol__refuse_array_types("chmap", KeyT, ValT);                            \
  __typeof__(KeyT) *hm_name##__ccol_key_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  __typeof__(ValT) *hm_name##__ccol_val_type_var __attribute__((unused)) =  \
      NULL;                                                                 \
  chmap hm_name ccol__destructor(chashmap__destroy_cleanup) = NULL;         \
  do {                                                                      \
    char *ccol__chmap_err = NULL;                                           \
    hm_name = chashmap_create_ch(                                           \
        CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE,                            \
        ccol_determine_ccol_data_type(*hm_name##__ccol_key_type_var),       \
        ccol_determine_ccol_data_type(*hm_name##__ccol_val_type_var),       \
        (custom_hashing_proc), &ccol__chmap_err);                           \
    if (!hm_name) {                                                         \
      ccol__fatal_err("Failed to create hash map '%s': %s", #hm_name,       \
                      ccol__chmap_err ? ccol__chmap_err : "unknown error"); \
    }                                                                       \
  } while (0)

// Type-inferred operation macros

// Destroys the map and sets chm to NULL; the typed-layer name of
// chashmap_destroy().
#define chmap_destroy(chm) chashmap_destroy(chm)

// Inserts or updates key with val, the everyday insert, and stops the program
// when that fails. It evaluates each argument once. Both are converted to the
// declared types like a plain assignment (never a reinterpretation of bytes;
// no implicit conversion means a compile error), so a float key in an int map
// is found again by an int lookup. A character pointer type keeps the
// expression's own pointer type. A NULL string key or value stops the program
// with ccol_invalid_args.
#define chmap_insert(hm_name, key, ccol__chmap_val)                            \
  ccol__chmap_insert_impl(hm_name, (key), (ccol__chmap_val),                   \
                          ccol__uniq(ccol__chmap_insert_key_tmp, __COUNTER__), \
                          ccol__uniq(ccol__chmap_insert_val_tmp, __COUNTER__), \
                          ccol__uniq(ccol__chmap_insert_kp, __COUNTER__),      \
                          ccol__uniq(ccol__chmap_insert_vp, __COUNTER__),      \
                          ccol__uniq(ccol__chmap_insert_r, __COUNTER__))

// Internal: the body of chmap_insert(). It copies key and val into
// temporaries of the declared types and calls chashmap_insert_elem(). The
// temporaries get their names from ccol__uniq(), so the macro nests, and they
// keep a copy of each expression for the whole call, so an rvalue works.
#define ccol__chmap_insert_impl(hm_name, key, ccol__chmap_val,                 \
                                ccol__chmap_key_tmp, ccol__chmap_val_tmp,      \
                                ccol__chmap_kp, ccol__chmap_vp, ccol__chmap_r) \
  do {                                                                         \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        ccol__chmap_key_tmp = (key);                                           \
    ccol__clear_padding(&ccol__chmap_key_tmp);                                 \
    ccol__declared_or_own_type(hm_name##__ccol_val_type_var,                   \
                               (ccol__chmap_val)) ccol__chmap_val_tmp =        \
        (ccol__chmap_val);                                                     \
    cmap_pair *ccol__chmap_kp = &(cmap_pair){};                                \
    cmap_pair *ccol__chmap_vp = &(cmap_pair){};                                \
    ccol__populate_cmap_pair(ccol__chmap_kp, ccol__chmap_key_tmp);             \
    ccol__populate_cmap_pair(ccol__chmap_vp, ccol__chmap_val_tmp);             \
    ccol_retval ccol__chmap_r =                                                \
        chashmap_insert_elem(hm_name, ccol__chmap_kp, ccol__chmap_vp);         \
    if (ccol__chmap_r != ccol_success &&                                       \
        ccol__chmap_r != ccol_key_already_present) {                           \
      ccol__dump_key_to_stderr(ccol__chmap_kp->ptr, ccol__chmap_kp->size);     \
      ccol__fatal_err("chmap_insert('%s'): r: %d (%s)", #hm_name,              \
                      ccol__chmap_r, ccol_retval_to_str(ccol__chmap_r));       \
    }                                                                          \
  } while (0)

// Removes key from the map and evaluates to ccol_success, ccol_key_not_found,
// or ccol_invalid_args for a NULL string key. Unlike the other typed macros it
// never stops the program, because a missing key is an ordinary outcome.
#define chmap_remove(hm_name, key)                                             \
  ccol__chmap_remove_impl(hm_name, (key),                                      \
                          ccol__uniq(ccol__chmap_remove_key_tmp, __COUNTER__), \
                          ccol__uniq(ccol__chmap_remove_kp, __COUNTER__),      \
                          ccol__uniq(ccol__chmap_remove_r, __COUNTER__))

// Internal: the body of chmap_remove(). The key goes through a temporary of
// the declared key type, as in chmap_insert(), so it finds what an insert
// stored.
#define ccol__chmap_remove_impl(hm_name, key, ccol__chmap_key_tmp,             \
                                ccol__chmap_kp, ccol__chmap_r)                 \
  ({                                                                           \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        ccol__chmap_key_tmp = (key);                                           \
    ccol__clear_padding(&ccol__chmap_key_tmp);                                 \
    cmap_pair *ccol__chmap_kp = &(cmap_pair){};                                \
    ccol__populate_cmap_pair(ccol__chmap_kp, ccol__chmap_key_tmp);             \
    ccol_retval ccol__chmap_r = chashmap_delete_elem(hm_name, ccol__chmap_kp); \
    ccol__chmap_r;                                                             \
  })

// Evaluates to the value of key itself (the char * for a string), for a key
// you know is there. It stops the program when the key is missing, for a NULL
// string key, and when the stored size doesn't match the declared value type.
#define chmap_get(hm_name, key)                                          \
  ccol__chmap_get_impl(hm_name, (key),                                   \
                       ccol__uniq(ccol__chmap_get_key_tmp, __COUNTER__), \
                       ccol__uniq(ccol__chmap_get_val, __COUNTER__),     \
                       ccol__uniq(ccol__chmap_get_kp, __COUNTER__),      \
                       ccol__uniq(ccol__chmap_get_vp, __COUNTER__),      \
                       ccol__uniq(ccol__chmap_get_r, __COUNTER__))

// Internal: the body of chmap_get(). The key goes through a temporary of the
// declared key type, as in chmap_insert(), so it finds what an insert stored.
#define ccol__chmap_get_impl(hm_name, key, ccol__chmap_key_tmp,               \
                             ccol__chmap_val, ccol__chmap_kp, ccol__chmap_vp, \
                             ccol__chmap_r)                                   \
  ({                                                                          \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))           \
        ccol__chmap_key_tmp = (key);                                          \
    ccol__clear_padding(&ccol__chmap_key_tmp);                                \
    __typeof__(*hm_name##__ccol_val_type_var) *ccol__chmap_val = NULL;        \
    cmap_pair *ccol__chmap_kp = &(cmap_pair){};                               \
    cmap_pair ccol__chmap_vp = {NULL, 0};                                     \
    ccol__populate_cmap_pair(ccol__chmap_kp, ccol__chmap_key_tmp);            \
    ccol_retval ccol__chmap_r =                                               \
        chashmap_get_elem_ref(hm_name, ccol__chmap_kp, &ccol__chmap_vp);      \
    if (ccol__chmap_r != ccol_success) {                                      \
      ccol__dump_key_to_stderr(ccol__chmap_kp->ptr, ccol__chmap_kp->size);    \
      ccol__fatal_err("chmap_get('%s'): r: %d (%s)", #hm_name, ccol__chmap_r, \
                      ccol_retval_to_str(ccol__chmap_r));                     \
    }                                                                         \
    if (ccol__is_char_ptr(*hm_name##__ccol_val_type_var)) {                   \
      ccol__chmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)&(ccol__chmap_vp.ptr); \
    } else if (ccol__chmap_vp.size != sizeof(*ccol__chmap_val)) {             \
      ccol__fatal_err(                                                        \
          "chmap_get('%s'): value size mismatch - stored: %lu bytes, "        \
          "requested: %lu bytes; wrong type or missing chmap_redeclare()?",   \
          #hm_name, (unsigned long)ccol__chmap_vp.size,                       \
          (unsigned long)sizeof(*ccol__chmap_val));                           \
    } else {                                                                  \
      ccol__chmap_val =                                                       \
          (__typeof__(*hm_name##__ccol_val_type_var) *)(ccol__chmap_vp.ptr);  \
    }                                                                         \
    *ccol__chmap_val;                                                         \
  })

// Internal: converts p to the result type of chmap_get_ptr(). For a string
// value type the target becomes const, because the only char * in the entry
// is the map's own accessor; storing another pointer there would leave its
// size describing the old string. Every other type comes back unchanged.
#define ccol__chmap_value_ptr_result(hm_name, p)                               \
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

// Evaluates to a pointer to the value of key, or NULL when it's missing (or
// for a NULL string key), for a lookup that may miss or that changes the value
// in place; the pointer is valid until the next insert, delete or resize. It
// stops the program when the stored size doesn't match the declared type. For
// a char * value the result is char *const *: edit the bytes of the string
// within its length, and use chmap_insert() to replace it.
//
//   char *const *sptr = chmap_get_ptr(names, key);
//   if (sptr) (*sptr)[0] = 'j';
#define chmap_get_ptr(hm_name, key)                                         \
  ccol__chmap_get_ptr_impl(                                                 \
      hm_name, (key), ccol__uniq(ccol__chmap_get_ptr_key_tmp, __COUNTER__), \
      ccol__uniq(ccol__chmap_get_ptr_val, __COUNTER__),                     \
      ccol__uniq(ccol__chmap_get_ptr_kp, __COUNTER__),                      \
      ccol__uniq(ccol__chmap_get_ptr_vp, __COUNTER__),                      \
      ccol__uniq(ccol__chmap_get_ptr_acc, __COUNTER__),                     \
      ccol__uniq(ccol__chmap_get_ptr_r, __COUNTER__))

// Internal: the body of chmap_get_ptr(). Strings go through
// chashmap__get_val_accessor(), every other type through
// chashmap_get_elem_ref(). The value pair needs no initializer, because the
// lookup fills it on every path and it's read only after a success. The
// string arm goes through uintptr_t to drop the library's const without a
// -Wcast-qual warning in the caller's build.
#define ccol__chmap_get_ptr_impl(                                              \
    hm_name, key, ccol__chmap_key_tmp, ccol__chmap_val, ccol__chmap_kp,        \
    ccol__chmap_vp, ccol__chmap_acc, ccol__chmap_r)                            \
  ({                                                                           \
    ccol__declared_or_own_type(hm_name##__ccol_key_type_var, (key))            \
        ccol__chmap_key_tmp = (key);                                           \
    ccol__clear_padding(&ccol__chmap_key_tmp);                                 \
    __typeof__(*hm_name##__ccol_val_type_var) *ccol__chmap_val = NULL;         \
    cmap_pair *ccol__chmap_kp = &(cmap_pair){};                                \
    cmap_pair ccol__chmap_vp;                                                  \
    const cmap_pair *ccol__chmap_acc = NULL;                                   \
    ccol_retval ccol__chmap_r;                                                 \
    ccol__populate_cmap_pair(ccol__chmap_kp, ccol__chmap_key_tmp);             \
    if (ccol__is_char_ptr(*hm_name##__ccol_val_type_var)) {                    \
      ccol__chmap_r = chashmap__get_val_accessor(hm_name, ccol__chmap_kp,      \
                                                 &ccol__chmap_acc);            \
      if (ccol__chmap_r == ccol_success) {                                     \
        ccol__chmap_val = (__typeof__(*hm_name##__ccol_val_type_var)           \
                               *)(uintptr_t)&ccol__chmap_acc->ptr;             \
      }                                                                        \
    } else {                                                                   \
      ccol__chmap_r =                                                          \
          chashmap_get_elem_ref(hm_name, ccol__chmap_kp, &ccol__chmap_vp);     \
      if (ccol__chmap_r == ccol_success) {                                     \
        if (ccol__chmap_vp.size != sizeof(*ccol__chmap_val)) {                 \
          ccol__fatal_err(                                                     \
              "chmap_get_ptr('%s'): value size mismatch - stored: %lu "        \
              "bytes, requested: %lu bytes; wrong type or missing "            \
              "chmap_redeclare()?",                                            \
              #hm_name, (unsigned long)ccol__chmap_vp.size,                    \
              (unsigned long)sizeof(*ccol__chmap_val));                        \
        }                                                                      \
        ccol__chmap_val =                                                      \
            (__typeof__(*hm_name##__ccol_val_type_var) *)(ccol__chmap_vp.ptr); \
      }                                                                        \
    }                                                                          \
    (__typeof__(ccol__chmap_value_ptr_result(                                  \
        hm_name,                                                               \
        (__typeof__(*hm_name##__ccol_val_type_var) *)0)))(ccol__chmap_val);    \
  })

// Evaluate to the entry count of the map, and empty it (see chashmap_reset());
// the typed-layer names of chashmap_elem_count() and chashmap_reset().
#define chmap_size(chm) chashmap_elem_count((chm))

#define chmap_reset(chm, new_bucket_array_size) \
  chashmap_reset((chm), (new_bucket_array_size))

#pragma GCC visibility pop
