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

#include <common.h>

/**
 * @file cyaml.h
 * @brief YAML 1.2 parser, serializer, and mutable in-memory DOM.
 *
 * The DOM mirrors the cjson module design: every node is a heap-allocated
 * opaque struct; lists are backed by cvector, dictionaries by chashmap.
 *
 * ### Supported YAML features
 *
 * - Block and flow dictionaries and lists.
 * - All scalar styles: plain, single-quoted, double-quoted, literal (|),
 *   folded (>), with all chomping modes (clip, strip, keep).
 * - Anchors (&name) and aliases (*name).
 * - Implicit core-schema typing: null, bool, integer (decimal/octal/hex),
 *   float (including .inf and .nan).
 * - Multi-document streams delimited by "---".  The leading "---" is optional
 *   for single-document inputs.  Multiple documents are returned as a
 *   CYAML_LIST (one element per document root); a single document is returned
 *   as its root node directly.
 *
 * ### Unsupported / explicitly out-of-scope
 *
 * - YAML tags (!!, custom tags) -- tags are silently ignored.
 * - Merge keys (<<:).
 * - Multi-line plain scalars: plain scalars are terminated at end-of-line.
 *   Use literal (|) or folded (>) block scalars for multiline string values.
 * - Tab characters for block indentation (forbidden by the YAML spec).
 * - %YAML and %TAG directives -- silently ignored.
 * - Null bytes inside strings: the double-quoted escape "\0" is accepted by
 *   the parser but the resulting null byte terminates the stored C string,
 *   silently discarding everything after it.  String values must not contain
 *   embedded null bytes.
 *
 * ### Implementation-defined behavior
 *
 * - Duplicate mapping keys: when a mapping contains the same key more than
 *   once, the last value wins and earlier values are silently replaced.
 *   This behavior is consistent with common YAML parsers but is not
 *   mandated by the YAML 1.2 specification.  The application is responsible
 *   for ensuring input does not contain unintentional duplicate keys.
 *
 * ### Custom memory management
 *
 * Every factory function and the parser accept a ccol_memmgmt_procs_t *mp
 * parameter.  Pass NULL to use the default malloc/free/calloc/realloc.
 * The allocator is stored per-node.  All nodes in a tree are expected to
 * carry the same allocator.
 *
 * **Thread-local node pool:** A thread-local free-list pool of up to 512
 * nodes accelerates the default-allocator case.  Custom-allocator nodes
 * bypass the pool.  The pool drains automatically at thread exit.
 *
 * ### Path syntax (cyaml_get / cyaml_set)
 *
 * Identical to cjson: dot-separated components, e.g. "server.hosts.#0.port".
 * A component beginning with '#' followed by digits indexes into a list
 * when the current node is CYAML_LIST; otherwise the full component
 * (including '#') is used as a literal dictionary key.
 *
 * ### Ownership
 *
 * - cyaml_create_*(), cyaml_parse*(), and cyaml_clone() return fully owned
 *   trees.
 * - cyaml_list_push() and cyaml_dictionary_set() transfer ownership of the
 *   child to the parent; do not free it afterwards.
 * - cyaml_get() returns a NON-OWNING reference valid until the tree is
 *   mutated or destroyed.
 * - cyaml_destroy() recursively frees the entire subtree and NULLs the
 *   handle.
 *
 * ### Serialization
 *
 * cyaml_serialize() produces block-style YAML.
 * cyaml_serialize_flow() produces compact single-line flow-style YAML.
 * The returned buffer is allocated with the root node's allocator; free it
 * with cyaml_serialize_free() (passing the same mp used at parse/create time,
 * or NULL for the default allocator).
 */

/* ========================================================================== */
/*                         NODE TYPE TAG                                      */
/* ========================================================================== */

/**
 * @brief YAML value kind tag.
 */
typedef enum cyaml_node_type {
  CYAML_NULL = 0,   /**< YAML null  (~, null, Null, NULL, or empty scalar) */
  CYAML_BOOL,       /**< YAML boolean (true/True/TRUE, false/False/FALSE)   */
  CYAML_INTEGER,    /**< Integer scalar (decimal, 0o octal, 0x hex)        */
  CYAML_FLOAT,      /**< Floating-point scalar (including .inf and .nan)   */
  CYAML_STRING,     /**< String scalar (any style; owned heap copy)        */
  CYAML_LIST,       /**< Ordered list of child nodes                       */
  CYAML_DICTIONARY, /**< String-keyed dictionary of child nodes               */
} cyaml_node_type_t;

/* ========================================================================== */
/*                         OPAQUE HANDLE                                      */
/* ========================================================================== */

/** @brief Opaque DOM node type.  Full definition lives in cyaml.c. */
typedef struct cyaml_node_t cyaml_node_t;

/** @brief Public handle: pointer to an opaque DOM node. */
typedef cyaml_node_t *cyaml;

/* ========================================================================== */
/*                         NODE CONSTRUCTION                                  */
/* ========================================================================== */

/**
 * @brief Allocate and return a YAML null node.
 * @param mp  Custom allocator, or NULL for default.
 */
cyaml cyaml_create_null_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a YAML null node (default allocator). */
static inline cyaml cyaml_create_null(void) {
  return cyaml_create_null_mp(NULL);
}

/**
 * @brief Allocate and return a YAML boolean node.
 * @param val  C boolean value.
 * @param mp   Custom allocator, or NULL for default.
 */
cyaml cyaml_create_bool_mp(bool val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a YAML boolean node (default allocator). */
static inline cyaml cyaml_create_bool(bool val) {
  return cyaml_create_bool_mp(val, NULL);
}

/**
 * @brief Allocate and return a YAML integer node (stored as long long).
 * @param val  Integer value.
 * @param mp   Custom allocator, or NULL for default.
 */
cyaml cyaml_create_int_mp(long long val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a YAML integer node (default allocator). */
static inline cyaml cyaml_create_int(long long val) {
  return cyaml_create_int_mp(val, NULL);
}

/**
 * @brief Allocate and return a YAML floating-point node.
 *
 * Unlike cjson, YAML explicitly supports .inf and .nan; non-finite values
 * are stored and serialized as .inf / -.inf / .nan.
 *
 * @param val  Double value.
 * @param mp   Custom allocator, or NULL for default.
 */
cyaml cyaml_create_double_mp(double val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a YAML floating-point node (default allocator).
 */
static inline cyaml cyaml_create_double(double val) {
  return cyaml_create_double_mp(val, NULL);
}

/**
 * @brief Allocate and return a YAML string node.
 * @param val  Null-terminated string; an owned copy is made.
 *             NULL produces a CYAML_NULL node.
 * @param mp   Custom allocator, or NULL for default.
 */
cyaml cyaml_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a YAML string node (default allocator). */
static inline cyaml cyaml_create_string(const char *val) {
  return cyaml_create_string_mp(val, NULL);
}

/**
 * @brief Allocate and return an empty YAML list node.
 * @param mp  Custom allocator, or NULL for default.
 */
cyaml cyaml_create_list_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return an empty YAML list node (default allocator).
 */
static inline cyaml cyaml_create_list(void) {
  return cyaml_create_list_mp(NULL);
}

/**
 * @brief Allocate and return an empty YAML dictionary node.
 * @param mp  Custom allocator, or NULL for default.
 */
cyaml cyaml_create_dictionary_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return an empty YAML dictionary node (default
 * allocator).
 */
static inline cyaml cyaml_create_dictionary(void) {
  return cyaml_create_dictionary_mp(NULL);
}

/* ========================================================================== */
/*                         PARSING                                            */
/* ========================================================================== */

/**
 * @brief Parse a null-terminated YAML string into a DOM tree.
 *
 * @param yaml_str  Input text (must be null-terminated).
 * @param err_str   Out-parameter for the error message on parse failure.
 *                  On success *err_str is set to NULL.  On failure a
 *                  heap-allocated string is stored; the caller must free() it.
 *                  Pass NULL to ignore error details.
 * @param mp        Custom allocator for all nodes in the resulting tree,
 *                  or NULL for the default allocator.
 * @return Root cyaml node on success, NULL on parse failure.
 */
cyaml cyaml_parse_mp(const char *yaml_str, char **err_str,
                     ccol_memmgmt_procs_t *mp);

/** @brief Parse a null-terminated YAML string (default allocator). */
static inline cyaml cyaml_parse(const char *yaml_str, char **err_str) {
  return cyaml_parse_mp(yaml_str, err_str, NULL);
}

/**
 * @brief Parse a bounded YAML buffer (need not be null-terminated).
 *
 * @param yaml_str  Input buffer.
 * @param len       Number of bytes to parse.
 * @param err_str   Out-parameter for the error message (same semantics as
 *                  cyaml_parse_mp).  Pass NULL to ignore.
 * @param mp        Custom allocator, or NULL for default.
 * @return Root cyaml node, or NULL on failure.
 */
cyaml cyaml_parse_n_mp(const char *yaml_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp);

/** @brief Parse a bounded YAML buffer (default allocator). */
static inline cyaml cyaml_parse_n(const char *yaml_str, size_t len,
                                  char **err_str) {
  return cyaml_parse_n_mp(yaml_str, len, err_str, NULL);
}

/* ========================================================================== */
/*                         SERIALIZATION                                      */
/* ========================================================================== */

/**
 * @brief Serialize a DOM tree to block-style YAML.
 *
 * Scalars are emitted in the most readable plain form when safe, or
 * double-quoted otherwise. Lists use "- item" block syntax.
 * Dictionaries use "key: value" block syntax.
 *
 * The returned buffer is allocated with the root node's allocator.
 * Free it with cyaml_serialize_free() (or cyaml_serialize_free_mp() with
 * the same mp used to create the tree).
 *
 * @param node  Root of the (sub-)tree to serialize.
 * @return Heap-allocated null-terminated YAML string, or NULL on OOM.
 */
char *cyaml_serialize(cyaml node);

/**
 * @brief Serialize a DOM tree to compact flow-style YAML.
 *
 * Output resembles JSON: lists become [a, b, c], dictionaries become
 * {key: value}.  Useful for compact single-line representations.
 *
 * @param node  Root of the (sub-)tree to serialize.
 * @return Heap-allocated null-terminated string, or NULL on OOM.
 */
char *cyaml_serialize_flow(cyaml node);

/**
 * @brief Free a string returned by cyaml_serialize() or
 *        cyaml_serialize_flow().
 *
 * @param s   String to free (may be NULL).
 * @param mp  The same allocator that was active when the tree was created.
 *            Pass NULL when the default allocator was used.
 */
void cyaml_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp);

/** @brief Free a serializer string allocated with the default allocator. */
static inline void cyaml_serialize_free(char *s) {
  cyaml_serialize_free_mp(s, NULL);
}

/* ========================================================================== */
/*                         TYPE INSPECTION                                    */
/* ========================================================================== */

/**
 * @brief Return the type tag of a node.
 * @param node  May be NULL (returns CYAML_NULL).
 */
cyaml_node_type_t cyaml_type(cyaml node);

/** @brief Return a string literal naming the type of @p node. */
static inline const char *cyaml_type_str(cyaml node) {
  switch (cyaml_type(node)) {
    case CYAML_NULL:
      return "CYAML_NULL";
    case CYAML_BOOL:
      return "CYAML_BOOL";
    case CYAML_INTEGER:
      return "CYAML_INTEGER";
    case CYAML_FLOAT:
      return "CYAML_FLOAT";
    case CYAML_STRING:
      return "CYAML_STRING";
    case CYAML_LIST:
      return "CYAML_LIST";
    case CYAML_DICTIONARY:
      return "CYAML_DICTIONARY";
    default:
      return "CYAML_UNKNOWN";
  }
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/**
 * @brief Return the boolean value.
 * Calls fatal_err() if the node's type is not CYAML_BOOL.
 */
bool cyaml_bool_val(cyaml node);

/**
 * @brief Return the integer value.
 * Calls fatal_err() if the node's type is not CYAML_INTEGER.
 */
long long cyaml_int_val(cyaml node);

/**
 * @brief Return the floating-point value.
 * Calls fatal_err() if the node's type is not CYAML_FLOAT.
 */
double cyaml_double_val(cyaml node);

/**
 * @brief Return the string value (owned by the node; do not free).
 * Calls fatal_err() if the node's type is not CYAML_STRING.
 */
const char *cyaml_str_val(cyaml node);

/**
 * @brief Return the element count of a list node.
 * Calls fatal_err() if the node's type is not CYAML_LIST.
 */
size_t cyaml_list_len(cyaml node);

/**
 * @brief Return the key count of a dictionary node.
 * Calls fatal_err() if the node's type is not CYAML_DICTIONARY.
 */
size_t cyaml_dictionary_size(cyaml node);

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/**
 * @brief Append a child node to a list.
 *
 * Ownership of @p child transfers unconditionally: on success the child is
 * stored in the list; on any failure the child is deep-freed by this function.
 * Do not free @p child afterwards regardless of the return value.
 *
 * @param seq    Target list node (must be CYAML_LIST).
 * @param child  Child to append.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t cyaml_list_push(cyaml seq, cyaml child);

/**
 * @brief Access an element of a list by index (non-owning).
 * @return Child node, or NULL if index is out of bounds or type mismatch.
 */
cyaml cyaml_list_get(cyaml seq, size_t index);

/**
 * @brief Set (insert or replace) a key in a dictionary.
 *
 * Ownership of @p child transfers unconditionally: on success the child is
 * stored in the dictionary; on any failure the child is deep-freed by this
 * function.  Do not free @p child afterwards regardless of the return value.
 * If the key already exists the previous child is deep-freed before the new
 * one is stored.
 *
 * @param map    Target dictionary node (must be CYAML_DICTIONARY).
 * @param key    Null-terminated key string; a copy is stored internally.
 * @param child  Value node.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t cyaml_dictionary_set(cyaml map, const char *key, cyaml child);

/**
 * @brief Look up a key in a dictionary (non-owning).
 * @return Child node, or NULL if the key is absent or type mismatch.
 */
cyaml cyaml_dictionary_get(cyaml map, const char *key);

/**
 * @brief Remove and deep-free the element at position index from a list.
 *
 * All elements after index are shifted left by one position.  The removed
 * subtree is recursively freed.
 *
 * @param seq    Target list node (must be CYAML_LIST).
 * @param index  Zero-based index of the element to remove.
 * @return ccol_success on success.
 *         ccol_invalid_args if seq is NULL, not a list, or index is out of
 *         bounds.
 */
ccol_retval_t cyaml_list_remove(cyaml seq, size_t index);

/**
 * @brief Remove and deep-free the entry with the given key from a dictionary.
 *
 * The removed subtree is recursively freed.
 *
 * @param map  Target dictionary node (must be CYAML_DICTIONARY).
 * @param key  Null-terminated key string.
 * @return ccol_success on success.
 *         ccol_invalid_args if map is NULL or not a dictionary.
 *         ccol_key_not_found if key does not exist.
 */
ccol_retval_t cyaml_dictionary_remove(cyaml map, const char *key);

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/**
 * @brief Return a fully independent deep copy of a DOM subtree.
 *
 * The clone uses the same allocator as the source tree.
 *
 * @return New root node (caller owns it), or NULL on allocation failure.
 */
cyaml cyaml_clone(cyaml node);

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cyaml variable.
 *
 * Provided for naming-convention symmetry with other container modules.
 *
 * @param var_name  Variable name.
 *
 * Example:
 * @code
 * cyaml_declare(root);
 * root = cyaml_parse("key: value\n", NULL);
 * cyaml_destroy(root);
 * @endcode
 */
#define cyaml_declare(var_name) cyaml var_name

/**
 * @brief Declare a cyaml variable with automatic destruction on scope exit.
 *
 * @param var_name  Variable name.
 *
 * Example:
 * @code
 * {
 *   cyaml_declare_scoped(root) = cyaml_parse("key: value\n", NULL);
 *   // root is freed here automatically
 * }
 * @endcode
 */
#define cyaml_declare_scoped(var_name) \
  cyaml var_name _ccol_destructor(___cyaml_destroy)

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/** @brief Recursively free a DOM tree (internal -- prefer the macro). */
void __cyaml_destroy(cyaml node);

/** @brief RAII cleanup helper for use with _ccol_destructor. */
static inline void ___cyaml_destroy(cyaml *node) {
  if (node && *node) {
    __cyaml_destroy(*node);
    *node = NULL;
  }
}

/**
 * @brief Recursively free a DOM tree and NULL the handle.
 *
 * Safe to call on NULL.
 */
#define cyaml_destroy(node)  \
  do {                       \
    if (node) {              \
      __cyaml_destroy(node); \
      (node) = NULL;         \
    }                        \
  } while (0)

/* ========================================================================== */
/*                         PATH NAVIGATION -- BACK-END                       */
/* ========================================================================== */

/**
 * @brief Navigate to the node addressed by a dot-separated path (back-end).
 *
 * Prefer the cyaml_get() macro.
 *
 * @return Target node, or NULL if any component is missing / type mismatch.
 */
cyaml _cyaml_get(cyaml root, const char *path);

/**
 * @brief Remove and deep-free the node addressed by a path (back-end).
 *
 * Prefer the cyaml_delete() macro.
 *
 * Navigates to the parent of the addressed node and calls
 * cyaml_dictionary_remove() or cyaml_list_remove() as appropriate.
 * The path uses the same dot-separated syntax as _cyaml_get and
 * _cyaml_set_typed, including escape sequences.
 *
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL/empty path or wrong parent type.
 *         ccol_key_not_found if any path component is absent.
 *         ccol_not_enough_memory on allocation failure.
 */
ccol_retval_t _cyaml_delete(cyaml root, const char *path);

/**
 * @brief Write a typed scalar value to the leaf addressed by a path (back-end).
 *
 * Prefer the cyaml_set() macro.
 *
 * Creates the leaf if absent (parent must exist).  Replaces and deep-frees
 * any existing value, including full subtrees.
 *
 * @param root             Root of the DOM tree.
 * @param path             Dot-separated path string.
 * @param type             YAML type of the new value.
 * @param raw              Pointer to the raw C value (passed as void *).
 * @param raw_size         sizeof() the original C expression.
 * @param is_signed        Whether the integer source type is signed.
 * @param raw_is_char_array true when raw points directly at a char[] array.
 * @return ccol_success on success, error code otherwise.
 */
ccol_retval_t _cyaml_set_typed(cyaml root, const char *path,
                               cyaml_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed,
                               bool raw_is_char_array);

/* ========================================================================== */
/*                         COMPILE-TIME TYPE HELPERS                         */
/* ========================================================================== */

/**
 * @brief Map a C expression's compile-time type to cyaml_node_type_t.
 */
#if defined __clang__
#define _cyaml_type_of(val)                                                 \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    cyaml_node_type_t _cyt = _Generic((val),                                \
        bool: CYAML_BOOL,                                                   \
        char: CYAML_INTEGER,                                                \
        signed char: CYAML_INTEGER,                                         \
        short: CYAML_INTEGER,                                               \
        int: CYAML_INTEGER,                                                 \
        long: CYAML_INTEGER,                                                \
        long long: CYAML_INTEGER,                                           \
        unsigned char: CYAML_INTEGER,                                       \
        unsigned short: CYAML_INTEGER,                                      \
        unsigned int: CYAML_INTEGER,                                        \
        unsigned long: CYAML_INTEGER,                                       \
        unsigned long long: CYAML_INTEGER,                                  \
        float: CYAML_FLOAT,                                                 \
        double: CYAML_FLOAT,                                                \
        char *: CYAML_STRING,                                               \
        const char *: CYAML_STRING,                                         \
        default: CYAML_NULL);                                               \
    _Pragma("GCC diagnostic pop");                                          \
    _cyt;                                                                   \
  })
#else
#define _cyaml_type_of(val)              \
  _Generic((val),                        \
      bool: CYAML_BOOL,                  \
      char: CYAML_INTEGER,               \
      signed char: CYAML_INTEGER,        \
      short: CYAML_INTEGER,              \
      int: CYAML_INTEGER,                \
      long: CYAML_INTEGER,               \
      long long: CYAML_INTEGER,          \
      unsigned char: CYAML_INTEGER,      \
      unsigned short: CYAML_INTEGER,     \
      unsigned int: CYAML_INTEGER,       \
      unsigned long: CYAML_INTEGER,      \
      unsigned long long: CYAML_INTEGER, \
      float: CYAML_FLOAT,                \
      double: CYAML_FLOAT,               \
      char *: CYAML_STRING,              \
      const char *: CYAML_STRING,        \
      default: CYAML_NULL)
#endif

/* Compile-time constant: true when the platform's plain 'char' is signed. */
#if defined(__CHAR_UNSIGNED__)
#define _CYAML_CHAR_SIGNED false
#else
#define _CYAML_CHAR_SIGNED true
#endif

#if defined __clang__
#define _cyaml_is_signed(val)                                               \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool _cyis = _Generic((val),                                            \
        char: _CYAML_CHAR_SIGNED,                                           \
        signed char: true,                                                  \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    _cyis;                                                                  \
  })
#else
#define _cyaml_is_signed(val)   \
  _Generic((val),               \
      char: _CYAML_CHAR_SIGNED, \
      signed char: true,        \
      short: true,              \
      int: true,                \
      long: true,               \
      long long: true,          \
      default: false)
#endif

/* ========================================================================== */
/*                         PUBLIC MACROS                                      */
/* ========================================================================== */

/**
 * @brief Navigate to the DOM node at a dot-separated path.
 *
 * @param root  Root cyaml handle (CYAML_DICTIONARY or CYAML_LIST at top).
 * @param path  Dot-separated path string literal or char *.
 * @return Non-owning cyaml handle, or NULL if the path does not exist.
 *
 * Example:
 * @code
 * cyaml port = cyaml_get(doc, "server.ports.#0");
 * if (cyaml_type(port) == CYAML_INTEGER)
 *     printf("%lld\n", cyaml_int_val(port));
 * @endcode
 */
#define cyaml_get(root, path) _cyaml_get((root), (path))

/**
 * @brief Write a C scalar to the DOM leaf addressed by a dot-separated path.
 *
 * Accepted value types: bool, any integer type, float, double, char *,
 * const char *.  Passing NULL sets the leaf to CYAML_NULL.
 *
 * The leaf is created when absent (its immediate parent must already exist).
 * If the leaf exists its type is changed unconditionally.
 *
 * @param root  Root cyaml handle.
 * @param path  Dot-separated path string.
 * @param val   C value whose type is detected at compile time via _Generic.
 * @return ccol_retval_t: ccol_success on success, error code otherwise.
 *
 * Example:
 * @code
 * cyaml_set(doc, "server.port", 8080);
 * cyaml_set(doc, "server.name", "prod");
 * cyaml_set(doc, "server.tls",  (bool)true);
 * @endcode
 */
#define cyaml_set(root, path, val)                                           \
  ({                                                                         \
    typeof(val) _cyaml_sv = (val);                                           \
    _cyaml_set_typed((root), (path), _cyaml_type_of(_cyaml_sv),              \
                     (void *)&_cyaml_sv, sizeof(_cyaml_sv),                  \
                     _cyaml_is_signed(_cyaml_sv), is_char_array(_cyaml_sv)); \
  })

/**
 * @brief Remove and deep-free the DOM node addressed by a dot-separated path.
 *
 * Navigates to the parent of the addressed node, then removes and recursively
 * frees the child.  For dictionary parents the leaf is addressed by key; for
 * list parents the leaf must be a '#N' component.
 *
 * @param root  Root cyaml handle.
 * @param path  Dot-separated path string (same syntax as cyaml_get/cyaml_set).
 * @return ccol_retval_t: ccol_success on success, error code otherwise.
 *
 * Example:
 * @code
 * cyaml_delete(doc, "server.debug");
 * cyaml_delete(doc, "hosts.#0");
 * cyaml_delete(doc, "users.alice.address");
 * @endcode
 */
#define cyaml_delete(root, path) _cyaml_delete((root), (path))
