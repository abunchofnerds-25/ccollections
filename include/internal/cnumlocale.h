/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

/**
 * @file cnumlocale.h
 * @brief INTERNAL ONLY. A scoped switch of the calling thread to the "C"
 *        locale, for number conversions that must not follow LC_NUMERIC.
 *
 * JSON (RFC 8259 sec. 6) and YAML 1.2 (the core schema) both write the
 * decimal point of a number as the ASCII '.', whatever the locale of the host
 * process is. strtod() and the "%g" conversions of the printf family follow
 * the radix character of the active LC_NUMERIC category instead; read
 * strtod(3) and printf(3). A process that calls setlocale() or uselocale()
 * and moves to a locale with ',' as its decimal point therefore makes
 * strtod() stop at the '.' of "1.5" and report no error, and makes "%g"
 * write "0,25", which no parser of either format reads as a number.
 *
 * ccol_c_locale_enter() moves the calling thread to the "C" locale with
 * uselocale(), and ccol_c_locale_leave() puts back the locale that was
 * active immediately before. uselocale() changes the locale of one thread
 * only. Unlike setlocale(), it never changes what another thread sees. The
 * scope nests: each enter saves its own predecessor, so a recursive or
 * nested scope restores correctly.
 *
 * The "C" locale_t object is created once, lazily, under a ccol_once_flag_t.
 * Every translation unit that includes this header holds its own flag and
 * its own object, because the state below has internal linkage. That keeps
 * the helper out of the exported symbol set and out of the static archive
 * namespace. A destructor frees the object at the exit of the process. It
 * clears the published pointer first, so an enter that runs after it (from
 * the destructor of another shared object, for example) finds no object and
 * leaves the locale of the thread alone instead of reading freed memory.
 * glibc and musl both give back their built-in, static "C" object for this
 * request, and freelocale() on it frees nothing.
 *
 * When the "C" object cannot be created at all (a broken locale installation
 * in the C library), enter reports that it switched nothing, and the
 * conversions run under whatever locale is already active.
 *
 * This header is internal. It carries no visibility block. make install does
 * not install it.
 */

#ifndef CCOL_CNUMLOCALE_H
#define CCOL_CNUMLOCALE_H

#include <common.h>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h> /* newlocale() and uselocale() */
#endif
#include <stdatomic.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif

/* What one ccol_c_locale_enter() changed, for the matching leave. */
typedef struct {
  locale_t prev;
  bool switched;
} ccol_c_locale_scope_t;

static ccol_once_flag_t _ccol_c_locale_once = CCOL_ONCE_INIT;
static _Atomic(locale_t) _ccol_c_locale_obj;

static void _ccol_c_locale_init(void) {
  atomic_store_explicit(&_ccol_c_locale_obj,
                        newlocale(LC_ALL_MASK, "C", (locale_t)0),
                        memory_order_release);
}

__attribute__((destructor)) static void _ccol_c_locale_fini(void) {
  locale_t loc = atomic_exchange_explicit(&_ccol_c_locale_obj, (locale_t)0,
                                          memory_order_acq_rel);
  if (loc) freelocale(loc);
}

/* Move the calling thread to the "C" locale until the matching
 * ccol_c_locale_leave(). */
static inline ccol_c_locale_scope_t ccol_c_locale_enter(void) {
  ccol_c_locale_scope_t scope = {(locale_t)0, false};
  ccol_call_once(_ccol_c_locale_once, _ccol_c_locale_init);
  locale_t loc =
      atomic_load_explicit(&_ccol_c_locale_obj, memory_order_acquire);
  if (loc) {
    scope.prev = uselocale(loc);
    scope.switched = true;
  }
  return scope;
}

/* Put back the locale that the matching ccol_c_locale_enter() replaced. */
static inline void ccol_c_locale_leave(ccol_c_locale_scope_t scope) {
  if (scope.switched) uselocale(scope.prev);
}

#endif /* CCOL_CNUMLOCALE_H */
