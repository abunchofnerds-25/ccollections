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

// The string type. It's opaque; code holds a cstr.
typedef struct cstring cstring;

// The handle of a growable, NUL-terminated string, a pointer to an opaque
// cstring. Make one with cstr_construct() or cstring_create() and free it with
// cstr_destroy() or cstring_destroy().
typedef cstring *cstr;

// Creates a string holding a copy of initial (NULL for an empty one), with
// your own allocator. m_procs is the allocator table, or NULL for malloc and
// free; all four of its functions must be set, and the string keeps its own
// copy of the table. The capacity is rounded up to a power of two, 16 at
// least. Gives the handle, or NULL on failure with a library-owned error
// string in *err when err isn't NULL. Destroy it with cstring_destroy().
cstr cstring_create_full(const char *initial, ccol_memmgmt_procs *m_procs,
                         char **err);

// Creates a string holding a copy of initial (NULL for an empty one), using
// malloc and free. The capacity is rounded up to a power of two, 16 at least.
// Gives the handle, or NULL on failure with a library-owned error string in
// *err when err isn't NULL. Destroy it with cstring_destroy().
static inline __attribute__((always_inline)) cstr
cstring_create(const char *initial, char **err) {
  return cstring_create_full(initial, NULL, err);
}

// Creates a string holding a copy of initial (NULL for an empty one), using
// malloc and free, for callers that only check for NULL: It gives the handle,
// or NULL when an allocation fails, with no error string. Destroy it with
// cstring_destroy().
static inline __attribute__((always_inline)) cstr
cstring_new(const char *initial) {
  return cstring_create_full(initial, NULL, NULL);
}

// Gives the allocator of s, or NULL for the default one, so a caller can
// allocate related memory the same way. Asserts on a NULL s.
ccol_memmgmt_procs *cstring_get_mprocs(cstr s);

// Internal: Frees the string. Use cstring_destroy() instead.
void cstring__destroy(cstr s);

// Frees the string and sets s to NULL, so a second call is harmless. s must be
// a modifiable lvalue and is evaluated once; a NULL s does nothing. Pointers
// from cstring_c_str() are invalid afterwards. It's a statement.
#define cstring_destroy(s)    \
  ccol__cstring_destroy_impl( \
      s, ccol__uniq(ccol__cstring_destroy_slot, __COUNTER__))

// Internal: The body of cstring_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings.
#define ccol__cstring_destroy_impl(s, slot) \
  do {                                      \
    __typeof__(s) *slot = &(s);             \
    if (*slot) {                            \
      cstring__destroy(*slot);              \
      *slot = NULL;                         \
    }                                       \
  } while (0)

// Internal: Destroys *sp and sets it to NULL. It's the cleanup function the
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

// Appends str to s, growing the buffer when needed; str may point into s. It
// gives ccol_success, ccol_invalid_args for a NULL str, ccol_container_full
// when the length would overflow size_t, or ccol_not_enough_memory, with s
// unchanged on failure. Asserts on a NULL s.
ccol_retval cstring_append(cstr s, const char *str);

// Puts str in front of the content of s, growing the buffer when needed. It
// gives ccol_success, ccol_invalid_args for a NULL str, ccol_container_full
// when the length would overflow size_t, or ccol_not_enough_memory, with s
// unchanged on failure. Asserts on a NULL s.
ccol_retval cstring_prepend(cstr s, const char *str);

// Inserts str at byte position pos (0 to the length), moving the rest to the
// right; str may point into s. It gives ccol_success, ccol_invalid_args for a
// NULL str or a pos past the end, ccol_container_full when the length would
// overflow size_t, or ccol_not_enough_memory, with s unchanged on failure.
// Asserts on a NULL s.
ccol_retval cstring_insert(cstr s, size_t pos, const char *str);

// Replaces the content of s with a copy of str; str may point into s. It
// gives ccol_success, ccol_invalid_args for a NULL str, or
// ccol_not_enough_memory, with s unchanged on failure (ccol_container_full
// would need a str that fills the address space). Asserts on a NULL s.
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

// Says whether the content of s equals str; a NULL str gives false. Asserts on
// a NULL s.
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

// Gives the index of the last needle in s (overlaps count: The last "aba" in
// "ababa" is at 2), the length of s for an empty needle, or ccol_invalid_size
// when it isn't there or needle is NULL. When s has 256 bytes or more, the
// search runs backwards in time linear in both lengths. A needle longer than
// 128 bytes may take a temporary table of one size_t per byte from the
// allocator of s; if that's refused the answer is the same but the time can be
// the product of the lengths. Asserts on a NULL s.
size_t cstring_rfind(cstr s, const char *needle);

// Creates a string holding the bytes [start, start + length) of s, clamped to
// its end; a start at or past the end gives an empty string. The new string
// uses the allocator of s. Gives NULL, with a library-owned error string in
// *err when err isn't NULL, when the allocation fails. You destroy the result.
// Asserts on a NULL s.
cstr cstring_substring(cstr s, size_t start, size_t length, char **err);

// Creates an independent copy of s that uses the allocator of s, or gives NULL
// on failure with a library-owned error string in *err when err isn't NULL.
// You destroy the result. Asserts on a NULL s.
cstr cstring_copy(cstr s, char **err);

// Splits s at every delimiter (not NULL or empty) into a cvec of new cstr
// tokens, with empty tokens kept; "a,,b" split at "," gives "a", "" and "b".
// Gives NULL on failure, with a library-owned error string in *err when err
// isn't NULL. You destroy every token and then the vector. Asserts on a NULL
// s.
cvec cstring_split(cstr s, const char *delimiter, char **err);

// Declares a string handle named s without creating the string. s is
// uninitialized; create the string with cstr_init() or cstr_init_mp() and free
// it with cstr_destroy(). It's a declaration.
#define cstr_declare(s) cstr s

// Declares a string handle named s, set to NULL. Create the string with
// cstr_init() or cstr_init_mp(). Whatever string s holds is destroyed when s
// goes out of scope, on every way out, so don't destroy it yourself and don't
// return it. It's a declaration.
#define cstr_declare_scoped(s) \
  cstr s ccol__destructor(cstring__destroy_cleanup) = NULL

// Creates a string holding a copy of initial (NULL for an empty one), using
// malloc and free, and assigns it to s, which cstr_declare() or
// cstr_declare_scoped() declared. s is assigned once, after the creation. A
// failed creation prints the reason and ends the program. It's a statement.
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

// Creates a string holding a copy of initial (NULL for an empty one) and
// assigns it to s, which cstr_declare() or cstr_declare_scoped() declared.
// mprocs is the allocator table, or NULL for malloc and free; the string keeps
// its own copy. A failed creation (an allocator with a NULL function included)
// prints the reason and ends the program. It's a statement.
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

// Declares s and creates in it a string holding a copy of initial (NULL for an
// empty one), using malloc and free. A failed creation prints the reason and
// ends the program. Free the string with cstr_destroy().
#define cstr_construct(s, initial) \
  cstr_declare(s);                 \
  cstr_init(s, initial)

// Declares s and creates in it a string holding a copy of initial (NULL for an
// empty one), using malloc and free. The string is destroyed when s goes out
// of scope, on every way out, so don't destroy it yourself and don't return
// it. A failed creation prints the reason and ends the program.
#define cstr_construct_scoped(s, initial) \
  cstr_declare_scoped(s);                 \
  cstr_init(s, initial)

// Declares s and creates in it a string holding a copy of initial (NULL for an
// empty one). mprocs is the allocator table, or NULL for malloc and free; the
// string keeps its own copy. A failed creation prints the reason and ends the
// program. Free the string with cstr_destroy().
#define cstr_construct_mp(s, initial, mprocs) \
  cstr_declare(s);                            \
  cstr_init_mp(s, initial, mprocs)

// Declares s and creates in it a string holding a copy of initial (NULL for an
// empty one). mprocs is the allocator table, or NULL for malloc and free; the
// string keeps its own copy. The string is destroyed when s goes out of scope,
// on every way out, so don't destroy it yourself and don't return it. A failed
// creation prints the reason and ends the program.
#define cstr_construct_mp_scoped(s, initial, mprocs) \
  cstr_declare_scoped(s);                            \
  cstr_init_mp(s, initial, mprocs)

// Frees the string and sets s to NULL, so a second call is harmless. s must be
// a modifiable lvalue and is evaluated once; a NULL s does nothing. It's a
// statement.
#define cstr_destroy(s) cstring_destroy((s))

// Appends str to s, growing the buffer when needed; str may point into s. A
// NULL str, a length that would overflow size_t or a failed allocation prints
// the error code and ends the program. Asserts on a NULL s. It's a statement.
#define cstr_append(s, str)                                                \
  do {                                                                     \
    ccol_retval cstr__append_r = cstring_append((s), (str));               \
    if (cstr__append_r != ccol_success) {                                  \
      ccol__fatal_err("cstr_append('%s'): r: %d (%s)", #s, cstr__append_r, \
                      ccol_retval_to_str(cstr__append_r));                 \
    }                                                                      \
  } while (0)

// Puts str in front of the content of s, growing the buffer when needed. A
// NULL str, a length that would overflow size_t or a failed allocation prints
// the error code and ends the program. Asserts on a NULL s. It's a statement.
#define cstr_prepend(s, str)                                                 \
  do {                                                                       \
    ccol_retval cstr__prepend_r = cstring_prepend((s), (str));               \
    if (cstr__prepend_r != ccol_success) {                                   \
      ccol__fatal_err("cstr_prepend('%s'): r: %d (%s)", #s, cstr__prepend_r, \
                      ccol_retval_to_str(cstr__prepend_r));                  \
    }                                                                        \
  } while (0)

// Inserts str into s at byte position pos (0 to the length), moving the rest
// to the right; str may point into s. A NULL str, a pos past the end, a length
// that would overflow size_t or a failed allocation prints the error code and
// ends the program. Asserts on a NULL s. It's a statement.
#define cstr_insert(s, pos, str)                                           \
  do {                                                                     \
    ccol_retval cstr__insert_r = cstring_insert((s), (pos), (str));        \
    if (cstr__insert_r != ccol_success) {                                  \
      ccol__fatal_err("cstr_insert('%s'): r: %d (%s)", #s, cstr__insert_r, \
                      ccol_retval_to_str(cstr__insert_r));                 \
    }                                                                      \
  } while (0)

// Replaces the whole content of s with a copy of str; str may point into s. A
// NULL str or a failed allocation prints the error code and ends the program.
// Asserts on a NULL s. It's a statement.
#define cstr_set(s, str)                                             \
  do {                                                               \
    ccol_retval cstr__set_r = cstring_set((s), (str));               \
    if (cstr__set_r != ccol_success) {                               \
      ccol__fatal_err("cstr_set('%s'): r: %d (%s)", #s, cstr__set_r, \
                      ccol_retval_to_str(cstr__set_r));              \
    }                                                                \
  } while (0)

// Replaces every non-overlapping needle in s with replacement; a replacement
// that contains needle is fine, and a needle that isn't there changes nothing.
// A NULL or empty needle, a NULL replacement, a length overflow or a failed
// allocation prints the error code and ends the program. Asserts on a NULL s.
// It's a statement.
#define cstr_replace(s, needle, replacement)                                 \
  do {                                                                       \
    ccol_retval cstr__replace_r =                                            \
        cstring_replace((s), (needle), (replacement));                       \
    if (cstr__replace_r != ccol_success) {                                   \
      ccol__fatal_err("cstr_replace('%s'): r: %d (%s)", #s, cstr__replace_r, \
                      ccol_retval_to_str(cstr__replace_r));                  \
    }                                                                        \
  } while (0)

// Makes room in s for at least cap bytes, the '\0' included, rounded up to a
// power of two (16 at least), so later appends don't reallocate. It never
// shrinks. cap is evaluated once. A size that is too large or a failed
// allocation prints an error and ends the program. It's a statement.
#define cstr_reserve(s, cap)        \
  ccol__cstr_reserve_impl(s, (cap), \
                          ccol__uniq(ccol__cstr_reserve_cap, __COUNTER__))

// Internal: The body of cstr_reserve(), with a ccol__uniq() temporary so it can
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

// Evaluates to a new cstr holding the bytes [start, start + len) of s,
// clamped to its end; a start at or past the end gives an empty string. The
// new string uses the allocator of s, and you destroy it. Each argument is
// evaluated once. A failed allocation prints the reason and ends the program.
#define cstr_substring(s, start, len)                                         \
  ccol__cstr_substring_impl(                                                  \
      s, (start), (len), ccol__uniq(ccol__cstr_substring_start, __COUNTER__), \
      ccol__uniq(ccol__cstr_substring_len, __COUNTER__),                      \
      ccol__uniq(ccol__cstr_substring_err, __COUNTER__),                      \
      ccol__uniq(ccol__cstr_substring_result, __COUNTER__))

// Internal: The body of cstr_substring(), with ccol__uniq() temporaries so it
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

// Evaluates to the length of s in bytes, without the '\0', as a size_t.
// Asserts on a NULL s.
#define cstr_length(s) cstring_length((s))

// Evaluates to the NUL-terminated content of s as a const char *, for any API
// that takes a C string. The pointer is valid until the next change to s.
// Asserts on a NULL s.
#define cstr_c_str(s) cstring_c_str((s))

// Evaluates to the byte at idx of s as a char, or '\0' when idx is out of
// range. Asserts on a NULL s.
#define cstr_at(s, idx) cstring_at((s), (idx))

// Evaluates to true when s holds no bytes. Asserts on a NULL s.
#define cstr_is_empty(s) cstring_is_empty((s))

// Empties s and tries to shrink its buffer back to 16 bytes, so you can fill
// it again; when the shrink fails the capacity stays, but the length is 0
// anyway. Asserts on a NULL s.
#define cstr_reset(s) cstring_reset((s))

// Turns the ASCII lower-case letters of s into upper case in place. Every other
// byte stays, whatever the locale. Asserts on a NULL s.
#define cstr_to_upper(s) cstring_to_upper((s))

// Turns the ASCII upper-case letters of s into lower case in place. Every other
// byte stays, whatever the locale. Asserts on a NULL s.
#define cstr_to_lower(s) cstring_to_lower((s))

// Removes white space at both ends of s in place: The six ASCII bytes space,
// tab, LF, VT, FF and CR, whatever the locale, so a byte above 0x7F (a piece
// of UTF-8) is never cut. Asserts on a NULL s.
#define cstr_trim(s) cstring_trim((s))

// Compares s with str as strcmp() does and evaluates to a negative, zero or
// positive int; a NULL str gives 1. Asserts on a NULL s.
#define cstr_compare(s, str) cstring_compare((s), (str))

// Evaluates to true when the content of s equals str; a NULL str gives false.
// Asserts on a NULL s.
#define cstr_equals(s, str) cstring_equals((s), (str))

// Evaluates to true when s starts with pfx. An empty pfx matches, a NULL one
// doesn't. Asserts on a NULL s.
#define cstr_starts_with(s, pfx) cstring_starts_with((s), (pfx))

// Evaluates to true when s ends with sfx. An empty sfx matches, a NULL one
// doesn't. Asserts on a NULL s.
#define cstr_ends_with(s, sfx) cstring_ends_with((s), (sfx))

// Evaluates to the index of the first needle in s, 0 for an empty needle, or
// ccol_invalid_size when it isn't there or needle is NULL. Asserts on a NULL
// s.
#define cstr_find(s, needle) cstring_find((s), (needle))

// Evaluates to the index of the last needle in s (overlaps count: The last
// "aba" in "ababa" is at 2), the length of s for an empty needle, or
// ccol_invalid_size when it isn't there or needle is NULL. Asserts on a NULL
// s.
#define cstr_rfind(s, needle) cstring_rfind((s), (needle))

// Evaluates to a new, independent copy of s that uses the allocator of s, or
// to NULL on failure with a library-owned error string in *err when err isn't
// NULL. You destroy the copy. Asserts on a NULL s.
#define cstr_copy(s, err) cstring_copy((s), (err))

// Splits s at every occurrence of delim (not NULL or empty) and evaluates to a
// cvec of new cstr tokens, with empty tokens kept; "a,,b" split at "," gives
// "a", "" and "b". On failure it gives NULL, with a library-owned error string
// in *err when err isn't NULL. You destroy every token and then the vector.
// Asserts on a NULL s.
#define cstr_split(s, delim, err) cstring_split((s), (delim), (err))

#pragma GCC visibility pop
