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
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <string.h>

#include "common.h"

/* Everything declared from here to the end of this header is part of the
 * public Application Binary Interface (ABI) of libccollections. The shared
 * library exports all of it. The library itself is built with
 * -fvisibility=hidden. Any function or object that one of these blocks does
 * not cover stays internal to the library. It is absent from the dynamic
 * symbol table of the library. The application that links against the
 * library cannot interpose it. A symbol of the same name in that application
 * cannot collide with it. */
#pragma GCC visibility push(default)

/**
 * @file clrucache.h
 * @brief Thread-safe generic Least Recently Used (LRU) cache. It can also
 *        use an optional remote source.
 *
 * A hash map gives the cache O(1) lookup. A doubly-linked list gives it O(1)
 * eviction. All operations are fully thread-safe.
 *
 * The library divides a cache that is large enough into several segments.
 * Each segment has its own lock. A key belongs to exactly one segment, and a
 * hash of the key selects that segment. Threads that work on keys in
 * different segments do not wait for one another. Operations on the same key
 * still serialise. The guarantees below rest on that. The division starts at
 * 128 entries. This is the first capacity that gives two segments 64 entries
 * each. Below 128 entries a cache is a single segment. It then keeps one
 * exact, global least-recently-used order. At 128 entries and above, the
 * library divides the capacity across the segments. Each segment then evicts
 * its own least recently used entry. See clrucache_create_full() for the full
 * account.
 *
 * Key concurrency guarantees:
 * - Many getters for the same uncached key coalesce. Only one remote fetch
 *   runs. All the other getters block, and they get the same result after
 *   that fetch completes.
 * - A getter blocks while a setter for the same key is in progress. This is
 *   why a getter always reads a consistent value. When the set fails and the
 *   key is then absent from the cache, the getter proceeds exactly as a get
 *   that starts after the failed set: it runs the remote getter, or joins a
 *   fetch that another getter started, or reports ccol_key_not_found when the
 *   cache has no remote getter.
 * - The library serializes many setters for the same key.
 *
 * Set semantics: the library always calls the remote setter before it updates
 * the cache, if the caller gave a remote setter. If the remote call fails,
 * the library does not update the cache. clrucache_set_full() then returns
 * ccol_unexpected_failure. A failed set does nothing at all to its key. The
 * key keeps its old value and its exact place in the eviction order. The
 * library does not spare the key, and it does not move the key forward to
 * become the next entry to evict.
 *
 * A set takes effect when its remote setter returns. While that call runs,
 * the key keeps its old value, its place in the eviction order and its place
 * in clrucache_size(). An insert of another key that needs room during that
 * time evicts the least recently used key, and that can be the key of the
 * set. The eviction callback then receives the old value. If the set then
 * fails, the key stays evicted. If it succeeds, the key comes back with the
 * new value as the most recently used entry, which can evict another key. The
 * result is always that of the insert followed by the set, and a segment never
 * holds more entries than its capacity.
 *
 * Limitations and caller responsibilities:
 * - The library calls the eviction callback while it holds the lock of the
 *   owning segment. The callback MUST NOT call back into the cache, because
 *   that is a deadlock. The library does NOT serialise the callback against
 *   itself. Two segments can evict at the same time on two threads. A
 *   callback that keeps state of its own must guard that state.
 * - Do not call clrucache_destroy() while another thread still uses the
 *   cache. Call it only after all the other threads stop.
 * - A key of a type that the cache does not know, such as a struct, is the
 *   same key as another one only when every byte of the two is equal. Both
 *   the choice of segment and the internal map read every byte of the key,
 *   and that includes the padding bytes of a struct. The cache takes no
 *   custom hash or key equality function, so the bytes of a padded key must
 *   be fully determined. Through the raw functions, such as
 *   clrucache_set_full(), the cache reads exactly the object that
 *   key_pair->ptr points at, so zero the whole key object with memset()
 *   before you set its members. Through clru_get() and clru_set(), the cache
 *   sees the macro's own copy of the key, and C leaves the padding of a
 *   struct copy unspecified. With GCC 11 or later the macros set that padding
 *   to zero. With any other compiler, use the raw functions for a padded key.
 */

/* ========================================================================== */
/*                         CALLBACK TYPES                                     */
/* ========================================================================== */

/**
 * @brief Remote getter callback
 *
 * The library calls this callback when the requested key is not in the cache
 * and a remote source exists. On success the callback must allocate the
 * value data on the heap. It must assign that pointer to val->ptr, and set
 * val->size to the byte length. The cache takes ownership of val->ptr. The
 * cache frees val->ptr with its own allocator. That allocator is the custom
 * ccol_memmgmt_procs_t that the caller gives to clrucache_create_full, or
 * malloc()/free() if the caller gives none. The allocation that the callback
 * makes MUST use that same allocator. Use the malloc or calloc of that
 * allocator if the caller configures one, or plain malloc() if not. Without
 * this, the later free of val->ptr by the cache corrupts the heap. On
 * failure the callback must leave val->ptr as NULL and return false.
 *
 * The library calls this callback with no lock of the cache held, so the
 * callback can use the cache for any OTHER key. It must not call back into
 * the same cache for the key that it serves: a get or a set of that key
 * waits for the operation that runs this callback, which never finishes, and
 * the thread deadlocks against itself. It must not destroy the cache either,
 * because the destroy waits for the call that runs this callback.
 *
 * @param key  Key pair (ptr + size)
 * @param val  Output pair to fill on success (ptr + size)
 * @return true on success, false on failure
 */
typedef bool (*clru_remote_getter_t)(const cmap_pair *key, cmap_pair *val);

/**
 * @brief Remote setter callback
 *
 * This callback tries to write a key-value pair to the remote source.
 *
 * The library calls this callback with no lock of the cache held, so the
 * callback can use the cache for any OTHER key. It must not call back into
 * the same cache for the key that it serves: a get or a set of that key
 * waits for the operation that runs this callback, which never finishes, and
 * the thread deadlocks against itself. It must not destroy the cache either,
 * because the destroy waits for the call that runs this callback.
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
 * The library calls this callback when it evicts an entry to make room for a
 * new one. The call is synchronous, and the library holds the lock of the
 * owning segment during the call. The callback must not call back into the
 * cache.
 *
 * The library holds the lock of that one segment only. Two evictions in
 * different segments can therefore run this callback at the same time on two
 * threads. Any state that the callback keeps of its own needs its own
 * protection.
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
 * clru_cache is an opaque VALUE handle, not a pointer. It is a packed pair of
 * a slot index and a generation. Never cast it to void*, and never cast a
 * void* to it. Never compare it with a pointer cast, and never treat it as an
 * address. Compare it directly against CLRU_CACHE_INVALID. You can also use
 * it in a truthiness check, because CLRU_CACHE_INVALID is 0. `if (!cache)` is
 * therefore a valid test for an invalid handle. Inside the library, every use
 * of a clru_cache goes through a slot table that the library owns. The
 * library resolves the handle there before it touches the cache object. The
 * library always detects a handle whose slot it already freed. It also
 * detects a handle whose slot it reused for a different, later cache. It
 * never dereferences freed memory, and it never dereferences the memory of
 * the wrong object. See the doc comment of __clrucache_destroy for what
 * happens when a stale handle reaches that function.
 */
typedef uint64_t clru_cache;

/** @brief Sentinel value for "no cache". It is the clru_cache equivalent of
 *         NULL. */
#define CLRU_CACHE_INVALID ((clru_cache)0)

/* ========================================================================== */
/*                         CREATION / DESTRUCTION                             */
/* ========================================================================== */

/**
 * @brief Create an LRU cache
 *
 * @param capacity     The maximum number of live entries before eviction
 *                     happens
 * @param key_type ccol_data_type of keys (from ccol_determine_ccol_data_type)
 * @param val_type ccol_data_type of values (from ccol_determine_ccol_data_type)
 * @param getter       Remote getter (can be NULL)
 * @param setter       Remote setter (can be NULL). If it is not NULL, the
 *                     library calls it synchronously before each update of
 *                     the cache.
 * @param eviction_cb  The library calls this on eviction (can be NULL)
 * @param mprocs       Custom allocator, or NULL for malloc/free. The cache
 *                     keeps a copy of the struct, so the caller may release
 *                     or reuse the struct once this call returns. The
 *                     functions that it names must stay callable until the
 *                     cache is destroyed.
 * @param err          Optional. On failure the library sets it to an error
 *                     string
 * @return The new cache handle, or CLRU_CACHE_INVALID on failure
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
 * cache must be a live handle. A live handle is one that
 * clrucache_create_full, clru_init or clru_construct returned, and that no
 * call destroyed yet. A stale handle is a fatal error. A handle is stale when
 * an earlier, complete call to this same function destroyed it. It is also
 * stale when another thread destroys it at this same moment. A forged value
 * or garbage is a fatal error too. In each of these cases this function calls
 * ccol_fatal_err(), which calls abort() and raises SIGABRT. It does this
 * rather than risk a use-after-free or a double-free. This rule covers a
 * purely sequential second destroy and a concurrent destroy that overlaps in
 * time. CLRU_CACHE_INVALID (0) is the one exception, and it stays a silent
 * no-op. This matches the "destroy sets the handle to NULL" idiom of
 * clru_destroy.
 *
 * On a live handle, this function blocks until every in-flight resolved use
 * of the handle stops. It then evicts all the entries that remain, and calls
 * the eviction callback for each one. Then it frees all the memory.
 */
void __clrucache_destroy(clru_cache cache);

/**
 * @brief Copy a value directly into a buffer that the caller gives. This is
 *        internal. Use the clru_get() macro instead.
 *
 * clrucache_get_full() allocates memory on the heap. This function does not.
 * It copies the stored value into buf while it holds the lock of the owning
 * segment, and then it returns.
 *
 * buf_size must be exactly equal to the size of the stored value. The
 * clru_get() macro obeys this rule, because it passes sizeof(ValT) and not
 * sizeof(*val_ptr). The macro reads into a temporary of type ValT. It then
 * converts that temporary and assigns it into *val_ptr. The type of *val_ptr
 * can differ from ValT for a good reason. For example, an int-valued cache
 * can have a double* out-parameter. The two sizes then differ too.
 *
 * A stored value of any other size than ValT gets ccol_unexpected_failure.
 * This is true for a larger value and for a smaller one. The function then
 * leaves buf completely untouched. It never copies a partial value into buf.
 * This only happens for a value in the cache that came from somewhere else.
 * A getter or a setter that always makes values of sizeof(ValT) bytes for
 * this key never produces it.
 */
ccol_retval_t __clrucache_get_into(clru_cache cache, const cmap_pair *key_pair,
                                   void *buf, size_t buf_size);

/* ========================================================================== */
/*                         OPERATIONS                                         */
/* ========================================================================== */

/**
 * @brief Get a value by key
 *
 * On success this function allocates a copy of the stored value on the heap.
 * It uses the custom allocator of the cache if the caller gave one at
 * construction, and malloc() if not. It assigns the copy to val_out->ptr. It
 * sets val_out->size to the stored byte size. The caller must free
 * val_out->ptr with the matching allocator. That is the custom free function
 * if the caller configured a custom allocator, and free() if not.
 *
 * On a cache miss the library calls the remote getter one time, if the caller
 * gave a remote getter. The other getters for the same key block until that
 * call completes. The library stores the fetched value in the cache and gives
 * it to all the waiters.
 *
 * This function blocks while a setter for this key is in progress. It waits
 * until the cache is in a consistent state. When that set fails and leaves
 * the key absent, this call continues as a cache miss, so the remote getter
 * runs for it.
 *
 * @param cache     Cache handle
 * @param key_pair  The key to look up. For a fixed-width key type, its size
 *                  must be equal to the width of that type. If it is not, the
 *                  call reports ccol_invalid_args and does not read the key
 * @param val_out   On success, val_out->ptr is a copy of the value on the
 *                  heap. The caller must free it with the allocator of the
 *                  cache. val_out->size is the size of the stored value
 * @return ccol_success, ccol_key_not_found, ccol_invalid_args,
 *         ccol_not_enough_memory, ccol_unexpected_failure or
 *         ccol_container_full. The call gives ccol_unexpected_failure when it
 *         joins the cache-miss fetch of another thread, and that fetch stores
 *         a value of the wrong size. The size is wrong when it does not agree
 *         with what the caller of that other thread expects. The call gives
 *         ccol_container_full on a cache miss, when the internal records of
 *         the cache reach their own maximum element count
 */
ccol_retval_t clrucache_get_full(clru_cache cache, const cmap_pair *key_pair,
                                 cmap_pair *val_out);

/**
 * @brief Set a value by key
 *
 * The library calls the remote setter first, if the caller gave one at
 * construction. On success the library updates the cache. The key then
 * becomes the most recently used entry of its segment. On failure the
 * function returns ccol_unexpected_failure, and the cache does not change for
 * that key. The stored value of the key survives the call, and so does the
 * exact position of the key in the eviction order. This is why a set that
 * stores nothing never costs another key its place as the next entry to
 * evict.
 *
 * The set takes effect when the remote setter returns. Until then the key is
 * an ordinary entry of its segment, and a concurrent insert that needs room
 * can evict it. A set that then succeeds inserts the key again with the new
 * value. A set that fails leaves it evicted. See the file-level
 * documentation of this header.
 *
 * The library serializes many setters for the same key. A getter for the same
 * key blocks until the set completes, because the cache is consistent at that
 * time.
 *
 * @param cache    Cache handle
 * @param key_pair The key to set
 * @param val_pair The value to pair with the key
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory,
 *         ccol_unexpected_failure or ccol_container_full. The call gives
 *         ccol_unexpected_failure when the synchronous remote call fails. It
 *         gives ccol_container_full when it sets a completely new key and the
 *         internal bookkeeping of the cache reaches its own maximum element
 *         count
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
 * A call on a *cp that is already CLRU_CACHE_INVALID is safe, and it does
 * nothing. A call on a stale handle that is not CLRU_CACHE_INVALID is fatal
 * misuse. A handle is stale when some other path already destroyed it.
 * __clrucache_destroy documents this same misuse.
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
 * This macro blocks until every in-flight resolved use of the handle stops.
 * Do not call it at the same time as another call on the same handle. See the
 * doc comment of __clrucache_destroy for what happens then. The result is a
 * fatal error, not a silent race.
 *
 * @note The macro evaluates name exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define clru_destroy(name)      \
  _ccol_clru_destroy_impl(name, \
                          _ccol_uniq(__ccol_clru_destroy_slot, __COUNTER__))

/* Internal. The body of clru_destroy. slot is a name from _ccol_uniq(), so
 * the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
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
 * Call clru_init() on the variable before you use it.
 */
#define clru_declare(name, KeyT, ValT)                                    \
  __typeof__(KeyT) *name##__clru_key_type_var                             \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  __typeof__(ValT) *name##__clru_val_type_var                             \
      __attribute__((unused)); /* deliberately not initialized to NULL */ \
  clru_cache name              /* deliberately not initialized to NULL */

/**
 * @brief Declare a cache variable that the compiler cleans up at scope exit
 */
#define clru_declare_scoped(name, KeyT, ValT)                                 \
  __typeof__(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  __typeof__(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL; \
  clru_cache name _ccol_destructor(___clrucache_destroy) = CLRU_CACHE_INVALID

/**
 * @brief Initialize a previously declared cache variable
 *
 * @param name       Cache variable. Declare it with clru_declare first
 * @param capacity   The maximum number of live entries before eviction
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
      ccol_fatal_err("clru_init('%s'): %s", #name,                             \
                     _clru_err ? _clru_err : "unknown error");                 \
    }                                                                          \
  } while (0)

/**
 * @brief Declare and initialize in one step. This is the most common use.
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
      ccol_fatal_err("clru_construct('%s'): %s", #name,                        \
                     _clru_err ? _clru_err : "unknown error");                 \
    }                                                                          \
  } while (0)

/**
 * @brief Declare, initialize, and destroy automatically at scope exit
 */
#define clru_construct_scoped(name, KeyT, ValT, capacity, getter, setter,      \
                              evict_cb)                                        \
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
      ccol_fatal_err("clru_construct_scoped('%s'): %s", #name,                 \
                     _clru_err ? _clru_err : "unknown error");                 \
    }                                                                          \
  } while (0)

/**
 * @brief Restore the type-tracking variables for a cache that the caller
 *        passes across scopes
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
  __typeof__(KeyT) *name##__clru_key_type_var __attribute__((unused)) = NULL; \
  __typeof__(ValT) *name##__clru_val_type_var __attribute__((unused)) = NULL

/* ========================================================================== */
/*                    TYPE-INFERRED GET / SET MACROS */
/* ========================================================================== */

/**
 * @brief Get a value by key. This is a type-inferred convenience wrapper.
 *
 * For a val type that is not char*, the macro converts the stored value to
 * the type of *val_ptr. It converts the value in the same way as a plain
 * C assignment. For example, a cache that stores int values and reads into a
 * double* out-parameter converts the int to a double. It does not copy the
 * raw bytes of the int. The macro then frees the internal heap allocation. A
 * type of *val_ptr that the compiler cannot convert implicitly from the
 * declared ValT of the cache is a compile error at this point. It is never a
 * value that the macro reinterprets in silence.
 *
 * For a char* val type, the macro gives the ownership of the string on the
 * heap to *(char **)val_ptr. The caller must free that string. Use the custom
 * allocator of the cache if the caller gave one, and free() if not.
 *
 * The macro also converts key to the declared KeyT of the cache, in the same
 * way as a plain C assignment. It does not look up the key with the natural
 * expression type of key. A cache with a character-pointer key type accepts
 * a const char * key as it is. A padded struct key follows the rule in the
 * file-level documentation of this header.
 *
 * Do this when you pass the cache across scopes, for example into a thread
 * function. Declare a local alias, and call clru_redeclare() before you call
 * clru_get(). The detection of the val type then works correctly.
 *
 * @param name     Cache variable. Declare it with clru_construct or
 *                 clru_declare, or restore it with clru_redeclare
 * @param key      Key expression (an lvalue or a literal string)
 * @param val_ptr  A pointer to the value type (ValT *), NOT a cmap_pair *.
 *                 For a val type that is not char*, this is for example a
 *                 double * for a double-valued cache. The macro copies the
 *                 value in and converts it to the type of *val_ptr. It
 *                 frees the internal allocation. The caller sees no heap
 *                 memory.
 *                 For a char* val type, this is a pointer to a char*
 *                 variable. The macro gives the ownership of the string on
 *                 the heap to the caller. The caller must free it with the
 *                 custom allocator of the cache if the caller gave one, and
 *                 with free() if not.
 * @return ccol_retval_t. This is ccol_success, ccol_key_not_found or another
 *         return value
 *
 * @note A NULL key of a character-pointer type is not a string. The macro
 * returns ccol_invalid_args for it
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

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_clru_get_impl(name, key, val_ptr, _clru_k, _clru_kp, _clru_r,  \
                            _clru_vtmp, _clru_vout)                          \
  ({                                                                         \
    /* The key temporary carries the DECLARED key type of the cache, so the  \
     * key converts exactly as clru_set() converted it. A character-pointer  \
     * key type keeps the type of the caller expression instead, so a const  \
     * char * key needs no cast; see _ccol_declared_or_own_type() in         \
     * common.h. Both the choice of segment and the map read every byte of   \
     * the key, padding included, so the padding of this copy is cleared.    \
     * For a type without padding that clear is empty. */                    \
    _ccol_declared_or_own_type(name##__clru_key_type_var, (key)) _clru_k =   \
        (key);                                                               \
    _ccol_clear_padding(&_clru_k);                                           \
    cmap_pair _clru_kp = {};                                                 \
    _populate_cmap_pair(&_clru_kp, _clru_k);                                 \
    ccol_retval_t _clru_r;                                                   \
    /* The compiler compiles both arms below for every ValT. The condition   \
     * is a constant that the compiler folds. It is not a preprocessor test. \
     * Every statement in each arm must therefore be well formed and         \
     * diagnosed clean. This is true for the ValT that makes the statement   \
     * dead, and for the ValT that makes it live. This shared temporary is   \
     * what keeps that true. Its type is the declared ValT of the cache.     \
     * Each arm therefore sizes its copy by that type. It does not size the  \
     * copy by a width that only the ValT of one arm has. Without this, a    \
     * copy in the char* arm sized by sizeof(void *) is a _FORTIFY_SOURCE    \
     * "will always overflow" error in the -Werror build of a caller. That   \
     * error happens whenever ValT is narrower than a pointer, and it        \
     * happens on a statement that never runs. */                            \
    __typeof__(*(name##__clru_val_type_var)) _clru_vtmp;                     \
    if (ccol_is_char_ptr(*(name##__clru_val_type_var))) {                    \
      /* char* path: allocate a copy on the heap and give the ownership of   \
       * it to the caller */                                                 \
      cmap_pair _clru_vout = {};                                             \
      _clru_r = clrucache_get_full((name), &_clru_kp, &_clru_vout);          \
      if (_clru_r == ccol_success && _clru_vout.ptr) {                       \
        /* Move the heap pointer into the ValT temporary as bytes. A cast    \
         * is a pointer-to-integer conversion for every ValT that makes      \
         * this arm dead. The assignment out of the temporary is then an     \
         * ordinary typed store. This is why it carries no question about    \
         * aliasing. */                                                      \
        memcpy(&_clru_vtmp, &_clru_vout.ptr, sizeof(_clru_vtmp));            \
        *(val_ptr) = _clru_vtmp;                                             \
      }                                                                      \
    } else {                                                                 \
      /* Path for a val type that is not char*: read into a temporary whose  \
       * type is the declared ValT of the cache. The type is not the         \
       * type of *val_ptr. Then assign that temporary into *val_ptr. This    \
       * routes the conversion through the assignment rules of the C         \
       * compiler, and not through a raw byte copy. This is why a *val_ptr   \
       * whose type differs from ValT gets a true numeric conversion. An     \
       * example is a double* out-parameter for an int-valued cache. The     \
       * macro does not reinterpret the raw bytes of ValT as the type of     \
       * *val_ptr. On failure the macro leaves *val_ptr untouched. This      \
       * matches the "buf untouched on failure" contract of                  \
       * __clrucache_get_into. */                                            \
      _clru_r = __clrucache_get_into((name), &_clru_kp, &_clru_vtmp,         \
                                     sizeof(_clru_vtmp));                    \
      if (_clru_r == ccol_success) *(val_ptr) = _clru_vtmp;                  \
    }                                                                        \
    _clru_r;                                                                 \
  })

/**
 * @brief Set a value by key. This is a type-inferred convenience wrapper.
 *
 * The library calls the remote setter synchronously before it updates the
 * cache, if the caller gave a remote setter at construction. See
 * clrucache_set_full().
 *
 * The macro converts key and val to the declared KeyT and ValT of the cache.
 * It converts them in the same way as a plain C assignment. For example, an
 * int literal that goes in as val for a double-valued cache becomes the
 * double value. The macro does not store the raw bytes of the int. The
 * compiler must be able to convert a val or key expression implicitly to the
 * declared ValT or KeyT of the cache. One that it cannot convert is a compile
 * error at this point. It is never a value that the macro reinterprets in
 * silence. A character-pointer KeyT or ValT is the exception: the macro
 * keeps the pointer type of the expression, so a const char * key or value
 * for a char * cache needs no cast. A padded struct key follows the rule in
 * the file-level documentation of this header.
 *
 * name must be a plain variable. Declare it with clru_construct or
 * clru_declare, or restore it with clru_redeclare(). This is what gives the
 * KeyT and the ValT for the conversion above. name cannot be any other
 * expression, for example a read of a struct member. Do this when you pass
 * the cache across scopes, for example into a thread function: declare a
 * local alias, and call clru_redeclare() first. clru_get() needs the same.
 *
 * @param name   Cache variable
 * @param key    Key expression
 * @param val    Value expression
 * @return ccol_retval_t
 *
 * @note A NULL key or value of a character-pointer type is not a string.
 * The macro returns ccol_invalid_args for it
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

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_clru_set_impl(name, key, val, _clru_k, _clru_v, _clru_kp,       \
                            _clru_vp)                                         \
  ({                                                                          \
    /* See clru_get() for the type of the key temporary and for why its       \
     * padding is cleared. The value temporary follows the same type rule, so \
     * a const char * value for a char * cache needs no cast either. */       \
    _ccol_declared_or_own_type(name##__clru_key_type_var, (key)) _clru_k =    \
        (key);                                                                \
    _ccol_clear_padding(&_clru_k);                                            \
    _ccol_declared_or_own_type(name##__clru_val_type_var, (val)) _clru_v =    \
        (val);                                                                \
    cmap_pair _clru_kp = {};                                                  \
    cmap_pair _clru_vp = {};                                                  \
    _populate_cmap_pair(&_clru_kp, _clru_k);                                  \
    _populate_cmap_pair(&_clru_vp, _clru_v);                                  \
    clrucache_set_full((name), &_clru_kp, &_clru_vp);                         \
  })

/**
 * @brief Return the number of live entries that the cache holds now
 */
size_t clrucache_size(clru_cache cache);

/**
 * @brief Return the capacity of the cache
 */
size_t clrucache_capacity(clru_cache cache);

#pragma GCC visibility pop
