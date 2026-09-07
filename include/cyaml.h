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
 *   folded (>), with all chomping modes (clip, strip, keep).  Plain,
 *   single-quoted, and double-quoted scalars may all span multiple physical
 *   lines, folded per the same rules as a folded block scalar (a single line
 *   break folds to a space; consecutive blank lines fold to that many
 *   newlines instead).  An implicit mapping key must still fit on one
 *   physical line; only an implicit *value* or an explicit key (? key) may
 *   span multiple lines.
 * - Anchors (&name) and aliases (*name).
 * - Implicit core-schema typing: null, bool, integer (decimal/octal/hex),
 *   float (including .inf and .nan).
 * - Non-scalar dictionary keys: a sequence, mapping, or flow collection used
 *   directly as a key is canonicalized to compact flow-style text, since
 *   this DOM's dictionaries always map char* -> cyaml.  A mapping's own
 *   entries are canonicalized in sorted (lexicographic-by-key) order at
 *   every nesting level, independent of the order they were originally
 *   inserted in, so two structurally-equal non-scalar keys always
 *   canonicalize identically and correctly collide as the same key
 *   regardless of construction order; a sequence's own element order is
 *   always significant and never reordered.
 * - Multi-document streams delimited by "---".  The leading "---" is optional
 *   for single-document inputs.  Multiple documents are returned as a
 *   CYAML_LIST (one element per document root); a single document is returned
 *   as its root node directly.  A later document may omit its own "---" only
 *   when the document immediately before it ended with an explicit "...";
 *   a document carrying its own %YAML directive always requires an explicit
 *   "..." before it, even when it supplies its own "---".
 * - The %YAML directive is validated against the MAJOR.MINOR grammar (at
 *   most one per document) but its version number has no effect on parsing;
 *   every input is parsed as YAML 1.2 regardless of what it declares.
 * - A tab character is rejected wherever it would be interpreted as
 *   block-structural indentation or a structural separator (e.g. leading
 *   indentation for a mapping/sequence/block-scalar line, immediately after
 *   a "-"/"?"/":" indicator, or on a flow collection's own continuation
 *   line); a tab is accepted as ordinary whitespace anywhere else, including
 *   same-line separators between a node property and its value, inside a
 *   comment or a quoted scalar, and as literal content once a block
 *   scalar's own indentation is already established.
 * - **Tags** (!!str, !!binary, a custom !foo, a %TAG-resolved shorthand or
 *   verbatim !<...> tag): resolved and queryable via cyaml_node_tag(); see
 *   the "Tags" section below for full semantics.
 * - **%TAG directive** shorthand-prefix scoping: tracked per document (a
 *   %TAG directive in one document never affects another); a shorthand tag
 *   using an undefined handle is a parse error.
 * - **Merge keys** (<<:): a mapping's own "<<:" entry, when its key is a
 *   plain, untagged scalar reading exactly "<<", expands the referenced
 *   mapping(s) into that mapping in place; see the "Merge keys" section
 *   below for full semantics.
 *
 * ### Unsupported / explicitly out-of-scope
 *
 * - Null bytes inside strings: a node's scalar value is a plain
 *   null-terminated C string with no separate length field, so an embedded
 *   null byte can never be represented; string values must not contain one,
 *   and any double-quoted escape that would decode to codepoint zero
 *   ("\0", "\x00", "\u0000", "\U00000000") is rejected as a parse error
 *   rather than silently truncating the value.  Base64-encode any data that
 *   may contain a null byte.  Consistent with this, !!binary is never given
 *   special decoding behavior: its base64 text is stored as an ordinary
 *   string, tagged "tag:yaml.org,2002:binary", never decoded into raw
 *   bytes.
 * - A raw (unescaped) C0 control byte (0x00-0x1F other than tab) or DEL
 *   (0x7F) is rejected as a parse error wherever it appears literally in
 *   scalar content (plain, single-quoted, double-quoted, or block),
 *   matching YAML 1.2's own scalar-content grammar.  A double-quoted
 *   scalar's explicit escape sequences (e.g. "\x01", "\x7f") remain the
 *   only way to embed a non-null one of these bytes in a string value; an
 *   escape that decodes to the null byte specifically is rejected instead,
 *   per the null-bytes note above.
 * - A tag decorating a dictionary key: this DOM's dictionary keys are
 *   plain C strings, not nodes (see cyaml_dictionary_get()), so a tag
 *   written directly on a key is parsed for validity but has nowhere to
 *   be stored and is discarded.
 * - A custom tag's original %TAG shorthand spelling does not survive a
 *   parse/serialize round-trip: cyaml_serialize()/cyaml_serialize_flow()
 *   always emit a custom tag in its fully-resolved, verbatim (!<...>)
 *   form.
 * - Character encoding validation: input is treated as an opaque byte
 *   string.  Only a raw C0 control byte (other than tab) or DEL is ever
 *   rejected outright (see above); no byte is checked for well-formed
 *   UTF-8, so a byte sequence that is not valid UTF-8 is accepted as
 *   literal scalar content and reproduced unchanged by cyaml_serialize()/
 *   cyaml_serialize_flow().  Feed cyaml_parse*() well-formed UTF-8 text if
 *   a downstream consumer of the resulting strings requires it.
 *
 * ### Tags
 *
 * A node's tag is queried with cyaml_node_tag() and set with
 * cyaml_node_set_tag(); see their own doc comments. Named constants for
 * the seven YAML 1.2 core-schema tag URIs (CYAML_TAG_NULL, _BOOL, _INT,
 * _FLOAT, _STR, _SEQ, _MAP) avoid hand-typing the URI strings.
 *
 * One of the five scalar core-schema tags (!!null, !!bool, !!int, !!float,
 * !!str) forces that type on a plain, single-quoted, double-quoted,
 * literal-block, or folded-block scalar, overriding whatever type the
 * scalar's own text would otherwise implicitly resolve to. The four
 * quoted/block styles have no implicit typing of their own at all (see
 * "Implicit core-schema typing" above); for those, the tag instead forces
 * a type onto text that would otherwise always become a plain string.
 *
 * !!bool, !!int, and !!float each validate their own scalar's text and
 * report a hard parse failure for the whole document (not a partial or
 * fallback result) on a mismatch, e.g. !!int applied to text that is not
 * a valid integer. !!null is the one exception: since there is no
 * canonical "wrong" spelling for a value whose whole point is to carry no
 * further information, !!null forces CYAML_NULL unconditionally, on any
 * text whatsoever, exactly like a custom tag decorating a scalar never
 * validates what it is attached to (see below). Applying any of the five
 * scalar core-schema tags to actual block/flow collection syntax, or
 * !!seq/!!map to the wrong collection kind or to a scalar, is always a
 * hard parse failure regardless of which scalar tag is involved.
 *
 * !!bool accepts a wider, case-insensitive vocabulary than implicit bool
 * typing does: "true"/"false", "yes"/"no", and "on"/"off" in any casing
 * (but not single-letter "y"/"n"). !!int and !!float additionally strip
 * trailing whitespace before parsing, so a literal or folded block
 * scalar's own chomped trailing newline(s) never spuriously trigger a
 * mismatch.
 *
 * A custom tag (anything other than the seven core-schema URIs, including
 * !!binary) never forces a scalar's type: it is attached to whatever type
 * the scalar's own text would otherwise resolve to (implicitly, for a
 * plain scalar; always a string, for the four quoted/block styles), or to
 * a collection as pure metadata with no effect on how it was parsed. A
 * bare "!" (the non-specific tag, distinct from any shorthand tag) never
 * forces a type either, behaving identically to no tag at all.
 *
 * ### Merge keys
 *
 * A mapping entry whose key is the literal, unquoted, untagged plain
 * scalar "<<" expands its value (a single mapping, or a sequence of
 * mappings) into that same mapping in place, then removes the "<<" key
 * itself. Both the implicit ("<<: ...") and explicit ("? <<" / ": ...")
 * key forms trigger this; either is treated identically, since both spell
 * an unquoted, untagged plain scalar reading exactly "<<". Explicit keys
 * already present in the mapping always win over anything merged in; for a
 * sequence of sources ("<<: [*a, *b]"), earlier sources win over later
 * ones on conflict. A merge source that is neither a mapping nor a
 * sequence of mappings is a hard parse failure. A merged-in mapping's own
 * "<<:" entry, if it had one, has already been expanded by the time it is
 * merged, so merging is transitive with no special handling required.
 *
 * A quoted ("<<") or explicitly tagged (!!str <<) key spelled "<<" is
 * always an ordinary literal key, never expanded; this matches how every
 * other core-schema type is only ever implicitly resolved for a plain,
 * untagged scalar.
 *
 * ### Implementation-defined behavior
 *
 * - Duplicate mapping keys: when a mapping contains the same key more than
 *   once, the last value wins and earlier values are silently replaced.
 *   This behavior is consistent with common YAML parsers but is not
 *   mandated by the YAML 1.2 specification.  The application is responsible
 *   for ensuring input does not contain unintentional duplicate keys.
 * - Dictionary key order: cyaml_serialize()/cyaml_serialize_flow() emit a
 *   CYAML_DICTIONARY node's entries in this DOM's own internal storage
 *   order, which is unrelated to insertion order, parse order, or any
 *   other caller-visible ordering; two dictionaries holding the same keys
 *   and values can serialize with their keys in a different relative
 *   order depending on how each was built, and a parse-then-serialize
 *   round trip is not guaranteed to reproduce the source document's own
 *   key order.  A CYAML_LIST's element order, by contrast, is always
 *   exactly the order its elements were pushed or parsed in.  A caller
 *   needing a specific, stable key order in serialized output must
 *   arrange for it itself (e.g. by re-inserting keys into a fresh
 *   dictionary in the desired order immediately before serializing).
 * - Nesting limits: a document nesting mappings, sequences, or explicit
 *   keys more than 500 levels deep is rejected with a parse error, rather
 *   than risking unbounded stack growth.  Separately, a non-scalar
 *   (sequence or mapping) dictionary key whose canonical flow-YAML text
 *   exceeds 64 KiB is also rejected; this specifically bounds a key that
 *   is itself a mapping containing another such key many levels deep,
 *   since canonicalizing it re-quotes each enclosing level's own already-
 *   quoted text, growing rapidly with nesting depth.  Neither limit is
 *   reachable by any realistic hand-written or generated document.
 *   cyaml_serialize()/cyaml_serialize_flow()/cyaml_clone() each carry the
 *   identical 500-level nesting limit independently: a tree passed to any
 *   of the three need not have come from cyaml_parse() at all (it may be
 *   built directly via cyaml_create_list()/cyaml_dictionary_set()), so
 *   serializing or cloning one nested past this depth returns NULL
 *   (indistinguishable from an allocation failure, since none of the three
 *   has a separate error-string channel) rather than recursing without
 *   bound.
 * - Node count limit: a single cyaml_parse()/cyaml_parse_n() call rejects
 *   a document that would require allocating more than 4,000,000 DOM
 *   nodes in total, with a parse error naming the limit explicitly (not
 *   an out-of-memory message), regardless of how much memory is actually
 *   available.  Reachable only by a document with millions of distinct
 *   scalar values, an extent no realistic hand-written or generated
 *   document approaches; this limit does not apply to a tree built
 *   directly via cyaml_create_list()/cyaml_dictionary_set() outside of
 *   parsing.
 * - Integer range: CYAML_INTEGER is a signed 64-bit long long.  An
 *   implicit decimal, hex, or octal integer literal whose value does not
 *   fit gracefully falls back the same way a too-wide decimal literal
 *   always has: to CYAML_FLOAT when the text also parses as one, or
 *   otherwise to a plain CYAML_STRING; it is never silently reinterpreted
 *   as a wrapped-around or wrong-signed CYAML_INTEGER.
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
 * Two escape sequences are recognised inside a path string, so a key
 * containing a literal '.' or '\' can still be addressed: '\.' resolves to
 * a literal '.' in the key (not a path separator), and '\\' resolves to a
 * literal '\'.  A '\' before any other character is passed through
 * unchanged.  For example, cyaml_get(doc, "a\\.b") addresses a key literally
 * named "a.b", and cyaml_get(doc, "a\\.b.c\\.d") addresses key "c.d" nested
 * one level under key "a.b".
 *
 * ### Ownership
 *
 * - cyaml_create_*(), cyaml_parse*(), and cyaml_clone() return fully owned
 *   trees.
 * - cyaml_list_push() and cyaml_dictionary_set() transfer ownership of the
 *   child to the parent; do not free it afterwards.
 * - cyaml_get() returns a NON-OWNING reference valid until the tree is
 *   mutated or destroyed.
 * - cyaml_destroy() frees the entire subtree and NULLs the handle. It
 *   tears down the tree iteratively rather than recursively, so freeing an
 *   arbitrarily deep tree never grows the native call stack.
 * - Every tree produced by cyaml_parse*() is acyclic by construction. A
 *   tree built or mutated directly through cyaml_list_push()/
 *   cyaml_dictionary_set() must stay acyclic too: neither function checks
 *   for a cycle (e.g. inserting a node into its own subtree), and passing
 *   a cyclic tree to cyaml_destroy(), cyaml_clone(), or cyaml_serialize*()
 *   is undefined behavior.
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
 *                  heap-allocated string is normally stored, allocated
 *                  through mp; the caller must free it with
 *                  cyaml_serialize_free_mp() (passing the same mp), which is
 *                  a safe no-op if *err_str is NULL.  *err_str may itself be
 *                  NULL even on failure if allocating the message text
 *                  fails too (only reachable under an already-exhausted
 *                  custom allocator); the return value, not *err_str, is
 *                  always what indicates success or failure.  Pass NULL to
 *                  ignore error details.
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
 *                  cyaml_parse_mp, including the free contract).  Pass NULL
 *                  to ignore.
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
 * Dictionaries use "key: value" block syntax; a dictionary's own key order
 * in the output is unspecified (see "Implementation-defined behavior" >
 * "Dictionary key order" above).
 *
 * The returned buffer is allocated with the root node's allocator.
 * Free it with cyaml_serialize_free() (or cyaml_serialize_free_mp() with
 * the same mp used to create the tree).
 *
 * @param node  Root of the (sub-)tree to serialize.  A NULL node serializes
 *              as if it were a CYAML_NULL node ("~\n"); this is a
 *              successful, non-NULL return, not a failure signal.
 * @return Heap-allocated null-terminated YAML string, or NULL on OOM or if
 *         node is nested more than 500 levels deep (see "Implementation-
 *         defined behavior" > "Nesting limits" above; the two failure
 *         causes are indistinguishable, since this function has no
 *         separate error-string channel).
 */
char *cyaml_serialize(cyaml node);

/**
 * @brief Serialize a DOM tree to compact flow-style YAML.
 *
 * Output resembles JSON: lists become [a, b, c], dictionaries become
 * {key: value}.  Useful for compact single-line representations.  A
 * dictionary's own key order in the output is unspecified, exactly like
 * cyaml_serialize()'s own identical note above.
 *
 * @param node  Root of the (sub-)tree to serialize.  A NULL node serializes
 *              as if it were a CYAML_NULL node ("~"); this is a
 *              successful, non-NULL return, not a failure signal.
 * @return Heap-allocated null-terminated string, or NULL on OOM or if node
 *         is nested more than 500 levels deep (see cyaml_serialize()'s own
 *         identical note above).
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
/*                         TAGS                                               */
/* ========================================================================== */

/**
 * @brief Named constants for the seven YAML 1.2 core-schema tag URIs.
 *
 * Compare against cyaml_node_tag()'s return value, or pass to
 * cyaml_node_set_tag(), without hand-typing the URI strings.
 */
#define CYAML_TAG_NULL "tag:yaml.org,2002:null"
#define CYAML_TAG_BOOL "tag:yaml.org,2002:bool"
#define CYAML_TAG_INT "tag:yaml.org,2002:int"
#define CYAML_TAG_FLOAT "tag:yaml.org,2002:float"
#define CYAML_TAG_STR "tag:yaml.org,2002:str"
#define CYAML_TAG_SEQ "tag:yaml.org,2002:seq"
#define CYAML_TAG_MAP "tag:yaml.org,2002:map"

/**
 * @brief Return a node's YAML tag, or NULL if it carries none.
 *
 * A node parsed from a document with an explicit tag ("!!str", a custom
 * "!foo", or a %TAG-resolved shorthand/verbatim tag) carries the fully
 * resolved tag string here; an untagged node (including one whose type was
 * determined purely by implicit core-schema resolution) returns NULL. One of
 * the seven CYAML_TAG_* constants above, or a custom tag string, depending
 * on what the source document (or cyaml_node_set_tag()) supplied.
 *
 * A dictionary key can never carry a tag: this DOM's keys are plain
 * strings, not nodes (see cyaml_dictionary_get()'s own doc comment), so a
 * tag decorating a key is parsed for validity but has nowhere to be stored;
 * it is silently discarded during parsing.
 *
 * @param node  May be NULL (returns NULL).
 */
const char *cyaml_node_tag(cyaml node);

/**
 * @brief Attach or replace a node's tag, or clear it.
 *
 * This sets metadata only; it never coerces the node's existing type or
 * value. Setting "tag:yaml.org,2002:str" on a CYAML_INTEGER node leaves it
 * a CYAML_INTEGER still carrying that (now type-inconsistent) tag string;
 * a caller wanting a node genuinely typed as a string should construct one
 * (cyaml_create_string()) and tag it, not tag an existing node of a
 * different type. A tag set here is preserved across a later cyaml_set()
 * call on the same node (which mutates only the node's value).
 *
 * @param node  Target node (any type).  NULL reports ccol_invalid_args and
 *              does nothing else.
 * @param tag   New tag string (copied); NULL clears any existing tag.
 * @return ccol_success, ccol_invalid_args if node is NULL, or
 *         ccol_not_enough_memory on allocation failure (the node's existing
 *         tag, if any, is left untouched in that case).
 */
ccol_retval_t cyaml_node_set_tag(cyaml node, const char *tag);

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
 * @p child must not be @p seq itself, nor a node that already (directly or
 * transitively) contains @p seq: this is not checked, and creating such a
 * cycle makes a later cyaml_destroy()/cyaml_clone()/cyaml_serialize*() on the
 * tree undefined behavior.
 *
 * @param seq    Target list node (must be CYAML_LIST).
 * @param child  Child to append.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_container_full (the list has reached its maximum
 *         representable element count; not reachable in practice).
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
 * If the key already exists, the new child is stored first and the previous
 * child is deep-freed afterward (never the other way around), so the slot
 * is never left pointing at freed memory if an intervening step were to
 * fail; both nodes may therefore be resident in memory at the same instant
 * during the replacement.
 * @p child must not be @p map itself, nor a node that already (directly or
 * transitively) contains @p map: this is not checked, and creating such a
 * cycle makes a later cyaml_destroy()/cyaml_clone()/cyaml_serialize*() on the
 * tree undefined behavior.
 *
 * @param map    Target dictionary node (must be CYAML_DICTIONARY).
 * @param key    Null-terminated key string; a copy is stored internally.
 * @param child  Value node.
 * @return ccol_success, ccol_invalid_args, ccol_not_enough_memory, or
 *         ccol_container_full (the dictionary has reached its maximum
 *         representable element count; not reachable in practice).
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
 * The clone uses the same allocator as the source tree, and each node's
 * tag (see cyaml_node_tag()), if any, is carried over unchanged.
 *
 * @param node  Root of the (sub-)tree to clone.  A NULL node returns NULL
 *              (unlike cyaml_serialize()/cyaml_serialize_flow(), which
 *              treat a NULL node as an already-successful CYAML_NULL
 *              value; there is no tree here to allocate a copy of).
 * @return New root node (caller owns it), or NULL if node is itself NULL,
 *         on OOM, or if node is nested more than 500 levels deep (see
 *         "Implementation-defined behavior" > "Nesting limits" above; the
 *         latter two failure causes are indistinguishable from each
 *         other, since this function has no separate error-string
 *         channel).
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
 *   cyaml_declare_scoped(root);
 *   root = cyaml_parse("key: value\n", NULL);
 *   // root is freed here automatically
 * }
 * @endcode
 */
#define cyaml_declare_scoped(var_name) \
  cyaml var_name _ccol_destructor(___cyaml_destroy) = NULL

/* ========================================================================== */
/*                         DESTRUCTION                                        */
/* ========================================================================== */

/** @brief Recursively free a DOM tree (internal; prefer the macro). */
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
 * Safe to call on a variable currently holding NULL (a no-op).  @p node
 * must be an assignable lvalue (a plain NULL literal cannot be passed
 * directly, since this macro NULLs it out afterward); use
 * ___cyaml_destroy() directly for a non-lvalue case.
 */
#define cyaml_destroy(node)  \
  do {                       \
    if (node) {              \
      __cyaml_destroy(node); \
      (node) = NULL;         \
    }                        \
  } while (0)

/* ========================================================================== */
/*                         PATH NAVIGATION; BACK-END                       */
/* ========================================================================== */

/**
 * @brief Navigate to the node addressed by a dot-separated path (back-end).
 *
 * Prefer the cyaml_get() macro.
 *
 * A NULL or empty path returns root itself (a zero-component path
 * addresses the root, consistent with cyaml_set()/cyaml_delete() treating
 * a leaf-only path, e.g. "key", as addressing a child of root without
 * requiring any leading path at all).
 *
 * @return Target node, or NULL if root is NULL, any path component is
 *         missing or type-mismatched, or the path contains an empty
 *         component (nothing between two dots, or a leading/trailing dot).
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
 *         ccol_invalid_args for a NULL/empty path, wrong parent type,
 *         malformed "#N" index syntax, or an empty path component (nothing
 *         between two dots, or a leading/trailing dot); at any path
 *         component, not just the leaf.
 *         ccol_key_not_found if a syntactically well-formed path component
 *         (a dictionary key or a "#N" index) is absent; at any position,
 *         including a syntactically valid but out-of-range list index.
 *         ccol_not_enough_memory on allocation failure.
 */
ccol_retval_t _cyaml_delete(cyaml root, const char *path);

/**
 * @brief Write a typed scalar value to the leaf addressed by a path (back-end).
 *
 * Prefer the cyaml_set() macro.
 *
 * For a dictionary leaf, creates the key if absent (the parent dictionary
 * must already exist).  For a list leaf ("#N"), the index must already be
 * in range; a list has no way to be auto-extended to fit an arbitrary index.
 * Either way, replaces and deep-frees any existing value at the leaf,
 * including full subtrees.
 *
 * @param root             Root of the DOM tree.
 * @param path             Dot-separated path string.
 * @param type             YAML type of the new value.
 * @param raw              Pointer to the raw C value (passed as void *).
 * @param raw_size         sizeof() the original C expression.
 * @param is_signed        Whether the integer source type is signed.
 * @param raw_is_char_array true when raw points directly at a char[] array.
 * @return ccol_success on success.
 *         ccol_invalid_args for a NULL/empty path, wrong parent type,
 *         malformed "#N" index syntax, or an empty path component (nothing
 *         between two dots, or a leading/trailing dot); at any path
 *         component, not just the leaf.
 *         ccol_key_not_found if a list index (leaf or intermediate) is
 *         syntactically well-formed but out of range, or if an intermediate
 *         dictionary key needed to resolve the parent path is absent (a
 *         missing dictionary key at the leaf itself is not an error: it is
 *         created).
 *         ccol_not_enough_memory on allocation failure.
 *         ccol_container_full if creating a new leaf dictionary key would
 *         exceed the parent's maximum representable element count (not
 *         reachable in practice).
 */
ccol_retval_t _cyaml_set_typed(cyaml root, const char *path,
                               cyaml_node_type_t type, void *raw,
                               size_t raw_size, bool is_signed,
                               bool raw_is_char_array);

/* ========================================================================== */
/*                         COMPILE-TIME TYPE HELPERS                         */
/* ========================================================================== */

/**
 * @brief Sentinel returned by _cyaml_type_of() for a C type that is not one of
 * the types cyaml_set() documents as accepted (bool, any integer type, float,
 * double, char *, const char *, or a bare NULL); examples include long
 * double, a struct, or any other type _Generic's default association below
 * catches.
 * A bare `NULL` literal (type void *) is deliberately given its own explicit
 * association below rather than falling into this default, since passing it
 * is documented, intentional usage (cyaml_set's own doc comment: "Passing
 * NULL sets the leaf to CYAML_NULL"), not a caller mistake.
 *
 * This is deliberately NOT a real cyaml_node_type_t enumerator: it is never
 * stored in a node or returned by cyaml_type(), only ever passed transiently
 * as the `type` argument of _cyaml_set_typed(), whose existing type-validation
 * switch (node_reinit_scalar() in cyaml.c) already rejects any value outside
 * {CYAML_NULL, CYAML_BOOL, CYAML_INTEGER, CYAML_FLOAT, CYAML_STRING} with
 * ccol_invalid_args before touching the target node. Using a value distinct
 * from every real enumerator (rather than defaulting to CYAML_NULL) is what
 * makes that guard actually fire for an unsupported type instead of being
 * silently bypassed by one that happens to already be on the accept-list.
 */
#define _CYAML_TYPE_UNSUPPORTED ((cyaml_node_type_t)0x7f)

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
        void *: CYAML_NULL,                                                 \
        default: _CYAML_TYPE_UNSUPPORTED);                                  \
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
      void *: CYAML_NULL,                \
      default: _CYAML_TYPE_UNSUPPORTED)
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
 * const char *, or a string literal.  Passing NULL sets the leaf to
 * CYAML_NULL.
 *
 * For a dictionary leaf, the key is created if absent (the parent
 * dictionary must already exist).  For a list leaf ("#N"), the index must
 * already be in range; a list has no way to be auto-extended to fit an
 * arbitrary index (see _cyaml_set_typed()'s own doc comment for the exact
 * ccol_key_not_found/ccol_invalid_args split this implies).  If the leaf
 * already exists, its type is changed unconditionally.
 *
 * @param root  Root cyaml handle.
 * @param path  Dot-separated path string.
 * @param val   C value whose type is detected at compile time via _Generic.
 *              A `char[N]` ARRAY VARIABLE (as opposed to a string literal
 *              written directly at the call site) cannot be passed
 *              directly; cast it to `(char *)` first.  This is a plain C
 *              language restriction (this macro copies val into a
 *              same-typed local, which is not possible for a non-literal
 *              array), not a cyaml-specific one.
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
