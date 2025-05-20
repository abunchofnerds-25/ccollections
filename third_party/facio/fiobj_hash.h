/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/
#ifndef H_FIOBJ_HASH_H
/**
 * The facil.io Hash object is an ordered Hash Table implementation.
 *
 * By compromising some of the HashMap's collision resistance (comparing only
 * the Hash values rather than comparing key data), memory comparison can be
 * avoided and performance increased.
 *
 * By being ordered it's possible to iterate over key-value pairs in the order
 * in which they were added to the Hash table, making it possible to output JSON
 * in a controlled manner.
 */
#define H_FIOBJ_HASH_H

#include <errno.h>
#include <fio_siphash.h>
#include <fiobj_str.h>
#include <fiobject.h>

#ifdef __cplusplus
extern "C" {
#endif

/* *****************************************************************************
Hash Creation
***************************************************************************** */

/**
 * Creates a mutable empty Hash object. Use `fiobj_free` when done.
 *
 * Notice that these Hash objects are optimized for smaller collections and
 * retain order of object insertion.
 */
FIOBJ fiobj_hash_new(void);

/* *****************************************************************************
Hash properties and state
***************************************************************************** */

/** Returns the number of elements in the Hash. */
size_t fiobj_hash_count(const FIOBJ hash);

/** Returns the key for the object in the current `fiobj_each` loop (if any). */
FIOBJ fiobj_hash_key_in_loop(void);

/* *****************************************************************************
Populating the Hash
***************************************************************************** */

/**
 * Sets a key-value pair in the Hash, duplicating the Symbol and **moving**
 * the ownership of the object to the Hash.
 *
 * Returns -1 on error.
 */
int fiobj_hash_set(FIOBJ hash, FIOBJ key, FIOBJ obj);

/**
 * Replaces the value in a key-value pair, returning the old value (and it's
 * ownership) to the caller.
 *
 * A return value of FIOBJ_INVALID indicates that no previous object existed
 * (but a new key-value pair was created.
 *
 * Errors are silently ignored.
 *
 * Remember to free the returned object.
 */
FIOBJ fiobj_hash_replace(FIOBJ hash, FIOBJ key, FIOBJ obj);

/**
 * Returns a temporary handle to the object associated hashed key value.
 *
 * This function takes a `uint64_t` Hash value (see `fio_siphash`) to
 * perform a lookup in the HashMap, which is slightly faster than the other
 * variations.
 *
 * Returns FIOBJ_INVALID if no object is associated with this hashed key value.
 */
FIOBJ fiobj_hash_get2(const FIOBJ hash, uint64_t key_hash);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
