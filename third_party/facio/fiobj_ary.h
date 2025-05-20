#ifndef FIOBJ_ARRAY_H
/*
Copyright: Boaz Segev, 2017-2019
License: MIT
*/

/**
A dynamic Array type for the fiobj_s dynamic type system.
*/
#define FIOBJ_ARRAY_H

#include <fiobject.h>

#ifdef __cplusplus
extern "C" {
#endif

/* *****************************************************************************
Array creation API
***************************************************************************** */

/** Creates a mutable empty Array object. Use `fiobj_free` when done. */
FIOBJ fiobj_ary_new(void);

/* *****************************************************************************
Array direct entry access API
***************************************************************************** */

/** Returns the number of elements in the Array. */
size_t fiobj_ary_count(FIOBJ ary);

/**
 * Returns a temporary object owned by the Array.
 *
 * Wrap this function call within `fiobj_dup` to get a persistent handle. i.e.:
 *
 *     fiobj_dup(fiobj_ary_index(array, 0));
 *
 * Negative values are retrieved from the end of the array. i.e., `-1`
 * is the last item.
 */
FIOBJ fiobj_ary_index(FIOBJ ary, int64_t pos);
/** alias for `fiobj_ary_index` */
#define fiobj_ary_entry(a, p) fiobj_ary_index((a), (p))

/* *****************************************************************************
Array push / shift API
***************************************************************************** */

/**
 * Pushes an object to the end of the Array.
 */
void fiobj_ary_push(FIOBJ ary, FIOBJ obj);

/** Pops an object from the end of the Array. */
FIOBJ fiobj_ary_pop(FIOBJ ary);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
