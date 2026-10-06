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

/* Everything that this header declares from here to the end of the file is
 * part of the public ABI of libccollections. The shared library exports all
 * of it. The build of the library uses -fvisibility=hidden. Any function or
 * object that one of these blocks does not cover stays internal to the
 * library. Such a symbol is absent from the dynamic symbol table of the
 * library. The application that links against the library cannot interpose
 * it. A symbol of the same name in that application cannot collide with
 * it. */
#pragma GCC visibility push(default)

/**
 * @file cstring.h
 * @brief Dynamic string container with a rich set of string operations
 *
 * This module gives you a string container on the heap. The container
 * changes its own size when it needs more room. The internal buffer always
 * holds a null-terminated C string. The capacity of that buffer is always a
 * power of two, and the smallest capacity is 16 bytes.
 *
 * Key features:
 * - The container manages its own capacity. It grows to the next power of
 *   two when it needs more room.
 * - A full set of common string operations. These are append, prepend,
 *   insert, find and other operations.
 * - Support for custom memory management
 * - RAII-style automatic destruction with the _ccol_destructor attribute
 * - Split gives back a cvec of cstr, which is easy to iterate over
 *
 * Integration with maps (chashmap / cbstmap):
 * cstr is NOT a key type or a value type that the map containers know. Those
 * containers understand char * keys directly. They hash such a key by its
 * content. They also keep a short key inside the entry itself, which is the
 * small string optimisation (SSO). Call cstring_c_str() to get a char * view
 * and give that as the key:
 *
 *     char *k = (char *)cstring_c_str(my_cstr);
 *     chmap_insert(map, k, value);
 *
 * The map copies the content of the string immediately. This is why you can
 * change or destroy the cstr afterwards. Such a change does not affect the
 * entry that the map holds.
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
 * This function allocates and initialises a new cstring. If @p initial is not
 * NULL, the function copies the content of @p initial into the new string. A
 * NULL value for @p initial creates an empty string. The function rounds the
 * capacity of the internal buffer up to the nearest power of two. The
 * smallest capacity is 16.
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
 * This function is a convenience wrapper around cstring_create_full(). It
 * uses the default malloc/free allocators.
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
 * This function is a convenience wrapper around cstring_create_full(). It
 * uses the default memory management mechanisms. It gives no error buffer,
 * because it expects success.
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
 * This macro frees the internal buffer and the container. It sets the pointer
 * to NULL after the destruction. This is why a second call to this macro is
 * safe.
 *
 * @param s  String variable to destroy (set to NULL on return). It must be a
 * modifiable lvalue, such as a variable or an element of a vector. The macro
 * evaluates it exactly once.
 */
#define cstring_destroy(s)    \
  _ccol_cstring_destroy_impl( \
      s, _ccol_uniq(__ccol_cstring_destroy_slot, __COUNTER__))

/* Internal. The body of cstring_destroy. slot is a name from _ccol_uniq(),
 * so the macro nests inside the argument of another destroy macro and stays
 * -Wshadow clean. The argument is evaluated exactly once. */
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
 * The program calls this function automatically when a scoped cstr variable
 * leaves its scope.
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
 * The function moves the characters at the positions >= @p pos to the right
 * to make room.
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
 * program cannot reach this. It needs a str that fills the full address
 * space.
 * @return ccol_not_enough_memory if reallocation fails
 * @note   Will assert if s is NULL
 */
ccol_retval_t cstring_set(cstr s, const char *str);

/**
 * @brief Clear all characters and shrink capacity back to the minimum
 *
 * If the reallocation that shrinks the buffer fails, the capacity does not
 * change. The function always resets the length to 0.
 *
 * @param s  String to reset
 * @note     Will assert if s is NULL
 */
void cstring_reset(cstr s);

/**
 * @brief Pre-allocate capacity for at least @p min_capacity bytes
 *
 * This function rounds @p min_capacity up to the nearest power of two. The
 * smallest capacity is 16. If the current capacity is already enough, the
 * function does nothing and returns true.
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
 * @brief Convert all characters to uppercase in-place
 *
 * @param s  String to change
 * @note     Will assert if s is NULL
 */
void cstring_to_upper(cstr s);

/**
 * @brief Convert all characters to lowercase in-place
 *
 * @param s  String to change
 * @note     Will assert if s is NULL
 */
void cstring_to_lower(cstr s);

/**
 * @brief Strip the whitespace at the start and at the end, in-place
 *
 * This function uses isspace() to classify whitespace.
 *
 * @param s  String to change
 * @note     Will assert if s is NULL
 */
void cstring_trim(cstr s);

/**
 * @brief Replace every non-overlapping occurrence of @p needle with
 * @p replacement
 *
 * The function builds the result in a new buffer. Then it swaps that buffer
 * in. This is why @p replacement can contain @p needle. Such a replacement
 * does not cause an infinite loop.
 *
 * @param s            String to change
 * @param needle       Substring to search for (must not be NULL or empty)
 * @param replacement  Substitute string (must not be NULL)
 *
 * @return ccol_success on success. This includes the case where the function
 * does not find needle.
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
 * The semantics are identical to strcmp().
 *
 * @param s    String to compare
 * @param str  C string to compare against
 *
 * @return Negative / zero / positive if s < / == / > str
 * @return 1 if str is NULL. The library treats a non-null string as the
 * greater one.
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
 * @return cstring_length(s) if @p needle is an empty string. This is the
 * past-the-end position. It is the same convention as the one that
 * std::string::rfind("") uses in C++.
 * @return ccol_invalid_size if not found, or if @p needle is NULL
 * @note   Will assert if s is NULL
 * @note   Occurrences that overlap count: the last "aba" in "ababa" starts
 * at index 2
 * @note   For a string of 256 bytes or more, the search runs from the end of
 * the string and stops at the first match that it meets, and its time is
 * linear in the length of the string plus the length of @p needle, whatever
 * the two hold. A shorter string is searched from its start, which is faster
 * at that length.
 * @note   A needle of up to 128 bytes needs no memory. A longer needle can
 * make the search take one temporary table of one size_t per byte of the
 * needle from the allocator of s, and free it before the call returns. When
 * that allocator refuses the table, the search still gives the same answer,
 * but its time can then grow with the product of the two lengths.
 */
size_t cstring_rfind(cstr s, const char *needle);

/* ========================================================================== */
/*                    SUBSTRING, COPY AND SPLIT                               */
/* ========================================================================== */

/**
 * @brief Create a new cstring holding a sub-range of this string
 *
 * The function copies the characters in the range [@p start, @p start +
 * @p length) into a new cstring. If @p start + @p length goes past the end
 * of the string, the function clamps the range to the end. If @p start is at
 * the length of the string or past it, the function returns an empty
 * cstring.
 *
 * @param s       Source string
 * @param start   Zero-based start index
 * @param length  Number of characters to include
 * @param err     Optional pointer to receive an error string on failure
 *
 * @return New cstring, or NULL on allocation failure
 * @note   The caller must destroy the cstring that this function returns
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
 * @note   The caller must destroy the cstring that this function returns
 * @note   Will assert if s is NULL
 */
cstr cstring_copy(cstr s, char **err);

/**
 * @brief Split the string by @p delimiter and return the parts as a vector
 *
 * The function puts each token between the occurrences of @p delimiter into
 * the cvec that it returns. Each token becomes a new cstr. An empty token at
 * the start or at the end is also a token. You get such a token when the
 * string starts with the delimiter or ends with it.
 *
 * @param s          String to split
 * @param delimiter  Separator string (must not be NULL or empty)
 * @param err        Optional pointer to receive an error string on failure
 *
 * @return cvec that holds the tokens, or NULL on failure. Its elem_size is
 * sizeof(cstr).
 *
 * @note The caller must destroy every cstr in the vector. The caller must
 * destroy the vector too. For example:
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
 * The program calls __cstring_destroy automatically when the variable leaves
 * its scope.
 *
 * @param s  Name of the variable to declare
 */
#define cstr_declare_scoped(s) \
  cstr s _ccol_destructor(___cstring_destroy) = NULL

/**
 * @brief Initialise a declared cstr with the default allocators
 *
 * This macro calls ccol_fatal_err() if the creation fails.
 *
 * @param s        cstr lvalue to initialise (you must declare it first). The
 *                 macro assigns it once, after the string is created.
 * @param initial  Initial content (C string or NULL for empty)
 */
#define cstr_init(s, initial)                                                  \
  do {                                                                         \
    char *__cstr_init_err = NULL;                                              \
    __typeof__(s) __cstr_init_s = cstring_create((initial), &__cstr_init_err); \
    if (!__cstr_init_s) {                                                      \
      ccol_fatal_err("cstr_init('%s'): %s", #s,                                \
                     __cstr_init_err ? __cstr_init_err : "unknown error");     \
    }                                                                          \
    (s) = __cstr_init_s;                                                       \
  } while (0)

/**
 * @brief Initialise a declared cstr with custom memory management
 *
 * This macro calls ccol_fatal_err() if the creation fails.
 *
 * @param s        cstr lvalue to initialise (you must declare it first). The
 *                 macro assigns it once, after the string is created.
 * @param initial  Initial content (C string or NULL for empty)
 * @param mprocs   Custom memory management procedures
 */
#define cstr_init_mp(s, initial, mprocs)                               \
  do {                                                                 \
    char *__cstr_init_mp_err = NULL;                                   \
    __typeof__(s) __cstr_init_mp_s =                                   \
        cstring_create_full((initial), (mprocs), &__cstr_init_mp_err); \
    if (!__cstr_init_mp_s) {                                           \
      ccol_fatal_err(                                                  \
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
 * @param s  cstr lvalue to destroy. The macro evaluates it exactly once.
 */
#define cstr_destroy(s) cstring_destroy((s))

/* ========================================================================== */
/*                         OPERATION MACROS                                   */
/* ========================================================================== */

/* Every retval local below has a name that belongs to one macro only and
 * that starts with two underscores. The shorter name '_r' is not safe here.
 * Each macro declares its local in one statement. The initializer of that
 * statement holds the arguments of the caller, because the macro substitutes
 * them there. The declarator-scope rule of C says that the scope of an
 * identifier begins directly after its own declarator. The scope therefore
 * begins before the compiler evaluates the initializer. This is the same
 * rule that makes `int x = x;` a self-reference and not a copy of an outer
 * x. A caller can give one of its own arguments the same name as the local.
 * That argument then binds to the not-yet-initialized local of this macro
 * and not to the argument of the caller, and no tool reports it. For
 * cstr_append, cstr_prepend, cstr_set and cstr_replace, every shadowed
 * parameter is a `const char *`. Such a collision with a short, plausible
 * name like '_r' therefore fails to compile, because the code puts an enum
 * where a pointer belongs. The `pos` parameter of cstr_insert is a size_t. A
 * size_t takes an enum value silently, and no compiler and no set of warning
 * flags must report this. That collision therefore compiles, and the caller
 * gets a wrong value. A name this specific to one macro is the only
 * practical fix for a C macro, because a C macro is not hygienic. */

/** @brief Append @p str. This macro calls ccol_fatal_err() on failure */
#define cstr_append(s, str)                                                \
  do {                                                                     \
    ccol_retval_t __cstr_append_r = cstring_append((s), (str));            \
    if (__cstr_append_r != ccol_success) {                                 \
      ccol_fatal_err("cstr_append('%s'): r: %d (%s)", #s, __cstr_append_r, \
                     ccol_retval_to_str(__cstr_append_r));                 \
    }                                                                      \
  } while (0)

/** @brief Prepend @p str. This macro calls ccol_fatal_err() on failure */
#define cstr_prepend(s, str)                                                 \
  do {                                                                       \
    ccol_retval_t __cstr_prepend_r = cstring_prepend((s), (str));            \
    if (__cstr_prepend_r != ccol_success) {                                  \
      ccol_fatal_err("cstr_prepend('%s'): r: %d (%s)", #s, __cstr_prepend_r, \
                     ccol_retval_to_str(__cstr_prepend_r));                  \
    }                                                                        \
  } while (0)

/** @brief Insert @p str at @p pos. This macro calls ccol_fatal_err() on
 * failure */
#define cstr_insert(s, pos, str)                                           \
  do {                                                                     \
    ccol_retval_t __cstr_insert_r = cstring_insert((s), (pos), (str));     \
    if (__cstr_insert_r != ccol_success) {                                 \
      ccol_fatal_err("cstr_insert('%s'): r: %d (%s)", #s, __cstr_insert_r, \
                     ccol_retval_to_str(__cstr_insert_r));                 \
    }                                                                      \
  } while (0)

/** @brief Replace the full content with @p str. This macro calls
 * ccol_fatal_err() on failure */
#define cstr_set(s, str)                                             \
  do {                                                               \
    ccol_retval_t __cstr_set_r = cstring_set((s), (str));            \
    if (__cstr_set_r != ccol_success) {                              \
      ccol_fatal_err("cstr_set('%s'): r: %d (%s)", #s, __cstr_set_r, \
                     ccol_retval_to_str(__cstr_set_r));              \
    }                                                                \
  } while (0)

/** @brief Replace all the occurrences of @p needle. This macro calls
 * ccol_fatal_err() on failure */
#define cstr_replace(s, needle, replacement)                                 \
  do {                                                                       \
    ccol_retval_t __cstr_replace_r =                                         \
        cstring_replace((s), (needle), (replacement));                       \
    if (__cstr_replace_r != ccol_success) {                                  \
      ccol_fatal_err("cstr_replace('%s'): r: %d (%s)", #s, __cstr_replace_r, \
                     ccol_retval_to_str(__cstr_replace_r));                  \
    }                                                                        \
  } while (0)

/** @brief Reserve at least @p cap bytes. This macro calls ccol_fatal_err()
 * on failure. It evaluates @p cap exactly once, so the failure message
 * reports the capacity that the call used. */
#define cstr_reserve(s, cap)        \
  _ccol_cstr_reserve_impl(s, (cap), \
                          _ccol_uniq(__ccol_cstr_reserve_cap, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
#define _ccol_cstr_reserve_impl(s, cap, __cstr_reserve_cap)                   \
  do {                                                                        \
    size_t __cstr_reserve_cap = (cap);                                        \
    if (!cstring_reserve((s), __cstr_reserve_cap)) {                          \
      ccol_fatal_err(                                                         \
          "cstr_reserve('%s'): failed to reserve %zu bytes (out of memory?)", \
          #s, __cstr_reserve_cap);                                            \
    }                                                                         \
  } while (0)

/** @brief Create a new cstring that holds the content of [start,start+len)
 * of the cstring s. This macro calls ccol_fatal_err() on failure. It
 * evaluates each argument exactly once, so the failure message reports the
 * start and the length that the call used. */
#define cstr_substring(s, start, len)                                          \
  _ccol_cstr_substring_impl(                                                   \
      s, (start), (len), _ccol_uniq(__ccol_cstr_substring_start, __COUNTER__), \
      _ccol_uniq(__ccol_cstr_substring_len, __COUNTER__),                      \
      _ccol_uniq(__ccol_cstr_substring_err, __COUNTER__),                      \
      _ccol_uniq(__ccol_cstr_substring_result, __COUNTER__))

/* The public macro above names every temporary of this body with
 * _ccol_uniq(), so the macro nests inside the argument of any
 * public macro, itself included. */
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
      ccol_fatal_err(                                                         \
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

/* ========================================================================== */
/*                         UNIT TEST INTERNALS                                */
/* ========================================================================== */

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Expose the internal capacity in bytes, for the tests
 *
 * @param s  String to query
 * @return   Number of bytes that the data buffer holds at this moment
 */
size_t cstring_get_capacity(cstr s);

/**
 * @brief Expose the internal guard against a wraparound of a size_t, for the
 * tests
 *
 * Every function that changes a string uses this guard. The guard rejects a
 * length of exactly SIZE_MAX. Without the guard, that length wraps silently
 * to 0 when the code adds 1 for the null terminator. This declaration lets a
 * test drive the guard directly. The test then needs no string that fills
 * the full address space to reach the guard through the public API.
 *
 * @param length  Candidate content length, in bytes, without the '\0'
 * @return        true if length + 1 fits in a size_t, false if length is
 * exactly SIZE_MAX
 */
bool cstring_length_fits_with_terminator_for_tests(size_t length);

/**
 * @brief Expose the length arithmetic of cstring_replace(), which checks for
 * an overflow, for the tests
 *
 * This declaration lets a test drive the ccol_container_full overflow guards
 * of cstring_replace() directly. The test then needs no real string of many
 * gigabytes to reach those guards through the public API.
 *
 * @param orig_length  Length of the string before the replace
 * @param nlen         Needle length
 * @param rlen         Replacement length
 * @param count        Number of non-overlapping occurrences (must be > 0)
 * @param new_len_out  Receives the new length on ccol_success
 *
 * @return ccol_success on success
 * @return ccol_container_full if the computation overflows a size_t
 */
ccol_retval_t cstring_replace_compute_new_length_for_tests(size_t orig_length,
                                                           size_t nlen,
                                                           size_t rlen,
                                                           size_t count,
                                                           size_t *new_len_out);
#endif

#pragma GCC visibility pop
