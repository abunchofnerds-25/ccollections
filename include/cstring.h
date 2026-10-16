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

/* Everything this header declares from here to the end of the file belongs
 * to the public ABI of libccollections, and the shared library exports all
 * of it. The library is built with -fvisibility=hidden, so a function or an
 * object outside these blocks stays internal: it isn't in the library's
 * dynamic symbol table, an application linking against the library can't
 * interpose it, and a symbol of the same name in that application can't
 * collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cstring.h
 * @brief Dynamic string container with a rich set of string operations
 *
 * A heap string container that resizes itself when it needs more room. The
 * internal buffer always holds a null-terminated C string, and its capacity
 * is always a power of two, 16 bytes at the smallest.
 *
 * Key features:
 * - The container manages its own capacity and grows to the next power of
 *   two when it runs out of room.
 * - A full set of common string operations: append, prepend, insert, find
 *   and so on.
 * - Support for custom memory management
 * - RAII-style automatic destruction with the _ccol_destructor attribute
 * - Split gives back a cvec of cstr, which is easy to iterate over
 *
 * Integration with maps (chashmap / cbstmap):
 * cstr is NOT a key or value type that the map containers know about. They
 * understand char * keys directly: such a key is hashed by its content, and
 * a short one is kept inside the entry itself (the small string
 * optimisation, SSO). Call cstring_c_str() to get a char * view and use that
 * as the key:
 *
 *     char *k = (char *)cstring_c_str(my_cstr);
 *     chmap_insert(map, k, value);
 *
 * The map copies the string's content right away, so changing or destroying
 * the cstr afterwards doesn't affect the entry the map holds.
 */

/** @brief Opaque string structure */
typedef struct cstring cstring;

/** @brief Pointer to string (handle type) */
typedef cstring *cstr;

/* ========================================================================== */
/*                         CORE STRING FUNCTIONS                              */
/* ========================================================================== */

/**
 * @brief Create a string with custom memory management
 *
 * Allocates and initialises a new cstring. A non-NULL @p initial is copied
 * into the new string, and a NULL @p initial gives an empty string. The
 * capacity of the internal buffer is rounded up to the nearest power of two,
 * 16 at the smallest.
 *
 * @param initial  Initial C string content, or NULL for an empty string
 * @param m_procs  Custom memory management procedures, or NULL for default
 * @param err      Optional pointer to receive an error string on failure
 *
 * @return Pointer to the new string, or NULL on failure
 *
 * @see cstring_create
 * @see cstring_destroy
 */
cstr cstring_create_full(const char *initial, ccol_memmgmt_procs_t *m_procs,
                         char **err);

/**
 * @brief Create a string with default memory management
 *
 * A convenience wrapper around cstring_create_full() with the default
 * malloc/free allocators.
 *
 * @param initial  Initial C string content, or NULL for an empty string
 * @param err      Optional pointer to receive an error string on failure
 *
 * @return Pointer to the new string, or NULL on failure
 */
static inline __attribute__((always_inline)) cstr
cstring_create(const char *initial, char **err) {
  return cstring_create_full(initial, NULL, err);
}

/**
 * @brief Create a string with the default memory management in an
 * optimistic manner
 *
 * A convenience wrapper around cstring_create_full() with the default
 * memory management. It takes no error buffer since it expects to succeed.
 *
 * @param initial The initial C string, or NULL for an empty string
 *
 * @return Pointer to the new string, or NULL on failure.
 *
 */
static inline __attribute__((always_inline)) cstr
cstring_new(const char *initial) {
  return cstring_create_full(initial, NULL, NULL);
}

/**
 * @brief Get the memory management procedures for a string
 *
 * @param s  String to query
 * @return   Pointer to the memory management procedures, or NULL if the
 *           string uses the default procedures
 * @note     Will assert if s is NULL
 */
ccol_memmgmt_procs_t *cstring_get_mprocs(cstr s);

/**
 * @brief Destroy a string (internal; do not call directly)
 *
 * @param s  String to destroy
 * @warning  Use the cstring_destroy() macro instead
 */
void __cstring_destroy(cstr s);

/**
 * @brief Destroy a string and set its pointer to NULL
 *
 * Frees the internal buffer and the container, then sets the pointer to
 * NULL, so calling it a second time is safe.
 *
 * @param s  String variable to destroy (set to NULL on return). It must be a
 * modifiable lvalue, such as a variable or an element of a vector, and it is
 * evaluated exactly once.
 */
#define cstring_destroy(s)    \
  _ccol_cstring_destroy_impl( \
      s, _ccol_uniq(__ccol_cstring_destroy_slot, __COUNTER__))

/* Internal: the body of cstring_destroy. slot is a name from _ccol_uniq(),
 * so the macro can nest inside the argument of another destroy macro and
 * still be -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_cstring_destroy_impl(s, slot) \
  do {                                      \
    __typeof__(s) *slot = &(s);             \
    if (*slot) {                            \
      __cstring_destroy(*slot);             \
      *slot = NULL;                         \
    }                                       \
  } while (0)

/**
 * @brief Cleanup helper for _ccol_destructor (RAII)
 *
 * Runs automatically when a scoped cstr variable goes out of scope.
 *
 * @param sp  Pointer to the cstr variable
 */
static inline void ___cstring_destroy(cstr *sp) {
  if (sp && *sp) {
    __cstring_destroy(*sp);
    *sp = NULL;
  }
}

/* ========================================================================== */
/*                         QUERY FUNCTIONS                                    */
/* ========================================================================== */

/**
 * @brief Return the number of characters in the string (without the '\0')
 *
 * @param s  String to query
 * @return   Length in bytes
 * @note     Will assert if s is NULL
 */
size_t cstring_length(cstr s);

/**
 * @brief Return a read-only pointer to the null-terminated character data
 *
 * @param s  String to query
 * @return   Pointer to internal null-terminated buffer
 * @warning  The pointer becomes invalid after any operation that changes the
 *           string
 * @note     Will assert if s is NULL
 */
const char *cstring_c_str(cstr s);

/**
 * @brief Return the character at a given index
 *
 * @param s    String to query
 * @param idx  Zero-based character index
 * @return     Character at @p idx, or '\0' if @p idx is out of bounds
 * @note       Will assert if s is NULL
 */
char cstring_at(cstr s, size_t idx);

/**
 * @brief Check whether the string contains no characters
 *
 * @param s  String to query
 * @return   true if length is zero
 * @note     Will assert if s is NULL
 */
bool cstring_is_empty(cstr s);

/* ========================================================================== */
/*                         MODIFICATION FUNCTIONS                             */
/* ========================================================================== */

/**
 * @brief Append a C string to the end of this string
 *
 * @param s    String to change
 * @param str  C string to append (must not be NULL)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if str is NULL
 * @return ccol_container_full if the result would overflow size_t
 * @return ccol_not_enough_memory if reallocation fails
 * @note   Will assert if s is NULL
 */
ccol_retval_t cstring_append(cstr s, const char *str);

/**
 * @brief Prepend a C string to the beginning of this string
 *
 * @param s    String to change
 * @param str  C string to prepend (must not be NULL)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if str is NULL
 * @return ccol_container_full if the result would overflow size_t
 * @return ccol_not_enough_memory if reallocation fails
 * @note   Will assert if s is NULL
 */
ccol_retval_t cstring_prepend(cstr s, const char *str);

/**
 * @brief Insert a C string at a given position
 *
 * The characters at positions >= @p pos move to the right to make room.
 *
 * @param s    String to change
 * @param pos  Insertion position (0 ... length, inclusive)
 * @param str  C string to insert (must not be NULL)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if str is NULL or pos > length
 * @return ccol_container_full if the result would overflow size_t
 * @return ccol_not_enough_memory if reallocation fails
 * @note   Will assert if s is NULL
 */
ccol_retval_t cstring_insert(cstr s, size_t pos, const char *str);

/**
 * @brief Replace the entire content of this string with a new C string
 *
 * @param s    String to change
 * @param str  New content (must not be NULL)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if str is NULL
 * @return ccol_container_full if str is too large to represent. A real
 * program can't get there, since it would need a str that fills the whole
 * address space.
 * @return ccol_not_enough_memory if reallocation fails
 * @note   Will assert if s is NULL
 */
ccol_retval_t cstring_set(cstr s, const char *str);

/**
 * @brief Clear all characters and shrink capacity back to the minimum
 *
 * If the shrinking reallocation fails the capacity stays as it is, but the
 * length is reset to 0 in any case.
 *
 * @param s  String to reset
 * @note     Will assert if s is NULL
 */
void cstring_reset(cstr s);

/**
 * @brief Pre-allocate capacity for at least @p min_capacity bytes
 *
 * @p min_capacity is rounded up to the nearest power of two, 16 at the
 * smallest. When the current capacity is already enough nothing happens and
 * the function returns true.
 *
 * @param s             String to reserve capacity for
 * @param min_capacity  Minimum number of bytes to reserve (with the '\0')
 *
 * @return true on success, false if the reallocation fails or if the size is
 *         too large
 * @note   Will assert if s is NULL
 */
bool cstring_reserve(cstr s, size_t min_capacity);

/**
 * @brief Convert the ASCII letters to uppercase in-place
 *
 * Every ASCII lower-case letter becomes upper case and every other byte is
 * left as it is. The locale has no effect on the result, and a byte above
 * 0x7F never changes.
 *
 * @param s  String to change
 * @note     Will assert if s is NULL
 */
void cstring_to_upper(cstr s);

/**
 * @brief Convert the ASCII letters to lowercase in-place
 *
 * Every ASCII upper-case letter becomes lower case and every other byte is
 * left as it is. The locale has no effect on the result, and a byte above
 * 0x7F never changes.
 *
 * @param s  String to change
 * @note     Will assert if s is NULL
 */
void cstring_to_lower(cstr s);

/**
 * @brief Strip the white space at the start and at the end, in-place
 *
 * White space means the six ASCII bytes space, '\t', '\n', '\v', '\f' and
 * '\r', whatever the locale. No other byte counts, so a byte above 0x7F,
 * like a no-break space or a piece of a UTF-8 sequence, is never removed and
 * a UTF-8 string stays valid.
 *
 * @param s  String to change
 * @note     Will assert if s is NULL
 */
void cstring_trim(cstr s);

/**
 * @brief Replace every non-overlapping occurrence of @p needle with
 * @p replacement
 *
 * The result is built in a new buffer that is then swapped in, so
 * @p replacement may contain @p needle without causing an infinite loop.
 *
 * @param s            String to change
 * @param needle       Substring to search for (must not be NULL or empty)
 * @param replacement  Substitute string (must not be NULL)
 *
 * @return ccol_success on success, also when needle isn't found.
 * @return ccol_invalid_args if needle is NULL or empty, or if replacement is
 * NULL
 * @return ccol_container_full if the result would overflow size_t
 * @return ccol_not_enough_memory if buffer allocation fails
 * @note   Will assert if s is NULL
 */
ccol_retval_t cstring_replace(cstr s, const char *needle,
                              const char *replacement);

/* ========================================================================== */
/*                         SEARCH AND COMPARISON                              */
/* ========================================================================== */

/**
 * @brief Compare the string to a C string (lexicographic order)
 *
 * Same semantics as strcmp().
 *
 * @param s    String to compare
 * @param str  C string to compare against
 *
 * @return Negative / zero / positive if s < / == / > str
 * @return 1 if str is NULL, since a non-null string counts as the greater
 * one.
 * @note   Will assert if s is NULL
 */
int cstring_compare(cstr s, const char *str);

/**
 * @brief Check whether the string's content equals a C string
 *
 * @param s    String to compare
 * @param str  C string to compare against
 *
 * @return true if the two contents are equal, false if they are not
 * @note   Will assert if s is NULL
 */
bool cstring_equals(cstr s, const char *str);

/**
 * @brief Check whether the string begins with @p prefix
 *
 * @param s       String to test
 * @param prefix  Prefix to look for
 *
 * @return true if the string starts with @p prefix
 * @return false if @p prefix is NULL
 * @note   An empty prefix always matches
 * @note   Will assert if s is NULL
 */
bool cstring_starts_with(cstr s, const char *prefix);

/**
 * @brief Check whether the string ends with @p suffix
 *
 * @param s       String to test
 * @param suffix  Suffix to look for
 *
 * @return true if the string ends with @p suffix
 * @return false if @p suffix is NULL
 * @note   An empty suffix always matches
 * @note   Will assert if s is NULL
 */
bool cstring_ends_with(cstr s, const char *suffix);

/**
 * @brief Find the first occurrence of @p needle
 *
 * @param s       String to search
 * @param needle  Substring to find
 *
 * @return Zero-based index of the first occurrence
 * @return 0 if @p needle is an empty string (matches at the start)
 * @return ccol_invalid_size if not found, or if @p needle is NULL
 * @note   Will assert if s is NULL
 */
size_t cstring_find(cstr s, const char *needle);

/**
 * @brief Find the last occurrence of @p needle
 *
 * @param s       String to search
 * @param needle  Substring to find
 *
 * @return Zero-based index of the last occurrence
 * @return cstring_length(s) if @p needle is an empty string, which is the
 * past-the-end position, the same convention std::string::rfind("") has in
 * C++.
 * @return ccol_invalid_size if not found, or if @p needle is NULL
 * @note   Will assert if s is NULL
 * @note   Overlapping occurrences count: the last "aba" in "ababa" starts at
 * index 2
 * @note   For a string of 256 bytes or more the search goes backwards from
 * the end and stops at the first match it meets, and its time is linear in
 * the length of the string plus the length of @p needle, whatever their
 * contents. A shorter string is searched from the start, which is faster at
 * that length.
 * @note   A needle of up to 128 bytes needs no memory. A longer needle may
 * make the search take one temporary table of one size_t per needle byte
 * from the allocator of s, which is freed before the call returns. If that
 * allocator refuses the table the answer is the same, but the time can then
 * grow with the product of the two lengths.
 */
size_t cstring_rfind(cstr s, const char *needle);

/* ========================================================================== */
/*                    SUBSTRING, COPY AND SPLIT                               */
/* ========================================================================== */

/**
 * @brief Create a new cstring holding a sub-range of this string
 *
 * Copies the characters in [@p start, @p start + @p length) into a new
 * cstring. If @p start + @p length goes past the end of the string the range
 * is clamped to the end, and if @p start is at or past the string's length
 * the result is an empty cstring.
 *
 * @param s       Source string
 * @param start   Zero-based start index
 * @param length  Number of characters to include
 * @param err     Optional pointer to receive an error string on failure
 *
 * @return New cstring, or NULL on allocation failure
 * @note   The caller must destroy the returned cstring
 * @note   Will assert if s is NULL
 */
cstr cstring_substring(cstr s, size_t start, size_t length, char **err);

/**
 * @brief Create an independent copy of a string
 *
 * @param s    String to copy
 * @param err  Optional pointer to receive an error string on failure
 *
 * @return New cstring with the same content, or NULL on failure
 * @note   The caller must destroy the returned cstring
 * @note   Will assert if s is NULL
 */
cstr cstring_copy(cstr s, char **err);

/**
 * @brief Split the string by @p delimiter and return the parts as a vector
 *
 * Every token between occurrences of @p delimiter goes into the returned
 * cvec as a new cstr. When the string starts or ends with the delimiter, the
 * empty token at the start or the end counts as a token too.
 *
 * @param s          String to split
 * @param delimiter  Separator string (must not be NULL or empty)
 * @param err        Optional pointer to receive an error string on failure
 *
 * @return cvec that holds the tokens, or NULL on failure. Its elem_size is
 * sizeof(cstr).
 *
 * @note The caller must destroy every cstr in the vector, and the vector
 * itself. For example:
 * @code
 * cvec parts = cstring_split(s, ",", NULL);
 * if (parts) {
 *   cvec_redeclare(parts, cstr);
 *   for (size_t i = 0; i < cvec_size(parts); i++) {
 *     cstr_destroy(cvec_at(parts, i));
 *   }
 *   cvec_destroy(parts);
 * }
 * @endcode
 * @note Will assert if s is NULL
 */
cvec cstring_split(cstr s, const char *delimiter, char **err);

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/**
 * @brief Declare an uninitialised cstr variable
 *
 * @param s  Name of the variable to declare
 */
#define cstr_declare(s) cstr s

/**
 * @brief Declare a cstr variable with automatic RAII destruction
 *
 * The compiler calls __cstring_destroy when the variable goes out of scope.
 *
 * @param s  Name of the variable to declare
 */
#define cstr_declare_scoped(s) \
  cstr s _ccol_destructor(___cstring_destroy) = NULL

/**
 * @brief Initialise a declared cstr with the default allocators
 *
 * Calls _ccol_fatal_err() if the creation fails.
 *
 * @param s        cstr lvalue to initialise (you must declare it first). It
 *                 is assigned once, after the string is created.
 * @param initial  Initial content (C string or NULL for empty)
 */
#define cstr_init(s, initial)                                                  \
  do {                                                                         \
    char *__cstr_init_err = NULL;                                              \
    __typeof__(s) __cstr_init_s = cstring_create((initial), &__cstr_init_err); \
    if (!__cstr_init_s) {                                                      \
      _ccol_fatal_err("cstr_init('%s'): %s", #s,                               \
                      __cstr_init_err ? __cstr_init_err : "unknown error");    \
    }                                                                          \
    (s) = __cstr_init_s;                                                       \
  } while (0)

/**
 * @brief Initialise a declared cstr with custom memory management
 *
 * Calls _ccol_fatal_err() if the creation fails.
 *
 * @param s        cstr lvalue to initialise (you must declare it first). It
 *                 is assigned once, after the string is created.
 * @param initial  Initial content (C string or NULL for empty)
 * @param mprocs   Custom memory management procedures
 */
#define cstr_init_mp(s, initial, mprocs)                               \
  do {                                                                 \
    char *__cstr_init_mp_err = NULL;                                   \
    __typeof__(s) __cstr_init_mp_s =                                   \
        cstring_create_full((initial), (mprocs), &__cstr_init_mp_err); \
    if (!__cstr_init_mp_s) {                                           \
      _ccol_fatal_err(                                                 \
          "cstr_init_mp('%s'): %s", #s,                                \
          __cstr_init_mp_err ? __cstr_init_mp_err : "unknown error");  \
    }                                                                  \
    (s) = __cstr_init_mp_s;                                            \
  } while (0)

/**
 * @brief Declare and initialise a cstr in one step
 *
 * @param s        Variable name to create
 * @param initial  Initial content (C string or NULL for empty)
 *
 * Example:
 * @code
 * cstr_construct(greeting, "Hello");
 * cstr_append(greeting, ", world!");
 * cstr_destroy(greeting);
 * @endcode
 */
#define cstr_construct(s, initial) \
  cstr_declare(s);                 \
  cstr_init(s, initial)

/**
 * @brief Declare and initialise a cstr with automatic RAII destruction
 *
 * @param s        Variable name to create
 * @param initial  Initial content (C string or NULL for empty)
 */
#define cstr_construct_scoped(s, initial) \
  cstr_declare_scoped(s);                 \
  cstr_init(s, initial)

/**
 * @brief Declare and initialise a cstr with custom memory management
 *
 * @param s        Variable name to create
 * @param initial  Initial content (C string or NULL for empty)
 * @param mprocs   Custom memory management procedures
 */
#define cstr_construct_mp(s, initial, mprocs) \
  cstr_declare(s);                            \
  cstr_init_mp(s, initial, mprocs)

/**
 * @brief Declare and initialise a cstr with custom memory management and RAII
 *
 * @param s        Variable name to create
 * @param initial  Initial content (C string or NULL for empty)
 * @param mprocs   Custom memory management procedures
 */
#define cstr_construct_mp_scoped(s, initial, mprocs) \
  cstr_declare_scoped(s);                            \
  cstr_init_mp(s, initial, mprocs)

/**
 * @brief Destroy a cstr and set it to NULL (type-inferred wrapper)
 *
 * @param s  cstr lvalue to destroy. It is evaluated exactly once.
 */
#define cstr_destroy(s) cstring_destroy((s))

/* ========================================================================== */
/*                         OPERATION MACROS                                   */
/* ========================================================================== */

/* Every retval local below has a name that only one macro uses and that
 * starts with two underscores, because the shorter '_r' isn't safe here.
 * Each macro declares its local in one statement whose initializer holds the
 * caller's arguments, since that's where they get substituted. In C the
 * scope of an identifier starts right after its own declarator, so it starts
 * before the initializer is evaluated; that's the same rule that makes
 * `int x = x;` a self-reference and not a copy of an outer x. So when a
 * caller's argument has the same name as the local, it binds to this
 * macro's uninitialized local instead of to the caller's variable, and no
 * tool says a word about it. In cstr_append, cstr_prepend, cstr_set and
 * cstr_replace every shadowed parameter is a `const char *`, so a collision
 * with a short plausible name like '_r' fails to compile, because an enum
 * ends up where a pointer belongs. cstr_insert's `pos` parameter is a
 * size_t though, and a size_t takes an enum value silently. No compiler has
 * to report that under any set of warning flags, so that collision compiles
 * and the caller gets a wrong value. Since C macros aren't hygienic, a name
 * this specific to one macro is the only practical fix. */

/** @brief Append @p str. Calls _ccol_fatal_err() on failure */
#define cstr_append(s, str)                                                 \
  do {                                                                      \
    ccol_retval_t __cstr_append_r = cstring_append((s), (str));             \
    if (__cstr_append_r != ccol_success) {                                  \
      _ccol_fatal_err("cstr_append('%s'): r: %d (%s)", #s, __cstr_append_r, \
                      ccol_retval_to_str(__cstr_append_r));                 \
    }                                                                       \
  } while (0)

/** @brief Prepend @p str. Calls _ccol_fatal_err() on failure */
#define cstr_prepend(s, str)                                                  \
  do {                                                                        \
    ccol_retval_t __cstr_prepend_r = cstring_prepend((s), (str));             \
    if (__cstr_prepend_r != ccol_success) {                                   \
      _ccol_fatal_err("cstr_prepend('%s'): r: %d (%s)", #s, __cstr_prepend_r, \
                      ccol_retval_to_str(__cstr_prepend_r));                  \
    }                                                                         \
  } while (0)

/** @brief Insert @p str at @p pos. Calls _ccol_fatal_err() on failure */
#define cstr_insert(s, pos, str)                                            \
  do {                                                                      \
    ccol_retval_t __cstr_insert_r = cstring_insert((s), (pos), (str));      \
    if (__cstr_insert_r != ccol_success) {                                  \
      _ccol_fatal_err("cstr_insert('%s'): r: %d (%s)", #s, __cstr_insert_r, \
                      ccol_retval_to_str(__cstr_insert_r));                 \
    }                                                                       \
  } while (0)

/** @brief Replace the full content with @p str. Calls _ccol_fatal_err() on
 * failure */
#define cstr_set(s, str)                                              \
  do {                                                                \
    ccol_retval_t __cstr_set_r = cstring_set((s), (str));             \
    if (__cstr_set_r != ccol_success) {                               \
      _ccol_fatal_err("cstr_set('%s'): r: %d (%s)", #s, __cstr_set_r, \
                      ccol_retval_to_str(__cstr_set_r));              \
    }                                                                 \
  } while (0)

/** @brief Replace every occurrence of @p needle. Calls _ccol_fatal_err() on
 * failure */
#define cstr_replace(s, needle, replacement)                                  \
  do {                                                                        \
    ccol_retval_t __cstr_replace_r =                                          \
        cstring_replace((s), (needle), (replacement));                        \
    if (__cstr_replace_r != ccol_success) {                                   \
      _ccol_fatal_err("cstr_replace('%s'): r: %d (%s)", #s, __cstr_replace_r, \
                      ccol_retval_to_str(__cstr_replace_r));                  \
    }                                                                         \
  } while (0)

/** @brief Reserve at least @p cap bytes. Calls _ccol_fatal_err() on failure.
 * @p cap is evaluated exactly once, so the failure message reports the
 * capacity the call really used. */
#define cstr_reserve(s, cap)        \
  _ccol_cstr_reserve_impl(s, (cap), \
                          _ccol_uniq(__ccol_cstr_reserve_cap, __COUNTER__))

/* The public macro above names all the temporaries of this body with
 * _ccol_uniq(), so it can nest inside the argument of any public macro,
 * itself included. */
#define _ccol_cstr_reserve_impl(s, cap, __cstr_reserve_cap)                   \
  do {                                                                        \
    size_t __cstr_reserve_cap = (cap);                                        \
    if (!cstring_reserve((s), __cstr_reserve_cap)) {                          \
      _ccol_fatal_err(                                                        \
          "cstr_reserve('%s'): failed to reserve %zu bytes (out of memory?)", \
          #s, __cstr_reserve_cap);                                            \
    }                                                                         \
  } while (0)

/** @brief Create a new cstring with the content of [start,start+len) of the
 * cstring s. Calls _ccol_fatal_err() on failure. Each argument is evaluated
 * exactly once, so the failure message reports the start and the length the
 * call really used. */
#define cstr_substring(s, start, len)                                          \
  _ccol_cstr_substring_impl(                                                   \
      s, (start), (len), _ccol_uniq(__ccol_cstr_substring_start, __COUNTER__), \
      _ccol_uniq(__ccol_cstr_substring_len, __COUNTER__),                      \
      _ccol_uniq(__ccol_cstr_substring_err, __COUNTER__),                      \
      _ccol_uniq(__ccol_cstr_substring_result, __COUNTER__))

/* The public macro above names all the temporaries of this body with
 * _ccol_uniq(), so it can nest inside the argument of any public macro,
 * itself included. */
#define _ccol_cstr_substring_impl(s, start, len, __cstr_substring_start,      \
                                  __cstr_substring_len, __cstr_substring_err, \
                                  __cstr_substring_result)                    \
  ({                                                                          \
    size_t __cstr_substring_start = (start);                                  \
    size_t __cstr_substring_len = (len);                                      \
    char *__cstr_substring_err = NULL;                                        \
    __auto_type __cstr_substring_result =                                     \
        cstring_substring((s), __cstr_substring_start, __cstr_substring_len,  \
                          &__cstr_substring_err);                             \
    if (!__cstr_substring_result) {                                           \
      _ccol_fatal_err(                                                        \
          "cstr_substring('%s', start=%zu, len=%zu): %s", #s,                 \
          __cstr_substring_start, __cstr_substring_len,                       \
          __cstr_substring_err ? __cstr_substring_err : "unknown error");     \
    }                                                                         \
    __cstr_substring_result;                                                  \
  })

/* Simple pass-through wrappers */
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
