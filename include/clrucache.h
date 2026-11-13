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

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif
#include <string.h>

#include "common.h"

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// The remote getter type. It's called for a missing key when a remote source
// exists. On success it allocates the value with the cache's allocator (the
// custom procs or malloc), stores it in val and returns true; the cache owns
// the value from then on. On failure it leaves val->ptr NULL and returns false.
// No cache lock is held during the call, so it may use the cache for other
// keys, but not for the key it serves and not to destroy the cache, which would
// deadlock.
typedef bool (*ccache_remote_getter_fn)(const cmap_pair *key, cmap_pair *val);

// The remote setter type: Writes key and val to the remote source and returns
// true on success. No cache lock is held during the call; the rules of the
// getter about the same key and the destroy apply.
typedef bool (*ccache_remote_setter_fn)(const cmap_pair *key,
                                        const cmap_pair *val);

// The eviction callback type: Gets the key and value of an entry evicted to
// make room. It runs with the owning segment's lock held, so it must not call
// back into the cache, and evictions in two segments can run it at the same
// time.
typedef void (*ccache_eviction_fn)(const cmap_pair *key, const cmap_pair *val);

// The opaque cache: A thread-safe least recently used cache with an optional
// remote source. A hash map gives O(1) lookup and a linked list O(1) eviction.
// From a capacity of 128 on, the cache is split into segments with their own
// locks, each key belongs to one segment by its hash, and each segment evicts
// its own least recently used entry; below 128 there is one segment and one
// exact order.
//
// Getters of the same missing key join one remote fetch and all get its
// result. A getter waits while a set of its key runs; when the set fails it
// starts over as an ordinary miss. Setters of one key are serialized. The
// remote setter always runs before the cache changes, and every allocation of a
// set happens before it, so ccol_not_enough_memory or ccol_container_full means
// the setter never ran. A failed set changes nothing (value and place in the
// eviction order stay); after a successful setter the cache update can't fail.
// While the setter runs the key stays an ordinary entry that an insert may
// evict; a successful set then inserts it again as the newest entry.
//
// The eviction callback runs with its segment's lock held and must not call
// back into the cache; two segments can run it at the same time. A key of a
// type the cache doesn't know (a struct) is compared by all its bytes, padding
// included: Zero such a key with memset() before using the raw functions.
// ccache_get() and ccache_set() clear the padding of their own copy with GCC 11
// or later; with other compilers use the raw functions for padded keys.
typedef struct clrucache clrucache;

// The handle is an opaque value (slot index plus generation), never a pointer.
// Compare it with CCACHE_NULL, which is 0, so `if (!cache)` works. Every use
// goes through the library's slot table, so a freed or reused slot is always
// detected.
typedef uint64_t ccache;

// "No cache": The failure value of a create and what a destroy leaves in the
// handle, so a handle can be tested for validity.
#define CCACHE_NULL ((ccache)0)

// Creates a thread-safe least recently used cache that holds up to capacity
// live entries (0 is refused); a set into a full cache evicts the least
// recently used entry. key_type and val_type describe the key and the value
// (ccol_determine_ccol_data_type() gives them for a C type). getter fetches a
// missing key from a remote source, setter writes each set to it before the
// cache changes, and eviction_cb hears about every evicted entry; each may be
// NULL. mprocs is your allocator table or NULL for the default; the cache
// keeps a copy of the struct, but its functions must stay callable until the
// destroy. Returns the handle, or CCACHE_NULL with *err set (also for a create
// after the library's destructor released the handle table). Destroy it with
// clrucache_destroy().
//
// From a capacity of 128 the capacity is divided across segments that evict on
// their own, so a full cache holds a few percent less than capacity;
// clrucache_capacity() reports capacity itself.
ccache clrucache_create_full(size_t capacity, ccol_data_type key_type,
                             ccol_data_type val_type,
                             ccache_remote_getter_fn getter,
                             ccache_remote_setter_fn setter,
                             ccache_eviction_fn eviction_cb,
                             ccol_memmgmt_procs *mprocs, char **err);

// Internal: Destroys a cache. It's the body of ccache_destroy(); use the macro,
// which also clears the handle. CCACHE_NULL is a no-op; any other handle that
// isn't live (already destroyed, being destroyed, garbage) is a fatal error.
// It waits for every call that resolved the handle, evicts the remaining
// entries through the eviction callback and frees everything.
void clrucache__destroy(ccache cache);

// Internal: Copies the value of key_pair into buf without a heap allocation.
// ccache_get() uses it for every value type other than char *, so a get
// allocates nothing. buf_size must equal the stored size (ccache_get passes
// sizeof(ValT)); any other size gives ccol_unexpected_failure and leaves buf
// untouched.
ccol_retval clrucache__get_into(ccache cache, const cmap_pair *key_pair,
                                void *buf, size_t buf_size);

// Looks key_pair up and gives a heap copy of its value in *val_out; you own
// val_out->ptr and free it with the cache's allocator (free() for the
// default). A hit makes the key the most recently used. It's the raw get, for
// callers that don't use the typed macros. On a miss the remote getter runs
// once and every other getter of that key waits for its result; a getter also
// waits while a set of the key runs. A fixed-width key must have the width of
// its type, or the call gives ccol_invalid_args without reading it. Returns
// ccol_success, ccol_key_not_found (missing and no getter, or the getter
// failed), ccol_invalid_args (also for CCACHE_NULL or a destroyed handle),
// ccol_not_enough_memory, ccol_unexpected_failure (a joined fetch stored a
// value of the wrong size) or ccol_container_full (the internal records are
// full).
ccol_retval clrucache_get_full(ccache cache, const cmap_pair *key_pair,
                               cmap_pair *val_out);

// Copies val_pair into the cache under key_pair, after the remote setter (if
// any) succeeded; it's the raw set. The key becomes its segment's newest entry,
// and a full cache evicts its least recently used entry to make room. A failed
// setter gives ccol_unexpected_failure and changes nothing in the cache. Every
// allocation happens before the setter runs, so ccol_not_enough_memory means
// the setter never ran. Setters of one key are serialized, and a getter of the
// key waits for the set. Returns ccol_success, ccol_invalid_args (also for
// CCACHE_NULL, a destroyed handle or a value of size 0),
// ccol_not_enough_memory, ccol_unexpected_failure or ccol_container_full.
ccol_retval clrucache_set_full(ccache cache, const cmap_pair *key_pair,
                               const cmap_pair *val_pair);

// Internal: Destroys *cp and clears it. The _scoped macros attach it with
// ccol__destructor; a CCACHE_NULL handle is a no-op, a stale one is fatal.
static inline __attribute__((always_inline)) void clrucache__destroy_cleanup(
    ccache *cp) {
  if (cp && *cp) {
    clrucache__destroy(*cp);
    *cp = CCACHE_NULL;
  }
}

// Destroys a cache and sets the handle to CCACHE_NULL, so the variable can't be
// used again by mistake. It waits for every call in flight on the handle,
// passes each remaining entry to the eviction callback and frees everything.
// CCACHE_NULL does nothing; a handle that was already destroyed, or a
// concurrent second destroy, is a fatal error. name must be a modifiable
// lvalue and is evaluated once.
#define clrucache_destroy(name) \
  ccol__clrucache_destroy_impl( \
      name, ccol__uniq(ccol__clrucache_destroy_slot, __COUNTER__))

// Internal: The body of clrucache_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define ccol__clrucache_destroy_impl(name, slot) \
  do {                                           \
    __typeof__(name) *slot = &(name);            \
    clrucache__destroy(*slot);                   \
    *slot = CCACHE_NULL;                         \
  } while (0)

// Destroys a cache and sets the handle to CCACHE_NULL, so the variable can't be
// used again by mistake. It waits for every call in flight on the handle,
// passes each remaining entry to the eviction callback and frees everything.
// CCACHE_NULL does nothing; a handle that was already destroyed, or a
// concurrent second destroy, is a fatal error. name must be a modifiable
// lvalue and is evaluated once.
#define ccache_destroy(name) clrucache_destroy(name)

// Declares a cache handle called name for key type KeyT and value type ValT,
// without creating a cache; the handle is left uninitialized. Create the cache
// with ccache_init() before any other ccache_* macro, and destroy it with
// ccache_destroy(). An array key or value type is refused at compile time;
// wrap it in a struct.
#define ccache_declare(name, KeyT, ValT)                                 \
  ccol__refuse_array_types("clru", KeyT, ValT);                          \
  __typeof__(KeyT) *name##__ccache_key_type_var __attribute__((unused)); \
  __typeof__(ValT) *name##__ccache_val_type_var __attribute__((unused)); \
  ccache name

// Declares a cache handle called name for key type KeyT and value type ValT,
// set to CCACHE_NULL, without creating a cache; create it with ccache_init().
// The cache is destroyed by itself when name goes out of scope, so no exit
// path can leak it; ccache_destroy() before that is allowed. An array key or
// value type is refused at compile time; wrap it in a struct.
#define ccache_declare_scoped(name, KeyT, ValT)                           \
  ccol__refuse_array_types("clru", KeyT, ValT);                           \
  __typeof__(KeyT) *name##__ccache_key_type_var __attribute__((unused)) = \
      NULL;                                                               \
  __typeof__(ValT) *name##__ccache_val_type_var __attribute__((unused)) = \
      NULL;                                                               \
  ccache name ccol__destructor(clrucache__destroy_cleanup) = CCACHE_NULL

// Creates the cache for a handle that ccache_declare() or
// ccache_declare_scoped() declared, with room for capacity entries (0 is
// refused) and the default allocator; the key and value types come from that
// declaration. getter fetches a missing key from a remote source, setter
// writes each set to it first, and evict_cb hears about every evicted entry;
// each may be NULL. Ends the program with a fatal error when the cache can't
// be created.
#define ccache_init(name, capacity, getter, setter, evict_cb)                  \
  do {                                                                         \
    char *ccache__err = NULL;                                                  \
    (name) = clrucache_create_full(                                            \
        (capacity),                                                            \
        ccol_determine_ccol_data_type(*name##__ccache_key_type_var),           \
        ccol_determine_ccol_data_type(*name##__ccache_val_type_var), (getter), \
        (setter), (evict_cb), NULL, &ccache__err);                             \
    if (!(name)) {                                                             \
      ccol__fatal_err("ccache_init('%s'): %s", #name,                          \
                      ccache__err ? ccache__err : "unknown error");            \
    }                                                                          \
  } while (0)

// Declares a cache handle called name for key type KeyT and value type ValT and
// creates a thread-safe least recently used cache with room for capacity
// entries (0 is refused) and the default allocator; the usual way to make one.
// getter fetches a missing key from a remote source, setter writes each set to
// it first, and evict_cb hears about every evicted entry; each may be NULL.
// Ends the program with a fatal error when the cache can't be created. Destroy
// it with ccache_destroy(). An array key or value type is refused at compile
// time.
//
//   ccache_construct(my_cache, int, double, 128, NULL, NULL, NULL);
//   ccache_set(my_cache, 42, 3.14);
//   double out;
//   ccache_get(my_cache, 42, &out);
//   ccache_destroy(my_cache);
#define ccache_construct(name, KeyT, ValT, capacity, getter, setter, evict_cb) \
  ccol__refuse_array_types("clru", KeyT, ValT);                                \
  __typeof__(KeyT) *name##__ccache_key_type_var __attribute__((unused)) =      \
      NULL;                                                                    \
  __typeof__(ValT) *name##__ccache_val_type_var __attribute__((unused)) =      \
      NULL;                                                                    \
  ccache name = CCACHE_NULL;                                                   \
  do {                                                                         \
    char *ccache__err = NULL;                                                  \
    (name) = clrucache_create_full(                                            \
        (capacity),                                                            \
        ccol_determine_ccol_data_type(*name##__ccache_key_type_var),           \
        ccol_determine_ccol_data_type(*name##__ccache_val_type_var), (getter), \
        (setter), (evict_cb), NULL, &ccache__err);                             \
    if (!(name)) {                                                             \
      ccol__fatal_err("ccache_construct('%s'): %s", #name,                     \
                      ccache__err ? ccache__err : "unknown error");            \
    }                                                                          \
  } while (0)

// Declares a cache handle called name for key type KeyT and value type ValT and
// creates a thread-safe least recently used cache with room for capacity
// entries (0 is refused) and the default allocator. The cache is destroyed by
// itself when name goes out of scope. getter fetches a missing key from a
// remote source, setter writes each set to it first, and evict_cb hears about
// every evicted entry; each may be NULL. Ends the program with a fatal error
// when the cache can't be created.
#define ccache_construct_scoped(name, KeyT, ValT, capacity, getter, setter,    \
                                evict_cb)                                      \
  ccol__refuse_array_types("clru", KeyT, ValT);                                \
  __typeof__(KeyT) *name##__ccache_key_type_var __attribute__((unused)) =      \
      NULL;                                                                    \
  __typeof__(ValT) *name##__ccache_val_type_var __attribute__((unused)) =      \
      NULL;                                                                    \
  ccache name ccol__destructor(clrucache__destroy_cleanup) = CCACHE_NULL;      \
  do {                                                                         \
    char *ccache__err = NULL;                                                  \
    (name) = clrucache_create_full(                                            \
        (capacity),                                                            \
        ccol_determine_ccol_data_type(*name##__ccache_key_type_var),           \
        ccol_determine_ccol_data_type(*name##__ccache_val_type_var), (getter), \
        (setter), (evict_cb), NULL, &ccache__err);                             \
    if (!(name)) {                                                             \
      ccol__fatal_err("ccache_construct_scoped('%s'): %s", #name,              \
                      ccache__err ? ccache__err : "unknown error");            \
    }                                                                          \
  } while (0)

// Makes the typed ccache_* macros usable on a cache handle that came from
// another scope, such as a function parameter or a struct field. It declares
// only the key and value type information those macros read; it creates and
// destroys nothing. KeyT and ValT must be the types the cache was created
// with. An array type is refused at compile time.
//
//   void use_cache(ccache c) {
//     ccache_redeclare(c, int, double);
//     double out;
//     ccache_get(c, 42, &out);
//   }
#define ccache_redeclare(name, KeyT, ValT)                                \
  ccol__refuse_array_types("clru", KeyT, ValT);                           \
  __typeof__(KeyT) *name##__ccache_key_type_var __attribute__((unused)) = \
      NULL;                                                               \
  __typeof__(ValT) *name##__ccache_val_type_var __attribute__((unused)) = NULL

// Looks key up in the typed cache name and stores the value in *val_ptr; a hit
// makes the key the most recently used, and a miss asks the remote getter, if
// there is one. Evaluates to ccol_success, ccol_key_not_found (missing and no
// getter, or the getter failed), ccol_invalid_args (CCACHE_NULL, a destroyed
// handle or a NULL char * key), ccol_not_enough_memory, or
// ccol_unexpected_failure (the getter gave a value of the wrong size). key is
// converted to the declared KeyT as by assignment (a char * key type keeps the
// caller's pointer type). A value of another type is converted into *val_ptr
// as by assignment and no heap memory is left; for a char * value type you get
// a new heap string on each call and free it with the cache's allocator
// (free() for the default). On failure *val_ptr is left untouched. A cache
// from another scope needs ccache_redeclare() first.
#define ccache_get(name, key, val_ptr)                                  \
  ccol__ccache_get_impl(name, (key), (val_ptr),                         \
                        ccol__uniq(ccol__ccache_get_k, __COUNTER__),    \
                        ccol__uniq(ccol__ccache_get_kp, __COUNTER__),   \
                        ccol__uniq(ccol__ccache_get_r, __COUNTER__),    \
                        ccol__uniq(ccol__ccache_get_vtmp, __COUNTER__), \
                        ccol__uniq(ccol__ccache_get_vout, __COUNTER__))

// The value type of cache name without top-level qualifiers, so ccache_get()
// can write a temporary of it when the value type is const.
#define ccache__unqual_val_type(name) \
  ccol__unqual_type(*(name##__ccache_val_type_var))

// Internal: The body of ccache_get(), with temporaries named by ccol__uniq() so
// it nests inside any public macro. The key copy has the declared key type and
// its padding cleared, since the segment choice and the map read every byte.
// Both arms are compiled for every ValT, so the shared ValT temporary sizes
// each copy by the declared type; without it the dead char * arm would be a
// _FORTIFY_SOURCE error for a narrow ValT.
#define ccol__ccache_get_impl(name, key, val_ptr, ccache__k, ccache__kp,       \
                              ccache__r, ccache__vtmp, ccache__vout)           \
  ({                                                                           \
    ccol__declared_or_own_type(name##__ccache_key_type_var, (key)) ccache__k = \
        (key);                                                                 \
    ccol__clear_padding(&ccache__k);                                           \
    cmap_pair ccache__kp = {};                                                 \
    ccol__populate_cmap_pair(&ccache__kp, ccache__k);                          \
    ccol_retval ccache__r;                                                     \
    ccache__unqual_val_type(name) ccache__vtmp;                                \
    if (ccol__is_char_ptr(*(name##__ccache_val_type_var))) {                   \
      cmap_pair ccache__vout = {};                                             \
      ccache__r = clrucache_get_full((name), &ccache__kp, &ccache__vout);      \
      if (ccache__r == ccol_success && ccache__vout.ptr) {                     \
        memcpy(&ccache__vtmp, &ccache__vout.ptr, sizeof(ccache__vtmp));        \
        *(val_ptr) = ccache__vtmp;                                             \
      }                                                                        \
    } else {                                                                   \
      ccache__r = clrucache__get_into((name), &ccache__kp, &ccache__vtmp,      \
                                      sizeof(ccache__vtmp));                   \
      if (ccache__r == ccol_success) *(val_ptr) = ccache__vtmp;                \
    }                                                                          \
    ccache__r;                                                                 \
  })

// Stores a copy of val under key in the typed cache name and makes the key the
// most recently used; a full cache evicts its least recently used entry. When
// the cache has a remote setter it runs first, and a failed setter gives
// ccol_unexpected_failure and changes nothing. Evaluates to ccol_success,
// ccol_unexpected_failure, ccol_invalid_args (CCACHE_NULL, a destroyed handle,
// or a NULL char * key or value), ccol_not_enough_memory or
// ccol_container_full. key and val are converted to the declared KeyT and
// ValT as by assignment (a char * type keeps the caller's pointer type). name
// must be a plain variable with its companion variables; a cache from another
// scope needs ccache_redeclare().
#define ccache_set(name, key, val)                                    \
  ccol__ccache_set_impl(name, (key), (val),                           \
                        ccol__uniq(ccol__ccache_set_k, __COUNTER__),  \
                        ccol__uniq(ccol__ccache_set_v, __COUNTER__),  \
                        ccol__uniq(ccol__ccache_set_kp, __COUNTER__), \
                        ccol__uniq(ccol__ccache_set_vp, __COUNTER__))

// Internal: The body of ccache_set(), with ccol__uniq() temporaries; the key
// and value copies follow the rules of ccache_get().
#define ccol__ccache_set_impl(name, key, val, ccache__k, ccache__v,            \
                              ccache__kp, ccache__vp)                          \
  ({                                                                           \
    ccol__declared_or_own_type(name##__ccache_key_type_var, (key)) ccache__k = \
        (key);                                                                 \
    ccol__clear_padding(&ccache__k);                                           \
    ccol__declared_or_own_type(name##__ccache_val_type_var, (val)) ccache__v = \
        (val);                                                                 \
    cmap_pair ccache__kp = {};                                                 \
    cmap_pair ccache__vp = {};                                                 \
    ccol__populate_cmap_pair(&ccache__kp, ccache__k);                          \
    ccol__populate_cmap_pair(&ccache__vp, ccache__v);                          \
    clrucache_set_full((name), &ccache__kp, &ccache__vp);                      \
  })

// Returns the number of live entries in the cache, for monitoring. Each
// segment is counted in turn, so under concurrent use the total is a snapshot.
// Returns 0 for CCACHE_NULL or a destroyed handle.
size_t clrucache_size(ccache cache);

// Returns the capacity the cache was created with, or 0 for CCACHE_NULL or a
// destroyed handle.
size_t clrucache_capacity(ccache cache);

// Evaluates to the number of live entries in the cache, as a size_t, for
// monitoring; under concurrent use the total is a snapshot. Gives 0 for
// CCACHE_NULL or a destroyed handle.
#define ccache_size(name) clrucache_size((name))

// Evaluates to the capacity the cache was created with, as a size_t, or 0 for
// CCACHE_NULL or a destroyed handle.
#define ccache_capacity(name) clrucache_capacity((name))

#pragma GCC visibility pop
