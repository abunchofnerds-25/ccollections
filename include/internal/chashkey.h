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
 * chashmap doesn't treat a key as an opaque run of bytes for every type it
 * accepts. A long double is hashed and compared by value, because on an
 * Application Binary Interface (ABI) where that type carries padding those
 * bytes are often not initialized, and -0.0 becomes 0.0 for a float and for
 * a double. So a module that has to agree with the map about which keys are
 * the same key can't just hash the key bytes; it must ask the map.
 *
 * clrucache is one such module. A segmented cache picks a segment from the
 * key and each segment is its own map, so two keys that the map sees as one
 * key must land in the same segment, otherwise the cache ends up with two
 * entries for one key.
 *
 * The header is internal and has no visibility block: make install leaves it
 * out, and its symbol isn't in the dynamic symbol table of the shared
 * library.
 */

#ifndef CCOL_CHASHKEY_H
#define CCOL_CHASHKEY_H

#include <common.h>
#include <stddef.h>

/**
 * @brief Hash a key exactly as chashmap hashes it with no custom hashing proc.
 *
 * Two keys that the map treats as equal get equal hashes here, even when
 * their representations differ in ways that the map ignores on purpose. The
 * hash is the one of a map's keyed mode, keyed with the process secrets,
 * whatever mode any map is in. A map that switched to the keyed mode stores
 * this value for the key, a map in the fast mode stores a different one, and
 * the value is different from one process to the next. Key identity doesn't
 * depend on the mode, so which keys are the same key comes out the same in
 * both. For a fixed-width type only as many bytes as the width of key_type
 * are read, so the caller must check key_size against
 * ccol_fixed_width_data_type_size() first.
 *
 * @param key_ptr  Key bytes.
 * @param key_size Size of the key. Bytes after the width of the key type are
 *                 never read, but the value is used for a float key, a
 *                 double key and a key with no fixed width, so it must match
 *                 the type.
 * @param key_type The declared key type that the caller made the map with.
 */
size_t ccol_chmap_hash_key(const void *key_ptr, size_t key_size,
                           ccol_data_type key_type);

#endif /* CCOL_CHASHKEY_H */
