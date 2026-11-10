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

// The remote setter type: writes key and val to the remote source and returns
// true on success. No cache lock is held during the call; the rules of the
// getter about the same key and the destroy apply.
typedef bool (*ccache_remote_setter_fn)(const cmap_pair *key,
                                        const cmap_pair *val);

// The eviction callback type: gets the key and value of an entry evicted to
// make room. It runs with the owning segment's lock held, so it must not call
// back into the cache, and evictions in two segments can run it at the same
// time.
typedef void (*ccache_eviction_fn)(const cmap_pair *key, const cmap_pair *val);

// The opaque cache: a thread-safe least recently used cache with an optional
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
// included: zero such a key with memset() before using the raw functions.
// ccache_get() and ccache_set() clear the padding of their own copy with GCC 11
// or later; with other compilers use the raw functions for padded keys.
typedef struct clrucache clrucache;

// The handle is an opaque value (slot index plus generation), never a pointer.
// Compare it with CCACHE_NULL, which is 0, so `if (!cache)` works. Every use
// goes through the library's slot table, so a freed or reused slot is always
// detected.
typedef uint64_t ccache;

// "No cache": the failure value of a create and what a destroy leaves in the
// handle, so a handle can be tested for validity.
#define CCACHE_NULL ((ccache)0)

// Creates a cache of capacity live entries (0 is refused). It's the one
// constructor behind ccache_init and the construct macros. key_type and
// val_type come from ccol_determine_ccol_data_type. getter, setter and
// eviction_cb may be NULL; a setter runs before each update of the cache.
// mprocs is your allocator or NULL; the cache keeps a copy of the struct, but
// its functions must stay callable until the destroy. Gives the handle, or
// CCACHE_NULL with *err set (also for a create after the library's destructor
// released the handle table).
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

// Internal: destroys a cache. It's the body of ccache_destroy(); use the macro,
// which also clears the handle. CCACHE_NULL is a no-op; any other handle that
// isn't live (already destroyed, being destroyed, garbage) is a fatal error.
// It waits for every call that resolved the handle, evicts the remaining
// entries through the eviction callback and frees everything.
void clrucache__destroy(ccache cache);

// Internal: copies the value of key_pair into buf without a heap allocation.
// ccache_get() uses it for every value type other than char *, so a get
// allocates nothing. buf_size must equal the stored size (ccache_get passes
// sizeof(ValT)); any other size gives ccol_unexpected_failure and leaves buf
// untouched.
ccol_retval clrucache__get_into(ccache cache, const cmap_pair *key_pair,
                                void *buf, size_t buf_size);

// Looks key_pair up and gives a heap copy of its value in *val_out, which the
// caller frees with the cache's allocator. It's the raw get, for callers that
// don't use the typed macros. On a miss the remote getter runs once and every
// other getter of that key waits for its result; a getter also waits while a
// set of the key runs. A fixed-width key must have the width of its type, or
// the call gives ccol_invalid_args without reading it. Gives ccol_success,
// ccol_key_not_found, ccol_invalid_args, ccol_not_enough_memory,
// ccol_unexpected_failure (a joined fetch stored a value of the wrong size) or
// ccol_container_full (the internal records are full).
ccol_retval clrucache_get_full(ccache cache, const cmap_pair *key_pair,
                               cmap_pair *val_out);

// Stores val_pair under key_pair, after the remote setter (if any) succeeded;
// it's the raw set. The key becomes its segment's newest entry. A failed setter
// gives ccol_unexpected_failure and changes nothing in the cache. Every
// allocation happens before the setter runs. Setters of one key are
// serialized, and a getter of the key waits for the set. Gives ccol_success,
// ccol_invalid_args, ccol_not_enough_memory, ccol_unexpected_failure or
// ccol_container_full.
ccol_retval clrucache_set_full(ccache cache, const cmap_pair *key_pair,
                               const cmap_pair *val_pair);

// Internal: destroys *cp and clears it. The _scoped macros attach it with
// ccol__destructor; a CCACHE_NULL handle is a no-op, a stale one is fatal.
static inline __attribute__((always_inline)) void clrucache__destroy_cleanup(
    ccache *cp) {
  if (cp && *cp) {
    clrucache__destroy(*cp);
    *cp = CCACHE_NULL;
  }
}

// Destroys a cache and sets the handle to CCACHE_NULL, so the variable can't be
// used again by mistake. It waits for every call in flight on the handle; a
// concurrent second destroy is a fatal error. name must be a modifiable lvalue
// and is evaluated once.
#define clrucache_destroy(name) \
  ccol__clrucache_destroy_impl( \
      name, ccol__uniq(ccol__clrucache_destroy_slot, __COUNTER__))

// Internal: the body of clrucache_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define ccol__clrucache_destroy_impl(name, slot) \
  do {                                           \
    __typeof__(name) *slot = &(name);            \
    clrucache__destroy(*slot);                   \
    *slot = CCACHE_NULL;                         \
  } while (0)

// The typed layer's name for clrucache_destroy(), so typed code reads the
// same throughout.
#define ccache_destroy(name) clrucache_destroy(name)

// Declares a cache variable called name together with the companion variables
// that record KeyT and ValT for the typed macros; initialize it with
// ccache_init(). Array types are refused, because the macros copy through a
// temporary of the declared type.
#define ccache_declare(name, KeyT, ValT)                                 \
  ccol__refuse_array_types("clru", KeyT, ValT);                          \
  __typeof__(KeyT) *name##__ccache_key_type_var __attribute__((unused)); \
  __typeof__(ValT) *name##__ccache_val_type_var __attribute__((unused)); \
  ccache name

// ccache_declare() for a cache that is destroyed when it goes out of scope, so
// no exit path can leak it.
#define ccache_declare_scoped(name, KeyT, ValT)                           \
  ccol__refuse_array_types("clru", KeyT, ValT);                           \
  __typeof__(KeyT) *name##__ccache_key_type_var __attribute__((unused)) = \
      NULL;                                                               \
  __typeof__(ValT) *name##__ccache_val_type_var __attribute__((unused)) = \
      NULL;                                                               \
  ccache name ccol__destructor(clrucache__destroy_cleanup) = CCACHE_NULL

// Creates the cache of a variable declared with ccache_declare(), taking the
// key and value types from its companion variables; getter, setter and
// evict_cb may be NULL. A failed create calls ccol__fatal_err.
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

// Declares and creates a typed cache in one step, the usual way to make one; a
// failed create calls ccol__fatal_err.
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

// ccache_construct() for a cache that is destroyed at the end of its scope.
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

// Declares the companion type variables again for a cache handle that came
// from another scope (a function parameter, for example), so the typed macros
// work on it:
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

// Looks key up in the typed cache name and stores the value in *val_ptr; it
// evaluates to the ccol_retval of the get. key is converted to the declared
// KeyT as by assignment (a char * key type keeps the caller's pointer type). A
// value of another type is converted into *val_ptr as by assignment and no heap
// memory is left; for a char * value type the caller gets a heap string to free
// with the cache's allocator. A NULL char * key gives ccol_invalid_args. A
// cache from another scope needs ccache_redeclare() first.
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

// Internal: the body of ccache_get(), with temporaries named by ccol__uniq() so
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

// Stores val under key in the typed cache name, after the remote setter, and
// evaluates to the ccol_retval of the set. key and val are converted to the
// declared KeyT and ValT as by assignment (a char * type keeps the caller's
// pointer type). name must be a plain variable with its companion variables. A
// NULL char * key or value gives ccol_invalid_args.
#define ccache_set(name, key, val)                                    \
  ccol__ccache_set_impl(name, (key), (val),                           \
                        ccol__uniq(ccol__ccache_set_k, __COUNTER__),  \
                        ccol__uniq(ccol__ccache_set_v, __COUNTER__),  \
                        ccol__uniq(ccol__ccache_set_kp, __COUNTER__), \
                        ccol__uniq(ccol__ccache_set_vp, __COUNTER__))

// Internal: the body of ccache_set(), with ccol__uniq() temporaries; the key
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

// Gives the number of live entries right now, for monitoring.
size_t clrucache_size(ccache cache);

// Gives the capacity the cache was created with.
size_t clrucache_capacity(ccache cache);

// The typed layer's name for clrucache_size().
#define ccache_size(name) clrucache_size((name))

// The typed layer's name for clrucache_capacity().
#define ccache_capacity(name) clrucache_capacity((name))

#pragma GCC visibility pop
