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
 * Some modules have objects that keep an allocator pointer for their whole
 * life and can't afford a copy of the procs struct in every object. Those
 * store the pointer that ccol_procs_intern() gives back, not the caller's
 * pointer, so the caller's struct is free to live on the stack of a helper
 * that returns, or to be freed, while the objects built with it are in use.
 *
 * None of this is public interface. The library never installs this header
 * and it has no visibility block.
 */

#include <common.h>

/**
 * How many distinct procs contents the process can intern. A procs struct is
 * four function pointers and nothing more, so the number of distinct
 * contents can't exceed the allocator function sets the program has, which
 * is a handful in any realistic program. Every interned copy sits in static
 * storage for the life of the process, so the table never allocates and
 * nothing ever frees it.
 */
#define CCOL_PROCS_INTERN_CAPACITY 64

/**
 * Gives back a pointer, valid for the life of the process, to a copy of the
 * struct that mp points at. Two calls with the same four function pointers
 * get the same pointer back whichever struct object they came from, so
 * comparing two interned pointers compares their contents. A pointer that
 * this function gave back is accepted as input and returned unchanged.
 * mp == NULL gives NULL, which selects the default allocator everywhere.
 *
 * The lookup takes no lock. A published copy never changes and the table
 * fills up from its first slot, so a lookup reads the published prefix with
 * acquire loads and stops at the first empty slot, and only the first use of
 * a new content writes anything. When a lookup meets a slot that another
 * thread is still writing it waits for that copy before moving on. That way
 * each content takes exactly one slot even when many threads use it for the
 * first time at the same moment, and the table holds
 * CCOL_PROCS_INTERN_CAPACITY distinct contents.
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
 * is valid for the life of the process, so an object that keeps an allocator
 * pointer can keep mp itself rather than a copy of the struct. NULL and any
 * pointer outside the table give false.
 */
bool ccol_procs_is_interned(const ccol_memmgmt_procs_t *mp);
