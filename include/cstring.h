/*
MIT License

Copyright (c) 2024 A bunch of nerds

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

#include <cvector.h>

/**
 * @file cstring.h
 * @brief Dynamic string container with a rich set of string operations
 *
 * Provides a heap-allocated, automatically resizing string container.
 * The internal buffer always holds a null-terminated C string and is
 * kept at a power-of-two capacity (minimum 16 bytes).
 *
 * Key features:
 * - Automatic capacity management (grows to next power of two when needed)
 * - Full set of common string operations (append, prepend, insert, find, etc.)
 * - Custom memory management support
 * - RAII-style automatic destruction via _ccol_destructor attribute
 * - Split returns a cvec of cstr for easy iteration
 *
 * Integration with maps (chashmap / cbstmap):
 * cstr is NOT a recognised key or value type in the map containers. Those
 * containers understand char * keys natively (content-based hashing, SSO).
 * Use cstring_c_str() to obtain a char * view and pass that as the key:
 *
 *     char *k = (char *)cstring_c_str(my_cstr);
 *     chmap_insert(map, k, value);
 *
 * The map copies the string content immediately, so the cstr can be
 * mutated or destroyed afterwards without affecting the stored entry.
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
 * Allocates and initialises a new cstring. If @p initial is non-NULL its
 * content is copied in; passing NULL creates an empty string.  The internal
 * buffer capacity is rounded up to the nearest power of two (minimum 16).
 *
 * @param initial  Initial C string content, or NULL for an empty string
 * @param m_procs  Custom memory management procedures, or NULL for default
 * @param err      Optional pointer to receive an error string on failure
 *
 * @return Pointer to newly created string, or NULL on failure
 *
 * @see cstring_create
 * @see cstring_destroy
 */
cstr cstring_create_full(const char *initial, ccol_memmgmt_procs_t *m_procs,
                         char **err);

/**
 * @brief Create a string with default memory management
 *
 * Convenience wrapper around cstring_create_full() that uses the default
 * malloc/free allocators.
 *
 * @param initial  Initial C string content, or NULL for an empty string
 * @param err      Optional pointer to receive an error string on failure
 *
 * @return Pointer to newly created string, or NULL on failure
 */
static inline __attribute__((always_inline)) cstr
cstring_create(const char *initial, char **err) {
  return cstring_create_full(initial, NULL, err);
}

/**
 * @brief Get the memory management procedures for a string
 *
 * @param s  String to query
 * @return   Pointer to memory management procedures, or NULL if using defaults
 */
ccol_memmgmt_procs_t *cstring_get_mprocs(cstr s);

/**
 * @brief Destroy a string (internal – do not call directly)
 *
 * @param s  String to destroy
 * @warning  Use the cstring_destroy() macro instead
 */
void __cstring_destroy(cstr s);

/**
 * @brief Destroy a string and set its pointer to NULL
 *
 * Frees the internal buffer and the container itself. The pointer is set to
 * NULL after destruction so double-free is safe.
 *
 * @param s  String variable to destroy (set to NULL on return)
 */
#define cstring_destroy(s)  \
  do {                      \
    if (s) {                \
      __cstring_destroy(s); \
      s = NULL;             \
    }                       \
  } while (0)

/**
 * @brief Cleanup helper for _ccol_destructor (RAII)
 *
 * Called automatically when a scoped cstr variable leaves scope.
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
 * @brief Return the number of characters in the string (excluding '\0')
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
 * @warning  Pointer becomes invalid after any mutating operation
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
 * @param s    String to modify
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
 * @param s    String to modify
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
 * Characters at positions >= @p pos are shifted right to make room.
 *
 * @param s    String to modify
 * @param pos  Insertion position (0 … length, inclusive)
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
 * @param s    String to modify
 * @param str  New content (must not be NULL)
 *
 * @return ccol_success on success
 * @return ccol_invalid_args if str is NULL
 * @return ccol_not_enough_memory if reallocation fails
 * @note   Will assert if s is NULL
 */
ccol_retval_t cstring_set(cstr s, const char *str);

/**
 * @brief Clear all characters and shrink capacity back to the minimum
 *
 * If the reallocation to shrink the buffer fails, the capacity is left
 * unchanged; the length is always reset to 0 regardless.
 *
 * @param s  String to reset
 * @note     Will assert if s is NULL
 */
void cstring_reset(cstr s);

/**
 * @brief Pre-allocate capacity for at least @p min_capacity bytes
 *
 * Rounds @p min_capacity up to the nearest power of two (minimum 16).
 * Does nothing and returns true if current capacity is already sufficient.
 *
 * @param s             String to reserve capacity for
 * @param min_capacity  Minimum number of bytes to reserve (including '\0')
 *
 * @return true on success, false if reallocation fails or size too large
 * @note   Will assert if s is NULL
 */
bool cstring_reserve(cstr s, size_t min_capacity);

/**
 * @brief Convert all characters to uppercase in-place
 *
 * @param s  String to modify
 * @note     Will assert if s is NULL
 */
void cstring_to_upper(cstr s);

/**
 * @brief Convert all characters to lowercase in-place
 *
 * @param s  String to modify
 * @note     Will assert if s is NULL
 */
void cstring_to_lower(cstr s);

/**
 * @brief Strip leading and trailing whitespace in-place
 *
 * Uses isspace() to classify whitespace.
 *
 * @param s  String to modify
 * @note     Will assert if s is NULL
 */
void cstring_trim(cstr s);

/**
 * @brief Replace every non-overlapping occurrence of @p needle with
 * @p replacement
 *
 * Builds the result into a fresh buffer then swaps it in, so @p replacement
 * may safely contain @p needle without causing infinite loops.
 *
 * @param s            String to modify
 * @param needle       Substring to search for (must not be NULL or empty)
 * @param replacement  Substitute string (must not be NULL)
 *
 * @return ccol_success on success (including when needle is not found)
 * @return ccol_invalid_args if needle is NULL/empty or replacement is NULL
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
 * Semantics identical to strcmp().
 *
 * @param s    String to compare
 * @param str  C string to compare against (must not be NULL)
 *
 * @return Negative / zero / positive if s < / == / > str
 * @note   Will assert if s is NULL
 */
int cstring_compare(cstr s, const char *str);

/**
 * @brief Check whether the string's content equals a C string
 *
 * @param s    String to compare
 * @param str  C string to compare against
 *
 * @return true if contents are equal, false otherwise
 * @note   Will assert if s is NULL
 */
bool cstring_equals(cstr s, const char *str);

/**
 * @brief Check whether the string begins with @p prefix
 *
 * @param s       String to test
 * @param prefix  Prefix to look for (must not be NULL)
 *
 * @return true if the string starts with @p prefix
 * @note   An empty prefix always matches
 * @note   Will assert if s is NULL
 */
bool cstring_starts_with(cstr s, const char *prefix);

/**
 * @brief Check whether the string ends with @p suffix
 *
 * @param s       String to test
 * @param suffix  Suffix to look for (must not be NULL)
 *
 * @return true if the string ends with @p suffix
 * @note   An empty suffix always matches
 * @note   Will assert if s is NULL
 */
bool cstring_ends_with(cstr s, const char *suffix);

/**
 * @brief Find the first occurrence of @p needle
 *
 * @param s       String to search
 * @param needle  Substring to find (must not be NULL)
 *
 * @return Zero-based index of the first occurrence, or ccol_invalid_size if
 * not found
 * @note   Will assert if s is NULL
 */
size_t cstring_find(cstr s, const char *needle);

/**
 * @brief Find the last occurrence of @p needle
 *
 * @param s       String to search
 * @param needle  Substring to find (must not be NULL)
 *
 * @return Zero-based index of the last occurrence, or ccol_invalid_size if
 * not found
 * @note   Will assert if s is NULL
 */
size_t cstring_rfind(cstr s, const char *needle);

/* ========================================================================== */
/*                    SUBSTRING, COPY AND SPLIT                               */
/* ========================================================================== */

/**
 * @brief Create a new cstring holding a sub-range of this string
 *
 * Characters in the range [@p start, @p start + @p length) are copied into
 * a freshly allocated cstring. If @p start + @p length extends beyond the
 * end of the string, the range is clamped to the end. If @p start is at or
 * beyond the string length, an empty cstring is returned.
 *
 * @param s       Source string
 * @param start   Zero-based start index
 * @param length  Number of characters to include
 * @param err     Optional pointer to receive an error string on failure
 *
 * @return New cstring, or NULL on allocation failure
 * @note   Caller must destroy the returned cstring when done
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
 * @note   Caller must destroy the returned cstring when done
 * @note   Will assert if s is NULL
 */
cstr cstring_copy(cstr s, char **err);

/**
 * @brief Split the string by @p delimiter and return the parts as a vector
 *
 * Each token between occurrences of @p delimiter (including empty tokens at
 * the start/end if the string begins/ends with the delimiter) is placed as a
 * newly allocated cstr into the returned cvec.
 *
 * @param s          String to split
 * @param delimiter  Separator string (must not be NULL or empty)
 * @param err        Optional pointer to receive an error string on failure
 *
 * @return cvec (of elem_size == sizeof(cstr)) containing the tokens, or NULL
 * on failure
 *
 * @note The caller is responsible for destroying every cstr inside the vector
 * as well as the vector itself, e.g.:
 * @code
 * cvec parts = cstring_split(s, ",", NULL);
 * cvec_redeclare(parts, cstr);
 * for (size_t i = 0; i < cvec_size(parts); i++) {
 *   cstr_destroy(cvec_at(parts, i));
 * }
 * cvec_destroy(parts);
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
 * When the variable leaves scope __cstring_destroy is called automatically.
 *
 * @param s  Name of the variable to declare
 */
#define cstr_declare_scoped(s) cstr s _ccol_destructor(___cstring_destroy)

/**
 * @brief Initialise a declared cstr using default allocators
 *
 * Calls fatal_err() if creation fails.
 *
 * @param s        cstr variable to initialise (must have been declared)
 * @param initial  Initial content (C string or NULL for empty)
 */
#define cstr_init(s, initial)                             \
  do {                                                    \
    char *_cstr_err = NULL;                               \
    s = cstring_create((initial), &_cstr_err);            \
    if (!s) {                                             \
      fatal_err("cstring_create failed: %s",              \
                _cstr_err ? _cstr_err : "unknown error"); \
    }                                                     \
  } while (0)

/**
 * @brief Initialise a declared cstr using custom memory management
 *
 * Calls fatal_err() if creation fails.
 *
 * @param s        cstr variable to initialise (must have been declared)
 * @param initial  Initial content (C string or NULL for empty)
 * @param mprocs   Custom memory management procedures
 */
#define cstr_init_mp(s, initial, mprocs)                      \
  do {                                                        \
    char *_cstr_err = NULL;                                   \
    s = cstring_create_full((initial), (mprocs), &_cstr_err); \
    if (!s) {                                                 \
      fatal_err("cstring_create_full failed: %s",             \
                _cstr_err ? _cstr_err : "unknown error");     \
    }                                                         \
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
 * @brief Destroy a cstr and set it to NULL (type-safe wrapper)
 *
 * @param s  cstr variable to destroy
 */
#define cstr_destroy(s) cstring_destroy(s)

/* ========================================================================== */
/*                         OPERATION MACROS                                   */
/* ========================================================================== */

/** @brief Append @p str; calls fatal_err() on failure */
#define cstr_append(s, str)                        \
  do {                                             \
    ccol_retval_t _r = cstring_append((s), (str)); \
    if (_r != ccol_success) {                      \
      fatal_err("cstring_append failed: %d", _r);  \
    }                                              \
  } while (0)

/** @brief Prepend @p str; calls fatal_err() on failure */
#define cstr_prepend(s, str)                        \
  do {                                              \
    ccol_retval_t _r = cstring_prepend((s), (str)); \
    if (_r != ccol_success) {                       \
      fatal_err("cstring_prepend failed: %d", _r);  \
    }                                               \
  } while (0)

/** @brief Insert @p str at @p pos; calls fatal_err() on failure */
#define cstr_insert(s, pos, str)                          \
  do {                                                    \
    ccol_retval_t _r = cstring_insert((s), (pos), (str)); \
    if (_r != ccol_success) {                             \
      fatal_err("cstring_insert failed: %d", _r);         \
    }                                                     \
  } while (0)

/** @brief Replace entire content with @p str; calls fatal_err() on failure */
#define cstr_set(s, str)                        \
  do {                                          \
    ccol_retval_t _r = cstring_set((s), (str)); \
    if (_r != ccol_success) {                   \
      fatal_err("cstring_set failed: %d", _r);  \
    }                                           \
  } while (0)

/** @brief Replace all occurrences of @p needle; calls fatal_err() on failure */
#define cstr_replace(s, needle, replacement)                          \
  do {                                                                \
    ccol_retval_t _r = cstring_replace((s), (needle), (replacement)); \
    if (_r != ccol_success) {                                         \
      fatal_err("cstring_replace failed: %d", _r);                    \
    }                                                                 \
  } while (0)

/** @brief Reserve at least @p cap bytes; calls fatal_err() on failure */
#define cstr_reserve(s, cap)                                              \
  do {                                                                    \
    if (!cstring_reserve((s), (cap))) {                                   \
      fatal_err("cstring_reserve failed - %p - %zu", (s), (size_t)(cap)); \
    }                                                                     \
  } while (0)

/** @brief Create a new cstring that contains the content of [start,start+len)
 * of the cstring s; calls fatal_err() on failure */
#define cstr_substring(s, start, len)                             \
  ({                                                              \
    char *_cstr_sub_err = NULL;                                   \
    cstr _cstr_sub_result =                                       \
        cstring_substring((s), (start), (len), &_cstr_sub_err);   \
    if (!_cstr_sub_result) {                                      \
      fatal_err("cstring_substring failed: %s",                   \
                _cstr_sub_err ? _cstr_sub_err : "unknown error"); \
    }                                                             \
    _cstr_sub_result;                                             \
  })

/* Simple pass-through wrappers */
#define cstr_length(s) cstring_length(s)
#define cstr_c_str(s) cstring_c_str(s)
#define cstr_at(s, idx) cstring_at((s), (idx))
#define cstr_is_empty(s) cstring_is_empty(s)
#define cstr_reset(s) cstring_reset(s)
#define cstr_to_upper(s) cstring_to_upper(s)
#define cstr_to_lower(s) cstring_to_lower(s)
#define cstr_trim(s) cstring_trim(s)
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
 * @brief Expose the internal allocated capacity (bytes) for testing
 *
 * @param s  String to query
 * @return   Number of bytes currently allocated for the data buffer
 */
size_t cstring_get_capacity(cstr s);
#endif
