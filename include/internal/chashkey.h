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
 * @brief INTERNAL ONLY. The hash that gives the map's own idea of key
 *        identity.
 *
 * chashmap does not treat a key as an opaque run of bytes for every type that
 * it accepts. It hashes and compares a long double by value. On an Application
 * Binary Interface (ABI) where that type carries padding, those bytes are
 * often not initialized. The map also changes -0.0 to 0.0 for a float and for
 * a double. Another module can have to agree with the map on which keys are
 * the same key. This is why such a module cannot hash the bytes of the key
 * itself. That module must ask the map.
 *
 * clrucache is such a module. A segmented cache picks a segment from the key,
 * and each segment is its own map. Two keys that the map treats as one key
 * must go to the same segment. If they do not, the cache holds two entries for
 * one key.
 *
 * This header is internal. It carries no visibility block. make install
 * excludes it, and its symbol is absent from the dynamic symbol table of the
 * shared library.
 */

#ifndef CCOL_CHASHKEY_H
#define CCOL_CHASHKEY_H

#include <common.h>
#include <stddef.h>

/**
 * @brief Hash a key exactly as chashmap hashes it with no custom hashing proc.
 *
 * Two keys that the map treats as equal get equal hashes here. This is also
 * true when the two keys have different representations that the map ignores
 * on purpose. The hash is the one of the keyed mode of a map, keyed with the
 * secrets of the process, whatever the mode of any map is: a map that has
 * switched to the keyed mode stores this value for the key, a map in the fast
 * mode stores a different one, and the value differs from one process to the
 * next. Key identity does not depend on the mode, so the answer to which keys
 * are the same key is the same in both. For a fixed-width type, this function
 * reads only as many bytes as the width of key_type. This is why the caller
 * must first check key_size against ccol_fixed_width_data_type_size().
 *
 * @param key_ptr  Key bytes.
 * @param key_size Size of the key. The function never reads the bytes that
 *                 come after the width of the key type. But it uses this value
 *                 for a float key, for a double key, and for a key that has no
 *                 fixed width. This is why the value must match the type.
 * @param key_type The declared key type that the caller made the map with.
 */
size_t ccol_chmap_hash_key(const void *key_ptr, size_t key_size,
                           ccol_data_type key_type);

#endif /* CCOL_CHASHKEY_H */
