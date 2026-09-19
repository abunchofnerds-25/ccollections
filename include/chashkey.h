/*
 * MIT License
 *
 * Copyright (c) 2026 - A bunch of nerds
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file chashkey.h
 * @brief INTERNAL ONLY. The map's own notion of key identity, as a hash.
 *
 * chashmap does not treat a key as an opaque run of bytes for every type it
 * accepts. A long double is hashed and compared by value, because on an ABI
 * where the type carries padding those bytes are routinely uninitialised, and
 * -0.0 is canonicalised to 0.0 for float and double. Any other module that has
 * to agree with the map on which keys are the same key therefore cannot hash
 * the key's bytes itself; it has to ask.
 *
 * clrucache is such a module: a segmented cache picks a segment from the key
 * and each segment is its own map, so two keys the map would treat as one must
 * land on the same segment or the cache holds two entries for one key.
 *
 * This header is internal. It carries no visibility block, is excluded from
 * make install, and its symbol is absent from the shared library's dynamic
 * symbol table.
 */

#ifndef CCOL_CHASHKEY_H
#define CCOL_CHASHKEY_H

#include <stddef.h>

#include "common.h"

/**
 * @brief Hash a key exactly as chashmap hashes it with no custom hashing proc.
 *
 * Keys the map considers equal hash equal here, including across a
 * representation the map deliberately ignores. Reads only as many bytes as
 * key_type's own width for a fixed-width type, so key_size must already have
 * been checked against ccol_fixed_width_data_type_size() by the caller.
 *
 * @param key_ptr  Key bytes.
 * @param key_size Size of the key. Bytes past the key type's own width are
 *                 never read, but the value is used for a float or double key
 *                 as well as for a non-fixed-width one, so it must match the
 *                 type.
 * @param key_type The declared key type the map was created with.
 */
size_t ccol_chmap_hash_key(const void *key_ptr, size_t key_size,
                           ccol_data_type key_type);

#endif /* CCOL_CHASHKEY_H */
