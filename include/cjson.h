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
// Allocator: every factory and parse function takes a ccol_memmgmt_procs *mp
// (NULL for the default allocator). The library keeps its own process-lifetime
// copy of the struct, one per distinct set of four functions, at most 64 of
// them; a call that would need a 65th fails with NULL. So your struct may go
// away, but its functions must stay callable until the last node built with
// them is destroyed. All nodes of one tree carry the same allocator, which
// serves the nodes, owned strings, list and dictionary stores, path copies and
// the serializer buffers. With the default allocator a thread-local pool of up
// to 512 nodes speeds up allocation; it's drained when the thread exits.
//
// Text: every string in a tree (value or key) is well-formed UTF-8. The parser
// refuses ill-formed bytes and lone surrogate escapes with a printable-ASCII
// message that names the bytes as 0xNN and the byte offset, and
// cjson_create_string(), cjson_set() and cjson_dictionary_set() refuse a C
// string that isn't UTF-8. Nothing is repaired.
//
// Nesting: parse, clone and serialize accept at most 500 nested containers and
// report a deeper document as an error. They walk with a heap worklist, so the
// limit doesn't depend on the thread's stack. cjson_destroy() has no limit.
//
// Memory: a parsed tree is linear in its input, at most about 65 bytes of tree
// per input byte on a 64-bit target, so bounding the input bounds a parse.
//
// Paths (cjson_get, cjson_set, cjson_delete): dot-separated components such as
// "users.#0.address.city". A plain component is a dictionary key; "#N" is a
// zero-based index, but only on a list (elsewhere it's a literal key). "\." and
// "\\" escape a dot and a backslash. An empty path gives the root for
// cjson_get and is an error for the others. A malformed component (empty, not
// UTF-8, a bad #N on a list) or a path whose shape can't apply to the tree
// (descending into a scalar, a name on a list) is ccol_invalid_args at any
// position; ccol_key_not_found means only that a well-formed name or index is
// absent. cjson_get() gives NULL in all these cases.
//
// Ownership: create, parse and clone return trees the caller owns. A
// successful cjson_list_push() or cjson_dictionary_set() moves the child to the
// parent. On failure ccol_invalid_args always means the child is untouched and
// still yours, and every other error code means the call already destroyed it.
// Everything that cjson_get() and the other getters return is borrowed and
// valid until the tree changes. cjson_destroy() frees a whole subtree.

#include "common.h"

// Everything from here to the end of the header is the public ABI and the
// shared library exports it. The library is built with -fvisibility=hidden, so
// anything outside such a block stays internal.
#pragma GCC visibility push(default)

// The tag of a node: one of the seven JSON value kinds.
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

// The factories: each creates one node of its type, for a caller that builds a
// tree by hand, and gives it to the caller, or NULL when the allocation fails.
// mp is the allocator (NULL for the default); the forms without _mp use the
// default allocator.
cjson cjson_create_null_mp(ccol_memmgmt_procs *mp);

static inline cjson cjson_create_null(void) {
  return cjson_create_null_mp(NULL);
}

cjson cjson_create_bool_mp(bool val, ccol_memmgmt_procs *mp);

static inline cjson cjson_create_bool(bool val) {
  return cjson_create_bool_mp(val, NULL);
}

cjson cjson_create_int_mp(long long val, ccol_memmgmt_procs *mp);

static inline cjson cjson_create_int(long long val) {
  return cjson_create_int_mp(val, NULL);
}

// Refuses a non-finite val with NULL, since JSON can't write it.
cjson cjson_create_double_mp(double val, ccol_memmgmt_procs *mp);

static inline cjson cjson_create_double(double val) {
  return cjson_create_double_mp(val, NULL);
}

// Copies val into the node. val must be valid UTF-8, or the call returns NULL;
// a NULL val gives a CJSON_NULL node.
cjson cjson_create_string_mp(const char *val, ccol_memmgmt_procs *mp);

static inline cjson cjson_create_string(const char *val) {
  return cjson_create_string_mp(val, NULL);
}

// Create an empty list and an empty dictionary, for the caller to fill.
cjson cjson_create_list_mp(ccol_memmgmt_procs *mp);

static inline cjson cjson_create_list(void) {
  return cjson_create_list_mp(NULL);
}

cjson cjson_create_dictionary_mp(ccol_memmgmt_procs *mp);

static inline cjson cjson_create_dictionary(void) {
  return cjson_create_dictionary_mp(NULL);
}

// Parses the NUL-terminated JSON text json_str into a tree, for a caller that
// reads JSON. mp is the allocator of every node (NULL for the default). Gives
// the root, which the caller owns, or NULL. On success *err_str becomes NULL.
// On failure it points at a printable ASCII message (input bytes outside
// printable ASCII appear as 0xNN) that the library owns in per-thread storage:
// never free it, and copy it if you need it past the next failing parse on the
// same thread. When the thread can't keep a buffer the message is a fixed
// text. err_str may be NULL. Numbers always use '.' whatever the LC_NUMERIC
// locale.
cjson cjson_parse_mp(const char *json_str, char **err_str,
                     ccol_memmgmt_procs *mp);

static inline cjson cjson_parse(const char *json_str, char **err_str) {
  return cjson_parse_mp(json_str, err_str, NULL);
}

// Like cjson_parse_mp(), for JSON that sits in a buffer of len bytes and isn't
// NUL-terminated.
cjson cjson_parse_n_mp(const char *json_str, size_t len, char **err_str,
                       ccol_memmgmt_procs *mp);

static inline cjson cjson_parse_n(const char *json_str, size_t len,
                                  char **err_str) {
  return cjson_parse_n_mp(json_str, len, err_str, NULL);
}

// Writes the subtree at node as compact JSON text, for a caller that stores or
// sends a tree. The text is valid UTF-8 with '.' as the decimal point in every
// locale. The buffer comes from the allocator of node: free it with
// cjson_serialize_free() for the default allocator and with
// cjson_serialize_free_mp() and the same mp otherwise. Gives NULL when an
// allocation fails or the subtree is nested deeper than 500 levels.
char *cjson_serialize(cjson node);

// Like cjson_serialize(), for output a person reads: indented by indent spaces
// per level (0 means 4).
char *cjson_serialize_pretty(cjson node, unsigned int indent);

// Frees a buffer that a serializer gave, with mp, the allocator the tree was
// built with (NULL for the default). s may be NULL.
void cjson_serialize_free_mp(char *s, ccol_memmgmt_procs *mp);

static inline void cjson_serialize_free(char *s) {
  cjson_serialize_free_mp(s, NULL);
}

// Gives the type tag of node (CJSON_NULL for a NULL node), so a caller can
// check a node before it reads it.
cjson_node_type cjson_type(cjson node);

// Gives the name of the type of node, for an error message.
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

// The leaf getters: each gives the value of a leaf of its type. Each calls
// ccol__fatal_err() for a NULL node or a node of another type, so check with
// cjson_type() when the type isn't known.
bool cjson_bool_val(cjson node);

long long cjson_int_val(cjson node);

double cjson_double_val(cjson node);

// The string stays owned by the node and is valid until the node gets a new
// value or is removed, replaced or destroyed. It's always valid UTF-8.
const char *cjson_str_val(cjson node);

// Give the element count of a list and the key count of a dictionary; fatal for
// a NULL node or the wrong type.
size_t cjson_list_len(cjson node);

size_t cjson_dictionary_size(cjson node);

// Appends child to the list arr, which takes ownership on success; it's how a
// caller fills a list. child must be a new node or a new clone; a borrowed
// reference, a node already attached somewhere, arr itself or an ancestor of
// arr is refused. Those refusals, a NULL argument and a wrong type return
// ccol_invalid_args and leave child untouched. ccol_not_enough_memory (also
// when the cycle check runs out of memory) and ccol_container_full mean
// ownership moved and child is destroyed.
ccol_retval cjson_list_push(cjson arr, cjson child);

// Gives element index of arr as a borrowed reference, for a caller that reads a
// list; NULL for a bad index or type.
cjson cjson_list_get(cjson arr, size_t index);

// Inserts key with child in obj, or replaces its value; it's how a caller fills
// a dictionary. The ownership rules are those of cjson_list_push(). A replace
// deep-frees the old value and keeps the key's place in the insertion order.
// Passing back the node that key already holds is a no-op. A NULL key or one
// that isn't UTF-8 is ccol_invalid_args too. The dictionary stores its own copy
// of key.
ccol_retval cjson_dictionary_set(cjson obj, const char *key, cjson child);

// Gives the value of key in obj as a borrowed reference, for a caller that
// reads a dictionary; NULL when the key is absent or the type is wrong.
cjson cjson_dictionary_get(cjson obj, const char *key);

// Removes element index from arr and deep-frees it; the later elements move
// left. ccol_invalid_args for a NULL or non-list arr or a bad index.
ccol_retval cjson_list_remove(cjson arr, size_t index);

// Removes the entry of key from obj and deep-frees its value.
// ccol_invalid_args for bad arguments, ccol_key_not_found when the key is
// absent.
ccol_retval cjson_dictionary_remove(cjson obj, const char *key);

// A cursor over a dictionary in insertion order, which the caller owns; it
// allocates nothing and can't fail. A key keeps the place where it first
// entered (a repeated key in parsed text keeps its first place and its last
// value). The cursor holds the successor of the current member and stays valid
// across any change that leaves that successor in place: replacing values,
// removing other members, inserting (whether this iteration sees a new key is
// unspecified). Removing the successor or destroying the dictionary invalidates
// it. After cjson_dictionary_set() replaces the current value, value names
// freed memory; cjson_set() updates the node in place.
typedef struct cjson_dictionary_iter {
  // The current member; both NULL once the iteration is over.
  const char *key;
  cjson value;
  // Private: the member that the next step reads.
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

// Steps the cursor it onto the next member. Gives false (key and value NULL)
// after the last one or for a NULL it.
bool cjson_dictionary_next(cjson_dictionary_iter *it);

// Makes a deep copy of the subtree at node with node's allocator, for a caller
// that wants an independent tree (for example to attach a copy of a borrowed
// node). Gives the copy, which the caller owns, or NULL when an allocation
// fails or the subtree is nested deeper than 500 levels.
cjson cjson_clone(cjson node);

// Declares an uninitialized cjson named var_name. It exists so cjson follows
// the naming convention of the other containers.
#define cjson_declare(var_name) cjson var_name

// Declares a cjson named var_name, set to NULL, that is destroyed at the end of
// its scope, so a function with several exits can't leak its tree.
#define cjson_declare_scoped(var_name) \
  cjson var_name ccol__destructor(cjson__destroy_cleanup) = NULL

// Internal: frees the tree at node. It's the body of cjson_destroy(); use the
// macro.
void cjson__destroy(cjson node);

// Internal: destroys *node and sets it to NULL. It's the cleanup function that
// cjson_declare_scoped() attaches.
static inline void cjson__destroy_cleanup(cjson *node) {
  if (node && *node) {
    cjson__destroy(*node);
    *node = NULL;
  }
}

// Frees the tree at node (each node with its own allocator) and sets the handle
// to NULL, so a later use is a plain NULL instead of a dangling pointer. NULL
// is safe. node is evaluated once and must be a modifiable lvalue.
#define cjson_destroy(node)      \
  ccol__cjson_destroy_impl(node, \
                           ccol__uniq(ccol__cjson_destroy_slot, __COUNTER__))

// Internal: the body of cjson_destroy(). slot is a ccol__uniq() name for the
// pointer to node, so nested destroy macros stay -Wshadow clean.
#define ccol__cjson_destroy_impl(node, slot) \
  do {                                       \
    __typeof__(node) *slot = &(node);        \
    if (*slot) {                             \
      cjson__destroy(*slot);                 \
      *slot = NULL;                          \
    }                                        \
  } while (0)

// Internal: gives the node at path, root for an empty path, or NULL; it's the
// back-end of cjson_get(). A path of 256 bytes or more is copied with the
// allocator of root, and NULL also means that copy failed.
cjson cjson__get(cjson root, const char *path);

// Internal: removes and deep-frees the node at path; it's the back-end of
// cjson_delete(). It walks to the parent and removes the leaf through
// cjson_dictionary_remove() or cjson_list_remove(). Errors follow the path
// rules at the top of this header; ccol_not_enough_memory when an allocation
// fails.
ccol_retval cjson__delete(cjson root, const char *path);

// Internal: writes a scalar to the leaf at path; it's the back-end of
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

// Evaluates to the node at the dot-separated path in the tree root, borrowed,
// or NULL when the path doesn't exist; it's how a caller reads a deep value in
// one call.
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
// parent must exist; an existing leaf takes the new type and an old subtree is
// deep-freed.
//
//   cjson_set(doc, "users.#0.active", (bool)true);
//   cjson_set(doc, "users.#0.score", 99);
#define cjson_set(root, path, val) \
  cjson__set_impl((root), (path), (val), ccol__uniq(cjson__sv, __COUNTER__))

// Internal: the body of cjson_set(). It copies val with __auto_type (arrays
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
// can drop a deep value in one call; evaluates to a ccol_retval. Under a list
// parent the leaf must be a #N component.
#define cjson_delete(root, path) cjson__delete((root), (path))

#pragma GCC visibility pop
