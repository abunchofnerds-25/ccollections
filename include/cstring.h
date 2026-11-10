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

#include "cvector.h"

// A heap string that grows by itself. The buffer always holds a NUL-terminated
// C string and its capacity is a power of two, 16 bytes at least. It offers the
// usual operations (append, insert, find, replace, split, ...) and takes a
// custom allocator.
//
// The maps don't know cstr as a key type; use cstring_c_str() as a char * key.
// The map copies the content, so changing or destroying the cstr later doesn't
// touch the entry.
//
// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// The string is opaque and cstr is the handle.
typedef struct cstring cstring;

typedef cstring *cstr;

// Creates a string holding a copy of initial (NULL for an empty one), with the
// allocator m_procs (NULL for malloc and free). It's the one constructor behind
// every create function and cstr_construct form. The capacity is rounded up to
// a power of two, 16 at least. Gives the handle, or NULL with *err set on
// failure. Destroy it with cstring_destroy().
cstr cstring_create_full(const char *initial, ccol_memmgmt_procs *m_procs,
                         char **err);

// Creates a string with the default allocator; the short form of
// cstring_create_full().
static inline __attribute__((always_inline)) cstr
cstring_create(const char *initial, char **err) {
  return cstring_create_full(initial, NULL, err);
}

// Creates a string with the default allocator and no error string, for callers
// that only check for NULL.
static inline __attribute__((always_inline)) cstr
cstring_new(const char *initial) {
  return cstring_create_full(initial, NULL, NULL);
}

// Gives the allocator of s, or NULL for the default one, so a caller can
// allocate related memory the same way. Asserts on a NULL s.
ccol_memmgmt_procs *cstring_get_mprocs(cstr s);

// Internal: frees the string. Use cstring_destroy() instead.
void cstring__destroy(cstr s);

// Frees the string and sets s to NULL, so a second call is harmless. s must be
// a modifiable lvalue and is evaluated once; NULL is fine.
#define cstring_destroy(s)    \
  ccol__cstring_destroy_impl( \
      s, ccol__uniq(ccol__cstring_destroy_slot, __COUNTER__))

// Internal: the body of cstring_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define ccol__cstring_destroy_impl(s, slot) \
  do {                                      \
    __typeof__(s) *slot = &(s);             \
    if (*slot) {                            \
      cstring__destroy(*slot);              \
      *slot = NULL;                         \
    }                                       \
  } while (0)

// Internal: destroys *sp and sets it to NULL. It's the cleanup function the
// _scoped forms attach with ccol__destructor.
static inline void cstring__destroy_cleanup(cstr *sp) {
  if (sp && *sp) {
    cstring__destroy(*sp);
    *sp = NULL;
  }
}

// Gives the length of s in bytes, without the '\0'. Asserts on a NULL s.
size_t cstring_length(cstr s);

// Gives the NUL-terminated content of s, for any API that takes a char *. The
// pointer is valid until the next change to s. Asserts on a NULL s.
const char *cstring_c_str(cstr s);

// Gives the byte at idx, or '\0' when idx is out of range. Asserts on a NULL s.
char cstring_at(cstr s, size_t idx);

// Says whether s has length 0. Asserts on a NULL s.
bool cstring_is_empty(cstr s);

// Appends str to s, growing the buffer when needed. It gives ccol_success,
// ccol_invalid_args for a NULL str, ccol_container_full when the length would
// overflow size_t, or ccol_not_enough_memory. Asserts on a NULL s.
ccol_retval cstring_append(cstr s, const char *str);

// Puts str in front of the content of s. Results as for cstring_append().
ccol_retval cstring_prepend(cstr s, const char *str);

// Inserts str at pos (0 to the length), moving the rest to the right. Results
// as for cstring_append(), plus ccol_invalid_args for pos past the end.
ccol_retval cstring_insert(cstr s, size_t pos, const char *str);

// Replaces the content of s with str. Results as for cstring_append();
// ccol_container_full would need a str that fills the address space.
ccol_retval cstring_set(cstr s, const char *str);

// Empties s and tries to shrink its buffer back to 16 bytes; when that realloc
// fails the capacity stays, but the length is 0 anyway. Asserts on a NULL s.
void cstring_reset(cstr s);

// Makes room for at least min_capacity bytes, the '\0' included, rounded up to
// a power of two (16 at least), so later appends don't reallocate. It never
// shrinks. Gives false when the size is too large or the realloc fails. Asserts
// on a NULL s.
bool cstring_reserve(cstr s, size_t min_capacity);

// Turns the ASCII lower-case letters of s into upper case in place. Every other
// byte stays, whatever the locale. Asserts on a NULL s.
void cstring_to_upper(cstr s);

// Turns the ASCII upper-case letters of s into lower case in place. Every other
// byte stays, whatever the locale. Asserts on a NULL s.
void cstring_to_lower(cstr s);

// Removes white space at both ends of s in place. White space is the six ASCII
// bytes space, tab, LF, VT, FF and CR, whatever the locale, so a byte above
// 0x7F (a piece of UTF-8) is never cut. Asserts on a NULL s.
void cstring_trim(cstr s);

// Replaces every non-overlapping needle in s with replacement. The result is
// built in a new buffer, so a replacement that contains needle is fine. It
// gives ccol_success (also when needle isn't found), ccol_invalid_args for a
// NULL or empty needle or a NULL replacement, ccol_container_full on a length
// overflow, or ccol_not_enough_memory. Asserts on a NULL s.
ccol_retval cstring_replace(cstr s, const char *needle,
                            const char *replacement);

// Compares s with str as strcmp() does; a NULL str gives 1. Asserts on a NULL
// s.
int cstring_compare(cstr s, const char *str);

// Says whether the content of s equals str. Asserts on a NULL s.
bool cstring_equals(cstr s, const char *str);

// Says whether s starts with prefix. An empty prefix matches, a NULL one
// doesn't. Asserts on a NULL s.
bool cstring_starts_with(cstr s, const char *prefix);

// Says whether s ends with suffix. An empty suffix matches, a NULL one doesn't.
// Asserts on a NULL s.
bool cstring_ends_with(cstr s, const char *suffix);

// Gives the index of the first needle in s, 0 for an empty needle, or
// ccol_invalid_size when it isn't there or needle is NULL. Asserts on a NULL s.
size_t cstring_find(cstr s, const char *needle);

// Gives the index of the last needle in s (overlaps count: the last "aba" in
// "ababa" is at 2), the length of s for an empty needle, or ccol_invalid_size
// when it isn't there or needle is NULL. From 256 bytes up the search runs
// backwards in time linear in both lengths. A needle longer than 128 bytes may
// take a temporary table of one size_t per byte from the allocator of s; if
// that's refused the answer is the same but the time can be the product of the
// lengths. Asserts on a NULL s.
size_t cstring_rfind(cstr s, const char *needle);

// Creates a string holding the bytes [start, start + length) of s, clamped to
// its end; a start at or past the end gives an empty string. Gives NULL with
// *err set when the allocation fails. You destroy the result. Asserts on a NULL
// s.
cstr cstring_substring(cstr s, size_t start, size_t length, char **err);

// Creates an independent copy of s, or NULL with *err set on failure. You
// destroy the result. Asserts on a NULL s.
cstr cstring_copy(cstr s, char **err);

// Splits s at every delimiter (not NULL or empty) into a cvec of new cstr, with
// empty tokens at the ends kept. Gives NULL with *err set on failure. You
// destroy every cstr and then the vector. Asserts on a NULL s.
cvec cstring_split(cstr s, const char *delimiter, char **err);

// Declares an uninitialized cstr named s.
#define cstr_declare(s) cstr s

// Declares a cstr named s, starting as NULL, that is destroyed at the end of
// its scope.
#define cstr_declare_scoped(s) \
  cstr s ccol__destructor(cstring__destroy_cleanup) = NULL

// cstr_init() and cstr_init_mp() create a string for a declared s, with the
// default allocator or with mprocs, and stop the program when that fails. s is
// assigned once, after the creation.
#define cstr_init(s, initial)                                                \
  do {                                                                       \
    char *cstr__init_err = NULL;                                             \
    __typeof__(s) cstr__init_s = cstring_create((initial), &cstr__init_err); \
    if (!cstr__init_s) {                                                     \
      ccol__fatal_err("cstr_init('%s'): %s", #s,                             \
                      cstr__init_err ? cstr__init_err : "unknown error");    \
    }                                                                        \
    (s) = cstr__init_s;                                                      \
  } while (0)

#define cstr_init_mp(s, initial, mprocs)                              \
  do {                                                                \
    char *cstr__init_mp_err = NULL;                                   \
    __typeof__(s) cstr__init_mp_s =                                   \
        cstring_create_full((initial), (mprocs), &cstr__init_mp_err); \
    if (!cstr__init_mp_s) {                                           \
      ccol__fatal_err(                                                \
          "cstr_init_mp('%s'): %s", #s,                               \
          cstr__init_mp_err ? cstr__init_mp_err : "unknown error");   \
    }                                                                 \
    (s) = cstr__init_mp_s;                                            \
  } while (0)

// The cstr_construct forms declare and create s in one step; _mp takes an
// allocator and _scoped destroys s at the end of its scope. Each stops the
// program on failure.
#define cstr_construct(s, initial) \
  cstr_declare(s);                 \
  cstr_init(s, initial)

#define cstr_construct_scoped(s, initial) \
  cstr_declare_scoped(s);                 \
  cstr_init(s, initial)

#define cstr_construct_mp(s, initial, mprocs) \
  cstr_declare(s);                            \
  cstr_init_mp(s, initial, mprocs)

#define cstr_construct_mp_scoped(s, initial, mprocs) \
  cstr_declare_scoped(s);                            \
  cstr_init_mp(s, initial, mprocs)

// Destroys s and sets it to NULL; the typed name of cstring_destroy().
#define cstr_destroy(s) cstring_destroy((s))

// cstr_append(), cstr_prepend(), cstr_insert(), cstr_set() and cstr_replace()
// call the cstring function of the same name and stop the program when it
// fails. Each local has a name that only its macro uses: a caller argument of
// the same name would bind to the uninitialized local, and for the size_t pos
// of cstr_insert that compiles without a warning.
#define cstr_append(s, str)                                                \
  do {                                                                     \
    ccol_retval cstr__append_r = cstring_append((s), (str));               \
    if (cstr__append_r != ccol_success) {                                  \
      ccol__fatal_err("cstr_append('%s'): r: %d (%s)", #s, cstr__append_r, \
                      ccol_retval_to_str(cstr__append_r));                 \
    }                                                                      \
  } while (0)

#define cstr_prepend(s, str)                                                 \
  do {                                                                       \
    ccol_retval cstr__prepend_r = cstring_prepend((s), (str));               \
    if (cstr__prepend_r != ccol_success) {                                   \
      ccol__fatal_err("cstr_prepend('%s'): r: %d (%s)", #s, cstr__prepend_r, \
                      ccol_retval_to_str(cstr__prepend_r));                  \
    }                                                                        \
  } while (0)

#define cstr_insert(s, pos, str)                                           \
  do {                                                                     \
    ccol_retval cstr__insert_r = cstring_insert((s), (pos), (str));        \
    if (cstr__insert_r != ccol_success) {                                  \
      ccol__fatal_err("cstr_insert('%s'): r: %d (%s)", #s, cstr__insert_r, \
                      ccol_retval_to_str(cstr__insert_r));                 \
    }                                                                      \
  } while (0)

#define cstr_set(s, str)                                             \
  do {                                                               \
    ccol_retval cstr__set_r = cstring_set((s), (str));               \
    if (cstr__set_r != ccol_success) {                               \
      ccol__fatal_err("cstr_set('%s'): r: %d (%s)", #s, cstr__set_r, \
                      ccol_retval_to_str(cstr__set_r));              \
    }                                                                \
  } while (0)

#define cstr_replace(s, needle, replacement)                                 \
  do {                                                                       \
    ccol_retval cstr__replace_r =                                            \
        cstring_replace((s), (needle), (replacement));                       \
    if (cstr__replace_r != ccol_success) {                                   \
      ccol__fatal_err("cstr_replace('%s'): r: %d (%s)", #s, cstr__replace_r, \
                      ccol_retval_to_str(cstr__replace_r));                  \
    }                                                                        \
  } while (0)

// Reserves at least cap bytes in s and stops the program when that fails. cap
// is evaluated once, so the message shows the value really used.
#define cstr_reserve(s, cap)        \
  ccol__cstr_reserve_impl(s, (cap), \
                          ccol__uniq(ccol__cstr_reserve_cap, __COUNTER__))

// Internal: the body of cstr_reserve(), with a ccol__uniq() temporary so it can
// nest in any public macro.
#define ccol__cstr_reserve_impl(s, cap, cstr__reserve_cap)                    \
  do {                                                                        \
    size_t cstr__reserve_cap = (cap);                                         \
    if (!cstring_reserve((s), cstr__reserve_cap)) {                           \
      ccol__fatal_err(                                                        \
          "cstr_reserve('%s'): failed to reserve %zu bytes (out of memory?)", \
          #s, cstr__reserve_cap);                                             \
    }                                                                         \
  } while (0)

// Evaluates to a new string holding [start, start + len) of s and stops the
// program when the creation fails. Each argument is evaluated once.
#define cstr_substring(s, start, len)                                         \
  ccol__cstr_substring_impl(                                                  \
      s, (start), (len), ccol__uniq(ccol__cstr_substring_start, __COUNTER__), \
      ccol__uniq(ccol__cstr_substring_len, __COUNTER__),                      \
      ccol__uniq(ccol__cstr_substring_err, __COUNTER__),                      \
      ccol__uniq(ccol__cstr_substring_result, __COUNTER__))

// Internal: the body of cstr_substring(), with ccol__uniq() temporaries so it
// can nest in any public macro.
#define ccol__cstr_substring_impl(s, start, len, cstr__substring_start,     \
                                  cstr__substring_len, cstr__substring_err, \
                                  cstr__substring_result)                   \
  ({                                                                        \
    size_t cstr__substring_start = (start);                                 \
    size_t cstr__substring_len = (len);                                     \
    char *cstr__substring_err = NULL;                                       \
    __auto_type cstr__substring_result =                                    \
        cstring_substring((s), cstr__substring_start, cstr__substring_len,  \
                          &cstr__substring_err);                            \
    if (!cstr__substring_result) {                                          \
      ccol__fatal_err(                                                      \
          "cstr_substring('%s', start=%zu, len=%zu): %s", #s,               \
          cstr__substring_start, cstr__substring_len,                       \
          cstr__substring_err ? cstr__substring_err : "unknown error");     \
    }                                                                       \
    cstr__substring_result;                                                 \
  })

// Short names for the cstring functions, for code that uses the cstr macros.
// Each evaluates to what its function gives.
#define cstr_length(s) cstring_length((s))
#define cstr_c_str(s) cstring_c_str((s))
#define cstr_at(s, idx) cstring_at((s), (idx))
#define cstr_is_empty(s) cstring_is_empty((s))
#define cstr_reset(s) cstring_reset((s))
#define cstr_to_upper(s) cstring_to_upper((s))
#define cstr_to_lower(s) cstring_to_lower((s))
#define cstr_trim(s) cstring_trim((s))
#define cstr_compare(s, str) cstring_compare((s), (str))
#define cstr_equals(s, str) cstring_equals((s), (str))
#define cstr_starts_with(s, pfx) cstring_starts_with((s), (pfx))
#define cstr_ends_with(s, sfx) cstring_ends_with((s), (sfx))
#define cstr_find(s, needle) cstring_find((s), (needle))
#define cstr_rfind(s, needle) cstring_rfind((s), (needle))
#define cstr_copy(s, err) cstring_copy((s), (err))
#define cstr_split(s, delim, err) cstring_split((s), (delim), (err))

#pragma GCC visibility pop
