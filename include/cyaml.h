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

// Everything from here to the matching pop is the public ABI and the shared
// library exports it. The library builds with -fvisibility=hidden, so anything
// outside these blocks stays internal.
#pragma GCC visibility push(default)

// A YAML 1.2 parser, serializer and mutable DOM. A list keeps its children in a
// cvector and a dictionary maps char * keys to children in a chashmap; the man
// pages (cyaml(7) and one page per call) hold the full rules. The main ones:
//
// The input must be well-formed UTF-8 made of YAML 1.2 c-printable characters
// only, and anything else is refused with an error that gives the line and the
// column; the parser never repairs input. A string value can't hold a NUL, so
// every escape that decodes to U+0000 is a parse error. Plain scalars get the
// core-schema types (null, bool, int with strict 0x/0o forms, float with .inf
// and .nan). A tag forces or checks a type as cyaml(7) describes, and a custom
// tag is kept as metadata. A collection used as a key is stored as its
// canonical flow text, with mapping entries sorted, and a scalar key is stored
// as its canonical text ("0x10" becomes "16"). Merge keys (<<) expand in place
// with PyYAML's precedence. A stream of several documents parses to a list of
// their roots.
//
// Dictionaries keep insertion order; a repeated key keeps its first place and
// its last value. A tree nests at most 500 containers, and a parse is bounded
// by a node budget, a byte budget and a fixed budget for what aliases, merges
// and collection keys copy. Numbers are parsed and written in the C locale for
// the length of each call. Every node carries the allocator it was made with,
// interned so the caller's procs struct may go away, and each thread pools up
// to 512 default-allocator nodes.
//
// The kind of a node.
typedef enum cyaml_node_type {
  CYAML_NULL = 0,
  CYAML_BOOL,
  CYAML_INTEGER,
  CYAML_FLOAT,
  CYAML_STRING,
  CYAML_LIST,
  CYAML_DICTIONARY,
} cyaml_node_type;

// The DOM node. The struct is opaque and cyaml is the handle.
typedef struct cyaml_node cyaml_node;

typedef cyaml_node *cyaml;

// Creates a null node. The create functions are how you build or extend a tree
// by hand. mp is your allocator, or NULL for the default one; each _mp form has
// a plain twin that passes NULL. Each gives the new node, which you own, or
// NULL when an allocation fails or a 65th distinct allocator would be needed.
cyaml cyaml_create_null_mp(ccol_memmgmt_procs *mp);

// cyaml_create_null_mp() with the default allocator.
static inline cyaml cyaml_create_null(void) {
  return cyaml_create_null_mp(NULL);
}

// Creates a bool node.
cyaml cyaml_create_bool_mp(bool val, ccol_memmgmt_procs *mp);

// cyaml_create_bool_mp() with the default allocator.
static inline cyaml cyaml_create_bool(bool val) {
  return cyaml_create_bool_mp(val, NULL);
}

// Creates an integer node.
cyaml cyaml_create_int_mp(long long val, ccol_memmgmt_procs *mp);

// cyaml_create_int_mp() with the default allocator.
static inline cyaml cyaml_create_int(long long val) {
  return cyaml_create_int_mp(val, NULL);
}

// Creates a float node. A non-finite value is stored, and serialized as .inf,
// -.inf or .nan.
cyaml cyaml_create_double_mp(double val, ccol_memmgmt_procs *mp);

// cyaml_create_double_mp() with the default allocator.
static inline cyaml cyaml_create_double(double val) {
  return cyaml_create_double_mp(val, NULL);
}

// Creates a string node holding a copy of val, which must be well-formed UTF-8
// (NULL gives a CYAML_NULL node). Gives NULL for a string that isn't
// well-formed UTF-8 or when memory runs out.
cyaml cyaml_create_string_mp(const char *val, ccol_memmgmt_procs *mp);

// cyaml_create_string_mp() with the default allocator.
static inline cyaml cyaml_create_string(const char *val) {
  return cyaml_create_string_mp(val, NULL);
}

// Creates an empty list node.
cyaml cyaml_create_list_mp(ccol_memmgmt_procs *mp);

// cyaml_create_list_mp() with the default allocator.
static inline cyaml cyaml_create_list(void) {
  return cyaml_create_list_mp(NULL);
}

// Creates an empty dictionary node.
cyaml cyaml_create_dictionary_mp(ccol_memmgmt_procs *mp);

// cyaml_create_dictionary_mp() with the default allocator.
static inline cyaml cyaml_create_dictionary(void) {
  return cyaml_create_dictionary_mp(NULL);
}

// Parses a NUL-terminated YAML text into a tree that you own; NULL on failure.
// On failure *err_str (when err_str isn't NULL) points at a printable-ASCII
// message that the library owns: don't free it. It's per thread and stays
// valid until the next failing parse on the same thread. On success *err_str is
// NULL. mp is the allocator of every node the parse builds.
cyaml cyaml_parse_mp(const char *yaml_str, char **err_str,
                     ccol_memmgmt_procs *mp);

// cyaml_parse_mp() with the default allocator.
static inline cyaml cyaml_parse(const char *yaml_str, char **err_str) {
  return cyaml_parse_mp(yaml_str, err_str, NULL);
}

// Parses a buffer that needn't end with a NUL, like cyaml_parse_mp() but
// reading exactly len bytes and nothing past them. A NUL byte inside them, or a
// UTF-8 sequence that len cuts short, is a parse error.
cyaml cyaml_parse_n_mp(const char *yaml_str, size_t len, char **err_str,
                       ccol_memmgmt_procs *mp);

// cyaml_parse_n_mp() with the default allocator.
static inline cyaml cyaml_parse_n(const char *yaml_str, size_t len,
                                  char **err_str) {
  return cyaml_parse_n_mp(yaml_str, len, err_str, NULL);
}

// Writes the tree as block-style YAML. A scalar is plain when YAML 1.1 readers
// would read the same text, and double-quoted otherwise. A float always holds a
// '.', with the shortest of 15, 16 or 17 digits that reads back the same. A
// NULL node gives "~\n". The buffer comes from the allocator of the root; free
// it with cyaml_serialize_free_mp(). NULL when memory runs out or the tree
// nests more than 500 containers; the two can't be told apart.
char *cyaml_serialize(cyaml node);

// Writes the tree as compact one-line flow YAML ([a, b], {k: v}), for logs and
// one-line output. The rest is as for cyaml_serialize(); a NULL node gives "~".
char *cyaml_serialize_flow(cyaml node);

// Writes each element of a list as its own "---" document, the counterpart of
// parsing a stream, so a multi-document file can be edited and written back. An
// empty list gives "", and a node that isn't a list is written as one document.
// The tag of the list isn't written. Each element may nest at most 499
// containers.
char *cyaml_serialize_stream(cyaml node);

// Frees a string from one of the serializers, which come from the tree's
// allocator. mp is the allocator of the tree (NULL for the default one), and s
// may be NULL.
void cyaml_serialize_free_mp(char *s, ccol_memmgmt_procs *mp);

// cyaml_serialize_free_mp() for a tree with the default allocator.
static inline void cyaml_serialize_free(char *s) {
  cyaml_serialize_free_mp(s, NULL);
}

// Gives the kind of node, so you can check it before an accessor that is fatal
// on the wrong kind; CYAML_NULL for a NULL node.
cyaml_node_type cyaml_type(cyaml node);

// Gives the name of the kind of node as a string literal, for messages and
// debugging.
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

// The seven core-schema tag URIs, to compare with cyaml_node_tag() or pass to
// cyaml_node_set_tag().
#define CYAML_TAG_NULL "tag:yaml.org,2002:null"
#define CYAML_TAG_BOOL "tag:yaml.org,2002:bool"
#define CYAML_TAG_INT "tag:yaml.org,2002:int"
#define CYAML_TAG_FLOAT "tag:yaml.org,2002:float"
#define CYAML_TAG_STR "tag:yaml.org,2002:str"
#define CYAML_TAG_SEQ "tag:yaml.org,2002:seq"
#define CYAML_TAG_MAP "tag:yaml.org,2002:map"

// Gives the resolved tag of node, so you can read the tags a document carried,
// or NULL when it has none (or node is NULL). The node owns the string. It
// stays valid until the node's tag or value is set again or the node is removed
// or destroyed.
const char *cyaml_node_tag(cyaml node);

// Sets, replaces or (with a NULL tag) clears the tag of node, so a tree you
// build can carry tags. It never changes the type or the value. A core-schema
// tag must name the node's own type, and "", "!" and a string that isn't
// well-formed UTF-8 are refused; each refusal, and a NULL node, gives
// ccol_invalid_args. ccol_not_enough_memory when the copy fails. On every
// failure the old tag stays.
ccol_retval cyaml_node_set_tag(cyaml node, const char *tag);

// Give the value of a scalar node. Each calls ccol__fatal_err() when node isn't
// of the matching type, so check cyaml_type() first when you aren't sure. The
// string belongs to the node and stays valid until the node gets a new value or
// is removed or destroyed.
bool cyaml_bool_val(cyaml node);

long long cyaml_int_val(cyaml node);

double cyaml_double_val(cyaml node);

const char *cyaml_str_val(cyaml node);

// Give the element count of a list and the key count of a dictionary, for
// walking them. Each calls ccol__fatal_err() on a node of another type.
size_t cyaml_list_len(cyaml node);

size_t cyaml_dictionary_size(cyaml node);

// Appends child to the list seq, which takes ownership of it. ccol_invalid_args
// (a NULL argument, seq not a list, child already attached to a parent, or
// child == seq) leaves child untouched and yours. Every other failure
// (ccol_not_enough_memory, ccol_container_full) deep-frees child. Cycles
// through an ancestor without a parent aren't checked; don't build one.
ccol_retval cyaml_list_push(cyaml seq, cyaml child);

// Gives a borrowed reference to the element at index, for reading a list, or
// NULL when index is out of range or seq isn't a list.
cyaml cyaml_list_get(cyaml seq, size_t index);

// Inserts or replaces key in map, which takes ownership of child. The ownership
// rules are those of cyaml_list_push(); a key that isn't well-formed UTF-8 is
// also ccol_invalid_args. A replaced child is deep-freed and the key keeps its
// place. Setting a key to the value it already holds does nothing and succeeds.
ccol_retval cyaml_dictionary_set(cyaml map, const char *key, cyaml child);

// Gives a borrowed reference to the value of key, for reading a dictionary, or
// NULL when it's absent or map isn't a dictionary.
cyaml cyaml_dictionary_get(cyaml map, const char *key);

// Removes and deep-frees the element at index; later elements move down.
// ccol_invalid_args for a NULL or non-list seq, or an index out of range.
ccol_retval cyaml_list_remove(cyaml seq, size_t index);

// Removes and deep-frees the entry of key. ccol_invalid_args for a NULL or
// non-dictionary map, ccol_key_not_found when the key is absent.
ccol_retval cyaml_dictionary_remove(cyaml map, const char *key);

// A cursor over the members of a dictionary in insertion order. It lives in
// your storage, and stepping allocates nothing. The cursor holds the member
// that the next step reads, so you may replace values, remove the current
// member or insert new keys while you iterate; removing that next member or
// destroying the dictionary invalidates it. Replacing the current value through
// cyaml_dictionary_set() frees the node that value names.
typedef struct cyaml_dictionary_iter {
  const char *key;
  cyaml value;
  const void *cyaml__next;
} cyaml_dictionary_iter;

// Starts a walk over a dictionary in insertion order: steps it onto the first
// member of dict. false (with key and value NULL) when the dictionary is empty,
// dict is NULL or not a dictionary, or it is NULL.
bool cyaml_dictionary_first(cyaml dict, cyaml_dictionary_iter *it);

// Steps a dictionary walk onto the next member; false once the iteration is
// over.
bool cyaml_dictionary_next(cyaml_dictionary_iter *it);

// Gives a deep copy that you own, for editing a tree while keeping the
// original. It has the same allocator, tags and member order. NULL for a NULL
// node, when memory runs out, or when the tree nests more than 500 containers.
cyaml cyaml_clone(cyaml node);

// Declares a cyaml variable named var_name, so code reads like the other
// modules' declare macros. It expands to the declaration.
#define cyaml_declare(var_name) cyaml var_name

// Declares a cyaml variable named var_name, set to NULL, that is destroyed when
// it goes out of scope, so a tree can't leak on an early return.
#define cyaml_declare_scoped(var_name) \
  cyaml var_name ccol__destructor(cyaml__destroy_cleanup) = NULL

// Internal: frees a whole tree, the body of cyaml_destroy(). It uses a loop, so
// any depth is fine. Use cyaml_destroy() instead.
void cyaml__destroy(cyaml node);

// Destroys *node and sets it to NULL; the cleanup function of
// cyaml_declare_scoped(), also usable when you have no lvalue for
// cyaml_destroy().
static inline void cyaml__destroy_cleanup(cyaml *node) {
  if (node && *node) {
    cyaml__destroy(*node);
    *node = NULL;
  }
}

// Frees the tree and sets node to NULL, so the handle can't dangle. node must
// be a modifiable lvalue and is evaluated once; NULL is fine. It's a statement
// and gives no value.
#define cyaml_destroy(node)      \
  ccol__cyaml_destroy_impl(node, \
                           ccol__uniq(ccol__cyaml_destroy_slot, __COUNTER__))

// Internal: the body of cyaml_destroy(). slot comes from ccol__uniq(), so
// destroy macros can nest without -Wshadow warnings, and node is evaluated once
// through it.
#define ccol__cyaml_destroy_impl(node, slot) \
  do {                                       \
    __typeof__(node) *slot = &(node);        \
    if (*slot) {                             \
      cyaml__destroy(*slot);                 \
      *slot = NULL;                          \
    }                                        \
  } while (0)

// Internal: the body of cyaml_get(). A NULL or empty path gives root. NULL for
// a missing component, a wrong type on the way, an empty component, or when the
// copy of a path of 256 bytes or more fails.
cyaml cyaml__get(cyaml root, const char *path);

// Internal: the body of cyaml_delete(). ccol_invalid_args for a NULL or empty
// path, a malformed component anywhere, or a parent of the wrong type;
// ccol_key_not_found for a well-formed component that is absent;
// ccol_not_enough_memory when the path copy fails.
ccol_retval cyaml__delete(cyaml root, const char *path);

// Internal: the body of cyaml_set(). raw points at the value, raw_size is its
// sizeof() (a pointer to the string for CYAML_STRING) and is checked before the
// value is read. The errors are those of cyaml__delete(), plus a string value
// that isn't well-formed UTF-8, and ccol_container_full for a new key past the
// largest count. An absent key at the leaf is created; an absent list index or
// an absent key above the leaf is ccol_key_not_found.
ccol_retval cyaml__set_typed(cyaml root, const char *path, cyaml_node_type type,
                             void *raw, size_t raw_size, bool is_signed);

// What cyaml__type_of() gives for a type that cyaml_set() doesn't accept, so
// such a call is refused at run time instead of storing garbage. It's no real
// node type, so the type check of cyaml__set_typed() refuses it.
#define CYAML__TYPE_UNSUPPORTED ((cyaml_node_type)0x7f)

// Maps the type of the C expression val to the node type cyaml_set() stores, so
// cyaml_set() works for any supported C type. It evaluates to a
// cyaml_node_type, or CYAML__TYPE_UNSUPPORTED. A bare NULL (void *) and nullptr
// map to CYAML_NULL on purpose. The clang form wraps the _Generic to silence
// -Wunreachable-code-generic-assoc.
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define cyaml__type_of(val) \
  cyaml__type_of_impl((val), ccol__uniq(cyaml__type_of_tmp, __COUNTER__))
#define cyaml__type_of_impl(val, cyaml__type_of_name)                       \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    cyaml_node_type cyaml__type_of_name = _Generic((val),                   \
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
        CCOL__NULLPTR_ASSOC(CYAML_NULL) void *: CYAML_NULL,                 \
        default: CYAML__TYPE_UNSUPPORTED);                                  \
    _Pragma("GCC diagnostic pop");                                          \
    cyaml__type_of_name;                                                    \
  })
#else
#define cyaml__type_of(val)                               \
  _Generic((val),                                         \
      bool: CYAML_BOOL,                                   \
      char: CYAML_INTEGER,                                \
      signed char: CYAML_INTEGER,                         \
      short: CYAML_INTEGER,                               \
      int: CYAML_INTEGER,                                 \
      long: CYAML_INTEGER,                                \
      long long: CYAML_INTEGER,                           \
      unsigned char: CYAML_INTEGER,                       \
      unsigned short: CYAML_INTEGER,                      \
      unsigned int: CYAML_INTEGER,                        \
      unsigned long: CYAML_INTEGER,                       \
      unsigned long long: CYAML_INTEGER,                  \
      float: CYAML_FLOAT,                                 \
      double: CYAML_FLOAT,                                \
      char *: CYAML_STRING,                               \
      const char *: CYAML_STRING,                         \
      CCOL__NULLPTR_ASSOC(CYAML_NULL) void *: CYAML_NULL, \
      default: CYAML__TYPE_UNSUPPORTED)
#endif

// True when plain char is signed on this target, for cyaml__is_signed().
#if defined(__CHAR_UNSIGNED__)
#define CYAML__CHAR_SIGNED false
#else
#define CYAML__CHAR_SIGNED true
#endif

// Evaluates to true when val has a signed integer type, so cyaml__set_typed()
// knows how to widen the raw integer. The clang form wraps the _Generic like
// cyaml__type_of().
#if defined(CCOL__CLANG_GENERIC_ASSOC_WARNING)
#define cyaml__is_signed(val) \
  cyaml__is_signed_impl((val), ccol__uniq(cyaml__is, __COUNTER__))
#define cyaml__is_signed_impl(val, cyaml__is_name)                          \
  ({                                                                        \
    _Pragma("GCC diagnostic push");                                         \
    _Pragma("GCC diagnostic ignored \"-Wunreachable-code-generic-assoc\""); \
    bool cyaml__is_name = _Generic((val),                                   \
        char: CYAML__CHAR_SIGNED,                                           \
        signed char: true,                                                  \
        short: true,                                                        \
        int: true,                                                          \
        long: true,                                                         \
        long long: true,                                                    \
        default: false);                                                    \
    _Pragma("GCC diagnostic pop");                                          \
    cyaml__is_name;                                                         \
  })
#else
#define cyaml__is_signed(val)   \
  _Generic((val),               \
      char: CYAML__CHAR_SIGNED, \
      signed char: true,        \
      short: true,              \
      int: true,                \
      long: true,               \
      long long: true,          \
      default: false)
#endif

// Gives a borrowed reference to the node at a dot-separated path ("a.#0.b"), so
// you can reach deep values in one call, or NULL. '#N' indexes a list; '\.'
// and '\\' escape a dot and a backslash inside a key. It evaluates to
// cyaml__get(root, path).
#define cyaml_get(root, path) cyaml__get((root), (path))

// Writes a C value to the leaf at path, so you can set a value of any supported
// C type in one call. val may be bool, any integer type, float, double, a
// string or NULL. Before C23 `true` is an int, so write (bool)true for a
// CYAML_BOOL. An unsigned value above LLONG_MAX becomes the nearest float, and
// a bit-field must be cast first. A missing key at the leaf is created; a list
// index must exist. A custom tag on the leaf survives, a core tag only while it
// matches the new type. It evaluates to the ccol_retval of cyaml__set_typed().
#define cyaml_set(root, path, val) \
  cyaml__set_impl((root), (path), (val), ccol__uniq(cyaml__sv, __COUNTER__))

// Internal: the body of cyaml_set(); cyaml__sv_name comes from ccol__uniq() so
// nested macros don't shadow each other. __auto_type decays an array and drops
// qualifiers, so a char[N] or a const pointer arrives as a plain char pointer.
// It evaluates to the ccol_retval.
#define cyaml__set_impl(root, path, val, cyaml__sv_name)              \
  ({                                                                  \
    __auto_type cyaml__sv_name = (val);                               \
    cyaml__set_typed((root), (path), cyaml__type_of(cyaml__sv_name),  \
                     (void *)&cyaml__sv_name, sizeof(cyaml__sv_name), \
                     cyaml__is_signed(cyaml__sv_name));               \
  })

// Removes and deep-frees the node at path, so you can drop a deep value in one
// call. It evaluates to the ccol_retval of cyaml__delete().
#define cyaml_delete(root, path) cyaml__delete((root), (path))

#pragma GCC visibility pop
