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

// Internal: A process-lifetime copy of a caller's allocator procs. Objects
// that keep an allocator pointer for their whole life store the interned
// pointer, so the caller's struct may live on a stack frame that returns, or
// be freed, while those objects are in use. Not installed, not exported.

#include <common.h>

// How many distinct procs contents the process can intern. A program has a
// handful of allocator sets at most. The copies sit in static storage, so the
// table never allocates and nothing frees it.
#define CCOL_PROCS_INTERN_CAPACITY 64

// Interns *mp: Stores in *out a pointer, valid for the life of the process, to
// a copy of it. Objects that keep an allocator pointer for their whole life
// (cjson and cyaml nodes) store this pointer, so the caller's struct may go
// away. The same four function pointers always give the same pointer, so two
// interned pointers compare by content. An interned pointer comes back as it
// is, and mp == NULL gives NULL. Gives ccol_success, or ccol_container_full
// (with *out NULL) when every slot holds another content.
// The lookup takes no lock: Published copies never change and the table fills
// from slot 0, so it reads the published prefix with acquire loads. A lookup
// that meets a slot another thread is still writing waits for it, so each
// content takes exactly one slot even when many threads intern it at once.
ccol_retval ccol_procs_intern(ccol_memmgmt_procs *mp, ccol_memmgmt_procs **out);

// Tells whether mp came from ccol_procs_intern(), so a container may keep mp
// itself instead of copying the struct into its own block. NULL and any other
// pointer give false.
bool ccol_procs_is_interned(const ccol_memmgmt_procs *mp);
