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
 * @file chashinsert.h
 * @brief INTERNAL ONLY. An insert into chashmap that hands back the entry of
 *        a key that is already present, in one hash and one probe.
 *
 * chmap_insert_elem() overwrites the value of a key that is already there.
 * A module that keeps an owning pointer as the value (the JSON and YAML
 * object parsers do) has to read the old pointer before it gets overwritten,
 * so that it can free what that pointer owns. Without this entry point that
 * would be a lookup and then an insert, two hashes and two probes of the
 * same key.
 *
 * The header also has the ordered cursor of a map, and a constructor for a
 * map that starts with fewer buckets than the public minimum, which suits a
 * module that makes lots of maps that mostly stay small.
 *
 * It's an internal header: it has no visibility block, make install leaves
 * it out, and its symbol isn't in the dynamic symbol table of the shared
 * library.
 */

#ifndef CCOL_CHASHINSERT_H
#define CCOL_CHASHINSERT_H

#include <chashmap.h>
#include <common.h>

/**
 * @brief Insert key_pair with val_pair when the key is absent, leave the map
 *        unchanged when it is present, and give the location and the size of
 *        the value of the key in both cases.
 *
 * Arguments are validated exactly as chmap_insert_elem() validates them, and
 * the key is hashed once and probed once.
 *
 * On both success codes *val_slot (a cmap_pair that belongs to the caller)
 * gets the location and the size of the value that the map holds for the
 * key, with the same contract as chmap_get_elem_ref(). So the caller may
 * overwrite the bytes that val_slot->ptr addresses within val_slot->size,
 * for example to store a pointer it builds after the call, or to replace an
 * owning pointer once it has freed what the old one owned. The pointer is
 * valid until the next operation that changes the map. Only the return value
 * tells the two cases apart.
 *
 * @param chm      The map.
 * @param key_pair The key. The same rules as chmap_insert_elem() apply.
 * @param val_pair The value to store when the key is absent. The map doesn't
 *                 read it when the key is present.
 * @param val_slot Out parameter, which must not be NULL. It is set to
 *                 {NULL, 0} on entry.
 *
 * @return ccol_success when the key was absent and the map holds it with
 *         val_pair; *val_slot describes the new entry.
 * @return ccol_key_already_present when the key was there. The map is
 *         unchanged and *val_slot describes the entry that was already
 *         there.
 * @return Any other code of chmap_insert_elem() when the insert fails
 *         (ccol_invalid_args, ccol_not_enough_memory or ccol_container_full);
 *         *val_slot stays {NULL, 0} and the map is unchanged. A NULL val_slot
 *         gives ccol_invalid_args.
 */
ccol_retval_t ccol_chmap_insert_or_get_elem(chmap chm,
                                            const cmap_pair *key_pair,
                                            const cmap_pair *val_pair,
                                            cmap_pair *val_slot);

/**
 * @brief ccol_chmap_insert_or_get_elem() that also gives the accessor of the
 *        key that the map stores for the entry.
 * It inserts, finds, validates and hashes exactly like
 * ccol_chmap_insert_or_get_elem(), with one hash and one probe. On both
 * success codes *val_slot points at the value accessor that the entry's node
 * keeps, so it is valid for exactly as long as *key_slot is, and the caller
 * may overwrite the bytes its ptr addresses within its size.
 * On both success codes *key_slot points at the accessor of the key bytes
 * that the map owns for the entry: ptr addresses the stored copy of the key
 * and size is its size. The caller must not write through ptr, because the
 * map hashes and compares those bytes.
 * A separate-chaining map keeps each entry in its own node and a resize only
 * relinks the nodes, so neither the accessor nor the key bytes behind it move
 * until the key is deleted, the map is reset or the map is destroyed.
 * Replacing the value of the key keeps both of them. A caller can hold on to
 * key_slot->ptr for that long, for example as the key of an order list it
 * keeps next to the map.
 * An open-addressing map keeps its keys inline in slots that move on every
 * resize and on a delete, and it has no accessor for a key, so this function
 * refuses such a map.
 * @param chm      The map. It must use separate chaining, which every map
 *                 with a key or a value that is not integral does.
 * @param key_pair The key. The same rules as chmap_insert_elem() apply.
 * @param val_pair The value to store when the key is absent. The map doesn't
 *                 read it when the key is present.
 * @param key_slot Out parameter, which must not be NULL. It is set to NULL
 *                 on entry.
 * @param val_slot Out parameter, which must not be NULL. It is set to NULL
 *                 on entry.
 * @return ccol_success or ccol_key_already_present, with the same meaning as
 *         for ccol_chmap_insert_or_get_elem(). Both out parameters name the
 *         entry of the key.
 * @return Any other code of chmap_insert_elem() when the insert fails. Both
 *         out parameters stay NULL and the map is unchanged. A NULL out
 *         parameter, a NULL chm and an open-addressing map give
 *         ccol_invalid_args.
 */
ccol_retval_t ccol_chmap_insert_or_get_entry(chmap chm,
                                             const cmap_pair *key_pair,
                                             const cmap_pair *val_pair,
                                             const cmap_pair **key_slot,
                                             const cmap_pair **val_slot);

/**
 * @brief An opaque reference to one entry of a separate-chaining map, used
 *        as the cursor of a walk in insertion order.
 * The reference is valid until its key is deleted, the map is reset or the
 * map is destroyed; inserts, resizes and value replacements of any key don't
 * affect it. It names the same node as the key accessor that
 * ccol_chmap_insert_or_get_entry() gives, so the key bytes it reaches are as
 * stable as that function documents.
 */
typedef struct ccol_chmap_entry_ref ccol_chmap_entry_ref;

/**
 * @brief Give the entry that was inserted first and is still live.
 * A separate-chaining map keeps all its live entries on one list in
 * insertion order. Replacing the value of a key keeps the entry where it is,
 * and a delete takes it off the list. Nothing is allocated and the call
 * can't fail.
 * @param chm The map.
 * @return The oldest entry. NULL when the map is empty, when chm is NULL, and
 *         for an open-addressing map, which keeps no insertion order.
 */
const ccol_chmap_entry_ref *ccol_chmap_oldest_entry(chmap chm);

/**
 * @brief Read the key and the value accessor of an entry, and give the entry
 *        that was inserted after it.
 * The accessors follow the contract of the key slot and the value slot of
 * ccol_chmap_insert_or_get_entry(). The successor is read at the time of the
 * call, so a caller that holds on to it may delete the key of entry and
 * carry on from the successor. Deleting any other key can free the
 * successor though.
 * @param entry A live entry, which must not be NULL.
 * @param key   Out parameter for the accessor of the stored key. It must not
 *              be NULL.
 * @param val   Out parameter for the accessor of the stored value. It must
 *              not be NULL.
 * @return The next newer entry, or NULL when entry is the newest one.
 */
const ccol_chmap_entry_ref *ccol_chmap_entry_read(
    const ccol_chmap_entry_ref *entry, const cmap_pair **key,
    const cmap_pair **val);

/**
 * Where an allocation-free walk over every entry of a map is; see
 * ccol_chmap_walk_next(). Set it to all zero before the first call.
 */
typedef struct ccol_chmap_walk {
  size_t next_slot;
  const ccol_chmap_entry_ref *next_entry;
  bool started;
} ccol_chmap_walk;

/**
 * @brief Give the next entry of a walk over every live entry of a map, of
 *        either backend.
 * A separate-chaining map is walked oldest first and an open-addressing map
 * in slot order. Nothing is allocated and the call can't fail, so a teardown
 * that has to release what the values point to can rely on it, where
 * chashmap_begin_iter() allocates and can fail. In a teardown, use this
 * walk and not the iterator. The map must not change during the walk.
 * @param chm  The map, or NULL, which walks like an empty map.
 * @param walk The position, all zero before the first call.
 * @param key  Out parameter for the stored key bytes. It must not be NULL.
 * @param val  Out parameter for the stored value bytes. It must not be NULL.
 * @return true with *key and *val set, or false once every entry was given.
 */
bool ccol_chmap_walk_next(chmap chm, ccol_chmap_walk *walk, const void **key,
                          const void **val);

/**
 * @brief Create a map whose first bucket array is smaller than the public
 *        minimum, for a module that makes lots of maps that mostly stay
 *        small.
 * The map behaves exactly like one from chmap_create_mp() with
 * CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, apart from where its bucket array
 * starts. A separate-chaining map starts with 4 buckets inside the map's own
 * block, so creating it is one allocation. It grows by the same factor and
 * at the same load as any other map, which means that after the first growth
 * it has the sizes of a map that started at the public minimum, and a delete
 * never shrinks it below that minimum. An open-addressing map starts at the
 * public minimum. You can't give a custom hashing proc or a custom key
 * equality proc.
 * @param key_type    The type of the keys.
 * @param val_type    The type of the values.
 * @param mmgmt_procs The allocator, or NULL for the default one. A pointer
 *                    that ccol_procs_intern() gave is kept as it is; any
 *                    other is copied, as chmap_create_mp() does.
 * @param err         Out parameter for an error message, or NULL.
 * @return The map, or NULL with *err set when the creation fails.
 */
chmap ccol_chmap_create_compact(ccol_data_type key_type,
                                ccol_data_type val_type,
                                ccol_memmgmt_procs_t *mmgmt_procs, char **err);

#endif /* CCOL_CHASHINSERT_H */
