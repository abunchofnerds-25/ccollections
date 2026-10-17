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
 * decimal point of a number as an ASCII '.' whatever the locale of the host
 * process is, but strtod() and the "%g" conversions of the printf family
 * follow the radix character of the active LC_NUMERIC category (see strtod(3)
 * and printf(3)). So once a process calls setlocale() or uselocale() and
 * moves to a locale whose decimal point is ',', strtod() stops at the '.' of
 * "1.5" without reporting any error, and "%g" writes "0,25", which no parser
 * of either format reads as a number.
 *
 * ccol_c_locale_enter() moves the calling thread to the "C" locale with
 * uselocale(), and ccol_c_locale_leave() puts back whatever locale was active
 * just before. Unlike setlocale(), uselocale() changes the locale of one
 * thread only and never what another thread sees. Scopes nest, since each
 * enter saves its own predecessor, so a recursive or nested scope restores
 * correctly.
 *
 * We create the "C" locale_t object once, lazily, under a _ccol_once_flag_t.
 * The state below has internal linkage, so every translation unit that
 * includes this header gets its own flag and its own object, and the helper
 * stays out of the exported symbol set and out of the namespace of the static
 * archive. A destructor frees the object at process exit. It clears the
 * published pointer first, so an enter that runs after it (from the
 * destructor of another shared object, for example) finds no object and
 * leaves the thread's locale alone instead of reading freed memory.
 * glibc and musl both hand back their built-in, static "C" object for this
 * request, and freelocale() on it frees nothing. Every other C library
 * allocates the object, and another thread can still be inside a scope when
 * the process exits, so there every scope counts itself in a users count and
 * the destructor frees the object only when that count is zero. The increment
 * of an enter comes before its load of the pointer and the destructor's clear
 * comes before its load of the count, all sequentially consistent, so either
 * the enter sees no object or the destructor sees the user. An object that a
 * scope still uses at exit stays allocated.
 *
 * If the "C" object can't be created at all (a broken locale installation in
 * the C library), enter reports that it switched nothing and the conversions
 * run under whatever locale is already active.
 *
 * The header is internal, so it has no visibility block and make install
 * leaves it out.
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

static _ccol_once_flag_t _ccol_c_locale_once = _CCOL_ONCE_INIT;
static _Atomic(locale_t) _ccol_c_locale_obj;
/* _CCOL_C_LOCALE_COUNT_USERS_FOR_TESTS lets a Linux test build the count
 * too. */
#if !defined(__linux__) || defined(_CCOL_C_LOCALE_COUNT_USERS_FOR_TESTS)
#define _CCOL_C_LOCALE_COUNTS_USERS 1
static _Atomic size_t _ccol_c_locale_users;
/* The object that the destructor couldn't free, kept reachable here. */
static locale_t _ccol_c_locale_kept;
#endif

static void _ccol_c_locale_init(void) {
  atomic_store_explicit(&_ccol_c_locale_obj,
                        newlocale(LC_ALL_MASK, "C", (locale_t)0),
                        memory_order_release);
}

__attribute__((destructor)) static void _ccol_c_locale_fini(void) {
#ifdef _CCOL_C_LOCALE_COUNTS_USERS
  locale_t loc = atomic_exchange(&_ccol_c_locale_obj, (locale_t)0);
  if (!loc) return;
  if (atomic_load(&_ccol_c_locale_users) == 0)
    freelocale(loc);
  else
    _ccol_c_locale_kept = loc;
#else
  locale_t loc = atomic_exchange_explicit(&_ccol_c_locale_obj, (locale_t)0,
                                          memory_order_acq_rel);
  if (loc) freelocale(loc);
#endif
}

/* Moves the calling thread to the "C" locale until the matching
 * ccol_c_locale_leave(). */
static inline ccol_c_locale_scope_t ccol_c_locale_enter(void) {
  ccol_c_locale_scope_t scope = {(locale_t)0, false};
  _ccol_call_once(_ccol_c_locale_once, _ccol_c_locale_init);
#ifdef _CCOL_C_LOCALE_COUNTS_USERS
  atomic_fetch_add(&_ccol_c_locale_users, 1);
  locale_t loc = atomic_load(&_ccol_c_locale_obj);
  if (!loc) {
    atomic_fetch_sub(&_ccol_c_locale_users, 1);
    return scope;
  }
#else
  locale_t loc =
      atomic_load_explicit(&_ccol_c_locale_obj, memory_order_acquire);
  if (!loc) return scope;
#endif
  scope.prev = uselocale(loc);
  scope.switched = true;
  return scope;
}

/* Puts back the locale that the matching ccol_c_locale_enter() replaced. */
static inline void ccol_c_locale_leave(ccol_c_locale_scope_t scope) {
  if (!scope.switched) return;
  uselocale(scope.prev);
#ifdef _CCOL_C_LOCALE_COUNTS_USERS
  atomic_fetch_sub(&_ccol_c_locale_users, 1);
#endif
}

#endif /* CCOL_CNUMLOCALE_H */
