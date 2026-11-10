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

// Internal: a lock-free index from a handle to a pointer, with per-slot pins.
// It's the hot half of the generation-tagged slot tables of clogger,
// clrucache, cthreadpool and chttpclient: a public call resolves its handle
// here and keeps the object alive against a concurrent destroy. A resolve does
// plain acquire loads and a pin writes only the calling thread's stripe, so
// the call gets faster with more threads instead of bouncing a shared lock and
// counter between cores. The owning module keeps its own slot table for the
// cold work; this table holds only what a resolve needs, and its writers hold
// the module's writer lock.
//
// A handle is (index << 32) | generation. The generation is never 0, so the
// all-zero handle is always invalid.
#ifndef CCOL_CPINTABLE_H
#define CCOL_CPINTABLE_H

#include <common.h>
#include <stdatomic.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
// bool, true and false are keywords in C23.
#include <stdbool.h>
#endif
#include <stddef.h>
#include <stdint.h>

// Slots per chunk. A chunk is never moved or freed, so a reader can index it
// without a lock.
#define CCOL_PIN_CHUNK_SLOTS 64u

// Chunk pointers in the directory; also the ceiling on live indices.
#define CCOL_PIN_MAX_CHUNKS 1024u

// How many indices the table can hold. The owning module must refuse to claim
// an index at or above it: such an index can never publish, and if it reached
// the module's free list every later acquire would pop it again and fail.
#define CCOL_PIN_MAX_SLOTS ((size_t)CCOL_PIN_MAX_CHUNKS * CCOL_PIN_CHUNK_SLOTS)

// Pin counters per slot, each on its own cache line. A thread pins on the
// stripe its id selects, so concurrent pins of one object don't contend.
#define CCOL_PIN_STRIPES 8u

// The cache line size we assume. The stride between two counts keeps them on
// different lines, whatever the alignment of the calloc'd block that holds
// them; only unread padding crosses a line boundary.
#define CCOL_PIN_LINE 64u

// One pin counter, padded to a cache line. It's signed because a stripe goes
// negative when a release lands on another stripe than its pin, and only the
// sum over the stripes means anything. That keeps the sum of a split count
// right, but a pin must still be released by the thread that took it (see
// ccol_pintable_unpin()).
typedef struct {
  _Atomic ptrdiff_t count;
  char pad[CCOL_PIN_LINE - sizeof(_Atomic ptrdiff_t)];
} ccol_pin_stripe;

// One entry of the table: what a resolve reads for one index.
typedef struct {
  // (generation << 1) | live, in one word so one acquire load reads both.
  _Atomic uint64_t state;
  // Written before the slot goes live and never while it's live.
  void *_Atomic ptr;
  // Allocated by the first publish into the slot and kept for the life of the
  // process, because a reader may hold this pointer.
  ccol_pin_stripe *_Atomic stripes;
} ccol_pin_slot;

// A fixed block of slots, allocated once and never moved.
typedef struct {
  ccol_pin_slot slots[CCOL_PIN_CHUNK_SLOTS];
} ccol_pin_chunk;

// The table: a directory of chunk pointers, filled in on first use.
typedef struct {
  ccol_pin_chunk *_Atomic chunks[CCOL_PIN_MAX_CHUNKS];
} ccol_pintable;

// Publishes ptr at idx under generation gen (never 0), so a resolve can find
// it. The owning module calls it when it acquires a handle, with its writer
// lock held, where it marks its slot in use. The chunk and the stripe block
// are allocated on first use. Gives false, and publishes nothing, when idx is
// above the ceiling or an allocation fails.
bool ccol_pintable_publish(ccol_pintable *t, uint32_t idx, uint32_t gen,
                           void *ptr);

// Stops new resolves from finding the object at idx; a destroy calls it with
// the module's writer lock held, where it clears its in-use flag. Pins already
// granted stay, so wait for ccol_pintable_pins() to reach 0 before the free.
void ccol_pintable_retire(ccol_pintable *t, uint32_t idx);

// Resolves a handle and pins the object, so a public call can use it while a
// concurrent destroy waits. It's lock-free and writes nothing another thread
// reads. Gives the published pointer, or NULL (with no pin taken, so no unpin)
// when the handle is 0, out of range, retired or on another generation.
void *ccol_pintable_pin(ccol_pintable *t, uint64_t handle);

// Releases a pin from ccol_pintable_pin() when the call ends, with the same
// handle and on the same thread. ccol_pintable_pins() reads the stripes one by
// one, so a pin released on another thread can show a sum of 0 while the
// caller still holds it, and a destroy would free the object under that
// caller. A zero handle does nothing.
void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle);

// Gives the pins outstanding against idx, summed over the stripes. A destroy
// that retired the index waits for 0; with the retire that means no pin is
// held and no new one can appear, because each pin is released on the thread
// that took it.
size_t ccol_pintable_pins(ccol_pintable *t, uint32_t idx);

// ccol_pintable_pins() for the index of a handle, for owners that keep a
// handle; 0 for a zero handle.
size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle);

// Frees every chunk and stripe block of t, so a leak checker doesn't report
// the storage at exit. Call it only from process teardown, when no call into
// the owning module can be in flight. Each chunk is unpublished before it's
// freed, so a later resolve answers NULL.
void ccol_pintable_dispose(ccol_pintable *t);

// Drops every pin against idx. Only for a fork() child handler: a pin held by
// a thread that didn't survive the fork can never be released, and a destroy
// would wait for it forever.
void ccol_pintable_reset(ccol_pintable *t, uint32_t idx);

// ccol_pintable_reset() for the index of a handle. A zero handle does nothing,
// since slot 0 is an ordinary slot that holds some other object.
void ccol_pintable_reset_for(ccol_pintable *t, uint64_t handle);

#endif
