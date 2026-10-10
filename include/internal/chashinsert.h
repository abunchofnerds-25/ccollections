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
 * chmap_insert_elem() overwrites the value of a key that is already present.
 * A module that stores an owning pointer as the value, such as the JSON and
 * YAML object parsers, must read the old pointer before it is overwritten so
 * that it can free what the pointer owns. Without this entry point, that
 * takes a lookup followed by an insert, which costs two hashes and two probes
 * of the same key.
 *
 * The header also gives the ordered cursor of a map, and a constructor of a
 * map that starts with fewer buckets than the public minimum, for a module
 * that makes many maps that usually stay small.
 *
 * This header is internal: it carries no visibility block, make install
 * excludes it, and its symbol is absent from the dynamic symbol table of the
 * shared library.
 */

#ifndef CCOL_CHASHINSERT_H
#define CCOL_CHASHINSERT_H

#include <chashmap.h>
#include <common.h>

/**
 * @brief Insert key_pair with val_pair when the key is absent, leave the map
 *        unchanged when it is present, and give the value accessor of the
 *        entry of the key in both cases.
 *
 * The function validates its arguments exactly as chmap_insert_elem() does,
 * and makes one hash of the key and one probe for it.
 *
 * On both success codes *val_slot points at the accessor of the value that
 * the map now stores for the key, with the same contract as
 * chmap_get_elem_ref(): the caller may read ptr and size, and may overwrite
 * the bytes that ptr addresses within size, for example to store a pointer
 * that it builds after the call, or to replace an owning pointer after it
 * frees what the old one owned. The accessor stays valid until the next
 * operation that changes the map. Only the return value tells the two cases
 * apart.
 *
 * @param chm      The map.
 * @param key_pair The key. The same rules as chmap_insert_elem() apply.
 * @param val_pair The value to store when the key is absent. The map does
 *                 not read it when the key is present.
 * @param val_slot Out parameter, which must not be NULL. The function sets
 *                 it to NULL on entry.
 *
 * @return ccol_success when the key was absent and the map now holds it with
 *         val_pair; *val_slot names the new entry.
 * @return ccol_key_already_present when the key was present. The map is
 *         unchanged, and *val_slot names the entry that was already there.
 * @return Any other code of chmap_insert_elem() when the insert fails
 *         (ccol_invalid_args, ccol_not_enough_memory or ccol_container_full);
 *         *val_slot stays NULL and the map is unchanged. A val_slot of NULL
 *         gives ccol_invalid_args.
 */
ccol_retval_t ccol_chmap_insert_or_get_elem(chmap chm,
                                            const cmap_pair *key_pair,
                                            const cmap_pair *val_pair,
                                            const cmap_pair **val_slot);

/**
 * @brief ccol_chmap_insert_or_get_elem() that also gives the accessor of the
 *        key that the map stores for the entry.
 * The function inserts, finds, validates and hashes exactly as
 * ccol_chmap_insert_or_get_elem() does, in one hash and one probe, and sets
 * *val_slot with the same contract.
 * On both success codes *key_slot points at the accessor of the key bytes
 * that the map owns for the entry: ptr addresses the stored copy of the key,
 * and size is its size. The caller must not write through ptr, because the
 * map hashes and compares those bytes.
 * A separate-chaining map stores each entry in a node of its own, and a
 * resize only relinks those nodes, so neither the accessor nor the key bytes
 * that it addresses move until the key is deleted, the map is reset or the
 * map is destroyed. A replacement of the value of the key keeps both. A
 * caller may keep key_slot->ptr for that long, for example as the key of an
 * order list that it maintains beside the map.
 * An open-addressing map keeps its keys inline in slots that move on every
 * resize and on a delete, and it keeps no accessor for a key, so the
 * function refuses such a map.
 * @param chm      The map. It must use separate chaining, which every map
 *                 with a key or a value that is not integral does.
 * @param key_pair The key. The same rules as chmap_insert_elem() apply.
 * @param val_pair The value to store when the key is absent. The map does
 *                 not read it when the key is present.
 * @param key_slot Out parameter, which must not be NULL. The function sets
 *                 it to NULL on entry.
 * @param val_slot Out parameter, which must not be NULL. The function sets
 *                 it to NULL on entry.
 * @return ccol_success or ccol_key_already_present, with the meaning that
 *         they have for ccol_chmap_insert_or_get_elem(). Both out parameters
 *         name the entry of the key.
 * @return Any other code of chmap_insert_elem() when the insert fails. Both
 *         out parameters stay NULL and the map is unchanged. A NULL out
 *         parameter, a NULL chm, and an open-addressing map give
 *         ccol_invalid_args.
 */
ccol_retval_t ccol_chmap_insert_or_get_entry(chmap chm,
                                             const cmap_pair *key_pair,
                                             const cmap_pair *val_pair,
                                             const cmap_pair **key_slot,
                                             const cmap_pair **val_slot);

/**
 * @brief An opaque reference to one entry of a separate-chaining map, which
 *        serves as the cursor of an iteration in insertion order.
 * A reference stays valid until its key is deleted, the map is reset or the
 * map is destroyed; an insert, a resize and a replacement of the value of
 * any key leave it valid. It names the same node as the key accessor that
 * ccol_chmap_insert_or_get_entry() gives, so the key bytes that it reaches
 * have the stability that that function documents.
 */
typedef struct ccol_chmap_entry_ref ccol_chmap_entry_ref;

/**
 * @brief Give the entry that was inserted first and is still live.
 * A separate-chaining map keeps every live entry on one list in insertion
 * order: a replacement of the value of a key keeps the place of its entry,
 * and a delete takes the entry out. The function allocates nothing and
 * cannot fail.
 * @param chm The map.
 * @return The oldest entry. NULL when the map is empty, when chm is NULL, and
 *         for an open-addressing map, which keeps no insertion order.
 */
const ccol_chmap_entry_ref *ccol_chmap_oldest_entry(chmap chm);

/**
 * @brief Read the key and the value accessor of an entry, and give the entry
 *        that was inserted after it.
 * The accessors have the contract of the key slot and the value slot of
 * ccol_chmap_insert_or_get_entry(). The function reads the successor at the
 * time of the call, so a caller that keeps that successor may delete the key
 * of entry and still continue from the successor. A delete of any other key
 * can free the successor.
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
 * @brief Create a map whose first bucket array is smaller than the public
 *        minimum, for a module that makes many maps that usually stay small.
 * The map behaves exactly as one from chmap_create_mp() with
 * CCOL_DEFAULT_INITIAL_BUCKET_ARRAY_SIZE, except for where its bucket array
 * starts. A separate-chaining map starts with 4 buckets inside the block of
 * the map, so the creation is one allocation. It grows by the same factor
 * and at the same load as any other map, so after its first growth it has
 * the sizes of a map that started at the public minimum, and a delete never
 * shrinks it below that minimum. An open-addressing map starts at the public
 * minimum. Neither a custom hashing proc nor a custom key equality proc can
 * be given.
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
