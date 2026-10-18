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
#include <stdbool.h> /* bool, true and false are keywords in C23 */
#endif
#include <string.h>

#include "common.h"

/* Everything this header declares from here to the end belongs to the public
 * Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of it. The library is built with -fvisibility=hidden,
 * so a function or an object outside these blocks stays internal: it isn't
 * in the dynamic symbol table, an application linking against the library
 * can't interpose it, and a symbol of the same name in that application
 * can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file clrucache.h
 * @brief Thread-safe generic Least Recently Used (LRU) cache, with an
 *        optional remote source.
 *
 * A hash map gives the cache O(1) lookup and a doubly-linked list gives it
 * O(1) eviction. All operations are fully thread-safe.
 *
 * A cache that is large enough is split into several segments, each with its
 * own lock. Every key belongs to exactly one segment, picked by a hash of
 * the key, so threads working on keys in different segments don't wait for
 * each other while operations on the same key are serialised, and the
 * guarantees below rest on that. The split starts at 128 entries, the first
 * capacity that gives two segments 64 entries each. Below 128 entries a
 * cache is a single segment and keeps one exact, global least-recently-used
 * order. From 128 entries on, the capacity is divided across the segments
 * and each segment evicts its own least recently used entry. The doc comment
 * of clrucache_create_full() has the details, and clrucache_create_full(3)
 * the measured effect on how full a cache gets.
 *
 * Key concurrency guarantees:
 * - Many getters for the same uncached key coalesce: only one remote fetch
 *   runs, and all the other getters block and get the same result once that
 *   fetch completes.
 * - A getter blocks while a setter for the same key is in progress, so a
 *   getter always reads a consistent value. If the set fails and the key is
 *   then absent from the cache, the getter goes on exactly like a get that
 *   started after the failed set: it runs the remote getter, or joins a
 *   fetch another getter started, or reports ccol_key_not_found when the
 *   cache has no remote getter.
 * - Many setters for the same key are serialized.
 *
 * Set semantics: when a remote setter was given, it is always called before
 * the cache is updated. If the remote call fails the cache isn't updated and
 * clrucache_set_full() returns ccol_unexpected_failure. A failed set does
 * nothing at all to its key: the key keeps its old value and its exact place
 * in the eviction order, so it is neither spared nor moved forward to be the
 * next entry to evict. Every step of a set that can fail for lack of memory,
 * including the copy of the new value, runs before the remote setter, so a
 * ccol_not_enough_memory or ccol_container_full result means the remote
 * setter was never called. Once the remote setter has succeeded, updating
 * the cache can't fail, so the cache never keeps serving a value older than
 * the one the remote store holds.
 *
 * A set takes effect when its remote setter returns. While that call runs,
 * the key keeps its old value, its place in the eviction order and its place
 * in clrucache_size(). An insert of another key that needs room meanwhile
 * evicts the least recently used key, which may be the key being set, and
 * the eviction callback then gets the old value. If the set then fails the
 * key stays evicted; if it succeeds the key comes back with the new value as
 * the most recently used entry, which may evict some other key. The result
 * is always that of the insert followed by the set, and a segment never
 * holds more entries than its capacity.
 *
 * Limitations and caller responsibilities:
 * - The eviction callback runs while the owning segment's lock is held, so
 *   it MUST NOT call back into the cache; that's a deadlock. The callback is
 *   NOT serialised against itself either: two segments can evict at the
 *   same time on two threads, so a callback with state of its own has to
 *   guard that state.
 * - A key of a type the cache doesn't know, such as a struct, is the same
 *   key as another one only when all their bytes are equal. The choice of
 *   segment and the internal map both read every byte of the key, padding
 *   bytes of a struct included, and the cache takes no custom hash or key
 *   equality function, so every byte of a padded key has to be determined.
 *   Through the raw functions, such as clrucache_set_full(), the cache reads
 *   exactly the object key_pair->ptr points at, so zero the whole key object
 *   with memset() before setting its members. Through clru_get() and
 *   clru_set() the cache sees the macro's own copy of the key, and C leaves
 *   the padding of a struct copy unspecified. With GCC 11 or later the
 *   macros zero that padding; with any other compiler, use the raw functions
 *   for a padded key.
 */

/* ========================================================================== */
/*                         CALLBACK TYPES                                     */
/* ========================================================================== */

/**
 * @brief Remote getter callback
 *
 * Called when the requested key isn't in the cache and a remote source
 * exists. On success the callback must allocate the value data on the heap,
 * assign that pointer to val->ptr and set val->size to the byte length. The
 * cache takes ownership of val->ptr and frees it with its own allocator,
 * which is the custom ccol_memmgmt_procs_t given to clrucache_create_full,
 * or malloc()/free() if none was given. The callback's allocation MUST use
 * that same allocator: its malloc or calloc when one is configured, plain
 * malloc() otherwise. If it doesn't, the cache's later free of val->ptr
 * corrupts the heap. On failure the callback must leave val->ptr as NULL and
 * return false.
 *
 * No lock of the cache is held during the call, so the callback can use the
 * cache for any OTHER key. It must not call back into the same cache for the
 * key it is serving: a get or a set of that key waits for the operation that
 * runs this callback, which never finishes, and the thread deadlocks against
 * itself. It must not destroy the cache either, because the destroy waits
 * for the call that runs this callback.
 *
 * @param key  Key pair (ptr + size)
 * @param val  Output pair to fill on success (ptr + size)
 * @return true on success, false on failure
 */
typedef bool (*clru_remote_getter_t)(const cmap_pair *key, cmap_pair *val);

/**
 * @brief Remote setter callback
 *
 * Tries to write a key-value pair to the remote source.
 *
 * No lock of the cache is held during the call, so the callback can use the
 * cache for any OTHER key. It must not call back into the same cache for the
 * key it is serving: a get or a set of that key waits for the operation that
 * runs this callback, which never finishes, and the thread deadlocks against
 * itself. It must not destroy the cache either, because the destroy waits
 * for the call that runs this callback.
 *
 * @param key  Key pair (ptr + size)
 * @param val  Value pair (ptr + size)
 * @return true on success, false on failure
 */
typedef bool (*clru_remote_setter_t)(const cmap_pair *key,
                                     const cmap_pair *val);

/**
 * @brief Eviction callback
 *
 * Called when an entry is evicted to make room for a new one. The call is
 * synchronous and the owning segment's lock is held during it, so the
 * callback must not call back into the cache.
 *
 * Only that one segment's lock is held, so two evictions in different
 * segments can run this callback at the same time on two threads. Whatever
 * state the callback keeps of its own needs its own protection.
 *
 * @param key  The key pair of the evicted entry (ptr + size)
 * @param val  The value pair of the evicted entry (ptr + size)
 */
typedef void (*clru_eviction_cb_t)(const cmap_pair *key, const cmap_pair *val);

/* ========================================================================== */
/*                         OPAQUE TYPE                                        */
/* ========================================================================== */

/** @brief Opaque LRU cache structure */
typedef struct clrucache clrucache;

/**
 * @brief Opaque LRU cache handle.
 *
 * clru_cache is an opaque VALUE handle, not a pointer: it packs a slot index
 * and a generation together. Never cast it to void* or a void* to it, never
 * compare it with a pointer cast, and never treat it as an address. Compare
 * it directly against CLRU_CACHE_INVALID. CLRU_CACHE_INVALID is 0, so it
 * also works in a truthiness check and `if (!cache)` is a valid test for an
 * invalid handle. Inside the library every use of a clru_cache goes through
 * a slot table that the library owns, and the handle is resolved there
 * before the cache object is touched. A handle whose slot has been freed is
 * always detected, and so is a handle whose slot has been reused for a
 * different, later cache, so the library never dereferences freed memory or
 * the memory of the wrong object. See the doc comment of
 * __clrucache_destroy for what happens when a stale handle reaches that
 * function.
 */
typedef uint64_t clru_cache;

/** @brief Sentinel value for "no cache", the clru_cache equivalent of
 *         NULL. */
#define CLRU_CACHE_INVALID ((clru_cache)0)

/* ========================================================================== */
/*                         CREATION / DESTRUCTION                             */
/* ========================================================================== */

/**
 * @brief Create an LRU cache
 *
 * The cache holds up to capacity live entries. Below a capacity of 128 it is
 * a single segment, and an insert into a full cache first evicts the least
 * recently used entry of the whole cache. From 128 entries on, the cache is
 * split into segments, each with its own lock, and every key belongs to
 * exactly one segment, picked by a hash of the key. The capacity is divided
 * across the segments, and each segment evicts its own least recently used
 * entry once its own share is full, so the entry that makes room is the
 * oldest inside one segment and not the oldest of the whole cache. Since
 * some segments run fuller than others, eviction starts a little before the
 * cache holds capacity entries, and a full cache holds a few percent less
 * than capacity. clrucache_capacity() reports capacity itself.
 *
 * @param capacity     The total number of live entries the cache holds;
 *                     from 128 on, each segment evicts when its own share is
 *                     full (see above). A capacity of 0 is refused: the
 *                     call fails and the string in *err contains
 *                     "capacity must be > 0".
 * @param key_type ccol_data_type of keys (from ccol_determine_ccol_data_type)
 * @param val_type ccol_data_type of values (from ccol_determine_ccol_data_type)
 * @param getter       Remote getter (can be NULL)
 * @param setter       Remote setter (can be NULL). When it isn't NULL it is
 *                     called synchronously before each update of the cache.
 * @param eviction_cb  Called on eviction (can be NULL)
 * @param mprocs       Custom allocator, or NULL for malloc/free. The cache
 *                     keeps a copy of the struct, so the caller may release
 *                     or reuse the struct once this call returns. The
 *                     functions it names must stay callable until the cache
 *                     is destroyed.
 * @param err          Optional. Set to an error string on failure
 * @return The new cache handle, or CLRU_CACHE_INVALID on failure. A create
 *         from a destructor that runs after the library's own destructor
 *         fails too, because by then the library has released its table of
 *         cache handles at the end of the process.
 */
clru_cache clrucache_create_full(size_t capacity, ccol_data_type key_type,
                                 ccol_data_type val_type,
                                 clru_remote_getter_t getter,
                                 clru_remote_setter_t setter,
                                 clru_eviction_cb_t eviction_cb,
                                 ccol_memmgmt_procs_t *mprocs, char **err);

/**
 * @brief Internal destroy. Use the clru_destroy() macro instead.
 *
 * cache must be a live handle, meaning one that clrucache_create_full,
 * clru_init or clru_construct returned and that hasn't been destroyed yet. A
 * stale handle is a fatal error; a handle is stale when an earlier, complete
 * call to this same function destroyed it, or when another thread is
 * destroying it at this same moment. A forged value or garbage is a fatal
 * error too. In all these cases the function calls _ccol_fatal_err(), which
 * calls abort() and raises SIGABRT, instead of risking a use-after-free or a
 * double-free. That covers both a plain sequential second destroy and a
 * concurrent destroy that overlaps in time. CLRU_CACHE_INVALID (0) is the
 * one exception and stays a silent no-op, matching the "destroy sets the
 * handle to NULL" idiom of clru_destroy.
 *
 * On a live handle it blocks until every in-flight resolved use of the
 * handle has stopped, then evicts all the remaining entries, calling the
 * eviction callback for each one, and then frees all the memory.
 */
void __clrucache_destroy(clru_cache cache);

/**
 * @brief Copy a value directly into a buffer that the caller gives. This is
 *        internal. Use the clru_get() macro instead.
 *
 * Unlike clrucache_get_full(), this doesn't allocate heap memory: it copies
 * the stored value into buf while holding the owning segment's lock, and
 * then returns.
 *
 * buf_size must be exactly the size of the stored value. clru_get() obeys
 * that because it passes sizeof(ValT) and not sizeof(*val_ptr): it reads
 * into a temporary of type ValT and then converts and assigns that
 * temporary into *val_ptr. The type of *val_ptr can legitimately differ from
 * ValT (an int-valued cache with a double* out-parameter, for example), and
 * then the two sizes differ too.
 *
 * A stored value of any size other than that of ValT, larger or smaller,
 * gets ccol_unexpected_failure, and buf is left completely untouched; a
 * partial value is never copied into it. That only happens for a value that
 * came into the cache from somewhere else, never from a getter or setter
 * that always produces values of sizeof(ValT) bytes for this key.
 */
ccol_retval_t __clrucache_get_into(clru_cache cache, const cmap_pair *key_pair,
                                   void *buf, size_t buf_size);

/* ========================================================================== */
/*                         OPERATIONS                                         */
/* ========================================================================== */

/**
 * @brief Get a value by key
 *
 * On success it allocates a heap copy of the stored value, with the cache's
 * custom allocator if one was given at construction and with malloc()
 * otherwise. The copy goes to val_out->ptr and val_out->size gets the stored
 * byte size. The caller must free val_out->ptr with the matching allocator:
 * the custom free function when a custom allocator was configured, free()
 * otherwise.
 *
 * On a cache miss the remote getter, if one was given, is called once. The
 * other getters for the same key block until that call completes, and the
 * fetched value is stored in the cache and handed to all the waiters.
 *
 * It blocks while a setter for this key is in progress, until the cache is
 * consistent again. When that set fails and leaves the key absent, this call
 * goes on as a cache miss, so the remote getter runs for it.
 *
 * @param cache     Cache handle
 * @param key_pair  The key to look up. For a fixed-width key type its size
 *                  must equal the width of that type; otherwise the call
 *                  reports ccol_invalid_args without reading the key
 * @param val_out   On success val_out->ptr is a heap copy of the value,
 *                  which the caller must free with the cache's allocator,
 *                  and val_out->size is the stored value's size
 * @return ccol_success, ccol_key_not_found, ccol_invalid_args,
 *         ccol_not_enough_memory, ccol_unexpected_failure or
 *         ccol_container_full. ccol_unexpected_failure comes back when the
 *         call joins another thread's cache-miss fetch and that fetch stores
 *         a value of the wrong size, one that doesn't agree with what the
 *         caller on that other thread expects. ccol_container_full comes back
 *         on a cache miss when the cache's internal records reach their own
 *         maximum element count
 */
ccol_retval_t clrucache_get_full(clru_cache cache, const cmap_pair *key_pair,
                                 cmap_pair *val_out);

/**
 * @brief Set a value by key
 *
 * The remote setter, if one was given at construction, is called first. On
 * success the cache is updated and the key becomes its segment's most
 * recently used entry. On failure the function returns
 * ccol_unexpected_failure and nothing changes in the cache for that key:
 * both its stored value and its exact position in the eviction order
 * survive the call. So a set that stores nothing never costs another key
 * its place as the next entry to evict.
 *
 * Every allocation of the call, the copy of the new value included, happens
 * before the remote setter runs. A ccol_not_enough_memory or
 * ccol_container_full result therefore means the remote setter was never
 * called and nothing changed in the cache for the key, and a remote setter
 * that succeeds is always followed by a cache update that can't fail.
 *
 * The set takes effect when the remote setter returns. Until then the key is
 * an ordinary entry of its segment and a concurrent insert that needs room
 * can evict it. A set that then succeeds inserts the key again with the new
 * value, and a set that fails leaves it evicted. See the file-level
 * documentation of this header.
 *
 * Many setters for the same key are serialized. A getter for the same key
 * blocks until the set completes, since the cache is consistent then.
 *
 * @param cache    Cache handle
 * @param key_pair The key to set
 * @param val_pair The value to pair with the key
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory,
 *         ccol_unexpected_failure or ccol_container_full.
 *         ccol_unexpected_failure comes back when the synchronous remote
 *         call fails, and ccol_container_full when it sets a brand new key
 *         and the cache's internal bookkeeping reaches its own maximum
 *         element count
 */
ccol_retval_t clrucache_set_full(clru_cache cache, const cmap_pair *key_pair,
                                 const cmap_pair *val_pair);

/* ========================================================================== */
/*                         DESTROY MACRO                                      */
/* ========================================================================== */

/**
 * @brief Cleanup function for Resource Acquisition Is Initialization (RAII).
 *        Use it with _ccol_destructor.
 *
 * Calling it on a *cp that is already CLRU_CACHE_INVALID is safe and does
 * nothing. Calling it on a stale handle that isn't CLRU_CACHE_INVALID (one
 * that some other path has already destroyed) is fatal misuse, as
 * __clrucache_destroy documents too.
 */
static inline __attribute__((always_inline)) void ___clrucache_destroy(
    clru_cache *cp) {
  if (cp && *cp) {
    __clrucache_destroy(*cp);
    *cp = CLRU_CACHE_INVALID;
  }
}

/**
 * @brief Destroy a cache and set the handle to CLRU_CACHE_INVALID
 *
 * Blocks until every in-flight resolved use of the handle has stopped. Don't
 * call it at the same time as another call on the same handle; the doc
 * comment of __clrucache_destroy describes what happens then, and it's a
 * fatal error, not a silent race.
 *
 * @note name is evaluated exactly once. It must be a modifiable lvalue, such
 * as a variable or an element of an array.
 */
#define clru_destroy(name)      \
  _ccol_clru_destroy_impl(name, \
                          _ccol_uniq(__ccol_clru_destroy_slot, __COUNTER__))

/* Internal: the body of clru_destroy. slot is a name from _ccol_uniq(), so
 * the macro can nest inside the argument of another destroy macro and still
 * be -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_clru_destroy_impl(name, slot) \
  do {                                      \
    __typeof__(name) *slot = &(name);       \
    __clrucache_destroy(*slot);             \
    *slot = CLRU_CACHE_INVALID;             \
  } while (0)

/* ========================================================================== */
/*                    TYPE-TRACKING DECLARE / INIT / CONSTRUCT                */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cache variable with type tracking
 *
 * Call clru_init() on the variable before using it.
 */
#define clru_declare(name, KeyT, ValT)                                    \
  _ccol_refuse_array_types("clru", KeyT, ValT);                           \
  __typeof__(KeyT) *name##__clru_key_type_var                             \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  __typeof__(ValT) *name##__clru_val_type_var                             \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  clru_cache name              /* deliberately not initialized to NULL */

/**
 * @brief Declare a cache variable that the compiler cleans up at scope exit
 */
#define clru_declare_scoped(name, KeyT, ValT)                                 \
  _ccol_refuse_array_types("clru", KeyT, ValT);                               \
  __typeof__(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  __typeof__(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL; \
  clru_cache name _ccol_destructor(___clrucache_destroy) = CLRU_CACHE_INVALID

/**
 * @brief Initialize a previously declared cache variable
 *
 * @param name       Cache variable, declared first with clru_declare
 * @param capacity   The total number of live entries; see
 *                   clrucache_create_full()
 * @param getter     Remote getter (can be NULL)
 * @param setter     Remote setter (can be NULL)
 * @param evict_cb   Eviction callback (can be NULL)
 */
#define clru_init(name, capacity, getter, setter, evict_cb)                    \
  do {                                                                         \
    char *_clru_err = NULL;                                                    \
    (name) = clrucache_create_full(                                            \
        (capacity), ccol_determine_ccol_data_type(*name##__clru_key_type_var), \
        ccol_determine_ccol_data_type(*name##__clru_val_type_var), (getter),   \
        (setter), (evict_cb), NULL, &_clru_err);                               \
    if (!(name)) {                                                             \
      _ccol_fatal_err("clru_init('%s'): %s", #name,                            \
                      _clru_err ? _clru_err : "unknown error");                \
    }                                                                          \
  } while (0)

/**
 * @brief Declare and initialize in one step, the most common use.
 *
 * Example:
 * @code
 * clru_construct(my_cache, int, double, 128, NULL, NULL, NULL);
 * int k = 42; double v = 3.14;
 * clru_set(my_cache, k, v);
 * double out;
 * clru_get(my_cache, k, &out);
 * clru_destroy(my_cache);
 * @endcode
 */
#define clru_construct(name, KeyT, ValT, capacity, getter, setter, evict_cb)   \
  _ccol_refuse_array_types("clru", KeyT, ValT);                                \
  __typeof__(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL;  \
  __typeof__(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL;  \
  clru_cache name = CLRU_CACHE_INVALID;                                        \
  do {                                                                         \
    char *_clru_err = NULL;                                                    \
    (name) = clrucache_create_full(                                            \
        (capacity), ccol_determine_ccol_data_type(*name##__clru_key_type_var), \
        ccol_determine_ccol_data_type(*name##__clru_val_type_var), (getter),   \
        (setter), (evict_cb), NULL, &_clru_err);                               \
    if (!(name)) {                                                             \
      _ccol_fatal_err("clru_construct('%s'): %s", #name,                       \
                      _clru_err ? _clru_err : "unknown error");                \
    }                                                                          \
  } while (0)

/**
 * @brief Declare, initialize, and destroy automatically at scope exit
 */
#define clru_construct_scoped(name, KeyT, ValT, capacity, getter, setter,      \
                              evict_cb)                                        \
  _ccol_refuse_array_types("clru", KeyT, ValT);                                \
  __typeof__(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL;  \
  __typeof__(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL;  \
  clru_cache name _ccol_destructor(___clrucache_destroy) = CLRU_CACHE_INVALID; \
  do {                                                                         \
    char *_clru_err = NULL;                                                    \
    (name) = clrucache_create_full(                                            \
        (capacity), ccol_determine_ccol_data_type(*name##__clru_key_type_var), \
        ccol_determine_ccol_data_type(*name##__clru_val_type_var), (getter),   \
        (setter), (evict_cb), NULL, &_clru_err);                               \
    if (!(name)) {                                                             \
      _ccol_fatal_err("clru_construct_scoped('%s'): %s", #name,                \
                      _clru_err ? _clru_err : "unknown error");                \
    }                                                                          \
  } while (0)

/**
 * @brief Restore the type-tracking variables for a cache passed across
 *        scopes
 *
 * Example:
 * @code
 * void use_cache(clru_cache c) {
 *   clru_redeclare(c, int, double);
 *   double out;
 *   clru_get(c, 42, &out);
 * }
 * @endcode
 */
#define clru_redeclare(name, KeyT, ValT)                                      \
  _ccol_refuse_array_types("clru", KeyT, ValT);                               \
  __typeof__(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  __typeof__(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL

/* ========================================================================== */
/*                    TYPE-INFERRED GET / SET MACROS */
/* ========================================================================== */

/**
 * @brief Get a value by key. This is a type-inferred convenience wrapper.
 *
 * For a val type other than char*, the stored value is converted to the
 * type of *val_ptr the way a plain C assignment converts it. A cache that
 * stores int values and reads into a double* out-parameter, for example,
 * converts the int to a double instead of copying the int's raw bytes. The
 * internal heap allocation is then freed. A type of *val_ptr that the
 * compiler can't convert implicitly from the cache's declared ValT is a
 * compile error right here, never a value that gets silently
 * reinterpreted.
 *
 * For a char* val type, ownership of the heap string goes to
 * *(char **)val_ptr, and the caller must free that string, with the cache's
 * custom allocator if one was given and with free() otherwise.
 *
 * key is also converted to the cache's declared KeyT the way a plain C
 * assignment converts it, instead of the key being looked up with its own
 * natural expression type. A cache with a character-pointer key type takes a
 * const char * key as it is. A padded struct key follows the rule in this
 * header's file-level documentation.
 *
 * When you pass the cache across scopes, into a thread function for
 * example, declare a local alias and call clru_redeclare() before
 * clru_get(), so the val type is detected correctly.
 *
 * @param name     Cache variable. Declare it with clru_construct or
 *                 clru_declare, or restore it with clru_redeclare
 * @param key      Key expression (an lvalue or a literal string)
 * @param val_ptr  A pointer to the value type (ValT *), NOT a cmap_pair *.
 *                 For a val type other than char* this is, for example, a
 *                 double * for a double-valued cache: the value is copied
 *                 in, converted to the type of *val_ptr, and the internal
 *                 allocation is freed, so the caller sees no heap memory.
 *                 For a char* val type it is a pointer to a char* variable,
 *                 and ownership of the heap string goes to the caller, who
 *                 must free it with the cache's custom allocator if one was
 *                 given and with free() otherwise.
 * @return ccol_retval_t: ccol_success, ccol_key_not_found or another
 *         return value
 *
 * @note A NULL key of a character-pointer type isn't a string, and gives
 * ccol_invalid_args
 *
 * Example:
 * @code
 * int k = 5; int v = 0;
 * if (clru_get(cache, k, &v) == ccol_success) { ... }
 *
 * char *str = NULL;
 * if (clru_get(str_cache, k, &str) == ccol_success) { free(str); }
 * @endcode
 */
#define clru_get(name, key, val_ptr)                                 \
  _ccol_clru_get_impl(name, (key), (val_ptr),                        \
                      _ccol_uniq(__ccol_clru_get_k, __COUNTER__),    \
                      _ccol_uniq(__ccol_clru_get_kp, __COUNTER__),   \
                      _ccol_uniq(__ccol_clru_get_r, __COUNTER__),    \
                      _ccol_uniq(__ccol_clru_get_vtmp, __COUNTER__), \
                      _ccol_uniq(__ccol_clru_get_vout, __COUNTER__))

/* The value type of the cache `name` without its top-level qualifiers, so
 * clru_get() can write a temporary of that type when the value type is
 * const. _ccol_unqual_type() of common.h gives it and leaves an array type
 * as it is. */
#define _clru_unqual_val_type(name) \
  _ccol_unqual_type(*(name##__clru_val_type_var))

/* The public macro above names all the temporaries of this body with
 * _ccol_uniq(), so it can nest inside the argument of any public macro,
 * itself included. */
#define _ccol_clru_get_impl(name, key, val_ptr, _clru_k, _clru_kp, _clru_r, \
                            _clru_vtmp, _clru_vout)                         \
  ({                                                                        \
    /* The key temporary has the cache's DECLARED key type, so the key is   \
     * converted exactly the way clru_set() converted it. A                 \
     * character-pointer key type keeps the type of the caller's expression \
     * instead, so a const char * key needs no cast; see                    \
     * _ccol_declared_or_own_type() in common.h. The choice of segment and  \
     * the map both read every byte of the key, padding included, so we     \
     * clear this copy's padding. For a type without padding the clear is   \
     * empty. */                                                            \
    _ccol_declared_or_own_type(name##__clru_key_type_var, (key)) _clru_k =  \
        (key);                                                              \
    _ccol_clear_padding(&_clru_k);                                          \
    cmap_pair _clru_kp = {};                                                \
    _populate_cmap_pair(&_clru_kp, _clru_k);                                \
    ccol_retval_t _clru_r;                                                  \
    /* Both arms below get compiled for every ValT, because the condition   \
     * is a constant the compiler folds and not a preprocessor test. So     \
     * every statement in each arm has to be well formed and diagnose       \
     * clean, both for the ValT that makes it dead and for the one that     \
     * makes it live, and this shared temporary is what keeps that true.    \
     * Its type is the cache's declared ValT, so each arm sizes its copy by \
     * that type instead of by a width only one arm's ValT has. Without     \
     * it, a copy in the char* arm sized by sizeof(void *) is a             \
     * _FORTIFY_SOURCE "will always overflow" error in a caller's -Werror   \
     * build whenever ValT is narrower than a pointer, and on a statement   \
     * that never even runs. */                                             \
    _clru_unqual_val_type(name) _clru_vtmp;                                 \
    if (_ccol_is_char_ptr(*(name##__clru_val_type_var))) {                  \
      /* char* path: make a heap copy and hand its ownership to the         \
       * caller */                                                          \
      cmap_pair _clru_vout = {};                                            \
      _clru_r = clrucache_get_full((name), &_clru_kp, &_clru_vout);         \
      if (_clru_r == ccol_success && _clru_vout.ptr) {                      \
        /* Move the heap pointer into the ValT temporary as bytes, because  \
         * a cast would be a pointer-to-integer conversion for every ValT   \
         * that makes this arm dead. Assigning out of the temporary is then \
         * an ordinary typed store, so there's no aliasing question. */     \
        memcpy(&_clru_vtmp, &_clru_vout.ptr, sizeof(_clru_vtmp));           \
        *(val_ptr) = _clru_vtmp;                                            \
      }                                                                     \
    } else {                                                                \
      /* Path for a val type other than char*: read into a temporary of     \
       * the cache's declared ValT, not of the type of *val_ptr, and then   \
       * assign that temporary to *val_ptr. The conversion then goes        \
       * through the C compiler's assignment rules instead of a raw byte    \
       * copy, so a *val_ptr whose type differs from ValT (a double*        \
       * out-parameter for an int-valued cache, say) gets a real numeric    \
       * conversion and not ValT's raw bytes reinterpreted as the type of   \
       * *val_ptr. On failure *val_ptr is left untouched, matching the      \
       * "buf untouched on failure" contract of __clrucache_get_into. */    \
      _clru_r = __clrucache_get_into((name), &_clru_kp, &_clru_vtmp,        \
                                     sizeof(_clru_vtmp));                   \
      if (_clru_r == ccol_success) *(val_ptr) = _clru_vtmp;                 \
    }                                                                       \
    _clru_r;                                                                \
  })

/**
 * @brief Set a value by key. This is a type-inferred convenience wrapper.
 *
 * If a remote setter was given at construction, it is called synchronously
 * before the cache is updated. See clrucache_set_full().
 *
 * key and val are converted to the cache's declared KeyT and ValT the way a
 * plain C assignment converts them, so an int literal passed as val for a
 * double-valued cache becomes the double value and not the int's raw bytes.
 * The compiler has to be able to convert a val or key expression implicitly
 * to the cache's declared ValT or KeyT; one it can't convert is a compile
 * error right here, never a value that gets silently reinterpreted. A
 * character-pointer KeyT or ValT is the exception: the expression's pointer
 * type is kept, so a const char * key or value for a char * cache needs no
 * cast. A padded struct key follows the rule in this header's file-level
 * documentation.
 *
 * name must be a plain variable, declared with clru_construct or
 * clru_declare or restored with clru_redeclare(), because that's where the
 * KeyT and ValT for the conversion come from; it can't be any other
 * expression, such as a read of a struct member. When you pass the cache
 * across scopes, into a thread function for example, declare a local alias
 * and call clru_redeclare() first. clru_get() needs the same.
 *
 * @param name   Cache variable
 * @param key    Key expression
 * @param val    Value expression
 * @return ccol_retval_t
 *
 * @note A NULL key or value of a character-pointer type isn't a string, and
 * gives ccol_invalid_args
 *
 * Example:
 * @code
 * int k = 5; double v = 2.71;
 * clru_set(cache, k, v);
 * @endcode
 */
#define clru_set(name, key, val)                                   \
  _ccol_clru_set_impl(name, (key), (val),                          \
                      _ccol_uniq(__ccol_clru_set_k, __COUNTER__),  \
                      _ccol_uniq(__ccol_clru_set_v, __COUNTER__),  \
                      _ccol_uniq(__ccol_clru_set_kp, __COUNTER__), \
                      _ccol_uniq(__ccol_clru_set_vp, __COUNTER__))

/* The public macro above names all the temporaries of this body with
 * _ccol_uniq(), so it can nest inside the argument of any public macro,
 * itself included. */
#define _ccol_clru_set_impl(name, key, val, _clru_k, _clru_v, _clru_kp,     \
                            _clru_vp)                                       \
  ({                                                                        \
    /* See clru_get() for the type of the key temporary and why its padding \
     * is cleared. The value temporary follows the same type rule, so a     \
     * const char * value for a char * cache needs no cast either. */       \
    _ccol_declared_or_own_type(name##__clru_key_type_var, (key)) _clru_k =  \
        (key);                                                              \
    _ccol_clear_padding(&_clru_k);                                          \
    _ccol_declared_or_own_type(name##__clru_val_type_var, (val)) _clru_v =  \
        (val);                                                              \
    cmap_pair _clru_kp = {};                                                \
    cmap_pair _clru_vp = {};                                                \
    _populate_cmap_pair(&_clru_kp, _clru_k);                                \
    _populate_cmap_pair(&_clru_vp, _clru_v);                                \
    clrucache_set_full((name), &_clru_kp, &_clru_vp);                       \
  })

/**
 * @brief Return the number of live entries the cache holds right now
 */
size_t clrucache_size(clru_cache cache);

/**
 * @brief Return the capacity of the cache
 */
size_t clrucache_capacity(clru_cache cache);

#pragma GCC visibility pop
