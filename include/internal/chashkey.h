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

// INTERNAL ONLY. The hash that gives chashmap's own idea of key identity.
// The map hashes and compares a long double by value (its padding bytes are
// often not initialized) and folds -0.0 into 0.0 for float and double, so a
// module that must agree with the map about which keys are the same key has
// to ask the map. The segment chooser of clrucache is one: two keys that the
// map sees as one must land in the same segment.

#ifndef CCOL_CHASHKEY_H
#define CCOL_CHASHKEY_H

#include <common.h>
#include <stddef.h>

// Hashes a key exactly as a map with no custom hashing proc does, so keys the
// map treats as equal get equal hashes. It's always the keyed-mode hash with
// the process secrets, whatever mode a map is in, and it differs from one
// process to the next. For a fixed-width type only the width of key_type is
// read, so check key_size with ccol_fixed_width_data_type_size() first.
// The size itself is used for float, double and keys with no fixed width.
size_t ccol_chmap_hash_key(const void *key_ptr, size_t key_size,
                           ccol_data_type key_type);

#endif /* CCOL_CHASHKEY_H */
