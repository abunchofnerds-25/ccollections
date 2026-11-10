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

// INTERNAL ONLY. A scoped switch of the calling thread to the "C" locale, for
// number conversions. JSON and YAML always write the decimal point as '.', but
// strtod() and "%g" follow LC_NUMERIC, so under a locale whose radix is ','
// they stop at the '.' of "1.5" and write "0,25". ccol_c_locale_enter() moves
// only the calling thread with uselocale(), and ccol_c_locale_leave() puts the
// previous locale back; scopes nest.
//
// The state below has internal linkage, so each translation unit has its own
// flag and its own object and nothing is exported. The header isn't
// installed.
#ifndef CCOL_CNUMLOCALE_H
#define CCOL_CNUMLOCALE_H

#include <common.h>
#include <locale.h>
// newlocale() and uselocale() live in <xlocale.h> on macOS.
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <stdatomic.h>
// C23 has bool, true and false as keywords.
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h>
#endif

// What one ccol_c_locale_enter() changed, for the matching leave.
typedef struct {
  locale_t prev;
  bool switched;
} ccol_c_locale_scope;

// The "C" locale object, created once and lazily. The destructor clears the
// pointer, so an enter after it finds no object and leaves the locale alone.
static ccol__once_flag ccol__c_locale_once = CCOL__ONCE_INIT;
static _Atomic(locale_t) ccol__c_locale_obj;
// glibc and musl hand back a static "C" object that freelocale() doesn't free.
// Elsewhere the object is allocated and a thread can still be inside a scope
// at exit, so every scope counts itself and the destructor frees the object
// only when nobody uses it; otherwise it keeps it reachable in
// ccol__c_locale_kept. CCOL__C_LOCALE_COUNT_USERS_FOR_TESTS lets a Linux test
// build the count too.
#if !defined(__linux__) || defined(CCOL__C_LOCALE_COUNT_USERS_FOR_TESTS)
#define CCOL__C_LOCALE_COUNTS_USERS 1
static _Atomic size_t ccol__c_locale_users;
static locale_t ccol__c_locale_kept;
#endif

// Creates the "C" locale object and publishes it in ccol__c_locale_obj (NULL
// when the C library refuses). It runs once under ccol__c_locale_once, on the
// first enter.
static void ccol__c_locale_init(void) {
  atomic_store_explicit(&ccol__c_locale_obj,
                        newlocale(LC_ALL_MASK, "C", (locale_t)0),
                        memory_order_release);
}

// Frees the "C" locale object at unload or exit, so it doesn't stay allocated.
// It clears ccol__c_locale_obj first and frees the object only when no scope
// uses it; otherwise it keeps it in ccol__c_locale_kept. The clear comes before
// the load of the users count, and an enter's increment before its load of the
// pointer, all sequentially consistent, so either the enter sees no object or
// the destructor sees the user.
__attribute__((destructor)) static void ccol__c_locale_fini(void) {
#ifdef CCOL__C_LOCALE_COUNTS_USERS
  locale_t loc = atomic_exchange(&ccol__c_locale_obj, (locale_t)0);
  if (!loc) return;
  if (atomic_load(&ccol__c_locale_users) == 0)
    freelocale(loc);
  else
    ccol__c_locale_kept = loc;
#else
  locale_t loc = atomic_exchange_explicit(&ccol__c_locale_obj, (locale_t)0,
                                          memory_order_acq_rel);
  if (loc) freelocale(loc);
#endif
}

// Moves the calling thread to the "C" locale, so the number conversions of
// cjson and cyaml read and write '.' as the decimal point. Gives the scope to
// pass to the matching ccol_c_locale_leave(). Creates the object on first use
// and counts itself in ccol__c_locale_users where that count exists. When the
// object doesn't exist it switches nothing and the conversions run under the
// current locale.
static inline ccol_c_locale_scope ccol_c_locale_enter(void) {
  ccol_c_locale_scope scope = {(locale_t)0, false};
  ccol__call_once(ccol__c_locale_once, ccol__c_locale_init);
#ifdef CCOL__C_LOCALE_COUNTS_USERS
  atomic_fetch_add(&ccol__c_locale_users, 1);
  locale_t loc = atomic_load(&ccol__c_locale_obj);
  if (!loc) {
    atomic_fetch_sub(&ccol__c_locale_users, 1);
    return scope;
  }
#else
  locale_t loc =
      atomic_load_explicit(&ccol__c_locale_obj, memory_order_acquire);
  if (!loc) return scope;
#endif
  scope.prev = uselocale(loc);
  scope.switched = true;
  return scope;
}

// Puts back the locale that the matching ccol_c_locale_enter() replaced, so the
// caller's thread ends a conversion in the locale it started in. Drops the
// scope's count in ccol__c_locale_users.
static inline void ccol_c_locale_leave(ccol_c_locale_scope scope) {
  if (!scope.switched) return;
  uselocale(scope.prev);
#ifdef CCOL__C_LOCALE_COUNTS_USERS
  atomic_fetch_sub(&ccol__c_locale_users, 1);
#endif
}

#endif
