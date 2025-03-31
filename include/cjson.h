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
 * @file cjson.h
 * @brief Generic JSON parser, serializer, and mutable in-memory DOM.
 *
 * The DOM is backed by the library's own cvector (arrays) and chashmap
 * (objects).  Every node is a heap-allocated tagged_node_t; the full struct
 * definition lives in cjson.c and is opaque to callers.
 *
 * ### Custom memory management
 *
 * Every factory function and the parser accept a `ccol_memmgmt_procs_t *mp`
 * parameter.  Pass NULL to use the default malloc/free/calloc/realloc.
 *
 * The allocator is stored **per node** in the DOM tree.  All nodes in a tree
 * are expected to carry the same allocator, which is naturally satisfied when
 * all nodes are created through the same factory/parse call chain.  The
 * allocator is used for:
 *   - The node struct itself
 *   - Owned string copies (CJSON_STRING values, object keys)
 *   - Backing cvec (arrays) and chmap (objects)
 *   - Temporary path copies in cjson_get / cjson_set
 *   - The serialization buffer returned by cjson_serialize()
 *
 * **Thread-local node pool:** A thread-local free-list pool of up to 512 nodes
 * accelerates the common case (default allocator, NULL mp).  Custom-allocator
 * nodes always bypass the pool and are allocated/freed directly.  The pool is
 * drained automatically at thread exit.
 *
 * **Serialization:** The buffer returned by cjson_serialize() /
 * cjson_serialize_pretty() is allocated with the root node's allocator.  Pass
 * the same mp to cjson_serialize_free() to free it correctly.
 *
 * ### Path syntax (cjson_get / cjson_set)
 *
 * Paths are dot-separated component strings, e.g. "users.#0.address.city".
 *
 *   - Plain components address object keys.
 *   - A component that begins with '#' followed by decimal digits addresses
 *     an array element by zero-based index **when the current node is an
 *     array**; otherwise the whole component (including the '#') is used as a
 *     literal object key.
 *   - An empty path string is a no-op for cjson_get (returns root) and an
 *     error for cjson_set.
 *
 * ### Ownership
 *
 * - cjson_create_*(), cjson_parse(), cjson_parse_mp(), and cjson_clone()
 *   return fully owned trees.
 * - cjson_array_push() and cjson_object_set() transfer ownership of the child
 *   to the parent; do not free it afterwards.
 * - cjson_get() returns a NON-OWNING reference valid until the tree is mutated
 *   or destroyed.
 * - cjson_destroy() recursively frees the entire subtree and NULLs the handle.
 */

/* ========================================================================== */
/*                         NODE TYPE TAG                                      */
/* ========================================================================== */

/**
 * @brief JSON value kind tag.
 *
 * Replaces the general-purpose ccol_data_type with a JSON-specific set that
 * covers all seven JSON value kinds, including the composite types (array,
 * object) that ccol_data_type lacks, plus null and boolean.
 */
typedef enum cjson_node_type {
  CJSON_NULL = 0, /**< JSON null literal                           */
  CJSON_BOOL,     /**< JSON boolean  (true / false)                */
  CJSON_INTEGER,  /**< JSON number with no decimal point/exponent  */
  CJSON_FLOAT,    /**< JSON number with decimal point or exponent  */
  CJSON_STRING,   /**< JSON string (UTF-8, owned heap copy)        */
  CJSON_ARRAY,    /**< JSON array  (ordered list of child nodes)   */
  CJSON_OBJECT,   /**< JSON object (string-keyed child nodes)      */
} cjson_node_type_t;

/* ========================================================================== */
/*                         OPAQUE HANDLE                                      */
/* ========================================================================== */

/** @brief Opaque DOM node type.  Full definition lives in cjson.c. */
typedef struct tagged_node_t cjson_node_t;

/** @brief Public handle: pointer to an opaque DOM node. */
typedef cjson_node_t *cjson;

/* ========================================================================== */
/*                         NODE CONSTRUCTION                                  */
/* ========================================================================== */

/**
 * @brief Allocate and return a JSON null node.
 * @param mp  Custom allocator, or NULL for default.
 */
cjson cjson_create_null_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a JSON null node (default allocator). */
static inline cjson cjson_create_null(void) {
  return cjson_create_null_mp(NULL);
}

/**
 * @brief Allocate and return a JSON boolean node.
 * @param val  C boolean value.
 * @param mp   Custom allocator, or NULL for default.
 */
cjson cjson_create_bool_mp(bool val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a JSON boolean node (default allocator). */
static inline cjson cjson_create_bool(bool val) {
  return cjson_create_bool_mp(val, NULL);
}

/**
 * @brief Allocate and return a JSON integer node (stored as long long).
 * @param val  Integer value.
 * @param mp   Custom allocator, or NULL for default.
 */
cjson cjson_create_int_mp(long long val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a JSON integer node (default allocator). */
static inline cjson cjson_create_int(long long val) {
  return cjson_create_int_mp(val, NULL);
}

/**
 * @brief Allocate and return a JSON floating-point number node.
 * @param val  Double value.  Returns NULL for non-finite values.
 * @param mp   Custom allocator, or NULL for default.
 */
cjson cjson_create_double_mp(double val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a JSON floating-point number node (default
 * allocator). */
static inline cjson cjson_create_double(double val) {
  return cjson_create_double_mp(val, NULL);
}

/**
 * @brief Allocate and return a JSON string node.
 * @param val  Null-terminated string; an owned copy is made.
 *             NULL produces a CJSON_NULL node.
 * @param mp   Custom allocator, or NULL for default.
 */
cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a JSON string node (default allocator). */
static inline cjson cjson_create_string(const char *val) {
  return cjson_create_string_mp(val, NULL);
}

/**
 * @brief Allocate and return an empty JSON array node.
 * @param mp  Custom allocator, or NULL for default.
 */
cjson cjson_create_array_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return an empty JSON array node (default allocator). */
static inline cjson cjson_create_array(void) {
  return cjson_create_array_mp(NULL);
}

/**
 * @brief Allocate and return an empty JSON object node.
 * @param mp  Custom allocator, or NULL for default.
 */
cjson cjson_create_object_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return an empty JSON object node (default allocator). */
static inline cjson cjson_create_object(void) {
  return cjson_create_object_mp(NULL);
}

/* ========================================================================== */
/*                         PARSING                                            */
/* ========================================================================== */

/**
 * @brief Parse a null-terminated JSON string into a DOM tree.
 *
 * @param json_str  Input text (must be null-terminated).
 * @param err_str   Out-parameter for the error message on parse failure.
 *                  On success *err_str is set to NULL.  On failure a
 *                  heap-allocated string is stored; the caller must free() it.
 *                  Pass NULL to ignore error details.
 * @param mp        Custom allocator for all nodes in the resulting tree,
 *                  or NULL for the default allocator.
 * @return Root cjson node on success, NULL on parse failure.
 */
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs_t *mp);

/** @brief Parse a null-terminated JSON string (default allocator, no error
 * string). */
static inline cjson cjson_parse(const char *json_str, char **err_str) {
  return cjson_parse_mp(json_str, err_str, NULL);
}

/**
 * @brief Parse a bounded JSON buffer (need not be null-terminated).
 *
 * @param json_str  Input buffer.
 * @param len       Number of bytes to parse.
 * @param err_str   Out-parameter for the error message (same semantics as
 *                  cjson_parse_mp).  Pass NULL to ignore error details.
 * @param mp        Custom allocator, or NULL for default.
 * @return Root cjson node, or NULL on failure.
 */
cjson cjson_parse_n_mp(const char *json_str, size_t len, char **err_str,
                       ccol_memmgmt_procs_t *mp);

/** @brief Parse a bounded JSON buffer (default allocator, no error string). */
static inline cjson cjson_parse_n(const char *json_str, size_t len,
                                  char **err_str) {
  return cjson_parse_n_mp(json_str, len, err_str, NULL);
}

/* ========================================================================== */
/*                         SERIALIZATION                                      */
/* ========================================================================== */

/**
 * @brief Serialize a DOM tree to a compact JSON string.
 *
 * The returned buffer is allocated with the same allocator as the root node.
 * Free it with cjson_serialize_free(), passing the same mp that was used to
 * create the tree (NULL for the default allocator).
 *
 * @param node  Root of the (sub-)tree.
 * @return Heap-allocated null-terminated string; free with
 *         cjson_serialize_free().  Returns NULL on allocation failure.
 */
char *cjson_serialize(cjson node);

/**
 * @brief Serialize a DOM tree to an indented (pretty-printed) JSON string.
 *
 * @param node    Root of the (sub-)tree.
 * @param indent  Spaces per indent level; 0 uses a default of 4 spaces.
 * @return Heap-allocated string; free with cjson_serialize_free().
 */
char *cjson_serialize_pretty(cjson node, unsigned int indent);

/**
 * @brief Free a string returned by cjson_serialize() or
 *        cjson_serialize_pretty().
 *
 * @param s   String to free (may be NULL).
 * @param mp  The same allocator that was active when the tree was created
 *            (i.e. the mp passed to cjson_parse_mp() / cjson_create_*_mp()).
 *            Pass NULL when the default allocator was used.
 */
void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs_t *mp);

/** @brief Free a serializer string allocated with the default allocator. */
static inline void cjson_serialize_free(char *s) {
  cjson_serialize_free_mp(s, NULL);
}

/* ========================================================================== */
/*                         TYPE INSPECTION                                    */
/* ========================================================================== */

/**
 * @brief Return the type tag of a node.
 * @param node  May be NULL (returns CJSON_NULL).
 */
cjson_node_type_t cjson_type(cjson node);

/** Returns a string literal for @p cjson node, suitable for reporting type
 * mismatches.
 */
static inline const char *cjson_type_str(cjson node) {
  switch (cjson_type(node)) {
    case CJSON_NULL:
      return "CJSON_NULL";
    case CJSON_BOOL:
      return "CJSON_BOOL";
    case CJSON_INTEGER:
      return "CJSON_INTEGER";
    case CJSON_FLOAT:
      return "CJSON_FLOAT";
    case CJSON_STRING:
      return "CJSON_STRING";
    case CJSON_ARRAY:
      return "CJSON_ARRAY";
    case CJSON_OBJECT:
      return "CJSON_OBJECT";
    default:
      return "CJSON_UNKNOWN";
  }
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/**
 * @brief Return the boolean value.
 * Calls fatal_err() if the node's type is not CJSON_BOOL.
 */
bool cjson_bool_val(cjson node);

/**
 * @brief Return the integer value.
 * Calls fatal_err() if the node's type is not CJSON_INTEGER.
 */
long long cjson_int_val(cjson node);

/**
 * @brief Return the floating-point value.
 * Calls fatal_err() if the node's type is not CJSON_FLOAT.
 */
double cjson_double_val(cjson node);

/**
 * @brief Return the string value (owned by the node; do not free).
 * Calls fatal_err() if the node's type is not CJSON_STRING.
 */
const char *cjson_str_val(cjson node);

/**
 * @brief Return the element count of an array node.
 * Calls fatal_err() if the node's type is not CJSON_ARRAY.
 */
size_t cjson_array_len(cjson node);

/**
 * @brief Return the key count of an object node.
 * Calls fatal_err() if the node's type is not CJSON_OBJECT.
 */
size_t cjson_object_size(cjson node);

/* ========================================================================== */
/*                         ARRAY / OBJECT MANIPULATION                        */
/* ========================================================================== */

/**
 * @brief Append a child node to an array.
 *
 * Ownership of @p child transfers to @p arr; do not free it afterwards.
 *
 * @param arr    Target array node (must be CJSON_ARRAY).
 * @param child  Child to append.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t cjson_array_push(cjson arr, cjson child);

/**
 * @brief Access an element of an array by index (non-owning).
 * @return Child node, or NULL if index is out of bounds or type is wrong.
 */
cjson cjson_array_get(cjson arr, size_t index);

/**
 * @brief Set (insert or replace) a key in an object.
 *
 * Ownership of @p child transfers to @p obj.  If the key already exists the
 * previous child is deep-freed before the new one is stored.
 *
 * @param obj    Target object node (must be CJSON_OBJECT).
 * @param key    Null-terminated key string; a copy is stored internally.
 * @param child  Value node.
 * @return ccol_success, ccol_invalid_args, or ccol_not_enough_memory.
 */
ccol_retval_t cjson_object_set(cjson obj, const char *key, cjson child);

/**
 * @brief Look up a key in an object (non-owning).
 * @return Child node, or NULL if the key is absent or type is wrong.
 */
cjson cjson_object_get(cjson obj, const char *key);

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/**
 * @brief Return a fully independent deep copy of a DOM subtree.
 *
 * The clone uses the same allocator as the source tree (taken from the root
 * node's stored m_procs).
 *
 * @return New root node (caller owns it), or NULL on allocation failure.
 */
cjson cjson_clone(cjson node);

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cjson variable.
 *
 * Equivalent to `cjson var_name`.  Provided for naming-convention symmetry
 * with the other container modules.  The variable must be assigned before use.
 *
 * @param var_name  Variable name.
 *
 * Example:
 * @code
 * cjson_declare(root);
 * root = cjson_parse("{\"k\":1}", NULL);
 * cjson_destroy(root);
 * @endcode
 */
#define cjson_declare(var_name) cjson var_name

/**
 * @brief Declare a cjson variable with automatic destruction on scope exit.
 *
 * The variable is destroyed (and set to NULL) automatically when it goes out
 * of scope via the GCC cleanup attribute.  Must be initialized before use.
 *
 * @param var_name  Variable name.
 *
 * Example:
 * @code
 * {
 *   cjson_declare_scoped(root) = cjson_parse("{\"k\":1}", NULL);
 *   // root is freed here automatically
 * }
 * @endcode
 */
#define cjson_declare_scoped(var_name) \
  cjson var_name _ccol_destructor(___cjson_destroy)

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/** @brief Recursively free a DOM tree (internal — prefer the macro). */
void __cjson_destroy(cjson node);

/** @brief RAII cleanup helper for use with _ccol_destructor. */
static inline void ___cjson_destroy(cjson *node) {
  if (node && *node) {
    __cjson_destroy(*node);
    *node = NULL;
  }
}

/**
 * @brief Recursively free a DOM tree and NULL the handle.
 *
 * Safe to call on NULL.  Uses each node's stored allocator (m_procs) to free
 * its own memory — no external allocator parameter needed.
 */
#define cjson_destroy(node)  \
  do {                       \
    if (node) {              \
      __cjson_destroy(node); \
      (node) = NULL;         \
    }                        \
  } while (0)

/* ========================================================================== */
/*                         PATH NAVIGATION — BACK-END                        */
/* ========================================================================== */

/**
 * @brief Navigate to the node addressed by a dot-separated path (back-end).
 *
 * Prefer the cjson_get() macro.
 *
 * @return Target node, or NULL if any component is missing / type mismatch.
 */
cjson _cjson_get(cjson root, const char *path);

/**
 * @brief Write a typed scalar value to the leaf addressed by a path (back-end).
 *
 * Prefer the cjson_set() macro.
 *
 * Creates the leaf if absent (parent must exist).  Replaces and deep-frees
 * any existing value, including full subtrees, so type changes are safe.
 *
 * @param root             Root of the DOM tree.
 * @param path             Dot-separated path string.
 * @param type             JSON type of the new value.
 * @param raw              Pointer to the raw C value (any type; passed as void
 * *).
 * @param raw_size         sizeof() the original C expression.
 * @param is_signed        Whether the integer source type is signed.
 * @param raw_is_char_array true when raw points directly at a char[] array
 *                         (e.g. a string literal captured via typeof); false
 *                         when raw points at a const char * variable.  Used
 *                         only when type == CJSON_STRING.
 * @return ccol_success on success, ccol_invalid_args / ccol_key_not_found /
 *         ccol_not_enough_memory on failure.
 */
ccol_retval_t _cjson_set_typed(cjson root, const char *path,
                               cjson_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed,
                               bool raw_is_char_array);

/* ========================================================================== */
/*                         COMPILE-TIME TYPE HELPERS                         */
/* ========================================================================== */

/**
 * @brief Map a C expression's compile-time type to cjson_node_type_t.
 */
#if defined __clang__
#define _cjson_type_of(val)                                                 \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    cjson_node_type_t _cjt = _Generic((val),                                \
        bool: CJSON_BOOL,                                                   \
        char: CJSON_INTEGER,                                                \
        signed char: CJSON_INTEGER,                                         \
        short: CJSON_INTEGER,                                               \
        int: CJSON_INTEGER,                                                 \
        long: CJSON_INTEGER,                                                \
        long long: CJSON_INTEGER,                                           \
        unsigned char: CJSON_INTEGER,                                       \
        unsigned short: CJSON_INTEGER,                                      \
        unsigned int: CJSON_INTEGER,                                        \
        unsigned long: CJSON_INTEGER,                                       \
        unsigned long long: CJSON_INTEGER,                                  \
        float: CJSON_FLOAT,                                                 \
        double: CJSON_FLOAT,                                                \
        char *: CJSON_STRING,                                               \
        const char *: CJSON_STRING,                                         \
        default: CJSON_NULL);                                               \
    _Pragma("GCC diagnostic pop");                                          \
    _cjt;                                                                   \
  })
#else
#define _cjson_type_of(val)              \
  _Generic((val),                        \
      bool: CJSON_BOOL,                  \
      char: CJSON_INTEGER,               \
      signed char: CJSON_INTEGER,        \
      short: CJSON_INTEGER,              \
      int: CJSON_INTEGER,                \
      long: CJSON_INTEGER,               \
      long long: CJSON_INTEGER,          \
      unsigned char: CJSON_INTEGER,      \
      unsigned short: CJSON_INTEGER,     \
      unsigned int: CJSON_INTEGER,       \
      unsigned long: CJSON_INTEGER,      \
      unsigned long long: CJSON_INTEGER, \
      float: CJSON_FLOAT,                \
      double: CJSON_FLOAT,               \
      char *: CJSON_STRING,              \
      const char *: CJSON_STRING,        \
      default: CJSON_NULL)
#endif

/**
 * @brief Return true if the C expression has a signed integer type.
 */

/* Compile-time constant: true when the platform's plain 'char' is signed. */
#if defined(__CHAR_UNSIGNED__)
#define _CJSON_CHAR_SIGNED false
#else
#define _CJSON_CHAR_SIGNED true
#endif

#if defined __clang__
#define _cjson_is_signed(val)                                               \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool _cjis = _Generic((val),                                            \
        char: _CJSON_CHAR_SIGNED,                                           \
        signed char: true,                                                  \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    _cjis;                                                                  \
  })
#else
#define _cjson_is_signed(val)   \
  _Generic((val),               \
      char: _CJSON_CHAR_SIGNED, \
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
 * @param root  Root cjson handle (CJSON_OBJECT or CJSON_ARRAY at top level).
 * @param path  Dot-separated path string literal or char *.
 * @return Non-owning cjson handle, or NULL if the path does not exist.
 *
 * Example:
 * @code
 * cjson city = cjson_get(doc, "users.#0.address.city");
 * if (cjson_type(city) == CJSON_STRING)
 *     printf("%s\n", cjson_str_val(city));
 * @endcode
 */
#define cjson_get(root, path) _cjson_get((root), (path))

/**
 * @brief Write a C scalar to the DOM leaf addressed by a dot-separated path.
 *
 * Accepted value types: bool, any integer type, float, double, char *,
 * const char *.  Passing NULL sets the leaf to CJSON_NULL.
 *
 * The leaf is created when absent (its immediate parent must already exist).
 * If the leaf exists its type is changed unconditionally — existing array or
 * object subtrees are deep-freed automatically.
 *
 * @param root  Root cjson handle.
 * @param path  Dot-separated path string.
 * @param val   C value whose type is detected at compile time via _Generic.
 * @return ccol_retval_t: ccol_success on success, error code otherwise.
 *
 * Example:
 * @code
 * cjson_set(doc, "users.#0.active", (bool)true);
 * cjson_set(doc, "users.#0.score",  99);
 * cjson_set(doc, "users.#0.tag",    "champion");
 * @endcode
 */
#define cjson_set(root, path, val)                                           \
  ({                                                                         \
    typeof(val) _cjson_sv = (val);                                           \
    _cjson_set_typed((root), (path), _cjson_type_of(_cjson_sv),              \
                     (void *)&_cjson_sv, sizeof(_cjson_sv),                  \
                     _cjson_is_signed(_cjson_sv), is_char_array(_cjson_sv)); \
  })
