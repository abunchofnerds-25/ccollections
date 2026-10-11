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

#pragma once

/**
 * @file cprocsintern.h
 * @brief INTERNAL ONLY. A process-lifetime copy of a caller's allocator procs.
 *
 * A module whose objects keep an allocator pointer for their whole life, and
 * that cannot afford a copy of the procs struct in each object, stores the
 * pointer that ccol_procs_intern() gives back instead of the pointer of the
 * caller. The caller's struct can then live on the stack of a helper that
 * returns, or be freed, while the objects built with it are still in use.
 *
 * Nothing here is part of the public interface. The library never installs
 * this header, and the header carries no visibility block.
 */

#include <common.h>

/**
 * The number of distinct procs contents that the process can intern. A procs
 * struct holds four function pointers and nothing else, so the number of
 * distinct contents is bounded by the allocator function sets that the
 * program contains, which is a handful in any realistic program. Every
 * interned copy lives in static storage for the life of the process, so the
 * table needs no allocation and nothing ever frees it.
 */
#define CCOL_PROCS_INTERN_CAPACITY 64

/**
 * Gives back a pointer, valid for the life of the process, to a copy of the
 * struct that mp points at. Two calls with the same four function pointers
 * give back the same pointer, whichever struct object they came from, so
 * a pointer comparison of two interned values is a content comparison. A
 * pointer that this function gave back is accepted as input and returned
 * unchanged. mp == NULL gives NULL, which selects the default allocator
 * everywhere.
 *
 * The lookup takes no lock: a published copy never changes, and the table
 * fills from its first slot, so a lookup reads the published prefix with
 * acquire loads and stops at the first empty slot. Only the first use of a
 * new content writes. Two threads that make the first use of one content at
 * the same moment can each take a slot for it, which costs one slot and
 * nothing else, because each copy is complete and correct.
 *
 * @param mp  The caller's procs, or NULL.
 * @param out Receives the interned pointer, or NULL for mp == NULL.
 * @return ccol_success, or ccol_container_full when every slot holds another
 *         content. *out is NULL on that failure.
 */
ccol_retval_t ccol_procs_intern(ccol_memmgmt_procs_t *mp,
                                ccol_memmgmt_procs_t **out);

/**
 * Gives true when mp is a pointer that ccol_procs_intern() gave back, which
 * stays valid for the life of the process. An object that keeps an allocator
 * pointer can then keep mp itself instead of a copy of the struct. NULL and
 * any pointer outside the table give false.
 */
bool ccol_procs_is_interned(const ccol_memmgmt_procs_t *mp);

#ifdef RUNNING_UNIT_TESTS
/* The number of slots that hold a copy. */
size_t _ccol_procs_intern_used_for_tests(void);
/* Empties every slot from index keep onward. A test calls it only while no
 * object holds a pointer into those slots. */
void _ccol_procs_intern_truncate_for_tests(size_t keep);
#endif /* RUNNING_UNIT_TESTS */
