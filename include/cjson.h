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

/* Every declaration from here to the end of this header is part of the
 * public Application Binary Interface (ABI) of libccollections. The shared
 * library exports all of them. The build of the library uses
 * -fvisibility=hidden. A function or object that no such block covers stays
 * internal to the library. It is absent from the dynamic symbol table of
 * the library. The application that links against the library cannot
 * interpose it. A symbol with the same name in that application cannot
 * collide with it. */
#pragma GCC visibility push(default)

/**
 * @file cjson.h
 * @brief Generic JSON parser, serializer, and mutable in-memory DOM.
 *
 * The Document Object Model (DOM) uses the library's own cvector for lists
 * and chashmap for dictionaries. Every node is a heap-allocated
 * cjson_node_t. The full struct definition is in cjson.c and is opaque to
 * the caller.
 *
 * ### Custom memory management
 *
 * Every factory function and the parser take a `ccol_memmgmt_procs_t *mp`
 * parameter.  Pass NULL to use the default malloc/free/calloc/realloc.
 *
 * The library keeps its own copy of the procs struct, and the nodes point
 * at that copy and never at the struct of the caller. The caller may
 * therefore build the struct on the stack of a helper function, or release
 * it, while trees built with it are still in use. The functions that it
 * names must stay callable until the last node built with them is
 * destroyed. The copies live for the life of the process, one for each
 * distinct set of four functions, and nodes built from two structs with
 * the same four functions share one copy. A process can use at most 64
 * distinct sets; a factory function or a parse that would need a 65th
 * fails (NULL).
 *
 * Each node in the DOM tree stores the allocator. All the nodes in one tree
 * must carry the same allocator. This is true when the same chain of
 * factory calls or parse calls creates all the nodes. The library uses the
 * allocator for:
 *   - The node struct itself
 *   - Owned string copies (CJSON_STRING values, dictionary keys)
 *   - The cvec of a list and the chmap of a dictionary
 *   - Temporary path copies in cjson_get / cjson_set
 *   - The buffer that cjson_serialize() returns
 *
 * **Thread-local node pool:** A thread-local free-list pool of up to 512
 * nodes makes the common case faster. The common case is the default
 * allocator, which is a NULL mp. A node with a custom allocator never uses
 * the pool. The library allocates and frees such a node directly. The
 * library drains the pool automatically when the thread exits.
 *
 * **Serialization:** cjson_serialize() and cjson_serialize_pretty() return
 * a buffer that comes from the allocator of the root node. Pass the same mp
 * to cjson_serialize_free() to free the buffer correctly.
 *
 * ### Text encoding
 *
 * RFC 8259 section 8.1 needs JSON text to be UTF-8. Every string that a
 * tree holds is therefore well-formed UTF-8. This covers a CJSON_STRING
 * value and a dictionary key alike. You can always treat the result of
 * cjson_str_val() as UTF-8. cjson_serialize() always makes valid JSON text.
 *
 * Both ways for bytes to get in refuse what is not UTF-8. Neither one
 * repairs it:
 *
 *   - The parser decodes text that a peer gives. A string literal or a key
 *     that holds a raw byte sequence that is not well-formed UTF-8 (a
 *     truncated sequence, a continuation byte with no lead byte, an
 *     overlong form, an encoded surrogate, a code point above U+10FFFF, or
 *     a byte from 0xF5 to 0xFF) is a parse error. So is a \uXXXX escape of a
 *     surrogate that is not one half of a high-low pair. The message names
 *     the defect, the bytes as 0xNN or the escape, and the byte offset in
 *     the input, and it is always printable ASCII. A repair would let two
 *     keys that differ only in their ill-formed bytes become one key.
 *   - The calling program gives a C string to cjson_create_string(),
 *     cjson_set() and cjson_dictionary_set(). A string that is not valid
 *     UTF-8 is a defect in that program. These functions therefore report
 *     it with NULL or with ccol_invalid_args. They treat it exactly as they
 *     treat a non-finite double.
 *
 * ### Nesting depth
 *
 * A parse, a clone and a serialize each accept at most 500 levels of
 * nesting. Each list or dictionary is one level, in any combination, and
 * a scalar adds none: 500 containers inside one another, with anything
 * inside the innermost of them, are accepted, and a 501st container is
 * not. Each one reports a deeper document as an error. All three walk
 * the nesting with an explicit worklist on the heap. The limit is therefore
 * a policy limit on what the library accepts. It is not a bet on the stack
 * size of the calling thread. A document at the limit costs the same small,
 * fixed amount of native stack as a flat one. cjson_destroy() has no limit
 * at all.
 *
 * ### Memory of a parse
 *
 * The tree that a parse builds is linear in the length of the input, and
 * JSON has no construct that copies one part of a document into another. An
 * empty list or dictionary costs one node; its backing store is created by
 * its first member. The densest documents (deep chains of one-member
 * dictionaries, or long lists of one-member dictionaries) hold at most about
 * 65 bytes of tree for each byte of input on a 64-bit target. A program
 * that parses input from a peer it does not trust bounds the memory of a
 * parse by bounding the length of that input.
 *
 * ### Path syntax (cjson_get / cjson_set)
 *
 * A path is a string of components that dots separate, for example
 * "users.#0.address.city".
 *
 *   - A plain component addresses a dictionary key.
 *   - A component that starts with '#' and then has decimal digits
 *     addresses a list element by a zero-based index. This is only true
 *     when the current node is a list. If it is not, the library uses the
 *     whole component as a literal dictionary key, and the '#' is part of
 *     that key.
 *   - An empty path string is a no-op for cjson_get, which returns the
 *     root. It is an error for cjson_set.
 *
 * ### Ownership
 *
 * - cjson_create_*(), cjson_parse(), cjson_parse_mp(), and cjson_clone()
 *   return trees that the caller owns completely.
 * - On success, cjson_list_push() and cjson_dictionary_set() transfer the
 *   ownership of the child to the parent. Do not free the child after such
 *   a call. The child must not already be attached to a list parent or a
 *   dictionary parent. The child must also not already contain the target
 *   container inside its own subtree. A new node is a correct child, and so
 *   is a new cjson_clone(). Four kinds of node are not correct children.
 *   The first is a borrowed reference from cjson_get(), cjson_list_get()
 *   or cjson_dictionary_get(). The second is the target container itself.
 *   The third is an ancestor of the target container. The fourth is a node
 *   that cjson_list_remove() or cjson_dictionary_remove() removed. Both of
 *   those functions always deep-free what they remove. Read the doc
 *   comment of each function for the exact rule and for its one no-op
 *   exception.
 * - On failure those two functions split by return code. This split is what
 *   makes a failure actionable. ccol_invalid_args ALWAYS means that the
 *   function rejected the arguments. The child is untouched and still
 *   belongs to the caller. Every other non-success code ALWAYS means that
 *   ownership transferred, and that the function already destroyed the
 *   child.
 * - cjson_get() returns a NON-OWNING reference. The reference is valid
 *   until something changes the tree or destroys it.
 * - cjson_destroy() frees the whole subtree recursively and sets the handle
 *   to NULL.
 */

/* ========================================================================== */
/*                         NODE TYPE TAG                                      */
/* ========================================================================== */

/**
 * @brief JSON value kind tag.
 *
 * The library uses this tag and not the general-purpose ccol_data_type.
 * This set covers all seven JSON value kinds. It covers the composite types
 * list and dictionary, which ccol_data_type does not have. It also covers
 * null and boolean.
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
 *
 * @p val must be valid UTF-8. JSON text is UTF-8 (RFC 8259 section 8.1).
 * Any other byte sequence has no JSON form. This function therefore refuses
 * such a sequence and does not store it. It does the same as
 * cjson_create_double_mp(), which refuses a non-finite double.
 *
 * @param val  Null-terminated, valid UTF-8 string. The node makes an owned
 *             copy of it. NULL gives a CJSON_NULL node.
 * @param mp   Custom allocator, or NULL for default.
 * @return New CJSON_STRING node. Returns NULL when the allocation fails or
 *         when @p val is not valid UTF-8.
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
 * @param err_str   Out-parameter for the error message when the parse
 *                  fails. On success the function sets *err_str to NULL. On
 *                  failure it points *err_str at a message that the LIBRARY
 *                  owns. Never free that string, and never free it with
 *                  cjson_serialize_free_mp(): it is not an allocation. This
 *                  is the same rule that the err out-parameter of every
 *                  other module in this library follows. The text stays
 *                  valid until the next FAILING parse on the same thread,
 *                  which is the lifetime that strerror(3) and dlerror(3)
 *                  give. The storage is per-thread, so two threads that
 *                  parse at the same time never overwrite one another's
 *                  message. Copy the text if you need it past that point.
 *                  Pass NULL to ignore the error details. The message is
 *                  printable ASCII whatever the input holds: it quotes a
 *                  printable ASCII byte of the input as it is, and spells
 *                  any other byte as 0xNN.
 * @param mp        Custom allocator for all the nodes in the new tree, or
 *                  NULL for the default allocator.
 * @return The root cjson node on success. Returns NULL when the parse
 *         fails.
 *
 * A number always uses '.' as the decimal separator, as RFC 8259 states.
 * The LC_NUMERIC locale of the calling thread does not change this.
 *
 * A string that is not well-formed UTF-8 is a parse error, so every string
 * in the new tree is well-formed UTF-8. A document with more than 500 levels
 * of nesting is a parse error. Read the "Text encoding" and "Nesting depth"
 * sections of this header.
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
 * @param err_str   Out-parameter for the error message. It behaves in the
 *                  same way as the one of cjson_parse_mp, and it has the
 *                  same ownership rule: the library owns the string and the
 *                  caller never frees it. Pass NULL to ignore the error
 *                  details.
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
 * The buffer comes from the same allocator as the root node. The way that
 * the caller built the tree therefore decides which function frees the
 * buffer. Free the buffer of a tree that uses the default allocator with
 * cjson_serialize_free(). Free the buffer of a tree that uses a custom
 * allocator with cjson_serialize_free_mp(), and pass that same allocator.
 * cjson_serialize_free() gives the buffer to a plain free(), which never
 * allocated the buffer of a custom allocator.
 *
 * A float value or a double value always uses '.' as the decimal separator,
 * as RFC 8259 states. The LC_NUMERIC locale of the calling thread does not
 * change this. The output is always valid JSON text, and this includes the
 * UTF-8. Read the "Text encoding" section of this header.
 *
 * This function refuses a subtree with more than 500 levels of nesting. It
 * reports that refusal in the same way as a failure of an allocation, and
 * cjson_clone() does the same. The serialize walks the nesting with an
 * explicit worklist on the heap. A tree at the limit therefore costs the
 * same small, fixed amount of native stack as a flat one.
 *
 * @param node  Root of the tree or of the subtree.
 * @return A heap-allocated, null-terminated string. Free it with
 *         cjson_serialize_free(). Returns NULL when the allocation fails or
 *         when the tree has too many levels of nesting.
 */
char *cjson_serialize(cjson node);

/**
 * @brief Serialize a DOM tree to an indented JSON string.
 *
 * @param node    Root of the tree or of the subtree.
 * @param indent  Spaces for each indent level. A 0 gives a default of 4
 *                spaces.
 * @return A heap-allocated string that comes from the allocator of the root
 *         node. Free it with cjson_serialize_free() when the tree uses the
 *         default allocator. Free it with cjson_serialize_free_mp() when
 *         the tree uses a custom allocator. This is exactly how you free
 *         the result of cjson_serialize(). Returns NULL when the allocation
 *         fails or when the tree has too many levels of nesting. The limit
 *         is the same as the limit of cjson_serialize().
 */
char *cjson_serialize_pretty(cjson node, unsigned int indent);

/**
 * @brief Free a string returned by cjson_serialize() or
 *        cjson_serialize_pretty().
 *
 * @param s   String to free. It can be NULL.
 * @param mp  The same allocator that the tree used at its creation. This is
 *            the mp that the caller gave to cjson_parse_mp() or to
 *            cjson_create_*_mp(). Pass NULL for the default allocator.
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

/** Returns a string literal for the @p node. Use it to report a mismatch of
 * types.
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
 * Calls ccol_fatal_err() if the node's type is not CJSON_BOOL.
 */
bool cjson_bool_val(cjson node);

/**
 * @brief Return the integer value.
 * Calls ccol_fatal_err() if the node's type is not CJSON_INTEGER.
 */
long long cjson_int_val(cjson node);

/**
 * @brief Return the floating-point value.
 * Calls ccol_fatal_err() if the node's type is not CJSON_FLOAT.
 */
double cjson_double_val(cjson node);

/**
 * @brief Return the string value. The node owns it, so do not free it.
 *
 * The string is always valid UTF-8, whatever the source of the tree is.
 * Read the "Text encoding" section of this header.
 *
 * Calls ccol_fatal_err() if the node's type is not CJSON_STRING.
 */
const char *cjson_str_val(cjson node);

/**
 * @brief Return the element count of a list node.
 * Calls ccol_fatal_err() if the node's type is not CJSON_LIST.
 */
size_t cjson_list_len(cjson node);

/**
 * @brief Return the key count of a dictionary node.
 * Calls ccol_fatal_err() if the node's type is not CJSON_DICTIONARY.
 */
size_t cjson_dictionary_size(cjson node);

/* ========================================================================== */
/*                          LIST / DICTIONARY MANIPULATION                    */
/* ========================================================================== */

/**
 * @brief Append a child node to a list.
 *
 * On success the ownership of @p child transfers to @p arr. Do not free
 * @p child after such a call.
 *
 * On failure, one return code carries one rule of ownership.
 * ccol_invalid_args ALWAYS means that the call rejected its arguments and
 * touched nothing. @p child is exactly as it was before the call. The
 * caller still owns it, or the container that it was already attached to
 * still owns it. The owner must still destroy it. Every other non-success
 * code ALWAYS means that the ownership transferred and that the call
 * already destroyed @p child. A second free of @p child is then a double
 * free. Without this split no caller can act on a code at all. A free on
 * ccol_invalid_args would double-free the rejections. No free on
 * ccol_not_enough_memory would leak the transfers.
 *
 * @p child must not already be attached to a list parent or a dictionary
 * parent. It must be a new node, or a new cjson_clone(). It must never be a
 * borrowed reference that cjson_get(), cjson_list_get() or
 * cjson_dictionary_get() returned. It must never be @p arr itself. Note:
 * cjson_list_remove() and cjson_dictionary_remove() always deep-free the
 * node that they remove. They never give back a live handle to it. A
 * removed node is therefore never a valid @p child either. The function
 * rejects an already-attached node with ccol_invalid_args and leaves
 * @p child completely untouched. The container that @p child was already
 * attached to still owns it. To accept such a node would give the same node
 * two owners. Each owner would then free the node on its own when its own
 * parent is destroyed.
 *
 * @p child must also not already contain @p arr somewhere inside its own
 * subtree. That is, @p child must not be an ancestor of @p arr in the tree
 * as the tree stands today. To attach @p child would make @p arr a new
 * ancestor of @p child through this call. @p arr would also stay a
 * descendant of @p child. That is a cycle in the graph. The function
 * rejects a cycle that it finds in the same way, with ccol_invalid_args and
 * with @p child untouched. The function cannot always complete the check
 * itself, because the check needs memory. When the check fails for that
 * reason, the function returns ccol_not_enough_memory instead, because to
 * continue silently could let a cycle through that nothing found. This call
 * then destroys @p child, as it does for every other outcome that transfers
 * ownership.
 *
 * These are therefore the ccol_invalid_args rejections. @p child is NULL.
 * @p child is already attached to a list or to a dictionary. @p child is
 * @p arr itself. @p arr is NULL or is not a CJSON_LIST. An attach of
 * @p child would close a cycle that the check finds. Two outcomes
 * transfer ownership instead. The cycle check runs out of memory, or the
 * insert itself fails.
 *
 * @param arr    Target list node, which must be a CJSON_LIST.
 * @param child  Child to append.
 * @return ccol_success. Or ccol_invalid_args, when arr or child is NULL or
 *         has the wrong type. The function also gives ccol_invalid_args
 *         when child is already attached somewhere else, and when child
 *         already contains arr. The function leaves child untouched in
 *         every one of those cases. Or
 *         ccol_not_enough_memory. Or ccol_container_full, when arr already
 *         holds its maximum element count. The function destroys child for
 *         those last two codes.
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
 * On success the ownership of @p child transfers to @p obj. If the key
 * already exists, the function stores the new child first. Only then does
 * it deep-free the previous child. A failed insert therefore never leaves
 * the slot with a dangling pointer.
 *
 * The split on failure is exactly the split of cjson_list_push().
 * ccol_invalid_args ALWAYS means that the function rejected the arguments,
 * that @p child is untouched, and that @p child still belongs to the
 * caller. Every other non-success code ALWAYS means that the ownership
 * transferred and that the function already destroyed @p child.
 *
 * @p child must not already be attached to a list parent or a dictionary
 * parent. There is one exception. To pass back the exact node that @p key
 * already holds is a harmless no-op. An example is
 * `cjson_dictionary_set(obj, k, cjson_dictionary_get(obj, k))`. The
 * function rejects any other already-attached @p child with
 * ccol_invalid_args and leaves it completely untouched. This covers a node
 * under a different key, a node in a different container, and @p obj
 * itself. The reason is the reason that cjson_list_push() describes. That
 * description also covers cjson_list_remove() and
 * cjson_dictionary_remove(). Both always deep-free the node that they
 * remove. A removed node is therefore never a valid @p child either.
 *
 * @p child must also not already contain @p obj somewhere inside its own
 * subtree. That is, @p child must not be an ancestor of @p obj in the tree
 * as the tree stands today. Read the doc comment of cjson_list_push(). It
 * gives the reason why the function rejects this in the same way as a
 * double attach. It also gives the reason why the check itself can fail
 * with ccol_not_enough_memory when there is not enough memory.
 *
 * These are therefore the ccol_invalid_args rejections. @p child is NULL.
 * @p child is already attached under a different key or in another
 * container. @p child is @p obj itself. @p obj is NULL or is not a
 * CJSON_DICTIONARY. @p key is NULL or is not valid UTF-8. An attach of
 * @p child would close a cycle that the check finds. Two outcomes
 * transfer ownership instead. The cycle check runs out of memory, or the
 * insert itself fails.
 *
 * Every key that the tree holds is valid UTF-8. This is what lets
 * cjson_serialize() emit the key without a change. The function therefore
 * refuses a @p key that is not valid UTF-8 and does not store it.
 *
 * @param obj    Target dictionary node, which must be a CJSON_DICTIONARY.
 * @param key    Null-terminated key string, which must be valid UTF-8. The
 *               dictionary stores a copy of it.
 * @param child  Value node.
 * @return ccol_success. Or ccol_invalid_args, when obj, key or child is
 *         NULL or has the wrong type. The function also gives
 *         ccol_invalid_args when key is not valid UTF-8, when child is
 *         already attached somewhere else, and when child already
 *         contains obj. Or ccol_not_enough_memory. Or ccol_container_full,
 *         when obj already holds its maximum element count.
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
 * The function moves every element after the index one position to the
 * left. It frees the removed subtree recursively.
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
 * The function frees the removed subtree recursively.
 *
 * @param obj  Target dictionary node, which must be a CJSON_DICTIONARY.
 * @param key  Null-terminated key string.
 * @return ccol_success on success.
 *         ccol_invalid_args when obj is NULL or is not a dictionary.
 *         ccol_key_not_found when the key does not exist.
 */
ccol_retval_t cjson_dictionary_remove(cjson obj, const char *key);

/* ========================================================================== */
/*                         DICTIONARY ITERATION                               */
/* ========================================================================== */

/**
 * @brief A cursor over the members of a dictionary, in insertion order.
 *
 * The caller owns the struct, usually on its own stack. The iteration
 * allocates nothing and cannot fail. Fill it with cjson_dictionary_first()
 * and step it with cjson_dictionary_next(). Read only @c key and @c value.
 * The fields whose names begin with an underscore are private.
 *
 * The order is the order in which each key first entered the dictionary:
 * the order of the members in the parsed text, or the order of the
 * cjson_dictionary_set() and cjson_set() calls that added them. A
 * replacement of the value of a key keeps its place, so a key that a
 * document repeats keeps the place of its first occurrence and the value
 * of its last. A removed key leaves the order. The
 * serializers and cjson_clone() use the same order.
 *
 * The cursor holds the member that the next step reads, which is the
 * successor of the member that the last successful call gave. It stays
 * valid across every change to the dictionary that leaves that successor
 * in place:
 *
 *   - a replacement of the value of any member, the current one included,
 *     through cjson_dictionary_set() or cjson_set(). The member keeps its
 *     place, so the iteration goes on in the same order;
 *   - the removal of the current member, or of any member other than the
 *     successor;
 *   - an insert of a new key. The new member goes after every other
 *     member. Whether this iteration still reaches it is unspecified.
 *
 * A removal of the successor, through cjson_dictionary_remove(),
 * cjson_delete() or any other call, invalidates the cursor, and so does a
 * destroy of the dictionary. Start again with cjson_dictionary_first()
 * after such a change.
 *
 * The @c key and @c value fields describe the current member as it was
 * when the cursor stepped onto it. A replacement of the value of the
 * current member through cjson_dictionary_set() destroys the old value, so
 * @c value then names freed memory; read the new value with
 * cjson_dictionary_get() and @c key. cjson_set() updates the existing
 * node in place, so @c value stays the same node.
 */
typedef struct cjson_dictionary_iter {
  /** The key of the current member. It stays valid until that member is
   *  removed or the dictionary is destroyed. NULL when the iteration is
   *  over. */
  const char *key;
  /** The value of the current member. It is a borrowed reference, with the
   *  rules of cjson_dictionary_get(). NULL when the iteration is over. */
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
 * @return true when @p it now stands on the first member. false when the
 *         dictionary is empty, when @p dict is NULL, or when @p dict is not
 *         a CJSON_DICTIONARY. The function then sets @c key and @c value to
 *         NULL. false also when @p it is NULL.
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
 * @return true when @p it now stands on the next member. false when the
 *         previous call gave the last member, or when an earlier call
 *         already returned false. The function then sets @c key and
 *         @c value to NULL. false also when @p it is NULL.
 */
bool cjson_dictionary_next(cjson_dictionary_iter *it);

/* ========================================================================== */
/*                         DEEP COPY                                          */
/* ========================================================================== */

/**
 * @brief Return a fully independent deep copy of a DOM subtree.
 *
 * The clone uses the same allocator as the source tree. That allocator is
 * the m_procs that the root node stores.
 *
 * This function refuses a subtree with more than 500 levels of nesting. It
 * reports that refusal in the same way as a failure of an allocation. The
 * limit is the same as the limit of the parser. You can therefore clone any
 * tree that parses. Only a direct call to cjson_list_push() or to
 * cjson_dictionary_set() can build a deeper tree. The clone walks the
 * nesting with an explicit worklist on the heap. A tree at the limit
 * therefore costs the same small, fixed amount of native stack as a flat
 * one.
 *
 * @return The new root node, which the caller owns. Returns NULL when the
 *         allocation fails or when the subtree has too many levels of
 *         nesting.
 */
cjson cjson_clone(cjson node);

/* ========================================================================== */
/*                         LIFECYCLE MACROS                                   */
/* ========================================================================== */

/**
 * @brief Declare an uninitialized cjson variable.
 *
 * This macro is the same as `cjson var_name`. It is here to keep the same
 * convention for names as the other container modules. Assign the variable
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
 * The GCC cleanup attribute destroys the variable automatically at the end
 * of its scope. It also sets the variable to NULL. Initialize the variable
 * before you use it.
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
 * A call on NULL is safe. Each node frees its own memory with the allocator
 * that it stores, which is its m_procs. The macro needs no external
 * allocator parameter.
 *
 * @note The macro evaluates node exactly once. It must be a modifiable
 * lvalue, such as a variable or an element of an array
 */
#define cjson_destroy(node)      \
  _ccol_cjson_destroy_impl(node, \
                           _ccol_uniq(__ccol_cjson_destroy_slot, __COUNTER__))

/* Internal. The body of cjson_destroy. slot is a name from _ccol_uniq(), so
 * the macro nests inside the argument of another destroy macro and stays
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
 *         a type does not match.
 */
cjson _cjson_get(cjson root, const char *path);

/**
 * @brief Remove and deep-free the node that a path addresses. This is the
 *        back-end.
 *
 * Use the cjson_delete() macro.
 *
 * The function goes to the parent of the node that the path addresses. It
 * then calls cjson_dictionary_remove() or cjson_list_remove(), whichever
 * one is correct. The path uses the same dot-separated syntax as
 * _cjson_get and _cjson_set_typed. That syntax includes the escape
 * sequences.
 *
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL path and for an empty path. The
 *         function also returns it for an empty path component, which a
 *         leading dot, a trailing dot or two dots together give. It
 *         returns it for a wrong parent type too, and for a malformed
 *         "#N" index.
 *         ccol_key_not_found when the parent path is absent. The function
 *         also returns it when a leaf key or index with a correct syntax is
 *         absent.
 *         ccol_not_enough_memory when an allocation fails.
 */
ccol_retval_t _cjson_delete(cjson root, const char *path);

/**
 * @brief Write a typed scalar value to the leaf that a path addresses. This
 *        is the back-end.
 *
 * Use the cjson_set() macro.
 *
 * The function creates the leaf when the leaf is absent. The parent must
 * exist. The function replaces and deep-frees any value that is already
 * there, and this includes a full subtree. A change of type is therefore
 * safe.
 *
 * @param root             Root of the DOM tree.
 * @param path             Dot-separated path string.
 * @param type             JSON type of the new value.
 * @param raw              Pointer to the raw C value. The value can have
 *                         any type, and the caller passes it as a void *.
 * @param raw_size         The sizeof() of the original C expression. The
 *                         function checks it against @p type before it
 *                         reads anything through @p raw. This check is
 *                         necessary, because a direct call to this back-end
 *                         can give a payload that is narrower than the type
 *                         needs.
 * @param is_signed        True when the integer source type is signed. An
 *                         unsigned 8-byte value above LLONG_MAX is stored
 *                         as a CJSON_FLOAT; read the doc comment of
 *                         cjson_set().
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL root or path, and for an empty path.
 *         The function also returns it for an empty path component, which
 *         a leading dot, a trailing dot or two dots together give. It
 *         returns it for a path component that is not valid UTF-8, for a
 *         wrong parent type, and for a malformed "#N" index. It returns it
 *         for a C type that the function does not support, and for a
 *         @p raw_size that does not match @p type. It returns it for a
 *         string value that is not valid UTF-8, and for a void * value
 *         that is not NULL. A NULL @p raw for any type
 *         other than CJSON_NULL also gives this code. For a void *, the
 *         function accepts only a bare NULL. Read the doc comment of
 *         cjson_set().
 *         ccol_key_not_found when the parent path is absent. The function
 *         also returns it for a list index that has a correct syntax but is
 *         out of range.
 *         ccol_not_enough_memory when an allocation fails.
 *         ccol_container_full when the parent dictionary already holds its
 *         maximum element count. This happens only for a new key.
 */
ccol_retval_t _cjson_set_typed(cjson root, const char *path,
                               cjson_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed);

/* ========================================================================== */
/*                         COMPILE-TIME TYPE HELPERS                         */
/* ========================================================================== */

/**
 * @brief Sentinel that _cjson_type_of() returns for a C type that
 * cjson_set() does not accept. cjson_set() accepts bool, any integer type,
 * float, double, char *, const char *, and a bare NULL. Every other type
 * gives this sentinel. Examples are long double and a struct. The default
 * association of the _Generic below catches all of them. A bare `NULL`
 * literal has the type void *, and the _Generic below gives it its own
 * explicit association, and so does the C23 nullptr, through
 * _CCOL_NULLPTR_ASSOC. Neither falls into the default. The reason is
 * that a NULL is documented and intentional use, not a mistake of the
 * caller. The doc comment of cjson_set says so.
 *
 * This sentinel is deliberately NOT a real cjson_node_type_t enumerator. No
 * node ever stores it, and cjson_type() never returns it. It only ever
 * passes through the `type` argument of _cjson_set_typed(). The switch that
 * checks the type there is node_reinit_scalar() in cjson.c. That switch
 * already rejects any value outside {CJSON_NULL, CJSON_BOOL, CJSON_INTEGER,
 * CJSON_FLOAT, CJSON_STRING}. It rejects the value with ccol_invalid_args
 * before it touches the target node. A value that differs from every real
 * enumerator is what makes that guard fire for a type that the library does
 * not support. A default of CJSON_NULL is already on the accept-list, and
 * it would pass the guard silently.
 */
#define _CJSON_TYPE_UNSUPPORTED ((cjson_node_type_t)0x7f)

/**
 * @brief Map the compile-time type of a C expression to cjson_node_type_t.
 */
#if defined __clang__
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

/* Compile-time constant. It is true when the plain 'char' of the platform
 * is signed. */
#if defined(__CHAR_UNSIGNED__)
#define _CJSON_CHAR_SIGNED false
#else
#define _CJSON_CHAR_SIGNED true
#endif

#if defined __clang__
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
 * @param path  Dot-separated path. It is a string literal or a char *.
 * @return A non-owning cjson handle. Returns NULL when the path does not
 *         exist.
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
 * The macro accepts these value types: bool, any integer type, float,
 * double, char *, const char *, a char array and a string literal. Passing
 * NULL, or the C23 nullptr, sets the leaf to CJSON_NULL. Before C23 the
 * macro `true` is the int 1, so cjson_set(doc, k, true) stores the
 * CJSON_INTEGER 1; write (bool)true for a CJSON_BOOL. In C23 `true` has the
 * type bool and needs no cast. The macro rejects a void * value that is
 * not NULL with ccol_invalid_args. An example is a variable of type void * that
 * does not hold NULL. The macro does not write such a value silently as
 * CJSON_NULL. It treats only a NULL as an intentional null.
 *
 * An integer value is stored as a CJSON_INTEGER, with one exception. An
 * unsigned value above LLONG_MAX, which only a 64-bit unsigned type can
 * hold, has no CJSON_INTEGER form. The macro stores it as the CJSON_FLOAT
 * nearest to it, which is what cjson_parse() makes of the same decimal
 * literal. Such a leaf reads back through cjson_double_val(), and the
 * serializer writes it in floating-point form, so a value above 2^53 can
 * lose its low digits.
 *
 * The macro creates the leaf when the leaf is absent. The immediate parent
 * of the leaf must already exist. If the leaf exists, the macro always
 * changes its type. It deep-frees a list subtree or a dictionary subtree
 * that is already there.
 *
 * @param root  Root cjson handle.
 * @param path  Dot-separated path string.
 * @param val   C value. _Generic finds its type at compile time. A string
 *              literal, a `char[N]` array and a pointer to char all give a
 *              CJSON_STRING, whatever their qualifiers. The macro copies
 *              val into a local whose type is the type of val after
 *              array-to-pointer conversion, with top-level qualifiers
 *              removed, so an array arrives as a pointer to its first
 *              character.
 * @return A ccol_retval_t. It is ccol_success on success, and an error code
 *         in every other case.
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
 * so cjson_set() nests inside its own argument, and inside the argument of any
 * other public macro, under -Wshadow. */
#define _cjson_set_impl(root, path, val, _cjson_sv_name)              \
  ({                                                                  \
    __auto_type _cjson_sv_name = (val);                               \
    _cjson_set_typed((root), (path), _cjson_type_of(_cjson_sv_name),  \
                     (void *)&_cjson_sv_name, sizeof(_cjson_sv_name), \
                     _cjson_is_signed(_cjson_sv_name));               \
  })

/**
 * @brief Remove and deep-free the DOM node that a dot-separated path
 *        addresses.
 *
 * The macro goes to the parent of the node that the path addresses. It then
 * removes the child and frees it recursively. For a dictionary parent, the
 * path addresses the leaf by its key. For a list parent, the leaf must be a
 * '#N' component.
 *
 * @param root  Root cjson handle.
 * @param path  Dot-separated path string. Its syntax is the syntax of
 *              cjson_get and cjson_set.
 * @return A ccol_retval_t. It is ccol_success on success, and an error code
 *         in every other case.
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
