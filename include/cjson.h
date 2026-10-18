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

#include "common.h"

/* Every declaration from here to the end of this header belongs to the public
 * Application Binary Interface (ABI) of libccollections, and the shared
 * library exports all of them. We build the library with -fvisibility=hidden,
 * so a function or object outside such a block stays internal: it isn't in
 * the library's dynamic symbol table, an application that links against the
 * library can't interpose it, and a symbol of the same name in that
 * application can't collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cjson.h
 * @brief Generic JSON parser, serializer, and mutable in-memory DOM.
 *
 * The Document Object Model (DOM) is built on the library's own cvector for
 * lists and chashmap for dictionaries. Every node is a heap-allocated
 * cjson_node_t, and its full struct definition lives in cjson.c, so it is
 * opaque to the caller.
 *
 * ### Custom memory management
 *
 * Every factory function and the parser take a `ccol_memmgmt_procs_t *mp`
 * parameter. Pass NULL to get the default malloc/free/calloc/realloc.
 *
 * The library keeps its own copy of the procs struct and the nodes point at
 * that copy, never at the caller's struct, so you may build the struct on the
 * stack of a helper function, or release it, while trees built with it are
 * still in use. The functions it names have to stay callable until the last
 * node built with them is destroyed. The copies live as long as the process,
 * one for each distinct set of four functions, and nodes built from two
 * structs with the same four functions share one copy. A process can use at
 * most 64 distinct sets, and a factory function or a parse that would need a
 * 65th fails (NULL).
 *
 * Each node of the tree stores the allocator, and all the nodes of one tree
 * must carry the same allocator, which holds when one chain of factory calls
 * or parse calls creates all of them. The library uses the allocator for:
 *   - The node struct itself
 *   - Owned string copies (CJSON_STRING values, dictionary keys)
 *   - The cvec of a list and the chmap of a dictionary
 *   - Temporary path copies in cjson_set, and in cjson_get for a path of
 *     256 bytes or more
 *   - The buffer that cjson_serialize() returns
 *
 * With the default allocator (a NULL mp), a thread-local free-list pool of up
 * to 512 nodes makes allocation faster. A node with a custom allocator never
 * goes through the pool; the library allocates and frees it directly. The
 * pool is drained automatically when the thread exits.
 *
 * cjson_serialize() and cjson_serialize_pretty() return a buffer that comes
 * from the allocator of the root node. Free it with cjson_serialize_free()
 * when the tree uses the default allocator, and with cjson_serialize_free_mp(),
 * passing the same mp, when it uses a custom one.
 *
 * ### Text encoding
 *
 * RFC 8259 section 8.1 requires JSON text to be UTF-8, so every string that a
 * tree holds, a CJSON_STRING value or a dictionary key, is well-formed UTF-8.
 * You can always treat the result of cjson_str_val() as UTF-8, and
 * cjson_serialize() always produces valid JSON text.
 *
 * Bytes get in by two ways, and both refuse what is not UTF-8 instead of
 * repairing it:
 *
 *   - The parser decodes text that a peer gives. A string literal or a key
 *     holding a raw byte sequence that is not well-formed UTF-8 (a truncated
 *     sequence, a continuation byte with no lead byte, an overlong form, an
 *     encoded surrogate, a code point above U+10FFFF, or a byte from 0xF5 to
 *     0xFF) is a parse error, and so is a \uXXXX escape of a surrogate that
 *     isn't one half of a high-low pair. The message names the defect, the
 *     bytes as 0xNN or the escape, and the byte offset in the input, and it is
 *     always printable ASCII. A repair would let two keys that differ only in
 *     their ill-formed bytes turn into one key.
 *   - The calling program hands a C string to cjson_create_string(),
 *     cjson_set() and cjson_dictionary_set(). A string that isn't valid UTF-8
 *     is a bug in that program, so these functions report it with NULL or
 *     with ccol_invalid_args, exactly as they treat a non-finite double.
 *
 * ### Nesting depth
 *
 * A parse, a clone and a serialize each accept at most 500 levels of nesting.
 * Each list or dictionary is one level, in any combination, and a scalar adds
 * none, so 500 containers inside one another, with anything inside the
 * innermost one, are accepted and a 501st container is not. Each of the three
 * reports a deeper document as an error. All three walk the nesting with an
 * explicit worklist on the heap, so the limit is a policy on what the library
 * accepts and doesn't depend on the stack size of the calling thread: a
 * document at the limit costs the same small, fixed amount of native stack as
 * a flat one. cjson_destroy() has no limit at all.
 *
 * ### Memory of a parse
 *
 * The tree that a parse builds is linear in the length of the input, since
 * JSON has no construct that copies one part of a document into another. An
 * empty list or dictionary costs one node, and its backing store is created
 * by its first member. The densest documents (deep chains of one-member
 * dictionaries, or long lists of one-member dictionaries) hold at most about
 * 65 bytes of tree per byte of input on a 64-bit target. A program that parses
 * input from a peer it doesn't trust bounds the memory of a parse by bounding
 * the length of that input.
 *
 * ### Path syntax (cjson_get / cjson_set)
 *
 * A path is a string of dot-separated components, for example
 * "users.#0.address.city".
 *
 *   - A plain component addresses a dictionary key.
 *   - A component that starts with '#' followed by decimal digits addresses a
 *     list element by its zero-based index, but only when the current node is
 *     a list. Otherwise the whole component is taken as a literal dictionary
 *     key, with the '#' as part of that key.
 *   - An empty path string is a no-op for cjson_get, which returns the root,
 *     and an error for cjson_set.
 *   - A malformed component (an empty one, one that isn't valid UTF-8, or a
 *     malformed '#N' on a list) and a path whose shape can't apply to the
 *     tree (a component that would have to descend into or below a string, a
 *     number, a bool or null, or a component that isn't '#N' on a list) make
 *     cjson_set() and cjson_delete() return ccol_invalid_args, at any position
 *     of the path. They return ccol_key_not_found only for a well-formed name
 *     or index that is absent. cjson_get() returns NULL in all of these cases.
 *
 * ### Ownership
 *
 * - cjson_create_*(), cjson_parse(), cjson_parse_mp(), and cjson_clone() return
 *   trees that the caller owns completely.
 * - On success, cjson_list_push() and cjson_dictionary_set() move the
 *   ownership of the child to the parent, so don't free the child after such
 *   a call. The child must not already be attached to a list or dictionary
 *   parent, and it must not contain the target container inside its own
 *   subtree. A new node is a correct child, and so is a new cjson_clone().
 *   These four are not: a borrowed reference from cjson_get(),
 *   cjson_list_get() or cjson_dictionary_get(); the target container itself;
 *   an ancestor of the target container; and a node that cjson_list_remove()
 *   or cjson_dictionary_remove() removed, since both of them always deep-free
 *   what they remove. The doc comment of each function gives the exact rule
 *   and its one no-op exception.
 * - On failure those two functions split by return code, and that split is
 *   what lets you act on a failure. ccol_invalid_args ALWAYS means that the
 *   arguments were rejected: the child is untouched and still belongs to the
 *   caller. Every other non-success code ALWAYS means that ownership moved and
 *   the function has already destroyed the child.
 * - cjson_get() returns a NON-OWNING reference, which stays valid until
 *   something changes or destroys the tree.
 * - cjson_destroy() frees the whole subtree recursively and sets the handle
 *   to NULL.
 */

/* ========================================================================== */
/*                         NODE TYPE TAG                                      */
/* ========================================================================== */

/**
 * @brief JSON value kind tag.
 *
 * We use this tag instead of the general-purpose ccol_data_type because it
 * covers all seven JSON value kinds, including the composite list and
 * dictionary that ccol_data_type doesn't have, and also null and boolean.
 */
typedef enum cjson_node_type {
  CJSON_NULL = 0,   /**< JSON null literal                           */
  CJSON_BOOL,       /**< JSON boolean  (true / false)                */
  CJSON_INTEGER,    /**< JSON number with no decimal point/exponent  */
  CJSON_FLOAT,      /**< JSON number with decimal point or exponent  */
  CJSON_STRING,     /**< JSON string (UTF-8, owned heap copy)        */
  CJSON_LIST,       /**< JSON list (list of child nodes)            */
  CJSON_DICTIONARY, /**< JSON dictionary (string-keyed child nodes)  */
} cjson_node_type_t;

/* ========================================================================== */
/*                         OPAQUE HANDLE                                      */
/* ========================================================================== */

/** @brief Opaque DOM node type.  The full definition is in cjson.c. */
typedef struct cjson_node_t cjson_node_t;

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
 * @param val  Double value. Returns NULL for a non-finite value.
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
 *
 * @p val must be valid UTF-8. JSON text is UTF-8 (RFC 8259 section 8.1) and
 *    any other byte sequence has no JSON form, so this function refuses such a
 *    sequence instead of storing it, the same way cjson_create_double_mp()
 *    refuses a non-finite double.
 *
 * @param val  Null-terminated, valid UTF-8 string, which the node copies into
 *             storage it owns. NULL gives a CJSON_NULL node.
 * @param mp   Custom allocator, or NULL for default.
 * @return New CJSON_STRING node, or NULL when the allocation fails or when
 * @p val is not valid UTF-8.
 */
cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return a JSON string node (default allocator). */
static inline cjson cjson_create_string(const char *val) {
  return cjson_create_string_mp(val, NULL);
}

/**
 * @brief Allocate and return an empty JSON list node.
 * @param mp  Custom allocator, or NULL for default.
 */
cjson cjson_create_list_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return an empty JSON list node (default allocator). */
static inline cjson cjson_create_list(void) {
  return cjson_create_list_mp(NULL);
}

/**
 * @brief Allocate and return an empty JSON dictionary node.
 * @param mp  Custom allocator, or NULL for default.
 */
cjson cjson_create_dictionary_mp(ccol_memmgmt_procs_t *mp);

/** @brief Allocate and return an empty JSON dictionary node (default
 * allocator). */
static inline cjson cjson_create_dictionary(void) {
  return cjson_create_dictionary_mp(NULL);
}

/* ========================================================================== */
/*                         PARSING                                            */
/* ========================================================================== */

/**
 * @brief Parse a null-terminated JSON string into a DOM tree.
 *
 * @param json_str  Input text, which must be null-terminated.
 * @param err_str   Out-parameter for the error message of a failed parse. On
 *                  success *err_str is set to NULL, and on failure it points
 *                  at a message that the LIBRARY owns. Never free that
 *                  string, not with cjson_serialize_free_mp() either, since
 *                  it isn't an allocation; the err out-parameter of every
 *                  other module in this library has the same rule. The text
 *                  stays valid until the next FAILING parse on the same
 *                  thread, the lifetime that strerror(3) and dlerror(3) give,
 *                  and the storage is per-thread, so two threads that parse
 *                  at the same time never overwrite each other's message.
 *                  Copy the text if you need it longer than that. When the
 *                  thread can't keep a message buffer (memory runs out, or
 *                  the process has no thread-specific key left for it), the
 *                  message is a fixed text that names no reason. Pass NULL to
 *                  ignore the error details. The message is printable ASCII
 *                  whatever the input holds: a printable ASCII byte of the
 *                  input is quoted as it is and any other byte is spelled as
 *                  0xNN.
 * @param mp        Custom allocator for all the nodes of the new tree, or
 *                  NULL for the default allocator.
 * @return The root cjson node on success, or NULL when the parse fails.
 *
 * A number always uses '.' as its decimal separator, as RFC 8259 says,
 * whatever the LC_NUMERIC locale of the calling thread is.
 *
 * A string that isn't well-formed UTF-8 is a parse error, so every string of
 * the new tree is well-formed UTF-8, and a document nested more than 500
 * levels deep is a parse error too. See the "Text encoding" and "Nesting
 * depth" sections of this header.
 */
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs_t *mp);

/** @brief Parse a null-terminated JSON string (default allocator, no error
 * string). */
static inline cjson cjson_parse(const char *json_str, char **err_str) {
  return cjson_parse_mp(json_str, err_str, NULL);
}

/**
 * @brief Parse a bounded JSON buffer, which need not be null-terminated.
 *
 * @param json_str  Input buffer.
 * @param len       Number of bytes to parse.
 * @param err_str   Out-parameter for the error message. It works like the one
 *                  of cjson_parse_mp and has the same ownership rule: the
 *                  library owns the string and the caller never frees it.
 *                  Pass NULL to ignore the error details.
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
 * The buffer comes from the same allocator as the root node, so how the tree
 * was built decides which function frees it. Free the buffer of a tree with
 * the default allocator with cjson_serialize_free(), and the buffer of a tree
 * with a custom allocator with cjson_serialize_free_mp(), passing that same
 * allocator. cjson_serialize_free() hands the buffer to a plain free(), which
 * never allocated the buffer of a custom allocator.
 *
 * A float or double value always uses '.' as its decimal separator, as RFC
 * 8259 says, whatever the LC_NUMERIC locale of the calling thread is. The
 * output is always valid JSON text, UTF-8 included; see the "Text encoding"
 * section of this header.
 *
 * A subtree nested more than 500 levels deep is refused, and the refusal is
 * reported the same way as a failed allocation, as in cjson_clone(). The
 * serializer walks the nesting with an explicit worklist on the heap, so a
 * tree at the limit costs the same small, fixed amount of native stack as a
 * flat one.
 *
 * @param node  Root of the tree or of the subtree.
 * @return A heap-allocated, null-terminated string; free it with
 *         cjson_serialize_free(). Returns NULL when the allocation fails or
 *         when the tree is nested too deeply.
 */
char *cjson_serialize(cjson node);

/**
 * @brief Serialize a DOM tree to an indented JSON string.
 *
 * @param node    Root of the tree or of the subtree.
 * @param indent  Spaces per indent level. 0 gives the default of 4 spaces.
 * @return A heap-allocated string from the allocator of the root node. Free
 *         it exactly like the result of cjson_serialize(): with
 *         cjson_serialize_free() when the tree uses the default allocator and
 *         with cjson_serialize_free_mp() when it uses a custom one. Returns
 *         NULL when the allocation fails or when the tree is nested too
 *         deeply, with the same limit as cjson_serialize().
 */
char *cjson_serialize_pretty(cjson node, unsigned int indent);

/**
 * @brief Free a string returned by cjson_serialize() or
 *        cjson_serialize_pretty().
 *
 * @param s   String to free. It can be NULL.
 * @param mp  The allocator that the tree was created with, i.e. the mp that
 *            was given to cjson_parse_mp() or to cjson_create_*_mp(). Pass
 *            NULL for the default allocator.
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
 * @param node  Can be NULL, and then the function returns CJSON_NULL.
 */
cjson_node_type_t cjson_type(cjson node);

/** Returns a string literal that names the type of @p node, handy when you
 * report a type mismatch.
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
    case CJSON_LIST:
      return "CJSON_LIST";
    case CJSON_DICTIONARY:
      return "CJSON_DICTIONARY";
    default:
      return "CJSON_UNKNOWN";
  }
}

/* ========================================================================== */
/*                         LEAF VALUE ACCESS                                  */
/* ========================================================================== */

/**
 * @brief Return the boolean value.
 * Calls _ccol_fatal_err() when node is NULL or its type is not CJSON_BOOL.
 */
bool cjson_bool_val(cjson node);

/**
 * @brief Return the integer value.
 * Calls _ccol_fatal_err() when node is NULL or its type is not CJSON_INTEGER.
 */
long long cjson_int_val(cjson node);

/**
 * @brief Return the floating-point value.
 * Calls _ccol_fatal_err() when node is NULL or its type is not CJSON_FLOAT.
 */
double cjson_double_val(cjson node);

/**
 * @brief Return the string value. The node owns it, so don't free it.
 *
 * The pointer is borrowed and stays valid until the node gets a new value or
 * is destroyed. Any of these ends it: a cjson_set() that reaches the node, a
 * cjson_list_remove(), cjson_dictionary_remove() or cjson_delete() of the
 * node, a cjson_dictionary_set() that replaces it, and a destroy of its tree.
 * Copy the text if you need it longer.
 *
 * The string is always valid UTF-8, wherever the tree came from; see the
 * "Text encoding" section of this header.
 *
 * Calls _ccol_fatal_err() when node is NULL or its type is not
 * CJSON_STRING.
 */
const char *cjson_str_val(cjson node);

/**
 * @brief Return the element count of a list node.
 * Calls _ccol_fatal_err() when node is NULL or its type is not CJSON_LIST.
 */
size_t cjson_list_len(cjson node);

/**
 * @brief Return the key count of a dictionary node.
 * Calls _ccol_fatal_err() when node is NULL or its type is not
 * CJSON_DICTIONARY.
 */
size_t cjson_dictionary_size(cjson node);

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/**
 * @brief Append a child node to a list.
 *
 * On success @p arr takes ownership of @p child, so don't free @p child after
 * the call.
 *
 * On failure each return code carries exactly one ownership rule.
 * ccol_invalid_args ALWAYS means that the call rejected its arguments and
 * touched nothing: @p child is exactly as it was, and the caller (or the
 * container it was already attached to) still owns it and still has to
 * destroy it. Every other non-success code ALWAYS means that ownership moved
 * and the call has already destroyed @p child, so freeing @p child again is a
 * double free. Without this split no caller could act on a code at all, since
 * a free on ccol_invalid_args would double-free the rejections and skipping
 * the free on ccol_not_enough_memory would leak the transfers.
 *
 * @p child must not already be attached to a list or dictionary parent. It
 *    must be a new node, or a new cjson_clone(), and never a borrowed
 *    reference returned by cjson_get(), cjson_list_get() or
 *    cjson_dictionary_get(), nor @p arr itself. cjson_list_remove() and
 *    cjson_dictionary_remove() always deep-free the node they remove and never
 *    hand back a live handle to it, so a removed node is never a valid
 * @p child either. An already-attached node is rejected with
 *    ccol_invalid_args and @p child is left completely untouched, still owned
 *    by the container it was attached to. Accepting it would give the node two
 *    owners, and each of them would free it when its own parent is destroyed.
 *
 * @p child must also not contain @p arr anywhere in its own subtree, i.e.
 *    @p child must not be an ancestor of @p arr in the tree as it stands.
 *    Attaching it would make @p arr a new ancestor of @p child while @p arr
 *    is still a descendant of @p child, which is a cycle. A cycle that the
 *    check finds is rejected the same way, with ccol_invalid_args and @p child
 *    untouched. The check needs memory, so it can't always complete; when it
 *    fails for that reason the function returns ccol_not_enough_memory
 *    instead, because carrying on silently could let an undetected cycle
 *    through. The call then destroys @p child, as it does for every other
 *    outcome that moves ownership.
 *
 * So the ccol_invalid_args rejections are: @p child is NULL; @p child is
 * already attached to a list or a dictionary; @p child is @p arr itself;
 * @p arr is NULL or not a CJSON_LIST; or attaching @p child would close a
 * cycle that the check finds. Two outcomes move ownership instead: the cycle
 *    check runs out of memory, or the insert itself fails.
 *
 * @param arr    Target list node, which must be a CJSON_LIST.
 * @param child  Child to append.
 * @return ccol_success. Or ccol_invalid_args, when arr or child is NULL or has
 *         the wrong type, when child is already attached somewhere else, or
 *         when child already contains arr; child is left untouched in all of
 *         those cases. Or ccol_not_enough_memory, or ccol_container_full when
 *         arr already holds its maximum element count; for those last two
 *         codes the function destroys child.
 */
ccol_retval_t cjson_list_push(cjson arr, cjson child);

/**
 * @brief Access an element of a list by its index. The result is
 *        non-owning.
 * @return The child node. Returns NULL when the index is out of bounds or
 *         when the type is wrong.
 */
cjson cjson_list_get(cjson arr, size_t index);

/**
 * @brief Set a key in a dictionary. This inserts the key or replaces it.
 *
 * On success @p obj takes ownership of @p child. If the key already exists,
 * the previous child is deep-freed and the new child goes into the same slot.
 * Nothing in that replacement can fail, so the slot never names freed memory
 * once the call returns, and the key keeps its place in the insertion order.
 *
 * Failures split exactly as in cjson_list_push(): ccol_invalid_args ALWAYS
 * means that the arguments were rejected, @p child is untouched and still
 * belongs to the caller. Every other non-success code ALWAYS means that
 * ownership moved and the function has already destroyed @p child.
 *
 * @p child must not already be attached to a list or dictionary parent, with
 *    one exception: passing back the exact node that @p key already holds, as
 *    in `cjson_dictionary_set(obj, k, cjson_dictionary_get(obj, k))`, is a
 *    harmless no-op. Any other already-attached
 * @p child is rejected with ccol_invalid_args and left completely untouched.
 *    That covers a node under a different key, a node in a different
 *    container, and
 * @p obj itself, for the reason given at cjson_list_push(). The same text
 *    covers cjson_list_remove() and cjson_dictionary_remove(), which always
 *    deep-free the node they remove, so a removed node is never a valid
 *    @p child either.
 *
 * @p child must also not contain @p obj anywhere in its own subtree, i.e.
 *    @p child must not be an ancestor of @p obj in the tree as it stands. The
 *    doc comment of cjson_list_push() explains why this is rejected like a
 *    double attach, and why the check itself can fail with
 *    ccol_not_enough_memory when memory runs short.
 *
 * So the ccol_invalid_args rejections are: @p child is NULL; @p child is
 * already attached under a different key or in another container; @p child
 * is @p obj itself; @p obj is NULL or not a CJSON_DICTIONARY; @p key is NULL
 * or not valid UTF-8; or attaching @p child would close a cycle that the
 * check finds. Two outcomes move ownership instead: the cycle check runs out
 * of memory, or the insert itself fails.
 *
 * Every key in the tree is valid UTF-8, which is what lets cjson_serialize()
 * write the key as it is, so a @p key that isn't valid UTF-8 is refused
 * instead of stored.
 *
 * @param obj    Target dictionary node, which must be a CJSON_DICTIONARY.
 * @param key    Null-terminated key string, which must be valid UTF-8. The
 *               dictionary stores its own copy.
 * @param child  Value node.
 * @return ccol_success. Or ccol_invalid_args, when obj, key or child is NULL or
 *         has the wrong type, when key is not valid UTF-8, when child is
 *         already attached somewhere else, or when child already contains obj.
 *         Or ccol_not_enough_memory. Or ccol_container_full, when obj already
 *         holds its maximum element count.
 */
ccol_retval_t cjson_dictionary_set(cjson obj, const char *key, cjson child);

/**
 * @brief Look up a key in a dictionary. The result is non-owning.
 * @return The child node. Returns NULL when the key is absent or when the
 *         type is wrong.
 */
cjson cjson_dictionary_get(cjson obj, const char *key);

/**
 * @brief Remove and deep-free the element at position index from a list.
 *
 * Every element after the index moves one position to the left, and the
 * removed subtree is freed recursively.
 *
 * @param arr    Target list node, which must be a CJSON_LIST.
 * @param index  Zero-based index of the element to remove.
 * @return ccol_success on success.
 *         ccol_invalid_args when arr is NULL, when arr is not a list, or
 *         when the index is out of bounds.
 */
ccol_retval_t cjson_list_remove(cjson arr, size_t index);

/**
 * @brief Remove and deep-free the entry with the given key from a dictionary.
 *
 * The removed subtree is freed recursively.
 *
 * @param obj  Target dictionary node, which must be a CJSON_DICTIONARY.
 * @param key  Null-terminated key string.
 * @return ccol_success on success.
 *         ccol_invalid_args when obj or key is NULL, or when obj is not a
 *         dictionary.
 *         ccol_key_not_found when the key does not exist.
 */
ccol_retval_t cjson_dictionary_remove(cjson obj, const char *key);

/* ========================================================================== */
/*                         DICTIONARY ITERATION                               */
/* ========================================================================== */

/**
 * @brief A cursor over the members of a dictionary, in insertion order.
 *
 * The caller owns the struct, usually on its own stack, and the iteration
 * allocates nothing and can't fail. Fill it with cjson_dictionary_first() and
 * step it with cjson_dictionary_next(). Read only @c key and @c value; the
 * fields whose names start with an underscore are private.
 *
 * The order is the order in which each key first entered the dictionary,
 * which is the order of the members in the parsed text, or the order of the
 * cjson_dictionary_set() and cjson_set() calls that added them. Replacing the
 * value of a key keeps its place, so a key that a document repeats keeps the
 * place of its first occurrence and the value of its last. A removed key
 * leaves the order. The serializers and cjson_clone() follow the same order.
 *
 * The cursor holds the member that the next step reads, which is the
 * successor of the member that the last successful call gave. It stays valid
 * across every change of the dictionary that leaves that successor in place:
 *
 *   - replacing the value of any member, the current one included, through
 *     cjson_dictionary_set() or cjson_set(). The member keeps its place, so
 *     the iteration goes on in the same order;
 *   - removing the current member, or any member other than the successor;
 *   - inserting a new key. The new member goes after every other member, and
 *     whether this iteration reaches it is unspecified.
 *
 * Removing the successor, through cjson_dictionary_remove(), cjson_delete()
 * or any other call, invalidates the cursor, and so does destroying the
 * dictionary. Start again with cjson_dictionary_first() after such a change.
 *
 * The @c key and @c value fields describe the current member as it was when
 * the cursor stepped onto it. Replacing the value of the current member
 * through cjson_dictionary_set() destroys the old value, so @c value then
 * names freed memory; read the new value with cjson_dictionary_get() and
 * @c key. cjson_set() updates the existing node in place, so @c value stays
 *    the same node.
 */
typedef struct cjson_dictionary_iter {
  /** The key of the current member. It stays valid until that member is
   *  removed or the dictionary is destroyed, and it is NULL once the
   *  iteration is over. */
  const char *key;
  /** The value of the current member, a borrowed reference with the rules
   *  of cjson_dictionary_get(). NULL once the iteration is over. */
  cjson value;
  /** Private: the member that the next step reads. */
  const void *_cjson_next;
} cjson_dictionary_iter;

/**
 * @brief Start an iteration over a dictionary, and step onto its first
 *        member.
 *
 * @param dict  The dictionary node.
 * @param it    The cursor to fill. It must not be NULL.
 * @return true when @p it stands on the first member. false when the
 *         dictionary is empty, when @p dict is NULL, or when @p dict is not
 *         a CJSON_DICTIONARY, and then @c key and @c value are set to NULL.
 *         false also when @p it is NULL.
 *
 * Example:
 * @code
 * cjson_dictionary_iter it;
 * for (bool ok = cjson_dictionary_first(obj, &it); ok;
 *      ok = cjson_dictionary_next(&it)) {
 *   printf("%s: %s\n", it.key, cjson_type_str(it.value));
 * }
 * @endcode
 */
bool cjson_dictionary_first(cjson dict, cjson_dictionary_iter *it);

/**
 * @brief Step a dictionary cursor onto the next member.
 *
 * @param it  A cursor that cjson_dictionary_first() filled.
 * @return true when @p it stands on the next member. false when the previous
 *         call gave the last member, or when an earlier call has already
 *         returned false, and then @c key and @c value are set to NULL.
 *         false also when @p it is NULL.
 */
bool cjson_dictionary_next(cjson_dictionary_iter *it);

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/**
 * @brief Return a fully independent deep copy of a DOM subtree.
 *
 * The clone uses the same allocator as the source tree, the m_procs that the
 * root node stores.
 *
 * A subtree nested more than 500 levels deep is refused, and the refusal is
 * reported the same way as a failed allocation. That's the parser's limit
 * too, so any tree that parses can be cloned; only direct calls to
 * cjson_list_push() or cjson_dictionary_set() can build a deeper one. The
 * clone walks the nesting with an explicit worklist on the heap, so a tree at
 * the limit costs the same small, fixed amount of native stack as a flat one.
 *
 * @return The new root node, which the caller owns, or NULL when an
 *         allocation fails or when the subtree is nested too deeply.
 */
cjson cjson_clone(cjson node);

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cjson variable.
 *
 * This is the same as `cjson var_name`; it's here so that cjson follows the
 * naming convention of the other container modules. Assign the variable
 * before you use it.
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
 * The GCC cleanup attribute destroys the variable at the end of its scope and
 * sets it to NULL. Initialize the variable before you use it.
 *
 * @param var_name  Variable name.
 *
 * Example:
 * @code
 * {
 *   cjson_declare_scoped(root);
 *   root = cjson_parse("{\"k\":1}", NULL);
 *   // root is freed here automatically
 * }
 * @endcode
 */
#define cjson_declare_scoped(var_name) \
  cjson var_name _ccol_destructor(___cjson_destroy) = NULL

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/** @brief Free a DOM tree recursively. This is internal. Use the macro. */
void __cjson_destroy(cjson node);

/** @brief Cleanup helper for use with _ccol_destructor. */
static inline void ___cjson_destroy(cjson *node) {
  if (node && *node) {
    __cjson_destroy(*node);
    *node = NULL;
  }
}

/**
 * @brief Free a DOM tree recursively and set the handle to NULL.
 *
 * Calling it on NULL is safe. Each node frees its own memory with the
 * allocator it stores (its m_procs), so the macro doesn't take an allocator
 * parameter.
 *
 * @note The macro evaluates node exactly once. It must be a modifiable lvalue,
 *       such as a variable or an array element.
 */
#define cjson_destroy(node)      \
  _ccol_cjson_destroy_impl(node, \
                           _ccol_uniq(__ccol_cjson_destroy_slot, __COUNTER__))

/* Internal. The body of cjson_destroy. slot is a name from _ccol_uniq(), so
 * the macro can nest inside the argument of another destroy macro and stay
 * -Wshadow clean. The argument is evaluated exactly once. */
#define _ccol_cjson_destroy_impl(node, slot) \
  do {                                       \
    __typeof__(node) *slot = &(node);        \
    if (*slot) {                             \
      __cjson_destroy(*slot);                \
      *slot = NULL;                          \
    }                                        \
  } while (0)

/* ========================================================================== */
/*                         PATH NAVIGATION - BACK-END                        */
/* ========================================================================== */

/**
 * @brief Go to the node that a dot-separated path addresses. This is the
 *        back-end.
 *
 * Use the cjson_get() macro.
 *
 * @return The target node. Returns NULL when a component is absent or when
 *         a type doesn't match.
 *         A path of 256 bytes or more is first copied with the allocator of
 *         root, and NULL also comes back when that copy fails; a shorter
 *         path needs no allocation.
 */
cjson _cjson_get(cjson root, const char *path);

/**
 * @brief Remove and deep-free the node that a path addresses. This is the
 *        back-end.
 *
 * Use the cjson_delete() macro.
 *
 * The function walks to the parent of the addressed node and then calls
 * cjson_dictionary_remove() or cjson_list_remove(), whichever fits. The path
 * uses the same dot-separated syntax as _cjson_get and _cjson_set_typed,
 * escape sequences included.
 *
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL path, an empty path, and an empty
 *         path component (a leading dot, a trailing dot or two dots in a
 *         row). It is also returned, at any position of the path, for a path
 *         whose shape can't apply to the tree (a component that would have
 *         to descend into or below a string, a number, a bool or null, or a
 *         component that isn't "#N" on a list), for a malformed "#N" index on
 *         a list, and for a component above the leaf that isn't valid UTF-8.
 *         ccol_key_not_found only when a well-formed name or index is absent,
 *         above the leaf or at the leaf.
 *         ccol_not_enough_memory when an allocation fails.
 */
ccol_retval_t _cjson_delete(cjson root, const char *path);

/**
 * @brief Write a typed scalar value to the leaf that a path addresses. This is
 *        the back-end.
 *
 * Use the cjson_set() macro.
 *
 * The leaf is created when it is absent, but the parent must exist. Any value
 * already there, a full subtree included, is replaced and deep-freed, so a
 * change of type is safe.
 *
 * @param root             Root of the DOM tree.
 * @param path             Dot-separated path string.
 * @param type             JSON type of the new value.
 * @param raw              Pointer to the raw C value. The value can have any
 *                         type, and the caller passes it as a void *.
 * @param raw_size         The sizeof() of the original C expression. It is
 *                         checked against @p type before anything is read
 *                         through @p raw, because a direct call to this
 *                         back-end can pass a payload narrower than the type
 *                         needs.
 * @param is_signed        True when the integer source type is signed. An
 *                         unsigned 8-byte value above LLONG_MAX is stored as a
 *                         CJSON_FLOAT; see the doc comment of cjson_set().
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL root or path, an empty path, and an
 *         empty path component (a leading dot, a trailing dot or two dots in
 *         a row). It is also returned for a path component that isn't valid
 *         UTF-8 at any position, for a path whose shape can't apply to the
 *         tree at any position (a component that would have to descend into
 *         or below a string, a number, a bool or null, or a component that
 *         isn't "#N" on a list), for a malformed "#N" index on a list at any
 *         position of the path, for a C type that isn't supported, for a
 *         @p raw_size that doesn't match @p type, for a string value that
 *         isn't valid UTF-8, and for a void * value that isn't NULL. A NULL
 *         @p raw for any type other than CJSON_NULL gives this code too. For
 *         a void *, only a bare NULL is accepted; see the doc comment of
 *         cjson_set().
 *         ccol_key_not_found only when a well-formed name or index on the
 *         parent path is absent, or for a list index of the leaf whose syntax
 *         is correct but which is out of range.
 *         ccol_not_enough_memory when an allocation fails.
 *         ccol_container_full when the parent dictionary already holds its
 *         maximum element count, which can only happen for a new key.
 */
ccol_retval_t _cjson_set_typed(cjson root, const char *path,
                               cjson_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed);

/* ========================================================================== */
/*                         COMPILE-TIME TYPE HELPERS                         */
/* ========================================================================== */

/**
 * @brief Sentinel that _cjson_type_of() returns for a C type that cjson_set()
 *        doesn't accept. cjson_set() accepts bool, any integer type, float,
 *        double, char *, const char *, and a bare NULL; every other type,
 *        such as long double or a struct, lands in the default association
 *        of the _Generic below and gets this sentinel. A bare `NULL` literal
 *        has the type void *, and the _Generic gives it its own explicit
 *        association, as it does for the C23 nullptr through
 *        _CCOL_NULLPTR_ASSOC, so neither falls into the default: passing NULL
 *        is documented, intentional use and not a caller mistake, as the doc
 *        comment of cjson_set says.
 *
 * The sentinel is on purpose NOT a real cjson_node_type_t enumerator. No node
 * ever stores it and cjson_type() never returns it; it only travels through
 * the `type` argument of _cjson_set_typed(). The switch that checks the type
 * there is scalar_payload_check() in cjson.c, which rejects any value outside
 * {CJSON_NULL, CJSON_BOOL, CJSON_INTEGER, CJSON_FLOAT, CJSON_STRING} with
 * ccol_invalid_args before it touches the target node. Because the value
 * differs from every real enumerator, that guard fires for an unsupported
 * type, whereas a default of CJSON_NULL, which is on the accept-list, would
 * pass the guard silently.
 */
#define _CJSON_TYPE_UNSUPPORTED ((cjson_node_type_t)0x7f)

/**
 * @brief Map the compile-time type of a C expression to cjson_node_type_t.
 */
#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
#define _cjson_type_of(val) \
  _cjson_type_of_impl((val), _ccol_uniq(_cjt, __COUNTER__))
#define _cjson_type_of_impl(val, _cjt_name)                                 \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    cjson_node_type_t _cjt_name = _Generic((val),                           \
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
        _CCOL_NULLPTR_ASSOC(CJSON_NULL) void *: CJSON_NULL,                 \
        default: _CJSON_TYPE_UNSUPPORTED);                                  \
    _Pragma("GCC diagnostic pop");                                          \
    _cjt_name;                                                              \
  })
#else
#define _cjson_type_of(val)                               \
  _Generic((val),                                         \
      bool: CJSON_BOOL,                                   \
      char: CJSON_INTEGER,                                \
      signed char: CJSON_INTEGER,                         \
      short: CJSON_INTEGER,                               \
      int: CJSON_INTEGER,                                 \
      long: CJSON_INTEGER,                                \
      long long: CJSON_INTEGER,                           \
      unsigned char: CJSON_INTEGER,                       \
      unsigned short: CJSON_INTEGER,                      \
      unsigned int: CJSON_INTEGER,                        \
      unsigned long: CJSON_INTEGER,                       \
      unsigned long long: CJSON_INTEGER,                  \
      float: CJSON_FLOAT,                                 \
      double: CJSON_FLOAT,                                \
      char *: CJSON_STRING,                               \
      const char *: CJSON_STRING,                         \
      _CCOL_NULLPTR_ASSOC(CJSON_NULL) void *: CJSON_NULL, \
      default: _CJSON_TYPE_UNSUPPORTED)
#endif

/**
 * @brief Return true when the C expression has a signed integer type.
 */

/* Compile-time constant, true when the platform's plain 'char' is signed. */
#if defined(__CHAR_UNSIGNED__)
#define _CJSON_CHAR_SIGNED false
#else
#define _CJSON_CHAR_SIGNED true
#endif

#if defined(_CCOL_CLANG_GENERIC_ASSOC_WARNING)
#define _cjson_is_signed(val) \
  _cjson_is_signed_impl((val), _ccol_uniq(_cjis, __COUNTER__))
#define _cjson_is_signed_impl(val, _cjis_name)                              \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool _cjis_name = _Generic((val),                                       \
        char: _CJSON_CHAR_SIGNED,                                           \
        signed char: true,                                                  \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    _cjis_name;                                                             \
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
 * @brief Go to the DOM node at a dot-separated path.
 *
 * @param root  Root cjson handle. At the top level it is a
 *              CJSON_DICTIONARY or a CJSON_LIST.
 * @param path  Dot-separated path, a string literal or a char *.
 * @return A non-owning cjson handle, or NULL when the path doesn't exist.
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
 * The value can be a bool, any integer type, float, double, char *,
 * const char *, a char array or a string literal. Passing NULL, or the C23
 * nullptr, sets the leaf to CJSON_NULL. Before C23 the macro `true` is the
 * int 1, so cjson_set(doc, k, true) stores the CJSON_INTEGER 1 and you have
 * to write (bool)true to get a CJSON_BOOL; in C23 `true` has the type bool
 * and needs no cast. A void * value that isn't NULL, such as a void *
 * variable holding some address, is rejected with ccol_invalid_args instead
 * of being written silently as CJSON_NULL, since only a NULL counts as an
 * intentional null. A bit-field doesn't compile as val because C forbids
 * __typeof__ on one, so cast it to its integer type or to bool.
 *
 * An integer value is stored as a CJSON_INTEGER, except for an unsigned value
 * above LLONG_MAX (only a 64-bit unsigned type can hold one). That has no
 * CJSON_INTEGER form, so it is stored as the nearest CJSON_FLOAT, which is
 * what cjson_parse() makes of the same decimal literal. Such a leaf reads
 * back through cjson_double_val() and the serializer writes it in
 * floating-point form, so a value above 2^53 can lose its low digits.
 *
 * The leaf is created when it is absent, but its immediate parent must
 * already exist. If the leaf exists, its type always changes, and a list or
 * dictionary subtree already there is deep-freed.
 *
 * @param root  Root cjson handle.
 * @param path  Dot-separated path string.
 * @param val   C value, whose type _Generic works out at compile time. A
 *              string literal, a `char[N]` array and a pointer to char all
 *              give a CJSON_STRING, whatever their qualifiers. val is copied
 *              into a local of its own type after array-to-pointer
 *              conversion, with top-level qualifiers removed, so an array
 *              arrives as a pointer to its first character.
 * @return A ccol_retval_t: ccol_success on success, and an error code in every
 *         other case.
 *
 * Example:
 * @code
 * cjson_set(doc, "users.#0.active", (bool)true);
 * cjson_set(doc, "users.#0.score",  99);
 * cjson_set(doc, "users.#0.tag",    "champion");
 * @endcode
 */
#define cjson_set(root, path, val) \
  _cjson_set_impl((root), (path), (val), _ccol_uniq(_cjson_sv, __COUNTER__))

/* The public macro above names the temporary of this body with _ccol_uniq(),
 * which lets cjson_set() nest inside its own argument, and inside the
 * argument of any other public macro, under -Wshadow. */
#define _cjson_set_impl(root, path, val, _cjson_sv_name)              \
  ({                                                                  \
    __auto_type _cjson_sv_name = (val);                               \
    _cjson_set_typed((root), (path), _cjson_type_of(_cjson_sv_name),  \
                     (void *)&_cjson_sv_name, sizeof(_cjson_sv_name), \
                     _cjson_is_signed(_cjson_sv_name));               \
  })

/**
 * @brief Remove and deep-free the DOM node that a dot-separated path addresses.
 *
 * The macro walks to the parent of the addressed node, then removes the child
 * and frees it recursively. Under a dictionary parent the path addresses the
 * leaf by its key, and under a list parent the leaf has to be a '#N'
 * component.
 *
 * @param root  Root cjson handle.
 * @param path  Dot-separated path string, with the syntax of cjson_get and
 *              cjson_set.
 * @return A ccol_retval_t: ccol_success on success, and an error code in every
 *         other case.
 *
 * Example:
 * @code
 * cjson_delete(doc, "users.#0.address");
 * cjson_delete(doc, "config.debug");
 * cjson_delete(doc, "items.#2");
 * @endcode
 */
#define cjson_delete(root, path) _cjson_delete((root), (path))

#pragma GCC visibility pop
