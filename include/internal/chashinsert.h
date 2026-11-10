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

// INTERNAL ONLY. Entry points that the JSON and YAML parsers and a few other
// modules need from chashmap: an insert that hands back the entry of a key
// that's already present (one hash and one probe, so the caller can free
// what an old owning pointer owns), an ordered cursor, an allocation-free
// walk, and a constructor for maps that mostly stay small.

#ifndef CCOL_CHASHINSERT_H
#define CCOL_CHASHINSERT_H

#include <chashmap.h>
#include <common.h>

// Inserts key_pair with val_pair when the key is absent and leaves the map
// alone when it's present, so a module that stores owning pointers can free
// the old value without a second lookup: one hash, one probe, and the same
// validation as chashmap_insert_elem(). On ccol_success (inserted) and on
// ccol_key_already_present (unchanged) *val_slot describes the value the map
// holds, with the contract of chashmap_get_elem_ref(): you may overwrite the
// bytes within val_slot->size until the next change of the map. On any other
// code the map is unchanged and *val_slot stays {NULL, 0}. A NULL val_slot
// gives ccol_invalid_args.
ccol_retval ccol_chmap_insert_or_get_elem(chmap chm, const cmap_pair *key_pair,
                                          const cmap_pair *val_pair,
                                          cmap_pair *val_slot);

// Inserts a key that's absent like ccol_chmap_insert_or_get_elem() and also
// gives the accessor of the stored key, so a caller can use the map's own copy
// of the key (as the key of an order list it keeps, say). On both success codes
// *key_slot and *val_slot point at the accessors the entry's node keeps; you
// may write the value bytes but never the key bytes, which the map hashes and
// compares. A separate-chaining node never moves, so both stay valid until the
// key is deleted or the map is reset or destroyed, value replacements included.
// An open-addressing map has no key accessor and is refused. On failure both
// out parameters stay NULL; a NULL out parameter, a NULL chm and an
// open-addressing map give ccol_invalid_args.
ccol_retval ccol_chmap_insert_or_get_entry(chmap chm, const cmap_pair *key_pair,
                                           const cmap_pair *val_pair,
                                           const cmap_pair **key_slot,
                                           const cmap_pair **val_slot);

// An opaque reference to one entry of a separate-chaining map, the cursor of
// a walk in insertion order. It names the same node as the key accessor of
// ccol_chmap_insert_or_get_entry() and stays valid until its key is deleted
// or the map is reset or destroyed.
typedef struct ccol_chmap_entry_ref ccol_chmap_entry_ref;

// Gives the oldest live entry, the start of a walk in insertion order, which
// the serializers use to write members in the order they were set. Replacing a
// value keeps an entry in place and a delete takes it off the list. NULL for an
// empty map, a NULL chm and an open-addressing map, which keeps no insertion
// order. It can't fail.
const ccol_chmap_entry_ref *ccol_chmap_oldest_entry(chmap chm);

// Reads the key and value accessors of a live entry and gives the next newer
// entry, or NULL; the step of the walk in insertion order. The successor is
// read during the call, so you may delete the key of entry and carry on from
// it; deleting any other key can free it, though.
const ccol_chmap_entry_ref *ccol_chmap_entry_read(
    const ccol_chmap_entry_ref *entry, const cmap_pair **key,
    const cmap_pair **val);

// The position of a ccol_chmap_walk_next() walk. Zero it before the first
// call.
typedef struct ccol_chmap_walk {
  size_t next_slot;
  const ccol_chmap_entry_ref *next_entry;
  bool started;
} ccol_chmap_walk;

// Gives the next live entry of a map of either backend: oldest first for
// separate chaining, slot order for open addressing. It allocates nothing and
// can't fail, so teardowns that release what the values point to use it
// instead of chashmap_begin_iter(). A NULL chm walks as an empty map. The map
// must not change during the walk. Gives false once every entry was given.
bool ccol_chmap_walk_next(chmap chm, ccol_chmap_walk *walk, const void **key,
                          const void **val);

// Creates a map for a module that makes many maps that mostly stay small. It
// behaves like chashmap_create_mp() with
// CHMAP_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, except that a separate-chaining
// map starts with 4 buckets inside its own block (one allocation). After its
// first growth it has the sizes of any other map, and deletes never shrink it
// below the public minimum. An open-addressing map starts at the minimum.
// No custom hash or equality proc. An interned mmgmt_procs is kept as it is,
// any other is copied.
chmap ccol_chmap_create_compact(ccol_data_type key_type,
                                ccol_data_type val_type,
                                ccol_memmgmt_procs *mmgmt_procs, char **err);

#endif /* CCOL_CHASHINSERT_H */
