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

// A JSON parser, serializer and mutable DOM. Lists are cvectors, dictionaries
// are chmaps, and every node is an opaque heap-allocated cjson_node.
//
// Allocator: Every factory and parse function takes a ccol_memmgmt_procs *mp
// (NULL for the default allocator). The library keeps its own process-lifetime
// copy of the struct, one per distinct set of four functions, at most 64 of
// them; a call that would need a 65th fails with NULL. So your struct may go
// away, but its functions must stay callable until the last node built with
// them is destroyed. All nodes of one tree carry the same allocator, which
// serves the nodes, owned strings, list and dictionary stores, path copies and
// the serializer buffers. With the default allocator a thread-local pool of up
// to 512 nodes speeds up allocation; it's drained when the thread exits.
//
// Text: Every string in a tree (value or key) is well-formed UTF-8. The parser
// refuses ill-formed bytes and lone surrogate escapes with a printable-ASCII
// message that names the bytes as 0xNN and the byte offset, and
// cjson_create_string(), cjson_set() and cjson_dictionary_set() refuse a C
// string that isn't UTF-8. Nothing is repaired.
//
// Nesting: Parse, clone and serialize accept at most 500 nested containers and
// report a deeper document as an error. They walk with a heap worklist, so the
// limit doesn't depend on the thread's stack. cjson_destroy() has no limit.
//
// Memory: A parsed tree is linear in its input, at most about 65 bytes of tree
// per input byte on a 64-bit target, so bounding the input bounds a parse.
//
// Paths (cjson_get, cjson_set, cjson_delete): Dot-separated components such as
// "users.#0.address.city". A plain component is a dictionary key; "#N" is a
// zero-based index, but only on a list (elsewhere it's a literal key). "\." and
// "\\" escape a dot and a backslash. An empty path gives the root for
// cjson_get and is an error for the others. A malformed component (empty, not
// UTF-8, a bad #N on a list) or a path whose shape can't apply to the tree
// (descending into a scalar, a name on a list) is ccol_invalid_args at any
// position; ccol_key_not_found means only that a well-formed name or index is
// absent. cjson_get() gives NULL in all these cases.
//
// Ownership: Create, parse and clone return trees the caller owns. A
// successful cjson_list_push() or cjson_dictionary_set() moves the child to the
// parent. On failure ccol_invalid_args always means the child is untouched and
// yours, and every other error code means the call already destroyed it.
// Everything that cjson_get() and the other getters return is borrowed and
// valid until the tree changes. cjson_destroy() frees a whole subtree.

#include "common.h"

// Everything from here to the end of the header is the public ABI and the
// shared library exports it. The library is built with -fvisibility=hidden, so
// anything outside such a block stays internal.
#pragma GCC visibility push(default)

// The tag of a node: One of the seven JSON value kinds.
typedef enum cjson_node_type {
  CJSON_NULL = 0,
  CJSON_BOOL,
  CJSON_INTEGER,
  CJSON_FLOAT,
  CJSON_STRING,
  CJSON_LIST,
  CJSON_DICTIONARY,
} cjson_node_type;

// The opaque node; cjson.c defines it.
typedef struct cjson_node cjson_node;

// The public handle.
typedef cjson_node *cjson;

// Creates a node that holds the JSON value null, for building a tree by hand.
// mp is the allocator of the node (NULL for the default allocator). Gives the
// new node, which the caller owns: Free it with cjson_destroy() or hand it to a
// parent with cjson_list_push() or cjson_dictionary_set(). NULL when the
// allocation fails or a 65th distinct allocator would be needed.
cjson cjson_create_null_mp(ccol_memmgmt_procs *mp);

// Creates a node that holds the JSON value null, with the default allocator.
// Gives the new node, which the caller owns: Free it with cjson_destroy() or
// hand it to a parent with cjson_list_push() or cjson_dictionary_set(). NULL
// when the allocation fails.
static inline cjson cjson_create_null(void) {
  return cjson_create_null_mp(NULL);
}

// Creates a CJSON_BOOL node that holds val. mp is the allocator of the node
// (NULL for the default allocator). The caller owns the new node and frees it
// with cjson_destroy(), unless a parent takes it through cjson_list_push() or
// cjson_dictionary_set(). Gives NULL when the allocation fails or a 65th
// distinct allocator would be needed.
cjson cjson_create_bool_mp(bool val, ccol_memmgmt_procs *mp);

// Creates a CJSON_BOOL node that holds val, with the default allocator. The
// caller owns the new node and frees it with cjson_destroy(), unless a parent
// takes it through cjson_list_push() or cjson_dictionary_set(). Gives NULL
// when the allocation fails.
static inline cjson cjson_create_bool(bool val) {
  return cjson_create_bool_mp(val, NULL);
}

// Creates a CJSON_INTEGER node that holds val. mp is the allocator of the node
// (NULL for the default allocator). The new node belongs to the caller, who
// frees it with cjson_destroy() or attaches it to a list or dictionary. NULL
// when the allocation fails or a 65th distinct allocator would be needed.
cjson cjson_create_int_mp(long long val, ccol_memmgmt_procs *mp);

// Creates a CJSON_INTEGER node that holds val, with the default allocator. The
// new node belongs to the caller, who frees it with cjson_destroy() or
// attaches it to a list or dictionary. NULL when the allocation fails.
static inline cjson cjson_create_int(long long val) {
  return cjson_create_int_mp(val, NULL);
}

// Creates a CJSON_FLOAT node that holds val. mp is the allocator of the node
// (NULL for the default allocator). Gives the new node, which the caller frees
// with cjson_destroy() or attaches to a parent. Gives NULL for Inf or NaN,
// which JSON can't write, when the allocation fails, or when a 65th distinct
// allocator would be needed.
cjson cjson_create_double_mp(double val, ccol_memmgmt_procs *mp);

// Creates a CJSON_FLOAT node that holds val, with the default allocator. Gives
// the new node, which the caller frees with cjson_destroy() or attaches to a
// parent. Gives NULL for Inf or NaN, which JSON can't write, and when the
// allocation fails.
static inline cjson cjson_create_double(double val) {
  return cjson_create_double_mp(val, NULL);
}

// Creates a CJSON_STRING node that holds its own copy of val; a NULL val gives
// a CJSON_NULL node. mp is the allocator of the node (NULL for the default
// allocator). The caller owns the new node and frees it with cjson_destroy(),
// unless a parent takes it. Gives NULL when val isn't valid UTF-8, when the
// allocation fails, or when a 65th distinct allocator would be needed.
cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs *mp);

// Creates a CJSON_STRING node that holds its own copy of val, with the default
// allocator; a NULL val gives a CJSON_NULL node. The caller owns the new node
// and frees it with cjson_destroy(), unless a parent takes it. Gives NULL when
// val isn't valid UTF-8 or the allocation fails.
static inline cjson cjson_create_string(const char *val) {
  return cjson_create_string_mp(val, NULL);
}

// Creates an empty CJSON_LIST node; fill it with cjson_list_push(). mp is the
// allocator of the list and of what it stores (NULL for the default
// allocator). The caller owns the list and frees it, children included, with
// cjson_destroy(). NULL when the allocation fails or a 65th distinct
// allocator would be needed.
cjson cjson_create_list_mp(ccol_memmgmt_procs *mp);

// Creates an empty CJSON_LIST node with the default allocator; fill it with
// cjson_list_push(). The caller owns the list and frees it, children included,
// with cjson_destroy(). NULL when the allocation fails.
static inline cjson cjson_create_list(void) {
  return cjson_create_list_mp(NULL);
}

// Creates an empty CJSON_DICTIONARY node; fill it with cjson_dictionary_set().
// mp is the allocator of the dictionary and of what it stores (NULL for the
// default allocator). The caller owns it and frees it, values included, with
// cjson_destroy(). NULL when the allocation fails or a 65th distinct
// allocator would be needed.
cjson cjson_create_dictionary_mp(ccol_memmgmt_procs *mp);

// Creates an empty CJSON_DICTIONARY node with the default allocator; fill it
// with cjson_dictionary_set(). The caller owns it and frees it, values
// included, with cjson_destroy(). NULL when the allocation fails.
static inline cjson cjson_create_dictionary(void) {
  return cjson_create_dictionary_mp(NULL);
}

// Parses the NUL-terminated JSON text json_str into a tree, for a caller that
// reads JSON. mp is the allocator of every node (NULL for the default). Gives
// the root, which the caller owns and frees with cjson_destroy(), or NULL when
// the text isn't valid JSON (ill-formed UTF-8 included), nests more than 500
// containers, or memory runs out. On success *err_str becomes NULL.
// On failure it points at a printable ASCII message (input bytes outside
// printable ASCII appear as 0xNN) that the library owns in per-thread storage:
// never free it, and copy it if you need it past the next failing parse on the
// same thread. When the thread can't keep a buffer the message is a fixed
// text. err_str may be NULL. Numbers always use '.' whatever the LC_NUMERIC
// locale.
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs *mp);

// Parses the NUL-terminated JSON text json_str into a tree, with the default
// allocator. Gives the root, which the caller owns and frees with
// cjson_destroy(), or NULL when the text isn't valid JSON (ill-formed UTF-8
// included), nests more than 500 containers, or memory runs out. On success
// *err_str becomes NULL; on failure it points at a printable ASCII message
// that the library owns per thread: Never free it, and copy it to keep it past
// the next failing parse on the same thread. err_str may be NULL. Numbers
// always use '.' whatever the LC_NUMERIC locale.
static inline cjson cjson_parse(const char *json_str, char **err_str) {
  return cjson_parse_mp(json_str, err_str, NULL);
}

// Parses exactly len bytes of JSON text at json_str, which needn't end with a
// NUL, into a tree. mp is the allocator of every node (NULL for the default).
// Gives the root, which the caller owns and frees with cjson_destroy(), or NULL
// when the text isn't valid JSON (ill-formed UTF-8 included), nests more than
// 500 containers, or memory runs out. On success *err_str becomes NULL; on
// failure it points at a printable ASCII message that the library owns per
// thread: Never free it, and copy it to keep it past the next failing parse on
// the same thread. err_str may be NULL.
cjson cjson_parse_n_mp(const char *json_str, size_t len, char **err_str,
                       ccol_memmgmt_procs *mp);

// Parses exactly len bytes of JSON text at json_str, which needn't end with a
// NUL, into a tree with the default allocator. Gives the root, which the caller
// owns and frees with cjson_destroy(), or NULL when the text isn't valid JSON
// (ill-formed UTF-8 included), nests more than 500 containers, or memory runs
// out. On success *err_str becomes NULL; on failure it points at a printable
// ASCII message that the library owns per thread: Never free it, and copy it
// to keep it past the next failing parse on the same thread. err_str may be
// NULL.
static inline cjson cjson_parse_n(const char *json_str, size_t len,
                                  char **err_str) {
  return cjson_parse_n_mp(json_str, len, err_str, NULL);
}

// Writes the subtree at node as compact JSON text, for a caller that stores or
// sends a tree; a NULL node gives "null". Members come out in insertion order,
// and the text is valid UTF-8 with '.' as the decimal point in every locale.
// The buffer comes from the allocator of node: Free it with
// cjson_serialize_free() for the default allocator and with
// cjson_serialize_free_mp() and the same mp otherwise. Gives NULL when an
// allocation fails or the subtree is nested deeper than 500 levels.
char *cjson_serialize(cjson node);

// Writes the subtree at node as JSON text for a person to read, one member or
// element per line, indented by indent spaces per level (0 means 4); a NULL
// node gives "null". Members come out in insertion order, and the decimal point
// is '.' in every locale. The buffer comes from the allocator of node: Free it
// with cjson_serialize_free() for the default allocator and with
// cjson_serialize_free_mp() and the same mp otherwise. Gives NULL when an
// allocation fails or the subtree is nested deeper than 500 levels.
char *cjson_serialize_pretty(cjson node, unsigned int indent);

// Frees a buffer that cjson_serialize() or cjson_serialize_pretty() gave. mp
// must be the allocator the tree was built with (NULL for the default). s may
// be NULL, and the call then does nothing.
void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs *mp);

// Frees a buffer that cjson_serialize() or cjson_serialize_pretty() gave for a
// tree built with the default allocator; a tree with its own allocator needs
// cjson_serialize_free_mp(). s may be NULL, and the call then does nothing.
static inline void cjson_serialize_free(char *s) {
  cjson_serialize_free_mp(s, NULL);
}

// Gives the type tag of node (CJSON_NULL for a NULL node), so a caller can
// check a node before it reads it.
cjson_node_type cjson_type(cjson node);

// Gives the name of the type of node, such as "CJSON_LIST", for an error
// message; a NULL node gives "CJSON_NULL". The string is a literal that lives
// as long as the program: Never free or change it.
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

// Gives the value of a CJSON_BOOL node. Ends the program with a fatal error
// when node is NULL or of another type, so check cjson_type() first when the
// type isn't known.
bool cjson_bool_val(cjson node);

// Gives the value of a CJSON_INTEGER node. A NULL node or a node of another
// type is a fatal error that ends the program; check cjson_type() first when
// you aren't sure.
long long cjson_int_val(cjson node);

// Gives the value of a CJSON_FLOAT node. For a NULL node or a node of another
// type it ends the program with a fatal error, so check cjson_type() first
// when the type isn't known.
double cjson_double_val(cjson node);

// Gives the text of a CJSON_STRING node, always valid UTF-8. The string stays
// owned by the node: Don't free it. It's valid until the node gets a new value
// or is removed, replaced or destroyed. A NULL node or a node of another type
// is a fatal error that ends the program; check cjson_type() first when needed.
const char *cjson_str_val(cjson node);

// Gives the number of elements of a CJSON_LIST node, for a caller that walks it
// by index. Ends the program with a fatal error when node is NULL or isn't a
// list.
size_t cjson_list_len(cjson node);

// Gives the number of keys of a CJSON_DICTIONARY node. Ends the program with a
// fatal error when node is NULL or isn't a dictionary.
size_t cjson_dictionary_size(cjson node);

// Appends child to the list arr, which takes ownership on success; it's how a
// caller fills a list. child must be a new node or a new clone; a borrowed
// reference, a node already attached somewhere, arr itself or an ancestor of
// arr is refused. Those refusals, a NULL argument and a wrong type return
// ccol_invalid_args and leave child untouched. ccol_not_enough_memory (also
// when the cycle check runs out of memory) and ccol_container_full mean
// ownership moved and child is destroyed.
ccol_retval cjson_list_push(cjson arr, cjson child);

// Gives element index of the list arr, for a caller that reads a list; NULL
// when arr is NULL or not a list, or index is out of range. The node is
// borrowed: The list keeps owning it, and it stays valid until the tree
// changes. Never destroy it or attach it elsewhere; cjson_clone() it instead.
cjson cjson_list_get(cjson arr, size_t index);

// Inserts key with child in obj, or replaces its value; it's how a caller fills
// a dictionary. On success obj owns child, and the dictionary stores its own
// copy of key. A replace deep-frees the old value and keeps the key's place in
// the insertion order; passing back the node that key already holds does
// nothing and succeeds. ccol_invalid_args (a NULL argument, obj not a
// dictionary, a key that isn't UTF-8, child already attached somewhere or
// borrowed from a tree, child == obj, or obj inside the subtree of child)
// leaves child untouched and with the caller. ccol_not_enough_memory (also
// when the cycle check runs out of memory) and ccol_container_full mean
// ownership moved and child is destroyed.
ccol_retval cjson_dictionary_set(cjson obj, const char *key, cjson child);

// Gives the value of key in the dictionary obj, for a caller that reads a
// dictionary; NULL when the key is absent, obj or key is NULL, or obj isn't a
// dictionary. The node is borrowed: The dictionary keeps owning it, and it
// stays valid until the tree changes. Never destroy it or attach it elsewhere.
cjson cjson_dictionary_get(cjson obj, const char *key);

// Removes element index from the list arr and deep-frees it; the later
// elements move left. Returns ccol_success, or ccol_invalid_args for a NULL or
// non-list arr or an index out of range.
ccol_retval cjson_list_remove(cjson arr, size_t index);

// Removes the entry of key from the dictionary obj and deep-frees its value;
// the other members keep their order. Returns ccol_success, ccol_invalid_args
// when obj or key is NULL or obj isn't a dictionary, and ccol_key_not_found
// when the key is absent.
ccol_retval cjson_dictionary_remove(cjson obj, const char *key);

// A cursor over a dictionary in insertion order, which the caller owns; it
// allocates nothing and can't fail. A key keeps the place where it first
// entered (a repeated key in parsed text keeps its first place and its last
// value). The cursor holds the successor of the current member and stays valid
// across any change that leaves that successor in place: Replacing values,
// removing other members, inserting (whether this iteration sees a new key is
// unspecified). Removing the successor or destroying the dictionary invalidates
// it. After cjson_dictionary_set() replaces the current value, value names
// freed memory; cjson_set() updates the node in place.
typedef struct cjson_dictionary_iter {
  // The current member; both NULL once the iteration is over.
  const char *key;
  cjson value;
  // Private: The member that the next step reads.
  const void *cjson__next;
} cjson_dictionary_iter;

// Starts the cursor it on dict and steps onto its first member, so a caller can
// walk a dictionary in insertion order. Gives false (with key and value NULL)
// for an empty, NULL or non-dictionary dict, and false for a NULL it.
//
//   cjson_dictionary_iter it;
//   for (bool ok = cjson_dictionary_first(obj, &it); ok;
//        ok = cjson_dictionary_next(&it))
//     printf("%s: %s\n", it.key, cjson_type_str(it.value));
bool cjson_dictionary_first(cjson dict, cjson_dictionary_iter *it);

// Steps the cursor it, which cjson_dictionary_first() started, onto the next
// member in insertion order and gives true, with it->key and it->value
// borrowed from the dictionary. Gives false (key and value NULL) after the
// last member or for a NULL it. Removing the member that the cursor reads next,
// or destroying the dictionary, invalidates the cursor.
bool cjson_dictionary_next(cjson_dictionary_iter *it);

// Makes a deep copy of the subtree at node with node's allocator, for a caller
// that wants an independent tree (for example to attach a copy of a borrowed
// node). Dictionaries keep their member order. Gives the copy, which the
// caller owns and frees with cjson_destroy(), or NULL for a NULL node, when an
// allocation fails, or when the subtree is nested deeper than 500 levels.
cjson cjson_clone(cjson node);

// Declares an uninitialized cjson named var_name; assign it, for example from
// cjson_parse() or a cjson_create_*() call, before you use it. It exists so
// cjson follows the naming convention of the other containers.
#define cjson_declare(var_name) cjson var_name

// Declares a cjson named var_name, set to NULL, whose tree is destroyed at the
// end of its scope on every way out, so a function with several exits can't
// leak its tree. Store only a tree the caller owns in it, never a borrowed
// node from cjson_get() or another getter. Needs GCC or Clang.
#define cjson_declare_scoped(var_name) \
  cjson var_name ccol__destructor(cjson__destroy_cleanup) = NULL

// Internal: Frees the tree at node. It's the body of cjson_destroy(); use the
// macro.
void cjson__destroy(cjson node);

// Internal: Destroys *node and sets it to NULL. It's the cleanup function that
// cjson_declare_scoped() attaches.
static inline void cjson__destroy_cleanup(cjson *node) {
  if (node && *node) {
    cjson__destroy(*node);
    *node = NULL;
  }
}

// Frees the tree at node (each node with its own allocator) and sets the handle
// to NULL, so a later use is a plain NULL instead of a dangling pointer. NULL
// is safe. node is evaluated once and must be a modifiable lvalue. Destroy
// only a root the caller owns, never a borrowed node or one attached to a
// parent; remove a child with cjson_list_remove() or cjson_dictionary_remove().
#define cjson_destroy(node)      \
  ccol__cjson_destroy_impl(node, \
                           ccol__uniq(ccol__cjson_destroy_slot, __COUNTER__))

// Internal: The body of cjson_destroy(). slot is a ccol__uniq() name for the
// pointer to node, so nested destroy macros stay -Wshadow clean.
#define ccol__cjson_destroy_impl(node, slot) \
  do {                                       \
    __typeof__(node) *slot = &(node);        \
    if (*slot) {                             \
      cjson__destroy(*slot);                 \
      *slot = NULL;                          \
    }                                        \
  } while (0)

// Internal: Gives the node at path, root for an empty path, or NULL; it's the
// back-end of cjson_get(). A path of 256 bytes or more is copied with the
// allocator of root, and NULL also means that copy failed.
cjson cjson__get(cjson root, const char *path);

// Internal: Removes and deep-frees the node at path; it's the back-end of
// cjson_delete(). It walks to the parent and removes the leaf through
// cjson_dictionary_remove() or cjson_list_remove(). Errors follow the path
// rules at the top of this header; ccol_not_enough_memory when an allocation
// fails.
ccol_retval cjson__delete(cjson root, const char *path);

// Internal: Writes a scalar to the leaf at path; it's the back-end of
// cjson_set(). type is the JSON type, raw points at the C value of raw_size
// bytes, and is_signed says whether an integer source was signed. raw_size is
// checked against type before anything is read, because a direct call can pass
// a narrower payload. The leaf is created when absent (its parent must exist)
// and any old value is deep-freed. It returns ccol_invalid_args for the path
// errors at the top of this header, an unsupported type, a size mismatch, a
// string that isn't UTF-8, a void * that isn't NULL, and a NULL raw for any
// type but CJSON_NULL; ccol_key_not_found for an absent parent or an
// out-of-range leaf index; ccol_not_enough_memory; ccol_container_full for a
// new key in a full dictionary.
ccol_retval cjson__set_typed(cjson root, const char *path, cjson_node_type type,
                             void *raw, size_t raw_size, bool is_signed);

// What cjson__type_of() gives for a type that cjson_set() refuses (long double,
// a struct). It's on purpose not a real cjson_node_type, so the type check in
// cjson__set_typed() refuses it; CJSON_NULL as a default would pass that check.
// NULL and the C23 nullptr have their own association, because passing them is
// intended.
#define CJSON__TYPE_UNSUPPORTED ((cjson_node_type)0x7f)

// Evaluates to the cjson_node_type that the compile-time type of val maps to,
// or CJSON__TYPE_UNSUPPORTED; cjson_set() uses it to tell the back-end what
// kind of value it passes. val is not evaluated. The Clang form silences the
// warning about unreachable _Generic associations.
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define cjson__type_of(val) \
  cjson__type_of_impl((val), ccol__uniq(cjson__type_of_tmp, __COUNTER__))
#define cjson__type_of_impl(val, cjson__type_of_name)                       \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    cjson_node_type cjson__type_of_name = _Generic((val),                   \
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
        CCOL__NULLPTR_ASSOC(CJSON_NULL) void *: CJSON_NULL,                 \
        default: CJSON__TYPE_UNSUPPORTED);                                  \
    _Pragma("GCC diagnostic pop");                                          \
    cjson__type_of_name;                                                    \
  })
#else
#define cjson__type_of(val)                               \
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
      CCOL__NULLPTR_ASSOC(CJSON_NULL) void *: CJSON_NULL, \
      default: CJSON__TYPE_UNSUPPORTED)
#endif

// True when plain char is signed on this platform, so cjson__is_signed() can
// classify a char value.
#if defined(__CHAR_UNSIGNED__)
#define CJSON__CHAR_SIGNED false
#else
#define CJSON__CHAR_SIGNED true
#endif

// Evaluates to true when val has a signed integer type; cjson_set() passes it
// on so the back-end widens the integer correctly. val is not evaluated.
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define cjson__is_signed(val) \
  cjson__is_signed_impl((val), ccol__uniq(cjson__is, __COUNTER__))
#define cjson__is_signed_impl(val, cjson__is_name)                          \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool cjson__is_name = _Generic((val),                                   \
        char: CJSON__CHAR_SIGNED,                                           \
        signed char: true,                                                  \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    cjson__is_name;                                                         \
  })
#else
#define cjson__is_signed(val)   \
  _Generic((val),               \
      char: CJSON__CHAR_SIGNED, \
      signed char: true,        \
      short: true,              \
      int: true,                \
      long: true,               \
      long long: true,          \
      default: false)
#endif

// Evaluates to the node at the dot-separated path in the tree root, so a caller
// can read a deep value in one call. A plain component is a dictionary key,
// "#N" is a zero-based index on a list, and "\." and "\\" escape a dot and a
// backslash. A NULL or empty path gives root. The node is borrowed: The tree
// keeps owning it, and it stays valid until the tree changes. Gives NULL for a
// NULL root, an absent component, a malformed path or one whose shape doesn't
// fit the tree, and when the copy of a path of 256 bytes or more fails.
//
//   cjson city = cjson_get(doc, "users.#0.address.city");
#define cjson_get(root, path) cjson__get((root), (path))

// Writes the C scalar val to the leaf at path in the tree root, so a caller can
// set a deep value in one call; evaluates to a ccol_retval. val can be a
// bool, any integer type, float, double, or a char pointer, array or literal
// (all give CJSON_STRING); NULL or nullptr gives CJSON_NULL, and any other
// void * is ccol_invalid_args. Before C23 true is the int 1, so write
// (bool)true for a CJSON_BOOL. A bit-field doesn't compile; cast it. An
// unsigned value above LLONG_MAX is stored as the nearest CJSON_FLOAT, as the
// parser reads the same literal. The leaf is created when absent, but its
// parent must exist; an existing leaf is updated in place, takes the new type,
// and an old subtree is deep-freed. path is dot-separated, with #N for a list
// index and "\." for a dot inside a key. Gives ccol_success; ccol_invalid_args
// for a NULL root, an empty or malformed path, a path whose shape doesn't fit
// the tree, an unsupported type, a string or key that isn't UTF-8, or a double
// that isn't finite; ccol_key_not_found when a parent component or a leaf list
// index is absent; ccol_not_enough_memory; ccol_container_full for a new key in
// a full dictionary.
//
//   cjson_set(doc, "users.#0.active", (bool)true);
//   cjson_set(doc, "users.#0.score", 99);
#define cjson_set(root, path, val) \
  cjson__set_impl((root), (path), (val), ccol__uniq(cjson__sv, __COUNTER__))

// Internal: The body of cjson_set(). It copies val with __auto_type (arrays
// decay and top-level qualifiers go) into a temporary named cjson__sv_name, a
// ccol__uniq() name so the macro nests under -Wshadow, and evaluates to the
// result of cjson__set_typed() on that copy.
#define cjson__set_impl(root, path, val, cjson__sv_name)              \
  ({                                                                  \
    __auto_type cjson__sv_name = (val);                               \
    cjson__set_typed((root), (path), cjson__type_of(cjson__sv_name),  \
                     (void *)&cjson__sv_name, sizeof(cjson__sv_name), \
                     cjson__is_signed(cjson__sv_name));               \
  })

// Removes the node at path from the tree root and deep-frees it, so a caller
// can drop a deep value in one call. path is dot-separated, with #N for a list
// index and "\." for a dot inside a key; under a list parent the leaf must be
// a #N component. Evaluates to ccol_success, ccol_invalid_args for a NULL root
// or path, an empty or malformed path or one whose shape doesn't fit the tree,
// ccol_key_not_found when a well-formed component is absent, or
// ccol_not_enough_memory.
#define cjson_delete(root, path) cjson__delete((root), (path))

#pragma GCC visibility pop
